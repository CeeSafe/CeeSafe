---
title: Running tests
summary: The ceesafec test command - what it prints for tests that pass, fail, panic, leak or time out, its options --filter, --list, --interp, --jobs, --timeout and --format, the allocation report behind a leak, and the exit status.
assumes: [ch01-03-hello-package, ch11-01-writing-tests]
keywords: ["ceesafec test", "--filter", "--list", "--interp", "--jobs", "--timeout", "--format json", leaked, panicked, "timed out", exit status]
specification: ["tooling.md §10.2"]
---

# Running tests

**This section teaches** how to run a package's tests with `ceesafec test`, how to read what it prints, and what
its options do. It assumes [How to write tests](./ch11-01-writing-tests) and the package commands of
[Hello, package](./ch01-03-hello-package).

## The command

Run `ceesafec test` in a package's directory. In the `laps` package of section 11.1, which has eight tests in four
files:

```sh
ceesafec test
```

```text
test digitReadsOnlyDigits ... ok
test paceIsTheAverage ... ok
test paceRoundsDown ... ok
test readsMinutesAndSeconds ... ok
test readsSeconds ... ok
test refusesALetter ... ok
test refusesNoText ... ok
test totalsRun ... ok

ceesafec test: 8 passed, 0 failed
```

The command does three things:

1. It checks the package with its tests, as `ceesafec check` does. A diagnostic stops it here.
2. It builds one executable that holds every test, `out/laps-tests`. Like `ceesafec build`, this step needs a C
   compiler.
3. It runs that executable once for each test, so every test is a process of its own. A test that panics or never
   ends cannot stop the others, and no test sees memory another one left behind.

Tests run in the order of their names, not the order of the files. A test's name is its function's name, and a
package's functions share one namespace, so no two tests have the same name.

::: tip Rule
`ceesafec test` runs every `@test` function of the package you are in, each as its own process, in the order of
their names. It takes no file arguments.

*Specification: `tooling.md` §10.2.*
:::

## When tests do not pass

The second package, `outcomes`, is a small program whose tests go wrong in every way a test can.

<span class="listing-caption"><strong>Listing 11-8</strong>, <code>src/main.cee</code>: The program under test.</span>

```ceesafe
/// The average of `count` laps that took `total` seconds together, rounded down.
fn average(total: u64, count: u64) -> u64 => total / count;

fn main() {
    Console.writeLine(average(180, 3));
}
```

```text
60
```

<span class="listing-caption"><strong>Listing 11-9</strong>, <code>tests/average.cee</code>: One test that passes, one with a wrong expectation, and one that divides by zero.</span>

<!-- ceesafe: skip -->
```ceesafe
@test
fn averages() -> Result<void, TestFailure> {
    Check.equalU64(60, average(180, 3))?;
    return Ok();
}

@test
fn averageRoundsUp() -> Result<void, TestFailure> {
    Check.equalU64(4, average(7, 2))?;
    return Ok();
}

@test
fn averageOfNoLaps() -> Result<void, TestFailure> {
    Check.equalU64(0, average(0, 0))?;
    return Ok();
}
```

<span class="listing-caption"><strong>Listing 11-10</strong>, <code>tests/others.cee</code>: A test that fails with its own error, one that prints, one that leaks and one that never ends.</span>

<!-- ceesafe: skip -->
```ceesafe
enum LapError {
    TooSlow,
}

@test
fn returnsItsOwnError() -> Result<void, LapError> {
    if (average(500, 2) > 200) {
        return Err(LapError.TooSlow);
    }
    return Ok();
}

@test
fn printsThenFails() -> Result<void, TestFailure> {
    Console.writeLine("the average is");
    Console.writeLine(average(7, 2));
    return Check.fail("on purpose");
}

@test
fn forgetsABox() {
    match (Box.create(5u32)) {
        Ok(b) => { let p = Box.intoRaw(b); }
        Err(_) => {}
    }
}

@test
fn spins() {
    var laps: u64 = 0;
    while (true) {
        laps = (laps + 1) % 1000;
    }
}
```

