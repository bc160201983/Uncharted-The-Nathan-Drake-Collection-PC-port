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
#include "runtime_uc2mp_netmaps.h"
#include <math.h>   /* mm- spawns */
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

/* ---- remote players' animation from snapshots (net games) ---- */
/* gow3 25 Oct 2026 (coop-mp.md, an-* entries): the NetCharacter stood in a default pose, and with
 * GOW3_COOP_NETANIM=1 both games died soon after Playing (ORBISANIM joint-offset asserts, then the
 * havok NaN fault 0x512f44). Cause, in this build's own snapshot code:
 * - The writer (0x634970) sends each anim leaf as (art group, 11-bit index into the group's sorted
 *   anim table) plus the blend tree; it sends no clip phase (frame) at all.
 * - The parse (0x66b430) skips the 11-bit indices (loop 0x66b660: pos += 11) and its anim leaf
 *   (opcode 2, 0x66bcd0 -> 0x66c035) only checks the art group and takes a stack slot: it never
 *   calls the generator's clip method (vtable 0x100ac80 +0x18 = 0x5f5770, which has no caller).
 *   So the blends (+0x08/+0x10) mixed pose slots that nothing wrote this frame (stale memory).
 * - Leaves of type 0xc (a pose anim of 'proto') carry 4 more zero bits that the parse never reads:
 *   the rest of that snapshot was parsed out of step (the "parses ended early" count).
 * The fix gives every leaf one 10-bit phase field and makes the parse play it:
 *   writer, type-8 leaf end 0x6364b5: + phase = frame (cmd+0x10) / (frames - 1) of its anim;
 *   writer, type-0xc leaf: its 4 zero bits become 10 (0x6367cd, 0x6368a2, 0x6368b9: phase 0);
 *   parse, index loop 0x66b637: the indices are read (per thread) instead of skipped;
 *   parse, leaf 0x66c035: reads the phase; for the pose generator (0x100ac80) it emits the clip
 *   (group's sorted table [[group+0x30]+0x10] {u32 count, entries {u32 name sid, .., u64 anim}},
 *   checked by the anim's name sid) through +0x18, or the bind pose (+0x28) when it cannot;
 *   the source 0x5f3ca0 (0x5f3d63): bind pose into both result slots before the two parses, so
 *   a parse that ends early still leaves a defined pose; after them (0x5f3dd1) a tree deeper than
 *   GOW3_COOP_NETANIM_DEPTH (default 4) slots above the base is dropped (na_check).
 * Every machine of a net game runs the same netinit, so writer and parse agree (net version 5).
 * GOW3_COOP_NETANIM=0 keeps the old default pose (0x5f3ccf), the format change stays. */
#define NA_BITS 10
#define NA_MAX ((1u << NA_BITS) - 1)
#define NA_GEN_POSE 0x100ac80
/* Nothing prints from the animation job (guest fiber stacks): counters and a few samples here,
 * printed by runtime_coop_mp_frame (coop_frame, main thread). */
static volatile LONG na_written, na_clips, na_poses, na_misses, na_direct, na_handle, na_rollbacks, na_depth_max;
static volatile LONG na_full, na_pos_max; /* run a07: phases not written (stream full), deepest leaf-end bit */
static volatile LONG na_sample_w, na_sample_r;
static struct { uint64_t anim; float frame; unsigned frames, q, slot; } na_samples_w[4], na_samples_r[4];
static struct { uint64_t group, x; uint32_t idx, sid; } na_samples_m[4];
static unsigned na_depth_limit = 4;   /* GOW3_COOP_NETANIM_DEPTH: deepest result slot above the base */
static _Thread_local struct { uint64_t list; uint32_t count, top; } na_mark;
static _Thread_local uint16_t na_index[32];
static _Thread_local uint32_t na_count;
static _Thread_local struct { uint64_t group, anim; uint32_t idx; } na_cache[8];
static _Thread_local unsigned na_cache_next;

static uint64_t na_q(uint64_t a) { uint64_t v = 0; return a && rd(a, &v, 8) ? v : 0; }
static uint32_t na_d(uint64_t a) { uint32_t v = 0; return a && rd(a, &v, 4) ? v : 0; }
static int na_name(uint64_t p, char *out, size_t size) {
    for (size_t at = 0; p && at + 16 <= size; at += 16) {
        if (!rd(p + at, out + at, 16)) return 0;
        if (memchr(out + at, 0, 16)) return 1;
    }
    return 0;
}
static uint32_t na_get(const uint32_t *words, uint32_t pos, unsigned n) {
    uint32_t v = 0;
    for (unsigned i = 0; i < n; ++i, ++pos) v = v << 1 | (words[pos >> 5] >> (31 - (pos & 31)) & 1u);
    return v;
}
static void na_put(uint32_t *words, uint32_t pos, uint32_t v, unsigned n) {
    for (unsigned i = 0; i < n; ++i, ++pos) {
        const uint32_t m = 1u << (31 - (pos & 31));
        if (v >> (n - 1 - i) & 1u) words[pos >> 5] |= m; else words[pos >> 5] &= ~m;
    }
}
static unsigned na_frames(uint64_t anim) {
    uint16_t frames = 0;
    const uint64_t clip = na_q(anim + 0x28);
    if (clip) rd(clip + 0x22, &frames, 2);
    return frames;
}
/* Writer (sender), end of a type-8 leaf: stream {u32 *words, u32 pos, u32 max}, cmd = the anim
 * command {u16 8, u16 size, u64 anim, u32 slot, float frame}. */
static void ABI na_write_phase(uint64_t stream, uint64_t cmd) {
    uint32_t *words, pos, max;
    memcpy(&words, (void *)(uintptr_t)stream, 8);
    memcpy(&pos, (void *)(uintptr_t)(stream + 8), 4);
    memcpy(&max, (void *)(uintptr_t)(stream + 12), 4);
    if (pos > (uint32_t)na_pos_max) InterlockedExchange(&na_pos_max, (LONG)pos);
    if (pos + NA_BITS > max) { InterlockedIncrement(&na_full); return; }
    uint64_t anim;
    float frame;
    memcpy(&anim, (void *)(uintptr_t)(cmd + 4), 8);
    memcpy(&frame, (void *)(uintptr_t)(cmd + 0x10), 4);
    const unsigned frames = na_frames(anim);
    uint32_t q = 0;
    if (frames > 1 && frame > 0.0f) {
        float p = frame / (float)(frames - 1);
        if (p > 1.0f) p = 1.0f;
        q = (uint32_t)(p * (float)NA_MAX + 0.5f);
    }
    na_put(words, pos, q, NA_BITS);
    pos += NA_BITS;
    memcpy((void *)(uintptr_t)(stream + 8), &pos, 4);
    InterlockedIncrement(&na_written);
    const LONG k = InterlockedIncrement(&na_sample_w) - 1;
    if (k < 4) {
        na_samples_w[k].anim = anim;
        na_samples_w[k].frame = frame;
        na_samples_w[k].frames = frames;
        na_samples_w[k].q = q;
    }
}
/* Parse: the anim (11-bit) indices, read instead of skipped. Reader at rbp-0x150. */
static void ABI na_read_indices(uint64_t rbp, uint32_t count) {
    const uint64_t reader = rbp - 0x150;
    const uint32_t *words;
    uint32_t pos, max;
    memcpy(&words, (void *)(uintptr_t)reader, 8);
    memcpy(&pos, (void *)(uintptr_t)(reader + 8), 4);
    memcpy(&max, (void *)(uintptr_t)(reader + 12), 4);
    na_count = count < 32 ? count : 32;
    for (uint32_t i = 0; i < count; ++i, pos += 11)
        if (i < 32) na_index[i] = pos + 11 <= max ? (uint16_t)na_get(words, pos, 11) : 0xffff;
    memcpy((void *)(uintptr_t)(reader + 8), &pos, 4);
}
static uint64_t na_anim(uint64_t group, uint32_t idx) {
    if (!group || idx >= 0x800) return 0;
    for (unsigned i = 0; i < 8; ++i)
        if (na_cache[i].group == group && na_cache[i].idx == idx && na_cache[i].anim) return na_cache[i].anim;
    const uint64_t tab = na_q(na_q(group + 0x30) + 0x10);
    if (!tab || idx >= na_d(tab)) return 0;
    const uint64_t entry = na_q(tab + 8) + (uint64_t)idx * 16;
    const uint32_t want = na_d(entry);
    const uint64_t x = na_q(entry + 8);
    uint64_t found = 0;
    for (int deref = 0; deref < 2 && !found && x; ++deref) {
        const uint64_t a = deref ? na_q(x) : x;
        char name[128];
        if (a && na_name(na_q(a), name, sizeof(name)) && sid(name) == want && na_frames(a)) {
            found = a;
            InterlockedIncrement(deref ? &na_handle : &na_direct);
        }
    }
    if (found) {
        const unsigned k = na_cache_next++ & 7;
        na_cache[k].group = group;
        na_cache[k].idx = idx;
        na_cache[k].anim = found;
    } else {
        const LONG k = InterlockedIncrement(&na_misses) - 1;
        if (k < 4) {
            na_samples_m[k].group = group;
            na_samples_m[k].x = x;
            na_samples_m[k].idx = idx;
            na_samples_m[k].sid = want;
        }
    }
    return found;
}
typedef void (ABI *NaClip)(uint64_t gen, uint64_t anim, uint32_t slot, float phase);
typedef void (ABI *NaPose)(uint64_t gen, uint32_t slot);
/* Parse, anim leaf (0x66c035): group = its art group (rax), next = its pair index + 1 (r14). */
static void ABI na_leaf(uint64_t rbp, uint64_t group, uint64_t next) {
    const uint64_t reader = rbp - 0x150;
    const uint32_t *words;
    uint32_t pos, max, q = 0;
    memcpy(&words, (void *)(uintptr_t)reader, 8);
    memcpy(&pos, (void *)(uintptr_t)(reader + 8), 4);
    memcpy(&max, (void *)(uintptr_t)(reader + 12), 4);
    if (pos + NA_BITS <= max) {
        q = na_get(words, pos, NA_BITS);
        pos += NA_BITS;
        memcpy((void *)(uintptr_t)(reader + 8), &pos, 4);
    }
    uint64_t gen, vt;
    memcpy(&gen, (void *)(uintptr_t)(rbp - 0x190), 8);
    if (!gen) return;
    memcpy(&vt, (void *)(uintptr_t)gen, 8);
    if (vt != (uint64_t)(uintptr_t)image + NA_GEN_POSE) return;   /* root locator, debug: bits only */
    uint32_t slot, sub;
    memcpy(&slot, (void *)(uintptr_t)(rbp - 0x180), 4);
    memcpy(&sub, (void *)(uintptr_t)(gen + 0xc), 4);
    if (sub + slot >= 7) return;   /* the generator's own limit (assert in 0x5f5770 / 0x5f5650) */
    const uint64_t anim = next >= 1 && next <= na_count ? na_anim(group, na_index[next - 1]) : 0;
    uint64_t fn;
    if (anim) {
        memcpy(&fn, (void *)(uintptr_t)(vt + 0x18), 8);
        ((NaClip)(uintptr_t)fn)(gen, anim, slot, (float)q / (float)NA_MAX);
        InterlockedIncrement(&na_clips);
        const LONG k = InterlockedIncrement(&na_sample_r) - 1;
        if (k < 4) {
            na_samples_r[k].anim = anim;
            na_samples_r[k].q = q;
            na_samples_r[k].slot = slot;
        }
    } else {
        memcpy(&fn, (void *)(uintptr_t)(vt + 0x28), 8);
        ((NaPose)(uintptr_t)fn)(gen, slot);
        InterlockedIncrement(&na_poses);
    }
}
/* Source 0x5f3ca0 after its two parses (0x5f3dd1, list = r15, base = ebx): a tree deeper than
 * na_depth_limit slots above the base (a garbled snapshot: the parse has no depth limit, and every
 * slot up to the deepest one gets a pose instance from the job's 64 KiB pool, 0xd36c60, whose
 * failure leaves garbage instance pointers) is dropped: the list goes back to the two bind poses
 * of na_prefill. Counted; the deepest tree seen is reported. */
static void ABI na_check(uint64_t list, uint32_t base) {
    uint32_t top;
    memcpy(&top, (void *)(uintptr_t)(list + 0x10), 4);
    const LONG depth = top >= base ? (LONG)(top - base) : 0;
    LONG seen = na_depth_max;
    while (depth > seen && InterlockedCompareExchange(&na_depth_max, depth, seen) != seen) seen = na_depth_max;
    if ((unsigned)depth <= na_depth_limit || na_mark.list != list) return;
    memcpy((void *)(uintptr_t)(list + 0xc), &na_mark.count, 4);
    memcpy((void *)(uintptr_t)(list + 0x10), &na_mark.top, 4);
    InterlockedIncrement(&na_rollbacks);
}
/* gow3 25 Oct 2026 (pd- agent, coop-mp.md): why ~100-200 parses/s of a remote player's snapshots
 * ended early (bind poses). The writer's stream is 32-bit words, bits from bit 31 down (stream bit p
 * = word p >> 5, bit 31 - (p & 31)), and the parse reads it as words. The snapshot's own serializer
 * (0x638938 -> 0x5e1a30) and the receiver (0x6398a0 -> 0x5e2100) copy the stream as BYTES in memory
 * order, ceil(bits / 8) of them (the PS3 original was big-endian: there, byte order = bit order).
 * On this little-endian build the last, partly filled word's stream bits sit in its HIGH bytes, the
 * copy takes its LOW bytes: the tail of every snapshot (up to 32 bits: the last blends and the end
 * marker) reached the receiver as whatever an earlier snapshot left in that history slot
 * (0x5f3510, 6 slots of 0xaa bytes). Same tree as before: harmless; a tree whose length class
 * changed (turning, aiming) stayed garbled until a longer snapshot refreshed those bytes, so whole
 * runs of parses ended early (0x66c0ae = the 8-bit blend weight past the end, 66bc32, 66be02, ...).
 * Fix, sender only: the bit count stored after the writer (0x60e296 player, 0x60e9f3 the other
 * writer call) is rounded up to whole words, so the copies carry every word whole; the padding is
 * zero (the writer clears the rest of its last word) and lies after the end marker. A count that
 * would pass 0x550 bits stays as it is (the history keeps at most 0xaa bytes). Wire format
 * unchanged. GOW3_COOP_NETANIM_PAD=0 = the old exact count (tests). */
static volatile LONG na_padded;
static int na_pad_sites;   /* writer sites hooked (of 2) */
static void ABI na_pad(uint64_t snapshot, uint32_t bits) {
    uint32_t n = (bits + 31u) & ~31u;
    if (n > 0x550) n = bits;
    if (n != bits) InterlockedIncrement(&na_padded);
    memcpy((void *)(uintptr_t)(snapshot + 0xbc), &n, 4);
}
static void np_frame(void);   /* co-op enemies, below */
static void mm_frame(void);   /* competitive spawns (mm- agent), below */
/* coop_frame (main thread): samples and the 10 s line. */
void runtime_coop_mp_frame(void) {
    static ULONGLONG last;
    static LONG shown_w, shown_r, shown_m;
    char name[96];
    const LONG before = shown_w + shown_r + shown_m;
    int printed = 0;
    for (; shown_w < na_sample_w && shown_w < 4; ++shown_w) {
        strcpy(name, "?");
        na_name(na_q(na_samples_w[shown_w].anim), name, sizeof(name));
        printf("CoopMP: snapshot leaf sent: %s, frame %.2f of %u -> phase %u/%u\n", name,
               (double)na_samples_w[shown_w].frame, na_samples_w[shown_w].frames, na_samples_w[shown_w].q, NA_MAX);
    }
    for (; shown_r < na_sample_r && shown_r < 4; ++shown_r) {
        strcpy(name, "?");
        na_name(na_q(na_samples_r[shown_r].anim), name, sizeof(name));
        printf("CoopMP: snapshot leaf played: %s, phase %u/%u, slot %u\n", name, na_samples_r[shown_r].q, NA_MAX,
               na_samples_r[shown_r].slot);
    }
    for (; shown_m < na_misses && shown_m < 4; ++shown_m)
        printf("CoopMP: snapshot leaf: no anim at index %u of group %llx (entry sid %08x, ptr %llx)\n",
               na_samples_m[shown_m].idx, (unsigned long long)na_samples_m[shown_m].group, na_samples_m[shown_m].sid,
               (unsigned long long)na_samples_m[shown_m].x);
    const ULONGLONG now = GetTickCount64();
    if (now - last >= 10000 && (na_written || na_clips || na_poses)) {
        last = now;
        printf("CoopMP: remote animation: %ld phases sent, %ld clips played (%ld/%ld direct/handle lookups), "
               "%ld bind poses, %ld lookup misses, %ld trees dropped (deepest %ld, limit %u); "
               "%ld phases not written (stream full), deepest leaf end bit %ld; %ld snapshots padded to whole words\n",
               (long)na_written, (long)na_clips, (long)na_direct, (long)na_handle, (long)na_poses, (long)na_misses,
               (long)na_rollbacks, (long)na_depth_max, na_depth_limit, (long)na_full, (long)na_pos_max, (long)na_padded);
        printed = 1;
    }
    if (printed || shown_w + shown_r + shown_m != before) fflush(stdout);
    np_frame();   /* co-op enemies (np-) */
    mm_frame();   /* competitive spawns (mm-) */
}
/* Source 0x5f3ca0 before its two parses: bind pose (command 0xd, 2 words) into the result slots
 * base and base + 1 of the command list {u32 *words, .., u32 count +0xc, u32 top slot +0x10}. */
