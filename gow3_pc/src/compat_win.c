/* Windows implementations of compat_win.h. */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat_win.h"

/* gow3_jmp_buf: rbx rbp rdi rsi r12-r15 (0-56), rsp after return (64), return address (72),
 * xmm6-15 (80-239), mxcsr (240), x87 control word (244). */
__asm__(".text\n"
        ".globl gow3_setjmp\n"
        ".def gow3_setjmp; .scl 2; .type 32; .endef\n"
        "gow3_setjmp:\n"
        "    mov %rbx,0(%rcx)\n    mov %rbp,8(%rcx)\n    mov %rdi,16(%rcx)\n    mov %rsi,24(%rcx)\n"
        "    mov %r12,32(%rcx)\n    mov %r13,40(%rcx)\n    mov %r14,48(%rcx)\n    mov %r15,56(%rcx)\n"
        "    lea 8(%rsp),%rdx\n    mov %rdx,64(%rcx)\n    mov (%rsp),%rdx\n    mov %rdx,72(%rcx)\n"
        "    movups %xmm6,80(%rcx)\n    movups %xmm7,96(%rcx)\n    movups %xmm8,112(%rcx)\n"
        "    movups %xmm9,128(%rcx)\n    movups %xmm10,144(%rcx)\n    movups %xmm11,160(%rcx)\n"
        "    movups %xmm12,176(%rcx)\n    movups %xmm13,192(%rcx)\n    movups %xmm14,208(%rcx)\n"
        "    movups %xmm15,224(%rcx)\n    stmxcsr 240(%rcx)\n    fnstcw 244(%rcx)\n"
        "    xor %eax,%eax\n    ret\n"
        ".globl gow3_longjmp\n"
        ".def gow3_longjmp; .scl 2; .type 32; .endef\n"
        "gow3_longjmp:\n"
        "    mov %edx,%eax\n    test %eax,%eax\n    jnz 1f\n    mov $1,%eax\n"
        "1:  mov 0(%rcx),%rbx\n    mov 8(%rcx),%rbp\n    mov 16(%rcx),%rdi\n    mov 24(%rcx),%rsi\n"
        "    mov 32(%rcx),%r12\n    mov 40(%rcx),%r13\n    mov 48(%rcx),%r14\n    mov 56(%rcx),%r15\n"
        "    movups 80(%rcx),%xmm6\n    movups 96(%rcx),%xmm7\n    movups 112(%rcx),%xmm8\n"
        "    movups 128(%rcx),%xmm9\n    movups 144(%rcx),%xmm10\n    movups 160(%rcx),%xmm11\n"
        "    movups 176(%rcx),%xmm12\n    movups 192(%rcx),%xmm13\n    movups 208(%rcx),%xmm14\n"
        "    movups 224(%rcx),%xmm15\n    ldmxcsr 240(%rcx)\n    fldcw 244(%rcx)\n"
        "    mov 64(%rcx),%rsp\n    jmp *72(%rcx)\n");

int compat_errno_from_win32(unsigned long error) {
    switch (error) {
    case ERROR_SUCCESS: return 0;
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_DRIVE:
    case ERROR_BAD_NETPATH: case ERROR_BAD_NET_NAME: case ERROR_INVALID_NAME: return ENOENT;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION:
    case ERROR_WRITE_PROTECT: return EACCES;
    case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: return EEXIST;
    case ERROR_INVALID_HANDLE: return EBADF;
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: case ERROR_COMMITMENT_LIMIT: return ENOMEM;
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: return ENOSPC;
    case ERROR_DIR_NOT_EMPTY: return ENOTEMPTY;
    case ERROR_DIRECTORY: return ENOTDIR;
    case ERROR_FILENAME_EXCED_RANGE: case ERROR_BUFFER_OVERFLOW: return ENAMETOOLONG;
    case ERROR_TOO_MANY_OPEN_FILES: return EMFILE;
    case ERROR_NOACCESS: return EFAULT;
    case ERROR_HANDLE_EOF: return 0;
    case ERROR_NEGATIVE_SEEK: case ERROR_INVALID_PARAMETER: return EINVAL;
    default: return EIO;
    }
}

/* Positioned I/O on a synchronous handle moves the file pointer: it is restored afterwards.
 * A concurrent read() on the same descriptor would race, as with a shared offset anyway. */
