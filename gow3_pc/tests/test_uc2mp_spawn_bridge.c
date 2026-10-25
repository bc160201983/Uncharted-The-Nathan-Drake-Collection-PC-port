#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#define ABI __attribute__((sysv_abi))
typedef struct { uint64_t address, size, flags; } Segment;
static unsigned char *image;
static size_t page_size = 4096, mock_size;
static unsigned allocations, protections;
static unsigned char *allocate_near_image(uint64_t size) {
    assert(size == mock_size);
    ++allocations;
    return image + mock_size;
}
static void protect(void *p, size_t size, unsigned flags) {
    assert(p == image + mock_size && size == 4096 && flags == 5);
    ++protections;
}
static unsigned char fake_level[0x40];
static unsigned char fake_proto[0x40];
static unsigned char fake_process[0x48];
static unsigned char fake_world[0xa60], fake_wrapper[0xa60], fake_group[0x28];
static _Alignas(16) unsigned char fake_player_spawn[0x68],fake_camera_spawn[0x68];
static uintptr_t fake_slot;
static int mapped_ok = 1;
int runtime_memory_is_mapped(uintptr_t p, uint64_t size) {
    if (p == (uintptr_t)fake_world || p == (uintptr_t)fake_wrapper) return size<=sizeof(fake_world);
    if (p == (uintptr_t)fake_group) return size<=sizeof(fake_group);
    if (p == (uintptr_t)fake_player_spawn || p == (uintptr_t)fake_camera_spawn) return size<=sizeof(fake_player_spawn);
    if (p == (uintptr_t)fake_level) return mapped_ok && size <= sizeof(fake_level);
    if (p == (uintptr_t)fake_proto) return size <= sizeof(fake_proto);
    if (p == (uintptr_t)fake_process) return size <= sizeof(fake_process);
    if (p == (uintptr_t)&fake_slot) return size <= sizeof(fake_slot);
    return 0; /* Runtime mapper tracks heap allocations, not the ELF image. */
}
#include "uc2mp_spawn_bridge.h"

