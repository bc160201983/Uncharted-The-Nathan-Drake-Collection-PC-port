// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "gow3_toggles.h"
#include <ranges>
#include "common/assert.h"
#include "common/thread.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"

#include <vk_mem_alloc.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <thread>
#include "common/thread.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace VideoCore {
// Diagnostics: which image or buffer occupies a VkDeviceMemory range, so a GPU fault address
// next to a memory block names its owner (Vulkan::DescribeBoundAddress).
struct GpuAllocationRecord {
    u64 memory, offset, size, handle;
    char what[160];
    bool alive;
};
static std::mutex gpu_allocations_mutex;
static std::vector<GpuAllocationRecord> gpu_allocations;
/// gow3: the records are kept only with the GPU fault diagnostics on (GOW3_GPU_CHECKPOINTS, the
/// one reader: Vulkan::DescribeBoundAddress). Always on, the list grew by every image and buffer
/// made in a session (never shrunk) and each free searched it (18 Oct 2026).
static bool GpuAllocationRecords() {
    static const bool on = [] {
        const char* env = std::getenv("GOW3_GPU_CHECKPOINTS");
        return env && env[0] != '0';
    }();
    return on;
}
void RecordGpuAllocation(u64 handle, const VmaAllocationInfo& info, const char* what) {
    if (!GpuAllocationRecords()) {
        return;
    }
    std::scoped_lock lock{gpu_allocations_mutex};
    GpuAllocationRecord record{u64(info.deviceMemory), info.offset, info.size, handle, {}, true};
    std::snprintf(record.what, sizeof(record.what), "%s", what);
    gpu_allocations.push_back(record);
}
/// Appends the guest identity of a texture cache image to its allocation record.
void NoteGpuAllocation(u64 handle, VAddr guest_address, u32 guest_size) {
    if (!GpuAllocationRecords()) {
        return;
    }
    std::scoped_lock lock{gpu_allocations_mutex};
    for (auto it = gpu_allocations.rbegin(); it != gpu_allocations.rend(); ++it) {
        if (it->handle == handle && it->alive) {
            const size_t used = std::strlen(it->what);
            std::snprintf(it->what + used, sizeof(it->what) - used, " guest %#llx+%#x",
                          (unsigned long long)guest_address, guest_size);
            return;
        }
    }
}
void RecordGpuFree(u64 handle) {
    if (!GpuAllocationRecords()) {
        return;
    }
    std::scoped_lock lock{gpu_allocations_mutex};
    for (auto it = gpu_allocations.rbegin(); it != gpu_allocations.rend(); ++it) {
        if (it->handle == handle && it->alive) {
            it->alive = false;
            return;
        }
    }
}
void DescribeGpuMemory(u64 memory, u64 offset) {
    std::scoped_lock lock{gpu_allocations_mutex};
    int printed = 0;
    for (auto it = gpu_allocations.rbegin(); it != gpu_allocations.rend() && printed < 16; ++it) {
        if (it->memory != memory) {
            continue;
        }
        ++printed;
        std::fprintf(stderr, "  memory %#llx+%#llx..%#llx %s %#llx: %s%s\n",
                     (unsigned long long)memory, (unsigned long long)it->offset,
                     (unsigned long long)(it->offset + it->size), it->alive ? "live" : "FREED",
                     (unsigned long long)it->handle, it->what,
                     offset >= it->offset && offset < it->offset + it->size ? " <== fault" : "");
    }
}
} // namespace VideoCore


