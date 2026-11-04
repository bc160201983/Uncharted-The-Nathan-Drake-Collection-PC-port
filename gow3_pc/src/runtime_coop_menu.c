/* gow3 25 Oct 2026: Uncharted 2 multiplayer menus in a net game (research, console only).
 *
 * The multiplayer pages (lobby, scoreboard, results, ...) are still defined in menu.bin
 * (*menu-pages*, all 103 pages loaded at GameInit by 0x5c53a0 into the menu host 0x1136640,
 * +0x370[200]); only the session layer that showed them is gone. This file shows them on the
 * game's own menu host. Notes: analysis/triage-1022/coop-menu.md.
 *
 * Menu calls must run inside the frame: the menu event constructor 0xccf830 reads the frame
 * parameters [0x159d490]/[0x159d498], which GameLoopUpdate sets at 0x3fae6b/0x3fae74 and clears
 * at 0x3fb9f0. The coop console runs at the entry of GameLoopUpdate, where they are NULL (run m1:
 * fault at 0xccf97c). So the console commands below only queue the call; an in-frame hook at
 * 0x3fae85 (rdtsc, right after the frame parameters are set) runs the queue on the game thread.
 * The hook is installed on the first command (nothing changes until a command is given).
 *
 * Console commands (GOW3_COOP_CMD / GOW3_COOP_BOOT, through runtime_coop.c):
 *   mpmenu show <page>   MenuHost::ShowPageInGame 0x5ccf80(host, page): opens the in-game menu
 *                        (pause mode) on that page (the game's own path for the kick page, 0x646850)
 *   mpmenu goto <page>   MenuHost::GoToPage 0x5cb990(host, page, 0) (page on top of the open menu)
 *   mpmenu back          MenuHost::Back 0x5cc6f0(host) (previous page; closes at the root)
 *   mpmenu state         host mode (+0: 0 main, 1 pause, 2 results), state (+0x1014), page stack
 *   <page>: lobby, scoreboard, results, roundresults, postgame, loading, multiplayer, pause,
 *   matchmaking, leaving, lastresults, a full name (PAGE_...), or a hex string id.
 * Uncharted 3 commands (u3 ...) are passed on to runtime_coop_u3.c.
 */
#include "runtime.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

#ifdef _WIN32
static unsigned char *image;

#define U2_MENU_HOST 0x1136640u
#define U2_MODE 0x14ce97cu
#define U2_NGM 0x113b760u
#define U2_FRAME_SITE 0x3fae85u

static int rd(uint64_t address, void *out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)address, out, n, &got) && got == n;
}
static int wr(uint64_t address, const void *in, size_t n) {
    SIZE_T put = 0;
    return WriteProcessMemory(GetCurrentProcess(), (LPVOID)(uintptr_t)address, in, n, &put) && put == n;
}
static uint32_t sid(const char *s) {
    uint32_t c = 0;
    for (; *s; ++s) {
        c ^= (uint32_t)(unsigned char)*s << 24;
        for (int k = 0; k < 8; ++k) c = c & 0x80000000u ? (c << 1) ^ 0x04c11db7u : c << 1;
    }
    return c;
}

/* U2 1.02 bytes at the in-frame site: lea rax, [rip+0x105390f]; mov byte [rax], 1; rdtsc;
 * shl rdx, 0x20; or rdx, rax. The hook replaces rdtsc + shl (6 bytes) and replays them. */
static const unsigned char u2_frame_bytes[19] = {0x48, 0x8d, 0x05, 0x0f, 0x39, 0x05, 0x01, 0xc6, 0x00, 0x01,
                                                 0x0f, 0x31, 0x48, 0xc1, 0xe2, 0x20, 0x48, 0x09, 0xc2};
static int is_u2(void) {
    /* 25 Oct: the NPC sync (runtime_coop_mp.c np_frame_start) hooks the lea at 0x3fae7b (7 bytes)
     * right before ours, so only the bytes from 0x3fae82 on (mov byte [rax], 1; rdtsc; shl; or) are
     * checked; our 6 bytes at 0x3fae85 do not overlap it. */
    return image && !memcmp(image + U2_FRAME_SITE - 3, u2_frame_bytes + 7, sizeof(u2_frame_bytes) - 7);
}

