/* SPDX-License-Identifier: GPL-2.0-only WITH Classpath-exception-2.0 */
/*
 * CeeSafe runtime: core interface (freestanding).
 *
 * Generated C includes only this header. It uses only C11 freestanding headers and defines the
 * checked operations whose semantics are specified in spec/type-system.md §6-7 and mirrored by
 * CeeSafe.Compiler.Semantics.IntegerSemantics. Platform services (output, exit, abort) are declared
 * at the bottom and supplied by a profile runtime, e.g. runtime/hosted/ceesafe_rt_hosted.c.
 *
 * Define CEE_NO_BUILTIN_OVERFLOW to use the portable overflow checks instead of compiler builtins.
 * The tests exercise both.
 */
#ifndef CEESAFE_RT_H
#define CEESAFE_RT_H
#define CEESAFE_ABI_VERSION 1

#include <float.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Floats (type-system.md §35.6): IEEE binary32/binary64, each operation rounded once to its type. Only programs
   that use floats define CEE_FLOATS (the generated main.c, and the float formatter); the others need not meet these
   checks, which a kernel built with -mgeneral-regs-only does not (x86-64 then has no SSE: FLT_EVAL_METHOD != 0). */
#if defined(CEE_FLOATS)
#if FLT_RADIX != 2 || FLT_MANT_DIG != 24 || DBL_MANT_DIG != 53
#error "CeeSafe needs IEEE 754 binary32 float and binary64 double"
#endif
#if defined(FLT_EVAL_METHOD) && FLT_EVAL_METHOD != 0
#error "CeeSafe needs FLT_EVAL_METHOD == 0: float and double evaluated in their own precision"
#endif
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#endif
/* gcc ignores the standard pragma, but does not contract under -std=c11; ceesafec passes -ffp-contract=off too. */
#endif

#if defined(__GNUC__) || defined(__clang__)
#define CEE_UNUSED __attribute__((unused))
#define CEE_COLD __attribute__((cold))
#else
#define CEE_UNUSED
#define CEE_COLD
#endif

/* A compiler temporary (`t_<n>`, c-backend.md §7): unused-tolerant, and under clang left out of the debug info. An
   optimised build gives every temporary a variable with a location list, which made the location lists most of a
   generated program's DWARF (CosmoOS's kernel: 4.8 bytes of them per byte of code, against 0.6 for its C). GCC has no
   `nodebug` for variables. `ceesafec build -g` (#line directives on) declares them CEE_UNUSED instead, for debuggers. */
#if defined(__clang__)
#define CEE_TEMP __attribute__((unused, nodebug))
#elif defined(__GNUC__)
#define CEE_TEMP __attribute__((unused))
#else
#define CEE_TEMP
#endif

/*
 * The most-aligned fundamental type: C11's max_align_t, which MSVC's <stddef.h> lacks in C mode; there
 * `double` is the strictest fundamental alignment (8; spec/c-backend.md §1.1).
 */
#if defined(_MSC_VER) && !defined(__clang__)
typedef double cee_max_align_t;
#else
typedef max_align_t cee_max_align_t;
#endif

#if !defined(CEE_NO_BUILTIN_OVERFLOW) && (defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 5))
#define CEE_HAVE_BUILTIN_OVERFLOW 1
#endif

/* ---- Panics ---------------------------------------------------------------------------- */

typedef enum cee_panic_kind {
    CEE_PANIC_OVERFLOW = 1,
    CEE_PANIC_DIVIDE_BY_ZERO = 2,
    CEE_PANIC_SHIFT = 3,
    CEE_PANIC_UNREACHABLE = 4,
    CEE_PANIC_INDEX = 5,
    CEE_PANIC_SLICE = 6,
    CEE_PANIC_UNWRAP = 7,
    CEE_PANIC_SCRATCH = 8,
    CEE_PANIC_UNWRAP_NULL = 9,
    CEE_PANIC_STR_BOUNDARY = 10,
    CEE_PANIC_FFI_ENUM = 11,
    CEE_PANIC_LENGTH = 12,
    CEE_PANIC_FLOAT_RANGE = 13,
    CEE_PANIC_ASYNC = 14, /* the executor cannot go on: every task waits for nothing, or its bookkeeping failed */
    CEE_PANIC_FRAME_STACK = 15, /* a task's frame stack could not grow (ADR 0056 §3.3) */
    CEE_PANIC_EXPLICIT = 16 /* `panic(message)` or a failed `assert` (ADR 0075 §2.8) */
} cee_panic_kind;

/* A CeeSafe source location; `file` points to a string in the generated program's file table. */
typedef struct cee_loc {
    const char *file;
    uint32_t line;
    uint32_t column;
} cee_loc;

#define CEE_LOC(file, line, column) ((cee_loc){(file), (line), (column)})

/* The checked helpers below take a pointer to a location constant (`ceeL_<n>` in generated code) and read it only
   when the check fails, so a passing check costs nothing for its location (c-backend.md §6, M9d). */

/* Reports the panic and aborts; never returns (spec/ownership.md §0: panic = abort). Defined by the hosted
   runtime, or by the environment of a freestanding package (profiles.md §4.2). */
CEE_COLD _Noreturn void ceesafe_panic(cee_panic_kind kind, cee_loc loc);

/* The text of a panic kind ("integer overflow", ...), for any ceesafe_panic; in the freestanding core too. */
const char *ceesafe_panic_reason(cee_panic_kind kind);

/* ---- Checked arithmetic: + - * ---------------------------------------------------------- */

#if defined(CEE_HAVE_BUILTIN_OVERFLOW)

#define CEE_ARITH(T, S)                                                                    \
    static inline T cee_add_##S(T a, T b, const cee_loc *loc) {                                    \
        T r;                                                                              \
        if (__builtin_add_overflow(a, b, &r)) ceesafe_panic(CEE_PANIC_OVERFLOW, *loc);        \
        return r;                                                                         \
    }                                                                                     \
    static inline T cee_sub_##S(T a, T b, const cee_loc *loc) {                                    \
        T r;                                                                              \
        if (__builtin_sub_overflow(a, b, &r)) ceesafe_panic(CEE_PANIC_OVERFLOW, *loc);        \
        return r;                                                                         \
    }                                                                                     \
    static inline T cee_mul_##S(T a, T b, const cee_loc *loc) {                                    \
        T r;                                                                              \
        if (__builtin_mul_overflow(a, b, &r)) ceesafe_panic(CEE_PANIC_OVERFLOW, *loc);        \
        return r;                                                                         \
    }
#define CEE_ARITH_SIGNED_NARROW(T, S, MIN, MAX) CEE_ARITH(T, S)
#define CEE_ARITH_UNSIGNED_NARROW(T, S, MAX) CEE_ARITH(T, S)
#define CEE_ARITH_I64 CEE_ARITH(int64_t, i64)
#define CEE_ARITH_U64 CEE_ARITH(uint64_t, u64)

#else /* portable fallback */

/* i8/i16/i32: the exact result always fits in int64_t. */
#define CEE_ARITH_SIGNED_NARROW(T, S, MIN, MAX)                                            \
    static inline T cee_narrow_##S(int64_t r, const cee_loc *loc) {                                \
        if (r < (MIN) || r > (MAX)) ceesafe_panic(CEE_PANIC_OVERFLOW, *loc);                   \
        return (T)r;                                                                      \
    }                                                                                     \
    static inline T cee_add_##S(T a, T b, const cee_loc *loc) { return cee_narrow_##S((int64_t)a + (int64_t)b, loc); } \
    static inline T cee_sub_##S(T a, T b, const cee_loc *loc) { return cee_narrow_##S((int64_t)a - (int64_t)b, loc); } \
    static inline T cee_mul_##S(T a, T b, const cee_loc *loc) { return cee_narrow_##S((int64_t)a * (int64_t)b, loc); }

/* u8/u16/u32: sums and products always fit in uint64_t; differences are checked first. */
#define CEE_ARITH_UNSIGNED_NARROW(T, S, MAX)                                               \
    static inline T cee_narrow_##S(uint64_t r, const cee_loc *loc) {                               \
        if (r > (MAX)) ceesafe_panic(CEE_PANIC_OVERFLOW, *loc);                                \
        return (T)r;                                                                      \
    }                                                                                     \
    static inline T cee_add_##S(T a, T b, const cee_loc *loc) { return cee_narrow_##S((uint64_t)a + (uint64_t)b, loc); } \
    static inline T cee_sub_##S(T a, T b, const cee_loc *loc) {                                    \
        if (a < b) ceesafe_panic(CEE_PANIC_OVERFLOW, *loc);                                    \
        return (T)(a - b);                                                                \
    }                                                                                     \
    static inline T cee_mul_##S(T a, T b, const cee_loc *loc) { return cee_narrow_##S((uint64_t)a * (uint64_t)b, loc); }

/* i64: pre-checks in the style of CERT INT32-C; no operation overflows before the check. */
#define CEE_ARITH_I64                                                                      \
    static inline int64_t cee_add_i64(int64_t a, int64_t b, const cee_loc *loc) {                  \
        if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b))                 \
            ceesafe_panic(CEE_PANIC_OVERFLOW, *loc);                                           \
        return a + b;                                                                     \
    }                                                                                     \
    static inline int64_t cee_sub_i64(int64_t a, int64_t b, const cee_loc *loc) {                  \
        if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b))                 \
            ceesafe_panic(CEE_PANIC_OVERFLOW, *loc);                                           \
        return a - b;                                                                     \
    }                                                                                     \
    static inline int64_t cee_mul_i64(int64_t a, int64_t b, const cee_loc *loc) {                  \
        if (a > 0) {                                                                      \
            if (b > 0 ? a > INT64_MAX / b : b < INT64_MIN / a)                            \
                ceesafe_panic(CEE_PANIC_OVERFLOW, *loc);                                       \
        } else if (a < 0) {                                                               \
            if (b > 0 ? a < INT64_MIN / b : (b < 0 && a < INT64_MAX / b))                 \
                ceesafe_panic(CEE_PANIC_OVERFLOW, *loc);                                       \
        }                                                                                 \
        return a * b;                                                                     \
    }

#define CEE_ARITH_U64                                                                      \
    static inline uint64_t cee_add_u64(uint64_t a, uint64_t b, const cee_loc *loc) {               \
        if (a > UINT64_MAX - b) ceesafe_panic(CEE_PANIC_OVERFLOW, *loc);                       \
        return a + b;                                                                     \
    }                                                                                     \
    static inline uint64_t cee_sub_u64(uint64_t a, uint64_t b, const cee_loc *loc) {               \
        if (a < b) ceesafe_panic(CEE_PANIC_OVERFLOW, *loc);                                    \
        return a - b;                                                                     \
    }                                                                                     \
    static inline uint64_t cee_mul_u64(uint64_t a, uint64_t b, const cee_loc *loc) {               \
        if (a != 0 && b > UINT64_MAX / a) ceesafe_panic(CEE_PANIC_OVERFLOW, *loc);             \
        return a * b;                                                                     \
    }

#endif

CEE_ARITH_SIGNED_NARROW(int8_t, i8, INT8_MIN, INT8_MAX)
CEE_ARITH_SIGNED_NARROW(int16_t, i16, INT16_MIN, INT16_MAX)
CEE_ARITH_SIGNED_NARROW(int32_t, i32, INT32_MIN, INT32_MAX)
CEE_ARITH_I64
CEE_ARITH_UNSIGNED_NARROW(uint8_t, u8, UINT8_MAX)
CEE_ARITH_UNSIGNED_NARROW(uint16_t, u16, UINT16_MAX)
CEE_ARITH_UNSIGNED_NARROW(uint32_t, u32, UINT32_MAX)
CEE_ARITH_U64

/* ---- Division, remainder, negation ------------------------------------------------------ */

/* `/` truncates toward zero; `%` has the dividend's sign; MIN / -1 overflows; MIN % -1 == 0. */
#define CEE_DIV_SIGNED(T, S, MIN)                                                          \
    static inline T cee_div_##S(T a, T b, const cee_loc *loc) {                                    \
        if (b == 0) ceesafe_panic(CEE_PANIC_DIVIDE_BY_ZERO, *loc);                             \
        if (a == (MIN) && b == -1) ceesafe_panic(CEE_PANIC_OVERFLOW, *loc);                    \
        return (T)(a / b);                                                                \
    }                                                                                     \
    static inline T cee_rem_##S(T a, T b, const cee_loc *loc) {                                    \
        if (b == 0) ceesafe_panic(CEE_PANIC_DIVIDE_BY_ZERO, *loc);                             \
        if (b == -1) return 0; /* avoids the undefined MIN % -1 in C */                   \
        return (T)(a % b);                                                                \
    }                                                                                     \
    static inline T cee_neg_##S(T a, const cee_loc *loc) {                                         \
        if (a == (MIN)) ceesafe_panic(CEE_PANIC_OVERFLOW, *loc);                               \
        return (T)(-a);                                                                   \
    }

#define CEE_DIV_UNSIGNED(T, S)                                                             \
    static inline T cee_div_##S(T a, T b, const cee_loc *loc) {                                    \
        if (b == 0) ceesafe_panic(CEE_PANIC_DIVIDE_BY_ZERO, *loc);                             \
        return (T)(a / b);                                                                \
    }                                                                                     \
    static inline T cee_rem_##S(T a, T b, const cee_loc *loc) {                                    \
        if (b == 0) ceesafe_panic(CEE_PANIC_DIVIDE_BY_ZERO, *loc);                             \
        return (T)(a % b);                                                                \
    }

