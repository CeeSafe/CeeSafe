/* SPDX-License-Identifier: GPL-2.0-only WITH Classpath-exception-2.0 */
/*
 * CeeSafe runtime core: integer formatting, output and panic reporting, heap accounting and regions, built
 * only on the platform services declared in ceesafe/rt.h. No libc.
 *
 * With CEE_FREESTANDING defined (the freestanding profile, profiles.md §4.2) only the first part
 * is compiled: code that needs no platform service and has no mutable global state. The environment
 * then defines ceesafe_panic.
 */
#include "ceesafe/rt.h"

const char *ceesafe_panic_reason(cee_panic_kind kind) {
    switch (kind) {
    case CEE_PANIC_OVERFLOW: return "integer overflow";
    case CEE_PANIC_DIVIDE_BY_ZERO: return "division by zero";
    case CEE_PANIC_SHIFT: return "shift amount out of range";
    case CEE_PANIC_UNREACHABLE: return "entered unreachable code";
    case CEE_PANIC_INDEX: return "index out of bounds";
    case CEE_PANIC_SLICE: return "slice range out of bounds";
    case CEE_PANIC_UNWRAP: return "unwrapped an Err value";
    case CEE_PANIC_SCRATCH: return "a scratch frame is already active; nest with frame.enter()";
    case CEE_PANIC_UNWRAP_NULL: return "unwrapped null";
    case CEE_PANIC_STR_BOUNDARY: return "string slice not on a character boundary";
    case CEE_PANIC_FFI_ENUM: return "invalid enum value from C";
    case CEE_PANIC_LENGTH: return "slice lengths differ";
    case CEE_PANIC_FLOAT_RANGE: return "float out of range";
    case CEE_PANIC_ASYNC: return "async executor: every task waits with nothing to wake it";
    case CEE_PANIC_FRAME_STACK: return "async frame stack: out of memory";
    default: return "unknown panic";
    }
}

cee_str ceesafe_io_message(uint32_t kind) {
    static const char *const messages[] = {
        "No such file or directory", "Permission denied", "File exists", "Is a directory",
        "Not a directory", "Invalid argument", "Broken pipe", "Connection refused",
        "Address already in use", "Out of memory", "Input/output error", "Connection reset by peer",
        "Software caused connection abort", "Transport endpoint is not connected", "Connection timed out",
        "No route to host", "Network is unreachable", "Cannot assign requested address", "Operation not supported",
        "Host not found",
    };
    const char *text = messages[kind <= CEE_IO_HOST_NOT_FOUND ? kind : CEE_IO_OTHER];
    uint64_t length = 0;
    while (text[length] != 0) length++;
    return (cee_str){ .ptr = (const uint8_t *)text, .len = length };
}

bool ceesafe_utf8_valid(const uint8_t *bytes, uint64_t length) {
    uint64_t i = 0;
    while (i < length) {
        uint8_t b = bytes[i];
        uint64_t extra;
        uint32_t min;
        uint32_t code;
        if (b < 0x80u) { i++; continue; }
        if ((b & 0xE0u) == 0xC0u) { extra = 1; min = 0x80u; code = b & 0x1Fu; }
        else if ((b & 0xF0u) == 0xE0u) { extra = 2; min = 0x800u; code = b & 0x0Fu; }
        else if ((b & 0xF8u) == 0xF0u) { extra = 3; min = 0x10000u; code = b & 0x07u; }
        else return false;
        if (extra > length - i - 1) return false;
        for (uint64_t k = 1; k <= extra; k++) {
            uint8_t c = bytes[i + k];
            if ((c & 0xC0u) != 0x80u) return false;
            code = (code << 6) | (c & 0x3Fu);
        }
        if (code < min || code > 0x10FFFFu || (code >= 0xD800u && code <= 0xDFFFu)) return false;
        i += extra + 1;
    }
    return true;
}

/* ---- Fixed-buffer allocator (type-system.md §19.4) ----------------------------------------- */

