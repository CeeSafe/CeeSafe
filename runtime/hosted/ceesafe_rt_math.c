/* SPDX-License-Identifier: GPL-2.0-only WITH Classpath-exception-2.0 */
/*
 * ceesafe_rt_math.c - the float methods that are the C library's math functions (type-system.md §35.8; ADR 0218,
 * ADR 0248).
 *
 * `x.exp()`, `x.sin()`, `x.pow(y)` … are calls of these functions, each the C function of the same meaning and
 * nothing more, so a compiled program's results are its system's libm's. The interpreter calls these same functions
 * (src/interpreter/math.cee calls the methods, so ceesafec links this file), so a program prints the same in both.
 *
 * They are a translation unit of their own on purpose: the generated code cannot be inlined into them, so the C
 * compiler never sees a constant argument and never folds a call at compile time (gcc evaluates `exp(2.0)` with MPFR,
 * which may differ from the libm in the last bit) or rewrites one (clang turns `pow(2.0, y)` into `exp2(y)`).
 *
 * Windows (ADR 0248): the functions are the system's C runtime's, ucrtbase.dll's, whatever compiles the program.
 * `cl` copies its SDK's static UCRT into a program by default, an msvcrt MinGW links msvcrt.dll's and libmingwex's
 * functions, and only a UCRT MinGW (or `cl /MD`) calls ucrtbase.dll; their last bits differ. So the first call of any
 * of them loads ucrtbase.dll from System32 and takes each function's address, once (InitOnceExecuteOnce); a function
 * it cannot find stays the one the program was linked with. The functions are pure and use nothing of the C runtime
 * but `errno`, which CeeSafe never reads, so calling the system's from a program linked with another is safe.
 * Elsewhere each is a direct call of the system libm.
 *
 * Hosted only: the checker rejects these methods with --profile freestanding (N0293), since CosmoOS has no libm.
 * The program links this file only if it calls one; on Linux and FreeBSD the link then adds `-lm`.
 */
#define CEE_FLOATS 1
#include "ceesafe/rt.h"
#include <math.h>

/* Every function but fma and scalbn: X1(name) takes one operand, X2(name) two. */
#define CEE_MATH_FUNCTIONS(X1, X2)                                                                                 \
    X1(exp) X1(exp2) X1(expm1) X1(log) X1(log2) X1(log10) X1(log1p) X1(sin) X1(cos) X1(tan) X1(asin) X1(acos)      \
    X1(atan) X1(sinh) X1(cosh) X1(tanh) X1(asinh) X1(acosh) X1(atanh) X1(cbrt) X1(logb)                            \
    X2(pow) X2(atan2) X2(hypot) X2(remainder) X2(fmod)

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <string.h>
#include <windows.h>

/* The functions called: each starts as the linked one and becomes ucrtbase.dll's if it has the name. */
#define CEE_MATH_POINTER1(NAME)                                                                                    \
    static double(__cdecl *cee_math_##NAME)(double) = NAME;                                                        \
    static float(__cdecl *cee_math_##NAME##f)(float) = NAME##f;
#define CEE_MATH_POINTER2(NAME)                                                                                    \
    static double(__cdecl *cee_math_##NAME)(double, double) = NAME;                                                \
    static float(__cdecl *cee_math_##NAME##f)(float, float) = NAME##f;
CEE_MATH_FUNCTIONS(CEE_MATH_POINTER1, CEE_MATH_POINTER2)
static double(__cdecl *cee_math_fma)(double, double, double) = fma;
static float(__cdecl *cee_math_fmaf)(float, float, float) = fmaf;
static double(__cdecl *cee_math_scalbn)(double, int) = scalbn;
static float(__cdecl *cee_math_scalbnf)(float, int) = scalbnf;

static INIT_ONCE cee_math_once = INIT_ONCE_STATIC_INIT;

/* `*pointer` (a function pointer of the function's own type) becomes ucrtbase.dll's `name`, if it has one. */
static void cee_math_take(HMODULE ucrt, const char *name, void *pointer, size_t size) {
    FARPROC found = GetProcAddress(ucrt, name);
    if (found != NULL && size == sizeof found) {
        memcpy(pointer, &found, sizeof found);
    }
}

static BOOL CALLBACK cee_math_resolve(PINIT_ONCE once, PVOID parameter, PVOID *context) {
    (void)once;
    (void)parameter;
    (void)context;
    /* System32 only: no ucrtbase.dll beside the program or in the current directory stands in for the system's. */
    HMODULE ucrt = LoadLibraryExW(L"ucrtbase.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (ucrt == NULL) {
        return TRUE; /* no UCRT (Windows 7 or 8.1 without KB2999226): the linked functions stay */
    }
    /* ucrtbase.dll exports hypotf as _hypotf (Microsoft's <math.h> maps the one to the other); a later one wins. */
    cee_math_take(ucrt, "_hypotf", &cee_math_hypotf, sizeof cee_math_hypotf);
#define CEE_MATH_TAKE(NAME)                                                                                        \
    cee_math_take(ucrt, #NAME, &cee_math_##NAME, sizeof cee_math_##NAME);                                          \
    cee_math_take(ucrt, #NAME "f", &cee_math_##NAME##f, sizeof cee_math_##NAME##f);
    CEE_MATH_FUNCTIONS(CEE_MATH_TAKE, CEE_MATH_TAKE)
    CEE_MATH_TAKE(fma)
    CEE_MATH_TAKE(scalbn)
#undef CEE_MATH_TAKE
    return TRUE; /* never freed: the pointers point into it */
}

/* The pointer to call: resolved first, once; InitOnceExecuteOnce orders the stores above before every call's load. */
#define CEE_MATH_CALL(NAME) ((void)InitOnceExecuteOnce(&cee_math_once, cee_math_resolve, NULL, NULL), cee_math_##NAME)
#else
#define CEE_MATH_CALL(NAME) NAME
#endif

#define CEE_MATH_UNARY(NAME)                                                                                       \
    double ceesafe_math_##NAME##_f64(double x) { return CEE_MATH_CALL(NAME)(x); }                                  \
    float ceesafe_math_##NAME##_f32(float x) { return CEE_MATH_CALL(NAME##f)(x); }
#define CEE_MATH_BINARY(NAME)                                                                                      \
    double ceesafe_math_##NAME##_f64(double x, double y) { return CEE_MATH_CALL(NAME)(x, y); }                     \
    float ceesafe_math_##NAME##_f32(float x, float y) { return CEE_MATH_CALL(NAME##f)(x, y); }
CEE_MATH_FUNCTIONS(CEE_MATH_UNARY, CEE_MATH_BINARY)

double ceesafe_math_fma_f64(double x, double y, double z) { return CEE_MATH_CALL(fma)(x, y, z); }
float ceesafe_math_fma_f32(float x, float y, float z) { return CEE_MATH_CALL(fmaf)(x, y, z); }
double ceesafe_math_scalbn_f64(double x, int32_t n) { return CEE_MATH_CALL(scalbn)(x, (int)n); }
float ceesafe_math_scalbn_f32(float x, int32_t n) { return CEE_MATH_CALL(scalbnf)(x, (int)n); }
