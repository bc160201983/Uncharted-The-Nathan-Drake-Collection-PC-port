// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdlib>

#include "common/types.h"
#include "shader_recompiler/ir/type.h"
#include "video_core/amdgpu/resource.h"

#include <boost/container/static_vector.hpp>

namespace Shader {

static constexpr u32 NUM_USER_DATA_REGS = 16;
static constexpr u32 NUM_IMAGES = 64;
static constexpr u32 NUM_BUFFERS = 40;
static constexpr u32 NUM_SAMPLERS = 16;
static constexpr u32 NUM_FMASKS = 8;

using SharpLocation = u16;

constexpr SharpLocation UNKNOWN_LOCATION = std::numeric_limits<u16>::max();

template <typename T>
struct SharpFetch {
    static constexpr std::size_t N = sizeof(T) / sizeof(u32);
    static_assert(N <= 8);

    std::array<u32, N> immediates;
    std::array<SharpLocation, N> offsets;
    u8 load_mask;

    bool operator==(const SharpFetch&) const = default;

    template <u32 num_dwords = N>
        requires(num_dwords <= N)
    constexpr bool Fetch(const u32* flatbuf, T* out) const {
        u8 mask = load_mask;
        for (u32 i = 0; i < num_dwords; i++) {
            if (offsets[i] == UNKNOWN_LOCATION) {
                return false;
            }
        }
        std::array<u32, num_dwords> out_dw;
        for (u32 i = 0; i < num_dwords; i++) {
            out_dw[i] = (mask & 1) ? flatbuf[offsets[i]] : immediates[i];
            mask >>= 1;
        }
        std::memcpy(out, out_dw.data(), sizeof(out_dw));
        return true;
    }
};

enum class SharpFetchPostOp : u8 {
    None,
    // For buffers
    BitwiseOrDw1WithImm,
    OffsetByProgramBase,
    // For images,
    ConvertCubeTo2DArray,
    // For samplers
    DisableAnisoIfSingleLod,
    ForceRepeatXyzClamp,
    ForceLastTexelXyClamp,
    ClearAnisoRatioAndThreshold,
};

enum class BufferType : u8 {
    Guest,
    Flatbuf,
    BdaPagetable,
    FaultBuffer,
    GdsBuffer,
    SharedMemory,
    ClipPlanes,
};

/// gow3 20 Oct 2026: set by the renderer: the bytes of [address, address + size) that are mapped
/// guest memory, counted from address.
inline u64 (*gpu_mapped_extent)(u64 address, u64 size) = nullptr;

/// gow3 20 Oct 2026: Naughty Dog's effect and crowd skinning compute shaders (cs 8f067540,
/// 7d7e7206; all three games) fetch V#s from table slots that hold floats for some effects (the
/// branches using them are not taken). BindBuffers binds those as null by this rule; they now
/// read as null for the specialization and the translation too. Their random strides and formats
/// made a new shader variant and compute pipeline each, built on the GPU thread: cs 8f067540 had
/// 1,177 variants in our cache, and a frame building 465 of them took 9 s on a laptop (GitHub
/// #71). GOW3_EFFECT_SHARP_FILTER=0: the old specialization (binding unchanged either way).
/// 20 Oct 2026 night: cs 0c936dd6 of the same family too (a laptop, 15 min of Uncharted
/// 3: its unmapped 1-3 GB V#s, already bound as null, made 100+ variants, frames up to 0.9 s).
inline bool IsEffectShader(u64 pgm_hash) {
    return pgm_hash == 0x8f067540 || pgm_hash == 0x7d7e7206 || pgm_hash == 0x0c936dd6;
}

/// 20 Oct 2026 night: for every shader, not only the effect family. A list of shaders let the
/// next one through (cs 0c936dd6, 100+ variants on the laptop): any V# BindBuffers binds as null
/// (reaching past mapped memory other than an unbounded raw buffer, or spanning 1 GiB) now reads
/// as null for the specialization and the translation as well, so garbage in a table slot can
/// never make a new variant. Only odd-looking V#s (unaligned base, stride of 8 KiB or more, over
/// 64 MiB: all the garbage seen so far) pay for the mapping lookup; the effect family keeps its
/// stricter rule (odd = garbage, mapped or not).
inline bool IsGarbageEffectBuffer(u64 pgm_hash, const AmdGpu::Buffer& buffer) {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_EFFECT_SHARP_FILTER");
        return !(env && env[0] == '0');
    }();
    if (!enabled || buffer.num_records == 0) {
        return false;
    }
    // gow3 24 Oct 2026 (0.5.0 logs of #144, #132, #24, #69): BindBuffers binds a V# with base 0,
    // or whose 32-bit size (stride * num_records) wraps to 0, as null ("unbound"). Float slots of
    // the effect shaders give exactly those (base 0 with ~1e9 records and a random format; stride
    // 15360 x 0xbf000000 records), and each new float value built another variant: every logged
    // 8f067540 / 7d7e7206 variant past 100 had one. They read as null here as well.
    if (buffer.base_address == 0 || buffer.GetSize() == 0) {
        return true;
    }
    const u64 size = buffer.GetSize();
    const bool odd =
        (buffer.base_address & 3) != 0 || buffer.stride >= 8192 || size > (u64{64} << 20);
    if (IsEffectShader(pgm_hash)) {
        if (odd) {
            return true;
        }
    } else if (!odd) {
        return false;
    }
    if (!gpu_mapped_extent) {
        return false;
    }
    const u64 mapped = gpu_mapped_extent(buffer.base_address, size);
    return mapped >= (u64{1} << 30) || (mapped != size && buffer.num_records < 0xFFFF0000u);
}

