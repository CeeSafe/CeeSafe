# ADR 0011 — M11 G7: floating-point types

> **Inherited.** ADR 0011 was accepted before CeeSafe's fork (Decision 0006), when the language had its
> earlier name. In this record, "the reference" is that language and project and "the reference compiler" its compiler:
> read them as CeeSafe and `ceesafec`. Links to documents CeeSafe did not keep are given as text.

- Status: **Accepted** (project owner, 2026-10-25, taking every recommendation in §5). The wording is the agent's;
  the owner may revise it. Not yet implemented: the decisions are written into `type-system.md` (a new §35),
  `grammar.md`, `c-backend.md` and `profiles.md` before any code (§6).
- Scope: M11 G7 (`milestone-11.md`), closing OD-23 (`open-decisions.md`). It amends `type-system.md` §1 (N0216 for
  floats), §5 (flexible literals) and §7 (conversions).

## 1. Context

Facts this design rests on, from the code and the docs as they are today:

- **What already exists.** The lexer reads float literals with `f32`/`f64` suffixes (`Syntax/Lexer.cs:236-239`,
  `grammar.md` §1: `1.` and `.5` are not floats). The checker rejects them: "floating-point numbers are not supported
  in M1" (N0216, `TypeChecker.cs` `Synth`, `FloatLiteralExpr`), and `f64` as a type is unknown (N0204). `language-spec.md` §2 and Ex2/Ex13 already use
  `f64` (IEEE 754, C `float`/`double`), and `c-backend.md`'s type table maps `f32`/`f64` to `float`/`double`.
- **Profiles.** `profiles.md` §1: floats are *yes* in hosted and CosmoOS user programs, **forbidden** in the
  CosmoOS kernel, and *target-defined* in the generic freestanding profile. CosmoOS builds its kernel and loader with
  `-mgeneral-regs-only` on both arches (CosmoOS `build/arch/x86_64.mk:6,14`, `aarch64.mk:19,28`), and
  `scripts/check-fpregs.sh` rejects objects that touch FP registers. The reference runs that script in `c4.py` and `c5.py`
  (`c4.py:204-206`, `c5.py:104`); `c6.py` and `c7.py` reuse C4's other checks but not this one.
- **No libm, no exact libc formatting.** CosmoOS's libc has no `<math.h>` (`libc/include/`), and its `printf`
  float conversions are "deliberately small … not exactly rounded in the last digit for every input"
  (`libc/src/printf.c:9-14`). Formatting therefore cannot go through libc if output is to be deterministic and equal
  to the interpreter's.
- **No compiler-rt** (`c-backend.md` §1). On the two 64-bit targets the reference supports, float arithmetic and
  `int64 ↔ double` conversions compile to instructions. `fmod`, `sqrt` and the like are libm calls unless they become
  builtins that lower to an instruction.
- **The interpreter is an oracle** (M7c). Every program must print the same output compiled and interpreted, and the
  fuzzer compares them (`tests/Reference.Fuzz`). .NET's `double` and `float` are IEEE binary64/binary32 with
  round-to-nearest-even. The JIT does not fuse `a * b + c` into an FMA unless asked (`Math.FusedMultiplyAdd`)
  (inferred, to verify in G7).
- **C compilers may contract.** clang contracts `a * b + c` into an FMA within an expression by default
  (`-ffp-contract=on`). gcc does in GNU modes (`-ffp-contract=fast`), but not under `-std=c11`. An FMA rounds once
  instead of twice, so the C result could differ from the interpreter's and between compilers (inferred from
  compiler documentation, to verify in G7).
- **Undefined behaviour.** In C, converting a float to an integer type that cannot hold its truncated value is
  undefined (C11 6.3.1.4). Generated C must have none (`c-backend.md`).

## 2. Options

| | A. IEEE floats, deterministic (proposed) | B. Floats as in C | C. No floats; fixed-point library |
|---|---|---|---|
| Semantics | IEEE 754 binary32/64, round-to-nearest-even, no contraction, no traps; same result compiled and interpreted | whatever the C compiler does (contraction, `-ffast-math` allowed) | integers only |
| Float → int | checked: panics on NaN and out of range | C's cast (UB out of range) | — |
| Printing | The reference's own shortest round-trip formatter | libc `printf("%g")` | — |
| Cost | a formatter in the runtime (~1–2k lines with tables, inferred) | small | a library |
| Safety | no UB; deterministic | UB on conversion; results vary by compiler | — |

B breaks two rules (no UB; the interpreter as oracle). C leaves Ex2/Ex13 and every numeric program out. **A is
proposed.**

## 3. Proposed decision

