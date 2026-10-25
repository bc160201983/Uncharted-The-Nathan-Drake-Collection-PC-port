#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#define ABI __attribute__((sysv_abi))
typedef struct { uint64_t address, size, flags; } Segment;
static unsigned char *image;
/* runtime_services.c restarts the game; here the request is only recorded. */
static int launch_calls, leave_calls;
static const char *launched_map;
int32_t runtime_load_exec(const char *path, const char *const *argv) {
    assert(!strcmp(path,"/app0/big2-ps4_Shipping.elf"));
    assert(!strcmp(argv[0],"eboot.bin") && !strcmp(argv[1],"ispartofuccollection"));
    if (!argv[2]) { ++leave_calls; return 0; } /* leaving a match: the main menu */
    assert(!strcmp(argv[2],"level"));
    launched_map=argv[3];
    /* Warzone (the tested map) brings its sky level, after the map itself. */
    assert(strcmp(launched_map,"warzone-multi") ||
           (!strcmp(argv[4],"level") && !strcmp(argv[5],"sky-multi-warzone") &&
            !strcmp(argv[6],"player") && !argv[7]));
    ++launch_calls;
    return 0;
}
#include "uc2mp_menu_bridge.h"

static void *foreign_menu(void *unused) {
    (void)unused;
    unsigned char value[0x50]={0};
    const uint32_t commands[]={16,26,48,52,57,58,98};
    for (unsigned i=0;i<sizeof(commands)/sizeof(*commands);++i) {
        memcpy(value+0x40,&commands[i],4);
        assert(!strcmp(uc2mp_menu_text(value),""));
        if (commands[i]==48 || commands[i]==52 || commands[i]==57 || commands[i]==58)
            assert(!uc2mp_menu_enabled(value));
    }
    assert(!uc2mp_menu_snapshot());
    uint32_t ready=UINT32_C(0x0e8bb2d7),setting=58;
    uc2mp_menu_toggle_ready(&ready);
    memcpy(value+0x40,&setting,4);
    uc2mp_menu_set_index(value,1);
    assert(uc2mp_menu_index(value)==0 && uc2mp_menu_max(value)==0);
    assert(!strcmp(uc2mp_menu_item_text(value,1,0),""));
    return NULL;
}

