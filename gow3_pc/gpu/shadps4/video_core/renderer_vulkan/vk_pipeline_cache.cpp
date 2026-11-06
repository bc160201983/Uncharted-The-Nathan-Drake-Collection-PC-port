// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <format>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <tuple>
#include <new>
#include <ranges>
#include <string>
#include <unordered_set>
#include "gow3_threads.h"

#include "common/hash.h"
#include "common/thread.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "gow3_settings.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_depth_stencil_state.h"
#include "video_core/renderer_vulkan/motion_history.h"
#include "video_core/renderer_vulkan/vk_draw_prep.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

extern thread_local bool gow3_assert_throws; // shim/common/assert.cpp

namespace Vulkan {

using Shader::HwStage;
using Shader::Output;
using Shader::SwStage;

struct AsyncShaderCompilation {
    AsyncJob<std::shared_ptr<AsyncShaderCompilation>> job;
    Shader::Info info;
    Shader::RuntimeInfo runtime{};
    std::vector<u32> code, user_data, geometry_code, binary;
    std::unique_ptr<Shader::Pools> pools;
    CompilerPool<Shader::Pools>* pool_cache{};
    std::optional<Shader::IR::Program> ir;
    Shader::StageSpecialization spec;
    Shader::Gcn::FetchShaderSnapshot fetch;
    Shader::Backend::Bindings binding;
    vk::Device device;
    vk::ShaderModule module{};
    size_t permutation{};
    bool initial{};
    ~AsyncShaderCompilation() {
        ir.reset();
        if (pools) {
            pool_cache->Recycle(std::move(pools));
        }
        if (module) {
            device.destroyShaderModule(module);
        }
    }
};

struct AsyncGraphicsCompilation {
    AsyncJob<std::shared_ptr<AsyncGraphicsCompilation>> job;
    PipelineSelection selection;
    std::array<std::unique_ptr<Shader::Info>, MaxShaderStages> infos;
    std::array<std::vector<u32>, MaxShaderStages> user_data;
    std::array<Shader::Gcn::FetchShaderSnapshot, MaxShaderStages> fetch;
    std::array<const Shader::Info*, MaxShaderStages> live_infos;
    GraphicsPipeline::SerializationSupport data;
    std::unique_ptr<GraphicsPipeline> pipeline;
    bool failure_logged = false;
};

constexpr static auto SpirvVersion1_6 = 0x00010600U;

constexpr static std::array DescriptorHeapSizes = {
    vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, 512},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, 1024},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampler, 1024},
};

static u32 MapOutputs(std::span<Shader::OutputMap, 3> outputs, const AmdGpu::VsOutputControl& ctl) {
    u32 num_outputs = 0;

    if (ctl.vs_out_misc_enable) {
        auto& misc_vec = outputs[num_outputs++];
        misc_vec[0] = ctl.use_vtx_point_size ? Output::PointSize : Output::None;
        misc_vec[1] = ctl.use_vtx_edge_flag
                          ? Output::EdgeFlag
                          : (ctl.use_vtx_gs_cut_flag ? Output::GsCutFlag : Output::None);
        misc_vec[2] =
            ctl.use_vtx_kill_flag
                ? Output::KillFlag
                : (ctl.use_vtx_render_target_idx ? Output::RenderTargetIndex : Output::None);
        misc_vec[3] = ctl.use_vtx_viewport_idx ? Output::ViewportIndex : Output::None;
    }

    if (ctl.vs_out_ccdist0_enable) {
        auto& ccdist0 = outputs[num_outputs++];
        ccdist0[0] = ctl.IsClipDistEnabled(0)
                         ? Output::ClipDist0
                         : (ctl.IsCullDistEnabled(0) ? Output::CullDist0 : Output::None);
        ccdist0[1] = ctl.IsClipDistEnabled(1)
                         ? Output::ClipDist1
                         : (ctl.IsCullDistEnabled(1) ? Output::CullDist1 : Output::None);
        ccdist0[2] = ctl.IsClipDistEnabled(2)
                         ? Output::ClipDist2
                         : (ctl.IsCullDistEnabled(2) ? Output::CullDist2 : Output::None);
        ccdist0[3] = ctl.IsClipDistEnabled(3)
                         ? Output::ClipDist3
                         : (ctl.IsCullDistEnabled(3) ? Output::CullDist3 : Output::None);
    }

    if (ctl.vs_out_ccdist1_enable) {
        auto& ccdist1 = outputs[num_outputs++];
        ccdist1[0] = ctl.IsClipDistEnabled(4)
                         ? Output::ClipDist4
                         : (ctl.IsCullDistEnabled(4) ? Output::CullDist4 : Output::None);
        ccdist1[1] = ctl.IsClipDistEnabled(5)
                         ? Output::ClipDist5
                         : (ctl.IsCullDistEnabled(5) ? Output::CullDist5 : Output::None);
        ccdist1[2] = ctl.IsClipDistEnabled(6)
                         ? Output::ClipDist6
                         : (ctl.IsCullDistEnabled(6) ? Output::CullDist6 : Output::None);
        ccdist1[3] = ctl.IsClipDistEnabled(7)
                         ? Output::ClipDist7
                         : (ctl.IsCullDistEnabled(7) ? Output::CullDist7 : Output::None);
    }

    return num_outputs;
}

// The compute stage's runtime info from its registers (the GPU thread's and the precompile's).
static void FillComputeRuntimeInfo(Shader::RuntimeInfo& info, const AmdGpu::ComputeProgram& cs_pgm) {
    info.props.num_user_data = cs_pgm.settings.num_user_regs;
    info.props.num_allocated_vgprs = cs_pgm.settings.num_vgprs * 4;
    info.props.fp_denorm_mode32 = cs_pgm.settings.fp_denorm_mode32;
    info.props.fp_denorm_mode16_64 = cs_pgm.settings.fp_denorm_mode64;
    info.props.fp_round_mode32 = cs_pgm.settings.fp_round_mode32;
    info.props.fp_round_mode16_64 = cs_pgm.settings.fp_round_mode64;
    info.hw.cs.workgroup_size = {cs_pgm.num_thread_x.full, cs_pgm.num_thread_y.full,
                                 cs_pgm.num_thread_z.full};
    info.hw.cs.tgid_enable = {cs_pgm.IsTgidEnabled(0), cs_pgm.IsTgidEnabled(1),
                              cs_pgm.IsTgidEnabled(2)};
    info.hw.cs.shared_memory_size = cs_pgm.SharedMemSize();
}

