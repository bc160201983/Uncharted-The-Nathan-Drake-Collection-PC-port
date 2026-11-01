// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdlib>
#include <functional>
#include <unordered_set>

#include <condition_variable>
#include <deque>
#include <coroutine>
#include <exception>
#include <mutex>
#include <semaphore>
#include <span>
#include <thread>
#include <vector>
#include <queue>

#include "common/assert.h"
#include "common/slot_vector.h"
#include "common/types.h"
#include "common/unique_function.h"
#include "video_core/amdgpu/cb_db_extent.h"
#include "video_core/amdgpu/regs.h"

namespace Vulkan {
class Rasterizer;
}

namespace Libraries::VideoOut {
struct VideoOutPort;
}

namespace AmdGpu {

struct PM4CmdWaitRegMem;

union PM4Header;

struct Liverpool {
    static constexpr u32 GfxQueueId = 0u;
    static constexpr u32 NumGfxRings = 1u;     // actually 2, but HP is reserved by system software
    static constexpr u32 NumComputePipes = 7u; // actually 8, but #7 is reserved by system software
    static constexpr u32 NumQueuesPerPipe = 8u;
    static constexpr u32 NumComputeRings = NumComputePipes * NumQueuesPerPipe;
    static constexpr u32 NumTotalQueues = NumGfxRings + NumComputeRings;
    static_assert(NumTotalQueues < 64u); // need to fit into u64 bitmap for ffs

    enum ContextRegs : u32 {
        DbZInfo = 0xA010,
        CbColor0Base = 0xA318,
        CbColor1Base = 0xA327,
        CbColor2Base = 0xA336,
        CbColor3Base = 0xA345,
        CbColor4Base = 0xA354,
        CbColor5Base = 0xA363,
        CbColor6Base = 0xA372,
        CbColor7Base = 0xA381,
        CbColor0Cmask = 0xA31F,
        CbColor1Cmask = 0xA32E,
        CbColor2Cmask = 0xA33D,
        CbColor3Cmask = 0xA34C,
        CbColor4Cmask = 0xA35B,
        CbColor5Cmask = 0xA36A,
        CbColor6Cmask = 0xA379,
        CbColor7Cmask = 0xA388,
    };

    Regs regs{};
    std::array<CbDbExtent, NUM_COLOR_BUFFERS> last_cb_extent{};
    CbDbExtent last_db_extent{};
    /// gow3: register blocks written since the last draw handed to the draw recording thread
    /// (Rasterizer::PostDraw sends them along).
    RegDirty pipe_dirty;
    /// gow3: fence writes handed to the draw recording thread and not yet done there: a
    /// WaitRegMem on one of them is met in stream order without waiting for that thread.
    struct PendingFence {
        VAddr address;
        u64 data;
        u32 num_bytes;
        u64 position;
    };
    std::deque<PendingFence> pending_fences;
    void NotePendingFences(const auto& event);
    bool PendingFenceValue(VAddr address, u32& value);
    bool LandDeferredFenceFor(const PM4CmdWaitRegMem& wait, const Regs& state);
    void NotePendingWrite(const struct PM4CmdWriteData& write_data, u32 num_bytes);
    /// gow3: running checksum of graphics-register packets (see ApplyGraphicsRegisterPacket).
    u64 gfx_reg_checksum{};
    /// Top-level graphics submissions, numbered for the draw preparation workers.
    static constexpr u64 NoSeq = ~0ull;
    u64 gfx_submit_seq{};
    std::mutex gfx_submit_order_mutex; ///< gow3: SubmitGfx order (numbering, prep, queue)
    static u64 HashRegisterPacket(u64 checksum, const u32* words, u32 count);
    /// `dirty` (draw-preparation scanner) records the register blocks written.
    static void ApplyGraphicsRegisterPacket(Regs& regs, const PM4Header* header, u64& checksum,
                                            RegDirty* dirty = nullptr);

public:
    explicit Liverpool();
    ~Liverpool();

    void SubmitGfx(std::span<const u32> dcb, std::span<const u32> ccb);
    void SubmitAsc(u32 gnm_vqid, std::span<const u32> acb);

    /// gow3 18 Oct 2026: compute shader precompile (GOW3_COMPUTE_PRECOMPILE, on by default).
    /// Submitted command buffers are copied to a scanner thread, which follows the compute
    /// program registers ahead of the GPU thread and calls the hook with each dispatch's state
    /// (a program not seen before). New areas and cutscenes dispatch hundreds of compute shaders
    /// never compiled before, which the GPU thread compiled one after another (1-2 s frames).
    using PrecompileHook = std::function<void(const ComputeProgram&)>;
    void SetComputePrecompileHook(PrecompileHook hook);

