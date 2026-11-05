/* Mouse look handlers (see runtime_mouselook.h). They run on the game's thread inside its camera
 * update, once per frame, so they take the mouse movement since the last frame and apply it to
 * the camera state the function is about to use. Small stacks: the game runs this in job fibers
 * with 64 KiB stacks. */
#include "runtime_mouselook.h"
#include "runtime.h"
#include "gpu/gow3gpu.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned char *image;

static void trace_init(void);
void runtime_mouselook_set_image(unsigned char *base) {
    image = base;
    trace_init();
}

int runtime_mouselook_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        /* Also with the mouse off: the hooks give the controller camera speed too. Without a
         * captured mouse (off, or no focus) they take no mouse movement. */
        const char *look = getenv("GOW3_MOUSE_LOOK");
        enabled = !(look && look[0] == '0');
    }
    return enabled;
}

/* Radians per mouse count at sensitivity 1 (the settings' mouse sensitivity scales it, live;
 * 0.0011 rad is about 0.063 degrees: a 2,000-count swipe turns about 126 degrees). */
static float look_scale(void) {
    const float s = gow3gpu_controls_mouse_sens();
    /* While aiming: the mouse aim sensitivity on top (20 Oct 2026). */
    const float aim = runtime_pad_aim_held() ? gow3gpu_controls_mouse_aim_sens() : 1.0f;
    return 0.0011f * (s > 0.0f ? s : 1.0f) * (aim > 0.0f ? aim : 1.0f);
}
/* The mouse movement with the left/right invert and the up/down speed applied (20 Oct 2026). */
static int take_mouse(float *dx, float *dy) {
    if (!gow3gpu_mouse_take_look(dx, dy)) return 0;
    const float vertical = gow3gpu_controls_mouse_vertical();
    if (gow3gpu_controls_mouse_invert_x()) *dx = -*dx;
    *dy *= vertical > 0.0f ? vertical : 1.0f;
    return 1;
}
static int take_look(float *dx, float *dy) {
    /* While aiming the aim cameras take the mouse (their own hooks below) or read the right stick,
     * which runtime_pad.c makes of the mouse; the follow camera keeps still (a turn of it would only
     * show after aiming). gow3 24 Oct 2026: it neither takes the movement nor counts as a hook call
     * then, so the pad's direct mode while aiming means an aim hook took the mouse at every pad read. */
    if (runtime_pad_aim_held()) {
        *dx = *dy = 0.0f;
        return 0;
    }
    if (!gow3gpu_mouse_take_look(dx, dy)) return 0;
    const float vertical = gow3gpu_controls_mouse_vertical();
    if (gow3gpu_controls_mouse_invert_x()) *dx = -*dx;
    *dy *= vertical > 0.0f ? vertical : 1.0f;
    return 1;
}
/* gow3 24 Oct 2026: the games' own invert options flip the stick when the camera reads it (U1 ints at
 * 0x95ca20 / 0x95ca24, U2 at 0x105de9c / 0x105dea0, U3 at *(0x1b24b80) + player * 4 + 0x4c50 /
 * 0x4c58, player = camera + 0xe4). The mouse-as-stick path (aiming, cover, climbing) goes through
 * them, so the direct turns do as well: every camera turns the same way. */
static void game_invert(const int *flags, float *dx, float *dy) {
    if (!flags) return;
    if (flags[0]) *dx = -*dx;
    if (flags[1]) *dy = -*dy;
}
static const int *u3_invert_flags(const unsigned char *camera) {
    const unsigned char *options = *(const unsigned char *const *)(image + 0x1b24b80);
    const int player = *(const int *)(camera + 0xe4);
    if (!options || player < 0 || player > 3) return NULL;
    static _Thread_local int flags[2];
    flags[0] = *(const int *)(options + player * 4 + 0x4c50);
    flags[1] = *(const int *)(options + player * 4 + 0x4c58);
    return flags;
}
static float look_invert(void) {
    return gow3gpu_controls_mouse_invert() ? -1.0f : 1.0f;
}

/* gow3 1 Nov 2026: GOW3_MOUSE_TRACE=<file> (a test aid): one line per camera hook call and per pad
 * read. Hooks: "<wall time s> <hook> <camera> <newest> <heading> <pitch> <dx> <dy> <mode>", heading
 * in degrees about +Y at the call's entry (before the mouse's turn), pitch the camera's own vertical
 * value at entry (degrees, or a 0..1 arc/curve position), dx/dy the counts it turned by, mode d
 * (direct turn), s (shared: another camera's turn of this frame), - (nothing to turn), p (the pad
 * gave the mouse to the stick). Pad reads: "<time> pad <direct> <stick x> <stick y>". Summing the
 * heading changes per camera between lines gives the real turn, with no wrap at 180 degrees. */
static FILE *trace_file;
static void trace_init(void) {
    static int done;
    if (done) return;
    done = 1;
    const char *path = getenv("GOW3_MOUSE_TRACE");
    trace_file = path && path[0] ? fopen(path, "w") : NULL;
    if (trace_file) setvbuf(trace_file, NULL, _IOFBF, 1 << 16);
}
static double wall_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
static void trace_flush(void) {
    static unsigned lines;
    if (++lines % 32 == 0) fflush(trace_file);
}
static float dir_heading(const float *dir) {
    return dir ? atan2f(dir[0], dir[2]) * 57.29578f : 0.0f;
}
static float dir_pitch(const float *dir) {
    return dir ? atan2f(dir[1], sqrtf(dir[0] * dir[0] + dir[2] * dir[2])) * 57.29578f : 0.0f;
}
/* Two more values a hook may add to its next trace line (camera fields under study). */
static _Thread_local float trace_extra[2];
static void mouse_trace(const char *hook, const void *camera, int newest, float heading, float pitch, float dx, float dy,
                        char mode) {
    if (!trace_file) return;
    /* The camera's class: its vtable as an image offset (0 when the first word is not one). */
    uintptr_t vtable = camera ? *(const uintptr_t *)camera : 0;
    vtable = vtable > (uintptr_t)image && vtable < (uintptr_t)image + 0x4000000 ? vtable - (uintptr_t)image : 0;
    fprintf(trace_file, "%.4f %s %p %d %.3f %.3f %.1f %.1f %c %.3f %.3f %#llx\n", wall_seconds(), hook, camera, newest,
            heading, pitch, dx, dy, mode, trace_extra[0], trace_extra[1], (unsigned long long)vtable);
    trace_extra[0] = trace_extra[1] = 0.0f;
    trace_flush();
}
void runtime_mouselook_trace_pad(int direct, float stick_x, float stick_y) {
    if (!trace_file) return;
    fprintf(trace_file, "%.4f pad %d %.3f %.3f\n", wall_seconds(), direct, stick_x, stick_y);
    trace_flush();
}

/* Controller camera and aim speed (menu Controls tab, GOW3_STICK_SPEED). Uncharted 2's and 3's
 * camera input functions read the right stick once, at their start, from stick + 8 and + 0xc
 * (floats, -1..1; nothing else of it). They get a scaled copy instead (the stub passes the
 * returned pointer as their third argument): the camera turns faster or slower at every
 * deflection, aiming included, with the game's ramp and invert options kept. The caller's stick
 * stays as it was. */
static int look_debug(void);
static void stick_debug(const char *game, const float *look, const float *stick);
static _Thread_local float scaled_stick[4];
static void *scale_stick(const float *stick) {
    const float speed = gow3gpu_controls_stick_speed();
    if (!stick || !(speed > 0.0f) || speed == 1.0f) return NULL;
    memcpy(scaled_stick, stick, sizeof(scaled_stick));
    scaled_stick[2] *= speed;
    scaled_stick[3] *= speed;
    return scaled_stick;
}

/* gow3 1 Nov 2026: one mouse turn per frame for every camera whose input runs in that frame. While the
 * games blend from one follow camera to another (near walls, narrow spots, a new camera setting with
 * another field of view), the input function runs for each camera of the blend, and the stick turns
 * all of them. The mouse went only to the first one (trace: up to 4 cameras a frame in Uncharted 2):
 * the shown view, a mix of the cameras, turned by a fraction of the movement until the blend ended
 * ("the camera gets stuck and the sensitivity drops ... anytime the fov changes"). A camera seen again
 * starts a new frame; the others of the frame get the same turn. GOW3_MOUSE_LOOK_SHARE=0: the first
 * camera takes the mouse alone, as before. */
static int share_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *env = getenv("GOW3_MOUSE_LOOK_SHARE");
        enabled = !(env && env[0] == '0');
    }
    return enabled;
}
typedef struct {
    const void *cameras[4];
    unsigned count;
    float dx, dy;
    int took;
    uint64_t at; /* ms of the frame's take */
} FrameTurn;
static uint64_t now_ms(void);
/* The mouse turn for this camera now: 1 and dx/dy (0 when there is none); 0 when the camera takes
 * no mouse (the pad gives it to the stick). *mode: 'd' a new turn, 's' the frame's shared turn. */
static int frame_take(FrameTurn *t, const void *camera, float *dx, float *dy, char *mode) {
    const uint64_t now = now_ms();
    int seen = 0;
    for (unsigned i = 0; i < t->count; ++i) seen |= t->cameras[i] == camera;
    if (share_enabled() && !seen && t->count > 0 && t->count < 4 && now - t->at < 100) {
        t->cameras[t->count++] = camera;
        *dx = t->dx;
        *dy = t->dy;
        *mode = 's';
        return t->took;
    }
    t->took = take_look(dx, dy);
    t->dx = t->took ? *dx : 0.0f;
    t->dy = t->took ? *dy : 0.0f;
    t->cameras[0] = camera;
    t->count = 1;
    t->at = now;
    *mode = !t->took ? 'p' : (*dx != 0.0f || *dy != 0.0f) ? 'd' : '-';
    return t->took;
}
/* gow3 1 Nov 2026 (near walls, GitHub #275 #291): Uncharted 2's follow cameras keep the stick they read
 * (camera + 0x344: left x, y, right x, y, copied from the pad by 0x42f150 before the input runs; the
 * input function gets a pointer to it) and look at it again after turning: the update 0x42d940 pulls the
 * camera in front of a wall at once while the stick turns it (|right x| > 0.2, at 0x42ddd4; otherwise it
 * eases in), 0x42f150 runs a wall timer while it turns hard (|x| >= 0.7 for 0.4 s, at 0x430b55), and the
 * input 0x428f40 notes the player's steering: 0x9f30, the time 0x9f00 (a walking Drake's camera swings
 * behind him only after tune[0xdc] s without steering, 0x429c20), the end of a scripted auto turn
 * (0x9ed1 cleared) and 0x9f08. The mouse turned the direction with the stick at 0, so near walls the
 * camera acted as if nobody steered it. Now a mouse turn also puts the stick deflection that turns the
 * camera as fast (the yaw rate over the stick's full rate tune[0x70] * scale, smoothed over ~50 ms) into
 * the kept stick for those readers and notes the steering the same way; the input function itself gets
 * a copy of the real stick (it would turn by the deflection otherwise). GOW3_MOUSE_LOOK_STEER=0: off. */
