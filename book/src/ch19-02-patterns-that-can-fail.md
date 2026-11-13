---
title: Patterns that can fail
summary: Which patterns fit every value and which do not, how the compiler decides that the arms of a match cover every value of each type, why a guarded arm does not count, and how it finds arms, alternatives and ranges that can never match.
assumes: [ch19-01-where-patterns-go, ch06-02-match, ch09-02-result]
keywords: [pattern, exhaustive, refutable, irrefutable, unreachable, guard, catch-all, N0256, N0257, N0357]
diagnostics: [N0256, N0257, N0357, N0404]
specification: ["type-system.md §15.2", "type-system.md §15.4"]
---

# Patterns that can fail

**This section teaches** the difference between a pattern that some value does not fit and a pattern that every
value fits, and the two checks the compiler builds on it: that a `match` covers every value, and that every arm can
run. It assumes [Where patterns go](./ch19-01-where-patterns-go), [`match`](./ch06-02-match) and the `Ok` and
`Err` of [`Result`](./ch09-02-result).

## Two kinds of pattern

Hold a pattern against a type and ask: is there a value of this type that does not fit?

- For `Some(n)` on an `i32?` there is: `null`. For `0` on an `i32` there are many. These patterns **can fail**.
- For `_` there is none, and for a lone name there is none. These patterns **cannot fail**. A pattern built from
  parts can be of this kind too: `Light.Red | Light.Green` cannot fail on an enum with those two variants.

Which kind a pattern is depends on the type: `0..=255` can fail on an `i32` and cannot on a `u8`.

Each place for a pattern treats the two kinds in its own way:

| Place | A pattern that can fail | A pattern that cannot fail |
|---|---|---|
| after `is` | the usual case: the test is the question | accepted; the test is always `true`, and `_` or a lone name gets a warning |
| `let … else` | the usual case: the `else` block takes the rest | accepted; the `else` block never runs, with the same warning |
| one arm of a `match` | the usual case | a catch-all: it must be the last arm |
| all arms of a `match` together | refused: the `match` must cover every value | required |
| a plain `let`, `for`, parameters | cannot be written | only its two plainest forms, a name and `_` |

<span class="listing-caption"><strong>Listing 19-8</strong>, <code>main.cee</code>: Three patterns that cannot fail. <code>is</code> accepts them, and the compiler warns about the two that test nothing.</span>

<!-- ceesafe: warns N0404 -->
```ceesafe
enum Light {
    Red,
    Green,
}

fn main() {
    let n = 5;
    if (n is _) {
        Console.writeLine("every i32 fits _");
    }
    if (n is m) {
        Console.writeLine(m);
    }
    let light = Light.Green;
    if (light is Light.Red | Light.Green) {
        Console.writeLine("one of the two it must be");
    }
}
```

```text
main.cee:8:14: warning[N0404]: this pattern tests nothing: it matches every value, so the test is always true; drop the test, and name the value with 'let' (type-system.md §15.4)
  |
8 |     if (n is _) {
  |              ^

main.cee:11:14: warning[N0404]: this pattern tests nothing: it matches every value, so the test is always true; drop the test, and name the value with 'let' (type-system.md §15.4)
   |
11 |     if (n is m) {
   |              ^
```

The program runs, and prints all three lines. An `is` that is always true is legal, and useless: for `_` and for a
lone name the compiler says so. The third pattern takes the value apart and happens to hold every `Light`, and gets
no warning. The check that matters is the one on `match`.

## A `match` covers every value

::: tip Rule
The arms of a `match` together must take every value of the matched type. If some value fits no arm, the `match`
does not compile, and the diagnostic (`N0256`) names such a value.

*Specification: `type-system.md` §15.2.*
:::

The compiler does not look at arms one by one. It looks at whole patterns, to any depth, and searches for a value
that gets through all of them.

<span class="listing-caption"><strong>Listing 19-9</strong>, <code>main.cee</code>: <code>Ok</code> has an arm and <code>Err</code> has an arm, and one value still gets through. This does not compile.</span>

