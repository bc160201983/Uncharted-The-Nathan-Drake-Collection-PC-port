// SPDX-License-Identifier: GPL-2.0-or-later
#include "gow3_perf_stats.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cwchar>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "common/thread.h"
#include "video_core/renderer_vulkan/vk_instance.h"

#ifdef _WIN32
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#endif

namespace Gow3PerfStats {

namespace {

std::mutex mutex;
Sample latest;
std::once_flag started;

// gow3 20 Oct 2026 (idle CPU): once started the thread sampled twice a second for good, the
// overlay hidden or not; its GPU counter query reads the GPU engines of every process (the most
// expensive part). It now samples once a second, and only while Start() was called in the last
// 2 s (each overlay frame showing a CPU, RAM, GPU or VRAM item calls it); otherwise it sleeps
// until the next call. GOW3_PERF_SAMPLER_PAUSE=0: every 500 ms, never paused.
bool PauseWhenHidden() {
    static const bool on = [] {
        const char* env = std::getenv("GOW3_PERF_SAMPLER_PAUSE");
        const bool pause = !(env && env[0] == '0');
        std::printf("Overlay: CPU/RAM/GPU/VRAM and their temperatures sampled %s\n",
                    pause ? "every second while shown (GOW3_PERF_SAMPLER_PAUSE=0: every 500 ms)"
                          : "every 500 ms (GOW3_PERF_SAMPLER_PAUSE=0)");
        return pause;
    }();
    return on;
}
constexpr auto IdleAfter = std::chrono::seconds(2);
std::atomic<int64_t> last_demand_ns{0}; ///< steady_clock time of the last Start()
std::mutex demand_mutex;
std::condition_variable demand_cv;
std::atomic<bool> paused{false}; ///< the thread sleeps on demand_cv (changed under demand_mutex)

int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::chrono::milliseconds SamplePeriod() {
    return std::chrono::milliseconds(PauseWhenHidden() ? 1000 : 500);
}

/// Sampling thread: sleeps while no overlay item needs the samples. True when it slept (the
/// CPU and GPU use need a new starting sample).
bool WaitWhileHidden() {
    const int64_t idle_ns = std::chrono::nanoseconds(IdleAfter).count();
    if (!PauseWhenHidden() || NowNs() - last_demand_ns.load() < idle_ns) {
        return false;
    }
    std::unique_lock lock{demand_mutex};
    paused = true;
    // Start() stores its time, then reads `paused` (both sequentially consistent): it either
    // sees true here and wakes this thread, or its time is seen by this check.
    if (NowNs() - last_demand_ns.load() < idle_ns) {
        paused = false;
        return false;
    }
    demand_cv.wait(lock, [] { return !paused.load(); });
    return true;
}

#ifdef _WIN32
uint64_t Ticks(const FILETIME& time) {
    return (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

// This process's use of the GPU's 3D engines, as Task Manager shows it ("GPU Engine" counters,
// any vendor). Negative until the second sample or without the counters.
class GpuUsage {
public:
    GpuUsage() {
        if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) {
            query = nullptr;
            return;
        }
        if (PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &counter) !=
            ERROR_SUCCESS) {
            PdhCloseQuery(query);
            query = nullptr;
            return;
        }
        PdhCollectQueryData(query);
        pid = L"pid_" + std::to_wstring(GetCurrentProcessId()) + L"_";
    }

    float Percent() {
        if (!query || PdhCollectQueryData(query) != ERROR_SUCCESS) {
            return -1.0f;
        }
        DWORD size = 0, count = 0;
        if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count,
                                         nullptr) != PDH_MORE_DATA) {
            return -1.0f;
        }
        buffer.resize(size);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
        if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count,
                                         items) != ERROR_SUCCESS) {
            return -1.0f;
        }
        double total = 0.0;
        for (DWORD i = 0; i < count; ++i) {
            const wchar_t* name = items[i].szName;
            if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA && wcsstr(name, pid.c_str()) &&
                wcsstr(name, L"engtype_3D")) {
                total += items[i].FmtValue.doubleValue;
            }
        }
        return std::clamp(float(total), 0.0f, 100.0f);
    }

private:
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER counter = nullptr;
    std::wstring pid;
    std::vector<unsigned char> buffer;
};

// gow3 24 Oct 2026: the graphics card's temperature from NVIDIA's driver (nvml.dll ships with
// every NVIDIA driver). Loaded by hand, so the build needs neither NVML headers nor its import
// library; a machine without the DLL (AMD, Intel) simply shows the use without a temperature.
// The device is picked by its name so a laptop's integrated GPU is not mistaken for the card.
class GpuTemp {
public:
    explicit GpuTemp(std::string vulkan_name) : name(std::move(vulkan_name)) { TryLoad(); }

