// Uncharted 2 HDR tone replacement. Live settings use binding 12; guest bindings 0-9 stay intact.
[[vk::binding(12, 0)]] ByteAddressBuffer HdrSettings;
#define RENODX_PEAK_WHITE_NITS asfloat(HdrSettings.Load(0))
#define RENODX_DIFFUSE_WHITE_NITS asfloat(HdrSettings.Load(4))
#include "draw.hlsl"
ByteAddressBuffer ssbo_1_1 : register(t0, space0);
ByteAddressBuffer ssbo_2_1 : register(t1, space0);
struct AuxData
{
    float push_data_xoffset;
    float push_data_yoffset;
    float push_data_xscale;
    float push_data_yscale;
    uint4 push_data_ud_regs0;
    uint4 push_data_ud_regs1;
    uint4 push_data_ud_regs2;
    uint4 push_data_ud_regs3;
    uint4 push_data_buf_offsets0;
    uint4 push_data_buf_offsets1;
    uint2 push_data_buf_offsets2;
    uint push_data_motion_param;
    uint push_data_scene_size;
};

[[vk::push_constant]] ConstantBuffer<AuxData> push_constants;
Texture2D<float4> fs_img0 : register(t2, space0);
SamplerState fs_samp0 : register(s8, space0);
Texture2D<float4> fs_img1 : register(t3, space0);
SamplerState fs_samp1 : register(s9, space0);
Texture2D<float4> fs_img2 : register(t4, space0);
Texture2D<float4> fs_img3 : register(t5, space0);
Texture3D<float4> fs_img4 : register(t6, space0);
Texture2D<float4> fs_img5 : register(t7, space0);

static float3 gl_BaryCoordEXT;
static float4 fs_in_attr0_p[3];
static float4 frag_color0;
static float4 renodx_pretonemap;
static float4 renodx_hdr;

struct SPIRV_Cross_Input
{
    nointerpolation float4 fs_in_attr0_p : TEXCOORD0;
    float3 gl_BaryCoordEXT : SV_Barycentrics;
};

struct SPIRV_Cross_Output
{
    float4 frag_color0 : SV_Target0;
    float4 renodx_pretonemap : SV_Target1;
    float4 renodx_hdr : SV_Target2;
};

uint spvBitfieldUExtract(uint Base, uint Offset, uint Count)
{
    uint Mask = Count == 32 ? 0xffffffff : ((1 << Count) - 1);
    return (Base >> Offset) & Mask;
}

uint2 spvBitfieldUExtract(uint2 Base, uint Offset, uint Count)
{
    uint Mask = Count == 32 ? 0xffffffff : ((1 << Count) - 1);
    return (Base >> Offset) & Mask;
}

uint3 spvBitfieldUExtract(uint3 Base, uint Offset, uint Count)
{
    uint Mask = Count == 32 ? 0xffffffff : ((1 << Count) - 1);
    return (Base >> Offset) & Mask;
}

uint4 spvBitfieldUExtract(uint4 Base, uint Offset, uint Count)
{
    uint Mask = Count == 32 ? 0xffffffff : ((1 << Count) - 1);
    return (Base >> Offset) & Mask;
}

int spvBitfieldSExtract(int Base, int Offset, int Count)
{
    int Mask = Count == 32 ? -1 : ((1 << Count) - 1);
    int Masked = (Base >> Offset) & Mask;
    int ExtendShift = (32 - Count) & 31;
    return (Masked << ExtendShift) >> ExtendShift;
}

int2 spvBitfieldSExtract(int2 Base, int Offset, int Count)
{
    int Mask = Count == 32 ? -1 : ((1 << Count) - 1);
    int2 Masked = (Base >> Offset) & Mask;
    int ExtendShift = (32 - Count) & 31;
    return (Masked << ExtendShift) >> ExtendShift;
}

int3 spvBitfieldSExtract(int3 Base, int Offset, int Count)
{
    int Mask = Count == 32 ? -1 : ((1 << Count) - 1);
    int3 Masked = (Base >> Offset) & Mask;
    int ExtendShift = (32 - Count) & 31;
    return (Masked << ExtendShift) >> ExtendShift;
}

int4 spvBitfieldSExtract(int4 Base, int Offset, int Count)
{
    int Mask = Count == 32 ? -1 : ((1 << Count) - 1);
    int4 Masked = (Base >> Offset) & Mask;
    int ExtendShift = (32 - Count) & 31;
    return (Masked << ExtendShift) >> ExtendShift;
}

// Native filmic curve: (x * c5 + c6) / (x * (x + c0) + c1) + c4, before its 0-1 clamp.
float RenoDXNativeFilmic(float x, float c0, float c1, float c4, float c5, float c6)
{
    return ((x * c5) + c6) / ((x * (x + c0)) + c1) + c4;
}

// Gain that maps scene-linear input onto the native filmic response at the scene value
// whose filmic output is linear mid-grey. Falls back to the gain at scene mid-grey if the
// curve does not bracket mid-grey on [0, 64].
float RenoDXFilmicAnchorGain(float c0, float c1, float c4, float c5, float c6)
{
    const float target = 0.18f;
    float lo = 0.0f;
    float hi = 64.0f;
    if (RenoDXNativeFilmic(lo, c0, c1, c4, c5, c6) >= target || RenoDXNativeFilmic(hi, c0, c1, c4, c5, c6) <= target)
    {
        return max(RenoDXNativeFilmic(target, c0, c1, c4, c5, c6), 0.0f) / target;
    }
    [loop]
    for (int i = 0; i < 32; i++)
    {
        float mid = 0.5f * (lo + hi);
        if (RenoDXNativeFilmic(mid, c0, c1, c4, c5, c6) < target)
        {
            lo = mid;
        }
        else
        {
            hi = mid;
        }
    }
    return target / (0.5f * (lo + hi));
}

