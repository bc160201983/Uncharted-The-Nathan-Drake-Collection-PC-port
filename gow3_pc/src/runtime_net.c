/* gow3 25 Oct 2026: LAN transport for Uncharted 2 net games (co-op / multiplayer research).
 *
 * Off unless GOW3_NET_HOST or GOW3_NET_JOIN is set (the launcher sets neither), and only driven
 * from runtime_coop.c's per-frame hook (coop_frame calls runtime_net_frame).
 *
 * What survives in the U2 1.02 executable (analysis/triage-1022/coop-net-stream.md): the per-
 * connection packet protocol. 32 connection records of 0x220 bytes at image offset 0x1152a10:
 *   +0x08 u32 connection id (compared with a net game slot's +0x24c)
 *   +0x0c u8 in use, +0x0e u8 relayed (SendEvent queues to a record directly only when this
 *         byte is 0, else it relays through the host), +0x3c u32 signaling connection id, +0x40 refcount
 *   +0x48 the protocol object (vtable 0x103fc80, constructor 0x609ec0, reset 0x60ba10):
 *         [0] 0x609e60 new packet record, [1] 0x60a110 ReadPacket(obj, bitstream),
 *         [2] 0x60ac70 WritePacket(obj, bitstream, record), [3] 0x60b780 PacketDelivered(obj,
 *         record), [4] 0x60b8f0 PacketDropped(obj, record); queues at +0x110 (reliable) and
 *         +0x120 (unreliable), filled by NetGameManager::SendEvent 0x644ab0.
 * The removed layer owned the sockets and the packet acks ("notify" protocol: every packet
 * written is later reported delivered or dropped). Over TCP every packet arrives, so each packet
 * is reported delivered right after it is written.
 *
 * Star topology: the host takes up to NET_MAX_PEERS joiners, each joiner talks to the host only.
 * Events between two joiners go through the host: SendEvent puts players without a direct record
 * on the event's relay list, and the host's ReadPacket forwards them (event vtable +0x28).
 *
 * GOW3_NET_HOST=[addr:]port   listen (default address 0.0.0.0) and take the joiners
 * GOW3_NET_JOIN=addr:port     connect to a host (retried every second)
 * GOW3_NET_ID=<n>|auto        our player id (default 1 when hosting, 2 when joining). "auto": the
 *                             host gives the joiner a free id in its welcome; a joiner asking for
 *                             a taken id also gets a free one (runtime_net_local_id follows it)
 * GOW3_NET_NAME=<text>        our player name (default "Drake" / "Sully")
 * GOW3_NET_SKIN=<actor>       our character (an MP skin actor such as mp-hero-elena, or
 *                             hero-default / hero-parka): sent in the hello and the player list;
 *                             every machine gives our slot that model (runtime_coop_mp.c)
 * GOW3_NET_GAME=<index>       host: the net game (*net-games* index) told to joiners and to the
 *                             launcher's LAN search (default: the net game manager's +0x1ca0)
 * GOW3_NET_TITLE=<text>       host: the game's title in the LAN search (e.g. "Village - Co-op")
 * A remote player's connection record is record <its player id> (its +8 = that id, which the
 * session side also uses as the slot's connection id).
 *
 * LAN search: the host also answers UDP queries on its port number. Query: "U2NQ" + u32
 * version. Answer: "U2NI", u32 version, u32 TCP port, u32 players, u32 most players, i32 net
 * game, char host name[32], char title[48] (launcher/net_lan.py sends the query as a broadcast).
 *
 * Wire format (TCP, little-endian): u32 kind, u32 bits, then (bits + 31) / 32 u32 words.
 *   kind 1 = hello {u32 version, u32 player id, char name[32], char skin[32]} (bits = 576;
 *            version 3 had no skin: bits = 320)
 *   kind 2 = one packet from WritePacket (the bitstream's words as the game wrote them)
 *   kind 3 = the host's net time {u32 ticks of 1/300 s: the net clock at [0x113a040] + 0xc},
 *            sent by the host every frame; a joiner's clock follows it (runtime_net_host_clock)
 *   kind 4 = the host's player list {u32 count, count x {u32 id, char name[32], char skin[32]}} (host
 *            included), sent to every joiner when someone joins or leaves (runtime_net_roster)
 *   kind 5 = the host's welcome to a joiner, after its hello {u32 the joiner's player id,
 *            i32 net game, u32 most players, char title[48]} (runtime_net_ready / _game)
 */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif
