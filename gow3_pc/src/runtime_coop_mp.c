/* gow3 25 Oct 2026: Uncharted 2 net games, the local player and competitive modes (research).
 *
 * Companion of runtime_coop.c (which owns the console, netinit and the frame hook). Everything
 * here is off unless its own knob is set; runtime_coop.c calls in at two points only:
 *   runtime_coop_mp_netinit(image, game)  at the end of its netinit command,
 *   runtime_coop_mp_command(op, args)     for console commands it does not know.
 *
 * Knobs (tests; since 25 Oct 11:00 the net game itself picks LEVELS / TEAMS / SKIN, and TWINS and
 * MENU_GUARD are on in every net game, so a joiner, who learns the net game only from the host's
 * welcome, gets the same settings: runtime_coop_mp_netinit). A knob that is set wins; "0" turns
 * TWINS / MENU_GUARD / TEAMS off:
 *   GOW3_COOP_MP_TWINS=1   more multiplayer DC symbols fall back to their single-player twin
 *                          (e.g. the MP explosion settings of every bullet, 0x3e2aeffe, which
 *                          faulted at 0xb011ac on the first shot). LookupSymbolValue (0xa15e70)
 *                          gets an entry detour: a symbol in the table below that is not loaded
 *                          is asked for as its twin. Extra pairs: GOW3_COOP_MP_TWINS=1,<mp>:<sp>,...
 *   GOW3_COOP_MP_LEVELS=<net pak level>[,<levels>]  the net game's map loads these levels and
 *                          takes its competitive spawners from the first (the classic maps' net
 *                          paks are gone; the-fort-multi / ruins-multi carry online spawners)
 *   GOW3_COOP_MP_TEAMS=1   team games: AssignTeams reads the local rank record (fault at 0x6405af)
 *   GOW3_COOP_MP_SKIN=<actor>  the default player art (every slot without its own character)
 *   GOW3_COOP_MP_MENU_GUARD=1  leaving a net game: menu pages that were never loaded are skipped
 *                          (fault at 0x5c7c92)
 *   GOW3_COOP_MP_RESULTS_SECONDS=<n>  how long the results stay up after a round (default 12;
 *                          0 = the game's own rule: joiners leave at once, the host waits for ever)
 * Per-player characters (25 Oct): each player's GOW3_NET_SKIN travels in our hello and the host's
 * player list (runtime_net.c); every machine gives slot k (player id k + 1) skin index k
 * (slot+0x138) in our own 10-entry copy of *net-skins* (the shipped table has 2 entries).
 * Tested 25 Oct (analysis/triage-1022/coop-mp-2.md): Fort deathmatch (net game 124) reaches Playing
 * with one player; the first shot no longer faults.
 * Always on in net games (netinit), opt-out GOW3_COOP_MP_KEEP_FG_HIDE=1: the online task's
 * "Disable Foreground Meshes" store is patched out, so players and pickups are drawn (run vis7).
 * Always on in net games, opt-out GOW3_COOP_MP_KEEP_MASK_BUG=1: the net bit writers' word mask at
 * bit offset 0 (remote player x stuck near -192, run vis13).
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

static __attribute__((unused)) int rd(uint64_t address, void *out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)address, out, n, &got) && got == n;
}
static int wr(uint64_t address, const void *in, size_t n) {
    SIZE_T put = 0;
    return WriteProcessMemory(GetCurrentProcess(), (LPVOID)(uintptr_t)address, in, n, &put) && put == n;
}
static int knob(const char *name) {
    const char *v = getenv(name);
    return v && v[0] == '1';
}

/* A page for stubs within +-2 GiB of the image (rel32 jumps). */
static unsigned char *stub_page(void) {
    static unsigned char *page;
    static unsigned used;
    if (page && used + 256 <= 4096) { unsigned char *p = page + used; used += 256; return p; }
    const uintptr_t lo = (uintptr_t)image > UINT64_C(0x60000000) ? (uintptr_t)image - UINT64_C(0x60000000) : 0x10000;
    const uintptr_t hi = (uintptr_t)image + UINT64_C(0x60000000) + UINT64_C(0x4000000);
    MEMORY_BASIC_INFORMATION info;
    for (uintptr_t address = lo; address < hi && VirtualQuery((void *)address, &info, sizeof(info));
         address = (uintptr_t)info.BaseAddress + info.RegionSize) {
        if (info.State != MEM_FREE) continue;
        const uintptr_t start = ((uintptr_t)info.BaseAddress + 0xffff) & ~(uintptr_t)0xffff;
        if (start + 4096 > (uintptr_t)info.BaseAddress + info.RegionSize) continue;
        void *p = VirtualAlloc((void *)start, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (p) { page = p; used = 256; return page; }
    }
    return NULL;
}
static int jump_to(uint64_t site, const unsigned char *stub, unsigned nops) {
    const int64_t delta = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + site + 5);
    if (delta < INT32_MIN || delta > INT32_MAX) return 0;
    unsigned char patch[16] = {0xe9};
    const int32_t r = (int32_t)delta;
    memcpy(patch + 1, &r, 4);
    memset(patch + 5, 0x90, nops);
    return wr((uint64_t)(uintptr_t)(image + site), patch, 5 + nops);
}

