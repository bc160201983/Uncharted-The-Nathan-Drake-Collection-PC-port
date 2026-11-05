// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "gow3_profiler.h"
#ifdef _WIN32
#include <windows.h>
#endif
#include <array>
#include <map>
#include <string>
#include <fmt/format.h>
#include <chrono>
#include <thread>
#include <pthread.h>
#ifndef _WIN32
#include <sys/resource.h>
#endif
#include <time.h>
#include "gow3_copy.h"
#include "gow3_toggles.h"
#include "gow3_watch_range.h"
#include <cstdio>
#include <boost/preprocessor/stringize.hpp>

#include "common/assert.h"
#include "common/debug.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/libraries/kernel/process.h"
#include "core/libraries/videoout/driver.h"
#include "core/memory.h"
#include "core/platform.h"
#include "video_core/amdgpu/liverpool.h"
#include "gow3_write_log.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/renderdoc.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h" // gow3: GOW3_READBACK_WAKE (scheduler)

namespace AmdGpu {

static const char* dcb_task_name{"DCB_TASK"};
static const char* ccb_task_name{"CCB_TASK"};

#define MAX_NAMES 56
static_assert(Liverpool::NumComputeRings <= MAX_NAMES);

#define NAME_NUM(z, n, name) BOOST_PP_STRINGIZE(name) BOOST_PP_STRINGIZE(n),
#define NAME_ARRAY(name, num) {BOOST_PP_REPEAT(num, NAME_NUM, name)}

static const char* acb_task_name[] = NAME_ARRAY(ACB_TASK, MAX_NAMES);

#define YIELD(name)                                                                                \
    FIBER_EXIT;                                                                                    \
    co_yield {};                                                                                   \
    FIBER_ENTER(name);

#define YIELD_CE() YIELD(ccb_task_name)
#define YIELD_GFX() YIELD(dcb_task_name)
#define YIELD_ASC(id) YIELD(acb_task_name[id])

#define RESUME(task, name)                                                                         \
    FIBER_EXIT;                                                                                    \
    task.handle.resume();                                                                          \
    FIBER_ENTER(name);

#define RESUME_CE(task) RESUME(task, ccb_task_name)
#define RESUME_GFX(task) RESUME(task, dcb_task_name)
#define RESUME_ASC(task, id) RESUME(task, acb_task_name[id])

std::array<u8, 48_KB> Liverpool::ConstantEngine::constants_heap;

static std::span<const u32> NextPacket(std::span<const u32> span, size_t offset) {
    if (offset > span.size()) {
        LOG_ERROR(
            Lib_GnmDriver,
            ": packet length exceeds remaining submission size. Packet dword count={}, remaining "
            "submission dwords={}",
            offset, span.size());
        // Return empty subspan so check for next packet bails out
        return {};
    }

    return span.subspan(offset);
}

Liverpool::Liverpool() : guest_markers_enabled{EmulatorSettings.IsVkGuestMarkersEnabled()} {
    num_counter_pairs = Libraries::Kernel::sceKernelIsNeoMode() ? 16 : 8;
    process_thread = std::jthread{std::bind_front(&Liverpool::Process, this)};
}

Liverpool::~Liverpool() {
    SetComputePrecompileHook({});
    process_thread.request_stop();
    process_thread.join();
}

void Liverpool::SetComputePrecompileHook(PrecompileHook hook) {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_COMPUTE_PRECOMPILE");
        return !(env && env[0] == '0');
    }();
    if (hook && !enabled) {
        return;
    }
    if (hook && !scan_thread.joinable()) {
        scan_thread = std::jthread{std::bind_front(&Liverpool::PrecompileScanner, this)};
    }
    {
        // Waits for a hook call in progress (the pipeline cache goes away after clearing it).
        std::scoped_lock lock{precompile_hook_mutex};
        precompile_hook = std::move(hook);
        precompile_enabled.store(static_cast<bool>(precompile_hook), std::memory_order_release);
    }
    if (!precompile_enabled.load() && scan_thread.joinable()) {
        scan_thread.request_stop();
        scan_cv.notify_all();
        scan_thread.join();
        scan_thread = {};
    }
}

void Liverpool::ScanForPrecompile(u32 queue, std::span<const u32> words) {
    if (!precompile_enabled.load(std::memory_order_acquire) || words.empty() ||
        words.size() > (1u << 22)) {
        return;
    }
    {
        std::scoped_lock lock{scan_mutex};
        if (scan_queue.size() >= 1024) { // the scanner fell behind: this one is left out
            return;
        }
        scan_queue.emplace_back(queue, std::vector<u32>(words.begin(), words.end()));
    }
    scan_cv.notify_one();
}

void Liverpool::PrecompileScanner(std::stop_token stop) {
    Common::SetCurrentThreadName("gow3:ComputePrecompileScan");
    while (!stop.stop_requested()) {
        std::pair<u32, std::vector<u32>> item;
        {
            std::unique_lock lock{scan_mutex};
            scan_cv.wait(lock, stop, [this] { return !scan_queue.empty(); });
            if (stop.stop_requested()) {
                return;
            }
            item = std::move(scan_queue.front());
            scan_queue.pop_front();
        }
        ScanPackets(item.first, item.second, 0);
    }
}

// The compute program registers of a command buffer as the GPU thread will see them (SET_SH_REG
// into the compute range, per queue), and the dispatches. Nested buffers are read through the
// backing view (never faults); a malformed stream just ends the scan of that buffer.
void Liverpool::ScanPackets(u32 queue, std::span<const u32> words, int depth) {
    auto& cs = scan_cs[queue];
    for (size_t pos = 0; pos < words.size();) {
        const auto* header = reinterpret_cast<const PM4Header*>(&words[pos]);
        if (header->type == 2) {
            pos += 1;
            continue;
        }
        if (header->type != 3) {
            return;
        }
        const size_t count = size_t(header->type3.NumWords()) + 1;
        if (pos + count > words.size()) {
            return;
        }
        switch (header->type3.opcode) {
        case PM4ItOpcode::SetShReg: {
            const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
            const u32 offset = set_data->reg_offset;
            const size_t values = count >= 2 ? count - 2 : 0;
            if (offset >= 0x200 && (offset - 0x200) + values <= sizeof(ComputeProgram) / 4) {
                std::memcpy(reinterpret_cast<u32*>(&cs) + (offset - 0x200), &words[pos + 2],
                            values * sizeof(u32));
            }
            break;
        }
        case PM4ItOpcode::DispatchDirect:
        case PM4ItOpcode::DispatchIndirect: {
            const u64 address = cs.Address<u64>();
            if (address == 0) {
                break;
            }
            if (scan_seen.size() > 65536) {
                scan_seen.clear();
            }
            if (!scan_seen.insert(address).second) {
                break;
            }
            std::scoped_lock lock{precompile_hook_mutex};
            if (precompile_hook) {
                precompile_hook(cs);
            }
            break;
        }
        case PM4ItOpcode::IndirectBuffer: {
            if (depth >= 2) {
                break;
            }
            const auto* ib = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
            const u32 size = ib->ib_size;
            const VAddr address = reinterpret_cast<VAddr>(ib->Address<u32>());
            if (size == 0 || size > (1u << 18) || address == 0) {
                break;
            }
            std::vector<u32> nested(size);
            if (Core::Memory::Instance()->TryReadBacking(address, nested.data(), size * sizeof(u32))) {
                ScanPackets(queue, nested, depth + 1);
            }
            break;
        }
        default:
            break;
        }
        pos += count;
    }
}

void Liverpool::ProcessCommands() {
    // gow3: fences deferred until their ray-cast readbacks land are written as soon as those land
    // (checked every 64 packets: the semaphore query is a kernel call), not only per submission.
    if (rasterizer) {
        static u32 packets = 0;
        auto& cache = rasterizer->GetBufferCache();
        // With multithreaded rendering only while the draw recording thread is idle: it owns the
        // readbacks while it runs (and lands them itself, per draw).
        if (cache.HasDeferredSignals() && (++packets & 63) == 0 && rasterizer->DrawPipeIdle()) {
            cache.PollReadbacksNow();
        }
    }
    // Process incoming commands with high priority
    while (num_commands) {
        Common::UniqueFunction<void> callback{};
        {
            std::scoped_lock lk{submit_mutex};
            // gow3 21 Oct 2026 (GOW3_WAIT_SERVICES_FAULTS=1): fault requests have a queue of
            // their own (empty otherwise).
            if (!fault_queue.empty()) {
                callback = std::move(fault_queue.front());
                fault_queue.pop();
                --num_fault_commands;
            } else {
                callback = std::move(command_queue.front());
                command_queue.pop();
            }
            --num_commands;
        }
        // gow3: commands touch the caches and land readbacks, which the draw recording thread
        // owns while it runs: drained before each one (18 Oct 2026). Once before the loop let a
        // command queued after that check, or one behind a flip that woke the recording thread
        // (SubmitFlipInternal -> RunInOrder), run while it landed readbacks. One atomic load when
        // it is idle.
        if (rasterizer) {
            rasterizer->DrainDrawPipe(Vulkan::DrawPipe::ReasonCommands);
        }
        ++command_depth;
        callback();
        --command_depth;
    }
}

// gow3 21 Oct 2026 (GOW3_WAIT_SERVICES_FAULTS=1): see liverpool.h. Only fault requests (they
// read or write the caches as at any packet boundary; never PM4 work or flips), only on the GPU
// command thread, never inside a running command (a flip sent as a command waits in the
// presenter), and only while the draw recording thread is idle (nothing to drain).
bool Liverpool::ServiceQueuedCommands() {
    if (!WaitServicesFaults() || num_fault_commands.load(std::memory_order_acquire) == 0 ||
        command_depth != 0) {
        return false;
    }
#if defined(__linux__) || defined(_WIN32)
    if (static_cast<u32>(gettid()) != gpu_tid) {
        return false;
    }
#endif
    if (rasterizer && !rasterizer->DrawPipeIdle()) {
        return false;
    }
    bool serviced = false;
    for (;;) {
        Common::UniqueFunction<void> callback{};
        {
            std::scoped_lock lk{submit_mutex};
            if (fault_queue.empty()) {
                break;
            }
            callback = std::move(fault_queue.front());
            fault_queue.pop();
            --num_fault_commands;
            --num_commands;
        }
        ++command_depth;
        callback();
        --command_depth;
        serviced = true;
    }
    return serviced;
}

// gow3 19 Oct 2026 (#47): a C++ exception nothing caught on this thread (memory refused by
// Windows or the driver, for example) ended the game as "exception 0x20474343", without saying
// what failed. It is reported with its message now, in the form the launcher shows.
void Liverpool::Process(std::stop_token stoken) {
    try {
        ProcessLoop(stoken);
    } catch (const std::bad_alloc&) {
        std::fprintf(stderr, "Fault: out of memory on the GPU command thread (Windows refused an "
                             "allocation; close other programs or enlarge the page file)\n");
        std::fflush(nullptr);
        std::_Exit(3);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Fault: C++ exception on the GPU command thread: %s (%s)\n", error.what(),
                     typeid(error).name());
        std::fflush(nullptr);
        std::_Exit(3);
    }
}

// gow3 21 Oct 2026 (GPU worker): GOW3_READBACK_WAKE=1 (see the idle loop in ProcessLoop). The
// scheduler's priority thread wakes the command thread when the GPU has passed a readback; the
// target is cleared before the Liverpool goes away.
namespace {
std::mutex g_readback_wake_mutex;
Liverpool* g_readback_wake_target = nullptr; ///< guarded by g_readback_wake_mutex
bool g_readback_woken = false;               ///< guarded by the target's submit_mutex
} // namespace

void Liverpool::ProcessLoop(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuCommandProcessor");
#ifndef _WIN32
    if (clockid_t clock; pthread_getcpuclockid(pthread_self(), &clock) == 0) {
        Gow3Stats::gpu_thread_clock.store(static_cast<int>(clock));
    }
#else
    Gow3Stats::gpu_thread_id.store(GetCurrentThreadId());
    Gow3Profiler::Start(GetCurrentThreadId(), "GPU command thread");
    // gow3 18 Oct 2026 (tests): GOW3_GPU_THREAD_PRIORITY=above|high and GOW3_GPU_THREAD_CORE=<n>
    // (one logical processor) for this thread, which limits the frame rate in heavy scenes.
    if (const char* env = std::getenv("GOW3_GPU_THREAD_PRIORITY"); env && *env) {
        const std::string_view value{env};
        const int priority = value == "high" ? THREAD_PRIORITY_HIGHEST
                             : value == "above" ? THREAD_PRIORITY_ABOVE_NORMAL
                                                : THREAD_PRIORITY_NORMAL;
        SetThreadPriority(GetCurrentThread(), priority);
        std::printf("GPU command thread: priority %s\n", env);
    }
    if (const char* env = std::getenv("GOW3_GPU_THREAD_CORE"); env && *env) {
        const int core = std::atoi(env);
        if (core >= 0 && core < 64) {
            SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << core);
            std::printf("GPU command thread: logical processor %d\n", core);
        }
    }
#endif
    gpu_id = std::this_thread::get_id();
#if defined(__linux__) || defined(_WIN32)
    gpu_tid = gettid();
