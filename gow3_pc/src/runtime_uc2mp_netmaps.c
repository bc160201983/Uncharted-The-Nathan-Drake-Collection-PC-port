/* The classic Uncharted 2 maps in the native net game (uc2mp, 25 Oct 2026).
 *
 * The competitive spawn choice (ChooseSpawn 0x644220: NetGameManager, slot -> spawner sid) and
 * GetSpawnLocator (0x662fa0: locator out, spawner sid, index -> found) read the online spawners
 * (tag online-spawner 0x5bfae217) of the net pak level only (0x644130: the level of *net-maps*
 * +0x20). The Collection lost every classic map's net pak ("<map>-multi-pak"), and their main
 * levels carry no online spawner, so runtime_coop_mp.c could only offer the Fort, whose main level
 * still has them. The PS3 wrappers kept them: uc2mp_extracted_spawn_catalog.h (poses only,
 * generated from the player's own PS3 files by tools/uc2mp_generate_spawn_catalog.py).
 *
 * For a competitive net game on Village, Plaza (warzone), Sanctuary (gompa), Ice Cave, Lost City
 * (shambhala), Train Wreck or Temple (treasure): the map's main level stands in for the net pak and
 * its companion levels load with it (uc2mp_map_levels.h, the PS3 net.bin lists); both functions
 * get an entry detour that answers from the catalogue. ChooseSpawn takes the catalogue spawner
 * farthest from where the other players last spawned (never the one this player just used);
 * GetSpawnLocator returns its pose. Every machine runs the same table, and the spawner sid is
 * what the native player events carry. Off unless runtime_coop_mp.c's netinit picks one of those
 * maps; GOW3_COOP_MP_LEVELS (set by hand) wins. The Fort keeps its own spawners. */
#include "runtime_uc2mp_netmaps.h"
#include "runtime.h"
#include "uc2mp_extracted_spawn_catalog.h"
#include "uc2mp_map_levels.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>

#define NETMAPS_CHOOSE 0x644220u
#define NETMAPS_LOCATOR 0x662fa0u
#define NETMAPS_SLOTS 10

typedef uint64_t (ABI *NetmapsDc)(uint32_t id);
typedef int (ABI *NetmapsLocator)(float *out, uint32_t sid, int32_t index);
typedef uint32_t (ABI *NetmapsChoose)(unsigned char *manager, unsigned char *slot);

static unsigned char *netmaps_image;
static const Uc2mpExtractedSpawnMap *netmaps_map;
static NetmapsLocator netmaps_locator_original;
static NetmapsChoose netmaps_choose_original;
static char netmaps_levels[256];
/* The competitive spawners of the map (the co-op ones left out) and where each slot last spawned. */
static const Uc2mpExtractedSpawnPose *netmaps_spawners[64];
static unsigned netmaps_count;
static int netmaps_last[NETMAPS_SLOTS];
static unsigned netmaps_choices;

static int netmaps_read(uint64_t address, void *out, size_t n) {
    if (!address || !runtime_memory_is_mapped((uintptr_t)address, n)) return 0;
    memcpy(out, (const void *)(uintptr_t)address, n);
    return 1;
}
static uint64_t netmaps_dc(uint32_t id) { return ((NetmapsDc)(netmaps_image + 0xa15e70))(id); }
static int netmaps_text(uint64_t address, char *out, size_t size) {
    memset(out, 0, size);
    if (!address) return 0;
    for (size_t i = 0; i + 1 < size; ++i) {
        if (!netmaps_read(address + i, out + i, 1)) return 0;
        if (!out[i]) return 1;
    }
    return 1;
}

/* *net-games* entry `game` (0x90 bytes: +4 map sid, +0xc mode) and its *net-maps* entry (0x38:
 * +0 map sid, +0x18 net pak level, +0x28 {u32 count, {char *level, u32 sid}[]}). */
