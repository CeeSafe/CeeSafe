---
title: Panics
summary: A panic stops the program at a bug - an index out of range, integer overflow, division by zero, an unwrap of nothing. What it prints, exit status 134, that nothing catches it, and when to call panic or assert.
assumes: [ch03-05-control-flow, ch06-03-optionals]
keywords: [panic, assert, "index out of bounds", "integer overflow", "division by zero", "unwrapped null", "exit status", "134", checkedMul]
specification: ["type-system.md §6", "type-system.md §8", "type-system.md §14.5", "type-system.md §22.1"]
---

# Panics

**This section teaches** what a panic is, which operations panic, what the program prints when one happens, and
when to cause one yourself. It assumes [control flow](./ch03-05-control-flow) and, for one listing,
[optionals](./ch06-03-optionals).

A **panic** is how a CeeSafe program stops when it finds a bug in itself. The program prints one line that says
what went wrong and where, and the process ends. The rest of the program does not run.

## An index out of range

<span class="listing-caption"><strong>Listing 9-1</strong>, <code>main.cee</code>: The loop runs one step too far. The fourth read panics.</span>

<!-- ceesafe: panics -->
```ceesafe
fn main() {
    let values = [10, 20, 30];
    var i: usize = 0;
    while (i <= values.length) {
        Console.writeLine(values[i]);
        i += 1;
    }
    Console.writeLine("done");
}
```

```text
10
20
30
```

The array has three elements, and `<=` lets `i` reach 3. The program prints the three values, then this line, and
stops:

```text
ceesafe panic: index out of bounds at main.cee:5:34
```

- The line goes to standard error, not standard output.
- It gives the reason, then the file, line and column of the operation that failed: line 5, column 34 is
  `values[i]`.
- `done` is never printed. Nothing after a panic runs.
- The process ends with **exit status 134**. A shell or a script that started the program sees that it failed.

The same happens in the interpreter (`ceesafec run --interp`) and in a built executable.

::: tip Rule
A panic prints `ceesafe panic: <reason> at <file>:<line>:<column>` on standard error and ends the process with
exit status 134.
:::

Reading `values[3]` cannot give a sensible answer. In some languages it would read whatever memory lies after the
array. CeeSafe checks every index, and a wrong one is a bug in the program, so the program stops at the bug.

## What panics

These operations panic. Each reason in the table is the text the program prints.

| Operation | Reason printed |
|---|---|
| `a[i]` with `i` at or past the length | `index out of bounds` |
| `+`, `-`, `*` whose result does not fit the type | `integer overflow` |
| `/` or `%` by zero | `division by zero` |
| `o!` on an optional that is `null` | `unwrapped null` |
| `r!` on a `Result` that is an `Err` ([section 9.2](./ch09-02-result)) | `unwrapped an Err value` |
| `panic("message")` | the message |
| `assert(condition, "message")` when the condition is false | the message |

Integer arithmetic does not wrap around silently:

<span class="listing-caption"><strong>Listing 9-2</strong>, <code>main.cee</code>: 400 does not fit in a <code>u8</code>, so the second call panics.</span>

<!-- ceesafe: panics -->
```ceesafe
fn double(n: u8) -> u8 => n * 2;

fn main() {
    Console.writeLine(double(100));
    Console.writeLine(double(200));
}
```

```text
200
```

Then, on standard error:

```text
ceesafe panic: integer overflow at main.cee:1:27
```

The location is the `*` inside `double`, not the call in `main`: a panic names the operation that failed.

Where overflow is a case your program expects, ask for it as a value. `n.checkedMul(2)` gives an optional that is
`null` when the result does not fit; `checkedAdd` and `checkedSub` do the same for `+` and `-`.

The `!` operator takes the value out of an optional, and panics when there is none:

<span class="listing-caption"><strong>Listing 9-3</strong>, <code>main.cee</code>: The empty string has no first byte, so the second <code>!</code> panics.</span>

<!-- ceesafe: panics -->
```ceesafe
fn first(text: StringView) -> u8? {
    if (text.length == 0) {
        return null;
    }
    return text[0];
}

fn main() {
    Console.writeLine(first("ada")!);
    Console.writeLine(first("")!);
}
```

```text
97
```

Then, on standard error:

```text
ceesafe panic: unwrapped null at main.cee:10:23
```

## A panic ends the process

A panic is not an exception. No statement catches it, and no caller sees it. The process ends at once:

- no later statement runs;
- cleanup code does not run either. Deferred statements and `drop` blocks, which other chapters cover, are skipped.

::: tip Rule
Nothing catches a panic, and nothing runs after it: no later statement and no cleanup.

*Specification: `type-system.md` §14.5.*
:::

This is deliberate. A panic means the program is in a state its author did not plan for. Running more of it, on
data that is already wrong, could write a bad file or send a bad answer. Stopping is the safe thing to do.

So a panic is never the way to report something that a correct program must expect, such as bad input. For that,
return a `Result`, which [section 9.2](./ch09-02-result) teaches.

## Calling `panic` yourself

`panic(message)` stops the program with your own message. Use it where your code finds that something which must
be true is false, and the cause can only be a bug.

