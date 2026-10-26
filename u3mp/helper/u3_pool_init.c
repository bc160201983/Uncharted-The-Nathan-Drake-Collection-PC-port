/* Experimental native PS4 fixed-pool reconstruction.
 * Layout corroboration: PS4 0x1003850, 0x125dc70 and 0x748bd0.
 * Capacities: PS3 NetGameManager::Init 0x305d2c.
 * This restores two containers, not the complete NetInit lifecycle.
 */
#include <stdint.h>
#include <stddef.h>

typedef struct U3Map {
    void **buckets;
    void *free_head;
    uint32_t stride, capacity;
    void *pool, *sentinel, *allocation;
    void **bucket_end;
    uint32_t reserved38, bucket_count;
    uint8_t initialized;
    uint8_t reserved41[15];
    uint64_t lock50;
    uint8_t reserved58[24];
} U3Map;
_Static_assert(offsetof(U3Map, stride)==0x10,"stride");
_Static_assert(offsetof(U3Map, capacity)==0x14,"capacity");
_Static_assert(offsetof(U3Map, pool)==0x18,"pool");
_Static_assert(offsetof(U3Map, bucket_count)==0x3c,"buckets");
_Static_assert(offsetof(U3Map, initialized)==0x40,"valid");
_Static_assert(offsetof(U3Map, lock50)==0x50,"lock");
_Static_assert(sizeof(U3Map)==0x70,"map size");

static size_t pool_prefix(uint32_t n) {
    uint32_t buckets=(n+15u)&~15u;
    return (((size_t)buckets+1u)*8u+15u)&~(size_t)15;
}
size_t u3_pool_bytes(uint32_t n,uint32_t stride) {
    if (!n || n>256 || stride<24 || stride>200 || (stride&7)) return 0;
    return pool_prefix(n)+((size_t)n+1)*stride;
}
int u3_pool_initialize(U3Map *m,void *block,size_t bytes,uint32_t n,uint32_t stride) {
    size_t required=u3_pool_bytes(n,stride);
    if (!m || !block || !required || bytes<required || ((uintptr_t)block&15)) return 0;
    if (m->buckets || m->initialized) return 0;
    uint32_t count=(n+15u)&~15u;
    uint8_t *pool=(uint8_t *)block+pool_prefix(n);
    void *sentinel=pool+(size_t)n*stride;
    void **buckets=(void **)block;
    for (uint32_t i=0;i<count;i++) buckets[i]=0;
    buckets[count]=sentinel;
    ((void **)sentinel)[0]=sentinel;
    ((void **)sentinel)[1]=sentinel;
    for(uint32_t i=0;i<n;i++) {
        void **node=(void **)(pool+(size_t)i*stride);
        node[0]=i+1<n ? pool+(size_t)(i+1)*stride : 0;
        node[1]=i ? pool+(size_t)(i-1)*stride : 0;
    }
    m->buckets=buckets;
    m->free_head=pool;
    m->stride=stride;
    m->capacity=n;
    m->pool=pool;
    m->sentinel=sentinel;
    m->allocation=block;
    m->bucket_end=buckets+count;
    m->bucket_count=count;
    m->lock50=0;
    m->initialized=1;
    return 1;
}

#ifndef U3_POOL_ONLY
typedef void *(*NativeAllocate)(void *,size_t,size_t,const char *,uint32_t,const char *);
typedef void (*NativePushScope)(uint32_t *,const char *,uint32_t,const char *);
/* Entry ABI is the guest's System V x86-64 ABI, including in the Windows host. */
__attribute__((section(".text.entry")))
int u3_initialize_manager_pools(uintptr_t image) {
    U3Map *maps[2]={(U3Map *)(image+0x17cb060+0x6520),(U3Map *)(image+0x17cb060+0x6590)};
    const uint32_t capacities[2]={16,256},strides[2]={200,64};
    uintptr_t gs;
    __asm__ volatile("movq %%gs:0,%0":"=r"(gs));
    uintptr_t tls_offset=*(uintptr_t *)(image+0x16e2f28);
    uintptr_t stack=*(uintptr_t *)(gs+tls_offset);
    if(!stack) return 0;
    uint32_t depth=*(uint32_t *)(stack+0x730);
    /* Native PushAllocator accepts depth zero (0..45). UI callbacks can
     * legitimately have no existing scope; our own scope supplies one. */
    if(depth>=46) return 0;
    const char *empty=(const char *)(image+0x13ac250);
    /* Match the surviving PS4 initializer at 0x125dc70. Its context-5
     * scope prevents general data allocations from using the process factory's
     * temporary allocator (which requires exactly the returned process block).
     */
    uint32_t tag=5;
    ((NativePushScope)(image+0xda1590))(&tag,empty,0,empty);
    if(*(uint32_t *)(stack+0x730)!=depth+1) return 0;
    int result=0;
    void *allocator=*(void **)(stack+(size_t)depth*0x28);
    if(!allocator) goto pop;
    NativeAllocate allocate=*(NativeAllocate *)(*(uintptr_t *)allocator+0x48);
    for(unsigned i=0;i<2;i++) {
        U3Map *m=maps[i];
        if(m->buckets && m->initialized) {
            if(m->capacity!=capacities[i] || m->stride!=strides[i]) goto pop;
            continue;
        }
        if(m->buckets || m->initialized) goto pop;
        size_t bytes=u3_pool_bytes(capacities[i],strides[i]);
        void *block=allocate(allocator,bytes,16,empty,0,empty);
        if(!u3_pool_initialize(m,block,bytes,capacities[i],strides[i])) goto pop;
    }
    result=1;
pop:
    /* Same five-qword clear and depth decrement as native PopAllocator. */
    if(*(uint32_t *)(stack+0x730)!=depth+1) return 0;
    for(unsigned i=0;i<5;i++) *(uintptr_t *)(stack+(size_t)depth*0x28+i*8)=0;
    *(uint32_t *)(stack+0x730)=depth;
    return result;
}
#endif
