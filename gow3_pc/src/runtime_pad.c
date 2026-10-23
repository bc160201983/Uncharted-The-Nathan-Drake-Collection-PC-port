/* libScePad on SDL3 gamepads, with keyboard and mouse. SDL events are pumped by the window
 * thread (gpu/shim/window.cpp); here state is only sampled.
 *
 * Default keyboard and mouse layout (GOW3_KEY_MAP / GOW3_MOUSE_MAP change it, see
 * apply_button_map; the launcher's Controls tab writes them):
 *   WASD left stick, mouse (or the arrow keys) right stick, right mouse L2 (aim), left mouse
 *   R2 (fire), Space Cross (jump), Left Ctrl Circle (cover, roll), F Square (melee),
 *   R Triangle (reload, pick up), Q / mouse X1 L1, E / mouse X2 R1, Left Shift L3,
 *   V / middle mouse R3, Escape or Enter Options, Tab left touchpad, Backspace right touchpad,
 *   1 2 3 4 d-pad left right up down, mouse wheel d-pad right (up) and left (down).
 * GOW3_KEYBOARD=0 turns the keyboard off. The controls (maps, mouse on, its sensitivity and
 * vertical invert, button icons) come from the settings (gow3gpu.h Gow3Controls: the in-game
 * menu's Controls tab and the launcher's; GOW3_MOUSE=0, GOW3_MOUSE_SENS, GOW3_MOUSE_INVERT=1,
 * GOW3_PROMPTS and the maps override them at start) and apply live. Button icons in the games'
 * text follow the device used last (keys and mouse buttons as bound, or the controller's
 * buttons: the game's own for PlayStation controllers, Xbox-style for others;
 * gpu/shim/gow3_prompts.cpp), or the fixed choice. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/gow3gpu.h"
#include "runtime_gamesettings.h"
#include "runtime_uc2mp_diag.h"
#include "runtime_uc2mp_tick.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <SDL3/SDL.h>
#include <sys/stat.h>

#define ERR_INVALID_ARG ((int32_t)0x80920001)
#define ERR_INVALID_HANDLE ((int32_t)0x80920003)
#define ERR_ALREADY_OPENED ((int32_t)0x80920004)
#define ERR_NOT_INITIALIZED ((int32_t)0x80920005)
#define PAD_HANDLE 1

enum {
    BTN_L3=0x2, BTN_R3=0x4, BTN_OPTIONS=0x8, BTN_UP=0x10, BTN_RIGHT=0x20, BTN_DOWN=0x40, BTN_LEFT=0x80,
    BTN_L2=0x100, BTN_R2=0x200, BTN_L1=0x400, BTN_R1=0x800, BTN_TRIANGLE=0x1000, BTN_CIRCLE=0x2000,
    BTN_CROSS=0x4000, BTN_SQUARE=0x8000, BTN_TOUCHPAD=0x100000,
    /* Host-only sources, never shown to the game: a click on one side of the touchpad from a
     * button or key without a touch surface (play_click turns them into a touch and a click). */
    CLICK_LEFT=0x40000000, CLICK_RIGHT=0x80000000,
};
typedef struct { uint16_t x, y; uint8_t id, reserve[3]; } PadTouch;
typedef struct {
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y;
    uint8_t l2, r2, analog_padding[2];
    float orientation[4], acceleration[3], angular_velocity[3];
    uint8_t touch_count, touch_reserve[3];
    uint32_t touch_held_time;
    PadTouch touches[2];
    uint8_t connected, pad0[3];
    uint64_t timestamp;
    uint8_t extension[16];
    uint8_t connected_count, reserve[2], unique_length, unique[12];
} PadData;
typedef struct {
    float pixel_density; uint16_t resolution_x, resolution_y;
    uint8_t dead_zone_left, dead_zone_right, connection_type, connected_count;
    uint8_t connected, pad[3];
    int32_t device_class;
    uint8_t reserve[8];
} ControllerInfo;
_Static_assert(sizeof(PadData)==120,"OrbisPadData layout");
_Static_assert(sizeof(PadTouch)==8,"OrbisPadTouch layout");
_Static_assert(__builtin_offsetof(PadData,touches)==60,"OrbisPadData touch offset");
_Static_assert(__builtin_offsetof(PadData,timestamp)==80,"OrbisPadData timestamp offset");
_Static_assert(sizeof(ControllerInfo)==28,"OrbisPadControllerInformation layout");

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int initialized, opened, sdl_ready;
static SDL_Gamepad *gamepad;
static size_t reads;
static uint8_t connected_count;
static PadData last_buffered;
static uint64_t last_buffered_time;
static int have_buffered;

