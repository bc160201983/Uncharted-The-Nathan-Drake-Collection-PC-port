/* Headless diagnostic peer for the real UC2MP lobby transport.
 * Windows standalone build:
 * clang -std=c11 -I src src/runtime_uc2mp.c src/runtime_uc2mp_lan.c
 *       tests/uc2mp_lan_peer.c -lws2_32 -lbcrypt -o uc2mp-lan-peer.exe
 *
 * Start host, then client in a second terminal with the SAME random token:
 * uc2mp-lan-peer --host 127.0.0.1 --port 3233 --token <32-hex-digits>
 *                --name Host --id 1 --seconds 120
 * uc2mp-lan-peer --connect 127.0.0.1 --port 3233 --token <32-hex-digits>
 *                --name Peer --id 2 --seconds 30 --ready
 * Can also join a running game's opt-in LAN lobby using its address/token.
 * --prepare-test on BOTH diagnostic processes also tests preparation v2,
 * confirmation and cancellation. No engine/Start call is made.
 * IPv4, trusted LAN, shared bearer token sent in clear. No firewall changes.
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "runtime_uc2mp_lan.h"
#include "runtime_uc2mp_config.h"
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#else
#include <time.h>
#endif

static volatile sig_atomic_t interrupted;
static void interrupt_handler(int signal_number) { (void)signal_number; interrupted = 1; }
static int random_id(uint64_t *value) {
    do {
#ifdef _WIN32
        if (BCryptGenRandom(NULL, (PUCHAR)value, sizeof(*value), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
            return 0;
#else
        FILE *file = fopen("/dev/urandom", "rb");
        if (!file) return 0;
        size_t count = fread(value, 1, sizeof(*value), file);
        fclose(file);
        if (count != sizeof(*value)) return 0;
#endif
    } while (*value < 2);
    return 1;
}
static int get_time(uint64_t *value) {
#ifdef _WIN32
    *value = GetTickCount64();
#else
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
    *value = (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
#endif
    return 1;
}
static void wait_tick(void) {
#ifdef _WIN32
    Sleep(10);
#else
    struct timespec duration = {0, 10000000};
    (void)nanosleep(&duration, NULL);
#endif
}
static const char *state_name(Uc2mpLanState state) {
    switch (state) {
        case UC2MP_LAN_HOSTING: return "hosting";
        case UC2MP_LAN_CONNECTING: return "connecting";
        case UC2MP_LAN_CONNECTED: return "connected";
        case UC2MP_LAN_REJECTED: return "rejected";
        case UC2MP_LAN_TIMED_OUT: return "timed-out";
        case UC2MP_LAN_CLOSED: return "closed";
        case UC2MP_LAN_IO_ERROR: return "io-error";
    }
    return "unknown";
}
static const char *phase_name(Uc2mpPhase phase) {
    switch (phase) {
        case UC2MP_PHASE_LOBBY: return "lobby";
        case UC2MP_PHASE_PREPARING: return "preparing";
        case UC2MP_PHASE_LOADING: return "loading";
    }
    return "invalid";
}
static void usage(FILE *out) {
    fputs("UC2MP lobby diagnostic (no gameplay or GPU)\n"
          "Usage: uc2mp-lan-peer (--host IPv4 | --connect IPv4) --token HEX32\n"
          "       [--port 1..65535] [--name UTF8] [--id 1..UINT64_MAX]\n"
          "       [--seconds 1..86400] [--ready] [--prepare-test]\n"
          "Defaults: port=3233, seconds=60, host id=1, client id=random.\n"
          "Host address is the bind interface; client address is the destination.\n"
          "Use 127.0.0.1 for a same-PC test, an explicit LAN address for LAN.\n"
          "Share a random 128-bit token (32 hex digits); it is sent in clear.\n"
          "No discovery/firewall changes. Ctrl+C closes the lobby connection.\n"
          "Protocol v2 only. --prepare-test is for TWO diagnostic processes;\n"
          "it selects Village/deathmatch, readies both, prepares, ACKs loading,\n"
          "then cancels and returns to lobby. No map/engine/Start is invoked.\n"
          "Its --seconds bound must be 3..120 (default60); incomplete is failure.\n"
          "Exit: 0 clean stop, 2 invalid setup, 3 transport, 4 preparation test.\n", out);
}
static void roster(const Uc2mpLobby *b) {
    printf("roster revision=%" PRIu64 " open=%d members=%u host=%" PRIu64
           " mode=%" PRIu32 " map=%" PRIu32 " capacity=%u\n",
           b->revision, b->open, b->count, b->host_id,
           b->settings.mode_id, b->settings.map_id, b->settings.capacity);
    for (unsigned i = 0; i < b->count; ++i)
        printf("  member id=%" PRIu64 " name=%s ready=%d%s%s\n",
               b->members[i].profile.id, b->members[i].profile.name, b->members[i].ready,
               b->members[i].profile.id == b->host_id ? " host" : "",
               b->members[i].profile.id == b->local.id ? " local" : "");
    printf("preparation phase=%s id=%" PRIu64 " prepared=%u loading=%u\n",
           phase_name(b->preparation.phase),b->preparation.id,
           b->preparation.prepared,b->preparation.loading);
}

typedef struct {
    uint64_t id;
    int started, ack_preparing, ack_loading, confirmed, cancel_requested, returned;
} PrepareTest;

/* No artificial success state: every step waits for the actual replicated
 * model. Client cancellation proves it received the host's loading ACK
 * snapshot; host keeps pumping until that cancellation and subsequent leave. */