static uint64_t netmaps_game(long game) {
    const uint64_t table = netmaps_dc(0x3b9a067du);
    int32_t count = 0;
    uint64_t entries = 0;
    if (!netmaps_read(table, &count, 4) || !netmaps_read(table + 8, &entries, 8) || !entries ||
        game < 0 || game >= count) return 0;
    return entries + (uint64_t)game * 0x90;
}
static uint64_t netmaps_map_entry(uint32_t map) {
    const uint64_t maps = netmaps_dc(0xdec97a4u);
    int32_t count = 0;
    uint64_t entries = 0;
    if (!netmaps_read(maps, &count, 4) || !netmaps_read(maps + 8, &entries, 8) || !entries) return 0;
    for (int32_t i = 0; i < count; ++i) {
        uint32_t id = 0;
        if (netmaps_read(entries + (uint64_t)i * 0x38, &id, 4) && id == map) return entries + (uint64_t)i * 0x38;
    }
    return 0;
}
/* The catalogue map among the levels a *net-maps* entry names (its net pak "<map>-pak" too). */
static const Uc2mpExtractedSpawnMap *netmaps_catalogue(uint64_t entry, char *pak, size_t pak_size) {
    uint64_t name = 0, head = 0, list = 0;
    uint32_t count = 0;
    netmaps_read(entry + 0x18, &name, 8);
    netmaps_text(name, pak, pak_size);
    char level[64];
    snprintf(level, sizeof(level), "%s", pak);
    const size_t n = strlen(level);
    if (n > 4 && !strcmp(level + n - 4, "-pak")) level[n - 4] = 0;
    const Uc2mpExtractedSpawnMap *map = uc2mp_extracted_spawn_map(level);
    if (map) return map;
    if (!netmaps_read(entry + 0x28, &head, 8) || !netmaps_read(head, &count, 4) || !netmaps_read(head + 8, &list, 8))
        return NULL;
    for (uint32_t i = 0; i < count && i < 16; ++i)
        if (netmaps_read(list + (uint64_t)i * 16, &name, 8) && netmaps_text(name, level, sizeof(level)) &&
            (map = uc2mp_extracted_spawn_map(level))) return map;
    return NULL;
}

static void netmaps_list(void) {
    const uint64_t table = netmaps_dc(0x3b9a067du);
    int32_t count = 0;
    netmaps_read(table, &count, 4);
    printf("UC2MP netmaps: %d net games (*net-games*):\n", count);
    for (long game = 0; game < count; ++game) {
        const uint64_t entry = netmaps_game(game);
        uint32_t map = 0;
        int32_t mode = -1;
        netmaps_read(entry + 4, &map, 4);
        netmaps_read(entry + 0xc, &mode, 4);
        char pak[64] = "";
        const uint64_t me = netmaps_map_entry(map);
        const Uc2mpExtractedSpawnMap *catalogue = me ? netmaps_catalogue(me, pak, sizeof(pak)) : NULL;
        printf("  %3ld mode %2d map %08x net pak %-28s%s%s\n", game, mode, map, *pak ? pak : "-",
               catalogue ? " catalogue " : "", catalogue ? catalogue->level_name : "");
    }
    fflush(stdout);
}

const char *runtime_uc2mp_netmaps_levels(unsigned char *image, long game) {
    netmaps_image = image;
    netmaps_map = NULL;
    netmaps_count = 0; /* a later net game on another map: the detours pass through */
    const char *list = getenv("UC2MP_NET_GAMES_LIST");
    if (list && list[0] == '1') netmaps_list();
    const uint64_t entry = netmaps_game(game);
    uint32_t map = 0;
    if (!entry || !netmaps_read(entry + 4, &map, 4)) return NULL;
    const uint64_t me = netmaps_map_entry(map);
    char pak[64];
    const Uc2mpExtractedSpawnMap *catalogue = me ? netmaps_catalogue(me, pak, sizeof(pak)) : NULL;
    /* The Fort's main level has its own online spawners (runtime_coop_mp.c's spawner_levels). */
    if (!catalogue || !strcmp(catalogue->level_name, "the-fort-multi")) return NULL;
    int n = snprintf(netmaps_levels, sizeof(netmaps_levels), "%s,%s", catalogue->level_name, catalogue->level_name);
    for (const char *const *level = uc2mp_map_companions(catalogue->level_name); *level && n > 0; ++level)
        n += snprintf(netmaps_levels + n, sizeof(netmaps_levels) - (size_t)n, ",%s", *level);
    netmaps_map = catalogue;
    return netmaps_levels;
}

