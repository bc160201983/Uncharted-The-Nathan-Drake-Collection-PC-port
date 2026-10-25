#include "runtime_uc2mp_lan.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET LanSocket;
typedef int LanSocklen;
#define LAN_BAD_SOCKET INVALID_SOCKET
#define lan_close closesocket
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
typedef int LanSocket;
typedef socklen_t LanSocklen;
#define LAN_BAD_SOCKET (-1)
#define lan_close close
#endif

/* Wire integers are big endian. Header: magic(4), version(1), kind(1),
 * total bytes(2), token(16), recipient/client nonce(8), sequence(8), host epoch(8).
 * Snapshot: generation(8), host ID(8), mode(4), map(4), capacity(1), count(1),
 * rule indices [score limit, time limit, rounds, friendly fire](4),
 * then count * [ID(8), NUL-padded validated UTF-8 name(32), ready(1)],
 * preparation [phase(1), attempt ID(8), remaining ms(4), prepared mask(2),
 * loading-receipt mask(2)]. Deadline is local, not a shared wall clock.
 * No compiler layouts, addresses, or guest game objects go onto the wire. */
enum { HEADER = 48, SETTINGS_BYTES = 30, PREPARATION_BYTES=17, MAX_PACKET = 505, JOIN = 1, PING, READY, LEAVE,
       SNAPSHOT, REJECT, SHUTDOWN, CHALLENGE, PREPARE_ACK, PREPARE_CANCEL };
typedef struct {
    struct sockaddr_in address;
    uint64_t nonce, id, sequence, seen;
    int active;
} LanPeer;
struct Uc2mpLan {
    LanSocket socket;
    Uc2mpLobby *lobby;
    Uc2mpLanState state;
    struct sockaddr_in host;
    uint8_t token[16];
    uint16_t port;
    uint64_t nonce, epoch, sequence, received_sequence, sent_revision;
    uint64_t now, received_at, sent_at, settings_revision;
    uint32_t heartbeat_ms, timeout_ms;
    Uc2mpSettings observed_settings;
    LanPeer peers[UC2MP_MAX_MEMBERS - 1];
    /* Retired nonces prevent delayed JOIN after leave/expiry re-creating a
     * player. Once full, new connections are rejected until a new session:
     * this bounded fail-closed policy avoids silently forgetting tombstones. */
    uint64_t retired[128];
    unsigned retired_count;
    int is_host, started_sending, desired_ready;
    uint64_t desired_ack, desired_cancel;
    Uc2mpPhase desired_phase;
};

