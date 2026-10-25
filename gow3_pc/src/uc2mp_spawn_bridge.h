/* Opt-in UC2 1.02 map-test initialization bridge. Include from probe.c AFTER
 * allocate_near_image(), with Segment/image/page_size/protect already defined.
 * Reuses the exact native actor-load sequence before the existing autoSpawn
 * call. It never creates substitute objects or chooses spawn coordinates. */
#ifndef UC2MP_SPAWN_BRIDGE_H
#define UC2MP_SPAWN_BRIDGE_H
#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include "uc2mp_extracted_spawn_catalog.h"
#include "uc2mp_map_levels.h"

typedef void *(ABI *Uc2mpSpawnLoad)(void *, const char *, int);
typedef void (ABI *Uc2mpSpawnFinish)(void *);
typedef void (ABI *Uc2mpSpawnOriginal)(void);
typedef int (*Uc2mpSpawnMapped)(uintptr_t, uint64_t);
typedef struct {
    unsigned char *base;
    Uc2mpSpawnLoad load;
    Uc2mpSpawnFinish finish;
    Uc2mpSpawnOriginal original;
    Uc2mpSpawnMapped is_mapped;
    uint64_t image_size;
} Uc2mpSpawnBridge;
static Uc2mpSpawnBridge uc2mp_spawn_bridge;
static Uc2mpSpawnFinish uc2mp_spawn_barrier_original;
static int uc2mp_spawn_deferred;
static atomic_int uc2mp_spawn_pending, uc2mp_spawn_completed;
typedef void *(ABI *Uc2mpSpawnLookup)(void *, uint32_t);
typedef void (ABI *Uc2mpSpawnTransform)(void *, const void *);
typedef void (ABI *Uc2mpSpawnFactory)(const void *, const void *, const char *, void *, void *, uintptr_t, int);
static Uc2mpSpawnLoad uc2mp_spawn_load_world;
static Uc2mpSpawnLookup uc2mp_spawn_lookup;
static Uc2mpSpawnTransform uc2mp_spawn_transform;
static Uc2mpSpawnFactory uc2mp_spawn_factory;
static int uc2mp_spawn_stage;
/* Village/Warzone: the Collection has no wrapper package with their online spawners, so the
 * poses come from the catalogue extracted from the PS3 wrappers (root records, world space). */
static const Uc2mpExtractedSpawnMap *uc2mp_spawn_catalog;
/* In-match pause. The Collection layer's gameplay hook (1.02 0xa61320) ends by allowing the
 * pause (menu manager 0x1136640 +0x105c = 1); a game started on a map from the command line
 * never runs it. The page choice (0x5c7bb0) takes PAGE_PAUSE_MULTIPLAYER only in network
 * mode ([0x14ce97c] >= 2): its jl to the campaign branch (0x5c7c05) is removed in a match. */
#define UC2MP_PAUSE_ALLOWED 0x1136640u+0x105cu
static int uc2mp_spawn_pause;
/* Called once the hero exists (probe.c: hides the multiplayer loading screen). */
static void (*uc2mp_spawn_on_spawned)(void);
static unsigned uc2mp_spawn_catalog_index;
/* Native synchronous loaders can execute/resume work on another OS thread.
 * Publish immutable poses once per isolated startup, rather than using TLS
 * across those calls. The exact autoSpawn call site claims them only once. */
static atomic_int uc2mp_spawn_poses_ready;
static _Alignas(16) float uc2mp_spawn_player_pose[8], uc2mp_spawn_camera_pose[8];

static ABI void uc2mp_spawn_factory_entry(const void *player, const void *camera,
        const char *skin, void *config, void *parent, uintptr_t unused, int native_flag) {
    /* Only the existing autoSpawn call site is redirected. Preserve all seven
     * SysV arguments, including the otherwise-unused R9 and stack argument. */
    int expected=1;
    const int claimed=atomic_compare_exchange_strong_explicit(&uc2mp_spawn_poses_ready,&expected,2,
                                                              memory_order_acquire,memory_order_relaxed);
    printf("UC2MP spawn: factory hook claimed=%d previous=%d parent=%p flag=%d\n",
           claimed,expected,parent,native_flag);
    fflush(stdout);
    if (!claimed) return;
    (void)player; (void)camera;
    uc2mp_spawn_factory(uc2mp_spawn_player_pose, uc2mp_spawn_camera_pose,
                        skin, config, parent, unused, native_flag);
}
typedef int (ABI *Uc2mpPlayerInit)(void *, const void *);
typedef void (ABI *Uc2mpPlayerFailure)(void *, const char *);
static Uc2mpPlayerInit uc2mp_player_init_original;
static Uc2mpPlayerFailure uc2mp_player_failure_original;
static _Thread_local int uc2mp_spawn_trace_active;
static atomic_uint uc2mp_spawn_init_entries, uc2mp_spawn_failure_entries;
/* Installed before image protections; outside the scoped init call these
 * callbacks are transparent. Native results and destruction remain intact. */
