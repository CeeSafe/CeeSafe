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

/* ceesafe_init's CEESAFE_INIT_LATE (ADR 0088 §2.2): set at the first block these functions hand out. A relaxed load
   (a plain one under MSVC, whose volatile accesses are not torn), and a store only the first time. */
uint32_t ceesafe_rt_allocated = 0u;

static void *ceesafe_note_allocated(void *p) {
#if defined(_MSC_VER) && !defined(__clang__)
    if (p != NULL && *(volatile uint32_t *)&ceesafe_rt_allocated == 0u) {
        *(volatile uint32_t *)&ceesafe_rt_allocated = 1u;
    }
#else
    if (p != NULL && __atomic_load_n(&ceesafe_rt_allocated, __ATOMIC_RELAXED) == 0u) {
        __atomic_store_n(&ceesafe_rt_allocated, 1u, __ATOMIC_RELAXED);
    }
#endif
    return p;
}

/* A block from the C library, with no hook installed: noted, and from now on rt.h's inline allocation may be used
   as far as this file is concerned (ADR 0101). The word is read first: it is written once in a program's life. */
static void *ceesafe_note_direct(void *p) {
    (void)ceesafe_note_allocated(p);
#if defined(_MSC_VER) && !defined(__clang__)
    uint32_t watched = *(volatile const uint32_t *)&ceesafe_alloc_watched;
#else
    uint32_t watched = __atomic_load_n(&ceesafe_alloc_watched, __ATOMIC_RELAXED);
#endif
    if (p != NULL && (watched & CEE_ALLOC_WATCH_PLATFORM) != 0u) {
        ceesafe_alloc_watch(CEE_ALLOC_WATCH_PLATFORM, false);
    }
    return p;
}

/* The host's allocator (ADR 0088 §2.5), copied by ceesafe_init (ceesafe_platform_use_allocator) before it publishes
   STARTED, and read without a fence on every allocation after: embed.h's ordering contract makes ceesafe_init happen
   before every call into CeeSafe. Until then, and in a program that never installs one, the C library's, whose blocks
   are 16-aligned on every target CeeSafe supports. */
static ceesafe_allocator host_allocator;
static bool host_allocator_used;

/* The C library's path falls through, a load and an untaken branch beside 0.28.0's direct call: laid out as the taken
   branch, it cost binary-trees 6% (benchmarks/alloc_hook.py). */
#if defined(__GNUC__) || defined(__clang__)
#define CEE_HOOKED() __builtin_expect(host_allocator_used, 0)
#else
#define CEE_HOOKED() host_allocator_used
#endif

void ceesafe_platform_use_allocator(const ceesafe_allocator *allocator) {
    /* Watched until the C library's functions hand out a block again (ADR 0101): never, while a hook is installed. */
    ceesafe_alloc_watch(CEE_ALLOC_WATCH_PLATFORM, true);
    if (allocator != NULL) {
        host_allocator = *allocator;
        host_allocator_used = true;
    } else {
        static const ceesafe_allocator none = {0};
        host_allocator_used = false;
        host_allocator = none;
    }
}

/* A block from the hook: NULL, or aligned to CEE_HEAP_ALIGN. A misaligned block would be undefined behaviour at the
   first 16-aligned access (a task frame, ADR 0088 §1.9), so it panics here, naming the hook: one mask and an untaken
   branch beside an indirect call, so every build checks, not only `-g` ones. */
static void *ceesafe_hook_block(void *p, const char *which) {
    if (p != NULL && ((uintptr_t)p & (CEE_HEAP_ALIGN - 1u)) != 0u) {
        ceesafe_panic_runtime(CEE_PANIC_ALLOCATOR, which);
    }
    return p;
}

/* The hooks are given at least 1 byte: malloc(0) and realloc(p, 0) mean different things in different C libraries. */
void *ceesafe_platform_alloc(size_t size) {
    if (CEE_HOOKED()) {
        void *p = host_allocator.alloc(host_allocator.context, size == 0u ? 1u : size);
        return ceesafe_note_allocated(ceesafe_hook_block(p, "ceesafe_allocator.alloc returned a misaligned block"));
    }
    return ceesafe_note_direct(malloc(size));
}

void *ceesafe_platform_alloc_zeroed(size_t size) {
    if (CEE_HOOKED()) {
        void *p = host_allocator.alloc_zeroed(host_allocator.context, size == 0u ? 1u : size);
        return ceesafe_note_allocated(
            ceesafe_hook_block(p, "ceesafe_allocator.alloc_zeroed returned a misaligned block"));
    }
    return ceesafe_note_direct(calloc(1u, size));
}

void *ceesafe_platform_realloc(void *block, size_t size) {
    if (CEE_HOOKED()) {
        if (block == NULL) {
            return ceesafe_platform_alloc(size); /* realloc(NULL, n) */
        }
        void *p = host_allocator.realloc(host_allocator.context, block, size == 0u ? 1u : size);
        return ceesafe_note_allocated(ceesafe_hook_block(p, "ceesafe_allocator.realloc returned a misaligned block"));
    }
    return ceesafe_note_direct(realloc(block, size));
}

void ceesafe_platform_free(void *p) {
    if (CEE_HOOKED()) {
        if (p != NULL) {
            host_allocator.free(host_allocator.context, p); /* the hook is never given NULL (embed.h) */
        }
        return;
    }
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