const Shader::RuntimeInfo& PipelineCache::BuildRuntimeInfo(PipelineSelection& sel, HwStage stage,
                                                             SwStage l_stage) {
    auto& info = sel.runtime_infos[u32(l_stage)];
    const auto& regs = (*sel.regs);
    const auto BuildCommon = [&](const auto& program) {
        info.props.num_user_data = program.settings.num_user_regs;
        info.props.num_input_vgprs = program.settings.vgpr_comp_cnt;
        info.props.num_allocated_vgprs = program.NumVgprs();
        info.props.fp_denorm_mode32 = program.settings.fp_denorm_mode32;
        info.props.fp_denorm_mode16_64 = program.settings.fp_denorm_mode64;
        info.props.fp_round_mode32 = program.settings.fp_round_mode32;
        info.props.fp_round_mode16_64 = program.settings.fp_round_mode64;
    };
    info.Initialize(stage, l_stage);
    switch (stage) {
    case HwStage::Local: {
        BuildCommon(regs.ls_program);
        Shader::TessellationDataConstantBuffer tess_constants{};
        const auto* hull_info = sel.infos[u32(SwStage::TessellationControl)];
        hull_info->ReadTessConstantBuffer(tess_constants);
        info.hw.ls.ls_stride = tess_constants.ls_stride;
        break;
    }
    case HwStage::Hull:
        BuildCommon(regs.hs_program);
        break;
    case HwStage::Export:
        BuildCommon(regs.es_program);
        info.hw.es.vertex_data_size = regs.vgt_esgs_ring_itemsize;
        break;
    case HwStage::Geometry: {
        BuildCommon(regs.gs_program);
        info.hw.gs.num_outputs = MapOutputs(info.hw.gs.outputs, regs.vs_output_control);
        info.hw.gs.output_vertices = regs.vgt_gs_max_vert_out;
        info.hw.gs.num_invocations =
            regs.vgt_gs_instance_cnt.IsEnabled() ? regs.vgt_gs_instance_cnt.count : 1;
        if (regs.stage_enable.raw == AmdGpu::ShaderStageEnable::LsHsEsGs) {
            info.hw.gs.in_primitive = [&]() {
                switch (regs.tess_config.topology) {
                case AmdGpu::TessellationTopology::Point:
                    return AmdGpu::PrimitiveType::PointList;
                case AmdGpu::TessellationTopology::Line:
                    return AmdGpu::PrimitiveType::LineList;
                case AmdGpu::TessellationTopology::TriangleCw:
                case AmdGpu::TessellationTopology::TriangleCcw:
                    return AmdGpu::PrimitiveType::TriangleList;
                default:
                    UNREACHABLE();
                }
            }();
        } else {
            info.hw.gs.in_primitive = regs.primitive_type;
        }
        for (u32 stream_id = 0; stream_id < Shader::GsMaxOutputStreams; ++stream_id) {
            info.hw.gs.out_primitive[stream_id] =
                regs.vgt_gs_out_prim_type.GetPrimitiveType(stream_id);
        }
        info.hw.gs.in_vertex_data_size = regs.vgt_esgs_ring_itemsize;
        info.hw.gs.out_vertex_data_size = regs.vgt_gs_vert_itemsize[0];
        info.hw.gs.mode = regs.vgt_gs_mode.mode;
        const auto params_vc = AmdGpu::GetParams(regs.vs_program);
        info.hw.gs.vs_copy = params_vc.code;
        info.hw.gs.vs_copy_hash = params_vc.hash;
        DumpShader(info.hw.gs.vs_copy, info.hw.gs.vs_copy_hash, Shader::HwStage::Vertex, 0,
                   "copy.bin");
        break;
    }
    case HwStage::Vertex: {
        BuildCommon(regs.vs_program);
        info.hw.vs.user_clip_plane_mask = regs.clipper_control.user_clip_plane_enable;
        info.hw.vs.num_outputs = MapOutputs(info.hw.vs.outputs, regs.vs_output_control);
        info.hw.vs.emulate_depth_negative_one_to_one =
            !instance.IsDepthClipControlSupported() &&
            regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW;
        info.hw.vs.clip_disable = regs.IsClipDisabled();
        info.hw.vs.motion_vectors = sel.motion;
        break;
    }
    case HwStage::Fragment: {
        BuildCommon(regs.ps_program);
        info.hw.fs.en_flags = regs.ps_input_ena;
        info.hw.fs.addr_flags = regs.ps_input_addr;
        info.hw.fs.num_inputs = regs.num_interp;
        info.hw.fs.front_face_all_bits = regs.barycentric_control.front_face_all_bits;
        info.hw.fs.num_samples =
            regs.ps_input_addr.sample_coverage_ena && regs.ps_input_ena.sample_coverage_ena
                ? regs.aa_config.NumSamples()
                : 1;
        info.hw.fs.z_export_format = regs.z_export_format;
        info.hw.fs.motion_vectors = sel.motion;
        u8 stencil_ref_export_enable = regs.depth_shader_control.stencil_op_val_export_enable |
                                       regs.depth_shader_control.stencil_test_val_export_enable;
        info.hw.fs.mrtz_mask = regs.depth_shader_control.z_export_enable |
                               (stencil_ref_export_enable << 1) |
                               (regs.depth_shader_control.mask_export_enable << 2) |
                               (regs.depth_shader_control.coverage_to_mask_enable << 3);
        const auto& cb0_blend = regs.blend_control[0];
        if (cb0_blend.enable) {
            info.hw.fs.dual_source_blending =
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_dst_factor) ||
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_src_factor);
            if (cb0_blend.separate_alpha_blend) {
                info.hw.fs.dual_source_blending |=
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_dst_factor) ||
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_src_factor);
            }
        } else {
            info.hw.fs.dual_source_blending = false;
        }
        const auto& ps_inputs = regs.ps_inputs;
        for (u32 i = 0; i < regs.num_interp; i++) {
            info.hw.fs.inputs[i] = {
                .param_index = u8(ps_inputs[i].input_offset),
                .is_default = bool(ps_inputs[i].use_default),
                .is_flat = bool(ps_inputs[i].flat_shade),
                .default_value = u8(ps_inputs[i].default_value),
            };
        }
        for (u32 i = 0; i < Shader::MaxColorBuffers; i++) {
            info.hw.fs.color_buffers[i] = sel.graphics_key.color_buffers[i];
        }
        // Lowered user clip planes ride the same emulation path as guest-exported distances, so
        // the fragment side arms whenever the hardware vertex stage lowers them, keeping its input
        // locations in sync with the shifted vertex outputs.
        const bool lowers_user_clip_planes =
            regs.clipper_control.user_clip_plane_enable &&
            !regs.stage_enable.IsStageEnabled(static_cast<u32>(HwStage::Geometry));
        info.hw.fs.clip_distance_emulation =
            ((regs.vs_output_control.clip_distance_enable &&
              !regs.stage_enable.IsStageEnabled(static_cast<u32>(HwStage::Local))) ||
             lowers_user_clip_planes) &&
            profile.needs_clip_distance_emulation;
        break;
    }
    case HwStage::Compute: {
        FillComputeRuntimeInfo(info, liverpool->GetCsRegs());
        break;
    }
    default:
        break;
    }
    switch (l_stage) {
    case SwStage::Vertex:
        info.sw.vs.step_rate_0 = regs.vgt_instance_step_rate_0;
        info.sw.vs.step_rate_1 = regs.vgt_instance_step_rate_1;
        info.sw.vs.vertex_sgpr_offset = sel.draw_indirect_params.vertex_sgpr_offset;
        info.sw.vs.instance_sgpr_offset = sel.draw_indirect_params.instance_sgpr_offset;
        info.sw.vs.tess_emulated_primitive =
            regs.primitive_type == AmdGpu::PrimitiveType::RectList ||
            regs.primitive_type == AmdGpu::PrimitiveType::QuadList;
        break;
    case SwStage::TessellationControl: {
        info.sw.tcs.num_input_control_points = regs.ls_hs_config.hs_input_control_points;
        info.sw.tcs.num_threads = regs.ls_hs_config.hs_output_control_points;
        info.sw.tcs.tess_type = regs.tess_config.type;
        info.sw.tcs.offchip_lds_enable = regs.hs_program.settings.oc_lds_en;
        break;
    }
    case SwStage::TessellationEval: {
        info.sw.tes.tess_type = regs.tess_config.type;
        info.sw.tes.tess_topology = regs.tess_config.topology;
        info.sw.tes.tess_partitioning = regs.tess_config.partitioning;
        break;
    }
    default:
        break;
    }
    return info;
}

PipelineCache::PipelineCache(const Instance& instance_, Scheduler& scheduler_,
                             AmdGpu::Liverpool* liverpool_, u32 sparse_page_shift)
    : instance{instance_}, scheduler{scheduler_}, liverpool{liverpool_},
      desc_heap{instance, scheduler.GetWorkSemaphore(), DescriptorHeapSizes} {
    sel.regs = &liverpool->regs;
    const auto& vk12_props = instance.GetVk12Properties();
    // gow3 5 Nov 2026: 1024-thread skinning workgroups as 4 of 256 on NVIDIA (Shader::WorkgroupSplit,
    // runtime_info.h). Set before any shader is loaded: the split shaders have their own cache names.
    {
        const char* env = std::getenv("GOW3_WORKGROUP_SPLIT");
        Shader::WorkgroupSplitOn() =
            env && env[0] ? env[0] == '1'
                          : instance.GetDriverID() == vk::DriverId::eNvidiaProprietary;
        std::printf("GPU: skinning shaders' 1024-thread workgroups %s (GOW3_WORKGROUP_SPLIT)\n",
                    Shader::WorkgroupSplitOn() ? "run as 4 workgroups of 256" : "run whole");
    }
    profile = Shader::Profile{
        .max_viewport_width = instance.GetMaxViewportWidth(),
        .max_viewport_height = instance.GetMaxViewportHeight(),
        .max_shared_memory_size = instance.MaxComputeSharedMemorySize(),
        .supported_spirv = SpirvVersion1_6,
        .subgroup_size = instance.SubgroupSize(),
        .sparse_page_shift = sparse_page_shift,
        .support_int8 = instance.IsShaderInt8Supported(),
        .support_int16 = instance.IsShaderInt16Supported(),
        .support_int64 = instance.IsShaderInt64Supported(),
        .support_float16 = instance.IsShaderFloat16Supported(),
        .support_float64 = instance.IsShaderFloat64Supported(),
        .supports_denorm_behavior_independence =
            vk12_props.denormBehaviorIndependence != vk::ShaderFloatControlsIndependence::eNone,
        .supports_rounding_mode_independence =
            vk12_props.roundingModeIndependence != vk::ShaderFloatControlsIndependence::eNone,
        .support_fp16_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat16),
        .support_fp16_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat16),
        .support_fp16_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat16),
        .support_fp32_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat32),
        .support_fp32_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat32),
        .support_fp32_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat32),
        .support_fp64_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat64),
        .support_fp64_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat64),
        .support_fp64_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat64),
        .support_fp16_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat16),
        .support_fp32_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat32),
        .support_fp64_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat64),
        .supports_image_load_store_lod = instance_.IsImageLoadStoreLodSupported(),
        .supports_native_cube_calc = instance_.IsAmdGcnShaderSupported(),
        .supports_trinary_minmax = instance_.IsAmdShaderTrinaryMinMaxSupported(),
        .supports_buffer_fp32_atomic_min_max =
            instance_.IsShaderAtomicFloatBuffer32MinMaxSupported(),
        .supports_image_fp32_atomic_min_max = instance_.IsShaderAtomicFloatImage32MinMaxSupported(),
        .supports_buffer_int64_atomics = instance_.IsBufferInt64AtomicsSupported(),
        .supports_shared_int64_atomics = instance_.IsSharedInt64AtomicsSupported(),
        .supports_workgroup_explicit_memory_layout =
            instance_.IsWorkgroupMemoryExplicitLayoutSupported(),
        .supports_amd_shader_explicit_vertex_parameter =
            instance_.IsAmdShaderExplicitVertexParameterSupported(),
        .supports_fragment_shader_barycentric = instance_.IsFragmentShaderBarycentricSupported(),
        .supports_shader_subgroup_clock = instance_.IsShaderSubgroupClockSupported(),
        .needs_manual_interpolation = instance.IsFragmentShaderBarycentricSupported() &&
                                      instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
        // gow3: older NVIDIA (Pascal) has no barycentrics; GOW3_INTERP_INT_FIX=0/1 overrides.
        .needs_integer_interpolation_fix = [&] {
            if (const char* env = std::getenv("GOW3_INTERP_INT_FIX")) {
                return env[0] == '1';
            }
            return !instance.IsFragmentShaderBarycentricSupported() &&
                   instance.GetDriverID() == vk::DriverId::eNvidiaProprietary;
        }(),
        .needs_lds_barriers = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary ||
                              instance.GetDriverID() == vk::DriverId::eMesaKosmickrisp,
        .needs_buffer_offsets = instance.StorageMinAlignment() > 4,
        .needs_unorm_fixup = instance.GetDriverID() == vk::DriverId::eMesaKosmickrisp,
        .needs_clip_distance_emulation = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
        .supports_shader_stencil_export = instance_.IsShaderStencilExportSupported(),
    };

    auto [cache_result, cache] = instance.GetDevice().createPipelineCacheUnique({});
    ASSERT_MSG(cache_result == vk::Result::eSuccess, "Failed to create pipeline cache: {}",
               vk::to_string(cache_result));
    pipeline_cache = std::move(cache);
    // gow3: the compiler threads always exist; the in-game menu switches async compilation live
    // (Gow3Settings::async_shaders, GOW3_ASYNC_SHADERS at start).
    {
        const auto count = std::clamp(Gow3Threads::Available() / 2, 1u, 6u);
        const auto capture_count = std::min(2u, std::max(1u, Gow3Threads::Available() / 4));
        for (unsigned i = 0; i < count; ++i) {
            auto [result, worker_cache] = instance.GetDevice().createPipelineCacheUnique({});
            ASSERT_MSG(result == vk::Result::eSuccess, "Failed to create compiler pipeline cache");
            worker_pipeline_caches.push_back(std::move(worker_cache));
        }
        capture_compiler = std::make_unique<AsyncCompiler>(capture_count, 8, [](unsigned index) {
            Common::SetCurrentThreadName(("gow3:ShaderCapture" + std::to_string(index)).c_str());
            Common::SetCurrentThreadPriority(Common::ThreadPriority::High);
        });
        compiler = std::make_unique<AsyncCompiler>(count, 64, [](unsigned index) {
            Common::SetCurrentThreadName(("gow3:ShaderCompile" + std::to_string(index)).c_str());
            Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
        });
        LOG_INFO(Render_Vulkan, "gow3: async graphics compilation ready ({} capture, {} compile workers)",
                 capture_count, count);
    }
}

