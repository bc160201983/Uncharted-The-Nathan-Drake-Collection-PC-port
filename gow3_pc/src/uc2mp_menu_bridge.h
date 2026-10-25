/* Opt-in Uncharted 2 v1.02 menu entry adapter. Included by probe.c after
 * Segment/mapped are defined. Addresses verified against the reconstructed
 * big2-ps4_Shipping ELF; signatures must match before any guest write. */
#ifndef UC2MP_MENU_BRIDGE_H
#define UC2MP_MENU_BRIDGE_H
#include "runtime_uc2mp.h"
#include "runtime_uc2mp_config.h"
#include "runtime_uc2mp_catalog.h"
#include "runtime_uc2mp_net_bridge.h"
#include "runtime_uc2mp_diag.h"
#include "uc2mp_extracted_spawn_catalog.h"
#include "uc2mp_map_levels.h"
#include "uc2mp_match_info.h"
#include "uc2mp_texts.h"

static Uc2mpLobby uc2mp_menu_lobby;
static int uc2mp_launch_possible(const Uc2mpLobby *lobby);
static unsigned char *uc2mp_menu_image;
static int uc2mp_menu_initialized;
static unsigned uc2mp_roster_logged;

/* Command98 is the original multiplayer page title (query1 result event+40,
 * handler65ecc0). Stable same-thread storage; no guest object is synthesized. */
static const char *uc2mp_menu_status(const Uc2mpLobby *lobby, int networked,
                                   Uc2mpLanState state) {
    static char label[128];
    if (networked) {
        switch (state) {
            case UC2MP_LAN_CONNECTING: return "Connexion au salon...";
            case UC2MP_LAN_REJECTED: return "Connexion refus\xc3\xa9" "e";
            case UC2MP_LAN_TIMED_OUT: return "Connexion expir\xc3\xa9" "e";
            case UC2MP_LAN_IO_ERROR: return "Erreur de connexion";
            case UC2MP_LAN_CLOSED: return "Salon ferm\xc3\xa9";
            case UC2MP_LAN_HOSTING:
            case UC2MP_LAN_CONNECTED: break;
            default: return "";
        }
    }
    if (!lobby || !lobby->open) return "";
    unsigned ready=0;
    for (unsigned i=0;i<lobby->count && i<UC2MP_MAX_MEMBERS;++i)
        ready += lobby->members[i].ready != 0;
    snprintf(label,sizeof(label),"Salon %s - %u/%u joueurs - %u pr\xc3\xaa" "ts",
             networked ? "LAN" : "local",lobby->count,lobby->settings.capacity,ready);
    return label;
}

static uint32_t uc2mp_menu_command(const void *value) {
    uint32_t command;
    memcpy(&command,(const unsigned char *)value+0x40,sizeof(command));
    return command;
}
static int uc2mp_menu_setting(unsigned command) { return command==57 || command==58; }
/* PAGE_LOBBY's rule rows (runtime_uc2mp_catalog.h uc2mp_rule_choices). */
static uint8_t *uc2mp_menu_rule_field(Uc2mpSettings *settings, unsigned command) {
    switch (command) {
        case 60: return &settings->score_limit;
        case 61: return &settings->time_limit;
        case 62: return &settings->friendly_fire;
        case 63: return &settings->rounds;
        default: return NULL;
    }
}
static int uc2mp_menu_rule(unsigned command) { return command>=60 && command<=63; }
/* The rows' own labels (MENU_MULTIPLAYER_CUSTOM*), from the game text in its language;
 * uc2mp_text_bridge.h supplies the two the Collection lost. */
static uint32_t uc2mp_menu_rule_label(unsigned command) {
    switch (command) {
        case 60: return UINT32_C(0x86da827f); /* MENU_MULTIPLAYER_CUSTOMSCORELIMIT */
        case 61: return UINT32_C(0x07774a0d); /* MENU_MULTIPLAYER_CUSTOMTIMELIMIT */
        case 62: return UINT32_C(0x9b7aa1a9); /* MENU_MULTIPLAYER_CUSTOMFRIENDLYFIRE */
        default: return UINT32_C(0x7f532e93); /* MENU_MULTIPLAYER_CUSTOMROUND */
    }
}

static const Uc2mpMember *uc2mp_menu_local_member(void) {
    if (!runtime_uc2mp_tick_is_owner()) return NULL;
    if (!uc2mp_menu_initialized || !uc2mp_menu_lobby.open) return NULL;
    if (uc2mp_menu_transport) {
        Uc2mpLanState state=uc2mp_lan_state(uc2mp_menu_transport);
        if (state!=UC2MP_LAN_HOSTING && state!=UC2MP_LAN_CONNECTED) return NULL;
    }
    for (unsigned i=0;i<uc2mp_menu_lobby.count && i<UC2MP_MAX_MEMBERS;++i)
        if (uc2mp_menu_lobby.members[i].profile.id==uc2mp_menu_lobby.local.id)
            return &uc2mp_menu_lobby.members[i];
    return NULL;
}

/* The original Invitations control is unused by manual LAN connections.
 * Its clicked-handler ABI is the same as entry/leave: SysV RDI=event,
 * event+0 contains control ID. This only changes the native PC local member. */
