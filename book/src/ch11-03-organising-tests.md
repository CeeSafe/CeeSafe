---
title: Organising tests
summary: Where tests live in a package - beside the code or in the tests directory - how they reach private functions, how to test a library through its public interface from a second package, and the alloc and cancel sweeps.
assumes: [ch07-01-package-and-manifest, ch07-02-files-and-public, ch09-03-allocation-failure, ch11-01-writing-tests, ch11-02-running-tests]
keywords: [tests directory, private, public, import, "--sweep alloc", "--sweep cancel", allocation failure, cancellation, unwrap]
diagnostics: [N0200]
specification: ["tooling.md §10.1", "tooling.md §10.2"]
---

# Organising tests

**This section teaches** where the tests of a package go, what they can see, how to test a library as its users see
it, and what the two sweeps of `ceesafec test` do. It assumes [How to write tests](./ch11-01-writing-tests) and
[Running tests](./ch11-02-running-tests), the manifest and `public`
([sections 7.1](./ch07-01-package-and-manifest) and [7.2](./ch07-02-files-and-public)), and allocation failure
([section 9.3](./ch09-03-allocation-failure)).

## Two places for tests

A package has two places for its tests, and the `laps` package of section 11.1 uses both:

```text
laps/
├── ceesafe.toml
├── src/
│   ├── pace.cee      pace, and two tests of it
│   ├── time.cee      seconds and digit
│   └── totals.cee    totals, and one test of it
└── tests/
    ├── digit.cee     one test
    └── seconds.cee   four tests
```