#endif

    // gow3 21 Oct 2026 (GPU worker): GOW3_READBACK_WAKE=1 - while fences wait for readbacks
    // and there is nothing else to do, the loop below polled them every 1 ms: the guest blocked on
    // a ray-cast fence learned of the results up to 1 ms after the GPU had them, and the GPU,
    // drained by then, idled meanwhile. It now also wakes when the GPU passes the oldest pending
    // readback (the 1 ms poll stays as the fallback).
    static const bool readback_wake = [] {
        const char* env = std::getenv("GOW3_READBACK_WAKE");
        const bool on = env && env[0] == '1';
        if (on) {
            std::printf("GPU command thread: wakes when the GPU finishes a pending readback "
                        "(GOW3_READBACK_WAKE=1)\n");
        }
        return on;
    }();
    if (readback_wake) {
        std::scoped_lock guard{g_readback_wake_mutex};
        g_readback_wake_target = this;
    }
    struct WakeTargetReset {
        bool on;
        ~WakeTargetReset() {
            if (on) {
                std::scoped_lock guard{g_readback_wake_mutex};
                g_readback_wake_target = nullptr;
            }
        }
    } wake_target_reset{readback_wake};
    u64 wake_armed_tick = 0;

    while (!stoken.stop_requested()) {
        {
            const auto idle_start = std::chrono::steady_clock::now();
            std::unique_lock lk{submit_mutex};
            // gow3: fences waiting for ray-cast readbacks are written when those land: poll for
            // them while there is nothing else to do (the guest may wait on such a fence first).
            while (rasterizer && rasterizer->GetBufferCache().HasDeferredSignals() &&
                   !(num_commands || num_submits || submit_done) && !stoken.stop_requested()) {
                lk.unlock();
                // Multithreaded rendering: the draw recording thread finishes first (it owns the
                // readbacks while it runs; nothing new reaches it while this thread is idle).
                rasterizer->DrainDrawPipe(Vulkan::DrawPipe::ReasonCommands);
                // The readback those fences wait for may still be in the open command buffer
                // (the guest waits for the fence before it submits more): send it to the GPU.
                if (rasterizer->GetBufferCache().HasUnsubmittedReadback()) {
                    rasterizer->Flush();
                }
                rasterizer->GetBufferCache().PollReadbacksNow();
                if (readback_wake) {
                    auto& scheduler = rasterizer->GetRuntime().GetScheduler();
                    const u64 tick = rasterizer->GetBufferCache().OldestPendingReadbackTick();
                    if (tick != 0 && tick != wake_armed_tick && tick < scheduler.CurrentTick()) {
                        wake_armed_tick = tick;
                        scheduler.DeferPriorityOperationAt(tick, [] {
                            std::scoped_lock guard{g_readback_wake_mutex};
                            if (Liverpool* target = g_readback_wake_target) {
                                std::scoped_lock target_lock{target->submit_mutex};
                                g_readback_woken = true;
                                target->submit_cv.notify_all();
                            }
                        });
                    }
                }
                lk.lock();
                if (rasterizer->GetBufferCache().HasDeferredSignals()) {
                    if (readback_wake) {
                        submit_cv.wait_for(lk, std::chrono::milliseconds{1}, [this] {
                            return std::exchange(g_readback_woken, false) || num_commands ||
                                   num_submits || submit_done;
                        });
                    } else {
                        submit_cv.wait_for(lk, std::chrono::milliseconds{1});
                    }
                }
            }
            Common::CondvarWait(submit_cv, lk, stoken,
                                [this] { return num_commands || num_submits || submit_done; });
            Gow3Stats::gpu_idle_ns.fetch_add(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - idle_start).count(),
                std::memory_order_relaxed);
        }
        if (stoken.stop_requested()) {
            break;
        }

        VideoCore::StartCapture();

        curr_qid = -1;

        while (num_submits || num_commands) {
            ProcessCommands();

            curr_qid = (curr_qid + 1) % num_mapped_queues;

            auto& queue = mapped_queues[curr_qid];

            Task::Handle task{};
            {
                std::scoped_lock lock{queue.m_access};
                if (queue.submits.empty()) {
                    continue;
                }
                task = queue.submits.front();
            }
            task.resume();

            if (task.done()) {
                task.destroy();
                if (curr_qid == GfxQueueId) {
                    gfx_submits_done.fetch_add(1, std::memory_order_release);
                }

                std::scoped_lock lock{queue.m_access};
                queue.submits.pop();

                --num_submits;
                std::scoped_lock lock2{submit_mutex};
                submit_cv.notify_all();
            }
        }

        if (submit_done) {
            VideoCore::EndCapture();
            if (rasterizer) {
                // GOW3_TEST_GPU_THROW=<n> (tests): a C++ exception on this thread after n
                // submissions, to check how Process reports it.
                static const long test_throw = [] {
                    const char* env = std::getenv("GOW3_TEST_GPU_THROW");
                    return env ? std::atol(env) : 0L;
                }();
                if (static long submits = 0; test_throw > 0 && ++submits == test_throw) {
                    throw std::runtime_error("test exception (GOW3_TEST_GPU_THROW)");
                }
                rasterizer->OnSubmit();
                rasterizer->Flush();
            }
            submit_done = false;
        }

        // gow3: the guest takes GPU idle (sceGnmSubmitDone) as its work being done and frees
        // the objects that hold its fence labels. The labels must be written before: the draw
        // recording thread and the fences it deferred to the Vulkan recording thread
        // (RecorderFences). Else a late fence write lands in freed memory (a corrupted guest
        // heap free list after minutes of play).
        // The same holds when sceGnmSubmitDone finds the GPU idle and does not block at all:
        // decoded (num_submits 0) is not done until then (work_retired).
        if (rasterizer) {
            rasterizer->DrainDrawPipe(Vulkan::DrawPipe::ReasonSubmissionEnd);
            rasterizer->WaitDeferredSignals();
        }
        {
            std::scoped_lock lk{submit_mutex};
            if (num_submits == 0) {
                work_retired = true;
            }
        }
        Platform::IrqC::Instance()->Signal(Platform::InterruptId::GpuIdle);
    }
}

namespace {
/// gow3 18 Oct 2026: GOW3_FEWER_DRAINS=1 (multithreaded rendering): memory waits met by a fence
/// still queued for the draw recording thread, nested compute buffers and compute memory waits
/// do not drain the draw pipe. Off by default until it has run long enough without problems.
bool FewerDrains() {
    static const bool on = [] {
        const char* env = std::getenv("GOW3_FEWER_DRAINS");
        return env && env[0] == '1';
    }();
    return on;
}

/// gow3 21 Oct 2026: GOW3_SKIP_IB_DRAIN=1 (multithreaded rendering): a nested command buffer on a
/// compute queue (INDIRECT_BUFFER) does not drain the draw pipe. The packet itself only reads the
/// nested buffer's words; every packet in it passes the drain check on its own, as nested buffers
/// on the graphics queue do (PipelinedOpcode). Uncharted 3 on the laptop: ~2,300 such
/// drains a second, the command thread waiting 10-28% of its time in them. Unlike
/// GOW3_FEWER_DRAINS, memory waits keep their drains. Off by default until measured.
bool SkipIbDrain() {
    static const bool on = [] {
        const char* env = std::getenv("GOW3_SKIP_IB_DRAIN");
        const bool value = env && env[0] == '1';
        if (value) {
            std::printf("GPU: nested compute buffers do not drain the draw pipe "
                        "(GOW3_SKIP_IB_DRAIN=1)\n");
        }
        return value;
    }();
    return on;
}

/// gow3: packets the GPU command thread handles while the draw recording thread may still
/// work on earlier draws (register state, draws it hands over, nested buffers). Every other
/// packet first waits for that thread to run dry (Rasterizer::DrainDrawPipe).
bool PipelinedOpcode(PM4ItOpcode opcode) {
    switch (opcode) {
    case PM4ItOpcode::Nop:
    case PM4ItOpcode::ContextControl:
    case PM4ItOpcode::ClearState:
    case PM4ItOpcode::SetConfigReg:
    case PM4ItOpcode::SetContextReg:
    case PM4ItOpcode::SetShReg:
    case PM4ItOpcode::SetUconfigReg:
    case PM4ItOpcode::IndexType:
    case PM4ItOpcode::NumInstances:
    case PM4ItOpcode::IndexBase:
    case PM4ItOpcode::IndexBufferSize:
    case PM4ItOpcode::SetBase:
    case PM4ItOpcode::DrawIndex2:
    case PM4ItOpcode::DrawIndexOffset2:
    case PM4ItOpcode::DrawIndexAuto:
    case PM4ItOpcode::IndirectBuffer:
    case PM4ItOpcode::AcquireMem:
    case PM4ItOpcode::PfpSyncMe:
    case PM4ItOpcode::IncrementDeCounter:
    case PM4ItOpcode::WaitOnCeCounter:
    case PM4ItOpcode::EventWrite:     // occlusion results only, no rasterizer state
    case PM4ItOpcode::EventWriteEop:  // runs in order on the recording thread (RunInOrder)
    case PM4ItOpcode::EventWriteEos:
    case PM4ItOpcode::WaitRegMem:     // waits for the recording thread only while unmet
    case PM4ItOpcode::DispatchDirect: // handed over like draws (PipelinedDispatch)
    case PM4ItOpcode::DrawIndirect:   // likewise (PipelinedIndirectDraws; else drains itself)
    case PM4ItOpcode::DrawIndirectMulti:
    case PM4ItOpcode::DrawIndexIndirect:
    case PM4ItOpcode::DrawIndexIndirectMulti:
    case PM4ItOpcode::DrawIndexIndirectCountMulti:
    case PM4ItOpcode::DispatchIndirect:
    case PM4ItOpcode::WriteData:     // run in order on the recording thread (RunInOrder)
    case PM4ItOpcode::DmaData:
    case PM4ItOpcode::SetPredication: // not implemented (ignored): nothing to wait for. God of
                                      // War III sets it around most draws; draining for it was
                                      // ~60% of the GPU thread's time (15-30k drains/s).
        return true;
    default:
        return false;
    }
}
} // namespace

namespace {
// gow3 diagnostics (GOW3_EOP_RANGE_TRACE=<lo>:<hi>[:<counter>] hex guest addresses): fence
// writes into [lo, hi) with the time, the writing thread and the 8-byte value at <counter> (e.g. the
// game's frame counter), 4000 lines at most.
// GOW3_TEST_OVERTAKE_LOG=1 (diagnostics): guest-visible writes and interrupts made while an
// earlier fence waits for its readback (BufferCache::DeferUntilLastReadback), so they reach the
// guest before it. Each kind and address once (up to 48), then counts every 5 s.
static void NoteOvertake(Vulkan::Rasterizer& rasterizer, const char* kind, u64 address) {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_TEST_OVERTAKE_LOG");
        return env && env[0] == '1';
    }();
    if (!enabled || !rasterizer.GetBufferCache().HasDeferredSignals()) {
        return;
    }
    static std::mutex mutex;
    static std::map<std::pair<std::string, u64>, u64> seen;
    static std::map<std::string, u64> counts;
    static auto last = std::chrono::steady_clock::now();
    std::scoped_lock lock{mutex};
    ++counts[kind];
    if (seen.size() < 48 && seen[{kind, address}]++ == 0) {
        std::printf("Overtake: %s at %#llx while a fence waits for its readback\n", kind,
                    static_cast<unsigned long long>(address));
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - last > std::chrono::seconds(5)) {
        last = now;
        std::string line = "Overtakes (5 s):";
        for (auto& [name, count] : counts) {
            line += fmt::format(" {} {}", name, count);
            count = 0;
        }
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
    }
}

static void TraceEopWrite(u64 address, u64 data) {
    static const std::array<u64, 3> range = [] {
        std::array<u64, 3> out{};
        const char* env = std::getenv("GOW3_EOP_RANGE_TRACE");
        for (int i = 0; env && *env && i < 3; ++i) {
            char* end = nullptr;
            out[i] = std::strtoull(env, &end, 16);
            env = end && *end == ':' ? end + 1 : nullptr;
        }
        return out;
    }();
    if (!range[1] || address < range[0] || address >= range[1]) {
        return;
    }
    static std::atomic<int> lines{0};
    if (lines.fetch_add(1) >= 4000) {
        return;
    }
    static const auto t0 = std::chrono::steady_clock::now();
    u64 counter = 0;
    if (range[2]) {
        std::memcpy(&counter, reinterpret_cast<const void*>(range[2]), sizeof(counter));
    }
    std::printf("EOP trace: t=%.2f ms label %#llx = %#llx, counter %llu, thread %lu\n",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(),
                static_cast<unsigned long long>(address), static_cast<unsigned long long>(data),
                static_cast<unsigned long long>(counter), static_cast<unsigned long>(GetCurrentThreadId()));
}

// GOW3_TEST_LATE_IRQ=<ms>:<seconds> (tests): every <seconds>, the end-of-pipe interrupts of the
// next <ms> reach the guest only at its end (in order), while the labels are written at once:
// the game's GPU event thread (ProcessGpuEvent, which sets each frame's m_gfxEopTick) woken late,
// as on a busy CPU.
static void SignalEopInterrupt() {
    static const auto late = [] {
        const char* env = std::getenv("GOW3_TEST_LATE_IRQ");
        unsigned ms = 0, seconds = 0;
        if (env && std::sscanf(env, "%u:%u", &ms, &seconds) == 2 && ms && seconds) {
            std::printf("Test: end-of-pipe interrupts held %u ms every %u s\n", ms, seconds);
        }
        return std::pair<unsigned, unsigned>{ms, seconds};
    }();
    if (late.first) {
        static std::mutex mutex;
        static u32 held = 0;
        static bool holding = false;
        static auto next = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        std::scoped_lock lock{mutex};
        if (!holding && std::chrono::steady_clock::now() >= next) {
            holding = true;
            next = std::chrono::steady_clock::now() + std::chrono::seconds(late.second);
            std::thread([ms = late.first] {
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                std::scoped_lock lock{mutex};
                for (; held; --held) {
                    Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxEop);
                }
                holding = false;
            }).detach();
        }
        if (holding) {
            ++held;
            return;
        }
    }
    Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxEop);
}

void SignalEop(const PM4CmdEventWriteEop& eop) {
    eop.SignalFence(
        [](void* address, u64 data, u32 num_bytes) {
            auto* memory = Core::Memory::Instance();
            TraceEopWrite(reinterpret_cast<u64>(address), data);
            Gow3WriteLog::Note(reinterpret_cast<u64>(address), &data, num_bytes, Gow3WriteLog::Fence);
            if (!memory->TryWriteBacking(address, &data, num_bytes)) {
                const Vulkan::GpuWriteSite site{5};
                memcpy(address, &data, num_bytes);
            }
        },
        [] { SignalEopInterrupt(); });
}

