# ADR 0129 — Which unwraps `ceesafec test --sweep alloc` sets apart: test-only code, not files with tests

- Status: **Accepted** by the owner, 2026-11-13 ("For D10, make the distinction based on execution context, not file
  boundaries. An `unwrap` executed as part of a test should not count as a `--sweep` allocation failure, even if the
  test is in the same file as production code. Test-only code is not part of the production execution path. However:
  An `unwrap` in production code must still be subject to the `--sweep` allocation-failure rules, regardless of
  whether that code is compiled or executed by a test. If a test invokes production code containing an `unwrap`, that
  production `unwrap` must still be checked. Do not exempt an entire file or module simply because it contains tests.
  Prefer compiler/runtime metadata that distinguishes test-only code from production code. Avoid relying on naming
  conventions or source-file heuristics. Add regression tests covering both cases: a test-only `unwrap` that is
  excluded, and a production `unwrap` reached during a test that remains subject to the allocation-failure checks."),
  implemented on branch `impl/0129`; not yet released. Raised by the book's authors (gaps2 D10).
- Amends: ADR 0075 §2.16 (G15) and its "Changes on implementation", item 6.
- Scope: the test runner (`src/host/test_runner.cee`), the driver (`src/main.cee`, `src/support/source.cee`: which
  source files come from the test directories); `spec/tooling.md` §10.2; `guide/testing.md`;
  `tools/test_command_check.py` and its fixture `tests/packages/sweep-unwraps`. No change to the language, the
  checker, the backends, the runtime, the harness protocol or the JSON document, and no new diagnostic.

## 1. Context

Under `--sweep alloc` a run may return the error; it may not panic, crash, time out or leak. ADR 0075 §2.16 set one
kind of panic apart: a `!` that unwraps the injected failure in the test itself, which is the test's business. As
written, that was a position "inside an `@test` function's own body". As implemented (§7 item 6), it was a position
in *any file that declares an `@test` function*, so that CosmoStore's test helpers, which unwrap, would count.

The by-file rule exempts production code. In the book's package `book/projects/ch15/tree-stops`, `insert` is the
code under test and unwraps its recursive call; its file holds the test; the sweep reported "0 sweep failure(s) …
(3 unwrapped by the test itself)", and only `--sweep-strict` showed the three panics. A `!` in production code was
never a sweep failure when its file also held tests, which is where CeeSafe's tests usually are (tooling.md §10.1:
"beside the code"). The rule also missed the other way: a helper in a file of the test directory that declares no
`@test` of its own was a sweep failure.

## 2. Decision

### 2.1 Test-only code

**Test-only code is the code of the root package that `ceesafec build` leaves out** (tooling.md §10.1, ADR 0049 §3.1).
That is exactly two things, both known to the compiler without reading a name:

1. **an `@test` function**: its whole declaration, so the closures written in it too. `build` emits no `@test`
   function, and nothing but the generated harness may call one (N0348);
2. **every file of the package's test directories** (`tests = [...]` in `[package]` of `ceesafe.toml`, by default
   `tests`): all of it, the functions without `@test`, methods and closures included. `build` does not read those
   files.

Everything else is **production code**: every other function of the root package, whoever calls it and whatever
else its file holds; a generic function's instance, which is classified by where the generic function is written,
not by who instantiated it; every dependency; and the libraries (`lib/`).

### 2.2 The rule

Under `--sweep alloc`, a run that panics with "unwrapped an Err value" or "unwrapped null" is classified by the
**position of the panic**, which is the position of the `!` that ran:

- in test-only code: **set apart**. It is counted, the summary ends `(N unwrapped by the test itself)`, the JSON
  document's `sweepUnwrappedByTest` holds the count, and it is not a sweep failure;
- anywhere else: a **sweep failure**, listed with its position, as any other panic is.

So a test's own `!` is set apart even when production code is in the same file, and a production `!` that a test
reaches is a failure even when the test is in the same file. The file is never what decides, with one exception that
is not a heuristic: a file of a test directory is test-only as a whole, because the manifest says so and `build`
acts on it.

`--sweep-strict` is unchanged: every such panic is a sweep failure. `--sweep cancel` is unchanged: it sets nothing
apart (a test that unwraps a cancellation fails that sweep, as before). The sweeps stay native only.

### 2.3 Test helpers

A helper that unwraps is test-only **when it is written in test-only code**: in a file of a test directory, or as a
closure inside the `@test` function that uses it. A helper written in a source file beside the tests is production
code, even if only tests call it: `build` checks and compiles it, and the compiler does not know that nothing else
will call it tomorrow. Its unwrap is a sweep failure. The two fixes are in §4.

A consequence of how packages are built, not new here: an `@test` function in a source file cannot call a helper in
a test directory, because `build` checks the source file's tests without the test directory. Tests that need shared
unwrapping helpers therefore live in the test directory with them. Tests of the test directory see the package's
items that are not `private`.

### 2.4 The metadata

`ceesafec test`'s first step writes `out/ceesafe-test/test-spans.txt` beside the harness, as before, now with one
line per span of test-only code: the file's source path, then the line and column of the span's first position and
of the position after its last, tab-separated. A file of a test directory is one span, from 1:1 to the line after
its last. An `@test` function elsewhere is the span of its declaration in the syntax tree, from `fn` to the end of its body. The runner takes the
`path:line:column` at the end of the panic's message and looks for a span of the same path that holds it. Paths are
compared whole (both are the source path the compiler gave the file, c-backend.md §7); the earlier match by a
common suffix is gone.

