/* SPDX-License-Identifier: GPL-2.0-only WITH Classpath-exception-2.0 */
/*
 * Async tasks for the hosted profile (type-system.md §40, ADR 0031; c-backend.md §3a and §6). One executor per thread:
 * Async.run's root task and the children its scopes spawn, run in turn from a queue. A task that parks has registered
 * what wakes it: a deadline (lib/std's Timer), a descriptor becoming writable (its console output), or its scope's last
 * child. While no task can run, the executor waits in poll() for the earliest deadline or descriptor. Child tasks and
 * their frames come from ceesafe_alloc, so the fault-injection sweep reaches them. Linked only into programs that call
 * ceesafe_async_* (c-backend.md §6).
 */
#if defined(CEE_FREESTANDING)
/* The freestanding executor (ADR 0039): only the portable part below, over the environment's ceesafe_env_* hooks. */
#include "ceesafe/rt.h"
#else
#if defined(__APPLE__) && !defined(__APPLE_USE_RFC_3542)
#define __APPLE_USE_RFC_3542 1 /* IPV6_PKTINFO and IPV6_TCLASS as control messages (ADR 0050) */
#endif
#if defined(__linux__) && !defined(_GNU_SOURCE)
/* ceesafec compiles with -std=c11, under which glibc hides POSIX's clock_gettime and CLOCK_MONOTONIC, and syscall(),
   which io_uring needs (ADR 0035). (Not on macOS, where such a macro would hide the BSD interfaces instead.) */
#define _GNU_SOURCE
#endif
#include "ceesafe/rt.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h> /* WSAPoll, WSAEventSelect, and POLLIN/POLLOUT for fd_events */
#include <ws2tcpip.h> /* sockaddr_in6 (ADR 0044) */
#include <windows.h>
#if defined(_MSC_VER)
#pragma comment(lib, "ws2_32.lib")
#endif
#elif defined(CEE_COSMOOS)
/* CosmoOS userland (ADR 0040): no poll, pthreads, sched_yield or fcntl. The executor waits on its own condition
   variable (cee_wait_cosmoos) and descriptors count as ready, so there is no reactor. */
#include <cosmo/thread.h>
#include <unistd.h>
#define CEE_NO_REACTOR 1
#define POLLIN 0x001 /* fd_events' two directions, numbered as poll.h numbers them */
#define POLLOUT 0x004
#else
#include <poll.h>
#include <pthread.h>
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#define CEE_KQUEUE 1
#include <sys/event.h>
#elif defined(__linux__)
#define CEE_EPOLL 1
#include <sys/epoll.h>
#if defined(__has_include)
#if __has_include(<linux/io_uring.h>)
#define CEE_URING 1
#include <linux/io_uring.h>
#include <netinet/in.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#endif
#endif
#endif
#include <fcntl.h>
#include <sched.h>
#include <netinet/in.h> /* in_pktinfo, in6_pktinfo (ADR 0050) */
#include <sys/socket.h> /* sendmsg (ADR 0045) */
#include <sys/uio.h>    /* struct iovec, writev */
#if defined(__linux__)
#include <netinet/udp.h> /* UDP_SEGMENT (ADR 0050) */
#if !defined(UDP_SEGMENT)
#define UDP_SEGMENT 103
#endif
#if !defined(SOL_UDP)
#define SOL_UDP 17
#endif
#endif
#if defined(__linux__)
#include <sys/sendfile.h> /* sendfile (ADR 0051) */
#endif
#include <unistd.h>
/* Empties a wake pipe, or reads an eventfd's counter, whose value nothing needs: how many bytes, and a failure
   (EAGAIN: already empty), change nothing. glibc declares read warn_unused_result under _FORTIFY_SOURCE, which
   Ubuntu's gcc defines at -O2, and gcc does not take a (void) cast as using a result: the result is kept, then
   dropped, so -Werror builds pass there too. */
static inline void cee_drain(int fd, void *buffer, size_t length) {
    ssize_t got = read(fd, buffer, length);
    (void)got;
}
#endif

#ifndef PIPE_BUF
#define PIPE_BUF 512 /* POSIX's minimum (_POSIX_PIPE_BUF) */
#endif
#endif /* CEE_FREESTANDING */

typedef struct cee_task {
    void *frame;
    union {
        bool (*root)(void *);
        bool (*child)(void *, ceesafe_scope *);
    } resume;              /* root when scope is NULL, else child */
    ceesafe_scope *scope;  /* the child's scope; NULL for the root */
    struct cee_task *next; /* the ready queue, or the parked list */
    struct cee_task *prev; /* the parked list */
    struct cee_task *link_next; /* the tasks waiting on its descriptor in its direction, or waiting to be woken */
    struct cee_task *link_prev;
    void (*release)(void *p, size_t size, size_t align); /* Async.scopeIn's allocator; NULL: the system heap */
    const ceesafe_frame_info *info; /* for ceesafe_async_tasks */
    size_t block_size;
    uint64_t deadline;     /* UINT64_MAX for none */
    uint64_t shield_deadline;
    uint64_t woken;        /* 0, or the wake's sequence number, set by any thread (ceesafe_atomic_*): readied in that order */
    int32_t fd;            /* -1 for none */
    uint32_t shields;      /* Async.shield depth (type-system.md §40.7) */
    uint32_t heap_index;   /* 1 + its place in the timer heap; 0 if it is not there */
    short fd_events;       /* POLLIN or POLLOUT */
    void *home;            /* the executor that runs it (ADR 0038: a spawnSend child's is the worker that took it) */
    uint32_t cancelled;    /* cancelled itself (the sweep's root), read atomically; see cee_cancelled */
    bool on_scope;         /* parked until its scope's last child returns */
    bool unpublished;      /* a spawnSend child whose frame is not yet initialised (ADR 0038) */
    bool embedded;         /* in its caller's frame (Async.race, Async.all): never freed */
    bool on_wake;          /* parked until another thread wakes it (Async.blocking, channels) */
    bool channel_wait;     /* on_wake for a channel: a cancellation wakes it too (type-system.md §40.7) */
    bool parked;           /* on the parked list */
    bool on_op;            /* waiting for an IOCP completion (ADR 0035): readied by it, cancellable while channel_wait */
} cee_task;

_Static_assert(sizeof(cee_task) <= sizeof(ceesafe_task_slot), "ceesafe_task_slot holds a task");

/* A child's task and frame are one block: the frame starts at the first max_align_t boundary after the task (its
   alignment, not its size: x86-64's max_align_t is 32 bytes aligned to 16, and a kernel heap gives 16). */
#define CEE_TASK_HEAD ((sizeof(cee_task) + _Alignof(cee_max_align_t) - 1u) / _Alignof(cee_max_align_t) * _Alignof(cee_max_align_t))

/* The tasks waiting on one descriptor (POSIX): readers and writers, each in the order they parked. */
typedef struct {
    cee_task *head[2]; /* [0] reading, [1] writing */
    cee_task *tail[2];
    uint32_t active;   /* 1 + its place in active_fds; 0 if no task waits on it */
    uint8_t armed;     /* the directions the reactor is armed for (bit 0 reading, bit 1 writing) */
    bool added;        /* epoll: the descriptor was added (it may still be, disabled, after a one-shot event) */
} cee_fd_slot;

/* An executor (L1 A4b): the ready queue; the parked list, which only cancellation and the debugger walk; and what
 * wakes parked tasks, each reached without walking the parked list: a heap of deadlines, a table of descriptor
 * waiters, and the tasks waiting to be woken. Descriptors wait in kqueue or epoll, armed one-shot at each park, or in
 * poll() (CEESAFE_REACTOR_POLL=1, or a host with neither). Its arrays are the runtime's own bookkeeping, from the
 * platform's heap. */
typedef struct {
    cee_task *ready_head;
    cee_task *ready_tail;
    cee_task *parked;
    cee_task *current;
    int wake_pipe[2];    /* another thread's wake: a byte written here ends the wait (-1 until first needed) */
#if defined(CEE_COSMOOS)
    cosmo_mutex_t wake_lock; /* CosmoOS (ADR 0040): what replaces the wake pipe. A wake sets wake_pending and */
    cosmo_cond_t wake_cond;  /* signals wake_cond under wake_lock, so one that comes before the wait is kept */
    uint32_t wake_pending;
#endif
#if defined(_WIN32)
    HANDLE wake_event;   /* Windows: the same, an auto-reset event cee_wait waits on (NULL until first needed) */
    HANDLE port;         /* Windows: the thread's completion port (IOCP, ADR 0035), or NULL with CEESAFE_REACTOR_POLL */
#endif
    uint32_t wake_count; /* tasks parked on_wake */
    uint32_t fd_count;   /* tasks parked on a descriptor */
    uint32_t file_count; /* Windows: those on a console or file descriptor, which count as ready */
    uint32_t op_count;   /* tasks waiting for an operation's completion (IOCP) */
    cee_task *wake_list; /* the tasks parked on_wake, linked by link_next */
    cee_task **heap;     /* the parked tasks with a deadline: a binary min-heap on cee_task_key */
    uint32_t heap_count;
    uint32_t heap_capacity;
    cee_fd_slot *slots;  /* by descriptor (POSIX) */
    uint32_t slot_count;
    int32_t *active_fds; /* the descriptors some task waits on */
    uint32_t active_count;
    uint32_t active_capacity;
    int reactor;         /* the kqueue or epoll descriptor; -1 until first needed */
    bool use_poll;       /* poll() for descriptors */
    bool pipe_watched;   /* the wake pipe is registered with the reactor */
#if defined(CEE_KQUEUE)
    struct kevent changes[64]; /* kqueue: arming and disarming not yet sent, sent with the next wait */
    int change_count;
#endif
#if defined(CEE_URING)
    struct cee_uring *uring;   /* io_uring for socket operations (CEESAFE_REACTOR_URING=1, ADR 0035), or NULL */
    bool want_uring;           /* asked for, not yet set up */
#endif
    struct cee_run *run;       /* the run it is a worker of when it has more than one (ADR 0038), else NULL */
    struct cee_task *root;     /* freestanding: the run's root (the hosted runtime's cee_cancel_root) */
    uint64_t wake_sequence;    /* freestanding: the hosted runtime's global cee_wake_sequence */
    uint64_t cancel_seen;      /* the run's cancel_epoch when this executor last rechecked its parked tasks */
} cee_executor;

#if defined(CEE_FREESTANDING)
/* No thread-local storage or mutable globals (profiles.md §4.4): the environment keeps each thread's executor. */
#define cee_exec (*(cee_executor *)*ceesafe_env_slot())
#else
static _Thread_local cee_executor cee_exec;
#endif

/* ---- Work stealing (ADR 0038) ----
 * Async.runWorkers(n, task) runs with n workers, the caller among them: each is an executor as above, on its own
 * thread. The run shares a queue of spawnSend children that have not started; a worker with nothing ready takes the
 * oldest. A started task stays on its worker, so frames, deadlines and descriptors never cross threads. A scope with
 * such children is shared: its count, its first error, its cancellation and its waiter change under the run's lock,
 * a cancellation is announced to every worker (cancel_epoch), and its last child wakes the waiting parent through the
 * parent's executor. Each worker counts the run as one more reason to wait for a wake, so its wake pipe is watched. */
#define CEE_MAX_WORKERS 64u

typedef struct cee_run {
    void *lock;               /* a ceesafe mutex: the queue, the executor list and every shared scope's state */
    cee_task *queue_head;     /* spawnSend children not yet started, oldest first, linked by next */
    cee_task *queue_tail;
    uint32_t queued;          /* their number, read atomically without the lock */
    uint32_t done;            /* atomic: the root has returned, so the workers stop */
    uint64_t cancel_epoch;    /* atomic: incremented when a shared scope is cancelled */
    cee_executor *executors[CEE_MAX_WORKERS]; /* the workers' executors, for wakes */
    uint32_t executor_count;
    uint32_t workers;         /* asked for, the caller included */
} cee_run;

#if !defined(CEE_FREESTANDING)
static _Thread_local uint32_t cee_next_workers; /* Async.runWorkers: the next run's workers, read and reset by it */

void ceesafe_async_workers(uint32_t n) { cee_next_workers = n; }
#endif

/* Ends another executor's wait, from any thread (its wake pipe, event or port). */
static void cee_poke(cee_executor *exec);

/* Pokes every other worker of the run: new work, a cancellation, or the run's end. */
static void cee_run_poke(cee_run *run) {
#if defined(CEE_FREESTANDING)
    (void)run; /* one worker */
    return;
#else
    ceesafe_mutex_lock(run->lock);
    for (uint32_t k = 0; k < run->executor_count; k++) {
        if (run->executors[k] != &cee_exec) {
            cee_poke(run->executors[k]);
        }
    }
    ceesafe_mutex_unlock(run->lock);
#endif
}

static void cee_scope_lock(ceesafe_scope *scope) {
#if !defined(CEE_FREESTANDING)
    if (scope->shared) {
        ceesafe_mutex_lock(cee_exec.run->lock);
    }
#else
    (void)scope;
#endif
}

static void cee_scope_unlock(ceesafe_scope *scope) {
#if !defined(CEE_FREESTANDING)
    if (scope->shared) {
        ceesafe_mutex_unlock(cee_exec.run->lock);
    }
#else
    (void)scope;
#endif
}

/* Cancels a scope's children (type-system.md §40.5): every worker rechecks its parked tasks if the scope is shared. */
static void cee_wake_waiters(void);

static void cee_cancel_scope(ceesafe_scope *scope) {
    ceesafe_atomic_store_u32(&scope->cancelled, 1u);
    cee_wake_waiters();
    if (scope->shared && cee_exec.run != NULL) {
        (void)ceesafe_atomic_add_u64(&cee_exec.run->cancel_epoch, 1u);
        cee_run_poke(cee_exec.run);
    }
}

/* The oldest unstarted spawnSend child, taken by this worker; NULL if there is none. */
static cee_task *cee_steal(void) {
#if defined(CEE_FREESTANDING)
    return NULL;
#else
    cee_run *run = cee_exec.run;
    if (run == NULL || ceesafe_atomic_load_u32(&run->queued) == 0u) {
        return NULL;
    }
    ceesafe_mutex_lock(run->lock);
    cee_task *t = run->queue_head;
    if (t != NULL) {
        run->queue_head = t->next;
        if (run->queue_head == NULL) {
            run->queue_tail = NULL;
        }
        (void)ceesafe_atomic_add_u32(&run->queued, UINT32_MAX); /* minus one */
        t->next = NULL;
    }
    ceesafe_mutex_unlock(run->lock);
    return t;
#endif
}

/* The cancellation sweep (testing.md §1): CEESAFE_CANCEL_AT=k cancels the running Async.run's root task at the
 * program's k-th suspension; CEESAFE_CANCEL_REPORT=1 prints the count after each Async.run. */
#if defined(CEE_FREESTANDING)
#define cee_cancel_root (cee_exec.root)
#else
static uint64_t cee_suspensions; /* every thread's executor counts here (ceesafe_atomic_*) */
static _Thread_local cee_task *cee_cancel_root; /* the running Async.run's root, which the sweep cancels */
static uint64_t cee_cancel_woken; /* tasks cancellations woke, every thread's (ceesafe_atomic_*): CEESAFE_CANCEL_REPORT */
#endif

