// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/motion_history.h"
#include "video_core/renderer_vulkan/vk_scene_resolution.h"
#include "video_core/renderer_vulkan/ui_composition.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <tuple>

#include <vk_mem_alloc.h>

#include "gow3_settings.h"
#include "gow3_toggles.h"
#include "ffx_vk_portable.h"
#include "video_core/host_shaders/hdr_decode_comp.h"
#include "video_core/host_shaders/hdr_encode_comp.h"
#include "video_core/host_shaders/hdr_from_linear_comp.h"
#include "video_core/host_shaders/upscale_merge_comp.h"
#include "video_core/host_shaders/upscale_reactive_comp.h"
#include "video_core/host_shaders/taa_comp.h"
#include "video_core/host_shaders/taa_sharpen_comp.h"
#include "video_core/host_shaders/taa_sharpen_ldr_comp.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vk_camera_motion.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

namespace {

FfxVkPortableImage Describe(vk::Image image, vk::Format format, u32 width, u32 height,
                            vk::ImageUsageFlags usage, vk::ImageAspectFlags aspect,
                            FfxVkPortableResourceState state) {
    FfxVkPortableImage out{};
    out.structSize = sizeof(out);
    out.image = image;
    out.format = static_cast<VkFormat>(format);
    out.extent = {width, height};
    out.mipCount = 1;
    out.arrayLayers = 1;
    out.usage = static_cast<VkImageUsageFlags>(usage);
    out.aspect = static_cast<VkImageAspectFlags>(aspect);
    out.state = state;
    return out;
}

/// gow3: GOW3_DUMP_TRIGGER=<file> GOW3_DUMP_DIR=<dir> (default out/dump): creating the file dumps
/// the upscaler's images of the next GOW3_DUMP_FRAMES (8) frames as raw files
/// <dir>/fNNN_<name>_<w>x<h>_<format>.raw, for checking temporal stability offline.
/// Returns the frame number to dump, or -1.
int DumpFrame() {
    static const char* trigger = std::getenv("GOW3_DUMP_TRIGGER");
    static int remaining = 0, index = 0, polls = 0;
    if (!trigger) {
        return -1;
    }
    if (remaining == 0) {
        if (++polls % 30 != 0 || std::remove(trigger) != 0) {
            return -1;
        }
        const char* frames = std::getenv("GOW3_DUMP_FRAMES");
        remaining = frames ? std::max(1, std::atoi(frames)) : 8;
        index = 0;
    }
    --remaining;
    return index++;
}

struct DumpImage {
    vk::Image image; ///< in layout General
    u32 width, height, bytes_per_pixel;
    const char* name;
    const char* format;
    vk::ImageAspectFlagBits aspect = vk::ImageAspectFlagBits::eColor;
};

/// Copies `images` into host buffers after the commands recorded so far and writes them to
/// files once the GPU is done.
void DumpImages(const Instance& instance, Scheduler& scheduler, vk::CommandBuffer cmdbuf,
                int frame, std::initializer_list<DumpImage> images) {
    static const std::string dir = [] {
        const char* env = std::getenv("GOW3_DUMP_DIR");
        return std::string{env && env[0] ? env : "out/dump"};
    }();
    const vk::MemoryBarrier2 before{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &before});
    for (const auto& image : images) {
        const VkDeviceSize size = VkDeviceSize(image.width) * image.height * image.bytes_per_pixel;
        const VkBufferCreateInfo buffer_ci{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        };
        const VmaAllocationCreateInfo alloc_ci{
            .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                     VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        };
        VkBuffer buffer{};
        VmaAllocation allocation{};
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &buffer, &allocation,
                            &info) != VK_SUCCESS) {
            std::printf("Dump: no host memory for %s\n", image.name);
            continue;
        }
        const vk::BufferImageCopy region{
            .imageSubresource = {image.aspect, 0, 0, 1},
            .imageExtent = {image.width, image.height, 1},
        };
        cmdbuf.copyImageToBuffer(image.image, vk::ImageLayout::eGeneral, buffer, region);
        char path[512];
        std::snprintf(path, sizeof(path), "%s/f%03d_%s_%ux%u_%s.raw", dir.c_str(), frame,
                      image.name, image.width, image.height, image.format);
        scheduler.DeferPriorityOperation(
            [allocator = instance.GetAllocator(), buffer, allocation, info, size,
             file = std::string{path}] {
                vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
                if (FILE* f = std::fopen(file.c_str(), "wb")) {
                    std::fwrite(info.pMappedData, 1, size, f);
                    std::fclose(f);
                }
                vmaDestroyBuffer(allocator, buffer, allocation);
            });
    }
    const vk::MemoryBarrier2 after{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eNone,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eNone,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after});
    std::printf("Dump: frame %d -> %s\n", frame, dir.c_str());
}

void PrintIssues(const char* what, u64 issues) {
    std::printf("Upscaler: %s invalid:", what);
    for (u32 bit = 0; bit < 64; ++bit) {
        if (issues & (1ull << bit)) {
            std::printf(" %s", ffxVkPortableValidationIssueName(1ull << bit));
        }
    }
    std::printf("\n");
}

} // namespace

TemporalUpscaler::TemporalUpscaler(const Instance& instance_, Scheduler& scheduler_,
                                   VideoCore::TextureCache& texture_cache_, Runtime& runtime_,
                                   CameraMotion& camera_motion_, SceneTargets& scene_targets_)
    : instance{instance_}, scheduler{scheduler_}, texture_cache{texture_cache_},
      runtime{runtime_}, camera_motion{camera_motion_}, scene_targets{scene_targets_} {
    // Reject unsupported shaders before allocating resources or recording a frame.
    Gow3Settings::ConfigureUpscalerSupport(
        instance.IsFsr4Int8Supported() && Fsr4Upscaler::AssetsPresent(),
        instance.IsFsr411Supported());
    fsr4 = std::make_unique<Fsr4Upscaler>(instance, scheduler);
    {
        const Dlss* dlss = Dlss::Get();
        static std::string problem;
        problem = !dlss ? "not installed (DLSS files missing)"
                        : dlss->Problem();
        Gow3Settings::ConfigureDlssSupport(dlss && dlss->Available(), problem.c_str());
    }
    // Available unless GOW3_UPSCALER=none; on/off and the parameters are the menu's settings.
    const char* env = std::getenv("GOW3_UPSCALER");
    enabled = !(env && std::strcmp(env, "none") == 0);
    if (const char* hash = std::getenv("GOW3_UPSCALE_BEFORE_CS")) {
        trigger_hash = std::strtoull(hash, nullptr, 16);
    }
    if (const char* hash = std::getenv("GOW3_UI_TRIGGER_VS")) {
        ui_trigger_vs = std::strtoull(hash, nullptr, 16);
    }
    if (const char* res = std::getenv("GOW3_OUTPUT_RES")) {
        u32 w = 0, h = 0;
        if (std::sscanf(res, "%ux%u", &w, &h) == 2 && w && h) {
            target_width = w;
            target_height = h;
        }
    }
    // Explicit resolution overrides retain the old compatibility path. Normal presets
    // keep guest allocations/UI native and change only host raster targets at frame boundaries.
    scaled_session = std::getenv("GOW3_RENDER_RES") && std::getenv("GOW3_RENDER_RES")[0];
    render_width = 1920;
    render_height = 1080;
    if (const char* res = std::getenv("GOW3_RENDER_RES")) {
        u32 w = 0, h = 0;
        if (std::sscanf(res, "%ux%u", &w, &h) == 2 && w && h) {
            render_width = w;
            render_height = h;
        }
    }
    // Scene depth copied into the output-size UI depth by a blit (depth aspect).
    const auto features = instance.GetPhysicalDevice()
                              .getFormatProperties(vk::Format::eD32SfloatS8Uint)
                              .optimalTilingFeatures;
    depth_blit = (features & vk::FormatFeatureFlagBits::eBlitSrc) &&
                 (features & vk::FormatFeatureFlagBits::eBlitDst);
    if (enabled && !instance.IsStorageImageWriteWithoutFormatEnabled()) {
        std::printf("Upscaler: shaderStorageImageWriteWithoutFormat unsupported, FSR 3 off\n");
        enabled = false;
    }
    Gow3Settings::Get().temporal_aa_available = enabled && trigger_hash != 0;
    // Uncharted below the output size (see OnComposite).
    if (const char* ps = std::getenv("GOW3_UI_COMPOSITE_PS")) {
        composite_ps = u32(std::strtoull(ps, nullptr, 16));
    }
    if (const char* ps = std::getenv("GOW3_UI_TONEMAP_PS")) {
        tonemap_ps = u32(std::strtoull(ps, nullptr, 16));
    }
    composite_frames = enabled && scaled_session && composite_ps && camera_motion.VelocityMode() &&
                       (render_width < target_width || render_height < target_height);
    if (composite_frames) {
        std::printf("Upscaler: the game draws %ux%u, frames are upscaled to %ux%u at pixel shader "
                    "%08llx; HUD and menus at %ux%u\n", render_width, render_height, target_width,
                    target_height, static_cast<unsigned long long>(composite_ps), target_width,
                    target_height);
    }
    if (enabled) {
        std::printf("Upscaler: FSR 3.1 available (%s) on scene color before compute shader "
                    "%016llx\n",
                    Gow3Settings::Get().upscaler != Gow3Settings::UpscalerOff ? "on" : "off",
                    static_cast<unsigned long long>(trigger_hash));
    }
}

TemporalUpscaler::~TemporalUpscaler() {
    if (resources_ready) scheduler.Finish();
    if (context) {
        scheduler.Finish();
        ffxVkPortableUpscaleContextDestroy(context);
    }
}

bool TemporalUpscaler::Active() const {
    // Toggle 1 << 24 switches it off at run time (A/B); history restarts after.
    return enabled && !failed &&
           (Gow3Settings::Get().upscaler == Gow3Settings::UpscalerFsr3 ||
            Gow3Settings::IsFsr4(Gow3Settings::Get().upscaler) ||
            Gow3Settings::Get().upscaler == Gow3Settings::UpscalerTaa ||
            Gow3Settings::Get().upscaler == Gow3Settings::UpscalerDlss) &&
           !Gow3Toggle::Disabled(1u << 24);
}

bool TemporalUpscaler::ReactiveOn() const {
    // FSR 4 takes no reactive mask: the opaque snapshot and the mask pass would be wasted.
    return Gow3Settings::Get().reactive && !Gow3Toggle::Disabled(1u << 27) && !UseFsr4() && !UseDlss() &&
           Gow3Settings::Get().upscaler != Gow3Settings::UpscalerTaa;
}

void TemporalUpscaler::OnSceneColor(VideoCore::ImageId color) {
    scene_color = color;
}

namespace {
/// The upscalers read 8-bit frames as the stored (sRGB-encoded) values: UNORM views of sRGB
/// images, as the game's own RGBA8 copy holds them.
vk::Format LdrFormat(vk::Format format) {
    switch (format) {
    case vk::Format::eR8G8B8A8Srgb:
        return vk::Format::eR8G8B8A8Unorm;
    case vk::Format::eB8G8R8A8Srgb:
        return vk::Format::eB8G8R8A8Unorm;
    default:
        return format;
    }
}

float Halton(u32 index, u32 base) {
    float f = 1.0f, result = 0.0f;
    for (u32 i = index; i > 0; i /= base) {
        f /= float(base);
        result += f * float(i % base);
    }
    return result;
}
} // namespace

bool TemporalUpscaler::ReducedScene(const VideoCore::Image& color) const {
    return !scaled_session && scene_targets.Reduced() && scene_targets.EligibleScene(color);
}

bool TemporalUpscaler::RasterScaling() const {
    return !scaled_session && scene_targets.Reduced() && !done_this_frame;
}

