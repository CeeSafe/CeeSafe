/* SPDX-License-Identifier: GPL-2.0-only WITH Classpath-exception-2.0 */
/*
 * Embedding CeeSafe in a host program whose `main` is not CeeSafe's (ADR 0088): the whole interface a host calls.
 * ceesafec writes this header beside ceesafe/rt.h in every build that writes headers, and beside the archive of
 * `--emit lib`; the internal ceesafe_rt_* functions are not part of it.
 *
 * - ceesafe_init, once, must happen before every call into CeeSafe on every thread: called before the host starts the
 *   threads that call CeeSafe, or published to them by the host's own synchronisation. A host that never calls it gets
 *   the C library's allocator, the printed panic, no Process arguments and the environment variables ignored.
 * - Exports may then be called concurrently from any host thread, with no registration (ADR 0088 §2.3).
 * - ceesafe_shutdown prints the allocation report when one was asked for and returns the live blocks. It frees
 *   nothing: exports stay callable afterwards.
 *
 * In a freestanding build (profiles.md §4) the config has only `size` and `flags`: there is no environment, no
 * Process, no heap, and the panic hook is the environment's link-time ceesafe_panic (profiles.md §4.2).
 */
#ifndef CEESAFE_EMBED_H
#define CEESAFE_EMBED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CEESAFE_EMBED_VERSION 1

/* ceesafe_init's results: 0, or one of these, with nothing changed. */
#define CEESAFE_INIT_AGAIN 1     /* called before, or concurrently: returned once that call has succeeded */
#define CEESAFE_INIT_LATE 2      /* counting or an allocator asked for by the config, and the runtime has already
                                    allocated: neither can change under live blocks */
#define CEESAFE_INIT_SIZE 3      /* config is NULL, or its size is not one this runtime knows */
#define CEESAFE_INIT_ALLOCATOR 4 /* an allocator hook with a NULL function */
#define CEESAFE_INIT_MEMORY 5    /* the copy of argv could not be allocated */

#if !defined(CEESAFE_EMBED_FREESTANDING)

/* config.flags: read CEESAFE_ALLOC_REPORT and CEESAFE_FAIL_ALLOC_AT from the environment. The other bits are
   reserved: pass 0 in them (ceesafe_init masks them off and ignores them; a later version may give them a meaning).
   When only the environment asks for counting and the runtime has already allocated, ceesafe_init succeeds without
   counting, says so on stderr, and takes the hook and argv; a config that asks itself gets CEESAFE_INIT_LATE. */
#define CEESAFE_INIT_ENVIRONMENT 1u

/* The host's allocator (ADR 0088 §2.5), for every block the runtime allocates: CeeSafe's own, the runtime's buffers,
   and a C library's through ceesafe/foreign.h. It sits under CeeSafe's accounting, so the report, CEESAFE_FAIL_ALLOC_AT
   and FailingAllocator work with it, and an injected failure never reaches it. malloc's contracts, with one change:
   every block is aligned to max(16, alignof(max_align_t)) (max_align_t is 8-aligned on macOS arm64 and under MSVC); a
   block that is not makes the runtime panic, naming the function. NULL on failure. The functions are called from any
   thread, CeeSafe's or the host's, at once, are never given a size of 0, and must stay valid, with the context, until
   the process exits. Each side frees what it allocated through its own code: a block CeeSafe hands out comes back
   through an export, not through this free, even when both sides use one heap.

   The exception is a vector's buffer that changes sides (ADR 0090 §2.3): one CeeSafe gives up with
   Vector.intoForeign, which the host then frees or reallocates with the functions these call, or one of this
   allocator's blocks that the host gives up and CeeSafe takes with Vector.fromForeign. A given block holds
   capacity × size(T) bytes, at least 1, and is aligned as above when CeeSafe allocated or grew it. A taken block need
   only be aligned for its elements, so a host that lets CeeSafe take blocks owes one thing more: realloc, given any
   block of this allocator, also one aligned below 16, returns a block aligned as above. Without an allocator here,
   both sides use the C library's malloc, realloc and free. */
typedef struct ceesafe_allocator {
    void *(*alloc)(void *context, size_t size);                 /* malloc's contract */
    void *(*alloc_zeroed)(void *context, size_t size);          /* calloc(1, size)'s */
    void *(*realloc)(void *context, void *block, size_t size);  /* realloc's; a failure keeps the block */
    void (*free)(void *context, void *block);                   /* free's; never given NULL */
    void *context;
} ceesafe_allocator;

typedef struct ceesafe_panic_info {
    uint32_t kind;                      /* cee_panic_kind (ceesafe/rt.h) */
    const char *reason;                 /* "index out of bounds", as ceesafe_panic_reason gives */
    const uint8_t *message;             /* panic("...")'s text, or a runtime abort's; else NULL */
    size_t message_length;
    const char *file;                   /* the CeeSafe source path, zero-terminated; NULL for a runtime abort */
    uint32_t line, column;              /* 1-based; 0 when file is NULL */
} ceesafe_panic_info;

/* Runs on the panicking thread, once per thread: several threads that panic at once run it at once, so a hook that
   reports serialises the reports itself. It must end the process or never return; it must not call CeeSafe; escaping
   it (longjmp, an exception, an unwind, ending the thread) is undefined behaviour. If it returns, the runtime prints
   the usual line and aborts. `info` and its message are valid until the hook returns. */
typedef void (*ceesafe_panic_hook)(void *context, const ceesafe_panic_info *info);

typedef struct ceesafe_config {
    uint32_t size;                      /* sizeof(ceesafe_config): fields are only ever appended */
    uint32_t flags;                     /* CEESAFE_INIT_ENVIRONMENT */
    int32_t argc;                       /* Process.argument's; 0 and NULL: none */
    const char *const *argv;            /* copied by ceesafe_init */
    uint64_t fail_alloc_at;             /* as CEESAFE_FAIL_ALLOC_AT; the environment's wins when read */
    bool alloc_report;                  /* as CEESAFE_ALLOC_REPORT */
    const ceesafe_allocator *allocator; /* NULL: the C library's; copied by ceesafe_init */
    ceesafe_panic_hook panic_hook;      /* NULL: print and abort */
    void *panic_context;
} ceesafe_config;

#else

typedef struct ceesafe_config {
    uint32_t size;                      /* sizeof(ceesafe_config) */
    uint32_t flags;                     /* none are defined: pass 0 (this version ignores the field) */
} ceesafe_config;

#endif

int32_t ceesafe_init(const ceesafe_config *config);   /* 0, or a CEESAFE_INIT_* error; nothing changed on error */
uint64_t ceesafe_shutdown(void);                      /* prints the report if asked; returns the live blocks */

#ifdef __cplusplus
}
#endif

#endif
