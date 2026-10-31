/* SPDX-License-Identifier: GPL-2.0-only WITH Classpath-exception-2.0 */
/*
 * Async tasks for the hosted profile (type-system.md §40, ADR 0031; c-backend.md §3a and §6). One executor per thread:
 * Async.run's root task and the children its scopes spawn, run in turn from a queue. A task that parks has registered
 * what wakes it: a deadline (lib/std's Timer), a descriptor becoming writable (its console output), or its scope's last
 * child. While no task can run, the executor waits in poll() for the earliest deadline or descriptor. Child tasks and
 * their frames come from ceesafe_alloc, so the fault-injection sweep reaches them. Linked only into programs that call
 * ceesafe_async_* (c-backend.md §6).
 */
#if defined(__linux__) && !defined(_POSIX_C_SOURCE)
/* ceesafec compiles with -std=c11, under which glibc hides POSIX's clock_gettime and CLOCK_MONOTONIC. (Not on macOS,
   where _POSIX_C_SOURCE would hide the BSD interfaces instead.) */
#define _POSIX_C_SOURCE 200809L
#endif
#include "ceesafe/rt.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <time.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h> /* WSAPoll, WSAEventSelect, and POLLIN/POLLOUT for fd_events */
#include <windows.h>
#if defined(_MSC_VER)
#pragma comment(lib, "ws2_32.lib")
#endif
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
    void (*release)(void *p, size_t size, size_t align); /* Async.scopeIn's allocator; NULL: the system heap */
    size_t block_size;
    const ceesafe_frame_info *info; /* for ceesafe_async_dump */
    bool on_wake;          /* parked until another thread wakes it (Async.blocking, channels) */
    bool channel_wait;     /* on_wake for a channel: a cancellation wakes it too (type-system.md §40.7) */
    uint64_t woken;        /* 0, or the wake's sequence number, set by any thread (ceesafe_atomic_*): readied in that order */
} cee_task;

_Static_assert(sizeof(cee_task) <= sizeof(ceesafe_task_slot), "ceesafe_task_slot holds a task");

/* A child's task and frame are one block: the frame starts at the first max_align_t boundary after the task. */
#define CEE_TASK_HEAD ((sizeof(cee_task) + sizeof(cee_max_align_t) - 1u) / sizeof(cee_max_align_t) * sizeof(cee_max_align_t))

typedef struct {
    cee_task *ready_head;
    cee_task *ready_tail;
    cee_task *parked;
    cee_task *current;
    int wake_pipe[2];    /* another thread's wake: a byte written here makes poll return (-1 until first needed) */
#if defined(_WIN32)
    HANDLE wake_event;   /* Windows: the same, an auto-reset event cee_wait waits on (NULL until first needed) */
#endif
    uint32_t wake_count; /* tasks parked on_wake */
} cee_executor;

static _Thread_local cee_executor cee_exec;

/* The cancellation sweep (testing.md §1): CEESAFE_CANCEL_AT=k cancels the running Async.run's root task at the
 * program's k-th suspension; CEESAFE_CANCEL_REPORT=1 prints the count after each Async.run. */
static uint64_t cee_suspensions; /* every thread's executor counts here (ceesafe_atomic_*) */

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
static uint64_t cee_wake_sequence; /* ceesafe_atomic_*: orders wakes, so woken tasks run first come, first served */

static void cee_wake_task(cee_executor *exec, cee_task *t) {
    uint64_t sequence = ceesafe_atomic_add_u64(&cee_wake_sequence, 1u) + 1u;
    uint64_t none = 0u;
    (void)ceesafe_atomic_cas_u64(&t->woken, none, sequence);
#if defined(_WIN32)
    if (exec->wake_event != NULL) {
        (void)SetEvent(exec->wake_event);
    }
#else
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
#if defined(_WIN32)
    if (t->fd >= CEE_SOCKET_BASE) {
        SOCKET s = (SOCKET)ceesafe_socket_handle(t->fd);
        if (s != INVALID_SOCKET) {
            (void)WSAEventSelect(s, NULL, 0); /* no more signals to this executor's event (the socket stays nonblocking) */
        }
    }
#endif
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
        cee_exec.current->fd_events = POLLOUT;
    }
}

void ceesafe_async_wait_readable(int32_t fd) {
    if (cee_exec.current != NULL) {
        cee_exec.current->fd = fd;
        cee_exec.current->fd_events = POLLIN;
    }
}