static int steer_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *env = getenv("GOW3_MOUSE_LOOK_STEER");
        enabled = !(env && env[0] == '0');
    }
    return enabled;
}
typedef struct {
    float rate;    /* smoothed mouse yaw rate, rad/s */
    float x;       /* the stick deflection that turns as fast, -1..1 */
    uint64_t at;   /* ms of the last update */
} Steer;
/* Once per frame (a new take): the frame's mouse yaw (counts) over the game's frame time. */
static void steer_update(Steer *s, float dx, float per_count, float dt, float full_rate) {
    const uint64_t now = now_ms();
    if (now - s->at > 100) s->rate = 0.0f; /* the hook did not run for a while */
    s->at = now;
    if (!(dt > 1e-4f && dt < 0.25f)) dt = 1.0f / 60.0f;
    const float k = dt / (dt + 0.05f);
    s->rate += (dx * per_count / dt - s->rate) * k;
    float x = full_rate > 1e-4f && isfinite(full_rate) ? s->rate / full_rate : 0.0f;
    if (!isfinite(x) || fabsf(x) < 0.01f) x = 0.0f;
    s->x = x > 1.0f ? 1.0f : x < -1.0f ? -1.0f : x;
}
/* Where a game keeps it (Uncharted 3's follow cameras work the same way: kept stick camera + 0x3c4, read
 * after the input at 0x4e9479 (|x| > 0.2) and 0x4ec802 (|x| >= 0.7); input 0x4e42f0 notes 0x7930, 0x7900,
 * 0x7908 and clears the auto turn 0x78d0; clock 0x17b4c24, frame time 0x17b4c2c). */
typedef struct {
    unsigned kept;       /* the kept stick (left x, y, right x, y) */
    unsigned steered;    /* byte: 1 = the player steered */
    unsigned steer_time; /* int: the clock when the player last steered */
    unsigned timer;      /* int: the timer the stick X keeps up (within 0.5 s) */
    unsigned auto_turn;  /* byte: a scripted turn toward a direction, ended by steering */
    uint64_t clock;      /* image offset of the game's clock (int, 300 per second) */
} SteerFields;
static const SteerFields u2_steer_fields = {0x344, 0x9f30, 0x9f00, 0x9f08, 0x9ed1, 0x1116304};
static const SteerFields u3_steer_fields = {0x3c4, 0x7930, 0x7900, 0x7908, 0x78d0, 0x17b4c24};
static Steer u1_steer, u2_steer, u3_steer;
/* 1 while the mouse turns the camera whose input or stick step ran last: the games' "the player
 * steers" result after it gets this too (runtime_mouselook_install_patches). */
static volatile int mouse_steering;
static int steering_now(const Steer *s) {
    return steer_enabled() && s->x != 0.0f && now_ms() - s->at < 100;
}
/* The kept stick and the steering notes for one follow camera; returns the stick copy for the input
 * (NULL: nothing written into the kept stick). */
static void *steer_camera(const SteerFields *g, const Steer *s, unsigned char *camera, void *stick) {
    const float x = s->x;
    if (!steer_enabled() || x == 0.0f || !camera) return NULL;
    const int ticks = *(const int *)(image + g->clock);
    camera[g->steered] = 1;
    *(int *)(camera + g->steer_time) = ticks;
    if (fabsf(x) > 0.3f) camera[g->auto_turn] = 0;
    int *timer = (int *)(camera + g->timer);
    if ((float)(ticks - *timer) * (1.0f / 300.0f) < 0.5f) *timer = ticks;
    if ((unsigned char *)stick != camera + g->kept) return NULL;
    float *kept = (float *)stick;
    void *copy = scale_stick(kept);
    if (!copy) {
        memcpy(scaled_stick, kept, sizeof(scaled_stick));
        copy = scaled_stick;
    }
    if (fabsf(kept[2]) < fabsf(x)) kept[2] = x;
    return copy;
}
static FrameTurn u3_frame, u2_frame;
/* Uncharted 3 (1.02), camera input at 0x4e42f0(camera, look direction, stick input, ...):
 * yaw rotates the look direction (x, y, z, 0) about +Y by -stickX * ramp * scale * dt degrees;
 * pitch is a 0..1 position on the camera's vertical curve, stepped by the game's own function
 * 0x100ef90(camera + 0x7750, pitch, delta) with delta = speed(0x7588) * dt * stickY *
 * scale(0x7608). The mouse goes through the same paths without the ramp, dead zone or speed cap.
 * The vertical step per count matches the horizontal one through the stick rates' ratio. */
typedef ABI float (*U3PitchStep)(void *limits, float pitch, float delta);
static ABI void *u3_camera_input(unsigned char *camera, float *look, void *stick) {
    if (look_debug()) stick_debug("U3", look, stick);
    float dx = 0, dy = 0;
    char mode = '-';
    const int took = frame_take(&u3_frame, camera, &dx, &dy, &mode);
    if (mode != 's' && camera) {
        const float *f = (const float *)camera;
        steer_update(&u3_steer, took ? dx : 0.0f, look_scale(), *(const float *)(image + 0x17b4c2c),
                     f[0x7578 / 4] * f[0x7604 / 4] * 0.017453292f);
    }
    if (camera) {
        trace_extra[0] = (float)((const unsigned char *)stick - camera); /* the kept stick's offset: 0x3c4 */
        trace_extra[1] = u3_steer.x;
    }
    mouse_trace("u3f", camera, 1, dir_heading(look), camera ? *(const float *)(camera + 0x7874) : 0.0f, dx, dy, mode);
    void *const steered = steer_camera(&u3_steer_fields, &u3_steer, camera, stick);
    mouse_steering = steering_now(&u3_steer);
    if (!took || (dx == 0.0f && dy == 0.0f) || !camera || !look) {
        return steered ? steered : scale_stick(stick);
    }
    game_invert(u3_invert_flags(camera), &dx, &dy);
    const float per_count = look_scale();
    if (dx != 0.0f) {
        const float angle = -dx * per_count;
        const float c = cosf(angle), s = sinf(angle);
        const float x = look[0], z = look[2];
        look[0] = x * c + z * s;
        look[2] = z * c - x * s;
    }
    if (dy != 0.0f) {
        const float *f = (const float *)camera;
        const float yaw_full = f[0x7578 / 4] * f[0x7604 / 4] * 0.017453292f; /* rad/s at full stick */
        const float pitch_full = f[0x7588 / 4] * f[0x7608 / 4];             /* curve units/s */
        if (yaw_full > 1e-4f && pitch_full > 0.0f && isfinite(yaw_full) && isfinite(pitch_full)) {
            const float delta = dy * per_count * look_invert() * (pitch_full / yaw_full);
            float *pitch = (float *)(camera + 0x7874);
            const U3PitchStep step = (U3PitchStep)(image + 0x100ef90);
            const float next = step(camera + 0x7750, *pitch, delta);
            if (isfinite(next)) {
                *pitch = next;
            }
        }
    }
    return steered ? steered : scale_stick(stick);
}

/* Yaw about +Y, shared by Uncharted 2 and 3: the same angle convention as their stick path
 * (positive stick X = negative angle = turning right). */
static void rotate_look(float *look, float dx, float per_count) {
    const float angle = -dx * per_count;
    const float c = cosf(angle), s = sinf(angle);
    const float x = look[0], z = look[2];
    look[0] = x * c + z * s;
    look[2] = z * c - x * s;
}

/* Uncharted 2 (1.02), camera input at 0x428f40 with the same arguments and math as Uncharted 3's,
 * other offsets: tuning at *(camera + 0x8d00) (yaw ramp max +0x70, pitch speed +0x80), yaw scale
 * camera + 0x9fa8, pitch scale + 0x9fac, pitch at + 0x9e60 stepped by 0x4288d0(camera, delta),
 * which returns the new pitch. */
typedef ABI float (*U2PitchStep)(void *camera, float delta);
static ABI void *u2_camera_input(unsigned char *camera, float *look, void *stick) {
    if (look_debug()) stick_debug("U2", look, stick);
    float dx = 0, dy = 0;
    char mode = '-';
    const int took = frame_take(&u2_frame, camera, &dx, &dy, &mode);
    const float *tune = camera ? *(const float *const *)(camera + 0x8d00) : NULL;
    if (mode != 's' && tune) {
        const float full_rate = tune[0x70 / 4] * *(const float *)(camera + 0x9fa8) * 0.017453292f;
        steer_update(&u2_steer, took ? dx : 0.0f, look_scale(), *(const float *)(image + 0x111630c), full_rate);
    }
    if (camera) {
        trace_extra[0] = *(const float *)(camera + 0x9fa0);
        trace_extra[1] = u2_steer.x;
    }
    mouse_trace("u2f", camera, 1, dir_heading(look), camera ? *(const float *)(camera + 0x9e60) : 0.0f, dx, dy, mode);
    void *const steered = steer_camera(&u2_steer_fields, &u2_steer, camera, stick);
    mouse_steering = steering_now(&u2_steer);
    if (!took || (dx == 0.0f && dy == 0.0f) || !camera || !look) {
        return steered ? steered : scale_stick(stick);
    }
    game_invert((const int *)(image + 0x105de9c), &dx, &dy);
    const float per_count = look_scale();
    if (dx != 0.0f) {
        rotate_look(look, dx, per_count);
    }
    if (dy != 0.0f && tune) {
        const float *f = (const float *)camera;
        const float yaw_full = tune[0x70 / 4] * f[0x9fa8 / 4] * 0.017453292f;
        const float pitch_full = tune[0x80 / 4] * f[0x9fac / 4];
        if (yaw_full > 1e-4f && pitch_full > 0.0f && isfinite(yaw_full) && isfinite(pitch_full)) {
            const float delta = dy * per_count * look_invert() * (pitch_full / yaw_full);
            const U2PitchStep step = (U2PitchStep)(image + 0x4288d0);
            const float next = step(camera, delta);
            if (isfinite(next)) {
                *(float *)(camera + 0x9e60) = next;
            }
        }
    }
    return steered ? steered : scale_stick(stick);
}

/* Uncharted 1 (1.02): the cameras' input step is a virtual (vtable +0x98) called from 0xb62a5
 * with params: +0x10 the pad input (floats: left x, left y, right x, right y; flags +0x30). The
 * orbit camera, 0xb7430, turns by right x * factor * the game's camera speed (0xa4cb10) * 180
 * degrees per second and pitches by right y * factor * speed * -90 (factor 1, or 1.5 / 0.75 for
 * input flags 0x100 / 0x300), over the frame time at 0xa67000; the mouse adds the stick value
 * that turns exactly its angle this frame (no dead zone, response curve or speed cap; the game's
 * own camera speed cancels out). GOW3_MOUSE_LOOK_DEBUG=1 logs the values once a second. */
static int look_debug(void) {
    static int debug = -1;
    if (debug < 0) {
        const char *env = getenv("GOW3_MOUSE_LOOK_DEBUG");
        debug = env && env[0] == '1';
    }
    return debug;
}
/* GOW3_MOUSE_LOOK_DEBUG=1: once a second the camera's heading (degrees about +Y from the look
 * direction at entry), how far it turned since the last line, and the right stick it reads. */
