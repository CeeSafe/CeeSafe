---
title: match
summary: Choosing code by the shape of a value - match on an enum, binding a variant's data, the rule that every case is covered, the catch-all, match as an expression, and patterns for numbers, ranges and text.
assumes: [ch06-01-defining-an-enum, ch03-05-control-flow]
keywords: [match, pattern, arm, exhaustive, "_", binding, guard, range, is]
diagnostics: [N0100, N0256, N0257]
specification: ["type-system.md §15.2", "type-system.md §15.3", "type-system.md §15.4"]
---

# `match`

**This section teaches** `match`: how it picks code by which variant an enum value is, how it names a variant's
data, and the compiler's check that no case is left out. It assumes [Defining an enum](./ch06-01-defining-an-enum)
and `if` from [Control flow](./ch03-05-control-flow).

## Matching on an enum

A `match` takes a value in parentheses and a list of **arms**. Each arm is a **pattern**, then `=>`, then a block.
The arms are tried from the top, and the block of the first pattern that fits the value runs. No other arm runs.

<span class="listing-caption"><strong>Listing 6-9</strong>, <code>main.cee</code>: One arm for each variant. The names in a pattern receive the variant's data.</span>

```ceesafe
enum Command {
    Move(i32, i32),
    Wait(u32),
    Quit,
}

fn run(command: Command) {
    match (command) {
        Command.Move(0, 0) => { Console.writeLine("stay"); }
        Command.Move(x, _) => {
            Console.write("move x by ");
            Console.writeLine(x);
        }
        Command.Wait(seconds) => {
            Console.write("wait ");
            Console.writeLine(seconds);
        }
        Command.Quit => { Console.writeLine("quit"); }
    }
}

fn main() {
    run(Command.Move(0, 0));
    run(Command.Move(5, 1));
    run(Command.Wait(3));
    run(Command.Quit);
}
```

```text
stay
move x by 5
wait 3
quit
```

In each position of a variant's payload, a pattern says one of three things:

- a **name**, as `seconds` in `Command.Wait(seconds)`: any value fits, and the name holds it inside the arm's block.
  This is called **binding** the value;
- a **literal**, as the zeros in `Command.Move(0, 0)`: only that value fits;
- `_`: any value fits, and it gets no name.

`Command.Move(5, 1)` does not fit the first arm, because 5 is not 0. It fits the second, which binds `x` to 5 and
ignores the 1.

## Every case is covered

::: tip Rule
A `match` must be **exhaustive**: every possible value fits some arm. If one does not, the program does not
compile, and the diagnostic names a value that was left out.

*Specification: `type-system.md` §15.2.*
:::

<span class="listing-caption"><strong>Listing 6-10</strong>, <code>main.cee</code>: No arm takes <code>Command.Wait</code>. This does not compile.</span>

<!-- ceesafe: fail N0256 -->
```ceesafe
enum Command {
    Move(i32, i32),
    Wait(u32),
    Quit,
}

fn run(command: Command) {
    match (command) {
        Command.Move(x, y) => { Console.writeLine(x + y); }
        Command.Quit => { Console.writeLine("quit"); }
    }
}

fn main() {
    run(Command.Quit);
}
```

```text
main.cee:8:12: error[N0256]: this match does not cover Command.Wait; add the missing arms or a '_' arm
  |
8 |     match (command) {
  |            ^^^^^^^
```

This check is the reason to prefer an enum and a `match` to a chain of `if`. When a variant is added to `Command`
later, every `match` on a `Command` that has no arm for it stops compiling, and the diagnostics are the list of
places to change.

## The catch-all

A pattern that is only `_`, or only a name, fits every value. As the last arm, it takes whatever the arms above
did not. This is how a `match` on a number is made exhaustive.

<span class="listing-caption"><strong>Listing 6-11</strong>, <code>main.cee</code>: Two status codes have arms of their own. A name catches the rest and holds the value.</span>

