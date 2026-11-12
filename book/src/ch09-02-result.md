---
title: Result, ? and !
summary: A function that can fail returns a Result, either Ok with a value or Err with a reason. Matching one, passing an error on with ?, unwrapping with !, your own error enum, converting with @from, and the ignored Result.
assumes: [ch06-01-defining-an-enum, ch06-02-match, ch06-03-optionals, ch09-01-panics]
keywords: [Result, Ok, Err, "?", "!", "@from", error, enum, match, isOk, isErr, ok, err, okOr, "let _"]
diagnostics: [N0260, N0261]
specification: ["type-system.md §17"]
---

# `Result`, `?` and `!`

**This section teaches** how a function reports a failure that is not a bug, and how its caller deals with it. It
assumes [enums](./ch06-01-defining-an-enum), [`match`](./ch06-02-match), [optionals](./ch06-03-optionals) and
[panics](./ch09-01-panics).

A function that can fail returns a `Result<T, E>`. It is a built-in enum with two variants:

- `Ok(value)` holds the answer, of type `T`;
- `Err(reason)` holds the reason for the failure, of type `E`.

`T` and `E` stand for types that you choose: `Result<u8, ParseError>` is either a `u8` or a `ParseError`.
[Chapter 10](./ch10-00-generics) covers types with parameters like these.

## Returning and matching a `Result`

The error type is usually an enum of your own, with one variant for each way the function can fail. A variant can
carry data about the failure.

<span class="listing-caption"><strong>Listing 9-9</strong>, <code>main.cee</code>: A function that reads a number from text, and a caller that handles every outcome.</span>

```ceesafe
enum ParseError {
    Empty,
    NotADigit(u8),
    TooLarge,
}

fn number(text: StringView) -> Result<u8, ParseError> {
    if (text.length == 0) {
        return Err(ParseError.Empty);
    }
    var value: u32 = 0;
    for (c in text.bytes()) {
        if (c < '0' || c > '9') {
            return Err(ParseError.NotADigit(c));
        }
        value = value * 10 + (c - '0') as u32;
        if (value > 255) {
            return Err(ParseError.TooLarge);
        }
    }
    return Ok(truncate<u8>(value));
}

fn show(text: StringView) {
    match (number(text)) {
        Ok(n) => { Console.writeLine(n); }
        Err(ParseError.Empty) => { Console.writeLine("nothing to read"); }
        Err(ParseError.NotADigit(c)) => {
            Console.write("not a digit: byte ");
            Console.writeLine(c);
        }
        Err(ParseError.TooLarge) => { Console.writeLine("more than 255"); }
    }
}

fn main() {
    show("42");
    show("4x");
    show("");
    show("300");
}
```

```text
42
not a digit: byte 120
nothing to read
more than 255
```

- `number` returns `Ok(…)` on the one path that succeeds and `Err(…)` on each path that fails.
- `Ok` and `Err` are written without a type in front, in the function and in the `match`.
- The `match` must cover every case, as for any enum. Remove the `TooLarge` arm and the program does not compile.
  `Err(_)` covers every error that the arms before it did not.

::: tip Rule
A function that can fail in a way a correct program must expect returns `Result<T, E>`: `Ok(value)` or
`Err(reason)`. The caller cannot use the value without saying what happens on `Err`.

*Specification: `type-system.md` §17.1.*
:::

A function that succeeds with nothing to return has the type `Result<void, E>`. It returns `Ok()`, and the pattern
that matches it is `Ok`:

<span class="listing-caption"><strong>Listing 9-10</strong>, <code>main.cee</code>: A check that returns no value, and a <code>main</code> that turns the failure into exit status 1.</span>

<!-- ceesafe: exits 1 -->
```ceesafe
enum CheckError {
    TooShort,
    NoDigit,
}

fn check(password: StringView) -> Result<void, CheckError> {
    if (password.length < 8) {
        return Err(CheckError.TooShort);
    }
    for (c in password.bytes()) {
        if (c >= '0' && c <= '9') {
            return Ok();
        }
    }
    return Err(CheckError.NoDigit);
}

fn main() -> i32 {
    match (check("correct horse")) {
        Ok => {
            Console.writeLine("accepted");
            return 0;
        }
        Err(CheckError.TooShort) => { Console.writeLine("too short"); }
        Err(CheckError.NoDigit) => { Console.writeLine("needs a digit"); }
    }
    return 1;
}
```

