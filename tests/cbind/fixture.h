/* The header tools/cbind_check.py generates a binding for: one of each row of ADR 0095 §2.2, and what is left out. */
#ifndef CBIND_FIXTURE_H
#define CBIND_FIXTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "other.h"

#define FX_LIMIT 22
#define FX_MASK ((uint32_t)1 << 31)
#define FX_NEGATIVE (-5)
#define FX_NAME "fixture"
#define FX_CALL(x) ((x) + 1)
#define FX_STATEMENT do { } while (0)
#define FX_FLOAT 1.5
#if FX_WIDE
#define FX_WIDTH 64
#else
#define FX_WIDTH 32
#endif

enum fx_color { FX_RED, FX_GREEN = 5, FX_BLUE };
enum fx_signed { FX_MINUS = -1, FX_PLUS = 1 };
enum { FX_ANON_A = 3, FX_ANON_B };
typedef enum { FX_T_ONE = 1, FX_T_TWO } fx_kind_t;

struct fx_point {
    int32_t x;
    int32_t y;
};
typedef struct {
    uint64_t v;
} fx_value_t;
typedef int (*fx_callback)(void *arg);
union fx_tagged {
    int a;
    long b;
};
typedef union {
    int a;
    void *p;
} fx_either_t;

struct fx_node; /* declared, never defined */

struct fx_box {
    struct fx_point origin;
    struct fx_point corners[4];
    enum fx_color color;
    fx_callback notify;
    int (*compare)(const void *, const void *);
    const fx_callback *table;
    _Atomic uint32_t refs;
    fx_value_t value;
    struct fx_node *next;
    struct other_thing *other;
    const char *name;
    char label[16];
    uint8_t type;
    bool live;
#if FX_WIDE
    uint64_t wide;
#endif
};
struct fx_bits {
    unsigned a : 3;
    unsigned b : 5;
};
struct fx_anon {
    int kind;
    union {
        int i;
        void *p;
    };
};
struct fx_flex {
    size_t n;
    char data[];
};
struct fx_vol {
    volatile uint32_t status;
};

extern int fx_count;
extern const struct fx_point fx_origin;
extern const char *const fx_names[4];
extern const char *fx_current;

int fx_add(int a, int b);
struct fx_point fx_make(int x, int y);
int fx_log(const char *fmt, ...);
int type(int in);
enum fx_color fx_pick(struct fx_box *box, fx_callback cb, const char *const argv[]);
void fx_visit(struct fx_node *n, other_cb cb, enum other_mode mode);
void fx_vprint(const char *fmt, __builtin_va_list ap);
static inline uint32_t fx_twice(uint32_t v) { return v * 2; }

#endif
