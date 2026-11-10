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
    case CEE_PANIC_EXPLICIT: return "explicit panic";
    case CEE_PANIC_ALLOCATOR: return "the host's allocator broke its contract";
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
        "Host not found", "No space left on device",
    };
    const char *text = messages[kind <= CEE_IO_STORAGE_FULL ? kind : CEE_IO_OTHER];
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
/* The environment reports panics by kind only (profiles.md §4.2). */
_Noreturn void ceesafe_panic_text(cee_str message, cee_loc loc) {
    (void)message;
    ceesafe_panic(CEE_PANIC_EXPLICIT, loc);
}
/* Embedding (ceesafe/embed.h's freestanding form, ADR 0088 §2.2): nothing to configure, and no mutable global state
   (profiles.md §4.4), so no state word either: the size is checked and nothing else. */
int32_t ceesafe_init(const ceesafe_config *config) {
    return config == NULL || config->size != (uint32_t)sizeof(ceesafe_config) ? CEESAFE_INIT_SIZE : 0;
}
uint64_t ceesafe_shutdown(void) {
    return 0u;
}
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

/* ---- Panics, and the host's panic hook (ADR 0088 §2.4) ------------------------------------- */

/* The runtime's state word (ADR 0088 §2.2): ceesafe_init moves it UNSTARTED -> STARTING with a compare-and-swap and
   publishes the copied config with the store of STARTED; on an error it stores UNSTARTED back. */
#define CEE_RT_UNSTARTED 0u
#define CEE_RT_STARTING 1u
#define CEE_RT_STARTED 2u
static uint32_t rt_state;
static ceesafe_panic_hook rt_panic_hook; /* written before STARTED is stored, read after it is loaded */
static void *rt_panic_context;
/* Set on the thread running ceesafe_init while it is STARTING, after the hook is written: the host's allocator can
   panic there (a misaligned argv copy, CEE_PANIC_ALLOCATOR), and that panic reaches the hook too. No other thread
   reads the hook before STARTED, so this adds no race. */
static _Thread_local uint8_t rt_initializing;

/* Set before the hook runs (ADR 0088 §2.4): ceesafe_any_panicked for every thread, then the thread's own flag, which
   ceesafe_entry_panicked reads at an export or @callback entry once the first is set (relaxed: a thread sees its own
   store, and another thread's panic only sends it here to read its own flag). */
uint32_t ceesafe_any_panicked;
static _Thread_local uint8_t ceesafe_thread_panicked;
/* This thread's first panic, for the line ceesafe_panic_reentered prints. Its message is copied (up to
   CEE_PANIC_KEPT bytes): a runtime-built message lives in the panicking frame, which is gone by the re-entry. */
#define CEE_PANIC_KEPT 256u
static _Thread_local ceesafe_panic_info thread_panic;
static _Thread_local uint8_t thread_panic_text[CEE_PANIC_KEPT];

/* Message format (relied on by run-fail tests): "ceesafe panic: <reason or message> at <file>:<line>:<column>\n"; a
   runtime abort, which has no CeeSafe location, ends after its message. */
static void write_panic_line(const ceesafe_panic_info *info) {
    write_err_cstr("ceesafe panic: ");
    if (info->message != NULL) {
        ceesafe_platform_write_stderr(info->message, info->message_length);
    } else {
        write_err_cstr(info->reason);
    }
    if (info->file != NULL) {
        write_err_cstr(" at ");
        write_err_cstr(info->file);
        write_err_cstr(":");
        write_err_u32(info->line);
        write_err_cstr(":");
        write_err_u32(info->column);
    }
    write_err_cstr("\n");
}

/* The one path every hosted panic takes: the hook on the thread's first panic, if ceesafe_init installed one; then,
   if the hook returns, or on a panic inside it, the printed line and the abort. */