CEE_DIV_SIGNED(int8_t, i8, INT8_MIN)
CEE_DIV_SIGNED(int16_t, i16, INT16_MIN)
CEE_DIV_SIGNED(int32_t, i32, INT32_MIN)
CEE_DIV_SIGNED(int64_t, i64, INT64_MIN)
CEE_DIV_UNSIGNED(uint8_t, u8)
CEE_DIV_UNSIGNED(uint16_t, u16)
CEE_DIV_UNSIGNED(uint32_t, u32)
CEE_DIV_UNSIGNED(uint64_t, u64)

/* ---- Shifts (left operand unsigned in M1; '>>' is logical) ------------------------------- */

static inline uint64_t cee_shift_amount_u(uint64_t n, uint64_t bits, const cee_loc *loc) {
    if (n >= bits) ceesafe_panic(CEE_PANIC_SHIFT, *loc);
    return n;
}

static inline uint64_t cee_shift_amount_s(int64_t n, uint64_t bits, const cee_loc *loc) {
    if (n < 0 || (uint64_t)n >= bits) ceesafe_panic(CEE_PANIC_SHIFT, *loc);
    return (uint64_t)n;
}

/* `n` has been validated by cee_shift_amount_*; bits shifted out are discarded. */
#define CEE_SHIFT(T, S)                                                                    \
    static inline T cee_shl_##S(T a, uint64_t n) { return (T)((uint64_t)a << n); }         \
    static inline T cee_shr_##S(T a, uint64_t n) { return (T)((uint64_t)a >> n); }

CEE_SHIFT(uint8_t, u8)
CEE_SHIFT(uint16_t, u16)
CEE_SHIFT(uint32_t, u32)
CEE_SHIFT(uint64_t, u64)

/* ---- Reinterpretation (truncate<iN>) ----------------------------------------------------- */

/* Converting an out-of-range value to a signed type is implementation-defined in C11
 * (6.3.1.3p3), so two's-complement reinterpretation is spelled out. */
static inline int8_t cee_as_i8(uint8_t u) { return u <= INT8_MAX ? (int8_t)u : (int8_t)(-(int)(UINT8_MAX - u) - 1); }
static inline int16_t cee_as_i16(uint16_t u) { return u <= INT16_MAX ? (int16_t)u : (int16_t)(-(int)(UINT16_MAX - u) - 1); }
static inline int32_t cee_as_i32(uint32_t u) { return u <= INT32_MAX ? (int32_t)u : (int32_t)(-(int64_t)(UINT32_MAX - u) - 1); }
static inline int64_t cee_as_i64(uint64_t u) { return u <= INT64_MAX ? (int64_t)u : -(int64_t)(UINT64_MAX - u) - 1; }

/* ---- Explicit overflow behaviour: wrapping, checked, saturating (type-system.md §6, M11) ----- */

/* cee_try_op_S(a, b, &r): true and the result in r, or false if it does not fit in the type. */
#if defined(CEE_HAVE_BUILTIN_OVERFLOW)
#define CEE_TRY(T, S)                                                                                  \
    static inline bool cee_try_add_##S(T a, T b, T *r) { return !__builtin_add_overflow(a, b, r); }    \
    static inline bool cee_try_sub_##S(T a, T b, T *r) { return !__builtin_sub_overflow(a, b, r); }    \
    static inline bool cee_try_mul_##S(T a, T b, T *r) { return !__builtin_mul_overflow(a, b, r); }
#define CEE_TRY_SIGNED_NARROW(T, S, MIN, MAX) CEE_TRY(T, S)
#define CEE_TRY_UNSIGNED_NARROW(T, S, MAX) CEE_TRY(T, S)
#define CEE_TRY_I64 CEE_TRY(int64_t, i64)
#define CEE_TRY_U64 CEE_TRY(uint64_t, u64)
#else /* portable fallback, as for the checked operators above */
#define CEE_TRY_SIGNED_NARROW(T, S, MIN, MAX)                                                          \
    static inline bool cee_tryfit_##S(int64_t x, T *r) {                                               \
        if (x < (MIN) || x > (MAX)) return false;                                                     \
        *r = (T)x;                                                                                    \
        return true;                                                                                  \
    }                                                                                                 \
    static inline bool cee_try_add_##S(T a, T b, T *r) { return cee_tryfit_##S((int64_t)a + (int64_t)b, r); } \
    static inline bool cee_try_sub_##S(T a, T b, T *r) { return cee_tryfit_##S((int64_t)a - (int64_t)b, r); } \
    static inline bool cee_try_mul_##S(T a, T b, T *r) { return cee_tryfit_##S((int64_t)a * (int64_t)b, r); }
#define CEE_TRY_UNSIGNED_NARROW(T, S, MAX)                                                             \
    static inline bool cee_tryfit_##S(uint64_t x, T *r) {                                              \
        if (x > (MAX)) return false;                                                                  \
        *r = (T)x;                                                                                    \
        return true;                                                                                  \
    }                                                                                                 \
    static inline bool cee_try_add_##S(T a, T b, T *r) { return cee_tryfit_##S((uint64_t)a + (uint64_t)b, r); } \
    static inline bool cee_try_sub_##S(T a, T b, T *r) {                                               \
        if (a < b) return false;                                                                      \
        *r = (T)(a - b);                                                                              \
        return true;                                                                                  \
    }                                                                                                 \
    static inline bool cee_try_mul_##S(T a, T b, T *r) { return cee_tryfit_##S((uint64_t)a * (uint64_t)b, r); }
#define CEE_TRY_I64                                                                                    \
    static inline bool cee_try_add_i64(int64_t a, int64_t b, int64_t *r) {                             \
        if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) return false;               \
        *r = a + b;                                                                                   \
        return true;                                                                                  \
    }                                                                                                 \
    static inline bool cee_try_sub_i64(int64_t a, int64_t b, int64_t *r) {                             \
        if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b)) return false;               \
        *r = a - b;                                                                                   \
        return true;                                                                                  \
    }                                                                                                 \
    static inline bool cee_try_mul_i64(int64_t a, int64_t b, int64_t *r) {                             \
        if (a > 0) {                                                                                  \
            if (b > 0 ? a > INT64_MAX / b : b < INT64_MIN / a) return false;                          \
        } else if (a < 0) {                                                                           \
            if (b > 0 ? a < INT64_MIN / b : (b < 0 && a < INT64_MAX / b)) return false;               \
        }                                                                                             \
        *r = a * b;                                                                                   \
        return true;                                                                                  \
    }
#define CEE_TRY_U64                                                                                    \
    static inline bool cee_try_add_u64(uint64_t a, uint64_t b, uint64_t *r) {                          \
        if (a > UINT64_MAX - b) return false;                                                         \
        *r = a + b;                                                                                   \
        return true;                                                                                  \
    }                                                                                                 \
    static inline bool cee_try_sub_u64(uint64_t a, uint64_t b, uint64_t *r) {                          \
        if (a < b) return false;                                                                      \
        *r = a - b;                                                                                   \
        return true;                                                                                  \
    }                                                                                                 \
    static inline bool cee_try_mul_u64(uint64_t a, uint64_t b, uint64_t *r) {                          \
        if (a != 0 && b > UINT64_MAX / a) return false;                                               \
        *r = a * b;                                                                                   \
        return true;                                                                                  \
    }
#endif

/* Wrapping: unsigned arithmetic in a type no narrower than unsigned int (so nothing is promoted to int),
   truncated; a signed type wraps through its unsigned twin and converts back with cee_as_iN. */
#define CEE_WRAP_UNSIGNED(T, S, W)                                                                     \
    static inline T cee_wrapping_add_##S(T a, T b) { return (T)((W)a + (W)b); }                        \
    static inline T cee_wrapping_sub_##S(T a, T b) { return (T)((W)a - (W)b); }                        \
    static inline T cee_wrapping_mul_##S(T a, T b) { return (T)((W)a * (W)b); }
#define CEE_WRAP_SIGNED(T, S, U, US)                                                                   \
    static inline T cee_wrapping_add_##S(T a, T b) { return cee_as_##S(cee_wrapping_add_##US((U)a, (U)b)); } \
    static inline T cee_wrapping_sub_##S(T a, T b) { return cee_as_##S(cee_wrapping_sub_##US((U)a, (U)b)); } \
    static inline T cee_wrapping_mul_##S(T a, T b) { return cee_as_##S(cee_wrapping_mul_##US((U)a, (U)b)); }

/* Saturating: the exact result if it fits, else the bound the overflow went past. */
#define CEE_SAT_UNSIGNED(T, S, MAX)                                                                    \
    static inline T cee_saturating_add_##S(T a, T b) { T r; return cee_try_add_##S(a, b, &r) ? r : (MAX); } \
    static inline T cee_saturating_sub_##S(T a, T b) { T r; return cee_try_sub_##S(a, b, &r) ? r : 0u; }   \
    static inline T cee_saturating_mul_##S(T a, T b) { T r; return cee_try_mul_##S(a, b, &r) ? r : (MAX); }
#define CEE_SAT_SIGNED(T, S, MIN, MAX)                                                                 \
    static inline T cee_saturating_add_##S(T a, T b) { T r; return cee_try_add_##S(a, b, &r) ? r : b < 0 ? (MIN) : (MAX); } \
    static inline T cee_saturating_sub_##S(T a, T b) { T r; return cee_try_sub_##S(a, b, &r) ? r : b < 0 ? (MAX) : (MIN); } \
    static inline T cee_saturating_mul_##S(T a, T b) { T r; return cee_try_mul_##S(a, b, &r) ? r : (a < 0) != (b < 0) ? (MIN) : (MAX); }

CEE_TRY_SIGNED_NARROW(int8_t, i8, INT8_MIN, INT8_MAX)
CEE_TRY_SIGNED_NARROW(int16_t, i16, INT16_MIN, INT16_MAX)
CEE_TRY_SIGNED_NARROW(int32_t, i32, INT32_MIN, INT32_MAX)
CEE_TRY_I64
CEE_TRY_UNSIGNED_NARROW(uint8_t, u8, UINT8_MAX)
CEE_TRY_UNSIGNED_NARROW(uint16_t, u16, UINT16_MAX)
CEE_TRY_UNSIGNED_NARROW(uint32_t, u32, UINT32_MAX)
CEE_TRY_U64
CEE_WRAP_UNSIGNED(uint8_t, u8, uint32_t)
CEE_WRAP_UNSIGNED(uint16_t, u16, uint32_t)
CEE_WRAP_UNSIGNED(uint32_t, u32, uint32_t)
CEE_WRAP_UNSIGNED(uint64_t, u64, uint64_t)
CEE_WRAP_SIGNED(int8_t, i8, uint8_t, u8)
CEE_WRAP_SIGNED(int16_t, i16, uint16_t, u16)
CEE_WRAP_SIGNED(int32_t, i32, uint32_t, u32)
CEE_WRAP_SIGNED(int64_t, i64, uint64_t, u64)
CEE_SAT_UNSIGNED(uint8_t, u8, UINT8_MAX)
CEE_SAT_UNSIGNED(uint16_t, u16, UINT16_MAX)
CEE_SAT_UNSIGNED(uint32_t, u32, UINT32_MAX)
CEE_SAT_UNSIGNED(uint64_t, u64, UINT64_MAX)
CEE_SAT_SIGNED(int8_t, i8, INT8_MIN, INT8_MAX)
CEE_SAT_SIGNED(int16_t, i16, INT16_MIN, INT16_MAX)
CEE_SAT_SIGNED(int32_t, i32, INT32_MIN, INT32_MAX)
CEE_SAT_SIGNED(int64_t, i64, INT64_MIN, INT64_MAX)

/* min and max on integers (type-system.md §6): the smaller or larger operand; never overflows. */
#define CEE_MINMAX(T, S)                                                                               \
    static inline T cee_min_##S(T a, T b) { return a <= b ? a : b; }                                   \
    static inline T cee_max_##S(T a, T b) { return a >= b ? a : b; }
CEE_MINMAX(uint8_t, u8)
CEE_MINMAX(uint16_t, u16)
CEE_MINMAX(uint32_t, u32)
CEE_MINMAX(uint64_t, u64)
CEE_MINMAX(int8_t, i8)
CEE_MINMAX(int16_t, i16)
CEE_MINMAX(int32_t, i32)
CEE_MINMAX(int64_t, i64)

/* unsignedAbs (ADR 0063 §2.5): the magnitude in the unsigned type of the same width, so the minimum fits. */
#define CEE_UNSIGNED_ABS(T, S, U) \
    static inline U cee_unsigned_abs_##S(T a) { return a < 0 ? (U)((U)0 - (U)a) : (U)a; }
CEE_UNSIGNED_ABS(int8_t, i8, uint8_t)
CEE_UNSIGNED_ABS(int16_t, i16, uint16_t)
CEE_UNSIGNED_ABS(int32_t, i32, uint32_t)
CEE_UNSIGNED_ABS(int64_t, i64, uint64_t)

/* The bit methods (ADR 0071 §2.3). Amounts are taken modulo the width, so no shift is by the width or more; the
   arithmetic is in a type no narrower than unsigned int, so nothing is promoted to int. Counts of a zero value are
   the width. GCC and clang get their builtins; MSVC gets portable loops (c-backend.md §1.1). */
