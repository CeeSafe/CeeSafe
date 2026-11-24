# ADR 0218 — Float math: the C library's functions as methods, and the exact roundings

- Status: **Proposed** 2026-11-24 and implemented on branch `fix/findings-math` at the owner's instruction (the
  SQLite port's `CEESAFE_FINDINGS.md`, findings 1 and 20); not released. §6's questions are the owner's. ADR 0248
  amends §2.2: on Windows the functions are ucrtbase.dll's whatever the C compiler, and the interpreter calls the
  runtime's functions, not libm by name.
- Scope: type-system.md §35.6 and §35.8; c-backend.md §1; tooling.md §8.3; diagnostics.md N0293;
  `src/check/io.cee` (the method table), `src/hir/builtins.cee`, `src/backend/c_body.cee`, `src/backend/c_backend.cee`,
  `src/host/toolchain.cee` (`-lm`), `src/interpreter/math.cee` and `runtime.cee`; `runtime/include/ceesafe/rt.h`,
  `runtime/hosted/ceesafe_rt_math.c`; `ceesafe.toml`; the guide's Floats section.

## 1. Context

`f64` and `f32` have `sqrt`, `abs`, `floor`, `ceil`, `min` and `max` (§35.8, ADR 0011 §5 Q5) and nothing else. ADR
0011 §3.8 left "a math library" out of G7 and §5 Q5 explained why: CosmoOS has no libm, so anything beyond what one
instruction does "would be a reference implementation with its own accuracy tests".

A SQLite port in CeeSafe met the gap twice:

- **Finding 1.** SQLite's SQL functions `exp()`, `ln()`, `log10()`, `log2()`, `pow()`, `sin()` … through `atanh()`
  need the C library's functions, and their results show 17 significant digits, so they must be the platform libm's
  in the last bit to match the `sqlite3` shell. The port carries `src/libm.cee`, 1,344 lines of FreeBSD's msun with
  double-double versions of the worst functions, and still differs from macOS's libm in the last bit for some
  arguments.
- **Finding 20.** Porting msun needed C's float helpers too: no `fma` (an exact product took Dekker's split, about
  ten operations), no `scalbn`/`ldexp`/`frexp`, no `copysign`, no `rint` (`x + 0x1.8p52 - 0x1.8p52` did it), and
  no hexadecimal float literals.

Every program that does geometry, statistics, audio or finance meets the same gap. The question is where the
functions come from.

## 2. Decision

### 2.1 Methods, in the float's own type

The functions are built-in methods of `f32` and `f64`, as `sqrt` is: `x.exp()`, `y.atan2(x)`, `x.fma(y, z)`. An
`f32` receiver gives an `f32` result, computed by the `f32` function (`expf`), not by `exp` and a rounding.

| Group | Methods | C |
|---|---|---|
| exponentials and logarithms | `exp`, `exp2`, `expM1`, `ln`, `log2`, `log10`, `ln1p`, `pow(y)` | `exp`, `exp2`, `expm1`, `log`, `log2`, `log10`, `log1p`, `pow` |
| trigonometry | `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2(x)` | the same names |
| hyperbolic | `sinh`, `cosh`, `tanh`, `asinh`, `acosh`, `atanh` | the same names |
| others | `cbrt`, `hypot(y)`, `fma(y, z)`, `remainder(y)`, `truncatingRemainder(y)`, `scaleB(n)`, `logB()` | `cbrt`, `hypot`, `fma`, `remainder`, `fmod`, `scalbn`, `logb` |
| exact roundings | `trunc`, `round`, `roundEven`, `copySign(y)` | `trunc`, `round`, `rint`, `copysign` (§2.3: not called) |

The names:

- **`ln`, not `log`.** `log` is the natural logarithm in C and base 10 on calculators and in SQL's `log(x)`; `ln`
  is unambiguous next to `log2` and `log10`. `ln1p` and `expM1` follow (`log1p`, `expm1` in C).
- **`truncatingRemainder`** for C's `fmod`, beside IEEE's `remainder`: the two differ in the quotient (toward zero,
  or to nearest), and the name says which. `%` stays undefined on floats (§35.6): a symbol would hide the choice.
