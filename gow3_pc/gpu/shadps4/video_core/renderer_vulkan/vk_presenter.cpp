// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/debug.h"
#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "sdl_window.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderdoc.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "gow3_overlay.h"
#include "gow3_settings.h"
#include "gow3_graphics.h"
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_warmup_inbox.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/amdgpu/liverpool.h" // gow3 21 Oct 2026: ServiceQueuedCommands
#include "video_core/texture_cache/image.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csetjmp>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <system_error>
#include <vector>
#include <vk_mem_alloc.h>
#ifdef MemoryBarrier
#undef MemoryBarrier // gow3: winnt.h macro (through fmt), clashes with vk::MemoryBarrier
#endif

namespace Vulkan {

namespace {
/// gow3 20 Oct 2026: presents on their own queue (Instance::SeparatePresentQueue) take that
/// queue's lock only; on the graphics queue the submit lock, as before.
std::mutex& PresentLock(const Instance& instance) {
    static std::mutex present_queue_mutex;
    return instance.SeparatePresentQueue() ? present_queue_mutex : Scheduler::submit_mutex;
}
} // namespace

bool CanBlitToSwapchain(const vk::PhysicalDevice physical_device, vk::Format format) {
    const vk::FormatProperties props{physical_device.getFormatProperties(format)};
    return static_cast<bool>(props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst);
}

[[nodiscard]] vk::ImageSubresourceLayers MakeImageSubresourceLayers() {
    return vk::ImageSubresourceLayers{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlit(s32 frame_width, s32 frame_height, s32 dst_width,
                                          s32 dst_height, s32 offset_x, s32 offset_y) {
    return vk::ImageBlit{
        .srcSubresource = MakeImageSubresourceLayers(),
        .srcOffsets =
            std::array{
                vk::Offset3D{
                    .x = 0,
                    .y = 0,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = frame_width,
                    .y = frame_height,
                    .z = 1,
                },
            },
        .dstSubresource = MakeImageSubresourceLayers(),
        .dstOffsets =
            std::array{
                vk::Offset3D{
                    .x = offset_x,
                    .y = offset_y,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = offset_x + dst_width,
                    .y = offset_y + dst_height,
                    .z = 1,
                },
            },
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlitStretch(s32 frame_width, s32 frame_height,
                                                 s32 swapchain_width, s32 swapchain_height) {
    return MakeImageBlit(frame_width, frame_height, swapchain_width, swapchain_height, 0, 0);
}

static vk::Rect2D FitImage(s32 frame_width, s32 frame_height, s32 swapchain_width,
                           s32 swapchain_height) {
    float frame_aspect = static_cast<float>(frame_width) / frame_height;
    float swapchain_aspect = static_cast<float>(swapchain_width) / swapchain_height;

    u32 dst_width = swapchain_width;
    u32 dst_height = swapchain_height;

    if (frame_aspect > swapchain_aspect) {
        dst_height = static_cast<s32>(swapchain_width / frame_aspect);
    } else {
        dst_width = static_cast<s32>(swapchain_height * frame_aspect);
    }

    const s32 offset_x = (swapchain_width - dst_width) / 2;
    const s32 offset_y = (swapchain_height - dst_height) / 2;

    return vk::Rect2D{{offset_x, offset_y}, {dst_width, dst_height}};
}

[[nodiscard]] vk::ImageBlit MakeImageBlitFit(s32 frame_width, s32 frame_height, s32 swapchain_width,
                                             s32 swapchain_height) {
    const auto& dst_rect = FitImage(frame_width, frame_height, swapchain_width, swapchain_height);

    return MakeImageBlit(frame_width, frame_height, dst_rect.extent.width, dst_rect.extent.height,
                         dst_rect.offset.x, dst_rect.offset.y);
}

// gow3: screenshot capture and ImGui overlays removed; the port presents directly.

Presenter::Presenter(Frontend::WindowSDL& window_, AmdGpu::Liverpool* liverpool_)
    : window{window_}, liverpool{liverpool_},
      instance{window, EmulatorSettings.GetGpuId(), EmulatorSettings.IsVkValidationEnabled(),
               EmulatorSettings.IsVkCrashDiagnosticEnabled()},
      draw_scheduler{instance, true}, present_scheduler{instance}, flip_scheduler{instance},
      swapchain{instance, window}, runtime{instance, draw_scheduler},
      rasterizer{std::make_unique<Rasterizer>(instance, draw_scheduler, runtime, liverpool)},
      texture_cache{rasterizer->GetTextureCache()} {
    const u32 num_images = swapchain.GetImageCount();
    const vk::Device device = instance.GetDevice();

    // Create presentation frames.
    present_frames.resize(num_images);
    for (u32 i = 0; i < num_images; i++) {
        Frame& frame = present_frames[i];
        frame.id = i;
        auto fence = Check<"create present done fence">(
            device.createFence({.flags = vk::FenceCreateFlagBits::eSignaled}));
        frame.present_done = fence;
        free_queue.push(&frame);
    }

    fsr_pass.Create(device, instance.GetAllocator(), num_images);
    pp_pass.Create(device, swapchain.GetSurfaceFormat().format,
                   vk::Format::eA2B10G10R10UnormPack32);
    Gow3Overlay::Init(instance, swapchain.GetSurfaceFormat().format, num_images);
    Gow3Settings::Get().hdr_supported = swapchain.HasHDR();
    // Before the pipeline warm-up below: SDR sessions build none of the HDR10 pipelines.
    Gow3Settings::Get().hdr_pipelines = Gow3Settings::Get().hdr && swapchain.HasHDR();

    const bool binary_cache = EmulatorSettingsImpl::Flag("GOW3_PIPELINE_BINARIES", false) &&
                              EmulatorSettings.IsPipelineCacheEnabled();
    const bool capture_only = EmulatorSettingsImpl::Flag("GOW3_PIPELINE_BINARY_ISOLATED", false) &&
                              EmulatorSettingsImpl::Flag("GOW3_PIPELINE_CAPTURE_ONLY", false);
    // gow3 18 Oct 2026: the background warm-up also with pipeline binaries. On demand, each compute
    // shader was translated on the GPU thread the first time a level used it: Uncharted 3 hitched
    // 90-170 ms right after loading (35 compiles of 10-15 ms in 5 s); with the warm-up (2,440
    // pipelines in 16 s on background threads during the title screen) none were left in play.
    // GOW3_ON_DEMAND_PIPELINES=1 keeps the on-demand start.
    if (EmulatorSettingsImpl::Flag("GOW3_ON_DEMAND_PIPELINES", false) &&
        UseOnDemandPipelines(binary_cache, instance.GetPipelineBinaryGlobalKey().keySize != 0,
                            capture_only)) {
        rasterizer->GetPipelineCache().StartOnDemand();
        return;
    }

    // gow3: by default the cached pipelines load while the game runs (a corner note shows the
    // progress). Otherwise rebuilding them takes up to minutes before the first game frame:
    // show what is happening instead of a black window.
    const char* background_warmup = std::getenv("GOW3_BACKGROUND_WARMUP");
    if ((!background_warmup || *background_warmup != '0') &&
        rasterizer->GetPipelineCache().StartBackgroundWarmUp()) {
        return;
    }
    Gow3Overlay::SetLoading(true, 0, 0);
    PresentLoadingFrame();
    auto last = std::chrono::steady_clock::now();
    rasterizer->GetPipelineCache().WarmUp([&](u32 done, u32 total) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last < std::chrono::milliseconds(250)) {
            return;
        }
        last = now;
        Gow3Overlay::SetLoading(true, done, total);
        PresentLoadingFrame();
    });
    Gow3Overlay::SetLoading(false, 0, 0);

}

Presenter::~Presenter() {

    draw_scheduler.Finish();
    present_scheduler.Finish();
    flip_scheduler.Finish();
    Check(draw_scheduler.CommandBuffer().reset());
    Check(present_scheduler.CommandBuffer().reset());
    Check(flip_scheduler.CommandBuffer().reset());

    const vk::Device device = instance.GetDevice();
    for (auto& frame : present_frames) {
        vmaDestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
        device.destroyImageView(frame.image_view);
        device.destroyFence(frame.present_done);
    }
    if (overlay_image) {
        device.destroyImageView(overlay_view);
        vmaDestroyImage(instance.GetAllocator(), overlay_image, overlay_allocation);
    }
}

void Presenter::RenderOverlay(vk::CommandBuffer cmdbuf, vk::Extent2D extent) {
    if (!swapchain.GetHDR()) {
        Gow3Overlay::Render(cmdbuf, swapchain.ImageView(), extent);
        return;
    }
    // HDR10: the menu keeps its 8-bit pipeline, drawn into a cleared image of the window's size.
    const vk::Device device = instance.GetDevice();
    if (!overlay_image || overlay_extent != extent) {
        if (overlay_image) {
            // vkDeviceWaitIdle excludes queue submissions; the game scheduler submits on another
            // thread (as in Swapchain::Destroy). The present lock is only taken later, at submit.
            std::scoped_lock submit_lock{Scheduler::submit_mutex};
            Check(device.waitIdle());
            device.destroyImageView(overlay_view);
            vmaDestroyImage(instance.GetAllocator(), overlay_image, overlay_allocation);
        }
        const VkImageCreateInfo image_info = static_cast<VkImageCreateInfo>(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = swapchain.GetSurfaceFormat().format,
            .extent = {extent.width, extent.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .usage = vk::ImageUsageFlagBits::eColorAttachment |
                     vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
        });
        const VmaAllocationCreateInfo alloc_info = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VkImage image{};
        if (vmaCreateImage(instance.GetAllocator(), &image_info, &alloc_info, &image,
                           &overlay_allocation, nullptr) != VK_SUCCESS) {
            overlay_image = vk::Image{};
            return;
        }
        overlay_image = vk::Image{image};
        overlay_view = Check<"create overlay view">(device.createImageView(vk::ImageViewCreateInfo{
            .image = overlay_image,
            .viewType = vk::ImageViewType::e2D,
            .format = swapchain.GetSurfaceFormat().format,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        overlay_extent = extent;
    }
    const auto barrier = [&](vk::ImageLayout from, vk::ImageLayout to, vk::AccessFlags src,
                             vk::AccessFlags dst) {
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eAllCommands, {}, {}, {},
                               vk::ImageMemoryBarrier{
                                   .srcAccessMask = src,
                                   .dstAccessMask = dst,
                                   .oldLayout = from,
                                   .newLayout = to,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .image = overlay_image,
                                   .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
                               });
    };
    barrier(vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal, {},
            vk::AccessFlagBits::eTransferWrite);
    cmdbuf.clearColorImage(overlay_image, vk::ImageLayout::eTransferDstOptimal,
                           vk::ClearColorValue{std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}},
                           vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1});
    barrier(vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eColorAttachmentOptimal,
            vk::AccessFlagBits::eTransferWrite,
            vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite);
    Gow3Overlay::Render(cmdbuf, overlay_view, extent);
    barrier(vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::AccessFlagBits::eColorAttachmentWrite, vk::AccessFlagBits::eShaderRead);
    pp_pass.RenderOverlay(cmdbuf, overlay_view, swapchain.ImageView(), extent, pp_settings);
}

bool Presenter::IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const {
    return std::ranges::find(vo_buffers_addr, color_buffer.Address()) != vo_buffers_addr.cend();
}

void Presenter::RecreateFrame(Frame* frame, u32 width, u32 height) {
    const vk::Device device = instance.GetDevice();
    if (frame->image_view) {
        device.destroyImageView(frame->image_view);
    }
    if (frame->image) {
        vmaDestroyImage(instance.GetAllocator(), frame->image, frame->allocation);
    }

    // gow3: HDR10 frames keep the 10-bit format up to the swapchain (an 8-bit frame would band).
    const vk::Format format = swapchain.GetImageFormat();
    const vk::ImageCreateInfo image_info = {
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
                 vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled,
    };

    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    VkImage unsafe_image{};
    VkImageCreateInfo unsafe_image_info = static_cast<VkImageCreateInfo>(image_info);

    VkResult result = vmaCreateImage(instance.GetAllocator(), &unsafe_image_info, &alloc_info,
                                     &unsafe_image, &frame->allocation, nullptr);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating texture with error {}",
                     vk::to_string(vk::Result{result}));
        UNREACHABLE();
    }
    frame->image = vk::Image{unsafe_image};
    SetObjectName(device, frame->image, "Frame image #{}", frame->id);

    const vk::ImageViewCreateInfo view_info = {
        .image = frame->image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    auto view = Check<"create frame image view">(device.createImageView(view_info));
    frame->image_view = view;
    frame->width = width;
    frame->height = height;

    frame->is_hdr = swapchain.GetHDR();
}

Frame* Presenter::PrepareLastFrame() {
    if (last_submit_frame == nullptr) {
        return nullptr;
    }

    Frame* frame = last_submit_frame;

    while (true) {
        vk::Result result = instance.GetDevice().waitForFences(frame->present_done, false,
                                                               std::numeric_limits<u64>::max());
        if (result == vk::Result::eSuccess) {
            break;
        }
        if (result == vk::Result::eTimeout) {
            continue;
        }
        ASSERT_MSG(result != vk::Result::eErrorDeviceLost,
                   "Device lost during waiting for a frame");
    }

    auto& scheduler = flip_scheduler;
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier =
        vk::ImageMemoryBarrier2{.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
                                .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                                .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                                .newLayout = vk::ImageLayout::eGeneral,
                                .image = frame->image,
                                .subresourceRange{frame_subresources}};

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

static vk::Format GetFrameViewFormat(const Libraries::VideoOut::PixelFormat format) {
    switch (format) {
    case Libraries::VideoOut::PixelFormat::A8B8G8R8Srgb:
        return vk::Format::eR8G8B8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb:
        return vk::Format::eB8G8R8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A2R10G10B10:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Bt2020Pq:
        return vk::Format::eA2R10G10B10UnormPack32;
    default:
        break;
    }
    UNREACHABLE_MSG("Unknown format={}", static_cast<u32>(format));
    return {};
}

// gow3: raw copy of an image for GOW3_FRAME_DUMP_TRIGGER, written once the GPU is done.
static void DumpRaw(const Instance& instance, Scheduler& scheduler, vk::CommandBuffer cmdbuf,
                    vk::Image image, vk::ImageLayout layout, u32 w, u32 h, const char* name,
                    int index, u32 pixel_bytes = 4,
                    vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor,
                    u32 block_width = 1, u32 block_height = 1, u32 layer = 0) {
    const VkBufferCreateInfo buffer_ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .size = VkDeviceSize((w + block_width - 1) / block_width) *
                                               ((h + block_height - 1) / block_height) * pixel_bytes,
                                       .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo alloc_ci{
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VkBuffer buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &buffer, &allocation,
                        &info) != VK_SUCCESS) {
        return;
    }
    cmdbuf.copyImageToBuffer(image, layout, buffer,
                             vk::BufferImageCopy{.imageSubresource = {aspect, 0, layer, 1},
                                                 .imageExtent = {w, h, 1}});
    const char* dir = std::getenv("GOW3_DUMP_DIR");
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%s_%03d_%ux%u.raw", dir && *dir ? dir : "out/dump", name,
                  index, w, h);
    scheduler.DeferPriorityOperation([allocator = instance.GetAllocator(), buffer, allocation, info,
                                      size = buffer_ci.size, file = std::string{path}] {
        vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
        if (FILE* f = std::fopen(file.c_str(), "wb")) {
            std::fwrite(info.pMappedData, 1, size, f);
            std::fclose(f);
        }
        vmaDestroyBuffer(allocator, buffer, allocation);
    });
}

Frame* Presenter::PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                               VAddr cpu_address) {
    // gow3: GOW3_FRAME_DUMP_TRIGGER=<file>: when the file exists it is removed and this frame's
    // guest display buffer and presented image are written to GOW3_DUMP_DIR (default out/dump)
    // as raw 32-bit pixels (menus and movies included; the upscaler dumps scene frames only).
    static const char* frame_dump_trigger = std::getenv("GOW3_FRAME_DUMP_TRIGGER");
    static int frame_dump_index = 0;
    // gow3 20 Oct 2026 (idle CPU): the file is looked for at most every 250 ms. A remove() per
    // frame (a failed file open in the kernel) was 0.6% of the GPU thread with diagnostics on.
    static auto next_dump_check = std::chrono::steady_clock::time_point{};
    bool frame_dump = false;
    if (frame_dump_trigger) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_dump_check) {
            next_dump_check = now + std::chrono::milliseconds(250);
            frame_dump = std::remove(frame_dump_trigger) == 0;
        }
    }
    // gow3: scaled upscaler presets: the output-size display buffer drawn by the port.
    TemporalUpscaler::Display display{};
    const bool upscaled = rasterizer->GetUpscaler().DisplayOverride(cpu_address, display);
    VideoCore::ImageId image_id{};
    if (!upscaled) {
        auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
        image_id = texture_cache.FindImage(desc);
        texture_cache.UpdateImage(image_id);
    }

    Frame* frame = GetRenderFrame();

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange{frame_subresources},
    };

    draw_scheduler.EndRendering();
    const auto cmdbuf = draw_scheduler.CommandBuffer();
    // Optional one-shot request: guest address and exact guest size per line, or
    // "size <width> <height>" for every cached image of that size, "max <width> <height>" for every
    // one up to that size, "range <lo> <hi>" (hex) for every one in those guest addresses (up to 256
    // in all).
    // Dump intermediate targets to locate where geometry/lighting disappears.
    if (frame_dump) {
        const char* dir = std::getenv("GOW3_DUMP_DIR");
        const auto request = std::filesystem::path(dir ? dir : "out/dump") / "guest-images.trigger";
        if (FILE* f = std::fopen(request.string().c_str(), "r")) {
            std::vector<std::pair<unsigned long long, unsigned long long>> requests;
            char line[256];
            constexpr size_t MaxRequests = 256;
            while (requests.size() < MaxRequests && std::fgets(line, sizeof(line), f)) {
                unsigned long long a, b;
                unsigned w, h;
                std::vector<std::pair<VAddr, u32>> images;
                if (std::sscanf(line, "size %u %u", &w, &h) == 2) {
                    images = texture_cache.DebugImagesOfSize(w, h);
                } else if (std::sscanf(line, "max %u %u", &w, &h) == 2) {
                    images = texture_cache.DebugImagesWhere(16, w, 16, h, 0, ~VAddr{0});
                } else if (std::sscanf(line, "range %llx %llx", &a, &b) == 2) {
                    images = texture_cache.DebugImagesWhere(1, ~0u, 1, ~0u, a, b);
                } else if (std::sscanf(line, "%llx %llx", &a, &b) == 2) {
                    requests.emplace_back(a, b);
                }
                for (const auto& [image_address, image_size] : images) {
                    if (requests.size() < MaxRequests) requests.emplace_back(image_address, image_size);
                }
            }
            for (const auto& [address, size] : requests) {
                const auto id = texture_cache.FindImageFromRange(address, size, false);
                if (!id) continue;
                auto& image = texture_cache.GetImage(id);
                const auto format = image.info.pixel_format;
                const bool depth16 = format == vk::Format::eD16Unorm;
                const bool depth = format == vk::Format::eD32SfloatS8Uint || format == vk::Format::eD32Sfloat ||
                                   depth16;
                const bool bc1 = format == vk::Format::eBc1RgbaUnormBlock ||
                                 format == vk::Format::eBc1RgbaSrgbBlock;
                const bool bc5 = format == vk::Format::eBc5UnormBlock ||
                                 format == vk::Format::eBc5SnormBlock;
                const bool bc16 = bc5 || format == vk::Format::eBc3UnormBlock ||
                                  format == vk::Format::eBc3SrgbBlock ||
                                  format == vk::Format::eBc2UnormBlock ||
                                  format == vk::Format::eBc2SrgbBlock ||
                                  format == vk::Format::eBc7UnormBlock ||
                                  format == vk::Format::eBc7SrgbBlock;
                bool compressed = bc1 || bc16;
                u32 bytes = format == vk::Format::eR8Unorm ? 1
                            : format == vk::Format::eR8G8Unorm || depth16 ? 2
                            : bc16 ? 16 : (bc1 || format == vk::Format::eR16G16B16A16Sfloat ? 8 : 4);
                if (image.info.num_samples != 1) continue;
                if (!depth && !compressed && image.info.num_bits != bytes * 8) {
                    // Any other format: its texel (or 4x4 block) size.
                    compressed = image.info.props.is_block;
                    bytes = image.info.num_bits / 8;
                    if (bytes == 0) continue;
                }
                runtime.Transit(&image, vk::ImageLayout::eTransferSrcOptimal,
                                vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
                runtime.FlushBarriers();
                const auto name = std::format("guest_{:x}_{}", address, vk::to_string(format));
                DumpRaw(instance, draw_scheduler, cmdbuf, image.GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                        image.info.size.width, image.info.size.height, name.c_str(), frame_dump_index,
                        bytes, depth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor,
                        compressed ? 4 : 1, compressed ? 4 : 1);
                // Array images (shadow cascades): every further layer as <name>_L<n>.
                for (u32 layer = 1; layer < std::min<u32>(image.info.resources.layers, 8); ++layer) {
                    const auto layer_name = std::format("{}_L{}", name, layer);
                    DumpRaw(instance, draw_scheduler, cmdbuf, image.GetImage(),
                            vk::ImageLayout::eTransferSrcOptimal, image.info.size.width,
                            image.info.size.height, layer_name.c_str(), frame_dump_index, bytes,
                            depth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor,
                            compressed ? 4 : 1, compressed ? 4 : 1, layer);
                }
                // Keep enough layout/state evidence to compare the cached image with a
                // separate read-only guest-memory snapshot. Do not refresh the image here:
                // that would hide the stale-cache condition this capture is investigating.
                const auto metadata_path = std::filesystem::path(dir ? dir : "out/dump") /
                    std::format("{}_{:03}.txt", name, frame_dump_index);
                if (FILE* metadata = std::fopen(metadata_path.string().c_str(), "w")) {
                    std::fprintf(metadata,
                        "address=%llx guest_size=%x flags=%x pitch=%u tile_mode=%u "
                        "array_mode=%u bank_swizzle=%u width=%u height=%u bits=%u\n",
                        static_cast<unsigned long long>(image.info.guest_address), image.info.guest_size,
                        static_cast<unsigned>(image.flags), image.info.pitch,
                        static_cast<unsigned>(image.info.tile_mode), static_cast<unsigned>(image.info.array_mode),
                        image.info.bank_swizzle, image.info.size.width, image.info.size.height, image.info.num_bits);
                    for (u32 mip = 0; mip < image.info.resources.levels; ++mip) {
                        const auto& layout = image.info.mips_layout[mip];
                        std::fprintf(metadata, "mip=%u offset=%x size=%x pitch=%u height=%u\n",
                                     mip, layout.offset, layout.size, layout.pitch, layout.height);
                    }
                    std::fclose(metadata);
                }
                if (format == vk::Format::eD32SfloatS8Uint) {
                    const auto stencil_name = name + "_stencil";
                    DumpRaw(instance, draw_scheduler, cmdbuf, image.GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                            image.info.size.width, image.info.size.height, stencil_name.c_str(), frame_dump_index,
                            1, vk::ImageAspectFlagBits::eStencil);
                }
            }
            std::fclose(f);
            std::filesystem::remove(request);
        }
    }
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    VideoCore::ImageViewInfo view_info{};
    view_info.format = GetFrameViewFormat(attribute.attrib.pixel_format);
    // Exclude alpha from output frame to avoid blending with UI.
    view_info.mapping.a = vk::ComponentSwizzle::eOne;

    vk::ImageView image_view{};
    vk::Extent2D image_size{};
    // gow3: the HDR10 chain's display buffer is RGBA16F holding the game's stored values
    // (TemporalUpscaler::OnComposite): read as they are, like a 10-bit sRGB buffer.
    const bool stored_display = upscaled && display.format == vk::Format::eR16G16B16A16Sfloat;
    if (upscaled) {
        const auto device = instance.GetDevice();
        image_view = Check(device.createImageView({
            .image = display.image,
            .viewType = vk::ImageViewType::e2D,
            .format = stored_display ? display.format : view_info.format,
            .components = {vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
                           vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eOne},
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        draw_scheduler.DeferOperation([device, image_view] { device.destroyImageView(image_view); });
        image_size = vk::Extent2D{display.width, display.height};
        if (frame_dump) {
            // gow3: the upscaled display buffer the presenter reads (HDR10 checks compare it
            // with the "output" dump).
            const vk::MemoryBarrier2 written{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .dstAccessMask = vk::AccessFlagBits2::eTransferRead};
            cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &written});
            DumpRaw(instance, draw_scheduler, cmdbuf, display.image, vk::ImageLayout::eGeneral,
                    display.width, display.height, "display", frame_dump_index,
                    stored_display ? 8 : 4);
        }
        const vk::ImageMemoryBarrier2 to_read{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput |
                            vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = display.image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read});
    } else {
        auto& image = texture_cache.GetImage(image_id);
        if (frame_dump) {
            runtime.Transit(&image, vk::ImageLayout::eTransferSrcOptimal,
                            vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
            runtime.FlushBarriers();
            DumpRaw(instance, draw_scheduler, cmdbuf, image.GetImage(),
                    vk::ImageLayout::eTransferSrcOptimal, image.info.size.width,
                    image.info.size.height, "display", frame_dump_index);
        }
        image_view = *image.FindView(view_info).image_view;
        image_size = vk::Extent2D{image.info.size.width, image.info.size.height};
        runtime.Transit(&image, vk::ImageLayout::eShaderReadOnlyOptimal,
                        vk::PipelineStageFlagBits2::eFragmentShader,
                        vk::AccessFlagBits2::eShaderRead);
        runtime.FlushBarriers();
    }
    expected_ratio = static_cast<float>(image_size.width) / static_cast<float>(image_size.height);

    // gow3: FSR 1, RCAS and its strength come from the in-game menu, every frame.
    const auto& graphics = Gow3Settings::Get();
    fsr_settings.enable = graphics.fsr1;
    fsr_settings.use_rcas = graphics.rcas;
    fsr_settings.rcas_attenuation = Gow3Graphics::RcasAttenuation(graphics.rcas_strength);
    Gow3Settings::Get().image_width = int(image_size.width);
    Gow3Settings::Get().image_height = int(image_size.height);
    Gow3Settings::Get().window_width = int(frame->width);
    Gow3Settings::Get().window_height = int(frame->height);
    Gow3Settings::Get().rcas_applied =
        fsr_settings.use_rcas &&
        Gow3Graphics::FsrPassRuns(fsr_settings.enable, true, int(image_size.width), int(image_size.height),
                                  int(frame->width), int(frame->height));
    // gow3: FSR's HDR sampling squares its output (for a guest PQ image); the HDR10 encode reads
    // the game's ordinary SDR image, so FSR 1 / RCAS stay in SDR mode for it.
    const bool encode_hdr = pp_settings.hdr == HostPasses::PostProcessingPass::HdrEncode;
    image_view = fsr_pass.Render(cmdbuf, image_view, image_size, {frame->width, frame->height},
                                 fsr_settings, frame->is_hdr && !encode_hdr);

    // Vulkan has no sRGB variant of the 10-bit format, so an A2R10G10B10Srgb buffer reaches
    // the post process pass still sRGB encoded and has to be decoded there instead.
    pp_settings.srgb_input =
        attribute.attrib.pixel_format == Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb ||
        stored_display;
    // The float composition is expressed in UI-white units: the scene was rebased before
    // HUD/menu blending, so its physical brightness stays independent of UI brightness.
    pp_settings.game_nits = stored_display && display.white_nits > 0.0f
                                ? display.white_nits : float(graphics.hdr_game_nits.load());
    pp_settings.peak_nits = float(graphics.hdr_peak_nits.load());
    pp_settings.ui_nits = float(graphics.hdr_ui_nits.load());
    pp_pass.Render(cmdbuf, image_view, image_size, *frame, pp_settings);
    if (frame_dump) {
        // The presented image as the swapchain blit reads it (General after the pass).
        const vk::MemoryBarrier2 done{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                      .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                                      .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                      .dstAccessMask = vk::AccessFlagBits2::eTransferRead};
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &done});
        DumpRaw(instance, draw_scheduler, cmdbuf, frame->image, vk::ImageLayout::eGeneral,
                frame->width, frame->height, "output", frame_dump_index);
        // gow3: what the raw files hold, for the offline HDR10 checks.
        const char* dir = std::getenv("GOW3_DUMP_DIR");
        const auto info_path = std::filesystem::path(dir && *dir ? dir : "out/dump") /
                               std::format("output_{:03}.txt", frame_dump_index);
        if (FILE* f = std::fopen(info_path.string().c_str(), "w")) {
            std::fprintf(f,
                         "output_format=%s\nguest_format=%u\nview_format=%s\nhdr_mode=%u\n"
                         "srgb_input=%u\ngame_nits=%.1f\npeak_nits=%.1f\nui_nits=%.1f\n"
                         "rcas=%d\nfsr1=%d\ninput=%ux%u\noutput=%ux%u\nstored_display=%d\n",
                         vk::to_string(swapchain.GetImageFormat()).c_str(),
                         u32(attribute.attrib.pixel_format), vk::to_string(view_info.format).c_str(),
                         pp_settings.hdr, pp_settings.srgb_input, pp_settings.game_nits,
                         pp_settings.peak_nits, pp_settings.ui_nits, int(fsr_settings.use_rcas),
                         int(fsr_settings.enable), image_size.width, image_size.height,
                         frame->width, frame->height, int(stored_display));
            std::fclose(f);
        }
        ++frame_dump_index;
    }

    // Flush frame creation commands.
    frame->ready_semaphore = draw_scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = draw_scheduler.CurrentTick();
    SubmitInfo info{};
    draw_scheduler.Flush(info);

    // gow3: the GPU command thread runs at most GOW3_FRAMES_AHEAD (default 1) guest frames
    // ahead of the GPU: it waits here for the frame that many flips back. When the GPU is the
    // bottleneck it finishes frames at an even rate; without this bound the command thread ran
    // ahead and then blocked wherever a resource ran out, so flips (and the guest's frame
    // timing) came in bursts: 12.5/25 ms alternation at 80 FPS. 0 turns it off.
    // The in-game menu changes it live (Gow3Settings::frames_queued; GOW3_FRAMES_AHEAD at start).
    const u32 frames_ahead = u32(std::max(0, Gow3Settings::Get().frames_queued.load()));
    if (!frames_ahead) {
        recent_frame_ticks.clear();
    } else {
        recent_frame_ticks.push_back(frame->ready_tick);
        while (recent_frame_ticks.size() > frames_ahead) {
            const u64 tick = recent_frame_ticks.front();
            recent_frame_ticks.pop_front();
            if (recent_frame_ticks.size() == frames_ahead) {
                // gow3 18 Oct 2026: while the command thread waits here for the GPU (up to a
                // frame), readbacks whose GPU work is done land meanwhile, and with them the
                // fences the guest waits for (ray-cast results). A plain wait held them until the
                // whole frame was done: Uncharted 3 on the rooftops made two or three 5-7 ms
                // frames, then one of 25-35 ms waiting for its ray casts (75 FPS, 1% lows 22 FPS
                // at the 120 limit). GOW3_POLL_WHILE_WAITING=0 waits plainly (A/B).
                static const bool poll_while_waiting = [] {
                    const char* env = std::getenv("GOW3_POLL_WHILE_WAITING");
                    return !(env && env[0] == '0');
                }();
                // gow3 20 Oct 2026 (idle CPU): the 0.25 ms polls woke the command thread up to
                // ~4,000 times a second while the GPU was the bottleneck. Readbacks land in tick
                // order once the GPU has passed their tick, so after a poll the oldest pending
                // one can land no sooner than the GPU's next finished submission: the thread
                // now sleeps until that submission (or the frame's tick) is done, lands what is
                // finished and repeats; with no readback pending it waits plainly (below). Each
                // readback still lands as soon as its submission is done (sooner than the next
                // 0.25 ms poll). Readbacks left after a poll although done (the test knobs
                // GOW3_TEST_READBACK_HOLD/LAG hold them) and a GPU that finishes nothing for
                // 1 s go back to the 0.25 ms polls. GOW3_POLL_ON_COMPLETION=0: the 0.25 ms polls.
                static const bool poll_on_completion = [] {
                    const char* env = std::getenv("GOW3_POLL_ON_COMPLETION");
                    const bool on = !(env && env[0] == '0') &&
                                    !std::getenv("GOW3_TEST_READBACK_HOLD") &&
                                    !std::getenv("GOW3_TEST_READBACK_LAG");
                    std::printf("Presenter: frames-ahead wait lands readbacks %s\n",
                                on ? "as each GPU submission finishes (GOW3_POLL_ON_COMPLETION=0: "
                                     "every 0.25 ms)"
                                   : "every 0.25 ms");
                    return on;
                }();
                // gow3 21 Oct 2026 (GOW3_WAIT_SERVICES_FAULTS=1, the CPU worker): guest fault
                // requests queued while this wait runs are served here (Uncharted 2: read faults
                // waited ~10 ms each for the command thread, 20-38% of a guest thread): the wait
                // goes in slices of at most 1 ms, re-checking the frame's tick after each slice
                // and each served request (one may submit work), and lands readbacks as each
                // submission finishes, as below. Only on the GPU command thread with the draw
                // recording thread idle (Liverpool::ServiceQueuedCommands checks).
                if (poll_while_waiting && rasterizer && liverpool &&
                    AmdGpu::Liverpool::WaitServicesFaults()) {
                    auto* semaphore = draw_scheduler.GetWorkSemaphore();
                    auto& buffer_cache = rasterizer->GetBufferCache();
                    Gow3Stats::WaitTimer timer{Gow3Stats::tick_wait_ns};
                    buffer_cache.PollReadbacksNow();
                    while (!semaphore->IsFree(tick)) {
                        liverpool->ServiceQueuedCommands();
                        if (semaphore->IsFree(tick)) {
                            break;
                        }
                        const u64 newest = buffer_cache.NewestPendingReadbackTick();
                        const u64 known = semaphore->KnownGpuTick();
                        const u64 target = newest > known ? std::min(tick, known + 1) : tick;
                        if (semaphore->WaitFor(target, 1'000'000)) {
                            buffer_cache.PollReadbacksNow();
                        }
                    }
                    liverpool->ServiceQueuedCommands();
                    buffer_cache.PollReadbacksNow();
                } else if (poll_while_waiting && rasterizer) {
                    auto* semaphore = draw_scheduler.GetWorkSemaphore();
                    auto& buffer_cache = rasterizer->GetBufferCache();
                    Gow3Stats::WaitTimer timer{Gow3Stats::tick_wait_ns};
                    if (poll_on_completion) {
                        bool on_completion = true;
                        buffer_cache.PollReadbacksNow();
                        while (!semaphore->IsFree(tick)) {
                            if (on_completion) {
                                // The newest pending readback's tick (0: none pending, also
                                // counting readbacks without data pieces, e.g. fence-only); the
                                // poll refreshed the GPU tick when any was pending.
                                const u64 newest = buffer_cache.NewestPendingReadbackTick();
                                if (newest == 0) {
                                    break; // nothing to land: the plain wait below
                                }
                                const u64 known = semaphore->KnownGpuTick();
                                if (newest > known) {
                                    if (semaphore->WaitFor(std::min(tick, known + 1),
                                                           1'000'000'000)) {
                                        buffer_cache.PollReadbacksNow();
                                        continue;
                                    }
                                }
                                on_completion = false; // the 0.25 ms polls, as before
                            }
                            if (semaphore->WaitFor(tick, 250'000)) {
                                break;
                            }
                            buffer_cache.PollReadbacksNow();
                        }
                    } else {
                        while (!semaphore->IsFree(tick)) {
                            buffer_cache.PollReadbacksNow();
                            if (semaphore->WaitFor(tick, 250'000)) {
                                break;
                            }
                        }
                    }
                    buffer_cache.PollReadbacksNow();
                }
                draw_scheduler.Wait(tick);
            }
        }
    }
    return frame;
}

Frame* Presenter::PrepareBlankFrame(bool present_thread) {
    // Request a free presentation frame.
    Frame* frame = GetRenderFrame();

    auto& scheduler = present_thread ? present_scheduler : draw_scheduler;
    scheduler.EndRendering();

    const auto cmdbuf = scheduler.CommandBuffer();

    constexpr vk::ImageSubresourceRange simple_subresource = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const auto post_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const vk::RenderingAttachmentInfo attachment = {
        .imageView = frame->image_view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .extent = {frame->width, frame->height},
            },
        .layerCount = 1,
        .colorAttachmentCount = 1u,
        .pColorAttachments = &attachment,
    };

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    cmdbuf.beginRendering(rendering_info);
    cmdbuf.endRendering();

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &post_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

void Presenter::PresentLoadingFrame() {
    if (window.GetWidth() == 0 || window.GetHeight() == 0) {
        return;
    }
    swapchain.UpdateVsync();
    if (window.GetWidth() != swapchain.GetWidth() || window.GetHeight() != swapchain.GetHeight()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
    }
    if (!swapchain.AcquireNextImage()) {
        return;
    }
    const vk::Image image = swapchain.Image();
    const vk::Extent2D extent = swapchain.GetExtent();
    const auto cmdbuf = present_scheduler.CommandBuffer();
    const vk::ImageSubresourceRange range{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };
    const auto barrier = [&](vk::ImageLayout from, vk::ImageLayout to, vk::AccessFlags src,
                             vk::AccessFlags dst) {
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eAllCommands, {}, {}, {},
                               vk::ImageMemoryBarrier{
                                   .srcAccessMask = src,
                                   .dstAccessMask = dst,
                                   .oldLayout = from,
                                   .newLayout = to,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .image = image,
                                   .subresourceRange = range,
                               });
    };
    barrier(vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal, {},
            vk::AccessFlagBits::eTransferWrite);
    cmdbuf.clearColorImage(image, vk::ImageLayout::eTransferDstOptimal,
                           vk::ClearColorValue{std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}},
                           range);
    barrier(vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eColorAttachmentOptimal,
            vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eColorAttachmentWrite);
    RenderOverlay(cmdbuf, extent);
    barrier(vk::ImageLayout::eColorAttachmentOptimal, swapchain.FinalLayout(),
            vk::AccessFlagBits::eColorAttachmentWrite, {});
    SubmitInfo info{};
    if (!swapchain.IsHeadless()) {
        info.AddWait(swapchain.GetImageAcquiredSemaphore());
        info.AddSignal(swapchain.GetPresentReadySemaphore());
    }
    present_scheduler.Flush(info);
    if (swapchain.IsHeadless()) present_scheduler.Finish();
    {
        std::scoped_lock present_lock{PresentLock(instance)};
        swapchain.Present();
    }
    // A few frames a second during start-up: waiting keeps semaphores and the swapchain simple.
    present_scheduler.Finish();
}

