---
title: "Arithmetic: checked, wrapping, conversions"
summary: The whole of number arithmetic - operators that panic on overflow, the wrapping, checked and saturating methods, bit operators and shifts on unsigned types, every conversion between integer and float types, how floats print, and the checks the compiler removes.
assumes: [ch03-02-data-types, ch06-03-optionals, ch09-01-panics]
keywords: [overflow, wrappingAdd, checkedAdd, saturatingAdd, mulHigh, unsignedAbs, shift, rotateLeft, countOnes, as, truncate, checkedTruncate, saturatingTruncate, round, f64, NaN, appendFixed, "--explain-overflow-checks"]
diagnostics: [N0207, N0217]
specification: ["type-system.md §5", "type-system.md §6", "type-system.md §7", "type-system.md §35.3", "type-system.md §35.4", "type-system.md §35.7", "optimisations.md §3.4"]
---

# Arithmetic: checked, wrapping, conversions

**This section teaches** everything about arithmetic on numbers: what each operator does when its result does
not fit, the methods that give another behaviour, bits and shifts, every way to convert a number to another type,
and how floats behave and print. It assumes [Data types](./ch03-02-data-types), which introduces the types and
the overflow panic, [Optionals](./ch06-03-optionals) and [Panics](./ch09-01-panics).

The principle is one sentence: **an operator never gives a wrong number silently.** Where C wraps around or has
undefined behaviour, a CeeSafe operator panics, and a method with a plain name gives each other behaviour.

## What the operators do

Both operands of a binary operator have the same type, and the result has that type. Nothing is widened.

| Operators | On | When the result does not fit |
|---|---|---|
| `+` `-` `*` | integers | panic, `integer overflow` |
| `/` `%` | integers | panic on a zero divisor, and on the least value divided by -1. `/` rounds toward zero; `%` has the sign of the left operand |
| unary `-` | signed integers | panic on the least value |
| `&` `\|` `^` | unsigned integers | cannot fail |
| `<<` `>>` | unsigned integers | panic when the amount is negative or not less than the width. Bits shifted out are dropped |
| `+` `-` `*` `/` | floats | never panic: the result is infinity or NaN |

The checks are in every build, optimised or not. `x += y` is `x = x + y` and panics the same way.

## Saying what overflow should do

Every integer type has these methods. Each takes one argument of the same type.

| Method | Result | When the true result does not fit |
|---|---|---|
| `wrappingAdd`, `wrappingSub`, `wrappingMul` | `T` | the low bits: the result modulo 2 to the power of the width |
| `checkedAdd`, `checkedSub`, `checkedMul` | `T?` | `null` |
| `saturatingAdd`, `saturatingSub`, `saturatingMul` | `T` | the nearest of the type's least and greatest values |
| `min`, `max` | `T` | cannot overflow |
| `mulHigh` | `T` | cannot overflow: the upper half of the full product |
| `abs` (signed only) | `T` | panic on the least value |
| `unsignedAbs` (signed only) | the unsigned type of the same width | cannot overflow |

<span class="listing-caption"><strong>Listing 20-29</strong>, <code>main.cee</code>: The family on a <code>u8</code> near its greatest value and an <code>i8</code> at its least.</span>

```ceesafe
fn main() {
    let level: u8 = 250;
    Console.writeLine(level.wrappingAdd(10));
    Console.writeLine(level.saturatingAdd(10));
    Console.writeLine(level.checkedAdd(10).isNull());
    Console.writeLine(level.checkedSub(10) ?? 0);
    Console.writeLine(level.wrappingMul(2));
    Console.writeLine(level.saturatingSub(251));

    let low: i8 = -128;
    Console.writeLine(low.wrappingSub(1));
    Console.writeLine(low.saturatingSub(1));
    Console.writeLine(low.unsignedAbs());
    Console.writeLine(low.checkedMul(-1).isNull());

    Console.writeLine(level.min(7));
    Console.writeLine(level.max(7));
    let big: u64 = 0xFFFF_FFFF_FFFF_FFFF;
    Console.writeLine(big.mulHigh(16));
    Console.writeLine(big.wrappingMul(16));
}
```