`forgetsABox` uses `Box`, which a later chapter covers: `Box.create` allocates memory for one value, and
`Box.intoRaw` gives up ownership of it, so nothing frees it. The package has two more tests, `keepsThreeLaps` and
`waitsTwice`, which pass; [section 11.3](./ch11-03-organising-tests) shows them. Because `spins` never ends, the run
below gives each test 2 seconds in place of the usual 60:

```sh
ceesafec test --timeout 2
```

```text
test averageOfNoLaps ... panicked
test averageRoundsUp ... failed
test averages ... ok
test forgetsABox ... leaked
test keepsThreeLaps ... ok
test printsThenFails ... failed
test returnsItsOwnError ... failed
test spins ... timed out
test waitsTwice ... ok

---- averageOfNoLaps: panicked ----
division by zero at src/main.cee:2:46

---- averageRoundsUp: failed ----
expected 4, got 3

---- forgetsABox: leaked ----
1 allocation(s) still live at the end

---- printsThenFails: failed ----
on purpose
-- its output:
the average is
3

---- returnsItsOwnError: failed ----
returned Err(LapError.TooSlow)

---- spins: timed out ----

ceesafec test: 3 passed, 6 failed
```

The first part has one line for each test, with its outcome. The second part has a block for each test that did not
pass:

- **panicked:** the panic's message and where it happened. `src/main.cee:2:46` is the division in `average`.
- **failed** with a `TestFailure`: the check's message.
- **failed** with the test's own error type: the variant that came back, `returned Err(LapError.TooSlow)`.
- **its output:** what the test printed. The command captures each test's output and shows it only for a test that
  did not pass, so a passing run stays short.

The last line counts every test that did not pass as failed.

## The outcomes

| Outcome | When |
|---|---|
| `ok` (passed) | the test returned `Ok()`, or returned from a `void` test |
| `failed` | it returned an `Err` |
| `panicked` | it panicked |
| `leaked` | it passed, but memory it allocated was not freed |
| `timed out` | it ran longer than `--timeout` allows |
| `crashed` | the process was ended by a signal, such as a fault in C code the test calls |
| `exited` | the test ended the process itself, with `Process.exit` |
| `skipped` | with `--interp`, the test calls C, which the interpreter cannot run |

The examples of this chapter show the first five. The last three are what `ceesafec test --help` lists beside
them.

## Leaks and the allocation report

CeeSafe has no garbage collector. A value's memory is freed when its owner goes away, and the compiler arranges
that, so safe code seldom leaks. Code that hands memory to C, or that takes ownership apart by hand as
`forgetsABox` does, can.

For every test, the runtime counts the allocations the test makes and the ones still live when it ends, and writes
one line, the **allocation report**. The command reads it. `--format json` shows the line, in the test's standard
error:

```sh
ceesafec test --filter forgets --format json
```

```text
{"version":1,"tests":[{"name":"forgetsABox","outcome":"leaked","milliseconds":137,"message":"1 allocation(s) still live at the end","stdout":"","stderr":"ceesafe-test 1 passed\nceesafe alloc: attempts=1 live=1\n"}],"sweepFailures":[],"sweepRuns":0,"sweepUnwrappedByTest":0,"passed":0,"failed":1,"skipped":0}
```

`attempts=1 live=1` says the test asked for memory once and one allocation was still live at the end. The test
itself passed (`ceesafe-test 1 passed`); the leak is what makes its outcome `leaked`.

::: tip Rule
A test that passes with memory still allocated has leaked, and a leak counts as a failure.
:::

## The options

**`--filter TEXT`** runs only the tests whose name contains `TEXT`. The match is on any part of the name, and it
distinguishes capitals from small letters.

```sh
ceesafec test --filter average
```

