/* Instructions older CPUs lack (gow3 1 Nov 2026; Windows; GitHub #154, #299).
 *
 * The games' code was built for the PS4's Jaguar CPU and uses MOVBE, BMI1 (ANDN, BEXTR, BLSI,
 * BLSMSK, BLSR, TZCNT), LZCNT and F16C (VCVTPH2PS). Intel CPUs before Haswell and AMD FX lack some
 * of them: Ivy Bridge MOVBE, BMI1 and LZCNT; Sandy Bridge also F16C; FX (Piledriver) MOVBE. The
 * runtime's own code needs none of them.
 *
 * On such a CPU, MOVBE, BMI1 and F16C instructions raise #UD, and runtime_isa_emulate (called by
 * probe.c's vectored handler) carries them out on the thread's context and resumes after them.
 * LZCNT and TZCNT do not fault there: the CPU ignores their F3 prefix and runs BSR / BSF, which
 * give other results. At load time their F3 prefix becomes F0 (LOCK, same length): LOCK BSR / BSF
 * raise #UD on every CPU, and the handler runs them as LZCNT / TZCNT (real code never holds a
 * LOCK BSR / BSF). runtime_redzone.c applies this to the game's functions after its own pass
 * (runtime_isa_convert_function) and to the copies it moves into its stubs (runtime_isa_fix_moved),
 * and in functions that keep data below rsp it moves these instructions into its stubs as it does
 * faulting accesses: the exception frame of the #UD then lands below the red zone.
 *
 * GOW3_ISA_COMPAT=0: off (the CPU check then stops the game, as before). =trap (tests, on a CPU
 * that has them all): every such instruction is made to fault (LZCNT/TZCNT as above, the others
 * replaced by ud2, their bytes kept in a table) so the handler runs each one. Every 30 s a line
 * gives the counts per kind and the busiest sites. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

int runtime_isa_active(void);
int runtime_isa_traps(const void *instruction);
void runtime_isa_fix_moved(unsigned char *code, unsigned length);
void runtime_isa_convert_function(unsigned char *image, uint64_t begin, uint64_t end);
void runtime_isa_finish(void);
int runtime_isa_emulate(void *context);
int runtime_isa_configure(uint32_t leaf1_ecx, uint32_t leaf7_ebx, uint32_t ext1_ecx, char *missing, size_t size);

#ifdef _WIN32
#include <windows.h>
#include <Zydis/Zydis.h>

enum { K_LZCNT, K_TZCNT, K_MOVBE, K_ANDN, K_BEXTR, K_BLS, K_F16C, K_COUNT };
static const char *const kind_names[K_COUNT] = {"lzcnt", "tzcnt", "movbe", "andn", "bextr", "blsi/blsmsk/blsr", "vcvtph2ps"};

static int isa_mode;                 /* 0 off, 1 the CPU lacks some, 2 trap (test) */
static int need[K_COUNT];            /* which kinds this CPU lacks (all of them in trap mode) */
static ZydisDecoder isa_decoder;
static volatile LONG isa_hits[K_COUNT];
static volatile LONG isa_exception_hits;   /* the part of isa_hits that came through the exception handler */
static volatile LONG64 isa_last_report;
static uint64_t isa_converted[K_COUNT];

/* Trap mode: the original bytes of each instruction replaced by ud2, by address (sorted). */
typedef struct { uintptr_t address; uint8_t length; uint8_t bytes[15]; } IsaTrap;
static IsaTrap *isa_traps;
static size_t isa_trap_count, isa_trap_cap;
static int isa_traps_sorted;

/* Busiest sites (a small lock-free table: address -> count). */
#define ISA_SITES 4096
static volatile LONG64 isa_site_key[ISA_SITES];
static volatile LONG isa_site_hits[ISA_SITES];

static int kind_of(const ZydisDecodedInstruction *in) {
    switch (in->mnemonic) {
    case ZYDIS_MNEMONIC_LZCNT: return K_LZCNT;
    case ZYDIS_MNEMONIC_TZCNT: return K_TZCNT;
    case ZYDIS_MNEMONIC_MOVBE: return K_MOVBE;
    case ZYDIS_MNEMONIC_ANDN: return K_ANDN;
    case ZYDIS_MNEMONIC_BEXTR: return K_BEXTR;
    case ZYDIS_MNEMONIC_BLSI: case ZYDIS_MNEMONIC_BLSMSK: case ZYDIS_MNEMONIC_BLSR: return K_BLS;
    case ZYDIS_MNEMONIC_VCVTPH2PS: return in->attributes & ZYDIS_ATTRIB_HAS_VEX ? K_F16C : -1;
    default: return -1;
    }
}

int runtime_isa_configure(uint32_t leaf1_ecx, uint32_t leaf7_ebx, uint32_t ext1_ecx, char *missing, size_t size) {
    const char *env = getenv("GOW3_ISA_COMPAT");
    if (missing && size) missing[0] = 0;
    if (env && env[0] == '0') return 0;
    /* trap: every kind; trap:ivy = what Ivy Bridge lacks (MOVBE, BMI1, LZCNT), trap:sandy = also
     * F16C, trap:fx = AMD FX (MOVBE): tests of one CPU family on a CPU that has them all. */
    const int trap = env && !strncmp(env, "trap", 4);
    const char *profile = trap && env[4] == ':' ? env + 5 : "";
    const int fx = !strcmp(profile, "fx"), ivy = !strcmp(profile, "ivy");
    need[K_LZCNT] = (trap && !fx) || !(ext1_ecx & (1u << 5));
    need[K_TZCNT] = need[K_ANDN] = need[K_BEXTR] = need[K_BLS] = (trap && !fx) || !(leaf7_ebx & (1u << 3));
    need[K_MOVBE] = trap || !(leaf1_ecx & (1u << 22));
    need[K_F16C] = (trap && !fx && !ivy) || !(leaf1_ecx & (1u << 29));
    int any = 0;
    for (int k = 0; k < K_COUNT; ++k) any |= need[k];
    if (!any) return 0;
    if (ZYAN_FAILED(ZydisDecoderInit(&isa_decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64))) return 0;
    isa_mode = trap ? 2 : 1;
    if (missing && size) {
        size_t used = 0;
        static const char *const sets[] = {"LZCNT", "BMI1", "MOVBE", "F16C"};
        const int lacks[] = {need[K_LZCNT], need[K_ANDN], need[K_MOVBE], need[K_F16C]};
        for (int i = 0; i < 4 && used + 8 < size; ++i)
            if (lacks[i]) used += (size_t)snprintf(missing + used, size - used, "%s%s", used ? ", " : "", sets[i]);
    }
    return isa_mode;
}

int runtime_isa_active(void) { return isa_mode != 0; }

static int decode_bytes(const unsigned char *bytes, size_t length, ZydisDecodedInstruction *in, ZydisDecodedOperand *ops) {
    return ZYAN_SUCCESS(ZydisDecoderDecodeFull(&isa_decoder, bytes, length, in, ops));
}

/* The legacy prefixes and REX of an instruction; *opcode = the offset after them. */
static unsigned prefix_end(const unsigned char *code, unsigned length) {
    unsigned i = 0;
    while (i < length && (code[i] == 0x66 || code[i] == 0x67 || code[i] == 0xf0 || code[i] == 0xf2 || code[i] == 0xf3 ||
                          code[i] == 0x2e || code[i] == 0x36 || code[i] == 0x3e || code[i] == 0x26 || code[i] == 0x64 ||
                          code[i] == 0x65))
        ++i;
    if (i < length && (code[i] & 0xf0) == 0x40) ++i;
    return i;
}
/* LZCNT/TZCNT (F3 .. 0F BD/BC) <-> their LOCK form (F0 .. 0F BD/BC). Returns 1 if changed. */
static int swap_count_prefix(unsigned char *code, unsigned length, unsigned char from, unsigned char to) {
    const unsigned op = prefix_end(code, length);
    if (op + 2 > length || code[op] != 0x0f || (code[op + 1] != 0xbd && code[op + 1] != 0xbc)) return 0;
    for (unsigned i = 0; i < op; ++i)
        if (code[i] == from) { code[i] = to; return 1; }
    return 0;
}

int runtime_isa_traps(const void *instruction) {
    if (!isa_mode) return 0;
    const int k = kind_of((const ZydisDecodedInstruction *)instruction);
    return k >= 0 && need[k];
}

static void add_trap(uintptr_t address, const unsigned char *bytes, unsigned length) {
    if (isa_trap_count == isa_trap_cap) {
        const size_t cap = isa_trap_cap ? isa_trap_cap * 2 : 4096;
        IsaTrap *grown = realloc(isa_traps, cap * sizeof(*grown));
        if (!grown) return;
        isa_traps = grown;
        isa_trap_cap = cap;
    }
    IsaTrap *t = &isa_traps[isa_trap_count++];
    t->address = address;
    t->length = (uint8_t)length;
    memcpy(t->bytes, bytes, length);
    isa_traps_sorted = 0;
}
/* LZCNT / TZCNT of 5 bytes or more become a jmp to their native sequence (runtime_isa_emit_count)
 * in stub space next to the image, which jumps back: no exception. GOW3_ISA_FAST=0: traps only. */
