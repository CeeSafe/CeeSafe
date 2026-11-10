/* ADR 0088 §8: a C constructor starts a thread that allocates through CeeSafe's heap and reads the program arguments
   while the generated `main`'s ceesafe_init writes the counting configuration and the arguments. Run under
   ThreadSanitizer (tools/embed_check.py): no data race. */
#include "ceesafe/rt.h"

#include <pthread.h>

static pthread_t early_thread;
static uint64_t seen_arguments;

static void *allocate_early(void *arg) {
    (void)arg;
    for (int k = 0; k < 200000; k++) {
        void *p = ceesafe_alloc(32u, CEE_ALLOC_FOREIGN);
        ceesafe_free(p, CEE_ALLOC_FOREIGN);
        uint64_t count = ceesafe_arg_count();
        cee_str text;
        if (count > 0 && ceesafe_arg(0, &text) && text.len > 0) {
            seen_arguments = count;
        }
    }
    return NULL;
}

__attribute__((constructor)) static void start_early(void) {
    pthread_create(&early_thread, NULL, allocate_early, NULL);
}

__attribute__((destructor)) static void join_early(void) {
    pthread_join(early_thread, NULL);
}