static void ABI na_prefill(uint64_t list, uint32_t base) {
    uint32_t *words, count, top;
    memcpy(&words, (void *)(uintptr_t)list, 8);
    memcpy(&count, (void *)(uintptr_t)(list + 0xc), 4);
    memcpy(&top, (void *)(uintptr_t)(list + 0x10), 4);
    for (uint32_t s = base; s <= base + 1; ++s) {
        words[count] = 0x0002000du;
        words[count + 1] = s;
        count += 2;
        if (top < s) top = s;
    }
    memcpy((void *)(uintptr_t)(list + 0xc), &count, 4);
    memcpy((void *)(uintptr_t)(list + 0x10), &top, 4);
    na_mark.list = list;   /* na_check's way back */
    na_mark.count = count;
    na_mark.top = top;
}
/* A detour in the middle of a function: all registers, flags and the SSE state are kept around the
 * C call (rdi/rsi/rdx from `args`), then `replay` runs and execution goes on at `back`. */
static int mid_hook(uint64_t site, const unsigned char *expect, unsigned len, const unsigned char *args, unsigned nargs,
                    void *fn, const unsigned char *replay, unsigned nreplay, uint64_t back) {
    if (len < 5 || memcmp(image + site, expect, len)) return 0;
    unsigned char *s = stub_page();
    if (!s) return 0;
    unsigned n = 0;
#define B(...) do { const unsigned char b_[] = {__VA_ARGS__}; memcpy(s + n, b_, sizeof(b_)); n += sizeof(b_); } while (0)
#define Q(v) do { const uint64_t q_ = (v); memcpy(s + n, &q_, 8); n += 8; } while (0)
    B(0x48, 0x8d, 0x64, 0x24, 0x80);                                  /* lea rsp, [rsp-0x80] (red zone) */
    B(0x9c);                                                          /* pushfq */
    B(0x50, 0x51, 0x52, 0x56, 0x57, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, 0x53); /* rax..r11, rbx */
    B(0x48, 0x89, 0xe3);                                              /* mov rbx, rsp */
    B(0x48, 0x83, 0xe4, 0xf0);                                        /* and rsp, -16 */
    B(0x48, 0x81, 0xec, 0x00, 0x02, 0x00, 0x00);                      /* sub rsp, 0x200 */
    B(0x48, 0x0f, 0xae, 0x04, 0x24);                                  /* fxsave64 [rsp] */
    memcpy(s + n, args, nargs); n += nargs;
    B(0x48, 0xb8); Q((uint64_t)(uintptr_t)fn);                        /* mov rax, fn */
    B(0xff, 0xd0);                                                    /* call rax */
    B(0x48, 0x0f, 0xae, 0x0c, 0x24);                                  /* fxrstor64 [rsp] */
    B(0x48, 0x89, 0xdc);                                              /* mov rsp, rbx */
    B(0x5b, 0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5f, 0x5e, 0x5a, 0x59, 0x58); /* pops */
    B(0x9d);                                                          /* popfq */
    B(0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00);                /* lea rsp, [rsp+0x80] */
    if (nreplay) { memcpy(s + n, replay, nreplay); n += nreplay; }
    B(0xff, 0x25, 0x00, 0x00, 0x00, 0x00); Q((uint64_t)(uintptr_t)(image + back)); /* jmp [rip] */
#undef B
#undef Q
    return n <= 256 && jump_to(site, s, len - 5);
}
static int install_netanim(void) {
    static int done;
    if (done) return done > 0;
    /* hook sites (all checked before any is written) */
    static const unsigned char w_leaf[6] = {0x41, 0xff, 0xc2, 0x41, 0xff, 0xc6};      /* 0x6364b5 inc r10d; inc r14d */
    static const unsigned char p_skip[7] = {0x4c, 0x8d, 0x35, 0x3c, 0xc7, 0x81, 0x00}; /* 0x66b637 lea r14, [rip+..] */
    static const unsigned char p_leaf[7] = {0x4c, 0x89, 0xb5, 0x78, 0xfe, 0xff, 0xff}; /* 0x66c035 mov [rbp-0x188], r14 */
    static const unsigned char s_pre[7] = {0x48, 0x8d, 0x0d, 0x16, 0x6f, 0xa1, 0x00};  /* 0x5f3d63 lea rcx, [rip+..] */
    static const unsigned char s_post[7] = {0x41, 0x8b, 0x47, 0x0c, 0x49, 0x8b, 0x0f}; /* 0x5f3dd1 mov eax,[r15+0xc]; mov rcx,[r15] */
    static const unsigned char c_room[3] = {0x8d, 0x42, 0x04}, c_clear[3] = {0x83, 0xf9, 0x03}, c_add[3] = {0x83, 0xc0, 0x04};
    if (memcmp(image + 0x6364b5, w_leaf, 6) || memcmp(image + 0x66b637, p_skip, 7) || memcmp(image + 0x66c035, p_leaf, 7) ||
        memcmp(image + 0x5f3d63, s_pre, 7) || memcmp(image + 0x5f3dd1, s_post, 7) ||
        memcmp(image + 0x6367cb, c_room, 3) || memcmp(image + 0x6368a0, c_clear, 3) ||
        memcmp(image + 0x6368b7, c_add, 3)) {
        done = -1;
        return 0;
    }
    static const unsigned char a_write[] = {0x4c, 0x89, 0xff, 0x4c, 0x89, 0xe6};   /* mov rdi, r15; mov rsi, r12 */
    static const unsigned char a_skip[] = {0x48, 0x89, 0xef, 0x44, 0x89, 0xf6};    /* mov rdi, rbp; mov esi, r14d */
    static const unsigned char a_leaf[] = {0x48, 0x89, 0xef, 0x48, 0x89, 0xc6, 0x4c, 0x89, 0xf2}; /* rdi rbp, rsi rax, rdx r14 */
    static const unsigned char a_pre[] = {0x48, 0x8b, 0x3b, 0x44, 0x89, 0xe6};     /* mov rdi, [rbx] (saved rbx); mov esi, r12d */
    static const unsigned char a_post[] = {0x4c, 0x89, 0xff, 0x8b, 0x33};          /* mov rdi, r15; mov esi, [rbx] (saved ebx = base) */
    unsigned char r_pre[10] = {0x48, 0xb9};                                          /* mov rcx, image + 0x100ac80 */
    const uint64_t pose_vt = (uint64_t)(uintptr_t)image + NA_GEN_POSE;
    memcpy(r_pre + 2, &pose_vt, 8);
    const unsigned char nine = 0x09, ten = 0x0a;
    const int ok = mid_hook(0x6364b5, w_leaf, 6, a_write, sizeof(a_write), (void *)na_write_phase, w_leaf, 6, 0x6364bb) &&
                   wr((uint64_t)(uintptr_t)image + 0x6367cd, &ten, 1) && wr((uint64_t)(uintptr_t)image + 0x6368a2, &nine, 1) &&
                   wr((uint64_t)(uintptr_t)image + 0x6368b9, &ten, 1) &&
                   mid_hook(0x66b637, p_skip, 7, a_skip, sizeof(a_skip), (void *)na_read_indices, NULL, 0, 0x66b6a5) &&
                   mid_hook(0x66c035, p_leaf, 7, a_leaf, sizeof(a_leaf), (void *)na_leaf, p_leaf, 7, 0x66c03c) &&
                   mid_hook(0x5f3d63, s_pre, 7, a_pre, sizeof(a_pre), (void *)na_prefill, r_pre, sizeof(r_pre), 0x5f3d6a) &&
                   mid_hook(0x5f3dd1, s_post, 7, a_post, sizeof(a_post), (void *)na_check, s_post, 7, 0x5f3dd8);
    done = ok ? 1 : -1;
    /* sender: snapshot bit counts rounded up to whole words (na_pad); the store is done there */
    const char *pad = getenv("GOW3_COOP_NETANIM_PAD");
    if (ok && !(pad && pad[0] == '0')) {
        static const unsigned char st_player[8] = {0x41, 0x89, 0x84, 0x24, 0xbc, 0x00, 0x00, 0x00}; /* 0x60e296 mov [r12+0xbc], eax */
        static const unsigned char st_other[6] = {0x89, 0x83, 0xbc, 0x00, 0x00, 0x00};               /* 0x60e9f3 mov [rbx+0xbc], eax */
        static const unsigned char a_player[] = {0x4c, 0x89, 0xe7, 0x89, 0xc6};                      /* mov rdi, r12; mov esi, eax */
        static const unsigned char a_other[] = {0x48, 0x8b, 0x3b, 0x89, 0xc6};                       /* mov rdi, [rbx] (saved rbx); mov esi, eax */
        na_pad_sites = mid_hook(0x60e296, st_player, 8, a_player, sizeof(a_player), (void *)na_pad, NULL, 0, 0x60e29e) +
                       mid_hook(0x60e9f3, st_other, 6, a_other, sizeof(a_other), (void *)na_pad, NULL, 0, 0x60e9f9);
    }
    return ok;
}

/* ---- co-op enemies: NPC network sync (np- agent, 25 Oct; GOW3_COOP_NPC=0 = off) ----
 * The game's own host side still works: Npc::Init (0x23bb06) names each NPC NetNpc-<n> (npc+0x68),
 * sends NpcSpawn (type 0x25, 0x6133d0) and adds an NPC tracker (0x644fa0); every frame the host
 * sends each NPC's CharacterMove (0x25e9e1 -> 0x63fbf0); hits are applied on the host and
 * broadcast (types 0xd / 0x10 -> 0x11); an NPC's end sends NpcDespawn (0x26, 0x240fcf).
 * Missing in this build: NpcSpawn's Read/Write/Execute and NpcDespawn's Read/Write are bare `ret`
 * (vtables 0x1040f30, 0x1040760), event 0x2c has a Write but no Read (vtable 0x10405d0: the rest of
 * its packet was read out of step), and nothing binds a joiner's own copy of an NPC to the host's
 * name, so a joiner dropped every NPC event (its tracker table ngm+0x1ba0 stayed empty).
 * Here (every machine of a net game, net version 6):
 *   0x25 Read/Write: 4 x 32 bits at +0xc..+0x18 (NetNpc sid, spawner sid [[info+0x40]+0x28], the
 *        NPC info's +0 and +0x40; U3's NpcSpawn carries the same four), Execute: queue + bind;
 *   0x26 Read/Write: 32 bits at +0xc (the NetNpc sid); 0x2c Read mirrors its Write (12/32/32/11/6);
 *   joiner Npc::Init (0x23bb5a, the branch after the host's naming): each local NPC is recorded with
 *        its spawner sid; a host NpcSpawn and a local NPC of the same spawner (oldest first) are
 *        bound: npc+0x68 = the host's NetNpc sid, AddNetNpcTracker(ngm, npc, [npc+0x3708]);
 *   0x240fcf: a joiner no longer sends NpcDespawn for its copies (only the host ends an NPC);
 *   0x638332 / 0x6383d7: count NPC CharacterMoves received / matched (log line every 10 s).
 * Nothing prints from guest threads: messages go through np_ring, printed by runtime_coop_mp_frame. */
#define NP_SPAWN_VT 0x1040f30
#define NP_DESPAWN_VT 0x1040760
#define NP_EV2C_VT 0x10405d0
#define NP_MAX 256
static volatile LONG np_spawn_w, np_spawn_r, np_despawn_w, np_despawn_r, np_2c_r, np_local_n, np_bound, np_own_skip;
static volatile LONG np_moves, np_moves_found, np_bind_fail;
static struct { uint64_t npc; uint32_t id, key, f0, f40, name; int state; } np_local[NP_MAX]; /* 1 waiting, 2 bound */
static struct { uint32_t name, key, f0, f40; int state, tried; ULONGLONG t; } np_remote[NP_MAX]; /* 1 waiting */
static unsigned np_local_next, np_remote_next;
static SRWLOCK np_lock = SRWLOCK_INIT;
static struct { int kind; uint32_t a, b, c; uint64_t p; } np_ring[256];
static volatile LONG np_ring_w;
static LONG np_ring_r;
static void np_msg(int kind, uint32_t a, uint32_t b, uint32_t c, uint64_t p) {
    const LONG k = InterlockedIncrement(&np_ring_w) - 1;
    if (k - np_ring_r >= 256) return;   /* printer behind: dropped */
    np_ring[k & 255].a = a; np_ring[k & 255].b = b; np_ring[k & 255].c = c; np_ring[k & 255].p = p;
    MemoryBarrier();
    np_ring[k & 255].kind = kind;
}
static int np_is_host(void) {
    const uint64_t s = (uint64_t)(uintptr_t)image + 0x1065b08;
    uint64_t session = 0;
    uint32_t ids[2] = {0, 0};
    rd(s + 0x10, &session, 8);
    rd(s + 0xb00, ids, 8);
    return !session || ids[0] == ids[1];
}
static uint32_t np_get(uint64_t stream, unsigned n) {
    const uint32_t *words;
    uint32_t pos, max;
    memcpy(&words, (void *)(uintptr_t)stream, 8);
    memcpy(&pos, (void *)(uintptr_t)(stream + 8), 4);
    memcpy(&max, (void *)(uintptr_t)(stream + 12), 4);
    if (pos + n > max) { memcpy((void *)(uintptr_t)(stream + 8), &max, 4); return 0; }
    const uint32_t v = na_get(words, pos, n);
    pos += n;
    memcpy((void *)(uintptr_t)(stream + 8), &pos, 4);
    return v;
}
static void np_put(uint64_t stream, uint32_t v, unsigned n) {
    uint32_t *words, pos, max;
    memcpy(&words, (void *)(uintptr_t)stream, 8);
    memcpy(&pos, (void *)(uintptr_t)(stream + 8), 4);
    memcpy(&max, (void *)(uintptr_t)(stream + 12), 4);
    if (pos + n > max) return;
    na_put(words, pos, n < 32 ? v & ((1u << n) - 1) : v, n);
    pos += n;
    memcpy((void *)(uintptr_t)(stream + 8), &pos, 4);
}
static void ABI np_spawn_write(uint64_t ev, uint64_t stream) {
    for (unsigned o = 0xc; o <= 0x18; o += 4) np_put(stream, *(uint32_t *)(uintptr_t)(ev + o), 32);
    if (InterlockedIncrement(&np_spawn_w) <= 64)
        np_msg(1, *(uint32_t *)(uintptr_t)(ev + 0xc), *(uint32_t *)(uintptr_t)(ev + 0x10), *(uint32_t *)(uintptr_t)(ev + 0x14), 0);
}
static void ABI np_spawn_read(uint64_t ev, uint64_t stream) {
    for (unsigned o = 0xc; o <= 0x18; o += 4) {
        const uint32_t v = np_get(stream, 32);
        memcpy((void *)(uintptr_t)(ev + o), &v, 4);
    }
}
static void ABI np_despawn_write(uint64_t ev, uint64_t stream) {
    np_put(stream, *(uint32_t *)(uintptr_t)(ev + 0xc), 32);
    InterlockedIncrement(&np_despawn_w);
}
static void ABI np_despawn_read(uint64_t ev, uint64_t stream) {
    const uint32_t name = np_get(stream, 32);
    memcpy((void *)(uintptr_t)(ev + 0xc), &name, 4);
    if (InterlockedIncrement(&np_despawn_r) <= 64) np_msg(5, name, 0, 0, 0);
    AcquireSRWLockExclusive(&np_lock);   /* a host NPC that ended before we bound it */
    for (unsigned i = 0; i < NP_MAX; ++i)
        if (np_remote[i].state == 1 && np_remote[i].name == name) np_remote[i].state = 0;
    ReleaseSRWLockExclusive(&np_lock);
}
/* 0x2c: the Write 0x61fb10 sends +0xc in 12 bits, +0x10 and +0x14 in 32, +0x18 (float * 150) in 11,
 * +0x1c in 6. */
static void ABI np_2c_read(uint64_t ev, uint64_t stream) {
    uint32_t v[4];
    v[0] = np_get(stream, 12);
    v[1] = np_get(stream, 32);
    v[2] = np_get(stream, 32);
    const float f = (float)np_get(stream, 11) / 150.0f;
    v[3] = np_get(stream, 6);
    memcpy((void *)(uintptr_t)(ev + 0xc), &v[0], 4);
    memcpy((void *)(uintptr_t)(ev + 0x10), &v[1], 4);
    memcpy((void *)(uintptr_t)(ev + 0x14), &v[2], 4);
    memcpy((void *)(uintptr_t)(ev + 0x18), &f, 4);
    memcpy((void *)(uintptr_t)(ev + 0x1c), &v[3], 4);
    InterlockedIncrement(&np_2c_r);
}
static int np_alive(uint64_t npc, uint32_t id) {
    uint32_t now = 0;
    return npc && id && rd(npc + 0x44, &now, 4) && now == id;
}
typedef uint64_t (ABI *NpAddTracker)(uint64_t ngm, uint64_t npc, uint32_t x);
/* Pairs waiting host spawns with waiting local NPCs of the same spawner (oldest first) and binds
 * them (outside the lock: AddNetNpcTracker is guest code). */