/* ---- MP -> SP DC symbol twins (GOW3_COOP_MP_TWINS) ---- */
static uint32_t twins[64][2] = {
    /* weapons.bin: every bullet names three explosion settings {SP, MP, ...}; the net game asks
     * for the MP one (explosions.bin has only the SP ones). Projectile Update 0xb01b70 ->
     * 0xb010b0 -> [proc vtable +0x228] -> 0xa15e70 -> NULL -> fault at 0xb011ac. */
    {0x3e2aeffeu, 0xe74d0f3cu},
};
static int twin_count = 1;
typedef uint64_t (ABI *U2Find)(uint32_t id);
static uint32_t ABI twin_remap(uint32_t id) {
    for (int i = 0; i < twin_count; ++i) {
        if (twins[i][0] != id) continue;
        if (((U2Find)(image + 0xa15300))(id)) return id;   /* loaded after all */
        static uint32_t told[64];
        if (!told[i]) {
            told[i] = 1;
            printf("CoopMP: DC symbol %08x not loaded -> twin %08x\n", id, twins[i][1]);
            fflush(stdout);
        }
        return twins[i][1];
    }
    return id;
}
static int install_twins(void) {
    static const unsigned char prologue[8] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56};
    const uint64_t site = 0xa15e70;
    if (memcmp(image + site, prologue, sizeof(prologue))) return image[site] == 0xe9;
    const char *v = getenv("GOW3_COOP_MP_TWINS");
    for (const char *p = v ? strchr(v, ',') : NULL; p && twin_count < 64; p = strchr(p + 1, ',')) {
        char *end;
        const uint32_t a = (uint32_t)strtoul(p + 1, &end, 16);
        if (*end != ':') continue;
        const uint32_t b = (uint32_t)strtoul(end + 1, NULL, 16);
        if (a && b) { twins[twin_count][0] = a; twins[twin_count][1] = b; ++twin_count; }
    }
    unsigned char *s = stub_page();
    if (!s) return 0;
    unsigned n = 0;
#define B(...) do { const unsigned char b_[] = {__VA_ARGS__}; memcpy(s + n, b_, sizeof(b_)); n += sizeof(b_); } while (0)
#define Q(v) do { const uint64_t q_ = (v); memcpy(s + n, &q_, 8); n += 8; } while (0)
    /* entry rsp = 8 mod 16; 8 pushes + 8 = 0 mod 16 for the call */
    B(0x50, 0x51, 0x52, 0x56, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53); /* push rax rcx rdx rsi r8-r11 */
    B(0x48, 0x83, 0xec, 0x08);                       /* sub rsp, 8 */
    B(0x48, 0xb8); Q((uint64_t)(uintptr_t)twin_remap); /* mov rax, twin_remap */
    B(0xff, 0xd0);                                   /* call rax (edi = id) */
    B(0x89, 0xc7);                                   /* mov edi, eax */
    B(0x48, 0x83, 0xc4, 0x08);                       /* add rsp, 8 */
    B(0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5e, 0x5a, 0x59, 0x58); /* pop r11-r8 rsi rdx rcx rax */
    memcpy(s + n, prologue, sizeof(prologue)); n += sizeof(prologue);           /* replayed */
    B(0xff, 0x25, 0x00, 0x00, 0x00, 0x00); Q((uint64_t)(uintptr_t)(image + site + 8)); /* jmp [rip] */
#undef B
#undef Q
    return jump_to(site, s, 3);
}

/* ---- the net game's map: levels and skins (GOW3_COOP_MP_LEVELS, GOW3_COOP_MP_SKIN) ---- */
/* ND string id: CRC-32 (polynomial 0x04c11db7, MSB first, initial value 0, no final xor). */
static uint32_t sid(const char *s) {
    uint32_t c = 0;
    for (; *s; ++s) {
        c ^= (uint32_t)(unsigned char)*s << 24;
        for (int k = 0; k < 8; ++k) c = c & 0x80000000u ? (c << 1) ^ 0x04c11db7u : c << 1;
    }
    return c;
}
typedef uint64_t (ABI *U2Dc)(uint32_t id);
#define U2_NGM 0x113b760
/* *net-games* (0x3b9a067d): 0x90-byte entries, +4 map sid, +0xc mode (0 Deathmatch, 1 Plunder,
 * 4 Co-op, 5 Turf War, 6 King of the Hill, 7 Chain Reaction, 8 Survival, 9 Gold Rush,
 * 10 Elimination, 11 Machinima: net.bin's mode table), +0x30 loadout.
 * *net-maps* (0xdec97a4): 0x38-byte entries, +0 map sid, +0x18 net pak level (string), +0x20 its
 * sid, +0x28 {u32 count, {char *level, u32 sid}[]}: StartNetGame (0x640fc0) loads the net pak and
 * the list (0x56c0e0); the competitive spawn choice (0x644220) and GetSpawnLocator (0x662fa0)
 * look for spawners tagged online-spawner (0x5bfae217) and team (0xd50bc34b) in the net pak's
 * record only (0x644130). The Collection lacks every "<map>-multi-pak" net pak except the
 * Stage's, but the DLC maps keep their online spawners in the main level (the-fort-multi: 37,
 * ruins-multi: 44; also dig-multi: 47), and several listed levels are not in the manifest.
 * GOW3_COOP_MP_LEVELS=<net pak level>[,<level>...] rewrites the net game's map entry: the first
 * level becomes the net pak (the spawners), the whole list the levels to load. */