// gow3: end-of-pipe/-shader events run in order with the draws (Rasterizer::RunInOrder: on
// the draw recording thread when the draw pipeline is in use).
void RunEventWriteEop(Vulkan::Rasterizer& rasterizer, const u8* data) {
    // GOW3_TEST_CP_STALL=<ms>:<seconds> (tests): every <seconds> one end-of-pipe event waits <ms>
    // first, like a shader compile on this thread (the FrameBegin m_gfxEopTick crash reports
    // follow 40-55 ms stalls, often compiles).
    static const auto stall = [] {
        const char* env = std::getenv("GOW3_TEST_CP_STALL");
        unsigned ms = 0, seconds = 0;
        if (env && std::sscanf(env, "%u:%u", &ms, &seconds) == 2 && ms && seconds) {
            std::printf("Test: command processor stalls %u ms every %u s\n", ms, seconds);
        }
        return std::pair<unsigned, unsigned>{ms, seconds};
    }();
    if (stall.first) {
        static auto next = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        if (std::chrono::steady_clock::now() >= next) {
            next = std::chrono::steady_clock::now() + std::chrono::seconds(stall.second);
            std::this_thread::sleep_for(std::chrono::milliseconds(stall.first));
        }
    }
    const auto eop = *reinterpret_cast<const PM4CmdEventWriteEop*>(data);
    rasterizer.ProcessDownloadImages();
    const bool results_pending = rasterizer.ReadBackEopResults();
    // The label is written through the backing view: older GPU data there must not come back.
    switch (eop.data_sel.Value()) {
    case DataSelect::None:
        break;
    case DataSelect::Data32Low:
        rasterizer.NoteLabelWrite(reinterpret_cast<VAddr>(eop.Address<u32>()), 4);
        break;
    default:
        rasterizer.NoteLabelWrite(reinterpret_cast<VAddr>(eop.Address<u32>()), 8);
        break;
    }
    // Guest memory copies on the copy threads precede the fence the guest sees.
    // GOW3_ASYNC_FENCES=1 writes it once they are done instead of waiting for them; measured
    // slower (79 vs 83 FPS): the guest waits on these fences, and later fences stall it more
    // than the wait costs.
    static const bool async_fences = [] {
        const char* env = std::getenv("GOW3_ASYNC_FENCES");
        return env && env[0] == '1';
    }();
    // gow3 16 Oct: a fence after ray-cast results (and later fences to the same label) is
    // written once those results are back in guest memory: the guest reads them as soon as
    // it sees the fence. Written at once, it read the stale buffer (Uncharted 2: NPCs placed
    // from old results fell from the sky). GOW3_DEFER_RAYCAST_FENCES=0: at once, as before.
    static const bool defer = [] {
        const char* env = std::getenv("GOW3_DEFER_RAYCAST_FENCES");
        return !(env && env[0] == '0');
    }();
    auto& cache = rasterizer.GetBufferCache();
    const bool wait_readbacks = defer && (results_pending || cache.HasDeferredSignals());
    if (async_fences && !Gow3Toggle::Disabled(Gow3Toggle::AsyncFences)) {
        Gow3Copy::AfterCopies([eop] { SignalEop(eop); });
    } else if (Vulkan::DrawPipe::OnStageB() && !Gow3Toggle::Disabled(Gow3Toggle::RecorderFences) &&
               !wait_readbacks) {
        // The draw recording thread does not wait: the Vulkan recording thread signals the
        // fence after the copies queued before it. A fence after readbacks takes the path below
        // on this thread too: signalled here, it overtook its readback and Uncharted 2's NPCs
        // floated again with multithreaded rendering (16 Oct).
        rasterizer.SignalAfterHostCopies([eop] { SignalEop(eop); });
    } else {
        // Labels are written in order: not before the ones deferred to the recording thread.
        rasterizer.WaitHostCopies();
        rasterizer.WaitDeferredSignals();
        const VAddr label =
            eop.data_sel.Value() == DataSelect::None ? 0 : reinterpret_cast<VAddr>(eop.Address<u32>());
        u64 value = 0;
        u32 bytes = 0;
        if (eop.data_sel.Value() == DataSelect::Data32Low) {
            value = eop.DataDWord();
            bytes = 4;
        } else if (eop.data_sel.Value() == DataSelect::Data64) {
            value = eop.DataQWord();
            bytes = 8;
        }
        // GOW3_TEST_LATE_TIMESTAMP=<ms>:<seconds> (tests): every <seconds> one end-of-pipe GPU
        // clock timestamp is written <ms> late on another thread while later work goes
        // on (the FrameBegin m_gfxEopTick check of frame N-3).
        static const auto late = [] {
            const char* env = std::getenv("GOW3_TEST_LATE_TIMESTAMP");
            unsigned ms = 0, seconds = 0;
            if (env && std::sscanf(env, "%u:%u", &ms, &seconds) == 2 && ms && seconds) {
                std::printf("Test: a 64-bit end-of-pipe value %u ms late every %u s\n", ms, seconds);
            }
            return std::pair<unsigned, unsigned>{ms, seconds};
        }();
        bool make_late = false;
        if (late.first && eop.data_sel.Value() == DataSelect::GpuClock64) {
            static auto next = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            if (std::chrono::steady_clock::now() >= next) {
                next = std::chrono::steady_clock::now() + std::chrono::seconds(late.second);
                make_late = true;
            }
        }
        const auto signal = [eop, make_late, ms = late.first] {
            if (!make_late) {
                SignalEop(eop);
                return;
            }
            std::thread([eop, ms] {
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                SignalEop(eop);
            }).detach();
        };
        // While any fence waits for its readback, later ones queue behind it: end-of-pipe fences
        // complete in order (the guest frees command buffers of frame N once it sees frame N+1
        // done; a later fence written first let Uncharted 2 overwrite a command buffer the GPU
        // thread was still reading).
        if (wait_readbacks && cache.DeferUntilLastReadback(signal, label, value, bytes)) {
            // The GPU starts on the ray casts now rather than when the frame's command buffer is
            // submitted at its end: the fence (and the guest waiting for it) follows sooner.
            // gow3 21 Oct 2026 (GPU worker): GOW3_RAYCAST_FLUSH=lazy leaves them in the command
            // buffer: ~65% of Uncharted 2's submissions came from here (~4 a frame), and on the
            // Radeon iGPU the GPU idles ~0.4 ms at every submission boundary. When the guest
            // blocks on the fence before submitting more, the idle loop in Process flushes them
            // at once (HasUnsubmittedReadback); otherwise they go with the next submission.
            static const bool lazy_flush = [] {
                const char* env = std::getenv("GOW3_RAYCAST_FLUSH");
                return env && std::string_view{env} == "lazy";
            }();
            if (results_pending && !lazy_flush) {
                rasterizer.Flush();
            }
            return;
        }
        signal();
    }
}

// gow3: a compute queue RELEASE_MEM (fence, GDS counter store, interrupt) runs in order with the
// dispatches handed to the draw recording thread (Rasterizer::RunInOrder) instead of draining
// that thread first: Uncharted 2 releases after nearly every dispatch (~45k drains per second,
// a third of the command thread's time with multithreaded rendering).
struct ComputeRelease {
    PM4CmdReleaseMem packet;
    u32 pipe_id;
};

void RunComputeReleaseMem(Vulkan::Rasterizer& rasterizer, const u8* data) {
    alignas(ComputeRelease) u8 storage[sizeof(ComputeRelease)];
    std::memcpy(storage, data, sizeof(storage));
    const auto& release = *reinterpret_cast<const ComputeRelease*>(storage);
    const u32 pipe_id = release.pipe_id;
    // gow3 17 Oct 2026: multithreaded rendering: like end-of-pipe fences (RunEventWriteEop), the
    // Vulkan recording thread signals the fence after the copies queued before it instead of
    // this thread waiting for them (Uncharted 3: ~1,800 compute releases a frame, 16% of the
    // draw recording thread blocked on host copies at 4K).
    const auto sel = release.packet.data_sel.Value();
    if (Vulkan::DrawPipe::OnStageB() && !Gow3Toggle::Disabled(Gow3Toggle::RecorderFences) &&
        sel != DataSelect::GdsMemStore) {
        rasterizer.ProcessDownloadImages();
        const VAddr label = release.packet.Address<VAddr>();
        NoteOvertake(rasterizer, "compute release (recorder)", label);
        const bool backing = rasterizer.GetBufferCache().MayBeGpuWritten(label & ~VAddr{4095}, 4096);
        if (backing) {
            rasterizer.NoteLabelWrite(label, sel == DataSelect::Data32Low ? 4 : 8);
        }
        rasterizer.SignalAfterHostCopies([packet = release.packet, pipe_id] {
            packet.SignalFence(
                [pipe_id] {
                    Platform::IrqC::Instance()->SignalAsync(static_cast<Platform::InterruptId>(pipe_id));
                },
                [](VAddr, u16, u16) {},
                [](void* address, u64 value, u32 num_bytes) {
                    // gow3 19 Oct 2026 (GitHub #51): always through the backing view. A plain
                    // store here faulted when the label's page was write-protected after this
                    // release was recorded (GPU tracking); the fault handler then waited for the
                    // buffer cache, which the GPU command thread held while it waited for this
                    // thread: Uncharted 3 froze for good with multithreaded rendering. The
                    // record-time NoteLabelWrite above still covers labels on GPU-written pages;
                    // a page that only became so since misses it once per 64 KiB window.
                    if (!Core::Memory::Instance()->TryWriteBacking(address, &value, num_bytes)) {
                        std::memcpy(address, &value, num_bytes);
                    }
                });
        });
        return;
    }
    // Copies deferred to the recording thread precede writes the guest sees, and so do
    // graphics fences deferred to it.
    rasterizer.WaitHostCopies();
    rasterizer.WaitDeferredSignals();
    rasterizer.ProcessDownloadImages();
    NoteOvertake(rasterizer, "compute release", release.packet.Address<VAddr>());
    // gow3 17 Oct 2026: the fence value goes through the backing view like end-of-pipe labels
    // (RunEventWriteEop). A plain store faulted when the label shared a page with GPU-written
    // data, and the fault fetched that page's GPU bytes on the second queue, waiting for the GPU:
    // Uncharted 3 at 4K, ~40 a second at ~1.7 ms each on the GPU command thread.
    release.packet.SignalFence(
        [pipe_id] {
            Platform::IrqC::Instance()->SignalAsync(static_cast<Platform::InterruptId>(pipe_id));
        },
        [&rasterizer](VAddr dst, u16 gds_index, u16 num_dwords) {
            rasterizer.CopyBuffer(dst, gds_index, num_dwords * sizeof(u32), false, true);
        },
        [&rasterizer](void* address, u64 value, u32 num_bytes) {
            const VAddr label = reinterpret_cast<VAddr>(address);
            // Only where GPU data may share the page (elsewhere the store does not fault on it;
            // Uncharted 2 releases ~45k times a second, and the bookkeeping cost it 4%).
            if (rasterizer.GetBufferCache().MayBeGpuWritten(label & ~VAddr{4095}, 4096)) {
                rasterizer.NoteLabelWrite(label, num_bytes);
                Gow3WriteLog::Note(label, &value, num_bytes, Gow3WriteLog::Fence);
                if (Core::Memory::Instance()->TryWriteBacking(address, &value, num_bytes)) {
                    return;
                }
            }
            const Vulkan::GpuWriteSite site{5};
            std::memcpy(address, &value, num_bytes);
        });
}

void RunDmaData(Vulkan::Rasterizer& rasterizer, const u8* data) {
    const Vulkan::GpuWriteSite site{4};
    const auto* dma_data = reinterpret_cast<const PM4DmaData*>(data);
    ASSERT(dma_data->command.das == 0);
    // A copy or fill into guest memory can be made there at once (CopyBuffer/FillBuffer's CPU
    // paths): like WriteData, not before the copies and fences deferred to the recording thread.
    // Uncharted 2 with multithreaded rendering: a DMA-written frame marker overtook the deferred
    // end-of-pipe timestamp of that frame (assert m_gfxEopTick in FrameBegin).
    if (dma_data->dst_sel == DmaDataDst::Memory || dma_data->dst_sel == DmaDataDst::MemoryUsingL2) {
        rasterizer.WaitHostCopies();
        rasterizer.WaitDeferredSignals();
        NoteOvertake(rasterizer, "dma", dma_data->DstAddress<VAddr>());
    }
    if (dma_data->src_sel == DmaDataSrc::Data && dma_data->dst_sel == DmaDataDst::Gds) {
        rasterizer.FillBuffer(dma_data->dst_addr_lo, dma_data->NumBytes(),
                               dma_data->data, true);
    } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
               dma_data->dst_sel == DmaDataDst::Gds) {
        rasterizer.CopyBuffer(dma_data->dst_addr_lo, dma_data->SrcAddress<VAddr>(),
                               dma_data->NumBytes(), true, false);
    } else if (dma_data->src_sel == DmaDataSrc::Data &&
               (dma_data->dst_sel == DmaDataDst::Memory ||
                dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
        rasterizer.FillBuffer(dma_data->DstAddress<VAddr>(), dma_data->NumBytes(),
                               dma_data->data, false);
    } else if (dma_data->src_sel == DmaDataSrc::Gds &&
               (dma_data->dst_sel == DmaDataDst::Memory ||
                dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
        rasterizer.CopyBuffer(dma_data->DstAddress<VAddr>(), dma_data->src_addr_lo,
                               dma_data->NumBytes(), false, true);
    } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
               (dma_data->dst_sel == DmaDataDst::Memory ||
                dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
        rasterizer.CopyBuffer(dma_data->DstAddress<VAddr>(),
                               dma_data->SrcAddress<VAddr>(), dma_data->NumBytes(),
                               false, false);
    } else {
        UNREACHABLE_MSG("WriteData src_sel = {}, dst_sel = {}", u32(dma_data->src_sel),
                        u32(dma_data->dst_sel));
    }
}

void SignalFlip(Vulkan::Rasterizer& rasterizer, const u8*) {
    rasterizer.WaitDeferredSignals();
    NoteOvertake(rasterizer, "flip interrupt", 0);
    Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxFlip);
}

void RunWriteData(Vulkan::Rasterizer& rasterizer, const u8* data) {
    const auto* header = reinterpret_cast<const PM4Header*>(data);
    const auto* write_data = reinterpret_cast<const PM4CmdWriteData*>(data);
    // Copies deferred to the recording thread, and fences deferred to it (RecorderFences),
    // precede writes the guest sees.
    rasterizer.WaitHostCopies();
    rasterizer.WaitDeferredSignals();
    NoteOvertake(rasterizer, "write data", write_data->Address<u64>());
    Gow3WriteLog::Note(write_data->Address<u64>(), write_data->data,
                     (header->type3.count.Value() - 2) * sizeof(u32), Gow3WriteLog::WriteData);
    Gow3WatchRange::Note("write data", 0, 0, write_data->Address<u64>(),
                         (header->type3.count.Value() - 2) * sizeof(u32));
    const Vulkan::GpuWriteSite site{1};
    std::memcpy(write_data->Address<u64*>(), write_data->data,
                (header->type3.count.Value() - 2) * sizeof(u32));
}

void RunEventWriteEos(Vulkan::Rasterizer& rasterizer, const u8* data) {
    const auto& event_eos = *reinterpret_cast<const PM4CmdEventWriteEos*>(data);
    // Copies deferred to the recording thread precede writes the guest sees.
    rasterizer.WaitHostCopies();
    rasterizer.WaitDeferredSignals();
    rasterizer.ProcessDownloadImages();
    rasterizer.NoteLabelWrite(event_eos.Address<VAddr>(), 4);
    NoteOvertake(rasterizer, "end-of-shader label", event_eos.Address<VAddr>());
    event_eos.SignalFence([](void* address, u64 value, u32 num_bytes) {
        auto* memory = Core::Memory::Instance();
        Gow3WriteLog::Note(reinterpret_cast<u64>(address), &value, num_bytes, Gow3WriteLog::Fence);
        if (!memory->TryWriteBacking(address, &value, num_bytes)) {
            const Vulkan::GpuWriteSite site{5};
            memcpy(address, &value, num_bytes);
        }
    });
    if (event_eos.command == PM4CmdEventWriteEos::Command::GdsStore) {
        ASSERT(event_eos.size == 1);
        rasterizer.Finish();
        const u32 value = rasterizer.ReadDataFromGds(event_eos.gds_index);
        *event_eos.Address() = value;
    }
}
} // namespace