1. **Types.** `f32` and `f64` are IEEE 754 binary32 and binary64, laid out per `TargetInfo` (4/4 and 8/8 on both
   targets). They are Copy, need no drop, hold no loans, and are FFI-safe (C `float`/`double`). The FP environment is
   the default one, never changed by CeeSafe code: round to nearest even, no traps, and no flags read.
2. **Literals.** `1.5`, `1e9`, `2.5e-3`, with an optional `f32`/`f64` suffix. An unsuffixed float literal is
   *flexible* like an integer one (§5): it takes an expected `f32` or `f64`, and defaults to `f64`. The literal's
   value is the decimal rounded to nearest (even) in its type, exactly once: `0.1f32` is the `f32` nearest 0.1, not the
   `f64` nearest 0.1 rounded again. An integer literal is not a float (`let x: f64 = 1;` is N0205; write `1.0`), as
   The reference has no implicit conversions. A literal that rounds to infinity is an error (N0206).
3. **Operators.** `+ - * /` and unary `-` on two operands of the same float type give that type. They **never
   panic**: overflow gives ±infinity, `0.0 / 0.0` gives NaN, as IEEE says. This is the one exception to "arithmetic
   panics on failure" (OD-6), and the spec states it. `%` is not defined on floats in G7 (N0207), since C's `fmod`
   is a libm call. Comparisons `== != < <= > >=` are IEEE's: NaN is unequal to everything, itself included, and
   `-0.0 == 0.0`. Bitwise and shift operators are N0207.
4. **No contraction, no excess precision.** Each operation rounds to its type. The generated C enforces this:
   - `rt.h` checks `FLT_EVAL_METHOD == 0` (`<float.h>` is a freestanding header) and emits
     `#pragma STDC FP_CONTRACT OFF`;
   - The reference compiler's builds pass `-ffp-contract=off`, and `c-backend.md`'s checklist requires it of integrations, since
     gcc ignores the pragma;
   - `-ffast-math` and its parts are unsupported, as `-fwrapv` already is not relied on.

   The interpreter computes each operation in .NET's `float` or `double`. Constant folding in the compiler uses the
   same .NET arithmetic.
5. **Conversions** (`type-system.md` §7).
   - `as` stays lossless only: `f32 as f64`; `i8`, `i16`, `u8` and `u16 as f32`; and `i8`–`i32`, `u8`–`u32 as f64`.
     Every value of the source type is exactly representable in the target type.
   - `round<T>(e)` (new, free-standing like `truncate`): any integer or float to a float `T`, **rounded to nearest
     even**. It is always defined: `round<f64>(x)` for `x: i64`, `round<f32>(d)` for `d: f64` (overflow gives
     infinity).
   - `truncate<T>(e)` extended: a float to an integer `T`, rounding **toward zero**. It **panics** if `e` is NaN or the
     truncated value does not fit in `T` ("float out of range", a new panic kind). The C code compares `e` against the
     exactly representable bounds before converting, so there is no UB.
   - `checkedTruncate<T>(e)` returns `T?` instead of panicking. `saturatingTruncate<T>(e)` clamps, with NaN giving 0,
     as Rust's `as` does.
   - `f.bits()` and `f64.fromBits(u)` (`f32` likewise) reinterpret the representation, for hashing and serialising.
     Literals are emitted to C by their bits, so their values never depend on the C compiler's decimal parsing.
6. **Printing.** `Console.write`, `writeLine` and `String.appendNumber` accept `f32` and `f64`. The text is the
   **shortest decimal that rounds back to the same value**, laid out as ECMAScript's `Number::toString` (ECMA-262
   §6.1.6.1.20):
   - plain digits for 1e-6 ≤ |x| < 1e21 (`0.1`, `1.5`, `100`, `123456.789`);
   - an exponent otherwise (`1e21`, `1.5e-7`);
   - `NaN`, `Infinity` and `-Infinity`, and `0` for both zeros.

   ECMAScript's rule is fully specified and widely implemented, so test vectors exist. The C runtime implements
   it with its own shortest-digits algorithm (Ryu or Schubfach, §5 Q4), using 64×64→128-bit multiplication in
   portable 32-bit halves (no `__int128`, which MSVC lacks). The interpreter gets the shortest digits from .NET's
   `ToString("R")` (shortest round-trip since .NET Core 3.0) and lays them out by the same rule.
7. **Profiles.** Floats are allowed in hosted programs and CosmoOS user programs. In the freestanding profile, a
   build option `--no-float` (CosmoOS kernel builds pass it, from `c4.py`–`c8.py`) makes any float type, literal or
   operation an error, with a new code. `check-fpregs.sh` stays the backstop, and G7 adds it to `c6.py`–`c8.py`. Without the option, freestanding code
   may use floats, as `profiles.md` §1's "target-defined" says. Printing floats is hosted-only (it uses the console).
