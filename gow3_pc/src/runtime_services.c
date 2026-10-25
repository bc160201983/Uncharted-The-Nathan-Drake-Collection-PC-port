/* System services for an offline, single-user console:
 *   - one local user (id 1) who is logged in but not signed in to PSN;
 *   - network cable unplugged: NetCtl disconnected, sockets unavailable;
 *   - common dialogs complete immediately (no UI is drawn yet);
 *   - the whole package is installed (PlayGo reports every chunk local).
 * Every entry here is an explicit contract; unknown functions still stop. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/gow3gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

/* gow3 24 Oct 2026 (GitHub #66, save profiles): the PS4 user the games play as. Their saves live
 * in <user>/savedata/<user id>/, so each launcher save profile is a user of its own
 * (GOW3_USER_ID, 1-16; default 1, the folder every earlier version used). */
static int32_t user_id(void) {
    static int32_t id;
    if (!id) {
        const char *v=getenv("GOW3_USER_ID");
        const int n=v ? atoi(v) : 1;
        id=n>=1 && n<=16 ? n : 1;
    }
    return id;
}
#define USER_ID user_id()
#define ORBIS_OK 0
#define USER_INVALID_ARGUMENT ((int32_t)0x80960005)
#define USER_NO_EVENT ((int32_t)0x80960007)
#define SYSTEM_NO_EVENT ((int32_t)0x80A10004)
#define SYSTEM_PARAMETER ((int32_t)0x80A10003)
#define NET_CTL_NOT_CONNECTED ((int32_t)0x80412108)
#define NET_CTL_INVALID_ADDR ((int32_t)0x80412107)
#define NP_SIGNED_OUT ((int32_t)0x80550006)
#define NP_INVALID_ARGUMENT ((int32_t)0x80550003)
#define NET_ENETUNREACH ((int32_t)0x80410133)
#define NET_EINVAL ((int32_t)0x80410116)
#define HTTP_NETWORK ((int32_t)0x80431063)
#define DIALOG_NOT_INITIALIZED ((int32_t)0x80B80003)
#define PLAYGO_BAD_HANDLE ((int32_t)0x80B20009)
#define PLAYGO_BAD_POINTER ((int32_t)0x80B2000A)
#define PLAYGO_BAD_SIZE ((int32_t)0x80B2000B)
#define PLAYGO_BAD_CHUNK ((int32_t)0x80B2000C)
#define AUDIO_IN_NOT_OPENED ((int32_t)0x80260109)
#define TROPHY_INVALID ((int32_t)0x80551604)

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int next_id=1;
static int new_id(void) { pthread_mutex_lock(&lock); int id=next_id++; pthread_mutex_unlock(&lock); return id; }
static void note(const char *what) { printf("Runtime: %s\n",what); }
/* Local execution has no Remote Play session or streaming transport. */
static ABI int32_t remoteplay_initialize(void) {
    note("Remote Play initialized (local execution, streaming unavailable)");
    return 0;
}

/* ---- UserService ---- */
static int login_event_pending=1;
static ABI int32_t user_initialize(const void *params) { (void)params; note("UserService initialized (user 1 logged in)"); return 0; }
static ABI int32_t user_terminate(void) { return 0; }
static ABI int32_t user_initial(int32_t *id) { if (!id) return USER_INVALID_ARGUMENT; *id=USER_ID; return 0; }
static ABI int32_t user_list(int32_t *ids) {
    if (!ids) return USER_INVALID_ARGUMENT;
    ids[0]=USER_ID; ids[1]=ids[2]=ids[3]=-1; return 0;
}
static ABI int32_t user_name(int32_t id,char *name,uint64_t size) {
    if (id!=USER_ID || !name) return USER_INVALID_ARGUMENT;
    const char *value=getenv("GOW3_USER_NAME") ? getenv("GOW3_USER_NAME") : "Hunter";
    if (strlen(value)+1>size) return (int32_t)0x8096000a; /* BUFFER_TOO_SHORT */
    strcpy(name,value); return 0;
}
static ABI int32_t user_event(int32_t *event) {
    if (!event) return USER_INVALID_ARGUMENT;
    pthread_mutex_lock(&lock);
    int pending=login_event_pending; login_event_pending=0;
    pthread_mutex_unlock(&lock);
    if (!pending) return USER_NO_EVENT;
    event[0]=0; event[1]=USER_ID; /* LOGIN */
    return 0;
}

/* ---- SystemService ---- */
static int language(void) { const char *v=getenv("GOW3_LANGUAGE"); return v ? atoi(v) : 1; }
static ABI int32_t system_param(int32_t id,int32_t *value) {
    if (!value) return SYSTEM_PARAMETER;
    switch (id) {
    case 1: *value=language(); break;           /* language (1 = English US, 8 = Russian) */
    case 2: *value=1; break;                    /* date format DD/MM/YYYY */
    case 3: *value=1; break;                    /* 24-hour clock */
#ifdef _WIN32
    case 4: *value=(int32_t)(compat_utc_offset(time(NULL))/60); break;
#else
    case 4: { time_t now=time(NULL); struct tm t; localtime_r(&now,&t); *value=(int32_t)(t.tm_gmtoff/60); break; }
#endif
    case 5: *value=0; break;                    /* summer time */
    case 7: *value=0; break;                    /* parental level off */
    case 1000: *value=1; break;                 /* enter button = cross */
    default: fprintf(stderr,"STOP: unsupported system parameter %d\n",id); exit(21);
    }
    return 0;
}
static ABI int32_t system_status(unsigned char *status) {
    if (!status) return SYSTEM_PARAMETER;
    memset(status,0,12); /* event_num=0, no overlay, foreground, normal CPU mode */
    return 0;
}
static ABI int32_t system_event(void *event) { (void)event; return SYSTEM_NO_EVENT; }
static ABI int32_t hide_splash(void) { note("SystemService: splash screen hidden"); return 0; }
static ABI int32_t safe_area(float *info) { if (!info) return SYSTEM_PARAMETER; *info=1.0f; return 0; } /* full screen */
static ABI int32_t disable_music_player(void) { return 0; }
static ABI int32_t launch_browser(void) { note("SystemService: web browser request ignored (offline)"); return 0; }
/* The Nathan Drake Collection is three executables: the menu (eboot.bin, also Drake's Fortune)
 * starts Uncharted 2/3 with LoadExec("/app0/big2-ps4_Shipping.elf", {"eboot.bin",
 * "ispartofuccollection"}) and they return the same way; argv is the new program's complete argv
 * (argv[0] "eboot.bin"; Uncharted 2/3 treat any unknown argument as a level to load and skip the
 * title screen). The PS4 replaces the running process;
 * here the path and arguments go to GOW3_LOADEXEC_FILE (one per line) and the process exits
 * with LOADEXEC_EXIT so the launcher prepares and starts the requested executable. */
#define LOADEXEC_EXIT 30
static ABI int32_t load_exec(const char *path,const char *const *argv) {
    if (!path || !*path) return SYSTEM_PARAMETER;
    printf("Runtime: sceSystemServiceLoadExec(%s",path);
    for (const char *const *a=argv;a && *a;++a) printf(", \"%s\"",*a);
    puts(")");
    const char *handoff=getenv("GOW3_LOADEXEC_FILE");
    FILE *f=handoff ? fopen(handoff,"w") : NULL;
    if (!f) {
        puts("STOP: game asked to switch executable; no launcher handoff (GOW3_LOADEXEC_FILE)");
        fflush(NULL);
        _exit(LOADEXEC_EXIT+1);
    }
    fprintf(f,"%s\n",path);
    for (const char *const *a=argv;a && *a;++a) fprintf(f,"%s\n",*a);
    fclose(f);
    puts("Runtime: executable switch handed to the launcher; exiting");
    fflush(NULL);
    _exit(LOADEXEC_EXIT); /* no destructors: GPU, audio and guest threads are still running */
}
/* The same switch requested by the runtime itself (uc2mp_menu_bridge.h: a multiplayer match
 * restarts the game on its map). Does not return when the launcher handoff exists. */
int32_t runtime_load_exec(const char *path,const char *const *argv) { return load_exec(path,argv); }