static unsigned char *stub_page(void) {
    static unsigned char *page;
    if (page) return page;
    const uintptr_t lo = (uintptr_t)image > UINT64_C(0x60000000) ? (uintptr_t)image - UINT64_C(0x60000000) : 0x10000;
    const uintptr_t hi = (uintptr_t)image + UINT64_C(0x60000000) + UINT64_C(0x4000000);
    MEMORY_BASIC_INFORMATION info;
    for (uintptr_t address = lo; address < hi && VirtualQuery((void *)address, &info, sizeof(info));
         address = (uintptr_t)info.BaseAddress + info.RegionSize) {
        if (info.State != MEM_FREE) continue;
        const uintptr_t start = ((uintptr_t)info.BaseAddress + 0xffff) & ~(uintptr_t)0xffff;
        if (start + 4096 > (uintptr_t)info.BaseAddress + info.RegionSize) continue;
        void *p = VirtualAlloc((void *)start, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (p) { page = p; return page; }
    }
    return NULL;
}

/* Mid-function stub: keeps the red zone, aligns the stack, saves rax..r11 and xmm0-15, calls the
 * handler, restores, replays the replaced bytes, jumps back. */
static unsigned build_mid_stub(unsigned char *p, void *handler, const unsigned char *replayed, unsigned length,
                               uintptr_t resume) {
    unsigned n = 0;
    static const unsigned char head[] = {
        0x48, 0x8d, 0x64, 0x24, 0x80,             /* lea rsp, [rsp-0x80] (red zone) */
        0x55,                                     /* push rbp */
        0x48, 0x89, 0xe5,                         /* mov rbp, rsp */
        0x48, 0x83, 0xe4, 0xf0,                   /* and rsp, -16 */
        0x48, 0x81, 0xec, 0x50, 0x01, 0x00, 0x00, /* sub rsp, 0x150 */
        0x48, 0x89, 0x04, 0x24,                   /* mov [rsp], rax */
        0x48, 0x89, 0x4c, 0x24, 0x08,             /* mov [rsp+8], rcx */
        0x48, 0x89, 0x54, 0x24, 0x10,             /* rdx */
        0x48, 0x89, 0x74, 0x24, 0x18,             /* rsi */
        0x48, 0x89, 0x7c, 0x24, 0x20,             /* rdi */
        0x4c, 0x89, 0x44, 0x24, 0x28,             /* r8 */
        0x4c, 0x89, 0x4c, 0x24, 0x30,             /* r9 */
        0x4c, 0x89, 0x54, 0x24, 0x38,             /* r10 */
        0x4c, 0x89, 0x5c, 0x24, 0x40,             /* r11 */
    };
    memcpy(p + n, head, sizeof(head)); n += sizeof(head);
    for (int i = 0; i < 16; ++i) {                /* movdqu [rsp+0x50+i*16], xmmi */
        p[n++] = 0xf3;
        if (i >= 8) p[n++] = 0x44;
        p[n++] = 0x0f; p[n++] = 0x7f; p[n++] = 0x84 | ((i & 7) << 3); p[n++] = 0x24;
        const int32_t d = 0x50 + i * 16;
        memcpy(p + n, &d, 4); n += 4;
    }
    p[n++] = 0x48; p[n++] = 0xb8;                 /* mov rax, handler */
    const uint64_t h = (uint64_t)(uintptr_t)handler;
    memcpy(p + n, &h, 8); n += 8;
    p[n++] = 0xff; p[n++] = 0xd0;                 /* call rax */
    for (int i = 0; i < 16; ++i) {                /* movdqu xmmi, [rsp+0x50+i*16] */
        p[n++] = 0xf3;
        if (i >= 8) p[n++] = 0x44;
        p[n++] = 0x0f; p[n++] = 0x6f; p[n++] = 0x84 | ((i & 7) << 3); p[n++] = 0x24;
        const int32_t d = 0x50 + i * 16;
        memcpy(p + n, &d, 4); n += 4;
    }
    static const unsigned char tail[] = {
        0x48, 0x8b, 0x04, 0x24, 0x48, 0x8b, 0x4c, 0x24, 0x08, 0x48, 0x8b, 0x54, 0x24, 0x10,
        0x48, 0x8b, 0x74, 0x24, 0x18, 0x48, 0x8b, 0x7c, 0x24, 0x20, 0x4c, 0x8b, 0x44, 0x24, 0x28,
        0x4c, 0x8b, 0x4c, 0x24, 0x30, 0x4c, 0x8b, 0x54, 0x24, 0x38, 0x4c, 0x8b, 0x5c, 0x24, 0x40,
        0x48, 0x89, 0xec,                         /* mov rsp, rbp */
        0x5d,                                     /* pop rbp */
        0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00, /* lea rsp, [rsp+0x80] */
    };
    memcpy(p + n, tail, sizeof(tail)); n += sizeof(tail);
    memcpy(p + n, replayed, length); n += length;
    static const unsigned char jump[] = {0xff, 0x25, 0x00, 0x00, 0x00, 0x00};
    memcpy(p + n, jump, sizeof(jump)); n += sizeof(jump);
    const uint64_t target = (uint64_t)resume;
    memcpy(p + n, &target, 8); n += 8;
    return n;
}

/* ---- the queue of menu calls, run inside the frame ---- */
typedef struct { int op; uint32_t page; uint64_t fn, a[4]; } MenuCall;
enum { CALL_SHOW = 1, CALL_GOTO, CALL_BACK, CALL_STATE, CALL_GUEST, CALL_ENDGAME };
static MenuCall queue[16];
static volatile int queue_head, queue_tail;
typedef uint64_t (ABI *Fn2)(uint64_t, uint64_t);
typedef uint64_t (ABI *Fn3)(uint64_t, uint64_t, uint64_t);
typedef uint64_t (ABI *Fn1)(uint64_t);
typedef uint64_t (ABI *Fn4)(uint64_t, uint64_t, uint64_t, uint64_t);

static void print_state(const char *when) {
    const uint64_t host = (uint64_t)(uintptr_t)image + U2_MENU_HOST;
    int32_t hmode = -1, hstate = -1, top = -1, depth = -1, mode = -1;
    int16_t game = -1;
    rd(host, &hmode, 4);
    rd(host + 0x1014, &hstate, 4);
    rd(host + 0x1010, &top, 4);
    rd(host + 0x1160, &depth, 4);
    rd((uint64_t)(uintptr_t)image + U2_MODE, &mode, 4);
    rd((uint64_t)(uintptr_t)image + U2_NGM + 0x1ca0, &game, 2);
    printf("CoopMenu: %s: host mode %d state %d, net mode %d, net game %d, shown pages:", when, hmode, hstate, mode, game);
    for (int i = 0; i <= top && i < 8; ++i) {
        uint64_t page = 0;
        uint32_t id = 0;
        if (rd(host + 0x9d0 + 8 * (uint64_t)i, &page, 8) && page && rd(page + 0x10, &id, 4)) printf(" %08x", id);
        else printf(" -");
    }
    printf("; saved stack (%d):", depth);
    for (int i = 0; i <= depth && i < 8; ++i) {
        uint32_t id = 0;
        rd(host + 0x1164 + 4 * (uint64_t)i, &id, 4);
        printf(" %08x", id);
    }
    printf("\n");
}


/* ---- mpmenu auto: the results screens of the online task (what the removed layer showed) ----
 * - ShowResults (state vtable 0x100aa40): its Enter 0x5f0ea0 is an empty `ret` (16 bytes with the
 *   padding), hooked: the hook shows PAGE_MULTIPLAYER_RESULTS.
 * - ShowRoundResults (0x100aa00): its Enter sets the net mode to 0x1e; seen by the frame tick,
 *   which shows PAGE_MULTIPLAYER_ROUND_RESULTS. PreResetting (0x1b) / Playing (0x1a) close it.
 * - The saved page stack's entry 0 (the main menu root, never set in a dev boot) is set to
 *   PAGE_ROOT_MAIN, so Leave's return to the menus (SetMode(0), 0x5c7c92) finds every page. */
static int auto_on;
static int32_t auto_last_mode = -1;
static int auto_shown;   /* 1: a results page is up (ours) */
#define PAGE_RESULTS 0x3d437466u
#define PAGE_ROUND_RESULTS 0x5dad5a68u
#define PAGE_ROOT_MAIN 0x37656388u
static void show_now(uint32_t page, const char *why) {
    const uint64_t host = (uint64_t)(uintptr_t)image + U2_MENU_HOST;
    ((Fn2)(image + 0x5ccf80))(host, page);
    auto_shown = 1;
    char what[64];
    snprintf(what, sizeof(what), "auto: %s -> show %08x", why, page);
    print_state(what);
    fflush(stdout);
}
static void close_now(const char *why) {
    const uint64_t host = (uint64_t)(uintptr_t)image + U2_MENU_HOST;
    for (int i = 0; i < 4; ++i) {
        int32_t state = 0;
        rd(host + 0x1014, &state, 4);
        if (!state || state == 5) break;
        ((Fn1)(image + 0x5cc6f0))(host);
    }
    auto_shown = 0;
    char what[64];
    snprintf(what, sizeof(what), "auto: %s -> closed", why);
    print_state(what);
    fflush(stdout);
}
static void ABI on_show_results(void) {
    if (auto_on) show_now(PAGE_RESULTS, "ShowResults");
}
static void auto_tick(void) {
    if (!auto_on) return;
    const uint64_t host = (uint64_t)(uintptr_t)image + U2_MENU_HOST;
    uint32_t root = 0;
    if (rd(host + 0x1164, &root, 4) && !root) {
        const uint32_t main_root = PAGE_ROOT_MAIN;
        wr(host + 0x1164, &main_root, 4);
        puts("CoopMenu: auto: saved page stack root set to PAGE_ROOT_MAIN");
        fflush(stdout);
    }
    int32_t mode = 0;
    rd((uint64_t)(uintptr_t)image + U2_MODE, &mode, 4);
    if (mode == auto_last_mode) return;
    const int32_t was = auto_last_mode;
    auto_last_mode = mode;
    printf("CoopMenu: auto: net mode %d -> %d\n", was, mode);
    fflush(stdout);
    if (mode == 0x1e) show_now(PAGE_ROUND_RESULTS, "ShowRoundResults");
    else if ((mode == 0x1b || mode == 0x1a) && auto_shown) close_now(mode == 0x1b ? "PreResetting" : "Playing");
}
static int install_results_hook(void) {
    static const unsigned char orig[16] = {0xc3, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x2e,
                                           0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00};
    const uint64_t site = 0x5f0ea0;
    if (memcmp(image + site, orig, sizeof(orig))) return image[site] == 0xe9;
    unsigned char *stub = stub_page();
    if (!stub) return 0;
    stub += 2048;   /* second half of the page (the frame stub is at its start) */
    static const unsigned char ret = 0xc3;
    build_mid_stub(stub, (void *)on_show_results, &ret, 1, (uintptr_t)(image + site + 1));
    const int64_t delta = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + site + 5);
    if (delta < INT32_MIN || delta > INT32_MAX) return 0;
    unsigned char patch[5] = {0xe9};
    const int32_t rel = (int32_t)delta;
    memcpy(patch + 1, &rel, 4);
    return wr((uint64_t)(uintptr_t)(image + site), patch, sizeof(patch));
}