    /// gow3 test (GOW3_GFX_WAIT_SUBMIT_DONE_MS): sceGnmSubmitDone calls so far.
    static inline std::atomic<u64> submit_done_gen{0};
    std::mutex gfx_submit_gen_mutex;
    std::deque<std::pair<u64, s64>> gfx_submit_gens; ///< per queued graphics submission

    void SubmitDone() noexcept {
        submit_done_gen.fetch_add(1, std::memory_order_acq_rel);
        std::scoped_lock lk{submit_mutex};
        mapped_queues[GfxQueueId].ccb_buffer_offset = 0;
        mapped_queues[GfxQueueId].dcb_buffer_offset = 0;
        submit_done = true;
        submit_cv.notify_one();
    }

    void WaitGpuIdle() noexcept {
        std::unique_lock lk{submit_mutex};
        submit_cv.wait(lk, [this] { return num_submits == 0; });
    }

    /// gow3: also the draw recording thread and the fences it deferred are done (see
    /// work_retired). sceGnmSubmitDone does not block the guest when this is true.
    bool IsGpuIdle() const {
        static const bool use_retired = [] {
            const char* env = std::getenv("GOW3_WORK_RETIRED");
            return !(env && env[0] == '0');
        }();
        return num_submits == 0 && (work_retired || !use_retired);
    }

    void SetVoPort(Libraries::VideoOut::VideoOutPort* port) {
        vo_port = port;
    }

    void BindRasterizer(Vulkan::Rasterizer* rasterizer_) {
        rasterizer = rasterizer_;
    }

    template <bool wait_done = false>
    void SendCommand(auto&& func) {
        if constexpr (wait_done) {
            std::binary_semaphore sem{0};
            {
                std::scoped_lock lk{submit_mutex};
                command_queue.emplace([&sem, &func] {
                    func();
                    sem.release();
                });
                ++num_commands;
                submit_cv.notify_one();
            }
            sem.acquire();
        } else {
            std::scoped_lock lk{submit_mutex};
            command_queue.emplace(std::move(func));
            ++num_commands;
            submit_cv.notify_one();
        }
    }

    /// gow3 21 Oct 2026 (GOW3_WAIT_SERVICES_FAULTS=1, default off): Uncharted 2's guest threads
    /// lost 20-38% of a thread in read faults (~10 ms each) whose requests waited in the command
    /// queue while the GPU command thread sat in the presenter's frames-ahead wait, which runs no
    /// commands. Fault requests (SendFaultCommand) then go into a queue of their own that
    /// ServiceQueuedCommands also runs from that wait; ProcessCommands takes them first, as
    /// before in order with the rest of each thread's requests (the sender waits for each).
    static bool WaitServicesFaults() {
        static const bool on = [] {
            const char* env = std::getenv("GOW3_WAIT_SERVICES_FAULTS");
            return env && env[0] == '1';
        }();
        return on;
    }

    /// A guest thread's fault request: SendCommand<true> unless GOW3_WAIT_SERVICES_FAULTS=1.
    void SendFaultCommand(auto&& func) {
        if (!WaitServicesFaults()) {
            SendCommand<true>(func);
            return;
        }
        std::binary_semaphore sem{0};
        {
            std::scoped_lock lk{submit_mutex};
            fault_queue.emplace([&sem, &func] {
                func();
                sem.release();
            });
            ++num_fault_commands;
            ++num_commands;
            submit_cv.notify_one();
        }
        sem.acquire();
    }

    /// GPU command thread, from waits (GOW3_WAIT_SERVICES_FAULTS=1): runs queued fault requests,
    /// not inside another command and only with the draw recording thread idle. True: some ran.
    bool ServiceQueuedCommands();

    void ReserveCopyBufferSpace() {
        GpuQueue& gfx_queue = mapped_queues[GfxQueueId];
        std::scoped_lock lk(gfx_queue.m_access);
        constexpr size_t GfxReservedSize = 2_MB >> 2;
        gfx_queue.ccb_buffer.reserve(GfxReservedSize);
        gfx_queue.dcb_buffer.reserve(GfxReservedSize);
    }

    inline ComputeProgram& GetCsRegs() {
        return mapped_queues[curr_qid].cs_state;
    }

    struct AscQueueInfo {
        static constexpr size_t Pm4BufferSize = 1024;
        VAddr map_addr;
        u32* read_addr;
        u32 ring_size_dw;
        u32 pipe_id;
        std::array<u32, Pm4BufferSize> tmp_packet;
        u32 tmp_dwords;
    };
    Common::SlotVector<AscQueueInfo> asc_queues{};

