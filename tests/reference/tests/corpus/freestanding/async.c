/* A host-side environment for freestanding/async.cee (ADR 0039): the executor's hooks over a mutex and a condition
 * variable, the allocator the program's EnvHeap calls, and a thread standing in for an interrupt that raises the
 * program's AsyncSignal. A kernel implements the same hooks with its clock, a wait queue and its interrupts. */
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "ceesafe/rt.h"
#include "ceesafe_exports.h"

void *env_alloc(size_t size, size_t align);
void env_free(void *p);
void env_start_irq(uint32_t ms);

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t woken = PTHREAD_COND_INITIALIZER;
static int pending; /* a wake not yet seen by an idle: kept, so none is lost */
static void *slot;  /* one thread runs tasks here */

uint64_t ceesafe_env_now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

void ceesafe_env_idle(void *executor, uint64_t deadline_ms) {
    (void)executor;
    pthread_mutex_lock(&lock);
    while (!pending) {
        uint64_t now = ceesafe_env_now_ms();
        if (deadline_ms != UINT64_MAX && now >= deadline_ms) {
            break;
        }
        if (deadline_ms == UINT64_MAX) {
            pthread_cond_wait(&woken, &lock);
        } else {
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            uint64_t ms = deadline_ms - now;
            until.tv_sec += (time_t)(ms / 1000u);
            until.tv_nsec += (long)(ms % 1000u) * 1000000L;
            if (until.tv_nsec >= 1000000000L) {
                until.tv_sec += 1;
                until.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&woken, &lock, &until);
        }
    }
    pending = 0;
    pthread_mutex_unlock(&lock);
}

void ceesafe_env_wake(void *executor) {
    (void)executor;
    pthread_mutex_lock(&lock);
    pending = 1;
    pthread_cond_signal(&woken);
    pthread_mutex_unlock(&lock);
}

void **ceesafe_env_slot(void) { return &slot; }

void *env_alloc(size_t size, size_t align) {
    void *p = NULL;
    return posix_memalign(&p, align < sizeof(void *) ? sizeof(void *) : align, size) == 0 ? p : NULL;
}

void env_free(void *p) { free(p); }

static void *irq(void *arg) {
    struct timespec pause = {0, (long)(uintptr_t)arg * 1000000L};
    nanosleep(&pause, NULL);
    raise_ready(); /* the program's @context(irq) export */
    return NULL;
}

void env_start_irq(uint32_t ms) {
    pthread_t t;
    if (pthread_create(&t, NULL, irq, (void *)(uintptr_t)ms) == 0) {
        pthread_detach(t);
    }
}

_Noreturn void ceesafe_panic(cee_panic_kind kind, cee_loc loc) {
    fflush(stdout);
    fprintf(stderr, "ceesafe panic: %s at %s:%u:%u\n", ceesafe_panic_reason(kind), loc.file, (unsigned)loc.line,
        (unsigned)loc.column);
    exit(134);
}

int main(void) {
    printf("async %u\n", (unsigned)async_selftest());
    return 0;
}
