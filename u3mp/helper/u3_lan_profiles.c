/* U3 LAN profile restoration: native IME provider, private persistence and
 * the existing type-0xcf frontend list. No network/session success is faked.
 */
#include <stdint.h>
#include <stddef.h>
#define MAGIC 0x504c3355u
#define VERSION 1u
#define LIMIT 8u
#define NAME_BYTES 64u

typedef struct { uint32_t magic,version,count,selected; char names[LIMIT][NAME_BYTES]; } ProfileFile;
typedef struct {
 uintptr_t init,status,result,term,open,read,write,close,rename;
 uint32_t initialized,phase,error,reserved;
 uint16_t text[16];
 ProfileFile profiles;
 uintptr_t show_page,lookup,loader,pool_init;
 uint32_t startup_phase,startup_error,startup_ticks,lobby_selected;
 uint32_t focus_done,focus_page,focus_moves,focus_busy;
 uint32_t k6_calls,k6_last,focus_tries,focus_fail;
 uint32_t lobby_error,reserved3;
 uint16_t ime_title[12];  /* IME title for the clan tag dialog (was padding) */
 uintptr_t profile_base;
 uintptr_t natives[64];  /* relocated native PS4 functions, see NATIVE_* */
} ProfileState;
#define NATIVE_SET_FOCUS 0   /* 0x623660 page SetFocusedControl(page,control) */
#define NATIVE_NMH_REFRESH 1 /* 0x7503c0 NetMenuHost::Refresh (PS3 0x32ada0) */
#define NATIVE_SELECT_NETGAME 2 /* 0x720610 NetGameManager::SetSelectedNetGameIdx (PS3 0x30573c) */
#define NATIVE_NMH_SELECT_TYPE 3 /* 0x750a90 NetMenuHost select game type (PS3 0x32b530) */
#define NATIVE_TEXT_FIND 4 /* 0xf32ae0 text table find(db,id) (PS3 0x799ed8) */
#define NATIVE_DS_GAMETYPE 5 /* 0x752cf0 datasource 0x26 LIST_CUSTUMGAMETYPE (PS3 0x32ed40) */
#define NATIVE_DS_MAP 6 /* 0x753330 datasource 0x27 LIST_CUSTUMMAP (PS3 0x33ca24) */
#define NATIVE_COUNTDOWN_DONE 7 /* 0x66e9b0 match countdown done -> session Create (PS3 0x288d1c) */
#define NATIVE_CREATE_PLAYER 8 /* 0x72eca0 NetGameManager create net player for session member (PS3 0x304948) */
#define NATIVE_FIND_MODULE 9 /* 0x11fd4c0 find loaded DC module holding a symbol (sentinel 0x1b3af40 if none) */
#define NATIVE_EMBLEM_SETUP 10 /* 0x66b1d0 build player emblems from net symbols 0xc7f8567c/0xe2311588/0x3ffae77 */
#define NATIVE_EMBLEM_INIT 11 /* 0x66b510 reset emblem state 0x17c3440 and fill emblem table 0x1b67e60 */
#define NATIVE_MENU_BUTTON 14 /* 0x64ce90 Menu::OnButton(menu 0x17bd080, action id, 0) = PS3 f_265c94 */
#define NATIVE_DS_TAB_PROFILE 15 /* 0x76bbf0 datasource 0xc3 TAB_NET_PROFILE (PS3 0x340290) */
#define NATIVE_DS_EMBLEM_LAYER 16 /* 0x7699e0 datasource 0xb5 NET_EMBLEM_LAYER (PS3 0x3354d8) */
#define NATIVE_DS_EMBLEM_TYPE 17 /* 0x76a580 datasource 0xbe NET_EMBLEM_TYPE (PS3 0x332f64) */
#define NATIVE_EVAL_LAMBDA 18 /* 0x11f2170 script lambda evaluate(lambda,0,0) */
#define NATIVE_MEDAL_EARNED 19 /* 0x5ada60 medal earned count(counts 0x17bb878, record, user) (PS3 f_59874) */
#define NATIVE_VTEX_FIND 20 /* 0x1189430 vtex texture by name(mgr 0x1b31128, name) (PS3 f_920cd0(0x1369000,..)) */
#define NATIVE_PROFILE_RANK 21 /* 0x781ea0 rank of a player profile (used by the profile box 0x761ea0 for its rank badge) */
#define NATIVE_DS_TIME_LIMIT 22 /* 0x753b70 datasource 0x29 custom-game time limit (PS3 0x3310a8) */
#define NATIVE_BOOSTER_LEVEL 23 /* 0x6882b0 booster level(mgr 0x17c3b40,user,def) (PS3 f_29cde8) */
#define NATIVE_UNLOCK_FIND 24 /* 0x7bb470 unlock entry(mgr 0x17f3068,id,0) (PS3 f_38b054) */
#define NATIVE_UNLOCK_IS 25 /* 0x7bba70 unlocked(mgr,user,entry) (PS3 f_38b390) */
#define NATIVE_ITEM_TEXT 26 /* 0x751b30 unlock/price text(id,mod) (PS3 f_32d4d8) */
#define NATIVE_ONLINE_ALLOWED 27 /* 0x7270f0 online features allowed(netgame mgr 0x17cb060) (PS3 f_30b3a4) */
#define NATIVE_SET_LOADOUT 28 /* 0x780220 SetLoadoutData(profile,item,value,loadout) (PS3 f_355e94) */
#define NATIVE_BOOSTER_LIST 29 /* 0x7596a0 build booster list(list,coop) (PS3 f_32cb90) */
#define NATIVE_BOOSTER_FIND 30 /* 0x687d30 booster def by id(mgr,id) (PS3 f_29d33c) */
#define NATIVE_MISSION_NEXT 31 /* 0x775d30 next mission tier(missions 0x17da200+user*0x1018,id) (PS3 f_34b1ac) */
#define NATIVE_UNLOCK_SEEN 32 /* 0x7bda50 mark seen(mgr,user,id,0) (PS3 f_38c9f0) */
#define NATIVE_KICKBACK_LEVEL 33 /* 0x74eba0 kickback level(state,entry) (PS3 f_328ccc) */
#define NATIVE_GET_LOADOUT 34 /* 0x7800d0 GetLoadoutData(profile,item,loadout) (PS3 f_355f5c) */
#define NATIVE_UNLOCK_STATUS 35 /* 0x7bca00 unlock status(mgr,user,entry): 0 owned, 1 buyable (PS3 f_38cde0) */
#define NATIVE_CAN_BUY 36 /* 0x7bd6f0 can buy(mgr,user,id,mod) (PS3 f_38be9c) */
#define NATIVE_UNLOCK_NEW 37 /* 0x7bdc70 any type-6/7 unlock of item available(mgr,user,id) (PS3 f_38ba84) */
#define NATIVE_TEXT_NUM 38 /* 0xf32e00 text with number(db 0x17026b8,buf,size,id,value,0) (PS3 f_79a604) */
#define NATIVE_MISSION_CAN 39 /* 0x771cf0 mission can be taken(missions,mission,0) (PS3 f_34bf40) */
#define NATIVE_NAME_HASH 40 /* 0x10e95b0 64-bit name hash for level requests */
#define NATIVE_LEVEL_REQUEST 41 /* 0x10eb990 request level(world 0x1afcf10, req) (PS3 f_89f0f0) */
#define NATIVE_LEVEL_FIND 42 /* 0x10eb1d0 find level(world, sid) (PS3 f_8a0118); state +0x30 == 8 loaded */
#define NATIVE_DS_PROFILE_BOX 43 /* 0x761ea0 datasource 0x89 profile box */
#define NATIVE_NMH_MAP_INDEX 44 /* 0x7508f0 NetMenuHost selected map index(&0x17d7b30) (PS3 f_32b688) */
#define NATIVE_LEVEL_SHOW 45 /* 0x10f25b0 Level show/hide (level, bool), used by the lobby stage loader 0x743420 */
#define NATIVE_MP_CAMERA 47 /* 0x5b4e60 MenuScene lobby camera at stage menu-spawner-1 (stage name at netgame+0x74b8); orphaned on PS4 (PS3 called it from the frontend loop) */
#define NATIVE_MP_SCENE_ENTER 46 /* 0x5b7020 MenuScene(0x17bba90).state=2 + MP loadout packages (0x997230 on 0x17f9938); orphaned on PS4 */
#define NATIVE_LOOKUP_SYMBOL 13 /* 0x11f2e30 ScriptManager LookupSymbolValue(id) */
#define NATIVE_SESSION_RESET 12 /* 0x7a2360 match session Init/Reset (op-done flags +0xa1..+0xf0 = 1, magic 0x1234) */
/* Diagnostic probe of the custom-game option sources, read by run_u3_probe.py at state+1280. */
typedef struct { uint32_t magic,runs,nmh_count,selected_netgame,mode,lan,flag65,gt_max,gt_sel,map_max,map_sel,game_array,game_count,name_array,name_count,party;
 char gt0[32],gt1[32],map0[32]; uint32_t time_raw,score_raw,start_calls,create_calls,backend_calls,last_slot,net_state,session_ready,alloc_lo,alloc_hi,net_alias,pad2; uint64_t game_array_ptr; } Probe;
/* LAN session backend (PS3 LAN backend class, ctor 0x371920 vtable 0x10e7f68; PS4 slots are the
 * PS3 slot offsets x2). Lives at state+1536, vtable at state+1600. Solo host for now: Create fills
 * the local member into g_matchSession (0x17d6420) the way the PS3 room-state reply (0xc9) does. */
typedef struct { void **vtable; uintptr_t base; ProfileState *state; uint32_t created,pad; } LanBackend;
typedef struct {
 int32_t user;uint32_t type;uint64_t languages;uint32_t enter_label,input_method;
 void *filter;uint32_t option,max_length;uint16_t *buffer;
 float x,y;uint32_t halign,valign;const uint16_t *placeholder,*title;int8_t reserved[16];
} ImeParam;
_Static_assert(offsetof(ProfileState,text)==88,"state ABI");
_Static_assert(offsetof(ProfileState,show_page)==648,"startup binding ABI");
_Static_assert(offsetof(ProfileState,natives)==768,"native table ABI");
_Static_assert(offsetof(ImeParam,buffer)==40,"IME ABI");
_Static_assert(sizeof(ProfileFile)==528,"profile storage ABI");
static const char path[]="/data/u3-lan-profiles.v1";
static const char temporary[]="/data/u3-lan-profiles.v1.tmp";
static const uint16_t prompt[]={ 'L','A','N',' ','P','r','o','f','i','l','e',' ','N','a','m','e',0 };
static void clear(void *p,size_t n){uint8_t *b=p;while(n--)*b++=0;}
static void copy(void *d,const void *s,size_t n){uint8_t *a=d;const uint8_t*b=s;while(n--)*a++=*b++;}
int u3_profiles_valid(const ProfileFile *p){
 if(p->magic!=MAGIC||p->version!=VERSION||p->count>LIMIT||p->selected>p->count)return 0;
 for(uint32_t i=0;i<p->count;i++){
  if(!p->names[i][0])return 0;
  unsigned n=0;while(n<NAME_BYTES&&p->names[i][n])n++;
  if(n==NAME_BYTES)return 0;
 }
 return 1;
}
/* Same forbidden LAN name characters normalized by the PS3 acceptance path.
 * UTF-16 input is limited to 15 code units by the actual IME provider.
 */
int u3_profile_name(const uint16_t *in,char out[NAME_BYTES]){
 unsigned n=0;clear(out,NAME_BYTES);
 for(unsigned i=0;i<15&&in[i];i++){
  uint32_t c=in[i];
  if(c<32||c==127)return 0;
  if(c=='|'||c=='\\'||c=='.')c='*';
  if(c>=0xd800&&c<=0xdbff){if(i+1>=15||in[i+1]<0xdc00||in[i+1]>0xdfff)return 0;c=0x10000+((c-0xd800)<<10)+(in[++i]-0xdc00);}
  else if(c>=0xdc00&&c<=0xdfff)return 0;
  if(c<0x80)out[n++]=(char)c;
  else if(c<0x800){out[n++]=(char)(0xc0|(c>>6));out[n++]=(char)(0x80|(c&63));}
  else if(c<0x10000){out[n++]=(char)(0xe0|(c>>12));out[n++]=(char)(0x80|((c>>6)&63));out[n++]=(char)(0x80|(c&63));}
  else{out[n++]=(char)(0xf0|(c>>18));out[n++]=(char)(0x80|((c>>12)&63));out[n++]=(char)(0x80|((c>>6)&63));out[n++]=(char)(0x80|(c&63));}
 }
 unsigned first=0;while(first<n&&out[first]==' ')first++;
 while(n>first&&out[n-1]==' ')n--;
 if(first){for(unsigned i=first;i<n;i++)out[i-first]=out[i];n-=first;}
 out[n]=0;return n!=0;
}
#ifndef U3_PROFILE_TEST
static void load(ProfileState *s){
 if(s->initialized)return;
 clear(&s->profiles,sizeof(s->profiles));s->profiles.magic=MAGIC;s->profiles.version=VERSION;
 int64_t fd=((int64_t(*)(const char*,int,int))s->open)(path,0,0);
 if(fd>=0){
  ProfileFile file;int64_t n=((int64_t(*)(int,void*,uint64_t))s->read)((int)fd,&file,sizeof(file));
  ((int64_t(*)(int))s->close)((int)fd);
  if(n==(int64_t)sizeof(file)&&u3_profiles_valid(&file))copy(&s->profiles,&file,sizeof(file));
  else s->error=1;
 }
 s->initialized=1;
}
static int save(ProfileState *s,const ProfileFile *p){
 int64_t fd=((int64_t(*)(const char*,int,int))s->open)(temporary,0x601,0600);
 if(fd<0)return 0;
 size_t written=0;
 while(written<sizeof(*p)){
  int64_t n=((int64_t(*)(int,const void*,uint64_t))s->write)((int)fd,(const uint8_t*)p+written,sizeof(*p)-written);
  if(n<=0||n>(int64_t)(sizeof(*p)-written)){((int64_t(*)(int))s->close)((int)fd);return 0;}
  written+=(size_t)n;
 }
 if(((int64_t(*)(int))s->close)((int)fd)<0)return 0;
 return ((int64_t(*)(const char*,const char*))s->rename)(temporary,path)==0;
}

/* Retag the stripped lobby datasources (b3 LIST_MAIN_MENU, ad LIST_PLAYERS) on PAGE_MULTIPLAYER
 * and PAGE_LOBBY to the cf handler hook with a LAN tag in the unused parameter. */
static int retag_page(uint8_t *page,uint32_t count){
 uint8_t *collection=*(uint8_t**)(page+0x48);if(!collection||*(uint32_t*)collection!=count)return 0;
 uint8_t *items=*(uint8_t**)(collection+8);if(!items)return 0;
 for(unsigned j=0;j<count;j++){
  uint8_t *item=items+j*0x68;uint32_t id=*(uint32_t*)(item+8),tag=0;
  uint32_t en=*(uint32_t*)(item+0x34);
  if(id==0xf75ffe57)tag=0x4c414eb3;
  else if(id==0x3406420c)tag=0x4c414ead;
  else if(id==0x6c3c0690)tag=0x4c414e39;   /* LIST_CINEMA: PS4 0x39 stub (0x755850), PS3 f_32f1a8 */
  else if(count==9&&(en==0x1e||en==0x32||(en==0xcf&&(*(uint32_t*)(item+0x40)>>8)==0x4c414e)))tag=0x4c414e00|((en==0xcf)?(*(uint32_t*)(item+0x40)&255):en);
  else continue;
  uint32_t original=tag&255;
  if(*(uint32_t*)(item+0x30)!=21)return 0;
  if(*(uint32_t*)(item+0x34)==0xcf&&*(uint32_t*)(item+0x40)==tag)continue;
  if(*(uint32_t*)(item+0x34)!=original||*(uint32_t*)(item+0x40)!=0)return 0;
  *(uint32_t*)(item+0x34)=0xcf;*(uint32_t*)(item+0x40)=tag;
 }
 return 1;
}
/* LIST_PLAYERS (PS4 0xad, a RET stub at 0x76c4c0 = PS3 f_34634c) sits on every multiplayer page
 * (PAGE_NET_PROFILE, PAGE_EMBLEM, PAGE_TAUNT, the character pages...). The stub leaves the event
 * defaults, so the party box takes focus and X goes to its click handler (0x674120 registration)
 * instead of the page's own list. Give every page's party box the same LAN source as the lobby. */
static void retag_party(uint8_t *page){
 uint8_t *collection=*(uint8_t**)(page+0x48);if(!collection)return;
 uint32_t count=*(uint32_t*)collection;uint8_t *items=*(uint8_t**)(collection+8);
 if(!items||count>64)return;
 for(unsigned j=0;j<count;j++){
  uint8_t *item=items+j*0x68;
  if(*(uint32_t*)(item+8)!=0x3406420c||*(uint32_t*)(item+0x30)!=21)continue;
  if(*(uint32_t*)(item+0x34)==0xad&&*(uint32_t*)(item+0x40)==0){*(uint32_t*)(item+0x34)=0xcf;*(uint32_t*)(item+0x40)=0x4c414ead;}
 }
 for(unsigned j=0;j<count;j++){
  uint8_t *item=items+j*0x68;
  if(*(uint32_t*)(item+8)!=0x97d33e70||*(uint32_t*)(item+0x30)!=21)continue;
  if(*(uint32_t*)(item+0x34)==0xc3&&*(uint32_t*)(item+0x40)==0){*(uint32_t*)(item+0x34)=0xcf;*(uint32_t*)(item+0x40)=0x4c414ec3;}
 }
 for(unsigned j=0;j<count;j++){
  uint8_t *item=items+j*0x68;uint32_t id=*(uint32_t*)(item+8),en=*(uint32_t*)(item+0x34);
  if(*(uint32_t*)(item+0x30)!=21||*(uint32_t*)(item+0x40)!=0)continue;
  if((id==0x653f2db1&&en==0xb5)||(id==0xef2a1452&&en==0xbe)||(id==0xce60491f&&(en==0xd3||en==0x43))||(id==0x92a259b3&&en==0x44)||en==0xa6||en==0xa7||en==0xd9||en==0xd4||en==0x89||en==0x31||(id==0&&en==0x29)){*(uint32_t*)(item+0x34)=0xcf;*(uint32_t*)(item+0x40)=0x4c414e00|en;}
 }
}
static int prepare_lobby_menu(int32_t *header){
 if(*(uint32_t*)header!=149)return 0;
 uint8_t *pages=*(uint8_t**)((uint8_t*)header+8);if(!pages)return 0;
 int ok=0;
 for(unsigned i=0;i<149;i++){
  uint8_t *page=pages+i*0x80;uint32_t id=*(uint32_t*)(page+8);
  if(id==0x53cbb5f2)ok=retag_page(page,4);
  else if(id==0xb99a3235)retag_page(page,9);
  else if(id==0x735c4dbe)retag_page(page,1); /* PAGE_MULTIPLAYER_CINEMA_LIST */
  else retag_party(page);
 }
 return ok;
}
/* The stripped main-list click registration is rebuilt only after the
 * existing LAN profile registration proves that this native table is ready.
 * Entry layout/capacity follow 0x6747xx: id, padding, callback, context (24 B).
 */