static char mp_level_names[8][48];
static __attribute__((aligned(16))) struct { uint64_t name; uint32_t id, pad; } mp_level_list[8];
static __attribute__((aligned(16))) struct { uint32_t count, pad; uint64_t list; } mp_level_head;
static uint64_t u2_net_game_entry(long game) {
    const uint64_t tbl = ((U2Dc)(image + 0xa15e70))(0x3b9a067du);
    int32_t count = 0;
    if (!tbl || !rd(tbl, &count, 4) || game < 0 || game >= count) return 0;
    uint64_t entries = 0;
    rd(tbl + 8, &entries, 8);
    return entries ? entries + (uint64_t)game * 0x90 : 0;
}
/* The *net-maps* entry of a net game's map (0 if none). */
static uint64_t u2_net_map_entry(long game) {
    const uint64_t entry = u2_net_game_entry(game);
    if (!entry) return 0;
    uint32_t map = 0;
    rd(entry + 4, &map, 4);
    const uint64_t maps = ((U2Dc)(image + 0xa15e70))(0xdec97a4u);
    int32_t n = 0;
    uint64_t arr = 0;
    if (maps) { rd(maps, &n, 4); rd(maps + 8, &arr, 8); }
    for (int i = 0; i < n && arr; ++i) {
        uint32_t m = 0;
        rd(arr + (uint64_t)i * 0x38, &m, 4);
        if (m == map) return arr + (uint64_t)i * 0x38;
    }
    return 0;
}
static void apply_levels(long game, const char *v) {
    const uint64_t entry = u2_net_game_entry(game);
    if (!v || !*v || !entry) return;
    uint32_t map = 0;
    rd(entry + 4, &map, 4);
    const uint64_t me = u2_net_map_entry(game);
    if (!me) { printf("CoopMP: levels: map %08x of net game %ld not in *net-maps*\n", map, game); return; }
    int k = 0;
    for (const char *p = v; *p && k < 8;) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len >= sizeof(mp_level_names[0])) len = sizeof(mp_level_names[0]) - 1;
        memcpy(mp_level_names[k], p, len);
        mp_level_names[k][len] = 0;
        if (len) ++k;
        p = e ? e + 1 : p + len;
    }
    if (!k) return;
    const int first_in_list = k > 1;   /* "a,b,c": a = net pak, b..c = levels; "a": a is both */
    int c = 0;
    for (int i = first_in_list; i < k; ++i, ++c) {
        mp_level_list[c].name = (uint64_t)(uintptr_t)mp_level_names[i];
        mp_level_list[c].id = sid(mp_level_names[i]);
    }
    if (!first_in_list) { mp_level_list[0].name = (uint64_t)(uintptr_t)mp_level_names[0]; mp_level_list[0].id = sid(mp_level_names[0]); c = 1; }
    mp_level_head.count = (uint32_t)c;
    mp_level_head.list = (uint64_t)(uintptr_t)mp_level_list;
    const uint64_t name = (uint64_t)(uintptr_t)mp_level_names[0], head = (uint64_t)(uintptr_t)&mp_level_head;
    const uint32_t id = sid(mp_level_names[0]);
    wr(me + 0x18, &name, 8);
    wr(me + 0x20, &id, 4);
    wr(me + 0x28, &head, 8);
    printf("CoopMP: levels: net game %ld map %08x: net pak %s (%08x), %d level(s) from %s\n", game, map,
           mp_level_names[0], id, c, mp_level_names[first_in_list]);
}
/* What the net game needs to start with one player: the minimum (0x63f5c4: Deathmatch, Plunder,
 * Turf War, King of the Hill, Chain Reaction, Machinima: 1, or 6 when [0x104f48c] == 1;
 * Elimination 2 or 6; co-op 2, GOW3_COOP_SOLO). */
static void report_game(long game) {
    const uint64_t entry = u2_net_game_entry(game);
    int32_t mode = -1, ranked = 0;
    if (entry) rd(entry + 0xc, &mode, 4);
    rd((uint64_t)(uintptr_t)image + 0x104f48c, &ranked, 4);
    printf("CoopMP: net game %ld: mode %d, [0x104f48c] = %d (1: six players needed in competitive modes)\n", game, mode, ranked);
}

/* ---- what the net game itself decides (a joiner gets only the net game index) ---- */
static int32_t net_game_mode(long game) {
    const uint64_t entry = u2_net_game_entry(game);
    int32_t mode = -1;
    if (entry) rd(entry + 0xc, &mode, 4);
    return mode;
}
/* Co-op modes (net.bin's mode table: 4 Co-op, 8 Survival, 9 Gold Rush, 12 unnamed). */
static int coop_mode(int32_t mode) { return mode == 4 || mode == 8 || mode == 9 || mode == 12; }
static int competitive_mode(int32_t mode) { return mode >= 0 && !coop_mode(mode) && mode != 11; }
/* Shipped levels that carry a competitive map's online spawners (the classic maps' net paks
 * "<map>-multi-pak" are not in the Collection; coop-mp-2.md 01:30). */