    float Celsius() {
        // At start the driver may not answer yet: try again every 30 s.
        if ((!lib || !handle) && NowMs() - last_try_ns > 30'000) {
            TryLoad();
        }
        if (!lib || !handle || !get_temperature) {
            return -1.0f;
        }
        unsigned value = 0;
        if (get_temperature(handle, 0 /* NVML_TEMPERATURE_GPU */, &value) != 0) {
            return -1.0f;
        }
        const float celsius = float(value);
        return celsius >= -20.0f && celsius <= 150.0f ? celsius : -1.0f;
    }

private:
    using InitFn = int (*)();
    using CountFn = int (*)(unsigned*);
    using HandleFn = int (*)(unsigned, void**);
    using NameFn = int (*)(void*, char*, unsigned);
    using TempFn = int (*)(void*, int, unsigned*);

    static int64_t NowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    static bool SameName(const char* from_driver, const std::string& from_vulkan) {
        const auto lower = [](std::string text) {
            for (char& c : text) {
                c = char(std::tolower(static_cast<unsigned char>(c)));
            }
            return text;
        };
        const std::string a = lower(from_driver), b = lower(from_vulkan);
        if (a.empty() || b.empty()) {
            return false;
        }
        return a == b || a.find(b) != std::string::npos || b.find(a) != std::string::npos;
    }

    void TryLoad() {
        last_try_ns = NowMs();
        if (!lib) {
            // From System32 only (the driver puts it there): a file of that name next to the
            // game is never loaded (24 Oct 2026).
            lib = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!lib) {
                return;
            }
            init = reinterpret_cast<InitFn>(GetProcAddress(lib, "nvmlInit_v2"));
            if (!init) {
                init = reinterpret_cast<InitFn>(GetProcAddress(lib, "nvmlInit"));
            }
            count = reinterpret_cast<CountFn>(GetProcAddress(lib, "nvmlDeviceGetCount_v2"));
            if (!count) {
                count = reinterpret_cast<CountFn>(GetProcAddress(lib, "nvmlDeviceGetCount"));
            }
            handle_by_index = reinterpret_cast<HandleFn>(
                GetProcAddress(lib, "nvmlDeviceGetHandleByIndex_v2"));
            if (!handle_by_index) {
                handle_by_index = reinterpret_cast<HandleFn>(
                    GetProcAddress(lib, "nvmlDeviceGetHandleByIndex"));
            }
            device_name = reinterpret_cast<NameFn>(GetProcAddress(lib, "nvmlDeviceGetName"));
            get_temperature = reinterpret_cast<TempFn>(GetProcAddress(lib, "nvmlDeviceGetTemperature"));
            if (!init || !count || !handle_by_index || !get_temperature || init() != 0) {
                FreeLibrary(lib);
                lib = nullptr;
                return;
            }
        }
        unsigned devices = 0;
        if (count(&devices) != 0 || devices == 0) {
            return;
        }
        unsigned chosen = 0;
        if (devices > 1 && device_name) {
            for (unsigned i = 0; i < devices; ++i) {
                void* candidate = nullptr;
                char candidate_name[96] = {};
                if (handle_by_index(i, &candidate) == 0 &&
                    device_name(candidate, candidate_name, sizeof(candidate_name)) == 0 &&
                    SameName(candidate_name, name)) {
                    chosen = i;
                    break;
                }
            }
        }
        void* device = nullptr;
        handle = handle_by_index(chosen, &device) == 0 ? device : nullptr;
    }

    std::string name;
    HMODULE lib = nullptr;
    void* handle = nullptr;
    int64_t last_try_ns = 0;
    InitFn init = nullptr;
    CountFn count = nullptr;
    HandleFn handle_by_index = nullptr;
    NameFn device_name = nullptr;
    TempFn get_temperature = nullptr;
};