PipelineCache::~PipelineCache() {
    StopWarmUp();
    FinishCompilations();
}

// gow3: shader/pipeline compile time on the GPU thread, reported by GOW3_FRAME_STATS.
std::atomic<u64> g_gow3_compile_ns;
std::atomic<u32> g_gow3_compiles;
// gow3 18 Oct 2026: the same by kind (GOW3_FRAME_STATS stall lines): compute shader, compute
// shader variant, graphics shader, graphics shader variant, compute pipeline, graphics pipeline.
std::atomic<u64> g_gow3_compile_kind_ns[6];
std::atomic<u32> g_gow3_compile_kind_count[6];
std::atomic<u64> g_gow3_capture_ns, g_gow3_async_ns, g_gow3_compile_wait_ns;
std::atomic<u64> g_gow3_snapshot_ns, g_gow3_guest_wait_ns;
std::atomic<u32> g_gow3_pool_reuses;
std::atomic<u32> g_gow3_pending_draws;
namespace {
struct PhaseTimer {
    std::atomic<u64>& counter;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    bool active = true;
    void Stop() {
        if (!active) {
            return;
        }
        counter += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now() - start).count());
        active = false;
    }
    ~PhaseTimer() { Stop(); }
};
struct CompileTimer {
    enum Kind : u32 { ComputeShader, ComputeVariant, GraphicsShader, GraphicsVariant, ComputePipe, GraphicsPipe };
    u32 kind;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    explicit CompileTimer(u32 kind_) : kind{kind_} {}
    ~CompileTimer() {
        const u64 ns = u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now() - start)
                               .count());
        g_gow3_compile_ns += ns;
        ++g_gow3_compiles;
        g_gow3_compile_kind_ns[kind] += ns;
        ++g_gow3_compile_kind_count[kind];
    }
};
} // namespace

bool PipelineCache::QueueShader(Program& program, HwStage stage, SwStage l_stage,
                                 const Shader::ShaderParams& params,
                                 Shader::RuntimeInfo runtime_info,
                                 Shader::Backend::Bindings binding, size_t permutation,
                                 bool initial,
                                 std::optional<Shader::StageSpecialization> specialization) {
    if (!compiler->HasCapacity()) {
        return false;
    }
    PhaseTimer snapshot{g_gow3_snapshot_ns};
    auto build = std::make_shared<AsyncShaderCompilation>();
    build->device = instance.GetDevice();
    build->code.assign(params.code.begin(), params.code.end());
    build->user_data.assign(params.user_data.begin(), params.user_data.end());
    build->info = Shader::Info(stage, l_stage, params);
    build->info.user_data = build->user_data;
    build->runtime = runtime_info;
    if (stage == HwStage::Geometry && !runtime_info.hw.gs.vs_copy.empty()) {
        build->geometry_code.assign(runtime_info.hw.gs.vs_copy.begin(), runtime_info.hw.gs.vs_copy.end());
        build->runtime.hw.gs.vs_copy = build->geometry_code;
    }
    build->binding = binding;
    build->permutation = permutation;
    build->initial = initial;
    if (specialization) {
        build->spec = *specialization;
    }
    build->pool_cache = &compiler_pools;
    snapshot.Stop();
#ifdef _WIN32
    const bool for_gpu_thread = static_cast<u32>(gettid()) == liverpool->GetGpuCommandProcessorThreadId();
#else
    const bool for_gpu_thread = std::this_thread::get_id() == liverpool->GetGpuCommandProcessorThread();
#endif
    AsyncJob<bool> capture_job;
    if (!capture_job.Start(*capture_compiler, [this, build, binding, for_gpu_thread] {
        PhaseTimer capture{g_gow3_capture_ns};
        struct ForGpuThread {
            explicit ForGpuThread(bool on) { capturing_for_gpu_thread = on; }
            ~ForGpuThread() { capturing_for_gpu_thread = false; }
        } for_gpu{for_gpu_thread};
        auto [pools, reused] = compiler_pools.Acquire();
        build->pools = std::move(pools);
        if (reused) {
            ++g_gow3_pool_reuses;
        }
        // gow3: the GPU thread waits until all guest reads and specialization are captured.
        build->ir.emplace(Shader::TranslateProgram(build->code, *build->pools, build->info,
                                                   build->runtime, profile));
        build->spec = Shader::StageSpecialization(build->info, build->runtime, profile, binding);
        build->fetch.Capture(build->info);
        return true;
    })) {
        return false;
    }
    {
        PhaseTimer waiting{g_gow3_guest_wait_ns};
        capture_job.Wait();
    }
    if (!capture_job.Poll()) {
        if (initial) {
            program.compilation_failed = true;
        } else {
            specialization->info = &program.info;
            program.failed_permutations.push_back(std::move(*specialization));
        }
        LOG_ERROR(Render_Vulkan, "gow3: shader {:#x}/{} capture failed: {}", params.hash,
                  permutation, capture_job.Error());
        return false;
    }
    const bool queued = build->job.Start(*compiler, [this, build] {
        PhaseTimer compile{g_gow3_async_ns};
        Shader::Gcn::ScopedFetchShaderSnapshot snapshot{build->fetch};
        auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, build->runtime, *build->ir,
                                                    build->binding);
        DumpShader(build->code, build->info.pgm_hash, build->info.hw_stage, build->permutation, "bin");
        DumpShader(spv, build->info.pgm_hash, build->info.hw_stage, build->permutation, "spv");
        auto patch = GetShaderPatch(build->info.pgm_hash, build->info.hw_stage,
                                    build->permutation, "spv");
        build->module = CompileSPV(patch && EmulatorSettings.IsPatchShaders() ? *patch : spv,
                                    instance.GetDevice());
        build->binary = std::move(spv);
        build->ir.reset();
        compiler_pools.Recycle(std::move(build->pools));
        return build;
    }, true);
    if (queued) {
        program.pending = std::move(build);
        pending_programs.push_back(&program);
    }
    return queued;
}

void PipelineCache::PublishShaders(bool wait) {
    for (size_t i = 0; i < pending_programs.size();) {
        auto* program = pending_programs[i];
        const auto hash = program->info.pgm_hash;
        const auto retire = [&] {
            pending_programs[i] = pending_programs.back();
            pending_programs.pop_back();
        };
        auto build = program->pending;
        if (wait) {
            build->job.Wait();
        }
        auto result = build->job.Poll();
        if (!result) {
            if (build->job.Failed()) {
                if (build->initial) {
                    program->compilation_failed = true;
                } else {
                    build->spec.info = &program->info;
                    program->failed_permutations.push_back(std::move(build->spec));
                }
                program->pending.reset();
                LOG_ERROR(Render_Vulkan, "gow3: shader {:#x}/{} compilation failed: {}", hash,
                          build->permutation, build->job.Error());
                retire();
            } else {
                ++i;
            }
            continue;
        }
        const auto perm_hash = HashCombine(hash, build->permutation);
        Vulkan::SetObjectName(instance.GetDevice(), build->module,
                             GetShaderName(build->info.hw_stage, hash, build->permutation));
        RegisterShaderBinary(std::move(build->binary), hash, build->permutation,
                             build->info.hw_stage == Shader::HwStage::Compute);
        {
            std::unique_lock lock{programs_mutex};
            if (build->initial) {
                program->info = std::move(build->info);
                // user_data is refreshed on the next lookup; no pointer into a completed job survives.
                program->info.user_data = {};
                program->info_template = std::make_unique<Shader::Info>(program->info);
            }
            build->spec.info = &program->info;
            RegisterShaderMeta(program->info, build->spec.fetch_shader_data, build->spec,
                               perm_hash, build->permutation);
            program->AddPermut(build->module, std::move(build->spec));
            build->module = nullptr;
            program->pending.reset();
        }
        retire();
    }
}

void PipelineCache::PublishGraphics(bool wait) {
    // gow3: called for every draw and dispatch. begin() of a robin map scans its buckets, and
    // the map never shrinks after a loading burst: nothing to do when no build is pending.
    if (pending_graphics.empty()) {
        return;
    }
    for (auto it = pending_graphics.begin(); it != pending_graphics.end();) {
        auto build = it.value();
        if (wait) {
            build->job.Wait();
        }
        auto result = build->job.Poll();
        if (!result) {
            if (build->job.Failed() && !build->failure_logged) {
                LOG_ERROR(Render_Vulkan, "gow3: pipeline {:#x} compilation failed: {}",
                          std::hash<GraphicsPipelineKey>{}(it.key()), build->job.Error());
                build->failure_logged = true;
            }
            if (build->job.Failed()) {
                // gow3: kept out of pending_graphics (so it can become empty) and never queued
                // again, as when it stayed there; a synchronous draw compiles it itself.
                failed_graphics.emplace(it.key(), std::move(build));
                it = pending_graphics.erase(it);
                continue;
            }
            ++it;
            continue;
        }
        const auto key = it.key();
        build->pipeline->PublishStages(build->live_infos);
        RegisterPipelineData(key, std::hash<GraphicsPipelineKey>{}(key), build->data);
        graphics_pipelines.emplace(key, std::move(build->pipeline));
        ++num_new_pipelines;
        it = pending_graphics.erase(it);
    }
}

