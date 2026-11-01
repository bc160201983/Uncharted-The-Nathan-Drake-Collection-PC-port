// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "gow3_toggles.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <bit>
#include <cstdlib>
#include <cstring>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/types.h"
#include "core/devtools/widget/gpu_memory.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"

namespace Vulkan {

using VideoCore::MemoryType;

namespace {

// gow3: staging memory is populated (allocated and cleared) by the kernel on first CPU access,
// ~2 ms per 16 MiB block. Streaming bursts a few seconds apart re-created the blocks trimmed
// in between and copies into them ran at 0.3 GB/s (stutter). Blocks now live 30 s idle, the
// upload ring keeps GOW3_STAGING_KEEP_MB (512) and is populated at startup.
constexpr u64 RING_IDLE_FRAMES = 3000;
constexpr u64 LARGE_IDLE_FRAMES = 3000;
constexpr u64 LARGE_IDLE_FRAMES_LOW_VRAM = 120;
constexpr u64 PREWARM_BLOCKS = 8;
constexpr u64 LARGE_MIN_GRANULARITY = 64_KB;
constexpr u64 BLOCK_SIZE = 16_MB;

u64 RoundAllocationSize(u64 size) {
    const u64 step = std::max(std::bit_floor(size) / 4, LARGE_MIN_GRANULARITY);
    return Common::AlignUp(size, step);
}

/// gow3 21 Oct 2026 (GPU worker): time of a staging allocation (Gow3Stats::t_staging, the Stall
/// lines' "staging"); allocations over 2 ms are logged (the first 64).
struct NewAllocationTimer {
    const char* what;
    MemoryType type;
    u64 bytes;
    Gow3Stats::Timer stats{Gow3Stats::t_staging};
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~NewAllocationTimer() {
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        static std::atomic<int> logged{0};
        if (ms >= 2.0 && logged.fetch_add(1, std::memory_order_relaxed) < 64) {
            static constexpr const char* names[] = {"DeviceLocal", "HostUncached", "HostCached"};
            std::printf("Staging: new %s (%s, %llu MiB) took %.1f ms\n", what,
                        u32(type) < 3 ? names[u32(type)] : "other",
                        static_cast<unsigned long long>(bytes >> 20), ms);
        }
    }
};

} // Anonymous namespace

StagingBufferPool::StagingBufferPool(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {
    const char* env = std::getenv("GOW3_STAGING_KEEP_MB");
    keep_blocks = (env ? std::strtoull(env, nullptr, 10) : 512) * 1_MB / BLOCK_SIZE;
    Ring& ring = rings[u32(MemoryType::HostUncached)];
    for (u64 i = 0; i < PREWARM_BLOCKS; ++i) {
        auto& block = ring.blocks.emplace_back(Block{
            .buffer = std::make_unique<VideoCore::StreamBuffer>(
                instance, scheduler, MemoryType::HostUncached, BLOCK_SIZE),
            .last_used_frame = frame,
        });
        if (!block.buffer->mapped_data.empty()) {
            std::memset(block.buffer->mapped_data.data(), 0, block.buffer->mapped_data.size());
        }
    }
}

StagingBufferPool::~StagingBufferPool() = default;

StagingBufferRef StagingBufferPool::Request(u64 size, MemoryType type, u64 alignment,
                                            bool deferred) {
    Ring& ring = rings[u32(type)];
    if (deferred || size > BLOCK_SIZE) {
        return RequestLarge(size, type, deferred);
    }
    return RequestFromRing(ring, size, alignment, type);
}

StagingBufferRef StagingBufferPool::RequestFromRing(Ring& ring, u64 size, u64 alignment,
                                                    MemoryType type) {
    Block* best{};
    u64 best_offset{};
    for (size_t i = 0; i < ring.blocks.size(); ++i) {
        const size_t index = (ring.current + i) % ring.blocks.size();
        Block* block = &ring.blocks[index];
        if (const auto offset = block->buffer->Reserve(size, alignment, false)) {
            ring.current = index;
            best = block;
            best_offset = *offset;
            break;
        }
    }

    if (!best) {
        // gow3 21 Oct 2026 (GPU worker): driver allocations made while recording (texture
        // streaming bursts) show as "staging" in the Stall lines, and slow ones are logged.
        const NewAllocationTimer timer{"ring block", type, BLOCK_SIZE};
        best = &ring.blocks.emplace_back(Block{
            .buffer =
                std::make_unique<VideoCore::StreamBuffer>(instance, scheduler, type, BLOCK_SIZE),
            .last_used_frame = frame,
        });
        ring.current = ring.blocks.size() - 1;
        best_offset = *best->buffer->Reserve(size, alignment, false);
    }

    auto& buffer = *best->buffer;
    best->last_used_frame = frame;

    StagingBufferRef ref{
        .buffer = &buffer,
        .offset = best_offset,
        .size = size,
        .type = type,
    };
    if (!buffer.mapped_data.empty()) {
        ref.mapped = buffer.mapped_data.data() + best_offset;
    }
    return ref;
}

StagingBufferRef StagingBufferPool::RequestLarge(u64 size, MemoryType type, bool deferred) {
    auto& cache = large_caches[u32(type)];
    const u64 max_size = std::max(size + size / 4, RoundAllocationSize(size));

    LargeBuffer* best{};
    for (LargeBuffer& entry : cache) {
        const u64 entry_size = entry.buffer->SizeBytes();
        if (entry.held || entry_size < size || entry_size > max_size ||
            !scheduler.IsFree(entry.tick)) {
            continue;
        }
        if (!best || entry_size < best->buffer->SizeBytes()) {
            best = &entry;
        }
    }

    if (!best) {
        const NewAllocationTimer timer{"large buffer", type, RoundAllocationSize(size)};
        best = &cache.emplace_back(LargeBuffer{
            .buffer = std::make_unique<VideoCore::Buffer>(instance, 0, RoundAllocationSize(size),
                                                          type, "StagingBufferPool:Dedicated"),
            .tick = 0,
            .last_used_frame = frame,
            .held = false,
        });
    }

    best->tick = scheduler.CurrentTick();
    best->last_used_frame = frame;
    best->held = deferred;

    VideoCore::Buffer& buffer = *best->buffer;
    StagingBufferRef ref{
        .buffer = &buffer,
        .offset = 0,
        .size = size,
        .type = type,
    };
    if (!buffer.mapped_data.empty()) {
        ref.mapped = buffer.mapped_data.data();
    }
    return ref;
}

void StagingBufferPool::FreeDeferred(const StagingBufferRef& ref) {
    auto& cache = large_caches[u32(ref.type)];
    const auto it = std::ranges::find_if(
        cache, [&](const LargeBuffer& entry) { return entry.buffer.get() == ref.buffer; });
    ASSERT_MSG(it != cache.end() && it->held, "FreeDeferred on an allocation that is not held");
    it->held = false;
    it->tick = scheduler.CurrentTick();
    it->last_used_frame = frame;
}

void StagingBufferPool::TickFrame() {
    ++frame;
    // gow3 1 Nov 2026 (2 GB cards): with low video memory, idle video-memory staging buffers
    // (texture detiling, up to tens of MiB each) are freed after 120 frames, not 3000, and the
    // video-memory ring keeps 2 blocks (not 32).
    for (u32 i = 0; i < NUM_TYPES; ++i) {
        const bool low = i == u32(MemoryType::DeviceLocal) && g_low_video_memory;
        TrimRing(rings[i], low ? 2 : keep_blocks,
                 low ? LARGE_IDLE_FRAMES_LOW_VRAM : RING_IDLE_FRAMES);
    }
    for (u32 i = 0; i < NUM_TYPES; ++i) {
        TrimLarge(large_caches[i], i == u32(MemoryType::DeviceLocal) && g_low_video_memory
                                       ? LARGE_IDLE_FRAMES_LOW_VRAM
                                       : LARGE_IDLE_FRAMES);
    }
    PublishStats();
    // gow3 statistics: the pool's size over time (a growing pool means growing per-use scans
    // and driver allocations).
    static auto last = std::chrono::steady_clock::now();
    if (const auto now = std::chrono::steady_clock::now(); now - last >= std::chrono::seconds(5)) {
        last = now;
        u64 ring_blocks = 0, large = 0, held = 0, large_bytes = 0;
        for (const Ring& ring : rings) {
            ring_blocks += ring.blocks.size();
        }
        for (const auto& cache : large_caches) {
            large += cache.size();
            for (const LargeBuffer& entry : cache) {
                held += entry.held;
                large_bytes += entry.buffer->SizeBytes();
            }
        }
        std::printf("Staging pool: %llu ring blocks (16 MB), %llu large buffers (%llu held, %llu MB)\n",
                    static_cast<unsigned long long>(ring_blocks), static_cast<unsigned long long>(large),
                    static_cast<unsigned long long>(held),
                    static_cast<unsigned long long>(large_bytes >> 20));
    }
}

void StagingBufferPool::TrimRing(Ring& ring, u64 keep, u64 idle_frames) {
    auto& blocks = ring.blocks;
    for (size_t i = blocks.size(); i-- > 0;) {
        const Block& block = blocks[i];
        if (blocks.size() <= keep || frame - block.last_used_frame < idle_frames ||
            !scheduler.IsFree(block.buffer->LastTick())) {
            continue;
        }
        blocks.erase(blocks.begin() + i);
        if (i < ring.current) {
            --ring.current;
        } else if (i == ring.current) {
            ring.current = 0;
        }
    }
    if (ring.current >= blocks.size()) {
        ring.current = 0;
    }
}

void StagingBufferPool::TrimLarge(std::vector<LargeBuffer>& cache, u64 idle_frames) {
    std::erase_if(cache, [this, idle_frames](const LargeBuffer& entry) {
        return !entry.held && frame - entry.last_used_frame >= idle_frames &&
               scheduler.IsFree(entry.tick);
    });
}

void StagingBufferPool::PublishStats() const {
    using namespace Core::Devtools::Widget;
    if (!GpuMemoryViewer::IsEnabled()) {
        return;
    }
    static constexpr std::array<const char*, NUM_TYPES> names = {"DeviceLocal", "HostUncached",
                                                                 "HostCached"};
    GpuMemoryGroup group{.name = "Staging"};
    for (size_t i = 0; i < NUM_TYPES; ++i) {
        u64 bytes = 0;
        for (const Block& block : rings[i].blocks) {
            bytes += block.buffer->SizeBytes();
        }
        for (const LargeBuffer& entry : large_caches[i]) {
            bytes += entry.buffer->SizeBytes();
        }
        if (bytes != 0) {
            group.rows.push_back({.name = names[i], .bytes = bytes});
        }
    }
    GpuMemoryViewer::Publish(std::move(group));
}

} // namespace Vulkan