static ABI void uc2mp_menu_toggle_ready(const void *event) {
    UC2MP_THREAD_OBSERVE(UC2MP_DIAG_MENU_READY);
    if (!event) return;
    uint32_t control;
    memcpy(&control,event,sizeof(control));
    if (control!=UINT32_C(0x0e8bb2d7)) return;
    const Uc2mpMember *member=uc2mp_menu_local_member();
    if (!member) return;
    int ready=!member->ready;
    Uc2mpResult result=uc2mp_net_ready(&uc2mp_menu_lobby,ready);
    printf("UC2MP: ready requested=%d player=%s result=%s\n",
           ready,uc2mp_menu_lobby.local.name,uc2mp_result_text(result));
    fflush(stdout);
}

/* MenuData index queries: op4=current, op5=max, op6=min, op9=set.
 * Verified in656550's jump table and wrappers65f940/65fc20/65fb20/65fa30. */
/* PAGE_MULTIPLAYER_LEAVEGAME_CONFIRM (command 128, from the in-match pause page): the original
 * handler 65e3b0 is a two-choice list, 0 "Quitter" / 1 "Annuler", under the question
 * "Voulez-vous vraiment quitter ?", but the original dispatcher answers nothing offline.
 * Choosing Quitter restarts the game on its main menu, as a normal start (LoadExec). */
enum { UC2MP_LEAVE_COMMAND = 128 };
static int uc2mp_leave_choice = 1;
static const char *uc2mp_menu_game_text(uint32_t id, const char *fallback) {
    if (!uc2mp_menu_image) return fallback;
    typedef const char *(ABI *Lookup)(void *, uint32_t);
    const char *text=((Lookup)(uc2mp_menu_image+0xcddc00))(uc2mp_menu_image+0x1066660,id);
    return text ? text : fallback;
}
static void uc2mp_leave_match(void) {
    puts("UC2MP: leaving the match; restarting the game on its main menu");
    fflush(stdout);
    const char *const argv[]={"eboot.bin","ispartofuccollection",NULL};
    runtime_load_exec("/app0/big2-ps4_Shipping.elf",argv);
    fputs("UC2MP: leaving the match failed: no launcher handoff\n",stderr);
}

/* Request 0x12 (slot 100b4e8, wrapper 65ff30: value, choice -> action id), what the original
 * handler answers with a7613ffe for Quitter and fc5ba05d for Annuler. The dialog sends it on
 * confirmation only; set_index (request 9) also follows cursor moves, so it just tracks. */
static ABI uint32_t uc2mp_menu_action(const void *value, int index) {
    uint32_t command=uc2mp_menu_command(value);
    if (command==UC2MP_LEAVE_COMMAND) {
        printf("UC2MP: leave confirmation action query choice=%d current=%d\n",index,uc2mp_leave_choice);
        fflush(stdout);
        /* Asked only when the choice is confirmed (not on cursor moves), for each choice. */
        if (index==0 && uc2mp_leave_choice==0) uc2mp_leave_match();
        return index ? UINT32_C(0xfc5ba05d) : UINT32_C(0xa7613ffe);
    }
    typedef uint32_t (ABI *Original)(const void *,int);
    return ((Original)(uc2mp_menu_image+0x65ff30))(value,index);
}