static _Noreturn void panic_report(const ceesafe_panic_info *info) {
    if (ceesafe_thread_panicked == 0u) {
#if defined(_MSC_VER) && !defined(__clang__)
        *(volatile uint32_t *)&ceesafe_any_panicked = 1u;
#else
        __atomic_store_n(&ceesafe_any_panicked, 1u, __ATOMIC_RELAXED);
#endif
        ceesafe_thread_panicked = 1u;
        thread_panic = *info;
        if (info->message != NULL) {
            size_t kept = info->message_length < CEE_PANIC_KEPT ? info->message_length : CEE_PANIC_KEPT;
            ceesafe_mem_copy(thread_panic_text, info->message, kept);
            thread_panic.message = thread_panic_text;
            thread_panic.message_length = kept;
        }
        /* The hook gets the whole message, which lives until the hook returns. It may run on several threads at
           once: one call per panicking thread (ADR 0088 §2.4). */
        if ((rt_initializing != 0u || ceesafe_atomic_load_u32(&rt_state) == CEE_RT_STARTED) && rt_panic_hook != NULL) {
            rt_panic_hook(rt_panic_context, info);
        }
    }
    write_panic_line(info);
    ceesafe_platform_abort();
}

_Noreturn void ceesafe_panic(cee_panic_kind kind, cee_loc loc) {
    ceesafe_panic_info info = {(uint32_t)kind, ceesafe_panic_reason(kind), NULL, 0u, loc.file, loc.line, loc.column};
    panic_report(&info);
}

/* The program's text in place of the reason (ADR 0075 §2.8). */
_Noreturn void ceesafe_panic_text(cee_str message, cee_loc loc) {
    static const uint8_t empty = 0u;
    ceesafe_panic_info info = {(uint32_t)CEE_PANIC_EXPLICIT, ceesafe_panic_reason(CEE_PANIC_EXPLICIT),
                               message.ptr != NULL ? message.ptr : &empty, (size_t)message.len, loc.file, loc.line,
                               loc.column};
    panic_report(&info);
}

_Noreturn void ceesafe_panic_runtime(cee_panic_kind kind, const char *message) {
    size_t length = 0;
    while (message[length] != '\0') {
        length++;
    }
    ceesafe_panic_info info = {(uint32_t)kind, ceesafe_panic_reason(kind), (const uint8_t *)message, length, NULL, 0u,
                               0u};
    panic_report(&info);
}

/* An export or @callback entered on a thread that has panicked: that panic's line (its message as kept), and the
   abort; never the hook. */
static _Noreturn void ceesafe_panic_reentered(void) {
    write_panic_line(&thread_panic);
    ceesafe_platform_abort();
}

/* ceesafe_entry_check's slow path, once some thread has panicked: this thread's flag. */
void ceesafe_entry_panicked(void) {
    if (ceesafe_thread_panicked != 0u) {
        ceesafe_panic_reentered();
    }
}

/* ---- Heap: accounting and fault injection (type-system.md §18) ---------------------------- */

static uint64_t alloc_attempts;   /* every allocation attempt, failed ones included */
static uint64_t alloc_live;       /* successful allocations not yet freed */
static uint64_t alloc_foreign;    /* CEE_ALLOC_FOREIGN's (ADR 0060): live, but not a leak */
static uint32_t alloc_foreign_used; /* C has allocated: the report gives alloc_foreign (ceesafe_atomic_*) */
static uint64_t alloc_fail_at;    /* CEESAFE_FAIL_ALLOC_AT: this attempt fails; 0 = never */
static bool alloc_report;         /* CEESAFE_ALLOC_REPORT: print the counters at the end of main */
/* Whether to count at all (ADR 0059): only a report, CEESAFE_FAIL_ALLOC_AT or `ceesafec test` reads the counters. Decided
   before the program's first allocation and never changed afterwards, so a free never subtracts what its allocation
   did not add. */
static bool alloc_counting;
/* FailingAllocator's countdown (type-system.md §18.1): process-wide, and changed from every thread, so atomic
   (ceesafe_atomic_*). failAfter stores `failing_left` before `failing_armed`, so an allocation that sees the flag sees
   the count it was armed with. */