static int register_lobby_click(uintptr_t base){
 uint32_t *count=(uint32_t*)(base+0x17be5dc);uint8_t *table=(uint8_t*)(base+0x17be5e0);
 uint32_t n=*count;if(n>128)return 0;
 int ready=0;
 for(uint32_t i=0;i<n;i++){
  uint8_t *entry=table+(size_t)i*24;uint32_t id=*(uint32_t*)entry;
  if(id==0xf75ffe57)return *(uintptr_t*)(entry+8)==base+0x672c60&&*(uintptr_t*)(entry+16)==0;
  if(id==0xbe55365f&&*(uintptr_t*)(entry+8)==base+0x672c60)ready=1;
 }
 if(!ready||n==128)return 0;
 uint8_t *entry=table+(size_t)n*24;
 clear(entry,24);*(uint32_t*)entry=0xf75ffe57;*(uintptr_t*)(entry+8)=base+0x672c60;
 *count=n+1;return 1;
}
static uint32_t active_page(uintptr_t base){
 if(!base)return 0;
 uint8_t *menu=(uint8_t*)(base+0x17bd080);uint32_t ui=*(uint32_t*)(menu+8);
 if(ui==7||ui==8)return 0;
 uint32_t depth=*(uint32_t*)(menu+0xdb0);if(depth>=160)return 0;
 uint8_t *page=*(uint8_t**)(menu+0x8b0+(size_t)depth*8);
 return page?*(uint32_t*)(page+0x10):0;
}
static uint8_t *active_page_object(uintptr_t base){
 uint8_t *menu=(uint8_t*)(base+0x17bd080);
 uint32_t depth=*(uint32_t*)(menu+0xdb0);if(depth>=160)return 0;
 return *(uint8_t**)(menu+0x8b0+(size_t)depth*8);
}
/* The lobby page activates (0x623520) while LIST_MAIN_MENU still reports no
 * focusable rows, so native focus falls through to LIST_PLAYERS. Once the
 * page is active and the main list can take focus, move focus there once
 * per visit through the native page focus setter 0x623660. */

/* v4.61 lobby stage. PS3 showed the selected map's "stage" level behind the MP menus (map list
 * 0xdec97a4 entry +0x80, loaded by PS4 0x743420 from NetGameManager 0x72b570, which the LAN path
 * never reaches). Only City of Brass (san-upper-multi-stage + sky-san-upper-multi) ships on PS4, so
 * request those two the way 0x743420 does ({hash, sid, 0, flags 3}), then show them (0x10f25b0(l,1),
 * level byte +2 |= 0x10) once the level manager has them. State word at s+0x3e10. */
static void stage_tick(uintptr_t base,ProfileState *s){
 uint32_t *st=(uint32_t*)((uint8_t*)s+0x3e10);
 if(!*(uint8_t*)(base+0x17d5c60))return;          /* LAN not started */
 void *(*find)(uintptr_t,uint32_t)=(void*(*)(uintptr_t,uint32_t))s->natives[NATIVE_LEVEL_FIND];
 uint64_t (*hash)(const char*)=(uint64_t(*)(const char*))s->natives[NATIVE_NAME_HASH];
 void (*request)(uintptr_t,void*)=(void(*)(uintptr_t,void*))s->natives[NATIVE_LEVEL_REQUEST];
 void (*showl)(void*,int)=(void(*)(void*,int))s->natives[NATIVE_LEVEL_SHOW];
 if(!find||!hash||!request||!showl)return;
 if(!(*st&0x10000u)&&s->natives[NATIVE_MP_SCENE_ENTER]){((void(*)(uintptr_t))s->natives[NATIVE_MP_SCENE_ENTER])(base+0x17bba90);*st|=0x10000u;}
 /* anim-proto-multiplayer: the menu scene waits for it before placing the lobby character */
 if(!(*st&0x20000u)){uint8_t req[0x18];for(int j=0;j<0x18;j++)req[j]=0;
  *(uint64_t*)req=hash("anim-proto-multiplayer");*(uint32_t*)(req+8)=0x67adb399u;
  request(base+0x1afcf10,req);*st|=0x20000u;}
 char *stage=(char*)(base+0x17cb060+0x74b8);       /* current lobby stage name (set by the native loader 0x743420) */
 static const char n0[]="san-upper-multi-stage",n1[]="sky-san-upper-multi";
 static const uint32_t sids[2]={0x703ab0f4u,0x5f482388u};
 const char *names[2]={n0,n1};
 for(int k=0;k<2;k++){
  uint32_t bit=1u<<k,shown=0x100u<<k;
  if(k==0&&stage[0]&&!(*st&bit))continue;          /* native loader already picked a stage */
  void *l=find(base+0x1afcf10,sids[k]);
  if(!l&&!(*st&bit)){uint8_t req[0x18];for(int j=0;j<0x18;j++)req[j]=0;
   *(uint64_t*)req=hash(names[k]);*(uint32_t*)(req+8)=sids[k];req[0xe]=3;
   request(base+0x1afcf10,req);*st|=bit;
   if(k==0&&!stage[0]){for(int j=0;n0[j];j++)stage[j]=n0[j];stage[sizeof(n0)-1]=0;}
   continue;}
  if(l&&!(*st&shown)){showl(l,1);*((uint8_t*)l+2)|=0x10;*st|=shown;}
 }
 /* lobby camera: once the named stage level is loaded (state 8), and again whenever the stage changes */
 if(stage[0]&&s->natives[NATIVE_MP_CAMERA]){
  uint32_t sid=0;{uint32_t c=0;for(const char *p=stage;*p;p++){c^=(uint32_t)(uint8_t)*p<<24;for(int b=0;b<8;b++)c=(c&0x80000000u)?(c<<1)^0x04C11DB7u:(c<<1);}sid=c;}
  uint8_t *l=(uint8_t*)find(base+0x1afcf10,sid);
  if(l&&*(uint32_t*)(l+0x30)==8&&st[1]!=sid){((void(*)(uintptr_t))s->natives[NATIVE_MP_CAMERA])(base+0x17bba90);st[1]=sid;}
 }
}
static void lobby_focus(uintptr_t base,ProfileState *s){
 if(s->focus_busy)return;
 uint8_t *page=active_page_object(base);
 uint32_t ui=*(uint32_t*)(base+0x17bd080+8);
 uint64_t *fl=(uint64_t*)((uint8_t*)s+0x2400); /* lobby focus log: [0] frames on page, [1] changes, [2] last id, [3] guard moves, [4] redirects, [6..21] ring {frame<<32|id} */
 if(!page||*(uint32_t*)(page+0x10)!=0x53cbb5f2){s->focus_done=0;fl[0]=0;return;}
 fl[0]++;
 fl[5]=(fl[5]&0xffffffffull)|((uint64_t)*(uint32_t*)(base+0x17bd080+0x14fc)<<32); /* popup page last opened by 0x6528d0 */
 {uint32_t n=*(uint32_t*)(page+0xac);uint8_t **controls=*(uint8_t***)(page+0xb0);uint32_t fid=0xffffffffu;
  for(uint32_t i=0;controls&&i<n&&i<64;i++){uint8_t *c=controls[i];if(c&&*(uint8_t*)(c+8))fid=*(uint32_t*)(c+0x10);}
  if(fid!=(uint32_t)fl[2]){fl[2]=fid;fl[6+(fl[1]%16)]=(fl[0]<<32)|fid;fl[1]++;}}
 /* Left from the party box (layout column 2) goes to the nearest focusable control in another
  * column (0x623730), and the profile box (column 0, id 0x649e809a, focusable whenever it has a
  * title, 0x5d9010) is nearer than LIST_MAIN_MENU: it took focus and Right went straight back to the
  * party (v4.45/v4.46). Whenever the profile box holds focus, hand it to LIST_MAIN_MENU. */
 if(s->focus_done&&*(uint8_t*)(page+8)&&s->natives[NATIVE_SET_FOCUS]){
  uint32_t n=*(uint32_t*)(page+0xac);uint8_t **controls=*(uint8_t***)(page+0xb0);
  uint8_t *mainc=0,*focused=0;
  for(uint32_t i=0;controls&&i<n&&i<64;i++){uint8_t *c=controls[i];if(!c)continue;
   if(*(uint32_t*)(c+0x10)==0xf75ffe57)mainc=c;if(*(uint8_t*)(c+8))focused=c;}
  /* The party box picks up focus by itself right after the page opens (seen in v4.46/v4.47 logs:
   * LIST_MAIN_MENU focused at first, the party ~1 s later). Keep LIST_MAIN_MENU focused for the first
   * 120 frames of each visit; after that only the player's Right moves into the party. */
  int guard=fl[0]<120&&focused&&*(uint32_t*)(focused+0x10)==0x3406420cu;
  if(mainc&&focused&&focused!=mainc&&(guard||*(uint32_t*)(focused+0x10)!=0x3406420cu)){ /* anything but the menu or the party */
   s->focus_busy=1;((void(*)(void*,void*))s->natives[NATIVE_SET_FOCUS])(page,mainc);s->focus_moves++;s->focus_busy=0;
   if(guard)fl[3]++;else fl[4]++;}
 }
 if(s->focus_done)return;
 s->focus_tries++;
 if(ui==7||ui==8){s->focus_fail=1;return;}
 if(!*(uint8_t*)(page+8)){s->focus_fail=2;return;}
 if(!s->natives[NATIVE_SET_FOCUS]){s->focus_fail=3;return;}
 uint32_t n=*(uint32_t*)(page+0xac);uint8_t **controls=*(uint8_t***)(page+0xb0);
 if(!controls||n>64){s->focus_fail=4;return;}
 uint8_t *main=0,*focused=0;
 for(uint32_t i=0;i<n;i++){uint8_t *c=controls[i];if(!c)continue;
  if(*(uint32_t*)(c+0x10)==0xf75ffe57)main=c;
  if(*(uint8_t*)(c+8))focused=c;}
 if(!main){s->focus_fail=5;return;}
 if(focused==main){s->focus_done=1;s->focus_fail=0;return;}
 s->focus_busy=1;
 int (*can_focus)(void*)=*(int(**)(void*))(*(uint8_t**)main+0x40);
 int ok=can_focus(main)&255;
 if(ok){((void(*)(void*,void*))s->natives[NATIVE_SET_FOCUS])(page,main);s->focus_moves++;s->focus_done=1;s->focus_fail=0;}
 else s->focus_fail=6;
 s->focus_busy=0;
}
/* PS3 f_799f8c: localized text by id, falling back to the table's untranslated string. */
static const char *text(uintptr_t base,ProfileState*s,uint32_t id){
 if(!s->natives[NATIVE_TEXT_FIND])return "";
 void *db=(void*)(base+0x17026b8);
 const char *t=((const char*(*)(void*,uint32_t))s->natives[NATIVE_TEXT_FIND])(db,id);
 if(!t)t=*(const char**)((uint8_t*)db+0x10);
 return t?t:"";
}
/* PAGE_LOBBY buttons. PS3 0x347994 (enum 0x32: Game Type / Map / Settings) keeps the button's own
 * label for the party leader in a custom game; PS3 0x347d34 (enum 0x1e: Start Game) is visible to
 * the leader and labelled 0x92db2b96 in custom-game mode. A LAN host is its own party leader. */
static int lobby_buttons(uintptr_t base,int32_t *event,ProfileState *s){
 uint8_t *source=*(uint8_t**)((uint8_t*)event+0x98);if(!source)return 0;
 uint32_t tag=*(uint32_t*)(source+0x1c);
 if(tag==0x4c414e32){
  if(event[0]==1)*(const char**)((uint8_t*)event+0x40)=text(base,s,*(uint32_t*)(source+8));
  else if(event[0]==8)event[1]=1;
  return 1;
 }
 if(tag==0x4c414e1e){
  if(event[0]==1)*(const char**)((uint8_t*)event+0x40)=text(base,s,*(uint32_t*)(base+0x17d603c)==2?0x92db2b96u:0x63091cd0u);
  else if(event[0]==8)event[1]=1;
  return 1;
 }
 return 0;
}

static Probe *probe_of(ProfileState *s){return (Probe*)((uint8_t*)s+1280);}
/* Slot call history for the runner's net trace: state+1900 count, state+1904 last 32 slots. */
static void be_note(ProfileState *s,int slot){uint32_t *h=(uint32_t*)((uint8_t*)s+1900);uint8_t *r=(uint8_t*)s+1904;r[h[0]%32]=(uint8_t)slot;h[0]++;}
static int64_t be_slot(LanBackend *b,int slot){Probe *p=probe_of(b->state);p->backend_calls++;p->last_slot=(uint32_t)slot;be_note(b->state,slot);return 0;}
#define BE_STUB(n) __attribute__((used,noinline)) static int64_t be_##n(LanBackend *b){return be_slot(b,n);}
BE_STUB(0) BE_STUB(1) BE_STUB(2) BE_STUB(3) BE_STUB(5) BE_STUB(6) BE_STUB(7) BE_STUB(8) BE_STUB(9) BE_STUB(10) BE_STUB(12) BE_STUB(13) BE_STUB(15)
BE_STUB(16) BE_STUB(17) BE_STUB(18) BE_STUB(19) BE_STUB(20) BE_STUB(21) BE_STUB(22) BE_STUB(23) BE_STUB(24) BE_STUB(25) BE_STUB(26) BE_STUB(27) BE_STUB(28) BE_STUB(29) BE_STUB(30) BE_STUB(31)
/* Slot 11 (PS3 +0x2c), called with (session, 1) when net state 0x11 starts the match: lock the room.
 * PS3 state 0x11 (0x2b4444) then creates a net player for every session member (f_304948 ==
 * PS4 0x72eca0, +0x21c = -1, ready +0x365 cleared); PS4 kept only the debug bot-fill branch, so the
 * member loop is restored here, right after the lock and before the players event is built. */
__attribute__((used,noinline)) static int64_t be_11(LanBackend *b,uint8_t *session,int32_t lock){
 be_slot(b,11);
 uintptr_t base=b->base;ProfileState *s=b->state;
 uint8_t *(*create)(uintptr_t,uint8_t*,uint8_t*,uint32_t,uint32_t)=(uint8_t*(*)(uintptr_t,uint8_t*,uint8_t*,uint32_t,uint32_t))s->natives[NATIVE_CREATE_PLAYER];
 if(!lock||!session||!create||*(uintptr_t*)(base+0x17cb060+0x380))return 0;
 for(int i=0;i<10;i++){
  uint8_t *m=session+0x680+i*0x120;
  if(!m[0x25])continue;
  uint8_t *pl=create(base+0x17cb060,session,m,0,0);
  if(pl){*(int32_t*)(pl+0x21c)=-1;pl[0x365]=0;}
 }
 return 0;
}
/* Slot 14 (PS3 +0x38), called by EstablishConnection 0x67fad0 once every member is connected, with the
 * member id list: the host announces the match settings to the room. Every machine (host included)
 * reacts by raising the session's start-load flag (+0x1338, normally from the settings event
 * 0x71c780), which moves EstablishConnection to LoadLevels. Solo host: raise it locally. */
__attribute__((used,noinline)) static int64_t be_14(LanBackend *b,uint8_t *session){
 be_slot(b,14);
 if(session&&*(uint32_t*)(session+0x12e4)==*(uint32_t*)(session+0x12e8))session[0x1338]=1;
 return 0;
}
/* Slot 4 (PS3 +0x10): Create/Host(session, max players, ...). */
__attribute__((used,noinline)) static int64_t be_create(LanBackend *b,uint8_t *session,uint32_t max_players){
 ProfileState *s=b->state;Probe *p=probe_of(s);p->create_calls++;p->last_slot=4;be_note(s,4);
 *(LanBackend**)(session+8)=b;                     /* session backend, as PS3 AttachSession */
 uint8_t *m=session+0x680;clear(m,0x120);
 const char *name=(s->profiles.selected&&s->profiles.selected<=s->profiles.count)?s->profiles.names[s->profiles.selected-1]:"Player";
 for(int i=0;i<31&&name[i];i++)m[i]=(uint8_t)name[i];
 m[0x25]=1;*(uint32_t*)(m+0x2c)=1;*(uint32_t*)(m+0x34)=0;
 *(uint32_t*)(session+0x18)=1;                     /* member count */
 *(uint32_t*)(session+0x12e4)=1;*(uint32_t*)(session+0x12e8)=1; /* self id, host id */
 *(int32_t*)(session+0xa4)=0;session[0xa1]=1;      /* create completed, result ok */
 b->created=1;(void)max_players;
 return 0;
}
static LanBackend *lan_backend(uintptr_t base,ProfileState *s){
 LanBackend *b=(LanBackend*)((uint8_t*)s+1536);void **vt=(void**)((uint8_t*)s+1600);
 #define SET(i,f) do{void *fp;__asm__ volatile("leaq " #f "(%%rip),%0":"=r"(fp));vt[i]=fp;}while(0)
 SET(0,be_0);SET(1,be_1);SET(2,be_2);SET(3,be_3);SET(4,be_create);SET(5,be_5);SET(6,be_6);SET(7,be_7);
 SET(8,be_8);SET(9,be_9);SET(10,be_10);SET(11,be_11);SET(12,be_12);SET(13,be_13);SET(14,be_14);SET(15,be_15);
 SET(16,be_16);SET(17,be_17);SET(18,be_18);SET(19,be_19);SET(20,be_20);SET(21,be_21);SET(22,be_22);SET(23,be_23);
 SET(24,be_24);SET(25,be_25);SET(26,be_26);SET(27,be_27);SET(28,be_28);SET(29,be_29);SET(30,be_30);SET(31,be_31);
 #undef SET
 b->vtable=vt;b->base=base;b->state=s;
 return b;
}
/* BUTTON_MULTIPLAYER_STARTMATCH (0x70e80076; PS4 0x66edf0 stub), PS3 0x292e3c custom-game branch.
 * PS4 has no countdown page 0x73a73dd8, so its completion 0x66e9b0 runs directly. Party-size and
 * team-balance checks need the party backend and are skipped while hosting solo. */
