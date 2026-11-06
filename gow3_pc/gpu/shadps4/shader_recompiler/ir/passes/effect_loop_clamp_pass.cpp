// SPDX-FileCopyrightText: Copyright 2026 gow3_pc
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <unordered_set>
#include <vector>

#include "common/alignment.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/resource.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Optimization {

// gow3 1 Nov 2026 (Uncharted 3 chapter 2 device lost, GitHub #292 #295 #238;
// analysis/handoff-book/11-U3-CH2-CRASH.md): Naughty Dog's effect and crowd skinning compute
// shaders (Shader::IsEffectShader: cs 8f067540, 7d7e7206, 0c936dd6, all three games) run, per
// vertex, a loop whose trip count is read from buffer 0 (u32 count, u32 offset per vertex):
//     entry = buffer0[vertex]; for (i = 0; i < entry.count; ++i) { ...buffer4[entry.offset + i]... }
// A count that is not a small influence count (stale or garbage data, as the garbage V#s of this
// family already showed on 16 Oct) runs the loop billions of times: the GPU hangs, Windows resets
// it after 2 s and the game stops with "Device lost". The loop's bound is clamped here to
// MaxEffectLoopCount, far above any real count, so a bad count costs microseconds instead.
// GOW3_EFFECT_LOOP_CLAMP=0: off (the shaders as before).
// GOW3_TEST_EFFECT_LOOP_COUNT=<n> (tests only): every count is raised to at least n first (with
// the clamp off, a large n reproduces the hang; with it on, the dispatch stays short).
constexpr u32 MaxEffectLoopCount = 4096;

// gow3 5 Nov 2026 (Uncharted 3 chapters 2-3 device lost at the museum exit, GitHub #292 #295 #354
// #328): pixel shader 689d1af9, a soft-shadow filter shared by the three games, loops over a line
// of depth taps whose count it reads from a constant buffer (one u32). The same stale or garbage
// data as above turns that count into billions and the GPU resets. A tap count is small, so its
// loop compares are clamped lower, to MaxShadowTaps. Same switch (GOW3_EFFECT_LOOP_CLAMP=0: off).
constexpr u64 ShadowTapShader = 0x689d1af9;
constexpr u32 MaxShadowTaps = 256;

// gow3 6 Nov 2026 (Uncharted 3 chapter 2 device lost as young Sully streams in, GitHub #18 #138
// #238 #292 #295 #342 #365; analysis/sully-1106/FINDINGS.md): the cloth solvers 1c957b01 and
// 809864d3 (Sully's shirt; 1024-thread groups, 2 KiB LDS) are the last new GPU work before every
// loss, and only GPUs with 32-lane (NVIDIA) or narrower (Intel) subgroups lose the device there.
// "Safe cloth" (GOW3_SAFE_CLOTH=0: off; own cache names):
// 1. Every loop of theirs runs to a count read from memory (constant buffers #1/#2, per-vertex
//    counts in buffers): each loop compare of such a count with its counter uses
//    min(count, cap), the cap far above any real count (ClothBoundCap). A garbage count costs a
//    frame of wrong cloth instead of a reset GPU.
// 2. The convergence loop (47 / 35 barriers inside) leaves when the error it reads from LDS is
//    small: with 32-lane subgroups, warps that read that word at different times disagree, part of
//    the group leaves while the rest waits at the barriers inside. All invocations now take one
//    decision: each writes its own to one LDS word after the game's code, a barrier, all read the
//    word back (SafeClothExitPass). Only on GPUs with subgroups under 64 lanes.
// Tests: GOW3_TEST_CLOTH_BOUND=<n> raises every count to at least n first (a garbage count);
// GOW3_TEST_CLOTH_DIVERGE=1 makes invocations 512 and up leave the convergence loop at once.
constexpr u64 ClothSolverA = 0x1c957b01;
constexpr u64 ClothSolverB = 0x809864d3;

static bool IsClothSolver(u64 pgm_hash) {
    return pgm_hash == ClothSolverA || pgm_hash == ClothSolverB;
}

static u32 EnvU32(const char* name) {
    const char* env = std::getenv(name);
    return env ? static_cast<u32>(std::strtoul(env, nullptr, 0)) : 0u;
}

static u32 TestClothBound() {
    static const u32 value = EnvU32("GOW3_TEST_CLOTH_BOUND");
    return value;
}

static bool TestClothDiverge() {
    static const bool value = EnvU32("GOW3_TEST_CLOTH_DIVERGE") != 0;
    return value;
}