static const char *const spawner_levels[] = {"the-fort-multi", "ruins-multi", "dig-multi"};
static char default_levels_text[48];
static const char *default_levels(long game) {
    const uint64_t me = u2_net_map_entry(game);
    uint64_t head = 0, list = 0;
    uint32_t count = 0;
    if (!me || !rd(me + 0x28, &head, 8) || !head || !rd(head, &count, 4) || !rd(head + 8, &list, 8) || !list)
        return NULL;
    for (uint32_t i = 0; i < count && i < 16; ++i) {
        uint64_t name = 0;
        char text[48] = {0};
        if (!rd(list + (uint64_t)i * 16, &name, 8) || !name || !rd(name, text, sizeof(text) - 1)) continue;
        for (size_t k = 0; k < sizeof(spawner_levels) / sizeof(*spawner_levels); ++k)
            if (!strcmp(text, spawner_levels[k])) {
                snprintf(default_levels_text, sizeof(default_levels_text), "%s", text);
                return default_levels_text;
            }
    }
    return NULL;
}
/* A knob: 1 set and on, 0 set and off, -1 not set. */
static int knob_set(const char *name) {
    const char *v = getenv(name);
    if (!v || !*v) return -1;
    return v[0] != '0';
}

/* ---- per-player characters: our *net-skins* (0x5a4731d2) ---- */
/* The net player spawns (local Player 0x663280, NetCharacter 0x6636b0) and StartNetGame's package
 * requests (0x641aa0: every slot in use asks the package manager 0x1134390 for its skin's actor,
 * so LoadLevels waits until it is loaded) read the art at +8 of entry [slot+0x138] of the skin
 * table {i32 count, 0x30-byte entries at +8}. The Collection ships 2 entries; ours has one per
 * slot (a copy of entry 0 each), entry k = the character of player id k + 1. */
#define SKIN_SLOTS 10
static __attribute__((aligned(16))) unsigned char skin_table[SKIN_SLOTS][0x30];
static char skin_names[SKIN_SLOTS][48];
static char skin_default[48];
static int skins_ready;
static int skin_name_ok(const char *s) {
    if (!s || !*s || strlen(s) >= 40) return 0;
    for (; *s; ++s)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-' || *s == '_')) return 0;
    return 1;
}
extern int runtime_net_skin(int id, char *out, size_t size);
/* Slot `slot` (player id `id`) gets skin index id - 1, whose entry names its character. Called by
 * runtime_coop.c whenever it fills a slot, and once for every slot at netinit. */
void runtime_coop_mp_slot_skin(unsigned char *base, uint64_t slot, int id) {
    image = base;
    if (!skins_ready || id < 1 || id > SKIN_SLOTS) return;
    const int k = id - 1;
    char chosen[48] = {0};
    const char *art = runtime_net_skin(id, chosen, sizeof(chosen)) && skin_name_ok(chosen) ? chosen : skin_default;
    if (strcmp(skin_names[k], art)) {
        snprintf(skin_names[k], sizeof(skin_names[k]), "%s", art);
        printf("CoopMP: player %d (slot %d): character %s\n", id, k, art);
        fflush(stdout);
    }
    const uint64_t name = (uint64_t)(uintptr_t)skin_names[k];
    memcpy(skin_table[k] + 8, &name, 8);
    const int32_t index = k;
    wr(slot + 0x138, &index, 4);
}
/* AssignTeams (0x6400d0, host) sets every slot's skin index from the removed lobby's choices: co-op
 * gives all three characters one value (0x6403c4 / 0x6403eb / 0x640414, r13d), team games the
 * slot's per-team byte +0x145 / +0x146 (0x640669, 0x6406a6, 0x640a19, 0x640a51, 0x640b0f, 0x640b32,
 * 0x640e25, 0x640e75), 0 without a lobby. So every player got slot 0's character (runs f2, f3, 25 Oct:
 * the joiner chose Elena and played as the host's character). The eleven stores are skipped,
 * slot+0x138 keeps ours, and the player-setup event (type 0x12: built from +0x138 at 0x6109ca,
 * applied by NpId at 0x6321e5) carries it to the joiners. */