static ABI int uc2mp_player_init_trace(void *object, const void *descriptor) {
    const unsigned entry = atomic_fetch_add_explicit(&uc2mp_spawn_init_entries, 1, memory_order_relaxed);
    const int log = uc2mp_spawn_trace_active || entry < 8;
    if (log) {
        printf("UC2MP spawn: Player::Init entered object=%p descriptor=%p scope=%d tls=%p entry=%u\n",
               object, descriptor, uc2mp_spawn_trace_active, (void *)&uc2mp_spawn_trace_active, entry);
        fflush(stdout);
    }
    const int result = uc2mp_player_init_original(object, descriptor);
    if (log) {
        printf("UC2MP spawn: Player::Init returned status=%#x object=%p\n", (unsigned)result, object);
        fflush(stdout);
    }
    return result;
}
static ABI void uc2mp_player_failure_trace(void *object, const char *message) {
    const unsigned entry = atomic_fetch_add_explicit(&uc2mp_spawn_failure_entries, 1, memory_order_relaxed);
    if (uc2mp_spawn_trace_active || entry < 8) {
        /* 9be450 supplies its bounded, NUL-terminated local formatting buffer. */
        printf("UC2MP spawn: Player failure object=%p scope=%d tls=%p entry=%u reason=%.255s\n",
               object, uc2mp_spawn_trace_active, (void *)&uc2mp_spawn_trace_active, entry,
               message ? message : "(null)");
        fflush(stdout);
    }
    uc2mp_player_failure_original(object, message);
}
static int uc2mp_spawn_readable(Uc2mpSpawnBridge *bridge, uintptr_t address, uint64_t bytes) {
    const uintptr_t base = (uintptr_t)bridge->base;
    if (address >= base && bytes <= bridge->image_size &&
        address-base <= bridge->image_size-bytes) return 1;
    return bridge->is_mapped(address, bytes);
}

/* Read-only process-handle snapshot. Native 975620 publishes slot/generation
 * at 115bd20; resolution checks the object generation at +44. Mapping checks
 * and a second slot read avoid treating a changing handle as a valid object. */
typedef struct {
    uintptr_t slot, object, vtable, init_first, init_second;
    uint32_t generation, object_generation, process_type;
    uint16_t flags;
    int readable, stable, valid;
} Uc2mpSpawnSnapshot;
static Uc2mpSpawnSnapshot uc2mp_spawn_snapshot(Uc2mpSpawnBridge *bridge) {
    Uc2mpSpawnSnapshot result = {0};
    const unsigned char *handle = bridge->base + 0x115bd20;
    if (bridge->image_size < UINT64_C(0x115bd20)+12) return result;
    result.readable = 1;
    memcpy(&result.slot, handle, 8);
    memcpy(&result.generation, handle+8, 4);
    if (!result.slot || !bridge->is_mapped(result.slot, 8)) return result;
    memcpy(&result.object, (const void *)result.slot, 8);
    if (result.object && bridge->is_mapped(result.object, 0x48)) {
        memcpy(&result.object_generation, (const void *)(result.object+0x44), 4);
        memcpy(&result.process_type, (const void *)(result.object+0x30), 4);
        memcpy(&result.flags, (const void *)(result.object+0x34), 2);
        memcpy(&result.vtable, (const void *)result.object, 8);
        if (result.vtable && uc2mp_spawn_readable(bridge, result.vtable, 0x30)) {
            memcpy(&result.init_first, (const void *)(result.vtable+0x20), 8);
            memcpy(&result.init_second, (const void *)(result.vtable+0x28), 8);
        }
    }
    uintptr_t slot_again, object_again;
    uint32_t generation_again;
    memcpy(&slot_again, handle, 8);
    memcpy(&generation_again, handle+8, 4);
    memcpy(&object_again, (const void *)result.slot, 8);
    result.stable = slot_again == result.slot && generation_again == result.generation &&
                    object_again == result.object;
    result.valid = result.stable && result.object && result.generation &&
                   bridge->is_mapped(result.object, 0x48) &&
                   result.object_generation == result.generation;
    return result;
}
static void uc2mp_spawn_log_snapshot(Uc2mpSpawnBridge *bridge, const char *phase) {
    const Uc2mpSpawnSnapshot s = uc2mp_spawn_snapshot(bridge);
    printf("UC2MP spawn: %s hero handle readable=%d slot=%p object=%p generation=%u object_generation=%u process_type=%08x stable=%d valid=%d vtable=%p flags=%#x init_first=%p init_second=%p\n",
           phase, s.readable, (void *)s.slot, (void *)s.object, s.generation,
           s.object_generation, s.process_type, s.stable, s.valid,
           (void *)s.vtable, s.flags, (void *)s.init_first, (void *)s.init_second);
    fflush(stdout);
}

