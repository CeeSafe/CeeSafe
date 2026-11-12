---
title: How to write tests
summary: A test is a function marked @test that takes nothing and returns void or a Result - an Err or a panic fails it. The library's checks, used with ?, and testing a function that returns a Result.
assumes: [ch01-03-hello-package, ch03-03-functions, ch09-01-panics, ch09-02-result]
keywords: ["@test", test, Check, CheckResult, TestFailure, equalU64, equalText, isOk, isErr, assert, N0348]
diagnostics: [N0262, N0348]
specification: ["tooling.md §10.1"]
---

# How to write tests

**This section teaches** how to write a test: its shape, what makes it pass or fail, the checks the library has,
and how to test a function that returns a `Result`. It assumes you can make a package
([Hello, package](./ch01-03-hello-package)) and write a function ([Functions](./ch03-03-functions)), and that you
know panics ([section 9.1](./ch09-01-panics)) and `Result` with `?` ([section 9.2](./ch09-02-result)).

## A test is a function

The examples are in a library package called `laps`, which works with lap times on a running track. Its first file
has one function and two tests of it.

<span class="listing-caption"><strong>Listing 11-1</strong>, <code>src/pace.cee</code>: A function and two tests of it, in the same file.</span>

```ceesafe
/// The average seconds for one lap, when `laps` laps took `total` seconds.
public fn pace(total: u64, laps: u64) -> u64 => total / laps;

@test
fn paceIsTheAverage() -> Result<void, TestFailure> {
    Check.equalU64(60, pace(180, 3))?;
    Check.equalU64(61, pace(183, 3))?;
    return Ok();
}

@test
fn paceRoundsDown() {
    assert(pace(7, 2) == 3, "7 / 2 rounds down to 3");
}
```

`@test` on the line before a function makes it a test. In the package's directory, `ceesafec test` runs the tests
([section 11.2](./ch11-02-running-tests) covers the command):

```sh
ceesafec test --filter pace
```

```text
test paceIsTheAverage ... ok
test paceRoundsDown ... ok

ceesafec test: 2 passed, 0 failed
```

The file has no `main`, and nothing calls the two tests. The command finds every `@test` function of the package
and calls each one itself.

::: tip Rule
A test is a top-level function marked `@test`. It takes no parameters, and it returns `void` or
`Result<void, E>` for any error type `E`.

*Specification: `tooling.md` §10.1.*
:::

## What makes a test pass or fail

The two tests of Listing 11-1 show the two shapes.

- `paceIsTheAverage` returns `Result<void, TestFailure>`. It **passes** when it returns `Ok()`, and it **fails**
  when it returns an `Err`.
- `paceRoundsDown` returns nothing. It passes when it reaches its end. The only way it can go wrong is a panic:
  `assert(condition, message)` panics with the message when the condition is false.

A panic stops a test of either shape, and the command reports that test as **panicked**. That covers the panics you
did not plan as well: an index out of bounds, a division by zero, an overflow.

::: tip Rule
A test passes when it returns `Ok()` or, for a `void` test, when it returns. It fails when it returns an `Err`, and
it is reported as panicked when it panics.
:::

Prefer the first shape. A failed check then returns from the test as any error does, and its message says what was
expected and what was found. An `assert` knows only that its condition was false.

## The checks

`Check.equalU64(60, pace(180, 3))` is a **check**: a function of the library that compares two values. The expected
value comes first. A check returns `Result<void, TestFailure>`: `Ok()` when it holds, and otherwise an `Err` with a
`TestFailure` that holds a message. The `?` after the call passes that `Err` on, so the test stops at its first
failed check.

A check is an ordinary function, so an ordinary program can call one and look at what comes back:

<span class="listing-caption"><strong>Listing 11-2</strong>, <code>main.cee</code>: Each check of <code>Check</code>, and the message of each failure.</span>

```ceesafe
fn show(check: Result<void, TestFailure>) {
    match (check) {
        Ok() => { Console.writeLine("holds"); }
        Err(failure) => {
            Console.writeLine(StringView.fromUtf8(failure.message.view()) ?? "?");
        }
    }
}

fn main() {
    let short = [1u8, 2];
    let long = [1u8, 2, 3];
    show(Check.equalU64(60, 60));
    show(Check.equalU64(60, 61));
    show(Check.equalI64(-5, 5));
    show(Check.equalText("1:15", "1:16"));
    show(Check.equalBytes(short[..], long[..]));
    show(Check.isTrue(2 > 3, "2 is more than 3"));
    show(Check.isFalse(3 > 2, "3 is more than 2"));
    show(Check.fail("never right"));
}
```

```text
holds
expected 60, got 61
expected -5, got 5
expected "1:15", got "1:16"
the bytes differ at 2: expected the end, got 0x03 (lengths 2 and 3)
expected true: 2 is more than 3
expected false: 3 is more than 2
never right
```