static void stick_debug(const char *game, const float *look, const float *stick) {
    static uint64_t last;
    static float last_heading;
    static unsigned calls;
    ++calls;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const uint64_t now = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
    if (now - last < 1000 || !look || !stick) return;
    const float heading = atan2f(look[0], look[2]) * 57.29578f;
    float turned = heading - last_heading;
    while (turned > 180.0f) turned -= 360.0f;
    while (turned < -180.0f) turned += 360.0f;
    printf("Camera debug: %s heading %.1f, turned %.1f over %u calls, right stick %.2f %.2f, speed %.2f\n", game,
           heading, last ? turned : 0.0f, calls, stick[2], stick[3], gow3gpu_controls_stick_speed());
    fflush(stdout);
    last = now;
    last_heading = heading;
    calls = 0;
}
static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
static void u1_debug(const char *site, const unsigned char *params, float dx, float dy) {
    static uint64_t last[2];
    static unsigned calls[2];
    const int i = site[0] == 'o' ? 0 : 1;
    ++calls[i];
    const uint64_t now = now_ms();
    if (now - last[i] < 1000 || !params) return;
    last[i] = now;
    const float *input = *(const float *const *)(params + 0x10);
    printf("Mouse look debug: %s %u calls, params+0x18 %g, dt %g, speed %g", site, calls[i],
           *(const float *)(params + 0x18), *(const float *)(image + 0xa67000), *(const float *)(image + 0xa4cb10));
    if (input)
        printf(", input %g %g %g %g flags %#x", input[0], input[1], input[2], input[3],
               *(const unsigned *)((const unsigned char *)input + 0x30));
    printf(", mouse %g %g\n", dx, dy);
    fflush(stdout);
    calls[i] = 0;
}
/* gow3 24 Oct 2026: on by default; GOW3_MOUSE_LOOK_U1=0 makes the mouse the right stick again. */
static int u1_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *env = getenv("GOW3_MOUSE_LOOK_U1");
        enabled = !(env && env[0] == '0');
    }
    return enabled;
}
/* The orbit camera's stick-equivalent input is not verified in game: on only with
 * GOW3_MOUSE_LOOK_U1_ORBIT=1 (otherwise the mouse is the stick for that camera). */
static int u1_orbit_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *env = getenv("GOW3_MOUSE_LOOK_U1_ORBIT");
        enabled = u1_enabled() && env && env[0] == '1';
    }
    return enabled;
}
static ABI void *u1_orbit_camera_input(unsigned char *self, unsigned char *camera, unsigned char *params) {
    (void)self;
    (void)camera;
    if (!u1_orbit_enabled() && !look_debug()) return NULL;
    float dx = 0, dy = 0;
    /* Only the camera the player steers (params + 8, see u1_follow_camera_input). */
    const int took = u1_orbit_enabled() && params && params[0x08] ? take_look(&dx, &dy) : 0;
    if (look_debug()) u1_debug("orbit", params, dx, dy);
    if (!took || (dx == 0.0f && dy == 0.0f) || !params) return NULL;
    float *input = *(float **)(params + 0x10);
    if (!input) return NULL;
    const unsigned mode = *(const unsigned *)((const unsigned char *)input + 0x30) & 0x300;
    const float factor = mode == 0x100 ? 1.5f : mode == 0x300 ? 0.75f : 1.0f;
    const float speed = *(const float *)(image + 0xa4cb10), dt = *(const float *)(image + 0xa67000);
    if (!(speed > 1e-3f && speed < 100.0f) || !(dt > 1e-4f && dt < 0.5f)) return NULL;
    const float degrees = 57.29578f * look_scale();
    input[2] += dx * degrees / (factor * speed * 180.0f * dt);
    input[3] += dy * look_invert() * degrees / (factor * speed * 90.0f * dt);
    return NULL;
}


/* gow3 24 Oct 2026: Uncharted 1's direct mouse cameras. The cameras' updates (vtable +0x98, called
 * per camera of the blend by 0xb60d0) get (rdi) the location they return, (rsi) the camera and (rdx)
 * the params: +0x08 a byte, 1 for the newest camera of the blend (the one the player steers) and 0
 * for older ones still blending out, +0x10 the pad input, +0x18 the blend progress. (The first
 * follow hook read the camera from rdi, the returned location, so it saw zeros.) The mouse turns the
 * camera's own direction and height before its stick step: no ramp, dead zone or turn-speed cap, the
 * same angle per count as in Uncharted 2 and 3, the vertical step matched to the horizontal one
 * through the stick rates' ratio, with the game's invert options (ints at 0x95ca20 left/right,
 * 0x95ca24 up/down; its stick reads go through them too).
 * Whether the game kept a turn shows at the next call: a camera that puts its direction back gets
 * the stick for half a second after 3 such turns in a row. Not the newest camera, an unusable
 * direction or that fallback: no mouse taken, so runtime_pad.c makes the mouse the right stick
 * (sample_mouse: direct only while a hook takes the mouse at every pad read). */
typedef struct {
    const char *name;
    const unsigned char *camera; /* the camera the last checked turn went to (NULL: nothing to check) */
    float before, after;         /* its heading (radians) before and after that turn */
    unsigned lost;               /* turns in a row the game did not keep */
    uint64_t stick_until;        /* ms: the mouse is the stick until then */
    unsigned turns, kept, dropped, fallbacks; /* for the debug lines */
    double taken[2];             /* mouse counts turned, for the debug lines */
} U1Look;
static U1Look u1_follow = {.name = "follow"}, u1_cover = {.name = "cover"};
static float wrap_angle(float a) {
    while (a > 3.14159265f) a -= 6.28318531f;
    while (a < -3.14159265f) a += 6.28318531f;
    return a;
}
/* Whether the camera may take the mouse now; first checks the last turn against the heading the
 * camera has now (dropped: back near the heading before it, not near the one after it; the game's
 * own swing behind a walking Drake, up to ~0.3 degrees a frame, must not count as that). */
static int u1_look_ready(U1Look *s, const unsigned char *camera, float heading, uint64_t now) {
    if (s->camera == camera) {
        const float turn = fabsf(wrap_angle(s->after - s->before));
        const float from_before = fabsf(wrap_angle(heading - s->before));
        const float from_after = fabsf(wrap_angle(heading - s->after));
        if (from_before < 0.2f * turn && from_after > 0.8f * turn) {
            ++s->dropped;
            if (++s->lost >= 3) {
                s->lost = 0;
                s->stick_until = now + 500;
                ++s->fallbacks;
                if (look_debug()) {
                    printf("U1 %s debug: the game put the direction back 3 times: the mouse is the stick for 0.5 s\n",
                           s->name);
                    fflush(stdout);
                }
            }
        } else {
            ++s->kept;
            s->lost = 0;
        }
    }
    s->camera = NULL;
    return now >= s->stick_until;
}
/* The mouse movement for a direct camera (counts, inverted as the game's options say), 0: none. */
static int u1_take(U1Look *s, float *dx, float *dy) {
    if (!take_look(dx, dy) || (*dx == 0.0f && *dy == 0.0f)) return 0;
    s->taken[0] += *dx;
    s->taken[1] += *dy;
    game_invert((const int *)(image + 0x95ca20), dx, dy);
    return 1;
}
/* Turns the look direction (x, y, z, 0) about +Y and keeps the turn for the check at the next call
 * (only turns of 1 degree or more are checked: smaller ones are too close to the game's own swing). */
static void u1_turn(U1Look *s, unsigned char *camera, float *dir, float dx, float per_count) {
    const float before = atan2f(dir[0], dir[2]);
    rotate_look(dir, dx, per_count);
    ++s->turns;
    const float after = atan2f(dir[0], dir[2]);
    if (fabsf(wrap_angle(after - before)) >= 0.01745f) {
        s->camera = camera;
        s->before = before;
        s->after = after;
    }
}
static const char *u1_mode(const U1Look *s, uint64_t now) {
    return !u1_enabled() ? "stick (off)" : now < s->stick_until ? "stick (fallback)" : "direct";
}

/* The follow camera (CameraControlFollow2, the default third-person camera; 0xaff80 is the update of
 * it and of two cameras built on it, and other follow-type cameras call it). It looks along camera
 * + 0x1310 (x, y, z, 0) and sits at + 0x12bc on its vertical arc (0..1). Its stick step (vtable
 * +0x168, 0xaac90) turns the direction about +Y by -stickX * rate * scale(+0x13d0) * frame time
 * degrees, the rate (+0x12c0) ramping from tune[0x58] up to tune[0x5c] deg/s while the stick is held
 * (tune = *(camera + 0x1e0)), and moves along the arc with 0xaa7b0(camera, delta), delta =
 * tune[0x6c] * scale(+0x13d4) * frame time * stickY, which walks the arc's curve and keeps it inside
 * 0..1. */
typedef ABI float (*U1ArcStep)(void *camera, float delta);
/* A direction the mouse can turn: finite, about unit length and not steep (the horizontal part at
 * least half of it). */
static int u1_follow_usable(const float *dir) {
    const float horizontal = dir[0] * dir[0] + dir[2] * dir[2];
    const float length = horizontal + dir[1] * dir[1];
    return isfinite(length) && length > 0.81f && length < 1.21f && horizontal >= 0.25f;
}
/* The newest follow camera's turn in the current frame, for the older ones of the blend. */
static struct {
    int frame; /* the game's frame counter when it was taken, -1: none */
    const unsigned char *camera;
    float dx, dy, per_count;
} u1_frame_turn = {.frame = -1};
static unsigned u1_older_turns; /* for the debug line */
/* GOW3_MOUSE_LOOK_DEBUG=1: twice a second the newest follow camera's state (calls: all follow
 * camera updates, the newest and older ones blending out). */