static void np_match(void) {
    for (int guard = 0; guard < NP_MAX; ++guard) {
        uint64_t npc = 0;
        uint32_t id = 0, name = 0, old = 0, key = 0;
        AcquireSRWLockExclusive(&np_lock);
        for (unsigned k = 0; k < NP_MAX && !npc; ++k) {
            const unsigned r = (np_remote_next + k) % NP_MAX;   /* oldest first */
            if (np_remote[r].state != 1) continue;
            for (unsigned j = 0; j < NP_MAX; ++j) {
                const unsigned l = (np_local_next + j) % NP_MAX;
                if (np_local[l].state != 1 || np_local[l].key != np_remote[r].key) continue;
                if (!np_alive(np_local[l].npc, np_local[l].id)) { np_local[l].state = 0; continue; }
                if (!np_remote[r].key && (np_local[l].f0 != np_remote[r].f0 || np_local[l].f40 != np_remote[r].f40)) continue;
                npc = np_local[l].npc; id = np_local[l].id; old = np_local[l].name;
                name = np_remote[r].name; key = np_remote[r].key;
                np_local[l].state = 2;
                np_local[l].name = name;
                np_remote[r].state = 0;
                break;
            }
        }
        ReleaseSRWLockExclusive(&np_lock);
        if (!npc) return;
        uint32_t x = 0;
        rd(npc + 0x3708, &x, 4);
        if (!np_alive(npc, id) || !wr(npc + 0x68, &name, 4)) { InterlockedIncrement(&np_bind_fail); continue; }
        const uint64_t ok = ((NpAddTracker)(uintptr_t)(image + 0x644fa0))((uint64_t)(uintptr_t)image + U2_NGM, npc, x);
        if (!(ok & 0xff)) InterlockedIncrement(&np_bind_fail);
        else InterlockedIncrement(&np_bound);
        np_msg(4, old, name, key, npc);
    }
}
static void ABI np_spawn_execute(uint64_t ev, uint64_t from) {
    (void)from;
    if (np_is_host()) return;
    const uint32_t name = *(uint32_t *)(uintptr_t)(ev + 0xc), key = *(uint32_t *)(uintptr_t)(ev + 0x10);
    if (InterlockedIncrement(&np_spawn_r) <= 64) np_msg(3, name, key, *(uint32_t *)(uintptr_t)(ev + 0x14), 0);
    AcquireSRWLockExclusive(&np_lock);
    const unsigned r = np_remote_next++ % NP_MAX;
    np_remote[r].name = name;
    np_remote[r].key = key;
    np_remote[r].f0 = *(uint32_t *)(uintptr_t)(ev + 0x14);
    np_remote[r].f40 = *(uint32_t *)(uintptr_t)(ev + 0x18);
    np_remote[r].state = 1;
    np_remote[r].tried = 0;
    np_remote[r].t = GetTickCount64();
    ReleaseSRWLockExclusive(&np_lock);
    np_match();
}
static void np_local_record(uint64_t npc, uint32_t key);
/* Npc::Init at 0x23bb5a (net game, after the host's naming): npc = r13, spawn info = r14. */
static void ABI np_local_init(uint64_t npc, uint64_t info) {
    if (np_is_host()) return;
    uint64_t spawner = 0;
    uint32_t key = 0;
    if (info && rd(info + 0x40, &spawner, 8) && spawner) rd(spawner + 0x28, &key, 4);
    np_local_record(npc, key);
}
static void np_local_record(uint64_t npc, uint32_t key) {
    uint32_t id = 0, f0 = 0, f40 = 0, name = 0;
    rd(npc + 0x44, &id, 4);
    rd(npc + 0x2568, &f0, 4);
    rd(npc + 0x2568 + 0x40, &f40, 4);
    rd(npc + 0x68, &name, 4);
    AcquireSRWLockExclusive(&np_lock);
    const unsigned l = np_local_next++ % NP_MAX;
    np_local[l].npc = npc;
    np_local[l].id = id;
    np_local[l].key = key;
    np_local[l].f0 = f0;
    np_local[l].f40 = f40;
    np_local[l].name = name;
    np_local[l].state = 1;
    ReleaseSRWLockExclusive(&np_lock);
    if (InterlockedIncrement(&np_local_n) <= 64) np_msg(2, name, key, f0, npc);
    np_match();
}
/* 0x240fcf (an NPC ends, net game): the host sends NpcDespawn as before; a joiner does not. */
typedef void (ABI *NpSendDespawn)(uint32_t name);
static void ABI np_despawn_site(uint64_t npc) {
    uint32_t name = 0;
    rd(npc + 0x68, &name, 4);
    if (np_is_host()) { ((NpSendDespawn)(uintptr_t)(image + 0x6135a0))(name); return; }
    InterlockedIncrement(&np_own_skip);
    AcquireSRWLockExclusive(&np_lock);
    for (unsigned i = 0; i < NP_MAX; ++i)
        if (np_local[i].state && np_local[i].npc == npc) np_local[i].state = 0;
    ReleaseSRWLockExclusive(&np_lock);
    np_msg(6, name, 0, 0, npc);
}
static void ABI np_move_seen(void) { InterlockedIncrement(&np_moves); }
static void ABI np_move_found(void) { InterlockedIncrement(&np_moves_found); }
/* Run np03: the joiner died in the hit event's Execute (type 0xd, 0x627da0 -> 0x4c09e1 -> ... ->
 * GetCurrentFrameNumber assert "pRenderFrameParams", fault 0xc79928): received events run from
 * coop_frame, i.e. at the entry of GameLoopUpdate (0x3facb0), where the frame params pointer
 * 0x159d498 is NULL (set at 0x3fae74, cleared at 0x3fb9fd). The NPC events whose Execute plays
 * hits (0xd, 0x10, 0x11) or ends an NPC (0x26) therefore wait, with a reference held (+0xa), until
 * just after the pointer is set (0x3fae7b, np_frame_start), and run there in arrival order.
 * Run np04 (the same death in type 7, the bullet event 0x62b9d0 -> 0x4c4090 -> 0x249551): every
 * gameplay event waits, i.e. the types whose vtables lie below 0x1040d00 (bullets, hits, grenades,
 * explosions, NPC events, ...) plus 0x10/0x11; the session events (vtables 0x1040d00 and up:
 * start/end of the game, player setup 0x12, ...) still run at once, as before; CharacterMove 0xc
 * waits too (it now moves bound NPCs on the joiner).
 * The co-op events whose Read/Write are still bare `ret` (C5 of the U3 study: 0x2f 0x31 0x33-0x37
 * 0x39 0x3d 0x43) arrive without their fields; their Executes are skipped (counted). Tables from
 * the factory 0x6180f0 and the ELF relocations (scratchpad np/evtypes.py). */
#define NP_FRAME_PARAMS 0x159d498
static const struct { uint8_t type; uint32_t vt, exec; } np_defer_types[] = {
    {0x07, 0x1040cb0, 0x62b9d0}, {0x08, 0x1040c60, 0x62ada0}, {0x09, 0x1040c10, 0x629460},
    {0x0a, 0x1040bc0, 0x629040}, {0x0d, 0x1040b70, 0x627da0}, {0x0e, 0x1040b20, 0x627060},
    {0x0f, 0x1040ad0, 0x6268b0}, {0x10, 0x10412f0, 0x633cb0}, {0x11, 0x10412a0, 0x632d50},
    {0x0c, 0x1041340, 0x6382d0},   /* CharacterMove too (np05): a bound NPC's moves now apply on the joiner */
    {0x16, 0x1040a80, 0x6262a0}, {0x17, 0x1040a30, 0x625890}, {0x18, 0x10409e0, 0x624fb0},
    {0x19, 0x1040990, 0x6242f0}, {0x1d, 0x1040940, 0x623ce0}, {0x1e, 0x10408f0, 0x623b80},
    {0x1f, 0x10408a0, 0x6236b0}, {0x20, 0x1040850, 0x622d60}, {0x21, 0x1040800, 0x622760},
    {0x24, 0x10407b0, 0x622090}, {0x26, 0x1040760, 0x621f80}, {0x27, 0x1040710, 0x621180},
    {0x28, 0x10406c0, 0x6207e0}, {0x2a, 0x1040670, 0x620100}, {0x2b, 0x1040620, 0x61fe10},
    {0x2c, 0x10405d0, 0x61fa70}, {0x30, 0x1040530, 0x61f700}, {0x32, 0x10403f0, 0x61e8a0},
    {0x38, 0x1040210, 0x61d3b0}, {0x3a, 0x10404e0, 0x61f390}, {0x3b, 0x1040490, 0x61f040},
    {0x40, 0x10400d0, 0x61cb50}, {0x42, 0x1040080, 0x61bef0}, {0x44, 0x103ffe0, 0x61ba40},
    {0x46, 0x103ff40, 0x61b7c0}, {0x47, 0x103fef0, 0x61b660}, {0x48, 0x103fea0, 0x61b410},
    {0x49, 0x103fe50, 0x61b230}, {0x4a, 0x103fe00, 0x61afe0}, {0x4c, 0x103fdb0, 0x61afa0},
    {0x4d, 0x103fd60, 0x61a950}, {0x4e, 0x103fd10, 0x619f50}, {0x4f, 0x103fcc0, 0x619db0},
};
static const struct { uint8_t type; uint32_t vt, exec; } np_block_types[] = {
    {0x2f, 0x1040580, 0x61fa10}, {0x31, 0x1040440, 0x61eb90}, {0x33, 0x10403a0, 0x61e710},
    {0x34, 0x1040350, 0x61e610}, {0x35, 0x1040300, 0x61e2b0}, {0x36, 0x10402b0, 0x61e1c0},
    {0x37, 0x1040260, 0x61d6d0}, {0x39, 0x10401c0, 0x61d280}, {0x3d, 0x1040120, 0x61d1e0},
    {0x43, 0x1040030, 0x61be10},
};
typedef void (ABI *NpExec)(uint64_t ev, uint64_t from);
typedef void (ABI *NpRelease)(uint64_t ev);
static struct { uint64_t ev, from, exec; } np_defer_q[512], np_defer_run[512];
static unsigned np_defer_n;
static SRWLOCK np_qlock = SRWLOCK_INIT;
static volatile LONG np_deferred, np_defer_full, np_blocked, np_exec_count[256], np_grenade_skip;
static int np_npc_grenades = 1;   /* GOW3_COOP_NPC_GRENADES=0: NPC grenades are not created on receivers */
typedef void (ABI *NpSer)(uint64_t ev, uint64_t stream);
static void ABI np_g8_write(uint64_t ev, uint64_t stream) {
    ((NpSer)(uintptr_t)(image + 0x62b250))(ev, stream);
    np_put(stream, *(uint32_t *)(uintptr_t)(ev + 0x40), 32);
}
static void ABI np_g8_read(uint64_t ev, uint64_t stream) {
    ((NpSer)(uintptr_t)(image + 0x62b650))(ev, stream);
    const uint32_t id = np_get(stream, 32);
    memcpy((void *)(uintptr_t)(ev + 0x40), &id, 4);
}
static void ABI np_exec_block(uint64_t ev, uint64_t from) {
    (void)ev;
    (void)from;
    InterlockedIncrement(&np_blocked);
}
static void ABI np_exec_defer(uint64_t ev, uint64_t from) {
    uint64_t vt = 0, exec = 0;
    memcpy(&vt, (void *)(uintptr_t)ev, 8);
    for (unsigned k = 0; k < sizeof(np_defer_types) / sizeof(np_defer_types[0]); ++k)
        if (vt == (uint64_t)(uintptr_t)image + np_defer_types[k].vt) {
            exec = (uint64_t)(uintptr_t)image + np_defer_types[k].exec;
            InterlockedIncrement(&np_exec_count[np_defer_types[k].type]);
            break;
        }
    if (!exec) return;
    if (vt == (uint64_t)(uintptr_t)image + 0x1040c60) {
        /* Run np05: the joiner died in a host NPC's grenade, type 8 (Execute 0x62ada0 spawns a
         * NetGrenade 0x1caece01 whose Init looks up its settings symbol, event +0x40 -> +0x324,
         * with the raw find 0xa15300 at 0xb0895a, then [entry]: NULL -> assert "settings" -> fault
         * 0xb089a8; in run np06 the entry existed and its value was NULL).
         * A grenade whose settings are not loaded on this machine is not created (counted, id
         * reported); the thrower's damage stays the owner's.
         * Runs np07/np09: the joiner still died in GetBulletSettings from a host NPC's grenade
         * although the event's own settings (+0x40, the thrower's projectile +0x324 at the sender
         * 0x60d105) were loaded here. So a grenade thrown by an NPC (+0xc == 0: +0x14 is the NPC's
         * name; a player thrower has +0xc != 0 and its id in +0x14, Execute 0x62afe6) is not
         * created on the receiving machine at all; player grenades still are.
         * Run np10 showed why: the receiver's +0x40 was garbage (0, 7, 0xd, 0x3ff5fb0): type 8's own
         * Write 0x62b250 / Read 0x62b650 carry +0x44 (weapon index) but NOT +0x40, which the
         * Execute uses (0x62afb5). np11: +0x40 travels after the game's fields (np_g8_read /
         * np_g8_write), so NPC grenades are created again when their settings are loaded here;
         * GOW3_COOP_NPC_GRENADES=0 keeps skipping them. */
        uint32_t id = 0, by_player = 0;
        memcpy(&by_player, (void *)(uintptr_t)(ev + 0xc), 4);
        if (!by_player && !np_npc_grenades) {
            if (InterlockedIncrement(&np_grenade_skip) <= 8) np_msg(9, *(uint32_t *)(uintptr_t)(ev + 0x40), 1, 0, 0);
            return;
        }
        memcpy(&id, (void *)(uintptr_t)(ev + 0x40), 4);
        typedef uint64_t (ABI *NpFind)(uint32_t id);
        const uint64_t entry = id ? ((NpFind)(uintptr_t)(image + 0xa15300))(id) : 1;
        if (!entry || (id && !na_q(entry))) {   /* no entry, or an entry whose value is NULL (run np06) */
            if (InterlockedIncrement(&np_grenade_skip) <= 8) np_msg(9, id, 0, 0, 0);
            return;
        }
    }
    const uint64_t params = *(volatile uint64_t *)(uintptr_t)(image + NP_FRAME_PARAMS);
    if (!params) {
        AcquireSRWLockExclusive(&np_qlock);
        if (np_defer_n < 512) {
            ++*(volatile uint8_t *)(uintptr_t)(ev + 0xa);
            np_defer_q[np_defer_n].ev = ev;
            np_defer_q[np_defer_n].from = from;
            np_defer_q[np_defer_n].exec = exec;
            ++np_defer_n;
            ReleaseSRWLockExclusive(&np_qlock);
            InterlockedIncrement(&np_deferred);
            return;
        }
        ReleaseSRWLockExclusive(&np_qlock);
        InterlockedIncrement(&np_defer_full);
    }
    ((NpExec)(uintptr_t)exec)(ev, from);
}
/* Run np07: the type-8 guard above never fired, yet the joiner died again in GetBulletSettings
 * (projectile-process.cpp, 0xb08950: [obj+0x324] -> raw find 0xa15300 -> [entry] -> +0xb0; NULL ->
 * assert "settings" -> fault 0xb089a8), reached from the NetGrenade Init. So the guard sits in
 * GetBulletSettings itself (0xb08954, rdi = the projectile): a settings id that is not loaded on
 * this machine is replaced by the last one that was (every bullet passes here), reported once per
 * id. The projectile then flies with those settings instead of ending the game. */
static volatile LONG np_settings_fixed;
static volatile uint32_t np_settings_good;
static void ABI np_bullet_settings(uint64_t proj) {
    typedef uint64_t (ABI *NpFind)(uint32_t id);
    uint32_t id = 0;
    memcpy(&id, (void *)(uintptr_t)(proj + 0x324), 4);
    const uint64_t entry = ((NpFind)(uintptr_t)(image + 0xa15300))(id);
    if (entry && *(volatile uint64_t *)(uintptr_t)entry) { np_settings_good = id; return; }
    const uint32_t good = np_settings_good;
    if (InterlockedIncrement(&np_settings_fixed) <= 8) np_msg(10, id, good, 0, proj);
    if (!good) {   /* nothing loaded seen yet: the game's own assert follows; say which id first */
        printf("CoopMP: NPC sync: projectile %llx: bullet settings %08x not loaded, no fallback\n", (unsigned long long)proj, id);
        fflush(stdout);
        return;
    }
    memcpy((void *)(uintptr_t)(proj + 0x324), &good, 4);
}
/* Joiner: a host NPC whose spawner has no local NPC after GOW3_COOP_NPC_SPAWN_MS (default 1000;
 * 0 = never) is spawned here from that spawner (0x56c7b0 lookup, 0x4be330 spawn: the joiner's
 * Npc::Init then records it and np_match binds it). A spawner that already holds a live process
 * (its handle +0x30/+0x38) gets that one recorded instead. */