```text
needs a digit
```

`main` cannot return a `Result`. It returns nothing, or an `i32` that becomes the program's exit status, so `main`
is where errors are finally handled: print a message, and return a status that is not 0.

## `?` passes the error on

Most functions cannot do anything useful with an error from a function they call. They want to stop and hand the
same error to their own caller. The `?` operator does that.

<span class="listing-caption"><strong>Listing 9-11</strong>, <code>main.cee</code>: <code>number</code> and <code>sum</code> pass errors on with <code>?</code>. Only <code>main</code> handles them.</span>

```ceesafe
enum ParseError {
    Empty,
    NotADigit(u8),
}

fn digit(c: u8) -> Result<u32, ParseError> {
    if (c < '0' || c > '9') {
        return Err(ParseError.NotADigit(c));
    }
    return Ok((c - '0') as u32);
}

fn number(text: StringView) -> Result<u32, ParseError> {
    if (text.length == 0) {
        return Err(ParseError.Empty);
    }
    var value: u32 = 0;
    for (c in text.bytes()) {
        value = value * 10 + digit(c)?;
    }
    return Ok(value);
}

fn sum(a: StringView, b: StringView) -> Result<u32, ParseError> {
    let x = number(a)?;
    let y = number(b)?;
    return Ok(x + y);
}

fn main() {
    match (sum("40", "2")) {
        Ok(n) => { Console.writeLine(n); }
        Err(_) => { Console.writeLine("not two numbers"); }
    }
    match (sum("40", "two")) {
        Ok(n) => { Console.writeLine(n); }
        Err(_) => { Console.writeLine("not two numbers"); }
    }
}
```

```text
42
not two numbers
```

`number(a)?` means: if `number(a)` is `Ok(v)`, the expression's value is `v`. If it is `Err(e)`, the function
returns `Err(e)` at once, as a `return` statement would. So `x` and `y` are plain `u32` values, and the line after
them runs only when both calls succeeded.

`?` returns from the function, so the function's return type must be able to carry the error.

::: tip Rule
`e?` is allowed only in a function that returns a `Result` with the same error type as `e`, or with an error type
that has an `@from` variant for it. It gives the value inside `Ok`, or returns the `Err` to the caller.

*Specification: `type-system.md` §17.4.*
:::

`?` also works on an optional, in a function that returns an optional: `o?` is the value, or returns `null`.

## `!` says this cannot fail

`r!` is the value inside `Ok`. If `r` is an `Err`, the program [panics](./ch09-01-panics).

<span class="listing-caption"><strong>Listing 9-12</strong>, <code>main.cee</code>: The first <code>!</code> is right. The second unwraps an <code>Err</code> and panics.</span>

<!-- ceesafe: panics -->
```ceesafe
enum ParseError {
    NotADigit(u8),
}

fn digit(c: u8) -> Result<u32, ParseError> {
    if (c < '0' || c > '9') {
        return Err(ParseError.NotADigit(c));
    }
    return Ok((c - '0') as u32);
}

fn main() {
    Console.writeLine(digit('7')!);
    Console.writeLine(digit('x')!);
}
```

```text
7
```

Then, on standard error, and the program exits with status 134:

```text
ceesafe panic: unwrapped an Err value at main.cee:14:23
```

Use `!` where an `Err` would be a bug: the argument is a constant you can see is valid, as `'7'` is here. It is
also the right choice in a small program, an example or a test, where stopping is an acceptable answer to any
failure. The panic message does not say which error it was, so `!` is not a way to report errors to a user.

## Converting one error type to another

A function often calls functions with different error types. Its own error enum can wrap theirs: give it a variant
whose payload is the other error type, and mark the variant `@from`. Then `?` does the wrapping.

<span class="listing-caption"><strong>Listing 9-13</strong>, <code>main.cee</code>: <code>?</code> turns a <code>ParseError</code> into <code>ConfigError.Parse</code>.</span>

