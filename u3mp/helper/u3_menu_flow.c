/* Diagnostic native page routing. Does not create a backend or claim a session.
 * LAN first page follows PS3 callback27fdf0. Online lobby is a dependency probe.
 */
#include <stdint.h>
uint32_t u3_route_page(uint32_t current,int lan) {
    if(lan) return current==0x1e66a744 ? 0x4608c1ad : 0;
    if(current==0x82d5c00b || current==0x2c3d7d0f) return 0x1e66a744;
    if(current==0x1e66a744) return 0x53cbb5f2;
    return 0;
}
#ifndef U3_FLOW_TEST
__attribute__((section(".text.entry"))) int u3_menu_flow(uintptr_t base,int lan) {
    uint8_t *menu=(uint8_t *)(base+0x17bd080);
    /* Native ShowPage writes 7/8 while changing pages. Do not interpret a
     * repeated Select as another choice during that transition. The 0x61
     * mask used elsewhere describes hidden/reset states, not interactive ones.
     */
    uint32_t state=*(uint32_t *)(menu+8);
    if(state==7 || state==8) return 0;
    uint32_t depth=*(uint32_t *)(menu+0xdb0);
    if(depth>=160) return 0;
    uint8_t *page=*(uint8_t **)(menu+0x8b0+(uintptr_t)depth*8);
    if(!page) return 0;
    uint32_t target=u3_route_page(*(uint32_t *)(page+0x10),lan);
    if(!target) return 0;
    if(target==0x53cbb5f2) {
        typedef void **(*Lookup)(uint32_t);
        Lookup lookup=(Lookup)(base+0x11f2bf0);
        void **sym=lookup(0xbd3bb3b9);
        if(!sym || !*sym) {
            uint32_t tag=0x12;
            ((void (*)(uint32_t,uint32_t *))(base+0x11f85e0))(0x4240ef2e,&tag);
            sym=lookup(0xbd3bb3b9);
        }
        if(!sym || !*sym) return 0;
    }
    return ((int (*)(void *,uint32_t,int,int))(base+0x651940))(menu,target,0,0)&255;
}
#endif