static uint32_t failing_armed;    /* FailingAllocator.failAfter has been called, reset has not */
static uint64_t failing_left;     /* successes FailingAllocator still allows */

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
 * The counters change from every thread of a threaded program (type-system.md §38), so atomically: every attempt
 * gets its own number, in the order of the increments, and CEESAFE_FAIL_ALLOC_AT fails exactly one. The configuration
 * (alloc_counting, alloc_fail_at, alloc_report) is written only by ceesafe_init and ceesafe_test_begin, which run
 * before any thread calls CeeSafe (ADR 0088 §2.2's ordering contract); a thread's start orders them before everything
 * it does.
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
    if (allocator == CEE_ALLOC_FAILING && ceesafe_atomic_load_u32(&failing_armed) != 0u) {
        /* One success taken from the countdown, or none left: a decrement that never goes below zero. */
        uint64_t left = ceesafe_atomic_load_u64(&failing_left);
        for (;;) {
            if (left == 0u) {
                return true;
            }
            uint64_t seen = ceesafe_atomic_cas_u64(&failing_left, left, left - 1u);
            if (seen == left) {
                break;
            }
            left = seen;
        }
    }
    return alloc_fail_at != 0u && attempt == alloc_fail_at;
}

/* One more live block (or one fewer, with UINT64_MAX) when counting, in C's count for CEE_ALLOC_FOREIGN (ADR 0060) and
   CEE_ALLOC_FOREIGN_INIT (ADR 0061), one count for both: a block may be freed under either. */