static void start_game(uintptr_t base,ProfileState *s){
 Probe *p=probe_of(s);p->start_calls++;
 void (*done)(void)=(void(*)(void))s->natives[NATIVE_COUNTDOWN_DONE];
 if(!done||*(uint32_t*)(base+0x17d603c)!=2||!*(uint8_t*)(base+0x17d5c60))return;
 if(*(uint32_t*)(base+0x1b68810)==3||*(uint32_t*)(base+0x1b68810)==0xb)return; /* already starting */
 {uint8_t *nc=(uint8_t*)s+0x2000;if(*(uint8_t**)(base+0x17c3580)==nc){*(uint64_t*)(nc+0xc)=0;*(uint32_t*)(nc+0x20)=0;*(uint32_t*)(nc+0x28)=0;}}
 /* Abort flag 0x16e7170 starts as 1 in data and is cleared by the state enter 0x666c20 that a
  * normal match start passes through (and by Leave's exit 0x6775f0); EstablishConnection 0x67fad0
  * leaves at once while it is set. Clear it and the net error flag 0x17d5c80 for a fresh start. */
 *(uint8_t*)(base+0x16e7170)=0;*(uint8_t*)(base+0x17d5c80)=0;
 /* Session Init/Reset 0x7a2360 has no PS4 caller: without it every async-operation "done" flag of
  * g_matchSession is 0 and Playing (0x67d760) ends the match on +0xc8. Reset before Create. */
 {int64_t (*reset)(uintptr_t)=(int64_t(*)(uintptr_t))s->natives[NATIVE_SESSION_RESET];if(reset)reset(base+0x17d6420);}
 /* SHORTCUT (missing PS4 asset): City of Brass's kill-volume level san-upper-multi-killvis is listed in
  * pak23_ps4.txt but shipped as an empty 244-byte stub with no -ingame pack, so it never leaves level
  * state 2 and the load queue stalls. PS3 loads it from map entry +0x38 when +0x40 is set (0x72b570).
  * Clear +0x40 of map-list (symbol 0xdec97a4) entries naming that level until the PS3 data is ported. */
 {void *(*lookup)(uint32_t)=(void*(*)(uint32_t))s->natives[NATIVE_LOOKUP_SYMBOL];
  uint8_t *ml=lookup?(uint8_t*)lookup(0x0dec97a4):0;
  if(ml){uint32_t n=*(uint32_t*)ml;uint8_t *e=*(uint8_t**)(ml+8);
   static const char kv[]="san-upper-multi-killvis";
   for(uint32_t i=0;e&&i<n&&i<64;i++,e+=0x98){const char *nm=*(const char**)(e+0x38);if(!nm)continue;
    int eq=1;for(int k=0;k<(int)sizeof(kv);k++)if(nm[k]!=kv[k]){eq=0;break;}
    if(eq){*(uint32_t*)(e+0x40)=0;probe_of(s)->pad2++;}}}}
 LanBackend *b=lan_backend(base,s);
 *(LanBackend**)(base+0x17dd768)=b;                /* session manager (PS3 0x1249294) */
 *(LanBackend**)(base+0x17d6428)=b;
 done();
}
static void probe_text(char *out,int32_t *ev){const char *t=*(const char**)((uint8_t*)ev+0x40);clear(out,32);if(t)for(int i=0;i<31&&t[i];i++)out[i]=t[i];}
static void probe_options(uintptr_t base,ProfileState *s){
 Probe *p=(Probe*)((uint8_t*)s+1280);
 p->time_raw=*(uint32_t*)(base+0x17d1a9c);p->score_raw=*(uint32_t*)(base+0x17d1a98);p->net_state=*(uint32_t*)(base+0x1b68810);p->session_ready=*(uint8_t*)(base+0x17d64c1);
 if(p->runs&&++p->runs%60)return;
 p->magic=0x424f5250;if(!p->runs)p->runs=1;
 p->nmh_count=*(uint8_t*)(base+0x17d7b30);p->selected_netgame=*(uint32_t*)(base+0x17d167c);p->mode=*(uint32_t*)(base+0x17d603c);
 p->lan=*(uint8_t*)(base+0x17d5c60);p->flag65=*(uint8_t*)(base+0x17d6065);p->party=*(uint32_t*)(base+0x17dc2b0);
 void **g=((void**(*)(uint32_t))s->lookup)(0x3b9a067d);p->game_array=g&&*g;p->game_array_ptr=g?(uint64_t)(uintptr_t)*g:0;p->game_count=g&&*g?*(uint32_t*)*g:0xffffffffu;
 void **n=((void**(*)(uint32_t))s->lookup)(0xf985c7b6);p->name_array=n&&*n;p->name_count=n&&*n?*(uint32_t*)*n:0xffffffffu;
 void (*gt)(int32_t*)=(void(*)(int32_t*))s->natives[NATIVE_DS_GAMETYPE];
 void (*mp)(int32_t*)=(void(*)(int32_t*))s->natives[NATIVE_DS_MAP];
 if(!gt||!mp||p->mode!=2)return;
 int32_t ev[64];
 clear(ev,sizeof(ev));ev[0]=6;ev[1]=-7;gt(ev);p->gt_max=(uint32_t)ev[1];
 clear(ev,sizeof(ev));ev[0]=5;ev[1]=-7;gt(ev);p->gt_sel=(uint32_t)ev[1];
 clear(ev,sizeof(ev));ev[0]=2;ev[1]=0;gt(ev);probe_text(p->gt0,ev);
 clear(ev,sizeof(ev));ev[0]=2;ev[1]=1;if(p->nmh_count>1){gt(ev);probe_text(p->gt1,ev);}
 clear(ev,sizeof(ev));ev[0]=6;ev[1]=-7;mp(ev);p->map_max=(uint32_t)ev[1];
 clear(ev,sizeof(ev));ev[0]=5;ev[1]=-7;mp(ev);p->map_sel=(uint32_t)ev[1];
 clear(ev,sizeof(ev));ev[0]=2;ev[1]=0;mp(ev);probe_text(p->map0,ev);
}
/* LIST_MAIN_MENU (PS4 source 0xb3 = PS3 0xb4, f_334ee8; PS4 0x76c4b0 is a RET stub). Rows are
 * {text id, icon id, action id}. LAN lobby uses PS3 table 0x11bf908 (6 rows; the 4-row table
 * 0x11bf8d8 applies when PS3 net-game+0x3fd is set - PS4 offset not identified yet). */
typedef struct { uint32_t text,icon,action; } LobbyRow;
static const LobbyRow lan_rows[6]={
 {0x66d63005u,0x7e2c2757u,0x7d593924u}, /* Custom Game */
 {0xebd77105u,0x23e9be55u,0x52f3d9f6u}, /* Find a Party */
 {0xa2aa79eau,0xb675afdbu,0xc4acc827u}, /* Profile */
 {0xcb379354u,0x12d18838u,0xb85cb12au}, /* Cinema */
 {0xfb08c743u,0x2c68ff83u,0xe7d7dbbbu}, /* Options */
 {0xd7d4ccadu,0x88c52b24u,0xcd127c29u}, /* Splitscreen ("Leave Splitscreen" 0x2b93528e while active) */
};
/* PS3 f_97202c(out,id,1) inlined on PS4 (see 0x7521a0 op 4): texture index of an icon id in the
 * UI texture table *(0x1b24bc8)+0x28 {u16 count at +2, 40-byte entries at +8, id at +8}. */
static int32_t icon_index(uintptr_t base,uint32_t id){
 uint8_t *ui=*(uint8_t**)(base+0x1b24bc8);if(!ui)return -1;
 uint8_t *t=*(uint8_t**)(ui+0x28);if(!t)return -1;
 uint32_t n=*(uint16_t*)(t+2);uint8_t *e=*(uint8_t**)(t+8);
 for(uint32_t i=0;e&&i<n;i++)if(*(uint32_t*)(e+i*40+8)==id)return (int32_t)i;
 return -1;
}
static void set_icon(uintptr_t base,int32_t *event,uint32_t id){
 event[2]=3;event[4]=0;event[5]=0;event[6]=0;event[7]=(int32_t)id;event[8]=1;
 event[9]=icon_index(base,id);event[10]=0;event[11]=-1;event[12]=0x3f800000;
}
static void emblem_probe(uintptr_t base,ProfileState *s);
static void set_text(int32_t *event,const char *t);
static void mission_take(uintptr_t base,ProfileState *s);
static void clan_tag_poll(uintptr_t base,ProfileState *s);
static uint8_t *player_profile(uintptr_t base);
static char *fmt_append(char *o,char *end,const char *t);
/* HUD material names: the PS4 hud.pak names its materials "dir/file:variant", the PS3 one
 * "dir/file/variant". The multiplayer code (inherited from PS3) searches the hud material table
 * (UI+0x20, entries 0x28 bytes, sid at +8; filled when the hud level logs in) for the PS3 sids, e.g.
 * 0x62ea60 looks for store/purchased 0xc1df5e2e, so those icons came out blank. Rewriting hud.pak
 * broke boot (v4.47/v4.50 NEXT), so rename the entries in memory instead, once multiplayer starts. */
static const uint32_t hud_alias[16][2]={
 {0x3cf077bf,0xa0cf6221},{0x4edc816b,0xd2e394f5},{0xe3b6f7f9,0x7f89e267},{0x7a8f9351,0xea36b17f},
 {0x658daf73,0xc1df5e2e},{0x0d289975,0xcce16bda},{0xab6b6a43,0x37547fdd},{0xa07ffbfe,0x61b60951},
 {0x9f207901,0x6e9d9193},{0x582a3a34,0x772e04ce},{0x48204c3f,0xd41f59a1},{0xafb865da,0x1f621808},
 {0xa6d33ad7,0x576ed245},{0xe7ca9bf7,0xd75be949},{0x7ddf5534,0xe1e040aa},{0x13b16465,0x56f1b7f5}};
static void hud_alias_names(uintptr_t base,ProfileState *s){
 uint32_t *done=(uint32_t*)((uint8_t*)s+0x3e00);
 uint8_t *ui=*(uint8_t**)(base+0x1b24bc8);uint8_t *t=ui?*(uint8_t**)(ui+0x20):0;
 if(!t){*done=0;return;}
 if(*done==(uint32_t)(uintptr_t)t)return;   /* table pointer changes if the hud level reloads */
 uint32_t n=*(uint16_t*)(t+2);uint8_t *e=*(uint8_t**)(t+8);if(!e)return;
 for(uint32_t i=0;i<n&&i<1024;i++){uint32_t *sp=(uint32_t*)(e+(size_t)i*40+8);
  for(int k=0;k<16;k++)if(*sp==hud_alias[k][0]){*sp=hud_alias[k][1];break;}}
 *done=(uint32_t)(uintptr_t)t;}
static int lobby_source(uintptr_t base,int32_t *event,ProfileState *s){
 uint8_t *source=*(uint8_t**)((uint8_t*)event+0x98);if(!source)return 0;
 uint32_t tag=*(uint32_t*)(source+0x1c);
 if(tag!=0x4c414eb3&&tag!=0x4c414ead)return 0;
 uint32_t index=(uint32_t)event[1];
 if(tag==0x4c414eb3){
  const uint32_t rows=6;
  switch(event[0]){
   case 1:*(const char**)((uint8_t*)event+0x40)="";break;                 /* PS3: empty title */
   case 2:if(event[0x15]==0&&index<rows){uint32_t id=lan_rows[index].text;
     *(const char**)((uint8_t*)event+0x40)=text(base,s,id);}break;
   case 4:{
     /* DIAGNOSTIC: record icon requests at state+0x2300 {count, last slot id, last row, last index, table count} */
     uint32_t *dg=(uint32_t*)((uint8_t*)s+0x2300);dg[0]++;dg[1]=(uint32_t)event[0x14];dg[2]=index;
     uint8_t *ui=*(uint8_t**)(base+0x1b24bc8);uint8_t *t=ui?*(uint8_t**)(ui+0x28):0;dg[4]=t?*(uint16_t*)(t+2):0xffffffffu;
     if(index<rows)dg[3]=(uint32_t)icon_index(base,lan_rows[index].icon);
     dg[5+(dg[0]&7)]=(uint32_t)event[0x14];
     if((uint32_t)event[0x14]==0xf5a294f3u&&index<rows)set_icon(base,event,lan_rows[index].icon);break;}
   case 5:event[1]=(int32_t)s->lobby_selected;break;
   case 6:event[1]=(int32_t)rows-1;break;                                 /* PS3: last row index */
   case 7:event[1]=0;break;
   case 10:if(index<rows)s->lobby_selected=index;break;
   /* Native list update 0x5de2d0 queries vtable+0x90 (operation 11); result is event+0x38. */
   case 11:event[14]=index<rows;break;
   case 23:if(s->lobby_selected>=rows)s->lobby_selected=0;break;
  }
  return 1;
 }
 /* LIST_PLAYERS (PS4 0xad = PS3 0xae, f_34634c): interim single-member view (the local LAN
  * profile). As on PS3 the row shows the rank badge (icon slot 0xf5a294f3 -> type 0x29 with the
  * rank, PS4 layout as in the profile box 0x761ea0) and "[ tag ] name". */
 /* Party model read by the member click handler 0x671d00 (PS3 f_34634c rows 0x12332a8..): 8-byte
  * rows {u32 player-table index, u8 local}. A local row makes it take the identity of the local
  * player (0x7a3400(0x17dc298)) before it opens the member popup (lock/unlock party, view stats). */
 *(uint32_t*)(base+0x1b68a80)=0;*(uint8_t*)(base+0x1b68a84)=1;
 const char *name="";
 if(event[0]==1)name="PARTY";
 else if(s->profiles.selected&&s->profiles.selected<=s->profiles.count){
  name=s->profiles.names[s->profiles.selected-1];
  uint8_t *pr=player_profile(base);
  if(pr&&pr[0xac0]){char *o=(char*)s+0x2b80,*end=o+100;o=fmt_append(o,end,"[ ");
   for(int k=0;k<4&&pr[0xac0+k]&&o<end;k++)*o++=(char)pr[0xac0+k];
   o=fmt_append(o,end," ] ");o=fmt_append(o,end,name);*o=0;name=(const char*)s+0x2b80;}
 }
 if(event[0]==4&&(uint32_t)event[0x14]==0xf5a294f3u&&index==0){
  uint32_t (*rank)(void*)=(uint32_t(*)(void*))s->natives[NATIVE_PROFILE_RANK];
  uint8_t *pr=player_profile(base);
  if(rank&&pr){event[2]=0x29;*(uint64_t*)(event+4)=0;event[6]=(int32_t)rank(pr);event[7]=0;event[8]=0;event[9]=-1;event[10]=0;event[11]=-1;event[12]=0x3f800000;}
  return 1;
 }
 switch(event[0]){
  case 1:*(const char**)((uint8_t*)event+0x40)=name;break;
  case 2:*(const char**)((uint8_t*)event+0x40)=event[0x15]==0?name:"";break;
  case 5:event[1]=0;break;
  case 6:event[1]=0;probe_options(base,s);clan_tag_poll(base,s);hud_alias_names(base,s);stage_tick(base,s);break;  /* emblem_probe retired: state+0x2400 now holds the lobby focus log */
  case 7:event[1]=0;break;
  /* PS3 answers 1. Focusable on the lobby page only (lobby_focus returns focus to LIST_MAIN_MENU
   * once per visit; with the profile box unfocusable, Left from the party reaches the main list). */
  /* Party box kept unfocusable for now: when focusable, focus returns to it right after LIST_MAIN_MENU
   * gets it (v4.48 focus log: main -> party within ~1 frame after page activation and after popups),
   * and Left from it does not move focus. Revisit with a hook on the page navigation (0x625761). */
  case 21:event[1]=0;break;
  case 11:event[14]=(index==0);break;
 }
 return 1;
}
static int show(uintptr_t base,ProfileState*s,uint32_t page,int silent){
 return ((int(*)(void*,uint32_t,int,int))s->show_page)((void*)(base+0x17bd080),page,0,silent)&255;
}
/* PS3 f_26a364 Menu::ShowDialog. Its struct (PS3 *(menu+0xcac)) is *(menu+0x1438) on PS4 with every
 * field 4 bytes further (PS4 inlines it, e.g. 0x64c685), then the dialog page 0x719e9fec. */
static void show_dialog(uintptr_t base,ProfileState*s,uint32_t body,uint32_t title,uint32_t action,int32_t buttons,uint32_t a6,uint32_t a7){
 uint8_t *d=*(uint8_t**)(base+0x17bd080+0x1438);if(!d||!s->show_page)return;
 if(!a7)a7=buttons==0?0x576ed245u:0xd75be949u;
 *(uint32_t*)(d+0x08)=body;*(uint32_t*)(d+0x14)=title;*(uint32_t*)(d+0x18)=0x11d8e78au;
 *(uint32_t*)(d+0x20)=0x1519fa3du;*(uint32_t*)(d+0x28)=action;*(uint32_t*)(d+0x24)=a7;
 *(int32_t*)(d+0x1c)=buttons;*(uint32_t*)(d+0x10)=buttons!=0;*(uint32_t*)(d+0x2c)=a6;
 show(base,s,0x719e9fec,0);
}
/* LIST_CINEMA, PS3 f_32f1a8. Its rows are the saved films scanned by the film manager (PS3
 * f_2b1cdc, "Refresh Cinema files"); PS4 has no film manager or film recording, so this is the
 * PS3 source with a film count of 0: title text, selection 0, last row -1 (no rows). */
static int cinema_source(uintptr_t base,int32_t *event,ProfileState *s){
 uint8_t *source=*(uint8_t**)((uint8_t*)event+0x98);if(!source||*(uint32_t*)(source+0x1c)!=0x4c414e39)return 0;
 switch(event[0]){
  case 1:*(const char**)((uint8_t*)event+0x40)=text(base,s,0xe626e948u);break; /* PS3 op1: flag 0x11bf4b4 (initially 1, local films) set -> 0xe626e948; clear (online files) -> 0xdc27f8a0 */
  case 5:event[1]=0;break;
  case 6:event[1]=-1;break;
  case 7:event[1]=0;break;
  case 11:event[14]=0;break;
  case 21:event[1]=0;break;
 }
 return 1;
}
/* TAB_NET_PROFILE runs its own PS4 source 0x76bbf0. On PAGE_LOADOUTS it answers image slot
 * 0x35587809 (default-loadout mark) with texture 0xc1df5e2e looked up in the UI texture table
 * *(0x1b24bc8)+0x20. That table belongs to the PS3 multi-menu level, which PS4 lacks, so the index
 * is -1; drawing that image goes through the list's image path (0x609ff0 -> text Draw 0xf1b7d0),
 * which asserts on a null window context and crashes. A texture that is not loaded cannot be
 * drawn, so drop image results with no texture until the multi-menu assets are ported. */
