// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/renderer_vulkan/hud_layout.h"

#include <chrono>
#include <deque>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <array>
#include <vector>
#include "common/recursive_lock.h"
#include "common/shared_first_mutex.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_bind_helper.h"
#include "video_core/renderer_vulkan/vk_camera_motion.h"
#include "video_core/renderer_vulkan/vk_constant_ring.h"
#include "video_core/renderer_vulkan/vk_scene_resolution.h"
#include "video_core/renderer_vulkan/vk_object_motion.h"
#include "video_core/renderer_vulkan/vk_draw_pipe.h"
#include "video_core/renderer_vulkan/vk_draw_prep.h"
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Core {
class MemoryManager;
}

namespace Vulkan {

/// gow3 diagnostics (GOW3_FAULT_STATS): which GPU command thread write into guest memory takes
/// a write fault (1 WriteData, 2 graphics WriteData inline, 3 compute WriteData, 4 DMA, 5 fence
/// fallback). Set around those writes.
inline thread_local int g_gpu_write_site = 0;
struct GpuWriteSite {
    int previous;
    explicit GpuWriteSite(int site) : previous{g_gpu_write_site} {
        g_gpu_write_site = site;
    }
    ~GpuWriteSite() {
        g_gpu_write_site = previous;
    }
};

class GraphicsPipeline;
class Runtime;

class Rasterizer {
public:
    explicit Rasterizer(const Instance& instance, Scheduler& scheduler, Runtime& runtime,
                        AmdGpu::Liverpool* liverpool);
    ~Rasterizer();

    [[nodiscard]] Runtime& GetRuntime() noexcept {
        return runtime;
    }

    [[nodiscard]] VideoCore::BufferCache& GetBufferCache() noexcept {
        return buffer_cache;
    }

    [[nodiscard]] TemporalUpscaler& GetUpscaler() noexcept {
        return *upscaler;
    }

    [[nodiscard]] VideoCore::TextureCache& GetTextureCache() noexcept {
        return texture_cache;
    }

    void Draw(bool is_indexed, u32 index_offset = 0, const PreparedDraw* prepared = nullptr);

    /// gow3: draw preparation workers (vk_draw_prep.h), fed and consumed by Liverpool.
    DrawPreparation& GetDrawPreparation() {
        return *draw_prep;
    }
    void DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 size, u32 max_count,
                      VAddr count_address, u16 vertex_sgpr_offset, u16 instance_sgpr_offset);

    void DispatchDirect();
    void DispatchIndirect(VAddr address, u32 offset, u32 size);

    void ScopeMarker(fmt::string_view fmt, fmt::format_args args, auto&& func) {
        if (host_markers_enabled) {
            ScopeMarkerBegin(fmt::vformat(fmt, args));
            func();
            ScopeMarkerEnd();
        } else {
            func();
        }
    }

    void ScopeMarkerBegin(const std::string_view& str, bool from_guest = false);
    void ScopeMarkerEnd(bool from_guest = false);
    void ScopedMarkerInsert(const std::string_view& str, bool from_guest = false);
    void ScopedMarkerInsertColor(const std::string_view& str, const u32 color,
                                 bool from_guest = false);