static int install_skin_keep(void) {
    static const struct { uint64_t at; unsigned char len, orig[8]; } stores[] = {
        {0x6403c4, 7, {0x44, 0x89, 0xae, 0x38, 0x01, 0x00, 0x00}},
        {0x6403eb, 8, {0x45, 0x89, 0xac, 0x24, 0x38, 0x01, 0x00, 0x00}},
        {0x640414, 7, {0x45, 0x89, 0xae, 0x38, 0x01, 0x00, 0x00}},
        {0x640669, 7, {0x89, 0x8c, 0x03, 0x48, 0x01, 0x00, 0x00}},
        {0x6406a6, 7, {0x89, 0x8c, 0x03, 0x48, 0x01, 0x00, 0x00}},
        {0x640a19, 6, {0x89, 0x91, 0x38, 0x01, 0x00, 0x00}},
        {0x640a51, 6, {0x89, 0x91, 0x38, 0x01, 0x00, 0x00}},
        {0x640b0f, 6, {0x89, 0x91, 0x38, 0x01, 0x00, 0x00}},
        {0x640b32, 6, {0x89, 0x90, 0x38, 0x01, 0x00, 0x00}},
        {0x640e25, 6, {0x89, 0x8b, 0x38, 0x01, 0x00, 0x00}},
        {0x640e75, 6, {0x89, 0xbe, 0x38, 0x01, 0x00, 0x00}},
    };
    static const unsigned char nop6[6] = {0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00};
    static const unsigned char nop7[7] = {0x0f, 0x1f, 0x80, 0x00, 0x00, 0x00, 0x00};
    static const unsigned char nop8[8] = {0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00};
    int ok = 0;
    for (size_t i = 0; i < sizeof(stores) / sizeof(*stores); ++i) {
        const unsigned char *nop = stores[i].len == 6 ? nop6 : stores[i].len == 7 ? nop7 : nop8;
        if (!memcmp(image + stores[i].at, nop, stores[i].len)) { ++ok; continue; }
        if (memcmp(image + stores[i].at, stores[i].orig, stores[i].len)) continue;
        if (wr((uint64_t)(uintptr_t)image + stores[i].at, nop, stores[i].len)) ++ok;
    }
    return ok;
}
static void install_skins(const char *fallback) {
    const uint64_t skins = ((U2Dc)(image + 0xa15e70))(0x5a4731d2u);
    int32_t count = 0;
    uint64_t entries = 0;
    if (skins) { rd(skins, &count, 4); rd(skins + 8, &entries, 8); }
    snprintf(skin_default, sizeof(skin_default), "%s", fallback);
    if (!skins || count < 1 || !entries) { printf("CoopMP: *net-skins* not loaded: characters NOT set\n"); return; }
    if (entries != (uint64_t)(uintptr_t)skin_table) {
        unsigned char model[0x30];
        if (!rd(entries, model, sizeof(model))) return;
        for (int k = 0; k < SKIN_SLOTS; ++k) {
            memcpy(skin_table[k], model, sizeof(model));
            snprintf(skin_names[k], sizeof(skin_names[k]), "%s", skin_default);
            const uint64_t name = (uint64_t)(uintptr_t)skin_names[k];
            memcpy(skin_table[k] + 8, &name, 8);
        }
        const int32_t n = SKIN_SLOTS;
        const uint64_t table = (uint64_t)(uintptr_t)skin_table;
        wr(skins + 8, &table, 8);
        wr(skins, &n, 4);
    }
    skins_ready = 1;
    printf("CoopMP: characters: our *net-skins* (%d slots, was %d entries), default %s; AssignTeams keeps them: %d of 11 stores skipped\n",
           SKIN_SLOTS, count, skin_default, install_skin_keep());
    const uint64_t ngm = (uint64_t)(uintptr_t)image + U2_NGM;
    for (int k = 0; k < SKIN_SLOTS; ++k) {
        const uint64_t slot = ngm + 0x10 + (uint64_t)k * 0x2c0;
        int32_t id = 0;
        rd(slot + 0x120, &id, 4);
        if (id == k + 1) runtime_coop_mp_slot_skin(image, slot, id);
    }
}
/* ---- after a round: ShowResults (online task state, vtable 0x100aa40) ---- */
/* ShowResults' Update (0x5f0b30) decides when to leave (-> [vtbl+0x198] = Leave):
 * - a joiner leaves as soon as -[[0x113a040]+0xc] / 300 < 0, i.e. at once with our net clock (it
 *   counts up: runs f4-f6, the joiner was back at the title within a second);
 * - the host leaves when every other player still in the game (slot+0x26c clear) has its
 *   "done" byte slot+0x26d set (the removed session's message 0x105 set it, 0x5f0d1b), or when
 *   alone once the same clock test passes. Nothing sets +0x26d any more: the host stayed in
 *   ShowResults for ever (f4-f6: results page or black screen, net mode 26).
 * The detour keeps both in ShowResults for GOW3_COOP_MP_RESULTS_SECONDS (default 12: the results
 * page is up, the menu agent's `mpmenu auto`), then lets Update run; on the host it first marks the
 * other players done. Both then leave to the title (Leave 0x5f00d0). Net games only (netinit). */
static unsigned results_hold_ms = 12000;
static int ABI results_hold(uint64_t state) {
    (void)state;
    static ULONGLONG since, last;
    static int released;
    const ULONGLONG now = GetTickCount64();
    if (!since || now - last > 1000) {   /* a new visit (Update runs every frame while in it) */
        since = now;
        released = 0;
        printf("CoopMP: round results: held for %u s\n", results_hold_ms / 1000);
        fflush(stdout);
    }
    last = now;
    if (now - since < results_hold_ms) return 1;
    if (!released) {
        released = 1;
        const uint64_t ngm = (uint64_t)(uintptr_t)image + U2_NGM;
        uint64_t local = 0;
        rd(ngm, &local, 8);
        int marked = 0;
        for (int k = 0; k < 10; ++k) {
            const uint64_t slot = ngm + 0x10 + (uint64_t)k * 0x2c0;
            char name = 0;
            const uint8_t done = 1;
            rd(slot + 0x20, &name, 1);
            if (!name || slot == local) continue;
            if (wr(slot + 0x26d, &done, 1)) ++marked;
        }
        printf("CoopMP: round results: leaving (%d other player(s) marked done)\n", marked);
        fflush(stdout);
    }
    return 0;
}
static int install_results_hold(void) {
    static const unsigned char prologue[8] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56};
    const uint64_t site = 0x5f0b30;
    if (memcmp(image + site, prologue, sizeof(prologue))) return image[site] == 0xe9;
    unsigned char *s = stub_page();
    if (!s) return 0;
    unsigned n = 0;
