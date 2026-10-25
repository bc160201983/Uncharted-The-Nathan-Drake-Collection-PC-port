/* UC2 1.02 task update hook. Included after uc2mp_spawn_bridge.h.
 * The original callback always executes. Only the observed native GameFrameJob
 * phase is eligible to consume future queued lobby-to-engine requests. */
#ifndef UC2MP_GAME_FRAME_BRIDGE_H
#define UC2MP_GAME_FRAME_BRIDGE_H
static unsigned char *uc2mp_game_frame_image;
static Uc2mpSpawnOriginal uc2mp_game_frame_original;
static atomic_uint uc2mp_game_frame_observations;
static int uc2mp_game_frame_context(uint64_t *raw_out, unsigned *flags_out) {
    int64_t offset; uint64_t raw; uintptr_t manager;
    memcpy(&offset,uc2mp_game_frame_image+0x104ad40,8);
    if (!runtime_thread_read_tls_u64(offset,&raw)) return 0;
    *raw_out=raw;
    const uint64_t index=(raw & 0x8000) ? (raw ^ 0x8000) : UINT64_MAX;
    if (index>5) return 0;
    memcpy(&manager,uc2mp_game_frame_image+0x118e280,8);
    if (!manager || !runtime_memory_is_mapped(manager+index*0x100+0x1b8,1)) return 0;
    const unsigned flags=*(const unsigned char *)(manager+index*0x100+0x1b8);
    *flags_out=flags;
    return (flags&0x40)!=0;
}
static ABI void uc2mp_game_frame_entry(void) {
    uc2mp_game_frame_original();
    uint64_t raw=0; unsigned flags=0;
    const int valid=uc2mp_game_frame_context(&raw,&flags);
    const unsigned n=atomic_fetch_add_explicit(&uc2mp_game_frame_observations,1,memory_order_relaxed);
    if (n<8) {
        printf("UC2MP game frame: post-task native_context=%d raw=%llx flags=%#x observation=%u\n",
               valid,(unsigned long long)raw,flags,n);
        fflush(stdout);
    }
}
static int install_uc2mp_game_frame_bridge(Segment *segments,uint64_t count,uint64_t size) {
    const char *enabled=getenv("UNCHARTED_UC2MP_FRAME_PROBE");
    if (!enabled || strcmp(enabled,"1")) return 0;
    static const unsigned char call[]={0xe8,0x05,0xed,0x65,0x00};
    static const unsigned char original[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
        0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xe4,0xe0,0xff,0xff,0xff};
    if (uc2mp_game_frame_image || !image || size<0x118e288 ||
        !uc2mp_spawn_range(segments,count,0x3fbcf6,sizeof(call),5) ||
        !uc2mp_spawn_range(segments,count,0xa5aa00,sizeof(original),5) ||
        !uc2mp_spawn_range(segments,count,0x104ad40,8,4) ||
        !uc2mp_spawn_range(segments,count,0x118e280,8,6) ||
        memcmp(image+0x3fbcf6,call,sizeof(call)) ||
        memcmp(image+0xa5aa00,original,sizeof(original))) return -1;
    unsigned char *stub=allocate_near_image(size);
    if (!stub) return -1;
    const int64_t delta=(int64_t)(uintptr_t)stub-(int64_t)(uintptr_t)(image+0x3fbcfb);
    if (delta<INT32_MIN || delta>INT32_MAX) return -1;
    const unsigned char jump[]={0xff,0x25,0,0,0,0};
    const uintptr_t target=(uintptr_t)uc2mp_game_frame_entry;
    memcpy(stub,jump,6); memcpy(stub+6,&target,8); protect(stub,page_size,5);
    uc2mp_game_frame_image=image;
    uc2mp_game_frame_original=(Uc2mpSpawnOriginal)(image+0xa5aa00);
    const int32_t relative=(int32_t)delta;
    memcpy(image+0x3fbcf7,&relative,4);
    puts("UC2MP game frame: native post-task context probe installed (no engine mutation)");
    return 1;
}
#endif
