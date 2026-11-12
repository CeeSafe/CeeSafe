---
title: Data types
summary: The built-in types - integers and their literals, floats, bool, characters and text, and arrays. Integer overflow stops the program, types never mix without a conversion, and as converts only when nothing can be lost.
assumes: [ch03-01-variables]
keywords: [i32, u8, usize, f64, bool, StringView, array, overflow, as, truncate, checkedTruncate, saturatingTruncate, tuple, type alias]
diagnostics: [N0100, N0101, N0205, N0207, N0208]
specification: ["type-system.md §1", "type-system.md §4", "type-system.md §6", "type-system.md §7", "type-system.md §16", "type-system.md §35"]
---

# Data types

**This section teaches** the types built into CeeSafe, what happens when arithmetic overflows, and how to convert a
value from one type to another. It assumes [Variables](./ch03-01-variables).

Every value has a type, and the compiler knows it before the program runs. The simple types are integers, floats
and `bool`. Text and arrays hold several values.

## Integers

| Bits | Signed | Unsigned |
|---|---|---|
| 8 | `i8` | `u8` |
| 16 | `i16` | `u16` |
| 32 | `i32` | `u32` |
| 64 | `i64` | `u64` |
| the machine's address size | `isize` | `usize` |

A signed type holds negative numbers, and an unsigned type does not: `i8` holds -128 to 127, and `u8` holds 0 to
255. Lengths and indexes are `usize`.

<span class="listing-caption"><strong>Listing 3-10</strong>, <code>main.cee</code>: Integer types and the ways to write an integer literal.</span>

```ceesafe
fn main() {
    let small: u8 = 255;
    let wide: i64 = -9_000_000_000;
    let mask = 0xFF;
    let bits = 0b1010;
    let mode = 0o17;
    let tagged = 7u16;
    let index: usize = 3;
    Console.writeLine(small);
    Console.writeLine(wide);
    Console.writeLine(mask);
    Console.writeLine(bits);
    Console.writeLine(mode);
    Console.writeLine(tagged);
    Console.writeLine(index);
}
```

```text
255
-9000000000
255
10
15
7
3
```

A literal can be decimal, hexadecimal (`0x`), binary (`0b`) or octal (`0o`), with `_` between digits to make it
readable. It takes its type from where it is used: `255` is a `u8` in `let small: u8 = 255;`. A suffix fixes the
type (`7u16`). With nothing to go by, an integer literal is an `i32`, so `mask`, `bits` and `mode` are `i32`.

A literal that does not fit its type, such as `let level: u8 = 256;`, is a compile error.

## Overflow is checked

When the result of `+`, `-` or `*` does not fit the type, the program stops with a **panic**. It does not wrap
around and continue with a wrong number. Dividing an integer by zero panics too.

<span class="listing-caption"><strong>Listing 3-11</strong>, <code>main.cee</code>: 260 does not fit a <code>u8</code>. This program panics.</span>

<!-- ceesafe: panics -->
```ceesafe
fn main() {
    var level: u8 = 250;
    level += 10;
    Console.writeLine(level);
}
```

The program prints nothing on standard output and writes this to standard error:

```text
ceesafe panic: integer overflow at main.cee:3:5
```

::: tip Rule
Integer arithmetic that overflows panics, in every build. To get another result, call a method that names it.

*Specification: `type-system.md` §6.*
:::

<span class="listing-caption"><strong>Listing 3-12</strong>, <code>main.cee</code>: Three ways to add when the sum may not fit.</span>

```ceesafe
fn main() {
    let level: u8 = 250;
    Console.writeLine(level.wrappingAdd(10));
    Console.writeLine(level.saturatingAdd(10));
    Console.writeLine(level.checkedAdd(10) ?? 0);
    Console.writeLine(level.checkedAdd(5) ?? 0);
}
```

```text
4
255
0
255
```

`wrappingAdd` wraps around (260 - 256), and `saturatingAdd` stops at the largest value. `checkedAdd` gives an
optional, which is a value or `null`; `?? 0` uses 0 when it is `null`. [Optionals](./ch06-03-optionals) are in
chapter 6. The same three exist for `Sub` and `Mul`.

## Floats

`f32` and `f64` are floating-point numbers of 32 and 64 bits. A literal with a decimal point or an exponent is a
float, and an `f64` unless something says `f32`.

<span class="listing-caption"><strong>Listing 3-13</strong>, <code>main.cee</code>: Float arithmetic beside integer arithmetic.</span>

```ceesafe
fn main() {
    let half = 0.5;
    let third: f32 = 1.0 / 3.0;
    let large = 2.5e3;
    Console.writeLine(half);
    Console.writeLine(third);
    Console.writeLine(large);
    Console.writeLine(0.1 + 0.2);
    Console.writeLine(7.0 / 2.0);
    Console.writeLine(7 / 2);
    Console.writeLine(-7 / 2);
    Console.writeLine(7 % 2);
    Console.writeLine(1.0 / 0.0);
}
```

