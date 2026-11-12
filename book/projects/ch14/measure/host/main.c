#include "ceesafe/embed.h"
#include "ceesafe_exports.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Called when CeeSafe code panics. It must end the process. */
static void on_panic(void *context, const ceesafe_panic_info *info) {
    (void)context;
    fprintf(stderr, "host: CeeSafe stopped: %s at %s:%u\n", info->reason, info->file, info->line);
    exit(3);
}

int main(void) {
    ceesafe_config config = {0};
    config.size = sizeof config;
    config.panic_hook = on_panic;
    if (ceesafe_init(&config) != 0) {
        return 1;
    }

    const char *text = "one archive for one program";
    struct Range range = {3, 10};
    printf("words: %llu\n", (unsigned long long)measure_words((const uint8_t *)text, strlen(text)));
    printf("width: %lld\n", (long long)measure_width(range));
    printf("mean: %lld\n", (long long)measure_mean(64, 2));
    printf("mean: %lld\n", (long long)measure_mean(64, 0));
    return 0;
}