#include "runtime.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define NET_RECORDS      0x1152a10
#define NET_RECORD_SIZE  0x220
#define NET_VTABLE       0x103fc80
#define NET_CTOR         0x609ec0
#define NET_RESET        0x60ba10
#define NET_READ         0x60a110
#define NET_WRITE        0x60ac70
#define NET_DELIVERED    0x60b780
#define NET_KIND_HELLO   1
#define NET_KIND_PACKET  2
#define NET_KIND_TIME    3
#define NET_KIND_ROSTER  4
#define NET_KIND_WELCOME 5
#define NET_KIND_START   6           /* host -> joiners: the lobby's Start Game (runtime_coop_menu.c) */
#define NET_CLOCK        0x113a040   /* -> the net clock struct, +0xc = ticks (1/300 s) */
#define NET_GAME_INDEX   (0x113b760 + 0x1ca0)  /* the net game manager's net game (int16) */
#define NET_GAME_END     (0x113b760 + 0x1e84)  /* its end state (SetGameEnd 0x64a7c0): 0 while playing */
#define NET_HEAP_SLOT    (0x11c6630 + 0x14 * 8) /* allocator of memory context 0x14 (Network) */
#define NET_VERSION      6           /* 4 (25 Oct): the skin in the hello and the player list;
                                        5: snapshot anim leaves carry a phase (runtime_coop_mp.c);
                                        6 (np- agent): NPC events NpcSpawn/NpcDespawn/0x2c carry data */
#define NET_MAX_BYTES    (64 << 10)
#define NET_MAX_PEERS    9           /* 10 player slots: the host and 9 joiners */
#define NET_MAX_PLAYERS  10

typedef struct { uint32_t *data; uint32_t pos, len; uint64_t spare[2]; } NetBits;
typedef struct { uint32_t seq, pad; uint64_t next, nodes; } NetPacketRecord;
typedef void (ABI *NetObjFn)(uint64_t obj);
typedef void (ABI *NetReadFn)(uint64_t obj, NetBits *bits);
typedef void (ABI *NetWriteFn)(uint64_t obj, NetBits *bits, NetPacketRecord *record);
typedef void (ABI *NetNotifyFn)(uint64_t obj, NetPacketRecord *record);

typedef struct NetFrame { struct NetFrame *next; ULONGLONG at; uint32_t kind, bits; uint32_t words[]; } NetFrame;

/* One connection: the joiner's link to the host, or one of the host's joiners. */
typedef struct {
    /* guarded by lock */
    int used;                        /* slot taken (by the threads until closed, then the game thread) */
    int up;                          /* socket live */
    int closed;                      /* the threads have ended: the game thread frees the slot */
    SOCKET sock;
    NetFrame *inbox, **inbox_tail;
    unsigned char *out;
    size_t out_len, out_cap;
    /* game thread only */
    int id;                          /* the player id from its hello; 0 until then */
    char name[32];
    char skin[32];                   /* its character (actor), "" = the default */
    int bound;                       /* its connection record (record <id>) is set up */
    uint64_t sent_packets, sent_bits, got_packets, got_bits, dropped_packets;
} NetPeer;

typedef struct { uint32_t id; char name[32]; char skin[32]; } NetPlayer;
#define NET_PLAYER_WORDS ((uint32_t)(sizeof(NetPlayer) / 4))

static unsigned char *image;
static int started, hosting;
static char address[128];
static int local_id;
static char local_name[32];
static char local_skin[32];
static CRITICAL_SECTION lock;
static NetPeer peers[NET_MAX_PEERS];
static int roster_dirty;                       /* host: send the player list (game thread) */
static NetPlayer roster[NET_MAX_PLAYERS];       /* joiner: the host's last player list */
static int roster_count;
static uint32_t host_ticks;                    /* joiner: the host's last net time */
static ULONGLONG host_ticks_at;                /* and when it arrived (GetTickCount64), 0: none */
static int welcomed;                           /* joiner: the host's welcome arrived */
static int start_got;                          /* joiner: the host's Start Game arrived (lobby) */
static int net_game = -1;                      /* the net game: host's own, or from the welcome */
static char title[48];                         /* the game's title (GOW3_NET_TITLE / the welcome) */
static volatile LONG public_players = 1;       /* for the LAN search: us + joiners with a hello */

static void out_append(NetPeer *p, const void *data, size_t n) {
    if (p->out_len + n > p->out_cap) {
        size_t cap = p->out_cap ? p->out_cap : 65536;
        while (cap < p->out_len + n) cap *= 2;
        unsigned char *b = realloc(p->out, cap);
        if (!b) return;
        p->out = b;
        p->out_cap = cap;
    }
    memcpy(p->out + p->out_len, data, n);
    p->out_len += n;
}
static void queue_frame(NetPeer *p, uint32_t kind, uint32_t bits, const void *words) {
    const uint32_t head[2] = {kind, bits};
    EnterCriticalSection(&lock);
    if (p->up) {
        out_append(p, head, sizeof(head));
        out_append(p, words, (bits + 31) / 32 * 4);
    }
    LeaveCriticalSection(&lock);
}
static void send_hello(NetPeer *p) {
    uint32_t hello[18] = {NET_VERSION, (uint32_t)local_id};
    memcpy(&hello[2], local_name, sizeof(local_name));
    memcpy(&hello[10], local_skin, sizeof(local_skin));
    queue_frame(p, NET_KIND_HELLO, sizeof(hello) * 8, hello);
}

