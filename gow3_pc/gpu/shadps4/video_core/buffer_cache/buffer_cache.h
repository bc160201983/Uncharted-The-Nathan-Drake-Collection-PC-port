// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <shared_mutex>

#include <format>
#include <string>

#include "gow3_copy.h"

#include <array>
#include <atomic>
#include <bitset>
#include <deque>
#include <map>
#include <memory>
#include <unordered_map>
#include <functional>
#include <unordered_set>
#include <utility>
#include <boost/container/small_vector.hpp>

#include "common/interval_set.h"
#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/fault_manager.h"
#include "video_core/buffer_cache/range_set.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Core {
class MemoryManager;
}

namespace Vulkan {
class GraphicsPipeline;
struct SubmitInfo;
class Runtime;
class StagingBufferPool;
} // namespace Vulkan

namespace VideoCore {

/// gow3 diagnostics (GOW3_STREAM_STATS=1): the binding ObtainBuffer serves (shader << 8 | slot)
/// and the bytes the shader reaches in it (0 = unknown), set by the rasterizer before the call.
inline thread_local u64 g_stream_stats_key = 0;
/// gow3: the last ObtainBuffer served a static range from the arena (UseArenaForStatic).
inline thread_local bool g_obtained_static = false;
inline thread_local u64 g_stream_stats_used = 0;

/// gow3: an allocator whose new elements are default-initialized: resize() leaves bytes as they
/// are instead of zeroing them (buffers that are filled completely right after, see
/// GOW3_READBACK_POOL in buffer_cache.cpp). resize(n, value) still fills.
template <typename T>
struct UninitAllocator : std::allocator<T> {
    template <typename U>
    struct rebind {
        using other = UninitAllocator<U>;
    };
    UninitAllocator() = default;
    template <typename U>
    UninitAllocator(const UninitAllocator<U>&) noexcept {}
    template <typename U>
    void construct(U* p) noexcept(std::is_nothrow_default_constructible_v<U>) {
        ::new (static_cast<void*>(p)) U;
    }
    template <typename U, typename... Args>
    void construct(U* p, Args&&... args) {
        ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
    }
};
using SnapshotBytes = std::vector<u8, UninitAllocator<u8>>;

/// gow3 diagnostics (GOW3_FAULT_STATS=1 GOW3_READ_SOURCES=1): records which shader wrote GPU data into each 1 MiB
/// window, listed with the windows the CPU reads GPU data from.
void NoteGpuWriter(VAddr address, u64 size, u64 shader_hash);
/// gow3 diagnostics (GOW3_READBACK_STATS=1): why a range joins the per-submission readback
/// (ReadbackKind bits), shown per 1 MiB window with the readback bytes every 5 s.
enum ReadbackKind : u32 {
    ReadbackSmallWrite = 1,  ///< storage write of 4 KiB or less (64 KiB window around it)
    ReadbackHotWindow = 2,   ///< part of a large storage write in a CPU-write-hot 1 MiB window
    ReadbackCpWrite = 4,     ///< CP fill or copy of 4 KiB or less
    ReadbackSharedPage = 8,  ///< page the CPU shares with GPU data in flight
    ReadbackEop = 16,        ///< ray-cast results / 6a6422a9 copies (fence waits)
};
bool ReadbackStatsEnabled();
void NoteReadbackKind(VAddr address, u64 size, u32 kind);

/// gow3: GPU-to-guest downloads done by this thread (fault statistics, page_manager.cpp).
inline thread_local u64 gow3_downloads = 0;


class TextureCache;
class MemoryTracker;
class PageManager;

class BufferCache {
    static constexpr u64 ADDRESS_SPACE_BITS = 40;
    // gow3: arena pages are 4 GiB unless the driver's buffer size limit is lower (AMD's
    // Windows driver: 2 GiB); an arena spans up to two pages, so they shrink to half the limit.
    static constexpr u64 MAX_ARENA_PAGE_BITS = 32;
    static constexpr u64 MIN_ARENA_PAGE_BITS = 30;
    static constexpr u64 MAX_ARENA_PAGES = u64{1} << (ADDRESS_SPACE_BITS - MIN_ARENA_PAGE_BITS);
    static constexpr u64 MIN_BLOCK_SIZE = 16_KB;

public:
    /// Read-only bindings up to this size are copied into a stream buffer (gow3: public for
    /// the draw pipeline's constant ring).
    static constexpr u64 STREAM_THRESHOLD = 16_KB;

    explicit BufferCache(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                         Vulkan::Runtime& runtime, AmdGpu::Liverpool* liverpool,
                         TextureCache& texture_cache, PageManager& tracker);
    ~BufferCache();

    /// Returns a pointer to GDS device local buffer.
    [[nodiscard]] const Buffer* GetGdsBuffer() const noexcept {
        return &gds_buffer;
    }

    /// Retrieves the device local DBA page table buffer.
    [[nodiscard]] Buffer* GetBdaPageTableBuffer() noexcept {
        return bda_pagetable_buffer.get();
    }

    /// Retrieves the fault buffer.
    [[nodiscard]] Buffer* GetFaultBuffer() noexcept {
        return fault_manager->GetFaultBuffer();
    }

    /// Retrieves the stream buffer.
    StreamBuffer& GetStreamBuffer() noexcept {
        return stream_buffer;
    }

