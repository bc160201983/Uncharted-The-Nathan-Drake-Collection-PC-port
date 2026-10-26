// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "gow3_profiler.h"
#include <thread>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <fmt/format.h>
#ifndef _WIN32
#include <dlfcn.h>
#endif
#include <functional>

#include "gow3_copy.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "gow3_toggles.h"
#include "video_core/renderer_vulkan/vk_wait_diagnostics.h"
#include "common/assert.h"
#include "common/debug.h"
#include "common/thread.h"
#include "imgui/renderer/texture_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "gow3_threads.h"

namespace Vulkan {

std::mutex Scheduler::submit_mutex;

Scheduler::Scheduler(const Instance& instance, bool threaded_recording)
    : instance{instance}, work_semaphore{instance}, command_pool{instance, &work_semaphore},
      count_lock_waits{threaded_recording} {
    // gow3: GOW3_VK_RECORD_THREAD=0 records on the calling thread.
    const char* env = std::getenv("GOW3_VK_RECORD_THREAD");
    const char* checkpoints = std::getenv("GOW3_GPU_CHECKPOINTS");
    checkpoint_callers = instance.HasGpuCheckpoints() && checkpoints && checkpoints[0] == '2';
    if (threaded_recording && !(env && env[0] == '0')) {
        record_chunk = AcquireChunk();
        recorder_thread = std::jthread(std::bind_front(&Scheduler::RecorderThread, this));
        recording_threaded = true;
    }
#if TRACY_GPU_ENABLED
    profiler_scope = reinterpret_cast<tracy::VkCtxScope*>(std::malloc(sizeof(tracy::VkCtxScope)));
#endif
    AllocateWorkerCommandBuffers();
    priority_pending_ops_thread =
        std::jthread(std::bind_front(&Scheduler::PriorityPendingOpsThread, this));
}

Scheduler::~Scheduler() {
    if (recorder_thread.joinable()) {
        SyncRecording();
        recorder_thread.request_stop();
        recorder_cv.notify_all();
        recorder_thread.join();
    }
#if TRACY_GPU_ENABLED
    std::free(profiler_scope);
#endif
}

// gow3 26 Oct 2026 (GitHub #236, AMD): a rendering instance that goes on with an attachment of
// the previous one (the same image view: the pass was only ended for a copy or a barrier in
// between) waits for that instance's attachment writes first. Image::GetBarriers does not count
// attachment writes as writes, so an unchanged layout and access recorded no barrier at all, and
// rasterization order holds only inside one rendering instance. AMD's driver overlapped the two:
// on Uncharted 2's title screen the fullscreen anti-aliasing draw into the display buffer landed
// after the logo and text draws of the next instance (pieces of "2" and "AMONG THIEVES",
// "Press X" and the copyright line missing in most frames on a Radeon 780M; RX 9070 too).
// GOW3_ATTACHMENT_SYNC=0: off, =1: on for every GPU; default on except NVIDIA's driver (no
// flicker there, and no extra waits).
static bool AttachmentSyncEnabled(const Instance& instance) {
    static const bool enabled = [&instance] {
        const char* env = std::getenv("GOW3_ATTACHMENT_SYNC");
        const bool on = env && env[0] ? env[0] != '0'
                                       : instance.GetDriverID() != vk::DriverId::eNvidiaProprietary;
        std::printf("GPU: rendering continued on the same attachments waits for their earlier "
                    "writes: %s (GOW3_ATTACHMENT_SYNC)\n",
                    on ? "on" : "off");
        std::fflush(stdout);
        return on;
    }();
    return enabled;
}

static vk::PipelineStageFlags2 SharedAttachmentStages(const RenderState& a, const RenderState& b) {
    vk::PipelineStageFlags2 stages{};
    for (u32 i = 0; i < a.num_color_attachments; ++i) {
        const auto view = a.color_attachments[i].image_view;
        if (!view) {
            continue;
        }
        for (u32 j = 0; j < b.num_color_attachments; ++j) {
            if (b.color_attachments[j].image_view == view) {
                stages |= vk::PipelineStageFlagBits2::eColorAttachmentOutput;
            }
        }
    }
    const auto& da = a.depth_stencil_attachment;
    const auto& db = b.depth_stencil_attachment;
    if ((da.has_depth || da.has_stencil) && (db.has_depth || db.has_stencil) && da.image_view &&
        da.image_view == db.image_view) {
        stages |= vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                  vk::PipelineStageFlagBits2::eLateFragmentTests;
    }
    return stages;
}

void Scheduler::BeginRendering(const RenderState& new_state) {
    if (is_rendering && render_state == new_state) {
        return;
    }
    const vk::PipelineStageFlags2 shared =
        has_render_state && AttachmentSyncEnabled(instance)
            ? SharedAttachmentStages(render_state, new_state)
            : vk::PipelineStageFlags2{};
    EndRendering();
    if (shared) {
        const bool color = bool(shared & vk::PipelineStageFlagBits2::eColorAttachmentOutput);
        const bool depth = bool(shared & vk::PipelineStageFlagBits2::eLateFragmentTests);
        const vk::MemoryBarrier2 barrier = {
            .srcStageMask = shared,
            .srcAccessMask =
                (color ? vk::AccessFlagBits2::eColorAttachmentWrite : vk::AccessFlagBits2{}) |
                (depth ? vk::AccessFlagBits2::eDepthStencilAttachmentWrite : vk::AccessFlagBits2{}),
            .dstStageMask = shared,
            .dstAccessMask =
                (color ? vk::AccessFlagBits2::eColorAttachmentRead |
                             vk::AccessFlagBits2::eColorAttachmentWrite
                       : vk::AccessFlagBits2{}) |
                (depth ? vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                             vk::AccessFlagBits2::eDepthStencilAttachmentWrite
                       : vk::AccessFlagBits2{}),
        };
        Record([barrier](vk::CommandBuffer cmdbuf) {
            cmdbuf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1,
                                                       .pMemoryBarriers = &barrier});
        });
    }
    is_rendering = true;
    has_render_state = true;
    render_state = new_state;

    std::array<vk::RenderingAttachmentInfo, 8> color_attachments;
    for (u32 i = 0; i < render_state.num_color_attachments; ++i) {
        const auto& cb = render_state.color_attachments[i];
        color_attachments[i] = vk::RenderingAttachmentInfo{
            .imageView = cb.image_view,
            .imageLayout = cb.image_layout,
            .loadOp = cb.is_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = vk::ClearValue{.color = vk::ClearColorValue{.uint32 = cb.clear_value}},
        };
    }

    const auto& db = render_state.depth_stencil_attachment;
    const vk::RenderingAttachmentInfo depth_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.depth_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue =
            vk::ClearValue{.depthStencil = vk::ClearDepthStencilValue{.depth = std::bit_cast<float>(
                                                                          db.clear_value[0])}},
    };
    const vk::RenderingAttachmentInfo stencil_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.stencil_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = vk::ClearValue{.depthStencil =
                                         vk::ClearDepthStencilValue{.stencil = db.clear_value[1]}},
    };

    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .offset = {0, 0},
                .extent = {render_state.width, render_state.height},
            },
        .layerCount = render_state.num_layers,
        .colorAttachmentCount = render_state.num_color_attachments,
        .pColorAttachments = color_attachments.data(),
        .pDepthAttachment = db.has_depth ? &depth_attachment : nullptr,
        .pStencilAttachment = db.has_stencil ? &stencil_attachment : nullptr,
    };

    if (!recording_threaded) {
        current_cmdbuf.beginRendering(rendering_info);
        return;
    }
    // The attachment infos live on this stack frame: the recorded closure keeps copies.
    Record([info = rendering_info, color_attachments, depth_attachment,
            stencil_attachment](vk::CommandBuffer cmdbuf) mutable {
        info.pColorAttachments = color_attachments.data();
        if (info.pDepthAttachment) {
            info.pDepthAttachment = &depth_attachment;
        }
        if (info.pStencilAttachment) {
            info.pStencilAttachment = &stencil_attachment;
        }
        cmdbuf.beginRendering(info);
    });
}

