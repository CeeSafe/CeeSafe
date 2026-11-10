/* The executor's three runtime aborts, reached directly (white box: the async runtime compiled into this file). */
#include "ceesafe_rt_async.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void hook(void *context, const ceesafe_panic_info *info) {
    (void)context;
    printf("hook kind=%u file=%s line=%u message=%.*s\n", info->kind, info->file != NULL ? info->file : "(null)",
           info->line, (int)info->message_length, (const char *)info->message);
    fflush(stdout);
    abort();
}

int main(int argc, char **argv) {
    ceesafe_config config;
    memset(&config, 0, sizeof config);
    config.size = (uint32_t)sizeof config;
    config.panic_hook = hook;
    if (argc < 2 || ceesafe_init(&config) != 0) return 1;
    if (strcmp(argv[1], "oom") == 0) cee_out_of_memory();
    if (strcmp(argv[1], "stuck") == 0) cee_stuck();
    if (strcmp(argv[1], "fd-wake") == 0) {
        static cee_task t;
        t.deadline = UINT64_MAX;
        t.on_wake = true;
        t.fd = 0;
        cee_park(&t);
    }
    return 0;
}