void Liverpool::NotePendingFences(const auto& event) {
    const u64 position = rasterizer->DrawPipeHead();
    while (!pending_fences.empty() && rasterizer->DrawPipeReached(pending_fences.front().position)) {
        pending_fences.pop_front();
    }
    constexpr bool is_eop = requires { event.SignalFence([](void*, u64, u32) {}, [] {}); };
    const auto note = [&](void* address, u64 data, u32 num_bytes) {
        Gow3WriteLog::NoteIntent(reinterpret_cast<u64>(address), &data, num_bytes,
                               is_eop ? Gow3WriteLog::FenceIntent : Gow3WriteLog::EosIntent);
        pending_fences.push_back({reinterpret_cast<VAddr>(address), data, num_bytes, position});
        rasterizer->NotePendingGpuWrite(reinterpret_cast<VAddr>(address), num_bytes);
    };
    if constexpr (requires { event.SignalFence(note, [] {}); }) {
        event.SignalFence(note, [] {});
    } else {
        event.SignalFence(note);
    }
}

void Liverpool::NotePendingWrite(const PM4CmdWriteData& write_data, u32 num_bytes) {
    if (num_bytes > sizeof(u64)) {
        return; // labels are small; larger writes are not waited on
    }
    u64 data = 0;
    std::memcpy(&data, write_data.data, num_bytes);
    pending_fences.push_back(
        {write_data.Address<VAddr>(), data, num_bytes, rasterizer->DrawPipeHead()});
}

// gow3: a GPU wait on a fence deferred until its ray-cast readback lands (RunEventWriteEop): the
// GPU would have written that fence before reaching the wait, so the readback lands now (its
// command buffer is submitted first if it is still open). Statistics: GOW3_FAULT_STATS lines.
bool Liverpool::LandDeferredFenceFor(const PM4CmdWaitRegMem& wait, const Regs& state) {
    if (!rasterizer || wait.mem_space.Value() != PM4CmdWaitRegMem::MemSpace::Memory ||
        wait.Test(state.reg_array)) {
        return false;
    }
    auto& cache = rasterizer->GetBufferCache();
    if (!cache.HasDeferredSignals()) {
        return false;
    }
    // Multithreaded rendering: the deferred fences belong to the draw recording thread while it
    // runs (an unmet wait drains it anyway).
    rasterizer->DrainDrawPipe(Vulkan::DrawPipe::ReasonCommands);
    u64 tick = 0;
    u32 value = 0;
    if (!cache.DeferredFenceTick(wait.Address<VAddr>(), tick, &value)) {
        return false;
    }
    // The deferred value counts as written: the GPU runs in stream order, so it would have written
    // the fence before reaching this wait. Safe because deferred fences complete in order (see
    // RunEventWriteEop). GOW3_DEFERRED_WAITS=land: the readback lands first instead (the GPU
    // thread waits for the GPU: Uncharted 2 at 42 instead of 60 FPS).
    static const bool pass = [] {
        const char* env = std::getenv("GOW3_DEFERRED_WAITS");
        return !(env && std::string_view{env} == "land");
    }();
    if (pass) {
        return wait.TestValue(value);
    }
    static std::atomic<u32> landed{0};
    static std::atomic<u64> waited_us{0};
    static auto last = std::chrono::steady_clock::now();
    const auto start = std::chrono::steady_clock::now();
    if (cache.HasUnsubmittedReadback()) {
        rasterizer->Flush();
    }
    cache.LandReadbacks(tick);
    const auto now = std::chrono::steady_clock::now();
    ++landed;
    waited_us += std::chrono::duration_cast<std::chrono::microseconds>(now - start).count();
    if (now - last >= std::chrono::seconds{5}) {
        last = now;
        const u32 n = landed.exchange(0);
        std::printf("GPU waits on deferred fences (5 s): %u, %.2f ms waiting in all\n", n,
                    waited_us.exchange(0) / 1e3);
        std::fflush(stdout);
    }
    return false;
}

bool Liverpool::PendingFenceValue(VAddr address, u32& value) {
    for (auto it = pending_fences.rbegin(); it != pending_fences.rend(); ++it) {
        if (rasterizer->DrawPipeReached(it->position)) {
            break; // written already: memory holds it (or a later write)
        }
        if (address >= it->address && address + sizeof(u32) <= it->address + it->num_bytes) {
            u64 data = it->data;
            value = static_cast<u32>(data >> ((address - it->address) * 8));
            return true;
        }
    }
    return false;
}

Liverpool::Task Liverpool::ProcessCeUpdate(std::span<const u32> ccb) {
    FIBER_ENTER(ccb_task_name);

    while (!ccb.empty()) {
        ProcessCommands();

        const auto* header = reinterpret_cast<const PM4Header*>(ccb.data());
        const u32 type = header->type;
        if (type != 3) {
            // No other types of packets were spotted so far
            UNREACHABLE_MSG("Invalid PM4 type {}", type);
        }

        const PM4ItOpcode opcode = header->type3.opcode;
        const auto* it_body = reinterpret_cast<const u32*>(header) + 1;
        switch (opcode) {
        case PM4ItOpcode::Nop: {
            // const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
            break;
        }
        case PM4ItOpcode::WriteConstRam: {
            const auto* write_const = reinterpret_cast<const PM4WriteConstRam*>(header);
            memcpy(cblock.constants_heap.data() + write_const->Offset(), &write_const->data,
                   write_const->Size());
            break;
        }
        case PM4ItOpcode::DumpConstRam: {
            // gow3: earlier draws on the recording thread may still read constants here.
            if (rasterizer) {
                rasterizer->DrainDrawPipe(Vulkan::DrawPipe::ReasonConstRam);
            }
            const auto* dump_const = reinterpret_cast<const PM4DumpConstRam*>(header);
            Gow3WriteLog::Note(reinterpret_cast<u64>(dump_const->Address<void*>()),
                               cblock.constants_heap.data() + dump_const->Offset(), dump_const->Size(),
                               Gow3WriteLog::ConstRam);
            memcpy(dump_const->Address<void*>(),
                   cblock.constants_heap.data() + dump_const->Offset(), dump_const->Size());
            break;
        }
        case PM4ItOpcode::IncrementCeCounter: {
            ++cblock.ce_count;
            break;
        }
        case PM4ItOpcode::WaitOnDeCounterDiff: {
            const auto diff = it_body[0];
            // gow3 4 Nov 2026: the CE waits while it is ahead of the DE, until the DE catches up (as
            // before). With the DE ahead of the CE the unsigned difference was a count that never
            // shrinks while the CE waits (it is the one that would have to count): the CE waited
            // for good and the DE then waited for the CE. It goes on now.
            while (cblock.ce_count > cblock.de_count &&
                   (cblock.de_count - cblock.ce_count) >= diff) {
                YIELD_CE();
            }
            break;
        }
        case PM4ItOpcode::IndirectBufferConst: {
            const auto* indirect_buffer = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
            auto task =
                ProcessCeUpdate({indirect_buffer->Address<const u32>(), indirect_buffer->ib_size});
            RESUME_CE(task);

            while (!task.handle.done()) {
                YIELD_CE();
                RESUME_CE(task);
            }
            // gow3 20 Oct 2026: a finished nested task stays suspended at final_suspend; only
            // top-level tasks were destroyed, so each indirect buffer leaked its coroutine frame
            // (host memory grew for as long as the game ran).
            task.handle.destroy();
            break;
        }
        default:
            const u32 count = header->type3.NumWords();
            UNREACHABLE_MSG("Unknown PM4 type 3 opcode {:#x} with count {}",
                            static_cast<u32>(opcode), count);
        }
        ccb = NextPacket(ccb, header->type3.NumWords() + 1);
    }

    FIBER_EXIT;
}

// gow3: graphics-register effects of a type-3 packet. The GPU thread and the draw-preparation
// workers both go through this function, and both fold every register-writing packet into a
// running checksum: equal checksums at a draw mean equal register files.
u64 Liverpool::HashRegisterPacket(u64 checksum, const u32* words, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        checksum = (checksum ^ words[i]) * 0x9E3779B97F4A7C15ull;
        checksum ^= checksum >> 29;
    }
    return checksum;
}

void Liverpool::ApplyGraphicsRegisterPacket(Regs& regs, const PM4Header* header, u64& checksum,
                                             RegDirty* dirty) {
    const u32 count = header->type3.NumWords();
    const auto* payload = reinterpret_cast<const u32*>(header + 2);
    const auto mark = [&](u32 word, u32 words) {
        if (dirty) {
            dirty->Mark(word, words);
        }
    };
    const auto mark_field = [&](const auto& field) {
        if (dirty) {
            dirty->MarkField(regs, field);
        }
    };
    switch (header->type3.opcode) {
    case PM4ItOpcode::ClearState:
        regs.SetDefaults();
        if (dirty) {
            dirty->Clear();
            dirty->reset = true;
        }
        break;
    case PM4ItOpcode::SetConfigReg: {
        const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
        std::memcpy(&regs.reg_array[Regs::ConfigRegWordOffset + set_data->reg_offset], payload,
                    (count - 1) * sizeof(u32));
        mark(Regs::ConfigRegWordOffset + set_data->reg_offset, count - 1);
        break;
    }
    case PM4ItOpcode::SetContextReg: {
        const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
        std::memcpy(&regs.reg_array[Regs::ContextRegWordOffset + set_data->reg_offset], payload,
                    (count - 1) * sizeof(u32));
        mark(Regs::ContextRegWordOffset + set_data->reg_offset, count - 1);
        break;
    }
    case PM4ItOpcode::SetShReg: {
        const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
        // The compute program range goes to the queue's cs_state, not to regs.
        if (!(set_data->reg_offset >= 0x200 &&
              set_data->reg_offset <= (0x200 + sizeof(ComputeProgram) / 4))) {
            std::memcpy(&regs.reg_array[Regs::ShRegWordOffset + set_data->reg_offset], payload,
                        (count - 1) * sizeof(u32));
            mark(Regs::ShRegWordOffset + set_data->reg_offset, count - 1);
        }
        break;
    }
    case PM4ItOpcode::SetUconfigReg: {
        const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
        std::memcpy(&regs.reg_array[Regs::UconfigRegWordOffset + set_data->reg_offset], payload,
                    (count - 1) * sizeof(u32));
        mark(Regs::UconfigRegWordOffset + set_data->reg_offset, count - 1);
        break;
    }
    case PM4ItOpcode::IndexType:
        regs.index_buffer_type.raw = reinterpret_cast<const PM4CmdDrawIndexType*>(header)->raw;
        mark_field(regs.index_buffer_type);
        break;
    case PM4ItOpcode::DrawIndex2: {
        const auto* draw_index = reinterpret_cast<const PM4CmdDrawIndex2*>(header);
        regs.max_index_size = draw_index->max_size;
        regs.index_base_address.base_addr_lo = draw_index->index_base_lo;
        regs.index_base_address.base_addr_hi = draw_index->index_base_hi;
        regs.num_indices = draw_index->index_count;
        mark_field(regs.max_index_size);
        mark_field(regs.index_base_address);
        mark_field(regs.num_indices);
        regs.draw_initiator = draw_index->draw_initiator;
        mark_field(regs.draw_initiator);
        break;
    }
    case PM4ItOpcode::DrawIndexOffset2: {
        const auto* draw_index_off = reinterpret_cast<const PM4CmdDrawIndexOffset2*>(header);
        regs.max_index_size = draw_index_off->max_size;
        regs.num_indices = draw_index_off->index_count;
        regs.draw_initiator = draw_index_off->draw_initiator;
        mark_field(regs.max_index_size);
        mark_field(regs.num_indices);
        mark_field(regs.draw_initiator);
        break;
    }
    case PM4ItOpcode::DrawIndexAuto: {
        const auto* draw_index = reinterpret_cast<const PM4CmdDrawIndexAuto*>(header);
        mark_field(regs.num_indices);
        mark_field(regs.draw_initiator);
        regs.num_indices = draw_index->index_count;
        regs.draw_initiator = draw_index->draw_initiator;
        break;
    }
    case PM4ItOpcode::NumInstances:
        regs.num_instances.num_instances =
            reinterpret_cast<const PM4CmdDrawNumInstances*>(header)->num_instances;
        mark_field(regs.num_instances);
        break;
    case PM4ItOpcode::IndexBase: {
        const auto* index_base = reinterpret_cast<const PM4CmdDrawIndexBase*>(header);
        regs.index_base_address.base_addr_lo = index_base->addr_lo;
        regs.index_base_address.base_addr_hi = index_base->addr_hi;
        mark_field(regs.index_base_address);
        break;
    }
    case PM4ItOpcode::IndexBufferSize:
        regs.num_indices = reinterpret_cast<const PM4CmdDrawIndexBufferSize*>(header)->num_indices;
        mark_field(regs.num_indices);
        break;
    case PM4ItOpcode::EventWrite: {
        const auto* event = reinterpret_cast<const PM4CmdEventWrite*>(header);
        if (event->event_type.Value() != EventType::SoVgtStreamoutFlush) {
            return;
        }
        // TODO: handle proper synchronization, for now signal that update is done immediately
        regs.cp_strmout_cntl.offset_update_done = 1;
        mark_field(regs.cp_strmout_cntl);
        break;
    }
    default:
        return;
    }
    checksum = HashRegisterPacket(checksum, reinterpret_cast<const u32*>(header), count + 1);
}