    void FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds);
    void CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds);
    u32 ReadDataFromGds(u32 gsd_offset);
    bool InvalidateMemory(VAddr addr, u64 size, bool assume_locks = false);
    /// GPU thread, before a write the guest can observe (see Scheduler::WaitHostCopies).
    void WaitHostCopies() {
        DrainDrawPipe();
        scheduler.WaitHostCopies();
    }

    /// gow3: GPU command thread: waits until the draw recording thread has recorded every
    /// draw handed to it (vk_draw_pipe.h); no-op on other threads.
    /// `line`/`function`: the caller, for the statistics of where stage A waits.
    void DrainDrawPipe(u32 reason = DrawPipe::ReasonRasterizer, u32 line = __builtin_LINE(),
                       const char* function = __builtin_FUNCTION());
    /// Runs `task(rasterizer, copy of data)` in order with the draws: on the draw recording
    /// thread while the draw pipeline is in use (PipelinedTasks), else here after a drain.
    using OrderedTask = void (*)(Rasterizer& rasterizer, const u8* data);
    /// Returns true when the task was handed to the recording thread (not run yet).
    bool RunInOrder(OrderedTask task, const void* data, u32 size,
                    u64 toggle = Gow3Toggle::PipelinedTasks);
    /// Stage A: the draw pipe position after the last handed-over packet, and whether the
    /// recording thread has run everything before a position.
    [[nodiscard]] u64 DrawPipeHead() const {
        return draw_pipe ? draw_pipe->Head() : 0;
    }
    [[nodiscard]] bool DrawPipeReached(u64 position) const {
        return !draw_pipe || draw_pipe->Reached(position);
    }
    /// Stage A: no draws waiting for the recording thread.
    [[nodiscard]] bool DrawPipeIdle() const {
        return !draw_pipe || draw_pipe->Idle();
    }
    /// Stage A, end of a submission: its prepared draws stay alive until the recording thread
    /// has recorded them (instead of a drain).
    void RetireSubmission();
    /// Stage A: guest memory the recording thread will write for work handed to it (storage
    /// buffers, DMA, WriteData, fences); constants overlapping it are bound there, not copied here.
    void NotePendingGpuWrite(VAddr address, u64 size);
    /// gow3: at an end-of-pipe event, the output of dispatches whose results the guest reads
    /// once it sees that event (eop_readbacks) is read back asynchronously with its pages
    /// read-protected: a guest read before the GPU is done waits for that readback only.
    /// (Delaying the event instead broke Uncharted 1: it is the frame's end-of-pipe tick, which
    /// FrameBegin asserts two frames later.)
    bool ReadBackEopResults();
    /// gow3: a fence label the runtime writes to guest memory (BufferCache::NoteCpuWrite).
    void NoteLabelWrite(VAddr address, u64 size) {
        buffer_cache.NoteCpuWrite(address, size);
    }
    /// Before a guest-visible write: fences deferred earlier are written first.
    void WaitDeferredSignals() {
        scheduler.WaitDeferredSignals();
    }
    /// Runs `signal` after the guest memory copies issued so far, without waiting here.
    /// gow3: draws go through the two-stage draw pipeline (multithreaded rendering).
    bool DrawPipeActive() const {
        return UseDrawPipe();
    }
    void SignalAfterHostCopies(std::function<void()> signal) {
        scheduler.SignalAfterHostCopies(std::move(signal));
    }
    /// The GPU command thread or the draw recording thread (fault handling runs inline there).
    bool IsGpuSideThread() const;
    bool IsGpuSideThreadId(u32 tid) const;
    /// A guest write hit a protected page.
    bool OnWriteFault(VAddr addr, bool assume_locks);
    /// gow3: a guest instruction keeps faulting on one page (see BufferCache::ReleaseStuckPage).
    void ReleaseStuckPage(VAddr page, bool is_write, bool assume_locks);
    bool ReadMemory(VAddr addr, u64 size, bool assume_locks = false);
    /// gow3: after a fault: lands a buffer readback that still read-protects the page of `addr`.
    void LandWatchedReadback(VAddr addr, bool assume_locks);
    void WaitDrawPipeForCapture();
    void ProcessDownloadImages();
    bool IsMapped(VAddr addr, u64 size);
    void MapMemory(VAddr addr, u64 size);
    void RegisterMemory(VAddr addr, u64 size);
    void UnmapMemory(VAddr addr, u64 size);

    u64 Flush();
    void Finish();
    void OnSubmit();

    PipelineCache& GetPipelineCache() {
        return pipeline_cache;
    }

    template <typename Func>
    void ForEachMappedRangeInRange(VAddr addr, u64 size, Func&& func) {
        const auto range = decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
        Common::RecursiveSharedLock lock{mapped_ranges_mutex};
        for (const auto& mapped_range : (mapped_ranges & range)) {
            func(mapped_range);
        }
    }

    std::thread::id GetGpuCommandProcessorThread();
#if defined(__linux__) || defined(_WIN32)
    u32 GetGpuCommandProcessorThreadId();
#endif