static void put64(uint8_t *p, uint64_t value) {
    for (int i = 7; i >= 0; --i) { p[i] = (uint8_t)value; value >>= 8; }
}
static uint64_t get64(const uint8_t *p) {
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}
static void put32(uint8_t *p, uint32_t value) {
    for (int i = 3; i >= 0; --i) { p[i] = (uint8_t)value; value >>= 8; }
}
static uint32_t get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static int same_address(const struct sockaddr_in *a, const struct sockaddr_in *b) {
    return a->sin_family == b->sin_family && a->sin_port == b->sin_port &&
           a->sin_addr.s_addr == b->sin_addr.s_addr;
}
static int same_settings(Uc2mpSettings a, Uc2mpSettings b) {
    return uc2mp_settings_equal(a, b);
}
static int would_block(void) {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}
static int oversized(void) {
#ifdef _WIN32
    return WSAGetLastError() == WSAEMSGSIZE;
#else
    return errno == EMSGSIZE;
#endif
}
static int transient_receive_error(void) {
#ifdef _WIN32
    /* UDP ICMP Port Unreachable, e.g. peer terminated before leave arrived. */
    return WSAGetLastError() == WSAECONNRESET;
#else
    return errno == ECONNREFUSED;
#endif
}
static void header(Uc2mpLan *l, uint8_t *p, unsigned type, unsigned size,
                   uint64_t nonce, uint64_t sequence) {
    memcpy(p, "UC2L", 4);
    p[4] = 3; p[5] = (uint8_t)type;
    p[6] = (uint8_t)(size >> 8); p[7] = (uint8_t)size;
    memcpy(p + 8, l->token, 16);
    put64(p + 24, nonce); put64(p + 32, sequence);
    put64(p + 40, l->epoch);
}
static void send_packet(Uc2mpLan *l, const struct sockaddr_in *to,
                        const uint8_t *p, unsigned size) {
    /* UDP back-pressure/dropped sends are recovered through heartbeat/retry. */
    (void)sendto(l->socket, (const char *)p, (int)size, 0,
                 (const struct sockaddr *)to, sizeof(*to));
}
static int read_profile(Uc2mpProfile *out, const uint8_t *p) {
    char name[UC2MP_NAME_BYTES];
    memcpy(name, p + 8, sizeof(name));
    const char *end = (const char *)memchr(name, 0, sizeof(name));
    if (!end) return 0;
    for (size_t i = (size_t)(end - name); i < sizeof(name); ++i)
        if (name[i]) return 0;
    return uc2mp_profile(out, get64(p), name) == UC2MP_OK;
}
static void send_snapshot(Uc2mpLan *l, const LanPeer *peer) {
    uint8_t p[MAX_PACKET] = {0};
    const Uc2mpLobby *b = l->lobby;
    unsigned size = HEADER + SETTINGS_BYTES + b->count * 41 + PREPARATION_BYTES;
    if (!b->open || !b->count || b->count > UC2MP_MAX_MEMBERS) return;
    header(l, p, SNAPSHOT, size, peer->nonce, b->revision);
    put64(p + HEADER, b->generation); put64(p + HEADER + 8, b->host_id);
    put32(p + HEADER + 16, b->settings.mode_id); put32(p + HEADER + 20, b->settings.map_id);
    p[HEADER + 24] = (uint8_t)b->settings.capacity; p[HEADER + 25] = (uint8_t)b->count;
    p[HEADER + 26] = b->settings.score_limit; p[HEADER + 27] = b->settings.time_limit;
    p[HEADER + 28] = b->settings.rounds; p[HEADER + 29] = b->settings.friendly_fire;
    for (unsigned i = 0; i < b->count; ++i) {
        uint8_t *m = p + HEADER + SETTINGS_BYTES + i * 41;
        put64(m, b->members[i].profile.id);
        memcpy(m + 8, b->members[i].profile.name, UC2MP_NAME_BYTES);
        m[40] = (uint8_t)b->members[i].ready;
    }
    uint8_t *prep=p+size-PREPARATION_BYTES;
    prep[0]=(uint8_t)b->preparation.phase;
    put64(prep+1,b->preparation.id);
    uint64_t remaining=b->preparation.deadline_ms>l->now ? b->preparation.deadline_ms-l->now : 0;
    put32(prep+9,(uint32_t)remaining);
    prep[13]=(uint8_t)(b->preparation.prepared>>8); prep[14]=(uint8_t)b->preparation.prepared;
    prep[15]=(uint8_t)(b->preparation.loading>>8); prep[16]=(uint8_t)b->preparation.loading;
    send_packet(l, &peer->address, p, size);
}
static void control(Uc2mpLan *l, const struct sockaddr_in *to, unsigned type,
                    uint64_t nonce, uint64_t seq) {
    uint8_t p[HEADER]; header(l, p, type, HEADER, nonce, seq);
    send_packet(l, to, p, sizeof(p));
}
static void retire_nonce(Uc2mpLan *l, uint64_t nonce) {
    for (unsigned i = 0; i < l->retired_count; ++i)
        if (l->retired[i] == nonce) return;
    if (l->retired_count < sizeof(l->retired) / sizeof(l->retired[0]))
        l->retired[l->retired_count++] = nonce;
}
static void retire(Uc2mpLan *l, LanPeer *peer) {
    retire_nonce(l, peer->nonce);
    (void)uc2mp_leave(l->lobby, peer->id);
    memset(peer, 0, sizeof(*peer));
}
static void host_packet(Uc2mpLan *l, const uint8_t *p, unsigned size,
                        const struct sockaddr_in *from) {
    unsigned type = p[5];
    uint64_t nonce = get64(p + 24), seq = get64(p + 32);
    if (!nonce || !seq || !((type>=JOIN && type<=LEAVE) || type==PREPARE_ACK || type==PREPARE_CANCEL)) return;
    if (get64(p + 40) != l->epoch) {
        Uc2mpProfile checked;
        if (type == JOIN && !get64(p + 40) && size == HEADER + 40 &&
            read_profile(&checked, p + HEADER))
            control(l, from, CHALLENGE, nonce, seq);
        return;
    }
    LanPeer *peer = NULL, *empty = NULL;
    for (unsigned i = 0; i < UC2MP_MAX_MEMBERS - 1; ++i) {
        LanPeer *candidate = &l->peers[i];
        if (!candidate->active) empty = candidate;
        else if (candidate->nonce == nonce) {
            if (!same_address(&candidate->address, from)) return;
            peer = candidate;
        }
    }
    if (peer && seq <= peer->sequence) return;
    if (type == JOIN) {
        Uc2mpProfile profile;
        if (size != HEADER + 40 || !read_profile(&profile, p + HEADER)) return;
        if (peer) {
            if (profile.id != peer->id) return;
            int match = 0;
            for (unsigned i = 0; i < l->lobby->count; ++i)
                if (l->lobby->members[i].profile.id == profile.id &&
                    !strcmp(l->lobby->members[i].profile.name, profile.name)) match = 1;
            if (!match) return;
        } else {
            int denied = !empty || l->retired_count >= 128;
            for (unsigned i = 0; i < l->retired_count; ++i)
                if (l->retired[i] == nonce) denied = 1;
            for (unsigned i = 0; i < l->lobby->count; ++i)
                if (l->lobby->members[i].profile.id == profile.id) denied = 1;
            if (denied || uc2mp_join(l->lobby, &profile) != UC2MP_OK) {
                control(l, from, REJECT, nonce, seq); return;
            }
            peer = empty;
            *peer = (LanPeer){.address = *from, .nonce = nonce, .id = profile.id, .active = 1};
        }
    } else {
        if (!peer) {
            /* UDP can deliver shutdown before the initial JOIN. Remember it
             * even without an inserted member so that delayed JOIN cannot
             * create a ghost after this connection has already been closed. */
            if (type == LEAVE && size == HEADER) retire_nonce(l, nonce);
            return;
        }
        if (type==PREPARE_ACK) {
            if (size!=HEADER+9 || uc2mp_prepare_ack(l->lobby,peer->id,get64(p+HEADER),
                    (Uc2mpPhase)p[HEADER+8])!=UC2MP_OK) return;
        } else if (type==PREPARE_CANCEL) {
            if (size!=HEADER+8 || get64(p+HEADER)!=l->lobby->preparation.id ||
                l->lobby->preparation.phase==UC2MP_PHASE_LOBBY) return;
            (void)uc2mp_prepare_cancel(l->lobby,l->lobby->host_id,get64(p+HEADER));
        } else if (type == READY) {
            if (size != HEADER + 9 || p[HEADER] > 1) return;
            uint64_t acknowledged = get64(p + HEADER + 1);
            if (acknowledged > l->lobby->revision) return;
            if (acknowledged >= l->settings_revision)
                (void)uc2mp_ready(l->lobby, peer->id, p[HEADER]);
        } else if (size != HEADER) return;
    }
    peer->sequence = seq;
    peer->seen = l->now;
    if (type == LEAVE) { retire(l, peer); return; }
    send_snapshot(l, peer);
}
static void client_packet(Uc2mpLan *l, const uint8_t *p, unsigned size,
                          const struct sockaddr_in *from) {
    uint64_t seq = get64(p + 32);
    if (!same_address(from, &l->host) || get64(p + 24) != l->nonce || !seq) return;
    if (p[5] == CHALLENGE && size == HEADER && l->state == UC2MP_LAN_CONNECTING &&
        !l->epoch && seq <= l->sequence && get64(p + 40)) {
        l->epoch = get64(p + 40); l->started_sending = 0; return;
    }
    if (!l->epoch || get64(p + 40) != l->epoch) return;
    if (p[5] == REJECT && size == HEADER && l->state == UC2MP_LAN_CONNECTING && seq <= l->sequence) {
        l->state = UC2MP_LAN_REJECTED; return;
    }
    if (p[5] == SHUTDOWN && size == HEADER && seq > l->received_sequence) {
        (void)uc2mp_close(l->lobby); l->state = UC2MP_LAN_CLOSED; return;
    }
    if (p[5] != SNAPSHOT || size < HEADER + SETTINGS_BYTES + PREPARATION_BYTES || seq < l->received_sequence) return;
    unsigned count = p[HEADER + 25], capacity = p[HEADER + 24];
    if (!count || count > capacity || capacity > UC2MP_MAX_MEMBERS || size != HEADER + SETTINGS_BYTES + count * 41 + PREPARATION_BYTES) return;
    Uc2mpLobby next = {0};
    next.local = l->lobby->local; next.open = 1;
    next.generation = get64(p + HEADER); next.host_id = get64(p + HEADER + 8);
    next.revision = seq; next.count = count;
    next.settings = (Uc2mpSettings){get32(p + HEADER + 16), get32(p + HEADER + 20), capacity,
                                    p[HEADER + 26], p[HEADER + 27], p[HEADER + 28], p[HEADER + 29]};
    if (!uc2mp_rules_valid(next.settings)) return;
    if (!next.generation || !next.host_id) return;
    int found = 0, local_ready = 0;
    unsigned local_index=0;
    for (unsigned i = 0; i < count; ++i) {
        const uint8_t *m = p + HEADER + SETTINGS_BYTES + i * 41;
        if (!read_profile(&next.members[i].profile, m) || m[40] > 1) return;
        next.members[i].ready = m[40];
        for (unsigned j = 0; j < i; ++j)
            if (next.members[i].profile.id == next.members[j].profile.id) return;
        if (next.members[i].profile.id == next.local.id) {
            if (strcmp(next.members[i].profile.name, next.local.name)) return;
            found = 1; local_ready = m[40];
            local_index=i;
        }
    }
    if (!found || next.members[0].profile.id != next.host_id || next.host_id == next.local.id) return;
    if (l->state == UC2MP_LAN_CONNECTED && next.generation != l->lobby->generation) return;
    const uint8_t *prep=p+size-PREPARATION_BYTES;
    next.preparation.phase=(Uc2mpPhase)prep[0];
    next.preparation.id=get64(prep+1);
    uint32_t remaining=get32(prep+9);
    next.preparation.prepared=(uint16_t)(((unsigned)prep[13]<<8)|prep[14]);
    next.preparation.loading=(uint16_t)(((unsigned)prep[15]<<8)|prep[16]);
    unsigned all=(1u<<count)-1;
    if (next.preparation.phase>UC2MP_PHASE_LOADING || remaining>UC2MP_PREPARE_MAX_MS ||
        (next.preparation.prepared&~all) || (next.preparation.loading&~all)) return;
    if (next.preparation.phase==UC2MP_PHASE_LOBBY) {
        if (remaining || next.preparation.prepared || next.preparation.loading) return;
    } else {
        if (!next.preparation.id || !remaining || !uc2mp_match_settings_valid(next.settings) ||
            l->now>UINT64_MAX-remaining) return;
        for (unsigned i=0;i<count;++i) if (!next.members[i].ready) return;
        if (next.preparation.phase==UC2MP_PHASE_PREPARING && next.preparation.loading) return;
        if (next.preparation.phase==UC2MP_PHASE_LOADING && next.preparation.prepared!=all) return;
        next.preparation.deadline_ms=l->now+remaining;
    }
    const Uc2mpPreparation *old=&l->lobby->preparation;
    if (next.preparation.id<old->id) return;
    if (l->state==UC2MP_LAN_CONNECTED && next.preparation.id==old->id) {
        if (old->phase==UC2MP_PHASE_LOBBY && next.preparation.phase!=UC2MP_PHASE_LOBBY) return;
        if (old->phase==UC2MP_PHASE_LOADING && next.preparation.phase==UC2MP_PHASE_PREPARING) return;
        if (next.preparation.phase!=UC2MP_PHASE_LOBBY) {
            if (next.count!=l->lobby->count || !same_settings(next.settings,l->lobby->settings)) return;
            for (unsigned i=0;i<count;++i)
                if (memcmp(&next.members[i].profile,&l->lobby->members[i].profile,sizeof(Uc2mpProfile))) return;
            if ((next.preparation.prepared&old->prepared)!=old->prepared ||
                (next.preparation.loading&old->loading)!=old->loading) return;
            if (old->deadline_ms<next.preparation.deadline_ms) next.preparation.deadline_ms=old->deadline_ms;
        }
    }
    /* Equal revision is a valid heartbeat but never replaces existing state. */
    if (seq > l->received_sequence) {
        if (!same_settings(next.settings, l->lobby->settings)) l->desired_ready = -1;
        *l->lobby = next;
        l->received_sequence = seq;
    }
    /* Even an equal-revision heartbeat may shorten, never extend, the budget. */
    if (next.preparation.phase!=UC2MP_PHASE_LOBBY && next.preparation.id==l->lobby->preparation.id &&
        next.preparation.deadline_ms<l->lobby->preparation.deadline_ms)
        l->lobby->preparation.deadline_ms=next.preparation.deadline_ms;
    if (l->lobby->preparation.phase!=UC2MP_PHASE_LOBBY) l->desired_ready=-1;
    if (l->desired_ack && (l->desired_ack!=l->lobby->preparation.id ||
        l->desired_phase!=l->lobby->preparation.phase ||
        ((l->desired_phase==UC2MP_PHASE_PREPARING ? l->lobby->preparation.prepared :
            l->lobby->preparation.loading)&(1u<<local_index)))) l->desired_ack=0;
    if (l->desired_cancel && (l->desired_cancel!=l->lobby->preparation.id ||
        l->lobby->preparation.phase==UC2MP_PHASE_LOBBY)) l->desired_cancel=0;
    if (l->desired_ready == local_ready) l->desired_ready = -1;
    l->received_at = l->now;
    l->state = UC2MP_LAN_CONNECTED;
}
static Uc2mpLan *create(Uc2mpLobby *lobby, const Uc2mpLanConfig *c,
                         uint64_t now, int host) {
    if (!lobby || !c || !c->bind_address || !c->nonce) return NULL;
    Uc2mpProfile checked;
    if (uc2mp_profile(&checked, lobby->local.id, lobby->local.name) != UC2MP_OK) return NULL;
    if (host ? (!lobby->open || lobby->count != 1 || lobby->host_id != lobby->local.id ||
                 lobby->members[0].profile.id != lobby->local.id || !lobby->revision ||
                 !lobby->generation || !lobby->settings.capacity || lobby->settings.capacity > UC2MP_MAX_MEMBERS ||
                 lobby->preparation.phase!=UC2MP_PHASE_LOBBY)
             : (lobby->open || !c->nonce || !c->host_address || !c->port)) return NULL;
    unsigned token_bits = 0;
    for (unsigned i = 0; i < 16; ++i) token_bits |= c->token[i];
    uint32_t heartbeat = c->heartbeat_ms ? c->heartbeat_ms : 500;
    uint32_t timeout = c->timeout_ms ? c->timeout_ms : 5000;
    if (!token_bits || heartbeat < 10 || (uint64_t)timeout < 3ULL * heartbeat) return NULL;
    struct sockaddr_in bind_address = {0}, host_address = {0};
    bind_address.sin_family = AF_INET;
    bind_address.sin_port = htons(host ? c->port : 0);
    if (inet_pton(AF_INET, c->bind_address, &bind_address.sin_addr) != 1) return NULL;
    if (!host) {
        host_address.sin_family = AF_INET; host_address.sin_port = htons(c->port);
        if (inet_pton(AF_INET, c->host_address, &host_address.sin_addr) != 1 ||
            host_address.sin_addr.s_addr == INADDR_ANY ||
            host_address.sin_addr.s_addr == INADDR_BROADCAST) return NULL;
    }
    Uc2mpLan *l = (Uc2mpLan *)calloc(1, sizeof(*l));
    if (!l) return NULL;
    l->socket = LAN_BAD_SOCKET;
#ifdef _WIN32
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data)) { free(l); return NULL; }
#endif
    l->socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (l->socket == LAN_BAD_SOCKET) goto fail;
