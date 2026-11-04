/* runtime_isa.c against the CPU (gow3 1 Nov 2026): each emulated instruction form runs natively
 * and through the handler (GOW3_ISA_COMPAT=trap: LZCNT/TZCNT in their LOCK form, the others as
 * ud2 sites), with the same inputs; results and the flags each instruction defines must match.
 * Needs a CPU with MOVBE, BMI1, LZCNT and F16C (the native side). Windows only. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

int runtime_isa_configure(uint32_t leaf1_ecx, uint32_t leaf7_ebx, uint32_t ext1_ecx, char *missing, size_t size);
void runtime_isa_fix_moved(unsigned char *code, unsigned length);
void runtime_isa_finish(void);
int runtime_isa_emulate(void *context);
unsigned runtime_isa_emit_count(unsigned char *out, const unsigned char *instruction, unsigned length, uintptr_t at);

/* probe.c's stub space next to the game image: here the upper half of the test's code page (within
 * jump range of the test bodies). "isa-test fast" lets the sites use it (stubs, no exception);
 * plain "isa-test" runs with GOW3_ISA_FAST=0 (every site an exception). */
static unsigned char *code_page;
static size_t stub_used;
unsigned char *runtime_near_image_alloc(size_t bytes, int executable) {
    (void)executable;
    if (!code_page || stub_used + bytes > (512u << 10)) return NULL;
    unsigned char *p = code_page + (512u << 10) + stub_used;
    stub_used += bytes;
    return p;
}

static LONG CALLBACK handler(EXCEPTION_POINTERS *info) {
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION && runtime_isa_emulate(info->ContextRecord))
        return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}

/* One test body: rax = input a, rcx = input b, rdx = memory pointer; runs the instruction under
 * test, then returns rax (result) with rflags in rdx (by an out pointer). Built at run time:
 *   mov rax, [rdi]; mov rcx, [rdi+8]; mov rdx, [rdi+16]; mov r8, [rdi+24]
 *   <instruction>
 *   mov [rdi], rax; mov [rdi+8], rcx; mov [rdi+24], r8; pushfq; pop rax; mov [rdi+32], rax; ret
 * (rdi = the state, Microsoft x64: rcx holds it on entry, moved to rdi). */
typedef struct { uint64_t rax, rcx, rdx, r8, flags; } State;
typedef void (*Body)(State *);

static size_t code_used;

static Body build(const unsigned char *instruction, unsigned length, int trap) {
    static const unsigned char prologue[] = {
        0x57,                               /* push rdi */
        0x48, 0x89, 0xcf,                   /* mov rdi, rcx */
        0x48, 0x8b, 0x07,                   /* mov rax, [rdi] */
        0x48, 0x8b, 0x4f, 0x08,             /* mov rcx, [rdi+8] */
        0x48, 0x8b, 0x57, 0x10,             /* mov rdx, [rdi+16] */
        0x4c, 0x8b, 0x47, 0x18,             /* mov r8, [rdi+24] */
    };
    static const unsigned char epilogue[] = {
        0x48, 0x89, 0x07,                   /* mov [rdi], rax */
        0x48, 0x89, 0x4f, 0x08,             /* mov [rdi+8], rcx */
        0x48, 0x89, 0x57, 0x10,             /* mov [rdi+16], rdx */
        0x4c, 0x89, 0x47, 0x18,             /* mov [rdi+24], r8 */
        0x9c, 0x58,                         /* pushfq; pop rax */
        0x48, 0x89, 0x47, 0x20,             /* mov [rdi+32], rax */
        0x5f, 0xc3,                         /* pop rdi; ret */
    };
    unsigned char *p = code_page + code_used;
    memcpy(p, prologue, sizeof prologue);
    unsigned char *site = p + sizeof prologue;
    memcpy(site, instruction, length);
    if (trap) runtime_isa_fix_moved(site, length);
    memcpy(site + length, epilogue, sizeof epilogue);
    code_used += (sizeof prologue + length + sizeof epilogue + 15) & ~(size_t)15;
    return (Body)(void *)p;
}

