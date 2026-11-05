// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450
#extension GL_GOOGLE_include_directive : require

#include "hdr_common.glsl"

layout (location = 0) in vec2 uv;
layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D texSampler;

// gow3: hdr 0 = SDR, 1 = the guest's own PQ image, 2 = HDR10 encode of the game's image.
layout (push_constant) uniform settings {
    float gamma;
    uint hdr;
    uint srgb_input;
    float game_nits;
    float peak_nits;
    float ui_nits;
} pp;

const float cutoff = 0.0031308, a = 1.055, b = 0.055, d = 12.92;
vec3 gamma(vec3 rgb) {
    return mix(
        a * pow(rgb, vec3(1.0 / (2.4 + 1.0 - pp.gamma))) - b,
        d * rgb / pp.gamma,
        lessThan(rgb, vec3(cutoff))
    );
}

// Exact inverse of gamma() at unit gamma, for buffers that are sRGB encoded but must be sampled
// through a UNORM view because Vulkan has no sRGB variant of the 10-bit format.
vec3 degamma(vec3 rgb) {
    return mix(
        pow(max(rgb + b, 0.0) / a, vec3(2.4)),
        rgb / d,
        lessThan(rgb, vec3(d * cutoff))
    );
}

void main() {
    vec4 color_linear = texture(texSampler, uv);
    if (pp.hdr == 1u) {
        color = color_linear;
    } else if (pp.hdr == 2u) {
        // The display buffer is read through an sRGB view (or RCAS's float copy of it): undo that
        // decode to get the game's stored values, which it made for a gamma 2.2 display.
        vec3 stored = pp.srgb_input != 0u ? color_linear.rgb : SrgbEncode(color_linear.rgb);
        color = vec4(Hdr10FromLinear(LinearFromGamma22(stored), pp.game_nits), 1.0);
    } else {
        if (pp.srgb_input != 0u) color_linear.rgb = degamma(color_linear.rgb);
        color = vec4(gamma(color_linear.rgb), color_linear.a);
    }
}