void Scheduler::EndRendering() {
    if (!is_rendering) {
        return;
    }
    is_rendering = false;
    Record([](vk::CommandBuffer cmdbuf) { cmdbuf.endRendering(); });
}

void Scheduler::TraceDirectRecording(void* caller) {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_RECORDER_TRACE");
        return env && env[0] == '1';
    }();
    if (!enabled) {
        return;
    }
    static std::mutex mutex;
    static std::unordered_map<void*, u64> callers;
    static u64 calls;
    std::scoped_lock lk{mutex};
    ++callers[caller];
    if (++calls % 2000) {
        return;
    }
    std::vector<std::pair<u64, void*>> top;
    for (const auto& [address, count] : callers) {
        top.emplace_back(count, address);
    }
    std::ranges::sort(top, std::greater{});
    for (size_t i = 0; i < std::min<size_t>(top.size(), 8); ++i) {
#ifdef _WIN32
        struct { const char* dli_fname; void* dli_fbase; } info{nullptr, nullptr};
#else
        Dl_info info{};
        dladdr(top[i].second, &info);
#endif
        std::printf("Recorder sync caller: %llu x %s+0x%lx\n",
                    static_cast<unsigned long long>(top[i].first),
                    info.dli_fname ? info.dli_fname : "?",
                    static_cast<unsigned long>(reinterpret_cast<uintptr_t>(top[i].second) -
                                               reinterpret_cast<uintptr_t>(info.dli_fbase)));
    }
    callers.clear();
}