static int recv_all(SOCKET s, void *p, int n) {
    char *c = p;
    while (n > 0) {
        const int got = recv(s, c, n, 0);
        if (got <= 0) return 0;
        c += got;
        n -= got;
    }
    return 1;
}
/* Reads one connection's frames into its inbox until the connection drops. */
static DWORD WINAPI receive_thread(LPVOID arg) {
    NetPeer *p = arg;
    const SOCKET s = p->sock;
    for (;;) {
        uint32_t head[2];
        if (!recv_all(s, head, sizeof(head))) break;
        if (head[1] > NET_MAX_BYTES * 8) { printf("Net: bad frame (%u bits), closing\n", head[1]); break; }
        const uint32_t bytes = (head[1] + 31) / 32 * 4;
        NetFrame *f = calloc(1, sizeof(NetFrame) + bytes);
        if (!f || !recv_all(s, f->words, (int)bytes)) { free(f); break; }
        f->kind = head[0];
        f->bits = head[1];
        f->at = GetTickCount64();
        EnterCriticalSection(&lock);
        *p->inbox_tail = f;
        p->inbox_tail = &f->next;
        LeaveCriticalSection(&lock);
    }
    EnterCriticalSection(&lock);
    p->up = 0;
    LeaveCriticalSection(&lock);
    return 0;
}

/* Runs one connection: hello, a receive thread, then the outbox to the socket until it drops. */
static void serve(NetPeer *p) {
    const BOOL yes = TRUE;
    setsockopt(p->sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&yes, sizeof(yes));
    send_hello(p);
    HANDLE rt = CreateThread(NULL, 0, receive_thread, p, 0, NULL);
    unsigned char *chunk = malloc(NET_MAX_BYTES * 2);
    for (;;) {
        size_t n = 0;
        int up;
        EnterCriticalSection(&lock);
        up = p->up;
        if (p->out_len && chunk) {
            n = p->out_len < NET_MAX_BYTES * 2 ? p->out_len : NET_MAX_BYTES * 2;
            memcpy(chunk, p->out, n);
            memmove(p->out, p->out + n, p->out_len - n);
            p->out_len -= n;
        }
        LeaveCriticalSection(&lock);
        if (!up) break;
        if (!n) { Sleep(1); continue; }
        size_t done = 0;
        while (done < n) {
            const int put = send(p->sock, (const char *)chunk + done, (int)(n - done), 0);
            if (put <= 0) break;
            done += (size_t)put;
        }
        if (done < n) break;
    }
    free(chunk);
    EnterCriticalSection(&lock);
    p->up = 0;
    LeaveCriticalSection(&lock);
    shutdown(p->sock, SD_BOTH);
    if (rt) { WaitForSingleObject(rt, 5000); CloseHandle(rt); }
    closesocket(p->sock);
    EnterCriticalSection(&lock);
    p->closed = 1;
    LeaveCriticalSection(&lock);
}
/* Takes a free slot for a new connection (NULL when all are taken). */
static NetPeer *claim(SOCKET s) {
    NetPeer *p = NULL;
    EnterCriticalSection(&lock);
    for (int i = 0; i < NET_MAX_PEERS && !p; ++i) {
        if (peers[i].used) continue;
        p = &peers[i];
        p->used = 1;
        p->up = 1;
        p->closed = 0;
        p->sock = s;
        p->inbox = NULL;
        p->inbox_tail = &p->inbox;
        p->out_len = 0;
    }
    LeaveCriticalSection(&lock);
    return p;
}
static DWORD WINAPI serve_thread(LPVOID arg) {
    serve(arg);
    return 0;
}

static int parse_addr(const char *text, const char *default_host, struct sockaddr_in *out) {
    char host[128];
    const char *colon = strrchr(text, ':');
    const char *port = colon ? colon + 1 : text;
    if (colon) snprintf(host, sizeof(host), "%.*s", (int)(colon - text), text);
    else snprintf(host, sizeof(host), "%s", default_host);
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons((u_short)atoi(port));
    if (inet_pton(AF_INET, host, &out->sin_addr) == 1) return 1;
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    if (getaddrinfo(host, NULL, &hints, &res) || !res) return 0;
    out->sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return 1;
}

