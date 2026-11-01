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
    return IsEffectShader(pgm_hash) && (enabled || TestEffectLoopCount() != 0);
}

void EffectLoopClampPass(IR::Program& program) {
    const u64 hash = program.info.pgm_hash;
    if (!EffectLoopClamp(hash)) {
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
