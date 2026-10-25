// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#ifdef _WIN32
#include <windows.h>
#endif
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "gow3_settings.h"
#include "gow3_toggles.h"
#include "imgui/renderer/imgui_core.h"
#include "sdl_window.h"
#include "video_core/renderer_vulkan/hdr_session.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"

namespace Vulkan {

static constexpr vk::SurfaceFormatKHR SURFACE_FORMAT_HDR = {
    .format = vk::Format::eA2B10G10R10UnormPack32,
    .colorSpace = vk::ColorSpaceKHR::eHdr10St2084EXT,
};

// gow3: the launcher sends the game's own size through the upscaling path at 1:1 when the ini
// has HDR on (GOW3_COMPOSITE_NATIVE, tools/probe_uncharted.py); it cannot see the display. Without
// HDR output that path only costs: the variables it set (GOW3_COMPOSITE_NATIVE_ENV) are removed
// before the renderer reads them (TemporalUpscaler, Gow3Settings::FixedRenderSession), so the
// session is the native one.
static void KeepNativeCompositeOnlyWithHdr(bool hdr_output) {
    if (!HdrSession::DropNativeComposite(std::getenv("GOW3_COMPOSITE_NATIVE"), hdr_output)) {
        return;
    }
    for (const std::string& name :
         HdrSession::NativeCompositeVariables(std::getenv("GOW3_COMPOSITE_NATIVE_ENV"))) {
#ifdef _WIN32
        _putenv_s(name.c_str(), "");
#else
        unsetenv(name.c_str());
#endif
    }
    std::printf("HDR: no HDR output (%s), the game's own size takes the native path\n",
                Gow3Settings::Get().hdr ? "the display does not offer HDR10" : "HDR is off");
    std::fflush(stdout);
}

Swapchain::Swapchain(const Instance& instance_, const Frontend::WindowSDL& window_)
    : instance{instance_}, window{window_} {
    const char* headless_env = std::getenv("GOW3_HEADLESS");
    headless = headless_env && headless_env[0] == '1' && headless_env[1] == '\0';
    if (headless) {
        surface_format = {vk::Format::eR8G8B8A8Unorm, vk::ColorSpaceKHR::eSrgbNonlinear};
        std::printf("GPU: GOW3_HEADLESS=1: real offscreen rendering, no WSI surface or visible output\n");
    } else {
        surface = CreateSurface(instance.GetInstance(), window);
        FindPresentFormat();
        FindPresentMode();
    }
    // Headless sessions have no HDR output either (supports_hdr stays false).
    KeepNativeCompositeOnlyWithHdr(supports_hdr && Gow3Settings::Get().hdr);

    Create(window.GetWidth(), window.GetHeight());
    ImGui::Core::Initialize(instance, window, image_count, surface_format.format);
}

Swapchain::~Swapchain() {
    Destroy();
    if (surface) instance.GetInstance().destroySurfaceKHR(surface);
}

void Swapchain::Create(u32 width_, u32 height_) {
    width = width_;
    height = height_;
    needs_recreation = false;

    Destroy();

    if (headless) {
        ASSERT_MSG(width && height, "Offscreen rendering requires nonzero dimensions");
        extent = {width, height};
        CreateOffscreenImages();
        return;
    }

    SetSurfaceProperties();

    const std::array queue_family_indices = {
        instance.GetGraphicsQueueFamilyIndex(),
        instance.GetPresentQueueFamilyIndex(),
    };

    const bool exclusive = queue_family_indices[0] == queue_family_indices[1];
    const u32 queue_family_indices_count = exclusive ? 1u : 2u;
    const vk::SharingMode sharing_mode =
        exclusive ? vk::SharingMode::eExclusive : vk::SharingMode::eConcurrent;
    const auto format = needs_hdr ? SURFACE_FORMAT_HDR : surface_format;
    const vk::SwapchainCreateInfoKHR swapchain_info = {
        .surface = surface,
        .minImageCount = image_count,
        .imageFormat = format.format,
        .imageColorSpace = format.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = vk::ImageUsageFlagBits::eColorAttachment |
                      vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
        .imageSharingMode = sharing_mode,
        .queueFamilyIndexCount = queue_family_indices_count,
        .pQueueFamilyIndices = queue_family_indices.data(),
        .preTransform = transform,
        .compositeAlpha = composite_alpha,
        .presentMode = present_mode,
        .clipped = true,
        .oldSwapchain = nullptr,
    };

    auto [swapchain_result, chain] = instance.GetDevice().createSwapchainKHR(swapchain_info);
    ASSERT_MSG(swapchain_result == vk::Result::eSuccess, "Failed to create swapchain: {}",
               vk::to_string(swapchain_result));
    swapchain = chain;

    SetupImages();
    RefreshSemaphores();
}

void Swapchain::Recreate(u32 width_, u32 height_) {
    LOG_DEBUG(Render_Vulkan, "Recreate the swapchain: width={} height={} HDR={}", width_, height_,
              needs_hdr);
    // gow3: how often (a capture hook such as OBS's sets itself up per swapchain).
    static u32 recreations = 0;
    static auto last = std::chrono::steady_clock::now();
    ++recreations;
    if (const auto now = std::chrono::steady_clock::now(); now - last >= std::chrono::seconds(5)) {
        std::printf("Swapchain: %u recreations (5 s), now %ux%u (was %ux%u)\n", recreations, width_,
                    height_, width, height);
        std::fflush(stdout);
        recreations = 0;
        last = now;
    }
    Create(width_, height_);
}

void Swapchain::SetHDR(bool hdr) {
    if (headless) return; // Diagnostic target is SDR RGBA8.
    if (needs_hdr == hdr) {
        return;
    }

    needs_hdr = hdr;
    // Recreate's Destroy waits for idle while excluding concurrent queue submissions.
    Recreate(width, height);
    ImGui::Core::OnSurfaceFormatChange(needs_hdr ? SURFACE_FORMAT_HDR.format
                                                 : surface_format.format);
}

bool Swapchain::AcquireNextImage() {
    if (headless) return true; // Presenter synchronously finishes each offscreen submission.
    vk::Device device = instance.GetDevice();
    vk::Result result;
    {
        Gow3Stats::WaitTimer timer{Gow3Stats::acquire_ns};
        result = device.acquireNextImageKHR(swapchain, std::numeric_limits<u64>::max(),
                                            image_acquired[frame_index], VK_NULL_HANDLE,
                                            &image_index);
    }

    switch (result) {
    case vk::Result::eSuccess:
        break;
    case vk::Result::eSuboptimalKHR:
    case vk::Result::eErrorSurfaceLostKHR:
    case vk::Result::eErrorOutOfDateKHR:
    case vk::Result::eErrorUnknown:
        needs_recreation = true;
        break;
    default:
        LOG_CRITICAL(Render_Vulkan, "Swapchain acquire returned unknown result {}",
                     vk::to_string(result));
        UNREACHABLE();
        break;
    }

    return !needs_recreation;
}

bool Swapchain::Present() {
    if (headless) {
        image_index = (image_index + 1) % image_count;
        frame_index = image_index;
        return true;
    }
    const vk::PresentInfoKHR present_info = {
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &present_ready[image_index],
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = &image_index,
    };

    vk::Result result;
    {
        Gow3Stats::WaitTimer timer{Gow3Stats::present_ns};
        result = instance.GetPresentQueue().presentKHR(present_info);
    }
    if (result == vk::Result::eErrorOutOfDateKHR || result == vk::Result::eSuboptimalKHR) {
        needs_recreation = true;
    } else {
        ASSERT_MSG(result == vk::Result::eSuccess, "Swapchain presentation failed: {}",
                   vk::to_string(result));
    }

    frame_index = (frame_index + 1) % image_count;

    return !needs_recreation;
}

void Swapchain::FindPresentFormat() {
    const auto [formats_result, formats] =
        instance.GetPhysicalDevice().getSurfaceFormatsKHR(surface);
    ASSERT_MSG(formats_result == vk::Result::eSuccess, "Failed to query surface formats: {}",
               vk::to_string(formats_result));

    // Check if the device supports HDR formats. Here we care of Rec.2020 PQ only as it is expected
    // game output. Other variants as e.g. linear Rec.2020 will require additional color space
    // rotation
    supports_hdr =
        std::find_if(formats.begin(), formats.end(), [](const vk::SurfaceFormatKHR& format) {
            return format == SURFACE_FORMAT_HDR;
        }) != formats.end();
    // Also make sure that user allowed us to use HDR
    supports_hdr &= EmulatorSettings.IsHdrAllowed();

    // If there is a single undefined surface format, the device doesn't care, so we'll just use
    // RGBA sRGB.
    if (formats[0].format == vk::Format::eUndefined) {
        surface_format.format = vk::Format::eR8G8B8A8Unorm;
        surface_format.colorSpace = vk::ColorSpaceKHR::eSrgbNonlinear;
        return;
    }

    // Try to find a suitable format: sRGB nonlinear first (HdrSession::PickSdrFormat).
    if (const auto* sformat = HdrSession::PickSdrFormat<vk::SurfaceFormatKHR>(
            formats, {vk::Format::eR8G8B8A8Unorm, vk::Format::eB8G8R8A8Unorm},
            vk::ColorSpaceKHR::eSrgbNonlinear)) {
        surface_format = *sformat;
        return;
    }

    UNREACHABLE_MSG("Unable to find required swapchain format!");
}

void Swapchain::FindPresentMode() {
    const auto [modes_result, modes] =
        instance.GetPhysicalDevice().getSurfacePresentModesKHR(surface);
    if (modes_result != vk::Result::eSuccess) {
        LOG_ERROR(Render, "Failed to query available present modes, falling back to Fifo as "
                          "guaranteed supported option.");
        present_mode = vk::PresentModeKHR::eFifo;
        return;
    }

    // gow3: VSync on is Fifo (no tearing, waits for the display); off is Mailbox when the
    // driver has it, else Immediate (tearing allowed), else Fifo.
    vsync = Gow3Settings::Get().vsync;
    const auto has = [&](vk::PresentModeKHR mode) { return std::ranges::find(modes, mode) != modes.cend(); };
    present_mode = vsync ? vk::PresentModeKHR::eFifo
                   : has(vk::PresentModeKHR::eMailbox) ? vk::PresentModeKHR::eMailbox
                   : has(vk::PresentModeKHR::eImmediate) ? vk::PresentModeKHR::eImmediate
                                                         : vk::PresentModeKHR::eFifo;
    std::printf("Swapchain: VSync %s (%s)\n", vsync ? "on" : "off", vk::to_string(present_mode).c_str());
}

void Swapchain::UpdateVsync() {
    if (headless) return;
    if (Gow3Settings::Get().vsync == vsync) {
        return;
    }
    FindPresentMode();
    Recreate(width, height);
}

void Swapchain::SetSurfaceProperties() {
    const auto [capabilities_result, capabilities] =
        instance.GetPhysicalDevice().getSurfaceCapabilitiesKHR(surface);
    if (capabilities_result != vk::Result::eSuccess) {
        // Capture the actual WSI failure boundary before aborting. This is
        // diagnostic only: no retry, window mutation, or fallback capability.
        const auto [support_result, supported] = instance.GetPhysicalDevice().getSurfaceSupportKHR(
            instance.GetPresentQueueFamilyIndex(), surface);
        std::fprintf(stderr, "WSI failure: capabilities=%s present_support=%s/%u queue=%u\n",
                     vk::to_string(capabilities_result).c_str(),
                     vk::to_string(support_result).c_str(), static_cast<unsigned>(supported),
                     instance.GetPresentQueueFamilyIndex());
#ifdef _WIN32
        const auto hwnd = static_cast<HWND>(window.GetWindowInfo().render_surface);
        DWORD owner_pid = 0;
        const DWORD owner_tid = GetWindowThreadProcessId(hwnd, &owner_pid);
        RECT client{}, outer{};
        const BOOL client_ok = GetClientRect(hwnd, &client);
        const BOOL outer_ok = GetWindowRect(hwnd, &outer);
        const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
        MONITORINFO monitor_info{};
        monitor_info.cbSize = sizeof(monitor_info);
        const BOOL monitor_ok = GetMonitorInfoW(monitor, &monitor_info);
        STARTUPINFOW startup_info{};
        startup_info.cb = sizeof(startup_info);
        GetStartupInfoW(&startup_info);
        std::fprintf(stderr,
                     "WSI Win32: hwnd=%p valid=%d visible=%d iconic=%d owner=%lu/%lu caller=%lu/%lu "
                     "client=%d:%ld,%ld,%ld,%ld outer=%d:%ld,%ld,%ld,%ld "
                     "monitor=%p valid=%d remote=%d style=%llx exstyle=%llx startup_flags=%lx startup_show=%u\n",
                     static_cast<void*>(hwnd), IsWindow(hwnd), IsWindowVisible(hwnd), IsIconic(hwnd),
                     owner_pid, owner_tid, GetCurrentProcessId(), GetCurrentThreadId(),
                     client_ok, client.left, client.top, client.right, client.bottom,
                     outer_ok, outer.left, outer.top, outer.right, outer.bottom,
                     static_cast<void*>(monitor), monitor_ok, GetSystemMetrics(SM_REMOTESESSION),
                     static_cast<unsigned long long>(GetWindowLongPtrW(hwnd, GWL_STYLE)),
                     static_cast<unsigned long long>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)),
                     startup_info.dwFlags, static_cast<unsigned>(startup_info.wShowWindow));
#endif
        std::fflush(stderr);
    }
    ASSERT_MSG(capabilities_result == vk::Result::eSuccess,
               "Failed to query surface capabilities: {}", vk::to_string(capabilities_result));

    extent = capabilities.currentExtent;
    if (capabilities.currentExtent.width == std::numeric_limits<u32>::max()) {
        extent.width = std::max(capabilities.minImageExtent.width,
                                std::min(capabilities.maxImageExtent.width, width));
        extent.height = std::max(capabilities.minImageExtent.height,
                                 std::min(capabilities.maxImageExtent.height, height));
    }

    // Select number of images in swap chain, we prefer one buffer in the background to work on
    image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0) {
        image_count = std::min(image_count, capabilities.maxImageCount);
    }

    // Prefer identity transform if possible
    transform = vk::SurfaceTransformFlagBitsKHR::eIdentity;
    if (!(capabilities.supportedTransforms & transform)) {
        transform = capabilities.currentTransform;
    }

    // Opaque is not supported everywhere.
    composite_alpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
    if (!(capabilities.supportedCompositeAlpha & vk::CompositeAlphaFlagBitsKHR::eOpaque)) {
        composite_alpha = vk::CompositeAlphaFlagBitsKHR::eInherit;
    }
}