/* ---- NetCtl / Net / Http / Ssl: no network ---- */
/* POSIX sockets (Uncharted 2/3 through libScePosix): the cable is unplugged. */
#define GUEST_EBADF 9
#define GUEST_ENETDOWN 50
static ABI int32_t posix_socket_none(void) { *runtime_errno()=GUEST_ENETDOWN; return -1; }
static ABI int32_t posix_socket_badf(void) { *runtime_errno()=GUEST_EBADF; return -1; }
static ABI int32_t posix_fcntl(int32_t fd,int32_t cmd,int64_t arg) {
    (void)fd; (void)cmd; (void)arg;
    return 0; /* F_GETFD/F_GETFL: no flags; set commands accepted */
}
static ABI int32_t pthread_thread_id(void) {
#ifdef _WIN32
    return (int32_t)GetCurrentThreadId();
#else
    return (int32_t)pthread_self();
#endif
}
static int32_t net_errno;
static ABI int32_t net_init(void) { return 0; }
static ABI int32_t net_term(void) { return 0; }
static ABI int32_t *net_errno_loc(void) { return &net_errno; }
static ABI int32_t net_pool_create(const char *name,int size,int flags) { (void)name; (void)size; (void)flags; return new_id(); }
static ABI int32_t net_pool_destroy(int id) { (void)id; return 0; }
static ABI int32_t net_unreachable(void) { net_errno=51; return NET_ENETUNREACH; }
static ABI int32_t net_epoll_create(const char *name,int flags) { (void)name; (void)flags; return new_id(); }
static ABI int32_t net_epoll_destroy(int id) { (void)id; return 0; }
static ABI int32_t net_resolver_create(const char *name,int pool,int flags) { (void)name; (void)pool; (void)flags; return new_id(); }
static ABI int32_t net_resolver_destroy(int id) { (void)id; return 0; }
static ABI uint16_t net_htons(uint16_t v) { return htons(v); }
static ABI uint16_t net_ntohs(uint16_t v) { return ntohs(v); }
static ABI uint32_t net_htonl(uint32_t v) { return htonl(v); }
static ABI uint32_t net_ntohl(uint32_t v) { return ntohl(v); }
static ABI int32_t net_pton(int af,const char *src,void *dst) {
    if (af!=2) { net_errno=47; return NET_EINVAL; }
    return inet_pton(AF_INET,src,dst);
}
static ABI const char *net_ntop(int af,const void *src,char *dst,uint32_t size) {
    if (af!=2) { net_errno=47; return NULL; }
    return inet_ntop(AF_INET,src,dst,size);
}
static ABI int32_t netctl_state(int32_t *state) { if (!state) return NET_CTL_INVALID_ADDR; *state=0; return 0; }
static ABI int32_t netctl_info(int code,void *info) { (void)code; (void)info; return NET_CTL_NOT_CONNECTED; }
static ABI int32_t netctl_register(void *cb,void *arg,int32_t *cid) { (void)cb; (void)arg; if (!cid) return NET_CTL_INVALID_ADDR; *cid=new_id(); return 0; }
static ABI int32_t netctl_check(void) { return 0; }
static ABI int32_t netctl_unregister(int cid) { (void)cid; return 0; }
static ABI int32_t netctl_nat(uint32_t *info) {
    if (!info) return NET_CTL_INVALID_ADDR;
    info[1]=0; info[2]=3; info[3]=0; /* stun failed, NAT type 3, no mapped address */
    return 0;
}
static ABI int32_t lib_init_id(void) { return new_id(); }
static ABI int32_t ok_void(void) { return 0; }
static ABI int32_t http_fail(void) { return HTTP_NETWORK; }
/* Objects are created so setup code proceeds; any transfer fails as unplugged. */
static ABI int32_t http_object(void) { return new_id(); }
static ABI int32_t http_epoll(int32_t ctx,void **handle) {
    (void)ctx;
    if (!handle) return (int32_t)0x80431077; /* HTTP INVALID_VALUE */
    *handle=(void *)(uintptr_t)(0x100+new_id()); return 0;
}
static ABI int32_t http_wait(void *handle,void *events,int32_t max,int64_t timeout) {
    (void)handle; (void)events; (void)max;
#ifdef _WIN32
    if (timeout>0) compat_sleep_ns((uint64_t)timeout*1000);
#else
    if (timeout>0) { struct timespec t={timeout/1000000,(timeout%1000000)*1000}; nanosleep(&t,NULL); }
#endif
    return 0; /* no events: nothing is in flight */
}

/* ---- NP (PSN): signed out ---- */
static ABI int32_t np_state(int32_t user,int32_t *state) {
    if (!state) return NP_INVALID_ARGUMENT;
    (void)user; *state=1; /* SIGNED_OUT */
    return 0;
}
static ABI int32_t np_signed_out(void) { return NP_SIGNED_OUT; }
/* TUS has no contexts/requests while the local user is signed out of PSN. */
static ABI int32_t np_tus_no_login(void) { return (int32_t)0x80550705; }
static ABI int32_t np_tus_invalid_id(void) { return (int32_t)0x8055070e; }
static ABI int32_t np_register(void *cb,void *arg) { (void)cb; (void)arg; return new_id(); }
static ABI void np_register_void(void *cb,void *arg) { (void)cb; (void)arg; }
static ABI int32_t np_request(const void *param) { (void)param; return new_id(); }
static ABI int32_t np_request_ctx(int32_t ctx,const void *param) { (void)ctx; (void)param; return new_id(); }
static ABI int32_t np_poll(int32_t request,int32_t *result) { (void)request; if (result) *result=NP_SIGNED_OUT; return 0; }
static ABI int32_t np_compare(const void *a,const void *b) {
    if (!a || !b) return NP_INVALID_ARGUMENT;
    return memcmp(a,b,16) ? (int32_t)0x80550609 : 0; /* NP_UTIL NOT_MATCH */
}

/* ---- Voice chat: ports exist, carry no audio ---- */
static ABI int32_t voice_port(void *param,uint32_t *port) { (void)param; if (!port) return (int32_t)0x8029000b; *port=(uint32_t)new_id(); return 0; }
static ABI int32_t voice_read(uint32_t port,void *data,uint32_t *size) { (void)port; (void)data; if (size) *size=0; return 0; }
static ABI int32_t voice_write(uint32_t port,const void *data,uint32_t *size) { (void)port; (void)data; (void)size; return 0; }
static ABI int32_t voice_info(uint32_t port,uint32_t *info) {
    (void)port;
    if (!info) return (int32_t)0x8029000b;
    memset(info,0,40); /* type, state=unconnected, no bytes available */
    return 0;
}

/* ---- Common dialogs: nothing is displayed; an opened dialog finishes. ---- */
typedef struct { const char *name; int initialized, status; uint32_t button; } Dialog;
static Dialog dialogs[]={{"CommonDialog",0,0,0},{"MsgDialog",0,0,0},{"SaveDataDialog",0,0,0},
                         {"NpProfileDialog",0,0,0},{"NpCommerceDialog",0,0,0},{"ImeDialog",0,0,0}};