static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u; }
static uint8_t axis(int16_t v) { int x=(v+32768)>>8; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static uint8_t trigger(int16_t v) { int x=v>>7; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static uint16_t touch_axis(float v, int max) {
    return (uint16_t)(v<=0.0f ? 0 : v>=1.0f ? max : (int)(v*max+0.5f));
}

/* A click without a touch surface (Back/View, Tab, Backspace, the pad file) is played like a
 * finger: the touch is reported CLICK_LEAD_US before the button, the button stays at least
 * CLICK_HOLD_US, and the touch lingers CLICK_TAIL_US after the release. God of War III opens
 * photo mode on the release of a left click with the finger still down, and ignores a touch
 * that comes and goes with the button. The side is chosen when the click starts and kept: the
 * right source with L1 held is a left click (photo mode on pads without a touchpad). */
enum { CLICK_LEAD_US=50000, CLICK_HOLD_US=50000, CLICK_TAIL_US=100000 };
static struct { int side; uint64_t pressed, released; } click={-1,0,0};
static void play_click(PadData *d) {
    const uint64_t now=d->timestamp;
    uint32_t sources=d->buttons & (CLICK_LEFT|CLICK_RIGHT);
    d->buttons&=~(CLICK_LEFT|CLICK_RIGHT);
    if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) { /* a replay, or a pad without fingers */
        d->buttons&=~BTN_TOUCHPAD;
        sources|=CLICK_LEFT;
    }
    const int side=(sources & CLICK_LEFT) ? 0 : (sources & CLICK_RIGHT) ? !(d->buttons & BTN_L1) : -1;
    if (side>=0 && (click.side<0 || click.released)) { click.side=side; click.pressed=now; click.released=0; }
    else if (side<0 && click.side>=0 && !click.released) click.released=now;
    if (click.side<0) return;
    const uint64_t down=click.pressed+CLICK_LEAD_US;
    uint64_t up=down+CLICK_HOLD_US;
    if (!click.released) up=UINT64_MAX;
    else if (click.released>up) up=click.released;
    if (now>=up && now-up>=CLICK_TAIL_US) { click.side=-1; return; }
    if (d->touch_count<2) {
        d->touches[d->touch_count]=(PadTouch){.x=click.side ? 1440 : 480,.y=471,.id=d->touch_count};
        ++d->touch_count;
    }
    if (now>=down && now<up) d->buttons|=BTN_TOUCHPAD;
}

static void apply_light_bar(SDL_Gamepad *g);
/* GOW3_PAD_INDEX=<n> (gow3 25 Oct 2026, local co-op: two game windows on one PC, one controller
 * each): this game reads only the n-th controller SDL lists (1 = the first), none while fewer are
 * connected; window.cpp then lets controller input in without the focus. Unset: the first one. */
static int pad_index(void) {
    static int index=-1;
    if (index<0) { const char *e=getenv("GOW3_PAD_INDEX"); index=e && atoi(e)>0 ? atoi(e) : 0; }
    return index;
}
/* Opens the first gamepad SDL knows about (or the GOW3_PAD_INDEX-th); called under lock. */
static SDL_Gamepad *current_gamepad(void) {
    if (!sdl_ready) sdl_ready = SDL_WasInit(SDL_INIT_GAMEPAD) ? 1 : SDL_InitSubSystem(SDL_INIT_GAMEPAD) ? 1 : -1;
    if (sdl_ready<0) return NULL;
    if (gamepad && !SDL_GamepadConnected(gamepad)) { SDL_CloseGamepad(gamepad); gamepad=NULL; }
    if (!gamepad) {
        int count=0;
        SDL_JoystickID *ids=SDL_GetGamepads(&count);
        const int want=pad_index() ? pad_index()-1 : 0;
        if (ids && count>want) {
            gamepad=SDL_OpenGamepad(ids[want]);
            if (gamepad) {
                ++connected_count; printf("Runtime: gamepad connected: %s%s\n",SDL_GetGamepadName(gamepad),
                                          pad_index() ? " (GOW3_PAD_INDEX)" : "");
                apply_light_bar(gamepad);
            }
        } else if (pad_index()) {
            static int told=-1;
            if (told!=count) { told=count; printf("Runtime: GOW3_PAD_INDEX=%d: %d controller(s) connected, waiting\n",pad_index(),count); fflush(stdout); }
        }
        SDL_free(ids);
    }
    return gamepad;
}
/* Button sources: a host button or key and the PS4 button it presses. The defaults below can be
 * changed per PS4 button with GOW3_PAD_MAP (gamepad) and GOW3_KEY_MAP (keyboard), e.g.
 * GOW3_KEY_MAP="cross=Q,square=Left Shift" or GOW3_PAD_MAP="cross=b,circle=a": comma-separated
 * <ps4 button>=<SDL key or gamepad button name>. A mapped PS4 button loses its default sources.
 * touchpad_left (alias touchpad) and touchpad_right are the two sides of the touchpad click:
 * photo mode and the pause menu. Back/View (Share on a DualSense) is the right click, and with
 * L1 held the left one. Sticks, triggers, the real touchpad click and the overlay shortcuts are
 * not remapped. */
static const struct { const char *name; uint32_t ps; } ps_names[]={
    {"cross",BTN_CROSS}, {"circle",BTN_CIRCLE}, {"square",BTN_SQUARE}, {"triangle",BTN_TRIANGLE},
    {"l1",BTN_L1}, {"r1",BTN_R1}, {"l2",BTN_L2}, {"r2",BTN_R2}, {"l3",BTN_L3}, {"r3",BTN_R3},
    {"options",BTN_OPTIONS}, {"touchpad",CLICK_LEFT}, {"touchpad_left",CLICK_LEFT}, {"touchpad_right",CLICK_RIGHT},
    {"up",BTN_UP}, {"down",BTN_DOWN}, {"left",BTN_LEFT}, {"right",BTN_RIGHT},
};
typedef struct { int source; uint32_t ps; } ButtonSource;
typedef struct { ButtonSource entries[48]; size_t count; } ButtonMap;
static const ButtonMap default_pad_map={{
    {SDL_GAMEPAD_BUTTON_SOUTH,BTN_CROSS}, {SDL_GAMEPAD_BUTTON_EAST,BTN_CIRCLE},
    {SDL_GAMEPAD_BUTTON_WEST,BTN_SQUARE}, {SDL_GAMEPAD_BUTTON_NORTH,BTN_TRIANGLE},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,BTN_L1}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,BTN_R1},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK,BTN_L3}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK,BTN_R3},
    {SDL_GAMEPAD_BUTTON_START,BTN_OPTIONS}, {SDL_GAMEPAD_BUTTON_BACK,CLICK_RIGHT},
    {SDL_GAMEPAD_BUTTON_TOUCHPAD,BTN_TOUCHPAD},
    {SDL_GAMEPAD_BUTTON_DPAD_UP,BTN_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN,BTN_DOWN},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT,BTN_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT,BTN_RIGHT},
},15};
static const ButtonMap default_key_map={{
    {SDL_SCANCODE_SPACE,BTN_CROSS}, {SDL_SCANCODE_LCTRL,BTN_CIRCLE}, {SDL_SCANCODE_F,BTN_SQUARE},
    {SDL_SCANCODE_R,BTN_TRIANGLE}, {SDL_SCANCODE_Q,BTN_L1}, {SDL_SCANCODE_E,BTN_R1},
    {SDL_SCANCODE_LSHIFT,BTN_L3}, {SDL_SCANCODE_V,BTN_R3},
    {SDL_SCANCODE_ESCAPE,BTN_OPTIONS}, {SDL_SCANCODE_RETURN,BTN_OPTIONS},
    {SDL_SCANCODE_TAB,CLICK_LEFT}, {SDL_SCANCODE_BACKSPACE,CLICK_RIGHT},
    {SDL_SCANCODE_1,BTN_LEFT}, {SDL_SCANCODE_2,BTN_RIGHT}, {SDL_SCANCODE_3,BTN_UP}, {SDL_SCANCODE_4,BTN_DOWN},
},16};
/* Mouse sources (bits of gow3gpu_mouse_take): left, middle, right, X1, X2, wheel up, wheel down. */
enum { MOUSE_LEFT=1, MOUSE_MIDDLE, MOUSE_RIGHT, MOUSE_X1, MOUSE_X2, MOUSE_WHEEL_UP, MOUSE_WHEEL_DOWN };
static const char *const mouse_names[]={"left","middle","right","x1","x2","wheel_up","wheel_down"};
static const ButtonMap default_mouse_map={{
    {MOUSE_RIGHT,BTN_L2}, {MOUSE_LEFT,BTN_R2}, {MOUSE_MIDDLE,BTN_R3}, {MOUSE_X1,BTN_L1}, {MOUSE_X2,BTN_R1},
    {MOUSE_WHEEL_UP,BTN_RIGHT}, {MOUSE_WHEEL_DOWN,BTN_LEFT},
},7};
/* Keys that move a stick: ls_up ls_down ls_left ls_right (left stick), rs_* (right stick). */
enum { LS_UP, LS_DOWN, LS_LEFT, LS_RIGHT, RS_UP, RS_DOWN, RS_LEFT, RS_RIGHT, STICK_KEYS };
static const char *const stick_key_names[STICK_KEYS]={"ls_up","ls_down","ls_left","ls_right","rs_up","rs_down","rs_left","rs_right"};
typedef struct { struct { int source, direction; } entries[32]; size_t count; } StickKeyMap;
static const StickKeyMap default_stick_keys={{
    {SDL_SCANCODE_W,LS_UP}, {SDL_SCANCODE_S,LS_DOWN}, {SDL_SCANCODE_A,LS_LEFT}, {SDL_SCANCODE_D,LS_RIGHT},
    {SDL_SCANCODE_UP,RS_UP}, {SDL_SCANCODE_DOWN,RS_DOWN}, {SDL_SCANCODE_LEFT,RS_LEFT}, {SDL_SCANCODE_RIGHT,RS_RIGHT},
},8};
/* The maps in use: the defaults with the settings' maps applied (sync_controls). */
static ButtonMap pad_map, key_map, mouse_map;
static StickKeyMap stick_keys;