void Swapchain::Destroy() {
    vk::Device device = instance.GetDevice();
    // vkDeviceWaitIdle requires host synchronization with queue submissions. The game
    // scheduler can submit on another thread while the presentation thread resizes.
    std::scoped_lock submit_lock{Scheduler::submit_mutex};
    const auto wait_result = device.waitIdle();
    if (wait_result != vk::Result::eSuccess) {
        LOG_WARNING(Render_Vulkan, "Failed to wait for device to become idle: {}",
                    vk::to_string(wait_result));
    }

    for (auto& image_view : images_view) {
        device.destroyImageView(image_view);
    }
    images_view.clear();

    for (size_t i = 0; i < offscreen_allocations.size(); ++i) {
        vmaDestroyImage(instance.GetAllocator(), images[i], offscreen_allocations[i]);
    }
    offscreen_allocations.clear();
    images.clear();

    if (swapchain) {
        device.destroySwapchainKHR(swapchain);
        swapchain = nullptr;
    }

    for (const auto& sem : image_acquired) {
        device.destroySemaphore(sem);
    }
    for (const auto& sem : present_ready) {
        device.destroySemaphore(sem);
    }

    image_acquired.clear();
    present_ready.clear();
}

void Swapchain::CreateOffscreenImages() {
    const auto device = instance.GetDevice();
    image_count = 3;
    image_index = frame_index = 0;
    images.resize(image_count);
    images_view.resize(image_count);
    offscreen_allocations.resize(image_count);
    const VkImageCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    const VmaAllocationCreateInfo allocation_info{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    for (u32 i = 0; i < image_count; ++i) {
        VkImage image{};
        const auto result = vmaCreateImage(instance.GetAllocator(), &ci, &allocation_info,
                                           &image, &offscreen_allocations[i], nullptr);
        ASSERT_MSG(result == VK_SUCCESS, "Failed to allocate offscreen image: {}", int(result));
        images[i] = image;
        const auto [view_result, view] = device.createImageView(vk::ImageViewCreateInfo{
            .image = images[i],
            .viewType = vk::ImageViewType::e2D,
            .format = surface_format.format,
            .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                 .levelCount = 1, .layerCount = 1},
        });
        ASSERT_MSG(view_result == vk::Result::eSuccess, "Failed to create offscreen view: {}",
                   vk::to_string(view_result));
        images_view[i] = view;
        SetObjectName(device, images[i], "Headless diagnostic image {}", i);
    }
}

