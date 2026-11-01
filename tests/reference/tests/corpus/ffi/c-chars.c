/* The C half of ffi/c-chars.cee: owns a pool whose name, tag and levels are C `char`s, and prints what CeeSafe did. */
#include <stdio.h>
#include <string.h>

#include "ceesafe_exports.h"

int32_t c_main(void);

size_t cc_length(const char *text) { return strlen(text); }

int32_t c_main(void) {
    struct cc_pool p;
    memset(&p, 0, sizeof p);
    p.id = 7;
    strcpy(p.name, "tank");
    p.tag = 'q';
    p.level[0] = -1;   /* 255 as a byte */
    p.level[1] = 100;
    p.key[0] = 200;
    printf("%u\n", (unsigned)cc_name_sum(&p));
    cc_shout(&p);      /* upper-cases the name in place, sets the tag */
    printf("%s %c %u\n", p.name, p.tag, (unsigned)cc_name_length(&p));
    cc_bump_tag(&p);
    printf("%c %d %u\n", p.tag, (int)p.level[0], (unsigned)p.key[0]);
    return 0;
}
