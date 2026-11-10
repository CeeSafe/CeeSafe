/* ADR 0088's stage A contract tests: a C host of the embed package's archive (tools/embed_check.py). */
#include "ceesafe/embed.h"
#include "ceesafe_exports.h"

#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void hook_print(const ceesafe_panic_info *info, const char *who) {
    printf("hook %s kind=%u reason=%s file=%s line=%u column=%u message=%.*s\n", who, info->kind, info->reason,
           info->file != NULL ? info->file : "(null)", info->line, info->column,
           info->message != NULL ? (int)info->message_length : 6,
           info->message != NULL ? (const char *)info->message : "(null)");
    fflush(stdout);
}

static void hook_abort(void *context, const ceesafe_panic_info *info) {
    (void)context;
    hook_print(info, "abort");
    abort();
}

static void hook_returns(void *context, const ceesafe_panic_info *info) {
    (void)context;
    hook_print(info, "returns");
}

static void hook_reenters(void *context, const ceesafe_panic_info *info) {
    (void)context;
    hook_print(info, "reenters");
    uint8_t bytes[2] = {1, 2};
    printf("sum=%llu\n", (unsigned long long)embed_sum(bytes, 2)); /* must not run */
    fflush(stdout);
}

static jmp_buf escape_to;
static void hook_escapes(void *context, const ceesafe_panic_info *info) {
    (void)context;
    hook_print(info, "escapes");
    longjmp(escape_to, 1); /* undefined behaviour by ADR 0088 §2.4: what the panicked flag is for */
}

/* Two threads panicking at once: the hook records both under a lock, and the second prints them and aborts. */
static pthread_mutex_t record_lock = PTHREAD_MUTEX_INITIALIZER;
static char records[2][256];
static int recorded;
static void hook_records(void *context, const ceesafe_panic_info *info) {
    (void)context;
    pthread_mutex_lock(&record_lock);
    snprintf(records[recorded], sizeof records[0], "record kind=%u file=%s line=%u column=%u", info->kind,
             info->file != NULL ? info->file : "(null)", info->line, info->column);
    recorded++;
    if (recorded == 2) {
        printf("%s\n%s\n", records[0], records[1]);
        fflush(stdout);
        abort();
    }
    pthread_mutex_unlock(&record_lock);
    for (int k = 0; k < 500; k++) {
        usleep(10000); /* the other thread's report: Bun's sleep_forever_if_another_thread_is_crashing */
    }
    printf("only one record\n");
    fflush(stdout);
    abort();
}

/* Stage B's allocator hook (ADR 0088 §2.5). Each block has a 16-byte header with a tag, so a block that reaches the
   wrong heap is caught both ways: one of the hook's given to the C library's free is a pointer malloc never returned
   (ASan, or the C library, stops), and one of the C library's given to the hook's free has no tag (hook_free
   aborts). The calls are counted, and the context is checked to be the host's. */
#define HOOK_TAG 0x0088B10C0088B10Cull
static int hook_context;
static uint64_t hook_allocs, hook_frees, hook_bad;

static void *hook_tag(unsigned char *base) {
    if (base == NULL) return NULL;
    *(uint64_t *)(void *)base = HOOK_TAG;
    return base + 16;
}

static void hook_check(void *context, void *block) {
    if (context != &hook_context || ((uintptr_t)block & 15u) != 0u ||
        *(uint64_t *)(void *)((unsigned char *)block - 16) != HOOK_TAG) {
        __atomic_add_fetch(&hook_bad, 1, __ATOMIC_RELAXED);
        printf("hook_free: a block the hook did not allocate\n");
        fflush(stdout);
        abort();
    }
}

static void *hook_alloc(void *context, size_t size) {
    if (context != &hook_context || size == 0 || size > SIZE_MAX - 16) __atomic_add_fetch(&hook_bad, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&hook_allocs, 1, __ATOMIC_RELAXED);
    return hook_tag(malloc(16 + size));
}

static void *hook_alloc_zeroed(void *context, size_t size) {
    if (context != &hook_context || size == 0 || size > SIZE_MAX - 16) __atomic_add_fetch(&hook_bad, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&hook_allocs, 1, __ATOMIC_RELAXED);
    return hook_tag(calloc(1, 16 + size));
}