void PipelineCache::FinishCompilations() {
    if (compiler) {
        capture_compiler->Stop();
        compiler->Stop();
        PublishShaders(true);
        PublishGraphics(true);
        std::vector<vk::PipelineCache> sources;
        for (const auto& cache : worker_pipeline_caches) {
            sources.push_back(*cache);
        }
        const auto result = instance.GetDevice().mergePipelineCaches(*pipeline_cache, sources);
        if (result != vk::Result::eSuccess) {
            LOG_ERROR(Render_Vulkan, "gow3: compiler cache merge failed: {}", vk::to_string(result));
        }
    }
}

bool PipelineCache::AsyncCompilation() const {
    return compiler && !sync_compile &&
           Gow3Settings::Get().async_shaders.load(std::memory_order_relaxed);
}

namespace {
// gow3: a draw whose color targets are all small (lookup tables, 1x1 luminance, the end of
// downsample chains). Such draws are often made once: Uncharted's 256x8 light ramp is drawn at
// load, and skipped while its pipeline compiled in the background the torch and flashlights lit
// nothing for the whole session (Uncharted 1 got synchronous compilation for it, 14 Oct; U2
// showed the same, 15 Oct). Compiling these at once costs a short stall the first time only.
bool SmallTargetDraw(const AmdGpu::Regs& regs) {
    bool any = false;
    for (u32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        if (!col_buf || !regs.color_target_mask.GetMask(cb)) {
            continue;
        }
        if (u64(col_buf.Pitch()) * col_buf.Height() > 256 * 256) {
            return false;
        }
        any = true;
    }
    return any;
}
} // namespace

void PipelineCache::SeedWorkerCaches() {
    if (!compiler || worker_caches_seeded) {
        return;
    }
    for (const auto& cache : worker_pipeline_caches) {
        const auto result = instance.GetDevice().mergePipelineCaches(*cache, *pipeline_cache);
        ASSERT_MSG(result == vk::Result::eSuccess, "Failed to seed compiler pipeline cache");
    }
    std::atomic_ref<bool>{worker_caches_seeded}.store(true, std::memory_order_release);
}

void PipelineCache::ReportCompilerStats() {
    if (compiler) {
        std::printf("Compiler stats: capture queue %zu, compile queue %zu; queue waits: capture "
                    "%.1f ms, compile %.1f ms; pools retained %zu, reused %u\n",
                    capture_compiler->Depth(), compiler->Depth(),
                    capture_compiler->QueueWaitNs() / 1e6, compiler->QueueWaitNs() / 1e6,
                    compiler_pools.Retained(), g_gow3_pool_reuses.exchange(0));
        const u32 queued = precompile_stats[0].exchange(0), adopted = precompile_stats[1].exchange(0),
                  waited = precompile_stats[2].exchange(0), taken = precompile_stats[3].exchange(0),
                  failed = precompile_stats[4].exchange(0);
        if (queued || adopted || waited || taken || failed) {
            std::printf("Compute precompile (5 s): %u queued, %u ready when needed, %u waited for "
                        "(%.1f ms), %u compiled by the GPU thread instead, %u failed\n",
                        queued, adopted, waited, precompile_wait_ns.exchange(0) / 1e6, taken, failed);
        }
    }
}

// gow3 18 Oct 2026: compute shader precompile (Liverpool's scanner -> here -> compiler workers).
struct PipelineCache::PrecompiledCompute {
    enum State : u32 { Queued, Running, Done, Cancelled };
    std::atomic<u32> state{Queued};
    u64 hash = 0;
    AmdGpu::ComputeProgram cs{};
    std::unique_ptr<Program> program;
    std::unique_ptr<Shader::Pools> pools;
    std::optional<Shader::IR::Program> ir;
    std::optional<Shader::StageSpecialization> spec;
    std::vector<u32> spirv;
    ComputePipelineKey key{};
    std::unique_ptr<ComputePipeline> pipeline;
    ComputePipeline::SerializationSupport sdata{};
    bool ok = false;
    bool waited = false; ///< the GPU thread waited for it (statistics)
};

namespace {
// SearchBinaryInfo without its assert: null when the address holds no shader binary.
// (gow3_assert_throws: shim/common/assert.cpp.)
const AmdGpu::BinaryInfo* FindBinaryInfo(const u32* code) {
    constexpr u32 token_mov_vcchi = 0xBEEB03FF;
    if (code[0] == token_mov_vcchi) {
        const auto* info = std::bit_cast<const AmdGpu::BinaryInfo*>(code + (code[1] + 1) * 2);
        if (info->Valid()) {
            return info;
        }
    }
    for (const u32* it = code; it < code + 0x4000; ++it) {
        if (const auto* info = std::bit_cast<const AmdGpu::BinaryInfo*>(it); info->Valid()) {
            return info;
        }
    }
    return nullptr;
}
} // namespace

void PipelineCache::PrecompileCompute(const AmdGpu::ComputeProgram& cs) {
    // Not before the GPU thread has seeded the workers' pipeline caches (its first draw: a merge
    // into a cache a worker uses would race), and at most half the compiler queue, which the
    // background graphics compiles share (draws are skipped while theirs wait).
    if (!compiler || !cs.Address<const u32*>() ||
        !std::atomic_ref<bool>{worker_caches_seeded}.load(std::memory_order_acquire) ||
        compiler->Depth() >= 32) {
        return;
    }
    // The program's key from its binary info in guest memory; a fault or no binary: skipped.
    u64 hash = 0;
    {
        sigjmp_buf recover;
        if (sigsetjmp(recover, 0)) {
            runtime_fault_recover = nullptr;
            return;
        }
        runtime_fault_recover = &recover;
        const auto* code = cs.Address<const u32*>();
        if (const auto* bininfo = FindBinaryInfo(code); bininfo && bininfo->length >= 4) {
            hash = Gow3Movie::ShaderKey(bininfo->shader_hash, code, bininfo->length / sizeof(u32));
        }
        runtime_fault_recover = nullptr;
    }
    if (hash == 0) {
        return;
    }
    {
        std::shared_lock lock{programs_mutex};
        if (program_cache.contains(hash)) {
            return;
        }
    }
    auto job = std::make_shared<PrecompiledCompute>();
    job->hash = hash;
    job->cs = cs;
    std::scoped_lock lock{precompile_mutex};
    if (precompile_jobs.contains(hash) || precompile_jobs.size() >= 512) {
        return;
    }
    if (!compiler->Submit([this, job] {
            // A GPU library assertion on this guess (stale registers or tables) throws here
            // instead of stopping the game; the GPU thread compiles the real dispatch itself.
            gow3_assert_throws = true;
            std::string error;
            try {
                RunPrecompile(*job);
            } catch (const std::exception& exception) {
                error = exception.what();
            } catch (...) {
                error = "unknown error";
            }
            gow3_assert_throws = false;
            if (!error.empty()) {
                runtime_fault_recover = nullptr;
                static std::atomic<u32> logged{0};
                if (logged.fetch_add(1, std::memory_order_relaxed) < 16) {
                    std::printf("Compute precompile: shader %#llx left to the GPU thread (%s)\n",
                                static_cast<unsigned long long>(job->hash), error.c_str());
                }
                ++precompile_stats[4];
                job->ok = false;
                std::scoped_lock lock{precompile_mutex};
                job->state = PrecompiledCompute::Done;
            }
            precompile_ready.store(true, std::memory_order_release);
            precompile_cv.notify_all();
        })) {
        return;
    }
    precompile_jobs.emplace(hash, std::move(job));
    ++precompile_stats[0];
}

void PipelineCache::RunPrecompile(PrecompiledCompute& job) {
    u32 expected = PrecompiledCompute::Queued;
    if (!job.state.compare_exchange_strong(expected, PrecompiledCompute::Running)) {
        return; // taken back by the GPU thread
    }
    const auto finish = [&](bool ok) {
        job.ok = ok;
        std::scoped_lock lock{precompile_mutex};
        job.state = PrecompiledCompute::Done;
    };
    Shader::RuntimeInfo runtime_info{};
    runtime_info.Initialize(HwStage::Compute, SwStage::Compute);
    FillComputeRuntimeInfo(runtime_info, job.cs);
    const Shader::Backend::Bindings start{};
    // Translation and specialization read guest memory (code, resource tables) the game may have
    // reused since the scan: a fault abandons the job. Its partial objects are leaked then.
    {
        sigjmp_buf recover;
        if (sigsetjmp(recover, 0)) {
            runtime_fault_recover = nullptr;
            (void)job.program.release();
            (void)job.pools.release();
            if (job.ir) {
                new (&job.ir) std::optional<Shader::IR::Program>{}; // leaked with its pools
            }
            ++precompile_stats[4];
            finish(false);
            return;
        }
        runtime_fault_recover = &recover;
        const auto params = AmdGpu::GetParams(job.cs);
        job.program = std::make_unique<Program>(HwStage::Compute, SwStage::Compute, params);
        job.pools = compiler_pools.Acquire().first;
        job.ir.emplace(Shader::TranslateProgram(params.code, *job.pools, job.program->info,
                                                runtime_info, profile));
        job.spec.emplace(job.program->info, runtime_info, profile, start);
        runtime_fault_recover = nullptr;
    }
    auto binding = start;
    auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, *job.ir, binding);
    job.ir.reset();
    compiler_pools.Recycle(std::move(job.pools));
    const auto module = CompileSPV(spv, instance.GetDevice());
    job.spirv = std::move(spv);
    job.program->AddPermut(module, std::move(*job.spec));
    job.spec.reset();
    job.key.value = HashCombine(job.hash, 0);
    job.pipeline = std::make_unique<ComputePipeline>(
        instance, scheduler, desc_heap, profile, *worker_pipeline_caches[AsyncCompiler::WorkerIndex()],
        job.key, job.program->info, module, job.sdata, false);
    finish(true);
}

