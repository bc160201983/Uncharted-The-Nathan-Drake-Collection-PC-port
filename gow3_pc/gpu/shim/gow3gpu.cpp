#include <fmt/format.h>
#include <tuple>
#include "gow3_write_log.h"
// gow3: glue between the C loader and the vendored shadPS4 video core.
#include "gow3_overlay.h"
#include "gow3_png.h"
#include "gow3_settings.h"
#include "gow3_trophies.h"
#include "gow3_copy.h"
#ifndef _WIN32
#include <sys/resource.h>
#endif
#include "gow3_toggles.h"
#include <algorithm>
#include <atomic>
#include <memory>
#include <condition_variable>
#include <boost/asio/io_context.hpp>
#include "common/polyfill_thread.h"
#include "video_core/renderdoc.h"
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <chrono>
#include <thread>
#include <vector>
#include <SDL3/SDL.h>
#include "../gow3gpu.h"
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/libs.h"
#include "core/memory.h"
#include "core/signals.h"
#include "sdl_window.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

extern "C" {
// runtime_memory.c
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
int runtime_memory_write_backing(uintptr_t address, const void* data, uint64_t size);
int runtime_memory_read_backing(uintptr_t address, void* data, uint64_t size);
void runtime_memory_copy_backing(uintptr_t address, void* dest, uint64_t size);
uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size);
int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped);
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write);
typedef void (*RuntimeGpuRange)(uintptr_t address, uint64_t size);
// runtime_kernel.c: one clock for guest and GPU timestamps
uint64_t runtime_process_time_us(void);
uint64_t runtime_process_time_counter(void);
uint64_t runtime_tsc_frequency(void);
uint64_t runtime_read_tsc(void);
int32_t* runtime_errno(void);
void runtime_memory_set_gpu_hooks(RuntimeGpuRange map, RuntimeGpuRange unmap, RuntimeGpuRange invalidate);
}

Frontend::WindowSDL* g_window = nullptr;

