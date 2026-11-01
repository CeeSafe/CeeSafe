/* The C types ffi/unions.cee imports (ADR 0041 §3.2): a struct with an anonymous union member, as CosmoOS's
 * struct netaddr is, and a union type embedded in a struct. */
#ifndef UNIONS_TYPES_H
#define UNIONS_TYPES_H

#include <stdint.h>

struct un_in6 {
    uint8_t s6_addr[16];
};

struct un_addr {
    uint16_t family;
    uint16_t port;
    union {
        uint32_t v4;
        struct un_in6 v6;
    };
};

union un_word {
    uint32_t whole;
    uint8_t bytes[4];
    uint16_t halves[2];
};

struct un_holder {
    union un_word w;
    uint8_t tag;
};

struct un_addr *un_addr_get(void); /* in unions.c */
struct un_holder *un_holder_get(void);

#endif