#define B(...) do { const unsigned char b_[] = {__VA_ARGS__}; memcpy(s + n, b_, sizeof(b_)); n += sizeof(b_); } while (0)
#define Q(v) do { const uint64_t q_ = (v); memcpy(s + n, &q_, 8); n += 8; } while (0)
    /* entry rsp = 8 mod 16; 8 pushes + 8 = 0 mod 16 for the call */
    B(0x50, 0x51, 0x52, 0x56, 0x57, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52); /* push rax rcx rdx rsi rdi r8-r10 */
    B(0x48, 0x83, 0xec, 0x08);                                        /* sub rsp, 8 */
    B(0x48, 0xb8); Q((uint64_t)(uintptr_t)results_hold);              /* mov rax, results_hold */
    B(0xff, 0xd0);                                                    /* call rax (rdi = state) */
    B(0x48, 0x83, 0xc4, 0x08);                                        /* add rsp, 8 */
    B(0x85, 0xc0);                                                    /* test eax, eax */
    B(0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5f, 0x5e, 0x5a, 0x59, 0x58); /* pop r10-r8 rdi rsi rdx rcx rax */
    B(0x74, 0x01);                                                    /* jz +1: run Update */
    B(0xc3);                                                          /* ret: held this frame */
    memcpy(s + n, prologue, sizeof(prologue)); n += sizeof(prologue); /* replayed */
    B(0xff, 0x25, 0x00, 0x00, 0x00, 0x00); Q((uint64_t)(uintptr_t)(image + site + 8)); /* jmp [rip] */
#undef B
#undef Q
    return jump_to(site, s, 3);
}

/* ---- end of a net game: menu page stack (GOW3_COOP_MP_MENU_GUARD) ---- */
/* Leave / back to the menus (0x5ccce0 -> 0x5c7bb0 state 0): every page id on the saved page
 * stack (menu +0x1164[i]) is looked up among the loaded pages (+0x370[200], id at page +0x10);
 * the multiplayer pages were never loaded (no lobby), the lookup gives NULL and
 * `mov rax, [rdi]` at 0x5c7c92 faults (t11-t13, 24 Oct). The guard skips pages that are not
 * loaded (the call to their vfunc +0x68). */
static int install_menu_guard(void) {
    static const unsigned char orig[14] = {0x48, 0x8b, 0x07, 0x41, 0x8b, 0xb4, 0x9d, 0x84, 0x14, 0x00, 0x00, 0xff, 0x50, 0x68};
    const uint64_t site = 0x5c7c92;
    if (memcmp(image + site, orig, sizeof(orig))) return image[site] == 0xe9;
    unsigned char *s = stub_page();
    if (!s) return 0;
    unsigned n = 0;
    const unsigned char body[] = {
        0x48, 0x85, 0xff,             /* test rdi, rdi */
        0x74, 0x0e,                   /* jz skip (+14) */
        0x48, 0x8b, 0x07,             /* mov rax, [rdi] */
        0x41, 0x8b, 0xb4, 0x9d, 0x84, 0x14, 0x00, 0x00, /* mov esi, [r13+rbx*4+0x1484] */
        0xff, 0x50, 0x68,             /* call [rax+0x68] */
    };
    memcpy(s + n, body, sizeof(body)); n += sizeof(body);
    s[n++] = 0xff; s[n++] = 0x25; memset(s + n, 0, 4); n += 4;   /* skip: jmp [rip] -> 0x5c7ca0 */
    const uint64_t back = (uint64_t)(uintptr_t)(image + site + sizeof(orig));
    memcpy(s + n, &back, 8); n += 8;
    return jump_to(site, s, sizeof(orig) - 5);
}

/* ---- team games: AssignTeams' rank source (GOW3_COOP_MP_TEAMS) ---- */
/* AssignTeams (0x6400d0), team games (net game +0x10 >= 2): every player's rank/skill comes from
 * `rdi = (session [0x1065b08+0x10] == NULL && [0x1065b08+0xb0c] == 0x30) ? 0x1065b08+0xb10 : NULL`
 * (0x640583..0x64059d; the session member's own stats pointer was removed with the session layer)
 * and is read at 0x6405af / 0x6405a6: with netinit's session stand-in it is NULL and the host faults
 * at 0x6405af (run mp6, Fort deathmatch). The patch takes the local stats always (sete bl ->
 * mov bl, 1) and marks them present (+0xb0c = 0x30, level +0xb18 = 1, skills zero). */
static int install_teams(void) {
    static const unsigned char orig[3] = {0x0f, 0x94, 0xc3}, patch[3] = {0xb3, 0x01, 0x90};
    const uint64_t site = 0x640588, I = (uint64_t)(uintptr_t)image;
    if (!memcmp(image + site, orig, 3)) { if (!wr(I + site, patch, 3)) return 0; }
    else if (memcmp(image + site, patch, 3)) return 0;
    int32_t state = 0;
    rd(I + 0x1065b08 + 0xb0c, &state, 4);
    if (state != 0x30) {
        static const unsigned char zero[0x40] = {0};
        const int32_t present = 0x30;
        const uint8_t level = 1;
        wr(I + 0x1065b08 + 0xb10, zero, sizeof(zero));
        wr(I + 0x1065b08 + 0xb0c, &present, 4);
        wr(I + 0x1065b08 + 0xb18, &level, 1);
    }
    return 1;
}