static int tab_source(uintptr_t base,int32_t *event,ProfileState *s){
 uint8_t *source=*(uint8_t**)((uint8_t*)event+0x98);if(!source||*(uint32_t*)(source+0x1c)!=0x4c414ec3)return 0;
 void (*native)(int32_t*)=(void(*)(int32_t*))s->natives[NATIVE_DS_TAB_PROFILE];
 if(!native)return 1;
 native(event);
 if(event[0]==4&&event[2]==3&&event[9]<0){event[2]=0;uint32_t *dg=(uint32_t*)((uint8_t*)s+0x2300);dg[12]++;}
 (void)base;return 1;
}
/* Profile box (enum 0x89, PS4 source 0x761ea0) took focus. v4.45 answered op 21 = 0, which its
 * control class ignores (see below), so Left from the party still went to it. Page navigation
 * (0x623730) moves Left/Right to the nearest focusable control (by y) in another layout column:
 * on PAGE_MULTIPLAYER the party box is column 2, LIST_MAIN_MENU and the profile box column 0, so
 * Left from the party landed on the profile box and Right went straight back (stuck in the party).
 * Run the native source and answer op 21 with 0, as a display-only box. */
static int profile_box_source(uintptr_t base,int32_t *event,ProfileState *s){
 uint8_t *source=*(uint8_t**)((uint8_t*)event+0x98);if(!source||*(uint32_t*)(source+0x1c)!=0x4c414e89)return 0;
 void (*native)(int32_t*)=(void(*)(int32_t*))s->natives[NATIVE_DS_PROFILE_BOX];
 if(event[0]==21){event[1]=0;return 1;}
 /* Its control class (vtable 0x16796a0) decides focus with 0x5d9010: focusable when the source's
  * title (operation 1, adapter +0x30) is a non-empty string. The native source has no title; answer
  * an empty one so the box can't take focus. */
 if(event[0]==1){set_text(event,"");return 1;}
 if(native)native(event);
 (void)base;return 1;
}
/* Emblem part tables: ids at 0x1b67e6c + k*0x18 (filled by 0x66b510), resolved by 0x66bc30
 * through FindModule(id,1) (0x11fd4c0), which also requires the owning module's condition
 * lambda (module+0x50 -> +0x20) to pass. The tables live in dc1/ps4/net.bin. 0x66bc30 does not
 * check the result, and on PAGE_EMBLEM it came back empty and crashed (null +0x48).
 * DIAGNOSTIC at state+0x2400 (u64): [0]=probes, then per table k (6 words from [1+k*6]):
 * id, find1, find0, module+0x48(data), module+0x50(info), info+0x20(lambda); [19]=lambda result,
 * [20]=flag *(0x1b3aec0)+0x3a04. Pointers are stored relative to base when inside the image. */
static int emblem_ready(uintptr_t base,ProfileState *s){
 void *(*find)(uint32_t,int)=(void*(*)(uint32_t,int))s->natives[NATIVE_FIND_MODULE];
 if(!find)return 0;
 /* PS3 fills the emblem part tables from the menu: the emblem object's update (f_278ca4, vtable
  * 0x11785a0) calls the table init f_27887c while its dirty byte is set. PS4 kept the init
  * (0x66b510) but calls it only from match setup (0x72c8ae), so in the menu the ids stay 0.
  * Run it the PS3 way, but only after NetInit: with 0x17d5c61 clear its other branch writes
  * through a null base (v4.32 crashed at boot at 0x66b63a). */
 void (*init)(uintptr_t)=(void(*)(uintptr_t))s->natives[NATIVE_EMBLEM_INIT];
 if(init&&*(uint8_t*)(base+0x17d5c61)&&!*(uint32_t*)(base+0x1b67e6c)){*(uint8_t*)(base+0x17c3440)=1;init(base+0x17c3440);}
 for(int k=0;k<3;k++){uint32_t id=*(uint32_t*)(base+0x1b67e6c+k*0x18);
  if(!id||find(id,1)==(void*)(base+0x1b3af40))return 0;}
 return 1;
}
/* True when a DC module holding the symbol is loaded. LookupSymbolValue (0x11f2e30) and the native
 * list builders assert on a missing symbol; the net symbols only exist once multi-menu is loaded,
 * and datasources receive events while the menus are built at boot (v4.40 crashed in 0x7596a0). */
static int sym_ready(uintptr_t base,ProfileState *s,uint32_t id){
 void *(*find)(uint32_t,int)=(void*(*)(uint32_t,int))s->natives[NATIVE_FIND_MODULE];
 if(!find)return 0;void *m=find(id,1);return m&&m!=(void*)(base+0x1b3af40);}
static void emblem_probe(uintptr_t base,ProfileState *s){
 uint64_t *dg=(uint64_t*)((uint8_t*)s+0x2400);
 if(!*(uint8_t*)(base+0x17d5c61))return;   /* only after NetInit */
 if(dg[0]++%30)return;
 void *(*find)(uint32_t,int)=(void*(*)(uint32_t,int))s->natives[NATIVE_FIND_MODULE];
 if(!find)return;
 emblem_ready(base,s);
 uintptr_t lambda=0;
 for(int k=0;k<3;k++){uint64_t *e=dg+1+k*6;uint32_t id=*(uint32_t*)(base+0x1b67e6c+k*0x18);
  e[0]=id;e[1]=e[2]=e[3]=e[4]=e[5]=0;if(!id)continue;
  uintptr_t f1=(uintptr_t)find(id,1),f0=(uintptr_t)find(id,0);
  e[1]=f1-base;e[2]=f0-base;
  if(f0!=base+0x1b3af40){uint8_t *m=(uint8_t*)f0;e[3]=*(uint64_t*)(m+0x48);uintptr_t info=*(uintptr_t*)(m+0x50);e[4]=info;
   if(info){e[5]=*(uint64_t*)(info+0x20);if(!lambda)lambda=(uintptr_t)e[5];}}
 }
 int (*eval)(uintptr_t,int,int)=(int(*)(uintptr_t,int,int))s->natives[NATIVE_EVAL_LAMBDA];
 dg[19]=(lambda&&eval)?(uint64_t)(uint32_t)eval(lambda,0,0):0xeeeeu;
 uint8_t *sm=*(uint8_t**)(base+0x1b3aec0);dg[20]=sm?sm[0x3a04]:0xffu;
}
/* NET_EMBLEM_LAYER / NET_EMBLEM_TYPE run their PS4 sources only while the emblem tables resolve. */
static int emblem_source(uintptr_t base,int32_t *event,ProfileState *s){
 uint8_t *source=*(uint8_t**)((uint8_t*)event+0x98);if(!source)return 0;
 uint32_t tag=*(uint32_t*)(source+0x1c);
 if(tag!=0x4c414eb5&&tag!=0x4c414ebe)return 0;
 void (*native)(int32_t*)=(void(*)(int32_t*))s->natives[tag==0x4c414eb5?NATIVE_DS_EMBLEM_LAYER:NATIVE_DS_EMBLEM_TYPE];
 if(native&&emblem_ready(base,s)){native(event);return 1;}
 uint64_t *dg=(uint64_t*)((uint8_t*)s+0x2400);dg[21]++;
 /* Empty list, answered like LIST_CINEMA with no films (v4.33 answered only op 6 and the list
  * kept re-querying, freezing the menu). */
 switch(event[0]){
  case 5:case 7:case 21:event[1]=0;break;
  case 6:event[1]=-1;break;
  case 11:event[14]=0;break;
  case 1:case 2:*(const char**)((uint8_t*)event+0x40)="";break;
 }
 return 1;
}
/* ---- shared helpers for ported PS3 list sources ---- */
/* Minimal sprintf for PS3 f_24a90c-style formats: %d %i %u %s %%. */
static char *fmt_append(char *o,char *end,const char *t){while(*t&&o<end)*o++=*t++;return o;}
static char *fmt_int(char *o,char *end,int64_t v,int commas){
 char tmp[24];int n=0;uint64_t u=v<0?(uint64_t)(-v):(uint64_t)v;
 do{if(commas&&n&&(n%4)==3)tmp[n++]=',';tmp[n++]=(char)('0'+u%10);u/=10;}while(u&&n<22);
 if(v<0&&o<end)*o++='-';
 while(n&&o<end)*o++=tmp[--n];return o;}
static const char *fmt(ProfileState *s,const char *f,int64_t a0,const char *s0,const char *s1,const char *s2){
 char *o=(char*)s+0x2a00,*end=o+255;const char *ss[3]={s0,s1,s2};int si=0;
 for(;*f&&o<end;f++){
  if(*f!='%'){*o++=*f;continue;}
  f++;while(*f>='0'&&*f<='9')f++;
  if(*f=='d'||*f=='i'||*f=='u')o=fmt_int(o,end,a0,0);
  else if(*f=='s')o=fmt_append(o,end,(si<3&&ss[si])?ss[si++]:"");
  else if(*f=='%')*o++='%';
  else if(!*f)break;
 }
 *o=0;return (const char*)s+0x2a00;}
static void set_text(int32_t *event,const char *t){*(const char**)((uint8_t*)event+0x40)=t;}
static uint32_t player_user(uintptr_t base){return *(uint8_t*)(base+0x17bde75)?1u:*(uint32_t*)(base+0x17bf714);}
/* PS3 f_341cbc: id of the current tab of the active page, from the tab table 0xf1514340
 * (PS4 entries 0x20 bytes: page, LAN flag, ..., tabs at +0x10 (12-byte entries), current at +0x18). */
static uint32_t current_tab(uintptr_t base,ProfileState *s){
 uint32_t page=active_page(base);if(!page||!s->lookup)return 0;
 void **sym=((void**(*)(uint32_t))s->lookup)(0xf1514340);if(!sym||!*sym)return 0;
 uint8_t *t=(uint8_t*)*sym;uint32_t n=*(uint32_t*)t;uint8_t *e=*(uint8_t**)(t+8);
 for(uint32_t i=0;e&&i<n;i++,e+=0x20){
  if(*(uint32_t*)e==page&&*(uint32_t*)(e+4)==*(uint8_t*)(base+0x17d5c60)){
   uint8_t *tabs=*(uint8_t**)(e+0x10);int32_t cur=*(int32_t*)(e+0x18);
   return (tabs&&cur>=0)?*(uint32_t*)(tabs+(size_t)cur*0xc):0;}
 }
 return 0;}

/* LIST_BOOSTERS on PAGE_MULTIPLAYER_MEDALS: PS4 0xd3 is a stub (0x759b70); PS3 f_347618.
 * Medal records: symbol 0x980d6543 {count, records}; PS3 records are 0x40 bytes, PS4 0x48 (the
 * PS3 +0x18 field is a pointer, so PS3 fields after it move by 4: PS3 +0x1c value -> PS4 +0x20).
 * The visible list (PS3 0x1233000: u16 ids, count at +0x200, selection 0x1232a8c) is kept at
 * state+0x2800 and rebuilt for the tab's mask (PS3 f_32d1cc: (mask & flags) == mask). */
typedef struct { uint16_t ids[256]; uint16_t count,pad; uint32_t selected,tab,built; } MedalList;
static MedalList *medal_list(ProfileState *s){return (MedalList*)((uint8_t*)s+0x2800);}
static uint8_t *medal_record(ProfileState *s,uint32_t i){
 void *(*lookup)(uint32_t)=(void*(*)(uint32_t))s->natives[NATIVE_LOOKUP_SYMBOL];
 uint8_t *t=lookup?(uint8_t*)lookup(0x980d6543):0;if(!t||i>=*(uint32_t*)t)return 0;
 return *(uint8_t**)(t+8)+(size_t)i*0x48;}
static void medal_build(uintptr_t base,ProfileState *s){
 MedalList *l=medal_list(s);uint32_t tab=current_tab(base,s),mask=0;
 switch(tab){case 0x091026d1u:mask=2;break;case 0xfb0dd522u:mask=4;break;case 0xa13b68fbu:mask=8;break;case 0xadbd5585u:mask=0x10;break;}
 l->tab=tab;l->built=1;l->count=0;
 void *(*lookup)(uint32_t)=(void*(*)(uint32_t))s->natives[NATIVE_LOOKUP_SYMBOL];
 uint8_t *t=lookup?(uint8_t*)lookup(0x980d6543):0;if(!t||!mask)return;
 uint32_t n=*(uint32_t*)t;
 for(uint32_t i=0;i<n&&l->count<256;i++){uint8_t *r=medal_record(s,i);if(r&&(mask&*(uint32_t*)(r+0x10))==mask)l->ids[l->count++]=(uint16_t)i;}
 if(l->selected>=l->count)l->selected=0;}
static int medals_source(uintptr_t base,int32_t *event,ProfileState *s){
 uint8_t *source=*(uint8_t**)((uint8_t*)event+0x98);if(!source||*(uint32_t*)(source+0x1c)!=0x4c414ed3)return 0;
 MedalList *l=medal_list(s);
 if(!l->built||l->tab!=current_tab(base,s))medal_build(base,s);
 uint32_t row=(uint32_t)event[1];
 switch(event[0]){
  case 10:l->selected=row; /* PS3 falls through into the text case */
  /* fallthrough */
  case 2:{
   if(row>=l->count)break;uint8_t *r=medal_record(s,l->ids[row]);if(!r)break;
   uint32_t sub=(uint32_t)event[0x15];
   if(sub==0)set_text(event,text(base,s,*(uint32_t*)r));
   else if(sub==0x80d4cc7bu)set_text(event,text(base,s,*(uint32_t*)(r+4)));
   else if(sub==0xadacdbf3u){
    uint32_t (*earned)(uintptr_t,void*,uint32_t)=(uint32_t(*)(uintptr_t,void*,uint32_t))s->natives[NATIVE_MEDAL_EARNED];
    int64_t c=earned?earned(base+0x17bb878,r,player_user(base)):0;
    set_text(event,fmt(s,text(base,s,0xe9a0ddd7u),c,0,0,0));}
   else if(sub==0xb684d68bu){
    char num[24];char *e=fmt_int(num,num+23,*(int32_t*)(r+0x20),1);*e=0;
    char *o=(char*)s+0x2b00,*end=o+120;o=fmt_append(o,end,text(base,s,0xb4b0d5c2u));o=fmt_append(o,end," |@Cff7cce96|");o=fmt_append(o,end,num);*o=0;
    set_text(event,(const char*)s+0x2b00);}
   break;}
  case 4:{
   /* PS3 case: icon slot 0xf5a294f3 gets the medal's vtex image (record name at PS4 +0x18),
    * or the placeholder type 0x10 while it is not loaded (PS4 layout as in 0x753330 op 4). */
   if(row>=l->count||(uint32_t)event[0x14]!=0xf5a294f3u)break;
   uint8_t *r=medal_record(s,l->ids[row]);if(!r)break;
   const char *name=*(const char**)(r+0x18);
   /* Sanity check: a short printable path like "net-medals/assistant". */
   if(name){unsigned k=0;int slash=0;for(;k<48&&name[k];k++){if(name[k]=='/')slash=1;if(name[k]<0x20||name[k]>0x7e){name=0;break;}}if(name&&(!slash||k==0||k>=48))name=0;}
   void *(*vtex)(uintptr_t,const char*)=(void*(*)(uintptr_t,const char*))s->natives[NATIVE_VTEX_FIND];
   void *tex=(vtex&&name)?vtex(base+0x1b31128,name):0;
   for(int k=4;k<=11;k++)event[k]=0;
   if(tex){event[2]=2;*(void**)(event+4)=tex;event[9]=-1;event[11]=-1;event[12]=0x3f800000;}
   else{event[2]=0x10;event[9]=-1;event[11]=-1;event[12]=0x40100000;}
   break;}
  case 5:event[1]=(int32_t)l->selected;break;
  case 6:event[1]=(int32_t)l->count-1;break;
  case 9:medal_build(base,s);break;
  case 11:if((uint32_t)event[0x13]==0x67a48eb9u)event[14]=4;break;
 }
 return 1;}

/* Custom game Time Limit (datasource 0x29, PS4 0x753b70 = PS3 0x3310a8). Both builds format the
 * value with the rule's text (rules table 0xf985c7b6, 0x58-byte entries: base +0x1c, max +0x20,
 * step +0x24, texts +0x28/+0x2c). The PS3 text was "[A] minutes"; the PS4 text table changed those
 * entries to a bare "minutes", so the number disappears. Run the PS4 source, then put the value in
 * front when the text carries no digit. */
static int time_limit_source(uintptr_t base,int32_t *event,ProfileState *s){
 uint8_t *source=*(uint8_t**)((uint8_t*)event+0x98);if(!source||*(uint32_t*)(source+0x1c)!=0x4c414e29)return 0;
 void (*native)(int32_t*)=(void(*)(int32_t*))s->natives[NATIVE_DS_TIME_LIMIT];
 if(!native)return 1;
 native(event);
 if(event[0]!=2||event[0x15]!=0||!s->lookup)return 1;
 const char *t=*(const char**)((uint8_t*)event+0x40);if(!t)return 1;
 for(const char *c=t;*c;c++)if(*c>='0'&&*c<='9')return 1;
 void **ng=((void**(*)(uint32_t))s->lookup)(0x3b9a067d),**rl=((void**(*)(uint32_t))s->lookup)(0xf985c7b6);
 if(!ng||!*ng||!rl||!*rl)return 1;
 int32_t sel=*(int32_t*)(base+0x17d167c);uint8_t *g=(uint8_t*)*ng;
 if(sel<0||sel>=*(int32_t*)g)return 1;
 int32_t type=*(int32_t*)(*(uint8_t**)(g+8)+(size_t)sel*0xd0+0xc);uint8_t *r=(uint8_t*)*rl;
 if(type<0||type>=*(int32_t*)r)return 1;
 uint8_t *e=*(uint8_t**)(r+8)+(size_t)type*0x58;
 int64_t v=(int64_t)*(int32_t*)(e+0x24)*event[1]+*(int32_t*)(e+0x1c);
 char *o=(char*)s+0x2c00,*end=o+120;o=fmt_int(o,end,v,0);o=fmt_append(o,end," ");o=fmt_append(o,end,t);*o=0;
 set_text(event,(const char*)s+0x2c00);
 return 1;
}
/* LIST_BOOSTERS on PAGE_BOOSTERS (competitive slots 1/2 and the co-op slot): PS4 0x43 is a stub
 * (0x7597f0); PS3 f_33b574. Current slot = 0x17d9990 (PS3 0x1232a88), loadout = 0x17d9988 (PS3
 * 0x1232a80, 4 = co-op). The per-slot lists (PS3 0x1232be0 + slot*0x108, filled by f_32cb90 =
 * PS4 0x7596a0, PS4 size 0x210: slot byte at +0, def pointers from +8, count at +0x208) and the
 * per-user selection (PS3 0x1232bd0) have no PS4 storage, so they live at state+0x3000.
 * PS4 booster def (0x38 bytes): +4 id, +8 name, +0xc slot, +0x10 tier count, +0x18 tiers (12-byte
 * entries, text id first), +0x20 cost, +0x28 icon, +0x30 loadout flags.
 * Icons (UI texture table, from PS3 net.pak) are not set yet. */