bool TemporalUpscaler::OnFrameStart() {
    // gow3: GOW3_PRESET_FILE=<file> holding a preset number, read about once a second: switches
    // the preset like the menu does (scripted tests of live preset changes).
    static const char* preset_file = std::getenv("GOW3_PRESET_FILE");
    if (preset_file && ++preset_file_frames % 64 == 0) {
        if (FILE* f = std::fopen(preset_file, "r")) {
            int value = 0;
            int output = -1;
            int provider = -1;
            const int fields = std::fscanf(f, "%d %d %d", &value, &output, &provider);
            if (fields >= 1 && value >= 0 &&
                value < Gow3Settings::PresetCount) {
                Gow3Settings::Get().preset.store(value);
            }
            if (fields >= 2 && output >= 0 && output < Gow3Settings::OutputCount) {
                Gow3Settings::Get().output_res.store(output);
            }
            if (fields == 3 && provider >= 0 && provider < Gow3Settings::UpscalerCount) {
                Gow3Settings::Get().upscaler.store(provider);
            }
            std::fclose(f);
        }
    }
    const auto& settings = Gow3Settings::Get();
    // The guest's startup resolution patch remains in effect until restart. Keep the FSR
    // model at the applied preset while the menu saves the requested one for run.sh.
    // Velocity mode (Uncharted) runs at the game's own (patched) size: anti-aliasing (Native AA)
    // only, as in the reference port; below the output size the preset (FSR 4's model) follows
    // the scale: 1.5 Quality, 1.7 Balanced, 2 Performance, more Ultra Performance.
    // By the height: ultrawide widths are rounded to steps of 8.
    const float scale = float(target_height) / float(std::max(1u, render_height));
    const int preset = composite_frames ? (scale <= 1.01f ? 0 : scale <= 1.51f ? 1 : scale <= 1.71f ? 2
                                                          : scale <= 2.01f ? 3 : 4)
                       : camera_motion.VelocityMode() ? 0 : Gow3Settings::RenderPreset();
    if (applied_preset != preset || settings.upscaler == Gow3Settings::UpscalerOff) failed = false;
    const bool active = Active();
    const bool jitter_on = active && settings.jitter && !Gow3Toggle::Disabled(1u << 25);
    const int upscaler = settings.upscaler.load();
    // Velocity mode keeps the native output (Scaled() false); the game's patched render size
    // (1440p, ultrawide) reaches the native path through the images' own sizes.
    const int output =
        camera_motion.VelocityMode() ? Gow3Settings::OutputDefault : settings.output_res.load();
    const bool output_changed = !scaled_session && applied_output != output;
    if (output_changed) {
        target_width = Gow3Settings::OutputWidths[output];
        target_height = Gow3Settings::OutputHeights[output];
        std::printf("Output resolution: %ux%u (live)\n", target_width, target_height);
        failed = false;
        fsr4_failed = false;
    }
    const bool changed = output_changed || applied_preset != preset || active != last_active ||
                         jitter_on != last_jitter || applied_upscaler != upscaler;
    if (applied_upscaler != upscaler) {
        // A failed provider keeps a fatal flag internally; a user retry gets a fresh context.
        scheduler.Finish();
        fsr4 = std::make_unique<Fsr4Upscaler>(instance, scheduler);
        if (Gow3Settings::IsFsr4(upscaler)) Gow3Settings::Get().fsr4_problem = nullptr;
    }
    if (applied_upscaler != upscaler) fsr4_failed = false; // retry after a menu change
    // Dynamic scene resolution scaling (live preset switching) works on all GPUs.
    // On GPUs without D32S8 blit support, UI depth is cleared instead of copied from scene.
    if (!scaled_session) {
        scene_targets.SetSize(SceneResolution::ForPreset(active ? preset : 0,
                                                        {target_width, target_height}));
        render_width = scene_targets.Size().width;
        render_height = scene_targets.Size().height;
    }
    Gow3Settings::Get().active_render_width = Scaled() ? render_width : scene_targets.Size().width;
    Gow3Settings::Get().active_render_height = Scaled() ? render_height : scene_targets.Size().height;
    if (changed || !dispatched_last_frame) reset = true;
    if (changed) jitter_index = 0;
    applied_preset = preset;
    applied_output = output;
    applied_upscaler = upscaler;
    last_active = active;
    last_jitter = jitter_on;
    dispatched_last_frame = false;

    // The display pass of an upscaled frame reads the upscaled UI image. After OnComposite the
    // HUD and menu draws of the frame just upscaled still go to its host display buffer.
    display_redirect = ui_phase || composite_done;
    // gow3: HDR10 output (Stage 2: the HUD and display buffers in RGBA16F), for this frame.
    hdr_chain = composite_frames && Gow3Settings::Get().hdr && Gow3Settings::Get().hdr_supported;
    tone_shadow.drawn = scene_shadow.drawn = false;
    dlss_hdr_output = false;
    composite_done = false;
    tonemap_pending = false;
    ui_phase = false;
    ui_read_barrier = false;
    done_this_frame = false;
    ldr_target = {};
    scene_color = {};
    snapshot_taken = false;
    opaque_valid = false;
    mask_ready = false;
    // Halton(2, 3); the menu or toggle 1 << 25 disables it.
    if (!jitter_on) {
        jitter = {};
        return changed;
    }
    // CameraMotion::OnDisplayPass has already cleared Depth(). Use the context's render
    // size, which survives the frame boundary (Performance 960 -> 1920 needs 32 phases).
    const u32 phases = Scaled() ? Motion::JitterPhases(render_width, target_width)
        : Motion::JitterPhases(scene_targets.Size().width, 1920);
    jitter_index = jitter_index % phases + 1;
    jitter = {Halton(jitter_index, 2) - 0.5f, Halton(jitter_index, 3) - 0.5f};
    return changed;
}

void TemporalUpscaler::OnDispatch(u64 cs_hash) {
    if (cs_hash != trigger_hash || done_this_frame || failed || Scaled()) {
        return;
    }
    done_this_frame = true;
    if (!scene_color || !camera_motion.Ready() || !Active()) {
        // gow3: why the trigger passed without running (first few times; diagnosis).
        static int reported = 0;
        if (camera_motion.VelocityMode() && Active() && reported < 3) {
            ++reported;
            std::printf("Upscaler: not run (scene color %d, depth %d, camera %d)\n",
                        int(bool(scene_color)), int(bool(camera_motion.Depth())),
                        int(camera_motion.Ready()));
        }
        reset = true;
        return;
    }
    Run();
}

bool TemporalUpscaler::EnsureResources(u32 w, u32 h, u32 ow, u32 oh, bool hdr) {
    // DLSS, like FSR 4, has its own context: no portable FSR 3 context is made.
    const bool use_fsr4 = UseFsr4() || UseDlss();
    const bool use_taa = Gow3Settings::Get().upscaler == Gow3Settings::UpscalerTaa;
    if (use_taa && (w != ow || h != oh)) {
        std::printf("TAA: remove GOW3_RENDER_RES to use native-resolution TAA\n");
        return false;
    }
    if (resources_ready && w == width && h == height && ow == out_width && oh == out_height &&
        hdr == context_hdr && use_fsr4 == resources_fsr4 && use_taa == resources_taa) {
        return true;
    }
    const auto device = instance.GetDevice();
    // FSR 4 owns no portable FSR 3 context, but its images can still be in flight.
    scheduler.Finish();
    resources_ready = false;
    if (context) {
        ffxVkPortableUpscaleContextDestroy(context);
        context = nullptr;
    }
    width = w;
    height = h;
    out_width = ow;
    out_height = oh;
    context_hdr = hdr;

    FfxVkPortableDeviceInfo device_info{};
    device_info.structSize = sizeof(device_info);
    device_info.instance = instance.GetInstance();
    device_info.physicalDevice = instance.GetPhysicalDevice();
    device_info.device = device;
    device_info.getDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;
    device_info.queue = instance.GetGraphicsQueue();
    device_info.queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex();
    device_info.shaderFloat16Enabled = instance.IsShaderFloat16Enabled();
    device_info.subgroupSizeControlEnabled = instance.IsSubgroupSizeControlEnabled();
    device_info.synchronization2Enabled = VK_TRUE;
    device_info.shaderStorageImageWriteWithoutFormatEnabled = VK_TRUE;

    FfxVkPortableUpscaleCreateInfo create_info{};
    create_info.structSize = sizeof(create_info);
    // Native AA runs on the HDR scene color; scaled presets on the game's tonemapped frame.
    create_info.flags = hdr ? FFX_VK_PORTABLE_CONTEXT_HDR_COLOR_INPUT |
                                  FFX_VK_PORTABLE_CONTEXT_AUTO_EXPOSURE
                            : 0;
    create_info.maxRenderSize = {w, h};
    create_info.maxOutputSize = {ow, oh};
    // FSR 4 has its own model context (vk_fsr4); the images below are shared.
    if (!use_fsr4 && !use_taa) {
        if (const u64 issues = ffxVkPortableValidateUpscaleCreateInfo(&create_info)) {
            PrintIssues("create info", issues);
            return false;
        }
        if (ffxVkPortableUpscaleContextCreate(&device_info, &create_info, &context) !=
            FFX_VK_PORTABLE_OK) {
            std::printf("Upscaler: FSR 3 context creation failed\n");
            context = nullptr;
            return false;
        }
    }

    const auto allocator = instance.GetAllocator();
    motion_view.reset();
    output_view.reset();
    motion_image = VideoCore::UniqueImage(device, allocator);
    motion_image.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR16G16Sfloat,
        .extent = {w, h, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    output_image = VideoCore::UniqueImage(device, allocator);
    output_image.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .extent = {ow, oh, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                 vk::ImageUsageFlagBits::eTransferSrc,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    output_view = Check(device.createImageViewUnique({
        .image = vk::Image(output_image),
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    motion_view = Check(device.createImageViewUnique({
        .image = vk::Image(motion_image),
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16Sfloat,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    const auto make_image = [&](VideoCore::UniqueImage& image, vk::UniqueImageView& view,
                                vk::Format format, vk::ImageUsageFlags usage, u32 iw = 0,
                                u32 ih = 0, vk::ImageAspectFlags aspect =
                                                vk::ImageAspectFlagBits::eColor,
                                vk::ImageCreateFlags flags = {}) {
        view.reset();
        image = VideoCore::UniqueImage(device, allocator);
        image.Create(vk::ImageCreateInfo{
            .flags = flags,
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent = {iw ? iw : w, ih ? ih : h, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = usage,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        view = Check(device.createImageViewUnique({
            .image = vk::Image(image),
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = {aspect, 0, 1, 0, 1},
        }));
    };
    make_image(opaque_image, opaque_view, vk::Format::eR16G16B16A16Sfloat,
               vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferDst |
                   vk::ImageUsageFlagBits::eTransferSrc);
    make_image(reactive_image, reactive_view, vk::Format::eR8Unorm,
               vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled);
    for (u32 i = 0; i < taa_history.size(); ++i) {
        taa_history_views[i].reset();
        taa_history[i] = VideoCore::UniqueImage{};
        if (use_taa) {
            make_image(taa_history[i], taa_history_views[i], vk::Format::eR32G32B32A32Sfloat,
                       vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                           vk::ImageUsageFlagBits::eTransferSrc,
                       ow, oh);
        }
    }
    taa_next = 0;
    CreatePipelines();
    opaque_valid = false;
    reset = true;
    resources_ready = true;
    resources_fsr4 = use_fsr4;
    resources_taa = use_taa;
    if (use_taa) {
        std::printf("TAA: context %ux%u -> %ux%u (%s), no FSR model\n", w, h, ow, oh,
                    hdr ? "HDR scene color" : "tonemapped frame");
        return true;
    }
    if (use_fsr4) {
        std::printf("Upscaler: FSR 4 inputs %ux%u -> %ux%u\n", w, h, ow, oh);
        return true;
    }
    FfxVkPortableMemoryUsage usage{};
    usage.structSize = sizeof(usage);
    ffxVkPortableUpscaleContextGetMemoryUsage(context, &usage);
    std::printf("Upscaler: FSR 3 context %ux%u -> %ux%u (%s), %.1f MB\n", w, h, ow, oh,
                hdr ? "HDR scene color" : "tonemapped frame", usage.totalUsageInBytes / 1e6);
    return true;
}

void TemporalUpscaler::CreatePipelines() {
    if (merge_pipeline) {
        return;
    }
    const auto device = instance.GetDevice();
    const auto storage_layout = [&](u32 count, vk::UniqueDescriptorSetLayout& layout,
                                    vk::UniquePipelineLayout& pipeline_layout, u32 push_size) {
        std::array<vk::DescriptorSetLayoutBinding, 3> bindings{};
        for (u32 i = 0; i < count; ++i) {
            bindings[i] = {.binding = i,
                           .descriptorType = vk::DescriptorType::eStorageImage,
                           .descriptorCount = 1,
                           .stageFlags = vk::ShaderStageFlagBits::eCompute};
        }
        layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = count,
            .pBindings = bindings.data(),
        }));
        const vk::PushConstantRange push{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                         .offset = 0,
                                         .size = push_size};
        pipeline_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push,
        }));
    };
    const auto compute = [&](const auto& code, vk::PipelineLayout layout) {
        const auto module = CompileSPV(code, device);
        auto pipeline = Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = module,
                              .pName = "main"},
                    .layout = layout,
                }));
        device.destroyShaderModule(module);
        return pipeline;
    };
    {
        // Merge: three storage images, then the motion and object motion images (debug).
        std::array<vk::DescriptorSetLayoutBinding, 5> bindings{};
        for (u32 i = 0; i < bindings.size(); ++i) {
            bindings[i] = {.binding = i,
                           .descriptorType = i < 3 ? vk::DescriptorType::eStorageImage
                                                   : vk::DescriptorType::eSampledImage,
                           .descriptorCount = 1,
                           .stageFlags = vk::ShaderStageFlagBits::eCompute};
        }
        merge_desc_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = u32(bindings.size()),
            .pBindings = bindings.data(),
        }));
        const vk::PushConstantRange push{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                         .offset = 0,
                                         .size = sizeof(u32)};
        merge_pipeline_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*merge_desc_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push,
        }));
    }
    merge_pipeline = compute(UPSCALE_MERGE_COMP, *merge_pipeline_layout);
    storage_layout(3, reactive_desc_layout, reactive_pipeline_layout, 3 * sizeof(float));
    reactive_pipeline = compute(UPSCALE_REACTIVE_COMP, *reactive_pipeline_layout);
    storage_layout(2, taa_sharpen_desc_layout, taa_sharpen_pipeline_layout, sizeof(float));
    taa_sharpen_pipeline = compute(TAA_SHARPEN_COMP, *taa_sharpen_pipeline_layout);
    taa_sharpen_ldr_pipeline = compute(TAA_SHARPEN_LDR_COMP, *taa_sharpen_pipeline_layout);
    {
        std::array<vk::DescriptorSetLayoutBinding, 7> bindings{};
        for (u32 i = 0; i < bindings.size(); ++i) {
            bindings[i] = {.binding = i,
                .descriptorType = i < 4 ? vk::DescriptorType::eCombinedImageSampler
                                        : vk::DescriptorType::eStorageImage,
                .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute};
        }
        taa_desc_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = u32(bindings.size()), .pBindings = bindings.data()}));
        const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, 64};
        taa_pipeline_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1, .pSetLayouts = &*taa_desc_layout,
            .pushConstantRangeCount = 1, .pPushConstantRanges = &push}));
        taa_sampler = Check(device.createSamplerUnique({
            .magFilter = vk::Filter::eNearest, .minFilter = vk::Filter::eNearest,
            .mipmapMode = vk::SamplerMipmapMode::eNearest,
            .addressModeU = vk::SamplerAddressMode::eClampToEdge,
            .addressModeV = vk::SamplerAddressMode::eClampToEdge,
            .addressModeW = vk::SamplerAddressMode::eClampToEdge}));
        taa_pipeline = compute(TAA_COMP, *taa_pipeline_layout);
    }
}