```ceesafe
fn report(status: u32) {
    match (status) {
        200 => { Console.writeLine("ok"); }
        404 => { Console.writeLine("not found"); }
        other => {
            Console.write("status ");
            Console.writeLine(other);
        }
    }
}

fn main() {
    report(200);
    report(404);
    report(500);
}
```

```text
ok
not found
status 500
```

Write `_ => { … }` instead when the block does not need the value. An arm that does nothing has an empty block:
`_ => {}`.

A catch-all has a cost. A `match` on an enum that ends with `_` stays exhaustive when a variant is added, so the
compiler no longer tells you about the new variant. On your own enums, list the variants when you can.

## `match` as an expression

A `match` can also produce a value. Then each arm is a pattern, `=>` and an **expression**, and the arms are
separated by commas. Every arm gives a value of the same type.

<span class="listing-caption"><strong>Listing 6-12</strong>, <code>main.cee</code>: A <code>match</code> as a function's whole body, and as the value of a <code>let</code>.</span>

```ceesafe
enum Shape {
    Empty,
    Circle(i64),
    Rect(i64, i64),
}

fn area(s: Shape) -> i64 => match (s) {
    Shape.Empty => 0,
    Shape.Circle(r) => 3 * r * r,
    Shape.Rect(w, h) => w * h,
};

fn main() {
    let shape = Shape.Rect(3, 4);
    let name = match (shape) {
        Shape.Empty => "nothing",
        Shape.Circle(_) => "a circle",
        Shape.Rect(w, h) => {
            Console.writeLine("comparing the sides");
            if (w == h) "a square" else "a rectangle"
        }
    };
    Console.writeLine(name);
    Console.writeLine(area(shape));
}
```

```text
comparing the sides
a rectangle
12
```

- `fn area(s: Shape) -> i64 => match (s) { … };` is a function whose body is one expression: `=>` and the
  expression take the place of a block with a `return`.
- An arm that needs statements uses a block that **ends with its value, without a semicolon**. In the `Rect` arm
  that value is an `if` expression.
- The `match` after `let name =` ends with `;` because the `let` statement does.

::: tip Rule
A `match` that begins a statement has a block in every arm. A `match` used as a value has an expression in every
arm, or a block that ends with its value.

*Specification: `type-system.md` §15.3.*
:::

## Numbers, ranges, text and conditions

A `match` takes more than enums: integers, characters, `bool`, and text. Patterns have a few more forms for them.

<span class="listing-caption"><strong>Listing 6-13</strong>, <code>main.cee</code>: Ranges, alternatives, a guard, text and <code>bool</code> as patterns.</span>

```ceesafe
fn grade(score: u32) -> StringView => match (score) {
    90..=100 => "A",
    80..90 => "B",
    n if (n > 100) => "not a score",
    _ => "below B",
};

fn method(word: StringView) -> u32 => match (word) {
    "get" | "head" => 1,
    "put" => 2,
    _ => 0,
};

fn answer(yes: bool) -> StringView => match (yes) {
    true => "yes",
    false => "no",
};

fn main() {
    Console.writeLine(grade(95));
    Console.writeLine(grade(80));
    Console.writeLine(grade(250));
    Console.writeLine(method("head"));
    Console.writeLine(answer(3 > 2));
}
```

```text
A
B
not a score
1
yes
```

| Pattern | Fits |
|---|---|
| `90..=100` | the integers from 90 to 100, both included |
| `80..90` | the integers from 80 to 89: `..` leaves the end out |
| `"get" \| "head"` | a value that fits either pattern |
| `n if (n > 100)` | the pattern before `if`, and only when the condition, called a **guard**, is true |
| `"put"` | a `StringView` with that text |
| `true`, `false` | a `bool`; the two together are exhaustive, so there is no `_` |

`StringView` is the type of a string literal. A `match` on text or on an integer needs a catch-all, unless its
literals and ranges hold every value of the type. An arm with a guard never counts toward exhaustiveness, because
the compiler does not know when the guard is true. A float cannot be matched: `1.5 =>` is refused as "expected a
pattern".