unsigned char *runtime_near_image_alloc(size_t bytes, int executable);   /* probe.c */
unsigned runtime_isa_emit_count(unsigned char *out, const unsigned char *instruction, unsigned length, uintptr_t at);
#define ISA_FAST_CHUNK ((size_t)64 << 10)
static unsigned char *isa_fast_chunks[16];
static size_t isa_fast_chunk_count, isa_fast_used;
static int isa_fast_off = -1;
static uint64_t isa_fast[K_COUNT], isa_native[K_COUNT];
static int fits32(int64_t v) { return v >= INT32_MIN && v <= INT32_MAX; }
static int make_fast(unsigned char *code, const ZydisDecodedInstruction *in, int k) {
    if (isa_fast_off < 0) { const char *env = getenv("GOW3_ISA_FAST"); isa_fast_off = env && env[0] == '0'; }
    if (isa_fast_off || in->length < 5) return 0;
    if (!isa_fast_chunk_count || isa_fast_used + 96 > ISA_FAST_CHUNK) {
        unsigned char *chunk = isa_fast_chunk_count < 16 ? runtime_near_image_alloc(ISA_FAST_CHUNK, 0) : NULL;
        if (!chunk) { isa_fast_off = 1; return 0; }
        isa_fast_chunks[isa_fast_chunk_count++] = chunk;
        isa_fast_used = 0;
    }
    unsigned char *stub = isa_fast_chunks[isa_fast_chunk_count - 1] + isa_fast_used;
    const unsigned n = runtime_isa_emit_count(stub, code, in->length, (uintptr_t)code);
    if (!n) return 0;
    const int64_t back = (int64_t)(uintptr_t)(code + in->length) - (int64_t)(uintptr_t)(stub + n + 5);
    const int64_t there = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(code + 5);
    if (!fits32(back) || !fits32(there)) return 0;
    const int32_t back32 = (int32_t)back, there32 = (int32_t)there;
    stub[n] = 0xe9;
    memcpy(stub + n + 1, &back32, 4);
    isa_fast_used += (n + 5 + 15) & ~(size_t)15;
    code[0] = 0xe9;
    memcpy(code + 1, &there32, 4);
    memset(code + 5, 0xcc, in->length - 5u);   /* inside the old instruction: no branch lands there */
    ++isa_fast[k];
    return 1;
}

/* gow3 1 Nov 2026: ANDN, BEXTR and 32/64-bit MOVBE as native sequences, no call (the games run
 * BEXTR up to ~100,000 times a frame). Built with Zydis's encoder at the stub's own address
 * (rip-relative operands re-aimed), exact results and the flags each defines:
 *   ANDN d, a, b : mov d, a ; not d ; and d, b             (and: SF ZF, CF = OF = 0)
 *   BEXTR d, s, c: s >> start, masked to len bits, through rcx and a scratch register saved below
 *                  the red zone; ends with test (ZF, CF = OF = 0)
 *   MOVBE r, m   : mov r, m ; bswap r          MOVBE m, r : bswap r ; mov m, r ; bswap r
 * Returns the length written, 0 to fall back to the call stub (rsp-based or 32-bit addresses,
 * fs/gs, 16-bit forms, a destination inside the source address, ...). */