#define DIALOG_RUNNING 2
#define DIALOG_FINISHED 3
#define DIALOG_NOT_FINISHED ((int32_t)0x80B80005)
#define DIALOG_PARAM_INVALID ((int32_t)0x80B8000A)
#define DIALOG_NOT_RUNNING ((int32_t)0x80B8000B)
#define DIALOG_ARG_NULL ((int32_t)0x80B8000D)
#define DIALOG_NOT_SUPPORTED ((int32_t)0x80B8000F)
static ABI int32_t common_init(void) { return 0; }
static int32_t dialog_init(int i) {
    if (dialogs[i].initialized) return (int32_t)0x80B80004;
    dialogs[i].initialized=1; dialogs[i].status=1; return 0;
}
static int32_t dialog_open(int i) {
    if (!dialogs[i].initialized) return DIALOG_NOT_INITIALIZED;
    printf("Runtime: %s opened; completed immediately (no dialog UI yet)\n",dialogs[i].name);
    dialogs[i].status=DIALOG_FINISHED; dialogs[i].button=1; return 0; /* OK / YES */
}
/* A progress bar shows the game's own work: it runs until the game closes it. */
static int32_t dialog_open_progress(int i) {
    if (!dialogs[i].initialized) return DIALOG_NOT_INITIALIZED;
    printf("Runtime: %s progress bar opened (runs until closed)\n",dialogs[i].name);
    dialogs[i].status=DIALOG_RUNNING; dialogs[i].button=0; return 0;
}
static int32_t dialog_close(int i) {
    if (dialogs[i].status!=DIALOG_RUNNING) return DIALOG_NOT_RUNNING;
    dialogs[i].status=DIALOG_FINISHED; return 0;
}
static int32_t dialog_progress(int i, uint32_t target) {
    if (dialogs[i].status!=DIALOG_RUNNING) return DIALOG_NOT_RUNNING;
    return target ? DIALOG_PARAM_INVALID : 0;
}
/* Result: result (OK), button, reserved; SaveDataDialog's begins with the mode. */
static int32_t dialog_result(int i, unsigned char *out, size_t offset) {
    if (dialogs[i].status!=DIALOG_FINISHED) return DIALOG_NOT_FINISHED;
    if (!out) return DIALOG_ARG_NULL;
    uint32_t values[2]={0,dialogs[i].button};
    memcpy(out+offset,values,8);
    return 0;
}
static int32_t dialog_status(int i) { return dialogs[i].status; }
static int32_t dialog_term(int i) {
    if (!dialogs[i].initialized) return DIALOG_NOT_INITIALIZED;
    dialogs[i].initialized=0; dialogs[i].status=0; return 0;
}
#define DIALOG(tag,i) \
    static ABI int32_t tag##_init(void) { return dialog_init(i); } \
    static ABI int32_t tag##_open(const void *p) { (void)p; return dialog_open(i); } \
    static ABI int32_t tag##_status(void) { return dialog_status(i); } \
    static ABI int32_t tag##_term(void) { return dialog_term(i); }
DIALOG(profile,3) DIALOG(commerce,4)
/* OrbisMsgDialogParam: BaseParam (48 bytes), size, mode (2 = progress bar).
 * OrbisSaveDataDialogParam: BaseParam, int32 size, mode (5 = progress bar). */
static int32_t dialog_open_mode(int i, const unsigned char *param, size_t mode_offset, uint32_t progress_mode) {
    if (!param) return DIALOG_ARG_NULL;
    uint32_t mode; memcpy(&mode,param+mode_offset,4);
    return mode==progress_mode ? dialog_open_progress(i) : dialog_open(i);
}
static ABI int32_t msg_init(void) { return dialog_init(1); }
static ABI int32_t msg_open(const unsigned char *p) { return dialog_open_mode(1,p,56,2); }
static ABI int32_t msg_status(void) { return dialog_status(1); }
static ABI int32_t msg_term(void) { return dialog_term(1); }
static ABI int32_t msg_close(void) { return dialog_close(1); }
static ABI int32_t msg_progress(uint32_t target, uint32_t value) { (void)value; return dialog_progress(1,target); }
static ABI int32_t msg_result(unsigned char *out) { return dialog_result(1,out,0); }
/* SaveDataDialog. OrbisSaveDataDialogParam: BaseParam (48), size, mode @52, dispType @56,
 * animParam @64, items @72, ..., userData @112. SaveDialogItems: userId @0, titleId @8,
 * dirName @16 (32-byte names), dirNameNum @24, newItem @32 (title @0), focusPos @40.
 * A list (mode 1) is shown for the player to choose; every other mode answers for the
 * first listed save. The result names the chosen save ("" = the new item). */
#define SAVE_MODE_LIST 1
#define SAVE_RESULT_CANCELED 1
static struct {
    uint32_t mode, result;
    void *user_data;
    char dirs[32][32];
    int dir_count, has_new, choosing;
} save_dialog;
static char save_labels[33][200], save_icons[33][720];
static void save_label(char *out, size_t size, const char *dir) {
    unsigned char param[RUNTIME_SAVE_PARAM_SIZE];
    if (runtime_savedata_param(USER_ID,dir,param)) { snprintf(out,size,"%s",dir); return; }
    int64_t mtime; memcpy(&mtime,param+1288,8);
    time_t t=(time_t)mtime; struct tm tm;
#ifdef _WIN32
    localtime_s(&tm,&t);
#else
    localtime_r(&t,&tm);
#endif
    char when[32]; strftime(when,sizeof(when),"%Y-%m-%d %H:%M",&tm);
    const char *title=(const char *)param, *subtitle=(const char *)param+128;
    /* Title, subtitle and date on their own lines, beside the save's picture. */
    snprintf(out,size,"%.60s\n%.60s%s%s",title[0] ? title : dir,subtitle,subtitle[0] ? "\n" : "",when);
}
static ABI int32_t save_init(void) { return dialog_init(2); }
static ABI int32_t save_open(const unsigned char *p) {
    if (!p) return DIALOG_ARG_NULL;
    if (!dialogs[2].initialized) return DIALOG_NOT_INITIALIZED;
    memset(&save_dialog,0,sizeof(save_dialog));
    memcpy(&save_dialog.mode,p+52,4);
    memcpy(&save_dialog.user_data,p+112,8);
    const unsigned char *items; memcpy(&items,p+72,8);
    if (items) {
        const char *dirs; uint32_t count; const void *new_item;
        memcpy(&dirs,items+16,8); memcpy(&count,items+24,4); memcpy(&new_item,items+32,8);
        for (uint32_t i=0;dirs && i<count && save_dialog.dir_count<32;++i)
            if (dirs[i*32]) memcpy(save_dialog.dirs[save_dialog.dir_count++],dirs+i*32,31);
        save_dialog.has_new=new_item!=NULL;
    }
    if (save_dialog.mode!=SAVE_MODE_LIST) return dialog_open_mode(2,p,52,5);
    const char *labels[33], *icons[33]; int n=0;
    if (save_dialog.has_new) { snprintf(save_labels[n],sizeof(save_labels[n]),"New save"); labels[n]=save_labels[n]; icons[n]=NULL; ++n; }
    for (int i=0;i<save_dialog.dir_count;++i) {
        save_label(save_labels[n],sizeof(save_labels[n]),save_dialog.dirs[i]); labels[n]=save_labels[n];
        icons[n]=runtime_savedata_icon(USER_ID,save_dialog.dirs[i],save_icons[n],sizeof(save_icons[n])) ? NULL : save_icons[n];
        ++n;
    }
    uint32_t type; memcpy(&type,p+56,4);
    const char *title=type==1 ? "Save game" : type==2 ? "Load game" : "Delete save";
    if (!n || !gow3gpu_choice_begin(title,labels,icons,n,0)) {
        printf("Runtime: SaveDataDialog list (%s): %s\n",title,n ? "no window, cancelled" : "no saves, cancelled");
        save_dialog.result=SAVE_RESULT_CANCELED;
        dialogs[2].status=DIALOG_FINISHED; dialogs[2].button=0;
        return 0;
    }
    printf("Runtime: SaveDataDialog list (%s): %d entries shown\n",title,n);
    save_dialog.choosing=1;
    dialogs[2].status=DIALOG_RUNNING;
    return 0;
}
static ABI int32_t save_status(void) {
    if (save_dialog.choosing) {
        int choice=gow3gpu_choice_poll();
        if (choice!=-1) {
            save_dialog.choosing=0;
            dialogs[2].status=DIALOG_FINISHED;
            if (choice<0) { save_dialog.result=SAVE_RESULT_CANCELED; dialogs[2].button=0; }
            else {
                int dir=choice-save_dialog.has_new;
                /* The chosen save goes first; the new item has no name. */
                if (dir>=0) memcpy(save_dialog.dirs[0],save_dialog.dirs[dir],32);
                else save_dialog.dirs[0][0]=0;
                save_dialog.dir_count=dir>=0 || save_dialog.dir_count ? 1 : 0;
                dialogs[2].button=1;
            }
            printf("Runtime: SaveDataDialog list: %s%s\n",choice<0 ? "cancelled" : "chose ",
                   choice<0 ? "" : save_dialog.dirs[0][0] ? save_dialog.dirs[0] : "new save");
        }
    }
    return dialog_status(2);
}
static ABI int32_t save_term(void) { return dialog_term(2); }
static ABI int32_t save_close(void) { return dialog_close(2); }
static ABI int32_t save_progress(uint32_t target, uint32_t rate) { (void)rate; return dialog_progress(2,target); }
/* OrbisSaveDataDialogResult: mode, result, buttonId, dirName* @16, param* @24, userData @32. */
static ABI int32_t save_result(unsigned char *out) {
    int32_t r=dialog_result(2,out,4);
    if (r) return r;
    memcpy(out,&save_dialog.mode,4);
    memcpy(out+4,&save_dialog.result,4);
    char *dir; unsigned char *param;
    memcpy(&dir,out+16,8); memcpy(&param,out+24,8);
    const char *name=save_dialog.dir_count && !save_dialog.result ? save_dialog.dirs[0] : "";
    if (dir) { memset(dir,0,32); memcpy(dir,name,strnlen(name,31)); }
    if (param && name[0]) runtime_savedata_param(USER_ID,name,param);
    memcpy(out+32,&save_dialog.user_data,8);
    return 0;
}
static ABI int32_t save_ready(void) { return 1; }
static ABI int32_t profile_result(void *result) { if (result) memset(result,0,4); return 0; }
/* ImeDialog: text typed on the keyboard into the game window (title bar shows it).
 * OrbisImeDialogParam: user, type, languages(8), enter label, method, filter,
 * option, max length, char16 buffer, position, alignment, placeholder, title. */