<span class="listing-caption"><strong>Listing 9-4</strong>, <code>main.cee</code>: A month outside 1 to 12 is a bug in the caller, so <code>daysIn</code> panics.</span>

<!-- ceesafe: panics -->
```ceesafe
fn daysIn(month: i32) -> i32 {
    if (month < 1 || month > 12) {
        panic("daysIn: month out of range");
    }
    if (month == 2) {
        return 28;
    }
    if (month == 4 || month == 6 || month == 9 || month == 11) {
        return 30;
    }
    return 31;
}

fn main() {
    Console.writeLine(daysIn(9));
    Console.writeLine(daysIn(13));
}
```

```text
30
```

Then, on standard error:

```text
ceesafe panic: daysIn: month out of range at main.cee:3:9
```

The location is the `panic` call. Put the function's name in the message, so the line says whose rule was broken.

A call of `panic` never returns, so the compiler does not ask for a `return` after it.

`assert(condition, message)` is the short form of the same test: it panics with the message when the condition is
false. `assert(condition)` does the same with the message `assertion failed`.

<span class="listing-caption"><strong>Listing 9-5</strong>, <code>main.cee</code>: <code>assert</code> states what the function needs. The second call breaks it.</span>

<!-- ceesafe: panics -->
```ceesafe
fn average(total: i32, count: i32) -> i32 {
    assert(count > 0, "average of nothing");
    return total / count;
}

fn main() {
    Console.writeLine(average(10, 4));
    Console.writeLine(average(10, 0));
}
```

```text
2
```

Then, on standard error:

```text
ceesafe panic: average of nothing at main.cee:2:5
```

Without the `assert`, the division would panic with `division by zero`. The `assert` panics one line earlier with
a message that says what the caller did wrong.

::: tip Rule
Call `panic` or `assert` for a condition that only a bug can cause. The message is for the programmer who will
fix it.

*Specification: `type-system.md` §8.*
:::

## Mistakes

::: warning Mistake: `!` on something that can be missing
`!` says "there is a value here, and if not, that is a bug". Input from outside the program can always be missing.
:::

<span class="listing-caption"><strong>Listing 9-6</strong>, <code>main.cee</code>: Run with no arguments, the program panics. The user did nothing wrong.</span>

<!-- ceesafe: panics -->
```ceesafe
fn main() {
    let name = Process.argument(1)!;
    Console.write("Hello, ");
    Console.writeLine(name);
}
```

`Process.argument(1)` is the first argument on the command line, as an optional. With no argument it is `null`,
and the program prints only:

```text
ceesafe panic: unwrapped null at main.cee:2:16
```

A user who forgot an argument gets a message about the program's source code. Give the optional a fallback with
`??`, or `match` on it and print a message the user can act on:

<span class="listing-caption"><strong>Listing 9-7</strong>, <code>main.cee</code>: A missing argument has a fallback.</span>

```ceesafe
fn main() {
    let name = Process.argument(1) ?? "world";
    Console.write("Hello, ");
    Console.writeLine(name);
}
```

```text
Hello, world
```

::: warning Mistake: expecting to recover from a panic
There is no way to run a piece of code and continue if it panics. If a failure is one your program must survive,
it has to be a value from the start: an optional or a `Result`.
:::

Listing 9-2 panics for 200. When a number too large is something the program expects, the function returns an
optional and the caller handles `null`:

<span class="listing-caption"><strong>Listing 9-8</strong>, <code>main.cee</code>: Overflow as a value. <code>checkedMul</code> gives <code>null</code> where <code>*</code> would panic.</span>

```ceesafe
fn double(n: u8) -> u8? => n.checkedMul(2);

fn main() {
    match (double(200)) {
        Some(d) => { Console.writeLine(d); }
        null => { Console.writeLine("too large for a u8"); }
    }
}
```

```text
too large for a u8
```

::: details Coming from Rust
A panic is Rust's `panic!` compiled with `panic = "abort"`: no unwinding, no `catch_unwind`, and destructors do
not run. `x!` is `x.unwrap()`. Integer overflow panics in every build, not only in debug builds.
:::

::: details Coming from C#, Java, Python or TypeScript
A panic is not an exception. There is no `try` and `catch`, and no `finally`. What those languages report with
exceptions is split in two here: bugs panic, and expected failures are `Result` values.
:::

::: details Coming from C
An index out of range, signed overflow and division by zero are not undefined behaviour. Each one stops the
program at a known place with a message. Exit status 134 is what a shell reports after `abort()`.
:::

## Summary

- A panic prints `ceesafe panic: <reason> at <file>:<line>:<column>` on standard error and ends the process with
  exit status 134.
- An index out of range, integer overflow, division by zero, and `!` on `null` or on an `Err` all panic.
- Nothing catches a panic, and no cleanup runs after it.
- Call `panic(message)` or `assert(condition, message)` where only a bug can be the cause.
- Do not use `!` on input from outside the program. Use `??`, `match`, or a `checked` method to get a value
  instead of a panic.

Next: [`Result`, `?` and `!`](./ch09-02-result), for failures that are not bugs.
