#include "ceesafe/embed.h"
#include "ceesafe_exports.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The host's allocator: the C library's, with a count of the calls CeeSafe makes. */
static unsigned long requests, releases;

static void *host_alloc(void *context, size_t size) {
    (void)context;
    requests++;
    return malloc(size);
}

static void *host_alloc_zeroed(void *context, size_t size) {
    (void)context;
    requests++;
    return calloc(1, size);
}

static void *host_realloc(void *context, void *block, size_t size) {
    (void)context;
    requests++;
    return realloc(block, size);
}

static void host_free(void *context, void *block) {
    (void)context;
    releases++;
    free(block);
}

static const ceesafe_allocator host_allocator = {host_alloc, host_alloc_zeroed, host_realloc, host_free, NULL};

static void on_panic(void *context, const ceesafe_panic_info *info) {
    (void)context;
    fprintf(stderr, "host: CeeSafe stopped: %s at %s:%u\n", info->reason, info->file, info->line);
    exit(3);
}

static void add(void *journal, const char *line) {
    if (!journal_add(journal, (const uint8_t *)line, strlen(line))) {
        fprintf(stderr, "host: no memory for a line\n");
    }
}

int main(void) {
    ceesafe_config config = {0};
    config.size = sizeof config;
    config.allocator = &host_allocator;
    config.alloc_report = true;
    config.panic_hook = on_panic;
    int32_t started = ceesafe_init(&config);
    if (started != 0) {
        fprintf(stderr, "host: ceesafe_init failed: %d\n", (int)started);
        return 1;
    }
    printf("second init: %d\n", (int)ceesafe_init(&config));

    void *journal = journal_open();
    if (journal == NULL) {
        return 1;
    }
    add(journal, "the host owns main");
    add(journal, "CeeSafe owns the journal");
    printf("size: %llu\n", (unsigned long long)journal_size(journal));
    printf("live before close: %llu\n", (unsigned long long)ceesafe_shutdown());
    journal_close(journal);
    printf("live after close: %llu\n", (unsigned long long)ceesafe_shutdown());
    printf("host allocator: %lu requests, %lu releases\n", requests, releases);
    return 0;
}