/* ---- the two detours ---- */
static float netmaps_distance(const Uc2mpExtractedSpawnPose *a, const Uc2mpExtractedSpawnPose *b) {
    const float x = a->pose[0] - b->pose[0], y = a->pose[1] - b->pose[1], z = a->pose[2] - b->pose[2];
    return sqrtf(x * x + y * y + z * z);
}
static ABI uint32_t netmaps_choose(unsigned char *manager, unsigned char *slot) {
    if (!netmaps_count) return netmaps_choose_original(manager, slot);
    /* the slot's place in the manager (slots of 0x2c0 bytes from +0x10): player id = place + 1 */
    const intptr_t offset = slot - (manager + 0x10);
    const int self = offset >= 0 && offset % 0x2c0 == 0 && offset / 0x2c0 < NETMAPS_SLOTS ? (int)(offset / 0x2c0) : -1;
    const int id = self + 1;
    /* The farthest from the others' last spawns; ties (nobody spawned yet) rotate with the
     * choice count and the player id, so two players start apart. */
    int best = -1;
    float best_distance = -1.0f;
    for (unsigned k = 0; k < netmaps_count; ++k) {
        const unsigned i = (k + netmaps_choices * 7u + (unsigned)(id > 0 ? id : 0) * 13u) % netmaps_count;
        if (self >= 0 && netmaps_last[self] == (int)i + 1 && netmaps_count > 1) continue;
        float nearest = 1e9f;
        for (int other = 0; other < NETMAPS_SLOTS; ++other)
            if (other != self && netmaps_last[other])
                nearest = fminf(nearest, netmaps_distance(netmaps_spawners[i], netmaps_spawners[netmaps_last[other] - 1]));
        if (nearest > best_distance) { best_distance = nearest; best = (int)i; }
    }
    if (best < 0) return 0;
    if (self >= 0) netmaps_last[self] = best + 1;
    if (netmaps_choices++ < 16) {
        printf("UC2MP netmaps: player %d spawns at %s (%.1f m from the others' last spawns)\n", id,
               netmaps_spawners[best]->name, best_distance > 1e8f ? 0.0 : (double)best_distance);
        fflush(stdout);
    }
    return netmaps_spawners[best]->sid;
}
static ABI int netmaps_locate(float *out, uint32_t sid, int32_t index) {
    for (unsigned i = 0; i < netmaps_count; ++i)
        if (netmaps_spawners[i]->sid == sid) {
            (void)index; /* the co-op offsets (index 1..4) do not apply to competitive spawns */
            memcpy(out, netmaps_spawners[i]->pose, sizeof(netmaps_spawners[i]->pose));
            return 1;
        }
    return netmaps_locator_original(out, sid, index);
}

/* The entry bytes of a function, replayed, then jmp [rip] back after them. */
static void *netmaps_trampoline(unsigned char *stub, uint64_t site, const unsigned char *entry, size_t n) {
    static const unsigned char jump[6] = {0xff, 0x25, 0, 0, 0, 0};
    const uint64_t back = (uint64_t)(uintptr_t)(netmaps_image + site + n);
    memcpy(stub, entry, n);
    memcpy(stub + n, jump, sizeof(jump));
    memcpy(stub + n + sizeof(jump), &back, 8);
    return stub;
}
/* State scripts' GetDeltaSeconds (0xa477c0, ss-track-group.cpp:667) asserts when the script's
 * process has flag 8 at +0x168 (a networked process: Village's level scripts in a net game,
 * 25 Oct; the Fort has none) and faults at 0xa4788f after it. Such processes get the frame's
 * delta like every other (je -> jmp at 0xa4785a). */
