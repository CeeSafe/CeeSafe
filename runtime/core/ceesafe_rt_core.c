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

/* This file defines ceesafe_alloc and ceesafe_free, and its own calls are to them: rt.h's inline fast path (ADR 0101)
   is for the generated code and the other runtime files. */
#undef ceesafe_alloc
#undef ceesafe_free

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
    case CEE_PANIC_POISONED: return "this lock was held by an isolated task that trapped: its data may be broken";
    case CEE_PANIC_OVERRAN: return "a task ran past its bound without waiting";
    case CEE_PANIC_NARROWING: return "conversion loses information on this 32-bit target";
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
        "Host not found", "No space left on device", "Too many open files",
    };
    const char *text = messages[kind <= CEE_IO_TOO_MANY_OPEN_FILES ? kind : CEE_IO_OTHER];
    cee_usize length = 0; /* a C string in memory: its length fits the address space */
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

void *ceesafe_vec_grow(void *ptr, uint64_t len, cee_usize *cap, uint64_t extra, uint64_t size, uint32_t allocator) {
    if (extra > UINT64_MAX - len) return NULL;
    uint64_t need = len + extra;
    uint64_t have = (uint64_t)*cap;
    if (need <= have) return ptr;
    uint64_t next = have > UINT64_MAX / 2u ? UINT64_MAX : have * 2u;
    if (next < 4u) next = 4u;
    if (next < need) next = need;
    if (size == 0u) return NULL;
    /* At most what the address space holds (a 32-bit target's 4 GiB, ADR 0192), but never less than is needed. */
    uint64_t most = (uint64_t)SIZE_MAX / size;
    if (next > most) next = most;
    if (next < need) return NULL;
    /* A first buffer is allocated; a later one is reallocated, which keeps the old block on failure. */
    void *fresh = *cap == 0u ? ceesafe_alloc((size_t)(next * size), allocator) : ceesafe_realloc(ptr, (size_t)(next * size), allocator);
    if (fresh == NULL) return NULL;
    *cap = (cee_usize)next; /* next * size fits size_t, so next fits cee_usize */
    return fresh;
}

/* A first buffer of at least `count` elements, every byte zero (ADR 0099): what ceesafe_vec_grow gives a vector with
   no buffer, from the zeroed allocation. One attempt and one live block, as there. */
void *ceesafe_vec_zeroed(cee_usize *cap, uint64_t count, uint64_t size, uint32_t allocator) {
    uint64_t next = count < 4u ? 4u : count;
    if (size == 0u || next > (uint64_t)SIZE_MAX / size) return NULL;
    void *fresh = ceesafe_alloc_zeroed((size_t)(next * size), allocator);
    if (fresh == NULL) return NULL;
    *cap = (cee_usize)next;
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
static void write_err_u64(uint64_t value);

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
    if (info->isolated_task != 0u) {
        write_err_cstr(" (isolated task ");
        write_err_u64(info->isolated_task);
        write_err_cstr(": ended alone)");
    }
    write_err_cstr("\n");
}

/* ---- Isolated tasks (ADR 0175): what the panic path and the generated entries from C need of them ----
 * ceesafe_units_live counts the isolation units that exist (ceesafe_atomic_*); while it is 0, an entry from C costs
 * what it cost. ceesafe_entry_counted keeps, for this thread, how many entries from C are on its stack (never below
 * 0: an entry made before any unit existed is not counted, and is outside every executor that runs a unit's task).
 * ceesafe_panic_isolate is the executor's: given a panic, it returns if the panic is not one an isolated task ends
 * alone for, and otherwise does not return. ceesafe_panic_notify is what it uses to report such a panic: the hook,
 * and the line. */
uint32_t ceesafe_units_live;
static _Thread_local int32_t cee_entry_depth;
void (*ceesafe_panic_isolate)(const ceesafe_panic_info *info);

void ceesafe_entry_counted(int32_t by) {
    int32_t depth = cee_entry_depth + by;
    cee_entry_depth = depth < 0 ? 0 : depth;
}

int32_t ceesafe_entry_depth(void) { return cee_entry_depth; }

void ceesafe_panic_notify(const ceesafe_panic_info *info) {
    if ((rt_initializing != 0u || ceesafe_atomic_load_u32(&rt_state) == CEE_RT_STARTED) && rt_panic_hook != NULL) {
        rt_panic_hook(rt_panic_context, info);
    }
    write_panic_line(info);
}

/* The one path every hosted panic takes: the hook on the thread's first panic, if ceesafe_init installed one; then,
   if the hook returns, or on a panic inside it, the printed line and the abort. */