typedef struct { uint8_t lists[4][0x210]; int32_t selected[4]; uint32_t built; } BoosterState;
static BoosterState *booster_state(ProfileState *s){return (BoosterState*)((uint8_t*)s+0x3000);}
static uint8_t *booster_list(uintptr_t base,ProfileState *s){
 uint32_t slot=*(uint32_t*)(base+0x17d9990);if(slot>3)slot=0;
 uint8_t *l=booster_state(s)->lists[slot];l[0]=(uint8_t)slot;return l;}
static uint8_t *booster_def(uint8_t *l,int32_t i){
 if(i<0||i>=*(int32_t*)(l+0x208))return 0;return *(uint8_t**)(l+8+(size_t)i*8);}
static int booster_unlocked(ProfileState *s,uint32_t user,uint32_t id){
 void *(*find)(uintptr_t,int,int)=(void*(*)(uintptr_t,int,int))s->natives[NATIVE_UNLOCK_FIND];
 uint64_t (*is)(uintptr_t,uint32_t,void*)=(uint64_t(*)(uintptr_t,uint32_t,void*))s->natives[NATIVE_UNLOCK_IS];
 uintptr_t base=s->profile_base;
 if(!find||!is)return 0;return (uint8_t)is(base+0x17f3068,user,find(base+0x17f3068,(int)id,0))!=0;}
static int boosters_source(uintptr_t base,int32_t *event,ProfileState *s){
 uint8_t *source=*(uint8_t**)((uint8_t*)event+0x98);if(!source||*(uint32_t*)(source+0x1c)!=0x4c414e43)return 0;
 s->profile_base=base;
 if(!sym_ready(base,s,0x916e03b0)){if(event[0]==6)event[1]=-1;return 1;}
 BoosterState *bs=booster_state(s);uint8_t *l=booster_list(base,s);
 uint32_t user=player_user(base)&3,slot=l[0];int32_t loadout=*(int32_t*)(base+0x17d9988);
 uint8_t *pr=player_profile(base);uint32_t money=pr?*(uint32_t*)(pr+0x74c):0;
 int (*level)(uintptr_t,uint32_t,void*)=(int(*)(uintptr_t,uint32_t,void*))s->natives[NATIVE_BOOSTER_LEVEL];
 if(!(bs->built&(1u<<slot))){void (*build)(void*,uint32_t)=(void(*)(void*,uint32_t))s->natives[NATIVE_BOOSTER_LIST];
  if(build){build(l,loadout==4);bs->built|=1u<<slot;}}
 int32_t row=event[1];uint8_t *d;
 switch(event[0]){
  case 1:{uint32_t id=slot==1?0x73b984c2u:slot==3?0xf917b294u:slot==0?0x77789975u:0;set_text(event,id?text(base,s,id):"");break;}
  case 2:{
   if(!(d=booster_def(l,row)))break;
   uint32_t sub=(uint32_t)event[0x15],id=*(uint32_t*)(d+4);int lv=level?level(base+0x17c3b40,user,d):-1;
   int32_t ntiers=*(int32_t*)(d+0x10);uint8_t *tiers=*(uint8_t**)(d+0x18);
   char *o=(char*)s+0x2c80,*end=o+250;*o=0;
   if(sub==0){set_text(event,text(base,s,*(uint32_t*)(d+8)));break;}
   if(sub==3){ /* "%s %d\n%s": Level, current tier+1, tier text */
    if(lv<0||lv>=ntiers||!tiers)break;
    o=fmt_append(o,end,text(base,s,0xadfc7da6u));o=fmt_append(o,end," ");o=fmt_int(o,end,lv+1,0);o=fmt_append(o,end,"\n");
    o=fmt_append(o,end,text(base,s,*(uint32_t*)(tiers+(size_t)lv*0xc)));*o=0;set_text(event,(char*)s+0x2c80);break;}
   if(sub==4){
    if(!booster_unlocked(s,user,id)){const char *(*it)(uint32_t,uint32_t)=(const char*(*)(uint32_t,uint32_t))s->natives[NATIVE_ITEM_TEXT];set_text(event,it?it(id,0):"");break;}
    int32_t cost=*(int32_t*)(d+0x20);uint64_t (*online)(uintptr_t)=(uint64_t(*)(uintptr_t))s->natives[NATIVE_ONLINE_ALLOWED];
    if(cost<1||!online||!(uint8_t)online(base+0x17cb060)){set_text(event,"");break;} /* PS3: next-tier mission text here */
    o=fmt_append(o,end,text(base,s,0x6ba044a1u));o=fmt_append(o,end,money<(uint32_t)cost?"|@Cffff0000|":"|@Cff7cce96|");o=fmt_int(o,end,cost,1);*o=0;
    set_text(event,(char*)s+0x2c80);break;}
   if(sub==8){o=fmt_append(o,end,text(base,s,0xba6d58f7u));o=fmt_append(o,end,"|@Cff00ff00|");o=fmt_int(o,end,money,1);*o=0;set_text(event,(char*)s+0x2c80);break;}
   int t=sub==0x80d4cc7bu?0:sub==0xadacdbf3u?1:sub==0xb684d68bu?2:-1;
   if(t<0||!tiers||t>=ntiers)break;
   if(t==0&&ntiers==1){set_text(event,text(base,s,*(uint32_t*)tiers));break;}
   o=fmt_append(o,end,text(base,s,0xadfc7da6u));o=fmt_append(o,end," ");o=fmt_int(o,end,t+1,0);o=fmt_append(o,end,": ");
   o=fmt_append(o,end,text(base,s,*(uint32_t*)(tiers+(size_t)t*0xc)));*o=0;set_text(event,(char*)s+0x2c80);break;}
  case 5:event[1]=bs->selected[user];break;
  case 6:event[1]=*(int32_t*)(l+0x208)-1;break;
  case 9:case 0x17:{
   void (*build)(void*,uint32_t)=(void(*)(void*,uint32_t))s->natives[NATIVE_BOOSTER_LIST];
   if(build){build(l,loadout==4);bs->built|=1u<<slot;}
   if((d=booster_def(l,bs->selected[user]))){void (*seen)(uintptr_t,uint32_t,uint32_t,uint32_t)=(void(*)(uintptr_t,uint32_t,uint32_t,uint32_t))s->natives[NATIVE_UNLOCK_SEEN];if(seen)seen(base+0x17f3068,user,*(uint32_t*)(d+4),0);}
   break;}
  case 10:if(booster_def(l,row))bs->selected[user]=row;break;
  case 11:{
   if(!(d=booster_def(l,row)))break;
   if((uint32_t)event[0x13]==0x67a48eb9u){if(booster_unlocked(s,user,*(uint32_t*)(d+4))&&level)event[14]=level(base+0x17c3b40,user,d)+1;}
   else if((uint32_t)event[0x13]==0x18567c73u){void *(*next)(uintptr_t,uint32_t)=(void*(*)(uintptr_t,uint32_t))s->natives[NATIVE_MISSION_NEXT];if(next)event[14]=next(base+0x17da200+(size_t)user*0x1018,*(uint32_t*)(d+4))!=0;}
   break;}
  case 0x11:event[1]=8;break;
  case 0x16:{ /* equip (PS3 f_29dea0 with equip=1, stripped on PS4) */
   if(!(d=booster_def(l,event[0x17])))break;uint32_t id=*(uint32_t*)(d+4);
   if(!booster_unlocked(s,user,id))break;
   int32_t st=*(int32_t*)(d+0xc);
   if(st>=0&&st<4)*(uint32_t*)(base+0x17c3b48+(size_t)st*4)=0;
   if(st>=0&&st<3&&pr){void (*set)(void*,int,uint32_t,int32_t)=(void(*)(void*,int,uint32_t,int32_t))s->natives[NATIVE_SET_LOADOUT];if(set)set(pr,st+4,id,loadout);}
   else if(st==3)*(uint32_t*)(base+0x17c3b40+(size_t)(user&1)*4)=id;
   break;}
 }
 return 1;}

/* PS3 f_79a604(textdb,buf,size,id,value,0) = PS4 0xf32e00: a text entry with its number filled in. */
static void text_num(uintptr_t base,ProfileState *s,char *buf,uint32_t size,uint32_t id,int32_t v){
 void (*tn)(uintptr_t,char*,uint32_t,uint32_t,int64_t,int64_t)=(void(*)(uintptr_t,char*,uint32_t,uint32_t,int64_t,int64_t))s->natives[NATIVE_TEXT_NUM];
 buf[0]=0;if(tn)tn(base+0x17026b8,buf,size,id,v,0);}
/* LIST_MEDAL_STREAKS (Medal Kickback) on PAGE_MEDAL_STREAKS: PS4 0x44 is a stub (0x7667e0); PS3
 * f_331e6c. Row 0 is "None", row n is the n-th kickback whose flags match the loadout (flag 2 in
 * the co-op loadout 4, else 1). Kickback table: symbol 0xd9f6ada2 {count, entries}; PS3 entries
 * 0x1c bytes, PS4 0x28 (+0 id, +4 name, +8 medal cost, +0xc tier count, +0x18 tiers, +0x20 flags;
 * tiers 0x18 bytes, stride from PS4 0x74fc10: +0 icon id, +4 description, +8 duration float). Selection = 0x17d998c (PS3 0x1232a84),
 * set by the PS4 loadout screen. Equipped kickback = loadout item 7. */
static uint8_t *kickback_nth(ProfileState *s,int32_t n,int coop){
 void *(*lookup)(uint32_t)=(void*(*)(uint32_t))s->natives[NATIVE_LOOKUP_SYMBOL];
 uint8_t *t=lookup?(uint8_t*)lookup(0xd9f6ada2):0;if(!t||n<0)return 0;
 uint32_t cnt=*(uint32_t*)t,flag=coop?2:1;uint8_t *e=*(uint8_t**)(t+8);
 for(uint32_t i=0;e&&i<cnt;i++,e+=0x28)if(*(uint32_t*)(e+0x20)&flag){if(n==0)return e;n--;}
 return 0;}
static int32_t kickback_count(ProfileState *s,int coop){
 void *(*lookup)(uint32_t)=(void*(*)(uint32_t))s->natives[NATIVE_LOOKUP_SYMBOL];
 uint8_t *t=lookup?(uint8_t*)lookup(0xd9f6ada2):0;if(!t)return 0;
 uint32_t cnt=*(uint32_t*)t,flag=coop?2:1,n=0;uint8_t *e=*(uint8_t**)(t+8);
 for(uint32_t i=0;e&&i<cnt;i++,e+=0x28)if(*(uint32_t*)(e+0x20)&flag)n++;
 return (int32_t)n;}
static void *unlock_entry(uintptr_t base,ProfileState *s,uint32_t id){
 void *(*find)(uintptr_t,int,int)=(void*(*)(uintptr_t,int,int))s->natives[NATIVE_UNLOCK_FIND];
 return find?find(base+0x17f3068,(int)id,0):0;}
static int kickback_source(uintptr_t base,int32_t *event,ProfileState *s){
 uint8_t *source=*(uint8_t**)((uint8_t*)event+0x98);if(!source||*(uint32_t*)(source+0x1c)!=0x4c414e44)return 0;
 s->profile_base=base;
 if(!sym_ready(base,s,0xd9f6ada2)){if(event[0]==6)event[1]=-1;return 1;}
 int32_t loadout=*(int32_t*)(base+0x17d9988);int coop=loadout==4;
 uint32_t user=player_user(base)&3;uint8_t *pr=player_profile(base);uint32_t money=pr?*(uint32_t*)(pr+0x74c):0;
 int32_t row=event[1];uint8_t *e;
 char *o=(char*)s+0x2c80,*end=o+250;*o=0;
 switch(event[0]){
  case 1:set_text(event,text(base,s,0x87b43182u));
   /* fallthrough: PS3 also marks the selected kickback as seen */
  case 9:
   if((e=kickback_nth(s,*(int32_t*)(base+0x17d998c)-1,coop))){void (*seen)(uintptr_t,uint32_t,uint32_t,uint32_t)=(void(*)(uintptr_t,uint32_t,uint32_t,uint32_t))s->natives[NATIVE_UNLOCK_SEEN];if(seen)seen(base+0x17f3068,user,*(uint32_t*)e,0);}
   break;
  case 2:{
   uint32_t sub=(uint32_t)event[0x15];e=kickback_nth(s,row-1,coop);
   if(sub==0){if(row==0)set_text(event,text(base,s,0x9d3c6e80u));else if(e)set_text(event,text(base,s,*(uint32_t*)(e+4)));break;}
   if(sub==3){
    if(row==0){set_text(event,text(base,s,0x034c8e1eu));break;}
    if(!e)break;
    /* 0x74eba0 (PS3 f_328ccc) only reads the user index at state+0xc (PS3 state = 0x12306ac+user*0x20) */
    int (*lvl)(void*,uint32_t)=(int(*)(void*,uint32_t))s->natives[NATIVE_KICKBACK_LEVEL];
    uint32_t kst[8]={0};kst[3]=user;
    int lv=lvl?lvl(kst,*(uint32_t*)e):0;
    uint8_t *tiers=*(uint8_t**)(e+0x18);if(lv<0)lv=0;
    /* "%s %d\\%s\\%s": Level, level+1, tier text, "Costs [A] Medals" */
    char tmp[128];tmp[0]=0;text_num(base,s,tmp,sizeof tmp,0x69443c24u,*(int32_t*)(e+8));
    o=fmt_append(o,end,text(base,s,0xadfc7da6u));o=fmt_append(o,end," ");o=fmt_int(o,end,lv+1,0);o=fmt_append(o,end,"\\");
    if(tiers&&lv<*(int32_t*)(e+0xc))o=fmt_append(o,end,text(base,s,*(uint32_t*)(tiers+(size_t)lv*0x18+4)));
    o=fmt_append(o,end,"\\");o=fmt_append(o,end,tmp);*o=0;set_text(event,(char*)s+0x2c80);break;}
   if(sub==4){
    if(row==0||!e)break;
    if(booster_unlocked(s,user,*(uint32_t*)e))set_text(event,text(base,s,0xecf338f6u));  /* PS3 f_29cd8c */
    else{const char *(*it)(uint32_t,uint32_t)=(const char*(*)(uint32_t,uint32_t))s->natives[NATIVE_ITEM_TEXT];set_text(event,it?it(*(uint32_t*)e,0):"");}
    break;}
   if(sub==8){o=fmt_append(o,end,text(base,s,0xba6d58f7u));o=fmt_append(o,end,"|@Cff00ff00|");o=fmt_int(o,end,money,1);*o=0;set_text(event,(char*)s+0x2c80);break;}
   if(!e)break;
   uint8_t *tiers=*(uint8_t**)(e+0x18);
   if(sub==0x80d4cc7bu){if(tiers)set_text(event,text(base,s,*(uint32_t*)(tiers+4)));break;}
   if(sub==0xadacdbf3u){
    if(!tiers)break;float dur=*(float*)(tiers+8);
    o=fmt_append(o,end,text(base,s,0x3badfdd5u));o=fmt_append(o,end," ");
    if(dur<=0.0f)o=fmt_append(o,end,text(base,s,0xa80e2d11u));
    else{o=fmt_int(o,end,(int64_t)(dur+0.5f),0);o=fmt_append(o,end," ");o=fmt_append(o,end,text(base,s,0xe7624faau));}
    *o=0;set_text(event,(char*)s+0x2c80);break;}
   if(sub==0xb684d68bu){set_text(event,fmt(s,text(base,s,0x819103ccu),*(int32_t*)(e+8),0,0,0));break;}
   break;}
  case 3:{ /* PS3: not unlocked -> result 2 */
   if(row==0||!(e=kickback_nth(s,row-1,coop)))break;
   if(!booster_unlocked(s,user,*(uint32_t*)e))event[14]=2;
   break;}
  case 5:event[1]=*(int32_t*)(base+0x17d998c);break;
  case 6:event[1]=kickback_count(s,coop);break;
  case 10:*(int32_t*)(base+0x17d998c)=row;break;
  case 11:if((uint32_t)event[0x13]==0x67a48eb9u)event[14]=4;break;
  case 0x11:event[1]=8;break;
  case 0x16:{ /* equip (PS3 f_328a40, stripped on PS4); buying is not ported */
   void (*set)(void*,int,uint32_t,int32_t)=(void(*)(void*,int,uint32_t,int32_t))s->natives[NATIVE_SET_LOADOUT];
   if(!set||!pr)break;
   int32_t r=event[0x17];
   if(r<=0){set(pr,7,0,loadout);break;}
   if(!(e=kickback_nth(s,r-1,coop)))break;
   int (*status)(uintptr_t,uint32_t,void*)=(int(*)(uintptr_t,uint32_t,void*))s->natives[NATIVE_UNLOCK_STATUS];
   if(status&&status(base+0x17f3068,user,unlock_entry(base,s,*(uint32_t*)e))==0)set(pr,7,*(uint32_t*)e,loadout);
   break;}
 }
 return 1;}

/* Icon fill that skips ids missing from the UI material table (the net level is not loaded on
 * PS4 yet; a type-3 image with index -1 crashes the text draw, see the TAB passthrough). */
static void set_icon_safe(uintptr_t base,int32_t *event,uint32_t id){if(icon_index(base,id)>=0)set_icon(base,event,id);}
static void set_lock_overlay(int32_t *event){
 event[2]=0x2f;event[3]=0;event[4]=0;event[5]=0;event[6]=0;event[7]=-1;event[8]=0;event[9]=-1;event[10]=0;event[11]=-1;event[12]=0x3f800000;}
/* Co-op weapon table: symbol 0x556e3fb5 {count, entries}; PS3 entries 0xc {id, mods*, nmods},
 * PS4 0x18 {+0 id, +8 mods*, +0x10 nmods} (0x767a60/0x5b3e40). Mod entries 0x14 bytes, +0 mod bit.
 * Weapon defs: hashed symbol 0x61d589d9 {u32 n, u32 keys*, defs**}, def +4 name (PS3 f_6ce8b4,
 * inlined on PS4 in 0x767a60). */
static uint8_t *coop_weapon(ProfileState *s,uint32_t idx){
 void **(*lookup)(uint32_t)=(void**(*)(uint32_t))s->lookup;
 void **h=lookup?lookup(0x556e3fb5):0;uint8_t *t=h?(uint8_t*)*h:0;if(!t||idx>=*(uint32_t*)t)return 0;
 return *(uint8_t**)(t+8)+(size_t)idx*0x18;}
static int32_t coop_weapon_count(ProfileState *s){
 void **(*lookup)(uint32_t)=(void**(*)(uint32_t))s->lookup;
 void **h=lookup?lookup(0x556e3fb5):0;uint8_t *t=h?(uint8_t*)*h:0;return t?*(int32_t*)t:0;}