std::unique_ptr<RecordChunk> Scheduler::AcquireChunk() {
    std::scoped_lock lk{recorder_mutex};
    if (free_chunks.empty()) {
        return std::make_unique<RecordChunk>();
    }
    auto chunk = std::move(free_chunks.back());
    free_chunks.pop_back();
    return chunk;
}

void Scheduler::SignalAfterHostCopies(std::function<void()> signal) {
    guest_write_epoch.fetch_add(1, std::memory_order_relaxed);
    if (!IsRecordingDeferred()) {
        WaitHostCopies();
        signal();
        return;
    }
    Gow3Copy::FlushBatch();
    deferred_signals_issued.fetch_add(1, std::memory_order_relaxed);
    Record([signal = std::move(signal), done = deferred_signals_done](vk::CommandBuffer) mutable {
        Gow3Copy::AfterCopies([signal = std::move(signal), done = std::move(done)] {
            signal();
            done->fetch_add(1, std::memory_order_release);
        });
    });
    KickRecording(true);
}

void Scheduler::WaitDeferredSignals() {
    guest_write_epoch.fetch_add(1, std::memory_order_relaxed);
    const u64 issued = deferred_signals_issued.load(std::memory_order_relaxed);
    if (deferred_signals_done->load(std::memory_order_acquire) >= issued ||
        Gow3Toggle::Disabled(Gow3Toggle::OrderedGuestWrites)) {
        return;
    }
    Gow3Stats::WaitTimer timer{Gow3Stats::host_copies_wait_ns};
    KickRecording(true);
    while (deferred_signals_done->load(std::memory_order_acquire) < issued) {
        // Helps the copy threads the signals wait for.
        Gow3Copy::WaitAsync();
        std::this_thread::yield();
    }
}

void Scheduler::WaitHostCopies() {
    guest_write_epoch.fetch_add(1, std::memory_order_relaxed);
    if (host_copies_done.load(std::memory_order_acquire) < host_copies_issued) {
        Gow3Stats::WaitTimer timer{Gow3Stats::host_copies_wait_ns};
        Gow3Stats::host_copy_waits.fetch_add(1, std::memory_order_relaxed);
        KickRecording(true);
        while (host_copies_done.load(std::memory_order_acquire) < host_copies_issued) {
            std::this_thread::yield();
        }
    }
    // gow3 20 Oct 2026 (idle CPU): nothing on the copy threads and nothing batched by this
    // thread (the common case): no clock reads for the statistics and no pool mutex. The
    // recorder's copies are ordered by the acquire loads of host_copies_done above (released
    // after each copy); the copy threads' by Gow3Copy::Idle's acquire load, which pairs with the
    // pool's release store made once the last task and its AfterCopies callbacks had run. The
    // caller's guest-visible write that follows therefore comes after every copy, as with
    // WaitAsync. GOW3_COPY_WAIT_BLOCK=0: Idle() is false, WaitAsync as before.
    if (Gow3Copy::Idle()) {
        return;
    }
    Gow3Stats::WaitTimer timer{Gow3Stats::copy_threads_wait_ns};
    Gow3Copy::WaitAsync();
}