    std::thread::id GetGpuCommandProcessorThread() {
        return gpu_id;
    }

#if defined(__linux__) || defined(_WIN32)
    u32 GetGpuCommandProcessorThreadId() {
        return gpu_tid;
    }
#endif

private:
    struct Task {
        struct promise_type {
            auto get_return_object() {
                Task task{};
                task.handle = std::coroutine_handle<promise_type>::from_promise(*this);
                return task;
            }
            static constexpr std::suspend_always initial_suspend() noexcept {
                // We want the task to be suspended at start
                return {};
            }
            static constexpr std::suspend_always final_suspend() noexcept {
                return {};
            }
            void unhandled_exception() {
                try {
                    std::rethrow_exception(std::current_exception());
                } catch (const std::exception& e) {
                    UNREACHABLE_MSG("Unhandled exception: {}", e.what());
                }
            }
            void return_void() {}
            struct empty {};
            std::suspend_always yield_value(empty&&) {
                return {};
            }
        };

        using Handle = std::coroutine_handle<promise_type>;
        Handle handle;
    };

    using CmdBuffer = std::pair<std::span<const u32>, std::span<const u32>>;
    CmdBuffer CopyCmdBuffers(std::span<const u32> dcb, std::span<const u32> ccb);
    Task ProcessGraphics(std::span<const u32> dcb, std::span<const u32> ccb, u64 seq = NoSeq);
    Task ProcessCeUpdate(std::span<const u32> ccb);
    template <bool is_indirect = false>
    Task ProcessCompute(std::span<const u32> acb, u32 vqid);

    void ProcessCommands();
    void Process(std::stop_token stoken);
    void ProcessLoop(std::stop_token stoken);

    /// gow3 diagnostics (GOW3_SYNC_SUBMIT): graphics submissions queued and finished.
    std::atomic<u64> gfx_submits_queued{0};
    std::atomic<u64> gfx_submits_done{0};
    struct GpuQueue {
        std::mutex m_access{};
        std::atomic<u32> dcb_buffer_offset;
        std::atomic<u32> ccb_buffer_offset;
        std::vector<u32> dcb_buffer;
        std::vector<u32> ccb_buffer;
        std::queue<Task::Handle> submits{};
        ComputeProgram cs_state{};
    };
    std::array<GpuQueue, NumTotalQueues> mapped_queues{};
    u32 num_mapped_queues{1u}; // GFX is always available

    VAddr indirect_args_addr{};
    u32 num_counter_pairs{};
    u64 pixel_counter{};

    struct ConstantEngine {
        void Reset() {
            ce_count = 0;
            de_count = 0;
            ce_compare_count = 0;
        }

        [[nodiscard]] u32 Diff() const {
            ASSERT_MSG(ce_count >= de_count, "DE counter is ahead of CE");
            return ce_count - de_count;
        }

        u32 ce_compare_count{};
        u32 ce_count{};
        u32 de_count{};
        static std::array<u8, 48_KB> constants_heap;
    } cblock{};

    Vulkan::Rasterizer* rasterizer{};
    Libraries::VideoOut::VideoOutPort* vo_port{};
    const bool guest_markers_enabled;
    std::jthread process_thread{};
    // gow3: compute precompile scanner (SetComputePrecompileHook).
    void ScanForPrecompile(u32 queue, std::span<const u32> words);
    void PrecompileScanner(std::stop_token stop);
    void ScanPackets(u32 queue, std::span<const u32> words, int depth);
    std::mutex precompile_hook_mutex;
    PrecompileHook precompile_hook;
    std::atomic<bool> precompile_enabled{false};
    std::mutex scan_mutex;
    std::condition_variable_any scan_cv;
    std::deque<std::pair<u32, std::vector<u32>>> scan_queue;
    std::array<ComputeProgram, NumTotalQueues> scan_cs{};
    std::unordered_set<u64> scan_seen; ///< program addresses already reported
    std::jthread scan_thread{};
    std::atomic<u32> num_submits{};
    /// gow3: false from a submission until its draws and deferred fences are done (stage A
    /// decrements num_submits once it has decoded a submission, before that). Under
    /// submit_mutex with num_submits.
    std::atomic<bool> work_retired{true};
    std::atomic<u32> num_commands{};
    std::atomic<bool> submit_done{};
    std::mutex submit_mutex;
    std::condition_variable_any submit_cv;
    std::queue<Common::UniqueFunction<void>> command_queue{};
    /// gow3 21 Oct 2026 (GOW3_WAIT_SERVICES_FAULTS=1): fault requests (SendFaultCommand), under
    /// submit_mutex; counted in num_commands as well.
    std::queue<Common::UniqueFunction<void>> fault_queue{};
    std::atomic<u32> num_fault_commands{};
    u32 command_depth = 0; ///< GPU command thread: commands running (ServiceQueuedCommands)
    std::thread::id gpu_id;
#if defined(__linux__) || defined(_WIN32)
    u32 gpu_tid{};
#endif
    s32 curr_qid{-1};
};

} // namespace AmdGpu