    /// Returns minimum granularity of a sparse memory bind.
    u32 GetSparsePageShift() const noexcept {
        return block_shift;
    }

    /// Invalidates any buffer in the logical page range.
    void InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks = false);

    /// gow3: a guest write fault at `device_addr` was handled; unprotect its neighbourhood.
    void ExtendWriteFault(VAddr device_addr);
    /// gow3: a guest instruction keeps faulting on `page` (PageManager counts the repeats): the
    /// pending readbacks that read-protect it land now (polled, or waited for).
    void ReleaseStuckPage(VAddr page, bool is_write, bool assume_locks);

    /// Flushes any GPU modified buffer in the logical page range back to CPU memory.
    void ReadMemory(VAddr device_addr, u64 size, bool is_write = false, bool assume_locks = false);

    /// gow3: 1 MiB windows where the CPU wrote into GPU-written memory (each write needed a
    /// download with a full GPU wait). GPU writes there join the per-submission download
    /// (Rasterizer::SyncSmallGpuWrites) so later CPU writes find the pages clean. GPU-side use.
    /// gow3 16 Oct: windows cool down 3 s after the last such CPU write (GOW3_HOT_WINDOW_TTL=<s>;
    /// 0 keeps them hot for good). Kept hot, every window the effect shaders (cs 8f067540,
    /// 7d7e7206) ever shared with the CPU was read back whole after each submission although the
    /// CPU no longer touched it: ~500 MB/s in Uncharted 2 and 3 (the CPU reads none of it, precise
    /// readback statistics). The cooldown was once blamed for Uncharted 2's floating NPCs; that
    /// was the 6a6422a9 copies (now EOP readbacks), and the NPCs stand on the floor with it.
    [[nodiscard]] bool IsCpuWriteHot(VAddr address, u64 size) const {
        if (cpu_write_hot.empty()) {
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        for (u64 window = address >> 20; window <= (address + size - 1) >> 20; ++window) {
            if (const auto it = cpu_write_hot.find(window);
                it != cpu_write_hot.end() && (!HotWindowTtl().count() || now - it->second < HotWindowTtl())) {
                return true;
            }
        }
        return false;
    }
    static std::chrono::seconds HotWindowTtl() {
        static const std::chrono::seconds ttl{[] {
            const char* env = std::getenv("GOW3_HOT_WINDOW_TTL");
            return env ? std::atoll(env) : 3LL;
        }()};
        return ttl;
    }

    /// gow3: copies the GPU-modified parts of the ranges back to guest memory with one GPU wait
    /// (GPU thread). For data the GPU writes and CPU-side code (the SRT walker) reads.
    void DownloadRanges(std::span<const std::pair<VAddr, u64>> ranges);

    /// gow3: GPU-written data newer than guest memory lies in [address, address + size).
    [[nodiscard]] bool IsRangeGpuWritten(VAddr address, u64 size) const {
        return gpu_modified_ranges.Intersects(address, size);
    }

    /// gow3: IsRangeGpuWritten from the GPU command thread while the draw recording thread may
    /// change the set (multithreaded rendering: SRT sync before a walk). The set is not thread
    /// safe: a lookup during an update followed a freed node (Uncharted 3 crashed within minutes).
    [[nodiscard]] bool IsRangeGpuWrittenShared(VAddr address, u64 size) const {
        if (!MayBeGpuWritten(address, size)) {
            return false;
        }
        std::shared_lock lock{gpu_ranges_mutex};
        return gpu_modified_ranges.Intersects(address, size);
    }

    /// gow3 17 Oct 2026: false when no GPU write ever reached a 64 KiB window of the range (one
    /// bit per window, set by GpuRangesAdd and never cleared): lock-free. The SRT walk checked
    /// every table of every draw under the set's lock (~5% of Uncharted 3's GPU command thread).
    [[nodiscard]] bool MayBeGpuWritten(VAddr address, u64 size) const {
        if (size == 0) {
            return false;
        }
        const u64 first = address >> 16;
        const u64 last = (address + size - 1) >> 16;
        if (last >= GpuWindowBits) {
            return true;
        }
        for (u64 w = first; w <= last; ++w) {
            if (gpu_window_bits[w >> 6].load(std::memory_order_relaxed) & (1ull << (w & 63))) {
                return true;
            }
        }
        return false;
    }

    /// gow3: the GPU thread is about to write [address, address + size) in guest memory itself
    /// (shader HLE). Unprotects it like a guest write fault would, without a download. False
    /// when GPU-written bytes are in the range: the GPU must do that write.
    bool PrepareHostWrite(VAddr address, u64 size);

    /// gow3: a small write the GPU thread makes for the guest (copy shader HLE, CP fills and
    /// copies), done at this point of the command stream in guest memory (backing view) and in
    /// the GPU's copy alike. The bytes stop being GPU data: no download, no protection change,
    /// and pending readbacks do not bring older GPU values back over them. False (nothing done)
    /// when the range is unsuitable (unaligned, over 64 KiB, unmapped).
    bool WriteThrough(VAddr address, const void* data, u64 size);

    /// gow3: the runtime writes [address, address + size) through the backing view (fence
    /// labels): the bytes stop being GPU data, and pending readbacks do not bring older GPU
    /// values back over them. Uncharted 2: a ray-cast readback covering the frame's end-of-pipe
    /// label restored its old value (assert m_gfxEopTick in FrameBegin).
    void NoteCpuWrite(VAddr address, u64 size);

    /// gow3 statistics: sizes of the cache's containers (growth over time).
    std::string DebugSizes() const {
        std::size_t entries = 0;
        for (const auto& [page, writes] : page_writes) {
            entries += writes.size();
        }
        return std::format("{} page write records ({} entries), {} lazy pages, {} hot windows, {} "
                           "arenas, {} pending readbacks, {} GPU-written ranges",
                           page_writes.size(), entries, lazy_pages.size(), cpu_write_hot.size(),
                           arenas.size(), pending_readbacks.size(), gpu_modified_ranges.Count());
    }
    /// Labels NoteCpuWrite found inside a pending readback since the last call (statistics).
    static u32 TakeLabelsInReadbacks();

    /// gow3: DownloadRanges without the GPU wait. The copies run after the work already
    /// recorded; PollReadbacks writes them to guest memory once the GPU has run them. Until
    /// then the ranges stay GPU-modified (a CPU write into them waits for its readback).
    /// `protect`: its pages are read-protected until it completes (a read waits for it), as
    /// for every readback with GOW3_ASYNC_READBACK=1.
    /// `yield_to_cpu`: a CPU write into a piece drops its GPU values instead of waiting for
    /// them (the guest refills the buffer; see Rasterizer::ReadBackEopResults).
    /// gow3: `never_protect`: the pages stay accessible whatever the readback mode (results
    /// the guest reads only after a fence written when they land).
    void DownloadRangesAsync(std::span<const std::pair<VAddr, u64>> ranges, bool protect = false,
                             bool yield_to_cpu = false, bool never_protect = false);

    /// gow3 diagnostics (GOW3_CLOTH_PEEK=1): copies the GPU's data of [address, size) into a
    /// staging buffer (guest memory untouched); PollPeeks scans finished copies for exploded
    /// floats (Uncharted 3's cloth flicker, 16 Oct).
    void PeekGpuRange(VAddr address, u64 size, u64 tag, u32 slot, u64 dispatch = 0, u32 phase = 1);
    /// The same for the exact buffer a shader binds (an arena or the stream buffer).
    void PeekGpuBuffer(const Buffer* source, u64 offset, VAddr address, u64 size, u64 tag, u32 slot,
                       u64 dispatch, u32 phase);
    void PollPeeks();

    /// gow3: writes the readbacks the GPU has finished to guest memory (GPU-side, per draw).
    void PollReadbacks() {
        if (!pending_readbacks.empty()) {
            CompleteReadbacks(false);
        }
    }


    /// Finds a buffer for the specified region.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBuffer(VAddr device_addr, u32 size,
                                                             bool is_written,
                                                             bool is_texel_buffer = false);