/* F16C: rcx/r8 = 8 halves into xmm1, ymm0 set to all ones, vcvtph2ps ymm0 or xmm0 from xmm1, then
 * rax/rcx = xmm0 and r8 = the low 8 bytes of ymm0's upper half (must be 0 for the xmm form). */
static Body build_f16c(int ymm, int trap, int clean) {
    static const unsigned char pre[] = {
        0x66, 0x48, 0x0f, 0x6e, 0xc9,             /* movq xmm1, rcx */
        0x66, 0x49, 0x0f, 0x3a, 0x22, 0xc8, 0x01, /* pinsrq xmm1, r8, 1 */
        0xc5, 0xfc, 0xc2, 0xc0, 0x0f,             /* vcmpps ymm0, ymm0, ymm0, true */
    };
    static const unsigned char post[] = {
        0x66, 0x48, 0x0f, 0x7e, 0xc0,             /* movq rax, xmm0 */
        0x66, 0x48, 0x0f, 0x3a, 0x16, 0xc1, 0x01, /* pextrq rcx, xmm0, 1 */
        0xc4, 0xe3, 0x7d, 0x19, 0xc2, 0x01,       /* vextractf128 xmm2, ymm0, 1 */
        0x66, 0x49, 0x0f, 0x7e, 0xd0,             /* movq r8, xmm2 */
        0xc5, 0xf8, 0x77,                         /* vzeroupper */
    };
    unsigned char sequence[64];
    unsigned char instruction[5] = {0xc4, 0xe2, 0x79, 0x13, 0xc1};   /* vcvtph2ps xmm0, xmm1 */
    if (ymm) instruction[2] = 0x7d;                                   /* vcvtph2ps ymm0, xmm1 */
    memcpy(sequence, pre, sizeof pre);
    if (clean) memcpy(sequence + 12, "\xc5\xf8\x77\x90\x90", 5);   /* vzeroupper: upper state clean */
    memcpy(sequence + sizeof pre, instruction, sizeof instruction);
    memcpy(sequence + sizeof pre + sizeof instruction, post, sizeof post);
    Body body = build(sequence, sizeof pre + sizeof instruction + sizeof post, 0);
    if (trap) {
        unsigned char *site = (unsigned char *)(void *)body + 19 + sizeof pre;   /* 19: the prologue */
        runtime_isa_fix_moved(site, sizeof instruction);
    }
    return body;
}

