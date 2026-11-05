// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

// gow3: the HDR10 session's start-up choices, without Vulkan (tests/test_hdr_session.cpp).
namespace Vulkan::HdrSession {

// The SDR swapchain format: an 8-bit one in sRGB nonlinear, another 8-bit one only if the
// surface offers none. With the color space extension a surface may list the 8-bit formats in
// other color spaces too. Null when there is no 8-bit format.
template <typename SurfaceFormat, typename Format, typename ColorSpace>
const SurfaceFormat* PickSdrFormat(std::span<const SurfaceFormat> formats,
                                   std::initializer_list<Format> eight_bit, ColorSpace srgb) {
    const auto usable = [&](const SurfaceFormat& f) {
        return std::find(eight_bit.begin(), eight_bit.end(), f.format) != eight_bit.end();
    };
    for (const bool srgb_only : {true, false}) {
        for (const SurfaceFormat& f : formats) {
            if (usable(f) && (!srgb_only || f.colorSpace == srgb)) {
                return &f;
            }
        }
    }
    return nullptr;
}

// The launcher's 1:1 composite path for HDR at the game's own size (GOW3_COMPOSITE_NATIVE=1)
// stays only with HDR output.
inline bool DropNativeComposite(const char* native, bool hdr_output) {
    return !hdr_output && native && native[0] == '1';
}

// The variables to remove then: those the launcher listed (GOW3_COMPOSITE_NATIVE_ENV, comma
// separated) and GOW3_COMPOSITE_NATIVE itself.
inline std::vector<std::string> NativeCompositeVariables(const char* listed) {
    std::vector<std::string> names;
    const std::string text = std::string(listed ? listed : "") + ",GOW3_COMPOSITE_NATIVE";
    for (size_t start = 0; start <= text.size();) {
        const size_t end = std::min(text.find(',', start), text.size());
        if (end > start) {
            names.push_back(text.substr(start, end - start));
        }
        start = end + 1;
    }
    return names;
}

// The tone pass replacement is the shader as translated with manual interpolation: the NVIDIA
// driver's profile with VK_KHR_fragment_shader_barycentric (PipelineCache,
// needs_manual_interpolation). Elsewhere the scene stays SDR inside HDR10 output.
constexpr bool ShadowShaderUsable(bool fragment_shader_barycentric, bool nvidia_driver) {
    return fragment_shader_barycentric && nvidia_driver;
}

} // namespace Vulkan::HdrSession