    /// Attempts to obtain a buffer without modifying the cache contents.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBufferForImage(VAddr device_addr, u32 size);

    /// Return true when a region is modified from the CPU
    [[nodiscard]] bool IsRegionCpuModified(VAddr addr, size_t size);

    /// Return true when a region is modified from the GPU
    [[nodiscard]] bool IsRegionGpuModified(VAddr addr, size_t size);

    /// Processes the fault buffer.
    void ProcessFaultBuffer();

    /// Synchronizes all buffers needed for DMA.
    void SynchronizeDmaBuffers();
    /// gow3: SynchronizeDmaBuffers limited to the resident memory in [address, address + size).
    void SynchronizeDmaRange(VAddr address, u64 size);

    /// Commits pending sparse buffer memory binds. Must be called before every scheduler submit.
    void SubmitPendingArenaBinds(Vulkan::SubmitInfo& info);

    /// gow3 20 Oct 2026 (GitHub #95): gives back the arena memory of 1 MiB windows the game has
    /// not used for a while, once the arenas hold more than their limit; called per submission
    /// (does its work every 2 s).
    void ReclaimArenaMemory();

private:
    struct ArenaBinds {
        const Buffer* arena;
        boost::container::small_vector<vk::SparseMemoryBind, 32> binds;
    };

    ArenaBinds* BindsForArena(const Buffer* arena) {
        auto it = std::ranges::find(pending_binds, arena, &ArenaBinds::arena);
        if (it != pending_binds.end()) {
            return std::addressof(*it);
        }
        return &pending_binds.emplace_back(arena);
    }

    const Buffer* GetArena(u64 first_block, u64 last_block);

    void EnsureResident(const Buffer* arena, u64 first_block, u64 last_block);

    /// gow3 1 Nov 2026: with a sparse page table, binds (and clears) the pages of it holding the
    /// entries of blocks [first_block, end_block) that have no memory yet.
    void BindPageTablePages(u64 first_block, u64 end_block);
    bool ZeroPageTableChunk(vk::DeviceMemory memory, u64 size);