static unsigned np_spawn_ms = 1000;
static volatile LONG np_spawned, np_spawn_fail;
typedef uint64_t (ABI *NpLookup)(uint64_t packages, uint32_t id);
typedef uint64_t (ABI *NpSpawnFn)(uint64_t spawner, uint64_t a, uint64_t b, uint64_t c);
static void np_spawn_missing(void) {
    if (!np_spawn_ms || np_is_host()) return;
    const ULONGLONG now = GetTickCount64();
    for (int guard = 0; guard < 8; ++guard) {
        uint32_t key = 0, name = 0;
        AcquireSRWLockExclusive(&np_lock);
        for (unsigned k = 0; k < NP_MAX && !key; ++k) {
            const unsigned r = (np_remote_next + k) % NP_MAX;
            if (np_remote[r].state != 1 || np_remote[r].tried || !np_remote[r].key || now - np_remote[r].t < np_spawn_ms) continue;
            np_remote[r].tried = 1;
            key = np_remote[r].key;
            name = np_remote[r].name;
        }
        ReleaseSRWLockExclusive(&np_lock);
        if (!key) return;
        const uint64_t sp = ((NpLookup)(uintptr_t)(image + 0x56c7b0))((uint64_t)(uintptr_t)image + 0x1130390, key);
        if (!sp) { InterlockedIncrement(&np_spawn_fail); np_msg(7, name, key, 0, 0); continue; }
        uint64_t h = 0, live = 0;
        uint32_t id = 0;
        rd(sp + 0x30, &h, 8);
        rd(sp + 0x38, &id, 4);
        if (h && id) { live = na_q(h); if (live && na_d(live + 0x44) != id) live = 0; }
        if (live) {
            /* the joiner's own copy (spawned before our hook saw it, or by the script) */
            int known = 0;
            AcquireSRWLockShared(&np_lock);
            for (unsigned i = 0; i < NP_MAX; ++i) known |= np_local[i].state && np_local[i].npc == live;
            ReleaseSRWLockShared(&np_lock);
            if (!known) np_local_record(live, key);
            np_msg(8, name, key, 1, live);
            continue;
        }
        const uint64_t proc = ((NpSpawnFn)(uintptr_t)(image + 0x4be330))(sp, 0, 0, 0);
        if (proc) InterlockedIncrement(&np_spawned); else InterlockedIncrement(&np_spawn_fail);
        np_msg(8, name, key, 0, proc);
    }
}
/* GameLoopUpdate 0x3fae7b, the frame params are set: deferred Executes, then missing NPCs. */
static void ABI np_frame_start(void) {
    unsigned n;
    AcquireSRWLockExclusive(&np_qlock);
    n = np_defer_n;
    memcpy(np_defer_run, np_defer_q, sizeof(np_defer_run[0]) * n);
    np_defer_n = 0;
    ReleaseSRWLockExclusive(&np_qlock);
    for (unsigned i = 0; i < n; ++i) {
        ((NpExec)(uintptr_t)np_defer_run[i].exec)(np_defer_run[i].ev, np_defer_run[i].from);
        volatile uint8_t *refs = (volatile uint8_t *)(uintptr_t)(np_defer_run[i].ev + 0xa);
        if (--*refs == 0) {
            uint64_t vt = 0, release = 0;
            memcpy(&vt, (void *)(uintptr_t)np_defer_run[i].ev, 8);
            memcpy(&release, (void *)(uintptr_t)(vt + 8), 8);
            ((NpRelease)(uintptr_t)release)(np_defer_run[i].ev);
        }
    }
    np_spawn_missing();
}
static int np_slot(uint64_t vt, unsigned off, uint64_t expect, void *fn) {
    uint64_t now = 0;
    const uint64_t a = (uint64_t)(uintptr_t)image + vt + off, want = (uint64_t)(uintptr_t)image + expect;
    const uint64_t to = (uint64_t)(uintptr_t)fn;
    if (!rd(a, &now, 8)) return 0;
    if (now == to) return 1;
    if (now != want) { printf("CoopMP: NPC sync: vtable %#llx+%#x holds %#llx\n", (unsigned long long)vt, off, (unsigned long long)(now - (uint64_t)(uintptr_t)image)); return 0; }
    /* vtables sit in read-only data: WriteProcessMemory writes code pages (execute-read) but not
     * PAGE_READONLY ones, so the slot is opened for the write */
    DWORD old;
    if (!VirtualProtect((void *)(uintptr_t)a, 8, PAGE_READWRITE, &old)) return 0;
    memcpy((void *)(uintptr_t)a, &to, 8);
    VirtualProtect((void *)(uintptr_t)a, 8, old, &old);
    return 1;
}
static int install_npc_sync(void) {
    static int done;
    if (done) return done > 0;
    static const unsigned char s_init[7] = {0x48, 0x8b, 0x83, 0x00, 0x0b, 0x00, 0x00};  /* 0x23bb5a mov rax, [rbx+0xb00] */
    static const unsigned char s_desp[8] = {0x8b, 0x7b, 0x68, 0xe8, 0xc9, 0x25, 0x3d, 0x00}; /* 0x240fcf mov edi,[rbx+0x68]; call 0x6135a0 */
    static const unsigned char s_mv[6] = {0x89, 0xb5, 0xc4, 0xfe, 0xff, 0xff};           /* 0x638332 mov [rbp-0x13c], esi */
    static const unsigned char s_mvf[6] = {0x4c, 0x8b, 0x30, 0x4d, 0x85, 0xf6};          /* 0x6383d7 mov r14,[rax]; test r14,r14 */
    static const unsigned char ret1[1] = {0xc3};
    static const unsigned char a_init[] = {0x4c, 0x89, 0xef, 0x4c, 0x89, 0xf6};   /* mov rdi, r13; mov rsi, r14 */
    static const unsigned char a_desp[] = {0x48, 0x8b, 0x3b};                      /* mov rdi, [rbx] (saved rbx = npc) */
    static const unsigned char a_none[1] = {0x90};
    /* Each part is checked and installed on its own; a part whose bytes differ is reported. */
    static const struct { uint64_t site; const unsigned char *expect; unsigned len; } chk[] = {
        {0x62f240, ret1, 1}, {0x62f230, ret1, 1}, {0x62f220, ret1, 1}, {0x622080, ret1, 1}, {0x622070, ret1, 1},
        {0x61fe00, ret1, 1}, {0x23bb5a, s_init, 7}, {0x240fcf, s_desp, 8}, {0x638332, s_mv, 6}, {0x6383d7, s_mvf, 6},
    };
    unsigned bad = 0;
    for (unsigned i = 0; i < sizeof(chk) / sizeof(chk[0]); ++i) {
        if (!memcmp(image + chk[i].site, chk[i].expect, chk[i].len)) continue;
        bad |= 1u << i;
        printf("CoopMP: NPC sync: %#llx is", (unsigned long long)chk[i].site);
        for (unsigned k = 0; k < 8; ++k) printf(" %02x", image[chk[i].site + k]);
        printf(" (expected %02x..)\n", chk[i].expect[0]);
    }
    int ok = 1, parts = 0;
#define NP_PART(bit, call) do { if (!(bad >> (bit) & 1)) { if (call) ++parts; else { ok = 0; printf("CoopMP: NPC sync: part %d failed\n", bit); } } else ok = 0; } while (0)
    NP_PART(0, np_slot(NP_SPAWN_VT, 0x10, 0x62f240, (void *)np_spawn_read));
    NP_PART(1, np_slot(NP_SPAWN_VT, 0x18, 0x62f230, (void *)np_spawn_write));
    NP_PART(2, np_slot(NP_SPAWN_VT, 0x20, 0x62f220, (void *)np_spawn_execute));
    NP_PART(3, np_slot(NP_DESPAWN_VT, 0x10, 0x622080, (void *)np_despawn_read));
    NP_PART(4, np_slot(NP_DESPAWN_VT, 0x18, 0x622070, (void *)np_despawn_write));
    NP_PART(5, np_slot(NP_EV2C_VT, 0x10, 0x61fe00, (void *)np_2c_read) &&
               np_slot(0x1040c60, 0x10, 0x62b650, (void *)np_g8_read) && np_slot(0x1040c60, 0x18, 0x62b250, (void *)np_g8_write));
    {
        const char *g = getenv("GOW3_COOP_NPC_GRENADES");
        if (g && g[0] == '0') np_npc_grenades = 0;
    }
    NP_PART(6, mid_hook(0x23bb5a, s_init, 7, a_init, sizeof(a_init), (void *)np_local_init, s_init, 7, 0x23bb61));
    NP_PART(7, mid_hook(0x240fcf, s_desp, 8, a_desp, sizeof(a_desp), (void *)np_despawn_site, NULL, 0, 0x240fd7));
    NP_PART(8, mid_hook(0x638332, s_mv, 6, a_none, 0, (void *)np_move_seen, s_mv, 6, 0x638338));
    NP_PART(9, mid_hook(0x6383d7, s_mvf, 6, a_none, 0, (void *)np_move_found, s_mvf, 6, 0x6383dd));
    {
        /* keep this hook at 0x3fae7b..0x3fae81 only: runtime_coop_menu.c hooks 0x3fae85 and checks from 0x3fae82 */
        /* deferred Executes (np_exec_defer) and the in-frame drain at 0x3fae7b (lea rax, [rip+..] = 0x144e791:
         * replayed as mov rax, imm64) */
        static const unsigned char s_fs[7] = {0x48, 0x8d, 0x05, 0x0f, 0x39, 0x05, 0x01};
        unsigned char r_fs[10] = {0x48, 0xb8};
        const uint64_t target = (uint64_t)(uintptr_t)image + 0x144e791;
        memcpy(r_fs + 2, &target, 8);
        const char *ms = getenv("GOW3_COOP_NPC_SPAWN_MS");
        if (ms && *ms) np_spawn_ms = (unsigned)atoi(ms);
        int defer = mid_hook(0x3fae7b, s_fs, 7, a_none, 0, (void *)np_frame_start, r_fs, sizeof(r_fs), 0x3fae82);
        for (unsigned k = 0; defer && k < sizeof(np_defer_types) / sizeof(np_defer_types[0]); ++k)
            defer = np_slot(np_defer_types[k].vt, 0x20, np_defer_types[k].exec, (void *)np_exec_defer);
        for (unsigned k = 0; defer && k < sizeof(np_block_types) / sizeof(np_block_types[0]); ++k)
            defer = np_slot(np_block_types[k].vt, 0x20, np_block_types[k].exec, (void *)np_exec_block);
        if (defer) ++parts; else { ok = 0; puts("CoopMP: NPC sync: deferred Executes / frame-start hook (0x3fae7b) failed"); }
        /* Run np08: THE JOINER'S SHOTS KILLED A HOST SOLDIER, and the host died in the kill's points:
         * GetCoopPointsForKill (moneymanager.cpp, 0x5decf0, from the 0x11 apply 0x632d50 -> 0x645c50 ->
         * 0x66e445) asserts "archetype isn't in the net-npc-info list" (*net-npc-info* 0xa977dad3 of
         * net.bin lacks this soldier's archetype) and the assert is fatal here. The not-found branch
         * 0x5ded4e now goes straight to the function's own "0 points" path 0x5dedda. */
        static const unsigned char s_pts[7] = {0x4c, 0x8b, 0x2d, 0x23, 0xf2, 0xf8, 0x00};   /* mov r13, [rip+0xf8f223] */
        if (!memcmp(image + 0x5ded4e, s_pts, 7)) {
            unsigned char j[7] = {0xe9, 0, 0, 0, 0, 0x90, 0x90};
            const int32_t rel = 0x5dedda - (0x5ded4e + 5);
            memcpy(j + 1, &rel, 4);
            if (!wr((uint64_t)(uintptr_t)image + 0x5ded4e, j, 7)) puts("CoopMP: NPC sync: co-op points patch (0x5ded4e) failed");
        } else if (image[0x5ded4e] != 0xe9) puts("CoopMP: NPC sync: co-op points site 0x5ded4e differs");
        static const unsigned char s_bs[6] = {0x8b, 0xbf, 0x24, 0x03, 0x00, 0x00};   /* 0xb08954 mov edi, [rdi+0x324] */
        if (!mid_hook(0xb08954, s_bs, 6, a_none, 0, (void *)np_bullet_settings, s_bs, 6, 0xb0895a))
            puts("CoopMP: NPC sync: bullet settings guard (0xb08954) NOT installed");
    }
#undef NP_PART
    printf("CoopMP: NPC sync: %d of 11 parts installed (joiner spawns missing NPCs after %u ms)\n", parts, np_spawn_ms);
    done = ok ? 1 : -1;
    return ok;
}
/* runtime_coop_mp_frame (main thread): queued messages and the 10 s line. */
static void np_frame(void) {
    static ULONGLONG last;
    static LONG seen_moves = -1;
    int printed = 0;
    while (np_ring_r < np_ring_w) {
        const unsigned k = (unsigned)np_ring_r & 255;
        if (!np_ring[k].kind) break;   /* being written */
        MemoryBarrier();
        const int kind = np_ring[k].kind;
        const uint32_t a = np_ring[k].a, b = np_ring[k].b, c = np_ring[k].c;
        const unsigned long long p = (unsigned long long)np_ring[k].p;
        np_ring[k].kind = 0;
        ++np_ring_r;
        printed = 1;
        if (kind == 1) printf("CoopMP: NPC sync: host sent NpcSpawn NetNpc %08x (spawner %08x, info %08x)\n", a, b, c);
        else if (kind == 2) printf("CoopMP: NPC sync: joiner NPC %llx init (name %08x, spawner %08x, info %08x)\n", p, a, b, c);
        else if (kind == 3) printf("CoopMP: NPC sync: joiner got NpcSpawn NetNpc %08x (spawner %08x, info %08x)\n", a, b, c);
        else if (kind == 4) printf("CoopMP: NPC sync: joiner NPC %llx (name %08x) bound to host NetNpc %08x (spawner %08x)\n", p, a, b, c);
        else if (kind == 5) printf("CoopMP: NPC sync: NpcDespawn received for %08x\n", a);
        else if (kind == 6) printf("CoopMP: NPC sync: joiner NPC %llx (%08x) ended, its NpcDespawn not sent\n", p, a);
        else if (kind == 9) printf("CoopMP: NPC sync: grenade event skipped: %s (settings %08x)\n",
                                   b ? "thrown by an NPC" : "its settings are not loaded here", a);
        else if (kind == 10) printf("CoopMP: NPC sync: projectile %llx: settings %08x not loaded here, using %08x\n", p, a, b);
        else if (kind == 7) printf("CoopMP: NPC sync: joiner has no spawner %08x for host NetNpc %08x\n", b, a);
        else if (kind == 8) printf("CoopMP: NPC sync: joiner %s %llx from spawner %08x for host NetNpc %08x\n",
                                   c ? "found its own NPC" : "spawned NPC", p, b, a);
    }
    const ULONGLONG now = GetTickCount64();
    if (now - last >= 10000 && (np_spawn_w || np_local_n || np_spawn_r || np_moves != seen_moves)) {
        last = now;
        seen_moves = np_moves;
        int waiting_l = 0, waiting_r = 0;
        AcquireSRWLockShared(&np_lock);
        for (unsigned i = 0; i < NP_MAX; ++i) { waiting_l += np_local[i].state == 1; waiting_r += np_remote[i].state == 1; }
        ReleaseSRWLockShared(&np_lock);
        int32_t counter = 0;
        rd((uint64_t)(uintptr_t)image + U2_NGM + 0x1c8c, &counter, 4);
        printf("CoopMP: NPC sync: %s; spawns sent %ld / received %ld, despawns sent %ld / received %ld, 0x2c read %ld; "
               "local NPCs %ld, bound %ld (failed %ld), waiting %d local / %d host; own despawns not sent %ld; "
               "NPC moves received %ld, matched %ld; NetNpc counter %d; joiner spawns %ld (failed %ld); "
               "executes: bullet %ld, hit %ld, request %ld, apply %ld, despawn %ld (%ld waited for the frame, %ld queue full, "
               "%ld field-less co-op events skipped)\n",
               np_is_host() ? "host" : "joiner", (long)np_spawn_w, (long)np_spawn_r, (long)np_despawn_w, (long)np_despawn_r,
               (long)np_2c_r, (long)np_local_n, (long)np_bound, (long)np_bind_fail, waiting_l, waiting_r, (long)np_own_skip,
               (long)np_moves, (long)np_moves_found, counter, (long)np_spawned, (long)np_spawn_fail,
               (long)np_exec_count[7], (long)np_exec_count[0xd], (long)np_exec_count[0x10], (long)np_exec_count[0x11],
               (long)np_exec_count[0x26], (long)np_deferred, (long)np_defer_full, (long)np_blocked);
        printed = 1;
    }
    if (printed) fflush(stdout);
}

