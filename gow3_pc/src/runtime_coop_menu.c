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
    return image && !memcmp(image + U2_FRAME_SITE - 10, u2_frame_bytes, sizeof(u2_frame_bytes));
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
}

static int install_frame_hook(void) {
    static int installed;
    if (installed) return installed > 0;
    installed = -1;
    if (!is_u2()) { puts("CoopMenu: not the Uncharted 2 1.02 executable"); return 0; }
    unsigned char *stub = stub_page();
    if (!stub) return 0;
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

int runtime_coop_menu_command(unsigned char *base, const char *op, const char *args) {
    image = base;
    if (!strcmp(op, "u3")) return runtime_coop_u3_command(base, op, args);
    if (strcmp(op, "mpmenu")) return 0;
    while (*args == ' ' || *args == '\t') ++args;
    char sub[16] = {0};
    int k = 0;
    while (*args && !isspace((unsigned char)*args) && k < 15) sub[k++] = *args++;
    if (!install_frame_hook()) { puts("CoopMenu: no in-frame hook, command ignored"); fflush(stdout); return 1; }
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
#endif