/* Requires the isolated manifest overlay. All objects/transforms are produced
 * by original loader/lookup/transform functions; no hand-built guest objects. */
static int uc2mp_spawn_world_ready(Uc2mpSpawnBridge *bridge, const char *name, void **out) {
    unsigned char *level = uc2mp_spawn_load_world(bridge->base+0x1130390,name,1);
    if (!level || !bridge->is_mapped((uintptr_t)level,0x9a8)) {
        fprintf(stderr,"UC2MP spawn: world %s returned unmapped Level=%p\n",name,(void *)level); return 0;
    }
    uint32_t state; uint16_t flags, kind;
    /* Native GameInit at3f8870 pins explicitly requested levels with bit40. */
    memcpy(&flags,level+0x18,2); flags |= 0x40; memcpy(level+0x18,&flags,2);
    memcpy(&state,level+0x30,4);
    /* Existing entries return directly even when the synchronous flag is set. */
    if (state != 7) bridge->finish(level);
    if (!bridge->is_mapped((uintptr_t)level,0x9a8)) return 0;
    memcpy(&state,level+0x30,4); memcpy(&flags,level+0x18,2); memcpy(&kind,level,2);
    printf("UC2MP spawn: world %s Level=%p state=%u flags=%#x kind=%#x\n",name,(void *)level,state,flags,kind);
    fflush(stdout);
    if (state != 7 || !(flags & 0x80) || !(kind & 1)) return 0;
    *out=level; return 1;
}
static int uc2mp_spawn_read_pose(Uc2mpSpawnBridge *bridge, uint32_t sid, void *level, float *pose) {
    const unsigned char *spawner=uc2mp_spawn_lookup(bridge->base+0x1130390,sid);
    if (!spawner || !bridge->is_mapped((uintptr_t)spawner,0x68)) {
        fprintf(stderr,"UC2MP spawn: spawner %08x missing/unmapped %p\n",sid,(const void *)spawner); return 0;
    }
    uintptr_t owner,parent;
    memcpy(&owner,spawner+0x58,8); memcpy(&parent,spawner+0x60,8);
    /* The detached autoSpawn path is suitable only for these static records. */
    if (owner != (uintptr_t)level || parent) {
        fprintf(stderr,"UC2MP spawn: spawner %08x owner=%p expected=%p parent=%p rejected\n",
                sid,(void *)owner,level,(void *)parent); return 0;
    }
    uc2mp_spawn_transform(pose,spawner);
    float norm=0;
    for (unsigned i=0;i<8;++i) if (!isfinite(pose[i])) return 0;
    for (unsigned i=4;i<8;++i) norm += pose[i]*pose[i];
    if (norm < 0.9f || norm > 1.1f) return 0;
    printf("UC2MP spawn: native spawner %08x world=(%.6f %.6f %.6f) quaternion=(%.6f %.6f %.6f %.6f)\n",
           sid,pose[0],pose[1],pose[2],pose[4],pose[5],pose[6],pose[7]);
    fflush(stdout); return 1;
}
static int uc2mp_spawn_prepare_stage(Uc2mpSpawnBridge *bridge) {
    uc2mp_spawn_poses_ready=0;
    void *world=NULL,*wrapper=NULL;
    if (!uc2mp_spawn_world_ready(bridge,"stage-multi",&world) ||
        !uc2mp_spawn_world_ready(bridge,"stage-multi-pak",&wrapper)) return 0;
    uint16_t flags; uintptr_t group; int32_t count;
    memcpy(&flags,(unsigned char *)wrapper+0x18,2);
    memcpy(&group,(unsigned char *)wrapper+0x9a0,8);
    if (!(flags & 0x200) || !group || !bridge->is_mapped(group,0x24)) {
        fprintf(stderr,"UC2MP spawn: wrapper flags=%#x group=%p not eligible\n",flags,(void *)group); return 0;
    }
    memcpy(&count,(const void *)(group+0x20),4);
    if (count != 85 || uc2mp_spawn_snapshot(bridge).valid) {
        fprintf(stderr,"UC2MP spawn: wrapper count=%d or existing hero prevents creation\n",count); return 0;
    }
    if (!uc2mp_spawn_read_pose(bridge,0x2a7ef449,wrapper,uc2mp_spawn_player_pose) ||
        !uc2mp_spawn_read_pose(bridge,0xe4b94334,wrapper,uc2mp_spawn_camera_pose)) return 0;
    atomic_store_explicit(&uc2mp_spawn_poses_ready,1,memory_order_release);
    puts("UC2MP spawn: validated stage poses published for one native factory call");
    fflush(stdout);
    return 1;
}

