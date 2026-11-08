/* ffi/imported-enums.cee's C types (ADR 0079): an enum, a typedef'd one, a member of one, and function pointer types
 * that take them -- the shapes CosmoOS's vnode_ops.mknod and vfs_dirent_cb have. */
#ifndef IMPORTED_ENUMS_TYPES_H
#define IMPORTED_ENUMS_TYPES_H

#include <stdint.h>

enum color { RED = 1, GREEN = 2, BLUE = 3 };
typedef enum { SMALL = 0, BIG = 9 } size_kind_t;

struct paint {
    enum color c;
    int32_t amount;
};

/* An operation table: CeeSafe's export is its entry, and CeeSafe calls through it. */
struct painter_ops {
    int32_t (*mix)(enum color a, size_kind_t s);
};

/* A callback type with an enum parameter. */
typedef int32_t (*color_cb)(void *arg, enum color c);

#endif