static char *trim(char *text) {
    while (*text==' ' || *text=='\t') ++text;
    char *end=text+strlen(text);
    while (end>text && (end[-1]==' ' || end[-1]=='\t' || end[-1]=='\r' || end[-1]=='\n')) *--end=0;
    return text;
}

/* Applies a GOW3_PAD_MAP / GOW3_KEY_MAP / GOW3_MOUSE_MAP value; returns how many entries were
 * used. The first entry for a PS4 button (or stick key direction) replaces its defaults, further
 * entries add sources, and an empty name leaves it unbound ("r3="). Bad entries are logged and
 * skipped, so their button keeps its default. */
enum { MAP_PAD, MAP_KEYBOARD, MAP_MOUSE };
static int map_source(int kind, const char *name) {
    if (kind==MAP_KEYBOARD) { int code=(int)SDL_GetScancodeFromName(name); return code>(int)SDL_SCANCODE_UNKNOWN ? code : -1; }
    if (kind==MAP_MOUSE) {
        for (size_t i=0;i<sizeof(mouse_names)/sizeof(*mouse_names);++i) if (!SDL_strcasecmp(name,mouse_names[i])) return (int)i+1;
        return -1;
    }
    int button=(int)SDL_GetGamepadButtonFromString(name);
    return button>(int)SDL_GAMEPAD_BUTTON_INVALID ? button : -1;
}
static int apply_button_map(ButtonMap *map, StickKeyMap *sticks, const char *spec, int kind) {
    if (!spec || !*spec) return 0;
    char copy[2048];
    snprintf(copy,sizeof(copy),"%s",spec);
    int used=0;
    uint32_t replaced=0, sticks_replaced=0;
    char *entry=copy;
    while (entry) {
        char *next=strchr(entry,',');
        if (next) *next++=0;
        char *eq=strchr(entry,'=');
        if (!eq) {
            if (*trim(entry)) printf("Runtime: pad map: ignored '%s' (expected button=name)\n",trim(entry));
            entry=next;
            continue;
        }
        *eq=0;
        const char *button=trim(entry), *name=trim(eq+1);
        entry=next;
        int direction=-1;
        if (kind==MAP_KEYBOARD)
            for (int i=0;i<STICK_KEYS;++i) if (!SDL_strcasecmp(button,stick_key_names[i])) direction=i;
        uint32_t ps=0;
        for (size_t i=0;i<sizeof(ps_names)/sizeof(*ps_names);++i) if (!SDL_strcasecmp(button,ps_names[i].name)) ps=ps_names[i].ps;
        const int source=*name ? map_source(kind,name) : -1;
        if ((!ps && direction<0) || (*name && source<0)) {
            printf("Runtime: pad map: ignored '%s=%s'\n",button,name);
            continue;
        }
        if (direction>=0) {
            if (!(sticks_replaced & (1u<<direction))) {
                sticks_replaced|=1u<<direction;
                size_t kept=0;
                for (size_t i=0;i<sticks->count;++i)
                    if (sticks->entries[i].direction!=direction) sticks->entries[kept++]=sticks->entries[i];
                sticks->count=kept;
            }
            if (source>=0 && sticks->count<sizeof(sticks->entries)/sizeof(*sticks->entries)) {
                sticks->entries[sticks->count].source=source;
                sticks->entries[sticks->count++].direction=direction;
            }
            ++used;
            continue;
        }
        if (!(replaced & ps)) {
            replaced|=ps;
            size_t kept=0;
            for (size_t i=0;i<map->count;++i) if (map->entries[i].ps!=ps) map->entries[kept++]=map->entries[i];
            map->count=kept;
        }
        if (source>=0 && map->count<sizeof(map->entries)/sizeof(*map->entries)) map->entries[map->count++]=(ButtonSource){source,ps};
        ++used;
    }
    return used;
}

/* A source's name as the maps take it (SDL key and gamepad button names, mouse_names). */
static const char *source_name(int kind, int source) {
    if (kind==MAP_KEYBOARD) return SDL_GetScancodeName((SDL_Scancode)source);
    if (kind==MAP_MOUSE) return source>=1 && source<=(int)(sizeof(mouse_names)/sizeof(*mouse_names)) ? mouse_names[source-1] : "";
    const char *name=SDL_GetGamepadStringForButton((SDL_GamepadButton)source);
    return name ? name : "";
}
/* A complete map: every PS4 button (and for the keyboard every stick direction) with its
 * sources, "button=" for none, so applied to the defaults it gives exactly this map. */
static void describe_map(const ButtonMap *map, const StickKeyMap *sticks, int kind, char *out, size_t size) {
    static const struct { const char *name; uint32_t ps; } buttons[]={
        {"cross",BTN_CROSS}, {"circle",BTN_CIRCLE}, {"square",BTN_SQUARE}, {"triangle",BTN_TRIANGLE},
        {"l1",BTN_L1}, {"r1",BTN_R1}, {"l2",BTN_L2}, {"r2",BTN_R2}, {"l3",BTN_L3}, {"r3",BTN_R3},
        {"options",BTN_OPTIONS}, {"touchpad_left",CLICK_LEFT}, {"touchpad_right",CLICK_RIGHT},
        {"up",BTN_UP}, {"down",BTN_DOWN}, {"left",BTN_LEFT}, {"right",BTN_RIGHT},
    };
    size_t used=0;
    out[0]=0;
#define DESCRIBE(name_, source_name_) do { \
        const int n_=snprintf(out+used,size-used,"%s%s=%s",used ? "," : "",(name_),(source_name_)); \
        if (n_>0 && (size_t)n_<size-used) used+=(size_t)n_; \
    } while (0)
    if (kind==MAP_KEYBOARD)
        for (int d=0;d<STICK_KEYS;++d) {
            int listed=0;
            for (size_t i=0;i<sticks->count;++i)
                if (sticks->entries[i].direction==d) { DESCRIBE(stick_key_names[d],source_name(kind,sticks->entries[i].source)); listed=1; }
            if (!listed) DESCRIBE(stick_key_names[d],"");
        }
    for (size_t b=0;b<sizeof(buttons)/sizeof(*buttons);++b) {
        int listed=0;
        for (size_t i=0;i<map->count;++i)
            if (map->entries[i].ps==buttons[b].ps) { DESCRIBE(buttons[b].name,source_name(kind,map->entries[i].source)); listed=1; }
        if (!listed) DESCRIBE(buttons[b].name,"");
    }
#undef DESCRIBE
}
/* For the in-game menu (any thread): the complete maps the given ones give on the defaults. */
void runtime_pad_effective_maps(const char *pad_spec, const char *key_spec, const char *mouse_spec,
                                char *pad_out, char *key_out, char *mouse_out, size_t size) {
    ButtonMap pad=default_pad_map, keys=default_key_map, mouse=default_mouse_map;
    StickKeyMap sticks=default_stick_keys;
    apply_button_map(&pad,&sticks,pad_spec,MAP_PAD);
    apply_button_map(&keys,&sticks,key_spec,MAP_KEYBOARD);
    apply_button_map(&mouse,&sticks,mouse_spec,MAP_MOUSE);
    describe_map(&pad,&sticks,MAP_PAD,pad_out,size);
    describe_map(&keys,&sticks,MAP_KEYBOARD,key_out,size);
    describe_map(&mouse,&sticks,MAP_MOUSE,mouse_out,size);
}

