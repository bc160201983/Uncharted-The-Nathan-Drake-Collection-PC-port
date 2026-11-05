// SPDX-FileCopyrightText: Copyright 2026 gow3_pc
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/resource.h"

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

void EffectLoopClampPass(IR::Program& program) {
    const u64 hash = program.info.pgm_hash;
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

} // namespace Shader::Optimization
