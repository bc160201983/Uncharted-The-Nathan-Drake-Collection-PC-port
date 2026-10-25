// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
#include "gpu/shadps4/video_core/renderer_vulkan/hdr_session.h"

namespace {
enum Format { Rgba8 = 37, Bgra8 = 44, A2b10 = 64, Rgba16f = 97 };
enum ColorSpace { SrgbNonlinear = 0, ExtendedSrgbLinear = 1000104002, Hdr10 = 1000104008 };
struct SurfaceFormat {
    Format format;
    ColorSpace colorSpace;
};

const SurfaceFormat* Pick(std::span<const SurfaceFormat> formats) {
    return Vulkan::HdrSession::PickSdrFormat(formats, {Rgba8, Bgra8}, SrgbNonlinear);
}
} // namespace

int main() {
    using namespace Vulkan::HdrSession;
    // An HDR display listing the 8-bit format in another color space first: sRGB nonlinear wins.
    const std::array hdr_display{SurfaceFormat{A2b10, Hdr10}, SurfaceFormat{Bgra8, ExtendedSrgbLinear},
                                 SurfaceFormat{Rgba16f, ExtendedSrgbLinear},
                                 SurfaceFormat{Bgra8, SrgbNonlinear}};
    const auto* picked = Pick(hdr_display);
    assert(picked == &hdr_display[3]);
    // No 8-bit sRGB nonlinear format: any 8-bit one, as before.
    const std::array other_space{SurfaceFormat{Rgba16f, ExtendedSrgbLinear},
                                 SurfaceFormat{Rgba8, ExtendedSrgbLinear}};
    assert(Pick(other_space) == &other_space[1]);
    // The first matching format in the surface's order.
    const std::array both{SurfaceFormat{Rgba8, SrgbNonlinear}, SurfaceFormat{Bgra8, SrgbNonlinear}};
    assert(Pick(both) == &both[0]);
    const std::array none{SurfaceFormat{A2b10, Hdr10}};
    assert(Pick(none) == nullptr);

    // The 1:1 path goes only without HDR output, and only when the launcher asked for it.
    assert(DropNativeComposite("1", false));
    assert(!DropNativeComposite("1", true));
    assert(!DropNativeComposite(nullptr, false));
    assert(!DropNativeComposite("0", false));
    assert(!DropNativeComposite("", false));

    const std::vector<std::string> launcher{"GOW3_RENDER_RES", "GOW3_OUTPUT_RES",
                                            "GOW3_UI_TONEMAP_PS", "GOW3_COMPOSITE_NATIVE"};
    assert(NativeCompositeVariables("GOW3_RENDER_RES,GOW3_OUTPUT_RES,GOW3_UI_TONEMAP_PS") ==
           launcher);
    // Without the list (an older launcher): only the flag itself.
    assert(NativeCompositeVariables(nullptr) == std::vector<std::string>{"GOW3_COMPOSITE_NATIVE"});
    assert(NativeCompositeVariables("") == std::vector<std::string>{"GOW3_COMPOSITE_NATIVE"});
    assert(NativeCompositeVariables(",A,,B,") ==
           (std::vector<std::string>{"A", "B", "GOW3_COMPOSITE_NATIVE"}));

    // Scene HDR only on the NVIDIA barycentric profile (not GTX 10xx, not AMD/Intel).
    static_assert(ShadowShaderUsable(true, true));
    static_assert(!ShadowShaderUsable(false, true));
    static_assert(!ShadowShaderUsable(true, false));
    std::puts("HDR session: PASS (SDR format pick, native composite without HDR, shadow shader)");
}