/* The settings' controls as the pad last applied them (sync_controls). */
static Gow3Controls controls={.mouse=1,.prompts=-1,.mouse_sens=1.0f,.stick_speed=1.0f};

static int keyboard_enabled(void) {
    static int enabled=-1;
    if (enabled<0) { const char *env=getenv("GOW3_KEYBOARD"); enabled=!(env && env[0]=='0'); }
    return enabled;
}
static int mouse_enabled(void) { return controls.mouse; }

/* Button prompts (gpu/shim/gow3_prompts.cpp): per PS4 button the label of the key or the mouse
 * source bound to it (L2/R2 show the mouse first, the rest the keyboard), and which device the
 * player used last: the games then show key caps and mice instead of the controller's buttons. */
static void key_label(int scancode, char *out, size_t size) {
    static const struct { const char *sdl, *label; } names[]={
        {"Space","SPACE"}, {"Left Ctrl","CTRL"}, {"Right Ctrl","CTRL"}, {"Left Shift","SHIFT"},
        {"Right Shift","SHIFT"}, {"Left Alt","ALT"}, {"Right Alt","ALT"}, {"Return","ENTER"},
        {"Keypad Enter","ENTER"}, {"Escape","ESC"}, {"Backspace","BKSP"}, {"Tab","TAB"},
        {"CapsLock","CAPS"}, {"Delete","DEL"}, {"Insert","INS"}, {"Home","HOME"}, {"End","END"},
        {"PageUp","PGUP"}, {"PageDown","PGDN"}, {"Up","\xe2\x86\x91"}, {"Down","\xe2\x86\x93"},
        {"Left","\xe2\x86\x90"}, {"Right","\xe2\x86\x92"},
    };
    const char *name=SDL_GetScancodeName((SDL_Scancode)scancode);
    if (!name) name="";
    for (size_t i=0;i<sizeof(names)/sizeof(*names);++i)
        if (!strcmp(name,names[i].sdl)) { snprintf(out,size,"%s",names[i].label); return; }
    if (!strncmp(name,"Keypad ",7)) snprintf(out,size,"NUM%s",name+7);
    else snprintf(out,size,"%s",name);
    for (char *c=out;*c;++c) *c=(char)toupper((unsigned char)*c);
    if (strlen(out)>5) out[5]=0;
}
/* Key and mouse icons for the devices that are on (GOW3_PROMPTS_LABELS=1: also for those off,
 * for scripted tests that keep the real keyboard and mouse off). */
static int prompt_labels_forced(void) {
    const char *env=getenv("GOW3_PROMPTS_LABELS");
    return env && env[0]=='1';
}
static int prompt_keyboard(void) { return keyboard_enabled() || prompt_labels_forced(); }
static int prompt_mouse(void) { return mouse_enabled() || prompt_labels_forced(); }
static void button_prompt(uint32_t ps, int prefer_mouse, char *out, size_t size) {
    static const char *const mouse_prompts[]={"@LMB","@MMB","@RMB","@X1","@X2","@WHEELUP","@WHEELDOWN"};
    int key=-1; const char *mouse=NULL;
    if (prompt_keyboard())
        for (size_t i=0;i<key_map.count && key<0;++i) if (key_map.entries[i].ps==ps) key=key_map.entries[i].source;
    if (prompt_mouse())
        for (size_t i=0;i<mouse_map.count && !mouse;++i) if (mouse_map.entries[i].ps==ps) mouse=mouse_prompts[mouse_map.entries[i].source-1];
    out[0]=0;
    if (mouse && (prefer_mouse || key<0)) snprintf(out,size,"%s",mouse);
    else if (key>=0) key_label(key,out,size);
}
static void stick_prompts(const int directions[4], char (*out)[16]) {
    for (int d=0;d<4;++d) {
        out[d][0]=0;
        if (!prompt_keyboard()) continue;
        for (size_t i=0;i<stick_keys.count;++i)
            if (stick_keys.entries[i].direction==directions[d]) { key_label(stick_keys.entries[i].source,out[d],16); break; }
    }
}
static void publish_prompts(void) {
    static const uint32_t buttons[GOW3_PROMPT_RIGHT+1]={BTN_CROSS,BTN_CIRCLE,BTN_SQUARE,BTN_TRIANGLE,
        BTN_L1,BTN_R1,BTN_L2,BTN_R2,BTN_L3,BTN_R3,BTN_OPTIONS,CLICK_LEFT,BTN_UP,BTN_DOWN,BTN_LEFT,BTN_RIGHT};
    static const int ls[4]={LS_UP,LS_LEFT,LS_DOWN,LS_RIGHT}, rs[4]={RS_UP,RS_LEFT,RS_DOWN,RS_RIGHT};
    char text[GOW3_PROMPT_COUNT][16];
    const char *labels[GOW3_PROMPT_COUNT];
    for (int i=0;i<=GOW3_PROMPT_RIGHT;++i)
        button_prompt(buttons[i],buttons[i]==BTN_L2 || buttons[i]==BTN_R2,text[i],sizeof(text[i]));
    stick_prompts(ls,&text[GOW3_PROMPT_LS_UP]);
    stick_prompts(rs,&text[GOW3_PROMPT_RS_UP]);
    if (prompt_mouse()) snprintf(text[GOW3_PROMPT_RS_UP],sizeof(text[0]),"@MOUSE"); /* the mouse moves the camera */
    for (int i=0;i<GOW3_PROMPT_COUNT;++i) labels[i]=text[i];
    gow3gpu_prompts_set_labels(0,labels,GOW3_PROMPT_COUNT);
}
/* The button icons setting: -1 auto (the device used last), 1 keyboard, 0 PlayStation, 2 Xbox,
 * 3 the connected controller's. */
static int prompts_fixed(void) { return controls.prompts; }
/* The controller's icons: 0 the game's (PlayStation controllers, or none), 2 drawn from the
 * labels below (Xbox-style; Nintendo controllers get their own letters). */