// gow3: GOW3_DCB_STATS=1 — structure of graphics command buffers (read-only scan, printed every
// 5 s): how many buffers, draws per buffer, state set before the first draw. Input for
// processing command buffers on several threads.
namespace {
struct DcbStats {
    u64 buffers[2]{}, with_draws[2]{}, draws[2]{}, dispatches[2]{}, max_draws[2]{};
    u64 clear_first[2]{}, ctx_before[2]{}, sh_before[2]{}, ibs[2]{}, dwords[2]{};
    u64 hist[2][6]{}; // draws per buffer: 0, 1-9, 10-99, 100-499, 500-1999, 2000+
    std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
};
DcbStats g_dcb_stats;
int g_dcb_depth;

bool IsDrawOpcode(PM4ItOpcode op) {
    switch (op) {
    case PM4ItOpcode::DrawIndex2:
    case PM4ItOpcode::DrawIndexOffset2:
    case PM4ItOpcode::DrawIndexAuto:
    case PM4ItOpcode::DrawIndirect:
    case PM4ItOpcode::DrawIndirectMulti:
    case PM4ItOpcode::DrawIndexIndirect:
    case PM4ItOpcode::DrawIndexIndirectMulti:
    case PM4ItOpcode::DrawIndexIndirectCountMulti:
        return true;
    default:
        return false;
    }
}

void ScanDcb(std::span<const u32> dcb, int depth) {
    auto& st = g_dcb_stats;
    const int d = depth > 0 ? 1 : 0;
    u64 draws = 0, dispatches = 0, ctx = 0, sh = 0, ibs = 0;
    bool clear_first = false;
    st.dwords[d] += dcb.size();
    for (size_t at = 0; at < dcb.size();) {
        const auto* header = reinterpret_cast<const PM4Header*>(dcb.data() + at);
        if (header->type == 2) {
            ++at;
            continue;
        }
        if (header->type != 3) {
            break;
        }
        const PM4ItOpcode op = header->type3.opcode;
        if (IsDrawOpcode(op)) {
            ++draws;
        } else if (op == PM4ItOpcode::DispatchDirect || op == PM4ItOpcode::DispatchIndirect) {
            ++dispatches;
        } else if (op == PM4ItOpcode::IndirectBuffer) {
            ++ibs;
        } else if (draws == 0) {
            if (op == PM4ItOpcode::ClearState) {
                clear_first = true;
            } else if (op == PM4ItOpcode::SetContextReg) {
                ctx += header->type3.NumWords() - 1;
            } else if (op == PM4ItOpcode::SetShReg) {
                sh += header->type3.NumWords() - 1;
            }
        }
        at += header->type3.NumWords() + 1;
    }
    ++st.buffers[d];
    st.draws[d] += draws;
    st.dispatches[d] += dispatches;
    st.ibs[d] += ibs;
    st.max_draws[d] = std::max(st.max_draws[d], draws);
    if (draws) {
        ++st.with_draws[d];
        st.clear_first[d] += clear_first;
        st.ctx_before[d] += ctx;
        st.sh_before[d] += sh;
    }
    const int bucket = draws == 0 ? 0 : draws < 10 ? 1 : draws < 100 ? 2 : draws < 500 ? 3
                     : draws < 2000 ? 4 : 5;
    ++st.hist[d][bucket];

    const auto now = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(now - st.window).count();
    if (depth == 0 && secs >= 5.0) {
        for (int k = 0; k < 2; ++k) {
            const double with = std::max<u64>(st.with_draws[k], 1);
            std::printf("DCB stats %s: %.0f buffers/s (%.0f with draws), %.0f draws/s, %.0f "
                        "dispatches/s, max %llu draws in one buffer, %.0f IBs/s, %.0f kdw/s; "
                        "draw buffers: %.0f%% start with ClearState, %.0f ctx + %.0f sh reg "
                        "writes before first draw; draws/buffer hist 0:%llu 1-9:%llu 10-99:%llu "
                        "100-499:%llu 500-1999:%llu 2000+:%llu\n",
                        k ? "indirect" : "submitted", st.buffers[k] / secs, st.with_draws[k] / secs,
                        st.draws[k] / secs, st.dispatches[k] / secs,
                        static_cast<unsigned long long>(st.max_draws[k]), st.ibs[k] / secs,
                        st.dwords[k] / secs / 1000.0, 100.0 * st.clear_first[k] / with,
                        st.ctx_before[k] / with, st.sh_before[k] / with,
                        static_cast<unsigned long long>(st.hist[k][0]),
                        static_cast<unsigned long long>(st.hist[k][1]),
                        static_cast<unsigned long long>(st.hist[k][2]),
                        static_cast<unsigned long long>(st.hist[k][3]),
                        static_cast<unsigned long long>(st.hist[k][4]),
                        static_cast<unsigned long long>(st.hist[k][5]));
        }
        st = DcbStats{};
    }
}
} // namespace

// gow3 test (GOW3_GFX_START_DELAY_US): a graphics submission starts at least this long after the
// guest submitted it, like a PS4 GPU still busy with the previous frame.
static std::atomic<s64> g_last_gfx_submit_ns{0};
static s64 SteadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