static unsigned loads, finishes, spawns;
static unsigned barriers;
static unsigned factories;
static int missing_spawner;
static ABI void *fake_world_load(void *manager,const char *name,int sync) {
    assert(manager==image+0x1130390 && sync==1);
    if (!strcmp(name,"stage-multi")) return fake_world;
    assert(!strcmp(name,"stage-multi-pak")); return fake_wrapper;
}
static ABI void *fake_lookup(void *manager,uint32_t sid) {
    assert(manager==image+0x1130390);
    if (missing_spawner) return NULL;
    if (sid==0x2a7ef449) return fake_player_spawn;
    assert(sid==0xe4b94334); return fake_camera_spawn;
}
static ABI void fake_transform(void *out,const void *spawner) { memcpy(out,spawner,32); }
static ABI void fake_factory(const void *player,const void *camera,const char *skin,
                             void *config,void *parent,uintptr_t unused,int flag) {
    assert(player==uc2mp_spawn_player_pose && camera==uc2mp_spawn_camera_pose);
    assert(!strcmp(skin,"hero-default") && config==fake_level && !parent && unused==0xdead && !flag);
    ++factories;
}
static void *factory_on_worker(void *unused) {
    (void)unused;
    assert(uc2mp_spawn_player_pose[0]==22 && uc2mp_spawn_player_pose[7]==1);
    assert(uc2mp_spawn_camera_pose[0]==25 && uc2mp_spawn_camera_pose[7]==1);
    uc2mp_spawn_factory_entry(NULL,NULL,"hero-default",fake_level,NULL,0xdead,0);
    return NULL;
}
static ABI void fake_barrier(void *manager) {
    assert(manager == image+0x1130390);
    assert(!spawns);
    ++barriers;
}
static unsigned init_calls, failure_calls;
static ABI int fake_player_init(void *object, const void *descriptor) {
    assert(object == fake_process && descriptor == fake_level); ++init_calls;
    return 0x0cf60009;
}
static ABI void fake_player_failure(void *object, const char *message) {
    assert(object == fake_process && !strcmp(message, "test failure unchanged")); ++failure_calls;
}
static int load_null, proto_null, finish_ready = 1, finish_unmaps;
static void state(uint32_t value, uint16_t flags) {
    memcpy(fake_level + 0x30, &value, 4);
    memcpy(fake_level + 0x18, &flags, 2);
}
static ABI void *fake_load(void *manager, const char *name, int arg) {
    assert(manager == image + 0x1130390 && arg == 0);
    ++loads;
    if (loads == 1) {
        assert(name == (const char *)image+0xeb05cd);
        return proto_null ? NULL : fake_proto;
    }
    assert(loads == 2 && name == (const char *)image+0xe9466e);
    uint32_t ready; memcpy(&ready,fake_proto+0x30,4); assert(ready == 7);
    return load_null ? NULL : fake_level;
}
static ABI void fake_finish(void *p) {
    if (p == fake_proto) {
        assert(loads == 1 && !spawns);
        uint16_t flags; memcpy(&flags,fake_proto+0x18,2); assert(flags & 0x40);
        ++finishes;
        if (finish_ready) {
            uint32_t ready=7; flags |= 0x80;
            memcpy(fake_proto+0x30,&ready,4); memcpy(fake_proto+0x18,&flags,2);
        }
        return;
    }
    assert(p == fake_level && loads == 2 && !spawns);
    uint16_t flags;
    memcpy(&flags, fake_level + 0x18, 2);
    assert(flags & 0x40);
    ++finishes;
    if (finish_ready) state(7, flags | 0x80);
    if (finish_unmaps) mapped_ok = 0;
}
static ABI void fake_spawn(void) {
    uint16_t flags; uint32_t value;
    memcpy(&flags, fake_level + 0x18, 2);
    memcpy(&value, fake_level + 0x30, 4);
    assert(loads == 2 && value == 7 && (flags & 0xc0) == 0xc0 && !(flags & 0x20));
    ++spawns;
}
static void reset(void) {
    loads = finishes = spawns = 0;
    load_null = proto_null = finish_unmaps = 0; finish_ready = mapped_ok = 1;
    memset(fake_level, 0, sizeof(fake_level));
    memset(fake_proto, 0, sizeof(fake_proto));
    uint32_t ready=7; uint16_t flags=0x80;
    memcpy(fake_proto+0x30,&ready,4); memcpy(fake_proto+0x18,&flags,2);
}
static void env(const char *value) {
#ifdef _WIN32
    assert(!_putenv_s("UNCHARTED_UC2MP_PRELOAD_HERO", value));
#else
    if (*value) assert(!setenv("UNCHARTED_UC2MP_PRELOAD_HERO", value, 1));
    else assert(!unsetenv("UNCHARTED_UC2MP_PRELOAD_HERO"));
#endif
}

