#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "runtime_uc2mp_net_bridge.h"

static void pause_tick(void) {
#ifdef _WIN32
    Sleep(1);
#else
    struct timespec delay={0,1000000}; nanosleep(&delay,NULL);
#endif
}
static void *foreign_network(void *context) {
    Uc2mpLobby *lobby=context;
    assert(uc2mp_net_open(lobby)==UC2MP_INVALID);
    assert(uc2mp_net_ready(lobby,1)==UC2MP_INVALID);
    uc2mp_net_poll(lobby);
    uc2mp_net_close(lobby);
    runtime_uc2mp_tick_run();
    return NULL;
}

static void env(const char *key, const char *value) {
#ifdef _WIN32
    assert(_putenv_s(key, value ? value : "") == 0);
#else
    assert((value ? setenv(key,value,1) : unsetenv(key)) == 0);
#endif
}
int main(void) {
    uint64_t number=7;
    assert(uc2mp_config_u64("18446744073709551615",&number) && number==UINT64_MAX);
    assert(!uc2mp_config_u64("18446744073709551616",&number) && number==UINT64_MAX);
    assert(!uc2mp_config_u64("-1",&number) && !uc2mp_config_u64("1x",&number));
    assert(!uc2mp_config_u64("",&number) && !uc2mp_config_u64(" 1",&number));
    unsigned char bytes[2]={9,9};
    assert(!uc2mp_config_hex("12q4",bytes,2) && bytes[0]==9 && bytes[1]==9);
    assert(uc2mp_config_hex("c3A9",bytes,2) && bytes[0]==0xc3 && bytes[1]==0xa9);
    env("UC2MP_LOCAL_ID",NULL); env("UC2MP_PLAYER_NAME",NULL); env("UC2MP_PLAYER_UTF8",NULL);
    env("UC2MP_LAN_HOST",NULL); env("UC2MP_LAN_CONNECT",NULL);
    env("UC2MP_LAN_PORT",NULL); env("UC2MP_LAN_TOKEN",NULL);
    Uc2mpProfile profile;
    assert(uc2mp_config_profile(&profile)==UC2MP_OK && profile.id==1 && !strcmp(profile.name,"Joueur"));
    env("UC2MP_PLAYER_UTF8","5a6fc3a9");
    assert(uc2mp_config_profile(&profile)==UC2MP_OK && !strcmp(profile.name,"Zo\xc3\xa9"));
    env("UC2MP_PLAYER_UTF8","410042"); assert(uc2mp_config_profile(&profile)==UC2MP_INVALID);
    env("UC2MP_PLAYER_UTF8","c080"); assert(uc2mp_config_profile(&profile)==UC2MP_INVALID);
    env("UC2MP_PLAYER_UTF8","123"); assert(uc2mp_config_profile(&profile)==UC2MP_INVALID);
    env("UC2MP_PLAYER_UTF8",NULL);
    env("UC2MP_LOCAL_ID","0"); assert(uc2mp_config_profile(&profile)==UC2MP_INVALID);
    env("UC2MP_LOCAL_ID","2"); assert(uc2mp_config_profile(&profile)==UC2MP_OK);
    Uc2mpLobby lobby;
    assert(uc2mp_init(&lobby,&profile)==UC2MP_OK);
    assert(uc2mp_net_open(&lobby)==UC2MP_OK && lobby.count==1);
    uc2mp_net_close(&lobby); assert(!lobby.open);
    assert(!runtime_uc2mp_tick_is_owner());
    env("UC2MP_LAN_CONNECT","127.0.0.1");
    env("UC2MP_LAN_TOKEN","00112233445566778899aabbccddeeff");
    env("UC2MP_LAN_PORT","65536"); assert(uc2mp_net_open(&lobby)==UC2MP_INVALID && !lobby.open);
    assert(!runtime_uc2mp_tick_is_owner());
    Uc2mpProfile host_profile;
    Uc2mpLobby host_lobby;
    assert(uc2mp_profile(&host_profile,1,"Host")==UC2MP_OK);
    assert(uc2mp_init(&host_lobby,&host_profile)==UC2MP_OK);
    assert(uc2mp_open(&host_lobby)==UC2MP_OK);
    Uc2mpLanConfig config={.bind_address="127.0.0.1",.nonce=123};
    assert(uc2mp_config_hex(getenv("UC2MP_LAN_TOKEN"),config.token,sizeof(config.token)));
    Uc2mpLan *host=uc2mp_lan_host(&host_lobby,&config,uc2mp_net_time());
    assert(host);
    char port[8]; snprintf(port,sizeof(port),"%u",uc2mp_lan_local_port(host));
    env("UC2MP_LAN_PORT",port);
    assert(uc2mp_net_open(&lobby)==UC2MP_OK && !lobby.open);
    assert(uc2mp_lan_state(uc2mp_menu_transport)==UC2MP_LAN_CONNECTING);
    /* Establish a real UDP lobby exclusively through the pad tick dispatch,
     * without any menu text queries. */
    uint64_t deadline=uc2mp_net_time()+2000;
    while (uc2mp_lan_state(uc2mp_menu_transport)!=UC2MP_LAN_CONNECTED && uc2mp_net_time()<deadline) {
        runtime_uc2mp_tick_run();
        uc2mp_lan_poll(host,uc2mp_net_time());
        pause_tick();
    }
    assert(lobby.count==2 && host_lobby.count==2 && uc2mp_menu_last_poll);
    uint64_t last_poll=uc2mp_menu_last_poll, revision=lobby.revision;
    pthread_t thread;
    assert(!pthread_create(&thread,NULL,foreign_network,&lobby));
    assert(!pthread_join(thread,NULL));
    assert(uc2mp_menu_transport && lobby.open && lobby.revision==revision);
    assert(uc2mp_menu_last_poll==last_poll && runtime_uc2mp_tick_is_owner());
    assert(uc2mp_net_ready(&lobby,1)==UC2MP_OK);
    deadline=uc2mp_net_time()+2000;
    while ((!host_lobby.members[1].ready || !lobby.members[1].ready) && uc2mp_net_time()<deadline) {
        runtime_uc2mp_tick_run();
        uc2mp_lan_poll(host,uc2mp_net_time());
        pause_tick();
    }
    assert(host_lobby.members[1].ready && lobby.members[1].ready);
    uc2mp_net_close(&lobby); assert(!uc2mp_menu_transport && !lobby.open);
    assert(!runtime_uc2mp_tick_is_owner());
    runtime_uc2mp_tick_run();
    deadline=uc2mp_net_time()+2000;
    while (host_lobby.count!=1 && uc2mp_net_time()<deadline) {
        uc2mp_lan_poll(host,uc2mp_net_time()); pause_tick();
    }
    assert(host_lobby.count==1);
    uc2mp_lan_stop(&host);
    uint64_t first,second;
    assert(uc2mp_net_nonce(&first) && uc2mp_net_nonce(&second) && first && second && first!=second);
    puts("UC2MP config: strict parsing, owner-only tick UDP join/ready/leave and close passed");
    return 0;
}
