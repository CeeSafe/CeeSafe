# ADR 0049 — `ceesafec test`: declared tests, a generated harness, a runner

- Status: **Accepted** by the owner, 2026-11-02 ("Yes, I accept ADR 0049 with your recommendations"): every
  question of §6 as recommended. Asked for by the owner, from "CeeSafe Testing Framework — Research, Architecture,
  and Roadmap" (2026-11-02), whose direction this follows: tests discovered by the compiler, a generated harness, a
  small assertion library, a runner. CeeSafe.Web's upstream document lists `ceesafec test` as its optional item T2.
- Scope: the core only. A test attribute, the `test` command and its runner, `lib/std`'s checks, per-test leak checks
  and sweeps. Parameterised cases, fixtures, property tests, JUnit output, benchmarks and a freestanding runner are a
  later ADR (§4).

## 1. Context

CeeSafe has no test command (`guide/ceesafec.md`). Its own tests are the corpus, whose files carry their expectations
(`// expect-stdout:`, `// error[N0100]`, snapshots), and Python tools around it. A package author has neither:
CeeSafe.Web wrote its own runner (`tools/run_tests.py`), one program per test directory, comparing printed output.

What CeeSafe already has, and a test command should use rather than rebuild:

- **Runtime hooks, per run:**
  - `CEESAFE_ALLOC_REPORT` counts live allocations at exit;
  - `CEESAFE_FAIL_ALLOC_AT=k` fails the k-th allocation;
  - `CEESAFE_CANCEL_AT=k` cancels the root task at the k-th suspension.

  `tools/fault_sweep.py` and `tools/cancel_sweep.py` drive them, on packages too (T1).
- **The interpreter:** one thread, a virtual clock, a run order fixed by a seed (type-system.md §40.8). An async test
  in it is deterministic, and a panic there is an interpreter stop, not the end of the process.
- **Annotations** (`@effects`, `@export`, `@cfg`, …) and an attribute checker that rejects unknown ones (N0290).

Three facts about the language decide the shape:

- **A panic ends the process natively**: a failed `!`, an index out of bounds, a `Process.exit`. A native runner cannot
  continue in the same process after one, so each test runs in a process of its own.
- **There is no `==` on a type parameter, and no equality interface in lib/core** (generic impls are future work,
  U10). A generic `equal<T>` cannot be written yet, so the checks are typed.
- **`Async.run` is sync only.** An async test is run by the harness, not by the test.

## 2. Options

**How a check fails.**

| | A. Checks return `Result` (proposed) | B. Checks panic |
|---|---|---|
| What | `Check.equal…(…)?` returns `Err(TestFailure)` from the test | a failed check aborts, as `assert` does in C |
| Cost | `?` after each check, as for any error | every failure is a crash: no message beyond the panic's, and in the interpreter, whose stops are recoverable, still one test per process to be uniform |

**A is proposed.** It is how CeeSafe code fails already. A test's cleanup runs (its locals are dropped as it returns),
so a leak check after a failing test still means something. A panic is still caught, as a crash of its own process.

**Where the runner is.** In `ceesafec` (proposed), or a library a package depends on. The runner starts processes,
measures time, and reads the runtime's reports; `ceesafec` does all three for `build` and `run` already, and every
package gets it with no dependency.

## 3. Proposed decision

### 3.1 Declaring tests

```ceesafe
@test
fn parses_a_request_line() -> Result<void, TestFailure> {
    let parsed = parseLine("GET / HTTP/1.1".bytes());   // its own error type: checked, not `?`-ed into TestFailure
    Check.isOk(borrow parsed, "the request line parses")?;
    let line = parsed!;
    Check.equalText("GET", line.method)?;
    Check.isTrue(line.version == 11, "HTTP/1.1")?;
    return Ok();
}

@test
async fn times_out_an_idle_read() -> Result<void, AsyncError> {
    // ...
    return Ok();
}

@test
fn plain() {
    // a void test fails only by panicking
}
```

- `@test` goes on a free function with no parameters and no type parameters, sync or async. It returns `void` or
  `Result<void, E>` for any `E`. Anything else is **N0348**: on a method, with parameters, generic, or returning
  another type.