static void u1_follow_debug(const unsigned char *camera, const unsigned char *params) {
    static uint64_t last;
    static float last_heading;
    static unsigned calls, top;
    ++calls;
    if (!params || !params[0x08]) return;
    ++top;
    const uint64_t now = now_ms();
    if (now - last < 500 || !camera) return;
    const float *d = (const float *)(camera + 0x1310), *f = (const float *)camera;
    const float *tune = *(const float *const *)(camera + 0x1e0);
    const float *input = *(const float *const *)(params + 0x10);
    const float heading = atan2f(d[0], d[2]) * 57.29578f;
    const U1Look *s = &u1_follow;
    printf("U1 follow debug: %u calls (%u newest), camera %p, heading %.1f, turned %.1f, dir %.3f %.3f %.3f, arc %.3f, "
           "stick %.2f %.2f, rate %.0f (%.0f..%.0f deg/s), scale %.2f %.2f, arc speed %.2f, dt %.4f, mouse %s, "
           "turns %u kept %u dropped %u fallbacks %u, counts %.0f %.0f, older turned %u\n",
           calls, top, (const void *)camera, heading,
           last ? wrap_angle((heading - last_heading) * 0.017453292f) * 57.29578f : 0.0f, d[0], d[1], d[2],
           f[0x12bc / 4], input ? input[2] : 0.0f, input ? input[3] : 0.0f, f[0x12c0 / 4],
           tune ? tune[0x58 / 4] : 0.0f, tune ? tune[0x5c / 4] : 0.0f, f[0x13d0 / 4], f[0x13d4 / 4],
           tune ? tune[0x6c / 4] : 0.0f, *(const float *)(image + 0xa4cb10), u1_mode(s, now), s->turns, s->kept,
           s->dropped, s->fallbacks, s->taken[0], s->taken[1], u1_older_turns);
    fflush(stdout);
    last = now;
    last_heading = heading;
    calls = top = 0;
}
/* The game's frame counter (int at +8 of the clock the cameras time their stick use with). */
static int u1_frame(void) {
    const int *clock = *(const int *const *)(image + 0xa4cb08);
    return clock ? clock[2] : -1;
}
/* gow3 1 Nov 2026: the steering notes the stick step 0xaac90 makes while the stick is not 0 (see
 * steer_camera for Uncharted 2 and 3): camera + 0x1348 = the clock (the follow camera swings behind
 * Drake only tune[0xc8] seconds after the last steering, 0xab7a9), and a stick past 0.3 ends the
 * look toward a point of interest (byte + 0x1321, direction + 0x1330). For every camera of the blend
 * the mouse turns, while the mouse turns (u1_steer). GOW3_MOUSE_LOOK_STEER=0: off. */
static int u1_steer_enabled(void);
static void u1_steer_notes(unsigned char *camera) {
    const float x = u1_steer.x;
    if (x == 0.0f || !camera || !u1_steer_enabled()) return;
    const int clock = u1_frame();
    if (clock < 0) return;
    *(int *)(camera + 0x1348) = clock;
    if (fabsf(x) > 0.3f) camera[0x1321] = 0;
}
/* Turns a follow camera: yaw about +Y, the arc through the game's step (its limits). */
static void u1_follow_turn(unsigned char *camera, float *dir, const float *tune, float dx, float dy, float per_count,
                           int newest) {
    if (dx != 0.0f) {
        if (newest) {
            u1_turn(&u1_follow, camera, dir, dx, per_count);
        } else {
            rotate_look(dir, dx, per_count);
        }
    }
    if (dy != 0.0f) {
        const float *f = (const float *)camera;
        const float yaw_full = tune[0x5c / 4] * f[0x13d0 / 4] * 0.017453292f; /* rad/s at full stick */
        const float arc_full = tune[0x6c / 4] * f[0x13d4 / 4];               /* arc units/s */
        if (yaw_full > 1e-4f && arc_full > 0.0f && isfinite(yaw_full) && isfinite(arc_full)) {
            const float delta = dy * per_count * look_invert() * (arc_full / yaw_full);
            const U1ArcStep step = (U1ArcStep)(image + 0xaa7b0);
            const float next = step(camera, delta);
            if (isfinite(next) && next >= 0.0f && next <= 1.0f) {
                *(float *)(camera + 0x12bc) = next;
            }
        }
    }
}
/* gow3 1 Nov 2026 (near walls): the follow update 0xaff80 keeps the direction it starts with (at
 * 0xb0024) and, after the stick step (vtable +0x168, 0xaac90, called at 0xb01fc), measures how far the
 * camera turned this frame (camera + 0x140c, at 0xb0805); its wall probes (0xac9d8) push a camera that is
 * not turning away from a wall (measured at a doorway: +2..3.5 degrees a frame against the mouse, the
 * stick turned through untouched). The mouse turned the direction at the update's entry, before that
 * start, so its turn never counted. Now the update's hook decides the turn and the stick step's hook
 * applies it, where the stick turns too; a turn the step did not take (an update without it) is applied
 * at the camera's next update. Not needed for the wall push (the step's result is, see u1_step_input; at
 * the doorway both gave the exact turn), so it is opt-in: GOW3_MOUSE_LOOK_U1_STEP=1. */
static int u1_step_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *env = getenv("GOW3_MOUSE_LOOK_U1_STEP");
        enabled = env && env[0] == '1';
    }
    return enabled;
}
static struct {
    unsigned char *camera;
    const float *tune;
    float dx, dy, per_count;
    int newest;
} u1_pending[4];
static unsigned u1_steps_applied, u1_late_applied; /* for the debug line */
static void u1_apply_pending(unsigned char *camera, int late) {
    for (unsigned i = 0; i < 4; ++i) {
        if (u1_pending[i].camera != camera) continue;
        u1_follow_turn(camera, (float *)(camera + 0x1310), u1_pending[i].tune, u1_pending[i].dx, u1_pending[i].dy,
                       u1_pending[i].per_count, u1_pending[i].newest);
        u1_pending[i].camera = NULL;
        ++*(late ? &u1_late_applied : &u1_steps_applied);
    }
}
/* Turns a follow camera now, or at its stick step (u1_step_enabled). */
static void u1_queue_turn(unsigned char *camera, const float *tune, float dx, float dy, float per_count, int newest) {
    if (!u1_step_enabled()) {
        u1_follow_turn(camera, (float *)(camera + 0x1310), tune, dx, dy, per_count, newest);
        return;
    }
    unsigned slot = 0;
    for (unsigned i = 0; i < 4; ++i) {
        if (u1_pending[i].camera == camera || !u1_pending[i].camera) {
            slot = i;
            break;
        }
    }
    if (u1_pending[slot].camera && u1_pending[slot].camera != camera) {
        u1_apply_pending(u1_pending[slot].camera, 1); /* all slots taken: the oldest gets its turn now */
    }
    u1_pending[slot].camera = camera;
    u1_pending[slot].tune = tune;
    u1_pending[slot].dx = dx;
    u1_pending[slot].dy = dy;
    u1_pending[slot].per_count = per_count;
    u1_pending[slot].newest = newest;
}
/* gow3 1 Nov 2026: the stick step returns whether the player steers (stick not 0, 0xab88a); the update
 * hands that to 0xa96f0 (edx) and to the wall push step 0xaede0 (vtable +0x178, r9d), which does nothing
 * while the player steers (0xaf119) and otherwise turns the camera away from a wall (measured: 2..3.5
 * degrees a frame against the mouse at a doorway). The patch after the step's call (0xb0204,
 * runtime_mouselook_install_patches) ORs this in: 1 while the mouse turns the camera (u1_steer). */
static ABI void *u1_step_input(unsigned char *camera, float *dir, void *input) {
    (void)dir;
    (void)input;
    if (camera) u1_apply_pending(camera, 0);
    mouse_steering = u1_steer_enabled() && steering_now(&u1_steer);
    return NULL;
}
/* gow3 1 Nov 2026 (user: Uncharted 1's camera feels jerky at a steady frame rate): GOW3_U1_CAMERA_TRACE=<file>,
 * one line per update of the camera the player steers: host time (us), the game's frame counter,
 * its frame time (dt), the heading (degrees). With the stick held at a constant angle, an even turn
 * gives the same heading step per unit of host time; a jerk shows as uneven steps. A test aid. */
static void u1_camera_trace(const unsigned char *camera, const unsigned char *params) {
    static FILE *file;
    static int state = -1;
    if (state < 0) {
        const char *path = getenv("GOW3_U1_CAMERA_TRACE");
        file = path && path[0] ? fopen(path, "w") : NULL;
        if (file) setvbuf(file, NULL, _IOFBF, 1 << 16);
        state = file != NULL;
    }
    if (!state || !camera || !params || !params[0x08]) return;
    const float *d = (const float *)(camera + 0x1310);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const float *input = *(const float *const *)(params + 0x10);
    fprintf(file, "%llu %d %.6f %.4f %.3f\n",
            (unsigned long long)ts.tv_sec * 1000000ull + (unsigned long long)ts.tv_nsec / 1000ull, u1_frame(),
            *(const float *)(image + 0xa4cb10), atan2f(d[0], d[2]) * 57.29578f, input ? input[2] : 0.0f);
    static unsigned lines;
    if (++lines % 256 == 0) fflush(file);
}
static ABI void *u1_follow_camera_input(unsigned char *location, unsigned char *camera, unsigned char *params) {
    (void)location;
    if (look_debug()) u1_follow_debug(camera, params);
    u1_camera_trace(camera, params);
    if (!u1_enabled() || !camera || !params) return NULL;
    u1_apply_pending(camera, 1); /* a turn its last stick step did not take */
    float *dir = (float *)(camera + 0x1310);
    const float *tune = *(const float *const *)(camera + 0x1e0);
    const int usable = tune && u1_follow_usable(dir);
    const float heading = dir_heading(dir), arc = *(const float *)(camera + 0x12bc);
    if (!params[0x08]) {
        /* An older camera still blending out: the newest camera's turn of this frame. The stick turns
         * every camera of the blend (they get the same pad input), so the view does not jump back
         * when the game returns to this one (standing <-> moving follow cameras while walking). */
        if (usable && u1_frame_turn.frame == u1_frame() && u1_frame_turn.camera != camera) {
            u1_queue_turn(camera, tune, u1_frame_turn.dx, u1_frame_turn.dy, u1_frame_turn.per_count, 0);
            u1_steer_notes(camera);
            ++u1_older_turns;
            mouse_trace("u1f", camera, 0, heading, arc, u1_frame_turn.dx, u1_frame_turn.dy, 's');
        } else {
            mouse_trace("u1f", camera, 0, heading, arc, 0.0f, 0.0f, '-');
        }
        return NULL;
    }
    u1_frame_turn.frame = -1;
    if (!usable) {
        u1_follow.camera = NULL;
        mouse_trace("u1f", camera, 1, heading, arc, 0.0f, 0.0f, 'u');
        return NULL;
    }
    float dx = 0, dy = 0;
    const float per_count = look_scale();
    /* The mouse's yaw rate for the stick reads after the turn (u1_stick_x_read). */
    const float yaw_full = tune[0x5c / 4] * *(const float *)(camera + 0x13d0) * 0.017453292f;
    const float frame_time = *(const float *)(image + 0xa4cb10);
    if (!u1_look_ready(&u1_follow, camera, atan2f(dir[0], dir[2]), now_ms())) {
        steer_update(&u1_steer, 0.0f, per_count, frame_time, yaw_full);
        mouse_trace("u1f", camera, 1, heading, arc, 0.0f, 0.0f, 'f');
        return NULL;
    }
    const int took = u1_take(&u1_follow, &dx, &dy);
    steer_update(&u1_steer, took ? dx : 0.0f, per_count, frame_time, yaw_full);
    u1_steer_notes(camera);
    {
        const float *input = *(const float *const *)(params + 0x10);
        trace_extra[0] = input ? input[2] : 0.0f; /* the right stick X the step will read */
    }
    trace_extra[1] = *(const float *)(camera + 0x12c0); /* the step's turn rate (ramps while held) */
    if (!took) {
        mouse_trace("u1f", camera, 1, heading, arc, dx, dy, '-');
        return NULL;
    }
    mouse_trace("u1f", camera, 1, heading, arc, dx, dy, 'd');
    u1_queue_turn(camera, tune, dx, dy, per_count, 1);
    u1_frame_turn.frame = u1_frame();
    u1_frame_turn.camera = camera;
    u1_frame_turn.dx = dx;
    u1_frame_turn.dy = dy;
    u1_frame_turn.per_count = per_count;
    return NULL;
}