- **`roundEven`** for `rint`: what it does, not C's name for it. It ignores the C rounding mode, which CeeSafe never
  changes.
- **`scaleB` and `logB`** are IEEE 754's names (§5.3.3). `scaleB`'s `n` is an `i32` (C's `int`).
- **No `frexp`.** It returns two values; CeeSafe has no tuples (ADR 0216 proposes none) and a struct for it would be
  a new built-in type for one function. `logB` and `scaleB` compose to it: for a finite non-zero `x`,
  `x.scaleB(-truncate<i32>(x.logB()))` is the significand in [1, 2), which is how msun's code uses `frexp` anyway.

Every name is reserved on float types (N0228), as the first ones are. Operands of two or three are checked against
`x`'s type, so a flexible literal takes it; `scaleB`'s against `i32`. Wrong counts are N0209, other types N0205.

### 2.2 The platform's libm, in C and in the interpreter

The methods of the first four groups call the platform C library's function and give its result, whatever it is:
NaN, infinities, signed zeros and domain and pole errors as C11 Annex F says (`(-1.0).ln()` is NaN, `0.0.ln()` is
`-Infinity`, `1000.0.exp()` is `Infinity`), and inexact results to the libm's accuracy. None panics; `errno` is not
part of the result.

- **In C**, the call is of `ceesafe_math_exp_f64` (`_f32`) in `runtime/hosted/ceesafe_rt_math.c`, a runtime file
  linked only when a program calls one (as `ceesafe_rt_float.c` is), whose functions call libm and do nothing else.
  It is a translation unit of its own so that no constant reaches a libm call: gcc evaluates `exp(2.0)` at compile
  time with MPFR, correctly rounded, which can differ from glibc's last bit, and clang rewrites `pow(2.0, y)` as
  `exp2(y)`. A program would otherwise print one thing for `2.0.exp()` and another for `x.exp()` with `x` 2.0.
- **In the interpreter**, the same libm functions are called from ceesafec (`src/interpreter/math.cee`, `extern "C"`
  with `@cname`). The interpreter is ceesafec, so on one machine it calls the libm the program's build links, and
  the two print the same: the "Interpreter agreement" contract (testing.md) holds bit for bit. They can differ
  only between machines, as two builds can.
- **Linking.** glibc's and FreeBSD's libm are libraries of their own, so a link with `ceesafe_rt_math.c` adds `-lm`,
  except on Windows, whose C runtime holds them; macOS's `-lm` names libSystem. ceesafec itself calls libm for the
  interpreter, so its manifest has `[target.linux]` and `[target.freebsd]` `links = ["m"]`, which stage 0 (0.94.5)
  already honours; the seed's commands already pass `-lm` (bootstrap/seed/README.md).

### 2.3 The exact roundings, everywhere

`trunc`, `round`, `roundEven` and `copySign` are exact and short, so they are portable C in `rt.h` beside `floor`
and `ceil`, with no libm, and are in every profile. For `|x| < 2^52` (`2^23` for `f32`), `t`, `x` converted toward
zero, is exact, and so is `d = x - t` (`|d| < 1`; Sterbenz): `round` steps away from zero when `|d| >= 0.5`,
`roundEven` when `|d| > 0.5` or `|d| = 0.5` and `t` is odd. Larger values, infinities and NaN are already integral. A
zero result takes `x`'s sign. `round` is therefore right where `floor(x + 0.5)` is not: `0.49999999999999994.round()`
is `0.0`. The interpreter computes them in CeeSafe the same way, from `floor` and `ceil`.

### 2.4 Hosted only

CosmoOS has no libm, so with `--profile freestanding` a call of a libm method is N0293, as `Console` is: "'exp', a C
library math function, needs the hosted runtime". The exact roundings and the methods of ADR 0011 remain.

### 2.5 Hexadecimal float literals: not here

Finding 20 asks for `0x1.8p52`. It is a lexer, parser, formatter and highlighter change of its own, with a decision
to make (must the literal be exact, or may it round?), and nothing in this ADR needs it: with `fma`, `scaleB` and
the libm, the constants that msun spells in hexadecimal are no longer needed in a program. `f64.fromBits(0x…)`
remains the exact spelling. This ADR's companion number, 0219, is left unused.

## 3. Options not taken

- **A correctly rounded library in lib/core** (CORE-MATH's functions, as finding 1 suggests). The results would be
  the same on every platform and in freestanding programs. But SQLite, and any program compared with C, Python or
  .NET on the same machine, wants the platform's results, which are not always correctly rounded (macOS's `atan2f(0,
  -0)` is `3.1415925`, below π), and porting CORE-MATH's functions to CeeSafe would be a project of its own, with its
  own accuracy tests. It can come later under other names, or as the freestanding
  implementation (§6, question 2).
- **Calling libm directly from the generated code** (`exp(x)` in `main.c`, with `<math.h>`). Simpler, but the C
  compiler would fold constant calls (§2.2), and `<math.h>` in every program's translation unit adds names that a
  program's `extern "C"` declarations may clash with.
- **Free functions in a `Math` struct** (`Math.exp(x)`). Methods are how `sqrt` and the integer methods are spelled,
  and a method takes its type from the receiver: `f32` gets `expf` with no generic machinery.
- **Methods in lib/std** over `extern "C"` calls, with the interpreter emulating the externs by name, as it does
  `ceesafe_clock_monotonic`. This needs no checker change, but lib/std cannot add methods to a built-in type, and
  `x.exp()` should not need a library import.

## 4. Consequences

- Thirty-two built-ins, `bfFloatExp` (145) to `bfFloatCopySign` (176), in the method table, the C backend and the
  interpreter. The names are reserved on float types: a program whose interface method of one of those names is
  implemented for `f64` no longer compiles (N0228), as with `sqrt` before.
- A hosted program on Linux that calls a libm method links libm; others do not.
- `--emit lib`'s archive of such a program holds `ceesafe_rt_math.o`, and the host links `-lm` (c-backend.md §1).
- Results may differ between platforms in the last bit, as C's do. The corpus therefore prints in full only the
  values every IEEE libm gives (Annex F's special values and exact results) and checks the others within 4 ulps of
  Python's math module (§5).
- The SQLite port can drop `src/libm.cee` and get the platform's results, which is what the `sqlite3` shell
  computes.

## 5. Tests and evidence (2026-11-24, macOS arm64)

- **The corpus**, generated by `tools/oracles/gen_float_math_cases.py` from Python's math module (with Annex F's
  values where Python raises, and `math.ldexp`, `math.frexp`, `fractions` and `decimal` where Python 3.9 has no such
  function; mpmath for `cbrt`):
  - `run-pass/float-libm.cee`: 195 exact `f64` results printed in full (every method, signed zeros, infinities, NaN,
    domain and pole errors, overflow, subnormal `scaleB`, `fma`'s single rounding, the rounding halves) and 201
    inexact ones within 4 ulps;
  - `run-pass/float-libm-f32.cee`: the same for `f32`, 58 exact and 134 within 4 ulps;
  - `check-fail/float-libm-methods.cee` (N0228, N0209, N0205, N0234) and `check-fail/float-libm-freestanding.cee`
    (N0293 for the libm methods, none for the exact ones).
- **Agreement.** `tools/oracles/float_math_sweep.py` builds and interprets one program that prints the bits of
  every method on seeded random operands (any bit pattern, and small exponents), `f64` and `f32`; its result is in
  ROADMAP §10.
- **Found on the way.** macOS's `atan2f(±0, -0)` is `±3.1415925`, not the `f32` nearest π (`3.1415927`, glibc's).
  Both meet Annex F, which says only "±π". The `f32` test checks those two within 4 ulps.

## 6. Questions for the owner

1. **Names.** `ln`/`ln1p`/`expM1`, `truncatingRemainder` for `fmod`, `roundEven` for `rint`, `logB`/`scaleB` instead
   of `frexp`/`ldexp`: as proposed, or C's names?
2. **Freestanding.** Leave the libm methods hosted only (proposed), or give freestanding programs a portable
   implementation later (msun's or CORE-MATH's, in lib/core), whose results would then differ from a hosted
   build's?
3. **Hexadecimal float literals** (§2.5): an ADR of their own, or not at all?