The file is the runner's own (it writes and reads it in one command), not part of the harness protocol.

## 3. Alternatives

- **By file** (ADR 0075 §7 item 6). Rejected by the owner: it exempts production code.
- **Only `@test` bodies** (ADR 0075 §2.16 as first written). It leaves no place for a helper, which is why item 6
  moved away from it. The test directory gives helpers a place, with a boundary `build` already draws.
- **Reachability: a function that is not public and that only test-only code refers to is test-only.** The compiler
  can compute it, and it would let a helper stay beside its tests in a source file. Not taken: the class of a
  function would depend on every other function of the package (one new call from production code turns sweeps
  red somewhere else); "refers to" needs a rule for each implicit call (an interface's method, `drop`, `for`'s
  `next`, a function value); a private function of a library that nothing calls yet would be exempt though it is
  production code that is merely unfinished; and the result disagrees with `build`, which compiles the helper.
  "What `build` leaves out" is one sentence and is already true.
- **A marker on a helper** (`@test` on a function that is not a test, or a new attribute). The honest form of the
  previous alternative, and the one to take if helpers beside tests in source files are wanted: `build` would then
  leave the marked function out, and a call of it from production code would be an error, as a call of a test is
  (N0348). It is a language change (an attribute, a checker rule, both backends), for something a test directory
  already does. Listed for the owner.
- **By execution, in the runtime: a panic "while a test's own frame is on top".** Every panic of a test run has the
  test's frame below it; what tells a test's unwrap from production code's is whose code the `!` is in, which is
  the position.
- **Names and paths** (`test_*`, `*_test.cee`, a directory called `tests` whatever the manifest says). Excluded by
  the decision. The test directories are the manifest's, not a name's.

## 4. Compatibility

Sweeps that passed because of the by-file rule now fail, with exit status 1 and the position of each unwrap. That is
the change asked for. What an author sees is a line per run,

```text
holdsFour: allocation 2 panicked: unwrapped an Err value at src/main.cee:11:17
```

and a smaller number in `(N unwrapped by the test itself)`. Each such line is one of two things:

1. **Production code that stops the program when memory runs out.** Return the error: give the function a
   `Result<…, AllocError>` (or its own error) and write `?` where it wrote `!`. If stopping is what the program
   wants there, the sweep's report is true and the test for it is one that does not sweep that path; there is no
   switch that hides it.
2. **A test helper in a source file.** Move it into test-only code: into a file of the test directory, with the tests
   that call it (§2.3), or into the test as a closure. Or let the helper return the error and the test unwrap it:
   `let store = openTemp()!;` in the `@test` function is the test's own.

One case moves the other way: an unwrap in a file of the test directory that declares no `@test` function (helpers
only) was a sweep failure and is now set apart.

In this repository (2026-11-13, `--sweep alloc` over the 29 packages with tests of `book/projects` and
`tests/packages`, before and after, 267 sweep runs each): one package changes, `book/projects/ch15/tree-stops`, from
"0 sweep failure(s) in 4 sweep run(s) (3 unwrapped by the test itself)" to 3 sweep failures, at
`src/main.cee:11:17`, `13:17` and `11:17`, the `!` of `insert`'s recursive calls. The other 129 runs set apart stay
set apart: each is a `!` in an `@test` function's body. The book's pages that describe the old rule are
`book/src/ch11-03-organising-tests.md` ("The sweep forgives one thing…") and `book/src/ch15-03-allocators.md` (the
`tree-stops` output and the sentence after it).

Applications outside this repository (CosmoStore, whose helpers were the reason for the by-file rule): helpers that
unwrap and sit in source files beside their tests are now reported, each at its position; fix 2 above applies.
`--sweep-strict` reports what it reported before.

## 5. Implementation

- `SourceText.testOnly` marks a file of the root package's test directories; the driver sets it where it adds those
  files to the package's sources (`Options.testFilesFrom`).
- `writeHarness` writes the spans (§2.4); `unwrapInTest` reads the panic's line and column and tests the spans.

## 6. Tests

`tools/test_command_check.py`, check 6, on `tests/packages/sweep-unwraps` (production code and tests in one file,
`src/main.cee`, and a test directory):

- set apart, and the sweep of each alone passes: an `@test` function's own `!` in the file of the production code
  (`unwrapsItself`), also when the function's body is an expression (`unwrapsInExpressionBody`); a closure's in an
  `@test` function (`unwrapsInItsClosure`); an `@test` function's in the test
  directory (`unwrapsInTestDirectory`); a helper's in a file of the test directory that has no `@test`
  (`callsHelperInTests`);
- sweep failures, each at the production position: a production function's `!` reached from a test of the same file
  (`callsProduction`) and from a test of the test directory (`callsProductionFromTestDirectory`); a generic
  production function's, instantiated by the test (`callsGeneric`); a helper's in the source file
  (`callsHelperBeside`);
- handled: `returnsIt`;
- `--sweep-strict` makes each of the five set apart a failure; `--sweep cancel` sets nothing apart; the summary's
  words; `build` builds the package without its tests.