/* ---- competitive maps and spawns on every map (mm- agent, 25 Oct 2026) -------------------------
 * coop-mp.md "(mm- agent)" entries. The competitive spawn choices (first spawn 0x644220, respawn
 * 0x642d20) and GetSpawnLocator (0x662fa0) read only the map's net pak level ("<map>-multi-pak"),
 * which the Collection lacks for every classic map; GetSpawnLocator then read [NULL+0x9a0].
 *  1. Classic maps (GOW3_COOP_MP_CLASSIC=0 = off): the net game's *net-maps* entry gets a shipped
 *     net pak (the map's "-multi-npc" level where one ships: Village, Plaza, Sanctuary, Lost City;
 *     else its main level) and the level list loses the missing net pak (+ the -npc level).
 *  2. GOW3_COOP_MP_SPAWNS=0 = off:
 *     - GetSpawnLocator 0x662fc9: our ids first, then the spawner in ANY loaded level (U3's order,
 *       U3 0x7838b0; U2 0x56c7b0), then the game's net pak lookup (NULL-checked);
 *     - entry detours on 0x644220 / 0x642d20: when the game finds no spawner (0), we pick one from the
 *       loaded levels: online spawners (any level), else the -npc level's Soldier spawners, else our
 *       per-map positions (mm_points), else weapon pickups, else effect positions (last resort).
 *       First spawn: team 0 / 1 on opposite halves of the map (main axis of the points); every pick
 *       is the point farthest from the enemies (live positions), never on top of another player.
 *  GOW3_COOP_MP_SPAWN_PROBE=<s> (test): the local player is killed every <s> seconds, respawns after
 *  1 s at the next candidate in turn, and where he stands 3 s later is logged (per-map positions). */
#define MM_SOLDIER 0xfd2c9c09u
#define MM_PICKUP 0x262e333bu
#define MM_PART 0xa51c3c09u
#define MM_SIMPLE 0x516a073bu
#define MM_LIGHT 0x18ae0208u
#define MM_NAVBLOCK 0xdadf1043u
#define MM_ONLINE 0x5bfae217u
#define MM_SYN 0x6d6d0000u   /* our point ids: 'mm' << 16 | index */
#define MMS_MAX 256
typedef uint64_t (ABI *MmLookup)(uint64_t mgr, uint32_t id);
typedef void *(ABI *MmLocator)(void *out, uint64_t spawner);
typedef void (ABI *MmVoid)(void);
/* classic maps: missing net pak -> the shipped level that becomes the net pak (+ an extra level) */
static const struct { const char *pak, *net_pak, *extra; } mm_maps[] = {
    {"village-multi-pak", "village-multi-npc", "village-multi-npc"},
    {"warzone-multi-pak", "warzone-multi-npc", "warzone-multi-npc"},
    {"gompa-multi-pak", "gompa-multi-npc", "gompa-multi-npc"},
    {"shambhala-multi-pak", "shambhala-multi-npc", "shambhala-multi-npc"},
    {"ice-cave-multi-pak", "ice-cave-multi", NULL},
    {"train-wreck-multi-pak", "train-wreck-multi", NULL},
    {"treasure-multi-pak", "treasure-multi", NULL},
};
/* per-map player positions (x, y, z, yaw degrees) where a map ships no spawner (filled from runs) */
typedef struct { float x, y, z, yaw; } MmPoint;
/* where the player was seen standing (runs mm03 / mm02, spawn probe): start points until the user's own
 * (GOW3_COOP_MP_SPAWN_FILE) are in */
static const MmPoint mm_pts_ice[] = {{-1.05f, -0.20f, -0.91f, 0}};
static const MmPoint mm_pts_temple[] = {{-4.45f, 0.03f, -26.54f, 0}, {-6.94f, 0.41f, -22.47f, 0}};
static const MmPoint mm_pts_train[] = {{39.73f, 23.12f, 9.46f, 0}, {-43.03f, 23.34f, 11.56f, 0}, {-40.02f, 24.00f, 3.68f, 0},
                                       {-22.10f, 17.78f, 46.78f, 0}, {-15.08f, 17.70f, 46.09f, 0}};
static const struct { const char *net_pak; const MmPoint *pts; int n; } mm_points[] = {
    {"ice-cave-multi", mm_pts_ice, 1},
    {"treasure-multi", mm_pts_temple, 2},
    {"train-wreck-multi", mm_pts_train, 5},
};
/* GOW3_COOP_MP_SPAWN_FILE (default <user folder>/spawn-points.txt, also tried without an instance
 * suffix "-i2"): lines "<net game> x y z qx qy qz qw" (the F8 recorder, GOW3_SPAWN_RECORD in
 * runtime_coop.c) are spawn points for the map of that net game, in every net game on that map. */
#define MM_FILE_MAX 128
static struct { float p[3], q[4]; } mm_file_pts[MM_FILE_MAX];
static int mm_file_n;
static int mm_classic_on = 1, mm_spawns_on = 1, mm_competitive, mm_probe_secs;
static uint32_t mm_net_pak;            /* sid of the net pak level in use (our rewrite or the game's) */
typedef struct { uint32_t id; char cls; char name[27]; float p[4], q[4]; } MmCand;
static MmCand mm_c[MMS_MAX];
static int mm_n, mm_built;
static char mm_cls = '-';
static uint32_t mm_sig;
static float mm_center[2], mm_axis[2];
static SRWLOCK mm_lock = SRWLOCK_INIT;
static volatile LONG mm_game_picks, mm_our_picks, mm_failed, mm_located, mm_located_ours, mm_builds;
static struct { ULONGLONG t; float p[3]; int team; } mm_recent[8];
static unsigned mm_recent_next, mm_turn;
/* log ring (hooks run on game threads: printed by mm_frame) */
static struct { int kind, slot, team, idx; uint32_t id; float p[3]; } mm_log[32];
static volatile LONG mm_log_w;
static LONG mm_log_r;
static int mm_probe_next = -1;          /* probe: next candidate for the local player */
static unsigned char mm_state[MMS_MAX];   /* spawn check: 0 untested, 1 the player stood, 2 rejected (fell / kept moving) */
static int mm_untested(int from) {
    for (int k = 0; k < mm_n; ++k) { const int i = (from + k) % mm_n; if (!mm_state[i]) return i; }
    return -1;
}
static int mm_probe_last = -1;          /* probe: the candidate last handed out (log) */
static volatile int mm_local_pick = -1;  /* the candidate last given to the local player (spawn check) */

static uint64_t mm_q(uint64_t a) { uint64_t v = 0; return a && rd(a, &v, 8) ? v : 0; }
static uint32_t mm_d(uint64_t a) { uint32_t v = 0; return a && rd(a, &v, 4) ? v : 0; }
/* tag sid of a spawner as int (value type 1); 0 if absent */
static int mm_tag_int(uint64_t sp, uint32_t tag, int32_t *out) {
    const uint64_t t = mm_q(sp + 0x68);
    const uint64_t e = mm_q(t);
    const uint32_t n = mm_d(t + 8);
    for (uint32_t i = 0; e && i < n && i < 64; ++i) {
        if (mm_d(e + 16 * (uint64_t)i) != tag) continue;
        const uint64_t v = mm_q(e + 16 * (uint64_t)i + 8);
        uint16_t type = 0;
        if (!v || !rd(v, &type, 2) || type != 1) return 0;
        const uint64_t data = mm_q(v + 8);
        return data && rd(data, out, 4);
    }
    return 0;
}
/* a slot's process position (handle at slot+0x10 / +0x18) */
static int mm_slot_pos(uint64_t slot, float *p) {
    const uint64_t h = mm_q(slot + 0x10);
    const uint32_t id = mm_d(slot + 0x18);
    const uint64_t proc = h && id ? mm_q(h) : 0;
    return proc && mm_d(proc + 0x44) == id && rd(proc + 0x110, p, 12);
}
static int mm_skip_name(const char *s) {
    static const char *const bad[] = {"distant", "valley", "godray", "bliz", "smokestack", "igc", "water-fall", "tanker",
                                      "cap-", "killview", "weather", "cutscene", "ss-", "rain", "sky", "splash", "sparks", NULL};
    for (int i = 0; bad[i]; ++i) if (strstr(s, bad[i])) return 1;
    return 0;
}
/* The candidate list from the loaded levels (rebuilt when the set of levels changes). Caller holds mm_lock. */
static void mm_build(void) {
    const uint64_t I = (uint64_t)(uintptr_t)image, base = mm_q(I + 0x1130390);
    if (!base) return;
    uint32_t sig = 0;
    for (int r = 0; r < 180; ++r) {
        const uint64_t rec = base + (uint64_t)r * 0xa60, t = mm_q(rec + 0x9a0);
        if (t) sig = sig * 31 + mm_d(rec + 0x74) + mm_d(t + 0x10);
    }
    if (mm_built && sig == mm_sig) return;
    mm_sig = sig;
    mm_built = 1;
    InterlockedIncrement(&mm_builds);
    static MmCand pool[5][MMS_MAX];   /* A online, B soldier, C pickup, D our points, E effects */
    int cnt[5] = {0};
    for (int r = 0; r < 180; ++r) {
        const uint64_t rec = base + (uint64_t)r * 0xa60, t = mm_q(rec + 0x9a0);
        const uint64_t arr = t ? mm_q(t + 8) : 0;
        const uint32_t n = t ? mm_d(t + 0x10) : 0;
        for (uint32_t j = 0; arr && j < n && j < 4096; ++j) {
            const uint64_t sp = mm_q(arr + 8 * (uint64_t)j);
            if (!sp) continue;
            const uint32_t type = mm_d(sp + 0x40);
            int32_t online = 0;
            char nm[27] = {0};
            const uint64_t name = mm_q(sp + 0x20);
            if (name) rd(name, nm, sizeof(nm) - 1);
            /* C = markers on walkable ground: weapon pickups, nav blockers (on the navmesh), traversal
             * action packs. The -npc levels' Soldier spawners (B) stand in an off-map holding grid
             * (x -83..-106, the same in all four levels; run mm03: the player fell from there): never used. */
            const int marker = type == MM_PICKUP || type == MM_NAVBLOCK || (type == 0 && !strncmp(nm, "traversal-action-pack", 21));
            int k = mm_tag_int(sp, MM_ONLINE, &online) && online ? 0 : type == MM_SOLDIER ? 1 : marker ? 2
                  : (type == MM_PART || type == MM_SIMPLE || type == MM_LIGHT || type == 0) ? 4 : -1;
            if (k < 0 || cnt[k] >= MMS_MAX) continue;
            MmCand *c = &pool[k][cnt[k]];
            memset(c, 0, sizeof(*c));
            memcpy(c->name, nm, sizeof(c->name));
            if (k == 4 && mm_skip_name(c->name)) continue;
            float loc[8] = {0};
            ((MmLocator)(image + 0x4bdfe0))(loc, sp);
            if (loc[0] != loc[0] || (!loc[0] && !loc[1] && !loc[2])) continue;
            memcpy(c->p, loc, 16);
            memcpy(c->q, loc + 4, 16);
            c->id = mm_d(sp + 0x28);
            c->cls = (char)('A' + k);
            if (k == 2 || k == 4) {   /* markers / effects: one per 3 m, 0.3 / 0.5 m up */
                int close_by = 0;
                for (int i = 0; i < cnt[k] && !close_by; ++i) {
                    const float dx = pool[k][i].p[0] - c->p[0], dy = pool[k][i].p[1] - c->p[1], dz = pool[k][i].p[2] - c->p[2];
                    close_by = dx * dx + dy * dy + dz * dz < 9.0f;
                }
                if (close_by) continue;
                c->p[1] += k == 2 ? 0.3f : 0.5f;
            }
            ++cnt[k];
        }
    }
    for (int i = 0; i < mm_file_n && cnt[3] < MMS_MAX; ++i) {
        MmCand *c = &pool[3][cnt[3]++];
        memset(c, 0, sizeof(*c));
        memcpy(c->p, mm_file_pts[i].p, 12);
        c->p[1] += 0.1f;
        c->p[3] = 1.0f;
        memcpy(c->q, mm_file_pts[i].q, 16);
        c->cls = 'D';
        snprintf(c->name, sizeof(c->name), "file-point-%d", i);
    }
    for (size_t m = 0; m < sizeof(mm_points) / sizeof(*mm_points); ++m) {
        if (!mm_points[m].net_pak || sid(mm_points[m].net_pak) != mm_net_pak) continue;
        for (int i = 0; i < mm_points[m].n && cnt[3] < MMS_MAX; ++i) {
            MmCand *c = &pool[3][cnt[3]++];
            memset(c, 0, sizeof(*c));
            c->p[0] = mm_points[m].pts[i].x; c->p[1] = mm_points[m].pts[i].y; c->p[2] = mm_points[m].pts[i].z; c->p[3] = 1.0f;
            const float half = mm_points[m].pts[i].yaw * 3.14159265f / 360.0f;
            c->q[1] = sinf(half); c->q[3] = cosf(half);
            c->cls = 'D';
            snprintf(c->name, sizeof(c->name), "point-%d", i);
        }
    }
    {   /* effects: ground fog / lights / fires first, candles last (probe order and fallback) */
        static const char *const rank[] = {"gfog", "fog-multi", "fog-tw", "light-multi", "fire", "torch", "box", "fog", "candle"};
        int pr[MMS_MAX];
        for (int i = 0; i < cnt[4]; ++i) {
            pr[i] = 8;
            for (int r = 0; r < 9; ++r) if (strstr(pool[4][i].name, rank[r])) { pr[i] = r == 8 ? 9 : r; break; }
        }
        for (int i = 1; i < cnt[4]; ++i)
            for (int j = i; j > 0 && pr[j - 1] > pr[j]; --j) {
                const MmCand c = pool[4][j]; pool[4][j] = pool[4][j - 1]; pool[4][j - 1] = c;
                const int x = pr[j]; pr[j] = pr[j - 1]; pr[j - 1] = x;
            }
    }
    int k = cnt[0] ? 0 : cnt[3] >= 4 ? 3 : cnt[2] ? 2 : 4;
    memset(mm_state, 0, sizeof(mm_state));
    if (!cnt[k]) k = 4;
    /* fewer than 4 of our points: they come first, the markers / effects after them (spawn check) */
    const int lead = !cnt[0] && k != 3 ? cnt[3] : 0;
    memcpy(mm_c, pool[3], sizeof(MmCand) * (size_t)lead);
    const int more = cnt[k] < MMS_MAX - lead ? cnt[k] : MMS_MAX - lead;
    memcpy(mm_c + lead, pool[k], sizeof(MmCand) * (size_t)more);
    mm_n = lead + more;
    mm_cls = mm_n ? (char)('A' + (lead ? 3 : k)) : '-';
    /* centre and main axis (xz) of the points; effect points get our ids and face the centre */
    float cx = 0, cz = 0, sxx = 0, szz = 0, sxz = 0;
    for (int i = 0; i < mm_n; ++i) { cx += mm_c[i].p[0]; cz += mm_c[i].p[2]; }
    if (mm_n) { cx /= (float)mm_n; cz /= (float)mm_n; }
    for (int i = 0; i < mm_n; ++i) {
        const float dx = mm_c[i].p[0] - cx, dz = mm_c[i].p[2] - cz;
        sxx += dx * dx; szz += dz * dz; sxz += dx * dz;
        if (mm_c[i].cls >= 'C') {   /* ours, markers, effects: our ids, upright, facing the centre */
            mm_c[i].id = MM_SYN | (uint32_t)i;
            if (mm_c[i].cls == 'C' || mm_c[i].cls == 'E') {
                const float half = atan2f(-dx, -dz) * 0.5f;
                mm_c[i].q[0] = mm_c[i].q[2] = 0; mm_c[i].q[1] = sinf(half); mm_c[i].q[3] = cosf(half);
            }
        }
    }
    const float a = 0.5f * atan2f(2 * sxz, sxx - szz);
    mm_center[0] = cx; mm_center[1] = cz;
    mm_axis[0] = cosf(a); mm_axis[1] = sinf(a);
    printf("CoopMP: spawns: %d candidates of class %c (online %d, soldier %d, pickup %d, ours %d, effects %d), "
           "net pak %08x, centre %.1f %.1f, axis %.2f %.2f\n", mm_n, mm_cls, cnt[0], cnt[1], cnt[2], cnt[3], cnt[4],
           mm_net_pak, cx, cz, mm_axis[0], mm_axis[1]);
}
static void mm_note(int kind, int slot, int team, int idx, uint32_t id, const float *p) {
    const LONG w = InterlockedIncrement(&mm_log_w) - 1;
    if (w - mm_log_r >= 32) return;
    mm_log[w % 32].kind = kind; mm_log[w % 32].slot = slot; mm_log[w % 32].team = team;
    mm_log[w % 32].idx = idx; mm_log[w % 32].id = id;
    memcpy(mm_log[w % 32].p, p, 12);
}
/* the pick: caller holds mm_lock, mm_n > 0 */
static uint32_t mm_pick(uint64_t ngm, uint64_t tracker, int kind) {
    const int self = (int)((int64_t)(tracker - (ngm + 0x10)) / 0x2c0);
    int32_t team = -1;
    rd(tracker + 0x134, &team, 4);
    const uint64_t local = mm_q(ngm);
    if (mm_probe_secs && tracker == local && mm_n && mm_untested(mm_probe_next < 0 ? 0 : mm_probe_next) >= 0) {   /* test: untested candidates in turn */
        const int i = mm_untested(mm_probe_next < 0 ? 0 : mm_probe_next);
        mm_probe_next = i + 1;
        mm_probe_last = i;
        mm_local_pick = i;
        mm_note(kind, self, team, i, mm_c[i].id, mm_c[i].p);
        return mm_c[i].id;
    }
    float op[16][3];
    int ot[16], no = 0;
    for (int k = 0; k < 10; ++k) {
        const uint64_t slot = ngm + 0x10 + (uint64_t)k * 0x2c0;
        uint8_t used = 0;
        if (slot == tracker || !rd(slot + 0x20, &used, 1) || !used) continue;
        if (!mm_slot_pos(slot, op[no])) continue;
        int32_t t = -1;
        rd(slot + 0x134, &t, 4);
        ot[no++] = t;
    }
    const ULONGLONG now = GetTickCount64();
    for (unsigned r = 0; r < 8 && no < 16; ++r)   /* this round's other picks (players not spawned yet) */
        if (mm_recent[r].t && now - mm_recent[r].t < 4000) { memcpy(op[no], mm_recent[r].p, 12); ot[no++] = mm_recent[r].team; }
    int best = -1;
    float best_score = -1e30f;
    for (int pass = 0; pass < 2 && best < 0; ++pass) {
        for (int i = 0; i < mm_n; ++i) {
            const float *p = mm_c[i].p;
            if (mm_state[i] == 2 && mm_untested(0) >= 0) continue;   /* rejected by the spawn check */
            if (!pass && kind == 0 && team >= 0 && team <= 1) {   /* first spawn: the team's half */
                const float s = (p[0] - mm_center[0]) * mm_axis[0] + (p[2] - mm_center[1]) * mm_axis[1];
                if ((s >= 0) != (team == 1)) continue;
            }
            float enemy = 1e9f, any = 1e9f;
            for (int o = 0; o < no; ++o) {
                const float dx = op[o][0] - p[0], dy = op[o][1] - p[1], dz = op[o][2] - p[2];
                const float d = sqrtf(dx * dx + dy * dy + dz * dz);
                if (d < any) any = d;
                if ((team < 0 || ot[o] != team) && d < enemy) enemy = d;
            }
            const uint32_t h = (uint32_t)i * 2654435761u ^ (mm_turn + 1) * 40503u ^ (uint32_t)self * 9973u;
            float score = (enemy < 1e8f ? (enemy < 60.0f ? enemy : 60.0f) : 30.0f) + (float)(h % 1000) * 0.012f;
            if (mm_c[i].cls == 'D' && mm_c[0].cls == 'D' && mm_c[mm_n - 1].cls != 'D') score += 100.0f;   /* a few of ours + markers: ours first */
            if (any < 2.5f) score -= 1000.0f;
            if (score > best_score) { best_score = score; best = i; }
        }
    }
    ++mm_turn;
    if (best < 0) return 0;
    mm_recent[mm_recent_next % 8].t = now;
    memcpy(mm_recent[mm_recent_next % 8].p, mm_c[best].p, 12);
    mm_recent[mm_recent_next++ % 8].team = team;
    if (tracker == local) mm_local_pick = best;
    mm_note(kind, self, team, best, mm_c[best].id, mm_c[best].p);
    return mm_c[best].id;
}
/* entry detours of 0x644220 (kind 0) / 0x642d20 (kind 1): the game's pick, else ours */
/* GOW3_COOP_MP_SPAWN_DUEL=1 (test): slots 0 and 1 always spawn at two points 7-14 m apart on one
 * level, each facing the other (the host can shoot the joiner without aiming round). Caller holds mm_lock. */
