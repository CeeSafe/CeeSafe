/* SPDX-License-Identifier: GPL-2.0-only WITH Classpath-exception-2.0 */
/*
 * Async tasks for the hosted profile (type-system.md §40, ADR 0031; c-backend.md §3a and §6). L1 A1: one task per
 * Async.run, so the executor resumes the root frame until it returns, and between resumes waits for what the task
 * registered before it parked: a deadline (lib/std's Timer) or a descriptor becoming writable (its console output).
 * Linked only into programs that call ceesafe_async_* (c-backend.md §6).
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

/* What the parked task waits for: one deadline (UINT64_MAX for none) and one descriptor (-1 for none). */
static _Thread_local uint64_t cee_async_deadline = UINT64_MAX;
static _Thread_local int32_t cee_async_fd = -1;

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
    if (deadline < cee_async_deadline) {
        cee_async_deadline = deadline;
    }
}

void ceesafe_async_wait_writable(int32_t fd) {
    cee_async_fd = fd;
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

/* Waits for what the parked task registered, then forgets it. */
static void cee_async_wait(void) {
    uint64_t deadline = cee_async_deadline;
    int32_t fd = cee_async_fd;
    cee_async_deadline = UINT64_MAX;
    cee_async_fd = -1;
    if (deadline == UINT64_MAX && fd < 0) {
        static const char message[] = "ceesafe: a task parked with nothing to wake it\n";
        ceesafe_platform_write_stderr((const uint8_t *)message, sizeof message - 1u);
        ceesafe_platform_abort();
    }
    for (;;) {
        uint64_t now = ceesafe_async_now();
        int timeout = -1;
        if (deadline != UINT64_MAX) {
            if (now >= deadline) {
                return;
            }
            uint64_t left = deadline - now;
            timeout = left > (uint64_t)INT_MAX ? INT_MAX : (int)left;
        }
#if defined(_WIN32)
        if (fd >= 0) {
            return; /* treated as writable: see ceesafe_async_write */
        }
        Sleep((DWORD)timeout);
#else
        struct pollfd p = {fd, POLLOUT, 0};
        int ready = poll(&p, fd >= 0 ? 1u : 0u, timeout);
        if (ready > 0) {
            return;
        }
        if (ready < 0 && errno != EINTR) {
            return; /* the task retries and sees the error */
        }
#endif
    }
}

void ceesafe_async_run(void *frame, bool (*resume)(void *)) {
    while (!resume(frame)) {
        cee_async_wait();
    }
}