Liverpool::Task Liverpool::ProcessGraphics(std::span<const u32> dcb, std::span<const u32> ccb,
                                           u64 seq) {
    FIBER_ENTER(dcb_task_name);
    static const s64 start_delay_ns = [] {
        const char* env = std::getenv("GOW3_GFX_START_DELAY_US");
        return env ? std::atoll(env) * 1000 : 0;
    }();
    // gow3 test (GOW3_GFX_WAIT_SUBMIT_DONE_MS): a graphics submission starts once the guest
    // finished the frame it belongs to (sceGnmSubmitDone), at most this long after it was submitted
    // (a PS4 GPU still busy with the previous frame reads the frame's data late).
    static const s64 wait_done_ns = [] {
        const char* env = std::getenv("GOW3_GFX_WAIT_SUBMIT_DONE_MS");
        return env ? std::atoll(env) * 1000000 : 0;
    }();
    if (wait_done_ns > 0) {
        u64 gen = 0;
        s64 submitted = 0;
        {
            std::scoped_lock lock{gfx_submit_gen_mutex};
            if (!gfx_submit_gens.empty()) {
                std::tie(gen, submitted) = gfx_submit_gens.front();
                gfx_submit_gens.pop_front();
            }
        }
        while (submit_done_gen.load(std::memory_order_acquire) <= gen &&
               SteadyNs() < submitted + wait_done_ns) {
            std::this_thread::sleep_for(std::chrono::microseconds{100});
        }
    }
    if (start_delay_ns > 0) {
        const s64 until = g_last_gfx_submit_ns.load(std::memory_order_acquire) + start_delay_ns;
        for (s64 now = SteadyNs(); now < until; now = SteadyNs()) {
            std::this_thread::sleep_for(std::chrono::nanoseconds{std::min<s64>(until - now, 200000)});
        }
    }
    // Top-level buffers enqueued for the draw preparation workers (nested IBs are not).
    Vulkan::DrawPreparation* draw_prep =
        seq != NoSeq && rasterizer ? &rasterizer->GetDrawPreparation() : nullptr;
    if (draw_prep) {
        draw_prep->BeginSubmission(seq, regs, gfx_reg_checksum);
    }
    static const bool dcb_stats = EmulatorSettingsImpl::Flag("GOW3_DCB_STATS", false);
    const int dcb_depth = g_dcb_depth;
    if (dcb_stats) {
        ScanDcb(dcb, dcb_depth);
    }

    cblock.Reset();

    // TODO: potentially, ASCs also can depend on CE and in this case the
    // CE task should be moved into more global scope
    Task ce_task{};

    if (!ccb.empty()) {
        // In case of CCB provided kick off CE asap to have the constant heap ready to use
        ce_task = ProcessCeUpdate(ccb);
        RESUME_GFX(ce_task);
    }

    const auto base_addr = reinterpret_cast<uintptr_t>(dcb.data());
    while (!dcb.empty()) {
        ProcessCommands();

        const auto* header = reinterpret_cast<const PM4Header*>(dcb.data());
        const u32 type = header->type;

        switch (type) {
        default:
            UNREACHABLE_MSG("Wrong PM4 type {}", type);
            break;
        case 0: {
            const VAddr at = reinterpret_cast<VAddr>(header);
            std::printf("Invalid packet at %#llx: offset %#llx in a command buffer at %#llx of %zu dwords "
                        "left (depth %d)\n",
                        static_cast<unsigned long long>(at),
                        static_cast<unsigned long long>(at - base_addr),
                        static_cast<unsigned long long>(base_addr), dcb.size(), dcb_depth);
            if (rasterizer) {
                auto& cache = rasterizer->GetBufferCache();
                std::printf("  GPU-written: %d, readback in flight: tick %llu, current tick %llu\n",
                            int(cache.IsRangeGpuWritten(at, 64)),
                            static_cast<unsigned long long>(cache.PendingReadbackTickFor(at, 64)),
                            0ULL);
            }
            const u32* words = reinterpret_cast<const u32*>(base_addr);
            std::printf("  first dwords:");
            for (int i = 0; i < 8; ++i) {
                std::printf(" %08x", words[i]);
            }
            std::printf("\n  at the packet:");
            for (int i = 0; i < 8 && size_t(i) < dcb.size(); ++i) {
                std::printf(" %08x", dcb[i]);
            }
            std::printf("\n");
            std::fflush(stdout);
            UNREACHABLE_MSG("Unimplemented PM4 type 0, base reg: {}, size: {}",
                            header->type0.base.Value(), header->type0.NumWords());
            break;
        }
        case 2:
            // Type-2 packet are used for padding purposes
            dcb = NextPacket(dcb, 1);
            continue;
        case 3:
            const u32 count = header->type3.NumWords();
            const PM4ItOpcode opcode = header->type3.opcode;
            ApplyGraphicsRegisterPacket(regs, header, gfx_reg_checksum, &pipe_dirty);
            // DmaData to 0x3022C does nothing here (skipped below): no need to wait.
            if (rasterizer && !PipelinedOpcode(opcode) &&
                !(opcode == PM4ItOpcode::DmaData &&
                  reinterpret_cast<const PM4DmaData*>(header)->dst_addr_lo == 0x3022C)) {
                rasterizer->DrainDrawPipe(static_cast<u32>(opcode));
                // They may write guest memory: not before fences deferred earlier.
                rasterizer->WaitDeferredSignals();
            }
            switch (opcode) {
            case PM4ItOpcode::Nop: {
                const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
                if (nop->header.count.Value() == 0) {
                    break;
                }

                switch (nop->data_block[0]) {
                case PM4CmdNop::PayloadType::PatchedFlip: {
                    // There is no evidence that GPU CP drives flip events by parsing
                    // special NOP packets. For convenience lets assume that it does.
                    // gow3: after the writes before it (the buffer label: WriteData, which
                    // may still be queued on the draw recording thread).
                    if (rasterizer) {
                        rasterizer->RunInOrder(&SignalFlip, header, sizeof(u32),
                                               Gow3Toggle::PipelinedMemoryWrites);
                    } else {
                        Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxFlip);
                    }
                    break;
                }
                case PM4CmdNop::PayloadType::DebugMarkerPush: {
                    if (guest_markers_enabled) {
                        const auto marker_sz = nop->header.count.Value() * 2;
                        const std::string_view label{
                            reinterpret_cast<const char*>(&nop->data_block[1]), marker_sz};
                        rasterizer->ScopeMarkerBegin(label, true);
                    }
                    break;
                }
                case PM4CmdNop::PayloadType::DebugColorMarkerPush: {
                    if (guest_markers_enabled) {
                        const auto marker_sz = nop->header.count.Value() * 2;
                        const std::string_view label{
                            reinterpret_cast<const char*>(&nop->data_block[1]), marker_sz};
                        const u32 color = *reinterpret_cast<const u32*>(
                            reinterpret_cast<const u8*>(&nop->data_block[1]) + marker_sz);
                        rasterizer->ScopedMarkerInsertColor(label, color, true);
                    }
                    break;
                }
                case PM4CmdNop::PayloadType::DebugMarkerPop: {
                    if (guest_markers_enabled) {
                        rasterizer->ScopeMarkerEnd(true);
                    }
                    break;
                }
                default:
                    break;
                }
                break;
            }
            case PM4ItOpcode::ContextControl: {
                break;
            }
            case PM4ItOpcode::ClearState:
            case PM4ItOpcode::SetConfigReg: {
                break; // registers: ApplyGraphicsRegisterPacket
            }
            case PM4ItOpcode::SetContextReg: {
                const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
                const auto reg_addr = Regs::ContextRegWordOffset + set_data->reg_offset;
                const auto* payload = reinterpret_cast<const u32*>(header + 2);

                // In the case of HW, render target memory has alignment as color block operates on
                // tiles. There is no information of actual resource extents stored in CB context
                // regs, so any deduction of it from slices/pitch will lead to a larger surface
                // created. The same applies to the depth targets. Fortunately, the guest always
                // sends a trailing NOP packet right after the context regs setup, so we can use the
                // heuristic below and extract the hint to determine actual resource dims.

                switch (reg_addr) {
                case ContextRegs::CbColor0Base:
                case ContextRegs::CbColor1Base:
                case ContextRegs::CbColor2Base:
                case ContextRegs::CbColor3Base:
                case ContextRegs::CbColor4Base:
                case ContextRegs::CbColor5Base:
                case ContextRegs::CbColor6Base:
                case ContextRegs::CbColor7Base: {
                    const auto col_buf_id = (reg_addr - ContextRegs::CbColor0Base) /
                                            (ContextRegs::CbColor1Base - ContextRegs::CbColor0Base);
                    ASSERT(col_buf_id < NUM_COLOR_BUFFERS);

                    const auto nop_offset = header->type3.count;
                    if (nop_offset == 0x0e || nop_offset == 0x0d || nop_offset == 0x0b) {
                        ASSERT_MSG(payload[nop_offset] == 0xc0001000,
                                   "NOP hint is missing in CB setup sequence");
                        last_cb_extent[col_buf_id].raw = payload[nop_offset + 1];
                    } else {
                        last_cb_extent[col_buf_id].raw = 0;
                    }
                    break;
                }
                case ContextRegs::CbColor0Cmask:
                case ContextRegs::CbColor1Cmask:
                case ContextRegs::CbColor2Cmask:
                case ContextRegs::CbColor3Cmask:
                case ContextRegs::CbColor4Cmask:
                case ContextRegs::CbColor5Cmask:
                case ContextRegs::CbColor6Cmask:
                case ContextRegs::CbColor7Cmask: {
                    const auto col_buf_id =
                        (reg_addr - ContextRegs::CbColor0Cmask) /
                        (ContextRegs::CbColor1Cmask - ContextRegs::CbColor0Cmask);
                    ASSERT(col_buf_id < NUM_COLOR_BUFFERS);

                    const auto nop_offset = header->type3.count;
                    if (nop_offset == 0x04) {
                        ASSERT_MSG(payload[nop_offset] == 0xc0001000,
                                   "NOP hint is missing in CB setup sequence");
                        last_cb_extent[col_buf_id].raw = payload[nop_offset + 1];
                    }
                    break;
                }
                case ContextRegs::DbZInfo: {
                    if (header->type3.count == 8) {
                        ASSERT_MSG(payload[20] == 0xc0001000,
                                   "NOP hint is missing in DB setup sequence");
                        last_db_extent.raw = payload[21];
                    } else {
                        last_db_extent.raw = 0;
                    }
                    break;
                }
                default:
                    break;
                }
                break;
            }
            case PM4ItOpcode::SetShReg: {
                const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
                const auto set_size = (count - 1) * sizeof(u32);

                if (set_data->reg_offset >= 0x200 &&
                    set_data->reg_offset <= (0x200 + sizeof(ComputeProgram) / 4)) {
                    ASSERT(set_size <= sizeof(ComputeProgram));
                    auto* addr = reinterpret_cast<u32*>(&mapped_queues[GfxQueueId].cs_state) +
                                 (set_data->reg_offset - 0x200);
                    std::memcpy(addr, header + 2, set_size);
                } // other SH registers: ApplyGraphicsRegisterPacket
                break;
            }
            case PM4ItOpcode::SetUconfigReg: {
                break; // registers: ApplyGraphicsRegisterPacket
            }
            case PM4ItOpcode::SetPredication: {
                // gow3: once; God of War III sets it around most draws (~1000 lines a frame).
                static std::once_flag predication_logged;
                std::call_once(predication_logged,
                               [] { LOG_WARNING(Render, "Unimplemented IT_SET_PREDICATION"); });
                break;
            }
            case PM4ItOpcode::IndexType: {
                break; // registers: ApplyGraphicsRegisterPacket
            }
            case PM4ItOpcode::DrawIndex2: {
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                const auto* prepared = draw_prep ? draw_prep->NextDraw() : nullptr;
                rasterizer->ScopeMarker("gfx:{}:DrawIndex2", fmt::make_format_args(cmd_address),
                                        [&] { rasterizer->Draw(true, 0, prepared); });
                break;
            }
            case PM4ItOpcode::DrawIndexOffset2: {
                const auto* draw_index_off =
                    reinterpret_cast<const PM4CmdDrawIndexOffset2*>(header);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                const auto* prepared = draw_prep ? draw_prep->NextDraw() : nullptr;
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndexOffset2", fmt::make_format_args(cmd_address),
                    [&] { rasterizer->Draw(true, draw_index_off->index_offset, prepared); });
                break;
            }
            case PM4ItOpcode::DrawIndexAuto: {
                const auto* draw_index = reinterpret_cast<const PM4CmdDrawIndexAuto*>(header);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                const auto* prepared = draw_prep ? draw_prep->NextDraw() : nullptr;
                rasterizer->ScopeMarker("gfx:{}:DrawIndexAuto", fmt::make_format_args(cmd_address),
                                        [&] { rasterizer->Draw(false, 0, prepared); });
                break;
            }
            case PM4ItOpcode::DrawIndirect: {
                const auto* draw_indirect = reinterpret_cast<const PM4CmdDrawIndirect*>(header);
                const auto offset = draw_indirect->data_offset;
                const auto stride = sizeof(DrawIndirectArgs);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndirect", fmt::make_format_args(cmd_address), [&] {
                        rasterizer->DrawIndirect(false, indirect_args_addr, offset, stride, 1, 0,
                                                 draw_indirect->base_vtx_loc,
                                                 draw_indirect->start_inst_loc);
                    });
                break;
            }
            case PM4ItOpcode::DrawIndirectMulti: {
                const auto* draw_indirect =
                    reinterpret_cast<const PM4CmdDrawIndirectMulti*>(header);
                const auto offset = draw_indirect->data_offset;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndirectMulti", fmt::make_format_args(cmd_address), [&] {
                        rasterizer->DrawIndirect(false, indirect_args_addr, offset,
                                                 draw_indirect->stride, draw_indirect->count, 0,
                                                 draw_indirect->base_vtx_loc,
                                                 draw_indirect->start_inst_loc);
                    });
                break;
            }
            case PM4ItOpcode::DrawIndexIndirect: {
                const auto* draw_index_indirect =
                    reinterpret_cast<const PM4CmdDrawIndexIndirect*>(header);
                const auto offset = draw_index_indirect->data_offset;
                const auto stride = sizeof(DrawIndexedIndirectArgs);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndexIndirect", fmt::make_format_args(cmd_address), [&] {
                        rasterizer->DrawIndirect(true, indirect_args_addr, offset, stride, 1, 0,
                                                 draw_index_indirect->base_vtx_loc,
                                                 draw_index_indirect->start_inst_loc);
                    });
                break;
            }
            case PM4ItOpcode::DrawIndexIndirectMulti: {
                const auto* draw_index_indirect =
                    reinterpret_cast<const PM4CmdDrawIndexIndirectMulti*>(header);
                const auto offset = draw_index_indirect->data_offset;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndexIndirectMulti", fmt::make_format_args(cmd_address), [&] {
                        rasterizer->DrawIndirect(
                            true, indirect_args_addr, offset, draw_index_indirect->stride,
                            draw_index_indirect->count, 0, draw_index_indirect->base_vtx_loc,
                            draw_index_indirect->start_inst_loc);
                    });
                break;
            }
            case PM4ItOpcode::DrawIndexIndirectCountMulti: {
                const auto* draw_index_indirect =
                    reinterpret_cast<const PM4CmdDrawIndexIndirectCountMulti*>(header);
                const auto offset = draw_index_indirect->data_offset;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndexIndirectCountMulti", fmt::make_format_args(cmd_address), [&] {
                        rasterizer->DrawIndirect(
                            true, indirect_args_addr, offset, draw_index_indirect->stride,
                            draw_index_indirect->count,
                            draw_index_indirect->count_indirect_enable.Value()
                                ? draw_index_indirect->count_addr
                                : 0,
                            draw_index_indirect->base_vtx_loc, draw_index_indirect->start_inst_loc);
                    });
                break;
            }
            case PM4ItOpcode::DispatchDirect: {
                const auto* dispatch_direct = reinterpret_cast<const PM4CmdDispatchDirect*>(header);
                auto& cs_program = GetCsRegs();
                cs_program.dim_x = dispatch_direct->dim_x;
                cs_program.dim_y = dispatch_direct->dim_y;
                cs_program.dim_z = dispatch_direct->dim_z;
                cs_program.dispatch_initiator = dispatch_direct->dispatch_initiator;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                                   cs_program);
                }
                if (!rasterizer || (cs_program.dispatch_initiator & 1) == 0) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker("gfx:{}:DispatchDirect", fmt::make_format_args(cmd_address),
                                        [&] { rasterizer->DispatchDirect(); });
                break;
            }
            case PM4ItOpcode::DispatchIndirect: {
                const auto* dispatch_indirect =
                    reinterpret_cast<const PM4CmdDispatchIndirect*>(header);
                auto& cs_program = GetCsRegs();
                const auto offset = dispatch_indirect->data_offset;
                const auto size = sizeof(PM4CmdDispatchIndirect::GroupDimensions);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                                   cs_program);
                }
                if (!rasterizer || (cs_program.dispatch_initiator & 1) == 0) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DispatchIndirect", fmt::make_format_args(cmd_address),
                    [&] { rasterizer->DispatchIndirect(indirect_args_addr, offset, size); });
                break;
            }
            case PM4ItOpcode::NumInstances:
            case PM4ItOpcode::IndexBase:
            case PM4ItOpcode::IndexBufferSize: {
                break; // registers: ApplyGraphicsRegisterPacket
            }
            case PM4ItOpcode::SetBase: {
                const auto* set_base = reinterpret_cast<const PM4CmdSetBase*>(header);
                ASSERT(set_base->base_index == PM4CmdSetBase::BaseIndex::DrawIndexIndirPatchTable);
                indirect_args_addr = set_base->Address<u64>();
                break;
            }
            case PM4ItOpcode::EventWrite: {
                const auto* event = reinterpret_cast<const PM4CmdEventWrite*>(header);
                LOG_DEBUG(Render, "Encountered EventWrite: event_type = {}, event_index = {}",
                          magic_enum::enum_name(event->event_type.Value()),
                          magic_enum::enum_name(event->event_index.Value()));
                if (event->event_type.Value() == EventType::SoVgtStreamoutFlush) {
                    // registers: ApplyGraphicsRegisterPacket
                } else if (event->event_index.Value() == EventIndex::ZpassDone) {
                    if (event->event_type.Value() == EventType::PixelPipeStatDump) {
                        static constexpr u64 OcclusionCounterValidMask = 0x8000000000000000ULL;
                        static constexpr u64 OcclusionCounterStep = 0x2FFFFFFULL;
                        u64* results = event->Address<u64*>();
                        for (s32 i = 0; i < num_counter_pairs; ++i, results += 2) {
                            *results = pixel_counter | OcclusionCounterValidMask;
                        }
                        pixel_counter += OcclusionCounterStep;
                    }
                }
                break;
            }
            case PM4ItOpcode::EventWriteEos: {
                const auto* event_eos = reinterpret_cast<const PM4CmdEventWriteEos*>(header);
                if (rasterizer &&
                    rasterizer->RunInOrder(&RunEventWriteEos, event_eos, sizeof(*event_eos))) {
                    NotePendingFences(*event_eos);
                }
                break;
            }
            case PM4ItOpcode::EventWriteEop: {
                const auto* event_eop = reinterpret_cast<const PM4CmdEventWriteEop*>(header);
                if (rasterizer) {
                    if (rasterizer->RunInOrder(&RunEventWriteEop, event_eop, sizeof(*event_eop))) {
                        NotePendingFences(*event_eop);
                    }
                } else {
                    SignalEop(*event_eop);
                }
                break;
            }
            case PM4ItOpcode::DmaData: {
                const auto* dma_data = reinterpret_cast<const PM4DmaData*>(header);
                if (dma_data->dst_addr_lo == 0x3022C || !rasterizer) {
                    break;
                }
                if (rasterizer->RunInOrder(&RunDmaData, dma_data, sizeof(PM4DmaData),
                                           Gow3Toggle::PipelinedMemoryWrites) &&
                    (dma_data->dst_sel == DmaDataDst::Memory ||
                     dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                    rasterizer->NotePendingGpuWrite(dma_data->DstAddress<VAddr>(),
                                                    dma_data->NumBytes());
                }
                break;
            }
            case PM4ItOpcode::WriteData: {
                const auto* write_data = reinterpret_cast<const PM4CmdWriteData*>(header);
                ASSERT(write_data->dst_sel.Value() == 2 || write_data->dst_sel.Value() == 5);
                ASSERT(!write_data->wr_one_addr.Value());
                if (rasterizer) {
                    // In order with the draws (on the draw recording thread when in use).
                    Gow3WriteLog::NoteIntent(write_data->Address<u64>(), write_data->data,
                                           (count - 2) * sizeof(u32),
                                           Gow3WriteLog::WriteDataIntent);
                    if (rasterizer->RunInOrder(&RunWriteData, header, (count + 1) * sizeof(u32),
                                               Gow3Toggle::PipelinedMemoryWrites)) {
                        NotePendingWrite(*write_data, (count - 2) * sizeof(u32));
                        rasterizer->NotePendingGpuWrite(write_data->Address<VAddr>(),
                                                        (count - 2) * sizeof(u32));
                    }
                } else {
                    const Vulkan::GpuWriteSite site{2};
                    std::memcpy(write_data->Address<u64*>(), write_data->data, (count - 2) * 4);
                }
                break;
            }
            case PM4ItOpcode::CopyData: {
                const auto* copy_data = reinterpret_cast<const PM4CmdCopyData*>(header);
                LOG_WARNING(Render,
                            "unhandled IT_COPY_DATA src_sel = {}, dst_sel = {}, "
                            "count_sel = {}, wr_confirm = {}, engine_sel = {}",
                            u32(copy_data->src_sel.Value()), u32(copy_data->dst_sel.Value()),
                            copy_data->count_sel.Value(), copy_data->wr_confirm.Value(),
                            u32(copy_data->engine_sel.Value()));
                break;
            }
            case PM4ItOpcode::MemSemaphore: {
                const auto* mem_semaphore = reinterpret_cast<const PM4CmdMemSemaphore*>(header);
                if (mem_semaphore->IsSignaling()) {
                    mem_semaphore->Signal();
                } else {
                    while (!mem_semaphore->Signaled()) {
                        YIELD_GFX();
                    }
                    mem_semaphore->Decrement();
                }
                break;
            }
            case PM4ItOpcode::AcquireMem: {
                // const auto* acquire_mem = reinterpret_cast<PM4CmdAcquireMem*>(header);
                break;
            }
            case PM4ItOpcode::Rewind: {
                if (!rasterizer) {
                    break;
                }
                const PM4CmdRewind* rewind = reinterpret_cast<const PM4CmdRewind*>(header);
                while (!rewind->Valid()) {
                    YIELD_GFX();
                }
                break;
            }
            case PM4ItOpcode::WaitRegMem: {
                const auto* wait_reg_mem = reinterpret_cast<const PM4CmdWaitRegMem*>(header);
                // ASSERT(wait_reg_mem->engine.Value() == PM4CmdWaitRegMem::Engine::Me);
                // gow3 diagnostics (GOW3_WAIT_TRACE=1): graphics waits on memory, 5 s statistics
                // by address window and whether they were met at once.
                static const bool wait_trace = [] {
                    const char* env = std::getenv("GOW3_WAIT_TRACE");
                    return env && env[0] == '1';
                }();
                if (wait_trace && wait_reg_mem->mem_space.Value() == PM4CmdWaitRegMem::MemSpace::Memory) {
                    static std::map<u64, std::array<u64, 2>> waits; // 1 MiB window -> (waits, unmet)
                    static auto last = std::chrono::steady_clock::now();
                    static int lines = 0;
                    const bool met = wait_reg_mem->Test(regs.reg_array);
                    auto& w = waits[wait_reg_mem->Address<VAddr>() >> 20];
                    ++w[0];
                    w[1] += !met;
                    if (lines < 40) {
                        ++lines;
                        std::printf("Wait trace: gfx wait %#llx ref %#x mask %#x func %u, value now %#x: %s\n",
                                    static_cast<unsigned long long>(wait_reg_mem->Address<VAddr>()),
                                    wait_reg_mem->ref, wait_reg_mem->mask,
                                    unsigned(wait_reg_mem->function.Value()),
                                    *wait_reg_mem->Address<u32*>(), met ? "met" : "waits");
                    }
                    if (const auto now = std::chrono::steady_clock::now(); now - last >= std::chrono::seconds{5}) {
                        last = now;
                        std::string text;
                        for (const auto& [window, n] : waits) {
                            text += fmt::format(" {:#x}: {} ({} unmet)", window << 20, n[0], n[1]);
                        }
                        std::printf("Wait trace (5 s): gfx memory waits by window:%s\n", text.c_str());
                        std::fflush(stdout);
                        waits.clear();
                    }
                }
                // Optimization: VO label waits are special because the emulator
                // will write to the label when presentation is finished. So if
                // there are no other submits to yield to we can sleep the thread
                // instead and allow other tasks to run.
                const u64* wait_addr = wait_reg_mem->Address<u64*>();
                if (vo_port->IsVoLabel(wait_addr) &&
                    num_submits == mapped_queues[GfxQueueId].submits.size()) {
                    if (rasterizer) {
                        rasterizer->DrainDrawPipe(static_cast<u32>(opcode));
                    }
                    vo_port->WaitVoLabel([&] { return wait_reg_mem->Test(regs.reg_array); });
                    break;
                }
                // gow3: a fence handed to the draw recording thread counts as written: that
                // thread runs everything in stream order.
                // 18 Oct 2026 (GOW3_FEWER_DRAINS=1): checked before LandDeferredFenceFor, which
                // drains the pipe first (8.7% of the U3 command thread on a Ryzen laptop). Off by
                // default: fewer drains made a readback race of multithreaded rendering frequent
                // (see Rasterizer::WaitDrawPipeForCapture).
                const auto pending_fence_met = [&] {
                    u32 value;
                    return wait_reg_mem->mem_space.Value() == PM4CmdWaitRegMem::MemSpace::Memory &&
                           !Gow3Toggle::Disabled(Gow3Toggle::PendingFenceWaits) &&
                           PendingFenceValue(wait_reg_mem->Address<VAddr>(), value) &&
                           wait_reg_mem->TestValue(value);
                };
                if (FewerDrains() && pending_fence_met()) {
                    break;
                }
                if (LandDeferredFenceFor(*wait_reg_mem, regs)) {
                    break;
                }
                if (pending_fence_met()) {
                    break;
                }
                // Else the value may come from a fence that thread has yet to write: let it
                // catch up before yielding to other queues.
                // gow3 diagnostics (GOW3_WAIT_TRACE=1): how long unmet waits take and which values
                // the label held meanwhile (a short pulse to the awaited value missed between two
                // checks would show as a wait of a whole frame).
                static const bool wait_durations = std::getenv("GOW3_WAIT_TRACE") != nullptr;
                const auto wait_start = std::chrono::steady_clock::now();
                u32 seen[4]{};
                u32 num_seen = 0, polls = 0;
                const bool memory_wait =
                    wait_reg_mem->mem_space.Value() == PM4CmdWaitRegMem::MemSpace::Memory;
                while (!wait_reg_mem->Test(regs.reg_array)) {
                    if (wait_durations && memory_wait) {
                        ++polls;
                        const u32 now_value = *wait_reg_mem->Address<u32*>();
                        if (num_seen == 0 || (seen[num_seen - 1] != now_value && num_seen < 4)) {
                            seen[num_seen++] = now_value;
                        }
                    }
                    if (rasterizer && !rasterizer->DrawPipeIdle()) {
                        rasterizer->DrainDrawPipe(static_cast<u32>(opcode));
                        continue;
                    }
                    YIELD_GFX();
                }
                if (wait_durations && memory_wait && polls) {
                    static std::mutex mutex;
                    static std::array<u64, 5> buckets{};
                    static int long_lines = 0;
                    static auto last = std::chrono::steady_clock::now();
                    const double ms = std::chrono::duration<double, std::milli>(
                                          std::chrono::steady_clock::now() - wait_start).count();
                    std::scoped_lock lock{mutex};
                    ++buckets[ms < 1 ? 0 : ms < 5 ? 1 : ms < 12 ? 2 : ms < 20 ? 3 : 4];
                    if (ms >= 5 && long_lines < 60) {
                        ++long_lines;
                        std::printf("Wait trace: long gfx wait %.1f ms at %#llx ref %#x func %u, %u polls, "
                                    "values seen %#x %#x %#x %#x (%u)\n",
                                    ms, static_cast<unsigned long long>(wait_reg_mem->Address<VAddr>()),
                                    wait_reg_mem->ref, unsigned(wait_reg_mem->function.Value()), polls,
                                    seen[0], seen[1], seen[2], seen[3], num_seen);
                    }
                    if (const auto now = std::chrono::steady_clock::now(); now - last >= std::chrono::seconds{5}) {
                        last = now;
                        std::printf("Wait trace (5 s): unmet gfx waits by duration: <1 ms %llu, 1-5 ms %llu, "
                                    "5-12 ms %llu, 12-20 ms %llu, >20 ms %llu\n",
                                    (unsigned long long)buckets[0], (unsigned long long)buckets[1],
                                    (unsigned long long)buckets[2], (unsigned long long)buckets[3],
                                    (unsigned long long)buckets[4]);
                        std::fflush(stdout);
                        buckets = {};
                    }
                }
                break;
            }
            case PM4ItOpcode::IndirectBuffer: {
                const auto* indirect_buffer = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
                auto task = ProcessGraphics(
                    {indirect_buffer->Address<const u32>(), indirect_buffer->ib_size}, {});
                // Tasks start suspended: the nested body (and its scan) runs on this resume.
                g_dcb_depth = dcb_depth + 1;
                RESUME_GFX(task);
                g_dcb_depth = dcb_depth;

                while (!task.handle.done()) {
                    YIELD_GFX();
                    RESUME_GFX(task);
                }
                task.handle.destroy(); // the nested frame (see the constant engine's)
                break;
            }
            case PM4ItOpcode::IncrementDeCounter: {
                ++cblock.de_count;
                break;
            }
            case PM4ItOpcode::WaitOnCeCounter: {
                while (cblock.ce_count <= cblock.de_count && !ce_task.handle.done()) {
                    RESUME_GFX(ce_task);
                }
                break;
            }
            case PM4ItOpcode::PfpSyncMe: {
                break;
            }
            case PM4ItOpcode::StrmoutBufferUpdate: {
                const auto* strmout = reinterpret_cast<const PM4CmdStrmoutBufferUpdate*>(header);
                LOG_WARNING(Render_Vulkan,
                            "Unimplemented IT_STRMOUT_BUFFER_UPDATE, update_memory = {}, "
                            "source_select = {}, buffer_select = {}",
                            strmout->update_memory.Value(),
                            magic_enum::enum_name(strmout->source_select.Value()),
                            strmout->buffer_select.Value());
                break;
            }
            case PM4ItOpcode::GetLodStats: {
                LOG_WARNING(Render_Vulkan, "Unimplemented IT_GET_LOD_STATS");
                break;
            }
            case PM4ItOpcode::CondExec: {
                const auto* cond_exec = reinterpret_cast<const PM4CmdCondExec*>(header);
                if (cond_exec->command.Value() != 0) {
                    LOG_WARNING(Render, "IT_COND_EXEC used a reserved command");
                }
                const auto skip = *cond_exec->Address() == false;
                if (skip) {
                    dcb = NextPacket(dcb,
                                     header->type3.NumWords() + 1 + cond_exec->exec_count.Value());
                    continue;
                }
                break;
            }
            default:
                UNREACHABLE_MSG("Unknown PM4 type 3 opcode {:#x} with count {}",
                                static_cast<u32>(opcode), count);
            }
            dcb = NextPacket(dcb, header->type3.NumWords() + 1);
            break;
        }
    }

    if (ce_task.handle) {
        while (!ce_task.handle.done()) {
            RESUME_GFX(ce_task);
        }
        ce_task.handle.destroy();
    }

    if (rasterizer && seq != NoSeq) {
        rasterizer->RetireSubmission(); // prepared draws live until the recording thread is past
    }
    if (draw_prep) {
        draw_prep->EndSubmission();
    }
    if (seq != NoSeq && Gow3Stats::enabled) {
        Gow3Stats::submissions.fetch_add(1, std::memory_order_relaxed);
#ifndef _WIN32 // gow3: per-thread rusage statistics are Linux-only
        if (rusage usage{}; getrusage(RUSAGE_THREAD, &usage) == 0) {
            Gow3Stats::gpu_user_us.store(u64(usage.ru_utime.tv_sec) * 1000000 + usage.ru_utime.tv_usec,
                                       std::memory_order_relaxed);
            Gow3Stats::gpu_sys_us.store(u64(usage.ru_stime.tv_sec) * 1000000 + usage.ru_stime.tv_usec,
                                      std::memory_order_relaxed);
            Gow3Stats::gpu_invol_switches.store(usage.ru_nivcsw, std::memory_order_relaxed);
            Gow3Stats::gpu_vol_switches.store(usage.ru_nvcsw, std::memory_order_relaxed);
            Gow3Stats::gpu_minor_faults.store(usage.ru_minflt, std::memory_order_relaxed);
        }
#endif
    }

    FIBER_EXIT;
}