void PipelineCache::AdoptPrecompiled() {
    if (!precompile_ready.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    std::vector<std::shared_ptr<PrecompiledCompute>> done;
    {
        std::scoped_lock lock{precompile_mutex};
        for (auto it = precompile_jobs.begin(); it != precompile_jobs.end();) {
            if (it->second->state.load() == PrecompiledCompute::Done) {
                done.push_back(it->second);
                it = precompile_jobs.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& job : done) {
        Adopt(*job);
    }
}

void PipelineCache::Adopt(PrecompiledCompute& job) {
    if (!job.ok || !job.program) {
        return;
    }
    if (program_cache.contains(job.hash)) {
        // The GPU thread compiled it meanwhile.
        job.pipeline.reset();
        for (auto& module : job.program->modules) {
            instance.GetDevice().destroyShaderModule(module.module);
        }
        return;
    }
    auto& program = job.program;
    const u64 perm_hash = HashCombine(job.hash, 0);
    RegisterShaderBinary(std::move(job.spirv), job.hash, 0, true); // compute precompile
    // user_data pointed into the job's registers: refreshed by the next lookup.
    program->info.user_data = {};
    RegisterShaderMeta(program->info, program->modules[0].spec.fetch_shader_data,
                       program->modules[0].spec, perm_hash, 0);
    program->info_template = std::make_unique<Shader::Info>(program->info);
    {
        std::unique_lock lock{programs_mutex};
        program_cache.emplace(job.hash, std::move(program));
    }
    if (const auto [it, is_new] = compute_pipelines.try_emplace(job.key); is_new) {
        it.value() = std::move(job.pipeline);
        RegisterPipelineData(job.key, job.sdata);
        ++num_new_pipelines;
    }
    if (!job.waited) {
        ++precompile_stats[1];
    }
}

bool PipelineCache::TakePrecompiled(u64 hash) {
    std::shared_ptr<PrecompiledCompute> job;
    {
        std::unique_lock lock{precompile_mutex};
        const auto it = precompile_jobs.find(hash);
        if (it == precompile_jobs.end()) {
            return false;
        }
        job = it->second;
        u32 expected = PrecompiledCompute::Queued;
        if (job->state.compare_exchange_strong(expected, PrecompiledCompute::Cancelled)) {
            // Not started (the workers are busy): compiling it here is as fast as waiting.
            precompile_jobs.erase(hash);
            ++precompile_stats[3];
            return false;
        }
        if (job->state.load() != PrecompiledCompute::Done) {
            const auto start = std::chrono::steady_clock::now();
            precompile_cv.wait_for(lock, std::chrono::seconds{2}, [&] {
                return job->state.load() == PrecompiledCompute::Done;
            });
            precompile_wait_ns += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now() - start).count());
            ++precompile_stats[2];
            job->waited = true;
            if (job->state.load() != PrecompiledCompute::Done) {
                return false; // compiled here; the job's result is dropped when adopted
            }
        }
        precompile_jobs.erase(hash);
    }
    Adopt(*job);
    return job->ok && program_cache.contains(hash);
}

bool PipelineCache::PrepareGraphicsPipeline(PipelineSelection& worker_sel) {
    // Tessellation stages read constant buffers from memory at selection time: not prepared.
    if (worker_sel.regs->stage_enable.hs_en) {
        return false;
    }
    return RefreshGraphicsKey(worker_sel) && !worker_sel.worker->failed;
}

const GraphicsPipeline* PipelineCache::TryPreparedPipeline(const PreparedDraw& prepared) {
    if (prepared.state.load(std::memory_order_acquire) != PreparedDraw::Ready ||
        prepared.reg_checksum != liverpool->gfx_reg_checksum) {
        return nullptr;
    }
    // Same registers; the stage programs and their flattened user data (which also covers
    // everything read from guest memory for the specialization) must match as well. This is
    // the per-stage work GetProgram does on the regular path.
    const auto& regs = liverpool->regs;
    for (u32 i = 0; i < prepared.num_stages; ++i) {
        const auto& stage = prepared.stages[i];
        const auto* pgm = regs.ProgramForStage(static_cast<u32>(stage.hw_stage));
        if (!pgm || !pgm->Address<u32*>()) {
            return nullptr;
        }
        const auto params = AmdGpu::GetParams(*pgm);
        if (params.hash != stage.hash) {
            return nullptr;
        }
        auto& info = const_cast<Program*>(stage.program)->info;
        info.pgm_base = params.Base();
        info.user_data = params.user_data;
        info.RefreshFlatBuf();
        if (info.pgm_base != stage.pgm_base || info.flattened_ud_buf.size() != stage.flat_size ||
            std::memcmp(info.flattened_ud_buf.data(), stage.flat,
                        stage.flat_size * sizeof(u32)) != 0) {
            return nullptr;
        }
    }
    // gow3 21 Oct 2026 (GOW3_CPU_TRIMS=1): with the hash the worker computed (key_hash is 0 when
    // the switch is off).
    const auto it = prepared.key_hash ? graphics_pipelines.find(prepared.key, prepared.key_hash)
                                      : graphics_pipelines.find(prepared.key);
    return it != graphics_pipelines.end() && !it->second->Failed() ? it->second.get() : nullptr;
}

const GraphicsPipeline* PipelineCache::GetGraphicsPipeline(const DrawIndirectParams params,
                                                           const PreparedDraw* prepared) {
    SeedWorkerCaches();
    PublishShaders();
    PublishGraphics();
    PumpWarmUp();
    used_prepared = nullptr;
    if (prepared) {
        if (const auto* pipeline = TryPreparedPipeline(*prepared)) {
            used_prepared = prepared;
            return pipeline;
        }
    }
    sel.draw_indirect_params = params;
    sync_compile = SmallTargetDraw(liverpool->regs);
    struct SyncReset {
        bool& flag;
        ~SyncReset() {
            flag = false;
        }
    } sync_reset{sync_compile};
    if (!RefreshGraphicsKey(sel)) {
        if (sel.compilation_pending) {
            ++g_gow3_pending_draws;
        }
        return nullptr;
    }
    if (AsyncCompilation() && !sel.regs->stage_enable.hs_en && !EmulatorSettings.IsShaderCollect()) {
        const auto found = graphics_pipelines.find(sel.graphics_key);
        if (found != graphics_pipelines.end()) {
            return found->second->Failed() ? nullptr : found->second.get();
        }
        if (!pending_graphics.contains(sel.graphics_key) &&
            (failed_graphics.empty() || !failed_graphics.contains(sel.graphics_key)) &&
            compiler->HasCapacity()) {
            PhaseTimer capture{g_gow3_snapshot_ns};
            auto build = std::make_shared<AsyncGraphicsCompilation>();
            build->selection = sel;
            build->selection.regs = nullptr;
            build->live_infos = sel.infos;
            for (u32 i = 0; i < MaxShaderStages; ++i) {
                if (sel.infos[i]) {
                    build->infos[i] = std::make_unique<Shader::Info>(*sel.infos[i]);
                    build->user_data[i].assign(sel.infos[i]->user_data.begin(), sel.infos[i]->user_data.end());
                    build->infos[i]->user_data = build->user_data[i];
                    build->selection.infos[i] = build->infos[i].get();
                    build->fetch[i].Capture(*build->infos[i]);
                }
            }
            const bool queued = build->job.Start(*compiler, [this, build] {
                // Only the vertex stage uses fetch-shader guest pointers during pipeline construction.
                Shader::Gcn::ScopedFetchShaderSnapshot snapshot{build->fetch[u32(SwStage::Vertex)]};
                PhaseTimer compile{g_gow3_async_ns};
                auto& s = build->selection;
                build->pipeline = std::make_unique<GraphicsPipeline>(
                    instance, scheduler, desc_heap, profile, s.graphics_key,
                    *worker_pipeline_caches[AsyncCompiler::WorkerIndex()],
                    s.infos, s.runtime_infos, s.fetch_shader, s.modules, build->data, false);
                return build;
            });
            if (queued) {
                pending_graphics.emplace(sel.graphics_key, std::move(build));
            }
        }
        ++g_gow3_pending_draws;
        return nullptr;
    }
    if (sync_compile) {
        // Queued in the background earlier: waited for, not compiled twice.
        if (const auto pending = pending_graphics.find(sel.graphics_key);
            pending != pending_graphics.end()) {
            pending.value()->job.Wait();
            PublishGraphics();
            if (const auto found = graphics_pipelines.find(sel.graphics_key);
                found != graphics_pipelines.end()) {
                return found->second->Failed() ? nullptr : found->second.get();
            }
        }
    }
    const auto [it, is_new] = graphics_pipelines.try_emplace(sel.graphics_key);
    if (is_new) {
        const auto pipeline_hash = std::hash<GraphicsPipelineKey>{}(sel.graphics_key);
        LOG_INFO(Render_Vulkan, "Compiling graphics pipeline {:#x}", pipeline_hash);
        CompileTimer timer{CompileTimer::GraphicsPipe};

        GraphicsPipeline::SerializationSupport sdata{};
        it.value() = std::make_unique<GraphicsPipeline>(
            instance, scheduler, desc_heap, profile, sel.graphics_key, *pipeline_cache, sel.infos,
            sel.runtime_infos, sel.fetch_shader, sel.modules, sdata, false);

        RegisterPipelineData(sel.graphics_key, pipeline_hash, sdata);
        ++num_new_pipelines;

        if (EmulatorSettings.IsShaderCollect()) {
            for (auto stage = 0; stage < MaxShaderStages; ++stage) {
                if (sel.infos[stage]) {
                    auto& m = sel.modules[stage];
                    module_related_pipelines[m].emplace_back(sel.graphics_key);
                }
            }
        }
        sel.fetch_shader.reset();
    }
    return it->second->Failed() ? nullptr : it->second.get();
}

const ComputePipeline* PipelineCache::GetComputePipeline() {
    PublishShaders();
    PublishGraphics();
    PumpWarmUp();
    AdoptPrecompiled();
    if (!RefreshComputeKey()) {
        return nullptr;
    }
    const auto [it, is_new] = compute_pipelines.try_emplace(compute_key);
    if (is_new) {
        const auto pipeline_hash = std::hash<ComputePipelineKey>{}(compute_key);
        LOG_INFO(Render_Vulkan, "Compiling compute pipeline {:#x}", pipeline_hash);
        CompileTimer timer{CompileTimer::ComputePipe};

        ComputePipeline::SerializationSupport sdata{};
        it.value() = std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile,
                                                       *pipeline_cache, compute_key, *sel.infos[0],
                                                       sel.modules[0], sdata, false);
        RegisterPipelineData(compute_key, sdata);
        ++num_new_pipelines;

        if (EmulatorSettings.IsShaderCollect()) {
            auto& m = sel.modules[0];
            module_related_pipelines[m].emplace_back(compute_key);
        }
        // gow3 20 Oct 2026 (GitHub #95): a shader that keeps making pipeline variants is named
        // in the log. In 0.4.1 two effect shaders built ~10,000 compute pipelines over a U2
        // session (RAM and video memory rising, the frame rate falling); the port normally holds
        // ~30. Logged at 100, 1,000 and 10,000 variants of one shader.
        static std::unordered_map<u64, u32> variants_per_shader;
        const u64 shader = sel.infos[0]->pgm_hash;
        const u32 variants = ++variants_per_shader[shader];
        if (variants == 100 || variants == 1000 || variants == 10000) {
            std::printf("GPU: compute shader %016llx has built %u pipeline variants (far more than "
                        "usual: memory use grows with each; a log with this line helps)\n",
                        static_cast<unsigned long long>(shader), variants);
            std::fflush(stdout);
        }
        // What the next new variants were made for (their buffer V#s), to find what varies.
        if (variants > 100 && variants <= 106) {
            const auto& info = *sel.infos[0];
            std::string text;
            for (u32 i = 0; i < info.buffers.size() && i < 12; ++i) {
                const auto& desc = info.buffers[i];
                if (desc.IsSpecial()) {
                    continue;
                }
                const auto sharp = desc.GetSharp(info);
                text += fmt::format(" [{}{} {:#x} stride {} records {} fmt {}/{}]", i,
                                    desc.is_written ? "w" : "", sharp.base_address, sharp.GetStride(),
                                    sharp.num_records, u32(sharp.GetDataFmt()), u32(sharp.GetNumberFmt()));
            }
            std::printf("GPU: compute shader %016llx variant %u buffers:%s\n",
                        static_cast<unsigned long long>(shader), variants, text.c_str());
            std::fflush(stdout);
        }
    }
    return it->second.get();
}