static ABI int uc2mp_menu_index(const void *value) {
    uint32_t command=uc2mp_menu_command(value);
    if (command==UC2MP_LEAVE_COMMAND) return uc2mp_leave_choice;
    if (uc2mp_menu_rule(command))
        return runtime_uc2mp_tick_is_owner() ? *uc2mp_menu_rule_field(&uc2mp_menu_lobby.settings,command) : 0;
    if (uc2mp_menu_setting(command)) {
        if (!runtime_uc2mp_tick_is_owner()) return 0;
        uint32_t id=command==57 ? uc2mp_menu_lobby.settings.mode_id : uc2mp_menu_lobby.settings.map_id;
        size_t count; const Uc2mpCatalogEntry *entries=uc2mp_catalog(command,&count);
        for (size_t i=0;i<count;++i) if(entries[i].id==id) return (int)i;
        return 0;
    }
    typedef int (ABI *Original)(const void *);
    return ((Original)(uc2mp_menu_image+0x65f940))(value);
}
static ABI int uc2mp_menu_min(const void *value) {
    uint32_t command=uc2mp_menu_command(value);
    if (uc2mp_menu_setting(command) || uc2mp_menu_rule(command) || command==UC2MP_LEAVE_COMMAND) return 0;
    typedef int (ABI *Original)(const void *);
    return ((Original)(uc2mp_menu_image+0x65fb20))(value);
}
static ABI int uc2mp_menu_max(const void *value) {
    uint32_t command=uc2mp_menu_command(value);
    if (command==UC2MP_LEAVE_COMMAND) return 1;
    if (uc2mp_menu_rule(command)) {
        size_t count; uc2mp_rule_choices(command,&count);
        return runtime_uc2mp_tick_is_owner() && uc2mp_menu_lobby.open ? (int)count-1 : 0;
    }
    if (uc2mp_menu_setting(command)) {
        if (!runtime_uc2mp_tick_is_owner()) return 0;
        size_t count; uc2mp_catalog(command,&count);
        return uc2mp_menu_lobby.open ? (int)count-1 : 0;
    }
    typedef int (ABI *Original)(const void *);
    return ((Original)(uc2mp_menu_image+0x65fc20))(value);
}
static ABI void uc2mp_menu_set_index(const void *value, int index) {
    UC2MP_THREAD_OBSERVE(UC2MP_DIAG_MENU_SET_INDEX);
    uint32_t command=uc2mp_menu_command(value);
    if (command==UC2MP_LEAVE_COMMAND) {
        printf("UC2MP: leave confirmation choice=%d\n",index);
        fflush(stdout);
        if (index==0 || index==1) uc2mp_leave_choice=index; /* cursor moves land here too */
        return;
    }
    if (uc2mp_menu_rule(command)) {
        size_t count; const Uc2mpRuleChoice *choices=uc2mp_rule_choices(command,&count);
        if (!runtime_uc2mp_tick_is_owner() || !uc2mp_menu_initialized || !uc2mp_menu_lobby.open ||
            index<0 || (size_t)index>=count) return;
        Uc2mpSettings settings=uc2mp_menu_lobby.settings;
        *uc2mp_menu_rule_field(&settings,command)=(uint8_t)index;
        Uc2mpResult result=uc2mp_configure(&uc2mp_menu_lobby,uc2mp_menu_lobby.local.id,settings);
        printf("UC2MP: original rule command=%u selected=%s result=%s\n",command,choices[index].label,
               uc2mp_result_text(result));
        fflush(stdout);
        return;
    }
    if (uc2mp_menu_setting(command)) {
        if (!runtime_uc2mp_tick_is_owner()) return;
        size_t count; const Uc2mpCatalogEntry *entries=uc2mp_catalog(command,&count);
        if (!uc2mp_menu_initialized || !uc2mp_menu_lobby.open || index<0 || (size_t)index>=count) return;
        Uc2mpSettings settings=uc2mp_menu_lobby.settings;
        if(command==57) settings.mode_id=entries[index].id; else settings.map_id=entries[index].id;
        Uc2mpResult result=uc2mp_configure(&uc2mp_menu_lobby,uc2mp_menu_lobby.local.id,settings);
        printf("UC2MP: original setting command=%u selected=%s result=%s\n",command,entries[index].key,uc2mp_result_text(result));
        fflush(stdout);
        return;
    }
    typedef void (ABI *Original)(const void *,int);
    ((Original)(uc2mp_menu_image+0x65fa30))(value,index);
}
static ABI const char *uc2mp_menu_item_text(const void *value,int index,int column) {
    uint32_t command=uc2mp_menu_command(value);
    /* Query48's original column1 is the pending invitation count. */
    if (command==48) return "";
    if (command==UC2MP_LEAVE_COMMAND) {
        if (column!=0 && column!=3) return "";
        if (index==0) return uc2mp_menu_game_text(UINT32_C(0x6318c53f),"Quitter");
        if (index==1) return uc2mp_menu_game_text(UINT32_C(0x81493ae7),"Annuler");
        return "";
    }
    if (uc2mp_menu_rule(command)) {
        size_t count; const Uc2mpRuleChoice *choices=uc2mp_rule_choices(command,&count);
        if (!runtime_uc2mp_tick_is_owner() || !uc2mp_menu_lobby.open || index<0 || (size_t)index>=count ||
            (column!=0 && column!=3)) return "";
        return choices[index].label;
    }
    if (uc2mp_menu_setting(command)) {
        if (!runtime_uc2mp_tick_is_owner()) return "";
        size_t count; const Uc2mpCatalogEntry *entries=uc2mp_catalog(command,&count);
        if (!uc2mp_menu_lobby.open || index<0 || (size_t)index>=count || (column!=0 && column!=3)) return "";
        return entries[index].label;
    }
    typedef const char *(ABI *Original)(const void *,int,int);
    return ((Original)(uc2mp_menu_image+0x65fd10))(value,index,column);
}
static ABI unsigned char uc2mp_menu_enabled(const void *value) {
    UC2MP_THREAD_OBSERVE(UC2MP_DIAG_MENU_ENABLED);
    uint32_t command=uc2mp_menu_command(value);
    if(command==UC2MP_LEAVE_COMMAND) return 1;
    if(command==48) return uc2mp_menu_local_member()!=NULL;
    if(uc2mp_menu_setting(command) || uc2mp_menu_rule(command))
        return runtime_uc2mp_tick_is_owner() && uc2mp_menu_lobby.open && uc2mp_menu_lobby.local.id==uc2mp_menu_lobby.host_id;
    if(command==50) return runtime_uc2mp_tick_is_owner() && uc2mp_menu_initialized &&
                           uc2mp_launch_possible(&uc2mp_menu_lobby);
    /* Shown as on PS3 but not selectable: matchmaking, cinema, leaderboards, trophies. */
    if(command==51 || command==53 || command==131 || command==136) return 0;
    if(command==52) return runtime_uc2mp_tick_is_owner() && uc2mp_menu_lobby.open;
    typedef unsigned char (ABI *Original)(const void *);
    return ((Original)(uc2mp_menu_image+0x65f5d0))(value);
}

/* ---- Match launch (PAGE_LOBBY's BUTTON_MULTIPLAYER_STARTMATCH, command50, control70e80076) ----
 * The host starts the transport's preparation; every participant acknowledges PREPARING, the
 * host authorizes LOADING, and each participant that holds the loading permission restarts its
 * game on the selected map through LoadExec (argv "level <map> player": the map test path, with
 * the launcher's spawn options in the environment). Without a transport the local lobby runs
 * the same model steps. Only maps with spawn support (the extracted catalogue) can start. */