void Scheduler::KickRecording(bool force) {
    if (!recording_threaded) {
        return;
    }
    // Callers kick where nobody holds the raw command buffer: deferral resumes.
    direct_mode = false;
    // Batches of tens of KiB keep the queue handoff cheap relative to the work it carries.
    if (!force && full_chunks.empty() && record_chunk->Size() < 32 * 1024) {
        return;
    }
    if (full_chunks.empty() && record_chunk->Empty()) {
        return;
    }
    bool wake;
    {
        std::scoped_lock lk{recorder_mutex};
        for (auto& chunk : full_chunks) {
            recorder_queue.push_back(std::move(chunk));
        }
        if (!record_chunk->Empty()) {
            recorder_queue.push_back(std::move(record_chunk));
        }
        wake = recorder_sleeping;
        queued_chunks.store(recorder_queue.size(), std::memory_order_release);
    }
    full_chunks.clear();
    // A busy recorder picks the new chunks up by itself: waking it is a syscall per draw.
    if (wake) {
        recorder_cv.notify_one();
    }
    if (!record_chunk) {
        record_chunk = AcquireChunk();
    }
}

void Scheduler::SyncRecording() {
    if (!recording_threaded) {
        return;
    }
    KickRecording(true);
    Gow3Stats::WaitTimer timer{Gow3Stats::sync_recording_ns};
    std::unique_lock lk{recorder_mutex};
    recorder_idle_cv.wait(lk, [this] { return recorder_queue.empty() && !recorder_busy; });
}

void Scheduler::RecorderThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("gow3:VkRecorder");
#ifdef _WIN32
    Gow3Profiler::Start(GetCurrentThreadId(), "Vulkan recording thread");
#endif
    while (true) {
        // Spin briefly before sleeping: the next chunk usually follows within microseconds,
        // and a sleeping recorder costs the GPU thread a wake-up syscall per kick. With few
        // hardware threads (Steam Deck: 8) the spin would take time from guest threads.
        static const auto spin_time = std::chrono::microseconds(
            Gow3Threads::Available() >= 12 ? 200 : 20);
        const auto spin_until = std::chrono::steady_clock::now() + spin_time;
        for (u32 spins = 1; queued_chunks.load(std::memory_order_acquire) == 0; ++spins) {
            __builtin_ia32_pause();
            // The clock is read every 256 pauses, not per iteration.
            if (!(spins & 255) &&
                (stoken.stop_requested() || std::chrono::steady_clock::now() >= spin_until)) {
                break;
            }
        }
        std::unique_ptr<RecordChunk> chunk;
        {
            std::unique_lock lk{recorder_mutex};
            recorder_sleeping = true;
            recorder_cv.wait(lk, stoken, [this] { return !recorder_queue.empty(); });
            recorder_sleeping = false;
            if (recorder_queue.empty()) {
                return; // stop requested
            }
            chunk = std::move(recorder_queue.front());
            recorder_queue.pop_front();
            queued_chunks.store(recorder_queue.size(), std::memory_order_release);
            recorder_busy = true;
        }
        // current_cmdbuf only changes after SyncRecording(), which waits for this thread.
        chunk->Execute(current_cmdbuf);
        {
            std::scoped_lock lk{recorder_mutex};
            free_chunks.push_back(std::move(chunk));
            recorder_busy = false;
            if (recorder_queue.empty()) {
                recorder_idle_cv.notify_all();
            }
        }
    }
}

void Scheduler::Flush(SubmitInfo& info) {
    // When flushing, we only send data to the driver; no waiting is necessary.
    SubmitExecution(info);
}