typedef struct {
    int32_t user; uint32_t type; uint64_t languages; uint32_t enter_label, input_method;
    void *filter; uint32_t option, max_length; uint16_t *buffer;
    float x, y; uint32_t halign, valign; const uint16_t *placeholder, *title; int8_t reserved[16];
} ImeParam;
static struct { int running, finished, end_status; uint16_t *buffer; uint32_t max_length; } ime;
static size_t utf16_to_utf8(const uint16_t *in, size_t limit, char *out, size_t size) {
    size_t n=0;
    for (size_t i=0; in && i<limit && in[i] && n+4<size; ++i) {
        uint32_t c=in[i];
        if (c>=0xD800 && c<0xDC00 && i+1<limit && in[i+1]>=0xDC00 && in[i+1]<0xE000) { c=0x10000+((c-0xD800)<<10)+(in[i+1]-0xDC00); ++i; }
        if (c<0x80) out[n++]=(char)c;
        else if (c<0x800) { out[n++]=(char)(0xC0|c>>6); out[n++]=(char)(0x80|(c&63)); }
        else if (c<0x10000) { out[n++]=(char)(0xE0|c>>12); out[n++]=(char)(0x80|((c>>6)&63)); out[n++]=(char)(0x80|(c&63)); }
        else { out[n++]=(char)(0xF0|c>>18); out[n++]=(char)(0x80|((c>>12)&63)); out[n++]=(char)(0x80|((c>>6)&63)); out[n++]=(char)(0x80|(c&63)); }
    }
    out[n]=0; return n;
}
static void utf8_to_utf16(const char *in, uint16_t *out, uint32_t max) {
    uint32_t n=0;
    for (const unsigned char *p=(const unsigned char *)in; *p && n<max;) {
        uint32_t c; int extra;
        if (*p<0x80) { c=*p; extra=0; } else if ((*p&0xE0)==0xC0) { c=*p&31; extra=1; }
        else if ((*p&0xF0)==0xE0) { c=*p&15; extra=2; } else { c=*p&7; extra=3; }
        ++p;
        for (int k=0;k<extra && (*p&0xC0)==0x80;++k) c=(c<<6)|(*p++&63);
        if (c>=0x10000) { if (n+2>max) break; c-=0x10000; out[n++]=(uint16_t)(0xD800+(c>>10)); out[n++]=(uint16_t)(0xDC00+(c&1023)); }
        else out[n++]=(uint16_t)c;
    }
    out[n<max ? n : max]=0;
}
static void ime_complete(int end_status, const char *text) {
    if (!end_status && ime.buffer && ime.max_length) utf8_to_utf16(text,ime.buffer,ime.max_length);
    ime.end_status=end_status; ime.finished=1; ime.running=0;
    printf("Runtime: ImeDialog %s%s%s\n",end_status ? "cancelled" : "text: ",end_status ? "" : text,"");
}
static ABI int32_t ime_init(const ImeParam *param, const void *extended) {
    (void)extended;
    if (!param || !param->buffer || !param->max_length) return (int32_t)0x80BC0004; /* IME INVALID_ADDRESS */
    if (ime.running) return (int32_t)0x80BC0003;                                  /* BUSY */
    memset(&ime,0,sizeof(ime));
    ime.buffer=param->buffer; ime.max_length=param->max_length; ime.running=1;
    char initial[512], prompt[256];
    utf16_to_utf8(param->buffer,param->max_length,initial,sizeof(initial));
    utf16_to_utf8(param->title,128,prompt,sizeof(prompt));
    const char *preset=getenv("GOW3_IME_TEXT");
    if (preset) { ime_complete(0,preset); return 0; }
    /* An empty field starts with the player name (launcher) or "Hunter": a controller alone
     * can accept it (the port has no on-screen keyboard). */
    const char *name=getenv("GOW3_USER_NAME");
    const char *start=initial[0] ? initial : name && *name ? name : "Hunter";
    if (!gow3gpu_text_input_begin(start,prompt[0] ? prompt : "Text")) ime_complete(0,start);
    else printf("Runtime: ImeDialog opened: shown on screen; keyboard Enter or Cross accepts, Esc or Circle cancels\n");
    return 0;
}
static ABI int32_t ime_status(void) {
    if (ime.running) {
        char text[512];
        int state=gow3gpu_text_input_poll(text,sizeof(text));
        if (state) ime_complete(state==1 ? 0 : 1,text);
    }
    return ime.running ? 1 : ime.finished ? 2 : 0; /* Running / Finished / None */
}
static ABI int32_t ime_result(uint32_t *result) {
    if (!ime.finished) return (int32_t)0x80BC0101; /* DIALOG NOT_FINISHED */
    if (result) *result=(uint32_t)ime.end_status;
    return 0;
}
static ABI int32_t ime_term(void) { memset(&ime,0,sizeof(ime)); return 0; }

/* ---- Trophies (gow3_trophies.cpp shows them) ----
 * The three games keep three trophy lists, sce_sys/trophy/trophy01-03.trp; a game's context names
 * its list by its service label. Earned trophies are saved in <user>/trophy/trophyNN.txt (an
 * "id time" line each) and reported back to the game; a newly earned one is shown as a popup. */