```text
0.5
0.33333334
2500
0.30000000000000004
3.5
3
-3
1
Infinity
```

Integer division drops the fraction, toward zero, and `%` gives the remainder. Float arithmetic never panics: it
gives `Infinity` or `NaN` instead. An integer literal is not a float: `let ratio: f64 = 3;` does not compile, and
`3.0` does.

## `bool`, characters and text

<span class="listing-caption"><strong>Listing 3-14</strong>, <code>main.cee</code>: A <code>bool</code>, text and its length, and a character literal, which is a number.</span>

```ceesafe
fn main() {
    let ready = true;
    let empty = 3 > 5;
    Console.writeLine(ready && !empty);
    let name = "Ada";
    Console.writeLine(name);
    Console.writeLine(name.length);
    Console.writeLine(name == "Ada");
    let letter: u8 = 'A';
    Console.writeLine(letter);
    Console.writeLine(letter == 'A');
    Console.writeLine('é');
    Console.writeLine("é".length);
}
```

```text
true
Ada
3
true
65
true
233
2
```

- `bool` is `true` or `false`. Comparisons give a `bool`; `&&`, `||` and `!` combine them.
- A string literal is a `StringView`: a read-only view of text stored as UTF-8 bytes. Its `length` counts bytes,
  so `"é"` has length 2. `==` compares two texts.
- CeeSafe has no character type. A character literal such as `'A'` is an integer, the character's Unicode number,
  typed like any integer literal. Here it is a `u8`, one byte of text.

Text that grows is a `String`, which [chapter 8](./ch08-02-strings) teaches.

## Arrays

An array is a fixed number of values of one type. Its type is written `[T; N]`: `[u8; 2]` is two `u8` values.

<span class="listing-caption"><strong>Listing 3-15</strong>, <code>main.cee</code>: Making arrays, and reading and changing their elements.</span>

```ceesafe
fn main() {
    let primes = [2, 3, 5, 7];
    var counts = [0; 3];
    let bytes: [u8; 2] = [10, 20];
    counts[0] = 4;
    counts[2] += 1;
    Console.writeLine(primes[3]);
    Console.writeLine(primes.length);
    Console.writeLine(counts[0] + counts[1] + counts[2]);
    Console.writeLine(bytes[1]);
}
```

```text
7
4
5
20
```

`[0; 3]` is three copies of `0`. Indexes start at 0 and are `usize`. Elements can be assigned only when the array
is a `var`. An array never changes its length; a `Vector` can grow, and [chapter 8](./ch08-01-vectors) teaches it.

An index is checked each time it is used. An index past the end panics; it never reads other memory.

<span class="listing-caption"><strong>Listing 3-16</strong>, <code>main.cee</code>: Index 4 of an array of four elements. This program panics.</span>

<!-- ceesafe: panics -->
```ceesafe
fn main() {
    let primes = [2, 3, 5, 7];
    var index: usize = 3;
    index += 1;
    Console.writeLine(primes[index]);
}
```

The panic names the place of the index:

```text
ceesafe panic: index out of bounds at main.cee:5:30
```

## Conversions

CeeSafe never converts a number to another type by itself. You write the conversion, and its spelling says what
can happen to the value.

| Spelling | What it does |
|---|---|
| `x as T` | converts when `T` holds every value of `x`'s type; otherwise it does not compile |
| `truncate<T>(x)` | keeps the low bits of an integer (wraps); for a float, drops the fraction |
| `checkedTruncate<T>(x)` | an optional: `null` when the value does not fit `T` |
| `saturatingTruncate<T>(x)` | the nearest value that `T` holds |
| `round<T>(x)` | to a float type `T`, the nearest value |

<span class="listing-caption"><strong>Listing 3-17</strong>, <code>main.cee</code>: Conversions that cannot lose anything, and conversions that say how they lose it.</span>

```ceesafe
fn main() {
    let small: u8 = 200;
    let wide: i64 = 300;
    Console.writeLine(small as i32 + 100);
    Console.writeLine(true as u8);
    Console.writeLine(small as f64 / 3.0);
    Console.writeLine(truncate<u8>(wide));
    Console.writeLine(saturatingTruncate<u8>(wide));
    Console.writeLine(checkedTruncate<u8>(wide) ?? 0);
    Console.writeLine(checkedTruncate<i16>(wide) ?? 0);
    Console.writeLine(truncate<i32>(2.75));
    Console.writeLine(round<f64>(wide) / 8.0);
}
```

```text
300
1
66.66666666666667
44
255
0
300
2
37.5
```

::: tip Rule
`as` converts only when no value can be lost. A conversion that can lose information is `truncate`,
`checkedTruncate`, `saturatingTruncate` or `round`.

*Specification: `type-system.md` §7 and §35.4.*
:::

## What CeeSafe does not have