static void alloc_count_live(uint32_t allocator, uint64_t delta) {
    if ((allocator == CEE_ALLOC_FOREIGN || allocator == CEE_ALLOC_FOREIGN_INIT) && alloc_counting) {
        (void)CEE_COUNT(alloc_foreign, delta);
        if (ceesafe_atomic_load_u32(&alloc_foreign_used) == 0u) {
            ceesafe_atomic_store_u32(&alloc_foreign_used, 1u);
        }
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
    ceesafe_atomic_store_u64(&failing_left, n);
    ceesafe_atomic_store_u32(&failing_armed, 1u);
}

void ceesafe_fail_reset(void) {
    ceesafe_atomic_store_u32(&failing_armed, 0u);
}

static void write_err_u64(uint64_t value) {
    uint8_t buffer[20];
    size_t start = format_u64(value, buffer);
    ceesafe_platform_write_stderr(buffer + start, 20u - start);
}

/* ---- Embedding: ceesafe_init and ceesafe_shutdown (ceesafe/embed.h, ADR 0088 §2.2) -------------------------- */

/* The config sizes this runtime accepts: the first version's, and each later version's end on a field boundary (a
   host built with an older ceesafe/embed.h). Only the first exists. */
static bool config_size_known(uint32_t size) {
    return size == (uint32_t)sizeof(ceesafe_config);
}

/* argv copied into one block from the platform functions (the host's allocator when ceesafe_init installs one, else
   the C library's), the array and its strings: not counted, never freed. */
static int32_t copy_arguments(int32_t argc, const char *const *argv, const uint8_t *const **out) {
    *out = NULL;
    if (argc <= 0 || argv == NULL) {
        return 0;
    }
    size_t count = (size_t)argc;
    if (count > (SIZE_MAX - sizeof(void *)) / sizeof(void *)) {
        return CEESAFE_INIT_MEMORY;
    }
    size_t total = (count + 1u) * sizeof(void *);
    for (size_t k = 0; k < count; k++) {
        size_t length = 0;
        if (argv[k] != NULL) {
            while (argv[k][length] != '\0') {
                length++;
            }
        }
        if (length + 1u > SIZE_MAX - total) {
            return CEESAFE_INIT_MEMORY;
        }
        total += length + 1u;
    }
    uint8_t *block = (uint8_t *)ceesafe_platform_alloc(total);
    if (block == NULL) {
        return CEESAFE_INIT_MEMORY;
    }
    const uint8_t **array = (const uint8_t **)(void *)block;
    uint8_t *text = block + (count + 1u) * sizeof(void *);
    for (size_t k = 0; k < count; k++) {
        size_t length = 0;
        if (argv[k] != NULL) {
            while (argv[k][length] != '\0') {
                length++;
            }
            ceesafe_mem_copy(text, argv[k], length);
        }
        text[length] = 0u;
        array[k] = text;
        text += length + 1u;
    }
    array[count] = NULL;
    *out = array;
    return 0;
}

static uint32_t allocated_already(void) {
#if defined(_MSC_VER) && !defined(__clang__)
    return *(volatile uint32_t *)&ceesafe_rt_allocated;
#else
    return __atomic_load_n(&ceesafe_rt_allocated, __ATOMIC_RELAXED);
#endif
}

/* Written by rt_init when the environment asked for counting after the runtime had allocated: counting is off, the
   hook and argv are taken, and this says so on stderr (ADR 0088 §2.2). */
static void warn_counting_unavailable(void) {
    static const char line[] = "ceesafe: CEESAFE_ALLOC_REPORT and CEESAFE_FAIL_ALLOC_AT ignored: the runtime had "
                               "already allocated before ceesafe_init, so allocations cannot be counted\n";
    ceesafe_platform_write_stderr((const uint8_t *)line, sizeof line - 1u);
}

/* ceesafe_init's work. `generated_main` is the generated C `main`'s call (ceesafe_rt_init_main), which counts even when
   the runtime has already allocated (a C constructor through CeeSafe), as 0.27.0 did: a host cannot ask for that,
   since the internal entry point is not in embed.h and config.flags' reserved bits are masked off. */
static int32_t rt_init(const ceesafe_config *config, bool generated_main) {
    for (;;) {
        uint32_t seen = ceesafe_atomic_cas_u32(&rt_state, CEE_RT_UNSTARTED, CEE_RT_STARTING);
        if (seen == CEE_RT_UNSTARTED) {
            break;
        }
        if (seen == CEE_RT_STARTED) {
            return CEESAFE_INIT_AGAIN; /* the load saw STARTED: the config is published to this thread */
        }
        /* STARTING: another thread's call is under way. AGAIN would let this caller call CeeSafe before that config
           is published, so wait for its end: STARTED (AGAIN), or UNSTARTED after its error (try again). */
        while (ceesafe_atomic_load_u32(&rt_state) == CEE_RT_STARTING) {
            cee_spin_hint();
        }
    }
    ceesafe_config given;
    ceesafe_mem_set(&given, 0, sizeof given); /* the fields an older header lacks take their defaults */
    int32_t error = 0;
    uint64_t fail_at = 0u;
    bool report = false;
    if (config == NULL || !config_size_known(config->size)) {
        error = CEESAFE_INIT_SIZE;
    } else {
        ceesafe_mem_copy(&given, config, config->size);
        given.flags &= CEESAFE_INIT_ENVIRONMENT; /* the reserved bits mean nothing (embed.h) */
        fail_at = given.fail_alloc_at;
        report = given.alloc_report;
        bool asked = report || fail_at != 0u; /* by the config itself */
        bool environment_asked = false;
        if ((given.flags & CEESAFE_INIT_ENVIRONMENT) != 0u) {
            uint64_t from_environment = ceesafe_platform_config("CEESAFE_FAIL_ALLOC_AT");
            if (from_environment != 0u) {
                fail_at = from_environment;
                environment_asked = true;
            }
            if (ceesafe_platform_config("CEESAFE_ALLOC_REPORT") != 0u) {
                report = true;
                environment_asked = true;
            }
        }
        const ceesafe_allocator *hook = given.allocator;
        if (hook != NULL && (hook->alloc == NULL || hook->alloc_zeroed == NULL || hook->realloc == NULL ||
                             hook->free == NULL)) {
            error = CEESAFE_INIT_ALLOCATOR; /* all four, or a block could be freed by the wrong heap */
        } else if (hook != NULL && allocated_already() != 0u) {
            /* The allocator cannot change under live blocks (ADR 0088 §2.5): one from the C library's heap would be
               given to the hook's free. Best effort, as for counting: an allocation racing with this call breaks the
               ordering contract and is not seen. */
            error = CEESAFE_INIT_LATE;
        } else if ((report || fail_at != 0u) && !generated_main && allocated_already() != 0u) {
            /* Counting cannot start under live blocks (ADR 0059): a free would subtract what was never added. Without
               counting, an earlier allocation changes nothing, so the hook and argv are taken. A config that asks for
               counting itself gets LATE; when only the environment asks (a variable the host's own environment
               happens to carry), counting is off and a warning says so, and the rest is taken. */
            if (asked) {
                error = CEESAFE_INIT_LATE;
            } else if (environment_asked) {
                report = false;
                fail_at = 0u;
                warn_counting_unavailable();
            }
        }
    }
    const uint8_t *const *arguments = NULL;
    if (error == 0) {
        /* Installed before argv is copied, so the copy is the hook's block too (§2.2). No other thread allocates
           meanwhile (the ordering contract), and on an error it is removed again: nothing changed. The panic hook is
           written first, so a panic of the allocator's on this thread (a misaligned block) reaches it. */
        rt_panic_hook = given.panic_hook;
        rt_panic_context = given.panic_context;
        rt_initializing = 1u;
        /* Only when one is given: without, the platform functions' state is not written at all, since a thread a C
           constructor started may be allocating through it while the generated main's ceesafe_init runs. */
        if (given.allocator != NULL) {
            ceesafe_platform_use_allocator(given.allocator);
        }
        error = copy_arguments(given.argc, given.argv, &arguments);
        rt_initializing = 0u;
        if (error != 0) {
            if (given.allocator != NULL) {
                ceesafe_platform_use_allocator(NULL);
            }
            rt_panic_hook = NULL;
            rt_panic_context = NULL;
        }
    }
    if (error != 0) {
        ceesafe_atomic_store_u32(&rt_state, CEE_RT_UNSTARTED);
        return error;
    }
    alloc_fail_at = fail_at;
    alloc_report = report;
    alloc_counting = report || fail_at != 0u;
    ceesafe_args_set(arguments != NULL ? given.argc : 0, arguments);
    ceesafe_atomic_store_u32(&rt_state, CEE_RT_STARTED); /* publishes everything above */
    return 0;
}

int32_t ceesafe_init(const ceesafe_config *config) {
    return rt_init(config, false);
}

int32_t ceesafe_rt_init_main(const ceesafe_config *config) {
    return rt_init(config, true);
}

/* Format (relied on by the fault-injection sweep): "ceesafe alloc: attempts=<n> live=<m>\n"; then, if C allocated
   with CEE_ALLOC_FOREIGN, "ceesafe alloc: foreign held=<k>\n" (ADR 0060), which no tool reads as a leak. */
uint64_t ceesafe_shutdown(void) {
    /* Before ceesafe_init has published the config (never called, or still under way on another thread), nothing
       was asked for: 0. The load orders that config before the reads below. */
    if (ceesafe_atomic_load_u32(&rt_state) != CEE_RT_STARTED) {
        return 0u;
    }
    uint64_t live = alloc_counting ? ceesafe_atomic_load_u64(&alloc_live) : 0u;
    if (alloc_report) {
        write_err_cstr("ceesafe alloc: attempts=");
        write_err_u64(ceesafe_atomic_load_u64(&alloc_attempts));
        write_err_cstr(" live=");
        write_err_u64(live);
        write_err_cstr("\n");
        if (ceesafe_atomic_load_u32(&alloc_foreign_used) != 0u) {
            write_err_cstr("ceesafe alloc: foreign held=");
            write_err_u64(ceesafe_atomic_load_u64(&alloc_foreign));
            write_err_cstr("\n");
        }
    }
    return live;
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

/* Each thread has its own scratch stack (type-system.md §19.5): a frame belongs to the thread that entered it, and
   neither a frame nor its allocations can reach another thread unless that thread is done with them before the frame
   ends (§38.6). _Thread_local as in ceesafe_rt_async.c (MSVC accepts it under /std:c11). A thread ends with every frame
   it entered dropped, so with no chunk left: nothing to free at its exit. */
static _Thread_local void *scratch_chunk; /* newest chunk; each starts with the previous chunk's address */
static _Thread_local uint8_t *scratch_cur;
static _Thread_local uint8_t *scratch_end;
static _Thread_local uint64_t scratch_depth; /* live frames */

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

/* Runs body(arg) on the calling thread as a thread of its own would run it: with an empty scratch stack, the caller's
   restored afterwards. For the places that run a thread's body here when no thread can be started (Thread.both,
   Async.blocking; type-system.md §38.6, §40.8), so the body can enter a root frame whether or not the caller has one.
   The body drops every frame it enters before it returns (they are scoped), so the stack is empty again then. */
void *ceesafe_scratch_aside(void *(*body)(void *), void *arg) {
    cee_scratch saved = {scratch_chunk, scratch_cur, scratch_end, scratch_depth};
    scratch_chunk = NULL;
    scratch_cur = NULL;
    scratch_end = NULL;
    scratch_depth = 0u;
    void *result = body(arg);
    scratch_chunk = saved.chunk;
    scratch_cur = saved.cur;
    scratch_end = saved.end;
    scratch_depth = saved.depth;
    return result;
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