#define TROPHY_LISTS 4
static pthread_mutex_t trophy_lock=PTHREAD_MUTEX_INITIALIZER;
static struct { int32_t ctx; int list; } trophy_contexts[8];
static uint32_t trophy_flags[TROPHY_LISTS][4];
static int trophy_loaded[TROPHY_LISTS];
static void trophy_file(int list,char *out,size_t size) {
    snprintf(out,size,"%s/trophy/trophy%02d.txt",runtime_file_user_dir(),list);
}
static void trophy_load(int list) { /* trophy_lock held */
    if (trophy_loaded[list]) return;
    trophy_loaded[list]=1;
    char path[700]; trophy_file(list,path,sizeof(path));
    FILE *f=fopen(path,"r");
    if (!f) return;
    int id; long long when;
    while (fscanf(f,"%d %lld",&id,&when)==2) if (id>=0 && id<128) trophy_flags[list][id/32]|=1u<<(id%32);
    fclose(f);
}
static int trophy_list_of(int32_t ctx) { /* trophy_lock held */
    for (int i=0;i<8;++i) if (trophy_contexts[i].ctx==ctx) return trophy_contexts[i].list;
    return 0;
}
static ABI int32_t trophy_context(int32_t *ctx,int32_t user,uint32_t label,uint64_t options) {
    (void)user; (void)options;
    if (!ctx) return TROPHY_INVALID;
    *ctx=new_id();
    const int list=label>=1 && label<TROPHY_LISTS ? (int)label : 0;
    printf("Runtime: trophy context %d: service label %u (list %d)\n",*ctx,label,list);
    pthread_mutex_lock(&trophy_lock);
    for (int i=0;i<8;++i) if (!trophy_contexts[i].ctx) { trophy_contexts[i].ctx=*ctx; trophy_contexts[i].list=list; break; }
    if (list) trophy_load(list);
    pthread_mutex_unlock(&trophy_lock);
    return 0;
}
static ABI int32_t trophy_handle(int32_t *handle) { if (!handle) return TROPHY_INVALID; *handle=new_id(); return 0; }
static ABI int32_t trophy_register(int32_t ctx,int32_t handle,uint64_t options) {
    (void)handle; (void)options;
    pthread_mutex_lock(&trophy_lock);
    const int list=trophy_list_of(ctx);
    uint32_t flags[4]={0};
    if (list) memcpy(flags,trophy_flags[list],sizeof(flags));
    pthread_mutex_unlock(&trophy_lock);
    if (list) {
        char guest[64], pack[1024];
        snprintf(guest,sizeof(guest),"/app0/sce_sys/trophy/trophy%02d.trp",list);
        if (!runtime_file_translate(guest,pack,sizeof(pack))) gow3gpu_trophy_list(pack,list,flags);
    }
    return 0;
}
static ABI int32_t trophy_unlock(int32_t ctx,int32_t handle,int32_t id,int32_t *platinum) {
    (void)handle;
    if (platinum) *platinum=-1;
    if (id<0 || id>=128) return TROPHY_INVALID;
    pthread_mutex_lock(&trophy_lock);
    const int list=trophy_list_of(ctx);
    int earned_now=0;
    if (list && !(trophy_flags[list][id/32]>>(id%32) & 1)) {
        trophy_flags[list][id/32]|=1u<<(id%32);
        earned_now=1;
        char path[700]; trophy_file(list,path,sizeof(path));
        char folder[700]; snprintf(folder,sizeof(folder),"%s/trophy",runtime_file_user_dir());
        mkdir(folder,0755);
        FILE *f=fopen(path,"a");
        if (f) { fprintf(f,"%d %lld\n",id,(long long)time(NULL)); fclose(f); }
    }
    pthread_mutex_unlock(&trophy_lock);
    printf("Runtime: trophy %d of list %d %s\n",id,list,earned_now ? "earned" : "unlocked again");
    if (earned_now) gow3gpu_trophy_earned(id);
    return 0;
}
static ABI int32_t trophy_game_info(int32_t ctx,int32_t handle,void *details,void *data) {
    (void)ctx; (void)handle;
    if (details) { uint64_t size; memcpy(&size,details,8); memset((char *)details+8,0,size>8 && size<4096 ? size-8 : 0); }
    if (data) { uint64_t size; memcpy(&size,data,8); memset((char *)data+8,0,size>8 && size<4096 ? size-8 : 0); }
    return 0;
}
static ABI int32_t trophy_info(int32_t ctx,int32_t handle,int32_t id,void *details,void *data) {
    (void)id; return trophy_game_info(ctx,handle,details,data);
}
/* The saved trophies of the context's list: a 128-trophy flag set. */
static ABI int32_t trophy_unlock_state(int32_t ctx,int32_t handle,uint32_t *flags,uint32_t *count) {
    if (!flags || !count) return TROPHY_INVALID;
    if (ctx<=0 || handle<=0) return TROPHY_INVALID;
    memset(flags,0,16); *count=0;
    pthread_mutex_lock(&trophy_lock);
    const int list=trophy_list_of(ctx);
    if (list) memcpy(flags,trophy_flags[list],16);
    pthread_mutex_unlock(&trophy_lock);
    for (int i=0;i<4;++i) for (uint32_t bits=flags[i];bits;bits&=bits-1) ++*count;
    return 0;
}
static ABI int32_t trophy_release(int32_t id) { return id>0 ? 0 : TROPHY_INVALID; }

/* ---- PlayGo: fully installed package ---- */
static int playgo_handle, playgo_chunks=-1;
static uint64_t playgo_language_mask;
static ABI int32_t playgo_init(const void *params) {
    (void)params;
    int fd=(int)runtime_file_open("/app0/sce_sys/playgo-chunk.dat",0,0);
    unsigned char header[12];
    playgo_chunks=1;
    int lang=language();
    __atomic_store_n(&playgo_language_mask,lang>=0 && lang<48 ? UINT64_C(1)<<(63-lang) : 0,__ATOMIC_RELAXED);
    if (fd>=0) {
        if (runtime_file_read(fd,header,sizeof(header))==sizeof(header)) playgo_chunks=header[10]|header[11]<<8;
        runtime_file_close(fd);
    }
    printf("Runtime: PlayGo initialized; %d chunks, all installed locally\n",playgo_chunks);
    return 0;
}
static ABI int32_t playgo_open(int32_t *handle,const void *param) {
    (void)param;
    if (!handle) return PLAYGO_BAD_POINTER;
    playgo_handle=1; *handle=1; return 0;
}
static ABI int32_t playgo_chunk_ids(int32_t handle,uint16_t *ids,uint32_t count,uint32_t *out) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!out) return PLAYGO_BAD_POINTER;
    if (ids && !count) return PLAYGO_BAD_SIZE;
    if (!ids) { *out=(uint32_t)playgo_chunks; return 0; }
    uint32_t n=count<(uint32_t)playgo_chunks ? count : (uint32_t)playgo_chunks;
    for (uint32_t i=0;i<n;++i) ids[i]=(uint16_t)i;
    *out=n; return 0;
}
static ABI int32_t playgo_locus(int32_t handle,const uint16_t *ids,uint32_t count,int8_t *loci) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!ids || !loci) return PLAYGO_BAD_POINTER;
    if (!count) return PLAYGO_BAD_SIZE;
    for (uint32_t i=0;i<count;++i) {
        if (ids[i]>=playgo_chunks) return PLAYGO_BAD_CHUNK;
        loci[i]=3; /* LocalFast */
    }
    return 0;
}
static ABI int32_t playgo_speed(int32_t handle,int32_t speed) { (void)speed; return handle==playgo_handle && handle ? 0 : PLAYGO_BAD_HANDLE; }
static ABI int32_t playgo_get_speed(int32_t handle,int32_t *speed) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!speed) return PLAYGO_BAD_POINTER;
    *speed=2; return 0; /* Full: nothing left to install */
}
/* Progress of installed chunks: done (1 of 1, never a zero total to divide by). */
static ABI int32_t playgo_progress(int32_t handle,const uint16_t *ids,uint32_t count,uint64_t *out) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!ids || !out) return PLAYGO_BAD_POINTER;
    if (!count) return PLAYGO_BAD_SIZE;
    for (uint32_t i=0;i<count;++i) if (ids[i]>=playgo_chunks) return PLAYGO_BAD_CHUNK;
    out[0]=out[1]=1; return 0;
}
static ABI int32_t playgo_todo(int32_t handle,const void *list,uint32_t count) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!list) return PLAYGO_BAD_POINTER;
    return count ? 0 : PLAYGO_BAD_SIZE; /* everything is installed: nothing to schedule */
}
static ABI int32_t playgo_close(int32_t handle) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    playgo_handle=0; return 0;
}
static ABI int32_t playgo_get_language(int32_t handle,uint64_t *mask) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!mask) return PLAYGO_BAD_POINTER;
    *mask=__atomic_load_n(&playgo_language_mask,__ATOMIC_RELAXED); return 0;
}
static ABI int32_t playgo_set_language(int32_t handle,uint64_t mask) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    __atomic_store_n(&playgo_language_mask,mask,__ATOMIC_RELAXED); return 0;
}
static ABI int32_t playgo_eta(int32_t handle,const uint16_t *ids,uint32_t count,int64_t *eta) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!ids || !eta) return PLAYGO_BAD_POINTER;
    if (!count) return PLAYGO_BAD_SIZE;
    for (uint32_t i=0;i<count;++i) if (ids[i]>=playgo_chunks) return PLAYGO_BAD_CHUNK;
    *eta=0; return 0; /* Fully extracted package: no download time remaining. */
}
static ABI int32_t playgo_prefetch(int32_t handle,const uint16_t *ids,uint32_t count,int32_t locus) {
    int64_t eta;
    int32_t result=playgo_eta(handle,ids,count,&eta);
    if (result) return result;
    return locus<0 || locus>3 ? (int32_t)0x80b20006 : 0;
}
static ABI int32_t playgo_get_todo(int32_t handle,void *list,uint32_t capacity,uint32_t *count) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!list || !count) return PLAYGO_BAD_POINTER;
    if (!capacity) return PLAYGO_BAD_SIZE;
    *count=0; return 0; /* All chunks already present locally. */
}
static ABI int32_t playgo_terminate(void) { playgo_handle=0; return 0; }