void Presenter::Present(Frame* frame, bool is_reusing_frame, bool is_game_frame) {
    // Free the frame for reuse
    const auto free_frame = [&] {
        if (!is_reusing_frame) {
            last_submit_frame = frame;
            std::scoped_lock fl{free_mutex};
            free_queue.push(frame);
            free_cv.notify_one();
        }
    };

    // gow3: a minimized window has no pixels on Windows (0x0); keep the swapchain and skip
    // presenting until the window is restored.
    if (window.GetWidth() == 0 || window.GetHeight() == 0) {
        free_frame();
        return;
    }

    // Recreate the swapchain if the window was resized.
    swapchain.UpdateVsync();
    if (window.GetWidth() != swapchain.GetWidth() || window.GetHeight() != swapchain.GetHeight()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
    }

    if (!swapchain.AcquireNextImage()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
        if (!swapchain.AcquireNextImage()) {
            // User resizes the window too fast and GPU can't keep up. Skip this frame.
            LOG_WARNING(Render_Vulkan, "Skipping frame!");
            free_frame();
            return;
        }
    }

    // Reset fence for queue submission. Do it here instead of GetRenderFrame() because we may
    // skip frame because of slow swapchain recreation. If a frame skip occurs, we skip signal
    // the frame's present fence and future GetRenderFrame() call will hang waiting for this frame.
    const auto reset_result = instance.GetDevice().resetFences(frame->present_done);
    ASSERT_MSG(reset_result == vk::Result::eSuccess,
               "Unexpected error resetting present done fence: {}", vk::to_string(reset_result));

    // gow3: the game frame is blitted (letterboxed) straight into the swapchain image.
    const vk::Image swapchain_image = swapchain.Image();
    auto& scheduler = present_scheduler;
    const auto cmdbuf = scheduler.CommandBuffer();

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = "Present",
        });
    }

    {
        const vk::Extent2D extent = swapchain.GetExtent();
        SetExpectedGameSize(s32(extent.width), s32(extent.height));
        const vk::ImageSubresourceRange color_range{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        };
        const std::array pre_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eNone,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = frame->image,
                .subresourceRange = color_range,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlagBits::eByRegion,
                               {}, {}, pre_barriers);
        const vk::ImageBlit blit =
            MakeImageBlitFit(frame->width, frame->height, extent.width, extent.height);
        // gow3 21 Oct 2026 (GPU worker): GOW3_PRESENT_SKIP_CLEAR=1 clears the swapchain image to
        // black (the letterbox bars) only when the blit leaves part of it uncovered. The frame is
        // made at the swapchain's size whenever the aspect ratios match, and the blit then writes
        // every pixel: the clear was a full-screen write per frame for nothing (integrated GPUs
        // share their memory bandwidth with the CPU).
        static const bool skip_clear = [] {
            const char* env = std::getenv("GOW3_PRESENT_SKIP_CLEAR");
            return env && env[0] == '1';
        }();
        const bool covers_all = blit.dstOffsets[0].x == 0 && blit.dstOffsets[0].y == 0 &&
                                u32(blit.dstOffsets[1].x) == extent.width &&
                                u32(blit.dstOffsets[1].y) == extent.height;
        if (!skip_clear || !covers_all) {
            const vk::ClearColorValue black{std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}};
            cmdbuf.clearColorImage(swapchain_image, vk::ImageLayout::eTransferDstOptimal, black,
                                   color_range);
            const vk::MemoryBarrier clear_done{
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
            };
            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                   vk::PipelineStageFlagBits::eTransfer,
                                   vk::DependencyFlagBits::eByRegion, clear_done, {}, {});
        }
        cmdbuf.blitImage(frame->image, vk::ImageLayout::eTransferSrcOptimal, swapchain_image,
                         vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eLinear);
        // gow3: the settings menu / FPS counter over the frame, at display resolution.
        const bool overlay = Gow3Overlay::Visible();
        const std::array post_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = overlay ? vk::AccessFlagBits::eColorAttachmentRead |
                                               vk::AccessFlagBits::eColorAttachmentWrite
                                         : vk::AccessFlagBits::eNone,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = overlay ? vk::ImageLayout::eColorAttachmentOptimal
                                     : swapchain.FinalLayout(),
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferRead,
                .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = frame->image,
                .subresourceRange = color_range,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eAllCommands,
                               vk::DependencyFlagBits::eByRegion, {}, {}, post_barriers);
        if (overlay) {
            RenderOverlay(cmdbuf, extent);
            const vk::ImageMemoryBarrier to_present{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eNone,
                .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .newLayout = swapchain.FinalLayout(),
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            };
            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                   vk::PipelineStageFlagBits::eBottomOfPipe,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, to_present);
        }
    }
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }

    // Flush vulkan commands.

    SubmitInfo info{};
    if (!swapchain.IsHeadless()) info.AddWait(swapchain.GetImageAcquiredSemaphore());
    info.AddWait(frame->ready_semaphore, frame->ready_tick);
    if (!swapchain.IsHeadless()) info.AddSignal(swapchain.GetPresentReadySemaphore());
    info.AddSignal(frame->present_done);
    scheduler.Flush(info);
    // No WSI acquire/present semaphores exist offscreen. Finish the real GPU
    // submission before image reuse; the frame-ready timeline and present-done
    // fence above remain intact. This mode prioritizes diagnostics over speed.
    if (swapchain.IsHeadless()) scheduler.Finish();

    // Present to swapchain.
    bool presented;
    {
        std::scoped_lock present_lock{PresentLock(instance)};
        presented = swapchain.Present();
    }
    // Recreate waits for device idle under the submit lock. Release the present lock first
    // because the shared-queue path uses that same mutex.
    if (!presented && window.GetWidth() != 0 && window.GetHeight() != 0) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
    }

    free_frame();
    if (!is_reusing_frame && is_game_frame) {
        DebugState.IncFlipFrameNum();
    }
}

