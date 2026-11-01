/* The C types ffi/c-chars.cee imports (ADR 0034): `char` array members and a `char` member, seen as bytes. */
#ifndef C_CHARS_TYPES_H
#define C_CHARS_TYPES_H

#include <stddef.h>
#include <stdint.h>

struct cc_pool {
    uint32_t id;
    char name[16];         /* [u8; 16] */
    char tag;              /* u8 */
    signed char level[4];  /* [u8; 4]: the bytes, whatever the signedness */
    unsigned char key[2];  /* [u8; 2] */
};

size_t cc_length(const char *text); /* strlen, in c-chars.c */

#endif