static uint8_t *weapon_def(ProfileState *s,uint32_t id){
 uint32_t *(*lookup)(uint32_t)=(uint32_t*(*)(uint32_t))s->natives[NATIVE_LOOKUP_SYMBOL];
 uint32_t *h=lookup?lookup(0x61d589d9):0;if(!h||!h[0])return 0;
 uint32_t *keys=*(uint32_t**)(h+2);uint8_t **defs=*(uint8_t***)(h+4);
 uint32_t i=id%h[0],st=i;
 while(keys[i]!=id){if(!keys[i])return 0;i=(i+1)%h[0];if(i==st)return 0;}
 return defs[i];}
/* Mod bit -> name / description text (PS3 f_6cd9d8 / f_6cdae0, inlined on PS4 in 0x7680f0). */
static uint32_t mod_name(uint32_t bit){
 static const uint32_t n[13]={0x9fe22c98,0xeebca10f,0x317982c3,0x868a039f,0xcf42ac41,0x75ac8784,0x2cb4e03f,0xca8044df,0x90a93da0,0x2be72ef2,0xc2fb6973,0xb6ab5db4,0x815211c8};
 for(int i=0;i<13;i++)if(bit==(1u<<i))return n[i];return 0x58840ecd;}
static uint32_t mod_desc(uint32_t bit){
 static const uint32_t d[13]={0x00ac24a8,0xaa3e1660,0xe2fa14a6,0x2d14e5b5,0x6ee89d56,0xd099ee2f,0xb3fae80f,0x611ea2f5,0x3b37db8a,0x8079c8d8,0x67ce00d8,0xe5f1eb97,0xefad05da};
 for(int i=0;i<13;i++)if(bit==(1u<<i))return d[i];return 0xd90c7fa3;}
/* Mod bit -> icon id via symbol 0x36a4c63b (PS3 f_6cdd7c, PS4 0x7680f0 op 4). */
static uint32_t mod_icon(ProfileState *s,uint32_t bit){
 void **(*lookup)(uint32_t)=(void**(*)(uint32_t))s->lookup;
 void **h=lookup?lookup(0x36a4c63b):0;uint32_t *t=h?(uint32_t*)*h:0;if(!t)return 0;
 for(uint32_t i=0;i<t[0]&&i<32;i++)if((1u<<i)==bit)return (*(uint32_t**)(t+2))[i];
 return 0;}
static int unlock_status(uintptr_t base,ProfileState *s,uint32_t user,uint32_t id,uint32_t mod){
 void *(*find)(uintptr_t,int,int)=(void*(*)(uintptr_t,int,int))s->natives[NATIVE_UNLOCK_FIND];
 int (*status)(uintptr_t,uint32_t,void*)=(int(*)(uintptr_t,uint32_t,void*))s->natives[NATIVE_UNLOCK_STATUS];
 if(!find||!status)return 3;return (uint8_t)status(base+0x17f3068,user,find(base+0x17f3068,(int)id,(int)mod));}
static void money_text(uintptr_t base,ProfileState *s,int32_t *event){
 uint8_t *pr=player_profile(base);char *o=(char*)s+0x2c80,*end=o+250;
 o=fmt_append(o,end,text(base,s,0xba6d58f7u));o=fmt_append(o,end,"|@Cff00ff00|");o=fmt_int(o,end,pr?*(uint32_t*)(pr+0x74c):0,1);*o=0;
 set_text(event,(char*)s+0x2c80);}
#define COOP_SEL(s) (*(int32_t*)((uint8_t*)(s)+0x3900)) /* PS3 0x1232ac0, no PS4 storage */
/* LIST_COOP_WEAPON (0xa6, PS4 stub; PS3 f_338ab8): pick the co-op weapon, stored in 0x17d9998
 * (PS3 0x1232aa0), then refresh the 3D preview and open the co-op weapon mods page. */
static int coop_weapon_source(uintptr_t base,int32_t *event,ProfileState *s){
 uint8_t *source=*(uint8_t**)((uint8_t*)event+0x98);if(!source||*(uint32_t*)(source+0x1c)!=0x4c414ea6)return 0;
 s->profile_base=base;uint32_t user=player_user(base)&3;
 if(!sym_ready(base,s,0x556e3fb5)||!sym_ready(base,s,0x61d589d9)){if(event[0]==6)event[1]=-1;return 1;}
 int32_t row=event[1];uint8_t *w;
 switch(event[0]){
  case 2:
   if(event[0x15]==0&&(w=coop_weapon(s,(uint32_t)row))){uint8_t *d=weapon_def(s,*(uint32_t*)w);if(d)set_text(event,text(base,s,*(uint32_t*)(d+4)));}
   break;
  case 4:{
   uint32_t slot=(uint32_t)event[0x14];
   if(slot==0xbb36238eu){
    uint8_t (*nw)(uintptr_t,uint32_t,uint32_t)=(uint8_t(*)(uintptr_t,uint32_t,uint32_t))s->natives[NATIVE_UNLOCK_NEW];
    if((w=coop_weapon(s,(uint32_t)row))&&nw&&nw(base+0x17f3068,user,*(uint32_t*)w))set_lock_overlay(event);
   }else if(slot==0x5095aa91u||slot==0x5454b726u){
    uint8_t *pr=player_profile(base);uint32_t mask=pr?*(uint8_t*)(pr+0x1534+row):0;
    if(!mask||!(w=coop_weapon(s,(uint32_t)row)))break;
    uint32_t want=slot==0x5095aa91u?0:1,n=*(uint32_t*)(w+0x10);uint8_t *mods=*(uint8_t**)(w+8);
    for(uint32_t i=0;mods&&i<n&&i<8;i++)if(mask>>i&1){if(want==0){set_icon_safe(base,event,mod_icon(s,*(uint32_t*)(mods+i*0x14)));break;}want--;}
   }
   break;}
  case 5:event[1]=COOP_SEL(s);break;
  case 6:event[1]=coop_weapon_count(s)-1;break;
  case 10:COOP_SEL(s)=row;break;
  case 0x11:event[1]=4;break;
  case 0x16:{
   *(int32_t*)(base+0x17d9998)=event[0x17];
   void (*preview)(uintptr_t,int,int,int)=(void(*)(uintptr_t,int,int,int))(base+0x5b3e40); /* PS3 f_2548b4(menu+0x10,0,1) -> f_2069b4(.,0,0,1) */
   preview(base+0x17bba90,0,0,1);
   *(int32_t*)(base+0x1b6ae68)=0;
   if(s->show_page)show(base,s,0xba7ae3b4u,0);
   break;}
 }
 return 1;}
/* LIST_COOP_WEAPON_MODS (0xa7, PS4 stub; PS3 f_33a3b8): toggle mods of the current co-op weapon;
 * the per-weapon mod mask is profile byte +0x1534+weapon (PS3 data+0x12ec, f_355c54/f_355c04);
 * selection 0x1b6ae68 (PS3 0x1233204). The 3D preview update (f_253bdc) and buying are not ported. */
static int coop_mods_source(uintptr_t base,int32_t *event,ProfileState *s){
 uint8_t *source=*(uint8_t**)((uint8_t*)event+0x98);if(!source||*(uint32_t*)(source+0x1c)!=0x4c414ea7)return 0;
 s->profile_base=base;uint32_t user=player_user(base)&3;
 if(!sym_ready(base,s,0x556e3fb5)||!sym_ready(base,s,0x61d589d9)){if(event[0]==6)event[1]=-1;return 1;}
 uint32_t widx=*(uint32_t*)(base+0x17d9998);uint8_t *w=coop_weapon(s,widx);if(!w){if(event[0]==6)event[1]=-1;return 1;}
 uint32_t wid=*(uint32_t*)w,n=*(uint32_t*)(w+0x10);uint8_t *mods=*(uint8_t**)(w+8);
 uint8_t *pr=player_profile(base);
 int32_t row=event[1];uint32_t bit=(mods&&row>=0&&(uint32_t)row<n)?*(uint32_t*)(mods+(size_t)row*0x14):0;
 switch(event[0]){
  case 2:{
   uint32_t sub=(uint32_t)event[0x15];
   if(sub==0){if(bit)set_text(event,text(base,s,mod_name(bit)));}
   else if(sub==3){if(bit)set_text(event,text(base,s,mod_desc(bit)));}
   else if(sub==1){if(bit&&unlock_status(base,s,user,wid,bit)!=0){const char *(*it)(uint32_t,uint32_t)=(const char*(*)(uint32_t,uint32_t))s->natives[NATIVE_ITEM_TEXT];if(it)set_text(event,it(wid,bit));}}
   else if(sub==8){int st=bit?unlock_status(base,s,user,wid,bit):0;if(st==1||st==2)money_text(base,s,event);}
   else if(sub==0x5d822913u){uint8_t *d=weapon_def(s,wid);if(d)set_text(event,text(base,s,*(uint32_t*)(d+4)));}
   break;}
  case 4:{
   uint32_t slot=(uint32_t)event[0x14];if(!bit)break;
   if(slot==0xf5a294f3u){set_icon_safe(base,event,mod_icon(s,bit));break;}
   if(slot!=0)break;
   uint8_t (*cb)(uintptr_t,uint32_t,uint32_t,uint32_t)=(uint8_t(*)(uintptr_t,uint32_t,uint32_t,uint32_t))s->natives[NATIVE_CAN_BUY];
   if(cb&&cb(base+0x17f3068,user,wid,bit)){set_lock_overlay(event);break;}
   switch(unlock_status(base,s,user,wid,bit)){
    case 0:if(pr&&(*(uint8_t*)(pr+0x1534+widx)>>(row&31)&1))set_icon_safe(base,event,0x08208d0au);break;
    case 1:set_icon_safe(base,event,0xc2bb8920u);break;
    case 2:set_icon_safe(base,event,0x83817313u);break;
    case 3:set_icon_safe(base,event,0x7bebc95fu);break;}
   break;}
  case 5:event[1]=*(int32_t*)(base+0x1b6ae68);break;
  case 6:event[1]=(int32_t)n-1;break;
  case 9:{
   int32_t sel=*(int32_t*)(base+0x1b6ae68);
   void (*seen)(uintptr_t,uint32_t,uint32_t,uint32_t)=(void(*)(uintptr_t,uint32_t,uint32_t,uint32_t))s->natives[NATIVE_UNLOCK_SEEN];
   if(seen&&mods&&sel>=0&&(uint32_t)sel<n)seen(base+0x17f3068,user,wid,*(uint32_t*)(mods+(size_t)sel*0x14));
   break;}
  case 10:*(int32_t*)(base+0x1b6ae68)=row;break;
  case 0x16:{
   int32_t r=event[0x17];if(!pr||!mods||r<0||(uint32_t)r>=n)break;
   if(unlock_status(base,s,user,wid,*(uint32_t*)(mods+(size_t)r*0x14))==0)*(uint8_t*)(pr+0x1534+widx)^=(uint8_t)(1u<<(r&7));
   break;}
 }
 return 1;}

/* LIST_MISSIONS (0xd9, PS4 stub; PS3 f_346e4c). Missions object 0x17da200 + user*0x1018 (PS3
 * 0x1233980 + user*0xa14), user 0x17c23a0 (as PS4 OnButton 0x64fa08): +0x1004 user, +0x100c set,
 * +0x1010 selected row. Mission sets: symbol 0x4360c988 (PS4 cache 0x1b6d360, see 0x772950),
 * {count, sets}, sets 0x20 bytes {+0 id, +8 count, +0x18 missions}; missions 0x4c bytes (same on
 * both): +0 id, +4 name, +8 description, +0x10 kind, +0x14 target, +0x18 games, +0x20 level,
 * +0x24 reward, +0x44 cost. Profile: active mission +0x1864, its start time +0x1868, progress
 * +0xf4c, completed bits +0xecc (PS3 data +0x161c/+0x1620/+0xd04/+0xc84). */
#define MISSION_SET(s) (*(int32_t*)((uint8_t*)(s)+0x3904)) /* PS3 0x123384c */
static uint8_t *mission_obj(uintptr_t base){return (uint8_t*)(base+0x17da200)+(size_t)(*(uint32_t*)(base+0x17c23a0)&3)*0x1018;}
static uint8_t *mission_profile(uintptr_t base){uint8_t *p=*(uint8_t**)(base+0x17dd610);return p?p+(size_t)(*(uint32_t*)(base+0x17c23a0)&3)*0x7b00:0;}
static uint8_t *mission_set(uintptr_t base,ProfileState *s,int32_t set){
 if(!sym_ready(base,s,0x4360c988))return 0;
 uint8_t *(*lookup)(uint32_t)=(uint8_t*(*)(uint32_t))s->natives[NATIVE_LOOKUP_SYMBOL];
 uint8_t *t=lookup?lookup(0x4360c988):0;if(!t||set<0||(uint32_t)set>=*(uint32_t*)t)return 0;
 return *(uint8_t**)(t+8)+(size_t)set*0x20;}
static int mission_done(uint8_t *pr,uint32_t id){return pr&&id<0x400&&(pr[0xecc+(id>>3)]>>(id&7)&1);}
static int32_t player_level(uintptr_t base,ProfileState *s,uint8_t *pr){ /* PS3 f_5ba34 */
 int32_t ov=*(int32_t*)(base+0x17d7abc);if(ov)return ov-1;
 uint32_t (*rank)(void*)=(uint32_t(*)(void*))s->natives[NATIVE_PROFILE_RANK];return (rank&&pr)?(int32_t)rank(pr):0;}
static int missions_source(uintptr_t base,int32_t *event,ProfileState *s){
 uint8_t *source=*(uint8_t**)((uint8_t*)event+0x98);if(!source||*(uint32_t*)(source+0x1c)!=0x4c414ed9)return 0;
 s->profile_base=base;
 uint8_t *mo=mission_obj(base),*pr=mission_profile(base);
 if(active_page(base)==0xdfdb36fau){ /* PS3: the page's tab picks the mission set */
  uint32_t tab=current_tab(base,s);int32_t set=tab==0x102fc82eu?1:tab==0x35fa8d17u?2:0;
  if(MISSION_SET(s)!=set){*(int32_t*)(mo+0x1010)=0;MISSION_SET(s)=set;}}
 *(int32_t*)(mo+0x100c)=MISSION_SET(s);
 uint8_t *set=mission_set(base,s,MISSION_SET(s));
 if(!set){if(event[0]==6)event[1]=-1;return 1;}
 uint32_t count=*(uint32_t*)(set+8);uint8_t *list=*(uint8_t**)(set+0x18);
 int32_t row=event[1];uint8_t *m=(list&&row>=0&&(uint32_t)row<count)?list+(size_t)row*0x4c:0;
 uint32_t active=pr?*(uint32_t*)(pr+0x1864):0;int32_t level=player_level(base,s,pr);
 uint32_t money=pr?*(uint32_t*)(pr+0x74c):0;
 const char *normal="|@Cff7cce96|",*red="|@Cfffe7c66|";
 char *o=(char*)s+0x2c80,*end=o+250;*o=0;char num[48];
 switch(event[0]){
  case 10:*(int32_t*)(mo+0x1010)=row;break;
  case 2:{
   if(!m)break;uint32_t sub=(uint32_t)event[0x15];
   if(sub==0){set_text(event,text(base,s,*(uint32_t*)(m+4)));break;}
   if(sub==0xbee56a68u){o=fmt_append(o,end,level+1<*(int32_t*)(m+0x20)?red:normal);o=fmt_int(o,end,*(int32_t*)(m+0x20),0);*o=0;set_text(event,(char*)s+0x2c80);break;}
   if(sub==0xed9203d6u){o=fmt_append(o,end,money<*(uint32_t*)(m+0x44)?red:normal);o=fmt_int(o,end,*(uint32_t*)(m+0x44),1);*o=0;set_text(event,(char*)s+0x2c80);break;}
   if(sub==0xa6ed5ba5u){o=fmt_append(o,end,normal);o=fmt_int(o,end,*(int32_t*)(m+0x24),1);*o=0;set_text(event,(char*)s+0x2c80);break;}
   if(sub==0x8dc65186u){int32_t g=*(int32_t*)(m+0x18);text_num(base,s,(char*)s+0x2c80,0x40,g==1?0x9d59ec8au:0x38e8ba93u,g);set_text(event,(char*)s+0x2c80);break;}
   if(sub==0x7046d624u){text_num(base,s,(char*)s+0x2c80,0x100,*(uint32_t*)(m+8),*(int32_t*)(m+0x14));set_text(event,(char*)s+0x2c80);break;}
   if(sub==0x4bdd7c07u){
    if(*(uint32_t*)(m+0x10)==0x5c494554u)text_num(base,s,(char*)s+0x2c80,0x80,0x4afc6214u,*(int32_t*)(m+0x14));
    else{o=fmt_int(o,end,*(int32_t*)(m+0x14),0);*o=0;}
    set_text(event,(char*)s+0x2c80);break;}
   /* 0xdda1fdb4 (time left on the active mission, PS3 f_34a8bc) is not ported */
   break;}
  case 4:
   if(!m||event[0x14]!=0)break;
   if(mission_done(pr,*(uint32_t*)m))set_icon_safe(base,event,0x08208d0au);
   else if(*(uint32_t*)m==active)set_icon_safe(base,event,0xe2315f64u);
   else if(level+1<*(int32_t*)(m+0x20))set_icon_safe(base,event,0x7bebc95fu);
   break;
  case 5:event[1]=*(int32_t*)(mo+0x1010);break;
  case 6:event[1]=(int32_t)count-1;break;
  case 11:{
   if(!m)break;uint32_t q=(uint32_t)event[0x13];
   if(q==0xe0bf7062u)event[14]=*(uint32_t*)m==active;
   else if(q==0x4bdd7c07u)event[14]=*(int32_t*)(m+0x14);
   else if(q==0x63f35e57u)event[14]=(*(uint32_t*)m==active&&active)?*(int32_t*)(pr+0xf4c):0;
   else if(q==0x464129abu)event[14]=mission_done(pr,*(uint32_t*)m);
   break;}
  case 0x16:{ /* PS3: active one -> offer to cancel (OnButton 0xe6452a77, native); else offer to take it (0xea1f40c5) */
   uint8_t *sel=list&&*(uint32_t*)(mo+0x1010)<count?list+(size_t)*(uint32_t*)(mo+0x1010)*0x4c:0;if(!sel)break;
   if(*(uint32_t*)sel==active){show_dialog(base,s,0x84538677u,0x0260a263u,0xe6452a77u,0x075bc82d,0x1bc6f0e5u,0);break;}
   uint8_t (*can)(uint8_t*,uint8_t*,int)=(uint8_t(*)(uint8_t*,uint8_t*,int))s->natives[NATIVE_MISSION_CAN];
   if(!can||!can(mo,sel,0))break;
   show_dialog(base,s,active?0x2aac16e8u:0x6748cb03u,0x0260a263u,0xea1f40c5u,0x075bc82d,0x1dd02cc2u,0);
   break;}
 }
 return 1;}