cee_fba ceesafe_fba_over(uint8_t *base, uint64_t capacity) {
    cee_fba f = {base, capacity, 0u};
    return f;
}

static void *fba_alloc(cee_fba *f, uint64_t size, uint64_t align) {
    uintptr_t start = (uintptr_t)f->base + (uintptr_t)f->used;
    uintptr_t aligned = (start + (uintptr_t)(align - 1u)) & ~(uintptr_t)(align - 1u);
    uint64_t offset = (uint64_t)(aligned - (uintptr_t)f->base);
    if (aligned < start || offset > f->capacity || size > f->capacity - offset) {
        return NULL;
    }
    f->used = offset + size;
    return (void *)aligned;
}

#ifdef CEE_FREESTANDING
/* Only fixed buffers exist without a heap (profiles.md §4.1). */
void *ceesafe_region_alloc(void *region, uint32_t kind, uint64_t size, uint64_t align) {
    return kind == CEE_REGION_FBA ? fba_alloc((cee_fba *)region, size, align) : NULL;
}
#else

static size_t format_u64(uint64_t value, uint8_t buffer[20]) {
    size_t start = 20;
    do {
        buffer[--start] = (uint8_t)('0' + (value % 10u));
        value /= 10u;
    } while (value != 0u);
    return start;
}

void ceesafe_write_u64(uint64_t value) {
    uint8_t buffer[20];
    size_t start = format_u64(value, buffer);
    ceesafe_platform_write_stdout(buffer + start, 20u - start);
}

void ceesafe_write_i64(int64_t value) {
    if (value < 0) {
        static const uint8_t minus = '-';
        ceesafe_platform_write_stdout(&minus, 1u);
        /* -(value + 1) + 1 avoids negating INT64_MIN. */
        ceesafe_write_u64((uint64_t)(-(value + 1)) + 1u);
    } else {
        ceesafe_write_u64((uint64_t)value);
    }
}

void ceesafe_write_bool(bool value) {
    static const uint8_t yes[] = {'t', 'r', 'u', 'e'};
    static const uint8_t no[] = {'f', 'a', 'l', 's', 'e'};
    if (value) {
        ceesafe_platform_write_stdout(yes, sizeof yes);
    } else {
        ceesafe_platform_write_stdout(no, sizeof no);
    }
}

void ceesafe_write_str(const char *text, size_t length) {
    ceesafe_platform_write_stdout((const uint8_t *)text, length);
}

cee_max_align_t ceesafe_vec_empty;

void *ceesafe_vec_grow(void *ptr, uint64_t len, uint64_t *cap, uint64_t extra, uint64_t size, uint32_t allocator) {
    if (extra > UINT64_MAX - len) return NULL;
    uint64_t need = len + extra;
    if (need <= *cap) return ptr;
    uint64_t next = *cap > UINT64_MAX / 2u ? UINT64_MAX : *cap * 2u;
    if (next < 4u) next = 4u;
    if (next < need) next = need;
    if (size == 0u || next > (uint64_t)SIZE_MAX / size) return NULL;
    /* A first buffer is allocated; a later one is reallocated, which keeps the old block on failure. */
    void *fresh = *cap == 0u ? ceesafe_alloc((size_t)(next * size), allocator) : ceesafe_realloc(ptr, (size_t)(next * size), allocator);
    if (fresh == NULL) return NULL;
    *cap = next;
    return fresh;
}

void *ceesafe_svec_slot(void **chunks, uint64_t len, uint64_t size, uint32_t allocator) {
    if (len > UINT64_MAX - 8u) return NULL;
    uint64_t j = len + 8u;
    unsigned k = 60u - cee_clz64(j);
    if (k >= CEE_SVEC_CHUNKS) return NULL;
    if (chunks[k] == NULL) {
        uint64_t count = (uint64_t)8u << k;
        if (size == 0u || count > (uint64_t)SIZE_MAX / size) return NULL;
        chunks[k] = ceesafe_alloc((size_t)(count * size), allocator);
        if (chunks[k] == NULL) return NULL;
    }
    return (uint8_t *)chunks[k] + (j - ((uint64_t)8u << k)) * size;
}

