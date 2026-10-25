/* gow3 25 Oct 2026: Uncharted 3 multiplayer / split-screen feasibility (research, console only).
 *
 * runtime_coop.c's console runs in Uncharted 3 too (frame hook at the U3 1.02 GameLoopUpdate,
 * 0x56e650); its generic commands (r, w, call, mon, dump, wait) work there. The U2 helpers do
 * not apply. Commands of this file start with "u3" (routed by runtime_coop_menu.c):
 *   u3 state     the net flags: net game object 0x17d5c58 (+9 = in a net game, the test of
 *                ~950 sites, e.g. net-split-screen? 0x79e970), splitscreen arg byte 0x17d7b06,
 *                session kind [0x17f2f98+0x28] (3 = split screen), GameConfig 0x17b6368
 *                (+0x5308 match, +0x5309 net boot, +0x5330 netgame sid, +0x5334 nettask sid,
 *                +0x5338 waitfriend)
 * Notes: analysis/triage-1022/coop-u3.md. Addresses are U3 1.02 image offsets.
 */
#include "runtime.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

#ifdef _WIN32
#define U3_NET 0x17d5c58u
#define U3_SPLIT 0x17d7b06u
#define U3_SESSION 0x17f2f98u
#define U3_CONFIG 0x17b6368u
#define U3_GAMELOOP 0x56e650u

static int rd(uint64_t address, void *out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)address, out, n, &got) && got == n;
}

/* The GameLoopUpdate prologue after the 6 bytes the console's frame hook replaces (push r14 ...
 * sub rsp, 0x68): run u3a found the first 6 bytes already patched. */
static int is_u3(const unsigned char *image) {
    static const unsigned char rest[11] = {0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x68};
    return image && !memcmp(image + U3_GAMELOOP + 6, rest, sizeof(rest));
}

int runtime_coop_u3_command(unsigned char *image, const char *op, const char *args) {
    (void)op;
    if (!is_u3(image)) { puts("CoopU3: not the Uncharted 3 1.02 executable"); fflush(stdout); return 1; }
    while (*args == ' ' || *args == '\t') ++args;
    const uint64_t I = (uint64_t)(uintptr_t)image;
    if (!strncmp(args, "state", 5)) {
        uint8_t net = 0, split = 0, match = 0, boot = 0;
        int32_t kind = -1;
        uint32_t game = 0, task = 0, wait = 0;
        rd(I + U3_NET + 9, &net, 1);
        rd(I + U3_SPLIT, &split, 1);
        rd(I + U3_SESSION + 0x28, &kind, 4);
        rd(I + U3_CONFIG + 0x5308, &match, 1);
        rd(I + U3_CONFIG + 0x5309, &boot, 1);
        rd(I + U3_CONFIG + 0x5330, &game, 4);
        rd(I + U3_CONFIG + 0x5334, &task, 4);
        rd(I + U3_CONFIG + 0x5338, &wait, 4);
        printf("CoopU3: net %u, splitscreen %u, session kind %d, match %u, net boot %u, netgame %08x, nettask %08x, waitfriend %u\n",
               net, split, kind, match, boot, game, task, wait);
    } else {
        printf("CoopU3: unknown u3 command '%s' (state)\n", args);
    }
    fflush(stdout);
    return 1;
}
#else
int runtime_coop_u3_command(unsigned char *image, const char *op, const char *args) { (void)image; (void)op; (void)args; return 0; }
#endif