/* Host: answers the launcher's LAN search (UDP on the TCP port's number; see the top). */
static DWORD WINAPI lan_thread(LPVOID arg) {
    const struct sockaddr_in *where = arg;
    const SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET || bind(s, (const struct sockaddr *)where, sizeof(*where))) {
        printf("Net: no LAN search answers (UDP error %d)\n", WSAGetLastError());
        fflush(stdout);
        return 0;
    }
    for (;;) {
        char query[16];
        struct sockaddr_in from;
        int from_len = sizeof(from);
        const int got = recvfrom(s, query, sizeof(query), 0, (struct sockaddr *)&from, &from_len);
        if (got == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAECONNRESET) continue;  /* an earlier answer was refused */
            Sleep(100);
            continue;
        }
        if (got < 8 || memcmp(query, "U2NQ", 4)) continue;
        struct {
            char magic[4];
            uint32_t version, port, players, most;
            int32_t game;
            char host[32], title[48];
        } answer;
        memset(&answer, 0, sizeof(answer));
        memcpy(answer.magic, "U2NI", 4);
        answer.version = NET_VERSION;
        answer.port = ntohs(where->sin_port);
        answer.players = (uint32_t)public_players;
        answer.most = NET_MAX_PLAYERS;
        answer.game = net_game;
        memcpy(answer.host, local_name, sizeof(answer.host));
        memcpy(answer.title, title, sizeof(answer.title));
        sendto(s, (const char *)&answer, sizeof(answer), 0, (struct sockaddr *)&from, from_len);
    }
}

/* Host: accepts joiners, one serving thread each. Joiner: (re)connects to the host. */
static DWORD WINAPI link_thread(LPVOID arg) {
    (void)arg;
    struct sockaddr_in addr;
    if (!parse_addr(address, hosting ? "0.0.0.0" : "127.0.0.1", &addr)) {
        printf("Net: bad address %s\n", address);
        fflush(stdout);
        return 0;
    }
    if (hosting) {
        const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        const BOOL yes = TRUE;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
        if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) || listen(listener, NET_MAX_PEERS)) {
            printf("Net: cannot listen on %s (error %d)\n", address, WSAGetLastError());
            fflush(stdout);
            return 0;
        }
        printf("Net: hosting on %s (up to %d joiners)\n", address, NET_MAX_PEERS);
        fflush(stdout);
        static struct sockaddr_in lan_addr;
        lan_addr = addr;
        const HANDLE lan = CreateThread(NULL, 0, lan_thread, &lan_addr, 0, NULL);
        if (lan) CloseHandle(lan);
        for (;;) {
            const SOCKET s = accept(listener, NULL, NULL);
            if (s == INVALID_SOCKET) { Sleep(1000); continue; }
            NetPeer *p = claim(s);
            if (!p) {
                puts("Net: a player tried to join, but the game is full");
                fflush(stdout);
                closesocket(s);
                continue;
            }
            printf("Net: a joiner connected (connection %d)\n", (int)(p - peers));
            fflush(stdout);
            const HANDLE t = CreateThread(NULL, 0, serve_thread, p, 0, NULL);
            if (t) CloseHandle(t);
            else {
                closesocket(s);
                EnterCriticalSection(&lock);
                p->up = 0;
                p->closed = 1;
                LeaveCriticalSection(&lock);
            }
        }
    }
    for (;;) {
        int busy;
        EnterCriticalSection(&lock);
        busy = peers[0].used;  /* the game thread frees it after a disconnect */
        LeaveCriticalSection(&lock);
        if (busy) { Sleep(100); continue; }
        const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (connect(s, (struct sockaddr *)&addr, sizeof(addr))) {
            closesocket(s);
            Sleep(1000);
            continue;
        }
        NetPeer *p = claim(s);
        if (!p) { closesocket(s); Sleep(1000); continue; }
        printf("Net: connected to the host (%s)\n", address);
        fflush(stdout);
        serve(p);
        printf("Net: connection to the host closed\n");
        fflush(stdout);
        Sleep(1000);
    }
}

static void start(void) {
    const char *host = getenv("GOW3_NET_HOST"), *join = getenv("GOW3_NET_JOIN");
    if (host && *host) { hosting = 1; snprintf(address, sizeof(address), "%s", host); }
    else if (join && *join) snprintf(address, sizeof(address), "%s", join);
    else return;
    const char *id = getenv("GOW3_NET_ID"), *name = getenv("GOW3_NET_NAME");
    if (id && !strcmp(id, "auto")) local_id = hosting ? 1 : 0;  /* the host's welcome gives ours */
    else local_id = id && *id ? atoi(id) : hosting ? 1 : 2;
    snprintf(local_name, sizeof(local_name), "%s", name && *name ? name : hosting ? "Drake" : "Sully");
    const char *skin = getenv("GOW3_NET_SKIN");
    snprintf(local_skin, sizeof(local_skin), "%s", skin ? skin : "");
    if (hosting) {
        const char *game = getenv("GOW3_NET_GAME"), *text = getenv("GOW3_NET_TITLE");
        if (game && *game) net_game = atoi(game);
        snprintf(title, sizeof(title), "%s", text ? text : "");
        /* The lobby's restart into the match (runtime_coop_menu.c) leaves the net game picked in
         * the lobby next to the launcher's handoff file: it wins over GOW3_NET_GAME, once. */
        const char *handoff = getenv("GOW3_LOADEXEC_FILE");
        if (handoff && *handoff) {
            char path[1024];
            snprintf(path, sizeof(path), "%s", handoff);
            char *slash = strrchr(path, '\\'), *slash2 = strrchr(path, '/');
            if (slash2 > slash) slash = slash2;
            if (slash && (size_t)(slash + 1 - path) + 20 < sizeof(path)) {
                strcpy(slash + 1, "net-game-next.txt");
                FILE *f = fopen(path, "r");
                int next = -1;
                if (f) { if (fscanf(f, "%d", &next) != 1) next = -1; fclose(f); remove(path); }
                if (next >= 0) {
                    printf("Net: the lobby picked net game %d (was %d)\n", next, net_game);
                    if (next != net_game) snprintf(title, sizeof(title), "Net game %d", next);
                    net_game = next;
                }
            }
        }
    }
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) { puts("Net: WSAStartup failed"); return; }
    InitializeCriticalSection(&lock);
    for (int i = 0; i < NET_MAX_PEERS; ++i) peers[i].inbox_tail = &peers[i].inbox;
    const HANDLE t = CreateThread(NULL, 0, link_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    started = 1;
    printf("Net: player id %d (%s%s%s), %s %s\n", local_id, local_name, local_skin[0] ? ", " : "", local_skin,
           hosting ? "host on" : "joining", address);
    fflush(stdout);
}