static ssize_t positioned(int fd, void *buffer, size_t size, int64_t offset, int write) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    if (offset < 0) { errno = EINVAL; return -1; }
    LARGE_INTEGER zero = {0}, saved;
    if (!SetFilePointerEx(h, zero, &saved, FILE_CURRENT)) { errno = compat_errno_from_win32(GetLastError()); return -1; }
    size_t done = 0;
    while (done < size) {
        DWORD chunk = size - done > 0x40000000u ? 0x40000000u : (DWORD)(size - done), n = 0;
        OVERLAPPED at = {0};
        uint64_t position = (uint64_t)offset + done;
        at.Offset = (DWORD)position; at.OffsetHigh = (DWORD)(position >> 32);
        BOOL ok = write ? WriteFile(h, (const char *)buffer + done, chunk, &n, &at)
                        : ReadFile(h, (char *)buffer + done, chunk, &n, &at);
        if (!ok) {
            DWORD e = GetLastError();
            if (e == ERROR_HANDLE_EOF) break;
            SetFilePointerEx(h, saved, NULL, FILE_BEGIN);
            if (done) return (ssize_t)done;
            errno = compat_errno_from_win32(e);
            return -1;
        }
        done += n;
        if (n < chunk) break;
    }
    SetFilePointerEx(h, saved, NULL, FILE_BEGIN);
    return (ssize_t)done;
}
ssize_t compat_pread(int fd, void *buffer, size_t size, int64_t offset) { return positioned(fd, buffer, size, offset, 0); }
ssize_t compat_pwrite(int fd, const void *buffer, size_t size, int64_t offset) { return positioned(fd, (void *)buffer, size, offset, 1); }

long compat_utc_offset(time_t t) {
    struct tm local, utc;
    if (localtime_s(&local, &t) || gmtime_s(&utc, &t)) return 0;
    /* mktime interprets utc as local time: the difference is the zone offset (with DST). */
    utc.tm_isdst = local.tm_isdst;
    time_t as_local = mktime(&utc);
    return as_local == (time_t)-1 ? 0 : (long)(t - as_local);
}

int compat_random(void *buffer, size_t size) {
    while (size) {
        ULONG chunk = size > 0x10000000u ? 0x10000000u : (ULONG)size;
        if (BCryptGenRandom(NULL, buffer, chunk, BCRYPT_USE_SYSTEM_PREFERRED_RNG)) return -1;
        buffer = (char *)buffer + chunk; size -= chunk;
    }
    return 0;
}

/* gow3 28 Oct 2026 (threads work, analysis/optimization-1028/THREADS.md): the process never asked
 * for a timer resolution, so every timed wait outside compat_sleep_ns (condition variables with a
 * timeout, std::this_thread::sleep_for, WaitForSingleObject(ms), the guest's timed cond/sema/equeue
 * waits) rounded up to Windows' default 15.6 ms tick. On 4 cores the game's frame then took
 * 9-10 ms, 16 ms or 31 ms (one or two ticks lost) while the GPU thread waited for it.
 *   GOW3_TIMER_RES: 1 ms tick (timeBeginPeriod), DEFAULT ON since 29 Oct (A/B all 3 games x 4 cores /
 *   Deck sim / full CPU: +9..+22% FPS; 15-minute soaks clean); GOW3_TIMER_RES=0 keeps 15.6 ms.
 *   The process opts out of power throttling by DEFAULT (29 Oct, with the tick); GOW3_POWER_THROTTLE=1
 *   keeps Windows' throttling (opting out = no EcoQoS
 *   on hybrid CPUs/laptops, and Windows 11 keeps the 1 ms tick while the window is hidden). */
#include <mmsystem.h>
static int env_switch(const char *name, int fallback) {
    const char *v = getenv(name);
    return v && *v ? strcmp(v, "0") != 0 : fallback;
}
void runtime_host_scheduling_init(void) {
    if (env_switch("GOW3_TIMER_RES", 1)) {
        MMRESULT r = timeBeginPeriod(1);
        printf("Host scheduling: 1 ms timer resolution%s (GOW3_TIMER_RES=0 keeps 15.6 ms)\n",
               r == TIMERR_NOERROR ? "" : " REFUSED");
    }
    if (!env_switch("GOW3_POWER_THROTTLE", 0)) {
        PROCESS_POWER_THROTTLING_STATE state = {0};
        state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED |
                            PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        state.StateMask = 0; /* both controlled, both off: full speed, timer request honoured */
        BOOL ok = SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof state);
        printf("Host scheduling: power throttling off%s (GOW3_POWER_THROTTLE=1 keeps it)\n",
               ok ? "" : " - REFUSED");
    }
}

/* gow3 28 Oct 2026 (threads work): a high-resolution waitable timer wakes ~0.5 ms late on this
 * Windows 11 (measured: 1, 10 and 100 us -> 517 us, 1000 us -> 1.5 ms). The engines call
 * sceKernelUsleep(1) at ~330 sites in Uncharted 2 alone (back-off in spin waits, ~µs on the PS4)
 * and usleep(1000) while polling job counters: each cost half a millisecond or more of a
 * frame. GOW3_PRECISE_SLEEP=1 (DEFAULT OFF until proven): the timer sleeps the part
 * longer than GOW3_SLEEP_SLACK_US (default 600) and the rest yields the core in a loop
 * (SwitchToThread runs any ready thread first; pause when none). GOW3_SLEEP_STATS=1 prints the
 * guest sleeps every 5 s. */