/* ---- mpmenu lobby host|join: the game's own Custom Game lobby before the match (25 Oct 2026) ----
 * The original flow lived in the menus, before the online task (coop-mp.md, lobby runs l01-l03):
 * - PAGE_LOBBY's update goes Back while the lobby kind (netinfo 0x104f1c8+0x2c4) is 0 or
 *   netinfo+0x1c is set; BUTTON_MULTIPLAYER_CUSTOM (0x5ecbd6) writes kind 2 and then GoToPage.
 * - Its rows (Start Game, Map, Difficulty) show only in the host's lobby step: net mode 3 (run l03;
 *   with net mode 0 the list is empty, run l02).
 * - BUTTON_MULTIPLAYER_STARTMATCH (sid 0x70e80076, menu host button table +0x18f0 {sid, fn, user}
 *   x 0x18, count +0x18ec) is a bare ret (0x5eb780): our handler takes its place.
 * - Start = the dev boot's own task start (GameInit 0x3f8c22..0x3f8c96): the task table
 *   0xa17280(0x6e51b48f, 0x67723261) -> {count, {sid, task} sorted}, then TaskStart 0xa5b020(task)
 *   with the task 'online'. runtime_net sends the host's start to the joiners (NET_KIND_START).
 * - After the round (Leave -> the menus), the lobby opens again. */