The message is kept as bytes, and `StringView.fromUtf8` turns them into text to print; `ceesafec test` does the same
for a failed test. These are all the checks:

| Check | It holds when |
|---|---|
| `Check.equalU64(expected, actual)` | two unsigned integers are equal |
| `Check.equalI64(expected, actual)` | two signed integers are equal |
| `Check.equalText(expected, actual)` | two `StringView` texts are equal |
| `Check.equalBytes(expected, actual)` | two `Slice<u8>` have the same bytes |
| `Check.isTrue(condition, what)` | `condition` is true; `what` describes it in the message |
| `Check.isFalse(condition, what)` | `condition` is false |
| `Check.fail(message)` | never: it is the failure itself |
| `CheckResult<T, E>.isOk(borrow r, what)` | the `Result<T, E>` `r` is `Ok` |
| `CheckResult<T, E>.isErr(borrow r, what)` | `r` is `Err` |

The comparisons are typed. There is no check that compares two values of any type, so a smaller integer is
converted with `as` (`Check.equalU64(3, v.length as u64)`), and anything else goes through `Check.isTrue` with a
description.

## Testing a function that returns a Result

The package's second file reads a time from text. It can fail, so it returns a `Result`.

<span class="listing-caption"><strong>Listing 11-3</strong>, <code>src/time.cee</code>: A function that reads a time, with its error type and a private helper.</span>

```ceesafe
/// Why a time could not be read.
public enum TimeError {
    Empty,
    NotADigit,
}

/// The value of the digit `b`, or null if `b` is not a digit.
fn digit(b: u8) -> u8? {
    if (b >= '0' && b <= '9') {
        return Some(b - '0');
    }
    return null;
}

/// The seconds in a time written as seconds, "75", or as minutes and seconds, "1:15".
public fn seconds(text: StringView) -> Result<u64, TimeError> {
    if (text.length == 0) {
        return Err(TimeError.Empty);
    }
    var minutes: u64 = 0;
    var value: u64 = 0;
    for (b in text.bytes()) {
        if (b == ':') {
            minutes = value;
            value = 0;
        } else {
            match (digit(b)) {
                Some(d) => { value = value * 10 + d as u64; }
                null => { return Err(TimeError.NotADigit); }
            }
        }
    }
    return Ok(minutes * 60 + value);
}
```

A test of `seconds` has two things to check: whether the result is `Ok` or `Err`, and the value inside. It cannot
write `seconds("75")?`, because the test's error type is `TestFailure` and the function's is `TimeError`. The tests
are in a file of their own, in the package's `tests` directory.

<span class="listing-caption"><strong>Listing 11-4</strong>, <code>tests/seconds.cee</code>: Four tests of <code>seconds</code>, two for its values and two for its errors.</span>

<!-- ceesafe: skip -->
```ceesafe
@test
fn readsSeconds() -> Result<void, TestFailure> {
    let parsed = seconds("75");
    CheckResult<u64, TimeError>.isOk(borrow parsed, "\"75\" is a time")?;
    Check.equalU64(75, parsed.ok() ?? 0)?;
    return Ok();
}

@test
fn readsMinutesAndSeconds() -> Result<void, TestFailure> {
    match (seconds("1:15")) {
        Ok(s) => { Check.equalU64(75, s)?; }
        Err(_) => { return Check.fail("\"1:15\" is a time"); }
    }
    return Ok();
}

@test
fn refusesALetter() -> Result<void, TestFailure> {
    let parsed = seconds("1:x5");
    CheckResult<u64, TimeError>.isErr(borrow parsed, "a letter is not a digit")?;
    return Ok();
}

@test
fn refusesNoText() -> Result<void, TestFailure> {
    match (seconds("")) {
        Err(TimeError.Empty) => { return Ok(); }
        _ => { return Check.fail("expected Err(TimeError.Empty)"); }
    }
}
```

There are two ways, and the listing uses both:

- **`CheckResult`.** `CheckResult<u64, TimeError>.isOk(borrow parsed, "…")` holds when `parsed` is `Ok`, and `isErr`
  when it is `Err`. The two types are written out because the checks belong to a generic struct. `parsed.ok()` then
  gives the value as an optional, and `?? 0` is a fallback that the test never reaches after `isOk`.
- **`match`.** `readsMinutesAndSeconds` checks the value in the `Ok` arm and fails in the other. `refusesNoText`
  matches the one error it expects, `Err(TimeError.Empty)`, so it also checks which error came back, which `isErr`
  does not.

## Testing that something panics

There is no way to write a test that passes because the code panicked. A panic ends the test's process, and the
command reports the test as panicked.

