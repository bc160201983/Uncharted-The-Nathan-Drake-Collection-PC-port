/* gow3 24 Oct 2026: co-op and multiplayer research for Uncharted 2 (and later 3).
 *
 * Everything here is off unless an environment knob turns it on (the launcher sets none).
 *
 * GOW3_COOP_CMD=<file>: a live command console for tests. The loader puts a jump at the entry
 *   of the game's per-frame GameLoopUpdate (U2 1.02 image offset 0x3facb0); the stub calls
 *   coop_frame() on the game's own thread, once per frame. coop_frame() re-reads the file when
 *   it changes and runs its lines in order (output: "Coop: ..." lines in boot.log):
 *     r <addr> [n]            hex dump (n bytes, at most 512)
 *     rd <addr> [n]           n dwords;  rq <addr> [n]  n qwords;  rf <addr> [n]  n floats
 *     rs <addr>               a C string
 *     w8|w16|w32|w64 <addr> <value>     wf <addr> <float>     (through WriteProcessMemory)
 *     call <addr> [arg...]    calls a guest function (SysV): integer/pointer args go in
 *                             rdi..r9, args written f:<float> go in xmm0..xmm3; prints rax
 *     mon <name> <fmt> <addr> prints the value once a second (fmt: d u x q f f3 f4 h s);
 *                             "mon clear" removes them all
 *     wait <frames>           runs the remaining lines that many frames later
 *     dump <addr> <len> <file>  writes guest memory to a host file (unreadable parts as zeros)
 *     find32 <addr> <len> <value>  aligned dwords equal to value (at most 64 matches)
 *   Uncharted 2 helpers (1.02 addresses):
 *     levels                  every loaded package record (0xa60 bytes each at [0x1130390]):
 *                             index, name id, address, spawner count
 *     spawners <level>        the level's spawners (<level> = record index or a name) with
 *                             their world locators (0x4bdfe0)
 *     spawnat <spawner> [art] kills the player (if any) and spawns the hero at the spawner
 *                             (name or hex id; found with 0x56c7b0) through 0x975190, art
 *                             "hero-default" unless given
 *     spawncam                the debug spawn at the camera (0x969320)
 *     pkgwait                 what the package manager still waits for (0x56ce50's two request
 *                             lists: id, record, state +0x30 (7 = in), flags +0x18 (0x80))
 *     dcwait                  script/DC modules (list 0x1534140) not in state 8 (loaded)
 *     galloc <ctx> <size>     the game's allocator 0xb332a0 (memory context id, e.g. 5 = AppCpu,
 *                             0x14 = Network); $ = the block
 *     netinit <game> [MiB]    what the removed session layer did before a net game, so that the
 *                             online task (dev argument "task online") can run one: creates the
 *                             network memory context 0x14 (0xb45580, MiB from context 5, default
 *                             16), gives the net game manager's two hash tables storage, sets the
 *                             net game index (+0x1ca0, an index into *net-games*: 114 = Village
 *                             co-op) and puts the local player in slot 0 (id 1, host)
 *     echo <text>
 *   Address and value expressions: hex numbers (0x optional), i:<hex> = image offset,
 *   [<expr>] = the 8 bytes at <expr>, h:<expr> = an ND process handle at <expr> (the process
 *   pointer when the handle is valid, else 0), sid:<name> = ND string id, s:<text> = a pointer to
 *   that text, $ / $d = rax / rdx of the last call, b: = a 4 KiB scratch buffer (64-byte aligned, kept between commands), followed by
 *   any number of +<hex> / -<hex>. call takes up to 8 integer args (the 7th and 8th on the stack)
 *   and prints rax and rdx (16-byte struct returns, e.g. ND process handles).
 * GOW3_COOP_BOOT=<file>: the same commands, run once when the game's frame count reaches the
 *   number on the file's first line ("at <frames>"), for unattended tests.
 */
#include "runtime.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

#ifdef _WIN32
static unsigned char *image;
static uint64_t image_size;
static const char *cmd_path, *boot_path;
static uint64_t frames;

typedef struct {
    const char *game;
    uint64_t offset;
    unsigned length;                 /* bytes replaced (whole instructions, >= 5) */
    unsigned char expected[16];      /* original bytes at the site (all compared) */
    int frame;                       /* 1: the frame hook (coop_frame), 0: a state hook */
} CoopSite;
/* U2 1.02 GameLoopUpdate (the "GameLoopUpdate" job of big.cpp, registered at 0x3f5b0b):
 * push rbp; mov rbp, rsp; push r15; push r14; push r13; push r12; push rbx; sub rsp, 0x1b8 */
/* U2 1.02 TaskManagerOnline EstablishConnection Update (state vtable 0x100ac40 slot 3). */
static const CoopSite sites[] = {
    {"Uncharted 2", 0x3facb0, 6, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec}, 1},
    {"Uncharted 2", 0x5f2ea0, 6, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec}, 0},
    {"Uncharted 3", 0x56e650, 6, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec}, 1}, /* U3 1.02 GameLoopUpdate (runtime_coop_u3.c) */
};

static int rd(uint64_t address, void *out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)address, out, n, &got) && got == n;
}
static int wr(uint64_t address, const void *in, size_t n) {
    SIZE_T put = 0;
    return WriteProcessMemory(GetCurrentProcess(), (LPVOID)(uintptr_t)address, in, n, &put) && put == n;
}
static uint64_t rq(uint64_t address) {
    uint64_t v = 0;
    rd(address, &v, 8);
    return v;
}

/* ND string id: CRC-32 (polynomial 0x04c11db7, MSB first, initial value 0, no final xor). */
static uint32_t sid(const char *s, size_t n) {
    uint32_t c = 0;
    for (size_t i = 0; i < n; ++i) {
        c ^= (uint32_t)(unsigned char)s[i] << 24;
        for (int k = 0; k < 8; ++k) c = c & 0x80000000u ? (c << 1) ^ 0x04c11db7u : c << 1;
    }
    return c;
}

/* Texts for s:<text> arguments (kept until the next command file). */
static char strings[16][128];
static int string_count;
static __attribute__((aligned(64))) unsigned char scratch[4096];
static uint64_t last_rax, last_rdx;

static const char *skip(const char *p) {
    while (*p == ' ' || *p == '\t') ++p;
    return p;
}
static int is_end(char c) { return !c || c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ']'; }

static uint64_t expr(const char **pp, int *ok);
static uint64_t primary(const char **pp, int *ok) {
    const char *p = skip(*pp);
    uint64_t v = 0;
    if (*p == '[') {
        ++p;
        const uint64_t a = expr(&p, ok);
        p = skip(p);
        if (*p == ']') ++p; else *ok = 0;
        v = rq(a);
    } else if (!strncmp(p, "i:", 2)) {
        char *end;
        v = (uint64_t)(uintptr_t)image + strtoull(p + 2, &end, 16);
        if (end == p + 2) *ok = 0;
        p = end;
    } else if (!strncmp(p, "h:", 2)) {
        p += 2;
        const uint64_t a = primary(&p, ok);
        const uint64_t ptr = rq(a);
        const uint32_t id = (uint32_t)rq(a + 8);
        const uint64_t proc = ptr ? rq(ptr) : 0;
        v = proc && id && (uint32_t)rq(proc + 0x44) == id ? proc : 0;
    } else if (!strncmp(p, "sid:", 4)) {
        p += 4;
        const char *s = p;
        while (!is_end(*p)) ++p;
        v = sid(s, (size_t)(p - s));
    } else if (*p == '$') {
        ++p;
        if (*p == 'd') { ++p; v = last_rdx; } else v = last_rax;
    } else if (!strncmp(p, "b:", 2)) {
        p += 2;
        v = (uint64_t)(uintptr_t)scratch;
    } else if (!strncmp(p, "s:", 2)) {
        p += 2;
        const char *s = p;
        while (!is_end(*p)) ++p;
        if (string_count < 16) {
            size_t n = (size_t)(p - s) < 127 ? (size_t)(p - s) : 127;
            memcpy(strings[string_count], s, n);
            strings[string_count][n] = 0;
            v = (uint64_t)(uintptr_t)strings[string_count++];
        } else *ok = 0;
    } else {
        char *end;
        v = strtoull(p, &end, 16);
        if (end == p) *ok = 0;
        p = end;
    }
    *pp = p;
    return v;
}
static uint64_t expr(const char **pp, int *ok) {
    uint64_t v = primary(pp, ok);
    for (;;) {
        const char *p = *pp;
        if (*p == '+' || *p == '-') {
            const int minus = *p == '-';
            ++p;
            const uint64_t w = primary(&p, ok);
            v = minus ? v - w : v + w;
            *pp = p;
        } else break;
    }
    return v;
}

typedef struct { char name[24]; char fmt[4]; char text[96]; } Monitor;
static Monitor monitors[24];
static int monitor_count;

static void show(const char *name, const char *fmt, uint64_t a) {
    char line[512];
    int n = snprintf(line, sizeof(line), "Coop: %s @%llx =", name, (unsigned long long)a);
    if (!strcmp(fmt, "d")) { int32_t v = 0; rd(a, &v, 4); n += snprintf(line + n, sizeof(line) - n, " %d", v); }
    else if (!strcmp(fmt, "u")) { uint32_t v = 0; rd(a, &v, 4); n += snprintf(line + n, sizeof(line) - n, " %u", v); }
    else if (!strcmp(fmt, "x")) { uint32_t v = 0; rd(a, &v, 4); n += snprintf(line + n, sizeof(line) - n, " %#x", v); }
    else if (!strcmp(fmt, "q")) n += snprintf(line + n, sizeof(line) - n, " %#llx", (unsigned long long)rq(a));
    else if (fmt[0] == 'f') {
        int count = fmt[1] ? fmt[1] - '0' : 1;
        if (count < 1 || count > 8) count = 1;
        float v[8] = {0};
        rd(a, v, (size_t)count * 4);
        for (int i = 0; i < count; ++i) n += snprintf(line + n, sizeof(line) - n, " %.3f", v[i]);
    } else if (!strcmp(fmt, "s")) {
        char s[96] = {0};
        rd(a, s, sizeof(s) - 1);
        n += snprintf(line + n, sizeof(line) - n, " \"%s\"", s);
    }
    puts(line);
}