static int pad_prompt_style;
static const char *pad_prompt(SDL_Gamepad *g, int xbox_letters, int button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: case SDL_GAMEPAD_BUTTON_EAST:
    case SDL_GAMEPAD_BUTTON_WEST: case SDL_GAMEPAD_BUTTON_NORTH:
        switch (g && !xbox_letters ? SDL_GetGamepadButtonLabel(g,(SDL_GamepadButton)button)
                                   : SDL_GetGamepadButtonLabelForType(SDL_GAMEPAD_TYPE_XBOXONE,(SDL_GamepadButton)button)) {
        case SDL_GAMEPAD_BUTTON_LABEL_A: return "#A";
        case SDL_GAMEPAD_BUTTON_LABEL_B: return "#B";
        case SDL_GAMEPAD_BUTTON_LABEL_X: return "#X";
        case SDL_GAMEPAD_BUTTON_LABEL_Y: return "#Y";
        default: return "";
        }
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "#LB";
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "#RB";
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "#LS";
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "#RS";
    case SDL_GAMEPAD_BUTTON_START: return "#MENU";
    case SDL_GAMEPAD_BUTTON_BACK: return "#VIEW";
    default: return ""; /* the d-pad, touchpad and sticks keep the game's pictures */
    }
}
/* When the controller or the controls change: its style and its labels (from the pad map). */
static int pad_prompts_stale=1;
static void publish_pad_prompts(SDL_Gamepad *g) {
    static int published; static SDL_Gamepad *last; static int last_count;
    if (published && !pad_prompts_stale && g==last && connected_count==last_count) return;
    pad_prompts_stale=0;
    published=1; last=g; last_count=connected_count;
    const SDL_GamepadType type=g ? SDL_GetGamepadType(g) : SDL_GAMEPAD_TYPE_UNKNOWN;
    const int playstation=type==SDL_GAMEPAD_TYPE_PS3 || type==SDL_GAMEPAD_TYPE_PS4 || type==SDL_GAMEPAD_TYPE_PS5;
    pad_prompt_style=g && !playstation ? 2 : 0;
    const int xbox_letters=prompts_fixed()==2 || playstation || !g;
    static const uint32_t buttons[GOW3_PROMPT_RIGHT+1]={BTN_CROSS,BTN_CIRCLE,BTN_SQUARE,BTN_TRIANGLE,
        BTN_L1,BTN_R1,BTN_L2,BTN_R2,BTN_L3,BTN_R3,BTN_OPTIONS,CLICK_LEFT,BTN_UP,BTN_DOWN,BTN_LEFT,BTN_RIGHT};
    const char *labels[GOW3_PROMPT_COUNT];
    for (int i=0;i<GOW3_PROMPT_COUNT;++i) labels[i]="";
    for (int i=0;i<=GOW3_PROMPT_RIGHT;++i)
        for (size_t j=0;j<pad_map.count;++j)
            if (pad_map.entries[j].ps==buttons[i]) { labels[i]=pad_prompt(g,xbox_letters,pad_map.entries[j].source); break; }
    if (!*labels[GOW3_PROMPT_L2]) labels[GOW3_PROMPT_L2]="#LT"; /* the triggers (not remapped) */
    if (!*labels[GOW3_PROMPT_R2]) labels[GOW3_PROMPT_R2]="#RT";
    gow3gpu_prompts_set_labels(1,labels,GOW3_PROMPT_COUNT);
}
/* Devices used in the current sample (sample_host), for update_prompt_device. */
static struct { int keyboard, gamepad, connected; } prompt_activity;
/* keyboard_used / gamepad_used: input from that device in this sample. Device: 0 the game's
 * PlayStation icons, 1 keyboard and mouse, 2 the drawn controller icons. */
static void update_prompt_device(int keyboard_used, int gamepad_used, int gamepad_connected) {
    static int current=-1, keyboard=-1;
    const int fixed=prompts_fixed();
    if (gamepad_used) keyboard=0;
    else if (keyboard_used) keyboard=1;
    else if (keyboard<0) keyboard=!gamepad_connected;
    const int next=fixed==1 ? 1 : fixed==0 ? 0 : fixed==2 ? 2 : fixed==3 ? pad_prompt_style
                  : keyboard ? 1 : pad_prompt_style;
    if (next!=current) { current=next; gow3gpu_prompts_set_device(next); }
}

/* The settings' controls, again whenever they change (the in-game menu applies them live). */
static void sync_controls(void) {
    static unsigned applied=~0u;
    const unsigned generation=gow3gpu_controls_generation();
    if (generation==applied) return;
    applied=generation;
    gow3gpu_controls_get(&controls);
    pad_map=default_pad_map; key_map=default_key_map; mouse_map=default_mouse_map; stick_keys=default_stick_keys;
    const int pad=apply_button_map(&pad_map,&stick_keys,controls.pad_map,MAP_PAD);
    const int keys=apply_button_map(&key_map,&stick_keys,controls.key_map,MAP_KEYBOARD);
    const int mouse=apply_button_map(&mouse_map,&stick_keys,controls.mouse_map,MAP_MOUSE);
    if (pad || keys || mouse)
        printf("Runtime: pad map: %d gamepad, %d keyboard and %d mouse entries\n",pad,keys,mouse);
    printf("Runtime: controls: mouse %s (sensitivity %.2f%s), controller camera speed %.2f, button icons %d\n",
           controls.mouse ? "on" : "off",controls.mouse_sens,controls.mouse_invert ? ", inverted" : "",
           controls.stick_speed,controls.prompts);
    publish_prompts();
    pad_prompts_stale=1;
}

static void key_axis(uint8_t *axis, bool negative, bool positive) {
    if (negative || positive) *axis=(uint8_t)(128-(negative ? 128 : 0)+(positive ? 127 : 0));
}
/* The mouse: its buttons through mouse_map, its movement as the right stick. The stick follows
 * the mouse's speed (pixels per ms, smoothed over ~25 ms), starts past the games' stick dead
 * zone so slow movements turn the camera too, and lets go when the mouse rests. */
