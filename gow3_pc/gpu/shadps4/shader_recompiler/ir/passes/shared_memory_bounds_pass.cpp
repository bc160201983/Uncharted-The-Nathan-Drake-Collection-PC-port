// SPDX-FileCopyrightText: Copyright 2026 gow3_pc
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "common/alignment.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Optimization {

// gow3 6 Nov 2026 (Uncharted 3 chapter 2 device lost as young Sully streams in, GitHub #18 #138
// #238 #292 #295 #342 #365; analysis/sully-1106/FINDINGS.md): the PS4's GPU checks every LDS
// access against the workgroup's LDS allocation (LDS_SIZE): a read beyond it returns 0, a write
// beyond it is dropped. Naughty Dog's cloth solvers rely on it: 1c957b01 / 809864d3 allocate 2 KiB
// (512 dwords) for 1024 threads and sum lds[i] + lds[i + 512]. Vulkan shared memory has no such
// check. Those reads land in memory after the array (our scratch: NaN, the old shirt flashes)
// or, depending on the GPU, outside the workgroup's shared memory, which NVIDIA and Intel GPUs
// stop on: a shader exception and a lost device, with an instruction pointer and no bad address
// in the report (the chapter 2 class). AMD GPUs bounds-check shared memory like the PS4, which is
// why no AMD report exists. Here every LDS access of a listed shader is checked as on the PS4: an
// out-of-range access goes to a spare dword after the allocation, and an out-of-range read or
// atomic returns 0. Every compute shader of the three games gets it (the user, 6 Nov: "if the same
// happens in U1 and U2, fix that as well": any of their shaders may rely on the same zeros).
// GOW3_LDS_BOUNDS=0: off; =<hex hashes, comma separated>: only those; =cloth: only the cloth
// solvers; unset or =all: every compute shader. Cache names: SharedMemoryBoundsTag (compute
// shaders only, so graphics shaders keep their cached binaries).

namespace {
enum class BoundsMode { Off, Listed, All };

struct BoundsConfig {
    BoundsMode mode = BoundsMode::All;
    std::vector<u64> hashes{0x1c957b01ULL, 0x809864d3ULL};
};

const BoundsConfig& Config() {
    static const BoundsConfig config = [] {
        BoundsConfig c;
        const char* env = std::getenv("GOW3_LDS_BOUNDS");
        if (!env || !env[0]) {
            return c;
        }
        const std::string value{env};
        if (value == "0") {
            c.mode = BoundsMode::Off;
        } else if (value == "all" || value == "1") {
            c.mode = BoundsMode::All;
        } else if (value == "cloth") {
            c.mode = BoundsMode::Listed;
        } else {
            c.mode = BoundsMode::Listed;
            c.hashes.clear();
            size_t start = 0;
            while (start < value.size()) {
                const size_t end = value.find(',', start);
                const std::string item =
                    value.substr(start, end == std::string::npos ? std::string::npos : end - start);
                if (!item.empty()) {
                    c.hashes.push_back(std::strtoull(item.c_str(), nullptr, 16));
                }
                if (end == std::string::npos) {
                    break;
                }
                start = end + 1;
            }
        }
        return c;
    }();
    return config;
}

/// Bytes an LDS instruction touches, or 0 if it is not one; `value` tells if it returns one.
u32 AccessBytes(IR::Opcode op, bool& value) {
    value = true;
    switch (op) {
    case IR::Opcode::LoadSharedU16:
        return 2;
    case IR::Opcode::WriteSharedU16:
        value = false;
        return 2;
    case IR::Opcode::LoadSharedU32:
    case IR::Opcode::SharedAtomicIAdd32:
    case IR::Opcode::SharedAtomicISub32:
    case IR::Opcode::SharedAtomicSMin32:
    case IR::Opcode::SharedAtomicUMin32:
    case IR::Opcode::SharedAtomicSMax32:
    case IR::Opcode::SharedAtomicUMax32:
    case IR::Opcode::SharedAtomicInc32:
    case IR::Opcode::SharedAtomicDec32:
    case IR::Opcode::SharedAtomicAnd32:
    case IR::Opcode::SharedAtomicOr32:
    case IR::Opcode::SharedAtomicXor32:
    case IR::Opcode::SharedAtomicCmpSwap32:
        return 4;
    case IR::Opcode::WriteSharedU32:
        value = false;
        return 4;
    case IR::Opcode::LoadSharedU64:
    case IR::Opcode::SharedAtomicIAdd64:
    case IR::Opcode::SharedAtomicISub64:
    case IR::Opcode::SharedAtomicSMin64:
    case IR::Opcode::SharedAtomicUMin64:
    case IR::Opcode::SharedAtomicSMax64:
    case IR::Opcode::SharedAtomicUMax64:
    case IR::Opcode::SharedAtomicInc64:
    case IR::Opcode::SharedAtomicDec64:
    case IR::Opcode::SharedAtomicAnd64:
    case IR::Opcode::SharedAtomicOr64:
    case IR::Opcode::SharedAtomicXor64:
    case IR::Opcode::SharedAtomicCmpSwap64:
        return 8;
    case IR::Opcode::WriteSharedU64:
        value = false;
        return 8;
    default:
        return 0;
    }
}
} // namespace