/* Guest call: integer args in rdi..r9 then the stack, floats in xmm0..xmm3 (SysV); a 16-byte
 * struct of two integers comes back in rax:rdx. */
typedef struct { uint64_t rax, rdx; } GuestPair;
typedef GuestPair (ABI *GuestCall)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t, float, float, float, float);


/* ---- Uncharted 2 (1.02) helpers ---- */
#define U2_PACKAGES      0x1130390   /* [this] = 180 package records of 0xa60 bytes */
#define U2_PLAYER        0x115bd20   /* player process handle */
typedef struct { uint64_t rax, rdx; } GuestPair2;
typedef GuestPair2 (ABI *U2Spawn)(void *, void *, const char *, uint64_t, uint64_t, uint64_t, uint64_t);
typedef void *(ABI *U2Locator)(void *out, uint64_t spawner);
typedef uint64_t (ABI *U2Lookup)(uint64_t mgr, uint32_t id);
typedef uint64_t (ABI *U2Dc)(uint32_t id);
typedef void (ABI *U2Void)(void);

static uint64_t u2_record(const char *arg) {
    const uint64_t base = rq((uint64_t)(uintptr_t)image + U2_PACKAGES);
    if (!base) return 0;
    char *end;
    const unsigned long index = strtoul(arg, &end, 10);
    if (end != arg && is_end(*end)) return index < 180 ? base + index * 0xa60 : 0;
    size_t n = 0;
    while (!is_end(arg[n])) ++n;
    const uint32_t id = sid(arg, n);
    for (int i = 0; i < 180; ++i) {
        uint32_t v = 0;
        rd(base + (uint64_t)i * 0xa60 + 0x74, &v, 4);
        if (v == id) return base + (uint64_t)i * 0xa60;
    }
    return 0;
}
static uint32_t name_or_id(const char *arg) {
    size_t n = 0;
    while (!is_end(arg[n])) ++n;
    if (n > 2 && arg[0] == '0' && arg[1] == 'x') return (uint32_t)strtoul(arg, NULL, 16);
    return sid(arg, n);
}
/* A spawner by id in any loaded package record (0x56c7b0 only searches the gameplay levels, not
 * e.g. the co-op net pak village-wide-coop). */
static uint64_t u2_find_spawner(uint32_t id) {
    const uint64_t base = rq((uint64_t)(uintptr_t)image + U2_PACKAGES);
    for (int i = 0; i < 180 && base; ++i) {
        const uint64_t t = rq(base + (uint64_t)i * 0xa60 + 0x9a0);
        if (!t) continue;
        int32_t n = 0;
        rd(t + 0x20, &n, 4);
        const uint64_t arr = rq(t + 0x18);
        for (int k = 0; k < n && arr; ++k) {
            uint32_t v = 0;
            rd(arr + (uint64_t)k * 16, &v, 4);
            if (v == id) return rq(arr + (uint64_t)k * 16 + 8);
        }
    }
    return 0;
}

/* ---- one-player net game (gow3 24 Oct 2026) ---- */
static unsigned char *page_near_image(void);
#define U2_NGM           0x113b760   /* g_netGameManager */
#define U2_SESSION       0x1065b08   /* +0x10 session (NULL: we are host), +0xb00 local id, +0xb04 host id */
#define U2_ALLOCATORS    0x11c6630   /* allocator per memory context */
typedef uint64_t (ABI *U2Alloc)(uint64_t size, const uint32_t *context, uint64_t align);
typedef void (ABI *U2NetHeapInit)(uint64_t buffer, uint32_t size);
typedef void (ABI *U2SlotInit)(uint64_t slot);

static uint64_t u2_alloc(uint32_t context, uint64_t size) {
    static uint32_t ctx;
    ctx = context;
    if (!rq((uint64_t)(uintptr_t)image + U2_ALLOCATORS + (uint64_t)context * 8)) return 0;
    return ((U2Alloc)(image + 0xb332a0))(size, &ctx, 16);
}
/* An empty ND HashTable (corelib/containers/hashtable.h) at base: +0 buckets (count + 1
 * pointers, the last = the end node), +8 free list, +0x14 node capacity, +0x18 node pool, +0x30 &buckets[count],
 * +0x3c bucket count. StartNetGame (0x640fc0) links the free list from the pool. */
static int u2_hashtable(uint64_t base, int buckets, int nodes, uint64_t node_size) {
    if (rq(base)) return 1;
    const uint64_t b = u2_alloc(0x14, (uint64_t)(buckets + 1) * 8);
    const uint64_t pool = u2_alloc(0x14, (uint64_t)nodes * node_size);
    if (!b || !pool) return 0;
    static unsigned char zero[4096];
    for (uint64_t at = 0; at < (uint64_t)(buckets + 1) * 8; at += sizeof(zero))
        wr(b + at, zero, (uint64_t)(buckets + 1) * 8 - at < sizeof(zero) ? (size_t)((uint64_t)(buckets + 1) * 8 - at) : sizeof(zero));
    for (uint64_t at = 0; at < (uint64_t)nodes * node_size; at += sizeof(zero))
        wr(pool + at, zero, (uint64_t)nodes * node_size - at < sizeof(zero) ? (size_t)((uint64_t)nodes * node_size - at) : sizeof(zero));
    const uint64_t end = b + (uint64_t)buckets * 8;
    /* buckets[count] = the end node: iterations walk the node chains until they reach it */
    const uint64_t end_node = u2_alloc(0x14, node_size);
    if (!end_node) return 0;
    wr(end_node, zero, (size_t)node_size);
    wr(end, &end_node, 8);
    const int32_t cap = nodes, count = buckets;
    wr(base, &b, 8);
    wr(base + 8, &pool, 8);
    wr(base + 0x14, &cap, 4);
    wr(base + 0x18, &pool, 8);
    wr(base + 0x30, &end, 8);
    wr(base + 0x3c, &count, 4);
    return 1;
}
/* The net clock: the game reads a struct through the pointer at 0x113a040 (+0xc = net time in
 * ticks of 1/300 s; WaitReady starts the game 900 ticks after everyone is ready, the HUD timers
 * and respawns use it). The removed net layer owned and advanced it; we keep one here and
 * advance it once per frame from coop_frame(). */
static __attribute__((aligned(64))) unsigned char net_clock[0x80];
extern int runtime_net_host_clock(uint32_t *ticks);
static ULONGLONG net_clock_start;
/* The Cinema recorder's bitstream at 0x113a258 ({u32 *data, u32 bit position, u32 bit
 * capacity, ...}): WaitReady's Exit writes the start-of-game record (type 5) into it and the
 * game appends to it while playing. Its buffer came from the removed layer too; ours is
 * rewound when three quarters full (the recording is not kept). */
static __attribute__((aligned(64))) unsigned char net_record[4 << 20];
static void u2_net_clock_tick(void) {
    if (!net_clock_start) return;
    /* Net time = the game's own clock (0x1116180 + 0x204, 300 ticks per second): StartPlaying
     * stamps the slots with the net time (ngm+0x1d60 -> slot+0x17c) and the co-op checkpoint
     * start stamps the respawn with the game clock (slot+0x188); the tracker spawns only when
     * the second is not older than the first. A joiner should take the host's (TODO: sync). */
    uint32_t ticks = 0;
    rd((uint64_t)(uintptr_t)image + 0x1116180 + 0x204, &ticks, 4);
    if (!ticks) ticks = (uint32_t)((GetTickCount64() - net_clock_start) * 300 / 1000) + 300;
    /* A joiner runs on the host's net time (runtime_net.c: frame kind 3, extrapolated at 300/s). */
    uint32_t host = 0;
    if (runtime_net_host_clock(&host)) {
        static int adopted;
        if (!adopted) { adopted = 1; printf("Coop: net clock follows the host (%u, local %u)\n", host, ticks); fflush(stdout); }
        /* The game clock itself (0x1116180+0x204, incremented by 5 per 60 Hz frame) must be the
         * host's too: snapshot times travel as 11 bits and are unwrapped against it (0x5e3680),
         * and the spawn timing compares it with the net time. With a local clock 15-20 s behind
         * the host's, the joiner's NetCharacter got snapshots in the wrong order (parse
         * overruns, NaN positions: runs q3/q4). Kept in step: forward jumps, or back when far. */
        const int32_t behind = (int32_t)(host - ticks);
        if (ticks && (behind > 15 || behind < -150)) {
            static int jumps;
            wr((uint64_t)(uintptr_t)image + 0x1116180 + 0x204, &host, 4);
            if (jumps++ < 5) { printf("Coop: game clock set to the host's (%u, was %u)\n", host, ticks); fflush(stdout); }
        }
        ticks = host;
    }
    memcpy(net_clock + 0xc, &ticks, 4);
    uint32_t bits = 0;
    const uint64_t rec = (uint64_t)(uintptr_t)image + 0x113a258;
    if (rq(rec) == (uint64_t)(uintptr_t)net_record && rd(rec + 8, &bits, 4) && bits > sizeof(net_record) * 6) {
        const uint32_t zero = 0;
        wr(rec + 8, &zero, 4);
    }
}
/* Slot 0 = the local player, the host. EstablishConnection's Enter (0x63db90) resets every
 * slot (the session layer then re-added its members), so coop_establish() re-adds it. */
static int net_local_wanted;
/* runtime_net.c (the LAN transport, inert unless GOW3_NET_HOST / GOW3_NET_JOIN is set). */
extern void runtime_net_frame(unsigned char *base);
extern int runtime_net_peer(int *id, char *name, size_t n);
extern int runtime_net_local_id(void);
/* One player slot: init (0x662a30), gamertag (+0x20, non-empty = in use), display name
 * (+0x60), player id (+0x120), connection id (+0x23c: the connection record whose +8 is this
 * id; -1 for ourselves), co-op character (+0x198: 0, 1, 2 = the checkpoint's first, second,
 * third spawner). */