/* ---- DiscMap: the game is fully installed, no disc bitmap exists ---- */
#define DISC_MAP_NO_BITMAP ((int32_t)0x81100004)
static ABI int32_t discmap_on_hdd(const char *path,int64_t offset,int64_t size,int32_t *result) {
    (void)path; (void)offset; (void)size; (void)result; return DISC_MAP_NO_BITMAP;
}
static ABI int32_t discmap_8a82(const char *path,int64_t offset,int64_t size,int32_t *flags,int32_t *r1,int32_t *r2) {
    (void)path; (void)offset; (void)size; (void)flags; (void)r1; (void)r2; return DISC_MAP_NO_BITMAP;
}

/* ---- Devices that are absent: mouse, microphone, voice chat ---- */
/* A mouse port opens but never reports a connected device. */
static ABI int32_t mouse_open(int32_t user,int32_t type,int32_t index,const void *param) { (void)user; (void)type; (void)index; (void)param; return new_id(); }
static ABI int32_t mouse_read(int32_t handle,unsigned char *data,int32_t count) {
    (void)handle;
    if (!data || count<1) return (int32_t)0x80DF0001;
    memset(data,0,40); return 1; /* timestamp 0, connected=false */
}
static ABI int32_t mouse_close(int32_t handle) { (void)handle; return 0; }
static ABI int32_t audio_in_open(void) { return AUDIO_IN_NOT_OPENED; }