void Scheduler::Flush() {
    SubmitInfo info{};
    Flush(info);
}

#ifdef _WIN32
extern "C" char __ImageBase;
extern "C" __declspec(dllimport) unsigned short __stdcall RtlCaptureStackBackTrace(
    unsigned long frames_to_skip, unsigned long frames_to_capture, void** back_trace,
    unsigned long* back_trace_hash);
#endif
namespace {
// gow3 21 Oct 2026 (GPU worker): GOW3_SUBMIT_STATS=1 (diagnostics) - the draw scheduler's
// submissions per 5 s by call stack: image offsets of the callers above SubmitExecution
// (llvm-symbolizer --obj=gow3-probe.exe 0x140000000+offset). On integrated GPUs every
// submission leaves the GPU idle while the next one is scheduled.
constexpr unsigned long SubmitStackDepth = 6;
using SubmitStack = std::array<u64, SubmitStackDepth>;
bool SubmitStatsEnabled() {
    static const bool on = [] {
        const char* env = std::getenv("GOW3_SUBMIT_STATS");
        return env && env[0] == '1';
    }();
    return on;
}
/// The callers above SubmitExecution as image offsets (zeros where the stack ends).
SubmitStack NoteSubmitCaller() {
    SubmitStack key{};
#ifdef _WIN32
    if (!SubmitStatsEnabled()) {
        return key;
    }
    constexpr unsigned long Depth = SubmitStackDepth;
    void* frames[Depth]{};
    const unsigned n = RtlCaptureStackBackTrace(2, Depth, frames, nullptr);
    for (unsigned i = 0; i < n && i < Depth; ++i) {
        key[i] = reinterpret_cast<u64>(frames[i]) - reinterpret_cast<u64>(&__ImageBase);
    }
    static std::mutex mutex;
    static std::map<std::array<u64, Depth>, u32> counts;
    static u64 total = 0;
    static auto last = std::chrono::steady_clock::now();
    std::scoped_lock lock{mutex};
    ++counts[key];
    ++total;
    if (const auto now = std::chrono::steady_clock::now(); now - last >= std::chrono::seconds{5}) {
        last = now;
        std::vector<std::pair<u32, std::array<u64, Depth>>> order;
        for (const auto& [stack, count] : counts) {
            order.emplace_back(count, stack);
        }
        std::ranges::sort(order, std::greater{});
        std::printf("Submit stats (5 s): %llu draw-scheduler submissions\n",
                    static_cast<unsigned long long>(total));
        for (size_t i = 0; i < std::min<size_t>(order.size(), 8); ++i) {
            std::printf("  %u x", order[i].first);
            for (const u64 offset : order[i].second) {
                std::printf(" %#llx", static_cast<unsigned long long>(offset));
            }
            std::printf("\n");
        }
        std::fflush(stdout);
        counts.clear();
        total = 0;
    }
#endif
    return key;
}

struct GpuCheckpoint { const char* operation{}; u64 first{},second{},sequence{}; };
std::array<GpuCheckpoint,65536> gpu_checkpoints;
std::atomic<u64> checkpoint_index{};
}
void Scheduler::Checkpoint(const char* operation,u64 first,u64 second) {
    if (!instance.HasGpuCheckpoints()) return;
    const u64 sequence=checkpoint_index.fetch_add(1);
    auto* marker=&gpu_checkpoints[sequence%gpu_checkpoints.size()];
    *marker={operation,first,second,sequence};
    Record([marker](vk::CommandBuffer commands) { commands.setCheckpointNV(static_cast<const void*>(marker)); });
}
/// Set by the buffer cache: prints which arena and guest address a GPU address belongs to.
std::function<void(u64)> g_describe_gpu_address;
void DescribeBoundAddress(u64 address);
void DumpDeviceFault(const Instance& instance) {
    if (!instance.HasDeviceFault()) return;
    const auto device=instance.GetDevice();
    vk::DeviceFaultCountsEXT counts{};
    if (device.getFaultInfoEXT(&counts,nullptr)!=vk::Result::eSuccess) {
        std::fprintf(stderr,"GPU fault: no report\n");
        return;
    }
    std::vector<vk::DeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
    std::vector<vk::DeviceFaultVendorInfoEXT> vendors(counts.vendorInfoCount);
    vk::DeviceFaultInfoEXT info{};
    info.pAddressInfos=addresses.data();
    info.pVendorInfos=vendors.data();
    counts.vendorBinarySize=0;
    const auto result=device.getFaultInfoEXT(&counts,&info);
    std::fprintf(stderr,"GPU fault (%s): %s\n",vk::to_string(result).c_str(),info.description);
    for (const auto& address:addresses) {
        std::fprintf(stderr,"GPU fault address: type=%s address=%016llx precision=%llx\n",
                     vk::to_string(address.addressType).c_str(),
                     (unsigned long long)address.reportedAddress,
                     (unsigned long long)address.addressPrecision);
        if (g_describe_gpu_address) g_describe_gpu_address(address.reportedAddress);
        DescribeBoundAddress(address.reportedAddress);
    }
    for (const auto& vendor:vendors) {
        std::fprintf(stderr,"GPU fault vendor: %s code=%llx data=%llx\n",vendor.description,
                     (unsigned long long)vendor.vendorFaultCode,
                     (unsigned long long)vendor.vendorFaultData);
    }
}
void DumpGpuCheckpoints(const Instance& instance) {
    if (!instance.HasGpuCheckpoints()) return;
    const auto checkpoints=instance.GetGraphicsQueue().getCheckpointDataNV();
    const u64 recorded=checkpoint_index.load();
#ifdef _WIN32
    std::fprintf(stderr,"GPU checkpoint records: %zu; markers recorded: %llu; image base %p\n",
                 checkpoints.size(),(unsigned long long)recorded,static_cast<void*>(&__ImageBase));
#else
    std::fprintf(stderr,"GPU checkpoint records: %zu; markers recorded: %llu\n",checkpoints.size(),
                 (unsigned long long)recorded);
#endif
    u64 last=0;
    for (const auto& checkpoint:checkpoints) {
        auto* marker=static_cast<GpuCheckpoint*>(checkpoint.pCheckpointMarker);
        const auto address=reinterpret_cast<uintptr_t>(marker);
        if (address<reinterpret_cast<uintptr_t>(gpu_checkpoints.data()) ||
            address>=reinterpret_cast<uintptr_t>(gpu_checkpoints.data()+gpu_checkpoints.size())) continue;
        last=std::max(last,marker->sequence);
        std::fprintf(stderr,"GPU checkpoint: stage=%s sequence=%llu operation=%s shaders=%016llx,%016llx\n",
                     vk::to_string(checkpoint.stage).c_str(),(unsigned long long)marker->sequence,
                     marker->operation,(unsigned long long)marker->first,(unsigned long long)marker->second);
    }
    // Operations recorded after the newest marker the GPU reached; the fault is in this range.
    for (u64 sequence=last>24 ? last-24 : 0; sequence<recorded && sequence<=last+40; ++sequence) {
        const auto& marker=gpu_checkpoints[sequence%gpu_checkpoints.size()];
        std::fprintf(stderr,"GPU pending: sequence=%llu operation=%s values=%016llx,%016llx\n",
                     (unsigned long long)marker.sequence,marker.operation,
                     (unsigned long long)marker.first,(unsigned long long)marker.second);
    }
}
void Scheduler::ReportCheckpoints() const { DumpGpuCheckpoints(instance); }

