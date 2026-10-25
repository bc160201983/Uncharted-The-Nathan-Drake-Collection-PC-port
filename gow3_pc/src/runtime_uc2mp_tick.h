#ifndef RUNTIME_UC2MP_TICK_H
#define RUNTIME_UC2MP_TICK_H

/* One game thread owns the callback and its context for the entire lobby
 * lifetime. Binding on another thread never transfers ownership. The owner
 * must unbind before destroying the context or exiting. No worker is created.
 * Call run only outside the pad mutex; the callback may perform nonblocking IO.
 * Long loading stalls without pad/menu reads can still exceed LAN timeouts. */
typedef void (*Uc2mpTickCallback)(void *context);
int runtime_uc2mp_tick_bind(Uc2mpTickCallback callback, void *context);
int runtime_uc2mp_tick_is_owner(void);
int runtime_uc2mp_tick_unbind(void);
void runtime_uc2mp_tick_run(void);

#endif