typedef struct { unsigned char *p; int ok; } Emit;
static void put(Emit *e, ZydisEncoderRequest *r) {
    if (!e->ok) return;
    ZyanUSize length = ZYDIS_MAX_INSTRUCTION_LENGTH;
    if (ZYAN_FAILED(ZydisEncoderEncodeInstructionAbsolute(r, e->p, &length, (ZyanU64)(uintptr_t)e->p))) { e->ok = 0; return; }
    e->p += length;
}
static void put_bytes(Emit *e, const void *bytes, size_t n) { if (e->ok) { memcpy(e->p, bytes, n); e->p += n; } }
static ZydisRegister sized(ZydisRegister r, unsigned bits) {
    const ZydisRegister big = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, r);
    return ZydisRegisterEncode(bits == 64 ? ZYDIS_REGCLASS_GPR64 : bits == 32 ? ZYDIS_REGCLASS_GPR32 : ZYDIS_REGCLASS_GPR16,
                               (ZyanU8)ZydisRegisterGetId(big));
}
static ZydisEncoderRequest req(ZydisMnemonic m, unsigned count) {
    ZydisEncoderRequest r;
    memset(&r, 0, sizeof(r));
    r.machine_mode = ZYDIS_MACHINE_MODE_LONG_64;
    r.mnemonic = m;
    r.operand_count = (ZyanU8)count;
    return r;
}
static void reg_op(ZydisEncoderOperand *o, ZydisRegister reg) { o->type = ZYDIS_OPERAND_TYPE_REGISTER; o->reg.value = reg; }
static void imm_op(ZydisEncoderOperand *o, int64_t v) { o->type = ZYDIS_OPERAND_TYPE_IMMEDIATE; o->imm.s = v; }
/* The original instruction's memory operand (rip-relative made absolute), or 0. */
static int mem_op(ZydisEncoderOperand *o, const ZydisDecodedInstruction *in, const ZydisDecodedOperand *d, uintptr_t at, unsigned bytes) {
    if (d->mem.segment == ZYDIS_REGISTER_FS || d->mem.segment == ZYDIS_REGISTER_GS || in->address_width != 64 ||
        d->mem.base == ZYDIS_REGISTER_RSP || d->mem.index == ZYDIS_REGISTER_RSP)
        return 0;
    o->type = ZYDIS_OPERAND_TYPE_MEMORY;
    o->mem.base = d->mem.base;
    o->mem.index = d->mem.index;
    o->mem.scale = d->mem.index != ZYDIS_REGISTER_NONE ? d->mem.scale : 0;
    o->mem.displacement = d->mem.base == ZYDIS_REGISTER_RIP ? (ZyanI64)(at + in->length) + d->mem.disp.value : d->mem.disp.value;
    o->mem.size = (ZyanU16)bytes;
    return 1;
}
/* Whether operand d (register or memory) reads register r (any width). */
static int uses(const ZydisDecodedOperand *d, ZydisRegister r) {
    const ZydisRegister big = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, r);
    if (d->type == ZYDIS_OPERAND_TYPE_REGISTER) return ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, d->reg.value) == big;
    if (d->type == ZYDIS_OPERAND_TYPE_MEMORY)
        return (d->mem.base != ZYDIS_REGISTER_NONE && ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, d->mem.base) == big) ||
               (d->mem.index != ZYDIS_REGISTER_NONE && ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, d->mem.index) == big);
    return 0;
}
/* A 64-bit scratch register none of the operands use, other than `avoid`. */
static ZydisRegister scratch(const ZydisDecodedOperand *ops, unsigned count, ZydisRegister avoid) {
    static const ZydisRegister pool[] = {ZYDIS_REGISTER_R11, ZYDIS_REGISTER_R10, ZYDIS_REGISTER_R9, ZYDIS_REGISTER_R8,
                                         ZYDIS_REGISTER_RSI, ZYDIS_REGISTER_RDI, ZYDIS_REGISTER_RBX, ZYDIS_REGISTER_RAX,
                                         ZYDIS_REGISTER_RDX};
    for (size_t i = 0; i < sizeof(pool) / sizeof(*pool); ++i) {
        int used = pool[i] == avoid;
        for (unsigned k = 0; k < count && !used; ++k) used = uses(&ops[k], pool[i]);
        if (!used) return pool[i];
    }
    return ZYDIS_REGISTER_NONE;
}
static const unsigned char rsp_down[] = {0x48, 0x8d, 0x64, 0x24, 0x80};                   /* lea rsp, [rsp-128] */
static const unsigned char rsp_up[] = {0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00};   /* lea rsp, [rsp+128] */
static void push_pop(Emit *e, ZydisMnemonic m, ZydisRegister r) {
    ZydisEncoderRequest q = req(m, 1);
    reg_op(&q.operands[0], r);
    put(e, &q);
}
/* mov / not / and / bswap ... with register and/or source operands. */
static void op2(Emit *e, ZydisMnemonic m, ZydisRegister d, const ZydisEncoderOperand *source) {
    ZydisEncoderRequest q = req(m, source ? 2 : 1);
    reg_op(&q.operands[0], d);
    if (source) q.operands[1] = *source;
    put(e, &q);
}
static unsigned emit_native(unsigned char *out, const ZydisDecodedInstruction *in, const ZydisDecodedOperand *ops, uintptr_t at) {
    Emit e = {out, 1};
    const unsigned width = ops[0].size;
    if (width != 32 && width != 64) return 0;
    /* The sequences move rsp (red zone, scratch saves) before they read their registers: an rsp
     * operand stays with the call stub (which reads the guest's rsp) or the exception handler. */
    for (unsigned k = 0; k < in->operand_count_visible; ++k)
        if (ops[k].type == ZYDIS_OPERAND_TYPE_REGISTER &&
            ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, ops[k].reg.value) == ZYDIS_REGISTER_RSP)
            return 0;
    if (in->mnemonic == ZYDIS_MNEMONIC_ANDN) {
        const ZydisRegister d = sized(ops[0].reg.value, width), a = sized(ops[1].reg.value, width);
        ZydisEncoderOperand b;
        memset(&b, 0, sizeof(b));
        if (ops[2].type == ZYDIS_OPERAND_TYPE_REGISTER) reg_op(&b, sized(ops[2].reg.value, width));
        else if (!mem_op(&b, in, &ops[2], at, width / 8u)) return 0;
        if (!uses(&ops[2], d)) {
            ZydisEncoderOperand source;
            memset(&source, 0, sizeof(source));
            reg_op(&source, a);
            if (d != a) op2(&e, ZYDIS_MNEMONIC_MOV, d, &source);
            op2(&e, ZYDIS_MNEMONIC_NOT, d, NULL);
            op2(&e, ZYDIS_MNEMONIC_AND, d, &b);
        } else {
            const ZydisRegister t = scratch(ops, 3, ZYDIS_REGISTER_NONE);
            if (t == ZYDIS_REGISTER_NONE) return 0;
            const ZydisRegister tw = sized(t, width);
            ZydisEncoderOperand source;
            memset(&source, 0, sizeof(source));
            put_bytes(&e, rsp_down, sizeof(rsp_down));
            push_pop(&e, ZYDIS_MNEMONIC_PUSH, t);
            reg_op(&source, a);
            op2(&e, ZYDIS_MNEMONIC_MOV, tw, &source);
            op2(&e, ZYDIS_MNEMONIC_NOT, tw, NULL);
            op2(&e, ZYDIS_MNEMONIC_AND, tw, &b);
            reg_op(&source, tw);
            op2(&e, ZYDIS_MNEMONIC_MOV, d, &source);
            push_pop(&e, ZYDIS_MNEMONIC_POP, t);
            put_bytes(&e, rsp_up, sizeof(rsp_up));
        }
        return e.ok ? (unsigned)(e.p - out) : 0;
    }
    if (in->mnemonic == ZYDIS_MNEMONIC_MOVBE) {
        if (ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {               /* load */
            const ZydisRegister d = sized(ops[0].reg.value, width);
            ZydisEncoderOperand m;
            memset(&m, 0, sizeof(m));
            if (!mem_op(&m, in, &ops[1], at, width / 8u)) return 0;
            op2(&e, ZYDIS_MNEMONIC_MOV, d, &m);
            op2(&e, ZYDIS_MNEMONIC_BSWAP, d, NULL);
        } else if (width == 64) {                                       /* store: bswap there and back */
            const ZydisRegister s = sized(ops[1].reg.value, width);
            if (uses(&ops[0], s)) return 0;                             /* the address would change */
            ZydisEncoderRequest q = req(ZYDIS_MNEMONIC_MOV, 2);
            if (!mem_op(&q.operands[0], in, &ops[0], at, width / 8u)) return 0;
            reg_op(&q.operands[1], s);
            op2(&e, ZYDIS_MNEMONIC_BSWAP, s, NULL);
            put(&e, &q);
            op2(&e, ZYDIS_MNEMONIC_BSWAP, s, NULL);
        } else {                                                        /* 32-bit store: a 32-bit bswap would clear */
            const ZydisRegister t = scratch(ops, 2, ZYDIS_REGISTER_NONE);   /* the source's upper half: a copy */
            if (t == ZYDIS_REGISTER_NONE) return 0;
            ZydisEncoderRequest q = req(ZYDIS_MNEMONIC_MOV, 2);
            if (!mem_op(&q.operands[0], in, &ops[0], at, 4)) return 0;
            reg_op(&q.operands[1], sized(t, 32));
            ZydisEncoderOperand source;
            memset(&source, 0, sizeof(source));
            reg_op(&source, sized(ops[1].reg.value, 32));
            put_bytes(&e, rsp_down, sizeof(rsp_down));
            push_pop(&e, ZYDIS_MNEMONIC_PUSH, t);
            op2(&e, ZYDIS_MNEMONIC_MOV, sized(t, 32), &source);
            op2(&e, ZYDIS_MNEMONIC_BSWAP, sized(t, 32), NULL);
            put(&e, &q);
            push_pop(&e, ZYDIS_MNEMONIC_POP, t);
            put_bytes(&e, rsp_up, sizeof(rsp_up));
        }
        return e.ok ? (unsigned)(e.p - out) : 0;
    }
    if (in->mnemonic == ZYDIS_MNEMONIC_BLSI || in->mnemonic == ZYDIS_MNEMONIC_BLSMSK ||
        in->mnemonic == ZYDIS_MNEMONIC_BLSR) {
        /* d (VEX.vvvv) from s (r/m): s into a scratch register t first (s may use d), then
         *   s == 0: BLSI d = 0, CF = 0; BLSMSK d = all ones, CF = 1; BLSR d = 0, CF = 1
         *   else  : BLSI d = -s & s, CF = 1; BLSMSK d = (s - 1) ^ s; BLSR d = (s - 1) & s
         * with ZF and SF from the result and OF = 0 (BLSMSK: ZF = 0). */
        const ZydisRegister t = scratch(ops, 2, ZYDIS_REGISTER_NONE);
        if (t == ZYDIS_REGISTER_NONE) return 0;
        const ZydisRegister tw = sized(t, width), d = sized(ops[0].reg.value, width);
        ZydisEncoderOperand s, tsrc, one;
        memset(&s, 0, sizeof(s));
        memset(&tsrc, 0, sizeof(tsrc));
        memset(&one, 0, sizeof(one));
        if (ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER) reg_op(&s, sized(ops[1].reg.value, width));
        else if (!mem_op(&s, in, &ops[1], at, width / 8u)) return 0;
        reg_op(&tsrc, tw);
        imm_op(&one, 1);
        put_bytes(&e, rsp_down, sizeof(rsp_down));
        push_pop(&e, ZYDIS_MNEMONIC_PUSH, t);
        op2(&e, ZYDIS_MNEMONIC_MOV, tw, &s);
        op2(&e, ZYDIS_MNEMONIC_TEST, tw, &tsrc);
        unsigned char *jz_zero = e.p;  put_bytes(&e, "\x74\x00", 2);
        op2(&e, ZYDIS_MNEMONIC_MOV, d, &tsrc);
        if (in->mnemonic == ZYDIS_MNEMONIC_BLSI) {
            op2(&e, ZYDIS_MNEMONIC_NEG, d, NULL);
            op2(&e, ZYDIS_MNEMONIC_AND, d, &tsrc);                  /* ZF SF; CF = OF = 0 */
            put_bytes(&e, "\xf9", 1);                               /* stc: s != 0 */
        } else {
            op2(&e, ZYDIS_MNEMONIC_SUB, d, &one);
            op2(&e, in->mnemonic == ZYDIS_MNEMONIC_BLSR ? ZYDIS_MNEMONIC_AND : ZYDIS_MNEMONIC_XOR, d, &tsrc);
        }
        unsigned char *jmp_done = e.p; put_bytes(&e, "\xeb\x00", 2);
        unsigned char *zero = e.p;
        if (in->mnemonic == ZYDIS_MNEMONIC_BLSMSK) {
            ZydisEncoderOperand all, self;
            memset(&all, 0, sizeof(all));
            memset(&self, 0, sizeof(self));
            imm_op(&all, -1);
            reg_op(&self, d);
            op2(&e, ZYDIS_MNEMONIC_MOV, d, &all);
            op2(&e, ZYDIS_MNEMONIC_OR, d, &self);                    /* SF = 1, ZF = 0, CF = OF = 0 */
            put_bytes(&e, "\xf9", 1);                               /* stc */
        } else {
            ZydisEncoderOperand self;
            memset(&self, 0, sizeof(self));
            reg_op(&self, sized(d, 32));
            op2(&e, ZYDIS_MNEMONIC_XOR, sized(d, 32), &self);        /* 0: ZF = 1, SF = CF = OF = 0 */
            if (in->mnemonic == ZYDIS_MNEMONIC_BLSR) put_bytes(&e, "\xf9", 1);   /* stc */
        }
        unsigned char *done = e.p;
        push_pop(&e, ZYDIS_MNEMONIC_POP, t);
        put_bytes(&e, rsp_up, sizeof(rsp_up));
        if (!e.ok) return 0;
        const struct { unsigned char *at, *to; } jumps[] = {{jz_zero, zero}, {jmp_done, done}};
        for (size_t i = 0; i < 2; ++i) {
            const ptrdiff_t rel = jumps[i].to - (jumps[i].at + 2);
            if (rel < -128 || rel > 127) return 0;
            jumps[i].at[1] = (unsigned char)(int8_t)rel;
        }
        return (unsigned)(e.p - out);
    }
    if (in->mnemonic != ZYDIS_MNEMONIC_BEXTR) return 0;
    /* BEXTR d, s, c (s: register or memory, c: register). */
    const ZydisRegister rcx = ZYDIS_REGISTER_RCX;
    const ZydisRegister t = scratch(ops, 3, rcx);
    if (t == ZYDIS_REGISTER_NONE || ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, ops[0].reg.value) == t)
        return 0;
    const ZydisRegister tw = sized(t, width), d = sized(ops[0].reg.value, width);
    const int d_is_rcx = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, ops[0].reg.value) == rcx;
    ZydisEncoderOperand s, c32;
    memset(&s, 0, sizeof(s));
    memset(&c32, 0, sizeof(c32));
    if (ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER) reg_op(&s, sized(ops[1].reg.value, width));
    else if (!mem_op(&s, in, &ops[1], at, width / 8u)) return 0;
    reg_op(&c32, sized(ops[2].reg.value, 32));
    put_bytes(&e, rsp_down, sizeof(rsp_down));
    push_pop(&e, ZYDIS_MNEMONIC_PUSH, rcx);
    push_pop(&e, ZYDIS_MNEMONIC_PUSH, t);
    op2(&e, ZYDIS_MNEMONIC_MOV, tw, &s);                                /* t = source (before rcx changes) */
    op2(&e, ZYDIS_MNEMONIC_MOV, ZYDIS_REGISTER_ECX, &c32);              /* ecx = control */
    ZydisEncoderOperand value, cl, ch;
    memset(&value, 0, sizeof(value));
    memset(&cl, 0, sizeof(cl));
    memset(&ch, 0, sizeof(ch));
    reg_op(&cl, ZYDIS_REGISTER_CL);
    reg_op(&ch, ZYDIS_REGISTER_CH);
    imm_op(&value, width);
    op2(&e, ZYDIS_MNEMONIC_CMP, ZYDIS_REGISTER_CL, &value);
    unsigned char *jae_zero1 = e.p;  put_bytes(&e, "\x73\x00", 2);     /* start >= W: 0 */
    op2(&e, ZYDIS_MNEMONIC_SHR, tw, &cl);
    op2(&e, ZYDIS_MNEMONIC_MOVZX, ZYDIS_REGISTER_ECX, &ch);              /* ecx = length */
    op2(&e, ZYDIS_MNEMONIC_CMP, ZYDIS_REGISTER_ECX, &value);
    unsigned char *jae_nomask = e.p; put_bytes(&e, "\x73\x00", 2);     /* length >= W: no mask */
    {
        ZydisEncoderOperand ecx;
        memset(&ecx, 0, sizeof(ecx));
        reg_op(&ecx, ZYDIS_REGISTER_ECX);
        op2(&e, ZYDIS_MNEMONIC_TEST, ZYDIS_REGISTER_ECX, &ecx);
    }
    unsigned char *jz_zero2 = e.p;   put_bytes(&e, "\x74\x00", 2);     /* length 0: 0 */
    op2(&e, ZYDIS_MNEMONIC_NEG, ZYDIS_REGISTER_ECX, NULL);
    op2(&e, ZYDIS_MNEMONIC_ADD, ZYDIS_REGISTER_ECX, &value);            /* cl = W - length */
    op2(&e, ZYDIS_MNEMONIC_SHL, tw, &cl);
    op2(&e, ZYDIS_MNEMONIC_SHR, tw, &cl);
    unsigned char *nomask = e.p;
    {
        ZydisEncoderOperand self;
        memset(&self, 0, sizeof(self));
        reg_op(&self, tw);
        op2(&e, ZYDIS_MNEMONIC_TEST, tw, &self);                        /* ZF; CF = OF = 0 */
    }
    unsigned char *jmp_done = e.p;   put_bytes(&e, "\xeb\x00", 2);
    unsigned char *zero = e.p;
    {
        ZydisEncoderOperand self;
        memset(&self, 0, sizeof(self));
        reg_op(&self, sized(t, 32));
        op2(&e, ZYDIS_MNEMONIC_XOR, sized(t, 32), &self);               /* 0; ZF = 1, CF = OF = 0 */
    }
    unsigned char *done = e.p;
    if (d_is_rcx) {                                                     /* into the saved rcx, popped below */
        ZydisEncoderRequest q = req(ZYDIS_MNEMONIC_MOV, 2);
        q.operands[0].type = ZYDIS_OPERAND_TYPE_MEMORY;
        q.operands[0].mem.base = ZYDIS_REGISTER_RSP;
        q.operands[0].mem.displacement = 8;
        q.operands[0].mem.size = 8;
        reg_op(&q.operands[1], t);
        put(&e, &q);
    } else {
        ZydisEncoderOperand source;
        memset(&source, 0, sizeof(source));
        reg_op(&source, tw);
        op2(&e, ZYDIS_MNEMONIC_MOV, d, &source);
    }
    push_pop(&e, ZYDIS_MNEMONIC_POP, t);
    push_pop(&e, ZYDIS_MNEMONIC_POP, rcx);
    put_bytes(&e, rsp_up, sizeof(rsp_up));
    if (!e.ok) return 0;
    const struct { unsigned char *at, *to; } jumps[] = {{jae_zero1, zero}, {jae_nomask, nomask}, {jz_zero2, zero}, {jmp_done, done}};
    for (size_t i = 0; i < 4; ++i) {
        const ptrdiff_t rel = jumps[i].to - (jumps[i].at + 2);
        if (rel < -128 || rel > 127) return 0;
        jumps[i].at[1] = (unsigned char)(int8_t)rel;
    }
    return (unsigned)(e.p - out);
}
static int make_native(unsigned char *code, const ZydisDecodedInstruction *in, const ZydisDecodedOperand *ops, int k) {
    if (isa_fast_off < 0) { const char *env = getenv("GOW3_ISA_FAST"); isa_fast_off = env && env[0] == '0'; }
    static int native_off = -1;
    if (native_off < 0) { const char *env = getenv("GOW3_ISA_NATIVE"); native_off = env && env[0] == '0'; }
    if (isa_fast_off || native_off || in->length < 5 || (k != K_ANDN && k != K_BEXTR && k != K_MOVBE && k != K_BLS)) return 0;
    if (!isa_fast_chunk_count || isa_fast_used + 160 > ISA_FAST_CHUNK) {
        unsigned char *chunk = isa_fast_chunk_count < 16 ? runtime_near_image_alloc(ISA_FAST_CHUNK, 0) : NULL;
        if (!chunk) { isa_fast_off = 1; return 0; }
        isa_fast_chunks[isa_fast_chunk_count++] = chunk;
        isa_fast_used = 0;
    }
    unsigned char *stub = isa_fast_chunks[isa_fast_chunk_count - 1] + isa_fast_used;
    const unsigned n = emit_native(stub, in, ops, (uintptr_t)code);
    if (!n) return 0;
    const int64_t back = (int64_t)(uintptr_t)(code + in->length) - (int64_t)(uintptr_t)(stub + n + 5);
    const int64_t there = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(code + 5);
    if (!fits32(back) || !fits32(there)) return 0;
    const int32_t back32 = (int32_t)back, there32 = (int32_t)there;
    stub[n] = 0xe9;
    memcpy(stub + n + 1, &back32, 4);
    isa_fast_used += (n + 5 + 15) & ~(size_t)15;
    code[0] = 0xe9;
    memcpy(code + 1, &there32, 4);
    memset(code + 5, 0xcc, in->length - 5u);
    ++isa_fast[k];
    ++isa_native[k];
    return 1;
}