#define PAGE_LOBBY 0xb99a3235u
#define U2_NETINFO 0x104f1c8u
#define BUTTON_STARTMATCH 0x70e80076u
extern int runtime_net_ready(void);
extern int runtime_net_send_start(void);
extern int runtime_net_take_start(void);
extern int runtime_net_hosting(void);
static int lobby_role;              /* 0 off, 1 host, 2 joiner */
static int lobby_state;             /* 0 wait for the menus, 1 lobby shown, 2 starting, 3 in the match */
static volatile LONG lobby_pressed; /* the Start Game button (host) */
static ULONGLONG lobby_since;
static uint32_t top_page(void) {
    const uint64_t host = (uint64_t)(uintptr_t)image + U2_MENU_HOST;
    int32_t top = -1;
    uint64_t page = 0;
    uint32_t id = 0;
    if (!rd(host + 0x1010, &top, 4) || top < 0 || top > 32) return 0;
    if (rd(host + 0x9d0 + 8 * (uint64_t)top, &page, 8) && page && rd(page + 0x10, &id, 4)) return id;
    return 0;
}
static uint64_t ABI on_start_match(uint64_t event) {
    (void)event;
    if (lobby_state == 1) InterlockedExchange(&lobby_pressed, 1);
    return 0;
}
static int install_start_button(void) {
    const uint64_t host = (uint64_t)(uintptr_t)image + U2_MENU_HOST;
    int32_t count = 0;
    if (!rd(host + 0x18ec, &count, 4) || count <= 0 || count > 0x80) return 0;
    for (int i = 0; i < count; ++i) {
        const uint64_t entry = host + 0x18f0 + (uint64_t)i * 0x18;
        uint32_t id = 0;
        if (!rd(entry, &id, 4) || id != BUTTON_STARTMATCH) continue;
        const uint64_t fn = (uint64_t)(uintptr_t)on_start_match;
        return wr(entry + 8, &fn, 8);
    }
    return 0;
}
/* The party list (Party Leader / Party Members): the party session object 0x105efb8 (the same
 * class as the game session 0x1065b08: +0x10 the removed layer's session, +0xb00 our id, +0xb04
 * the leader's id, members at +0x20c, 10 x 0xd0: SceNpId (name at +0) | valid +0x25 | id +0x2c |
 * connection +0x34). It gets a stand-in and our LAN player list. GOW3_LOBBY_PARTY=0: off. */
