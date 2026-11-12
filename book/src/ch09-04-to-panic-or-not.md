---
title: To panic or not
summary: How to choose between a panic and a Result - a bug or a broken invariant panics, an expected failure is a value. When ! is right in examples, prototypes and tests, and validating once at a boundary with a type.
assumes: [ch05-00-structs, ch09-01-panics, ch09-02-result]
keywords: [panic, Result, "!", invariant, validation, boundary, prototype, "exit status", private, design]
---

# To panic or not

**This section teaches** how to decide, for a failure in your own code, between a panic and a `Result`. It
assumes [panics](./ch09-01-panics), [`Result`, `?` and `!`](./ch09-02-result) and, for one listing,
[structs](./ch05-00-structs).

The language gives two tools. A panic stops the program. A `Result` hands the failure to the caller as a value.
The choice between them is a choice about who is at fault.

## The question to ask

Ask: **can this happen when every line of the program is correct?**

- If it can, it is an **expected failure**. The user types a word where a number belongs. A file is missing. The
  network is down. Memory runs out. Return a `Result`, or an optional when the only thing to say is "there is
  none". The caller decides what to do.
- If it cannot, it is a **bug**. An index computed by the program is past the end. A value that an earlier step
  was meant to check is wrong. Panic. No caller can do anything useful about a mistake in the code except have it
  found and fixed, and a panic names the place.

::: tip Rule
A failure that a correct program must expect is a value: a `Result` or an optional. A failure that only a bug can
cause is a panic.
:::

| Situation | Use |
|---|---|
| Input from a user, a file, the network, the command line | `Result` |
| Looking something up that may not be there | an optional |
| An allocation, in a library or a program that must keep running | `Result<…, AllocError>` with `?` |
| An argument that breaks the function's documented requirements | `panic` or `assert` |
| A state that the program's own logic rules out | `panic` or `assert` |
| A call that you can see cannot fail here | `!` |
| An example, a prototype, a test | `!` |

The same operation can fall on either side. It depends on where the value came from.

<span class="listing-caption"><strong>Listing 9-26</strong>, <code>main.cee</code>: A search that may find nothing returns an optional. Where the program knows the value is there, it writes <code>!</code>.</span>

```ceesafe
fn indexOf(values: Slice<i32>, wanted: i32) -> usize? {
    for (i in 0..values.length) {
        if (values[i] == wanted) {
            return i;
        }
    }
    return null;
}

fn main() {
    let primes = [2, 3, 5, 7];
    match (indexOf(primes[..], 5)) {
        Some(i) => { Console.writeLine(i); }
        null => { Console.writeLine("not there"); }
    }
    match (indexOf(primes[..], 6)) {
        Some(i) => { Console.writeLine(i); }
        null => { Console.writeLine("not there"); }
    }
    Console.writeLine(primes[indexOf(primes[..], 7)!]);
}
```

```text
2
not there
7
```

`indexOf` cannot know whether a missing value is a bug, so it reports it as a value and does not panic. The first
two callers treat "not there" as an ordinary answer. The last caller can see that 7 is in the array, so for it a
`null` would be a bug, and `!` says so.

That is the general shape. **A function that is called from many places returns the failure. The caller, which
knows more, decides whether it is a bug.**

## Panics for broken requirements