```ceesafe
enum ParseError {
    NotADigit(u8),
}

enum ConfigError {
    Missing,
    @from Parse(ParseError),
}

fn digit(c: u8) -> Result<u32, ParseError> {
    if (c < '0' || c > '9') {
        return Err(ParseError.NotADigit(c));
    }
    return Ok((c - '0') as u32);
}

fn port(text: StringView) -> Result<u32, ConfigError> {
    if (text.length == 0) {
        return Err(ConfigError.Missing);
    }
    var value: u32 = 0;
    for (c in text.bytes()) {
        value = value * 10 + digit(c)?;
    }
    return Ok(value);
}

fn main() {
    match (port("80a")) {
        Ok(n) => { Console.writeLine(n); }
        Err(ConfigError.Missing) => { Console.writeLine("no port given"); }
        Err(ConfigError.Parse(ParseError.NotADigit(c))) => {
            Console.write("bad port: byte ");
            Console.writeLine(c);
        }
    }
}
```

```text
bad port: byte 97
```

In `port`, `digit(c)?` has the error type `ParseError`, and `port` returns `ConfigError`. Because of `@from`, an
`Err(e)` from `digit` leaves `port` as `Err(ConfigError.Parse(e))`. The caller can still match all the way down to
the byte.

::: tip Rule
`@from` on a variant with one payload lets `?` convert that payload's type into the enum. Only `?` converts: a
`return Err(…)` names the variant itself. An enum has at most one `@from` variant for each payload type.

*Specification: `type-system.md` §17.4.*
:::

This is the only conversion between error types that the language has. There is no method that maps the error
inside a `Result` to another type; where `@from` does not fit, `match` and build the new error by hand.

## Asking without matching

A `Result` has four methods for the cases where a `match` is more than you need:

| Method | Gives |
|---|---|
| `r.isOk()`, `r.isErr()` | a `bool` |
| `r.ok()` | the value as an optional `T?`: `null` for an `Err`. The error is thrown away |
| `r.err()` | the error as an optional `E?`: `null` for an `Ok` |

`o.okOr(e)` goes the other way: it turns an optional into a `Result`, with `e` as the error for `null`.

<span class="listing-caption"><strong>Listing 9-14</strong>, <code>main.cee</code>: Testing a <code>Result</code>, and turning it into an optional with a fallback.</span>

```ceesafe
enum ParseError {
    NotADigit(u8),
}

fn digit(c: u8) -> Result<u32, ParseError> {
    if (c < '0' || c > '9') {
        return Err(ParseError.NotADigit(c));
    }
    return Ok((c - '0') as u32);
}

fn main() {
    let d = digit('7');
    Console.writeLine(d.isOk());
    Console.writeLine(d.ok() ?? 0);
    Console.writeLine(digit('x').ok() ?? 0);
    Console.writeLine(digit('x').err().isSome());
}
```

```text
true
7
0
true
```

## Mistakes

::: warning Mistake: `?` in a function that does not return a `Result`
`?` returns the error, so the function needs somewhere to return it to. `main` has no such return type.
:::

<span class="listing-caption"><strong>Listing 9-15</strong>, <code>main.cee</code>: <code>main</code> returns nothing, so it cannot use <code>?</code>. This does not compile.</span>

<!-- ceesafe: fail N0261 -->
```ceesafe
enum ParseError {
    NotADigit(u8),
}

fn digit(c: u8) -> Result<u32, ParseError> {
    if (c < '0' || c > '9') {
        return Err(ParseError.NotADigit(c));
    }
    return Ok((c - '0') as u32);
}

fn main() {
    let d = digit('7')?;
    Console.writeLine(d);
}
```

```text
main.cee:13:13: error[N0261]: '?' returns 'Err' from the function, so 'main' must return 'Result<_, ParseError>'
   |
13 |     let d = digit('7')?;
   |             ^^^^^^^^^^^
```

In an ordinary function, change the return type to a `Result` with that error type. In `main`, which cannot
return a `Result`, handle the error with `match` as Listing 9-10 does, or write `!` if stopping is acceptable.

::: warning Mistake: `?` across two error types with no `@from`
Take `@from` away from Listing 9-13 and `?` has no way to turn a `ParseError` into a `ConfigError`.
:::

<span class="listing-caption"><strong>Listing 9-16</strong>, <code>main.cee</code>: <code>Parse</code> is not marked <code>@from</code>. This does not compile.</span>

<!-- ceesafe: fail N0261 -->
```ceesafe
enum ParseError {
    NotADigit(u8),
}

enum ConfigError {
    Missing,
    Parse(ParseError),
}

fn digit(c: u8) -> Result<u32, ParseError> {
    if (c < '0' || c > '9') {
        return Err(ParseError.NotADigit(c));
    }
    return Ok((c - '0') as u32);
}

fn port(text: StringView) -> Result<u32, ConfigError> {
    if (text.length == 0) {
        return Err(ConfigError.Missing);
    }
    return Ok(digit(text[0])?);
}

fn main() {
    Console.writeLine(port("8").isOk());
}
```