void ceesafe_svec_free(void **chunks, uint32_t allocator) {
    for (unsigned k = 0; k < CEE_SVEC_CHUNKS; k++) {
        if (chunks[k] != NULL) ceesafe_free(chunks[k], allocator);
    }
}

void ceesafe_write_bytes(const uint8_t *bytes, uint64_t length) {
    ceesafe_platform_write_stdout(bytes, (size_t)length);
}

void ceesafe_write_error(cee_str text) {
    ceesafe_platform_write_stderr(text.ptr, (size_t)text.len);
}

void ceesafe_write_view(cee_str text) {
    ceesafe_platform_write_stdout(text.ptr, (size_t)text.len);
}

void ceesafe_write_newline(void) {
    static const uint8_t newline = '\n';
    ceesafe_platform_write_stdout(&newline, 1u);
}

_Noreturn void ceesafe_exit(int32_t code) {
    ceesafe_platform_exit(code);
}

static void write_err_cstr(const char *text) {
    size_t length = 0;
    while (text[length] != '\0') {
        length++;
    }
    ceesafe_platform_write_stderr((const uint8_t *)text, length);
}

static void write_err_u32(uint32_t value) {
    uint8_t buffer[20];
    size_t start = format_u64(value, buffer);
    ceesafe_platform_write_stderr(buffer + start, 20u - start);
}

/* Message format (relied on by run-fail tests): "ceesafe panic: <reason> at <file>:<line>:<column>\n" */
_Noreturn void ceesafe_panic(cee_panic_kind kind, cee_loc loc) {
    const char *reason = ceesafe_panic_reason(kind);
    write_err_cstr("ceesafe panic: ");
    write_err_cstr(reason);
    write_err_cstr(" at ");
    write_err_cstr(loc.file);
    write_err_cstr(":");
    write_err_u32(loc.line);
    write_err_cstr(":");
    write_err_u32(loc.column);
    write_err_cstr("\n");
    ceesafe_platform_abort();
}

/* ---- Heap: accounting and fault injection (type-system.md §18) ---------------------------- */

static uint64_t alloc_attempts;   /* every allocation attempt, failed ones included */
static uint64_t alloc_live;       /* successful allocations not yet freed */
static uint64_t alloc_foreign;    /* CEE_ALLOC_FOREIGN's (ADR 0060): live, but not a leak */
static bool alloc_foreign_used;   /* C has allocated: the report gives alloc_foreign */
static uint64_t alloc_fail_at;    /* CEESAFE_FAIL_ALLOC_AT: this attempt fails; 0 = never */
static bool alloc_report;         /* CEESAFE_ALLOC_REPORT: print the counters at the end of main */
/* Whether to count at all (ADR 0059): only a report, CEESAFE_FAIL_ALLOC_AT or `ceesafec test` reads the counters. Decided
   before the program's first allocation and never changed afterwards, so a free never subtracts what its allocation
   did not add. */
static bool alloc_counting;
static bool failing_armed;        /* FailingAllocator.failAfter has been called */
static uint64_t failing_left;     /* successes FailingAllocator still allows */

void ceesafe_rt_start(void) {
    alloc_fail_at = ceesafe_platform_config("CEESAFE_FAIL_ALLOC_AT");
    alloc_report = ceesafe_platform_config("CEESAFE_ALLOC_REPORT") != 0u;
    alloc_counting = alloc_report || alloc_fail_at != 0u;
}

/* `ceesafec test`'s harness (ADR 0049): the allocation report on, and the sweeps' points from its arguments rather than
   the environment (0: none). */
uint64_t ceesafe_test_cancel_at;
bool ceesafe_test_reports;

void ceesafe_test_begin(uint64_t fail_alloc_at, uint64_t cancel_at) {
    alloc_report = true;
    alloc_counting = true;
    alloc_fail_at = fail_alloc_at;
    ceesafe_test_cancel_at = cancel_at;
    ceesafe_test_reports = true;
}