static int uc2mp_launch_started;
static uint64_t uc2mp_launch_acked_id;
static Uc2mpPhase uc2mp_launch_acked_phase;

static const Uc2mpCatalogEntry *uc2mp_launch_map(const Uc2mpLobby *lobby) {
    const Uc2mpCatalogEntry *map=uc2mp_catalog_id(58,lobby->settings.map_id);
    return map && map->id && uc2mp_extracted_spawn_map(map->key) ? map : NULL;
}
static int uc2mp_launch_possible(const Uc2mpLobby *lobby) {
    if (!lobby->open || lobby->local.id!=lobby->host_id || lobby->preparation.phase!=UC2MP_PHASE_LOBBY ||
        !uc2mp_match_settings_valid(lobby->settings) || !uc2mp_launch_map(lobby) || !lobby->count) return 0;
    for (unsigned i=0;i<lobby->count && i<UC2MP_MAX_MEMBERS;++i) if (!lobby->members[i].ready) return 0;
    return 1;
}
static void uc2mp_launch_now(const Uc2mpLobby *lobby) {
    const Uc2mpCatalogEntry *map=uc2mp_launch_map(lobby);
    if (!map || uc2mp_launch_started) return;
    uc2mp_launch_started=1;
    printf("UC2MP: match launch: map=%s mode=%08x members=%u; restarting the game on the map\n",
           map->key,lobby->settings.mode_id,lobby->count);
    fflush(stdout);
    const char *argv[6+2*UC2MP_MAP_LEVELS_MAX]={"eboot.bin","ispartofuccollection","level",map->key};
    size_t argc=4;
    for (const char *const *level=uc2mp_map_companions(map->key);*level;++level) {
        argv[argc++]="level"; argv[argc++]=*level;
    }
    argv[argc++]="player"; argv[argc]=NULL;
    Uc2mpMatchInfo info;
    if (!uc2mp_match_info_from(lobby->settings,&info) || !uc2mp_match_info_write(&info))
        fputs("UC2MP: match loading screen details not written\n",stderr);
    runtime_load_exec("/app0/big2-ps4_Shipping.elf",argv);
    fprintf(stderr,"UC2MP: match launch failed: no launcher handoff\n");
}
static void uc2mp_launch_ack(Uc2mpLobby *lobby, uint64_t id, Uc2mpPhase phase) {
    if (uc2mp_launch_acked_id==id && uc2mp_launch_acked_phase==phase) return;
    Uc2mpResult result=uc2mp_menu_transport ? uc2mp_lan_prepare_ack(uc2mp_menu_transport,id,phase) :
        uc2mp_prepare_ack(lobby,lobby->local.id,id,phase);
    if (result==UC2MP_OK) { uc2mp_launch_acked_id=id; uc2mp_launch_acked_phase=phase; }
    printf("UC2MP: match preparation attempt=%llu %s acknowledged: %s\n",(unsigned long long)id,
           phase==UC2MP_PHASE_PREPARING ? "PREPARING" : "LOADING",uc2mp_result_text(result));
    fflush(stdout);
}
static void uc2mp_launch_step(Uc2mpLobby *lobby) {
    if (!uc2mp_menu_initialized || !lobby->open || uc2mp_launch_started) return;
    const Uc2mpPreparation p=lobby->preparation;
    if (p.phase==UC2MP_PHASE_LOBBY) return;
    const int host=lobby->local.id==lobby->host_id;
    const uint16_t all=(uint16_t)((1u<<lobby->count)-1);
    if (!uc2mp_menu_transport && uc2mp_prepare_expire(lobby,uc2mp_net_time())) return;
    if (p.phase==UC2MP_PHASE_PREPARING) {
        uc2mp_launch_ack(lobby,p.id,UC2MP_PHASE_PREPARING);
        if (host && lobby->preparation.prepared==all) {
            Uc2mpResult result=uc2mp_menu_transport ? uc2mp_lan_prepare_load(uc2mp_menu_transport,p.id) :
                uc2mp_prepare_load(lobby,lobby->local.id,p.id,uc2mp_net_time());
            if (result!=UC2MP_NOT_READY) {
                printf("UC2MP: match loading authorized: %s\n",uc2mp_result_text(result));
                fflush(stdout);
            }
        }
        return;
    }
    uc2mp_launch_ack(lobby,p.id,UC2MP_PHASE_LOADING);
    const int can=uc2mp_menu_transport ? uc2mp_lan_prepare_can_block(uc2mp_menu_transport) :
        lobby->preparation.loading==all;
    if (can) uc2mp_launch_now(lobby);
}
static ABI void uc2mp_menu_start(const void *event) {
    if (!event || !uc2mp_menu_image) return;
    uint32_t control;
    memcpy(&control,event,sizeof(control));
    if (control!=UINT32_C(0x70e80076) || !runtime_uc2mp_tick_is_owner() || !uc2mp_menu_initialized) return;
    if (!uc2mp_launch_possible(&uc2mp_menu_lobby)) {
        puts("UC2MP: match launch refused: host only, every member ready, mode and supported map chosen");
        fflush(stdout);
        return;
    }
    Uc2mpResult result=uc2mp_menu_transport ? uc2mp_lan_prepare_begin(uc2mp_menu_transport,30000) :
        uc2mp_prepare_begin(&uc2mp_menu_lobby,uc2mp_menu_lobby.local.id,uc2mp_net_time(),30000);
    printf("UC2MP: match preparation begun: %s\n",uc2mp_result_text(result));
    fflush(stdout);
    uc2mp_launch_step(&uc2mp_menu_lobby);
}