/* LIST_TREASURES (0xd4, PS4 stub; PS3 f_340bd0), used by PAGE_NET_TREASURE_SETS (0xda577fac) and
 * the treasure list page 0xecfa738a. Sets: symbol 0xf1f296cb {count, 0xc entries: +0 id, +4 name,
 * +8 group} (same on PS4, see 0x757130). Treasures: symbol 0xb114fe00, PS3 0x48 / PS4 0x68 entries;
 * PS3 pointers at +4/+0x34/+0x3c/+0x44 grow, so PS3 +8..+0x30 sit 8 bytes later on PS4 (id +0x10,
 * cf. 0x7b7180): +0 name, +0x14 description, +0x28 set id. Owned = profile byte +0xacc+index (PS3
 * f_356470, data+0x884; PS4 0x759ba0). The PS3 list buffers (0x1232ac8 sets, 0x1233644 treasures,
 * rows 0x1232a90, set id 0x1232a98) live at state+0x3a00. Not ported: the set reward line
 * (0xe353b9c3, PS3 f_38b284/f_41233c) and the map/mode line (sub 4). */
typedef struct { uint8_t sets[256]; uint32_t nsets; uint16_t items[256]; uint32_t nitems; int32_t row; uint32_t set_id; } TreasureState;
static TreasureState *treasure_state(ProfileState *s){return (TreasureState*)((uint8_t*)s+0x3a00);}
static uint8_t *sym_table(ProfileState *s,uint32_t id){void **(*lookup)(uint32_t)=(void**(*)(uint32_t))s->lookup;void **h=lookup?lookup(id):0;return h?(uint8_t*)*h:0;}
static void treasure_build_sets(ProfileState *s,TreasureState *ts,int32_t group){ /* PS3 f_32c734 */
 uint8_t *t=sym_table(s,0xf1f296cb);ts->nsets=0;if(!t)return;
 uint32_t n=*(uint32_t*)t;uint8_t *e=*(uint8_t**)(t+8);
 for(uint32_t i=0;e&&i<n&&ts->nsets<256;i++)if(*(int32_t*)(e+i*0xc+8)==group)ts->sets[ts->nsets++]=(uint8_t)i;}
static void treasure_build_items(ProfileState *s,TreasureState *ts,uint32_t set){ /* PS3 f_32c624 */
 uint8_t *t=sym_table(s,0xb114fe00);ts->nitems=0;if(!t)return;
 uint32_t n=*(uint32_t*)t;uint8_t *e=*(uint8_t**)(t+8);
 for(uint32_t i=0;e&&i<n&&ts->nitems<256;i++)if(*(uint32_t*)(e+(size_t)i*0x68+0x28)==set)ts->items[ts->nitems++]=(uint16_t)i;}
static uint8_t *treasure_set_entry(ProfileState *s,TreasureState *ts,int32_t row){
 uint8_t *t=sym_table(s,0xf1f296cb);if(!t||row<0||(uint32_t)row>=ts->nsets||ts->sets[row]>=*(uint32_t*)t)return 0;
 return *(uint8_t**)(t+8)+(size_t)ts->sets[row]*0xc;}
static uint8_t *treasure_entry(ProfileState *s,uint32_t idx){
 uint8_t *t=sym_table(s,0xb114fe00);if(!t||idx>=*(uint32_t*)t)return 0;return *(uint8_t**)(t+8)+(size_t)idx*0x68;}
static uint8_t *treasure_tab(uintptr_t base,ProfileState *s){ /* the PAGE_NET_TREASURE_SETS entry of the tab table */
 uint8_t *t=sym_table(s,0xf1514340);if(!t)return 0;uint32_t n=*(uint32_t*)t;uint8_t *e=*(uint8_t**)(t+8);
 for(uint32_t i=0;e&&i<n;i++,e+=0x20)if(*(uint32_t*)e==0xda577facu&&*(uint32_t*)(e+4)==*(uint8_t*)(base+0x17d5c60)){
  uint8_t *tabs=*(uint8_t**)(e+0x10);int32_t cur=*(int32_t*)(e+0x18);return (tabs&&cur>=0)?tabs+(size_t)cur*0xc:0;}
 return 0;}
static int treasures_source(uintptr_t base,int32_t *event,ProfileState *s){
 uint8_t *source=*(uint8_t**)((uint8_t*)event+0x98);if(!source||*(uint32_t*)(source+0x1c)!=0x4c414ed4)return 0;
 s->profile_base=base;TreasureState *ts=treasure_state(s);
 if(!sym_table(s,0xf1f296cb)||!sym_table(s,0xb114fe00)){if(event[0]==6)event[1]=-1;return 1;}
 uint8_t *pr=player_profile(base);int32_t row=event[1];uint8_t *se;
 if(event[0x22]==1){ /* PS3 [0x1e]: the treasure list page asks for its title: the chosen set's name */
  if(event[0]==1){uint8_t *t=sym_table(s,0xf1f296cb);uint32_t n=*(uint32_t*)t;uint8_t *e=*(uint8_t**)(t+8);
   for(uint32_t i=0;e&&i<n;i++)if(*(uint32_t*)(e+i*0xc)==ts->set_id)set_text(event,text(base,s,*(uint32_t*)(e+i*0xc+4)));}
  return 1;}
 switch(event[0]){
  case 2:{
   uint32_t sub=(uint32_t)event[0x15];
   if(sub==0){if((se=treasure_set_entry(s,ts,row)))set_text(event,text(base,s,*(uint32_t*)(se+4)));}
   else if(sub==1){ /* owned/total of the set (PS3 f_9ccc4) */
    if(!(se=treasure_set_entry(s,ts,row)))break;
    uint8_t *t=sym_table(s,0xb114fe00);uint32_t n=*(uint32_t*)t,own=0,tot=0;uint8_t *e=*(uint8_t**)(t+8);
    for(uint32_t i=0;e&&i<n;i++)if(*(uint32_t*)(e+(size_t)i*0x68+0x28)==*(uint32_t*)se){tot++;if(pr&&i<0x400&&pr[0xacc+i])own++;}
    char *o=(char*)s+0x2c80,*end=o+60;o=fmt_int(o,end,own,0);o=fmt_append(o,end,"/");o=fmt_int(o,end,tot,0);*o=0;set_text(event,(char*)s+0x2c80);}
   else if(sub==0x843d4adu||sub==0x0a56c0abu){
    if(row<0||(uint32_t)row>=ts->nitems)break;uint8_t *e=treasure_entry(s,ts->items[row]);
    if(e)set_text(event,text(base,s,*(uint32_t*)(e+(sub==0x843d4adu?0:0x14))));}
   break;}
  case 4:
   if((uint32_t)event[0x14]==0xf5a294f3u){uint8_t *tab=treasure_tab(base,s);if(tab)set_icon_safe(base,event,*(uint32_t*)(tab+4));}
   else if((uint32_t)event[0x14]==0x21981e34u&&row>=0&&(uint32_t)row<ts->nitems){uint32_t i=ts->items[row];if(pr&&i<0x400&&pr[0xacc+i])set_icon_safe(base,event,0x08208d0au);}
   break;
  case 5:event[1]=ts->row;break;
  case 6:event[1]=(int32_t)ts->nsets-1;break;
  case 9:{ /* rebuild the sets of the current tab, then the treasures of the highlighted set */
   uint8_t *tab=treasure_tab(base,s);if(!tab)break;uint32_t id=*(uint32_t*)tab;
   treasure_build_sets(s,ts,id==0x49d4b22du?2:id==0x5baab3cdu?3:1);
   if((se=treasure_set_entry(s,ts,ts->row)))treasure_build_items(s,ts,*(uint32_t*)se);else ts->nitems=0;
   break;}
  case 10:ts->row=row;break;
  case 11:if((uint32_t)event[0x13]==0xbcf5ea24u)event[14]=(int32_t)ts->nitems;break;
  case 0x14:if(event[0x16]==1)event[0xf]=-1;break; /* PS3 case 0x12: [0x12]==1 -> [0xc]=-1 */
  case 0x16:
   if((se=treasure_set_entry(s,ts,event[0x17]))){ts->set_id=*(uint32_t*)se;treasure_build_items(s,ts,ts->set_id);if(s->show_page)show(base,s,0xecfa738au,0);}
   break;
 }
 return 1;}
/* Enum 0x31 on PAGE_LOBBY / PAGE_CUSTOM_LOAD (PS4 stub 0x753740; PS3 f_347a20): the chosen
 * custom game's map preview, "map name\\subtitle" and the game type name. NetMenuHost 0x17d7b30
 * (PS3 0x1230810): u8 game count, games of 0x308 bytes from 0x17d7b38 (+0 game type id), map
 * entries {u64 map*} every 0x18 from +8 (see PS4 0x753330); map: +4 name, +8 subtitle, +0x10 vtex
 * name (PS3 +0xc). Game type names: rules table 0xf985c7b6 (0x58 bytes, +0 name). */
static uint8_t *custom_map(uintptr_t base,ProfileState *s,int32_t *gametype){
 if(*(uint32_t*)(base+0x17d603c)!=2)return 0;
 uint8_t *nmh=(uint8_t*)(base+0x17d7b30);uint32_t n=nmh[0];
 if(!n){void (*refresh)(void*)=(void(*)(void*))s->natives[NATIVE_NMH_REFRESH];if(refresh)refresh(nmh);n=nmh[0];if(!n)return 0;}
 /* current game: the one whose type matches the selected net game's (0x3b9a067d, 0xd0 bytes, +0xc) */
 uint8_t *ng=sym_table(s,0x3b9a067d);int32_t sel=*(int16_t*)(base+0x17d167c);uint32_t gi=0;
 if(ng&&sel>=0&&(uint32_t)sel<*(uint32_t*)ng){uint32_t type=*(uint32_t*)(*(uint8_t**)(ng+8)+(size_t)sel*0xd0+0xc);
  for(uint32_t i=0;i<n;i++)if(*(uint32_t*)(nmh+8+(size_t)i*0x308)==type){gi=i;break;}}
 int (*mapidx)(void*)=(int(*)(void*))s->natives[NATIVE_NMH_MAP_INDEX];int mi=mapidx?mapidx(nmh):0;if(mi<0)mi=0;
 uint8_t *game=nmh+8+(size_t)gi*0x308;if(gametype)*gametype=*(int32_t*)game;
 return *(uint8_t**)(game+8+(size_t)mi*0x18);}
static int custom_load_source(uintptr_t base,int32_t *event,ProfileState *s){
 uint8_t *source=*(uint8_t**)((uint8_t*)event+0x98);if(!source||*(uint32_t*)(source+0x1c)!=0x4c414e31)return 0;
 if(*(uint32_t*)(base+0x17d603c)!=2){set_text(event,"");event[1]=0;return 1;}
 int32_t gt=0;uint8_t *map=custom_map(base,s,&gt);if(!map)return 1;
 switch(event[0]){
  case 4:
   if((uint32_t)event[0x14]==0x16094dd2u){
    void *(*vtex)(uintptr_t,const char*)=(void*(*)(uintptr_t,const char*))s->natives[NATIVE_VTEX_FIND];
    const char *name=*(const char**)(map+0x10);void *t=(vtex&&name)?vtex(base+0x1b31128,name):0;
    for(int k=3;k<=12;k++)event[k]=0;event[9]=-1;event[11]=-1;
    if(t){event[2]=2;*(void**)(event+4)=t;event[12]=0x3f800000;}else{event[2]=0x10;event[12]=0x40100000;}}
   break;
  case 2:{
   if(event[0x15]!=0)break;uint32_t tag=(uint32_t)event[1];
   if(tag==0x16094dd2u){
    if(*(uint32_t*)(map+8)){char *o=(char*)s+0x2c80,*end=o+250;o=fmt_append(o,end,text(base,s,*(uint32_t*)(map+4)));o=fmt_append(o,end,"\\");o=fmt_append(o,end,text(base,s,*(uint32_t*)(map+8)));*o=0;set_text(event,(char*)s+0x2c80);}
    else set_text(event,text(base,s,*(uint32_t*)(map+4)));}
   else if(tag==0xa42c91c5u){uint8_t *r=sym_table(s,0xf985c7b6);if(r&&gt>=0&&(uint32_t)gt<*(uint32_t*)r)set_text(event,text(base,s,*(uint32_t*)(*(uint8_t**)(r+8)+(size_t)gt*0x58)));}
   /* 0xacebf52d (ranked level line, online only) not ported */
   break;}
  case 11:{uint32_t q=(uint32_t)event[0x13];if(q==0x17809498u||q==0xbedebbb3u||q==0x7754e7afu||q==0x7bc431fbu)event[14]=0;break;}
 }
 return 1;}
/* OnButton 0xea1f40c5 (take the selected mission): PS3 Menu::OnButton -> f_34d328, then save.
 * PS4 still shows that dialog (0x64d57b) but dropped the handler. Not ported: the notification,
 * the start time (+0x1868, only used by timed missions) and the cost (PS3 f_5b764). */
static void mission_take(uintptr_t base,ProfileState *s){
 uint8_t *mo=mission_obj(base),*pr=mission_profile(base);if(!pr)return;
 uint8_t *set=mission_set(base,s,*(int32_t*)(mo+0x100c));if(!set)return;
 uint32_t count=*(uint32_t*)(set+8),row=*(uint32_t*)(mo+0x1010);uint8_t *list=*(uint8_t**)(set+0x18);
 if(!list||row>=count)return;uint8_t *m=list+(size_t)row*0x4c;
 uint8_t (*can)(uint8_t*,uint8_t*,int)=(uint8_t(*)(uint8_t*,uint8_t*,int))s->natives[NATIVE_MISSION_CAN];
 if(!can||!can(mo,m,0)||mission_done(pr,*(uint32_t*)m))return;
 *(uint32_t*)(pr+0x1864)=*(uint32_t*)m;*(uint32_t*)(pr+0xf4c)=0;}

/* Clan Tag (LIST_PROFILE row action 0xb69ea58b -> click registration 0x672c50, a RET stub on PS4).
 * PS3 f_288828 opens the system keyboard titled with text 0x639e0fa8 and stores the result in the
 * player profile (PS3 profile+0x878; the PS4 LIST_PROFILE source reads it as 4 chars at
 * profile+0xac0, profile = *(0x17dd610) + user*0x7b00, user = 0x17bf714 or 1 if 0x17bde75). The
 * PS4 system keyboard is the IME dialog the LAN profile name already uses; phase 3 marks the clan
 * tag dialog, polled from the party box every frame. */
static uint8_t *player_profile(uintptr_t base){
 uint8_t *pr=*(uint8_t**)(base+0x17dd610);if(!pr)return 0;
 uint32_t user=*(uint8_t*)(base+0x17bde75)?1u:*(uint32_t*)(base+0x17bf714);
 return user<4?pr+(size_t)user*0x7b00:0;
}
static void clan_tag_open(uintptr_t base,ProfileState *s){
 if(s->phase!=0||!s->init)return;
 uint8_t *pr=player_profile(base);if(!pr)return;
 const char *t=text(base,s,0x639e0fa8u);unsigned i=0;
 uint16_t *title=(uint16_t*)((uint8_t*)s+0x2d80);   /* 32 UTF-16 chars */
 for(;t[i]&&i<31;i++)title[i]=(uint8_t)t[i];
 title[i]=0;
 clear(s->text,sizeof(s->text));
 for(unsigned j=0;j<4&&pr[0xac0+j];j++)s->text[j]=pr[0xac0+j];
 ImeParam param;clear(&param,sizeof(param));
 param.user=1;param.max_length=4;param.buffer=s->text;param.title=title;
 if(((int(*)(const ImeParam*,const void*))s->init)(&param,0)<0){s->error=6;return;}
 s->phase=3;
}
static void clan_tag_poll(uintptr_t base,ProfileState *s){
 if(s->phase!=3)return;
 if(((int(*)(void))s->status)()!=2)return;
 uint32_t result=1;int r=((int(*)(uint32_t*))s->result)(&result);
 ((int(*)(void))s->term)();s->phase=0;
 if(r<0||result)return;
 uint8_t *pr=player_profile(base);if(!pr)return;
 /* PS3 shows '\\' in a clan tag as a space; the PS4 text renderer treats it as a line break. */
 for(unsigned j=0;j<4;j++){uint16_t c=s->text[j];if(c=='\\')c=' ';pr[0xac0+j]=(c>=0x20&&c<0x7f)?(uint8_t)c:0;if(!c){for(;j<4;j++)pr[0xac0+j]=0;break;}}
}
/* PS4 0x672450, the catch-all of the menu's click registrations (id 0, called by Menu::OnButton
 * 0x64ce90 for every action), is a RET stub; PS3 f_28dbf8 handles these actions there. Only the
 * in-menu, non-online cases are ported. event[0] = action id. */
static void catch_all(uintptr_t base,int32_t *event,ProfileState *s){
 {uint64_t *fl=(uint64_t*)((uint8_t*)s+0x2400);if((uint32_t)event[0]==0x3406420cu)fl[5]+=1;else fl[5]+=0x10000;}
 switch((uint32_t)event[0]){
  case 0x9db19163u:show(base,s,0xc98ee52a,0);break;   /* Records > Medals -> PAGE_MULTIPLAYER_MEDALS */
  case 0xe9bce205u:treasure_state(s)->row=0;show(base,s,0xda577fac,0);break;   /* Treasures -> PAGE_NET_TREASURE_SETS (PS3 also clears 0x1232a90 = set row) */
  case 0x420f5e96u:show(base,s,0x6281f21f,0);break;   /* Records -> PAGE_RECORDS */
  case 0x58dda3cdu:show(base,s,0x0c3a3716,0);break;   /* Unlocks -> PAGE_MULTIPLAYER_UNLOCKS */
  case 0xab58f9dau:show(base,s,0x53cbb5f2,0);break;   /* back to PAGE_MULTIPLAYER (PS3 also f_262418) */
  case 0xe4794ab9u:show(base,s,0xdfdb36fa,0);break;   /* -> PAGE_MISSIONS */
  case 0xea1f40c5u:mission_take(base,s);break;
  /* Party member popup (0x671d00): PS3 0x28dbf8 sets the party-locked flag (PS4 0x17c3570, read by
   * 0x671d00 for the Lock/Unlock label) and tells the session (f_3745c4, no LAN session yet). */
  case 0xccbf832eu:*(uint8_t*)(base+0x17c3570)=1;break;  /* Lock Party */
  case 0x0d0c91cdu:*(uint8_t*)(base+0x17c3570)=0;break;  /* Unlock Party */
  case 0x089143eau:show(base,s,0x83f24f04u,0);break;     /* View Stats -> stats page */          /* mission dialog: take it (PS3 OnButton -> f_34d328) */
 }
}
/* PS3 0x28dbf8 case 0xb85cb12a (Cinema), party of fewer than 2 (a LAN host alone): net menu mode 4,
 * deselect the net game (index -1, handled by PS4 0x720610), refresh and select film 0 (no film
 * manager on PS4: nothing to load), then PS3 f_26f924 shows PAGE_PROGRESS while a thread rescans
 * and continues to PAGE_MULTIPLAYER_CINEMA_LIST. With no scan to wait for, open the list directly. */