template <bool is_indirect>
Liverpool::Task Liverpool::ProcessCompute(std::span<const u32> acb, u32 vqid) {
    FIBER_ENTER(acb_task_name[vqid]);
    auto& queue = asc_queues[{vqid}];

    struct IndirectPatch {
        const PM4Header* header;
        VAddr indirect_addr;
    };
    boost::container::small_vector<IndirectPatch, 4> indirect_patches;

    auto base_addr = reinterpret_cast<VAddr>(acb.data());
    size_t acb_size = acb.size_bytes();
    while (!acb.empty()) {
        ProcessCommands();

        auto* header = reinterpret_cast<const PM4Header*>(acb.data());
        u32 next_dw_off = header->type3.NumWords() + 1;

        // If we have a buffered packet, use it.
        if (queue.tmp_dwords > 0) [[unlikely]] {
            header = reinterpret_cast<const PM4Header*>(queue.tmp_packet.data());
            next_dw_off = header->type3.NumWords() + 1 - queue.tmp_dwords;
            std::memcpy(queue.tmp_packet.data() + queue.tmp_dwords, acb.data(),
                        next_dw_off * sizeof(u32));
            queue.tmp_dwords = 0;
        }

        // If the packet is split across ring boundary, buffer until next submission
        if (next_dw_off > acb.size()) [[unlikely]] {
            std::memcpy(queue.tmp_packet.data(), acb.data(), acb.size_bytes());
            queue.tmp_dwords = acb.size();
            if constexpr (!is_indirect) {
                *queue.read_addr += acb.size();
                *queue.read_addr %= queue.ring_size_dw;
            }
            break;
        }

        if (header->type == 2) {
            // Type-2 packet are used for padding purposes
            next_dw_off = 1;
            acb = NextPacket(acb, next_dw_off);
            if constexpr (!is_indirect) {
                *queue.read_addr += next_dw_off;
                *queue.read_addr %= queue.ring_size_dw;
            }
            continue;
        }

        if (header->type != 3) {
            // No other types of packets were spotted so far
            UNREACHABLE_MSG("Invalid PM4 type {}", header->type.Value());
        }

        const PM4ItOpcode opcode = header->type3.opcode;

        // gow3: compute work uses the caches, so the draw recording thread runs dry first. Not for
        // no-ops and register writes (per-queue compute state, or SH registers the draw packets
        // snapshot) nor for dispatches (handed to that thread in stream order, or drained by
        // DispatchDirect/Indirect themselves): Uncharted 2 drained ~900 times per frame here,
        // 20-25% of the command thread's time with multithreaded rendering.
        // 18 Oct 2026 (GOW3_FEWER_DRAINS=1): nor for nested buffers (their packets pass this
        // check one by one) and memory waits (they drain only while unmet, as on the graphics
        // queue): compute drains were up to 35% of the command thread in Uncharted 3 on a Ryzen
        // laptop. Off by default (see the graphics WaitRegMem).
        // 21 Oct 2026 (GOW3_SKIP_IB_DRAIN=1): nested buffers alone.
        const bool pipelined_wait =
            (FewerDrains() &&
             (opcode == PM4ItOpcode::IndirectBuffer || opcode == PM4ItOpcode::WaitRegMem)) ||
            (opcode == PM4ItOpcode::IndirectBuffer && SkipIbDrain());
        if (rasterizer && opcode != PM4ItOpcode::Nop && opcode != PM4ItOpcode::SetShReg &&
            opcode != PM4ItOpcode::SetQueueReg && opcode != PM4ItOpcode::DispatchDirect &&
            opcode != PM4ItOpcode::DispatchIndirect && opcode != PM4ItOpcode::ReleaseMem &&
            !pipelined_wait) {
            rasterizer->DrainDrawPipe(Vulkan::DrawPipe::ReasonCompute);
            // GOW3_FRAME_STATS: which compute packets drain, per 5 s.
            static const bool stats = std::getenv("GOW3_FRAME_STATS") != nullptr;
            if (stats) {
                static std::array<u32, 256> counts{};
                static auto last = std::chrono::steady_clock::now();
                ++counts[static_cast<u32>(opcode) & 0xff];
                if (const auto now = std::chrono::steady_clock::now();
                    now - last >= std::chrono::seconds(5)) {
                    last = now;
                    std::string text;
                    for (u32 op = 0; op < counts.size(); ++op) {
                        if (counts[op]) {
                            text += fmt::format(" {}={}", magic_enum::enum_name(PM4ItOpcode(op)),
                                                counts[op]);
                        }
                    }
                    std::printf("Compute drains (5 s):%s\n", text.c_str());
                    std::fflush(stdout);
                    counts.fill(0);
                }
            }
        }

        const auto* it_body = reinterpret_cast<const u32*>(header) + 1;
        switch (opcode) {
        case PM4ItOpcode::Nop: {
            const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
            break;
        }
        case PM4ItOpcode::IndirectBuffer: {
            const auto* indirect_buffer = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
            auto task = ProcessCompute<true>(
                {indirect_buffer->Address<const u32>(), indirect_buffer->ib_size}, vqid);
            RESUME_ASC(task, vqid);

            while (!task.handle.done()) {
                YIELD_ASC(vqid);
                RESUME_ASC(task, vqid);
            }
            task.handle.destroy(); // the nested frame (see the constant engine's)
            break;
        }
        case PM4ItOpcode::DmaData: {
            const auto* dma_data = reinterpret_cast<const PM4DmaData*>(header);
            if (dma_data->dst_addr_lo == 0x3022C || !rasterizer) {
                break;
            }
            ASSERT(dma_data->command.das == 0);
            if (dma_data->src_sel == DmaDataSrc::Data && dma_data->dst_sel == DmaDataDst::Gds) {
                rasterizer->FillBuffer(dma_data->dst_addr_lo, dma_data->NumBytes(), dma_data->data,
                                       true);
            } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                        dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
                       dma_data->dst_sel == DmaDataDst::Gds) {
                rasterizer->CopyBuffer(dma_data->dst_addr_lo, dma_data->SrcAddress<VAddr>(),
                                       dma_data->NumBytes(), true, false);
            } else if (dma_data->src_sel == DmaDataSrc::Data &&
                       (dma_data->dst_sel == DmaDataDst::Memory ||
                        dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                rasterizer->FillBuffer(dma_data->DstAddress<VAddr>(), dma_data->NumBytes(),
                                       dma_data->data, false);
            } else if (dma_data->src_sel == DmaDataSrc::Gds &&
                       (dma_data->dst_sel == DmaDataDst::Memory ||
                        dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                rasterizer->CopyBuffer(dma_data->DstAddress<VAddr>(), dma_data->src_addr_lo,
                                       dma_data->NumBytes(), false, true);
            } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                        dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
                       (dma_data->dst_sel == DmaDataDst::Memory ||
                        dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                const u32 num_bytes = dma_data->NumBytes();
                const VAddr src_addr = dma_data->SrcAddress<VAddr>();
                const VAddr dst_addr = dma_data->DstAddress<VAddr>();
                const PM4Header* header =
                    reinterpret_cast<const PM4Header*>(dst_addr - sizeof(PM4Header));
                if (dst_addr >= base_addr && dst_addr < base_addr + acb_size &&
                    num_bytes == sizeof(PM4CmdDispatchIndirect::GroupDimensions) &&
                    header->type == 3 && header->type3.opcode == PM4ItOpcode::DispatchDirect) {
                    indirect_patches.emplace_back(header, src_addr);
                } else {
                    rasterizer->CopyBuffer(dst_addr, src_addr, num_bytes, false, false);
                }
            } else {
                UNREACHABLE_MSG("WriteData src_sel = {}, dst_sel = {}", u32(dma_data->src_sel),
                                u32(dma_data->dst_sel));
            }
            break;
        }
        case PM4ItOpcode::AcquireMem: {
            break;
        }
        case PM4ItOpcode::Rewind: {
            if (!rasterizer) {
                break;
            }
            const PM4CmdRewind* rewind = reinterpret_cast<const PM4CmdRewind*>(header);
            while (!rewind->Valid()) {
                YIELD_ASC(vqid);
            }
            break;
        }
        case PM4ItOpcode::SetShReg: {
            const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
            const auto set_size = (header->type3.NumWords() - 1) * sizeof(u32);

            if (set_data->reg_offset >= 0x200 &&
                set_data->reg_offset <= (0x200 + sizeof(ComputeProgram) / 4)) {
                ASSERT(set_size <= sizeof(ComputeProgram));
                auto* addr = reinterpret_cast<u32*>(&mapped_queues[vqid + 1].cs_state) +
                             (set_data->reg_offset - 0x200);
                std::memcpy(addr, header + 2, set_size);
            } else {
                std::memcpy(&regs.reg_array[Regs::ShRegWordOffset + set_data->reg_offset],
                            header + 2, set_size);
                // gow3: interleaves with the graphics stream at an unpredictable point, so
                // draw-preparation workers cannot reproduce it: make their checksums differ.
                gfx_reg_checksum = HashRegisterPacket(gfx_reg_checksum ^ 0xA5C0A5C0A5C0ull,
                                                      reinterpret_cast<const u32*>(header),
                                                      header->type3.NumWords() + 1);
            }
            break;
        }
        case PM4ItOpcode::SetQueueReg: {
            const auto* set_data = reinterpret_cast<const PM4CmdSetQueueReg*>(header);
            LOG_WARNING(Render, "Encountered compute SetQueueReg: vqid = {}, reg_offset = {:#x}",
                        set_data->vqid.Value(), set_data->reg_offset.Value());
            break;
        }
        case PM4ItOpcode::DispatchDirect: {
            const auto* dispatch_direct = reinterpret_cast<const PM4CmdDispatchDirect*>(header);
            if (auto it = std::ranges::find(indirect_patches, header, &IndirectPatch::header);
                it != indirect_patches.end()) {
                const auto size = sizeof(PM4CmdDispatchIndirect::GroupDimensions);
                rasterizer->DispatchIndirect(it->indirect_addr, 0, size);
                break;
            }
            auto& cs_program = GetCsRegs();
            cs_program.dim_x = dispatch_direct->dim_x;
            cs_program.dim_y = dispatch_direct->dim_y;
            cs_program.dim_z = dispatch_direct->dim_z;
            cs_program.dispatch_initiator = dispatch_direct->dispatch_initiator;
            if (DebugState.DumpingCurrentReg()) {
                DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                               cs_program);
            }
            if (!rasterizer || (cs_program.dispatch_initiator & 1) == 0) {
                break;
            }
            const auto cmd_address = reinterpret_cast<const void*>(header);
            rasterizer->ScopeMarker("asc[{}]:{}:DispatchDirect",
                                    fmt::make_format_args(vqid, cmd_address),
                                    [&] { rasterizer->DispatchDirect(); });
            break;
        }
        case PM4ItOpcode::DispatchIndirect: {
            const auto* dispatch_indirect =
                reinterpret_cast<const PM4CmdDispatchIndirectMec*>(header);
            auto& cs_program = GetCsRegs();
            const auto ib_address = dispatch_indirect->Address<VAddr>();
            const auto size = sizeof(PM4CmdDispatchIndirect::GroupDimensions);
            if (DebugState.DumpingCurrentReg()) {
                DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                               cs_program);
            }
            if (!rasterizer || (cs_program.dispatch_initiator & 1) == 0) {
                break;
            }
            const auto cmd_address = reinterpret_cast<const void*>(header);
            rasterizer->ScopeMarker("asc[{}]:{}:DispatchIndirect",
                                    fmt::make_format_args(vqid, cmd_address),
                                    [&] { rasterizer->DispatchIndirect(ib_address, 0, size); });
            break;
        }
        case PM4ItOpcode::WriteData: {
            // gow3: copies deferred to the recording thread precede writes the guest sees, and
            // so do graphics fences deferred to it (the pipe was drained before this packet).
            if (rasterizer) {
                rasterizer->WaitHostCopies();
                rasterizer->WaitDeferredSignals();
            }
            const auto* write_data = reinterpret_cast<const PM4CmdWriteData*>(header);
            ASSERT(write_data->dst_sel.Value() == 2 || write_data->dst_sel.Value() == 5);
            const u32 data_size = (header->type3.count.Value() - 2) * 4;
            if (!write_data->wr_one_addr.Value()) {
                const Vulkan::GpuWriteSite site{3};
                std::memcpy(write_data->Address<void*>(), write_data->data, data_size);
            } else {
                UNREACHABLE();
            }
            break;
        }
        case PM4ItOpcode::MemSemaphore: {
            const auto* mem_semaphore = reinterpret_cast<const PM4CmdMemSemaphore*>(header);
            if (mem_semaphore->IsSignaling()) {
                mem_semaphore->Signal();
            } else {
                while (!mem_semaphore->Signaled()) {
                    YIELD_ASC(vqid);
                }
                mem_semaphore->Decrement();
            }
            break;
        }
        case PM4ItOpcode::WaitRegMem: {
            const auto* wait_reg_mem = reinterpret_cast<const PM4CmdWaitRegMem*>(header);
            ASSERT(wait_reg_mem->engine.Value() == PM4CmdWaitRegMem::Engine::Me);
            if (FewerDrains()) {
                // Not drained up front: as on the graphics queue, a fence handed to the draw
                // recording thread counts as written, and an unmet wait lets it catch up first.
                if (u32 value; wait_reg_mem->mem_space.Value() ==
                                   PM4CmdWaitRegMem::MemSpace::Memory &&
                               !Gow3Toggle::Disabled(Gow3Toggle::PendingFenceWaits) &&
                               PendingFenceValue(wait_reg_mem->Address<VAddr>(), value) &&
                               wait_reg_mem->TestValue(value)) {
                    break;
                }
                if (!LandDeferredFenceFor(*wait_reg_mem, regs)) {
                    while (!wait_reg_mem->Test(regs.reg_array)) {
                        if (rasterizer && !rasterizer->DrawPipeIdle()) {
                            rasterizer->DrainDrawPipe(static_cast<u32>(opcode));
                            continue;
                        }
                        YIELD_ASC(vqid);
                    }
                }
                break;
            }
            if (!LandDeferredFenceFor(*wait_reg_mem, regs)) {
                while (!wait_reg_mem->Test(regs.reg_array)) {
                    YIELD_ASC(vqid);
                }
            }
            break;
        }
        case PM4ItOpcode::ReleaseMem: {
            const auto* release_mem = reinterpret_cast<const PM4CmdReleaseMem*>(header);
            if (rasterizer) {
                ComputeRelease release{.packet = *release_mem, .pipe_id = queue.pipe_id};
                rasterizer->RunInOrder(&RunComputeReleaseMem, &release, sizeof(release));
                break;
            }
            release_mem->SignalFence(
                [pipe_id = queue.pipe_id] {
                    Platform::IrqC::Instance()->SignalAsync(static_cast<Platform::InterruptId>(pipe_id));
                },
                [](VAddr, u16, u16) {});
            break;
        }
        case PM4ItOpcode::EventWrite: {
            // const auto* event = reinterpret_cast<const PM4CmdEventWrite*>(header);
            break;
        }
        default:
            UNREACHABLE_MSG("Unknown PM4 type 3 opcode {:#x} with count {}",
                            static_cast<u32>(opcode), header->type3.NumWords());
        }

        acb = NextPacket(acb, next_dw_off);

        if constexpr (!is_indirect) {
            *queue.read_addr += next_dw_off;
            *queue.read_addr %= queue.ring_size_dw;
        }
    }

    FIBER_EXIT;
}