```text
4
255
true
240
244
0
127
-128
128
true
7
250
15
18446744073709551600
```

- 250 + 10 is 260. Wrapping keeps 260 - 256, saturating stops at 255, and checking gives `null`.
- On a signed type, wrapping is two's complement: one below -128 is 127.
- `low.abs()` would panic, because 128 is not an `i8`. `unsignedAbs` returns a `u8`, which holds it.
- `mulHigh` and `wrappingMul` are the two halves of a product twice as wide as the type.

Choose by what a result outside the type means in your program:

- it cannot happen, and if it does the program has a bug: the **operator**;
- it is an expected case that the code handles: **checked**, with `??`, `match` or `?`;
- the value is a level or a count with a natural limit: **saturating**;
- the arithmetic is modular by design, as in a hash, a checksum or a sequence number: **wrapping**.

There is no checked or wrapping division. `/` and `%` fail only on a zero divisor and on the least value divided
by -1, and an `if` tests for both.

## Bits and shifts

The bit operators `&`, `|`, `^` and the shifts work on **unsigned** types only. So no sign bit is ever shifted
or masked by accident, and `>>` always fills with zeros. To work on the bits of a signed value, convert it with
`truncate` first. There is no `~`: complement with `x ^ 0xFF`, using as many `F`s as the type is wide.

<span class="listing-caption"><strong>Listing 20-30</strong>, <code>main.cee</code>: Bit operators, shifts and the bit methods on a <code>u8</code>, and the bits of a negative <code>i32</code>.</span>

```ceesafe
fn main() {
    let flags: u8 = 0b1001_0110;
    Console.writeLine(flags & 0x0F);
    Console.writeLine(flags | 0x01);
    Console.writeLine(flags ^ 0xFF);
    Console.writeLine(flags << 1u32);
    Console.writeLine(flags >> 4);
    Console.writeLine(flags.wrappingShl(9));
    Console.writeLine(flags.rotateLeft(4));
    Console.writeLine(flags.countOnes());
    Console.writeLine(flags.leadingZeros());
    Console.writeLine(flags.trailingZeros());
    Console.writeLine(0x1234u16.byteSwap());

    let signed: i32 = -8;
    let bits = truncate<u32>(signed);
    Console.writeLine(bits >> 28);
    Console.writeLine(truncate<i32>(bits >> 1));
}
```

```text
6
151
105
44
9
44
105
4
0
1
13330
15
2147483644
```

- `flags << 1` loses the top bit: 150 doubled is 300, and the eight bits that remain are 44. A shift panics for
  a bad *amount*, not for bits that leave.
- The amount may be of any integer type. A literal amount is a `u32`.
- `wrappingShl(n)` and `wrappingShr(n)` take the amount modulo the width, so 9 shifts a `u8` by 1 and nothing
  panics. `rotateLeft` and `rotateRight` bring the bits back at the other end.
- `countOnes`, `leadingZeros` and `trailingZeros` return a `u32`. `byteSwap` reverses the bytes.

## Arithmetic on literals is done by the compiler

An expression made only of integer literals and operators is computed when the program is compiled. If it would
panic, that is a compile error, `N0217`, since the statement could never run without panicking. Listing 20-34
shows three. The rule is about what is written: as soon as a name or a call is part of the expression, it is
ordinary arithmetic and is checked when it runs.

## Conversions

No number changes type by itself. Four spellings convert, and each says what may happen to the value.