// gow3 24 Oct 2026: the processor's temperature from the ACPI thermal zone (Windows' "Thermal
// Zone Information" counters; Kelvin, no administrator needed, present on most laptops and
// desktops). It follows the processor's load but reads a few degrees away from its internal
// sensor, which Windows exposes only to a kernel driver. No zone: no temperature.
class CpuTemp {
public:
    CpuTemp() {
        if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) {
            query = nullptr;
            return;
        }
        if (PdhAddEnglishCounterW(query, L"\\Thermal Zone Information(*)\\Temperature", 0,
                                  &counter) != ERROR_SUCCESS) {
            PdhCloseQuery(query);
            query = nullptr;
            return;
        }
        PdhCollectQueryData(query);
    }

    float Celsius() {
        if (!query || PdhCollectQueryData(query) != ERROR_SUCCESS) {
            return -1.0f;
        }
        DWORD size = 0, count = 0;
        if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &size, &count, nullptr) !=
            PDH_MORE_DATA) {
            return -1.0f;
        }
        buffer.resize(size);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
        if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &size, &count, items) !=
            ERROR_SUCCESS) {
            return -1.0f;
        }
        double hottest = 0.0;
        for (DWORD i = 0; i < count; ++i) {
            if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA) {
                continue;
            }
            double kelvin = items[i].FmtValue.doubleValue;
            if (kelvin > 1000.0) {
                kelvin /= 10.0; // some firmware reports tenths of a kelvin
            }
            // 200-400 K is -73 to 127 C: outside it the counter is not a temperature.
            if (kelvin >= 200.0 && kelvin <= 400.0 && kelvin > hottest) {
                hottest = kelvin;
            }
        }
        return hottest > 0.0 ? float(hottest - 273.15) : -1.0f;
    }

private:
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER counter = nullptr;
    std::vector<unsigned char> buffer;
};

void Run(const Vulkan::Instance* instance) {
    GpuUsage gpu;
    GpuTemp gpu_temp(std::string(instance->GetModelName()));
    CpuTemp cpu_temp;
    FILETIME created, ended, kernel, user, now;
    uint64_t last_process = 0, last_wall = 0;
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    constexpr double GiB = 1024.0 * 1024.0 * 1024.0;
    while (true) {
        if (WaitWhileHidden()) {
            // Shown again: CPU and GPU use are differences between two samples. New starting
            // sample now, the first values 250 ms later (not the average over the pause).
            GetSystemTimeAsFileTime(&now);
            if (GetProcessTimes(GetCurrentProcess(), &created, &ended, &kernel, &user)) {
                last_process = Ticks(kernel) + Ticks(user);
                last_wall = Ticks(now);
            }
            gpu.Percent();
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        Sample s;
        s.gpu_name = std::string(instance->GetModelName());
        GetSystemTimeAsFileTime(&now);
        if (GetProcessTimes(GetCurrentProcess(), &created, &ended, &kernel, &user)) {
            const uint64_t process = Ticks(kernel) + Ticks(user), wall = Ticks(now);
            if (last_wall) {
                s.cpu_percent = CpuPercent(process - last_process, wall - last_wall, cores);
            }
            last_process = process;
            last_wall = wall;
        }
        // The game's own memory (its working set), not the whole system's (24 Oct 2026).
        PROCESS_MEMORY_COUNTERS memory{};
        if (K32GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory))) {
            s.ram_used_gb = double(memory.WorkingSetSize) / GiB;
        }
        MEMORYSTATUSEX system{.dwLength = sizeof(MEMORYSTATUSEX)};
        if (GlobalMemoryStatusEx(&system)) {
            s.ram_total_gb = double(system.ullTotalPhys) / GiB;
        }
        if (instance->CanReportMemoryUsage()) {
            s.vram_used_gb = double(instance->GetDeviceMemoryUsage()) / GiB;
            s.vram_total_gb = double(instance->GetDeviceMemoryBudgetNow()) / GiB;
        }
        s.gpu_percent = gpu.Percent();
        s.gpu_temp_c = gpu_temp.Celsius();
        s.cpu_temp_c = cpu_temp.Celsius();
        {
            std::scoped_lock lock{mutex};
            latest = std::move(s);
        }
        std::this_thread::sleep_for(SamplePeriod());
    }
}
#else
void Run(const Vulkan::Instance* instance) {
    while (true) {
        WaitWhileHidden();
        Sample s;
        s.gpu_name = std::string(instance->GetModelName());
        {
            std::scoped_lock lock{mutex};
            latest = std::move(s);
        }
        std::this_thread::sleep_for(SamplePeriod());
    }
}
#endif

} // namespace

void Start(const Vulkan::Instance& instance) {
    last_demand_ns.store(NowNs());
    // ponytail: never joined; a thread that ended while the game ran crashed in ntdll
    // (vk_pipeline_serialization.cpp, StopWarmUp). It sleeps between samples, and while the
    // overlay shows no sampled item (gow3 20 Oct 2026, see PauseWhenHidden).
    std::call_once(started, [&] {
        std::thread([&instance] {
            Common::SetCurrentThreadName("gow3:PerfStats");
            Run(&instance);
        }).detach();
    });
    if (paused.load()) {
        {
            std::scoped_lock lock{demand_mutex};
            paused = false;
        }
        demand_cv.notify_one();
    }
}

Sample Latest() {
    std::scoped_lock lock{mutex};
    return latest;
}

} // namespace Gow3PerfStats
