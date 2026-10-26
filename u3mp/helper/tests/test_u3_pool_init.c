#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define U3_POOL_ONLY
#include "../u3_pool_init.c"
static void check(unsigned n,unsigned stride) {
    U3Map m={0};size_t size=u3_pool_bytes(n,stride);
    unsigned char *raw=malloc(size+48);
    unsigned char *block=(unsigned char *)(((uintptr_t)(raw+16)+15)&~(uintptr_t)15);
    memset(raw,0xa5,size+48);
    assert(!u3_pool_initialize(&m,block,size-1,n,stride));
    assert(m.buckets==0);
    assert(u3_pool_initialize(&m,block,size,n,stride));
    assert(((uintptr_t)m.pool&15)==0);
    assert(m.bucket_end==m.buckets+m.bucket_count);
    assert(*m.bucket_end==m.sentinel);
    assert(((void **)m.sentinel)[0]==m.sentinel && ((void **)m.sentinel)[1]==m.sentinel);
    for(unsigned i=0;i<m.bucket_count;i++) assert(m.buckets[i]==0);
    unsigned count=0;void **previous=0;
    for(void **node=m.free_head;node;node=node[0]) {
        assert(count<n);assert(node[1]==previous);
        assert((unsigned char *)node==(unsigned char *)m.pool+(size_t)count*stride);
        previous=node;count++;
    }
    assert(count==n);
    assert(block[-1]==0xa5 && block[size]==0xa5);
    assert(!u3_pool_initialize(&m,block,size,n,stride));
    /* Exercise a bucket insertion/removal and the pool's exhausted boundary. */
    void **first=m.free_head;m.free_head=first[0];first[0]=m.buckets[3];first[1]=0;m.buckets[3]=first;
    assert(m.buckets[3]==m.pool);m.buckets[3]=first[0];first[0]=m.free_head;m.free_head=first;
    assert(m.free_head==m.pool && m.buckets[3]==0);
    for(unsigned i=0;i<n;i++) {assert(m.free_head);m.free_head=*(void **)m.free_head;}
    assert(!m.free_head);
    printf("capacity=%u stride=%u bytes=%zu: alignment, sentinel, full free list, guards, insert/remove, exhaustion OK\n",n,stride,size);
    free(raw);
}
int main(void) {
    assert(u3_pool_bytes(0,64)==0);assert(u3_pool_bytes(257,64)==0);assert(u3_pool_bytes(16,25)==0);
    check(16,200);check(256,64);return 0;
}