static uint64_t record_at(int index) {
    return (uint64_t)(uintptr_t)image + NET_RECORDS + (uint64_t)index * NET_RECORD_SIZE;
}
/* Gives a remote player a connection record: record <id>, +8 = id, in use and direct. */
static void bind_record(NetPeer *p) {
    const uint64_t r = record_at(p->id), obj = r + 0x48;
    if (*(uint64_t *)(uintptr_t)obj != (uint64_t)(uintptr_t)image + NET_VTABLE) {
        ((NetObjFn)(image + NET_CTOR))(obj);
        printf("Net: record %d: protocol object constructed\n", p->id);
    }
    *(uint32_t *)(uintptr_t)(r + 8) = (uint32_t)p->id;
    *(uint8_t *)(uintptr_t)(r + 0xc) = 1;
    /* +0xe = "relayed", not "direct": SendEvent 0x644ab0 (first loop, 0x644b98) puts a slot
     * whose record has +0xe == 0 into the direct mask and queues to it; +0xe != 0 (or no record)
     * puts it on the relay list, which only goes out when the target is the host's slot. With 1
     * here the host never sent anything (coop agent, 25 Oct 01:05; coop-mp.md). */
    *(uint8_t *)(uintptr_t)(r + 0xe) = 0;
    *(uint32_t *)(uintptr_t)(r + 0x3c) = (uint32_t)p->id;
    *(uint32_t *)(uintptr_t)(r + 0x40) = 1;
    p->bound = 1;
    printf("Net: player %d (%s) bound to connection record %d at %llx\n", p->id, p->name, p->id,
           (unsigned long long)r);
}
/* A remote player left: its record's queues are emptied (nothing would drain them) and the
 * record is no longer direct, as the game's own teardown (0x5e9dd0) does. */
static void unbind_record(NetPeer *p) {
    const uint64_t r = record_at(p->id);
    ((NetObjFn)(image + NET_RESET))(r + 0x48);
    *(uint8_t *)(uintptr_t)(r + 0xc) = 0;
    *(uint8_t *)(uintptr_t)(r + 0xe) = 1;   /* relayed: SendEvent no longer queues to it directly */
    *(uint32_t *)(uintptr_t)(r + 0x40) = 0;
    p->bound = 0;
}
static int id_taken(const NetPeer *self, int id) {
    if (id == local_id) return 1;
    for (int i = 0; i < NET_MAX_PEERS; ++i)
        if (&peers[i] != self && peers[i].id == id) return 1;
    return 0;
}
static void hello(NetPeer *p, const NetFrame *f) {
    const int id = (int)f->words[1];
    char name[32], skin[32] = {0};
    memcpy(name, &f->words[2], sizeof(name));
    name[sizeof(name) - 1] = 0;
    if (f->bits >= 18 * 32) {
        memcpy(skin, &f->words[10], sizeof(skin));
        skin[sizeof(skin) - 1] = 0;
    }
    printf("Net: hello from player %d (%s), version %u%s%s\n", id, name, f->words[0], skin[0] ? ", character " : "", skin);
    if (f->words[0] != NET_VERSION)
        printf("Net: player %d runs another version of this transport (%u, ours %u)\n", id, f->words[0], NET_VERSION);
    int given = id;
    if (hosting && (id <= 1 || id >= 32 || id_taken(p, id))) {
        /* "auto" (0) or a taken id: the lowest free one (1 is the host) */
        given = 0;
        for (int k = 2; k < 32 && !given; ++k)
            if (!id_taken(p, k)) given = k;
        if (given) printf("Net: player %s gets id %d\n", name, given);
    }
    if (given <= 0 || given >= 32 || id_taken(p, given)) {
        printf("Net: player id %d is out of range or taken: closing that connection\n", id);
        shutdown(p->sock, SD_BOTH);
        return;
    }
    p->id = given;
    memcpy(p->name, name, sizeof(name));
    memcpy(p->skin, skin, sizeof(skin));
    bind_record(p);
    if (hosting) {
        uint32_t welcome[15] = {(uint32_t)given, (uint32_t)net_game, NET_MAX_PLAYERS};
        memcpy(&welcome[3], title, sizeof(title));
        queue_frame(p, NET_KIND_WELCOME, sizeof(welcome) * 8, welcome);
        roster_dirty = 1;
    }
}
/* Joiner: the host's welcome (our id, the net game, its title). */
static void welcome(const NetFrame *f) {
    const int id = (int)f->words[0];
    if (id != local_id) {
        if (local_id) printf("Net: the host gave us player id %d instead of %d\n", id, local_id);
        local_id = id;
    }
    net_game = (int32_t)f->words[1];
    memcpy(title, &f->words[3], sizeof(title));
    title[sizeof(title) - 1] = 0;
    welcomed = 1;
    printf("Net: welcome from the host: player id %d, net game %d%s%s\n", local_id, net_game,
           title[0] ? ", " : "", title);
}
static void send_roster(void) {
    uint32_t words[1 + NET_MAX_PLAYERS * NET_PLAYER_WORDS] = {0};
    uint32_t n = 0;
    NetPlayer *list = (NetPlayer *)&words[1];
    list[n].id = (uint32_t)local_id;
    memcpy(list[n].skin, local_skin, sizeof(local_skin));
    memcpy(list[n++].name, local_name, sizeof(local_name));
    for (int i = 0; i < NET_MAX_PEERS; ++i)
        if (peers[i].bound) {
            list[n].id = (uint32_t)peers[i].id;
            memcpy(list[n].skin, peers[i].skin, sizeof(peers[i].skin));
            memcpy(list[n++].name, peers[i].name, sizeof(peers[i].name));
        }
    words[0] = n;
    for (int i = 0; i < NET_MAX_PEERS; ++i)
        if (peers[i].bound) queue_frame(&peers[i], NET_KIND_ROSTER, (1 + n * NET_PLAYER_WORDS) * 32, words);
}