int64_t ceesafe_async_poll(const int32_t *fds, const uint32_t *events, uint32_t *ready, uint64_t count, int64_t timeout) {
#if defined(_WIN32)
    /* Sockets through WSAPoll; any other descriptor counts as ready (as the executor's). */
    WSAPOLLFD list[64];
    ULONG polled = 0;
    uint64_t index[64];
    int64_t others = 0;
    if (count > 64u) {
        count = 64u;
    }
    for (uint64_t k = 0; k < count; k++) {
        ready[k] = 0u;
        SOCKET s = fds[k] >= CEE_SOCKET_BASE ? (SOCKET)ceesafe_socket_handle(fds[k]) : INVALID_SOCKET;
        if (s == INVALID_SOCKET) {
            ready[k] = events[k] & 3u;
            others += ready[k] != 0u;
            continue;
        }
        list[polled].fd = s;
        list[polled].events = (SHORT)(((events[k] & 1u) != 0u ? POLLRDNORM : 0) | ((events[k] & 2u) != 0u ? POLLWRNORM : 0));
        list[polled].revents = 0;
        index[polled] = k;
        polled += 1u;
    }
    int n = 0;
    if (polled > 0u) {
        n = WSAPoll(list, polled, others > 0 ? 0 : (timeout < 0 ? -1 : (timeout > INT_MAX ? INT_MAX : (int)timeout)));
        for (ULONG j = 0; n > 0 && j < polled; j++) {
            SHORT r = list[j].revents;
            ready[index[j]] = ((r & (POLLRDNORM | POLLHUP | POLLERR | POLLNVAL)) != 0 ? 1u : 0u) |
                              ((r & (POLLWRNORM | POLLHUP | POLLERR | POLLNVAL)) != 0 ? 2u : 0u);
        }
    } else if (others == 0 && timeout > 0) {
        Sleep(timeout > INT_MAX ? INT_MAX : (DWORD)timeout);
    }
    return n < 0 ? -(int64_t)EIO : (int64_t)(n + others);
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

void *ceesafe_async_spawn(ceesafe_scope *scope, size_t frame_size, bool (*resume)(void *frame, ceesafe_scope *scope),
                          void *(*allocate)(size_t size, size_t align), void (*release)(void *p, size_t size, size_t align),
                          const ceesafe_frame_info *info) {
    scope->owner = cee_exec.current;
    if (scope->oom) {
        return NULL; /* the scope already failed to start a child: it starts no more */
    }
    size_t block_size = CEE_TASK_HEAD + frame_size;
    unsigned char *block = allocate != NULL ? (unsigned char *)allocate(block_size, sizeof(cee_max_align_t))
                                            : (unsigned char *)ceesafe_alloc(block_size, CEE_ALLOC_SYSTEM);
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
    t->release = release;
    t->block_size = block_size;
    t->info = info;
    t->on_wake = false;
    t->channel_wait = false;
    t->woken = 0u;
    scope->live += 1u;
    cee_ready(t);
    return t->frame;
}

void ceesafe_async_start(ceesafe_scope *scope, ceesafe_task_slot *slot, void *frame,
                         bool (*resume)(void *frame, ceesafe_scope *scope), const ceesafe_frame_info *info) {
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
    t->release = NULL;
    t->info = info;
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
#if defined(_WIN32)
    if (cee_exec.wake_event == NULL) {
        cee_exec.wake_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    }
#else
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
    ceesafe_atomic_store_u32(&b->done, 0u);
    b->thread = ceesafe_thread_start(body, arg);
    if (b->thread == NULL) {
        body(arg); /* no thread: at once, here (as Thread.both, type-system.md §38.6) */
    }
}

void ceesafe_async_blocking_finish(ceesafe_blocking *b) {
    ceesafe_atomic_store_u32(&b->done, 1u);
    cee_wake_task((cee_executor *)b->executor, (cee_task *)b->task);
}

bool ceesafe_async_blocking_ready(ceesafe_blocking *b) {
    if (ceesafe_atomic_load_u32(&b->done) != 0u) {
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

#if defined(_WIN32)
/* The parked tasks whose sockets are ready now (WSAPoll, no wait), unparked: how many. A socket that is closed, or
   in error, counts as ready, so its task retries and sees the error. */
static unsigned cee_poll_sockets(void) {
    WSAPOLLFD small[64];
    cee_task *small_owners[64];
    WSAPOLLFD *list = small;
    cee_task **owners = small_owners;
    size_t capacity = 64u;
    size_t waiting = 0;
    for (cee_task *t = cee_exec.parked; t != NULL; t = t->next) {
        waiting += t->fd >= CEE_SOCKET_BASE;
    }
    if (waiting > capacity) {
        WSAPOLLFD *big = (WSAPOLLFD *)ceesafe_platform_alloc(waiting * sizeof *big);
        cee_task **big_owners = (cee_task **)ceesafe_platform_alloc(waiting * sizeof *big_owners);
        if (big != NULL && big_owners != NULL) {
            list = big;
            owners = big_owners;
            capacity = waiting;
        } else {
            ceesafe_platform_free(big);
            ceesafe_platform_free(big_owners);
        }
    }
    ULONG count = 0;
    unsigned readied = 0;
    for (cee_task *t = cee_exec.parked; t != NULL && count < capacity; t = t->next) {
        if (t->fd < CEE_SOCKET_BASE) {
            continue;
        }
        SOCKET s = (SOCKET)ceesafe_socket_handle(t->fd);
        if (s == INVALID_SOCKET) {
            continue; /* closed while it waited: readied below */
        }
        list[count].fd = s;
        list[count].events = t->fd_events == POLLOUT ? POLLWRNORM : POLLRDNORM;
        list[count].revents = 0;
        owners[count] = t;
        count += 1u;
    }
    int n = count > 0u ? WSAPoll(list, count, 0) : 0;
    for (ULONG k = 0; k < count; k++) {
        if (n == SOCKET_ERROR || list[k].revents != 0) {
            cee_unpark(owners[k]);
            readied += 1u;
        }
    }
    for (cee_task *t = cee_exec.parked, *next = NULL; t != NULL; t = next) {
        next = t->next;
        if (t->fd >= CEE_SOCKET_BASE && (SOCKET)ceesafe_socket_handle(t->fd) == INVALID_SOCKET) {
            cee_unpark(t);
            readied += 1u;
        }
    }
    if (list != small) {
        ceesafe_platform_free(list);
        ceesafe_platform_free(owners);
    }
    return readied;
}
#endif

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
        /* A console or file descriptor has no readiness here: its task retries at once (see ceesafe_async_write). */
        bool sockets = false;
        for (cee_task *t = cee_exec.parked, *next = NULL; t != NULL; t = next) {
            next = t->next;
            if (t->fd >= 0 && t->fd < CEE_SOCKET_BASE) {
                cee_unpark(t);
                any = true;
            } else if (t->fd >= CEE_SOCKET_BASE) {
                sockets = true;
            }
        }
        if (sockets) {
            cee_open_wake_pipe();
        }
        if (sockets && cee_exec.wake_event != NULL) {
            /* Each socket signals the executor's event, then WSAPoll says which are ready. A socket that becomes ready
               between the poll and the wait has already been told to signal, so no readiness is lost. */
            for (cee_task *t = cee_exec.parked; t != NULL; t = t->next) {
                SOCKET s = t->fd >= CEE_SOCKET_BASE ? (SOCKET)ceesafe_socket_handle(t->fd) : INVALID_SOCKET;
                if (s != INVALID_SOCKET) {
                    long wanted = t->fd_events == POLLOUT ? (FD_WRITE | FD_CONNECT | FD_CLOSE) : (FD_READ | FD_ACCEPT | FD_CLOSE);
                    (void)WSAEventSelect(s, cee_exec.wake_event, wanted);
                }
            }
            if (!any && cee_poll_sockets() == 0) {
                (void)WaitForSingleObject(cee_exec.wake_event, timeout < 0 ? INFINITE : (DWORD)timeout);
                any = cee_poll_sockets() > 0;
            } else {
                any = true;
            }
        } else if (any) {
            /* retry the unparked tasks at once */
        } else if (cee_exec.wake_count > 0u && cee_exec.wake_event != NULL) {
            (void)WaitForSingleObject(cee_exec.wake_event, timeout < 0 ? INFINITE : (DWORD)timeout); /* or a wake */
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
#endif
        /* Woken tasks, earliest wake first. */
        for (;;) {
            cee_task *first = NULL;
            uint64_t first_at = UINT64_MAX;
            for (cee_task *w = cee_exec.parked; w != NULL; w = w->next) {
                uint64_t at = ceesafe_atomic_load_u64(&w->woken);
                if (at != 0u && at < first_at) {
                    first = w;
                    first_at = at;
                }
            }
            if (first == NULL) {
                break;
            }
            ceesafe_atomic_store_u64(&first->woken, 0u);
            cee_unpark(first);
            any = true;
        }
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

void ceesafe_async_run(void *frame, bool (*resume)(void *), const ceesafe_frame_info *info) {
    cee_executor saved = cee_exec;
    cee_exec.ready_head = NULL;
    cee_exec.ready_tail = NULL;
    cee_exec.parked = NULL;
    cee_exec.current = NULL;
    cee_exec.wake_pipe[0] = -1;
    cee_exec.wake_pipe[1] = -1;
#if defined(_WIN32)
    cee_exec.wake_event = NULL;
#endif
    cee_exec.wake_count = 0u;
    uint64_t cancel_at = ceesafe_platform_config("CEESAFE_CANCEL_AT");
    bool cancel_report = ceesafe_platform_config("CEESAFE_CANCEL_REPORT") != 0u;
    cee_task root = {0};
    root.frame = frame;
    root.resume_root = resume;
    root.deadline = UINT64_MAX;
    root.fd = -1;
    root.shield_deadline = UINT64_MAX;
    root.info = info;
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
            if (ceesafe_atomic_swap_u64(&t->woken, 0u) != 0u) {
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
            if (ceesafe_atomic_add_ord_u64(&cee_suspensions, 1u, 0) + 1u == cancel_at) {
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
            if (t->release != NULL) {
                t->release(t, t->block_size, sizeof(cee_max_align_t));
            } else {
                ceesafe_free(t, CEE_ALLOC_SYSTEM);
            }
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
#if defined(_WIN32)
    if (cee_exec.wake_event != NULL) {
        (void)CloseHandle(cee_exec.wake_event);
    }
#else
    if (cee_exec.wake_pipe[0] >= 0) {
        (void)close(cee_exec.wake_pipe[0]);
        (void)close(cee_exec.wake_pipe[1]);
    }
#endif
    if (cancel_report) {
        char line[64];
        int length = snprintf(line, sizeof line, "ceesafe cancel: points=%llu\n",
                              (unsigned long long)ceesafe_atomic_load_ord_u64(&cee_suspensions, 0));
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
    uint64_t refs;     /* ceesafe_atomic_* */
    uint64_t epoch;    /* ceesafe_atomic_*: advances on every send, receive and close */
    uint64_t capacity;
    uint64_t stride;
    uint64_t head;
    uint64_t count;
    bool closed;
    uint32_t blocked;  /* threads waiting in ceesafe_chan_wait (ceesafe_atomic_*) */
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
    while (ceesafe_atomic_swap_ord_u32(&c->lock, 1u, 1) != 0u) {
        while (ceesafe_atomic_load_ord_u32(&c->lock, 0) != 0u) {
#if !defined(_WIN32)
            sched_yield();
#endif
        }
    }
}

static void cee_chan_unlock(cee_chan *c) {
    ceesafe_atomic_store_ord_u32(&c->lock, 0u, 2);
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
    (void)ceesafe_atomic_add_ord_u64(&((cee_chan *)channel)->refs, 1u, 0);
}

bool ceesafe_chan_release(void *channel) {
    return ceesafe_atomic_add_ord_u64(&((cee_chan *)channel)->refs, UINT64_MAX, 3) == 1u;
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
    (void)ceesafe_atomic_add_u64(&c->epoch, 1u);
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
    if (ceesafe_atomic_load_u32(&c->blocked) != 0u) {
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
    *epoch = ceesafe_atomic_load_u64(&c->epoch);
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
    *epoch = ceesafe_atomic_load_u64(&c->epoch);
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
    (void)ceesafe_atomic_add_u32(&c->blocked, 1u);
#if defined(_WIN32)
    AcquireSRWLockExclusive(&c->m);
    while (ceesafe_atomic_load_u64(&c->epoch) == epoch) {
        (void)SleepConditionVariableSRW(&c->cv, &c->m, INFINITE, 0);
    }
    ReleaseSRWLockExclusive(&c->m);
#else
    (void)pthread_mutex_lock(&c->m);
    while (ceesafe_atomic_load_u64(&c->epoch) == epoch) {
        (void)pthread_cond_wait(&c->cv, &c->m);
    }
    (void)pthread_mutex_unlock(&c->m);
#endif
    (void)ceesafe_atomic_add_u32(&c->blocked, UINT32_MAX);
}

/* A task's wait: false if the epoch already moved (retry at once); else its node is queued and the task waits to be
 * woken (or cancelled: a channel's wait is cancellable, type-system.md §40.7). */
bool ceesafe_chan_park(void *channel, void *wait, uint64_t epoch) {
    cee_chan *c = (cee_chan *)channel;
    cee_chan_node *node = (cee_chan_node *)(void *)wait;
    cee_open_wake_pipe();
    cee_chan_lock(c);
    if (ceesafe_atomic_load_u64(&c->epoch) != epoch) {
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

/* ---- ceesafe_async_tasks: the current thread's tasks, for a debugger (editors/debug, `ceesafe-tasks`) ---- */

static char cee_tasks_text[8192];

#define CEE_TASKS_SHOWN 256u

typedef struct {
    const cee_task *task;
    const char *how;
} cee_shown_task;

static size_t cee_tasks_line(size_t used, const cee_shown_task *shown, unsigned depth) {
    const cee_task *t = shown->task;
    const ceesafe_frame_info *info = t->info;
    uint32_t state = t->frame != NULL ? *(const uint32_t *)t->frame : 0u; /* every frame starts with cee_state */
    uint32_t at = info != NULL && state < info->count ? info->lines[state] : 0u;
    size_t room = sizeof cee_tasks_text - used;
    int n = snprintf(cee_tasks_text + used, room, "%*s%s at %s:%u (%s)\n", (int)(2u * depth), "",
                     info != NULL ? info->name : "?", info != NULL ? info->file : "?", (unsigned)at, shown->how);
    return n < 0 ? used : (size_t)n < room ? used + (size_t)n : sizeof cee_tasks_text - 1u;
}

/* Task k, then the tasks whose scope it owns, depth first. */
static size_t cee_tasks_tree(size_t used, const cee_shown_task *shown, size_t count, size_t k, unsigned depth) {
    used = cee_tasks_line(used, &shown[k], depth);
    for (size_t j = 0; j < count && depth < 64u; j++) {
        const ceesafe_scope *scope = shown[j].task->scope;
        if (scope != NULL && scope->owner == (const void *)shown[k].task) {
            used = cee_tasks_tree(used, shown, count, j, depth + 1u);
        }
    }
    return used;
}

const char *ceesafe_async_tasks(void) {
    cee_shown_task shown[CEE_TASKS_SHOWN];
    size_t count = 0;
    if (cee_exec.current != NULL) {
        shown[count++] = (cee_shown_task){cee_exec.current, "running"};
    }
    for (const cee_task *t = cee_exec.ready_head; t != NULL && count < CEE_TASKS_SHOWN; t = t->next) {
        shown[count++] = (cee_shown_task){t, "ready"};
    }
    for (const cee_task *t = cee_exec.parked; t != NULL && count < CEE_TASKS_SHOWN; t = t->next) {
        shown[count++] = (cee_shown_task){t, t->on_scope ? "waiting for its scope" : t->on_wake ? "waiting to be woken" :
                                                 t->fd >= 0 ? "waiting on a descriptor" : "waiting for a deadline"};
    }
    size_t used = 0;
    cee_tasks_text[0] = '\0';
    for (size_t k = 0; k < count; k++) {
        /* A root: no scope, or a scope whose owner is not among the shown tasks. */
        bool root = true;
        const ceesafe_scope *scope = shown[k].task->scope;
        for (size_t j = 0; j < count && scope != NULL; j++) {
            if (scope->owner == (const void *)shown[j].task) {
                root = false;
            }
        }
        if (root) {
            used = cee_tasks_tree(used, shown, count, k, 0u);
        }
    }
    return cee_tasks_text;
}

void ceesafe_async_dump(void) {
    const char *text = ceesafe_async_tasks();
    size_t length = 0;
    while (text[length] != '\0') {
        length += 1u;
    }
    ceesafe_platform_write_stderr((const uint8_t *)text, length);
}