static int uc2mp_spawn_pose_valid(const float *pose) {
    float norm=0;
    for (unsigned i=0;i<8;++i) if (!isfinite(pose[i])) return 0;
    for (unsigned i=4;i<8;++i) norm += pose[i]*pose[i];
    return norm >= 0.9f && norm <= 1.1f;
}
/* Catalogue maps: the native Level must be loaded, ready and kept, with a zero origin
 * (Level+0x20), which is what the extracted root poses assume. The player spawner is
 * catalogue entry UC2MP_SPAWN_INDEX (default 0); the camera is the nearest camera record. */
static int uc2mp_spawn_prepare_catalog(Uc2mpSpawnBridge *bridge) {
    uc2mp_spawn_poses_ready=0;
    const Uc2mpExtractedSpawnMap *map=uc2mp_spawn_catalog;
    void *world=NULL;
    if (!map || !map->player_count || !map->camera_count ||
        !uc2mp_spawn_world_ready(bridge,map->level_name,&world)) return 0;
    float origin[4];
    memcpy(origin,(const unsigned char *)world+0x20,sizeof(origin));
    if (origin[0] != 0.0f || origin[1] != 0.0f || origin[2] != 0.0f) {
        fprintf(stderr,"UC2MP spawn: %s origin (%f %f %f) is not zero; catalogue poses rejected\n",
                map->level_name,origin[0],origin[1],origin[2]);
        return 0;
    }
    if (uc2mp_spawn_snapshot(bridge).valid) {
        fputs("UC2MP spawn: existing hero prevents creation\n",stderr); return 0;
    }
    const Uc2mpExtractedSpawnPose *player=&map->players[uc2mp_spawn_catalog_index % map->player_count];
    const Uc2mpExtractedSpawnPose *camera=&map->cameras[0];
    float best=INFINITY;
    for (size_t i=0;i<map->camera_count;++i) {
        float d=0;
        for (unsigned k=0;k<3;++k) {
            const float delta=map->cameras[i].pose[k]-player->pose[k];
            d += delta*delta;
        }
        if (d < best) { best=d; camera=&map->cameras[i]; }
    }
    if (!uc2mp_spawn_pose_valid(player->pose) || !uc2mp_spawn_pose_valid(camera->pose)) return 0;
    memcpy(uc2mp_spawn_player_pose,player->pose,sizeof(uc2mp_spawn_player_pose));
    memcpy(uc2mp_spawn_camera_pose,camera->pose,sizeof(uc2mp_spawn_camera_pose));
    atomic_store_explicit(&uc2mp_spawn_poses_ready,1,memory_order_release);
    printf("UC2MP spawn: %s catalogue poses published: %s world=(%.6f %.6f %.6f), camera %s\n",
           map->level_name,player->name,player->pose[0],player->pose[1],player->pose[2],camera->name);
    fflush(stdout);
    return 1;
}

/* These two gates are independent: the launcher opts in only for --map-test,
 * and the actual guest command line must request one supported map + player.
 * Further levels are only that map's companions (uc2mp_map_levels.h). */
static int uc2mp_spawn_requested(int argc, const char *const *argv) {
    const char *enabled = getenv("UNCHARTED_UC2MP_PRELOAD_HERO");
    if (!enabled || strcmp(enabled, "1") || argc < 1 || !argv) return 0;
    int level = 0, player = 0;
    const char *map = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!argv[i]) return 0;
        if (!strcmp(argv[i], "level")) {
            if (++i >= argc || !argv[i]) return 0;
            if (++level > 1) {
                if (!uc2mp_map_companion(map, argv[i])) return 0;
            } else if (strcmp(argv[i], "stage-multi") && !uc2mp_extracted_spawn_map(argv[i])) {
                return 0;
            }
            if (level == 1) map = argv[i];
        } else if (!strcmp(argv[i], "player")) {
            player = 1;
        }
    }
    return level >= 1 && player;
}