Frame* Presenter::GetRenderFrame() {
    // Wait for free presentation frames
    Frame* frame;
    // gow3 20 Oct 2026: the time this waits for presentation (counted; Stall lines).
    Gow3Stats::WaitTimer wait_timer{Gow3Stats::frame_wait_ns};
    {
        std::unique_lock lock{free_mutex};
        free_cv.wait(lock, [this] { return !free_queue.empty(); });
        LOG_DEBUG(Render_Vulkan, "Got render frame, remaining {}", free_queue.size() - 1);

        // Take the frame from the queue
        frame = free_queue.front();
        free_queue.pop();
    }

    const vk::Device device = instance.GetDevice();
    vk::Result result{};

    const auto wait = [&]() {
        result = device.waitForFences(frame->present_done, false, std::numeric_limits<u64>::max());
        return result;
    };

    // Wait for the presentation to be finished so all frame resources are free
    while (wait() != vk::Result::eSuccess) {
        ASSERT_MSG(result != vk::Result::eErrorDeviceLost,
                   "Device lost during waiting for a frame");
        // Retry if the waiting times out
        if (result == vk::Result::eTimeout) {
            continue;
        }
    }

    if (frame->width != expected_frame_width || frame->height != expected_frame_height ||
        frame->is_hdr != swapchain.GetHDR()) {
        RecreateFrame(frame, expected_frame_width, expected_frame_height);
    }

    return frame;
}

void Presenter::SetExpectedGameSize(s32 width, s32 height) {
    if (width <= 0 || height <= 0) {
        return; // no surface (minimized window): keep the last frame size
    }
    const float ratio = (float)width / (float)height;

    expected_frame_height = height;
    expected_frame_width = width;
    if (ratio > expected_ratio) {
        expected_frame_width = static_cast<s32>(height * expected_ratio);
    } else {
        expected_frame_height = static_cast<s32>(width / expected_ratio);
    }
}

} // namespace Vulkan