void Scheduler::Finish(std::source_location caller) {
    // When finishing, we need to wait for the submission to have executed on the device.
    const u64 presubmit_tick = CurrentTick();
    const bool diagnose = WaitDiagnostics::Enabled();
    const auto start = diagnose ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    SubmitInfo info{};
    SubmitExecution(info);
    const u64 submit_ns = diagnose ? std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count() : 0;
    Wait(presubmit_tick, caller, submit_ns);
}

void Scheduler::WaitSubmitted(std::source_location caller) {
    const u64 tick = CurrentTick();
    if (tick > 1) {
        Wait(tick - 1, caller);
    }
}

void Scheduler::Wait(u64 tick, std::source_location caller, u64 submit_ns) {
    const bool diagnose = WaitDiagnostics::Enabled();
    auto start = diagnose ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (tick >= work_semaphore.CurrentTick()) {
        // Make sure we are not waiting for the current tick without signalling
        SubmitInfo info{};
        Flush(info);
    }
    Gow3Stats::WaitTimer timer{Gow3Stats::tick_wait_ns};
    const auto submitted = diagnose ? std::chrono::steady_clock::now() : start;
    work_semaphore.Wait(tick);
    if (diagnose) {
        const auto completed = std::chrono::steady_clock::now();
        WaitDiagnostics::Get().Add(caller.file_name(), caller.line(),
            submit_ns + std::chrono::duration_cast<std::chrono::nanoseconds>(submitted - start).count(),
            std::chrono::duration_cast<std::chrono::nanoseconds>(completed - submitted).count());
    }
}

