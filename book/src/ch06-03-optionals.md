---
title: Optionals
summary: A value that may be absent has the type T? and is Some(value) or null - testing it, the fallback ??, unwrapping with !, matching, binding with is Some(x) in a condition, and passing null on with ?.
assumes: [ch06-01-defining-an-enum, ch06-02-match]
keywords: [optional, "T?", null, Some, isSome, isNull, "??", "!", "?", is, "if let"]
diagnostics: [N0100, N0207, N0260]
specification: ["type-system.md §22.1", "type-system.md §15.4"]
---

# Optionals

**This section teaches** how CeeSafe says "there may be no value here", and the ways to get the value out safely.
It assumes [Defining an enum](./ch06-01-defining-an-enum) and [`match`](./ch06-02-match).

In CeeSafe an `i32` is always a number and a `Point` is always a point. No ordinary value can be null. When a
value may be absent, its type says so: `T?`, an **optional** `T`. The `?` goes after the type.

An optional is a built-in enum with two variants: `Some(value)`, which carries a `T`, and `null`, which carries
nothing. Both are written without a type name in front.

## Returning and matching an optional

<span class="listing-caption"><strong>Listing 6-17</strong>, <code>main.cee</code>: A division that has no answer when the divisor is zero.</span>

```ceesafe
fn divide(a: i32, b: i32) -> i32? {
    if (b == 0) {
        return null;
    }
    return Some(a / b);
}

fn show(result: i32?) {
    match (result) {
        Some(n) => { Console.writeLine(n); }
        null => { Console.writeLine("no answer"); }
    }
}

fn main() {
    show(divide(12, 4));
    show(divide(1, 0));
}
```

```text
3
no answer
```

- `-> i32?` says that `divide` returns an `i32` or nothing.
- `return null;` is the absent case, and `return Some(a / b);` the present one.
- `match` works as on any enum. The patterns are `Some(n)`, which binds the value, and `null`. The two together
  are exhaustive.

::: tip Rule
A `T?` is not a `T`. Before you use the value, you say what happens when it is absent: with `match`, `??`, `!`,
`is Some(x)` or `?`.

*Specification: `type-system.md` §22.1.*
:::

## Making and testing an optional

<span class="listing-caption"><strong>Listing 6-18</strong>, <code>main.cee</code>: An optional field, a plain value that becomes an optional, and the tests for absence.</span>

```ceesafe
struct Reading {
    celsius: i32;
    humidity: u32?;
}

fn main() {
    var reading = Reading { celsius: 21, humidity: null };
    Console.writeLine(reading.humidity.isNull());
    reading.humidity = 40;
    Console.writeLine(reading.humidity.isSome());
    Console.writeLine(reading.humidity == null);

    let limit: i32? = null;
    Console.writeLine(limit != null);
}
```

```text
true
true
false
false
```

- Where a `T?` is expected, a plain `T` converts by itself: `reading.humidity = 40` stores `Some(40)`. You may also
  write `Some(40)`.
- `null` needs a place that says which optional it is: a field, a parameter, a return type, or a type on the
  `let`, as in `let limit: i32? = null;`.
- `o.isSome()` and `o.isNull()` give a `bool`. So do `o != null` and `o == null`.

## A fallback, or stopping

Two operators turn a `T?` into a `T` in one step.

<span class="listing-caption"><strong>Listing 6-19</strong>, <code>main.cee</code>: <code>??</code> gives a fallback. <code>!</code> takes the value and stops the program if there is none.</span>

```ceesafe
fn divide(a: i32, b: i32) -> i32? {
    if (b == 0) {
        return null;
    }
    return Some(a / b);
}

fn main() {
    Console.writeLine(divide(12, 4) ?? -1);
    Console.writeLine(divide(12, 0) ?? -1);
    Console.writeLine(divide(1, 0) ?? divide(8, 0) ?? divide(8, 2) ?? 0);

    let sure = divide(100, 5)!;
    Console.writeLine(sure + 1);
}
```

```text
3
-1
4
21
```

- `o ?? fallback` is the value of `o`, or `fallback` when `o` is `null`. The fallback is evaluated only when it is
  needed. In a chain, `a ?? b ?? c`, the result is the first value that is present.
- `o!` is the value of `o`. If `o` is `null`, the program stops with the message `unwrapped null` and the place.
  This is called **unwrapping**. Use it when `null` would mean the program itself is wrong.

## Binding in a condition

Often the code wants to do something only when a value is present, and nothing otherwise. `value is pattern` tests
a value against a pattern and gives a `bool`. In the condition of an `if` or a `while`, the names the pattern binds
can be used in the rest of the condition after `&&`, and in the body.

<span class="listing-caption"><strong>Listing 6-20</strong>, <code>main.cee</code>: <code>is Some(n)</code> tests an optional and names its value for the body.</span>

```ceesafe
fn divide(a: i32, b: i32) -> i32? {
    if (b == 0) {
        return null;
    }
    return Some(a / b);
}

fn main() {
    if (divide(12, 4) is Some(n)) {
        Console.writeLine(n);
    } else {
        Console.writeLine("no answer");
    }

    if (divide(90, 3) is Some(n) && n > 20) {
        Console.writeLine("more than twenty");
    }

    var left: i32? = 3;
    while (left is Some(n)) {
        Console.writeLine(n);
        left = if (n > 1) Some(n - 1) else null;
    }
}
```

```text
3
more than twenty
3
2
1
```

The name exists only where the test is known to be true: `n` is not in scope in the `else` block or after the
`if`.

## Passing `null` on

A function that returns an optional often calls another that does, and has nothing to add when that one returns
`null`. A `?` after the call says: if this is `null`, return `null` from this function now; otherwise give me the
value.