/* Returns 1 when the mouse was used (moved or a button held while captured). */
static int pad_aim_held;
static int mouse_look_debug(void) {
    static int debug=-1;
    if (debug<0) { const char *e=getenv("GOW3_MOUSE_LOOK_DEBUG"); debug=e && e[0]=='1'; }
    return debug;
}
int runtime_pad_aim_held(void) { return __atomic_load_n(&pad_aim_held,__ATOMIC_RELAXED); }
static int sample_mouse(PadData *d) {
    if (!mouse_enabled()) return 0;
    /* 20 Oct 2026: the aim sensitivity while aiming, the up/down speed and the left/right invert
     * (the mouse look handlers apply the same). */
    const float aim=runtime_pad_aim_held() ? gow3gpu_controls_mouse_aim_sens() : 1.0f;
    const float vertical=gow3gpu_controls_mouse_vertical();
    const float sensitivity=(controls.mouse_sens>0.0f ? controls.mouse_sens : 1.0f)*(aim>0.0f ? aim : 1.0f);
    const float invert=controls.mouse_invert ? -1.0f : 1.0f;
    const float invert_x=gow3gpu_controls_mouse_invert_x() ? -1.0f : 1.0f;
    float dx=0, dy=0; unsigned held=0;
    const int captured=gow3gpu_mouse_take(&dx,&dy,&held);
    const int used=captured && (held || fabsf(dx)+fabsf(dy)>2.0f);
    for (size_t i=0;i<mouse_map.count;++i)
        if (held & (1u<<(mouse_map.entries[i].source-1))) d->buttons|=mouse_map.entries[i].ps;
    static float vx, vy; static uint64_t last, last_motion;
    const uint64_t now=d->timestamp;
    float dt=last ? (float)(now-last)/1000.0f : 16.0f;
    last=now;
    if (dt<1.0f) dt=1.0f;
    if (dt>50.0f) dt=50.0f;
    if (dx!=0.0f || dy!=0.0f) last_motion=now;
    const float blend=dt/(dt+25.0f);
    vx+=(dx/dt-vx)*blend;
    vy+=(dy/dt-vy)*blend;
    if (!captured || now-last_motion>60000) { vx=vy=0; return used; }
    /* Mouse look drives the game's camera directly (src/runtime_mouselook.c): no stick. While
     * aiming the games turn their aim cameras by the right stick, so the mouse is the stick then
     * (20 Oct 2026: the follow camera's hook kept being called while aiming, which switched this
     * path off most of the time: aiming with the mouse was choppy). */
    const int aiming=runtime_pad_aim_held();
    /* The follow camera's hook turns the camera directly only while that camera runs at every pad
     * read (the last 4); climbing, hanging, scripted and aim cameras read the stick, and the hook
     * may still be called now and then for a camera not on screen (20 Oct 2026: the mouse looked
     * stuck there behind the old 200 ms rule). Otherwise the mouse is the stick and the hook gets
     * nothing, so it never turns twice. */
    static unsigned seen_calls, history;
    const unsigned calls=gow3gpu_mouse_look_calls();
    history=((history<<1)|(calls!=seen_calls ? 1u : 0u))&15u;
    seen_calls=calls;
    /* gow3 24 Oct 2026: while aiming only the aim cameras' hooks count (Uncharted 2 and 3, see
     * runtime_mouselook.c; the follow hooks neither take nor count then), so direct while aiming
     * means an aim hook took the mouse at every pad read. Without one (Uncharted 1, the hook off, the
     * game putting the direction back while it zooms in) the mouse is the stick, as before. */
    const int direct=history==15u;
    gow3gpu_mouse_look_pause(!direct);
    if (mouse_look_debug()) {
        static uint64_t debug_last; static unsigned reads, direct_reads;
        ++reads; direct_reads+=direct;
        if (now-debug_last>=1000000) {
            printf("Mouse debug: %u pad reads, %u with the camera hook (direct), aiming %d, stick %.2f %.2f\n",
                   reads,direct_reads,aiming,vx,vy);
            fflush(stdout);
            debug_last=now; reads=direct_reads=0;
        }
    }
    if (direct) { vx=vy=0; return used; }
    /* The games turn at about (deflection past a ~0.15 dead zone)^1.5 of their full speed. The
     * deflection is the inverse of that, so the turn follows the mouse's speed evenly (the linear
     * mapping before turned slow movements too little and fast ones too much). */
    float sx=vx*sensitivity*0.08f*invert_x, sy=vy*sensitivity*0.08f*invert*(vertical>0.0f ? vertical : 1.0f);
    const float magnitude=sqrtf(sx*sx+sy*sy);
    if (magnitude<0.002f) return used;
    const float shaped=powf(magnitude>1.0f ? 1.0f : magnitude,2.0f/3.0f);
    const float out=0.15f+0.85f*shaped>1.0f ? 1.0f : 0.15f+0.85f*shaped;
    sx*=out/magnitude; sy*=out/magnitude;
    d->right_x=(uint8_t)(sx<-1.0f ? 0 : sx>1.0f ? 255 : (int)(128.0f+sx*127.0f));
    d->right_y=(uint8_t)(sy<-1.0f ? 0 : sy>1.0f ? 255 : (int)(128.0f+sy*127.0f));
    return used;
}
static void sample_host(PadData *d) {
    memset(d,0,sizeof(*d));
    d->left_x=d->left_y=d->right_x=d->right_y=128;
    d->orientation[3]=1.0f;
    d->connected=1; d->connected_count=connected_count ? connected_count : 1;
    d->timestamp=now_us();
    SDL_Gamepad *g=current_gamepad();
    prompt_activity.keyboard=prompt_activity.gamepad=0;
    if (gow3gpu_overlay_captures_input()) return; /* settings menu open: neutral input */
    const bool *k=SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetKeyboardState(NULL) : NULL;
    sync_controls();
    runtime_gamesettings_tick();
    publish_pad_prompts(g);
    int keyboard_used=0, gamepad_used=0;
    /* SDL delivers gamepad events in the background too (window.cpp): the game takes them
     * only while its window is in front, or always with its own controller (GOW3_PAD_INDEX,
     * local co-op: two windows, one controller each). */
    if (g && (pad_index() || gow3gpu_window_foreground())) {
        for (size_t i=0;i<pad_map.count;++i)
            if (SDL_GetGamepadButton(g,(SDL_GamepadButton)pad_map.entries[i].source)) d->buttons|=pad_map.entries[i].ps;
        const uint32_t mapped=d->buttons;
        d->left_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTX)); d->left_y=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTY));
        d->right_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTX)); d->right_y=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTY));
        d->l2=trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFT_TRIGGER)); d->r2=trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
        if (d->l2>30) d->buttons|=BTN_L2;
        if (d->r2>30) d->buttons|=BTN_R2;
        if (mapped & BTN_L2) d->l2=255;
        if (mapped & BTN_R2) d->r2=255;
        /* Past half way: resting sticks that drift do not count as use. */
        gamepad_used=mapped || abs(d->left_x-128)>64 || abs(d->left_y-128)>64 || abs(d->right_x-128)>64 ||
                     abs(d->right_y-128)>64 || d->l2>64 || d->r2>64;
        if (SDL_GetNumGamepadTouchpads(g)>0) {
            const int fingers=SDL_GetNumGamepadTouchpadFingers(g,0);
            for (int finger=0;finger<fingers && d->touch_count<2;++finger) {
                bool down=false;
                float x=0, y=0;
                if (SDL_GetGamepadTouchpadFinger(g,0,finger,&down,&x,&y,NULL) && down) {
                    d->touches[d->touch_count++]=(PadTouch){.x=touch_axis(x,1919),
                        .y=touch_axis(y,942),.id=(uint8_t)finger};
                }
            }
        }
    }
    /* The keyboard works with or without a gamepad: its buttons add to the pad's, and a stick
     * follows the keys only while one of them is held. GOW3_KEYBOARD=0 turns it off (typing in
     * another window that lost the focus to the game would otherwise press buttons). */
    if (k && keyboard_enabled()) {
        for (size_t i=0;i<key_map.count;++i) if (k[key_map.entries[i].source]) { d->buttons|=key_map.entries[i].ps; keyboard_used=1; }
        bool held[STICK_KEYS]={0};
        for (size_t i=0;i<stick_keys.count;++i) if (k[stick_keys.entries[i].source]) { held[stick_keys.entries[i].direction]=true; keyboard_used=1; }
        key_axis(&d->left_x,held[LS_LEFT],held[LS_RIGHT]);
        key_axis(&d->left_y,held[LS_UP],held[LS_DOWN]);
        key_axis(&d->right_x,held[RS_LEFT],held[RS_RIGHT]);
        key_axis(&d->right_y,held[RS_UP],held[RS_DOWN]);
    }
    if (sample_mouse(d)) keyboard_used=1;
    if (d->buttons & BTN_L2) d->l2=255;
    __atomic_store_n(&pad_aim_held,(d->buttons & BTN_L2) || d->l2>30,__ATOMIC_RELAXED);
    if (d->buttons & BTN_R2) d->r2=255;
    prompt_activity.keyboard=keyboard_used;
    prompt_activity.gamepad=gamepad_used;
    prompt_activity.connected=g!=NULL;
}

/* GOW3_PAD_FILE=<file>: scripted input for automated runs. The file holds whitespace-separated
 * tokens, re-read when it changes: button names (cross circle square triangle l1 r1 l2 r2 l3 r3
 * options touchpad touchpad_left touchpad_right up down left right) are held while listed;
 * touchpad is the left side; lx= ly= rx= ry= (0..255) override the sticks; mx= my= move the
 * mouse (counts per 10 ms, for mouse look tests); kbm / pad count as keyboard / gamepad use (the
 * button icons follow, see update_prompt_device). An empty file releases everything. */
