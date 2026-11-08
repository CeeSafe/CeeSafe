/* ffi/cfg-code.cee's configuration (ADR 0080 §3.3), as a kernel's build defines it: one switch on, one off. The
 * struct's members and the functions exist only where their switch is, as CONFIG_DEBUG's do in CosmoOS. */
#ifndef CFG_CODE_TYPES_H
#define CFG_CODE_TYPES_H

#include <stdint.h>

#define CFG_ON 1
#define CFG_OFF 0

struct engine {
    uint32_t blocks;
#if CFG_ON
    uint32_t armed;
#endif
#if CFG_OFF
    uint32_t never;
#endif
};

struct engine *the_engine(void);
#if CFG_ON
int32_t on_only(int32_t x);
#endif
#if CFG_OFF
int32_t off_only(int32_t x);
#endif

#endif