static _Noreturn void panic_end(const ceesafe_panic_info *info) {
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

/* A panic of the program's: an isolated task's ends that task (ADR 0175), any other ends the process. */
static _Noreturn void panic_report(const ceesafe_panic_info *info) {
    if (ceesafe_panic_isolate != NULL && ceesafe_thread_panicked == 0u && info->kind != (uint32_t)CEE_PANIC_ASYNC &&
        info->kind != (uint32_t)CEE_PANIC_ALLOCATOR) {
        ceesafe_panic_isolate(info); /* returns if this one is not an isolated task's */
    }
    panic_end(info);
}

_Noreturn void ceesafe_panic(cee_panic_kind kind, cee_loc loc) {
    ceesafe_panic_info info = {(uint32_t)kind, ceesafe_panic_reason(kind), NULL, 0u, loc.file, loc.line, loc.column, 0u};
    panic_report(&info);
}

#if defined(CEE_POLLS)
/* Polls (ADR 0186): each thread's poll word, which only the async runtime's monitor sets, and the slow path it
   installs before it sets one. */
_Thread_local uint32_t ceesafe_poll_word;
uint32_t ceesafe_poll_any;
void (*ceesafe_poll_hook)(const cee_loc *loc, bool may_pause);

/* A word set while it was 0 counts one more in ceesafe_poll_any; one cleared while it was set, one fewer. A clear that
   comes between the monitor's set and its count leaves the count a moment above 0 (or wrapped below it), which sends
   polls to the slow path for nothing, never past a task that is overdue. */
void ceesafe_poll_set(uint32_t *word, uint32_t epoch) {
    if (ceesafe_atomic_swap_u32(word, epoch) == 0u) {
        (void)ceesafe_atomic_add_u32(&ceesafe_poll_any, 1u);
    }
}

void ceesafe_poll_clear(void) {
    if (ceesafe_atomic_load_ord_u32(&ceesafe_poll_word, 0) != 0u && ceesafe_atomic_swap_u32(&ceesafe_poll_word, 0u) != 0u) {
        (void)ceesafe_atomic_add_u32(&ceesafe_poll_any, 0xFFFFFFFFu);
    }
}

static void cee_poll_slow(const cee_loc *loc, bool may_pause) {
    if (ceesafe_atomic_load_ord_u32(&ceesafe_poll_word, 0) == 0u) {
        return; /* another thread's task is overdue */
    }
    void (*hook)(const cee_loc *loc, bool may_pause) = ceesafe_poll_hook;
    if (hook != NULL) {
        hook(loc, may_pause);
    } else {
        ceesafe_poll_clear();
    }
}

void ceesafe_poll_slow(const cee_loc *loc) { cee_poll_slow(loc, true); }

/* A poll in a function that calls a C function itself: its task may be ended, never paused (ADR 0189 §2.3). */
void ceesafe_poll_slow_end(const cee_loc *loc) { cee_poll_slow(loc, false); }
#endif

/* A runtime panic of the program's kind, with its own text and place: an isolated task's ends that task (ADR 0186's
   overrun). */
_Noreturn void ceesafe_panic_with(cee_panic_kind kind, const uint8_t *message, size_t length, cee_loc loc) {
    ceesafe_panic_info info = {(uint32_t)kind, ceesafe_panic_reason(kind), message, length, loc.file, loc.line,
                               loc.column, 0u};
    panic_report(&info);
}

_Noreturn void ceesafe_panic_poisoned(void) {
    ceesafe_panic_info info = {(uint32_t)CEE_PANIC_POISONED, ceesafe_panic_reason(CEE_PANIC_POISONED), NULL, 0u, NULL, 0u,
                               0u, 0u};
    panic_report(&info);
}

/* The program's text in place of the reason (ADR 0075 §2.8). */
_Noreturn void ceesafe_panic_text(cee_str message, cee_loc loc) {
    static const uint8_t empty = 0u;
    ceesafe_panic_info info = {(uint32_t)CEE_PANIC_EXPLICIT, ceesafe_panic_reason(CEE_PANIC_EXPLICIT),
                               message.ptr != NULL ? message.ptr : &empty, (size_t)message.len, loc.file, loc.line,
                               loc.column, 0u};
    panic_report(&info);
}

_Noreturn void ceesafe_panic_runtime(cee_panic_kind kind, const char *message) {
    size_t length = 0;
    while (message[length] != '\0') {
        length++;
    }
    ceesafe_panic_info info = {(uint32_t)kind, ceesafe_panic_reason(kind), (const uint8_t *)message, length, NULL, 0u,
                               0u, 0u};
    panic_end(&info); /* the runtime's own: never an isolated task's (ADR 0175 §2.3) */
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
static uint64_t alloc_isolated; /* blocks crashed isolated units reach at the end (ADR 0175 amendment 2) */
static uint64_t alloc_given;      /* blocks a host adopted (Vector.intoForeign, ADR 0090): no longer in alloc_live */
static uint64_t alloc_taken;      /* blocks taken from a host (Vector.fromForeign): now in alloc_live */
static uint32_t alloc_moved_used; /* a block changed sides: the report gives both counts (ceesafe_atomic_*) */
static uint64_t alloc_fail_at;    /* CEESAFE_FAIL_ALLOC_AT: this attempt fails; 0 = never */
static uint32_t alloc_report;     /* CEESAFE_ALLOC_REPORT: print the counters at the end of main */
/* Whether to count at all (ADR 0059): only a report, CEESAFE_FAIL_ALLOC_AT or `ceesafec test` reads the counters. Decided
   before the program's first allocation and never changed afterwards, so a free never subtracts what its allocation
   did not add. */
static uint32_t alloc_counting;

/* The three above are written once, by ceesafe_init or ceesafe_test_begin, and read on every allocation, through these:
   relaxed atomic accesses, a plain load or store on every target (a volatile one under MSVC, whose aligned volatile
   accesses are not torn). The generated main's ceesafe_init may run while a thread a C constructor started allocates
   (ceesafe_rt_init_main tolerates a constructor's allocations); plain accesses were a data race there (ADR 0088 §8).
   Such a thread may see counting turn on between an allocation and its free, which only the report would notice. */
static inline uint32_t config_u32(const uint32_t *p) {
#if defined(_MSC_VER) && !defined(__clang__)
    return *(volatile const uint32_t *)p;
#else
    return __atomic_load_n(p, __ATOMIC_RELAXED);
#endif
}

static inline uint64_t config_u64(const uint64_t *p) {
#if defined(_MSC_VER) && !defined(__clang__)
    return *(volatile const uint64_t *)p;
#else
    return __atomic_load_n(p, __ATOMIC_RELAXED);
#endif
}

static inline void config_set_u32(uint32_t *p, uint32_t v) {
#if defined(_MSC_VER) && !defined(__clang__)
    *(volatile uint32_t *)p = v;
#else
    __atomic_store_n(p, v, __ATOMIC_RELAXED);
#endif
}

static inline void config_set_u64(uint64_t *p, uint64_t v) {
#if defined(_MSC_VER) && !defined(__clang__)
    *(volatile uint64_t *)p = v;
#else
    __atomic_store_n(p, v, __ATOMIC_RELAXED);
#endif
}
#ifndef CEE_FREESTANDING
/* Whether rt.h's inline allocation may not be used (ADR 0101), by reason: zero when by none. It starts with the
   platform's reason, which the platform functions clear at their first block from the C library. */
uint32_t ceesafe_alloc_watched = CEE_ALLOC_WATCH_PLATFORM;

void ceesafe_alloc_watch(uint32_t bit, bool on) {
#if defined(_MSC_VER) && !defined(__clang__)
    if (on) {
        (void)_InterlockedOr((volatile long *)&ceesafe_alloc_watched, (long)bit);
    } else {
        (void)_InterlockedAnd((volatile long *)&ceesafe_alloc_watched, ~(long)bit);
    }
#else
    if (on) {
        (void)__atomic_fetch_or(&ceesafe_alloc_watched, bit, __ATOMIC_RELAXED);
    } else {
        (void)__atomic_fetch_and(&ceesafe_alloc_watched, ~bit, __ATOMIC_RELAXED);
    }
#endif
}
#endif

/* Sets whether allocations are counted, and with it whether they may bypass this file (ADR 0101): every write of
   alloc_counting. An attempt can fail only when counting (alloc_fail_at needs the attempt's number). */
static void set_alloc_counting(uint32_t counting);

/* FailingAllocator's countdown (type-system.md §18.1): process-wide, and changed from every thread, so atomic
   (ceesafe_atomic_*). failAfter stores `failing_left` before `failing_armed`, so an allocation that sees the flag sees
   the count it was armed with. */
static uint32_t failing_armed;    /* FailingAllocator.failAfter has been called, reset has not */
static uint64_t failing_left;     /* successes FailingAllocator still allows */

/* `ceesafec test`'s harness (ADR 0049): the allocation report on, and the sweeps' points from its arguments rather than
   the environment (0: none). */
uint64_t ceesafe_test_cancel_at;
bool ceesafe_test_reports;

static void set_alloc_counting(uint32_t counting) {
#ifndef CEE_FREESTANDING
    if (counting != 0u) {
        ceesafe_alloc_watch(CEE_ALLOC_WATCH_COUNTED, true); /* before the counters are read */
    }
#endif
    config_set_u32(&alloc_counting, counting);
#ifndef CEE_FREESTANDING
    if (counting == 0u) {
        ceesafe_alloc_watch(CEE_ALLOC_WATCH_COUNTED, false);
    }
#endif
}

void ceesafe_test_begin(uint64_t fail_alloc_at, uint64_t cancel_at) {
    config_set_u32(&alloc_report, 1u);
    set_alloc_counting(1u);
    config_set_u64(&alloc_fail_at, fail_alloc_at);
    ceesafe_test_cancel_at = cancel_at;
    ceesafe_test_reports = true;
}

/*
 * The counters change from every thread of a threaded program (type-system.md §38), so atomically: every attempt
 * gets its own number, in the order of the increments, and CEESAFE_FAIL_ALLOC_AT fails exactly one. The configuration
 * (alloc_counting, alloc_fail_at, alloc_report) is written only by ceesafe_init and ceesafe_test_begin, which run
 * before any thread calls CeeSafe (ADR 0088 §2.2's ordering contract), a thread's start ordering them before everything
 * it does; its accesses are relaxed atomics all the same (config_u32, above), for a C constructor's thread.
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
    uint64_t attempt = config_u32(&alloc_counting) != 0u ? CEE_COUNT(alloc_attempts, 1u) : 0u;
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
    uint64_t fail_at = config_u64(&alloc_fail_at);
    return fail_at != 0u && attempt == fail_at;
}

/* One more live block (or one fewer, with UINT64_MAX) when counting, in C's count for CEE_ALLOC_FOREIGN (ADR 0060) and
   CEE_ALLOC_FOREIGN_INIT (ADR 0061), one count for both: a block may be freed under either. */
static void alloc_count_live(uint32_t allocator, uint64_t delta) {
    if ((allocator == CEE_ALLOC_FOREIGN || allocator == CEE_ALLOC_FOREIGN_INIT) && config_u32(&alloc_counting) != 0u) {
        (void)CEE_COUNT(alloc_foreign, delta);
        if (ceesafe_atomic_load_u32(&alloc_foreign_used) == 0u) {
            ceesafe_atomic_store_u32(&alloc_foreign_used, 1u);
        }
    } else if (config_u32(&alloc_counting) != 0u) {
        (void)CEE_COUNT(alloc_live, delta);
    }
}

#ifndef CEE_FREESTANDING
/* ---- Allocation budgets (ADR 0170; rt.h says what each function does) ----
 * A budget is a record of a limit and of the bytes held under it, inside the budget that was current when it was
 * made. The thread's current budget is the running task's (the executor swaps it around each resume) or, outside a
 * task, the one entered last on the thread. A record lives while something names it: its guard, a budget inside
 * it, a task that was made under it. The counts are atomic: a block may be freed on another thread than it was
 * allocated on. Only CeeSafe's own allocators' blocks are counted, not what C libraries ask for through
 * CEE_ALLOC_FOREIGN. */
typedef struct cee_budget {
    uint64_t limit;
    uint64_t held;
    uint32_t refs;
    struct cee_budget *parent;
} cee_budget;

static _Thread_local cee_budget *cee_budget_now;
static uint32_t cee_budgets_live; /* records that exist (ceesafe_atomic_*): rt.h's inline path is off while any does */

static bool budget_counts(uint32_t allocator) {
    return allocator == CEE_ALLOC_SYSTEM || allocator == CEE_ALLOC_FAILING;
}

/* `bytes` off what `b` and the budgets it is inside hold, never below zero. */
static void budget_give_back(cee_budget *b, uint64_t bytes) {
    for (; b != NULL; b = b->parent) {
        uint64_t held = ceesafe_atomic_load_u64(&b->held);
        for (;;) {
            uint64_t seen = ceesafe_atomic_cas_u64(&b->held, held, held > bytes ? held - bytes : 0u);
            if (seen == held) {
                break;
            }
            held = seen;
        }
    }
}

/* `bytes` onto what `b` and the budgets it is inside hold; false, with nothing added, if one of them would pass its
   limit. `forced` adds whatever the limits say: for what is already held (a block that grew more than was asked). */
static bool budget_take(cee_budget *b, uint64_t bytes, bool forced) {
    for (cee_budget *k = b; k != NULL; k = k->parent) {
        uint64_t held = ceesafe_atomic_load_u64(&k->held);
        for (;;) {
            if (!forced && (bytes > k->limit || held > k->limit - bytes)) {
                for (cee_budget *taken = b; taken != k; taken = taken->parent) {
                    uint64_t had = ceesafe_atomic_load_u64(&taken->held);
                    for (;;) {
                        uint64_t saw = ceesafe_atomic_cas_u64(&taken->held, had, had > bytes ? had - bytes : 0u);
                        if (saw == had) {
                            break;
                        }
                        had = saw;
                    }
                }
                return false;
            }
            uint64_t seen = ceesafe_atomic_cas_u64(&k->held, held, held + bytes);
            if (seen == held) {
                break;
            }
            held = seen;
        }
    }
    return true;
}

static void budget_unname(cee_budget *b) {
    while (b != NULL && ceesafe_atomic_add_u32(&b->refs, UINT32_MAX) == 1u) { /* minus one: it was the last name */
        cee_budget *parent = b->parent;
        ceesafe_platform_free(b);
        if (ceesafe_atomic_add_u32(&cee_budgets_live, UINT32_MAX) == 1u) {
            ceesafe_alloc_watch(CEE_ALLOC_WATCH_BUDGET, false);
        }
        b = parent;
    }
}

void *ceesafe_budget_enter(uint64_t limit) {
    if (!ceesafe_platform_block_sizes()) {
        return NULL;
    }
    cee_budget *b = (cee_budget *)ceesafe_platform_alloc(sizeof *b); /* not counted: the record is the runtime's */
    if (b == NULL) {
        return NULL;
    }
    b->limit = limit;
    b->held = 0u;
    b->refs = 1u;
    b->parent = cee_budget_now;
    if (b->parent != NULL) {
        (void)ceesafe_atomic_add_u32(&b->parent->refs, 1u);
    }
    if (ceesafe_atomic_add_u32(&cee_budgets_live, 1u) == 0u) {
        ceesafe_alloc_watch(CEE_ALLOC_WATCH_BUDGET, true);
    }
    cee_budget_now = b;
    return b;
}

/* A budget of `limit` bytes inside the current one, not made current: an isolated task's own (ADR 0175), which the
   executor makes the task's. NULL as ceesafe_budget_enter. ceesafe_budget_held_of is what such a record holds. */
void *ceesafe_budget_make(uint64_t limit) {
    cee_budget *outer = cee_budget_now;
    void *made = ceesafe_budget_enter(limit);
    cee_budget_now = outer;
    return made;
}

uint64_t ceesafe_budget_held_of(void *budget) {
    return budget == NULL ? 0u : ceesafe_atomic_load_u64(&((cee_budget *)budget)->held);
}

void ceesafe_budget_leave(void *budget) {
    cee_budget *b = (cee_budget *)budget;
    if (b == NULL) {
        return;
    }
    if (cee_budget_now == b) {
        cee_budget_now = b->parent; /* else its guard ended elsewhere than it was made: nothing is current through it */
    }
    budget_unname(b);
}

int64_t ceesafe_budget_held(void) {
    return cee_budget_now == NULL ? -1 : (int64_t)ceesafe_atomic_load_u64(&cee_budget_now->held);
}

int64_t ceesafe_budget_limit(void) {
    return cee_budget_now == NULL ? -1 : (int64_t)cee_budget_now->limit;
}

void *ceesafe_budget_retain(void) {
    if (cee_budget_now != NULL) {
        (void)ceesafe_atomic_add_u32(&cee_budget_now->refs, 1u);
    }
    return cee_budget_now;
}

void *ceesafe_budget_swap(void *next) {
    void *before = cee_budget_now;
    cee_budget_now = (cee_budget *)next;
    return before;
}

void ceesafe_budget_release(void *budget) { budget_unname((cee_budget *)budget); }

/* The current budget, if this allocator's blocks are counted. */
static cee_budget *budget_for(uint32_t allocator) {
    return cee_budget_now != NULL && budget_counts(allocator) ? cee_budget_now : NULL;
}

/* A fresh block under the current budget: the block, or NULL with the block freed if the budget has no room. */
static void *budget_new_block(void *p, uint32_t allocator) {
    cee_budget *b = p == NULL ? NULL : budget_for(allocator);
    if (b != NULL && !budget_take(b, (uint64_t)ceesafe_platform_block_size(p), false)) {
        ceesafe_platform_free(p);
        return NULL;
    }
    return p;
}

#else
/* Freestanding: no budgets (ADR 0170 is the hosted runtime's). The allocator below is the same code, with nothing
   to count. */
typedef struct cee_budget cee_budget;
static cee_budget *budget_for(uint32_t allocator) { (void)allocator; return NULL; }
static void *budget_new_block(void *p, uint32_t allocator) { (void)allocator; return p; }
static bool budget_take(cee_budget *b, uint64_t bytes, bool forced) { (void)b; (void)bytes; (void)forced; return true; }
static void budget_give_back(cee_budget *b, uint64_t bytes) { (void)b; (void)bytes; }
#define ceesafe_platform_block_size(p) ((size_t)0)
#endif

/* ---- What a crashed isolated unit leaves (ADR 0175 amendment 2) ----
 * Only while allocations are counted (`ceesafec test`, CEESAFE_ALLOC_REPORT, CEESAFE_FAIL_ALLOC_AT): a table of the
 * live blocks counted in `live` (their addresses and the sizes asked for), and the task blocks of the units that
 * crashed. At the end, every live block those reach, through any 8-byte-aligned word that holds the address of a
 * live block or of a byte inside one, is counted as `isolated leaked`, which `ceesafec test` takes off `live`. A
 * unit holds no loan (N0349): what it reaches it owns, or holds a count of, and keeps for ever. The walk is
 * conservative, so a stale word may add a block; it never misses one the unit reaches through its words. Nothing of
 * this runs where allocations are not counted. The table's own memory is the C library's, never a host's allocator
 * hook's (ADR 0088 §2.5: the host sees only CeeSafe's blocks), and is given back at the end. */
#ifndef CEE_FREESTANDING
#include <stdlib.h>
typedef struct cee_live_block {
    uintptr_t at;  /* 0: empty; 1: removed */
    size_t size;
} cee_live_block;

static cee_live_block *live_table;
static size_t live_capacity;  /* a power of two, or 0 */
static size_t live_used;      /* entries that are not empty: blocks and removed ones */
static size_t live_blocks;
static const void **dead_roots;
static size_t dead_count;
static size_t dead_capacity;
static uint32_t live_lock; /* ceesafe_atomic_*: allocations come from every thread */

static void live_take(void) {
    while (ceesafe_atomic_cas_u32(&live_lock, 0u, 1u) != 0u) {
    }
}

static void live_give(void) { ceesafe_atomic_store_u32(&live_lock, 0u); }

static size_t live_slot(uintptr_t at, size_t capacity) {
    uint64_t h = (uint64_t)at * 0x9E3779B97F4A7C15u;
    return (size_t)(h >> 17) & (capacity - 1u);
}

/* Room for one more; false if the platform heap has none, and then the table stays as it was. */
static bool live_room(void) {
    if (live_capacity != 0u && (live_used + 1u) * 2u <= live_capacity) {
        return true;
    }
    size_t capacity = live_capacity == 0u ? 1024u : (live_blocks * 4u > live_capacity ? live_capacity * 2u : live_capacity);
    cee_live_block *table = (cee_live_block *)calloc(capacity, sizeof(cee_live_block));
    if (table == NULL) {
        return false;
    }
    for (size_t i = 0; i < live_capacity; i++) {
        if (live_table[i].at > 1u) {
            size_t k = live_slot(live_table[i].at, capacity);
            while (table[k].at != 0u) k = (k + 1u) & (capacity - 1u);
            table[k] = live_table[i];
        }
    }
    if (live_table != NULL) free(live_table);
    live_table = table;
    live_capacity = capacity;
    live_used = live_blocks;
    return true;
}

static void live_track(const void *p, size_t size, uint32_t allocator) {
    if (p == NULL || config_u32(&alloc_counting) == 0u || allocator == CEE_ALLOC_FOREIGN ||
        allocator == CEE_ALLOC_FOREIGN_INIT) {
        return;
    }
    live_take();
    if (live_room()) {
        size_t k = live_slot((uintptr_t)p, live_capacity);
        while (live_table[k].at > 1u) k = (k + 1u) & (live_capacity - 1u);
        if (live_table[k].at == 0u) live_used += 1u;
        live_table[k].at = (uintptr_t)p;
        live_table[k].size = size == 0u ? 1u : size;
        live_blocks += 1u;
    }
    live_give();
}

static void live_untrack(const void *p) {
    if (p == NULL || config_u32(&alloc_counting) == 0u) {
        return;
    }
    live_take();
    if (live_capacity == 0u) {
        live_give();
        return;
    }
    size_t k = live_slot((uintptr_t)p, live_capacity);
    while (live_table[k].at != 0u) {
        if (live_table[k].at == (uintptr_t)p) {
            live_table[k].at = 1u;
            live_blocks -= 1u;
            break;
        }
        k = (k + 1u) & (live_capacity - 1u);
    }
    live_give();
}

void ceesafe_alloc_dead_root(const void *block) {
    if (block == NULL || config_u32(&alloc_counting) == 0u) {
        return;
    }
    live_take();
    if (dead_count == dead_capacity) {
        size_t capacity = dead_capacity == 0u ? 16u : dead_capacity * 2u;
        const void **roots = (const void **)realloc((void *)dead_roots, capacity * sizeof(void *));
        if (roots == NULL) {
            live_give();
            return; /* uncounted: the runner may report what this unit reaches as a leak */
        }
        dead_roots = roots;
        dead_capacity = capacity;
    }
    dead_roots[dead_count++] = block;
    live_give();
}

/* The live block that holds address `a` in `sorted` (by address), or SIZE_MAX. */
static size_t live_holder(const cee_live_block *sorted, size_t n, uintptr_t a) {
    size_t lo = 0u, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;
        if (sorted[mid].at <= a) lo = mid + 1u; else hi = mid;
    }
    if (lo == 0u) return SIZE_MAX;
    const cee_live_block *b = &sorted[lo - 1u];
    return a < b->at + b->size ? lo - 1u : SIZE_MAX;
}

/* Sorts by address: a heap sort, with no C library. */
static void live_sift(cee_live_block *v, size_t root, size_t n) {
    for (;;) {
        size_t child = root * 2u + 1u;
        if (child >= n) return;
        if (child + 1u < n && v[child + 1u].at > v[child].at) child += 1u;
        if (v[root].at >= v[child].at) return;
        cee_live_block t = v[root];
        v[root] = v[child];
        v[child] = t;
        root = child;
    }
}

/* How many live blocks the crashed units reach; 0 if none crashed or there is no memory to look. */
static uint64_t live_dead_reach(void) {
    live_take();
    uint64_t found = 0u;
    if (dead_count != 0u && live_blocks != 0u) {
        size_t n = live_blocks;
        cee_live_block *sorted = (cee_live_block *)malloc(n * sizeof(cee_live_block));
        uint8_t *seen = (uint8_t *)calloc(n, 1u);
        size_t *work = (size_t *)malloc(n * sizeof(size_t));
        if (sorted != NULL && seen != NULL && work != NULL) {
            size_t m = 0u;
            for (size_t i = 0; i < live_capacity && m < n; i++) {
                if (live_table[i].at > 1u) sorted[m++] = live_table[i];
            }
            for (size_t i = m / 2u; i > 0u; i--) live_sift(sorted, i - 1u, m);
            for (size_t end = m; end > 1u; end--) {
                cee_live_block t = sorted[0];
                sorted[0] = sorted[end - 1u];
                sorted[end - 1u] = t;
                live_sift(sorted, 0u, end - 1u);
            }
            size_t top = 0u;
            for (size_t r = 0; r < dead_count; r++) {
                size_t k = live_holder(sorted, m, (uintptr_t)dead_roots[r]);
                if (k != SIZE_MAX && seen[k] == 0u) {
                    seen[k] = 1u;
                    work[top++] = k;
                }
            }
            while (top > 0u) {
                const cee_live_block *b = &sorted[work[--top]];
                found += 1u;
                /* Pointer-aligned words: 8 bytes apart on a 64-bit target, 4 on a 32-bit one (ADR 0192 §2.5). */
                const uintptr_t step = (uintptr_t)sizeof(uintptr_t);
                uintptr_t first = (b->at + step - 1u) & ~(step - 1u);
                for (uintptr_t a = first; a + sizeof(uintptr_t) <= b->at + b->size; a += step) {
                    uintptr_t word;
                    ceesafe_mem_copy(&word, (const void *)a, sizeof word);
                    size_t k = live_holder(sorted, m, word);
                    if (k != SIZE_MAX && seen[k] == 0u) {
                        seen[k] = 1u;
                        work[top++] = k;
                    }
                }
            }
        }
        free(sorted);
        free(seen);
        free(work);
    }
    /* The end: the table and the roots go back to the C library. Blocks freed after this are not looked for. */
    free(live_table);
    free((void *)dead_roots);
    live_table = NULL;
    live_capacity = 0u;
    live_used = 0u;
    live_blocks = 0u;
    dead_roots = NULL;
    dead_count = 0u;
    dead_capacity = 0u;
    live_give();
    return found;
}
#else
static void live_track(const void *p, size_t size, uint32_t allocator) { (void)p; (void)size; (void)allocator; }
static void live_untrack(const void *p) { (void)p; }
static uint64_t live_dead_reach(void) { return 0u; }
void ceesafe_alloc_dead_root(const void *block) { (void)block; }
#endif

void *ceesafe_alloc(size_t size, uint32_t allocator) {
    if (alloc_refused(allocator)) {
        return NULL;
    }
    void *p = budget_new_block(ceesafe_platform_alloc(size == 0u ? 1u : size), allocator);
    if (p != NULL && config_u32(&alloc_counting) != 0u) {
        alloc_count_live(allocator, 1u);
        live_track(p, size, allocator);
    }
    return p;
}

void *ceesafe_alloc_zeroed(size_t size, uint32_t allocator) {
    if (alloc_refused(allocator)) {
        return NULL;
    }
    void *p = budget_new_block(ceesafe_platform_alloc_zeroed(size == 0u ? 1u : size), allocator);
    if (p != NULL && config_u32(&alloc_counting) != 0u) {
        alloc_count_live(allocator, 1u);
        live_track(p, size, allocator);
    }
    return p;
}

static void *realloc_block(void *p, size_t size, uint32_t allocator);

void *ceesafe_realloc(void *p, size_t size, uint32_t allocator) {
    if (p == NULL) {
        return ceesafe_alloc(size, allocator); /* C's realloc(NULL, n), for CEE_ALLOC_FOREIGN's callers */
    }
    void *q = realloc_block(p, size, allocator);
    if (q != NULL && config_u32(&alloc_counting) != 0u) {
        live_untrack(p); /* the table follows the block, which may have moved */
        live_track(q, size, allocator);
    }
    return q;
}

static void *realloc_block(void *p, size_t size, uint32_t allocator) {
    if (alloc_refused(allocator)) {
        return NULL;
    }
    cee_budget *b = budget_for(allocator);
    if (b == NULL) {
        return ceesafe_platform_realloc(p, size == 0u ? 1u : size);
    }
    /* What is asked beyond the block's size is taken first, so that a refusal leaves the block as it was. */
    uint64_t before = (uint64_t)ceesafe_platform_block_size(p);
    uint64_t asked = (uint64_t)size > before ? (uint64_t)size - before : 0u;
    if (asked != 0u && !budget_take(b, asked, false)) {
        return NULL;
    }
    void *q = ceesafe_platform_realloc(p, size == 0u ? 1u : size);
    if (q == NULL) {
        budget_give_back(b, asked);
        return NULL;
    }
    uint64_t after = (uint64_t)ceesafe_platform_block_size(q);
    if (after > before + asked) {
        (void)budget_take(b, after - before - asked, true);
    } else {
        budget_give_back(b, before + asked - after);
    }
    return q;
}

void ceesafe_free(void *p, uint32_t allocator) {
    /* every allocator frees to the platform heap */
    if (p == NULL) {
        return; /* C's free(NULL); CeeSafe's own code never frees NULL */
    }
    cee_budget *b = budget_for(allocator);
    if (b != NULL) {
        budget_give_back(b, (uint64_t)ceesafe_platform_block_size(p));
    }
    if (config_u32(&alloc_counting) != 0u) {
        alloc_count_live(allocator, UINT64_MAX); /* minus one */
        live_untrack(p);
    }
    ceesafe_platform_free(p);
}

/* A vector's block changes sides (ADR 0090 §2.3): out of `live` when the host adopts it, into `live` when CeeSafe
   takes one of the host's. Neither is an allocation attempt: they are not numbered and never failed. Each count has
   its own guard, written out, as alloc_counting_check asks of every CEE_COUNT. */
static void alloc_moved(void) {
    if (ceesafe_atomic_load_u32(&alloc_moved_used) == 0u) {
        ceesafe_atomic_store_u32(&alloc_moved_used, 1u);
    }
}

void ceesafe_alloc_given(void) {
    if (config_u32(&alloc_counting) != 0u) {
        (void)CEE_COUNT(alloc_live, UINT64_MAX); /* minus one */
    }
    if (config_u32(&alloc_counting) != 0u) {
        (void)CEE_COUNT(alloc_given, 1u);
        alloc_moved();
    }
}

void ceesafe_alloc_taken(void) {
    if (config_u32(&alloc_counting) != 0u) {
        (void)CEE_COUNT(alloc_live, 1u);
    }
    if (config_u32(&alloc_counting) != 0u) {
        (void)CEE_COUNT(alloc_taken, 1u);
        alloc_moved();
    }
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
    config_set_u64(&alloc_fail_at, fail_at);
    config_set_u32(&alloc_report, report ? 1u : 0u);
    set_alloc_counting(report || fail_at != 0u ? 1u : 0u);
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
   with CEE_ALLOC_FOREIGN, "ceesafe alloc: foreign held=<k>\n" (ADR 0060), which no tool reads as a leak; then, if
   isolated units trapped, "ceesafe alloc: isolated leaked=<k>\n" (ADR 0175 amendments 1 and 2), the live blocks they
   reach, which `live` counts and `ceesafec test` takes off it; then, if a vector's block changed sides, "ceesafe alloc: given=<k>
   taken=<m>\n" (ADR 0090), for information too. */
uint64_t ceesafe_shutdown(void) {
    /* Before ceesafe_init has published the config (never called, or still under way on another thread), nothing
       was asked for: 0. The load orders that config before the reads below. */
    if (ceesafe_atomic_load_u32(&rt_state) != CEE_RT_STARTED) {
        return 0u;
    }
    uint64_t live = config_u32(&alloc_counting) != 0u ? ceesafe_atomic_load_u64(&alloc_live) : 0u;
    if (config_u32(&alloc_counting) != 0u) {
        ceesafe_atomic_store_u64(&alloc_isolated, live_dead_reach()); /* ADR 0175 amendment 2 */
    }
    if (config_u32(&alloc_report) != 0u) {
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
        if (ceesafe_atomic_load_u64(&alloc_isolated) != 0u) {
            write_err_cstr("ceesafe alloc: isolated leaked=");
            write_err_u64(ceesafe_atomic_load_u64(&alloc_isolated));
            write_err_cstr("\n");
        }
        if (ceesafe_atomic_load_u32(&alloc_moved_used) != 0u) {
            write_err_cstr("ceesafe alloc: given=");
            write_err_u64(ceesafe_atomic_load_u64(&alloc_given));
            write_err_cstr(" taken=");
            write_err_u64(ceesafe_atomic_load_u64(&alloc_taken));
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