#define U2_PARTY 0x105efb8u
extern int runtime_net_roster(int *ids, char (*names)[32], int max);
extern int runtime_net_local_id(void);
extern const char *runtime_net_local_name(void);
static __attribute__((aligned(64))) unsigned char party_standin[4096];
static void lobby_party(void) {
    static int off = -1, shown = -1;
    if (off < 0) { const char *v = getenv("GOW3_LOBBY_PARTY"); off = v && v[0] == '0'; }
    if (off) return;
    const uint64_t party = (uint64_t)(uintptr_t)image + U2_PARTY;
    uint64_t session = 0;
    if (rd(party + 0x10, &session, 8) && !session) {
        session = (uint64_t)(uintptr_t)party_standin;
        wr(party + 0x10, &session, 8);
    }
    int ids[10];
    char names[10][32];
    int n = runtime_net_roster(ids, names, 9);
    const int local = runtime_net_local_id() > 0 ? runtime_net_local_id() : 1;
    const char *me = runtime_net_local_name();
    ids[n] = local;
    snprintf(names[n], sizeof(names[n]), "%s", me && me[0] ? me : "Player");
    ++n;
    /* the leader (id 1, the host) first */
    for (int i = 1; i < n; ++i)
        if (ids[i] == 1) {
            int t = ids[0]; ids[0] = ids[i]; ids[i] = t;
            char s[32]; memcpy(s, names[0], 32); memcpy(names[0], names[i], 32); memcpy(names[i], s, 32);
        }
    for (int i = 0; i < 10; ++i) {
        unsigned char m[0xd0] = {0};
        if (i < n) {
            memcpy(m, names[i], 16);
            m[0x25] = 1;
            memcpy(m + 0x2c, &ids[i], 4);
            memcpy(m + 0x34, &ids[i], 4);
        }
        wr(party + 0x20c + (uint64_t)i * 0xd0, m, sizeof(m));
    }
    const int32_t leader = 1;
    wr(party + 0xb00, &local, 4);
    wr(party + 0xb04, &leader, 4);
    if (n != shown) {
        shown = n;
        printf("CoopMenu: lobby: party list: %d player(s)\n", n);
        fflush(stdout);
    }
}
static void lobby_keep(void) {
    const uint64_t netinfo = (uint64_t)(uintptr_t)image + U2_NETINFO;
    const int32_t kind = 2, zero = 0, step = 3;
    wr(netinfo + 0x2c4, &kind, 4);
    wr(netinfo + 0x1c, &zero, 4);
    wr((uint64_t)(uintptr_t)image + U2_MODE, &step, 4);
    lobby_party();
}
static int start_online_task(void) {
    typedef uint64_t (ABI *Lookup)(uint32_t, uint32_t);
    const uint64_t at = ((Lookup)(image + 0xa17280))(0x6e51b48fu, 0x67723261u);
    uint64_t table = 0, entries = 0;
    int32_t count = 0;
    if (!at || !rd(at, &table, 8) || !table || !rd(table, &count, 4) || !rd(table + 8, &entries, 8)) return 0;
    const uint32_t want = sid("online");
    for (int32_t i = 0; i < count && i < 256; ++i) {
        uint32_t id = 0;
        uint64_t task = 0;
        if (rd(entries + 16 * (uint64_t)i, &id, 4) && id == want && rd(entries + 16 * (uint64_t)i + 8, &task, 8) && task) {
            ((Fn1)(image + 0xa5b020))(task);
            return 1;
        }
    }
    return 0;
}
static void lobby_tick(void) {
    if (!lobby_role) return;
    const uint64_t host = (uint64_t)(uintptr_t)image + U2_MENU_HOST;
    const ULONGLONG now = GetTickCount64();
    static int played;   /* state 3: the match reached Playing (net modes 0x1a..0x1e) */
    int32_t hmode = -1, mode = 0;
    rd(host, &hmode, 4);
    rd((uint64_t)(uintptr_t)image + U2_MODE, &mode, 4);
    const uint32_t top = top_page();
    switch (lobby_state) {
    case 0: {   /* the menus up (main menu mode, a page shown), the net side ready: open the lobby */
        uint64_t net_heap = 0;
        rd((uint64_t)(uintptr_t)image + 0x11c6630 + 0x14 * 8, &net_heap, 8);
        const int ok = hmode == 0 && top && net_heap && runtime_net_ready() && (mode < 0x1a || mode > 0x1e);
        if (!ok) { lobby_since = 0; break; }
        if (!lobby_since) lobby_since = now;
        if (now - lobby_since < 1500) break;
        lobby_keep();
        const int button = install_start_button();
        ((Fn3)(image + 0x5cb990))(host, PAGE_LOBBY, 0);
        lobby_state = 1;
        lobby_pressed = 0;
        lobby_since = now;
        printf("CoopMenu: lobby: open (%s; Start Game button %s)\n", lobby_role == 1 ? "host" : "joiner",
               button ? "ours" : "NOT found");
        print_state("lobby open");
        fflush(stdout);
        break;
    }
    case 1: {   /* in the lobby: keep its state; the host's Start Game or the host's start message */
        lobby_keep();
        if (top != PAGE_LOBBY && now - lobby_since > 1000) {
            /* left the lobby (Back): open it again (this game was started for multiplayer) */
            puts("CoopMenu: lobby: left the lobby page, opening it again");
            fflush(stdout);
            lobby_state = 0;
            lobby_since = 0;
            break;
        }
        int go = 0;
        if (lobby_role == 1 && InterlockedExchange(&lobby_pressed, 0)) {
            const int sent = runtime_net_send_start();
            printf("CoopMenu: lobby: Start Game: sent to %d player(s)\n", sent);
            go = 1;
        } else if (lobby_role == 2 && runtime_net_take_start()) {
            puts("CoopMenu: lobby: the host started the game");
            go = 1;
        }
        if (go) { lobby_state = 2; lobby_since = now; }
        fflush(stdout);
        break;
    }
    case 2: {   /* close the lobby, then start the match */
        if (top == PAGE_LOBBY) { ((Fn1)(image + 0x5cc6f0))(host); break; }
        {   /* the party stand-in only lives in the lobby (the match code tests the party too) */
            const uint64_t none = 0;
            wr((uint64_t)(uintptr_t)image + U2_PARTY + 0x10, &none, 8);
        }
        /* Run lb02: TaskStart('online') from the menus crashed both games while the village level
         * logged in (0x4bf48b, *net-games* 0x3b9a067d NULL: the task switch reloads the DC modules).
         * The dev boot's own path works (task online at GameInit), so each game restarts into it
         * through the launcher's executable switch (GOW3_LOADEXEC_FILE, exit 30), as the
         * Collection's menu starts Uncharted 2. The host waits 1.5 s so the start reaches the
         * joiners first. GOW3_LOBBY_TASKSTART=1 tries the in-process task start instead. */
        static int in_process = -1;
        if (in_process < 0) { const char *v = getenv("GOW3_LOBBY_TASKSTART"); in_process = v && v[0] == '1'; }
        const char *handoff = getenv("GOW3_LOADEXEC_FILE");
        if (!in_process && handoff && *handoff) {
            if (lobby_role == 1 && now - lobby_since < 1500) break;
            if (lobby_role == 1) {   /* the net game picked in the lobby (Map / Game Type), for runtime_net */
                int16_t game = -1;
                rd((uint64_t)(uintptr_t)image + U2_NGM + 0x1ca0, &game, 2);
                char path[1024];
                snprintf(path, sizeof(path), "%s", handoff);
                char *slash = strrchr(path, '\\'), *slash2 = strrchr(path, '/');
                if (slash2 > slash) slash = slash2;
                if (game >= 0 && slash && (size_t)(slash + 1 - path) + 20 < sizeof(path)) {
                    strcpy(slash + 1, "net-game-next.txt");
                    FILE *g = fopen(path, "w");
                    if (g) { fprintf(g, "%d\n", game); fclose(g); printf("CoopMenu: lobby: net game %d for the match\n", game); }
                }
            }
            FILE *f = fopen(handoff, "w");
            if (f) {
                fputs("/app0/big2-ps4_Shipping.elf\neboot.bin\nispartofuccollection\ntask\nonline\n", f);
                fclose(f);
                puts("CoopMenu: lobby: restarting into the match (task online)");
                fflush(NULL);
                _exit(30);
            }
        }
        const int started = start_online_task();
        printf("CoopMenu: lobby: online task %s\n", started ? "started" : "NOT found");
        print_state("lobby start");
        fflush(stdout);
        lobby_state = started ? 3 : 1;
        lobby_since = now;
        played = 0;
        break;
    }
    case 3:     /* the match; after it (Leave -> menus) the lobby again */
        if (mode >= 0x1a && mode <= 0x1e) { played = 1; lobby_since = now; }
        else if (played && hmode == 0 && top && now - lobby_since > 5000) {
            printf("CoopMenu: lobby: the match is over (net mode %d): back to the lobby\n", mode);
            fflush(stdout);
            lobby_state = 0;
            lobby_since = 0;
        }
        break;
    }
}