bool PipelineCache::RefreshGraphicsKey(PipelineSelection& sel) {
    std::memset(&sel.graphics_key, 0, sizeof(GraphicsPipelineKey));
    const auto& regs = (*sel.regs);
    auto& key = sel.graphics_key;

    // gow3: a depth/stencil buffer that cannot affect the draw is not attached: on the PC a
    // smaller one would crop a full-screen pass (eltutz, vk_depth_stencil_state.h).
    const bool db_enabled = GetEffectiveDepthStencilState(regs).needs_attachment;

    key.z_format = db_enabled && regs.depth_buffer.DepthValid()
                       ? regs.depth_buffer.z_info.format
                       : AmdGpu::DepthBuffer::ZFormat::Invalid;
    key.stencil_format = db_enabled && regs.depth_buffer.StencilValid()
                             ? regs.depth_buffer.stencil_info.format
                             : AmdGpu::DepthBuffer::StencilFormat::Invalid;
    key.depth_clamp_enable = !regs.depth_render_override.disable_viewport_clamp;
    key.depth_clip_enable = regs.clipper_control.ZclipEnable();
    key.clip_space = regs.clipper_control.clip_space;
    key.provoking_vtx_last = regs.polygon_control.provoking_vtx_last;
    key.prim_type = regs.primitive_type;
    key.polygon_mode = regs.polygon_control.PolyMode();
    key.patch_control_points =
        regs.stage_enable.hs_en ? regs.ls_hs_config.hs_input_control_points : 0;
    key.logic_op = regs.color_control.rop3;
    key.depth_samples = db_enabled ? regs.depth_buffer.NumSamples() : 1;
    key.num_samples = key.depth_samples;
    key.cb_shader_mask = regs.color_shader_mask;

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;

    // Only potentially animated G-buffer draws may use the extra motion attachment. The
    // ordinary stage variant is needed first to inspect the vertex shader's resources.
    bool motion_possible = false;
    {
        u32 bound = 0;
        for (s32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS && !skip_cb_binding; ++cb) {
            bound += (regs.color_buffers[cb] && regs.color_target_mask.GetMask(cb)) ? 1 : 0;
        }
        // Uncharted: its G-buffer passes draw one target with the scene depth and write the
        // stencil (characters set bit 1); the rasterizer picks the character draws.
        const bool uncharted_gbuffer = Shader::MotionVectors::stencil_gate && bound >= 1 &&
                                       regs.depth_control.stencil_enable &&
                                       regs.depth_control.depth_write_enable;
        motion_possible = Shader::MotionVectors::positions_address != 0 &&
                          (bound >= 5 || uncharted_gbuffer) &&
                          !regs.color_buffers[Shader::MotionVectors::Output] &&
                          regs.depth_buffer.DepthValid() &&
                          regs.depth_buffer.NumSamples() == 1 && !regs.IsClipDisabled() &&
                          regs.stage_enable.raw == AmdGpu::ShaderStageEnable::VgtStages::Vs;
    }
    sel.motion = false;

    // First pass to fill render target information needed by shader recompiler
    for (s32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        if (!col_buf || !regs.color_target_mask.GetMask(cb)) {
            // No attachment bound or writing to it is disabled.
            continue;
        }

        // Fill color target information
        auto& color_buffer = key.color_buffers[cb];
        color_buffer.data_format = col_buf.GetDataFmt();
        color_buffer.num_format = col_buf.GetNumberFmt();
        color_buffer.num_conversion = col_buf.GetNumberConversion();
        color_buffer.export_format = regs.color_export_format.GetFormat(cb);
        color_buffer.swizzle = col_buf.Swizzle();

    }

    // Compile and bind shader stages
    if (!RefreshGraphicsStages(sel)) {
        return false;
    }
    if (motion_possible) {
        const auto* vs = sel.infos[static_cast<u32>(Shader::SwStage::Vertex)];
        if (vs && Shader::MotionVectors::stencil_gate) {
            sel.motion = true; // Uncharted: per draw (the stencil value it writes)
        } else if (vs) {
            // Shaders with a bone palette (motion_history.h). Small skeletons (weapons,
            // props) also include static world pieces: the rasterizer gives those
            // history only while their constants change.
            for (const auto& resource : vs->buffers) {
                if (resource.IsSpecial()) continue;
                const auto buffer = resource.GetSharp(*vs);
                if (buffer.Valid() && buffer.GetStride() == 16 &&
                    Motion::ClassifyBuffer(buffer.GetSize()) != Motion::BufferRole::Other) {
                    sel.motion = true;
                    break;
                }
            }
        }
        // GOW3_MOTION_SELECT_LOG=1: each G-buffer vertex shader once, with its buffer sizes
        // and the selection, to find animated models that the size rule leaves out.
        static const bool select_log = [] {
            const char* value = std::getenv("GOW3_MOTION_SELECT_LOG");
            return value && value[0] == '1';
        }();
        if (select_log && vs) {
            std::string sizes;
            for (const auto& resource : vs->buffers) {
                if (resource.IsSpecial()) continue;
                const auto buffer = resource.GetSharp(*vs);
                sizes += fmt::format(" {}/{}", buffer.GetSize(), buffer.GetStride());
            }
            static std::mutex log_mutex;
            static std::unordered_set<u64> logged;
            std::scoped_lock lock{log_mutex};
            if (logged.insert(vs->pgm_hash ^ std::hash<std::string>{}(sizes)).second) {
                std::printf("Motion select: vs %016llx %s, buffers (size/stride):%s\n",
                            static_cast<unsigned long long>(vs->pgm_hash),
                            sel.motion ? "ON " : "off", sizes.c_str());
            }
        }
        // Keep the old broad path available for visual A/B tests.
        static const bool all_motion = [] {
            const char* value = std::getenv("GOW3_OBJECT_MOTION_ALL");
            return value && value[0] == '1';
        }();
        if (all_motion) {
            sel.motion = true;
        }
        if (sel.motion && !RefreshGraphicsStages(sel)) {
            return false;
        }
    }

    // Second pass to mask out render targets not written by shader and fill remaining info
    u8 color_samples = 0;
    bool all_color_samples_same = true;
    for (s32 cb = 0; cb < key.num_color_attachments && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (!col_buf || !target_mask) {
            continue;
        }
        if ((key.mrt_mask & (1u << cb)) == 0) {
            std::memset(&key.color_buffers[cb], 0, sizeof(Shader::PsColorBuffer));
            continue;
        }

        // Fill color blending information
        if (regs.blend_control[cb].enable && !col_buf.info.blend_bypass) {
            key.blend_controls[cb] = regs.blend_control[cb];
        }

        // Apply swizzle to target mask
        key.write_masks[cb] =
            vk::ColorComponentFlags{key.color_buffers[cb].swizzle.ApplyMask(target_mask)};

        // Fill color samples
        const u8 prev_color_samples = std::exchange(color_samples, col_buf.NumSamples());
        all_color_samples_same &= color_samples == prev_color_samples || prev_color_samples == 0;
        key.color_samples[cb] = color_samples;
        key.num_samples = std::max(key.num_samples, color_samples);
    }

    if (sel.motion) {
        constexpr u32 mv = Shader::MotionVectors::Output;
        key.motion_vectors = 1;
        key.mrt_mask |= 1u << mv;
        key.num_color_attachments = mv + 1;
        auto& color_buffer = key.color_buffers[mv];
        color_buffer.data_format = AmdGpu::DataFormat::Format32_32_32_32;
        color_buffer.num_format = AmdGpu::NumberFormat::Float;
        color_buffer.swizzle = AmdGpu::IdentityMapping;
        key.write_masks[mv] = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                              vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        key.color_samples[mv] = 1;
    }

    // Force all color samples to match depth samples to avoid unsupported MSAA configuration
    if (color_samples != 0) {
        const bool depth_mismatch = db_enabled && color_samples != key.depth_samples;
        if (!all_color_samples_same && !instance.IsMixedAnySamplesSupported() ||
            all_color_samples_same && depth_mismatch && !instance.IsMixedDepthSamplesSupported()) {
            key.color_samples.fill(key.depth_samples);
            key.num_samples = key.depth_samples;
        }
    }

    return true;
}