static void u2_fill_slot(uint64_t slot, int32_t id, int32_t connection, const char *name) {
    ((U2SlotInit)(image + 0x662a30))(slot);
    char text[0x40] = {0};
    snprintf(text, sizeof(text), "%s", name && *name ? name : "Player");
    wr(slot + 0x20, text, sizeof(text));
    wr(slot + 0x60, text, sizeof(text));
    const int32_t character = id >= 1 && id <= 3 ? id - 1 : 0;
    wr(slot + 0x120, &id, 4);
    wr(slot + 0x23c, &connection, 4);
    wr(slot + 0x198, &character, 4);
    /* +0x240: the player's SceNpId (36 bytes; the removed session layer filled it). The host's
     * player-setup event (type 0x12, sent by 0x610840 after AssignTeams) carries each slot's
     * NpId, team, co-op character and skin, and its Execute (0x632110) applies each entry to
     * the slot whose NpId matches (strcmp): with every NpId empty all entries landed on the
     * first slot. A unique text per player id keeps them apart on both sides. */
    char npid[0x24] = {0};
    snprintf(npid, sizeof(npid), "lan-player-%d", id);
    wr(slot + 0x240, npid, sizeof(npid));
    /* the player's character: skin index id - 1 in runtime_coop_mp.c's skin table (net games) */
    { extern void runtime_coop_mp_slot_skin(unsigned char *, uint64_t, int); runtime_coop_mp_slot_skin(image, slot, id); }
}
/* Player id n lives in slot n - 1 on every machine (host = id 1 = slot 0): the co-op
 * checkpoint start (0x646f10) gives each slot index its own spawner of the checkpoint, so the
 * order must be the same everywhere (else both players spawn on spawner 0). ngm+0 = our slot,
 * ngm+8 = the host's; the session's +0xb00 / +0xb04 = our id / the host's id. */
static uint64_t u2_slot_of(int32_t id) {
    if (id < 1 || id > 10) id = 1;
    return (uint64_t)(uintptr_t)image + U2_NGM + 0x10 + (uint64_t)(id - 1) * 0x2c0;
}
static void u2_add_local(void) {
    const uint64_t I = (uint64_t)(uintptr_t)image, ngm = I + U2_NGM;
    int32_t id = runtime_net_local_id();
    if (id <= 0) id = 1;
    const uint64_t slot = u2_slot_of(id);
    /* our own name (run f7: the joiner's results listed itself as "Partner") */
    extern const char *runtime_net_local_name(void);
    const char *own = runtime_net_local_name();
    u2_fill_slot(slot, id, -1, own && *own ? own : id == 1 ? "Drake" : "Partner");
    const int32_t host = 1;
    wr(ngm, &slot, 8);
    if (id == host) wr(ngm + 8, &slot, 8);
    wr(I + U2_SESSION + 0xb00, &id, 4);
    wr(I + U2_SESSION + 0xb04, &host, 4);
}
static int u2_add_peer(void) {
    int peer = 0;
    char name[32] = {0};
    if (!runtime_net_peer(&peer, name, sizeof(name)) || peer <= 0) {
        /* LAN game, peer not here yet: slot 1 holds a placeholder that is in use but not
         * connected (+0x275 set, +0x148 clear), so EstablishConnection keeps waiting. */
        const char *host = getenv("GOW3_NET_HOST"), *join = getenv("GOW3_NET_JOIN");
        if ((!host || !*host) && (!join || !*join)) return 0;
        const int32_t expected = runtime_net_local_id() == 2 ? 1 : 2;
        const uint64_t slot = u2_slot_of(expected);
        int32_t have = 0;
        rd(slot + 0x120, &have, 4);
        if (have != expected) {
            u2_fill_slot(slot, expected, expected, "(waiting)");
            const uint8_t one = 1;
            wr(slot + 0x275, &one, 1);
            printf("Coop: EstablishConnection: waiting for player %d (placeholder in slot %d)\n", expected, expected - 1);
            fflush(stdout);
        }
        return 0;
    }
    const uint64_t ngm = (uint64_t)(uintptr_t)image + U2_NGM, slot = u2_slot_of(peer);
    int32_t have = 0;
    char current[16] = {0};
    rd(slot + 0x120, &have, 4);
    rd(slot + 0x20, current, sizeof(current) - 1);
    if (have != peer || !strcmp(current, "(waiting)")) {
        u2_fill_slot(slot, peer, peer, name);
        const uint8_t one = 1;
        wr(slot + 0x275, &one, 1);   /* connected */
        wr(slot + 0x148, &one, 1);   /* ready (EstablishConnection) */
        printf("Coop: EstablishConnection: peer %d (%s) added to slot %d\n", peer, name, peer - 1);
        fflush(stdout);
    }
    if (peer == 1) wr(ngm + 8, &slot, 8);
    return 1;
}
/* Every frame while a net game is set up: the peer's slot as soon as its hello is in (a
 * joiner's TaskManagerOnline goes from Active to LoadLevels without EstablishConnection). */
static void u2_net_peer_frame(void) {
    if (!net_local_wanted) return;
    /* Our own slot must stay: it is the source of our ready event (0x60c4c0 sends [ngm]+0x120)
     * and of everything we send. */
    if (!rq((uint64_t)(uintptr_t)image + U2_NGM)) {
        u2_add_local();
        puts("Coop: net game: local player (re)added to its slot (ngm+0 was empty)");
        fflush(stdout);
    }
    int peer = 0;
    char name[32];
    if (runtime_net_peer(&peer, name, sizeof(name)) && peer > 0) u2_add_peer();
}
/* Hook: entry of TaskManagerOnline EstablishConnection's Update (0x5f2ea0), on the game thread.
 * The Enter (0x63db90) cleared every slot; the removed session layer re-added its members while
 * EstablishConnection waited for them. */
static void ABI coop_establish(void) {
    if (!net_local_wanted) return;
    int32_t id = 0;
    const int32_t local = runtime_net_local_id() > 0 ? runtime_net_local_id() : 1;
    rd(u2_slot_of(local) + 0x120, &id, 4);
    if (!id) {
        u2_add_local();
        puts("Coop: EstablishConnection: local player (re)added to its slot");
        fflush(stdout);
    }
    u2_add_peer();
}
/* The net game manager's per-frame update sorts its snapshot entries with scratch memory taken
 * from the tail of a heap arena (e.g. 0x66d150 -> 0x6700c0: "SetHeapCurrent" on the arena at
 * +0x50 of the allocator of memory context 0x45, several call sites). Context 0x45 resolves to
 * the single-frame allocator, which is not a heap (Bluepoint's assert at 0xb2f3ab), so while a
 * net game runs, HeapAllocator::GetAllocator (0xb2f280) answers context 0x45 with our own
 * scratch arena: allocator + 0x50 = {current, end, base, (capacity << 2) | 2 (external
 * modification allowed)}. Other contexts take the original code. */
static __attribute__((aligned(64))) unsigned char net_scratch[1 << 20];
static __attribute__((aligned(64))) uint64_t net_scratch_allocator[16];
static int u2_patch_sort_arena(void) {
    const uint64_t site = 0xb2f280;
    static const unsigned char prologue[6] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
    if (memcmp(image + site, prologue, sizeof(prologue))) return image[site] == 0xe9;  /* done before */
    unsigned char *stub = page_near_image();
    if (!stub) return 0;
    uint64_t *arena = net_scratch_allocator + 10;  /* +0x50 */
    arena[0] = (uint64_t)(uintptr_t)net_scratch;
    arena[1] = (uint64_t)(uintptr_t)net_scratch + sizeof(net_scratch);
    arena[2] = (uint64_t)(uintptr_t)net_scratch;
    arena[3] = ((uint64_t)sizeof(net_scratch) << 2) | 2;
    unsigned n = 0;
    static const unsigned char cmp45[] = {0x81, 0x3f, 0x45, 0x00, 0x00, 0x00, 0x75, 0x0b}; /* cmp dword [rdi], 0x45; jne +11 */
    memcpy(stub + n, cmp45, sizeof(cmp45)); n += sizeof(cmp45);
    stub[n++] = 0x48; stub[n++] = 0xb8;                                                /* mov rax, imm64 */
    const uint64_t a = (uint64_t)(uintptr_t)net_scratch_allocator;
    memcpy(stub + n, &a, 8); n += 8;
    stub[n++] = 0xc3;                                                                  /* ret */
    memcpy(stub + n, prologue, sizeof(prologue)); n += sizeof(prologue);               /* replayed */
    static const unsigned char jump[] = {0xff, 0x25, 0x00, 0x00, 0x00, 0x00};          /* jmp [rip+0] */
    memcpy(stub + n, jump, sizeof(jump)); n += sizeof(jump);
    const uint64_t back = (uint64_t)(uintptr_t)(image + site + sizeof(prologue));
    memcpy(stub + n, &back, 8); n += 8;
    DWORD old;
    VirtualProtect(stub, 4096, PAGE_EXECUTE_READ, &old);
    const int64_t delta = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + site + 5);
    if (delta < INT32_MIN || delta > INT32_MAX) return 0;
    unsigned char patch[6] = {0xe9, 0, 0, 0, 0, 0x90};
    const int32_t r = (int32_t)delta;
    memcpy(patch + 1, &r, 4);
    return wr((uint64_t)(uintptr_t)(image + site), patch, sizeof(patch));
}
/* The session object (0x1065b08) points at the removed layer's session at +0x10; most code
 * only tests it for NULL (then IsHost compares +0xb00 / +0xb04) and Playing's Update ends the
 * game at once (SetGameEnd 3 at 0x5f1c72) unless it is set and +0xc8 ("joined") is non-zero.
 * It gets a zeroed stand-in. */