/*
 * The counters change from every thread of a threaded program (type-system.md §38), so atomically. The fault-injection
 * flags are set before threads exist (FailingAllocator is single-threaded test code).
 */
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#define CEE_COUNT(counter, delta) ((uint64_t)_InterlockedExchangeAdd64((volatile __int64 *)&(counter), (__int64)(delta)) + (uint64_t)(delta))
#else
#define CEE_COUNT(counter, delta) __atomic_add_fetch(&(counter), (uint64_t)(delta), __ATOMIC_RELAXED)
#endif

/* Counts an attempt when counting; true if it is to fail (FailingAllocator, or CEESAFE_FAIL_ALLOC_AT, which only a
   counting program has). A C library's one-time initialisation (CEE_ALLOC_FOREIGN_INIT, ADR 0061) is neither counted
   nor failed. */
static bool alloc_refused(uint32_t allocator) {
    if (allocator == CEE_ALLOC_FOREIGN_INIT) {
        return false;
    }
    uint64_t attempt = alloc_counting ? CEE_COUNT(alloc_attempts, 1u) : 0u;
    if (allocator == CEE_ALLOC_FAILING && failing_armed) {
        if (failing_left == 0u) {
            return true;
        }
        failing_left--;
    }
    return alloc_fail_at != 0u && attempt == alloc_fail_at;
}

/* One more live block (or one fewer, with UINT64_MAX) when counting, in C's count for CEE_ALLOC_FOREIGN (ADR 0060) and
   CEE_ALLOC_FOREIGN_INIT (ADR 0061), one count for both: a block may be freed under either. */
static void alloc_count_live(uint32_t allocator, uint64_t delta) {
    if ((allocator == CEE_ALLOC_FOREIGN || allocator == CEE_ALLOC_FOREIGN_INIT) && alloc_counting) {
        (void)CEE_COUNT(alloc_foreign, delta);
        alloc_foreign_used = true;
    } else if (alloc_counting) {
        (void)CEE_COUNT(alloc_live, delta);
    }
}

void *ceesafe_alloc(size_t size, uint32_t allocator) {
    if (alloc_refused(allocator)) {
        return NULL;
    }
    void *p = ceesafe_platform_alloc(size == 0u ? 1u : size);
    if (p != NULL && alloc_counting) {
        alloc_count_live(allocator, 1u);
    }
    return p;
}

void *ceesafe_alloc_zeroed(size_t size, uint32_t allocator) {
    if (alloc_refused(allocator)) {
        return NULL;
    }
    void *p = ceesafe_platform_alloc_zeroed(size == 0u ? 1u : size);
    if (p != NULL && alloc_counting) {
        alloc_count_live(allocator, 1u);
    }
    return p;
}

void *ceesafe_realloc(void *p, size_t size, uint32_t allocator) {
    if (p == NULL) {
        return ceesafe_alloc(size, allocator); /* C's realloc(NULL, n), for CEE_ALLOC_FOREIGN's callers */
    }
    if (alloc_refused(allocator)) {
        return NULL;
    }
    return ceesafe_platform_realloc(p, size == 0u ? 1u : size);
}

void ceesafe_free(void *p, uint32_t allocator) {
    /* every allocator frees to the platform heap */
    if (p == NULL) {
        return; /* C's free(NULL); CeeSafe's own code never frees NULL */
    }
    if (alloc_counting) {
        alloc_count_live(allocator, UINT64_MAX); /* minus one */
    }
    ceesafe_platform_free(p);
}

void ceesafe_fail_after(uint64_t n) {
    failing_armed = true;
    failing_left = n;
}

void ceesafe_fail_reset(void) {
    failing_armed = false;
}

static void write_err_u64(uint64_t value) {
    uint8_t buffer[20];
    size_t start = format_u64(value, buffer);
    ceesafe_platform_write_stderr(buffer + start, 20u - start);
}