void TemporalUpscaler::RecordTaa(vk::CommandBuffer cmdbuf, vk::ImageView color,
                                vk::ImageView depth) {
    // Make scene writes and the last storage-image history writes visible to sampled reads.
    const vk::MemoryBarrier2 inputs{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite |
                         vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead |
                         vk::AccessFlagBits2::eShaderStorageWrite,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1,.pMemoryBarriers = &inputs});
    // Histories alternate between reads and writes. Queue ordering plus this dependency
    // protects both the previous write and a reused destination's previous read.
    std::array<vk::ImageMemoryBarrier2, 2> barriers{};
    for (u32 i = 0; i < barriers.size(); ++i) {
        barriers[i] = {.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite |
                             vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead |
                             vk::AccessFlagBits2::eShaderStorageWrite,
            .oldLayout = reset ? vk::ImageLayout::eUndefined : vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = vk::Image(taa_history[i]),
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
    }
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = u32(barriers.size()),
                             .pImageMemoryBarriers = barriers.data()});
    const std::array<vk::ImageView, 7> views{
        color, depth, *motion_view, *taa_history_views[1 - taa_next],
        *output_view, *taa_history_views[taa_next], *opaque_view};
    std::array<vk::DescriptorImageInfo, 7> infos{};
    std::array<vk::WriteDescriptorSet, 7> writes{};
    for (u32 i = 0; i < infos.size(); ++i) {
        infos[i] = {.sampler = i < 4 ? *taa_sampler : vk::Sampler{},
                    .imageView = views[i], .imageLayout = vk::ImageLayout::eGeneral};
        writes[i] = {.dstBinding = i, .descriptorCount = 1,
            .descriptorType = i < 4 ? vk::DescriptorType::eCombinedImageSampler
                                    : vk::DescriptorType::eStorageImage,
            .pImageInfo = &infos[i]};
    }
    struct Params {
        std::array<float, 2> jitter;
        u32 reset, pad;
        std::array<std::array<float, 4>, 3> depth;
    } params{jitter, reset ? 1u : 0u, std::getenv("GOW3_TAA_DIAGNOSTICS") ?
                 u32(std::clamp(std::atoi(std::getenv("GOW3_TAA_DIAGNOSTICS")),1,3)) : 0u,
             camera_motion.TaaDepthParameters()};
    // Optional techniques for A/B in one run (GOW3_TOGGLE_FILE bits 51-54): tonemapped blending,
    // YCoCg clipping, variance clipping, 3x3 reconstruction. Measured in game on 2026-10-02
    // (static and panning camera, interleaved captures): none improved stability beyond run
    // noise; YCoCg clipping and the reconstruction made it worse. Default: all off.
    params.pad |= (Gow3Toggle::Disabled(Gow3Toggle::TaaTonemapBlend) ? 1u << 8 : 0u) |
                  (Gow3Toggle::Disabled(Gow3Toggle::TaaClip) ? 2u << 8 : 0u) |
                  (Gow3Toggle::Disabled(Gow3Toggle::TaaVariance) ? 4u << 8 : 0u) |
                  (Gow3Toggle::Disabled(Gow3Toggle::TaaFilter) ? 8u << 8 : 0u) |
                  (Gow3Toggle::Disabled(Gow3Toggle::TaaKeepNearerHistory) ? 16u << 8 : 0u);
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *taa_pipeline);
    if (params.pad & 0xffu) {
        static u32 diagnostic_frames = 0;
        if (diagnostic_frames++ < 16)
            std::printf("TAA diagnostic: reset=%u jitter=%.6f,%.6f mode=%u\n",
                        params.reset, jitter[0], jitter[1], params.pad);
    }
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *taa_pipeline_layout, 0, writes);
    cmdbuf.pushConstants(*taa_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                         sizeof(params), &params);
    cmdbuf.dispatch((out_width + 7) / 8, (out_height + 7) / 8, 1);
    const auto& settings = Gow3Settings::Get();
    const float strength = std::clamp(settings.sharpness.load(), 0.0f, 2.0f);
    if (settings.sharpen && strength > 0.0f && !(params.pad & 0xffu)) {
        const vk::MemoryBarrier2 resolved{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead |
                             vk::AccessFlagBits2::eShaderStorageWrite};
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &resolved});
        const std::array<vk::DescriptorImageInfo, 2> sharpen_infos{{
            {.imageView = *taa_history_views[taa_next], .imageLayout = vk::ImageLayout::eGeneral},
            {.imageView = *output_view, .imageLayout = vk::ImageLayout::eGeneral}}};
        std::array<vk::WriteDescriptorSet, 2> sharpen_writes{};
        for (u32 i = 0; i < sharpen_writes.size(); ++i)
            sharpen_writes[i] = {.dstBinding = i, .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &sharpen_infos[i]};
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *taa_sharpen_pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,
                                    *taa_sharpen_pipeline_layout, 0, sharpen_writes);
        cmdbuf.pushConstants(*taa_sharpen_pipeline_layout, vk::ShaderStageFlagBits::eCompute,
                             0, sizeof(strength), &strength);
        cmdbuf.dispatch((out_width + 7) / 8, (out_height + 7) / 8, 1);
    }
    taa_next = 1 - taa_next;
}

