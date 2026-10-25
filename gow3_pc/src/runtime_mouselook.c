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

void runtime_mouselook_set_image(unsigned char *base) { image = base; }

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
    if (!take_look(&dx, &dy) || (dx == 0.0f && dy == 0.0f) || !camera || !look) {
        return scale_stick(stick);
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
    return scale_stick(stick);
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
    if (!take_look(&dx, &dy) || (dx == 0.0f && dy == 0.0f) || !camera || !look) {
        return scale_stick(stick);
    }
    game_invert((const int *)(image + 0x105de9c), &dx, &dy);
    const float per_count = look_scale();
    if (dx != 0.0f) {
        rotate_look(look, dx, per_count);
    }
    const float *tune = *(const float *const *)(camera + 0x8d00);
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
    return scale_stick(stick);
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
static ABI void *u1_follow_camera_input(unsigned char *location, unsigned char *camera, unsigned char *params) {
    (void)location;
    if (look_debug()) u1_follow_debug(camera, params);
    if (!u1_enabled() || !camera || !params) return NULL;
    float *dir = (float *)(camera + 0x1310);
    const float *tune = *(const float *const *)(camera + 0x1e0);
    const int usable = tune && u1_follow_usable(dir);
    if (!params[0x08]) {
        /* An older camera still blending out: the newest camera's turn of this frame. The stick turns
         * every camera of the blend (they get the same pad input), so the view does not jump back
         * when the game returns to this one (standing <-> moving follow cameras while walking). */
        if (usable && u1_frame_turn.frame == u1_frame() && u1_frame_turn.camera != camera) {
            u1_follow_turn(camera, dir, tune, u1_frame_turn.dx, u1_frame_turn.dy, u1_frame_turn.per_count, 0);
            ++u1_older_turns;
        }
        return NULL;
    }
    u1_frame_turn.frame = -1;
    if (!usable) {
        u1_follow.camera = NULL;
        return NULL;
    }
    float dx = 0, dy = 0;
    if (!u1_look_ready(&u1_follow, camera, atan2f(dir[0], dir[2]), now_ms()) || !u1_take(&u1_follow, &dx, &dy)) {
        return NULL;
    }
    const float per_count = look_scale();
    u1_follow_turn(camera, dir, tune, dx, dy, per_count, 1);
    u1_frame_turn.frame = u1_frame();
    u1_frame_turn.camera = camera;
    u1_frame_turn.dx = dx;
    u1_frame_turn.dy = dy;
    u1_frame_turn.per_count = per_count;
    return NULL;
}

/* The cover camera (CameraControlCover, update 0xa4670; not verified in game: on only with
 * GOW3_MOUSE_LOOK_U1_COVER=1). Active while camera + 0x1f4 is set; tune = *(camera + 0x1300). Its
 * stick step (0xa3f40) turns the flat direction camera + 0x240 (x, 0, z, 0) about +Y by -stickX *
 * rate(+0x270, ramping tune[0x1c]..tune[0x20] deg/s) * frame time degrees once the frame counter
 * passes camera + 0x1e4 (the stick fades in until + 0x1e8), and lowers the pitch + 0x230 (degrees)
 * by tune[0x30] * frame time * stickY, kept inside -tune[0xc]..tune[8] (lower toward the wall, which
 * the game still applies after the mouse). */
static int u1_cover_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *env = getenv("GOW3_MOUSE_LOOK_U1_COVER");
        enabled = u1_enabled() && env && env[0] == '1';
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
    if (!u1_cover_enabled() || !params[0x08] || !tune) return NULL;
    const float horizontal = dir[0] * dir[0] + dir[2] * dir[2];
    if (!isfinite(horizontal) || horizontal < 0.81f || horizontal > 1.21f) {
        u1_cover.camera = NULL;
        return NULL;
    }
    float dx = 0, dy = 0;
    if (!u1_look_ready(&u1_cover, camera, atan2f(dir[0], dir[2]), now) || !u1_take(&u1_cover, &dx, &dy)) return NULL;
    /* Still entering cover (the game ignores the stick then): the movement is dropped. */
    const int *clock = *(const int *const *)(image + 0xa4cb08);
    if (clock && clock[2] <= *(const int *)(camera + 0x1e4)) return NULL;
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
 * =0 the stick again), _U3_COVER, _U2_COVER (off until verified). */
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
    if (enabled < 0) enabled = knob("GOW3_MOUSE_LOOK_U3_COVER", 0);
    return enabled;
}
static int u2_cover_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) enabled = knob("GOW3_MOUSE_LOOK_U2_COVER", 0);
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
static void aim_input(U1Look *s, int enabled, unsigned char *camera, float *dir, const int *flags, float lo, float hi) {
    if (!enabled || !runtime_pad_aim_held()) {
        s->camera = NULL;
        return;
    }
    if (!aim_usable(dir)) {
        s->camera = NULL;
        return;
    }
    float dx = 0, dy = 0;
    if (!look_ready(s, camera, atan2f(dir[0], dir[2]), now_ms()) || !aim_take(s, flags, &dx, &dy)) return;
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
    if (valid) aim_input(&u3_aim, u3_aim_enabled(), camera, dir, u3_aim_invert_flags(player), lo, hi);
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
    aim_input(&u2_aim, u2_aim_enabled(), camera, dir, (const int *)(image + 0x105dea4), lo, hi);
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
    if (aim_take(&u1_aim, (const int *)(image + 0x95ca20), &dx, &dy)) {
        u1_press.dx += dx;
        u1_press.dy += dy;
    }
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
static void cover_input(U1Look *s, int enabled, unsigned char *camera, float *dir, float *pitch, const int *flags,
                        float lo, float hi) {
    if (!enabled) {
        s->camera = NULL;
        return;
    }
    const float horizontal = dir[0] * dir[0] + dir[2] * dir[2];
    if (!isfinite(horizontal) || horizontal < 0.81f || horizontal > 1.21f || !isfinite(*pitch)) {
        s->camera = NULL;
        return;
    }
    float dx = 0, dy = 0;
    if (!look_ready(s, camera, atan2f(dir[0], dir[2]), now_ms()) || !take_look(&dx, &dy) ||
        (dx == 0.0f && dy == 0.0f))
        return;
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
    cover_input(&u3_cover_look, u3_cover_enabled(), camera, dir, pitch, u3_invert_flags(camera),
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
    cover_input(&u2_cover_look, u2_cover_enabled(), camera, dir, pitch, (const int *)(image + 0x105de9c),
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
    /* gow3 25 Oct 2026: the aim function (its 4th argument, the remembered aim direction, also reaches
     * the handler: the stub leaves rcx as it was). */
    {"Uncharted 1 (aim)",
     0x276a10,
     6,
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec,
      0x08, 0x03, 0x00, 0x00, 0x45, 0x89, 0xc4, 0x48},
     24,
     (void *)u1_aim_input},
};

const MouseLookSite *runtime_mouselook_sites(unsigned *count) {
    *count = sizeof(sites) / sizeof(sites[0]);
    return sites;
}