/* MOVBE / BMI1 sites of 5 bytes or more jump to a stub that keeps the red zone (rsp - 128), saves
 * the flags, the general registers and xmm0-5 (the handler is ordinary Windows x64 code), and calls
 * runtime_isa_site, which runs the same emulation as the exception handler (run) on them: no
 * exception (~1 us each). Then it restores them and jumps back. */
typedef struct {
    ZydisDecodedInstruction in;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    uintptr_t rip;
    int kind;
} IsaSite;
static volatile LONG isa_site_failures;
static int run(CONTEXT *c, const ZydisDecodedInstruction *in, const ZydisDecodedOperand *ops, uintptr_t rip);
static void note_site(uintptr_t rip);
static void report(void);
/* saved: r15 r14 r13 r12 r11 r10 r9 r8 rdi rsi rbp rbx rdx rcx rax rflags (the stub's pushes). */
void runtime_isa_site(uint64_t *saved, const IsaSite *site) {
    CONTEXT c;
    c.R15 = saved[0]; c.R14 = saved[1]; c.R13 = saved[2]; c.R12 = saved[3];
    c.R11 = saved[4]; c.R10 = saved[5]; c.R9 = saved[6];  c.R8 = saved[7];
    c.Rdi = saved[8]; c.Rsi = saved[9]; c.Rbp = saved[10]; c.Rbx = saved[11];
    c.Rdx = saved[12]; c.Rcx = saved[13]; c.Rax = saved[14];
    c.EFlags = (DWORD)saved[15];
    c.Rsp = (DWORD64)(uintptr_t)(saved + 16) + 128;   /* the guest's rsp at the instruction */
    c.Rip = site->rip;
    if (!run(&c, &site->in, site->ops, site->rip)) {
        if (InterlockedIncrement(&isa_site_failures) <= 4) {
            char line[96];
            const int n = snprintf(line, sizeof(line), "CPU emulation: instruction at %#llx not carried out\n",
                                   (unsigned long long)site->rip);
            DWORD written;
            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line, (DWORD)n, &written, NULL);
        }
        return;
    }
    saved[0] = c.R15; saved[1] = c.R14; saved[2] = c.R13; saved[3] = c.R12;
    saved[4] = c.R11; saved[5] = c.R10; saved[6] = c.R9;  saved[7] = c.R8;
    saved[8] = c.Rdi; saved[9] = c.Rsi; saved[10] = c.Rbp; saved[11] = c.Rbx;
    saved[12] = c.Rdx; saved[13] = c.Rcx; saved[14] = c.Rax;
    saved[15] = (saved[15] & ~UINT64_C(0xffffffff)) | c.EFlags;
    InterlockedIncrement(&isa_hits[site->kind]);
    note_site(site->rip);
    report();
}
static int make_fast_call(unsigned char *code, const ZydisDecodedInstruction *in, const ZydisDecodedOperand *ops, int k) {
    if (isa_fast_off < 0) { const char *env = getenv("GOW3_ISA_FAST"); isa_fast_off = env && env[0] == '0'; }
    if (isa_fast_off || in->length < 5 || k == K_F16C) return 0;
    for (unsigned i = 0; i < in->operand_count; ++i)
        if (ops[i].type == ZYDIS_OPERAND_TYPE_MEMORY &&
            (ops[i].mem.segment == ZYDIS_REGISTER_FS || ops[i].mem.segment == ZYDIS_REGISTER_GS))
            return 0;
    /* (The stub does not write rsp back: an rsp destination stays with the exception handler.) */
    if (ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
        ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, ops[0].reg.value) == ZYDIS_REGISTER_RSP)
        return 0;
    static const unsigned char head[] = {
        0x48, 0x8d, 0x64, 0x24, 0x80,                                     /* lea rsp, [rsp-128] */
        0x9c, 0xfc, 0x50, 0x51, 0x52, 0x53, 0x55, 0x56, 0x57,             /* pushfq; cld; push rax..rdi */
        0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
        0x48, 0x89, 0xe5,                                                 /* mov rbp, rsp */
        0x48, 0x83, 0xe4, 0xf0,                                           /* and rsp, -16 */
        0x48, 0x83, 0xec, 0x70,                                           /* sub rsp, 112 */
        0xf3, 0x0f, 0x7f, 0x04, 0x24,                                     /* movdqu [rsp], xmm0 */
        0xf3, 0x0f, 0x7f, 0x4c, 0x24, 0x10, 0xf3, 0x0f, 0x7f, 0x54, 0x24, 0x20,
        0xf3, 0x0f, 0x7f, 0x5c, 0x24, 0x30, 0xf3, 0x0f, 0x7f, 0x64, 0x24, 0x40,
        0xf3, 0x0f, 0x7f, 0x6c, 0x24, 0x50,                               /* .. xmm5 */
        0x48, 0x83, 0xec, 0x20,                                           /* sub rsp, 32 */
        0x48, 0x89, 0xe9,                                                 /* mov rcx, rbp */
    };
    static const unsigned char tail[] = {
        0xff, 0xd0,                                                       /* call rax */
        0x48, 0x83, 0xc4, 0x20,                                           /* add rsp, 32 */
        0xf3, 0x0f, 0x6f, 0x04, 0x24,                                     /* movdqu xmm0, [rsp] */
        0xf3, 0x0f, 0x6f, 0x4c, 0x24, 0x10, 0xf3, 0x0f, 0x6f, 0x54, 0x24, 0x20,
        0xf3, 0x0f, 0x6f, 0x5c, 0x24, 0x30, 0xf3, 0x0f, 0x6f, 0x64, 0x24, 0x40,
        0xf3, 0x0f, 0x6f, 0x6c, 0x24, 0x50,                               /* .. xmm5 */
        0x48, 0x89, 0xec,                                                 /* mov rsp, rbp */
        0x41, 0x5f, 0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c, 0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58,
        0x5f, 0x5e, 0x5d, 0x5b, 0x5a, 0x59, 0x58, 0x9d,                   /* pop rdi..rax; popfq */
        0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00,                   /* lea rsp, [rsp+128] */
    };
    const size_t size = sizeof(head) + 10 + 10 + sizeof(tail) + 5;
    if (!isa_fast_chunk_count || isa_fast_used + size + 16 > ISA_FAST_CHUNK) {
        unsigned char *chunk = isa_fast_chunk_count < 16 ? runtime_near_image_alloc(ISA_FAST_CHUNK, 0) : NULL;
        if (!chunk) { isa_fast_off = 1; return 0; }
        isa_fast_chunks[isa_fast_chunk_count++] = chunk;
        isa_fast_used = 0;
    }
    IsaSite *site = malloc(sizeof(*site));
    if (!site) return 0;
    site->in = *in;
    memcpy(site->ops, ops, sizeof(site->ops));
    site->rip = (uintptr_t)code;
    site->kind = k;
    unsigned char *stub = isa_fast_chunks[isa_fast_chunk_count - 1] + isa_fast_used, *p = stub;
    const int64_t back = (int64_t)(uintptr_t)(code + in->length) - (int64_t)(uintptr_t)(stub + size);
    const int64_t there = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(code + 5);
    if (!fits32(back) || !fits32(there)) { free(site); return 0; }
    memcpy(p, head, sizeof(head)); p += sizeof(head);
    const uint64_t record = (uint64_t)(uintptr_t)site, helper = (uint64_t)(uintptr_t)&runtime_isa_site;
    *p++ = 0x48; *p++ = 0xba; memcpy(p, &record, 8); p += 8;               /* mov rdx, site */
    *p++ = 0x48; *p++ = 0xb8; memcpy(p, &helper, 8); p += 8;               /* mov rax, runtime_isa_site */
    memcpy(p, tail, sizeof(tail)); p += sizeof(tail);
    const int32_t back32 = (int32_t)back, there32 = (int32_t)there;
    *p++ = 0xe9; memcpy(p, &back32, 4);
    isa_fast_used += (size + 15) & ~(size_t)15;
    code[0] = 0xe9;
    memcpy(code + 1, &there32, 4);
    memset(code + 5, 0xcc, in->length - 5u);
    ++isa_fast[k];
    return 1;
}