<!-- ceesafe: fail N0256 -->
```ceesafe
enum Colour {
    Red,
    Green,
}

fn name(r: Result<Colour, i32>) -> StringView => match (r) {
    Ok(Colour.Red) => "red",
    Err(_) => "error",
};

fn main() {
    Console.writeLine(name(Ok(Colour.Red)));
}
```

```text
main.cee:6:57: error[N0256]: this match does not cover Ok(Colour.Green); add the missing arms or a '_' arm
  |
6 | fn name(r: Result<Colour, i32>) -> StringView => match (r) {
  |                                                         ^
```

The message names one value. When several are missing, add an arm for the one it names and compile again: it then
names the next.

What it takes to cover a type:

| Type | The arms cover it when they take |
|---|---|
| an enum | every variant, and for a variant with data, every value of its data |
| `bool` | `true` and `false` |
| an optional `T?` | `null`, and `Some(p)` for every value of `T` |
| `Result<T, E>` | `Ok(p)` for every value of `T`, and `Err(q)` for every value of `E` |
| an integer | every value of the type, through literals and ranges |
| a `StringView` | never by text literals alone: there is always another text |
| a slice | every length: `[]` and `[first, ..]` do, for example |
| an array `[T; N]` | its one length, `N` |

A catch-all (`_` or a name) covers any of them, at the top of an arm or in any position inside one.

For integers the compiler does real arithmetic on the ranges.

<span class="listing-caption"><strong>Listing 19-10</strong>, <code>main.cee</code>: Two ranges hold every <code>u8</code>, so the <code>match</code> needs no catch-all.</span>

```ceesafe
fn kind(b: u8) -> StringView => match (b) {
    0x00..=0x7F => "ascii",
    0x80..=0xFF => "high",
};

fn main() {
    Console.writeLine(kind(65));
    Console.writeLine(kind(200));
}
```

```text
ascii
high
```

Change the second range to `0x80..=0xFE` and the diagnostic is `this match does not cover 255`. For a missing
integer it names the lowest one, and writes a printable byte of a `u8` as a character, such as `'{'`.

## Guards do not count

A guard is an ordinary condition, and the compiler does not reason about when it is true. So an arm with a guard
covers nothing.

<span class="listing-caption"><strong>Listing 19-11</strong>, <code>main.cee</code>: To a reader the three arms take every number. To the compiler only the first counts. This does not compile.</span>

<!-- ceesafe: fail N0256 -->
```ceesafe
fn sign(n: i32) -> StringView => match (n) {
    0 => "zero",
    x if (x > 0) => "positive",
    x if (x < 0) => "negative",
};

fn main() {
    Console.writeLine(sign(3));
}
```

```text
main.cee:1:41: error[N0256]: this match does not cover -2147483648 without its guards: arms with a guard do not count; add '_' or an arm without a guard
  |
1 | fn sign(n: i32) -> StringView => match (n) {
  |                                         ^
```

<span class="listing-caption"><strong>Listing 19-12</strong>, <code>main.cee</code>: The last arm loses its guard and becomes the catch-all.</span>

```ceesafe
fn sign(n: i32) -> StringView => match (n) {
    0 => "zero",
    x if (x > 0) => "positive",
    _ => "negative",
};

fn main() {
    Console.writeLine(sign(3));
    Console.writeLine(sign(-3));
}
```

```text
positive
negative
```

::: tip Rule
An arm with a guard never counts toward covering the type. The last arm of a `match` that uses guards is one
without a guard.

*Specification: `type-system.md` §15.2.*
:::

## Every arm can run

The second check is the first one turned round. An arm is **unreachable** when every value it takes is taken by
the arms above it. Such an arm is dead code, and usually a mistake in the arms above, so it is an error (`N0257`),
not a warning. The check goes inside alternatives.

<span class="listing-caption"><strong>Listing 19-13</strong>, <code>main.cee</code>: The first arm takes 2, so the second alternative of the next arm never matches. This does not compile.</span>

<!-- ceesafe: fail N0257 -->
```ceesafe
fn f(n: u8) -> StringView => match (n) {
    1 | 2 => "low",
    3 | 2 => "three",
    _ => "big",
};

fn main() {
    Console.writeLine(f(5));
}
```

```text
main.cee:3:9: error[N0257]: this alternative can never match: earlier arms or alternatives match every value it does
  |
3 |     3 | 2 => "three",
  |         ^
```