static LONGLONG qpc_now(void) { LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart; }
static LONGLONG qpc_per_us(void) {
    static LONGLONG f;
    if (!f) { LARGE_INTEGER q; QueryPerformanceFrequency(&q); f = q.QuadPart / 1000000; if (!f) f = 1; }
    return f;
}
static void sleep_stats(uint64_t ns, LONGLONG took) {
    static int on = -1;
    if (on < 0) on = env_switch("GOW3_SLEEP_STATS", 0);
    if (!on) return;
    static volatile LONG64 count[3], spent_us, window_start;
    InterlockedIncrement64(&count[ns <= 100000 ? 0 : ns <= 2000000 ? 1 : 2]);
    InterlockedAdd64(&spent_us, took / qpc_per_us());
    LONGLONG now = qpc_now(), start = window_start;
    if (!start) { InterlockedCompareExchange64(&window_start, now, 0); return; }
    if (now - start >= 5000000 * qpc_per_us() &&
        InterlockedCompareExchange64(&window_start, now, start) == start) {
        printf("Guest sleeps (5 s): %lld <=100 us, %lld <=2 ms, %lld longer; %.1f ms asleep in all\n",
               (long long)InterlockedExchange64(&count[0], 0), (long long)InterlockedExchange64(&count[1], 0),
               (long long)InterlockedExchange64(&count[2], 0), InterlockedExchange64(&spent_us, 0) / 1000.0);
    }
}

/* One high-resolution timer per thread (Windows 10 1803+); a plain waitable timer before. */
static __thread HANDLE sleep_timer;
static void timer_sleep_ns(uint64_t ns);
void compat_sleep_ns(uint64_t ns) {
    if (!ns) { SwitchToThread(); return; }
    timer_sleep_ns(ns);
}
/* The guest's usleep/nanosleep (runtime_kernel.c); host threads keep compat_sleep_ns. */
void compat_guest_sleep_ns(uint64_t ns) {
    if (!ns) { SwitchToThread(); return; }
    static int precise = -1, slack_us = 600;
    if (precise < 0) {
        const char *s = getenv("GOW3_SLEEP_SLACK_US");
        if (s && *s) slack_us = atoi(s);
        precise = env_switch("GOW3_PRECISE_SLEEP", 0);
    }
    LONGLONG start = qpc_now();
    if (precise) {
        const LONGLONG deadline = start + (LONGLONG)(ns / 1000) * qpc_per_us() + (LONGLONG)(ns % 1000) * qpc_per_us() / 1000;
        if (ns > (uint64_t)slack_us * 1000) timer_sleep_ns(ns - (uint64_t)slack_us * 1000);
        while (qpc_now() < deadline) {
            if (!SwitchToThread()) __builtin_ia32_pause();
        }
    } else {
        timer_sleep_ns(ns);
    }
    sleep_stats(ns, qpc_now() - start);
}
static void timer_sleep_ns(uint64_t ns) {
    if (!sleep_timer) {
        sleep_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!sleep_timer) sleep_timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    }
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)((ns + 99) / 100); /* relative, 100 ns units */
    if (sleep_timer && SetWaitableTimer(sleep_timer, &due, 0, NULL, NULL, FALSE))
        WaitForSingleObject(sleep_timer, INFINITE);
    else
        Sleep((DWORD)((ns + 999999) / 1000000));
}

int compat_remove_tree(const char *path) {
    DWORD attributes = GetFileAttributesA(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) return 0;
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        if (attributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesA(path, attributes & ~FILE_ATTRIBUTE_READONLY);
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) return RemoveDirectoryA(path) ? 0 : -1;
        return DeleteFileA(path) ? 0 : -1;
    }
    char pattern[MAX_PATH * 4];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    WIN32_FIND_DATAA entry;
    HANDLE find = FindFirstFileA(pattern, &entry);
    int result = 0;
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(entry.cFileName, ".") || !strcmp(entry.cFileName, "..")) continue;
            char child[MAX_PATH * 4];
            snprintf(child, sizeof(child), "%s\\%s", path, entry.cFileName);
            if (compat_remove_tree(child)) result = -1;
        } while (FindNextFileA(find, &entry));
        FindClose(find);
    }
    if (!RemoveDirectoryA(path)) result = -1;
    return result;
}

int compat_cpu_times(int who, int64_t *user_us, int64_t *kernel_us) {
    FILETIME created, exited, kernel, user;
    BOOL ok = who ? GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)
                  : GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    if (!ok) return -1;
    *user_us = (int64_t)(((uint64_t)user.dwHighDateTime << 32 | user.dwLowDateTime) / 10);
    *kernel_us = (int64_t)(((uint64_t)kernel.dwHighDateTime << 32 | kernel.dwLowDateTime) / 10);
    return 0;
}
#endif