/* gow3 25 Oct 2026 (runs vis1-vis7, coop-mp.md): the invisible players. The online task's update
 * (0x5f14d0, TaskManagerOnline) sets the render option "Disable Foreground Meshes"
 * (0x1050d40+0xbf) to 1 every frame at 0x5f228d and clears it only while a loaded level record
 * (type != 2, +0x998 set) lacks flag 0x10 at +0x19; in our net games every such record keeps the
 * flag (no session layer), so no foreground mesh was drawn: players, NetCharacters, pickups, props.
 * The patch stores 0 there instead (mov byte [r15+0xbf], 1 -> 0). Net games only (netinit);
 * GOW3_COOP_MP_KEEP_FG_HIDE=1 keeps the game's store (tests). */
static int install_show_foreground(void) {
    static const unsigned char orig[8] = {0x41, 0xc6, 0x87, 0xbf, 0x00, 0x00, 0x00, 0x01};
    const uint64_t site = 0x5f228d;
    if (!memcmp(image + site, orig, 8)) {
        const unsigned char zero = 0;
        return wr((uint64_t)(uintptr_t)image + site + 7, &zero, 1);
    }
    return image[site + 7] == 0 && !memcmp(image + site, orig, 7);
}

/* gow3 25 Oct 2026 (runs vis11-vis13, coop-mp.md): the remote NetCharacter's x stuck near -192.
 * The net bit writers keep the bits of the destination word above the field with
 * `mov r32, -1; mov cl, <count>; shl r32, cl` (count = 32 - bit offset): at bit offset 0 the count
 * is 32, x86 masks it to 0 and the old word is kept whole, so the field is ORed into stale bits.
 * The player snapshot's locator x started on a word whose buffer is not cleared: x became the OR
 * of every x sent (0x27fff.. = -192.0), while y and z (other offsets) were exact.
 * The first fix (coop36) covered 33 copies (the esi form only). 25 Oct 11:00: the same idiom is
 * compiled 342 times in the net code, with other registers (eax, ebx, esi, edi), a count from
 * dl / al / sil / r8b-r11b, or `sub ecx, eax` / `sub eax, edx` in between; the 309 others write
 * e.g. the player-state event (type 0x0c, 0x638a73) and the snapshot animation the receiver
 * parses (0x66b430: overruns and NaN joints with GOW3_COOP_NETANIM=1). Checked offline for all
 * 342 (analysis/triage-1022/coop-mp.md): the count is always 0x20 - x (1..32), the register is
 * next written as 32 bits (and r32, [word]) before any 64-bit or partial read, and no flags are
 * read before the next flag write. The patch shifts in 64 bits (`or r64, -1; shl r64, cl`, the
 * in-between instructions first; same length): mask 0 at offset 0, the same low 32 bits
 * otherwise. Patched only when exactly the 342 are found (U2 1.02), else the esi form alone.
 * Net games only (netinit); GOW3_COOP_MP_KEEP_MASK_BUG=1 keeps the original code (tests). */
static unsigned mask_mov(const unsigned char *p, int *reg) {
    static const unsigned char ones[4] = {0xff, 0xff, 0xff, 0xff};
    if (p[0] >= 0xb8 && p[0] <= 0xbf && !memcmp(p + 1, ones, 4)) { *reg = p[0] - 0xb8; return 5; }
    if (p[0] == 0x41 && p[1] >= 0xb8 && p[1] <= 0xbf && !memcmp(p + 2, ones, 4)) { *reg = 8 + p[1] - 0xb8; return 6; }
    return 0;
}
/* What may stand between the mov and the shift: mov cl, r8 (88 /r with rm = cl, REX 40 / 44) or
 * sub r32, r32 (29 /r, register form); neither may touch the mask register. */