- **Where tests are.**
  - In the package's sources, beside the code they test, and in its **test directories**: the manifest's
    `tests = ["tests"]`, by default `tests` when that directory exists.
  - Test directories are part of the same package, so a test sees the package's private items. Tests limited to the
    public API, as a dependent package sees it, are a later addition.
  - A test function's name is its identity: `module.function` for one in a module, the function's name otherwise.
- **Build and check.**
  - `check` checks the test directories too, so tests do not rot unseen.
  - `build` leaves them out, and emits no `@test` function. A test cannot change a release build.

### 3.2 The checks (lib/std, `testing.cee`)

```ceesafe
public struct TestFailure {           // what a failed check returns; its message is shown with the test's name
    public message: Vector<u8>;
    public fn create(message: StringView) -> TestFailure;
}

public struct Check {
    public fn isTrue(condition: bool, what: StringView) -> Result<void, TestFailure>;
    public fn isFalse(condition: bool, what: StringView) -> Result<void, TestFailure>;
    public fn equalU64(expected: u64, actual: u64) -> Result<void, TestFailure>;    // "expected 3, got 4"
    public fn equalI64(expected: i64, actual: i64) -> Result<void, TestFailure>;
    public fn equalText(expected: StringView, actual: StringView) -> Result<void, TestFailure>;
    public fn equalBytes(expected: Slice<u8>, actual: Slice<u8>) -> Result<void, TestFailure>;  // the first difference
    public fn isOk<T, E>(r: borrow Result<T, E>, what: StringView) -> Result<void, TestFailure>;
    public fn isErr<T, E>(r: borrow Result<T, E>, what: StringView) -> Result<void, TestFailure>;
    public fn fail(message: StringView) -> Result<void, TestFailure>;
}
```

- A message is built into an owned `Vector<u8>`, so a failure borrows nothing from the test.
- **Other error types.** A test that returns `Result<void, E>` for another `E` (`AsyncError`, `IoError`, its own) fails
  with that error. The runner shows `IoError`'s and `AsyncError`'s message, and for any other type the variant's name
  where the generated code knows it.
- Generic `equal`, and messages for any type, wait for an equality and a text interface (U10).

### 3.3 The command

```text
ceesafec test [--filter TEXT] [--list] [--interp] [--jobs N] [--timeout SECONDS]
              [--sweep alloc|cancel] [--format text|json]
```

- **Building.** It builds the package's test harness once: the sources, the test directories and a generated entry
  point. Then it runs each test, filtered by `--filter` (a substring of the name), in the name's order.
- **Results.**
  - Text: one line per test (`ok`, or `FAILED` with the failure), a summary, and exit status 1 if any failed.
  - `--format json`: one document, as `check --diagnostics-format=json` does. Each test gives its name, its outcome,
    its time, its failure message, and its captured output.
- **Isolation.** Each test runs as its own process: the harness started with `--ceesafe-test <name>`. Its standard
  output and error are captured and shown only when it fails. `--jobs N` runs N at once (default 1).
- **Timeouts.** A test that runs past `--timeout` (default 60 s) is ended and counts as timed out. That needs the host
  library to wait for a process with a limit, and end it: a small addition to `Process` (§5).
- **Outcomes,** each reported as such:

  | Outcome | When |
  |---|---|
  | passed | the test returned `Ok` or `void` |
  | failed | it returned `Err` |
  | panicked | the runtime's panic report, with its location |
  | crashed | a signal or exception |
  | timed out | it ran past `--timeout` |
  | leaked | it passed, but allocations were still live at exit |

- **Leaks.** `CEESAFE_ALLOC_REPORT` is on for every test, so a leak fails it: CeeSafe has no garbage collector, and a
  leak is a bug.