static __attribute__((aligned(64))) unsigned char net_session_standin[4096];
/* Multiplayer twins of single-player DC tables: in a net game the code asks for the MP symbol
 * (e.g. `mov edi, 0x837a26b6; cmp [mode], 1; jg; mov edi, 0x974cf651` at 0xa61000 and 0xada639,
 * GetPlayerWeaponSettings), which lived in a DC module the Collection lacks. LookupSymbolValue
 * (0xa15e70) gets a detour on its not-found branch (0xa15e95, test rax,rax; je): a symbol in
 * this table is looked up again (0xa15300) under its single-player twin. */
static const uint32_t mp_twins[][2] = {
    {0x837a26b6u, 0x974cf651u},
    {0, 0},
};
static int u2_patch_dc_twins(void) {
    static const unsigned char expected[5] = {0x48, 0x85, 0xc0, 0x74, 0x08};
    const uint64_t site = 0xa15e95;
    if (memcmp(image + site, expected, 5)) return image[site] == 0xe9;
    unsigned char *p = page_near_image();
    if (!p) return 0;
    const uint64_t table = (uint64_t)(uintptr_t)mp_twins, found = (uint64_t)(uintptr_t)(image + 0xa15e9a),
                   missing = (uint64_t)(uintptr_t)(image + 0xa15ea2), find = (uint64_t)(uintptr_t)(image + 0xa15300);
    unsigned n = 0;
#define B(...) do { const unsigned char b_[] = {__VA_ARGS__}; memcpy(p + n, b_, sizeof(b_)); n += sizeof(b_); } while (0)
#define Q(v) do { const uint64_t q_ = (v); memcpy(p + n, &q_, 8); n += 8; } while (0)
    B(0x48, 0x85, 0xc0);                 /* test rax, rax */
    B(0x75, 0x00); const unsigned j_hit = n - 1;  /* jnz hit (patched below) */
    B(0x48, 0xbe); Q(table);             /* mov rsi, table */
    const unsigned loop = n;
    B(0x8b, 0x06);                       /* mov eax, [rsi] */
    B(0x85, 0xc0);                       /* test eax, eax */
    B(0x74, 0x00); const unsigned j_miss1 = n - 1; /* jz miss */
    B(0x39, 0xd8);                       /* cmp eax, ebx */
    B(0x74, 0x06);                       /* je twin (skip add + jmp: 4 + 2) */
    B(0x48, 0x83, 0xc6, 0x08);           /* add rsi, 8 */
    B(0xeb, (unsigned char)(loop - (n + 2)));  /* jmp loop */
    B(0x8b, 0x7e, 0x04);                 /* twin: mov edi, [rsi+4] */
    B(0x48, 0xb8); Q(find);              /* mov rax, 0xa15300 */
    B(0xff, 0xd0);                       /* call rax */
    B(0x48, 0x85, 0xc0);                 /* test rax, rax */
    B(0x74, 0x00); const unsigned j_miss2 = n - 1; /* jz miss */
    const unsigned hit = n;
    B(0x48, 0xb9); Q(found);             /* hit: mov rcx, 0xa15e9a (mov rax,[rax] ...) */
    B(0xff, 0xe1);                       /* jmp rcx */
    const unsigned miss = n;
    B(0x48, 0xb9); Q(missing);           /* miss: mov rcx, 0xa15ea2 (message, NULL) */
    B(0xff, 0xe1);                       /* jmp rcx */
#undef B
#undef Q
    p[j_hit] = (unsigned char)(hit - (j_hit + 1));
    p[j_miss1] = (unsigned char)(miss - (j_miss1 + 1));
    p[j_miss2] = (unsigned char)(miss - (j_miss2 + 1));
    DWORD old;
    VirtualProtect(p, 4096, PAGE_EXECUTE_READ, &old);
    const int64_t delta = (int64_t)(uintptr_t)p - (int64_t)(uintptr_t)(image + site + 5);
    if (delta < INT32_MIN || delta > INT32_MAX) return 0;
    unsigned char jump[5] = {0xe9, 0, 0, 0, 0};
    const int32_t r = (int32_t)delta;
    memcpy(jump + 1, &r, 4);
    return wr((uint64_t)(uintptr_t)(image + site), jump, 5);
}
/* Snapshot parses ended early by netinit's assert patch (0x66b430), reported by coop_frame;
 * per assert block too (25 Oct, run a07: which check ends the host's parses of the joiner). */