/* Split from the installed entry so contracts can be tested with fake native
 * function boundaries, without executing game code in a unit test. */
static int uc2mp_spawn_preload_actor(Uc2mpSpawnBridge *bridge, uint64_t name_rva, const char *name) {
    unsigned char *level = bridge->load(bridge->base + 0x1130390,
                                      (const char *)bridge->base + name_rva, 0);
    if (!level || !bridge->is_mapped((uintptr_t)level, 0x34)) {
        fprintf(stderr, "UC2MP spawn: %s load returned no valid Level; autoSpawn skipped\n", name);
        return 0;
    }
    uint16_t flags;
    uint32_t state;
    memcpy(&flags, level + 0x18, sizeof(flags));
    flags |= 0x40;
    memcpy(level + 0x18, &flags, sizeof(flags));
    memcpy(&state, level + 0x30, sizeof(state));
    printf("UC2MP spawn: %s Level=%p state=%u flags=%#x; native preload\n",
           name, (void *)level, state, flags);
    fflush(stdout);
    if (state != 7) bridge->finish(level);
    if (!bridge->is_mapped((uintptr_t)level, 0x34)) {
        fprintf(stderr, "UC2MP spawn: %s Level unmapped after native preload; autoSpawn skipped\n", name);
        return 0;
    }
    memcpy(&state, level + 0x30, sizeof(state));
    memcpy(&flags, level + 0x18, sizeof(flags));
    if (state != 7 || !(flags & 0x80)) {
        fprintf(stderr, "UC2MP spawn: %s not ready (state=%u flags=%#x); autoSpawn skipped\n",
                name, state, flags);
        return 0;
    }
    flags &= (uint16_t)~0x20u;
    memcpy(level + 0x18, &flags, sizeof(flags));
    printf("UC2MP spawn: %s ready state=7 flags=%#x\n", name, flags);
    fflush(stdout);
    return 1;
}
static int uc2mp_spawn_preload(Uc2mpSpawnBridge *bridge) {
    /* hero-default foreground skeleton SID27e57f43 is supplied by proto's
     * JOINT_HIERARCHY. GameInit normally queues proto after this early spawn. */
    if (!uc2mp_spawn_preload_actor(bridge, 0xeb05cd, "proto") ||
        !uc2mp_spawn_preload_actor(bridge, 0xe9466e, "hero-default")) return 0;
    if (uc2mp_spawn_stage && !uc2mp_spawn_prepare_stage(bridge)) {
        fputs("UC2MP spawn: stage world/spawner validation failed; player creation skipped\n",stderr);
        return 0;
    }
    if (uc2mp_spawn_catalog && !uc2mp_spawn_prepare_catalog(bridge)) {
        fputs("UC2MP spawn: catalogue map validation failed; player creation skipped\n",stderr);
        return 0;
    }
    uc2mp_spawn_log_snapshot(bridge, "before original");
    const int previous_trace = uc2mp_spawn_trace_active;
    uc2mp_spawn_trace_active = 1;
    printf("UC2MP spawn: calling original scope=1 tls=%p init_entries=%u failure_entries=%u\n",
           (void *)&uc2mp_spawn_trace_active, atomic_load(&uc2mp_spawn_init_entries),
           atomic_load(&uc2mp_spawn_failure_entries));
    fflush(stdout);
    bridge->original();
    uc2mp_spawn_poses_ready=0;
    uc2mp_spawn_trace_active = previous_trace;
    printf("UC2MP spawn: returned original init_entries=%u failure_entries=%u\n",
           atomic_load(&uc2mp_spawn_init_entries), atomic_load(&uc2mp_spawn_failure_entries));
    uc2mp_spawn_log_snapshot(bridge, "immediately after original");
    if (uc2mp_spawn_pause) {
        bridge->base[UC2MP_PAUSE_ALLOWED] = 1;
        puts("UC2MP spawn: in-match pause allowed (multiplayer pause page)");
    }
    if (uc2mp_spawn_on_spawned) uc2mp_spawn_on_spawned();
    puts("UC2MP spawn: original autoSpawnPlayer returned; avatar/control still require validation");
    fflush(stdout);
    return 1;
}

static ABI void uc2mp_spawn_entry(void) {
    if (uc2mp_spawn_deferred) {
        if (!atomic_load(&uc2mp_spawn_completed)) atomic_store(&uc2mp_spawn_pending,1);
        puts("UC2MP spawn: autoSpawn deferred until original GameInit level requests");
        fflush(stdout);
        return;
    }
    (void)uc2mp_spawn_preload(&uc2mp_spawn_bridge);
}