static int mm_duel_on, mm_duel[2] = {-1, -1};
static void mm_duel_pick(void) {
    mm_duel[0] = mm_duel[1] = -1;
    for (int i = 0; i < mm_n && mm_duel[0] < 0; ++i)
        for (int j = i + 1; j < mm_n; ++j) {
            const float dx = mm_c[i].p[0] - mm_c[j].p[0], dy = mm_c[i].p[1] - mm_c[j].p[1], dz = mm_c[i].p[2] - mm_c[j].p[2];
            const float d = sqrtf(dx * dx + dz * dz);
            if (d >= 7.0f && d <= 14.0f && fabsf(dy) < 0.3f) { mm_duel[0] = i; mm_duel[1] = j; break; }
        }
}
static uint32_t ABI mm_choose(uint64_t ngm, uint64_t tracker, uint32_t game_id, uint32_t kind) {
    const int slot = (int)((int64_t)(tracker - (ngm + 0x10)) / 0x2c0);
    if (mm_duel_on && mm_competitive && (slot == 0 || slot == 1)) {
        AcquireSRWLockExclusive(&mm_lock);
        mm_build();
        if (mm_duel[0] < 0) mm_duel_pick();
        const int i = mm_duel[slot];
        const uint32_t id = i >= 0 ? mm_c[i].id : game_id;
        if (i >= 0) mm_note((int)kind, slot, -2, i, id, mm_c[i].p);
        ReleaseSRWLockExclusive(&mm_lock);
        return id ? id : game_id;
    }
    if (game_id && !(mm_probe_secs && tracker == mm_q(ngm))) {
        InterlockedIncrement(&mm_game_picks);
        const float none[3] = {0, 0, 0};
        mm_note((int)kind, slot, -3, -1, game_id, none);   /* the game's own pick (logged) */
        return game_id;
    }
    if (!mm_competitive) return game_id;
    AcquireSRWLockExclusive(&mm_lock);
    mm_build();
    const uint32_t id = mm_n ? mm_pick(ngm, tracker, (int)kind) : 0;
    ReleaseSRWLockExclusive(&mm_lock);
    InterlockedIncrement(id ? &mm_our_picks : &mm_failed);
    return id ? id : game_id;
}
/* GetSpawnLocator 0x662fc9: 1 = out (pos, quat) filled */
static int ABI mm_locate(uint32_t id, float *out) {
    if ((id & 0xffff0000u) == MM_SYN) {
        AcquireSRWLockExclusive(&mm_lock);
        mm_build();
        const uint32_t i = id & 0xffff;
        int ok = 0;
        if ((int)i < mm_n && mm_c[i].id == id) { memcpy(out, mm_c[i].p, 16); memcpy(out + 4, mm_c[i].q, 16); ok = 1; }
        else if (mm_n) { memcpy(out, mm_c[0].p, 16); memcpy(out + 4, mm_c[0].q, 16); ok = 1; }   /* not ours here: never the assert path */
        ReleaseSRWLockExclusive(&mm_lock);
        if (ok) InterlockedIncrement(&mm_located_ours);
        return ok;
    }
    if (mm_duel_on && mm_duel[0] >= 0) {   /* test: the duel points face each other */
        int me = -1;
        AcquireSRWLockExclusive(&mm_lock);
        for (int k = 0; k < 2; ++k) if (mm_duel[k] >= 0 && mm_duel[k] < mm_n && mm_c[mm_duel[k]].id == id) me = k;
        if (me >= 0) {
            const float *a = mm_c[mm_duel[me]].p, *b = mm_c[mm_duel[!me]].p;
            const float half = atan2f(b[0] - a[0], b[2] - a[2]) * 0.5f;
            const float q[4] = {0, sinf(half), 0, cosf(half)};
            memcpy(out, a, 16);
            memcpy(out + 4, q, 16);
        }
        ReleaseSRWLockExclusive(&mm_lock);
        if (me >= 0) return 1;
    }
    const uint64_t I = (uint64_t)(uintptr_t)image;
    uint64_t sp = ((MmLookup)(image + 0x56c7b0))(I + 0x1130390, id);
    if (!sp) {   /* records 0x56c7b0 does not search (e.g. a net pak) */
        const uint64_t base = mm_q(I + 0x1130390);
        for (int r = 0; r < 180 && base && !sp; ++r) {
            const uint64_t t = mm_q(base + (uint64_t)r * 0xa60 + 0x9a0), arr = t ? mm_q(t + 8) : 0;
            const uint32_t n = t ? mm_d(t + 0x10) : 0;
            for (uint32_t j = 0; arr && j < n && j < 4096 && !sp; ++j) {
                const uint64_t s = mm_q(arr + 8 * (uint64_t)j);
                if (s && mm_d(s + 0x28) == id) sp = s;
            }
        }
    }
    if (!sp) return 0;
    ((MmLocator)(image + 0x4bdfe0))(out, sp);
    InterlockedIncrement(&mm_located);
    return 1;
}
static int mm_install(void) {
    static int done;
    if (done) return done > 0;
    static const unsigned char loc_site[7] = {0x48, 0x8d, 0x3d, 0x90, 0x87, 0xad, 0x00};   /* 0x662fc9 lea rdi, [0x113b760] */
    static const unsigned char prologue[8] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56};
    if (memcmp(image + 0x662fc9, loc_site, 7) || memcmp(image + 0x644220, prologue, 8) || memcmp(image + 0x642d20, prologue, 8)) {
        done = -1;
        return 0;
    }
    const uint64_t I = (uint64_t)(uintptr_t)image;
    int ok = 1;
    {   /* GetSpawnLocator: r12d = id, [rbp-0x50] = the locator, rsp = 0 mod 16 here */
        unsigned char *s = stub_page();
        unsigned n = 0;
        if (!s) return done = -1, 0;
#define B(...) do { const unsigned char b_[] = {__VA_ARGS__}; memcpy(s + n, b_, sizeof(b_)); n += sizeof(b_); } while (0)
#define Q(v) do { const uint64_t q_ = (v); memcpy(s + n, &q_, 8); n += 8; } while (0)
        B(0x44, 0x89, 0xe7);                                   /* mov edi, r12d */
        B(0x48, 0x8d, 0x75, 0xb0);                             /* lea rsi, [rbp-0x50] */
        B(0x48, 0xb8); Q((uint64_t)(uintptr_t)mm_locate);      /* mov rax, mm_locate */
        B(0xff, 0xd0);                                         /* call rax */
        B(0x85, 0xc0);                                         /* test eax, eax */
        B(0x74, 0x0c);                                         /* jz game (+12) */
        B(0x48, 0xb8); Q(I + 0x663064);                        /* mov rax, 0x663064 (after 0x4bdfe0) */
        B(0xff, 0xe0);                                         /* jmp rax */
        B(0x48, 0xbf); Q(I + U2_NGM);                          /* game: mov rdi, ngm */
        B(0x48, 0xb8); Q(I + 0x644130);                        /* mov rax, 0x644130 (net pak record) */
        B(0xff, 0xd0);                                         /* call rax */
        B(0x48, 0x85, 0xc0);                                   /* test rax, rax */
        B(0x74, 0x0c);                                         /* jz miss (+12) */
        B(0x48, 0xb9); Q(I + 0x662fd5);                        /* mov rcx, 0x662fd5 (its table) */
        B(0xff, 0xe1);                                         /* jmp rcx */
        B(0x48, 0xb9); Q(I + 0x66307a);                        /* miss: mov rcx, 0x66307a (not found) */
        B(0xff, 0xe1);                                         /* jmp rcx */
        ok &= jump_to(0x662fc9, s, 2);
    }
    for (int k = 0; k < 2 && ok; ++k) {   /* the two choices */
        const uint64_t site = k ? 0x642d20 : 0x644220;
        unsigned char *s = stub_page();
        unsigned n = 0;
        if (!s) { ok = 0; break; }
        B(0x57, 0x56);                                         /* push rdi; push rsi */
        B(0x48, 0x83, 0xec, 0x08);                             /* sub rsp, 8 (rsp = 0 mod 16) */
        B(0x48, 0xb8); const unsigned tramp_at = n; Q(0);      /* mov rax, trampoline */
        B(0xff, 0xd0);                                         /* call rax (the original) */
        B(0x48, 0x8b, 0x7c, 0x24, 0x10);                       /* mov rdi, [rsp+16] (ngm) */
        B(0x48, 0x8b, 0x74, 0x24, 0x08);                       /* mov rsi, [rsp+8] (tracker) */
        B(0x89, 0xc2);                                         /* mov edx, eax */
        B(0xb9, (unsigned char)k, 0, 0, 0);                    /* mov ecx, kind */
        B(0x48, 0xb8); Q((uint64_t)(uintptr_t)mm_choose);      /* mov rax, mm_choose */
        B(0xff, 0xd0);                                         /* call rax */
        B(0x48, 0x83, 0xc4, 0x18);                             /* add rsp, 24 */
        B(0xc3);                                               /* ret */
        const uint64_t tramp = (uint64_t)(uintptr_t)(s + n);
        memcpy(s + tramp_at, &tramp, 8);
        memcpy(s + n, prologue, 8); n += 8;                    /* the replayed prologue */
        B(0xff, 0x25, 0x00, 0x00, 0x00, 0x00); Q(I + site + 8); /* jmp [rip] */
        ok &= jump_to(site, s, 3);
    }
