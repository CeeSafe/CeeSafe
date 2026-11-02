/* SPDX-License-Identifier: GPL-2.0-only WITH Classpath-exception-2.0 */
/*
 * Threads and mutexes for the hosted profile (type-system.md §38, ADR 0016; the atomics are in ceesafe/rt.h): POSIX threads, Win32 on Windows,
 * or CosmoOS userland's cosmo_thread_* and cosmo_mutex_* (CEE_COSMOOS, set by integrations/cosmoos). Linked only into
 * programs that use them (c-backend.md §6). Thread and mutex memory comes from ceesafe_alloc,
 * so the fault-injection sweep reaches it.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
/* nanosleep, for the test watchdog (ADR 0049), under glibc's strict C11 headers. */
#define _GNU_SOURCE
#endif
#include "ceesafe/rt.h"

#if defined(CEE_COSMOOS)
#include <cosmo/thread.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#endif

struct cee_thread {
#if defined(CEE_COSMOOS)
    cosmo_thread_t thread;
#elif defined(_WIN32)
    HANDLE handle;
#else
    pthread_t thread;
#endif
    void *(*body)(void *);
    void *arg;
};

#if defined(_WIN32) && !defined(CEE_COSMOOS)
static DWORD WINAPI ceesafe_thread_entry(LPVOID p) {
    cee_thread *t = (cee_thread *)p;
    (void)t->body(t->arg);
    return 0;
}
#endif

cee_thread *ceesafe_thread_start(void *(*body)(void *), void *arg) {
    cee_thread *t = (cee_thread *)ceesafe_alloc(sizeof *t, CEE_ALLOC_SYSTEM);
    if (t == NULL) {
        return NULL;
    }
    t->body = body;
    t->arg = arg;
#if defined(CEE_COSMOOS)
    if (cosmo_thread_start(&t->thread, body, arg, 0) != 0) {
        ceesafe_free(t, CEE_ALLOC_SYSTEM);
        return NULL;
    }
#elif defined(_WIN32)
    t->handle = CreateThread(NULL, 0, ceesafe_thread_entry, t, 0, NULL);
    if (t->handle == NULL) {
        ceesafe_free(t, CEE_ALLOC_SYSTEM);
        return NULL;
    }
#else
    if (pthread_create(&t->thread, NULL, body, arg) != 0) {
        ceesafe_free(t, CEE_ALLOC_SYSTEM);
        return NULL;
    }
#endif
    return t;
}

void ceesafe_thread_join(cee_thread *t) {
#if defined(CEE_COSMOOS)
    (void)cosmo_thread_join(&t->thread, NULL);
#elif defined(_WIN32)
    (void)WaitForSingleObject(t->handle, INFINITE);
    (void)CloseHandle(t->handle);
#else
    (void)pthread_join(t->thread, NULL);
#endif
    ceesafe_free(t, CEE_ALLOC_SYSTEM);
}

/* ---- Mutexes (type-system.md §38.3): the platform lock, allocated so a Mutex<T> may move ---------------------- */

#if defined(CEE_COSMOOS)
typedef cosmo_mutex_t cee_mutex_impl;
#elif defined(_WIN32)
typedef SRWLOCK cee_mutex_impl;
#else
typedef pthread_mutex_t cee_mutex_impl;
#endif

void *ceesafe_mutex_create(void) {
    cee_mutex_impl *m = (cee_mutex_impl *)ceesafe_alloc(sizeof *m, CEE_ALLOC_SYSTEM);
    if (m == NULL) {
        return NULL;
    }
#if defined(CEE_COSMOOS)
    m->state = 0u;
#elif defined(_WIN32)
    InitializeSRWLock(m);
#else
    if (pthread_mutex_init(m, NULL) != 0) {
        ceesafe_free(m, CEE_ALLOC_SYSTEM);
        return NULL;
    }
#endif
    return m;
}

void ceesafe_mutex_lock(void *m) {
#if defined(CEE_COSMOOS)
    cosmo_mutex_lock((cee_mutex_impl *)m);
#elif defined(_WIN32)
    AcquireSRWLockExclusive((cee_mutex_impl *)m);
#else
    (void)pthread_mutex_lock((cee_mutex_impl *)m);
#endif
}

void ceesafe_mutex_unlock(void *m) {
#if defined(CEE_COSMOOS)
    cosmo_mutex_unlock((cee_mutex_impl *)m);
#elif defined(_WIN32)
    ReleaseSRWLockExclusive((cee_mutex_impl *)m);
#else
    (void)pthread_mutex_unlock((cee_mutex_impl *)m);
#endif
}

void ceesafe_mutex_destroy(void *m) {
#if !defined(_WIN32) && !defined(CEE_COSMOOS)
    (void)pthread_mutex_destroy((cee_mutex_impl *)m);
#endif
    ceesafe_free(m, CEE_ALLOC_SYSTEM);
}

/* The atomics (§38.3, ADR 0021) are static inline in ceesafe/rt.h (ADR 0026). */

/* `ceesafec test`'s timeout (ADR 0049 §8): a detached thread that ends the process with status 124 once `ms` pass,
   after a line the runner reads. Its own platform thread, so the program's allocation report does not count it. */
#if defined(CEE_COSMOOS)
void ceesafe_thread_watchdog(uint64_t ms) { (void)ms; }
#else
static uint64_t cee_watchdog_ms;

#if defined(_WIN32)
static DWORD WINAPI cee_watchdog(LPVOID unused) {
    (void)unused;
    Sleep((DWORD)cee_watchdog_ms);
    static const char line[] = "ceesafe-test 1 timeout\n";
    ceesafe_platform_write_stderr((const uint8_t *)line, sizeof line - 1u);
    ExitProcess(124);
    return 0;
}

void ceesafe_thread_watchdog(uint64_t ms) {
    if (ms == 0u) {
        return;
    }
    cee_watchdog_ms = ms;
    HANDLE h = CreateThread(NULL, 0, cee_watchdog, NULL, 0, NULL);
    if (h != NULL) {
        CloseHandle(h);
    }
}
#else
static void *cee_watchdog(void *unused) {
    (void)unused;
    struct timespec t = {(time_t)(cee_watchdog_ms / 1000u), (long)(cee_watchdog_ms % 1000u) * 1000000L};
    while (nanosleep(&t, &t) != 0) {
    }
    static const char line[] = "ceesafe-test 1 timeout\n";
    ceesafe_platform_write_stderr((const uint8_t *)line, sizeof line - 1u);
    _exit(124);
    return NULL;
}

void ceesafe_thread_watchdog(uint64_t ms) {
    if (ms == 0u) {
        return;
    }
    cee_watchdog_ms = ms;
    pthread_t t;
    if (pthread_create(&t, NULL, cee_watchdog, NULL) == 0) {
        (void)pthread_detach(t);
    }
}
#endif
#endif