```text
test averageOfNoLaps ... panicked
test averageRoundsUp ... failed
test averages ... ok

---- averageOfNoLaps: panicked ----
division by zero at src/main.cee:2:46

---- averageRoundsUp: failed ----
expected 4, got 3

ceesafec test: 1 passed, 2 failed
```

**`--list`** prints the names, one a line, and runs nothing:

```sh
ceesafec test --list
```

```text
averageOfNoLaps
averageRoundsUp
averages
forgetsABox
keepsThreeLaps
printsThenFails
returnsItsOwnError
spins
waitsTwice
```

**`--interp`** runs each test in the compiler's interpreter, as `ceesafec run --interp` runs a program. It needs no
C compiler. The outcomes are the same, leaks and timeouts too: `ceesafec test --interp --timeout 2` in `outcomes`
ends with the same `3 passed, 6 failed`. Two things differ. A test that calls C is skipped. And for async code,
which a later chapter covers, the interpreter's clock is virtual and its order of running tasks is fixed, so a test
that waits takes no real time and runs the same way every time.

**`--jobs N`** runs `N` tests at once. The default is 1. The report keeps the order of the names.

**`--timeout SECONDS`** sets how long one test may run. The default is 60. A test that is still running then is
ended and reported as `timed out`.

**`--format json`** prints the whole report as one JSON document, for a program to read: each test's name, outcome,
time in milliseconds, message, standard output and standard error, then the counts.

`ceesafec test --help` lists the options. Section 11.3 covers the last one, `--sweep`.

## The exit status

The command exits with status 0 when every test passed or was skipped, and with 1 otherwise. The two runs of
`outcomes` above exit with 1, and the run of `laps` with 0. A script, or a continuous integration service, needs
nothing more than the status.

::: tip Rule
`ceesafec test` exits with 0 if every test passed, and with 1 if any test failed, panicked, leaked, timed out,
crashed or exited.
:::

## Mistakes

::: warning Mistake: a filter that matches no test
A mistyped filter runs nothing. If that counted as success, a script with a wrong name in it would pass for ever,
so it is an error with exit status 1.
:::

```sh
ceesafec test --filter Average
```

```text
ceesafec test: no test's name contains 'Average'
```

The names begin `average`, with a small letter. Use `--list` to see the names.

::: warning Mistake: naming a file
`ceesafec check` and `ceesafec run` accept files. `ceesafec test` does not: it always tests the package.
:::

```sh
ceesafec test tests/average.cee
```

```text
ceesafec: 'test' runs a package's tests: run it in the package's directory, without files
```

To run the tests of one file, give them names that share a word and use `--filter`.

::: warning Mistake: expecting a test's output
A passing test's `Console.writeLine` output is not shown. To see a value while you work on a test, make the test
fail, with `return Check.fail("look");`, and read the output under its name.
:::

::: details Coming from Rust
`ceesafec test` is `cargo test` with three differences. Each test is a process, not a thread, so tests cannot
disturb each other and `--jobs` is 1 unless you raise it. Output is captured in the same way, and there is no
`--nocapture`. A leak fails a test.
:::

::: details Coming from Go, C# or Python
There is no test class, no set-up or tear-down method, and no test discovery by file name. Every `@test` function
of the package is a test, wherever it is.
:::

## Summary

- `ceesafec test`, in a package, builds the tests once and runs each as its own process, in the order of the names.
- Each test is `ok`, `failed`, `panicked`, `leaked`, `timed out`, `crashed`, `exited` or `skipped`. A test that
  did not pass gets a block with its message and its output.
- A leak, memory still allocated at the end of a passing test, is a failure.
- `--filter TEXT` chooses tests by a part of the name, and matching nothing is an error. `--list` prints the names.
- `--interp` runs the tests in the interpreter, `--jobs N` runs `N` at once, `--timeout SECONDS` limits each test
  (60 by default), and `--format json` prints the report for a program.
- The exit status is 0 only when every test passed or was skipped.

Next: [where tests go in a package](./ch11-03-organising-tests), and the sweeps.
