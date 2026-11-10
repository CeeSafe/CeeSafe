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
    } else if (strcmp(what, "threads") == 0) {
        if (init_with(NULL, 0, NULL, 0, 0) != 0) return 1;
        pthread_t a, b;
        pthread_create(&a, NULL, scratch_thread, (void *)1000);
        pthread_create(&b, NULL, scratch_thread, (void *)5000);
        pthread_join(a, NULL);
        pthread_join(b, NULL);
    }
    fflush(stdout);
    return 0;
}