This follows from what a panic is in CeeSafe: a bug, not a result a caller is meant to handle
([section 9.1](./ch09-01-panics)). If callers need to know that an input is wrong, the function returns a `Result`,
as `seconds` does, and the test checks for the `Err`, as `refusesALetter` does. `pace(180, 0)` divides by zero and
panics; to make that testable, `pace` would return a `Result`.

## Mistakes

::: warning Mistake: a check without `?`
A check does not stop the test. It returns a `Result`, and without `?` nobody looks at it. The compiler does not
warn.
:::

<span class="listing-caption"><strong>Listing 11-5</strong>, <code>src/mistakes.cee</code>: Two tests with the same wrong expectation. Only the second has the <code>?</code>.</span>

```ceesafe
fn pace(total: u64, laps: u64) -> u64 => total / laps;

@test
fn dropsItsCheck() {
    Check.equalU64(4, pace(7, 2));
}

@test
fn keepsItsCheck() -> Result<void, TestFailure> {
    Check.equalU64(4, pace(7, 2))?;
    return Ok();
}
```

`ceesafec test` in that package prints:

```text
test dropsItsCheck ... ok
test keepsItsCheck ... failed

---- keepsItsCheck: failed ----
expected 4, got 3

ceesafec test: 1 passed, 1 failed
```

`dropsItsCheck` passes although its check fails. Give a test that uses checks the return type
`Result<void, TestFailure>`, and end every check with `?`.

::: warning Mistake: `??` on a Result
`??` gives a fallback for an optional. A `Result` is not an optional.
:::

<span class="listing-caption"><strong>Listing 11-6</strong>, <code>main.cee</code>: A fallback for a <code>Result</code>. This does not compile.</span>

<!-- ceesafe: fail N0262 -->
```ceesafe
enum HalfError {
    Odd,
}

fn half(n: u64) -> Result<u64, HalfError> {
    if (n % 2 == 1) {
        return Err(HalfError.Odd);
    }
    return Ok(n / 2);
}

@test
fn halves() -> Result<void, TestFailure> {
    Check.equalU64(4, half(8) ?? 0)?;
    return Ok();
}
```

```text
main.cee:14:23: error[N0262]: '??' needs an optional value on its left, found 'Result<u64, HalfError>'
   |
14 |     Check.equalU64(4, half(8) ?? 0)?;
   |                       ^^^^^^^
```

Write `half(8).ok() ?? 0`, or `match` on the result as Listing 11-4 does.

::: warning Mistake: calling a test
Only `ceesafec test` runs a test. A call from `main`, or from another test, does not compile.
:::

<span class="listing-caption"><strong>Listing 11-7</strong>, <code>main.cee</code>: <code>main</code> calls a test. This does not compile.</span>

<!-- ceesafe: fail N0348 -->
```ceesafe
fn pace(total: u64, laps: u64) -> u64 => total / laps;

@test
fn paceIsTheAverage() -> Result<void, TestFailure> {
    Check.equalU64(60, pace(180, 3))?;
    return Ok();
}

fn main() {
    paceIsTheAverage()!;
    Console.writeLine("tested");
}
```

```text
main.cee:10:5: error[N0348]: a test is run by 'ceesafec test', not called (ADR 0049 §3.1)
   |
10 |     paceIsTheAverage()!;
   |     ^^^^^^^^^^^^^^^^
```

Put code that two tests share in an ordinary function without `@test`, and call that. `N0348` is also the code for
a test of the wrong shape: one with a parameter, or one that returns `bool`.

::: details Coming from Rust
- `@test` is `#[test]`. There is no `#[cfg(test)] mod tests`: `ceesafec build` leaves every `@test` function out.
- There is no `assert_eq!` for any type. The checks are typed functions that return a `Result`, and they need `?`.
- There is no `#[should_panic]`. Test for an `Err`.
:::

::: details Coming from C#, Java, TypeScript or Python
A failed check is a returned value, not a thrown exception. That is why each check ends with `?`, and why a test
that uses checks returns a `Result`.
:::

## Summary

- A test is a top-level function marked `@test`, with no parameters, returning `void` or `Result<void, E>`.
- It passes by returning `Ok()` or by returning; it fails by returning an `Err`; a panic is reported as panicked.
- The checks (`Check.equalU64`, `equalI64`, `equalText`, `equalBytes`, `isTrue`, `isFalse`, `fail`, and
  `CheckResult<T, E>.isOk` and `isErr`) return `Result<void, TestFailure>`. End each with `?`.
- Test a function that returns a `Result` with `CheckResult` or with `match`. `??` does not apply to a `Result`
  (`N0262`).
- No test can expect a panic. Return a `Result` from code whose failure you want to test.
- Only `ceesafec test` calls a test (`N0348`).

Next: [running the tests](./ch11-02-running-tests), and what the command prints when one fails.