/* PAGE_MULTIPLAYER's button texts (the original dispatcher answers nothing offline), through
 * the game's own text lookup so they follow its language: 51 matchmaking, 52 custom game,
 * 53 cinema; 131 leaderboards and 136 trophies come from uc2mp_text_bridge.h. */
static const char *uc2mp_menu_page_text(uint32_t command) {
    uint32_t id;
    switch (command) {
        case 51: id = UINT32_C(0xbd1d2c53); break;
        case 52: id = UINT32_C(0x66d63005); break;
        case 53: id = UINT32_C(0xcb379354); break;
        case 131: id = UC2MP_TEXT_LEADERBOARDS; break;
        case 136: id = UC2MP_TEXT_TROPHIES; break;
        default: return NULL;
    }
    if (!uc2mp_menu_initialized || !uc2mp_menu_lobby.open) return "";
    if (!uc2mp_menu_image) return command == 52 ? "Partie personnalis\xc3\xa9" "e" : "";
    typedef const char *(ABI *Lookup)(void *, uint32_t);
    const char *text = ((Lookup)(uc2mp_menu_image + 0xcddc00))(uc2mp_menu_image + 0x1066660, id);
    return text ? text : "";
}

static const char *uc2mp_roster_name(unsigned index) {
    if (!runtime_uc2mp_tick_is_owner()) return "";
    const Uc2mpMember *member = uc2mp_menu_initialized ?
        uc2mp_member(&uc2mp_menu_lobby, index) : NULL;
    if (member && index < UC2MP_MAX_MEMBERS) {
        unsigned bit = 1u << index;
        if (!(uc2mp_roster_logged & bit)) {
            uc2mp_roster_logged |= bit;
            printf("UC2MP: original UI requested roster text row=%u name=%s\n", index, member->profile.name);
            fflush(stdout);
        }
        return member->profile.name;
    }
    return "";
}

/* MenuData text virtual method. Rizin 65f6fa reads this+40 as the command;
 * 65f727 issues query1; 65f79c returns its pointer64 result at event+40.
 * Party commands16..35 are the main/custom page's two groups of ten. Other
 * methods and controls retain
 * their original behavior, including the guest's network-state gate. */
static ABI const char *uc2mp_menu_text(const void *value) {
    UC2MP_THREAD_OBSERVE(UC2MP_DIAG_MENU_TEXT);
    uc2mp_net_poll(&uc2mp_menu_lobby);
    uint32_t command;
    memcpy(&command, (const unsigned char *)value + 0x40, sizeof(command));
    if ((command==48 || command==98 || (command>=50 && command<=53) || command==131 || command==136 ||
         uc2mp_menu_setting(command) || uc2mp_menu_rule(command) ||
         (command>=16 && command<16+2*UC2MP_MAX_MEMBERS)) && !runtime_uc2mp_tick_is_owner()) return "";
    if (command==UC2MP_LEAVE_COMMAND)
        return uc2mp_menu_game_text(UINT32_C(0x2df029e8),"Voulez-vous vraiment quitter ?");
    if (command==48) {
        const Uc2mpMember *member=uc2mp_menu_local_member();
        return !member ? "" : (member->ready ? "Annuler pr\xc3\xaa" "t" : "Pr\xc3\xaa" "t");
    }
    if (command == 98)
        return uc2mp_menu_status(&uc2mp_menu_lobby,uc2mp_menu_transport!=NULL,
                                 uc2mp_menu_transport_state);
    if (command >= 16 && command < 16 + 2*UC2MP_MAX_MEMBERS)
        return uc2mp_roster_name((command - 16)%UC2MP_MAX_MEMBERS);
    /* MenuControl::CanSelect (57a780) requires a nonempty MenuData text,
     * independently of its enabled predicate. The original text dispatcher
     * produces empty strings while offline, hiding the intact Custom action. */
    const char *page_text = uc2mp_menu_page_text(command);
    if (page_text) return page_text;
    if (command == 50) {
        if (!uc2mp_menu_initialized || !uc2mp_menu_lobby.open) return "";
        if (uc2mp_menu_lobby.preparation.phase != UC2MP_PHASE_LOBBY) return "Lancement...";
        return uc2mp_menu_lobby.local.id == uc2mp_menu_lobby.host_id ?
            "Lancer la partie" : "En attente de l'h\xc3\xb4te";
    }
    if (uc2mp_menu_rule(command)) {
        if (!uc2mp_menu_lobby.open) return "";
        if (!uc2mp_menu_image) return "rule";
        typedef const char *(ABI *Lookup)(void *, uint32_t);
        const char *text=((Lookup)(uc2mp_menu_image+0xcddc00))(uc2mp_menu_image+0x1066660,uc2mp_menu_rule_label(command));
        return text ? text : "";
    }
    if (uc2mp_menu_setting(command)) {
        /* The ordinary choice control obtains its left label through query1
         * and selected right value through query2. Keep those roles distinct. */
        return !uc2mp_menu_lobby.open ? "" : command==57 ? "Mode de jeu" : "Carte";
    }
    typedef const char *(ABI *OriginalText)(const void *);
    return ((OriginalText)(uc2mp_menu_image + 0x65f6d0))(value);
}

