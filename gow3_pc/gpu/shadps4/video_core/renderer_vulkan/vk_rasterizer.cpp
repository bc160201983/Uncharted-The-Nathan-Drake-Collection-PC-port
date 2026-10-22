#include <algorithm>
#include <bit>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <mutex>
#include <optional>
#include <thread>
#include <filesystem>
#include <boost/container/small_vector.hpp>
// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <xxhash.h>
#include "video_core/renderer_vulkan/ui_composition.h"
#include "gow3_toggles.h"
#include "gow3_settings.h"
#include "gow3_watch_range.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "common/debug.h"
#include "common/decoder.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_depth_stencil_state.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "gow3_threads.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

static Shader::PushData MakeUserData(const AmdGpu::Regs& regs) {
    // TODO(roamic): Add support for multiple viewports and geometry shaders when ViewportIndex
    // is encountered and implemented in the recompiler.
    Shader::PushData push_data{};
    push_data.xoffset = regs.viewport_control.xoffset_enable ? regs.viewports[0].xoffset : 0.f;
    push_data.xscale = regs.viewport_control.xscale_enable ? regs.viewports[0].xscale : 1.f;
    push_data.yoffset = regs.viewport_control.yoffset_enable ? regs.viewports[0].yoffset : 0.f;
    push_data.yscale = regs.viewport_control.yscale_enable ? regs.viewports[0].yscale : 1.f;
    return push_data;
}

extern "C" void runtime_exec_watch_retarget(uint64_t address);
namespace {
// gow3 16 Oct: a depth image sampled by a draw that also has it as its depth attachment
// (Uncharted 3's half-resolution depth, 960x540 D32S8, with depth writes on) used to be bound as
// a texture in DepthStencilReadOnlyOptimal and then moved to the attachment layout before the
// render pass: the descriptor named a stale layout and every such draw had two transitions
// (Vulkan synchronization validation: write-after-write and read-after-write hazards). On NVIDIA
// objects flickered as flat-coloured shapes. Both uses now share the general layout, like a
// colour target in a feedback loop.
constexpr vk::ImageLayout DepthFeedbackLayout = vk::ImageLayout::eGeneral;
constexpr vk::PipelineStageFlags2 DepthFeedbackStages =
    vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests |
    vk::PipelineStageFlagBits2::eAllGraphics;
constexpr vk::AccessFlags2 DepthFeedbackAccess = vk::AccessFlagBits2::eShaderRead |
                                                 vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                                                 vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
// Opt-in (GOW3_DEPTH_FEEDBACK_GENERAL=1) until tested in Uncharted 1 and 2; it removed the
// validation hazards in Uncharted 3 but not its flicker.
bool DepthFeedbackGeneral() {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_DEPTH_FEEDBACK_GENERAL");
        return env && env[0] == '1';
    }();
    return enabled;
}
// gow3 21 Oct 2026 (GOW3_CPU_TRIMS=1, low-end CPUs): small savings on the GPU command thread's
// per-draw path that change no result (each one names this switch).
bool CpuTrims() {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_CPU_TRIMS");
        return env && env[0] == '1';
    }();
    return enabled;
}
// gow3: live trigger files (some are set for every player) are looked for at most every 250 ms;
// a file-system call per submission or frame is a kernel round trip each time.
bool TriggerPollDue(std::chrono::steady_clock::time_point& last) {
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::milliseconds(250)) {
        return false;
    }
    last = now;
    return true;
}
} // namespace
namespace {
bool SrtCheckEnabled();
void NoteSrtWalk(const Shader::Info& info);
extern Rasterizer* g_srt_check_rasterizer;
bool LateConstantsEnabled();
void NoteLateGuestCopy(VideoCore::Buffer& buffer, u64 offset, VAddr address, u32 size, u64 due);
void NoteLateFlatCopy(VideoCore::Buffer& buffer, u64 offset, const Shader::Info& stage, u32 size,
                      u64 due);
void NoteLateDue(u64 ring_end);
void RefreshLateConstants(Core::MemoryManager* memory);
} // namespace

Rasterizer::Rasterizer(const Instance& instance_, Scheduler& scheduler_, Runtime& runtime_,
                       AmdGpu::Liverpool* liverpool_)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_}, page_manager{this},
      buffer_cache{instance, scheduler, runtime, liverpool_, texture_cache, page_manager},
      texture_cache{instance, scheduler, runtime, liverpool_, buffer_cache, page_manager},
      liverpool{liverpool_}, memory{Core::Memory::Instance()},
      pipeline_cache{instance, scheduler, liverpool, buffer_cache.GetSparsePageShift()},
      host_markers_enabled{EmulatorSettings.IsVkHostMarkersEnabled()},
      guest_markers_enabled{EmulatorSettings.IsVkGuestMarkersEnabled()} {
    // Before the rasterizer is bound: Liverpool enqueues buffers only once it sees it.
    draw_prep = std::make_unique<DrawPreparation>(pipeline_cache);
    // The effect shaders' garbage V# rule (shader_recompiler/resource.h) needs the guest mappings.
    Shader::gpu_mapped_extent = [](u64 address, u64 size) {
        return Core::Memory::Instance()->ClampRangeSize(address, size);
    };
    scene_targets = std::make_unique<SceneTargets>(instance, scheduler, runtime, texture_cache);
    // Object motion first: it fixes the buffer addresses the motion shader variants embed.
    object_motion = std::make_unique<ObjectMotion>(instance, scheduler);
    camera_motion = std::make_unique<CameraMotion>(instance, scheduler, texture_cache, runtime);
    camera_motion->SetObjectMotion(object_motion.get());
    upscaler = std::make_unique<TemporalUpscaler>(instance, scheduler, texture_cache, runtime,
                                                  *camera_motion, *scene_targets);
    if (!EmulatorSettings.IsNullGPU()) {
        liverpool->BindRasterizer(this);
        // gow3: compute shaders seen ahead of this thread compile on the compiler workers.
        liverpool->SetComputePrecompileHook(
            [this](const AmdGpu::ComputeProgram& cs) { pipeline_cache.PrecompileCompute(cs); });
    }
    memory->SetRasterizer(this);
    static Rasterizer* srt_rasterizer;
    static const bool srt_sync = [] {
        const char* env = std::getenv("GOW3_SRT_SYNC");
        return !env || env[0] != '0';
    }();
    if (srt_sync) {
        srt_rasterizer = this;
        Shader::g_srt_pre_walk = [](const Shader::Info& info) { srt_rasterizer->SyncSrtTables(info); };
    }
    static Rasterizer* report_rasterizer;
    report_rasterizer = this;
    Shader::g_srt_fault_report = [](const Shader::Info& info) { report_rasterizer->ReportSrtFault(info); };
    if (SrtCheckEnabled()) {
        g_srt_check_rasterizer = this;
        Shader::g_srt_post_walk = &NoteSrtWalk;
    }

    GpuProfiler::Init(instance, scheduler);
    if (DrawPipeWanted()) {
        constant_ring = std::make_unique<ConstantRing>(instance, scheduler);
        draw_pipe = std::make_unique<DrawPipe>(&RunDrawPacket, this);
    }
    // gow3: this thread joins the texture binding helper before it changes image state.
    runtime.SetImageAccessHook(&JoinBindHelper, this);
    scheduler.SetSubmitCallback([this](Vulkan::SubmitInfo& info) {
        if (LateConstantsEnabled()) {
            RefreshLateConstants(memory);
        }
        runtime.FlushBarriers();
        buffer_cache.SubmitPendingArenaBinds(info);
    });
}

Rasterizer::~Rasterizer() {
    liverpool->SetComputePrecompileHook({}); // before the pipeline cache goes
    Shader::g_srt_pre_walk = nullptr;
}

inline const AmdGpu::Regs& Rasterizer::Regs() const {
    return stage_regs ? *stage_regs : liverpool->regs;
}

inline AmdGpu::CbDbExtent Rasterizer::CbExtent(u32 index) const {
    return stage_regs ? pipe_cb_extent[index] : liverpool->last_cb_extent[index];
}

inline const AmdGpu::ComputeProgram& Rasterizer::CsRegs() const {
    return stage_cs ? *stage_cs : liverpool->GetCsRegs();
}

inline AmdGpu::CbDbExtent Rasterizer::DbExtent() const {
    return stage_regs ? pipe_db_extent : liverpool->last_db_extent;
}

namespace {
// gow3 16 Oct: late constants (GOW3_LATE_CONSTANTS=1). Uncharted 3 fills some per-object constants
// after it submitted the draws that read them: a PS4 GPU reads them when it runs the draw, here
// they were copied when the draw was processed. About 0.7% of resource-table walks read zeros the
// game replaced before the submission (GOW3_SRT_CHECK), and objects flickered as flat-coloured
// shapes. Constant copies and flattened resource tables made for the GPU are compared with guest
// memory right before their command buffer is submitted, and refreshed where the game changed them.
// gow3 21 Oct 2026: made cheap enough for multithreaded rendering (Uncharted 2 needs it there:
// Drake, his gun and train cars drew flat white for single frames). Before, every copy took one
// global lock and allocated a copy of its user data, and at each submission the submitting thread
// (the draw recording thread, which limits the frame rate with multithreaded rendering) copied
// every source again and compared it with the copy, ~2 million copies per 5 s, ~0.05% changed.
// Now a note is a fixed record in blocks that never move, with an XXH3 hash of the bytes copied
// (taken by the thread that made the copy, mostly the GPU command thread, which waits for the
// recording thread most of the time). The check hashes each distinct source of the submission
// once (a guest range, or a table walk with its user data) and compares hashes; only copies whose
// source changed are copied or walked again. Copies into the constant ring are checked at the
// submission of their own draw: the GPU command thread copies up to a frame ahead of the draw
// recording thread, and before, the next submission checked them, often long before their draw
// was submitted (GOW3_LATE_CONSTANTS_DEFER=0: as before).
struct LateConstant {
    VideoCore::Buffer* buffer;
    u64 offset;
    u64 due;                      // constant ring copies: ring position after the copy (else 0)
    u64 hash;                     // XXH3 of the bytes copied
    VAddr address;                // guest bytes copied (0: flattened user data)
    Shader::PFN_SrtWalker walker; // flattened user data: the walk to repeat
    u32 size;
    u32 ud_size;
    std::array<u32, Shader::NUM_USER_DATA_REGS> user_data; // the first ud_size are set
};

// Notes in blocks: the copying thread appends under the lock; the submitting thread reads the
// notes published so far without it (a block never moves) and recycles the blocks it passed.
class LateQueue {
public:
    static constexpr u32 BlockSize = 2048;
    static constexpr u64 MaxPending = 200000; // as before: beyond this, copies go unchecked

    template <typename Fill>
    void Push(Fill&& fill) {
        std::scoped_lock lock{mutex};
        if (pending >= MaxPending) {
            return;
        }
        if (!tail || tail->count == BlockSize) {
            Block* block = TakeBlock();
            if (tail) {
                tail->next = block;
            } else {
                head = block;
                read = 0;
            }
            tail = block;
        }
        fill(tail->items[tail->count++]);
        ++pending;
    }

    /// The submitting thread: `process` on each note in order while `due` holds for it, then
    /// `done_reading` (the notes may be reused after it).
    template <typename Due, typename Process, typename DoneReading>
    void Consume(Due&& due, Process&& process, DoneReading&& done_reading) {
        Block* block;
        Block* end;
        u32 at, end_count;
        {
            std::scoped_lock lock{mutex};
            if (!head) {
                return;
            }
            block = head;
            at = read;
            end = tail;
            end_count = tail->count;
        }
        u64 done = 0;
        for (;;) {
            const u32 count = block == end ? end_count : BlockSize;
            if (at == count) {
                if (block == end) {
                    break;
                }
                block = block->next;
                at = 0;
                continue;
            }
            const LateConstant& entry = block->items[at];
            if (!due(entry)) {
                break;
            }
            process(entry);
            ++at;
            ++done;
        }
        done_reading();
        std::scoped_lock lock{mutex};
        while (head != block) {
            Block* passed = head;
            head = passed->next;
            passed->count = 0;
            passed->next = nullptr;
            free_blocks.push_back(passed);
        }
        read = at;
        if (head == tail && read == head->count) {
            head->count = 0; // all read: appends start at the block's start again
            read = 0;
        }
        pending -= done;
    }

    [[nodiscard]] u64 Pending() {
        std::scoped_lock lock{mutex};
        return pending;
    }

private:
    struct Block {
        std::array<LateConstant, BlockSize> items;
        u32 count;
        Block* next;
    };

    Block* TakeBlock() {
        Block* block;
        if (free_blocks.empty()) {
            block = new Block; // kept for the process
        } else {
            block = free_blocks.back();
            free_blocks.pop_back();
        }
        block->count = 0;
        block->next = nullptr;
        return block;
    }

    std::mutex mutex;
    Block* head = nullptr; // first block with notes not yet read
    Block* tail = nullptr; // appended to
    u32 read = 0;          // next note to read in head
    u64 pending = 0;
    std::vector<Block*> free_blocks;
};

LateQueue g_late_ring;                 // constant ring copies, in ring order (GPU command thread)
LateQueue g_late_now;                  // other copies: checked at the next submission
std::atomic<u64> g_late_due_end{0};    // ring position after the draw being recorded
std::atomic<u64> g_late_refreshed{0}, g_late_checked{0}, g_late_bytes{0}, g_late_sources{0},
    g_late_walks{0}, g_late_refreshed_walks{0};
// GOW3_LATE_CONSTANTS_VERIFY=1 (diagnostics, costs what the old check did): each check also reads
// its source again and compares bytes with the copy, as before 21 Oct: copies changed since the
// note (should be 0) and checks where the byte compare and the hash compare disagree.
std::atomic<u64> g_late_verified{0}, g_late_copy_changed{0}, g_late_missed{0}, g_late_extra{0};

bool LateConstantsVerify() {
    static const bool verify = [] {
        const char* env = std::getenv("GOW3_LATE_CONSTANTS_VERIFY");
        return env && env[0] == '1';
    }();
    return verify;
}

bool LateConstantsEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_LATE_CONSTANTS");
        return env && env[0] == '1';
    }();
    return enabled;
}

bool LateConstantsDefer() {
    static const bool defer = [] {
        const char* env = std::getenv("GOW3_LATE_CONSTANTS_DEFER");
        return !(env && env[0] == '0');
    }();
    return defer;
}

/// `due`: the constant ring position after the copy (checked at the submission of its draw), or 0
/// (checked at the next submission).
void NoteLateGuestCopy(VideoCore::Buffer& buffer, u64 offset, VAddr address, u32 size, u64 due) {
    const u64 hash = XXH3_64bits(buffer.mapped_data.data() + offset, size);
    (due ? g_late_ring : g_late_now).Push([&](LateConstant& entry) {
        entry.buffer = &buffer;
        entry.offset = offset;
        entry.due = due;
        entry.hash = hash;
        entry.address = address;
        entry.walker = nullptr;
        entry.size = size;
        entry.ud_size = 0;
    });
}

void NoteLateFlatCopy(VideoCore::Buffer& buffer, u64 offset, const Shader::Info& stage, u32 size,
                      u64 due) {
    if (!stage.srt_info.walker_func) {
        return; // only user data registers: nothing in memory to change
    }
    const auto user_data = stage.UserData();
    const u32 ud_size = static_cast<u32>(std::min<size_t>(user_data.size(), Shader::NUM_USER_DATA_REGS));
    const u64 hash = XXH3_64bits(buffer.mapped_data.data() + offset, size);
    (due ? g_late_ring : g_late_now).Push([&](LateConstant& entry) {
        entry.buffer = &buffer;
        entry.offset = offset;
        entry.due = due;
        entry.hash = hash;
        entry.address = 0;
        entry.walker = stage.srt_info.walker_func;
        entry.size = size;
        entry.ud_size = ud_size;
        std::memcpy(entry.user_data.data(), user_data.data(), ud_size * sizeof(u32));
    });
}

/// The draw recording thread, per packet: copies up to `ring_end` belong to draws recorded by now.
void NoteLateDue(u64 ring_end) {
    g_late_due_end.store(ring_end, std::memory_order_release);
}

/// The flattened tables `entry` would read now (user data, then the walk), in `flat`; false
/// when the walk faulted (stale pointers: left as copied, as before).
bool WalkLateTables(const LateConstant& entry, u32* flat) {
    const size_t dwords = entry.size / sizeof(u32);
    std::fill(flat, flat + dwords, 0u);
    std::memcpy(flat, entry.user_data.data(), std::min<size_t>(entry.ud_size, dwords) * sizeof(u32));
    sigjmp_buf recover;
    if (sigsetjmp(recover, 0)) {
        runtime_fault_recover = nullptr;
        return false;
    }
    runtime_fault_recover = &recover;
    entry.walker(entry.user_data.data(), flat);
    runtime_fault_recover = nullptr;
    return true;
}

/// Runs in Scheduler::SubmitExecution under its submit lock (one submitting thread at a time).
void RefreshLateConstants(Core::MemoryManager* memory) {
    // Distinct sources of this submission (open addressing on a key hash): what each reads now.
    struct Source {
        u64 key;
        const LateConstant* first; // null: free slot
        u64 hash;                  // XXH3 of the bytes now
        bool valid;                // false: the walk faulted
    };
    static constexpr u32 TableSize = 16384, TableLimit = 12288;
    static std::vector<Source> table(TableSize);
    static std::vector<u32> used;
    static std::vector<u8> now, refreshed;
    u64 checked = 0, changed = 0, bytes = 0, sources = 0, walks = 0, changed_walks = 0;
    u64 verified = 0, copy_changed = 0, missed = 0, extra = 0;
    const auto same_source = [](const LateConstant& a, const LateConstant& b) {
        if (a.address || b.address) {
            return a.address == b.address && a.size == b.size;
        }
        return a.walker == b.walker && a.size == b.size && a.ud_size == b.ud_size &&
               std::memcmp(a.user_data.data(), b.user_data.data(), a.ud_size * sizeof(u32)) == 0;
    };
    // XXH3 of the bytes `entry`'s source holds now; false when unreadable (faulted walk).
    const auto hash_now = [&](const LateConstant& entry, u64& hash) {
        ++sources;
        if (now.size() < entry.size + sizeof(u32)) {
            now.resize(entry.size + sizeof(u32));
        }
        if (entry.address) {
            memory->CopySparseMemory(entry.address, now.data(), entry.size);
        } else {
            ++walks;
            if (!WalkLateTables(entry, reinterpret_cast<u32*>(now.data()))) {
                return false;
            }
        }
        hash = XXH3_64bits(now.data(), entry.size);
        return true;
    };
    const auto check = [&](const LateConstant& entry) {
        const u64 key = entry.address
                            ? XXH3_64bits_withSeed(&entry.address, sizeof(entry.address), entry.size)
                            : XXH3_64bits_withSeed(entry.user_data.data(), entry.ud_size * sizeof(u32),
                                                   reinterpret_cast<u64>(entry.walker) ^
                                                       (u64(entry.size) << 40));
        u64 hash = 0;
        bool valid = false;
        u32 slot = static_cast<u32>(key) & (TableSize - 1);
        for (;; slot = (slot + 1) & (TableSize - 1)) {
            Source& source = table[slot];
            if (!source.first) {
                valid = hash_now(entry, hash);
                if (used.size() < TableLimit) {
                    source = {key, &entry, hash, valid};
                    used.push_back(slot);
                }
                break;
            }
            if (source.key == key && same_source(*source.first, entry)) {
                hash = source.hash;
                valid = source.valid;
                break;
            }
        }
        if (!valid) {
            return;
        }
        ++checked;
        u8* dst = entry.buffer->mapped_data.data() + entry.offset;
        if (LateConstantsVerify()) {
            // The old check: the source read again, compared byte for byte with the copy.
            ++verified;
            if (XXH3_64bits(dst, entry.size) != entry.hash) {
                ++copy_changed;
            }
            if (refreshed.size() < entry.size + sizeof(u32)) {
                refreshed.resize(entry.size + sizeof(u32));
            }
            bool read = true;
            if (entry.address) {
                memory->CopySparseMemory(entry.address, refreshed.data(), entry.size);
            } else {
                read = WalkLateTables(entry, reinterpret_cast<u32*>(refreshed.data()));
            }
            if (read) {
                const bool bytes_differ = std::memcmp(dst, refreshed.data(), entry.size) != 0;
                missed += bytes_differ && hash == entry.hash;
                extra += !bytes_differ && hash != entry.hash;
            }
        }
        if (hash == entry.hash) {
            return;
        }
        // Changed since the copy: copied or walked again into it.
        if (entry.address) {
            memory->CopySparseMemory(entry.address, dst, entry.size);
        } else {
            if (refreshed.size() < entry.size + sizeof(u32)) {
                refreshed.resize(entry.size + sizeof(u32));
            }
            if (!WalkLateTables(entry, reinterpret_cast<u32*>(refreshed.data()))) {
                return;
            }
            std::memcpy(dst, refreshed.data(), entry.size);
        }
        entry.buffer->Flush(entry.offset, entry.size);
        ++changed;
        changed_walks += !entry.address;
        bytes += entry.size;
    };
    // The table points into the queue's blocks: emptied before the queue recycles them.
    const auto clear_table = [] {
        for (const u32 slot : used) {
            table[slot].first = nullptr;
        }
        used.clear();
    };
    const u64 due_end =
        LateConstantsDefer() ? g_late_due_end.load(std::memory_order_acquire) : ~u64{0};
    g_late_now.Consume([](const LateConstant&) { return true; }, [&](const LateConstant& entry) {
        check(entry);
    }, clear_table);
    g_late_ring.Consume([due_end](const LateConstant& entry) { return entry.due <= due_end; },
                        [&](const LateConstant& entry) { check(entry); }, clear_table);
    g_late_checked.fetch_add(checked, std::memory_order_relaxed);
    g_late_refreshed.fetch_add(changed, std::memory_order_relaxed);
    g_late_bytes.fetch_add(bytes, std::memory_order_relaxed);
    g_late_sources.fetch_add(sources, std::memory_order_relaxed);
    g_late_walks.fetch_add(walks, std::memory_order_relaxed);
    g_late_refreshed_walks.fetch_add(changed_walks, std::memory_order_relaxed);
    if (verified) {
        g_late_verified.fetch_add(verified, std::memory_order_relaxed);
        g_late_copy_changed.fetch_add(copy_changed, std::memory_order_relaxed);
        g_late_missed.fetch_add(missed, std::memory_order_relaxed);
        g_late_extra.fetch_add(extra, std::memory_order_relaxed);
    }
    static auto last = std::chrono::steady_clock::now();
    if (const auto t = std::chrono::steady_clock::now(); t - last >= std::chrono::seconds{5}) {
        last = t;
        const u64 refreshed_all = g_late_refreshed.exchange(0);
        const u64 refreshed_walks = g_late_refreshed_walks.exchange(0);
        std::printf("Late constants (5 s): %llu copies checked before submission, %llu refreshed "
                    "(%.1f KB; %llu guest copies, %llu table walks); %llu distinct sources read "
                    "(%llu table walks), %llu ring copies waiting for their draw\n",
                    static_cast<unsigned long long>(g_late_checked.exchange(0)),
                    static_cast<unsigned long long>(refreshed_all), g_late_bytes.exchange(0) / 1e3,
                    static_cast<unsigned long long>(refreshed_all - refreshed_walks),
                    static_cast<unsigned long long>(refreshed_walks),
                    static_cast<unsigned long long>(g_late_sources.exchange(0)),
                    static_cast<unsigned long long>(g_late_walks.exchange(0)),
                    static_cast<unsigned long long>(g_late_ring.Pending()));
        if (LateConstantsVerify()) {
            std::printf("Late constants verify (5 s): %llu checks compared byte for byte; %llu copies "
                        "changed since their note, %llu changes the hash check missed, %llu hash "
                        "changes with equal bytes\n",
                        static_cast<unsigned long long>(g_late_verified.exchange(0)),
                        static_cast<unsigned long long>(g_late_copy_changed.exchange(0)),
                        static_cast<unsigned long long>(g_late_missed.exchange(0)),
                        static_cast<unsigned long long>(g_late_extra.exchange(0)));
        }
        std::fflush(stdout);
    }
}

// gow3 diagnostics (GOW3_SRT_CHECK=1): a sample of resource-table walks (user data and the
// flattened result) is walked again at the next submission. A table the game changed in between
// was completed after the draw was processed: the walk read old descriptors (5 s statistics).
Rasterizer* g_srt_check_rasterizer = nullptr;

struct SrtWalkCheck {
    Shader::PFN_SrtWalker walker;
    size_t walker_size;
    u64 hash;
    std::vector<u32> user_data;
    std::vector<u32> flat;
    s64 walk_ns;
    // gow3 21 Oct 2026 (U3 museum red carpet): who walked (a draw-preparation worker walks ahead
    // of the GPU thread, and the GPU thread walks again and compares) and, for the GPU thread,
    // the draw: a late table only matters when the walk the GPU uses read it.
    bool worker;
    Rasterizer::SrtDrawContext draw;
};
s64 SrtNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
std::mutex g_srt_check_mutex;
std::vector<SrtWalkCheck> g_srt_checks;

bool SrtCheckEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_SRT_CHECK");
        return env && env[0] == '1';
    }();
    return enabled;
}

void NoteSrtWalk(const Shader::Info& info) {
    static std::atomic<u32> sample{0};
    if ((sample.fetch_add(1, std::memory_order_relaxed) & 3) != 0) {
        return;
    }
    // Draw-preparation workers walk with a fault recovery point set (PipelineCache::GetProgram).
    const bool worker = runtime_fault_recover != nullptr;
    const Rasterizer::SrtDrawContext draw =
        !worker && g_srt_check_rasterizer && info.sw_stage != Shader::SwStage::Compute
            ? g_srt_check_rasterizer->CurrentDrawContext()
            : Rasterizer::SrtDrawContext{};
    std::scoped_lock lock{g_srt_check_mutex};
    if (g_srt_checks.size() >= 50000) {
        return;
    }
    g_srt_checks.push_back({info.srt_info.walker_func, info.srt_info.walker_func_size, info.pgm_hash,
                            std::vector<u32>(info.user_data.begin(), info.user_data.end()),
                            info.flattened_ud_buf, SrtNowNs(), worker, draw});
}

void CheckSrtWalks() {
    if (!SrtCheckEnabled()) {
        return;
    }
    std::vector<SrtWalkCheck> checks;
    {
        std::scoped_lock lock{g_srt_check_mutex};
        checks.swap(g_srt_checks);
    }
    static std::map<u64, std::pair<u64, u64>> stats; // shader -> (walks checked, changed)
    static std::map<std::string, u64> kinds;           // what the caches knew about the source
    static std::array<u64, 5> ages{};                  // walk to check: <2, <5, <10, <20, more ms
    static std::map<u64, u64> windows;                 // 1 MiB window of the source
    static auto last = std::chrono::steady_clock::now();
    static u32 described = 0;
    static u64 changed_gpu = 0, changed_worker = 0; // gow3 21 Oct 2026: by who walked
    std::vector<u32> now;
    for (const auto& check : checks) {
        now.assign(check.flat.size(), 0);
        std::memcpy(now.data(), check.user_data.data(),
                    std::min(check.user_data.size(), now.size()) * sizeof(u32));
        sigjmp_buf recover;
        if (sigsetjmp(recover, 0)) {
            runtime_fault_recover = nullptr;
            continue;
        }
        runtime_fault_recover = &recover;
        check.walker(check.user_data.data(), now.data());
        runtime_fault_recover = nullptr;
        auto& s = stats[check.hash];
        ++s.first;
        if (now != check.flat) {
            ++s.second;
            size_t first = 0;
            u32 differ = 0;
            for (size_t i = 0; i < now.size(); ++i) {
                if (now[i] != check.flat[i]) {
                    if (!differ++) {
                        first = i;
                    }
                }
            }
            const char* kind = "unknown";
            VAddr source = 0;
            if (g_srt_check_rasterizer) {
                source = g_srt_check_rasterizer->SrtSourceAddress(check.walker, check.walker_size,
                                                                  check.user_data, u32(first), &kind);
            }
            ++kinds[kind];
            ++windows[source >> 20];
            {
                const s64 age_ms = (SrtNowNs() - check.walk_ns) / 1000000;
                ++ages[age_ms < 2 ? 0 : age_ms < 5 ? 1 : age_ms < 10 ? 2 : age_ms < 20 ? 3 : 4];
            }
            // GOW3_SRT_WATCH=1: watch the first late-changed source for the game code writing it.
            static const bool watch = [] {
                const char* env = std::getenv("GOW3_SRT_WATCH");
                return env && env[0] == '1';
            }();
            static bool watching = false;
            if (watch && !watching && source) {
                watching = true;
                runtime_exec_watch_retarget(source);
            }
            ++(check.worker ? changed_worker : changed_gpu);
            // gow3 21 Oct 2026: GOW3_SRT_CHECK_LINES=<n> (default 16) such lines per run; GPU-thread
            // walks name their draw (colour target 0, depth, index buffer and count) to match a
            // frame capture's passes and meshes.
            static const u32 max_lines = [] {
                const char* env = std::getenv("GOW3_SRT_CHECK_LINES");
                return env ? u32(std::strtoul(env, nullptr, 10)) : 16u;
            }();
            if (described < max_lines) {
                ++described;
                std::printf("SRT check: shader %016llx table changed after the walk: %u of %zu dwords, first "
                            "dword %zu %#x -> %#x, from %#llx (%s); %s walk, %llu ms old%s\n",
                            static_cast<unsigned long long>(check.hash), differ, now.size(), first,
                            check.flat[first], now[first], static_cast<unsigned long long>(source), kind,
                            check.worker ? "worker" : "GPU thread",
                            static_cast<unsigned long long>((SrtNowNs() - check.walk_ns) / 1000000),
                            check.worker
                                ? ""
                                : fmt::format(", draw color0 {:#x} depth {:#x} ib {:#x} n {}",
                                              check.draw.color0, check.draw.depth, check.draw.index,
                                              check.draw.num_indices)
                                      .c_str());
            }
        }
    }
    if (const auto t = std::chrono::steady_clock::now(); t - last >= std::chrono::seconds{5}) {
        last = t;
        std::vector<std::pair<u64, u64>> changed;
        u64 walks = 0, total_changed = 0;
        for (const auto& [hash, s] : stats) {
            walks += s.first;
            total_changed += s.second;
            if (s.second) {
                changed.emplace_back(s.second, hash);
            }
        }
        std::ranges::sort(changed, std::greater{});
        std::string top;
        for (size_t i = 0; i < std::min<size_t>(changed.size(), 6); ++i) {
            top += fmt::format(" {:016x}x{}/{}", changed[i].second, changed[i].first,
                               stats[changed[i].second].first);
        }
        std::string kind_text, window_text;
        for (const auto& [kind, n] : kinds) {
            kind_text += fmt::format(" {} {}", kind, n);
        }
        std::vector<std::pair<u64, u64>> sorted_windows;
        for (const auto& [window, n] : windows) {
            sorted_windows.emplace_back(n, window);
        }
        std::ranges::sort(sorted_windows, std::greater{});
        for (size_t i = 0; i < std::min<size_t>(sorted_windows.size(), 5); ++i) {
            window_text += fmt::format(" {:#x}x{}", sorted_windows[i].second << 20, sorted_windows[i].first);
        }
        std::printf("SRT check (5 s): %llu sampled walks, %llu tables changed by the submission;%s; sources:%s; "
                    "windows:%s\n",
                    static_cast<unsigned long long>(walks), static_cast<unsigned long long>(total_changed),
                    top.c_str(), kind_text.c_str(), window_text.c_str());
        std::printf("SRT check (5 s): changed tables by walk age at the check: <2 ms %llu, 2-5 %llu, 5-10 %llu, "
                    "10-20 %llu, 20+ %llu\n",
                    (unsigned long long)ages[0], (unsigned long long)ages[1], (unsigned long long)ages[2],
                    (unsigned long long)ages[3], (unsigned long long)ages[4]);
        std::printf("SRT check (5 s): changed tables by walker: GPU thread %llu (the draws used them), "
                    "draw-preparation workers %llu (re-walked by the GPU thread)\n",
                    (unsigned long long)changed_gpu, (unsigned long long)changed_worker);
        changed_gpu = changed_worker = 0;
        ages = {};
        kinds.clear();
        windows.clear();
        std::fflush(stdout);
        stats.clear();
    }
}

/// gow3: compute shaders whose output the guest reads as soon as the next end-of-pipe event
/// fires (Rasterizer::eop_readbacks): Uncharted 1's Cs_RayTraceGatherHits (the closest hit of
/// each mesh ray cast).
/// GOW3_EOP_READBACK: 0 off, 1 (default) asynchronous with the pages read-protected, 2 waiting
/// for the GPU at the event (diagnostics).
int EopReadbackMode() {
    static const int mode = [] {
        const char* env = std::getenv("GOW3_EOP_READBACK");
        return env && env[0] >= '0' && env[0] <= '2' ? env[0] - '0' : 1;
    }();
    return mode;
}

/// gow3 16 Oct: Uncharted 2's 64-wide copy shader (6a6422a9, run on the GPU: its sources are
/// completed after the frame is queued) writes data the game reads back on the CPU (one value
/// in a 1 KiB block it rewrites every frame). Read back with the submission, that block counted
/// as recurring (readable, a frame old): NPCs floated in the air and Nate sank into the floor,
/// fixed by precise readbacks at ~50 FPS. As an end-of-pipe readback the fence after it waits
/// for it. GOW3_COPY64_EOP_READBACK=1.
bool Copy64EopReadback() {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_COPY64_EOP_READBACK");
        return env && env[0] == '1';
    }();
    return enabled;
}

bool IsEopReadbackShader(u64 hash) {
    return (hash == 0xc92150ba || (hash == 0x6a6422a9 && Copy64EopReadback())) &&
           EopReadbackMode() != 0;
}

/// gow3: a T# that passes the format check but cannot be a real texture: Uncharted 2's treasure
/// effects fetch T#s from resource table slots that hold other data (CPU-written; the shader
/// branches using them are not taken). Built, such an image spans gigabytes or reaches past
/// mapped memory (tracked, uploaded, out of device memory) or has reserved swizzles.
/// The raw fields, before anything decodes them (unknown tile modes, reserved swizzles and
/// image types stop the decoders).
bool PlausibleTSharp(const AmdGpu::Image& tsharp) {
    if (!tsharp.Valid() || tsharp.tiling_index > 26 || tsharp.last_level < tsharp.base_level) {
        return false; // 27-30 reserved, 31 (linear general) unhandled
    }
    for (const u32 swizzle : {u32(tsharp.dst_sel_x), u32(tsharp.dst_sel_y),
                              u32(tsharp.dst_sel_z), u32(tsharp.dst_sel_w)}) {
        if (swizzle == 2 || swizzle == 3) {
            return false;
        }
    }
    return true;
}

/// gow3 20 Oct 2026 (GitHub #63): the host image a description would create, roughly (all
/// layers and slices of the first level, a third more with mips), and its layer count against
/// the device's limit. Garbage descriptions make images of gigabytes: one of 960x540 RGBA8 with
/// 6772 layers (16 GB) took all video memory and the next allocation stopped Drake's Fortune.
bool PlausibleHostImage(const VideoCore::ImageInfo& info, u32 max_layers) {
    if (info.resources.layers == 0 || info.resources.layers > max_layers) {
        return false;
    }
    // Compressed formats: num_bits is per 4x4 block.
    const u64 width = info.props.is_block ? (info.size.width + 3) / 4 : info.size.width;
    const u64 height = info.props.is_block ? (info.size.height + 3) / 4 : info.size.height;
    const u64 bytes = width * height * std::max(info.size.depth, 1u) * info.resources.layers *
                      std::max(info.num_bits / 8u, 1u);
    return bytes + (info.resources.levels > 1 ? bytes / 3 : 0) < 1_GB;
}

bool PlausibleImage(const VideoCore::ImageInfo& info, const AmdGpu::Image& tsharp,
                    Core::MemoryManager* memory, u32 max_layers) {
    if (info.guest_size == 0 || info.guest_size >= 1_GB || !PlausibleHostImage(info, max_layers)) {
        return false;
    }
    return memory->ClampRangeSize(info.guest_address, info.guest_size) == info.guest_size;
}

/// A render target or depth buffer whose registers describe an impossible image: skipped
/// (the draw goes on without that attachment), as garbage T#s are.
bool PlausibleTarget(const VideoCore::ImageInfo& info, u32 max_layers, const char* what) {
    if (info.guest_size < 1_GB && PlausibleHostImage(info, max_layers)) {
        return true;
    }
    static std::atomic<u32> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
        std::printf("GPU: skipping a garbage %s at %#llx: %ux%ux%u, %u layers, %u mips, %u bits\n",
                    what, static_cast<unsigned long long>(info.guest_address), info.size.width,
                    info.size.height, info.size.depth, info.resources.layers, info.resources.levels,
                    info.num_bits);
        std::fflush(stdout);
    }
    return false;
}

/// gow3 test (21 Oct 2026): GOW3_TEST_GARBAGE_TSHARP=<seconds>[,<count>]. After <seconds> from
/// the first texture bind, the next <count> T#s (default 600) become the garbage one that stopped
/// Uncharted 2 on a laptop (BC2, number format 13, a macro-tiled mode; image_info.cpp UpdateSize
/// asserted), to check that such a T# is dropped. Off by default.
void TestGarbageTSharp(AmdGpu::Image& tsharp) {
    struct Params {
        double delay_s = -1.0;
        u32 count = 600;
    };
    static const Params params = [] {
        Params p;
        if (const char* env = std::getenv("GOW3_TEST_GARBAGE_TSHARP"); env && env[0]) {
            p.delay_s = std::atof(env);
            if (const char* comma = std::strchr(env, ',')) {
                p.count = static_cast<u32>(std::strtoul(comma + 1, nullptr, 10));
            }
        }
        return p;
    }();
    if (params.delay_s < 0.0 || tsharp.Address() == 0) {
        return;
    }
    static const auto start = std::chrono::steady_clock::now();
    static std::atomic<u32> injected{0};
    if (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() <
            params.delay_s ||
        injected.load(std::memory_order_relaxed) >= params.count) {
        return;
    }
    tsharp.data_format = u64(AmdGpu::DataFormat::FormatBc2);
    tsharp.num_format = 13;
    for (u32 index = 0; index <= 26; ++index) {
        tsharp.tiling_index = index;
        if (AmdGpu::IsMacroTiled(AmdGpu::GetArrayMode(tsharp.GetTileMode()))) {
            break;
        }
    }
    const u32 n = injected.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1 || n == params.count) {
        std::printf("GPU test: garbage T# %u of %u (BC2, number format 13, tile mode %u) at %#llx\n",
                    n, params.count, u32(tsharp.tiling_index),
                    static_cast<unsigned long long>(tsharp.Address()));
        std::fflush(stdout);
    }
}
} // namespace

namespace {
/// magic_enum::enum_contains, from a table: it scans every enumerator (per texture per draw).
bool IsKnownFormat(AmdGpu::DataFormat data_fmt, AmdGpu::NumberFormat num_fmt) {
    static const auto tables = [] {
        std::pair<std::array<bool, 256>, std::array<bool, 256>> t{};
        for (const auto value : magic_enum::enum_values<AmdGpu::DataFormat>()) {
            if (static_cast<u32>(value) < 256) {
                t.first[static_cast<u32>(value)] = true;
            }
        }
        for (const auto value : magic_enum::enum_values<AmdGpu::NumberFormat>()) {
            if (static_cast<u32>(value) < 256) {
                t.second[static_cast<u32>(value)] = true;
            }
        }
        return t;
    }();
    const u32 d = static_cast<u32>(data_fmt), n = static_cast<u32>(num_fmt);
    return d < 256 && n < 256 && tables.first[d] && tables.second[n];
}
} // namespace

bool Rasterizer::FilterDraw() {
    const auto& regs = Regs();
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::EliminateFastClear) {
        // Clears the render target if FCE is launched before any draws
        EliminateFastClear();
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::FmaskDecompress) {
        // TODO: check for a valid MRT1 to promote the draw to the resolve pass.
        LOG_TRACE(Render_Vulkan, "FMask decompression pass skipped");
        ScopedMarkerInsert("FmaskDecompress");
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Resolve) {
        LOG_TRACE(Render_Vulkan, "Resolve pass");
        Resolve();
        return false;
    }
    if (regs.primitive_type == AmdGpu::PrimitiveType::None) {
        LOG_TRACE(Render_Vulkan, "Primitive type 'None' skipped");
        ScopedMarkerInsert("PrimitiveTypeNone");
        return false;
    }

    const bool cb_disabled =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    const auto depth_copy =
        regs.depth_render_override.force_z_dirty && regs.depth_render_override.force_z_valid &&
        regs.depth_buffer.DepthValid() && regs.depth_buffer.DepthWriteValid() &&
        regs.depth_buffer.DepthAddress() != regs.depth_buffer.DepthWriteAddress();
    const auto stencil_copy =
        regs.depth_render_override.force_stencil_dirty &&
        regs.depth_render_override.force_stencil_valid && regs.depth_buffer.StencilValid() &&
        regs.depth_buffer.StencilWriteValid() &&
        regs.depth_buffer.StencilAddress() != regs.depth_buffer.StencilWriteAddress();
    if (cb_disabled && (depth_copy || stencil_copy)) {
        // Games may disable color buffer and enable force depth/stencil dirty and valid to
        // do a copy from one depth-stencil surface to another, without a pixel shader.
        // We need to detect this case and perform the copy, otherwise it will have no effect.
        LOG_TRACE(Render_Vulkan, "Performing depth-stencil override copy");
        DepthStencilCopy(depth_copy, stencil_copy);
        return false;
    }

    return true;
}

template <typename... Parts>
VideoCore::ImageId Rasterizer::FindTargetMemoized(VideoCore::TextureCache::ImageDesc& desc,
                                                  LastTarget& last, auto&& make_desc,
                                                  const Parts&... parts) {
    static_assert((sizeof(Parts) + ...) <= 256, "target memo key too large");
    std::array<u8, 256> key{};
    u32 key_size = 0;
    const auto append = [&](const auto& part) {
        std::memcpy(key.data() + key_size, &part, sizeof(part));
        key_size += sizeof(part);
    };
    (append(parts), ...);
    const u64 generation = texture_cache.RegistryGeneration();
    const bool memo_enabled = !Gow3Toggle::Disabled(Gow3Toggle::TextureBindingMemo);
    if (memo_enabled && last.generation == generation && last.key_size == key_size &&
        std::memcmp(last.key.data(), key.data(), key_size) == 0) {
        texture_cache.MarkFound(last.image_id);
        return last.image_id;
    }
    const auto remember = [&](VideoCore::ImageId image_id) {
        last.key = key;
        last.key_size = key_size;
        last.generation = generation;
        last.image_id = image_id;
    };
    u64 hash = 0xCBF29CE484222325ull;
    for (u32 i = 0; i < key_size; i += 8) {
        u64 word;
        std::memcpy(&word, key.data() + i, 8);
        hash = (hash ^ word) * 0x100000001B3ull;
        hash ^= hash >> 29;
    }
    auto& memo = target_memo[hash % target_memo.size()];
    if (memo.generation == generation && memo.key_size == key_size &&
        std::memcmp(memo.key.data(), key.data(), key_size) == 0 && memo_enabled) {
        desc = memo.desc;
        texture_cache.MarkFound(memo.image_id);
        remember(memo.image_id);
        return memo.image_id;
    }
    make_desc();
    if (!PlausibleTarget(desc.info, instance.GetMaxImageArrayLayers(),
                         desc.info.props.is_depth ? "depth buffer" : "render target")) {
        last.generation = ~0ULL;
        return {};
    }
    const VideoCore::ImageId image_id = texture_cache.FindImage(desc);
    if (generation == texture_cache.RegistryGeneration()) {
        memo.key = key;
        memo.key_size = key_size;
        memo.generation = generation;
        memo.image_id = image_id;
        memo.desc = desc;
        remember(image_id);
    } else {
        last.generation = ~0ULL;
    }
    return image_id;
}

void Rasterizer::PrepareRenderState(const GraphicsPipeline* pipeline) {
    // Prefetch render targets to handle overlaps with bound textures (e.g. mipgen)
    const auto& key = pipeline->GetGraphicsKey();
    const auto& regs = Regs();
    if (regs.color_control.degamma_enable) {
        LOG_WARNING(Render_Vulkan, "Color buffers require gamma correction");
    }

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    for (s32 cb = 0; cb < std::bit_width(key.mrt_mask); ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (skip_cb_binding || !col_buf || !target_mask || (key.mrt_mask & (1 << cb)) == 0) {
            image_id = {};
            continue;
        }
        const auto& hint = CbExtent(cb);
        const u32 tag = 1;
        const auto found = FindTargetMemoized(
            desc, last_targets[cb], [&] { std::construct_at(&desc, col_buf, hint); }, tag,
            col_buf, hint);
        if (!found) {
            image_id = {}; // garbage registers (FindTargetMemoized): no attachment
            continue;
        }
        image_id = bound_images.emplace_back(found);
        auto& image = texture_cache.GetImage(image_id);
        image.binding.is_target = 1u;
    }

    if (GetEffectiveDepthStencilState(regs).needs_attachment) {
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        const auto& hint = DbExtent();
        auto& [image_id, desc] = db_desc;
        const u32 tag = 2;
        const auto found = FindTargetMemoized(
            desc, last_targets[AmdGpu::NUM_COLOR_BUFFERS],
            [&] {
                std::construct_at(&desc, regs.depth_buffer, regs.depth_view, regs.depth_control,
                                  htile_address, hint);
            },
            tag, regs.depth_buffer, regs.depth_view, regs.depth_control, htile_address, hint);
        image_id = found ? bound_images.emplace_back(found) : VideoCore::ImageId{};
        if (image_id) {
            auto& image = texture_cache.GetImage(image_id);
            image.binding.is_target = 1u;
        }
    } else {
        db_desc.first = {};
    }
    // gow3: the G-buffer pass (5+ color targets) holds the scene depth, and its constants the
    // main camera (shadow passes bind the same layout with the light's camera).
    gbuffer_draw = camera_motion->Enabled() && std::popcount(key.mrt_mask) >= 5 && db_desc.first;
    if (gbuffer_draw) {
        camera_motion->OnGBufferPass(db_desc.first);
    } else if (camera_motion->VelocityMode() && db_desc.first && !camera_motion->Depth()) {
        // Uncharted: no G-buffer of 5 targets. The frame's first pass at the render size that is
        // depth-only (U1's prepass) or draws RGBA16F color (U2's first scene pass) holds the scene
        // depth (after the reference port; the size is the patched render size, ultrawide too).
        const auto& depth = texture_cache.GetImage(db_desc.first);
        const auto [scene_w, scene_h] = camera_motion->SceneSize();
        bool scene_pass = !cb_descs[0].first;
        if (cb_descs[0].first) {
            const auto& color = texture_cache.GetImage(cb_descs[0].first);
            scene_pass = color.info.pixel_format == vk::Format::eR16G16B16A16Sfloat &&
                         color.info.size.width == scene_w && color.info.size.height == scene_h;
        }
        if (scene_pass && depth.info.size.width == scene_w && depth.info.size.height == scene_h &&
            (depth.info.pixel_format == vk::Format::eD32SfloatS8Uint ||
             depth.info.pixel_format == vk::Format::eD32Sfloat)) {
            camera_motion->OnGBufferPass(db_desc.first);
        }
    }
    if (upscaler->Enabled() && cb_descs[0].first) {
        upscaler->OnColorTarget(cb_descs[0].first);
    }
    // gow3: scene color: a full-size RGBA16F target drawn with the scene depth.
    if (upscaler->Enabled() && db_desc.first && db_desc.first == camera_motion->Depth() &&
        cb_descs[0].first && std::popcount(key.mrt_mask) <= 2) {
        const auto& color = texture_cache.GetImage(cb_descs[0].first);
        const auto& depth = texture_cache.GetImage(db_desc.first);
        if (color.info.pixel_format == vk::Format::eR16G16B16A16Sfloat &&
            color.info.size.width == depth.info.size.width &&
            color.info.size.height == depth.info.size.height) {
            upscaler->OnSceneColor(cb_descs[0].first);
            // Blended geometry without depth writes (not full-screen passes): transparents and
            // effects start. Blended layers writing depth (decals, wet/blood ground) are
            // surfaces and stay in the history.
            if (regs.blend_control[0].enable && !regs.color_buffers[0].info.blend_bypass &&
                !regs.depth_control.depth_write_enable &&
                (regs.num_indices > 6 || regs.num_instances.NumInstances() > 1)) {
                upscaler->OnBlendedSceneDraw();
            } else if (regs.num_indices <= 6 && regs.num_instances.NumInstances() <= 1) {
                // A full-screen pass over the scene (the fog composite): the transparents are in.
                upscaler->OnSceneComposite();
            }
        }
    }
}

static std::pair<u32, u32> GetDrawOffsets(
    const AmdGpu::Regs& regs, const Shader::Info& info,
    const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader) {
    u32 vertex_offset = regs.index_offset;
    u32 instance_offset = 0;
    if (fetch_shader) {
        if (vertex_offset == 0 && fetch_shader->vertex_offset_sgpr != -1) {
            vertex_offset = info.UserData()[fetch_shader->vertex_offset_sgpr];
        }
        if (fetch_shader->instance_offset_sgpr != -1) {
            instance_offset = info.UserData()[fetch_shader->instance_offset_sgpr];
        }
    }
    return {vertex_offset, instance_offset};
}

void Rasterizer::EliminateFastClear() {
    auto& col_buf = Regs().color_buffers[0];
    if (!col_buf || !col_buf.info.fast_clear) {
        return;
    }
    VideoCore::TextureCache::ImageDesc desc(col_buf, CbExtent(0));
    if (!PlausibleTarget(desc.info, instance.GetMaxImageArrayLayers(), "fast-clear target")) {
        return; // gow3 (#63): garbage registers, see PlausibleTarget
    }
    const auto image_id = texture_cache.FindImage(desc);
    const auto& image_view = texture_cache.FindRenderTarget(image_id, desc);
    if (!texture_cache.IsMetaCleared(col_buf.CmaskAddress(), col_buf.view.slice_start)) {
        return;
    }
    for (u32 slice = col_buf.view.slice_start; slice <= col_buf.view.slice_max; ++slice) {
        texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);
    }
    auto& image = texture_cache.GetImage(image_id);
    const auto clear_value = LiverpoolToVK::ColorBufferClearValue(col_buf);

    ScopeMarkerBegin(fmt::format("EliminateFastClear:MRT={:#x}:M={:#x}", col_buf.Address(),
                                 col_buf.CmaskAddress()));
    runtime.ClearImage(&image, desc.view_info.range, clear_value);
    ScopeMarkerEnd();
}

// gow3: two-stage draw pipeline (vk_draw_pipe.h). Packet layout, 8-byte aligned parts:
// DrawPacket, u16 block indices, u32 block words (RegDirty::BlockWords each), then per stage a
// PacketStage with its user data and flattened user data, then (verification) the full file.
namespace {
enum class PacketKind : u32 { Draw, Task, Dispatch, Special, Indirect, IndirectDispatch };
struct TaskPacket {
    PacketKind kind;
    u32 size;
    Rasterizer::OrderedTask task;
};
struct DrawPacket {
    PacketKind kind;
    const Pipeline* pipeline;
    const PreparedDraw* prepared;
    u32 index_offset;
    u8 is_indexed;
    u8 regs_reset;
    u16 num_blocks;
    u32 num_stages;
    u32 verify;
    std::array<AmdGpu::CbDbExtent, AmdGpu::NUM_COLOR_BUFFERS> cb_extent;
    AmdGpu::CbDbExtent db_extent;
    u64 ring_end; ///< constant ring position after this packet's copies
};
struct PacketStage {
    const Shader::Info* info;
    VAddr pgm_base;
    u32 ud_size;
    u32 flat_size;
    u32 num_ring;
    u32 pad;
};
constexpr u32 AlignPacket(u32 size) {
    return (size + 7) & ~7u;
}
u32 VerifyInterval() {
    static const u32 interval = [] {
        const char* env = std::getenv("GOW3_PIPE_VERIFY");
        return env ? static_cast<u32>(std::strtoul(env, nullptr, 10)) : 0u;
    }();
    return interval;
}
} // namespace

bool Rasterizer::DrawPipeWanted() {
    const char* env = std::getenv("GOW3_DRAW_PIPE");
    if (env && env[0]) {
        return env[0] == '1';
    }
    // Stage B spins while draws flow. Measured ahead with 16 threads (+19%) and with 4 cores /
    // 8 threads (taskset, Steam Deck-like: +18%).
    return Gow3Threads::Available() >= 8;
}

bool Rasterizer::UseDrawPipe() const {
    return draw_pipe && !host_markers_enabled && !Gow3Toggle::Disabled(Gow3Toggle::DrawPipeline);
}

bool Rasterizer::OnStageA() const {
#ifdef _WIN32
    // gow3 (Windows): libc++ compares std::thread::id through winpthreads' pthread_equal,
    // which asks the kernel (GetThreadId) for both threads: two system calls per check, many
    // per draw. The numeric thread id comes from the TEB.
    return static_cast<u32>(gettid()) == liverpool->GetGpuCommandProcessorThreadId();
#else
    return std::this_thread::get_id() == liverpool->GetGpuCommandProcessorThread();
#endif
}

namespace {
/// Stage A: cycles waited per drain site (GOW3_FRAME_STATS), keyed by function name and line.
std::unordered_map<const char*, std::unordered_map<u32, u64>> drain_sites;
} // namespace

void Rasterizer::DrainDrawPipe(u32 reason, u32 line, const char* function) {
    if (!draw_pipe || !OnStageA()) {
        return;
    }
    static const bool stats = std::getenv("GOW3_FRAME_STATS") != nullptr;
    if (!stats) {
        draw_pipe->Drain(reason);
        return;
    }
    const u64 before = draw_pipe->drain_cycles;
    draw_pipe->Drain(reason);
    if (const u64 waited = draw_pipe->drain_cycles - before) {
        drain_sites[function][line] += waited;
    }
}

bool Rasterizer::IsGpuSideThread() const {
    return OnStageA() || DrawPipe::OnStageB() || PipelineCache::capturing_for_gpu_thread;
}

bool Rasterizer::IsGpuSideThreadId(u32 tid) const {
    // The userfaultfd thread handles the fault while the faulting thread waits: without locks
    // only if that thread owns the caches. The GPU command thread does not while the draw
    // recording thread may be running, so its faults take the locked path of guest threads.
    if (draw_pipe) {
        return tid == draw_pipe->StageBThreadId();
    }
    return tid == liverpool->GetGpuCommandProcessorThreadId();
}

namespace {
// gow3 21 Oct 2026: stale per-object constants with multithreaded rendering (Uncharted 2: Drake,
// his gun, enemies and train cars drawn flat white for single frames; Drake's Fortune: flat grey
// weapons, GitHub #23). The games write those constants with the GPU command stream itself: cs
// 2d215c3b copies them into the frame's constant area (~250-300 dispatches a frame in combat),
// and the next draws read them through their resource tables. The copy runs on the CPU
// (vk_shader_hle.cpp ExecuteScalarCopyHLE), in stream order on one thread when single-threaded;
// with the draw pipe it is a packet run later by stage B, while stage A selects the next draws'
// pipelines at once, walking (flattening) their tables and copying their constant buffers into
// the ring. Walked before stage B had made the copy, a draw kept another object's or the last
// frame's constants. Two holes, each closed by a switch below (on by default, =0 turns it off).

/// GOW3_PIPE_WRITE_END - a buffer a draw or dispatch writes is pending until stage B has run THAT
/// packet. Its position was the pipe head while the packet was being built (CollectRingBindings,
/// before Begin/Commit), i.e. its start: once stage B had run the packet before it (always so
/// when the pipe was idle), PendingWriteOverlaps let a ring copy take the bytes before the write.
bool PipeWriteEnd() {
    static const bool on = [] {
        const char* env = std::getenv("GOW3_PIPE_WRITE_END");
        return !(env && env[0] == '0');
    }();
    return on;
}

/// GOW3_SRT_PENDING_DRAIN - stage A walks a shader's resource tables only after stage B has run
/// the copy shaders queued before the draw that write into them (SyncSrtTables; only GPU-written
/// tables were synced, pending writes guarded ring copies but never walks). Only the copy
/// shaders' destinations are checked (pending_copy_writes, a few merged ranges): checking every
/// pending write per walk cost Uncharted 3's crowd scene (~1,500 writing dispatches a frame)
/// 60 -> 46 FPS on the GPU command thread. U2 at a stage B slowed to a laptop's pace
/// (GOW3_TEST_PIPE_DELAY_NS=2000): 6 white flashes in 535 frames -> 0, 25-48 walks a second
/// wait. 1 (default): wait until stage B has run the packets writing the table; 2: the same, and
/// report what the wait changed (per shader, 5 s).
int SrtPendingDrain() {
    static const int mode = [] {
        const char* env = std::getenv("GOW3_SRT_PENDING_DRAIN");
        if (!env || !env[0]) {
            return 1;
        }
        return env[0] >= '0' && env[0] <= '2' ? env[0] - '0' : 1;
    }();
    return mode;
}

/// The copy shaders stage B runs whose destinations the next draws' resource tables read: the
/// scalar copies 2d215c3b / 6a6422a9 and the copy shader fefebf9f (vk_shader_hle.cpp).
bool IsTableCopyShader(u64 hash) {
    return hash == 0x2d215c3b || hash == 0x6a6422a9 || hash == 0xfefebf9f;
}
} // namespace

void Rasterizer::NotePendingGpuWrite(VAddr address, u64 size) {
    if (!draw_pipe || !size) {
        return;
    }
    // Draws write the same buffers over and over: an entry for the range only moves on.
    const bool uncommitted = noting_packet && PipeWriteEnd();
    const u64 position = uncommitted ? PendingUncommitted : draw_pipe->Head();
    const VAddr end = address + size;
    if (noting_copy) {
        // A copy shader's destination, for the resource table walks (SyncSrtTables): pending
        // until stage B has run the copy's packet (its end, set at the commit).
        copy_uncommitted |= noting_packet;
        const u64 copy_position = noting_packet ? PendingUncommitted : draw_pipe->Head();
        copy_min = std::min(copy_min, address);
        copy_max = std::max(copy_max, end);
        bool merged = false;
        for (auto& write : pending_copy_writes) {
            if (address <= write.end && write.begin <= end) {
                write.begin = std::min(write.begin, address);
                write.end = std::max(write.end, end);
                write.position = copy_position;
                merged = true;
                break;
            }
        }
        if (!merged) {
            pending_copy_writes.push_back({address, end, copy_position});
        }
    }
    for (u32 i = 0; i < pending_writes.size(); ++i) {
        auto& write = pending_writes[i];
        if (address <= write.end && write.begin <= end) {
            write.begin = std::min(write.begin, address);
            write.end = std::max(write.end, end);
            write.position = position;
            pending_min = std::min(pending_min, address);
            pending_max = std::max(pending_max, end);
            if (uncommitted) {
                uncommitted_indices.push_back(i);
            }
            return;
        }
    }
    if (uncommitted) {
        uncommitted_indices.push_back(static_cast<u32>(pending_writes.size()));
    }
    pending_writes.push_back({address, end, position});
    pending_min = std::min(pending_min, address);
    pending_max = std::max(pending_max, end);
}

bool Rasterizer::PendingCopyWriteOverlaps(VAddr address, u64 size) {
    if (pending_copy_writes.empty() || address >= copy_max || address + size <= copy_min) {
        return false;
    }
    // Entries stage B has written are dropped (the list stays a few entries long).
    std::erase_if(pending_copy_writes, [&](const PendingWrite& write) {
        return write.position != PendingUncommitted && draw_pipe->Reached(write.position);
    });
    copy_min = ~VAddr{0};
    copy_max = 0;
    bool overlaps = false;
    const VAddr end = address + size;
    for (const auto& write : pending_copy_writes) {
        copy_min = std::min(copy_min, write.begin);
        copy_max = std::max(copy_max, write.end);
        overlaps |= address < write.end && write.begin < end;
    }
    return overlaps;
}

void Rasterizer::WaitPendingWrites(const std::vector<PendingWrite>& writes, VAddr address,
                                   u64 size) {
    u64 until = 0;
    const VAddr end = address + size;
    for (const auto& write : writes) {
        if (address < write.end && write.begin < end && write.position != PendingUncommitted) {
            until = std::max(until, write.position);
        }
    }
    for (u32 spins = 0; !draw_pipe->Reached(until); ++spins) {
        if (spins < 4096) {
            __builtin_ia32_pause();
        } else {
            std::this_thread::yield();
        }
    }
}

bool Rasterizer::PendingWriteOverlaps(VAddr address, u64 size) {
    if (pending_writes.empty() || address >= pending_max || address + size <= pending_min) {
        return false;
    }
    // Now and then drop entries whose writes the recording thread has done: the bounds shrink.
    // (Not while PostDraw notes a packet: uncommitted_indices point into the vector.)
    if ((++pending_checks & 63) == 0 && uncommitted_indices.empty()) {
        std::erase_if(pending_writes, [&](const PendingWrite& write) {
            return draw_pipe->Reached(write.position);
        });
        pending_min = ~VAddr{0};
        pending_max = 0;
        for (const auto& write : pending_writes) {
            pending_min = std::min(pending_min, write.begin);
            pending_max = std::max(pending_max, write.end);
        }
    }
    const VAddr end = address + size;
    for (const auto& write : pending_writes) {
        if (address < write.end && write.begin < end && !draw_pipe->Reached(write.position)) {
            return true;
        }
    }
    return false;
}

void Rasterizer::CollectRingBindings(const Shader::Info& stage, const PreparedDraw* prepared,
                           boost::container::static_vector<RingBinding, Shader::NUM_BUFFERS>& out) {
    const PreparedStage* prepared_stage = nullptr;
    if (prepared && !Gow3Toggle::Disabled(Gow3Toggle::PreparedResources)) {
        for (u32 i = 0; i < prepared->num_stages; ++i) {
            const auto& candidate = prepared->stages[i];
            if (&candidate.program->info == &stage &&
                candidate.num_buffers == stage.buffers.size()) {
                prepared_stage = &candidate;
                break;
            }
        }
    }
    const u64 alignment = std::max<u64>(instance.StorageMinAlignment(),
                                        instance.UniformMinAlignment());
    // gow3 diagnostics (GOW3_RING_STATS=1): guest copies per 5 s, their sizes, and how many
    // repeat a range already copied since the last flush.
    static const bool ring_stats = [] {
        const char* env = std::getenv("GOW3_RING_STATS");
        return env && env[0] == '1';
    }();
    if (ring_stats) {
        static auto last = std::chrono::steady_clock::now();
        if (const auto now = std::chrono::steady_clock::now(); now - last >= std::chrono::seconds(5)) {
            last = now;
            std::printf("Ring copies (5 s): %llu copies, %.1f MB; repeated within a flush: %llu, "
                        "%.1f MB; sizes <=256 %llu, <=1K %llu, <=4K %llu, <=16K %llu\n",
                        (unsigned long long)ring_copy_count, ring_copy_bytes / 1e6,
                        (unsigned long long)ring_repeat_count, ring_repeat_bytes / 1e6,
                        (unsigned long long)ring_size_buckets[0], (unsigned long long)ring_size_buckets[1],
                        (unsigned long long)ring_size_buckets[2], (unsigned long long)ring_size_buckets[3]);
            std::fflush(stdout);
            ring_copy_count = ring_copy_bytes = ring_repeat_count = ring_repeat_bytes = 0;
            ring_size_buckets = {};
        }
    }
    const auto copy = [&](u32 index, const void* source, VAddr address, u64 size) {
        if (ring_stats && !source) {
            ++ring_copy_count;
            ring_copy_bytes += size;
            ++ring_size_buckets[size <= 256 ? 0 : size <= 1024 ? 1 : size <= 4096 ? 2 : 3];
            if (const u64 flushes = flush_count.load(std::memory_order_relaxed);
                ring_seen_flush != flushes) {
                ring_seen.clear();
                ring_seen_flush = flushes;
            }
            if (!ring_seen.insert((u64(address) << 16) ^ size).second) {
                ++ring_repeat_count;
                ring_repeat_bytes += size;
            }
        }
        const auto offset = constant_ring->Allocate(size, alignment);
        if (!offset) {
            return;
        }
        u8* dst = constant_ring->Data(*offset);
        if (source) {
            std::memcpy(dst, source, size);
        } else {
            memory->CopySparseMemory(address, dst, size);
        }
        constant_ring->Flush(*offset, size);
        if (LateConstantsEnabled()) {
            // gow3 21 Oct 2026: checked at the submission of this draw (its ring position).
            const u64 due = constant_ring->Position();
            if (source) {
                NoteLateFlatCopy(constant_ring->Storage(), *offset, stage, static_cast<u32>(size), due);
            } else {
                NoteLateGuestCopy(constant_ring->Storage(), *offset, address, static_cast<u32>(size),
                                  due);
            }
        }
        out.push_back({index, static_cast<u32>(size), *offset, address});
    };
    for (u32 index = 0; index < stage.buffers.size(); ++index) {
        const auto& desc = stage.buffers[index];
        if (desc.IsSpecial()) {
            if (desc.buffer_type == Shader::BufferType::Flatbuf &&
                !stage.flattened_ud_buf.empty()) {
                copy(index, stage.flattened_ud_buf.data(), 0,
                     stage.flattened_ud_buf.size() * sizeof(u32));
            }
            continue;
        }
        const auto vsharp = prepared_stage ? prepared_stage->buffer_sharps[index]
                                           : desc.GetSharp(stage);
        const VAddr address = vsharp.base_address;
        if (address == 0 || vsharp.GetSize() == 0) {
            continue;
        }
        const u64 size = memory->ClampRangeSize(address, vsharp.GetSize());
        if (desc.is_written) {
            NotePendingGpuWrite(address, size);
            continue;
        }
        // The stream path of BufferCache::ObtainBuffer, taken here: small, read-only, not
        // written by the GPU (now or by work still queued for the recording thread).
        if (size == 0 || size > VideoCore::BufferCache::STREAM_THRESHOLD ||
            buffer_cache.IsRegionGpuModified(address, size) || PendingWriteOverlaps(address, size)) {
            continue;
        }
        copy(index, nullptr, address, size);
    }
}

void Rasterizer::PostDraw(const Pipeline* pipeline, const PreparedDraw* used_prepared,
                          bool is_indexed, u32 index_offset, const AmdGpu::ComputeProgram* cs,
                          const IndirectDraw* indirect) {
    auto& dirty = liverpool->pipe_dirty;
    if (!pipe_synced) {
        // Stage B has not run yet: its register copy starts as this one.
        pipe_regs = liverpool->regs;
        dirty.Clear();
        pipe_synced = true;
    }
    std::array<u16, AmdGpu::RegDirty::NumBlocks> blocks;
    u32 num_blocks = 0;
#ifdef __GLIBCXX__
    for (size_t block = dirty.blocks._Find_first(); block < dirty.blocks.size();
         block = dirty.blocks._Find_next(block)) {
        blocks[num_blocks++] = static_cast<u16>(block);
    }
#else
    if (dirty.blocks.any()) {
        for (size_t block = 0; block < dirty.blocks.size(); ++block) {
            if (dirty.blocks.test(block)) {
                blocks[num_blocks++] = static_cast<u16>(block);
            }
        }
    }
#endif
    const auto stages =
        pipeline ? pipeline->GetStages() : std::span<const Shader::Info* const>{};
    // Constants: copied here into the ring, the recording thread only binds them.
    thread_local std::array<boost::container::static_vector<RingBinding, Shader::NUM_BUFFERS>,
                            Shader::MaxStageTypes>
        rings;
    u32 num_stages = 0;
    u32 size = sizeof(DrawPacket) + AlignPacket(num_blocks * sizeof(u16)) +
               num_blocks * AmdGpu::RegDirty::BlockWords * sizeof(u32);
    for (const auto* stage : stages) {
        if (stage) {
            auto& ring = rings[num_stages];
            ring.clear();
            if (constant_ring && !Gow3Toggle::Disabled(Gow3Toggle::ConstantRing)) {
                noting_packet = true;
                noting_copy = SrtPendingDrain() != 0 && IsTableCopyShader(stage->pgm_hash);
                CollectRingBindings(*stage, used_prepared, ring);
                noting_packet = noting_copy = false;
            }
            ++num_stages;
            size += sizeof(PacketStage) +
                    AlignPacket(u32(stage->user_data.size() + stage->flattened_ud_buf.size()) *
                                sizeof(u32)) +
                    u32(ring.size() * sizeof(RingBinding));
        }
    }
    if (cs) {
        size += AlignPacket(sizeof(AmdGpu::ComputeProgram));
    }
    if (indirect) {
        size += AlignPacket(sizeof(IndirectDraw));
    }
    const u32 interval = VerifyInterval();
    const bool verify = interval && (draw_pipe->packets % interval) == 0;
    if (verify) {
        size += sizeof(AmdGpu::Regs);
    }

    u8* out = draw_pipe->Begin(size);
    u8* const start = out;
    auto& packet = *reinterpret_cast<DrawPacket*>(out);
    packet.kind = cs         ? (indirect ? PacketKind::IndirectDispatch : PacketKind::Dispatch)
                  : indirect ? PacketKind::Indirect
                  : pipeline ? PacketKind::Draw
                             : PacketKind::Special;
    packet.pipeline = pipeline;
    packet.prepared = used_prepared;
    packet.index_offset = index_offset;
    packet.is_indexed = is_indexed;
    packet.regs_reset = dirty.reset;
    packet.num_blocks = static_cast<u16>(num_blocks);
    packet.num_stages = num_stages;
    packet.verify = verify;
    packet.cb_extent = liverpool->last_cb_extent;
    packet.db_extent = liverpool->last_db_extent;
    packet.ring_end = constant_ring ? constant_ring->Position() : 0;
    out += sizeof(DrawPacket);
    std::memcpy(out, blocks.data(), num_blocks * sizeof(u16));
    out += AlignPacket(num_blocks * sizeof(u16));
    for (u32 i = 0; i < num_blocks; ++i) {
        std::memcpy(out,
                    liverpool->regs.reg_array.data() + blocks[i] * AmdGpu::RegDirty::BlockWords,
                    AmdGpu::RegDirty::BlockWords * sizeof(u32));
        out += AmdGpu::RegDirty::BlockWords * sizeof(u32);
    }
    u32 stage_index = 0;
    for (const auto* stage : stages) {
        if (!stage) {
            continue;
        }
        const auto& ring = rings[stage_index++];
        auto& header = *reinterpret_cast<PacketStage*>(out);
        header = {stage,
                  stage->pgm_base,
                  static_cast<u32>(stage->user_data.size()),
                  static_cast<u32>(stage->flattened_ud_buf.size()),
                  static_cast<u32>(ring.size()),
                  0};
        out += sizeof(PacketStage);
        std::memcpy(out, ring.data(), ring.size() * sizeof(RingBinding));
        out += ring.size() * sizeof(RingBinding);
        std::memcpy(out, stage->user_data.data(), stage->user_data.size_bytes());
        std::memcpy(out + stage->user_data.size_bytes(), stage->flattened_ud_buf.data(),
                    stage->flattened_ud_buf.size() * sizeof(u32));
        out += AlignPacket(u32(stage->user_data.size() + stage->flattened_ud_buf.size()) *
                           sizeof(u32));
    }
    if (cs) {
        std::memcpy(out, cs, sizeof(AmdGpu::ComputeProgram));
        out += AlignPacket(sizeof(AmdGpu::ComputeProgram));
    }
    if (indirect) {
        std::memcpy(out, indirect, sizeof(IndirectDraw));
        out += AlignPacket(sizeof(IndirectDraw));
    }
    if (verify) {
        std::memcpy(out, &liverpool->regs, sizeof(AmdGpu::Regs));
        out += sizeof(AmdGpu::Regs);
    }
    dirty.Clear();
    draw_pipe->Commit(static_cast<u32>(out - start));
    if (!uncommitted_indices.empty() || copy_uncommitted) {
        // GOW3_PIPE_WRITE_END: this packet's writes are done once stage B is past its end.
        const u64 end = draw_pipe->Head();
        for (const u32 i : uncommitted_indices) {
            if (pending_writes[i].position == PendingUncommitted) {
                pending_writes[i].position = end;
            }
        }
        uncommitted_indices.clear();
        if (copy_uncommitted) {
            for (auto& write : pending_copy_writes) {
                if (write.position == PendingUncommitted) {
                    write.position = end;
                }
            }
            copy_uncommitted = false;
        }
    }
    PrintPipeStats();
}

void Rasterizer::RetireSubmission() {
    if (!draw_pipe || !OnStageA()) {
        return;
    }
    while (!pipe_keepalive.empty() && draw_pipe->Reached(pipe_keepalive.front().first)) {
        pipe_keepalive.pop_front();
    }
    if (!draw_pipe->Idle()) {
        pipe_keepalive.emplace_back(draw_pipe->Head(), draw_prep->KeepCurrent());
    }
}

bool Rasterizer::RunInOrder(OrderedTask task, const void* data, u32 size, u64 toggle) {
    // gow3 18 Oct 2026: the ring has one producer, the GPU command thread. On the draw recording
    // thread (an EOP flip: SignalFlip -> SubmitFlipInternal queues the flip from there) every
    // earlier packet has run already, so the task runs here, in stream order; pushed from both
    // threads, two packets could take the same ring slot.
    // (Handing tasks from other threads to the command thread instead was tried: the command
    // thread may be waiting for the very flip it would queue. Not done.)
    if (!UseDrawPipe() || Gow3Toggle::Disabled(toggle) || DrawPipe::OnStageB()) {
        DrainDrawPipe(DrawPipe::ReasonRasterizer);
        task(*this, static_cast<const u8*>(data));
        return false;
    }
    const u32 total = sizeof(TaskPacket) + AlignPacket(size);
    u8* out = draw_pipe->Begin(total);
    *reinterpret_cast<TaskPacket*>(out) = {PacketKind::Task, size, task};
    std::memcpy(out + sizeof(TaskPacket), data, size);
    draw_pipe->Commit(total);
    return true;
}

void Rasterizer::RunDrawPacket(void* context, const u8* data, u32 size) {
    auto& self = *static_cast<Rasterizer*>(context);
    if (*reinterpret_cast<const PacketKind*>(data) == PacketKind::Task) {
        const auto& task = *reinterpret_cast<const TaskPacket*>(data);
        task.task(self, data + sizeof(TaskPacket));
        return;
    }
    const auto& packet = *reinterpret_cast<const DrawPacket*>(data);
    if (LateConstantsEnabled()) {
        // gow3 21 Oct 2026: this draw's constant ring copies are checked at the next submission.
        NoteLateDue(packet.ring_end);
    }
    const u8* in = data + sizeof(DrawPacket);
    const auto* blocks = reinterpret_cast<const u16*>(in);
    in += AlignPacket(packet.num_blocks * sizeof(u16));
    auto& regs = self.pipe_regs;
    if (packet.regs_reset) {
        regs.SetDefaults();
    }
    for (u32 i = 0; i < packet.num_blocks; ++i) {
        std::memcpy(regs.reg_array.data() + blocks[i] * AmdGpu::RegDirty::BlockWords, in,
                    AmdGpu::RegDirty::BlockWords * sizeof(u32));
        in += AmdGpu::RegDirty::BlockWords * sizeof(u32);
    }
    ASSERT(packet.num_stages <= Shader::Info::MaxUdSnapshots);
    for (u32 i = 0; i < packet.num_stages; ++i) {
        const auto& stage = *reinterpret_cast<const PacketStage*>(in);
        in += sizeof(PacketStage);
        self.ring_stages[i] = {stage.info, reinterpret_cast<const RingBinding*>(in),
                               stage.num_ring};
        in += stage.num_ring * sizeof(RingBinding);
        const auto* words = reinterpret_cast<const u32*>(in);
        Shader::Info::ud_snapshots[i] = {stage.info, words, stage.ud_size, words + stage.ud_size,
                                         stage.flat_size, stage.pgm_base};
        in += AlignPacket((stage.ud_size + stage.flat_size) * sizeof(u32));
    }
    Shader::Info::num_ud_snapshots = packet.num_stages;
    self.num_ring_stages = packet.num_stages;
    const AmdGpu::ComputeProgram* cs = nullptr;
    if (packet.kind == PacketKind::Dispatch || packet.kind == PacketKind::IndirectDispatch) {
        cs = reinterpret_cast<const AmdGpu::ComputeProgram*>(in);
        in += AlignPacket(sizeof(AmdGpu::ComputeProgram));
    }
    const IndirectDraw* indirect = nullptr;
    if (packet.kind == PacketKind::Indirect || packet.kind == PacketKind::IndirectDispatch) {
        indirect = reinterpret_cast<const IndirectDraw*>(in);
        in += AlignPacket(sizeof(IndirectDraw));
    }
    if (packet.verify) {
        // The resource tables the GPU thread walked must read the same now: else a write
        // still queued here (WriteData, DMA) changed them after that thread read them.
        static u32 flat_reports = 0;
        for (u32 i = 0; i < packet.num_stages && flat_reports < 32; ++i) {
            const auto& snapshot = Shader::Info::ud_snapshots[i];
            const auto& srt = snapshot.info->srt_info;
            // Stages the pipeline selection did not refresh (no user data) keep their tables.
            if (!srt.walker_func || snapshot.flat_size == 0 || snapshot.user_data_size == 0) {
                continue;
            }
            std::vector<u32> flat(snapshot.flat_size);
            std::memcpy(flat.data(), snapshot.user_data,
                        std::min(snapshot.user_data_size, snapshot.flat_size) * sizeof(u32));
            // Pointers in the tables may be stale by now: a fault only skips the check.
            sigjmp_buf recover;
            if (sigsetjmp(recover, 0)) {
                runtime_fault_recover = nullptr;
                continue;
            }
            runtime_fault_recover = &recover;
            srt.walker_func(snapshot.user_data, flat.data());
            runtime_fault_recover = nullptr;
            if (std::memcmp(flat.data(), snapshot.flat, snapshot.flat_size * sizeof(u32)) != 0) {
                u32 first = 0, count = 0;
                for (u32 w = 0; w < snapshot.flat_size; ++w) {
                    if (flat[w] != snapshot.flat[w]) {
                        first = count++ ? first : w;
                    }
                }
                std::printf("Draw pipe: resource tables of shader %016llx changed after the GPU "
                            "thread read them (%u of %u words, first %u: %#x -> %#x, user data "
                            "%u words)\n",
                            static_cast<unsigned long long>(snapshot.info->pgm_hash), count,
                            snapshot.flat_size, first, snapshot.flat[first], flat[first],
                            snapshot.user_data_size);
                ++flat_reports;
            }
        }
        const auto* live = reinterpret_cast<const AmdGpu::Regs*>(in);
        in += sizeof(AmdGpu::Regs);
        static u32 reports = 0;
        for (u32 block = 0; block < AmdGpu::RegDirty::NumBlocks && reports < 32; ++block) {
            const u32 at = block * AmdGpu::RegDirty::BlockWords;
            if (std::memcmp(regs.reg_array.data() + at, live->reg_array.data() + at,
                            AmdGpu::RegDirty::BlockWords * sizeof(u32)) != 0) {
                for (u32 w = at; w < at + AmdGpu::RegDirty::BlockWords; ++w) {
                    if (regs.reg_array[w] != live->reg_array[w]) {
                        std::printf("Draw pipe: register copy differs at word %#x (%#x vs %#x)\n",
                                    w, regs.reg_array[w], live->reg_array[w]);
                        ++reports;
                        break;
                    }
                }
            }
        }
    }
    ASSERT(static_cast<u32>(in - data) == size);
    stage_regs = &regs;
    self.pipe_cb_extent = packet.cb_extent;
    self.pipe_db_extent = packet.db_extent;
    if (packet.kind == PacketKind::Special) {
        // A draw FilterDraw handles (fast clear elimination, resolve, depth copy, skip).
        FrameCapture::Poll();
        self.scheduler.PopPendingOperations();
        self.FilterDraw();
    } else if (cs) {
        stage_cs = cs;
        const auto* pipeline = static_cast<const ComputePipeline*>(packet.pipeline);
        if (indirect) {
            self.DispatchIndirectRecord(pipeline, indirect->args, indirect->stride);
        } else {
            self.DispatchRecord(pipeline);
        }
        stage_cs = nullptr;
    } else if (indirect) {
        self.DrawIndirectRecord(static_cast<const GraphicsPipeline*>(packet.pipeline),
                                packet.is_indexed, *indirect);
    } else {
        self.DrawRecord(static_cast<const GraphicsPipeline*>(packet.pipeline), packet.prepared,
                        packet.is_indexed, packet.index_offset);
    }
    Shader::Info::num_ud_snapshots = 0;
    self.num_ring_stages = 0;
    if (self.constant_ring) {
        self.constant_ring->Stamp(packet.ring_end);
    }
}

void Rasterizer::PrintPipeStats() {
    static const bool stats = std::getenv("GOW3_FRAME_STATS") != nullptr;
    if (!stats || (draw_pipe->packets & 1023) != 0) {
        return;
    }
    static auto window = std::chrono::steady_clock::now();
    static u64 last_packets = 0, last_drains = 0, last_drain_cycles = 0, last_busy = 0;
    static u64 last_tsc = __rdtsc();
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - window).count();
    if (seconds < 5.0) {
        return;
    }
    const u64 tsc = __rdtsc();
    const double cycles = double(tsc - last_tsc);
    const u64 busy = draw_pipe->busy_cycles.load(std::memory_order_relaxed);
    std::printf("Draw pipe: %.0f draws/s pipelined, %.0f drains/s that waited, stage A waited "
                "%.1f%%, stage B busy %.1f%%\n",
                (draw_pipe->packets - last_packets) / seconds,
                (draw_pipe->drains - last_drains) / seconds,
                100.0 * (draw_pipe->drain_cycles - last_drain_cycles) / cycles,
                100.0 * (busy - last_busy) / cycles);
    std::printf("  drains by reason/s:");
    for (u32 r = 0; r < DrawPipe::NumReasons; ++r) {
        if (const u64 n = draw_pipe->drains_by_reason[r]) {
            static constexpr const char* names[] = {"ConstRam", "commands", "compute",
                                                    "submission end", "draw", "rasterizer"};
            const double share = 100.0 * draw_pipe->cycles_by_reason[r] / cycles;
            draw_pipe->cycles_by_reason[r] = 0;
            if (r < 256) {
                std::printf(" %s=%.0f (%.1f%%)",
                            magic_enum::enum_name(AmdGpu::PM4ItOpcode(r)).data(), n / seconds,
                            share);
            } else {
                std::printf(" %s=%.0f (%.1f%%)", names[r - 256], n / seconds, share);
            }
        }
    }
    std::printf("\n  stage A waits over 0.5%%:");
    for (const auto& [function, lines] : drain_sites) {
        for (const auto& [line, waited] : lines) {
            if (waited * 200 > cycles) {
                std::printf(" %s:%u %.1f%%", function, line, 100.0 * waited / cycles);
            }
        }
    }
    std::printf("\n");
    drain_sites.clear();
    draw_pipe->drains_by_reason = {};
    window = now;
    last_tsc = tsc;
    last_packets = draw_pipe->packets;
    last_drains = draw_pipe->drains;
    last_drain_cycles = draw_pipe->drain_cycles;
    last_busy = busy;
}

bool Rasterizer::FilterDrawPasses() const {
    // FilterDraw's checks without its side effects: false when it would skip the draw or run
    // a pass of its own (fast clear elimination, resolve, depth/stencil copy).
    const auto& regs = Regs();
    using Mode = AmdGpu::ColorControl::OperationMode;
    const auto mode = regs.color_control.mode;
    if (mode == Mode::EliminateFastClear || mode == Mode::FmaskDecompress ||
        mode == Mode::Resolve || regs.primitive_type == AmdGpu::PrimitiveType::None) {
        return false;
    }
    const bool depth_copy =
        regs.depth_render_override.force_z_dirty && regs.depth_render_override.force_z_valid &&
        regs.depth_buffer.DepthValid() && regs.depth_buffer.DepthWriteValid() &&
        regs.depth_buffer.DepthAddress() != regs.depth_buffer.DepthWriteAddress();
    const bool stencil_copy =
        regs.depth_render_override.force_stencil_dirty &&
        regs.depth_render_override.force_stencil_valid && regs.depth_buffer.StencilValid() &&
        regs.depth_buffer.StencilWriteValid() &&
        regs.depth_buffer.StencilAddress() != regs.depth_buffer.StencilWriteAddress();
    return !(mode == Mode::Disable && (depth_copy || stencil_copy));
}

void Rasterizer::Draw(bool is_indexed, u32 index_offset, const PreparedDraw* prepared) {
    RENDERER_TRACE;
    Gow3Stats::draws.fetch_add(1, std::memory_order_relaxed);

    // gow3: with the draw pipeline this thread only selects the pipeline and hands the draw
    // to the recording thread (DrawRecord there); draws FilterDraw handles itself run here.
    const bool pipelined = UseDrawPipe();
    if (pipelined && !FilterDrawPasses() &&
        !Gow3Toggle::Disabled(Gow3Toggle::PipelinedMemoryWrites)) {
        PostDraw(nullptr, nullptr, false, 0);
        return;
    }
    if (!pipelined || !FilterDrawPasses()) {
        DrainDrawPipe(DrawPipe::ReasonDraw);
        FrameCapture::Poll();
        scheduler.PopPendingOperations();
        if (!FilterDraw()) {
            return;
        }
    }
    const GraphicsPipeline* pipeline = pipeline_cache.GetGraphicsPipeline({}, prepared);
    const PreparedDraw* used_prepared = pipeline_cache.UsedPrepared();
    if (prepared) {
        draw_prep->Count(pipeline != nullptr &&
                         prepared->state.load(std::memory_order_acquire) == PreparedDraw::Ready &&
                         prepared->reg_checksum == liverpool->gfx_reg_checksum);
    }
    if (!pipeline) {
        // gow3 21 Oct 2026 (Uncharted 3 museum: a carpet never drawn, the scene's background red
        // through it): which draws find no pipeline, by their stage programs, per 5 s.
        static std::unordered_map<u64, u32> dropped;
        static auto dropped_report = std::chrono::steady_clock::now();
        const auto& regs = Regs();
        const auto program_hash = [&](u32 stage) -> u64 {
            const auto* pgm = regs.ProgramForStage(stage);
            return pgm && pgm->Address<u32*>() ? AmdGpu::GetParams(*pgm).hash : 0;
        };
        const u64 key = (program_hash(static_cast<u32>(Shader::HwStage::Fragment)) & 0xffffffffull) |
                        (u64(regs.stage_enable.raw) << 32) |
                        (u64(program_hash(static_cast<u32>(Shader::HwStage::Vertex)) & 0xffffff) << 40);
        ++dropped[key];
        if (const auto now = std::chrono::steady_clock::now();
            now - dropped_report >= std::chrono::seconds(5)) {
            dropped_report = now;
            std::string text;
            for (const auto& [k, n] : dropped) {
                text += fmt::format(" ps {:08x} vs {:06x}.. stages {:#x} x{}", u32(k), u32(k >> 40),
                                    u32((k >> 32) & 0xff), n);
            }
            std::printf("Draws without a pipeline (5 s):%s\n", text.c_str());
            std::fflush(stdout);
            dropped.clear();
        }
        return;
    }
    if (pipelined && FilterDrawPasses()) {
        PostDraw(pipeline, used_prepared, is_indexed, index_offset);
        return;
    }
    DrawRecord(pipeline, used_prepared, is_indexed, index_offset);
}


namespace {
/// Diagnostics: GOW3_SKIP_FILE=<file> lists shader hashes (hex, whitespace separated); draws
/// and dispatches using one are skipped. Re-read when the file changes, so shaders can be
/// switched off while the game runs.
/// gow3 21 Oct 2026 (U3 museum red carpet): values above 32 bits are index buffer addresses
/// (frame capture "mesh" lines); then only draws of those meshes are skipped, and only those using
/// a listed shader when shaders are listed too (one mesh in one pass).
bool SkippedShader(u64 first, u64 second = 0, u64 index_base = 0) {
    static const char* path = std::getenv("GOW3_SKIP_FILE");
    if (!path || !path[0]) {
        return false;
    }
    static std::mutex mutex;
    static std::vector<u64> hashes;
    static std::vector<u64> meshes;
    static std::filesystem::file_time_type stamp{};
    static std::chrono::steady_clock::time_point checked{};
    std::scoped_lock lock{mutex};
    const auto now = std::chrono::steady_clock::now();
    if (now - checked > std::chrono::milliseconds(500)) {
        checked = now;
        std::error_code ec;
        const auto time = std::filesystem::last_write_time(path, ec);
        if (ec) {
            hashes.clear();
            meshes.clear();
        } else if (time != stamp) {
            stamp = time;
            hashes.clear();
            meshes.clear();
            if (FILE* f = std::fopen(path, "r")) {
                unsigned long long value;
                while (std::fscanf(f, "%llx", &value) == 1) {
                    (value > 0xffffffffull ? meshes : hashes).push_back(value);
                }
                std::fclose(f);
            }
            std::printf("Skip list: %zu shaders, %zu meshes\n", hashes.size(), meshes.size());
        }
    }
    const bool shader_listed = std::ranges::any_of(
        hashes, [&](u64 h) { return h == first || (second && h == second); });
    if (meshes.empty()) {
        return shader_listed;
    }
    return index_base && std::ranges::find(meshes, index_base) != meshes.end() &&
           (hashes.empty() || shader_listed);
}
} // namespace

// gow3 19 Oct 2026, from Supermedo/uncharted_pc #261 (kj2223): GOW3_JITTER_PS=<pixel shader
// hash>,... (tools/probe_uncharted.py: f614041e): full-screen passes that rebuild positions from
// the scene depth through an interpolated texture coordinate get the same jitter as the scene
// geometry. Unjittered, they rebuilt each pixel from depth rendered half a pixel away: the sun
// shadow pass (f614041e, shared by the three games) moved against the geometry with every jitter
// step and its shadows flickered under TAA, FSR and DLAA.
static bool JitterFullScreenPass(u64 hash) {
    static const std::vector<u64> listed = [] {
        std::vector<u64> list;
        if (const char* env = std::getenv("GOW3_JITTER_PS")) {
            for (const char* p = env; *p;) {
                char* end;
                const u64 value = std::strtoull(p, &end, 16);
                if (end == p) {
                    break;
                }
                list.push_back(u64(u32(value)));
                p = *end == ',' ? end + 1 : end;
            }
        }
        return list;
    }();
    return std::find(listed.begin(), listed.end(), u64(u32(hash))) != listed.end();
}

void Rasterizer::DrawRecord(const GraphicsPipeline* pipeline, const PreparedDraw* used_prepared,
                            bool is_indexed, u32 index_offset) {
    const auto* fragment=pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
    if (SkippedShader(pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash,
                      fragment ? fragment->pgm_hash : 0,
                      is_indexed ? Regs().index_base_address.Address<VAddr>() : 0)) {
        return;
    }
    scheduler.Checkpoint("draw",pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash,
                         fragment ? fragment->pgm_hash : 0);
    // gow3 21 Oct 2026 diagnostics (U3 museum red carpet), GOW3_MESH_DUMP=<index buffer hex>:
    // for draws of that mesh, the vertex shader's user data and the 16 dwords behind each
    // pointer of its resource table (as floats), once per vertex shader every 10 s, to compare
    // the matrices the depth pass and the shading passes read.
    if (static const u64 dump_ib = [] {
            const char* env = std::getenv("GOW3_MESH_DUMP");
            return env ? std::strtoull(env, nullptr, 16) : 0ull;
        }();
        dump_ib && is_indexed && Regs().index_base_address.Address<VAddr>() == dump_ib) {
        static std::mutex dump_mutex;
        static std::map<u64, std::chrono::steady_clock::time_point> dumped;
        // GOW3_MESH_DUMP_ALL=1: every draw of the mesh for 2 s every 15 s (all instances of
        // one frame), with the vertex shader's buffer descriptors.
        static const bool dump_all = std::getenv("GOW3_MESH_DUMP_ALL") != nullptr;
        static auto window_start = std::chrono::steady_clock::time_point{};
        const auto& vs = pipeline->GetStage(Shader::SwStage::Vertex);
        const auto now = std::chrono::steady_clock::now();
        std::scoped_lock lock{dump_mutex};
        bool due = false;
        if (dump_all) {
            if (now - window_start > std::chrono::seconds(15)) {
                window_start = now;
            }
            due = now - window_start < std::chrono::seconds(2);
        } else if (auto [it, fresh] = dumped.try_emplace(vs.pgm_hash, now);
                   fresh || now - it->second > std::chrono::seconds(10)) {
            it->second = now;
            due = true;
        }
        if (due) {
            const auto ud = vs.UserData();
            std::string text = fmt::format("Mesh dump: ib {:#x} vs {:08x} ps {:08x} user data",
                                           dump_ib, u32(vs.pgm_hash),
                                           u32(fragment ? fragment->pgm_hash : 0));
            for (size_t i = 0; i < std::min<size_t>(ud.size(), 16); ++i) {
                text += fmt::format(" {:08x}", ud[i]);
            }
            for (u32 i = 0; i < vs.buffers.size(); ++i) {
                const auto sharp = vs.buffers[i].GetSharp(vs);
                text += fmt::format("\n  vs buffer {}: base {:#x} stride {} records {} fmt {}/{}",
                                    i, sharp.base_address, sharp.GetStride(), sharp.num_records,
                                    u32(sharp.GetDataFmt()), u32(sharp.GetNumberFmt()));
            }
            if (fragment) {
                const auto& ps = *fragment;
                for (u32 i = 0; i < ps.images.size(); ++i) {
                    const auto image = ps.images[i].GetSharp(ps);
                    text += fmt::format(
                        "\n  ps image {}: {:#x} {}x{} fmt {}/{} tiling {} type {} levels {}-{} dst {}{}{}{}",
                        i, image.Address(), u32(image.width) + 1, u32(image.height) + 1,
                        u32(image.GetDataFmt()), u32(image.GetNumberFmt()),
                        u32(image.tiling_index), u32(image.type), u32(image.base_level),
                        u32(image.last_level), u32(image.dst_sel_x), u32(image.dst_sel_y),
                        u32(image.dst_sel_z), u32(image.dst_sel_w));
                }
                for (u32 i = 0; i < ps.buffers.size(); ++i) {
                    const auto sharp = ps.buffers[i].GetSharp(ps);
                    text += fmt::format("\n  ps buffer {}: base {:#x} stride {} records {} fmt {}/{}",
                                        i, sharp.base_address, sharp.GetStride(),
                                        sharp.num_records, u32(sharp.GetDataFmt()),
                                        u32(sharp.GetNumberFmt()));
                }
            }
            if (ud.size() >= 2) {
                const VAddr table = (u64(ud[1]) << 32 | ud[0]) & 0xffffffffffffull;
                u32 slots[32]{};
                memory->CopySparseMemory(table, reinterpret_cast<u8*>(slots), sizeof(slots));
                for (u32 k = 0; k < 16; ++k) {
                    const VAddr p = (u64(slots[2 * k + 1]) << 32 | slots[2 * k]) & 0xffffffffffffull;
                    if (p < 0x100000 || p >= 0x2000000000ull) {
                        continue;
                    }
                    float v[16]{};
                    memory->CopySparseMemory(p, reinterpret_cast<u8*>(v), sizeof(v));
                    text += fmt::format("\n  T[{},{}] -> {:#x}:", 2 * k, 2 * k + 1, p);
                    for (const float f : v) {
                        text += fmt::format(" {:.9g}", f);
                    }
                }
            }
            std::printf("%s\n", text.c_str());
            std::fflush(stdout);
        }
    }
    if (DrawPipe::OnStageB()) {
        FrameCapture::Poll();
        scheduler.PopPendingOperations();
    }
    const auto& regs = Regs();
    const auto frame_start = [&] {
        scene_started = false;
        camera_motion->OnDisplayPass(cb_descs[0].first);
        if (upscaler->OnFrameStart()) {
            object_motion->InvalidateHistory();
            camera_motion->InvalidateHistory();
        }
        camera_motion->SetJitter(upscaler->Jitter());
        object_motion->OnFrameStart();
        NoteFrameStart();
        static const char* scene_debug = std::getenv("GOW3_SCENE_DEBUG");
        static auto scene_debug_poll = std::chrono::steady_clock::time_point{};
        scene_debug_frame =
            scene_debug && TriggerPollDue(scene_debug_poll) && std::remove(scene_debug) == 0;
        scene_targets->debug = scene_debug_frame;
    };
    // gow3: the pass copying a finished frame to a display buffer; the previous draw's
    // target is that frame. 19 Oct 2026, from Supermedo/uncharted_pc #261 (kj2223): Uncharted
    // draws twice into the same display buffer per frame (484cdbf5, then 749bbec9; measured 2
    // frame starts per shown frame): only the first starts a frame. The second reset the
    // upscaler's history every frame (shimmer on every edge under TAA, FSR and DLAA) and
    // skipped every other jitter phase. Toggle 1 << 59 brings the old behaviour back.
    // Below the output size Uncharted's frames start at the upscale instead
    // (TemporalUpscaler::OnComposite): its finished frame, the copy back and the HUD are
    // several draws into the display buffer.
    if (camera_motion->Enabled() && !upscaler->CompositeFrames() && regs.color_buffers[0] &&
        FrameCapture::IsDisplayBuffer(regs.color_buffers[0].Address()) &&
        (scene_started || regs.color_buffers[0].Address() != frame_display_address ||
         Gow3Toggle::Disabled(Gow3Toggle::OneFrameStart))) {
        frame_display_address = regs.color_buffers[0].Address();
        frame_start();
    }
    // gow3: without camera motion (Uncharted) the GPU profiler marks frames at the display pass
    // too. Its results are read with a wait four frames later, so everything recorded so far is
    // submitted first (a wait on unsubmitted timestamps hung the menu).
    else if (!camera_motion->Enabled() && GpuProfiler::Get() && regs.color_buffers[0] &&
             FrameCapture::IsDisplayBuffer(regs.color_buffers[0].Address())) {
        Flush();
        NoteFrameStart();
    }
    bind_prepared = used_prepared;
    motion_draw = pipeline->GetGraphicsKey().motion_vectors;
    motion_geometry = 0;

    PrepareRenderState(pipeline);
    if (upscaler->CompositeFrames() && fragment &&
        upscaler->OnComposite(fragment->pgm_hash, cb_descs[0].first)) {
        bind_prepared = nullptr; // as after a draw: later dispatches bind without its sharps
        frame_start();           // the guest's copy is replaced by the upscale
        return;
    }
    if (upscaler->Enabled() && std::popcount(pipeline->GetGraphicsKey().mrt_mask) == 1) {
        const auto& viewport = Regs().viewports[0];
        upscaler->OnDraw(pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash,
                         cb_descs[0].first, db_desc.first,
                         UiComposition::NativeViewport(viewport.xscale * 2, viewport.yscale * 2));
    }
    const PreparedDraw* draw_prepared = bind_prepared;
    // gow3: vertex and index buffers are resolved while the helper binds textures (their
    // commands are recorded after BeginRendering, as before).
    draw_inputs = {pipeline, draw_prepared, index_offset, is_indexed, true, false};
    const bool bound = BindResources(pipeline);
    bind_prepared = nullptr; // indirect draws and dispatches bind without prepared sharps
    const bool inputs_resolved = draw_inputs.resolved;
    draw_inputs.pending = false;
    if (!bound) {
        return;
    }
    if (instance.HasGpuCheckpoints()) {
        // Diagnostics: name this draw's resources in the device loss report.
        for (const VideoCore::ImageId image_id : bound_images) {
            const auto& image = texture_cache.GetImage(image_id);
            const auto& info = image.info;
            scheduler.Checkpoint("image", info.guest_address,
                                 u64(info.size.width) | u64(info.size.height) << 16 |
                                     u64(info.resources.levels) << 32 |
                                     u64(info.resources.layers) << 40 |
                                     u64(u32(info.pixel_format) & 0xffff) << 48);
        }
        for (const auto& bound : bound_buffers) {
            scheduler.Checkpoint("buffer",
                                 bound.buffer->CpuAddr() + bound.offset,
                                 u64(bound.size) | u64(bound.is_written) << 63);
        }
    }
    const auto state = BeginRendering(pipeline);
    if (float_color0 && !pipeline->FloatTargetHandle()) [[unlikely]] {
        SkipFloatTargetDraw(*pipeline);
        return;
    }

    if (!inputs_resolved) {
        ResolveVertexBuffers(pipeline, draw_prepared);
        if (is_indexed) {
            ResolveIndexBuffer(index_offset);
        }
    }
    // Allocate before emitting vertex/index state: a stream-buffer wrap may submit recording.
    vk::DescriptorBufferInfo hdr_settings{};
    if (upscaler->HdrChain() && pipeline->HdrShadowHandle() && !float_color0) {
        const auto& settings = Gow3Settings::Get();
        const std::array<float, 2> nits{float(settings.hdr_peak_nits.load()),
                                      float(settings.hdr_game_nits.load())};
        auto& stream = buffer_cache.GetStreamBuffer();
        const auto [data, offset] = stream.Map(sizeof(nits), instance.StorageMinAlignment());
        std::memcpy(data, nits.data(), sizeof(nits));
        stream.Commit();
        hdr_settings = {stream.Handle(), offset, sizeof(nits)};
    }
    EmitVertexBuffers();
    if (is_indexed) {
        EmitIndexBuffer();
    }

    if (needs_barrier) {
        runtime.FlushBarriers();
    }

    // gow3: screen-space (clip disabled) draws into the upscaler's output-size images.
    push_data.xscale *= target_scale[0];
    push_data.xoffset *= target_scale[0];
    push_data.yscale *= target_scale[1];
    push_data.yoffset *= target_scale[1];
    // Uncharted: the animated draws of its G-buffer passes are the skinned ones, whose vertex
    // buffers the GPU wrote this session (the skinning compute; the level's vertices stay as
    // loaded). Its shaders read vertices from buffers (no vertex inputs): the draw's identity
    // across frames comes from those buffers' sizes and strides, not their addresses (skinned
    // vertices can move between frames). (The stencil's character bit differs between the
    // games: Uncharted 2 sets it on the whole level.)
    bool character_draw = false;
    if (motion_draw && Shader::MotionVectors::stencil_gate) {
        const auto& vs = pipeline->GetStage(Shader::SwStage::Vertex);
        u64 hash = 0x6a09e667f3bcc909ULL;
        for (const auto& resource : vs.buffers) {
            if (resource.IsSpecial()) continue;
            const auto buffer = resource.GetSharp(vs);
            if (!buffer.Valid()) continue;
            hash = XXH3_64bits_withSeed(&buffer.num_records, sizeof(buffer.num_records),
                                        hash ^ buffer.GetStride());
            // Constant buffers (stride 16) are not vertices.
            character_draw |= buffer.GetStride() != 16 &&
                              buffer_cache.IsRangeGpuWrittenShared(buffer.base_address,
                                                                    buffer.GetSize());
        }
        if (character_draw && !motion_geometry) {
            motion_geometry = hash;
        }
    }
    if (motion_draw && motion_geometry &&
        (!Shader::MotionVectors::stencil_gate || character_draw)) {
        const auto& vs = pipeline->GetStage(Shader::SwStage::Vertex);
        const auto [base_vertex, first_instance] =
            GetDrawOffsets(regs, vs, pipeline->GetFetchShader());
        const u32 index_size =
            regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16 ? 2u : 4u;
        const VAddr index_address = is_indexed
            ? regs.index_base_address.Address<VAddr>() + u64(index_offset) * index_size : 0;
        // Without a character-size skeleton the draw is gated on its bone palette changing
        // since last frame (model constants follow the camera, the palette does not). The
        // check comes first: most gated draws are static world pieces with long index lists.
        static const bool all_motion = [] {
            const char* value = std::getenv("GOW3_OBJECT_MOTION_ALL");
            return value && value[0] == '1';
        }();
        bool gated = !all_motion && !character_draw;
        u64 palette = 0;
        for (const auto& resource : vs.buffers) {
            if (resource.IsSpecial()) continue;
            const auto buffer = resource.GetSharp(vs);
            const u32 size = buffer.GetSize();
            if (!buffer.Valid() || buffer.GetStride() != 16) continue;
            const auto role = Motion::ClassifyBuffer(size);
            if (role == Motion::BufferRole::Skeleton) {
                gated = false;
                break;
            }
            const VAddr address = buffer.base_address;
            if (role == Motion::BufferRole::SmallSkeleton &&
                memory->IsValidGpuMapping(address, 0) &&
                memory->ClampRangeSize(address, size) == size) {
                palette = XXH3_64bits_withSeed(reinterpret_cast<const void*>(address), size,
                                               palette);
            }
        }
        push_data.motion_param = 0;
        if (!gated || object_motion->Moving({.shader = vs.pgm_hash,
                                             .geometry = motion_geometry,
                                             .indices = index_address,
                                             .index_count = regs.num_indices,
                                             .instances = regs.num_instances.NumInstances(),
                                             .first_instance = first_instance},
                                            palette)) {
            Motion::VertexRange range{base_vertex, regs.num_indices};
            u64 topology = 0;
            if (is_indexed) {
                range = {};
                const u64 bytes = u64(regs.num_indices) * index_size;
                if (index_address && memory->IsValidGpuMapping(index_address, 0) &&
                    memory->ClampRangeSize(index_address, bytes) == bytes) {
                    const bool restart = regs.enable_primitive_restart != 0;
                    const u32 count = regs.num_indices;
                    const auto scanned = object_motion->IndexRange(
                        {index_address, count, index_size, s32(base_vertex), restart}, [&] {
                            const auto* data = reinterpret_cast<const void*>(index_address);
                            const auto scan_range =
                                index_size == 2
                                    ? Motion::IndexedRange(
                                          std::span(static_cast<const u16*>(data), count),
                                          s32(base_vertex), restart)
                                    : Motion::IndexedRange(
                                          std::span(static_cast<const u32*>(data), count),
                                          s32(base_vertex), restart);
                            return Motion::IndexRangeCache::Result{scan_range,
                                                                   XXH3_64bits(data, bytes)};
                        });
                    range = scanned.range;
                    topology = scanned.topology;
                }
            }
            push_data.motion_param = object_motion->PrepareDraw({
                .shader = vs.pgm_hash,
                .geometry = motion_geometry,
                .indices = index_address,
                .topology = topology,
                .index_count = regs.num_indices,
                .instances = regs.num_instances.NumInstances(),
                .first_instance = first_instance,
                .vertices = range,
            });
        }
    }
    pipeline->BindResources(set_writes, push_data);
    // gow3: jitter geometry drawn with the scene depth, not full-screen passes (a shifted
    // full-screen quad leaves an edge column unwritten).
    draw_jitter = {};
    if (upscaler->Enabled() && db_desc.first && db_desc.first == camera_motion->Depth() &&
        (regs.num_indices > 6 || regs.num_instances.NumInstances() > 1)) {
        draw_jitter = upscaler->Jitter();
    } else if (const auto* ps = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
               ps && upscaler->Enabled() && JitterFullScreenPass(ps->pgm_hash) &&
               !Gow3Toggle::Disabled(Gow3Toggle::FullScreenPassJitter)) {
        draw_jitter = upscaler->Jitter();
        static bool noted = false;
        if (!noted) {
            noted = true;
            std::printf("Upscaler: full-screen pass %08x gets the scene jitter\n",
                        u32(ps->pgm_hash));
        }
    }
    UpdateDynamicState(pipeline, is_indexed);
    MarkPass(pipeline, state);
    scheduler.BeginRendering(state);

    const auto& vs_info = pipeline->GetStage(Shader::SwStage::Vertex);
    const auto& fetch_shader = pipeline->GetFetchShader();
    const auto [vertex_offset, instance_offset] = GetDrawOffsets(regs, vs_info, fetch_shader);

    const vk::Pipeline handle =
        float_color0 ? pipeline->FloatTargetHandle() : pipeline->Handle();
    const vk::Pipeline square = float_color0 ? vk::Pipeline{} : SquarePass(*pipeline);
    const u32 num_indices = regs.num_indices;
    const u32 num_instances = regs.num_instances.NumInstances();
    const u32 first_vertex = vertex_offset;
    const u32 first_instance = instance_offset;
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        const auto draw = [&] {
            if (is_indexed) {
                cmdbuf.drawIndexed(num_indices, num_instances, 0, s32(first_vertex),
                                   first_instance);
            } else {
                cmdbuf.draw(num_indices, num_instances, first_vertex, first_instance);
            }
        };
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, handle);
        draw();
        if (square) [[unlikely]] {
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, square);
            draw();
        }
    });
    if (upscaler->HdrChain()) [[unlikely]] {
        RecordHdrShadow(*pipeline, state, is_indexed, num_indices, num_instances, first_vertex,
                        first_instance, hdr_settings);
    }
    // gow3: the camera of the game's velocity pass (GOW3_VELOCITY_PS, Uncharted): its constant
    // buffer, as this draw reads it, for the upscaler's motion vectors.
    if (camera_motion->VelocityMode()) {
        const auto* ps = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
        if (ps && camera_motion->IsVelocityShader(ps->pgm_hash) && !ps->buffers.empty() &&
            !ps->buffers[0].IsSpecial()) {
            const PreparedStage* prepared_stage = nullptr;
            for (u32 i = 0; draw_prepared && i < draw_prepared->num_stages; ++i) {
                const auto& candidate = draw_prepared->stages[i];
                if (&candidate.program->info == ps && candidate.num_buffers == ps->buffers.size()) {
                    prepared_stage = &candidate;
                }
            }
            const auto sharp = prepared_stage ? prepared_stage->buffer_sharps[0]
                                              : ps->buffers[0].GetSharp(*ps);
            const u32 bytes = sharp.GetSize();
            if (sharp.base_address && bytes >= 64 * sizeof(float) &&
                memory->IsValidGpuMapping(sharp.base_address, bytes)) {
                camera_motion->OnVelocityConstants(
                    reinterpret_cast<const float*>(sharp.base_address), bytes / sizeof(float));
            }
        }
    }
    if (FrameCapture::Active()) {
        const auto* ps = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
        FrameCapture::Draw(vs_info.pgm_hash, ps ? ps->pgm_hash : 0, num_indices, num_instances);
        const auto& dc = regs.depth_control;
        const auto& rc = regs.depth_render_control;
        const auto& sr = regs.stencil_ref_front;
        const auto effective = GetEffectiveDepthStencilState(regs);
        char depth_note[320];
        std::snprintf(depth_note, sizeof(depth_note),
                      "\n  state vs %08x ps %08x: depth %u write %u func %u clear %u stencil %u func %u ref %u opval %u mask %u write %u clear %u effective %u%u%u%u",
                      u32(vs_info.pgm_hash), u32(ps ? ps->pgm_hash : 0), u32(dc.depth_enable),
                      u32(dc.depth_write_enable), u32(dc.depth_func), u32(rc.depth_clear_enable),
                      u32(dc.stencil_enable), u32(dc.stencil_ref_func), u32(sr.stencil_test_val),
                      u32(sr.stencil_op_val), u32(sr.stencil_mask), u32(sr.stencil_write_mask),
                      u32(rc.stencil_clear_enable), u32(effective.needs_attachment),
                      u32(effective.depth_test_enable), u32(effective.depth_write_enable),
                      u32(effective.stencil_test_enable));
        FrameCapture::Note(depth_note);
        {
            // gow3 21 Oct 2026 (U3 museum red carpet): the mesh of each draw (index buffer, first
            // index, count, base vertex, instances), to pair a depth-pass draw with the material
            // pass draw of the same mesh and with GOW3_SRT_CHECK lines.
            char mesh_note[160];
            std::snprintf(mesh_note, sizeof(mesh_note),
                          "\n  mesh vs %08x ps %08x: ib %#llx first %u n %u%s base vertex %u inst %u",
                          u32(vs_info.pgm_hash), u32(ps ? ps->pgm_hash : 0),
                          static_cast<unsigned long long>(
                              is_indexed ? regs.index_base_address.Address<VAddr>() : 0),
                          is_indexed ? index_offset : 0, num_indices,
                          is_indexed ? (regs.index_buffer_type.index_type ==
                                                AmdGpu::IndexType::Index16
                                            ? " u16"
                                            : " u32")
                                     : " (not indexed)",
                          first_vertex, num_instances);
            FrameCapture::Note(mesh_note);
        }
        {
            // gow3: depth transform evidence (D16 shadow map cleared by a z=-1 RectList quad).
            const auto& cc = regs.clipper_control;
            const auto& vc = regs.viewport_control;
            const auto& vp = regs.viewports[0];
            const auto& vd = regs.viewport_depths[0];
            char clip_note[320];
            std::snprintf(clip_note, sizeof(clip_note),
                          "\n  clip vs %08x: prim %u clip_disable %u/%u space %u zclip near/far off %u%u "
                          "vte z scale/off %u%u xy/z/w transformed %u%u%u zscale %g zoffset %g zmin %g zmax %g "
                          "vp_clamp_off %u",
                          u32(vs_info.pgm_hash), u32(regs.primitive_type), u32(cc.clip_disable),
                          u32(regs.IsClipDisabled()), u32(cc.clip_space), u32(cc.zclip_near_disable),
                          u32(cc.zclip_far_disable), u32(vc.zscale_enable), u32(vc.zoffset_enable),
                          u32(vc.xy_transformed), u32(vc.z_transformed), u32(vc.w_transformed), vp.zscale,
                          vp.zoffset, vd.zmin, vd.zmax,
                          u32(regs.depth_render_override.disable_viewport_clamp));
            FrameCapture::Note(clip_note);
            const auto& vs0 = regs.viewport_scissors[0];
            char rect_note[320];
            std::snprintf(rect_note, sizeof(rect_note),
                          "\n  rect vs %08x: vp x %g+%g y %g+%g; scissors screen %d,%d-%d,%d window %d,%d-%d,%d "
                          "generic %d,%d-%d,%d vp0 %d,%d-%d,%d (vport_scissor %u) depth slices %u-%u",
                          u32(vs_info.pgm_hash), vp.xoffset, vp.xscale, vp.yoffset, vp.yscale,
                          int(regs.screen_scissor.top_left_x), int(regs.screen_scissor.top_left_y),
                          int(regs.screen_scissor.bottom_right_x), int(regs.screen_scissor.bottom_right_y),
                          int(regs.window_scissor.top_left_x), int(regs.window_scissor.top_left_y),
                          int(regs.window_scissor.bottom_right_x), int(regs.window_scissor.bottom_right_y),
                          int(regs.generic_scissor.top_left_x), int(regs.generic_scissor.top_left_y),
                          int(regs.generic_scissor.bottom_right_x), int(regs.generic_scissor.bottom_right_y),
                          int(vs0.top_left_x), int(vs0.top_left_y), int(vs0.bottom_right_x),
                          int(vs0.bottom_right_y), u32(regs.mode_control.vport_scissor_enable),
                          u32(regs.depth_view.slice_start), u32(regs.depth_view.slice_max));
            FrameCapture::Note(rect_note);
        }
        if (const auto& bc = regs.blend_control[0]; bc.enable && regs.color_buffers[0]) {
            char note[128];
            std::snprintf(note, sizeof(note), "\n  blend ps %08x idx %u: src %u dst %u func %u z %u%u",
                          u32(ps ? ps->pgm_hash : 0), num_indices, u32(bc.color_src_factor),
                          u32(bc.color_dst_factor), u32(bc.color_func),
                          u32(regs.depth_control.depth_enable),
                          u32(regs.depth_control.depth_write_enable));
            FrameCapture::Note(note);
        }
    }
    DebugState.IncDrawCall();

    ResetBindings(false);
    scheduler.KickRecording();
}

void Rasterizer::DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 stride,
                              u32 max_count, VAddr count_address, u16 vertex_sgpr_offset,
                              u16 instance_sgpr_offset) {
    RENDERER_TRACE;
    // gow3: like direct draws, handed to the recording thread after the pipeline selection
    // (else the GPU thread waited here for a whole frame of queued draws).
    const bool pipelined = UseDrawPipe() && FilterDrawPasses() &&
                           !Gow3Toggle::Disabled(Gow3Toggle::PipelinedIndirectDraws);
    if (!pipelined) {
        DrainDrawPipe();
        scheduler.PopPendingOperations();
        if (!FilterDraw()) {
            return;
        }
    }

    const DrawIndirectParams params = {
        .vertex_sgpr_offset = vertex_sgpr_offset,
        .instance_sgpr_offset = instance_sgpr_offset,
    };
    const GraphicsPipeline* pipeline = pipeline_cache.GetGraphicsPipeline(params);
    if (!pipeline) {
        return;
    }
    const IndirectDraw indirect = {arg_address + offset, count_address, stride, max_count};
    if (pipelined) {
        PostDraw(pipeline, nullptr, is_indexed, 0, nullptr, &indirect);
        return;
    }
    DrawIndirectRecord(pipeline, is_indexed, indirect);
}

void Rasterizer::DrawIndirectRecord(const GraphicsPipeline* pipeline, bool is_indexed,
                                    const IndirectDraw& indirect) {
    if (DrawPipe::OnStageB()) {
        FrameCapture::Poll();
        scheduler.PopPendingOperations();
    }
    const VAddr count_address = indirect.count;
    const u32 stride = indirect.stride;
    const u32 max_count = indirect.max_count;

    // Indirect arguments may be GPU-written: leave these draws on camera fallback.
    motion_draw = false;
    motion_geometry = 0;
    PrepareRenderState(pipeline);
    if (upscaler->Enabled() && std::popcount(pipeline->GetGraphicsKey().mrt_mask) == 1) {
        const auto& viewport = Regs().viewports[0];
        upscaler->OnDraw(pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash,
                         cb_descs[0].first, db_desc.first,
                         UiComposition::NativeViewport(viewport.xscale * 2, viewport.yscale * 2));
    }
    if (!BindResources(pipeline)) {
        return;
    }
    const auto state = BeginRendering(pipeline);
    if (float_color0 && !pipeline->FloatTargetHandle()) [[unlikely]] {
        SkipFloatTargetDraw(*pipeline);
        return;
    }

    BindVertexBuffers(pipeline);
    if (is_indexed) {
        BindIndexBuffer();
    }

    const auto [buffer, base] =
        buffer_cache.ObtainBuffer(indirect.args, stride * max_count, false);
    needs_barrier |= runtime.IsBufferAccessed(buffer, base, stride * max_count);

    const VideoCore::Buffer* count_buffer;
    u64 count_offset;
    if (count_address != 0) {
        std::tie(count_buffer, count_offset) = buffer_cache.ObtainBuffer(count_address, 4, false);
        needs_barrier |= runtime.IsBufferAccessed(count_buffer, count_offset, 4);
    }

    if (needs_barrier) {
        runtime.FlushBarriers();
    }

    // gow3: screen-space (clip disabled) draws into the upscaler's output-size images.
    push_data.xscale *= target_scale[0];
    push_data.xoffset *= target_scale[0];
    push_data.yscale *= target_scale[1];
    push_data.yoffset *= target_scale[1];
    pipeline->BindResources(set_writes, push_data);
    draw_jitter = {};
    if (upscaler->Enabled() && db_desc.first && db_desc.first == camera_motion->Depth()) {
        draw_jitter = upscaler->Jitter();
    }
    UpdateDynamicState(pipeline, is_indexed);
    MarkPass(pipeline, state);
    if (FrameCapture::Active()) {
        // Indirect draws: arguments as the CPU sees them (GPU-written ones may be stale here).
        const auto* vs = pipeline->GetStages()[u32(Shader::SwStage::Vertex)];
        const auto* ps = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
        const u32* args = reinterpret_cast<const u32*>(indirect.args);
        const u32 count = count_address ? *reinterpret_cast<const u32*>(count_address) : 1;
        FrameCapture::Draw(vs ? vs->pgm_hash : 0, ps ? ps->pgm_hash : 0, args[0],
                           std::max(args[1], 1u));
        char note[200];
        std::snprintf(note, sizeof(note),
                      "\n  indirect vs %08x ps %08x: args %llx stride %u max %u count@%llx=%u "
                      "first %u %u %u %u %u",
                      u32(vs ? vs->pgm_hash : 0), u32(ps ? ps->pgm_hash : 0),
                      (unsigned long long)indirect.args, stride, max_count,
                      (unsigned long long)count_address, count, args[0], args[1], args[2], args[3],
                      args[4]);
        FrameCapture::Note(note);
    }
    scheduler.BeginRendering(state);

    ASSERT(stride == (is_indexed ? sizeof(VkDrawIndexedIndirectCommand)
                                 : sizeof(VkDrawIndirectCommand)));
    const vk::Pipeline handle =
        float_color0 ? pipeline->FloatTargetHandle() : pipeline->Handle();
    const vk::Pipeline square = float_color0 ? vk::Pipeline{} : SquarePass(*pipeline);
    const vk::Buffer args = buffer->Handle();
    const u64 args_offset = base;
    const vk::Buffer counts = count_address != 0 ? count_buffer->Handle() : vk::Buffer{};
    const u64 counts_offset = count_address != 0 ? count_offset : 0;
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        for (const vk::Pipeline bound : {handle, square}) {
            if (!bound) {
                break;
            }
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, bound);
            if (is_indexed) {
                if (counts) {
                    cmdbuf.drawIndexedIndirectCount(args, args_offset, counts, counts_offset, max_count,
                                                    stride);
                } else {
                    cmdbuf.drawIndexedIndirect(args, args_offset, max_count, stride);
                }
            } else if (counts) {
                cmdbuf.drawIndirectCount(args, args_offset, counts, counts_offset, max_count, stride);
            } else {
                cmdbuf.drawIndirect(args, args_offset, max_count, stride);
            }
        }
    });
    DebugState.IncDrawCall();

    ResetBindings(false);
    scheduler.KickRecording();
}

// gow3 diagnostics (GOW3_FULL_BARRIERS=1): a full memory and execution barrier before every
// compute dispatch (all earlier GPU work done and visible). Uncharted 3's cloth flicker shows only
// above 30 FPS: a missing read-before-write dependency between a draw and the next frame's cloth
// dispatch would only bite when the two are recorded back to back (16 Oct).
static u64 g_cloth_dispatches = 0; ///< GOW3_CLOTH_PEEK dispatch numbers

// gow3 diagnostics: who reads the cloth solvers' outputs (ClothTrace, further down).
// GOW3_CLOTH_TRACE=1 (no GPU copies, unlike GOW3_CLOTH_PEEK=1, which turns it on as well).
static bool ClothTraceEnabled() {
    static const bool enabled = [] {
        const char* trace = std::getenv("GOW3_CLOTH_TRACE");
        const char* peek = std::getenv("GOW3_CLOTH_PEEK");
        return (trace && trace[0] == '1') || (peek && peek[0] == '1');
    }();
    return enabled;
}

// gow3 diagnostics (GOW3_CLOTH_TRACE): the barrier epoch after a solver dispatch put its writes
// in the barrier tracker. A read of its newest output in the same epoch must see a hazard; one
// that does not would run without a barrier after the solver (a missed hazard).
static u32 g_cloth_epoch = 0;
static u64 g_cloth_epoch_shader = 0;
static bool FullBarriersEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_FULL_BARRIERS");
        return env && env[0] == '1';
    }();
    return enabled;
}
static void RecordFullBarrier(vk::CommandBuffer cmdbuf) {
    const vk::MemoryBarrier2 barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite | vk::AccessFlagBits2::eMemoryRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryWrite | vk::AccessFlagBits2::eMemoryRead,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
}

void Rasterizer::DispatchDirect() {
    RENDERER_TRACE;
    Gow3Stats::dispatches.fetch_add(1, std::memory_order_relaxed);
    // gow3: like draws, handed to the draw recording thread after the pipeline selection.
    const bool pipelined = UseDrawPipe() && !Gow3Toggle::Disabled(Gow3Toggle::PipelinedDispatch);
    if (!pipelined) {
        DrainDrawPipe();
    }
    const ComputePipeline* pipeline = pipeline_cache.GetComputePipeline();
    if (!pipeline) {
        return;
    }
    if (pipelined) {
        PostDraw(pipeline, nullptr, false, 0, &CsRegs());
        return;
    }
    DispatchRecord(pipeline);
}

void Rasterizer::DispatchRecord(const ComputePipeline* pipeline) {
    if (BarrierStatsEnabled()) {
        g_barrier_stats_shader = pipeline->GetStage(Shader::SwStage::Compute).pgm_hash;
    }
    if (SkippedShader(pipeline->GetStage(Shader::SwStage::Compute).pgm_hash)) {
        return;
    }
    FrameCapture::Poll();
    gbuffer_draw = false;

    scheduler.PopPendingOperations();

    const auto& cs_program = CsRegs();

    const auto& cs = pipeline->GetStage(Shader::SwStage::Compute);
    // After the resource binding: its transfers (image uploads, downloads) mark themselves.
    const auto mark = [&] {
        if (auto* profiler = GpuProfiler::Get()) {
            profiler->Mark(cs.pgm_hash ^ 0xD15Aull, [&] {
                return fmt::format("dispatch cs {:016x} ({}x{}x{} groups)", cs.pgm_hash,
                                   cs_program.dim_x, cs_program.dim_y, cs_program.dim_z);
            });
        }
    };
    if (upscaler->Enabled()) {
        upscaler->OnDispatch(cs.pgm_hash);
    }
    if (ExecuteShaderHLE(cs, Regs(), cs_program, *this)) {
        return;
    }
    // gow3 diagnostics (GOW3_DUMP_CS_CONST=<hex shader hash>): the first 32 dwords (as floats) of
    // every buffer the dispatch reads that is up to 4 KiB, with a timestamp, 2000 lines at most.
    static const u64 dump_cs = [] {
        const char* env = std::getenv("GOW3_DUMP_CS_CONST");
        return env ? std::strtoull(env, nullptr, 16) : 0ULL;
    }();
    if (dump_cs && cs.pgm_hash == dump_cs) {
        static std::atomic<int> lines{0};
        static const auto t0 = std::chrono::steady_clock::now();
        for (u32 i = 0; i < cs.buffers.size() && lines.load() < 2000; ++i) {
            const auto& desc = cs.buffers[i];
            if (desc.IsSpecial() || desc.is_written) {
                continue;
            }
            const auto sharp = desc.GetSharp(cs);
            if (!sharp.base_address || sharp.GetSize() == 0 || sharp.GetSize() > 4096 ||
                !IsMapped(sharp.base_address, std::min<u64>(sharp.GetSize(), 128))) {
                continue;
            }
            float values[32]{};
            const u64 bytes = std::min<u64>(sharp.GetSize(), sizeof(values));
            memory->CopySparseMemory(sharp.base_address, reinterpret_cast<u8*>(values), bytes);
            std::string text;
            for (u64 k = 0; k < bytes / 4; ++k) {
                text += fmt::format(" {:.5g}", values[k]);
            }
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            std::printf("CS const %llx t=%.1f buf %u @%#llx size %u:%s\n",
                        static_cast<unsigned long long>(cs.pgm_hash), ms, i,
                        static_cast<unsigned long long>(sharp.base_address), unsigned(sharp.GetSize()),
                        text.c_str());
            ++lines;
        }
        std::fflush(stdout);
    }

    // gow3 diagnostics (GOW3_TRACKER_CAP=<n>): a barrier before a dispatch once the barrier
    // tracker holds n ranges (it is emptied by barriers only). Off: 1024 cost Uncharted 3's
    // crowded street at 4K 48 -> 39 FPS (draws keep it above the cap, a barrier per dispatch).
    {
        static const size_t cap = [] {
            const char* env = std::getenv("GOW3_TRACKER_CAP");
            return env ? size_t(std::strtoull(env, nullptr, 10)) : size_t(0);
        }();
        if (cap && runtime.BarrierTrackerSize() > cap) {
            runtime.FlushBarriers();
        }
    }
    if (!BindResources(pipeline)) {
        return;
    }

    if (needs_barrier) {
        runtime.FlushBarriers();
    }

    scheduler.EndRendering();
    mark();
    // gow3 20 Oct 2026 (GOW3_SKIP_COMPUTE_REBIND=0: always bind): the compute pipeline is not
    // bound again when the previous dispatch bound the same one and nothing but this dispatch's
    // push constants and descriptors was recorded since, in the same command buffer (the
    // scheduler's record serial: every Record() and CommandBuffer() call, and every new command
    // buffer, advance it; any other compute bind goes through one of them). Only while Vulkan
    // commands are recorded on this thread (no recording thread, no draw pipeline). Uncharted 3
    // dispatches ~1,275 times a frame, mostly the same few shaders back to back; on the
    // laptop (Ryzen 7840HS, 47 FPS, the GPU command thread CPU-bound) the driver's
    // vkCmdBindPipeline was 2.4% of that thread and vkCmdDispatch 4.3% (together 0.6% on the
    // Radeon 780M).
    static const bool skip_rebind = [] {
        const char* env = std::getenv("GOW3_SKIP_COMPUTE_REBIND");
        return !(env && env[0] == '0');
    }();
    const u64 serial_before_bind = scheduler.RecordSerial();
    pipeline->BindResources(set_writes, push_data);
    const u64 serial_after_bind = scheduler.RecordSerial();

    const vk::Pipeline handle = pipeline->Handle();
    const u32 dim_x = cs_program.dim_x, dim_y = cs_program.dim_y, dim_z = cs_program.dim_z;
    scheduler.Checkpoint("dispatch",cs.pgm_hash);
    if (FrameCapture::Active()) {
        FrameCapture::Dispatch(cs.pgm_hash, dim_x, dim_y, dim_z);
    }
    const u64 cloth_dispatch = cloth_peeks.empty() ? 0 : ++g_cloth_dispatches;
    for (const auto& peek : cloth_peeks) {
        buffer_cache.PeekGpuBuffer(peek.buffer, peek.buffer_offset, peek.address, peek.size, peek.shader,
                                   peek.slot, cloth_dispatch, 0);
    }
    const bool full_barrier = FullBarriersEnabled();
    const bool bound = skip_rebind && handle == bound_compute_pipeline &&
                       serial_before_bind == bound_compute_serial &&
                       scheduler.RecordSerial() == serial_after_bind &&
                       !scheduler.IsRecordingThreaded() && !UseDrawPipe();
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        if (full_barrier) {
            RecordFullBarrier(cmdbuf);
        }
        if (!bound) {
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, handle);
        }
        cmdbuf.dispatch(dim_x, dim_y, dim_z);
        if (full_barrier) {
            RecordFullBarrier(cmdbuf);
        }
    });
    bound_compute_pipeline = handle;
    bound_compute_serial = scheduler.RecordSerial();
    for (const auto& peek : cloth_peeks) {
        if (peek.written) {
            buffer_cache.PeekGpuBuffer(peek.buffer, peek.buffer_offset, peek.address, peek.size, peek.shader,
                                       peek.slot, cloth_dispatch, 1);
        }
    }
    cloth_peeks.clear();
    DebugState.IncDispatch();

    ResetBindings(true);
    if (ClothTraceEnabled() && (cs.pgm_hash == 0x1c957b01 || cs.pgm_hash == 0x809864d3)) {
        g_cloth_epoch = runtime.AccessEpoch();
        g_cloth_epoch_shader = cs.pgm_hash;
    }
    scheduler.KickRecording();
}

void Rasterizer::DispatchIndirect(VAddr address, u32 offset, u32 size) {
    RENDERER_TRACE;
    // gow3: handed to the recording thread like direct dispatches.
    const bool pipelined = UseDrawPipe() && !Gow3Toggle::Disabled(Gow3Toggle::PipelinedIndirectDraws);
    if (!pipelined) {
        DrainDrawPipe();
    }
    const ComputePipeline* pipeline = pipeline_cache.GetComputePipeline();
    if (!pipeline) {
        return;
    }
    if (pipelined) {
        const IndirectDraw indirect = {address + offset, 0, size, 1};
        PostDraw(pipeline, nullptr, false, 0, &CsRegs(), &indirect);
        return;
    }
    DispatchIndirectRecord(pipeline, address + offset, size);
}

void Rasterizer::DispatchIndirectRecord(const ComputePipeline* pipeline, VAddr address,
                                        u32 size) {
    if (SkippedShader(pipeline->GetStage(Shader::SwStage::Compute).pgm_hash)) {
        return;
    }
    scheduler.PopPendingOperations();
    const u32 offset = 0;

    if (!BindResources(pipeline)) {
        return;
    }

    const auto [buffer, base] = buffer_cache.ObtainBuffer(address + offset, size, false);
    needs_barrier |= runtime.IsBufferAccessed(buffer, base, size);

    if (needs_barrier) {
        runtime.FlushBarriers();
    }

    scheduler.EndRendering();
    pipeline->BindResources(set_writes, push_data);
    if (FrameCapture::Active()) {
        const u32* args = reinterpret_cast<const u32*>(address + offset);
        FrameCapture::Dispatch(pipeline->GetStage(Shader::SwStage::Compute).pgm_hash, args[0],
                               args[1], args[2]);
        FrameCapture::Note("\n  indirect dispatch (CPU view of args)");
    }

    const vk::Pipeline handle = pipeline->Handle();
    const vk::Buffer args = buffer->Handle();
    const u64 args_offset = base;
    const u64 cloth_dispatch = cloth_peeks.empty() ? 0 : ++g_cloth_dispatches;
    for (const auto& peek : cloth_peeks) {
        buffer_cache.PeekGpuBuffer(peek.buffer, peek.buffer_offset, peek.address, peek.size, peek.shader,
                                   peek.slot, cloth_dispatch, 0);
    }
    const bool full_barrier = FullBarriersEnabled();
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        if (full_barrier) {
            RecordFullBarrier(cmdbuf);
        }
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, handle);
        cmdbuf.dispatchIndirect(args, args_offset);
        if (full_barrier) {
            RecordFullBarrier(cmdbuf);
        }
    });
    for (const auto& peek : cloth_peeks) {
        if (peek.written) {
            buffer_cache.PeekGpuBuffer(peek.buffer, peek.buffer_offset, peek.address, peek.size, peek.shader,
                                       peek.slot, cloth_dispatch, 1);
        }
    }
    cloth_peeks.clear();
    DebugState.IncDispatch();

    ResetBindings(true);
    if (const u64 hash = pipeline->GetStage(Shader::SwStage::Compute).pgm_hash;
        ClothTraceEnabled() && (hash == 0x1c957b01 || hash == 0x809864d3)) {
        g_cloth_epoch = runtime.AccessEpoch();
        g_cloth_epoch_shader = hash;
    }
    scheduler.KickRecording();
}

u64 Rasterizer::Flush() {
    DrainDrawPipe();
    flush_count.fetch_add(1, std::memory_order_relaxed);
    const u64 current_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return current_tick;
}

void Rasterizer::Finish() {
    DrainDrawPipe();
    scheduler.Finish();
}

void Rasterizer::OnSubmit() {
    DrainDrawPipe();
    buffer_cache.ReclaimArenaMemory();
    buffer_cache.PollPeeks();
    memory->CheckCopiedSources();
    CheckSrtWalks();
    CheckScalarCopySources();
    if (fault_process_pending) {
        fault_process_pending = false;
        buffer_cache.ProcessFaultBuffer();
    }
    SyncSmallGpuWrites();
    texture_cache.ProcessDownloadImages();
    // Diagnostics: GOW3_RETEXTURE_FILE=<file>; creating it re-reads all CPU-sourced textures.
    static const char* retexture = std::getenv("GOW3_RETEXTURE_FILE");
    static auto retexture_poll = std::chrono::steady_clock::time_point{};
    if (retexture && retexture[0] && TriggerPollDue(retexture_poll) &&
        std::filesystem::exists(retexture)) {
        std::error_code ec;
        std::filesystem::remove(retexture, ec);
        std::printf("Texture cache: %zu textures marked for re-upload\n", texture_cache.DebugInvalidateAll());
    }
    texture_cache.RunGarbageCollector();
    runtime.TickFrame();
    // gow3 statistics: container sizes, to find what grows while the frame rate falls.
    static auto sizes_at = std::chrono::steady_clock::now();
    if (const auto now = std::chrono::steady_clock::now(); now - sizes_at >= std::chrono::seconds(5)) {
        sizes_at = now;
        std::printf("Sizes: textures: %s\n", texture_cache.DebugSizes().c_str());
        std::printf("Sizes: buffers: %s\n", buffer_cache.DebugSizes().c_str());
        std::printf("Sizes: pipelines: %s; scheduler: %s\n", pipeline_cache.DebugSizes().c_str(),
                    scheduler.DebugSizes().c_str());
        std::printf("Sizes: rasterizer: %zu SRT plans, %zu keep-alive entries, %zu small GPU writes\n",
                    srt_plans.size(), pipe_keepalive.size(), small_gpu_writes.size());
        std::fflush(stdout);
    }
}

// gow3: Uncharted writes per-object constants with a GPU copy shader (2d215c3b) and its draws read
// them through SRT pointers, which the CPU-side walker flattens from guest memory. With Relaxed
// readbacks those GPU writes reach guest memory only when the CPU later writes the same page, so
// the walker kept reading zeros (ground missing, Nate unlit until a menu happened to touch the
// page). GOW3_SYNC_GPU_CONSTANTS=1 copies small shader writes back at each submission; the next
// frame's walker sees them. GOW3_READBACK_TRIGGER=<file>: "auto"/"off" switches that at run
// time; "<lo> <hi>" (hex) copies one range back once (diagnostics).
// gow3: a shader reading guest memory through computed addresses (DMA, e.g. Uncharted's tiled
// lighting cs 43b8ee5e) needs CPU writes on the GPU first. The full sync uploads every
// CPU-modified resident range (per-frame constant rings, command buffers, ...) and stalls on
// staging space: about half of the GPU thread's time in Uncharted's temple. GOW3_DMA_TRIGGER
// (file, read live) selects the mode: "full", "off", or "pointers <KiB>" = only windows of that
// size around the guest addresses in the stage's user data and flattened SRT data.
// GOW3_DMA_SYNC sets the start mode the same way. GOW3_FRAME_STATS adds 5 s statistics.
void Rasterizer::SyncDmaMemory(const Shader::Info* stage) {
    enum class Mode { Full, Off, Pointers };
    static Mode mode = Mode::Full;
    static u64 window = 1_MB;
    static const auto parse = [](const std::string& text) {
        if (text.starts_with("bias")) {
            depth_bias_off = text.find("off") != std::string::npos;
            std::printf("GPU depth bias: %s%c", depth_bias_off.load() ? "off" : "normal", 10);
            std::fflush(stdout);
            return;
        }
        if (text.starts_with("dbclear")) {
            depth16_clear_one = text.find("one") != std::string::npos;
            std::printf("GPU 16-bit depth clears: %s\n", depth16_clear_one.load() ? "1.0" : "normal");
            std::fflush(stdout);
            return;
        }
        if (text.starts_with("clear")) {
            shadow_clear_far = text.find("far") != std::string::npos;
            std::printf("GPU shadow clear quad: %s\n", shadow_clear_far.load() ? "far (1.0)" : "normal");
            std::fflush(stdout);
            return;
        }
        if (text.starts_with("cmp")) {
            const int value = text.find("never") != std::string::npos          ? 1
                              : text.find("always") != std::string::npos       ? 2
                              : text.find("border-black") != std::string::npos ? 3
                              : text.find("border-white") != std::string::npos ? 4
                              : text.find("edge") != std::string::npos         ? 5
                                                                               : 0;
            VideoCore::sampler_compare_override = value;
            static const char* const names[] = {"normal", "never", "always", "border-black", "border-white", "edge"};
            std::printf("GPU compare samplers: %s\n", names[value]);
            std::fflush(stdout);
            return;
        }
        if (text.starts_with("off")) {
            mode = Mode::Off;
        } else if (text.starts_with("pointers")) {
            mode = Mode::Pointers;
            const u64 kib = std::strtoull(text.c_str() + 8, nullptr, 10);
            window = std::clamp<u64>(kib ? kib : 1024, 64, 65536) * 1024;
        } else {
            mode = Mode::Full;
        }
        std::printf("GPU DMA sync: %s", mode == Mode::Full ? "full" : mode == Mode::Off ? "off" : "pointers");
        if (mode == Mode::Pointers) {
            std::printf(" (%llu KiB windows)", (unsigned long long)(window / 1024));
        }
        std::printf("\n");
        std::fflush(stdout);
    };
    static const bool init = [] {
        if (const char* env = std::getenv("GOW3_DMA_SYNC"); env && env[0]) {
            parse(env);
        }
        return true;
    }();
    (void)init;
    static const char* trigger = std::getenv("GOW3_DMA_TRIGGER");
    static auto last_poll = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (trigger && trigger[0] && now - last_poll > std::chrono::milliseconds(500)) {
        last_poll = now;
        if (std::filesystem::exists(trigger)) {
            std::string text;
            if (FILE* f = std::fopen(trigger, "r")) {
                char line[64]{};
                if (std::fgets(line, sizeof(line), f)) {
                    text = line;
                }
                std::fclose(f);
            }
            std::error_code ec;
            std::filesystem::remove(trigger, ec);
            parse(text);
        }
    }
    static const bool stats = std::getenv("GOW3_FRAME_STATS") != nullptr;
    const u64 bytes_before = Gow3Stats::buffer_upload_bytes.load(std::memory_order_relaxed);
    const auto start = std::chrono::steady_clock::now();
    u32 windows = 0;
    if (mode == Mode::Full || !stage) {
        buffer_cache.SynchronizeDmaBuffers();
    } else if (mode == Mode::Pointers) {
        // Guest addresses: dword pairs (V# base or raw pointer) in the direct memory range.
        boost::container::small_vector<VAddr, 16> bases;
        const auto scan = [&](std::span<const u32> data) {
            for (size_t i = 0; i + 1 < data.size(); ++i) {
                const VAddr address = data[i] | (VAddr(data[i + 1] & 0xFFFF) << 32);
                if (address < 0x1000000000ULL || address >= 0x3000000000ULL) {
                    continue;
                }
                const VAddr base = Common::AlignDown(address, 4_KB);
                if (std::ranges::find(bases, base) == bases.end()) {
                    bases.push_back(base);
                }
            }
        };
        scan(stage->user_data);
        scan(stage->flattened_ud_buf);
        // ObtainBuffer makes the window resident and uploads its CPU writes: a DMA read of a
        // page the GPU page table does not map yet returns 0 and is only mapped after the
        // submission (FaultManager), so per-frame data on fresh pages flickered (Uncharted hair
        // and cobwebs reading their shadow cascades through MUBUF ADDR64 loads). A window can
        // cover fiber stacks: those stay writable (gow3gpu.cpp Gow3KeepWritable).
        for (const VAddr base : bases) {
            if (IsMapped(base, window)) {
                const auto [buffer, offset] = buffer_cache.ObtainBuffer(base, static_cast<u32>(window), false);
                if (std::ranges::find(dma_read_arenas, buffer) == dma_read_arenas.end()) {
                    dma_read_arenas.push_back(buffer);
                }
            }
        }
        // DMA reads go through the page table to any address of these arenas, and the barrier
        // tracker knows nothing about them: the uploads just recorded (and any other pending
        // write there) must finish before the draw reads, and the draw's reads before later
        // copies overwrite them (ResetBindings). Without this the hair's shadow cascade data
        // was read mid-upload now and then (Uncharted 3: hair lit for a frame).
        for (const auto* arena : dma_read_arenas) {
            needs_barrier |= runtime.IsBufferAccessed(arena, 0, arena->SizeBytes());
        }
        windows = static_cast<u32>(bases.size());
    }
    if (stats) {
        static u64 calls, bytes, ns, window_count;
        static auto report = std::chrono::steady_clock::now();
        const auto end = std::chrono::steady_clock::now();
        ++calls;
        bytes += Gow3Stats::buffer_upload_bytes.load(std::memory_order_relaxed) - bytes_before;
        ns += std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        window_count += windows;
        if (end - report >= std::chrono::seconds(5)) {
            report = end;
            std::printf("DMA sync (5 s): %llu calls, %.1f MB uploaded, %.1f ms on the GPU thread, %llu windows\n",
                        (unsigned long long)calls, bytes / 1e6, ns / 1e6, (unsigned long long)window_count);
            std::fflush(stdout);
            calls = bytes = ns = window_count = 0;
        }
    }
}

bool Rasterizer::ReadBackEopResults() {
    // gow3: tiny GPU-written images (TextureCache::ProcessTinyReadbacks), unprotected: the CPU
    // reads the previous value until they land (Uncharted 2 reads its aim focus distance every
    // frame; a read fault waiting for the GPU each time stalled its threads). Queued first, so the
    // fence waits for the last readback below if there is one.
    bool soft_pending = false;
    if (!eop_soft_readbacks.empty()) {
        std::erase_if(eop_soft_readbacks, [this](const auto& r) { return !IsMapped(r.first, r.second); });
        if (!eop_soft_readbacks.empty()) {
            buffer_cache.DownloadRangesAsync(eop_soft_readbacks, false, true);
            soft_pending = true;
        }
        eop_soft_readbacks.clear();
    }
    // Ray-cast results: unprotected (see eop_open_readbacks); their fence waits for them.
    bool open_pending = false;
    if (!eop_open_readbacks.empty()) {
        std::erase_if(eop_open_readbacks,
                      [this](const auto& r) { return !IsMapped(r.first, r.second); });
        if (!eop_open_readbacks.empty()) {
            if (EopReadbackMode() == 2) {
                buffer_cache.DownloadRanges(eop_open_readbacks);
            } else {
                buffer_cache.DownloadRangesAsync(eop_open_readbacks, false, true, true);
                open_pending = true;
            }
        }
        eop_open_readbacks.clear();
    }
    if (eop_readbacks.empty()) {
        return soft_pending || open_pending;
    }
    std::erase_if(eop_readbacks, [this](const auto& r) { return !IsMapped(r.first, r.second); });
    static u32 described = 0;
    for (const auto& [address, size] : eop_readbacks) {
        if (described < 6) {
            ++described;
            std::printf("EOP readback range %#llx size %llu\n", static_cast<unsigned long long>(address),
                        static_cast<unsigned long long>(size));
        }
    }
    bool pending = false;
    if (EopReadbackMode() == 2) {
        buffer_cache.DownloadRanges(eop_readbacks);
    } else {
        buffer_cache.DownloadRangesAsync(eop_readbacks, true, true);
        pending = true;
    }
    eop_readbacks.clear();
    static u32 readbacks = 0;
    static auto last = std::chrono::steady_clock::now();
    ++readbacks;
    if (const auto now = std::chrono::steady_clock::now(); now - last >= std::chrono::seconds(5)) {
        std::printf("EOP readbacks (5 s): %u (ray cast results and 6a6422a9 copies, read-protected "
                    "until copied); fence labels inside one: %u\n",
                    readbacks, VideoCore::BufferCache::TakeLabelsInReadbacks());
        std::fflush(stdout);
        readbacks = 0;
        last = now;
    }
    return pending || open_pending;
}

void Rasterizer::SyncSmallGpuWrites() {
    buffer_cache.PollReadbacks();
    static const char* trigger = std::getenv("GOW3_READBACK_TRIGGER");
    static auto trigger_poll = std::chrono::steady_clock::time_point{};
    if (trigger && trigger[0] && TriggerPollDue(trigger_poll) && std::filesystem::exists(trigger)) {
        std::string text;
        if (FILE* f = std::fopen(trigger, "r")) {
            char line[128]{};
            if (std::fgets(line, sizeof(line), f)) {
                text = line;
            }
            std::fclose(f);
        }
        std::error_code ec;
        std::filesystem::remove(trigger, ec);
        if (text.starts_with("auto")) {
            sync_gpu_constants = true;
            std::printf("GPU constants sync: on\n");
        } else if (text.starts_with("off")) {
            sync_gpu_constants = false;
            small_gpu_writes.clear();
            std::printf("GPU constants sync: off\n");
        } else if (text.starts_with("range")) {
            // "range <lo> <hi>": downloaded after every submission (until "range clear").
            char* end = nullptr;
            const u64 lo = std::strtoull(text.c_str() + 5, &end, 16);
            const u64 hi = end ? std::strtoull(end, nullptr, 16) : 0;
            if (text.find("clear") != std::string::npos) {
                synced_ranges.clear();
                std::printf("GPU readback ranges: cleared\n");
            } else if (hi > lo && IsMapped(lo, hi - lo)) {
                synced_ranges.emplace_back(lo, hi - lo);
                std::printf("GPU readback range %#llx-%#llx: after every submission\n",
                            (unsigned long long)lo, (unsigned long long)hi);
            } else {
                std::printf("GPU readback: ignored trigger '%s'\n", text.c_str());
            }
        } else {
            char* end = nullptr;
            const u64 lo = std::strtoull(text.c_str(), &end, 16);
            const u64 hi = end ? std::strtoull(end, nullptr, 16) : 0;
            if (hi > lo && IsMapped(lo, hi - lo)) {
                const std::pair<VAddr, u64> range{lo, hi - lo};
                buffer_cache.DownloadRanges(std::span{&range, 1});
                std::printf("GPU readback: %#llx-%#llx copied to guest memory\n",
                            (unsigned long long)lo, (unsigned long long)hi);
            } else {
                std::printf("GPU readback: ignored trigger '%s'\n", text.c_str());
            }
        }
        std::fflush(stdout);
    }
    if (!synced_ranges.empty()) {
        buffer_cache.DownloadRanges(synced_ranges);
    }
    // Pages the CPU now shares with GPU data in flight (BufferCache: shared write path).
    for (const auto& range : buffer_cache.TakeSharedReadbacks()) {
        small_gpu_writes.push_back(range);
    }
    if (!sync_gpu_constants || small_gpu_writes.empty()) {
        return;
    }
    // Merged in 256-byte granules (GOW3_SMALL_READBACK_ALIGN=<bytes>, a power of two). They were
    // 64 KiB windows ("the copy shader writes many small neighbouring pieces"): with the effect
    // shaders' small writes (cs 8f067540) spread over 30 MiB, the windows joined up and every
    // GPU-written byte between them came back each submission, ~2.3 GB per 5 s in Uncharted 2
    // and 3 that the CPU never reads (16 Oct). Neighbouring pieces still merge.
    static const u64 granule = [] {
        const char* env = std::getenv("GOW3_SMALL_READBACK_ALIGN");
        const u64 value = env ? std::strtoull(env, nullptr, 0) : 0;
        return value >= 64 && std::has_single_bit(value) ? value : u64(256);
    }();
    std::sort(small_gpu_writes.begin(), small_gpu_writes.end());
    std::vector<std::pair<VAddr, u64>> windows;
    for (const auto& [address, size] : small_gpu_writes) {
        const VAddr start = Common::AlignDown(address, granule);
        const VAddr end = Common::AlignUp(address + size, granule);
        if (!windows.empty() && start <= windows.back().first + windows.back().second) {
            const VAddr back_end = windows.back().first + windows.back().second;
            windows.back().second = std::max(back_end, end) - windows.back().first;
        } else {
            windows.emplace_back(start, end - start);
        }
    }
    small_gpu_writes.clear();
    std::erase_if(windows, [this](const auto& w) { return !IsMapped(w.first, w.second); });
    // gow3: GOW3_ASYNC_READBACK=1: the copies run after this submission and reach guest memory
    // when the GPU is done (BufferCache::PollReadbacks, per draw), with no wait for the whole
    // frame on the GPU; until then their pages are read-protected (a read waits for that
    // readback). =2: unprotected, as first tried: data the game (or the SRT walker) read the
    // next frame could be stale (Uncharted 3 "Command context dcb full" at the London pub;
    // garbage sharps for the treasure pickup effect, cs 8f067540, in U1 and U2). Off by default.
    // Since 15 Oct the default is 2 (unset): with the copy shaders and CP fills/copies written
    // through (BufferCache::WriteThrough), GPU-written resource tables downloaded on demand
    // (SyncSrtTables) and no lazy-write merges, these readbacks only keep CPU-read GPU results
    // (luminance and the like) about a frame behind, as on the console; the full wait per
    // submission cost ~12 ms per frame in heavy scenes. GOW3_ASYNC_READBACK=0 waits instead.
    static const bool async_readback = [] {
        const char* env = std::getenv("GOW3_ASYNC_READBACK");
        return !(env && env[0] == '0');
    }();
    if (async_readback) {
        buffer_cache.DownloadRangesAsync(windows);
    } else {
        buffer_cache.DownloadRanges(windows);
    }
}

// gow3: resource tables (SRT) that another shader writes earlier in the same command stream.
// Uncharted 2's treasure pickup effect (cs 7d7e7206, 8f067540) bound garbage V#s and the GPU
// faulted (device lost): the CPU-side walker reads guest memory, which receives GPU writes only
// at the per-submission readback (SyncSmallGpuWrites) or later, so a new effect's tables were not
// there yet. The tables a walker reads come from its code (PushPtr: push rdi; mov rdi,
// [rdi + off]; reads [rdi + off]; pop rdi). Tables with GPU-written bytes are downloaded before
// the walk (one GPU wait each). GOW3_SRT_SYNC=0 turns this off.
const Rasterizer::SrtPlan& Rasterizer::SrtPlanFor(const Shader::Info& info) {
    const auto& srt = info.srt_info;
    // gow3 20 Oct 2026 (laptop CPUs): the map lookup on every walk was ~1% of the GPU command
    // thread on a Ryzen 7840HS (NVIDIA GPU and 780M, Uncharted 3). Plans are never erased and the
    // map's nodes do not move, so a small cache of plan addresses by walker is safe.
    const void* walker = reinterpret_cast<const void*>(srt.walker_func);
    auto& memo = srt_plan_memo[(reinterpret_cast<uintptr_t>(walker) >> 4) & (SrtPlanMemoSize - 1)];
    if (memo.first == walker && memo.second) {
        return *memo.second;
    }
    auto [it, inserted] = srt_plans.try_emplace(walker);
    auto& plan = it->second;
    if (!inserted) {
        memo = {walker, &plan};
        return plan;
    }
    // Straight-line code: rdi is the current table (the user data at the start), rsi the
    // flattened buffer; values pass through r10.
    std::vector<s32> saved;
    s32 current = -1;
    s32 last_node = -2;
    u32 last_off = 0;
    const u8* code = reinterpret_cast<const u8*>(srt.walker_func);
    const u8* end = code + srt.walker_func_size;
    ZydisDecodedInstruction inst;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    while (code < end && ZYAN_SUCCESS(Common::Decoder::Instance()->decodeInstruction(
                             inst, ops, const_cast<u8*>(code), end - code))) {
        code += inst.length;
        const bool rdi_reg = inst.operand_count_visible > 0 &&
                             ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                             ops[0].reg.value == ZYDIS_REGISTER_RDI;
        if (inst.mnemonic == ZYDIS_MNEMONIC_RET) {
            break;
        } else if (inst.mnemonic == ZYDIS_MNEMONIC_PUSH && rdi_reg) {
            saved.push_back(current);
        } else if (inst.mnemonic == ZYDIS_MNEMONIC_POP && rdi_reg) {
            current = saved.empty() ? -1 : saved.back();
            if (!saved.empty()) {
                saved.pop_back();
            }
        } else if (inst.mnemonic == ZYDIS_MNEMONIC_XOR && rdi_reg) {
            // A pointer load the fault handler patched out: a null table.
            plan.nodes.push_back({current, 0, 0, true});
            current = s32(plan.nodes.size() - 1);
        } else if (inst.mnemonic == ZYDIS_MNEMONIC_MOV && inst.operand_count_visible == 2 &&
                   ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY &&
                   ops[1].mem.base == ZYDIS_REGISTER_RDI) {
            const bool indexed = ops[1].mem.index != ZYDIS_REGISTER_NONE;
            const u32 disp = ops[1].mem.disp.has_displacement ? u32(ops[1].mem.disp.value) : 0;
            if (rdi_reg) {
                plan.nodes.push_back({current, disp, 0, indexed});
                current = s32(plan.nodes.size() - 1);
            } else {
                last_node = indexed ? -2 : current;
                last_off = disp;
                if (current >= 0) {
                    // A computed read offset: the first KiB.
                    const u32 extent = indexed ? 1024 : disp + ops[1].size / 8;
                    plan.nodes[current].extent = std::max(plan.nodes[current].extent, extent);
                }
            }
        } else if (inst.mnemonic == ZYDIS_MNEMONIC_MOV && inst.operand_count_visible == 2 &&
                   ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY &&
                   ops[0].mem.base == ZYDIS_REGISTER_RSI && last_node != -2) {
            const u32 disp = ops[0].mem.disp.has_displacement ? u32(ops[0].mem.disp.value) : 0;
            for (u32 b = 0; b < ops[0].size / 8; b += 4) {
                plan.copies.push_back({(disp + b) / 4, last_node, last_off + b});
            }
        }
    }
    return plan;
}

// gow3 21 Oct 2026 (GOW3_SRT_CHECK): the draw whose tables the GPU thread (or the draw pipe's
// stage B, which has its own registers) walks right now.
Rasterizer::SrtDrawContext Rasterizer::CurrentDrawContext() const {
    const auto& regs = Regs();
    return {
        .color0 = regs.color_buffers[0] ? regs.color_buffers[0].Address() : 0,
        .depth = regs.depth_buffer.DepthValid() ? regs.depth_buffer.DepthAddress() : 0,
        .index = regs.index_base_address.Address<VAddr>(),
        .num_indices = regs.num_indices,
    };
}

VAddr Rasterizer::SrtSourceAddress(Shader::PFN_SrtWalker walker, size_t walker_size,
                                   std::span<const u32> user_data, u32 flat_dw, const char** kind) {
    Shader::Info info{};
    info.srt_info.walker_func = walker;
    info.srt_info.walker_func_size = walker_size;
    info.user_data = user_data;
    const auto& plan = SrtPlanFor(info);
    std::vector<VAddr> address(plan.nodes.size(), 0);
    ResolveSrtTables(plan, info, address.data());
    *kind = "not in plan";
    for (const auto& copy : plan.copies) {
        if (copy.flat_dw != flat_dw) {
            continue;
        }
        if (copy.node < 0) {
            *kind = "user data";
            return 0;
        }
        const VAddr source = address[copy.node] ? address[copy.node] + copy.src_off : 0;
        if (!source) {
            *kind = "table unknown";
            return 0;
        }
        if (buffer_cache.IsRangeGpuWritten(source, 4)) {
            *kind = buffer_cache.PendingReadbackTickFor(source, 4) ? "GPU-written, readback pending"
                                                                        : "GPU-written";
        } else if (buffer_cache.IsRegionCpuModified(source, 4)) {
            *kind = "CPU-written since upload";
        } else {
            *kind = "untracked/clean";
        }
        return source;
    }
    return 0;
}

void Rasterizer::ResolveSrtTables(const SrtPlan& plan, const Shader::Info& info, VAddr* address) {
    const auto user_data = std::as_bytes(info.user_data);
    for (size_t i = 0; i < plan.nodes.size(); ++i) {
        const auto& node = plan.nodes[i];
        address[i] = 0;
        if (node.dynamic) {
            continue;
        }
        u64 value = 0;
        if (node.parent < 0) {
            if (node.offset + sizeof(value) > user_data.size()) {
                continue;
            }
            std::memcpy(&value, user_data.data() + node.offset, sizeof(value));
        } else {
            const VAddr table = address[node.parent];
            if (!table || !IsMapped(table + node.offset, sizeof(value))) {
                continue;
            }
            std::memcpy(&value, reinterpret_cast<const void*>(table + node.offset), sizeof(value));
        }
        address[i] = value & 0xFFFFFFFFFFFFULL;
    }
}

void Rasterizer::SyncSrtTables(const Shader::Info& info) {
    const auto& srt = info.srt_info;
    if (!srt.walker_func || !srt.walker_func_size || !OnStageA() ||
        PipelineCache::capturing_for_gpu_thread) {
        return;
    }
    const auto& plan = SrtPlanFor(info);
    if (plan.nodes.empty()) {
        return;
    }
    const auto user_data = std::as_bytes(info.user_data);
    boost::container::small_vector<VAddr, 16> address(plan.nodes.size(), 0);
    for (size_t i = 0; i < plan.nodes.size(); ++i) {
        const auto& node = plan.nodes[i];
        if (node.dynamic) {
            continue;
        }
        u64 value = 0;
        if (node.parent < 0) {
            if (node.offset + sizeof(value) > user_data.size()) {
                continue;
            }
            std::memcpy(&value, user_data.data() + node.offset, sizeof(value));
        } else {
            const VAddr table = address[node.parent];
            if (!table || !IsMapped(table + node.offset, sizeof(value))) {
                continue;
            }
            std::memcpy(&value, reinterpret_cast<const void*>(table + node.offset), sizeof(value));
        }
        const VAddr table = value & 0xFFFFFFFFFFFFULL;
        address[i] = table;
        // GOW3_SRT_PENDING_DRAIN: a copy shader queued for stage B (scalar copy HLE) that writes
        // into this table runs before the walk reads it. Parents come first in the plan, so the
        // pointers read so far are current.
        if (table && node.extent && draw_pipe && !pending_copy_writes.empty() &&
            PendingCopyWriteOverlaps(table, node.extent) && IsMapped(table, node.extent)) {
            const bool report = SrtPendingDrain() == 2;
            const u32 bytes = std::min<u32>(node.extent, 512);
            std::array<u8, 512> before;
            if (report) {
                std::memcpy(before.data(), reinterpret_cast<const void*>(table), bytes);
            }
            const auto wait_start = report ? std::chrono::steady_clock::now()
                                           : std::chrono::steady_clock::time_point{};
            WaitPendingWrites(pending_copy_writes, table, node.extent);
            if (report) {
                srt_pending_ns += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now() - wait_start)
                                          .count());
            }
            ++srt_pending_drains;
            if (report) {
                u32 changed = 0;
                bool zero = true;
                const auto* now = reinterpret_cast<const u8*>(table);
                for (u32 b = 0; b < bytes; ++b) {
                    changed += before[b] != now[b];
                    zero &= before[b] == 0;
                }
                static std::map<u64, std::array<u64, 3>> by_shader; // drains, changed, all zero
                static u32 lines = 0;
                auto& s = by_shader[info.pgm_hash];
                ++s[0];
                if (changed) {
                    ++s[1];
                    ++srt_pending_changed;
                    s[2] += zero;
                    srt_pending_zero += zero;
                    if (lines < 24) {
                        ++lines;
                        std::printf("SRT pending: shader %016llx table %#llx (+%u bytes, level %d): "
                                    "%u bytes changed by stage B's queued writes%s\n",
                                    static_cast<unsigned long long>(info.pgm_hash),
                                    static_cast<unsigned long long>(table), node.extent,
                                    node.parent < 0 ? 1 : 2, changed,
                                    zero ? " (were all zero)" : "");
                    }
                }
                static auto last = std::chrono::steady_clock::now();
                if (const auto t = std::chrono::steady_clock::now(); t - last >= std::chrono::seconds{5}) {
                    last = t;
                    std::string top;
                    for (const auto& [hash, n] : by_shader) {
                        if (n[1]) {
                            top += fmt::format(" {:08x}:{}/{}{}", hash, n[1], n[0],
                                               n[2] ? fmt::format("(z{})", n[2]) : std::string{});
                        }
                    }
                    std::printf("SRT pending (5 s): %llu walks waited for stage B (%.1f ms), %llu tables "
                                "changed by it (%llu all zero before); changed/waits by shader:%s\n",
                                static_cast<unsigned long long>(srt_pending_drains),
                                srt_pending_ns / 1e6,
                                static_cast<unsigned long long>(srt_pending_changed),
                                static_cast<unsigned long long>(srt_pending_zero), top.c_str());
                    std::fflush(stdout);
                    by_shader.clear();
                    srt_pending_drains = srt_pending_changed = srt_pending_zero = srt_pending_ns = 0;
                }
            }
        }
        if (!table || !node.extent || !buffer_cache.IsRangeGpuWrittenShared(table, node.extent) ||
            !IsMapped(table, node.extent)) {
            continue;
        }
        DrainDrawPipe();
        const std::pair<VAddr, u64> range{table, node.extent};
        buffer_cache.DownloadRanges(std::span{&range, 1});
        ++srt_syncs;
        if (srt_sync_reports < 16) {
            ++srt_sync_reports;
            std::printf("SRT sync: shader %016llx table %#llx (+%u bytes, level %d) had "
                        "GPU-written data; downloaded before the walk\n",
                        static_cast<unsigned long long>(info.pgm_hash),
                        static_cast<unsigned long long>(table), node.extent,
                        node.parent < 0 ? 1 : 2);
            std::fflush(stdout);
        }
    }
    static const bool stats = std::getenv("GOW3_FRAME_STATS") != nullptr;
    // gow3 21 Oct 2026 (GOW3_CPU_TRIMS=1): the clock every 256 walks (once a table had synced it
    // was read on every walk: ~1% of the GPU command thread).
    static u32 stat_walks = 0;
    if (stats && srt_syncs && (!CpuTrims() || (++stat_walks & 255) == 0)) {
        static auto report = std::chrono::steady_clock::now();
        if (const auto now = std::chrono::steady_clock::now(); now - report >= std::chrono::seconds(5)) {
            report = now;
            std::printf("SRT sync (5 s): %llu tables downloaded before the walk\n",
                        static_cast<unsigned long long>(srt_syncs));
            std::fflush(stdout);
            srt_syncs = 0;
        }
    }
}

void Rasterizer::ReportSrtFault(const Shader::Info& info) {
    static u32 reports = 0;
    if (reports >= 12 || !info.srt_info.walker_func) {
        return;
    }
    ++reports;
    const auto& plan = SrtPlanFor(info);
    const auto describe = [&](const char* when) {
        boost::container::small_vector<VAddr, 16> address(plan.nodes.size(), 0);
        ResolveSrtTables(plan, info, address.data());
        std::string text = fmt::format("SRT fault report ({}): shader {:016x}", when, info.pgm_hash);
        for (size_t i = 0; i < plan.nodes.size(); ++i) {
            const auto& node = plan.nodes[i];
            const VAddr table = address[i];
            text += fmt::format("\n  table {} at {:#x} (+{} in {}{}, {} bytes{})", i, table,
                                node.offset, node.parent < 0 ? "user data" : "table ",
                                node.parent < 0 ? std::string{} : std::to_string(node.parent),
                                node.extent, node.dynamic ? ", computed" : "");
            if (!table || !node.extent) {
                continue;
            }
            const u64 size = std::min<u64>(node.extent, 64);
            if (!IsMapped(table, size)) {
                text += " NOT MAPPED";
                continue;
            }
            text += fmt::format(" gpu-written {}:", buffer_cache.IsRangeGpuWritten(table, node.extent));
            for (u64 off = 0; off + 8 <= size; off += 8) {
                u64 value;
                std::memcpy(&value, reinterpret_cast<const void*>(table + off), sizeof(value));
                text += fmt::format(" {:016x}", value);
            }
        }
        std::printf("%s\n", text.c_str());
        std::fflush(stdout);
    };
    describe("at the walk");
    std::this_thread::sleep_for(std::chrono::milliseconds{3});
    describe("3 ms later");
}

void Rasterizer::DescribeSharpSource(const Shader::Info& info, u32 flat_dw, const char* problem) {
    static std::unordered_map<u64, u32> per_shader;
    static u32 total = 0;
    if (total >= 40 || per_shader[info.pgm_hash]++ >= 4) {
        return;
    }
    ++total;
    const auto ud = info.user_data;
    std::string text = fmt::format("Garbage V# ({}): shader {:016x} flat dword {}", problem,
                                   info.pgm_hash, flat_dw);
    if (flat_dw < info.flattened_ud_buf.size() && flat_dw + 4 <= info.flattened_ud_buf.size()) {
        text += fmt::format(" = {:08x} {:08x} {:08x} {:08x}", info.flattened_ud_buf[flat_dw],
                            info.flattened_ud_buf[flat_dw + 1], info.flattened_ud_buf[flat_dw + 2],
                            info.flattened_ud_buf[flat_dw + 3]);
    }
    text += "; user data";
    for (const u32 v : ud) {
        text += fmt::format(" {:08x}", v);
    }
    if (flat_dw < Shader::NUM_USER_DATA_REGS || !info.srt_info.walker_func) {
        std::printf("%s; from the user data\n", text.c_str());
        std::fflush(stdout);
        return;
    }
    const auto& plan = SrtPlanFor(info);
    boost::container::small_vector<VAddr, 16> address(plan.nodes.size(), 0);
    ResolveSrtTables(plan, info, address.data());
    const auto copy = std::ranges::find(plan.copies, flat_dw, &SrtCopy::flat_dw);
    if (copy == plan.copies.end()) {
        std::printf("%s; source not found in the walker (%zu tables, %zu copies)\n", text.c_str(),
                    plan.nodes.size(), plan.copies.size());
        std::fflush(stdout);
        return;
    }
    for (s32 n = copy->node; n >= 0; n = plan.nodes[n].parent) {
        const auto& node = plan.nodes[n];
        text += fmt::format("; table {} at {:#x} (+{} in {}{}, {} bytes read{})", n, address[n],
                            node.offset, node.parent < 0 ? "user data" : "table ",
                            node.parent < 0 ? std::string{} : std::to_string(node.parent),
                            node.extent, node.dynamic ? ", computed offset" : "");
        if (address[n] && node.extent) {
            text += fmt::format(" gpu-written {}",
                                buffer_cache.IsRangeGpuWritten(address[n], node.extent));
        }
    }
    if (copy->node >= 0 && address[copy->node]) {
        const VAddr src = address[copy->node] + copy->src_off;
        text += fmt::format("; read at {:#x}", src);
        if (IsMapped(src, 16)) {
            const u32* p = reinterpret_cast<const u32*>(src);
            text += fmt::format(" now {:08x} {:08x} {:08x} {:08x} gpu-written {}", p[0], p[1], p[2],
                                p[3], buffer_cache.IsRangeGpuWritten(src, 16));
        }
    }
    std::printf("%s\n", text.c_str());
    std::fflush(stdout);
}

const PreparedStage* Rasterizer::FindPreparedStage(const Shader::Info& stage) const {
    // Sharps a draw-preparation worker read from the same flattened user data.
    if (!bind_prepared || Gow3Toggle::Disabled(Gow3Toggle::PreparedResources)) {
        return nullptr;
    }
    for (u32 i = 0; i < bind_prepared->num_stages; ++i) {
        const auto& candidate = bind_prepared->stages[i];
        if (&candidate.program->info == &stage &&
            candidate.num_images == stage.images.size() &&
            candidate.num_samplers == stage.samplers.size() &&
            candidate.num_buffers == stage.buffers.size()) {
            return &candidate;
        }
    }
    return nullptr;
}

bool Rasterizer::BindHelperWanted() {
    // A spinning helper pays off only with cores to spare (the recording thread spins too).
    const char* env = std::getenv("GOW3_TEXTURE_HELPER");
    if (env && env[0]) {
        return env[0] == '1';
    }
    // Measured neutral on 16 threads (docs/parallel_gpu.md): opt-in.
    return false;
}

bool Rasterizer::HelperEligible(const Pipeline* pipeline) const {
    if (pipeline->IsCompute() || !bind_helper.Available() || FrameCapture::Active() ||
        Gow3Toggle::Disabled(Gow3Toggle::TextureBindHelper) ||
        Gow3Toggle::Disabled(Gow3Toggle::TextureBindingMemo) ||
        Gow3Toggle::Disabled(Gow3Toggle::UpdateImageFastPath)) {
        return false;
    }
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        // Mip arrays make the descriptor layout depend on the T#s.
        for (const auto& image : stage->images) {
            if (image.mip_fallback_mode != Shader::MipStorageFallbackMode::None) {
                return false;
            }
        }
        // Storage writes invalidate cached images from the buffer side.
        for (const auto& buffer : stage->buffers) {
            if (!buffer.IsSpecial() && buffer.is_written) {
                return false;
            }
        }
    }
    return true;
}

bool Rasterizer::TexturesBindableOnHelper(const Shader::Info& stage,
                                          const PreparedStage* prepared) {
    const u64 generation = texture_cache.RegistryGeneration();
    for (u32 image_index = 0; image_index < stage.images.size(); ++image_index) {
        const auto& image_desc = stage.images[image_index];
        if (image_desc.is_written) {
            return false;
        }
        const auto tsharp =
            prepared ? prepared->image_sharps[image_index] : image_desc.GetSharp(stage);
        const auto data_fmt = tsharp.GetDataFmt();
        const auto num_fmt = tsharp.GetNumberFmt();
        if (tsharp.Address() == 0 || data_fmt == AmdGpu::DataFormat::FormatInvalid ||
            !memory->IsValidGpuMapping(tsharp.Address(), 0) ||
            !IsKnownFormat(data_fmt, num_fmt) || !PlausibleTSharp(tsharp)) {
            continue; // null descriptor
        }
        const auto& entry =
            prepared ? CachedImageDescEntry(tsharp, image_desc, prepared->image_hashes[image_index])
                     : CachedImageDescEntry(tsharp, image_desc);
        if (entry.found_generation != generation ||
            !PlausibleImage(entry.desc.info, tsharp, memory, instance.GetMaxImageArrayLayers())) {
            return false;
        }
        VideoCore::ImageId image_id = entry.found_id;
        auto* image = &texture_cache.GetImage(image_id);
        if (const auto depth_image_id = texture_cache.GetAssociatedDepth(*image)) {
            image_id = depth_image_id;
            image = &texture_cache.GetImage(image_id);
        }
        if (image->binding.needs_rebind || !texture_cache.IsUpToDate(image_id) ||
            scene_targets->Tracks(image->image_uid) ||
            (upscaler->Enabled() && upscaler->RedirectsSampled(image_id))) {
            return false;
        }
    }
    return true;
}

void Rasterizer::RunTextureTask(void* context) {
    auto& self = *static_cast<Rasterizer*>(context);
    auto& task = self.texture_task;
    for (u32 i = 0; i < task.count; ++i) {
        auto& stage = task.stages[i];
        if (!self.TexturesBindableOnHelper(*stage.info, stage.prepared)) {
            return;
        }
        auto binding = stage.binding;
        u32 write_index = stage.write_index;
        self.BindTextures(*stage.info, stage.prepared, binding, write_index, task.barrier, true);
        task.completed = i + 1;
    }
}

void Rasterizer::JoinBindHelper(void* context) {
    if (!BindHelper::OnHelper()) {
        static_cast<Rasterizer*>(context)->bind_helper.Join();
    }
}

bool Rasterizer::BindResources(const Pipeline* pipeline) {
    if (IsComputeImageCopy(pipeline) || IsComputeMetaClear(pipeline) ||
        IsComputeImageClear(pipeline)) {
        return false;
    }

    set_write_index = 0;
    set_writes.clear();
    buffer_infos.clear();
    image_infos.clear();
    buffer_cache.PollReadbacks();
    // gow3: G-buffer passes sample material textures for a scene rendered below the output
    // size; the temporal upscaler restores the detail of the output's mip level (FSR guide:
    // log2(render / output)). Shadows, post-processing and UI keep the guest's bias.
    sampler_lod_bias = 0.0f;
    if (!pipeline->IsCompute() && !Gow3Toggle::Disabled(Gow3Toggle::SceneMipBias) &&
        std::popcount(static_cast<const GraphicsPipeline*>(pipeline)->GetGraphicsKey().mrt_mask &
                      0xff) >= 5) {
        sampler_lod_bias = upscaler->SceneMipBias();
        static float reported = 0.0f;
        if (sampler_lod_bias != reported) {
            reported = sampler_lod_bias;
            std::printf("Upscaler: scene texture LOD bias %.2f\n", sampler_lod_bias);
        }
    }

    bool uses_dma = false;
    const Shader::Info* dma_stage = nullptr;

    static const bool stats = std::getenv("GOW3_FRAME_STATS") != nullptr;
    if (stats && ((helper_full + helper_partial + helper_serial) & 1023) == 0) {
        static auto window = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - window >= std::chrono::seconds(5)) {
            window = now;
            const double forks = std::max<double>(1, helper_full + helper_partial);
            std::printf("Texture helper: %llu binds in parallel, %llu partly, %llu serial; "
                        "per fork %.0f cycles helper task, %.0f cycles GPU thread wait\n",
                        static_cast<unsigned long long>(helper_full),
                        static_cast<unsigned long long>(helper_partial),
                        static_cast<unsigned long long>(helper_serial),
                        bind_helper.task_cycles.exchange(0) / forks,
                        bind_helper.wait_cycles / forks);
            bind_helper.wait_cycles = 0;
            helper_full = helper_partial = helper_serial = 0;
        }
    }

    // Bind resource buffers and textures.
    Shader::Backend::Bindings binding{};
    push_data = MakeUserData(Regs());
    if (!HelperEligible(pipeline)) {
        ++helper_serial;
        for (const auto* stage : pipeline->GetStages()) {
            if (!stage) {
                continue;
            }
            set_writes.resize(set_writes.size() + stage->buffers.size() + stage->images.size() +
                              stage->samplers.size());
            stage->PushUd(binding, push_data);
            const PreparedStage* prepared = FindPreparedStage(*stage);
            BindBuffers(*stage, prepared, binding, push_data, set_write_index);
            BindTextures(*stage, prepared, binding, set_write_index, needs_barrier);
            uses_dma |= stage->uses_dma;
            if (stage->uses_dma) {
                dma_stage = stage;
            }
        }
    } else {
        // gow3: textures on the helper, buffers here. Without mip arrays every stage's
        // descriptors are [buffers][images][samplers], one each, so both sides know their
        // binding numbers and descriptor write slots up front.
        struct BufferStage {
            const Shader::Info* info;
            const PreparedStage* prepared;
            Shader::Backend::Bindings binding;
            u32 write_index;
        };
        std::array<BufferStage, Shader::MaxStageTypes> buffer_stages;
        auto& task = texture_task;
        task.count = 0;
        task.completed = 0;
        task.barrier = false;
        for (const auto* stage : pipeline->GetStages()) {
            if (!stage) {
                continue;
            }
            const u32 num_buffers = static_cast<u32>(stage->buffers.size());
            const u32 num_descriptors =
                num_buffers + static_cast<u32>(stage->images.size() + stage->samplers.size());
            set_writes.resize(set_writes.size() + num_descriptors);
            stage->PushUd(binding, push_data);
            const PreparedStage* prepared = FindPreparedStage(*stage);
            buffer_stages[task.count] = {stage, prepared, binding, set_write_index};
            auto texture_binding = binding;
            texture_binding.buffer += num_buffers;
            texture_binding.unified += num_buffers;
            task.stages[task.count] = {stage, prepared, texture_binding,
                                       set_write_index + num_buffers};
            ++task.count;
            binding.buffer += num_buffers;
            binding.unified += num_descriptors;
            set_write_index += num_descriptors;
            uses_dma |= stage->uses_dma;
            if (stage->uses_dma) {
                dma_stage = stage;
            }
        }
        bind_helper.Fork(&RunTextureTask, this);
        for (u32 i = 0; i < task.count; ++i) {
            auto& stage = buffer_stages[i];
            BindBuffers(*stage.info, stage.prepared, stage.binding, push_data, stage.write_index);
        }
        if (draw_inputs.pending && !Gow3Toggle::Disabled(Gow3Toggle::EarlyDrawInputs)) {
            ResolveVertexBuffers(draw_inputs.pipeline, draw_inputs.prepared);
            if (draw_inputs.is_indexed) {
                ResolveIndexBuffer(draw_inputs.index_offset);
            }
            draw_inputs.resolved = true;
        }
        bind_helper.Join();
        needs_barrier |= task.barrier;
        ++(task.completed == task.count ? helper_full
                                        : task.completed ? helper_partial : helper_serial);
        // Stages the helper left (an image needing work that records commands).
        for (u32 i = task.completed; i < task.count; ++i) {
            auto& stage = task.stages[i];
            u32 write_index = stage.write_index;
            BindTextures(*stage.info, stage.prepared, stage.binding, write_index, needs_barrier);
        }
    }

    if (uses_dma) {
        SyncDmaMemory(dma_stage);
        fault_process_pending = true;
    }

    return true;
}

void Rasterizer::BindVertexBuffers(const GraphicsPipeline* pipeline, const PreparedDraw* prepared) {
    ResolveVertexBuffers(pipeline, prepared);
    EmitVertexBuffers();
}

void Rasterizer::EmitVertexBuffers() {
    // gow3: only the used entries go into the recording chunk (the static vectors hold 32 of
    // each: ~3 KiB copied per draw).
    auto& v = vertex_binds;
    const bool dynamic_input = instance.IsVertexInputDynamicState();
    const u32 num_buffers = v.num_buffers;
    scheduler.ReserveRecordData(
        v.bindings.size() * sizeof(v.bindings[0]) + v.attributes.size() * sizeof(v.attributes[0]) +
        num_buffers * (sizeof(vk::Buffer) + 3 * sizeof(vk::DeviceSize)) + 128);
    if (dynamic_input) {
        scheduler.Record([bindings = scheduler.RecordData(std::span<const vk::VertexInputBindingDescription2EXT>{v.bindings.data(), v.bindings.size()}),
                          attributes = scheduler.RecordData(std::span<const vk::VertexInputAttributeDescription2EXT>{
                              v.attributes.data(), v.attributes.size()})](
                             vk::CommandBuffer cmdbuf) {
            cmdbuf.setVertexInputEXT(bindings, attributes);
        });
    }
    if (num_buffers == 0) {
        return;
    }
    const auto buffers = scheduler.RecordData(std::span<const vk::Buffer>{v.host_buffers.data(), num_buffers});
    const auto offsets = scheduler.RecordData(std::span<const vk::DeviceSize>{v.host_offsets.data(), num_buffers});
    if (dynamic_input) {
        scheduler.Record([num_buffers, buffers, offsets](vk::CommandBuffer cmdbuf) {
            cmdbuf.bindVertexBuffers(0, num_buffers, buffers.data(), offsets.data());
        });
        return;
    }
    const auto sizes = scheduler.RecordData(std::span<const vk::DeviceSize>{v.host_sizes.data(), num_buffers});
    const auto strides = scheduler.RecordData(std::span<const vk::DeviceSize>{v.host_strides.data(), num_buffers});
    scheduler.Record([num_buffers, buffers, offsets, sizes, strides](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindVertexBuffers2(0, num_buffers, buffers.data(), offsets.data(), sizes.data(),
                                  strides.data());
    });
}

// gow3 diagnostics (GOW3_CLOTH_PEEK=1): who reads the cloth solvers' outputs (1c957b01,
// 809864d3), and whether what they read is the newest output (the solver's latest dispatch), an
// older one, or no cloth output at all (for readers that read cloth outputs at other times).
// slot ~0u with compute = a solver dispatch starts. Recording thread only (mutex anyway).
// Returns true when a read hits the newest output of a solver.

static bool ClothTrace(u64 shader, u32 slot, VAddr address, u64 size, bool written, bool compute) {
    if (!ClothTraceEnabled()) {
        return false;
    }
    bool newest = false;
    struct Output {
        VAddr end;
        u64 shader, seq;
        u32 slot;
    };
    static std::mutex mutex;
    static std::map<VAddr, Output> outputs;
    static std::map<u64, u64> latest;
    // (reader, slot) -> solver/slot -> newest, previous, older; readers' binds without cloth data
    static std::map<std::pair<u64, u32>, std::map<std::pair<u64, u32>, std::array<u64, 8>>> reads;
    static std::map<std::pair<u64, u32>, u64> misses;
    static auto last = std::chrono::steady_clock::now();
    std::scoped_lock lock{mutex};
    const bool solver = compute && (shader == 0x1c957b01 || shader == 0x809864d3);
    if (solver && slot == ~0u) {
        ++latest[shader];
        return false;
    }
    if (address == 0 || size == 0 || slot == ~0u) {
        return false;
    }
    const VAddr end = address + size;
    auto it = outputs.upper_bound(address);
    if (it != outputs.begin()) {
        --it;
    }
    if (written) {
        // Anything written here replaces cloth outputs it overlaps.
        while (it != outputs.end() && it->first < end) {
            if (it->second.end > address) {
                it = outputs.erase(it);
            } else {
                ++it;
            }
        }
        if (solver) {
            outputs[address] = {end, shader, latest[shader], slot};
        }
    } else {
        bool hit = false;
        for (; it != outputs.end() && it->first < end; ++it) {
            if (it->second.end <= address || (solver && it->second.shader == shader)) {
                continue;
            }
            hit = true;
            const u64 age = latest[it->second.shader] - it->second.seq;
            newest |= age == 0;
            ++reads[{shader, slot}][{it->second.shader, it->second.slot}][age < 5 ? age : age < 10 ? 5 : age < 100 ? 6 : 7];
        }
        if (!hit && reads.contains({shader, slot})) {
            ++misses[{shader, slot}];
        }
    }
    if (const auto now = std::chrono::steady_clock::now(); now - last >= std::chrono::seconds{5}) {
        last = now;
        std::string text;
        for (auto& [reader, sources] : reads) {
            text += fmt::format(" {:08x}/s{}:", reader.first, reader.second);
            for (auto& [source, n] : sources) {
                text += fmt::format(" {:08x}/s{} ages 0-4 {} {} {} {} {}, 5-9 {}, 10-99 {}, 100+ {};", source.first,
                                    source.second, n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7]);
                n = {};
            }
            if (const auto miss = misses.find(reader); miss != misses.end() && miss->second) {
                text += fmt::format(" no cloth data {};", miss->second);
                miss->second = 0;
            }
        }
        std::printf("Cloth readers (5 s):%s\n", text.c_str());
        std::fflush(stdout);
    }
    return newest;
}


static void NoteClothRead(u64 shader, u32 slot, VAddr address, u64 size, u64 tracked, bool hazard,
                          u32 epoch) {
    static u64 protected_reads = 0, flushed_reads = 0, missed = 0;
    static int lines = 0;
    static auto last = std::chrono::steady_clock::now();
    if (epoch != g_cloth_epoch) {
        ++flushed_reads;
    } else if (hazard) {
        ++protected_reads;
    } else {
        ++missed;
        if (lines < 40) {
            ++lines;
            std::printf("Cloth MISSED HAZARD: reader %08llx slot %u at %#llx size %llu tracked %llu "
                        "after %08llx\n",
                        (unsigned long long)shader, slot, (unsigned long long)address,
                        (unsigned long long)size, (unsigned long long)tracked,
                        (unsigned long long)g_cloth_epoch_shader);
        }
    }
    if (const auto now = std::chrono::steady_clock::now(); now - last >= std::chrono::seconds{5}) {
        last = now;
        std::printf("Cloth newest reads (5 s): %llu with a barrier pending, %llu after a flush, %llu missed\n",
                    (unsigned long long)protected_reads, (unsigned long long)flushed_reads,
                    (unsigned long long)missed);
        std::fflush(stdout);
        protected_reads = flushed_reads = missed = 0;
    }
}

void Rasterizer::ResolveVertexBuffers(const GraphicsPipeline* pipeline,
                                      const PreparedDraw* prepared) {
    const auto& regs = Regs();
    auto& v = vertex_binds;
    v.num_buffers = 0;
    v.host_buffers.clear();
    v.host_offsets.clear();
    v.host_sizes.clear();
    v.host_strides.clear();
    auto& attributes = v.attributes;
    auto& bindings = v.bindings;
    attributes.clear();
    bindings.clear();
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT> divisors;
    VertexInputs<AmdGpu::Buffer> guest_buffers;
    // Inputs a draw-preparation worker read from the same registers and (verified) user data.
    const auto& fetch = pipeline->GetFetchShader();
    const PreparedVertexInputs* ready =
        prepared && prepared->vertex.valid && instance.IsVertexInputDynamicState() && fetch &&
                fetch->attributes.size() == prepared->vertex.count && !FrameCapture::Active() &&
                !Gow3Toggle::Disabled(Gow3Toggle::PreparedResources)
            ? &prepared->vertex
            : nullptr;
    if (ready) {
        attributes.assign(ready->attributes, ready->attributes + ready->count);
        bindings.assign(ready->bindings, ready->bindings + ready->count);
        guest_buffers.assign(ready->buffers, ready->buffers + ready->count);
    } else {
        pipeline->GetVertexInputs(attributes, bindings, divisors, guest_buffers,
                                  regs.vgt_instance_step_rate_0, regs.vgt_instance_step_rate_1);
    }

    if (bindings.empty()) {
        // If there are no bindings, there is nothing further to do.
        return;
    }
    if (motion_draw && !guest_buffers.empty()) {
        // Include every stream: identical position buffers can be paired with different
        // skinning/instance data. Index topology and base offsets are matched separately.
        motion_geometry = ready ? ready->buffers_hash
                                : XXH3_64bits(guest_buffers.data(),
                                              guest_buffers.size() * sizeof(AmdGpu::Buffer));
    }
    // Object motion research: vertex streams of G-buffer draws.
    if (FrameCapture::Active() && gbuffer_draw) {
        for (const auto& vb : guest_buffers) {
            char note[128];
            std::snprintf(note, sizeof(note), "\n  gb vb at %#llx size %u stride %u",
                          (unsigned long long)vb.base_address, u32(vb.GetSize()),
                          u32(vb.GetStride()));
            FrameCapture::Note(note);
        }
    }

    struct BufferRange {
        VAddr base_address;
        VAddr end_address;
        const VideoCore::Buffer* buffer;
        u64 offset;

        [[nodiscard]] size_t GetSize() const {
            return end_address - base_address;
        }
    };

    {
        const u64 vs = pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash;
        for (u32 i = 0; i < guest_buffers.size(); ++i) {
            ClothTrace(vs, 100 + i, guest_buffers[i].base_address, guest_buffers[i].GetSize(), false, false);
        }
    }

    // Build list of ranges covering the requested buffers
    VertexInputs<BufferRange> ranges{};
    VertexInputs<BufferRange> ranges_merged{};
    if (ready) {
        for (u32 i = 0; i < ready->num_ranges; ++i) {
            ranges_merged.emplace_back(ready->ranges[i].base, ready->ranges[i].end);
        }
    } else {
        for (const auto& buffer : guest_buffers) {
            if (buffer.base_address != 0 && buffer.GetSize() > 0) {
                ranges.emplace_back(buffer.base_address, buffer.base_address + buffer.GetSize());
            }
        }
    }

    // gow3 diagnostics (GOW3_TRACE_BIND_RANGE=<lo>:<hi> hex): vertex buffers in the range, by
    // vertex shader, 5 s statistics.
    {
        static const std::pair<u64, u64> trace_range = [] {
            const char* env = std::getenv("GOW3_TRACE_BIND_RANGE");
            if (!env) {
                return std::pair<u64, u64>{0, 0};
            }
            char* end = nullptr;
            const u64 lo = std::strtoull(env, &end, 16);
            const u64 hi = end && *end == ':' ? std::strtoull(end + 1, nullptr, 16) : 0;
            return std::pair<u64, u64>{lo, hi};
        }();
        if (trace_range.second > trace_range.first) {
            for (const auto& buffer : guest_buffers) {
                if (buffer.base_address < trace_range.second &&
                    buffer.base_address + buffer.GetSize() > trace_range.first) {
                    static std::mutex mutex;
                    static std::map<std::pair<u64, u64>, u32> seen;
                    static auto last = std::chrono::steady_clock::now();
                    std::scoped_lock lock{mutex};
                    ++seen[{pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash,
                            pipeline->GetStage(Shader::SwStage::Fragment).pgm_hash}];
                    if (const auto now = std::chrono::steady_clock::now(); now - last >= std::chrono::seconds{5}) {
                        last = now;
                        std::string text;
                        for (const auto& [key, n] : seen) {
                            text += fmt::format(" vs {:08x} ps {:08x} x{}", key.first, key.second, n);
                        }
                        std::printf("Vertex trace (5 s):%s (e.g. %#llx size %u stride %u)\n", text.c_str(),
                                    static_cast<unsigned long long>(buffer.base_address),
                                    unsigned(buffer.GetSize()), unsigned(buffer.GetStride()));
                        std::fflush(stdout);
                        seen.clear();
                    }
                    break;
                }
            }
        }
    }

    // Merge connecting ranges together
    if (!ranges.empty()) {
        std::ranges::sort(ranges, [](const BufferRange& lhv, const BufferRange& rhv) {
            return lhv.base_address < rhv.base_address;
        });
        ranges_merged.emplace_back(ranges[0]);
        for (auto range : ranges) {
            auto& prev_range = ranges_merged.back();
            if (prev_range.end_address < range.base_address) {
                ranges_merged.emplace_back(range);
            } else {
                prev_range.end_address = std::max(prev_range.end_address, range.end_address);
            }
        }
    }

    // Map buffers for merged ranges
    for (auto& range : ranges_merged) {
        const u64 size = memory->ClampRangeSize(range.base_address, range.GetSize());
        std::tie(range.buffer, range.offset) =
            buffer_cache.ObtainBuffer(range.base_address, size, false);
        needs_barrier |= runtime.IsBufferAccessed(range.buffer, range.offset, size);
    }

    // Bind vertex buffers
    auto& host_buffers = v.host_buffers;
    auto& host_offsets = v.host_offsets;
    auto& host_sizes = v.host_sizes;
    auto& host_strides = v.host_strides;
    u32 stream_with_memory = 0;
    for (const auto& buffer : guest_buffers) {
        if (buffer.base_address != 0 && buffer.GetSize() > 0) {
            const auto host_buffer_info =
                ready ? ranges_merged.begin() + ready->range_index[stream_with_memory++]
                      : std::ranges::find_if(ranges_merged, [&](const BufferRange& range) {
                            return buffer.base_address >= range.base_address &&
                                   buffer.base_address < range.end_address;
                        });
            ASSERT(host_buffer_info != ranges_merged.cend());
            host_buffers.emplace_back(host_buffer_info->buffer->Handle());
            host_offsets.push_back(host_buffer_info->offset + buffer.base_address -
                                   host_buffer_info->base_address);
        } else {
            host_buffers.emplace_back(VK_NULL_HANDLE);
            host_offsets.push_back(0);
        }
        host_sizes.push_back(buffer.GetSize());
        host_strides.push_back(buffer.GetStride());
    }

    v.num_buffers = static_cast<u32>(guest_buffers.size());
}

void Rasterizer::BindIndexBuffer(u32 index_offset) {
    ResolveIndexBuffer(index_offset);
    EmitIndexBuffer();
}

void Rasterizer::EmitIndexBuffer() {
    scheduler.Record([handle = index_bind.handle, offset = index_bind.offset,
                      type = index_bind.type](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindIndexBuffer(handle, offset, type);
    });
}

void Rasterizer::ResolveIndexBuffer(u32 index_offset) {
    const auto& regs = Regs();

    // Figure out index type and size.
    const bool is_index16 = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16;
    const vk::IndexType index_type = is_index16 ? vk::IndexType::eUint16 : vk::IndexType::eUint32;
    const u32 index_size = is_index16 ? sizeof(u16) : sizeof(u32);
    const VAddr index_address =
        regs.index_base_address.Address<VAddr>() + index_offset * index_size;

    // Bind index buffer.
    const u32 index_buffer_size = regs.num_indices * index_size;
    const auto [buffer, offset] =
        buffer_cache.ObtainBuffer(index_address, index_buffer_size, false);
    needs_barrier |= runtime.IsBufferAccessed(buffer, offset, index_buffer_size);
    index_bind = {buffer->Handle(), offset, index_type};
}

void Rasterizer::ResetBindings(bool is_compute) {
    for (auto& image_id : bound_images) {
        texture_cache.GetImage(image_id).binding = {};
    }
    // gow3 17 Oct 2026: overlapping or adjacent ranges of one buffer with the same access are
    // tracked as one (the same tracker state, fewer inserts): Uncharted 3's crowd skinning shader
    // binds each output array through views at +0/+12/+20, ~1,500 dispatches a frame. Compute
    // only: sorting every draw's bindings cost Uncharted 1 and 2 (draw-heavy) 3-4%.
    if (is_compute && bound_buffers.size() > 1) {
        std::sort(bound_buffers.begin(), bound_buffers.end(), [](const BoundBuffer& a, const BoundBuffer& b) {
            return std::tie(a.buffer, a.is_written, a.offset) < std::tie(b.buffer, b.is_written, b.offset);
        });
        size_t out = 0;
        for (size_t i = 1; i < bound_buffers.size(); ++i) {
            auto& last = bound_buffers[out];
            const auto& next = bound_buffers[i];
            if (next.buffer == last.buffer && next.is_written == last.is_written &&
                next.offset <= last.offset + last.size) {
                const u64 end = std::max<u64>(last.offset + last.size, next.offset + next.size);
                last.size = static_cast<u32>(end - last.offset);
            } else {
                bound_buffers[++out] = next;
            }
        }
        bound_buffers.resize(out + 1);
    }
    for (const auto [buffer, offset, size, is_written] : bound_buffers) {
        const auto dst_stage = is_compute ? vk::PipelineStageFlagBits2::eComputeShader
                                          : vk::PipelineStageFlagBits2::eAllGraphics;
        const auto write_flag =
            is_written ? vk::AccessFlagBits2::eShaderWrite : vk::AccessFlagBits2::eNone;
        runtime.AccessBuffer(buffer, offset, size, dst_stage,
                             vk::AccessFlagBits2::eShaderRead | write_flag);
    }
    for (const auto* arena : dma_read_arenas) {
        runtime.AccessBuffer(arena, 0, arena->SizeBytes(),
                             is_compute ? vk::PipelineStageFlagBits2::eComputeShader
                                        : vk::PipelineStageFlagBits2::eAllGraphics,
                             vk::AccessFlagBits2::eShaderRead);
    }
    dma_read_arenas.clear();
    bound_images.clear();
    bound_buffers.clear();
    needs_barrier = false;
}

bool Rasterizer::IsComputeMetaClear(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Most of the time when a metadata is updated with a shader it gets cleared. It means
    // we can skip the whole dispatch and update the tracked state instead. Also, it is not
    // intended to be consumed and in such rare cases (e.g. HTile introspection, CRAA) we
    // will need its full emulation anyways.
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);

    // gow3: the cheap descriptor tests come before reading the V# (GetSharp: user data or the
    // flattened SRT, and for the effect shaders a mapped-memory query) on every dispatch; a
    // shader writing no guest buffer cannot be a metadata clear.
    bool writes_buffer = false;
    // Assume if a shader reads metadata, it is a copy shader.
    for (const auto& desc : info.buffers) {
        if (desc.IsSpecial()) {
            continue;
        }
        if (desc.is_written) {
            writes_buffer = true;
            continue;
        }
        const VAddr address = desc.GetSharp(info).base_address;
        if (texture_cache.IsMeta(address)) {
            return false;
        }
    }

    // Metadata surfaces are tiled and thus need address calculation to be written properly.
    // If a shader wants to encode HTILE, for example, from a depth image it will have to compute
    // proper tile address from dispatch invocation id. This address calculation contains an xor
    // operation so use it as a heuristic for metadata writes that are probably not clears.
    if (!info.has_bitwise_xor && writes_buffer) {
        // Assume if a shader writes metadata without address calculation, it is a clear shader.
        for (const auto& desc : info.buffers) {
            if (desc.IsSpecial() || !desc.is_written) {
                continue;
            }
            const VAddr address = desc.GetSharp(info).base_address;
            if (texture_cache.ClearMeta(address)) {
                // Assume all slices were updates
                LOG_TRACE(Render_Vulkan, "Metadata update skipped");
                return true;
            }
        }
    }
    return false;
}

bool Rasterizer::IsComputeImageCopy(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Ensure shader only has 2 bound buffers
    const auto& cs_pgm = CsRegs();
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() != 2 || !info.images.empty()) {
        return false;
    }

    // Those 2 buffers must both be formatted. One must be source and another destination.
    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (!desc0.is_formatted || !desc1.is_formatted || desc0.is_written == desc1.is_written) {
        return false;
    }

    // Buffers must have the same size and each thread of the dispatch must copy 1 dword of data
    const AmdGpu::Buffer buf0 = desc0.GetSharp(info);
    const AmdGpu::Buffer buf1 = desc1.GetSharp(info);
    if (buf0.GetSize() != buf1.GetSize() || cs_pgm.dim_x != (buf0.GetSize() / 256)) {
        return false;
    }

    // Find images the buffer alias
    const auto image0_id = texture_cache.FindImageFromRange(buf0.base_address, buf0.GetSize());
    if (!image0_id) {
        return false;
    }
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return false;
    }

    // Image copy must be valid
    VideoCore::Image& image0 = texture_cache.GetImage(image0_id);
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image0.info.guest_size != image1.info.guest_size ||
        image0.info.pitch != image1.info.pitch || image0.info.guest_size != buf0.GetSize() ||
        image0.info.num_bits != image1.info.num_bits) {
        return false;
    }

    // Perform image copy
    VideoCore::Image& src_image = desc0.is_written ? image1 : image0;
    VideoCore::Image& dst_image = desc0.is_written ? image0 : image1;
    runtime.CopyColorAndDepth(&src_image, &dst_image);
    return true;
}

bool Rasterizer::IsComputeImageClear(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Ensure shader only has 2 bound buffers
    const auto& cs_pgm = CsRegs();
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() != 2 || !info.images.empty()) {
        return false;
    }

    // From those 2 buffers, first must hold the clear vector and second the image being cleared
    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (desc0.is_formatted || !desc1.is_formatted || desc0.is_written || !desc1.is_written) {
        return false;
    }

    // First buffer must have size of vec4 and second the size of a single layer
    const AmdGpu::Buffer buf0 = desc0.GetSharp(info);
    const AmdGpu::Buffer buf1 = desc1.GetSharp(info);
    const u32 buf1_bpp = AmdGpu::NumBitsPerBlock(buf1.GetDataFmt());
    if (buf0.GetSize() != 16 || (cs_pgm.dim_x * 128ULL * (buf1_bpp / 8)) != buf1.GetSize()) {
        return false;
    }

    // Find image the buffer alias
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return false;
    }

    // Image clear must be valid
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image1.info.guest_size != buf1.GetSize() || image1.info.num_bits != buf1_bpp ||
        image1.info.props.is_depth) {
        return false;
    }

    // Perform image clear
    const float* values = reinterpret_cast<float*>(buf0.base_address);
    const vk::ClearValue clear = {
        .color = {.float32 = std::array<float, 4>{values[0], values[1], values[2], values[3]}},
    };
    const VideoCore::SubresourceRange range = {
        .base =
            {
                .level = 0,
                .layer = 0,
            },
        .extent = image1.info.resources,
    };
    runtime.ClearImage(&image1, range, clear);
    return true;
}

static bool TracedShader(u64 hash);

// gow3 diagnostics (GOW3_CLOTH_CHECK=1): the CPU-written inputs of Uncharted 3's cloth compute
// shaders (cs 1c957b01, 809864d3; Sully's shirt flickers as exploding triangles above 30 FPS,
// 16 Oct) are scanned at each dispatch for non-finite or huge floats; hits are printed with the
// frame number, plus a 5 s summary of dispatches and buffers checked.
static void CheckClothInputs(Core::MemoryManager* memory, VideoCore::BufferCache& cache, u64 hash,
                             u32 slot, VAddr address, u64 size, bool ring) {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_CLOTH_CHECK");
        return env && env[0] == '1';
    }();
    if (!enabled || (hash != 0x1c957b01 && hash != 0x809864d3) || !address || !size) {
        return;
    }
    static std::mutex mutex;
    static u64 checked = 0, bad_buffers = 0, gpu_written = 0;
    static int printed = 0;
    static auto last = std::chrono::steady_clock::now();
    std::scoped_lock lock{mutex};
    const u64 frame = Gow3Stats::gpu_frames.load(std::memory_order_relaxed);
    if (!ring && cache.IsRegionGpuModified(address, size)) {
        ++gpu_written; // GPU data: guest memory does not hold it
    } else if (size <= 64_KB && memory->IsValidGpuMapping(address, size)) {
        ++checked;
        static std::vector<float> values;
        values.resize(size / 4);
        memory->CopySparseMemory(address, reinterpret_cast<u8*>(values.data()), values.size() * 4);
        u32 bad = 0, first = 0;
        for (u32 i = 0; i < values.size(); ++i) {
            const float v = values[i];
            const u32 bits = std::bit_cast<u32>(v);
            // Integers stored as floats look like denormals or tiny values: only NaN/inf and
            // magnitudes over 1e6 that are not plain integers count.
            const bool nonfinite = ((bits >> 23) & 0xff) == 0xff;
            const bool huge = std::isfinite(v) && std::abs(v) > 1e6f && (bits & 0xff000000u) != 0;
            if (nonfinite || huge) {
                if (!bad) {
                    first = i;
                }
                ++bad;
            }
        }
        if (bad) {
            ++bad_buffers;
            if (printed < 200) {
                ++printed;
                std::printf("Cloth check: frame %llu shader %08llx slot %u%s at %#llx size %llu: %u bad "
                            "floats, first at +%u = %g (%08x)\n",
                            static_cast<unsigned long long>(frame), static_cast<unsigned long long>(hash), slot,
                            ring ? " (ring)" : "", static_cast<unsigned long long>(address),
                            static_cast<unsigned long long>(size), bad, first * 4, values[first],
                            std::bit_cast<u32>(values[first]));
            }
        }
    }
    if (const auto now = std::chrono::steady_clock::now(); now - last >= std::chrono::seconds{5}) {
        last = now;
        std::printf("Cloth check (5 s): %llu input buffers checked, %llu with bad floats, %llu GPU-written\n",
                    static_cast<unsigned long long>(checked), static_cast<unsigned long long>(bad_buffers),
                    static_cast<unsigned long long>(gpu_written));
        std::fflush(stdout);
        checked = bad_buffers = gpu_written = 0;
    }
}

// gow3 16 Oct: Uncharted 2's aim depth of field focuses on the depth under the crosshair: ps
// 9f801363 samples a 24-pixel box of the scene depth at uv = c[6] * (ndc * c[2] + 0.5) + c[0]
// (and 1 - c[1] + ... for v), c = 8 floats the game writes per frame. c[0], c[1] are the crosshair
// in pixels of the HUD's 1920x1080 space times 1 / render size (c[6], c[7]): with our render
// resolution above 1080p the box left the crosshair (4K: uv 0.25 instead of 0.5, the depth of the
// wall beside Nate) and everything behind the target was blurred while aiming. The centre is
// corrected to the HUD space for every render size (no change at 1080p). GOW3_AIM_FOCUS_FIX=0: off.
static void FixAimFocusCentre(Core::MemoryManager* memory, VideoCore::BufferCache& buffer_cache,
                              VAddr address) {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_AIM_FOCUS_FIX");
        return !(env && env[0] == '0');
    }();
    // gow3 20 Oct 2026: what the fix did per 5 s (calls, skips by reason, corrections, writes
    // through to the GPU's copy), printed while the shader runs.
    static std::atomic<u32> n_calls{0}, n_unmapped{0}, n_odd{0}, n_native{0}, n_same{0}, n_fixed{0},
        n_through{0};
    static std::atomic<s64> n_last{0};
    ++n_calls;
    if (const s64 now = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
        now - n_last.load() >= 5) {
        n_last = now;
        std::printf("Aim focus (5 s): %u binds; skipped %u unmapped, %u not the box constants, %u at "
                    "1080p, %u already corrected; %u corrected (%u through to the GPU copy)\n",
                    n_calls.exchange(0), n_unmapped.exchange(0), n_odd.exchange(0), n_native.exchange(0),
                    n_same.exchange(0), n_fixed.exchange(0), n_through.exchange(0));
        std::fflush(stdout);
    }
    if (!enabled || !memory->IsValidGpuMapping(address, 32)) {
        ++n_unmapped;
        return;
    }
    float c[8];
    memory->CopySparseMemory(address, reinterpret_cast<u8*>(c), sizeof(c));
    const float inv_width = c[6], inv_height = c[7];
    if (!(inv_width > 0.0f && inv_width < 1.0f && inv_height > 0.0f && inv_height < 1.0f)) {
        ++n_odd;
        return;
    }
    const float x_scale = 1920.0f * inv_width, y_scale = 1080.0f * inv_height;
    if (std::abs(x_scale - 1.0f) < 1e-4f && std::abs(y_scale - 1.0f) < 1e-4f) {
        ++n_native;
        return; // 1080p: already right
    }
    // The block may be bound by more than one draw: values corrected once stay as they are.
    static std::mutex mutex;
    static std::unordered_map<VAddr, std::pair<float, float>> corrected;
    std::scoped_lock lock{mutex};
    if (const auto it = corrected.find(address);
        it != corrected.end() && it->second == std::pair{c[0], c[1]}) {
        ++n_same;
        return;
    }
    ++n_fixed;
    const float fixed[2] = {c[0] / x_scale, c[1] / y_scale};
    // gow3 20 Oct 2026: written through to the GPU's copy as well (BufferCache::WriteThrough).
    // A write to the backing view alone is not seen by the page tracking: when another draw had
    // already uploaded the block in that frame, the GPU kept the uncorrected centre. The user
    // saw the aim blur again in Uncharted 2 at 4K: the focus read back alternated between the
    // target's 2.1 m and 5.64 m, the wall beside Nate.
    if (buffer_cache.WriteThrough(address, fixed, sizeof(fixed))) {
        ++n_through;
    } else {
        memory->TryWriteBacking(reinterpret_cast<void*>(address), fixed, sizeof(fixed));
    }
    if (corrected.size() >= 512) {
        corrected.clear();
    }
    corrected[address] = {fixed[0], fixed[1]};
    static bool reported = false;
    if (!reported) {
        reported = true;
        std::printf("Aim focus: crosshair box centre %.4f,%.4f -> %.4f,%.4f (render %ux%u)\n", c[0], c[1],
                    fixed[0], fixed[1], u32(1.0f / inv_width + 0.5f), u32(1.0f / inv_height + 0.5f));
        std::fflush(stdout);
    }
}

void Rasterizer::BindBuffers(const Shader::Info& stage, const PreparedStage* prepared,
                             Shader::Backend::Bindings& binding,
                             Shader::PushData& push_data, u32& write_index) {
    const u64 alignment = instance.StorageMinAlignment();
    const bool compute_stage = stage.hw_stage == Shader::HwStage::Compute;
    ClothTrace(stage.pgm_hash, ~0u, 0, 0, false, compute_stage);
    for (u32 buffer_index = 0; buffer_index < stage.buffers.size(); ++buffer_index) {
        const auto& desc = stage.buffers[buffer_index];
        const RingBinding* ring = num_ring_stages ? FindRingBinding(stage, buffer_index) : nullptr;
        if (!desc.IsSpecial() && !desc.is_written &&
            (stage.pgm_hash == 0x1c957b01 || stage.pgm_hash == 0x809864d3)) {
            if (ring) {
                CheckClothInputs(memory, buffer_cache, stage.pgm_hash, buffer_index, ring->address,
                                 ring->size, true);
            } else {
                const auto vsharp = prepared ? prepared->buffer_sharps[buffer_index] : desc.GetSharp(stage);
                CheckClothInputs(memory, buffer_cache, stage.pgm_hash, buffer_index, vsharp.base_address,
                                 vsharp.GetSize(), false);
            }
        }
        if (TracedShader(stage.pgm_hash) && !desc.IsSpecial()) {
            static std::atomic<int> traced{0};
            if (traced.fetch_add(1) < 40) {
                VAddr address = 0;
                u64 size = 0;
                if (ring) {
                    address = ring->address;
                    size = ring->size;
                } else {
                    const auto vsharp = prepared ? prepared->buffer_sharps[buffer_index] : desc.GetSharp(stage);
                    address = vsharp.base_address;
                    size = vsharp.GetSize();
                }
                std::string text = fmt::format("PS trace buffer: shader {:08x} slot {} at {:#x} size {}{}:",
                                               stage.pgm_hash, buffer_index, address, size, ring ? " (ring)" : "");
                if (address && memory->IsValidGpuMapping(address, 0)) {
                    float values[16]{};
                    memory->CopySparseMemory(address, reinterpret_cast<u8*>(values),
                                             std::min<u64>(size, sizeof(values)));
                    for (u32 i = 0; i < std::min<u64>(size / 4, 16); ++i) {
                        text += fmt::format(" {}", values[i]);
                    }
                }
                std::printf("%s\n", text.c_str());
                std::fflush(stdout);
            }
        }
        if (ring) {
            // Copied by the GPU command thread into the constant ring (read-only).
            if (FrameCapture::Active() && memory->IsValidGpuMapping(ring->address, 0)) {
                FrameCapture::Buffer(stage.pgm_hash, buffer_index, ring->address,
                                     reinterpret_cast<const void*>(ring->address), ring->size);
            }
            if (!desc.IsSpecial()) {
                if (ring->size == 864 && gbuffer_draw &&
                    memory->IsValidGpuMapping(ring->address, 0)) {
                    camera_motion->OnConstants(reinterpret_cast<const float*>(ring->address));
                }
                push_data.AddOffset(binding.buffer, 0);
            }
            buffer_infos.emplace_back(constant_ring->Handle(), ring->offset, ring->size);
        } else if (desc.IsSpecial()) {
            if (desc.buffer_type == Shader::BufferType::GdsBuffer) {
                const auto* gds_buf = buffer_cache.GetGdsBuffer();
                buffer_infos.emplace_back(gds_buf->Handle(), 0, gds_buf->SizeBytes());
                needs_barrier |= runtime.IsBufferAccessed(gds_buf, 0, gds_buf->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::Flatbuf) {
                auto& vk_buffer = buffer_cache.GetStreamBuffer();
                const u32 ubo_size = stage.FlatUserData().size() * sizeof(u32);
                if (FrameCapture::Active()) {
                    FrameCapture::Buffer(stage.pgm_hash, binding.buffer, 0,
                                         stage.FlatUserData().data(), ubo_size);
                }
                const u64 offset =
                    vk_buffer.Copy(stage.FlatUserData().data(), ubo_size, alignment);
                if (LateConstantsEnabled()) {
                    NoteLateFlatCopy(vk_buffer, offset, stage, ubo_size, 0);
                }
                buffer_infos.emplace_back(vk_buffer.Handle(), offset, ubo_size);
            } else if (desc.buffer_type == Shader::BufferType::ClipPlanes) {
                // Permutations compiled without enabled planes never read the buffer, so the
                // declared binding is satisfied with a null descriptor instead of a copy.
                if (Regs().clipper_control.user_clip_plane_enable == 0) {
                    buffer_infos.emplace_back(VK_NULL_HANDLE, 0, VK_WHOLE_SIZE);
                } else {
                    auto& vk_buffer = buffer_cache.GetStreamBuffer();
                    std::array<float, AmdGpu::NUM_CLIP_PLANES * 4> planes{};
                    for (u32 i = 0; i < AmdGpu::NUM_CLIP_PLANES; ++i) {
                        const auto& plane = Regs().clip_user_data[i];
                        planes[i * 4 + 0] = std::bit_cast<float>(plane.data_x);
                        planes[i * 4 + 1] = std::bit_cast<float>(plane.data_y);
                        planes[i * 4 + 2] = std::bit_cast<float>(plane.data_z);
                        planes[i * 4 + 3] = std::bit_cast<float>(plane.data_w);
                    }
                    const u32 ubo_size = static_cast<u32>(sizeof(planes));
                    const u64 offset = vk_buffer.Copy(planes.data(), ubo_size, alignment);
                    buffer_infos.emplace_back(vk_buffer.Handle(), offset, ubo_size);
                }
            } else if (desc.buffer_type == Shader::BufferType::BdaPagetable) {
                const auto* bda_buffer = buffer_cache.GetBdaPageTableBuffer();
                buffer_infos.emplace_back(bda_buffer->Handle(), 0, bda_buffer->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::FaultBuffer) {
                const auto* fault_buffer = buffer_cache.GetFaultBuffer();
                buffer_infos.emplace_back(fault_buffer->Handle(), 0, fault_buffer->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::SharedMemory) {
                auto& lds_buffer = buffer_cache.GetStreamBuffer();
                const auto& cs_program = CsRegs();
                const auto lds_size = cs_program.SharedMemSize() * cs_program.NumWorkgroups();
                const auto [data, offset] = lds_buffer.Map(lds_size, alignment);
                std::memset(data, 0, lds_size);
                lds_buffer.Commit();
                buffer_infos.emplace_back(lds_buffer.Handle(), offset, lds_size);
            } else {
                UNREACHABLE_MSG("Unexpected buffer type {}", u32(desc.buffer_type));
            }
        } else {
            const auto vsharp =
                prepared ? prepared->buffer_sharps[buffer_index] : desc.GetSharp(stage);
            if (vsharp.GetSize() == 864 && gbuffer_draw &&
                memory->IsValidGpuMapping(vsharp.base_address, 0)) {
                camera_motion->OnConstants(reinterpret_cast<const float*>(vsharp.base_address));
            }
            // Object motion research: the vertex shader buffers of G-buffer draws.
            if (FrameCapture::Active() && gbuffer_draw && stage.sw_stage == Shader::SwStage::Vertex &&
                vsharp.base_address != 0 && memory->IsValidGpuMapping(vsharp.base_address, 0)) {
                const auto* f = reinterpret_cast<const float*>(vsharp.base_address);
                char note[192];
                std::snprintf(note, sizeof(note),
                              "\n  gb vs %08x slot %u at %#llx size %u stride %u: %g %g %g %g",
                              u32(stage.pgm_hash), binding.buffer,
                              (unsigned long long)vsharp.base_address, u32(vsharp.GetSize()),
                              u32(vsharp.GetStride()), f[0], f[1], f[2], f[3]);
                FrameCapture::Note(note);
            }
            if (FrameCapture::Active() && vsharp.base_address != 0 && vsharp.GetSize() != 0 &&
                memory->IsValidGpuMapping(vsharp.base_address, 0)) {
                FrameCapture::Buffer(stage.pgm_hash, binding.buffer, vsharp.base_address,
                                     reinterpret_cast<const void*>(vsharp.base_address),
                                     memory->ClampRangeSize(vsharp.base_address, vsharp.GetSize()));
            }
            // gow3: garbage V#s. Uncharted 2's treasure effect shaders (cs 7d7e7206, 8f067540)
            // fetch V#s from table slots that hold floats for that effect (CPU-written: the
            // branches using them are not taken). Bound, they span random guest memory: written
            // ones made gigabytes of game data "GPU-modified" (stale GPU copies written back over
            // it: the game's fg-draw-mgr assert), and the arena over them ran out of device
            // memory. A V# reaching past mapped memory, other than an unbounded raw buffer
            // (num_records ~0), or spanning 1 GiB is bound as null. Normal play has none.
            const bool unbound = vsharp.base_address == 0 || vsharp.GetSize() == 0;
            const u64 mapped_size =
                unbound ? 0 : memory->ClampRangeSize(vsharp.base_address, vsharp.GetSize());
            // gow3 16 Oct: the treasure effect's float slots can also make a V# that lands in mapped
            // memory: after 90 minutes one with an unaligned base, a stride of ~15 KB and ~1e9
            // records was bound, the GPU hung (OBS's own device was removed with DEVICE_HUNG at the
            // same moment) and the game stopped with "Device lost during submit". For these two
            // shaders an unaligned base, a stride of 8 KiB or more or over 64 MiB is garbage too.
            const bool treasure_effect = Shader::IsEffectShader(stage.pgm_hash);
            const bool garbage =
                !unbound && (mapped_size >= 1_GB ||
                             (mapped_size != vsharp.GetSize() && vsharp.num_records < 0xFFFF0000u) ||
                             (treasure_effect && ((vsharp.base_address & 3) != 0 || vsharp.stride >= 8192 ||
                                                  vsharp.GetSize() > 64_MB)));
            if (garbage) {
                static u32 garbage_reports = 0;
                if (garbage_reports < 32) {
                    ++garbage_reports;
                    LOG_ERROR(Render, "Ignoring a garbage V# for stage {:#x}: {:#x}, {} of {} bytes "
                              "mapped, num_records {}, stride {}", stage.pgm_hash,
                              vsharp.base_address, mapped_size, vsharp.GetSize(),
                              vsharp.num_records, vsharp.stride);
                }
                if (!prepared && (desc.sharp_fetch.load_mask & 1)) {
                    DescribeSharpSource(stage, desc.sharp_fetch.offsets[0], "garbage");
                }
            }
            if (unbound || garbage) {
                buffer_infos.emplace_back(VK_NULL_HANDLE, 0, VK_WHOLE_SIZE);
            } else {
                const u64 size = memory->ClampRangeSize(vsharp.base_address, vsharp.GetSize());
                // gow3 diagnostics (GOW3_TRACE_BIND_RANGE=<lo>:<hi> hex): bindings that overlap
                // the range, by shader and slot, 5 s statistics.
                {
                    static const std::pair<u64, u64> trace_range = [] {
                        const char* env = std::getenv("GOW3_TRACE_BIND_RANGE");
                        if (!env) {
                            return std::pair<u64, u64>{0, 0};
                        }
                        char* end = nullptr;
                        const u64 lo = std::strtoull(env, &end, 16);
                        const u64 hi = end && *end == ':' ? std::strtoull(end + 1, nullptr, 16) : 0;
                        return std::pair<u64, u64>{lo, hi};
                    }();
                    if (trace_range.second > trace_range.first &&
                        vsharp.base_address < trace_range.second &&
                        vsharp.base_address + size > trace_range.first) {
                        static std::mutex mutex;
                        static std::map<std::tuple<u64, u32, bool>, u32> seen;
                        static auto last = std::chrono::steady_clock::now();
                        std::scoped_lock lock{mutex};
                        ++seen[{stage.pgm_hash, binding.buffer, bool(desc.is_written)}];
                        if (const auto now = std::chrono::steady_clock::now(); now - last >= std::chrono::seconds{5}) {
                            last = now;
                            std::string text;
                            for (const auto& [key, n] : seen) {
                                text += fmt::format(" {:08x}/slot{}{}x{}", std::get<0>(key), std::get<1>(key),
                                                    std::get<2>(key) ? "(write)" : "", n);
                            }
                            std::printf("Bind trace (5 s):%s\n", text.c_str());
                            std::fflush(stdout);
                            seen.clear();
                        }
                    }
                }
                if (size != vsharp.GetSize()) {
                    LOG_ERROR(Render, "Clamped size from {} to {} for stage {:#x}",
                              vsharp.GetSize(), size, stage.pgm_hash);
                    if (!prepared && (desc.sharp_fetch.load_mask & 1)) {
                        DescribeSharpSource(stage, desc.sharp_fetch.offsets[0], "size");
                    }
                }
                if (stage.pgm_hash == 0x9f801363 && !desc.is_written && size >= 32) {
                    FixAimFocusCentre(memory, buffer_cache, vsharp.base_address);
                }
                static const bool cloth_peek = [] {
                    const char* env = std::getenv("GOW3_CLOTH_PEEK");
                    return env && env[0] == '1';
                }();

                const bool reads_new_cloth = ClothTrace(stage.pgm_hash, buffer_index, vsharp.base_address,
                                                        size, desc.is_written, compute_stage);
                VideoCore::g_stream_stats_key = stage.pgm_hash << 8 | (buffer_index & 0xff);
                VideoCore::g_stream_stats_used =
                    desc.access_extent != 0 && desc.access_extent != ~0u && vsharp.stride != 0 &&
                            vsharp.num_records != 0 && !vsharp.swizzle_enable
                        ? u64(vsharp.num_records - 1) * vsharp.stride + desc.access_extent
                        : 0;
                const auto [buffer, offset] = buffer_cache.ObtainBuffer(
                    vsharp.base_address, size, desc.is_written, desc.is_formatted);
                if (cloth_peek && (stage.pgm_hash == 0x1c957b01 || stage.pgm_hash == 0x809864d3) &&
                    size <= 1_MB) {
                    cloth_peeks.push_back({vsharp.base_address, size, stage.pgm_hash, buffer_index,
                                           desc.is_written, buffer, offset});
                }
                const u64 offset_aligned = Common::AlignDown(offset, alignment);
                const u64 adjust = offset - offset_aligned;
                if (adjust % 4 != 0) {
                    LOG_WARNING(Render_Vulkan, "Buffer binding in shader {:#x} isn't dword aligned",
                                stage.pgm_hash);
                }
                push_data.AddOffset(binding.buffer, adjust);
                buffer_infos.emplace_back(buffer->Handle(), offset_aligned, size + adjust);
                // gow3 17 Oct 2026: a strided buffer covers (records - 1) * stride plus the bytes
                // the shader reaches in a record, not a whole stride more. Uncharted's effect
                // shader (cs 8f067540, ~1500 dispatches a frame in crowds) writes one record array
                // per dispatch through views at +0/+12/+20: the +20 view's V# range ran 20 bytes
                // into the next dispatch's array, so every dispatch waited for the one before
                // (~10 ms of GPU time a frame). Needs the tracker's gap merge (BarrierTracker::
                // InsertRange): without those barriers its tree was never emptied (Uncharted 2's
                // museum 79 -> 41 FPS); with it 79 -> 85. GOW3_EXACT_STRIDED_RANGES=0: whole ranges.
                u64 tracked_size = size;
                {
                    static const bool exact = [] {
                        const char* env = std::getenv("GOW3_EXACT_STRIDED_RANGES");
                        return !(env && env[0] == '0');
                    }();
                    const u32 extent = desc.access_extent;
                    if (exact && extent != 0 && extent != ~0u && vsharp.stride != 0 &&
                        extent < vsharp.stride && vsharp.num_records != 0 &&
                        !vsharp.swizzle_enable && !vsharp.add_tid_enable) {
                        tracked_size = std::min<u64>(
                            size, u64(vsharp.num_records - 1) * vsharp.stride + extent);
                    }
                }
                // gow3 17 Oct 2026: static ranges from the arena (never GPU-written mesh data,
                // ~10k binds a frame in Uncharted 3's crowds) are tracked in 64 KiB pieces: far
                // fewer barrier tracker nodes; a wider read range only adds barriers.
                // GOW3_STATIC_TRACK_KB (default 64, 0 = exact).
                static const u64 static_track = [] {
                    const char* env = std::getenv("GOW3_STATIC_TRACK_KB");
                    return (env ? std::strtoull(env, nullptr, 10) : 64ull) * 1024;
                }();
                if (VideoCore::g_obtained_static && static_track && !desc.is_written) {
                    const u64 lo = offset & ~(static_track - 1);
                    const u64 hi = Common::AlignUp(offset + tracked_size, static_track);
                    bound_buffers.emplace_back(buffer, lo, u32(std::min<u64>(hi - lo, buffer->SizeBytes() - lo)),
                                               false);
                } else {
                    bound_buffers.emplace_back(buffer, offset, tracked_size, desc.is_written);
                }
                if (desc.is_written) {
                    Gow3WatchRange::Note("storage", stage.pgm_hash, binding.buffer,
                                         vsharp.base_address, size);
                    VideoCore::NoteGpuWriter(vsharp.base_address, size, stage.pgm_hash);
                    if (IsEopReadbackShader(stage.pgm_hash) &&
                        eop_readbacks.size() + eop_open_readbacks.size() < 256) {
                        // The whole bound buffer counts as GPU-written (up to 9 MiB for
                        // 6a6422a9); read back only what this dispatch writes: dword i for
                        // i < min(groups * 64, s6, num_records) (vk_shader_hle.cpp). Only its
                        // small copies: the game reads a value in 1 KiB blocks (~160 per second),
                        // while its two large copies per frame (~0.2 and ~2.4 MB, never read in
                        // a precise-readback run) were 300 MB/s of readback.
                        u64 readback_size = size;
                        if (stage.pgm_hash == 0x6a6422a9) {
                            const auto ud = stage.UserData();
                            readback_size = 0;
                            if (vsharp.stride == 4 && ud.size() > 6) {
                                const u64 count = std::min<u64>(
                                    {u64(CsRegs().dim_x) * 64, ud[6], vsharp.num_records});
                                readback_size = std::min<u64>(size, count * 4);
                            }
                            if (readback_size > 64_KB) {
                                readback_size = 0;
                            }
                        }
                        static const bool open_raycasts = [] {
                            const char* open = std::getenv("GOW3_OPEN_RAYCAST_READBACKS");
                            const char* defer = std::getenv("GOW3_DEFER_RAYCAST_FENCES");
                            return open && open[0] == '1' && !(defer && defer[0] == '0');
                        }();
                        if (readback_size) {
                            (open_raycasts && stage.pgm_hash != 0x6a6422a9 ? eop_open_readbacks
                                                                          : eop_readbacks)
                                .emplace_back(vsharp.base_address, readback_size);
                        }
                    }
                    // Small writes (constants) and writes into windows where the CPU writes into
                    // GPU data (each such CPU write otherwise downloads with a full GPU wait).
                    static const u64 readback_skip = [] {
                        const char* env = std::getenv("GOW3_READBACK_SKIP");
                        return env ? std::strtoull(env, nullptr, 16) : 0ULL;
                    }();
                    if (size <= 4096 && Gow3Stats::enabled) {
                        // gow3: a shader whose first 3 writes were printed is remembered lock-free
                        // (direct-mapped by hash; a collision only takes the lock again), so the
                        // mutex and map are not taken per written binding of every dispatch.
                        static std::array<std::atomic<u64>, 256> writers_done{};
                        auto& writer_done =
                            writers_done[(stage.pgm_hash * 0x9E3779B97F4A7C15ull) >> 56];
                        if (writer_done.load(std::memory_order_relaxed) != ~stage.pgm_hash) {
                            static std::mutex writers_mutex;
                            static std::unordered_map<u64, u32> writers;
                            std::scoped_lock lock{writers_mutex};
                            const u32 written = writers[stage.pgm_hash]++;
                            if (written < 3) {
                                std::printf("Small GPU write: shader %#llx slot %u at %#llx size %llu\n",
                                            static_cast<unsigned long long>(stage.pgm_hash),
                                            binding.buffer,
                                            static_cast<unsigned long long>(vsharp.base_address),
                                            static_cast<unsigned long long>(size));
                            }
                            if (written >= 2) {
                                writer_done.store(~stage.pgm_hash, std::memory_order_relaxed);
                            }
                        }
                    }
                    if (sync_gpu_constants && small_gpu_writes.size() < 16384 &&
                        stage.pgm_hash != readback_skip) {
                        if (size <= 4096) {
                            small_gpu_writes.emplace_back(vsharp.base_address, size);
                            VideoCore::NoteReadbackKind(vsharp.base_address, size,
                                                        VideoCore::ReadbackSmallWrite);
                        } else if (buffer_cache.IsCpuWriteHot(vsharp.base_address, size)) {
                            // Only the hot 1 MiB windows of a large write.
                            const VAddr end = vsharp.base_address + size;
                            for (VAddr window = Common::AlignDown(vsharp.base_address, 1_MB);
                                 window < end; window += 1_MB) {
                                if (buffer_cache.IsCpuWriteHot(window, 1_MB)) {
                                    const VAddr lo = std::max(window, VAddr(vsharp.base_address));
                                    const VAddr hi = std::min(window + 1_MB, end);
                                    small_gpu_writes.emplace_back(lo, hi - lo);
                                    VideoCore::NoteReadbackKind(lo, hi - lo,
                                                                VideoCore::ReadbackHotWindow);
                                }
                            }
                        }
                    }
                    // Raw storage-buffer writes can also make an aliased cached image stale.
                    texture_cache.InvalidateMemoryFromGPU(vsharp.base_address, size);
                }
                {
                    const bool hazard = runtime.IsBufferAccessed(buffer, bound_buffers.back().offset,
                                                                 bound_buffers.back().size,
                                                                 desc.is_written);
                    needs_barrier |= hazard;
                    if (reads_new_cloth && !desc.is_written) {
                        NoteClothRead(stage.pgm_hash, buffer_index, vsharp.base_address, size,
                                      bound_buffers.back().size, needs_barrier, runtime.AccessEpoch());
                    }
                    // gow3 diagnostics (GOW3_BARRIER_STATS=1): which binding of which shader
                    // reports a hazard (first 40, then 5 s counts by shader/slot).
                    if (hazard && BarrierStatsEnabled()) {
                        static u32 logged = 0;
                        static std::map<std::tuple<u64, u32, bool, bool>, u32> counts;
                        static auto last = std::chrono::steady_clock::now();
                        const bool raw = runtime.IsBufferAccessed(buffer, offset, size, false);
                        ++counts[{stage.pgm_hash, binding.buffer, bool(desc.is_written), raw}];
                        if (logged < 40 && stage.pgm_hash == 0x8f067540) {
                            ++logged;
                            std::printf("Hazard: shader %08llx slot %u %s %#llx+%llu tracked %u, stride %u, "
                                        "records %u, extent %#x, swizzle %u tid %u (%s)\n",
                                        static_cast<unsigned long long>(stage.pgm_hash), binding.buffer,
                                        desc.is_written ? "write" : "read",
                                        static_cast<unsigned long long>(vsharp.base_address),
                                        static_cast<unsigned long long>(size), bound_buffers.back().size,
                                        u32(vsharp.stride), u32(vsharp.num_records), desc.access_extent,
                                        u32(vsharp.swizzle_enable), u32(vsharp.add_tid_enable),
                                        raw ? "after a GPU write" : "after a read only");
                        }
                        if (const auto now = std::chrono::steady_clock::now(); now - last >= std::chrono::seconds{5}) {
                            last = now;
                            std::vector<std::pair<u32, std::tuple<u64, u32, bool, bool>>> order;
                            for (const auto& [key, n] : counts) {
                                order.emplace_back(n, key);
                            }
                            std::ranges::sort(order, std::greater{});
                            std::string text;
                            for (size_t i = 0; i < std::min<size_t>(order.size(), 10); ++i) {
                                const auto& [shader, slot, written, after_write] = order[i].second;
                                text += fmt::format(" {:08x}/slot{} {}{}={};", shader, slot,
                                                    written ? "write" : "read",
                                                    after_write ? " after write" : " after read",
                                                    order[i].first);
                            }
                            std::printf("Hazards (5 s):%s\n", text.c_str());
                            std::fflush(stdout);
                            counts.clear();
                        }
                    }
                }
            }
        }

        auto& set_write = set_writes[write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified++;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = 1;
        set_write.descriptorType = vk::DescriptorType::eStorageBuffer;
        set_write.pBufferInfo = &buffer_infos.back();
        ++binding.buffer;
    }
}

Rasterizer::ImageDescCacheEntry& Rasterizer::CachedImageDescEntry(const AmdGpu::Image& sharp,
                                                                   const Shader::ImageResource& res,
                                                                   u64 hash) {
    std::array<u64, 4> key;
    std::memcpy(key.data(), &sharp, sizeof(key));
    const u32 flags = u32(res.is_written) | u32(res.is_depth) << 1 | u32(res.is_array) << 2;
    // Two-way set associative: a frame binds a few thousand distinct T#s.
    auto* set = &image_desc_cache[(hash % (image_desc_cache.size() / 2)) * 2];
    const auto matches = [&](const ImageDescCacheEntry& e) {
        return e.flags == flags && e.sharp == key;
    };
    const bool disabled = Gow3Toggle::Disabled(Gow3Toggle::ImageDescCache);
    ImageDescCacheEntry* slot = nullptr;
    if (!disabled && matches(set[0])) {
        slot = &set[0];
    } else if (!disabled && matches(set[1])) {
        slot = &set[1];
    } else {
        // Replace the least recently used way that no binding of this call points at.
        slot = set[0].last_use <= set[1].last_use ? &set[0] : &set[1];
        if (slot->pinned == bind_epoch) {
            slot = slot == &set[0] ? &set[1] : &set[0];
        }
        if (slot->pinned == bind_epoch) {
            slot = &image_desc_overflow.emplace_back();
        }
    }
    slot->last_use = ++desc_use_counter;
    auto& entry = *slot;
    if (entry.flags != flags || entry.sharp != key ||
        Gow3Toggle::Disabled(Gow3Toggle::ImageDescCache)) {
        entry.desc = VideoCore::TextureCache::ImageDesc{sharp, res};
        entry.sharp = key;
        entry.flags = flags;
        entry.found_generation = ~0ULL;
        entry.view_memo = {};
    }
    return entry;
}

// gow3 diagnostics: GOW3_TRACE_PS=<hex hash>[,...] (see BeginRenderingFull): their textures too.
static bool TracedShader(u64 hash) {
    static const std::vector<u64> hashes = [] {
        std::vector<u64> out;
        const char* env = std::getenv("GOW3_TRACE_PS");
        while (env && *env) {
            char* end = nullptr;
            out.push_back(std::strtoull(env, &end, 16));
            env = end && *end == ',' ? end + 1 : nullptr;
        }
        return out;
    }();
    return !hashes.empty() && std::ranges::find(hashes, hash) != hashes.end();
}

void Rasterizer::BindTextures(const Shader::Info& stage, const PreparedStage* prepared,
                              Shader::Backend::Bindings& binding, u32& write_index,
                              bool& barrier, bool on_helper) {
    // gow3 21 Oct 2026 (GOW3_CPU_TRIMS=1): a stage without images only binds its samplers (the
    // path below did the same after clearing its lists and initializing ~1 KiB of memo state).
    if (CpuTrims() && stage.images.empty()) {
        BindSamplers(stage, prepared, binding, write_index);
        return;
    }
    const u32 first_image_idx = image_infos.size();
    TextureSet* set_slot = nullptr;
    if (!on_helper && BindTexturesFromSet(stage, prepared, first_image_idx, barrier, set_slot)) {
        // Descriptor writes: one per image (no mip arrays in a memoized set).
        for (u32 i = 0; i < stage.images.size(); ++i) {
            auto& set_write = set_writes[write_index++];
            set_write.dstSet = VK_NULL_HANDLE;
            set_write.dstBinding = binding.unified++;
            set_write.dstArrayElement = 0;
            set_write.descriptorCount = 1;
            set_write.descriptorType = vk::DescriptorType::eSampledImage;
            set_write.pImageInfo = &image_infos[first_image_idx + i];
        }
        BindSamplers(stage, prepared, binding, write_index);
        return;
    }
    image_bindings.clear();
    image_binding_entries.clear();
    image_desc_overflow.clear();
    image_desc_storage.clear();
    ++bind_epoch;
    // To emulate storing to explicit mip levels, build a descriptor array with each mip level.
    boost::container::small_vector<u32, 8> image_descriptor_array_sizes;
    // TextureSetMemo: what this call resolves, to remember when the set qualifies.
    // GOW3_CPU_TRIMS=1: a reused array; only a memoizing call (GPU command thread) resets the
    // entries of its images, the only ones it writes and copies.
    std::optional<std::array<TextureSetEntry, TextureSet::MaxImages>> resolved_local;
    auto& resolved = CpuTrims() ? texture_set_scratch : resolved_local.emplace();
    bool set_ok = set_slot != nullptr;
    if (CpuTrims() && set_ok) {
        std::fill_n(resolved.begin(), std::min<size_t>(stage.images.size(), resolved.size()),
                    TextureSetEntry{});
    }
    // SampleSceneProxies: bindings that may read a reduced scene proxy instead of the native
    // image (normalized sampling only; see Shader::ImageResource::needs_native).
    boost::container::small_vector<bool, 16> binding_proxy_ok;
    const bool sample_proxies = !Gow3Toggle::Disabled(Gow3Toggle::SampleSceneProxies) &&
                                !on_helper && scene_targets->Reduced();

    for (u32 image_index = 0; image_index < stage.images.size(); ++image_index) {
        const auto& image_desc = stage.images[image_index];
        auto fetched_tsharp =
            prepared ? prepared->image_sharps[image_index] : image_desc.GetSharp(stage);
        TestGarbageTSharp(fetched_tsharp);
        const auto tsharp = fetched_tsharp;
        if (TracedShader(stage.pgm_hash)) {
            static std::atomic<int> traced{0};
            if (traced.fetch_add(1) < 80) {
                std::printf("PS trace texture: shader %08llx image %u at %#llx %ux%u pitch %u levels %u-%u "
                            "format %u/%u\n",
                            static_cast<unsigned long long>(stage.pgm_hash), image_index,
                            static_cast<unsigned long long>(tsharp.Address()), u32(tsharp.width) + 1,
                            u32(tsharp.height) + 1, u32(tsharp.pitch) + 1, u32(tsharp.base_level),
                            u32(tsharp.last_level), u32(tsharp.GetDataFmt()), u32(tsharp.GetNumberFmt()));
                std::fflush(stdout);
            }
        }
        // gow3: a hash lookup per texture per draw for a diagnostic only.
        static const bool warn_meta = std::getenv("GOW3_WARN_META_TEXTURE") != nullptr;
        if (warn_meta && texture_cache.IsMeta(tsharp.Address())) {
            LOG_WARNING(Render_Vulkan, "Unexpected metadata read by a shader (texture)");
        }

        const auto data_fmt = tsharp.GetDataFmt();
        const auto num_fmt = tsharp.GetNumberFmt();
        if (tsharp.Address() == 0 || data_fmt == AmdGpu::DataFormat::FormatInvalid) {
            image_bindings.emplace_back(VideoCore::ImageId{}, &image_desc_storage.emplace_back());
            image_binding_entries.push_back(nullptr);
            binding_proxy_ok.push_back(false);
            image_descriptor_array_sizes.push_back(1);
            continue;
        }

        if (!memory->IsValidGpuMapping(tsharp.Address(), 0) ||
            !IsKnownFormat(data_fmt, num_fmt) || !PlausibleTSharp(tsharp)) {
            LOG_WARNING(Render_Vulkan,
                        "Rejecting invalid T# address={:#x}, pitch={}, width={}, "
                        "data_format={}, num_format={}",
                        tsharp.Address(), tsharp.pitch, tsharp.width, static_cast<u32>(data_fmt),
                        static_cast<u32>(num_fmt));
            if (!prepared && !on_helper && (image_desc.sharp_fetch.load_mask & 1)) {
                DescribeSharpSource(stage, image_desc.sharp_fetch.offsets[0], "texture");
            }
            image_bindings.emplace_back(VideoCore::ImageId{}, &image_desc_storage.emplace_back());
            image_binding_entries.push_back(nullptr);
            binding_proxy_ok.push_back(false);
            image_descriptor_array_sizes.push_back(1);
            continue;
        }

        const Shader::MipStorageFallbackMode mip_fallback_mode = image_desc.mip_fallback_mode;
        // Same as image_desc.NumBindings(stage), without fetching the T# again.
        const u32 num_bindings =
            mip_fallback_mode == Shader::MipStorageFallbackMode::DynamicIndex
                ? static_cast<u32>(tsharp.last_level - tsharp.base_level + 1)
                : 1u;
        const bool proxy_candidate = sample_proxies && num_bindings == 1 &&
                                     !image_desc.needs_native && !image_desc.is_written &&
                                     mip_fallback_mode == Shader::MipStorageFallbackMode::None;

        auto& desc_entry =
            prepared ? CachedImageDescEntry(tsharp, image_desc, prepared->image_hashes[image_index])
                     : CachedImageDescEntry(tsharp, image_desc);
        if (!PlausibleImage(desc_entry.desc.info, tsharp, memory, instance.GetMaxImageArrayLayers())) {
            static u32 reports = 0;
            if (reports < 32) {
                ++reports;
                LOG_WARNING(Render_Vulkan,
                            "Ignoring a garbage T# for stage {:#x}: {:#x}, {} bytes, {}x{}",
                            stage.pgm_hash, tsharp.Address(), desc_entry.desc.info.guest_size,
                            tsharp.width + 1, tsharp.height + 1);
            }
            image_bindings.emplace_back(VideoCore::ImageId{}, &image_desc_storage.emplace_back());
            image_binding_entries.push_back(nullptr);
            binding_proxy_ok.push_back(false);
            image_descriptor_array_sizes.push_back(1);
            continue;
        }
        // GOW3_SCENE_DEBUG: proxied scene targets this binding reads at the native size, and why.
        const auto debug_native = [&](const VideoCore::Image& image) {
            if (!scene_debug_frame || proxy_candidate || !image.scene_proxy) return;
            std::printf("Scene native read: %s %016llx %s %ux%u needs_native %d written %d "
                        "bindings %u mip_fallback %d\n",
                        stage.sw_stage == Shader::SwStage::Compute ? "cs" : "gfx",
                        (unsigned long long)stage.pgm_hash,
                        vk::to_string(image.info.pixel_format).c_str(), image.info.size.width,
                        image.info.size.height, int(image_desc.needs_native),
                        int(image_desc.is_written), num_bindings, int(mip_fallback_mode));
        };
        for (auto i = 0; i < num_bindings; i++) {
            // gow3: a plain binding (no mip override) of the same T# resolves to the same image
            // while no image was registered or unregistered.
            if (mip_fallback_mode == Shader::MipStorageFallbackMode::None &&
                desc_entry.found_generation == texture_cache.RegistryGeneration() &&
                !Gow3Toggle::Disabled(Gow3Toggle::TextureBindingMemo)) {
                desc_entry.pinned = bind_epoch;
                auto& [image_id, _] =
                    image_bindings.emplace_back(desc_entry.found_id, &desc_entry.found_desc);
                image_binding_entries.push_back(&desc_entry);
                binding_proxy_ok.push_back(proxy_candidate);
                texture_cache.MarkFound(image_id);
                auto* image = &texture_cache.GetImage(image_id);
                if (auto depth_image_id = texture_cache.GetAssociatedDepth(*image)) {
                    image_id = depth_image_id;
                    image = &texture_cache.GetImage(image_id);
                }
                debug_native(*image);
                if (image->binding.is_bound) {
                    image->binding.force_general |= image_desc.is_written;
                }
                image->binding.is_bound = 1u;
                continue;
            }
            auto& desc = image_desc_storage.emplace_back(desc_entry.desc);
            auto& [image_id, _] = image_bindings.emplace_back(VideoCore::ImageId{}, &desc);
            image_binding_entries.push_back(nullptr);
            binding_proxy_ok.push_back(proxy_candidate);

            if (mip_fallback_mode == Shader::MipStorageFallbackMode::ConstantIndex) {
                ASSERT(num_bindings == 1);
                desc.view_info.range.base.level += image_desc.constant_mip_index;
                desc.view_info.range.extent.levels = 1;
            } else if (mip_fallback_mode == Shader::MipStorageFallbackMode::DynamicIndex) {
                desc.view_info.range.base.level += i;
                desc.view_info.range.extent.levels = 1;
            }

            const u64 generation = texture_cache.RegistryGeneration();
            image_id = texture_cache.FindImage(desc);
            if (mip_fallback_mode == Shader::MipStorageFallbackMode::None &&
                generation == texture_cache.RegistryGeneration() &&
                desc_entry.pinned != bind_epoch) {
                desc_entry.found_generation = generation;
                desc_entry.found_id = image_id;
                desc_entry.found_desc = desc;
                desc_entry.view_memo = {};
            }
            auto* image = &texture_cache.GetImage(image_id);
            if (auto depth_image_id = texture_cache.GetAssociatedDepth(*image)) {
                // If this image has an associated depth image, it's a stencil attachment.
                // Redirect the access to the actual depth-stencil buffer.
                image_id = depth_image_id;
                image = &texture_cache.GetImage(image_id);
            }
            debug_native(*image);
            if (image->binding.is_bound) {
                // The image is already bound. In case if it is about to be used as storage we
                // need to force general layout on it.
                image->binding.force_general |= image_desc.is_written;
            }
            image->binding.is_bound = 1u;
        }

        image_descriptor_array_sizes.push_back(num_bindings);
    }

    // Second pass to re-bind images that were updated after binding
    for (u32 binding_index = 0; binding_index < image_bindings.size(); ++binding_index) {
        auto& [image_id, desc_ptr] = image_bindings[binding_index];
        auto* memo_entry = image_binding_entries[binding_index];
        const auto& desc = *desc_ptr;
        bool is_storage = desc.type == VideoCore::TextureCache::BindingType::Storage;
        if (is_storage && image_id) {
            u64 lo, hi;
            if (Gow3WatchRange::Range(lo, hi)) {
                const auto& written = texture_cache.GetImage(image_id);
                Gow3WatchRange::Note("storage image", stage.pgm_hash, binding_index,
                                     written.info.guest_address, written.info.guest_size);
            }
        }
        if (!image_id) {
            image_infos.emplace_back(VK_NULL_HANDLE, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
        } else {
            if (auto& old_image = texture_cache.GetImage(image_id);
                old_image.binding.needs_rebind) {
                old_image.binding = {};
                auto& rebind_desc = image_desc_storage.emplace_back(desc);
                desc_ptr = &rebind_desc;
                image_id = texture_cache.FindImage(rebind_desc);
                memo_entry = nullptr;
                set_ok = false;
            }

            bound_images.emplace_back(image_id);

            auto& image = texture_cache.GetImage(image_id);
            auto& image_view = texture_cache.FindTexture(
                image_id, desc, memo_entry ? &memo_entry->view_memo : nullptr, !on_helper);
            const auto binding = image.binding;

            // gow3: a proxied scene image sampled with normalized coordinates reads the proxy:
            // no resample to the native size (the native image is not touched).
            if (binding_proxy_ok[binding_index] && !is_storage && !binding.force_general &&
                !binding.is_target &&
                !(upscaler->Enabled() && upscaler->RedirectsSampled(image_id))) {
                if (const auto proxy = scene_targets->SampleProxy(image, desc.view_info)) {
                    image.usage.texture = 1u;
                    image_infos.emplace_back(VK_NULL_HANDLE, proxy->view, proxy->layout);
                    if (set_ok && binding_index < resolved.size()) {
                        resolved[binding_index] = {image_id, proxy->view, image.backing,
                                                   desc.view_info.range, true};
                    }
                    ++proxy_samples;
                    continue;
                }
            }
            // The image is either bound as storage in a separate descriptor or bound as render
            // target in feedback loop. Depth images are excluded because they can't be bound as
            // storage; a depth target sampled by its own draw shares the general layout with the
            // attachment (DepthFeedbackLayout).
            if (DepthFeedbackGeneral() && binding.is_target && image.info.props.is_depth &&
                !is_storage) {
                barrier |= runtime.Transit(&image, DepthFeedbackLayout, DepthFeedbackStages,
                                           DepthFeedbackAccess);
            } else if ((binding.force_general || binding.is_target) && !image.info.props.is_depth) {
                if (instance.IsAttachmentFeedbackLoopLayoutSupported() && image.binding.is_target) {
                    barrier |= runtime.Transit(
                        &image, vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT,
                        vk::PipelineStageFlagBits2::eAllGraphics, vk::AccessFlagBits2::eShaderRead);
                } else {
                    barrier |= runtime.Transit(
                        &image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eAllCommands,
                        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
                }
            } else {
                if (is_storage) {
                    barrier |= runtime.Transit(
                        &image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eAllCommands,
                        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                        desc.view_info.range);
                } else {
                    const auto new_layout = image.info.props.is_depth
                                                ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                                : vk::ImageLayout::eShaderReadOnlyOptimal;
                    barrier |= runtime.Transit(
                        &image, new_layout, vk::PipelineStageFlagBits2::eAllCommands,
                        vk::AccessFlagBits2::eShaderRead, desc.view_info.range);
                }
            }
            image.usage.storage |= is_storage;
            image.usage.texture |= !is_storage;
            if (FrameCapture::Active()) {
                FrameCapture::Sampled(image.info, is_storage);
            }

            vk::ImageView view = *image_view.image_view;
            vk::ImageLayout layout = image.backing->state.layout;
            if (upscaler->Enabled()) {
                // gow3: the display pass reads the upscaled frame (scaled presets).
                upscaler->RedirectSampled(image_id, image_view.info, view, layout);
            }
            image_infos.emplace_back(VK_NULL_HANDLE, view, layout);
            if (set_ok && binding_index < resolved.size()) {
                resolved[binding_index] = {image_id, *image_view.image_view, image.backing,
                                           desc.view_info.range};
                set_ok = !is_storage && !binding.force_general && !binding.is_target;
            }
        }
    }
    if (set_ok && image_bindings.size() == stage.images.size()) {
        // Remember the set: the same T#s resolve the same way while no image is (un)registered.
        set_slot->generation = texture_cache.RegistryGeneration();
        set_slot->scene_generation = scene_targets->Generation();
        set_slot->count = static_cast<u32>(stage.images.size());
        std::copy_n(resolved.begin(), set_slot->count, set_slot->entries.begin());
    } else if (set_slot) {
        set_slot->key = 0;
    }

    u32 image_info_idx = first_image_idx;
    u32 image_binding_idx = 0;
    for (u32 array_size : image_descriptor_array_sizes) {
        const auto& desc = *image_bindings[image_binding_idx].second;
        const bool is_storage = desc.type == VideoCore::TextureCache::BindingType::Storage;
        auto& set_write = set_writes[write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = array_size;
        set_write.descriptorType =
            is_storage ? vk::DescriptorType::eStorageImage : vk::DescriptorType::eSampledImage;
        set_write.pImageInfo = &image_infos[image_info_idx];

        image_info_idx += array_size;
        image_binding_idx += array_size;
        binding.unified += array_size;
    }

    BindSamplers(stage, prepared, binding, write_index);
}

void Rasterizer::BindSamplers(const Shader::Info& stage, const PreparedStage* prepared,
                              Shader::Backend::Bindings& binding, u32& write_index) {
    for (u32 sampler_index = 0; sampler_index < stage.samplers.size(); ++sampler_index) {
        const auto& sampler = stage.samplers[sampler_index];
        auto ssharp =
            prepared ? prepared->sampler_sharps[sampler_index] : sampler.GetSharp(stage);
        const auto vk_sampler = texture_cache.GetSampler(
            ssharp, Regs().ta_bc_base, sampler.is_depth, sampler.is_depth ? 0.0f : sampler_lod_bias);
        image_infos.emplace_back(vk_sampler, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
        auto& set_write = set_writes[write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified++;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = 1;
        set_write.descriptorType = vk::DescriptorType::eSampler;
        set_write.pImageInfo = &image_infos.back();
    }
}

Rasterizer::BeginSignature Rasterizer::MakeBeginSignature(const GraphicsPipeline* pipeline) const {
    const auto& key = pipeline->GetGraphicsKey();
    const auto& regs = Regs();
    BeginSignature sig;
    const u32 num_color = std::bit_width(key.mrt_mask);
    for (u32 cb = 0; cb < num_color && cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
        sig.ids[cb] = cb_descs[cb].first;
        if (cb_descs[cb].first) {
            sig.views[cb] = cb_descs[cb].second.view_info;
        }
        sig.color_samples[cb] = key.color_samples[cb];
    }
    sig.ids[AmdGpu::NUM_COLOR_BUFFERS] = db_desc.first;
    if (db_desc.first) {
        sig.views[AmdGpu::NUM_COLOR_BUFFERS] = db_desc.second.view_info;
    }
    sig.mrt_mask = key.mrt_mask;
    sig.num_samples = key.num_samples;
    sig.motion = key.motion_vectors;
    sig.scene_started = scene_started;
    sig.raster_scaling = upscaler->RasterScaling();
    sig.upscaler_state = upscaler->RedirectState();
    sig.generation = texture_cache.RegistryGeneration();
    std::memcpy(&sig.depth_control, &regs.depth_control, sizeof(u32));
    sig.depth_valid = regs.depth_buffer.DepthValid();
    sig.stencil_valid = regs.depth_buffer.StencilValid();
    return sig;
}

bool Rasterizer::BindTexturesFromSet(const Shader::Info& stage, const PreparedStage* prepared,
                                     u32 first_image_idx, bool& barrier, TextureSet*& slot) {
    const u32 count = static_cast<u32>(stage.images.size());
    if (!prepared || count == 0 || count > TextureSet::MaxImages ||
        Gow3Toggle::Disabled(Gow3Toggle::TextureSetMemo) || FrameCapture::Active()) {
        return false;
    }
    for (const auto& image : stage.images) {
        if (image.is_written || image.mip_fallback_mode != Shader::MipStorageFallbackMode::None) {
            return false;
        }
    }
    u64 key = XXH3_64bits_withSeed(prepared->image_hashes, count * sizeof(u64),
                                   reinterpret_cast<u64>(&stage));
    key |= 1; // 0 marks an empty slot
    auto& set = texture_sets[key % texture_sets.size()];
    slot = &set;
    const u64 generation = texture_cache.RegistryGeneration();
    const bool match = set.key == key && set.stage == &stage && set.count == count &&
                       set.generation == generation &&
                       set.scene_generation == scene_targets->Generation() &&
                       std::equal(set.hashes.begin(), set.hashes.begin() + count,
                                  prepared->image_hashes);
    if (!match) {
        ++texture_set_why[set.key != key ? 0 : set.generation != generation ? 1 : 3];
        ++texture_set_misses;
        set.key = key;
        set.stage = &stage;
        set.count = 0;
        set.generation = ~0ull;
        std::copy_n(prepared->image_hashes, count, set.hashes.begin());
        return false;
    }
    // Every image still has the backing its view belongs to and needs no refresh.
    for (u32 i = 0; i < count; ++i) {
        const auto& entry = set.entries[i];
        if (!entry.id) {
            continue;
        }
        const auto& image = texture_cache.GetImage(entry.id);
        // Proxy entries need a current proxy; native entries of a proxied image go through
        // BindTextures, which may sample the proxy instead.
        const bool proxies_on = !Gow3Toggle::Disabled(Gow3Toggle::SampleSceneProxies);
        if (image.backing != entry.backing || image.binding.needs_rebind ||
            image.binding.is_target || !texture_cache.IsUpToDate(entry.id) ||
            (entry.proxy ? !proxies_on ||
                               !scene_targets->ProxyCurrent(image, entry.range.base.level,
                                                            entry.range.extent.levels)
                         : image.scene_proxy && proxies_on) ||
            (upscaler->Enabled() && upscaler->RedirectsSampled(entry.id))) {
            ++texture_set_why[2];
            ++texture_set_misses;
            set.generation = ~0ull;
            return false;
        }
    }
    ++texture_set_hits;
    slot = nullptr;
    for (u32 i = 0; i < count; ++i) {
        const auto& entry = set.entries[i];
        if (!entry.id) {
            image_infos.emplace_back(VK_NULL_HANDLE, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
            continue;
        }
        texture_cache.MarkFound(entry.id);
        auto& image = texture_cache.GetImage(entry.id);
        image.binding.is_bound = 1u;
        bound_images.emplace_back(entry.id);
        image.usage.texture = 1u;
        if (entry.proxy) {
            image_infos.emplace_back(VK_NULL_HANDLE, entry.view,
                                     scene_targets->PrepareSample(image, entry.range.base.level));
            ++proxy_samples;
            continue;
        }
        if (DepthFeedbackGeneral() && image.info.props.is_depth && image.binding.is_target) {
            barrier |= runtime.Transit(&image, DepthFeedbackLayout, DepthFeedbackStages,
                                       DepthFeedbackAccess);
            image_infos.emplace_back(VK_NULL_HANDLE, entry.view, image.backing->state.layout);
            continue;
        }
        const auto new_layout = image.info.props.is_depth
                                    ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                    : vk::ImageLayout::eShaderReadOnlyOptimal;
        barrier |= runtime.Transit(&image, new_layout, vk::PipelineStageFlagBits2::eAllCommands,
                                   vk::AccessFlagBits2::eShaderRead, entry.range);
        image_infos.emplace_back(VK_NULL_HANDLE, entry.view, image.backing->state.layout);
    }
    (void)first_image_idx;
    return true;
}

RenderState Rasterizer::BeginRendering(const GraphicsPipeline* pipeline) {
    // gow3: RenderStateMemo (see BeginMemo).
    const bool memo_on = !Gow3Toggle::Disabled(Gow3Toggle::RenderStateMemo) && !FrameCapture::Active();
    if (Gow3Stats::enabled && ((begin_memo_hits + begin_memo_misses) & 0x3FFFF) == 0x3FFFF) {
        std::printf("Render state memo: %llu hits, %llu misses; texture sets: %llu hits, %llu "
                    "misses\n",
                    static_cast<unsigned long long>(begin_memo_hits),
                    static_cast<unsigned long long>(begin_memo_misses),
                    static_cast<unsigned long long>(texture_set_hits),
                    static_cast<unsigned long long>(texture_set_misses));
        std::printf("  texture set misses: other key %llu, generation %llu, image check %llu, "
                    "other %llu\n",
                    (unsigned long long)texture_set_why[0], (unsigned long long)texture_set_why[1],
                    (unsigned long long)texture_set_why[2], (unsigned long long)texture_set_why[3]);
        std::printf("  scene proxies sampled directly: %llu bindings\n",
                    static_cast<unsigned long long>(proxy_samples));
        proxy_samples = 0;
        texture_set_why = {};
        begin_memo_hits = begin_memo_misses = texture_set_hits = texture_set_misses = 0;
    }
    BeginSignature signature;
    if (memo_on) {
        signature = MakeBeginSignature(pipeline);
        const auto& regs = Regs();
        bool usable = begin_memo.valid && scheduler.IsRenderingWith(begin_memo.state) &&
                      !regs.depth_render_control.depth_clear_enable &&
                      !regs.depth_render_control.stencil_clear_enable &&
                      signature == begin_memo.signature;
        for (const auto id : signature.ids) {
            if (usable && id) {
                const auto& image = texture_cache.GetImage(id);
                usable = !image.binding.is_bound && !image.binding.needs_rebind;
            }
        }
        if (usable) {
            ++begin_memo_hits;
            attachment_feedback_loop = false;
            push_data.scene_size = begin_memo.scene_size;
            target_scale = begin_memo.target_scale;
            float_color0 = begin_memo.float_color0;
            return begin_memo.state;
        }
        ++begin_memo_misses;
    }
    begin_memo.valid = false;
    RenderState state = BeginRenderingFull(pipeline);
    if (memo_on && !attachment_feedback_loop) {
        // (is_clear of the depth attachment shares its bytes with has_depth.)
        bool clears = state.depth_stencil_attachment.depth_clear ||
                      state.depth_stencil_attachment.stencil_clear;
        for (u32 i = 0; i < state.num_color_attachments; ++i) {
            clears |= state.color_attachments[i].is_clear != 0;
        }
        if (!clears) {
            // Recomputed after the full path: it may have started the scene (scene_started).
            begin_memo = {true, MakeBeginSignature(pipeline), state, push_data.scene_size,
                          target_scale, float_color0};
        }
    }
    return state;
}

RenderState Rasterizer::BeginRenderingFull(const GraphicsPipeline* pipeline) {
    attachment_feedback_loop = false;
    float_color0 = false;
    using VulkanUpscalerTarget = TemporalUpscaler::Target;
    VulkanUpscalerTarget redirect{};
    bool color_redirected = false, depth_redirected = false;
    std::pair<u32, u32> guest_extent{1, 1};
    std::pair<vk::ImageView, vk::ImageLayout> original_color{};
    const auto& regs = Regs();
    const auto& key = pipeline->GetGraphicsKey();
    {
        u64 lo, hi;
        if (Gow3WatchRange::Range(lo, hi)) {
            const auto* ps = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
            for (u32 cb = 0; cb < cb_descs.size(); ++cb) {
                if (cb_descs[cb].first) {
                    const auto& image = texture_cache.GetImage(cb_descs[cb].first);
                    Gow3WatchRange::Note("render target", ps ? ps->pgm_hash : 0, cb,
                                         image.info.guest_address, image.info.guest_size);
                }
            }
        }
    }
    // gow3 diagnostics (GOW3_TRACE_PS=<hex hash>[,<hash>...]): draws with these pixel shaders:
    // render targets, depth, viewport and scissors (60 lines at most).
    {
        static const std::vector<u64> trace_ps = [] {
            std::vector<u64> out;
            const char* env = std::getenv("GOW3_TRACE_PS");
            while (env && *env) {
                char* end = nullptr;
                out.push_back(std::strtoull(env, &end, 16));
                env = end && *end == ',' ? end + 1 : nullptr;
            }
            return out;
        }();
        const auto* ps = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
        static std::atomic<int> traced{0};
        if (ps && !trace_ps.empty() && std::ranges::find(trace_ps, ps->pgm_hash) != trace_ps.end() &&
            traced.fetch_add(1) < 60) {
            const auto* vs = pipeline->GetStages()[u32(Shader::SwStage::Vertex)];
            std::string text = fmt::format("PS trace: ps {:08x} vs {:08x}", ps->pgm_hash, vs ? vs->pgm_hash : 0);
            for (u32 cb = 0; cb < cb_descs.size(); ++cb) {
                if (cb_descs[cb].first) {
                    const auto& image = texture_cache.GetImage(cb_descs[cb].first);
                    text += fmt::format("; rt{} {:#x} {}x{} {}", cb, image.info.guest_address,
                                        image.info.size.width, image.info.size.height,
                                        vk::to_string(image.info.pixel_format));
                }
            }
            if (db_desc.first) {
                const auto& depth = texture_cache.GetImage(db_desc.first);
                text += fmt::format("; depth {:#x} {}x{}", depth.info.guest_address, depth.info.size.width,
                                    depth.info.size.height);
            }
            const auto& vp = regs.viewports[0];
            text += fmt::format("; viewport scale {} {} offset {} {}", vp.xscale, vp.yscale, vp.xoffset,
                                vp.yoffset);
            const auto& s = regs.screen_scissor;
            text += fmt::format("; screen scissor {},{}-{},{}", s.top_left_x, s.top_left_y, s.bottom_right_x,
                                s.bottom_right_y);
            const auto scissor = [](const AmdGpu::ViewportScissor& v) {
                return fmt::format("{},{}-{},{}", u32(v.top_left_x), u32(v.top_left_y), u32(v.bottom_right_x),
                                   u32(v.bottom_right_y));
            };
            text += fmt::format("; window {} generic {} vp0 {}; window offset {},{}", scissor(regs.window_scissor),
                                scissor(regs.generic_scissor), scissor(regs.viewport_scissors[0]),
                                s32(regs.window_offset.window_x_offset), s32(regs.window_offset.window_y_offset));
            std::printf("%s\n", text.c_str());
            std::fflush(stdout);
        }
    }
    if (std::popcount(key.mrt_mask & 0x7f) >= 5 && db_desc.first &&
        scene_targets->EligibleScene(texture_cache.GetImage(db_desc.first))) scene_started = true;
    // A proxy attachment cannot represent MSAA or a feedback loop that reads the
    // same image through the guest's native descriptor during this draw.
    bool reduced = scene_started && upscaler->RasterScaling() && key.num_samples == 1;
    // GOW3_SCENE_DEBUG=<file>: once the file exists, why the passes of one frame keep the
    // native size (see NoteFrameStart).
    const bool debug_pass = scene_debug_frame && scene_started;
    std::string why;
    if (debug_pass) {
        why = fmt::format("pass vs {:08x} ps {:08x} mrt {:#x} samples {} raster_scaling {}:",
                          pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash,
                          key.mrt_mask ? pipeline->GetStage(Shader::SwStage::Fragment).pgm_hash
                                       : 0,
                          key.mrt_mask, u32(key.num_samples), upscaler->RasterScaling());
    }
    // Only this pass's attachments: slots past the mask's width keep earlier passes' targets,
    // often the G-buffer images the lighting passes sample (bound), which kept those native.
    const u32 num_attachments = Gow3Toggle::Disabled(Gow3Toggle::SceneAttachmentsOnly)
                                    ? u32(cb_descs.size())
                                    : u32(std::bit_width(key.mrt_mask));
    // All attachments of a reduced pass share one proxy size (scene or half resolution).
    std::optional<SceneResolution::Size> pass_size;
    const auto same_size = [&](const VideoCore::Image& image, u32 level) {
        const auto proxy = scene_targets->ProxySize(image, level);
        if (!pass_size) pass_size = proxy;
        return *pass_size == proxy;
    };
    for (u32 cb = 0; cb < num_attachments; ++cb) {
        const auto& [id, desc] = cb_descs[cb];
        if (id) {
            const auto& image = texture_cache.GetImage(id);
            // Mip levels of eligible chains have their own proxies (SceneTargets::Get).
            if (!scene_targets->Eligible(image) || image.binding.is_bound ||
                image.binding.needs_rebind || desc.view_info.range.base.layer ||
                !same_size(image, desc.view_info.range.base.level)) reduced = false;
            if (debug_pass) {
                why += fmt::format(" [{} {}x{} eligible {} bound {} rebind {} level {} layer {}]",
                                   vk::to_string(image.info.pixel_format), image.info.size.width,
                                   image.info.size.height, scene_targets->Eligible(image),
                                   u32(image.binding.is_bound), u32(image.binding.needs_rebind),
                                   desc.view_info.range.base.level,
                                   desc.view_info.range.base.layer);
            }
        }
    }
    if (db_desc.first && (!scene_targets->Eligible(texture_cache.GetImage(db_desc.first)) ||
                          !same_size(texture_cache.GetImage(db_desc.first), 0))) {
        reduced = false;
    }
    if (debug_pass) {
        if (db_desc.first) {
            const auto& depth = texture_cache.GetImage(db_desc.first);
            why += fmt::format(" depth [{} {}x{} eligible {}]",
                               vk::to_string(depth.info.pixel_format), depth.info.size.width,
                               depth.info.size.height, scene_targets->Eligible(depth));
        }
        std::printf("Scene pass: reduced %d %s\n", reduced, why.c_str());
    }
    push_data.scene_size = reduced ? SceneResolution::Pack(scene_targets->Size()) : 0;
    if (Gow3Stats::enabled) {
        Gow3Stats::reduced_draws.fetch_add(reduced, std::memory_order_relaxed);
        Gow3Stats::scene_draws.fetch_add(scene_started, std::memory_order_relaxed);
    }
    RenderState state;
    state.width = instance.GetMaxFramebufferWidth();
    state.height = instance.GetMaxFramebufferHeight();
    state.num_layers = std::numeric_limits<u16>::max();
    state.num_color_attachments = std::bit_width(key.mrt_mask);
    for (auto cb = 0u; cb < state.num_color_attachments; ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        if (!image_id) {
            state.color_attachments[cb] = {};
            continue;
        }
        auto* image = &texture_cache.GetImage(image_id);
        if (image->binding.needs_rebind) {
            last_targets[cb].generation = ~0ULL; // FindImage may rewrite the description
            image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
            image = &texture_cache.GetImage(image_id);
        }
        texture_cache.UpdateImage(image_id);
        runtime.SetBackingSamples(image, key.color_samples[cb]);
        const auto& image_view = texture_cache.FindRenderTarget(image_id, desc);
        const auto slice = image_view.info.range.base.layer;
        const auto mip = image_view.info.range.base.level;

        const auto& col_buf = regs.color_buffers[cb];
        const bool is_clear = texture_cache.IsMetaCleared(col_buf.CmaskAddress(), slice);
        texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);

        if (reduced) {
            // The native image is resolved lazily when a shader/copy actually reads it.
        } else if (image->binding.is_bound) {
            ASSERT_MSG(!image->binding.force_general,
                       "Having image both as storage and render target is unsupported");
            runtime.FlushBarriers();
            needs_barrier |=
                runtime.Transit(image,
                                instance.IsAttachmentFeedbackLoopLayoutSupported()
                                    ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
                                    : vk::ImageLayout::eGeneral,
                                vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                vk::AccessFlagBits2::eColorAttachmentWrite |
                                    vk::AccessFlagBits2::eColorAttachmentRead);
            attachment_feedback_loop = true;
        } else {
            needs_barrier |= runtime.Transit(image, vk::ImageLayout::eColorAttachmentOptimal,
                                             vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                             vk::AccessFlagBits2::eColorAttachmentWrite |
                                                 vk::AccessFlagBits2::eColorAttachmentRead,
                                             desc.view_info.range);
        }

        state.width = std::min<u32>(state.width, std::max(image->info.size.width >> mip, 1u));
        state.height = std::min<u32>(state.height, std::max(image->info.size.height >> mip, 1u));
        state.num_layers = std::min<u32>(state.num_layers, image_view.info.range.extent.layers);

        const auto clear_value =
            is_clear ? LiverpoolToVK::ColorBufferClearValue(col_buf) : vk::ClearValue{};
        auto& attachment = state.color_attachments[cb];
        attachment.image_view = *image_view.image_view;
        attachment.image_layout = image->backing->state.layout;
        attachment.clear_value = clear_value.color.uint32;
        attachment.is_clear = is_clear;

        if (reduced) {
            const auto target = scene_targets->Attachment(image_id, desc.view_info);
            attachment.image_view = target.view;
            attachment.image_layout = target.layout;
        }
        image->usage.render_target = 1u;
        if (cb == 0 && upscaler->Enabled()) {
            color_redirected = upscaler->RedirectColor(image_id, desc.view_info, redirect);
            float_color0 = color_redirected && upscaler->RedirectedToFloat();
            if (color_redirected) {
                guest_extent = {image->info.size.width, image->info.size.height};
                original_color = {attachment.image_view, attachment.image_layout};
                attachment.image_view = redirect.view;
                attachment.image_layout = redirect.layout;
            }
        }
    }
    for (u32 cb = state.num_color_attachments; cb < state.color_attachments.size(); ++cb) {
        state.color_attachments[cb] = {};
    }

    if (auto image_id = db_desc.first; image_id) {
        auto& desc = db_desc.second;
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        const auto& image_view = texture_cache.FindDepthTarget(image_id, desc);
        auto& image = texture_cache.GetImage(image_id);

        const auto slice = image_view.info.range.base.layer;
        const bool is_depth_clear =
            (regs.depth_render_control.depth_clear_enable && regs.depth_control.depth_enable &&
             regs.depth_control.depth_write_enable) ||
            texture_cache.IsMetaCleared(htile_address, slice);
        const bool is_stencil_clear = regs.depth_render_control.stencil_clear_enable;
        texture_cache.TouchMeta(htile_address, slice, false);
        ASSERT(desc.view_info.range.extent.levels == 1 && !image.binding.needs_rebind);

        const bool has_stencil = image.info.props.has_stencil;
        // Stencil writes can be enabled while depth writes are off.
        const bool stencil_write =
            has_stencil && regs.depth_control.stencil_enable && !desc.view_info.is_storage;
        const auto new_layout = desc.view_info.is_storage
                                    ? has_stencil ? vk::ImageLayout::eDepthStencilAttachmentOptimal
                                                  : vk::ImageLayout::eDepthAttachmentOptimal
                                : stencil_write
                                    ? vk::ImageLayout::eDepthReadOnlyStencilAttachmentOptimal
                                : has_stencil ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                              : vk::ImageLayout::eDepthReadOnlyOptimal;
        if (!reduced && DepthFeedbackGeneral() && image.binding.is_bound) {
            needs_barrier |= runtime.Transit(&image, DepthFeedbackLayout, DepthFeedbackStages,
                                             DepthFeedbackAccess);
        } else if (!reduced) needs_barrier |= runtime.Transit(&image, new_layout,
                                         vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                             vk::PipelineStageFlagBits2::eLateFragmentTests,
                                         vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
                                             vk::AccessFlagBits2::eDepthStencilAttachmentRead,
                                         desc.view_info.range);

        state.width = std::min<u32>(state.width, image.info.size.width);
        state.height = std::min<u32>(state.height, image.info.size.height);
        state.num_layers = std::min<u32>(state.num_layers, image_view.info.range.extent.layers);

        auto& attachment = state.depth_stencil_attachment;
        attachment.image_view = *image_view.image_view;
        attachment.image_layout = image.backing->state.layout;
        {
            static const u64 trace_address = [] {
                const char* env = std::getenv("GOW3_TRACE_IMAGE");
                return env ? std::strtoull(env, nullptr, 16) : 0ULL;
            }();
            if (trace_address && image.info.guest_address == trace_address) {
                static std::atomic<int> lines{0};
                if (lines.fetch_add(1) < 300) {
                    std::printf("Trace image: depth attachment, reduced %d, storage %d, stencil write %d, "
                                "depth write %d, layout %s, state access %s\n",
                                int(reduced), int(desc.view_info.is_storage), int(stencil_write),
                                int(regs.depth_control.depth_write_enable),
                                vk::to_string(image.backing->state.layout).c_str(),
                                vk::to_string(image.backing->state.access_mask).c_str());
                    std::fflush(stdout);
                }
            }
        }
        attachment.clear_value = {};
        attachment.is_clear = 0;

        if (regs.depth_buffer.DepthValid()) {
            attachment.clear_value[0] = is_depth_clear ? std::bit_cast<u32>(regs.depth_clear) : 0u;
            // gow3 diagnostics (Uncharted shadow map D16 0x11352d0000 found cleared to 0): every
            // distinct depth clear (format, value, how it was requested) once, and the live switch
            // "dbclear one" (GOW3_DMA_TRIGGER) clearing 16-bit depth targets to 1.0.
            if (is_depth_clear) {
                const bool meta = !(regs.depth_render_control.depth_clear_enable &&
                                    regs.depth_control.depth_enable &&
                                    regs.depth_control.depth_write_enable);
                static std::mutex clear_mutex;
                static std::vector<u64> clears_seen;
                const u64 key = (u64(std::bit_cast<u32>(regs.depth_clear)) << 32) |
                                (u64(image.info.pixel_format) << 1) | u64(meta);
                std::scoped_lock lock{clear_mutex};
                if (std::find(clears_seen.begin(), clears_seen.end(), key) == clears_seen.end()) {
                    clears_seen.push_back(key);
                    std::printf("GPU: depth clear %s %ux%u at %#llx to %g (%s)\n",
                                vk::to_string(image.info.pixel_format).c_str(), image.info.size.width,
                                image.info.size.height, (unsigned long long)image.info.guest_address,
                                regs.depth_clear, meta ? "HTILE meta clear" : "DB_RENDER_CONTROL clear");
                    std::fflush(stdout);
                }
                if (depth16_clear_one.load(std::memory_order_relaxed) &&
                    image.info.pixel_format == vk::Format::eD16Unorm) {
                    attachment.clear_value[0] = std::bit_cast<u32>(1.0f);
                }
            }
            attachment.has_depth = true;
            attachment.depth_clear = is_depth_clear;
        }
        if (regs.depth_buffer.StencilValid()) {
            attachment.clear_value[1] = is_stencil_clear ? regs.stencil_clear : 0u;
            attachment.has_stencil = true;
            attachment.stencil_clear = is_stencil_clear;
        }

        if (reduced) {
            const auto target = scene_targets->Attachment(image_id, desc.view_info);
            attachment.image_view = target.view;
            attachment.image_layout = target.layout;
        }
        image.usage.depth_target = true;
        if (upscaler->Enabled()) {
            VulkanUpscalerTarget depth_redirect;
            depth_redirected = upscaler->RedirectDepth(image_id, depth_redirect);
            if (depth_redirected) {
                guest_extent = {image.info.size.width, image.info.size.height};
                attachment.image_view = depth_redirect.view;
                attachment.image_layout = depth_redirect.layout;
                redirect = depth_redirect;
            }
        }
    } else {
        state.depth_stencil_attachment = {};
    }

    // gow3: a pass drawn into the upscaler's output-size images (UI, display pass): every
    // attachment must be redirected, viewports and scissors are scaled.
    target_scale = {1.0f, 1.0f};
    if (color_redirected || depth_redirected) {
        u32 color_targets = 0;
        for (u32 cb = 0; cb < state.num_color_attachments; ++cb) {
            color_targets += state.color_attachments[cb].image_view ? 1 : 0;
        }
        const bool has_depth = bool(db_desc.first);
        if ((color_targets > 0 && !color_redirected) || (has_depth && !depth_redirected) ||
            color_targets > 1) {
            static u32 warned = 0;
            if (warned < 8) {
                ++warned;
                const auto describe = [&](VideoCore::ImageId id) {
                    if (!id) {
                        return std::string{"-"};
                    }
                    const auto& info = texture_cache.GetImage(id).info;
                    return fmt::format("{:#x} {}x{} {}", info.guest_address, info.size.width,
                                       info.size.height, vk::to_string(info.pixel_format));
                };
                std::printf("Upscaler: pass not redirected (color %s: %s, depth %s: %s, %u "
                            "targets)\n",
                            color_redirected ? "yes" : "no", describe(cb_descs[0].first).c_str(),
                            depth_redirected ? "yes" : "no", describe(db_desc.first).c_str(),
                            state.num_color_attachments);
            }
            if (color_redirected) {
                state.color_attachments[0].image_view = original_color.first;
                state.color_attachments[0].image_layout = original_color.second;
            }
            if (depth_redirected) {
                auto& depth_image = texture_cache.GetImage(db_desc.first);
                state.depth_stencil_attachment.image_view =
                    *texture_cache.FindDepthTarget(db_desc.first, db_desc.second).image_view;
                state.depth_stencil_attachment.image_layout = depth_image.backing->state.layout;
            }
        } else {
            state.width = redirect.width;
            state.height = redirect.height;
            // The UI movie viewport is now 1920x1080 even when the scene is 960x540.
            // A native viewport (or native window-space UI vertices) must not be doubled
            // again. The final display copy still uses the guest render-size coordinates.
            const auto& viewport = regs.viewports[0];
            const bool native_coordinates = redirect.native_ui &&
                (UiComposition::NativeViewport(viewport.xscale * 2, viewport.yscale * 2) ||
                 (regs.IsClipDisabled() && !regs.viewport_control.xscale_enable &&
                  !regs.viewport_control.yscale_enable));
            target_scale = UiComposition::Scale(guest_extent.first, guest_extent.second,
                                                redirect.width, redirect.height,
                                                native_coordinates);
        }
    }

    if (state.num_layers == std::numeric_limits<u16>::max()) {
        state.num_layers = 1;
    }

    if (reduced) {
        // Half-resolution passes keep the scene's factor (proxy / native = scene / 1920).
        const auto size = scene_targets->Size();
        const auto proxy = pass_size.value_or(size);
        state.width = proxy.width;
        state.height = proxy.height;
        target_scale = {float(size.width) / 1920.0f, float(size.height) / 1080.0f};
    }
    if (FrameCapture::Active()) {
        std::array<const VideoCore::ImageInfo*, AmdGpu::NUM_COLOR_BUFFERS> colors{};
        for (u32 cb = 0; cb < state.num_color_attachments; ++cb) {
            if (cb_descs[cb].first) {
                colors[cb] = &texture_cache.GetImage(cb_descs[cb].first).info;
            }
        }
        const auto* depth =
            db_desc.first ? &texture_cache.GetImage(db_desc.first).info : nullptr;
        FrameCapture::BeginPass(colors.data(), state.num_color_attachments, depth);
    }
    // gow3: object motion vector attachment of G-buffer pipelines.
    if (key.motion_vectors) {
        object_motion->Attach(state, state.width, state.height);
    }
    return state;
}

void Rasterizer::Resolve() {
    const auto& mrt0_hint = CbExtent(0);
    const auto& mrt1_hint = CbExtent(1);
    VideoCore::TextureCache::ImageDesc mrt0_desc{Regs().color_buffers[0], mrt0_hint};
    VideoCore::TextureCache::ImageDesc mrt1_desc{Regs().color_buffers[1], mrt1_hint};
    if (const u32 max_layers = instance.GetMaxImageArrayLayers();
        !PlausibleTarget(mrt0_desc.info, max_layers, "resolve source") ||
        !PlausibleTarget(mrt1_desc.info, max_layers, "resolve target")) {
        return; // gow3 (#63): garbage registers, see PlausibleTarget
    }
    auto& mrt0_image = texture_cache.GetImage(texture_cache.FindImage(mrt0_desc, true));
    auto& mrt1_image = texture_cache.GetImage(texture_cache.FindImage(mrt1_desc, true));

    ScopeMarkerBegin(fmt::format("Resolve:MRT0={:#x}:MRT1={:#x}",
                                 Regs().color_buffers[0].Address(),
                                 Regs().color_buffers[1].Address()));
    runtime.ResolveImage(&mrt0_image, &mrt1_image, mrt0_desc.view_info.range,
                         mrt1_desc.view_info.range);
    ScopeMarkerEnd();
}

void Rasterizer::DepthStencilCopy(bool is_depth, bool is_stencil) {
    auto& regs = Regs();

    auto read_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), DbExtent(), false);
    auto write_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), DbExtent(), true);
    if (const u32 max_layers = instance.GetMaxImageArrayLayers();
        !PlausibleTarget(read_desc.info, max_layers, "depth copy source") ||
        !PlausibleTarget(write_desc.info, max_layers, "depth copy target")) {
        return; // gow3 (#63): garbage registers, see PlausibleTarget
    }

    auto& read_image = texture_cache.GetImage(texture_cache.FindImage(read_desc));
    auto& write_image = texture_cache.GetImage(texture_cache.FindImage(write_desc));

    VideoCore::SubresourceRange sub_range;
    sub_range.base.layer = Regs().depth_view.slice_start;
    sub_range.extent.layers = Regs().depth_view.NumSlices() - sub_range.base.layer;

    ScopeMarkerBegin(fmt::format(
        "DepthStencilCopy:DR={:#x}:SR={:#x}:DW={:#x}:SW={:#x}", regs.depth_buffer.DepthAddress(),
        regs.depth_buffer.StencilAddress(), regs.depth_buffer.DepthWriteAddress(),
        regs.depth_buffer.StencilWriteAddress()));

    runtime.CopyDepthStencil(&read_image, &write_image, sub_range);

    ScopeMarkerEnd();
}

void Rasterizer::FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds) {
    DrainDrawPipe();
    if (!is_gds) {
        Gow3WatchRange::Note("fill", value, 0, address, num_bytes);
    }
    ASSERT_MSG(address % 4 == 0 && num_bytes % 4 == 0,
               "FillBuffer address and size must be a multiple of 4 bytes");
    if (!is_gds) {
        texture_cache.ClearMeta(address);
        if (!buffer_cache.IsRegionGpuModified(address, num_bytes)) {
            u32* buffer = std::bit_cast<u32*>(address);
            std::fill(buffer, buffer + (num_bytes / sizeof(u32)), value);
            return;
        }
        // gow3: small fills into pages holding other GPU data (fence and frame labels the game
        // reads on the CPU) land in guest memory too, not only in the GPU's copy.
        if (num_bytes <= 64_KB) {
            const std::vector<u32> pattern(num_bytes / sizeof(u32), value);
            if (buffer_cache.WriteThrough(address, pattern.data(), num_bytes)) {
                return;
            }
        }
    }
    const auto [buffer, offset] = [&] -> std::pair<const VideoCore::Buffer*, u64> {
        if (is_gds) {
            return {buffer_cache.GetGdsBuffer(), address};
        }
        return buffer_cache.ObtainBuffer(address, num_bytes, true);
    }();
    runtime.FillBuffer(buffer, offset, num_bytes, value);
    // gow3: GPU-side writes into guest memory come back with this submission's readback.
    if (!is_gds && sync_gpu_constants && num_bytes <= 4096 && small_gpu_writes.size() < 16384) {
        small_gpu_writes.emplace_back(address, num_bytes);
        VideoCore::NoteReadbackKind(address, num_bytes, VideoCore::ReadbackCpWrite);
    }
}

void Rasterizer::CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds) {
    DrainDrawPipe();
    // gow3: the two GPU-modified lookups only choose the watch line's text (GOW3_WATCH_RANGE).
    if (!dst_gds && !Gow3WatchRange::Ranges().empty()) {
        Gow3WatchRange::Note(buffer_cache.IsRegionGpuModified(dst, num_bytes) ||
                                     (!src_gds && buffer_cache.IsRegionGpuModified(src, num_bytes))
                                 ? "dma copy (gpu path)"
                                 : "dma copy",
                             src, 0, dst, num_bytes);
    }
    if (!dst_gds && !buffer_cache.IsRegionGpuModified(dst, num_bytes)) {
        if (!src_gds && !buffer_cache.IsRegionGpuModified(src, num_bytes) &&
            !texture_cache.FindImageFromRange(src, num_bytes)) {
            // Both buffers were not transferred to GPU yet. Can safely copy in host memory.
            std::memcpy(std::bit_cast<void*>(dst), std::bit_cast<void*>(src), num_bytes);
            return;
        }
    }
    // gow3: a small copy whose source bytes are current in guest memory, into a page holding
    // other GPU data: made in guest memory and in the GPU's copy alike (BufferCache::WriteThrough)
    // so the CPU sees it without a download.
    if (!dst_gds && !src_gds && num_bytes <= 64_KB && dst % 4 == 0 && num_bytes % 4 == 0 &&
        IsMapped(src, num_bytes) && !buffer_cache.IsRangeGpuWritten(src, num_bytes) &&
        !texture_cache.FindImageFromRange(src, num_bytes)) {
        std::vector<u8> data(num_bytes);
        if (!memory->TryReadBacking(src, data.data(), num_bytes)) {
            std::memcpy(data.data(), std::bit_cast<const void*>(src), num_bytes);
        }
        if (buffer_cache.WriteThrough(dst, data.data(), num_bytes)) {
            return;
        }
    }
    texture_cache.InvalidateMemoryFromGPU(dst, num_bytes);
    const auto* gds_buffer = buffer_cache.GetGdsBuffer();
    const auto [src_buffer, src_offset] = [&] -> std::pair<const VideoCore::Buffer*, u64> {
        if (src_gds) {
            return {gds_buffer, src};
        }
        return buffer_cache.ObtainBuffer(src, num_bytes, false, true);
    }();
    const auto [dst_buffer, dst_offset] = [&] -> std::pair<const VideoCore::Buffer*, u64> {
        if (dst_gds) {
            return {gds_buffer, dst};
        }
        return buffer_cache.ObtainBuffer(dst, num_bytes, true, true);
    }();
    const vk::BufferCopy copy = {
        .srcOffset = src_offset,
        .dstOffset = dst_offset,
        .size = num_bytes,
    };
    runtime.CopyBuffer(src_buffer, dst_buffer, std::span{&copy, 1});
    // gow3: GPU-side copies into guest memory (GDS counters the CPU reads) come back with this
    // submission's readback, so the CPU's writes next to them wait for that readback at most.
    if (!dst_gds && sync_gpu_constants && num_bytes <= 4096 && small_gpu_writes.size() < 16384) {
        small_gpu_writes.emplace_back(dst, num_bytes);
        VideoCore::NoteReadbackKind(dst, num_bytes, VideoCore::ReadbackCpWrite);
    }
}

u32 Rasterizer::ReadDataFromGds(u32 gds_offset) {
    DrainDrawPipe();
    auto* gds_buf = buffer_cache.GetGdsBuffer();
    u32 value;
    std::memcpy(&value, gds_buf->mapped_data.data() + gds_offset, sizeof(u32));
    return value;
}

bool Rasterizer::InvalidateMemory(VAddr addr, u64 size, bool assume_locks) {
    DrainDrawPipe();
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    texture_cache.ResolveReadbacks(addr, size, assume_locks, VideoCore::ReadbackSource::CpuWrite);
    buffer_cache.InvalidateMemory(addr, size, assume_locks);
    texture_cache.InvalidateMemory(addr, size);
    return true;
}

bool Rasterizer::OnWriteFault(VAddr addr, bool assume_locks) {
    // gow3: GOW3_FAULT_STATS=0 is off, as for the other fault statistics.
    static const bool stats = [] {
        const char* env = std::getenv("GOW3_FAULT_STATS");
        return env && env[0] != '0';
    }();
    if (stats && OnStageA()) {
        static std::array<u64, 8> sites{};
        static std::map<u64, u64> windows;
        static auto last = std::chrono::steady_clock::now();
        ++sites[std::clamp(g_gpu_write_site, 0, 7)];
        ++windows[addr >> 20];
        if (const auto now = std::chrono::steady_clock::now(); now - last >= std::chrono::seconds{5}) {
            last = now;
            std::string text;
            for (const auto& [window, n] : windows) {
                text += fmt::format(" {:#x}:{}", window << 20, n);
            }
            std::printf("GPU thread write faults (5 s): WriteData %llu, inline %llu, compute %llu, DMA %llu, "
                        "fence %llu, other %llu; windows%s\n",
                        (unsigned long long)sites[1], (unsigned long long)sites[2],
                        (unsigned long long)sites[3], (unsigned long long)sites[4],
                        (unsigned long long)sites[5], (unsigned long long)sites[0], text.c_str());
            std::fflush(stdout);
            sites = {};
            windows.clear();
        }
    }
    // gow3 21 Oct 2026 diagnostics (GOW3_FAULT_TIMES=1, write-fault hitches on the laptop): how
    // long each write fault's handling takes, by step, and the slowest ones per 5 s.
    static const bool times = [] {
        const char* env = std::getenv("GOW3_FAULT_TIMES");
        return env && env[0] == '1';
    }();
    if (!times) {
        DrainDrawPipe();
        WaitDrawPipeForCapture();
        if (!InvalidateMemory(addr, 8, assume_locks)) {
            return false;
        }
        buffer_cache.ExtendWriteFault(addr);
        LandWatchedReadback(addr, assume_locks);
        return true;
    }
    // InvalidateMemory's steps, timed one by one.
    const auto t0 = std::chrono::steady_clock::now();
    DrainDrawPipe();
    WaitDrawPipeForCapture();
    if (!IsMapped(addr, 8)) {
        return false;
    }
    texture_cache.ResolveReadbacks(addr, 8, assume_locks, VideoCore::ReadbackSource::CpuWrite);
    const auto t1 = std::chrono::steady_clock::now();
    buffer_cache.InvalidateMemory(addr, 8, assume_locks);
    const auto t2 = std::chrono::steady_clock::now();
    texture_cache.InvalidateMemory(addr, 8);
    const auto t3 = std::chrono::steady_clock::now();
    buffer_cache.ExtendWriteFault(addr);
    const auto t4 = std::chrono::steady_clock::now();
    LandWatchedReadback(addr, assume_locks);
    const auto t5 = std::chrono::steady_clock::now();
    const std::array<u64, 5> steps{u64((t1 - t0).count()), u64((t2 - t1).count()),
                                   u64((t3 - t2).count()), u64((t4 - t3).count()),
                                   u64((t5 - t4).count())};
    NoteWriteFaultTime(addr, assume_locks, steps);
    return true;
}

void Rasterizer::NoteWriteFaultTime(VAddr addr, bool gpu_side, const std::array<u64, 5>& steps) {
    static constexpr u64 Limits[] = {20'000, 100'000, 1'000'000, 10'000'000, ~0ull};
    static std::atomic<u64> counts[5], sums[5], step_ns[5], gpu_side_count;
    static std::mutex slow_mutex;
    static std::vector<std::tuple<u64, VAddr, std::array<u64, 5>, bool>> slow;
    static std::atomic<s64> last_report{0};
    u64 total = 0;
    for (u32 i = 0; i < 5; ++i) {
        total += steps[i];
        step_ns[i].fetch_add(steps[i], std::memory_order_relaxed);
    }
    u32 bucket = 0;
    while (total >= Limits[bucket]) {
        ++bucket;
    }
    counts[bucket].fetch_add(1, std::memory_order_relaxed);
    sums[bucket].fetch_add(total, std::memory_order_relaxed);
    gpu_side_count.fetch_add(gpu_side, std::memory_order_relaxed);
    if (total >= 1'000'000) {
        std::scoped_lock lock{slow_mutex};
        slow.emplace_back(total, addr, steps, gpu_side);
    }
    const s64 now = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    s64 last = last_report.load(std::memory_order_relaxed);
    if (last == 0) {
        last_report.compare_exchange_strong(last, now);
        return;
    }
    if (now - last < 5 || !last_report.compare_exchange_strong(last, now)) {
        return;
    }
    u64 n[5], ns[5], all = 0, all_ns = 0;
    for (u32 i = 0; i < 5; ++i) {
        n[i] = counts[i].exchange(0);
        ns[i] = sums[i].exchange(0);
        all += n[i];
        all_ns += ns[i];
    }
    std::string text = fmt::format(
        "Write fault times (5 s): {} faults ({} GPU-side), {:.1f} ms; <20us {} {:.1f} ms, <100us {} "
        "{:.1f} ms, <1ms {} {:.1f} ms, <10ms {} {:.1f} ms, >=10ms {} {:.1f} ms; steps: image "
        "readbacks {:.1f} ms, buffers {:.1f} ms, images {:.1f} ms, extend {:.1f} ms, land {:.1f} ms",
        all, gpu_side_count.exchange(0), all_ns / 1e6, n[0], ns[0] / 1e6, n[1], ns[1] / 1e6, n[2],
        ns[2] / 1e6, n[3], ns[3] / 1e6, n[4], ns[4] / 1e6, step_ns[0].exchange(0) / 1e6,
        step_ns[1].exchange(0) / 1e6, step_ns[2].exchange(0) / 1e6, step_ns[3].exchange(0) / 1e6,
        step_ns[4].exchange(0) / 1e6);
    {
        std::scoped_lock lock{slow_mutex};
        std::ranges::sort(slow, std::greater{});
        for (size_t i = 0; i < std::min<size_t>(slow.size(), 8); ++i) {
            const auto& [t, a, s, side] = slow[i];
            text += fmt::format("\n  slow write fault {:#x}: {:.2f} ms (image readbacks {:.2f}, "
                                "buffers {:.2f}, images {:.2f}, extend {:.2f}, land {:.2f}){}",
                                a, t / 1e6, s[0] / 1e6, s[1] / 1e6, s[2] / 1e6, s[3] / 1e6,
                                s[4] / 1e6, side ? " GPU-side" : "");
        }
        slow.clear();
    }
    std::printf("%s\n", text.c_str());
    std::fflush(stdout);
}

void Rasterizer::ReleaseStuckPage(VAddr page, bool is_write, bool assume_locks) {
    DrainDrawPipe();
    buffer_cache.ReleaseStuckPage(page, is_write, assume_locks);
}

// gow3 18 Oct 2026: a fault handled while a pending readback still read-protects the page would
// fault again on the retried access (BufferCache::LandReadbacksWatching): land that readback.
void Rasterizer::LandWatchedReadback(VAddr addr, bool assume_locks) {
    if (!page_manager.IsReadWatched(addr)) {
        return;
    }
    static std::atomic<u32> landed{0};
    if (const u32 n = landed.fetch_add(1, std::memory_order_relaxed); n < 8 || (n & (n - 1)) == 0) {
        std::printf("Readback watch: a fault at %#llx left its page read-protected: landing the "
                    "readback (%u so far)\n",
                    static_cast<unsigned long long>(addr), n + 1);
        std::fflush(stdout);
    }
    const VAddr page = Common::AlignDown(addr, VAddr{4096});
    if (assume_locks) {
        buffer_cache.LandReadbacksWatching(page);
        return;
    }
    // gow3 21 Oct 2026 (GPU worker): GOW3_LAND_ON_GUEST=1 - the faulting guest thread waits for
    // the GPU itself instead of the command thread. LandReadbacksWatching waits for the readback's
    // submission on the command thread (~5 ms each on the desktop's Radeon iGPU, 20-50 per 5 s in
    // Uncharted 2): meanwhile nothing new reached the GPU, which ran dry and then idled until the
    // next submission (2.5-3.4 ms of idle GPU per 33 ms frame). Here the command thread only lands
    // what has finished and names the newest readback still holding the page; the guest thread
    // (holding no cache locks) waits for that submission, then asks again (other readbacks may
    // hold the page by then). It never waits for work not yet submitted (the command thread might
    // itself be waiting for this thread): the old path then, as after 4 rounds.
    static const bool on_guest = [] {
        const char* env = std::getenv("GOW3_LAND_ON_GUEST");
        return env && env[0] == '1';
    }();
    if (on_guest) {
        static std::atomic<u32> done_at_once{0}, waited{0}, fallback{0};
        static std::atomic<u64> wait_us{0};
        static std::atomic<s64> last_report{0};
        const auto report = [] {
            const s64 now = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
            s64 last = last_report.load(std::memory_order_relaxed);
            if (now - last >= 5 && last_report.compare_exchange_strong(last, now)) {
                std::printf("Readback watch landings (5 s): %u found landed, %u after the guest "
                            "thread waited (%.1f ms), %u on the command thread\n",
                            done_at_once.exchange(0), waited.exchange(0), wait_us.exchange(0) / 1e3,
                            fallback.exchange(0));
                std::fflush(stdout);
            }
        };
        auto* semaphore = scheduler.GetWorkSemaphore();
        bool waited_here = false;
        for (int round = 0; round < 4; ++round) {
            u64 tick = 0;
            bool submitted = false;
            liverpool->SendCommand<true>([this, page, &tick, &submitted] {
                buffer_cache.PollReadbacksNow();
                tick = buffer_cache.WatchingTick(page);
                submitted = tick != 0 && tick < scheduler.CurrentTick();
            });
            if (tick == 0) {
                if (page_manager.IsReadWatched(page)) {
                    break; // a watch no readback owns: the old path reports it
                }
                ++(waited_here ? waited : done_at_once);
                report();
                return;
            }
            if (!submitted) {
                break;
            }
            const auto start = std::chrono::steady_clock::now();
            semaphore->Wait(tick);
            wait_us += std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
            waited_here = true;
        }
        ++fallback;
        report();
    }
    liverpool->SendCommand<true>([this, page] { buffer_cache.LandReadbacksWatching(page); });
}

// gow3 18 Oct 2026: a shader capture for the GPU command thread (which waits for it) handles its
// faults inline and may land readbacks, which the draw recording thread owns while it runs: with
// multithreaded rendering both landed the same readback (staging buffer freed twice, page watcher
// count below zero, heap corruption). The command thread pushes nothing meanwhile, so the
// recording thread runs dry: the capture waits for that first (logged after 2 s).
void Rasterizer::WaitDrawPipeForCapture() {
    if (!draw_pipe || !PipelineCache::capturing_for_gpu_thread || DrawPipe::OnStageB()) {
        return;
    }
    const auto warn_at = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    bool warned = false;
    while (!draw_pipe->Idle()) {
        if (!warned && std::chrono::steady_clock::now() >= warn_at) {
            warned = true;
            static std::atomic<u32> logged{0};
            if (logged.fetch_add(1, std::memory_order_relaxed) < 4) {
                std::printf("Draw pipe: shader capture fault waiting for the recording thread "
                            "for 2 s\n");
            }
        }
        std::this_thread::yield();
    }
}

bool Rasterizer::ReadMemory(VAddr addr, u64 size, bool assume_locks) {
    DrainDrawPipe();
    WaitDrawPipeForCapture();
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    const bool image_readback =
        texture_cache.ResolveReadbacks(addr, size, assume_locks, VideoCore::ReadbackSource::CpuRead);
    if (!image_readback || buffer_cache.IsRegionGpuModified(addr, size)) {
        buffer_cache.ReadMemory(addr, size, false, assume_locks);
    }
    LandWatchedReadback(addr, assume_locks);
    return true;
}

void Rasterizer::ProcessDownloadImages() {
    DrainDrawPipe();
    texture_cache.ProcessDownloadImages();
    for (const auto& range : texture_cache.TakeTinyReadbacks()) {
        if (eop_soft_readbacks.size() < 64) {
            eop_soft_readbacks.push_back(range);
        }
    }
}

bool Rasterizer::IsMapped(VAddr addr, u64 size) {
    if (size == 0) {
        // There is no memory, so not mapped.
        return false;
    }
    if (static_cast<u64>(addr) > std::numeric_limits<u64>::max() - size) {
        // Memory range wrapped the address space, cannot be mapped.
        return false;
    }
    // gow3 17 Oct 2026: the resource-table checks ask this for every table of every draw (6% of
    // the GPU command thread in Uncharted 3): the last mapped intervals found stay valid until
    // something is unmapped.
    struct CachedInterval {
        u64 generation;
        VAddr start, end;
    };
    thread_local std::array<CachedInterval, 4> cached{};
    thread_local u32 next_slot = 0;
    const u64 generation = mapped_generation.load(std::memory_order_acquire);
    for (const auto& entry : cached) {
        if (entry.generation == generation && addr >= entry.start && addr + size <= entry.end) {
            return true;
        }
    }
    Common::RecursiveSharedLock lock{mapped_ranges_mutex};
    const auto it = mapped_ranges.find(addr);
    if (it == mapped_ranges.end() || addr + size > it->upper()) {
        return false;
    }
    cached[next_slot++ & 3] = {generation, it->lower(), it->upper()};
    return true;
}

void Rasterizer::MapMemory(VAddr addr, u64 size) {
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_ranges += decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
    }
}

void Rasterizer::RegisterMemory(VAddr addr, u64 size) {
    page_manager.OnGpuMap(addr, size);
}

} // namespace Vulkan
namespace VideoCore {
extern std::atomic<u64> g_invalidate_downloads, g_invalidate_download_ns;
}
namespace Vulkan {

void Rasterizer::UnmapMemory(VAddr addr, u64 size) {
    // gow3 19 Oct 2026: time of each part (U1: ~23 ms per 1 MiB unmap, a 1.8 s hitch before a
    // cutscene); a line when they took 20 ms or more since the last one (at most every 0.25 s).
    using Clock = std::chrono::steady_clock;
    static std::atomic<u64> unmaps{0}, drain_ns{0}, buffer_ns{0}, texture_ns{0};
    static std::atomic<s64> last_report{0};
    const auto ns = [](Clock::duration d) {
        return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count());
    };
    const auto t0 = Clock::now();
    DrainDrawPipe();
    const auto t1 = Clock::now();
    buffer_cache.InvalidateMemory(addr, size);
    const auto t2 = Clock::now();
    texture_cache.UnmapMemory(addr, size);
    const auto t3 = Clock::now();
    unmaps.fetch_add(1, std::memory_order_relaxed);
    drain_ns.fetch_add(ns(t1 - t0), std::memory_order_relaxed);
    buffer_ns.fetch_add(ns(t2 - t1), std::memory_order_relaxed);
    texture_ns.fetch_add(ns(t3 - t2), std::memory_order_relaxed);
    const s64 now = s64(ns(t3.time_since_epoch()));
    s64 last = last_report.load(std::memory_order_relaxed);
    if (now - last >= 250'000'000 &&
        drain_ns.load(std::memory_order_relaxed) + buffer_ns.load(std::memory_order_relaxed) +
                texture_ns.load(std::memory_order_relaxed) >= 20'000'000 &&
        last_report.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
        std::printf("GPU: unmap time: %llu unmaps; draw pipe %.1f ms, buffer cache %.1f ms (%llu "
                    "downloads of GPU data, %.1f ms), texture cache %.1f ms\n",
                    (unsigned long long)unmaps.exchange(0), drain_ns.exchange(0) / 1e6,
                    buffer_ns.exchange(0) / 1e6,
                    (unsigned long long)VideoCore::g_invalidate_downloads.exchange(0),
                    VideoCore::g_invalidate_download_ns.exchange(0) / 1e6,
                    texture_ns.exchange(0) / 1e6);
        std::fflush(stdout);
    }
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_ranges -= decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
        mapped_generation.fetch_add(1, std::memory_order_release);
    }
}

void Rasterizer::UpdateDynamicState(const GraphicsPipeline* pipeline, const bool is_indexed) const {
    {
        const auto* vs = pipeline ? pipeline->GetStages()[u32(Shader::SwStage::Vertex)] : nullptr;
        dynamic_vs_hash = vs ? u32(vs->pgm_hash) : 0;
        const auto* ps = pipeline ? pipeline->GetStages()[u32(Shader::SwStage::Fragment)] : nullptr;
        dynamic_ps_hash = ps ? u32(ps->pgm_hash) : 0;
    }
    UpdateViewportScissorState();
    UpdateDepthStencilState();
    UpdatePrimitiveState(is_indexed);
    UpdateRasterizationState();
    UpdateColorBlendingState(pipeline);

    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.CommitWith(instance.IsDepthBoundsSupported(),
                             instance.IsDynamicColorWriteMaskSupported(),
                             instance.IsAttachmentFeedbackLoopLayoutSupported(),
                             [&](auto&& command) { scheduler.Record(std::move(command)); });
}

void Rasterizer::UpdateViewportScissorState() const {
    const auto& regs = Regs();

    const auto combined_scissor_value_tl = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::max({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const auto combined_scissor_value_br = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::min({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const bool enable_offset = !regs.window_scissor.window_offset_disable;

    AmdGpu::Scissor scsr{};
    scsr.top_left_x = combined_scissor_value_tl(
        regs.screen_scissor.top_left_x, s16(regs.window_scissor.top_left_x),
        s16(regs.generic_scissor.top_left_x),
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.top_left_y = combined_scissor_value_tl(
        regs.screen_scissor.top_left_y, s16(regs.window_scissor.top_left_y),
        s16(regs.generic_scissor.top_left_y),
        enable_offset ? regs.window_offset.window_y_offset : 0);
    scsr.bottom_right_x = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_x, regs.window_scissor.bottom_right_x,
        regs.generic_scissor.bottom_right_x,
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.bottom_right_y = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_y, regs.window_scissor.bottom_right_y,
        regs.generic_scissor.bottom_right_y,
        enable_offset ? regs.window_offset.window_y_offset : 0);

    boost::container::static_vector<vk::Viewport, AmdGpu::NUM_VIEWPORTS> viewports;
    boost::container::static_vector<vk::Rect2D, AmdGpu::NUM_VIEWPORTS> scissors;

    if (regs.polygon_control.enable_window_offset &&
        (regs.window_offset.window_x_offset != 0 || regs.window_offset.window_y_offset != 0)) {
        LOG_ERROR(Render_Vulkan,
                  "PA_SU_SC_MODE_CNTL.VTX_WINDOW_OFFSET_ENABLE support is not yet implemented.");
    }

    const auto& vp_ctl = regs.viewport_control;
    for (u32 i = 0; i < AmdGpu::NUM_VIEWPORTS; i++) {
        const auto& vp = regs.viewports[i];
        const auto& vp_d = regs.viewport_depths[i];
        if (vp.xscale == 0) {
            continue;
        }

        const auto zoffset = vp_ctl.zoffset_enable ? vp.zoffset : 0.f;
        const auto zscale = vp_ctl.zscale_enable ? vp.zscale : 1.f;

        vk::Viewport viewport{};

        // https://gitlab.freedesktop.org/mesa/mesa/-/blob/209a0ed/src/amd/vulkan/radv_pipeline_graphics.c#L688-689
        // https://gitlab.freedesktop.org/mesa/mesa/-/blob/209a0ed/src/amd/vulkan/radv_cmd_buffer.c#L3103-3109
        // When the clip space is ranged [-1...1], the zoffset is centered.
        // By reversing the above viewport calculations, we get the following:
        if (regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW) {
            viewport.minDepth = zoffset - zscale;
            viewport.maxDepth = zoffset + zscale;
        } else {
            viewport.minDepth = zoffset;
            viewport.maxDepth = zoffset + zscale;
        }

        if (!instance.IsDepthRangeUnrestrictedSupported()) {
            // Unrestricted depth range not supported by device. Restrict to valid range.
            viewport.minDepth = std::max(viewport.minDepth, 0.f);
            viewport.maxDepth = std::min(viewport.maxDepth, 1.f);
        }

        if (regs.IsClipDisabled()) {
            // In case if clipping is disabled we patch the shader to convert vertex position
            // from screen space coordinates to NDC by defining a render space as full hardware
            // window range [0..16383, 0..16383] and setting the viewport to its size.
            viewport.x = 0.f;
            viewport.y = 0.f;
            viewport.width = float(std::min<u32>(instance.GetMaxViewportWidth(), 16_KB));
            viewport.height = float(std::min<u32>(instance.GetMaxViewportHeight(), 16_KB));
        } else {
            const auto xoffset = vp_ctl.xoffset_enable ? vp.xoffset : 0.f;
            const auto xscale = vp_ctl.xscale_enable ? vp.xscale : 1.f;
            const auto yoffset = vp_ctl.yoffset_enable ? vp.yoffset : 0.f;
            const auto yscale = vp_ctl.yscale_enable ? vp.yscale : 1.f;

            // gow3: sub-pixel jitter of scene geometry for the temporal upscaler; the same
            // shift as jittering the projection.
            viewport.x = (xoffset - xscale) * target_scale[0] + draw_jitter[0];
            viewport.y = (yoffset - yscale) * target_scale[1] + draw_jitter[1];
            viewport.width = xscale * 2.0f * target_scale[0];
            viewport.height = yscale * 2.0f * target_scale[1];
        }

        // gow3 diagnostic ("clear far" in GOW3_DMA_TRIGGER): Uncharted's shadow map clear quad
        // (vs a7fbc2a8, z = -1) writes depth 1.0 instead of the clamped 0.
        if (shadow_clear_far.load(std::memory_order_relaxed) && dynamic_vs_hash == 0xa7fbc2a8u) {
            viewport.minDepth = viewport.maxDepth = 1.0f;
        }
        viewports.push_back(viewport);

        auto vp_scsr = scsr;
        if (regs.mode_control.vport_scissor_enable) {
            vp_scsr.top_left_x =
                std::max(vp_scsr.top_left_x, s16(regs.viewport_scissors[i].top_left_x));
            vp_scsr.top_left_y =
                std::max(vp_scsr.top_left_y, s16(regs.viewport_scissors[i].top_left_y));
            vp_scsr.bottom_right_x = std::min(AmdGpu::Scissor::Clamp(vp_scsr.bottom_right_x),
                                              regs.viewport_scissors[i].bottom_right_x);
            vp_scsr.bottom_right_y = std::min(AmdGpu::Scissor::Clamp(vp_scsr.bottom_right_y),
                                              regs.viewport_scissors[i].bottom_right_y);
        }
        if (target_scale[0] != 1.0f || target_scale[1] != 1.0f) {
            const auto scale = [](s32 v, float f) { return s32(std::lround(float(v) * f)); };
            const s32 x0 = scale(vp_scsr.top_left_x, target_scale[0]);
            const s32 y0 = scale(vp_scsr.top_left_y, target_scale[1]);
            const s32 x1 = scale(vp_scsr.top_left_x + s32(vp_scsr.GetWidth()), target_scale[0]);
            const s32 y1 = scale(vp_scsr.top_left_y + s32(vp_scsr.GetHeight()), target_scale[1]);
            scissors.push_back({
                .offset = {x0, y0},
                .extent = {u32(std::max(x1 - x0, 0)), u32(std::max(y1 - y0, 0))},
            });
        } else {
            scissors.push_back({
                .offset = {vp_scsr.top_left_x, vp_scsr.top_left_y},
                .extent = {vp_scsr.GetWidth(), vp_scsr.GetHeight()},
            });
        }
    }

    if (viewports.empty()) {
        // Vulkan requires providing at least one viewport.
        constexpr vk::Viewport empty_viewport = {
            .x = -1.0f,
            .y = -1.0f,
            .width = 1.0f,
            .height = 1.0f,
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        constexpr vk::Rect2D empty_scissor = {
            .offset = {0, 0},
            .extent = {1, 1},
        };
        viewports.push_back(empty_viewport);
        scissors.push_back(empty_scissor);
    }

    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetViewports(viewports);
    dynamic_state.SetScissors(scissors);
}

// gow3: HDR10 scene (TemporalUpscaler::HdrShadow). The tone pass is drawn again with its RenoDX
// replacement into the Tone shadow; the pass after it again with the Tone shadow in place of its
// input (binding 2) into the Scene shadow, through its float-target pipeline: the game's own
// math (distortion, overlays, the 2.2 power) on the HDR values. Same constants, buffers and
// render area; the guest's own draws are untouched.
void Rasterizer::RecordHdrShadow(const GraphicsPipeline& pipeline, const RenderState& state,
                                 bool is_indexed, u32 num_indices, u32 num_instances,
                                 u32 first_vertex, u32 first_instance,
                                 const vk::DescriptorBufferInfo& settings) {
    using Shadow = TemporalUpscaler::HdrShadow;
    const auto* ps = pipeline.GetStages()[u32(Shader::SwStage::Fragment)];
    if (!ps || state.num_color_attachments != 1 || float_color0) {
        return;
    }
    Pipeline::DescriptorWrites writes = set_writes;
    vk::DescriptorImageInfo tone_info{};
    vk::Pipeline handle{};
    Shadow which{};
    if (pipeline.HdrShadowHandle()) {
        handle = pipeline.HdrShadowHandle();
        which = Shadow::Tone;
        writes.push_back({.dstBinding = 12,
                          .descriptorCount = 1,
                          .descriptorType = vk::DescriptorType::eStorageBuffer,
                          .pBufferInfo = &settings});
    } else if (upscaler->HdrPostPass(ps->pgm_hash) && pipeline.FloatTargetHandle()) {
        tone_info = {.imageView = upscaler->ToneShadowView(),
                     .imageLayout = vk::ImageLayout::eGeneral};
        bool swapped = false;
        for (auto& write : writes) {
            if (write.dstBinding == 2 && write.descriptorCount == 1 &&
                write.descriptorType == vk::DescriptorType::eSampledImage) {
                write.pImageInfo = &tone_info;
                swapped = true;
            }
        }
        if (!swapped) {
            return;
        }
        handle = pipeline.FloatTargetHandle();
        which = Shadow::Scene;
    } else {
        return;
    }
    const auto target = upscaler->HdrShadowTarget(which, state.width, state.height);
    scheduler.EndRendering();
    const auto shadow_barrier = [&](vk::ImageLayout old_layout, vk::PipelineStageFlags2 src,
                                    vk::AccessFlags2 src_access, vk::PipelineStageFlags2 dst,
                                    vk::AccessFlags2 dst_access) {
        const vk::ImageMemoryBarrier2 b{
            .srcStageMask = src,
            .srcAccessMask = src_access,
            .dstStageMask = dst,
            .dstAccessMask = dst_access,
            .oldLayout = old_layout,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = target.image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        scheduler.Record([b](vk::CommandBuffer cmdbuf) {
            cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
        });
    };
    shadow_barrier(vk::ImageLayout::eUndefined, vk::PipelineStageFlagBits2::eAllCommands,
                   vk::AccessFlagBits2::eMemoryRead, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                   vk::AccessFlagBits2::eColorAttachmentWrite);
    RenderState shadow = state;
    shadow.color_attachments[0].image_view = target.view;
    shadow.color_attachments[0].image_layout = vk::ImageLayout::eGeneral;
    shadow.color_attachments[0].is_clear = 0;
    shadow.depth_stencil_attachment.depth_clear = false;
    shadow.depth_stencil_attachment.stencil_clear = false;
    scheduler.BeginRendering(shadow);
    pipeline.BindResources(writes, push_data);
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, handle);
        if (is_indexed) {
            cmdbuf.drawIndexed(num_indices, num_instances, 0, s32(first_vertex), first_instance);
        } else {
            cmdbuf.draw(num_indices, num_instances, first_vertex, first_instance);
        }
    });
    scheduler.EndRendering();
    shadow_barrier(vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                   vk::AccessFlagBits2::eColorAttachmentWrite,
                   vk::PipelineStageFlagBits2::eAllCommands,
                   vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderStorageRead);
    upscaler->HdrShadowDrawn(which);
}

// gow3: a draw sent to the HDR10 chain's float HUD / display image whose pipeline has no
// RGBA16F variant (not one RGBA8 target, or a squaring blend): skipped instead of recorded with
// a mismatched attachment format. Named for the first few pixel shaders.
void Rasterizer::SkipFloatTargetDraw(const GraphicsPipeline& pipeline) const {
    static std::atomic<u32> skipped{0};
    if (skipped++ < 8) {
        const auto* ps = pipeline.GetStages()[u32(Shader::SwStage::Fragment)];
        std::printf("HDR: draw skipped, pixel shader %016llx has no float-target pipeline\n",
                    static_cast<unsigned long long>(ps ? ps->pgm_hash : 0));
        std::fflush(stdout);
    }
}

vk::Pipeline Rasterizer::SquarePass(const GraphicsPipeline& pipeline) const {
    const vk::Pipeline square = pipeline.SquarePassHandle();
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_SQUARE_PASS");
        return !env || env[0] != '0';
    }();
    if (!square || !enabled) [[likely]] {
        return {};
    }
    // MIN(s * s, d * d) = MIN(s, d)^2: the second draw needs the first one's coverage, which a
    // depth or stencil write would change.
    const auto& regs = Regs();
    const bool depth_write = regs.depth_control.depth_enable &&
                             regs.depth_control.depth_write_enable && regs.depth_buffer.DepthValid();
    if (depth_write || regs.depth_control.stencil_enable) {
        static bool warned = false;
        if (!std::exchange(warned, true)) {
            LOG_WARNING(Render_Vulkan, "Scaled MIN/MAX blend not squared: the draw writes depth "
                                       "or stencil");
        }
        return {};
    }
    return square;
}

// Opt-in A/B diagnostic, never a default rendering workaround. Change the file to
// 0 (guest compare), 1 (Equal -> LessOrEqual), or 2 (Equal -> Always).
// gow3 21 Oct 2026: hex shader hashes after the mode (low 32 bits, e.g. "2 8270ec0e f3d7d4eb")
// limit it to draws whose vertex or pixel shader is listed (to find one mesh's material draw).
// Also 3 (Equal -> GreaterOrEqual) and "4 <n>" (Equal -> LessOrEqual with the fragments pulled
// n depth bias units closer): how far a failing material draw is behind the stored depth.
static int g_depth_probe_bias = 0;
static int DepthCompareProbe(u32 vs_hash, u32 ps_hash) {
    static const char* path = std::getenv("GOW3_DEPTH_PROBE_FILE");
    if (!path || !path[0]) return 0;
    static std::mutex mutex;
    static std::chrono::steady_clock::time_point checked{};
    static int mode = 0;
    static std::vector<u32> only;
    std::scoped_lock lock{mutex};
    const auto now = std::chrono::steady_clock::now();
    if (now - checked > std::chrono::milliseconds(250)) {
        checked = now;
        int next = 0;
        std::vector<u32> next_only;
        if (FILE* f = std::fopen(path, "r")) {
            if (std::fscanf(f, "%d", &next) != 1 || next < 0 || next > 4) next = 0;
            int bias = 0;
            if (next == 4 && std::fscanf(f, "%d", &bias) != 1) bias = 0;
            if (bias != g_depth_probe_bias) {
                std::printf("Depth compare probe: bias %d units\n", bias);
            }
            g_depth_probe_bias = bias;
            unsigned long long value;
            while (std::fscanf(f, "%llx", &value) == 1) {
                next_only.push_back(u32(value));
            }
            std::fclose(f);
        }
        if (mode != next || only != next_only) {
            std::printf("Depth compare probe: %d (%s)\n", next,
                        next_only.empty() ? "all draws"
                                          : fmt::format("{} shaders", next_only.size()).c_str());
        }
        mode = next;
        only = std::move(next_only);
    }
    if (!only.empty() && std::ranges::find(only, vs_hash) == only.end() &&
        std::ranges::find(only, ps_hash) == only.end()) {
        return 0;
    }
    return mode;
}

void Rasterizer::UpdateDepthStencilState() const {
    const auto& regs = Regs();
    auto& dynamic_state = scheduler.GetDynamicState();

    const auto depth_stencil = GetEffectiveDepthStencilState(regs);
    const bool depth_test_enabled = depth_stencil.depth_test_enable;
    dynamic_state.SetDepthTestEnabled(depth_test_enabled);
    dynamic_state.SetDepthWriteEnabled(depth_stencil.depth_write_enable &&
                                       !regs.depth_render_control.depth_clear_enable);
    int probe = 0;
    if (depth_test_enabled) {
        auto compare = LiverpoolToVK::CompareOp(regs.depth_control.depth_func);
        if (!depth_stencil.depth_write_enable && compare == vk::CompareOp::eEqual) {
            probe = DepthCompareProbe(dynamic_vs_hash, dynamic_ps_hash);
            if (probe == 1 || probe == 4) compare = vk::CompareOp::eLessOrEqual;
            if (probe == 2) compare = vk::CompareOp::eAlways;
            if (probe == 3) compare = vk::CompareOp::eGreaterOrEqual;
        }
        dynamic_state.SetDepthCompareOp(compare);
    }

    const bool depth_bounds_test_enabled = depth_stencil.depth_bounds_enable;
    dynamic_state.SetDepthBoundsTestEnabled(depth_bounds_test_enabled);
    if (depth_bounds_test_enabled) {
        dynamic_state.SetDepthBounds(regs.depth_bounds_min, regs.depth_bounds_max);
    }

    const auto depth_bias_enabled =
        regs.polygon_control.NeedsBias() && !depth_bias_off.load(std::memory_order_relaxed);
    dynamic_state.SetDepthBiasEnabled(depth_bias_enabled);
    if (depth_bias_enabled) {
        const bool front = regs.polygon_control.enable_polygon_offset_front;
        {
            // gow3 diagnostics: each distinct polygon offset setup once (Uncharted D16 shadows).
            static std::mutex bias_mutex;
            static std::vector<std::array<u32, 6>> bias_seen;
            const std::array<u32, 6> key{std::bit_cast<u32>(regs.poly_offset.front_offset),
                                         std::bit_cast<u32>(regs.poly_offset.front_scale),
                                         std::bit_cast<u32>(regs.poly_offset.back_offset),
                                         std::bit_cast<u32>(regs.poly_offset.depth_bias),
                                         regs.poly_offset_db_fmt.raw,
                                         u32(regs.depth_buffer.z_info.format)};
            std::scoped_lock lock{bias_mutex};
            if (bias_seen.size() < 64 && std::ranges::find(bias_seen, key) == bias_seen.end()) {
                bias_seen.push_back(key);
                std::printf("GPU: depth bias front offset %g scale %g back offset %g clamp %g, "
                            "db fmt %#x, z format %u (front %u back %u)\n",
                            regs.poly_offset.front_offset, regs.poly_offset.front_scale,
                            regs.poly_offset.back_offset, regs.poly_offset.depth_bias,
                            regs.poly_offset_db_fmt.raw, u32(regs.depth_buffer.z_info.format),
                            u32(regs.polygon_control.enable_polygon_offset_front),
                            u32(regs.polygon_control.enable_polygon_offset_back));
                std::fflush(stdout);
            }
        }
        // gow3 (from the MediEvil port): the constant offset is in units of the depth format the
        // game names (PA_SU_POLY_OFFSET_DB_FMT_CNTL: 2^-16 for Z16). When the host depth buffer
        // has another precision the same number is off by a power of two (shadow acne/flicker).
        float constant_scale = 1.0f;
        if (const auto db_fmt = regs.poly_offset_db_fmt; !db_fmt.IsFloat() && db_fmt.NumDbBits() > 0) {
            static thread_local u32 cached_key = ~0u;
            static thread_local s32 cached_host_bits = 0;
            const u32 key = (u32(regs.depth_buffer.z_info.format) << 8) |
                            u32(regs.depth_buffer.stencil_info.format);
            if (key != cached_key) {
                cached_key = key;
                switch (instance.GetSupportedFormat(
                    LiverpoolToVK::DepthFormat(regs.depth_buffer.z_info.format,
                                               regs.depth_buffer.stencil_info.format),
                    vk::FormatFeatureFlagBits2::eDepthStencilAttachment)) {
                case vk::Format::eD16Unorm:
                case vk::Format::eD16UnormS8Uint:
                    cached_host_bits = 16;
                    break;
                case vk::Format::eX8D24UnormPack32:
                case vk::Format::eD24UnormS8Uint:
                case vk::Format::eD32Sfloat: // float: 2^(e-23), 2^-24 for depths in [0.5, 1)
                case vk::Format::eD32SfloatS8Uint:
                    cached_host_bits = 24;
                    break;
                default:
                    cached_host_bits = 0;
                    break;
                }
            }
            if (cached_host_bits != 0 && cached_host_bits != db_fmt.NumDbBits()) {
                constant_scale = std::ldexp(1.0f, cached_host_bits - db_fmt.NumDbBits());
                static std::atomic<bool> reported{false};
                if (!reported.exchange(true)) {
                    std::printf("GPU: depth bias in %d-bit units scaled x%g for the host depth format\n",
                                db_fmt.NumDbBits(), constant_scale);
                    std::fflush(stdout);
                }
            }
        }
        dynamic_state.SetDepthBias(
            (front ? regs.poly_offset.front_offset : regs.poly_offset.back_offset) * constant_scale,
            regs.poly_offset.depth_bias,
            (front ? regs.poly_offset.front_scale : regs.poly_offset.back_scale) / 16.f);
    }
    if (probe == 4) { // diagnostics only (DepthCompareProbe)
        dynamic_state.SetDepthBiasEnabled(true);
        dynamic_state.SetDepthBias(-float(g_depth_probe_bias), 0.0f, 0.0f);
    }

    const bool stencil_test_enabled = depth_stencil.stencil_test_enable;
    dynamic_state.SetStencilTestEnabled(stencil_test_enabled);
    if (stencil_test_enabled) {
        const StencilOps front_ops{
            .fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_front),
            .pass_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_front),
            .depth_fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_front),
            .compare_op = LiverpoolToVK::CompareOp(regs.depth_control.stencil_ref_func),
        };
        const StencilOps back_ops = regs.depth_control.backface_enable ? StencilOps{
            .fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_back),
            .pass_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_back),
            .depth_fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_back),
            .compare_op = LiverpoolToVK::CompareOp(regs.depth_control.stencil_bf_func),
        } : front_ops;
        dynamic_state.SetStencilOps(front_ops, back_ops);

        const bool stencil_clear = regs.depth_render_control.stencil_clear_enable;
        const auto front = regs.stencil_ref_front;
        const auto back =
            regs.depth_control.backface_enable ? regs.stencil_ref_back : regs.stencil_ref_front;
        // GCN REPLACE_OP writes DB_STENCILREFMASK.STENCILOPVAL, so a face whose stencil ops
        // include ReplaceOp takes its Vulkan reference from op_val.
        const auto& sc = regs.stencil_control;
        const auto uses_op_val = [](AmdGpu::StencilFunc fail, AmdGpu::StencilFunc zpass,
                                    AmdGpu::StencilFunc zfail) {
            return fail == AmdGpu::StencilFunc::ReplaceOp ||
                   zpass == AmdGpu::StencilFunc::ReplaceOp ||
                   zfail == AmdGpu::StencilFunc::ReplaceOp;
        };
        const bool front_op =
            uses_op_val(sc.stencil_fail_front, sc.stencil_zpass_front, sc.stencil_zfail_front);
        const bool back_op =
            regs.depth_control.backface_enable
                ? uses_op_val(sc.stencil_fail_back, sc.stencil_zpass_back, sc.stencil_zfail_back)
                : front_op;
        const auto ref_conflict = [](AmdGpu::CompareFunc func, const AmdGpu::StencilRefMask& ref) {
            return func != AmdGpu::CompareFunc::Always && func != AmdGpu::CompareFunc::Never &&
                   ref.stencil_test_val != ref.stencil_op_val;
        };
        if ((front_op && ref_conflict(regs.depth_control.stencil_ref_func, front)) ||
            (back_op && regs.depth_control.backface_enable &&
             ref_conflict(regs.depth_control.stencil_bf_func, back))) {
            LOG_WARNING(Render_Vulkan, "Stencil test requires test_val while ReplaceOp requires "
                                       "op_val; the stencil test will use op_val");
        }
        dynamic_state.SetStencilReferences(front_op ? front.stencil_op_val : front.stencil_test_val,
                                           back_op ? back.stencil_op_val : back.stencil_test_val);
        dynamic_state.SetStencilWriteMasks(!stencil_clear ? front.stencil_write_mask : 0U,
                                           !stencil_clear ? back.stencil_write_mask : 0U);
        dynamic_state.SetStencilCompareMasks(front.stencil_mask, back.stencil_mask);
    }
}

void Rasterizer::UpdatePrimitiveState(const bool is_indexed) const {
    const auto& regs = Regs();
    auto& dynamic_state = scheduler.GetDynamicState();

    const auto is_list_topology = [](const AmdGpu::PrimitiveType type) {
        const auto topology = LiverpoolToVK::PrimitiveType(type);
        return topology == vk::PrimitiveTopology::ePointList ||
               topology == vk::PrimitiveTopology::eLineList ||
               topology == vk::PrimitiveTopology::eTriangleList ||
               topology == vk::PrimitiveTopology::eLineListWithAdjacency ||
               topology == vk::PrimitiveTopology::eTriangleListWithAdjacency;
    };
    const auto is_patch_list_topology = [](const AmdGpu::PrimitiveType type) {
        // Quad and rect lists are emulated using tessellation.
        return type == AmdGpu::PrimitiveType::PatchPrimitive ||
               type == AmdGpu::PrimitiveType::QuadList || type == AmdGpu::PrimitiveType::RectList;
    };

    const auto prim_restart =
        (regs.enable_primitive_restart & 1) != 0 &&
        (instance.IsListRestartSupported() || !is_list_topology(regs.primitive_type)) &&
        (instance.IsPatchListRestartSupported() || !is_patch_list_topology(regs.primitive_type));
    ASSERT_MSG(!is_indexed || !prim_restart || regs.primitive_restart_index == 0xFFFF ||
                   regs.primitive_restart_index == 0xFFFFFFFF,
               "Primitive restart index other than -1 is not supported yet");

    const auto cull_mode = LiverpoolToVK::IsPrimitiveCulled(regs.primitive_type)
                               ? LiverpoolToVK::CullMode(regs.polygon_control.CullingMode())
                               : vk::CullModeFlagBits::eNone;
    const auto front_face = LiverpoolToVK::FrontFace(regs.polygon_control.front_face);

    dynamic_state.SetPrimitiveRestartEnabled(prim_restart);
    dynamic_state.SetRasterizerDiscardEnabled(regs.clipper_control.dx_rasterization_kill);
    dynamic_state.SetCullMode(cull_mode);
    dynamic_state.SetFrontFace(front_face);
}

void Rasterizer::UpdateRasterizationState() const {
    const auto& regs = Regs();
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetLineWidth(regs.line_control.Width());
}

void Rasterizer::UpdateColorBlendingState(const GraphicsPipeline* pipeline) const {
    const auto& regs = Regs();
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetBlendConstants(regs.blend_constants);
    dynamic_state.SetColorWriteMasks(pipeline->GetGraphicsKey().write_masks);
    dynamic_state.SetAttachmentFeedbackLoopEnabled(attachment_feedback_loop);
}

void Rasterizer::ScopeMarkerBegin(const std::string_view& str, bool from_guest) {
    DrainDrawPipe();
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopeMarkerEnd(bool from_guest) {
    DrainDrawPipe();
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.endDebugUtilsLabelEXT();
}

void Rasterizer::ScopedMarkerInsert(const std::string_view& str, bool from_guest) {
    DrainDrawPipe();
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopedMarkerInsertColor(const std::string_view& str, const u32 color,
                                         bool from_guest) {
    DrainDrawPipe();
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
        .color = std::array<f32, 4>(
            {(f32)((color >> 16) & 0xff) / 255.0f, (f32)((color >> 8) & 0xff) / 255.0f,
             (f32)(color & 0xff) / 255.0f, (f32)((color >> 24) & 0xff) / 255.0f})});
}

std::thread::id Rasterizer::GetGpuCommandProcessorThread() {
    return liverpool->GetGpuCommandProcessorThread();
}

#if defined(__linux__) || defined(_WIN32)
u32 Rasterizer::GetGpuCommandProcessorThreadId() {
    return liverpool->GetGpuCommandProcessorThreadId();
}
#endif

} // namespace Vulkan

namespace Vulkan {

void Rasterizer::MarkPass(const GraphicsPipeline* pipeline, const RenderState& state) {
    auto* profiler = GpuProfiler::Get();
    if (!profiler || !scheduler.WillBeginRendering(state)) {
        return;
    }
    const auto& vs = pipeline->GetStage(Shader::SwStage::Vertex);
    const auto* ps = pipeline->GetStages()[u32(Shader::SwStage::Fragment)];
    std::array<u64, AmdGpu::NUM_COLOR_BUFFERS + 6> parts{};
    parts[0] = state.width | u64(state.height) << 32;
    parts[1] = vs.pgm_hash;
    parts[2] = ps ? ps->pgm_hash : 0;
    parts[3] = db_desc.first ? u64(texture_cache.GetImage(db_desc.first).info.pixel_format) : 0;
    for (u32 cb = 0; cb < state.num_color_attachments && cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
        parts[4 + cb] = cb_descs[cb].first
                            ? u64(texture_cache.GetImage(cb_descs[cb].first).info.pixel_format) + 1
                            : 0;
    }
    const u64 key = XXH3_64bits(parts.data(), sizeof(parts));
    profiler->Mark(key, [&] {
        std::string targets;
        for (u32 cb = 0; cb < state.num_color_attachments; ++cb) {
            targets += cb_descs[cb].first
                           ? vk::to_string(texture_cache.GetImage(cb_descs[cb].first).info.pixel_format)
                           : std::string{"-"};
            targets += ' ';
        }
        return fmt::format("pass {}x{} [{}] depth {} vs {:016x} ps {:016x}", state.width,
                           state.height, targets,
                           db_desc.first ? vk::to_string(
                                               texture_cache.GetImage(db_desc.first).info.pixel_format)
                                         : std::string{"-"},
                           vs.pgm_hash, ps ? ps->pgm_hash : 0);
    });
}

void Rasterizer::NoteFrameStart() {
    if (auto* profiler = GpuProfiler::Get()) {
        profiler->BeginFrame();
    }
    const u64 frame = Gow3Stats::gpu_frames.fetch_add(1, std::memory_order_relaxed) + 1;
    // GOW3_BUFFER_STATS=1: how many earlier frames the GPU has not finished when the GPU thread
    // starts a frame — the lag that decides whether guest memory can be read in place.
    static const bool stats = [] {
        const char* value = std::getenv("GOW3_BUFFER_STATS");
        return value && value[0] == '1';
    }();
    if (!stats) {
        return;
    }
    static std::array<u64, 16> frame_ticks{};
    static std::array<u64, 17> lag_histogram{};
    u32 lag = 0;
    for (u32 back = 1; back < frame_ticks.size() && back < frame; ++back) {
        const u64 tick = frame_ticks[(frame - back) % frame_ticks.size()];
        if (tick && !scheduler.IsFree(tick)) {
            lag = back;
        }
    }
    ++lag_histogram[lag];
    frame_ticks[frame % frame_ticks.size()] = scheduler.CurrentTick();
    if (frame % 600 == 0) {
        std::printf("GPU lag at frame start (frames not finished: count):");
        for (u32 i = 0; i < lag_histogram.size(); ++i) {
            if (lag_histogram[i]) {
                std::printf(" %u:%llu", i, static_cast<unsigned long long>(lag_histogram[i]));
            }
        }
        std::printf("\n");
        lag_histogram = {};
    }
}

} // namespace Vulkan
