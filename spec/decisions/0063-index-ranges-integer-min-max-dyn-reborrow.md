# ADR 0063 — Index ranges, integer `min`/`max`, and reborrowing into `dyn`

- Status: **Accepted** by the owner, 2026-11-03 ("I accept the ADR", then "Both 0063 and 0064"): §5 questions 1 and 3
  as recommended (the narrow index rule; no `min`/`max` on `char` or `bool`). Question 2: "Both", so §2.5 adds a
  panicking `abs` and a never-panicking `unsignedAbs`. Implemented on the working tree; not released. Raised by the ClayWind
  port (`~/dev/CeeSafeApps/CEESAFE-FINDINGS.md`, items A1, A4 and B), a ~25k-line pure CeeSafe program: a Clay layout
  engine, a software rasterizer matched to ThorVG, a TrueType reader, controls and three hosts.
- Scope: the checker (`src/check/body.cee`, `src/check/expr.cee`, `src/check/io.cee`, `src/check/members.cee`, the new
  `src/check/range_index.cee`), the built-in list (`src/hir/builtins.cee`), the interpreter
  (`src/interpreter/runtime.cee`), the C backend (`src/backend/c_body.cee`) and `rt.h`. Diagnostics: N0246's message
  and fix, N0295's explanation. Spec: type-system.md §6, §16.4, §37.2.

## 1. Context

Three frictions came up over and over in a real program, each with a cheap, local fix.

1. **`for (i in 0..16) { a[i] = … }` does not compile.** Both bounds are flexible literals, so the range is `i32`,
   and an index is `usize` (N0205). The port wrote `0usize..16` in about forty places. The guide's "common mistakes"
   lists it. A range of literals that indexes is the commonest loop there is.
2. **`i32` and `u32` have no `min` or `max`.** Floats do (§35.5). Integer code wrote `if (a < b) a else b`, or small
   `imin`/`imax` helpers per package.
3. **A `borrow mut T` passed where a `borrow mut dyn I` is expected is moved, not reborrowed.** §13.3 says every use of
   a `borrow mut` reborrows, and it does at the same type. The coercion's operand was the variable itself, so the
   move checker saw a move (N0250 on the second use). The port wrapped a platform in a struct to lend a field each
   frame. A `borrow mut` *field* could not be passed this way at all (N0251).

## 2. Decision

### 2.1 Index ranges (§16.4)

When both bounds of a `for` range are flexible literals, the range is `usize` if the body **indexes with** the
variable, and `i32` (the literals' default) otherwise. The body indexes with `i` where `i` is an index (`a[i]`) or a
slice bound (`a[lo..i]`), or an operand of `+ - * / %` that is one (`a[i * w + x]`), anywhere in the body: nested
blocks and closures included. Inside a call, cast, comparison or condition within an index it does not count.

- **Syntactic, before the body is checked** (`indexesWith`, `src/check/range_index.cee`). Locals cannot be shadowed
  (N0203), so the name means the variable throughout, and the decision needs no type information. The first use does
  not decide it, so the order of statements never changes a type.
- **Narrow on purpose.** `a[truncate<usize>(i)]` and `a[f(i)]` keep `i32`: there the program already converts, and
  changing the variable's type would break the conversion or the call. Only the pattern the program cannot write
  today becomes valid.
- **Compatibility.** A program that compiles today does not index with an `i32` loop variable, since that is N0205.
  It could still use `i` both inside an index's arithmetic and as an `i32` elsewhere, but only if the index were also
  converted, which the arithmetic rule excludes. The compiler itself (about 170k lines), `lib/`, the corpus and the
  ClayWind and CosmoStack packages check unchanged. The one false positive found while building (a variable inside
  an `if` condition within an index, `nir_builder.cee:2340`) is what fixed the rule to arithmetic operands only.

### 2.2 Integer `min` and `max` (§6)

Every integer type gets `x.min(y)` and `x.max(y)`, of result `T`. They take one argument of the receiver's type (a
flexible literal adapts) and never overflow; with equal operands the result is `x`. They are built-ins like
`wrappingAdd` (`bfIntMin`, `bfIntMax`): `cee_min_<S>`/`cee_max_<S>` in `rt.h`, and an `intCompare` in the
interpreter. Their names join the reserved integer method names (N0228).