/* VCVTPH2PS (F16C, Sandy Bridge lacks it; Uncharted 1 runs it ~60,000 times a second): the stub
 * also saves ymm0-15 (Sandy Bridge has AVX), and runtime_isa_site_vector converts into the saved
 * copy of the destination, which the stub then loads back with the rest. */
static float half_to_float(uint16_t h);
static uintptr_t operand_address(CONTEXT *c, const ZydisDecodedInstruction *in, const ZydisDecodedOperand *o, uintptr_t rip);
void runtime_isa_site_vector(uint64_t *saved, const IsaSite *site, unsigned char *ymm) {
    CONTEXT c;
    c.R15 = saved[0]; c.R14 = saved[1]; c.R13 = saved[2]; c.R12 = saved[3];
    c.R11 = saved[4]; c.R10 = saved[5]; c.R9 = saved[6];  c.R8 = saved[7];
    c.Rdi = saved[8]; c.Rsi = saved[9]; c.Rbp = saved[10]; c.Rbx = saved[11];
    c.Rdx = saved[12]; c.Rcx = saved[13]; c.Rax = saved[14];
    c.Rsp = (DWORD64)(uintptr_t)(saved + 16) + 128;
    const ZydisDecodedOperand *ops = site->ops;
    const unsigned count = ops[0].size / 32u;
    uint16_t halves[8] = {0};
    if (ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY) {
        const uintptr_t address = operand_address(&c, &site->in, &ops[1], site->rip);
        if (!address) return;
        memcpy(halves, (const void *)address, count * 2u);
    } else {
        memcpy(halves, ymm + 32u * (unsigned)(ops[1].reg.value - ZYDIS_REGISTER_XMM0), count * 2u);
    }
    const unsigned dest = (unsigned)(ops[0].reg.value - (count == 8 ? ZYDIS_REGISTER_YMM0 : ZYDIS_REGISTER_XMM0));
    float floats[8] = {0};
    for (unsigned i = 0; i < count; ++i) floats[i] = half_to_float(halves[i]);
    memcpy(ymm + 32u * dest, floats, 32);              /* the xmm form clears the upper half */
    InterlockedIncrement(&isa_hits[K_F16C]);
    note_site(site->rip);
    report();
}
static int make_fast_vector(unsigned char *code, const ZydisDecodedInstruction *in, const ZydisDecodedOperand *ops) {
    if (isa_fast_off < 0) { const char *env = getenv("GOW3_ISA_FAST"); isa_fast_off = env && env[0] == '0'; }
    if (isa_fast_off || in->length < 5) return 0;
    const int source_ok = ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY
                              ? ops[1].mem.segment != ZYDIS_REGISTER_FS && ops[1].mem.segment != ZYDIS_REGISTER_GS
                              : ops[1].reg.value >= ZYDIS_REGISTER_XMM0 && ops[1].reg.value <= ZYDIS_REGISTER_XMM15;
    const ZydisRegister first = ops[0].size == 256 ? ZYDIS_REGISTER_YMM0 : ZYDIS_REGISTER_XMM0;
    if (!source_ok || ops[0].type != ZYDIS_OPERAND_TYPE_REGISTER || ops[0].reg.value < first || ops[0].reg.value > first + 15)
        return 0;
    static const unsigned char head[] = {
        0x48, 0x8d, 0x64, 0x24, 0x80,                                     /* lea rsp, [rsp-128] */
        0x9c, 0xfc, 0x50, 0x51, 0x52, 0x53, 0x55, 0x56, 0x57,             /* pushfq; cld; push rax..rdi */
        0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
        0x48, 0x89, 0xe5,                                                 /* mov rbp, rsp */
        0x48, 0x83, 0xe4, 0xe0,                                           /* and rsp, -32 */
        0x48, 0x81, 0xec, 0x00, 0x02, 0x00, 0x00,                         /* sub rsp, 512 */
    };
    static const unsigned char call[] = {
        0x48, 0x83, 0xec, 0x20,                                           /* sub rsp, 32 */
        0x48, 0x89, 0xe9,                                                 /* mov rcx, rbp */
        0x4c, 0x8d, 0x44, 0x24, 0x20,                                     /* lea r8, [rsp+32] */
    };
    static const unsigned char after_call[] = {
        0xff, 0xd0,                                                       /* call rax */
        0x48, 0x83, 0xc4, 0x20,                                           /* add rsp, 32 */
    };
    static const unsigned char tail[] = {
        0x48, 0x89, 0xec,                                                 /* mov rsp, rbp */
        0x41, 0x5f, 0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c, 0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58,
        0x5f, 0x5e, 0x5d, 0x5b, 0x5a, 0x59, 0x58, 0x9d,                   /* pop rdi..rax; popfq */
        0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00,                   /* lea rsp, [rsp+128] */
    };
    const size_t size = sizeof(head) + 16 * 9 + sizeof(call) + 20 + sizeof(after_call) + 16 * 9 + sizeof(tail) + 5;
    if (!isa_fast_chunk_count || isa_fast_used + size + 16 > ISA_FAST_CHUNK) {
        unsigned char *chunk = isa_fast_chunk_count < 16 ? runtime_near_image_alloc(ISA_FAST_CHUNK, 0) : NULL;
        if (!chunk) { isa_fast_off = 1; return 0; }
        isa_fast_chunks[isa_fast_chunk_count++] = chunk;
        isa_fast_used = 0;
    }
    IsaSite *site = malloc(sizeof(*site));
    if (!site) return 0;
    site->in = *in;
    memcpy(site->ops, ops, sizeof(site->ops));
    site->rip = (uintptr_t)code;
    site->kind = K_F16C;
    unsigned char *stub = isa_fast_chunks[isa_fast_chunk_count - 1] + isa_fast_used, *p = stub;
    const int64_t back = (int64_t)(uintptr_t)(code + in->length) - (int64_t)(uintptr_t)(stub + size);
    const int64_t there = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(code + 5);
    if (!fits32(back) || !fits32(there)) { free(site); return 0; }
    memcpy(p, head, sizeof(head)); p += sizeof(head);
    for (unsigned r = 0; r < 16; ++r) {                                   /* vmovdqu [rsp+32r], ymm_r */
        const uint32_t offset = 32u * r;
        *p++ = 0xc5; *p++ = r < 8 ? 0xfe : 0x7e; *p++ = 0x7f;
        *p++ = (unsigned char)(0x84 | ((r & 7) << 3)); *p++ = 0x24; memcpy(p, &offset, 4); p += 4;
    }
    memcpy(p, call, sizeof(call)); p += sizeof(call);
    const uint64_t record = (uint64_t)(uintptr_t)site, helper = (uint64_t)(uintptr_t)&runtime_isa_site_vector;
    *p++ = 0x48; *p++ = 0xba; memcpy(p, &record, 8); p += 8;               /* mov rdx, site */
    *p++ = 0x48; *p++ = 0xb8; memcpy(p, &helper, 8); p += 8;               /* mov rax, runtime_isa_site_vector */
    memcpy(p, after_call, sizeof(after_call)); p += sizeof(after_call);
    for (unsigned r = 0; r < 16; ++r) {                                   /* vmovdqu ymm_r, [rsp+32r] */
        const uint32_t offset = 32u * r;
        *p++ = 0xc5; *p++ = r < 8 ? 0xfe : 0x7e; *p++ = 0x6f;
        *p++ = (unsigned char)(0x84 | ((r & 7) << 3)); *p++ = 0x24; memcpy(p, &offset, 4); p += 4;
    }
    memcpy(p, tail, sizeof(tail)); p += sizeof(tail);
    const int32_t back32 = (int32_t)back, there32 = (int32_t)there;
    *p++ = 0xe9; memcpy(p, &back32, 4);
    isa_fast_used += (size + 15) & ~(size_t)15;
    code[0] = 0xe9;
    memcpy(code + 1, &there32, 4);
    memset(code + 5, 0xcc, in->length - 5u);
    ++isa_fast[K_F16C];
    return 1;
}