struct BufferResource {
    SharpFetch<AmdGpu::Buffer> sharp_fetch{};
    IR::Type used_types{};
    BufferType buffer_type{};
    bool is_written{};
    bool is_formatted{};
    SharpFetchPostOp post_op{};
    u32 post_op_dw1_mask{};
    /// gow3: bytes from a record's start that this shader's accesses can reach (constant offsets
    /// plus access size); 0: none seen, ~0u: unknown (a VGPR offset, a format from the V#).
    /// Lets the barrier tracker use the bytes a strided buffer really covers.
    u32 access_extent{};

    bool IsSpecial() const noexcept {
        return buffer_type != BufferType::Guest;
    }

    constexpr AmdGpu::Buffer GetSharp(const auto& info) const noexcept {
        AmdGpu::Buffer buffer;
        if (!sharp_fetch.Fetch(info.FlatUserData().data(), &buffer)) {
            return AmdGpu::Buffer::Null();
        }
        if (post_op == SharpFetchPostOp::BitwiseOrDw1WithImm) {
            reinterpret_cast<u32*>(&buffer)[1] |= post_op_dw1_mask;
        } else if (post_op == SharpFetchPostOp::OffsetByProgramBase) {
            buffer.base_address += info.ProgramBase();
        }
        if (!buffer.Valid() || IsGarbageEffectBuffer(info.pgm_hash, buffer)) {
            return AmdGpu::Buffer::Null();
        }
        return buffer;
    }
};
using BufferResourceList = boost::container::static_vector<BufferResource, NUM_BUFFERS>;

enum class MipStorageFallbackMode : u16 {
    None,
    DynamicIndex,
    ConstantIndex,
};

struct ImageResource {
    SharpFetch<AmdGpu::Image> sharp_fetch{};
    bool is_depth{};
    bool is_atomic{};
    bool is_array{};
    bool is_written{};
    bool is_r128{};
    /// gow3: read other than by normalized sampling without offsets (texel loads, size
    /// queries, offsets): needs the native-size image, not a reduced scene proxy.
    bool needs_native{};
    u8 constant_mip_index{};
    MipStorageFallbackMode mip_fallback_mode{};
    SharpFetchPostOp post_op{};

    constexpr AmdGpu::Image GetSharp(const auto& info) const noexcept {
        AmdGpu::Image image{};
        if (!Fetch(info.FlatUserData().data(), &image)) {
            return AmdGpu::Image::Null(is_depth);
        }
        if (post_op == SharpFetchPostOp::ConvertCubeTo2DArray) {
            image.type = u64(AmdGpu::ImageType::Color2DArray);
            image.depth = (image.depth + 1) * 6 - 1;
        }
        // gow3: garbage T#s (resource table slots holding other data for this draw: Uncharted
        // 2's treasure effects) stop the decoders: reserved tile modes and swizzles, inverted
        // mip ranges. Such slots are not sampled by the shader; a null image keeps it safe.
        const auto reserved_swizzle = [](u64 sel) { return sel == 2 || sel == 3; };
        if (!image.Valid() || image.tiling_index > 26 || image.last_level < image.base_level ||
            reserved_swizzle(image.dst_sel_x) || reserved_swizzle(image.dst_sel_y) ||
            reserved_swizzle(image.dst_sel_z) || reserved_swizzle(image.dst_sel_w)) {
            image = AmdGpu::Image::Null(is_depth);
        } else if (is_depth) {
            const auto data_fmt = image.GetDataFmt();
            if (data_fmt != AmdGpu::DataFormat::Format16 &&
                data_fmt != AmdGpu::DataFormat::Format32) {
                image = AmdGpu::Image::Null(true);
            }
        }
        return image;
    }

