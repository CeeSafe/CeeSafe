/* The C half of ffi/unions.cee: statics whose unions CeeSafe reads through one member after C or CeeSafe wrote
 * another, and the values C sees afterwards. */
#include <stdio.h>
#include <string.h>

#include "ceesafe_exports.h"

static struct un_addr g_addr = { .family = 2, .port = 80, .v4 = 0x0100007f };
static struct un_holder g_holder = { .w = { .whole = 0x11223344 }, .tag = 9 };

struct un_addr *un_addr_get(void) { return &g_addr; }
struct un_holder *un_holder_get(void) { return &g_holder; }

int32_t c_main(void);

int32_t c_main(void) {
    un_use();
    printf("%x %x %x\n", (unsigned)g_addr.v4, (unsigned)g_holder.w.whole, (unsigned)g_holder.w.bytes[1]);
    memset(g_addr.v6.s6_addr, 0xff, sizeof g_addr.v6.s6_addr); /* an IPv6 address over the IPv4 one */
    printf("%u\n", (unsigned)un_v4());
    return 0;
}