#define CEE_BITS_UNSIGNED(T, S, W, N)                                                                  \
    static inline T cee_rotl_##S(T x, uint32_t n) {                                                     \
        n &= (N) - 1u;                                                                                  \
        return n == 0u ? x : (T)(((W)x << n) | ((W)x >> ((N) - n)));                                    \
    }                                                                                                   \
    static inline T cee_rotr_##S(T x, uint32_t n) {                                                     \
        n &= (N) - 1u;                                                                                  \
        return n == 0u ? x : (T)(((W)x >> n) | ((W)x << ((N) - n)));                                    \
    }                                                                                                   \
    static inline T cee_shl_wrap_##S(T x, uint32_t n) { return (T)((W)x << (n & ((N) - 1u))); }          \
    static inline T cee_shr_wrap_##S(T x, uint32_t n) { return (T)((W)x >> (n & ((N) - 1u))); }          \
    static inline uint32_t cee_popcount_##S(T x) {                                                      \
        uint32_t c = 0u;                                                                                \
        W v = (W)x;                                                                                     \
        while (v != 0u) { v &= v - 1u; c++; }                                                           \
        return c;                                                                                       \
    }                                                                                                   \
    static inline uint32_t cee_clz_##S(T x) {                                                           \
        uint32_t c = 0u;                                                                                \
        W top = (W)1 << ((N) - 1u);                                                                     \
        W v = (W)x;                                                                                     \
        if (v == 0u) return (N);                                                                        \
        while ((v & top) == 0u) { v <<= 1; c++; }                                                       \
        return c;                                                                                       \
    }                                                                                                   \
    static inline uint32_t cee_ctz_##S(T x) {                                                           \
        uint32_t c = 0u;                                                                                \
        W v = (W)x;                                                                                     \
        if (v == 0u) return (N);                                                                        \
        while ((v & 1u) == 0u) { v >>= 1; c++; }                                                        \
        return c;                                                                                       \
    }
CEE_BITS_UNSIGNED(uint8_t, u8, uint32_t, 8u)
CEE_BITS_UNSIGNED(uint16_t, u16, uint32_t, 16u)
#if defined(__GNUC__) || defined(__clang__)
static inline uint32_t cee_rotl_u32(uint32_t x, uint32_t n) { n &= 31u; return n == 0u ? x : (x << n) | (x >> (32u - n)); }
static inline uint32_t cee_rotr_u32(uint32_t x, uint32_t n) { n &= 31u; return n == 0u ? x : (x >> n) | (x << (32u - n)); }
static inline uint32_t cee_shl_wrap_u32(uint32_t x, uint32_t n) { return x << (n & 31u); }
static inline uint32_t cee_shr_wrap_u32(uint32_t x, uint32_t n) { return x >> (n & 31u); }
static inline uint32_t cee_popcount_u32(uint32_t x) {
    /* Not __builtin_popcount: without a popcount instruction GCC calls libgcc, which a freestanding build may not link. */
    x = x - ((x >> 1) & 0x55555555u);
    x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
    return (((x + (x >> 4)) & 0x0F0F0F0Fu) * 0x01010101u) >> 24;
}
static inline uint32_t cee_clz_u32(uint32_t x) { return x == 0u ? 32u : (uint32_t)__builtin_clz(x); }
static inline uint32_t cee_ctz_u32(uint32_t x) { return x == 0u ? 32u : (uint32_t)__builtin_ctz(x); }
static inline uint64_t cee_rotl_u64(uint64_t x, uint32_t n) { n &= 63u; return n == 0u ? x : (x << n) | (x >> (64u - n)); }
static inline uint64_t cee_rotr_u64(uint64_t x, uint32_t n) { n &= 63u; return n == 0u ? x : (x >> n) | (x << (64u - n)); }
static inline uint64_t cee_shl_wrap_u64(uint64_t x, uint32_t n) { return x << (n & 63u); }
static inline uint64_t cee_shr_wrap_u64(uint64_t x, uint32_t n) { return x >> (n & 63u); }
static inline uint32_t cee_popcount_u64(uint64_t x) {
    x = x - ((x >> 1) & 0x5555555555555555u);
    x = (x & 0x3333333333333333u) + ((x >> 2) & 0x3333333333333333u);
    return (uint32_t)((((x + (x >> 4)) & 0x0F0F0F0F0F0F0F0Fu) * 0x0101010101010101u) >> 56);
}
static inline uint32_t cee_clz_u64(uint64_t x) { return x == 0u ? 64u : (uint32_t)__builtin_clzll(x); }
static inline uint32_t cee_ctz_u64(uint64_t x) { return x == 0u ? 64u : (uint32_t)__builtin_ctzll(x); }
static inline uint16_t cee_bswap_u16(uint16_t x) { return __builtin_bswap16(x); }
static inline uint32_t cee_bswap_u32(uint32_t x) { return __builtin_bswap32(x); }
static inline uint64_t cee_bswap_u64(uint64_t x) { return __builtin_bswap64(x); }
#else
CEE_BITS_UNSIGNED(uint32_t, u32, uint32_t, 32u)
CEE_BITS_UNSIGNED(uint64_t, u64, uint64_t, 64u)
static inline uint16_t cee_bswap_u16(uint16_t x) { return (uint16_t)(((uint32_t)x << 8) | ((uint32_t)x >> 8)); }
static inline uint32_t cee_bswap_u32(uint32_t x) {
    return (x << 24) | ((x & 0xFF00u) << 8) | ((x >> 8) & 0xFF00u) | (x >> 24);
}
static inline uint64_t cee_bswap_u64(uint64_t x) {
    return ((uint64_t)cee_bswap_u32((uint32_t)x) << 32) | (uint64_t)cee_bswap_u32((uint32_t)(x >> 32));
}
#endif
static inline uint8_t cee_bswap_u8(uint8_t x) { return x; }
static inline int8_t cee_bswap_i8(int8_t x) { return x; }
static inline int16_t cee_bswap_i16(int16_t x) { return cee_as_i16(cee_bswap_u16((uint16_t)x)); }
static inline int32_t cee_bswap_i32(int32_t x) { return cee_as_i32(cee_bswap_u32((uint32_t)x)); }
static inline int64_t cee_bswap_i64(int64_t x) { return cee_as_i64(cee_bswap_u64((uint64_t)x)); }

#if defined(CEE_FLOATS)
/* ---- Floats: representation and conversions (type-system.md §35.4–§35.6) ----------------------- */

/* A float from its bits and back; a union reinterprets without aliasing questions (C11 6.5.2.3). */
static inline float cee_f32_from_bits(uint32_t u) { union { uint32_t u; float f; } v; v.u = u; return v.f; }
static inline double cee_f64_from_bits(uint64_t u) { union { uint64_t u; double f; } v; v.u = u; return v.f; }
static inline uint32_t cee_f32_bits(float f) { union { uint32_t u; float f; } v; v.f = f; return v.u; }
static inline uint64_t cee_f64_bits(double f) { union { uint64_t u; double f; } v; v.f = f; return v.u; }

/* round<f32>(d): C leaves converting a finite double beyond FLT_MAX undefined (C11 6.3.1.5), so those values are
   rounded here: up to the midpoint 2^128 - 2^103 they round to FLT_MAX, from it on (ties to even) to infinity. */
static inline float cee_f64_to_f32(double d) {
    double limit = cee_f64_from_bits(0x47EFFFFFF0000000u); /* 2^128 - 2^103 */
    if (d > (double)FLT_MAX) return d >= limit ? cee_f32_from_bits(0x7F800000u) : FLT_MAX;
    if (d < -(double)FLT_MAX) return d <= -limit ? cee_f32_from_bits(0xFF800000u) : -FLT_MAX;
    return (float)d;
}

/* truncate<T>(f) and its variants for each integer type: `x` (an f32 promoted exactly, or an f64) converts only after
   it is known to lie strictly between MIN - 1 and MAX + 1, which C requires (C11 6.3.1.4). The bounds are exact
   doubles: MIN - 1 for 8 to 32 bits; for i64 MIN itself (no double lies between MIN - 1 and MIN); 2^N or 2^(N-1). */
#define CEE_FTOI(T, S, IN_LOW, HIGH, MIN, MAX)                                                             static inline bool cee_ftoi_fits_##S(double x) { return (IN_LOW) && x < (HIGH); }                     static inline T cee_ftoi_##S(double x, const cee_loc *loc) {                                                if (!cee_ftoi_fits_##S(x)) ceesafe_panic(CEE_PANIC_FLOAT_RANGE, *loc);                                     return (T)x;                                                                                      }                                                                                                     static inline bool cee_try_ftoi_##S(double x, T *r) {                                                      if (!cee_ftoi_fits_##S(x)) return false;                                                               *r = (T)x;                                                                                            return true;                                                                                      }                                                                                                     static inline T cee_sat_ftoi_##S(double x) {                                                               if (x != x) return 0;                                                                                 if (x < (HIGH) && !(IN_LOW)) return (MIN);                                                            if (!(x < (HIGH))) return (MAX);                                                                      return (T)x;                                                                                      }
CEE_FTOI(int8_t, i8, x > -129.0, 128.0, INT8_MIN, INT8_MAX)
CEE_FTOI(int16_t, i16, x > -32769.0, 32768.0, INT16_MIN, INT16_MAX)
CEE_FTOI(int32_t, i32, x > -2147483649.0, 2147483648.0, INT32_MIN, INT32_MAX)
CEE_FTOI(int64_t, i64, x >= -9223372036854775808.0, 9223372036854775808.0, INT64_MIN, INT64_MAX)
CEE_FTOI(uint8_t, u8, x > -1.0, 256.0, 0u, UINT8_MAX)
CEE_FTOI(uint16_t, u16, x > -1.0, 65536.0, 0u, UINT16_MAX)
CEE_FTOI(uint32_t, u32, x > -1.0, 4294967296.0, 0u, UINT32_MAX)
CEE_FTOI(uint64_t, u64, x > -1.0, 18446744073709551616.0, 0u, UINT64_MAX)

/* Math methods (type-system.md §35.8), never a libm call: CosmoOS has no libm, and baseline x86-64 has no instruction
   for floor or ceil (roundsd needs SSE4.1). */
#if defined(_MSC_VER) && !defined(__clang__)
#include <math.h> /* MSVC's sqrt is in its C runtime, which every program links */
#endif
static inline double cee_sqrt_f64(double x) {
#if defined(__clang__) && defined(__has_builtin)
#if __has_builtin(__builtin_elementwise_sqrt)
    return __builtin_elementwise_sqrt(x); /* the instruction; never sets errno */
#else
    return __builtin_sqrt(x);
#endif
#elif defined(__GNUC__)
    return __builtin_sqrt(x); /* with -fno-math-errno (ceesafec passes it), the instruction alone */
#else
    return sqrt(x);
#endif
}
static inline float cee_sqrt_f32(float x) {
#if defined(__clang__) && defined(__has_builtin)
#if __has_builtin(__builtin_elementwise_sqrt)
    return __builtin_elementwise_sqrt(x);
#else
    return __builtin_sqrtf(x);
#endif
#elif defined(__GNUC__)
    return __builtin_sqrtf(x);
#else
    return sqrtf(x);
#endif
}
static inline double cee_abs_f64(double x) { return cee_f64_from_bits(cee_f64_bits(x) & ~(UINT64_C(1) << 63)); }
static inline float cee_abs_f32(float x) { return cee_f32_from_bits(cee_f32_bits(x) & ~(UINT32_C(1) << 31)); }
/* The magnitude of m with the sign of s. */
static inline double cee_with_sign_f64(double m, double s) {
    return cee_f64_from_bits((cee_f64_bits(m) & ~(UINT64_C(1) << 63)) | (cee_f64_bits(s) & (UINT64_C(1) << 63)));
}
static inline float cee_with_sign_f32(float m, float s) {
    return cee_f32_from_bits((cee_f32_bits(m) & ~(UINT32_C(1) << 31)) | (cee_f32_bits(s) & (UINT32_C(1) << 31)));
}
/* floor and ceil: a value with |x| >= 2^52 (2^23), an infinity or NaN is already integral; any other converts to an
   integer exactly, toward zero, and one step corrects it. A zero result takes x's sign, as IEEE says. */
static inline double cee_floor_f64(double x) {
    if (!(x > -4503599627370496.0 && x < 4503599627370496.0)) return x;
    double t = (double)(int64_t)x;
    if (t > x) t -= 1.0;
    return t == 0.0 ? cee_with_sign_f64(t, x) : t;
}
static inline double cee_ceil_f64(double x) {
    if (!(x > -4503599627370496.0 && x < 4503599627370496.0)) return x;
    double t = (double)(int64_t)x;
    if (t < x) t += 1.0;
    return t == 0.0 ? cee_with_sign_f64(t, x) : t;
}
static inline float cee_floor_f32(float x) {
    if (!(x > -8388608.0f && x < 8388608.0f)) return x;
    float t = (float)(int32_t)x;
    if (t > x) t -= 1.0f;
    return t == 0.0f ? cee_with_sign_f32(t, x) : t;
}
static inline float cee_ceil_f32(float x) {
    if (!(x > -8388608.0f && x < 8388608.0f)) return x;
    float t = (float)(int32_t)x;
    if (t < x) t += 1.0f;
    return t == 0.0f ? cee_with_sign_f32(t, x) : t;
}
/* IEEE 754-2019 minimum and maximum: a NaN if either operand is one, and -0 below +0. */
static inline double cee_min_f64(double a, double b) {
    if (a != a || b != b) return a + b;
    if (a == b) return (cee_f64_bits(a) >> 63) != 0 ? a : b;
    return a < b ? a : b;
}
static inline double cee_max_f64(double a, double b) {
    if (a != a || b != b) return a + b;
    if (a == b) return (cee_f64_bits(a) >> 63) != 0 ? b : a;
    return a > b ? a : b;
}
static inline float cee_min_f32(float a, float b) {
    if (a != a || b != b) return a + b;
    if (a == b) return (cee_f32_bits(a) >> 31) != 0 ? a : b;
    return a < b ? a : b;
}
static inline float cee_max_f32(float a, float b) {
    if (a != a || b != b) return a + b;
    if (a == b) return (cee_f32_bits(a) >> 31) != 0 ? b : a;
    return a > b ? a : b;
}
#endif /* CEE_FLOATS */