/* Format (relied on by the fault-injection sweep): "ceesafe alloc: attempts=<n> live=<m>\n"; then, if C allocated
   with CEE_ALLOC_FOREIGN, "ceesafe alloc: foreign held=<k>\n" (ADR 0060), which no tool reads as a leak. */
int32_t ceesafe_rt_finish(int32_t code) {
    if (alloc_report) {
        write_err_cstr("ceesafe alloc: attempts=");
        write_err_u64(alloc_attempts);
        write_err_cstr(" live=");
        write_err_u64(alloc_live);
        write_err_cstr("\n");
        if (alloc_foreign_used) {
            write_err_cstr("ceesafe alloc: foreign held=");
            write_err_u64(alloc_foreign);
            write_err_cstr("\n");
        }
    }
    return code;
}

/* ---- Arena (type-system.md §19.2) ---------------------------------------------------------- */

#define CEE_ARENA_HEADER 16u /* the previous-chunk link, padded so data starts 16-aligned */

cee_arena ceesafe_arena_create(uint32_t allocator, uint64_t chunk_size) {
    cee_arena a = {NULL, NULL, NULL, chunk_size, allocator};
    return a;
}

void *ceesafe_arena_alloc(cee_arena *a, uint64_t size, uint64_t align) {
    uintptr_t cur = (uintptr_t)a->cur;
    uintptr_t aligned = (cur + (uintptr_t)(align - 1u)) & ~(uintptr_t)(align - 1u);
    if (a->cur != NULL && aligned >= cur && size <= (uint64_t)((uintptr_t)a->end - aligned)) {
        a->cur = (uint8_t *)(aligned + (uintptr_t)size);
        return (void *)aligned;
    }
    /* A new chunk: the usual size, or larger for a large allocation. */
    uint64_t need = size + CEE_ARENA_HEADER;
    if (need < size) {
        return NULL; /* overflow */
    }
    uint64_t total = need > a->chunk_size ? need : a->chunk_size;
#if SIZE_MAX < UINT64_MAX
    if (total > (uint64_t)SIZE_MAX) {
        return NULL;
    }
#endif
    uint8_t *chunk = (uint8_t *)ceesafe_alloc((size_t)total, (uint32_t)a->allocator);
    if (chunk == NULL) {
        return NULL;
    }
    *(void **)(void *)chunk = a->chunks;
    a->chunks = chunk;
    a->end = chunk + total;
    uint8_t *data = chunk + CEE_ARENA_HEADER; /* 16-aligned: malloc memory is, and align <= 16 */
    a->cur = data + size;
    return data;
}

void ceesafe_arena_reset(cee_arena *a) {
    void *chunk = a->chunks;
    while (chunk != NULL) {
        void *previous = *(void **)chunk;
        ceesafe_free(chunk, (uint32_t)a->allocator);
        chunk = previous;
    }
    a->chunks = NULL;
    a->cur = NULL;
    a->end = NULL;
}

void ceesafe_arena_drop(cee_arena *a) {
    ceesafe_arena_reset(a);
}

/* ---- Scratch frames (type-system.md §19.5; ADR 0004) -------------------------------------- */

#define CEE_SCRATCH_CHUNK 65536u

static void *scratch_chunk;   /* newest chunk; each starts with the previous chunk's address */
static uint8_t *scratch_cur;
static uint8_t *scratch_end;
static uint64_t scratch_depth; /* live frames */

cee_scratch ceesafe_scratch_enter_root(cee_loc loc) {
    if (scratch_depth != 0u) {
        ceesafe_panic(CEE_PANIC_SCRATCH, loc);
    }
    scratch_depth = 1u;
    cee_scratch frame = {scratch_chunk, scratch_cur, scratch_end, 1u};
    return frame;
}

cee_scratch ceesafe_scratch_enter_child(cee_scratch *parent) {
    scratch_depth = parent->depth + 1u;
    cee_scratch frame = {scratch_chunk, scratch_cur, scratch_end, scratch_depth};
    return frame;
}