namespace {
constexpr u64 Unknown = ~0ULL;

/// An upper bound of an LDS address (thread ids at their largest), or Unknown.
u64 UpperBound(const IR::Value& value, const RuntimeInfo& runtime_info, u32 depth = 0) {
    if (value.IsImmediate()) {
        return value.Type() == IR::Type::U32 ? value.U32() : Unknown;
    }
    const IR::Inst* inst = value.TryInst();
    if (!inst || depth > 12) {
        return Unknown;
    }
    const auto& wg = runtime_info.hw.cs.workgroup_size;
    const auto arg = [&](u32 i) { return UpperBound(inst->Arg(i), runtime_info, depth + 1); };
    const auto both = [](u64 a, u64 b, auto op) { return a == Unknown || b == Unknown ? Unknown : op(a, b); };
    switch (inst->GetOpcode()) {
    case IR::Opcode::GetAttributeU32:
        if (inst->Arg(0).Attribute() == IR::Attribute::LocalInvocationId) {
            const u32 component = inst->Arg(1).U32();
            return component < 3 ? u64(wg[component]) - 1 : Unknown;
        }
        if (inst->Arg(0).Attribute() == IR::Attribute::LocalInvocationIndex) {
            return u64(wg[0]) * wg[1] * wg[2] - 1;
        }
        return Unknown;
    case IR::Opcode::IAdd32:
        return both(arg(0), arg(1), [](u64 a, u64 b) { return a + b; });
    case IR::Opcode::BitwiseOr32:
        return both(arg(0), arg(1), [](u64 a, u64 b) { return a + b; });
    case IR::Opcode::IMul32:
        return both(arg(0), arg(1), [](u64 a, u64 b) { return a * b; });
    case IR::Opcode::ShiftLeftLogical32:
        return both(arg(0), arg(1), [](u64 a, u64 b) { return b < 32 ? a << b : Unknown; });
    case IR::Opcode::ShiftRightLogical32:
        // x >> s <= x: only an immediate shift tightens the bound.
        return inst->Arg(1).IsImmediate() ? (arg(0) == Unknown ? Unknown : arg(0) >> (inst->Arg(1).U32() & 31))
                                          : arg(0);
    case IR::Opcode::BitwiseAnd32: {
        const u64 a = arg(0), b = arg(1);
        return a == Unknown ? b : b == Unknown ? a : std::min(a, b);
    }
    case IR::Opcode::UMin32: {
        const u64 a = arg(0), b = arg(1);
        return a == Unknown ? b : b == Unknown ? a : std::min(a, b);
    }
    case IR::Opcode::SelectU32:
        return both(arg(1), arg(2), [](u64 a, u64 b) { return std::max(a, b); });
    default:
        return Unknown;
    }
}
} // namespace

bool SharedMemoryBounds(u64 pgm_hash) {
    const BoundsConfig& c = Config();
    switch (c.mode) {
    case BoundsMode::Off:
        return false;
    case BoundsMode::All:
        return true;
    case BoundsMode::Listed:
        break;
    }
    for (const u64 hash : c.hashes) {
        if (hash == pgm_hash) {
            return true;
        }
    }
    return false;
}

std::string SharedMemoryBoundsTag(u64 pgm_hash, bool compute) {
    if (!compute || !SharedMemoryBounds(pgm_hash)) {
        return {};
    }
    return Config().mode == BoundsMode::All ? "_lba" : "_lb1";
}