/* ---- Bounds checks (type-system.md §16.3) ------------------------------------------------- */

static inline void cee_check_index(uint64_t index, uint64_t length, const cee_loc *loc) {
    if (index >= length) ceesafe_panic(CEE_PANIC_INDEX, *loc);
}

/* ---- CPU and target queries (type-system.md §29.3) ----------------------------------------- */

/* callerAddress() (ADR 0030): the address an export's wrapper returns to, taken in the wrapper. A macro, since a
 * function would name its own caller instead. */
#if defined(__GNUC__) || defined(__clang__)
#define cee_return_address() ((uint64_t)(uintptr_t)__builtin_return_address(0))
#elif defined(_MSC_VER)
#define cee_return_address() ((uint64_t)(uintptr_t)_ReturnAddress())
#else
#define cee_return_address() ((uint64_t)0)
#endif

/* A hint inside a busy-wait loop: no effect on the program's meaning. */
static inline void cee_spin_hint(void) {
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
    __builtin_ia32_pause();
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
    __asm__ __volatile__("yield");
#endif
}

/* The architecture the generated C is compiled for: one generated file serves every LP64 target. */
static inline bool cee_target_is_x86_64(void) {
#if defined(__x86_64__) || defined(_M_X64)
    return true;
#else
    return false;
#endif
}

static inline bool cee_target_is_aarch64(void) {
#if defined(__aarch64__) || defined(_M_ARM64)
    return true;
#else
    return false;
#endif
}

/* A constant index that its preceding check rejects: hides the constant from -Warray-bounds (c-backend.md §5). */
static inline uint64_t cee_opaque_index(uint64_t index) { return index; }

static inline void cee_check_range(uint64_t low, uint64_t high, uint64_t length, const cee_loc *loc) {
    if (low > high || high > length) ceesafe_panic(CEE_PANIC_SLICE, *loc);
}

/* ---- Bulk slice operations (type-system.md §16.5) ------------------------------------------ */

/*
 * Byte copies, moves and fills. The builtins may lower to calls to memcpy, memmove and memset, which every profile provides
 * (profiles.md §4.4); MSVC builds are hosted, so <string.h> is there.
 */
#if defined(__GNUC__) || defined(__clang__)
#define ceesafe_mem_copy __builtin_memcpy
#define ceesafe_mem_move __builtin_memmove
#define ceesafe_mem_set __builtin_memset
#else
#include <string.h>
#define ceesafe_mem_copy memcpy
#define ceesafe_mem_move memmove
#define ceesafe_mem_set memset
#endif

/* `d.copyFrom(s)`: the lengths must be equal. */
static inline void cee_check_length(uint64_t source, uint64_t destination, const cee_loc *loc) {
    if (source != destination) ceesafe_panic(CEE_PANIC_LENGTH, *loc);
}

/* `count` elements of `size` bytes, possibly overlapping. A slice from raw parts may be null when empty. */
static inline void cee_slice_copy(void *destination, const void *source, uint64_t count, size_t size) {
    if (count != 0u) ceesafe_mem_move(destination, source, (size_t)count * size);
}

/* `d.fill(v)` for a one-byte element. */
static inline void cee_slice_fill_byte(void *destination, uint8_t value, uint64_t count) {
    if (count != 0u) ceesafe_mem_set(destination, value, (size_t)count);
}

/* ---- String views (type-system.md §22.2) ------------------------------------------------- */

/* `StringView`: UTF-8 bytes, laid out like a `Slice<u8>`. */
typedef struct cee_str { const uint8_t *ptr; uint64_t len; } cee_str;

/* `panic(message)` and a failed `assert` (ADR 0075 §2.8): hosted, "ceesafe panic: <message> at <location>"; in a
   freestanding package, the environment's ceesafe_panic with CEE_PANIC_EXPLICIT (the message is not shown). */
CEE_COLD _Noreturn void ceesafe_panic_text(cee_str message, cee_loc loc);

/* True if `index` is not inside a UTF-8 multi-byte sequence (the end counts as a boundary). */
static inline bool cee_str_boundary(cee_str text, uint64_t index) {
    return index >= text.len || (text.ptr[index] & 0xC0u) != 0x80u;
}

/* `s.appendNumber(n)` (type-system.md §23.2): the decimal text of n, written at the end of buf, which has room for
   the longest (20 bytes: "-9223372036854775808", "18446744073709551615"); the view is of buf. */