static void *hook_realloc(void *context, void *block, size_t size) {
    hook_check(context, block);
    if (size == 0 || size > SIZE_MAX - 16) __atomic_add_fetch(&hook_bad, 1, __ATOMIC_RELAXED);
    unsigned char *base = realloc((unsigned char *)block - 16, 16 + size);
    return base == NULL ? NULL : base + 16; /* the tag moved with the block; on failure the block is kept */
}

static void hook_free(void *context, void *block) {
    hook_check(context, block);
    *(uint64_t *)(void *)((unsigned char *)block - 16) = 0;
    __atomic_add_fetch(&hook_frees, 1, __ATOMIC_RELAXED);
    free((unsigned char *)block - 16);
}

static const ceesafe_allocator tagged = {hook_alloc, hook_alloc_zeroed, hook_realloc, hook_free, &hook_context};

/* malloc's contract kept exactly, but for one function (`odd_which`: "alloc", "alloc_zeroed" or "realloc"), whose
   blocks are 8-aligned and never 16-aligned (ADR 0088 §1.9's evidence); the others give 16-aligned blocks. Each block
   is base + offset (24, or 32 when aligned) of a malloc block, with its size and base in the 16 bytes before it. */
static const char *odd_which = "";

static void *odd_block(size_t size, const char *function, int zero) {
    size_t offset = strcmp(odd_which, function) == 0 ? 24 : 32;
    unsigned char *base = zero ? calloc(1, size + 32) : malloc(size + 32);
    if (base == NULL) return NULL;
    unsigned char *p = base + offset;
    memcpy(p - 16, &size, sizeof size);
    memcpy(p - 8, &base, sizeof base);
    return p;
}

static void *odd_alloc(void *context, size_t size) {
    (void)context;
    return odd_block(size, "alloc", 0);
}

static void *odd_alloc_zeroed(void *context, size_t size) {
    (void)context;
    return odd_block(size, "alloc_zeroed", 1);
}

static void odd_free(void *context, void *block) {
    (void)context;
    unsigned char *base;
    memcpy(&base, (unsigned char *)block - 8, sizeof base);
    free(base);
}

static void *odd_realloc(void *context, void *block, size_t size) {
    size_t old;
    memcpy(&old, (unsigned char *)block - 16, sizeof old);
    void *p = odd_block(size, "realloc", 0);
    if (p == NULL) return NULL;
    memcpy(p, block, old < size ? old : size);
    odd_free(context, block);
    return p;
}

static const ceesafe_allocator misaligned = {odd_alloc, odd_alloc_zeroed, odd_realloc, odd_free, NULL};

static int init_allocator(const ceesafe_allocator *allocator, ceesafe_panic_hook hook, uint32_t flags,
                          uint64_t fail_at) {
    ceesafe_config config;
    memset(&config, 0, sizeof config);
    config.size = (uint32_t)sizeof config;
    config.flags = flags;
    config.fail_alloc_at = fail_at;
    config.allocator = allocator;
    config.panic_hook = hook;
    return ceesafe_init(&config);
}

static void hook_counts(const char *label) {
    printf("%s allocs=%llu frees=%llu bad=%llu\n", label,
           (unsigned long long)__atomic_load_n(&hook_allocs, __ATOMIC_RELAXED),
           (unsigned long long)__atomic_load_n(&hook_frees, __ATOMIC_RELAXED),
           (unsigned long long)__atomic_load_n(&hook_bad, __ATOMIC_RELAXED));
    fflush(stdout);
}

static void __attribute__((noinline)) scribble(void) {
    volatile char junk[4096];
    memset((char *)junk, 'z', sizeof junk);
}

static int init_with(ceesafe_panic_hook hook, int argc, char **argv, uint32_t flags, uint64_t fail_at) {
    ceesafe_config config;
    memset(&config, 0, sizeof config);
    config.size = (uint32_t)sizeof config;
    config.flags = flags;
    config.argc = argc;
    config.argv = (const char *const *)argv;
    config.fail_alloc_at = fail_at;
    config.panic_hook = hook;
    int32_t r = ceesafe_init(&config);
    if (r != 0) {
        printf("ceesafe_init=%d\n", r);
    }
    return r;
}

static void *panic_thread(void *arg) {
    (void)arg;
    usleep(1000);
    printf("%u\n", embed_panic(9));
    return NULL;
}

