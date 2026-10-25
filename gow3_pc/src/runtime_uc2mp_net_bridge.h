#ifndef RUNTIME_UC2MP_NET_BRIDGE_H
#define RUNTIME_UC2MP_NET_BRIDGE_H
#include "runtime_uc2mp_lan.h"
#include "runtime_uc2mp_config.h"
#include "runtime_uc2mp_tick.h"
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <time.h>
#endif

/* One transport per game instance, owned exclusively by its lobby/game thread.
 * Pad reads and menu queries share the same throttled poll. */
static Uc2mpLan *uc2mp_menu_transport;
static Uc2mpLanState uc2mp_menu_transport_state;
static uint64_t uc2mp_menu_last_poll;
static unsigned uc2mp_menu_last_count;

static inline void uc2mp_net_poll(Uc2mpLobby *lobby);
static void uc2mp_net_tick(void *context) { uc2mp_net_poll(context); }
static inline int uc2mp_net_bind(Uc2mpLobby *lobby) {
    return runtime_uc2mp_tick_bind(uc2mp_net_tick,lobby);
}

static inline uint64_t uc2mp_net_time(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
#endif
}

static inline int uc2mp_net_nonce(uint64_t *nonce) {
    do {
#ifdef _WIN32
        if (BCryptGenRandom(NULL, (PUCHAR)nonce, sizeof(*nonce), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
            return 0;
#else
        FILE *random = fopen("/dev/urandom", "rb");
        if (!random) return 0;
        size_t read = fread(nonce, 1, sizeof(*nonce), random);
        fclose(random);
        if (read != sizeof(*nonce)) return 0;
#endif
    } while (!*nonce);
    return 1;
}

static inline const char *uc2mp_net_state_name(Uc2mpLanState state) {
    switch (state) {
        case UC2MP_LAN_HOSTING: return "hosting";
        case UC2MP_LAN_CONNECTING: return "connecting";
        case UC2MP_LAN_CONNECTED: return "connected";
        case UC2MP_LAN_REJECTED: return "rejected";
        case UC2MP_LAN_TIMED_OUT: return "timed out";
        case UC2MP_LAN_CLOSED: return "closed";
        case UC2MP_LAN_IO_ERROR: return "I/O error";
    }
    return "unknown";
}

static inline Uc2mpResult uc2mp_net_open_owned(Uc2mpLobby *lobby) {
    const char *host = getenv("UC2MP_LAN_HOST");
    const char *connect = getenv("UC2MP_LAN_CONNECT");
    if (!host && !connect) return uc2mp_open(lobby);
    if (host && connect) return UC2MP_INVALID;
    if (uc2mp_menu_transport) {
        Uc2mpLanState state = uc2mp_lan_state(uc2mp_menu_transport);
        if (state == UC2MP_LAN_HOSTING || state == UC2MP_LAN_CONNECTING ||
            state == UC2MP_LAN_CONNECTED) return UC2MP_OK;
        uc2mp_lan_stop(&uc2mp_menu_transport);
    }
    Uc2mpLanConfig config = {0};
    uint64_t port = 3233;
    const char *port_text = getenv("UC2MP_LAN_PORT");
    if ((port_text && !uc2mp_config_u64(port_text, &port)) || !port || port > 65535 ||
        !uc2mp_config_hex(getenv("UC2MP_LAN_TOKEN"), config.token, sizeof(config.token)) ||
        !uc2mp_net_nonce(&config.nonce)) return UC2MP_INVALID;
    config.bind_address = host ? host : "0.0.0.0";
    config.host_address = connect;
    config.port = (uint16_t)port;
    /* A fresh nonce/host epoch is generated even when reopening in one process. */
    uint64_t now = uc2mp_net_time();
    if (host) {
        if (uc2mp_open(lobby) != UC2MP_OK) return UC2MP_INVALID;
        uc2mp_menu_transport = uc2mp_lan_host(lobby, &config, now);
        if (!uc2mp_menu_transport) uc2mp_close(lobby);
    } else {
        if (lobby->local.id == 1) return UC2MP_INVALID; /* ID1 is reserved for host. */
        uc2mp_close(lobby);
        uc2mp_menu_transport = uc2mp_lan_client(lobby, &config, now);
    }
    if (!uc2mp_menu_transport) return UC2MP_INVALID;
    uc2mp_menu_last_poll = 0;
    uc2mp_menu_last_count = lobby->count;
    uc2mp_menu_transport_state = uc2mp_lan_state(uc2mp_menu_transport);
    printf("UC2MP LAN: %s address=%s port=%u\n",
           uc2mp_net_state_name(uc2mp_menu_transport_state), host ? host : connect, config.port);
    fflush(stdout);
    return UC2MP_OK;
}

static inline Uc2mpResult uc2mp_net_open(Uc2mpLobby *lobby) {
    int already_owned=runtime_uc2mp_tick_is_owner();
    if (!uc2mp_net_bind(lobby)) return UC2MP_INVALID;
    Uc2mpResult result=uc2mp_net_open_owned(lobby);
    if (result!=UC2MP_OK && !already_owned) runtime_uc2mp_tick_unbind();
    return result;
}

/* Optional owner-thread step after each poll (uc2mp_menu_bridge.h: match launch), also
 * without a transport (local lobby). */
static void (*uc2mp_net_after_poll)(Uc2mpLobby *lobby);
static inline void uc2mp_net_poll_transport(Uc2mpLobby *lobby);
static inline void uc2mp_net_poll(Uc2mpLobby *lobby) {
    if (!runtime_uc2mp_tick_is_owner()) return;
    uc2mp_net_poll_transport(lobby);
    if (uc2mp_net_after_poll) uc2mp_net_after_poll(lobby);
}
static inline void uc2mp_net_poll_transport(Uc2mpLobby *lobby) {
    if (!uc2mp_menu_transport) return;
    uint64_t now = uc2mp_net_time();
    if (now - uc2mp_menu_last_poll < 10) return;
    uc2mp_menu_last_poll = now;
    Uc2mpLanState state = uc2mp_lan_poll(uc2mp_menu_transport, now);
    if (state != uc2mp_menu_transport_state || lobby->count != uc2mp_menu_last_count) {
        printf("UC2MP LAN: %s members=%u\n", uc2mp_net_state_name(state), lobby->count);
        fflush(stdout);
        uc2mp_menu_transport_state = state;
        uc2mp_menu_last_count = lobby->count;
    }
}

static inline void uc2mp_net_close(Uc2mpLobby *lobby) {
    if (!runtime_uc2mp_tick_is_owner()) return;
    uc2mp_lan_stop(&uc2mp_menu_transport);
    uc2mp_close(lobby);
    /* No callback can run concurrently: only this thread has the TLS slot.
     * Release ownership after cleanup so a fresh owner cannot race teardown. */
    runtime_uc2mp_tick_unbind();
}

static inline Uc2mpResult uc2mp_net_ready(Uc2mpLobby *lobby, int ready) {
    if (!runtime_uc2mp_tick_is_owner()) return UC2MP_INVALID;
    return uc2mp_menu_transport ? uc2mp_lan_ready(uc2mp_menu_transport, ready) :
        uc2mp_ready(lobby, lobby->local.id, ready);
}
#endif