8. **Not in G7.** `%`, a math library (`sqrt`, `floor`, `sin`, …), parsing floats from text, `f16`/`f128`,
   SIMD, rounding modes and FP exceptions, floats as `match` patterns, and floats in `const` expressions beyond
   literals and the four operators. The first two come next (§5 Q5).

## 4. Consequences

- Ex2 and Ex13 of `language-spec.md` become runnable, and numeric code can be written. The README's "What the reference does
  not have yet" loses "floating-point types".
- A float result is the same compiled with clang, gcc or MSVC and interpreted, so the fuzzer can generate float code
  and compare outputs. The cost is the forbidden contraction: FMA-heavy kernels are slower than C compiled with
  contraction on. A future explicit `f.mulAdd(a, b)` (an FMA builtin) would restore it where wanted. Any performance
  claim needs a benchmark first (a float workload in `benchmarks/suite`, e.g. n-body).
- The C runtime grows by a formatter and its tables (inferred: about 10 KiB of tables for Ryu's `double` path; less
  for Schubfach). It is linked only into programs that print floats.
- `truncate` on floats can panic, unlike `truncate` on integers. The name says what happens to the fraction, and the
  panic is the out-of-range case. Q3 offers a different name.
- The CosmoOS kernel profile is unchanged in effect: floats were forbidden, and stay so through `--no-float`.

## 5. Questions for the owner (decided 2026-10-25: each as recommended)

1. **Approach A** (IEEE, deterministic, no contraction)? Recommended: yes.
2. **Never-panicking float arithmetic** (infinity and NaN) as an explicit exception to OD-6? Recommended: yes. The
   alternative, panicking on non-finite results, is expensive (a check after every operation) and unlike every
   other language.
3. **Conversion names.** `round<T>` for to-float, and `truncate<T>` (panicking), `checkedTruncate<T>` and
   `saturatingTruncate<T>` for to-integer? Alternative: Rust's saturating `as` for float → int. Recommended: the
   proposal, since an unexpected NaN or huge value then fails loudly.
4. **Formatter.** A port of Ryu (Apache-2.0 or Boost licence; its licence would sit in `runtime/`), or a new
   implementation of Schubfach from its paper (no licence question; more of our own code to test)? Recommended:
   Schubfach, tested against .NET's shortest output on several million random and boundary values.
5. **Math library scope after G7.** Recommended: next, `sqrt`, `abs`, `floor`, `ceil`, `min` and `max`, which lower
   to single instructions on both targets (`__builtin_sqrt` with `-fno-math-errno`, `__builtin_floor`, …). Without
   libm, everything else would be a reference implementation with its own accuracy tests.
6. **`--no-float`** as an option rather than a separate profile? Recommended: an option. Profiles stay as
   `profiles.md` defines them, and a Linux kernel build would use the same option.

## 5a. Amendments during implementation (2026-10-25)

- **G7a.** `rt.h`'s IEEE checks, contraction pragmas and float helpers apply only in programs that use floats
  (`CEE_FLOATS`).
  - Without that guard, the check `FLT_EVAL_METHOD == 0` broke every CosmoOS kernel object on x86-64.
  - CosmoOS builds those objects with `-mgeneral-regs-only`, which leaves no SSE there (`c4.py`).
- **G7b.**
  - **Exponent sign.** §3.6's examples wrote `1e21`. ECMAScript writes `1e+21`, and the reference follows ECMAScript exactly,
    so its test vectors apply (type-system.md §35.7).
  - **The interpreter's digits** come from an exact search over rationals, not from .NET's `ToString("R")`. .NET 10
    prints 2^-25 as `2.980232238769531E-08`, which does not read back as 2^-25.
  - **One-digit results.** Schubfach is used without the Java code's two-digit minimum, so `5e-324` prints as
    `5e-324`.

## 6. Implementation plan (once accepted)

Spec first (`type-system.md` §35, `grammar.md`, `c-backend.md`, `profiles.md`), then:

- **G7a.** Types, literals, operators, comparisons, `as`, `round`, `truncate` and its variants, `bits`/`fromBits`, C
  emission and the interpreter.
- **G7b.** The formatter and printing, in the C runtime and the interpreter.
- **G7c.** `--no-float`, the fuzzer generating float expressions, an MSVC run, and a float benchmark.

Tests to add (none run yet):

- run-pass for every operator at ±0, ±infinity, NaN, subnormals and the boundaries of each conversion;
- run-fail for `truncate` of NaN and out-of-range values;
- check-fail for mixed types, `%`, bitwise operators and integer literals where a float is expected;
- a formatter test against .NET on random bits and on ECMAScript's test cases;
- C output compiled with clang, gcc and MSVC, and compared with the interpreter.
