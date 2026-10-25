/* C interface of the GPU library (gpu/): shadPS4's Liverpool/Vulkan video core,
 * GnmDriver, VideoOut and kernel event queues, adapted to the native loader. */
#ifndef GOW3GPU_H
#define GOW3GPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *title;          /* window title */
    const char *serial;         /* CUSA id, names the pipeline cache */
    const char *user_dir;       /* pipeline cache/logs directory */
    uint32_t sdk_version;       /* from the eboot's procparam */
    uint32_t psf_attributes;    /* param.sfo ATTRIBUTE */
    int32_t width, height;      /* initial window size */
} Gow3GpuConfig;
/* Registers kernel event queues (needed with or without graphics). */
void gow3gpu_register_kernel(void);
/* Creates window, Vulkan device, presenter and GPU command processor. */
int gow3gpu_init(const Gow3GpuConfig *config);
/* Function for an imported NID ("NID#lib#mod"), or 0 when the GPU library does not provide it. */
uintptr_t gow3gpu_resolve(const char *scoped_nid);
/* Called first by the loader's SIGSEGV handler: 1 when a GPU page-tracking fault was handled. */
int gow3gpu_handle_fault(void *ucontext, void *address);
/* GOW3_WRITE_LOG=1: prints the logged GPU-side writes to guest memory near the fault. */
void gow3gpu_dump_guest_writes(void *ucontext);
/* Keyboard text entry through the game window (IME dialog). begin returns 0 when
 * no window exists; poll returns 0 typing, 1 confirmed, 2 cancelled (UTF-8 text). */
int gow3gpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
int gow3gpu_text_input_poll(char *out_utf8, uint64_t size);
/* A list for the player to choose from, drawn over the game (save list). images: a PNG path per
 * item drawn beside it (NULL array or entry: none). begin: 0 when no window exists.
 * poll: -1 while open, -2 cancelled, else the chosen index. */
int gow3gpu_choice_begin(const char *title, const char *const *items, const char *const *images, int count, int focus);
int gow3gpu_choice_poll(void);
/* Trophies (gow3_trophies.cpp): the running game's list (its trophy pack, a host path; the list
 * number 1-3; 128 earned flags), then each newly earned trophy, shown as a popup. */
void gow3gpu_trophy_list(const char *pack_path, int list, const uint32_t earned[4]);
void gow3gpu_trophy_earned(int id);
/* 1 while the in-game settings menu is open: the game's pad input is held neutral. */
int gow3gpu_overlay_captures_input(void);
/* Mouse movement since the last call (pixels) and held sources (bit 0 left, 1 middle, 2 right,
 * 3 X1, 4 X2, 5 wheel up, 6 wheel down); returns 1 while the mouse is captured (gow3_mouse.h). */
int gow3gpu_mouse_take(float *dx, float *dy, unsigned *buttons);
int gow3gpu_mouse_take_look(float *dx, float *dy);
int gow3gpu_mouse_look_recent(void);
/* 20 Oct 2026: camera hook calls so far, and whether hooks get the mouse (0) or not (1). */
unsigned gow3gpu_mouse_look_calls(void);
void gow3gpu_mouse_look_pause(int paused);
void gow3gpu_mouse_inject(float dx, float dy);
/* Button prompts (gow3_prompts.cpp): per PS4 button a label. Set 0, keyboard and mouse: its key
 * ("SPACE", "F", UTF-8 arrows) or mouse source ("@LMB" "@RMB" "@MMB" "@X1" "@X2" "@WHEELUP"
 * "@WHEELDOWN"; "@MOUSE" for GOW3_PROMPT_RS_UP: the mouse moves the camera). Set 1, controllers
 * other than PlayStation ones: "#A" "#B" "#X" "#Y" "#LB" "#RB" "#LT" "#RT" "#LS" "#RS" "#MENU"
 * "#VIEW". "" keeps the PlayStation icon. set_device: 0 the game's PlayStation icons, 1 keyboard
 * and mouse (set 0), 2 another controller (set 1). */
enum {
    GOW3_PROMPT_CROSS, GOW3_PROMPT_CIRCLE, GOW3_PROMPT_SQUARE, GOW3_PROMPT_TRIANGLE,
    GOW3_PROMPT_L1, GOW3_PROMPT_R1, GOW3_PROMPT_L2, GOW3_PROMPT_R2, GOW3_PROMPT_L3, GOW3_PROMPT_R3,
    GOW3_PROMPT_OPTIONS, GOW3_PROMPT_TOUCHPAD,
    GOW3_PROMPT_UP, GOW3_PROMPT_DOWN, GOW3_PROMPT_LEFT, GOW3_PROMPT_RIGHT,
    GOW3_PROMPT_LS_UP, GOW3_PROMPT_LS_LEFT, GOW3_PROMPT_LS_DOWN, GOW3_PROMPT_LS_RIGHT,
    GOW3_PROMPT_RS_UP, GOW3_PROMPT_RS_LEFT, GOW3_PROMPT_RS_DOWN, GOW3_PROMPT_RS_RIGHT,
    GOW3_PROMPT_COUNT
};
void gow3gpu_prompts_set_labels(int set, const char *const *labels, int count);
void gow3gpu_prompts_set_device(int device);
/* Controls: the in-game menu's Controls tab and the launcher's, kept in the settings file
 * (gow3_settings.h; GOW3_PAD_MAP, GOW3_KEY_MAP, GOW3_MOUSE_MAP, GOW3_MOUSE, GOW3_MOUSE_SENS,
 * GOW3_MOUSE_INVERT, GOW3_PROMPTS and GOW3_STICK_SPEED override the file at start). The pad
 * (src/runtime_pad.c) reapplies them when the generation changes. Maps in runtime_pad.c's syntax
 * on top of its defaults ("" = the defaults). prompts: -1 auto, 0 PlayStation, 1 keyboard,
 * 2 Xbox, 3 the connected controller's. stick_speed: controller camera and aim speed. */
typedef struct {
    char pad_map[1024], key_map[2048], mouse_map[512];
    int mouse, mouse_invert, prompts;
    float mouse_sens, stick_speed;
} Gow3Controls;
unsigned gow3gpu_controls_generation(void);
void gow3gpu_controls_get(Gow3Controls *out);
/* Live values for the camera hooks (src/runtime_mouselook.c). */
float gow3gpu_controls_mouse_sens(void);
/* gow3 20 Oct 2026: while aiming (times mouse_sens), up/down compared with left/right, invert left/right. */
float gow3gpu_controls_mouse_aim_sens(void);
float gow3gpu_controls_mouse_vertical(void);
int gow3gpu_controls_mouse_invert_x(void);
int gow3gpu_controls_vibration(void);
int gow3gpu_controls_mouse_invert(void);
float gow3gpu_controls_stick_speed(void);
/* The games' draw and shadow distances (menu Image tab, percent of the game's own;
 * src/runtime_gamesettings.c). */
int gow3gpu_settings_fade_distance(void);
int gow3gpu_settings_sun_shadow_distance(void);
int gow3gpu_settings_light_shadow_distance(void);
/* Number of symbols registered by the vendored libraries (diagnostics). */
unsigned gow3gpu_symbol_count(void);
#ifdef __cplusplus
}
#endif
#endif