| Spelling | From, to | What happens to a value that does not fit |
|---|---|---|
| `x as T` | only where every value of the source fits `T` | cannot happen: the compiler refuses the others |
| `truncate<T>(x)` | integer to integer | the low bits are kept (two's complement) |
| `truncate<T>(x)` | float to integer | the fraction is dropped; a value out of range, an infinity or NaN panics |
| `checkedTruncate<T>(x)` | integer or float to integer | `null` |
| `saturatingTruncate<T>(x)` | integer or float to integer | the nearest value of `T`; NaN gives 0 |
| `round<T>(x)` | integer or float to float | the nearest float; too large gives infinity |

`as` accepts a wider integer of the same signedness (`u8` to `u32`), an unsigned integer to a wider signed one
(`u8` to `i16`), `bool` to any integer, `f32` to `f64`, and an integer to a float that holds all its values:
`i8`, `i16`, `u8`, `u16` to either float type, `i32` and `u32` to `f64`.

<span class="listing-caption"><strong>Listing 20-31</strong>, <code>main.cee</code>: Each conversion, on integers and on floats.</span>

```ceesafe
fn main() {
    let small: i16 = -300;
    let wide = small as i64;
    let ratio = small as f64;
    Console.writeLine(wide);
    Console.writeLine(ratio / 8.0);

    let n: i64 = 70000;
    Console.writeLine(truncate<u16>(n));
    Console.writeLine(saturatingTruncate<u16>(n));
    Console.writeLine(checkedTruncate<u16>(n).isNull());
    Console.writeLine(round<f32>(n));

    let x = 2.75;
    Console.writeLine(truncate<i32>(x));
    Console.writeLine(truncate<i32>(-x));
    Console.writeLine(truncate<i32>(x.floor()));
    Console.writeLine(truncate<i32>((x + 0.5).floor()));
    Console.writeLine(saturatingTruncate<u8>(1e9));
    Console.writeLine(saturatingTruncate<u8>(0.0 / 0.0));
    Console.writeLine(checkedTruncate<u8>(-1.0).isNull());
    Console.writeLine(round<f32>(0.1) as f64);
}
```

```text
-300
-37.5
4464
65535
true
70000
2
-2
2
3
255
0
true
0.10000000149011612
```

- 70000 is 65536 + 4464, so `truncate<u16>` keeps 4464. `saturatingTruncate` gives the greatest `u16`.
- `truncate` of a float rounds toward zero: 2.75 becomes 2 and -2.75 becomes -2. For other rounding, round the
  float first with `floor()` or `ceil()`. `(x + 0.5).floor()` rounds a positive value to the nearest integer.
- An `i64` does not convert to a float with `as`, since a float cannot hold every `i64`. `round<f32>(n)` says
  that the nearest one will do.
- The last line shows why `f64` to `f32` is `round` and not `as`: the `f32` nearest 0.1 is another number.

A common need is an index: a `u32` becomes a `usize` with `as`, and an `i64` or a `usize` going the other way
needs `checkedTruncate<i64>(n)` or `truncate`.

## Floats

`f32` and `f64` follow IEEE 754. Their arithmetic never panics: a result too large is `Infinity`, and a result
with no value, such as zero divided by zero, is `NaN`. NaN is unequal to everything, itself included. Floats
have no `%`, no bit operators, and cannot be matched on. The methods are `sqrt`, `abs`, `floor`, `ceil`, `min`
and `max`.

<span class="listing-caption"><strong>Listing 20-32</strong>, <code>main.cee</code>: How floats print, and three functions that format a number into a <code>String</code>.</span>

```ceesafe
fn main() {
    Console.writeLine(1.0 / 3.0);
    Console.writeLine(0.1 + 0.2);
    Console.writeLine(0.1f32);
    Console.writeLine(1e21);
    Console.writeLine(1.5e-7);
    Console.writeLine(100.0);
    Console.writeLine(1.0 / 0.0);
    Console.writeLine(0.0 / 0.0);
    Console.writeLine(2.0.sqrt());

    var text = String.create();
    text.appendFixed(3.14159, 2)!;
    text.append(" ")!;
    text.appendHex(255, 4, false)!;
    text.append(" ")!;
    text.appendPadded("7", 3, TextAlign.Right, '0')!;
    Console.writeLine(text.view());
}
```

```text
0.3333333333333333
0.30000000000000004
0.1
1e+21
1.5e-7
100
Infinity
NaN
1.4142135623730951
3.14 00ff 007
```

A float prints as the shortest decimal that reads back as the same value, in the layout JavaScript uses: no
trailing `.0`, and an exponent from 1e21 up and below 1e-6. There are no format strings. For a fixed number of
decimals use `appendFixed(value, digits)` on a `String`; `appendHex(value, minWidth, upper)` writes hexadecimal
and `appendPadded(text, width, align, fill)` pads text.

## The checks the compiler removes

A check costs a comparison and a branch. The compiler removes each one it can prove will never fail: a loop
counter that stays below its bound, a subtraction after a test. `ceesafec build --explain-overflow-checks` lists
every checked `+`, `-`, `*` and negation of the program, with `removed` or `kept`.

<span class="listing-caption"><strong>Listing 20-33</strong>, <code>main.cee</code>: Two ways to write a midpoint, and a checksum that wraps on purpose.</span>

```ceesafe
fn average(a: u32, b: u32) -> u32 => (a + b) / 2;

fn middle(low: usize, high: usize) -> usize => low + (high - low) / 2;

fn checksum(bytes: Slice<u8>) -> u32 {
    var sum: u32 = 0;
    for (b in bytes) {
        sum = sum.wrappingMul(31).wrappingAdd(b as u32);
    }
    return sum;
}

fn main() {
    Console.writeLine(average(10, 20));
    Console.writeLine(middle(4, 10));
    Console.writeLine(checksum("overflow".bytes()));
}
```

```text
15
7
529642498
```

```sh
ceesafec build main.cee --explain-overflow-checks
```

```text
main.cee:1:38: kept
main.cee:3:48: removed
main.cee:3:54: kept
main.cee:7:5: removed
```

`a + b` in `average` is kept: two large `u32`s overflow, and the program would panic there. In `middle`,
`high - low` is kept, since nothing says `low` is the smaller. Once it has succeeded, `low` plus half the
difference cannot pass `high`, so that `+` is removed. The loop's hidden counter is removed too. The wrapping
methods are not in the list: they have no check. Do not reach for `wrappingAdd` to make code faster. It changes
what the program computes, and the checks that matter in a loop are usually the ones the compiler removes.

## Mistakes

::: warning Mistake: arithmetic on literals that cannot succeed
The compiler computes it, and reports what the program would have panicked with.
:::

<span class="listing-caption"><strong>Listing 20-34</strong>, <code>main.cee</code>: Three expressions of literals: an overflow, a shift too far, and a division by zero. This does not compile.</span>

<!-- ceesafe: fail N0217 -->
```ceesafe
fn main() {
    let big: u8 = 200 + 100;
    let width: u32 = 1 << 40;
    let share = 100 / (4 - 4);
    Console.writeLine(big);
}
```

```text
main.cee:2:19: error[N0217]: constant evaluation failed: arithmetic overflow: the result 300 does not fit in 'u8'
  |
2 |     let big: u8 = 200 + 100;
  |                   ^^^^^^^^^

main.cee:3:22: error[N0217]: constant evaluation failed: shift amount 40 is out of range for 'u32' (0..31)
  |
3 |     let width: u32 = 1 << 40;
  |                      ^^^^^^^

main.cee:4:17: error[N0217]: constant evaluation failed: division by zero
  |
4 |     let share = 100 / (4 - 4);
  |                 ^^^^^^^^^^^^^
```

Give the variable a type that holds the value (`let big: u16`, `let width: u64`), or call the method that names
the behaviour you want: `200u8.wrappingAdd(100)`.

::: warning Mistake: bit operators on a signed integer
`&`, `|`, `^`, `<<` and `>>` need unsigned operands.
:::

<span class="listing-caption"><strong>Listing 20-35</strong>, <code>main.cee</code>: A mask applied to an <code>i32</code>. This does not compile.</span>

<!-- ceesafe: fail N0207 -->
```ceesafe
fn main() {
    let mask: i32 = 0xF0;
    let value: i32 = 0x3C;
    Console.writeLine(value & mask);
}
```

```text
main.cee:4:23: error[N0207]: operator '&' cannot be applied to type 'i32'; bitwise operators and shifts require unsigned operands (reinterpret with truncate<uN>(x))
  |
4 |     Console.writeLine(value & mask);
  |                       ^^^^^^^^^^^^
```

Declare flags and masks as `u32`. For a value that must be signed, write
`truncate<u32>(value) & truncate<u32>(mask)`.

::: warning Mistake: a shift by the width or more
C leaves this undefined. Here it panics.
:::

<span class="listing-caption"><strong>Listing 20-36</strong>, <code>main.cee</code>: A <code>u32</code> shifted by 31 and then by 32.</span>

<!-- ceesafe: panics -->
```ceesafe
fn shifted(value: u32, by: u32) -> u32 => value << by;

fn main() {
    Console.writeLine(shifted(1, 31));
    Console.writeLine(shifted(1, 32));
}
```

```text
2147483648
```

The second call stops the program:

```text
ceesafe panic: shift amount out of range at main.cee:1:43
```

When the amount comes from data, test it, or use `wrappingShl`, which takes it modulo the width.

::: warning Mistake: `truncate` of a float that has no integer
`truncate` of an integer always succeeds. `truncate` of a float panics for a value out of range, an infinity or
NaN.
:::

<span class="listing-caption"><strong>Listing 20-37</strong>, <code>main.cee</code>: A float far beyond an <code>i64</code>.</span>

<!-- ceesafe: panics -->
```ceesafe
fn main() {
    let x = 1e300;
    Console.writeLine(truncate<i64>(x));
}
```

The program prints nothing and stops:

```text
ceesafe panic: float out of range at main.cee:3:23
```

For a float that comes from input or from a division, use `checkedTruncate`, which gives `null`, or
`saturatingTruncate`, which clamps.

::: details Coming from Rust
- Overflow panics in every build, not only in debug builds. The methods have Rust's names in camel case:
  `wrapping_add` is `wrappingAdd`. There are no `overflowing_*` methods and no `Wrapping` type.
- `as` never loses information. Rust's `x as u8` is `truncate<u8>(x)`; `u8::try_from(x)` is
  `checkedTruncate<u8>(x)`, an optional; a float `as` integer, which saturates in Rust, is
  `saturatingTruncate`.
- Bit operators and shifts are for unsigned types only, and there is no `!x` on integers.
- A shift by the width or more panics in every build.
:::

::: details Coming from C
- Nothing is promoted: a `u8` plus a `u8` is a `u8`, and it panics at 256.
- Signed overflow is not undefined and unsigned overflow does not wrap: both panic. Wrapping is a method.
- A cast that can lose information does not exist. Choose `truncate`, `checkedTruncate` or `saturatingTruncate`.
- `>>` on a negative number does not exist either: convert to unsigned and decide what the top bits should be.
:::

::: details Coming from C#, Java, TypeScript or Go
- Java, Go and unchecked C# wrap silently on overflow. Here that is `wrappingAdd`; the operator panics, as in
  C#'s `checked` blocks.
- There is no implicit widening from `i32` to `i64` or from an integer to a float: write `as`.
- JavaScript's one number type is `f64`, and floats print the same way in both languages. Integers are separate
  types and never become floats by themselves.
:::

## Summary

- `+`, `-`, `*`, a zero divisor, negating the least value and a shift amount out of range panic, in every build.
- `wrapping…` keeps the low bits, `checked…` gives an optional, `saturating…` clamps; each for `Add`, `Sub` and
  `Mul`. `min`, `max`, `mulHigh` and `unsignedAbs` cannot overflow.
- Bit operators and shifts need unsigned operands (`N0207`). There is no `~`.
- Arithmetic on literals alone is computed by the compiler, and one that would panic is an error (`N0217`).
- `as` converts only without loss. `truncate` keeps the low bits of an integer and drops the fraction of a
  float; `checkedTruncate` gives an optional; `saturatingTruncate` clamps; `round<T>` converts to a float.
- Float arithmetic never panics and gives `Infinity` or `NaN`. Floats print as the shortest decimal that reads
  back; `appendFixed` writes a fixed number of decimals.
- `ceesafec build --explain-overflow-checks` shows which checks the compiler proved away.

Next: [Derives and macros](./ch20-05-derives-and-macros).