static volatile uint32_t *coop_parse_aborts;
#define COOP_PARSE_SITES 32
static uint32_t coop_parse_site_at[COOP_PARSE_SITES];
static __attribute__((aligned(64))) volatile uint32_t coop_parse_site_count[COOP_PARSE_SITES];
static int coop_parse_sites;
/* NaN keyframe targets skipped by netinit's patch at 0x527f49, reported by coop_frame. */
static volatile uint32_t *coop_nan_skips;
static int u2_netinit(const char *p) {
    char *end;
    const long game = strtol(skip(p), &end, 0);
    long mib = strtol(end, NULL, 0);
    if (mib <= 0) mib = 16;
    const uint64_t I = (uint64_t)(uintptr_t)image;
    if (!rq(I + U2_ALLOCATORS + 0x14 * 8)) {
        /* host memory: context 5 has only about 25 MB free in a game, a 2-player net game
         * needs more than 16 MiB here */
        const uint64_t heap = (uint64_t)(uintptr_t)VirtualAlloc(NULL, (SIZE_T)mib << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!heap) { printf("Coop: netinit: no %ld MiB of host memory\n", mib); return 1; }
        ((U2NetHeapInit)(image + 0xb45580))(heap, (uint32_t)((uint64_t)mib << 20));
        printf("Coop: netinit: network memory context 0x14: %ld MiB at %llx\n", mib, (unsigned long long)heap);
    }
    const uint64_t ngm = I + U2_NGM;
    const int t1 = u2_hashtable(ngm + 0x1ba0, 64, 256, 0x60), t2 = u2_hashtable(ngm + 0x1c10, 64, 256, 0x38);
    printf("Coop: netinit: hash tables %s %s\n", t1 ? "ok" : "FAILED", t2 ? "ok" : "FAILED");
    if (game >= 0) { const int16_t g = (int16_t)game; wr(ngm + 0x1ca0, &g, 2); }
    u2_add_local();
    net_local_wanted = 1;
    printf("Coop: netinit: snapshot sort scratch arena %s\n", u2_patch_sort_arena() ? "patched (GetAllocator 0x45 at 0xb2f280)" : "NOT patched");
    /* 0x104f1c0: starts at 1 and Playing's Update ends the game at once while it is set
     * (endstate 3); the Cinema start (0x5e564c) clears it, so did the lobby. 0x1139fe0: the
     * money manager's divisor, set by Playing's Enter only after StartPlaying (0x647a50)
     * already divided by it (5 is the game's own default). */
    const uint8_t zero8 = 0, one8 = 1;
    wr(I + 0x104f1c0, &zero8, 1);
    /* Each net game names its player loadout (weapon list) at +0x30: 0x8cf8e1cc for most,
     * 0x5635d23f for Village / Monastery co-op, ... They lived in a DC module the Collection
     * does not ship ("LookupSymbolValue: Symbol ... not found" + assert when the player spawns,
     * 0x663400 -> 0x976300). The game's own debug loadout *player-debug-weapons* (0xc66bbbc7:
     * Colt Defender + machine gun; the co-op maps also lay weapons out) stands in. */
    if (game >= 0) {
        const uint64_t tbl = ((U2Dc)(image + 0xa15e70))(0x3b9a067d); /* *net-games* */
        int32_t count = 0;
        if (tbl) rd(tbl, &count, 4);
        const uint64_t entries = tbl ? rq(tbl + 8) : 0;
        if (entries && game < count) {
            uint32_t setup = 0;
            const uint32_t def = 0xc66bbbc7u;
            rd(entries + (uint64_t)game * 0x90 + 0x30, &setup, 4);
            if (setup != def) {
                wr(entries + (uint64_t)game * 0x90 + 0x30, &def, 4);
                printf("Coop: netinit: net game %ld loadout %#x (not shipped) -> *player-debug-weapons*\n", game, setup);
            }
        }
    }
    /* Player skins: the net player spawn (0x663280) takes its art from the skin table
     * (DC symbol 0x5a4731d2, 0x30-byte entries, +8 = actor name) at the slot's skin index
     * (slot+0x138, 0 after init; since 25 Oct 11:00 runtime_coop_mp.c replaces the table with one
     * entry per slot, each player's own character, and these first entries are only its model). The multiplayer skins' actors are mostly not shipped (the
     * default "mp-hero" is not), and an art that did not load leaves the new player without a
     * skeleton (fault at 0x981df6). The first three entries get Drake's single-player actors,
     * which the overlay manifest adds to the co-op net paks: hero-parka on the snow maps,
     * hero-default on Warzone (net game 113). */
    {
        static const char parka[] = "hero-parka", plain[] = "hero-default";
        const uint64_t skins = ((U2Dc)(image + 0xa15e70))(0x5a4731d2);
        int32_t count = 0;
        if (skins) rd(skins, &count, 4);
        const uint64_t entries = skins ? rq(skins + 8) : 0;
        const uint64_t name = (uint64_t)(uintptr_t)(game == 113 ? plain : parka);
        for (int i = 0; i < 3 && i < count && entries; ++i) wr(entries + (uint64_t)i * 0x30 + 8, &name, 8);
        printf("Coop: netinit: skins 0-2 of %d -> %s\n", count, (const char *)(uintptr_t)name);
    }
    /* Missing DC symbols: the multiplayer variants of several tables were in DC modules the
     * Collection lacks (e.g. 0x837a26b6, the MP twin of 0x974cf651 read at 0xa61000). The
     * lookup (0xa15e70) prints "LookupSymbolValue: Symbol ... not found", asserts and then
     * returns NULL; most callers test for NULL. While a net game runs the assert is skipped
     * (0xa15ed2: jmp to the "return NULL" at 0xa15f27); the message stays in boot.log. */
    {
        static const unsigned char before[2] = {0x48, 0x8b}, after[2] = {0xeb, 0x53};
        if (!memcmp(image + 0xa15ed2, before, 2)) {
            wr((uint64_t)(uintptr_t)(image + 0xa15ed2), after, 2);
            puts("Coop: netinit: missing DC symbols return NULL without the assert (0xa15ed2)");
        }
    }
    printf("Coop: netinit: MP DC twins -> SP tables %s\n", u2_patch_dc_twins() ? "patched (0xa15e95)" : "NOT patched");
    /* Co-op needs 2 players (0x63f60b: mov ecx, 2 -> the host ends the game, reason 3, when
     * fewer are in); GOW3_COOP_SOLO=1 makes it 1 so one player can play a co-op map alone. */
    {
        const char *solo = getenv("GOW3_COOP_SOLO");
        static const unsigned char two[5] = {0xb9, 0x02, 0x00, 0x00, 0x00};
        if (solo && solo[0] == '1' && !memcmp(image + 0x63f60b, two, 5)) {
            const uint8_t one = 1;
            wr((uint64_t)(uintptr_t)(image + 0x63f60c), &one, 1);
            puts("Coop: netinit: co-op minimum players 2 -> 1 (GOW3_COOP_SOLO)");
        }
    }
    /* The remote player's NetCharacter process runs on its own heap (process spawn 0x9b5c20
     * allocates the type's registered size and pushes it as the allocator): 128 KiB, registered
     * at 0x6075a4 (type node 0x14cee28: +0x10 id 0xf2c86c22, +0x18 size). Sized for the MP
     * player models; Drake's single-player actors (hero-parka / hero-default) need more ("Allocation
     * failed: 40 bytes ... NetCharacter [F:16 T:131072]" in NetCharacter init 0x5fd5ce). The
     * local Player process gets 512 KiB (0x979ae0); so does the NetCharacter now. */
    {
        const uint64_t node = I + 0x14cee28;
        uint32_t id = 0;
        uint64_t size = 0;
        rd(node + 0x10, &id, 4);
        rd(node + 0x18, &size, 8);
        if (id == 0xf2c86c22u && size == 0x20000) {
            const uint64_t bigger = 0x80000;
            wr(node + 0x18, &bigger, 8);
            puts("Coop: netinit: NetCharacter process heap 128 -> 512 KiB (type node 0x14cee28)");
        } else if (size != 0x80000)
            printf("Coop: netinit: NetCharacter type node not as expected (id %#x size %#llx)\n", id, (unsigned long long)size);
    }
    /* The player-setup event (type 0x12) on a joiner first clears every slot (0x632148: call
     * 0x63db90) and then fills the slots it finds by NpId; the removed session layer had
     * re-added the members in between. Ours keeps the slots (local + host, with their NpIds),
     * so the clear is skipped: without it the joiner lost its own slot and sent its ready
     * event with player id 0, and the host waited in WaitReady for ever (run q1). */
    {
        static const unsigned char call_clear[5] = {0xe8, 0x43, 0xba, 0x00, 0x00};
        static const unsigned char nops[5] = {0x0f, 0x1f, 0x44, 0x00, 0x00};
        if (!memcmp(image + 0x632148, call_clear, 5)) {
            wr((uint64_t)(uintptr_t)(image + 0x632148), nops, 5);
            puts("Coop: netinit: player-setup event keeps the joiner's slots (0x632148)");
        }
    }
    /* Snapshot parse (net-snapshot.cpp, 0x66b430, run by the NetCharacter's animation source
     * 0x5f3ca0): the joiner's NetCharacter for the host died in it at once, first on
     * "ASSERTION: pAnimArtGroup" (run q2, int 0x41 at 0x66c033: an art group id that is not
     * loaded), then, with that one skipped, on "m_maxBits >= m_numBits" (run q3, 0x66bc65: the
     * parse reads past the snapshot's bytes). The host's Player writes its own animation state
     * (0x634970); the parse expects what an MP player writes (art group dictionary 0 at
     * 0x1041800: proto, anim-proto-multiplayer, ...). Until that is matched, every assert in the
     * parse (21 blocks, each starting with `mov rax, [rip -> 0x156df78]`, the assert hook)
     * jumps to a counter and the parse's own epilogue (0x66c380): the snapshot's animation part
     * is dropped instead of killing the game. The count shows in the 5 s coop line. */
    {
        static __attribute__((aligned(64))) volatile uint32_t parse_aborts;
        coop_parse_aborts = &parse_aborts;
        /* one 32-byte stub per assert block: counts the total and its own block, then the epilogue */
        unsigned char *page = page_near_image();
        int patched = 0;
        for (uint64_t at = 0x66b430; page && at + 7 <= 0x66c380 && patched < COOP_PARSE_SITES; ++at) {
            const unsigned char *p = image + at;
            if (p[0] != 0x48 || p[1] != 0x8b || p[2] != 0x05) continue;
            int32_t disp;
            memcpy(&disp, p + 3, 4);
            if (at + 7 + (int64_t)disp != 0x156df78) continue;
            unsigned char *stub = page + patched * 32;
            unsigned n = 0;
            stub[n++] = 0x48; stub[n++] = 0xb8;                           /* mov rax, &parse_aborts */
            uint64_t a = (uint64_t)(uintptr_t)&parse_aborts;
            memcpy(stub + n, &a, 8); n += 8;
            stub[n++] = 0xf0; stub[n++] = 0xff; stub[n++] = 0x00;         /* lock inc dword [rax] */
            stub[n++] = 0x48; stub[n++] = 0xb8;                           /* mov rax, &its own count */
            a = (uint64_t)(uintptr_t)&coop_parse_site_count[patched];
            memcpy(stub + n, &a, 8); n += 8;
            stub[n++] = 0xf0; stub[n++] = 0xff; stub[n++] = 0x00;         /* lock inc dword [rax] */
            const int64_t back = (int64_t)(uintptr_t)(image + 0x66c380) - (int64_t)(uintptr_t)(stub + n + 5);
            const int64_t rel = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + at + 5);
            if (back < INT32_MIN || back > INT32_MAX || rel < INT32_MIN || rel > INT32_MAX) break;
            const int32_t b = (int32_t)back;
            stub[n++] = 0xe9; memcpy(stub + n, &b, 4); n += 4;            /* jmp 0x66c380 (epilogue) */
            unsigned char jump[7] = {0xe9, 0, 0, 0, 0, 0x90, 0x90};
            const int32_t r = (int32_t)rel;
            memcpy(jump + 1, &r, 4);
            if (!wr((uint64_t)(uintptr_t)(image + at), jump, 7)) break;
            coop_parse_site_at[patched++] = (uint32_t)at;
            at += 6;
        }
        if (page) {
            DWORD old;
            VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);
        }
        coop_parse_sites = patched;
        printf("Coop: netinit: snapshot parse: %d asserts end the parse instead (0x66b430..0x66c380)\n", patched);
    }
    /* HavokApplyHardKeyFrameAndSync (0x527b20): a NaN target position asserts (0x527f49, run q5:
     * both games, a body in the NetCharacter's process heap, as soon as real snapshots arrived).
     * While that is investigated, a NaN target skips the keyframe (counter, then the function's
     * epilogue 0x528aef) so both games stay up. */
    {
        static const unsigned char mov_hook[3] = {0x48, 0x8b, 0x05};
        static __attribute__((aligned(64))) volatile uint32_t nan_skips;
        unsigned char *stub = !memcmp(image + 0x527f49, mov_hook, 3) ? page_near_image() : NULL;
        if (stub) {
            unsigned n = 0;
            stub[n++] = 0x48; stub[n++] = 0xb8;
            const uint64_t a = (uint64_t)(uintptr_t)&nan_skips;
            memcpy(stub + n, &a, 8); n += 8;
            stub[n++] = 0xf0; stub[n++] = 0xff; stub[n++] = 0x00;             /* lock inc dword [rax] */
            const int32_t b = (int32_t)((int64_t)(uintptr_t)(image + 0x528aef) - (int64_t)(uintptr_t)(stub + n + 5));
            stub[n++] = 0xe9; memcpy(stub + n, &b, 4); n += 4;
            DWORD old;
            VirtualProtect(stub, 4096, PAGE_EXECUTE_READ, &old);
            unsigned char jump[7] = {0xe9, 0, 0, 0, 0, 0x90, 0x90};
            const int32_t r = (int32_t)((int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + 0x527f49 + 5));
            memcpy(jump + 1, &r, 4);
            if (wr((uint64_t)(uintptr_t)(image + 0x527f49), jump, 7)) {
                coop_nan_skips = &nan_skips;
                puts("Coop: netinit: NaN keyframe targets are skipped (0x527f49)");
            }
        }
    }
    /* The NetCharacter's animation from snapshots (0x5f3ca0, needs >= 3 buffered) still breaks
     * on the receiver (run q6: parse overruns, then NaN joints) although the host's own snapshot
     * parses cleanly through writer -> Serialize -> Deserialize -> parse in one process (run
     * t43). Until that is found, the animation source always takes its "too few snapshots"
     * branch (0x5f3ccf jb -> jmp 0x5f3e34: the default idle anim 0x908bb9ef of 'proto'); the
     * root locator still comes from the snapshots (0x5f3f70), so the remote player moves.
     * GOW3_COOP_NETANIM=1 keeps the snapshot animation (for that investigation). 25 Oct (runs f1,
     * f1b, all 342 bit writers fixed): still breaks, now as ORBISANIM_JOB_ASSERT offsets past the
     * NetCharacter's numJoints, then the havok fault at 0x512f44.
     * 25 Oct 14:00: cause found (this build's parse never plays its anim leaves and the writer sends
     * no phases); the fix and the 0x5f3ccf choice live in runtime_coop_mp.c (install_netanim, called
     * from runtime_coop_mp_netinit below). GOW3_COOP_NETANIM=0 keeps the default pose. */
    if (!rq(I + U2_SESSION + 0x10)) {
        const uint64_t standin = (uint64_t)(uintptr_t)net_session_standin;
        wr(I + U2_SESSION + 0x10, &standin, 8);
        wr(I + U2_SESSION + 0xc8, &one8, 1);
        printf("Coop: netinit: session stand-in %llx (0x1065b08+0x10), joined flag +0xc8 set\n", (unsigned long long)standin);
    }
    int32_t money = 0;
    rd(I + 0x1139fe0, &money, 4);
    if (!money) { const int32_t five = 5; wr(I + 0x1139fe0, &five, 4); }
    if (!rq(I + 0x113a040)) {
        memset(net_clock, 0, sizeof(net_clock));
        const float one = 1.0f, dt = 1.0f / 60.0f;
        memcpy(net_clock + 4, &one, 4);
        memcpy(net_clock + 0x14, &dt, 4);
        net_clock_start = GetTickCount64();
        u2_net_clock_tick();
        const uint64_t clock = (uint64_t)(uintptr_t)net_clock;
        wr(I + 0x113a040, &clock, 8);
        printf("Coop: netinit: net clock at %llx (0x113a040)\n", (unsigned long long)clock);
    }
    if (!rq(I + 0x113a258)) {
        const uint64_t buf = (uint64_t)(uintptr_t)net_record;
        const uint32_t zero = 0, capacity = (uint32_t)(sizeof(net_record) * 8);
        wr(I + 0x113a258, &buf, 8);
        wr(I + 0x113a258 + 8, &zero, 4);
        wr(I + 0x113a258 + 0xc, &capacity, 4);
        printf("Coop: netinit: record bitstream %llx, %u bits (0x113a258)\n", (unsigned long long)buf, capacity);
    }
    printf("Coop: netinit: net game %ld, local player in slot 0 (id 1, host)\n", game);
    { extern void runtime_coop_mp_netinit(unsigned char *, long); runtime_coop_mp_netinit(image, game); } /* runtime_coop_mp.c (own knobs) */
    return 1;
}