int main(void) {
    unsigned char value[0x50] = {0}, event[0x50], original[0x50];
    uint32_t command = 16, opcode = 1, index = 0;
    memcpy(value + 0x40, &command, 4);
    assert(!strcmp(uc2mp_menu_text(value), ""));
    assert(!uc2mp_menu_snapshot());
    Uc2mpProfile local;
    assert(uc2mp_profile(&local, 42, "Tony") == UC2MP_OK);
    assert(uc2mp_init(&uc2mp_menu_lobby, &local) == UC2MP_OK);
    uc2mp_menu_initialized = 1;
    assert(uc2mp_net_open(&uc2mp_menu_lobby) == UC2MP_OK);
    uint64_t initial_revision=uc2mp_menu_lobby.revision;
    pthread_t thread;
    assert(!pthread_create(&thread,NULL,foreign_menu,NULL));
    assert(!pthread_join(thread,NULL));
    assert(uc2mp_menu_lobby.revision==initial_revision && runtime_uc2mp_tick_is_owner());
    assert(!strcmp(uc2mp_menu_text(value), "Tony"));
    command = 98;
    memcpy(value + 0x40, &command, 4);
    assert(!strcmp(uc2mp_menu_text(value),"Salon local - 1/10 joueurs - 0 pr\xc3\xaa" "ts"));
    assert(uc2mp_ready(&uc2mp_menu_lobby,42,1)==UC2MP_OK);
    assert(!strcmp(uc2mp_menu_text(value),"Salon local - 1/10 joueurs - 1 pr\xc3\xaa" "ts"));
    assert(!strcmp(uc2mp_menu_status(&uc2mp_menu_lobby,1,UC2MP_LAN_HOSTING),
                   "Salon LAN - 1/10 joueurs - 1 pr\xc3\xaa" "ts"));
    assert(!strcmp(uc2mp_menu_status(NULL,1,UC2MP_LAN_CONNECTING),"Connexion au salon..."));
    assert(!strcmp(uc2mp_menu_status(NULL,1,UC2MP_LAN_REJECTED),"Connexion refus\xc3\xa9" "e"));
    assert(!strcmp(uc2mp_menu_status(NULL,1,UC2MP_LAN_TIMED_OUT),"Connexion expir\xc3\xa9" "e"));
    assert(!strcmp(uc2mp_menu_status(NULL,1,UC2MP_LAN_IO_ERROR),"Erreur de connexion"));
    assert(!strcmp(uc2mp_menu_status(&uc2mp_menu_lobby,1,UC2MP_LAN_CLOSED),"Salon ferm\xc3\xa9"));
    assert(uc2mp_ready(&uc2mp_menu_lobby,42,0)==UC2MP_OK);
    command = 48;
    memcpy(value + 0x40, &command, 4);
    assert(uc2mp_menu_enabled(value));
    assert(!strcmp(uc2mp_menu_text(value),"Pr\xc3\xaa" "t"));
    assert(!strcmp(uc2mp_menu_item_text(value,0,1),""));
    uint32_t ready_control=UINT32_C(0x0e8bb2d7),wrong_control=UINT32_C(0x70e80076);
    uint64_t before_ready=uc2mp_menu_lobby.revision;
    uc2mp_menu_toggle_ready(NULL);
    uc2mp_menu_toggle_ready(&wrong_control);
    assert(uc2mp_menu_lobby.revision==before_ready && !uc2mp_menu_lobby.members[0].ready);
    uc2mp_menu_toggle_ready(&ready_control);
    assert(uc2mp_menu_lobby.members[0].ready);
    assert(!strcmp(uc2mp_menu_text(value),"Annuler pr\xc3\xaa" "t"));
    uc2mp_menu_toggle_ready(&ready_control);
    assert(!uc2mp_menu_lobby.members[0].ready && uc2mp_menu_lobby.revision==before_ready+2);
    /* A client may occupy any roster slot; never toggle slot0 blindly. */
    Uc2mpProfile other;
    assert(uc2mp_profile(&other,99,"Other")==UC2MP_OK);
    assert(uc2mp_join(&uc2mp_menu_lobby,&other)==UC2MP_OK);
    Uc2mpMember first=uc2mp_menu_lobby.members[0];
    uc2mp_menu_lobby.members[0]=uc2mp_menu_lobby.members[1];
    uc2mp_menu_lobby.members[1]=first;
    uc2mp_menu_toggle_ready(&ready_control);
    assert(!uc2mp_menu_lobby.members[0].ready && uc2mp_menu_lobby.members[1].ready);
    uc2mp_menu_toggle_ready(&ready_control);
    uc2mp_menu_lobby.members[1]=uc2mp_menu_lobby.members[0];
    uc2mp_menu_lobby.members[0]=first;
    assert(uc2mp_leave(&uc2mp_menu_lobby,99)==UC2MP_OK);
    command = 52;
    memcpy(value + 0x40, &command, 4);
    assert(!strcmp(uc2mp_menu_text(value), "Partie personnalis\xc3\xa9" "e"));
    command = 57;
    memcpy(value + 0x40, &command, 4);
    /* The lobby opens like the PS3 custom game: deathmatch on the first map, no "choose" entry. */
    assert(uc2mp_menu_min(value)==0 && uc2mp_menu_max(value)==0);
    assert(uc2mp_menu_index(value)==0 && uc2mp_menu_enabled(value));
    assert(uc2mp_menu_lobby.settings.mode_id==UINT32_C(0xd3b86d60));
    assert(!strcmp(uc2mp_menu_item_text(value,0,0),"Match \xc3\xa0 mort"));
    command = 58;
    memcpy(value + 0x40, &command, 4);
    const int last_map=(int)(sizeof(uc2mp_maps)/sizeof(*uc2mp_maps))-1;
    assert(uc2mp_menu_max(value)==last_map && last_map>=7);
    assert(uc2mp_menu_index(value)==0 && !strcmp(uc2mp_menu_item_text(value,0,0),"Le Village"));
    uc2mp_menu_set_index(value,1);
    assert(uc2mp_menu_lobby.settings.map_id==UINT32_C(0x64b8d44b));
    assert(uc2mp_menu_index(value)==1 && !strcmp(uc2mp_menu_text(value),"Carte"));
    assert(!strcmp(uc2mp_menu_item_text(value,1,0),"La Place"));
    uint64_t revision=uc2mp_menu_lobby.revision;
    uc2mp_menu_set_index(value,-1);
    uc2mp_menu_set_index(value,last_map+1);
    assert(uc2mp_menu_lobby.revision==revision);
    assert(!strcmp(uc2mp_menu_item_text(value,-1,0),""));
    assert(!strcmp(uc2mp_menu_item_text(value,last_map+1,0),""));
    assert(!strcmp(uc2mp_menu_item_text(value,last_map,0),"Le Temple"));
    /* Rule rows: score limit (default 50 kills), time limit (default 20 minutes) and
     * friendly fire (default off). */
    command = 60;
    memcpy(value + 0x40, &command, 4);
    assert(uc2mp_menu_index(value)==1 && !strcmp(uc2mp_menu_item_text(value,1,0),"50 \xc3\xa9liminations"));
    command = 61;
    memcpy(value + 0x40, &command, 4);
    assert(uc2mp_menu_min(value)==0 && uc2mp_menu_max(value)==4 && uc2mp_menu_index(value)==3);
    assert(!strcmp(uc2mp_menu_item_text(value,3,0),"20 minutes") && uc2mp_menu_enabled(value));
    assert(!strcmp(uc2mp_menu_text(value),"rule"));
    uc2mp_menu_set_index(value,4);
    assert(uc2mp_menu_lobby.settings.time_limit==4 && uc2mp_menu_index(value)==4);
    uc2mp_menu_set_index(value,5);
    assert(uc2mp_menu_lobby.settings.time_limit==4);
    command = 62;
    memcpy(value + 0x40, &command, 4);
    assert(uc2mp_menu_max(value)==1 && !strcmp(uc2mp_menu_item_text(value,1,3),"Activ\xc3\xa9"));
    uc2mp_menu_set_index(value,1);
    assert(uc2mp_menu_lobby.settings.friendly_fire==1);
    revision=uc2mp_menu_lobby.revision;
    /* In-match leave confirmation: Quitter (0) restarts on the main menu, Annuler (1) stays. */
    command = 128;
    memcpy(value + 0x40, &command, 4);
    assert(uc2mp_menu_max(value)==1 && uc2mp_menu_min(value)==0 && uc2mp_menu_index(value)==1);
    assert(uc2mp_menu_enabled(value) && !strcmp(uc2mp_menu_text(value),"Voulez-vous vraiment quitter ?"));
    assert(!strcmp(uc2mp_menu_item_text(value,0,0),"Quitter") && !strcmp(uc2mp_menu_item_text(value,1,0),"Annuler"));
    uc2mp_menu_set_index(value,0);            /* a cursor move: no exit */
    uc2mp_menu_set_index(value,1);
    assert(!leave_calls && uc2mp_menu_index(value)==1);
    /* Confirming Annuler asks for every choice's action, never leaves. */
    assert(uc2mp_menu_action(value,1)==UINT32_C(0xfc5ba05d) && uc2mp_menu_action(value,0)==UINT32_C(0xa7613ffe));
    assert(!leave_calls);
    uc2mp_menu_set_index(value,0);            /* confirming Quitter leaves */
    assert(uc2mp_menu_action(value,0)==UINT32_C(0xa7613ffe) && leave_calls==1);
    command = 58;
    memcpy(value + 0x40, &command, 4);
    assert(!strcmp(uc2mp_menu_item_text(value,1,99),""));
    uc2mp_menu_lobby.host_id=99;
    assert(!uc2mp_menu_enabled(value));
    uc2mp_menu_set_index(value,1);
    assert(uc2mp_menu_lobby.revision==revision);
    uc2mp_menu_lobby.host_id=42;
    command=50;
    memcpy(value+0x40,&command,4);
    assert(!uc2mp_menu_enabled(value));
    /* Match launch, local lobby: host ready, deathmatch on Warzone (a catalogue map). */
    uint32_t start_control=UINT32_C(0x70e80076);
    uc2mp_menu_image=value;
    uc2mp_menu_start(&start_control);
    assert(uc2mp_menu_lobby.preparation.phase==UC2MP_PHASE_LOBBY && !launch_calls);
    assert(uc2mp_ready(&uc2mp_menu_lobby,42,1)==UC2MP_OK);
    assert(uc2mp_menu_enabled(value) && !strcmp(uc2mp_menu_text(value),"Lancer la partie"));
    uc2mp_menu_start(&ready_control);
    assert(uc2mp_menu_lobby.preparation.phase==UC2MP_PHASE_LOBBY);
    uc2mp_menu_start(&start_control);
    assert(uc2mp_menu_lobby.preparation.phase==UC2MP_PHASE_LOADING && !launch_calls);
    assert(!uc2mp_menu_enabled(value) && !strcmp(uc2mp_menu_text(value),"Lancement..."));
    uc2mp_launch_step(&uc2mp_menu_lobby);
    assert(launch_calls==1 && !strcmp(launched_map,"warzone-multi"));
    uc2mp_launch_step(&uc2mp_menu_lobby);
    assert(launch_calls==1);
    assert(uc2mp_prepare_cancel(&uc2mp_menu_lobby,42,uc2mp_menu_lobby.preparation.id)==UC2MP_OK);
    uc2mp_launch_started=0;
    uc2mp_menu_image=NULL;
    command = 16;
    memcpy(value + 0x40, &command, 4);
    const char *stable_name = uc2mp_menu_text(value);
    assert(uc2mp_rename(&uc2mp_menu_lobby, "Local") == UC2MP_OK);
    assert(!strcmp(stable_name, "Local"));
    command=26;
    memcpy(value+0x40,&command,4);
    assert(!strcmp(uc2mp_menu_text(value),"Local"));
    for (command = 17; command < 26; ++command) {
        memcpy(value + 0x40, &command, 4);
        assert(!strcmp(uc2mp_menu_text(value), ""));
    }
    memset(event, 0xa5, sizeof(event));
    memcpy(event, &opcode, 4);
    memcpy(event + 0x48, &index, 4);
    memcpy(original, event, sizeof(event));
    uc2mp_roster_query(event);
    const char *label;
    memcpy(&label, event + 0x40, sizeof(label));
    assert(!strcmp(label, "Local"));
    /* The callback owns only the text result, not the other guest fields. */
    memcpy(original + 0x40, &label, sizeof(label));
    assert(!memcmp(original, event, sizeof(event)));
    const uint32_t unrelated[] = {0, 2, 3, 4, 5, 6, 7, UINT32_MAX};
    for (unsigned i = 0; i < sizeof(unrelated) / sizeof(*unrelated); ++i) {
        memcpy(event, &unrelated[i], 4);
        memcpy(original, event, sizeof(event));
        uc2mp_roster_query(event);
        assert(!memcmp(original, event, sizeof(event)));
    }
    memcpy(event, &opcode, 4);
    index = 20;
    memcpy(event + 0x48, &index, 4);
    memcpy(original, event, sizeof(event));
    uc2mp_roster_query(event);
    assert(!memcmp(original, event, sizeof(event)));
    uc2mp_net_close(&uc2mp_menu_lobby);
    assert(!runtime_uc2mp_tick_is_owner());
    command = 48;
    memcpy(value + 0x40, &command, 4);
    before_ready=uc2mp_menu_lobby.revision;
    uc2mp_menu_toggle_ready(&ready_control);
    assert(uc2mp_menu_lobby.revision==before_ready);
    assert(!uc2mp_menu_enabled(value) && !strcmp(uc2mp_menu_text(value),""));
    command = 98;
    memcpy(value + 0x40, &command, 4);
    assert(!strcmp(uc2mp_menu_text(value), ""));
    command = 52;
    memcpy(value + 0x40, &command, 4);
    assert(!strcmp(uc2mp_menu_text(value), ""));
    command = 16;
    memcpy(value + 0x40, &command, 4);
    assert(!strcmp(uc2mp_menu_text(value), ""));
    /* Tiny/mismatched images must be rejected before any memory write. */
    image = value;
#ifdef _WIN32
    assert(!_putenv_s("UNCHARTED_LOCAL_LOBBY", "1"));
#else
    assert(!setenv("UNCHARTED_LOCAL_LOBBY", "1", 1));
#endif
    memcpy(original, value, sizeof(value));
    install_uc2mp_menu_bridge(NULL, 0, sizeof(value));
    assert(!memcmp(original, value, sizeof(value)));
    assert(!uc2mp_menu_image);
    puts("UC2MP: roster labels, guest query field isolation, match launch and rejected image passed");
    return 0;
}