void Scheduler::PopPendingOperations() {
    if (num_pending_ops.load(std::memory_order_acquire) == 0) {
        return; // every draw comes here
    }
    std::unique_lock lk(pending_ops_mutex);
    // gow3: this runs on every draw and dispatch. Querying the timeline semaphore is an
    // ioctl, so it is skipped when nothing waits and done once per 32 calls (~0.3 ms; reading
    // the clock per draw instead was itself a hot spot).
    if (pending_ops.empty()) {
        return;
    }
    if (!work_semaphore.IsFree(pending_ops.front().gpu_tick)) {
        if ((++pending_polls & 31) != 0 && !Gow3Toggle::Disabled(Gow3Toggle::PendingPollLimit)) {
            return;
        }
        work_semaphore.Refresh();
    }
    while (!pending_ops.empty() && work_semaphore.IsFree(pending_ops.front().gpu_tick)) {
        pending_ops.front().callback();
        pending_ops.pop();
        num_pending_ops.fetch_sub(1, std::memory_order_release);
    }
}

void Scheduler::AllocateWorkerCommandBuffers() {
    const vk::CommandBufferBeginInfo begin_info = {
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    };

    current_cmdbuf = command_pool.Commit();
    Check(current_cmdbuf.begin(begin_info));
    ++record_serial; // gow3: a new command buffer binds nothing (RecordSerial)

    // Invalidate dynamic state so it gets applied to the new command buffer.
    dynamic_state.Invalidate();

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        static const auto scope_loc =
            GPU_SCOPE_LOCATION("Guest Frame", MarkersPalette::GpuMarkerColor);
        new (profiler_scope) tracy::VkCtxScope{profiler_ctx, &scope_loc, current_cmdbuf, true};
    }
#endif
}

void Scheduler::SubmitExecution(SubmitInfo& info) {
    std::unique_lock lk{submit_mutex, std::try_to_lock};
    if (!lk.owns_lock()) {
        if (count_lock_waits) {
            Gow3Stats::WaitTimer timer{Gow3Stats::submit_lock_ns};
            lk.lock();
        } else {
            lk.lock();
        }
    }
    const u64 signal_value = work_semaphore.NextTick();
    // the draw scheduler's callers (GOW3_SUBMIT_STATS)
    const SubmitStack submit_stack = count_lock_waits ? NoteSubmitCaller() : SubmitStack{};

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        profiler_scope->~VkCtxScope();
        TracyVkCollect(profiler_ctx, current_cmdbuf);
    }
#endif

    if (on_submit) {
        on_submit(info);
    }

    EndRendering();
    if (auto* profiler = GpuProfiler::Get(); profiler && profiler->Records(this)) {
        // Until the next submission's first timestamp: mostly the GPU waiting for it.
        // GOW3_SUBMIT_STATS=1: one label per submitting call stack (which submissions leave
        // the GPU idle).
        u64 key = 0x5B317ull;
        for (const u64 offset : submit_stack) {
            key = (key ^ offset) * 0x100000001B3ull;
        }
        profiler->Mark(key, [&] {
            std::string text{"(between submissions: GPU idle)"};
            if (submit_stack[0]) {
                text += " after";
                for (const u64 offset : submit_stack) {
                    text += fmt::format(" {:#x}", offset);
                }
            }
            return text;
        });
    }
    SyncRecording();
    // Guest memory copies into staging read by this submission (copy threads).
    WaitHostCopies();
    if (host_read_barrier) {
#pragma push_macro("MemoryBarrier")
#undef MemoryBarrier
        const vk::MemoryBarrier to_host = {
            .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
            .dstAccessMask = vk::AccessFlagBits::eHostRead,
        };
        current_cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                                       vk::PipelineStageFlagBits::eHost, {}, to_host, {}, {});