static bool SafeClothExit() {
    static const bool value = EnvU32("GOW3_SAFE_CLOTH_EXIT") != 0;
    return value;
}

bool SafeCloth(u64 pgm_hash) {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_SAFE_CLOTH");
        return !(env && env[0] == '0');
    }();
    return enabled && IsClothSolver(pgm_hash);
}

std::string SafeClothCacheTag(u64 pgm_hash) {
    if (!IsClothSolver(pgm_hash)) {
        return {};
    }
    // _sc3: the measured caps, one bound per compare signedness.
    std::string tag = SafeCloth(pgm_hash) ? "_sc3" : "";
    if (const u32 cap = EnvU32("GOW3_SAFE_CLOTH_CAP"); cap && SafeCloth(pgm_hash)) {
        tag += "_cap" + std::to_string(cap);
    }
    if (const u32 bound = TestClothBound()) {
        tag += "_tb" + std::to_string(bound);
    }
    if (TestClothDiverge()) {
        tag += "_td";
    }
    if (SafeClothExit()) {
        tag += "_ue";
    }
    return tag;
}

/// The cap of a loop count: constant-buffer words by (binding, dword), per-vertex counts by kind.
/// Real counts, 6 Nov (GOW3_CLOTH_BOUNDS=1, young Sully in the chapter 2 square): cb1[160]
/// substeps 2-9 (round(dt * 90)), cb1[163] 3, cb1[164] convergence iterations 128, cb1[165] 1-6,
/// cb2[3] 252-260, cb2[22] 7-8, per-vertex counts 7-9. The caps are 4-16 times those, and all of
/// them at once keep a dispatch far below the 2 s at which Windows resets the GPU.
static u32 ClothBoundCap(const IR::Inst& count) {
    static const u32 forced = EnvU32("GOW3_SAFE_CLOTH_CAP");
    if (forced) {
        return forced;
    }
    if (count.GetOpcode() == IR::Opcode::ReadConstBuffer && count.Arg(0).IsImmediate() &&
        count.Arg(1).IsImmediate()) {
        const u32 binding = count.Arg(0).U32(), dword = count.Arg(1).U32();
        if (binding == 1) {
            switch (dword) {
            case 160: // substeps this frame
                return 32;
            case 163:
                return 32;
            case 164: // convergence iterations
                return 512;
            case 165:
                return 64;
            default:
                break;
            }
        } else if (binding == 2) {
            switch (dword) {
            case 3:
                return 4096;
            case 22:
                return 128;
            default:
                break;
            }
        }
        return 1024;
    }
    return 128; // per-vertex counts (buffer loads)
}

static bool IsLoopCounter(const IR::Value& value) {
    if (value.IsPhi()) {
        return true;
    }
    // `i + 1 < count` forms: an add of an immediate to a phi.
    const IR::Inst* inst = value.TryInst();
    return inst && inst->GetOpcode() == IR::Opcode::IAdd32 &&
           ((inst->Arg(0).IsPhi() && inst->Arg(1).IsImmediate()) ||
            (inst->Arg(1).IsPhi() && inst->Arg(0).IsImmediate()));
}

static bool IsSignedCompare(IR::Opcode op) {
    switch (op) {
    case IR::Opcode::SLessThan32:
    case IR::Opcode::SLessThanEqual32:
    case IR::Opcode::SGreaterThan32:
    case IR::Opcode::SGreaterThanEqual32:
        return true;
    default:
        return false;
    }
}

static bool IsLoopCompare(IR::Opcode op) {
    switch (op) {
    case IR::Opcode::ULessThan32:
    case IR::Opcode::ULessThanEqual32:
    case IR::Opcode::UGreaterThan32:
    case IR::Opcode::UGreaterThanEqual32:
    case IR::Opcode::SLessThan32:
    case IR::Opcode::SLessThanEqual32:
    case IR::Opcode::SGreaterThan32:
    case IR::Opcode::SGreaterThanEqual32:
    case IR::Opcode::IEqual32:
    case IR::Opcode::INotEqual32:
        return true;
    default:
        return false;
    }
}