namespace VideoCore {

using namespace Vulkan;

Common::IncrementalIdProvider<u64> Image::global_image_uid{};

static vk::ImageUsageFlags ImageUsageFlags(const Vulkan::Instance& instance,
                                           const ImageInfo& info) {
    vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eTransferSrc |
                                vk::ImageUsageFlagBits::eTransferDst |
                                vk::ImageUsageFlagBits::eSampled;
    if (!info.props.is_block) {
        if (info.props.is_depth) {
            usage |= vk::ImageUsageFlagBits::eDepthStencilAttachment;
        } else {
            usage |= vk::ImageUsageFlagBits::eColorAttachment;
            if (instance.IsAttachmentFeedbackLoopLayoutSupported()) {
                usage |= vk::ImageUsageFlagBits::eAttachmentFeedbackLoopEXT;
            }
            // Always create images with storage flag to avoid needing re-creation in case of e.g
            // compute clears This sacrifices a bit of performance but is less work. ExtendedUsage
            // flag is also used.
            usage |= vk::ImageUsageFlagBits::eStorage;
        }
    } else {
        // Similarly to above, we specify storage usage. This is typically not supported by
        // compressed formats, but may be used for uncompressed views. In order to satisfy this,
        // we will also specify the extended usage bit.
        usage |= vk::ImageUsageFlagBits::eStorage;
    }

    return usage;
}

static vk::ImageType ConvertImageType(AmdGpu::ImageType type) noexcept {
    switch (type) {
    case AmdGpu::ImageType::Color1D:
    case AmdGpu::ImageType::Color1DArray:
        return vk::ImageType::e1D;
    case AmdGpu::ImageType::Color2D:
    case AmdGpu::ImageType::Color2DMsaa:
    case AmdGpu::ImageType::Color2DArray:
        return vk::ImageType::e2D;
    case AmdGpu::ImageType::Color3D:
        return vk::ImageType::e3D;
    default:
        UNREACHABLE();
    }
}

static vk::FormatFeatureFlags2 FormatFeatureFlags(const vk::ImageUsageFlags usage_flags) {
    vk::FormatFeatureFlags2 feature_flags{};
    if (usage_flags & vk::ImageUsageFlagBits::eTransferSrc) {
        feature_flags |= vk::FormatFeatureFlagBits2::eTransferSrc;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eTransferDst) {
        feature_flags |= vk::FormatFeatureFlagBits2::eTransferDst;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eSampled) {
        feature_flags |= vk::FormatFeatureFlagBits2::eSampledImage;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eColorAttachment) {
        feature_flags |= vk::FormatFeatureFlagBits2::eColorAttachment;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eDepthStencilAttachment) {
        feature_flags |= vk::FormatFeatureFlagBits2::eDepthStencilAttachment;
    }
    // Note: StorageImage is intentionally ignored for now since it is always set, and can mess up
    // compatibility checks.
    return feature_flags;
}

namespace {
/// gow3: images and their memory are freed on this thread instead of the GPU command thread.
/// Freeing a dedicated allocation is a kernel call (D3DKMTDestroyAllocation2): about 3% of the
/// GPU command thread in Uncharted 2 (18 Oct 2026). VMA is internally synchronized; nothing
/// uses an image once its UniqueImage is gone. Opt-in (GOW3_IMAGE_REAPER=1): memory freed a
/// little later could make an allocation within budget fail on a 4 GB GPU; no FPS gain measured.
class ImageReaper {
public:
    static bool Enabled() {
        static const bool enabled = [] {
            const char* env = std::getenv("GOW3_IMAGE_REAPER");
            return env && env[0] == '1';
        }();
        return enabled;
    }

    static ImageReaper& Get() {
        static ImageReaper* reaper = new ImageReaper; // lives until the process ends
        return *reaper;
    }

    void Destroy(VmaAllocator allocator, VkImage image, VmaAllocation allocation) {
        {
            std::scoped_lock lock{mutex};
            queue.push_back({allocator, image, allocation});
        }
        work_cv.notify_one();
    }

    /// Waits until every queued image is freed (before the allocator goes).
    void Flush() {
        std::unique_lock lock{mutex};
        idle_cv.wait(lock, [this] { return queue.empty() && !busy; });
    }

private:
    struct Item {
        VmaAllocator allocator;
        VkImage image;
        VmaAllocation allocation;
    };

    ImageReaper() {
        std::thread([this] { Run(); }).detach();
    }

    void Run() {
        Common::SetCurrentThreadName("gow3:ImageReaper");
        std::vector<Item> items;
        while (true) {
            {
                std::unique_lock lock{mutex};
                busy = false;
                idle_cv.notify_all();
                work_cv.wait(lock, [this] { return !queue.empty(); });
                items.swap(queue);
                busy = true;
            }
            for (const auto& item : items) {
                vmaDestroyImage(item.allocator, item.image, item.allocation);
            }
            items.clear();
        }
    }