/* Makes the instruction at `code` (decoded as `in`) fault on this CPU (or, LZCNT / TZCNT, run
 * natively in a stub; MOVBE / BMI1 / F16C, call the emulation from a stub). */
static void make_trap(unsigned char *code, const ZydisDecodedInstruction *in, const ZydisDecodedOperand *ops) {
    const int k = kind_of(in);
    if (k < 0 || !need[k]) return;
    if (k == K_LZCNT || k == K_TZCNT) {
        if (make_fast(code, in, k)) return;
        if (swap_count_prefix(code, in->length, 0xf3, 0xf0)) ++isa_converted[k];
        return;
    }
    if (make_native(code, in, ops, k)) return;
    if (k == K_F16C ? make_fast_vector(code, in, ops) : make_fast_call(code, in, ops, k)) return;
    if (isa_mode != 2) return;   /* the others fault by themselves on a CPU without them */
    add_trap((uintptr_t)code, code, in->length);
    code[0] = 0x0f;
    code[1] = 0x0b;              /* ud2; the rest of the bytes stay (never reached) */
    ++isa_converted[k];
}

void runtime_isa_fix_moved(unsigned char *code, unsigned length) {
    if (!isa_mode) return;
    ZydisDecodedInstruction in;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    if (decode_bytes(code, length, &in, ops) && in.length == length) make_trap(code, &in, ops);
}

/* gow3 1 Nov 2026: a copy the red-zone pass moved to `code` (`length` bytes, with room for
 * RUNTIME_ISA_ROOM more): its native sequence written there in place (LZCNT/TZCNT, ANDN, BEXTR,
 * MOVBE; any length, also the 4-byte forms a jmp does not fit), else as runtime_isa_fix_moved.
 * Returns the copy's new length. */
unsigned runtime_isa_relocated(unsigned char *code, unsigned length) {
    if (!isa_mode) return length;
    ZydisDecodedInstruction in;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    unsigned char copy[16];
    if (length > 15 || !decode_bytes(code, length, &in, ops) || in.length != length) return length;
    const int k = kind_of(&in);
    if (k < 0 || !need[k]) return length;
    if (isa_fast_off < 0) { const char *env = getenv("GOW3_ISA_FAST"); isa_fast_off = env && env[0] == '0'; }
    const char *native_env = getenv("GOW3_ISA_NATIVE");
    if (!isa_fast_off && !(native_env && native_env[0] == '0')) {
        memcpy(copy, code, length);
        unsigned n = 0;
        if (k == K_LZCNT || k == K_TZCNT) n = runtime_isa_emit_count(code, copy, length, (uintptr_t)code);
        else if (k == K_ANDN || k == K_BEXTR || k == K_MOVBE || k == K_BLS) {
            ZydisDecodedInstruction in2;
            ZydisDecodedOperand ops2[ZYDIS_MAX_OPERAND_COUNT];
            if (decode_bytes(copy, length, &in2, ops2)) n = emit_native(code, &in2, ops2, (uintptr_t)code);
        }
        if (n) {
            ++isa_native[k];
            ++isa_fast[k];
            return n;
        }
        memcpy(code, copy, length);    /* a failed emit may have written part of it */
    }
    make_trap(code, &in, ops);
    return length;
}

void runtime_isa_convert_function(unsigned char *image, uint64_t begin, uint64_t end) {
    if (!isa_mode) return;
    for (uint64_t at = begin; at < end;) {
        ZydisDecodedInstruction in;
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
        if (!decode_bytes(image + at, (size_t)(end - at), &in, ops)) return;   /* undecodable: as the red-zone pass, leave the rest */
        make_trap(image + at, &in, ops);
        at += in.length;
    }
}

/* gow3 1 Nov 2026: LZCNT / TZCNT as native code, no exception: the instruction without its F3
 * prefix is BSR / BSF with the same operands, and a few instructions on the destination make the
 * LZCNT / TZCNT result and flags (CF = source was 0, ZF = result is 0; the others are undefined):
 *     bsr dst, src ; jnz 1f ; mov dst, -1 ; 1: neg dst ; add dst, W-1 ; test dst, dst ; bt dst, log2 W
 *     bsf dst, src ; jnz 1f ; mov dst, W  ; 1:                          test dst, dst ; bt dst, log2 W
 * Writes it to `out` (which runs at `out` itself; a rip-relative source is re-aimed from `at`, the
 * instruction's own address) and returns its length, 0 if the instruction is not one of them. */
static unsigned put_reg_op(unsigned char *p, unsigned width, unsigned reg, unsigned rex_r, const unsigned char *opcode,
                           unsigned opcode_length, unsigned modrm_reg_field) {
    unsigned n = 0;
    if (width == 16) p[n++] = 0x66;
    const unsigned rex = (width == 64 ? 0x08u : 0) | (reg >= 8 ? 0x01u : 0) | (rex_r ? 0x04u : 0);
    if (rex) p[n++] = (unsigned char)(0x40 | rex);
    memcpy(p + n, opcode, opcode_length);
    n += opcode_length;
    p[n++] = (unsigned char)(0xc0 | (modrm_reg_field << 3) | (reg & 7));
    return n;
}
unsigned runtime_isa_emit_count(unsigned char *out, const unsigned char *instruction, unsigned length, uintptr_t at) {
    if (!isa_mode && ZYAN_FAILED(ZydisDecoderInit(&isa_decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64))) return 0;
    ZydisDecodedInstruction in;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    if (length > 15 || !decode_bytes(instruction, length, &in, ops) || in.length != length) return 0;
    const int lz = in.mnemonic == ZYDIS_MNEMONIC_LZCNT;
    if (!lz && in.mnemonic != ZYDIS_MNEMONIC_TZCNT) return 0;
    const unsigned width = ops[0].size;
    if (ops[0].type != ZYDIS_OPERAND_TYPE_REGISTER || (width != 16 && width != 32 && width != 64)) return 0;
    const ZydisRegister enclosing = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, ops[0].reg.value);
    if (enclosing < ZYDIS_REGISTER_RAX || enclosing > ZYDIS_REGISTER_R15) return 0;
    const unsigned reg = (unsigned)(enclosing - ZYDIS_REGISTER_RAX);
    for (unsigned k = 0; k < in.operand_count_visible; ++k)   /* rsp-based: the caller may have moved rsp */
        if (ops[k].type == ZYDIS_OPERAND_TYPE_MEMORY &&
            (ops[k].mem.base == ZYDIS_REGISTER_RSP || ops[k].mem.index == ZYDIS_REGISTER_RSP))
            return 0;
    /* 1. The instruction without its F3 prefix (BSR / BSF), re-aimed if rip-relative. */
    unsigned n = 0, removed = 0;
    for (unsigned i = 0; i < length; ++i) {
        if (!removed && i < in.raw.prefix_count && instruction[i] == 0xf3) { removed = 1; continue; }
        out[n++] = instruction[i];
    }
    if (!removed) return 0;
    for (unsigned k = 0; k < in.operand_count_visible; ++k)
        if (ops[k].type == ZYDIS_OPERAND_TYPE_MEMORY && ops[k].mem.base == ZYDIS_REGISTER_RIP) {
            const int64_t target = (int64_t)at + length + in.raw.disp.value;
            const int64_t disp = target - (int64_t)((uintptr_t)out + n);
            if (disp < INT32_MIN || disp > INT32_MAX || in.raw.disp.size != 32) return 0;
            const int32_t d = (int32_t)disp;
            memcpy(out + in.raw.disp.offset - 1, &d, 4);   /* one byte earlier: the prefix is gone */
        }
    /* 2. jnz over the zero case. */
    unsigned char zero_case[12];
    unsigned z = 0;
    if (width == 16) zero_case[z++] = 0x66;
    if (reg >= 8 || width == 64) zero_case[z++] = (unsigned char)(0x40 | (width == 64 ? 0x08 : 0) | (reg >= 8 ? 0x01 : 0));
    if (width == 64) {                                  /* mov r64, imm32 (sign-extended) */
        zero_case[z++] = 0xc7;
        zero_case[z++] = (unsigned char)(0xc0 | (reg & 7));
        const int32_t value = lz ? -1 : 64;
        memcpy(zero_case + z, &value, 4);
        z += 4;
    } else {                                            /* mov r32 / r16, imm */
        zero_case[z++] = (unsigned char)(0xb8 | (reg & 7));
        const uint32_t value = lz ? 0xffffffffu : width;
        memcpy(zero_case + z, &value, width / 8u);
        z += width / 8u;
    }
    out[n++] = 0x75;                                    /* jnz rel8 */
    out[n++] = (unsigned char)z;
    memcpy(out + n, zero_case, z);
    n += z;
    /* 3. LZCNT: dst = W-1 - dst. */
    if (lz) {
        static const unsigned char neg[] = {0xf7}, add[] = {0x83};
        n += put_reg_op(out + n, width, reg, 0, neg, 1, 3);
        n += put_reg_op(out + n, width, reg, 0, add, 1, 0);
        out[n++] = (unsigned char)(width - 1);
    }
    /* 4. Flags: test dst, dst (ZF, CF = 0); bt dst, log2 W (CF = result is W; ZF kept). */
    {
        static const unsigned char test[] = {0x85}, bt[] = {0x0f, 0xba};
        n += put_reg_op(out + n, width, reg, reg >= 8, test, 1, reg & 7);
        n += put_reg_op(out + n, width, reg, 0, bt, 2, 4);
        out[n++] = (unsigned char)(width == 16 ? 4 : width == 32 ? 5 : 6);
    }
    return n;
}

