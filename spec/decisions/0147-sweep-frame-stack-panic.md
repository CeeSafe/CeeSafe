# ADR 0147 — A sweep's stacked awaits out of memory are counted apart

- Status: **Accepted** by the owner, 2026-11-15. Asked whether "the test sweep accepts the §3.3 panic as the fault
  script does, or keeps failing it", the owner chose "Count apart (Recommended)". Reported by CeeSafe.Web as U18
  (`~/dev/CeeSafe.Web/docs/ceesafe-upstream-work.md`, found 2026-11-03 with ceesafec 0.10.1). Implemented on branch
  `release-0.53.0-prep`.
- Scope: `ceesafec test --sweep alloc` (`src/host/test_runner.cee`), its help (`src/main.cee`), spec/tooling.md §10.2,
  guide/testing.md, ADR 0056 §3.3's note, `tools/test_command_check.py` and its fixture
  `tests/packages/sweep-frame-stack`. No change to the language, the C backend, the runtime or the interpreter.

## 1. Context

- **ADR 0056 §3.3** makes `async frame stack: out of memory` the outcome when a stacked await's chunk cannot be
  allocated (`runtime/core/ceesafe_rt_core.c:33`, `CEE_PANIC_FRAME_STACK`): memory for recursion through `await` is
  the call stack's kind, not a value, as a sync program whose stack overflows stops. Code that wants the failure as a
  value keeps `Async.boxed`.
- **The two sweeps disagreed.** `tools/fault_injection.py:68` accepts that panic. `ceesafec test --sweep alloc`
  counted every panic as a sweep failure ("it may not panic", `src/host/test_runner.cee`), except, since ADR 0075
  §2.16, an unwrap in a file that has tests. So the same program passed one sweep and failed the other.
- **Found by CeeSafe.Web** (U18): the first draft of its HTTP/2 driver called a handler through a wrapper that
  implemented `Application` over a `Box<dyn Application>`, a cycle through `dyn`, so the call was stacked. The test
  sweep reported 4 failures in 2,655 runs (allocations 82, 108 and 125). The wrapper was removed, so nothing there
  depends on the rule today.

## 2. Decision

- **Counted apart.** Under `--sweep alloc`, a run that panics with "async frame stack: out of memory" is not a sweep
  failure. It is counted, as ADR 0075 §2.16's unwraps are:
  - in the summary's parenthesis, `N stopped by a stacked await out of memory`, after `N unwrapped by the test
    itself` when there are both;
  - as `sweepFrameStackOutOfMemory` in the JSON document, beside `sweepUnwrappedByTest`.
- **`--sweep-strict`** makes it a sweep failure again, as it does an unwrap: the sweep for code that must not stop
  where memory runs out.
- **Not by file.** An unwrap is set apart only in a file with tests, because the tests' own `!` is their business. A
  stacked await's panic is set apart wherever the await is, because it is not a fault of the code: it is the outcome
  the language defines.
- **`--sweep cancel`** is unchanged: cancellation allocates nothing, so the panic cannot be its outcome.

## 3. Consequences

- `ceesafec test --sweep alloc` and `tools/fault_injection.py` agree on the same program.
- A library still sees where a stacked await can stop the process: the count is in every summary that has one, and
  `--sweep-strict` turns it into failures with their allocation numbers.
- A program that recursed through `await` and passed `--sweep alloc` only because nothing stacked still passes; one
  that failed only on this panic now passes, with the count shown.

## 4. Tests

Run on 2026-11-15, macOS arm64, `out/ceesafec` built from this tree by ceesafec 0.51.1:

- `tests/packages/sweep-frame-stack`: one test recursing 5,000 deep through `await`.
  - `ceesafec test --sweep alloc`: 1 passed, 0 sweep failures in 10 sweep runs, "(10 stopped by a stacked await out
    of memory)".
  - With `--sweep-strict`: 10 sweep failures in 10 runs, each "allocation k panicked: async frame stack: out of
    memory".
  - ceesafec 0.51.1, on the same package: 10 sweep failures in 10 runs, the behaviour this ADR changes.
- `tools/test_command_check.py --compiler out/ceesafec`: 16 tests, 7 checks (the 7th is this rule, with and without
  `--sweep-strict`, through the JSON and the summary's words), 0 problems.