    constexpr bool Fetch(const u32* flatbuf, AmdGpu::Image* out) const {
        if (!is_r128) {
            // Fetch full 8 byte T#
            return sharp_fetch.Fetch(flatbuf, out);
        }
        // Fetch r128 T# and fix pitch
        if (!sharp_fetch.Fetch<4>(flatbuf, out)) {
            return false;
        }
        out->pitch = out->width;
        return true;
    }

    u32 NumBindings(const auto& info) const {
        const AmdGpu::Image tsharp = GetSharp(info);
        return (mip_fallback_mode == MipStorageFallbackMode::DynamicIndex)
                   ? (tsharp.last_level - tsharp.base_level + 1)
                   : 1;
    }
};
using ImageResourceList = boost::container::static_vector<ImageResource, NUM_IMAGES>;

struct SamplerResource {
    SharpFetch<AmdGpu::Sampler> sharp_fetch{};
    SharpFetchPostOp post_op{};
    SharpLocation post_op_tsharp_dw3_off{};
    bool is_depth{};

    constexpr AmdGpu::Sampler GetSharp(const auto& info) const noexcept {
        AmdGpu::Sampler sampler{};
        sharp_fetch.Fetch(info.FlatUserData().data(), &sampler);
        if (post_op == SharpFetchPostOp::DisableAnisoIfSingleLod) {
            const u32 tsharp_dw3 = info.FlatUserData()[post_op_tsharp_dw3_off];
            if (((tsharp_dw3 >> 12) & 0xff) == 0) {
                sampler.max_aniso.Assign(AmdGpu::AnisoRatio::One);
            }
        } else if (post_op == SharpFetchPostOp::ForceRepeatXyzClamp) {
            sampler.clamp_x.Assign(AmdGpu::ClampMode::Wrap);
            sampler.clamp_y.Assign(AmdGpu::ClampMode::Wrap);
            sampler.clamp_z.Assign(AmdGpu::ClampMode::Wrap);
        } else if (post_op == SharpFetchPostOp::ForceLastTexelXyClamp) {
            sampler.clamp_x.Assign(AmdGpu::ClampMode::ClampLastTexel);
            sampler.clamp_y.Assign(AmdGpu::ClampMode::ClampLastTexel);
            sampler.clamp_z.Assign(AmdGpu::ClampMode::Wrap);
        } else if (post_op == SharpFetchPostOp::ClearAnisoRatioAndThreshold) {
            sampler.max_aniso.Assign(AmdGpu::AnisoRatio::One);
            sampler.aniso_threshold.Assign(0);
        }
        return sampler;
    }
};
using SamplerResourceList = boost::container::static_vector<SamplerResource, NUM_SAMPLERS>;

struct FMaskResource {
    SharpLocation sharp_idx;

    constexpr AmdGpu::Image GetSharp(const auto& info) const noexcept {
        return info.template ReadUdSharp<AmdGpu::Image>(sharp_idx);
    }
};
using FMaskResourceList = boost::container::static_vector<FMaskResource, NUM_FMASKS>;

struct PushData {
    static constexpr u32 XOffsetIndex = 0;
    static constexpr u32 YOffsetIndex = 1;
    static constexpr u32 XScaleIndex = 2;
    static constexpr u32 YScaleIndex = 3;
    static constexpr u32 UdRegsIndex = 4;
    static constexpr u32 BufOffsetIndex = UdRegsIndex + NUM_USER_DATA_REGS / 4;
    static constexpr u32 MotionParamIndex = BufOffsetIndex + NUM_BUFFERS / 16 + 1;
    static constexpr u32 SceneSizeIndex = MotionParamIndex + 1;

    float xoffset;
    float yoffset;
    float xscale;
    float yscale;
    std::array<u32, NUM_USER_DATA_REGS> ud_regs;
    std::array<u8, NUM_BUFFERS> buf_offsets;
    u32 motion_param; ///< gow3: object motion parameter index (0: off)
    u32 scene_size; ///< reduced width | height << 16; zero means native pixel coordinates

    void AddOffset(u32 binding, u32 offset) {
        ASSERT(offset < 256 && binding < buf_offsets.size());
        buf_offsets[binding] = offset;
    }
};
static_assert(sizeof(PushData) <= 128,
              "PushData size is greater than minimum size guaranteed by Vulkan spec");

} // namespace Shader