/* gow3 1 Nov 2026 (near walls, measured in Uncharted 1 at a doorway: a mouse turn toward the wall was
 * pushed back by ~80% each frame, the kept check then gave the mouse to the stick (0.7 deg a frame),
 * while the stick itself turned through at full speed): after its stick step the follow camera's update
 * 0xaff80 reads the right stick X again through 0xb1b60 (the invert option applied), at 0xb0219 (|x| >=
 * 0.7 held: a wall timer, camera + 0x1448) and 0xb08e5 (|x| > 0.2: the camera pulls in front of a wall
 * at once instead of turning against it). The same rules as Uncharted 2 and 3 (steer_camera above).
 * Those two calls go to u1_stick_x_read, which gives the stick X or, when larger, the deflection that
 * turns as fast as the mouse (u1_steer, the newest follow camera's yaw rate); the stick step itself
 * (0xaacd0) and every other reader keep the plain stick. GOW3_MOUSE_LOOK_STEER=0 (or _U1=0): off. */
static ABI float u1_stick_x_read(const float *input) {
    float x = input ? input[2] : 0.0f;
    if (*(const int *)(image + 0x95ca20)) x = -x;
    const float phantom = u1_steer.x;
    if (phantom != 0.0f && now_ms() - u1_steer.at < 100 && fabsf(phantom) > fabsf(x)) x = phantom;
    return x;
}
/* A call redirect: "call 0xb1b60" (E8 rel32) at a call site becomes a call to a stub next to the image
 * that keeps every register 0xb1b60 keeps (all but rax and xmm0), calls the handler (SysV, rdi as
 * given) and returns its float in xmm0. */
static unsigned build_call_stub(unsigned char *p, void *handler) {
    unsigned n = 0;
    static const unsigned char push[] = {0x51, 0x52, 0x56, 0x57, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53};
    memcpy(p + n, push, sizeof(push)); /* push rcx rdx rsi rdi r8 r9 r10 r11 */
    n += sizeof(push);
    /* sub rsp, 0xf8 (entry rsp = 8 mod 16, + 8 pushes, + 0xf8 = 0 mod 16 at the call) */
    p[n++] = 0x48; p[n++] = 0x81; p[n++] = 0xec; p[n++] = 0xf8; p[n++] = 0x00; p[n++] = 0x00; p[n++] = 0x00;
    for (unsigned r = 1; r < 16; ++r) { /* movdqu [rsp + (r-1)*16], xmm r */
        p[n++] = 0xf3;
        if (r >= 8) p[n++] = 0x44;
        p[n++] = 0x0f; p[n++] = 0x7f; p[n++] = (unsigned char)(0x84 | ((r & 7) << 3)); p[n++] = 0x24;
        const uint32_t off = (r - 1) * 16;
        memcpy(p + n, &off, 4);
        n += 4;
    }
    p[n++] = 0x48; p[n++] = 0xb8; /* mov rax, handler */
    const uint64_t target = (uint64_t)(uintptr_t)handler;
    memcpy(p + n, &target, 8);
    n += 8;
    p[n++] = 0xff; p[n++] = 0xd0; /* call rax */
    for (unsigned r = 1; r < 16; ++r) { /* movdqu xmm r, [rsp + (r-1)*16] */
        p[n++] = 0xf3;
        if (r >= 8) p[n++] = 0x44;
        p[n++] = 0x0f; p[n++] = 0x6f; p[n++] = (unsigned char)(0x84 | ((r & 7) << 3)); p[n++] = 0x24;
        const uint32_t off = (r - 1) * 16;
        memcpy(p + n, &off, 4);
        n += 4;
    }
    p[n++] = 0x48; p[n++] = 0x81; p[n++] = 0xc4; p[n++] = 0xf8; p[n++] = 0x00; p[n++] = 0x00; p[n++] = 0x00;
    static const unsigned char pop[] = {0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5f, 0x5e, 0x5a, 0x59};
    memcpy(p + n, pop, sizeof(pop)); /* pop r11 r10 r9 r8 rdi rsi rdx rcx */
    n += sizeof(pop);
    p[n++] = 0xc3; /* ret */
    return n;
}
unsigned char *runtime_near_image_alloc(size_t bytes, int executable); /* probe.c */
static int u1_steer_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) enabled = u1_enabled() && steer_enabled();
    return enabled;
}
/* A jump from image + at (length bytes, whole instructions) to code at p; the rest becomes nops. */
static int patch_jump(uint64_t at, unsigned length, const unsigned char *p) {
    const int64_t delta = (int64_t)(uintptr_t)p - (int64_t)(uintptr_t)(image + at + 5);
    if (delta < INT32_MIN || delta > INT32_MAX || length < 5) return 0;
    const int32_t rel = (int32_t)delta;
    image[at] = 0xe9;
    memcpy(image + at + 1, &rel, 4);
    memset(image + at + 5, 0x90, length - 5);
    return 1;
}
/* "jmp [rip]; dq image + resume" */
static unsigned emit_jump_back(unsigned char *p, uint64_t resume) {
    static const unsigned char jmp[] = {0xff, 0x25, 0x00, 0x00, 0x00, 0x00};
    memcpy(p, jmp, sizeof(jmp));
    const uint64_t target = (uint64_t)(uintptr_t)(image + resume);
    memcpy(p + sizeof(jmp), &target, 8);
    return sizeof(jmp) + 8;
}
/* Uncharted 2 and 3: the follow camera's update stores the input function's "the player steers" result
 * (al) in the camera (U2 0x430a96: mov [rbx + 0x271], al, read by the update 0x42d940 for its wall rules,
 * which note the steering time + 0x9e98 in 0x42c690; U3 0x4ec5c3: mov [rbx + 0x2c1], al). The store gets
 * "or al, [mouse_steering]" in front. */
static int patch_store_al(unsigned char *p, uint64_t at, uint32_t field) {
    const unsigned char store[] = {0x88, 0x83, (unsigned char)field, (unsigned char)(field >> 8), 0x00, 0x00};
    if (memcmp(image + at, store, sizeof(store))) return 0;
    unsigned n = 0;
    p[n++] = 0x51;                /* push rcx */
    p[n++] = 0x48; p[n++] = 0xb9; /* mov rcx, &mouse_steering */
    const uint64_t flag = (uint64_t)(uintptr_t)&mouse_steering;
    memcpy(p + n, &flag, 8);
    n += 8;
    p[n++] = 0x0a; p[n++] = 0x01; /* or al, [rcx] */
    p[n++] = 0x59;                /* pop rcx */
    memcpy(p + n, store, sizeof(store)); /* mov [rbx + field], al */
    n += sizeof(store);
    emit_jump_back(p + n, at + sizeof(store));
    return patch_jump(at, sizeof(store), p);
}
/* Uncharted 1: the two stick reads after the follow camera's stick step (u1_stick_x_read) and the step's
 * "steering" result handed on at 0xb0204 (movzx edx, al; mov rdi, r13) with mouse_steering ORed in. */
static void install_u1_patches(unsigned char *stub) {
    /* 0xb1b60: lea rax, [rip + 0x8a66a1]; cmp dword [rax + 0x4818], 0; vmovss xmm0, [rdi + 8] ... (1.02) */
    static const unsigned char read_x[] = {0x48, 0x8d, 0x05, 0xa1, 0x66, 0x8a, 0x00, 0x83, 0xb8, 0x18,
                                           0x48, 0x00, 0x00, 0x00, 0xc5, 0xfa, 0x10, 0x47, 0x08};
    static const uint64_t calls[] = {0xb0219, 0xb08e5};
    static const unsigned char after_step[] = {0x0f, 0xb6, 0xd0, 0x4c, 0x89, 0xef};
    const uint64_t at = 0xb0204;
    if (memcmp(image + 0xb1b60, read_x, sizeof(read_x)) || memcmp(image + at, after_step, sizeof(after_step))) return;
    for (unsigned i = 0; i < sizeof(calls) / sizeof(calls[0]); ++i) {
        int32_t rel;
        memcpy(&rel, image + calls[i] + 1, 4);
        if (image[calls[i]] != 0xe8 || calls[i] + 5 + (int64_t)rel != 0xb1b60) return;
        const int64_t delta = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + calls[i] + 5);
        if (delta < INT32_MIN || delta > INT32_MAX) return;
    }
    build_call_stub(stub, (void *)u1_stick_x_read);
    for (unsigned i = 0; i < sizeof(calls) / sizeof(calls[0]); ++i) {
        const int32_t rel = (int32_t)((int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + calls[i] + 5));
        memcpy(image + calls[i] + 1, &rel, 4);
    }
    unsigned char *p = stub + 1024;
    unsigned n = 0;
    p[n++] = 0x0f; p[n++] = 0xb6; p[n++] = 0xd0; /* movzx edx, al */
    p[n++] = 0x50;                               /* push rax */
    p[n++] = 0x48; p[n++] = 0xb8;                /* mov rax, &mouse_steering */
    const uint64_t flag = (uint64_t)(uintptr_t)&mouse_steering;
    memcpy(p + n, &flag, 8);
    n += 8;
    p[n++] = 0x0b; p[n++] = 0x10;                /* or edx, [rax] */
    p[n++] = 0x58;                               /* pop rax */
    p[n++] = 0x4c; p[n++] = 0x89; p[n++] = 0xef; /* mov rdi, r13 */
    emit_jump_back(p + n, at + sizeof(after_step));
    const int steer = patch_jump(at, sizeof(after_step), p);
    printf("Mouse look: Uncharted 1 follow camera sees mouse turns as steering (stick reads 0xb0219 0xb08e5%s)\n",
           steer ? ", step result 0xb0204" : "; step result patch unavailable");
}
void runtime_mouselook_install_patches(void) {
    if (!image || !steer_enabled()) return;
    static const unsigned char u1_step_result[] = {0x0f, 0xb6, 0xd0, 0x4c, 0x89, 0xef};
    static const unsigned char u2_store[] = {0x88, 0x83, 0x71, 0x02, 0x00, 0x00};
    static const unsigned char u3_store[] = {0x88, 0x83, 0xc1, 0x02, 0x00, 0x00};
    const int u1 = u1_steer_enabled() && !memcmp(image + 0xb0204, u1_step_result, sizeof(u1_step_result));
    const int u2 = !u1 && !memcmp(image + 0x430a96, u2_store, sizeof(u2_store));
    const int u3 = !u1 && !u2 && !memcmp(image + 0x4ec5c3, u3_store, sizeof(u3_store));
    if (!u1 && !u2 && !u3) return;
    unsigned char *stub = runtime_near_image_alloc(4096, 1);
    if (!stub) {
        puts("Mouse look: steering patches unavailable (no stub page near the image)");
        return;
    }
    if (u1) install_u1_patches(stub);
    if (u2)
        printf("Mouse look: Uncharted 2 follow cameras see mouse turns as steering (0x430a96 %s)\n",
               patch_store_al(stub, 0x430a96, 0x271) ? "patched" : "unavailable");
    if (u3)
        printf("Mouse look: Uncharted 3 follow cameras see mouse turns as steering (0x4ec5c3 %s)\n",
               patch_store_al(stub, 0x4ec5c3, 0x2c1) ? "patched" : "unavailable");
}