void TemporalUpscaler::ExtraSharpen(vk::CommandBuffer cmdbuf, vk::Image target, bool ldr, u32 w,
                                    u32 h) {
    const auto& settings = Gow3Settings::Get();
    const float extra = std::clamp(settings.sharpness.load(), 0.0f, 2.0f) - 1.0f;
    if (!settings.sharpen || extra <= 0.0f) {
        return;
    }
    const auto device = instance.GetDevice();
    if (extra_sharpen_width != w || extra_sharpen_height != h) {
        // Retired command buffers may still read the old copy. `cmdbuf` stays open.
        scheduler.WaitSubmitted();
        extra_sharpen_view.reset();
        extra_sharpen_image = VideoCore::UniqueImage(device, instance.GetAllocator());
        extra_sharpen_image.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = vk::Format::eR32G32B32A32Sfloat,
            .extent = {w, h, 1},
            .mipLevels = 1, .arrayLayers = 1, .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferDst,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        extra_sharpen_view = Check(device.createImageViewUnique({
            .image = vk::Image(extra_sharpen_image), .viewType = vk::ImageViewType::e2D,
            .format = vk::Format::eR32G32B32A32Sfloat,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        extra_sharpen_width = w;
        extra_sharpen_height = h;
    }
    vk::ImageView target_view = *output_view;
    if (ldr) {
        if (!ui_storage_view) {
            // The UI image's own format may be sRGB; FSR 3 writes it through a UNORM view too.
            ui_storage_view = Check(device.createImageViewUnique({
                .image = target, .viewType = vk::ImageViewType::e2D,
                .format = vk::Format::eR8G8B8A8Unorm,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            }));
        }
        target_view = *ui_storage_view;
    }
    const auto image_barrier = [&](vk::Image image, vk::ImageLayout old_layout,
                                   vk::PipelineStageFlags2 src, vk::AccessFlags2 src_access,
                                   vk::PipelineStageFlags2 dst, vk::AccessFlags2 dst_access) {
        const vk::ImageMemoryBarrier2 b{
            .srcStageMask = src, .srcAccessMask = src_access,
            .dstStageMask = dst, .dstAccessMask = dst_access,
            .oldLayout = old_layout, .newLayout = vk::ImageLayout::eGeneral,
            .image = image, .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
    };
    constexpr auto all = vk::PipelineStageFlagBits2::eAllCommands;
    constexpr auto rw = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
    image_barrier(target, vk::ImageLayout::eGeneral, all, rw, vk::PipelineStageFlagBits2::eBlit,
                  vk::AccessFlagBits2::eTransferRead);
    image_barrier(vk::Image(extra_sharpen_image), vk::ImageLayout::eUndefined, all, rw,
                  vk::PipelineStageFlagBits2::eBlit, vk::AccessFlagBits2::eTransferWrite);
    const vk::ImageBlit region{
        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .srcOffsets = std::array{vk::Offset3D{}, vk::Offset3D{s32(w), s32(h), 1}},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstOffsets = std::array{vk::Offset3D{}, vk::Offset3D{s32(w), s32(h), 1}},
    };
    cmdbuf.blitImage(target, vk::ImageLayout::eGeneral, vk::Image(extra_sharpen_image),
                     vk::ImageLayout::eGeneral, region, vk::Filter::eNearest);
    image_barrier(vk::Image(extra_sharpen_image), vk::ImageLayout::eGeneral,
                  vk::PipelineStageFlagBits2::eBlit, vk::AccessFlagBits2::eTransferWrite,
                  vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageRead);
    image_barrier(target, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eBlit,
                  vk::AccessFlagBits2::eTransferRead, vk::PipelineStageFlagBits2::eComputeShader,
                  vk::AccessFlagBits2::eShaderStorageWrite);
    const std::array<vk::DescriptorImageInfo, 2> infos{{
        {.imageView = *extra_sharpen_view, .imageLayout = vk::ImageLayout::eGeneral},
        {.imageView = target_view, .imageLayout = vk::ImageLayout::eGeneral}}};
    std::array<vk::WriteDescriptorSet, 2> writes{};
    for (u32 i = 0; i < writes.size(); ++i)
        writes[i] = {.dstBinding = i, .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &infos[i]};
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute,
                        ldr ? *taa_sharpen_ldr_pipeline : *taa_sharpen_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *taa_sharpen_pipeline_layout, 0,
                                writes);
    cmdbuf.pushConstants(*taa_sharpen_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                         sizeof(extra), &extra);
    cmdbuf.dispatch((w + 7) / 8, (h + 7) / 8, 1);
    image_barrier(target, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                  vk::AccessFlagBits2::eShaderStorageWrite, all, rw);
}

void TemporalUpscaler::OnBlendedSceneDraw() {
    // The mask is opt-in (menu, GOW3_REACTIVE=1): on thin mist it traded trails for
    // jitter shimmer, which looked worse. Toggle 1 << 27 switches it off.
    if (snapshot_taken || !scene_color || !Active() || !ReactiveOn()) {
        return;
    }
    snapshot_taken = true;
    auto& color = texture_cache.GetImage(scene_color);
    // With a live preset the scene is in the reduced proxy: the mask and the FSR context
    // must use that size, or each frame would recreate the context (native <-> reduced).
    const bool reduced = ReducedScene(color);
    const u32 ow = color.info.size.width, oh = color.info.size.height;
    // Scaled presets: the scene fills the top-left of its aligned targets (see RunScaled).
    const auto scene = Scaled() ? SceneSize(ow, oh) : std::array<u32, 2>{ow, oh};
    const u32 w = reduced ? scene_targets.Size().width : scene[0];
    const u32 h = reduced ? scene_targets.Size().height : scene[1];
    if (color.info.pixel_format != vk::Format::eR16G16B16A16Sfloat ||
        !(color.usage_flags & vk::ImageUsageFlagBits::eTransferSrc)) {
        return;
    }
    if (!EnsureResources(w, h, Scaled() ? target_width : ow,
                         Scaled() ? target_height : oh, !Scaled())) {
        failed = true;
        return;
    }
    scheduler.EndRendering();
    vk::Image source = color.GetImage();
    vk::ImageLayout source_layout = vk::ImageLayout::eTransferSrcOptimal;
    if (reduced) {
        VideoCore::ImageViewInfo ci;
        ci.format = color.info.pixel_format;
        source = scene_targets.Read(scene_color, ci, vk::PipelineStageFlagBits2::eTransfer,
                                    vk::AccessFlagBits2::eTransferRead).image;
        source_layout = vk::ImageLayout::eGeneral;
    } else {
        runtime.Transit(&color, vk::ImageLayout::eTransferSrcOptimal,
                        vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
        runtime.FlushBarriers();
    }
    const vk::Image opaque = vk::Image(opaque_image);
    const vk::ImageCopy region{
        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .extent = {w, h, 1},
    };
    // On the recording thread: direct recording here would move the rest of the scene there.
    scheduler.Record([opaque, source, source_layout, region](vk::CommandBuffer cmdbuf) {
        const auto to_general = [&](vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                                    vk::ImageLayout old_layout, vk::PipelineStageFlags2 dst_stage,
                                    vk::AccessFlags2 dst_access) {
            const vk::ImageMemoryBarrier2 barrier{
                .srcStageMask = src_stage,
                .srcAccessMask = src_access,
                .dstStageMask = dst_stage,
                .dstAccessMask = dst_access,
                .oldLayout = old_layout,
                .newLayout = vk::ImageLayout::eGeneral,
                .image = opaque,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            };
            cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
        };
        // The previous frame's mask pass read it: wait for that before overwriting.
        to_general(vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eNone,
                   vk::ImageLayout::eUndefined, vk::PipelineStageFlagBits2::eTransfer,
                   vk::AccessFlagBits2::eTransferWrite);
        cmdbuf.copyImage(source, source_layout, opaque, vk::ImageLayout::eGeneral, region);
        to_general(vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                   vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                   vk::AccessFlagBits2::eShaderRead);
    });
    opaque_valid = true;
}

void TemporalUpscaler::OnSceneComposite() {
    if (!opaque_valid || mask_ready || failed || !ReactiveOn()) {
        return;
    }
    auto& color = texture_cache.GetImage(scene_color);
    const bool reduced = ReducedScene(color);
    const auto scene = Scaled() ? SceneSize(color.info.size.width, color.info.size.height)
                                : std::array<u32, 2>{color.info.size.width, color.info.size.height};
    const u32 w = reduced ? scene_targets.Size().width : scene[0];
    const u32 h = reduced ? scene_targets.Size().height : scene[1];
    if (w != width || h != height || !(color.usage_flags & vk::ImageUsageFlagBits::eStorage)) {
        return;
    }
    scheduler.EndRendering();
    if (reduced) {
        VideoCore::ImageViewInfo ci;
        ci.format = color.info.pixel_format;
        const auto proxy = scene_targets.Read(scene_color, ci);
        mask_ready = RecordReactive(proxy.view);
        return;
    }
    const auto device = instance.GetDevice();
    const auto color_view = Check(device.createImageView({
        .image = vk::Image(color.backing->image),
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    runtime.Transit(&color, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    runtime.FlushBarriers();
    mask_ready = RecordReactive(color_view);
    scheduler.DeferOperation([device, color_view] { device.destroyImageView(color_view); });
}

bool TemporalUpscaler::RecordReactive(vk::ImageView color_view) {
    if (!opaque_valid || !ReactiveOn()) {
        return false;
    }
    const vk::Image reactive = vk::Image(reactive_image);
    const vk::ImageView opaque = *opaque_view, mask = *reactive_view;
    const vk::Pipeline pipeline = *reactive_pipeline;
    const vk::PipelineLayout layout = *reactive_pipeline_layout;
    const u32 w = width, h = height;
    // Relative color change times the scale (default 1), zero below the threshold (0.2), at
    // most the maximum (0.9, never fully reactive) — the defaults of AMD's mask generator.
    const auto& settings = Gow3Settings::Get();
    const std::array<float, 3> params{settings.reactive_scale, settings.reactive_max,
                                      settings.reactive_threshold};
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        const vk::ImageMemoryBarrier2 to_write{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = reactive,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_write});
        const vk::DescriptorImageInfo opaque_info{.imageView = opaque,
                                                  .imageLayout = vk::ImageLayout::eGeneral};
        const vk::DescriptorImageInfo scene_info{.imageView = color_view,
                                                 .imageLayout = vk::ImageLayout::eGeneral};
        const vk::DescriptorImageInfo mask_info{.imageView = mask,
                                                .imageLayout = vk::ImageLayout::eGeneral};
        const std::array<vk::WriteDescriptorSet, 3> writes = {{
            {.dstBinding = 0,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .pImageInfo = &opaque_info},
            {.dstBinding = 1,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .pImageInfo = &scene_info},
            {.dstBinding = 2,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .pImageInfo = &mask_info},
        }};
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0,
                                    writes);
        cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0,
                             sizeof(params), params.data());
        cmdbuf.dispatch((w + 7) / 8, (h + 7) / 8, 1);
        const vk::ImageMemoryBarrier2 to_read{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = reactive,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read});
    });
    return true;
}

void TemporalUpscaler::Run() {
    if (auto* profiler = GpuProfiler::Get()) {
        const char* label = Gow3Settings::Get().upscaler == Gow3Settings::UpscalerTaa
            ? "upscaler Run (TAA)" : "upscaler Run (FSR)";
        profiler->Mark(0xF5A0'0000ull ^ std::hash<std::string_view>{}(label),
                       [label] { return std::string{label}; });
    }
    auto& color = texture_cache.GetImage(scene_color);
    auto& depth = texture_cache.GetImage(camera_motion.Depth());
    const u32 ow = color.info.size.width, oh = color.info.size.height;
    const bool reduced = ReducedScene(color) && scene_targets.EligibleScene(depth);
    const u32 w = reduced ? scene_targets.Size().width : ow;
    const u32 h = reduced ? scene_targets.Size().height : oh;
    if (color.info.pixel_format != vk::Format::eR16G16B16A16Sfloat ||
        depth.info.size.width != ow || depth.info.size.height != oh ||
        !(color.usage_flags & vk::ImageUsageFlagBits::eStorage)) {
        return;
    }
    if (!EnsureResources(w, h, ow, oh, true)) {
        failed = true;
        return;
    }
    const auto depth_format = depth.info.pixel_format;
    const auto depth_view =
        CachedView(depth, depth_format, vk::ImageAspectFlagBits::eDepth);
    const auto color_view =
        CachedView(color, vk::Format::eR16G16B16A16Sfloat, vk::ImageAspectFlagBits::eColor);

    scheduler.EndRendering();
    runtime.Transit(&depth, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    runtime.Transit(&color, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    runtime.FlushBarriers();
    vk::Image input_color = color.GetImage(), input_depth = depth.GetImage();
    vk::ImageView input_color_view = color_view, input_depth_view = depth_view;
    if (reduced) {
        VideoCore::ImageViewInfo ci;
        ci.format = color.info.pixel_format;
        const auto c = scene_targets.Read(scene_color, ci);
        ci.format = depth.info.pixel_format;
        const auto d = scene_targets.Read(camera_motion.Depth(), ci);
        input_color = c.image;
        input_depth = d.image;
        input_color_view = c.view;
        input_depth_view = d.view;
    }
    const auto cmdbuf = scheduler.CommandBuffer();

    const auto own_barrier = [&](vk::Image image, vk::ImageLayout old_layout,
                                 vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                                 vk::ImageLayout new_layout, vk::PipelineStageFlags2 dst_stage,
                                 vk::AccessFlags2 dst_access) {
        const vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = src_stage,
            .srcAccessMask = src_access,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
            .oldLayout = old_layout,
            .newLayout = new_layout,
            .image = image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    };
    const auto all = vk::PipelineStageFlagBits2::eAllCommands;
    const auto rw = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
    // Previous contents are not needed: the layouts start from undefined every frame.
    own_barrier(vk::Image(motion_image), vk::ImageLayout::eUndefined, all,
                vk::AccessFlagBits2::eNone, vk::ImageLayout::eGeneral,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite);
    own_barrier(vk::Image(output_image), vk::ImageLayout::eUndefined, all,
                vk::AccessFlagBits2::eNone, vk::ImageLayout::eGeneral, all, rw);

    if (camera_motion.VelocityMode()) {
        camera_motion.RecordVelocity(cmdbuf, input_depth_view, *motion_view, w, h);
    } else {
        camera_motion.RecordMotion(cmdbuf, input_depth_view, *motion_view, w, h);
    }
    own_barrier(vk::Image(motion_image), vk::ImageLayout::eGeneral,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite,
                vk::ImageLayout::eGeneral, all, vk::AccessFlagBits2::eShaderRead);

    const bool has_reactive = mask_ready || RecordReactive(input_color_view);

    const auto now = std::chrono::steady_clock::now();
    float frame_ms = std::chrono::duration<float, std::milli>(now - last_frame).count();
    if (frame_ms <= 0.0f || frame_ms > 200.0f) {
        frame_ms = 16.6f;
    }
    last_frame = now;

    bool dispatched = false;
    if (Gow3Settings::Get().upscaler == Gow3Settings::UpscalerTaa) {
        RecordTaa(cmdbuf, input_color_view, input_depth_view);
        dispatched = true;
    } else if (UseFsr4()) {
        dispatched = RecordFsr4(cmdbuf, {input_color, input_color_view, w, h},
                                {input_depth, input_depth_view, w, h}, w, h, ow, oh, frame_ms);
    } else if (UseDlss()) {
        dispatched = RecordDlss(
            cmdbuf,
            {input_color, input_color_view, color.info.pixel_format, vk::ImageAspectFlagBits::eColor, w, h},
            {input_depth, input_depth_view, depth_format, vk::ImageAspectFlagBits::eDepth, w, h}, w, h,
            ow, oh, frame_ms, true);
    } else {
        FfxVkPortableUpscaleDispatchInfo info{};
        info.structSize = sizeof(info);
        info.commandBuffer = cmdbuf;
        info.color = Describe(input_color, color.info.pixel_format, w, h,
                              color.usage_flags, vk::ImageAspectFlagBits::eColor,
                              FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
        info.depth = Describe(input_depth, depth_format, w, h, depth.usage_flags,
                              vk::ImageAspectFlagBits::eDepth,
                              FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
        info.motionVectors = Describe(vk::Image(motion_image), vk::Format::eR16G16Sfloat, w, h,
                                      vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                                      vk::ImageAspectFlagBits::eColor,
                                      FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
        info.output = Describe(vk::Image(output_image), vk::Format::eR16G16B16A16Sfloat, ow, oh,
                               vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                                   vk::ImageUsageFlagBits::eTransferSrc,
                               vk::ImageAspectFlagBits::eColor,
                               FFX_VK_PORTABLE_RESOURCE_STATE_UNORDERED_ACCESS);
        // Optional inputs, absent: a described but null image.
        info.exposure.structSize = sizeof(info.exposure);
        info.reactiveMask.structSize = sizeof(info.reactiveMask);
        if (has_reactive) {
            info.reactiveMask = Describe(vk::Image(reactive_image), vk::Format::eR8Unorm, w, h,
                                         vk::ImageUsageFlagBits::eStorage |
                                             vk::ImageUsageFlagBits::eSampled,
                                         vk::ImageAspectFlagBits::eColor,
                                         FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
        }
        info.transparencyAndCompositionMask.structSize = sizeof(info.transparencyAndCompositionMask);
        // Toggle 1 << 26 (tests): the opposite sign convention for FSR.
        const float sign = Gow3Toggle::Disabled(1u << 26) ? -1.0f : 1.0f;
        info.jitterOffset = {sign * jitter[0], sign * jitter[1]};
        info.motionVectorScale = {1.0f, 1.0f};
        info.renderSize = {w, h};
        info.outputSize = {ow, oh};
        info.frameTimeMilliseconds = frame_ms;
        info.preExposure = 1.0f;
        info.cameraNear = camera_motion.Near();
        info.cameraFar = camera_motion.VelocityMode() ? 10000.0f : 3000.0f;
        info.cameraVerticalFovRadians = camera_motion.VerticalFov();
        info.viewSpaceToMeters = 1.0f;
        // RCAS strength 0..1 (menu, GOW3_FSR_SHARPNESS); jitter at 1:1 softens the image slightly.
        // AMD's RCAS ends at 1; ExtraSharpen adds the rest of the menu's 0..2.
        info.sharpness = std::min(Gow3Settings::Get().sharpness.load(), 1.0f);
        info.enableSharpening = Gow3Settings::Get().sharpen ? VK_TRUE : VK_FALSE;
        info.reset = reset ? VK_TRUE : VK_FALSE;
        info.frameId = frame_id++;
    
        FfxVkPortableUpscaleCreateInfo create_info{};
        create_info.structSize = sizeof(create_info);
        create_info.flags = FFX_VK_PORTABLE_CONTEXT_HDR_COLOR_INPUT | FFX_VK_PORTABLE_CONTEXT_AUTO_EXPOSURE;
        create_info.maxRenderSize = {w, h};
        create_info.maxOutputSize = {ow, oh};
        if (const u64 issues = ffxVkPortableValidateUpscaleDispatchInfo(&create_info, &info)) {
            static bool printed = false;
            if (!printed) {
                printed = true;
                PrintIssues("dispatch", issues);
            }
            failed = true;
        } else if (ffxVkPortableUpscaleContextRecordDispatch(context, &info) !=
                   FFX_VK_PORTABLE_OK) {
            std::printf("Upscaler: FSR 3 dispatch failed\n");
            failed = true;
        } else {
            dispatched = true;
        }
    }
    if (dispatched) {
        reset = false;
        dispatched_last_frame = true;
        if (Gow3Settings::Get().upscaler != Gow3Settings::UpscalerTaa) {
            ExtraSharpen(cmdbuf, vk::Image(output_image), false, ow, oh);
        }
        // gow3: GOW3_DUMP_TRIGGER also dumps native anti-aliasing (Uncharted): its input, motion
        // vectors, result and depth, before the result replaces the scene color.
        if (const int dump = DumpFrame(); dump >= 0) {
            DumpImages(instance, scheduler, cmdbuf, dump,
                       {{input_color, w, h, 8, "input", "rgba16f"},
                        {vk::Image(motion_image), w, h, 4, "motion", "rg16f"},
                        {vk::Image(output_image), ow, oh, 8, "output", "rgba16f"}});
            if (depth_format == vk::Format::eD32SfloatS8Uint ||
                depth_format == vk::Format::eD32Sfloat) {
                DumpImages(instance, scheduler, cmdbuf, dump,
                           {{input_depth, w, h, 4, "depth", "f32",
                             vk::ImageAspectFlagBits::eDepth}});
            }
        }
        // The result replaces the scene color's RGB (its alpha carries data for the post).
        own_barrier(vk::Image(output_image), vk::ImageLayout::eGeneral, all, rw,
                    vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
        runtime.Transit(&color, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
        runtime.FlushBarriers();
        const vk::DescriptorImageInfo out_info{.imageView = *output_view,
                                               .imageLayout = vk::ImageLayout::eGeneral};
        const vk::DescriptorImageInfo scene_info{.imageView = color_view,
                                                 .imageLayout = vk::ImageLayout::eGeneral};
        const vk::DescriptorImageInfo mask_info{.imageView = *reactive_view,
                                                .imageLayout = vk::ImageLayout::eGeneral};
        // Sampled for the debug view only; the object image falls back to the motion image.
        const vk::ImageView objects = camera_motion.ObjectMotionView();
        const vk::DescriptorImageInfo motion_info{.imageView = *motion_view,
                                                  .imageLayout = vk::ImageLayout::eGeneral};
        const vk::DescriptorImageInfo objects_info{
            .imageView = objects ? objects : *motion_view,
            .imageLayout = vk::ImageLayout::eGeneral};
        const std::array<vk::WriteDescriptorSet, 5> writes = {{
            {.dstBinding = 0,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .pImageInfo = &out_info},
            {.dstBinding = 1,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .pImageInfo = &scene_info},
            {.dstBinding = 2,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .pImageInfo = &mask_info},
            {.dstBinding = 3,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .pImageInfo = &motion_info},
            {.dstBinding = 4,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .pImageInfo = &objects_info},
        }};
        // Debug (menu or toggle 1 << 28): the reactive mask in red over a darkened frame;
        // or the motion vectors (menu).
        const int debug_view = Gow3Settings::Get().debug_view;
        const u32 mode = has_reactive && (debug_view == Gow3Settings::DebugReactive ||
                                          Gow3Toggle::Disabled(1u << 28))
                             ? 1
                         : debug_view == Gow3Settings::DebugMotion ? 2
                                                                 : 0;
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *merge_pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *merge_pipeline_layout, 0,
                                    writes);
        cmdbuf.pushConstants(*merge_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                             sizeof(mode), &mode);
        cmdbuf.dispatch((ow + 7) / 8, (oh + 7) / 8, 1);
    }
}

vk::ImageView TemporalUpscaler::CachedView(const VideoCore::Image& image, vk::Format format,
                                           vk::ImageAspectFlags aspect) {
    const vk::Image handle = image.GetImage();
    ViewEntry* oldest = &view_cache[0];
    for (auto& entry : view_cache) {
        if (entry.view && entry.image == handle && entry.uid == image.image_uid &&
            entry.format == format && entry.aspect == aspect) {
            entry.last_use = ++view_uses;
            return entry.view;
        }
        if (entry.last_use < oldest->last_use) {
            oldest = &entry;
        }
    }
    const auto device = instance.GetDevice();
    if (oldest->view) {
        scheduler.DeferOperation([device, view = oldest->view] { device.destroyImageView(view); });
    }
    *oldest = {
        .image = handle,
        .uid = image.image_uid,
        .format = format,
        .aspect = aspect,
        .view = Check(device.createImageView({
            .image = handle,
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = {aspect, 0, 1, 0, 1},
        })),
        .last_use = ++view_uses,
    };
    return oldest->view;
}

} // namespace Vulkan

namespace Vulkan {

std::array<u32, 2> TemporalUpscaler::SceneSize(u32 w, u32 h) const {
    if (!scaled_session) return {render_width, render_height};
    // The game's own render size (scene constants); GOW3_RENDER_RES until the first camera.
    auto size = camera_motion.RenderSize();
    if (size[0] == 0 || size[1] == 0) {
        size = {render_width, render_height};
    }
    return {std::min(size[0], w), std::min(size[1], h)};
}

float TemporalUpscaler::SceneMipBias() const {
    if (!Active() || Gow3Settings::Get().upscaler == Gow3Settings::UpscalerTaa) return 0.0f;
    const float render = float(Gow3Settings::Get().active_render_width.load());
    const float output = float(Scaled() ? target_width : 1920u);
    return render > 0.0f && render < output ? std::log2(render / output) : 0.0f;
}

bool TemporalUpscaler::Scaled() const {
    return scaled_session || target_width != 1920 || target_height != 1080;
}

void TemporalUpscaler::OnColorTarget(VideoCore::ImageId color) {
    if (composite_frames) {
        // Uncharted: the copy of the finished frame (RGBA8 at the render size) that the composite
        // draw puts back into the display buffer; also in menus without a 3D scene.
        const auto& image = texture_cache.GetImage(color);
        if (image.info.pixel_format == vk::Format::eR8G8B8A8Unorm &&
            RenderTarget(image.info.size.width, image.info.size.height) &&
            !FrameCapture::IsDisplayBuffer(image.info.guest_address)) {
            ldr_target = color;
        }
        return;
    }
    if (ui_phase || !camera_motion.Depth()) {
        return;
    }
    const auto& image = texture_cache.GetImage(color);
    const auto& depth = texture_cache.GetImage(camera_motion.Depth());
    if (image.info.pixel_format == vk::Format::eR8G8B8A8Unorm &&
        image.info.size.width == depth.info.size.width &&
        image.info.size.height == depth.info.size.height) {
        ldr_target = color;
    }
}

void TemporalUpscaler::OnDraw(u64 vs_hash, VideoCore::ImageId color,
                              VideoCore::ImageId depth, bool native_viewport) {
    if (composite_frames) {
        return; // the UI starts at OnComposite, not at Bloodborne's Scaleform draws
    }
    if (!Scaled() && vs_hash == ui_trigger_vs) done_this_frame = true;
    if (!Scaled() || !color) {
        return;
    }
    if (ui_phase) {
        // A UI movie can start without stencil and enable it for later text/masks.
        if (color == ui_color && depth && depth != ui_depth) {
            EnsureUiResources(ui_width, ui_height, ui_format,
                              texture_cache.GetImage(depth).info.pixel_format);
            PrepareUiDepth(depth);
            ui_depth = depth;
        }
        return;
    }
    const auto& image = texture_cache.GetImage(color);
    const bool movie = vs_hash == ui_trigger_vs || UiComposition::MovieShader(vs_hash) ||
                       (scaled_session && native_viewport);
    const bool ui_draw = movie &&
        (image.info.pixel_format == vk::Format::eR8G8B8A8Unorm ||
         image.info.pixel_format == vk::Format::eR8G8B8A8Srgb) &&
        RenderTarget(image.info.size.width, image.info.size.height) &&
        !FrameCapture::IsDisplayBuffer(image.info.guest_address);
    switch (UiComposition::Choose(Scaled(), ui_draw,
                                  camera_motion.Ready() && ldr_target == color,
                                  Active() && !failed)) {
    case UiComposition::Background::None:
        return;
    case UiComposition::Background::Temporal:
        RunScaled();
        if (ui_phase) {
            return;
        }
        // A failed FSR dispatch must not force UI text back to the scene resolution.
        [[fallthrough]];
    case UiComposition::Background::Copy:
        RunUiOnly(color, depth);
        return;
    }
}

void TemporalUpscaler::EnsureUiResources(u32 w, u32 h, vk::Format color, vk::Format depth) {
    const bool resized = w != ui_width || h != ui_height;
    const bool new_color = !ui_image || resized || color != ui_format;
    const bool new_depth = !ui_depth_image || resized || depth != ui_depth_format;
    if (!new_color && !new_depth) {
        return;
    }
    scheduler.Finish();
    const auto device = instance.GetDevice();
    const auto allocator = instance.GetAllocator();
    ui_width = w;
    ui_height = h;
    if (new_color) {
        ui_views.clear();
        ui_decode_view.reset();
        ui_view.reset();
        ui_storage_view.reset();
        ui_image = VideoCore::UniqueImage(device, allocator);
        ui_image.Create(vk::ImageCreateInfo{
            .flags = vk::ImageCreateFlagBits::eMutableFormat | vk::ImageCreateFlagBits::eExtendedUsage,
            .imageType = vk::ImageType::e2D,
            .format = color,
            .extent = {w, h, 1},
            .mipLevels = 1, .arrayLayers = 1, .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                     vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
                     vk::ImageUsageFlagBits::eTransferSrc,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        ui_format = color;
    }
    if (new_depth) {
        ui_depth_view.reset();
        ui_depth_image = VideoCore::UniqueImage(device, allocator);
        ui_depth_image.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = depth,
            .extent = {w, h, 1},
            .mipLevels = 1, .arrayLayers = 1, .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eDepthStencilAttachment |
                     vk::ImageUsageFlagBits::eTransferDst,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        const auto aspect = vk::ImageAspectFlagBits::eDepth |
            (depth == vk::Format::eD32SfloatS8Uint ? vk::ImageAspectFlagBits::eStencil
                                                : vk::ImageAspectFlags{});
        ui_depth_view = Check(device.createImageViewUnique({
            .image = vk::Image(ui_depth_image), .viewType = vk::ImageViewType::e2D,
            .format = depth, .subresourceRange = {aspect, 0, 1, 0, 1},
        }));
        ui_depth_format = depth;
    }
    std::printf("UI: native composition %ux%u (scene %ux%u), independent of FSR history\n",
                w, h, render_width, render_height);
}

void TemporalUpscaler::PrepareUiDepth(VideoCore::ImageId depth_id) {
    scheduler.EndRendering();
    const auto aspect = vk::ImageAspectFlagBits::eDepth |
        (ui_depth_format == vk::Format::eD32SfloatS8Uint ? vk::ImageAspectFlagBits::eStencil
                                                      : vk::ImageAspectFlags{});
    // Copy depth if blit is supported; otherwise just clear it (UI depth test still works).
    const bool copy = depth_id && depth_blit &&
        texture_cache.GetImage(depth_id).info.pixel_format == ui_depth_format;
    if (copy) {
        runtime.Transit(&texture_cache.GetImage(depth_id), vk::ImageLayout::eTransferSrcOptimal,
                        vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
        runtime.FlushBarriers();
    }
    const auto cmd = scheduler.CommandBuffer();
    const vk::ImageSubresourceRange range{aspect, 0, 1, 0, 1};
    vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eTransferDstOptimal,
        .image = vk::Image(ui_depth_image), .subresourceRange = range,
    };
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    cmd.clearDepthStencilImage(vk::Image(ui_depth_image), vk::ImageLayout::eTransferDstOptimal,
                               {.depth = 1.0f, .stencil = 0}, range);
    if (copy) {
        barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
        barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
        barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
        const auto& source = texture_cache.GetImage(depth_id);
        const vk::ImageBlit region{
            .srcSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1},
            .srcOffsets = std::array{vk::Offset3D{0, 0, 0},
                vk::Offset3D{s32(source.info.size.width), s32(source.info.size.height), 1}},
            .dstSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1},
            .dstOffsets = std::array{vk::Offset3D{0, 0, 0},
                vk::Offset3D{s32(ui_width), s32(ui_height), 1}},
        };
        cmd.blitImage(vk::Image(source.backing->image), vk::ImageLayout::eTransferSrcOptimal,
                      vk::Image(ui_depth_image), vk::ImageLayout::eTransferDstOptimal,
                      region, vk::Filter::eNearest);
    }
    // Without blit support, UI depth is cleared but not copied from the scene.
    // UI elements will still depth-test correctly against the cleared buffer.
    barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
    barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
    barrier.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                           vk::PipelineStageFlagBits2::eLateFragmentTests;
    barrier.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                            vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
    barrier.newLayout = vk::ImageLayout::eGeneral;
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
}

void TemporalUpscaler::RunUiOnly(VideoCore::ImageId color_id, VideoCore::ImageId depth_id) {
    if (auto* profiler = GpuProfiler::Get()) {
        profiler->Mark(0xF5A1'0000ull, [] { return std::string{"upscaler RunUiOnly"}; });
    }
    const auto& color = texture_cache.GetImage(color_id);
    const auto depth_format = depth_id ? texture_cache.GetImage(depth_id).info.pixel_format
                                      : vk::Format::eD32SfloatS8Uint;
    EnsureUiResources(target_width, target_height, color.info.pixel_format, depth_format);
    PrepareUiDepth(depth_id);
    // Copy only the background already drawn. The subsequent menu/HUD/text draws execute
    // directly at output resolution, including frames with no scene or camera at all.
    vk::Image source = color.GetImage();
    auto source_layout = vk::ImageLayout::eTransferSrcOptimal;
    u32 source_width = color.info.size.width, source_height = color.info.size.height;
    if (!scaled_session && camera_motion.Depth() && scene_targets.Reduced() &&
        scene_targets.EligibleScene(color)) {
        VideoCore::ImageViewInfo view;
        view.format = color.info.pixel_format;
        const auto proxy = scene_targets.Read(color_id, view,
                                              vk::PipelineStageFlagBits2::eTransfer,
                                              vk::AccessFlagBits2::eTransferRead);
        source = proxy.image;
        source_layout = proxy.layout;
        source_width = render_width;
        source_height = render_height;
    } else {
        runtime.Transit(&texture_cache.GetImage(color_id), vk::ImageLayout::eTransferSrcOptimal,
                        vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
        runtime.FlushBarriers();
    }
    const auto cmd = scheduler.CommandBuffer();
    vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eTransferDstOptimal,
        .image = vk::Image(ui_image),
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    const vk::ImageBlit region{
        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .srcOffsets = std::array{vk::Offset3D{0, 0, 0},
            vk::Offset3D{s32(source_width), s32(source_height), 1}},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstOffsets = std::array{vk::Offset3D{0, 0, 0},
            vk::Offset3D{s32(ui_width), s32(ui_height), 1}},
    };
    cmd.blitImage(source, source_layout,
                  vk::Image(ui_image), vk::ImageLayout::eTransferDstOptimal, region,
                  vk::Filter::eLinear);
    barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
    barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
    barrier.dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
    barrier.dstAccessMask = vk::AccessFlagBits2::eColorAttachmentRead |
                            vk::AccessFlagBits2::eColorAttachmentWrite;
    barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
    barrier.newLayout = vk::ImageLayout::eGeneral;
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    reset = true; // returning to 3D must not reuse history from before a menu/loading screen
    done_this_frame = ui_phase = true;
    ui_color = color_id;
    ui_depth = depth_id;
    DecodeToHdrUi(cmd);
}

void TemporalUpscaler::RunScaled() {
    if (auto* profiler = GpuProfiler::Get()) {
        const char* label = Gow3Settings::Get().upscaler == Gow3Settings::UpscalerTaa
            ? "upscaler RunScaled (TAA)" : "upscaler RunScaled (FSR)";
        profiler->Mark(0xF5A0'0000ull ^ std::hash<std::string_view>{}(label),
                       [label] { return std::string{label}; });
    }
    auto& color = texture_cache.GetImage(ldr_target);
    auto& depth = texture_cache.GetImage(camera_motion.Depth());
    // The scene fills the top-left w x h of its targets (iw x ih, aligned by the game).
    const u32 iw = color.info.size.width, ih = color.info.size.height;
    const auto [w, h] = SceneSize(iw, ih);
    const u32 ow = target_width, oh = target_height;
    if (depth.info.size.width != iw || depth.info.size.height != ih || w > ow || h > oh) {
        return;
    }
    // TAA resolves at the render size (tw x th); FSR 1 (or a bilinear stretch) brings it to the
    // output size after. The others upscale themselves.
    const bool taa = Gow3Settings::Get().upscaler == Gow3Settings::UpscalerTaa;
    const u32 tw = taa ? w : ow, th = taa ? h : oh;
    if (!EnsureResources(w, h, tw, th, false)) {
        failed = true;
        return;
    }
    EnsureUiResources(ow, oh, LdrFormat(color.info.pixel_format), depth.info.pixel_format);
    PrepareUiDepth(camera_motion.Depth());
    const auto depth_format = depth.info.pixel_format;
    // Views kept across frames: FSR 4 registers images by view in a registry of eight.
    vk::Image depth_image = depth.GetImage(), color_image = color.GetImage();
    vk::ImageView depth_view{}, color_view{};
    u32 source_width = iw, source_height = ih;
    if (!scaled_session) {
        VideoCore::ImageViewInfo ci, di;
        ci.format = color.info.pixel_format;
        di.format = depth_format;
        const auto cp = scene_targets.Read(ldr_target, ci);
        const auto dp = scene_targets.Read(camera_motion.Depth(), di);
        color_image = cp.image;
        color_view = cp.view;
        depth_image = dp.image;
        depth_view = dp.view;
        source_width = w;
        source_height = h;
    } else {
        depth_view = CachedView(depth, depth_format, vk::ImageAspectFlagBits::eDepth);
        color_view = CachedView(color, LdrFormat(color.info.pixel_format), vk::ImageAspectFlagBits::eColor);
        runtime.Transit(&depth, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                        vk::AccessFlagBits2::eShaderRead);
        runtime.Transit(&color, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                        vk::AccessFlagBits2::eShaderRead);
        runtime.FlushBarriers();
    }

    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();
    const auto barrier = [&](vk::Image image, vk::ImageAspectFlags aspect,
                             vk::ImageLayout old_layout, vk::PipelineStageFlags2 src_stage,
                             vk::AccessFlags2 src_access, vk::ImageLayout new_layout,
                             vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
        const vk::ImageMemoryBarrier2 b{
            .srcStageMask = src_stage,
            .srcAccessMask = src_access,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
            .oldLayout = old_layout,
            .newLayout = new_layout,
            .image = image,
            .subresourceRange = {aspect, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
    };
    const auto all = vk::PipelineStageFlagBits2::eAllCommands;
    const auto color_access = vk::AccessFlagBits2::eColorAttachmentRead |
                              vk::AccessFlagBits2::eColorAttachmentWrite;
    const auto rw = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
    // The previous frame's display pass read it: all commands before.
    barrier(vk::Image(ui_image), vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eUndefined,
            all, vk::AccessFlagBits2::eNone, vk::ImageLayout::eGeneral, all, rw);
    barrier(vk::Image(motion_image), vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eUndefined,
            all, vk::AccessFlagBits2::eNone, vk::ImageLayout::eGeneral,
            vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite);
    if (camera_motion.VelocityMode()) {
        camera_motion.RecordVelocity(cmdbuf, depth_view, *motion_view, w, h);
    } else {
        camera_motion.RecordMotion(cmdbuf, depth_view, *motion_view, w, h);
    }
    barrier(vk::Image(motion_image), vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eGeneral,
            vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite,
            vk::ImageLayout::eGeneral, all, vk::AccessFlagBits2::eShaderRead);

    const auto now = std::chrono::steady_clock::now();
    float frame_ms = std::chrono::duration<float, std::milli>(now - last_frame).count();
    if (frame_ms <= 0.0f || frame_ms > 200.0f) {
        frame_ms = 16.6f;
    }
    last_frame = now;

    // gow3: FSR 4 writes its HDR-format output, copied into the output-size UI image.
    if (UseFsr4() || UseDlss() || Gow3Settings::Get().upscaler == Gow3Settings::UpscalerTaa) {
        barrier(vk::Image(output_image), vk::ImageAspectFlagBits::eColor,
                vk::ImageLayout::eUndefined, all, vk::AccessFlagBits2::eNone,
                vk::ImageLayout::eGeneral, all, rw);
        bool ok4 = true;
        if (Gow3Settings::Get().upscaler == Gow3Settings::UpscalerTaa) {
            RecordTaa(cmdbuf, color_view, depth_view);
        } else if (UseDlss()) {
            // gow3: HDR10 chain: DLSS in HDR mode on the Scene shadow (linear, the game's own
            // frame with RenoDX highlights), drawn this frame by the pass the port upscales.
            const bool hdr_input = hdr_chain && scene_shadow.drawn && HdrStage() >= 3 &&
                                   scene_shadow.width == color.info.size.width &&
                                   scene_shadow.height == color.info.size.height;
            ok4 = hdr_input
                      ? RecordDlss(cmdbuf,
                                   {vk::Image(scene_shadow.image), *scene_shadow.view,
                                    vk::Format::eR16G16B16A16Sfloat,
                                    vk::ImageAspectFlagBits::eColor, source_width, source_height},
                                   {depth_image, depth_view, depth_format,
                                    vk::ImageAspectFlagBits::eDepth, source_width, source_height},
                                   w, h, ow, oh, frame_ms, true)
                      : RecordDlss(cmdbuf,
                                   {color_image, color_view, LdrFormat(color.info.pixel_format),
                                    vk::ImageAspectFlagBits::eColor, source_width, source_height},
                                   {depth_image, depth_view, depth_format,
                                    vk::ImageAspectFlagBits::eDepth, source_width, source_height},
                                   w, h, ow, oh, frame_ms, false);
            dlss_hdr_output = hdr_input && ok4;
        } else {
            ok4 = RecordFsr4(cmdbuf, {color_image, color_view, source_width, source_height},
                            {depth_image, depth_view, source_width, source_height}, w, h, ow,
                            oh, frame_ms);
        }
        if (ok4) {
            if (Gow3Settings::Get().upscaler != Gow3Settings::UpscalerTaa) {
                ExtraSharpen(cmdbuf, vk::Image(output_image), false, ow, oh);
            }
            // HDR10 chain: DLSS's HDR output goes to the HUD image itself (DecodeToHdrUi).
            if (!dlss_hdr_output) {
                vk::Image source = vk::Image(output_image);
                vk::ImageLayout source_layout = vk::ImageLayout::eGeneral;
                u32 sw = tw, sh = th;
                if (taa && Gow3Settings::Get().fsr1 && tw < ow && th < oh) {
                    barrier(source, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eGeneral, all, rw,
                            vk::ImageLayout::eShaderReadOnlyOptimal,
                            vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead);
                    if (!fsr1_created) {
                        fsr1_pass.Create(instance.GetDevice(), instance.GetAllocator(), 2);
                        fsr1_created = true;
                    }
                    fsr1_pass.Render(cmdbuf, *output_view, {tw, th}, {ow, oh},
                                     {.enable = true, .use_rcas = false}, false);
                    source = fsr1_pass.LastOutput();
                    source_layout = vk::ImageLayout::eTransferSrcOptimal;
                    sw = ow;
                    sh = oh;
                    barrier(source, vk::ImageAspectFlagBits::eColor,
                            vk::ImageLayout::eShaderReadOnlyOptimal,
                            vk::PipelineStageFlagBits2::eComputeShader,
                            vk::AccessFlagBits2::eShaderStorageWrite, source_layout,
                            vk::PipelineStageFlagBits2::eBlit, vk::AccessFlagBits2::eTransferRead);
                } else {
                    barrier(source, vk::ImageAspectFlagBits::eColor,
                            vk::ImageLayout::eGeneral, all, rw, vk::ImageLayout::eGeneral,
                            vk::PipelineStageFlagBits2::eBlit, vk::AccessFlagBits2::eTransferRead);
                }
                barrier(vk::Image(ui_image), vk::ImageAspectFlagBits::eColor,
                        vk::ImageLayout::eGeneral, all, rw, vk::ImageLayout::eGeneral,
                        vk::PipelineStageFlagBits2::eBlit, vk::AccessFlagBits2::eTransferWrite);
                const vk::ImageBlit region{
                    .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                    .srcOffsets = std::array{vk::Offset3D{0, 0, 0},
                                             vk::Offset3D{s32(sw), s32(sh), 1}},
                    .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                    .dstOffsets = std::array{vk::Offset3D{0, 0, 0},
                                             vk::Offset3D{s32(ow), s32(oh), 1}},
                };
                cmdbuf.blitImage(source, source_layout, vk::Image(ui_image), vk::ImageLayout::eGeneral,
                                 region, sw == ow && sh == oh ? vk::Filter::eNearest : vk::Filter::eLinear);
                barrier(vk::Image(ui_image), vk::ImageAspectFlagBits::eColor,
                        vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eBlit,
                        vk::AccessFlagBits2::eTransferWrite, vk::ImageLayout::eGeneral,
                        vk::PipelineStageFlagBits2::eColorAttachmentOutput, color_access);
            } else {
                barrier(vk::Image(output_image), vk::ImageAspectFlagBits::eColor,
                        vk::ImageLayout::eGeneral, all, rw, vk::ImageLayout::eGeneral,
                        vk::PipelineStageFlagBits2::eComputeShader,
                        vk::AccessFlagBits2::eShaderStorageRead);
            }
            reset = false;
            dispatched_last_frame = true;
            if (const int dump = DumpFrame(); dump >= 0) {
                DumpImages(instance, scheduler, cmdbuf, dump,
                           {{color_image, source_width, source_height, 4, "input", "rgba"},
                            {vk::Image(motion_image), w, h, 4, "motion", "rg16f"},
                            {vk::Image(output_image), tw, th, 8, "output", "rgba16f"}});
                DumpImages(instance, scheduler, cmdbuf, dump,
                           {{depth_image, source_width, source_height, 4, "depth", "f32",
                             vk::ImageAspectFlagBits::eDepth}});
                if (Gow3Settings::Get().upscaler == Gow3Settings::UpscalerTaa &&
                    std::getenv("GOW3_TAA_DIAGNOSTICS")) {
                    DumpImages(instance, scheduler, cmdbuf, dump,
                               {{vk::Image(taa_history[taa_next]),tw,th,16,
                                 "previous_history","rgba32f"},
                                {vk::Image(opaque_image),tw,th,8,"history_sample","rgba16f"}});
                }
                // ObjectMotion's attachment can be in a different resolution from the FSR
                // inputs: only dump it when it matches, to avoid an out-of-bounds GPU copy.
                if (const auto object = camera_motion.ObjectMotionImage(w, h); object) {
                    DumpImages(instance, scheduler, cmdbuf, dump,
                               {{object, w, h, 16, "objects", "rgba32f"}});
                }
            }
        }
        done_this_frame = true;
        if (ok4) {
            ui_phase = true;
            ui_color = ldr_target;
            ui_depth = camera_motion.Depth();
            DecodeToHdrUi(cmdbuf, dlss_hdr_output ? *output_view : vk::ImageView{});
        }
        return;
    }

    const auto& settings = Gow3Settings::Get();
    FfxVkPortableUpscaleDispatchInfo info{};
    info.structSize = sizeof(info);
    info.commandBuffer = cmdbuf;
    info.color = Describe(color_image, LdrFormat(color.info.pixel_format), source_width, source_height,
                          color.usage_flags, vk::ImageAspectFlagBits::eColor,
                          FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    info.depth = Describe(depth_image, depth_format, source_width, source_height, depth.usage_flags,
                          vk::ImageAspectFlagBits::eDepth,
                          FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    info.motionVectors = Describe(vk::Image(motion_image), vk::Format::eR16G16Sfloat, w, h,
                                  vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                                  vk::ImageAspectFlagBits::eColor,
                                  FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    info.output = Describe(vk::Image(ui_image), vk::Format::eR8G8B8A8Unorm, ow, oh,
                           vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                               vk::ImageUsageFlagBits::eColorAttachment,
                           vk::ImageAspectFlagBits::eColor,
                           FFX_VK_PORTABLE_RESOURCE_STATE_UNORDERED_ACCESS);
    info.exposure.structSize = sizeof(info.exposure);
    info.reactiveMask.structSize = sizeof(info.reactiveMask);
    if (mask_ready && ReactiveOn()) {
        info.reactiveMask = Describe(vk::Image(reactive_image), vk::Format::eR8Unorm, w, h,
            vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
            vk::ImageAspectFlagBits::eColor, FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    }

    info.transparencyAndCompositionMask.structSize = sizeof(info.transparencyAndCompositionMask);
    const float sign = Gow3Toggle::Disabled(1u << 26) ? -1.0f : 1.0f;
    info.jitterOffset = {sign * jitter[0], sign * jitter[1]};
    info.motionVectorScale = {1.0f, 1.0f};
    info.renderSize = {w, h};
    info.outputSize = {ow, oh};
    info.frameTimeMilliseconds = frame_ms;
    info.preExposure = 1.0f;
    info.cameraNear = camera_motion.Near();
    info.cameraFar = 3000.0f;
    info.cameraVerticalFovRadians = camera_motion.VerticalFov();
    info.viewSpaceToMeters = 1.0f;
    info.sharpness = std::min(settings.sharpness.load(), 1.0f);
    info.enableSharpening = settings.sharpen ? VK_TRUE : VK_FALSE;
    info.reset = reset ? VK_TRUE : VK_FALSE;
    info.frameId = frame_id++;

    FfxVkPortableUpscaleCreateInfo create_info{};
    create_info.structSize = sizeof(create_info);
    create_info.maxRenderSize = {w, h};
    create_info.maxOutputSize = {ow, oh};
    bool ok = false;
    if (const u64 issues = ffxVkPortableValidateUpscaleDispatchInfo(&create_info, &info)) {
        PrintIssues("scaled dispatch", issues);
        failed = true;
    } else if (ffxVkPortableUpscaleContextRecordDispatch(context, &info) != FFX_VK_PORTABLE_OK) {
        std::printf("Upscaler: FSR 3 scaled dispatch failed\n");
        failed = true;
    } else {
        ok = true;
        reset = false;
        dispatched_last_frame = true;
        ExtraSharpen(cmdbuf, vk::Image(ui_image), true, ow, oh);
        // GOW3_DUMP_TRIGGER: the inputs, the motion with and without the characters' own and
        // the scene stencil (motion vector checks).
        if (const int dump = DumpFrame(); dump >= 0 && camera_motion.VelocityMode()) {
            DumpImages(instance, scheduler, cmdbuf, dump,
                       {{color_image, source_width, source_height, 4, "input", "rgba"},
                        {vk::Image(motion_image), w, h, 4, "motion", "rg16f"}});
            DumpImages(instance, scheduler, cmdbuf, dump,
                       {{depth_image, source_width, source_height, 4, "depth", "f32",
                         vk::ImageAspectFlagBits::eDepth}});
            DumpImages(instance, scheduler, cmdbuf, dump,
                       {{depth_image, source_width, source_height, 1, "stencil", "u8",
                         vk::ImageAspectFlagBits::eStencil}});
            barrier(vk::Image(motion_image), vk::ImageAspectFlagBits::eColor,
                    vk::ImageLayout::eGeneral, all, vk::AccessFlagBits2::eMemoryRead,
                    vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderWrite);
            camera_motion.RecordVelocity(cmdbuf, depth_view, *motion_view, w, h, false);
            DumpImages(instance, scheduler, cmdbuf, dump,
                       {{vk::Image(motion_image), w, h, 4, "camera", "rg16f"}});
        }
    }
    barrier(vk::Image(ui_image), vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eGeneral, all,
            rw, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            color_access);

    done_this_frame = true; // the UI is not jittered
    if (ok) {
        ui_phase = true;
        ui_color = ldr_target;
        ui_depth = camera_motion.Depth();
        DecodeToHdrUi(cmdbuf);
    }
}


// gow3: GOW3_HDR_TRACE=1: each (use, view format) pair on the output-size images, once.
static void HdrTraceUse(const char* use, vk::Format format) {
    static const bool trace = [] {
        const char* env = std::getenv("GOW3_HDR_TRACE");
        return env && env[0] == '1';
    }();
    if (!trace) {
        return;
    }
    static std::mutex mutex;
    static std::vector<std::pair<std::string, vk::Format>> seen;
    std::scoped_lock lock{mutex};
    for (const auto& [u, f] : seen) {
        if (u == use && f == format) {
            return;
        }
    }
    seen.emplace_back(use, format);
    std::printf("HDR trace: %s as %s\n", use, vk::to_string(format).c_str());
}
TemporalUpscaler::ShadowTarget TemporalUpscaler::HdrShadowTarget(HdrShadow which, u32 w,
                                                                 u32 h) {
    auto& shadow = which == HdrShadow::Tone ? tone_shadow : scene_shadow;
    if (!shadow.image || shadow.width != w || shadow.height != h) {
        const auto device = instance.GetDevice();
        // Preserve the command buffer bindings and dynamic state used by the shadow redraw.
        // Retire old resources only after their last GPU use.
        if (shadow.image) {
            scheduler.DeferOperation([image = std::move(shadow.image),
                                      view = std::move(shadow.view)]() mutable { view.reset(); });
        }
        shadow.image = VideoCore::UniqueImage(device, instance.GetAllocator());
        shadow.image.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = vk::Format::eR16G16B16A16Sfloat,
            .extent = {w, h, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
                     vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        shadow.view = Check(device.createImageViewUnique({
            .image = vk::Image(shadow.image),
            .viewType = vk::ImageViewType::e2D,
            .format = vk::Format::eR16G16B16A16Sfloat,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        shadow.width = w;
        shadow.height = h;
        std::printf("HDR: %s shadow %ux%u\n", which == HdrShadow::Tone ? "tone" : "scene", w, h);
    }
    return {vk::Image(shadow.image), *shadow.view};
}

void TemporalUpscaler::RecordHdrTransfer(vk::CommandBuffer cmd, HdrTransfer mode,
                                          vk::ImageView source, vk::ImageView target, u32 w,
                                          u32 h, float stored_scale) {
    if (!hdr_decode_pipeline) {
        const auto device = instance.GetDevice();
        std::array<vk::DescriptorSetLayoutBinding, 2> bindings{};
        for (u32 i = 0; i < bindings.size(); ++i) {
            bindings[i] = {.binding = i,
                           .descriptorType = vk::DescriptorType::eStorageImage,
                           .descriptorCount = 1,
                           .stageFlags = vk::ShaderStageFlagBits::eCompute};
        }
        hdr_transfer_desc_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = u32(bindings.size()),
            .pBindings = bindings.data(),
        }));
        const vk::PushConstantRange composition_range{
            .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(float)};
        hdr_transfer_pipeline_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*hdr_transfer_desc_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &composition_range,
        }));
        const auto compute = [&](const auto& code) {
            const auto module = CompileSPV(code, device);
            auto pipeline = Check(device.createComputePipelineUnique(
                {}, vk::ComputePipelineCreateInfo{
                        .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                                  .module = module,
                                  .pName = "main"},
                        .layout = *hdr_transfer_pipeline_layout,
                    }));
            device.destroyShaderModule(module);
            return pipeline;
        };
        hdr_decode_pipeline = compute(HDR_DECODE_COMP);
        hdr_encode_pipeline = compute(HDR_ENCODE_COMP);
        hdr_from_linear_pipeline = compute(HDR_FROM_LINEAR_COMP);
    }
    const std::array infos{
        vk::DescriptorImageInfo{.imageView = source, .imageLayout = vk::ImageLayout::eGeneral},
        vk::DescriptorImageInfo{.imageView = target, .imageLayout = vk::ImageLayout::eGeneral},
    };
    const std::array writes{
        vk::WriteDescriptorSet{.dstBinding = 0,
                               .descriptorCount = 1,
                               .descriptorType = vk::DescriptorType::eStorageImage,
                               .pImageInfo = &infos[0]},
        vk::WriteDescriptorSet{.dstBinding = 1,
                               .descriptorCount = 1,
                               .descriptorType = vk::DescriptorType::eStorageImage,
                               .pImageInfo = &infos[1]},
    };
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute,
                     mode == HdrTransfer::Decode   ? *hdr_decode_pipeline
                     : mode == HdrTransfer::Encode ? *hdr_encode_pipeline
                                                   : *hdr_from_linear_pipeline);
    cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *hdr_transfer_pipeline_layout, 0,
                             writes);
    cmd.pushConstants(*hdr_transfer_pipeline_layout, vk::ShaderStageFlagBits::eCompute,
                       0, sizeof(stored_scale), &stored_scale);
    cmd.dispatch((w + 7) / 8, (h + 7) / 8, 1);
}

void TemporalUpscaler::DecodeToHdrUi(vk::CommandBuffer cmd, vk::ImageView hdr_scene) {
    if (!hdr_chain) {
        return;
    }
    const auto& settings = Gow3Settings::Get();
    hdr_ui_white = float(settings.hdr_ui_nits.load());
    const float stored_scale = std::pow(float(settings.hdr_game_nits.load()) / hdr_ui_white,
                                        1.0f / 2.2f);
    // The decode reads ui_image's bytes through an RGBA8 UNORM storage view (it is mutable).
    if (!hdr_scene && ui_format != vk::Format::eR8G8B8A8Unorm &&
        ui_format != vk::Format::eR8G8B8A8Srgb) {
        hdr_chain = false;
        return;
    }
    const auto device = instance.GetDevice();
    if (!ui_hdr_image || ui_hdr_width != ui_width || ui_hdr_height != ui_height) {
        // The caller has already recorded into cmd. Finish would submit it and leave this
        // function writing to an ended command buffer. Retain old resources through this
        // submission instead, keeping cmd and the caller's bindings intact.
        if (ui_hdr_image) {
            scheduler.DeferOperation([image = std::move(ui_hdr_image),
                                      views = std::move(ui_hdr_views),
                                      storage = std::move(ui_hdr_storage_view)]() mutable {
                views.clear();
                storage.reset();
            });
        }
        ui_hdr_image = VideoCore::UniqueImage(device, instance.GetAllocator());
        ui_hdr_image.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = vk::Format::eR16G16B16A16Sfloat,
            .extent = {ui_width, ui_height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                     vk::ImageUsageFlagBits::eColorAttachment |
                     vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        ui_hdr_storage_view = Check(device.createImageViewUnique({
            .image = vk::Image(ui_hdr_image),
            .viewType = vk::ImageViewType::e2D,
            .format = vk::Format::eR16G16B16A16Sfloat,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        ui_hdr_width = ui_width;
        ui_hdr_height = ui_height;
    }
    if (!hdr_scene && !ui_decode_view) {
        const vk::ImageViewUsageCreateInfo usage{.usage = vk::ImageUsageFlagBits::eStorage};
        ui_decode_view = Check(device.createImageViewUnique({
            .pNext = &usage,
            .image = vk::Image(ui_image),
            .viewType = vk::ImageViewType::e2D,
            .format = vk::Format::eR8G8B8A8Unorm,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
    }
    const std::array to_decode{
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = vk::Image(ui_image),
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        },
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = vk::Image(ui_hdr_image),
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        },
    };
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = u32(to_decode.size()),
                          .pImageMemoryBarriers = to_decode.data()});
    if (hdr_scene) {
        RecordHdrTransfer(cmd, HdrTransfer::FromLinear, hdr_scene, *ui_hdr_storage_view, ui_width,
                          ui_height, stored_scale);
    } else {
        RecordHdrTransfer(cmd, HdrTransfer::Decode, *ui_decode_view, *ui_hdr_storage_view,
                          ui_width, ui_height, stored_scale);
    }
    const vk::ImageMemoryBarrier2 decoded{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentRead |
                         vk::AccessFlagBits2::eColorAttachmentWrite |
                         vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = vk::Image(ui_hdr_image),
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &decoded});
}

vk::ImageView TemporalUpscaler::Mirror(vk::Image image, std::vector<MirrorView>& views,
                                       vk::Format format, vk::ComponentMapping mapping) {
    for (const auto& entry : views) {
        if (entry.format == format && entry.mapping == mapping) {
            return *entry.view;
        }
    }
    const vk::ImageViewUsageCreateInfo usage{
        .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eColorAttachment,
    };
    auto view = Check(instance.GetDevice().createImageViewUnique({
        .pNext = &usage,
        .image = image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .components = mapping,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    const vk::ImageView handle = *view;
    views.push_back({format, mapping, std::move(view)});
    // gow3: GOW3_HDR_TRACE=1 lists the view formats the HUD and display passes use on the
    // output-size images (HDR10 planning).
    static const bool trace = [] {
        const char* env = std::getenv("GOW3_HDR_TRACE");
        return env && env[0] == '1';
    }();
    if (trace) {
        const char* kind = image == vk::Image(ui_image) ? "ui_image" : "display";
        std::printf("HDR trace: new %s view %s (identity mapping %d), ui_phase=%d\n", kind,
                    vk::to_string(format).c_str(), int(mapping == vk::ComponentMapping{}),
                    int(ui_phase));
    }
    return handle;
}

bool TemporalUpscaler::RedirectColor(VideoCore::ImageId color,
                                     const VideoCore::ImageViewInfo& view_info, Target& target) {
    redirected_float = false;
    if (ui_phase && color == ui_color) {
        if (hdr_chain && ui_hdr_image) {
            HdrTraceUse("HUD draw into ui_hdr_image", view_info.format);
            target = {Mirror(vk::Image(ui_hdr_image), ui_hdr_views,
                             vk::Format::eR16G16B16A16Sfloat, {}),
                      vk::ImageLayout::eGeneral, ui_width, ui_height, true};
            redirected_float = true;
            return true;
        }
        HdrTraceUse("HUD draw into ui_image", view_info.format);
        target = {Mirror(vk::Image(ui_image), ui_views, view_info.format, {}),
                  vk::ImageLayout::eGeneral, ui_width, ui_height, true};
        return true;
    }
    const auto& image = texture_cache.GetImage(color);
    const VAddr address = image.info.guest_address;
    if (!FrameCapture::IsDisplayBuffer(address)) {
        display_redirect = false;
        return false;
    }
    std::scoped_lock lock{display_mutex};
    if (!display_redirect) {
        displays[address].valid = false; // not upscaled: the presenter shows the guest buffer
        return false;
    }
    auto& display = HostDisplay(address, hdr_chain ? vk::Format::eR16G16B16A16Sfloat
                                                    : image.info.pixel_format);
    scheduler.EndRendering();
    // OnComposite has written the upscaled frame into it: the HUD draws keep it (no discard).
    const vk::ImageMemoryBarrier2 b{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = composite_frames ? vk::AccessFlagBits2::eMemoryWrite : vk::AccessFlagBits2::eNone,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentRead |
                         vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = composite_frames ? vk::ImageLayout::eGeneral : vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = vk::Image(display.image),
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    scheduler.Record([b](vk::CommandBuffer cmd) {
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
    });
    display.valid = true;
    HdrTraceUse("draw into display buffer", view_info.format);
    redirected_float = display.format == vk::Format::eR16G16B16A16Sfloat;
    target = {Mirror(vk::Image(display.image), display.views,
                     redirected_float ? display.format : view_info.format, {}),
              vk::ImageLayout::eGeneral, ui_width, ui_height};
    return true;
}

TemporalUpscaler::DisplayImage& TemporalUpscaler::HostDisplay(VAddr address, vk::Format format) {
    auto& display = displays[address];
    if (!display.image || display.format != format || display.width != ui_width ||
        display.height != ui_height) {
        const auto device = instance.GetDevice();
        scheduler.Finish();
        display.views.clear();
        display.storage_view.reset(); // destroy every view before replacing its image
        display.white_nits = 0.0f;
        std::printf("Display: host buffer %ux%u (live)\n", ui_width, ui_height);
        display.format = format;
        display.width = ui_width;
        display.height = ui_height;
        display.image = VideoCore::UniqueImage(device, instance.GetAllocator());
        display.image.Create(vk::ImageCreateInfo{
            .flags = vk::ImageCreateFlagBits::eMutableFormat,
            .imageType = vk::ImageType::e2D,
            .format = display.format,
            .extent = {ui_width, ui_height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
                     vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst |
                     (format == vk::Format::eR16G16B16A16Sfloat ? vk::ImageUsageFlagBits::eStorage
                                                                : vk::ImageUsageFlags{}),
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        display.views.clear();
        display.storage_view.reset();
    }
    return display;
}

void TemporalUpscaler::UpscaleToUi(VideoCore::ImageId source) {
    // The upscale writes ui_image (output size): FSR 3.1 / FSR 4 / DLSS / TAA with the scene's
    // depth and the velocity pass's camera. Without them (anti-aliasing off, menus without a 3D
    // scene, the upscaler failed) FSR 1 (EASU) upscales the frame when it is on, else a bilinear
    // stretch; the sharpening (RCAS) follows at the presenter, on the output-size frame. After
    // it, draws into the source go to ui_image at the output size (RedirectColor).
    ldr_target = source;
    if (Active() && !failed && camera_motion.Ready()) {
        RunScaled();
    }
    auto& ldr = texture_cache.GetImage(ldr_target);
    if (!ui_phase) {
        EnsureUiResources(target_width, target_height, LdrFormat(ldr.info.pixel_format),
                          vk::Format::eD32SfloatS8Uint);
        const auto [w, h] = SceneSize(ldr.info.size.width, ldr.info.size.height);
        // An sRGB source (the tonemapped frame) would be decoded by a blit into the UNORM
        // ui_image: FSR 1 reads it through a UNORM view instead.
        const bool srgb = LdrFormat(ldr.info.pixel_format) != ldr.info.pixel_format;
        const bool fsr1 = (Gow3Settings::Get().fsr1 || srgb) && w < ui_width && h < ui_height;
        if (fsr1) {
            runtime.Transit(&ldr, vk::ImageLayout::eShaderReadOnlyOptimal,
                            vk::PipelineStageFlagBits2::eComputeShader,
                            vk::AccessFlagBits2::eShaderRead);
        } else {
            runtime.Transit(&ldr, vk::ImageLayout::eTransferSrcOptimal,
                            vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
        }
        runtime.FlushBarriers();
        const vk::ImageView ldr_view =
            fsr1 ? CachedView(ldr, LdrFormat(ldr.info.pixel_format), vk::ImageAspectFlagBits::eColor)
                 : vk::ImageView{};
        scheduler.EndRendering();
        const auto cmd = scheduler.CommandBuffer();
        const vk::ImageMemoryBarrier2 to_dst{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = vk::Image(ui_image),
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        if (fsr1) {
            if (!fsr1_created) {
                fsr1_pass.Create(instance.GetDevice(), instance.GetAllocator(), 2);
                fsr1_created = true;
            }
            // EASU on the game's sRGB-encoded values (FSR 1's intended space), into a float
            // image; the blit to ui_image (UNORM) converts at the same size without encoding.
            fsr1_pass.Render(cmd, ldr_view, {w, h}, {ui_width, ui_height},
                             {.enable = true, .use_rcas = false}, false,
                             {ldr.info.size.width, ldr.info.size.height});
            const std::array to_copy{
                vk::ImageMemoryBarrier2{
                    .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                    .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
                    .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                    .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
                    .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                    .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                    .image = fsr1_pass.LastOutput(),
                    .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
                },
                to_dst,
            };
            cmd.pipelineBarrier2({.imageMemoryBarrierCount = u32(to_copy.size()),
                                  .pImageMemoryBarriers = to_copy.data()});
            const vk::ImageBlit same{
                .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .srcOffsets = std::array{vk::Offset3D{0, 0, 0},
                                         vk::Offset3D{s32(ui_width), s32(ui_height), 1}},
                .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .dstOffsets = std::array{vk::Offset3D{0, 0, 0},
                                         vk::Offset3D{s32(ui_width), s32(ui_height), 1}},
            };
            cmd.blitImage(fsr1_pass.LastOutput(), vk::ImageLayout::eTransferSrcOptimal,
                          vk::Image(ui_image), vk::ImageLayout::eGeneral, same, vk::Filter::eNearest);
        } else {
            cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_dst});
            const vk::ImageBlit stretch{
                .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .srcOffsets = std::array{vk::Offset3D{0, 0, 0}, vk::Offset3D{s32(w), s32(h), 1}},
                .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .dstOffsets = std::array{vk::Offset3D{0, 0, 0},
                                         vk::Offset3D{s32(ui_width), s32(ui_height), 1}},
            };
            cmd.blitImage(ldr.GetImage(), vk::ImageLayout::eTransferSrcOptimal, vk::Image(ui_image),
                          vk::ImageLayout::eGeneral, stretch, vk::Filter::eLinear);
        }
        reset = true; // the history doesn't continue across stretched frames
        done_this_frame = true;
        ui_phase = true;
        ui_color = ldr_target;
        ui_depth = {};
        DecodeToHdrUi(cmd);
    }
}

bool TemporalUpscaler::OnComposite(u64 ps_hash, VideoCore::ImageId color) {
    if (!composite_frames || !color) {
        return false;
    }
    // The tonemap draw finishes the frame in its target; Uncharted 3 then draws world-anchored
    // prompts (the journal, the d-pad) into it before the copies. The draw after the tonemap
    // upscales the clean frame, so those prompts are drawn over it at the output size.
    if (tonemap_pending) {
        tonemap_pending = false;
        UpscaleToUi(tonemap_target);
    }
    if (tonemap_ps && ps_hash == tonemap_ps && !ui_phase) {
        tonemap_target = color;
        tonemap_pending = true;
        return false;
    }
    if (ps_hash != composite_ps) {
        return false;
    }
    const auto& target = texture_cache.GetImage(color);
    const VAddr address = target.info.guest_address;
    if (!FrameCapture::IsDisplayBuffer(address)) {
        return false;
    }
    if (!ui_phase) {
        // No tonemap this frame: the copy's source, as it is.
        if (!ldr_target) {
            return false;
        }
        UpscaleToUi(ldr_target);
    }
    // ui_image -> the host display buffer, byte for byte: the game's copy already holds the
    // sRGB-encoded values its composite writes through an sRGB buffer unchanged (a blit would
    // encode them a second time: a washed-out picture).
    std::scoped_lock lock{display_mutex};
    const bool hdr = hdr_chain && ui_hdr_image;
    auto& display =
        HostDisplay(address, hdr ? vk::Format::eR16G16B16A16Sfloat : target.info.pixel_format);
    scheduler.EndRendering();
    const auto cmd = scheduler.CommandBuffer();
    if (hdr) {
        display.white_nits = hdr_ui_white;
        // gow3: HDR10 chain: the linear frame with its HUD, encoded to the stored values the
        // display pass's UNORM views blend (the copy below, without the 8-bit limit).
        if (!display.storage_view) {
            const vk::ImageViewUsageCreateInfo usage{.usage = vk::ImageUsageFlagBits::eStorage};
            display.storage_view = Check(instance.GetDevice().createImageViewUnique({
                .pNext = &usage,
                .image = vk::Image(display.image),
                .viewType = vk::ImageViewType::e2D,
                .format = vk::Format::eR16G16B16A16Sfloat,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            }));
        }
        const std::array to_encode{
            vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eGeneral,
                .image = vk::Image(ui_hdr_image),
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            },
            vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryRead,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eGeneral,
                .image = vk::Image(display.image),
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            },
        };
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = u32(to_encode.size()),
                              .pImageMemoryBarriers = to_encode.data()});
        RecordHdrTransfer(cmd, HdrTransfer::Encode, *ui_hdr_storage_view, *display.storage_view,
                          ui_width, ui_height);
        const vk::ImageMemoryBarrier2 encoded{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentRead |
                             vk::AccessFlagBits2::eColorAttachmentWrite |
                             vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = vk::Image(display.image),
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &encoded});
        display.valid = true;
        composite_done = true;
        return true;
    }
    const std::array before{
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = vk::Image(ui_image),
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        },
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = vk::Image(display.image),
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        },
    };
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = u32(before.size()),
                          .pImageMemoryBarriers = before.data()});
    const vk::ImageCopy copy{
        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .extent = {ui_width, ui_height, 1},
    };
    cmd.copyImage(vk::Image(ui_image), vk::ImageLayout::eGeneral, vk::Image(display.image),
                  vk::ImageLayout::eTransferDstOptimal, copy);
    const vk::ImageMemoryBarrier2 after{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentRead |
                         vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eTransferDstOptimal,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = vk::Image(display.image),
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &after});
    display.valid = true;
    composite_done = true;
    return true;
}

bool TemporalUpscaler::RedirectDepth(VideoCore::ImageId depth, Target& target) {
    if (!ui_phase || depth != ui_depth) {
        return false;
    }
    target = {*ui_depth_view, vk::ImageLayout::eGeneral, ui_width, ui_height, true};
    return true;
}

bool TemporalUpscaler::RedirectSampled(VideoCore::ImageId image,
                                       const VideoCore::ImageViewInfo& info, vk::ImageView& view,
                                       vk::ImageLayout& layout) {
    if (!display_redirect || image != ui_color) {
        return false;
    }
    if (!ui_read_barrier) {
        ui_read_barrier = true;
        scheduler.EndRendering();
        const vk::ImageMemoryBarrier2 b{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = hdr_chain && ui_hdr_image ? vk::Image(ui_hdr_image) : vk::Image(ui_image),
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        scheduler.Record([b](vk::CommandBuffer cmd) {
            cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
        });
    }
    HdrTraceUse("sampled read of ui_image", info.format);
    view = hdr_chain && ui_hdr_image
               ? Mirror(vk::Image(ui_hdr_image), ui_hdr_views, vk::Format::eR16G16B16A16Sfloat,
                        info.mapping)
               : Mirror(vk::Image(ui_image), ui_views, info.format, info.mapping);
    layout = vk::ImageLayout::eGeneral;
    return true;
}

bool TemporalUpscaler::DisplayOverride(VAddr address, Display& display) {
    std::scoped_lock lock{display_mutex};
    const auto it = displays.find(address);
    if (it == displays.end() || !it->second.valid) {
        return false;
    }
    display = {vk::Image(it->second.image), it->second.format, it->second.width, it->second.height,
               it->second.white_nits};
    // Diagnostic capture of the actual completed display buffer, including native UI.
    // GOW3_PRESENT_DUMP_COUNT=N: the trigger dumps N consecutive frames (temporal stability).
    static const char* dump = std::getenv("GOW3_PRESENT_DUMP_TRIGGER");
    static const int dump_count = [] {
        const char* env = std::getenv("GOW3_PRESENT_DUMP_COUNT");
        return env ? std::max(1, std::atoi(env)) : 1;
    }();
    static int dump_index = 0;
    static int dump_remaining = 0;
    // gow3: the trigger file is looked for at most every 250 ms, not with a kernel call per frame.
    static auto dump_poll = std::chrono::steady_clock::time_point{};
    if (dump && dump_remaining == 0) {
        if (const auto now = std::chrono::steady_clock::now();
            now - dump_poll >= std::chrono::milliseconds(250)) {
            dump_poll = now;
            if (std::remove(dump) == 0) {
                dump_remaining = dump_count;
            }
        }
    }
    if (dump_remaining > 0) {
        --dump_remaining;
        scheduler.EndRendering();
        const bool bgra = display.format == vk::Format::eB8G8R8A8Unorm ||
                          display.format == vk::Format::eB8G8R8A8Srgb;
        DumpImages(instance, scheduler, scheduler.CommandBuffer(), dump_index++,
                   {{display.image, display.width, display.height, 4, "present",
                     bgra ? "bgra" : "rgba"}});
    }
    return true;
}

} // namespace Vulkan

namespace Vulkan {

bool TemporalUpscaler::UseFsr4() const {
    const int selected = Gow3Settings::Get().upscaler;
    const bool supported = selected == Gow3Settings::UpscalerFsr411
                               ? instance.IsFsr411Supported()
                               : instance.IsFsr4Int8Supported();
    return Gow3Settings::IsFsr4(selected) && supported && !fsr4_failed;
}

bool TemporalUpscaler::RecordFsr4(vk::CommandBuffer cmdbuf, Fsr4Upscaler::Image color,
                                  Fsr4Upscaler::Image depth, u32 w, u32 h, u32 ow, u32 oh,
                                  float frame_ms) {
    const auto& settings = Gow3Settings::Get();
    // Same jitter convention as FSR 3; the menu (or toggle 1 << 26) flips it for tests.
    const float sign =
        Gow3Toggle::Disabled(1u << 26) != settings.fsr4_invert_jitter.load() ? -1.0f : 1.0f;
    const bool ok = fsr4->Record({
        .cmdbuf = cmdbuf,
        .color = color,
        .depth = depth,
        .motion = {vk::Image(motion_image), *motion_view, w, h},
        .output = {vk::Image(output_image), *output_view, ow, oh},
        .render_width = w,
        .render_height = h,
        .preset = applied_preset,
        .jitter = {sign * jitter[0], sign * jitter[1]},
        .frame_ms = frame_ms,
        .near_plane = camera_motion.Near(),
        .far_plane = 3000.0f,
        .vertical_fov = camera_motion.VerticalFov(),
        .sharpness = std::min(settings.sharpness.load(), 1.0f),
        .sharpen = settings.sharpen,
        .reset = reset,
        .auto_exposure = settings.fsr4_auto_exposure,
    });
    // The menu shows the reason; it outlives this frame (FSR 4 keeps its last message).
    static std::string shown;
    const char* problem = fsr4->Problem();
    if (!problem) {
        Gow3Settings::Get().fsr4_problem = nullptr;
    } else if (shown != problem) {
        shown = problem;
        static std::array<std::string, 8> kept;
        static u32 next = 0;
        kept[next] = shown;
        Gow3Settings::Get().fsr4_problem = kept[next].c_str();
        next = (next + 1) % kept.size();
    }
    if (!ok && fsr4->Fatal()) {
        std::printf("Upscaler: falling back to FSR 3.1\n");
        Gow3Settings::Get().upscaler = Gow3Settings::UpscalerFsr3;
        fsr4_failed = true; // EnsureResources creates the FSR 3 context next frame
        reset = true;
    }
    return ok;
}

bool TemporalUpscaler::UseDlss() const {
    const Dlss* dlss = Dlss::Get();
    return Gow3Settings::Get().upscaler == Gow3Settings::UpscalerDlss && dlss && dlss->Available() &&
           !dlss_failed;
}

bool TemporalUpscaler::RecordDlss(vk::CommandBuffer cmdbuf, const Dlss::Resource& color,
                                  const Dlss::Resource& depth, u32 w, u32 h, u32 ow, u32 oh,
                                  float frame_ms, bool hdr) {
    Dlss* dlss = Dlss::Get();
    const Dlss::FeatureDesc desc{w, h, ow, oh, Dlss::QualityForScale(float(ow) / float(w)), hdr};
    bool ok = true;
    if (!dlss->HasFeature(desc)) {
        // The previous feature may still be in use by submitted work; `cmdbuf` stays open.
        scheduler.WaitSubmitted();
        dlss->ReleaseFeature();
        ok = dlss->CreateFeature(cmdbuf, desc);
        reset = true;
    }
    if (ok) {
        // The jitter and motion vectors FSR 3 gets: render pixels, current to previous.
        const float sign = Gow3Toggle::Disabled(1u << 26) ? -1.0f : 1.0f;
        const auto& settings = Gow3Settings::Get();
        ok = dlss->Evaluate(cmdbuf, {
            .color = color,
            .depth = depth,
            .motion = {vk::Image(motion_image), *motion_view, vk::Format::eR16G16Sfloat,
                       vk::ImageAspectFlagBits::eColor, w, h},
            .output = {vk::Image(output_image), *output_view, vk::Format::eR16G16B16A16Sfloat,
                       vk::ImageAspectFlagBits::eColor, ow, oh},
            .jitter_x = sign * jitter[0],
            .jitter_y = sign * jitter[1],
            .reset = reset,
            .frame_ms = frame_ms,
            .sharpness = settings.sharpen ? std::min(settings.sharpness.load(), 1.0f) : 0.0f,
        });
    }
    if (!ok) {
        std::printf("Upscaler: DLSS failed; falling back to FSR 3.1\n");
        Gow3Settings::Get().upscaler = Gow3Settings::UpscalerFsr3;
        dlss_failed = true; // EnsureResources creates the FSR 3 context next frame
        reset = true;
    }
    return ok;
}

} // namespace Vulkan