Liverpool::CmdBuffer Liverpool::CopyCmdBuffers(std::span<const u32> dcb, std::span<const u32> ccb) {
    auto& queue = mapped_queues[GfxQueueId];
    ASSERT_MSG(queue.dcb_buffer.capacity() >= queue.dcb_buffer_offset + dcb.size(),
               "dcb copy buffer out of reserved space");
    ASSERT_MSG(queue.ccb_buffer.capacity() >= queue.ccb_buffer_offset + ccb.size(),
               "ccb copy buffer out of reserved space");

    queue.dcb_buffer.resize(
        std::max(queue.dcb_buffer.size(), queue.dcb_buffer_offset + dcb.size()));
    queue.ccb_buffer.resize(
        std::max(queue.ccb_buffer.size(), queue.ccb_buffer_offset + ccb.size()));

    const u32 prev_dcb_buffer_offset = queue.dcb_buffer_offset;
    const u32 prev_ccb_buffer_offset = queue.ccb_buffer_offset;
    if (!dcb.empty()) {
        std::memcpy(queue.dcb_buffer.data() + queue.dcb_buffer_offset, dcb.data(),
                    dcb.size_bytes());
        queue.dcb_buffer_offset += dcb.size();
        dcb = std::span<const u32>{queue.dcb_buffer.begin() + prev_dcb_buffer_offset,
                                   queue.dcb_buffer.begin() + queue.dcb_buffer_offset};
    }

    if (!ccb.empty()) {
        std::memcpy(queue.ccb_buffer.data() + queue.ccb_buffer_offset, ccb.data(),
                    ccb.size_bytes());
        queue.ccb_buffer_offset += ccb.size();
        ccb = std::span<const u32>{queue.ccb_buffer.begin() + prev_ccb_buffer_offset,
                                   queue.ccb_buffer.begin() + queue.ccb_buffer_offset};
    }

    return std::make_pair(dcb, ccb);
}

void Liverpool::SubmitGfx(std::span<const u32> dcb, std::span<const u32> ccb) {
    auto& queue = mapped_queues[GfxQueueId];
    g_last_gfx_submit_ns.store(SteadyNs(), std::memory_order_release);
    static const bool wait_done_test = std::getenv("GOW3_GFX_WAIT_SUBMIT_DONE_MS") != nullptr;
    if (wait_done_test) { // the test switch only: otherwise nothing takes these off again
        std::scoped_lock lock{gfx_submit_gen_mutex};
        gfx_submit_gens.emplace_back(submit_done_gen.load(std::memory_order_acquire), SteadyNs());
    }

    if (EmulatorSettings.IsCopyGpuBuffers()) {
        std::tie(dcb, ccb) = CopyCmdBuffers(dcb, ccb);
    }

    ScanForPrecompile(GfxQueueId, dcb);
    // The copy for the draw preparation workers is made before taking the queue lock, which
    // the GPU thread needs to pick up work.
    auto* draw_prep = rasterizer ? &rasterizer->GetDrawPreparation() : nullptr;
    auto prep_submission = draw_prep ? draw_prep->Build(dcb) : nullptr;
    // gow3 1 Nov 2026 (heavy-scene profile, Uncharted 2 train yard): numbering, enqueueing and
    // queueing keep one order under the submitters' own lock; the queue lock, which the GPU
    // command thread takes for every task it looks at, is held for the push only. Under the
    // queue lock the draw preparation's Enqueue waited for its workers (a worker copies the
    // 208 KiB register file under that lock), and the command thread waited behind it: 5% of
    // its time in U2. GOW3_SUBMIT_ORDER_LOCK=0: everything under the queue lock, as before.
    static const bool order_lock = [] {
        const char* env = std::getenv("GOW3_SUBMIT_ORDER_LOCK");
        return !(env && env[0] == '0');
    }();
    if (order_lock) {
        std::scoped_lock order{gfx_submit_order_mutex};
        u64 seq = NoSeq;
        if (draw_prep) {
            seq = gfx_submit_seq++;
            draw_prep->Enqueue(seq, std::move(prep_submission));
        }
        auto task = ProcessGraphics(dcb, ccb, seq);
        std::scoped_lock lock{queue.m_access};
        queue.submits.emplace(task.handle);
    } else {
        // Numbering, enqueueing and queueing under one lock keep the three orders identical.
        std::scoped_lock lock{queue.m_access};
        u64 seq = NoSeq;
        if (draw_prep) {
            seq = gfx_submit_seq++;
            draw_prep->Enqueue(seq, std::move(prep_submission));
        }
        auto task = ProcessGraphics(dcb, ccb, seq);
        queue.submits.emplace(task.handle);
    }

    const u64 my_submit = gfx_submits_queued.fetch_add(1, std::memory_order_relaxed) + 1;
    {
        std::scoped_lock lk{submit_mutex};
        ++num_submits;
        work_retired = false;
        submit_cv.notify_one();
    }
    // gow3 diagnostics (GOW3_SYNC_SUBMIT=1): the guest returns from the submission only once the
    // GPU command thread has processed it (at most 50 ms: the submission may wait for the guest),
    // so the game never runs ahead of the port's processing (Uncharted 3's cloth flicker, 16 Oct).
    static const bool sync_submit = [] {
        const char* env = std::getenv("GOW3_SYNC_SUBMIT");
        return env && env[0] == '1';
    }();
    if (sync_submit) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds{50};
        while (gfx_submits_done.load(std::memory_order_acquire) < my_submit &&
               std::chrono::steady_clock::now() < until) {
            std::this_thread::yield();
        }
    }
}

void Liverpool::SubmitAsc(u32 gnm_vqid, std::span<const u32> acb) {
    ASSERT_MSG(gnm_vqid > 0 && gnm_vqid < NumTotalQueues, "Invalid virtual ASC queue index");
    auto& queue = mapped_queues[gnm_vqid];
    ScanForPrecompile(gnm_vqid, acb);

    const auto vqid = gnm_vqid - 1;
    const auto& task = ProcessCompute(acb, vqid);
    {
        std::scoped_lock lock{queue.m_access};
        queue.submits.emplace(task.handle);
    }

    std::scoped_lock lk{submit_mutex};
    num_mapped_queues = std::max(num_mapped_queues, gnm_vqid + 1);
    ++num_submits;
    work_retired = false;
    submit_cv.notify_one();
}

} // namespace AmdGpu