static int prepare_step(Uc2mpLan *lan,Uc2mpLobby *b,int hosting,PrepareTest *test,uint32_t budget_ms) {
    if (!b->open) return 0;
    if (test->returned) return hosting && b->count==1 ? 1 : 0;
    if (!test->started && hosting) {
        if (b->count!=2 || !b->members[0].ready || !b->members[1].ready) return 0;
        Uc2mpResult result=uc2mp_lan_prepare_begin(lan,budget_ms);
        if (result!=UC2MP_OK) return -1;
        printf("prepare-test event=begin id=%" PRIu64 "\n",b->preparation.id);
    }
    if (!test->started) {
        if (b->preparation.phase==UC2MP_PHASE_LOBBY) return 0;
        if (b->preparation.phase!=UC2MP_PHASE_PREPARING) return -1;
        test->id=b->preparation.id; test->started=1;
    }
    if (b->preparation.id!=test->id || b->settings.mode_id!=UINT32_C(0xd3b86d60) ||
        b->settings.map_id!=UINT32_C(0xdbaad5e4) || b->settings.capacity!=2 || b->count!=2) return -1;
    if (b->preparation.phase==UC2MP_PHASE_PREPARING) {
        if (!test->ack_preparing) {
            if (uc2mp_lan_prepare_ack(lan,test->id,UC2MP_PHASE_PREPARING)!=UC2MP_OK) return -1;
            test->ack_preparing=1;
            printf("prepare-test event=ack-preparing id=%" PRIu64 "\n",test->id);
        }
        if (hosting && b->preparation.prepared==3) {
            Uc2mpResult result=uc2mp_lan_prepare_load(lan,test->id);
            if (result!=UC2MP_OK && result!=UC2MP_NOT_READY) return -1;
            if (result==UC2MP_OK)
                printf("prepare-test event=authorize-loading id=%" PRIu64 "\n",test->id);
        }
    }
    if (b->preparation.phase==UC2MP_PHASE_LOADING) {
        if (!test->ack_preparing) return -1;
        if (!test->ack_loading) {
            if (uc2mp_lan_prepare_ack(lan,test->id,UC2MP_PHASE_LOADING)!=UC2MP_OK) return -1;
            test->ack_loading=1;
            printf("prepare-test event=ack-loading id=%" PRIu64 "\n",test->id);
        }
        if (!test->confirmed && uc2mp_lan_prepare_can_block(lan)) {
            test->confirmed=1;
            printf("prepare-test event=confirmed-loading id=%" PRIu64 "\n",test->id);
        }
        if (!hosting && test->confirmed && !test->cancel_requested) {
            if (uc2mp_lan_prepare_cancel(lan,test->id)!=UC2MP_OK) return -1;
            test->cancel_requested=1;
            printf("prepare-test event=cancel-request id=%" PRIu64 "\n",test->id);
        }
    } else if (b->preparation.phase==UC2MP_PHASE_LOBBY) {
        if (!test->confirmed || (!hosting && !test->cancel_requested) ||
            b->members[0].ready || b->members[1].ready || b->preparation.prepared || b->preparation.loading) return -1;
        test->returned=1;
        printf("prepare-test result=PASS id=%" PRIu64 " returned-lobby members=2 ready=0\n",test->id);
        return hosting ? 0 : 1;
    }
    return 0;
}
int main(int argc, char **argv) {
    const char *address = NULL, *name = NULL, *token_text = NULL;
    uint64_t port = 3233, seconds = 60, id = 0;
    unsigned seen = 0;
    int hosting = 0, ready = 0, prepare_test = 0;
    enum { ROLE = 1, TOKEN = 2, PORT = 4, NAME = 8, ID = 16, SECONDS = 32, READY = 64, PREPARE_TEST=128 };
    if (argc == 2 && !strcmp(argv[1], "--help")) { usage(stdout); return 0; }
    for (int i = 1; i < argc; ++i) {
        const char *option = argv[i];
        unsigned bit;
        if (!strcmp(option, "--host") || !strcmp(option, "--connect")) bit = ROLE;
        else if (!strcmp(option, "--token")) bit = TOKEN;
        else if (!strcmp(option, "--port")) bit = PORT;
        else if (!strcmp(option, "--name")) bit = NAME;
        else if (!strcmp(option, "--id")) bit = ID;
        else if (!strcmp(option, "--seconds")) bit = SECONDS;
        else if (!strcmp(option, "--ready")) bit = READY;
        else if (!strcmp(option, "--prepare-test")) bit = PREPARE_TEST;
        else { fprintf(stderr, "Unknown option: %s\n", option); usage(stderr); return 2; }
        if (seen & bit) { fprintf(stderr, "Duplicate/conflicting option: %s\n", option); return 2; }
        seen |= bit;
        if (bit == READY) { ready = 1; continue; }
        if (bit == PREPARE_TEST) { prepare_test = 1; continue; }
        if (++i == argc) { fprintf(stderr, "Missing value for %s\n", option); return 2; }
        const char *value = argv[i];
        switch (bit) {
            case ROLE: address = value; hosting = !strcmp(option, "--host"); break;
            case TOKEN: token_text = value; break;
            case NAME: name = value; break;
            case PORT:
                if (!uc2mp_config_u64(value, &port) || !port || port > 65535) {
                    fputs("Invalid --port\n", stderr); return 2;
                }
                break;
            case ID:
                if (!uc2mp_config_u64(value, &id) || !id) { fputs("Invalid --id\n", stderr); return 2; }
                break;
            case SECONDS:
                if (!uc2mp_config_u64(value, &seconds) || !seconds || seconds > 86400) {
                    fputs("Invalid --seconds\n", stderr); return 2;
                }
                break;
        }
    }
    if (prepare_test && (seconds<3 || seconds>120)) {
        fputs("--prepare-test requires --seconds in 3..120\n",stderr); return 2;
    }
    if (prepare_test) ready=1;
    Uc2mpLanConfig c = {0};
    if (!(seen & ROLE) || !(seen & TOKEN) || !uc2mp_config_hex(token_text, c.token, 16)) {
        fputs("Specify one role and a 32-digit hexadecimal token.\n", stderr); usage(stderr); return 2;
    }
    unsigned nonzero = 0;
    for (unsigned i = 0; i < 16; ++i) nonzero |= c.token[i];
    if (!nonzero) { fputs("Token must not be zero.\n", stderr); return 2; }
    if (!random_id(&c.nonce) || (!hosting && !id && !random_id(&id))) {
        fputs("Cannot generate connection identity.\n", stderr); return 2;
    }
    if (!id) id = 1;
    if (!name) name = hosting ? "Diagnostic host" : "Diagnostic peer";
    Uc2mpProfile profile; Uc2mpLobby lobby;
    if (uc2mp_profile(&profile, id, name) != UC2MP_OK || uc2mp_init(&lobby, &profile) != UC2MP_OK) {
        fputs("Invalid name/profile (valid UTF-8, 1..31 bytes, no controls).\n", stderr); return 2;
    }
    c.bind_address = hosting ? address : "0.0.0.0";
    c.host_address = hosting ? NULL : address; c.port = (uint16_t)port;
    uint64_t now;
    if (!get_time(&now)) { fputs("Cannot read monotonic clock.\n", stderr); return 2; }
    if (hosting && uc2mp_open(&lobby) != UC2MP_OK) return 2;
    Uc2mpLan *transport = hosting ? uc2mp_lan_host(&lobby, &c, now) :
                                   uc2mp_lan_client(&lobby, &c, now);
    if (!transport) { fputs("Cannot open transport (check numeric IPv4/bind port).\n", stderr); return 2; }
    if (prepare_test && hosting && uc2mp_configure(&lobby,lobby.local.id,
            (Uc2mpSettings){UINT32_C(0xd3b86d60),UINT32_C(0xdbaad5e4),2, 1, 1, 0, 0})!=UC2MP_OK) {
        uc2mp_lan_stop(&transport); return 2;
    }
    (void)setvbuf(stdout, NULL, _IONBF, 0);
    (void)signal(SIGINT, interrupt_handler); (void)signal(SIGTERM, interrupt_handler);
    printf("UC2MP diagnostic: role=%s address=%s port=%" PRIu64
           " local-port=%u id=%" PRIu64 " name=%s seconds=%" PRIu64 "\n",
           hosting ? "host" : "client", address, port, uc2mp_lan_local_port(transport), id, name, seconds);
    puts("Protocol v2. Lobby/preparation only; no engine or gameplay. Token is not written to this log.");
    uint64_t deadline = now + seconds * 1000, revision = UINT64_MAX;
    Uc2mpLanState previous = UC2MP_LAN_CLOSED;
    int requested_ready = 0, status = 0;
    PrepareTest preparation={0};
    int preparation_done=0;
    for (;;) {
        if (!get_time(&now)) { status = 3; break; }
        if (interrupted || now >= deadline) break;
        Uc2mpLanState state = uc2mp_lan_poll(transport, now);
        if (state != previous) { printf("state=%s\n", state_name(state)); previous = state; }
        if (ready && !requested_ready && (state == UC2MP_LAN_HOSTING || state == UC2MP_LAN_CONNECTED)) {
            if (uc2mp_lan_ready(transport, 1) != UC2MP_OK) { status = 3; break; }
            requested_ready = 1; puts("ready requested=1");
        }
        if (lobby.revision != revision) { roster(&lobby); revision = lobby.revision; }
        if (state == UC2MP_LAN_REJECTED || state == UC2MP_LAN_TIMED_OUT || state == UC2MP_LAN_IO_ERROR) {
            status = 3; break;
        }
        if (state == UC2MP_LAN_CLOSED) break;
        if (prepare_test) {
            int result=prepare_step(transport,&lobby,hosting,&preparation,(uint32_t)(deadline-now));
            if (result<0) { status=4; break; }
            if (result>0) { preparation_done=1; break; }
        }
        wait_tick();
    }
    Uc2mpLanState final_state = uc2mp_lan_state(transport);
    if (final_state == UC2MP_LAN_CONNECTING) status = 3;
    if (prepare_test && !preparation_done) {
        if (!status) status=4;
        printf("prepare-test result=FAIL id=%" PRIu64 " phase=%s incomplete=1\n",
               preparation.id,phase_name(lobby.preparation.phase));
    }
    printf("stopping reason=%s state=%s exit=%d\n",
           preparation_done ? "prepare-test-complete" : interrupted ? "interrupt" : (now >= deadline ? "duration" : "transport"),
           state_name(final_state), status);
    uc2mp_lan_stop(&transport);
    puts("closed socket and local lobby");
    return status;
}