A guarded arm is checked for this too: an arm with a guard below a `_` is refused. And a range that holds no value
is refused where it stands, with a code of its own.

<span class="listing-caption"><strong>Listing 19-14</strong>, <code>main.cee</code>: The bounds are the wrong way round. This does not compile.</span>

<!-- ceesafe: fail N0357 -->
```ceesafe
fn f(n: u8) -> StringView => match (n) {
    'z'..='a' => "letter",
    _ => "other",
};

fn main() {
    Console.writeLine(f(5));
}
```

```text
main.cee:2:5: error[N0357]: this range matches no value: 'lo..=hi' needs lo <= hi
  |
2 |     'z'..='a' => "letter",
  |     ^^^^^^^^^
```

## Mistakes

::: warning Mistake: a misspelt constant
A bare name in a pattern is a constant if a `const` of that name is in scope. If not, it is a new name, which
cannot fail and takes everything. A spelling mistake turns the first into the second, and the diagnostic appears
on the arm below.
:::

<span class="listing-caption"><strong>Listing 19-15</strong>, <code>main.cee</code>: <code>LIMT</code> is no constant, so it binds every value. This does not compile.</span>

<!-- ceesafe: fail N0257 -->
```ceesafe
const LIMIT: u8 = 10;

fn f(n: u8) -> StringView => match (n) {
    LIMT => "at the limit",
    _ => "other",
};

fn main() {
    Console.writeLine(f(5));
}
```

```text
main.cee:5:5: error[N0257]: this arm can never match: earlier arms match every value it does
  |
5 |     _ => "other",
  |     ^
  = note: the earlier arm 'LIMT' is a new name, which matches every value: no constant of that name is in scope (a misspelt constant?) (4:5)
```

The note names the bare name that took everything. Correct its spelling.

::: warning Mistake: a `match` on text without a catch-all
No list of text literals holds every text. The value the diagnostic names is `_`: it has no particular text to
show.
:::

<span class="listing-caption"><strong>Listing 19-16</strong>, <code>main.cee</code>: Two texts are not all texts. This does not compile.</span>

<!-- ceesafe: fail N0256 -->
```ceesafe
fn f(s: StringView) -> i32 => match (s) {
    "a" => 1,
    "b" => 2,
};

fn main() {
    Console.writeLine(f("a"));
}
```

```text
main.cee:1:38: error[N0256]: this match does not cover _; add the missing arms or a '_' arm
  |
1 | fn f(s: StringView) -> i32 => match (s) {
  |                                      ^
```

Add `_ => 0,` as the last arm.

::: details Coming from Rust
Rust's words are *refutable* (can fail) and *irrefutable* (cannot). Rust needs the words because `let`, `for` and
parameters take irrefutable patterns and `if let` takes refutable ones. In CeeSafe the only irrefutable patterns
outside `match`, `is` and `let … else` are a name and `_`, so the question arises for a `match` only. One
difference in the checks: an unreachable arm is an error here, where Rust warns. An `is` or a `let … else` whose
pattern is `_` or a name gets a warning (N0404), as Rust's irrefutable `if let` does.
:::

::: details Coming from C#, Java or TypeScript
A `switch` that misses a case compiles in those languages, with a warning at most, and fails or does nothing when
it runs. A `match` that misses a value does not compile. There is no `default` that the compiler adds for you, and
none that throws.
:::

## Summary

- A pattern can fail when some value of the type does not fit it. `_`, a name, and alternatives that hold every
  value cannot fail.
- `is` takes either kind, and warns about a `_` or a lone name (`N0404`). The arms of a `match` together must not fail (`N0256`, which names a value that
  fits no arm).
- Enums are covered by their variants, `bool` by `true` and `false`, optionals by `null` and `Some`, a `Result` by
  `Ok` and `Err`, integers by literals and ranges that hold every value, slices by their lengths. Text needs a
  catch-all.
- An arm with a guard covers nothing.
- An arm or an alternative that the ones before it make unreachable is an error (`N0257`), and so is a range that
  holds no value (`N0357`).

Next: [The syntax, whole](./ch19-03-the-syntax-whole), every form of pattern one by one.