static void u2_pkg_list(uint64_t mgr, uint64_t count_at, uint64_t array_at, const char *label) {
    int32_t n = 0;
    rd(mgr + count_at, &n, 4);
    const uint64_t arr = rq(mgr + array_at);
    const uint64_t buckets = rq(mgr + 0x10);
    int32_t nb = 0;
    rd(mgr + 0x4c, &nb, 4);
    int waiting = 0;
    for (int i = 0; i < n && arr && i < 4096; ++i) {
        uint32_t id = 0, flags = 0;
        rd(arr + (uint64_t)i * 16 + 8, &id, 4);
        rd(arr + (uint64_t)i * 16 + 0xc, &flags, 4);
        /* the hash: FNV-1a over the 4 id bytes, modulo the bucket count */
        uint32_t h = 0x811c9dc5u;
        for (int k = 0; k < 4; ++k) h = (h ^ ((id >> (8 * k)) & 0xff)) * 0x1000193u;
        uint64_t node = nb > 0 ? rq(buckets + (uint64_t)(h % (uint32_t)nb) * 8) : 0, rec = 0;
        for (int guard = 0; node && guard < 4096; ++guard, node = rq(node)) {
            uint32_t key = 0;
            rd(node + 0x10, &key, 4);
            if (key == id) { rec = rq(node + 0x18); break; }
        }
        int32_t state = -1;
        uint32_t rflags = 0;
        if (rec) { rd(rec + 0x30, &state, 4); rd(rec + 0x18, &rflags, 4); }
        if ((flags & 1) && label[0] == 'a') continue;
        if (rec && state == 7 && (rflags & 0x80)) continue;
        ++waiting;
        printf("Coop: pkgwait %s %d id %08x flags %x record %llx state %d rflags %x\n", label, i, id, flags,
               (unsigned long long)rec, state, rflags);
    }
    printf("Coop: pkgwait %s: %d entries, %d not in\n", label, n, waiting);
}
static int u2_helper(const char *op, const char *p) {
    if (!strcmp(op, "levels")) {
        const uint64_t base = rq((uint64_t)(uintptr_t)image + U2_PACKAGES);
        for (int i = 0; i < 180 && base; ++i) {
            const uint64_t r = base + (uint64_t)i * 0xa60;
            uint32_t id = 0;
            rd(r + 0x74, &id, 4);
            if (!id) continue;
            const uint64_t t = rq(r + 0x9a0);
            int32_t n = 0;
            if (t) rd(t + 0x20, &n, 4);
            printf("Coop: level %d id %08x at %llx spawners %d\n", i, id, (unsigned long long)r, n);
        }
        return 1;
    }
    if (!strcmp(op, "spawners")) {
        const uint64_t r = u2_record(skip(p));
        const uint64_t t = r ? rq(r + 0x9a0) : 0;
        int32_t n = 0;
        if (t) rd(t + 0x20, &n, 4);
        const uint64_t arr = t ? rq(t + 0x18) : 0;
        printf("Coop: spawners of %llx: %d\n", (unsigned long long)r, n);
        for (int i = 0; i < n && arr; ++i) {
            uint32_t id = 0;
            rd(arr + (uint64_t)i * 16, &id, 4);
            const uint64_t sp = rq(arr + (uint64_t)i * 16 + 8);
            float *loc = (float *)scratch;
            memset(loc, 0, 32);
            if (sp) ((U2Locator)(image + 0x4bdfe0))(loc, sp);
            printf("Coop: spawner %08x at %llx pos %.2f %.2f %.2f rot %.3f %.3f %.3f %.3f\n", id,
                   (unsigned long long)sp, loc[0], loc[1], loc[2], loc[4], loc[5], loc[6], loc[7]);
        }
        return 1;
    }
    if (!strcmp(op, "netinit")) {
        /* "netinit auto [MiB]": the net game from runtime_net (the host's GOW3_NET_GAME, or the
         * one in the host's welcome on a joiner; run "netwait" first). */
        const char *a = skip(p);
        if (!strncmp(a, "auto", 4) && is_end(a[4])) {
            extern int runtime_net_game(void);
            const int game = runtime_net_game();
            if (game < 0) { puts("Coop: netinit auto: no net game known (GOW3_NET_GAME / netwait)"); return 1; }
            char args[64];
            snprintf(args, sizeof(args), "%d %s", game, skip(a + 4));
            return u2_netinit(args);
        }
        return u2_netinit(p);
    }
    if (!strcmp(op, "pkgwait")) {
        const uint64_t mgr = (uint64_t)(uintptr_t)image + U2_PACKAGES;
        u2_pkg_list(mgr, 0x5a98, 0x5aa0, "a");
        u2_pkg_list(mgr, 0x5aa8, 0x5ab0, "b");
        return 1;
    }
    if (!strcmp(op, "dcwait")) {
        const uint64_t head = (uint64_t)(uintptr_t)image + 0x1534140;
        int n = 0, bad = 0;
        for (uint64_t node = rq(head); node && node != head && n < 4096; node = rq(node), ++n) {
            int32_t state = 0;
            uint32_t id = 0;
            rd(node + 0x10, &state, 4);
            rd(node + 0x98, &id, 4);
            if (state != 8) { ++bad; printf("Coop: dcwait module %08x state %d\n", id, state); }
        }
        printf("Coop: dcwait: %d modules, %d not loaded\n", n, bad);
        return 1;
    }
    if (!strcmp(op, "galloc")) {
        int ok = 1;
        const uint32_t ctx = (uint32_t)expr(&p, &ok);
        const uint64_t size = expr(&p, &ok);
        last_rax = ok ? u2_alloc(ctx, size) : 0;
        printf("Coop: galloc ctx %#x size %#llx -> %llx\n", ctx, (unsigned long long)size, (unsigned long long)last_rax);
        return 1;
    }
    if (!strcmp(op, "spawncam")) {
        ((U2Void)(image + 0x969320))();
        printf("Coop: spawncam done\n");
        return 1;
    }
    if (!strcmp(op, "spawnat")) {
        p = skip(p);
        const uint32_t id = name_or_id(p);
        while (!is_end(*p)) ++p;
        p = skip(p);
        static char art[64];
        size_t n = 0;
        while (!is_end(p[n]) && n < 63) { art[n] = p[n]; ++n; }
        art[n] = 0;
        if (!n) strcpy(art, "hero-default");
        uint64_t sp = ((U2Lookup)(image + 0x56c7b0))((uint64_t)(uintptr_t)image + U2_PACKAGES, id);
        if (!sp) sp = u2_find_spawner(id);
        if (!sp) { printf("Coop: spawnat: spawner %08x not found\n", id); return 1; }
        float *loc = (float *)scratch;
        ((U2Locator)(image + 0x4bdfe0))(loc, sp);
        float *loc2 = (float *)(scratch + 64);
        memcpy(loc2, loc, 32);
        /* the player, if any, goes first (the debug toggle's kill path) */
        if (rq((uint64_t)(uintptr_t)image + U2_PLAYER) &&
            (uint32_t)rq((uint64_t)(uintptr_t)image + U2_PLAYER + 8)) ((U2Void)(image + 0x9691d0))();
        const uint64_t weapons = ((U2Dc)(image + 0xa15e70))(0xc66bbbc7u); /* *player-debug-weapons* */
        const GuestPair2 h = ((U2Spawn)(image + 0x975190))(loc, loc2, art, weapons, 0, 0, 0); /* spawner 0: with it the spawn fails */
        printf("Coop: spawnat %08x (%s) pos %.2f %.2f %.2f -> %llx %llx\n", id, art, loc[0], loc[1], loc[2],
               (unsigned long long)h.rax, (unsigned long long)h.rdx);
        return 1;
    }
    return 0;
}

static char pending[16384];
static int pending_wait;