/* The cover camera (CameraControlCover, update 0xa4670; gow3 2 Nov 2026: on, the user tried it in game
 * with a real mouse, "u1 mouse is working"; GOW3_MOUSE_LOOK_U1_COVER=0: the stick emulation again).
 * Active while camera + 0x1f4 is set; tune = *(camera + 0x1300). Its
 * stick step (0xa3f40) turns the flat direction camera + 0x240 (x, 0, z, 0) about +Y by -stickX *
 * rate(+0x270, ramping tune[0x1c]..tune[0x20] deg/s) * frame time degrees once the frame counter
 * passes camera + 0x1e4 (the stick fades in until + 0x1e8), and lowers the pitch + 0x230 (degrees)
 * by tune[0x30] * frame time * stickY, kept inside -tune[0xc]..tune[8] (lower toward the wall, which
 * the game still applies after the mouse). */
static int u1_cover_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *env = getenv("GOW3_MOUSE_LOOK_U1_COVER");
        enabled = u1_enabled() && !(env && env[0] == '0');
    }
    return enabled;
}
static ABI void *u1_cover_camera_input(unsigned char *location, unsigned char *camera, unsigned char *params) {
    (void)location;
    if (!camera || !params || !camera[0x1f4]) return NULL;
    float *dir = (float *)(camera + 0x240);
    const float *tune = *(const float *const *)(camera + 0x1300);
    const uint64_t now = now_ms();
    if (look_debug()) {
        static uint64_t last;
        if (now - last >= 500 && tune) {
            last = now;
            printf("U1 cover debug: newest %d, heading %.1f, dir %.3f %.3f %.3f, pitch %.1f (%.1f..%.1f), rate %.0f "
                   "(%.0f..%.0f deg/s), pitch speed %.0f, mouse %s, turns %u kept %u dropped %u, counts %.0f %.0f\n",
                   params[0x08], atan2f(dir[0], dir[2]) * 57.29578f, dir[0], dir[1], dir[2],
                   *(const float *)(camera + 0x230), -tune[0xc / 4], tune[0x8 / 4], *(const float *)(camera + 0x270),
                   tune[0x1c / 4], tune[0x20 / 4], tune[0x30 / 4],
                   u1_cover_enabled() ? u1_mode(&u1_cover, now) : "stick (off)", u1_cover.turns, u1_cover.kept,
                   u1_cover.dropped, u1_cover.taken[0], u1_cover.taken[1]);
            fflush(stdout);
        }
    }
    const float heading = dir_heading(dir), pitch_in = *(const float *)(camera + 0x230);
    if (!u1_cover_enabled() || !params[0x08] || !tune) {
        mouse_trace("u1c", camera, params[0x08], heading, pitch_in, 0.0f, 0.0f, 'o');
        return NULL;
    }
    const float horizontal = dir[0] * dir[0] + dir[2] * dir[2];
    if (!isfinite(horizontal) || horizontal < 0.81f || horizontal > 1.21f) {
        u1_cover.camera = NULL;
        mouse_trace("u1c", camera, 1, heading, pitch_in, 0.0f, 0.0f, 'u');
        return NULL;
    }
    float dx = 0, dy = 0;
    if (!u1_look_ready(&u1_cover, camera, atan2f(dir[0], dir[2]), now)) {
        mouse_trace("u1c", camera, 1, heading, pitch_in, 0.0f, 0.0f, 'f');
        return NULL;
    }
    if (!u1_take(&u1_cover, &dx, &dy)) {
        mouse_trace("u1c", camera, 1, heading, pitch_in, 0.0f, 0.0f, '-');
        return NULL;
    }
    /* Still entering cover (the game ignores the stick then): the movement is dropped. */
    const int *clock = *(const int *const *)(image + 0xa4cb08);
    if (clock && clock[2] <= *(const int *)(camera + 0x1e4)) {
        mouse_trace("u1c", camera, 1, heading, pitch_in, dx, dy, 'e');
        return NULL;
    }
    mouse_trace("u1c", camera, 1, heading, pitch_in, dx, dy, 'd');
    const float per_count = look_scale();
    if (dx != 0.0f) u1_turn(&u1_cover, camera, dir, dx, per_count);
    const float yaw_full = tune[0x20 / 4] * 0.017453292f, pitch_full = tune[0x30 / 4];
    if (dy != 0.0f && yaw_full > 1e-4f && pitch_full > 0.0f && isfinite(yaw_full) && isfinite(pitch_full)) {
        float *pitch = (float *)(camera + 0x230);
        float next = *pitch - dy * per_count * look_invert() * (pitch_full / yaw_full);
        if (next < -tune[0xc / 4]) next = -tune[0xc / 4];
        if (next > tune[0x8 / 4]) next = tune[0x8 / 4];
        if (isfinite(next)) *pitch = next;
    }
    return NULL;
}

/* The jeep's mounted gun (CameraControlRailVehicle, GitHub #250; the camera of the rail vehicle chases,
 * the gun fires where it looks). Its update (0x2a7530, vtable 0x92ebd0 + 0x98) calls the look step
 * 0x2a9690(camera, look, pad input): look + 4 the yaw and + 8 the pitch (radians), each turned by
 * -stick * rate * the game's camera speed (0xa4cb10) degrees past a dead zone (right x: rate tune[0x20],
 * dead zone tune[0x44]; right y: tune[0x1c], tune[0x40]; tune = *(camera + 0x1e8 + facing * 8), facing
 * camera + 0x204), with their own invert options (ints at 0x95ca28 left/right, 0x95ca2c up/down), then
 * kept inside tune[0x2c]..tune[0x30] (yaw) and tune[0x24]..tune[0x28] (pitch) degrees. The mouse turns
 * the yaw and pitch at the step's entry, the same angle per count as the other cameras (pitch through
 * the rates' ratio); the game's limits still apply after it. Not verified in game yet;
 * GOW3_MOUSE_LOOK_U1_RAIL=0: the mouse is the stick there again. */
static int u1_rail_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *env = getenv("GOW3_MOUSE_LOOK_U1_RAIL");
        enabled = u1_enabled() && !(env && env[0] == '0');
    }
    return enabled;
}
static FrameTurn u1_rail_frame;
static ABI void *u1_rail_look_input(unsigned char *camera, unsigned char *look, void *input) {
    (void)input;
    if (!camera || !look) return NULL;
    float *yaw = (float *)(look + 4), *pitch = (float *)(look + 8);
    const unsigned facing = *(const unsigned *)(camera + 0x204);
    const float *tune = facing < 4 ? *(const float *const *)(camera + 0x1e8 + facing * 8) : NULL;
    const float heading = *yaw * 57.29578f, pitch_in = *pitch * 57.29578f;
    if (look_debug()) {
        static uint64_t last;
        const uint64_t now = now_ms();
        if (now - last >= 500) {
            last = now;
            printf("U1 rail debug: facing %u, yaw %.1f, pitch %.1f, rates %g %g, mouse %s\n", facing, heading, pitch_in,
                   tune ? tune[0x20 / 4] : 0.0f, tune ? tune[0x1c / 4] : 0.0f, u1_rail_enabled() ? "direct" : "stick (off)");
            fflush(stdout);
        }
    }
    if (!u1_rail_enabled() || !tune) {
        mouse_trace("u1r", camera, 1, heading, pitch_in, 0.0f, 0.0f, 'o');
        return NULL;
    }
    if (!isfinite(*yaw) || !isfinite(*pitch) || fabsf(*yaw) > 7.0f || fabsf(*pitch) > 3.2f) {
        mouse_trace("u1r", camera, 1, heading, pitch_in, 0.0f, 0.0f, 'u');
        return NULL;
    }
    float dx = 0, dy = 0;
    char mode = '-';
    const int took = frame_take(&u1_rail_frame, camera, &dx, &dy, &mode);
    mouse_trace("u1r", camera, 1, heading, pitch_in, dx, dy, mode);
    if (!took || (dx == 0.0f && dy == 0.0f)) return NULL;
    game_invert((const int *)(image + 0x95ca28), &dx, &dy);
    const float per_count = look_scale();
    const float yaw_rate = tune[0x20 / 4], pitch_rate = tune[0x1c / 4];
    const float ratio = isfinite(yaw_rate) && isfinite(pitch_rate) && yaw_rate > 1e-3f && pitch_rate > 0.0f
                            ? pitch_rate / yaw_rate : 1.0f;
    if (dx != 0.0f) *yaw -= dx * per_count;
    if (dy != 0.0f) *pitch -= dy * per_count * look_invert() * ratio;
    return NULL;
}

/* gow3 24 Oct 2026: direct mouse aim and cover cameras in Uncharted 2 and 3. While the aim button is
 * held the games turn their aim (zoom) cameras by the right stick through a shared aim function
 * (U3 0x9285c0, U2 0x71a1b0) with a dead zone, a response curve and a turn rate that ramps up while
 * the stick is held: the mouse as that stick was slow and accelerated. The aim camera's update (U3
 * 0x511370, U2 0x450e40; rdi the camera, rsi the input) keeps its aim direction (x, y, z, 0; U3
 * camera + 0x11e0, U2 + 0x1f30) from frame to frame and only adds the stick's turn to it, so the
 * mouse turns that direction before the update runs, the same angle per count as the follow camera
 * (with the aim sensitivity on top): yaw about +Y, pitch kept inside the game's aim limits (degrees;
 * U3 per player at 0x17f97a0 + player * 64: -[+8]..[+0xc], player = camera + 0x1e0; U2 globals
 * -[0x104eb18]..[0x104eb14]). The aim function flips the stick by the games' aim invert options (U3
 * 0x17b6368 + player * 4 + 0x521c / 0x5224, U2 ints at 0x105dea4 / 0x105dea8), so the mouse does too.
 * The cover cameras turn their flat direction (U3 + 0x2f0, U2 + 0x1c0) by the stick with a ramp too,
 * and their pitch (degrees; U3 + 0x2e0 inside -[+0x21c]..[+0x218]; U2 + 0x1b0 inside -tune[0xc]..
 * tune[8], tune = *(camera + 0x1290)); the mouse turns both the same way.
 * A turn the game puts back (a camera that sets its direction itself, like the aim camera while it
 * zooms in) shows at the next call: 3 in a row give the stick for half a second (U1Look above).
 * Knobs (each also needs GOW3_MOUSE_LOOK on): GOW3_MOUSE_LOOK_U3_AIM, _U2_AIM (on: verified 24 Oct, the
 * same angle per count at slow and fast mouse movement, 63 degrees per 1000 counts at sensitivity 1;
 * =0 the stick again), _U3_COVER, _U2_COVER (gow3 1 Nov 2026: on, verified in game with injected mouse:
 * the same angle per count both ways, the pitch field followed (U2 -66 / +65 degrees for 1048 / 1036
 * counts, U3 -66.7 / +65.6 for 1058 / 1041), nothing put back while idle; =0 the stick again). */