static void ABI menu_frame(void) {
    while (queue_tail != queue_head) {
        const MenuCall c = queue[queue_tail % 16];
        queue_tail = queue_tail + 1;
        const uint64_t host = (uint64_t)(uintptr_t)image + U2_MENU_HOST;
        uint64_t r = 0;
        switch (c.op) {
        case CALL_SHOW: r = ((Fn2)(image + 0x5ccf80))(host, c.page); break;
        case CALL_GOTO: r = ((Fn3)(image + 0x5cb990))(host, c.page, 0); break;
        case CALL_BACK: r = ((Fn1)(image + 0x5cc6f0))(host) & 0xff; break;
        case CALL_GUEST: r = ((Fn4)(image + c.fn))(c.a[0], c.a[1], c.a[2], c.a[3]); break;
        case CALL_ENDGAME: {
            /* SendEndGame 0x60c7f0(reason, &time), time = net clock [0x113a040] + 0xc (the
             * Net debug menu's "End Game" items 0x64e97c / 0x64e9fc do the same). */
            uint64_t clock = 0;
            static uint32_t when;
            rd((uint64_t)(uintptr_t)image + 0x113a040, &clock, 8);
            if (!clock || !rd(clock + 0xc, &when, 4)) { puts("CoopMenu: endgame: no net clock (not a net game)"); break; }
            r = ((Fn2)(image + 0x60c7f0))(c.page, (uint64_t)(uintptr_t)&when);
            break;
        }
        default: break;
        }
        char what[64];
        snprintf(what, sizeof(what), "%s %08x -> %#llx", c.op == CALL_SHOW ? "show" : c.op == CALL_GOTO ? "goto" : c.op == CALL_BACK ? "back" :
                 c.op == CALL_GUEST ? "call" : c.op == CALL_ENDGAME ? "endgame" : "state",
                 c.page, (unsigned long long)r);
        print_state(what);
        fflush(stdout);
    }
    auto_tick();
    lobby_tick();
}

