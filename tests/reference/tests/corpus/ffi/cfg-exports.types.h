/* ffi/cfg-exports.cee's configuration (ADR 0043), as a kernel's build defines it: one switch on, one off. With
 * CFG_OFF unset, cfg_off_fn is this header's stub, as netif.h's netif_tx_probe_set is outside self-test builds, so a
 * definition of the same name elsewhere would not compile. */
#ifndef CFG_EXPORTS_TYPES_H
#define CFG_EXPORTS_TYPES_H

#include <stdint.h>

#define CFG_ON 1
#define CFG_OFF 0

#if !CFG_OFF
static inline int32_t cfg_off_fn(void) { return 7; }
#endif

#endif