    std::mutex mutex;
    std::condition_variable work_cv, idle_cv;
    std::vector<Item> queue;
    bool busy = false;
};

void DestroyImage(VmaAllocator allocator, vk::Image image, VmaAllocation allocation) {
    RecordGpuFree(u64(VkImage(image)));
    if (ImageReaper::Enabled()) {
        ImageReaper::Get().Destroy(allocator, image, allocation);
    } else {
        vmaDestroyImage(allocator, image, allocation);
    }
}
} // namespace

void FlushImageReaper() {
    if (ImageReaper::Enabled()) {
        ImageReaper::Get().Flush();
    }
}

UniqueImage::~UniqueImage() {
    if (image) {
        DestroyImage(allocator, image, allocation);
    }
}

void UniqueImage::Destroy() {
    if (image) {
        DestroyImage(allocator, image, allocation);
        image = vk::Image{};
        allocation = {};
    }
}

namespace {
/// gow3 18 Oct 2026: AMD's Windows driver takes ~170 ms to allocate a 256 MiB block of video
/// memory (Radeon 780M), and VMA allocates a new block the moment an image no longer fits: the
/// GPU thread stalled for 70-240 ms (Uncharted 2/3 hitches). This thread keeps room in reserve
/// instead: when the memory types images use have no free range of 64 MiB left, it allocates a
/// 64 MiB spare there (VMA makes a new block, with room for the next images) and frees the
/// previous spare. GOW3_SPARE_IMAGE_BLOCK=0: off.
class SpareBlockKeeper {
public:
    static void Note(VmaAllocator allocator, u32 memory_type) {
        // gow3 1 Nov 2026: not with low video memory (a 64 MiB spare is a whole block there).
        static SpareBlockKeeper* const keeper = [allocator]() -> SpareBlockKeeper* {
            const char* env = std::getenv("GOW3_SPARE_IMAGE_BLOCK");
            const bool off = env ? env[0] == '0' : Vulkan::g_low_video_memory;
            return off ? nullptr : new SpareBlockKeeper(allocator); // process-long
        }();
        if (keeper) {
            keeper->Wake(memory_type);
        }
    }

private:
    static constexpr VkDeviceSize Spare = VkDeviceSize{64} << 20;

    explicit SpareBlockKeeper(VmaAllocator allocator_) : allocator{allocator_} {
        std::thread{[this] { Run(); }}.detach();
    }

    void Wake(u32 memory_type) {
        {
            std::scoped_lock lock{mutex};
            types |= 1u << memory_type;
            pending = true;
        }
        cv.notify_one();
    }