A function may require something of its arguments that the type does not express: a month from 1 to 12, a count
that is not zero. When a caller breaks the requirement, the caller has a bug. Listing 9-4 in
[section 9.1](./ch09-01-panics#calling-panic-yourself) panics for month 13, and that is right when months come
from the program itself.

Keep such a check at the top of the function, with `assert` or `panic`, and say in the function's documentation
what it requires. A check that stops at the first sign of a bug is worth more than a wrong answer that travels
further.

## `!` in examples, prototypes and tests

Handling every error well takes thought about what the user should see. A first version of a program, or an
example that teaches something else, does not need that yet. Write `!` and move on: the program stops with a
location if anything fails.

`!` is easy to find again. When the prototype becomes a real program, search for `!` and decide, for each one,
whether it is a claim ("this cannot fail") or a debt ("I have not handled this yet"). Replace each debt with `?`
or a `match`.

In a test, a panic is a failed test, so `!` is the natural way to say "this must work".

A finished program handles expected failures where it can say something useful, which is often `main`:

<span class="listing-caption"><strong>Listing 9-27</strong>, <code>main.cee</code>: <code>age</code> returns the failure. <code>main</code> uses <code>!</code> on text it can see is a number, and handles text it cannot.</span>

<!-- ceesafe: exits 2 -->
```ceesafe
enum AgeError {
    NotANumber,
}

fn age(text: StringView) -> Result<u32, AgeError> {
    var value: u32 = 0;
    for (c in text.bytes()) {
        if (c < '0' || c > '9') {
            return Err(AgeError.NotANumber);
        }
        value = value * 10 + (c - '0') as u32;
    }
    return Ok(value);
}

fn main() -> i32 {
    Console.writeLine(age("36")!);
    match (age("thirty-six")) {
        Ok(n) => { Console.writeLine(n); }
        Err(AgeError.NotANumber) => {
            Console.writeLine("please give your age as a number");
            return 2;
        }
    }
    return 0;
}
```

```text
36
please give your age as a number
```

The user sees a sentence about what to do, and the program ends with exit status 2, which a script can test.

## Validate at the boundary, with a type

A program takes in values from outside at a few places: where it reads arguments, input and files. Those places
are its **boundary**. If every function inside checks its arguments again, the checks are repeated everywhere,
and each one needs an answer to "what if it is wrong here?".

The alternative is to check once, at the boundary, and put the checked value in a type of its own. The only way
to get a value of that type is through the check. After that, having the value is the proof that it is valid.

<span class="listing-caption"><strong>Listing 9-28</strong>, <code>main.cee</code>: A <code>Percent</code> can only be made by <code>Percent.of</code>, which checks the range once.</span>

```ceesafe
enum RangeError {
    Negative,
    TooLarge,
}

struct Percent {
    private value: u8;

    fn of(n: i32) -> Result<Percent, RangeError> {
        if (n < 0) {
            return Err(RangeError.Negative);
        }
        if (n > 100) {
            return Err(RangeError.TooLarge);
        }
        return Ok(Percent { value: truncate<u8>(n) });
    }
}

fn bar(p: Percent) {
    for (i in 0..(p.value / 10)) {
        Console.write("#");
    }
    Console.writeLine("");
}

fn show(n: i32) {
    match (Percent.of(n)) {
        Ok(p) => { bar(p); }
        Err(RangeError.Negative) => { Console.writeLine("less than 0"); }
        Err(RangeError.TooLarge) => { Console.writeLine("more than 100"); }
    }
}

fn main() {
    show(70);
    show(170);
}
```

```text
#######
more than 100
```

- `Percent.of` is a function of the struct that takes no `this`, so it is called on the type. It is the boundary:
  it takes any `i32` and returns a `Result`.
- `private` on the field limits it to the file that declares the struct. Code in other files cannot write
  `Percent { value: 200 }`, and so cannot make a `Percent` except through `Percent.of`.
- `bar` takes a `Percent`. It does not check the range, does not return a `Result`, and has no case for a bad
  value, because there is no bad `Percent`.

The expected failure is handled once, in `show`. Everything past that point is simpler. If a function deep in the
program takes a plain `i32` and must panic or return an error when it is over 100, ask whether a type like this
would move the check to the boundary.

::: tip Rule
Check a value from outside once, where it enters, and give the checked value its own type. Functions that take
that type need no check and no error.
:::

## Mistakes

::: warning Mistake: a panic for bad input
Text that a person typed is never guaranteed to be a number. A function that panics on it turns a typing mistake
into a crash.
:::

<span class="listing-caption"><strong>Listing 9-29</strong>, <code>main.cee</code>: <code>age</code> panics on input that is wrong but not a bug.</span>

<!-- ceesafe: panics -->
```ceesafe
fn age(text: StringView) -> u32 {
    var value: u32 = 0;
    for (c in text.bytes()) {
        if (c < '0' || c > '9') {
            panic("age: not a number");
        }
        value = value * 10 + (c - '0') as u32;
    }
    return value;
}

fn main() {
    Console.writeLine(age("36"));
    Console.writeLine(age("thirty-six"));
}
```

```text
36
```

Then the program prints this on standard error and exits with status 134:

```text
ceesafe panic: age: not a number at main.cee:5:13
```

The caller has no way to ask "is this a number?" without risking the panic. Return a `Result`, as Listing 9-27
does. A caller that is sure can still write `!`; a caller that is not can `match`.

::: warning Mistake: a `Result` for a bug
The opposite mistake makes every caller handle an error that a correct caller never causes.
:::

<span class="listing-caption"><strong>Listing 9-30</strong>, <code>main.cee</code>: The loop can only pass months 1 to 12, and must unwrap an error that cannot happen.</span>

```ceesafe
enum MonthError {
    OutOfRange,
}

fn daysIn(month: i32) -> Result<i32, MonthError> {
    if (month < 1 || month > 12) {
        return Err(MonthError.OutOfRange);
    }
    if (month == 2) {
        return Ok(28);
    }
    if (month == 4 || month == 6 || month == 9 || month == 11) {
        return Ok(30);
    }
    return Ok(31);
}

fn main() {
    var total = 0;
    for (month in 1..13) {
        total += daysIn(month)!;
    }
    Console.writeLine(total);
}
```

```text
365
```

The program works, but the `Result` buys nothing: each caller writes `!`, and a new error type exists for a
condition that is a bug wherever it happens. When every sensible caller would write `!`, the function wants a
panic, as in Listing 9-4. If months do come from outside the program, validate them at the boundary into a type,
as Listing 9-28 does for percentages.

::: details Coming from Rust
The advice is the Rust Book's: `panic!` for bugs and broken contracts, `Result` for expected failures, `unwrap`
in examples and tests, and types that validate (the `Guess` example). Two things are stricter here. A panic
always ends the process, so it can never be used as a recoverable error at a thread or request boundary. And
allocation failure is on the `Result` side.
:::

::: details Coming from C#, Java, Python or TypeScript
Exceptions carry both kinds of failure, and the difference is left to the `catch`. Here the function's author
chooses when writing the signature. A rough guide: what you would catch and show to the user is a `Result`; what
you would leave uncaught, so that it reaches the log as a stack trace, is a panic.
:::

## Summary

- Ask whether the failure can happen in a correct program. If yes, return a `Result` or an optional. If no, panic.
- A function called from many places returns the failure; the caller decides whether it is a bug, and writes `!`
  if it is.
- `!` is right for a call you can see cannot fail, and in examples, prototypes and tests. Search for it when a
  prototype grows up.
- Handle expected failures where the program can say something useful, often in `main`, with a message and an
  exit status.
- Check outside values once, at the boundary, and keep the checked value in a type that cannot be made any other
  way.

Next: [chapter 10](./ch10-00-generics) covers generics, which is how `Result<T, E>` and `Vector<T>` take a type
as a parameter.