namespace Libraries::GnmDriver { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::AvPlayer { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::VideoOut { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::Kernel { void RegisterEventQueue(Core::Loader::SymbolsResolver* sym); }

namespace {
struct Symbol {
    std::string nid, library, module;
    u64 address;
};
std::vector<Symbol> g_symbols;
u32 g_sdk_version;
} // namespace

namespace Core::Loader {
void SymbolsResolver::AddSymbol(const char* nid, const char* library, const char* module, SymbolType,
                                u64 address) {
    g_symbols.push_back({nid, library, module, address});
}
} // namespace Core::Loader

// ElfInfo's fields are private to Core::Emulator; the port fills them here.
namespace Core {
class Emulator {
public:
    static void FillElfInfo(const Gow3GpuConfig& config) {
        auto& info = Common::ElfInfo::Instance();
        info.initialized = true;
        info.game_serial = config.serial ? config.serial : "UNKNOWN";
        info.title = config.title ? config.title : "";
        info.sdk_ver = config.sdk_version;
        info.psf_attributes.raw = config.psf_attributes;
    }
};

void MemoryManager::SetRasterizer(Vulkan::Rasterizer* rasterizer_) {
    rasterizer = rasterizer_;
    // Existing guest mappings are replayed by the runtime when hooks are installed.
    runtime_memory_set_gpu_hooks(
        [](uintptr_t address, uint64_t size) {
            auto* r = Memory::Instance()->GetRasterizer();
            r->RegisterMemory(address, size);
            r->MapMemory(address, size);
        },
        [](uintptr_t address, uint64_t size) { Memory::Instance()->GetRasterizer()->UnmapMemory(address, size); },
        [](uintptr_t address, uint64_t size) {
            Memory::Instance()->GetRasterizer()->InvalidateMemory(address, size);
        });
}
void MemoryManager::InvalidateMemory(VAddr address, u64 size) {
    if (rasterizer) rasterizer->InvalidateMemory(address, size);
}
u64 MemoryManager::ClampRangeSize(VAddr virtual_addr, u64 size) {
    return runtime_memory_clamp(virtual_addr, size);
}
// gow3: through the backing view (runtime_memory_copy_backing): uploads run while the GPU thread
// holds the buffer tracker's locks, and must not fault on pages read-protected for readbacks.
static void CopySparseSerial(VAddr source, u8* dest, u64 size) {
    // GOW3_UPLOAD_BACKING=0: through the guest view, as before 16 Oct (diagnosis).
    static const bool backing = [] {
        const char* env = std::getenv("GOW3_UPLOAD_BACKING");
        return !(env && env[0] == '0');
    }();
    if (backing) {
        runtime_memory_copy_backing(source, dest, size);
        return;
    }
    while (size) {
        uintptr_t start = 0, end = 0;
        int mapped = 0;
        if (!runtime_memory_region(source, &start, &end, &mapped)) {
            end = source + size;
        }
        const u64 n = std::min<u64>(size, end - source);
        if (mapped) std::memcpy(dest, reinterpret_cast<const void*>(source), n);
        else std::memset(dest, 0, n);
        source += n; dest += n; size -= n;
    }
}
// gow3 diagnostics (GOW3_UPLOAD_CHECK=1): a sample of the guest copies made for the GPU (up to
// 64 KiB) is hashed; at the frame's submit-done the guest bytes are hashed again. A source that
// changed was written by the game after the copy was taken (5 s statistics by 1 MiB window).
namespace {
struct CopiedSource {
    VAddr source;
    u64 size;
    u64 hash;
};
std::mutex g_copied_mutex;
std::vector<CopiedSource> g_copied;
bool UploadCheckEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_UPLOAD_CHECK");
        return env && env[0] == '1';
    }();
    return enabled;
}
u64 HashBytes(const u8* data, u64 size) {
    u64 h = 1469598103934665603ULL;
    for (u64 i = 0; i < size; ++i) {
        h = (h ^ data[i]) * 1099511628211ULL;
    }
    return h;
}
} // namespace

void MemoryManager::CheckCopiedSources() {
    if (!UploadCheckEnabled()) {
        return;
    }
    std::vector<CopiedSource> copies;
    {
        std::scoped_lock lock{g_copied_mutex};
        copies.swap(g_copied);
    }
    static std::map<u64, std::pair<u64, u64>> windows; // 1 MiB window -> (copies, changed)
    static u64 checked = 0, changed = 0;
    static auto last = std::chrono::steady_clock::now();
    std::vector<u8> now;
    for (const auto& copy : copies) {
        now.resize(copy.size);
        CopySparseSerial(copy.source, now.data(), copy.size);
        auto& w = windows[copy.source >> 20];
        ++w.first;
        ++checked;
        if (HashBytes(now.data(), copy.size) != copy.hash) {
            ++w.second;
            ++changed;
        }
    }
    if (const auto t = std::chrono::steady_clock::now(); t - last >= std::chrono::seconds{5}) {
        last = t;
        std::vector<std::tuple<u64, u64, u64>> sorted;
        for (const auto& [window, n] : windows) {
            if (n.second) {
                sorted.emplace_back(n.second, n.first, window);
            }
        }
        std::ranges::sort(sorted, std::greater{});
        std::string text;
        for (size_t i = 0; i < std::min<size_t>(sorted.size(), 8); ++i) {
            const auto [c, n, window] = sorted[i];
            text += fmt::format(" {:#x}: {}/{}", window << 20, c, n);
        }
        std::printf("Upload check (5 s): %llu sampled copies, %llu sources changed by submit-done;%s\n",
                    static_cast<unsigned long long>(checked), static_cast<unsigned long long>(changed),
                    text.c_str());
        std::fflush(stdout);
        windows.clear();
        checked = changed = 0;
    }
}

void MemoryManager::CopySparseMemory(VAddr source, u8* dest, u64 size) {
    if (UploadCheckEnabled() && size <= 64 * 1024) {
        CopySparseSerial(source, dest, size);
        static std::atomic<u32> sample{0};
        if ((sample.fetch_add(1, std::memory_order_relaxed) & 3) == 0) {
            const u64 hash = HashBytes(dest, size);
            std::scoped_lock lock{g_copied_mutex};
            if (g_copied.size() < 100000) {
                g_copied.push_back({source, size, hash});
            }
        }
        return;
    }
    // gow3: large uploads (streaming) are split across the copy threads.
    constexpr u64 Chunk = 512 * 1024;
    if (size < 4 * Chunk || !Gow3Copy::Enabled()) {
        return CopySparseSerial(source, dest, size);
    }
    Gow3Copy::ParallelFor((size + Chunk - 1) / Chunk, [&](std::size_t i) {
        const u64 offset = i * Chunk;
        CopySparseSerial(source + offset, dest + offset, std::min(Chunk, size - offset));
    });
}
bool MemoryManager::TryReadBacking(VAddr address, void* data, u64 size) {
    return runtime_memory_read_backing(address, data, size) != 0;
}
// gow3 17 Oct 2026: 64 KiB windows of the 40-bit guest address space written through the backing
// view (labels, readback landings, image downloads): page tracking does not see these writes, so
// the buffer cache keeps no arena copy of small read-only ranges there (UseArenaForStatic).
namespace {
constexpr u64 BackingWindowBits = u64(1) << 24;
std::unique_ptr<std::atomic<u64>[]> g_backing_windows{new std::atomic<u64>[BackingWindowBits / 64]{}};
} // namespace

bool MemoryManager::BackingWritten(VAddr address, u64 size) {
    if (size == 0) {
        return false;
    }
    const u64 last = (address + size - 1) >> 16;
    if (last >= BackingWindowBits) {
        return true;
    }
    for (u64 w = address >> 16; w <= last; ++w) {
        if (g_backing_windows[w >> 6].load(std::memory_order_relaxed) & (u64(1) << (w & 63))) {
            return true;
        }
    }
    return false;
}

bool MemoryManager::TryWriteBacking(void* address, const void* data, u64 size) {
    if (size) {
        const VAddr addr = reinterpret_cast<VAddr>(address);
        const u64 last = std::min<u64>((addr + size - 1) >> 16, BackingWindowBits - 1);
        for (u64 w = addr >> 16; w <= last; ++w) {
            auto& word = g_backing_windows[w >> 6];
            const u64 bit = u64(1) << (w & 63);
            if (!(word.load(std::memory_order_relaxed) & bit)) {
                word.fetch_or(bit, std::memory_order_relaxed);
            }
        }
    }
    Gow3WriteLog::Note(reinterpret_cast<uintptr_t>(address), data, size, Gow3WriteLog::Backing);
    return runtime_memory_write_backing(reinterpret_cast<uintptr_t>(address), data, size) != 0;
}
// Uncharted: fiber stacks (gow3_fiber.cpp) stay writable. GPU write tracking protects any guest
// memory a buffer binding covers, and the DMA pointer windows (Rasterizer::SyncDmaMemory) can
// cover the job system's fiber stacks; a write-protected stack in use kills the process, since
// Windows cannot push the exception frame onto it (Uncharted 2, ChooseJob). Their CPU writes
// are not tracked: the GPU never reads them.
namespace {
std::mutex g_keep_writable_mutex;
std::map<VAddr, VAddr> g_keep_writable; // page-aligned start -> end
std::atomic<bool> g_keep_writable_any{false};
} // namespace

bool MemoryManager::KeptWritable(VAddr address, u64 size) {
    if (!g_keep_writable_any.load(std::memory_order_relaxed)) {
        return false;
    }
    std::scoped_lock lock{g_keep_writable_mutex};
    auto it = g_keep_writable.upper_bound(address);
    if (it != g_keep_writable.begin()) {
        --it;
    }
    for (; it != g_keep_writable.end() && it->first < address + size; ++it) {
        if (it->second > address) {
            return true;
        }
    }
    return false;
}

void KeepWritable(const void* address, u64 size, bool keep) {
    const VAddr start = reinterpret_cast<VAddr>(address) & ~VAddr(4095);
    const VAddr end = (reinterpret_cast<VAddr>(address) + size + 4095) & ~VAddr(4095);
    if (!address || !size) {
        return;
    }
    {
        std::scoped_lock lock{g_keep_writable_mutex};
        if (keep) {
            auto& stored = g_keep_writable[start];
            stored = std::max(stored, end);
        } else {
            g_keep_writable.erase(start);
        }
        g_keep_writable_any = !g_keep_writable.empty();
    }
    if (keep) {
        // Undo protection set before the stack was known (GPU restrictions only).
        runtime_memory_gpu_protect(start, end - start, 1, 1);
    }
}

void AddressSpace::Protect(VAddr virtual_addr, u64 size, MemoryPermission perms) {
    Gow3Stats::Timer timer{Gow3Stats::t_protect};
    const u64 pages = (size + 4095) / 4096;
    Gow3Stats::protect_calls.fetch_add(1, std::memory_order_relaxed);
    Gow3Stats::protect_pages.fetch_add(pages, std::memory_order_relaxed);
    const bool read = True(perms & MemoryPermission::Read);
    const bool write = True(perms & MemoryPermission::Write);
    if (!write) {
        Gow3Stats::protect_revoke_calls.fetch_add(1, std::memory_order_relaxed);
        Gow3Stats::protect_revoke_pages.fetch_add(pages, std::memory_order_relaxed);
    }
    if (write || !g_keep_writable_any.load(std::memory_order_relaxed)) {
        runtime_memory_gpu_protect(virtual_addr, size, read, write);
        return;
    }
    // Protect around the fiber stacks in the range; they keep read and write access (a stack
    // page without them kills the process: Windows can't deliver the exception).
    VAddr cursor = virtual_addr;
    const VAddr end = virtual_addr + size;
    std::scoped_lock lock{g_keep_writable_mutex};
    auto it = g_keep_writable.upper_bound(virtual_addr);
    if (it != g_keep_writable.begin()) {
        --it;
    }
    for (; it != g_keep_writable.end() && it->first < end; ++it) {
        const VAddr lo = std::max(it->first, virtual_addr);
        const VAddr hi = std::min(it->second, end);
        if (lo >= hi) {
            continue;
        }
        if (lo > cursor) {
            runtime_memory_gpu_protect(cursor, lo - cursor, read, false);
        }
        runtime_memory_gpu_protect(lo, hi - lo, true, true); // nor read access (async readbacks)
        cursor = std::max(cursor, hi);
    }
    if (cursor < end) {
        runtime_memory_gpu_protect(cursor, end - cursor, read, false);
    }
}
boost::icl::interval_set<VAddr> AddressSpace::GetUsableRegions() {
    boost::icl::interval_set<VAddr> set;
    set += boost::icl::interval<VAddr>::right_open(0x10'0000'0000ULL, 0xfc'0000'0000ULL);
    return set;
}
} // namespace Core

void Gow3KeepWritable(const void* address, uint64_t size, bool keep) {
    Core::KeepWritable(address, size, keep);
}

// Kernel services the vendored libraries call directly.
namespace Libraries::Kernel {
u64 PS4_SYSV_ABI sceKernelGetTscFrequency() { return runtime_tsc_frequency(); }
u64 PS4_SYSV_ABI sceKernelReadTsc() { return runtime_read_tsc(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTime() { return runtime_process_time_us(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounter() { return runtime_process_time_counter(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounterFrequency() { return runtime_tsc_frequency(); }
s32 PS4_SYSV_ABI sceKernelIsNeoMode() { return 0; } // base PS4
s32 PS4_SYSV_ABI sceKernelGetCompiledSdkVersion(s32* ver) {
    if (!ver) return ORBIS_KERNEL_ERROR_EINVAL;
    *ver = s32(g_sdk_version);
    return ORBIS_OK;
}
s32 PS4_SYSV_ABI sceKernelUsleep(u32 microseconds) {
    std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
    return ORBIS_OK;
}
} // namespace Libraries::Kernel

namespace {
// SDL video must be driven from one thread: the window lives on its own host thread.
std::thread g_window_thread;
std::mutex g_window_mutex;
std::condition_variable g_window_cv;
bool g_window_ready;

// gow3 20 Oct 2026 (idle CPU): the window thread pumped events every 2 ms forever (500 passes a
// second, each taking the text-input and overlay locks and running SDL's event pump). While the
// window has the focus it still does (mouse look, keyboard, pad). Without the focus SDL drops
// the pad's and the keyboard's input anyway (SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS is not
// set) and the mouse is released: it then pumps every 8 ms, every 16 ms while minimized, and a
// request for the window thread (the text input dialog) wakes it at once.
// GOW3_WINDOW_IDLE_BACKOFF=0: every 2 ms always.
std::mutex g_window_wake_mutex;
std::condition_variable g_window_wake_cv;
bool g_window_wake = false; ///< under g_window_wake_mutex

void WakeWindowThread() {
    {
        std::scoped_lock lock{g_window_wake_mutex};
        g_window_wake = true;
    }
    g_window_wake_cv.notify_one();
}

/// The window thread's wait between event pumps (see above).
void WindowThreadIdle(SDL_Window* window) {
    static const bool backoff = [] {
        const char* env = std::getenv("GOW3_WINDOW_IDLE_BACKOFF");
        const bool on = !(env && env[0] == '0');
        std::printf("Window: events every 2 ms%s\n",
                    on ? ", 8 ms without the focus, 16 ms minimized "
                         "(GOW3_WINDOW_IDLE_BACKOFF=0: 2 ms)"
                       : " (GOW3_WINDOW_IDLE_BACKOFF=0)");
        return on;
    }();
    const SDL_WindowFlags flags = backoff ? SDL_GetWindowFlags(window) : 0;
    if (!backoff || ((flags & SDL_WINDOW_INPUT_FOCUS) && !(flags & SDL_WINDOW_MINIMIZED))) {
        SDL_Delay(2);
        return;
    }
    const auto wait = std::chrono::milliseconds((flags & SDL_WINDOW_MINIMIZED) ? 16 : 8);
    std::unique_lock lock{g_window_wake_mutex};
    g_window_wake_cv.wait_for(lock, wait, [] { return g_window_wake; });
    g_window_wake = false;
}
} // namespace

double Gow3DisplayRefreshRate() {
    static const double hz = [] {
        const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
        return mode && mode->refresh_rate > 0 ? double(mode->refresh_rate) : 0.0;
    }();
    return hz;
}

u32 Gow3DisplayRefreshHz() {
    static const u32 hz = [] {
        const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
        const u32 rate = mode && mode->refresh_rate > 0 ? u32(mode->refresh_rate + 0.5f) : 60;
        std::printf("GPU: vblank follows the display refresh rate, %u Hz\n", rate);
        return std::max<u32>(rate, 60);
    }();
    return hz;
}

#ifdef GOW3_PGO_GENERATE
extern "C" void __gcov_dump(void);
extern "C" void __gcov_reset(void);
// Instrumented build (GOW3_PGO=generate): the game often ends through _exit (watchdog, guest
// exit), which skips the profile write at exit. Write every 30 s and reset: the files sum the
// intervals.
static void StartProfileWriter() {
    std::thread([] {
        Common::SetCurrentThreadName("gow3:pgo");
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            __gcov_dump();
            __gcov_reset();
            std::printf("PGO: profile written\n");
        }
    }).detach();
}
#endif

extern "C" int gow3gpu_init(const Gow3GpuConfig* config) {
    Gow3Settings::Load();
#ifdef GOW3_PGO_GENERATE
    StartProfileWriter();
#endif
    g_sdk_version = config->sdk_version;
#ifdef _WIN32
    if (config->user_dir && !std::getenv("GOW3_GPU_USER_DIR")) _putenv_s("GOW3_GPU_USER_DIR", config->user_dir);
#else
    if (config->user_dir) setenv("GOW3_GPU_USER_DIR", config->user_dir, 0);
#endif
    Core::Emulator::FillElfInfo(*config);
    // gow3: the port's author in the title bar.
    const std::string title = std::string(config->title ? config->title : "Uncharted: The Nathan Drake Collection") +
                              " — PC port by BilalReloaded";
    const s32 width = config->width, height = config->height;
    g_window_thread = std::thread([title, width, height] {
        Common::SetCurrentThreadName("gow3:window");
        auto* window = new Frontend::WindowSDL(width, height, title.c_str());
        {
            std::scoped_lock lock{g_window_mutex};
            g_window = window;
            g_window_ready = true;
        }
        g_window_cv.notify_all();
        while (window->PollEvents()) {
            WindowThreadIdle(window->GetSDLWindow());
        }
        LOG_INFO(Frontend, "Window closed by user");
        std::fflush(stdout);
        std::_Exit(0);
    });
    g_window_thread.detach();
    {
        std::unique_lock lock{g_window_mutex};
        g_window_cv.wait(lock, [] { return g_window_ready; });
    }
    Core::Loader::SymbolsResolver resolver;
    // GnmDriver creates the presenter that the VideoOut present thread uses.
    Libraries::GnmDriver::RegisterLib(&resolver);
    Libraries::VideoOut::RegisterLib(&resolver);
    return 0;
}

namespace Libraries::Kernel { void StartKernelService(); }
extern "C" void gow3gpu_register_kernel(void) {
    Libraries::Kernel::StartKernelService();
    Core::Loader::SymbolsResolver resolver;
    Libraries::Kernel::RegisterEventQueue(&resolver);
    Libraries::AvPlayer::RegisterLib(&resolver);
}

extern "C" uintptr_t gow3gpu_resolve(const char* scoped_nid) {
    const char* hash = std::strchr(scoped_nid, '#');
    const size_t length = hash ? size_t(hash - scoped_nid) : std::strlen(scoped_nid);
    for (const auto& symbol : g_symbols) {
        if (symbol.nid.size() == length && !std::memcmp(symbol.nid.data(), scoped_nid, length)) {
            return uintptr_t(symbol.address);
        }
    }
    return 0;
}

extern "C" int gow3gpu_handle_fault(void* ucontext, void* address) {
    return Core::Signals::Instance()->DispatchAccessViolation(ucontext, address) ? 1 : 0;
}

extern "C" unsigned gow3gpu_symbol_count(void) {
    return unsigned(g_symbols.size());
}

// Kernel service thread: runs boost::asio timers for equeue timer events.
namespace Libraries::Kernel {
boost::asio::io_context io_context;
static std::mutex m_asio_req;
static std::condition_variable_any cv_asio_req;
static std::atomic<u32> asio_requests;
static std::jthread service_thread;

void KernelSignalRequest() {
    std::unique_lock lock{m_asio_req};
    ++asio_requests;
    cv_asio_req.notify_one();
}

static void KernelServiceThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("gow3:kernel_service");
    while (!stoken.stop_requested()) {
        {
            std::unique_lock lock{m_asio_req};
            cv_asio_req.wait(lock, stoken, [] { return asio_requests != 0; });
        }
        if (stoken.stop_requested()) break;
        io_context.run();
        io_context.restart();
        asio_requests = 0;
    }
}

void StartKernelService() {
    service_thread = std::jthread{KernelServiceThread};
}

int* PS4_SYSV_ABI __Error() {
    return runtime_errno();
}
} // namespace Libraries::Kernel

// RenderDoc capture hooks: not used by the port.
namespace VideoCore {
void LoadRenderDoc() {}
void StartCapture() {}
void EndCapture() {}
void TriggerCapture() {}
void SetOutputDir(const std::filesystem::path&, const std::string&) {}
bool IsRenderDocLoaded() { return false; }
void RequestScreenshot(ScreenshotRequest) {}
u32 ConsumeGameOnlyScreenshotRequests() { return 0; }
u32 ConsumeWithOverlaysScreenshotRequests() { return 0; }
ScreenshotRequests ConsumeScreenshotRequests() { return {}; }
} // namespace VideoCore

extern "C" int gow3gpu_overlay_captures_input(void) {
    return Gow3Overlay::CapturesInput() ? 1 : 0;
}

extern "C" int gow3gpu_text_input_begin(const char* initial, const char* prompt) {
    if (!g_window) return 0;
    g_window->BeginTextInput(initial ? initial : "", prompt ? prompt : "Text");
    WakeWindowThread(); // gow3: started by the window thread, which may be in its idle wait
    return 1;
}

// gow3 tests: GOW3_TEST_CHOICE=<n> picks entry n of the game's list dialogs (the save list: 0 is
// "New save" when offered) at once, without showing them. Automated runs cannot answer them: their
// keyboard is off and the pad file only reaches the game.
static int g_test_choice = -1;
static int TestChoice() {
    static const int pick = [] {
        const char* env = std::getenv("GOW3_TEST_CHOICE");
        return env && env[0] ? std::atoi(env) : -1;
    }();
    return pick;
}

extern "C" int gow3gpu_choice_begin(const char* title, const char* const* items, const char* const* images,
                                    int count, int focus) {
    if (!g_window || count <= 0) return 0;
    if (const int pick = TestChoice(); pick >= 0) {
        g_test_choice = std::min(pick, count - 1);
        std::printf("Test: list dialog \"%s\": entry %d (%s) chosen (GOW3_TEST_CHOICE)\n", title ? title : "",
                    g_test_choice, items[g_test_choice] ? items[g_test_choice] : "");
        return 1;
    }
    // Pictures are decoded here, on the game's thread, before the list is shown.
    std::vector<Gow3Png::Image> pictures(count);
    for (int i = 0; images && i < count; ++i) {
        if (images[i] && !Gow3Png::LoadRgba(images[i], pictures[i])) {
            LOG_WARNING(Lib_SaveDataDialog, "save list: no picture from {}", images[i]);
        }
    }
    Gow3Overlay::BeginChoice(title ? title : "", std::vector<std::string>(items, items + count),
                             std::move(pictures), focus);
    return 1;
}

extern "C" void gow3gpu_trophy_list(const char* pack_path, int list, const uint32_t earned[4]) {
    Gow3Trophies::SetList(pack_path ? pack_path : "", list, earned);
}

extern "C" void gow3gpu_trophy_earned(int id) {
    Gow3Trophies::Earned(id);
}

extern "C" void gow3gpu_match_loading(int active, const char* title, const char* map,
                                      const char* rules, const char* picture) {
    Gow3Png::Image image;
    if (active && picture && !Gow3Png::LoadRgba(picture, image)) {
        LOG_WARNING(Render, "match loading: no picture from {}", picture);
    }
    Gow3Overlay::SetMatchLoading(active != 0, title ? title : "", map ? map : "", rules ? rules : "",
                                 std::move(image));
}

extern "C" int gow3gpu_choice_poll(void) {
    if (g_test_choice >= 0) {
        const int pick = g_test_choice;
        g_test_choice = -1;
        return pick;
    }
    return Gow3Overlay::PollChoice();
}

extern "C" int gow3gpu_text_input_poll(char* out, uint64_t size) {
    if (!g_window) return 2;
    std::string text;
    const int state = g_window->PollTextInput(text);
    if (size) {
        const size_t n = std::min<size_t>(text.size(), size - 1);
        std::memcpy(out, text.data(), n);
        out[n] = 0;
    }
    return state;
}