A pattern can also be tested on its own. `value is pattern` is a `bool`: `command is Command.Quit` is true when
`command` is that variant. It is the short form of a `match` with two arms, and
[Optionals](./ch06-03-optionals#binding-in-a-condition) uses it to bind a value in an `if`.

## Mistakes

::: warning Mistake: an arm after the catch-all
The arms are tried in order, so nothing below a `_` or a lone name can ever run. The compiler refuses the arm
instead of ignoring it.
:::

<span class="listing-caption"><strong>Listing 6-14</strong>, <code>main.cee</code>: The catch-all is first. This does not compile.</span>

<!-- ceesafe: fail N0257 -->
```ceesafe
fn report(status: u32) {
    match (status) {
        _ => { Console.writeLine("something else"); }
        200 => { Console.writeLine("ok"); }
    }
}

fn main() {
    report(200);
}
```

```text
main.cee:4:9: error[N0257]: this arm can never match: an earlier '_' matches everything
  |
4 |         200 => { Console.writeLine("ok"); }
  |         ^^^
```

Put the catch-all last.

::: warning Mistake: a variant without its enum's name in a pattern
In a pattern, a bare name is not a variant. `Quit` is a new name that binds any command, so it is a catch-all,
and the arms under it can never run.
:::

<span class="listing-caption"><strong>Listing 6-15</strong>, <code>main.cee</code>: <code>Quit</code> binds every command. This does not compile.</span>

<!-- ceesafe: fail N0257 -->
```ceesafe
enum Command {
    Wait(u32),
    Quit,
}

fn run(command: Command) {
    match (command) {
        Quit => { Console.writeLine("quit"); }
        Command.Wait(seconds) => { Console.writeLine(seconds); }
    }
}

fn main() {
    run(Command.Wait(3));
}
```

```text
main.cee:9:9: error[N0257]: this arm can never match: earlier arms match every value it does
  |
9 |         Command.Wait(seconds) => { Console.writeLine(seconds); }
  |         ^^^^^^^^^^^^^^^^^^^^^
```

The message points at the arm below, not at `Quit`. When an arm "can never match" and you see no reason, look for a
bare name above it, and write `Command.Quit`.

::: warning Mistake: a statement's arm without a block
An arm of a `match` statement is a block, also when it is one call.
:::

<span class="listing-caption"><strong>Listing 6-16</strong>, <code>main.cee</code>: The first arm has no braces. This does not compile.</span>

<!-- ceesafe: fail N0100 -->
```ceesafe
fn report(status: u32) {
    match (status) {
        200 => Console.writeLine("ok"),
        _ => {}
    }
}

fn main() {
    report(200);
}
```

```text
main.cee:3:16: error[N0100]: expected '{', found identifier
  |
3 |         200 => Console.writeLine("ok"),
  |                ^^^^^^^
```

Write `200 => { Console.writeLine("ok"); }`.

::: details Coming from Rust
`match` has parentheses around its value. Variants are `Command.Quit`, with a dot. A statement's arms are blocks,
with no comma needed between them. A guard is `if (cond)`, with parentheses. `value is pattern` does the work of
`matches!`, `if let` and `while let`.
:::

::: details Coming from C, C#, Java or TypeScript
`match` is not `switch`. There is no `break` and no fall-through: one arm runs. There is no `default:`; the
catch-all is the pattern `_`. And a `match` that misses a case does not compile.
:::

## Summary

- `match (value) { pattern => { … } … }` runs the block of the first arm whose pattern fits.
- A name in a pattern binds the value in that position; `_` ignores it; a literal must equal it.
- A `match` covers every possible value (`N0256` names one that is missed).
- `_` or a lone name as the last arm catches the rest. An arm that cannot be reached is an error (`N0257`).
- As a value, a `match` has an expression in every arm, separated by commas.
- Patterns also take integers, ranges (`a..=b`, `a..b`), text, `bool`, alternatives (`p | q`) and guards
  (`p if (cond)`).

Next: [Optionals](./ch06-03-optionals), the built-in enum for a value that may be absent.