bool PipelineCache::RefreshGraphicsStages(PipelineSelection& sel) {
    const auto& regs = (*sel.regs);
    auto& key = sel.graphics_key;
    // gow3: a stage's fetch shader data is assigned to sel.fetch_shader in place (GetProgram),
    // not reset here and copied anew for every draw; it is reset only when no stage has one,
    // before it is read below and on every early return.
    struct FetchReset {
        PipelineSelection& sel;
        bool assigned = false;
        void Apply() {
            if (!assigned) {
                sel.fetch_shader.reset();
            }
        }
        ~FetchReset() {
            Apply();
        }
    } fetch_reset{sel};
    sel.compilation_pending = false;

    Shader::Backend::Bindings binding{};
    const auto bind_stage = [&](HwStage stage_in, SwStage stage_out) -> bool {
        const auto stage_in_idx = static_cast<u32>(stage_in);
        const auto stage_out_idx = static_cast<u32>(stage_out);
        if (!regs.stage_enable.IsStageEnabled(stage_in_idx)) {
            key.stage_hashes[stage_out_idx] = 0;
            sel.infos[stage_out_idx] = nullptr;
            return false;
        }

        const auto* pgm = regs.ProgramForStage(stage_in_idx);
        if (!pgm || !pgm->Address<u32*>()) {
            key.stage_hashes[stage_out_idx] = 0;
            sel.infos[stage_out_idx] = nullptr;
            return false;
        }

        const auto params = AmdGpu::GetParams(*pgm);
        bool has_fetch = false;
        std::tie(sel.infos[stage_out_idx], sel.modules[stage_out_idx], has_fetch,
                 key.stage_hashes[stage_out_idx]) =
            GetProgram(sel, stage_in, stage_out, params, binding, sel.fetch_shader);
        if (!sel.infos[stage_out_idx]) {
            sel.compilation_pending = true;
            return false;
        }
        fetch_reset.assigned |= has_fetch;
        return true;
    };

    sel.infos.fill(nullptr);
    sel.modules.fill(nullptr);

    bind_stage(HwStage::Fragment, SwStage::Fragment);
    if (sel.compilation_pending) {
        return false;
    }

    const auto* fs_info = sel.infos[static_cast<u32>(SwStage::Fragment)];
    key.mrt_mask = fs_info ? fs_info->mrt_mask : 0u;
    key.num_color_attachments = std::bit_width(key.mrt_mask);

    switch (regs.stage_enable.raw) {
    case AmdGpu::ShaderStageEnable::VgtStages::EsGs:
        if (!instance.IsGeometryStageSupported()) {
            LOG_WARNING(Render_Vulkan, "Geometry shader stage unsupported, skipping");
            return false;
        }
        if (regs.vgt_gs_mode.onchip || regs.vgt_strmout_config.raw) {
            LOG_WARNING(Render_Vulkan, "Geometry shader features unsupported, skipping");
            return false;
        }
        if (!bind_stage(HwStage::Export, SwStage::Vertex)) {
            return false;
        }
        if (!bind_stage(HwStage::Geometry, SwStage::Geometry)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHs:
        if (!instance.IsTessellationSupported()) {
            return false;
        }
        if (!bind_stage(HwStage::Hull, SwStage::TessellationControl)) {
            return false;
        }
        if (!bind_stage(HwStage::Vertex, SwStage::TessellationEval)) {
            return false;
        }
        if (!bind_stage(HwStage::Local, SwStage::Vertex)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHsEsGs:
        if (!instance.IsTessellationSupported()) {
            return false;
        }
        if (!instance.IsGeometryStageSupported()) {
            LOG_WARNING(Render_Vulkan, "Geometry shader stage unsupported, skipping");
            return false;
        }
        if (regs.vgt_gs_mode.onchip || regs.vgt_strmout_config.raw) {
            LOG_WARNING(Render_Vulkan, "Geometry shader features unsupported, skipping");
            return false;
        }
        if (!bind_stage(HwStage::Hull, SwStage::TessellationControl)) {
            return false;
        }
        if (!bind_stage(HwStage::Export, SwStage::TessellationEval)) {
            return false;
        }
        if (!bind_stage(HwStage::Local, SwStage::Vertex)) {
            return false;
        }
        if (!bind_stage(HwStage::Geometry, SwStage::Geometry)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::Vs:
        bind_stage(HwStage::Vertex, SwStage::Vertex);
        if (sel.compilation_pending) {
            return false;
        }
        break;
    default:
        LOG_WARNING(Render_Vulkan, "unimplemented shader stage {}", (u32)regs.stage_enable.raw);
        return false;
    }

    fetch_reset.Apply();
    const auto* vs_info = sel.infos[static_cast<u32>(SwStage::Vertex)];
    if (vs_info && sel.fetch_shader && !instance.IsVertexInputDynamicState()) {
        // Without vertex input dynamic state, the pipeline needs to specialize on format.
        // Stride will still be handled outside the pipeline using dynamic state.
        u32 vertex_binding = 0;
        for (const auto& attrib : sel.fetch_shader->attributes) {
            const auto& buffer = attrib.GetSharp(*vs_info);
            ASSERT_MSG(vertex_binding < MaxVertexBufferCount,
                       "Vertex attribute binding count exceeded limit: {} >= {}", vertex_binding,
                       MaxVertexBufferCount);
            key.vertex_buffer_formats[vertex_binding++] =
                Vulkan::LiverpoolToVK::SurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt());
        }
    }

    return true;
}

bool PipelineCache::RefreshComputeKey() {
    Shader::Backend::Bindings binding{};
    const auto& cs_pgm = liverpool->GetCsRegs();
    const auto cs_params = AmdGpu::GetParams(cs_pgm);
    bool has_fetch = false;
    std::tie(sel.infos[0], sel.modules[0], has_fetch, compute_key.value) =
        GetProgram(sel, HwStage::Compute, SwStage::Compute, cs_params, binding, sel.fetch_shader);
    if (!has_fetch) {
        sel.fetch_shader.reset();
    }
    return sel.infos[0] != nullptr;
}

vk::ShaderModule PipelineCache::CompileModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                              const std::span<const u32>& code, size_t perm_idx,
                                              Shader::Backend::Bindings& binding) {
    LOG_INFO(Render_Vulkan, "Compiling {} shader {:#x} {}", info.hw_stage, info.pgm_hash,
             perm_idx != 0 ? "(permutation)" : "");
    DumpShader(code, info.pgm_hash, info.hw_stage, perm_idx, "bin");
    CompileTimer timer{info.hw_stage == Shader::HwStage::Compute
                           ? (perm_idx ? CompileTimer::ComputeVariant : CompileTimer::ComputeShader)
                           : (perm_idx ? CompileTimer::GraphicsVariant : CompileTimer::GraphicsShader)};

    const auto ir_program = Shader::TranslateProgram(code, pools, info, runtime_info, profile);
    auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, ir_program, binding);
    DumpShader(spv, info.pgm_hash, info.hw_stage, perm_idx, "spv");

    vk::ShaderModule module;

    auto patch = GetShaderPatch(info.pgm_hash, info.hw_stage, perm_idx, "spv");
    const bool is_patched = patch && EmulatorSettings.IsPatchShaders();
    if (is_patched) {
        LOG_INFO(Loader, "Loaded patch for {} shader {:#x}", info.hw_stage, info.pgm_hash);
        module = CompileSPV(*patch, instance.GetDevice());
    } else {
        module = CompileSPV(spv, instance.GetDevice());
    }

    RegisterShaderBinary(std::move(spv), info.pgm_hash, perm_idx,
                         info.hw_stage == Shader::HwStage::Compute);

    const auto name = GetShaderName(info.hw_stage, info.pgm_hash, perm_idx);
    Vulkan::SetObjectName(instance.GetDevice(), module, name);
    if (EmulatorSettings.IsShaderCollect()) {
        DebugState.CollectShader(name, info.sw_stage, module, spv, code,
                                 patch ? *patch : std::span<const u32>{}, is_patched);
    }
    return module;
}

// gow3: see Result: copied in place, so the attribute vector keeps its allocation.
static bool AssignFetch(std::optional<Shader::Gcn::FetchShaderData>& to,
                        const std::optional<Shader::Gcn::FetchShaderData>& from) {
    if (!from) {
        return false;
    }
    if (to) {
        *to = *from;
    } else {
        to = *from;
    }
    return true;
}

