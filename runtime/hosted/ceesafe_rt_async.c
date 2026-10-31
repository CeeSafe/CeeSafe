/* SPDX-License-Identifier: GPL-2.0-only WITH Classpath-exception-2.0 */
/*
 * Async tasks for the hosted profile (type-system.md §40, ADR 0031; c-backend.md §3a and §6). One executor per thread:
 * Async.run's root task and the children its scopes spawn, run in turn from a queue. A task that parks has registered
 * what wakes it: a deadline (lib/std's Timer), a descriptor becoming writable (its console output), or its scope's last
 * child. While no task can run, the executor waits in poll() for the earliest deadline or descriptor. Child tasks and
 * their frames come from ceesafe_alloc, so the fault-injection sweep reaches them. Linked only into programs that call
 * ceesafe_async_* (c-backend.md §6).
 */
#include "ceesafe/rt.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <time.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#endif

#ifndef PIPE_BUF
#define PIPE_BUF 512 /* POSIX's minimum (_POSIX_PIPE_BUF) */
#endif

typedef struct cee_task {
    void *frame;
    bool (*resume_root)(void *);
    bool (*resume_child)(void *, ceesafe_scope *);
    ceesafe_scope *scope; /* the child's scope; NULL for the root */
    struct cee_task *next; /* the ready queue, or the parked list */
    uint64_t deadline;     /* UINT64_MAX for none */
    int32_t fd;            /* -1 for none */
    short fd_events;       /* POLLIN or POLLOUT */
    bool on_scope;         /* parked until its scope's last child returns */
    bool cancelled;        /* cancelled itself (the sweep's root); see cee_cancelled */
    uint32_t shields;      /* Async.shield depth (type-system.md §40.7) */
    uint64_t shield_deadline;
    bool embedded;         /* in its caller's frame (Async.race, Async.all): never freed */
    bool on_wake;          /* parked until another thread wakes it (Async.blocking, channels) */
    bool channel_wait;     /* on_wake for a channel: a cancellation wakes it too (type-system.md §40.7) */
    uint64_t woken;        /* 0, or the wake's sequence number, set by any thread (__atomic): readied in that order */
} cee_task;

_Static_assert(sizeof(cee_task) <= sizeof(ceesafe_task_slot), "ceesafe_task_slot holds a task");

/* A child's task and frame are one block: the frame starts at the first max_align_t boundary after the task. */
#define CEE_TASK_HEAD ((sizeof(cee_task) + sizeof(max_align_t) - 1u) / sizeof(max_align_t) * sizeof(max_align_t))

typedef struct {
    cee_task *ready_head;
    cee_task *ready_tail;
    cee_task *parked;
    cee_task *current;
    int wake_pipe[2];    /* another thread's wake: a byte written here makes poll return (-1 until first needed) */
    uint32_t wake_count; /* tasks parked on_wake */
} cee_executor;

static _Thread_local cee_executor cee_exec;

/* The cancellation sweep (testing.md §1): CEESAFE_CANCEL_AT=k cancels the running Async.run's root task at the
 * program's k-th suspension; CEESAFE_CANCEL_REPORT=1 prints the count after each Async.run. */
static uint64_t cee_suspensions; /* every thread's executor counts here (__atomic) */

/* Whether a task is cancelled: itself, or its scope (a sibling failed), or the task that owns its scope. */
static bool cee_cancelled(const cee_task *t) {
    while (t != NULL) {
        if (t->cancelled) {
            return true;
        }
        if (t->scope == NULL) {
            return false;
        }
        if (t->scope->cancelled) {
            return true;
        }
        t = (const cee_task *)t->scope->owner;
    }
    return false;
}

/* After a cancellation: every task parked on a deadline or a descriptor runs again, to see it (its waits retry). */
static void cee_wake_waiters(void);

static void cee_ready(cee_task *t) {
    t->next = NULL;
    if (cee_exec.ready_tail != NULL) {
        cee_exec.ready_tail->next = t;
    } else {
        cee_exec.ready_head = t;
    }
    cee_exec.ready_tail = t;
}