#define CEE_DIGITS 20
static inline cee_str cee_format_u64(uint64_t value, uint8_t *buf) {
    uint8_t *p = buf + CEE_DIGITS;
    do {
        *--p = (uint8_t)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    return (cee_str){ .ptr = p, .len = (uint64_t)(buf + CEE_DIGITS - p) };
}
static inline cee_str cee_format_i64(int64_t value, uint8_t *buf) {
    /* The magnitude in unsigned arithmetic, so INT64_MIN negates without overflow. */
    uint64_t magnitude = value < 0 ? 0u - (uint64_t)value : (uint64_t)value;
    cee_str digits = cee_format_u64(magnitude, buf);
    if (value >= 0) return digits;
    buf[CEE_DIGITS - digits.len - 1u] = (uint8_t)'-';
    return (cee_str){ .ptr = buf + CEE_DIGITS - digits.len - 1u, .len = digits.len + 1u };
}

#if defined(CEE_FLOATS)
/* Console.write and String.appendNumber of a float (type-system.md §35.7): the shortest decimal that reads back as the
   same value, laid out as ECMAScript's Number::toString, written into buf (CEE_FLOAT_CHARS bytes); the view is of buf.
   runtime/hosted/ceesafe_rt_float.c, linked only into programs that print floats. */
#define CEE_FLOAT_CHARS 32
cee_str ceesafe_format_f64(double value, uint8_t *buf);
cee_str ceesafe_format_f32(float value, uint8_t *buf);
#endif

/* The range check of `s[lo..hi]`, then both bounds on character boundaries. */
static inline void cee_check_str_range(cee_str text, uint64_t low, uint64_t high, const cee_loc *loc) {
    cee_check_range(low, high, text.len, loc);
    if (!cee_str_boundary(text, low) || !cee_str_boundary(text, high)) ceesafe_panic(CEE_PANIC_STR_BOUNDARY, *loc);
}

static inline bool ceesafe_str_eq(cee_str a, cee_str b) {
    if (a.len != b.len) return false;
    for (uint64_t i = 0; i < a.len; i++) {
        if (a.ptr[i] != b.ptr[i]) return false;
    }
    return true;
}

/* `==` on arrays and slices of integers, bools and fieldless enums (ADR 0072 §2.4): their bytes, which have no
   padding. Not constant-time. */
static inline bool cee_mem_eq(const void *a, const void *b, size_t size) {
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    for (size_t i = 0; i < size; i++) {
        if (x[i] != y[i]) return false;
    }
    return true;
}

static inline bool cee_slice_eq(const void *a, uint64_t alen, const void *b, uint64_t blen, size_t size) {
    return alen == blen && (alen == 0 || cee_mem_eq(a, b, (size_t)alen * size));
}

/* ---- Platform services (supplied by the profile runtime) -------------------------------- */

void ceesafe_platform_write_stdout(const uint8_t *bytes, size_t length);
void ceesafe_platform_write_stderr(const uint8_t *bytes, size_t length);
_Noreturn void ceesafe_platform_exit(int32_t code);
_Noreturn void ceesafe_platform_abort(void);
/* Heap memory; NULL on failure. Must be aligned for any CeeSafe type (max_align_t). */
void *ceesafe_platform_alloc(size_t size);
/* The same, with every byte zero (a C library's calloc can skip zeroing memory that already is). */
void *ceesafe_platform_alloc_zeroed(size_t size);
/* Resizes a block from ceesafe_platform_alloc, keeping its first bytes; NULL with the block unchanged on failure. */
void *ceesafe_platform_realloc(void *p, size_t size);
void ceesafe_platform_free(void *p);
/* A configuration number from the environment; 0 if absent or not a number (hosted test builds). */
uint64_t ceesafe_platform_config(const char *name);

/* ---- Heap (type-system.md §18; runtime/core/ceesafe_rt_core.c) ------------------------------ */

/* Allocator types, as passed by generated code. */
#define CEE_ALLOC_SYSTEM 0u
#define CEE_ALLOC_FAILING 1u
/* CEE_ALLOC_FOREIGN (2u, for C code: ADR 0060), CEE_ALLOC_FOREIGN_INIT (3u, a C library's one-time initialisation:
   ADR 0061) and the three calls are declared in ceesafe/foreign.h, which a package's C files include on their own. */
#include "foreign.h" /* beside this header, whatever the include path */

/*
 * Allocation for Box and every later heap type. Every attempt is counted, and the attempt numbered
 * CEESAFE_FAIL_ALLOC_AT (1-based, all allocators) fails: the fault-injection hook for the M3 sweep.
 */
/* ceesafe_alloc(size_t size, uint32_t allocator): in ceesafe/foreign.h. */
/* ceesafe_alloc with every byte zero: for a box whose value is all zero bytes, such as `[0u8; N]`. Counted and
   fault-injected like ceesafe_alloc, freed with ceesafe_free. */
void *ceesafe_alloc_zeroed(size_t size, uint32_t allocator);
/* ceesafe_realloc (one attempt, the block unchanged and still live on failure) and ceesafe_free: in
   ceesafe/foreign.h. */
/* FailingAllocator.failAfter(n) / FailingAllocator.reset(). */
void ceesafe_fail_after(uint64_t n);
void ceesafe_fail_reset(void);
/* `ceesafec test`'s harness (ADR 0049): turns the allocation report and the cancellation report on, and sets the
   allocation that fails and the suspension that cancels (0: none), overriding CEESAFE_FAIL_ALLOC_AT and
   CEESAFE_CANCEL_AT. ceesafe_thread_watchdog ends the process with status 124 after `ms` (0: never). */
void ceesafe_test_begin(uint64_t fail_alloc_at, uint64_t cancel_at);
extern uint64_t ceesafe_test_cancel_at;
extern bool ceesafe_test_reports;
void ceesafe_thread_watchdog(uint64_t ms);

/* ---- Threads and mutexes (type-system.md §38; runtime/hosted/ceesafe_rt_thread.c), and atomics (here) ---------- */

typedef struct cee_thread cee_thread;
/* Starts a thread running body(arg); NULL if it or its record could not be made. */
cee_thread *ceesafe_thread_start(void *(*body)(void *), void *arg);
/* Waits for the thread to end, then frees its record. */
void ceesafe_thread_join(cee_thread *t);
/* A platform mutex in allocated memory, or NULL. */
void *ceesafe_mutex_create(void);
void ceesafe_mutex_lock(void *m);
void ceesafe_mutex_unlock(void *m);
void ceesafe_mutex_destroy(void *m);
/* ---- Atomics (§38.3): sequentially consistent, and with an ordering (ADR 0021); compare-exchange returns the
 * previous value. Static inline (ADR 0026), so an operation is its instructions where the compiler inlines it, and
 * a freestanding build needs no runtime for them. An ordering is lib/core's code: 0 relaxed, 1 acquire, 2 release, 3 acquire-release, 4 sequentially
 * consistent. lib/core passes a load only 0, 1 or 4 and a store only 0, 2 or 4, so no ordering C11 leaves undefined
 * reaches a builtin. --------------------------------------------------------------------------------------------- */

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
static inline uint64_t ceesafe_atomic_load_u64(uint64_t *p) { return (uint64_t)_InterlockedCompareExchange64((volatile __int64 *)p, 0, 0); }
static inline void ceesafe_atomic_store_u64(uint64_t *p, uint64_t v) { (void)_InterlockedExchange64((volatile __int64 *)p, (__int64)v); }
static inline uint64_t ceesafe_atomic_swap_u64(uint64_t *p, uint64_t v) { return (uint64_t)_InterlockedExchange64((volatile __int64 *)p, (__int64)v); }
static inline uint64_t ceesafe_atomic_cas_u64(uint64_t *p, uint64_t expected, uint64_t desired) {
    return (uint64_t)_InterlockedCompareExchange64((volatile __int64 *)p, (__int64)desired, (__int64)expected);
}
static inline uint64_t ceesafe_atomic_add_u64(uint64_t *p, uint64_t v) { return (uint64_t)_InterlockedExchangeAdd64((volatile __int64 *)p, (__int64)v); }
static inline uint32_t ceesafe_atomic_load_u32(uint32_t *p) { return (uint32_t)_InterlockedCompareExchange((volatile long *)p, 0, 0); }
static inline void ceesafe_atomic_store_u32(uint32_t *p, uint32_t v) { (void)_InterlockedExchange((volatile long *)p, (long)v); }
static inline uint32_t ceesafe_atomic_swap_u32(uint32_t *p, uint32_t v) { return (uint32_t)_InterlockedExchange((volatile long *)p, (long)v); }
static inline uint32_t ceesafe_atomic_cas_u32(uint32_t *p, uint32_t expected, uint32_t desired) {
    return (uint32_t)_InterlockedCompareExchange((volatile long *)p, (long)desired, (long)expected);
}
static inline uint32_t ceesafe_atomic_add_u32(uint32_t *p, uint32_t v) { return (uint32_t)_InterlockedExchangeAdd((volatile long *)p, (long)v); }
/* fetchOr and fetchAnd (ADR 0041). */
static inline uint64_t ceesafe_atomic_or_u64(uint64_t *p, uint64_t v) { return (uint64_t)_InterlockedOr64((volatile __int64 *)p, (__int64)v); }
static inline uint64_t ceesafe_atomic_and_u64(uint64_t *p, uint64_t v) { return (uint64_t)_InterlockedAnd64((volatile __int64 *)p, (__int64)v); }
static inline uint32_t ceesafe_atomic_or_u32(uint32_t *p, uint32_t v) { return (uint32_t)_InterlockedOr((volatile long *)p, (long)v); }
static inline uint32_t ceesafe_atomic_and_u32(uint32_t *p, uint32_t v) { return (uint32_t)_InterlockedAnd((volatile long *)p, (long)v); }
/* Every _Interlocked intrinsic is a full barrier: a correct, stronger, implementation of each ordering. */
static inline uint64_t ceesafe_atomic_load_ord_u64(uint64_t *p, int32_t o) { (void)o; return ceesafe_atomic_load_u64(p); }
static inline void ceesafe_atomic_store_ord_u64(uint64_t *p, uint64_t v, int32_t o) { (void)o; ceesafe_atomic_store_u64(p, v); }
static inline uint64_t ceesafe_atomic_swap_ord_u64(uint64_t *p, uint64_t v, int32_t o) { (void)o; return ceesafe_atomic_swap_u64(p, v); }
static inline uint64_t ceesafe_atomic_cas_ord_u64(uint64_t *p, uint64_t e, uint64_t d, int32_t o) { (void)o; return ceesafe_atomic_cas_u64(p, e, d); }
static inline uint64_t ceesafe_atomic_add_ord_u64(uint64_t *p, uint64_t v, int32_t o) { (void)o; return ceesafe_atomic_add_u64(p, v); }
static inline uint32_t ceesafe_atomic_load_ord_u32(uint32_t *p, int32_t o) { (void)o; return ceesafe_atomic_load_u32(p); }
static inline void ceesafe_atomic_store_ord_u32(uint32_t *p, uint32_t v, int32_t o) { (void)o; ceesafe_atomic_store_u32(p, v); }
static inline uint32_t ceesafe_atomic_swap_ord_u32(uint32_t *p, uint32_t v, int32_t o) { (void)o; return ceesafe_atomic_swap_u32(p, v); }
static inline uint32_t ceesafe_atomic_cas_ord_u32(uint32_t *p, uint32_t e, uint32_t d, int32_t o) { (void)o; return ceesafe_atomic_cas_u32(p, e, d); }
static inline uint32_t ceesafe_atomic_add_ord_u32(uint32_t *p, uint32_t v, int32_t o) { (void)o; return ceesafe_atomic_add_u32(p, v); }
static inline uint64_t ceesafe_atomic_or_ord_u64(uint64_t *p, uint64_t v, int32_t o) { (void)o; return ceesafe_atomic_or_u64(p, v); }
static inline uint64_t ceesafe_atomic_and_ord_u64(uint64_t *p, uint64_t v, int32_t o) { (void)o; return ceesafe_atomic_and_u64(p, v); }
static inline uint32_t ceesafe_atomic_or_ord_u32(uint32_t *p, uint32_t v, int32_t o) { (void)o; return ceesafe_atomic_or_u32(p, v); }
static inline uint32_t ceesafe_atomic_and_ord_u32(uint32_t *p, uint32_t v, int32_t o) { (void)o; return ceesafe_atomic_and_u32(p, v); }
#else
static inline uint64_t ceesafe_atomic_load_u64(uint64_t *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static inline void ceesafe_atomic_store_u64(uint64_t *p, uint64_t v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
static inline uint64_t ceesafe_atomic_swap_u64(uint64_t *p, uint64_t v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
static inline uint64_t ceesafe_atomic_cas_u64(uint64_t *p, uint64_t expected, uint64_t desired) {
    (void)__atomic_compare_exchange_n(p, &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected; /* the previous value: equal to the argument exactly when the exchange happened */
}
static inline uint64_t ceesafe_atomic_add_u64(uint64_t *p, uint64_t v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }
static inline uint32_t ceesafe_atomic_load_u32(uint32_t *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static inline void ceesafe_atomic_store_u32(uint32_t *p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
static inline uint32_t ceesafe_atomic_swap_u32(uint32_t *p, uint32_t v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
static inline uint32_t ceesafe_atomic_cas_u32(uint32_t *p, uint32_t expected, uint32_t desired) {
    (void)__atomic_compare_exchange_n(p, &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}
static inline uint32_t ceesafe_atomic_add_u32(uint32_t *p, uint32_t v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }
/* fetchOr and fetchAnd (ADR 0041). */
static inline uint64_t ceesafe_atomic_or_u64(uint64_t *p, uint64_t v) { return __atomic_fetch_or(p, v, __ATOMIC_SEQ_CST); }
static inline uint64_t ceesafe_atomic_and_u64(uint64_t *p, uint64_t v) { return __atomic_fetch_and(p, v, __ATOMIC_SEQ_CST); }
static inline uint32_t ceesafe_atomic_or_u32(uint32_t *p, uint32_t v) { return __atomic_fetch_or(p, v, __ATOMIC_SEQ_CST); }
static inline uint32_t ceesafe_atomic_and_u32(uint32_t *p, uint32_t v) { return __atomic_fetch_and(p, v, __ATOMIC_SEQ_CST); }

/* A builtin's ordering must be a constant for the compiler to choose its instructions (a variable one is treated as
 * sequentially consistent), so each entry point switches. A compare-exchange fails with the ordering C11 allows for
 * its success ordering: acquire for acquire-release, relaxed for release. */
#define CEE_LOAD_ORD(T, W)                                                                                            \
    static inline T ceesafe_atomic_load_ord_##W(T *p, int32_t o) {                                                                    \
        switch (o) {                                                                                                 \
        case 0: return __atomic_load_n(p, __ATOMIC_RELAXED);                                                         \
        case 1: return __atomic_load_n(p, __ATOMIC_ACQUIRE);                                                         \
        default: return __atomic_load_n(p, __ATOMIC_SEQ_CST);                                                        \
        }                                                                                                            \
    }
#define CEE_STORE_ORD(T, W)                                                                                           \
    static inline void ceesafe_atomic_store_ord_##W(T *p, T v, int32_t o) {                                                           \
        switch (o) {                                                                                                 \
        case 0: __atomic_store_n(p, v, __ATOMIC_RELAXED); break;                                                     \
        case 2: __atomic_store_n(p, v, __ATOMIC_RELEASE); break;                                                     \
        default: __atomic_store_n(p, v, __ATOMIC_SEQ_CST); break;                                                    \
        }                                                                                                            \
    }
#define CEE_RMW_ORD(T, W, NAME, CALL)                                                                                 \
    static inline T ceesafe_atomic_##NAME##_ord_##W(T *p, T v, int32_t o) {                                                           \
        switch (o) {                                                                                                 \
        case 0: return CALL(p, v, __ATOMIC_RELAXED);                                                                 \
        case 1: return CALL(p, v, __ATOMIC_ACQUIRE);                                                                 \
        case 2: return CALL(p, v, __ATOMIC_RELEASE);                                                                 \
        case 3: return CALL(p, v, __ATOMIC_ACQ_REL);                                                                 \
        default: return CALL(p, v, __ATOMIC_SEQ_CST);                                                                \
        }                                                                                                            \
    }
#define CEE_CAS_ORD(T, W)                                                                                             \
    static inline T ceesafe_atomic_cas_ord_##W(T *p, T e, T d, int32_t o) {                                                           \
        switch (o) {                                                                                                 \
        case 0: (void)__atomic_compare_exchange_n(p, &e, d, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED); break;       \
        case 1: (void)__atomic_compare_exchange_n(p, &e, d, false, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE); break;       \
        case 2: (void)__atomic_compare_exchange_n(p, &e, d, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED); break;       \
        case 3: (void)__atomic_compare_exchange_n(p, &e, d, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE); break;       \
        default: (void)__atomic_compare_exchange_n(p, &e, d, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); break;      \
        }                                                                                                            \
        return e;                                                                                                    \
    }
CEE_LOAD_ORD(uint64_t, u64)
CEE_STORE_ORD(uint64_t, u64)
CEE_RMW_ORD(uint64_t, u64, swap, __atomic_exchange_n)
CEE_RMW_ORD(uint64_t, u64, add, __atomic_fetch_add)
CEE_RMW_ORD(uint64_t, u64, or, __atomic_fetch_or)
CEE_RMW_ORD(uint64_t, u64, and, __atomic_fetch_and)
CEE_CAS_ORD(uint64_t, u64)
CEE_LOAD_ORD(uint32_t, u32)
CEE_STORE_ORD(uint32_t, u32)
CEE_RMW_ORD(uint32_t, u32, swap, __atomic_exchange_n)
CEE_RMW_ORD(uint32_t, u32, add, __atomic_fetch_add)
CEE_RMW_ORD(uint32_t, u32, or, __atomic_fetch_or)
CEE_RMW_ORD(uint32_t, u32, and, __atomic_fetch_and)
CEE_CAS_ORD(uint32_t, u32)
#undef CEE_LOAD_ORD
#undef CEE_STORE_ORD
#undef CEE_RMW_ORD
#undef CEE_CAS_ORD
#endif

/* ---- Vectors and strings (type-system.md §23; runtime/core/ceesafe_rt_core.c) ---------------- */

/*
 * The buffer address of every empty vector: valid and aligned for any element type (elements are
 * at most max_align_t-aligned, asserted by the generated helpers), and never dereferenced.
 */
extern cee_max_align_t ceesafe_vec_empty;

/*
 * Room for `extra` more elements of `size` bytes after the `len` in use. Returns the buffer to use:
 * `ptr` itself if it has room; otherwise a new one (capacity doubled, at least 4 and at least
 * len + extra) holding the old bytes, after the old one (if any) is freed, with *cap updated.
 * NULL, with nothing changed, if the size overflows or the allocation fails.
 */
void *ceesafe_vec_grow(void *ptr, uint64_t len, uint64_t *cap, uint64_t extra, uint64_t size, uint32_t allocator);

/* ---- Stable vectors and slot maps (type-system.md §24) ------------------------------------ */

/* A StableVector's 32 chunks hold 8, 16, 32, ... elements: element i is in chunk log2(i + 8) - 3. */
#define CEE_SVEC_CHUNKS 32u

/* Leading zero bits of a nonzero value: the builtin where GCC or clang has it, else a loop (MSVC, c-backend.md §1.1). */
static inline unsigned cee_clz64(uint64_t x) {
#if defined(__GNUC__) || defined(__clang__)
    return (unsigned)__builtin_clzll(x);
#else
    unsigned n = 0;
    while ((x & ((uint64_t)1 << 63)) == 0) {
        x <<= 1;
        n++;
    }
    return n;
#endif
}

/* The address of element `i` (i < length) of a stable vector with elements of `size` bytes. */
static inline void *ceesafe_svec_at(void *const *chunks, uint64_t i, uint64_t size) {
    uint64_t j = i + 8u;
    unsigned k = 60u - cee_clz64(j);
    return (uint8_t *)chunks[k] + (j - ((uint64_t)8u << k)) * size;
}

/* The slot for element `len`, allocating its chunk if it is the chunk's first; NULL if full or out of memory. */
void *ceesafe_svec_slot(void **chunks, uint64_t len, uint64_t size, uint32_t allocator);

/* Destroys nothing: frees every chunk of a stable vector. */
void ceesafe_svec_free(void **chunks, uint32_t allocator);

/* `Handle<T>`: a slot index and the generation it was given (odd: a live value). */
typedef struct cee_handle { uint32_t index; uint32_t gen; } cee_handle;

static inline bool ceesafe_handle_eq(cee_handle a, cee_handle b) { return a.index == b.index && a.gen == b.gen; }
/* ---- Arena (type-system.md §19.2; runtime/core/ceesafe_rt_core.c) ---------------------------- */

/*
 * A bump allocator over a list of chunks from a free-standing allocator. Layout fixed by CeeSafe's
 * TypeTable (five 8-byte fields: 40 bytes, alignment 8 on LP64).
 */
typedef struct cee_arena {
    void *chunks;         /* most recent chunk; each chunk starts with the previous chunk's address */
    uint8_t *cur;         /* next free byte in the current chunk */
    uint8_t *end;         /* end of the current chunk */
    uint64_t chunk_size;  /* bytes per chunk (larger allocations get their own) */
    uint64_t allocator;   /* CEE_ALLOC_* */
} cee_arena;
#if UINTPTR_MAX == UINT64_MAX
_Static_assert(sizeof(cee_arena) == 40 && _Alignof(cee_arena) == 8, "ceesafe/rt.h: cee_arena layout");
#endif

cee_arena ceesafe_arena_create(uint32_t allocator, uint64_t chunk_size);
/* `size` bytes aligned to `align` (a power of two ≤ 16); NULL if no chunk can be obtained. */
void *ceesafe_arena_alloc(cee_arena *a, uint64_t size, uint64_t align);
/* Releases every chunk; the arena stays usable. Drop = reset. */
void ceesafe_arena_reset(cee_arena *a);
void ceesafe_arena_drop(cee_arena *a);

/* ---- Fixed-buffer allocator and scratch frames (type-system.md §19.4-19.5) ---------------- */

/* Allocation from caller-provided bytes; nothing is freed. 24 bytes on LP64. */
typedef struct cee_fba {
    uint8_t *base;
    uint64_t capacity;
    uint64_t used;
} cee_fba;

/*
 * A scratch frame: the scratch stack's state when the frame was entered, which its drop restores.
 * The scratch stack itself (runtime-owned heap chunks) is global; v0 is single-threaded. 32 bytes.
 */
typedef struct cee_scratch {
    void *chunk;
    uint8_t *cur;
    uint8_t *end;
    uint64_t depth;
} cee_scratch;
#if UINTPTR_MAX == UINT64_MAX
_Static_assert(sizeof(cee_fba) == 24 && _Alignof(cee_fba) == 8, "ceesafe/rt.h: cee_fba layout");
_Static_assert(sizeof(cee_scratch) == 32 && _Alignof(cee_scratch) == 8, "ceesafe/rt.h: cee_scratch layout");
#endif

cee_fba ceesafe_fba_over(uint8_t *base, uint64_t capacity);
/* Panics if a scratch frame is already active: nested frames come from ceesafe_scratch_enter_child. */
cee_scratch ceesafe_scratch_enter_root(cee_loc loc);
cee_scratch ceesafe_scratch_enter_child(cee_scratch *parent);
void ceesafe_scratch_drop(cee_scratch *frame);

/* ---- Pools (type-system.md §19.6) ------------------------------------------------------- */

/* Up to `capacity` slots of one size, allocated at the first take. 48 bytes on LP64. */
typedef struct cee_pool {
    uint8_t *slots;
    void *free;           /* free list through the slots; NULL with `used < capacity` means untouched slots */
    uint64_t capacity;
    uint64_t used;        /* slots handed out at least once */
    uint64_t slot_size;
    uint64_t allocator;
} cee_pool;
#if UINTPTR_MAX == UINT64_MAX
_Static_assert(sizeof(cee_pool) == 48 && _Alignof(cee_pool) == 8, "ceesafe/rt.h: cee_pool layout");
#endif

cee_pool ceesafe_pool_create(uint32_t allocator, uint64_t capacity);
/* A free slot of `size` bytes (the same for every call on a pool), aligned to 16; NULL if none. */
void *ceesafe_pool_take(cee_pool *p, uint64_t size);
void ceesafe_pool_release(cee_pool *p, void *slot);
void ceesafe_pool_drop(cee_pool *p);

/* Allocation from any region (the generated helpers' single entry point); NULL on failure. */
#define CEE_REGION_ARENA 0u
#define CEE_REGION_FBA 1u
#define CEE_REGION_SCRATCH 2u
void *ceesafe_region_alloc(void *region, uint32_t kind, uint64_t size, uint64_t align);

/*
 * What the generated helpers call: an arena's bump within its current chunk inline (the same test and result as
 * ceesafe_arena_alloc's fast path), everything else, a new chunk included, through ceesafe_region_alloc. The helpers
 * pass `kind` as a constant once inlined, so the test folds away for the other regions.
 */
static inline void *ceesafe_region_alloc_fast(void *region, uint32_t kind, uint64_t size, uint64_t align) {
    if (kind == CEE_REGION_ARENA) {
        cee_arena *a = (cee_arena *)region;
        uintptr_t cur = (uintptr_t)a->cur;
        uintptr_t aligned = (cur + (uintptr_t)(align - 1u)) & ~(uintptr_t)(align - 1u);
        if (a->cur != NULL && aligned >= cur && size <= (uint64_t)((uintptr_t)a->end - aligned)) {
            a->cur = (uint8_t *)(aligned + (uintptr_t)size);
            return (void *)aligned;
        }
    }
    return ceesafe_region_alloc(region, kind, size, align);
}

/* Called by the generated `main`: configuration at start; the CEESAFE_ALLOC_REPORT line at the end. */
void ceesafe_rt_start(void);
int32_t ceesafe_rt_finish(int32_t code);

/* ---- Output and process control (runtime/core/ceesafe_rt_core.c) --------------------------- */

void ceesafe_write_i64(int64_t value);
void ceesafe_write_u64(uint64_t value);
void ceesafe_write_bool(bool value);
void ceesafe_write_str(const char *text, size_t length);
void ceesafe_write_view(cee_str text);
/* Console.writeBytes: raw bytes to standard output, in order with the other writes. */
void ceesafe_write_bytes(const uint8_t *bytes, uint64_t length);
/* Console.writeError: text to standard error (standard output is flushed first). */
void ceesafe_write_error(cee_str text);

/* ---- Program arguments, files and sockets (type-system.md §26; runtime/hosted/ceesafe_rt_io.c) ---- */

/* The outcome of an I/O call: `error` is 0 on success (then `value` is the result), else the C library's errno. */
typedef struct cee_io_result { int64_t value; int32_t error; } cee_io_result;

/* IoError's variants, in declaration order (the checker declares the enum in this order). */
#define CEE_IO_NOT_FOUND 0u
#define CEE_IO_PERMISSION_DENIED 1u
#define CEE_IO_ALREADY_EXISTS 2u
#define CEE_IO_IS_DIRECTORY 3u
#define CEE_IO_NOT_DIRECTORY 4u
#define CEE_IO_INVALID_INPUT 5u
#define CEE_IO_BROKEN_PIPE 6u
#define CEE_IO_CONNECTION_REFUSED 7u
#define CEE_IO_ADDRESS_IN_USE 8u
#define CEE_IO_OUT_OF_MEMORY 9u
#define CEE_IO_OTHER 10u
/* ADR 0044: appended after Other, so the kinds above keep their numbers. */
#define CEE_IO_CONNECTION_RESET 11u
#define CEE_IO_CONNECTION_ABORTED 12u
#define CEE_IO_NOT_CONNECTED 13u
#define CEE_IO_TIMED_OUT 14u
#define CEE_IO_HOST_UNREACHABLE 15u
#define CEE_IO_NETWORK_UNREACHABLE 16u
#define CEE_IO_ADDRESS_NOT_AVAILABLE 17u
#define CEE_IO_UNSUPPORTED 18u
/* ADR 0060: after Unsupported. The resolver's "no such host" is the runtime-only error number below, which no
   system's errno uses; ceesafe_io_kind maps it to CEE_IO_HOST_NOT_FOUND. */
#define CEE_IO_HOST_NOT_FOUND 19u
#define CEE_ERROR_HOST_NOT_FOUND 65536
/* ADR 0077: after HostNotFound. ENOSPC, and EDQUOT where the system has it; Windows' ERROR_DISK_FULL. */
#define CEE_IO_STORAGE_FULL 20u

/* The IoError variant for an errno value (hosted), and its message (core: static text). */
uint32_t ceesafe_io_kind(int32_t error);
cee_str ceesafe_io_message(uint32_t kind);

/* Called by the generated `main` before anything else: keeps argv for Process.argument (never copied). */
void ceesafe_rt_args(int32_t count, const uint8_t *const *arguments);
uint64_t ceesafe_arg_count(void);
/* Argument `index` as a view; false if out of range or not valid UTF-8. */
bool ceesafe_arg(uint64_t index, cee_str *out);
bool ceesafe_utf8_valid(const uint8_t *bytes, uint64_t length);
/* Host services (type-system.md §26.7-§26.10, ADR 0028; lib/std/host.cee): a value >= 0, or a negated errno. The types
 * are written as ceesafec writes lib/std's extern declarations, which repeat them. */
int64_t ceesafe_host_env(uint8_t const *name, uint64_t length, uint8_t const **value, uint64_t *value_length);
int64_t ceesafe_host_path_kind(uint8_t const *path, uint64_t length);
int64_t ceesafe_host_dir_create(uint8_t const *path, uint64_t length);
int64_t ceesafe_host_dir_remove(uint8_t const *path, uint64_t length);
void *ceesafe_host_dir_open(uint8_t const *path, uint64_t length, int32_t *error);
int64_t ceesafe_host_dir_next(void *dir, uint8_t const **name, uint64_t *length);
void ceesafe_host_dir_close(void *dir);
int64_t ceesafe_host_temporary(uint8_t *buffer, uint64_t capacity);
int64_t ceesafe_host_current(uint8_t *buffer, uint64_t capacity);
int64_t ceesafe_host_spawn(void const *arguments, uint64_t count, uint8_t const *input, uint64_t input_length,
                        uint8_t const *output, uint64_t output_length, uint8_t const *errors, uint64_t errors_length);

/* Files: `mode` 0 opens for reading, 1 creates or truncates for writing; 2 opens for reading and writing, 3 also
   creates a missing file, 4 creates a new one (ADR 0075 §2.1); `value` is the descriptor. */
cee_io_result ceesafe_file_open(cee_str path, uint32_t mode);
cee_io_result ceesafe_file_remove(cee_str path);
/* Reads up to `length` bytes (0 at the end); retries interrupted calls. */
cee_io_result ceesafe_fd_read(int32_t fd, uint8_t *buffer, uint64_t length);
/* Writes all `length` bytes, or fails. */
cee_io_result ceesafe_fd_write(int32_t fd, const uint8_t *bytes, uint64_t length);
/* Positional I/O, syncs, length (ADR 0075 §2.1); `all` 1 syncs all metadata. */
cee_io_result ceesafe_fd_read_at(int32_t fd, uint8_t *buffer, uint64_t length, uint64_t offset);
cee_io_result ceesafe_fd_write_at(int32_t fd, const uint8_t *bytes, uint64_t length, uint64_t offset);
cee_io_result ceesafe_fd_sync(int32_t fd, uint32_t all);
cee_io_result ceesafe_fd_length(int32_t fd);
cee_io_result ceesafe_fd_set_length(int32_t fd, uint64_t length);
uint64_t ceesafe_clock_monotonic(void);
int64_t ceesafe_host_rename(uint8_t const *from, uint64_t fromLength, uint8_t const *to, uint64_t toLength);
int64_t ceesafe_host_dir_sync(uint8_t const *path, uint64_t length);
int64_t ceesafe_host_space(uint8_t const *path, uint64_t length, uint64_t *total, uint64_t *free, uint64_t *available);
int64_t ceesafe_host_modified(uint8_t const *path, uint64_t length, int64_t *seconds, uint32_t *nanoseconds);
int64_t ceesafe_host_set_permissions(uint8_t const *path, uint64_t length, uint32_t mode);
int64_t ceesafe_host_console_flush(void);
/* The drop glue of File, TcpListener and TcpStream: closes the descriptor, ignoring errors. */
void ceesafe_fd_drop(int32_t *fd);

/* TCP over IPv4: `address` is dotted text ("127.0.0.1"); `port` 0 binds any free port. */
cee_io_result ceesafe_tcp_listen(cee_str address, uint16_t port);
cee_io_result ceesafe_tcp_port(int32_t fd);
cee_io_result ceesafe_tcp_accept(int32_t fd);
cee_io_result ceesafe_tcp_connect(cee_str address, uint16_t port);
cee_io_result ceesafe_tcp_shutdown_write(int32_t fd);
/* ADR 0044: the blocking stream's options (CEE_SOCKET_OPTION_*, below) and its read side's shutdown. */
cee_io_result ceesafe_tcp_option(int32_t fd, uint32_t option, int64_t value);
cee_io_result ceesafe_tcp_shutdown_read(int32_t fd);

/* Process.run: starts `program` (searched on PATH) with `arguments`, waits, and returns its exit status
   (128 + the signal number if a signal ended it). Standard output is flushed first. */
cee_io_result ceesafe_process_run(cee_str program, const cee_str *arguments, uint64_t count);
void ceesafe_write_newline(void);
_Noreturn void ceesafe_exit(int32_t code);

/* Async tasks (type-system.md §40; runtime/hosted/ceesafe_rt_async.c, c-backend.md §3a). ceesafe_async_run runs the
 * root task and the children its scopes spawn until the root returns, waiting while no task can run for what the parked
 * tasks registered: a deadline on the monotonic clock (milliseconds) or a descriptor becoming writable. ceesafe_async_write writes at most PIPE_BUF bytes
 * if `fd` is writable now: a count, 0 if it is not, or a negated errno. */
/* What a debugger shows of an async fn's task (c-backend.md §3a): its name, file, and the source line of each resume
 * state (lines[0] the fn's own line; lines[k] suspension point k's). ceesafe_async_tasks describes the current thread's
 * tasks, one line each: the task's own async fn at the line it last suspended at, children indented under the task
 * whose scope they are in. The text is in a static buffer, for a debugger to read; ceesafe_async_dump prints it to
 * standard error. */
typedef struct ceesafe_frame_info {
    const char *name;
    const char *file;
    const uint32_t *lines;
    uint32_t count;
} ceesafe_frame_info;
void ceesafe_async_run(void *frame, bool (*resume)(void *), const ceesafe_frame_info *info);
const char *ceesafe_async_tasks(void);
void ceesafe_async_dump(void);
/* A scope (type-system.md §40.5), the first member of the generated struct in the awaiting task's frame that also holds
 * its first error. Zero is an empty scope. ceesafe_async_spawn allocates a child task with a frame of `frame_size`
 * bytes and queues it, returning the frame for the caller to initialise before the child first runs; NULL if it cannot
 * be allocated, which sets `oom`. `resume` runs the child and, once it returns, records its result in the scope.
 * ceesafe_async_scope_done is true once every child has returned; otherwise the current task waits for the last. */
typedef struct ceesafe_scope {
    uint32_t live;
    uint32_t cancelled; /* a child failed or could not start: the others are cancelled (§40.7); read atomically */
    bool failed;
    bool oom;
    bool shared;        /* it has spawnSend children, which other workers may run: its state changes under the run's
                           lock (ADR 0038) */
    bool set;           /* a task set's (ADR 0053): a child's error cancels no sibling, a failed allocation stops
                           nothing */
    bool listed;        /* on its owner's list of scopes (ADR 0062): from its first child until it is done */
    void *waiter;
    void *owner;        /* the task whose scope it is */
    void *children;     /* its live children that run on this worker, linked by their siblings (ADR 0062) */
    void *outer;        /* the next scope on its owner's list (ADR 0062) */
} ceesafe_scope;
/* `allocate` and `release` (both NULL: the system heap) are Async.scopeIn's allocator's (c-backend.md §3a). */
void *ceesafe_async_spawn(ceesafe_scope *scope, size_t frame_size, bool (*resume)(void *frame, ceesafe_scope *scope),
                          void *(*allocate)(size_t size, size_t align), void (*release)(void *p, size_t size, size_t align),
                          const ceesafe_frame_info *info);
bool ceesafe_async_scope_done(ceesafe_scope *scope);
/* Work stealing (ADR 0038). ceesafe_async_spawn_send is ceesafe_async_spawn for `s.spawnSend(child)`: with more than
 * one worker, the child is queued for any worker of the run once ceesafe_async_publish(frame) says its frame is
 * initialised (with one worker it is spawn, and publish does nothing). ceesafe_async_claim_failure is true for the
 * first child of a scope to fail, which records its error. ceesafe_async_workers(n) gives the next ceesafe_async_run
 * on this thread n workers, the caller among them (Async.runWorkers). */
void *ceesafe_async_spawn_send(ceesafe_scope *scope, size_t frame_size, bool (*resume)(void *frame, ceesafe_scope *scope),
                               void *(*allocate)(size_t size, size_t align),
                               void (*release)(void *p, size_t size, size_t align), const ceesafe_frame_info *info);
void ceesafe_async_publish(void *frame);
bool ceesafe_async_claim_failure(ceesafe_scope *scope);
void ceesafe_async_workers(uint32_t n);
/* Task sets (ADR 0053): a scope whose children are started while it runs, at most `limit` live at once, from the task
 * that owns it only (a child cannot capture the set's handle), so one task at most waits for room. The record is in
 * the frame of lib/core's AsyncTasks.run. ceesafe_async_set_init empties it, with the slot where a child's first error
 * is copied (a Result<void, E>). ceesafe_async_set_admit is 0 when a child may start, 2 once the set or its task is
 * cancelled, and 1 when `limit` children run: then, with `wait`, the task is registered to be woken when one returns
 * (it parks, and asks again). ceesafe_async_set_spawn starts a child as ceesafe_async_spawn does, its frame from the
 * set's allocator (ceesafe_async_set_allocator; NULL: the system heap); NULL if it cannot be allocated, which
 * ceesafe_async_set_oom then reports. A child's thunk copies its error to ceesafe_async_set_error(scope) if
 * ceesafe_async_claim_failure says it is the first. ceesafe_async_set_done is ceesafe_async_scope_done for the set,
 * and ceesafe_async_set_cancel cancels its children (the body's error, §3.3). */
typedef struct ceesafe_task_set {
    ceesafe_scope scope;
    uint32_t limit;
    bool oom;
    void *error;
    void *room_waiter;
    void *(*allocate)(size_t size, size_t align);
    void (*release)(void *p, size_t size, size_t align);
} ceesafe_task_set;
void ceesafe_async_set_init(void *set, uint32_t limit, void *error);
uint32_t ceesafe_async_set_admit(void *set, bool wait);
void *ceesafe_async_set_spawn(void *set, size_t frame_size, bool (*resume)(void *frame, ceesafe_scope *scope),
                              const ceesafe_frame_info *info);
/* `set.spawnSend(child)` (ADR 0053 §3.2, ADR 0038): as ceesafe_async_spawn_send, the child published once its frame is
 * initialised; with a user allocator (tasksIn) it runs on this worker, since its frame is freed where it returns. */
void *ceesafe_async_set_spawn_send(void *set, size_t frame_size, bool (*resume)(void *frame, ceesafe_scope *scope),
                                   const ceesafe_frame_info *info);
void *ceesafe_async_set_error(ceesafe_scope *scope);
bool ceesafe_async_set_done(void *set);
void ceesafe_async_set_cancel(void *set);
bool ceesafe_async_set_oom(void *set);
void ceesafe_async_set_allocator(void *set, void *(*allocate)(size_t size, size_t align),
                                 void (*release)(void *p, size_t size, size_t align));
/* AsyncSignal (ADR 0039), four words of lib/core's struct: raised, the waiting task, its executor, and a raise in
 * progress. ceesafe_async_signal_raise may be called from anywhere, an interrupt handler included: it marks the record
 * raised and wakes the task waiting on it. ceesafe_async_signal_take, in the waiting task, lowers a raised record and
 * is true, or registers the task and is false (it parks). ceesafe_async_signal_forget takes the task out (cancelled);
 * a raise it races with stays raised. */
bool ceesafe_async_signal_take(void *signal);
/* The freestanding executor's environment (ADR 0039; profiles.md §4.2), defined by it as ceesafe_panic is (declared
 * in every profile, so the environment's own C sees the prototypes):
 * - ceesafe_env_now_ms: a monotonic clock in milliseconds;
 * - ceesafe_env_idle: block the calling thread until `deadline_ms` (UINT64_MAX: none) or ceesafe_env_wake(executor);
 * - ceesafe_env_wake: end that idling, from any thread or an interrupt handler; a wake before the idle is not lost;
 * - ceesafe_env_slot: the calling thread's slot for its executor (a pointer it keeps, NULL at first), since the
 *   profile has no thread-local storage. Async.run stores its executor there and restores the old value. */
uint64_t ceesafe_env_now_ms(void);
void ceesafe_env_idle(void *executor, uint64_t deadline_ms);
void ceesafe_env_wake(void *executor);
void **ceesafe_env_slot(void);
void ceesafe_async_signal_forget(void *signal);
void ceesafe_async_signal_raise(void *signal);
/* Async.race and Async.all (type-system.md §40.4): a child whose task and frame are the caller's storage, in its frame,
 * so nothing is allocated; it is started like a spawned child and never freed. ceesafe_async_cancel cancels a scope's
 * children and wakes those waiting. */
typedef struct ceesafe_task_slot {
    _Alignas(16) unsigned char bytes[192];
} ceesafe_task_slot;
void ceesafe_async_start(ceesafe_scope *scope, ceesafe_task_slot *slot, void *frame,
                         bool (*resume)(void *frame, ceesafe_scope *scope), const ceesafe_frame_info *info);
void ceesafe_async_cancel(ceesafe_scope *scope);
/* A task's frame stack (ADR 0056): an await on a cycle of the await graph (recursion through `await`, directly or
 * through `dyn`) takes its callee's frame from the current task's stack, `size` bytes aligned to `align`, and gives it
 * back when the call has returned. Frames are pushed and popped last in, first out, in 16 KiB chunks (or one of the
 * frame's size) kept by the executor for the next tasks, up to 64. If no chunk can be had, the program panics
 * (CEE_PANIC_FRAME_STACK). Hosted only. */
void *ceesafe_async_stack_push(size_t size, size_t align);
void ceesafe_async_stack_pop(void *frame);
/* Async.blocking (type-system.md §40.4): `body(arg)` runs on a worker thread, which calls ceesafe_async_blocking_finish
 * when it is done; ceesafe_async_blocking_ready is true once it is, and otherwise parks the task until the worker wakes
 * it through its executor (any thread may wake a task: a flag and the executor's wake pipe). If no thread can be
 * started, the body runs at once on this one: the result is the same. A blocking body is not cancelled. */
typedef struct ceesafe_blocking {
    void *thread;
    void *task;
    void *executor;
    uint32_t done; /* written with ceesafe_atomic_* */
} ceesafe_blocking;
void ceesafe_async_blocking_start(ceesafe_blocking *b, void *(*body)(void *), void *arg);
bool ceesafe_async_blocking_ready(ceesafe_blocking *b);
void ceesafe_async_blocking_finish(ceesafe_blocking *b);
/* Channels (ADR 0032; lib/std/channel.cee): a reference-counted bounded ring of `capacity` values of `size` bytes, under
 * a spin lock whose critical sections are constant steps. lib/std moves each value in or out between begin and end
 * (begin returns the slot and keeps the lock; status 0 a slot, 1 full or empty, 2 closed), and drops values after end.
 * Every change advances an epoch: a blocked thread waits on a condition variable for the epoch to move (wait has
 * `sleep`); a task registers a wait node in its frame (park; false if it should retry at once) and is woken by the
 * next change, from any thread, then unregisters it (unpark). */
typedef struct ceesafe_chan_node {
    void *words[6];
} ceesafe_chan_node;
void *ceesafe_chan_create(uint64_t capacity, uint64_t size, uint64_t align);
void ceesafe_chan_retain(void *c);
bool ceesafe_chan_release(void *c);
void ceesafe_chan_free(void *c);
void *ceesafe_chan_begin_send(void *c, uint32_t *status, uint64_t *epoch);
void ceesafe_chan_end_send(void *c);
void *ceesafe_chan_begin_recv(void *c, uint32_t *status, uint64_t *epoch);
void ceesafe_chan_end_recv(void *c);
void ceesafe_chan_close(void *c);
/* Whether ceesafe_chan_close has been called (ADR 0060), read under the lock. */
bool ceesafe_chan_closed(void *c);
void ceesafe_chan_wait(void *c, uint64_t epoch);
bool ceesafe_chan_park(void *c, void *node, uint64_t epoch);
void ceesafe_chan_unpark(void *c, void *node);
/* Cancellation (type-system.md §40.7): 0 if the current task runs on, 1 if it is cancelled, 2 if its shield's time is
 * up. Async.shield brackets its body with enter and leave; inside, cancellation is not reported. */
uint32_t ceesafe_async_status(void);
void ceesafe_async_shield_enter(uint64_t ms);
void ceesafe_async_shield_leave(void);
uint64_t ceesafe_async_now(void);
void ceesafe_async_wait_until(uint64_t deadline);
void ceesafe_async_wait_writable(int32_t fd);
void ceesafe_async_wait_readable(int32_t fd);
/* Async sockets (runtime/hosted/ceesafe_rt_io.c): non-blocking descriptors; a value >= 0, CEE_ASYNC_WOULD_BLOCK, or a
 * negated errno. Windows: L1 A4. */
#define CEE_ASYNC_WOULD_BLOCK INT64_MIN
/* Operations (ADR 0035): a socket operation is steps over a record in the awaiting task's frame, lib/std's AsyncOp.
 * Each step returns the result (a value >= 0 or a negated errno) once the operation is complete, or
 * CEE_ASYNC_PENDING when the task must park and step again. On a readiness backend a step is an attempt that
 * registers the wait if it would block; on a completion backend (IOCP, io_uring) the first step submits the
 * operation, which the kernel completes into the caller's buffer. ceesafe_async_op_cancel abandons the operation:
 * it cancels one in flight in the kernel and returns true while it still is (the task then waits, uncancellable,
 * stepping until the completion), else releases what the record holds (a connecting socket) and returns false. The
 * record is lib/std's AsyncOp's first field, passed as `void *` (as the channels' wait nodes are). */
#define CEE_ASYNC_PENDING (INT64_MIN + 1)
typedef struct ceesafe_async_op {
    uint64_t words[24];
} ceesafe_async_op;
int64_t ceesafe_async_op_read(void *op, int32_t fd, uint8_t *buffer, uint64_t length);
int64_t ceesafe_async_op_send(void *op, int32_t fd, const uint8_t *bytes, uint64_t length);
int64_t ceesafe_async_op_accept(void *op, int32_t fd);
int64_t ceesafe_async_op_connect(void *op, const uint8_t *address, uint64_t length, uint16_t port);
bool ceesafe_async_op_cancel(void *op);
/* DNS (ADR 0048; lib/std's AsyncDns): a lookup runs getaddrinfo (GetAddrInfoW on Windows) on one of at most
 * CEE_DNS_THREADS resolver threads, the rest queued first come first served. Its record and the threads are the
 * runtime's own (platform memory, detached threads), not the program's. ceesafe_dns_start copies the name and queues
 * the lookup (NULL: out of memory). ceesafe_dns_poll is 1 once it is done, a negated errno if it failed, else 0 after
 * registering the current task to be woken (cancellably, as a channel's wait). ceesafe_dns_count and
 * ceesafe_dns_address read a done lookup's addresses (family 4 or 6, the address bytes, an IPv6 scope).
 * ceesafe_dns_end ends it: a done or queued one is freed; a running one is abandoned, and its thread frees it when the
 * resolver returns, without waking anyone. CosmoOS userland has no resolver: every lookup is EOPNOTSUPP. */
#define CEE_DNS_THREADS 4u
/* HashMap's per-process key (ADR 0052): 128 bits from the system's random source (getrandom, arc4random_buf, rand_s),
 * read once and the same for every map; a fixed key where there is none. */
void ceesafe_random_key(uint64_t *key);
/* Random.fill (ADR 0060): `length` bytes from the system's cryptographic source (getrandom or /dev/urandom,
 * arc4random_buf, BCryptGenRandom); 0, or a negated errno (EOPNOTSUPP where there is no source: CosmoOS userland). */
int64_t ceesafe_random_fill(uint8_t *out, uint64_t length);
void *ceesafe_dns_start(const uint8_t *host, uint64_t length, uint16_t port);
int32_t ceesafe_dns_poll(void *lookup);
uint32_t ceesafe_dns_count(void *lookup);
void ceesafe_dns_address(void *lookup, uint32_t index, uint8_t *bytes, uint32_t *family, uint32_t *scope);
void ceesafe_dns_end(void *lookup);
#if defined(_WIN32)
bool ceesafe_winsock_ready(void); /* Winsock started for the process (ceesafe_rt_io.c), for the resolver */
#endif
/* Sending a file (ADR 0051): a step of ceesafe_async_op_sendfile sends from `file` (a File: its int32_t descriptor), at
 * `offset`, at most `length` bytes, to `socket`: the count sent (0 at the file's end), a negated errno,
 * CEE_ASYNC_PENDING, or CEE_ASYNC_NO_SENDFILE where the system cannot (lib/std then copies through a buffer). The file's
 * own position is not used. ceesafe_file_read_at reads at most `length` bytes at `offset` (pread): the count, 0 at the
 * end, or a negated errno. */
#define CEE_ASYNC_NO_SENDFILE (INT64_MIN + 2)
int64_t ceesafe_async_op_sendfile(void *op, int32_t socket, const void *file, uint64_t offset, uint64_t length);
int64_t ceesafe_file_read_at(const void *file, uint8_t *buffer, uint64_t length, uint64_t offset);
/* Wall-clock time (ADR 0055): the system's real time, seconds since 1970-01-01T00:00:00Z (POSIX time) and the
 * nanoseconds (0 to 999999999) after them; 1970 itself if the system has no real-time clock. */
void ceesafe_clock_realtime(int64_t *seconds, uint32_t *nanoseconds);
/* Message operations (ADR 0045): vectored sends now, datagram messages later (U6). Their record is larger than an
 * operation's and starts with the same header, so ceesafe_async_op_cancel takes either; it is lib/std's AsyncMsgOp's
 * first field. Before the first step, ceesafe_async_msg_part puts part `index` (< CEE_MSG_PARTS) in the record; the
 * runtime reads the pointer and length only during that call, and the kernel reads the bytes until the operation
 * completes. A step of ceesafe_async_op_sendv sends from the first `count` parts: a count >= 0 (it may stop inside any
 * part), a negated errno, or CEE_ASYNC_PENDING. */
#define CEE_MSG_PARTS 8u
typedef struct ceesafe_async_msg_op {
    uint64_t words[64];
} ceesafe_async_msg_op;
void ceesafe_async_msg_part(void *op, uint32_t index, const uint8_t *bytes, uint64_t length);
int64_t ceesafe_async_op_sendv(void *op, int32_t fd, uint32_t count);
/* UDP (ADR 0050). ceesafe_async_udp_bind makes a nonblocking datagram socket bound to the address, with the CEE_UDP_*
 * flags and the buffer sizes (0: the system's): its descriptor, or a negated errno (EOPNOTSUPP for an option the
 * platform lacks; CosmoOS userland has no UDP). On Windows the port-unreachable reports on receive are off
 * (SIO_UDP_CONNRESET).
 * A datagram send uses a message record: its parts (ceesafe_async_msg_part), then ceesafe_async_msg_send_to with the
 * destination, the socket's family (an IPv4 destination of an IPv6 socket is sent IPv4-mapped), an optional source
 * address (`from_family` 0: the system's choice), the ECN bits (0: not set) and a segment size (0: one datagram), then
 * steps of ceesafe_async_op_sendmsg: the bytes sent (the whole datagram, or every segment), a negated errno (EMSGSIZE as
 * EINVAL), CEE_ASYNC_PENDING, or CEE_ASYNC_NO_SEGMENTS where the system cannot send segments in one call (lib/std then
 * sends them one by one).
 * A receive: steps of ceesafe_async_op_recvmsg into `buffer`, the datagram's length in it; then
 * ceesafe_async_msg_address gives the peer (`which` 0) or the local address it arrived at (1, with
 * CEE_UDP_RECEIVE_LOCAL; its port 0): (family << 16) | port with the 16 bytes and the scope written, or -1 if there is
 * none; and ceesafe_async_msg_flags gives bit 0 for a truncated datagram and the ECN bits in bits 8-9.
 * ceesafe_async_udp_send and ceesafe_async_udp_recv are the same without a record or a wait, one call each: a count,
 * CEE_ASYNC_WOULD_BLOCK or a negated errno; the receive writes the peer's bytes and scope and, in `info`, the family,
 * the port and the truncation bit ((family << 24) | (truncated << 16) | port). For code that runs its own wait loop,
 * as the interpreter does. */
#define CEE_UDP_REUSE_ADDRESS 1u
#define CEE_UDP_REUSE_PORT 2u
#define CEE_UDP_V6_ONLY 4u
#define CEE_UDP_RECEIVE_ECN 8u
#define CEE_UDP_RECEIVE_LOCAL 16u
#define CEE_ASYNC_NO_SEGMENTS (INT64_MIN + 3)
int64_t ceesafe_async_udp_bind(uint32_t family, const uint8_t *bytes, uint16_t port, uint32_t scope, uint32_t flags,
                               uint32_t send_buffer, uint32_t receive_buffer);
void ceesafe_async_msg_send_to(void *op, uint32_t socket_family, uint32_t family, const uint8_t *bytes, uint16_t port,
                               uint32_t scope, uint32_t from_family, const uint8_t *from_bytes, uint32_t ecn,
                               uint32_t segment);
int64_t ceesafe_async_op_sendmsg(void *op, int32_t fd, uint32_t count);
int64_t ceesafe_async_op_recvmsg(void *op, int32_t fd, uint8_t *buffer, uint64_t length);
int64_t ceesafe_async_msg_address(void *op, uint32_t which, uint8_t *bytes, uint32_t *scope);
uint32_t ceesafe_async_msg_flags(void *op);
int64_t ceesafe_async_udp_send(int32_t fd, const uint8_t *data, uint64_t length, uint32_t socket_family, uint32_t family,
                               const uint8_t *bytes, uint16_t port, uint32_t scope);
int64_t ceesafe_async_udp_recv(int32_t fd, uint8_t *buffer, uint64_t length, uint8_t *bytes, uint32_t *scope,
                               uint32_t *info);
#if defined(_WIN32)
/* Windows sockets (L1 A4a): a descriptor from CEE_SOCKET_BASE up names a SOCKET in ceesafe_rt_io.c's table, so
 * lib/std's i32 descriptors never hold a truncated handle; below it, a C runtime file descriptor.
 * ceesafe_socket_handle gives the SOCKET, or INVALID_SOCKET (~0) for an id that names none. */
#define CEE_SOCKET_BASE 0x40000000
uintptr_t ceesafe_socket_handle(int32_t id);
/* The executor's readiness (L1 A4b): ceesafe_socket_select has the socket signal `event` for every network event
 * (WSAEventSelect; a call only when that changes), and ceesafe_socket_unselect detaches every socket that signals
 * `event`, before the executor closes it. */
void ceesafe_socket_select(int32_t id, void *event);
void ceesafe_socket_unselect(void *event);
/* IOCP (ADR 0035): ceesafe_socket_associate ties the socket to `port` once, for life: 1 if it is (now or before),
 * 0 if it is another port's (it is then used by readiness), -1 on failure. ceesafe_socket_adopt gives a new SOCKET
 * an id (closing it if the table cannot grow: -1), recording `port` if it is already tied to one (else NULL);
 * ceesafe_socket_errno is a Winsock error as an errno. */
int32_t ceesafe_socket_associate(int32_t id, void *port);
int32_t ceesafe_socket_adopt(uintptr_t s, void *port);
int32_t ceesafe_socket_errno(int error);
#endif
/* Dotted IPv4 text to its four bytes; false if malformed (the TCP calls' own check, for completion-based connects). */
bool ceesafe_ipv4_address(cee_str text, uint8_t out[4]);
/* ADR 0044: a socket address as the runtime passes it between its files: family 4 or 6, the port, an IPv6 scope id,
 * and the address bytes in network order (4 or 16 of them). lib/std's SocketAddress crosses as these fields. */
typedef struct ceesafe_endpoint {
    uint32_t family;
    uint16_t port;
    uint32_t scope;
    uint8_t bytes[16];
} ceesafe_endpoint;
/* An IPv4 or IPv6 address's text (no port, no zone) and a port as an endpoint; false if malformed. */
bool ceesafe_endpoint_parse(cee_str text, uint16_t port, ceesafe_endpoint *out);
/* The fields as an endpoint; false unless the family is 4 or 6. */
bool ceesafe_endpoint_make(uint32_t family, const uint8_t *bytes, uint16_t port, uint32_t scope, ceesafe_endpoint *out);
/* The endpoint as the platform's sockaddr in `out` (`room` bytes: a sockaddr_in6 fits): its length, 0 if it does not
 * fit or the platform has no such family. */
uint32_t ceesafe_endpoint_sockaddr(const ceesafe_endpoint *e, void *out, uint32_t room);
/* A sockaddr (IPv4 or IPv6) as an endpoint, an IPv4-mapped address as IPv4; false for another family. And an endpoint
 * as the sockaddr of a socket of `socket_family`: an IPv4 endpoint IPv4-mapped for an IPv6 socket. */
bool ceesafe_endpoint_from_sockaddr(const void *address, ceesafe_endpoint *out);
uint32_t ceesafe_endpoint_sockaddr_for(const ceesafe_endpoint *e, uint32_t socket_family, void *out, uint32_t room);
/* A new socket of the endpoint's family for TCP (macOS: SO_NOSIGPIPE), or a negated errno. POSIX only; Windows makes
 * its overlapped sockets itself. */
int64_t ceesafe_tcp_socket_for(const ceesafe_endpoint *e);
/* Listen options (ListenOptions): the flags' bits; a backlog of 0 is SOMAXCONN. */
#define CEE_LISTEN_REUSE_ADDRESS 1u
#define CEE_LISTEN_REUSE_PORT 2u
#define CEE_LISTEN_V6_ONLY 4u
/* Stream options (AsyncTcpStream's setters): the value is 0/1 for NO_DELAY, the idle seconds or -1 (off) for
 * KEEP_ALIVE (0: on, the system's idle time), the size for the buffers, and the seconds or -1 (the default) for LINGER. */
#define CEE_SOCKET_OPTION_NO_DELAY 1u
#define CEE_SOCKET_OPTION_KEEP_ALIVE 2u
#define CEE_SOCKET_OPTION_SEND_BUFFER 3u
#define CEE_SOCKET_OPTION_RECEIVE_BUFFER 4u
#define CEE_SOCKET_OPTION_LINGER 5u
int64_t ceesafe_async_tcp_listen_at(uint32_t family, const uint8_t *bytes, uint16_t port, uint32_t scope,
                                    uint32_t backlog, uint32_t flags);
int64_t ceesafe_async_tcp_connect_at(uint32_t family, const uint8_t *bytes, uint16_t port, uint32_t scope);
int64_t ceesafe_async_tcp_connect_endpoint(const ceesafe_endpoint *e);
int64_t ceesafe_async_op_connect_at(void *op, uint32_t family, const uint8_t *bytes, uint16_t port, uint32_t scope);
/* A socket's own (peer 0) or its peer's (peer 1) address: its 16 bytes and scope written, the result
 * (family << 16) | port, or a negated errno. IPv4-mapped IPv6 addresses are reported as IPv4. */
int64_t ceesafe_async_socket_name(int32_t fd, uint32_t peer, uint8_t *bytes, uint32_t *scope);
int64_t ceesafe_async_socket_option(int32_t fd, uint32_t option, int64_t value);
int64_t ceesafe_async_shutdown_read(int32_t fd);
/* An interface's index by name (an IPv6 zone), 0 if there is none or the platform cannot say. */
uint32_t ceesafe_interface_index(const uint8_t *name, uint64_t length);
int64_t ceesafe_async_tcp_listen(const uint8_t *address, uint64_t length, uint16_t port);
int64_t ceesafe_async_tcp_port(int32_t fd);
int64_t ceesafe_async_tcp_accept(int32_t fd);
int64_t ceesafe_async_tcp_connect(const uint8_t *address, uint64_t length, uint16_t port);
int64_t ceesafe_async_tcp_connected(int32_t fd);
int64_t ceesafe_async_read(int32_t fd, uint8_t *buffer, uint64_t length);
int64_t ceesafe_async_send(int32_t fd, const uint8_t *bytes, uint64_t length);
int64_t ceesafe_async_shutdown_write(int32_t fd);
void ceesafe_async_close(int32_t fd);
/* Waits up to `timeout` ms (-1: no limit) for any of `count` descriptors: `events` 1 readable, 2 writable; `ready`
 * receives each one's readiness the same way. The count ready, 0 on timeout, or a negated errno. For the interpreter,
 * which has no executor of its own descriptors (src/host). */
int64_t ceesafe_async_poll(const int32_t *fds, const uint32_t *events, uint32_t *ready, uint64_t count, int64_t timeout);
int64_t ceesafe_async_write(int32_t fd, const uint8_t *bytes, uint64_t length);

#endif /* CEESAFE_RT_H */