/* Whether a task is cancelled: itself, or its scope (a sibling failed), or the task that owns its scope. */
static bool cee_cancelled(cee_task *t) {
    while (t != NULL) {
        if (ceesafe_atomic_load_u32(&t->cancelled) != 0u) {
            return true;
        }
        if (t->scope == NULL) {
            return false;
        }
        if (ceesafe_atomic_load_u32(&t->scope->cancelled) != 0u) {
            return true;
        }
        t = (cee_task *)t->scope->owner;
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

static _Noreturn void cee_out_of_memory(void) {
#if defined(CEE_FREESTANDING)
    ceesafe_panic(CEE_PANIC_ASYNC, CEE_LOC("ceesafe_rt_async.c", 0u, 0u));
#else
    static const char message[] = "ceesafe: out of memory for the executor's own bookkeeping\n";
    ceesafe_platform_write_stderr((const uint8_t *)message, sizeof message - 1u);
    ceesafe_platform_abort();
#endif
}

/* ---- The timer heap: the earliest deadline first, a shield's end counting as one (type-system.md §40.7). ---- */

static uint64_t cee_task_key(const cee_task *t) {
    uint64_t at = t->deadline;
    if (t->shields > 0u && !t->on_scope && t->shield_deadline < at) {
        at = t->shield_deadline; /* a shield's end wakes its task, to see TimedOut */
    }
    return at;
}

#if !defined(CEE_FREESTANDING)
static void cee_heap_set(uint32_t k, cee_task *t) {
    cee_exec.heap[k] = t;
    t->heap_index = k + 1u;
}

static void cee_heap_up(uint32_t k) {
    cee_task *t = cee_exec.heap[k];
    uint64_t key = cee_task_key(t);
    while (k > 0u) {
        uint32_t parent = (k - 1u) / 2u;
        if (cee_task_key(cee_exec.heap[parent]) <= key) {
            break;
        }
        cee_heap_set(k, cee_exec.heap[parent]);
        k = parent;
    }
    cee_heap_set(k, t);
}

static void cee_heap_down(uint32_t k) {
    cee_task *t = cee_exec.heap[k];
    uint64_t key = cee_task_key(t);
    for (;;) {
        uint32_t left = 2u * k + 1u;
        if (left >= cee_exec.heap_count) {
            break;
        }
        uint32_t child = left;
        if (left + 1u < cee_exec.heap_count && cee_task_key(cee_exec.heap[left + 1u]) < cee_task_key(cee_exec.heap[left])) {
            child = left + 1u;
        }
        if (cee_task_key(cee_exec.heap[child]) >= key) {
            break;
        }
        cee_heap_set(k, cee_exec.heap[child]);
        k = child;
    }
    cee_heap_set(k, t);
}

static void cee_heap_push(cee_task *t) {
    if (cee_exec.heap_count == cee_exec.heap_capacity) {
        uint32_t capacity = cee_exec.heap_capacity == 0u ? 64u : cee_exec.heap_capacity * 2u;
        cee_task **grown = (cee_task **)ceesafe_platform_realloc(cee_exec.heap, (size_t)capacity * sizeof *grown);
        if (grown == NULL) {
            cee_out_of_memory();
        }
        cee_exec.heap = grown;
        cee_exec.heap_capacity = capacity;
    }
    cee_heap_set(cee_exec.heap_count, t);
    cee_exec.heap_count += 1u;
    cee_heap_up(cee_exec.heap_count - 1u);
}

static void cee_heap_remove(cee_task *t) {
    uint32_t k = t->heap_index - 1u;
    t->heap_index = 0u;
    cee_exec.heap_count -= 1u;
    if (k < cee_exec.heap_count) {
        cee_task *moved = cee_exec.heap[cee_exec.heap_count]; /* the last entry fills the hole, then settles */
        cee_heap_set(k, moved);
        cee_heap_up(k);
        cee_heap_down(moved->heap_index - 1u);
    }
}
#else
/* Freestanding: no heap for the executor's arrays, so the deadlines are found by walking the parked tasks, which a
   kernel's handful of tasks keeps short (ADR 0039). */
static void cee_heap_push(cee_task *t) { (void)t; }
static void cee_heap_remove(cee_task *t) { (void)t; }
#endif

static void cee_unpark(cee_task *t);
static void cee_open_wake_pipe(void);
#if defined(CEE_URING)
static void cee_uring_close(void);
static int cee_uring_eventfd(void);
static bool cee_uring_drain(void);
#endif
#if defined(_WIN32)
static HANDLE cee_iocp_port(void);
#endif

#if !defined(_WIN32) && !defined(CEE_FREESTANDING) && !defined(CEE_NO_REACTOR)
/* ---- Descriptor waiters (POSIX) and the reactor. ---- */

static unsigned cee_direction(const cee_task *t) { return t->fd_events == POLLOUT ? 1u : 0u; }

/* Every task waiting on `fd` in `direction` runs again: it retries, and sees an error if there was one. */
static void cee_fd_ready(int32_t fd, unsigned direction) {
    while ((uint32_t)fd < cee_exec.slot_count && cee_exec.slots[fd].head[direction] != NULL) {
        cee_unpark(cee_exec.slots[fd].head[direction]);
    }
}

static bool cee_reactor_open(void) {
    if (cee_exec.reactor >= 0 || cee_exec.use_poll) {
        return !cee_exec.use_poll;
    }
#if defined(CEE_KQUEUE)
    cee_exec.reactor = kqueue();
#elif defined(CEE_EPOLL)
    cee_exec.reactor = epoll_create1(EPOLL_CLOEXEC);
#endif
    if (cee_exec.reactor < 0) {
        cee_exec.use_poll = true; /* no kqueue or epoll: poll() */
    }
    return !cee_exec.use_poll;
}

#if defined(CEE_KQUEUE)
/* Sends the queued changes and takes the events kqueue has, waiting at most `limit` (NULL: until one comes). A change
   that failed comes back as an EV_ERROR event, which readies its direction's tasks: their retry sees the error (a
   failed EV_DELETE readies none, or retries one at worst). True if a task was readied. */
static bool cee_kqueue_call(const struct timespec *limit) {
    struct kevent events[64];
    int n = kevent(cee_exec.reactor, cee_exec.changes, cee_exec.change_count, events, 64, limit);
    cee_exec.change_count = 0;
    bool any = false;
    for (int k = 0; k < n; k++) {
        int32_t fd = (int32_t)events[k].ident;
        if (fd == cee_exec.wake_pipe[0] && (events[k].flags & EV_ERROR) == 0) {
            unsigned char drained[64];
            cee_drain(fd, drained, sizeof drained);
            continue;
        }
        unsigned d = events[k].filter == EVFILT_WRITE ? 1u : 0u;
        if ((uint32_t)fd < cee_exec.slot_count) {
            cee_exec.slots[fd].armed &= (uint8_t)~(1u << d); /* one-shot, or failed: no longer armed */
            any = any || cee_exec.slots[fd].head[d] != NULL;
            cee_fd_ready(fd, d);
        }
    }
    return any;
}

static void cee_kqueue_change(int32_t fd, int16_t filter, uint16_t flags) {
    if (cee_exec.change_count == 64) {
        static const struct timespec now = {0, 0};
        (void)cee_kqueue_call(&now); /* full: send them, taking what is ready (those tasks run after the wait) */
    }
    EV_SET(&cee_exec.changes[cee_exec.change_count], (uintptr_t)fd, filter, flags, 0, 0, NULL);
    cee_exec.change_count += 1;
}
#endif

/* Arms the reactor for the directions `fd`'s tasks wait in, one-shot. A failure readies them: their retry sees it. */
static void cee_reactor_arm(int32_t fd) {
    cee_fd_slot *slot = &cee_exec.slots[fd];
    uint8_t wanted = (uint8_t)((slot->head[0] != NULL ? 1u : 0u) | (slot->head[1] != NULL ? 2u : 0u));
    if (wanted == slot->armed || !cee_reactor_open()) {
        return;
    }
#if defined(CEE_KQUEUE)
    for (unsigned d = 0; d < 2u; d++) {
        uint8_t bit = (uint8_t)(1u << d);
        if ((wanted & bit) != 0u && (slot->armed & bit) == 0u) {
            cee_kqueue_change(fd, d == 1u ? EVFILT_WRITE : EVFILT_READ, EV_ADD | EV_ONESHOT);
            slot->armed |= bit;
        }
    }
#elif defined(CEE_EPOLL)
    struct epoll_event change;
    ceesafe_mem_set(&change, 0, sizeof change);
    change.events = ((wanted & 1u) != 0u ? (uint32_t)EPOLLIN : 0u) | ((wanted & 2u) != 0u ? (uint32_t)EPOLLOUT : 0u) |
                    (uint32_t)EPOLLONESHOT;
    change.data.fd = fd;
    int status = epoll_ctl(cee_exec.reactor, slot->added ? EPOLL_CTL_MOD : EPOLL_CTL_ADD, fd, &change);
    if (status < 0 && errno == ENOENT) {
        status = epoll_ctl(cee_exec.reactor, EPOLL_CTL_ADD, fd, &change); /* closed and reopened since */
    } else if (status < 0 && errno == EEXIST) {
        status = epoll_ctl(cee_exec.reactor, EPOLL_CTL_MOD, fd, &change);
    }
    if (status < 0) {
        /* EPERM: a regular file, always ready; anything else, the retry sees it. */
        slot->added = false;
        slot->armed = 0u;
        cee_fd_ready(fd, 0u);
        cee_fd_ready(fd, 1u);
        return;
    }
    slot->added = true;
    slot->armed = wanted;
#endif
}

/* Takes back what the reactor is armed for and no task waits for any more (a task left early: cancelled, or due). */
static void cee_reactor_disarm(int32_t fd) {
    cee_fd_slot *slot = &cee_exec.slots[fd];
    uint8_t wanted = (uint8_t)((slot->head[0] != NULL ? 1u : 0u) | (slot->head[1] != NULL ? 2u : 0u));
    if ((slot->armed & ~wanted) == 0u || cee_exec.reactor < 0) {
        return;
    }
#if defined(CEE_KQUEUE)
    for (unsigned d = 0; d < 2u; d++) {
        uint8_t bit = (uint8_t)(1u << d);
        if ((slot->armed & bit) != 0u && (wanted & bit) == 0u) {
            cee_kqueue_change(fd, d == 1u ? EVFILT_WRITE : EVFILT_READ, EV_DELETE);
            slot->armed &= (uint8_t)~bit;
        }
    }
#elif defined(CEE_EPOLL)
    if (wanted == 0u) {
        (void)epoll_ctl(cee_exec.reactor, EPOLL_CTL_DEL, fd, NULL);
        slot->added = false;
        slot->armed = 0u;
    } else {
        slot->armed = 0u; /* re-armed for what is still wanted */
        cee_reactor_arm(fd);
    }
#endif
}

static void cee_fd_attach(cee_task *t) {
    int32_t fd = t->fd;
    if ((uint32_t)fd >= cee_exec.slot_count) {
        uint32_t count = cee_exec.slot_count == 0u ? 64u : cee_exec.slot_count;
        while (count <= (uint32_t)fd) {
            count *= 2u;
        }
        cee_fd_slot *grown = (cee_fd_slot *)ceesafe_platform_realloc(cee_exec.slots, (size_t)count * sizeof *grown);
        if (grown == NULL) {
            cee_out_of_memory();
        }
        ceesafe_mem_set(grown + cee_exec.slot_count, 0, (size_t)(count - cee_exec.slot_count) * sizeof *grown);
        cee_exec.slots = grown;
        cee_exec.slot_count = count;
    }
    cee_fd_slot *slot = &cee_exec.slots[fd];
    unsigned d = cee_direction(t);
    t->link_next = NULL;
    t->link_prev = slot->tail[d];
    if (slot->tail[d] != NULL) {
        slot->tail[d]->link_next = t;
    } else {
        slot->head[d] = t;
    }
    slot->tail[d] = t;
    if (slot->active == 0u) {
        if (cee_exec.active_count == cee_exec.active_capacity) {
            uint32_t capacity = cee_exec.active_capacity == 0u ? 64u : cee_exec.active_capacity * 2u;
            int32_t *grown = (int32_t *)ceesafe_platform_realloc(cee_exec.active_fds, (size_t)capacity * sizeof *grown);
            if (grown == NULL) {
                cee_out_of_memory();
            }
            cee_exec.active_fds = grown;
            cee_exec.active_capacity = capacity;
        }
        cee_exec.active_fds[cee_exec.active_count] = fd;
        cee_exec.active_count += 1u;
        slot->active = cee_exec.active_count;
    }
    if (!cee_exec.use_poll) {
        cee_reactor_arm(fd); /* last: a failure unparks the task */
    }
}

static void cee_fd_detach(cee_task *t) {
    int32_t fd = t->fd;
    cee_fd_slot *slot = &cee_exec.slots[fd];
    unsigned d = cee_direction(t);
    if (t->link_prev != NULL) {
        t->link_prev->link_next = t->link_next;
    } else {
        slot->head[d] = t->link_next;
    }
    if (t->link_next != NULL) {
        t->link_next->link_prev = t->link_prev;
    } else {
        slot->tail[d] = t->link_prev;
    }
    t->link_next = NULL;
    t->link_prev = NULL;
    if (slot->head[0] == NULL && slot->head[1] == NULL && slot->active != 0u) {
        uint32_t k = slot->active - 1u;
        int32_t last = cee_exec.active_fds[cee_exec.active_count - 1u];
        cee_exec.active_fds[k] = last;
        cee_exec.slots[last].active = k + 1u;
        cee_exec.active_count -= 1u;
        slot->active = 0u;
    }
    cee_reactor_disarm(fd);
}
#endif

/* Parks the task that just suspended: on the parked list, and with what wakes it. */
static void cee_park(cee_task *t) {
    t->parked = true;
    t->prev = NULL;
    t->next = cee_exec.parked;
    if (cee_exec.parked != NULL) {
        cee_exec.parked->prev = t;
    }
    cee_exec.parked = t;
    if (cee_task_key(t) != UINT64_MAX) {
        cee_heap_push(t);
    }
    if (t->on_wake) {
#if !defined(CEE_FREESTANDING)
        if (t->fd >= 0) {
            static const char message[] = "ceesafe: a task waits on a descriptor and a wake at once\n";
            ceesafe_platform_write_stderr((const uint8_t *)message, sizeof message - 1u);
            ceesafe_platform_abort();
        }
#endif
        t->link_prev = NULL;
        t->link_next = cee_exec.wake_list;
        if (cee_exec.wake_list != NULL) {
            cee_exec.wake_list->link_prev = t;
        }
        cee_exec.wake_list = t;
    }
    if (t->fd >= 0) {
        cee_exec.fd_count += 1u;
#if defined(_WIN32)
        if (t->fd >= CEE_SOCKET_BASE) {
            cee_open_wake_pipe();
            if (cee_exec.wake_event != NULL) {
                ceesafe_socket_select(t->fd, cee_exec.wake_event); /* every direction: another task may wait on it too */
            }
        } else {
            cee_exec.file_count += 1u; /* no readiness for consoles and files: readied by the next wait */
        }
#elif !defined(CEE_FREESTANDING) && !defined(CEE_NO_REACTOR)
        cee_fd_attach(t); /* last: it may ready the task at once */
#endif
    }
}

/* Wakes a task of this or another thread's executor: safe from any thread while the task waits (it cannot end before
 * it is woken). */
#if !defined(CEE_FREESTANDING)
static uint64_t cee_wake_sequence; /* ceesafe_atomic_*: orders wakes, so woken tasks run first come, first served */
#endif

static void cee_wake_task(cee_executor *exec, cee_task *t) {
#if defined(CEE_FREESTANDING)
    uint64_t sequence = ceesafe_atomic_add_u64(&exec->wake_sequence, 1u) + 1u;
#else
    uint64_t sequence = ceesafe_atomic_add_u64(&cee_wake_sequence, 1u) + 1u;
#endif
    uint64_t none = 0u;
    (void)ceesafe_atomic_cas_u64(&t->woken, none, sequence);
    cee_poke(exec);
}

static void cee_poke(cee_executor *exec) {
#if defined(CEE_FREESTANDING)
    ceesafe_env_wake(exec);
#elif defined(CEE_COSMOOS)
    cosmo_mutex_lock(&exec->wake_lock);
    exec->wake_pending = 1u;
    cosmo_cond_signal(&exec->wake_cond);
    cosmo_mutex_unlock(&exec->wake_lock);
#elif defined(_WIN32)
    if (exec->port != NULL) {
        (void)PostQueuedCompletionStatus(exec->port, 0, 0, NULL); /* ends its wait in the port */
    } else if (exec->wake_event != NULL) {
        (void)SetEvent(exec->wake_event);
    }
#else
    if (exec->wake_pipe[1] >= 0) {
        unsigned char byte = 1u;
        ssize_t written;
        do {
            written = write(exec->wake_pipe[1], &byte, 1u);
        } while (written < 0 && errno == EINTR); /* EAGAIN: the pipe is full, so a wake is already pending */
    }
#endif
}

static void cee_unpark(cee_task *t) {
    if (t->parked) {
        t->parked = false;
        if (t->prev != NULL) {
            t->prev->next = t->next;
        } else {
            cee_exec.parked = t->next;
        }
        if (t->next != NULL) {
            t->next->prev = t->prev;
        }
        t->prev = NULL;
        if (t->heap_index != 0u) {
            cee_heap_remove(t);
        }
        if (t->fd >= 0) {
            cee_exec.fd_count -= 1u;
#if defined(_WIN32)
            if (t->fd < CEE_SOCKET_BASE) {
                cee_exec.file_count -= 1u; /* a socket stays selected until the run ends: another task may wait on it */
            }
#elif !defined(CEE_FREESTANDING) && !defined(CEE_NO_REACTOR)
            cee_fd_detach(t);
#endif
        }
        if (t->on_wake) {
            if (t->link_prev != NULL) {
                t->link_prev->link_next = t->link_next;
            } else {
                cee_exec.wake_list = t->link_next;
            }
            if (t->link_next != NULL) {
                t->link_next->link_prev = t->link_prev;
            }
            t->link_next = NULL;
            t->link_prev = NULL;
        }
    }
    t->deadline = UINT64_MAX;
    t->fd = -1;
    t->on_scope = false;
    if (t->on_op) {
        t->on_op = false;
        t->channel_wait = false;
        cee_exec.op_count -= 1u;
    }
    if (t->on_wake) {
        t->on_wake = false;
        t->channel_wait = false;
        cee_exec.wake_count -= 1u;
    }
    cee_ready(t);
}

uint64_t ceesafe_async_now(void) {
#if defined(CEE_FREESTANDING)
    return ceesafe_env_now_ms();
#elif defined(_WIN32)
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

#if !defined(CEE_FREESTANDING)
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
#elif defined(CEE_COSMOOS)
    /* No readiness on CosmoOS (ADR 0040): every descriptor is ready in the directions asked; with none, the timeout
       is slept. */
    (void)fds;
    int64_t n = 0;
    for (uint64_t k = 0; k < count; k++) {
        ready[k] = events[k] & 3u;
        n += ready[k] != 0u;
    }
    if (n == 0 && timeout > 0) {
        struct timespec pause = {(time_t)(timeout / 1000), (long)(timeout % 1000) * 1000000L};
        (void)nanosleep(&pause, NULL);
    }
    return n;
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
#elif defined(CEE_COSMOOS)
    /* No readiness on CosmoOS (ADR 0040): the write is a step of at most PIPE_BUF bytes, which may block a moment. */
    uint64_t step = length < PIPE_BUF ? length : PIPE_BUF;
    ssize_t written;
    do {
        written = write(fd, bytes, (size_t)step);
    } while (written < 0 && errno == EINTR);
    return written < 0 ? -(int64_t)errno : (int64_t)written;
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
#endif /* the console and descriptors */

static void cee_task_init(cee_task *t, void *frame, ceesafe_scope *scope, const ceesafe_frame_info *info) {
    ceesafe_mem_set(t, 0, sizeof *t);
    t->frame = frame;
    t->scope = scope;
    t->deadline = UINT64_MAX;
    t->shield_deadline = UINT64_MAX;
    t->fd = -1;
    t->info = info;
}

static void *cee_spawn(ceesafe_scope *scope, size_t frame_size, bool (*resume)(void *frame, ceesafe_scope *scope),
                       void *(*allocate)(size_t size, size_t align), void (*release)(void *p, size_t size, size_t align),
                       const ceesafe_frame_info *info, bool send) {
    if (scope->owner != cee_exec.current) {
        scope->owner = cee_exec.current; /* before its first child, which other workers may read it from */
    }
    if (scope->oom) {
        return NULL; /* the scope already failed to start a child: it starts no more */
    }
    size_t block_size = CEE_TASK_HEAD + frame_size;
#if defined(CEE_FREESTANDING)
    if (allocate == NULL) {
        cee_out_of_memory(); /* no system heap: the checker allows only Async.scopeIn here (ADR 0039) */
    }
    unsigned char *block = (unsigned char *)allocate(block_size, _Alignof(cee_max_align_t));
#else
    unsigned char *block = allocate != NULL ? (unsigned char *)allocate(block_size, _Alignof(cee_max_align_t))
                                            : (unsigned char *)ceesafe_alloc(block_size, CEE_ALLOC_SYSTEM);
#endif
    if (block == NULL) {
        scope->oom = true;
        cee_cancel_scope(scope); /* the children already started stop (type-system.md §40.5) */
        return NULL;
    }
    cee_task *t = (cee_task *)(void *)block;
    cee_task_init(t, block + CEE_TASK_HEAD, scope, info);
    t->resume.child = resume;
    t->release = release;
    t->block_size = block_size;
    if (send && !scope->shared) {
        scope->shared = true; /* before its first child can run elsewhere: its state changes under the lock from now */
    }
    cee_scope_lock(scope);
    scope->live += 1u;
    cee_scope_unlock(scope);
    if (send) {
        t->unpublished = true; /* queued by ceesafe_async_publish once its frame is initialised */
    } else {
        cee_ready(t);
    }
    return t->frame;
}

void *ceesafe_async_spawn(ceesafe_scope *scope, size_t frame_size, bool (*resume)(void *frame, ceesafe_scope *scope),
                          void *(*allocate)(size_t size, size_t align), void (*release)(void *p, size_t size, size_t align),
                          const ceesafe_frame_info *info) {
    return cee_spawn(scope, frame_size, resume, allocate, release, info, false);
}

#if !defined(CEE_FREESTANDING)
void *ceesafe_async_spawn_send(ceesafe_scope *scope, size_t frame_size, bool (*resume)(void *frame, ceesafe_scope *scope),
                               void *(*allocate)(size_t size, size_t align),
                               void (*release)(void *p, size_t size, size_t align), const ceesafe_frame_info *info) {
    bool workers = cee_exec.run != NULL && cee_exec.run->workers > 1u;
    return cee_spawn(scope, frame_size, resume, allocate, release, info, workers); /* one worker: spawn */
}

void ceesafe_async_publish(void *frame) {
    cee_task *t = (cee_task *)(void *)((unsigned char *)frame - CEE_TASK_HEAD);
    if (!t->unpublished) {
        return; /* spawned on this worker: already ready */
    }
    t->unpublished = false;
    cee_run *run = cee_exec.run;
    ceesafe_mutex_lock(run->lock);
    t->next = NULL;
    if (run->queue_tail != NULL) {
        run->queue_tail->next = t;
    } else {
        run->queue_head = t;
    }
    run->queue_tail = t;
    (void)ceesafe_atomic_add_u32(&run->queued, 1u);
    ceesafe_mutex_unlock(run->lock);
    cee_run_poke(run); /* an idle worker takes it; this one does too, when it has nothing ready */
}
#endif /* work stealing */

/* ---- AsyncSignal (ADR 0039): raised from anywhere, waited on by one task. ----
 * raise sets `raised`, then takes the waiting task (swapping `task` to 0) and wakes it, `busy` while it does, so
 * that a task which lost the race to take itself back waits until the raise no longer touches it before it returns
 * (its frame, and the task, may end once it does). */
typedef struct {
    uint64_t raised;
    uint64_t task;
    uint64_t executor;
    uint64_t busy;
} cee_signal;

/* Waits until no raise is waking this task: a few instructions, on another processor or in an interrupt. */
static void cee_signal_settle(cee_signal *s) {
    while (ceesafe_atomic_load_u64(&s->busy) != 0u) {
    }
}

bool ceesafe_async_signal_take(void *signal) {
    cee_signal *s = (cee_signal *)signal;
    if (ceesafe_atomic_swap_u64(&s->raised, 0u) != 0u) {
        return true;
    }
    cee_task *t = cee_exec.current;
    uint64_t me = (uint64_t)(uintptr_t)t;
    cee_open_wake_pipe(); /* a raise only pokes: it never touches the queues, as it may be an interrupt's */
    ceesafe_atomic_store_u64(&s->executor, (uint64_t)(uintptr_t)&cee_exec);
    ceesafe_atomic_store_u64(&s->task, me);
    if (ceesafe_atomic_swap_u64(&s->raised, 0u) != 0u) {
        if (ceesafe_atomic_swap_u64(&s->task, 0u) == me) {
            return true; /* raised meanwhile, and no raise holds this task */
        }
        cee_signal_settle(s); /* a raise holds it and wakes it: run again, then take */
        ceesafe_atomic_store_u64(&s->raised, 1u); /* the raise this took, for that take */
    }
    if (!t->on_wake) {
        t->on_wake = true;
        t->channel_wait = true; /* a cancellation wakes it too (type-system.md §40.7) */
        cee_exec.wake_count += 1u;
    }
    return false;
}

void ceesafe_async_signal_forget(void *signal) {
    cee_signal *s = (cee_signal *)signal;
    uint64_t me = (uint64_t)(uintptr_t)cee_exec.current;
    if (ceesafe_atomic_cas_u64(&s->task, me, 0u) != me) {
        cee_signal_settle(s); /* a raise took it (and stays raised), or it was not waiting */
    }
}

void ceesafe_async_signal_raise(void *signal) {
    cee_signal *s = (cee_signal *)signal;
    ceesafe_atomic_store_u64(&s->busy, 1u);
    ceesafe_atomic_store_u64(&s->raised, 1u);
    cee_task *t = (cee_task *)(uintptr_t)ceesafe_atomic_swap_u64(&s->task, 0u);
    if (t != NULL) {
        cee_wake_task((cee_executor *)(uintptr_t)ceesafe_atomic_load_u64(&s->executor), t);
    }
    ceesafe_atomic_store_u64(&s->busy, 0u);
}

bool ceesafe_async_claim_failure(ceesafe_scope *scope) {
    cee_scope_lock(scope);
    bool first = !scope->failed;
    scope->failed = true;
    cee_scope_unlock(scope);
    return first;
}

void ceesafe_async_start(ceesafe_scope *scope, ceesafe_task_slot *slot, void *frame,
                         bool (*resume)(void *frame, ceesafe_scope *scope), const ceesafe_frame_info *info) {
    cee_task *t = (cee_task *)(void *)slot;
    scope->owner = cee_exec.current;
    cee_task_init(t, frame, scope, info);
    t->resume.child = resume;
    t->embedded = true;
    scope->live += 1u;
    cee_ready(t);
}

void ceesafe_async_cancel(ceesafe_scope *scope) {
    if (ceesafe_atomic_load_u32(&scope->cancelled) == 0u) {
        cee_cancel_scope(scope);
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
        bool cancellable = t->on_op ? t->channel_wait : (!t->on_wake || t->channel_wait);
        /* Only a task the cancellation reaches (bug B5): any other would retry its wait and park again, a system call
           and a reactor change each, for every cancellation, so a server with n idle connections paid O(n) per race. */
        if (!t->on_scope && cancellable && cee_cancelled(t)) { /* a worker's wait, an operation being cancelled, not */
#if !defined(CEE_FREESTANDING)
            (void)ceesafe_atomic_add_ord_u64(&cee_cancel_woken, 1u, 0);
#endif
            cee_unpark(t);
            /* Unparking can ready others: kqueue's change queue, when full, is flushed and readies what is ready
               (bug B4). If that took `next` off the parked list, its links are the ready list's now: start again
               from the head. Every restart follows an unpark, and nothing parks during the walk, so it ends. */
            if (next != NULL && !next->parked) {
                next = cee_exec.parked;
            }
        }
        t = next;
    }
}

static void cee_open_wake_pipe(void) {
#if defined(CEE_FREESTANDING)
    /* the environment's ceesafe_env_wake needs nothing opened */
#elif defined(CEE_COSMOOS)
    /* the executor's wake_lock and wake_cond are ready from cee_exec_begin */
#elif defined(_WIN32)
    if (cee_exec.wake_event == NULL) {
        cee_exec.wake_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    }
#else
    if (cee_exec.wake_pipe[0] < 0) {
        int fds[2];
        if (pipe(fds) == 0) {
            /* Non-blocking: a full pipe already says "wake up", and a busy worker drains it only when it waits. */
            (void)fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
            (void)fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL) | O_NONBLOCK);
            cee_exec.wake_pipe[0] = fds[0];
            cee_exec.wake_pipe[1] = fds[1];
        }
    }
#endif
}

#if !defined(CEE_FREESTANDING)
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
#endif /* Async.blocking */

bool ceesafe_async_scope_done(ceesafe_scope *scope) {
#if !defined(CEE_FREESTANDING)
    if (scope->shared) {
        /* Its last child may return on another worker, which wakes this task through its executor (ADR 0038). */
        ceesafe_mutex_lock(cee_exec.run->lock);
        bool done = scope->live == 0u;
        if (!done) {
            scope->waiter = cee_exec.current;
            cee_exec.current->on_wake = true; /* not cancellable: on_wake without channel_wait */
            cee_exec.wake_count += 1u;
        }
        ceesafe_mutex_unlock(cee_exec.run->lock);
        return done;
    }
#endif
    if (scope->live == 0u) {
        return true;
    }
    scope->waiter = cee_exec.current;
    cee_exec.current->on_scope = true;
    return false;
}

static _Noreturn void cee_stuck(void) {
#if defined(CEE_FREESTANDING)
    ceesafe_panic(CEE_PANIC_ASYNC, CEE_LOC("ceesafe_rt_async.c", 0u, 0u));
#else
    static const char message[] = "ceesafe: every task is parked with nothing to wake it\n";
    ceesafe_platform_write_stderr((const uint8_t *)message, sizeof message - 1u);
    ceesafe_platform_abort();
#endif
}

#if !defined(CEE_FREESTANDING)
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

#if defined(_WIN32)
/* Windows: consoles and files count as ready; sockets signal the executor's event (WSAEventSelect), and WSAPoll
   says which are ready. True if a task was readied. */
static bool cee_iocp_drain(int timeout);

static bool cee_wait_windows(int timeout) {
    bool any = false;
    if (cee_exec.port != NULL) {
        /* IOCP (ADR 0035): completions and other threads' wakes come to the thread's port. Consoles and files count
           as ready; a socket tied to another thread's port waits by readiness, polled between slices of 1 ms. */
        if (cee_exec.file_count > 0u) {
            for (cee_task *t = cee_exec.parked, *next = NULL; t != NULL; t = next) {
                next = t->next;
                if (t->fd >= 0 && t->fd < CEE_SOCKET_BASE) {
                    cee_unpark(t);
                    any = true;
                }
            }
        }
        bool sockets = cee_exec.fd_count > cee_exec.file_count;
        if (sockets && cee_poll_sockets() > 0) {
            any = true;
        }
        int slice = any ? 0 : timeout;
        if (sockets && (slice < 0 || slice > 1)) {
            slice = 1;
        }
        if (cee_iocp_drain(slice)) {
            any = true;
        }
        if (sockets && cee_poll_sockets() > 0) {
            any = true;
        }
        return any;
    }
    /* A console or file descriptor has no readiness here: its task retries at once (see ceesafe_async_write). */
    if (cee_exec.file_count > 0u) {
        for (cee_task *t = cee_exec.parked, *next = NULL; t != NULL; t = next) {
            next = t->next;
            if (t->fd >= 0 && t->fd < CEE_SOCKET_BASE) {
                cee_unpark(t);
                any = true;
            }
        }
    }
    bool sockets = cee_exec.fd_count > cee_exec.file_count;
    if (sockets && cee_exec.wake_event != NULL) {
        /* Each socket was selected for the executor's event when its task parked; WSAPoll says which are ready. A
           socket that becomes ready between the poll and the wait signals the event, so no readiness is lost. */
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
    return any;
}
#elif defined(CEE_COSMOOS)
/* CosmoOS userland (ADR 0040): descriptors have no readiness, so a task parked on one is ready now, as consoles and
   files are on Windows; with none, the executor waits on its condition variable until the earliest deadline
   (`timeout` ms, -1 for none) or a wake. The woken tasks and the due deadlines are cee_wait's to ready. */
static bool cee_wait_cosmoos(int timeout) {
    bool any = false;
    if (cee_exec.fd_count > 0u) {
        for (cee_task *t = cee_exec.parked, *next = NULL; t != NULL; t = next) {
            next = t->next;
            if (t->fd >= 0) {
                cee_unpark(t);
                any = true;
            }
        }
        return any;
    }
    cosmo_mutex_lock(&cee_exec.wake_lock);
    if (cee_exec.wake_pending == 0u && timeout != 0) {
        if (timeout < 0) {
            cosmo_cond_wait(&cee_exec.wake_cond, &cee_exec.wake_lock);
        } else {
            (void)cosmo_cond_timedwait(&cee_exec.wake_cond, &cee_exec.wake_lock, (unsigned long long)timeout * 1000000ull);
        }
    }
    cee_exec.wake_pending = 0u;
    cosmo_mutex_unlock(&cee_exec.wake_lock);
    return any;
}
#else
/* poll(): one entry per descriptor a task waits on, plus the wake pipe while a task waits to be woken. */
static bool cee_wait_poll(int timeout) {
    struct pollfd small[65];
    struct pollfd *fds = small;
    size_t needed = (size_t)cee_exec.active_count + 1u;
    if (needed > sizeof small / sizeof small[0]) {
        fds = (struct pollfd *)ceesafe_platform_alloc(needed * sizeof *fds);
        if (fds == NULL) {
            cee_out_of_memory();
        }
    }
    nfds_t count = 0;
    bool pipe = cee_exec.wake_count > 0u && cee_exec.wake_pipe[0] >= 0;
    if (pipe) {
        fds[0].fd = cee_exec.wake_pipe[0]; /* another thread's wake */
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        count = 1u;
    }
    for (uint32_t k = 0; k < cee_exec.active_count; k++) {
        int32_t fd = cee_exec.active_fds[k];
        const cee_fd_slot *slot = &cee_exec.slots[fd];
        fds[count].fd = fd;
        fds[count].events = (short)((slot->head[0] != NULL ? POLLIN : 0) | (slot->head[1] != NULL ? POLLOUT : 0));
        fds[count].revents = 0;
        count += 1u;
    }
    int ready = poll(count > 0u ? fds : NULL, count, timeout);
    bool any = false;
    for (nfds_t k = 0; ready > 0 && k < count; k++) {
        short r = fds[k].revents;
        if (r == 0) {
            continue;
        }
        if (pipe && k == 0u) {
            unsigned char drained[64];
            cee_drain(cee_exec.wake_pipe[0], drained, sizeof drained);
            continue;
        }
        if ((r & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0 && cee_exec.slots[fds[k].fd].head[0] != NULL) {
            cee_fd_ready(fds[k].fd, 0u);
            any = true;
        }
        if ((r & (POLLOUT | POLLHUP | POLLERR | POLLNVAL)) != 0 && cee_exec.slots[fds[k].fd].head[1] != NULL) {
            cee_fd_ready(fds[k].fd, 1u);
            any = true;
        }
    }
    if (fds != small) {
        ceesafe_platform_free(fds);
    }
    return any;
}

/* kqueue or epoll: the descriptors were armed when their tasks parked, and the wake pipe stays registered. */
static bool cee_wait_reactor(int timeout) {
    if (cee_exec.wake_pipe[0] >= 0 && !cee_exec.pipe_watched) {
#if defined(CEE_KQUEUE)
        cee_kqueue_change(cee_exec.wake_pipe[0], EVFILT_READ, EV_ADD); /* level-triggered, kept */
        cee_exec.pipe_watched = true;
#elif defined(CEE_EPOLL)
        struct epoll_event change;
        ceesafe_mem_set(&change, 0, sizeof change);
        change.events = EPOLLIN;
        change.data.fd = cee_exec.wake_pipe[0];
        cee_exec.pipe_watched = epoll_ctl(cee_exec.reactor, EPOLL_CTL_ADD, cee_exec.wake_pipe[0], &change) == 0;
#endif
    }
    bool any = false;
#if defined(CEE_KQUEUE)
    struct timespec limit = {timeout / 1000, (long)(timeout % 1000) * 1000000L};
    any = cee_kqueue_call(timeout < 0 ? NULL : &limit);
#elif defined(CEE_EPOLL)
    struct epoll_event events[64];
    int n = epoll_wait(cee_exec.reactor, events, 64, timeout);
    for (int k = 0; k < n; k++) {
        int32_t fd = events[k].data.fd;
        if (fd == cee_exec.wake_pipe[0]) {
            unsigned char drained[64];
            cee_drain(fd, drained, sizeof drained);
            continue;
        }
#if defined(CEE_URING)
        if (cee_exec.uring != NULL && fd == cee_uring_eventfd()) {
            if (cee_uring_drain()) {
                any = true; /* completions: their tasks are readied */
            }
            continue;
        }
#endif
        if ((uint32_t)fd >= cee_exec.slot_count) {
            continue;
        }
        uint32_t r = events[k].events;
        cee_exec.slots[fd].armed = 0u; /* one-shot: the descriptor is disabled until armed again */
        if ((r & (uint32_t)(EPOLLIN | EPOLLHUP | EPOLLERR)) != 0u && cee_exec.slots[fd].head[0] != NULL) {
            cee_fd_ready(fd, 0u);
            any = true;
        }
        if ((r & (uint32_t)(EPOLLOUT | EPOLLHUP | EPOLLERR)) != 0u && cee_exec.slots[fd].head[1] != NULL) {
            cee_fd_ready(fd, 1u);
            any = true;
        }
        if (cee_exec.slots[fd].head[0] != NULL || cee_exec.slots[fd].head[1] != NULL) {
            cee_reactor_arm(fd); /* the direction that did not fire still waits */
        }
    }
#else
    (void)timeout;
#endif
    return any;
}
#endif
#endif /* the platforms' waits */

/* While no task can run: waits for the earliest deadline, a descriptor or a wake, then readies what is due. Nothing
   here walks the parked list: deadlines are a heap, descriptors a table, wakes a list. */
#if defined(CEE_FREESTANDING)
/* Freestanding (ADR 0039): the earliest deadline among the parked tasks, then the environment's idle until it or a
   wake; then woken tasks, earliest wake first, and those whose time has come. */
static void cee_wait(void) {
    for (;;) {
        uint64_t deadline = UINT64_MAX;
        for (cee_task *t = cee_exec.parked; t != NULL; t = t->next) {
            uint64_t key = cee_task_key(t);
            if (key < deadline) {
                deadline = key;
            }
        }
        if (deadline == UINT64_MAX && cee_exec.wake_count == 0u) {
            cee_stuck();
        }
        bool any = false;
        for (;;) {
            cee_task *first = NULL;
            uint64_t first_at = UINT64_MAX;
            for (cee_task *w = cee_exec.wake_list; w != NULL; w = w->link_next) {
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
        uint64_t now = ceesafe_async_now();
        for (cee_task *t = cee_exec.parked, *next = NULL; t != NULL; t = next) {
            next = t->next;
            if (cee_task_key(t) <= now) {
                cee_unpark(t);
                any = true;
            }
        }
        if (any) {
            return;
        }
        ceesafe_env_idle(&cee_exec, deadline);
    }
}
#else
static void cee_wait(void) {
    for (;;) {
        if (cee_exec.heap_count == 0u && cee_exec.fd_count == 0u && cee_exec.wake_count == 0u && cee_exec.op_count == 0u) {
            cee_stuck();
        }
        uint64_t now = ceesafe_async_now();
        int timeout = -1;
        if (cee_exec.heap_count > 0u) {
            uint64_t deadline = cee_task_key(cee_exec.heap[0]);
            timeout = now >= deadline ? 0 : (deadline - now > (uint64_t)INT_MAX ? INT_MAX : (int)(deadline - now));
        }
        bool any = false;
#if defined(_WIN32)
        any = cee_wait_windows(timeout);
#elif defined(CEE_COSMOOS)
        any = cee_wait_cosmoos(timeout);
#else
        if (cee_exec.wake_count > 0u || cee_exec.fd_count > 0u || cee_exec.op_count > 0u || timeout != 0) {
            any = cee_reactor_open() ? cee_wait_reactor(timeout) : cee_wait_poll(timeout);
        }
#endif
        /* Woken tasks, earliest wake first. */
        for (;;) {
            cee_task *first = NULL;
            uint64_t first_at = UINT64_MAX;
            for (cee_task *w = cee_exec.wake_list; w != NULL; w = w->link_next) {
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
        /* The tasks whose time has come run earliest deadline first, however late the executor woke. */
        now = ceesafe_async_now();
        while (cee_exec.heap_count > 0u && cee_task_key(cee_exec.heap[0]) <= now) {
            cee_unpark(cee_exec.heap[0]);
            any = true;
        }
        if (cee_exec.run != NULL) {
            /* The run's events (ADR 0038): a shared scope cancelled, work to take, or the run's end. */
            uint64_t epoch = ceesafe_atomic_load_u64(&cee_exec.run->cancel_epoch);
            if (epoch != cee_exec.cancel_seen) {
                cee_exec.cancel_seen = epoch;
                cee_wake_waiters();
                any = true;
            }
            if (ceesafe_atomic_load_u32(&cee_exec.run->queued) > 0u || ceesafe_atomic_load_u32(&cee_exec.run->done) != 0u) {
                return;
            }
        }
        if (any) {
            return;
        }
    }
}
#endif

#if defined(CEE_FREESTANDING)
static void cee_exec_begin(cee_run *run) {
    (void)run; /* one worker */
    ceesafe_mem_set(&cee_exec, 0, sizeof cee_exec);
    cee_exec.wake_pipe[0] = -1;
    cee_exec.wake_pipe[1] = -1;
    cee_exec.reactor = -1;
}

static void cee_exec_end(void) {
}
#else
/* A fresh executor for this thread, a worker of `run` (NULL for a run with one worker). */
static void cee_exec_begin(cee_run *run) {
    cee_exec.ready_head = NULL;
    cee_exec.ready_tail = NULL;
    cee_exec.parked = NULL;
    cee_exec.current = NULL;
    cee_exec.wake_pipe[0] = -1;
    cee_exec.wake_pipe[1] = -1;
#if defined(_WIN32)
    cee_exec.wake_event = NULL;
    cee_exec.port = NULL;
#endif
#if defined(CEE_COSMOOS)
    cee_exec.wake_lock = (cosmo_mutex_t)COSMO_MUTEX_INIT;
    cee_exec.wake_cond = (cosmo_cond_t)COSMO_COND_INIT;
    cee_exec.wake_pending = 0u;
#endif
    cee_exec.wake_count = 0u;
    cee_exec.fd_count = 0u;
    cee_exec.file_count = 0u;
    cee_exec.op_count = 0u;
    cee_exec.wake_list = NULL;
    cee_exec.heap = NULL;
    cee_exec.heap_count = 0u;
    cee_exec.heap_capacity = 0u;
    cee_exec.slots = NULL;
    cee_exec.slot_count = 0u;
    cee_exec.active_fds = NULL;
    cee_exec.active_count = 0u;
    cee_exec.active_capacity = 0u;
    cee_exec.reactor = -1;
    cee_exec.use_poll = ceesafe_platform_config("CEESAFE_REACTOR_POLL") != 0u;
#if defined(_WIN32)
    if (!cee_exec.use_poll) {
        cee_exec.port = cee_iocp_port(); /* IOCP by default on Windows (ADR 0035) */
    }
#endif
    cee_exec.pipe_watched = false;
#if defined(CEE_KQUEUE)
    cee_exec.change_count = 0;
#endif
#if defined(CEE_URING)
    cee_exec.uring = NULL;
    cee_exec.want_uring = !cee_exec.use_poll && ceesafe_platform_config("CEESAFE_REACTOR_URING") != 0u;
#endif
    cee_exec.run = run;
    cee_exec.cancel_seen = 0u;
    if (run != NULL) {
        cee_open_wake_pipe();
        cee_exec.wake_count += 1u; /* the run's own wakes: new work, a cancellation, its end */
        cee_exec.cancel_seen = ceesafe_atomic_load_u64(&run->cancel_epoch);
        ceesafe_mutex_lock(run->lock);
        run->executors[run->executor_count] = &cee_exec;
        run->executor_count += 1u;
        ceesafe_mutex_unlock(run->lock);
    }
}

static void cee_exec_end(void) {
    if (cee_exec.run != NULL) {
        cee_run *run = cee_exec.run;
        ceesafe_mutex_lock(run->lock); /* before its wake pipe closes: no worker pokes it after this */
        for (uint32_t k = 0; k < run->executor_count; k++) {
            if (run->executors[k] == &cee_exec) {
                run->executors[k] = run->executors[run->executor_count - 1u];
                run->executor_count -= 1u;
                break;
            }
        }
        ceesafe_mutex_unlock(run->lock);
    }
#if defined(_WIN32)
    if (cee_exec.wake_event != NULL) {
        ceesafe_socket_unselect(cee_exec.wake_event); /* before the handle can name anything else */
        (void)CloseHandle(cee_exec.wake_event);
    }
#else
    if (cee_exec.wake_pipe[0] >= 0) {
        (void)close(cee_exec.wake_pipe[0]);
        (void)close(cee_exec.wake_pipe[1]);
    }
#if defined(CEE_URING)
    cee_uring_close();
#endif
    if (cee_exec.reactor >= 0) {
        (void)close(cee_exec.reactor);
    }
#endif
    ceesafe_platform_free(cee_exec.heap);
    ceesafe_platform_free(cee_exec.slots);
    ceesafe_platform_free(cee_exec.active_fds);
}
#endif

/* A child that returned: its block freed, its scope told; the scope's first error cancels its siblings, and its last
   child wakes the task waiting for it, on whichever worker that task runs (ADR 0038). */
static void cee_child_done(cee_task *t) {
    ceesafe_scope *scope = t->scope;
    if (!t->embedded) {
        if (t->release != NULL) {
            t->release(t, t->block_size, _Alignof(cee_max_align_t));
        } else {
#if !defined(CEE_FREESTANDING)
            ceesafe_free(t, CEE_ALLOC_SYSTEM); /* freestanding: every child is from Async.scopeIn's allocator */
#endif
        }
    }
    cee_scope_lock(scope);
    scope->live -= 1u;
    bool cancel = scope->failed && ceesafe_atomic_load_u32(&scope->cancelled) == 0u;
    cee_task *waiter = NULL;
    if (scope->live == 0u && scope->waiter != NULL) {
        waiter = (cee_task *)scope->waiter;
        scope->waiter = NULL;
    }
    cee_scope_unlock(scope);
    if (cancel) {
        cee_cancel_scope(scope); /* the first error cancels the other children (type-system.md §40.5) */
    }
    if (waiter != NULL) {
        if (waiter->home == (void *)&cee_exec) {
            cee_unpark(waiter);
        } else {
            cee_wake_task((cee_executor *)waiter->home, waiter);
        }
    }
}

/* Runs tasks until `root` returns, or, for a worker (root NULL), until the run is done. */
static void cee_exec_loop(cee_task *root, uint64_t cancel_at) {
    for (;;) {
        cee_task *t = cee_take_ready();
        if (t == NULL) {
            t = cee_steal();
        }
        if (t == NULL) {
            if (root == NULL && ceesafe_atomic_load_u32(&cee_exec.run->done) != 0u && cee_exec.parked == NULL) {
                break; /* the root returned, so every child has: nothing is left here */
            }
            cee_wait();
            continue;
        }
        t->home = &cee_exec;
        cee_exec.current = t;
        bool done = t->scope == NULL ? t->resume.root(t->frame) : t->resume.child(t->frame, t->scope);
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
            if (t->deadline == UINT64_MAX && t->fd < 0 && !t->on_scope && !t->on_wake && !t->on_op && !(t->shields > 0u)) {
                cee_stuck(); /* parked without registering anything: nothing can wake it */
            }
            cee_park(t);
#if defined(CEE_FREESTANDING)
            (void)cancel_at; /* the cancellation sweep is the hosted runtime's (testing.md §1) */
#else
            if (ceesafe_atomic_add_ord_u64(&cee_suspensions, 1u, 0) + 1u == cancel_at && cee_cancel_root != NULL) {
                ceesafe_atomic_store_u32(&cee_cancel_root->cancelled, 1u); /* the sweep's cancellation (testing.md §1) */
                cee_wake_waiters();
                if (cee_exec.run != NULL) {
                    (void)ceesafe_atomic_add_u64(&cee_exec.run->cancel_epoch, 1u);
                    cee_run_poke(cee_exec.run);
                }
            }
#endif
            continue;
        }
        if (t == root) {
            break; /* its scopes have ended, so every child has returned */
        }
        cee_child_done(t);
    }
}

#if defined(CEE_FREESTANDING)
/* Freestanding (ADR 0039): one worker; the executor lives in this frame, named by the environment's slot. */
void ceesafe_async_run(void *frame, bool (*resume)(void *), const ceesafe_frame_info *info) {
    cee_executor executor;
    void **slot = ceesafe_env_slot();
    void *saved = *slot;
    *slot = &executor;
    cee_exec_begin(NULL);
    cee_task root;
    cee_task_init(&root, frame, NULL, info);
    root.resume.root = resume;
    cee_exec.root = &root;
    cee_ready(&root);
    cee_exec_loop(&root, 0u);
    cee_exec_end();
    *slot = saved;
}
#else
typedef struct {
    cee_run *run;
    cee_task *root;
    uint64_t cancel_at;
} cee_worker_start;

static void *cee_worker(void *arg) {
    cee_worker_start *start = (cee_worker_start *)arg;
    cee_executor saved = cee_exec;
    cee_task *saved_root = cee_cancel_root;
    cee_cancel_root = start->root; /* the sweep may count its point on any worker */
    cee_exec_begin(start->run);
    cee_exec_loop(NULL, start->cancel_at);
    cee_exec_end();
    cee_cancel_root = saved_root;
    cee_exec = saved;
    return NULL;
}

void ceesafe_async_run(void *frame, bool (*resume)(void *), const ceesafe_frame_info *info) {
    uint32_t workers = cee_next_workers;
    cee_next_workers = 0u;
    if (workers > CEE_MAX_WORKERS) {
        workers = CEE_MAX_WORKERS;
    }
    cee_executor saved = cee_exec;
    cee_task *saved_root = cee_cancel_root;
    cee_run run;
    cee_run *shared = NULL;
    if (workers > 1u) {
        ceesafe_mem_set(&run, 0, sizeof run);
        run.lock = ceesafe_mutex_create();
        run.workers = workers;
        if (run.lock != NULL) {
            shared = &run; /* without a lock, one worker: the result is the same (ADR 0038 §3.3) */
        }
    }
    cee_exec_begin(shared);
    uint64_t cancel_at = ceesafe_test_reports ? ceesafe_test_cancel_at : ceesafe_platform_config("CEESAFE_CANCEL_AT");
    bool cancel_report = ceesafe_test_reports || ceesafe_platform_config("CEESAFE_CANCEL_REPORT") != 0u;
    cee_task root;
    cee_task_init(&root, frame, NULL, info);
    root.resume.root = resume;
    cee_cancel_root = &root;
    cee_thread *threads[CEE_MAX_WORKERS];
    cee_worker_start start = {shared, &root, cancel_at};
    uint32_t started = 0u;
    if (shared != NULL) {
        for (uint32_t k = 1u; k < workers; k++) {
            threads[started] = ceesafe_thread_start(cee_worker, &start);
            if (threads[started] != NULL) {
                started += 1u; /* a worker that cannot start leaves fewer: the result is the same */
            }
        }
    }
    cee_ready(&root);
    cee_exec_loop(&root, cancel_at);
    if (shared != NULL) {
        ceesafe_atomic_store_u32(&run.done, 1u);
        cee_run_poke(&run);
        for (uint32_t k = 0; k < started; k++) {
            ceesafe_thread_join(threads[k]);
        }
    }
    cee_exec_end();
    if (shared != NULL) {
        ceesafe_mutex_destroy(run.lock);
    }
    if (cancel_report) {
        char line[96];
        int length = snprintf(line, sizeof line, "ceesafe cancel: points=%llu woken=%llu\n",
                              (unsigned long long)ceesafe_atomic_load_ord_u64(&cee_suspensions, 0),
                              (unsigned long long)ceesafe_atomic_load_ord_u64(&cee_cancel_woken, 0));
        ceesafe_platform_write_stderr((const uint8_t *)line, (size_t)length);
    }
    cee_cancel_root = saved_root;
    cee_exec = saved;
}
#endif

#if !defined(CEE_FREESTANDING)
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
#elif defined(CEE_COSMOOS)
    cosmo_mutex_t m; /* ADR 0040 */
    cosmo_cond_t cv;
#else
    pthread_mutex_t m;
    pthread_cond_t cv;
#endif
    unsigned char *data;
} cee_chan;

static void cee_chan_lock(cee_chan *c) {
    while (ceesafe_atomic_swap_ord_u32(&c->lock, 1u, 1) != 0u) {
        while (ceesafe_atomic_load_ord_u32(&c->lock, 0) != 0u) {
#if !defined(_WIN32) && !defined(CEE_COSMOOS)
            sched_yield(); /* CosmoOS has none in its libc: the loop retries (ADR 0040) */
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
#elif defined(CEE_COSMOOS)
    c->m = (cosmo_mutex_t)COSMO_MUTEX_INIT;
    c->cv = (cosmo_cond_t)COSMO_COND_INIT;
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
#if !defined(_WIN32) && !defined(CEE_COSMOOS)
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
#elif defined(CEE_COSMOOS)
        cosmo_mutex_lock(&c->m);
        cosmo_cond_broadcast(&c->cv);
        cosmo_mutex_unlock(&c->m);
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
#elif defined(CEE_COSMOOS)
    cosmo_mutex_lock(&c->m);
    while (ceesafe_atomic_load_u64(&c->epoch) == epoch) {
        cosmo_cond_wait(&c->cv, &c->m);
    }
    cosmo_mutex_unlock(&c->m);
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
        shown[count++] = (cee_shown_task){t, t->on_scope ? "waiting for its scope" : t->on_op ? "waiting for an operation" : t->on_wake ? "waiting to be woken" :
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

/* ---- Operations (ADR 0035): lib/std's AsyncOp record, and the steps of each backend. ---- */

/* The record. On a readiness backend it holds only a connecting socket between a connect's steps; on IOCP, the
   operation in flight, with the OVERLAPPED the kernel completes. */
typedef struct {
    uint32_t backend;    /* 0: holds nothing; 1: a readiness connect in progress (fd); 2: an IOCP operation */
    uint32_t state;      /* IOCP: 0 idle, 1 in flight, 2 complete (result) */
    int32_t fd;          /* a readiness connect's socket */
    uint32_t cancelling; /* IOCP: CancelIoEx was asked; the task waits uncancellable for the completion */
    int64_t result;
    cee_task *task;      /* IOCP: the task waiting for the completion */
#if defined(_WIN32)
    uint32_t kind;       /* 1 read, 2 send, 3 accept, 4 connect, 5 vectored send (ADR 0045), 6 file (ADR 0051) */
    SOCKET socket;       /* the socket the operation is on */
    SOCKET accepted;     /* AcceptEx's and ConnectEx's new socket, until it has an id */
    SOCKET listener;
    OVERLAPPED overlapped;
    union {
        struct {
            WSABUF buffer; /* a read's or a send's */
            DWORD flags;
        } io;
        uint8_t addresses[2 * (sizeof(struct sockaddr_in6) + 16)]; /* AcceptEx's, for either family (ADR 0044) */
        HANDLE file; /* TransmitFile's own handle to the file (ADR 0051), closed at the completion */
    } u;
#endif
} cee_op;

_Static_assert(sizeof(cee_op) <= sizeof(ceesafe_async_op), "ceesafe_async_op holds an operation");

#if defined(_WIN32) || defined(CEE_URING)
/* The current task waits for `o`'s completion (IOCP, io_uring): readied by it, not by a wake, and cancellable until
   it asks the kernel to cancel. */
static void cee_op_wait(cee_op *o) {
    cee_task *t = cee_exec.current;
    o->task = t;
    if (t != NULL) {
        if (!t->on_op) {
            t->on_op = true;
            cee_exec.op_count += 1u;
        }
        t->channel_wait = o->cancelling == 0u;
    }
}

/* A step of a submitted operation: its result once complete, else still pending. */
static int64_t cee_op_poll(cee_op *o) {
    if (o->state == 2u) {
        o->state = 0u;
        o->backend = 0u;
        o->cancelling = 0u;
        return o->result;
    }
    cee_op_wait(o);
    return CEE_ASYNC_PENDING;
}

/* A completion: the operation's result, and its task readied. */
static void cee_op_done(cee_op *o, int64_t result) {
    o->result = result;
    o->state = 2u;
    if (o->task != NULL && o->task->parked) {
        cee_unpark(o->task);
    }
}
#endif

static int64_t cee_ready_read(int32_t fd, uint8_t *buffer, uint64_t length) {
    int64_t r = ceesafe_async_read(fd, buffer, length);
    if (r == CEE_ASYNC_WOULD_BLOCK) {
        ceesafe_async_wait_readable(fd);
        return CEE_ASYNC_PENDING;
    }
    return r;
}

static int64_t cee_ready_send(int32_t fd, const uint8_t *bytes, uint64_t length) {
    int64_t r = ceesafe_async_send(fd, bytes, length);
    if (r == CEE_ASYNC_WOULD_BLOCK) {
        ceesafe_async_wait_writable(fd);
        return CEE_ASYNC_PENDING;
    }
    return r;
}

static int64_t cee_ready_accept(int32_t fd) {
    int64_t r = ceesafe_async_tcp_accept(fd);
    if (r == CEE_ASYNC_WOULD_BLOCK) {
        ceesafe_async_wait_readable(fd);
        return CEE_ASYNC_PENDING;
    }
    return r;
}

static int64_t cee_ready_connect(cee_op *o, const ceesafe_endpoint *e) {
    if (o->backend != 1u) {
        int64_t fd = ceesafe_async_tcp_connect_endpoint(e);
        if (fd < 0) {
            return fd;
        }
        o->backend = 1u;
        o->fd = (int32_t)fd;
    }
    int64_t done = ceesafe_async_tcp_connected(o->fd);
    if (done == CEE_ASYNC_WOULD_BLOCK) {
        ceesafe_async_wait_writable(o->fd);
        return CEE_ASYNC_PENDING;
    }
    int32_t fd = o->fd;
    o->backend = 0u;
    if (done != 0) {
        ceesafe_async_close(fd);
        return done;
    }
    return fd; /* the stream's from now on */
}

#if defined(_WIN32)
#include <mswsock.h>
#include <io.h> /* _get_osfhandle, for TransmitFile (ADR 0051) */

/* The thread's completion port, for the life of the thread: a socket is tied to one port for ever. */
static _Thread_local HANDLE cee_thread_port;

static HANDLE cee_iocp_port(void) {
    if (cee_thread_port == NULL) {
        cee_thread_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    }
    return cee_thread_port;
}

/* The socket of id `fd`, tied to the executor's port, or INVALID_SOCKET (then readiness steps are used). */
static SOCKET cee_iocp_socket(int32_t fd) {
    if (cee_exec.port == NULL || fd < CEE_SOCKET_BASE || ceesafe_socket_associate(fd, cee_exec.port) != 1) {
        return INVALID_SOCKET;
    }
    return (SOCKET)ceesafe_socket_handle(fd);
}

static int64_t cee_wsa_failure(void) { return -(int64_t)ceesafe_socket_errno(WSAGetLastError()); }

/* After an operation was submitted: in flight (the kernel completes it to the port), or failed at once. */
static int64_t cee_iocp_submitted(cee_op *o, int status, bool failed) {
    if (failed && WSAGetLastError() != WSA_IO_PENDING) {
        (void)status;
        int64_t r = cee_wsa_failure();
        if (o->accepted != INVALID_SOCKET) {
            (void)closesocket(o->accepted);
            o->accepted = INVALID_SOCKET;
        }
        o->backend = 0u;
        return r;
    }
    o->state = 1u;
    cee_op_wait(o);
    return CEE_ASYNC_PENDING;
}

static void cee_iocp_begin(cee_op *o, uint32_t kind, SOCKET s) {
    ceesafe_mem_set(&o->overlapped, 0, sizeof o->overlapped);
    o->backend = 2u;
    o->state = 0u;
    o->cancelling = 0u;
    o->kind = kind;
    o->socket = s;
    o->accepted = INVALID_SOCKET;
    o->u.io.flags = 0;
}

static LPFN_ACCEPTEX cee_accept_ex;
static LPFN_CONNECTEX cee_connect_ex;

static void *cee_extension(SOCKET s, GUID id) {
    void *function = NULL;
    DWORD bytes = 0;
    if (WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &id, sizeof id, &function, sizeof function, &bytes, NULL,
                 NULL) != 0) {
        return NULL;
    }
    return function;
}

/* A completion from the port: the operation's result, and its task readied. */
static void cee_msg_received(cee_op *o, DWORD flags, bool truncated);

static void cee_iocp_complete(cee_op *o) {
    DWORD bytes = 0;
    DWORD flags = 0;
    if (o->kind == 7u) {
        /* A datagram's receive (ADR 0050): one longer than the buffer completes with WSAEMSGSIZE, the buffer full. */
        BOOL done = WSAGetOverlappedResult(o->socket, &o->overlapped, &bytes, FALSE, &flags);
        bool truncated = !done && WSAGetLastError() == WSAEMSGSIZE;
        o->result = done || truncated ? (int64_t)bytes : cee_wsa_failure();
        cee_msg_received(o, flags, truncated);
        cee_op_done(o, o->result);
        return;
    }
    if (!WSAGetOverlappedResult(o->socket, &o->overlapped, &bytes, FALSE, &flags)) {
        o->result = cee_wsa_failure();
        if (o->accepted != INVALID_SOCKET) {
            (void)closesocket(o->accepted);
            o->accepted = INVALID_SOCKET;
        }
    } else if (o->kind == 3u || o->kind == 4u) {
        SOCKET made = o->accepted;
        o->accepted = INVALID_SOCKET;
        if (o->kind == 3u) {
            (void)setsockopt(made, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (const char *)&o->listener, sizeof o->listener);
        } else {
            (void)setsockopt(made, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0);
        }
        u_long on = 1;
        (void)ioctlsocket(made, FIONBIO, &on);
        /* ConnectEx's socket was tied to the port before it connected; AcceptEx's is not tied yet. */
        int32_t id = ceesafe_socket_adopt((uintptr_t)made, o->kind == 4u ? cee_exec.port : NULL);
        o->result = id < 0 ? -(int64_t)ENOMEM : (int64_t)id;
    } else {
        o->result = (int64_t)bytes;
    }
    if (o->kind == 6u) {
        (void)CloseHandle(o->u.file);
    }
    cee_op_done(o, o->result);
}

/* Waits up to `timeout` ms for completions and takes every one there is. True if a task was readied. */
static bool cee_iocp_drain(int timeout) {
    OVERLAPPED_ENTRY entries[64];
    ULONG removed = 0;
    bool any = false;
    DWORD limit = timeout < 0 ? INFINITE : (DWORD)timeout;
    while (GetQueuedCompletionStatusEx(cee_exec.port, entries, 64, &removed, limit, FALSE) && removed > 0u) {
        for (ULONG k = 0; k < removed; k++) {
            if (entries[k].lpOverlapped == NULL) {
                continue; /* another thread's wake: the woken tasks are found by their flag */
            }
            cee_op *o = (cee_op *)(void *)((char *)entries[k].lpOverlapped - offsetof(cee_op, overlapped));
            cee_iocp_complete(o);
            any = true;
        }
        limit = 0; /* then whatever else is already there */
    }
    return any;
}
#endif

#if defined(CEE_URING)
/* ---- io_uring (ADR 0035; opt-in, CEESAFE_REACTOR_URING=1): socket operations submitted to a ring set up with the raw
   system calls (no liburing). Completions signal an eventfd in the epoll set, so the executor waits in epoll as
   always and drains the ring when it fires. Deadlines, consoles and the wake pipe stay epoll's. ---- */

struct cee_uring {
    int fd;
    int event;          /* the eventfd the ring signals */
    unsigned *sq_head;
    unsigned *sq_tail;
    unsigned *sq_mask;
    unsigned *sq_array;
    unsigned *sq_flags; /* IORING_SQ_CQ_OVERFLOW: completions wait in the kernel until an enter flushes them */
    struct io_uring_sqe *sqes;
    unsigned *cq_head;
    unsigned *cq_tail;
    unsigned *cq_mask;
    struct io_uring_cqe *cqes;
    void *sq_ring;
    void *cq_ring;
    size_t sq_size;
    size_t cq_size;
    size_t sqes_size;
    unsigned entries;
};

#define CEE_URING_CANCEL_TAG 1u /* the user data of a cancellation's own completion: an op's address | 1 */

static void cee_uring_close(void) {
    struct cee_uring *u = cee_exec.uring;
    if (u == NULL) {
        return;
    }
    if (u->sqes != NULL) {
        (void)munmap(u->sqes, u->sqes_size);
    }
    if (u->cq_ring != NULL && u->cq_ring != u->sq_ring) {
        (void)munmap(u->cq_ring, u->cq_size);
    }
    if (u->sq_ring != NULL) {
        (void)munmap(u->sq_ring, u->sq_size);
    }
    if (u->event >= 0) {
        (void)close(u->event);
    }
    (void)close(u->fd);
    ceesafe_platform_free(u);
    cee_exec.uring = NULL;
}

static int cee_uring_eventfd(void) { return cee_exec.uring != NULL ? cee_exec.uring->event : -1; }

/* Sets the ring up at the first operation; false (and epoll stays) if the kernel refuses it. */
static bool cee_uring_open(void) {
    if (cee_exec.uring != NULL) {
        return true;
    }
    if (!cee_exec.want_uring || !cee_reactor_open()) {
        return false;
    }
    cee_exec.want_uring = false; /* one try */
    struct io_uring_params p;
    ceesafe_mem_set(&p, 0, sizeof p);
    p.flags = IORING_SETUP_CQSIZE;
    p.cq_entries = 16384u; /* room for every waiting operation's completion, and a cancellation's of each */
    int fd = (int)syscall(__NR_io_uring_setup, 256u, &p);
    if (fd < 0) {
        return false;
    }
    struct cee_uring *u = (struct cee_uring *)ceesafe_platform_alloc(sizeof *u);
    if (u == NULL) {
        (void)close(fd);
        return false;
    }
    ceesafe_mem_set(u, 0, sizeof *u);
    u->fd = fd;
    u->event = -1;
    u->entries = p.sq_entries;
    u->sq_size = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    u->cq_size = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
    bool single = (p.features & IORING_FEAT_SINGLE_MMAP) != 0u;
    if (single && u->cq_size > u->sq_size) {
        u->sq_size = u->cq_size;
    }
    u->sq_ring = mmap(NULL, u->sq_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
    u->cq_ring = single ? u->sq_ring
                        : mmap(NULL, u->cq_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_CQ_RING);
    u->sqes_size = p.sq_entries * sizeof(struct io_uring_sqe);
    void *sqes = mmap(NULL, u->sqes_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES);
    cee_exec.uring = u;
    if (u->sq_ring == MAP_FAILED || u->cq_ring == MAP_FAILED || sqes == MAP_FAILED) {
        if (u->sq_ring == MAP_FAILED) u->sq_ring = NULL;
        if (u->cq_ring == MAP_FAILED) u->cq_ring = NULL;
        u->sqes = sqes == MAP_FAILED ? NULL : (struct io_uring_sqe *)sqes;
        cee_uring_close();
        return false;
    }
    u->sqes = (struct io_uring_sqe *)sqes;
    char *sq = (char *)u->sq_ring;
    char *cq = (char *)u->cq_ring;
    u->sq_head = (unsigned *)(void *)(sq + p.sq_off.head);
    u->sq_tail = (unsigned *)(void *)(sq + p.sq_off.tail);
    u->sq_mask = (unsigned *)(void *)(sq + p.sq_off.ring_mask);
    u->sq_array = (unsigned *)(void *)(sq + p.sq_off.array);
    u->sq_flags = (unsigned *)(void *)(sq + p.sq_off.flags);
    u->cq_head = (unsigned *)(void *)(cq + p.cq_off.head);
    u->cq_tail = (unsigned *)(void *)(cq + p.cq_off.tail);
    u->cq_mask = (unsigned *)(void *)(cq + p.cq_off.ring_mask);
    u->cqes = (struct io_uring_cqe *)(void *)(cq + p.cq_off.cqes);
    u->event = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    struct epoll_event change;
    ceesafe_mem_set(&change, 0, sizeof change);
    change.events = EPOLLIN;
    change.data.fd = u->event;
    if (u->event < 0 || syscall(__NR_io_uring_register, fd, IORING_REGISTER_EVENTFD, &u->event, 1) < 0 ||
        epoll_ctl(cee_exec.reactor, EPOLL_CTL_ADD, u->event, &change) < 0) {
        cee_uring_close();
        return false;
    }
    return true;
}

/* A submission entry, zeroed; submitted (with every one before it) by cee_uring_submit. NULL if the ring is full. */
static struct io_uring_sqe *cee_uring_entry(void) {
    struct cee_uring *u = cee_exec.uring;
    unsigned tail = *u->sq_tail;
    unsigned head = __atomic_load_n(u->sq_head, __ATOMIC_ACQUIRE);
    if (tail - head >= u->entries) {
        return NULL;
    }
    unsigned index = tail & *u->sq_mask;
    struct io_uring_sqe *e = &u->sqes[index];
    ceesafe_mem_set(e, 0, sizeof *e);
    u->sq_array[index] = index;
    __atomic_store_n(u->sq_tail, tail + 1u, __ATOMIC_RELEASE);
    return e;
}

static bool cee_uring_drain(void);

/* Submits the entry just made. EBUSY means completions overflowed in the kernel: they are taken (flushed by the
   drain), then the submission is tried again, so a cancellation is never dropped. */
static bool cee_uring_submit(void) {
    for (int attempt = 0; attempt < 64; attempt++) {
        long r = syscall(__NR_io_uring_enter, cee_exec.uring->fd, 1u, 0u, 0u, NULL, 0);
        if (r >= 0) {
            return true;
        }
        if (errno != EBUSY && errno != EINTR) {
            return false;
        }
        (void)cee_uring_drain();
    }
    return false;
}

/* Takes every completion there is. True if a task was readied. */
static bool cee_uring_drain(void) {
    struct cee_uring *u = cee_exec.uring;
    uint64_t counter = 0;
    cee_drain(u->event, &counter, sizeof counter);
    bool any = false;
    unsigned head = *u->cq_head;
    for (;;) {
        unsigned tail = __atomic_load_n(u->cq_tail, __ATOMIC_ACQUIRE);
        if (head == tail) {
            break;
        }
        struct io_uring_cqe *e = &u->cqes[head & *u->cq_mask];
        uintptr_t data = (uintptr_t)e->user_data;
        int32_t res = e->res;
        head += 1u;
        if ((data & CEE_URING_CANCEL_TAG) != 0u) {
            continue; /* a cancellation's own completion: the operation's comes by itself */
        }
        cee_op *o = (cee_op *)data;
        int64_t result = res;
        if (o->fd >= 0 && o->state == 1u && o->backend == 3u && o->result == 4) {
            /* a connect: its socket is the stream's once connected */
            if (res == 0) {
                result = o->fd;
            } else {
                (void)close(o->fd);
            }
            o->fd = -1;
        }
        cee_op_done(o, result);
        any = true;
        if (head == __atomic_load_n(u->cq_tail, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(u->cq_head, head, __ATOMIC_RELEASE);
            if ((__atomic_load_n(u->sq_flags, __ATOMIC_ACQUIRE) & IORING_SQ_CQ_OVERFLOW) != 0u) {
                /* the kernel kept completions back: an enter with GETEVENTS moves them into the ring */
                (void)syscall(__NR_io_uring_enter, u->fd, 0u, 0u, IORING_ENTER_GETEVENTS, NULL, 0);
            }
        }
    }
    __atomic_store_n(u->cq_head, head, __ATOMIC_RELEASE);
    return any;
}

/* Submits `e` for `o` (kind: 1 read, 2 send, 3 accept, 4 connect); pending, or the error if it could not be. */
static int64_t cee_uring_go(cee_op *o, struct io_uring_sqe *e) {
    e->user_data = (uint64_t)(uintptr_t)o;
    o->state = 1u;
    if (!cee_uring_submit()) {
        o->state = 0u;
        o->backend = 0u;
        return -(int64_t)errno;
    }
    cee_op_wait(o);
    return CEE_ASYNC_PENDING;
}

static void cee_uring_begin(cee_op *o, uint32_t kind) {
    o->backend = 3u;
    o->state = 0u;
    o->cancelling = 0u;
    o->result = kind; /* the kind, until the completion (cee_uring_drain reads it for a connect) */
    o->fd = -1;
}

static int64_t cee_uring_read(cee_op *o, int32_t fd, uint8_t *buffer, uint64_t length) {
    struct io_uring_sqe *e = cee_uring_entry();
    if (e == NULL) {
        return CEE_ASYNC_WOULD_BLOCK; /* the ring is full: a readiness step instead */
    }
    cee_uring_begin(o, 1u);
    e->opcode = IORING_OP_RECV;
    e->fd = fd;
    e->addr = (uint64_t)(uintptr_t)buffer;
    e->len = length > (1u << 30) ? 1u << 30 : (unsigned)length;
    return cee_uring_go(o, e);
}

static int64_t cee_uring_send(cee_op *o, int32_t fd, const uint8_t *bytes, uint64_t length) {
    struct io_uring_sqe *e = cee_uring_entry();
    if (e == NULL) {
        return CEE_ASYNC_WOULD_BLOCK;
    }
    cee_uring_begin(o, 2u);
    e->opcode = IORING_OP_SEND;
    e->fd = fd;
    e->addr = (uint64_t)(uintptr_t)bytes;
    e->len = length > (1u << 30) ? 1u << 30 : (unsigned)length;
    e->msg_flags = MSG_NOSIGNAL;
    return cee_uring_go(o, e);
}

static int64_t cee_uring_accept(cee_op *o, int32_t fd) {
    struct io_uring_sqe *e = cee_uring_entry();
    if (e == NULL) {
        return CEE_ASYNC_WOULD_BLOCK;
    }
    cee_uring_begin(o, 3u);
    e->opcode = IORING_OP_ACCEPT;
    e->fd = fd;
    e->accept_flags = SOCK_NONBLOCK | SOCK_CLOEXEC;
    return cee_uring_go(o, e);
}

/* The connect's address, in the record after its fields (the kernel reads it while the operation is in flight). */
typedef struct {
    cee_op op;
    union {
        struct sockaddr_in v4;
        struct sockaddr_in6 v6;
    } address;
} cee_uring_connect_op;

_Static_assert(sizeof(cee_uring_connect_op) <= sizeof(ceesafe_async_op), "ceesafe_async_op holds a connect");

static int64_t cee_uring_connect(cee_op *o, const ceesafe_endpoint *endpoint) {
    cee_uring_connect_op *c = (cee_uring_connect_op *)(void *)o;
    uint32_t length = ceesafe_endpoint_sockaddr(endpoint, &c->address, (uint32_t)sizeof c->address);
    if (length == 0) {
        return -(int64_t)EAFNOSUPPORT;
    }
    int fd = socket(endpoint->family == 6u ? AF_INET6 : AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -(int64_t)errno;
    }
    struct io_uring_sqe *e = cee_uring_entry();
    if (e == NULL) {
        (void)close(fd);
        return CEE_ASYNC_WOULD_BLOCK;
    }
    cee_uring_begin(o, 4u);
    o->fd = fd;
    e->opcode = IORING_OP_CONNECT;
    e->fd = fd;
    e->addr = (uint64_t)(uintptr_t)&c->address;
    e->off = length;
    return cee_uring_go(o, e);
}

static void cee_uring_cancel(cee_op *o) {
    struct io_uring_sqe *e = cee_uring_entry();
    if (e == NULL) {
        return; /* the ring is full: the operation completes by itself (it waits for its socket) */
    }
    e->opcode = IORING_OP_ASYNC_CANCEL;
    e->addr = (uint64_t)(uintptr_t)o;
    e->user_data = (uint64_t)(uintptr_t)o | CEE_URING_CANCEL_TAG;
    (void)cee_uring_submit();
}
#endif

int64_t ceesafe_async_op_read(void *op, int32_t fd, uint8_t *buffer, uint64_t length) {
    cee_op *o = (cee_op *)op;
#if defined(_WIN32)
    if (o->backend == 2u) {
        return cee_op_poll(o);
    }
    SOCKET s = cee_iocp_socket(fd);
    if (s != INVALID_SOCKET) {
        cee_iocp_begin(o, 1u, s);
        o->u.io.buffer.buf = (char *)buffer;
        o->u.io.buffer.len = length > (1u << 30) ? (ULONG)1u << 30 : (ULONG)length;
        int status = WSARecv(s, &o->u.io.buffer, 1, NULL, &o->u.io.flags, &o->overlapped, NULL);
        return cee_iocp_submitted(o, status, status != 0);
    }
#endif
#if defined(CEE_URING)
    if (o->backend == 3u) {
        return cee_op_poll(o);
    }
    if (cee_uring_open()) {
        int64_t r = cee_uring_read(o, fd, buffer, length);
        if (r != CEE_ASYNC_WOULD_BLOCK) {
            return r;
        }
    }
#endif
    (void)o;
    return cee_ready_read(fd, buffer, length);
}

int64_t ceesafe_async_op_send(void *op, int32_t fd, const uint8_t *bytes, uint64_t length) {
    cee_op *o = (cee_op *)op;
#if defined(_WIN32)
    if (o->backend == 2u) {
        return cee_op_poll(o);
    }
    SOCKET s = cee_iocp_socket(fd);
    if (s != INVALID_SOCKET) {
        cee_iocp_begin(o, 2u, s);
        o->u.io.buffer.buf = (char *)(uintptr_t)bytes;
        o->u.io.buffer.len = length > (1u << 30) ? (ULONG)1u << 30 : (ULONG)length;
        int status = WSASend(s, &o->u.io.buffer, 1, NULL, 0, &o->overlapped, NULL);
        return cee_iocp_submitted(o, status, status != 0);
    }
#endif
#if defined(CEE_URING)
    if (o->backend == 3u) {
        return cee_op_poll(o);
    }
    if (cee_uring_open()) {
        int64_t r = cee_uring_send(o, fd, bytes, length);
        if (r != CEE_ASYNC_WOULD_BLOCK) {
            return r;
        }
    }
#endif
    (void)o;
    return cee_ready_send(fd, bytes, length);
}

int64_t ceesafe_async_op_accept(void *op, int32_t fd) {
    cee_op *o = (cee_op *)op;
#if defined(_WIN32)
    if (o->backend == 2u) {
        return cee_op_poll(o);
    }
    SOCKET s = cee_iocp_socket(fd);
    if (s != INVALID_SOCKET) {
        if (cee_accept_ex == NULL) {
            GUID id = WSAID_ACCEPTEX;
            cee_accept_ex = (LPFN_ACCEPTEX)cee_extension(s, id);
        }
        if (cee_accept_ex != NULL) {
            cee_iocp_begin(o, 3u, s);
            o->listener = s;
            /* The new socket has the listener's family (ADR 0044). */
            struct sockaddr_in6 own;
            int ownLength = (int)sizeof own;
            int family = getsockname(s, (struct sockaddr *)&own, &ownLength) == 0 ? own.sin6_family : AF_INET;
            o->accepted = WSASocketW(family, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
            if (o->accepted == INVALID_SOCKET) {
                o->backend = 0u;
                return cee_wsa_failure();
            }
            DWORD received = 0;
            BOOL done = cee_accept_ex(s, o->accepted, o->u.addresses, 0, sizeof(struct sockaddr_in6) + 16,
                                      sizeof(struct sockaddr_in6) + 16, &received, &o->overlapped);
            return cee_iocp_submitted(o, 0, !done);
        }
    }
#endif
#if defined(CEE_URING)
    if (o->backend == 3u) {
        return cee_op_poll(o);
    }
    if (cee_uring_open()) {
        int64_t r = cee_uring_accept(o, fd);
        if (r != CEE_ASYNC_WOULD_BLOCK) {
            return r;
        }
    }
#endif
    (void)o;
    return cee_ready_accept(fd);
}

/* A connect's step to `e` on the backend in use: ConnectEx (IOCP), IORING_OP_CONNECT, or readiness. Every step gets
   the same endpoint; only the first uses it. */
static int64_t cee_op_connect(cee_op *o, const ceesafe_endpoint *endpoint) {
#if defined(_WIN32)
    if (o->backend == 2u) {
        return cee_op_poll(o);
    }
    if (o->backend == 0u && cee_exec.port != NULL) {
        /* ConnectEx: a new overlapped socket of the endpoint's family, bound to its any-address, tied to the port,
           then connected. */
        union {
            struct sockaddr sa;
            struct sockaddr_in v4;
            struct sockaddr_in6 v6;
        } remote, local;
        int remoteLength = (int)ceesafe_endpoint_sockaddr(endpoint, &remote, (uint32_t)sizeof remote);
        if (remoteLength == 0) {
            return -(int64_t)EAFNOSUPPORT;
        }
        int family = endpoint->family == 6u ? AF_INET6 : AF_INET;
        SOCKET s = WSASocketW(family, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
        if (s == INVALID_SOCKET) {
            return cee_wsa_failure();
        }
        ceesafe_mem_set(&local, 0, sizeof local);
        local.sa.sa_family = (ADDRESS_FAMILY)family; /* the any-address of its family, any port */
        int localLength = family == AF_INET6 ? (int)sizeof local.v6 : (int)sizeof local.v4;
        if (cee_connect_ex == NULL) {
            GUID id = WSAID_CONNECTEX;
            cee_connect_ex = (LPFN_CONNECTEX)cee_extension(s, id);
        }
        if (cee_connect_ex == NULL || bind(s, &local.sa, localLength) != 0 ||
            CreateIoCompletionPort((HANDLE)s, cee_exec.port, 0, 0) == NULL) {
            int64_t failed = cee_wsa_failure();
            (void)closesocket(s);
            return failed;
        }
        cee_iocp_begin(o, 4u, s);
        o->accepted = s; /* the new socket, given an id when it connects */
        BOOL done = cee_connect_ex(s, &remote.sa, remoteLength, NULL, 0, NULL, &o->overlapped);
        return cee_iocp_submitted(o, 0, !done);
    }
#endif
#if defined(CEE_URING)
    if (o->backend == 3u) {
        return cee_op_poll(o);
    }
    if (o->backend == 0u && cee_uring_open()) {
        int64_t r = cee_uring_connect(o, endpoint);
        if (r != CEE_ASYNC_WOULD_BLOCK) {
            return r;
        }
    }
#endif
    return cee_ready_connect(o, endpoint);
}

int64_t ceesafe_async_op_connect(void *op, const uint8_t *address, uint64_t length, uint16_t port) {
    cee_str text = {address, length};
    ceesafe_endpoint e;
    if (!ceesafe_endpoint_parse(text, port, &e)) {
        return -(int64_t)EINVAL;
    }
    return cee_op_connect((cee_op *)op, &e);
}

int64_t ceesafe_async_op_connect_at(void *op, uint32_t family, const uint8_t *bytes, uint16_t port, uint32_t scope) {
    ceesafe_endpoint e;
    if (!ceesafe_endpoint_make(family, bytes, port, scope, &e)) {
        return -(int64_t)EINVAL;
    }
    return cee_op_connect((cee_op *)op, &e);
}

bool ceesafe_async_op_cancel(void *op) {
    cee_op *o = (cee_op *)op;
#if defined(_WIN32)
    if (o->backend == 2u && o->state == 1u) {
        /* In flight: the kernel stops it, and its completion (aborted, or done meanwhile) still comes. Until then the
           task waits uncancellable, so its frame and the buffer outlive the kernel's use (type-system.md §40.7). */
        o->cancelling = 1u;
        (void)CancelIoEx((HANDLE)o->socket, &o->overlapped);
        if (cee_exec.current != NULL) {
            cee_exec.current->channel_wait = false;
        }
        return true;
    }
#endif
#if defined(CEE_URING)
    if (o->backend == 3u && o->state == 1u) {
        o->cancelling = 1u; /* as IOCP's: the completion still comes, and the task waits for it */
        cee_uring_cancel(o);
        if (cee_exec.current != NULL) {
            cee_exec.current->channel_wait = false;
        }
        return true;
    }
#endif
    if (o->backend == 1u) {
        ceesafe_async_close(o->fd); /* a connection abandoned half made */
        o->backend = 0u;
    }
    return false;
}

/* ---- Message operations (ADR 0045): lib/std's AsyncMsgOp record, a vectored send's steps. ---- */

/* A part as the kernel reads it. */
#if defined(CEE_COSMOOS)
typedef struct {
    const uint8_t *base;
    uint64_t len;
} cee_msg_buffer;
#elif defined(_WIN32)
typedef WSABUF cee_msg_buffer;
#else
typedef struct iovec cee_msg_buffer;
#endif

/* Room for a datagram's ancillary data (U6): packet info, traffic class and segment size on every platform. */
#if defined(_WIN32)
#define CEE_MSG_CONTROL 72u /* a packet info and an ECN value: the record's room after the datagram fields */
#else
#define CEE_MSG_CONTROL 128u
#endif

/* The record: an operation's header (so the cancel and completion paths take it), the parts, the message header,
   and, for U6's datagrams, a peer address and the control buffer. The kernel reads the parts, and on io_uring the
   message header, while the operation is in flight, so they are here, in the task's frame. */
typedef struct {
    cee_op op;
    uint32_t count;
    cee_msg_buffer parts[CEE_MSG_PARTS];
#if defined(_WIN32)
    WSAMSG msg;
#elif !defined(CEE_COSMOOS)
    struct msghdr msg;
#endif
    uint32_t name[8]; /* a sockaddr_in6 or sockaddr_in (28 bytes at most) */
    uint64_t control[CEE_MSG_CONTROL / 8u];
    struct {
        uint32_t socket_family; /* a datagram send's (ADR 0050) */
        uint32_t ecn;
        uint32_t segment;
        uint32_t name_length;
        uint32_t control_length;
    } datagram;
} cee_msg_op;

_Static_assert(sizeof(cee_msg_op) <= sizeof(ceesafe_async_msg_op), "ceesafe_async_msg_op holds a message operation");

void ceesafe_async_msg_part(void *op, uint32_t index, const uint8_t *bytes, uint64_t length) {
    cee_msg_op *m = (cee_msg_op *)op;
    if (index >= CEE_MSG_PARTS) {
        return;
    }
    uint64_t n = length > (1u << 30) ? (uint64_t)1u << 30 : length; /* as a send's chunk: a partial count is normal */
#if defined(CEE_COSMOOS)
    m->parts[index].base = bytes;
    m->parts[index].len = n;
#elif defined(_WIN32)
    m->parts[index].buf = (char *)(uintptr_t)bytes;
    m->parts[index].len = (ULONG)n;
#else
    m->parts[index].iov_base = (void *)(uintptr_t)bytes;
    m->parts[index].iov_len = (size_t)n;
#endif
}

/* A readiness step: one vectored send, or the wait for writability. */
static int64_t cee_ready_sendv(cee_msg_op *m, int32_t fd, uint32_t count) {
#if defined(CEE_COSMOOS)
    /* One buffer per call there: the first non-empty part, a legal partial send. */
    for (uint32_t k = 0; k < count; k++) {
        if (m->parts[k].len > 0u) {
            return cee_ready_send(fd, m->parts[k].base, m->parts[k].len);
        }
    }
    return 0;
#elif defined(_WIN32)
    SOCKET s = (SOCKET)ceesafe_socket_handle(fd);
    if (s == INVALID_SOCKET) {
        return -(int64_t)EBADF;
    }
    DWORD sent = 0;
    if (WSASend(s, m->parts, (DWORD)count, &sent, 0, NULL, NULL) != 0) {
        if (WSAGetLastError() == WSAEWOULDBLOCK) {
            ceesafe_async_wait_writable(fd);
            return CEE_ASYNC_PENDING;
        }
        return cee_wsa_failure();
    }
    return (int64_t)sent;
#else
    ssize_t n;
    do {
#if defined(MSG_NOSIGNAL)
        ceesafe_mem_set(&m->msg, 0, sizeof m->msg);
        m->msg.msg_iov = m->parts;
        m->msg.msg_iovlen = (__typeof__(m->msg.msg_iovlen))count; /* int on macOS, size_t on Linux */
        n = sendmsg(fd, &m->msg, MSG_NOSIGNAL);
#else
        n = writev(fd, m->parts, (int)count); /* the socket has SO_NOSIGPIPE */
#endif
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            ceesafe_async_wait_writable(fd);
            return CEE_ASYNC_PENDING;
        }
        return -(int64_t)errno;
    }
    return (int64_t)n;
#endif
}

int64_t ceesafe_async_op_sendv(void *op, int32_t fd, uint32_t count) {
    cee_msg_op *m = (cee_msg_op *)op;
    cee_op *o = &m->op;
    if (count > CEE_MSG_PARTS) {
        count = CEE_MSG_PARTS;
    }
#if defined(_WIN32)
    if (o->backend == 2u) {
        return cee_op_poll(o);
    }
    SOCKET s = cee_iocp_socket(fd);
    if (s != INVALID_SOCKET) {
        cee_iocp_begin(o, 5u, s);
        int status = WSASend(s, m->parts, (DWORD)count, NULL, 0, &o->overlapped, NULL);
        return cee_iocp_submitted(o, status, status != 0);
    }
#endif
#if defined(CEE_URING)
    if (o->backend == 3u) {
        return cee_op_poll(o);
    }
    if (cee_uring_open()) {
        struct io_uring_sqe *e = cee_uring_entry();
        if (e != NULL) {
            ceesafe_mem_set(&m->msg, 0, sizeof m->msg);
            m->msg.msg_iov = m->parts;
            m->msg.msg_iovlen = (__typeof__(m->msg.msg_iovlen))count; /* int on macOS, size_t on Linux */
            cee_uring_begin(o, 5u);
            e->opcode = IORING_OP_SENDMSG;
            e->fd = fd;
            e->addr = (uint64_t)(uintptr_t)&m->msg;
            e->len = 1u;
            e->msg_flags = MSG_NOSIGNAL;
            return cee_uring_go(o, e);
        }
    }
#endif
    (void)o;
    return cee_ready_sendv(m, fd, count);
}

/* ---- Datagrams (ADR 0050): a send with its destination and control messages, a receive with the peer's address and
 * the control messages it brought, on the message record. ---- */

#if defined(_WIN32)
/* ws2def.h's control-message macros, whose TYPE_ALIGNMENT defines a type inside offsetof, which clang warns about:
   headers and data aligned to 8, a pointer's size on the 64-bit targets Windows builds for. */
#define CEE_WSA_ALIGN(n) (((size_t)(n) + 7u) & ~(size_t)7u)
#define CEE_WSA_DATA(c) ((unsigned char *)(c) + CEE_WSA_ALIGN(sizeof(WSACMSGHDR)))
#define CEE_WSA_LEN(n) (CEE_WSA_ALIGN(sizeof(WSACMSGHDR)) + (size_t)(n))
#define CEE_WSA_SPACE(n) (CEE_WSA_ALIGN(sizeof(WSACMSGHDR)) + CEE_WSA_ALIGN(n))

static WSACMSGHDR *cee_wsa_first(WSAMSG *msg) {
    return msg->Control.len >= sizeof(WSACMSGHDR) ? (WSACMSGHDR *)(void *)msg->Control.buf : NULL;
}

static WSACMSGHDR *cee_wsa_next(WSAMSG *msg, WSACMSGHDR *c) {
    unsigned char *next = (unsigned char *)c + CEE_WSA_ALIGN(c->cmsg_len);
    unsigned char *end = (unsigned char *)msg->Control.buf + msg->Control.len;
    return c->cmsg_len < sizeof(WSACMSGHDR) || next + sizeof(WSACMSGHDR) > end ? NULL : (WSACMSGHDR *)(void *)next;
}

static LPFN_WSARECVMSG cee_wsa_recvmsg;

/* The WSARecvMsg extension, found once on the first socket that asks. */
static bool cee_have_recvmsg(SOCKET s) {
    if (cee_wsa_recvmsg == NULL) {
        GUID id = WSAID_WSARECVMSG;
        cee_wsa_recvmsg = (LPFN_WSARECVMSG)cee_extension(s, id);
    }
    return cee_wsa_recvmsg != NULL;
}

static void cee_msg_received(cee_op *o, DWORD flags, bool truncated) {
    cee_msg_op *m = (cee_msg_op *)o;
    m->msg.dwFlags = flags | (truncated ? (DWORD)MSG_TRUNC : 0u);
}
#endif

#if !defined(CEE_COSMOOS)
/* Appends one control message to the record's control buffer; false if it does not fit. */
static bool cee_control_add(cee_msg_op *m, uint32_t *used, int level, int type, const void *data, uint32_t size) {
#if defined(_WIN32)
    uint32_t space = (uint32_t)CEE_WSA_SPACE(size);
    if (*used + space > (uint32_t)sizeof m->control) return false;
    WSACMSGHDR *c = (WSACMSGHDR *)(void *)((uint8_t *)m->control + *used);
    ceesafe_mem_set(c, 0, space);
    c->cmsg_len = CEE_WSA_LEN(size);
    c->cmsg_level = level;
    c->cmsg_type = type;
    memcpy(CEE_WSA_DATA(c), data, size);
#else
    uint32_t space = (uint32_t)CMSG_SPACE(size);
    if (*used + space > (uint32_t)sizeof m->control) return false;
    struct cmsghdr *c = (struct cmsghdr *)(void *)((uint8_t *)m->control + *used);
    ceesafe_mem_set(c, 0, space);
    c->cmsg_len = (__typeof__(c->cmsg_len))CMSG_LEN(size); /* socklen_t on macOS, size_t on Linux */
    c->cmsg_level = level;
    c->cmsg_type = type;
    memcpy(CMSG_DATA(c), data, size);
#endif
    *used += space;
    return true;
}
#endif

void ceesafe_async_msg_send_to(void *op, uint32_t socket_family, uint32_t family, const uint8_t *bytes, uint16_t port,
                               uint32_t scope, uint32_t from_family, const uint8_t *from_bytes, uint32_t ecn,
                               uint32_t segment) {
    cee_msg_op *m = (cee_msg_op *)op;
    m->datagram.socket_family = socket_family;
    m->datagram.ecn = ecn & 3u;
    m->datagram.segment = segment;
    m->datagram.name_length = 0;
    m->datagram.control_length = 0;
    ceesafe_endpoint e;
    if (ceesafe_endpoint_make(family, bytes, port, scope, &e)) {
        m->datagram.name_length = ceesafe_endpoint_sockaddr_for(&e, socket_family, m->name, (uint32_t)sizeof m->name);
    }
#if defined(CEE_COSMOOS)
    (void)from_family;
    (void)from_bytes;
#else
    uint32_t used = 0;
    bool v6 = socket_family == 6u;
    if (from_family != 0u && v6) {
        struct in6_pktinfo info;
        ceesafe_mem_set(&info, 0, sizeof info);
        uint8_t *to = (uint8_t *)&info.ipi6_addr;
        if (from_family == 4u) {
            to[10] = 0xffu;
            to[11] = 0xffu;
            memcpy(to + 12, from_bytes, 4);
        } else {
            memcpy(to, from_bytes, 16);
        }
        (void)cee_control_add(m, &used, IPPROTO_IPV6, IPV6_PKTINFO, &info, (uint32_t)sizeof info);
    } else if (from_family == 4u) {
        struct in_pktinfo info;
        ceesafe_mem_set(&info, 0, sizeof info);
#if defined(_WIN32)
        memcpy(&info.ipi_addr, from_bytes, 4);
#else
        memcpy(&info.ipi_spec_dst, from_bytes, 4); /* the source address to use */
#endif
        (void)cee_control_add(m, &used, IPPROTO_IP, IP_PKTINFO, &info, (uint32_t)sizeof info);
    }
#if defined(__linux__)
    if (m->datagram.ecn != 0u) {
        int value = (int)m->datagram.ecn;
        (void)cee_control_add(m, &used, v6 ? IPPROTO_IPV6 : IPPROTO_IP, v6 ? IPV6_TCLASS : IP_TOS, &value,
                              (uint32_t)sizeof value);
    }
    if (segment > 0u) {
        uint16_t size = (uint16_t)segment;
        (void)cee_control_add(m, &used, SOL_UDP, UDP_SEGMENT, &size, (uint32_t)sizeof size);
    }
#elif defined(_WIN32) && defined(IP_ECN) && defined(IPV6_ECN)
    if (m->datagram.ecn != 0u) {
        INT value = (INT)m->datagram.ecn;
        (void)cee_control_add(m, &used, v6 ? IPPROTO_IPV6 : IPPROTO_IP, v6 ? IPV6_ECN : IP_ECN, &value,
                              (uint32_t)sizeof value);
    }
#endif
    m->datagram.control_length = used;
#endif
}

/* The datagram's length: its parts' total. */
static uint64_t cee_msg_total(const cee_msg_op *m, uint32_t count) {
    uint64_t total = 0;
    for (uint32_t k = 0; k < count; k++) {
#if defined(CEE_COSMOOS)
        total += m->parts[k].len;
#elif defined(_WIN32)
        total += m->parts[k].len;
#else
        total += m->parts[k].iov_len;
#endif
    }
    return total;
}

#if !defined(CEE_COSMOOS)
/* The record's message header for a send, or for a receive into the first part. */
static void cee_msg_header(cee_msg_op *m, uint32_t count, bool receiving) {
    ceesafe_mem_set(&m->msg, 0, sizeof m->msg);
#if defined(_WIN32)
    m->msg.name = (LPSOCKADDR)(void *)m->name;
    m->msg.namelen = receiving ? (INT)sizeof m->name : (INT)m->datagram.name_length;
    m->msg.lpBuffers = m->parts;
    m->msg.dwBufferCount = count;
    uint32_t control = receiving ? (uint32_t)sizeof m->control : m->datagram.control_length;
    if (control > 0u) {
        m->msg.Control.buf = (CHAR *)(void *)m->control;
        m->msg.Control.len = control;
    }
#else
    m->msg.msg_name = m->name;
    m->msg.msg_namelen = receiving ? (socklen_t)sizeof m->name : (socklen_t)m->datagram.name_length;
    m->msg.msg_iov = m->parts;
    m->msg.msg_iovlen = (__typeof__(m->msg.msg_iovlen))count; /* int on macOS, size_t on Linux */
    uint32_t control = receiving ? (uint32_t)sizeof m->control : m->datagram.control_length;
    if (control > 0u) {
        m->msg.msg_control = m->control;
        m->msg.msg_controllen = (__typeof__(m->msg.msg_controllen))control;
    }
#endif
}
#endif

/* A readiness step of a datagram send: one sendmsg, or the wait for writability. */
static int64_t cee_ready_sendmsg(cee_msg_op *m, int32_t fd, uint32_t count) {
#if defined(CEE_COSMOOS)
    (void)m; (void)fd; (void)count;
    return -(int64_t)EOPNOTSUPP;
#elif defined(_WIN32)
    SOCKET s = (SOCKET)ceesafe_socket_handle(fd);
    if (s == INVALID_SOCKET) {
        return -(int64_t)EBADF;
    }
    cee_msg_header(m, count, false);
    DWORD sent = 0;
    if (WSASendMsg(s, &m->msg, 0, &sent, NULL, NULL) != 0) {
        if (WSAGetLastError() == WSAEWOULDBLOCK) {
            ceesafe_async_wait_writable(fd);
            return CEE_ASYNC_PENDING;
        }
        return cee_wsa_failure();
    }
    return (int64_t)sent;
#else
    cee_msg_header(m, count, false);
    bool v6 = m->datagram.socket_family == 6u;
#if !defined(__linux__)
    /* No ECN control message here: the socket's own value, set for this send and cleared after it (ADR 0050 §3.2). */
    if (m->datagram.ecn != 0u) {
        int value = (int)m->datagram.ecn;
        (void)setsockopt(fd, v6 ? IPPROTO_IPV6 : IPPROTO_IP, v6 ? IPV6_TCLASS : IP_TOS, &value, sizeof value);
    }
#endif
    ssize_t n;
    do {
        n = sendmsg(fd, &m->msg, 0);
    } while (n < 0 && errno == EINTR);
    int failed = errno;
#if !defined(__linux__)
    if (m->datagram.ecn != 0u) {
        int zero = 0;
        (void)setsockopt(fd, v6 ? IPPROTO_IPV6 : IPPROTO_IP, v6 ? IPV6_TCLASS : IP_TOS, &zero, sizeof zero);
    }
#else
    (void)v6;
#endif
    if (n < 0) {
        if (failed == EAGAIN || failed == EWOULDBLOCK) {
            ceesafe_async_wait_writable(fd);
            return CEE_ASYNC_PENDING;
        }
        return -(int64_t)failed;
    }
    return (int64_t)n;
#endif
}

int64_t ceesafe_async_op_sendmsg(void *op, int32_t fd, uint32_t count) {
    cee_msg_op *m = (cee_msg_op *)op;
    cee_op *o = &m->op;
    if (count > CEE_MSG_PARTS) {
        count = CEE_MSG_PARTS;
    }
#if defined(_WIN32)
    if (o->backend == 2u) {
        return cee_op_poll(o);
    }
#endif
#if defined(CEE_URING)
    if (o->backend == 3u) {
        return cee_op_poll(o);
    }
#endif
    if (m->datagram.name_length == 0u) {
        return -(int64_t)EINVAL;
    }
    if (count == 0u) {
        /* An empty datagram: one empty part, since macOS refuses a message without any. */
        ceesafe_async_msg_part(op, 0u, (const uint8_t *)m->name, 0u);
        count = 1u;
    }
    if (m->datagram.segment > 0u && cee_msg_total(m, count) > m->datagram.segment) {
#if defined(__linux__)
        /* UDP_SEGMENT: at most 64 segments a call, and the datagram's own limit */
        if ((cee_msg_total(m, count) + m->datagram.segment - 1u) / m->datagram.segment > 64u ||
            cee_msg_total(m, count) > 65000u) {
            return CEE_ASYNC_NO_SEGMENTS;
        }
#else
        return CEE_ASYNC_NO_SEGMENTS; /* lib/std sends the segments one by one */
#endif
    }
#if defined(_WIN32)
    SOCKET s = cee_iocp_socket(fd);
    if (s != INVALID_SOCKET) {
        cee_msg_header(m, count, false);
        cee_iocp_begin(o, 8u, s);
        int status = WSASendMsg(s, &m->msg, 0, NULL, &o->overlapped, NULL);
        return cee_iocp_submitted(o, status, status != 0);
    }
#endif
#if defined(CEE_URING)
    if (cee_uring_open()) {
        struct io_uring_sqe *e = cee_uring_entry();
        if (e != NULL) {
            cee_msg_header(m, count, false);
            cee_uring_begin(o, 8u);
            e->opcode = IORING_OP_SENDMSG;
            e->fd = fd;
            e->addr = (uint64_t)(uintptr_t)&m->msg;
            e->len = 1u;
            return cee_uring_go(o, e);
        }
    }
#endif
    (void)o;
    return cee_ready_sendmsg(m, fd, count);
}

/* A readiness step of a receive: one recvmsg, or the wait for readability. */
static int64_t cee_ready_recvmsg(cee_msg_op *m, int32_t fd) {
#if defined(CEE_COSMOOS)
    (void)m; (void)fd;
    return -(int64_t)EOPNOTSUPP;
#elif defined(_WIN32)
    SOCKET s = (SOCKET)ceesafe_socket_handle(fd);
    if (s == INVALID_SOCKET) {
        return -(int64_t)EBADF;
    }
    if (!cee_have_recvmsg(s)) {
        return -(int64_t)EOPNOTSUPP;
    }
    cee_msg_header(m, 1u, true);
    DWORD got = 0;
    if (cee_wsa_recvmsg(s, &m->msg, &got, NULL, NULL) != 0) {
        int failed = WSAGetLastError();
        if (failed == WSAEWOULDBLOCK) {
            ceesafe_async_wait_readable(fd);
            return CEE_ASYNC_PENDING;
        }
        if (failed != WSAEMSGSIZE) {
            return -(int64_t)ceesafe_socket_errno(failed);
        }
        m->msg.dwFlags |= (DWORD)MSG_TRUNC;
        got = m->parts[0].len; /* the buffer is full, the rest is gone */
    }
    return (int64_t)got;
#else
    cee_msg_header(m, 1u, true);
    ssize_t n;
    do {
        n = recvmsg(fd, &m->msg, 0);
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            ceesafe_async_wait_readable(fd);
            return CEE_ASYNC_PENDING;
        }
        return -(int64_t)errno;
    }
    return (int64_t)n;
#endif
}

int64_t ceesafe_async_op_recvmsg(void *op, int32_t fd, uint8_t *buffer, uint64_t length) {
    cee_msg_op *m = (cee_msg_op *)op;
    cee_op *o = &m->op;
#if defined(_WIN32)
    if (o->backend == 2u) {
        return cee_op_poll(o);
    }
#endif
#if defined(CEE_URING)
    if (o->backend == 3u) {
        return cee_op_poll(o);
    }
#endif
    uint64_t room = length > 65536u ? 65536u : length; /* a datagram is at most 65507 bytes */
    ceesafe_async_msg_part(op, 0u, buffer, room);
#if defined(_WIN32)
    SOCKET s = cee_iocp_socket(fd);
    if (s != INVALID_SOCKET && cee_have_recvmsg(s)) {
        cee_msg_header(m, 1u, true);
        cee_iocp_begin(o, 7u, s);
        int status = cee_wsa_recvmsg(s, &m->msg, NULL, &o->overlapped, NULL);
        if (status != 0 && WSAGetLastError() == WSAEMSGSIZE) {
            status = 0; /* completed at once, truncated: the completion still comes to the port */
        }
        return cee_iocp_submitted(o, status, status != 0);
    }
#endif
#if defined(CEE_URING)
    if (cee_uring_open()) {
        struct io_uring_sqe *e = cee_uring_entry();
        if (e != NULL) {
            cee_msg_header(m, 1u, true);
            cee_uring_begin(o, 7u);
            e->opcode = IORING_OP_RECVMSG;
            e->fd = fd;
            e->addr = (uint64_t)(uintptr_t)&m->msg;
            e->len = 1u;
            return cee_uring_go(o, e);
        }
    }
#endif
    (void)o;
    return cee_ready_recvmsg(m, fd);
}

#if !defined(CEE_COSMOOS)
/* The local address an IPv6 packet info names: IPv4-mapped as IPv4; a link-local address with its interface. */
static int64_t cee_local_v6(const uint8_t *address, uint32_t index, uint8_t *bytes, uint32_t *scope) {
    bool mapped = address[10] == 0xffu && address[11] == 0xffu;
    for (int k = 0; k < 10 && mapped; k++) {
        mapped = address[k] == 0u;
    }
    ceesafe_mem_set(bytes, 0, 16);
    if (mapped) {
        memcpy(bytes, address + 12, 4);
        *scope = 0;
        return (int64_t)4 << 16;
    }
    memcpy(bytes, address, 16);
    *scope = address[0] == 0xfeu && (address[1] & 0xc0u) == 0x80u ? index : 0u;
    return (int64_t)6 << 16;
}
#endif

int64_t ceesafe_async_msg_address(void *op, uint32_t which, uint8_t *bytes, uint32_t *scope) {
    cee_msg_op *m = (cee_msg_op *)op;
    if (which == 0u) {
        ceesafe_endpoint e;
        if (!ceesafe_endpoint_from_sockaddr(m->name, &e)) {
            return -1;
        }
        memcpy(bytes, e.bytes, 16);
        *scope = e.scope;
        return ((int64_t)e.family << 16) | (int64_t)e.port;
    }
#if defined(CEE_COSMOOS)
    (void)bytes;
    (void)scope;
    return -1;
#elif defined(_WIN32)
    if (m->msg.Control.buf == NULL) {
        return -1;
    }
    for (WSACMSGHDR *c = cee_wsa_first(&m->msg); c != NULL; c = cee_wsa_next(&m->msg, c)) {
        if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO) {
            IN_PKTINFO info;
            memcpy(&info, CEE_WSA_DATA(c), sizeof info);
            ceesafe_mem_set(bytes, 0, 16);
            memcpy(bytes, &info.ipi_addr, 4);
            *scope = 0;
            return (int64_t)4 << 16;
        }
        if (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_PKTINFO) {
            IN6_PKTINFO info;
            memcpy(&info, CEE_WSA_DATA(c), sizeof info);
            return cee_local_v6((const uint8_t *)&info.ipi6_addr, (uint32_t)info.ipi6_ifindex, bytes, scope);
        }
    }
    return -1;
#else
    if (m->msg.msg_control == NULL) {
        return -1;
    }
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&m->msg); c != NULL; c = CMSG_NXTHDR(&m->msg, c)) {
#if defined(IP_PKTINFO)
        if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO) {
            struct in_pktinfo info;
            memcpy(&info, CMSG_DATA(c), sizeof info);
            ceesafe_mem_set(bytes, 0, 16);
            memcpy(bytes, &info.ipi_addr, 4); /* the datagram's destination */
            *scope = 0;
            return (int64_t)4 << 16;
        }
#endif
#if defined(IP_RECVDSTADDR)
        if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_RECVDSTADDR) {
            ceesafe_mem_set(bytes, 0, 16);
            memcpy(bytes, CMSG_DATA(c), 4);
            *scope = 0;
            return (int64_t)4 << 16;
        }
#endif
        if (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_PKTINFO) {
            struct in6_pktinfo info;
            memcpy(&info, CMSG_DATA(c), sizeof info);
            return cee_local_v6((const uint8_t *)&info.ipi6_addr, (uint32_t)info.ipi6_ifindex, bytes, scope);
        }
    }
    return -1;
#endif
}

uint32_t ceesafe_async_msg_flags(void *op) {
    cee_msg_op *m = (cee_msg_op *)op;
#if defined(CEE_COSMOOS)
    (void)m;
    return 0;
#elif defined(_WIN32)
    uint32_t flags = (m->msg.dwFlags & (DWORD)MSG_TRUNC) != 0u ? 1u : 0u;
    if (m->msg.Control.buf != NULL) {
        for (WSACMSGHDR *c = cee_wsa_first(&m->msg); c != NULL; c = cee_wsa_next(&m->msg, c)) {
#if defined(IP_ECN) && defined(IPV6_ECN)
            if ((c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_ECN) ||
                (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_ECN)) {
                INT value = 0;
                memcpy(&value, CEE_WSA_DATA(c), sizeof value);
                flags |= ((uint32_t)value & 3u) << 8;
            }
#endif
        }
    }
    return flags;
#else
    uint32_t flags = (m->msg.msg_flags & MSG_TRUNC) != 0 ? 1u : 0u;
    if (m->msg.msg_control != NULL) {
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&m->msg); c != NULL; c = CMSG_NXTHDR(&m->msg, c)) {
            bool tos = c->cmsg_level == IPPROTO_IP && (c->cmsg_type == IP_TOS
#if defined(IP_RECVTOS)
                                                      || c->cmsg_type == IP_RECVTOS
#endif
                                                      );
            if (tos) {
                flags |= ((uint32_t)CMSG_DATA(c)[0] & 3u) << 8; /* one byte */
            }
            if (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_TCLASS) {
                int value = 0;
                memcpy(&value, CMSG_DATA(c), sizeof value);
                flags |= ((uint32_t)value & 3u) << 8;
            }
        }
    }
    return flags;
#endif
}

/* ---- Sending a file (ADR 0051): sendfile, or TransmitFile on IOCP; CEE_ASYNC_NO_SENDFILE where neither applies. ---- */

#if !defined(_WIN32) && !defined(CEE_COSMOOS)
/* A readiness step: one sendfile, or the wait for writability. */
static int64_t cee_ready_sendfile(int32_t socket, int32_t file, uint64_t offset, uint64_t length) {
#if defined(__linux__)
    size_t count = length > 0x7FFFF000u ? (size_t)0x7FFFF000u : (size_t)length; /* Linux's limit for one call */
    off_t at = (off_t)offset;
    ssize_t n;
    do {
        n = sendfile(socket, file, &at, count);
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            ceesafe_async_wait_writable(socket);
            return CEE_ASYNC_PENDING;
        }
        if (errno == EINVAL || errno == ENOSYS) {
            return CEE_ASYNC_NO_SENDFILE; /* not a regular file, or no sendfile for it */
        }
        return -(int64_t)errno;
    }
    return (int64_t)n;
#elif defined(__APPLE__)
    off_t sent = (off_t)(length > ((uint64_t)1 << 40) ? ((uint64_t)1 << 40) : length);
    int r = sendfile(file, socket, (off_t)offset, &sent, NULL, 0);
    if (r < 0) {
        if ((errno == EAGAIN || errno == EINTR) && sent > 0) {
            return (int64_t)sent; /* a partial send: the count is the step's */
        }
        if (errno == EAGAIN || errno == EINTR) {
            ceesafe_async_wait_writable(socket);
            return CEE_ASYNC_PENDING;
        }
        if (errno == ENOTSUP || errno == ENOTSOCK || errno == EINVAL || errno == EOPNOTSUPP) {
            return CEE_ASYNC_NO_SENDFILE;
        }
        return -(int64_t)errno;
    }
    return (int64_t)sent;
#else
    (void)socket; (void)file; (void)offset; (void)length;
    return CEE_ASYNC_NO_SENDFILE;
#endif
}
#endif

#if defined(_WIN32)
static LPFN_TRANSMITFILE cee_transmit_file;
#endif

int64_t ceesafe_async_op_sendfile(void *op, int32_t socket, const void *file, uint64_t offset, uint64_t length) {
    cee_op *o = (cee_op *)op;
    int32_t fd = *(const int32_t *)file;
#if defined(_WIN32)
    if (o->backend == 2u) {
        return cee_op_poll(o);
    }
    SOCKET s = cee_iocp_socket(socket);
    if (s == INVALID_SOCKET) {
        return CEE_ASYNC_NO_SENDFILE; /* WSAPoll: lib/std copies */
    }
    HANDLE base = (HANDLE)(intptr_t)_get_osfhandle(fd);
    if (base == INVALID_HANDLE_VALUE) {
        return -(int64_t)EBADF;
    }
    /* TransmitFile refuses a count past the file's end, so the step stops there; 0 at the end, as sendfile's. */
    LARGE_INTEGER size;
    if (!GetFileSizeEx(base, &size)) {
        return CEE_ASYNC_NO_SENDFILE;
    }
    if (offset >= (uint64_t)size.QuadPart) {
        return 0;
    }
    if (length > (uint64_t)size.QuadPart - offset) {
        length = (uint64_t)size.QuadPart - offset;
    }
    if (cee_transmit_file == NULL) {
        GUID id = WSAID_TRANSMITFILE;
        cee_transmit_file = (LPFN_TRANSMITFILE)cee_extension(s, id);
    }
    if (cee_transmit_file == NULL) {
        return CEE_ASYNC_NO_SENDFILE;
    }
    DWORD count = length > 0x7FFFFFFEu ? (DWORD)0x7FFFFFFEu : (DWORD)length; /* TransmitFile's limit for one call */
    /* A handle of the operation's own: on a synchronous handle TransmitFile moves the file pointer, which the File's
       position must not see, and other streams may send the same file at once. */
    HANDLE h = ReOpenFile(base, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0);
    if (h == INVALID_HANDLE_VALUE) {
        return CEE_ASYNC_NO_SENDFILE;
    }
    cee_iocp_begin(o, 6u, s);
    o->u.file = h;
    o->overlapped.Offset = (DWORD)(offset & 0xFFFFFFFFu);
    o->overlapped.OffsetHigh = (DWORD)(offset >> 32);
    BOOL done = cee_transmit_file(s, h, count, 0, &o->overlapped, NULL, 0);
    if (!done && WSAGetLastError() != WSA_IO_PENDING) {
        (void)CloseHandle(h); /* failed at once: no completion will come */
    }
    return cee_iocp_submitted(o, 0, !done);
#elif defined(CEE_COSMOOS)
    (void)o; (void)socket; (void)fd; (void)offset; (void)length;
    return CEE_ASYNC_NO_SENDFILE;
#else
    (void)o; /* io_uring too: the readiness path's sendfile (ADR 0051 §2) */
    return cee_ready_sendfile(socket, fd, offset, length);
#endif
}
#endif /* !CEE_FREESTANDING: channels, the debugger's tasks, operations */

#if !defined(CEE_FREESTANDING)
#if !defined(CEE_COSMOOS)
/* The current task, marked as waiting to be woken, cancellably (as a channel's wait), and its executor: a lookup's
   waiter, which its resolver thread wakes (ADR 0048). */
static void *cee_dns_wait_task(void **executor) {
    cee_open_wake_pipe();
    cee_task *t = cee_exec.current;
    *executor = &cee_exec;
    if (t != NULL && !t->on_wake) {
        t->on_wake = true;
        t->channel_wait = true;
        cee_exec.wake_count += 1u;
    }
    return t;
}
#endif

/* ---- DNS (ADR 0048): lookups on at most CEE_DNS_THREADS detached resolver threads, the rest queued. A lookup's record
   is platform memory, so an abandoned one its thread still holds is not the program's. Its state, its waiter and the
   queue are under one lock: a thread wakes a task only while it is registered, and ceesafe_dns_end unregisters it. */

#if defined(CEE_COSMOOS)
void *ceesafe_dns_start(const uint8_t *host, uint64_t length, uint16_t port) {
    (void)host; (void)length; (void)port;
    return ceesafe_platform_alloc_zeroed(1u); /* a token: every poll is EOPNOTSUPP */
}
int32_t ceesafe_dns_poll(void *lookup) { (void)lookup; return -(int32_t)EOPNOTSUPP; }
uint32_t ceesafe_dns_count(void *lookup) { (void)lookup; return 0u; }
void ceesafe_dns_address(void *lookup, uint32_t index, uint8_t *bytes, uint32_t *family, uint32_t *scope) {
    (void)lookup; (void)index; (void)bytes; *family = 0u; *scope = 0u;
}
void ceesafe_dns_end(void *lookup) { ceesafe_platform_free(lookup); }
#else
#include <stdlib.h> /* getenv (POSIX), strtoul: the test hook */
#if !defined(_WIN32)
#include <netdb.h>
#endif

typedef struct cee_dns {
    struct cee_dns *next; /* the queue */
    uint32_t state;       /* 0 queued, 1 running, 2 done */
    uint32_t abandoned;   /* running, and its task has left: the thread frees it */
    void *task;           /* the registered waiter, or NULL */
    void *executor;
    int32_t error;        /* 0, or a negated errno */
    uint32_t count;
    ceesafe_endpoint *addresses;
    uint16_t port;
    char host[];          /* NUL-terminated */
} cee_dns;

static cee_dns *cee_dns_head;
static cee_dns *cee_dns_tail;
static uint32_t cee_dns_threads; /* running resolver threads */
#if defined(_WIN32)
static SRWLOCK cee_dns_lock_word = SRWLOCK_INIT;
static void cee_dns_lock(void) { AcquireSRWLockExclusive(&cee_dns_lock_word); }
static void cee_dns_unlock(void) { ReleaseSRWLockExclusive(&cee_dns_lock_word); }
#else
static pthread_mutex_t cee_dns_lock_word = PTHREAD_MUTEX_INITIALIZER;
static void cee_dns_lock(void) { (void)pthread_mutex_lock(&cee_dns_lock_word); }
static void cee_dns_unlock(void) { (void)pthread_mutex_unlock(&cee_dns_lock_word); }
#endif

static void cee_dns_free(cee_dns *d) {
    ceesafe_platform_free(d->addresses);
    ceesafe_platform_free(d);
}

/* The test hook CEESAFE_DNS_DELAY_MS: each lookup waits that long first, so a test can cancel one while it runs. */
static void cee_dns_delay(void) {
#if defined(_WIN32)
    char buffer[32];
    DWORD got = GetEnvironmentVariableA("CEESAFE_DNS_DELAY_MS", buffer, (DWORD)sizeof buffer);
    if (got == 0u || got >= sizeof buffer) {
        return;
    }
    const char *text = buffer;
#else
    const char *text = getenv("CEESAFE_DNS_DELAY_MS");
    if (text == NULL) {
        return;
    }
#endif
    unsigned long ms = strtoul(text, NULL, 10);
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec t = {(time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L};
    while (nanosleep(&t, &t) != 0 && errno == EINTR) {
    }
#endif
}

/* One address of the resolver's into `out`; false for another family. */
static bool cee_dns_endpoint(const struct sockaddr *a, uint16_t port, ceesafe_endpoint *out) {
    ceesafe_mem_set(out, 0, sizeof *out);
    out->port = port;
    if (a->sa_family == AF_INET) {
        const struct sockaddr_in *v4 = (const struct sockaddr_in *)(const void *)a;
        out->family = 4u;
        ceesafe_mem_copy(out->bytes, &v4->sin_addr, 4u);
        return true;
    }
    if (a->sa_family == AF_INET6) {
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)(const void *)a;
        out->family = 6u;
        ceesafe_mem_copy(out->bytes, &v6->sin6_addr, 16u);
        out->scope = (uint32_t)v6->sin6_scope_id;
        return true;
    }
    return false;
}

/* Adds `e` unless an equal address is there already. */
static void cee_dns_add(cee_dns *d, const ceesafe_endpoint *e, uint32_t room) {
    for (uint32_t k = 0; k < d->count; k++) {
        const ceesafe_endpoint *x = &d->addresses[k];
        if (x->family == e->family && x->scope == e->scope && memcmp(x->bytes, e->bytes, 16u) == 0) {
            return;
        }
    }
    if (d->count < room) {
        d->addresses[d->count++] = *e;
    }
}

/* The resolver's answer for `d`: its addresses, or a negated errno (ADR 0048 §3.1's table). */
static int32_t cee_dns_resolve(cee_dns *d) {
#if defined(_WIN32)
    if (!ceesafe_winsock_ready()) {
        return -(int32_t)EIO;
    }
    int wide = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, d->host, -1, NULL, 0);
    if (wide <= 0) {
        return -(int32_t)EINVAL;
    }
    wchar_t *name = (wchar_t *)ceesafe_platform_alloc((size_t)wide * sizeof(wchar_t));
    if (name == NULL) {
        return -(int32_t)ENOMEM;
    }
    (void)MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, d->host, -1, name, wide);
    ADDRINFOW hints;
    ceesafe_mem_set(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    ADDRINFOW *list = NULL;
    int r = GetAddrInfoW(name, NULL, &hints, &list);
    ceesafe_platform_free(name);
    if (r != 0) {
        switch (r) {
        case WSAHOST_NOT_FOUND: case WSANO_DATA: return -(int32_t)ENOENT;
        case WSATRY_AGAIN: return -(int32_t)ETIMEDOUT;
        case WSA_NOT_ENOUGH_MEMORY: return -(int32_t)ENOMEM;
        case WSAEINVAL: case WSAEAFNOSUPPORT: case WSATYPE_NOT_FOUND: return -(int32_t)EINVAL;
        default: return -(int32_t)EIO;
        }
    }
    uint32_t room = 0;
    for (ADDRINFOW *a = list; a != NULL; a = a->ai_next) room++;
    d->addresses = (ceesafe_endpoint *)ceesafe_platform_alloc((room > 0u ? room : 1u) * sizeof(ceesafe_endpoint));
    if (d->addresses == NULL) {
        FreeAddrInfoW(list);
        return -(int32_t)ENOMEM;
    }
    for (ADDRINFOW *a = list; a != NULL; a = a->ai_next) {
        ceesafe_endpoint e;
        if (a->ai_addr != NULL && cee_dns_endpoint(a->ai_addr, d->port, &e)) cee_dns_add(d, &e, room);
    }
    FreeAddrInfoW(list);
#else
    struct addrinfo hints;
    ceesafe_mem_set(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *list = NULL;
    int r = getaddrinfo(d->host, NULL, &hints, &list);
    if (r != 0) {
        switch (r) {
        case EAI_NONAME:
#if defined(EAI_NODATA) && EAI_NODATA != EAI_NONAME
        case EAI_NODATA:
#endif
#if defined(EAI_ADDRFAMILY)
        case EAI_ADDRFAMILY:
#endif
            return -(int32_t)ENOENT;
        case EAI_AGAIN: return -(int32_t)ETIMEDOUT;
        case EAI_MEMORY: return -(int32_t)ENOMEM;
        case EAI_FAMILY: case EAI_SERVICE: case EAI_BADFLAGS: return -(int32_t)EINVAL;
#if defined(EAI_SYSTEM)
        case EAI_SYSTEM: return -(int32_t)(errno != 0 ? errno : EIO);
#endif
        default: return -(int32_t)EIO;
        }
    }
    uint32_t room = 0;
    for (struct addrinfo *a = list; a != NULL; a = a->ai_next) room++;
    d->addresses = (ceesafe_endpoint *)ceesafe_platform_alloc((room > 0u ? room : 1u) * sizeof(ceesafe_endpoint));
    if (d->addresses == NULL) {
        freeaddrinfo(list);
        return -(int32_t)ENOMEM;
    }
    for (struct addrinfo *a = list; a != NULL; a = a->ai_next) {
        ceesafe_endpoint e;
        if (a->ai_addr != NULL && cee_dns_endpoint(a->ai_addr, d->port, &e)) cee_dns_add(d, &e, room);
    }
    freeaddrinfo(list);
#endif
    return d->count == 0u ? -(int32_t)ENOENT : 0;
}

/* A resolver thread: lookups from the queue until it is empty, then it ends. */
#if defined(_WIN32)
static DWORD WINAPI cee_dns_thread(LPVOID unused)
#else
static void *cee_dns_thread(void *unused)
#endif
{
    (void)unused;
    for (;;) {
        cee_dns_lock();
        cee_dns *d = cee_dns_head;
        if (d == NULL) {
            cee_dns_threads -= 1u;
            cee_dns_unlock();
            break;
        }
        cee_dns_head = d->next;
        if (cee_dns_head == NULL) {
            cee_dns_tail = NULL;
        }
        d->next = NULL;
        d->state = 1u;
        cee_dns_unlock();
        cee_dns_delay();
        int32_t error = cee_dns_resolve(d);
        cee_dns_lock();
        if (d->abandoned != 0u) {
            cee_dns_unlock();
            cee_dns_free(d);
            continue;
        }
        d->error = error;
        d->state = 2u;
        if (d->task != NULL) {
            cee_wake_task((cee_executor *)d->executor, (cee_task *)d->task);
            d->task = NULL;
        }
        cee_dns_unlock();
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

/* Starts a resolver thread, detached; false if none could start. */
static bool cee_dns_spawn(void) {
#if defined(_WIN32)
    HANDLE h = CreateThread(NULL, 0, cee_dns_thread, NULL, 0, NULL);
    if (h == NULL) {
        return false;
    }
    CloseHandle(h);
    return true;
#else
    pthread_t t;
    if (pthread_create(&t, NULL, cee_dns_thread, NULL) != 0) {
        return false;
    }
    (void)pthread_detach(t);
    return true;
#endif
}

void *ceesafe_dns_start(const uint8_t *host, uint64_t length, uint16_t port) {
    cee_dns *d = (cee_dns *)ceesafe_platform_alloc_zeroed(sizeof(cee_dns) + (size_t)length + 1u);
    if (d == NULL) {
        return NULL;
    }
    ceesafe_mem_copy(d->host, host, (size_t)length);
    d->host[length] = '\0';
    d->port = port;
    cee_dns_lock();
    if (cee_dns_tail != NULL) {
        cee_dns_tail->next = d;
    } else {
        cee_dns_head = d;
    }
    cee_dns_tail = d;
    bool spawn = cee_dns_threads < CEE_DNS_THREADS;
    if (spawn) {
        cee_dns_threads += 1u;
    }
    cee_dns_unlock();
    if (spawn && !cee_dns_spawn()) {
        /* No thread: resolve here, at once (as Async.blocking does), unless another thread already took it. */
        cee_dns_lock();
        cee_dns_threads -= 1u;
        bool mine = d->state == 0u;
        if (mine) {
            cee_dns *prev = NULL;
            for (cee_dns *x = cee_dns_head; x != NULL; prev = x, x = x->next) {
                if (x == d) {
                    if (prev != NULL) prev->next = d->next; else cee_dns_head = d->next;
                    if (cee_dns_tail == d) cee_dns_tail = prev;
                    break;
                }
            }
            d->state = 1u;
        }
        cee_dns_unlock();
        if (mine) {
            d->error = cee_dns_resolve(d);
            d->state = 2u;
        }
    }
    return d;
}

int32_t ceesafe_dns_poll(void *lookup) {
    cee_dns *d = (cee_dns *)lookup;
    cee_dns_lock();
    int32_t r = 0;
    if (d->state == 2u) {
        d->task = NULL;
        r = d->error != 0 ? d->error : 1;
    } else {
        d->task = cee_dns_wait_task(&d->executor);
    }
    cee_dns_unlock();
    return r;
}

uint32_t ceesafe_dns_count(void *lookup) { return ((cee_dns *)lookup)->count; }

void ceesafe_dns_address(void *lookup, uint32_t index, uint8_t *bytes, uint32_t *family, uint32_t *scope) {
    const ceesafe_endpoint *e = &((cee_dns *)lookup)->addresses[index];
    ceesafe_mem_copy(bytes, e->bytes, 16u);
    *family = e->family;
    *scope = e->scope;
}

void ceesafe_dns_end(void *lookup) {
    cee_dns *d = (cee_dns *)lookup;
    cee_dns_lock();
    d->task = NULL;
    bool free_now = true;
    if (d->state == 0u) {
        cee_dns *prev = NULL;
        for (cee_dns *x = cee_dns_head; x != NULL; prev = x, x = x->next) {
            if (x == d) {
                if (prev != NULL) prev->next = d->next; else cee_dns_head = d->next;
                if (cee_dns_tail == d) cee_dns_tail = prev;
                break;
            }
        }
    } else if (d->state == 1u) {
        d->abandoned = 1u; /* its thread frees it */
        free_now = false;
    }
    cee_dns_unlock();
    if (free_now) {
        cee_dns_free(d);
    }
}
#endif
#endif /* !CEE_FREESTANDING: DNS */

