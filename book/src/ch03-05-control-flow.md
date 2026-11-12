---
title: Control flow
summary: Choosing and repeating - if as a statement and as an expression, while, loop, for over a range and over an array, break and continue, and a first look at match, with the mistakes the compiler catches.
assumes: [ch03-01-variables, ch03-02-data-types, ch03-03-functions]
keywords: [if, else, while, loop, for, range, break, continue, match, condition, bool]
diagnostics: [N0100, N0205, N0256]
specification: ["type-system.md §9", "type-system.md §15.3", "type-system.md §16.4"]
---

# Control flow

**This section teaches** how a program chooses what to run and how it repeats work. It assumes
[Variables](./ch03-01-variables), [Data types](./ch03-02-data-types) and [Functions](./ch03-03-functions).

## `if`

An `if` runs a block when its condition is true. `else` gives the block to run otherwise, and `else if` adds
another condition.

<span class="listing-caption"><strong>Listing 3-33</strong>, <code>main.cee</code>: The first condition that is true chooses the block.</span>

```ceesafe
fn main() {
    let temperature = 14;
    if (temperature > 25) {
        Console.writeLine("warm");
    } else if (temperature > 10) {
        Console.writeLine("mild");
    } else {
        Console.writeLine("cold");
    }
}
```

```text
mild
```

::: tip Rule
A condition is written in parentheses and must be a `bool`. A body is a block in braces, even when it is one
statement.
:::

No other type stands for true or false: a number is not a condition. Write the comparison, `count != 0`.

## `if` as an expression

Where a value is needed, `if` chooses between two values. Then each branch is an expression, and the `else` is
required, because the `if` must have a value either way.

<span class="listing-caption"><strong>Listing 3-34</strong>, <code>main.cee</code>: An <code>if</code> on the right of a <code>let</code>, with expression branches and with block branches.</span>

```ceesafe
fn main() {
    let temperature = 14;
    let coat = if (temperature < 10) "heavy" else "light";
    Console.writeLine(coat);
    let layers = if (temperature < 0) {
        let base = 2;
        base + 1
    } else {
        1
    };
    Console.writeLine(layers);
}
```

```text
light
1
```

A branch of an `if` expression can be a block that ends with its value, written without a semicolon: `base + 1`
above. A function body and the block of an `if` statement have no value.

::: tip Rule
An `if` expression needs its `else`, and both branches must have the same type. Only a branch of an `if` or
`match` expression can be a block that ends with a value.

*Specification: `type-system.md` §15.3.*
:::

## `while` and `loop`

`while` repeats its block as long as its condition is true. It tests the condition before each pass.

<span class="listing-caption"><strong>Listing 3-35</strong>, <code>main.cee</code>: Counting the steps until <code>n</code> reaches 1.</span>

```ceesafe
fn main() {
    var n = 27;
    var steps = 0;
    while (n != 1) {
        n = if (n % 2 == 0) n / 2 else 3 * n + 1;
        steps += 1;
    }
    Console.writeLine(steps);
}
```

```text
111
```

`loop` repeats its block with no condition. It ends when the block runs `break`, or returns from the function.

<span class="listing-caption"><strong>Listing 3-36</strong>, <code>main.cee</code>: A <code>loop</code> that stops at the first power of two above 1000.</span>

```ceesafe
fn main() {
    var power = 1;
    loop {
        power *= 2;
        if (power > 1000) {
            break;
        }
    }
    Console.writeLine(power);
}
```

```text
1024
```

For a loop that has no condition at its top, write `loop`, not `while (true)`. The compiler knows that the code
after a `loop` without `break` is never reached; it assumes that any `while` can finish.

## `for`

`for` runs its block once for each value of a range or each element of a collection.

A range `lo..hi` is the integers from `lo` up to, but not including, `hi`.

<span class="listing-caption"><strong>Listing 3-37</strong>, <code>main.cee</code>: Two ranges; the second loop uses <code>continue</code> and <code>break</code>.</span>

```ceesafe
fn main() {
    var sum = 0;
    for (i in 1..5) {
        sum += i;
    }
    Console.writeLine(sum);
    for (i in 0..10) {
        if (i % 2 == 0) {
            continue;
        }
        if (i > 7) {
            break;
        }
        Console.writeLine(i);
    }
}
```

```text
10
1
3
5
7
```

- `1..5` is 1, 2, 3, 4, so the sum is 10.
- `continue` skips the rest of the block and goes to the next value.
- `break` leaves the loop. In a loop inside another loop, both act on the inner one.
- The loop variable is new for each pass and cannot be assigned.

`break` and `continue` work the same way in `while` and `loop`.

A `for` also walks an array, giving each element in order:

<span class="listing-caption"><strong>Listing 3-38</strong>, <code>main.cee</code>: A loop over the elements of an array, and a loop over its indexes.</span>

```ceesafe
fn main() {
    let scores = [70, 85, 90];
    var total = 0;
    for (score in scores) {
        total += score;
    }
    Console.writeLine(total);
    for (i in 0..scores.length) {
        Console.write(i);
        Console.write(": ");
        Console.writeLine(scores[i]);
    }
}
```

```text
245
0: 70
1: 85
2: 90
```

Use the first form when you need the elements and the second when you need the index too. `scores.length` is a
`usize`, so `i` is a `usize` and can index the array. The same `for` walks the other collections of
[chapter 8](./ch08-00-collections).

::: tip Rule
`lo..hi` stops before `hi`. Both ends have the same integer type, and that is the loop variable's type.

*Specification: `type-system.md` §16.4.*
:::

## A first look at `match`

`match` compares a value with a list of patterns and runs the first arm that fits. It replaces a chain of
`else if` on one value.

