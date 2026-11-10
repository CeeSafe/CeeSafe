/* ADR 0088 §2.2: a C constructor that allocates through CeeSafe's heap before the generated `main` runs. */
#include "ceesafe/foreign.h"

static void *early;

__attribute__((constructor)) static void allocate_early(void) {
    early = ceesafe_alloc(16u, CEE_ALLOC_FOREIGN);
}
