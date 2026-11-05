// SPDX-License-Identifier: GPL-2.0-or-later
// gow3 4 Nov 2026: a shared mutex on a Windows slim reader/writer lock. libc++'s
// std::shared_mutex is a mutex plus two condition variables, so every shared lock takes and
// releases that inner mutex and readers on several threads (the GPU command thread, the
// draw-preparation workers, the shader compile workers) queue behind each other. An SRW lock
// takes a shared lock with one atomic operation. Same rules as std::shared_mutex: not
// recursive, no upgrade. GOW3_SRW_LOCKS=0 (read once): std::shared_mutex, as before.

#pragma once

#include <cstdlib>
#include <shared_mutex>
#ifdef _WIN32
#include <windows.h>
#endif

namespace Common {

class SrwSharedMutex {
public:
    void lock() {
#ifdef _WIN32
        if (UseSrw()) {
            AcquireSRWLockExclusive(&srw);
            return;
        }
#endif
        fallback.lock();
    }
    bool try_lock() {
#ifdef _WIN32
        if (UseSrw()) {
            return TryAcquireSRWLockExclusive(&srw) != 0;
        }
#endif
        return fallback.try_lock();
    }
    void unlock() {
#ifdef _WIN32
        if (UseSrw()) {
            ReleaseSRWLockExclusive(&srw);
            return;
        }
#endif
        fallback.unlock();
    }
    void lock_shared() {
#ifdef _WIN32
        if (UseSrw()) {
            AcquireSRWLockShared(&srw);
            return;
        }
#endif
        fallback.lock_shared();
    }
    bool try_lock_shared() {
#ifdef _WIN32
        if (UseSrw()) {
            return TryAcquireSRWLockShared(&srw) != 0;
        }
#endif
        return fallback.try_lock_shared();
    }
    void unlock_shared() {
#ifdef _WIN32
        if (UseSrw()) {
            ReleaseSRWLockShared(&srw);
            return;
        }
#endif
        fallback.unlock_shared();
    }

private:
    static bool UseSrw() {
        static const bool on = [] {
            const char* env = std::getenv("GOW3_SRW_LOCKS");
            return !(env && env[0] == '0');
        }();
        return on;
    }
#ifdef _WIN32
    SRWLOCK srw = SRWLOCK_INIT;
#endif
    std::shared_mutex fallback;
};

} // namespace Common