/// The shadow-tap shader: every integer compare of a constant-buffer u32 with a loop counter (a
/// phi, either operand) compares with min(count, MaxShadowTaps) instead. Returns the counts patched.
static u32 ClampShadowTapLoops(IR::Program& program) {
    u32 patched = 0;
    for (IR::Block* const block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            const IR::Opcode op = inst.GetOpcode();
            if (op != IR::Opcode::ReadConstBuffer && op != IR::Opcode::LoadBufferU32) {
                continue;
            }
            std::vector<IR::Use> compares;
            for (const IR::Use& use : inst.Uses()) {
                if (!IsLoopCompare(use.user->GetOpcode())) {
                    continue;
                }
                if (use.user->Arg(use.operand == 0 ? 1 : 0).IsPhi()) {
                    compares.push_back(use);
                }
            }
            if (compares.empty()) {
                continue;
            }
            IR::IREmitter ir{*inst.GetParent(),
                             std::next(IR::Block::InstructionList::s_iterator_to(inst))};
            const IR::U32 bound = ir.IMin(IR::U32{&inst}, ir.Imm32(MaxShadowTaps), false);
            for (const IR::Use& use : compares) {
                use.user->SetArg(use.operand, bound);
            }
            ++patched;
        }
    }
    return patched;
}

static u32 TestEffectLoopCount() {
    static const u32 value = [] {
        const char* env = std::getenv("GOW3_TEST_EFFECT_LOOP_COUNT");
        return env ? static_cast<u32>(std::strtoul(env, nullptr, 0)) : 0u;
    }();
    return value;
}

bool EffectLoopClamp(u64 pgm_hash) {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_EFFECT_LOOP_CLAMP");
        return !(env && env[0] == '0');
    }();
    if (pgm_hash == ShadowTapShader) {
        return enabled;
    }
    return IsEffectShader(pgm_hash) && (enabled || TestEffectLoopCount() != 0);
}

/// The cloth solvers: every loop compare of a count read from memory (a constant-buffer word, a
/// u32 buffer load, or a component of a u32x2 buffer load) with a loop counter compares with
/// min(count, ClothBoundCap) instead (signed min for signed compares). Returns the counts patched.
static u32 ClampClothLoops(IR::Program& program, bool clamp) {
    u32 patched = 0;
    for (IR::Block* const block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            const IR::Opcode op = inst.GetOpcode();
            bool count = op == IR::Opcode::ReadConstBuffer || op == IR::Opcode::LoadBufferU32;
            if (op == IR::Opcode::CompositeExtractU32x2) {
                const IR::Inst* source = inst.Arg(0).TryInst();
                count = source && source->GetOpcode() == IR::Opcode::LoadBufferU32x2;
            }
            if (!count) {
                continue;
            }
            std::vector<IR::Use> compares;
            for (const IR::Use& use : inst.Uses()) {
                if (!IsLoopCompare(use.user->GetOpcode()) ||
                    !IsLoopCounter(use.user->Arg(use.operand == 0 ? 1 : 0))) {
                    continue;
                }
                compares.push_back(use);
            }
            if (compares.empty()) {
                continue;
            }
            // One bound per signedness: a signed min would let a count with bit 31 set through
            // to an unsigned or equality compare (review 6 Nov).
            IR::IREmitter ir{*inst.GetParent(),
                             std::next(IR::Block::InstructionList::s_iterator_to(inst))};
            std::optional<IR::U32> bounds[2];
            const auto bound_for = [&](bool is_signed) -> IR::U32 {
                auto& bound = bounds[is_signed ? 1 : 0];
                if (!bound) {
                    IR::U32 value{&inst};
                    if (const u32 test = TestClothBound()) {
                        value = ir.IMax(value, ir.Imm32(test), is_signed);
                    }
                    if (clamp) {
                        value = ir.IMin(value, ir.Imm32(ClothBoundCap(inst)), is_signed);
                    }
                    bound = value;
                }
                return *bound;
            };
            for (const IR::Use& use : compares) {
                use.user->SetArg(use.operand, bound_for(IsSignedCompare(use.user->GetOpcode())));
            }
            ++patched;
        }
    }
    return patched;
}