/* Original roster callback is a removed-function stub. Table105e730 entries
 * are {command, callback64, ordinal}; dispatch65f778..65f799 supplies ordinal
 * at event+48 and calls with RDI=&event. Unknown queries stay untouched. */
static ABI void uc2mp_roster_query(void *event) {
    unsigned char *bytes = event;
    uint32_t opcode, index;
    memcpy(&opcode, bytes, sizeof(opcode));
    if (opcode != 1) return;
    memcpy(&index, bytes + 0x48, sizeof(index));
    if (index >= 2*UC2MP_MAX_MEMBERS) return;
    const char *name = uc2mp_roster_name(index%UC2MP_MAX_MEMBERS);
    memcpy(bytes + 0x40, &name, sizeof(name));
}

/* Same-thread adapter view only. These host structures are never PS4 objects. */
static inline const Uc2mpLobby *uc2mp_menu_snapshot(void) {
    return runtime_uc2mp_tick_is_owner() && uc2mp_menu_initialized ? &uc2mp_menu_lobby : NULL;
}

static ABI void uc2mp_menu_enter(const void *event) {
    UC2MP_THREAD_OBSERVE(UC2MP_DIAG_MENU_ENTER);
    uint32_t control = 0;
    if (!event || !uc2mp_menu_image) return;
    memcpy(&control, event, sizeof(control));
    if (control != UINT32_C(0xed9d8ae1)) return;
    if (!uc2mp_net_bind(&uc2mp_menu_lobby)) return;
    if (!uc2mp_menu_initialized) {
        Uc2mpProfile profile;
        Uc2mpResult result = uc2mp_config_profile(&profile);
        if (result == UC2MP_OK) result = uc2mp_init(&uc2mp_menu_lobby, &profile);
        if (result != UC2MP_OK) {
            runtime_uc2mp_tick_unbind();
            fprintf(stderr, "UC2MP: local profile rejected: %s\n", uc2mp_result_text(result));
            return;
        }
        uc2mp_menu_initialized = 1;
    }
    Uc2mpResult result = uc2mp_net_open(&uc2mp_menu_lobby);
    if (result != UC2MP_OK) {
        uc2mp_net_close(&uc2mp_menu_lobby);
        fprintf(stderr, "UC2MP: local lobby open failed: %s\n", uc2mp_result_text(result));
        return;
    }
    /* MenuHost::OpenPage(this, CRC32 page ID, flags). Verified callers clear
     * EDX; the function returns success in AL, not a full-width integer. */
    typedef unsigned char (ABI *OpenPage)(void *, uint32_t, uint32_t);
    OpenPage open_page = (OpenPage)(uc2mp_menu_image + 0x5cb990);
    if (!open_page(uc2mp_menu_image + 0x1136640, UINT32_C(0x53cbb5f2), 0)) {
        uc2mp_net_close(&uc2mp_menu_lobby);
        fprintf(stderr, "UC2MP: multiplayer page transition refused; lobby closed\n");
        return;
    }
    printf("UC2MP: lobby entry: local=%s, members=%u; original page opened, roster text adapter installed\n",
           uc2mp_menu_lobby.local.name, uc2mp_menu_lobby.count);
    fflush(stdout);
}

static ABI void uc2mp_menu_leave(const void *event) {
    UC2MP_THREAD_OBSERVE(UC2MP_DIAG_MENU_LEAVE);
    uint32_t control;
    if (!event || !uc2mp_menu_image) return;
    memcpy(&control,event,sizeof(control));
    if (control != UINT32_C(0xafb7834c)) return;
    if (!runtime_uc2mp_tick_is_owner()) return;
    /* Reproduce original 5eb180's MenuHost::Back call. Close only when the
     * original navigation succeeds; a refused pop keeps the lobby intact. */
    typedef unsigned char (ABI *Back)(void *);
    unsigned char left = ((Back)(uc2mp_menu_image+0x5cc6f0))(uc2mp_menu_image+0x1136640);
    if (left && uc2mp_menu_initialized) {
        uc2mp_net_close(&uc2mp_menu_lobby);
        uc2mp_roster_logged = 0;
        printf("UC2MP: original leave-multiplayer command closed the local lobby\n");
        fflush(stdout);
    }
}

static int uc2mp_menu_range(Segment *segments, uint64_t count,
                           uint64_t address, uint64_t bytes, unsigned flags) {
    for (uint64_t i = 0; i < count; ++i)
        if ((segments[i].flags & flags) == flags && address >= segments[i].address &&
            bytes <= segments[i].size && address - segments[i].address <= segments[i].size - bytes)
            return 1;
    return 0;
}

