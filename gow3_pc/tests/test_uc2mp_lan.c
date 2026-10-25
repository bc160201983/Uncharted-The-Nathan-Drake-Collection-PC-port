#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "runtime_uc2mp_lan.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET TestSocket;
#define CLOSE closesocket
static void pause_tick(void) { Sleep(1); }
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
typedef int TestSocket;
#define CLOSE close
static void pause_tick(void) { struct timespec t = {0, 1000000}; nanosleep(&t, NULL); }
#endif
enum { H = 48, JOIN = 1, PING, READY, LEAVE, SNAPSHOT, REJECT, SHUTDOWN, CHALLENGE, PREPARE_ACK, PREPARE_CANCEL };
static uint64_t clock_ms = 100;
static const uint8_t token[16] = {11, 22, 33, 44, 55, 66, 77, 88, 99, 12, 23, 34, 45, 56, 67, 78};
static Uc2mpLanConfig config(uint64_t nonce) {
    Uc2mpLanConfig c = {.bind_address = "127.0.0.1", .host_address = "127.0.0.1",
                        .nonce = nonce, .heartbeat_ms = 10, .timeout_ms = 3000};
    memcpy(c.token, token, 16); return c;
}
static void init(Uc2mpLobby *b, uint64_t id, const char *name, int open) {
    Uc2mpProfile p;
    assert(uc2mp_profile(&p, id, name) == UC2MP_OK);
    assert(uc2mp_init(b, &p) == UC2MP_OK);
    if (open) assert(uc2mp_open(b) == UC2MP_OK);
}
static void pump(Uc2mpLan *host, Uc2mpLan **clients, unsigned count, unsigned ticks) {
    for (unsigned i = 0; i < ticks; ++i) {
        clock_ms += 10;
        for (unsigned j = 0; j < count; ++j) (void)uc2mp_lan_poll(clients[j], clock_ms);
        (void)uc2mp_lan_poll(host, clock_ms);
        pause_tick();
    }
}
static void put64(uint8_t *p, uint64_t v) {
    for (int i = 7; i >= 0; --i) { p[i] = (uint8_t)v; v >>= 8; }
}
static void put32(uint8_t *p,uint32_t v) {
    for (int i=3;i>=0;--i) { p[i]=(uint8_t)v; v>>=8; }
}
static void packet(uint8_t *p, unsigned size, unsigned kind, uint64_t nonce,
                   uint64_t seq, uint64_t epoch) {
    memset(p, 0, size); memcpy(p, "UC2L", 4); p[4] = 3; p[5] = (uint8_t)kind;
    p[6] = (uint8_t)(size >> 8); p[7] = (uint8_t)size;
    memcpy(p + 8, token, 16); put64(p + 24, nonce); put64(p + 32, seq); put64(p + 40, epoch);
}
static void member(uint8_t *p, uint64_t id, const char *name, int ready) {
    put64(p, id); memcpy(p + 8, name, strlen(name) + 1); p[40] = (uint8_t)ready;
}
static TestSocket raw_socket(uint16_t *port) {
    TestSocket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
    assert(s != INVALID_SOCKET);
#else
    assert(s >= 0);
#endif
    struct sockaddr_in address = {0}; address.sin_family = AF_INET;
    assert(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
    assert(!bind(s, (struct sockaddr *)&address, sizeof(address)));
#ifdef _WIN32
    int len = sizeof(address);
#else
    socklen_t len = sizeof(address);
#endif
    assert(!getsockname(s, (struct sockaddr *)&address, &len));
#ifdef _WIN32
    u_long enabled = 1; assert(!ioctlsocket(s, FIONBIO, &enabled));
#else
    assert(fcntl(s, F_SETFL, O_NONBLOCK) != -1);
#endif
    *port = ntohs(address.sin_port); return s;
}
static int raw_read(TestSocket s, uint8_t *p, unsigned capacity) {
    for (unsigned i = 0; i < 30; ++i) {
        int size = (int)recv(s, (char *)p, (int)capacity, 0);
        if (size >= 0) return size;
        pause_tick();
    }
    assert(!"Expected loopback datagram did not arrive within bounded wait"); return -1;
}
static void raw_send(TestSocket s, uint16_t port, uint8_t *data, unsigned n) {
    struct sockaddr_in address = {0}; address.sin_family = AF_INET;
    address.sin_port = htons(port);
    assert(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
    assert(sendto(s, (const char *)data, (int)n, 0, (struct sockaddr *)&address,
                  sizeof(address)) == (int)n);
}
static void poll_raw(Uc2mpLan *l) {
    pause_tick(); clock_ms += 10; (void)uc2mp_lan_poll(l, clock_ms);
}
static void basic_lifecycle(void) {
    Uc2mpLobby hb, cb;
    init(&hb, 1, "Tony", 1); init(&cb, 2, "Client", 0);
    Uc2mpLanConfig hc = config(1001), cc = config(2001);
    Uc2mpLan *host = uc2mp_lan_host(&hb, &hc, clock_ms); assert(host);
    cc.port = uc2mp_lan_local_port(host);
    Uc2mpLan *client = uc2mp_lan_client(&cb, &cc, clock_ms); assert(client);
    pump(host, &client, 1, 12);
    assert(uc2mp_lan_state(client) == UC2MP_LAN_CONNECTED);
    assert(hb.count == 2 && cb.count == 2 && cb.host_id == 1);
    assert(!strcmp(cb.members[0].profile.name, "Tony"));
    assert(uc2mp_configure(&hb, 1, (Uc2mpSettings){7, 9, 4, 1, 1, 0, 0}) == UC2MP_OK);
    pump(host, &client, 1, 4);
    assert(cb.settings.mode_id == 7 && cb.settings.map_id == 9 && cb.settings.capacity == 4);
    /* Match rules travel with the settings and reset readiness like any change. */
    assert(uc2mp_configure(&hb, 1, (Uc2mpSettings){7, 9, 4, 3, 4, 2, 1}) == UC2MP_OK);
    pump(host, &client, 1, 4);
    assert(cb.settings.score_limit == 3 && cb.settings.time_limit == 4 &&
           cb.settings.rounds == 2 && cb.settings.friendly_fire == 1);
    assert(uc2mp_configure(&hb, 1, (Uc2mpSettings){7, 9, 4, 4, 0, 0, 0}) == UC2MP_INVALID);
    assert(uc2mp_configure(&hb, 1, (Uc2mpSettings){7, 9, 4, 1, 1, 0, 0}) == UC2MP_OK);
    pump(host, &client, 1, 4);
    assert(uc2mp_lan_ready(client, 1) == UC2MP_OK);
    assert(uc2mp_lan_ready(host, 1) == UC2MP_OK);
    pump(host, &client, 1, 4);
    assert(hb.members[1].ready && cb.members[1].ready && cb.members[0].ready);
    assert(uc2mp_configure(&hb, 1, (Uc2mpSettings){7, 10, 4, 1, 1, 0, 0}) == UC2MP_OK);
    pump(host, &client, 1, 4);
    assert(!hb.members[1].ready && !cb.members[1].ready && !cb.members[0].ready);
    uc2mp_lan_stop(&client); assert(!client && !cb.open);
    pump(host, NULL, 0, 4); assert(hb.count == 1);
    cc.nonce++;
    client = uc2mp_lan_client(&cb, &cc, clock_ms); assert(client);
    pump(host, &client, 1, 10); assert(hb.count == 2 && cb.count == 2);
    uc2mp_lan_stop(&host);
    pump(NULL, &client, 1, 4); assert(uc2mp_lan_state(client) == UC2MP_LAN_CLOSED && !cb.open);
    uc2mp_lan_stop(&client); uc2mp_lan_stop(&client); uc2mp_lan_stop(NULL);
}
static void reject_and_expire(void) {
    Uc2mpLobby hb, cb[10]; Uc2mpLan *clients[10] = {0};
    init(&hb, 1, "Tony", 1);
    Uc2mpLanConfig hc = config(3001);
    Uc2mpLan *host = uc2mp_lan_host(&hb, &hc, clock_ms); assert(host);
    Uc2mpLanConfig cc = config(4001); cc.port = uc2mp_lan_local_port(host);
    /* ID collision is rejected, never aliases the host. */
    init(&cb[0], 1, "Tony", 0);
    clients[0] = uc2mp_lan_client(&cb[0], &cc, clock_ms); assert(clients[0]);
    pump(host, clients, 1, 10);
    assert(uc2mp_lan_state(clients[0]) == UC2MP_LAN_REJECTED && hb.count == 1);
    uc2mp_lan_stop(&clients[0]);
    for (unsigned i = 0; i < 10; ++i) {
        char name[20]; snprintf(name, sizeof(name), "Player%u", i + 2);
        init(&cb[i], i + 2, name, 0); cc.nonce++;
        clients[i] = uc2mp_lan_client(&cb[i], &cc, clock_ms); assert(clients[i]);
        pump(host, clients, i + 1, 10);
    }
    assert(hb.count == 10 && cb[0].count == 10);
    assert(uc2mp_lan_state(clients[9]) == UC2MP_LAN_REJECTED);
    /* No polls from clients: expiry removes all remote roster members. */
    clock_ms += 3001; (void)uc2mp_lan_poll(host, clock_ms); assert(hb.count == 1);
    for (unsigned i = 0; i < 10; ++i) uc2mp_lan_stop(&clients[i]);
    uc2mp_lan_stop(&host);
    init(&cb[0], 2, "Client", 0); cc.nonce++;
    clients[0] = uc2mp_lan_client(&cb[0], &cc, clock_ms); assert(clients[0]);
    clock_ms += 3001;
    assert(uc2mp_lan_poll(clients[0], clock_ms) == UC2MP_LAN_TIMED_OUT);
    assert(!cb[0].open); uc2mp_lan_stop(&clients[0]);
}
static void hostile_requests(void) {
    Uc2mpLobby hb; init(&hb, 1, "Tony", 1);
    Uc2mpLanConfig hc = config(5001);
    Uc2mpLan *host = uc2mp_lan_host(&hb, &hc, clock_ms); assert(host);
    uint16_t port = uc2mp_lan_local_port(host), unused;
    TestSocket a = raw_socket(&unused), other = raw_socket(&unused);
    uint8_t p[900];
    packet(p, H, LEAVE, 6099, 2, hc.nonce);
    raw_send(a, port, p, H); poll_raw(host);
    packet(p, H + 40, JOIN, 6099, 1, hc.nonce);
    put64(p + H, 2); memcpy(p + H + 8, "Peer", 5);
    raw_send(a, port, p, H + 40); poll_raw(host); assert(hb.count == 1);
    packet(p, H + 40, JOIN, 6001, 10, hc.nonce); put64(p + H, 2); memcpy(p + H + 8, "Peer", 5);
    p[8] ^= 1; raw_send(a, port, p, H + 40); poll_raw(host); assert(hb.count == 1); p[8] ^= 1;
    p[4] = 1; raw_send(a, port, p, H + 40); poll_raw(host); assert(hb.count == 1); p[4] = 3;
    raw_send(a, port, p, H + 39); poll_raw(host); assert(hb.count == 1);
    memset(p + H + 8, 'x', 32); raw_send(a, port, p, H + 40); poll_raw(host); assert(hb.count == 1);
    packet(p, sizeof(p), JOIN, 6001, 10, hc.nonce);
    raw_send(a, port, p, sizeof(p)); poll_raw(host); assert(hb.count == 1);
    packet(p, H + 40, JOIN, 6001, 10, hc.nonce); put64(p + H, 2); memcpy(p + H + 8, "Peer", 5);
    raw_send(a, port, p, H + 40); poll_raw(host); assert(hb.count == 2);
    uint64_t revision = hb.revision;
    raw_send(a, port, p, H + 40); poll_raw(host); assert(hb.count == 2 && hb.revision == revision);
    put64(p + 32, 11); raw_send(a, port, p, H + 40); poll_raw(host);
    assert(hb.count == 2 && hb.revision == revision); /* Fresh retransmission. */
    packet(p, H + 9, READY, 6001, 20, hc.nonce); p[H] = 1; put64(p + H + 1, hb.revision);
    raw_send(other, port, p, H + 9); poll_raw(host); assert(!hb.members[1].ready);
    raw_send(a, port, p, H + 9); poll_raw(host); assert(hb.members[1].ready);
    p[H] = 0; put64(p + 32, 19); raw_send(a, port, p, H + 9); poll_raw(host); assert(hb.members[1].ready);
    /* A ready request based on old settings must not undo configure's reset. */
    uint64_t previous = hb.revision;
    assert(uc2mp_configure(&hb, 1, (Uc2mpSettings){7, 9, 10, 1, 1, 0, 0}) == UC2MP_OK);
    p[H] = 1; put64(p + 32, 21); put64(p + H + 1, previous);
    raw_send(a, port, p, H + 9); poll_raw(host); assert(!hb.members[1].ready);
    packet(p, H, LEAVE, 6001, 22, hc.nonce); raw_send(a, port, p, H); poll_raw(host); assert(hb.count == 1);
    packet(p, H + 40, JOIN, 6001, 23, hc.nonce); put64(p + H, 2); memcpy(p + H + 8, "Peer", 5);
    raw_send(a, port, p, H + 40); poll_raw(host); assert(hb.count == 1); /* Retired nonce. */
    uc2mp_lan_stop(&host); assert(uc2mp_open(&hb) == UC2MP_OK);
    hc.port = port; hc.nonce++;
    host = uc2mp_lan_host(&hb, &hc, clock_ms); assert(host);
    raw_send(a, port, p, H + 40); poll_raw(host); assert(hb.count == 1); /* Old host epoch. */
    uc2mp_lan_stop(&host); CLOSE(a); CLOSE(other);
}
static void hostile_snapshots(void) {
    uint16_t port, unused;
    TestSocket server = raw_socket(&port), foreign = raw_socket(&unused);
    Uc2mpLobby cb; init(&cb, 2, "Client", 0);
    Uc2mpLanConfig cc = config(7001); cc.port = port;
    Uc2mpLan *client = uc2mp_lan_client(&cb, &cc, clock_ms); assert(client);
    (void)uc2mp_lan_poll(client, clock_ms);
    uint16_t client_port = uc2mp_lan_local_port(client);
    uint8_t p[501];
    /* Deliberately discard the first JOIN: a fresh request must be retried. */
    assert(raw_read(server, p, sizeof(p)) == H + 40 && p[5] == JOIN && p[39] == 1);
    poll_raw(client);
    assert(raw_read(server, p, sizeof(p)) == H + 40 && p[5] == JOIN && p[39] == 2);
    packet(p, H, CHALLENGE, cc.nonce, 2, 8001);
    raw_send(server, client_port, p, H); poll_raw(client);
    assert(raw_read(server, p, sizeof(p)) == H + 40 && p[5] == JOIN && p[39] == 3);
    assert(p[46] == (8001 >> 8) && p[47] == (8001 & 255));
    unsigned n = H + 30 + 2 * 41 + 17;
    packet(p, n, SNAPSHOT, cc.nonce, 1, 8001);
    put64(p + H, 1); put64(p + H + 8, 1); p[H + 24] = 10; p[H + 25] = 2;
    member(p + H + 30, 1, "Tony", 0); member(p + H + 30 + 41, 2, "Client", 0);
    raw_send(foreign, client_port, p, n); poll_raw(client); assert(!cb.open);
    raw_send(server, client_port, p, n); poll_raw(client); assert(cb.open && cb.count == 2);
    put64(p + 32, 2); put64(p + H + 30 + 41, 1); /* Duplicate IDs. */
    raw_send(server, client_port, p, n); poll_raw(client); assert(cb.revision == 1);
    put64(p + H + 30 + 41, 2); p[H + 24] = 1;
    raw_send(server, client_port, p, n); poll_raw(client); assert(cb.revision == 1);
    p[H + 24] = 10; p[H + 30 + 41 + 40] = 2;
    raw_send(server, client_port, p, n); poll_raw(client); assert(cb.revision == 1);
    p[H + 30 + 41 + 40] = 0; p[H + 27] = 9; /* Out-of-range time limit index. */
    put64(p + 32, 3); raw_send(server, client_port, p, n); poll_raw(client); assert(cb.revision == 1);
    p[H + 27] = 0; put64(p + 32, 2);
    p[H + 30 + 41 + 40] = 1; put64(p + 32, 3);
    raw_send(server, client_port, p, n); poll_raw(client); assert(cb.revision == 3 && cb.members[1].ready);
    put64(p + 32, 2); p[H + 30 + 41 + 40] = 0;
    raw_send(server, client_port, p, n); poll_raw(client); assert(cb.revision == 3 && cb.members[1].ready);
    packet(p, H, SHUTDOWN, cc.nonce, 4, 8000);
    raw_send(server, client_port, p, H); poll_raw(client); assert(cb.open);
    clock_ms += 3001;
    assert(uc2mp_lan_poll(client, clock_ms) == UC2MP_LAN_TIMED_OUT && !cb.open);
    uc2mp_lan_stop(&client); CLOSE(server); CLOSE(foreign);
}
static void invalid_setup(void) {
    Uc2mpLobby b; init(&b, 1, "Tony", 1);
    Uc2mpLanConfig c = config(9001); Uc2mpLobby before = b;
    c.nonce = 0; assert(!uc2mp_lan_host(&b, &c, clock_ms)); c.nonce = 9001;
    memset(c.token, 0, 16); assert(!uc2mp_lan_host(&b, &c, clock_ms)); memcpy(c.token, token, 16);
    c.bind_address = "localhost"; assert(!uc2mp_lan_host(&b, &c, clock_ms)); c.bind_address = "127.0.0.1";
    c.timeout_ms = 20; assert(!uc2mp_lan_host(&b, &c, clock_ms)); c.timeout_ms = 3000;
    assert(!memcmp(&b, &before, sizeof(b)));
    Uc2mpLan *a = uc2mp_lan_host(&b, &c, clock_ms); assert(a);
    c.port = uc2mp_lan_local_port(a); assert(!uc2mp_lan_host(&b, &c, clock_ms));
    uc2mp_lan_stop(&a);
}
static void prepare_lifecycle(void) {
    Uc2mpLobby hb,cb; init(&hb,1,"Tony",1); init(&cb,2,"Peer",0);
    Uc2mpLanConfig hc=config(10001),cc=config(10002);
    Uc2mpLan *host=uc2mp_lan_host(&hb,&hc,clock_ms); assert(host);
    cc.port=uc2mp_lan_local_port(host);
    Uc2mpLan *client=uc2mp_lan_client(&cb,&cc,clock_ms); assert(client);
    pump(host,&client,1,10);
    assert(uc2mp_configure(&hb,1,(Uc2mpSettings){0xd3b86d60,0xdbaad5e4,10, 1, 1, 0, 0})==UC2MP_OK);
    pump(host,&client,1,4);
    assert(uc2mp_lan_ready(host,1)==UC2MP_OK && uc2mp_lan_ready(client,1)==UC2MP_OK);
    pump(host,&client,1,4);
    assert(uc2mp_lan_prepare_begin(client,120000)==UC2MP_NOT_HOST);
    assert(uc2mp_lan_prepare_begin(host,120000)==UC2MP_OK);
    uint64_t id=hb.preparation.id, deadline=hb.preparation.deadline_ms;
    assert(uc2mp_lan_prepare_ack(host,id,UC2MP_PHASE_PREPARING)==UC2MP_OK);
    assert(uc2mp_lan_prepare_load(host,id)==UC2MP_NOT_READY);
    pump(host,&client,1,4);
    assert(cb.preparation.id==id && cb.preparation.phase==UC2MP_PHASE_PREPARING);
    assert(uc2mp_lan_ready(client,0)==UC2MP_BUSY);
    assert(uc2mp_lan_prepare_ack(client,id,UC2MP_PHASE_PREPARING)==UC2MP_OK);
    pump(host,&client,1,4);
    assert(hb.preparation.prepared==3 && cb.preparation.prepared==3);
    assert(uc2mp_lan_prepare_load(client,id)==UC2MP_NOT_HOST);
    assert(uc2mp_lan_prepare_load(host,id)==UC2MP_OK && hb.preparation.deadline_ms==deadline);
    assert(!uc2mp_lan_prepare_can_block(host));
    assert(uc2mp_lan_prepare_ack(host,id,UC2MP_PHASE_LOADING)==UC2MP_OK);
    pump(host,&client,1,4);
    assert(!uc2mp_lan_prepare_can_block(client));
    assert(uc2mp_lan_prepare_ack(client,id,UC2MP_PHASE_LOADING)==UC2MP_OK);
    pump(host,&client,1,4);
    assert(uc2mp_lan_prepare_can_block(host) && uc2mp_lan_prepare_can_block(client));
    uint64_t client_deadline=cb.preparation.deadline_ms;
    clock_ms+=10000; /* Both game threads blocked well beyond ordinary 3s timeout. */
    assert(uc2mp_lan_poll(host,clock_ms)==UC2MP_LAN_HOSTING && hb.count==2);
    assert(uc2mp_lan_poll(client,clock_ms)==UC2MP_LAN_CONNECTED && cb.count==2);
    pump(host,&client,1,4);
    assert(hb.preparation.deadline_ms==deadline && cb.preparation.deadline_ms<=client_deadline);
    assert(uc2mp_lan_prepare_cancel(client,id)==UC2MP_OK);
    pump(host,&client,1,4);
    assert(hb.preparation.phase==UC2MP_PHASE_LOBBY && cb.preparation.phase==UC2MP_PHASE_LOBBY);
    assert(!hb.members[0].ready && !cb.members[1].ready);
    assert(uc2mp_lan_ready(host,1)==UC2MP_OK && uc2mp_lan_ready(client,1)==UC2MP_OK);
    pump(host,&client,1,4);
    assert(uc2mp_lan_prepare_begin(host,120000)==UC2MP_OK && hb.preparation.id==id+1);
    assert(uc2mp_lan_prepare_ack(host,id+1,UC2MP_PHASE_PREPARING)==UC2MP_OK);
    pump(host,&client,1,4);
    assert(uc2mp_lan_prepare_ack(client,id,UC2MP_PHASE_PREPARING)==UC2MP_INVALID);
    assert(uc2mp_lan_prepare_load(host,id+1)==UC2MP_NOT_READY);
    clock_ms=hb.preparation.deadline_ms;
    uc2mp_lan_poll(host,clock_ms);
    assert(hb.preparation.phase==UC2MP_PHASE_LOBBY && !uc2mp_lan_prepare_can_block(host));
    clock_ms=cb.preparation.deadline_ms+1;
    assert(uc2mp_lan_poll(client,clock_ms)==UC2MP_LAN_TIMED_OUT && !cb.open);
    uc2mp_lan_stop(&client); uc2mp_lan_stop(&host);
    /* The client retains an old attempt tombstone, while a restarted host
     * process starts at ID0. Fresh nonce/epoch resets this namespace safely. */
    assert(cb.preparation.id==id+1);
    init(&hb,1,"Tony",1); hc.nonce++; hc.port=cc.port; cc.nonce++;
    host=uc2mp_lan_host(&hb,&hc,clock_ms); assert(host);
    client=uc2mp_lan_client(&cb,&cc,clock_ms); assert(client);
    pump(host,&client,1,10);
    assert(cb.open && cb.preparation.id==0 && hb.count==2);
    uc2mp_lan_stop(&client); uc2mp_lan_stop(&host);
}
static void prepare_host_packets(void) {
    Uc2mpLobby hb; init(&hb,1,"Tony",1);
    Uc2mpLanConfig hc=config(11001);
    Uc2mpLan *host=uc2mp_lan_host(&hb,&hc,clock_ms); assert(host);
    uint16_t port=uc2mp_lan_local_port(host),unused;
    TestSocket peer=raw_socket(&unused),foreign=raw_socket(&unused);
    uint8_t p[H+40];
    packet(p,H+40,JOIN,11002,1,hc.nonce); put64(p+H,2); memcpy(p+H+8,"Peer",5);
    raw_send(peer,port,p,H+40); poll_raw(host); assert(hb.count==2);
    assert(uc2mp_configure(&hb,1,(Uc2mpSettings){0xd3b86d60,0xdbaad5e4,10, 1, 1, 0, 0})==UC2MP_OK);
    assert(uc2mp_ready(&hb,1,1)==UC2MP_OK && uc2mp_ready(&hb,2,1)==UC2MP_OK);
    assert(uc2mp_lan_prepare_begin(host,120000)==UC2MP_OK);
    uint64_t id=hb.preparation.id,rev=hb.revision;
    packet(p,H+9,PREPARE_ACK,11002,10,hc.nonce); put64(p+H,id+1); p[H+8]=UC2MP_PHASE_PREPARING;
    raw_send(peer,port,p,H+9); poll_raw(host); assert(hb.revision==rev);
    put64(p+H,id); p[H+8]=99;
    raw_send(peer,port,p,H+9); poll_raw(host); assert(hb.revision==rev);
    p[H+8]=UC2MP_PHASE_PREPARING;
    raw_send(foreign,port,p,H+9); poll_raw(host); assert(hb.revision==rev);
    put64(p+40,hc.nonce-1); raw_send(peer,port,p,H+9); poll_raw(host); assert(hb.revision==rev);
    put64(p+40,hc.nonce); raw_send(peer,port,p,H+8); poll_raw(host); assert(hb.revision==rev);
    put64(p+32,4); raw_send(peer,port,p,H+9); poll_raw(host); assert(hb.preparation.prepared==2);
    rev=hb.revision;
    raw_send(peer,port,p,H+9); poll_raw(host); assert(hb.revision==rev);
    put64(p+32,5); raw_send(peer,port,p,H+9); poll_raw(host); assert(hb.revision==rev); /* Lost reply: fresh retry. */
    packet(p,H+8,PREPARE_CANCEL,11002,3,hc.nonce); put64(p+H,id);
    raw_send(peer,port,p,H+8); poll_raw(host); assert(hb.preparation.phase==UC2MP_PHASE_PREPARING);
    assert(uc2mp_lan_prepare_ack(host,id,UC2MP_PHASE_PREPARING)==UC2MP_OK);
    clock_ms+=4000;
    uc2mp_lan_poll(host,clock_ms);
    assert(hb.count==2 && uc2mp_lan_prepare_load(host,id)==UC2MP_NOT_READY); /* Old ACK, absent peer. */
    packet(p,H,PING,11002,6,hc.nonce); raw_send(peer,port,p,H); poll_raw(host);
    assert(uc2mp_lan_prepare_load(host,id)==UC2MP_OK);
    packet(p,H+9,PREPARE_ACK,11002,7,hc.nonce); put64(p+H,id); p[H+8]=UC2MP_PHASE_PREPARING;
    raw_send(peer,port,p,H+9); poll_raw(host); assert(!hb.preparation.loading);
    p[H+8]=UC2MP_PHASE_LOADING; put64(p+32,8);
    raw_send(peer,port,p,H+9); poll_raw(host); assert(hb.preparation.loading==2);
    assert(uc2mp_lan_prepare_cancel(host,id)==UC2MP_OK);
    assert(uc2mp_ready(&hb,1,1)==UC2MP_OK && uc2mp_ready(&hb,2,1)==UC2MP_OK);
    assert(uc2mp_lan_prepare_begin(host,120000)==UC2MP_OK);
    put64(p+32,9); raw_send(peer,port,p,H+9); poll_raw(host); assert(!hb.preparation.prepared);
    packet(p,H,LEAVE,11002,10,hc.nonce); raw_send(peer,port,p,H); poll_raw(host);
    assert(hb.count==1 && hb.preparation.phase==UC2MP_PHASE_LOBBY && !hb.members[0].ready);
    uc2mp_lan_stop(&host); CLOSE(peer); CLOSE(foreign);
}
static void prepare_client_packets(void) {
    uint16_t port;
    TestSocket server=raw_socket(&port);
    Uc2mpLobby cb; init(&cb,2,"Client",0);
    Uc2mpLanConfig cc=config(12002); cc.port=port;
    Uc2mpLan *client=uc2mp_lan_client(&cb,&cc,clock_ms); assert(client);
    uint16_t client_port=uc2mp_lan_local_port(client);
    uint8_t p[501],ack[501],retry[501];
    uc2mp_lan_poll(client,clock_ms);
    assert(raw_read(server,p,sizeof(p))==H+40);
    packet(p,H,CHALLENGE,cc.nonce,1,12001);
    raw_send(server,client_port,p,H); poll_raw(client);
    assert(raw_read(server,p,sizeof(p))==H+40);
    unsigned n=H+30+2*41+17;
    packet(p,n,SNAPSHOT,cc.nonce,1,12001);
    put64(p+H,1); put64(p+H+8,1); p[H+24]=10; p[H+25]=2;
    put32(p+H+16,0xd3b86d60); put32(p+H+20,0xdbaad5e4);
    member(p+H+30,1,"Tony",1); member(p+H+30+41,2,"Client",1);
    raw_send(server,client_port,p,n); poll_raw(client); assert(cb.open);
    uint8_t *prep=p+n-17;
    put64(p+32,2); prep[0]=UC2MP_PHASE_PREPARING; put64(prep+1,1); put32(prep+9,120000);
    prep[13]=0xff; raw_send(server,client_port,p,n); poll_raw(client); assert(cb.revision==1);
    prep[13]=0; prep[0]=99; raw_send(server,client_port,p,n); poll_raw(client); assert(cb.revision==1);
    prep[0]=UC2MP_PHASE_PREPARING; put32(prep+9,120001);
    raw_send(server,client_port,p,n); poll_raw(client); assert(cb.revision==1);
    put32(prep+9,0); raw_send(server,client_port,p,n); poll_raw(client); assert(cb.revision==1);
    put32(prep+9,120000); put32(p+H+20,123);
    raw_send(server,client_port,p,n); poll_raw(client); assert(cb.revision==1);
    put32(p+H+20,0xdbaad5e4);
    raw_send(server,client_port,p,n); poll_raw(client); assert(cb.preparation.phase==UC2MP_PHASE_PREPARING);
    uint64_t deadline=cb.preparation.deadline_ms;
    assert(uc2mp_lan_prepare_ack(client,1,UC2MP_PHASE_PREPARING)==UC2MP_OK);
    poll_raw(client);
    unsigned tries=0;
    do { assert(++tries<32); raw_read(server,ack,sizeof(ack)); } while (ack[5]!=PREPARE_ACK);
    /* Drop this actual UDP ACK; no acknowledgement snapshot is returned. */
    poll_raw(client);
    tries=0;
    do { assert(++tries<32); raw_read(server,retry,sizeof(retry)); } while (retry[5]!=PREPARE_ACK);
    assert(memcmp(ack+32,retry+32,8)); /* Retried with a new sequence. */
    assert(ack[H+8]==UC2MP_PHASE_PREPARING && retry[H+8]==UC2MP_PHASE_PREPARING);
    /* A delayed, equal-revision heartbeat cannot renew the loading budget. */
    clock_ms+=1000;
    raw_send(server,client_port,p,n); poll_raw(client); assert(cb.preparation.deadline_ms==deadline);
    put64(p+32,3); prep[0]=UC2MP_PHASE_LOADING; prep[14]=1;
    raw_send(server,client_port,p,n); poll_raw(client); assert(cb.revision==2); /* Missing prepare ACK. */
    prep[14]=3; put64(p+32,4);
    raw_send(server,client_port,p,n); poll_raw(client); assert(cb.preparation.phase==UC2MP_PHASE_LOADING);
    assert(!uc2mp_lan_prepare_can_block(client));
    prep[16]=2; put64(p+32,5);
    raw_send(server,client_port,p,n); poll_raw(client); assert(uc2mp_lan_prepare_can_block(client));
    /* Reordered phase regression and un-ACK are rejected even at a new revision. */
    prep[0]=UC2MP_PHASE_PREPARING; prep[16]=0; put64(p+32,6);
    raw_send(server,client_port,p,n); poll_raw(client); assert(cb.revision==5);
    prep[0]=UC2MP_PHASE_LOADING;
    raw_send(server,client_port,p,n); poll_raw(client); assert(cb.revision==5);
    /* Host cancellation, followed by a delayed old attempt, never re-arms it. */
    prep[0]=UC2MP_PHASE_LOBBY; prep[14]=0; put32(prep+9,0);
    raw_send(server,client_port,p,n); poll_raw(client); assert(cb.preparation.phase==UC2MP_PHASE_LOBBY);
    prep[0]=UC2MP_PHASE_PREPARING; put32(prep+9,120000); put64(p+32,7);
    raw_send(server,client_port,p,n); poll_raw(client); assert(cb.revision==6);
    put64(prep+1,2); put64(p+32,8);
    raw_send(server,client_port,p,n); poll_raw(client); assert(cb.preparation.id==2);
    deadline=cb.preparation.deadline_ms;
    clock_ms=deadline;
    raw_send(server,client_port,p,n); /* Fresh heartbeat must not evade deadline. */
    assert(uc2mp_lan_poll(client,clock_ms)==UC2MP_LAN_TIMED_OUT && !cb.open);
    uc2mp_lan_stop(&client); CLOSE(server);
}
int main(void) {
#ifdef _WIN32
    WSADATA data; assert(!WSAStartup(MAKEWORD(2, 2), &data));
#endif
    basic_lifecycle(); reject_and_expire(); hostile_requests(); hostile_snapshots(); invalid_setup();
    prepare_lifecycle(); prepare_host_packets(); prepare_client_packets();
#ifdef _WIN32
    WSACleanup();
#endif
    puts("uc2mp LAN tests: lifecycle, roster/settings/ready, capacity, identity collision, retries,");
    puts("expiry, malformed/oversize packets, endpoint/epoch validation, stale sequences: PASS");
    puts("preparation: explicit ACK barrier, dropped ACK retry, malformed/reordered phases,");
    puts("fixed deadline, blocking-load grace, cancel/leave and fresh-epoch reconnect: PASS");
    return 0;
}