static void cinema(uintptr_t base,ProfileState*s){
 if(!s->natives[NATIVE_SELECT_NETGAME]){s->lobby_error=5;return;}
 *(uint32_t*)(base+0x17d603c)=4;
 ((void(*)(void*,int,void*,uint32_t))s->natives[NATIVE_SELECT_NETGAME])((void*)(base+0x17cb060),-1,0,0xc);
 if(!show(base,s,0x735c4dbe,0))s->lobby_error=6;
}
/* PS3 0x28dbf8 case 0xcd127c29 (Splitscreen): with no second user available (splitscreen manager
 * 0x125ef1c +0x24 clear) and splitscreen not active (f_383ff8), it shows dialog 0x7c31f4d0 with
 * title 0xc6467f2d. PS4 has no splitscreen manager or second-user selection ("Select Second User"
 * is absent), so that is the branch a single-user PS4 build takes. */
static void splitscreen(uintptr_t base,ProfileState*s){
 show_dialog(base,s,0x7c31f4d0u,0xc6467f2du,0,0,0,0);
}
/* PS3 0x28dbf8 case 0x7d593924: mode 2 (custom game), refresh NetMenuHost, select net game 0
 * with mode 0xc, select the first game type and open PAGE_LOBBY. The party broadcast (message
 * 0x110 to party members) is skipped: a LAN host starts alone. */
static void custom_game(uintptr_t base,ProfileState*s){
 if(!s->natives[NATIVE_NMH_REFRESH]||!s->natives[NATIVE_SELECT_NETGAME]||!s->natives[NATIVE_NMH_SELECT_TYPE]){s->lobby_error=1;return;}
 if(!*(uint64_t*)(base+0x17dd610)){s->lobby_error=2;return;}  /* PAGE_LOBBY enter dereferences it */
 *(uint32_t*)(base+0x17d603c)=2;
 uint8_t *nmh=(uint8_t*)(base+0x17d7b30);
 if(!*nmh)((void(*)(void*))s->natives[NATIVE_NMH_REFRESH])(nmh);
 if(!*nmh){s->lobby_error=3;return;}
 /* v4.60: start on net game 6 (team deathmatch on Sanctuary). Sanctuary is the one MP map whose lobby
  * stage (san-upper-multi-stage + sky) ships on PS4, so the lobby stage loader 0x743420 finds a level. */
 ((void(*)(void*,int,void*,uint32_t))s->natives[NATIVE_SELECT_NETGAME])((void*)(base+0x17cb060),6,0,0xc);
 ((void(*)(void*,int))s->natives[NATIVE_NMH_SELECT_TYPE])(nmh,0);
 s->lobby_error=0;
 if(!show(base,s,0xb99a3235,0))s->lobby_error=4;
}
static void begin_startup(uintptr_t base,ProfileState*s){
 if(!s->show_page||s->startup_phase||!s->profiles.selected||s->profiles.selected>s->profiles.count)return;
 register_lobby_click(base);
 s->startup_phase=1;s->startup_ticks=0;s->startup_error=0;
 if(!show(base,s,0x9e1863b1,0)){s->startup_phase=0;s->startup_error=1;}
}
static void startup_tick(uintptr_t base,ProfileState*s){
 if(!s->startup_phase||s->startup_phase>=3)return;
 uint32_t ui=*(uint32_t*)(base+0x17bd080+8);
 if(ui==7||ui==8)return;
 s->startup_ticks++;
 if(s->startup_phase==1){
  void **symbol=((void**(*)(uint32_t))s->lookup)(0xbd3bb3b9);
  if(!symbol||!*symbol){uint32_t tag=0x12;((void(*)(uint32_t,uint32_t*))s->loader)(0x4240ef2e,&tag);symbol=((void**(*)(uint32_t))s->lookup)(0xbd3bb3b9);}
  if(!symbol||!*symbol){s->startup_error=2;s->startup_phase=4;return;}
  if(!((int(*)(uintptr_t))s->pool_init)(base)){s->startup_error=3;s->startup_phase=4;return;}
  /* PS3 NetInit(0,1) stores its LAN argument in the net game object (PS3 0x122d654,
   * PS4 0x17d5c58+8); LAN-specific frontend paths test it. */
  *(uint8_t*)(base+0x17d5c60)=1;
  /* The PS3 NetInit worker (0x290ca0) also raises the net game's multiplayer-active byte (PS3
   * 0x122d655, PS4 0x17d5c58+9; cleared again at net shutdown 0x28cb80). Match setup (0x727440 from
   * net state 0x2c) reads the player slots only through it. */
  *(uint8_t*)(base+0x17d5c61)=1;
  /* Everything unlocked from the start (user request): raise the game's own debug switch
   * "Give all Store Items" (0x17b2a94, debug menu item built at 0x4bae0b; PS3 0x11ce9e4). The
   * unlock manager (0x7bba70, and 0x7bca00 through it) then reports every item owned, and the
   * booster (0x68830c/0x689088) and kickback (0x74ece0) levels return their maximum tier. */
  *(uint8_t*)(base+0x17b2a94)=1;
  /* PS3 Connecting (f_28d358) requests the background level "net" (net.pak: the MP menu icons, read
   * by 0x11b7600 into the UI material table once the level reaches state 8). PS4 lost the level
   * entry and the request; the overlay adds both the pak23 entry and the converted net.pak. Request
   * it as PS4 0x678d14 requests "multi-menu": {u64 hash, u32 sid, u16 0, u8 0, u8 flags 0x0a, u64 0}. */
  {void *(*find)(uintptr_t,uint32_t)=(void*(*)(uintptr_t,uint32_t))s->natives[NATIVE_LEVEL_FIND];
   uint64_t (*hash)(const char*)=(uint64_t(*)(const char*))s->natives[NATIVE_NAME_HASH];
   void (*request)(uintptr_t,void*)=(void(*)(uintptr_t,void*))s->natives[NATIVE_LEVEL_REQUEST];
   if(find&&hash&&request&&!find(base+0x1afcf10,0x4240ef2eu)){
    static const char net_name[]="net";
    uint8_t req[0x18];for(int k=0;k<0x18;k++)req[k]=0;
    *(uint64_t*)req=hash(net_name);*(uint32_t*)(req+8)=0x4240ef2eu;req[0xe]=0x0a;
    request(base+0x1afcf10,req);}}
  /* The local player's name buffer (0x17d5c58+0x3b8, the PSN online id on PS4) is what the profile
   * box (0x761ea0: "[%s] %s" with the clan tag) and other MP UI print. PS3 LAN play uses the
   * chosen LAN profile name instead of an online id, so put it there. */
  if(s->profiles.selected&&s->profiles.selected<=s->profiles.count){
   char *dst=(char*)(base+0x17d6010);const char *src=s->profiles.names[s->profiles.selected-1];
   unsigned n=0;for(;n<16&&src[n];n++)dst[n]=src[n];dst[n]=0;}
  /* Clan tags saved with '\\' (a space on PS3, a line break for the PS4 text renderer). */
  {uint8_t *pr=player_profile(base);if(pr)for(int k=0;k<4;k++)if(pr[0xac0+k]=='\\')pr[0xac0+k]=' ';}
  /* LAN room list read by the Find a Party source 0xd0 (0x76f2a0): +0x50 count, rooms of 0x38 from
   * +0x58 (address at +0, name at +0x12). Created with the LAN backend objects in NetInit. */
  /* kAllocNetwork (0x14) / kAllocNetworkProcess (0x15) are never registered on PS4 (their
   * descriptors were dropped from the allocator list feeding 0x18417b0). Match setup allocates
   * from 0x14 (0x6b16b0 -> 0xd9e990). Back them with a general heap: wrapped malloc (9) first. */
  {uintptr_t *alloc=(uintptr_t*)(base+0x18417b0);static const uint8_t pick[]={9,5,7,0x13};
   Probe *pr=(Probe*)((uint8_t*)s+1280);pr->alloc_lo=pr->alloc_hi=0;
   for(int i=0;i<64;i++)if(alloc[i]){if(i<32)pr->alloc_lo|=1u<<i;else pr->alloc_hi|=1u<<(i-32);}
   for(unsigned k=0;k<sizeof(pick);k++)if(alloc[pick[k]]){
    if(!alloc[0x14]){alloc[0x14]=alloc[pick[k]];pr->net_alias=pick[k];}
    if(!alloc[0x15])alloc[0x15]=alloc[pick[k]];
    break;}}
  /* Net clock pointer 0x17c3580 (never assigned on PS4): MP code reads its time at +0xc in 300 Hz
   * ticks. Engine clocks live at 0x17b4a90+0x48+k*0x40; clock 3 (0x17b4b98) is the one whose time
   * 0x17b4ba4 the net state machine 0x6a5fa0 already uses directly. */
  /* Own net clock at state+0x2000: engine clock 3's settings, time zeroed. The builder makes the
   * engine's per-frame clock pass (0x10ff75c) also run Clock::Update 0x1197090 on it. MP code times
   * states against it (LoadLevels 0x67fad0 leaves after 9001 ticks = 30 s), so it starts at match start. */
  {uint8_t *nc=(uint8_t*)s+0x2000;copy(nc,(void*)(base+0x17b4b98),0x40);
   *(uint64_t*)(nc+0xc)=0;*(uint32_t*)(nc+0x18)=0;*(uint16_t*)(nc+0x1c)=0;*(uint32_t*)(nc+0x20)=0;*(uint32_t*)(nc+0x24)=0;*(uint32_t*)(nc+0x28)=0;*(uint32_t*)(nc+0x2c)=0;
   *(uint8_t**)(base+0x17c3580)=nc;}
  /* DC symbol handle 0x17c3b40 (+0x20 value, +0x28 version, +0x2c id, +0x30 module) is created by
   * static init 0x689440 with id 0 on PS4. Its users (0x687d30, 29 calls) look up 0x38-byte entries
   * keyed at +4 by character id (0x7800d0(player,4)); the only net.bin table of that shape keyed by
   * character ids (profile default 0x6afcc877) is 0x916e03b0, the MP character list. */
  if(!*(uint32_t*)(base+0x17c3b40+0x2c)){*(uint32_t*)(base+0x17c3b40+0x2c)=0x916e03b0u;*(uint64_t*)(base+0x17c3b40+0x30)=0;*(int32_t*)(base+0x17c3b40+0x28)=-1;}
  if(!*(uintptr_t*)(base+0x17dd758))*(uint8_t**)(base+0x17dd758)=(uint8_t*)s+0x1000;
  s->startup_phase=2;return;
 }
 /* Keep the actual loader page for its resource work, then return through
  * the profile page before pushing the lobby so Back cannot loop into loading.
  */
 s->startup_phase=3;s->phase=0;
 if(!show(base,s,0x4608c1ad,1)||!show(base,s,0x53cbb5f2,0)){s->startup_error=4;s->startup_phase=4;}
}

static void finish(ProfileState *s){
 if(s->phase!=1)return;
 int status=((int(*)(void))s->status)();
 if(status!=2)return;
 uint32_t result=1;
 int r=((int(*)(uint32_t*))s->result)(&result);
 ((int(*)(void))s->term)();s->phase=0;
 if(r<0||result)return;
 ProfileFile next;copy(&next,&s->profiles,sizeof(next));
 char name[NAME_BYTES];if(!u3_profile_name(s->text,name)){s->error=2;return;}
 uint32_t index=0;
 for(;index<next.count;index++){
  unsigned j=0;while(j<NAME_BYTES&&name[j]==next.names[index][j]&&name[j])j++;
  if(j<NAME_BYTES&&name[j]==next.names[index][j])break;
 }
 if(index==next.count){if(next.count>=LIMIT){s->error=3;return;}copy(next.names[next.count++],name,NAME_BYTES);}
 next.selected=index+1;
 if(!save(s,&next)){s->error=4;return;}
 copy(&s->profiles,&next,sizeof(next));s->error=0;s->phase=2;s->startup_phase=0;
}
__attribute__((section(".text.entry")))
int u3_profile_event(uintptr_t base,int32_t *event,int kind,ProfileState *s){
 if(!s||!event)return 0;
 load(s);
 if(kind==8){int ok=prepare_lobby_menu(event);if(ok&&base)register_lobby_click(base);return ok;}
 if(kind==3){if(event[0]==6)startup_tick(base,s);return 1;}
 if(kind==9){start_game(base,s);return 1;}
 if(kind==11){clan_tag_open(base,s);return 1;}
 if(kind==12){if(s->show_page)catch_all(base,event,s);return 1;}
 if(kind==10){
  /* Match setup 0x72b570 -> emblem setup 0x66b1d0, which dereferences its module lookups without a
   * check; at match start the net module's script condition rejects the lookup. Run it only when all
   * three emblem tables resolve. */
  void *(*find)(uint32_t,int)=(void*(*)(uint32_t,int))s->natives[NATIVE_FIND_MODULE];
  void (*setup)(void)=(void(*)(void))s->natives[NATIVE_EMBLEM_SETUP];
  static const uint32_t ids[3]={0xc7f8567cu,0xe2311588u,0x03ffae77u};
  void (*init)(uintptr_t)=(void(*)(uintptr_t))s->natives[NATIVE_EMBLEM_INIT];
  /* 0x72b570 fills the emblem table (0x66b510) only after this call: on the first start the table
   * at 0x1b67e60 (ids at +0xc, stride 0x18, 4 entries) is still empty, so fill it first. */
  /* 0x66b510 only rebuilds when its dirty byte (0x17c3440+0) is set. */
  if(init&&!*(uint32_t*)(base+0x1b67e6c)){*(uint8_t*)(base+0x17c3440)=1;init(base+0x17c3440);}
  int ok=find&&setup&&*(uint32_t*)(base+0x1b67e6c);
  for(int i=0;ok&&i<3;i++)if(find(ids[i],1)==(void*)(base+0x1b3af40))ok=0;
  if(ok)setup();else{uint32_t *h=(uint32_t*)((uint8_t*)s+1896);h[0]++;}
  return 1;
 }
 if(kind==6){s->k6_calls++;s->k6_last=(uint32_t)event[0];if(event[0]==6)lobby_focus(base,s);return 1;}
 if(kind==2){
  if(lobby_buttons(base,event,s))return 1;
  if(cinema_source(base,event,s))return 1;
  if(tab_source(base,event,s))return 1;
  if(emblem_source(base,event,s))return 1;
  if(medals_source(base,event,s))return 1;
  if(time_limit_source(base,event,s))return 1;
  if(boosters_source(base,event,s))return 1;
  if(kickback_source(base,event,s))return 1;
  if(coop_weapon_source(base,event,s))return 1;
  if(coop_mods_source(base,event,s))return 1;
  if(missions_source(base,event,s))return 1;
  if(profile_box_source(base,event,s))return 1;
  if(custom_load_source(base,event,s))return 1;
  if(treasures_source(base,event,s))return 1;
  if(lobby_source(base,event,s)){
   /* Row-count queries of LIST_MAIN_MENU arrive every frame from the list update: a second,
    * known-periodic chance to apply the one-time lobby focus fix. */
   uint8_t *src=*(uint8_t**)((uint8_t*)event+0x98);
   if(event[0]==6&&src&&*(uint32_t*)(src+0x1c)==0x4c414eb3)lobby_focus(base,s);
   return 1;
  }
  uint32_t index=(uint32_t)event[1];
  switch(event[0]){
   case 2:if(index&&index<=s->profiles.count&&event[0x15]==0){*(const char**)((uint8_t*)event+0x40)=s->profiles.names[index-1];return 1;}return 0;
   case 5:event[1]=(int32_t)s->profiles.selected;return 1;
   case 6:event[1]=(int32_t)s->profiles.count;return 1;
   case 10:if(index<=s->profiles.count)s->profiles.selected=index;return 1;
   case 23:if(s->profiles.selected>s->profiles.count)s->profiles.selected=0;return 1;
   default:return 0;
  }
 }
 if(kind==1){
  if(event[0]==6){finish(s);if(s->phase==2)begin_startup(base,s);}
  else if((event[0]==2||event[0]==3)&&s->phase==1){((int(*)(void))s->term)();s->phase=0;}
  return 1;
 }
 if(kind!=0)return 0;
 if(active_page(base)==0x53cbb5f2){
  /* LIST_MAIN_MENU rows, ported from the PS3 catch-all menu event handler 0x28dbf8
   * (row events of table 0x11bf908; PS4 catch-all 0x672450 is a RET stub). */
  if((uint32_t)event[1]<6){s->reserved=(uint32_t)event[1]+1;
   uint32_t action=lan_rows[event[1]].action;
   void (*button)(void*,uint32_t,int)=(void(*)(void*,uint32_t,int))s->natives[NATIVE_MENU_BUTTON];
   switch(action){
    case 0x7d593924u:custom_game(base,s);break;   /* PS3 0x28dbf8 case: Custom Game */
    case 0x52f3d9f6u:if(s->show_page)show(base,s,0xbb3595bc,0);break; /* PS3 0x28dbf8: room browsers reset, LAN Parties page */
    case 0xc4acc827u:case 0xe7d7dbbbu:if(button)button((void*)(base+0x17bd080),action,0);break; /* kept in PS4 0x64ce90 */
    case 0xb85cb12au:cinema(base,s);break;       /* PS3 0x28dbf8 case: Cinema */
    case 0xcd127c29u:splitscreen(base,s);break;  /* PS3 0x28dbf8 case: Splitscreen */
   }
  }
  return 1;
 }
 /* The profile create/select actions below belong to PAGE_LAN_CHOOSE_PROFILE only. */
 if(active_page(base)!=0x4608c1ad)return 1;
 if(s->error==1)return 1;
 if(event[1]>0){
  if((uint32_t)event[1]<=s->profiles.count){
   ProfileFile next;copy(&next,&s->profiles,sizeof(next));next.selected=(uint32_t)event[1];
   if(save(s,&next)){copy(&s->profiles,&next,sizeof(next));s->phase=2;s->startup_phase=0;}else s->error=4;
  }
  return 1;
 }
 if(s->phase==1||s->profiles.count>=LIMIT)return 1;
 ImeParam param;clear(&param,sizeof(param));clear(s->text,sizeof(s->text));
 param.user=1;param.max_length=15;param.buffer=s->text;param.title=prompt;
 int r=((int(*)(const ImeParam*,const void*))s->init)(&param,0);
 if(r<0){s->error=5;return 1;}
 s->phase=1;finish(s);
 return 1;
}
#endif