static void install_uc2mp_menu_bridge(Segment *segments, uint64_t count, uint64_t size) {
    const char *enabled = getenv("UNCHARTED_LOCAL_LOBBY");
    if (!enabled || strcmp(enabled, "1")) return;
    static const unsigned char entry[] = {
        0xc3,0x66,0x66,0x66,0x66,0x66,0x66,0x2e,0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00
    };
    static const unsigned char registration[] = {
        0xc7,0x84,0xc3,0xf0,0x18,0,0,0xe1,0x8a,0x9d,0xed,
        0x48,0x8d,0x0d,0x73,0xd7,0xff,0xff,0x48,0x89,0x8c,0xc3,0xf8,0x18,0,0
    };
    static const unsigned char page_entry[] = {
        0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,
        0x53,0x48,0x81,0xec,0xc8,0,0,0,0x41,0x89,0xd5,0x49,0x89,0xfe
    };
    static const unsigned char text_entry[] = {
        0x55,0x48,0x89,0xe5,0x41,0x56,0x53,0x48,0x81,0xe4,0xe0,0xff,0xff,0xff,
        0x48,0x81,0xec,0xa0,0,0,0,0x48,0x89,0xfb
    };
    static const unsigned char leave_entry[] = {
        0x48,0x8d,0x3d,0xb9,0xb4,0xb4,0,0xe9,0x64,0x15,0xfe,0xff,0x0f,0x1f,0x40,0
    };
    static const unsigned char leave_registration[] = {
        0xc7,0x84,0xc3,0xf0,0x18,0,0,0x4c,0x83,0xb7,0xaf,
        0x48,0x8d,0x0d,0x11,0xd7,0xff,0xff,0x48,0x89,0x8c,0xc3,0xf8,0x18,0,0
    };
    static const unsigned char start_registration[] = {
        0xc7,0x84,0xc3,0xf0,0x18,0,0,0x76,0,0xe8,0x70,
        0x48,0x8d,0x0d,0x8f,0xdc,0xff,0xff,0x48,0x89,0x8c,0xc3,0xf8,0x18,0,0
    };
    static const unsigned char ready_registration[] = {
        0xc7,0x84,0xc3,0xf0,0x18,0,0,0xd7,0xb2,0x8b,0x0e,
        0x48,0x8d,0x0d,0x97,0xdd,0xff,0xff,0x48,0x89,0x8c,0xc3,0xf8,0x18,0,0
    };
    static const unsigned char status_entry[] = {
        0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x50,0x49,0x89
    };
    static const unsigned char back_entry[] = {
        0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x48,0x81,0xec,0x80,0
    };
    static const unsigned char scalar_query_entry[] = {
        0x55,0x48,0x89,0xe5,0x53,0x48,0x81,0xe4,0xe0,0xff,0xff,0xff,0x48,0x81
    };
    const struct { uint64_t slot, original; uintptr_t replacement; } queries[] = {
        {0x100b430,0x65f940,(uintptr_t)uc2mp_menu_index},
        {0x100b438,0x65fa30,(uintptr_t)uc2mp_menu_set_index},
        {0x100b460,0x65fb20,(uintptr_t)uc2mp_menu_min},
        {0x100b468,0x65fc20,(uintptr_t)uc2mp_menu_max},
        {0x100b470,0x65fd10,(uintptr_t)uc2mp_menu_item_text},
        {0x100b4f0,0x65f5d0,(uintptr_t)uc2mp_menu_enabled}
    };
    if (size < UINT64_C(0x1136640) + 0x2510 ||
        !uc2mp_menu_range(segments,count,0x5eb160,sizeof(entry),5) ||
        !uc2mp_menu_range(segments,count,0x5ed9db,sizeof(registration),5) ||
        !uc2mp_menu_range(segments,count,0x5cb990,0x870,5) ||
        !uc2mp_menu_range(segments,count,0x1136640,0x2510,6) ||
        !uc2mp_menu_range(segments,count,0x659bb0,sizeof(entry),5) ||
        !uc2mp_menu_range(segments,count,0x65f6d0,0x126,5) ||
        !uc2mp_menu_range(segments,count,0x100b458,8,4) ||
        !uc2mp_menu_range(segments,count,0x105e730,2*UC2MP_MAX_MEMBERS*24,4) ||
        !uc2mp_menu_range(segments,count,0x5eb180,sizeof(leave_entry),5) ||
        !uc2mp_menu_range(segments,count,0x5eda5d,sizeof(leave_registration),5) ||
        !uc2mp_menu_range(segments,count,0x5cc6f0,0xf8,5) ||
        !uc2mp_menu_range(segments,count,0x5eb780,sizeof(entry),5) ||
        !uc2mp_menu_range(segments,count,0x5edadf,sizeof(start_registration),5) ||
        !uc2mp_menu_range(segments,count,0x65ecc0,sizeof(status_entry),5) ||
        !uc2mp_menu_range(segments,count,0x105ec88,24,4) ||
        !uc2mp_menu_range(segments,count,0x5ebea0,sizeof(entry),5) ||
        !uc2mp_menu_range(segments,count,0x5ee0f7,sizeof(ready_registration),5) ||
        !uc2mp_menu_range(segments,count,0x105e928,24,4) ||
        memcmp(image+0x5eb160,entry,sizeof(entry)) ||
        memcmp(image+0x5ebea0,entry,sizeof(entry)) ||
        memcmp(image+0x5ee0f7,ready_registration,sizeof(ready_registration)) ||
        memcmp(image+0x659bb0,entry,sizeof(entry)) ||
        memcmp(image+0x65f6d0,text_entry,sizeof(text_entry)) ||
        memcmp(image+0x5eb180,leave_entry,sizeof(leave_entry)) ||
        memcmp(image+0x5eda5d,leave_registration,sizeof(leave_registration)) ||
        memcmp(image+0x5cc6f0,back_entry,sizeof(back_entry)) ||
        /* Even an alternative activation path for Start must remain a RET. */
        memcmp(image+0x5eb780,entry,sizeof(entry)) ||
        memcmp(image+0x5edadf,start_registration,sizeof(start_registration)) ||
        memcmp(image+0x65ecc0,status_entry,sizeof(status_entry)) ||
        memcmp(image+0x5ed9db,registration,sizeof(registration)) ||
        memcmp(image+0x5cb990,page_entry,sizeof(page_entry))) {
        fprintf(stderr,"UC2MP: local menu bridge disabled: unsupported image or signature mismatch\n");
        return;
    }
    uint32_t ready_command,ready_ordinal;
    uintptr_t ready_callback;
    memcpy(&ready_command,image+0x105e928,4);
    memcpy(&ready_callback,image+0x105e930,8);
    memcpy(&ready_ordinal,image+0x105e938,4);
    if(ready_command!=48 || ready_ordinal!=0 || ready_callback!=(uintptr_t)(image+0x659b50)) {
        fprintf(stderr,"UC2MP: local menu bridge disabled: invitation/ready query mismatch\n");
        return;
    }
    uint32_t title_command,title_ordinal;
    uintptr_t title_callback;
    memcpy(&title_command,image+0x105ec88,4);
    memcpy(&title_callback,image+0x105ec90,8);
    memcpy(&title_ordinal,image+0x105ec98,4);
    if(title_command!=98 || title_ordinal!=1 || title_callback!=(uintptr_t)(image+0x65ecc0)) {
        fprintf(stderr,"UC2MP: local menu bridge disabled: page title query mismatch\n");
        return;
    }
    uintptr_t original_text;
    memcpy(&original_text,image+0x100b458,8);
    if (original_text != (uintptr_t)(image+0x65f6d0)) {
        fprintf(stderr,"UC2MP: local menu bridge disabled: text vtable mismatch\n");
        return;
    }
    for(size_t i=0;i<sizeof(queries)/sizeof(*queries);++i) {
        uintptr_t original;
        if(!uc2mp_menu_range(segments,count,queries[i].slot,8,4) ||
           !uc2mp_menu_range(segments,count,queries[i].original,0x100,5)) {
            fprintf(stderr,"UC2MP: local menu bridge disabled: settings mapping mismatch\n");
            return;
        }
        memcpy(&original,image+queries[i].slot,8);
        if(original!=(uintptr_t)(image+queries[i].original) ||
           memcmp(image+queries[i].original,scalar_query_entry,sizeof(scalar_query_entry))) {
            fprintf(stderr,"UC2MP: local menu bridge disabled: settings query signature mismatch\n");
            return;
        }
    }
    uintptr_t action_original;
    if (!uc2mp_menu_range(segments,count,0x100b4e8,8,4) ||
        !uc2mp_menu_range(segments,count,0x65ff30,sizeof(scalar_query_entry),5) ||
        (memcpy(&action_original,image+0x100b4e8,8), action_original!=(uintptr_t)(image+0x65ff30)) ||
        memcmp(image+0x65ff30,scalar_query_entry,sizeof(scalar_query_entry))) {
        fprintf(stderr,"UC2MP: local menu bridge disabled: action query signature mismatch\n");
        return;
    }
    for (unsigned i=0; i<2*UC2MP_MAX_MEMBERS; ++i) {
        uint32_t command, ordinal;
        uintptr_t callback;
        const unsigned char *row=image+0x105e730+24*i;
        memcpy(&command,row,4); memcpy(&callback,row+8,8); memcpy(&ordinal,row+16,4);
        if (command != 16+i || ordinal != i || callback != (uintptr_t)(image+0x659bb0)) {
            fprintf(stderr,"UC2MP: local menu bridge disabled: roster query table mismatch\n");
            return;
        }
    }
    /* RIP-relative absolute jump uses 14 bytes, fits wholly inside the stub's
     * 16-byte island and preserves all incoming registers/stack alignment. */
    unsigned char jump[14] = {0xff,0x25,0,0,0,0};
    uintptr_t target = (uintptr_t)uc2mp_menu_enter;
    memcpy(jump+6,&target,sizeof(target));
    uc2mp_menu_image = image;
    memcpy(image+0x5eb160,jump,sizeof(jump));
    target = (uintptr_t)uc2mp_roster_query;
    memcpy(jump+6,&target,sizeof(target));
    memcpy(image+0x659bb0,jump,sizeof(jump));
    target = (uintptr_t)uc2mp_menu_leave;
    memcpy(jump+6,&target,sizeof(target));
    memcpy(image+0x5eb180,jump,sizeof(jump));
    target = (uintptr_t)uc2mp_menu_toggle_ready;
    memcpy(jump+6,&target,sizeof(target));
    memcpy(image+0x5ebea0,jump,sizeof(jump));
    target = (uintptr_t)uc2mp_menu_start;
    memcpy(jump+6,&target,sizeof(target));
    memcpy(image+0x5eb780,jump,sizeof(jump));
    uc2mp_net_after_poll = uc2mp_launch_step;
    target = (uintptr_t)uc2mp_menu_text;
    memcpy(image+0x100b458,&target,sizeof(target));
    for(size_t i=0;i<sizeof(queries)/sizeof(*queries);++i)
        memcpy(image+queries[i].slot,&queries[i].replacement,8);
    {
        const uintptr_t action=(uintptr_t)uc2mp_menu_action;
        memcpy(image+0x100b4e8,&action,8);
    }
    printf("UC2MP: v1.02 local menu entry, roster, ready and match launch bridges installed (opt-in)\n");
}
#endif
