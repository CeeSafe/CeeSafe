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
#include <errno.h>
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
    uint32_t finished; /* ADR 0222 §2.3: set (ceesafe_atomic_*) by the entry once the body has returned */
};

/* The thread's entry: the body, then the flag ceesafe_thread_finished reads. The body's result was written into the
   thread's record before it returned, and the sequentially consistent store and load order it before the reader. */
static void *cee_thread_entry(void *p) {
    cee_thread *t = (cee_thread *)p;
    (void)t->body(t->arg);
    ceesafe_atomic_store_u32(&t->finished, 1u);
    return NULL;
}

#if defined(_WIN32) && !defined(CEE_COSMOOS)
static DWORD WINAPI ceesafe_thread_entry(LPVOID p) {
    (void)cee_thread_entry(p);
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
    t->finished = 0u;
#if defined(CEE_COSMOOS)
    if (cosmo_thread_start(&t->thread, cee_thread_entry, t, 0) != 0) {
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
    if (pthread_create(&t->thread, NULL, cee_thread_entry, t) != 0) {
        ceesafe_free(t, CEE_ALLOC_SYSTEM);
        return NULL;
    }
#endif
    return t;
}

bool ceesafe_thread_finished(cee_thread *t) {
    return ceesafe_atomic_load_u32(&t->finished) != 0u;
}

/* `Thread.sleep(ms)` (ADR 0222 §2.1): at least `ms` milliseconds; a signal's interruption sleeps the time left. */
void ceesafe_thread_sleep(uint64_t ms) {
#if defined(CEE_COSMOOS)
    /* A timed wait on a condition nothing signals (cosmo_cond_timedwait takes nanoseconds from now), a second at a
       time so the count never overflows. Not built or run here: a wake before a step's end would shorten the sleep,
       if CosmoOS's condition ever woke without a signal. */
    if (ms == 0u) {
        return;
    }
    cosmo_mutex_t m = (cosmo_mutex_t)COSMO_MUTEX_INIT;
    cosmo_cond_t c = (cosmo_cond_t)COSMO_COND_INIT;
    cosmo_mutex_lock(&m);
    while (ms > 0u) {
        uint64_t step = ms > 1000u ? 1000u : ms;
        (void)cosmo_cond_timedwait(&c, &m, (unsigned long long)step * 1000000ull);
        ms -= step;
    }
    cosmo_mutex_unlock(&m);
#elif defined(_WIN32)
    while (ms > 0u) {
        DWORD step = ms > 0x7FFFFFFFu ? 0x7FFFFFFFu : (DWORD)ms; /* below INFINITE */
        Sleep(step);
        ms -= step;
    }
#else
    if (ms == 0u) {
        return;
    }
    struct timespec t = {(time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L};
    if ((uint64_t)t.tv_sec != ms / 1000u) {
        t.tv_sec = (time_t)((~(uint64_t)0) >> 33); /* a time_t of 32 bits: about 34 years, then on */
    }
    while (nanosleep(&t, &t) != 0 && errno == EINTR) {
    }
#endif
}

/* ---- Service threads (ADR 0182): started, named and never joined ---------------------------------------------- */

typedef struct {
    void *(*body)(void *);
    void *arg;
    char name[16]; /* NUL-terminated; Linux's limit (15 bytes and the NUL), the shortest of the platforms' */
} cee_service;

#if defined(_WIN32) && !defined(CEE_COSMOOS)
typedef HRESULT(WINAPI *cee_set_description)(HANDLE, PCWSTR);
#endif

/* The service's own entry: it names its thread (macOS names only the calling thread), frees its record, and runs. */
static void *cee_service_entry(void *p) {
    cee_service s = *(cee_service *)p;
    ceesafe_free(p, CEE_ALLOC_SYSTEM);
#if defined(CEE_COSMOOS)
#elif defined(_WIN32)
    /* SetThreadDescription is Windows 10 1607 and later: looked up, so older systems run unnamed. */
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    cee_set_description describe = kernel == NULL ? NULL
        : (cee_set_description)(void (*)(void))GetProcAddress(kernel, "SetThreadDescription");
    if (describe != NULL && s.name[0] != 0) {
        WCHAR wide[16];
        int k = 0;
        for (; k < 15 && s.name[k] != 0; k++) {
            wide[k] = (WCHAR)(unsigned char)s.name[k];
        }
        wide[k] = 0;
        (void)describe(GetCurrentThread(), wide);
    }
#elif defined(__APPLE__)
    if (s.name[0] != 0) {
        (void)pthread_setname_np(s.name);
    }
#elif defined(__linux__)
    if (s.name[0] != 0) {
        (void)pthread_setname_np(pthread_self(), s.name);
    }
#endif
    return s.body(s.arg);
}

#if defined(_WIN32) && !defined(CEE_COSMOOS)
static DWORD WINAPI cee_service_entry_win(LPVOID p) {
    (void)cee_service_entry(p);
    return 0;
}
#endif

int ceesafe_thread_service(void *(*body)(void *), void *arg, const uint8_t *name, uint64_t length) {
    cee_service *s = (cee_service *)ceesafe_alloc(sizeof *s, CEE_ALLOC_SYSTEM);
    if (s == NULL) {
        return 0;
    }
    s->body = body;
    s->arg = arg;
    uint64_t n = length < 15u ? length : 15u;
    for (uint64_t k = 0; k < n; k++) {
        /* A name is text; a NUL inside it ends it early, as C would read it. */
        s->name[k] = (char)name[k];
    }
    s->name[n] = 0;
#if defined(CEE_COSMOOS)
    cosmo_thread_t thread; /* CosmoOS has no detach: the handle is not kept, and the thread ends with the process */
    if (cosmo_thread_start(&thread, cee_service_entry, s, 0) != 0) {
        ceesafe_free(s, CEE_ALLOC_SYSTEM);
        return 0;
    }
#elif defined(_WIN32)
    HANDLE handle = CreateThread(NULL, 0, cee_service_entry_win, s, 0, NULL);
    if (handle == NULL) {
        ceesafe_free(s, CEE_ALLOC_SYSTEM);
        return 0;
    }
    (void)CloseHandle(handle);
#else
    pthread_t thread;
    if (pthread_create(&thread, NULL, cee_service_entry, s) != 0) {
        ceesafe_free(s, CEE_ALLOC_SYSTEM);
        return 0;
    }
    (void)pthread_detach(thread);
#endif
    return 1;
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

/* Takes the lock if no thread holds it (ADR 0112): true if taken. It never waits; the thread that holds the lock
   gets false too (the platform locks are not recursive). */
bool ceesafe_mutex_try_lock(void *m) {
#if defined(CEE_COSMOOS)
    return cosmo_mutex_trylock((cee_mutex_impl *)m) == 0;
#elif defined(_WIN32)
    return TryAcquireSRWLockExclusive((cee_mutex_impl *)m) != 0;
#else
    return pthread_mutex_trylock((cee_mutex_impl *)m) == 0;
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

/*
 * The lock of OnceLock's initialisers (ADR 0096). A OnceLock holds no lock of its own, so that it needs no
 * allocation and can be a static's initialiser; a thread that finds a cell empty takes this lock, looks again, and
 * runs the initialiser holding it. The owner's identity makes it recursive: an initialiser may fill another
 * OnceLock. The owner is read without the lock, so it is an atomic; only the owner reads or writes the depth.
 */
#if defined(CEE_COSMOOS)
static cee_mutex_impl cee_once_mutex = {0};
#elif defined(_WIN32)
static cee_mutex_impl cee_once_mutex = SRWLOCK_INIT;
#else
static cee_mutex_impl cee_once_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif
static uint64_t cee_once_owner;
static uint32_t cee_once_depth;

/* The running thread, never 0. */
static uint64_t cee_once_self(void) {
#if defined(CEE_COSMOOS)
    /* The thread's id from its thread block (cosmo/thread.h): no system call. One more, so that it is never 0. */
    return (uint64_t)cosmo_thread_id() + 1u;
#elif defined(_WIN32)
    return (uint64_t)GetCurrentThreadId();
#else
    return (uint64_t)(uintptr_t)pthread_self();
#endif
}

void ceesafe_mutex_once_lock(void) {
    uint64_t self = cee_once_self();
    if (ceesafe_atomic_load_u64(&cee_once_owner) == self) {
        cee_once_depth++;
        return;
    }
    ceesafe_mutex_lock(&cee_once_mutex);
    ceesafe_atomic_store_u64(&cee_once_owner, self);
    cee_once_depth = 1;
}

void ceesafe_mutex_once_unlock(void) {
    if (--cee_once_depth == 0) {
        ceesafe_atomic_store_u64(&cee_once_owner, 0);
        ceesafe_mutex_unlock(&cee_once_mutex);
    }
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
