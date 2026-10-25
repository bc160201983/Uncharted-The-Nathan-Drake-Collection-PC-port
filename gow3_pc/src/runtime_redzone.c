/* Red-zone safety for guest leaf functions (gow3 25 Oct 2026; Windows).
 *
 * The games follow the System V x86-64 ABI: a leaf function may keep data in the 128 bytes below
 * rsp (the red zone). The GPU page tracker resolves its access faults on the faulting guest thread,
 * and Windows builds the exception frame on that thread's stack directly below rsp, over those
 * bytes. Our test PC's Windows build happens to leave about 1.6 KB free there; players' builds may
 * leave none, and a leaf that faults then carries on with corrupted locals (the Drake's Fortune
 * constant-table crash, GitHub #27/#42; the Uncharted 2 run grouping at 0xd09665).
 *
 * At load time, after every other code hook, this pass finds the guest functions from the
 * image's .eh_frame_hdr tables, decodes them, and in each function that accesses memory below rsp
 * moves every access that can fault (base register not rsp, not rip-relative) into a stub:
 *     lea rsp, [rsp - 128]; <the access, with the instructions after it if it is short>;
 *     lea rsp, [rsp + 128]; jmp back
 * (lea leaves the flags alone). An exception frame then lands below the function's red zone.
 * A site needs 5 bytes for its jmp; instructions that are moved along never cross a branch
 * target. Functions changed by a hook (a branch out of the image, or into the middle of another
 * function: our stubs and probe_uncharted.py's code caves) are left alone, as are functions with
 * an indirect jump (a jump table: its targets are unknown, and its entries may sit in the code).
 *
 * GOW3_RED_ZONE=0: off. GOW3_RED_ZONE=list: each rewritten function is printed.
 * GOW3_RED_ZONE_SCRIBBLE=1 (test): after each GPU-tracking fault the guest survives, the 128 bytes
 * below the faulting rsp are filled with 0xcc, as a Windows build without that slack would do.
 * A fault from a red-zone function's access that could not be moved is reported once per site. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

uint64_t runtime_redzone_install(unsigned char *image, uint64_t size, const uint64_t *segments, uint64_t count);
void runtime_redzone_after_fault(void *context);

#ifdef _WIN32
#include <windows.h>
#include <Zydis/Zydis.h>

typedef struct { uint64_t begin, end; uint8_t red, hooked, bad, indirect; } RzFunction;
/* One decoded instruction of a function being rewritten. branch: a direct jcc (1) or jmp (2)
 * that a stub can repeat with a 32-bit displacement (cc: its condition), to image offset `to`. */
typedef struct { uint64_t at, to; uint8_t length, fault, move, target, moved, branch, cc; } RzOp;

static unsigned char *rz_image;
static uint64_t rz_size, rz_code_end;
static uint8_t *rz_targets, *rz_starts;      /* one bit per image byte up to rz_code_end */
static ZydisDecoder rz_decoder;
/* Stub space: 64 KiB chunks within jump range of the image (the runtime's reservations leave
 * only small free holes near the low image, so there is no one large block). */
#define RZ_CHUNK ((size_t)64 << 10)
#define RZ_MAX_CHUNKS 128
static unsigned char *rz_chunk[RZ_MAX_CHUNKS];
static size_t rz_chunks, rz_chunk_used, rz_stub_bytes;
static int rz_no_space;
static unsigned char *allocate_near(size_t bytes);
/* Faulting accesses in red-zone functions that stayed in place (sorted image offsets). */
static uint32_t *rz_skipped;
static volatile LONG *rz_warned;
static size_t rz_skipped_count, rz_skipped_cap;
static int rz_scribble, rz_ready, rz_protecting;
/* Test mode (GOW3_RED_ZONE_SCRIBBLE=1) diagnostics: the red-zone functions (sorted ranges), each
 * stub's start and the site it replaces (in stub order), first-fault flags, fault counts. */
typedef struct { uint32_t begin, end; } RzRange;
static RzRange *rz_red;
static size_t rz_red_count;
typedef struct { uintptr_t stub; uint32_t site; } RzStub;
static RzStub *rz_stubs;
static size_t rz_stub_count, rz_stub_cap;
static volatile LONG *rz_red_seen, *rz_stub_seen;
static volatile LONG rz_scribbles, rz_in_red, rz_in_stub, rz_prints;
static volatile LONG64 rz_last_report;