void frag_main()
{
    uint buf0_dword_off = spvBitfieldUExtract(push_constants.push_data_buf_offsets0.x, 0, 8) >> 2u;
    precise float _38 = fs_in_attr0_p[1].x - fs_in_attr0_p[0].x;
    float _RESERVED_IDENTIFIER_FIXUP_116 = _38;
    precise float _45 = fs_in_attr0_p[1].y - fs_in_attr0_p[0].y;
    float _RESERVED_IDENTIFIER_FIXUP_123 = _45;
    precise float _52 = fs_in_attr0_p[2].x - fs_in_attr0_p[0].x;
    float _RESERVED_IDENTIFIER_FIXUP_129 = _52;
    float _RESERVED_IDENTIFIER_FIXUP_130 = mad(_RESERVED_IDENTIFIER_FIXUP_129, gl_BaryCoordEXT.z, mad(_RESERVED_IDENTIFIER_FIXUP_116, gl_BaryCoordEXT.y, fs_in_attr0_p[0].x));
    precise float _72 = fs_in_attr0_p[2].y - fs_in_attr0_p[0].y;
    float _RESERVED_IDENTIFIER_FIXUP_135 = _72;
    float _RESERVED_IDENTIFIER_FIXUP_136 = mad(_RESERVED_IDENTIFIER_FIXUP_135, gl_BaryCoordEXT.z, mad(_RESERVED_IDENTIFIER_FIXUP_123, gl_BaryCoordEXT.y, fs_in_attr0_p[0].y));
    float4 _RESERVED_IDENTIFIER_FIXUP_141 = fs_img0.Sample(fs_samp0, float2(_RESERVED_IDENTIFIER_FIXUP_130, _RESERVED_IDENTIFIER_FIXUP_136));
    float _RESERVED_IDENTIFIER_FIXUP_142 = _RESERVED_IDENTIFIER_FIXUP_141.x;
    float _RESERVED_IDENTIFIER_FIXUP_143 = _RESERVED_IDENTIFIER_FIXUP_141.y;
    float _RESERVED_IDENTIFIER_FIXUP_144 = _RESERVED_IDENTIFIER_FIXUP_141.z;
    uint _RESERVED_IDENTIFIER_FIXUP_146 = 88u + buf0_dword_off;
    float _RESERVED_IDENTIFIER_FIXUP_208;
    float _RESERVED_IDENTIFIER_FIXUP_209;
    float _RESERVED_IDENTIFIER_FIXUP_210;
    if ((ssbo_1_1.Load<uint>(_RESERVED_IDENTIFIER_FIXUP_146 * 4 + 0) & 2u) != 0u)
    {
        float4 _RESERVED_IDENTIFIER_FIXUP_155 = fs_img1.Sample(fs_samp1, float2(_RESERVED_IDENTIFIER_FIXUP_130, _RESERVED_IDENTIFIER_FIXUP_136));
        float _RESERVED_IDENTIFIER_FIXUP_156 = _RESERVED_IDENTIFIER_FIXUP_155.x;
        float _RESERVED_IDENTIFIER_FIXUP_157 = _RESERVED_IDENTIFIER_FIXUP_155.y;
        float _RESERVED_IDENTIFIER_FIXUP_158 = _RESERVED_IDENTIFIER_FIXUP_155.z;
        float _RESERVED_IDENTIFIER_FIXUP_159 = _RESERVED_IDENTIFIER_FIXUP_155.w;
        float4 _RESERVED_IDENTIFIER_FIXUP_164 = fs_img2.Sample(fs_samp0, float2(_RESERVED_IDENTIFIER_FIXUP_130, _RESERVED_IDENTIFIER_FIXUP_136));
        float _RESERVED_IDENTIFIER_FIXUP_165 = _RESERVED_IDENTIFIER_FIXUP_164.x;
        bool _RESERVED_IDENTIFIER_FIXUP_168 = (ssbo_1_1.Load<uint>(_RESERVED_IDENTIFIER_FIXUP_146 * 4 + 0) & 128u) != 0u;
        float _RESERVED_IDENTIFIER_FIXUP_183;
        float _RESERVED_IDENTIFIER_FIXUP_184;
        float _RESERVED_IDENTIFIER_FIXUP_185;
        if (_RESERVED_IDENTIFIER_FIXUP_168)
        {
            precise float _175 = 10.0f * _RESERVED_IDENTIFIER_FIXUP_165;
            float _RESERVED_IDENTIFIER_FIXUP_171 = _175;
            float _RESERVED_IDENTIFIER_FIXUP_173 = min(1.0f, _RESERVED_IDENTIFIER_FIXUP_171);
            precise float _183 = _RESERVED_IDENTIFIER_FIXUP_156 - _RESERVED_IDENTIFIER_FIXUP_142;
            float _RESERVED_IDENTIFIER_FIXUP_174 = _183;
            precise float _187 = _RESERVED_IDENTIFIER_FIXUP_157 - _RESERVED_IDENTIFIER_FIXUP_143;
            float _RESERVED_IDENTIFIER_FIXUP_175 = _187;
            precise float _191 = _RESERVED_IDENTIFIER_FIXUP_158 - _RESERVED_IDENTIFIER_FIXUP_144;
            float _RESERVED_IDENTIFIER_FIXUP_176 = _191;
            precise float _195 = _RESERVED_IDENTIFIER_FIXUP_173 * _RESERVED_IDENTIFIER_FIXUP_174;
            float _RESERVED_IDENTIFIER_FIXUP_177 = _195;
            precise float _199 = _RESERVED_IDENTIFIER_FIXUP_177 + _RESERVED_IDENTIFIER_FIXUP_142;
            float _RESERVED_IDENTIFIER_FIXUP_178 = _199;
            precise float _203 = _RESERVED_IDENTIFIER_FIXUP_173 * _RESERVED_IDENTIFIER_FIXUP_175;
            float _RESERVED_IDENTIFIER_FIXUP_179 = _203;
            precise float _207 = _RESERVED_IDENTIFIER_FIXUP_179 + _RESERVED_IDENTIFIER_FIXUP_143;
            float _RESERVED_IDENTIFIER_FIXUP_180 = _207;
            precise float _211 = _RESERVED_IDENTIFIER_FIXUP_173 * _RESERVED_IDENTIFIER_FIXUP_176;
            float _RESERVED_IDENTIFIER_FIXUP_181 = _211;
            precise float _215 = _RESERVED_IDENTIFIER_FIXUP_181 + _RESERVED_IDENTIFIER_FIXUP_144;
            float _RESERVED_IDENTIFIER_FIXUP_182 = _215;
            _RESERVED_IDENTIFIER_FIXUP_183 = _RESERVED_IDENTIFIER_FIXUP_178;
            _RESERVED_IDENTIFIER_FIXUP_184 = _RESERVED_IDENTIFIER_FIXUP_180;
            _RESERVED_IDENTIFIER_FIXUP_185 = _RESERVED_IDENTIFIER_FIXUP_182;
        }
        else
        {
            _RESERVED_IDENTIFIER_FIXUP_183 = _RESERVED_IDENTIFIER_FIXUP_142;
            _RESERVED_IDENTIFIER_FIXUP_184 = _RESERVED_IDENTIFIER_FIXUP_143;
            _RESERVED_IDENTIFIER_FIXUP_185 = _RESERVED_IDENTIFIER_FIXUP_144;
        }
        float _RESERVED_IDENTIFIER_FIXUP_205;
        float _RESERVED_IDENTIFIER_FIXUP_206;
        float _RESERVED_IDENTIFIER_FIXUP_207;
        if (!_RESERVED_IDENTIFIER_FIXUP_168)
        {
            precise float _234 = 1.0f / max(0.00999999977648258209228515625f, _RESERVED_IDENTIFIER_FIXUP_159);
            float _RESERVED_IDENTIFIER_FIXUP_188 = _234;
            precise float _238 = _RESERVED_IDENTIFIER_FIXUP_165 * _RESERVED_IDENTIFIER_FIXUP_159;
            float _RESERVED_IDENTIFIER_FIXUP_189 = _238;
            precise float _242 = _RESERVED_IDENTIFIER_FIXUP_156 * _RESERVED_IDENTIFIER_FIXUP_165;
            float _RESERVED_IDENTIFIER_FIXUP_190 = _242;
            precise float _246 = _RESERVED_IDENTIFIER_FIXUP_165 * _RESERVED_IDENTIFIER_FIXUP_157;
            float _RESERVED_IDENTIFIER_FIXUP_191 = _246;
            precise float _250 = _RESERVED_IDENTIFIER_FIXUP_165 * _RESERVED_IDENTIFIER_FIXUP_158;
            float _RESERVED_IDENTIFIER_FIXUP_192 = _250;
            precise float _255 = (-_RESERVED_IDENTIFIER_FIXUP_189) * _RESERVED_IDENTIFIER_FIXUP_188;
            float _RESERVED_IDENTIFIER_FIXUP_194 = _255;
            precise float _258 = _RESERVED_IDENTIFIER_FIXUP_194 + 1.0f;
            float _RESERVED_IDENTIFIER_FIXUP_195 = _258;
            precise float _262 = _RESERVED_IDENTIFIER_FIXUP_190 * _RESERVED_IDENTIFIER_FIXUP_188;
            float _RESERVED_IDENTIFIER_FIXUP_196 = _262;
            precise float _266 = _RESERVED_IDENTIFIER_FIXUP_191 * _RESERVED_IDENTIFIER_FIXUP_188;
            float _RESERVED_IDENTIFIER_FIXUP_197 = _266;
            precise float _270 = _RESERVED_IDENTIFIER_FIXUP_192 * _RESERVED_IDENTIFIER_FIXUP_188;
            float _RESERVED_IDENTIFIER_FIXUP_198 = _270;
            precise float _274 = _RESERVED_IDENTIFIER_FIXUP_142 * _RESERVED_IDENTIFIER_FIXUP_195;
            float _RESERVED_IDENTIFIER_FIXUP_199 = _274;
            precise float _278 = _RESERVED_IDENTIFIER_FIXUP_199 + _RESERVED_IDENTIFIER_FIXUP_196;
            float _RESERVED_IDENTIFIER_FIXUP_200 = _278;
            precise float _282 = _RESERVED_IDENTIFIER_FIXUP_195 * _RESERVED_IDENTIFIER_FIXUP_143;
            float _RESERVED_IDENTIFIER_FIXUP_201 = _282;
            precise float _286 = _RESERVED_IDENTIFIER_FIXUP_201 + _RESERVED_IDENTIFIER_FIXUP_197;
            float _RESERVED_IDENTIFIER_FIXUP_202 = _286;
            precise float _290 = _RESERVED_IDENTIFIER_FIXUP_195 * _RESERVED_IDENTIFIER_FIXUP_144;
            float _RESERVED_IDENTIFIER_FIXUP_203 = _290;
            precise float _294 = _RESERVED_IDENTIFIER_FIXUP_203 + _RESERVED_IDENTIFIER_FIXUP_198;
            float _RESERVED_IDENTIFIER_FIXUP_204 = _294;
            _RESERVED_IDENTIFIER_FIXUP_205 = _RESERVED_IDENTIFIER_FIXUP_200;
            _RESERVED_IDENTIFIER_FIXUP_206 = _RESERVED_IDENTIFIER_FIXUP_202;
            _RESERVED_IDENTIFIER_FIXUP_207 = _RESERVED_IDENTIFIER_FIXUP_204;
        }
        else
        {
            _RESERVED_IDENTIFIER_FIXUP_205 = _RESERVED_IDENTIFIER_FIXUP_183;
            _RESERVED_IDENTIFIER_FIXUP_206 = _RESERVED_IDENTIFIER_FIXUP_184;
            _RESERVED_IDENTIFIER_FIXUP_207 = _RESERVED_IDENTIFIER_FIXUP_185;
        }
        _RESERVED_IDENTIFIER_FIXUP_208 = _RESERVED_IDENTIFIER_FIXUP_205;
        _RESERVED_IDENTIFIER_FIXUP_209 = _RESERVED_IDENTIFIER_FIXUP_206;
        _RESERVED_IDENTIFIER_FIXUP_210 = _RESERVED_IDENTIFIER_FIXUP_207;
    }
    else
    {
        _RESERVED_IDENTIFIER_FIXUP_208 = _RESERVED_IDENTIFIER_FIXUP_142;
        _RESERVED_IDENTIFIER_FIXUP_209 = _RESERVED_IDENTIFIER_FIXUP_143;
        _RESERVED_IDENTIFIER_FIXUP_210 = _RESERVED_IDENTIFIER_FIXUP_144;
    }
    float _RESERVED_IDENTIFIER_FIXUP_420;
    float _RESERVED_IDENTIFIER_FIXUP_421;
    float _RESERVED_IDENTIFIER_FIXUP_422;
    if ((ssbo_1_1.Load<uint>(_RESERVED_IDENTIFIER_FIXUP_146 * 4 + 0) & 1u) != 0u)
    {
        float _RESERVED_IDENTIFIER_FIXUP_237 = asfloat(ssbo_1_1.Load<uint>((22u + buf0_dword_off) * 4 + 0));
        precise float _332 = _RESERVED_IDENTIFIER_FIXUP_136 * _RESERVED_IDENTIFIER_FIXUP_237;
        float _RESERVED_IDENTIFIER_FIXUP_238 = _332;
        precise float _336 = _RESERVED_IDENTIFIER_FIXUP_238 + (-0.5f);
        float _RESERVED_IDENTIFIER_FIXUP_240 = _336;
        float _RESERVED_IDENTIFIER_FIXUP_241 = frac(_RESERVED_IDENTIFIER_FIXUP_240);
        precise float _343 = _RESERVED_IDENTIFIER_FIXUP_241 * _RESERVED_IDENTIFIER_FIXUP_241;
        float _RESERVED_IDENTIFIER_FIXUP_242 = _343;
        precise float _347 = _RESERVED_IDENTIFIER_FIXUP_241 * _RESERVED_IDENTIFIER_FIXUP_242;
        float _RESERVED_IDENTIFIER_FIXUP_243 = _347;
        precise float _351 = 3.0f * _RESERVED_IDENTIFIER_FIXUP_243;
        float _RESERVED_IDENTIFIER_FIXUP_245 = _351;
        precise float _354 = 3.0f * _RESERVED_IDENTIFIER_FIXUP_242;
        float _RESERVED_IDENTIFIER_FIXUP_246 = _354;
        precise float _359 = 6.0f * (-_RESERVED_IDENTIFIER_FIXUP_242);
        float _RESERVED_IDENTIFIER_FIXUP_249 = _359;
        precise float _363 = _RESERVED_IDENTIFIER_FIXUP_249 + _RESERVED_IDENTIFIER_FIXUP_245;
        float _RESERVED_IDENTIFIER_FIXUP_250 = _363;
        precise float _366 = 3.0f * _RESERVED_IDENTIFIER_FIXUP_242;
        float _RESERVED_IDENTIFIER_FIXUP_252 = _366;
        precise float _371 = _RESERVED_IDENTIFIER_FIXUP_252 + (-_RESERVED_IDENTIFIER_FIXUP_243);
        float _RESERVED_IDENTIFIER_FIXUP_253 = _371;
        precise float _374 = _RESERVED_IDENTIFIER_FIXUP_241 * 3.0f;
        float _RESERVED_IDENTIFIER_FIXUP_254 = _374;
        precise float _378 = _RESERVED_IDENTIFIER_FIXUP_254 + _RESERVED_IDENTIFIER_FIXUP_246;
        float _RESERVED_IDENTIFIER_FIXUP_255 = _378;
        precise float _382 = 3.0f * (-_RESERVED_IDENTIFIER_FIXUP_241);
        float _RESERVED_IDENTIFIER_FIXUP_257 = _382;
        precise float _386 = _RESERVED_IDENTIFIER_FIXUP_257 + _RESERVED_IDENTIFIER_FIXUP_253;
        float _RESERVED_IDENTIFIER_FIXUP_258 = _386;
        precise float _390 = _RESERVED_IDENTIFIER_FIXUP_243 * (-3.0f);
        float _RESERVED_IDENTIFIER_FIXUP_260 = _390;
        precise float _394 = _RESERVED_IDENTIFIER_FIXUP_260 + _RESERVED_IDENTIFIER_FIXUP_255;
        float _RESERVED_IDENTIFIER_FIXUP_261 = _394;
        precise float _398 = 0.16666667163372039794921875f * _RESERVED_IDENTIFIER_FIXUP_243;
        float _RESERVED_IDENTIFIER_FIXUP_263 = _398;
        precise float _401 = _RESERVED_IDENTIFIER_FIXUP_261 * 0.16666667163372039794921875f;
        float _RESERVED_IDENTIFIER_FIXUP_264 = _401;
        precise float _405 = _RESERVED_IDENTIFIER_FIXUP_264 + _RESERVED_IDENTIFIER_FIXUP_263;
        float _RESERVED_IDENTIFIER_FIXUP_265 = _405;
        precise float _408 = 0.16666667163372039794921875f + _RESERVED_IDENTIFIER_FIXUP_265;
        float _RESERVED_IDENTIFIER_FIXUP_266 = _408;
        precise float _412 = _RESERVED_IDENTIFIER_FIXUP_240 - _RESERVED_IDENTIFIER_FIXUP_241;
        float _RESERVED_IDENTIFIER_FIXUP_267 = _412;
        float _RESERVED_IDENTIFIER_FIXUP_269 = mad(0.16666667163372039794921875f, _RESERVED_IDENTIFIER_FIXUP_250, 0.666666686534881591796875f);
        precise float _419 = _RESERVED_IDENTIFIER_FIXUP_258 * 0.16666667163372039794921875f;
        float _RESERVED_IDENTIFIER_FIXUP_270 = _419;
        precise float _423 = _RESERVED_IDENTIFIER_FIXUP_270 + _RESERVED_IDENTIFIER_FIXUP_269;
        float _RESERVED_IDENTIFIER_FIXUP_271 = _423;
        precise float _426 = 0.16666667163372039794921875f + _RESERVED_IDENTIFIER_FIXUP_271;
        float _RESERVED_IDENTIFIER_FIXUP_272 = _426;
        precise float _429 = 1.0f / _RESERVED_IDENTIFIER_FIXUP_272;
        precise float _431 = _429 * _RESERVED_IDENTIFIER_FIXUP_269;
        float _RESERVED_IDENTIFIER_FIXUP_274 = _431;
        precise float _435 = _RESERVED_IDENTIFIER_FIXUP_274 + _RESERVED_IDENTIFIER_FIXUP_267;
        float _RESERVED_IDENTIFIER_FIXUP_275 = _435;
        precise float _439 = 1.0f / _RESERVED_IDENTIFIER_FIXUP_266;
        precise float _440 = _RESERVED_IDENTIFIER_FIXUP_263 * _439;
        float _RESERVED_IDENTIFIER_FIXUP_277 = _440;
        precise float _444 = _RESERVED_IDENTIFIER_FIXUP_277 + _RESERVED_IDENTIFIER_FIXUP_267;
        float _RESERVED_IDENTIFIER_FIXUP_278 = _444;
        precise float _447 = (-0.5f) + _RESERVED_IDENTIFIER_FIXUP_275;
        float _RESERVED_IDENTIFIER_FIXUP_279 = _447;
        precise float _450 = 1.0f / _RESERVED_IDENTIFIER_FIXUP_237;
        float _RESERVED_IDENTIFIER_FIXUP_280 = _450;
        precise float _454 = 1.5f + _RESERVED_IDENTIFIER_FIXUP_278;
        float _RESERVED_IDENTIFIER_FIXUP_282 = _454;
        precise float _458 = _RESERVED_IDENTIFIER_FIXUP_280 * _RESERVED_IDENTIFIER_FIXUP_279;
        float _RESERVED_IDENTIFIER_FIXUP_283 = _458;
        precise float _462 = _RESERVED_IDENTIFIER_FIXUP_272 + _RESERVED_IDENTIFIER_FIXUP_266;
        float _RESERVED_IDENTIFIER_FIXUP_284 = _462;
        precise float _465 = 1.0f / _RESERVED_IDENTIFIER_FIXUP_284;
        precise float _467 = _465 * _RESERVED_IDENTIFIER_FIXUP_272;
        float _RESERVED_IDENTIFIER_FIXUP_286 = _467;
        float _RESERVED_IDENTIFIER_FIXUP_287 = asfloat(ssbo_1_1.Load<uint>((21u + buf0_dword_off) * 4 + 0));
        precise float _478 = _RESERVED_IDENTIFIER_FIXUP_130 * _RESERVED_IDENTIFIER_FIXUP_287;
        float _RESERVED_IDENTIFIER_FIXUP_288 = _478;
        precise float _481 = _RESERVED_IDENTIFIER_FIXUP_288 + (-0.5f);
        float _RESERVED_IDENTIFIER_FIXUP_289 = _481;
        float _RESERVED_IDENTIFIER_FIXUP_290 = frac(_RESERVED_IDENTIFIER_FIXUP_289);
        precise float _488 = _RESERVED_IDENTIFIER_FIXUP_290 * _RESERVED_IDENTIFIER_FIXUP_290;
        float _RESERVED_IDENTIFIER_FIXUP_291 = _488;
        precise float _492 = _RESERVED_IDENTIFIER_FIXUP_290 * _RESERVED_IDENTIFIER_FIXUP_291;
        float _RESERVED_IDENTIFIER_FIXUP_292 = _492;
        precise float _495 = 3.0f * _RESERVED_IDENTIFIER_FIXUP_291;
        float _RESERVED_IDENTIFIER_FIXUP_293 = _495;
        precise float _498 = 3.0f * _RESERVED_IDENTIFIER_FIXUP_292;
        float _RESERVED_IDENTIFIER_FIXUP_294 = _498;
        precise float _501 = _RESERVED_IDENTIFIER_FIXUP_290 * 3.0f;
        float _RESERVED_IDENTIFIER_FIXUP_295 = _501;
        precise float _505 = _RESERVED_IDENTIFIER_FIXUP_295 + _RESERVED_IDENTIFIER_FIXUP_293;
        float _RESERVED_IDENTIFIER_FIXUP_296 = _505;
        precise float _509 = 6.0f * (-_RESERVED_IDENTIFIER_FIXUP_291);
        float _RESERVED_IDENTIFIER_FIXUP_298 = _509;
        precise float _513 = _RESERVED_IDENTIFIER_FIXUP_298 + _RESERVED_IDENTIFIER_FIXUP_294;
        float _RESERVED_IDENTIFIER_FIXUP_299 = _513;
        precise float _516 = _RESERVED_IDENTIFIER_FIXUP_292 * (-3.0f);
        float _RESERVED_IDENTIFIER_FIXUP_300 = _516;
        precise float _520 = _RESERVED_IDENTIFIER_FIXUP_300 + _RESERVED_IDENTIFIER_FIXUP_296;
        float _RESERVED_IDENTIFIER_FIXUP_301 = _520;
        precise float _523 = 0.16666667163372039794921875f * _RESERVED_IDENTIFIER_FIXUP_292;
        float _RESERVED_IDENTIFIER_FIXUP_302 = _523;
        float _RESERVED_IDENTIFIER_FIXUP_303 = mad(0.16666667163372039794921875f, _RESERVED_IDENTIFIER_FIXUP_299, 0.666666686534881591796875f);
        precise float _529 = _RESERVED_IDENTIFIER_FIXUP_301 * 0.16666667163372039794921875f;
        float _RESERVED_IDENTIFIER_FIXUP_304 = _529;
        precise float _533 = _RESERVED_IDENTIFIER_FIXUP_304 + _RESERVED_IDENTIFIER_FIXUP_302;
        float _RESERVED_IDENTIFIER_FIXUP_305 = _533;
        precise float _536 = 0.16666667163372039794921875f + _RESERVED_IDENTIFIER_FIXUP_305;
        float _RESERVED_IDENTIFIER_FIXUP_306 = _536;
        precise float _540 = _RESERVED_IDENTIFIER_FIXUP_289 - _RESERVED_IDENTIFIER_FIXUP_290;
        float _RESERVED_IDENTIFIER_FIXUP_308 = _540;
        precise float _543 = 1.0f / _RESERVED_IDENTIFIER_FIXUP_306;
        precise float _545 = _543 * _RESERVED_IDENTIFIER_FIXUP_302;
        float _RESERVED_IDENTIFIER_FIXUP_309 = _545;
        precise float _549 = _RESERVED_IDENTIFIER_FIXUP_309 + _RESERVED_IDENTIFIER_FIXUP_308;
        float _RESERVED_IDENTIFIER_FIXUP_310 = _549;
        precise float _552 = 3.0f * _RESERVED_IDENTIFIER_FIXUP_291;
        float _RESERVED_IDENTIFIER_FIXUP_312 = _552;
        precise float _557 = _RESERVED_IDENTIFIER_FIXUP_312 + (-_RESERVED_IDENTIFIER_FIXUP_292);
        float _RESERVED_IDENTIFIER_FIXUP_313 = _557;
        precise float _561 = 3.0f * (-_RESERVED_IDENTIFIER_FIXUP_290);
        float _RESERVED_IDENTIFIER_FIXUP_315 = _561;
        precise float _565 = _RESERVED_IDENTIFIER_FIXUP_315 + _RESERVED_IDENTIFIER_FIXUP_313;
        float _RESERVED_IDENTIFIER_FIXUP_316 = _565;
        precise float _568 = _RESERVED_IDENTIFIER_FIXUP_316 * 0.16666667163372039794921875f;
        float _RESERVED_IDENTIFIER_FIXUP_317 = _568;
        precise float _572 = _RESERVED_IDENTIFIER_FIXUP_317 + _RESERVED_IDENTIFIER_FIXUP_303;
        float _RESERVED_IDENTIFIER_FIXUP_318 = _572;
        precise float _575 = 0.16666667163372039794921875f + _RESERVED_IDENTIFIER_FIXUP_318;
        float _RESERVED_IDENTIFIER_FIXUP_319 = _575;
        precise float _578 = 1.0f / _RESERVED_IDENTIFIER_FIXUP_287;
        float _RESERVED_IDENTIFIER_FIXUP_320 = _578;
        precise float _582 = _RESERVED_IDENTIFIER_FIXUP_319 + _RESERVED_IDENTIFIER_FIXUP_306;
        float _RESERVED_IDENTIFIER_FIXUP_321 = _582;
        precise float _585 = 1.0f / _RESERVED_IDENTIFIER_FIXUP_321;
        precise float _587 = _585 * _RESERVED_IDENTIFIER_FIXUP_319;
        float _RESERVED_IDENTIFIER_FIXUP_323 = _587;
        precise float _591 = 1.0f / _RESERVED_IDENTIFIER_FIXUP_319;
        precise float _592 = _RESERVED_IDENTIFIER_FIXUP_303 * _591;
        float _RESERVED_IDENTIFIER_FIXUP_325 = _592;
        precise float _596 = _RESERVED_IDENTIFIER_FIXUP_325 + _RESERVED_IDENTIFIER_FIXUP_308;
        float _RESERVED_IDENTIFIER_FIXUP_326 = _596;
        precise float _599 = 1.5f + _RESERVED_IDENTIFIER_FIXUP_310;
        float _RESERVED_IDENTIFIER_FIXUP_327 = _599;
        precise float _602 = (-0.5f) + _RESERVED_IDENTIFIER_FIXUP_326;
        float _RESERVED_IDENTIFIER_FIXUP_328 = _602;
        precise float _606 = _RESERVED_IDENTIFIER_FIXUP_320 * _RESERVED_IDENTIFIER_FIXUP_327;
        float _RESERVED_IDENTIFIER_FIXUP_329 = _606;
        precise float _610 = _RESERVED_IDENTIFIER_FIXUP_320 * _RESERVED_IDENTIFIER_FIXUP_328;
        float _RESERVED_IDENTIFIER_FIXUP_330 = _610;
        float4 _RESERVED_IDENTIFIER_FIXUP_336 = fs_img3.SampleLevel(fs_samp1, float2(_RESERVED_IDENTIFIER_FIXUP_329, _RESERVED_IDENTIFIER_FIXUP_283), 2.0f);
        float _RESERVED_IDENTIFIER_FIXUP_337 = _RESERVED_IDENTIFIER_FIXUP_336.x;
        float _RESERVED_IDENTIFIER_FIXUP_338 = _RESERVED_IDENTIFIER_FIXUP_336.y;
        float _RESERVED_IDENTIFIER_FIXUP_339 = _RESERVED_IDENTIFIER_FIXUP_336.z;
        float4 _RESERVED_IDENTIFIER_FIXUP_344 = fs_img3.SampleLevel(fs_samp1, float2(_RESERVED_IDENTIFIER_FIXUP_330, _RESERVED_IDENTIFIER_FIXUP_283), 2.0f);
        precise float _641 = _RESERVED_IDENTIFIER_FIXUP_280 * _RESERVED_IDENTIFIER_FIXUP_282;
        float _RESERVED_IDENTIFIER_FIXUP_348 = _641;
        float4 _RESERVED_IDENTIFIER_FIXUP_353 = fs_img3.SampleLevel(fs_samp1, float2(_RESERVED_IDENTIFIER_FIXUP_330, _RESERVED_IDENTIFIER_FIXUP_348), 2.0f);
        float4 _RESERVED_IDENTIFIER_FIXUP_361 = fs_img3.SampleLevel(fs_samp1, float2(_RESERVED_IDENTIFIER_FIXUP_329, _RESERVED_IDENTIFIER_FIXUP_348), 2.0f);
        float _RESERVED_IDENTIFIER_FIXUP_362 = _RESERVED_IDENTIFIER_FIXUP_361.x;
        float _RESERVED_IDENTIFIER_FIXUP_363 = _RESERVED_IDENTIFIER_FIXUP_361.y;
        float _RESERVED_IDENTIFIER_FIXUP_364 = _RESERVED_IDENTIFIER_FIXUP_361.z;
        float _RESERVED_IDENTIFIER_FIXUP_365 = asfloat(ssbo_1_1.Load<uint>((26u + buf0_dword_off) * 4 + 0));
        precise float _678 = _RESERVED_IDENTIFIER_FIXUP_344.x - _RESERVED_IDENTIFIER_FIXUP_337;
        float _RESERVED_IDENTIFIER_FIXUP_366 = _678;
        precise float _682 = _RESERVED_IDENTIFIER_FIXUP_366 * _RESERVED_IDENTIFIER_FIXUP_323;
        float _RESERVED_IDENTIFIER_FIXUP_367 = _682;
        precise float _686 = _RESERVED_IDENTIFIER_FIXUP_367 + _RESERVED_IDENTIFIER_FIXUP_337;
        float _RESERVED_IDENTIFIER_FIXUP_368 = _686;
        precise float _691 = _RESERVED_IDENTIFIER_FIXUP_344.y - _RESERVED_IDENTIFIER_FIXUP_338;
        float _RESERVED_IDENTIFIER_FIXUP_369 = _691;
        precise float _695 = _RESERVED_IDENTIFIER_FIXUP_369 * _RESERVED_IDENTIFIER_FIXUP_323;
        float _RESERVED_IDENTIFIER_FIXUP_370 = _695;
        precise float _699 = _RESERVED_IDENTIFIER_FIXUP_370 + _RESERVED_IDENTIFIER_FIXUP_338;
        float _RESERVED_IDENTIFIER_FIXUP_371 = _699;
        precise float _704 = _RESERVED_IDENTIFIER_FIXUP_344.z - _RESERVED_IDENTIFIER_FIXUP_339;
        float _RESERVED_IDENTIFIER_FIXUP_372 = _704;
        precise float _708 = _RESERVED_IDENTIFIER_FIXUP_323 * _RESERVED_IDENTIFIER_FIXUP_372;
        float _RESERVED_IDENTIFIER_FIXUP_373 = _708;
        precise float _712 = _RESERVED_IDENTIFIER_FIXUP_373 + _RESERVED_IDENTIFIER_FIXUP_339;
        float _RESERVED_IDENTIFIER_FIXUP_374 = _712;
        precise float _717 = _RESERVED_IDENTIFIER_FIXUP_353.x - _RESERVED_IDENTIFIER_FIXUP_362;
        float _RESERVED_IDENTIFIER_FIXUP_375 = _717;
        precise float _721 = _RESERVED_IDENTIFIER_FIXUP_323 * _RESERVED_IDENTIFIER_FIXUP_375;
        float _RESERVED_IDENTIFIER_FIXUP_376 = _721;
        precise float _725 = _RESERVED_IDENTIFIER_FIXUP_376 + _RESERVED_IDENTIFIER_FIXUP_362;
        float _RESERVED_IDENTIFIER_FIXUP_377 = _725;
        precise float _730 = _RESERVED_IDENTIFIER_FIXUP_353.z - _RESERVED_IDENTIFIER_FIXUP_364;
        float _RESERVED_IDENTIFIER_FIXUP_378 = _730;
        precise float _734 = _RESERVED_IDENTIFIER_FIXUP_323 * _RESERVED_IDENTIFIER_FIXUP_378;
        float _RESERVED_IDENTIFIER_FIXUP_379 = _734;
        precise float _738 = _RESERVED_IDENTIFIER_FIXUP_379 + _RESERVED_IDENTIFIER_FIXUP_364;
        float _RESERVED_IDENTIFIER_FIXUP_380 = _738;
        precise float _742 = _RESERVED_IDENTIFIER_FIXUP_368 - _RESERVED_IDENTIFIER_FIXUP_377;
        float _RESERVED_IDENTIFIER_FIXUP_381 = _742;
        precise float _746 = _RESERVED_IDENTIFIER_FIXUP_381 * _RESERVED_IDENTIFIER_FIXUP_286;
        float _RESERVED_IDENTIFIER_FIXUP_382 = _746;
        precise float _750 = _RESERVED_IDENTIFIER_FIXUP_382 + _RESERVED_IDENTIFIER_FIXUP_377;
        float _RESERVED_IDENTIFIER_FIXUP_383 = _750;
        precise float _754 = _RESERVED_IDENTIFIER_FIXUP_374 - _RESERVED_IDENTIFIER_FIXUP_380;
        float _RESERVED_IDENTIFIER_FIXUP_384 = _754;
        precise float _758 = _RESERVED_IDENTIFIER_FIXUP_384 * _RESERVED_IDENTIFIER_FIXUP_286;
        float _RESERVED_IDENTIFIER_FIXUP_385 = _758;
        precise float _762 = _RESERVED_IDENTIFIER_FIXUP_385 + _RESERVED_IDENTIFIER_FIXUP_380;
        float _RESERVED_IDENTIFIER_FIXUP_386 = _762;
        precise float _767 = _RESERVED_IDENTIFIER_FIXUP_353.y - _RESERVED_IDENTIFIER_FIXUP_363;
        float _RESERVED_IDENTIFIER_FIXUP_387 = _767;
        precise float _771 = _RESERVED_IDENTIFIER_FIXUP_323 * _RESERVED_IDENTIFIER_FIXUP_387;
        float _RESERVED_IDENTIFIER_FIXUP_388 = _771;
        precise float _775 = _RESERVED_IDENTIFIER_FIXUP_388 + _RESERVED_IDENTIFIER_FIXUP_363;
        float _RESERVED_IDENTIFIER_FIXUP_389 = _775;
        precise float _779 = _RESERVED_IDENTIFIER_FIXUP_371 - _RESERVED_IDENTIFIER_FIXUP_389;
        float _RESERVED_IDENTIFIER_FIXUP_390 = _779;
        precise float _783 = _RESERVED_IDENTIFIER_FIXUP_390 * _RESERVED_IDENTIFIER_FIXUP_286;
        float _RESERVED_IDENTIFIER_FIXUP_391 = _783;
        precise float _787 = _RESERVED_IDENTIFIER_FIXUP_391 + _RESERVED_IDENTIFIER_FIXUP_389;
        float _RESERVED_IDENTIFIER_FIXUP_392 = _787;
        float _RESERVED_IDENTIFIER_FIXUP_393 = asfloat(ssbo_1_1.Load<uint>((25u + buf0_dword_off) * 4 + 0));
        precise float _798 = _RESERVED_IDENTIFIER_FIXUP_393 * _RESERVED_IDENTIFIER_FIXUP_383;
        float _RESERVED_IDENTIFIER_FIXUP_394 = _798;
        precise float _802 = _RESERVED_IDENTIFIER_FIXUP_394 + _RESERVED_IDENTIFIER_FIXUP_365;
        float _RESERVED_IDENTIFIER_FIXUP_395 = _802;
        precise float _806 = _RESERVED_IDENTIFIER_FIXUP_393 * _RESERVED_IDENTIFIER_FIXUP_386;
        float _RESERVED_IDENTIFIER_FIXUP_396 = _806;
        precise float _810 = _RESERVED_IDENTIFIER_FIXUP_396 + _RESERVED_IDENTIFIER_FIXUP_365;
        float _RESERVED_IDENTIFIER_FIXUP_397 = _810;
        precise float _814 = _RESERVED_IDENTIFIER_FIXUP_393 * _RESERVED_IDENTIFIER_FIXUP_392;
        float _RESERVED_IDENTIFIER_FIXUP_398 = _814;
        precise float _818 = _RESERVED_IDENTIFIER_FIXUP_398 + _RESERVED_IDENTIFIER_FIXUP_365;
        float _RESERVED_IDENTIFIER_FIXUP_399 = _818;
        float _RESERVED_IDENTIFIER_FIXUP_406 = asfloat(ssbo_1_1.Load<uint>((27u + buf0_dword_off) * 4 + 0));
        precise float _831 = _RESERVED_IDENTIFIER_FIXUP_406 * log2(abs(_RESERVED_IDENTIFIER_FIXUP_395));
        float _RESERVED_IDENTIFIER_FIXUP_407 = _831;
        precise float _837 = _RESERVED_IDENTIFIER_FIXUP_406 * log2(abs(_RESERVED_IDENTIFIER_FIXUP_399));
        float _RESERVED_IDENTIFIER_FIXUP_408 = _837;
        precise float _843 = _RESERVED_IDENTIFIER_FIXUP_406 * log2(abs(_RESERVED_IDENTIFIER_FIXUP_397));
        float _RESERVED_IDENTIFIER_FIXUP_409 = _843;
        float _RESERVED_IDENTIFIER_FIXUP_413 = asfloat(ssbo_1_1.Load<uint>((23u + buf0_dword_off) * 4 + 0));
        precise float _855 = _RESERVED_IDENTIFIER_FIXUP_413 * exp2(_RESERVED_IDENTIFIER_FIXUP_407);
        float _RESERVED_IDENTIFIER_FIXUP_414 = _855;
        precise float _859 = _RESERVED_IDENTIFIER_FIXUP_414 + _RESERVED_IDENTIFIER_FIXUP_208;
        float _RESERVED_IDENTIFIER_FIXUP_415 = _859;
        precise float _864 = _RESERVED_IDENTIFIER_FIXUP_413 * exp2(_RESERVED_IDENTIFIER_FIXUP_408);
        float _RESERVED_IDENTIFIER_FIXUP_416 = _864;
        precise float _868 = _RESERVED_IDENTIFIER_FIXUP_416 + _RESERVED_IDENTIFIER_FIXUP_209;
        float _RESERVED_IDENTIFIER_FIXUP_417 = _868;
        precise float _873 = _RESERVED_IDENTIFIER_FIXUP_413 * exp2(_RESERVED_IDENTIFIER_FIXUP_409);
        float _RESERVED_IDENTIFIER_FIXUP_418 = _873;
        precise float _877 = _RESERVED_IDENTIFIER_FIXUP_418 + _RESERVED_IDENTIFIER_FIXUP_210;
        float _RESERVED_IDENTIFIER_FIXUP_419 = _877;
        _RESERVED_IDENTIFIER_FIXUP_420 = _RESERVED_IDENTIFIER_FIXUP_415;
        _RESERVED_IDENTIFIER_FIXUP_421 = _RESERVED_IDENTIFIER_FIXUP_417;
        _RESERVED_IDENTIFIER_FIXUP_422 = _RESERVED_IDENTIFIER_FIXUP_419;
    }
    else
    {
        _RESERVED_IDENTIFIER_FIXUP_420 = _RESERVED_IDENTIFIER_FIXUP_208;
        _RESERVED_IDENTIFIER_FIXUP_421 = _RESERVED_IDENTIFIER_FIXUP_209;
        _RESERVED_IDENTIFIER_FIXUP_422 = _RESERVED_IDENTIFIER_FIXUP_210;
    }
    bool _RESERVED_IDENTIFIER_FIXUP_425 = (ssbo_1_1.Load<uint>(_RESERVED_IDENTIFIER_FIXUP_146 * 4 + 0) & 4u) != 0u;
    float _RESERVED_IDENTIFIER_FIXUP_436;
    float _RESERVED_IDENTIFIER_FIXUP_437;
    float _RESERVED_IDENTIFIER_FIXUP_438;
    if (_RESERVED_IDENTIFIER_FIXUP_425)
    {
        float _RESERVED_IDENTIFIER_FIXUP_429 = asfloat(ssbo_2_1.Load<uint>((0u + (spvBitfieldUExtract(push_constants.push_data_buf_offsets0.x, 8, 8) >> 2u)) * 4 + 0));
        precise float _914 = _RESERVED_IDENTIFIER_FIXUP_429 * _RESERVED_IDENTIFIER_FIXUP_420;
        float _RESERVED_IDENTIFIER_FIXUP_430 = _914;
        precise float _918 = _RESERVED_IDENTIFIER_FIXUP_429 * _RESERVED_IDENTIFIER_FIXUP_421;
        float _RESERVED_IDENTIFIER_FIXUP_431 = _918;
        precise float _922 = _RESERVED_IDENTIFIER_FIXUP_429 * _RESERVED_IDENTIFIER_FIXUP_422;
        float _RESERVED_IDENTIFIER_FIXUP_432 = _922;
        _RESERVED_IDENTIFIER_FIXUP_436 = max(0.0f, _RESERVED_IDENTIFIER_FIXUP_430);
        _RESERVED_IDENTIFIER_FIXUP_437 = max(0.0f, _RESERVED_IDENTIFIER_FIXUP_431);
        _RESERVED_IDENTIFIER_FIXUP_438 = max(0.0f, _RESERVED_IDENTIFIER_FIXUP_432);
    }
    else
    {
        _RESERVED_IDENTIFIER_FIXUP_436 = _RESERVED_IDENTIFIER_FIXUP_420;
        _RESERVED_IDENTIFIER_FIXUP_437 = _RESERVED_IDENTIFIER_FIXUP_421;
        _RESERVED_IDENTIFIER_FIXUP_438 = _RESERVED_IDENTIFIER_FIXUP_422;
    }
    precise float _940 = 0.2125000059604644775390625f * _RESERVED_IDENTIFIER_FIXUP_436;
    float _RESERVED_IDENTIFIER_FIXUP_440 = _940;
    precise float _944 = _RESERVED_IDENTIFIER_FIXUP_437 * 0.7153999805450439453125f;
    float _RESERVED_IDENTIFIER_FIXUP_442 = _944;
    precise float _948 = _RESERVED_IDENTIFIER_FIXUP_442 + _RESERVED_IDENTIFIER_FIXUP_440;
    float _RESERVED_IDENTIFIER_FIXUP_443 = _948;
    precise float _952 = _RESERVED_IDENTIFIER_FIXUP_438 * 0.07209999859333038330078125f;
    float _RESERVED_IDENTIFIER_FIXUP_445 = _952;
    precise float _956 = _RESERVED_IDENTIFIER_FIXUP_445 + _RESERVED_IDENTIFIER_FIXUP_443;
    float _RESERVED_IDENTIFIER_FIXUP_446 = _956;
    precise float _960 = _RESERVED_IDENTIFIER_FIXUP_446 - _RESERVED_IDENTIFIER_FIXUP_436;
    float _RESERVED_IDENTIFIER_FIXUP_451 = _960;
    precise float _964 = _RESERVED_IDENTIFIER_FIXUP_446 - _RESERVED_IDENTIFIER_FIXUP_437;
    float _RESERVED_IDENTIFIER_FIXUP_452 = _964;
    precise float _968 = _RESERVED_IDENTIFIER_FIXUP_446 - _RESERVED_IDENTIFIER_FIXUP_438;
    float _RESERVED_IDENTIFIER_FIXUP_453 = _968;
    float _RESERVED_IDENTIFIER_FIXUP_454 = asfloat(ssbo_1_1.Load<uint>((35u + buf0_dword_off) * 4 + 0));
    precise float _979 = _RESERVED_IDENTIFIER_FIXUP_454 * _RESERVED_IDENTIFIER_FIXUP_451;
    float _RESERVED_IDENTIFIER_FIXUP_455 = _979;
    precise float _983 = _RESERVED_IDENTIFIER_FIXUP_455 + _RESERVED_IDENTIFIER_FIXUP_436;
    float _RESERVED_IDENTIFIER_FIXUP_456 = _983;
    precise float _987 = _RESERVED_IDENTIFIER_FIXUP_454 * _RESERVED_IDENTIFIER_FIXUP_452;
    float _RESERVED_IDENTIFIER_FIXUP_457 = _987;
    precise float _991 = _RESERVED_IDENTIFIER_FIXUP_457 + _RESERVED_IDENTIFIER_FIXUP_437;
    float _RESERVED_IDENTIFIER_FIXUP_458 = _991;
    precise float _995 = _RESERVED_IDENTIFIER_FIXUP_454 * _RESERVED_IDENTIFIER_FIXUP_453;
    float _RESERVED_IDENTIFIER_FIXUP_459 = _995;
    precise float _999 = _RESERVED_IDENTIFIER_FIXUP_459 + _RESERVED_IDENTIFIER_FIXUP_438;
    float _RESERVED_IDENTIFIER_FIXUP_460 = _999;
    float3 renodx_scene = float3(_RESERVED_IDENTIFIER_FIXUP_456, _RESERVED_IDENTIFIER_FIXUP_458, _RESERVED_IDENTIFIER_FIXUP_460);
    float _RESERVED_IDENTIFIER_FIXUP_546;
    float _RESERVED_IDENTIFIER_FIXUP_547;
    float _RESERVED_IDENTIFIER_FIXUP_548;
    float renodx_filmic_gain = 1.0f;
    if (_RESERVED_IDENTIFIER_FIXUP_425)
    {
        precise float _1017 = asfloat(ssbo_1_1.Load<uint>((36u + buf0_dword_off) * 4 + 0)) * _RESERVED_IDENTIFIER_FIXUP_456;
        float _RESERVED_IDENTIFIER_FIXUP_506 = _1017;
        precise float _1026 = _RESERVED_IDENTIFIER_FIXUP_506 + asfloat(ssbo_1_1.Load<uint>((40u + buf0_dword_off) * 4 + 0));
        float _RESERVED_IDENTIFIER_FIXUP_507 = _1026;
        precise float _1035 = asfloat(ssbo_1_1.Load<uint>((37u + buf0_dword_off) * 4 + 0)) * _RESERVED_IDENTIFIER_FIXUP_458;
        float _RESERVED_IDENTIFIER_FIXUP_509 = _1035;
        precise float _1044 = _RESERVED_IDENTIFIER_FIXUP_509 + asfloat(ssbo_1_1.Load<uint>((41u + buf0_dword_off) * 4 + 0));
        float _RESERVED_IDENTIFIER_FIXUP_510 = _1044;
        precise float _1053 = asfloat(ssbo_1_1.Load<uint>((38u + buf0_dword_off) * 4 + 0)) * _RESERVED_IDENTIFIER_FIXUP_460;
        float _RESERVED_IDENTIFIER_FIXUP_512 = _1053;
        precise float _1062 = _RESERVED_IDENTIFIER_FIXUP_512 + asfloat(ssbo_1_1.Load<uint>((42u + buf0_dword_off) * 4 + 0));
        float _RESERVED_IDENTIFIER_FIXUP_513 = _1062;
        renodx_scene = float3(_RESERVED_IDENTIFIER_FIXUP_507, _RESERVED_IDENTIFIER_FIXUP_510, _RESERVED_IDENTIFIER_FIXUP_513);
        float _RESERVED_IDENTIFIER_FIXUP_514 = asfloat(ssbo_1_1.Load<uint>((0u + buf0_dword_off) * 4 + 0));
        precise float _1076 = _RESERVED_IDENTIFIER_FIXUP_514 + _RESERVED_IDENTIFIER_FIXUP_507;
        float _RESERVED_IDENTIFIER_FIXUP_515 = _1076;
        precise float _1080 = _RESERVED_IDENTIFIER_FIXUP_514 + _RESERVED_IDENTIFIER_FIXUP_510;
        float _RESERVED_IDENTIFIER_FIXUP_516 = _1080;
        precise float _1084 = _RESERVED_IDENTIFIER_FIXUP_514 + _RESERVED_IDENTIFIER_FIXUP_513;
        float _RESERVED_IDENTIFIER_FIXUP_517 = _1084;
        float _RESERVED_IDENTIFIER_FIXUP_518 = asfloat(ssbo_1_1.Load<uint>((1u + buf0_dword_off) * 4 + 0));
        precise float _1094 = _RESERVED_IDENTIFIER_FIXUP_507 * _RESERVED_IDENTIFIER_FIXUP_515;
        float _RESERVED_IDENTIFIER_FIXUP_519 = _1094;
        precise float _1098 = _RESERVED_IDENTIFIER_FIXUP_519 + _RESERVED_IDENTIFIER_FIXUP_518;
        float _RESERVED_IDENTIFIER_FIXUP_520 = _1098;
        precise float _1102 = _RESERVED_IDENTIFIER_FIXUP_510 * _RESERVED_IDENTIFIER_FIXUP_516;
        float _RESERVED_IDENTIFIER_FIXUP_521 = _1102;
        precise float _1106 = _RESERVED_IDENTIFIER_FIXUP_521 + _RESERVED_IDENTIFIER_FIXUP_518;
        float _RESERVED_IDENTIFIER_FIXUP_522 = _1106;
        precise float _1110 = _RESERVED_IDENTIFIER_FIXUP_513 * _RESERVED_IDENTIFIER_FIXUP_517;
        float _RESERVED_IDENTIFIER_FIXUP_523 = _1110;
        precise float _1114 = _RESERVED_IDENTIFIER_FIXUP_523 + _RESERVED_IDENTIFIER_FIXUP_518;
        float _RESERVED_IDENTIFIER_FIXUP_524 = _1114;
        float _RESERVED_IDENTIFIER_FIXUP_525 = asfloat(ssbo_1_1.Load<uint>((6u + buf0_dword_off) * 4 + 0));
        float _RESERVED_IDENTIFIER_FIXUP_527 = asfloat(ssbo_1_1.Load<uint>((5u + buf0_dword_off) * 4 + 0));
        precise float _1132 = _RESERVED_IDENTIFIER_FIXUP_507 * _RESERVED_IDENTIFIER_FIXUP_527;
        float _RESERVED_IDENTIFIER_FIXUP_528 = _1132;
        precise float _1136 = _RESERVED_IDENTIFIER_FIXUP_528 + _RESERVED_IDENTIFIER_FIXUP_525;
        float _RESERVED_IDENTIFIER_FIXUP_529 = _1136;
        precise float _1140 = _RESERVED_IDENTIFIER_FIXUP_510 * _RESERVED_IDENTIFIER_FIXUP_527;
        float _RESERVED_IDENTIFIER_FIXUP_531 = _1140;
        precise float _1144 = _RESERVED_IDENTIFIER_FIXUP_531 + _RESERVED_IDENTIFIER_FIXUP_525;
        float _RESERVED_IDENTIFIER_FIXUP_532 = _1144;
        precise float _1148 = _RESERVED_IDENTIFIER_FIXUP_527 * _RESERVED_IDENTIFIER_FIXUP_513;
        float _RESERVED_IDENTIFIER_FIXUP_534 = _1148;
        precise float _1152 = _RESERVED_IDENTIFIER_FIXUP_534 + _RESERVED_IDENTIFIER_FIXUP_525;
        float _RESERVED_IDENTIFIER_FIXUP_535 = _1152;
        float _RESERVED_IDENTIFIER_FIXUP_536 = asfloat(ssbo_1_1.Load<uint>((4u + buf0_dword_off) * 4 + 0));
        precise float _1161 = 1.0f / _RESERVED_IDENTIFIER_FIXUP_520;
        precise float _1163 = _1161 * _RESERVED_IDENTIFIER_FIXUP_529;
        float _RESERVED_IDENTIFIER_FIXUP_537 = _1163;
        precise float _1167 = _RESERVED_IDENTIFIER_FIXUP_537 + _RESERVED_IDENTIFIER_FIXUP_536;
        float _RESERVED_IDENTIFIER_FIXUP_538 = _1167;
        precise float _1170 = 1.0f / _RESERVED_IDENTIFIER_FIXUP_522;
        precise float _1172 = _1170 * _RESERVED_IDENTIFIER_FIXUP_532;
        float _RESERVED_IDENTIFIER_FIXUP_540 = _1172;
        precise float _1176 = _RESERVED_IDENTIFIER_FIXUP_540 + _RESERVED_IDENTIFIER_FIXUP_536;
        float _RESERVED_IDENTIFIER_FIXUP_541 = _1176;
        precise float _1179 = 1.0f / _RESERVED_IDENTIFIER_FIXUP_524;
        precise float _1181 = _1179 * _RESERVED_IDENTIFIER_FIXUP_535;
        float _RESERVED_IDENTIFIER_FIXUP_543 = _1181;
        precise float _1185 = _RESERVED_IDENTIFIER_FIXUP_543 + _RESERVED_IDENTIFIER_FIXUP_536;
        float _RESERVED_IDENTIFIER_FIXUP_544 = _1185;
        _RESERVED_IDENTIFIER_FIXUP_546 = clamp(_RESERVED_IDENTIFIER_FIXUP_538, 0.0f, 1.0f);
        _RESERVED_IDENTIFIER_FIXUP_547 = clamp(_RESERVED_IDENTIFIER_FIXUP_541, 0.0f, 1.0f);
        _RESERVED_IDENTIFIER_FIXUP_548 = clamp(_RESERVED_IDENTIFIER_FIXUP_544, 0.0f, 1.0f);
        renodx_filmic_gain = RenoDXFilmicAnchorGain(
            _RESERVED_IDENTIFIER_FIXUP_514, _RESERVED_IDENTIFIER_FIXUP_518, _RESERVED_IDENTIFIER_FIXUP_536,
            _RESERVED_IDENTIFIER_FIXUP_527, _RESERVED_IDENTIFIER_FIXUP_525);
    }
    else
    {
        _RESERVED_IDENTIFIER_FIXUP_546 = _RESERVED_IDENTIFIER_FIXUP_456;
        _RESERVED_IDENTIFIER_FIXUP_547 = _RESERVED_IDENTIFIER_FIXUP_458;
        _RESERVED_IDENTIFIER_FIXUP_548 = _RESERVED_IDENTIFIER_FIXUP_460;
    }
    float3 renodx_neutral = float3(_RESERVED_IDENTIFIER_FIXUP_546, _RESERVED_IDENTIFIER_FIXUP_547, _RESERVED_IDENTIFIER_FIXUP_548);
    precise float _1206 = 0.454545438289642333984375f * log2(abs(clamp(max(0.0f, _RESERVED_IDENTIFIER_FIXUP_546), 0.0f, 1.0f)));
    float _RESERVED_IDENTIFIER_FIXUP_577 = _1206;
    precise float _1213 = 0.454545438289642333984375f * log2(abs(clamp(max(0.0f, _RESERVED_IDENTIFIER_FIXUP_547), 0.0f, 1.0f)));
    float _RESERVED_IDENTIFIER_FIXUP_578 = _1213;
    precise float _1220 = 0.454545438289642333984375f * log2(abs(clamp(max(0.0f, _RESERVED_IDENTIFIER_FIXUP_548), 0.0f, 1.0f)));
    float _RESERVED_IDENTIFIER_FIXUP_579 = _1220;
    float _RESERVED_IDENTIFIER_FIXUP_580 = exp2(_RESERVED_IDENTIFIER_FIXUP_577);
    float _RESERVED_IDENTIFIER_FIXUP_581 = exp2(_RESERVED_IDENTIFIER_FIXUP_578);
    float _RESERVED_IDENTIFIER_FIXUP_582 = exp2(_RESERVED_IDENTIFIER_FIXUP_579);
    float _RESERVED_IDENTIFIER_FIXUP_605;
    float _RESERVED_IDENTIFIER_FIXUP_606;
    float _RESERVED_IDENTIFIER_FIXUP_607;
    if (asfloat(ssbo_1_1.Load<uint>((76u + buf0_dword_off) * 4 + 0)) > 0.5f)
    {
        float _RESERVED_IDENTIFIER_FIXUP_589 = asfloat(ssbo_1_1.Load<uint>((78u + buf0_dword_off) * 4 + 0));
        float _RESERVED_IDENTIFIER_FIXUP_590 = asfloat(ssbo_1_1.Load<uint>((77u + buf0_dword_off) * 4 + 0));
        precise float _1259 = _RESERVED_IDENTIFIER_FIXUP_590 * clamp(max(0.0f, _RESERVED_IDENTIFIER_FIXUP_580), 0.0f, 1.0f);
        float _RESERVED_IDENTIFIER_FIXUP_591 = _1259;
        precise float _1263 = _RESERVED_IDENTIFIER_FIXUP_591 + _RESERVED_IDENTIFIER_FIXUP_589;
        float _RESERVED_IDENTIFIER_FIXUP_592 = _1263;
        precise float _1269 = _RESERVED_IDENTIFIER_FIXUP_590 * clamp(max(0.0f, _RESERVED_IDENTIFIER_FIXUP_581), 0.0f, 1.0f);
        float _RESERVED_IDENTIFIER_FIXUP_593 = _1269;
        precise float _1273 = _RESERVED_IDENTIFIER_FIXUP_593 + _RESERVED_IDENTIFIER_FIXUP_589;
        float _RESERVED_IDENTIFIER_FIXUP_594 = _1273;
        precise float _1279 = _RESERVED_IDENTIFIER_FIXUP_590 * clamp(max(0.0f, _RESERVED_IDENTIFIER_FIXUP_582), 0.0f, 1.0f);
        float _RESERVED_IDENTIFIER_FIXUP_595 = _1279;
        precise float _1283 = _RESERVED_IDENTIFIER_FIXUP_595 + _RESERVED_IDENTIFIER_FIXUP_589;
        float _RESERVED_IDENTIFIER_FIXUP_596 = _1283;
        float4 _RESERVED_IDENTIFIER_FIXUP_601 = fs_img4.Sample(fs_samp1, float3(_RESERVED_IDENTIFIER_FIXUP_592, _RESERVED_IDENTIFIER_FIXUP_594, _RESERVED_IDENTIFIER_FIXUP_596));
        _RESERVED_IDENTIFIER_FIXUP_605 = _RESERVED_IDENTIFIER_FIXUP_601.x;
        _RESERVED_IDENTIFIER_FIXUP_606 = _RESERVED_IDENTIFIER_FIXUP_601.y;
        _RESERVED_IDENTIFIER_FIXUP_607 = _RESERVED_IDENTIFIER_FIXUP_601.z;
    }
    else
    {
        _RESERVED_IDENTIFIER_FIXUP_605 = _RESERVED_IDENTIFIER_FIXUP_580;
        _RESERVED_IDENTIFIER_FIXUP_606 = _RESERVED_IDENTIFIER_FIXUP_581;
        _RESERVED_IDENTIFIER_FIXUP_607 = _RESERVED_IDENTIFIER_FIXUP_582;
    }
    uint _RESERVED_IDENTIFIER_FIXUP_609 = 108u + buf0_dword_off;
    float _RESERVED_IDENTIFIER_FIXUP_632;
    float _RESERVED_IDENTIFIER_FIXUP_633;
    float _RESERVED_IDENTIFIER_FIXUP_634;
    if (asfloat(ssbo_1_1.Load<uint>(_RESERVED_IDENTIFIER_FIXUP_609 * 4 + 0)) > 0.0f)
    {
        float4 _RESERVED_IDENTIFIER_FIXUP_618 = fs_img5.Sample(fs_samp0, float2(_RESERVED_IDENTIFIER_FIXUP_130, _RESERVED_IDENTIFIER_FIXUP_136));
        precise float _1334 = _RESERVED_IDENTIFIER_FIXUP_618.x - _RESERVED_IDENTIFIER_FIXUP_605;
        float _RESERVED_IDENTIFIER_FIXUP_622 = _1334;
        precise float _1339 = _RESERVED_IDENTIFIER_FIXUP_618.y - _RESERVED_IDENTIFIER_FIXUP_606;
        float _RESERVED_IDENTIFIER_FIXUP_623 = _1339;
        precise float _1344 = _RESERVED_IDENTIFIER_FIXUP_618.z - _RESERVED_IDENTIFIER_FIXUP_607;
        float _RESERVED_IDENTIFIER_FIXUP_624 = _1344;
        float _RESERVED_IDENTIFIER_FIXUP_625 = asfloat(ssbo_1_1.Load<uint>(_RESERVED_IDENTIFIER_FIXUP_609 * 4 + 0));
        precise float _1353 = _RESERVED_IDENTIFIER_FIXUP_625 * _RESERVED_IDENTIFIER_FIXUP_622;
        float _RESERVED_IDENTIFIER_FIXUP_626 = _1353;
        precise float _1357 = _RESERVED_IDENTIFIER_FIXUP_626 + _RESERVED_IDENTIFIER_FIXUP_605;
        float _RESERVED_IDENTIFIER_FIXUP_627 = _1357;
        precise float _1361 = _RESERVED_IDENTIFIER_FIXUP_625 * _RESERVED_IDENTIFIER_FIXUP_623;
        float _RESERVED_IDENTIFIER_FIXUP_628 = _1361;
        precise float _1365 = _RESERVED_IDENTIFIER_FIXUP_628 + _RESERVED_IDENTIFIER_FIXUP_606;
        float _RESERVED_IDENTIFIER_FIXUP_629 = _1365;
        precise float _1369 = _RESERVED_IDENTIFIER_FIXUP_625 * _RESERVED_IDENTIFIER_FIXUP_624;
        float _RESERVED_IDENTIFIER_FIXUP_630 = _1369;
        precise float _1373 = _RESERVED_IDENTIFIER_FIXUP_630 + _RESERVED_IDENTIFIER_FIXUP_607;
        float _RESERVED_IDENTIFIER_FIXUP_631 = _1373;
        _RESERVED_IDENTIFIER_FIXUP_632 = _RESERVED_IDENTIFIER_FIXUP_627;
        _RESERVED_IDENTIFIER_FIXUP_633 = _RESERVED_IDENTIFIER_FIXUP_629;
        _RESERVED_IDENTIFIER_FIXUP_634 = _RESERVED_IDENTIFIER_FIXUP_631;
    }
    else
    {
        _RESERVED_IDENTIFIER_FIXUP_632 = _RESERVED_IDENTIFIER_FIXUP_605;
        _RESERVED_IDENTIFIER_FIXUP_633 = _RESERVED_IDENTIFIER_FIXUP_606;
        _RESERVED_IDENTIFIER_FIXUP_634 = _RESERVED_IDENTIFIER_FIXUP_607;
    }
    frag_color0.x = _RESERVED_IDENTIFIER_FIXUP_632;
    frag_color0.y = _RESERVED_IDENTIFIER_FIXUP_633;
    frag_color0.z = _RESERVED_IDENTIFIER_FIXUP_634;
    frag_color0.w = 0.0f;
    renodx_pretonemap = float4(renodx_scene, 1.0f);
    // Below the mid-grey anchor the native filmic output is used as-is, so the toe and grade
    // stay vanilla; above it the gain-matched scene signal extends the highlights.
    float3 renodx_anchored = max(renodx_neutral, renodx_scene * renodx_filmic_gain);
    float renodx_anchor_weight = saturate(renodx::color::y::from::BT709(renodx_neutral) / 0.18f);
    renodx_anchor_weight *= renodx_anchor_weight;
    float3 renodx_untonemapped = lerp(renodx_neutral, renodx_anchored, renodx_anchor_weight);
    renodx_hdr = float4(renodx::draw::ToneMapPass(renodx_untonemapped, pow(max(frag_color0.rgb, 0.0f), 2.2f), renodx_neutral), 1.0f);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    gl_BaryCoordEXT = stage_input.gl_BaryCoordEXT;
    fs_in_attr0_p[0] = GetAttributeAtVertex(stage_input.fs_in_attr0_p, 0);
    fs_in_attr0_p[1] = GetAttributeAtVertex(stage_input.fs_in_attr0_p, 1);
    fs_in_attr0_p[2] = GetAttributeAtVertex(stage_input.fs_in_attr0_p, 2);
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.frag_color0 = frag_color0;
    stage_output.renodx_pretonemap = renodx_pretonemap;
    stage_output.renodx_hdr = renodx_hdr;
    return stage_output;
}

// Port HDR10 chain: the same draw again into a float shadow target. The HDR result is written
// gamma 2.2 encoded, where the game writes its graded SDR value, so the next pass (0x92A23F78,
// which raises it to the power 2.2) reads it unchanged and yields linear HDR.
struct HdrShadowOutput
{
    float4 color : SV_Target0;
};

HdrShadowOutput main_hdr_shadow(SPIRV_Cross_Input stage_input)
{
    gl_BaryCoordEXT = stage_input.gl_BaryCoordEXT;
    fs_in_attr0_p[0] = GetAttributeAtVertex(stage_input.fs_in_attr0_p, 0);
    fs_in_attr0_p[1] = GetAttributeAtVertex(stage_input.fs_in_attr0_p, 1);
    fs_in_attr0_p[2] = GetAttributeAtVertex(stage_input.fs_in_attr0_p, 2);
    frag_main();
    HdrShadowOutput output;
    output.color = float4(sign(renodx_hdr.rgb) * pow(abs(renodx_hdr.rgb), 1.0f / 2.2f), frag_color0.w);
    return output;
}