static struct { uint32_t buttons; int stick[4]; float mouse[2]; int kbm, pad; } injected={0,{-1,-1,-1,-1},{0,0},0,0};
static int replay_armed;      /* 1 while a GOW3_PAD_REPLAY recording plays, 2 once it ended */
static int record_armed;      /* GOW3_PAD_RECORD: 1 after the token "record", 2 once recording */
static uint64_t replay_start; /* 0: (re)start at the next sample */
static void read_inject(void) {
    static const char *path; static int checked; static uint64_t last_check; static struct timespec mtime;
    if (!checked) { path=getenv("GOW3_PAD_FILE"); checked=1; }
    if (!path || !*path) return;
    uint64_t now=now_us();
    if (now-last_check<20000) return;
    last_check=now;
    struct stat st;
    if (stat(path,&st)!=0) return;
#ifdef _WIN32
    /* Whole-second mtimes: the size tells two edits within a second apart. */
    if (st.st_mtime==mtime.tv_sec && st.st_size==mtime.tv_nsec) return;
    mtime=(struct timespec){st.st_mtime,(long)st.st_size};
#else
    if (st.st_mtim.tv_sec==mtime.tv_sec && st.st_mtim.tv_nsec==mtime.tv_nsec) return;
    mtime=st.st_mtim;
#endif
    FILE *f=fopen(path,"r");
    if (!f) return;
    static const char *sticks[]={"lx=","ly=","rx=","ry="};
    injected.buttons=0;
    for (int i=0;i<4;++i) injected.stick[i]=-1;
    injected.mouse[0]=injected.mouse[1]=0;
    injected.kbm=injected.pad=0;
    char token[64];
    while (fscanf(f,"%63s",token)==1) {
        if (!strcmp(token,"replay") && replay_armed!=1) { replay_armed=1; replay_start=0; } /* GOW3_PAD_REPLAY */
        if (!strcmp(token,"record") && record_armed==0) record_armed=1; /* GOW3_PAD_RECORD */
        for (size_t i=0;i<sizeof(ps_names)/sizeof(*ps_names);++i) if (!strcmp(token,ps_names[i].name)) injected.buttons|=ps_names[i].ps;
        for (int i=0;i<4;++i) if (!strncmp(token,sticks[i],3)) { int v=atoi(token+3); injected.stick[i]=v<0 ? 0 : v>255 ? 255 : v; }
        if (!strncmp(token,"mx=",3)) injected.mouse[0]=(float)atof(token+3);
        if (!strncmp(token,"my=",3)) injected.mouse[1]=(float)atof(token+3);
        if (!strcmp(token,"kbm")) injected.kbm=1;
        if (!strcmp(token,"pad")) injected.pad=1;
    }
    fclose(f);
    printf("Runtime: pad file: buttons 0x%x sticks %d %d %d %d\n",injected.buttons,
           injected.stick[0],injected.stick[1],injected.stick[2],injected.stick[3]);
}
/* GOW3_PAD_RECORD=<file>: F9 starts and stops recording the pad state (gamepad or keyboard) with
 * the time since F9; GOW3_PAD_REPLAY=<file> plays such a recording back, started by the token
 * "replay" in GOW3_PAD_FILE (scripted tests repeat a route the player ran once). Lines: ms buttons
 * lx ly rx ry l2 r2, written when the state changes.
 * 21 Oct 2026: the token "record" in GOW3_PAD_FILE arms it instead: the recording starts at the
 * player's first stick or button input (time 0 = that input) and lasts until the game ends; F9
 * does nothing then. A test script arms it when the level runs, as it starts a replay. */
typedef struct { uint32_t ms, buttons; uint8_t axes[4], l2, r2; } PadSample;
static FILE *record_file;
static uint64_t record_start;
static PadSample record_last;
static void record_sample(const PadData *d) {
    static const char *path; static int checked, f9_was_down;
    if (!checked) { path=getenv("GOW3_PAD_RECORD"); checked=1; }
    if (!path || !*path || !sdl_ready) return;
    if (record_armed==1 && !record_file) {
        const int moved=d->buttons || abs(d->left_x-128)>24 || abs(d->left_y-128)>24 ||
                        abs(d->right_x-128)>24 || abs(d->right_y-128)>24 || d->l2>32 || d->r2>32;
        if (moved && (record_file=fopen(path,"w"))) {
            record_armed=2;
            record_start=now_us();
            memset(&record_last,0xff,sizeof(record_last));
            printf("Runtime: pad recording started at the first input (%s)\n",path);
        }
    }
    const bool *k=SDL_GetKeyboardState(NULL);
    const int f9=k && k[SDL_SCANCODE_F9] && !record_armed;
    if (f9 && !f9_was_down) {
        if (record_file) {
            fclose(record_file); record_file=NULL;
            printf("Runtime: pad recording stopped (%s)\n",path);
        } else if ((record_file=fopen(path,"w"))) {
            record_start=now_us();
            memset(&record_last,0xff,sizeof(record_last));
            printf("Runtime: pad recording started (%s, F9 stops)\n",path);
        }
    }
    f9_was_down=f9;
    if (!record_file) return;
    PadSample s={(uint32_t)((now_us()-record_start)/1000),d->buttons,
                 {d->left_x,d->left_y,d->right_x,d->right_y},d->l2,d->r2};
    if (s.buttons==record_last.buttons && !memcmp(s.axes,record_last.axes,4) &&
        s.l2==record_last.l2 && s.r2==record_last.r2) return;
    record_last=s;
    fprintf(record_file,"%u %u %u %u %u %u %u %u\n",s.ms,s.buttons,s.axes[0],s.axes[1],s.axes[2],
            s.axes[3],s.l2,s.r2);
    fflush(record_file);
}
static PadSample *replay; static size_t replay_count, replay_next;
static void replay_sample(PadData *d) {
    if (!replay_armed) return;
    if (!replay_start) {
        static int loaded;
        if (!loaded) {
            loaded=1;
            const char *path=getenv("GOW3_PAD_REPLAY");
            FILE *f=path ? fopen(path,"r") : NULL;
            PadSample s; unsigned v[8]; size_t cap=0;
            while (f && fscanf(f,"%u %u %u %u %u %u %u %u",&v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7])==8) {
                s=(PadSample){v[0],v[1],{(uint8_t)v[2],(uint8_t)v[3],(uint8_t)v[4],(uint8_t)v[5]},(uint8_t)v[6],(uint8_t)v[7]};
                if (replay_count==cap && !(replay=realloc(replay,(cap=cap ? cap*2 : 1024)*sizeof(*replay)))) break;
                replay[replay_count++]=s;
            }
            if (f) fclose(f);
            printf("Runtime: pad replay of %zu samples from %s\n",replay_count,path ? path : "(unset)");
        }
        replay_start=now_us();
        replay_next=0;
    }
    const uint32_t ms=(uint32_t)((now_us()-replay_start)/1000);
    while (replay_next<replay_count && replay[replay_next].ms<=ms) ++replay_next;
    if (!replay_next) return;
    if (replay_next==replay_count && ms>replay[replay_count-1].ms+500) {
        if (replay_armed==1) { puts("Runtime: pad replay finished"); replay_armed=2; }
        return;
    }
    const PadSample *s=&replay[replay_next-1];
    d->buttons=s->buttons;
    d->left_x=s->axes[0]; d->left_y=s->axes[1]; d->right_x=s->axes[2]; d->right_y=s->axes[3];
    d->l2=s->l2; d->r2=s->r2;
}
/* After the menu or the text dialog closes, buttons still held (the Cross that accepted a
 * name) stay hidden until released: the game would take them as a new press. */