/* Existing GameInit call site, not an arbitrary frame callback. 56bf60 queues
 * CLI level requests; it does NOT wait for them. Stage mode explicitly waits
 * on each native Level before spawning. Startup may migrate between native job
 * threads; an atomic one-shot guards the invocation, without imposing host TLS. */
static ABI void uc2mp_spawn_barrier_entry(void *manager) {
    uc2mp_spawn_barrier_original(manager);
    if (!atomic_exchange(&uc2mp_spawn_pending,0) || atomic_exchange(&uc2mp_spawn_completed,1)) return;
    puts("UC2MP spawn: original GameInit level requests returned; deferred preload/spawn");
    fflush(stdout);
    (void)uc2mp_spawn_preload(&uc2mp_spawn_bridge);
}

static int uc2mp_spawn_range(Segment *segments, uint64_t count, uint64_t address,
                            uint64_t bytes, unsigned flags) {
    for (uint64_t i = 0; i < count; ++i)
        if ((segments[i].flags & flags) == flags && address >= segments[i].address &&
            bytes <= segments[i].size && address - segments[i].address <= segments[i].size - bytes)
            return 1;
    return 0;
}

/* 0=not requested, 1=installed, -1=requested but rejected. Installation occurs
 * before executable protections and before any guest initializer executes. */