static int install_frame_hook(void) {
    static int installed, told;
    if (installed) return installed > 0;
    /* 25 Oct (run lb01): at frame 1 on the title screen the hook could not be placed on the host;
     * a failure is retried (runtime_coop_menu_tick) instead of being final. */
    if (!is_u2()) {
        if (!told++) printf("CoopMenu: in-frame hook site %#x does not match (yet): commands wait\n", U2_FRAME_SITE);
        return 0;
    }
    unsigned char *stub = stub_page();
    if (!stub) { if (!told++) puts("CoopMenu: no stub page near the image (yet): commands wait"); return 0; }
    const unsigned length = 6;   /* rdtsc (2) + shl rdx, 0x20 (4) */
    build_mid_stub(stub, (void *)menu_frame, image + U2_FRAME_SITE, length, (uintptr_t)(image + U2_FRAME_SITE + length));
    const int64_t delta = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + U2_FRAME_SITE + 5);
    if (delta < INT32_MIN || delta > INT32_MAX) return 0;
    unsigned char patch[6] = {0xe9, 0, 0, 0, 0, 0x90};
    const int32_t rel = (int32_t)delta;
    memcpy(patch + 1, &rel, 4);
    if (!wr((uint64_t)(uintptr_t)(image + U2_FRAME_SITE), patch, sizeof(patch))) return 0;
    installed = 1;
    printf("CoopMenu: in-frame hook at %#x (menu calls run there)\n", U2_FRAME_SITE);
    return 1;
}

static const struct { const char *alias, *name; } pages[] = {
    {"lobby", "PAGE_LOBBY"},
    {"scoreboard", "PAGE_MULTIPLAYER_SCOREBOARD"},
    {"results", "PAGE_MULTIPLAYER_RESULTS"},
    {"roundresults", "PAGE_MULTIPLAYER_ROUND_RESULTS"},
    {"postgame", "PAGE_MULTIPLAYER_POSTGAME"},
    {"loading", "PAGE_MULTIPLAYER_LOADING"},
    {"multiplayer", "PAGE_MULTIPLAYER"},
    {"pause", "PAGE_PAUSE_MULTIPLAYER"},
    {"matchmaking", "PAGE_MULTIPLAYER_MATCHMAKING"},
    {"leaving", "PAGE_MULTIPLAYER_LEAVING_GAME"},
    {"lastresults", "PAGE_MULTIPLAYER_LASTGAMERESULTS"},
};
static uint32_t page_id(const char *p) {
    char word[64];
    int k = 0;
    while (*p == ' ' || *p == '\t') ++p;
    while (*p && !isspace((unsigned char)*p) && k < 63) word[k++] = *p++;
    word[k] = 0;
    if (!k) return 0;
    for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); ++i)
        if (!strcmp(word, pages[i].alias)) return sid(pages[i].name);
    if (!strncmp(word, "PAGE_", 5) || !strncmp(word, "BP_PAGE_", 8)) return sid(word);
    return (uint32_t)strtoul(word, NULL, 16);
}

static void push(int op, uint32_t page) {
    if (queue_head - queue_tail >= 16) { puts("CoopMenu: queue full"); return; }
    queue[queue_head % 16].op = op;
    queue[queue_head % 16].page = page;
    queue_head = queue_head + 1;
}

extern int runtime_coop_u3_command(unsigned char *base, const char *op, const char *args);

static char pending[8][160];
static int pending_count;
int runtime_coop_menu_command(unsigned char *base, const char *op, const char *args);
/* Every frame (runtime_coop.c coop_frame): mpmenu commands given before the in-frame hook could
 * be placed run once it is in. */
