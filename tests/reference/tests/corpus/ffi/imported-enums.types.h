/* ffi/imported-enums.cee's C types (ADR 0079): an enum, a typedef'd one, a member of one, and function pointer types
 * that take them -- the shapes CosmoOS's vnode_ops.mknod and vfs_dirent_cb have. */
#ifndef IMPORTED_ENUMS_TYPES_H
#define IMPORTED_ENUMS_TYPES_H

#include <stdint.h>

/* The negative enumerators make both enums' integer `int` on every target (type-system.md §31.6): without one GCC and
 * Clang give an enum `unsigned int` on Linux and macOS, and MSVC gives every enum `int` on Windows. */
enum color { NO_COLOR = -1, RED = 1, GREEN = 2, BLUE = 3 };
typedef enum { NO_SIZE = -1, SMALL = 0, BIG = 9 } size_kind_t;

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