typedef struct { const char *name; unsigned char bytes[15]; unsigned length; uint32_t flags; int memory; } Case;
/* Flags compared: the ones each instruction defines (CF 0x1, ZF 0x40, SF 0x80, OF 0x800). */
static const Case cases[] = {
    {"lzcnt eax, ecx", {0xf3, 0x0f, 0xbd, 0xc1}, 4, 0x41, 0},
    {"lzcnt rax, rcx", {0xf3, 0x48, 0x0f, 0xbd, 0xc1}, 5, 0x41, 0},
    {"lzcnt ax, cx", {0x66, 0xf3, 0x0f, 0xbd, 0xc1}, 5, 0x41, 0},
    {"lzcnt rax, [rdx]", {0xf3, 0x48, 0x0f, 0xbd, 0x02}, 5, 0x41, 1},
    {"tzcnt eax, ecx", {0xf3, 0x0f, 0xbc, 0xc1}, 4, 0x41, 0},
    {"tzcnt rax, rcx", {0xf3, 0x48, 0x0f, 0xbc, 0xc1}, 5, 0x41, 0},
    {"tzcnt eax, [rdx+8]", {0xf3, 0x0f, 0xbc, 0x42, 0x08}, 5, 0x41, 1},
    {"movbe eax, [rdx]", {0x0f, 0x38, 0xf0, 0x02}, 4, 0, 1},
    {"movbe rax, [rdx]", {0x48, 0x0f, 0x38, 0xf0, 0x02}, 5, 0, 1},
    {"movbe ax, [rdx]", {0x66, 0x0f, 0x38, 0xf0, 0x02}, 5, 0, 1},
    {"movbe [rdx+8], ecx", {0x0f, 0x38, 0xf1, 0x4a, 0x08}, 5, 0, 1},
    {"movbe [rdx+8], rcx", {0x48, 0x0f, 0x38, 0xf1, 0x4a, 0x08}, 6, 0, 1},
    {"andn eax, ecx, r8d", {0xc4, 0xc2, 0x70, 0xf2, 0xc0}, 5, 0x8c1, 0},
    {"andn rax, rcx, r8", {0xc4, 0xc2, 0xf0, 0xf2, 0xc0}, 5, 0x8c1, 0},
    {"andn rax, rcx, [rdx]", {0xc4, 0xe2, 0xf0, 0xf2, 0x02}, 5, 0x8c1, 1},
    {"bextr eax, r8d, ecx", {0xc4, 0xc2, 0x70, 0xf7, 0xc0}, 5, 0x841, 0},
    {"bextr rax, r8, rcx", {0xc4, 0xc2, 0xf0, 0xf7, 0xc0}, 5, 0x841, 0},
    {"bextr rax, [rdx], rcx", {0xc4, 0xe2, 0xf0, 0xf7, 0x02}, 5, 0x841, 1},
    {"blsi eax, ecx", {0xc4, 0xe2, 0x78, 0xf3, 0xd9}, 5, 0x8c1, 0},
    {"blsi rax, rcx", {0xc4, 0xe2, 0xf8, 0xf3, 0xd9}, 5, 0x8c1, 0},
    {"blsmsk eax, ecx", {0xc4, 0xe2, 0x78, 0xf3, 0xd1}, 5, 0x8c1, 0},
    {"blsmsk rax, rcx", {0xc4, 0xe2, 0xf8, 0xf3, 0xd1}, 5, 0x8c1, 0},
    {"blsr eax, ecx", {0xc4, 0xe2, 0x78, 0xf3, 0xc9}, 5, 0x8c1, 0},
    {"blsr rax, rcx", {0xc4, 0xe2, 0xf8, 0xf3, 0xc9}, 5, 0x8c1, 0},
    {"blsr rax, [rdx]", {0xc4, 0xe2, 0xf8, 0xf3, 0x0a}, 5, 0x8c1, 1},
    /* Native-sequence corner cases (memory 2: rdx is the destination and is compared too). */
    {"andn rax, rcx, rax", {0xc4, 0xe2, 0xf0, 0xf2, 0xc0}, 5, 0x8c1, 0},
    {"andn eax, ecx, eax", {0xc4, 0xe2, 0x70, 0xf2, 0xc0}, 5, 0x8c1, 0},
    {"andn rdx, rcx, [rdx]", {0xc4, 0xe2, 0xf0, 0xf2, 0x12}, 5, 0x8c1, 2},
    {"andn r8, r8, rcx", {0xc4, 0x62, 0xb8, 0xf2, 0xc1}, 5, 0x8c1, 0},
    {"bextr rcx, rax, r8", {0xc4, 0xe2, 0xb8, 0xf7, 0xc8}, 5, 0x841, 0},
    {"bextr ecx, eax, ecx", {0xc4, 0xe2, 0x70, 0xf7, 0xc8}, 5, 0x841, 0},
    {"bextr r8d, ecx, r8d", {0xc4, 0x62, 0x38, 0xf7, 0xc1}, 5, 0x841, 0},
    {"movbe rdx, [rdx]", {0x48, 0x0f, 0x38, 0xf0, 0x12}, 5, 0, 2},
    {"movbe [rdx+16], r8", {0x4c, 0x0f, 0x38, 0xf1, 0x42, 0x10}, 6, 0, 1},
    {"blsr rax, rax", {0xc4, 0xe2, 0xf8, 0xf3, 0xc8}, 5, 0x8c1, 0},
    {"blsi rcx, rcx", {0xc4, 0xe2, 0xf0, 0xf3, 0xd9}, 5, 0x8c1, 0},
    {"blsmsk rdx, [rdx]", {0xc4, 0xe2, 0xe8, 0xf3, 0x12}, 5, 0x8c1, 2},
    {"blsr r8d, ecx", {0xc4, 0xe2, 0x38, 0xf3, 0xc9}, 5, 0x8c1, 0},
};