    void DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size);

    /// gow3: a CPU write fault next to, not on, GPU-written bytes: unprotects the page without
    /// a download (see LazyPage). False when the download is needed.
    bool TryLazyWrite(VAddr device_addr, u64 size);

    /// gow3: records the stale guest copy of the GPU-written bytes in the pages of a CPU write
    /// that is about to unprotect them (see LazyPage).
    void SnapshotLazyPages(VAddr address, u64 size);

    /// gow3: calls func(address, size) for the parts of an upload range to copy: GPU-written
    /// bytes of lazy pages that the CPU has not overwritten keep the GPU's value.
    template <typename Func>
    void ForEachLazyUploadPiece(VAddr address, u64 size, Func&& func);

    /// gow3: writes downloaded GPU data to guest memory, except bytes of lazy pages that the
    /// CPU overwrote after the GPU (the CPU value is newer; whole 64-byte lines with the
    /// readback guard, shared pages included). For an asynchronous readback,
    /// `rewritten` holds the bytes the GPU wrote again since: their lazy records are moved to
    /// the written value (still stale), the others are dropped.
    void WriteBackGpuData(VAddr address, const u8* data, u64 size,
                          const RangeSet* rewritten = nullptr);

    /// gow3: forgets the lazy pages in [address, address + size) (their GPU data was downloaded).
    void EraseLazyPages(VAddr address, u64 size);

    /// gow3: writes finished asynchronous readbacks to guest memory, oldest first. With
    /// `wait_until` set, waits for the GPU up to that readback tick (~0: all of them).
    void CompleteReadbacks(bool wait, u64 wait_until = ~0ULL);

    /// gow3: the latest tick of the pending readbacks with bytes in [address, address + size),
    /// or 0. With `newest_only`, only readbacks holding the newest GPU data of those bytes
    /// count (not rewritten by the GPU since).
    u64 PendingReadbackTick(VAddr address, u64 size, bool newest_only) const;

    bool SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size, bool is_written,
                           bool is_texel_buffer);

    /// gow3: batched small copies on the copy threads (Gow3Copy::QueueCopy).
    static void RunGuestCopy(const Gow3Copy::Item& item);
    void SmallGuestCopy(const Gow3Copy::Item& item);

    const Buffer* UploadCopies(const Buffer* arena, std::span<vk::BufferCopy> copies,
                               size_t total_size_bytes);

    bool SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size);

    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    // gow3: texel buffer ranges known not to alias an image, per image registry generation.
    struct ImageMiss {
        VAddr address = 0;
        u32 size = 0;
        u64 generation = ~0ULL;
    };
    std::array<ImageMiss, 256> image_miss_cache{};
    Vulkan::Runtime& runtime;
    Vulkan::StagingBufferPool& staging_pool;
    AmdGpu::Liverpool* liverpool;
    Core::MemoryManager* memory;
    TextureCache& texture_cache;
    std::unique_ptr<MemoryTracker> memory_tracker;

    StreamBuffer stream_buffer;
    /// gow3: read-only stream copies of a guest range, reused for the same range in the same
    /// submission until the guest sees a GPU write (ObtainBuffer).
    struct StreamCopy {
        u64 tick;
        u64 epoch;
        u64 wraps;
        u64 offset;
    };
    std::unordered_map<u64, StreamCopy> stream_copies;
    u64 stream_copied = 0, stream_copied_bytes = 0, stream_reused = 0, stream_reused_bytes = 0;
    /// gow3: small read-only ranges bound in several submissions (static mesh data) go to the
    /// arena, where CPU write tracking keeps them, instead of a copy per bind (UseArenaForStatic).
    struct StaticRange {
        u64 key = 0;
        u64 last_tick = 0;
        u32 ticks = 0;
        bool uploaded = false;
        bool demoted = false;
    };
    // gow3 20 Oct 2026 (laptop CPUs): 2^18 slots (6 MiB) for ~1,900 ranges in use made each
    // lookup a cache and TLB miss (~1.6% of the GPU command thread with the NVIDIA GPU, ~2.2% with
    // the 780M, Ryzen 7840HS); 2^16 (1.5 MiB) touches a quarter of the pages, at ~3% occupancy
    // (a newcomer whose slot holds an active range streams instead).
    static constexpr u32 StaticRangeBits = 16;
    std::vector<StaticRange> static_ranges = std::vector<StaticRange>(1u << StaticRangeBits);
    u64 static_binds = 0, static_bytes = 0, static_promoted = 0, static_demoted = 0;
    /// 0: stream it; 1: arena, synchronize (first use); 2: arena, already uploaded and clean.
    int UseArenaForStatic(VAddr device_addr, u32 size);
    Buffer gds_buffer;
    RangeSet gpu_modified_ranges;
    /// gow3: 64 KiB windows of the 40-bit guest address space a GPU write ever reached.
    static constexpr u64 GpuWindowBits = u64(1) << 24;
    std::unique_ptr<std::atomic<u64>[]> gpu_window_bits{new std::atomic<u64>[GpuWindowBits / 64]{}};
    /// gow3: held exclusively while gpu_modified_ranges changes, shared by IsRangeGpuWrittenShared.
    mutable std::shared_mutex gpu_ranges_mutex;
    void GpuRangesAdd(VAddr address, u64 size) {
        if (size) {
            const u64 first = address >> 16;
            const u64 last = std::min<u64>((address + size - 1) >> 16, GpuWindowBits - 1);
            for (u64 w = first; w <= last; ++w) {
                auto& word = gpu_window_bits[w >> 6];
                const u64 bit = 1ull << (w & 63);
                if (!(word.load(std::memory_order_relaxed) & bit)) {
                    word.fetch_or(bit, std::memory_order_relaxed);
                }
            }
        }
        std::unique_lock lock{gpu_ranges_mutex};
        // gow3 20 Oct 2026 (laptop CPUs): Uncharted 3 dispatches ~1,300 compute jobs a frame,
        // most writing the ranges they wrote before; adding a range the set already holds cost
        // the GPU command thread ~2.3% on a Ryzen 7840HS (laptop, 47 FPS CPU-bound),
        // and the window ticks of a range already noted in this submission ~1.4%. A small cache
        // of recent adds skips both while nothing was subtracted from the range since
        // (GpuRangesSubtract clears overlapping entries). GOW3_GPU_RANGE_CACHE=0: off.
        static const bool range_cache = [] {
            const char* env = std::getenv("GOW3_GPU_RANGE_CACHE");
            return !(env && env[0] == '0');
        }();
        const u64 tick = size && range_cache ? RangeTick() : 0;
        if (range_cache && size) {
            auto& entry = gpu_add_cache[((address >> 6) ^ (address >> 18) ^ size) & (GpuAddCacheSize - 1)];
            if (entry.address == address && entry.size == size) {
                if (entry.tick != tick) {
                    entry.tick = tick;
                    NoteGpuWriteWindows(address, size);
                }
                return;
            }
            entry = {address, size, tick};
        }
        gpu_modified_ranges.Add(address, size);
        if (size) {
            NoteGpuWriteWindows(address, size);
        }
    }
    /// gow3: recent GpuRangesAdd ranges (all inside gpu_modified_ranges) and the tick their
    /// windows were noted at; guarded by gpu_ranges_mutex.
    struct GpuAddCacheEntry {
        VAddr address = 0;
        u64 size = 0;
        u64 tick = 0;
    };
    static constexpr u32 GpuAddCacheSize = 512;
    std::array<GpuAddCacheEntry, GpuAddCacheSize> gpu_add_cache{};
    void NoteGpuWriteWindows(VAddr address, u64 size);
    /// gow3: the scheduler tick NoteGpuWriteWindows records (out of line: Scheduler is
    /// incomplete here).
    u64 RangeTick() const;
    /// gow3: scheduler tick of the newest large GPU write (over 64 KiB) into each 1 MiB window,
    /// and of the newest smaller one into each 4 KiB page: when the GPU data of a page is final.
    std::unordered_map<u64, u64> gpu_write_window_tick;
    std::unordered_map<u64, u64> gpu_write_page_tick;
    /// gow3: the page's GPU bytes are final and in mapped memory: copied to guest memory, page
    /// no longer GPU-written. False: not possible (in flight, unmapped, readbacks pending).
    bool ReadFinishedGpuPage(VAddr page);
    /// gow3: the same through a copy on the second queue that waits for the newest GPU write
    /// into the page's window only (already submitted). False: not possible.
    /// gow3 21 Oct 2026 (GOW3_FETCH_NO_BLOCK=1): `finished_only` takes only pages whose GPU
    /// writes have finished (false when the page's own have not), so the copy waits for no GPU
    /// work.
    bool CopyFinishedGpuPage(VAddr page, bool finished_only = false);
    /// gow3: copies GPU data of `pieces` to guest memory with a copy on the second queue that
    /// waits for main-queue tick `wait_tick` only. False: not possible.
    bool SideQueueFetch(std::span<const std::pair<VAddr, u64>> pieces, u64 wait_tick);
    /// gow3: a page whose GPU bytes are partly in flight: the finished ones are fetched (second
    /// queue) and stop being GPU data, so the rest can take the shared path.
    bool FetchOldGpuBytes(VAddr page);
    /// The window tick of `page` when its GPU data can be fetched early (submitted), else 0.
    /// `count_refusals`: false for the neighbour pages a batched fetch looks at (statistics).
    u64 FetchableWindowTick(VAddr page, bool count_refusals = true);
    void FinishPageFetch(VAddr page);
    bool arena_host_mapped = false;
    vk::UniqueCommandPool urgent_pool;
    vk::CommandBuffer urgent_cmd;
    /// gow3: timeline semaphore the side queue signals per fetch (no fence to reset: on AMD's
    /// Windows driver vkResetFences is a kernel call, ~6% of the U3 command thread).
    vk::UniqueSemaphore urgent_timeline;
    u64 urgent_value = 0;
    void GpuRangesSubtract(VAddr address, u64 size) {
        std::unique_lock lock{gpu_ranges_mutex};
        gpu_modified_ranges.Subtract(address, size);
        // gow3 20 Oct 2026: cached adds overlapping the range are no longer whole in the set.
        const VAddr end = address + size;
        for (auto& entry : gpu_add_cache) {
            if (entry.size && entry.address < end && address < entry.address + entry.size) {
                entry = {};
            }
        }
    }

    std::unique_ptr<FaultManager> fault_manager;
    std::unique_ptr<Buffer> bda_pagetable_buffer;
    /// gow3 1 Nov 2026 (2 GB cards): the page table as a sparse buffer (BindPageTablePages).
    bool sparse_pagetable = false;
    u64 pagetable_page_bits = 16;
    u32 pagetable_memory_type = 0;
    std::vector<bool> pagetable_bound;
    std::vector<vk::DeviceMemory> pagetable_chunks;
    u64 pagetable_chunk_used = 0;
    bool pagetable_binds_pending = false;
    u64 pagetable_chunk_capacity = 0;
    static constexpr u64 PageTableFirstChunk = u64{16} << 20;

    std::array<const Buffer*, MAX_ARENA_PAGES> address_space{};
    u64 arena_page_bits{MAX_ARENA_PAGE_BITS};
    u64 arena_page_size{u64{1} << MAX_ARENA_PAGE_BITS};
    u64 num_arena_pages{u64{1} << (ADDRESS_SPACE_BITS - MAX_ARENA_PAGE_BITS)};
    std::deque<Buffer> arenas;
    std::vector<ArenaBinds> pending_binds;
    Vulkan::Semaphore memory_semaphore;

    struct Backing : public Interval {
        vk::DeviceMemory memory;
        u64 offset;
        u8* mapped = nullptr; ///< gow3: the allocation's CPU mapping (CPU-mappable arenas)
        constexpr bool CanMergeWith(const Backing& other) const noexcept {
            return memory == other.memory && offset + (end - start) == other.offset;
        }
        constexpr Backing SubRange(u64 a, u64 b) const noexcept {
            return {{a, b}, memory, offset + (a - start), mapped};
        }
    };
    IntervalList<Backing> resident_ranges;
    std::unordered_map<u64, std::chrono::steady_clock::time_point> cpu_write_hot; ///< see IsCpuWriteHot
    /// gow3: page_writes and cpu_write_hot are swept every two seconds (SweepTracking).
    std::chrono::steady_clock::time_point last_tracking_sweep{};
    void SweepTracking();


    // gow3: a CPU write into a page with GPU-written bytes used to download the page with a full
    // GPU wait (~800 per 5 s in the U1 temple, half the frame time). The writes never hit the
    // GPU-written bytes themselves, so the page is unprotected without a download instead. The
    // guest copy of the GPU-written bytes stays stale and is recorded here; uploads leave those
    // bytes out and downloads fill them in. A guest byte that differs from its record was
    // written by the CPU after the GPU: the CPU value wins. GPU-side use, like
    // gpu_modified_ranges.
    static constexpr u64 LAZY_PAGE_SIZE = 4096;
    struct LazyPage {
        std::array<u8, LAZY_PAGE_SIZE> bytes{}; ///< guest bytes at the write fault
        std::bitset<LAZY_PAGE_SIZE> tracked;    ///< GPU-written bytes with a stale guest copy
        /// gow3: shared page (CPU writes next to GPU data still in flight): the tracked bytes
        /// are the GPU's (uploads skip them, readbacks write them). Since 18 Oct 2026 `bytes`
        /// holds their guest copy from that write too: a 64-byte line whose GPU bytes the CPU
        /// overwrote later is the CPU's (readback guard; GOW3_READBACK_GUARD=0: the GPU wins).
        bool gpu_wins = false;
    };
    /// gow3: the GPU writes into each 4 KiB page (writes up to 1 MiB): byte range and tick.
    struct PageWrite {
        u16 lo, hi;
        u64 tick;
    };
    std::unordered_map<u64, boost::container::small_vector<PageWrite, 4>> page_writes;
    /// Scheduler tick when the last per-submission readback was recorded: GPU writes from then
    /// on come back with the next one.
    u64 last_readback_tick = 0;
    /// Every GPU byte in `page` is from work in flight, from this submission (the next
    /// readback brings it) or in a pending readback: the CPU can write next to it.
    bool GpuBytesInFlight(VAddr page);
    /// Pages shared with the CPU whose GPU bytes the next readback must bring back.
    std::vector<std::pair<VAddr, u64>> shared_readbacks;
    u64 g_shared_writes_count = 0;

