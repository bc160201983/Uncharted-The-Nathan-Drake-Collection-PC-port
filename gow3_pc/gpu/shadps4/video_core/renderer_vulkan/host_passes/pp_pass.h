//  SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Frame;
}

namespace Vulkan::HostPasses {

class PostProcessingPass {
public:
    /// gow3: `hdr` 0 = SDR, 1 = the guest's own PQ image, 2 = HDR10 encode of the game's image
    /// (gamma 2.2 display-referred, 1.0 = `game_nits`). `ui_nits` is the menu overlay's white.
    struct Settings {
        float gamma = 1.0f;
        u32 hdr = 0;
        u32 srgb_input = 0;
        float game_nits = 203.0f;
        float peak_nits = 1000.0f;
        float ui_nits = 203.0f;
    };

    static constexpr u32 HdrGuestPq = 1;
    static constexpr u32 HdrEncode = 2;

    void Create(vk::Device device, vk::Format surface_format, vk::Format hdr_format);

    void Render(vk::CommandBuffer cmdbuf, vk::ImageView input, vk::Extent2D input_size,
                Frame& output, Settings settings);

    /// Blends `overlay` (8-bit, premultiplied by ImGui's blending into transparent black) onto
    /// the HDR10 `target`, which must be in ColorAttachmentOptimal.
    void RenderOverlay(vk::CommandBuffer cmdbuf, vk::ImageView overlay, vk::ImageView target,
                       vk::Extent2D extent, const Settings& settings);

private:
    void Draw(vk::CommandBuffer cmdbuf, vk::Pipeline target_pipeline, vk::ImageView input,
              vk::ImageView output, vk::Extent2D extent, vk::AttachmentLoadOp load,
              const Settings& settings);

    vk::UniquePipeline pipeline{};
    vk::UniquePipeline hdr_pipeline{};
    vk::UniquePipeline overlay_pipeline{};
    vk::UniquePipelineLayout pipeline_layout{};
    vk::UniqueDescriptorSetLayout desc_set_layout{};
    vk::UniqueSampler sampler{};
};

} // namespace Vulkan::HostPasses