static uint64_t next(uint64_t *s) {   /* xorshift64*, with values biased to edge cases */
    *s ^= *s >> 12; *s ^= *s << 25; *s ^= *s >> 27;
    const uint64_t r = *s * UINT64_C(2685821657736338717);
    switch (r & 15) {
    case 0: return 0;
    case 1: return 1;
    case 2: return ~UINT64_C(0);
    case 3: return UINT64_C(1) << (r >> 58);
    case 4: return (r >> 8) & 0xffff;   /* small, e.g. bextr start/length */
    case 5: return (r >> 40) & 0x7f7f;
    default: return r;
    }
}

int main(int argc, char **argv) {
    const int fast_mode = argc > 1 && !strcmp(argv[1], "fast");
    _putenv("GOW3_ISA_COMPAT=trap");
    _putenv(fast_mode ? "GOW3_ISA_FAST=1" : "GOW3_ISA_FAST=0");
    printf("mode: %s\n", fast_mode ? "stubs where possible (fast)" : "every site an exception");
    if (!runtime_isa_configure(0, 0, 0, NULL, 0)) { puts("configure failed"); return 1; }
    code_page = VirtualAlloc(NULL, 1 << 20, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!code_page) return 1;
    AddVectoredExceptionHandler(1, handler);
    const size_t n = sizeof cases / sizeof *cases;
    Body native[64], emulated[64];
    for (size_t i = 0; i < n; ++i) {
        native[i] = build(cases[i].bytes, cases[i].length, 0);
        emulated[i] = build(cases[i].bytes, cases[i].length, 1);
    }
    /* LZCNT / TZCNT as native BSR / BSF sequences (runtime_isa_emit_count): no exception. */
    Body fast[64] = {0};
    for (size_t i = 0; i < n; ++i) {
        unsigned char sequence[64];
        unsigned char *site = code_page + code_used + 19;   /* where build() puts the instruction */
        const unsigned length = runtime_isa_emit_count(sequence, cases[i].bytes, cases[i].length, (uintptr_t)site);
        if (length) fast[i] = build(sequence, length, 0);
    }
    /* [0] xmm, [1] ymm, [2] xmm and [3] ymm with the upper halves clean (vzeroupper before). */
    const Body f16c_native[4] = {build_f16c(0, 0, 0), build_f16c(1, 0, 0), build_f16c(0, 0, 1), build_f16c(1, 0, 1)};
    const Body f16c_emulated[4] = {build_f16c(0, 1, 0), build_f16c(1, 1, 0), build_f16c(0, 1, 1), build_f16c(1, 1, 1)};
    runtime_isa_finish();
    FlushInstructionCache(GetCurrentProcess(), code_page, 1 << 20);
    uint64_t seed = 0x9e3779b97f4a7c15ull;
    int failures = 0;
    for (int variant = 0; variant < 4; ++variant) {
        const int ymm = variant & 1;
        int bad = 0;
        for (int round = 0; round < 20000 && bad < 3; ++round) {
            State a = {0, next(&seed), 0, next(&seed), 0};
            if (round & 1) a.rcx = (a.rcx & 0x83ff83ff83ff83ffull) | 0x7c007c0000000000ull;   /* inf / NaN / subnormal halves */
            State b = a;
            f16c_native[variant](&a);
            f16c_emulated[variant](&b);
            if (a.rax != b.rax || a.rcx != b.rcx || a.r8 != b.r8) {
                printf("MISMATCH vcvtph2ps %s: native %#llx %#llx upper %#llx | emulated %#llx %#llx upper %#llx\n",
                       ymm ? "ymm" : "xmm", (unsigned long long)a.rax, (unsigned long long)a.rcx, (unsigned long long)a.r8,
                       (unsigned long long)b.rax, (unsigned long long)b.rcx, (unsigned long long)b.r8);
                ++bad;
            }
        }
        printf("%-24s%s %s\n", ymm ? "vcvtph2ps ymm0, xmm1" : "vcvtph2ps xmm0, xmm1", variant >= 2 ? " (clean)" : "", bad ? "FAILED" : "ok (20000 inputs)");
        failures += bad != 0;
    }
    for (size_t i = 0; i < n; ++i) {
        int bad = 0;
        for (int round = 0; round < 20000 && bad < 3; ++round) {
            uint64_t memory_a[4], memory_b[4];
            for (int m = 0; m < 4; ++m) memory_a[m] = memory_b[m] = next(&seed);
            State a = {next(&seed), next(&seed), (uint64_t)(uintptr_t)memory_a, next(&seed), 0};
            State b = a;
            b.rdx = (uint64_t)(uintptr_t)memory_b;
            /* Flags going in: random too (instructions must leave undefined ones alone or set them). */
            native[i](&a);
            emulated[i](&b);
            const uint64_t mask = cases[i].flags;
            if (a.rax != b.rax || a.rcx != b.rcx || a.r8 != b.r8 || (a.flags & mask) != (b.flags & mask) ||
                (cases[i].memory == 2 && a.rdx != b.rdx) ||
                memcmp(memory_a, memory_b, sizeof memory_a)) {
                printf("MISMATCH %s: native rax %#llx rcx %#llx r8 %#llx flags %#llx | emulated rax %#llx rcx %#llx r8 %#llx flags %#llx\n",
                       cases[i].name, (unsigned long long)a.rax, (unsigned long long)a.rcx, (unsigned long long)a.r8,
                       (unsigned long long)(a.flags & mask), (unsigned long long)b.rax, (unsigned long long)b.rcx,
                       (unsigned long long)b.r8, (unsigned long long)(b.flags & mask));
                ++bad;
            }
        }
        printf("%-24s %s\n", cases[i].name, bad ? "FAILED" : "ok (20000 inputs)");
        failures += bad != 0;
        if (!fast[i]) continue;
        bad = 0;
        for (int round = 0; round < 20000 && bad < 3; ++round) {
            uint64_t memory_a[4], memory_b[4];
            for (int m = 0; m < 4; ++m) memory_a[m] = memory_b[m] = next(&seed);
            State a = {next(&seed), next(&seed), (uint64_t)(uintptr_t)memory_a, next(&seed), 0};
            State b = a;
            b.rdx = (uint64_t)(uintptr_t)memory_b;
            native[i](&a);
            fast[i](&b);
            const uint64_t mask = cases[i].flags;
            if (a.rax != b.rax || a.rcx != b.rcx || a.r8 != b.r8 || (a.flags & mask) != (b.flags & mask) ||
                (cases[i].memory == 2 && a.rdx != b.rdx) ||
                memcmp(memory_a, memory_b, sizeof memory_a)) {
                printf("MISMATCH fast %s: native rax %#llx flags %#llx | sequence rax %#llx flags %#llx (in rcx %#llx)\n",
                       cases[i].name, (unsigned long long)a.rax, (unsigned long long)(a.flags & mask),
                       (unsigned long long)b.rax, (unsigned long long)(b.flags & mask), (unsigned long long)a.rcx);
                ++bad;
            }
        }
        printf("%-24s %s\n", "  as native sequence", bad ? "FAILED" : "ok (20000 inputs)");
        failures += bad != 0;
    }
    /* Cost of one emulated instruction (exception + handler + resume). */
    {
        State s = {5, 7, (uint64_t)(uintptr_t)&seed, 9, 0};
        LARGE_INTEGER t0, t1, hz;
        QueryPerformanceFrequency(&hz);
        QueryPerformanceCounter(&t0);
        for (int i = 0; i < 200000; ++i) native[13](&s);
        QueryPerformanceCounter(&t1);
        const double native_ns = (double)(t1.QuadPart - t0.QuadPart) * 1e9 / (double)hz.QuadPart / 200000;
        QueryPerformanceCounter(&t0);
        for (int i = 0; i < 200000; ++i) emulated[13](&s);
        QueryPerformanceCounter(&t1);
        const double emulated_ns = (double)(t1.QuadPart - t0.QuadPart) * 1e9 / (double)hz.QuadPart / 200000;
        printf("andn: native %.1f ns, emulated %.0f ns per instruction (with the test body)\n", native_ns, emulated_ns);
    }
    printf("%d of %zu forms failed\n", failures, n + 4);
    return failures != 0;
}