static unsigned mask_between(const unsigned char *p, int reg) {
    unsigned n = 0;
    int rex = 0;
    if (p[0] == 0x40 || p[0] == 0x44) { rex = p[0]; n = 1; }
    if (p[n] == 0x88 && (p[n + 1] & 0xc7) == 0xc1) {
        const int src = ((p[n + 1] >> 3) & 7) | (rex == 0x44 ? 8 : 0);
        if (src == reg || (!rex && src >= 4 && (src & 3) == reg)) return 0;
        return n + 2;
    }
    if (!rex && p[0] == 0x29 && (p[1] & 0xc0) == 0xc0) {
        if ((p[1] & 7) == reg || ((p[1] >> 3) & 7) == reg) return 0;
        return 2;
    }
    return 0;
}
/* One site at `at`: its length and the 64-bit replacement in patch (0: not a site). */
static unsigned mask_site(uint64_t at, unsigned char *patch) {
    int reg = -1;
    const unsigned mov = mask_mov(image + at, &reg);
    if (!mov) return 0;
    unsigned mid = 0;
    for (int step = 0; step < 2; ++step) {
        const unsigned k = mask_between(image + at + mov + mid, reg);
        if (!k) break;
        mid += k;
    }
    const unsigned char *sh = image + at + mov + mid;
    const int shl = reg < 8 ? sh[0] == 0xd3 && sh[1] == 0xe0 + reg
                            : sh[0] == 0x41 && sh[1] == 0xd3 && sh[2] == 0xe0 + (reg - 8);
    if (!shl) return 0;
    const unsigned len = mov + mid + (reg < 8 ? 2 : 3);
    const unsigned char w = reg < 8 ? 0x48 : 0x49;
    unsigned n = mid;
    memcpy(patch, image + at + mov, mid);
    patch[n++] = w; patch[n++] = 0x83; patch[n++] = (unsigned char)(0xc8 + (reg & 7)); patch[n++] = 0xff; /* or r64, -1 */
    patch[n++] = w; patch[n++] = 0xd3; patch[n++] = (unsigned char)(0xe0 + (reg & 7));                    /* shl r64, cl */
    while (n < len) patch[n++] = 0x90;
    return len;
}
#define MASK_SITES_102 342
static int install_bit_writer_masks(int *found) {
    static int done, done_found, done_patched;
    if (done) { *found = done_found; return done_patched; }
    unsigned char patch[32];
    int count = 0, patched = 0;
    for (uint64_t at = 0x5e0000; at + 16 <= 0x640000; ++at) {
        const unsigned len = mask_site(at, patch);
        if (len) { ++count; at += len - 1; }
    }
    if (count == MASK_SITES_102) {
        for (uint64_t at = 0x5e0000; at + 16 <= 0x640000; ++at) {
            const unsigned len = mask_site(at, patch);
            if (!len) continue;
            if (wr((uint64_t)(uintptr_t)image + at, patch, len)) ++patched;
            at += len - 1;
        }
    } else {
        /* another executable: only the first fix's form (esi, count in al) */
        static const unsigned char orig[9] = {0xbe, 0xff, 0xff, 0xff, 0xff, 0x88, 0xc1, 0xd3, 0xe6};
        static const unsigned char fixed[9] = {0x88, 0xc1, 0x48, 0x83, 0xce, 0xff, 0x48, 0xd3, 0xe6};
        printf("CoopMP: net bit writers: %d mask sites, not the %d of U2 1.02: the esi form only\n", count, MASK_SITES_102);
        count = 0;
        for (uint64_t at = 0x5e0000; at + 9 <= 0x640000; ++at) {
            if (memcmp(image + at, orig, 9)) continue;
            ++count;
            if (wr((uint64_t)(uintptr_t)image + at, fixed, 8)) ++patched;
            at += 8;
        }
    }
    done = 1;
    done_found = *found = count;
    done_patched = patched;
    return patched;
}

/* ---- entry points for runtime_coop.c ---- */
void runtime_coop_mp_netinit(unsigned char *base, long game) {
    image = base;
    if (!knob("GOW3_COOP_MP_KEEP_MASK_BUG")) {
        int found = 0;
        const int patched = install_bit_writer_masks(&found);
        printf("CoopMP: net bit writers: %d of %d word masks fixed (offset 0 clears the word)\n", patched, found);
    }
    if (!knob("GOW3_COOP_MP_KEEP_FG_HIDE"))
        printf("CoopMP: foreground meshes %s\n", install_show_foreground() ? "shown (0x5f228d stores 0)" : "NOT patched");
    /* What the net game decides (the host's and every joiner's netinit see the same index). */
    const int32_t mode = net_game_mode(game);
    const int competitive = competitive_mode(mode);
    const char *levels = getenv("GOW3_COOP_MP_LEVELS");
    if (!levels && competitive) levels = default_levels(game);
    const char *skin = getenv("GOW3_COOP_MP_SKIN");
    if (!skin_name_ok(skin)) skin = competitive || game == 113 ? "hero-default" : "hero-parka";
    const int teams = knob_set("GOW3_COOP_MP_TEAMS") >= 0 ? knob_set("GOW3_COOP_MP_TEAMS") : competitive;
    printf("CoopMP: net game %ld: mode %d (%s): levels %s, teams fix %s, default character %s\n", game, mode,
           competitive ? "competitive" : coop_mode(mode) ? "co-op" : "other", levels ? levels : "(the game's)",
           teams ? "on" : "off", skin);
    if (knob_set("GOW3_COOP_MP_TWINS") != 0) {
        const int ok = install_twins();
        printf("CoopMP: MP DC twins (%d pairs) %s\n", twin_count, ok ? "on (entry detour 0xa15e70)" : "NOT installed");
    }
    if (knob_set("GOW3_COOP_MP_MENU_GUARD") != 0)
        printf("CoopMP: menu page guard %s\n", install_menu_guard() ? "on (0x5c7c92: pages not loaded are skipped)" : "NOT installed");
    if (teams)
        printf("CoopMP: team games: local rank source %s\n", install_teams() ? "on (0x640588, 0x1065b08+0xb0c)" : "NOT installed");
    apply_levels(game, levels);
    install_skins(skin);
    {
        const char *v = getenv("GOW3_COOP_MP_RESULTS_SECONDS");
        if (v && *v) results_hold_ms = (unsigned)(atof(v) * 1000);
        if (results_hold_ms)
            printf("CoopMP: round results: %s (%u s, then everyone leaves)\n",
                   install_results_hold() ? "hold on (ShowResults Update 0x5f0b30)" : "hold NOT installed", results_hold_ms / 1000);
    }
    report_game(game);
    fflush(stdout);
}
int runtime_coop_mp_command(unsigned char *base, const char *op, const char *args) {
    image = base;
    (void)args;
    (void)op;
    return 0;
}
#else
void runtime_coop_mp_netinit(unsigned char *base, long game) { (void)base; (void)game; }
void runtime_coop_mp_slot_skin(unsigned char *base, uint64_t slot, int id) { (void)base; (void)slot; (void)id; }
int runtime_coop_mp_command(unsigned char *base, const char *op, const char *args) { (void)base; (void)op; (void)args; return 0; }
#endif