    void Run() {
        Common::SetCurrentThreadName("GPU:SpareImageBlock");
        u32 made = 0;
        for (;;) {
            u32 wanted;
            {
                std::unique_lock lock{mutex};
                cv.wait(lock, [this] { return pending; });
                // Images come in bursts: act once one is over, so that the block is allocated
                // while the GPU thread creates no images (VMA holds the memory type's lock while
                // the driver allocates; an image created meanwhile waited for it).
                do {
                    pending = false;
                    cv.wait_for(lock, std::chrono::milliseconds{150}, [this] { return pending; });
                } while (pending);
                wanted = types;
            }
            VmaTotalStatistics stats{};
            vmaCalculateStatistics(allocator, &stats);
            for (u32 type = 0; type < VK_MAX_MEMORY_TYPES; ++type) {
                if (!(wanted >> type & 1) || stats.memoryType[type].unusedRangeSizeMax >= Spare) {
                    continue;
                }
                const VkMemoryRequirements requirements = {
                    .size = Spare, .alignment = 64 * 1024, .memoryTypeBits = 1u << type};
                const VmaAllocationCreateInfo create = {
                    .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
                    .usage = VMA_MEMORY_USAGE_UNKNOWN,
                    .memoryTypeBits = 1u << type,
                };
                // The spare stays allocated (a block that only held a freed spare would be
                // deleted by VMA at the next free); the previous one is freed once the new one
                // has its block, which opens room for images in an older block.
                VmaAllocation spare{};
                const auto started = std::chrono::steady_clock::now();
                if (vmaAllocateMemory(allocator, &requirements, &create, &spare, nullptr) ==
                    VK_SUCCESS) {
                    if (held[type]) {
                        vmaFreeMemory(allocator, held[type]);
                    }
                    held[type] = spare;
                    if (++made <= 16) {
                        std::printf("GPU: spare image memory block ready (type %u, %.0f ms)\n", type,
                                    std::chrono::duration<double, std::milli>(
                                        std::chrono::steady_clock::now() - started).count());
                    }
                }
            }
            // At most one check per 20 ms: image creation comes in bursts.
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
    }

    VmaAllocator allocator;
    std::mutex mutex;
    std::condition_variable cv;
    u32 types = 0;
    bool pending = false;
    std::array<VmaAllocation, VK_MAX_MEMORY_TYPES> held{};
};
} // namespace

void UniqueImage::Create(const vk::ImageCreateInfo& image_ci) {
    this->image_ci = image_ci;
    ASSERT(!image);
    const VmaAllocationCreateInfo alloc_ci = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    const VkImageCreateInfo image_ci_unsafe = static_cast<VkImageCreateInfo>(image_ci);
    VkImage unsafe_image{};
    VmaAllocationInfo alloc_info{};
    const auto started = std::chrono::steady_clock::now();
    // gow3 18 Oct 2026: NVIDIA prefers (does not require) dedicated memory for render targets
    // and VMA honours that with a driver allocation per image: 4-21 ms per 960x540 target on the
    // RTX 5060 Laptop, and Uncharted 2 creates one every few seconds (hitches). Images that only
    // prefer it go into VMA's device-local blocks like the rest (SpareBlockKeeper keeps room
    // there). GOW3_DEDICATED_IMAGES=1: VMA decides, as before.
    static const bool dedicated_images = [] {
        const char* env = std::getenv("GOW3_DEDICATED_IMAGES");
        return env && env[0] == '1';
    }();
    VkResult result = VK_ERROR_FEATURE_NOT_PRESENT;
    if (!dedicated_images) {
        const auto& dispatch = VULKAN_HPP_DEFAULT_DISPATCHER;
        const VkDevice vk_device = static_cast<VkDevice>(device);
        const VkResult created =
            dispatch.vkCreateImage(vk_device, &image_ci_unsafe, nullptr, &unsafe_image);
        ASSERT_MSG(created == VK_SUCCESS, "Failed creating image with error {}",
                   vk::to_string(vk::Result{created}));
        VkMemoryDedicatedRequirements dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
        VkMemoryRequirements2 requirements{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dedicated};
        const VkImageMemoryRequirementsInfo2 requirements_info{
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, nullptr, unsafe_image};
        dispatch.vkGetImageMemoryRequirements2(vk_device, &requirements_info, &requirements);
        // Device-local memory types without host access (the ones VMA's AUTO_PREFER_DEVICE picks
        // for optimally tiled images).
        static const u32 device_only_types = [this] {
            const VkPhysicalDeviceMemoryProperties* props{};
            vmaGetMemoryProperties(allocator, &props);
            u32 mask = 0;
            for (u32 type = 0; type < props->memoryTypeCount; ++type) {
                const auto flags = props->memoryTypes[type].propertyFlags;
                if ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
                    !(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                    mask |= 1u << type;
                }
            }
            return mask;
        }();
        const u32 types = requirements.memoryRequirements.memoryTypeBits & device_only_types;
        if (dedicated.prefersDedicatedAllocation && !dedicated.requiresDedicatedAllocation &&
            types != 0) {
            const VmaAllocationCreateInfo block_ci = {
                .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
                .usage = VMA_MEMORY_USAGE_UNKNOWN,
                .requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                .memoryTypeBits = types,
            };
            result = vmaAllocateMemory(allocator, &requirements.memoryRequirements, &block_ci,
                                       &allocation, &alloc_info);
            if (result == VK_SUCCESS) {
                result = vmaBindImageMemory(allocator, allocation, unsafe_image);
                if (result != VK_SUCCESS) {
                    vmaFreeMemory(allocator, allocation);
                    allocation = {};
                }
            }
        }
        if (result != VK_SUCCESS) {  // dedicated not just preferred, or no block within budget
            dispatch.vkDestroyImage(vk_device, unsafe_image, nullptr);
            unsafe_image = VK_NULL_HANDLE;
        }
    }
    if (result != VK_SUCCESS) {
        result = vmaCreateImage(allocator, &image_ci_unsafe, &alloc_ci, &unsafe_image,
                                &allocation, &alloc_info);
    }
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    ASSERT_MSG(result == VK_SUCCESS, "Failed allocating image with error {}",
               vk::to_string(vk::Result{result}));
    image = vk::Image{unsafe_image};
    size_bytes = alloc_info.size;
    char what[96];
    std::snprintf(what, sizeof(what), "image %ux%ux%u mips %u layers %u %s", image_ci.extent.width,
                  image_ci.extent.height, image_ci.extent.depth, image_ci.mipLevels,
                  image_ci.arrayLayers, vk::to_string(image_ci.format).c_str());
    // gow3 18 Oct 2026: images whose memory took long to get (a new driver allocation; AMD's
    // Windows driver needs up to ~70 ms per one), the first 64.
    if (ms >= 4.0) {
        static std::atomic<int> slow_logged{0};
        if (slow_logged.fetch_add(1, std::memory_order_relaxed) < 64) {
            VmaAllocationInfo2 info2{};
            vmaGetAllocationInfo2(allocator, allocation, &info2);
            std::printf("Slow image create: %.1f ms, %s, %.1f MiB in a %s of %.0f MiB\n", ms, what,
                        alloc_info.size / 1048576.0,
                        info2.dedicatedMemory ? "dedicated allocation" : "VMA block",
                        info2.blockSize / 1048576.0);
        }
    }
    if (GpuAllocationRecords()) {
        RecordGpuAllocation(u64(unsafe_image), alloc_info, what);
    }
    SpareBlockKeeper::Note(allocator, alloc_info.memoryType);
}

Image::Image(const Vulkan::Instance& instance, Vulkan::Runtime& runtime_,
             Common::SlotVector<ImageView>& slot_image_views_, const ImageInfo& info_)
    : guest_begin{info_.guest_address}, guest_end{info_.guest_address + info_.guest_size},
      info{info_}, runtime{&runtime_}, slot_image_views{&slot_image_views_} {
    Gow3Stats::Timer timer{Gow3Stats::t_image_create};
    if (info.pixel_format == vk::Format::eUndefined) {
        return;
    }

    image_uid = global_image_uid.Next();
    mip_hashes.resize(info.resources.levels);
    vk::ImageCreateFlags flags{vk::ImageCreateFlagBits::eMutableFormat |
                               vk::ImageCreateFlagBits::eExtendedUsage};
    if (info.props.is_volume) {
        flags |= vk::ImageCreateFlagBits::e2DArrayCompatible;
        if (instance.Is2dViewOf3dSupported()) {
            flags |= vk::ImageCreateFlagBits::e2DViewCompatibleEXT;
        }
    }
    if (info.props.is_block && instance.IsBlockTexelViewSupported()) {
        flags |= vk::ImageCreateFlagBits::eBlockTexelViewCompatible;
    }

    usage_flags = ImageUsageFlags(instance, info);
    format_features = FormatFeatureFlags(usage_flags);
    if (info.props.is_depth) {
        aspect_mask = vk::ImageAspectFlagBits::eDepth;
        if (info.props.has_stencil) {
            aspect_mask |= vk::ImageAspectFlagBits::eStencil;
        }
    }

    constexpr auto tiling = vk::ImageTiling::eOptimal;
    const auto supported_format = instance.GetSupportedFormat(info.pixel_format, format_features);
    vk::PhysicalDeviceImageFormatInfo2 format_info{
        .format = supported_format,
        .type = ConvertImageType(info.type),
        .tiling = tiling,
        .usage = usage_flags,
        .flags = flags,
    };
    auto image_format_properties =
        instance.GetPhysicalDevice().getImageFormatProperties2(format_info);
    // gow3: storage usage is speculative (see ImageUsageFlags). Drivers that refuse it for a
    // format (AMD's Windows driver for BC6H) get the image without it: creating the unsupported
    // combination anyway loses the device a few frames later.
    if (image_format_properties.result == vk::Result::eErrorFormatNotSupported &&
        (usage_flags & vk::ImageUsageFlagBits::eStorage)) {
        format_info.usage = usage_flags & ~vk::ImageUsageFlagBits::eStorage;
        image_format_properties = instance.GetPhysicalDevice().getImageFormatProperties2(format_info);
        if (image_format_properties.result == vk::Result::eSuccess) {
            usage_flags = format_info.usage;
            format_features = FormatFeatureFlags(usage_flags);
        } else {
            format_info.usage = usage_flags;
        }
    }
    if (image_format_properties.result == vk::Result::eErrorFormatNotSupported) {
        LOG_ERROR(Render_Vulkan, "image format {} type {} is not supported (flags {}, usage {})",
                  vk::to_string(supported_format), vk::to_string(format_info.type),
                  vk::to_string(format_info.flags), vk::to_string(format_info.usage));
    }
    supported_samples = image_format_properties.result == vk::Result::eSuccess
                            ? image_format_properties.value.imageFormatProperties.sampleCounts
                            : vk::SampleCountFlagBits::e1;

    const vk::ImageCreateInfo image_ci = {
        .flags = flags,
        .imageType = ConvertImageType(info.type),
        .format = supported_format,
        .extent{
            .width = info.size.width,
            .height = info.size.height,
            .depth = info.size.depth,
        },
        .mipLevels = static_cast<u32>(info.resources.levels),
        .arrayLayers = static_cast<u32>(info.resources.layers),
        .samples = LiverpoolToVK::NumSamples(info.num_samples, supported_samples),
        .tiling = tiling,
        .usage = usage_flags,
        .initialLayout = vk::ImageLayout::eUndefined,
    };

    backing = &backing_images.emplace_back();
    backing->num_samples = info.num_samples;
    backing->image = UniqueImage{instance.GetDevice(), instance.GetAllocator()};
    backing->image.Create(image_ci);
    NoteGpuAllocation(u64(VkImage(backing->image.image)), info.guest_address, info.guest_size);

    Vulkan::SetObjectName(instance.GetDevice(), GetImage(),
                          "Image {}x{}x{} {} {} {:#x}:{:#x} L:{} M:{} S:{}", info.size.width,
                          info.size.height, info.size.depth, AmdGpu::NameOf(info.tile_mode),
                          vk::to_string(info.pixel_format), info.guest_address, info.guest_size,
                          info.resources.layers, info.resources.levels, info.num_samples);
}

Image::~Image() = default;

ImageView& Image::FindView(const ImageViewInfo& view_info, bool ensure_guest_samples) {
    if (ensure_guest_samples && backing->num_samples > 1 != info.num_samples > 1) {
        runtime->SetBackingSamples(this, info.num_samples);
    }
    const auto& view_infos = backing->image_view_infos;
    if (backing->last_view < view_infos.size() && view_infos[backing->last_view] == view_info) {
        return (*slot_image_views)[backing->image_view_ids[backing->last_view]];
    }
    const auto it = std::ranges::find(view_infos, view_info);
    if (it != view_infos.end()) {
        backing->last_view = static_cast<u32>(std::distance(view_infos.begin(), it));
        const auto view_id = backing->image_view_ids[backing->last_view];
        return (*slot_image_views)[view_id];
    }
    const auto view_id = slot_image_views->insert(runtime->GetInstance(), view_info, *this);
    backing->image_view_infos.emplace_back(view_info);
    backing->image_view_ids.emplace_back(view_id);
    return (*slot_image_views)[view_id];
}

void Image::GetBarriers(Barriers& barriers, vk::ImageLayout dst_layout, vk::AccessFlags2 dst_mask,
                        vk::PipelineStageFlags2 dst_stage,
                        std::optional<SubresourceRange> subres_range) {
    auto& last_state = backing->state;
    auto& subresource_states = backing->subresource_states;

    const bool needs_partial_transition =
        subres_range &&
        (subres_range->base != SubresourceBase{} || subres_range->extent != info.resources);
    const bool partially_transited = !subresource_states.empty();

    if (needs_partial_transition || partially_transited) {
        if (!partially_transited) {
            subresource_states.resize(info.resources.levels * info.resources.layers);
            std::fill(subresource_states.begin(), subresource_states.end(), last_state);
        }

        // In case of partial transition, we need to change the specified subresources only.
        // Otherwise all subresources need to be set to the same state so we can use a full
        // resource transition for the next time.
        // gow3 5 Nov 2026: a garbage T# can ask for levels or layers past the image it matched
        // (the view's levels are not clamped): the range stops at the image's own (it ended the
        // game on the assertion below, exit 23).
        const u32 level_end = std::min<u32>(info.resources.levels,
                                            subres_range ? subres_range->base.level +
                                                               subres_range->extent.levels
                                                         : info.resources.levels);
        const u32 layer_end = std::min<u32>(info.resources.layers,
                                            subres_range ? subres_range->base.layer +
                                                               subres_range->extent.layers
                                                         : info.resources.layers);
        const auto mips =
            needs_partial_transition
                ? std::ranges::views::iota(std::min<u32>(subres_range->base.level, level_end),
                                           level_end)
                : std::views::iota(0u, info.resources.levels);
        const auto layers =
            needs_partial_transition
                ? std::ranges::views::iota(std::min<u32>(subres_range->base.layer, layer_end),
                                           layer_end)
                : std::views::iota(0u, info.resources.layers);

        for (u32 mip : mips) {
            for (u32 layer : layers) {
                // NOTE: these loops may produce a lot of small barriers.
                // If this becomes a problem, we can optimize it by merging adjacent barriers.
                const auto subres_idx = mip * info.resources.layers + layer;
                ASSERT(subres_idx < subresource_states.size());
                auto& state = subresource_states[subres_idx];

                constexpr auto write_flags = vk::AccessFlagBits2::eTransferWrite |
                                             vk::AccessFlagBits2::eShaderWrite |
                                             vk::AccessFlagBits2::eMemoryWrite;
                const bool is_write = static_cast<bool>(state.access_mask & write_flags);
                if (state.layout != dst_layout || state.access_mask != dst_mask || is_write) {
                    barriers.emplace_back(vk::ImageMemoryBarrier2{
                        .srcStageMask = state.pl_stage,
                        .srcAccessMask = state.access_mask,
                        .dstStageMask = dst_stage,
                        .dstAccessMask = dst_mask,
                        .oldLayout = state.layout,
                        .newLayout = dst_layout,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = GetImage(),
                        .subresourceRange{
                            .aspectMask = aspect_mask,
                            .baseMipLevel = mip,
                            .levelCount = 1,
                            .baseArrayLayer = layer,
                            .layerCount = 1,
                        },
                    });
                    state.layout = dst_layout;
                    state.access_mask = dst_mask;
                    state.pl_stage = dst_stage;
                }
            }
        }

        if (!needs_partial_transition) {
            subresource_states.clear();
        }
    } else { // Full resource transition
        constexpr auto write_flags = vk::AccessFlagBits2::eTransferWrite |
                                     vk::AccessFlagBits2::eShaderWrite |
                                     vk::AccessFlagBits2::eMemoryWrite;
        const bool is_write = static_cast<bool>(last_state.access_mask & write_flags);
        if (last_state.layout == dst_layout && last_state.access_mask == dst_mask && !is_write) {
            return;
        }

        barriers.emplace_back(vk::ImageMemoryBarrier2{
            .srcStageMask = last_state.pl_stage,
            .srcAccessMask = last_state.access_mask,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_mask,
            .oldLayout = last_state.layout,
            .newLayout = dst_layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = GetImage(),
            .subresourceRange{
                .aspectMask = aspect_mask,
                .baseMipLevel = 0,
                .levelCount = VK_REMAINING_MIP_LEVELS,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        });
    }

    last_state.layout = dst_layout;
    last_state.access_mask = dst_mask;
    last_state.pl_stage = dst_stage;
}

} // namespace VideoCore