static int knob(const char *name, int fallback) {
    const char *env = getenv(name);
    return env && env[0] ? env[0] != '0' : fallback;
}
static int u3_aim_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) enabled = knob("GOW3_MOUSE_LOOK_U3_AIM", 1);
    return enabled;
}
static int u2_aim_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) enabled = knob("GOW3_MOUSE_LOOK_U2_AIM", 1);
    return enabled;
}
static int u3_cover_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) enabled = knob("GOW3_MOUSE_LOOK_U3_COVER", 1);
    return enabled;
}
static int u2_cover_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) enabled = knob("GOW3_MOUSE_LOOK_U2_COVER", 1);
    return enabled;
}
static U1Look u3_aim = {.name = "U3 aim"}, u2_aim = {.name = "U2 aim"};
static U1Look u3_cover_look = {.name = "U3 cover"}, u2_cover_look = {.name = "U2 cover"};
/* Whether a camera may take the mouse now (u1_look_ready for one camera at a time: a call for another
 * camera, one blending out, leaves the pending check alone). */
static int look_ready(U1Look *s, const unsigned char *camera, float heading, uint64_t now) {
    if (s->camera && s->camera != camera) return now >= s->stick_until;
    return u1_look_ready(s, camera, heading, now);
}
/* A direction the mouse can turn: finite, about unit length and not steeper than ~78 degrees. */
static int aim_usable(const float *dir) {
    const float horizontal = dir[0] * dir[0] + dir[2] * dir[2];
    const float length = horizontal + dir[1] * dir[1];
    return isfinite(length) && length > 0.81f && length < 1.21f && horizontal >= 0.04f;
}
/* Pitch limits in degrees, from the game when they look sane. */
static void aim_limits(float lo, float hi, float *out_lo, float *out_hi) {
    *out_lo = isfinite(lo) && lo <= -1.0f && lo >= -89.0f ? lo : -80.0f;
    *out_hi = isfinite(hi) && hi >= 1.0f && hi <= 89.0f ? hi : 80.0f;
}
/* Pitches a direction (x, y, z, 0) by -dy counts (mouse back = down), kept inside lo..hi degrees
 * (never pushed further out when it already is). */
static void pitch_dir(float *dir, float dy, float per_count, float lo, float hi) {
    const float length = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    const float horizontal = sqrtf(dir[0] * dir[0] + dir[2] * dir[2]);
    if (!(length > 0.0f) || !(horizontal > 1e-4f)) return;
    const float pitch = atan2f(dir[1], horizontal);
    float next = pitch - dy * per_count * look_invert();
    lo *= 0.017453292f;
    hi *= 0.017453292f;
    if (next > hi && next > pitch) next = pitch > hi ? pitch : hi;
    if (next < lo && next < pitch) next = pitch < lo ? pitch : lo;
    const float c = cosf(next) * length / horizontal;
    dir[0] *= c;
    dir[2] *= c;
    dir[1] = sinf(next) * length;
}
static const char *look_mode(int enabled, const U1Look *s, uint64_t now) {
    return !enabled ? "stick (off)" : now < s->stick_until ? "stick (fallback)" : "direct";
}
/* GOW3_MOUSE_LOOK_DEBUG=1: twice a second an aim or cover camera's heading and pitch (degrees), how far
 * it turned since the last line, the mouse counts taken for it and the right stick it reads. */
typedef struct {
    uint64_t last;
    float heading, pitch;
    unsigned calls, cameras;
    const unsigned char *camera;
} LookDebug;
static void look_debug_line(LookDebug *d, const U1Look *s, int enabled, const unsigned char *camera, const float *dir,
                            float extra, float lo, float hi, const float *stick, int aiming) {
    ++d->calls;
    if (camera != d->camera) ++d->cameras, d->camera = camera;
    const uint64_t now = now_ms();
    if (now - d->last < 500 || !dir) return;
    const float horizontal = sqrtf(dir[0] * dir[0] + dir[2] * dir[2]);
    const float heading = atan2f(dir[0], dir[2]) * 57.29578f, pitch = atan2f(dir[1], horizontal) * 57.29578f;
    printf("%s debug: %u calls (%u camera changes), camera %p, heading %.2f, pitch %.2f, turned %.2f %.2f, "
           "extra %.2f, limits %.1f..%.1f, stick %.2f %.2f, aiming %d, mouse %s, turns %u kept %u dropped %u "
           "fallbacks %u, counts %.0f %.0f\n",
           s->name, d->calls, d->cameras, (const void *)camera, heading, pitch,
           d->last ? wrap_angle((heading - d->heading) * 0.017453292f) * 57.29578f : 0.0f,
           d->last ? pitch - d->pitch : 0.0f, extra, lo, hi, stick ? stick[0] : 0.0f, stick ? stick[1] : 0.0f,
           aiming, look_mode(enabled, s, now), s->turns, s->kept, s->dropped, s->fallbacks, s->taken[0],
           s->taken[1]);
    fflush(stdout);
    d->last = now;
    d->heading = heading;
    d->pitch = pitch;
    d->calls = d->cameras = 0;
}
/* The mouse for an aim camera: only while the aim button is held (released, the follow camera takes
 * it), inverted as the game's aim options say. */
static int aim_take(U1Look *s, const int *flags, float *dx, float *dy) {
    if (!runtime_pad_aim_held() || !take_mouse(dx, dy) || (*dx == 0.0f && *dy == 0.0f)) return 0;
    s->taken[0] += *dx;
    s->taken[1] += *dy;
    game_invert(flags, dx, dy);
    return 1;
}
/* Turns an aim camera's direction (both games). */
static void aim_input(const char *hook, U1Look *s, int enabled, unsigned char *camera, float *dir, const int *flags,
                      float lo, float hi) {
    const float heading = dir_heading(dir), pitch = dir_pitch(dir);
    if (!enabled || !runtime_pad_aim_held()) {
        s->camera = NULL;
        mouse_trace(hook, camera, 1, heading, pitch, 0.0f, 0.0f, !enabled ? 'o' : 'r');
        return;
    }
    if (!aim_usable(dir)) {
        s->camera = NULL;
        mouse_trace(hook, camera, 1, heading, pitch, 0.0f, 0.0f, 'u');
        return;
    }
    float dx = 0, dy = 0;
    if (!look_ready(s, camera, atan2f(dir[0], dir[2]), now_ms())) {
        mouse_trace(hook, camera, 1, heading, pitch, 0.0f, 0.0f, 'f');
        return;
    }
    if (!aim_take(s, flags, &dx, &dy)) {
        mouse_trace(hook, camera, 1, heading, pitch, 0.0f, 0.0f, '-');
        return;
    }
    mouse_trace(hook, camera, 1, heading, pitch, dx, dy, 'd');
    const float per_count = look_scale();
    if (dx != 0.0f) u1_turn(s, camera, dir, dx, per_count);
    if (dy != 0.0f) pitch_dir(dir, dy, per_count, lo, hi);
}
static const int *u3_aim_invert_flags(int player) {
    static _Thread_local int flags[2];
    flags[0] = *(const int *)(image + 0x17b6368 + player * 4 + 0x521c);
    flags[1] = *(const int *)(image + 0x17b6368 + player * 4 + 0x5224);
    return flags;
}
static ABI void *u3_aim_camera_input(unsigned char *camera, unsigned char *params, void *unused) {
    (void)unused;
    if (!camera) return NULL;
    const int player = *(const int *)(camera + 0x1e0);
    const int valid = player >= 0 && player <= 3;
    const float *table = valid ? (const float *)(image + 0x17f97a0 + player * 64) : NULL;
    float lo, hi;
    aim_limits(table ? -table[2] : NAN, table ? table[3] : NAN, &lo, &hi);
    float *dir = (float *)(camera + 0x11e0);
    if (look_debug()) {
        static LookDebug d;
        look_debug_line(&d, &u3_aim, u3_aim_enabled(), camera, dir, (float)player, lo, hi,
                        params ? (const float *)(params + 0x5c) : NULL, runtime_pad_aim_held());
    }
    if (valid) aim_input("u3a", &u3_aim, u3_aim_enabled(), camera, dir, u3_aim_invert_flags(player), lo, hi);
    return NULL;
}
static ABI void *u2_aim_camera_input(unsigned char *camera, unsigned char *params, void *unused) {
    (void)unused;
    if (!camera) return NULL;
    float lo, hi;
    aim_limits(-*(const float *)(image + 0x104eb18), *(const float *)(image + 0x104eb14), &lo, &hi);
    float *dir = (float *)(camera + 0x1f30);
    if (look_debug()) {
        static LookDebug d;
        look_debug_line(&d, &u2_aim, u2_aim_enabled(), camera, dir, 0.0f, lo, hi,
                        params ? (const float *)(params + 0x5c) : NULL, runtime_pad_aim_held());
    }
    aim_input("u2a", &u2_aim, u2_aim_enabled(), camera, dir, (const int *)(image + 0x105dea4), lo, hi);
    return NULL;
}
/* gow3 25 Oct 2026: direct mouse aim in Uncharted 1. The aim camera's update calls the aim function
 * 0x276a10 (its only caller, at 0xc4c52) with the controller (rsi, camera + 0xa50) and the camera's
 * remembered aim direction (x, y, z; rcx, the 4th argument, which reaches the handler unchanged).
 * The function turns that direction by the right stick (dead zone, curve, a turn rate that ramps up:
 * the mouse as the stick was slow and accelerated), the camera keeps it, and aim assist and recoil go
 * on top of the shown direction. The mouse turns the remembered direction before the function runs,
 * the same angle per count as the other aim cameras (aim sensitivity on top), pitch inside +-70
 * degrees.
 * The game makes new aim cameras during one press (after a roll, about every 100 ms while Drake gets
 * up; two can take turns within a frame), and each has its own direction. So the press's mouse turn
 * is summed, and each controller remembers how much of it it already has: every camera gets each
 * turn once. A new controller that starts where the last turned direction ended (within 3 degrees)
 * already has the turns; one that starts elsewhere (the game's own direction after a roll) gets them
 * on top. The follow camera's invert options apply (U1 has no separate aim ones that we know of).
 * GOW3_MOUSE_LOOK_U1_AIM=0: the mouse is the stick while aiming again. */