#undef B
#undef Q
    done = ok ? 1 : -1;
    return ok;
}
/* classic maps: the *net-maps* entry of this net game gets a shipped net pak and level list */
static void mm_classic_levels(long game) {
    const uint64_t me = u2_net_map_entry(game);
    if (!me) return;
    uint32_t pak = 0;
    rd(me + 0x20, &pak, 4);
    mm_net_pak = pak;
    for (size_t m = 0; m < sizeof(mm_maps) / sizeof(*mm_maps); ++m) {
        if (sid(mm_maps[m].pak) != pak) continue;
        uint64_t head = 0, list = 0;
        uint32_t count = 0;
        if (!rd(me + 0x28, &head, 8) || !head || !rd(head, &count, 4) || !rd(head + 8, &list, 8) || !list || count > 16) return;
        struct Lv { uint64_t name; uint32_t id, pad; };
        struct Lv *out = calloc(18, sizeof(struct Lv));
        struct { uint32_t count, pad; uint64_t list; } *h = calloc(1, 16);
        if (!out || !h) return;
        const uint32_t extra = mm_maps[m].extra ? sid(mm_maps[m].extra) : 0;
        int n = 0, has_extra = 0;
        char text[256] = {0};
        size_t tl = 0;
        for (uint32_t i = 0; i < count; ++i) {
            struct Lv e;
            if (!rd(list + 16 * (uint64_t)i, &e, sizeof(e)) || e.id == pak) continue;
            char nm[48] = {0};
            if (e.name) rd(e.name, nm, sizeof(nm) - 1);
            /* village-multi-tank: its tank's track script asserted at the first frame of Playing
             * (ss-track-group.cpp GetDeltaSeconds, then a fault in the level's spawn pass, run mm01b) */
            const size_t nl = strlen(nm);
            const char *tank = getenv("GOW3_COOP_MP_TANK");
            if (nl > 5 && !strcmp(nm + nl - 5, "-tank") && !(tank && tank[0] == '1')) continue;
            if (e.id == extra) has_extra = 1;
            out[n++] = e;
            tl += (size_t)snprintf(text + tl, sizeof(text) - tl, "%s%s", tl ? "," : "", nm);
            if (tl >= sizeof(text)) tl = sizeof(text) - 1;
        }
        if (extra && !has_extra) {
            out[n].name = (uint64_t)(uintptr_t)mm_maps[m].extra;
            out[n++].id = extra;
            tl += (size_t)snprintf(text + tl, sizeof(text) - tl, ",%s", mm_maps[m].extra);
        }
        h->count = (uint32_t)n;
        h->list = (uint64_t)(uintptr_t)out;
        const uint64_t name = (uint64_t)(uintptr_t)mm_maps[m].net_pak, hp = (uint64_t)(uintptr_t)h;
        const uint32_t id = sid(mm_maps[m].net_pak);
        wr(me + 0x18, &name, 8);
        wr(me + 0x20, &id, 4);
        wr(me + 0x28, &hp, 8);
        mm_net_pak = id;
        printf("CoopMP: classic map: net game %ld: net pak %s -> %s, levels %s\n", game, mm_maps[m].pak, mm_maps[m].net_pak, text);
        return;
    }
}
/* main thread (coop_frame): log lines, probe */
static void mm_objects_frame(void);
static void mm_frame(void) {
    if (!mm_competitive) return;
    mm_objects_frame();   /* classic maps' objectives, once the net pak level is in */
    int printed_ = 0, *printed = &printed_;
    for (; mm_log_r < mm_log_w; ++mm_log_r) {
        const int i = (int)(mm_log_r % 32);
        printf("CoopMP: spawn %s: slot %d team %d -> %s #%d (%08x %s) at %.2f %.2f %.2f\n", mm_log[i].kind ? "respawn" : "first",
               mm_log[i].slot, mm_log[i].team,
               mm_log[i].team == -3 ? "the game's" : mm_log[i].team == -2 ? "duel" : mm_probe_secs ? "probe" : "ours",
               mm_log[i].idx, mm_log[i].id,
               mm_log[i].idx >= 0 && mm_log[i].idx < mm_n ? mm_c[mm_log[i].idx].name : "?", mm_log[i].p[0], mm_log[i].p[1], mm_log[i].p[2]);
        *printed = 1;
    }
    const ULONGLONG now = GetTickCount64();
    static ULONGLONG last;
    if (now - last >= 15000 && (mm_game_picks || mm_our_picks || mm_failed || mm_located)) {
        last = now;
        printf("CoopMP: spawns: game picks %ld, ours %ld, none %ld; locator: level spawners %ld, our points %ld\n",
               (long)mm_game_picks, (long)mm_our_picks, (long)mm_failed, (long)mm_located, (long)mm_located_ours);
        *printed = 1;
    }
    {   /* spawn check (all competitive games, our points only): the local player must stand where he spawned */
        typedef void (ABI *MmRespawn)(uint64_t tracker, uint32_t id, uint32_t flag);
        const uint64_t I = (uint64_t)(uintptr_t)image, ngm = I + U2_NGM, local = mm_q(ngm);
        static uint32_t last_pid;
        static ULONGLONG t0, tsample;
        static int cand = -1, phase, all_bad_said;
        static float y_spawn, y_prev;
        const int32_t mode = (int32_t)mm_d(I + 0x14ce97c), ended = (int32_t)mm_d(ngm + 0x1e84);
        const uint32_t pid = local ? mm_d(local + 0x18) : 0;
        float p[3];
        const int alive = mode == 26 && local && mm_slot_pos(local, p);
        int verdict = 0;   /* 1 stands, 2 rejected */
        const char *why = "";
        if (mode != 26 || !local) last_pid = 0;
        else if (pid != last_pid) {
            if (last_pid && cand >= 0 && phase == 2) { verdict = 2; why = "died"; }   /* the life ended before the check */
            else { last_pid = pid; t0 = now; cand = -1; phase = alive ? 1 : 0; }
        } else if (alive && phase == 1 && now - t0 >= 150) {   /* whose point: the nearest (xz) within 2 m */
            AcquireSRWLockExclusive(&mm_lock);
            float best = 4.0f;
            cand = -1;
            const int lp = mm_local_pick;   /* our last pick for this player, if he is there */
            if (lp >= 0 && lp < mm_n && mm_c[lp].cls != 'A') {
                const float dx = mm_c[lp].p[0] - p[0], dz = mm_c[lp].p[2] - p[2];
                if (dx * dx + dz * dz < best) cand = lp;
            }
            for (int i = 0; i < mm_n && cand < 0; ++i) {   /* else the nearest point (y counts: points stacked above each other) */
                if (mm_c[i].cls == 'A') continue;   /* the map's own spawners: trusted */
                const float dx = mm_c[i].p[0] - p[0], dz = mm_c[i].p[2] - p[2], dy = mm_c[i].p[1] - p[1];
                if (dx * dx + dz * dz < 4.0f && dx * dx + dz * dz + dy * dy < best) { best = dx * dx + dz * dz + dy * dy; cand = i; }
            }
            if (cand < 0) for (int i = 0; i < mm_n; ++i) {
                if (mm_c[i].cls == 'A') continue;
                const float dx = mm_c[i].p[0] - p[0], dz = mm_c[i].p[2] - p[2];
                if (dx * dx + dz * dz < 4.0f) { cand = i; break; }
            }
            if (cand >= 0) y_spawn = mm_c[cand].p[1];
            ReleaseSRWLockExclusive(&mm_lock);
            phase = cand >= 0 ? 2 : 4;
            y_prev = p[1];
            tsample = now;
        } else if (alive && phase == 2) {
            if (p[1] < y_spawn - 2.0f) { verdict = 2; why = "fell"; }
            else if (now - tsample >= 500) {
                if (now - t0 >= 2000) {
                    if (fabsf(p[1] - y_prev) > 0.2f) { verdict = 2; why = "kept moving"; }
                    else verdict = 1;
                }
                y_prev = p[1];
                tsample = now;
            }
        } else if (alive && phase == 3 && mm_probe_secs && now - t0 >= 2500 && !ended) {   /* probe: next untested point */
            AcquireSRWLockExclusive(&mm_lock);
            const int next = mm_untested(cand + 1);
            ReleaseSRWLockExclusive(&mm_lock);
            phase = 5;
            if (next >= 0) {
                mm_probe_next = next + 1;
                mm_probe_last = next;
                mm_local_pick = next;
                ((MmRespawn)(image + 0x663c60))(local, mm_c[next].id, 0);
                printf("CoopMP: spawn probe: respawn at #%d %s (last handed out #%d)\n", next, mm_c[next].name, mm_probe_last);
            } else {
                int good = 0;
                for (int i = 0; i < mm_n; ++i) good += mm_state[i] == 1;
                printf("CoopMP: spawn probe: all %d points tested, %d stand\n", mm_n, good);
            }
            *printed = 1;
        }
        if (verdict && cand >= 0 && cand < mm_n) {
            mm_state[cand] = (unsigned char)verdict;
            if (verdict == 1)
                printf("CoopMP: spawn check #%d %s (%c): stands at %.2f %.2f %.2f (spawned at %.2f %.2f %.2f)\n", cand, mm_c[cand].name,
                       mm_c[cand].cls, p[0], p[1], p[2], mm_c[cand].p[0], mm_c[cand].p[1], mm_c[cand].p[2]);
            else
                printf("CoopMP: spawn check #%d %s (%c): REJECTED (%s; spawned at %.2f %.2f %.2f, now y %.2f)\n", cand, mm_c[cand].name,
                       mm_c[cand].cls, why, mm_c[cand].p[0], mm_c[cand].p[1], mm_c[cand].p[2], alive ? p[1] : -999.0);
            *printed = 1;
            phase = 3;
            static ULONGLONG window;
            static int fixes;
            if (now - window > 60000) { window = now; fixes = 0; }
            if (verdict == 2 && pid == last_pid && alive && !ended && fixes++ < 12) {   /* still alive (falling / moving): a new point now */
                AcquireSRWLockExclusive(&mm_lock);
                uint32_t id = mm_untested(0) >= 0 ? mm_pick(ngm, local, 1) : 0;
                if (id && mm_local_pick >= 0 && mm_local_pick < mm_n && mm_state[mm_local_pick] == 2) id = 0;   /* nothing new */
                ReleaseSRWLockExclusive(&mm_lock);
                if (id) {
                    ((MmRespawn)(image + 0x663c60))(local, id, 0);
                    printf("CoopMP: spawn check: respawn at %08x\n", id);
                } else if (!all_bad_said++) puts("CoopMP: spawn check: no untested point left on this map");
            }
            if (pid != last_pid) { last_pid = pid; t0 = now; cand = -1; phase = alive ? 1 : 0; }
        }
    }
    if (printed) fflush(stdout);
}
/* Objectives on the classic maps (GOW3_COOP_MP_OBJECTS=0 = off): Plunder (1), Turf War (5), King of the
 * Hill (6), Chain Reaction (7) take their objects from spawner records in the net pak level: round
 * start 0x647a50 spawns the start-flag treasure and every NetTerritory, 0x64bb80 lists the hills,
 * 0x63e1b0 the treasure's later spots (is-net-flag), 0x64b960 the goals (net-capture-team); the
 * spawn events look the ids up in every level (0x56c7b0). The classic maps' records were in the
 * missing net paks, but their main levels still load the actors (territory-marker,
 * capture-idol-gold, capture-chest: pak22_ps4.txt). We add records of Fort's layout and tags (no
 * net-game tag: the filter 0x4bea60 keeps them) to the net pak level's spawner table (+8/+0x10 list,
 * +0x18/+0x20 {id, spawner} pairs sorted by id) once it is loaded, at points spread over the map
 * (the spawn candidates): every machine builds the same records, so ids agree. */