static cee_task *cee_take_ready(void) {
    cee_task *t = cee_exec.ready_head;
    if (t != NULL) {
        cee_exec.ready_head = t->next;
        if (cee_exec.ready_head == NULL) {
            cee_exec.ready_tail = NULL;
        }
        t->next = NULL;
    }
    return t;
}

/* Wakes a task of this or another thread's executor: safe from any thread while the task waits (it cannot end before
 * it is woken). */
static uint64_t cee_wake_sequence; /* __atomic: orders wakes, so woken tasks run first come, first served */

static void cee_wake_task(cee_executor *exec, cee_task *t) {
    uint64_t sequence = __atomic_add_fetch(&cee_wake_sequence, 1u, __ATOMIC_SEQ_CST);
    uint64_t none = 0u;
    (void)__atomic_compare_exchange_n(&t->woken, &none, sequence, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
#if !defined(_WIN32)
    if (exec->wake_pipe[1] >= 0) {
        unsigned char byte = 1u;
        ssize_t written;
        do {
            written = write(exec->wake_pipe[1], &byte, 1u);
        } while (written < 0 && errno == EINTR);
    }
#endif
}

static void cee_unpark(cee_task *t) {
    cee_task **link = &cee_exec.parked;
    while (*link != NULL && *link != t) {
        link = &(*link)->next;
    }
    if (*link == t) {
        *link = t->next;
    }
    t->deadline = UINT64_MAX;
    t->fd = -1;
    t->on_scope = false;
    if (t->on_wake) {
        t->on_wake = false;
        t->channel_wait = false;
        cee_exec.wake_count -= 1u;
    }
    cee_ready(t);
}

uint64_t ceesafe_async_now(void) {
#if defined(_WIN32)
    return (uint64_t)GetTickCount64();
#else
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
#endif
}

void ceesafe_async_wait_until(uint64_t deadline) {
    cee_task *t = cee_exec.current;
    if (t != NULL && deadline < t->deadline) {
        t->deadline = deadline;
    }
}

void ceesafe_async_wait_writable(int32_t fd) {
    if (cee_exec.current != NULL) {
        cee_exec.current->fd = fd;
#if !defined(_WIN32)
        cee_exec.current->fd_events = POLLOUT;
#endif
    }
}

void ceesafe_async_wait_readable(int32_t fd) {
    if (cee_exec.current != NULL) {
        cee_exec.current->fd = fd;
#if !defined(_WIN32)
        cee_exec.current->fd_events = POLLIN;
#endif
    }
}

int64_t ceesafe_async_poll(const int32_t *fds, const uint32_t *events, uint32_t *ready, uint64_t count, int64_t timeout) {
#if defined(_WIN32)
    (void)fds; (void)events; (void)ready; (void)count; (void)timeout;
    return -(int64_t)ENOSYS;
#else
    struct pollfd list[64];
    if (count > 64u) {
        count = 64u;
    }
    for (uint64_t k = 0; k < count; k++) {
        list[k].fd = fds[k];
        list[k].events = (short)(((events[k] & 1u) != 0u ? POLLIN : 0) | ((events[k] & 2u) != 0u ? POLLOUT : 0));
        list[k].revents = 0;
    }
    int n;
    do {
        n = poll(list, (nfds_t)count, timeout < 0 ? -1 : (timeout > INT_MAX ? INT_MAX : (int)timeout));
    } while (n < 0 && errno == EINTR);
    for (uint64_t k = 0; k < count; k++) {
        ready[k] = (list[k].revents & (POLLIN | POLLHUP | POLLERR)) != 0 ? 1u : 0u;
        ready[k] |= (list[k].revents & (POLLOUT | POLLHUP | POLLERR)) != 0 ? 2u : 0u;
    }
    return n < 0 ? -(int64_t)errno : (int64_t)n;
#endif
}

/* Writes at most PIPE_BUF bytes to `fd` if it is writable now: a count, 0 if it is not, or a negated errno.
 * A write of at most PIPE_BUF bytes to a descriptor poll reports writable does not block (POSIX write()), so this
 * honours lib/std's @effects(). Standard output is flushed first, so the output stays in order with Console's. */
int64_t ceesafe_async_write(int32_t fd, const uint8_t *bytes, uint64_t length) {
    if (fd == 1) {
        (void)fflush(stdout);
    } else if (fd == 2) {
        (void)fflush(stderr);
    }
#if defined(_WIN32)
    /* No readiness for console handles: the write is a step of at most PIPE_BUF bytes. */
    uint64_t step = length < PIPE_BUF ? length : PIPE_BUF;
    FILE *stream = fd == 2 ? stderr : stdout;
    size_t written = fwrite(bytes, 1u, (size_t)step, stream);
    (void)fflush(stream);
    return written == step ? (int64_t)step : -(int64_t)EIO;
#else
    struct pollfd p = {fd, POLLOUT, 0};
    int ready;
    do {
        ready = poll(&p, 1, 0);
    } while (ready < 0 && errno == EINTR);
    if (ready < 0) {
        return -(int64_t)errno;
    }
    if (ready == 0 || (p.revents & POLLOUT) == 0) {
        if ((p.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return -(int64_t)EPIPE;
        }
        return 0;
    }
    uint64_t step = length < PIPE_BUF ? length : PIPE_BUF;
    ssize_t written;
    do {
        written = write(fd, bytes, (size_t)step);
    } while (written < 0 && errno == EINTR);
    return written < 0 ? -(int64_t)errno : (int64_t)written;
#endif
}

void *ceesafe_async_spawn(ceesafe_scope *scope, size_t frame_size, bool (*resume)(void *frame, ceesafe_scope *scope)) {
    scope->owner = cee_exec.current;
    if (scope->oom) {
        return NULL; /* the scope already failed to start a child: it starts no more */
    }
    unsigned char *block = (unsigned char *)ceesafe_alloc(CEE_TASK_HEAD + frame_size, CEE_ALLOC_SYSTEM);
    if (block == NULL) {
        scope->oom = true;
        scope->cancelled = true; /* the children already started stop (type-system.md §40.5) */
        cee_wake_waiters();
        return NULL;
    }
    cee_task *t = (cee_task *)(void *)block;
    t->frame = block + CEE_TASK_HEAD;
    t->resume_root = NULL;
    t->resume_child = resume;
    t->scope = scope;
    t->deadline = UINT64_MAX;
    t->fd = -1;
    t->on_scope = false;
    t->cancelled = false;
    t->shields = 0u;
    t->shield_deadline = UINT64_MAX;
    t->embedded = false;
    t->on_wake = false;
    t->channel_wait = false;
    t->woken = 0u;
    scope->live += 1u;
    cee_ready(t);
    return t->frame;
}

void ceesafe_async_start(ceesafe_scope *scope, ceesafe_task_slot *slot, void *frame,
                         bool (*resume)(void *frame, ceesafe_scope *scope)) {
    cee_task *t = (cee_task *)(void *)slot;
    scope->owner = cee_exec.current;
    t->frame = frame;
    t->resume_root = NULL;
    t->resume_child = resume;
    t->scope = scope;
    t->deadline = UINT64_MAX;
    t->fd = -1;
    t->on_scope = false;
    t->cancelled = false;
    t->shields = 0u;
    t->shield_deadline = UINT64_MAX;
    t->embedded = true;
    t->on_wake = false;
    t->channel_wait = false;
    t->woken = 0u;
    scope->live += 1u;
    cee_ready(t);
}

void ceesafe_async_cancel(ceesafe_scope *scope) {
    if (!scope->cancelled) {
        scope->cancelled = true;
        cee_wake_waiters();
    }
}

/* 0 if the current task runs on, 1 if it is cancelled, 2 if its shield's time is up (type-system.md §40.7). */
uint32_t ceesafe_async_status(void) {
    cee_task *t = cee_exec.current;
    if (t == NULL) {
        return 0u;
    }
    if (t->shields > 0u) {
        return ceesafe_async_now() >= t->shield_deadline ? 2u : 0u;
    }
    return cee_cancelled(t) ? 1u : 0u;
}

/* Async.shield(ms, …): cancellation is ignored inside, for at most `ms` milliseconds (type-system.md §40.7). */
void ceesafe_async_shield_enter(uint64_t ms) {
    cee_task *t = cee_exec.current;
    if (t->shields == 0u) {
        uint64_t now = ceesafe_async_now();
        t->shield_deadline = ms > UINT64_MAX - now ? UINT64_MAX : now + ms;
    }
    t->shields += 1u;
}

void ceesafe_async_shield_leave(void) {
    cee_task *t = cee_exec.current;
    t->shields -= 1u;
    if (t->shields == 0u) {
        t->shield_deadline = UINT64_MAX;
    }
}

static void cee_wake_waiters(void) {
    cee_task *t = cee_exec.parked;
    while (t != NULL) {
        cee_task *next = t->next;
        if (!t->on_scope && (!t->on_wake || t->channel_wait)) { /* a worker's wait is not cancelled */
            cee_unpark(t);
        }
        t = next;
    }
}

static void cee_open_wake_pipe(void) {
#if !defined(_WIN32)
    if (cee_exec.wake_pipe[0] < 0) {
        int fds[2];
        if (pipe(fds) == 0) {
            cee_exec.wake_pipe[0] = fds[0];
            cee_exec.wake_pipe[1] = fds[1];
        }
    }
#endif
}

void ceesafe_async_blocking_start(ceesafe_blocking *b, void *(*body)(void *), void *arg) {
    cee_open_wake_pipe();
    b->task = cee_exec.current;
    b->executor = &cee_exec;
    __atomic_store_n(&b->done, 0u, __ATOMIC_SEQ_CST);
    b->thread = ceesafe_thread_start(body, arg);
    if (b->thread == NULL) {
        body(arg); /* no thread: at once, here (as Thread.both, type-system.md §38.6) */
    }
}

void ceesafe_async_blocking_finish(ceesafe_blocking *b) {
    __atomic_store_n(&b->done, 1u, __ATOMIC_SEQ_CST);
    cee_wake_task((cee_executor *)b->executor, (cee_task *)b->task);
}

bool ceesafe_async_blocking_ready(ceesafe_blocking *b) {
    if (__atomic_load_n(&b->done, __ATOMIC_SEQ_CST) != 0u) {
        if (b->thread != NULL) {
            ceesafe_thread_join((cee_thread *)b->thread);
            b->thread = NULL;
        }
        return true;
    }
    cee_task *t = cee_exec.current;
    if (!t->on_wake) {
        t->on_wake = true;
        cee_exec.wake_count += 1u;
    }
    return false;
}

bool ceesafe_async_scope_done(ceesafe_scope *scope) {
    if (scope->live == 0u) {
        return true;
    }
    scope->waiter = cee_exec.current;
    cee_exec.current->on_scope = true;
    return false;
}

static _Noreturn void cee_stuck(void) {
    static const char message[] = "ceesafe: every task is parked with nothing to wake it\n";
    ceesafe_platform_write_stderr((const uint8_t *)message, sizeof message - 1u);
    ceesafe_platform_abort();
}

/* While no task can run: waits for the earliest deadline or a registered descriptor, then readies what is due. */
static void cee_wait(void) {
    uint64_t deadline = UINT64_MAX;
    int registered = 0;
    for (cee_task *t = cee_exec.parked; t != NULL; t = t->next) {
        if (t->deadline < deadline) {
            deadline = t->deadline;
        }
        if (t->shields > 0u && !t->on_scope && t->shield_deadline < deadline) {
            deadline = t->shield_deadline; /* a shield's end wakes its task, to see TimedOut */
        }
        if (t->fd >= 0) {
            registered += 1;
        }
    }
    if (deadline == UINT64_MAX && registered == 0 && cee_exec.wake_count == 0u) {
        cee_stuck();
    }
    for (;;) {
        uint64_t now = ceesafe_async_now();
        int timeout = -1;
        if (deadline != UINT64_MAX) {
            timeout = now >= deadline ? 0 : (deadline - now > (uint64_t)INT_MAX ? INT_MAX : (int)(deadline - now));
        }
        bool any = false;
#if defined(_WIN32)
        if (registered > 0) {
            any = true; /* treated as writable: see ceesafe_async_write */
        } else if (timeout > 0) {
            Sleep((DWORD)timeout);
        }
#else
        /* One entry per waiting descriptor, plus the wake pipe: on the stack up to 64, else from the platform's heap
           (not counted: the executor's own bookkeeping). */
        struct pollfd small_fds[65];
        cee_task *small_owners[65];
        struct pollfd *fds = small_fds;
        cee_task **owners = small_owners;
        size_t capacity = 65u;
        if ((size_t)registered + 1u > capacity) {
            struct pollfd *big_fds = (struct pollfd *)ceesafe_platform_alloc(((size_t)registered + 1u) * sizeof *big_fds);
            cee_task **big_owners = (cee_task **)ceesafe_platform_alloc(((size_t)registered + 1u) * sizeof *big_owners);
            if (big_fds != NULL && big_owners != NULL) {
                fds = big_fds;
                owners = big_owners;
                capacity = (size_t)registered + 1u;
            } else {
                ceesafe_platform_free(big_fds);
                ceesafe_platform_free(big_owners);
            }
        }
        nfds_t count = 0;
        if (cee_exec.wake_count > 0u && cee_exec.wake_pipe[0] >= 0) {
            fds[0].fd = cee_exec.wake_pipe[0]; /* another thread's wake */
            fds[0].events = POLLIN;
            fds[0].revents = 0;
            owners[0] = NULL;
            count = 1u;
        }
        for (cee_task *t = cee_exec.parked; t != NULL && count < capacity; t = t->next) {
            if (t->fd >= 0) {
                fds[count].fd = t->fd;
                fds[count].events = t->fd_events;
                fds[count].revents = 0;
                owners[count] = t;
                count += 1u;
            }
        }
        int ready = poll(count > 0u ? fds : NULL, count, timeout);
        if (ready < 0 && errno == EINTR) {
            if (fds != small_fds) {
                ceesafe_platform_free(fds);
                ceesafe_platform_free(owners);
            }
            continue;
        }
        for (nfds_t k = 0; ready > 0 && k < count; k++) {
            if (fds[k].revents != 0 && owners[k] == NULL) {
                unsigned char drained[64];
                (void)read(cee_exec.wake_pipe[0], drained, sizeof drained);
            } else if (fds[k].revents != 0) {
                cee_unpark(owners[k]); /* the task retries, and sees an error if there was one */
                any = true;
            }
        }
        if (fds != small_fds) {
            ceesafe_platform_free(fds);
            ceesafe_platform_free(owners);
        }
        /* Woken tasks, earliest wake first. */
        for (;;) {
            cee_task *first = NULL;
            uint64_t first_at = UINT64_MAX;
            for (cee_task *w = cee_exec.parked; w != NULL; w = w->next) {
                uint64_t at = __atomic_load_n(&w->woken, __ATOMIC_SEQ_CST);
                if (at != 0u && at < first_at) {
                    first = w;
                    first_at = at;
                }
            }
            if (first == NULL) {
                break;
            }
            __atomic_store_n(&first->woken, 0u, __ATOMIC_SEQ_CST);
            cee_unpark(first);
            any = true;
        }
#endif
        now = ceesafe_async_now();
        /* The tasks whose time has come run earliest deadline first, however late the executor woke. */
        for (;;) {
            cee_task *due = NULL;
            uint64_t due_at = UINT64_MAX;
            for (cee_task *t = cee_exec.parked; t != NULL; t = t->next) {
                uint64_t at = t->deadline;
                if (t->shields > 0u && !t->on_scope && t->shield_deadline < at) {
                    at = t->shield_deadline;
                }
                if (at != UINT64_MAX && now >= at && (due == NULL || at < due_at)) {
                    due = t;
                    due_at = at;
                }
            }
            if (due == NULL) {
                break;
            }
            cee_unpark(due);
            any = true;
        }
        if (any) {
            return;
        }
    }
}

void ceesafe_async_run(void *frame, bool (*resume)(void *)) {
    cee_executor saved = cee_exec;
    cee_exec.ready_head = NULL;
    cee_exec.ready_tail = NULL;
    cee_exec.parked = NULL;
    cee_exec.current = NULL;
    cee_exec.wake_pipe[0] = -1;
    cee_exec.wake_pipe[1] = -1;
    cee_exec.wake_count = 0u;
    uint64_t cancel_at = ceesafe_platform_config("CEESAFE_CANCEL_AT");
    bool cancel_report = ceesafe_platform_config("CEESAFE_CANCEL_REPORT") != 0u;
    cee_task root = {0};
    root.frame = frame;
    root.resume_root = resume;
    root.deadline = UINT64_MAX;
    root.fd = -1;
    root.shield_deadline = UINT64_MAX;
    cee_ready(&root);
    for (;;) {
        cee_task *t = cee_take_ready();
        if (t == NULL) {
            cee_wait();
            continue;
        }
        cee_exec.current = t;
        bool done = t->scope == NULL ? t->resume_root(t->frame) : t->resume_child(t->frame, t->scope);
        cee_exec.current = NULL;
        if (!done) {
            if (__atomic_exchange_n(&t->woken, 0u, __ATOMIC_SEQ_CST) != 0u) {
                if (t->on_wake) {
                    t->on_wake = false;
                    t->channel_wait = false;
                    cee_exec.wake_count -= 1u;
                }
                cee_ready(t); /* woken between its check and its park */
                continue;
            }
            if (t->deadline == UINT64_MAX && t->fd < 0 && !t->on_scope && !t->on_wake && !(t->shields > 0u)) {
                cee_stuck(); /* parked without registering anything: nothing can wake it */
            }
            t->next = cee_exec.parked;
            cee_exec.parked = t;
            if (__atomic_add_fetch(&cee_suspensions, 1u, __ATOMIC_RELAXED) == cancel_at) {
                root.cancelled = true; /* the sweep's cancellation (testing.md §1) */
                cee_wake_waiters();
            }
            continue;
        }
        if (t == &root) {
            break; /* its scopes have ended, so every child has returned */
        }
        ceesafe_scope *scope = t->scope;
        if (!t->embedded) {
            ceesafe_free(t, CEE_ALLOC_SYSTEM);
        }
        scope->live -= 1u;
        if (scope->failed && !scope->cancelled) {
            scope->cancelled = true; /* the first error cancels the other children (type-system.md §40.5) */
            cee_wake_waiters();
        }
        if (scope->live == 0u && scope->waiter != NULL) {
            cee_task *waiter = (cee_task *)scope->waiter;
            scope->waiter = NULL;
            cee_unpark(waiter);
        }
    }
#if !defined(_WIN32)
    if (cee_exec.wake_pipe[0] >= 0) {
        (void)close(cee_exec.wake_pipe[0]);
        (void)close(cee_exec.wake_pipe[1]);
    }
#endif
    if (cancel_report) {
        char line[64];
        int length = snprintf(line, sizeof line, "ceesafe cancel: points=%llu\n",
                              (unsigned long long)__atomic_load_n(&cee_suspensions, __ATOMIC_RELAXED));
        ceesafe_platform_write_stderr((const uint8_t *)line, (size_t)length);
    }
    cee_exec = saved;
}

/* ---- Channels (ADR 0032) ---- */

typedef struct cee_chan_node {
    struct cee_chan_node *next;
    struct cee_chan_node *prev;
    cee_task *task;
    cee_executor *executor;
    uint64_t queued;
} cee_chan_node;

_Static_assert(sizeof(cee_chan_node) <= sizeof(ceesafe_chan_node), "ceesafe_chan_node holds a wait node");

typedef struct cee_chan {
    uint32_t lock;
    uint64_t refs;     /* __atomic */
    uint64_t epoch;    /* __atomic: advances on every send, receive and close */
    uint64_t capacity;
    uint64_t stride;
    uint64_t head;
    uint64_t count;
    bool closed;
    uint32_t blocked;  /* threads waiting in ceesafe_chan_wait (__atomic) */
    cee_chan_node *waiters;
#if defined(_WIN32)
    SRWLOCK m;
    CONDITION_VARIABLE cv;
#else
    pthread_mutex_t m;
    pthread_cond_t cv;
#endif
    unsigned char *data;
} cee_chan;

static void cee_chan_lock(cee_chan *c) {
    while (__atomic_exchange_n(&c->lock, 1u, __ATOMIC_ACQUIRE) != 0u) {
        while (__atomic_load_n(&c->lock, __ATOMIC_RELAXED) != 0u) {
#if !defined(_WIN32)
            sched_yield();
#endif
        }
    }
}

static void cee_chan_unlock(cee_chan *c) {
    __atomic_store_n(&c->lock, 0u, __ATOMIC_RELEASE);
}

void *ceesafe_chan_create(uint64_t capacity, uint64_t size, uint64_t align) {
    uint64_t stride = size == 0u ? 1u : (size + align - 1u) / align * align;
    uint64_t head = (sizeof(cee_chan) + 15u) / 16u * 16u;
    if (capacity > (UINT64_MAX - head) / stride) {
        return NULL;
    }
    unsigned char *block = (unsigned char *)ceesafe_alloc((size_t)(head + capacity * stride), CEE_ALLOC_SYSTEM);
    if (block == NULL) {
        return NULL;
    }
    cee_chan *c = (cee_chan *)(void *)block;
    c->lock = 0u;
    c->refs = 1u;
    c->epoch = 0u;
    c->capacity = capacity;
    c->stride = stride;
    c->head = 0u;
    c->count = 0u;
    c->closed = false;
    c->blocked = 0u;
    c->waiters = NULL;
#if defined(_WIN32)
    InitializeSRWLock(&c->m);
    InitializeConditionVariable(&c->cv);
#else
    (void)pthread_mutex_init(&c->m, NULL);
    (void)pthread_cond_init(&c->cv, NULL);
#endif
    c->data = block + head;
    return c;
}

void ceesafe_chan_retain(void *channel) {
    (void)__atomic_add_fetch(&((cee_chan *)channel)->refs, 1u, __ATOMIC_RELAXED);
}

bool ceesafe_chan_release(void *channel) {
    return __atomic_sub_fetch(&((cee_chan *)channel)->refs, 1u, __ATOMIC_ACQ_REL) == 0u;
}

void ceesafe_chan_free(void *channel) {
    cee_chan *c = (cee_chan *)channel;
#if !defined(_WIN32)
    (void)pthread_cond_destroy(&c->cv);
    (void)pthread_mutex_destroy(&c->m);
#endif
    ceesafe_free(c, CEE_ALLOC_SYSTEM);
}

/* After a change, under the lock: the epoch moves, every waiting task is woken (each retries), and blocked threads are
 * signalled. Waking everyone keeps the waits simple; FIFO order among waiters is kept by their order of retrying. */
static void cee_chan_changed(cee_chan *c) {
    (void)__atomic_add_fetch(&c->epoch, 1u, __ATOMIC_SEQ_CST);
    /* New waiters are at the head: wake from the tail, the oldest first (ADR 0032 §3.4). */
    cee_chan_node *last = c->waiters;
    while (last != NULL && last->next != NULL) {
        last = last->next;
    }
    for (cee_chan_node *w = last; w != NULL; w = w->prev) {
        w->queued = 0u;
        cee_wake_task(w->executor, w->task);
    }
    c->waiters = NULL;
}

static void cee_chan_signal(cee_chan *c) {
    if (__atomic_load_n(&c->blocked, __ATOMIC_SEQ_CST) != 0u) {
#if defined(_WIN32)
        AcquireSRWLockExclusive(&c->m);
        WakeAllConditionVariable(&c->cv);
        ReleaseSRWLockExclusive(&c->m);
#else
        (void)pthread_mutex_lock(&c->m);
        (void)pthread_cond_broadcast(&c->cv);
        (void)pthread_mutex_unlock(&c->m);
#endif
    }
}

void *ceesafe_chan_begin_send(void *channel, uint32_t *status, uint64_t *epoch) {
    cee_chan *c = (cee_chan *)channel;
    cee_chan_lock(c);
    *epoch = __atomic_load_n(&c->epoch, __ATOMIC_SEQ_CST);
    if (c->closed) {
        *status = 2u;
    } else if (c->count == c->capacity) {
        *status = 1u;
    } else {
        *status = 0u;
        return c->data + ((c->head + c->count) % c->capacity) * c->stride; /* the lock is kept until end */
    }
    cee_chan_unlock(c);
    return NULL;
}

void ceesafe_chan_end_send(void *channel) {
    cee_chan *c = (cee_chan *)channel;
    c->count += 1u;
    cee_chan_changed(c);
    cee_chan_unlock(c);
    cee_chan_signal(c);
}

void *ceesafe_chan_begin_recv(void *channel, uint32_t *status, uint64_t *epoch) {
    cee_chan *c = (cee_chan *)channel;
    cee_chan_lock(c);
    *epoch = __atomic_load_n(&c->epoch, __ATOMIC_SEQ_CST);
    if (c->count > 0u) {
        *status = 0u;
        return c->data + c->head * c->stride; /* the lock is kept until end */
    }
    *status = c->closed ? 2u : 1u;
    cee_chan_unlock(c);
    return NULL;
}

void ceesafe_chan_end_recv(void *channel) {
    cee_chan *c = (cee_chan *)channel;
    c->head = (c->head + 1u) % c->capacity;
    c->count -= 1u;
    cee_chan_changed(c);
    cee_chan_unlock(c);
    cee_chan_signal(c);
}

void ceesafe_chan_close(void *channel) {
    cee_chan *c = (cee_chan *)channel;
    cee_chan_lock(c);
    c->closed = true;
    cee_chan_changed(c);
    cee_chan_unlock(c);
    cee_chan_signal(c);
}

/* A thread's wait (send and recv, `sleep`): until the epoch moves from the one its begin saw. */
void ceesafe_chan_wait(void *channel, uint64_t epoch) {
    cee_chan *c = (cee_chan *)channel;
    (void)__atomic_add_fetch(&c->blocked, 1u, __ATOMIC_SEQ_CST);
#if defined(_WIN32)
    AcquireSRWLockExclusive(&c->m);
    while (__atomic_load_n(&c->epoch, __ATOMIC_SEQ_CST) == epoch) {
        (void)SleepConditionVariableSRW(&c->cv, &c->m, INFINITE, 0);
    }
    ReleaseSRWLockExclusive(&c->m);
#else
    (void)pthread_mutex_lock(&c->m);
    while (__atomic_load_n(&c->epoch, __ATOMIC_SEQ_CST) == epoch) {
        (void)pthread_cond_wait(&c->cv, &c->m);
    }
    (void)pthread_mutex_unlock(&c->m);
#endif
    (void)__atomic_sub_fetch(&c->blocked, 1u, __ATOMIC_SEQ_CST);
}

/* A task's wait: false if the epoch already moved (retry at once); else its node is queued and the task waits to be
 * woken (or cancelled: a channel's wait is cancellable, type-system.md §40.7). */
bool ceesafe_chan_park(void *channel, void *wait, uint64_t epoch) {
    cee_chan *c = (cee_chan *)channel;
    cee_chan_node *node = (cee_chan_node *)(void *)wait;
    cee_open_wake_pipe();
    cee_chan_lock(c);
    if (__atomic_load_n(&c->epoch, __ATOMIC_SEQ_CST) != epoch) {
        cee_chan_unlock(c);
        return false;
    }
    node->task = cee_exec.current;
    node->executor = &cee_exec;
    node->queued = 1u;
    node->prev = NULL;
    node->next = c->waiters;
    if (c->waiters != NULL) {
        c->waiters->prev = node;
    }
    c->waiters = node;
    cee_task *t = cee_exec.current;
    if (!t->on_wake) {
        t->on_wake = true;
        t->channel_wait = true;
        cee_exec.wake_count += 1u;
    }
    cee_chan_unlock(c);
    return true;
}

void ceesafe_chan_unpark(void *channel, void *wait) {
    cee_chan *c = (cee_chan *)channel;
    cee_chan_node *node = (cee_chan_node *)(void *)wait;
    cee_chan_lock(c);
    if (node->queued != 0u) {
        if (node->prev != NULL) {
            node->prev->next = node->next;
        } else {
            c->waiters = node->next;
        }
        if (node->next != NULL) {
            node->next->prev = node->prev;
        }
        node->queued = 0u;
    }
    cee_chan_unlock(c);
}