private:
    void PrepareRenderState(const GraphicsPipeline* pipeline);
    RenderState BeginRendering(const GraphicsPipeline* pipeline);
    RenderState BeginRenderingFull(const GraphicsPipeline* pipeline);
    /// gow3: a draw continuing the open render pass with the same inputs gets the same render
    /// state (RenderStateMemo); nothing can have broken the pass in between (barriers, copies and
    /// dispatches end it).
    struct BeginSignature {
        std::array<VideoCore::ImageId, AmdGpu::NUM_COLOR_BUFFERS + 1> ids{};
        std::array<VideoCore::ImageViewInfo, AmdGpu::NUM_COLOR_BUFFERS + 1> views{};
        u32 mrt_mask = 0;
        u32 num_samples = 0;
        std::array<u8, AmdGpu::NUM_COLOR_BUFFERS> color_samples{};
        bool motion = false;
        bool scene_started = false;
        bool raster_scaling = false;
        u64 upscaler_state = 0;
        u64 generation = 0;
        u32 depth_control = 0;
        bool depth_valid = false;
        bool stencil_valid = false;
        bool operator==(const BeginSignature&) const = default;
    };
    BeginSignature MakeBeginSignature(const GraphicsPipeline* pipeline) const;
    struct BeginMemo {
        bool valid = false;
        BeginSignature signature;
        RenderState state;
        u32 scene_size = 0;
        std::array<float, 2> target_scale{};
    } begin_memo;
    u64 begin_memo_hits = 0, begin_memo_misses = 0;
    void Resolve();
    void DepthStencilCopy(bool is_depth, bool is_stencil);
    void EliminateFastClear();

    void UpdateDynamicState(const GraphicsPipeline* pipeline, bool is_indexed) const;
    void UpdateViewportScissorState() const;
    void UpdateDepthStencilState() const;
    /// gow3: the squaring pipeline of a scaled MIN/MAX blend when this draw can use it.
    vk::Pipeline SquarePass(const GraphicsPipeline& pipeline) const;
    void UpdatePrimitiveState(bool is_indexed) const;
    void UpdateRasterizationState() const;
    void UpdateColorBlendingState(const GraphicsPipeline* pipeline) const;

    bool FilterDraw();
    bool FilterDrawPasses() const;
    /// Everything of a direct draw after the pipeline selection (GPU thread or stage B).
    void DrawRecord(const GraphicsPipeline* pipeline, const PreparedDraw* used_prepared,
                    bool is_indexed, u32 index_offset);
    static bool DrawPipeWanted();
    bool UseDrawPipe() const;
    bool OnStageA() const;
    /// Arguments of an indirect draw: `args` holds `max_count` commands `stride` apart.
    struct IndirectDraw {
        VAddr args;
        VAddr count; ///< the draw count, or 0 for max_count draws
        u32 stride;
        u32 max_count;
    };
    /// Everything of an indirect draw after the pipeline selection (GPU thread or stage B).
    void DrawIndirectRecord(const GraphicsPipeline* pipeline, bool is_indexed,
                            const IndirectDraw& indirect);
    /// Everything of an indirect dispatch after the pipeline selection.
    void DispatchIndirectRecord(const ComputePipeline* pipeline, VAddr args, u32 size);
    /// Hands a draw (or, with `cs`, a dispatch) to the recording thread.
    void PostDraw(const Pipeline* pipeline, const PreparedDraw* used_prepared, bool is_indexed,
                  u32 index_offset, const AmdGpu::ComputeProgram* cs = nullptr,
                  const IndirectDraw* indirect = nullptr);
    /// Everything of a direct dispatch after the pipeline selection (GPU thread or stage B).
    void DispatchRecord(const ComputePipeline* pipeline);
    /// The compute registers of the dispatch being recorded.
    const AmdGpu::ComputeProgram& CsRegs() const;
    static void RunDrawPacket(void* rasterizer, const u8* packet, u32 size);
    struct RingBinding;
    void CollectRingBindings(const Shader::Info& stage, const PreparedDraw* prepared,
                             boost::container::static_vector<RingBinding, Shader::NUM_BUFFERS>& out);
    void PrintPipeStats();
    /// The registers of the draw being recorded: stage B's copy there, else Liverpool's.
    const AmdGpu::Regs& Regs() const;
    AmdGpu::CbDbExtent CbExtent(u32 index) const;
    AmdGpu::CbDbExtent DbExtent() const;

    void BindBuffers(const Shader::Info& stage, const PreparedStage* prepared,
                     Shader::Backend::Bindings& binding,
                     Shader::PushData& push_data, u32& write_index);
    /// `on_helper`: run by the texture binding helper; out-of-date images are not refreshed.
    void BindTextures(const Shader::Info& stage, const PreparedStage* prepared,
                      Shader::Backend::Bindings& binding, u32& write_index, bool& barrier,
                      bool on_helper = false);
    bool BindResources(const Pipeline* pipeline);
    void BindSamplers(const Shader::Info& stage, const PreparedStage* prepared,
                      Shader::Backend::Bindings& binding, u32& write_index);
    /// gow3: a stage's resolved textures remembered by its prepared T# hashes
    /// (TextureSetMemo): a hit only redoes the per-draw effects (binding flags, transitions).
    struct TextureSetEntry {
        VideoCore::ImageId id{}; ///< after the depth redirect; null descriptor when invalid
        vk::ImageView view;
        const void* backing = nullptr;
        VideoCore::SubresourceRange range;
        bool proxy = false; ///< sampled from the scene proxy (view: SampleProxy on each use)
    };
    struct TextureSet {
        u64 key = 0;
        const Shader::Info* stage = nullptr;
        u64 generation = ~0ull;
        u64 scene_generation = 0; ///< SceneTargets::Generation of the proxy views
        u32 count = 0;
        static constexpr u32 MaxImages = 16; ///< larger sets are not memoized
        std::array<u64, MaxImages> hashes{};
        std::array<TextureSetEntry, MaxImages> entries{};
    };
    std::array<TextureSet, 32768> texture_sets{};
    /// gow3 21 Oct 2026 (GOW3_CPU_TRIMS=1): BindTextures' resolved set, reused (only the entries
    /// of the stage's images are reset; the GPU command thread alone memoizes sets).
    std::array<TextureSetEntry, TextureSet::MaxImages> texture_set_scratch{};
    /// gow3 21 Oct 2026 diagnostics (GOW3_FAULT_TIMES=1): see OnWriteFault.
    static void NoteWriteFaultTime(VAddr addr, bool gpu_side, const std::array<u64, 5>& steps);
    u64 texture_set_hits = 0, texture_set_misses = 0;
    std::array<u64, 4> texture_set_why{}; ///< misses: other key, generation, image check, new
    /// Returns true when the stage's images were bound from the memo.
    bool BindTexturesFromSet(const Shader::Info& stage, const PreparedStage* prepared,
                             u32 first_image_idx, bool& barrier, TextureSet*& slot);
    /// The prepared sharps of `stage` in the current prepared draw, if they fit it.
    const PreparedStage* FindPreparedStage(const Shader::Info& stage) const;
    /// Whether the helper may bind textures for this pipeline at all (fixed descriptor layout,
    /// no storage writes that invalidate cached images).
    bool HelperEligible(const Pipeline* pipeline) const;
    /// Whether every texture of `stage` takes the memoized path without refreshing, creating or
    /// redirecting anything that records commands or touches the buffer cache.
    bool TexturesBindableOnHelper(const Shader::Info& stage, const PreparedStage* prepared);
    static void RunTextureTask(void* rasterizer);
    static void JoinBindHelper(void* rasterizer);

    /// Display pass: counts the frame (Gow3Stats::gpu_frames) and, with GOW3_BUFFER_STATS, the lag.
    void NoteFrameStart();
    /// GOW3_GPU_PROFILE: a timestamp where a render pass starts (vk_gpu_profiler.h).
    void MarkPass(const GraphicsPipeline* pipeline, const RenderState& state);
    void BindVertexBuffers(const GraphicsPipeline* pipeline,
                           const PreparedDraw* prepared = nullptr);
    void BindIndexBuffer(u32 index_offset = 0);
    /// gow3: BindVertexBuffers/BindIndexBuffer in two halves: obtaining the buffers
    /// (vertex_binds, index_bind) and recording the binds.
    void ResolveVertexBuffers(const GraphicsPipeline* pipeline, const PreparedDraw* prepared);
    void EmitVertexBuffers();
    void ResolveIndexBuffer(u32 index_offset);
    void EmitIndexBuffer();

    void ResetBindings(bool is_compute);

    bool IsComputeMetaClear(const Pipeline* pipeline);
    bool IsComputeImageCopy(const Pipeline* pipeline);
    bool IsComputeImageClear(const Pipeline* pipeline);