#ifdef _WIN32
    u_long nonblocking = 1;
    if (ioctlsocket(l->socket, FIONBIO, &nonblocking)) goto fail;
    BOOL exclusive = TRUE;
    if (setsockopt(l->socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   (const char *)&exclusive, sizeof(exclusive))) goto fail;
#else
    if (fcntl(l->socket, F_SETFL, O_NONBLOCK) == -1) goto fail;
#endif
    if (bind(l->socket, (struct sockaddr *)&bind_address, sizeof(bind_address))) goto fail;
    LanSocklen length = sizeof(bind_address);
    if (getsockname(l->socket, (struct sockaddr *)&bind_address, &length)) goto fail;
    l->port = ntohs(bind_address.sin_port);
    l->host = host_address; l->is_host = host; l->lobby = lobby;
    l->state = host ? UC2MP_LAN_HOSTING : UC2MP_LAN_CONNECTING;
    memcpy(l->token, c->token, 16); l->nonce = c->nonce;
    l->epoch = host ? c->nonce : 0;
    l->heartbeat_ms = heartbeat; l->timeout_ms = timeout;
    l->now = l->received_at = now; l->desired_ready = -1;
    l->observed_settings = lobby->settings; l->settings_revision = lobby->revision;
    /* A new authenticated host epoch has its own attempt-ID namespace. A
     * closed client's old tombstone must not reject a newly opened host's ID0.
     * Old datagrams still fail the fresh client nonce/host challenge checks. */
    if (!host) lobby->preparation=(Uc2mpPreparation){0};
    return l;