public:
    /// gow3: the shared pages' GPU bytes for the next readback (Rasterizer::SyncSmallGpuWrites).
    std::vector<std::pair<VAddr, u64>> TakeSharedReadbacks() {
        return std::exchange(shared_readbacks, {});
    }

private:
    std::map<VAddr, std::unique_ptr<LazyPage>> lazy_pages;

    // gow3: DownloadRangesAsync. The per-submission readback of small GPU writes (the CPU-side
    // SRT walker reads them) waited for the whole frame on the GPU (~10 ms, 40% of the GPU
    // thread in the temple). Its copies now land in `download` and reach guest memory a
    // little later. The pieces stay in gpu_modified_ranges until then; `rewritten` holds GPU
    // writes recorded after the readback, which stay GPU-modified when it completes.
    struct Peek {
        u64 tick;
        Buffer* buffer; ///< staging allocation (as PendingReadback keeps it)
        u64 offset;
        u64 staging_size;
        u8* mapped;
        VAddr address;
        u64 size;
        u64 tag;
        u32 slot;
        double ms;
        u64 dispatch; ///< the dispatch's number (0: none)
        u32 phase;    ///< 0: before the dispatch, 1: after
    };
    std::deque<Peek> peeks;
    struct PendingReadback {
        u64 tick = 0;
        Buffer* buffer = nullptr;
        u64 offset = 0;
        u64 size = 0;
        u8* mapped = nullptr;
        std::vector<std::pair<VAddr, u64>> pieces; ///< address, size
        std::vector<u64> offsets;                  ///< of each piece in `mapped`
        RangeSet rewritten;
        /// gow3 20 Oct 2026: the address span of `pieces` (set when queued; pieces never change
        /// after). `rewritten` is only ever looked at inside the pieces, so writes outside the
        /// span are not recorded (every written binding of every draw and dispatch was added to
        /// every pending readback's set, growing it until landing).
        VAddr span_lo = ~VAddr{0};
        VAddr span_hi = 0;
        void NoteRewrite(VAddr address, u64 size) {
            const VAddr lo = std::max(address, span_lo);
            const VAddr hi = std::min(address + size, span_hi);
            if (lo < hi) {
                rewritten.Add(lo, hi - lo);
            }
        }
        /// Pages to read-protect (merged ranges) until the data is back: a CPU read of them waits
        /// for this readback only (GOW3_ASYNC_READBACK=1; =2 leaves them readable, stale).
        std::vector<std::pair<VAddr, u64>> watched;
        bool watching = false; ///< `watched` is read-protected (from the first poll after submission)
        bool yield_to_cpu = false; ///< see DownloadRangesAsync
        bool open = false; ///< gow3: never protected (fence-gated results), in open_readback_ranges
        /// gow3: run once the data is in guest memory (fence labels of ray-cast batches).
        std::vector<std::function<void()>> on_landed;
        /// gow3 tests (GOW3_TEST_READBACK_LAG): when it was queued.
        std::chrono::steady_clock::time_point queued = std::chrono::steady_clock::now();
        /// gow3 18 Oct 2026 (readback guard): the guest bytes of each piece when it was queued
        /// (same layout as `mapped`). A 64-byte line that differs at landing, and not by the
        /// GPU's value, was written meanwhile (the guest reusing the memory once it saw the
        /// fence) and keeps that value.
        SnapshotBytes before;
    };
    PageManager* page_manager = nullptr;
    std::deque<PendingReadback> pending_readbacks;
    /// gow3 20 Oct 2026 (GOW3_READBACK_POOL): snapshot buffers of landed readbacks, reused by
    /// the next ones (DownloadRangesAsync); guarded like pending_readbacks.
    std::vector<SnapshotBytes> snapshot_pool;
    void RecycleSnapshot(SnapshotBytes& bytes);
    /// gow3: writes one GPU-modified run [lo, hi) of piece `piece` of `readback` (the oldest
    /// pending one) to guest memory, except lines the guest changed since it was queued.
    void LandGuarded(PendingReadback& readback, size_t piece, VAddr lo, VAddr hi);
    /// gow3: guest bytes [lo, hi) were `pre` before a landing: the snapshots of later pending
    /// readbacks that still held those bytes move to the landed values.
    void UpdateLaterSnapshots(VAddr lo, VAddr hi, const u8* pre);
    /// gow3 18 Oct: bytes on their way back in open (unprotected, fence-gated) readbacks: other
    /// readbacks of the same bytes (the per-submission small-write readback) do not protect them
    /// again.
    RangeSet open_readback_ranges;
    /// gow3: pages where a CPU read faulted on other data than a pending readback's GPU bytes
    /// (false sharing: the CPU's own data next to small GPU results in the same 4 KiB) -> until
    /// when async readbacks leave them unprotected. Uncharted 2: 89% of the read faults, each one
    /// waiting ~14 ms for the GPU thread and the GPU.
    std::unordered_map<u64, std::chrono::steady_clock::time_point> noisy_pages;
    /// Stops read-protecting one page for every pending readback (a false-sharing read).
    void UnwatchPage(VAddr page);