/* Called once per frame on the game thread (coop_frame, the entry of GameLoopUpdate). */
void runtime_net_frame(unsigned char *base) {
    static int tried;
    if (!tried) { tried = 1; image = base; start(); }
    if (!started) return;
    static __attribute__((aligned(64))) uint32_t words[NET_MAX_BYTES / 4];
    uint32_t ticks = 0;
    int have_ticks = 0;
    if (hosting) {
        const uint64_t clock = *(uint64_t *)(uintptr_t)(image + NET_CLOCK);
        if (clock) { ticks = *(uint32_t *)(uintptr_t)(clock + 0xc); have_ticks = 1; }
        static int game_from_env = -1;
        if (game_from_env < 0) game_from_env = net_game >= 0;
        if (!game_from_env) net_game = *(int16_t *)(uintptr_t)(image + NET_GAME_INDEX);
        LONG players = 1;
        for (int i = 0; i < NET_MAX_PEERS; ++i) players += peers[i].bound;
        public_players = players;
    }
    /* The game's net setup has run (runtime_coop.c netinit makes the network heap the received
     * events are made in, then the rest in the same frame). A joiner runs it only after the host's
     * welcome, while the host's packets already arrive: until then they wait in the inbox (25 Oct
     * 2026: reading them first faulted in the allocator at 0xb333be). */
    const int ready = *(uint64_t *)(uintptr_t)(image + NET_HEAP_SLOT) != 0;
    /* gow3 25 Oct 2026 (run f3): once the round has ended here (the end-game event, type 6, set the
     * net game manager's end state), the other side's packets are no longer read: the host died in
     * ReadPacket -> an event's Deserialize (byte reader 0x5e2100, write to 0 at 0x5e21b5) in the
     * frame its end-game event ran, on a packet the joiner sent before it ended. Our own packets
     * still go out (the end-game event itself, sent in the same frame). */
    const int32_t end_state = *(int32_t *)(uintptr_t)(image + NET_GAME_END);
    static int was_on;                 /* the end state was 0 once the net setup existed */
    if (ready && !end_state) was_on = 1;
    const int ended = was_on && end_state != 0;
    static int told_ended;
    if (ended != told_ended) {
        told_ended = ended;
        printf("Net: the round %s (end state %d): %s\n", ended ? "has ended" : "is on again",
               *(int32_t *)(uintptr_t)(image + NET_GAME_END), ended ? "received game packets are dropped" : "packets read again");
        fflush(stdout);
    }
    for (int i = 0; i < NET_MAX_PEERS; ++i) {
        NetPeer *p = &peers[i];
        NetFrame *frames;
        int used, up, closed;
        EnterCriticalSection(&lock);
        used = p->used;
        up = p->up;
        closed = p->closed;
        frames = p->inbox;
        p->inbox = NULL;
        p->inbox_tail = &p->inbox;
        LeaveCriticalSection(&lock);
        if (!used) continue;
        while (frames) {
            NetFrame *f = frames;
            frames = f->next;
            if (f->kind == NET_KIND_HELLO && f->bits >= 320 && !p->id) {
                hello(p, f);
                fflush(stdout);
            } else if (f->kind == NET_KIND_TIME && f->bits >= 32 && !hosting) {
                if (!host_ticks_at) printf("Net: host net time %u ticks\n", f->words[0]);
                host_ticks = f->words[0];
                host_ticks_at = f->at;
            } else if (f->kind == NET_KIND_WELCOME && f->bits >= 15 * 32 && !hosting) {
                welcome(f);
                fflush(stdout);
            } else if (f->kind == NET_KIND_ROSTER && f->bits >= 32 && !hosting) {
                const uint32_t n = f->words[0];
                if (n <= NET_MAX_PLAYERS && f->bits >= (1 + n * NET_PLAYER_WORDS) * 32) {
                    memcpy(roster, &f->words[1], n * sizeof(NetPlayer));
                    for (uint32_t k = 0; k < n; ++k) roster[k].name[31] = roster[k].skin[31] = 0;
                    roster_count = (int)n;
                    printf("Net: player list from the host: %u players\n", n);
                    fflush(stdout);
                }
            } else if (f->kind == NET_KIND_START && !hosting) {
                start_got = 1;
                printf("Net: the host started the game (net game %d)\n", f->bits >= 32 ? (int)f->words[0] : -1);
                fflush(stdout);
            } else if (f->kind == NET_KIND_PACKET && p->bound && !ready) {
                /* this frame and the ones after it back to the front of the inbox, in order */
                NetFrame *last = f;
                while (last->next) last = last->next;
                EnterCriticalSection(&lock);
                last->next = p->inbox;
                if (!p->inbox) p->inbox_tail = &last->next;
                p->inbox = f;
                LeaveCriticalSection(&lock);
                break;
            } else if (f->kind == NET_KIND_PACKET && p->bound && ended) {
                ++p->dropped_packets;
            } else if (f->kind == NET_KIND_PACKET && p->bound) {
                NetBits bits = {f->words, 0, f->bits, {0, 0}};
                ((NetReadFn)(image + NET_READ))(record_at(p->id) + 0x48, &bits);
                ++p->got_packets;
                p->got_bits += f->bits;
            }
            free(f);
        }
        if (closed) {
            if (p->bound) {
                printf("Net: player %d (%s) left\n", p->id, p->name);
                unbind_record(p);
                fflush(stdout);
            }
            roster_dirty = hosting;
            if (!hosting) { roster_count = 0; host_ticks_at = 0; welcomed = 0; }
            EnterCriticalSection(&lock);
            NetFrame *rest = p->inbox;
            free(p->out);
            p->out = NULL;
            p->out_len = p->out_cap = 0;
            p->inbox = NULL;
            p->inbox_tail = &p->inbox;
            p->id = 0;
            p->name[0] = 0;
            p->skin[0] = 0;
            p->sent_packets = p->sent_bits = p->got_packets = p->got_bits = p->dropped_packets = 0;
            p->used = 0;
            LeaveCriticalSection(&lock);
            while (rest) { NetFrame *next = rest->next; free(rest); rest = next; }
            continue;
        }
        if (!up) continue;
        if (have_ticks) queue_frame(p, NET_KIND_TIME, 32, &ticks);
        if (!p->bound || !ready) continue;
        const uint64_t obj = record_at(p->id) + 0x48;
        NetPacketRecord record = {0};
        NetBits bits = {words, 0, NET_MAX_BYTES * 8, {0, 0}};
        memset(words, 0, sizeof(words));
        ((NetWriteFn)(image + NET_WRITE))(obj, &bits, &record);
        if (record.nodes) {
            queue_frame(p, NET_KIND_PACKET, bits.pos, words);
            ++p->sent_packets;
            p->sent_bits += bits.pos;
        }
        /* TCP delivers every packet: report it delivered now (frees unreliable events, advances
         * the reliable window). */
        ((NetNotifyFn)(image + NET_DELIVERED))(obj, &record);
    }
    if (roster_dirty) {
        roster_dirty = 0;
        send_roster();
    }
    static ULONGLONG last;
    const ULONGLONG now = GetTickCount64();
    if (now - last >= 5000) {
        last = now;
        int any = 0;
        for (int i = 0; i < NET_MAX_PEERS; ++i) {
            const NetPeer *p = &peers[i];
            if (!p->used) continue;
            any = 1;
            printf("Net: player %d (%s): sent %llu packets (%llu bytes), received %llu (%llu bytes), dropped %llu\n",
                   p->id, p->id ? p->name : "no hello yet", (unsigned long long)p->sent_packets,
                   (unsigned long long)(p->sent_bits / 8), (unsigned long long)p->got_packets,
                   (unsigned long long)(p->got_bits / 8), (unsigned long long)p->dropped_packets);
        }
        if (!any) printf("Net: %s\n", hosting ? "no joiners" : "not connected");
        fflush(stdout);
    }
}

