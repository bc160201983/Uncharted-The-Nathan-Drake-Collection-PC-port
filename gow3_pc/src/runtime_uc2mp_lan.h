#ifndef RUNTIME_UC2MP_LAN_H
#define RUNTIME_UC2MP_LAN_H
#include "runtime_uc2mp.h"

/* Opt-in UDP IPv4 lobby transport, protocol 2 (v1 rejected). Single-thread ownership.
 * No discovery, DNS, UPnP, firewall changes, PSN, or gameplay synchronization.
 * token is a 128-bit shared bearer secret sent IN CLEAR: trusted LAN only.
 * Caller generates a random shared token and a fresh random nonzero nonce
 * for every host session/client connection. A challenge binds the first join
 * to the current host epoch so delayed packets cannot join a reopened room.
 * Lobby/profile storage must outlive the transport. Do not change a client's
 * replicated lobby directly; host may use the ordinary model operations.
 */
typedef struct Uc2mpLan Uc2mpLan;
typedef enum {
    UC2MP_LAN_HOSTING, UC2MP_LAN_CONNECTING, UC2MP_LAN_CONNECTED,
    UC2MP_LAN_REJECTED, UC2MP_LAN_TIMED_OUT, UC2MP_LAN_CLOSED,
    UC2MP_LAN_IO_ERROR
} Uc2mpLanState;
typedef struct {
    const char *bind_address; /* Numeric IPv4, required. 127.0.0.1 or 0.0.0.0. */
    uint16_t port; /* Host bind port; client destination port. Host 0 = ephemeral. */
    const char *host_address; /* Required numeric IPv4 for client only. */
    uint8_t token[16]; /* Must not be all zero. */
    uint64_t nonce; /* Fresh random nonzero host epoch or client nonce. */
    uint32_t heartbeat_ms; /* 0 defaults to 500; minimum 10. */
    uint32_t timeout_ms; /* 0 defaults to 5000; >= 3 * heartbeat_ms. */
} Uc2mpLanConfig;

/* NULL on invalid input/socket setup failure, leaving lobby untouched.
 * Host requires open lobby containing only local host. Client requires closed
 * lobby with initialized local profile. Client binds an ephemeral UDP port. */
Uc2mpLan *uc2mp_lan_host(Uc2mpLobby *lobby, const Uc2mpLanConfig *config,
                        uint64_t now_ms);
Uc2mpLan *uc2mp_lan_client(Uc2mpLobby *lobby, const Uc2mpLanConfig *config,
                          uint64_t now_ms);
/* now_ms must be monotonic. At most 32 datagrams processed per call. */
Uc2mpLanState uc2mp_lan_poll(Uc2mpLan *lan, uint64_t now_ms);
Uc2mpLanState uc2mp_lan_state(const Uc2mpLan *lan);
uint16_t uc2mp_lan_local_port(const Uc2mpLan *lan);
/* Host modifies local readiness; client sends retried request to host. */
Uc2mpResult uc2mp_lan_ready(Uc2mpLan *lan, int ready);
/* Preparation API only, not wired to Start. Calls use the most recent poll's
 * monotonic time. Pump before these calls, and keep pumping through handshake.
 * Deadline starts at begin, max120s, never renewed by ACK/heartbeat. During an
 * active attempt this bounded grace replaces the ordinary peer timeout.
 * Client cancel is a failure report; the host cancels the entire attempt.
 * The host's load authorization additionally requires fresh authenticated
 * observations of every remote member, not just old prepare ACKs. */
Uc2mpResult uc2mp_lan_prepare_begin(Uc2mpLan *lan, uint32_t timeout_ms);
Uc2mpResult uc2mp_lan_prepare_ack(Uc2mpLan *lan, uint64_t id, Uc2mpPhase phase);
Uc2mpResult uc2mp_lan_prepare_load(Uc2mpLan *lan, uint64_t id);
Uc2mpResult uc2mp_lan_prepare_cancel(Uc2mpLan *lan, uint64_t id);
/* Host: all LOADING receipt ACKs; client: own receipt ACK replicated by host.
 * This is not a perfect distributed barrier: UDP confirmation can be lost.
 * A future loader must pump during preparation and honor cancellation/deadline.
 * No callback into an engine and no implicit start occurs here. */
int uc2mp_lan_prepare_can_block(const Uc2mpLan *lan);
/* Sends best-effort leave/close, closes lobby/socket and frees object. Safe
 * on NULL and clears *lan. Peer timeout handles lost shutdown datagrams. */
void uc2mp_lan_stop(Uc2mpLan **lan);
#endif
