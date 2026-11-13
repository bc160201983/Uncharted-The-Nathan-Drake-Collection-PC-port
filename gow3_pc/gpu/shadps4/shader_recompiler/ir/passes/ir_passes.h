// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/passes/resource_pass.h"
#include "shader_recompiler/ir/program.h"

namespace Shader {
struct Profile;
void InjectClipDistanceAttributes(IR::Program& program, RuntimeInfo& runtime_info);
} // namespace Shader

namespace Shader::Optimization {

void SsaRewritePass(IR::Program& program);
void SsaRepairPass(IR::Program& program);
void PhiSimplificationPass(IR::Program& program);
void InverseBallotEliminationPass(IR::Program& program);
void LowerWave64BallotPass(IR::Program& program, const RuntimeInfo& runtime_info,
                           const Profile& profile);
void LowerHardwareIntrinsics(IR::Program& program);
void LowerPhisToRegsPass(IR::Program& program);
void DeadCodeEliminationPass(IR::Program& program);
/// gow3: exact integers from interpolated float varyings on GPUs without manual interpolation.
void InterpolatedIntegerPass(IR::Program& program);
void ConstantPropagationPass(IR::BlockList& program);
void FlattenExtendedUserdataPass(IR::Program& program);
void ReadLaneEliminationPass(IR::Program& program);
ResourceDiscoveryList ResourceDiscoverPass(IR::Program& program, const Profile& profile);
void ResourcePatchingPass(Shader::Info& info, const ResourceDiscoveryList& sharp_usages,
                          const Profile& profile);
void CollectShaderInfoPass(IR::Program& program, const Profile& profile);
void LowerBufferFormatToRaw(IR::Program& program);
void LowerUserClipPlanes(IR::Program& program, const RuntimeInfo& runtime_info);
void LowerFp64ToFp32(IR::Program& program);
void RingAccessElimination(const IR::Program& program, const RuntimeInfo& runtime_info);
void TessellationPreprocess(IR::Program& program, RuntimeInfo& runtime_info);
void HullShaderTransform(IR::Program& program, const RuntimeInfo& runtime_info);
void DomainShaderTransform(const IR::Program& program, const RuntimeInfo& runtime_info);
void SharedMemoryBarrierPass(IR::Program& program, const RuntimeInfo& runtime_info,
                             const Profile& profile);
/// gow3: whether the shader gets LDS barriers for groups of several waves on 32-lane subgroups.
bool MultiwaveLdsBarriers(u64 pgm_hash);
/// gow3 1 Nov 2026: clamps the per-vertex loop bound of the effect/crowd skinning shaders
/// (effect_loop_clamp_pass.cpp); EffectLoopClamp tells whether a shader gets it (cache names).
void EffectLoopClampPass(IR::Program& program);
bool EffectLoopClamp(u64 pgm_hash);
/// gow3 6 Nov 2026: "safe cloth" for the cloth solvers 1c957b01 / 809864d3 (loop counts capped in
/// EffectLoopClampPass; SafeClothExitPass gives their LDS-decided loop exit one decision for the
/// whole group on GPUs with subgroups under 64 lanes). SafeClothCacheTag: cache name suffix.
bool SafeCloth(u64 pgm_hash);
/// gow3 6 Nov 2026: LDS accesses bounds-checked like the PS4 (shared_memory_bounds_pass.cpp).
void SharedMemoryBoundsPass(IR::Program& program, const RuntimeInfo& runtime_info,
                            const Profile& profile);
bool SharedMemoryBounds(u64 pgm_hash);
std::string SharedMemoryBoundsTag(u64 pgm_hash, bool compute);
std::string SafeClothCacheTag(u64 pgm_hash);
/// gow3 9 Nov 2026: group barriers inside ifs that only some invocations enter (Intel hangs).
void UniformBarrierPass(IR::Program& program, const RuntimeInfo& runtime_info, const Profile& profile);
void SetUniformBarrierGpu(bool intel);
std::string UniformBarrierTag(u64 pgm_hash, bool compute);
void SafeClothExitPass(IR::Program& program, const RuntimeInfo& runtime_info,
                       const Profile& profile);
void SharedMemorySimplifyPass(IR::Program& program, const Profile& profile);
void SharedMemoryToStoragePass(IR::Program& program, const RuntimeInfo& runtime_info,
                               const Profile& profile);

} // namespace Shader::Optimization