static void *scratch_thread(void *arg) {
    uint64_t seed = (uint64_t)(uintptr_t)arg, wrong = 0;
    for (int k = 0; k < 200000; k++) {
        if (embed_scratch(seed) != 64 * seed + 64 * 63 / 2) {
            wrong++;
        }
    }
    printf("seed=%llu wrong=%llu\n", (unsigned long long)seed, (unsigned long long)wrong);
    return NULL;
}

static ceesafe_config race_config;
static int32_t race_results[2];
static void *race_thread(void *arg) {
    int k = (int)(intptr_t)arg;
    race_results[k] = ceesafe_init(&race_config);
    return NULL;
}

/* ceesafe_init from two components at once, as review found: each calls CeeSafe as soon as its call returns, 0 or
   AGAIN, so AGAIN must mean the config is published (ADR 0088 §2.2); with `shutter`, a third thread calls
   ceesafe_shutdown meanwhile, which must not read a config being written. Run under ThreadSanitizer. */
static volatile int init_go;
static void *init_component(void *arg) {
    (void)arg;
    while (!__atomic_load_n(&init_go, __ATOMIC_ACQUIRE)) {
    }
    ceesafe_config config;
    memset(&config, 0, sizeof config);
    config.size = (uint32_t)sizeof config;
    config.alloc_report = false;
    config.fail_alloc_at = 1000000; /* counting on, never reached */
    int32_t r = ceesafe_init(&config);
    uint64_t sum = 0;
    uint8_t bytes[3] = {1, 2, 3};
    for (int k = 0; k < 1000; k++) {
        sum += embed_sum(bytes, 3);
    }
    return (void *)(uintptr_t)(r == 0 ? sum : r == CEESAFE_INIT_AGAIN ? sum + 1 : 0);
}

static void *init_shutter(void *arg) {
    (void)arg;
    while (!__atomic_load_n(&init_go, __ATOMIC_ACQUIRE)) {
    }
    for (int k = 0; k < 50; k++) {
        (void)ceesafe_shutdown();
    }
    return NULL;
}

static void *sum_thread(void *arg) {
    (void)arg;
    uint8_t bytes[3] = {1, 2, 3};
    printf("other thread sum=%llu\n", (unsigned long long)embed_sum(bytes, 3));
    fflush(stdout);
    return NULL;
}

