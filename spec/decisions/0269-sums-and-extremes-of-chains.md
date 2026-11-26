# ADR 0269 — `sum`, `min` and `max` on chains, through a `Number<T>` interface

- Status: **Accepted** by the owner, 2026-11-27 ("Accept all": §6's five questions as recommended). Proposed the same
  day, at the owner's choice ("Add sum/min/max") after `fold(0, …)` was found not to compile over a chain of `usize`.
  **Implemented** on the branch `adr/0269-chain-sums` (§7), not released.
- Scope: lib/core (an interface `Number<T>` and its impls for the number types; three methods of `Chain<T, I>`,
  ADR 0264); type-system.md §36.4a; guide/for-agents.md, guide/coming-from/rust.md, the book's 13.2 and appendix C;
  spec/api/core.md; corpus tests. Amends ADR 0264 §2.4 ("No `sum`, `min` or `max`").

## 1. Context

ADR 0264 left chains without `sum`, `min` and `max`, since a generic `T` has no `+` or `<`, and said that `fold`
does them: `fold(0, (a, x) => a + x)`. For a chain of `i32` it does. For any other element type it does not compile:

```text
let total = words.values().map((w) => w.length).fold(0, (a, n) => a + n);   // N0205: expected 'i32', found 'usize'
```

The literal `0` is `i32` unless its parameter's type fixes it, and `fold`'s accumulator `A` is fixed by nothing else
before the closure is checked (type-system.md §20.2): the closure's `a` is `A`, still open, so the closure determines
nothing. The call's expected type is not used either, so `let total: usize = …` fails the same way. The guide
(for-agents.md, coming-from/rust.md) and type-system.md §36.4a recommend exactly that `fold`. Lengths, counts and
sizes are `usize`, so the case is common.

A prototype (ceesafec 0.95.22) shows the library can carry the operations: an interface may be implemented for a
number type (`impl Number<usize> for usize`), a generic function bounded by it calls `x.plus(y)`, `x.less(y)` and the
static `T.zero()`, interpreted and native.

## 2. Decision

### 2.1 `Number<T>` in lib/core

```ceesafe
public interface Number<T> {
    fn zero() -> T;                // 0, or 0.0
    fn plus(this, other: T) -> T;  // `this + other`: an integer overflow panics, as `+` does
    fn less(this, other: T) -> bool; // `this < other`: for floats IEEE's, so NaN is less than nothing
}
```

lib/core implements it for `i8`, `i16`, `i32`, `i64`, `isize`, `u8`, `u16`, `u32`, `u64`, `usize`, `f32` and `f64`.
A program may implement it for a type of its own.

### 2.2 Three consumers of a chain

In an `impl<T: Number<T> + Copy, I: Iterator<T>> Chain<T, I>`:

| Method | Gives |
|---|---|
| `sum()` | `T`: the values added left to right from `T.zero()`; `zero()` for an empty chain; an integer overflow panics |
| `min()` | `T?`: the first of the smallest values, null for an empty chain |
| `max()` | `T?`: the first of the largest values, null for an empty chain |

So `words.values().map((w) => w.length).sum()` is a `usize`. A NaN is never chosen by `min` or `max` unless it is the
first value (`less` is false for it either way).

### 2.3 What is corrected

type-system.md §36.4a, the guide and the book stop recommending `fold(0, …)` for a sum. Where a `fold` starts from a
literal, they say that the literal is `i32` unless written otherwise (`fold(0usize, …)`).

## 3. Options not taken

- **Changing inference** (an integer literal decided after the closures, or from the call's expected type). It would
  fix `fold(0, …)` too, but it changes a deliberate rule of §20.2 and needs a literal type that waits to be decided:
  a language change with effects well beyond chains. The owner chose the library.
- **`sum` per element type** (`sumUsize()`, …). Twelve names for one operation.
- **A trait for `+` itself** (an operator interface). More general, and a language change: `a + b` on a type
  parameter.

## 4. Consequences

- `sum`, `min` and `max` work on any chain of numbers, and on a program's own type that implements `Number<T>`.
- `fold(0, …)` over a non-`i32` chain is still N0205; its message is unchanged, and the documents say to write the
  literal's type.
- One more lib/core name, `Number`. A program's own `Number` takes precedence in its own code, as with any lib/core
  name.

## 5. What is not known

- Whether `sum` over floats should add in another order (pairwise, compensated) for accuracy. Left to right is what
  a loop does, and what this ADR specifies.

## 6. Questions for the owner

1. **The interface**: `Number<T>` with `zero`, `plus` and `less`, implemented for the twelve number types?
   Recommended: yes.
2. **The methods**: `sum()` (`zero()` when empty, overflow panics), `min()` and `max()` (`T?`, the first of equals)?
   Recommended: yes.
3. **The name** `Number`? Recommended: yes; it says what the types are, and a program's own `Number` still wins in
   its code.
4. **The documents**: correct every `fold(0, …)` advice, and say how to type the literal? Recommended: yes.
5. **Release** as a patch once the suite passes? Recommended: yes.

## 7. The design as built (2026-11-27)

- **lib/core/chains.cee**: `public interface Number<T>` (`zero()`, `plus`, `less`) and its impls for the twelve number
  types (`this + other`, `this < other`, `0` or `0.0`), and `impl<T: Number<T> + Copy, I: Iterator<T>> Chain<T, I>`
  with `sum`, `min` and `max` as §2.2 says. No compiler change.
- **Documents**: type-system.md §36.4a (a "Numbers" item; `sum`, `min` and `max` no longer under "Not provided"),
  guide/for-agents.md, guide/coming-from/rust.md, the book's 13.2 (its chain example is `….sum()`), ADR 0264 §2.4's
  note; appendix C and spec/api/core.md regenerated.
- **Tests**: the corpus's `run-pass/chain-sums.cee` (`usize` lengths, `i64`, floats, an empty chain, ties, a program's
  own `Number<T>`) and `run-fail/chain-sum-overflow.cee` (a `u8` sum that overflows panics in lib/core's `plus`),
  built and interpreted, with leak checks.