#pragma pop_macro("MemoryBarrier")
    }
    Check(current_cmdbuf.end());

    const vk::Semaphore timeline = work_semaphore.Handle();
    info.AddSignal(timeline, signal_value);

    static constexpr std::array<vk::PipelineStageFlags, 2> wait_stage_masks = {
        vk::PipelineStageFlagBits::eAllCommands,
        vk::PipelineStageFlagBits::eColorAttachmentOutput,
    };

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .waitSemaphoreValueCount = info.num_wait_semas,
        .pWaitSemaphoreValues = info.wait_ticks.data(),
        .signalSemaphoreValueCount = info.num_signal_semas,
        .pSignalSemaphoreValues = info.signal_ticks.data(),
    };

    const vk::SubmitInfo submit_info = {
        .pNext = &timeline_si,
        .waitSemaphoreCount = info.num_wait_semas,
        .pWaitSemaphores = info.wait_semas.data(),
        .pWaitDstStageMask = wait_stage_masks.data(),
        .commandBufferCount = 1U,
        .pCommandBuffers = &current_cmdbuf,
        .signalSemaphoreCount = info.num_signal_semas,
        .pSignalSemaphores = info.signal_semas.data(),
    };

    ImGui::Core::TextureManager::Submit();
    // gow3 diagnostics: a submit the driver held for more than 8 ms (hitch reports: 70 ms in the
    // NVIDIA driver, kernel calls), with the images registered since the previous slow one.
    const auto submit_start = std::chrono::steady_clock::now();
    auto submit_result = instance.GetGraphicsQueue().submit(submit_info, info.fence);
    if (const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                   submit_start)
                              .count();
        ms > 8.0) {
        static u64 last_images = 0;
        const u64 images = Gow3Stats::images_registered.load(std::memory_order_relaxed);
        std::printf("Slow submit: %.1f ms in the driver (%u wait semaphores, %u signal); %llu images "
                    "registered since the last slow one\n",
                    ms, info.num_wait_semas, info.num_signal_semas,
                    static_cast<unsigned long long>(images - last_images));
        last_images = images;
    }
    if (submit_result==vk::Result::eErrorDeviceLost) { ReportCheckpoints(); DumpDeviceFault(instance); }
    ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");

    work_semaphore.Refresh();
    AllocateWorkerCommandBuffers();

    // Apply pending operations
    PopPendingOperations();
}

void Scheduler::PriorityPendingOpsThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuSchedPriorityPendingOpsRunner");

    while (!stoken.stop_requested()) {
        PendingOp op;
        {
            std::unique_lock lk(priority_pending_ops_mutex);
            priority_pending_ops_cv.wait(lk, stoken,
                                         [this] { return !priority_pending_ops.empty(); });
            if (stoken.stop_requested()) {
                break;
            }

            op = std::move(priority_pending_ops.front());
            priority_pending_ops.pop();
        }

        work_semaphore.Wait(op.gpu_tick);
        if (stoken.stop_requested()) {
            break;
        }

        op.callback();
    }
}

void DynamicState::Commit(const Instance& instance, const vk::CommandBuffer& cmdbuf) {
    // A null command buffer only updates the dirty flags, exactly as recording would.
    CommitWith(instance.IsDepthBoundsSupported(), instance.IsDynamicColorWriteMaskSupported(),
               instance.IsAttachmentFeedbackLoopLayoutSupported(), [&](auto&& command) {
                   if (cmdbuf) {
                       command(cmdbuf);
                   }
               });
}

} // namespace Vulkan