/* For the session side (runtime_coop.c). The first remote player with a hello: the host for a
 * joiner, the first joiner for the host. */
int runtime_net_peer(int *id, char *name, size_t size) {
    if (!started) return 0;
    for (int i = 0; i < NET_MAX_PEERS; ++i)
        if (peers[i].bound) {
            if (id) *id = peers[i].id;
            if (name && size) snprintf(name, size, "%s", peers[i].name);
            return 1;
        }
    return 0;
}
/* Every remote player (ids and names, at most max); returns the count. The host lists its
 * joiners; a joiner lists the host's player list without itself (or just the host before the
 * list arrived). Players who left are gone from the list. */
int runtime_net_roster(int *ids, char (*names)[32], int max) {
    if (!started) return 0;
    int n = 0;
    if (!hosting && roster_count) {
        for (int i = 0; i < roster_count && n < max; ++i) {
            if ((int)roster[i].id == local_id) continue;
            ids[n] = (int)roster[i].id;
            if (names) memcpy(names[n], roster[i].name, 32);
            ++n;
        }
        return n;
    }
    for (int i = 0; i < NET_MAX_PEERS && n < max; ++i)
        if (peers[i].bound) {
            ids[n] = peers[i].id;
            if (names) memcpy(names[n], peers[i].name, 32);
            ++n;
        }
    return n;
}
int runtime_net_local_id(void) { return started ? local_id : 0; }
/* Our player name (GOW3_NET_NAME, default Drake / Sully); NULL when the transport is off. */
const char *runtime_net_local_name(void) { return started ? local_name : NULL; }
/* A player's character (GOW3_NET_SKIN of that machine: ours, from its hello, or from the host's
 * player list); 0 when unknown or none was chosen (the default model then). */