<span class="listing-caption"><strong>Listing 6-21</strong>, <code>main.cee</code>: <code>?</code> returns early with <code>null</code>.</span>

```ceesafe
fn divide(a: i32, b: i32) -> i32? {
    if (b == 0) {
        return null;
    }
    return Some(a / b);
}

fn average(total: i32, count: i32, scale: i32) -> i32? {
    let mean = divide(total, count)?;
    return divide(mean, scale);
}

fn main() {
    Console.writeLine(average(120, 4, 3) ?? -1);
    Console.writeLine(average(120, 0, 3) ?? -1);
}
```

```text
10
-1
```

`mean` is an `i32`, not an `i32?`. The `?` works only in a function that itself returns an optional. Chapter 9
shows the same `?` on `Result`, the enum that carries an error instead of `null`.

## Mistakes

::: warning Mistake: using an optional as if it were the value
Arithmetic, printing and assignment to a `T` all refuse a `T?`.
:::

<span class="listing-caption"><strong>Listing 6-22</strong>, <code>main.cee</code>: <code>half</code> is an <code>i32?</code>, and <code>+</code> wants an <code>i32</code>. This does not compile.</span>

<!-- ceesafe: fail N0207 -->
```ceesafe
fn divide(a: i32, b: i32) -> i32? {
    if (b == 0) {
        return null;
    }
    return Some(a / b);
}

fn main() {
    let half = divide(10, 2);
    Console.writeLine(half + 1);
}
```

```text
main.cee:10:23: error[N0207]: operator '+' requires operands of the same type, found 'i32?' and 'i32'
   |
10 |     Console.writeLine(half + 1);
   |                       ^^^^^^^^
```

Decide the absent case first: `(half ?? 0) + 1`, or a `match`, or `if (half is Some(h))`.

::: warning Mistake: `null` for a type that is not optional
There is no null `i32`, null struct or null `String`.
:::

<span class="listing-caption"><strong>Listing 6-23</strong>, <code>main.cee</code>: <code>count</code> is an <code>i32</code>. This does not compile.</span>

<!-- ceesafe: fail N0260 -->
```ceesafe
fn main() {
    let count: i32 = null;
    Console.writeLine(count);
}
```

```text
main.cee:2:22: error[N0260]: 'null' is an absent optional, but 'i32' is expected here
  |
2 |     let count: i32 = null;
  |                      ^^^^
```

If the value may be absent, give it the type `i32?`. If it may not, give it a real starting value.

::: warning Mistake: unwrapping what may be absent
`!` is a claim that the value is there. When the claim is false the program stops.
:::

<span class="listing-caption"><strong>Listing 6-24</strong>, <code>main.cee</code>: The second <code>!</code> meets <code>null</code>. The program compiles, and stops when it runs.</span>

<!-- ceesafe: panics -->
```ceesafe
fn divide(a: i32, b: i32) -> i32? {
    if (b == 0) {
        return null;
    }
    return Some(a / b);
}

fn main() {
    Console.writeLine(divide(6, 3)!);
    Console.writeLine(divide(6, 0)!);
}
```

```text
2
```

After the `2`, the program prints `ceesafe panic: unwrapped null at main.cee:10:23` on standard error and exits
with a failure status. When absence is possible, use `??`, `match` or `is Some(x)` instead.

::: warning Mistake: `if let`
CeeSafe has no `if let`. An `if` takes a condition in parentheses.
:::

<span class="listing-caption"><strong>Listing 6-25</strong>, <code>main.cee</code>: Rust's <code>if let</code>. This does not compile.</span>

<!-- ceesafe: fail N0100 -->
```ceesafe
fn main() {
    let found: i32? = 5;
    if let Some(n) = found {
        Console.writeLine(n);
    }
}
```

```text
main.cee:3:8: error[N0100]: expected '(', found 'let'
  |
3 |     if let Some(n) = found {
  |        ^^^
```

Write `if (found is Some(n)) { … }`: the value first, then `is`, then the pattern.

::: details Coming from Rust
`T?` is `Option<T>`, `null` is `None`, and `Some` is `Some`. `o ?? d` is `o.unwrap_or_else(|| d)`, `o!` is
`o.unwrap()`, and `o?` is the same. `if (o is Some(x))` and `while (o is Some(x))` replace `if let` and
`while let`. A `T` converts to `T?` by itself where one is expected; Rust needs `Some(…)`.
:::

::: details Coming from C#, TypeScript, Java or Go
`null` exists only inside a `T?`. There is no null reference to meet by surprise, because a type without `?`
cannot hold one. `??` is the operator you know. `!` after a value is not TypeScript's unchecked assertion: it tests
at run time and stops the program on `null`. An optional is not a condition: `if (o)` does not compile; write
`if (o.isSome())` or `if (o is Some(x))`.
:::

## Summary

- `T?` is a `T` or nothing: `Some(value)` or `null`. A plain `T` converts to `T?` where one is expected.
- A `T?` cannot be used as a `T` (`N0207`), and `null` cannot be given to a `T` (`N0260`).
- `o.isSome()`, `o.isNull()`, `o == null` and `o != null` test it.
- `o ?? fallback` gives the value or the fallback. `o!` gives the value or stops the program.
- `match (o) { Some(x) => …, null => … }` handles both cases.
- `if (o is Some(x)) { … }` and `while (o is Some(x)) { … }` bind the value for the body. There is no `if let`.
- `o?` returns `null` from the enclosing function when `o` is `null`.

Next: chapter 7, on packages and files. Chapter 9 returns to these ideas with `Result`, for operations that can
fail with a reason.