static int netmaps_script_delta(void) {
    unsigned char *at = netmaps_image + 0xa4785a;
    if (at[0] == 0xeb && at[1] == 0x37) return 1;
    if (at[0] != 0x74 || at[1] != 0x37) return 0;
    DWORD old;
    if (!VirtualProtect(at, 1, PAGE_EXECUTE_READWRITE, &old)) return 0;
    at[0] = 0xeb;
    VirtualProtect(at, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 1);
    return 1;
}
/* movabs rax, target; jmp rax (12 bytes) over a function's entry. */
static int netmaps_jump(uint64_t site, const void *target) {
    unsigned char patch[12] = {0x48, 0xb8};
    const uint64_t to = (uint64_t)(uintptr_t)target;
    memcpy(patch + 2, &to, 8);
    patch[10] = 0xff;
    patch[11] = 0xe0;
    DWORD old;
    if (!VirtualProtect(netmaps_image + site, sizeof(patch), PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(netmaps_image + site, patch, sizeof(patch));
    VirtualProtect(netmaps_image + site, sizeof(patch), old, &old);
    FlushInstructionCache(GetCurrentProcess(), netmaps_image + site, sizeof(patch));
    return 1;
}

int runtime_uc2mp_netmaps_install(unsigned char *image) {
    netmaps_image = image;
    if (!netmaps_map) return 0;
    /* push rbp; mov rbp, rsp; push r15..r12 (ChooseSpawn); + push rbx; sub rsp, 0x48 (the locator) */
    static const unsigned char choose_entry[12] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54};
    static const unsigned char locator_entry[17] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                                    0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x48};
    netmaps_count = 0;
    memset(netmaps_last, 0, sizeof(netmaps_last));
    for (size_t i = 0; i < netmaps_map->player_count && netmaps_count < sizeof(netmaps_spawners) / sizeof(*netmaps_spawners); ++i)
        if (!strstr(netmaps_map->players[i].name, "-coop")) netmaps_spawners[netmaps_count++] = &netmaps_map->players[i];
    if (!netmaps_count) return 0;
    if (!netmaps_locator_original) {
        if (memcmp(image + NETMAPS_CHOOSE, choose_entry, sizeof(choose_entry)) ||
            memcmp(image + NETMAPS_LOCATOR, locator_entry, sizeof(locator_entry))) {
            printf("UC2MP netmaps: unknown ChooseSpawn / GetSpawnLocator entries: %s keeps no spawner\n",
                   netmaps_map->level_name);
            return 0;
        }
        unsigned char *stub = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (!stub) return 0;
        netmaps_locator_original = (NetmapsLocator)netmaps_trampoline(stub, NETMAPS_LOCATOR, locator_entry, sizeof(locator_entry));
        netmaps_choose_original = (NetmapsChoose)netmaps_trampoline(stub + 64, NETMAPS_CHOOSE, choose_entry, sizeof(choose_entry));
        DWORD old;
        VirtualProtect(stub, 4096, PAGE_EXECUTE_READ, &old);
        if (!netmaps_jump(NETMAPS_LOCATOR, (const void *)netmaps_locate) ||
            !netmaps_jump(NETMAPS_CHOOSE, (const void *)netmaps_choose)) return 0;
    }
    printf("UC2MP netmaps: %s: %u online spawners from the PS3 catalogue (ChooseSpawn 0x644220, "
           "GetSpawnLocator 0x662fa0); networked scripts' delta %s\n", netmaps_map->level_name, netmaps_count,
           netmaps_script_delta() ? "= the frame's (0xa4785a)" : "NOT patched");
    fflush(stdout);
    return 1;
}
#else
const char *runtime_uc2mp_netmaps_levels(unsigned char *image, long game) { (void)image; (void)game; return NULL; }
int runtime_uc2mp_netmaps_install(unsigned char *image) { (void)image; return 0; }
#endif