int runtime_net_skin(int id, char *out, size_t size) {
    if (!started || !out || !size) return 0;
    const char *skin = NULL;
    if (id == local_id) skin = local_skin;
    for (int i = 0; i < NET_MAX_PEERS && !skin; ++i)
        if (peers[i].bound && peers[i].id == id) skin = peers[i].skin;
    for (int i = 0; i < roster_count && !hosting && (!skin || !*skin); ++i)
        if ((int)roster[i].id == id) skin = roster[i].skin;
    if (!skin || !*skin) return 0;
    snprintf(out, size, "%s", skin);
    return 1;
}
/* 1 once our player id and the net game are known: at once on the host, after the host's welcome
 * on a joiner (the session side waits for it before it fills our slot or picks the net game). */
int runtime_net_ready(void) { return started && (hosting || welcomed); }
/* The lobby (runtime_coop_menu.c, 25 Oct 2026): the host's Start Game goes to every joiner
 * (game thread); a joiner takes the start once. */
int runtime_net_send_start(void) {
    if (!started || !hosting) return 0;
    const uint32_t game = (uint32_t)net_game;
    int sent = 0;
    for (int i = 0; i < NET_MAX_PEERS; ++i)
        if (peers[i].used && peers[i].up && peers[i].id) { queue_frame(&peers[i], NET_KIND_START, 32, &game); ++sent; }
    return sent;
}
int runtime_net_take_start(void) {
    if (!start_got) return 0;
    start_got = 0;
    return 1;
}
int runtime_net_hosting(void) { return started && hosting; }
/* The net game (*net-games* index): the host's own (GOW3_NET_GAME, else the net game manager's),
 * or the one in the host's welcome; -1 when not known yet. */
int runtime_net_game(void) { return started ? net_game : -1; }
/* For a joiner's net clock (runtime_coop.c): the host's net time now, extrapolated from its last
 * time frame (300 ticks per second). 0 on the host or before the first time frame. */
int runtime_net_host_clock(uint32_t *ticks) {
    if (!started || hosting || !host_ticks_at) return 0;
    *ticks = host_ticks + (uint32_t)((GetTickCount64() - host_ticks_at) * 300 / 1000);
    return 1;
}
#else
void runtime_net_frame(unsigned char *base) { (void)base; }
int runtime_net_peer(int *id, char *name, size_t size) { (void)id; (void)name; (void)size; return 0; }
int runtime_net_roster(int *ids, char (*names)[32], int max) { (void)ids; (void)names; (void)max; return 0; }
int runtime_net_local_id(void) { return 0; }
const char *runtime_net_local_name(void) { return NULL; }
int runtime_net_skin(int id, char *out, size_t size) { (void)id; (void)out; (void)size; return 0; }
int runtime_net_ready(void) { return 0; }
int runtime_net_send_start(void) { return 0; }
int runtime_net_take_start(void) { return 0; }
int runtime_net_hosting(void) { return 0; }
int runtime_net_game(void) { return -1; }
int runtime_net_host_clock(uint32_t *ticks) { (void)ticks; return 0; }
#endif