static const RuntimeExport exports[]={
    {"sceUserServiceInitialize",user_initialize}, {"sceUserServiceTerminate",user_terminate},
    {"sceUserServiceGetInitialUser",user_initial}, {"sceUserServiceGetLoginUserIdList",user_list},
    {"sceUserServiceGetUserName",user_name}, {"sceUserServiceGetEvent",user_event},
    {"sceSystemServiceParamGetInt",system_param}, {"sceSystemServiceGetStatus",system_status},
    {"sceSystemServiceReceiveEvent",system_event}, {"sceSystemServiceHideSplashScreen",hide_splash},
    {"sceSystemServiceLaunchWebBrowser",launch_browser},
    {"sceSystemServiceLoadExec",load_exec},
    {"socket",posix_socket_none}, {"accept",posix_socket_badf}, {"bind",posix_socket_badf},
    {"connect",posix_socket_badf}, {"listen",posix_socket_badf}, {"send",posix_socket_badf},
    {"recv",posix_socket_badf}, {"setsockopt",posix_socket_badf}, {"select",posix_socket_badf},
    {"fcntl",posix_fcntl}, {"scePthreadGetthreadid",pthread_thread_id},
    {"sceSystemServiceGetDisplaySafeAreaInfo",safe_area}, {"sceSystemServiceDisableMusicPlayer",disable_music_player},
    {"sceNetInit",net_init}, {"sceNetTerm",net_term}, {"sceNetErrnoLoc",net_errno_loc},
    {"sceNetPoolCreate",net_pool_create}, {"sceNetPoolDestroy",net_pool_destroy},
    {"sceNetEpollCreate",net_epoll_create}, {"sceNetEpollDestroy",net_epoll_destroy},
    {"sceNetResolverCreate",net_resolver_create}, {"sceNetResolverDestroy",net_resolver_destroy},
    {"sceNetHtons",net_htons}, {"sceNetNtohs",net_ntohs}, {"sceNetHtonl",net_htonl}, {"sceNetNtohl",net_ntohl},
    {"sceNetInetPton",net_pton}, {"sceNetInetNtop",net_ntop},
    {"sceNetSocket",net_unreachable}, {"sceNetConnect",net_unreachable}, {"sceNetBind",net_unreachable},
    {"sceNetListen",net_unreachable}, {"sceNetAccept",net_unreachable}, {"sceNetSend",net_unreachable},
    {"sceNetSendto",net_unreachable}, {"sceNetRecv",net_unreachable}, {"sceNetRecvfrom",net_unreachable},
    {"sceNetSetsockopt",net_unreachable}, {"sceNetGetsockopt",net_unreachable},
    {"sceNetGetsockname",net_unreachable}, {"sceNetShutdown",net_unreachable},
    {"sceNetSocketClose",net_unreachable}, {"sceNetSocketAbort",net_unreachable},
    {"sceNetEpollControl",net_unreachable}, {"sceNetEpollWait",net_unreachable}, {"sceNetEpollAbort",net_unreachable},
    {"sceNetResolverStartNtoa",net_unreachable}, {"sceNetResolverStartAton",net_unreachable},
    {"sceNetCtlGetState",netctl_state}, {"sceNetCtlGetInfo",netctl_info},
    {"sceNetCtlRegisterCallback",netctl_register}, {"sceNetCtlCheckCallback",netctl_check},
    {"sceNetCtlUnregisterCallback",netctl_unregister}, {"sceNetCtlGetNatInfo",netctl_nat},
    {"sceSslInit",lib_init_id}, {"sceSslTerm",ok_void},
    {"sceHttpInit",lib_init_id}, {"sceHttpTerm",ok_void},
    {"sceHttpCreateTemplate",http_object}, {"sceHttpDeleteTemplate",ok_void},
    {"sceHttpCreateConnectionWithURL",http_object}, {"sceHttpCreateRequestWithURL",http_object},
    {"sceHttpSendRequest",http_fail}, {"sceHttpCreateEpoll",http_epoll},
    {"sceHttpSetNonblock",ok_void}, {"sceHttpSetConnectTimeOut",ok_void},
    {"sceHttpsEnableOption",ok_void}, {"sceHttpsDisableOption",ok_void},
    {"sceHttpAddRequestHeader",ok_void}, {"sceHttpSetRequestContentLength",ok_void},
    {"sceHttpDeleteConnection",ok_void}, {"sceHttpDeleteRequest",ok_void},
    {"sceHttpAbortWaitRequest",ok_void}, {"sceHttpDestroyEpoll",ok_void},
    {"sceHttpSetEpoll",ok_void}, {"sceHttpUnsetEpoll",ok_void}, {"sceHttpWaitRequest",http_wait},
    {"sceHttpGetStatusCode",http_fail}, {"sceHttpGetResponseContentLength",http_fail},
    {"sceHttpReadData",http_fail},
    {"sceNpGetState",np_state}, {"sceNpGetOnlineId",np_signed_out}, {"sceNpGetNpId",np_signed_out},
    {"sceNpRegisterStateCallback",np_register}, {"sceNpUnregisterStateCallback",ok_void},
    {"sceNpRegisterGamePresenceCallback",np_register_void}, {"sceNpRegisterPlusEventCallback",np_register},
    {"sceNpUnregisterPlusEventCallback",ok_void}, {"sceNpCheckCallback",ok_void},
    {"sceNpSetNpTitleId",ok_void}, {"sceNpNotifyPlusFeature",ok_void}, {"sceNpSetContentRestriction",ok_void},
    {"sceNpCreateAsyncRequest",np_request}, {"sceNpDeleteRequest",ok_void}, {"sceNpAbortRequest",ok_void},
    {"sceNpPollAsync",np_poll}, {"sceNpCheckNpAvailability",np_signed_out},
    {"sceNpGetParentalControlInfo",np_signed_out}, {"sceNpCheckPlus",np_signed_out},
    {"sceNpGetGamePresenceStatus",np_signed_out},
    {"sceNpTusCreateNpTitleCtx",np_tus_no_login}, {"sceNpTusCreateRequest",np_tus_invalid_id},
    {"sceNpTusDeleteNpTitleCtx",np_tus_invalid_id}, {"sceNpTusDeleteRequest",np_tus_invalid_id},
    {"sceNpTusPollAsync",np_tus_invalid_id}, {"sceNpTusGetDataAsync",np_tus_invalid_id},
    {"sceNpTusGetFriendsDataStatusAsync",np_tus_invalid_id}, {"sceNpTusSetDataAsync",np_tus_invalid_id},
    {"sceNpCmpNpId",np_compare}, {"sceNpCmpOnlineId",np_compare},
    {"sceNpAuthCreateAsyncRequest",np_request}, {"sceNpAuthDeleteRequest",ok_void},
    {"sceNpAuthPollAsync",np_poll}, {"sceNpAuthGetAuthorizationCode",np_signed_out},
    {"sceNpLookupCreateTitleCtx",np_request}, {"sceNpLookupDeleteTitleCtx",ok_void},
    {"sceNpLookupCreateAsyncRequest",np_request_ctx}, {"sceNpLookupDeleteRequest",ok_void},
    {"sceNpLookupAbortRequest",ok_void}, {"sceNpLookupPollAsync",np_poll}, {"sceNpLookupNpId",np_signed_out},
    {"sceNpScoreCreateNpTitleCtx",np_request_ctx}, {"sceNpScoreDeleteNpTitleCtx",ok_void},
    {"sceNpScoreCreateRequest",np_request}, {"sceNpScoreDeleteRequest",ok_void}, {"sceNpScoreAbortRequest",ok_void},
    {"sceNpWebApiInitialize",lib_init_id}, {"sceNpWebApiTerminate",ok_void},
    {"sceNpWebApiCreateContext",np_signed_out},
    {"sceNpWebApiCreateRequest",np_signed_out}, {"sceNpWebApiSendRequest",np_signed_out},
    {"sceNpWebApiDeleteRequest",ok_void}, {"sceNpWebApiAbortRequest",ok_void},
    {"sceNpWebApiDeleteContext",ok_void}, {"sceNpWebApiReadData",np_signed_out},
    {"sceNpWebApiGetHttpStatusCode",np_signed_out}, {"sceNpWebApiGetHttpResponseHeaderValue",np_signed_out},
    {"sceNpWebApiGetHttpResponseHeaderValueLength",np_signed_out},
    {"sceNpWebApiCreatePushEventFilter",np_signed_out}, {"sceNpWebApiDeletePushEventFilter",ok_void},
    {"sceNpWebApiRegisterPushEventCallback",np_signed_out}, {"sceNpWebApiUnregisterPushEventCallback",ok_void},
    {"sceNpWebApiUtilityParseNpId",np_signed_out},
    {"sceNpMatching2ContextStart",np_signed_out}, {"sceNpMatching2ContextStop",ok_void},
    {"sceNpMatching2DestroyContext",ok_void},
    {"sceNpMatching2RegisterContextCallback",ok_void}, {"sceNpMatching2RegisterLobbyEventCallback",ok_void},
    {"sceNpMatching2RegisterRoomEventCallback",ok_void}, {"sceNpMatching2RegisterSignalingCallback",ok_void},
    {"sceNpMatching2SetDefaultRequestOptParam",ok_void},
    {"sceNpMatching2CreateJoinRoom",np_signed_out}, {"sceNpMatching2JoinRoom",np_signed_out},
    {"sceNpMatching2LeaveRoom",np_signed_out}, {"sceNpMatching2SearchRoom",np_signed_out},
    {"sceNpMatching2GetServerId",np_signed_out}, {"sceNpMatching2GetWorldInfoList",np_signed_out},
    {"sceNpMatching2GetLobbyInfoList",np_signed_out}, {"sceNpMatching2JoinLobby",np_signed_out},
    {"sceNpMatching2LeaveLobby",np_signed_out}, {"sceNpMatching2GrantRoomOwner",np_signed_out},
    {"sceNpMatching2KickoutRoomMember",np_signed_out}, {"sceNpMatching2SetRoomDataExternal",np_signed_out},
    {"sceNpMatching2SetRoomDataInternal",np_signed_out}, {"sceNpMatching2SetRoomMemberDataInternal",np_signed_out},
    {"sceNpMatching2SignalingGetConnectionStatus",np_signed_out}, {"sceNpMatching2SignalingGetPingInfo",np_signed_out},
    {"sceNpSignalingDeleteContext",ok_void}, {"sceNpSignalingActivateConnection",np_signed_out},
    {"sceNpSignalingDeactivateConnection",ok_void}, {"sceNpSignalingGetConnectionStatus",np_signed_out},
    {"sceNpScoreCensorComment",np_signed_out}, {"sceNpScoreSanitizeComment",np_signed_out},
    {"sceNpScoreGetBoardInfo",np_signed_out}, {"sceNpScoreGetGameData",np_signed_out},
    {"sceNpScoreGetRankingByNpIdPcId",np_signed_out}, {"sceNpScoreGetRankingByRange",np_signed_out},
    {"sceNpScoreRecordGameData",np_signed_out}, {"sceNpScoreRecordScore",np_signed_out},
    {"sceNpScoreSetPlayerCharacterId",ok_void},
    {"sceVoiceCreatePort",voice_port}, {"sceVoiceDeletePort",ok_void},
    {"sceVoiceConnectIPortToOPort",ok_void}, {"sceVoiceDisconnectIPortFromOPort",ok_void},
    {"sceVoiceStart",ok_void}, {"sceVoiceStop",ok_void}, {"sceVoiceGetPortInfo",voice_info},
    {"sceVoiceReadFromOPort",voice_read}, {"sceVoiceWriteToIPort",voice_write},
    {"sceAudioInInput",audio_in_open}, {"sceAudioInClose",audio_in_open},
    {"sceNpMatching2Initialize",ok_void}, {"sceNpMatching2Terminate",ok_void},
    {"sceNpMatching2CreateContext",np_signed_out},
    {"sceNpSignalingInitialize",ok_void}, {"sceNpSignalingTerminate",ok_void},
    {"sceNpSignalingGetConnectionInfo",np_signed_out}, {"sceNpSignalingTerminateConnection",ok_void},
    {"sceNpSignalingGetPeerNetInfo",np_signed_out}, {"sceNpScorePollAsync",np_poll},
    {"sceNpSignalingCreateContext",np_signed_out},
    {"sceCommonDialogInitialize",common_init},
    {"sceMsgDialogInitialize",msg_init}, {"sceMsgDialogOpen",msg_open},
    {"sceMsgDialogUpdateStatus",msg_status}, {"sceMsgDialogTerminate",msg_term},
    {"sceMsgDialogClose",msg_close}, {"sceMsgDialogProgressBarSetValue",msg_progress},
    {"sceMsgDialogGetResult",msg_result}, {"sceMsgDialogGetStatus",msg_status},
    {"sceSaveDataDialogInitialize",save_init}, {"sceSaveDataDialogOpen",save_open},
    {"sceSaveDataDialogUpdateStatus",save_status}, {"sceSaveDataDialogTerminate",save_term},
    {"sceSaveDataDialogGetStatus",save_status}, {"sceSaveDataDialogClose",save_close},
    {"sceSaveDataDialogProgressBarSetValue",save_progress}, {"sceSaveDataDialogGetResult",save_result},
    {"sceSaveDataDialogIsReadyToDisplay",save_ready},
    {"sceNpProfileDialogInitialize",profile_init}, {"sceNpProfileDialogOpen",profile_open},
    {"sceNpProfileDialogUpdateStatus",profile_status}, {"sceNpProfileDialogTerminate",profile_term},
    {"sceNpProfileDialogGetResult",profile_result},
    {"sceNpCommerceDialogInitialize",commerce_init}, {"sceNpCommerceDialogOpen",commerce_open},
    {"sceNpCommerceDialogUpdateStatus",commerce_status}, {"sceNpCommerceDialogTerminate",commerce_term},
    {"sceImeDialogInit",ime_init}, {"sceImeDialogGetStatus",ime_status}, {"sceImeDialogGetResult",ime_result},
    {"sceImeDialogTerm",ime_term}, {"sceImeDialogAbort",ime_term},
    {"sceNpTrophyCreateContext",trophy_context}, {"sceNpTrophyCreateHandle",trophy_handle},
    {"sceNpTrophyRegisterContext",trophy_register}, {"sceNpTrophyUnlockTrophy",trophy_unlock},
    {"sceNpTrophyGetGameInfo",trophy_game_info}, {"sceNpTrophyGetTrophyInfo",trophy_info},
    {"sceNpTrophyGetTrophyUnlockState",trophy_unlock_state}, {"sceNpTrophyAbortHandle",trophy_release},
    {"sceNpTrophyDestroyHandle",trophy_release}, {"sceNpTrophyDestroyContext",trophy_release},
    {"scePlayGoInitialize",playgo_init}, {"scePlayGoOpen",playgo_open}, {"scePlayGoGetChunkId",playgo_chunk_ids},
    {"scePlayGoGetLocus",playgo_locus}, {"scePlayGoSetInstallSpeed",playgo_speed},
    {"scePlayGoGetInstallSpeed",playgo_get_speed}, {"scePlayGoGetProgress",playgo_progress},
    {"scePlayGoSetToDoList",playgo_todo}, {"scePlayGoClose",playgo_close}, {"scePlayGoTerminate",playgo_terminate},
    {"scePlayGoGetLanguageMask",playgo_get_language}, {"scePlayGoSetLanguageMask",playgo_set_language},
    {"scePlayGoGetEta",playgo_eta}, {"scePlayGoPrefetch",playgo_prefetch}, {"scePlayGoGetToDoList",playgo_get_todo},
    {"sceMouseInit",ok_void}, {"sceMouseOpen",mouse_open}, {"sceMouseRead",mouse_read}, {"sceMouseClose",mouse_close},
    {"sceAudioInOpen",audio_in_open},
    {"sceDiscMapIsRequestOnHDD",discmap_on_hdd}, {"sceDiscMap_8A828CAEE7EDD5E9",discmap_8a82},
    {"sceVoiceInit",ok_void}, {"sceVoiceEnd",ok_void},
};
/* ---- Network trace (GOW3_NET_TRACE=1) ----
 * Wraps every sceNet/sceNetCtl/sceNp (except trophies) and sceVoice import in a thunk that
 * prints its name, first four arguments and result: the map of what the game asks the network
 * and PSN for. The first NET_TRACE_CALLS calls of each function are printed. The thunks pass
 * six integer registers through, which covers every contract above (none takes stack
 * arguments or floating-point values). */
