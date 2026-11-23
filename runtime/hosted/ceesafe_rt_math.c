/* SPDX-License-Identifier: GPL-2.0-only WITH Classpath-exception-2.0 */
/*
 * ceesafe_rt_math.c - the float methods that are the C library's math functions (type-system.md §35.8; ADR 0218).
 *
 * `x.exp()`, `x.sin()`, `x.pow(y)` … are calls of these functions, each the C function of the same meaning and
 * nothing more, so a compiled program's results are its platform libm's. The interpreter calls the same libm
 * functions (src/interpreter/math.cee), so a program prints the same in both.
 *
 * They are a translation unit of their own on purpose: the generated code cannot be inlined into them, so the C
 * compiler never sees a constant argument and never folds a call at compile time (gcc evaluates `exp(2.0)` with MPFR,
 * which may differ from the libm in the last bit) or rewrites one (clang turns `pow(2.0, y)` into `exp2(y)`).
 *
 * Hosted only: the checker rejects these methods with --profile freestanding (N0293), since CosmoOS has no libm.
 * The program links this file only if it calls one; on Linux and FreeBSD the link then adds `-lm`.
 */
#define CEE_FLOATS 1
#include "ceesafe/rt.h"
#include <math.h>

#define CEE_MATH_UNARY(NAME, F64, F32)                                                                             \
    double ceesafe_math_##NAME##_f64(double x) { return F64(x); }                                                  \
    float ceesafe_math_##NAME##_f32(float x) { return F32(x); }
#define CEE_MATH_BINARY(NAME, F64, F32)                                                                            \
    double ceesafe_math_##NAME##_f64(double x, double y) { return F64(x, y); }                                     \
    float ceesafe_math_##NAME##_f32(float x, float y) { return F32(x, y); }

CEE_MATH_UNARY(exp, exp, expf)
CEE_MATH_UNARY(exp2, exp2, exp2f)
CEE_MATH_UNARY(expm1, expm1, expm1f)
CEE_MATH_UNARY(log, log, logf)
CEE_MATH_UNARY(log2, log2, log2f)
CEE_MATH_UNARY(log10, log10, log10f)
CEE_MATH_UNARY(log1p, log1p, log1pf)
CEE_MATH_UNARY(sin, sin, sinf)
CEE_MATH_UNARY(cos, cos, cosf)
CEE_MATH_UNARY(tan, tan, tanf)
CEE_MATH_UNARY(asin, asin, asinf)
CEE_MATH_UNARY(acos, acos, acosf)
CEE_MATH_UNARY(atan, atan, atanf)
CEE_MATH_UNARY(sinh, sinh, sinhf)
CEE_MATH_UNARY(cosh, cosh, coshf)
CEE_MATH_UNARY(tanh, tanh, tanhf)
CEE_MATH_UNARY(asinh, asinh, asinhf)
CEE_MATH_UNARY(acosh, acosh, acoshf)
CEE_MATH_UNARY(atanh, atanh, atanhf)
CEE_MATH_UNARY(cbrt, cbrt, cbrtf)
CEE_MATH_UNARY(logb, logb, logbf)
CEE_MATH_BINARY(pow, pow, powf)
CEE_MATH_BINARY(atan2, atan2, atan2f)
CEE_MATH_BINARY(hypot, hypot, hypotf)
CEE_MATH_BINARY(remainder, remainder, remainderf)
CEE_MATH_BINARY(fmod, fmod, fmodf)

double ceesafe_math_fma_f64(double x, double y, double z) { return fma(x, y, z); }
float ceesafe_math_fma_f32(float x, float y, float z) { return fmaf(x, y, z); }
double ceesafe_math_scalbn_f64(double x, int32_t n) { return scalbn(x, (int)n); }
float ceesafe_math_scalbn_f32(float x, int32_t n) { return scalbnf(x, (int)n); }