static void run_line(const char *line) {
    char op[16] = {0};
    const char *p = skip(line);
    int k = 0;
    while (!is_end(*p) && k < 15) op[k++] = *p++;
    op[k] = 0;
    if (!op[0] || op[0] == '#') return;
    int ok = 1;
    if (!strcmp(op, "echo")) { printf("Coop: %s\n", skip(p)); return; }
    if (!strcmp(op, "mon")) {
        p = skip(p);
        if (!strncmp(p, "clear", 5)) { monitor_count = 0; puts("Coop: monitors cleared"); return; }
        if (monitor_count >= 24) return;
        Monitor *m = &monitors[monitor_count];
        if (sscanf(p, "%23s %3s %95[^\r\n]", m->name, m->fmt, m->text) == 3) ++monitor_count;
        return;
    }
    if (!strcmp(op, "r") || !strcmp(op, "rd") || !strcmp(op, "rq") || !strcmp(op, "rf")) {
        const uint64_t a = expr(&p, &ok);
        p = skip(p);
        uint64_t n = *p ? strtoull(p, NULL, 0) : (op[1] ? 4 : 64);
        if (!ok) { printf("Coop: bad address in '%s'\n", line); return; }
        unsigned char buf[512];
        const size_t unit = op[1] == 'd' || op[1] == 'f' ? 4 : op[1] == 'q' ? 8 : 1;
        if (n * unit > sizeof(buf)) n = sizeof(buf) / unit;
        if (!rd(a, buf, n * unit)) { printf("Coop: %llx not readable\n", (unsigned long long)a); return; }
        for (uint64_t i = 0; i < n * unit; i += 32) {
            char out[400];
            int m = snprintf(out, sizeof(out), "Coop: %llx:", (unsigned long long)(a + i));
            for (uint64_t j = i; j < i + 32 && j < n * unit; j += unit) {
                if (unit == 1) m += snprintf(out + m, sizeof(out) - m, " %02x", buf[j]);
                else if (op[1] == 'd') { uint32_t v; memcpy(&v, buf + j, 4); m += snprintf(out + m, sizeof(out) - m, " %08x", v); }
                else if (op[1] == 'f') { float v; memcpy(&v, buf + j, 4); m += snprintf(out + m, sizeof(out) - m, " %.4g", v); }
                else { uint64_t v; memcpy(&v, buf + j, 8); m += snprintf(out + m, sizeof(out) - m, " %016llx", (unsigned long long)v); }
            }
            puts(out);
        }
        return;
    }
    if (!strcmp(op, "dump")) {
        const uint64_t a = expr(&p, &ok);
        const uint64_t n = expr(&p, &ok);
        p = skip(p);
        char path[260];
        int k2 = 0;
        while (*p && *p != '\r' && *p != '\n' && k2 < 259) path[k2++] = *p++;
        path[k2] = 0;
        FILE *f = ok && n <= (UINT64_C(64) << 20) ? fopen(path, "wb") : NULL;
        if (!f) { printf("Coop: dump failed '%s'\n", line); return; }
        static unsigned char chunk[65536];
        uint64_t bad = 0;
        for (uint64_t at = 0; at < n; at += sizeof(chunk)) {
            const size_t c = n - at < sizeof(chunk) ? (size_t)(n - at) : sizeof(chunk);
            if (!rd(a + at, chunk, c)) {
                /* page by page */
                for (size_t q = 0; q < c; q += 4096) {
                    const size_t m = c - q < 4096 ? c - q : 4096;
                    if (!rd(a + at + q, chunk + q, m)) { memset(chunk + q, 0, m); bad += m; }
                }
            }
            fwrite(chunk, 1, c, f);
        }
        fclose(f);
        printf("Coop: dumped %llx+%llx to %s (%llu bytes unreadable)\n", (unsigned long long)a,
               (unsigned long long)n, path, (unsigned long long)bad);
        return;
    }
    if (!strcmp(op, "find32")) {
        const uint64_t a = expr(&p, &ok);
        const uint64_t n = expr(&p, &ok);
        const uint32_t v = (uint32_t)expr(&p, &ok);
        if (!ok) { printf("Coop: bad find32 '%s'\n", line); return; }
        static uint32_t chunk[16384];
        int hits = 0;
        for (uint64_t at = 0; at < n && hits < 64; at += sizeof(chunk)) {
            const size_t c = n - at < sizeof(chunk) ? (size_t)(n - at) : sizeof(chunk);
            if (!rd(a + at, chunk, c)) continue;
            for (size_t i = 0; i < c / 4 && hits < 64; ++i)
                if (chunk[i] == v) { printf("Coop: find32 %x at %llx\n", v, (unsigned long long)(a + at + i * 4)); ++hits; }
        }
        printf("Coop: find32 %x: %d matches\n", v, hits);
        return;
    }
    if (!strcmp(op, "rs")) {
        const uint64_t a = expr(&p, &ok);
        show("string", "s", a);
        return;
    }
    if (op[0] == 'w') {
        const uint64_t a = expr(&p, &ok);
        p = skip(p);
        int done = 0;
        if (!strcmp(op, "wf")) { float f = strtof(p, NULL); done = wr(a, &f, 4); }
        else {
            const uint64_t v = expr(&p, &ok);
            const size_t n = !strcmp(op, "w8") ? 1 : !strcmp(op, "w16") ? 2 : !strcmp(op, "w32") ? 4 : 8;
            if (ok) done = wr(a, &v, n);
        }
        printf("Coop: %s %llx %s\n", op, (unsigned long long)a, done ? "ok" : "FAILED");
        return;
    }
    if (!strcmp(op, "call")) {
        const uint64_t fn = expr(&p, &ok);
        uint64_t a[8] = {0};
        float f[4] = {0};
        int na = 0, nf = 0;
        for (p = skip(p); *p && *p != '\r' && *p != '\n'; p = skip(p)) {
            if (!strncmp(p, "f:", 2)) {
                char *end;
                const float v = strtof(p + 2, &end);
                if (nf < 4) f[nf++] = v;
                p = end;
            } else {
                const uint64_t v = expr(&p, &ok);
                if (na < 8) a[na++] = v;
            }
            if (!ok) break;
        }
        if (!ok || !fn) { printf("Coop: bad call '%s'\n", line); return; }
        printf("Coop: call %llx (%d args)\n", (unsigned long long)fn, na);
        fflush(stdout);
        const GuestPair r = ((GuestCall)(uintptr_t)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], f[0], f[1], f[2], f[3]);
        last_rax = r.rax;
        last_rdx = r.rdx;
        printf("Coop: call %llx -> rax %#llx rdx %#llx\n", (unsigned long long)fn, (unsigned long long)r.rax, (unsigned long long)r.rdx);
        return;
    }
    if (u2_helper(op, p)) return;
    { extern int runtime_coop_mp_command(unsigned char *, const char *, const char *); if (runtime_coop_mp_command(image, op, p)) return; } /* runtime_coop_mp.c */
    { extern int runtime_coop_menu_command(unsigned char *, const char *, const char *); if (runtime_coop_menu_command(image, op, p)) return; } /* runtime_coop_menu.c (mpmenu, u3) */
    printf("Coop: unknown command '%s'\n", op);
}

/* Runs lines from text; a "wait N" keeps the rest for N frames later. */
static void run_text(const char *text) {
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        char line[512];
        if (n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, p, n);
        line[n] = 0;
        p = e ? e + 1 : p + n;
        const char *q = skip(line);
        if (!strncmp(q, "wait", 4) && is_end(q[4])) {
            pending_wait = atoi(q + 4);
            snprintf(pending, sizeof(pending), "%s", p);
            return;
        }
        /* netwait: the remaining lines run once runtime_net knows our player id and the net
         * game (at once on a host, after the host's welcome on a joiner; checked every 30
         * frames). For the launcher's Multiplayer page: "netwait" then "netinit auto". */
        if (!strncmp(q, "netwait", 7) && is_end(q[7])) {
            extern int runtime_net_ready(void);
            if (!runtime_net_ready()) {
                static int said;
                if (!said++) puts("Coop: netwait: waiting for the host's welcome");
                pending_wait = 30;
                snprintf(pending, sizeof(pending), "%s\n%s", line, p);
                return;
            }
            continue;
        }
        run_line(line);
    }
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    static char text[16384];
    const size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    return text;
}