static void bit_set(uint8_t *map, uint64_t at) { if (at < rz_code_end) map[at >> 3] |= (uint8_t)(1u << (at & 7)); }
static int bit_get(const uint8_t *map, uint64_t at) { return at < rz_code_end && ((map[at >> 3] >> (at & 7)) & 1); }
static uint32_t rd32(const unsigned char *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static int32_t rd32s(const unsigned char *p) { int32_t v; memcpy(&v, p, 4); return v; }

static int executable_at(const uint64_t *segs, uint64_t nseg, uint64_t at, uint64_t *segment_end) {
    for (uint64_t s = 0; s < nseg; ++s) {
        const uint64_t base = segs[3 * s], bytes = segs[3 * s + 1], flags = segs[3 * s + 2];
        if ((flags & 1) && at >= base && at - base < bytes) {
            if (segment_end) *segment_end = base + bytes;
            return 1;
        }
    }
    return 0;
}
/* An FDE's pc_begin (pc-relative sdata4, as in all three games), or -1 for a CIE / bad entry. */
static int64_t fde_begin(uint64_t fde) {
    if (fde + 16 > rz_size) return -1;
    const uint32_t length = rd32(rz_image + fde);
    if (length < 12 || length == 0xffffffffu || !rd32(rz_image + fde + 4)) return -1;
    return (int64_t)fde + 8 + rd32s(rz_image + fde + 8);
}
/* The entry count of a .eh_frame_hdr at image offset `at` (version 1; eh_frame_ptr pcrel sdata4,
 * fde_count udata4, table datarel sdata4), 0 if this is not one: the table must be sorted, name
 * code in an executable segment, and its first FDEs must agree on where their functions start. */
static uint32_t eh_table(uint64_t at, const uint64_t *segs, uint64_t nseg) {
    const unsigned char *h = rz_image + at;
    if (at + 12 > rz_size || h[0] != 1 || h[1] != 0x1b || h[2] != 0x03 || h[3] != 0x3b) return 0;
    const uint32_t count = rd32(h + 8);
    if (count < 16 || count > (1u << 22) || at + 12 + (uint64_t)count * 8 > rz_size) return 0;
    const int64_t frames = (int64_t)at + 4 + rd32s(h + 4);
    if (frames < 0 || (uint64_t)frames >= rz_size) return 0;
    int64_t previous = -1;
    for (uint32_t i = 0; i < count; ++i) {
        const int64_t pc = (int64_t)at + rd32s(h + 12 + 8 * (uint64_t)i);
        const int64_t fde = (int64_t)at + rd32s(h + 16 + 8 * (uint64_t)i);
        if (pc <= previous || !executable_at(segs, nseg, (uint64_t)pc, NULL) || fde < 0 || (uint64_t)fde + 16 > rz_size)
            return 0;
        if (i < 64 && fde_begin((uint64_t)fde) != pc) return 0;
        previous = pc;
    }
    return count;
}

static int decode(uint64_t at, uint64_t end, ZydisDecodedInstruction *in, ZydisDecodedOperand *ops) {
    return ZYAN_SUCCESS(ZydisDecoderDecodeFull(&rz_decoder, rz_image + at, (ZyanUSize)(end - at), in, ops));
}
static int stack_register(ZydisRegister r) {
    return r == ZYDIS_REGISTER_RSP || r == ZYDIS_REGISTER_ESP || r == ZYDIS_REGISTER_SP || r == ZYDIS_REGISTER_SPL;
}
static int control_flow(const ZydisDecodedInstruction *in) {
    switch (in->meta.category) {
    case ZYDIS_CATEGORY_COND_BR: case ZYDIS_CATEGORY_UNCOND_BR: case ZYDIS_CATEGORY_CALL:
    case ZYDIS_CATEGORY_RET: case ZYDIS_CATEGORY_INTERRUPT: case ZYDIS_CATEGORY_SYSCALL:
    case ZYDIS_CATEGORY_SYSRET: case ZYDIS_CATEGORY_SYSTEM:
        return 1;
    default:
        return 0;
    }
}
/* 1 for a relative branch; *target = its image offset, or UINT64_MAX outside the image. */
static int branch_target(const ZydisDecodedInstruction *in, const ZydisDecodedOperand *ops, uint64_t at, uint64_t *target) {
    for (unsigned k = 0; k < in->operand_count_visible; ++k) {
        if (ops[k].type != ZYDIS_OPERAND_TYPE_IMMEDIATE || !ops[k].imm.is_relative) continue;
        ZyanU64 address = 0;
        if (ZYAN_FAILED(ZydisCalcAbsoluteAddress(in, &ops[k], (ZyanU64)(uintptr_t)(rz_image + at), &address))) return 0;
        const uint64_t offset = (uint64_t)address - (uint64_t)(uintptr_t)rz_image;
        *target = offset < rz_size ? offset : UINT64_MAX;
        return 1;
    }
    return 0;
}
/* Pass 1: branch targets (all functions), whether the function keeps data below rsp, whether a
 * hook changed it, and whether it has an indirect jump (a jump table's targets are unknown). */
static void scan_function(RzFunction *f) {
    for (uint64_t at = f->begin; at < f->end;) {
        ZydisDecodedInstruction in;
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
        if (!decode(at, f->end, &in, ops)) { f->bad = 1; return; }
        uint64_t target;
        if (branch_target(&in, ops, at, &target)) {
            if (target == UINT64_MAX) f->hooked = 1;           /* a hook's jump to its stub page */
            else {
                bit_set(rz_targets, target);
                /* Code only enters another function at its start; anything else is a code cave. */
                if ((target < f->begin || target >= f->end) && !bit_get(rz_starts, target)) f->hooked = 1;
            }
        } else if (in.meta.category == ZYDIS_CATEGORY_UNCOND_BR &&
                   !(ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY && ops[0].mem.base == ZYDIS_REGISTER_RIP &&
                     ops[0].mem.index == ZYDIS_REGISTER_NONE)) {
            f->indirect = 1;
        }
        for (unsigned k = 0; k < in.operand_count_visible; ++k)
            if (ops[k].type == ZYDIS_OPERAND_TYPE_MEMORY && ops[k].mem.type != ZYDIS_MEMOP_TYPE_AGEN &&
                ops[k].mem.base == ZYDIS_REGISTER_RSP && ops[k].mem.disp.value < 0)
                f->red = 1;
        at += in.length;
    }
}
static int prefetch(ZydisMnemonic m) {
    return m == ZYDIS_MNEMONIC_PREFETCHNTA || m == ZYDIS_MNEMONIC_PREFETCHT0 || m == ZYDIS_MNEMONIC_PREFETCHT1 ||
           m == ZYDIS_MNEMONIC_PREFETCHT2 || m == ZYDIS_MNEMONIC_PREFETCHW || m == ZYDIS_MNEMONIC_PREFETCH;
}
/* fault: the instruction reads or writes memory that can be a GPU-tracked page.
 * move: it can run from a stub (no control flow, rsp only as a memory base with a SIB byte,
 * rip-relative only with a 32-bit displacement). */
static void classify(const ZydisDecodedInstruction *in, const ZydisDecodedOperand *ops, RzOp *op) {
    int move = !control_flow(in) && !(in->attributes & (ZYDIS_ATTRIB_HAS_EVEX | ZYDIS_ATTRIB_HAS_MVEX));
    int fault = 0, rsp = 0, rip = 0;
    for (unsigned k = 0; k < in->operand_count; ++k) {
        const ZydisDecodedOperand *o = &ops[k];
        if (o->type == ZYDIS_OPERAND_TYPE_REGISTER && stack_register(o->reg.value)) move = 0;
        if (o->type != ZYDIS_OPERAND_TYPE_MEMORY) continue;
        if (stack_register(o->mem.index)) move = 0;
        if (o->mem.base == ZYDIS_REGISTER_RSP) {
            if (o->visibility != ZYDIS_OPERAND_VISIBILITY_EXPLICIT) move = 0; else rsp = 1;
            continue;
        }
        if (o->mem.base == ZYDIS_REGISTER_RIP) { rip = 1; continue; }
        if (o->mem.type != ZYDIS_MEMOP_TYPE_MEM && o->mem.type != ZYDIS_MEMOP_TYPE_VSIB) continue;
        if (o->mem.segment == ZYDIS_REGISTER_FS || o->mem.segment == ZYDIS_REGISTER_GS) continue;  /* TEB */
        if (o->mem.base == ZYDIS_REGISTER_NONE && o->mem.index == ZYDIS_REGISTER_NONE) continue;  /* image */
        if (in->mnemonic == ZYDIS_MNEMONIC_NOP || prefetch(in->mnemonic)) continue;
        fault = 1;
    }
    if (rsp && (!(in->attributes & ZYDIS_ATTRIB_HAS_MODRM) || !(in->attributes & ZYDIS_ATTRIB_HAS_SIB) ||
                (in->raw.disp.size && in->raw.disp.offset != in->raw.sib.offset + 1))) move = 0;
    if (rip && in->raw.disp.size != 32) move = 0;
    if (rsp && rip) move = 0;
    op->fault = (uint8_t)fault;
    op->move = (uint8_t)move;
}
/* Copies the instruction at image offset `at` to `out` (its address), fixing an rsp-based
 * displacement (+128: rsp is 128 lower in the stub) or a rip-relative one. Returns the new length,
 * 0 if it cannot be expressed. */
static unsigned relocate(uint64_t at, uint64_t end, unsigned char *out) {
    ZydisDecodedInstruction in;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    if (!decode(at, end, &in, ops)) return 0;
    const unsigned char *src = rz_image + at;
    int rsp = 0, rip = 0;
    for (unsigned k = 0; k < in.operand_count_visible; ++k)
        if (ops[k].type == ZYDIS_OPERAND_TYPE_MEMORY) {
            if (ops[k].mem.base == ZYDIS_REGISTER_RSP) rsp = 1;
            if (ops[k].mem.base == ZYDIS_REGISTER_RIP) rip = 1;
        }
    if (rsp) {
        const unsigned modrm = in.raw.modrm.offset, place = in.raw.sib.offset + 1u, old = in.raw.disp.size / 8u;
        const int64_t disp = (old ? in.raw.disp.value : 0) + 128;
        unsigned n = place;
        memcpy(out, src, place);
        if (disp >= -128 && disp <= 127) {
            out[modrm] = (unsigned char)((src[modrm] & 0x3f) | 0x40);
            out[n++] = (unsigned char)(int8_t)disp;
        } else if (disp >= INT32_MIN && disp <= INT32_MAX) {
            const int32_t d = (int32_t)disp;
            out[modrm] = (unsigned char)((src[modrm] & 0x3f) | 0x80);
            memcpy(out + n, &d, 4);
            n += 4;
        } else {
            return 0;
        }
        memcpy(out + n, src + place + old, in.length - place - old);
        return n + (unsigned)(in.length - place - old);
    }
    memcpy(out, src, in.length);
    if (rip) {
        const int64_t target = (int64_t)(uintptr_t)(src + in.length) + in.raw.disp.value;
        const int64_t disp = target - (int64_t)(uintptr_t)(out + in.length);
        if (disp < INT32_MIN || disp > INT32_MAX) return 0;
        const int32_t d = (int32_t)disp;
        memcpy(out + in.raw.disp.offset, &d, 4);
    }
    return in.length;
}
static int jump_fits(const unsigned char *from_end, const unsigned char *to) {
    const int64_t delta = (int64_t)(uintptr_t)to - (int64_t)(uintptr_t)from_end;
    return delta >= INT32_MIN && delta <= INT32_MAX;
}
/* Moves ops[k..j) into a stub and writes the jump to it. 0 if the stub cannot be built. Moved
 * branches run with rsp as it was (their targets see the normal stack), the other instructions
 * between lea rsp,[rsp-128] and lea rsp,[rsp+128]; a stub that does not end in a jmp goes back
 * to the instruction after the group. */
static int emit_group(const RzFunction *f, RzOp *ops, size_t k, size_t j) {
    static const unsigned char down[] = {0x48, 0x8d, 0x64, 0x24, 0x80};                    /* lea rsp,[rsp-128] */
    static const unsigned char up[] = {0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00};    /* lea rsp,[rsp+128] */
    const uint64_t site = ops[k].at, resume = ops[j - 1].at + ops[j - 1].length;
    size_t worst = 5;
    for (size_t m = k; m < j; ++m) worst += ops[m].length + 4u + sizeof(down) + sizeof(up) + 6u;
    if (worst > RZ_CHUNK) return 0;
    if (!rz_chunks || rz_chunk_used + worst > RZ_CHUNK) {
        unsigned char *chunk = rz_no_space || rz_chunks == RZ_MAX_CHUNKS ? NULL : allocate_near(RZ_CHUNK);
        if (!chunk) { rz_no_space = 1; return 0; }
        rz_chunk[rz_chunks++] = chunk;
        rz_chunk_used = 0;
    }
    unsigned char *const base = rz_chunk[rz_chunks - 1];
    unsigned char *stub = base + rz_chunk_used, *p = stub;
    if (!jump_fits(rz_image + site + 5, stub)) return 0;
    int low = 0;
    for (size_t m = k; m < j; ++m) {
        if (ops[m].branch) {
            if (low) { memcpy(p, up, sizeof(up)); p += sizeof(up); low = 0; }
            const unsigned size = ops[m].branch == 1 ? 6u : 5u;
            if (!jump_fits(p + size, rz_image + ops[m].to)) return 0;
            const int32_t rel = (int32_t)((int64_t)(uintptr_t)(rz_image + ops[m].to) - (int64_t)(uintptr_t)(p + size));
            if (ops[m].branch == 1) { *p++ = 0x0f; *p++ = (unsigned char)(0x80 | ops[m].cc); }  /* jcc rel32 */
            else *p++ = 0xe9;                                                                   /* jmp rel32 */
            memcpy(p, &rel, 4);
            p += 4;
            continue;
        }
        if (!low) { memcpy(p, down, sizeof(down)); p += sizeof(down); low = 1; }
        const unsigned n = relocate(ops[m].at, f->end, p);
        if (!n) return 0;
        p += n;
    }
    if (low) { memcpy(p, up, sizeof(up)); p += sizeof(up); }
    if (ops[j - 1].branch != 2) {
        if (!jump_fits(p + 5, rz_image + resume)) return 0;
        const int32_t back = (int32_t)((int64_t)(uintptr_t)(rz_image + resume) - (int64_t)(uintptr_t)(p + 5));
        *p++ = 0xe9; memcpy(p, &back, 4); p += 4;
    }
    const int32_t there = (int32_t)((int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(rz_image + site + 5));
    rz_image[site] = 0xe9;
    memcpy(rz_image + site + 1, &there, 4);
    /* The rest of the replaced bytes: int3 (nothing branches there; anything that did would stop
     * with a report instead of skipping the moved instructions). */
    memset(rz_image + site + 5, 0xcc, (size_t)(resume - site - 5));
    if (rz_scribble) {
        if (rz_stub_count == rz_stub_cap) {
            const size_t cap = rz_stub_cap ? rz_stub_cap * 2 : 4096;
            RzStub *grown = realloc(rz_stubs, cap * sizeof(*grown));
            if (grown) { rz_stubs = grown; rz_stub_cap = cap; }
        }
        if (rz_stub_count < rz_stub_cap) rz_stubs[rz_stub_count++] = (RzStub){(uintptr_t)stub, (uint32_t)site};
    }
    const size_t used = (size_t)(p - base + 15) & ~(size_t)15;
    rz_stub_bytes += used - rz_chunk_used;
    rz_chunk_used = used;
    for (size_t m = k; m < j; ++m) ops[m].moved = 1;
    return 1;
}
static void note_skipped(uint64_t at) {
    if (rz_skipped_count == rz_skipped_cap) {
        const size_t cap = rz_skipped_cap ? rz_skipped_cap * 2 : 256;
        uint32_t *grown = realloc(rz_skipped, cap * sizeof(*grown));
        if (!grown) return;
        rz_skipped = grown;
        rz_skipped_cap = cap;
    }
    rz_skipped[rz_skipped_count++] = (uint32_t)at;
}
/* Pass 2 for one red-zone function: groups of instructions around each faulting access, each at
 * least 5 bytes, no branch target inside (its first instruction may be one), moved to stubs. */
static void protect_function(const RzFunction *f, int list, uint64_t *moved, uint64_t *skipped, uint64_t *groups) {
    size_t cap = (size_t)(f->end - f->begin), n = 0;
    RzOp *ops = calloc(cap ? cap : 1, sizeof(*ops));
    if (!ops) return;
    for (uint64_t at = f->begin; at < f->end && n < cap;) {
        ZydisDecodedInstruction in;
        ZydisDecodedOperand dec[ZYDIS_MAX_OPERAND_COUNT];
        if (!decode(at, f->end, &in, dec)) { free(ops); return; }
        ops[n].at = at;
        ops[n].length = in.length;
        ops[n].target = (uint8_t)bit_get(rz_targets, at);
        classify(&in, dec, &ops[n]);
        uint64_t to;
        if ((in.meta.category == ZYDIS_CATEGORY_COND_BR || in.meta.category == ZYDIS_CATEGORY_UNCOND_BR) &&
            branch_target(&in, dec, at, &to) && to != UINT64_MAX) {
            if ((in.opcode_map == ZYDIS_OPCODE_MAP_DEFAULT && in.opcode >= 0x70 && in.opcode <= 0x7f) ||
                (in.opcode_map == ZYDIS_OPCODE_MAP_0F && in.opcode >= 0x80 && in.opcode <= 0x8f)) {
                ops[n].branch = 1;
                ops[n].cc = (uint8_t)(in.opcode & 15);
            } else if (in.opcode_map == ZYDIS_OPCODE_MAP_DEFAULT && (in.opcode == 0xeb || in.opcode == 0xe9)) {
                ops[n].branch = 2;
            }
            ops[n].to = to;
        }
        ++n;
        at += in.length;
    }
    uint64_t here_moved = 0, here_skipped = 0, here_groups = 0;
    size_t next_free = 0;   /* instructions before it are already in a stub */
    /* An indirect jump may use a jump table, whose targets are unknown and whose entries the
     * compiler can place inside the function's bytes: such functions are left as they are. */
    if (!f->hooked && !f->indirect)
        for (size_t i = 0; i < n; ++i) {
            if (i < next_free || !ops[i].fault || !ops[i].move) continue;
            /* The access first, then up to four instructions before it if the ones after it run
             * out. Direct jcc/jmp may be part of a group; nothing follows a jmp in one. */
            for (size_t back = 0; back <= 4 && back <= i && i - back >= next_free; ++back) {
                const size_t k = i - back;
                if (back && ((!ops[k].move && !ops[k].branch) || ops[k].branch == 2 || ops[k + 1].target)) break;
                unsigned length = 0;
                for (size_t m = k; m <= i; ++m) length += ops[m].length;
                size_t j = i + 1;
                int ok = 1;
                while (length < 5) {
                    if (j >= n || ops[j].target || (!ops[j].move && !ops[j].branch) || ops[j - 1].branch == 2) { ok = 0; break; }
                    length += ops[j].length;
                    ++j;
                }
                if (ok && emit_group(f, ops, k, j)) {
                    ++here_groups;
                    next_free = j;
                    break;
                }
            }
        }
    for (size_t i = 0; i < n; ++i) {
        if (!ops[i].fault) continue;
        if (ops[i].moved) { ++here_moved; continue; }
        ++here_skipped;
        note_skipped(ops[i].at);
        if (list && !f->hooked && !f->indirect) printf("Red zone:   access left in place at %#" PRIx64 "\n", ops[i].at);
    }
    if (list)
        printf("Red zone: function %#" PRIx64 " (+%#" PRIx64 ")%s: %" PRIu64 " accesses moved in %" PRIu64 " stubs, %" PRIu64 " left\n",
               f->begin, f->end - f->begin, f->hooked ? " hooked, left as it is" : f->indirect ? " indirect jump, left as it is" : "",
               here_moved, here_groups, here_skipped);
    *moved += here_moved;
    *skipped += here_skipped;
    *groups += here_groups;
    free(ops);
}
/* Stub space within relative-jump range of the image (as allocate_near_image in probe.c: free
 * regions within +-1.5 GiB, below the image too). */
static unsigned char *allocate_near(size_t bytes) {
    const uintptr_t base = (uintptr_t)rz_image;
    const uintptr_t lo = base > UINT64_C(0x60000000) ? base - UINT64_C(0x60000000) : 0x10000;
    const uintptr_t hi = base + (uintptr_t)rz_size + UINT64_C(0x60000000);
    MEMORY_BASIC_INFORMATION info;
    for (uintptr_t address = lo; address < hi && VirtualQuery((void *)address, &info, sizeof(info));
         address = (uintptr_t)info.BaseAddress + info.RegionSize) {
        if (info.State != MEM_FREE) continue;
        const uintptr_t start = ((uintptr_t)info.BaseAddress + 0xffff) & ~(uintptr_t)0xffff;
        if (start + bytes > (uintptr_t)info.BaseAddress + info.RegionSize) continue;
        void *p = VirtualAlloc((void *)start, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (p) return p;
    }
    return NULL;
}
static int compare_functions(const void *a, const void *b) {
    const RzFunction *x = a, *y = b;
    return x->begin < y->begin ? -1 : x->begin > y->begin;
}
static int compare_stubs(const void *a, const void *b) {
    const RzStub *x = a, *y = b;
    return x->stub < y->stub ? -1 : x->stub > y->stub;
}
static int in_stubs(uintptr_t rip) {
    for (size_t i = 0; i < rz_chunks; ++i)
        if (rip - (uintptr_t)rz_chunk[i] < RZ_CHUNK) return 1;
    return 0;
}
static int compare_u32(const void *a, const void *b) {
    const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

uint64_t runtime_redzone_install(unsigned char *image, uint64_t size, const uint64_t *segs, uint64_t nseg) {
    const char *env = getenv("GOW3_RED_ZONE");
    const char *scribble = getenv("GOW3_RED_ZONE_SCRIBBLE");
    rz_image = image;
    rz_size = size;
    rz_scribble = scribble && scribble[0] == '1';
    if (rz_scribble) puts("Red zone: test mode, the 128 bytes below rsp are overwritten after each handled fault");
    rz_ready = 1;
    /* Off: nothing changes; the test mode still lists the red-zone functions (for its counts). */
    rz_protecting = !(env && env[0] == '0');
    if (!rz_protecting && !rz_scribble) { puts("Red zone: protection off (GOW3_RED_ZONE=0)"); return 0; }
    const int list = env && !strcmp(env, "list");
    LARGE_INTEGER t0, t1, hz;
    QueryPerformanceCounter(&t0);
    if (ZYAN_FAILED(ZydisDecoderInit(&rz_decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64))) {
        puts("Red zone: protection unavailable (decoder)");
        return 0;
    }
    /* The functions: every .eh_frame_hdr table in the image (the eboot's, and the linked modules'). */
    RzFunction *functions = NULL;
    size_t count = 0, cap = 0;
    unsigned tables = 0;
    for (uint64_t s = 0; s < nseg; ++s) {
        const uint64_t base = segs[3 * s], end = segs[3 * s] + segs[3 * s + 1];
        for (uint64_t at = (base + 3) & ~UINT64_C(3); at + 12 <= end && at + 12 <= size; at += 4) {
            if (image[at] != 1 || image[at + 1] != 0x1b) continue;
            const uint32_t entries = eh_table(at, segs, nseg);
            if (!entries) continue;
            ++tables;
            for (uint32_t i = 0; i < entries; ++i) {
                const uint64_t pc = (uint64_t)((int64_t)at + rd32s(image + at + 12 + 8 * (uint64_t)i));
                const uint64_t fde = (uint64_t)((int64_t)at + rd32s(image + at + 16 + 8 * (uint64_t)i));
                uint64_t segment_end = 0;
                if (fde_begin(fde) != (int64_t)pc || !executable_at(segs, nseg, pc, &segment_end)) continue;
                const int32_t range = rd32s(image + fde + 12);
                if (range <= 0 || pc + (uint64_t)range > segment_end) continue;
                if (count == cap) {
                    cap = cap ? cap * 2 : 65536;
                    RzFunction *grown = realloc(functions, cap * sizeof(*grown));
                    if (!grown) { free(functions); puts("Red zone: protection unavailable (memory)"); return 0; }
                    functions = grown;
                }
                functions[count++] = (RzFunction){pc, pc + (uint64_t)range, 0, 0, 0, 0};
            }
            at += 12 + (uint64_t)entries * 8 - 4;
        }
    }
    if (!count) { puts("Red zone: protection unavailable (no .eh_frame_hdr function table found)"); return 0; }
    qsort(functions, count, sizeof(*functions), compare_functions);
    rz_code_end = 0;
    for (size_t i = 0; i < count; ++i) if (functions[i].end > rz_code_end) rz_code_end = functions[i].end;
    rz_targets = calloc((size_t)(rz_code_end / 8 + 1), 1);
    rz_starts = calloc((size_t)(rz_code_end / 8 + 1), 1);
    if (!rz_targets || !rz_starts) { free(functions); free(rz_targets); free(rz_starts); puts("Red zone: protection unavailable (memory)"); return 0; }
    for (size_t i = 0; i < count; ++i) bit_set(rz_starts, functions[i].begin);
    for (size_t i = 0; i < count; ++i) scan_function(&functions[i]);
    uint64_t red = 0, hooked = 0, bad = 0, indirect = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!functions[i].red) continue;
        ++red;
        if (functions[i].bad) ++bad;
        else if (functions[i].hooked) ++hooked;
        else if (functions[i].indirect) ++indirect;
    }
    if (rz_scribble && red) {
        rz_red = calloc((size_t)red, sizeof(*rz_red));
        rz_red_seen = calloc((size_t)red, sizeof(*rz_red_seen));
        for (size_t i = 0; rz_red && rz_red_seen && i < count; ++i)
            if (functions[i].red) rz_red[rz_red_count++] = (RzRange){(uint32_t)functions[i].begin, (uint32_t)functions[i].end};
    }
    if (!rz_protecting) {
        printf("Red zone: protection off (GOW3_RED_ZONE=0); %" PRIu64 " of %zu functions keep data below rsp (test counts)\n", red, count);
        free(rz_targets); free(rz_starts); rz_targets = rz_starts = NULL;
        free(functions);
        return 0;
    }
    uint64_t moved = 0, skipped = 0, groups = 0, protected_functions = 0;
    for (size_t i = 0; i < count; ++i) {
        const RzFunction *f = &functions[i];
        if (!f->red) continue;
        if (f->bad) {
            if (list) printf("Red zone: function %#" PRIx64 " (+%#" PRIx64 ") undecodable, left as it is\n", f->begin, f->end - f->begin);
            continue;
        }
        const uint64_t before = groups;
        protect_function(f, list, &moved, &skipped, &groups);
        if (groups != before) ++protected_functions;
    }
    for (size_t i = 0; i < rz_chunks; ++i) {
        DWORD old;
        VirtualProtect(rz_chunk[i], RZ_CHUNK, PAGE_EXECUTE_READ, &old);
        FlushInstructionCache(GetCurrentProcess(), rz_chunk[i], RZ_CHUNK);
    }
    if (rz_no_space) puts("Red zone: stub space within jump range of the image ran out (the rest stays in place)");
    if (rz_skipped_count) {
        qsort(rz_skipped, rz_skipped_count, sizeof(*rz_skipped), compare_u32);
        rz_warned = calloc(rz_skipped_count, sizeof(*rz_warned));
    }
    if (rz_stub_count) {
        qsort(rz_stubs, rz_stub_count, sizeof(*rz_stubs), compare_stubs);
        rz_stub_seen = calloc(rz_stub_count, sizeof(*rz_stub_seen));
    }
    free(rz_targets); free(rz_starts); rz_targets = rz_starts = NULL;
    free(functions);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&hz);
    printf("Red zone: %u tables, %zu functions scanned, %" PRIu64 " keep data below rsp, %" PRIu64 " protected "
           "(left alone: %" PRIu64 " hooked, %" PRIu64 " with an indirect jump, %" PRIu64 " undecodable); %" PRIu64
           " accesses moved in %" PRIu64 " stubs (%zu KiB in %zu blocks), %" PRIu64 " left in place; %.0f ms\n",
           tables, count, red, protected_functions, hooked, indirect, bad, moved, groups, rz_stub_bytes / 1024, rz_chunks, skipped,
           (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)hz.QuadPart);
    return moved;
}

/* Called by the fault handler after a GPU-tracking fault was handled, before the thread resumes. */
void runtime_redzone_after_fault(void *context) {
    if (!rz_ready) return;
    CONTEXT *c = context;
    const uintptr_t rip = (uintptr_t)c->Rip;
    const int guest = rip - (uintptr_t)rz_image < rz_size;
    const int stub = !guest && in_stubs(rip);
    if (guest && rz_warned) {
        const uint64_t offset = rip - (uintptr_t)rz_image;
        size_t lo = 0, hi = rz_skipped_count;
        while (lo < hi) {
            const size_t mid = lo + (hi - lo) / 2;
            if (rz_skipped[mid] < offset) lo = mid + 1; else hi = mid;
        }
        if (lo < rz_skipped_count && rz_skipped[lo] == offset && !InterlockedExchange(&rz_warned[lo], 1)) {
            char line[128];
            const int n = snprintf(line, sizeof(line), "Red zone: fault at an access left in place, guest offset %#llx\n",
                                   (unsigned long long)offset);
            DWORD written;
            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line, (DWORD)n, &written, NULL);
        }
    }
    if (rz_scribble && (guest || stub)) {
        unsigned char *below = (unsigned char *)(uintptr_t)(c->Rsp - 128);
        const uintptr_t frame = (uintptr_t)c;
        /* (Not over the exception's own context, should it lie there.) */
        if (frame + sizeof(*c) <= (uintptr_t)below || frame >= (uintptr_t)below + 128) memset(below, 0xcc, 128);
        /* Counts, and the first fault in each stub / red-zone function (at most 100 lines). */
        const LONG total = InterlockedIncrement(&rz_scribbles);
        char line[192];
        int n = 0;
        if (stub) {
            InterlockedIncrement(&rz_in_stub);
            size_t lo = 0, hi = rz_stub_count;
            while (lo < hi) {
                const size_t mid = lo + (hi - lo) / 2;
                if (rz_stubs[mid].stub <= rip) lo = mid + 1; else hi = mid;
            }
            if (lo && rz_stub_seen && !InterlockedExchange(&rz_stub_seen[lo - 1], 1) && InterlockedIncrement(&rz_prints) <= 100)
                n = snprintf(line, sizeof(line), "Red zone test: fault in the stub for guest offset %#x (access moved, red zone kept)\n",
                             (unsigned)rz_stubs[lo - 1].site);
        } else if (rz_red_count) {
            const uint32_t at = (uint32_t)(rip - (uintptr_t)rz_image);
            size_t lo = 0, hi = rz_red_count;
            while (lo < hi) {
                const size_t mid = lo + (hi - lo) / 2;
                if (rz_red[mid].begin <= at) lo = mid + 1; else hi = mid;
            }
            if (lo && at < rz_red[lo - 1].end) {
                InterlockedIncrement(&rz_in_red);
                if (!InterlockedExchange(&rz_red_seen[lo - 1], 1) && InterlockedIncrement(&rz_prints) <= 100)
                    n = snprintf(line, sizeof(line), "Red zone test: fault inside red-zone function %#x at guest offset %#x (%s)\n",
                                 (unsigned)rz_red[lo - 1].begin, (unsigned)at, rz_protecting ? "access not moved" : "protection off");
            }
        }
        DWORD written;
        if (n > 0) WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line, (DWORD)n, &written, NULL);
        const LONG64 now = (LONG64)GetTickCount64(), last = rz_last_report;
        if (now - last >= 30000 && InterlockedCompareExchange64(&rz_last_report, now, last) == last) {
            n = snprintf(line, sizeof(line), "Red zone test: %ld faults scribbled, %ld in red-zone functions, %ld in stubs\n",
                         (long)total, (long)rz_in_red, (long)rz_in_stub);
            if (n > 0) WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line, (DWORD)n, &written, NULL);
        }
    }
}
#else
uint64_t runtime_redzone_install(unsigned char *image, uint64_t size, const uint64_t *segments, uint64_t count) {
    /* Signal frames skip the 128-byte red zone on Linux: nothing to do. */
    (void)image; (void)size; (void)segments; (void)count;
    return 0;
}
void runtime_redzone_after_fault(void *context) { (void)context; }
#endif