/* Optional real-ELF installer check, no guest instructions executed. */
static void real_image(const char *path, int argc, const char *const *argv) {
    FILE *f = fopen(path, "rb"); assert(f);
    unsigned char header[64]; assert(fread(header, 1, 64, f) == 64);
    assert(!memcmp(header, "\177ELF\2\1", 6));
    uint64_t offset; uint16_t entry_size, count;
    memcpy(&offset, header + 32, 8); memcpy(&entry_size, header + 54, 2); memcpy(&count, header + 56, 2);
    assert(entry_size == 56 && count < 64);
    struct Phdr { uint32_t type, flags; uint64_t offset, address, physical, filesz, memsz, alignment; } ph[64];
    assert(!fseek(f, (long)offset, SEEK_SET)); assert(fread(ph, 56, count, f) == count);
    Segment segments[64]; uint64_t ns = 0, size = 0;
    for (unsigned i=0; i<count; ++i) if (ph[i].type == 1 || ph[i].type == 0x61000010) {
        assert(ph[i].address < 0x10000000 && ph[i].memsz < 0x10000000);
        segments[ns++] = (Segment){ph[i].address, ph[i].memsz, ph[i].flags};
        if (ph[i].address + ph[i].memsz > size) size = ph[i].address + ph[i].memsz;
    }
    assert(size && size < 0x10000000);
    free(image); mock_size = (size_t)size;
    image = calloc(1, mock_size + page_size); assert(image);
    for (unsigned i=0; i<count; ++i) if (ph[i].type == 1 || ph[i].type == 0x61000010) {
        assert(!fseek(f, (long)ph[i].offset, SEEK_SET));
        assert(fread(image + ph[i].address, 1, (size_t)ph[i].filesz, f) == ph[i].filesz);
    }
    /* Apply only the two real relative relocations inspected by the installer.
     * Match the loader's pre-install state, without executing initializers. */
    uint64_t dynamic_offset=0, dynamic_size=0, lib_offset=0, rela_offset=0, rela_size=0;
    for (unsigned i=0;i<count;++i) {
        if (ph[i].type==2) { dynamic_offset=ph[i].offset; dynamic_size=ph[i].filesz; }
        if (ph[i].type==0x61000000) lib_offset=ph[i].offset;
    }
    assert(dynamic_offset && lib_offset);
    assert(!fseek(f,(long)dynamic_offset,SEEK_SET));
    for (uint64_t i=0;i<dynamic_size;i+=16) {
        uint64_t pair[2]; assert(fread(pair,8,2,f)==2);
        if (pair[0]==0x6100002f) rela_offset=pair[1];
        if (pair[0]==0x61000031) rela_size=pair[1];
    }
    unsigned found=0;
    assert(!fseek(f,(long)(lib_offset+rela_offset),SEEK_SET));
    for (uint64_t i=0;i<rela_size;i+=24) {
        uint64_t rel[3]; assert(fread(rel,8,3,f)==3);
        if (rel[0]==0x10109a0 || rel[0]==0x1010a48) {
            assert(rel[1]==8);
            const uintptr_t target=(uintptr_t)image+rel[2];
            memcpy(image+rel[0],&target,8); ++found;
        }
    }
    assert(found==2); fclose(f);
    unsigned char original[15]; memcpy(original, image+0x3f8406, 15);
    env(""); assert(install_uc2mp_spawn_bridge(segments, ns, size, argc, argv) == 0);
    assert(!memcmp(original, image+0x3f8406, 15) && !allocations);
    env("1");
#ifdef _WIN32
    assert(!_putenv_s("UNCHARTED_UC2MP_DEFER_SPAWN", "1"));
    assert(!_putenv_s("UNCHARTED_UC2MP_STAGE_SPAWN", "1"));
#else
    assert(!setenv("UNCHARTED_UC2MP_DEFER_SPAWN", "1", 1));
    assert(!setenv("UNCHARTED_UC2MP_STAGE_SPAWN", "1", 1));
#endif
    image[0x56c200] ^= 1;
    assert(install_uc2mp_spawn_bridge(segments, ns, size, argc, argv) == -1);
    assert(!memcmp(original, image+0x3f8406, 15) && !allocations);
    image[0x56c200] ^= 1;
    assert(install_uc2mp_spawn_bridge(segments, ns, size, argc, argv) == 1);
    assert(allocations == 1 && protections == 1);
    assert(!memcmp(original, image+0x3f8406, 11)); // conditional + CALL opcode retained
    int32_t relative; memcpy(&relative, image+0x3f8411, 4);
    assert(image+0x3f8415+relative == image+mock_size);
    assert(!memcmp(image+mock_size, "\xff\x25\0\0\0\0", 6));
    uintptr_t target; memcpy(&target, image+mock_size+6, 8);
    assert(target == (uintptr_t)uc2mp_spawn_entry);
    memcpy(&target,image+0x10109a0,8); assert(target==(uintptr_t)uc2mp_player_init_trace);
    memcpy(&target,image+0x1010a48,8); assert(target==(uintptr_t)uc2mp_player_failure_trace);
    memcpy(&relative,image+0x3f88e0,4);
    assert(image+0x3f88e4+relative == image+mock_size+16);
    memcpy(&target,image+mock_size+22,8);
    assert(target == (uintptr_t)uc2mp_spawn_barrier_entry);
    memcpy(&relative,image+0x9695f1,4);
    assert(image+0x9695f5+relative == image+mock_size+32);
    memcpy(&target,image+mock_size+38,8); assert(target==(uintptr_t)uc2mp_spawn_factory_entry);
    assert(install_uc2mp_spawn_bridge(segments, ns, size, argc, argv) == -1 && allocations == 1);
    puts("UC2MP spawn: real ELF signatures/segments and call trampoline validated; no guest execution");
}