void EffectLoopClampPass(IR::Program& program) {
    const u64 hash = program.info.pgm_hash;
    if (IsClothSolver(hash)) {
        const bool clamp = SafeCloth(hash);
        if (!clamp && TestClothBound() == 0) {
            return;
        }
        const u32 patched = ClampClothLoops(program, clamp);
        static u32 cloth_reports = 0;
        if (cloth_reports < 8) {
            ++cloth_reports;
            std::printf("GPU: cloth solver %#llx: %u loop count(s) %s%s (GOW3_SAFE_CLOTH)\n",
                        static_cast<unsigned long long>(hash), patched,
                        clamp ? "capped" : "NOT capped",
                        TestClothBound() ? " [test count forced]" : "");
            std::fflush(stdout);
        }
        return;
    }
    if (!EffectLoopClamp(hash)) {
        return;
    }
    if (hash == ShadowTapShader) {
        const u32 patched = ClampShadowTapLoops(program);
        static u32 shadow_reports = 0;
        if (shadow_reports < 4) {
            ++shadow_reports;
            std::printf("GPU: shadow filter %#llx: tap loop clamped to %u in %u place(s) "
                        "(GOW3_EFFECT_LOOP_CLAMP)\n",
                        static_cast<unsigned long long>(hash), MaxShadowTaps, patched);
            std::fflush(stdout);
        }
        return;
    }
    static const bool clamp_on = [] {
        const char* env = std::getenv("GOW3_EFFECT_LOOP_CLAMP");
        return !(env && env[0] == '0');
    }();
    u32 patched = 0;
    for (IR::Block* const block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::LoadBufferU32x2) {
                continue;
            }
            const IR::Value handle = inst.Arg(0);
            if (!handle.IsImmediate() || handle.U32() != 0) {
                continue;
            }
            const std::vector<IR::Use> loads(inst.Uses().begin(), inst.Uses().end());
            for (const IR::Use& load_use : loads) {
                IR::Inst* const count = load_use.user;
                if (count->GetOpcode() != IR::Opcode::CompositeExtractU32x2 ||
                    !count->Arg(1).IsImmediate() || count->Arg(1).U32() != 0) {
                    continue;
                }
                // Only the loop bound: `i < count`.
                std::vector<IR::Use> bounds;
                for (const IR::Use& use : count->Uses()) {
                    if (use.user->GetOpcode() == IR::Opcode::ULessThan32 && use.operand == 1) {
                        bounds.push_back(use);
                    }
                }
                if (bounds.empty()) {
                    continue;
                }
                IR::Block* const parent = count->GetParent();
                IR::IREmitter ir{*parent,
                                 std::next(IR::Block::InstructionList::s_iterator_to(*count))};
                IR::U32 bound{count};
                if (const u32 test = TestEffectLoopCount()) {
                    bound = ir.IMax(bound, ir.Imm32(test), false);
                }
                if (clamp_on) {
                    bound = ir.IMin(bound, ir.Imm32(MaxEffectLoopCount), false);
                }
                for (const IR::Use& use : bounds) {
                    use.user->SetArg(use.operand, bound);
                }
                ++patched;
            }
        }
    }
    static u32 reports = 0;
    if (reports < 8) {
        ++reports;
        std::printf("GPU: effect shader %#llx: loop bound %s in %u place(s)%s "
                    "(GOW3_EFFECT_LOOP_CLAMP)\n",
                    static_cast<unsigned long long>(hash),
                    clamp_on ? "clamped to 4096" : "NOT clamped", patched,
                    TestEffectLoopCount() ? " [test count forced]" : "");
        std::fflush(stdout);
    }
}

/// True if `value` is computed from an LDS load (through any instruction, phis included).
static bool ReadsShared(const IR::Value& value) {
    std::vector<const IR::Inst*> stack;
    std::unordered_set<const IR::Inst*> seen;
    if (const IR::Inst* inst = value.TryInst()) {
        stack.push_back(inst);
    }
    while (!stack.empty()) {
        const IR::Inst* const inst = stack.back();
        stack.pop_back();
        if (!seen.insert(inst).second) {
            continue;
        }
        switch (inst->GetOpcode()) {
        case IR::Opcode::LoadSharedU16:
        case IR::Opcode::LoadSharedU32:
        case IR::Opcode::LoadSharedU64:
            return true;
        default:
            break;
        }
        for (size_t i = 0; i < inst->NumArgs(); ++i) {
            if (const IR::Inst* arg = inst->Arg(i).TryInst()) {
                stack.push_back(arg);
            }
        }
    }
    return false;
}

