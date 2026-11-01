/* The C half of ffi/cfg-exports.cee: calls the export its build has, the header's stub for the one it does not, and
 * an export that calls the configured-out function from CeeSafe, where it exists in every build. */
#include <stdio.h>

#include "ceesafe_exports.h"

int32_t c_main(void);

int32_t c_main(void) {
    printf("%d %d %d\n", (int)cfg_on_fn(), (int)cfg_off_fn(), (int)cfg_inner());
    return 0;
}