typedef struct { uint16_t type, size; uint32_t pad; uint64_t data; } MmVal;
typedef struct { uint32_t sid, pad; uint64_t val; } MmTag;
static int mm_obj_mode = -1, mm_obj_done;
static unsigned char *mm_pool;
static size_t mm_pool_used;
static uint64_t mm_take(size_t n) {
    if (!mm_pool) mm_pool = VirtualAlloc(NULL, 1 << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    n = (n + 15) & ~(size_t)15;
    if (!mm_pool || mm_pool_used + n > (1 << 20)) return 0;
    const uint64_t p = (uint64_t)(uintptr_t)(mm_pool + mm_pool_used);
    mm_pool_used += n;
    return p;
}
static uint64_t mm_val(uint16_t type, const void *data, size_t n) {
    MmVal *v = (MmVal *)(uintptr_t)mm_take(sizeof(MmVal));
    const uint64_t d = mm_take(n < 16 ? 16 : n);
    if (!v || !d) return 0;
    memcpy((void *)(uintptr_t)d, data, n);
    v->type = type;
    v->size = 4;
    v->data = d;
    return (uint64_t)(uintptr_t)v;
}
static uint64_t mm_vi(int32_t x) { return mm_val(1, &x, 4); }
static uint64_t mm_vf(float x) { return mm_val(4, &x, 4); }
static uint64_t mm_vsid(uint32_t x) { return mm_val(0xd, &x, 4); }
static uint64_t mm_vstr(const char *s) {
    const size_t n = strlen(s);
    const uint64_t copy = mm_take(n + 1);
    if (!copy) return 0;
    memcpy((void *)(uintptr_t)copy, s, n + 1);
    const uint64_t v = mm_val(5, &copy, 8);
    if (v) ((MmVal *)(uintptr_t)v)->size = (uint16_t)n;
    return v;
}
static uint64_t mm_obj_level;   /* +0x58 of the net pak's own spawners (world = local + [level+0x20]) */
static float mm_obj_w;
static uint64_t mm_record(const char *name, uint32_t type, const char *art, uint32_t flags, const float *pos, MmTag *tags, int n) {
    const uint64_t r = mm_take(0xa0), nm = mm_take(strlen(name) + 1), a = art ? mm_take(strlen(art) + 1) : 0;
    const uint64_t entries = mm_take(16 * (size_t)(n ? n : 1)), table = mm_take(16);
    if (!r || !nm || (art && !a) || !entries || !table) return 0;
    for (int i = 1; i < n; ++i)   /* tags sorted by sid (binary searches) */
        for (int j = i; j > 0 && tags[j - 1].sid > tags[j].sid; --j) { MmTag t = tags[j]; tags[j] = tags[j - 1]; tags[j - 1] = t; }
    memcpy((void *)(uintptr_t)entries, tags, 16 * (size_t)n);
    memcpy((void *)(uintptr_t)nm, name, strlen(name) + 1);
    if (a) memcpy((void *)(uintptr_t)a, art, strlen(art) + 1);
    unsigned char *p = (unsigned char *)(uintptr_t)r;
    float v[8] = {pos[0], pos[1], pos[2], mm_obj_w, 0, 0, 0, 1};
    memcpy(p, v, 32);
    memcpy(p + 0x20, &nm, 8);
    const uint32_t id = sid(name), cnt = (uint32_t)n;
    memcpy(p + 0x28, &id, 4);
    memcpy(p + 0x40, &type, 4);
    memcpy(p + 0x48, &a, 8);
    memcpy(p + 0x50, &flags, 4);
    memcpy(p + 0x58, &mm_obj_level, 8);
    memcpy(p + 0x68, &table, 8);
    memcpy((void *)(uintptr_t)table, &entries, 8);
    memcpy((void *)(uintptr_t)(table + 8), &cnt, 4);
    return r;
}
/* k points spread over the candidates (farthest-point sampling from the one nearest the centre) */
static int mm_spread(int k, int *out) {
    int n = 0;
    if (!mm_n) return 0;
    int first = 0;
    float bd = 1e30f;
    for (int i = 0; i < mm_n; ++i) {
        const float dx = mm_c[i].p[0] - mm_center[0], dz = mm_c[i].p[2] - mm_center[1];
        if (dx * dx + dz * dz < bd) { bd = dx * dx + dz * dz; first = i; }
    }
    out[n++] = first;
    while (n < k && n < mm_n) {
        int best = -1;
        float bdist = -1;
        for (int i = 0; i < mm_n; ++i) {
            float m = 1e30f;
            for (int j = 0; j < n; ++j) {
                const float dx = mm_c[i].p[0] - mm_c[out[j]].p[0], dy = mm_c[i].p[1] - mm_c[out[j]].p[1], dz = mm_c[i].p[2] - mm_c[out[j]].p[2];
                const float d = dx * dx + dy * dy + dz * dz;
                if (d < m) m = d;
            }
            if (m > bdist) { bdist = m; best = i; }
        }
        if (best < 0 || bdist < 1.0f) break;
        out[n++] = best;
    }
    return n;
}
static float mm_side(int i) { return (mm_c[i].p[0] - mm_center[0]) * mm_axis[0] + (mm_c[i].p[2] - mm_center[1]) * mm_axis[1]; }
static void mm_objects_frame(void) {
    if (mm_obj_mode < 0 || mm_obj_done) return;
    const uint64_t I = (uint64_t)(uintptr_t)image, base = mm_q(I + 0x1130390);
    uint64_t t = 0;
    for (int r = 0; r < 180 && base && !t; ++r)
        if (mm_d(base + (uint64_t)r * 0xa60 + 0x74) == mm_net_pak) t = mm_q(base + (uint64_t)r * 0xa60 + 0x9a0);
    const uint32_t count = t ? mm_d(t + 0x10) : 0, pairs = t ? mm_d(t + 0x20) : 0;
    const uint64_t arr = t ? mm_q(t + 8) : 0, parr = t ? mm_q(t + 0x18) : 0;
    if (!t || !count || !arr || !parr) return;
    const uint64_t sp0 = mm_q(arr);
    mm_obj_level = mm_q(sp0 + 0x58);
    rd(sp0 + 0xc, &mm_obj_w, 4);
    AcquireSRWLockExclusive(&mm_lock);
    mm_build();
    uint64_t recs[16];
    int nrec = 0, idx[8];
    char name[48];
    const int mode = mm_obj_mode;
    if (mode == 5 || mode == 6 || mode == 7) {   /* territories / hills */
        int k = mm_spread(mode == 6 ? 4 : 5, idx);
        if (mode == 7)   /* a chain from team 0's side to team 1's */
            for (int i = 1; i < k; ++i)
                for (int j = i; j > 0 && mm_side(idx[j - 1]) > mm_side(idx[j]); --j) { const int x = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = x; }
        for (int i = 0; i < k; ++i) {
            MmTag tg[8];
            int n = 0;
            tg[n++] = (MmTag){0x2a0f755bu, 0, mm_vi(-1)};
            tg[n++] = (MmTag){0x577cdba2u, 0, mm_vf(2.0f)};
            tg[n++] = (MmTag){0x7a26d6dcu, 0, mm_vf(5.0f)};   /* radius */
            tg[n++] = (MmTag){0x83af8af1u, 0, mm_vf(1.0f)};
            tg[n++] = (MmTag){0xc6ffe0e5u, 0, mm_vi(-1)};     /* start-team */
            if (mode == 7) {
                char link[48];
                if (i + 1 < k) { snprintf(link, sizeof(link), "mm-territory-%d", i + 1); tg[n++] = (MmTag){0x91831700u, 0, mm_vstr(link)}; }
                if (i > 0) { snprintf(link, sizeof(link), "mm-territory-%d", i - 1); tg[n++] = (MmTag){0xca22cbcau, 0, mm_vstr(link)}; }
            }
            snprintf(name, sizeof(name), "mm-territory-%d", i);
            recs[nrec++] = mm_record(name, 0x58609830u, "territory-marker", 2, mm_c[idx[i]].p, tg, n);
        }
    } else if (mode == 1) {   /* Plunder: treasure spots near the centre, a goal (+ chest) on each team's side */
        const int k = mm_spread(3, idx);
        for (int i = 0; i < k; ++i) {
            MmTag tg[16];
            int n = 0;
            tg[n++] = (MmTag){0x1c964773u, 0, mm_vi(1)};      /* is-net-flag */
            tg[n++] = (MmTag){0x375fc812u, 0, mm_vf(1.0f)};
            tg[n++] = (MmTag){0x57b55dfdu, 0, mm_vsid(0x35bf7d31u)};
            tg[n++] = (MmTag){0x69817598u, 0, mm_vstr("*carry-net-flag*")};
            tg[n++] = (MmTag){0x705cbd1cu, 0, mm_vf(1.0f)};
            if (!i) tg[n++] = (MmTag){0xad66fbf6u, 0, mm_vi(1)};   /* start-flag */
            tg[n++] = (MmTag){0xaedbe2cbu, 0, mm_vi(0)};
            tg[n++] = (MmTag){0xafd01baau, 0, mm_vf(1.0f)};
            tg[n++] = (MmTag){0xc01060e5u, 0, mm_vi(0)};
            tg[n++] = (MmTag){0xd7a1aad8u, 0, mm_vi(1)};
            tg[n++] = (MmTag){0xe4f0e9a3u, 0, mm_vf(30.0f)};
            tg[n++] = (MmTag){0xea32e096u, 0, mm_vf(1.0f)};
            tg[n++] = (MmTag){0xeb85972au, 0, mm_vi(1)};
            tg[n++] = (MmTag){0xfa11b8efu, 0, mm_vf(-1.0f)};
            snprintf(name, sizeof(name), "mm-net-flag-%d", i);
            recs[nrec++] = mm_record(name, 0x310f465eu, "capture-idol-gold", 2, mm_c[idx[i]].p, tg, n);
        }
        int lo = 0, hi = 0;
        for (int i = 1; i < mm_n; ++i) { if (mm_side(i) < mm_side(lo)) lo = i; if (mm_side(i) > mm_side(hi)) hi = i; }
        for (int team = 0; team < 2; ++team) {
            const float *p = mm_c[team ? hi : lo].p;
            MmTag tg[10];
            int n = 0;
            tg[n++] = (MmTag){0x26cbf874u, 0, mm_vi(team)};   /* net-capture-team */
            snprintf(name, sizeof(name), "mm-capture-region-%d", team);
            recs[nrec++] = mm_record(name, 0, NULL, 2, p, tg, n);
            n = 0;
            tg[n++] = (MmTag){0x45c51f9fu, 0, mm_vstr("ss-chest-open")};
            tg[n++] = (MmTag){0x56da20e1u, 0, mm_vi(0)};
            tg[n++] = (MmTag){0x66c6d3feu, 0, mm_vi(0)};
            tg[n++] = (MmTag){0x871aab05u, 0, mm_vi(0)};
            tg[n++] = (MmTag){0xa88f1ca6u, 0, mm_vi(0)};
            tg[n++] = (MmTag){0xc867d993u, 0, mm_vi(0)};
            tg[n++] = (MmTag){0xc87fbb93u, 0, mm_vi(0)};
            tg[n++] = (MmTag){0xe07cb817u, 0, mm_vsid(0x39bad0f3u)};
            snprintf(name, sizeof(name), "mm-capture-chest-%d", team);
            recs[nrec++] = mm_record(name, 0x516a073bu, "capture-chest", 0, p, tg, n);
        }
    }
    ReleaseSRWLockExclusive(&mm_lock);
    int ok = nrec > 0;
    for (int i = 0; i < nrec; ++i) ok &= recs[i] != 0;
    const uint64_t na = ok ? mm_take(8 * ((size_t)count + (size_t)nrec)) : 0, np = ok ? mm_take(16 * ((size_t)pairs + (size_t)nrec)) : 0;
    if (!ok || !na || !np) { mm_obj_done = -1; printf("CoopMP: objectives: NOT added (mode %d, %d records)\n", mode, nrec); fflush(stdout); return; }
    rd(arr, (void *)(uintptr_t)na, 8 * (size_t)count);
    memcpy((void *)(uintptr_t)(na + 8 * (uint64_t)count), recs, 8 * (size_t)nrec);
    rd(parr, (void *)(uintptr_t)np, 16 * (size_t)pairs);
    struct P { uint32_t id, pad; uint64_t sp; } *pp = (struct P *)(uintptr_t)np;
    int m = (int)pairs;
    for (int i = 0; i < nrec; ++i) {
        struct P e = {mm_d(recs[i] + 0x28), 0, recs[i]};
        int j = m++;
        for (; j > 0 && pp[j - 1].id > e.id; --j) pp[j] = pp[j - 1];
        pp[j] = e;
    }
    const uint32_t c2 = count + (uint32_t)nrec, p2 = (uint32_t)m;
    wr(t + 8, &na, 8);
    wr(t + 0x18, &np, 8);
    wr(t + 0x20, &p2, 4);
    wr(t + 0x10, &c2, 4);
    mm_obj_done = 1;
    printf("CoopMP: objectives: %d records added to the net pak level (mode %d):", nrec, mode);
    for (int i = 0; i < nrec; ++i) {
        float p[3];
        char nm[32] = {0};
        rd(recs[i], p, 12);
        rd(mm_q(recs[i] + 0x20), nm, sizeof(nm) - 1);
        printf(" %s (%.1f %.1f %.1f)", nm, p[0], p[1], p[2]);
    }
    printf("\n");
    fflush(stdout);
}

/* Objective modes on the DLC maps (GOW3_COOP_MP_OBJ_ALIAS=0 = off): each map's objects (treasure,
 * chests, hills, territories) carry a net-game tag naming ONE net game per mode,
 * "NET_GAME_<FORT|RUINS|DIG>_<CTF|KOTH|TERRITORY|TUG>_1" (= the Standard variant's *net-games* id; TUG =
 * Chain Reaction). The net-game filter 0x4bea60 keeps a spawner only when a tag equals the running
 * net game's id (+0), so the Crushing variants (Fort 131/133/165, Ruins 167/169/171) and Fort's Chain
 * Reaction (134/135) dropped every object. The filter's id load (0x4beab8) maps our id to the data's. */
static int mm_alias(long game) {
    const char *v = getenv("GOW3_COOP_MP_OBJ_ALIAS");
    if (v && v[0] == '0') return 0;
    const int32_t mode = net_game_mode(game);
    const char *what = mode == 1 ? "CTF" : mode == 6 ? "KOTH" : mode == 5 ? "TERRITORY" : mode == 7 ? "TUG" : NULL;
    const char *map = mm_net_pak == sid("the-fort-multi") ? "FORT" : mm_net_pak == sid("ruins-multi") ? "RUINS"
                    : mm_net_pak == sid("dig-multi") ? "DIG" : NULL;
    const uint64_t entry = u2_net_game_entry(game);
    if (!what || !map || !entry) return 0;
    char text[64];
    snprintf(text, sizeof(text), "NET_GAME_%s_%s_1", map, what);
    const uint32_t data = sid(text), id = mm_d(entry);
    if (data == id) return 0;
    static const unsigned char site[9] = {0x44, 0x8b, 0x32, 0x4c, 0x8b, 0x06, 0x8b, 0x56, 0x08}; /* 0x4beab8 */
    if (memcmp(image + 0x4beab8, site, 9)) return -1;
    unsigned char *s = stub_page();
    if (!s) return -1;
    unsigned n = 0;
#define B(...) do { const unsigned char b_[] = {__VA_ARGS__}; memcpy(s + n, b_, sizeof(b_)); n += sizeof(b_); } while (0)
    B(0x44, 0x8b, 0x32);                                       /* mov r14d, [rdx] (the net game's id) */
    B(0x41, 0x81, 0xfe); memcpy(s + n, &id, 4); n += 4;        /* cmp r14d, id */
    B(0x75, 0x06);                                             /* jne +6 */
    B(0x41, 0xbe); memcpy(s + n, &data, 4); n += 4;            /* mov r14d, data id */
    B(0x4c, 0x8b, 0x06, 0x8b, 0x56, 0x08);                     /* mov r8, [rsi]; mov edx, [rsi+8] */
    B(0xff, 0x25, 0x00, 0x00, 0x00, 0x00);                     /* jmp [rip] -> 0x4beac1 */
#undef B
    const uint64_t back = (uint64_t)(uintptr_t)image + 0x4beac1;
    memcpy(s + n, &back, 8);
    if (!jump_to(0x4beab8, s, 4)) return -1;
    printf("CoopMP: objectives: net game %ld (%08x) uses the map's %s objects (%08x, filter 0x4beab8)\n", game, id, text, data);
    return 1;
}
/* the map of a net game: the index of its classic map (mm_maps) + 1, else its net pak sid */
static uint32_t mm_map_key(long game) {
    const uint64_t me = u2_net_map_entry(game);
    const uint32_t pak = me ? mm_d(me + 0x20) : 0;
    for (size_t m = 0; m < sizeof(mm_maps) / sizeof(*mm_maps); ++m)
        if (pak == sid(mm_maps[m].pak) || pak == sid(mm_maps[m].net_pak)) return (uint32_t)m + 1;
    return pak;
}
static void mm_file_load(long game) {
    char path[1024];
    const char *v = getenv("GOW3_COOP_MP_SPAWN_FILE");
    FILE *f = NULL;
    if (v && *v) { snprintf(path, sizeof(path), "%s", v); f = fopen(path, "r"); }
    else {
        const char *user = runtime_file_user_dir();
        snprintf(path, sizeof(path), "%s/spawn-points.txt", user ? user : "user");
        f = fopen(path, "r");
        if (!f && user) {   /* a second local instance (uncharted-user-i2): the main folder's file */
            const char *dash = strrchr(user, '-');
            if (dash && dash[1] == 'i' && dash[2] >= '0' && dash[2] <= '9') {
                snprintf(path, sizeof(path), "%.*s/spawn-points.txt", (int)(dash - user), user);
                f = fopen(path, "r");
            }
        }
    }
    if (!f) return;
    const uint32_t key = mm_map_key(game);
    char line[256];
    int lines = 0;
    while (fgets(line, sizeof(line), f) && mm_file_n < MM_FILE_MAX) {
        long g;
        float x, y, z, qx = 0, qy = 0, qz = 0, qw = 1;
        if (sscanf(line, "%ld %f %f %f %f %f %f %f", &g, &x, &y, &z, &qx, &qy, &qz, &qw) < 4) continue;
        ++lines;
        if (mm_map_key(g) != key) continue;
        const float n = sqrtf(qx * qx + qy * qy + qz * qz + qw * qw);
        if (n < 0.5f || n > 1.5f) { qx = qy = qz = 0; qw = 1; }
        else { qx /= n; qy /= n; qz /= n; qw /= n; }
        mm_file_pts[mm_file_n].p[0] = x; mm_file_pts[mm_file_n].p[1] = y; mm_file_pts[mm_file_n].p[2] = z;
        mm_file_pts[mm_file_n].q[0] = qx; mm_file_pts[mm_file_n].q[1] = qy; mm_file_pts[mm_file_n].q[2] = qz; mm_file_pts[mm_file_n].q[3] = qw;
        ++mm_file_n;
    }
    fclose(f);
    printf("CoopMP: spawn file %s: %d of %d point(s) are on this map\n", path, mm_file_n, lines);
}
static void mm_netinit(long game, int competitive) {
    mm_competitive = competitive;
    const char *v = getenv("GOW3_COOP_MP_CLASSIC");
    mm_classic_on = !(v && v[0] == '0');
    v = getenv("GOW3_COOP_MP_SPAWNS");
    mm_spawns_on = !(v && v[0] == '0');
    v = getenv("GOW3_COOP_MP_SPAWN_DUEL");
    mm_duel_on = v && v[0] == '1';
    v = getenv("GOW3_COOP_MP_SPAWN_PROBE");
    mm_probe_secs = v && *v ? atoi(v) : 0;
    if (!competitive) return;
    if (mm_classic_on && !getenv("GOW3_COOP_MP_LEVELS")) mm_classic_levels(game);
    else { const uint64_t me = u2_net_map_entry(game); if (me) rd(me + 0x20, &mm_net_pak, 4); }
    mm_file_load(game);   /* the user's spawn points (F8 recorder) */
    if (mm_alias(game) < 0) puts("CoopMP: objectives: net-game filter NOT patched (bytes differ)");
    {   /* classic maps' objectives (mm_objects_frame) */
        const int32_t mode = net_game_mode(game);
        const char *o = getenv("GOW3_COOP_MP_OBJECTS");
        int classic = 0;
        for (size_t m = 0; m < sizeof(mm_maps) / sizeof(*mm_maps); ++m) classic |= sid(mm_maps[m].net_pak) == mm_net_pak;
        if (classic && (mode == 1 || mode == 5 || mode == 6 || mode == 7) && !(o && o[0] == '0')) {
            mm_obj_mode = mode;
            printf("CoopMP: objectives: mode %d on a classic map: our records go into the net pak level once it is loaded\n", mode);
        }
    }
    if (mm_spawns_on)
        printf("CoopMP: spawns on every map: %s\n", mm_install() ? "on (locator 0x662fc9: any level; choices 0x644220 / 0x642d20: our pick when the game has none)"
                                                                 : "NOT installed (bytes differ)");
    else mm_competitive = 0;
    if (mm_probe_secs) {   /* test: 1 s respawn time (*net-games* +0x24) */
        const uint64_t entry = u2_net_game_entry(game);
        const float one = 1.0f;
        if (entry) wr(entry + 0x24, &one, 4);
        printf("CoopMP: spawn probe every %d s (respawn time 1 s)\n", mm_probe_secs);
    }
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
    /* the classic maps: their main level and the PS3 online spawners (runtime_uc2mp_netmaps.c, from
     * PR #231 by Sparks); our own classic-map spawn picks (mm_netinit) stay for the other maps.
     * GOW3_UC2MP_NETMAPS=0: off (the mm- path for every map). */
    const char *classic = !levels && competitive && knob_set("GOW3_UC2MP_NETMAPS") != 0 ? runtime_uc2mp_netmaps_levels(image, game) : NULL;
    if (classic) levels = classic;
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
    if (classic) runtime_uc2mp_netmaps_install(image);   /* PS3 online spawners (ChooseSpawn 0x644220, GetSpawnLocator 0x662fa0) */
    else mm_netinit(game, competitive);   /* mm- agent: classic maps' levels + spawns on every map (GOW3_COOP_MP_CLASSIC / _SPAWNS) */
    install_skins(skin);
    {
        /* remote players' animation (see install_netanim); the default pose is the fallback */
        static const unsigned char jb[6] = {0x0f, 0x82, 0x5f, 0x01, 0x00, 0x00}, jmp[6] = {0xe9, 0x60, 0x01, 0x00, 0x00, 0x90};
        const char *v = getenv("GOW3_COOP_NETANIM");
        const char *depth = getenv("GOW3_COOP_NETANIM_DEPTH");
        if (depth && *depth) na_depth_limit = (unsigned)atoi(depth);
        const int hooks = install_netanim();
        /* GOW3_COOP_NETANIM_ANIMS=<4..6> (test knob, UNTESTED, default off = the game's 3): how
         * many anim leaves the Player's snapshot writer sends (0x60e285 mov r9d, 3). Run a05: once
         * the sender stood turned round, ~200 parses/s ended early on the receiver (bind poses);
         * suspect: trees with more than 3 anims, whose skipped leaves' blends are still written. */
        const char *anims = getenv("GOW3_COOP_NETANIM_ANIMS");
        static const unsigned char cap3[6] = {0x41, 0xb9, 0x03, 0x00, 0x00, 0x00};
        if (hooks && anims && atoi(anims) >= 4 && atoi(anims) <= 6 && !memcmp(image + 0x60e285, cap3, 6)) {
            const unsigned char n = (unsigned char)atoi(anims);
            if (wr((uint64_t)(uintptr_t)image + 0x60e287, &n, 1)) printf("CoopMP: snapshot writer sends up to %u anims (0x60e285)\n", n);
        }
        const int idle = !hooks || (v && v[0] == '0');
        if (idle && !memcmp(image + 0x5f3ccf, jb, 6)) wr((uint64_t)(uintptr_t)image + 0x5f3ccf, jmp, 6);
        printf("CoopMP: remote animation: snapshot leaves %s; remote players %s; snapshot bit counts padded to "
               "whole words at %d of 2 writer sites (0x60e296, 0x60e9f3)\n",
               hooks ? "carry clip phases (0x6364b5, 0x66b637, 0x66c035, 0x5f3d63)" : "NOT changed",
               idle ? "keep the default pose (0x5f3ccf)" : "play the sender's animation", na_pad_sites);
    }
    {
        const char *v = getenv("GOW3_COOP_MP_RESULTS_SECONDS");
        if (v && *v) results_hold_ms = (unsigned)(atof(v) * 1000);
        if (results_hold_ms)
            printf("CoopMP: round results: %s (%u s, then everyone leaves)\n",
                   install_results_hold() ? "hold on (ShowResults Update 0x5f0b30)" : "hold NOT installed", results_hold_ms / 1000);
    }
    {
        /* co-op enemies (np- agent): NPC events on the wire + the joiner's binding; GOW3_COOP_NPC=0 = off */
        const char *v = getenv("GOW3_COOP_NPC");
        if (!(v && v[0] == '0'))
            printf("CoopMP: NPC sync: %s\n", install_npc_sync()
                       ? "on (NpcSpawn/NpcDespawn/0x2c serializers, joiner binding 0x23bb5a, despawn 0x240fcf, move counts 0x638332)"
                       : "NOT fully installed (see the lines above)");
    }
    report_game(game);
    fflush(stdout);
}
int runtime_coop_mp_command(unsigned char *base, const char *op, const char *args) {
    image = base;
    (void)args;
    if (!strcmp(op, "mmspawns")) {   /* mm- agent: the spawn candidates of this map */
        AcquireSRWLockExclusive(&mm_lock);
        mm_build();
        for (int i = 0; i < mm_n; ++i)
            printf("CoopMP: spawn candidate #%d %c %08x %s at %.2f %.2f %.2f\n", i, mm_c[i].cls, mm_c[i].id, mm_c[i].name,
                   mm_c[i].p[0], mm_c[i].p[1], mm_c[i].p[2]);
        ReleaseSRWLockExclusive(&mm_lock);
        fflush(stdout);
        return 1;
    }
    return 0;
}
#else
void runtime_coop_mp_frame(void) {}
void runtime_coop_mp_netinit(unsigned char *base, long game) { (void)base; (void)game; }
void runtime_coop_mp_slot_skin(unsigned char *base, uint64_t slot, int id) { (void)base; (void)slot; (void)id; }
int runtime_coop_mp_command(unsigned char *base, const char *op, const char *args) { (void)base; (void)op; (void)args; return 0; }
#endif