`abs` is §2.5.

### 2.5 `abs` and `unsignedAbs` (accepted §5 question 2: "Both")

Every signed integer type gets:
- `x.abs()`, of type `T`. It panics at `T`'s minimum, whose magnitude `T` cannot hold, exactly as `-x` does (§6), and
  the panic is reported at the call.
- `x.unsignedAbs()`, of the unsigned type of the same width. It never panics: the minimum's magnitude fits.

On unsigned types both are N0234, since a non-negative value has no sign to drop. Both names join the reserved integer
method names (N0228).

### 2.3 Reborrowing into `dyn` (§37.2)

A `borrow mut T` variable or field that converts to `borrow mut dyn I` is first reborrowed, as at the same type
(`coerce`, `src/check/expr.cee`): the coercion's operand is the `Reborrow`, so the variable stays usable after the
call and is frozen while the reborrow lives (B4). This is a fix, not a new rule: §13.3 already says it. It also lets a
`borrow mut` field be passed through `dyn`, which was N0251. The aliasing checks are unchanged:
`check-fail/dyn-reborrow.cee` shows a write and a read of the original while a `dyn` reborrow is live, still N0240 and
N0242.

### 2.4 Diagnostics

- **N0246** (`var k = len;` with `len: borrow mut T` and `T` Copy) adds: to copy the value, declare the variable's
  type. It carries a fix that inserts `: T` (`ceesafec fix` applies it), after which the declaration reads through
  the reference.
- **N0295 and the other effect explanations**: a chain that reaches an interface method without `@effects` now says
  so, and says that the list belongs on the interface (`@effects() fn …`), as it already did for a C function.
  Before, the explanation stopped at "which calls 'dyn I.m'".

## 3. Evidence (run 2026-11-03, macOS arm64)

- **New corpus tests:**
  - `run-pass/dyn-reborrow.cee`, `check-fail/dyn-reborrow.cee`, `run-pass/int-min-max.cee`,
    `run-pass/for-range-index.cee` and `check-fail/for-range-index.cee`. The run-pass expectations come from Python
    (`max`, `min`, `sum`).
  - With the released ceesafec 0.10.3 the dyn-reborrow pair fails, as the bug predicts. With this change all five
    pass, compiled and with `--interp`.
- **Full corpus:** `tools/run_corpus.py --corpus tests/reference/tests/corpus` passes 580 tests with 0 failures. The 4
  freestanding cases are partial, as before.
- **Fixed point:** the changed compiler builds itself, and that build emits byte-identical C (`--emit c`,
  `diff -rq`).
- **Contract suite:** `tools/contract_tests.py` (result recorded in ROADMAP §10).
- **Downstream:** ClayWind's 72 tests pass, and CosmoStack checks.

## 4. Consequences

- `0usize..N` stays valid, so nothing has to change. The guide's "common mistakes" entry can drop its index advice.
- The interpreter and the C backend each gain two small built-ins. `rt.h` gains `CEE_MINMAX`.
- The range rule makes a loop variable's type depend on its body. The rule is syntactic and narrow, and the error
  for a mixed use (`let s: i32 = i` beside `a[i]`) is the ordinary N0205 at that use.

## 5. Open questions for the owner

1. Accept §2.1's rule as specified, or the wider one (any mention inside an index)? The wider rule reports the
   compiler's own `nir_builder.cee:2340` loops, which use `k` in an `if` inside an index and as an `i64`.
2. `abs`: a panicking built-in (the operators' panic, at the call), or `unsignedAbs() -> uN`, which cannot overflow?
3. Should `min`/`max` also be on `char`/`bool`? Proposed: no.
