/* SPDX-License-Identifier: GPL-2.0-only WITH Classpath-exception-2.0 */
/*
 * CeeSafe runtime platform layer for hosted C environments (Linux, macOS, CosmoOS userland libc).
 * Uses only standard C library functions; Windows (MSVC, best effort) also uses its C runtime's
 * binary-mode and _exit calls (spec/c-backend.md §1.1).
 */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* getenv is not deprecated in standard C */
#endif
#include "ceesafe/rt.h"
#include <errno.h>

#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

/* Windows translates "\n" to "\r\n" in text mode: CeeSafe's output is bytes, so both streams become binary. Several
   threads may write at once (ceesafe_atomic_*): the first switches the streams, and the others wait until it has. */
#ifdef _WIN32
static uint32_t ceesafe_binary_state; /* 0 not switched, 1 being switched, 2 switched */
#endif
static void ceesafe_binary_streams(void) {
#ifdef _WIN32
    if (ceesafe_atomic_load_u32(&ceesafe_binary_state) != 2u) {
        if (ceesafe_atomic_cas_u32(&ceesafe_binary_state, 0u, 1u) == 0u) {
            (void)_setmode(_fileno(stdout), _O_BINARY);
            (void)_setmode(_fileno(stderr), _O_BINARY);
            ceesafe_atomic_store_u32(&ceesafe_binary_state, 2u);
        } else {
            while (ceesafe_atomic_load_u32(&ceesafe_binary_state) != 2u) {
            }
        }
    }
#endif
}

void ceesafe_platform_write_stdout(const uint8_t *bytes, size_t length) {
    ceesafe_binary_streams();
    (void)fwrite(bytes, 1u, length, stdout);
}

/* `Console.flush()` (ADR 0077): what standard output holds buffered is written. */
int64_t ceesafe_host_console_flush(void) {
    return fflush(stdout) == 0 ? 0 : -(int64_t)EIO;
}

void ceesafe_platform_write_stderr(const uint8_t *bytes, size_t length) {
    ceesafe_binary_streams();
    /* Flush stdout first so output printed before a panic appears before the message. */
    (void)fflush(stdout);
    (void)fwrite(bytes, 1u, length, stderr);
}

_Noreturn void ceesafe_platform_exit(int32_t code) {
    exit((int)code); /* flushes stdio */
}

_Noreturn void ceesafe_platform_abort(void) {
    (void)fflush(stdout);
    (void)fflush(stderr);
#ifdef _WIN32
    _exit(134); /* the status abort() gives on Linux and macOS (128 + SIGABRT); Windows' abort() gives 3 */
#else
    abort();
#endif
}

void *ceesafe_platform_alloc(size_t size) {
    return malloc(size); /* aligned for max_align_t */
}

void *ceesafe_platform_alloc_zeroed(size_t size) {
    return calloc(1u, size); /* aligned for max_align_t */
}

void *ceesafe_platform_realloc(void *p, size_t size) {
    return realloc(p, size); /* aligned for max_align_t */
}

void ceesafe_platform_free(void *p) {
    free(p);
}

uint64_t ceesafe_platform_config(const char *name) {
    const char *text = getenv(name);
    if (text == NULL) {
        return 0u;
    }
    uint64_t value = 0u;
    for (; *text >= '0' && *text <= '9'; text++) {
        value = value * 10u + (uint64_t)(*text - '0');
    }
    return value;
}