static int hold_after_capture;
static void sample(PadData *d) {
    sample_host(d);
    if (gow3gpu_overlay_captures_input()) { hold_after_capture=1; click.side=-1; return; }
    record_sample(d); /* the click sources as they are: a replay plays them again */
    read_inject();
    update_prompt_device(prompt_activity.keyboard || injected.kbm,prompt_activity.gamepad || injected.pad,
                         prompt_activity.connected);
    replay_sample(d);
    d->buttons|=injected.buttons;
    if (injected.buttons & BTN_L2) d->l2=255;
    if (injected.buttons & BTN_R2) d->r2=255;
    /* Aiming also when the pad file or a replay holds L2 (sample_host saw only the devices). */
    if (d->buttons & BTN_L2) __atomic_store_n(&pad_aim_held,1,__ATOMIC_RELAXED);
    uint8_t *axes[4]={&d->left_x,&d->left_y,&d->right_x,&d->right_y};
    for (int i=0;i<4;++i) if (injected.stick[i]>=0) *axes[i]=(uint8_t)injected.stick[i];
    {
        static uint64_t last_mouse;
        const uint64_t now=d->timestamp;
        if ((injected.mouse[0]!=0 || injected.mouse[1]!=0) && last_mouse) {
            const float ms=(float)(now-last_mouse)/1000.0f;
            if (ms>0 && ms<100) gow3gpu_mouse_inject(injected.mouse[0]*ms/10.0f,injected.mouse[1]*ms/10.0f);
        }
        last_mouse=now;
    }
    play_click(d);
    if (hold_after_capture) {
        if (d->buttons) d->buttons=0;
        else hold_after_capture=0;
    }
}

static ABI int32_t pad_init(void) { pthread_mutex_lock(&lock); initialized=1; pthread_mutex_unlock(&lock); return 0; }
static ABI int32_t pad_open(int32_t user, int32_t type, int32_t index, const void *param) {
    (void)param;
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (user!=1) return ERR_INVALID_ARG;
    if (type!=0 && type!=2) return ERR_INVALID_ARG; /* standard / special port */
    if (index) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    int already=opened; opened=1;
    pthread_mutex_unlock(&lock);
    if (already) return ERR_ALREADY_OPENED;
    puts("Runtime: pad opened for user 1 (SDL gamepad or keyboard)");
    return PAD_HANDLE;
}
static ABI int32_t pad_close(int32_t handle) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    opened=0; return 0;
}
static ABI int32_t pad_read_state(int32_t handle, PadData *data) {
    UC2MP_THREAD_OBSERVE(UC2MP_DIAG_PAD_READ_STATE);
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!data) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    sample(data); ++reads;
    pthread_mutex_unlock(&lock);
    return 0;
}
/* Buffered reads consume reports. Tight polling must not synthesize an
 * unlimited stream of identical reports from one SDL input snapshot. */
static int buffered_report(PadData *out,const PadData *sampled) {
    PadData state=*sampled;
    state.timestamp=0;
    if (have_buffered && !memcmp(&state,&last_buffered,sizeof(state)) &&
        sampled->timestamp-last_buffered_time<4000) return 0;
    last_buffered=state; last_buffered_time=sampled->timestamp; have_buffered=1;
    *out=*sampled;
    return 1;
}
static ABI int32_t pad_read(int32_t handle, PadData *data, int32_t count) {
    UC2MP_THREAD_OBSERVE(UC2MP_DIAG_PAD_READ);
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!data || count<1 || count>64) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    PadData sampled;
    sample(&sampled); ++reads;
    int result=buffered_report(data,&sampled);
    pthread_mutex_unlock(&lock);
    /* Observed UC2 v1.02 caller shares the lobby's game thread. TLS dispatch
     * rejects all other callers; never perform network IO under pad lock. */
    runtime_uc2mp_tick_run();
    return result;
}
static ABI int32_t pad_info(int32_t handle, ControllerInfo *info) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!info) return ERR_INVALID_ARG;
    memset(info,0,sizeof(*info));
    info->pixel_density=44.86f; info->resolution_x=1920; info->resolution_y=943;
    info->dead_zone_left=info->dead_zone_right=2;
    info->connection_type=0; info->connected=1; info->device_class=0;
    pthread_mutex_lock(&lock);
    current_gamepad();
    info->connected_count=connected_count ? connected_count : 1;
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t pad_vibration(int32_t handle, const uint8_t *param) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!param) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    SDL_Gamepad *g=current_gamepad();
    /* gow3 24 Oct 2026 (GitHub #166): Controls -> Controller vibration off stops every rumble
     * (a zero rumble also ends one that was running when the option was turned off). */
    const int on=gow3gpu_controls_vibration();
    if (g) SDL_RumbleGamepad(g,on ? (uint16_t)(param[0]*257) : 0,on ? (uint16_t)(param[1]*257) : 0,1000);
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t pad_ok_handle(int32_t handle) { return handle==PAD_HANDLE && opened ? 0 : ERR_INVALID_HANDLE; }
static ABI int32_t pad_ok_handle_flag(int32_t handle, uint8_t flag) { (void)flag; return pad_ok_handle(handle); }

/* The light bar colour the game sets (DualShock 4 / DualSense through SDL); kept for a gamepad
   connected later. scePadResetLightBar goes back to the PS4's player 1 blue. */
static uint8_t light_bar[3]={0,0,255};
static int light_bar_set;
static void apply_light_bar(SDL_Gamepad *g) {
    if (g && light_bar_set) SDL_SetGamepadLED(g,light_bar[0],light_bar[1],light_bar[2]);
}
static void set_light_bar(uint8_t r, uint8_t g, uint8_t b) {
    static uint32_t seen[16]; static int seen_count;
    const uint32_t rgb=(uint32_t)r<<16 | (uint32_t)g<<8 | b;
    int known=0;
    for (int i=0;i<seen_count;++i) known|=seen[i]==rgb;
    if (!known && seen_count<16) {
        seen[seen_count++]=rgb;
        printf("Runtime: light bar colour %06x\n",rgb);
    }
    pthread_mutex_lock(&lock);
    light_bar[0]=r; light_bar[1]=g; light_bar[2]=b; light_bar_set=1;
    apply_light_bar(current_gamepad());
    pthread_mutex_unlock(&lock);
}
static ABI int32_t pad_light_bar(int32_t handle, const uint8_t *rgb) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!rgb) return ERR_INVALID_ARG;
    set_light_bar(rgb[0],rgb[1],rgb[2]);
    return 0;
}
static ABI int32_t pad_reset_light_bar(int32_t handle) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    set_light_bar(0,0,255);
    return 0;
}
static const RuntimeExport exports[]={
    {"scePadSetLightBar",pad_light_bar}, {"scePadResetLightBar",pad_reset_light_bar},
    {"scePadInit",pad_init}, {"scePadOpen",pad_open}, {"scePadClose",pad_close},
    {"scePadReadState",pad_read_state}, {"scePadRead",pad_read},
    {"scePadGetControllerInformation",pad_info}, {"scePadSetVibration",pad_vibration},
    {"scePadResetOrientation",pad_ok_handle},
    {"scePadSetAngularVelocityDeadbandState",pad_ok_handle_flag}, {"scePadSetTiltCorrectionState",pad_ok_handle_flag},
    {"scePadSetMotionSensorState",pad_ok_handle_flag},
};
uintptr_t runtime_pad_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_pad_report(void) { printf("Runtime: pad reads=%zu, gamepad=%s\n",reads,gamepad ? SDL_GetGamepadName(gamepad) : "none"); }