#define NET_TRACE_SLOTS 256
#define NET_TRACE_CALLS 20
typedef ABI uint64_t (*TraceTarget)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
static struct { const char *name; TraceTarget target; unsigned calls; } traced[NET_TRACE_SLOTS];
static int traced_count;
static void *trace_thunks[NET_TRACE_SLOTS];
/* 24 Oct 2026: trace_thunk_<digits> used to hand its digit suffix to trace_call as a plain
 * literal (e.g. trace_call(0010,...)); a leading-zero C literal is octal, so most thunks passed
 * a slot far from their real position in trace_thunks[] and ended up reading an unregistered
 * traced[] entry (null target), crashing the game on the first such call entering Uncharted 2
 * multiplayer. Thunks now pass their own address; trace_call finds its slot by searching
 * trace_thunks[] for it (256 entries at most, and only while net tracing is on). */
static uint64_t trace_call(void *self,uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    int slot=-1;
    for (int i=0;i<traced_count;++i) if (trace_thunks[i]==self) { slot=i; break; }
    if (slot<0) { printf("Net trace: unregistered thunk called; call skipped\n"); return 0; }
    TraceTarget target=traced[slot].target;
    if (!target) {
        printf("Net trace: slot %d (%s) has no bound target; call skipped\n",slot,
               traced[slot].name ? traced[slot].name : "?");
        return 0;
    }
    uint64_t result=target(a,b,c,d,e,f);
    unsigned n=__atomic_add_fetch(&traced[slot].calls,1,__ATOMIC_RELAXED);
    if (n<=NET_TRACE_CALLS)
        printf("Net trace: %s(0x%llx, 0x%llx, 0x%llx, 0x%llx) = 0x%08x%s\n",traced[slot].name,
               (unsigned long long)a,(unsigned long long)b,(unsigned long long)c,(unsigned long long)d,
               (uint32_t)result,n==NET_TRACE_CALLS ? " (further calls not printed)" : "");
    return result;
}
#define TRACE_THUNK(i) static ABI uint64_t trace_thunk_##i(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) \
    { return trace_call((void *)trace_thunk_##i,a,b,c,d,e,f); }
#define TRACE_THUNK4(i) TRACE_THUNK(i##0) TRACE_THUNK(i##1) TRACE_THUNK(i##2) TRACE_THUNK(i##3)
#define TRACE_THUNK16(i) TRACE_THUNK4(i##0) TRACE_THUNK4(i##1) TRACE_THUNK4(i##2) TRACE_THUNK4(i##3)
#define TRACE_THUNK64(i) TRACE_THUNK16(i##0) TRACE_THUNK16(i##1) TRACE_THUNK16(i##2) TRACE_THUNK16(i##3)
TRACE_THUNK64(0) TRACE_THUNK64(1) TRACE_THUNK64(2) TRACE_THUNK64(3)
#define TRACE_REF(i) trace_thunk_##i,
#define TRACE_REF4(i) TRACE_REF(i##0) TRACE_REF(i##1) TRACE_REF(i##2) TRACE_REF(i##3)
#define TRACE_REF16(i) TRACE_REF4(i##0) TRACE_REF4(i##1) TRACE_REF4(i##2) TRACE_REF4(i##3)
#define TRACE_REF64(i) TRACE_REF16(i##0) TRACE_REF16(i##1) TRACE_REF16(i##2) TRACE_REF16(i##3)
static void *const trace_thunks_init[NET_TRACE_SLOTS]={ TRACE_REF64(0) TRACE_REF64(1) TRACE_REF64(2) TRACE_REF64(3) };
static int net_traced(const char *symbol) {
    static int enabled=-1;
    if (enabled<0) {
        const char *v=getenv("GOW3_NET_TRACE");
        enabled=v && v[0]=='1';
        memcpy(trace_thunks,trace_thunks_init,sizeof(trace_thunks));
    }
    if (!enabled || !symbol) return 0;
    if (!strncmp(symbol,"sceNpTrophy",11)) return 0;
    if (!strcmp(symbol,"sceNetHtons") || !strcmp(symbol,"sceNetNtohs") ||
        !strcmp(symbol,"sceNetHtonl") || !strcmp(symbol,"sceNetNtohl")) return 0;
    return !strncmp(symbol,"sceNet",6) || !strncmp(symbol,"sceNp",5) || !strncmp(symbol,"sceVoice",8);
}
static uintptr_t net_trace_wrap(const char *nid,uintptr_t function) {
    const char *symbol=runtime_symbol(nid);
    if (!function || !net_traced(symbol)) return function;
    pthread_mutex_lock(&lock);
    for (int i=0;i<traced_count;++i)
        if (!strcmp(traced[i].name,symbol)) { pthread_mutex_unlock(&lock); return (uintptr_t)trace_thunks[i]; }
    if (traced_count==NET_TRACE_SLOTS) { pthread_mutex_unlock(&lock); return function; }
    int slot=traced_count++;
    traced[slot].name=symbol; traced[slot].target=(TraceTarget)function;
    pthread_mutex_unlock(&lock);
    printf("Net trace: bound %s\n",symbol);
    return (uintptr_t)trace_thunks[slot];
}
uintptr_t runtime_services_resolve(const char *name) {
    if (!strcmp(name,"k1SwgkMSOM8#libSceRemoteplay")) return (uintptr_t)remoteplay_initialize;
    return net_trace_wrap(name,RUNTIME_LOOKUP(exports,name));
}