private:
    friend class VideoCore::BufferCache;

    const Instance& instance;
    Scheduler& scheduler;
    Runtime& runtime;
    VideoCore::PageManager page_manager;
    VideoCore::BufferCache buffer_cache;
    VideoCore::TextureCache texture_cache;
    AmdGpu::Liverpool* liverpool;
    Core::MemoryManager* memory;
    boost::icl::interval_set<VAddr> mapped_ranges;
    Common::SharedFirstMutex mapped_ranges_mutex;
    /// gow3: bumped on every unmap; IsMapped's per-thread cache of mapped intervals checks it.
    std::atomic<u64> mapped_generation{1};
    PipelineCache pipeline_cache;
    std::unique_ptr<DrawPreparation> draw_prep;
    std::unique_ptr<CameraMotion> camera_motion; // gow3: motion vectors (docs/upscaler.md)
    std::unique_ptr<SceneTargets> scene_targets;
    bool scene_started = false;
    VAddr frame_display_address = 0; ///< display buffer of the last frame start
    std::unique_ptr<ObjectMotion> object_motion;
    bool motion_draw = false;
    u64 motion_geometry{};    ///< vertex-stream identity of the current direct draw
    bool gbuffer_draw = false;
    std::unique_ptr<TemporalUpscaler> upscaler; // gow3: FSR (docs/upscaler.md)
    std::array<float, 2> draw_jitter{};         ///< viewport offset of the current draw, pixels
    std::array<float, 2> target_scale{1.0f, 1.0f}; ///< pass drawn into the upscaler's output-size images
    // gow3: Uncharted 2 ultrawide HUD (hud_layout.h): the draws into a display buffer after the
    // frame copy are drawn into the centred area of hud_aspect.
    float hud_aspect = 0.0f;   ///< GOW3_HUD_ASPECT (0: off)
    HudLayout::Tracker hud_tracker; ///< which draws are the HUD's
    bool hud_draw = false;     ///< the current draw is the HUD's
    bool hud_wide = false;     ///< GOW3_HUD_MODE=wide: side elements at the screen edges
    HudLayout::Anchor hud_anchor = HudLayout::Anchor::Centre; ///< where the current HUD draw goes
    bool hud_trace = false;    ///< GOW3_HUD_TRACE: counts and shaders in the log
    float hud_k = 1.0f;        ///< this draw's horizontal squeeze (1: none)
    u32 hud_width = 0;         ///< and its target's width
    bool hud_split = false;    ///< wide mode: this draw is recorded once per anchor (a third each)
    u32 hud_height = 0;        ///< and height
    float hud_wide_aspect = 0.0f; ///< GOW3_HUD_WIDE_ASPECT: area of the side elements (0: screen)
    void NoteHudDraw(u64 ps_hash, bool indexed);
    /// The horizontal extent of a draw's vertex positions (its first float position attribute).
    bool HudExtent(const GraphicsPipeline* pipeline, bool is_indexed, u32 index_offset,
                   float& min_x, float& max_x, std::string* detail = nullptr) const;
    void NoteHudTarget(const RenderState& state);
    const bool host_markers_enabled;
    const bool guest_markers_enabled;

    using RenderTargetInfo = std::pair<VideoCore::ImageId, VideoCore::TextureCache::ImageDesc>;
    std::array<RenderTargetInfo, AmdGpu::NUM_COLOR_BUFFERS> cb_descs;
    std::pair<VideoCore::ImageId, VideoCore::TextureCache::ImageDesc> db_desc;
    boost::container::static_vector<vk::DescriptorImageInfo, Shader::NUM_IMAGES> image_infos;
    boost::container::static_vector<vk::DescriptorBufferInfo, Shader::NUM_BUFFERS> buffer_infos;
    boost::container::static_vector<VideoCore::ImageId, Shader::NUM_IMAGES> bound_images;
    struct BoundBuffer {
        const VideoCore::Buffer* buffer;
        u64 offset;
        u32 size;
        bool is_written;
    };
    boost::container::static_vector<BoundBuffer, Shader::NUM_BUFFERS> bound_buffers;
    /// gow3: arenas the current draw's DMA reads (SyncDmaMemory windows), for barriers.
    boost::container::small_vector<const VideoCore::Buffer*, 4> dma_read_arenas;

    u32 set_write_index{};
    Pipeline::DescriptorWrites set_writes;
    Shader::PushData push_data;

    // gow3: bindings point at their description instead of copying it (a hot spot): into
    // image_desc_cache for memoized lookups (pinned for the current BindTextures call), else
    // into image_desc_storage.
    using ImageBindingInfo = std::pair<VideoCore::ImageId, const VideoCore::TextureCache::ImageDesc*>;
    // gow3: texture descriptions depend only on the T# and three resource flags; building
    // them (mip layout sizes) for every binding of every draw was a hot spot.
    struct ImageDescCacheEntry {
        std::array<u64, 4> sharp{};
        u32 flags = ~0u;
        VideoCore::TextureCache::ImageDesc desc;
        // Memoized FindImage for bindings without mip overrides (TextureBindingMemo).
        u64 found_generation = ~0ULL;
        VideoCore::ImageId found_id{};
        VideoCore::TextureCache::ImageDesc found_desc;
        u64 pinned = 0; ///< bind_epoch of the BindTextures call referencing found_desc
        u64 last_use = 0;
        // Memoized FindView for found_id (after the depth redirect): valid while the entry's
        // FindImage memo holds and the image keeps this backing (TextureViewMemo).
        VideoCore::TextureCache::ViewMemo view_memo;
    };
    std::array<ImageDescCacheEntry, 4096> image_desc_cache{};
    u64 desc_use_counter = 0;
    /// Entries replacing pinned cache slots during one BindTextures call.
    boost::container::static_vector<ImageDescCacheEntry, Shader::NUM_IMAGES> image_desc_overflow;
    boost::container::static_vector<VideoCore::TextureCache::ImageDesc, Shader::NUM_IMAGES * 2>
        image_desc_storage;
    u64 bind_epoch = 0;
    // gow3: render/depth target lookups memoized by their raw register bytes while image
    // registrations are unchanged (the descriptions depend only on those registers).
    struct TargetMemo {
        std::array<u8, 256> key{};
        u32 key_size = 0;
        u64 generation = ~0ULL;
        VideoCore::ImageId image_id{};
        VideoCore::TextureCache::ImageDesc desc;
    };
    std::array<TargetMemo, 64> target_memo{};
    // gow3: consecutive draws mostly keep their targets; the slot's description (cb_descs,
    // db_desc) is then still the right one and is neither looked up nor copied.
    struct LastTarget {
        std::array<u8, 256> key{};
        u32 key_size = 0;
        u64 generation = ~0ULL;
        VideoCore::ImageId image_id{};
    };
    std::array<LastTarget, AmdGpu::NUM_COLOR_BUFFERS + 1> last_targets{}; ///< CBs, then DB
    template <typename... Parts>
    VideoCore::ImageId FindTargetMemoized(VideoCore::TextureCache::ImageDesc& desc,
                                          LastTarget& last, auto&& make_desc,
                                          const Parts&... parts);
    /// The prepared draw whose pipeline the current Draw uses (its sharps are valid), or null.
    const PreparedDraw* bind_prepared = nullptr;
    /// `hash` is ImageDescHash(sharp, res), computed by a draw-preparation worker or here.
    ImageDescCacheEntry& CachedImageDescEntry(const AmdGpu::Image& sharp,
                                              const Shader::ImageResource& res, u64 hash);
    ImageDescCacheEntry& CachedImageDescEntry(const AmdGpu::Image& sharp,
                                              const Shader::ImageResource& res) {
        return CachedImageDescEntry(sharp, res, ImageDescHash(sharp, res));
    }
    const VideoCore::TextureCache::ImageDesc& CachedImageDesc(const AmdGpu::Image& sharp,
                                                              const Shader::ImageResource& res) {
        return CachedImageDescEntry(sharp, res).desc;
    }
    boost::container::static_vector<ImageBindingInfo, Shader::NUM_IMAGES> image_bindings;
    struct VertexBinds {
        VertexInputs<vk::VertexInputAttributeDescription2EXT> attributes;
        VertexInputs<vk::VertexInputBindingDescription2EXT> bindings;
        VertexInputs<vk::Buffer> host_buffers;
        VertexInputs<vk::DeviceSize> host_offsets;
        VertexInputs<vk::DeviceSize> host_sizes;
        VertexInputs<vk::DeviceSize> host_strides;
        u32 num_buffers = 0;
    } vertex_binds;
    struct IndexBind {
        vk::Buffer handle;
        u64 offset;
        vk::IndexType type;
    } index_bind{};
    /// The direct draw whose vertex/index buffers BindResources may resolve early.
    struct DrawInputs {
        const GraphicsPipeline* pipeline;
        const PreparedDraw* prepared;
        u32 index_offset;
        bool is_indexed;
        bool pending;
        bool resolved;
    } draw_inputs{};
    // gow3: textures of a draw bound on the helper thread while this thread binds buffers.
    static bool BindHelperWanted();
    BindHelper bind_helper{BindHelperWanted()};
    struct TextureTask {
        struct Stage {
            const Shader::Info* info;
            const PreparedStage* prepared;
            Shader::Backend::Bindings binding;
            u32 write_index;
        };
        std::array<Stage, Shader::MaxStageTypes> stages;
        u32 count = 0;
        u32 completed = 0; ///< stages the helper bound; the rest are bound after the join
        bool barrier = false;
    } texture_task;
    /// Draws whose textures the helper bound completely, partly or not at all (GOW3_FRAME_STATS).
    u64 helper_full = 0, helper_partial = 0, helper_serial = 0;
    /// The memoized cache entry of each image binding (FindImage memo taken), else null.
    boost::container::static_vector<ImageDescCacheEntry*, Shader::NUM_IMAGES> image_binding_entries;
    bool fault_process_pending{};
    // gow3: small shader storage writes since the last submission (GOW3_SYNC_GPU_CONSTANTS).
    std::vector<std::pair<VAddr, u64>> small_gpu_writes;
    /// Ranges downloaded after every submission (GOW3_READBACK_TRIGGER "range <lo> <hi>").
    std::vector<std::pair<VAddr, u64>> synced_ranges;
    /// gow3: storage written by shaders whose results the guest reads right after the next
    /// end-of-pipe event (ReadBackEopResults). Uncharted 1's mesh ray casts (bullet impacts,
    /// blood and other decals): the event's interrupt marks the batch done and the game reads
    /// the hits at once; fences are written when recorded, so it read stale results (no hits).
    std::vector<std::pair<VAddr, u64>> eop_readbacks;
    /// gow3 18 Oct: ray-cast results (all EOP readback shaders but the 6a6422a9 copies) are read
    /// back without protecting their pages: the guest reads them after the fence, which is
    /// written once they land (GOW3_DEFER_RAYCAST_FENCES). Protected, every guest write into
    /// those pages (the next batch) faulted and waited for the GPU thread and the readback:
    /// Uncharted 3's ray-cast thread stalled with the ray-cast lock held (rooftops: 2-3 fast
    /// frames, then one of 25-35 ms). Opt-in, GOW3_OPEN_RAYCAST_READBACKS=1: measured on the
    /// rooftops it removed the faults on ray-cast pages but not the uneven frames (18 Oct).
    std::vector<std::pair<VAddr, u64>> eop_open_readbacks;
    std::vector<std::pair<VAddr, u64>> eop_soft_readbacks; ///< gow3: tiny images, unprotected
    struct ClothPeek {
        VAddr address;
        u64 size;
        u64 shader;
        u32 slot;
        bool written;
        const VideoCore::Buffer* buffer; ///< the buffer bound (arena or stream buffer)
        u64 buffer_offset;
    };
    std::vector<ClothPeek> cloth_peeks; ///< gow3 diagnostics (GOW3_CLOTH_PEEK)
    bool sync_gpu_constants = [] {
        const char* env = std::getenv("GOW3_SYNC_GPU_CONSTANTS");
        return env && env[0] == '1';
    }();
    void SyncSmallGpuWrites();
    /// gow3: before the SRT walker of `info` runs on the GPU command thread: the tables it reads
    /// that hold GPU-written bytes not yet in guest memory are downloaded (one GPU wait).
    void SyncSrtTables(const Shader::Info& info);
    struct SrtNode {
        s32 parent;      ///< -1: a pointer in the user data
        u32 offset;      ///< byte offset of the pointer in the user data or the parent table
        u32 extent;      ///< bytes the walker reads from this table
        bool dynamic;    ///< the pointer's offset is computed (not followed)
    };
    struct SrtCopy {
        u32 flat_dw;     ///< destination dword in the flattened user data
        s32 node;        ///< source table (-1: the user data)
        u32 src_off;     ///< byte offset in that table
    };
    /// Tables a walker reads and what it copies from them, decoded from its code (parents
    /// first), per walker.
    struct SrtPlan {
        std::vector<SrtNode> nodes;
        std::vector<SrtCopy> copies;
    };
    std::unordered_map<const void*, SrtPlan> srt_plans;
    /// gow3: recent walkers and their plans (SrtPlanFor).
    static constexpr u32 SrtPlanMemoSize = 32;
    std::array<std::pair<const void*, const SrtPlan*>, SrtPlanMemoSize> srt_plan_memo{};
    const SrtPlan& SrtPlanFor(const Shader::Info& info);
    /// Guest address of each plan table for this user data (0: unknown), without downloads.
    void ResolveSrtTables(const SrtPlan& plan, const Shader::Info& info, VAddr* address);