void ceesafe_scratch_drop(cee_scratch *frame) {
    /* Frames are strictly LIFO (ADR 0004), so everything newer than the frame's entry state is its own. */
    while (scratch_chunk != frame->chunk) {
        void *previous = *(void **)scratch_chunk;
        ceesafe_free(scratch_chunk, CEE_ALLOC_SYSTEM);
        scratch_chunk = previous;
    }
    scratch_cur = frame->cur;
    scratch_end = frame->end;
    scratch_depth = frame->depth - 1u;
}

static void *scratch_alloc(uint64_t size, uint64_t align) {
    uintptr_t cur = (uintptr_t)scratch_cur;
    uintptr_t aligned = (cur + (uintptr_t)(align - 1u)) & ~(uintptr_t)(align - 1u);
    if (scratch_cur != NULL && aligned >= cur && size <= (uint64_t)((uintptr_t)scratch_end - aligned)) {
        scratch_cur = (uint8_t *)(aligned + (uintptr_t)size);
        return (void *)aligned;
    }
    uint64_t need = size + CEE_ARENA_HEADER;
    if (need < size) {
        return NULL;
    }
    uint64_t total = need > CEE_SCRATCH_CHUNK ? need : CEE_SCRATCH_CHUNK;
#if SIZE_MAX < UINT64_MAX
    if (total > (uint64_t)SIZE_MAX) {
        return NULL;
    }
#endif
    uint8_t *chunk = (uint8_t *)ceesafe_alloc((size_t)total, CEE_ALLOC_SYSTEM);
    if (chunk == NULL) {
        return NULL;
    }
    *(void **)(void *)chunk = scratch_chunk;
    scratch_chunk = chunk;
    scratch_end = chunk + total;
    scratch_cur = chunk + CEE_ARENA_HEADER + size;
    return chunk + CEE_ARENA_HEADER;
}

void *ceesafe_region_alloc(void *region, uint32_t kind, uint64_t size, uint64_t align) {
    switch (kind) {
    case CEE_REGION_ARENA: return ceesafe_arena_alloc((cee_arena *)region, size, align);
    case CEE_REGION_FBA: return fba_alloc((cee_fba *)region, size, align);
    default: return scratch_alloc(size, align);
    }
}

/* ---- Pools (type-system.md §19.6) ---------------------------------------------------------- */

cee_pool ceesafe_pool_create(uint32_t allocator, uint64_t capacity) {
    cee_pool p = {NULL, NULL, capacity, 0u, 0u, allocator};
    return p;
}

void *ceesafe_pool_take(cee_pool *p, uint64_t size) {
    if (p->slots == NULL) {
        /* Slots hold either a value or the free-list link, and start 16-aligned. */
        uint64_t slot = size < sizeof(void *) ? sizeof(void *) : size;
        slot = (slot + 15u) & ~(uint64_t)15u;
        if (p->capacity == 0u || slot > UINT64_MAX / p->capacity) {
            return NULL;
        }
        uint64_t total = slot * p->capacity;
#if SIZE_MAX < UINT64_MAX
        if (total > (uint64_t)SIZE_MAX) {
            return NULL;
        }
#endif
        p->slots = (uint8_t *)ceesafe_alloc((size_t)total, (uint32_t)p->allocator);
        if (p->slots == NULL) {
            return NULL;
        }
        p->slot_size = slot;
    }
    if (p->free != NULL) {
        void *slot = p->free;
        p->free = *(void **)slot;
        return slot;
    }
    if (p->used == p->capacity) {
        return NULL;
    }
    return p->slots + p->slot_size * p->used++;
}

void ceesafe_pool_release(cee_pool *p, void *slot) {
    *(void **)slot = p->free;
    p->free = slot;
}

void ceesafe_pool_drop(cee_pool *p) {
    /* Every box is gone (the borrow checker keeps the pool alive while one is live). */
    if (p->slots != NULL) {
        ceesafe_free(p->slots, (uint32_t)p->allocator);
    }
}
#endif /* CEE_FREESTANDING */
