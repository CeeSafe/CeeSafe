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
    bool on_scope;         /* parked until its scope's last child returns */
} cee_task;

/* A child's task and frame are one block: the frame starts at the first max_align_t boundary after the task. */
#define CEE_TASK_HEAD ((sizeof(cee_task) + sizeof(max_align_t) - 1u) / sizeof(max_align_t) * sizeof(max_align_t))

typedef struct {
    cee_task *ready_head;
    cee_task *ready_tail;
    cee_task *parked;
    cee_task *current;
} cee_executor;

static _Thread_local cee_executor cee_exec;

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
    }
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
    if (scope->oom) {
        return NULL; /* the scope already failed to start a child: it starts no more */
    }
    unsigned char *block = (unsigned char *)ceesafe_alloc(CEE_TASK_HEAD + frame_size, CEE_ALLOC_SYSTEM);
    if (block == NULL) {
        scope->oom = true;
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
    scope->live += 1u;
    cee_ready(t);
    return t->frame;
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
        if (t->fd >= 0) {
            registered += 1;
        }
    }
    if (deadline == UINT64_MAX && registered == 0) {
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
        struct pollfd fds[64];
        cee_task *owners[64];
        nfds_t count = 0;
        for (cee_task *t = cee_exec.parked; t != NULL && count < 64u; t = t->next) {
            if (t->fd >= 0) {
                fds[count].fd = t->fd;
                fds[count].events = POLLOUT;
                fds[count].revents = 0;
                owners[count] = t;
                count += 1u;
            }
        }
        int ready = poll(count > 0u ? fds : NULL, count, timeout);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        for (nfds_t k = 0; ready > 0 && k < count; k++) {
            if (fds[k].revents != 0) {
                cee_unpark(owners[k]); /* the task retries, and sees an error if there was one */
                any = true;
            }
        }
#endif
        now = ceesafe_async_now();
        cee_task *t = cee_exec.parked;
        while (t != NULL) {
            cee_task *next = t->next;
            if (t->deadline != UINT64_MAX && now >= t->deadline) {
                cee_unpark(t);
                any = true;
            }
            t = next;
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
    cee_task root = {frame, resume, NULL, NULL, NULL, UINT64_MAX, -1, false};
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
            if (t->deadline == UINT64_MAX && t->fd < 0 && !t->on_scope) {
                cee_stuck(); /* parked without registering anything: nothing can wake it */
            }
            t->next = cee_exec.parked;
            cee_exec.parked = t;
            continue;
        }
        if (t == &root) {
            break; /* its scopes have ended, so every child has returned */
        }
        ceesafe_scope *scope = t->scope;
        ceesafe_free(t, CEE_ALLOC_SYSTEM);
        scope->live -= 1u;
        if (scope->live == 0u && scope->waiter != NULL) {
            cee_task *waiter = (cee_task *)scope->waiter;
            scope->waiter = NULL;
            cee_unpark(waiter);
        }
    }
    cee_exec = saved;
}