static int compare_traps(const void *a, const void *b) {
    const IsaTrap *x = a, *y = b;
    return x->address < y->address ? -1 : x->address > y->address;
}
void runtime_isa_finish(void) {
    if (!isa_mode) return;
    if (isa_trap_count) qsort(isa_traps, isa_trap_count, sizeof(*isa_traps), compare_traps);
    isa_traps_sorted = 1;
    for (size_t i = 0; i < isa_fast_chunk_count; ++i) {
        DWORD old;
        VirtualProtect(isa_fast_chunks[i], ISA_FAST_CHUNK, PAGE_EXECUTE_READ, &old);
        FlushInstructionCache(GetCurrentProcess(), isa_fast_chunks[i], ISA_FAST_CHUNK);
    }
    printf("CPU emulation: %s; made to fault:", isa_mode == 2 ? "test mode (GOW3_ISA_COMPAT=trap)" : "instructions this CPU lacks run in the exception handler (GOW3_ISA_COMPAT=0: off)");
    for (int k = 0; k < K_COUNT; ++k)
        if (need[k]) printf(" %s %" PRIu64, kind_names[k], isa_converted[k]);
    printf(" (the others fault by themselves); without an exception (GOW3_ISA_FAST=0: off):");
    for (int k = 0; k < K_COUNT; ++k)
        if (need[k] && isa_fast[k]) printf(" %s %" PRIu64, kind_names[k], isa_fast[k]);
    printf(" (as native code: andn %" PRIu64 " bextr %" PRIu64 " movbe %" PRIu64 " blsi/blsmsk/blsr %" PRIu64 "; GOW3_ISA_NATIVE=0: call stubs)\n",
           isa_native[K_ANDN], isa_native[K_BEXTR], isa_native[K_MOVBE], isa_native[K_BLS]);
    fflush(stdout);
}

static const IsaTrap *find_trap(uintptr_t address) {
    if (!isa_traps_sorted || !isa_trap_count) return NULL;
    size_t lo = 0, hi = isa_trap_count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (isa_traps[mid].address < address) lo = mid + 1; else hi = mid;
    }
    return lo < isa_trap_count && isa_traps[lo].address == address ? &isa_traps[lo] : NULL;
}

/* The CONTEXT slot of a general register, and its width in bits; NULL for others. */
static DWORD64 *gpr(CONTEXT *c, ZydisRegister r, unsigned *bits) {
    *bits = ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LONG_64, r);
    switch (ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, r)) {
    case ZYDIS_REGISTER_RAX: return &c->Rax; case ZYDIS_REGISTER_RCX: return &c->Rcx;
    case ZYDIS_REGISTER_RDX: return &c->Rdx; case ZYDIS_REGISTER_RBX: return &c->Rbx;
    case ZYDIS_REGISTER_RSP: return &c->Rsp; case ZYDIS_REGISTER_RBP: return &c->Rbp;
    case ZYDIS_REGISTER_RSI: return &c->Rsi; case ZYDIS_REGISTER_RDI: return &c->Rdi;
    case ZYDIS_REGISTER_R8: return &c->R8;   case ZYDIS_REGISTER_R9: return &c->R9;
    case ZYDIS_REGISTER_R10: return &c->R10; case ZYDIS_REGISTER_R11: return &c->R11;
    case ZYDIS_REGISTER_R12: return &c->R12; case ZYDIS_REGISTER_R13: return &c->R13;
    case ZYDIS_REGISTER_R14: return &c->R14; case ZYDIS_REGISTER_R15: return &c->R15;
    default: return NULL;
    }
}
static uint64_t mask_of(unsigned bits) { return bits >= 64 ? ~UINT64_C(0) : (UINT64_C(1) << bits) - 1; }