void SharedMemoryBoundsPass(IR::Program& program, const RuntimeInfo& runtime_info,
                            const Profile& profile) {
    if (program.info.hw_stage != HwStage::Compute || !SharedMemoryBounds(program.info.pgm_hash)) {
        return;
    }
    const u32 size = runtime_info.hw.cs.shared_memory_size;
    if (size < 8) {
        return;
    }
    // The spare dword must fit under the device's limit (review 6 Nov: a shader at the limit,
    // e.g. 32 KiB on a GPU that allows 32 KiB, would be declared over it). Such shaders keep the
    // accesses as before.
    if (u64(size) + program.info.shared_memory_scratch_size + 16 > profile.max_shared_memory_size) {
        static std::atomic<u32> full_reports{0};
        if (full_reports.fetch_add(1) < 8) {
            std::printf("GPU: cs %#llx: %u bytes of LDS leave no room for the bounds checks' spare "
                        "dword under the device's %u: not checked\n",
                        static_cast<unsigned long long>(program.info.pgm_hash), size,
                        profile.max_shared_memory_size);
            std::fflush(stdout);
        }
        return;
    }
    // The spare dword (8 bytes, for 64-bit accesses) after the allocation and any scratch so far.
    const u32 spare =
        Common::AlignUp(size + program.info.shared_memory_scratch_size, u32(sizeof(u64)));
    u32 checked = 0, beyond = 0;
    u64 furthest = 0;
    for (IR::Block* const block : program.blocks) {
        auto& insts = block->Instructions();
        for (auto it = insts.begin(); it != insts.end(); ++it) {
            IR::Inst& inst = *it;
            bool returns_value = false;
            const u32 bytes = AccessBytes(inst.GetOpcode(), returns_value);
            if (bytes == 0 || inst.Flags<bool>()) { // not LDS, or GDS
                continue;
            }
            const IR::Value address = inst.Arg(0);
            if (address.IsImmediate() && u64(address.U32()) + bytes <= size) {
                continue;
            }
            // Evidence for the log: accesses whose address provably reaches past the allocation.
            const u64 bound = UpperBound(address, runtime_info);
            if (bound != Unknown && bound + bytes > size) {
                ++beyond;
                furthest = std::max(furthest, bound + bytes);
            }
            IR::IREmitter before{*block, it};
            const IR::U1 in_range =
                before.ILessThanEqual(IR::U32{address}, before.Imm32(size - bytes), false);
            inst.SetArg(0, before.Select(in_range, address, before.Imm32(spare)));
            ++checked;
            if (!returns_value) {
                continue;
            }
            // Out of range reads 0: a select after the access takes over its uses.
            const std::vector<IR::Use> uses(inst.Uses().begin(), inst.Uses().end());
            IR::IREmitter after{*block, std::next(it)};
            IR::Value result;
            if (bytes == 2) {
                const IR::U32 wide{after.UConvert(32, IR::U16{&inst})};
                result = after.UConvert(16, IR::U32{after.Select(in_range, wide, after.Imm32(0))});
            } else if (bytes == 8) {
                result = after.Select(in_range, IR::Value{&inst}, after.Imm64(u64{0}));
            } else {
                result = after.Select(in_range, IR::Value{&inst}, after.Imm32(0));
            }
            for (const IR::Use& use : uses) {
                use.user->SetArg(use.operand, result);
            }
            it = IR::Block::InstructionList::s_iterator_to(*result.Inst());
        }
    }
    if (checked) {
        program.info.shared_memory_scratch_size = spare + sizeof(u64) - size;
    }
    // Every shader whose accesses provably reach past its allocation (the PS4 reads zeros there);
    // a few of the others.
    static std::atomic<u32> reports{0}, beyond_reports{0};
    if (beyond ? beyond_reports.fetch_add(1) < 128 : reports.fetch_add(1) < 4) {
        char beyond_text[96] = "";
        if (beyond) {
            std::snprintf(beyond_text, sizeof(beyond_text),
                          "; %u of them reach past it, up to byte %llu", beyond,
                          static_cast<unsigned long long>(furthest));
        }
        std::printf("GPU: cs %#llx: %u LDS accesses bounds-checked like the PS4 (%u bytes, %ux%ux%u "
                    "threads)%s\n",
                    static_cast<unsigned long long>(program.info.pgm_hash), checked, size,
                    runtime_info.hw.cs.workgroup_size[0], runtime_info.hw.cs.workgroup_size[1],
                    runtime_info.hw.cs.workgroup_size[2],
                    beyond_text);
        std::fflush(stdout);
    }
}

} // namespace Shader::Optimization