```text
main.cee:21:15: error[N0261]: '?' would return 'Err' of type 'ParseError', but 'port' returns errors of type 'ConfigError'; mark a variant of the function's error enum '@from' to convert, as in '@from Variant(ParseError)'
   |
21 |     return Ok(digit(text[0])?);
   |               ^^^^^^^^^^^^^^^
```

The message says what to do: write `@from Parse(ParseError)`.

::: warning Mistake: calling a function and ignoring its `Result`
The compiler does not stop you, and does not warn. A call whose `Result` is not used compiles, and the error is
lost.
:::

<span class="listing-caption"><strong>Listing 9-17</strong>, <code>main.cee</code>: The second <code>save</code> fails and nobody looks. The program prints something false.</span>

```ceesafe
enum SaveError {
    Full,
}

fn save(slot: i32) -> Result<void, SaveError> {
    if (slot > 2) {
        return Err(SaveError.Full);
    }
    return Ok();
}

fn main() {
    save(1);
    save(9);
    Console.writeLine("saved both");
}
```

```text
saved both
```

Make it a habit that every call of a function that returns a `Result` ends in `?` or `!`, or is the subject of a
`match`. With `save(9)!;` this program panics with `unwrapped an Err value` instead of printing a false line. If
you do mean to ignore the outcome, say so: `let _ = save(9);` evaluates the call and discards its value, and a
reader sees that it was a decision.

::: warning Mistake: `Ok` or `Err` where no type says which `Result`
`Ok(42)` alone does not say what the error type is. The compiler needs the whole type from somewhere: a return
type, a parameter type, or a declared variable type.
:::

<span class="listing-caption"><strong>Listing 9-18</strong>, <code>main.cee</code>: Nothing gives <code>r</code> an error type. This does not compile.</span>

<!-- ceesafe: fail N0260 -->
```ceesafe
fn main() {
    let r = Ok(42);
    Console.writeLine(r!);
}
```

```text
main.cee:2:13: error[N0260]: the 'Result' type of 'Ok(...)' is unknown here; give the variable or parameter a 'Result<T, E>' type
  |
2 |     let r = Ok(42);
  |             ^^^^^^
```

Write the type: `let r: Result<i32, ParseError> = Ok(42);`. Inside a function, `return Ok(42);` needs nothing
more, because the return type gives it.

::: details Coming from Rust
`Result`, `Ok`, `Err` and `?` are Rust's. Four things differ:

- `r!` is `r.unwrap()`. There is no `expect`, and the panic message does not print the error.
- `?` converts through an `@from` variant of the error enum, not through a `From` implementation. It is one step.
- There are no `map`, `map_err`, `and_then` or `unwrap_or` on a `Result`. Use `match`, or `r.ok() ?? fallback`.
- There is no `#[must_use]`: an ignored `Result` is silently discarded, with no warning.
:::

::: details Coming from Go
`Result<T, E>` is the `(value, err)` pair as one value that is always exactly one of the two. `x := f()?` is
`x, err := f(); if err != nil { return err }`.
:::

::: details Coming from C#, Java, Python or TypeScript
There are no exceptions. A function that can fail says so in its return type, and each call that can fail is
marked with `?` or `!` where it happens. An error crosses a function only if that function passes it on.
:::

## Summary

- A function that can fail returns `Result<T, E>`: `Ok(value)` or `Err(reason)`. `Result<void, E>` returns `Ok()`.
- The error type is usually your own enum of reasons. `match` on the result to handle each one.
- `e?` gives the value, or returns the `Err` from the function. The function must return a `Result` with the same
  error type, or one with an `@from` variant for it (`N0261`).
- `e!` gives the value, or panics with `unwrapped an Err value`.
- `main` cannot return a `Result`: handle the error there, and return an exit status.
- `Ok(…)` and `Err(…)` need a `Result` type from their context (`N0260`).
- An ignored `Result` is lost without a diagnostic. End the call with `?` or `!`, `match` on it, or write
  `let _ = f();`.

Next: [allocation failure](./ch09-03-allocation-failure), the error that almost every program meets first.