- **`--interp`** runs each test in the interpreter, in the `ceesafec` process, one after another: a panic is a stop the
  runner catches. The clock is virtual and the run order fixed by the seed, so an async test's timing is deterministic.
  Tests that call C (`extern "C"`, a package's C files) are reported as skipped there, as `run --interp` cannot run C.
- **`--sweep alloc` and `--sweep cancel`** run each passing test again under the existing hooks:
  - `alloc`: once per allocation, with `CEESAFE_FAIL_ALLOC_AT=k`;
  - `cancel`: once per suspension point, with `CEESAFE_CANCEL_AT=k` (sampled past 200, as the sweep tools do).

  Each run must end without a panic or a leak. A failing run is reported with its k, so it can be reproduced.

### 3.4 The harness

- `ceesafec` generates the entry point, in CeeSafe's NIR, before C exists: no reflection, nothing at run time beyond a
  table.
- **The generated program:**
  - `--ceesafe-list` prints the tests' names;
  - `--ceesafe-test <name>` runs that test and exits: 0 passed, 1 failed with the failure on standard error between
    marker lines the runner reads, the runtime's own status for a panic;
  - an async test runs under `Async.run`.
- An executable package's own `main` is not run by the harness. It is still checked and compiled.
- The protocol between harness and runner is these two arguments and the marker lines. It is versioned
  (`ceesafe-test 1`) so a runner can refuse a harness it does not understand.

## 4. Later, in another ADR

- parameterised cases;
- fixtures and setup;
- tags and skips;
- property tests with shrinking;
- fuzzing with a kept corpus;
- snapshots;
- JUnit XML;
- benchmarks;
- a freestanding runner over a serial sink (CosmoOS);
- public-API-only integration tests;
- a generic `equal` once an equality interface exists.

They build on this one's harness and runner. Each needs its own design: a case table needs data the language can
write without tuples.

## 5. Consequences

- A package's tests are `@test` functions and `ceesafec test`, with no runner of its own. CeeSafe.Web can move from
  `tools/run_tests.py` to it.
- Every test is leak-checked, and can be swept for allocation failures and cancellations: the checks CeeSafe's own
  corpus has, for every package.
- **New diagnostic:** N0348 (a `@test` function's shape).
- **Host library:** `Process` gains a wait with a limit that ends the process when the limit passes. On POSIX that is
  `kill`; on Windows, `TerminateProcess`. `test` uses it, and it is public for others.
- **The compiler's own corpus is unchanged.** It tests the compiler: diagnostics, snapshots, the C. `ceesafec test` is
  for packages.

## 6. Questions for the owner

1. `@test` on parameterless free functions, sync or async, returning `void` or `Result<void, E>`, in the sources and
   in test directories (`tests = [...]`, default `tests`) that share the package's private items? Recommended: yes.
2. Checks return `Result<void, TestFailure>` and are used with `?` (option A), with typed comparisons until an equality
   interface exists? Recommended: yes.
3. Each test in its own process natively; `--interp` in-process in the interpreter, deterministic; `--jobs` default
   1? Recommended: yes.
4. A leak fails a test by default? Recommended: yes.
5. `--sweep alloc|cancel` per test, reusing the runtime's hooks? Recommended: yes.
6. Leave cases, fixtures, property tests, JUnit, benchmarks and the freestanding runner to a later ADR (§4)?
   Recommended: yes.

## 7. Implementation plan (once accepted)

1. Spec:
   - tooling.md: the `test` command, the `tests` field, the harness protocol;
   - type-system.md: `@test`;
   - diagnostics.md: N0348;
   - the API page for `testing.cee`;
   - the guide (a new page, "Testing").
2. `src/`:
   - the attribute and its checks;
   - the test directories in the workspace;
   - `build` leaving them out;
   - the generated entry point;
   - the runner: processes, timeouts, the outcomes, JSON, the sweeps, `--interp`.
3. lib/std: `testing.cee`; `Process`'s wait with a limit (runtime: `ceesafe_process_*`); then `tools/gen_library.py`
   and `tools/gen_runtime.py`.
4. Tests:
   - a fixture package with a test of every outcome: pass, fail, panic, crash (a C file that raises a signal), time
     out, leak, and an async test. `ceesafec test` must report each as itself, natively and with `--interp`;
   - `--filter`, `--list`, `--jobs 4`, and the JSON document against an expected file reviewed by hand;
   - `--sweep alloc` and `--sweep cancel` against `tools/fault_sweep.py` and `tools/cancel_sweep.py` on the same
     programs, which must agree on the number of points;
   - N0348 for each wrong shape; `build` emits no test;
   - the contract suite step, on macOS, Debian and Windows.
5. ROADMAP §10, and CeeSafe.Web's runner moved onto it as the first user.
