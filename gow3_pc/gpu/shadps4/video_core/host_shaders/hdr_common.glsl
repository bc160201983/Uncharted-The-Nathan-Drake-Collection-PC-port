// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: HDR10 output helpers shared by the post process and the HDR overlay pass.

const mat3 BT709_TO_BT2020 = mat3(
    0.6274040, 0.0690970, 0.0163916,
    0.3292820, 0.9195400, 0.0880132,
    0.0433136, 0.0113612, 0.8955950);

// SMPTE ST 2084 inverse EOTF: absolute nits -> PQ signal.
vec3 PqFromNits(vec3 nits) {
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    vec3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(m1));
    return pow((c1 + c2 * y) / (1.0 + c3 * y), vec3(m2));
}

// The game's 8-bit values are display-referred for a gamma 2.2 monitor (sign kept above 1.0,
// where the HDR scene goes past SDR white).
vec3 LinearFromGamma22(vec3 encoded) {
    return sign(encoded) * pow(abs(encoded), vec3(2.2));
}

vec3 SrgbEncode(vec3 linear) {
    return mix(1.055 * pow(max(linear, 0.0), vec3(1.0 / 2.4)) - 0.055, linear * 12.92,
               lessThan(linear, vec3(0.0031308)));
}

// Rec. 709 linear, 1.0 = `white_nits` -> HDR10 signal.
vec3 Hdr10FromLinear(vec3 linear, float white_nits) {
    return PqFromNits(max(BT709_TO_BT2020 * (linear * white_nits), 0.0));
}
