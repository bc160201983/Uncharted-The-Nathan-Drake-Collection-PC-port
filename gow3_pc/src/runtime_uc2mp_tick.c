#include "runtime_uc2mp_tick.h"
#include <stdatomic.h>
#include <stdint.h>

/* Only the identity is shared. Callback/context/busy are accessed exclusively
 * through TLS, so a foreign pad reader cannot race the owner's teardown. */
static atomic_uintptr_t owner;
static _Thread_local unsigned char identity;
static _Thread_local Uc2mpTickCallback local_callback;
static _Thread_local void *local_context;
static _Thread_local int running;

int runtime_uc2mp_tick_is_owner(void) {
    return local_callback && atomic_load_explicit(&owner, memory_order_acquire) ==
        (uintptr_t)&identity;
}

int runtime_uc2mp_tick_bind(Uc2mpTickCallback callback, void *context) {
    if (!callback || running) return 0;
    uintptr_t self = (uintptr_t)&identity;
    uintptr_t expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&owner, &expected, self,
            memory_order_acq_rel, memory_order_acquire)) {
        return expected == self && local_callback == callback && local_context == context;
    }
    local_context = context;
    local_callback = callback;
    return 1;
}

int runtime_uc2mp_tick_unbind(void) {
    if (!runtime_uc2mp_tick_is_owner()) return 0;
    local_callback = 0;
    local_context = 0;
    /* Keep ownership until a running callback returns, including a callback
     * that unbinds itself. A foreign thread cannot acquire a half-closed lobby. */
    if (!running) atomic_store_explicit(&owner, 0, memory_order_release);
    return 1;
}

void runtime_uc2mp_tick_run(void) {
    if (running || !runtime_uc2mp_tick_is_owner()) return;
    Uc2mpTickCallback callback = local_callback;
    void *context = local_context;
    running = 1;
    callback(context);
    running = 0;
    if (!local_callback) atomic_store_explicit(&owner, 0, memory_order_release);
}