- **Beside the code.** A `@test` function can be in any source file under `src/`.
  [Listing 11-1](./ch11-01-writing-tests#a-test-is-a-function) has a function and its tests in one file. This suits
  a few short tests that also show a reader how the function is used.
- **In the `tests` directory.** Every `.cee` file under `tests/`, beside `src/`, is a source file of the package
  for `ceesafec check` and `ceesafec test`. This suits tests that are longer than the code, or that test several
  files together.

The commands treat the two places alike, with one difference between the commands themselves:

| Command | `@test` functions in `src/` | Files in `tests/` |
|---|---|---|
| `ceesafec check` | checked | checked |
| `ceesafec test` | run | checked, and their tests run |
| `ceesafec build`, `ceesafec run` | left out | left out |

So a test cannot rot unseen, because `check` reads it, and a test cannot change the program you ship, because
`build` leaves it out.

::: tip Rule
Tests go in the package's source files or in its `tests` directory. Both are checked by `ceesafec check`, and
neither is part of what `ceesafec build` makes.

*Specification: `tooling.md` §10.1.*
:::

The directory has another name if the manifest says so: `tests = ["checks"]` under `[package]` makes `checks/` the
test directory. Without that line it is `tests`.

## Tests of private functions

A file in `tests/` is part of the same package as the files in `src/`. It names their functions directly, with no
`import`, and it sees the ones that are not `public`. `digit` in
[Listing 11-3](./ch11-01-writing-tests#testing-a-function-that-returns-a-result) is private, and this test calls
it:

<span class="listing-caption"><strong>Listing 11-11</strong>, <code>tests/digit.cee</code>: A test in the <code>tests</code> directory of a private function.</span>

<!-- ceesafe: skip -->
```ceesafe
@test
fn digitReadsOnlyDigits() -> Result<void, TestFailure> {
    Check.equalU64(7, (digit('7') ?? 0) as u64)?;
    Check.isTrue(digit('x').isNull(), "'x' is not a digit")?;
    return Ok();
}
```

`digit` returns an optional ([section 6.3](./ch06-03-optionals)): `?? 0` gives its value or 0, and `isNull()` tests
for no value.

::: tip Rule
The tests of a package see everything in the package, private or public, wherever the tests are.
:::

## Tests through the public interface

A library's users see only its `public` items. A test inside the library cannot show that the public items are
enough, because it sees the private ones too. `ceesafec test` has no kind of test that is limited to the public
interface, so the way to get one is a second package that depends on the library, as a user's package does.

`race` is an executable package next to `laps`. Its manifest names the library:

```toml
[package]
name = "race"
version = "0.1.0"
kind = "executable"

[dependencies]
laps = { path = "../laps" }
```

<span class="listing-caption"><strong>Listing 11-12</strong>, <code>race/tests/public.cee</code>: A test of the library <code>laps</code> from outside it.</span>

<!-- ceesafe: skip -->
```ceesafe
import laps;

@test
fn readsATime() -> Result<void, TestFailure> {
    Check.equalU64(125, seconds("2:05").ok() ?? 0)?;
    return Ok();
}
```

`import laps;` brings the library's public items into this file, as [chapter 7](./ch07-00-packages) describes. In
`race`, after `ceesafec fetch` has written `ceesafe.lock`:

```sh
ceesafec test
```

```text
test readsATime ... ok

ceesafec test: 1 passed, 0 failed
```

Only the tests of the package you are in run. The eight tests of `laps` do not run when you test `race`.

A package needs at least one source file under `src/`, so `race` has a small `src/main.cee` that prints
`seconds("2:05")`. A package cannot be tests alone.

## The sweeps

A test run exercises the path where nothing goes wrong. Two kinds of failure almost never happen in a test run and
do happen in production: an allocation that fails because memory ran out, and an async task that is cancelled
while it waits. The code for those cases is the code nobody ran. A **sweep** runs it.

### `--sweep alloc`

`totals` in `laps` builds a vector, so it allocates, and it passes an allocation failure on with `?`.

<span class="listing-caption"><strong>Listing 11-13</strong>, <code>src/totals.cee</code>: A function that allocates, and a test that handles its error.</span>

```ceesafe
/// The time on the clock at the end of each lap: the running totals of `laps`.
public fn totals(laps: Slice<u64>) -> Result<Vector<u64>, AllocError> {
    var clock = Vector<u64>.create();
    var now: u64 = 0;
    for (lap in laps) {
        now += lap;
        clock.push(now)?;
    }
    return Ok(clock);
}

@test
fn totalsRun() -> Result<void, TestFailure> {
    let laps = [60u64, 62, 58, 61, 59];
    match (totals(laps[..])) {
        Ok(clock) => {
            Check.equalU64(5, clock.length as u64)?;
            Check.equalU64(60, clock[0])?;
            Check.equalU64(300, clock[4])?;
        }
        Err(_) => { return Check.fail("out of memory"); }
    }
    return Ok();
}
```

With `--sweep alloc`, the command first runs the tests as usual. Then it runs each passing test again, once for
each allocation the test made: in run 1 the first allocation fails, in run 2 the second, and so on.

```sh
ceesafec test --sweep alloc --filter totals
```

```text
test totalsRun ... ok

ceesafec test: 1 passed, 0 failed, 0 sweep failure(s) in 2 sweep run(s)
```

`totalsRun` allocates twice, because the vector grows twice for five values, so there were two sweep runs. In each,
`push` returned an `Err`, `totals` passed it on, and the test returned `Check.fail("out of memory")`. A sweep run
is allowed to fail in that way. It is not allowed to panic, crash, time out or leak. The sweep asks one question:
when memory runs out here, does the code return an error and free what it holds?

The `outcomes` package of section 11.2 has a function for which the answer is no.

<span class="listing-caption"><strong>Listing 11-14</strong>, <code>outcomes/src/three.cee</code>: A function that unwraps every allocation with <code>!</code>.</span>

```ceesafe
/// Three lap times. It unwraps each push, so it panics if memory runs out.
fn threeLaps() -> Vector<u64> {
    var laps = Vector<u64>.create();
    laps.push(60)!;
    laps.push(62)!;
    laps.push(58)!;
    return laps;
}
```

<span class="listing-caption"><strong>Listing 11-15</strong>, <code>outcomes/tests/unwraps.cee</code>: Its test, which passes in an ordinary run.</span>

<!-- ceesafe: skip -->
```ceesafe
@test
fn keepsThreeLaps() -> Result<void, TestFailure> {
    let laps = threeLaps();
    Check.equalU64(3, laps.length as u64)?;
    return Ok();
}
```

```sh
ceesafec test --sweep alloc --filter keeps
```

```text
test keepsThreeLaps ... ok
keepsThreeLaps: allocation 1 panicked: unwrapped an Err value at src/three.cee:4:5

ceesafec test: 1 passed, 0 failed, 1 sweep failure(s) in 1 sweep run(s)
```

The test passed, and the sweep found the panic that a real shortage of memory would cause, with the number of the
allocation and the place. The exit status is 1. The fix is the shape of `totals`: return
`Result<Vector<u64>, AllocError>` and write `?`.

The sweep forgives one thing. A `!` in a test file is the test's own business, not a fault in the code under test,
so a panic from an unwrap in a file that declares a test is not a sweep failure. The last line then counts it:
`(1 unwrapped by the test itself)`.

### `--sweep cancel`

The second sweep is for async functions, which a later chapter covers. An async test can wait, and each `await`
where it waits is a point at which its task can be cancelled. This test waits twice:

<span class="listing-caption"><strong>Listing 11-16</strong>, <code>outcomes/tests/waits.cee</code>: An async test with two points where it waits.</span>

```ceesafe
@test
async fn waitsTwice() -> Result<void, AsyncError> {
    await Timer.sleep(1)?;
    await Timer.sleep(1)?;
    return Ok();
}
```

```sh
ceesafec test --sweep cancel --filter waits
```

```text
test waitsTwice ... ok

ceesafec test: 1 passed, 0 failed, 0 sweep failure(s) in 2 sweep run(s)
```

`--sweep cancel` runs each passing test again once for each such point, and cancels the test's task there. As with
`alloc`, the run may fail, and it may not panic, crash, time out or leak. A test that is not async has no such
point, so the sweep runs it no further.

::: tip Rule
`--sweep alloc` runs each passing test once more for each allocation, with that allocation failing.
`--sweep cancel` runs each passing async test once more for each point where it waits, cancelled there. A sweep run
may return an error. It may not panic or leak.

*Specification: `tooling.md` §10.2.*
:::

The sweeps take longer than a plain run, since a test with 50 allocations runs 51 times. Run `ceesafec test` while
you work, and the sweeps before you share the code.

## Mistakes

::: warning Mistake: testing a private function from another package
The second package sees what a user sees. `digit` is private to `laps`, so this test in `race` does not compile.
:::

<span class="listing-caption"><strong>Listing 11-17</strong>, <code>race-private/tests/public.cee</code>: The same test file with one more test, which calls a private function of <code>laps</code>. This does not compile.</span>

<!-- ceesafe: skip -->
```ceesafe
import laps;

@test
fn readsATime() -> Result<void, TestFailure> {
    Check.equalU64(125, seconds("2:05").ok() ?? 0)?;
    return Ok();
}

@test
fn readsADigit() -> Result<void, TestFailure> {
    Check.isTrue(digit('7').isSome(), "'7' is a digit")?;
    return Ok();
}
```

```text
tests/public.cee:11:18: error[N0200]: cannot find function 'digit' in this scope
   |
11 |     Check.isTrue(digit('7').isSome(), "'7' is a digit")?;
   |                  ^^^^^
```

Test `digit` inside `laps`, as Listing 11-11 does, or test it through `seconds`, which is public.

::: warning Mistake: a sweep in the interpreter
The sweeps use the runtime of a built program. `ceesafec test --interp --sweep alloc` is refused, with exit
status 2:
:::

```text
ceesafec: '--sweep' runs the tests natively, under the runtime's allocation and cancellation hooks: it cannot be combined with '--interp'
```

::: warning Mistake: unwrapping allocations in code under test
`laps.push(60)!` is fine in a test and in a short program. In a library it turns "out of memory" into a panic that
no caller can handle, and no ordinary test shows it. Listing 11-14 passes every test; only the sweep reports it.
:::

::: details Coming from Rust
- Tests beside the code need no `mod tests` and no `use super::*`: they are functions of the same package.
- The `tests` directory is not Cargo's. Its files are part of the package and see private items. For Cargo's
  integration tests, which see only the public interface, write a second package.
- Rust has no counterpart of the sweeps in `cargo test`. A Rust `Vec::push` aborts when memory runs out; in CeeSafe
  `push` returns a `Result`, so the path exists and the sweep can test it.
:::

## Summary

- Tests go beside the code in `src/`, or in `tests/` (or the directories the manifest's `tests` names).
- `ceesafec check` checks both. `ceesafec build` and `ceesafec run` leave both out.
- Tests see the private items of their package, wherever they are.
- To test a library as its users see it, write the tests in a second package that depends on it. A private item is
  not found there (`N0200`).
- `--sweep alloc` fails each allocation of each passing test in turn. `--sweep cancel` cancels each passing async
  test at each point where it waits. A sweep run may fail, and may not panic or leak.
- The sweeps run natively only, not with `--interp`.

Next, [chapter 12](./ch12-00-command-line-project) builds a command-line program, and
[section 12.4](./ch12-04-test-first) writes its tests first.