<span class="listing-caption"><strong>Listing 3-39</strong>, <code>main.cee</code>: A <code>match</code> as an expression, and a <code>match</code> as a statement.</span>

```ceesafe
fn name(day: i32) -> StringView => match (day) {
    1 => "Monday",
    2 => "Tuesday",
    6 | 7 => "weekend",
    _ => "another day",
};

fn main() {
    Console.writeLine(name(2));
    Console.writeLine(name(7));
    match (3) {
        1 => {
            Console.writeLine("first");
        }
        _ => {
            Console.writeLine("not first");
        }
    }
}
```

```text
Tuesday
weekend
not first
```

- An arm is `pattern => result`. Here the patterns are integer literals.
- `6 | 7` fits either value.
- `_` fits anything. It goes last and takes every value the other arms leave.
- As an expression, each arm gives a value and the arms are separated by commas. As a statement, each arm is a
  block.

A `match` must cover every possible value, and the compiler checks it. [Chapter 6](./ch06-02-match) teaches
`match` fully, with patterns that take a value apart.

## Mistakes

::: warning Mistake: a condition without parentheses
The parentheses are part of `if`, `while`, `for` and `match`.
:::

<span class="listing-caption"><strong>Listing 3-40</strong>, <code>main.cee</code>: The condition has no parentheses. This does not compile.</span>

<!-- ceesafe: fail N0100 -->
```ceesafe
fn main() {
    let n = 3;
    if n > 2 {
        Console.writeLine("big");
    }
}
```

```text
main.cee:3:8: error[N0100]: expected '(', found identifier
  |
3 |     if n > 2 {
  |        ^
```

Write `if (n > 2) {`.

::: warning Mistake: a number as a condition
Only a `bool` is true or false.
:::

<span class="listing-caption"><strong>Listing 3-41</strong>, <code>main.cee</code>: <code>count</code> is an <code>i32</code>, not a <code>bool</code>. This does not compile.</span>

<!-- ceesafe: fail N0205 -->
```ceesafe
fn main() {
    let count = 3;
    if (count) {
        Console.writeLine("some");
    }
}
```

```text
main.cee:3:9: error[N0205]: expected 'bool', found 'i32'
  |
3 |     if (count) {
  |         ^^^^^
```

Write `if (count != 0) {`.

::: warning Mistake: an `if` expression without `else`
An `if` that gives a value needs a value for both cases.
:::

<span class="listing-caption"><strong>Listing 3-42</strong>, <code>main.cee</code>: <code>size</code> would have no value when <code>n</code> is small. This does not compile.</span>

<!-- ceesafe: fail N0100 -->
```ceesafe
fn main() {
    let n = 3;
    let size = if (n > 2) "big";
    Console.writeLine(size);
}
```

```text
main.cee:3:32: error[N0100]: expected 'else', found ';'
  |
3 |     let size = if (n > 2) "big";
  |                                ^
```

Add the other branch: `if (n > 2) "big" else "small"`.

::: warning Mistake: an inclusive range in a `for`
A range in a `for` is always half-open. There is no `..=` there.
:::

<span class="listing-caption"><strong>Listing 3-43</strong>, <code>main.cee</code>: A range that tries to include 3. This does not compile.</span>

<!-- ceesafe: fail N0100 -->
```ceesafe
fn main() {
    for (i in 1..=3) {
        Console.writeLine(i);
    }
}
```

```text
main.cee:2:16: error[N0100]: expected '..', found '..=': ranges in expressions are half-open, written '0..10' (ADR 0085 §2.4)
  |
2 |     for (i in 1..=3) {
  |                ^^^
```

Write `1..4`.

::: warning Mistake: a `match` that misses a value
The arms together must cover every value of the type.
:::

<span class="listing-caption"><strong>Listing 3-44</strong>, <code>main.cee</code>: Two arms for an <code>i32</code>. This does not compile.</span>

<!-- ceesafe: fail N0256 -->
```ceesafe
fn main() {
    let day = 3;
    match (day) {
        1 => {
            Console.writeLine("Monday");
        }
        2 => {
            Console.writeLine("Tuesday");
        }
    }
}
```

```text
main.cee:3:12: error[N0256]: this match does not cover -2147483648; add the missing arms or a '_' arm
  |
3 |     match (day) {
  |            ^^^
```

The message names one value that no arm fits. Add a `_` arm for the rest, with an empty block `{}` if there is
nothing to do.

::: details Coming from Rust
The condition is in parentheses. `if` and `match` are expressions, but `loop` is not: `break` takes no value.
`0..n` is the only range in a `for`; `..=` exists only in patterns.
:::

::: details Coming from C, C#, Java or JavaScript
There is no three-part `for (init; test; step)`: use a range or `while`. There is no `?:` operator: an `if`
expression does that work. `match` has no fall-through from one arm to the next, and needs no `break`.
:::

::: details Coming from Python
`for (i in 0..n)` is `for i in range(n)`. An empty array, zero and empty text are not false: conditions are `bool`
only.
:::

## Summary

- `if (condition) { … } else { … }`: the condition is a `bool` in parentheses (`N0205` otherwise), and the bodies
  are blocks.
- `if (c) a else b` is an expression; it needs its `else`, and a block branch ends with its value without `;`.
- `while (condition) { … }` tests before each pass; `loop { … }` repeats until `break` or `return`.
- `for (i in lo..hi)` runs from `lo` up to but not including `hi`; `for (x in array)` gives each element.
- `break` leaves the innermost loop, and `continue` starts its next pass.
- `match (value) { pattern => …, _ => … }` runs the first arm that fits, and must cover every value (`N0256`).

Next: [chapter 4](./ch04-00-ownership), on ownership.