int main(int argc, char **argv) {
    const char *args[] = {"eboot.bin", "ispartofuccollection", "level", "stage-multi", "player"};
    const char *menu[] = {"eboot.bin", "ispartofuccollection"};
    const char *wrong[] = {"eboot.bin", "level", "story-level", "player"};
    env(""); assert(!uc2mp_spawn_requested(5, args));
    env("true"); assert(!uc2mp_spawn_requested(5, args));
    env("1"); assert(uc2mp_spawn_requested(5, args));
    assert(!uc2mp_spawn_requested(2, menu) && !uc2mp_spawn_requested(4, wrong));
    const char *warzone[] = {"eboot.bin", "level", "warzone-multi", "level", "sky-multi-warzone", "player"};
    const char *foreign[] = {"eboot.bin", "level", "warzone-multi", "level", "sky-village-multi", "player"};
    const char *sky_first[] = {"eboot.bin", "level", "sky-multi-warzone", "level", "warzone-multi", "player"};
    assert(uc2mp_spawn_requested(6, warzone) && !uc2mp_spawn_requested(6, foreign) &&
           !uc2mp_spawn_requested(6, sky_first));
    assert(install_uc2mp_spawn_bridge(NULL, 0, 0, 2, menu) == 0 && !allocations);
    assert(!uc2mp_spawn_requested(4, args)); // no player
    assert(install_uc2mp_spawn_bridge(NULL, 0, 0, 5, args) == -1 && !allocations);
    image = calloc(1, 0x1160000); assert(image);
    Uc2mpSpawnBridge bridge = {image, fake_load, fake_finish, fake_spawn, runtime_memory_is_mapped, 0x1160000};
    uc2mp_player_init_original=fake_player_init;
    uc2mp_player_failure_original=fake_player_failure;
    for (int active=0; active<=1; ++active) {
        uc2mp_spawn_trace_active=active;
        assert(uc2mp_player_init_trace(fake_process,fake_level)==0x0cf60009);
        uc2mp_player_failure_trace(fake_process,"test failure unchanged");
    }
    assert(init_calls==2 && failure_calls==2); uc2mp_spawn_trace_active=0;
    Uc2mpSpawnSnapshot snap = uc2mp_spawn_snapshot(&bridge);
    assert(snap.readable && !snap.valid && !snap.slot);
    uintptr_t slot_address = (uintptr_t)&fake_slot;
    uint32_t generation = 3, process_type = 0xa5a63e9b;
    memcpy(image+0x115bd20, &slot_address, 8);
    memcpy(image+0x115bd28, &generation, 4);
    snap = uc2mp_spawn_snapshot(&bridge);
    assert(snap.stable && !snap.valid && !snap.object);
    fake_slot = (uintptr_t)fake_process;
    memcpy(fake_process+0x44, &generation, 4);
    memcpy(fake_process+0x30, &process_type, 4);
    snap = uc2mp_spawn_snapshot(&bridge);
    assert(snap.valid && snap.process_type == process_type);
    generation = 4; memcpy(fake_process+0x44, &generation, 4);
    assert(!uc2mp_spawn_snapshot(&bridge).valid); // recycled object
    fake_slot = 1; assert(!uc2mp_spawn_snapshot(&bridge).valid); // unmapped object
    memset(image+0x115bd20, 0, 12);
    uc2mp_spawn_load_world=fake_world_load; uc2mp_spawn_lookup=fake_lookup;
    uc2mp_spawn_transform=fake_transform; uc2mp_spawn_factory=fake_factory;
    uint32_t ready_world=7; uint16_t world_flags=0x280,world_kind=1;
    memcpy(fake_world,&world_kind,2); memcpy(fake_world+0x18,&world_flags,2); memcpy(fake_world+0x30,&ready_world,4);
    memcpy(fake_wrapper,fake_world,sizeof(fake_world));
    uintptr_t group_address=(uintptr_t)fake_group,wrapper_address=(uintptr_t)fake_wrapper;
    memcpy(fake_wrapper+0x9a0,&group_address,8);
    int32_t spawn_count=85; memcpy(fake_group+0x20,&spawn_count,4);
    const float player_pose[8]={22,1,20,0,0,0,0,1},camera_pose[8]={25,3,28,0,0,0,0,1};
    memcpy(fake_player_spawn,player_pose,32); memcpy(fake_camera_spawn,camera_pose,32);
    memcpy(fake_player_spawn+0x58,&wrapper_address,8); memcpy(fake_camera_spawn+0x58,&wrapper_address,8);
    uc2mp_spawn_factory_entry(NULL,NULL,"hero-default",fake_level,NULL,0xdead,0);
    assert(!factories); // no unvalidated poses reach the native factory
    assert(uc2mp_spawn_prepare_stage(&bridge));
    uc2mp_spawn_factory_entry(NULL,NULL,"hero-default",fake_level,NULL,0xdead,0);
    assert(factories==1 && !memcmp(uc2mp_spawn_player_pose,player_pose,32));
    assert(!memcmp(uc2mp_spawn_camera_pose,camera_pose,32));
    assert(uc2mp_spawn_prepare_stage(&bridge));
    pthread_t workers[2];
    assert(!pthread_create(&workers[0],NULL,factory_on_worker,NULL));
    assert(!pthread_create(&workers[1],NULL,factory_on_worker,NULL));
    assert(!pthread_join(workers[0],NULL) && !pthread_join(workers[1],NULL));
    assert(factories==2); // publication survives worker migration; exactly one claims
    uc2mp_spawn_poses_ready=0;
    missing_spawner=1; assert(!uc2mp_spawn_prepare_stage(&bridge)); missing_spawner=0;
    spawn_count=84; memcpy(fake_group+0x20,&spawn_count,4); assert(!uc2mp_spawn_prepare_stage(&bridge));
    spawn_count=85; memcpy(fake_group+0x20,&spawn_count,4);
    memcpy(fake_player_spawn+0x60,&wrapper_address,8); assert(!uc2mp_spawn_prepare_stage(&bridge));
    memset(fake_player_spawn+0x60,0,8);
    float invalid=NAN; memcpy(fake_player_spawn,&invalid,4); assert(!uc2mp_spawn_prepare_stage(&bridge));
    memcpy(fake_player_spawn,player_pose,32);
    assert(!uc2mp_spawn_poses_ready);
    reset(); state(1, 0x20); assert(uc2mp_spawn_preload(&bridge));
    assert(loads == 2 && finishes == 1 && spawns == 1);
    reset(); state(7, 0xa0); assert(uc2mp_spawn_preload(&bridge));
    assert(!finishes && spawns == 1);
    reset(); state(7,0x80); memset(fake_proto,0,sizeof(fake_proto));
    assert(uc2mp_spawn_preload(&bridge) && finishes==1 && loads==2 && spawns==1);
    reset(); memset(fake_proto,0,sizeof(fake_proto)); finish_ready=0;
    assert(!uc2mp_spawn_preload(&bridge) && finishes==1 && loads==1 && !spawns);
    reset(); load_null = 1; assert(!uc2mp_spawn_preload(&bridge) && !finishes && !spawns);
    reset(); proto_null = 1; assert(!uc2mp_spawn_preload(&bridge) && loads == 1 && !finishes && !spawns);
    reset(); mapped_ok = 0; assert(!uc2mp_spawn_preload(&bridge) && !finishes && !spawns);
    reset(); finish_ready = 0; assert(!uc2mp_spawn_preload(&bridge) && finishes == 1 && !spawns);
    reset(); state(7, 0); assert(!uc2mp_spawn_preload(&bridge) && !finishes && !spawns);
    reset(); finish_unmaps = 1; assert(!uc2mp_spawn_preload(&bridge) && finishes == 1 && !spawns);
    reset(); state(7,0x80);
    uc2mp_spawn_bridge=bridge;
    uc2mp_spawn_barrier_original=fake_barrier;
    uc2mp_spawn_deferred=1;
    uc2mp_spawn_entry();
    assert(uc2mp_spawn_pending && !loads && !spawns);
    uc2mp_spawn_barrier_entry(image+0x1130390);
    assert(barriers==1 && spawns==1 && !uc2mp_spawn_pending && uc2mp_spawn_completed);
    spawns=0;
    uc2mp_spawn_entry();
    uc2mp_spawn_barrier_entry(image+0x1130390);
    assert(barriers==2 && !spawns && loads==2); /* no repeated creation */
    uc2mp_spawn_pending=uc2mp_spawn_completed=uc2mp_spawn_deferred=0;
    memset(&uc2mp_spawn_bridge,0,sizeof(uc2mp_spawn_bridge));
    if (argc > 1) real_image(argv[1], 5, args);
    free(image); env("");
    puts("UC2MP spawn: opt-in, sequencing, readiness and failure isolation passed");
    return 0;
}