static int u1_aim_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) enabled = knob("GOW3_MOUSE_LOOK_U1_AIM", 1);
    return enabled;
}
static U1Look u1_aim = {.name = "U1 aim"};
static struct {
    int held;                    /* aiming at the last call */
    float dx, dy;                /* this press's mouse turn so far (counts, game invert applied) */
    unsigned clock;              /* calls, for replacing the least recently used controller */
    struct {
        const unsigned char *controller;
        float dx, dy;            /* how much of the press's turn this controller's direction has */
        unsigned used;
    } given[4];
    float last[3];               /* the last turned direction */
    int have_last;
} u1_press;
static ABI void *u1_aim_input(unsigned char *out, unsigned char *controller, void *unused, float *dir) {
    (void)out;
    (void)unused;
    if (!u1_aim_enabled() || !controller || !dir) return NULL;
    if (!runtime_pad_aim_held()) {
        u1_press.held = 0;
        return NULL;
    }
    if (!u1_press.held) {
        memset(&u1_press, 0, sizeof(u1_press));
        u1_press.held = 1;
    }
    if (!aim_usable(dir)) return NULL;
    float dx = 0, dy = 0;
    const float heading = dir_heading(dir), pitch = dir_pitch(dir);
    if (aim_take(&u1_aim, (const int *)(image + 0x95ca20), &dx, &dy)) {
        u1_press.dx += dx;
        u1_press.dy += dy;
    }
    mouse_trace("u1a", controller, 1, heading, pitch, dx, dy, dx != 0.0f || dy != 0.0f ? 'd' : '-');
    int slot = 0, found = 0;
    for (int i = 0; i < 4; ++i) {
        if (u1_press.given[i].controller == controller) {
            slot = i;
            found = 1;
            break;
        }
        if (u1_press.given[i].used < u1_press.given[slot].used) slot = i;
    }
    if (!found) {
        const float *l = u1_press.last;
        const float dot = dir[0] * l[0] + dir[1] * l[1] + dir[2] * l[2];
        const float lengths = sqrtf((dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]) *
                                    (l[0] * l[0] + l[1] * l[1] + l[2] * l[2]));
        const int continues = u1_press.have_last && lengths > 0.0f && dot > 0.9986f * lengths;
        u1_press.given[slot].controller = controller;
        u1_press.given[slot].dx = continues ? u1_press.dx : 0.0f;
        u1_press.given[slot].dy = continues ? u1_press.dy : 0.0f;
    }
    u1_press.given[slot].used = ++u1_press.clock;
    const float turn_x = u1_press.dx - u1_press.given[slot].dx, turn_y = u1_press.dy - u1_press.given[slot].dy;
    const float per_count = look_scale();
    if (turn_x != 0.0f) rotate_look(dir, turn_x, per_count);
    if (turn_y != 0.0f) pitch_dir(dir, turn_y, per_count, -70.0f, 70.0f);
    u1_press.given[slot].dx = u1_press.dx;
    u1_press.given[slot].dy = u1_press.dy;
    memcpy(u1_press.last, dir, sizeof(u1_press.last));
    u1_press.have_last = 1;
    if (look_debug()) {
        static LookDebug d;
        look_debug_line(&d, &u1_aim, 1, controller, dir, (float)slot, -70.0f, 70.0f, NULL, 1);
    }
    return NULL;
}
/* A cover camera: flat direction dir (x, 0, z, 0) and pitch in degrees inside lo..hi; follow-camera
 * input (not while aiming), the follow camera's invert options. */
static void cover_input(const char *hook, FrameTurn *frame, U1Look *s, int enabled, unsigned char *camera, float *dir,
                        float *pitch, const int *flags, float lo, float hi) {
    const float heading = dir_heading(dir), pitch_in = *pitch;
    if (!enabled) {
        s->camera = NULL;
        mouse_trace(hook, camera, 1, heading, pitch_in, 0.0f, 0.0f, 'o');
        return;
    }
    const float horizontal = dir[0] * dir[0] + dir[2] * dir[2];
    if (!isfinite(horizontal) || horizontal < 0.81f || horizontal > 1.21f || !isfinite(*pitch)) {
        s->camera = NULL;
        mouse_trace(hook, camera, 1, heading, pitch_in, 0.0f, 0.0f, 'u');
        return;
    }
    float dx = 0, dy = 0;
    char mode = 'f';
    const int ready = look_ready(s, camera, atan2f(dir[0], dir[2]), now_ms());
    const int took = ready && frame_take(frame, camera, &dx, &dy, &mode);
    mouse_trace(hook, camera, 1, heading, pitch_in, dx, dy, mode);
    if (!took || (dx == 0.0f && dy == 0.0f)) return;
    s->taken[0] += dx;
    s->taken[1] += dy;
    game_invert(flags, &dx, &dy);
    const float per_count = look_scale();
    if (dx != 0.0f) u1_turn(s, camera, dir, dx, per_count);
    if (dy != 0.0f) {
        float next = *pitch - dy * per_count * look_invert() * 57.29578f;
        if (next > hi && next > *pitch) next = *pitch > hi ? *pitch : hi;
        if (next < lo && next < *pitch) next = *pitch < lo ? *pitch : lo;
        if (isfinite(next)) *pitch = next;
    }
}
/* Uncharted 3's cover camera update (0x4cd030, vtable 0x1670210 + 0x140; rdi the camera, rsi the input). */
static ABI void *u3_cover_camera_input(unsigned char *camera, unsigned char *params, void *unused) {
    (void)unused;
    if (!camera) return NULL;
    float *dir = (float *)(camera + 0x2f0), *pitch = (float *)(camera + 0x2e0);
    const float lo = -*(const float *)(camera + 0x21c), hi = *(const float *)(camera + 0x218);
    if (look_debug()) {
        static LookDebug d;
        look_debug_line(&d, &u3_cover_look, u3_cover_enabled(), camera, dir, *pitch, lo, hi,
                        params ? (const float *)(params + 0x5c) : NULL, runtime_pad_aim_held());
    }
    cover_input("u3c", &u3_frame, &u3_cover_look, u3_cover_enabled(), camera, dir, pitch, u3_invert_flags(camera),
                isfinite(lo) && lo < 0.0f && lo > -90.0f ? lo : -60.0f,
                isfinite(hi) && hi > 0.0f && hi < 90.0f ? hi : 60.0f);
    return NULL;
}
/* Uncharted 2's cover camera update (0x417450, vtable 0xfff0d0 + 0xe0; it returns a struct: rdi the
 * returned location, rsi the camera, rdx the input). */
static ABI void *u2_cover_camera_input(unsigned char *location, unsigned char *camera, unsigned char *params) {
    (void)location;
    if (!camera) return NULL;
    const float *tune = *(const float *const *)(camera + 0x1290);
    if (!tune) return NULL;
    float *dir = (float *)(camera + 0x1c0), *pitch = (float *)(camera + 0x1b0);
    const float lo = -tune[0xc / 4], hi = tune[0x8 / 4];
    if (look_debug()) {
        static LookDebug d;
        look_debug_line(&d, &u2_cover_look, u2_cover_enabled(), camera, dir, *pitch, lo, hi,
                        params ? (const float *)(params + 0x5c) : NULL, runtime_pad_aim_held());
    }
    cover_input("u2c", &u2_frame, &u2_cover_look, u2_cover_enabled(), camera, dir, pitch, (const int *)(image + 0x105de9c),
                isfinite(lo) && lo < 0.0f && lo > -90.0f ? lo : -60.0f,
                isfinite(hi) && hi > 0.0f && hi < 90.0f ? hi : 60.0f);
    return NULL;
}

static const MouseLookSite sites[] = {
    {"Uncharted 1 (orbit camera)",
     0xb7430,
     6, /* push rbp; mov rbp, rsp; push r15 */
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec,
      0x28, 0x01, 0x00, 0x00, 0x49, 0x89, 0xd7, 0x48},
     24,
     (void *)u1_orbit_camera_input},
    {"Uncharted 1 (follow camera)",
     0xaff80,
     6,
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec,
      0x78, 0x02, 0x00, 0x00, 0x48, 0x89, 0xd3, 0x49},
     24,
     (void *)u1_follow_camera_input},
    {"Uncharted 1 (cover camera)",
     0xa4670,
     6,
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec,
      0x88, 0x01, 0x00, 0x00, 0x49, 0x89, 0xd7, 0x48},
     24,
     (void *)u1_cover_camera_input},
    {"Uncharted 2",
     0x428f40,
     6, /* push rbp; mov rbp, rsp; push r15 */
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec,
      0x78, 0xc5, 0xfa, 0x11, 0x85, 0x70, 0xff, 0xff},
     24,
     (void *)u2_camera_input},
    {"Uncharted 3",
     0x4e42f0,
     6, /* push rbp; mov rbp, rsp; push r15 */
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec,
      0xc8, 0x00, 0x00, 0x00, 0x45, 0x89, 0xc7, 0x48},
     24,
     (void *)u3_camera_input},
    /* gow3 24 Oct 2026: aim (zoom) and cover cameras. */
    {"Uncharted 3 (aim camera)",
     0x511370,
     6,
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec,
      0xb8, 0x06, 0x00, 0x00, 0x48, 0x89, 0xf3, 0x49},
     24,
     (void *)u3_aim_camera_input},
    {"Uncharted 2 (aim camera)",
     0x450e40,
     6,
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec,
      0xd8, 0x01, 0x00, 0x00, 0x49, 0x89, 0xf6, 0x49},
     24,
     (void *)u2_aim_camera_input},
    {"Uncharted 3 (cover camera)",
     0x4cd030,
     6,
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec,
      0xf8, 0x00, 0x00, 0x00, 0x49, 0x89, 0xf6, 0x49},
     24,
     (void *)u3_cover_camera_input},
    {"Uncharted 2 (cover camera)",
     0x417450,
     6,
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec,
      0x68, 0x01, 0x00, 0x00, 0x49, 0x89, 0xd7, 0x48},
     24,
     (void *)u2_cover_camera_input},
    /* gow3 1 Nov 2026: the follow cameras' stick step (vtable +0x168): the mouse turn decided at the
     * update's entry is applied here (u1_step_input). */
    {"Uncharted 1 (follow camera stick step)",
     0xaac90,
     6,
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec,
      0xa8, 0x00, 0x00, 0x00, 0xc5, 0xfa, 0x11, 0x85},
     24,
     (void *)u1_step_input},
    /* gow3 25 Oct 2026: the aim function (its 4th argument, the remembered aim direction, also reaches
     * the handler: the stub leaves rcx as it was). */
    {"Uncharted 1 (aim)",
     0x276a10,
     6,
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec,
      0x08, 0x03, 0x00, 0x00, 0x45, 0x89, 0xc4, 0x48},
     24,
     (void *)u1_aim_input},
    /* The jeep's mounted gun (CameraControlRailVehicle's look step, #250). */
    {"Uncharted 1 (rail vehicle gun)",
     0x2a9690,
     6, /* push rbp; mov rbp, rsp; push r15 */
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50, 0x49, 0x89,
      0xd7, 0x49, 0x89, 0xf4, 0x49, 0x89, 0xfe, 0x45},
     24,
     (void *)u1_rail_look_input},
};

const MouseLookSite *runtime_mouselook_sites(unsigned *count) {
    *count = sizeof(sites) / sizeof(sites[0]);
    return sites;
}
