#define U3_MENU_HOST_TEST
#include "../u3_menu_entry.c"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(void) {
    _Alignas(8) uint8_t pages_data[149*0x80]={0},dst_data[10*0x68]={0},src_data[8*0x68]={0},before[sizeof(dst_data)];
    Collection pages={149,0,pages_data},dst={10,0,dst_data},src={8,0,src_data};
    *(uint32_t *)(pages_data+8)=0x82d5c00b;
    *(uint32_t *)(pages_data+0x80+8)=0x0da46ad0;
    *(Collection **)(pages_data+0x48)=&dst;
    *(Collection **)(pages_data+0x80+0x48)=&src;
    uint8_t *s=src_data+4*0x68,*d=dst_data+2*0x68;
    *(uint64_t *)s=0x1234567890; *(uint32_t *)(s+8)=0xed9d8ae1;
    *(uint64_t *)(s+0x10)=2; *(uint64_t *)(s+0x20)=0x1234567800;
    *(uint32_t *)(s+0x28)=0xf75b3a65;
    *(uint64_t *)(d+0x10)=1; *(uint64_t *)(d+0x18)=10;
    memcpy(before,dst_data,sizeof before);
    assert(u3_restore_menu_entry(&pages)==1);
    assert(!memcmp(s,d,0x68));
    assert(!memcmp(before,dst_data,2*0x68));
    assert(!memcmp(before+3*0x68,dst_data+3*0x68,7*0x68));
    assert(u3_restore_menu_entry(&pages)==2);
    d[0x50]=1; assert(u3_restore_menu_entry(&pages)==0);
    memcpy(dst_data,before,sizeof before);
    *(uint32_t *)(s+0x40)=1; assert(u3_restore_menu_entry(&pages)==0);
    assert(!memcmp(before,dst_data,sizeof before));
    *(uint32_t *)(s+0x40)=0; dst.count=9;
    assert(u3_restore_menu_entry(&pages)==0);
    assert(!memcmp(before,dst_data,sizeof before));
    puts("PASS: relocated copy, neighbor preservation, idempotence and rejection guards");
}