PipelineCache::Result PipelineCache::GetProgram(PipelineSelection& sel, HwStage hw_stage,
                                                SwStage sw_stage,
                                                const Shader::ShaderParams& params,
                                                Shader::Backend::Bindings& binding,
                                                std::optional<Shader::Gcn::FetchShaderData>& fetch) {
    auto runtime_info = BuildRuntimeInfo(sel, hw_stage, sw_stage);
    if (sel.worker) {
        // gow3: draw-preparation worker: look up only, with the worker's own Info copy.
        auto& worker = *sel.worker;
        std::shared_lock lk{programs_mutex};
        const auto found_program = program_cache.find(params.hash);
        if (found_program == program_cache.end() || !found_program->second->info_template) {
            worker.failed = true;
            return {};
        }
        const Program* program = found_program->second.get();
        auto [it_info, new_info] = worker.infos.try_emplace(program, *program->info_template);
        auto& info = it_info->second;
        info.pgm_base = params.Base();
        info.user_data = params.user_data;
        // The walk of resource tables and the fetch shader parse read guest memory through
        // pointers in the registers; ahead of the GPU thread that memory may already be
        // reused. A fault returns here (runtime_fault_recover) and the draw is left to the GPU
        // thread. A jump out of the specialization leaks its partial allocations (rare).
        sigjmp_buf recover;
        if (sigsetjmp(recover, 0)) {
            worker.failed = true;
            return {};
        }
        runtime_fault_recover = &recover;
        // gow3 5 Nov 2026: a GPU library assertion on this guess (stale or garbage tables, mostly
        // after "garbage T#/V#" warnings) throws here and the draw is left to the GPU thread, as
        // for the compute precompile; it stopped the game with exit 23 before.
        std::optional<Shader::StageSpecialization> guessed;
        gow3_assert_throws = true;
        try {
            info.RefreshFlatBuf();
            guessed.emplace(info, runtime_info, profile, binding);
        } catch (...) {
        }
        gow3_assert_throws = false;
        runtime_fault_recover = nullptr;
        if (!guessed) {
            worker.failed = true;
            return {};
        }
        auto& spec = *guessed;
        const auto it = std::ranges::find(program->modules, spec, &Program::Module::spec);
        if (it == program->modules.end()) {
            worker.failed = true;
            return {};
        }
        info.AddBindings(binding);
        const size_t perm_idx = std::distance(program->modules.begin(), it);
        worker.stages.push_back(
            {program, params.hash, hw_stage, info.pgm_base, &info.flattened_ud_buf});
        // (copied while the programs lock is held: the GPU thread may add permutations)
        return std::make_tuple(&info, it->module, AssignFetch(fetch, it->spec.fetch_shader_data),
                               HashCombine(params.hash, perm_idx));
    }

    auto it_pgm = program_cache.find(params.hash); // this thread is the only writer
    const bool compile_async = AsyncCompilation() && hw_stage != HwStage::Compute &&
                               !sel.regs->stage_enable.hs_en &&
                               !EmulatorSettings.IsShaderCollect();
    if (!compile_async && it_pgm != program_cache.end() && it_pgm->second->pending) {
        {
            PhaseTimer waiting{g_gow3_compile_wait_ns};
            const auto wait_start = std::chrono::steady_clock::now();
            it_pgm->second->pending->job.Wait();
            // gow3 diagnostics: the GPU thread waited for a shader a worker compiles.
            if (const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                           wait_start)
                                      .count();
                ms > 8.0) {
                std::printf("Slow shader wait: %.1f ms for %s shader %#llx (compiled by a worker)\n", ms,
                            hw_stage == HwStage::Compute ? "a compute" : "a graphics",
                            static_cast<unsigned long long>(params.hash));
            }
        }
        PublishShaders();
    }
    if (!compile_async && it_pgm != program_cache.end() && it_pgm->second->modules.empty() &&
        !it_pgm->second->compilation_failed) {
        std::unique_lock lock{programs_mutex};
        program_cache.erase(it_pgm);
        it_pgm = program_cache.end();
    }
    // gow3: a compute program the precompile scanner saw ahead: its worker's result.
    if (it_pgm == program_cache.end() && hw_stage == HwStage::Compute && TakePrecompiled(params.hash)) {
        it_pgm = program_cache.find(params.hash);
    }
    if (it_pgm == program_cache.end()) {
        auto new_program = std::make_unique<Program>(hw_stage, sw_stage, params);
        if (compile_async) {
            QueueShader(*new_program, hw_stage, sw_stage, params, runtime_info, binding, 0, true);
            std::unique_lock lock{programs_mutex};
            program_cache.emplace(params.hash, std::move(new_program));
            return {};
        }
        auto start = binding;
        const auto compile_start = std::chrono::steady_clock::now();
        const auto module =
            CompileModule(new_program->info, runtime_info, params.code, 0, binding);
        // gow3 diagnostics: a shader compiled on the GPU thread (compute shaders always are).
        if (const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                       compile_start)
                                  .count();
            ms > 8.0) {
            std::printf("Slow shader compile: %.1f ms for %s shader %#llx on the GPU thread\n", ms,
                        hw_stage == HwStage::Compute ? "a compute" : "a graphics",
                        static_cast<unsigned long long>(params.hash));
        }
        auto spec = Shader::StageSpecialization(new_program->info, runtime_info, profile, start);
        const auto perm_hash = HashCombine(params.hash, 0);

        RegisterShaderMeta(new_program->info, spec.fetch_shader_data, spec, perm_hash, 0);
        new_program->AddPermut(module, std::move(spec));
        new_program->info_template = std::make_unique<Shader::Info>(new_program->info);
        Program* program = new_program.get();
        {
            std::unique_lock lk{programs_mutex};
            program_cache.emplace(params.hash, std::move(new_program));
        }
        return std::make_tuple(&program->info, module,
                               AssignFetch(fetch, program->modules[0].spec.fetch_shader_data),
                               perm_hash);
    }

    auto& program = it_pgm.value();
    if (program->compilation_failed || (program->modules.empty() && program->pending)) {
        return {};
    }
    if (program->modules.empty() && compile_async) {
        QueueShader(*program, hw_stage, sw_stage, params, runtime_info, binding, 0, true);
        return {};
    }
    if (!program->info_template) {
        // Programs loaded by the pipeline cache warm-up get their template on first use.
        std::unique_lock lk{programs_mutex};
        program->info_template = std::make_unique<Shader::Info>(program->info);
    }
    auto& info = program->info;
    info.pgm_base = params.Base(); // Needs to be actualized for inline cbuffer address fixup
    info.user_data = params.user_data;
    info.RefreshFlatBuf();
    auto spec = Shader::StageSpecialization(info, runtime_info, profile, binding);

    size_t perm_idx = program->modules.size();
    u64 perm_hash = HashCombine(params.hash, perm_idx);

    vk::ShaderModule module{};

    // gow3: consecutive draws of a program almost always use the same permutation.
    auto it = program->last_used < program->modules.size() &&
                      program->modules[program->last_used].spec == spec
                  ? program->modules.begin() + program->last_used
                  : std::ranges::find(program->modules, spec, &Program::Module::spec);
    if (it != program->modules.end()) {
        program->last_used = std::distance(program->modules.begin(), it);
    }
    if (it == program->modules.end()) {
        if (program->pending || std::ranges::find(program->failed_permutations, spec) !=
                                    program->failed_permutations.end()) {
            return {};
        }
        if (compile_async) {
            QueueShader(*program, hw_stage, sw_stage, params, runtime_info, binding, perm_idx, false,
                        std::move(spec));
            return {};
        }
        auto new_info = Shader::Info(hw_stage, sw_stage, params);
        module = CompileModule(new_info, runtime_info, params.code, perm_idx, binding);

        RegisterShaderMeta(info, spec.fetch_shader_data, spec, perm_hash, perm_idx);
        std::unique_lock lk{programs_mutex};
        program->AddPermut(module, std::move(spec));
    } else {
        info.AddBindings(binding);
        module = it->module;
        perm_idx = std::distance(program->modules.begin(), it);
        perm_hash = HashCombine(params.hash, perm_idx);
        // gow3 6 Nov 2026: a compute permutation built for another LDS size (its shared array has
        // that size): logged once per shader and size pair (GOW3_LDS_SIZE_KEY=1 keys them apart).
        if (hw_stage == Shader::HwStage::Compute &&
            it->spec.runtime_info.hw.cs.shared_memory_size !=
                runtime_info.hw.cs.shared_memory_size) {
            static std::mutex lds_mutex;
            static std::set<std::tuple<u64, u32, u32>> lds_seen;
            const auto entry = std::make_tuple(params.hash, it->spec.runtime_info.hw.cs.shared_memory_size,
                                               runtime_info.hw.cs.shared_memory_size);
            std::scoped_lock lds_lock{lds_mutex};
            if (lds_seen.size() < 64 && lds_seen.insert(entry).second) {
                std::printf("GPU: cs %#llx permutation %zu built for %u bytes of LDS, dispatched "
                            "with %u (GOW3_LDS_SIZE_KEY)\n",
                            static_cast<unsigned long long>(params.hash), perm_idx,
                            std::get<1>(entry), std::get<2>(entry));
                std::fflush(stdout);
            }
        }
    }
    return std::make_tuple(&program->info, module,
                           AssignFetch(fetch, program->modules[perm_idx].spec.fetch_shader_data),
                           perm_hash);
}

std::optional<vk::ShaderModule> PipelineCache::ReplaceShader(vk::ShaderModule module,
                                                             std::span<const u32> spv_code) {
    PublishShaders(true);
    PublishGraphics(true);
    std::optional<vk::ShaderModule> new_module{};
    for (const auto& [_, program] : program_cache) {
        for (auto& m : program->modules) {
            if (m.module == module) {
                const auto& d = instance.GetDevice();
                d.destroyShaderModule(m.module);
                m.module = CompileSPV(spv_code, d);
                new_module = m.module;
            }
        }
    }
    if (module_related_pipelines.contains(module)) {
        auto& pipeline_keys = module_related_pipelines[module];
        for (auto& key : pipeline_keys) {
            if (std::holds_alternative<GraphicsPipelineKey>(key)) {
                auto& graphics_key = std::get<GraphicsPipelineKey>(key);
                graphics_pipelines.erase(graphics_key);
            } else if (std::holds_alternative<ComputePipelineKey>(key)) {
                auto& compute_key = std::get<ComputePipelineKey>(key);
                compute_pipelines.erase(compute_key);
            }
        }
    }
    return new_module;
}

std::string PipelineCache::GetShaderName(Shader::HwStage stage, u64 hash,
                                         std::optional<size_t> perm) {
    if (perm) {
        return fmt::format("{}_{:#018x}_{}", stage, hash, *perm);
    }
    return fmt::format("{}_{:#018x}", stage, hash);
}

void PipelineCache::DumpShader(std::span<const u32> code, u64 hash, Shader::HwStage stage,
                               size_t perm_idx, std::string_view ext) {
    if (!EmulatorSettings.IsDumpShaders()) {
        return;
    }

    using namespace Common::FS;
    const auto dump_dir = GetUserPath(PathType::ShaderDir) / "dumps";
    if (!std::filesystem::exists(dump_dir)) {
        std::filesystem::create_directories(dump_dir);
    }
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    const auto file = IOFile{dump_dir / filename, FileAccessMode::Create};
    file.WriteSpan(code);
}

std::optional<std::vector<u32>> PipelineCache::GetShaderPatch(u64 hash, Shader::HwStage stage,
                                                              size_t perm_idx,
                                                              std::string_view ext) {

    using namespace Common::FS;
    const auto patch_dir = GetUserPath(PathType::ShaderDir) / "patch";
    if (!std::filesystem::exists(patch_dir)) {
        std::filesystem::create_directories(patch_dir);
    }
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    const auto filepath = patch_dir / filename;
    if (!std::filesystem::exists(filepath)) {
        return {};
    }
    const auto file = IOFile{patch_dir / filename, FileAccessMode::Read};
    std::vector<u32> code(file.GetSize() / sizeof(u32));
    file.Read(code);
    return code;
}
} // namespace Vulkan

namespace Vulkan {
std::string PipelineCache::DebugSizes() const {
    return std::format("{} graphics and {} compute pipelines, {} programs, {} pending compiles",
                       graphics_pipelines.size(), compute_pipelines.size(), program_cache.size(),
                       pending_graphics.size() + failed_graphics.size());
}
} // namespace Vulkan