/* The address of a memory operand; 0 if it cannot be formed here (fs/gs, vector index). */
static uintptr_t operand_address(CONTEXT *c, const ZydisDecodedInstruction *in, const ZydisDecodedOperand *o, uintptr_t rip) {
    if (o->mem.segment == ZYDIS_REGISTER_FS || o->mem.segment == ZYDIS_REGISTER_GS) return 0;
    uint64_t address = (uint64_t)o->mem.disp.value;
    unsigned bits;
    if (o->mem.base == ZYDIS_REGISTER_RIP) address += rip + in->length;
    else if (o->mem.base != ZYDIS_REGISTER_NONE) {
        DWORD64 *base = gpr(c, o->mem.base, &bits);
        if (!base) return 0;
        address += *base & mask_of(bits);
    }
    if (o->mem.index != ZYDIS_REGISTER_NONE) {
        DWORD64 *index = gpr(c, o->mem.index, &bits);
        if (!index) return 0;
        address += (*index & mask_of(bits)) * o->mem.scale;
    }
    if (in->address_width == 32) address &= 0xffffffffu;
    return (uintptr_t)address;
}
/* Reads a register or memory source operand (zero-extended); 0 if it cannot. */
static int read_source(CONTEXT *c, const ZydisDecodedInstruction *in, const ZydisDecodedOperand *o, uintptr_t rip, uint64_t *value) {
    unsigned bits;
    if (o->type == ZYDIS_OPERAND_TYPE_REGISTER) {
        DWORD64 *r = gpr(c, o->reg.value, &bits);
        if (!r) return 0;
        *value = *r & mask_of(bits);
        return 1;
    }
    if (o->type != ZYDIS_OPERAND_TYPE_MEMORY) return 0;
    const uintptr_t address = operand_address(c, in, o, rip);
    if (!address) return 0;
    uint64_t v = 0;
    memcpy(&v, (const void *)address, o->size / 8u);   /* a fault here is handled as the game's own would be */
    *value = v;
    return 1;
}
static int write_register(CONTEXT *c, ZydisRegister reg, uint64_t value) {
    unsigned bits;
    DWORD64 *r = gpr(c, reg, &bits);
    if (!r) return 0;
    if (bits == 32 || bits == 64) *r = value & mask_of(bits);          /* 32-bit writes clear the upper half */
    else *r = (*r & ~mask_of(bits)) | (value & mask_of(bits));        /* 16-bit writes keep the rest */
    return 1;
}
enum { F_CF = 1u << 0, F_PF = 1u << 2, F_AF = 1u << 4, F_ZF = 1u << 6, F_SF = 1u << 7, F_OF = 1u << 11 };
static void set_flags(CONTEXT *c, DWORD changed, DWORD values) { c->EFlags = (c->EFlags & ~changed) | (values & changed); }
static uint64_t bswap(uint64_t v, unsigned bits) {
    uint64_t r = 0;
    for (unsigned i = 0; i < bits / 8; ++i) r = (r << 8) | ((v >> (8 * i)) & 0xff);
    return r;
}
static float half_to_float(uint16_t h) {
    const uint32_t sign = (uint32_t)(h & 0x8000) << 16, exponent = (h >> 10) & 0x1f, mantissa = h & 0x3ff;
    uint32_t bits;
    if (exponent == 0x1f) bits = sign | 0x7f800000u | (mantissa << 13) | (mantissa ? 0x400000u : 0);   /* inf / NaN (made quiet, as the CPU does) */
    else if (exponent) bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
    else if (!mantissa) bits = sign;
    else {                                                                          /* subnormal half: normal float */
        int e = -1;
        uint32_t m = mantissa;
        do { ++e; m <<= 1; } while (!(m & 0x400));
        bits = sign | ((uint32_t)(112 - e) << 23) | ((m & 0x3ff) << 13);
    }
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

/* One instruction; 1 if done (registers, flags and memory updated; rip not yet moved). */
static int run(CONTEXT *c, const ZydisDecodedInstruction *in, const ZydisDecodedOperand *ops, uintptr_t rip) {
    const int k = kind_of(in);
    if (k < 0) return 0;
    const unsigned width = ops[0].size;
    uint64_t a = 0, b = 0;
    switch (k) {
    case K_LZCNT: case K_TZCNT: {
        if (!read_source(c, in, &ops[1], rip, &a)) return 0;
        unsigned n = 0;
        if (k == K_LZCNT) while (n < width && !((a >> (width - 1 - n)) & 1)) ++n;
        else while (n < width && !((a >> n) & 1)) ++n;
        if (!write_register(c, ops[0].reg.value, n)) return 0;
        set_flags(c, F_CF | F_ZF, (a == 0 ? F_CF : 0) | (n == 0 ? F_ZF : 0));
        return 1;
    }
    case K_MOVBE:
        if (ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {
            if (!read_source(c, in, &ops[1], rip, &a)) return 0;
            return write_register(c, ops[0].reg.value, bswap(a, width));
        } else {
            if (!read_source(c, in, &ops[1], rip, &a)) return 0;
            const uintptr_t address = operand_address(c, in, &ops[0], rip);
            if (!address) return 0;
            const uint64_t v = bswap(a, width);
            memcpy((void *)address, &v, width / 8u);
            return 1;
        }
    case K_ANDN: {                               /* dest = ~src1 & src2 */
        if (!read_source(c, in, &ops[1], rip, &a) || !read_source(c, in, &ops[2], rip, &b)) return 0;
        const uint64_t r = ~a & b & mask_of(width);
        if (!write_register(c, ops[0].reg.value, r)) return 0;
        set_flags(c, F_CF | F_OF | F_ZF | F_SF, (r == 0 ? F_ZF : 0) | ((r >> (width - 1)) & 1 ? F_SF : 0));
        return 1;
    }
    case K_BEXTR: {                              /* dest = src1 [start, start + len) */
        if (!read_source(c, in, &ops[1], rip, &a) || !read_source(c, in, &ops[2], rip, &b)) return 0;
        const unsigned start = (unsigned)(b & 0xff), length = (unsigned)((b >> 8) & 0xff);
        uint64_t r = start >= width ? 0 : a >> start;
        if (length < 64) r &= mask_of(length);
        r &= mask_of(width);
        if (!write_register(c, ops[0].reg.value, r)) return 0;
        set_flags(c, F_CF | F_OF | F_ZF, r == 0 ? F_ZF : 0);
        return 1;
    }
    case K_BLS: {                                /* dest (VEX.vvvv) from src (r/m) */
        if (!read_source(c, in, &ops[1], rip, &a)) return 0;
        const uint64_t m = mask_of(width);
        uint64_t r;
        DWORD flags;
        if (in->mnemonic == ZYDIS_MNEMONIC_BLSI) {
            r = (0 - a) & a & m;
            flags = (a != 0 ? F_CF : 0) | (r == 0 ? F_ZF : 0);
        } else if (in->mnemonic == ZYDIS_MNEMONIC_BLSMSK) {
            r = ((a - 1) ^ a) & m;
            flags = a == 0 ? F_CF : 0;          /* ZF cleared */
        } else {
            r = ((a - 1) & a) & m;
            flags = (a == 0 ? F_CF : 0) | (r == 0 ? F_ZF : 0);
        }
        if ((r >> (width - 1)) & 1) flags |= F_SF;
        if (!write_register(c, ops[0].reg.value, r)) return 0;
        set_flags(c, F_CF | F_OF | F_ZF | F_SF, flags);
        return 1;
    }
    case K_F16C: {                               /* vcvtph2ps xmm/ymm, xmm/m64/m128 */
        const unsigned count = ops[0].size / 32u;
        uint16_t halves[8] = {0};
        if (ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY) {
            const uintptr_t address = operand_address(c, in, &ops[1], rip);
            if (!address) return 0;
            memcpy(halves, (const void *)address, count * 2u);
        } else {
            const int source = ops[1].reg.value - ZYDIS_REGISTER_XMM0;
            if (source < 0 || source > 15) return 0;
            memcpy(halves, &(&c->Xmm0)[source], count * 2u);
        }
        const int dest = (int)(ops[0].reg.value - (count == 8 ? ZYDIS_REGISTER_YMM0 : ZYDIS_REGISTER_XMM0));
        if (dest < 0 || dest > 15) return 0;
        DWORD length = 0;
        M128A *upper = (M128A *)LocateXStateFeature(c, XSTATE_AVX, &length);
        if (!upper || length < 16u * 16u) return 0;        /* the upper halves must be written (zeroed or set) */
        /* Upper halves in their initial state (vzeroupper before) are not restored from the
         * context unless AVX is marked present: make them explicit zeros and mark it. */
        DWORD64 features = 0;
        if (!GetXStateFeaturesMask(c, &features)) return 0;
        if (!(features & XSTATE_MASK_AVX)) {
            memset(upper, 0, 16u * 16u);
            if (!SetXStateFeaturesMask(c, features | XSTATE_MASK_AVX)) return 0;
        }
        float floats[8] = {0};
        for (unsigned i = 0; i < count; ++i) floats[i] = half_to_float(halves[i]);
        memcpy(&(&c->Xmm0)[dest], floats, 16);
        memcpy(&upper[dest], count == 8 ? floats + 4 : (float[4]){0, 0, 0, 0}, 16);
        return 1;
    }
    }
    return 0;
}

#define ISA_EXCEPTION_SITE (INT64_C(1) << 62)
static void note_site(uintptr_t rip) {
    const uint64_t h = ((uint64_t)rip * UINT64_C(0x9e3779b97f4a7c15)) >> 52;
    for (unsigned probe = 0; probe < 16; ++probe) {
        const size_t i = (size_t)((h + probe) & (ISA_SITES - 1));
        const LONG64 key = isa_site_key[i];
        if (key == (LONG64)rip || (!key && InterlockedCompareExchange64(&isa_site_key[i], (LONG64)rip, 0) == 0) ||
            isa_site_key[i] == (LONG64)rip) {
            InterlockedIncrement(&isa_site_hits[i]);
            return;
        }
    }
}
static void report(void) {
    const LONG64 now = (LONG64)GetTickCount64(), last = isa_last_report;
    if (now - last < 30000 || InterlockedCompareExchange64(&isa_last_report, now, last) != last) return;
    char line[512];
    int n = snprintf(line, sizeof(line), "CPU emulation (30 s):");
    for (int k = 0; k < K_COUNT && n < (int)sizeof(line) - 40; ++k) {
        const LONG hits = InterlockedExchange(&isa_hits[k], 0);
        if (hits) n += snprintf(line + n, sizeof(line) - (size_t)n, " %s %ld", kind_names[k], (long)hits);
    }
    n += snprintf(line + n, sizeof(line) - (size_t)n, " (%ld through the exception handler)",
                  (long)InterlockedExchange(&isa_exception_hits, 0));
    /* The three busiest sites since the last line, and the three busiest through the exception
     * handler (their keys carry ISA_EXCEPTION_SITE). */
    for (int pass = 0; pass < 2; ++pass)
        for (int top = 0; top < 3 && n < (int)sizeof(line) - 64; ++top) {
            size_t best = ISA_SITES;
            LONG most = 0;
            for (size_t i = 0; i < ISA_SITES; ++i)
                if (isa_site_hits[i] > most && !(isa_site_key[i] & ISA_EXCEPTION_SITE) == !pass) { most = isa_site_hits[i]; best = i; }
            if (best == ISA_SITES) break;
            n += snprintf(line + n, sizeof(line) - (size_t)n, "%s %#llx x%ld", top ? "," : pass ? "; by exception" : "; busiest",
                          (unsigned long long)(isa_site_key[best] & ~ISA_EXCEPTION_SITE), (long)most);
            isa_site_hits[best] = 0;
        }
    for (size_t i = 0; i < ISA_SITES; ++i) isa_site_hits[i] = 0;
    line[n++] = '\n';
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line, (DWORD)n, &written, NULL);
}

int runtime_isa_emulate(void *context) {
    if (!isa_mode) return 0;
    CONTEXT *c = context;
    const uintptr_t rip = (uintptr_t)c->Rip;
    unsigned char bytes[16];
    unsigned length;
    const IsaTrap *trap = isa_mode == 2 ? find_trap(rip) : NULL;
    if (trap) {
        memcpy(bytes, trap->bytes, trap->length);
        length = trap->length;
    } else {
        /* Up to 15 bytes, not past the end of the readable pages. */
        MEMORY_BASIC_INFORMATION info;
        const uintptr_t page_end = (rip | 0xfff) + 1;
        length = page_end - rip >= 15 ? 15u : (unsigned)(page_end - rip);
        if (length < 15 && VirtualQuery((const void *)page_end, &info, sizeof(info)) && info.State == MEM_COMMIT &&
            (info.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            length = 15;
        memcpy(bytes, (const void *)rip, length);
        swap_count_prefix(bytes, length, 0xf0, 0xf3);     /* a converted LZCNT / TZCNT */
    }
    ZydisDecodedInstruction in;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    if (!decode_bytes(bytes, length, &in, ops)) return 0;
    const int k = kind_of(&in);
    if (k < 0 || !run(c, &in, ops, rip)) return 0;
    c->Rip = rip + in.length;
    InterlockedIncrement(&isa_hits[k]);
    InterlockedIncrement(&isa_exception_hits);
    note_site(rip | (uintptr_t)ISA_EXCEPTION_SITE);
    report();
    return 1;
}
#else
int runtime_isa_active(void) { return 0; }
int runtime_isa_traps(const void *instruction) { (void)instruction; return 0; }
void runtime_isa_fix_moved(unsigned char *code, unsigned length) { (void)code; (void)length; }
void runtime_isa_convert_function(unsigned char *image, uint64_t begin, uint64_t end) { (void)image; (void)begin; (void)end; }
void runtime_isa_finish(void) {}
int runtime_isa_emulate(void *context) { (void)context; return 0; }
int runtime_isa_configure(uint32_t leaf1_ecx, uint32_t leaf7_ebx, uint32_t ext1_ecx, char *missing, size_t size) {
    (void)leaf1_ecx; (void)leaf7_ebx; (void)ext1_ecx;
    if (missing && size) missing[0] = 0;
    return 0;
}
#endif
