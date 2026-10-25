#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "runtime_uc2mp_diag.h"
#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#endif

enum { MAX_THREADS = 8, MAX_RETURNS = 4 };
typedef struct {
    uint64_t thread;
    uintptr_t returns[MAX_RETURNS];
    unsigned return_count;
    int used;
} Observation;
static Observation observations[UC2MP_DIAG_SITE_COUNT][MAX_THREADS];
static atomic_flag observation_lock = ATOMIC_FLAG_INIT;
static atomic_int enabled = -1;
static const char *const names[UC2MP_DIAG_SITE_COUNT] = {
    "pad-read", "pad-read-state", "menu-text", "menu-enter", "menu-leave",
    "menu-ready", "menu-set-index", "menu-enabled"
};

static uint64_t native_thread(void) {
#ifdef _WIN32
    return GetCurrentThreadId();
#elif defined(__linux__)
    return (uint64_t)syscall(SYS_gettid);
#else
    return (uint64_t)(uintptr_t)pthread_self();
#endif
}
static uint64_t now_ms(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
#endif
}
void runtime_uc2mp_diag(Uc2mpDiagSite site, const void *return_address) {
    if ((unsigned)site >= UC2MP_DIAG_SITE_COUNT) return;
    int active = atomic_load_explicit(&enabled, memory_order_relaxed);
    if (active < 0) {
        const char *value = getenv("UC2MP_THREAD_DIAG");
        active = value && !strcmp(value, "1");
        /* Environment is fixed before guest startup; multiple first callers
         * can read it safely and publish the same opt-in decision. */
        atomic_store_explicit(&enabled, active, memory_order_relaxed);
    }
    if (!active) return;
    uint64_t thread = native_thread();
    uintptr_t address = (uintptr_t)return_address;
    int report = 0;
    while (atomic_flag_test_and_set_explicit(&observation_lock, memory_order_acquire)) {}
    Observation *slot = NULL, *empty = NULL;
    for (unsigned i = 0; i < MAX_THREADS; ++i) {
        Observation *candidate = &observations[site][i];
        if (!candidate->used) { if (!empty) empty = candidate; }
        else if (candidate->thread == thread) { slot = candidate; break; }
    }
    if (!slot && empty) {
        slot = empty; slot->used = 1; slot->thread = thread;
    }
    if (slot && slot->return_count < MAX_RETURNS) {
        int seen = 0;
        for (unsigned i = 0; i < slot->return_count; ++i)
            if (slot->returns[i] == address) seen = 1;
        if (!seen) { slot->returns[slot->return_count++] = address; report = 1; }
    }
    atomic_flag_clear_explicit(&observation_lock, memory_order_release);
    /* Never hold our bookkeeping lock during stdio, and never call the game. */
    if (report) {
        printf("UC2MP thread: site=%s tid=%" PRIu64 " return=0x%" PRIxPTR
               " monotonic-ms=%" PRIu64 "\n", names[site], thread, address, now_ms());
        fflush(stdout);
    }
}