void SafeClothExitPass(IR::Program& program, const RuntimeInfo& runtime_info,
                       const Profile& profile) {
    const u64 hash = program.info.pgm_hash;
    if (!IsClothSolver(hash) || program.info.hw_stage != HwStage::Compute) {
        return;
    }
    // Off by default (GOW3_SAFE_CLOTH_EXIT=1: on): with the LDS bounds checks the exit reads
    // the PS4's value; a forced early exit still hung the GPU with it (6 Nov tests t2, t2c).
    const bool uniform = SafeCloth(hash) && profile.subgroup_size < 64 && SafeClothExit();
    const bool diverge = TestClothDiverge();
    if (!uniform && !diverge) {
        return;
    }
    using Type = IR::AbstractSyntaxNode::Type;
    auto& list = program.syntax_list;
    // Loops (by merge block) that hold a barrier.
    std::unordered_set<const IR::Block*> barrier_loops;
    {
        std::vector<std::pair<const IR::Block*, bool>> open;
        for (const IR::AbstractSyntaxNode& node : list) {
            if (node.type == Type::Loop) {
                open.emplace_back(node.data.loop.merge, false);
            } else if (node.type == Type::Repeat && !open.empty()) {
                if (open.back().second) {
                    barrier_loops.insert(open.back().first);
                    if (open.size() > 1) {
                        open[open.size() - 2].second = true;
                    }
                }
                open.pop_back();
            } else if (node.type == Type::Block && !open.empty() && !open.back().second) {
                for (const IR::Inst& inst : node.data.block->Instructions()) {
                    if (inst.GetOpcode() == IR::Opcode::Barrier) {
                        open.back().second = true;
                        break;
                    }
                }
            }
        }
    }
    // The decision word goes after the game's LDS and any scratch of earlier passes.
    const u32 used = runtime_info.hw.cs.shared_memory_size + program.info.shared_memory_scratch_size;
    const u32 flag_offset = Common::AlignUp(used, 4u);
    u32 patched = 0;
    // Per open loop: its merge block and the ifs opened inside it so far.
    std::vector<std::pair<const IR::Block*, u32>> loops;
    for (size_t i = 0; i < list.size(); ++i) {
        IR::AbstractSyntaxNode& node = list[i];
        switch (node.type) {
        case Type::Loop:
            loops.emplace_back(node.data.loop.merge, 0u);
            continue;
        case Type::Repeat:
            if (!loops.empty()) {
                loops.pop_back();
            }
            continue;
        case Type::If:
            if (!loops.empty()) {
                ++loops.back().second;
            }
            continue;
        case Type::EndIf:
            if (!loops.empty() && loops.back().second > 0) {
                --loops.back().second;
            }
            continue;
        case Type::Break:
            break;
        default:
            continue;
        }
        // A break of the innermost loop, outside any if of that loop (every invocation that is
        // in the loop reaches it), in a loop with barriers, on a value read from LDS.
        if (loops.empty() || loops.back().first != node.data.break_node.merge ||
            loops.back().second != 0 || !barrier_loops.contains(node.data.break_node.merge) ||
            i == 0 || list[i - 1].type != Type::Block ||
            !ReadsShared(IR::Value{node.data.break_node.cond})) {
            continue;
        }
        IR::Block* const block = list[i - 1].data.block;
        IR::IREmitter ir{*block, block->end()};
        IR::U1 cond = node.data.break_node.cond;
        if (diverge) {
            const IR::U32 index = ir.GetAttributeU32(IR::Attribute::LocalInvocationIndex);
            cond = ir.LogicalOr(cond, ir.IGreaterThanEqual(index, ir.Imm32(512u), false));
        }
        if (uniform) {
            const IR::U32 offset = ir.Imm32(flag_offset);
            ir.WriteShared(32, ir.Select(cond, ir.Imm32(1u), ir.Imm32(0u)), offset);
            ir.Barrier();
            const IR::U32 decision{ir.LoadShared(32, false, offset)};
            ir.Barrier();
            cond = ir.INotEqual(decision, ir.Imm32(0u));
        }
        node.data.break_node.cond = ir.ConditionRef(cond);
        ++patched;
    }
    if (uniform && patched) {
        program.info.shared_memory_scratch_size =
            flag_offset + 4 - runtime_info.hw.cs.shared_memory_size;
    }
    static u32 reports = 0;
    if (reports < 8) {
        ++reports;
        std::printf("GPU: cloth solver %#llx: %u LDS-decided loop exit(s) %s%s (GOW3_SAFE_CLOTH, "
                    "%u-lane subgroups)\n",
                    static_cast<unsigned long long>(hash), patched,
                    uniform ? "taken as one group decision" : "left per invocation",
                    diverge ? " [test: invocations 512+ leave at once]" : "",
                    profile.subgroup_size);
        std::fflush(stdout);
    }
}

} // namespace Shader::Optimization
