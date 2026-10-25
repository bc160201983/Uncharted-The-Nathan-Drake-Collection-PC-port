#ifndef RUNTIME_UC2MP_DIAG_H
#define RUNTIME_UC2MP_DIAG_H

/* Read-only thread ownership observation. Enabled only by
 * UC2MP_THREAD_DIAG=1 at process start. No guest-memory reads, callbacks,
 * transport polling, or lobby mutations. Bounded to eight native threads
 * per site and four distinct return addresses per thread/site. */
typedef enum {
    UC2MP_DIAG_PAD_READ,
    UC2MP_DIAG_PAD_READ_STATE,
    UC2MP_DIAG_MENU_TEXT,
    UC2MP_DIAG_MENU_ENTER,
    UC2MP_DIAG_MENU_LEAVE,
    UC2MP_DIAG_MENU_READY,
    UC2MP_DIAG_MENU_SET_INDEX,
    UC2MP_DIAG_MENU_ENABLED,
    UC2MP_DIAG_SITE_COUNT
} Uc2mpDiagSite;

void runtime_uc2mp_diag(Uc2mpDiagSite site, const void *return_address);

/* Invoke directly in the observed ABI entry, not in a helper: frame zero is
 * that entry's caller. A tail-jump hook preserves the original guest return.
 * Clang/GCC only; do not walk or dereference a guest stack frame. */
#define UC2MP_THREAD_OBSERVE(site) \
    runtime_uc2mp_diag((site), __builtin_extract_return_addr(__builtin_return_address(0)))
#endif
