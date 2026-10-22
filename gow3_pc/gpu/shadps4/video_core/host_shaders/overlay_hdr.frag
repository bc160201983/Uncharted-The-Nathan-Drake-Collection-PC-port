// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: the settings menu / FPS counter, drawn into a cleared 8-bit image, blended onto the HDR10
// swapchain at the HUD brightness.

#version 450
#extension GL_GOOGLE_include_directive : require

#include "hdr_common.glsl"

layout (location = 0) in vec2 uv;
layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D overlay;

layout (push_constant) uniform settings {
    float gamma;
    uint hdr;
    uint srgb_input;
    float game_nits;
    float peak_nits;
    float ui_nits;
} pp;

void main() {
    // ImGui's blending into transparent black leaves premultiplied color and coverage.
    vec4 o = texture(overlay, uv);
    if (o.a <= 0.0) {
        discard;
    }
    vec3 encoded = clamp(o.rgb / o.a, 0.0, 1.0);
    color = vec4(Hdr10FromLinear(LinearFromGamma22(encoded), pp.ui_nits), o.a);
}
