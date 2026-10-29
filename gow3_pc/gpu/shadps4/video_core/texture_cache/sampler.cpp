// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>
#include "core/memory.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/texture_cache/sampler.h"

namespace VideoCore {

Sampler::Sampler(const Vulkan::Instance& instance, const AmdGpu::Sampler& sampler,
                 const AmdGpu::BorderColorBuffer border_color_base, const bool is_depth, float extra_lod_bias) {
    using namespace Vulkan;
    // gow3 29 Oct 2026 (graphics fidelity): GOW3_ANISOTROPY=<2..16> raises texture filtering for
    // mipmapped, linearly filtered colour textures (world surfaces seen at an angle) to at least
    // that ratio; depth/compare samplers, point-filtered and mip-less textures (UI, lookup tables,
    // render targets) keep the game's own setting. 0 or unset = the game's setting.
    static const float forced_aniso = [] {
        const char* env = std::getenv("GOW3_ANISOTROPY");
        const float value = env ? static_cast<float>(std::atof(env)) : 0.0f;
        return value >= 2.0f ? std::min(value, 16.0f) : 0.0f;
    }();
    const bool force_aniso = forced_aniso > 0.0f && !is_depth &&
                             sampler.xy_min_filter != AmdGpu::Filter::Point &&
                             sampler.xy_min_filter != AmdGpu::Filter::AnisoPoint &&
                             sampler.mip_filter != AmdGpu::MipFilter::None && sampler.MaxLod() > 0.0f;
    const bool anisotropy_enable = instance.IsAnisotropicFilteringSupported() &&
                                   (force_aniso || AmdGpu::IsAnisoFilter(sampler.xy_mag_filter) ||
                                    AmdGpu::IsAnisoFilter(sampler.xy_min_filter));
    const float max_anisotropy =
        anisotropy_enable
            ? std::clamp(force_aniso ? std::max(sampler.MaxAniso(), forced_aniso) : sampler.MaxAniso(),
                         1.0f, instance.MaxSamplerAnisotropy())
            : 1.0f;
    auto border_color = LiverpoolToVK::BorderColor(sampler.border_color_type);
    if (border_color == vk::BorderColor::eFloatCustomEXT &&
        !instance.IsCustomBorderColorSupported()) {
        LOG_WARNING(Render_Vulkan, "Custom border color is not supported, falling back to black");
        border_color = vk::BorderColor::eFloatOpaqueBlack;
    }

    // gow3 20 Oct 2026 (GitHub #56, Uncharted 2 on a Radeon 780M): a sampler asked for a custom
    // border color while the border color table's address was 0: the colour was read at
    // 0 + index * 16 and the port stopped (read access to 0xe4a0). It is now read through the
    // backing view (no fault); no table, or none mapped there: transparent black.
    std::array<float, 4> custom_border_color_array{};
    if (border_color == vk::BorderColor::eFloatCustomEXT) {
        const VAddr table = reinterpret_cast<VAddr>(border_color_base.Address<std::array<float, 4>*>());
        const VAddr entry = table + u64(sampler.border_color_ptr.Value()) * sizeof(custom_border_color_array);
        if (!table || !Core::Memory::Instance()->TryReadBacking(entry, custom_border_color_array.data(),
                                                                 sizeof(custom_border_color_array))) {
            static std::atomic<u32> logged{0};
            if (logged.fetch_add(1, std::memory_order_relaxed) < 4) {
                std::printf("GPU: custom border color %u without a readable table (base %#llx): "
                            "transparent black\n",
                            u32(sampler.border_color_ptr.Value()), static_cast<unsigned long long>(table));
                std::fflush(stdout);
            }
            border_color = vk::BorderColor::eFloatTransparentBlack;
        }
    }
    const auto custom_color = [&]() -> std::optional<vk::SamplerCustomBorderColorCreateInfoEXT> {
        if (border_color == vk::BorderColor::eFloatCustomEXT) {
            const vk::SamplerCustomBorderColorCreateInfoEXT ret{
                .customBorderColor =
                    vk::ClearColorValue{
                        .float32 = custom_border_color_array,
                    },
                .format = vk::Format::eR32G32B32A32Sfloat,
            };
            return ret;
        } else {
            return std::nullopt;
        }
    }();

    if (is_depth) {
        // gow3: each distinct shadow-lookup sampler setup (compare, clamp, border), once.
        static std::mutex mutex;
        static std::vector<u64> seen;
        const u64 key = u64(sampler.depth_compare_func.Value()) | (u64(sampler.clamp_x.Value()) << 4) |
                        (u64(sampler.clamp_y.Value()) << 8) | (u64(sampler.clamp_z.Value()) << 12) |
                        (u64(sampler.border_color_type.Value()) << 16) |
                        (u64(sampler.border_color_ptr.Value()) << 20);
        std::scoped_lock lock{mutex};
        if (std::find(seen.begin(), seen.end(), key) == seen.end()) {
            seen.push_back(key);
            std::printf("GPU: compare sampler: depth compare %u, clamp %u/%u/%u, border type %u ptr %u, "
                        "filter %u/%u mip %u\n",
                        u32(sampler.depth_compare_func.Value()), u32(sampler.clamp_x.Value()),
                        u32(sampler.clamp_y.Value()), u32(sampler.clamp_z.Value()),
                        u32(sampler.border_color_type.Value()), u32(sampler.border_color_ptr.Value()),
                        u32(sampler.xy_mag_filter.Value()), u32(sampler.xy_min_filter.Value()),
                        u32(sampler.mip_filter.Value()));
            std::fflush(stdout);
        }
    }
    // gow3 diagnostic overrides for compare samplers (sampler.h): 3 = clamp to an opaque black
    // border, 4 = clamp to a white border, 5 = clamp to the edge texel.
    const int override_mode = is_depth ? sampler_compare_override.load() : 0;
    if (override_mode == 3 || override_mode == 4) {
        border_color = override_mode == 3 ? vk::BorderColor::eFloatOpaqueBlack : vk::BorderColor::eFloatOpaqueWhite;
    }
    const auto address = [&](AmdGpu::ClampMode mode) {
        if (override_mode == 3 || override_mode == 4) {
            return vk::SamplerAddressMode::eClampToBorder;
        }
        if (override_mode == 5) {
            return vk::SamplerAddressMode::eClampToEdge;
        }
        return LiverpoolToVK::ClampMode(mode);
    };
    const vk::SamplerCreateInfo sampler_ci = {
        .pNext = custom_color && border_color == vk::BorderColor::eFloatCustomEXT ? &*custom_color : nullptr,
        .magFilter = LiverpoolToVK::Filter(sampler.xy_mag_filter),
        .minFilter = LiverpoolToVK::Filter(sampler.xy_min_filter),
        .mipmapMode = LiverpoolToVK::MipFilter(sampler.mip_filter),
        .addressModeU = address(sampler.clamp_x),
        .addressModeV = address(sampler.clamp_y),
        .addressModeW = address(sampler.clamp_z),
        .mipLodBias = std::clamp(sampler.LodBias() + extra_lod_bias, -instance.MaxSamplerLodBias(),
                                 instance.MaxSamplerLodBias()),
        .anisotropyEnable = anisotropy_enable,
        .maxAnisotropy = max_anisotropy,
        // GCN compares per instruction; a plain read of a compare sampler is undefined in Vulkan.
        .compareEnable = is_depth,
        .compareOp = sampler_compare_override == 1   ? vk::CompareOp::eNever
                     : sampler_compare_override == 2 ? vk::CompareOp::eAlways
                                                      : LiverpoolToVK::DepthCompare(sampler.depth_compare_func),
        .minLod = sampler.MinLod(),
        .maxLod = sampler.MaxLod(),
        .borderColor = border_color,
        .unnormalizedCoordinates = false, // Handled in shader due to Vulkan limitations.
    };
    auto [sampler_result, smplr] = instance.GetDevice().createSamplerUnique(sampler_ci);
    ASSERT_MSG(sampler_result == vk::Result::eSuccess, "Failed to create sampler: {}",
               vk::to_string(sampler_result));
    handle = std::move(smplr);
}

Sampler::~Sampler() = default;

} // namespace VideoCore
