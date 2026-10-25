#include "../src/runtime_pad.c"
#include <assert.h>
int gow3gpu_overlay_captures_input(void) { return 1; }
uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name) {
    (void)table; (void)count; (void)name; return 0;
}
static unsigned ticks;
static void check_unlocked(void *context) {
    assert(context==&ticks);
    assert(!pthread_mutex_trylock(&lock));
    assert(!pthread_mutex_unlock(&lock));
    ++ticks;
}
static void *foreign_read(void *unused) {
    (void)unused;
    PadData out;
    assert(pad_read(PAD_HANDLE,&out,1)>=0);
    return NULL;
}
int main(void) {
    PadData current={0},out;
    current.left_x=current.left_y=current.right_x=current.right_y=128;
    current.connected=1; current.timestamp=10000;
    assert(buffered_report(&out,&current)==1);
    memset(&out,0xa5,sizeof(out));
    current.timestamp=10001;
    assert(buffered_report(&out,&current)==0 && out.buttons==0xa5a5a5a5);
    current.buttons=BTN_CROSS; current.timestamp=10002;
    assert(buffered_report(&out,&current)==1 && out.buttons==BTN_CROSS);
    for (unsigned i=0;i<100;++i) {
        current.timestamp=10003+i;
        assert(buffered_report(&out,&current)==0);
    }
    current.buttons=0; current.timestamp=10110;
    assert(buffered_report(&out,&current)==1 && !out.buttons);
    current.timestamp=14110;
    assert(buffered_report(&out,&current)==1);
    opened=1;
    assert(runtime_uc2mp_tick_bind(check_unlocked,&ticks));
    assert(pad_read(PAD_HANDLE,&out,1)>=0 && ticks==1);
    assert(pad_read(PAD_HANDLE,&out,1)>=0 && ticks==2);
    assert(pad_read(99,&out,1)==ERR_INVALID_HANDLE && ticks==2);
    assert(pad_read(PAD_HANDLE,NULL,1)==ERR_INVALID_ARG && ticks==2);
    pthread_t thread;
    assert(!pthread_create(&thread,NULL,foreign_read,NULL));
    assert(!pthread_join(thread,NULL));
    assert(ticks==2);
    assert(runtime_uc2mp_tick_unbind());
    assert(pad_read(PAD_HANDLE,&out,1)>=0 && ticks==2);
    puts("Pad tick runs outside mutex on its owner, including overlay capture; foreign reads ignored");
    puts("Buffered pad press/release and duplicate-report tests passed");
}