- **No tuples.** To group values, declare a struct ([chapter 5](./ch05-00-structs)).
- **No type aliases.** Write the type itself, or wrap it in a struct.
- **No character type**, as above.

## Mistakes

::: warning Mistake: mixing two number types
Both sides of an operator must have the same type, even `i32` and `i64`.
:::

<span class="listing-caption"><strong>Listing 3-18</strong>, <code>main.cee</code>: An <code>i64</code> divided by an <code>i32</code>. This does not compile.</span>

<!-- ceesafe: fail N0207 -->
```ceesafe
fn main() {
    let count: i32 = 3;
    let total: i64 = 1000;
    Console.writeLine(total / count);
}
```

```text
main.cee:4:23: error[N0207]: operator '/' requires operands of the same type, found 'i64' and 'i32'
  |
4 |     Console.writeLine(total / count);
  |                       ^^^^^^^^^^^^^
```

Convert the narrower one: `total / (count as i64)` prints `333`.

::: warning Mistake: narrowing with `as`
`as` is not a cast that cuts bits off.
:::

<span class="listing-caption"><strong>Listing 3-19</strong>, <code>main.cee</code>: An <code>i64</code> may not fit an <code>i32</code>. This does not compile.</span>

<!-- ceesafe: fail N0208 -->
```ceesafe
fn main() {
    let total: i64 = 1000;
    let small = total as i32;
    Console.writeLine(small);
}
```

```text
main.cee:3:17: error[N0208]: cannot convert 'i64' to 'i32' with 'as': 'as' only allows conversions that never lose information; use checkedTruncate<i32>(...) for an optional that is null when it does not fit, or truncate<i32>(...) to convert with wrap-around
  |
3 |     let small = total as i32;
  |                 ^^^^^^^^^^^^
```

Choose what should happen to a value that does not fit, and write that function.

::: warning Mistake: indexing with an `i32`
A variable set from a plain literal is an `i32`, and an index is a `usize`.
:::

<span class="listing-caption"><strong>Listing 3-20</strong>, <code>main.cee</code>: <code>index</code> is an <code>i32</code>. This does not compile.</span>

<!-- ceesafe: fail N0205 -->
```ceesafe
fn main() {
    let primes = [2, 3, 5, 7];
    let index = 2;
    Console.writeLine(primes[index]);
}
```

```text
main.cee:4:30: error[N0205]: expected 'usize', found 'i32'
  |
4 |     Console.writeLine(primes[index]);
  |                              ^^^^^
```

Declare it as `let index: usize = 2;`.

::: warning Mistake: writing a tuple
Parentheses only group an expression.
:::

<span class="listing-caption"><strong>Listing 3-21</strong>, <code>main.cee</code>: There is no pair of values. This does not compile.</span>

<!-- ceesafe: fail N0100 -->
```ceesafe
fn main() {
    let pair = (1, 2);
}
```

```text
main.cee:2:18: error[N0100]: expected ')', found ','
  |
2 |     let pair = (1, 2);
  |                  ^
```

::: warning Mistake: declaring a type alias
`type` is a reserved word, but it declares nothing.
:::

<span class="listing-caption"><strong>Listing 3-22</strong>, <code>main.cee</code>: There is no second name for a type. This does not compile.</span>

<!-- ceesafe: fail N0101 -->
```ceesafe
type Count = u32;

fn main() {
    let n: Count = 3;
    Console.writeLine(n);
}
```

```text
main.cee:1:1: error[N0101]: expected 'fn', 'const', 'struct' or 'enum', found 'type'
  |
1 | type Count = u32;
  | ^^^^
```

::: details Coming from Rust
The integer and float types have Rust's names. Three things differ: overflow panics in every build, not only in
debug builds; `as` refuses any conversion that can lose information, where Rust's `as` truncates silently; and
there is no `char`, no tuple and no `type` alias.
:::

::: details Coming from C, C#, Java, JavaScript or TypeScript
Nothing is promoted or widened for you: an `i32` plus an `i64` is an error, not an `i64`. Overflow does not wrap.
`'A'` is a number, as in C, but its type comes from where it is used.
:::

## Summary

- Integers are `i8` to `i64`, `u8` to `u64`, `isize` and `usize`; floats are `f32` and `f64`. An integer literal
  with no other type is an `i32`, a float literal an `f64`.
- Integer overflow, division by zero and an index out of bounds panic.
- `wrappingAdd`, `saturatingAdd` and `checkedAdd` (and `Sub`, `Mul`) say what overflow should do.
- Operands must have the same type (`N0207`); a value must have the type expected (`N0205`).
- `as` converts only without loss (`N0208`); `truncate`, `checkedTruncate`, `saturatingTruncate` and `round` do
  the rest.
- Text is a `StringView` of UTF-8 bytes; a character literal is an integer.
- An array `[T; N]` has a fixed length and `usize` indexes.
- There are no tuples (`N0100`) and no type aliases (`N0101`).

Next: [functions](./ch03-03-functions).