void runtime_coop_menu_tick(unsigned char *base) {
    if (!pending_count) return;
    image = base;
    if (!install_frame_hook()) return;
    const int n = pending_count;
    pending_count = 0;
    for (int i = 0; i < n; ++i) {
        printf("CoopMenu: in-frame hook in: running the waiting 'mpmenu %s'\n", pending[i]);
        runtime_coop_menu_command(base, "mpmenu", pending[i]);
    }
    fflush(stdout);
}

int runtime_coop_menu_command(unsigned char *base, const char *op, const char *args) {
    image = base;
    if (!strcmp(op, "u3")) return runtime_coop_u3_command(base, op, args);
    if (strcmp(op, "mpmenu")) return 0;
    while (*args == ' ' || *args == '\t') ++args;
    char sub[16] = {0};
    int k = 0;
    while (*args && !isspace((unsigned char)*args) && k < 15) sub[k++] = *args++;
    if (!install_frame_hook()) {
        /* kept and replayed once the hook is in (runtime_coop_menu_tick) */
        if (pending_count < 8) {
            snprintf(pending[pending_count++], sizeof(pending[0]), "%s%s", sub, args);
            printf("CoopMenu: no in-frame hook yet: '%s' waits\n", sub);
        }
        fflush(stdout);
        return 1;
    }
    if (!strcmp(sub, "show") || !strcmp(sub, "goto")) {
        const uint32_t id = page_id(args);
        if (!id) { puts("CoopMenu: which page?"); return 1; }
        push(!strcmp(sub, "show") ? CALL_SHOW : CALL_GOTO, id);
        printf("CoopMenu: queued %s %08x\n", sub, id);
    } else if (!strcmp(sub, "back")) {
        push(CALL_BACK, 0);
    } else if (!strcmp(sub, "state")) {
        push(CALL_STATE, 0);
    } else if (!strcmp(sub, "endgame")) {
        const char *q = args;
        while (*q == ' ') ++q;
        push(CALL_ENDGAME, *q ? (uint32_t)strtoul(q, NULL, 0) : 4);
    } else if (!strcmp(sub, "call")) {
        /* mpmenu call <image offset> [args]: a guest call inside the frame (hex numbers;
         * i:<hex> = an image offset as an argument) */
        char *end = NULL;
        const uint64_t fn = strtoull(args, &end, 16);
        uint64_t a[4] = {0};
        const char *q = end;
        for (int i = 0; i < 4 && q && *q; ++i) {
            while (*q == ' ') ++q;
            if (!*q) break;
            const int img = !strncmp(q, "i:", 2);
            if (img) q += 2;
            a[i] = strtoull(q, &end, 16) + (img ? (uint64_t)(uintptr_t)image : 0);
            q = end;
        }
        if (!fn || queue_head - queue_tail >= 16) { puts("CoopMenu: call: bad address or queue full"); return 1; }
        MenuCall *c = &queue[queue_head % 16];
        c->op = CALL_GUEST;
        c->page = 0;
        c->fn = fn;
        memcpy(c->a, a, sizeof(a));
        queue_head = queue_head + 1;
    } else if (!strcmp(sub, "lobby")) {
        /* mpmenu lobby [host|join]: the Custom Game lobby before every match (default: the
         * runtime_net role) */
        while (*args == ' ') ++args;
        lobby_role = !strncmp(args, "join", 4) ? 2 : !strncmp(args, "host", 4) ? 1 : runtime_net_hosting() ? 1 : 2;
        lobby_state = 0;
        lobby_since = 0;
        {   /* started into the match (the lobby's restart: "task online", name at 0x147ea20): the
             * lobby comes after this round */
            char task[16] = {0};
            if (rd((uint64_t)(uintptr_t)image + 0x147ea20, task, sizeof(task) - 1) && !strcmp(task, "online")) {
                lobby_state = 3;
                lobby_since = GetTickCount64();
                puts("CoopMenu: lobby: this game started into the match; the lobby opens after the round");
            }
        }
        printf("CoopMenu: lobby on (%s): the game opens the Custom Game lobby; the host's Start Game starts every game\n",
               lobby_role == 1 ? "host" : "joiner");
    } else if (!strcmp(sub, "auto")) {
        auto_on = 1;
        printf("CoopMenu: auto results screens on (ShowResults hook %s)\n", install_results_hook() ? "at 0x5f0ea0" : "NOT installed");
    } else {
        printf("CoopMenu: unknown mpmenu command '%s' (show, goto, back, state, endgame, call, auto)\n", sub);
    }
    fflush(stdout);
    return 1;
}
#else
int runtime_coop_menu_command(unsigned char *base, const char *op, const char *args) { (void)base; (void)op; (void)args; return 0; }
void runtime_coop_menu_tick(unsigned char *base) { (void)base; }
#endif