public:
    /// gow3: lands the pending readbacks that read-protect `page`, waiting for the GPU if needed
    /// (GPU thread: a CPU access faulted on that page and would fault again).
    void LandReadbacksWatching(VAddr page);
    /// gow3 21 Oct 2026 (GPU worker): the newest tick of the pending readbacks that
    /// read-protect `page` (0: none), GPU thread. A faulting guest thread waits for it itself
    /// before LandReadbacksWatching (which then has nothing to wait for), so the command thread
    /// keeps feeding the GPU meanwhile (GOW3_LAND_ON_GUEST).
    [[nodiscard]] u64 WatchingTick(VAddr page) const;

    /// gow3: runs `fn` (GPU thread) when the newest pending readback has landed in guest memory;
    /// false when no readback is pending. Ray-cast batches' end-of-pipe fences: the guest reads
    /// the results once it sees the fence, so the fence is written after them (as on the console,
    /// where the GPU writes both in order), not when the command is recorded.
    bool DeferUntilLastReadback(std::function<void()> fn, VAddr label, u64 value = 0, u32 bytes = 0);
    /// The readback a deferred fence at `address` (4 bytes) waits for. A GPU wait (WAIT_REG_MEM)
    /// on such a fence lands that readback first (LandReadbacks), as the GPU would execute the
    /// fence before the wait. Counting the value as written instead let the GPU thread run past
    /// waits the guest relies on (Uncharted 2 read a command buffer before it was written).
    [[nodiscard]] bool DeferredFenceTick(VAddr address, u64& tick, u32* value = nullptr) const;
    /// gow3 diagnosis: GPU data or a readback in flight in [address, address + size).
    [[nodiscard]] u64 PendingReadbackTickFor(VAddr address, u64 size) const {
        return PendingReadbackTick(address, size, false);
    }
    /// gow3: the newest pending readback's tick (0: none pending), with or without data pieces:
    /// the presenter's frames-ahead wait sleeps until the GPU passes it, then lands it.
    /// gow3 21 Oct 2026 (GPU worker): the oldest pending readback's tick (0: none); readbacks
    /// land oldest first (GOW3_READBACK_WAKE: the command thread wakes when the GPU passes it).
    [[nodiscard]] u64 OldestPendingReadbackTick() const {
        return pending_readbacks.empty() ? 0 : pending_readbacks.front().tick;
    }
    [[nodiscard]] u64 NewestPendingReadbackTick() const {
        u64 tick = 0;
        for (const auto& readback : pending_readbacks) {
            tick = std::max(tick, readback.tick);
        }
        return tick;
    }
    /// Waits for the readbacks up to `tick` and lands them (their deferred fences are written).
    void LandReadbacks(u64 tick);
    /// A fence label still waiting for a readback: later fences to it wait behind it.
    [[nodiscard]] bool IsLabelDeferred(VAddr label) const {
        return deferred_labels.contains(label);
    }
    [[nodiscard]] bool HasDeferredSignals() const {
        return deferred_signals.load(std::memory_order_acquire) != 0;
    }
    /// The newest pending readback is in the command buffer still being recorded: fences waiting
    /// for it would wait forever if the guest waits for them before submitting more work.
    [[nodiscard]] bool HasUnsubmittedReadback() const;
    /// Lands finished readbacks now (the GPU thread polls while idle when fences wait on them).
    void PollReadbacksNow();

