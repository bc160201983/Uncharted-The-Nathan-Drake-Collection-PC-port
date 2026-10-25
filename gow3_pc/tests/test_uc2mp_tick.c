#include "runtime_uc2mp_tick.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>

static unsigned calls;
static void count(void *context) {
    assert(context==&calls);
    ++calls;
    runtime_uc2mp_tick_run(); /* Reentrant dispatch must not recurse. */
}
static void *foreign(void *unused) {
    (void)unused;
    for (unsigned i=0;i<10000;++i) {
        assert(!runtime_uc2mp_tick_is_owner());
        assert(!runtime_uc2mp_tick_bind(count,&calls));
        assert(!runtime_uc2mp_tick_unbind());
        runtime_uc2mp_tick_run();
    }
    return NULL;
}
static void join_foreign(void) {
    pthread_t thread;
    assert(!pthread_create(&thread,NULL,foreign,NULL));
    assert(!pthread_join(thread,NULL));
}
static void close_during_callback(void *context) {
    assert(context==&calls);
    ++calls;
    assert(runtime_uc2mp_tick_unbind());
    runtime_uc2mp_tick_run();
    assert(!runtime_uc2mp_tick_bind(count,&calls));
    /* Ownership stays reserved until this callback actually returns. */
    join_foreign();
}
static void *new_owner(void *unused) {
    (void)unused;
    assert(runtime_uc2mp_tick_bind(count,&calls));
    runtime_uc2mp_tick_run();
    assert(runtime_uc2mp_tick_unbind());
    return NULL;
}
int main(void) {
    assert(!runtime_uc2mp_tick_is_owner());
    assert(!runtime_uc2mp_tick_bind(NULL,&calls));
    runtime_uc2mp_tick_run();
    assert(runtime_uc2mp_tick_bind(count,&calls));
    assert(runtime_uc2mp_tick_bind(count,&calls));
    assert(!runtime_uc2mp_tick_bind(count,NULL));
    assert(!runtime_uc2mp_tick_bind(close_during_callback,&calls));
    pthread_t thread;
    assert(!pthread_create(&thread,NULL,foreign,NULL));
    for (unsigned i=0;i<10000;++i) runtime_uc2mp_tick_run();
    assert(!pthread_join(thread,NULL));
    assert(calls==10000);
    assert(runtime_uc2mp_tick_unbind());
    assert(!runtime_uc2mp_tick_unbind());
    runtime_uc2mp_tick_run();
    assert(calls==10000);
    assert(runtime_uc2mp_tick_bind(close_during_callback,&calls));
    runtime_uc2mp_tick_run();
    assert(calls==10001 && !runtime_uc2mp_tick_is_owner());
    assert(!pthread_create(&thread,NULL,new_owner,NULL));
    assert(!pthread_join(thread,NULL));
    assert(calls==10002);
    assert(runtime_uc2mp_tick_bind(count,&calls));
    runtime_uc2mp_tick_run();
    assert(calls==10003);
    assert(runtime_uc2mp_tick_unbind());
    puts("UC2MP tick: owner-only dispatch, reentrancy, teardown and rebind passed");
}