public:
    /// gow3 diagnostics (GOW3_SRT_CHECK): the guest address the walker copies flattened dword
    /// `flat_dw` from for this user data (0: unknown) and what the caches know about it.
    VAddr SrtSourceAddress(Shader::PFN_SrtWalker walker, size_t walker_size,
                           std::span<const u32> user_data, u32 flat_dw, const char** kind);
    /// gow3 diagnostics (GOW3_SRT_CHECK, 21 Oct 2026): the draw whose resource tables are being
    /// walked (colour target 0, depth target, index buffer, index count), so a table the game
    /// completed late can be matched to a pass and a mesh of a frame capture.
    struct SrtDrawContext {
        VAddr color0;
        VAddr depth;
        VAddr index;
        u32 num_indices;
    };
    SrtDrawContext CurrentDrawContext() const;

private:
    /// Diagnostics for a garbage V#: where the walker took flattened dword `flat_dw` from.
    void DescribeSharpSource(const Shader::Info& info, u32 flat_dw, const char* problem);
    /// gow3 diagnostics: a resource-table walk faulted: the tables it read, now and 3 ms later.
    void ReportSrtFault(const Shader::Info& info);
    u64 srt_syncs = 0, srt_sync_reports = 0;
    void SyncDmaMemory(const Shader::Info* stage);
    /// VS of the draw whose dynamic state is being set (diagnostic overrides).
    mutable u32 dynamic_vs_hash = 0;
    /// gow3 21 Oct 2026: its pixel shader (GOW3_DEPTH_PROBE_FILE shader filter).
    mutable u32 dynamic_ps_hash = 0;
    static inline std::atomic<bool> shadow_clear_far{false};
    static inline std::atomic<bool> depth16_clear_one{false};
    static inline std::atomic<bool> depth_bias_off{false};
    bool attachment_feedback_loop{};
    bool needs_barrier{};
    /// gow3 (GOW3_SKIP_COMPUTE_REBIND): the compute pipeline the last DispatchRecord bound and
    /// the scheduler's RecordSerial() right after its dispatch.
    vk::Pipeline bound_compute_pipeline{};
    u64 bound_compute_serial = ~u64{0};
    // gow3: two-stage draw pipeline (stage B state; the thread is the last member so it
    // stops first).
    static inline thread_local const AmdGpu::Regs* stage_regs = nullptr;
    static inline thread_local const AmdGpu::ComputeProgram* stage_cs = nullptr;
    AmdGpu::Regs pipe_regs{};
    bool pipe_synced = false;
    std::array<AmdGpu::CbDbExtent, AmdGpu::NUM_COLOR_BUFFERS> pipe_cb_extent{};
    AmdGpu::CbDbExtent pipe_db_extent{};
    /// Stage A: small read-only guest buffers copied into the constant ring for a packet.
    struct RingBinding {
        u32 index;  ///< buffer resource of the stage
        u32 size;
        u64 offset; ///< in the ring
        VAddr address;
    };
    struct PendingWrite {
        VAddr begin;
        VAddr end;
        u64 position;
    };
    std::vector<PendingWrite> pending_writes;
    VAddr pending_min = ~VAddr{0}, pending_max = 0; ///< bounds of pending_writes
    u32 pending_checks = 0;
    /// gow3 21 Oct 2026 (GOW3_PIPE_WRITE_END): writes noted for the packet PostDraw is building
    /// take this position until it is committed (then its end: stage B has run it once there).
    static constexpr u64 PendingUncommitted = ~u64{0};
    bool noting_packet = false;
    std::vector<u32> uncommitted_indices; ///< pending_writes entries to set at the commit
    /// gow3 21 Oct 2026 (GOW3_SRT_PENDING_DRAIN): destinations of the copy shaders stage B runs
    /// (scalar copy HLE etc.), which the next draws' resource tables read. Small (they merge).
    bool noting_copy = false, copy_uncommitted = false;
    std::vector<PendingWrite> pending_copy_writes;
    VAddr copy_min = ~VAddr{0}, copy_max = 0;
    bool PendingCopyWriteOverlaps(VAddr address, u64 size);
    /// gow3 21 Oct 2026 (GOW3_SRT_PENDING_DRAIN): walks that drained the pipe for a pending write.
    u64 srt_pending_drains = 0, srt_pending_changed = 0, srt_pending_zero = 0, srt_pending_ns = 0;
    u64 proxy_samples = 0; ///< texture bindings that read a scene proxy (statistics)
    float sampler_lod_bias = 0.0f; ///< gow3: extra bias of this draw's samplers
    bool scene_debug_frame = false; ///< GOW3_SCENE_DEBUG: this frame's passes are printed
    bool PendingWriteOverlaps(VAddr address, u64 size);
    /// gow3 21 Oct 2026: stage A waits until stage B has run the packets that write the range
    /// (`writes`; not the whole pipe: the work queued after them keeps stage B busy meanwhile).
    void WaitPendingWrites(const std::vector<PendingWrite>& writes, VAddr address, u64 size);
    /// Stage B: the ring bindings of the stages of the packet being recorded.
    struct RingStage {
        const Shader::Info* info;
        const RingBinding* bindings;
        u32 count;
    };
    std::array<RingStage, Shader::MaxStageTypes> ring_stages{};
    u32 num_ring_stages = 0;
    const RingBinding* FindRingBinding(const Shader::Info& stage, u32 index) const {
        for (u32 i = 0; i < num_ring_stages; ++i) {
            if (ring_stages[i].info == &stage) {
                for (u32 j = 0; j < ring_stages[i].count; ++j) {
                    if (ring_stages[i].bindings[j].index == index) {
                        return &ring_stages[i].bindings[j];
                    }
                }
                return nullptr;
            }
        }
        return nullptr;
    }
    std::unique_ptr<ConstantRing> constant_ring;
    // gow3: Rasterizer::Flush calls (either pipe stage), and the GOW3_RING_STATS counters.
    std::atomic<u64> flush_count{0};
    u64 ring_copy_count = 0, ring_copy_bytes = 0, ring_repeat_count = 0, ring_repeat_bytes = 0;
    std::array<u64, 4> ring_size_buckets{};
    std::unordered_set<u64> ring_seen;
    u64 ring_seen_flush = ~0ULL;
    /// Submissions (prepared draws) kept alive until stage B reaches the position.
    std::deque<std::pair<u64, std::shared_ptr<const void>>> pipe_keepalive;
    std::unique_ptr<DrawPipe> draw_pipe;
};

} // namespace Vulkan
