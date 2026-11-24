# ADR 0223 — Shifts of signed integers: `wrappingShl` and an arithmetic `wrappingShr`

- Status: **Proposed** 2026-11-24 and implemented on the branch `fix/findings-core`, at the owner's instruction (the
  SQLite port's findings, finding 2: "offer named operations … with defined behaviour for out-of-range shift
  counts"). Not released. §6's question is the owner's.
- Scope: type-system.md §6 (bit methods, the restriction of the bit operators); `src/check/io.cee`
  (checkIntegerBits), the C backend's helpers in `runtime/include/ceesafe/rt.h`, the interpreter
  (`src/interpreter/runtime.cee`); guide/builtins.md, guide/for-agents.md; book Appendix C.

## 1. Context

`>>` and `<<` take unsigned operands (§6, N0207): C's shifts of negative values are implementation-defined (`>>`)
or undefined (`<<`), and CeeSafe refuses them rather than inherit that. ADR 0071 §2.3 gave the unsigned types
`x.wrappingShl(n)` and `x.wrappingShr(n)`, with the amount taken modulo the width, and refused every bit method on
signed types (N0234), byteSwap aside.

A port of SQLite needs an arithmetic right shift: `pwr10to2(p) = (p * 108853) >> 15` is a floor division of a
possibly negative number by 2^15. Without one the port wrote `floorDivide(value, divisor)` by hand. Reinterpreting
with `truncate<u32>(x)` gives a logical shift, which is wrong for negative values; dividing by a power of two rounds
towards zero, which is wrong too. Hashes, fixed-point arithmetic and codecs (zigzag encoding, sign extension of a
field) want the same operations.

## 2. Decision

1. `x.wrappingShl(n)` and `x.wrappingShr(n)` exist on every signed integer type, with the signature they have on
   unsigned types: a `u32` amount (a flexible literal adapts), taken modulo the width `w`, and a result of type `T`.
   No other bit method is added to signed types (`rotateLeft`, `countOnes` and the rest stay N0234), and the
   operators stay unsigned only (N0207).
2. `wrappingShl` shifts the two's complement bits left: the result is `x` × 2^(n mod w) modulo 2^w, read in two's
   complement. It never panics: bits shifted past the top, the sign bit included, are lost.
3. `wrappingShr` is **arithmetic**: the bits shifted in are copies of the sign bit, so the result is
   ⌊`x` / 2^(n mod w)⌋, rounding towards negative infinity: `(-7).wrappingShr(1)` is −4, `(-1).wrappingShr(k)` is
   −1 for every `k`. It never panics.
4. An amount of the width or more wraps round, as on unsigned types and as Rust's `wrapping_shr` does:
   `x.wrappingShr(w)` is `x`. A shift that saturates instead, ⌊`x` / 2^n⌋ for every `n`, is written
   `x.wrappingShr(n.min(w - 1))`, which the spec states.
5. The C has no implementation-defined or undefined behaviour: `wrappingShl` is the unsigned shift of the same bits
   converted back with `cee_as_iN`; `wrappingShr` shifts only non-negative values, `x < 0 ? ~(~x >> n) : x >> n`,
   with `n` masked first. The interpreter computes on the type's bits and fills the top `n` bits with the sign.
6. The message of N0234 for a bit method on a signed type names the two shifts.

## 3. Why these names

The task offered `shiftRightArithmetic` and `shiftLeftWrapping`. The existing names already say what the operations
are, on the type the method is called on: Rust's `i32::wrapping_shr` is arithmetic and `u32::wrapping_shr` logical,
and a reader of CeeSafe's tables finds one row for both. A second name for the same operation on another
signedness would be two ways to write one thing. The cost is that "arithmetic" is not in the name: the spec, the
guide's table and Appendix C say it, with an example.

## 4. Alternatives not taken

- `>>` on signed types, defined as arithmetic, and `<<` panicking on overflow: the operators would then mean two
  different things by signedness, and `<<`'s overflow check (the sign bit changes) is a new kind of panic. Relaxing
  N0207 stays possible later (§6 says it is backward compatible).
- A saturating amount (⌊`x` / 2^n⌋ for `n ≥ w` too): mathematically tidier for `>>`, but it would make the signed and
  unsigned `wrappingShr` disagree on what an amount of `w` means. §2.4 gives it as one call.

## 5. Evidence

- `tests/reference/tests/corpus/run-pass/signed-shifts.cee`: 28 values across `i8`, `i16`, `i32`, `i64` and
  `isize` (amounts 0, below the width, the width, above it), expected values from Python's `>>` and `<<` reduced
  modulo 2^w; the last line is SQLite's `pwr10to2(-7)`. Built and with `run --interp`.
- `tests/reference/tests/corpus/check-fail/signed-shifts.cee`: the shifts' argument and result types, and the other
  bit methods and the operators still refused on signed types.

## 6. Question for the owner

Accept §2 with these names, or add `shiftRightArithmetic` as a second name for discoverability?
