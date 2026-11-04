/* Diagnostic menu restoration only. Native Multiplayer callbacks are RET stubs.
 * Copy the existing relocated item; never transplant serialized pointers.
 */
#include <stdint.h>
#include <stddef.h>
typedef struct { uint32_t count, reserved; uint8_t *items; } Collection;
static uint32_t word(const void *p) { return *(const uint32_t *)p; }
static uint64_t quad(const void *p) { return *(const uint64_t *)p; }

int u3_restore_menu_entry(Collection *pages) {
    if (!pages || pages->count!=149 || !pages->items) return 0;
    uint8_t *profile=0,*demo=0;
    for(uint32_t i=0;i<pages->count;i++) {
        uint8_t *p=pages->items+i*0x80;
        if(word(p+8)==0x82d5c00b) { if(profile)return 0; profile=p; }
        if(word(p+8)==0x0da46ad0) { if(demo)return 0; demo=p; }
    }
    if(!profile || !demo) return 0;
    Collection *dst=*(Collection **)(profile+0x48),*src=*(Collection **)(demo+0x48);
    if(!dst || !src || dst->count!=10 || src->count!=8 || !dst->items || !src->items) return 0;
    uint8_t *s=src->items+4*0x68,*d=dst->items+2*0x68;
    if(word(s+8)!=0xed9d8ae1 || quad(s+0x10)!=2 || word(s+0x28)!=0xf75b3a65 || word(s+0x40)!=0 || !quad(s) || !quad(s+0x20)) return 0;
    /* Idempotence is allowed only for a byte-identical previously copied item. */
    if(word(d+8)==0xed9d8ae1) {
        for(unsigned i=0;i<0x68;i++) if(d[i]!=s[i]) return 0;
        return 2;
    }
    if(quad(d) || quad(d+8) || quad(d+0x10)!=1 || quad(d+0x18)!=10 || quad(d+0x20) || word(d+0x40)) return 0;
    for(unsigned i=0;i<0x68;i++) d[i]=s[i];
    return 1;
}

#ifndef U3_MENU_HOST_TEST
__attribute__((section(".text.entry"))) int u3_menu_entry(uintptr_t base) {
    typedef void **(*Lookup)(uint32_t);
    typedef void (*Load)(uint32_t,uint32_t *);
    Lookup lookup=(Lookup)(base+0x11f2bf0);
    void **symbol=lookup(0x74ffceeb);
    if(!symbol || !*symbol) {
        uint32_t tag=0x12;
        ((Load)(base+0x11f85e0))(0xd006e7b5,&tag);
        symbol=lookup(0x74ffceeb);
    }
    return symbol ? u3_restore_menu_entry((Collection *)*symbol) : 0;
}
#endif