static void u2_net_clock_tick(void);
static void u2_net_peer_frame(void);
static void ABI coop_frame(void) {
    ++frames;
    u2_net_clock_tick();
    runtime_net_frame(image);
    u2_net_peer_frame();
    { extern void runtime_coop_menu_tick(unsigned char *); runtime_coop_menu_tick(image); } /* mpmenu commands waiting for the menu hook */
    {   /* GOW3_SPAWN_RECORD=<file> (25 Oct, the user picks spawn points for the maps that ship none:
         * Ice Cave, Train Wreck, Temple): F8 appends "<net game> x y z qx qy qz qw" of the local
         * player (process +0x110 position, +0x120 rotation) and beeps. */
        static const char *record = (const char *)1;
        if (record == (const char *)1) record = getenv("GOW3_SPAWN_RECORD");
        if (record && *record && (GetAsyncKeyState(VK_F8) & 1)) {
            const uint64_t h = (uint64_t)(uintptr_t)image + U2_PLAYER;
            const uint64_t ptr = rq(h), proc = ptr ? rq(ptr) : 0;
            const uint32_t id = (uint32_t)rq(h + 8);
            float v[8] = {0};
            int16_t game = -1;
            rd((uint64_t)(uintptr_t)image + U2_NGM + 0x1ca0, &game, 2);
            if (proc && id && (uint32_t)rq(proc + 0x44) == id && rd(proc + 0x110, v, sizeof(v))) {
                FILE *f = fopen(record, "a");
                if (f) {
                    fprintf(f, "%d %.3f %.3f %.3f %.4f %.4f %.4f %.4f\n", game, v[0], v[1], v[2], v[4], v[5], v[6], v[7]);
                    fclose(f);
                }
                printf("Coop: spawn point recorded (net game %d): %.2f %.2f %.2f\n", game, v[0], v[1], v[2]);
                MessageBeep(MB_OK);
            } else {
                puts("Coop: spawn point: no player right now");
            }
            fflush(stdout);
        }
    }
    if (coop_parse_aborts) {
        static ULONGLONG last_abort_line;
        static uint32_t reported;
        const ULONGLONG t = GetTickCount64();
        if (t - last_abort_line >= 5000 && *coop_parse_aborts != reported) {
            last_abort_line = t;
            printf("Coop: snapshot parses ended early: %u (+%u); by assert block:", *coop_parse_aborts, *coop_parse_aborts - reported);
            for (int i = 0; i < coop_parse_sites; ++i)
                if (coop_parse_site_count[i]) printf(" %x=%u", coop_parse_site_at[i], coop_parse_site_count[i]);
            printf("\n");
            reported = *coop_parse_aborts;
            fflush(stdout);
        }
    }
    { extern void runtime_coop_mp_frame(void); runtime_coop_mp_frame(); } /* runtime_coop_mp.c: remote animation lines */
    if (coop_nan_skips) {
        static ULONGLONG last_nan_line;
        static uint32_t reported_nan;
        const ULONGLONG t = GetTickCount64();
        if (t - last_nan_line >= 5000 && *coop_nan_skips != reported_nan) {
            last_nan_line = t;
            printf("Coop: NaN keyframe targets skipped: %u (+%u)\n", *coop_nan_skips, *coop_nan_skips - reported_nan);
            reported_nan = *coop_nan_skips;
            fflush(stdout);
        }
    }
    static ULONGLONG last_check, last_monitor;
    static FILETIME last_write;
    static int boot_done;
    const ULONGLONG now = GetTickCount64();
    if (pending_wait > 0 && --pending_wait == 0) {
        static char text[sizeof(pending)];
        memcpy(text, pending, sizeof(text));
        pending[0] = 0;
        run_text(text);
        fflush(stdout);
    }
    if (boot_path && !boot_done) {
        static char boot_text[16384], *boot;
        static unsigned long long at = ~0ull;
        if (!boot) {
            const char *text = read_file(boot_path);
            if (text) {
                snprintf(boot_text, sizeof(boot_text), "%s", text);
                boot = boot_text;
                if (!strncmp(boot, "at ", 3)) at = strtoull(boot + 3, NULL, 10);
            } else boot_done = 1;
        }
        if (boot && frames >= at) {
            boot_done = 1;
            static char text[16384];
            snprintf(text, sizeof(text), "%s", strchr(boot, '\n') ? strchr(boot, '\n') + 1 : "");
            printf("Coop: boot commands at frame %llu\n", frames);
            string_count = 0;
            run_text(text);
            fflush(stdout);
        }
    }
    if (cmd_path && now - last_check >= 250) {
        last_check = now;
        WIN32_FILE_ATTRIBUTE_DATA info;
        if (GetFileAttributesExA(cmd_path, GetFileExInfoStandard, &info) &&
            CompareFileTime(&info.ftLastWriteTime, &last_write)) {
            last_write = info.ftLastWriteTime;
            char *text = read_file(cmd_path);
            if (text) {
                printf("Coop: commands at frame %llu\n", frames);
                string_count = 0;
                run_text(text);
                fflush(stdout);
            }
        }
    }
    if (monitor_count && now - last_monitor >= 1000) {
        last_monitor = now;
        printf("Coop: frame %llu\n", frames);
        for (int i = 0; i < monitor_count; ++i) {
            const char *p = monitors[i].text;
            int ok = 1;
            const uint64_t a = expr(&p, &ok);
            if (!strcmp(monitors[i].fmt, "h")) {
                printf("Coop: %s = %#llx\n", monitors[i].name, (unsigned long long)a);
                continue;
            }
            if (ok) show(monitors[i].name, monitors[i].fmt, a);
        }
        fflush(stdout);
    }
}

/* Stub: saves rax, rcx, rdx, rsi, rdi, r8-r11 and xmm0-7, calls the handler (rsp 16-byte aligned:
 * the entry's rsp is 8 mod 16, 9 pushes make it 0), restores them, replays the replaced bytes and
 * jumps back. */
static unsigned build_stub(unsigned char *p, void *handler, const unsigned char *replayed, unsigned length, uintptr_t resume) {
    static const unsigned char save[] = {
        0x50, 0x51, 0x52, 0x56, 0x57, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53,
        0x48, 0x81, 0xec, 0x80, 0x00, 0x00, 0x00,
        0xf3, 0x0f, 0x7f, 0x04, 0x24,       0xf3, 0x0f, 0x7f, 0x4c, 0x24, 0x10,
        0xf3, 0x0f, 0x7f, 0x54, 0x24, 0x20, 0xf3, 0x0f, 0x7f, 0x5c, 0x24, 0x30,
        0xf3, 0x0f, 0x7f, 0x64, 0x24, 0x40, 0xf3, 0x0f, 0x7f, 0x6c, 0x24, 0x50,
        0xf3, 0x0f, 0x7f, 0x74, 0x24, 0x60, 0xf3, 0x0f, 0x7f, 0x7c, 0x24, 0x70,
    };
    static const unsigned char restore[] = {
        0xf3, 0x0f, 0x6f, 0x04, 0x24,       0xf3, 0x0f, 0x6f, 0x4c, 0x24, 0x10,
        0xf3, 0x0f, 0x6f, 0x54, 0x24, 0x20, 0xf3, 0x0f, 0x6f, 0x5c, 0x24, 0x30,
        0xf3, 0x0f, 0x6f, 0x64, 0x24, 0x40, 0xf3, 0x0f, 0x6f, 0x6c, 0x24, 0x50,
        0xf3, 0x0f, 0x6f, 0x74, 0x24, 0x60, 0xf3, 0x0f, 0x6f, 0x7c, 0x24, 0x70,
        0x48, 0x81, 0xc4, 0x80, 0x00, 0x00, 0x00,
        0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5f, 0x5e, 0x5a, 0x59, 0x58,
    };
    unsigned n = 0;
    memcpy(p + n, save, sizeof(save)); n += sizeof(save);
    p[n++] = 0x48; p[n++] = 0xb8; /* mov rax, handler */
    const uint64_t h = (uint64_t)(uintptr_t)handler;
    memcpy(p + n, &h, 8); n += 8;
    p[n++] = 0xff; p[n++] = 0xd0; /* call rax */
    memcpy(p + n, restore, sizeof(restore)); n += sizeof(restore);
    memcpy(p + n, replayed, length); n += length;
    static const unsigned char jump[] = {0xff, 0x25, 0x00, 0x00, 0x00, 0x00}; /* jmp [rip+0] */
    memcpy(p + n, jump, sizeof(jump)); n += sizeof(jump);
    const uint64_t target = (uint64_t)resume;
    memcpy(p + n, &target, 8); n += 8;
    return n;
}

static unsigned char *page_near_image(void) {
    const uintptr_t lo = (uintptr_t)image > UINT64_C(0x60000000) ? (uintptr_t)image - UINT64_C(0x60000000) : 0x10000;
    const uintptr_t hi = (uintptr_t)image + (uintptr_t)image_size + UINT64_C(0x60000000);
    MEMORY_BASIC_INFORMATION info;
    for (uintptr_t address = lo; address < hi && VirtualQuery((void *)address, &info, sizeof(info));
         address = (uintptr_t)info.BaseAddress + info.RegionSize) {
        if (info.State != MEM_FREE) continue;
        const uintptr_t start = ((uintptr_t)info.BaseAddress + 0xffff) & ~(uintptr_t)0xffff;
        if (start + 4096 > (uintptr_t)info.BaseAddress + info.RegionSize) continue;
        void *p = VirtualAlloc((void *)start, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (p) return p;
    }
    return NULL;
}

/* probe.c calls this while the image is still writable (before its final protection). */
void runtime_coop_install(unsigned char *base, uint64_t size) {
    cmd_path = getenv("GOW3_COOP_CMD");
    boot_path = getenv("GOW3_COOP_BOOT");
    if ((!cmd_path || !*cmd_path) && (!boot_path || !*boot_path)) return;
    if (cmd_path && !*cmd_path) cmd_path = NULL;
    if (boot_path && !*boot_path) boot_path = NULL;
    image = base;
    image_size = size;
    int installed = 0;
    for (size_t i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i) {
        const CoopSite *s = &sites[i];
        if (s->offset + sizeof(s->expected) > size || memcmp(image + s->offset, s->expected, sizeof(s->expected))) continue;
        if (!s->frame && !installed) continue;  /* state hooks only with the frame hook (same game) */
        unsigned char *stub = page_near_image();
        if (!stub) { puts("Coop: no stub page in range"); return; }
        build_stub(stub, s->frame ? (void *)coop_frame : (void *)coop_establish, image + s->offset, s->length,
                   (uintptr_t)(image + s->offset + s->length));
        DWORD old;
        VirtualProtect(stub, 4096, PAGE_EXECUTE_READ, &old);
        const int64_t delta = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + s->offset + 5);
        if (delta < INT32_MIN || delta > INT32_MAX) { puts("Coop: stub out of range"); return; }
        const int32_t rel = (int32_t)delta;
        image[s->offset] = 0xe9;
        memcpy(image + s->offset + 1, &rel, 4);
        memset(image + s->offset + 5, 0x90, s->length - 5);
        if (s->frame)
            printf("Coop: %s frame hook at image offset %#llx (commands: %s, boot: %s)\n", s->game,
                   (unsigned long long)s->offset, cmd_path ? cmd_path : "-", boot_path ? boot_path : "-");
        else
            printf("Coop: %s state hook at image offset %#llx\n", s->game, (unsigned long long)s->offset);
        installed += s->frame;
    }
    if (!installed) puts("Coop: no known frame function in this executable");
}
#else
void runtime_coop_install(unsigned char *base, uint64_t size) { (void)base; (void)size; }
#endif