int main(int argc, char **argv) {
    const char *what = argc > 1 ? argv[1] : "run";
    if (strcmp(what, "run") == 0) {
        if (init_with(NULL, argc, argv, CEESAFE_INIT_ENVIRONMENT, 0) != 0) return 1;
        uint8_t bytes[3] = {1, 2, 3};
        printf("sum=%llu twice=%llu async=%llu bump=%llu,%llu\n", (unsigned long long)embed_sum(bytes, 3),
               (unsigned long long)embed_twice(21), (unsigned long long)embed_async(41),
               (unsigned long long)embed_bump(), (unsigned long long)embed_bump());
        uint32_t (*f)(uint32_t) = embed_callback();
        printf("callback=%u\n", f(4));
        fflush(stdout);
        printf("live=%llu\n", (unsigned long long)ceesafe_shutdown());
    } else if (strcmp(what, "noinit") == 0) {
        /* 0.27.0's behaviour: the environment ignored, no report, no arguments */
        embed_leak();
        printf("boxes=%llu args=%llu live=%llu\n", (unsigned long long)embed_boxes(5),
               (unsigned long long)embed_arg_count(), (unsigned long long)ceesafe_shutdown());
    } else if (strcmp(what, "noinit-panic") == 0) {
        fflush(stdout);
        printf("%u\n", embed_panic(9));
    } else if (strcmp(what, "boxes") == 0) {
        /* boxes K: fail_alloc_at = K through the config */
        if (init_with(NULL, 0, NULL, 0, strtoull(argv[2], NULL, 10)) != 0) return 1;
        printf("boxes=%llu\n", (unsigned long long)embed_boxes(5));
    } else if (strcmp(what, "report") == 0) {
        if (init_with(NULL, 0, NULL, CEESAFE_INIT_ENVIRONMENT, 0) != 0) return 1;
        embed_leak();
        printf("boxes=%llu\n", (unsigned long long)embed_boxes(5));
        fflush(stdout);
        printf("live=%llu\n", (unsigned long long)ceesafe_shutdown());
    } else if (strcmp(what, "late") == 0) {
        printf("boxes=%llu\n", (unsigned long long)embed_boxes(1));
        int32_t a = init_with(NULL, 0, NULL, 0, 1);
        int32_t b = init_with(NULL, 0, NULL, 0, 1);
        printf("late=%d,%d boxes=%llu\n", a, b, (unsigned long long)embed_boxes(5)); /* fail_alloc_at=1 not applied */
    } else if (strcmp(what, "late-quiet") == 0) {
        /* An allocation before ceesafe_init, which asks for no counting: 0, and the hook and argv are taken. */
        printf("boxes=%llu\n", (unsigned long long)embed_boxes(1));
        char *arguments[2] = {"prog", "one"};
        int32_t r = init_with(hook_abort, 2, arguments, 0, 0);
        printf("quiet=%d args=%llu\n", r, (unsigned long long)embed_arg_count());
        fflush(stdout);
        printf("%u\n", embed_panic(9)); /* the hook fires */
    } else if (strcmp(what, "late-env") == 0) {
        /* Counting asked for by the environment alone (CEESAFE_ALLOC_REPORT set in the host's), after an allocation:
           0, a warning on stderr, the hook and argv taken, and no report. */
        printf("boxes=%llu\n", (unsigned long long)embed_boxes(1));
        char *arguments[2] = {"prog", "one"};
        int32_t r = init_with(hook_abort, 2, arguments, CEESAFE_INIT_ENVIRONMENT, 0);
        printf("env=%d args=%llu live=%llu\n", r, (unsigned long long)embed_arg_count(),
               (unsigned long long)ceesafe_shutdown());
        fflush(stdout);
        printf("%u\n", embed_panic(9)); /* the hook fires */
    } else if (strcmp(what, "reserved") == 0) {
        /* The reserved bits are masked off: bit 31, the generated main's signal before, does not skip LATE. */
        printf("boxes=%llu\n", (unsigned long long)embed_boxes(1));
        int32_t r = init_with(NULL, 0, NULL, 0x80000000u | 0x40000000u, 1);
        printf("reserved=%d\n", r);
    } else if (strcmp(what, "misuse") == 0) {
        ceesafe_config config;
        memset(&config, 0, sizeof config);
        config.size = (uint32_t)sizeof config + 8u;
        int32_t larger = ceesafe_init(&config);
        config.size = 12u;
        int32_t smaller = ceesafe_init(&config);
        int32_t null = ceesafe_init(NULL);
        static const ceesafe_allocator allocator;
        config.size = (uint32_t)sizeof config;
        config.allocator = &allocator;
        int32_t hook = ceesafe_init(&config);
        config.allocator = NULL;
        int32_t first = ceesafe_init(&config);
        int32_t again = ceesafe_init(&config);
        printf("larger=%d smaller=%d null=%d allocator=%d first=%d again=%d\n", larger, smaller, null, hook, first,
               again);
    } else if (strcmp(what, "race") == 0) {
        race_config.size = (uint32_t)sizeof race_config;
        pthread_t a, b;
        pthread_create(&a, NULL, race_thread, (void *)0);
        pthread_create(&b, NULL, race_thread, (void *)1);
        pthread_join(a, NULL);
        pthread_join(b, NULL);
        printf("race=%d,%d\n", race_results[0] < race_results[1] ? race_results[0] : race_results[1],
               race_results[0] < race_results[1] ? race_results[1] : race_results[0]);
    } else if (strcmp(what, "init-race") == 0 || strcmp(what, "init-shutdown") == 0) {
        pthread_t a, b, s;
        bool shutter = strcmp(what, "init-shutdown") == 0;
        pthread_create(&a, NULL, init_component, NULL);
        pthread_create(&b, NULL, init_component, NULL);
        if (shutter) pthread_create(&s, NULL, init_shutter, NULL);
        __atomic_store_n(&init_go, 1, __ATOMIC_RELEASE);
        void *ra, *rb;
        pthread_join(a, &ra);
        pthread_join(b, &rb);
        if (shutter) pthread_join(s, NULL);
        uintptr_t x = (uintptr_t)ra, y = (uintptr_t)rb;
        printf("init sums=%lu,%lu live=%llu\n", (unsigned long)(x < y ? x : y), (unsigned long)(x < y ? y : x),
               (unsigned long long)ceesafe_shutdown());
    } else if (strcmp(what, "argv") == 0) {
        /* The host's argument strings, overwritten and freed after ceesafe_init. */
        char **copy = malloc(3 * sizeof *copy);
        copy[0] = strdup("prog");
        copy[1] = strdup("alpha");
        copy[2] = strdup("zz");
        if (init_with(NULL, 3, copy, 0, 0) != 0) return 1;
        for (int k = 0; k < 3; k++) {
            memset(copy[k], 'X', strlen(copy[k]));
            free(copy[k]);
        }
        free(copy);
        printf("args=%llu codes=%llu,%llu,%llu\n", (unsigned long long)embed_arg_count(),
               (unsigned long long)embed_arg_code(0), (unsigned long long)embed_arg_code(1),
               (unsigned long long)embed_arg_code(2));
    } else if (strcmp(what, "hook") == 0) {
        /* hook panic|thread|worker|callback|explicit */
        if (init_with(hook_abort, 0, NULL, 0, 0) != 0) return 1;
        const char *where = argv[2];
        fflush(stdout);
        if (strcmp(where, "panic") == 0) printf("%u\n", embed_panic(9));
        if (strcmp(where, "thread") == 0) printf("%u\n", embed_thread_panic(9));
        if (strcmp(where, "worker") == 0) printf("%u\n", embed_worker_panic(9));
        if (strcmp(where, "explicit") == 0) embed_explicit();
        if (strcmp(where, "callback") == 0) {
            uint32_t (*f)(uint32_t) = embed_callback();
            printf("%u\n", f(0));
        }
        printf("no panic\n");
    } else if (strcmp(what, "hook-returns") == 0) {
        if (init_with(hook_returns, 0, NULL, 0, 0) != 0) return 1;
        printf("%u\n", embed_panic(9));
    } else if (strcmp(what, "hook-reenters") == 0) {
        if (init_with(hook_reenters, 0, NULL, 0, 0) != 0) return 1;
        printf("%u\n", embed_panic(9));
    } else if (strcmp(what, "escape") == 0) {
        if (init_with(hook_escapes, 0, NULL, 0, 0) != 0) return 1;
        if (setjmp(escape_to) == 0) {
            printf("%u\n", embed_panic(9));
        }
        printf("escaped\n");
        fflush(stdout);
        pthread_t t;
        pthread_create(&t, NULL, sum_thread, NULL); /* another thread is not stopped */
        pthread_join(t, NULL);
        uint8_t bytes[3] = {1, 2, 3};
        printf("same thread sum=%llu\n", (unsigned long long)embed_sum(bytes, 3)); /* the flag: aborts */
    } else if (strcmp(what, "escape-built") == 0) {
        /* The re-entry line after an escape prints the first panic's message, built in a frame that is gone and
           whose stack the host has since reused. */
        if (init_with(hook_escapes, 0, NULL, 0, 0) != 0) return 1;
        if (setjmp(escape_to) == 0) {
            embed_built_panic(1);
        }
        scribble();
        uint8_t bytes[3] = {1, 2, 3};
        printf("same thread sum=%llu\n", (unsigned long long)embed_sum(bytes, 3)); /* the flag: aborts */
    } else if (strcmp(what, "two-panics") == 0) {
        if (init_with(hook_records, 0, NULL, 0, 0) != 0) return 1;
        pthread_t a, b;
        pthread_create(&a, NULL, panic_thread, NULL);
        pthread_create(&b, NULL, panic_thread, NULL);
        pthread_join(a, NULL);
        pthread_join(b, NULL);
    } else if (strcmp(what, "alloc-hook") == 0) {
        /* Every kind of block the runtime makes, through the tagged hook: CeeSafe's own (boxes, a thread's, the
           async executor's buffers and frames, a scratch frame's chunks), argv's copy, and the window layer's (run
           with CEESAFE_WINDOW_SCRIPT). The report counts CeeSafe's attempts beside the hook's. */
        char *arguments[2] = {"prog", "one"};
        ceesafe_config config;
        memset(&config, 0, sizeof config);
        config.size = (uint32_t)sizeof config;
        config.argc = 2;
        config.argv = (const char *const *)arguments;
        config.alloc_report = true;
        config.allocator = &tagged;
        config.panic_hook = hook_abort;
        if (ceesafe_init(&config) != 0) return 1;
        uint8_t bytes[3] = {1, 2, 3};
        printf("sum=%llu twice=%llu async=%llu boxes=%llu scratch=%llu window=%u args=%llu\n",
               (unsigned long long)embed_sum(bytes, 3), (unsigned long long)embed_twice(21),
               (unsigned long long)embed_async(41), (unsigned long long)embed_boxes(5),
               (unsigned long long)embed_scratch(1000), embed_window(), (unsigned long long)embed_arg_count());
        hook_counts("hook");
        printf("live=%llu\n", (unsigned long long)ceesafe_shutdown());
    } else if (strcmp(what, "alloc-fail") == 0) {
        /* alloc-fail K: fail_alloc_at = K with the hook. An injected failure never reaches it, so the hook's calls
           are the boxes made. */
        if (init_allocator(&tagged, NULL, 0, strtoull(argv[2], NULL, 10)) != 0) return 1;
        printf("boxes=%llu ", (unsigned long long)embed_boxes(5));
        hook_counts("hook");
    } else if (strcmp(what, "alloc-misaligned") == 0) {
        /* alloc-misaligned alloc|alloc_zeroed|realloc|init: that function gives 8-aligned blocks; the runtime's panic,
           naming it, reaches the panic hook before any misaligned access. `init`: alloc's, at argv's copy inside
           ceesafe_init. */
        const char *which = argv[2];
        odd_which = strcmp(which, "init") == 0 ? "alloc" : which;
        char *arguments[2] = {"prog", "one"};
        ceesafe_config config;
        memset(&config, 0, sizeof config);
        config.size = (uint32_t)sizeof config;
        config.argc = strcmp(which, "init") == 0 ? 2 : 0;
        config.argv = (const char *const *)arguments;
        config.allocator = &misaligned;
        config.panic_hook = hook_abort;
        int32_t r = ceesafe_init(&config);
        printf("init=%d\n", r);
        fflush(stdout);
        if (strcmp(which, "alloc") == 0) printf("boxes=%llu\n", (unsigned long long)embed_boxes(1));
        if (strcmp(which, "alloc_zeroed") == 0) printf("window=%u\n", embed_window());
        if (strcmp(which, "realloc") == 0) printf("grow=%llu\n", (unsigned long long)embed_grow(100));
    } else if (strcmp(what, "alloc-window") == 0) {
        /* The window layer's own blocks, exactly: a scripted window opened, run and closed through the hook, with no
           counting, so every hook call here is the window's (CeeSafe's side of Window.open allocates nothing). */
        if (init_allocator(&tagged, hook_abort, 0, 0) != 0) return 1;
        uint32_t events = embed_window();
        printf("window=%u ", events);
        hook_counts("hook");
    } else if (strcmp(what, "alloc-late") == 0) {
        /* An allocator after the runtime has allocated: LATE, nothing changed; then the C library's heap goes on, and
           the hook is never called. */
        printf("boxes=%llu\n", (unsigned long long)embed_boxes(1));
        int32_t late = init_allocator(&tagged, NULL, 0, 0);
        int32_t plain = init_allocator(NULL, NULL, 0, 0);
        printf("alloc-late=%d,%d boxes=%llu ", late, plain, (unsigned long long)embed_boxes(3));
        hook_counts("hook");
    } else if (strcmp(what, "alloc-partial") == 0) {
        /* A hook missing one function: ALLOCATOR, nothing changed. */
        ceesafe_allocator partial = tagged;
        partial.free = NULL;
        int32_t r = init_allocator(&partial, NULL, 0, 0);
        int32_t again = init_allocator(&tagged, NULL, 0, 0);
        printf("partial=%d then=%d boxes=%llu ", r, again, (unsigned long long)embed_boxes(2));
        hook_counts("hook");
    } else if (strcmp(what, "threads") == 0) {
        /* threads [hook]: with `hook`, every block of both threads through the tagged allocator hook */
        if (argc > 2 && strcmp(argv[2], "hook") == 0) {
            if (init_allocator(&tagged, NULL, 0, 0) != 0) return 1;
        } else if (init_with(NULL, 0, NULL, 0, 0) != 0) {
            return 1;
        }
        pthread_t a, b;
        pthread_create(&a, NULL, scratch_thread, (void *)1000);
        pthread_create(&b, NULL, scratch_thread, (void *)5000);
        pthread_join(a, NULL);
        pthread_join(b, NULL);
        if (argc > 2) hook_counts("hook");
    }
    fflush(stdout);
    return 0;
}