private:
    std::unordered_multiset<VAddr> deferred_labels;
    struct DeferredValue {
        u64 value;
        u32 bytes;
        u64 tick;
    };
    std::unordered_map<VAddr, DeferredValue> deferred_values; ///< newest value per label
    /// Atomic: with multithreaded rendering the draw recording thread defers fences while the GPU
    /// command thread checks for them (it drains that thread before touching the rest).
    std::atomic<u32> deferred_signals{0};
    /// gow3: readback pieces that landed (address -> size, when): a piece read back again at the
    /// same address and size within RecurringWindow is a recurring GPU result (Uncharted's
    /// 16-byte luminance at a fixed address, read by the CPU every frame). Its previous value,
    /// one frame old as on the console where the GPU runs behind, stays readable: no read
    /// protection. New pieces (ring memory reused for other data) are protected, so a CPU read
    /// waits for them instead of seeing unrelated old bytes.
    std::unordered_map<VAddr, std::pair<u64, std::chrono::steady_clock::time_point>> landed_pieces;
    u32 readback_polls = 0;
    /// gow3 1 Nov 2026: CompleteReadbacks sets up read watches for readbacks whose submission
    /// went out; it scans the queue again only when the tick moved or a readback was added.
    u64 watch_scan_tick = ~0ULL;

    u32 arena_memory_type_index{};
    u32 block_size{};
    u32 block_shift{};
    u32 blocks_per_arena_page{};
    u32 blocks_per_arena_page_shift{};
    /// gow3 20 Oct 2026 (GitHub #95, ReclaimArenaMemory): the 2 s epoch in which each 1 MiB
    /// window of guest GPU memory was last used through an arena (0: never), the scheduler tick
    /// at the start of each recent epoch, and arena memory given back (its runs are bound again
    /// before new memory is allocated; quarantined until the unbinds are submitted).
    static constexpr u32 ReclaimWindowBits = 20;
    static constexpr u32 ReclaimEpochRing = 64;
    std::unique_ptr<std::atomic<u16>[]> window_epoch{
        new std::atomic<u16>[u64(1) << (ADDRESS_SPACE_BITS - ReclaimWindowBits)]{}};
    u16 reclaim_epoch = 1;
    std::chrono::steady_clock::time_point next_reclaim_epoch{};
    std::array<u64, ReclaimEpochRing> epoch_start_tick{};
    struct FreeRun {
        vk::DeviceMemory memory;
        u64 offset; ///< bytes into `memory`
        u8* mapped; ///< the allocation's CPU mapping (CPU-mappable arenas) or null
        u64 blocks;
    };
    std::vector<FreeRun> free_runs, quarantined_runs;
    u64 resident_in_use_blocks = 0;
    bool has_pending_unbinds = false;
    void TouchArenaWindows(u64 first_block, u64 last_block) {
        const u16 epoch = reclaim_epoch;
        const u64 last = std::min<u64>(((last_block << block_shift) >> ReclaimWindowBits),
                                       (u64(1) << (ADDRESS_SPACE_BITS - ReclaimWindowBits)) - 1);
        for (u64 w = (first_block << block_shift) >> ReclaimWindowBits; w <= last; ++w) {
            if (window_epoch[w].load(std::memory_order_relaxed) != epoch) {
                window_epoch[w].store(epoch, std::memory_order_relaxed);
            }
        }
    }
    /// Releases the resident blocks in [first_block, end_block); returns how many.
    u64 ReleaseArenaBlocks(u64 first_block, u64 end_block);
    /// Drops pending unbinds of `arena` inside [first_block, end_block) (bound again).
    void TrimPendingUnbinds(const Buffer* arena, u64 first_block, u64 end_block);
    /// gow3: arena memory in chunks allocated ahead on a background thread (buffer_cache.cpp).
    struct ResidencyPool;
    std::unique_ptr<ResidencyPool> residency_pool;
};

} // namespace VideoCore