static int install_uc2mp_spawn_bridge(Segment *segments, uint64_t count, uint64_t size,
                                      int guest_argc, const char *const *guest_argv) {
    if (!uc2mp_spawn_requested(guest_argc, guest_argv)) return 0;
    if (uc2mp_spawn_bridge.base) {
        fprintf(stderr, "UC2MP spawn: bridge already installed; refusing second mutation\n");
        return -1;
    }
    static const unsigned char call_site[] = {
        0x41,0x80,0xbd,0x18,0x48,0,0,0,0x74,0x05,0xe8,0x0b,0x0f,0x57,0
    };
    static const unsigned char spawn_entry[] = {
        0x55,0x48,0x89,0xe5,0x41,0x56,0x53,0x48,0x83,0xec,0x60,0x4c,0x8b,0x35,0xce,0x18
    };
    static const unsigned char barrier_call[] = {
        0x48,0x8d,0x3d,0xb1,0x7a,0xd3,0,0xe8,0x7c,0x36,0x17,0
    };
    static const unsigned char barrier_entry[] = {
        0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,
        0x53,0x48,0x83,0xec,0x18,0x48,0x89,0x7d,0xc0,0x48,0x8b,0x05
    };
    const char *defer_env = getenv("UNCHARTED_UC2MP_DEFER_SPAWN");
    const int deferred = defer_env && !strcmp(defer_env, "1");
    const char *stage_env = getenv("UNCHARTED_UC2MP_STAGE_SPAWN");
    const int spawn_records = stage_env && !strcmp(stage_env,"1");
    const char *level_arg = NULL;
    for (int i=1;i+1<guest_argc && !level_arg;++i) /* the map: the first level */
        if (!strcmp(guest_argv[i],"level")) level_arg=guest_argv[i+1];
    /* Same opt-in for every map: the retained stage spawners, or the extracted catalogue. */
    const int stage = spawn_records && level_arg && !strcmp(level_arg,"stage-multi");
    const Uc2mpExtractedSpawnMap *catalog = spawn_records && !stage ? uc2mp_extracted_spawn_map(level_arg) : NULL;
    if (spawn_records && (!deferred || (!stage && !catalog))) goto rejected;
    unsigned catalog_index = 0;
    const char *index_env = getenv("UC2MP_SPAWN_INDEX");
    if (catalog && index_env && *index_env) {
        char *end;
        const unsigned long value = strtoul(index_env,&end,10);
        if (*end || value >= catalog->player_count) goto rejected;
        catalog_index = (unsigned)value;
    }
    const int factory_hook = stage || catalog;
    static const unsigned char world_entry[] = {
        0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,
        0x53,0x48,0x83,0xec,0x18,0x41,0x89,0xd6,0x49,0x89,0xf7,0x49
    };
    static const unsigned char lookup_entry[] = {
        0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x53,0x50,0x41,0x89,
        0xf7,0x49,0x89,0xfe,0x41,0x81,0xff,0x90,0x4a,0xdb,0xff,0x75
    };
    static const unsigned char transform_entry[] = {
        0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x53,0x48,0x83,0xec,
        0x48,0x48,0x89,0xf3,0x49,0x89,0xfe,0x4c,0x8b,0x3d,0x06,0xcc
    };
    static const unsigned char factory_entry[] = {
        0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,
        0x53,0x48,0x81,0xec,0x48,0x01,0,0
    };
    static const unsigned char factory_call[] = {0xe8,0x9b,0xbb,0,0};
    static const unsigned char load_entry[] = {
        0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,
        0x53,0x48,0x83,0xec,0x18,0x41,0x89,0xd6,0x49,0x89,0xf7,0x49
    };
    static const unsigned char finish_entry[] = {
        0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,
        0x53,0x48,0x81,0xe4,0xe0,0xff,0xff,0xff,0x48,0x81,0xec,0xa0
    };
    static const unsigned char player_init_entry[] = {
        0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,
        0x53,0x48,0x81,0xec,0x68,0x01,0,0,0x49,0x89,0xf6,0x49
    };
    static const unsigned char player_failure_entry[] = {
        0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,
        0x53,0x48,0x81,0xec,0x98,0x02,0,0,0x49,0x89,0xf7,0x48
    };
    const struct { uint64_t at; const void *bytes; size_t length; unsigned flags; } signatures[] = {
        {0x3f8406, call_site, sizeof(call_site), 5},
        {0x969320, spawn_entry, sizeof(spawn_entry), 5},
        {0x56c200, load_entry, sizeof(load_entry), 5},
        {0x56ec70, finish_entry, sizeof(finish_entry), 5},
        {0x96bc80, player_init_entry, sizeof(player_init_entry), 5},
        {0x99ba60, player_failure_entry, sizeof(player_failure_entry), 5},
        {0xe9466e, "hero-default", sizeof("hero-default"), 4},
        {0xeb05cd, "proto", sizeof("proto"), 4},
    };
    if (!image || size < UINT64_C(0x115bd20) + 12 ||
        !uc2mp_spawn_range(segments,count,0x1130390,0x5c08,6) ||
        !uc2mp_spawn_range(segments,count,0x115bd20,12,6) ||
        !uc2mp_spawn_range(segments,count,0x1010980,0xd0,4)) goto rejected;
    uintptr_t init_target, failure_target;
    memcpy(&init_target, image+0x10109a0, 8);
    memcpy(&failure_target, image+0x1010a48, 8);
    if (init_target != (uintptr_t)(image+0x96bc80) ||
        failure_target != (uintptr_t)(image+0x99ba60)) goto rejected;
    for (size_t i=0; i<sizeof(signatures)/sizeof(*signatures); ++i) {
        const uint64_t at = signatures[i].at;
        if (at > size || signatures[i].length > size-at ||
            !uc2mp_spawn_range(segments,count,at,signatures[i].length,signatures[i].flags) ||
            memcmp(image+at,signatures[i].bytes,signatures[i].length)) goto rejected;
    }
    if (deferred &&
        (!uc2mp_spawn_range(segments,count,0x3f88d8,sizeof(barrier_call),5) ||
         !uc2mp_spawn_range(segments,count,0x56bf60,sizeof(barrier_entry),5) ||
         memcmp(image+0x3f88d8,barrier_call,sizeof(barrier_call)) ||
         memcmp(image+0x56bf60,barrier_entry,sizeof(barrier_entry)))) goto rejected;
    if (factory_hook) {
        const struct {uint64_t at; const void *bytes; size_t len;} checks[] = {
            {0x56ad90,world_entry,sizeof(world_entry)}, {0x56c7b0,lookup_entry,sizeof(lookup_entry)},
            {0x4bdfe0,transform_entry,sizeof(transform_entry)}, {0x975190,factory_entry,sizeof(factory_entry)},
            {0x9695f0,factory_call,sizeof(factory_call)}
        };
        for (size_t i=0;i<sizeof(checks)/sizeof(*checks);++i)
            if (!uc2mp_spawn_range(segments,count,checks[i].at,checks[i].len,5) ||
                memcmp(image+checks[i].at,checks[i].bytes,checks[i].len)) goto rejected;
    }
    unsigned char *stub = allocate_near_image(size);
    if (!stub) {
        fprintf(stderr, "UC2MP spawn: no nearby trampoline page; bridge rejected\n");
        return -1;
    }
    const int64_t displacement = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image+0x3f8415);
    const int64_t barrier_displacement = (int64_t)(uintptr_t)(stub+16) - (int64_t)(uintptr_t)(image+0x3f88e4);
    const int64_t factory_displacement = (int64_t)(uintptr_t)(stub+32) - (int64_t)(uintptr_t)(image+0x9695f5);
    if (displacement < INT32_MIN || displacement > INT32_MAX ||
        (deferred && (barrier_displacement < INT32_MIN || barrier_displacement > INT32_MAX)) ||
        (factory_hook && (factory_displacement < INT32_MIN || factory_displacement > INT32_MAX))) {
        fprintf(stderr, "UC2MP spawn: trampoline out of rel32 range; bridge rejected\n");
        return -1;
    }
    const unsigned char absolute_jump[6] = {0xff,0x25,0,0,0,0};
    const uintptr_t target = (uintptr_t)uc2mp_spawn_entry;
    memcpy(stub,absolute_jump,sizeof(absolute_jump));
    memcpy(stub+6,&target,sizeof(target));
    if (deferred) {
        const uintptr_t barrier_target = (uintptr_t)uc2mp_spawn_barrier_entry;
        memcpy(stub+16,absolute_jump,sizeof(absolute_jump));
        memcpy(stub+22,&barrier_target,sizeof(barrier_target));
    }
    if (factory_hook) {
        const uintptr_t factory_target=(uintptr_t)uc2mp_spawn_factory_entry;
        memcpy(stub+32,absolute_jump,sizeof(absolute_jump));
        memcpy(stub+38,&factory_target,sizeof(factory_target));
    }
    protect(stub,page_size,5);
    uc2mp_spawn_bridge = (Uc2mpSpawnBridge){image,
        (Uc2mpSpawnLoad)(image+0x56c200), (Uc2mpSpawnFinish)(image+0x56ec70),
        (Uc2mpSpawnOriginal)(image+0x969320), runtime_memory_is_mapped, size};
    uc2mp_spawn_deferred = deferred;
    uc2mp_spawn_barrier_original = (Uc2mpSpawnFinish)(image+0x56bf60);
    uc2mp_spawn_stage=stage;
    uc2mp_spawn_catalog=catalog;
    uc2mp_spawn_catalog_index=catalog_index;
    uc2mp_spawn_load_world=(Uc2mpSpawnLoad)(image+0x56ad90);
    uc2mp_spawn_lookup=(Uc2mpSpawnLookup)(image+0x56c7b0);
    uc2mp_spawn_transform=(Uc2mpSpawnTransform)(image+0x4bdfe0);
    uc2mp_spawn_factory=(Uc2mpSpawnFactory)(image+0x975190);
    uc2mp_player_init_original = (Uc2mpPlayerInit)init_target;
    uc2mp_player_failure_original = (Uc2mpPlayerFailure)failure_target;
    const uintptr_t init_trace = (uintptr_t)uc2mp_player_init_trace;
    const uintptr_t failure_trace = (uintptr_t)uc2mp_player_failure_trace;
    memcpy(image+0x10109a0, &init_trace, 8);
    memcpy(image+0x1010a48, &failure_trace, 8);
    const int32_t relative = (int32_t)displacement;
    memcpy(image+0x3f8411,&relative,sizeof(relative)); /* retain CALL opcode */
    if (deferred) {
        const int32_t barrier_relative = (int32_t)barrier_displacement;
        memcpy(image+0x3f88e0,&barrier_relative,sizeof(barrier_relative));
        puts("UC2MP spawn: original GameInit level-request call wrapped for deferred spawn (diagnostic)");
    }
    if (factory_hook) {
        static const unsigned char campaign_pause_branch[] = {0x0f,0x8c,0x06,0x02,0,0};
        static const unsigned char nop6[] = {0x66,0x0f,0x1f,0x44,0,0};
        if (uc2mp_spawn_range(segments,count,0x5c7c05,sizeof(nop6),5) &&
            !memcmp(image+0x5c7c05,campaign_pause_branch,sizeof(campaign_pause_branch))) {
            memcpy(image+0x5c7c05,nop6,sizeof(nop6));
            uc2mp_spawn_pause=1;
        } else {
            fputs("UC2MP spawn: pause page branch differs; in-match pause left untouched\n",stderr);
        }
        const int32_t factory_relative=(int32_t)factory_displacement;
        memcpy(image+0x9695f1,&factory_relative,4);
        if (stage) puts("UC2MP spawn: original autoSpawn uses retained stage player/camera spawners (diagnostic)");
        else printf("UC2MP spawn: original autoSpawn uses extracted %s spawner %u (diagnostic)\n",
                    catalog->level_name, catalog_index);
    }
    puts("UC2MP spawn: v1.02 map-test hero preload installed at original init call (opt-in)");
    return 1;
rejected:
    fprintf(stderr, "UC2MP spawn: unsupported image/mapping/signature; bridge rejected\n");
    return -1;
}
#endif