void Swapchain::RefreshSemaphores() {
    const vk::Device device = instance.GetDevice();
    image_acquired.resize(image_count);
    present_ready.resize(image_count);

    for (vk::Semaphore& semaphore : image_acquired) {
        auto [semaphore_result, sem] = device.createSemaphore({});
        ASSERT_MSG(semaphore_result == vk::Result::eSuccess,
                   "Failed to create image acquired semaphore: {}",
                   vk::to_string(semaphore_result));
        semaphore = sem;
    }
    for (vk::Semaphore& semaphore : present_ready) {
        auto [semaphore_result, sem] = device.createSemaphore({});
        ASSERT_MSG(semaphore_result == vk::Result::eSuccess,
                   "Failed to create present ready semaphore: {}", vk::to_string(semaphore_result));
        semaphore = sem;
    }

    for (u32 i = 0; i < image_count; ++i) {
        SetObjectName(device, image_acquired[i], "Swapchain Semaphore: image_acquired {}", i);
        SetObjectName(device, present_ready[i], "Swapchain Semaphore: present_ready {}", i);
    }
}

void Swapchain::SetupImages() {
    vk::Device device = instance.GetDevice();
    auto [images_result, imgs] = device.getSwapchainImagesKHR(swapchain);
    ASSERT_MSG(images_result == vk::Result::eSuccess, "Failed to create swapchain images: {}",
               vk::to_string(images_result));
    images = std::move(imgs);
    image_count = static_cast<u32>(images.size());
    images_view.resize(image_count);
    for (u32 i = 0; i < image_count; ++i) {
        if (images_view[i]) {
            device.destroyImageView(images_view[i]);
        }
        auto [im_view_result, im_view] = device.createImageView(vk::ImageViewCreateInfo{
            .image = images[i],
            .viewType = vk::ImageViewType::e2D,
            .format = needs_hdr ? SURFACE_FORMAT_HDR.format : surface_format.format,
            .subresourceRange =
                {
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .levelCount = 1,
                    .layerCount = 1,
                },
        });
        ASSERT_MSG(im_view_result == vk::Result::eSuccess, "Failed to create image view: {}",
                   vk::to_string(im_view_result));
        images_view[i] = im_view;
    }

    for (u32 i = 0; i < image_count; ++i) {
        SetObjectName(device, images[i], "Swapchain Image {}", i);
        SetObjectName(device, images_view[i], "Swapchain ImageView {}", i);
    }
}

} // namespace Vulkan