fail:
    if (l->socket != LAN_BAD_SOCKET) lan_close(l->socket);
#ifdef _WIN32
    WSACleanup();
#endif
    free(l); return NULL;
}
Uc2mpLan *uc2mp_lan_host(Uc2mpLobby *l, const Uc2mpLanConfig *c, uint64_t now) {
    return create(l, c, now, 1);
}
Uc2mpLan *uc2mp_lan_client(Uc2mpLobby *l, const Uc2mpLanConfig *c, uint64_t now) {
    return create(l, c, now, 0);
}
Uc2mpLanState uc2mp_lan_poll(Uc2mpLan *l, uint64_t now) {
    if (!l) return UC2MP_LAN_CLOSED;
    if (l->state != UC2MP_LAN_HOSTING && l->state != UC2MP_LAN_CONNECTING &&
        l->state != UC2MP_LAN_CONNECTED) return l->state;
    if (now < l->now) return l->state;
    l->now = now;
    if (l->is_host) {
        if (!l->lobby->open) { l->state = UC2MP_LAN_CLOSED; return l->state; }
        (void)uc2mp_prepare_expire(l->lobby,now);
        if (!same_settings(l->observed_settings, l->lobby->settings)) {
            l->observed_settings = l->lobby->settings;
            l->settings_revision = l->lobby->revision;
        }
    } else if (l->lobby->preparation.phase!=UC2MP_PHASE_LOBBY && now>=l->lobby->preparation.deadline_ms) {
        l->state=UC2MP_LAN_TIMED_OUT; (void)uc2mp_close(l->lobby); return l->state;
    }
    for (unsigned i = 0; i < 32; ++i) {
        /* Extra byte detects truncation on POSIX; Windows returns WSAEMSGSIZE. */
        uint8_t p[MAX_PACKET + 1];
        struct sockaddr_in from = {0}; LanSocklen length = sizeof(from);
        int size = (int)recvfrom(l->socket, (char *)p, sizeof(p), 0,
                                  (struct sockaddr *)&from, &length);
        if (size < 0) {
            if (would_block()) break;
            if (oversized() || transient_receive_error()) continue;
            l->state = UC2MP_LAN_IO_ERROR; (void)uc2mp_close(l->lobby); return l->state;
        }
        if (size < HEADER || size > MAX_PACKET || memcmp(p, "UC2L", 4) ||
            p[4] != 3 || (((unsigned)p[6] << 8) | p[7]) != (unsigned)size ||
            memcmp(p + 8, l->token, 16)) continue;
        if (l->is_host) host_packet(l, p, (unsigned)size, &from);
        else client_packet(l, p, (unsigned)size, &from);
        if (l->state >= UC2MP_LAN_REJECTED) return l->state;
    }
    int preparing=l->lobby->preparation.phase!=UC2MP_PHASE_LOBBY;
    if (l->is_host && !preparing) {
        for (unsigned i = 0; i < UC2MP_MAX_MEMBERS - 1; ++i)
            if (l->peers[i].active && now - l->peers[i].seen >= l->timeout_ms)
                retire(l, &l->peers[i]);
    } else if (!l->is_host && !preparing && now - l->received_at >= l->timeout_ms) {
        l->state = UC2MP_LAN_TIMED_OUT; (void)uc2mp_close(l->lobby); return l->state;
    }
    if (!l->started_sending || now - l->sent_at >= l->heartbeat_ms ||
        (l->is_host && l->sent_revision != l->lobby->revision)) {
        l->started_sending = 1; l->sent_at = now;
        if (l->is_host) {
            for (unsigned i = 0; i < UC2MP_MAX_MEMBERS - 1; ++i)
                if (l->peers[i].active) send_snapshot(l, &l->peers[i]);
            l->sent_revision = l->lobby->revision;
        } else {
            if (l->sequence == UINT64_MAX) {
                l->state = UC2MP_LAN_IO_ERROR; (void)uc2mp_close(l->lobby); return l->state;
            }
            uint8_t p[HEADER + 40] = {0}; unsigned size = HEADER, type = PING;
            if (l->state == UC2MP_LAN_CONNECTING) {
                size += 40; type = JOIN;
                put64(p + HEADER, l->lobby->local.id);
                memcpy(p + HEADER + 8, l->lobby->local.name, UC2MP_NAME_BYTES);
            } else if (l->desired_cancel) {
                size+=8; type=PREPARE_CANCEL; put64(p+HEADER,l->desired_cancel);
            } else if (l->desired_ack) {
                size+=9; type=PREPARE_ACK; put64(p+HEADER,l->desired_ack);
                p[HEADER+8]=(uint8_t)l->desired_phase;
            } else if (l->desired_ready >= 0) {
                size += 9; type = READY;
                p[HEADER] = (uint8_t)l->desired_ready;
                put64(p + HEADER + 1, l->received_sequence);
            }
            header(l, p, type, size, l->nonce, ++l->sequence);
            send_packet(l, &l->host, p, size);
        }
    }
    return l->state;
}
Uc2mpLanState uc2mp_lan_state(const Uc2mpLan *l) {
    return l ? l->state : UC2MP_LAN_CLOSED;
}
uint16_t uc2mp_lan_local_port(const Uc2mpLan *l) { return l ? l->port : 0; }
Uc2mpResult uc2mp_lan_ready(Uc2mpLan *l, int ready) {
    if (!l || (ready != 0 && ready != 1)) return UC2MP_INVALID;
    if (l->state == UC2MP_LAN_HOSTING)
        return uc2mp_ready(l->lobby, l->lobby->local.id, ready);
    if (l->state != UC2MP_LAN_CONNECTED) return UC2MP_CLOSED;
    if (l->lobby->preparation.phase!=UC2MP_PHASE_LOBBY) return UC2MP_BUSY;
    l->desired_ready = ready; l->started_sending = 0;
    return UC2MP_OK;
}
Uc2mpResult uc2mp_lan_prepare_begin(Uc2mpLan *l,uint32_t timeout) {
    if (!l) return UC2MP_INVALID;
    if (!l->is_host) return UC2MP_NOT_HOST;
    if (l->state!=UC2MP_LAN_HOSTING) return UC2MP_CLOSED;
    return uc2mp_prepare_begin(l->lobby,l->lobby->local.id,l->now,timeout);
}
Uc2mpResult uc2mp_lan_prepare_ack(Uc2mpLan *l,uint64_t id,Uc2mpPhase phase) {
    if (!l || !id || (phase!=UC2MP_PHASE_PREPARING && phase!=UC2MP_PHASE_LOADING)) return UC2MP_INVALID;
    if (l->state!=UC2MP_LAN_HOSTING && l->state!=UC2MP_LAN_CONNECTED) return UC2MP_CLOSED;
    if (l->lobby->preparation.id!=id || l->lobby->preparation.phase!=phase) return UC2MP_INVALID;
    if (l->is_host) return uc2mp_prepare_ack(l->lobby,l->lobby->local.id,id,phase);
    l->desired_ack=id; l->desired_phase=phase; l->started_sending=0;
    return UC2MP_OK;
}
Uc2mpResult uc2mp_lan_prepare_load(Uc2mpLan *l,uint64_t id) {
    if (!l) return UC2MP_INVALID;
    if (!l->is_host) return UC2MP_NOT_HOST;
    if (l->state!=UC2MP_LAN_HOSTING) return UC2MP_CLOSED;
    /* Grace preserves a blocked peer's slot, not proof that an absent peer
     * may start. Before authorizing loading, require every remote participant
     * still has a fresh authenticated transport observation. */
    for (unsigned m=0;m<l->lobby->count;++m) {
        uint64_t member=l->lobby->members[m].profile.id;
        if (member==l->lobby->local.id) continue;
        int live=0;
        for (unsigned i=0;i<UC2MP_MAX_MEMBERS-1;++i)
            if (l->peers[i].active && l->peers[i].id==member &&
                l->now-l->peers[i].seen<l->timeout_ms) live=1;
        if (!live) return UC2MP_NOT_READY;
    }
    return uc2mp_prepare_load(l->lobby,l->lobby->local.id,id,l->now);
}
Uc2mpResult uc2mp_lan_prepare_cancel(Uc2mpLan *l,uint64_t id) {
    if (!l || !id) return UC2MP_INVALID;
    if (l->state!=UC2MP_LAN_HOSTING && l->state!=UC2MP_LAN_CONNECTED) return UC2MP_CLOSED;
    if (id!=l->lobby->preparation.id || l->lobby->preparation.phase==UC2MP_PHASE_LOBBY) return UC2MP_INVALID;
    if (l->is_host) return uc2mp_prepare_cancel(l->lobby,l->lobby->local.id,id);
    l->desired_cancel=id; l->desired_ack=0; l->started_sending=0;
    return UC2MP_OK;
}
int uc2mp_lan_prepare_can_block(const Uc2mpLan *l) {
    if (!l || (l->state!=UC2MP_LAN_HOSTING && l->state!=UC2MP_LAN_CONNECTED) ||
        l->lobby->preparation.phase!=UC2MP_PHASE_LOADING || l->now>=l->lobby->preparation.deadline_ms) return 0;
    unsigned mask=l->lobby->preparation.loading;
    if (l->is_host) return mask==((1u<<l->lobby->count)-1);
    for (unsigned i=0;i<l->lobby->count;++i)
        if (l->lobby->members[i].profile.id==l->lobby->local.id) return !!(mask&(1u<<i));
    return 0;
}
void uc2mp_lan_stop(Uc2mpLan **handle) {
    if (!handle || !*handle) return;
    Uc2mpLan *l = *handle; *handle = NULL;
    for (unsigned repeat = 0; repeat < 3; ++repeat) {
        if (l->is_host) {
            for (unsigned i = 0; i < UC2MP_MAX_MEMBERS - 1; ++i)
                if (l->peers[i].active)
                    control(l, &l->peers[i].address, SHUTDOWN, l->peers[i].nonce, l->lobby->revision + 1);
        } else if ((l->state == UC2MP_LAN_CONNECTED || l->state == UC2MP_LAN_CONNECTING) &&
                   l->epoch && l->sequence != UINT64_MAX)
            control(l, &l->host, LEAVE, l->nonce, ++l->sequence);
    }
    (void)uc2mp_close(l->lobby);
    lan_close(l->socket);
#ifdef _WIN32
    WSACleanup();
#endif
    memset(l, 0, sizeof(*l)); free(l);
}
