# ADR 0059 — Allocation counters only when something reads them

- Status: **Accepted** by the owner, 2026-11-03 ("I accept both ADRs with your recommendations"): every question of
  §5 as recommended. From the independent benchmark's analysis (pasted by the owner): the runtime's allocation
  counters are the whole of `binary-trees`' gap to Rust on x86-64.
- Scope: the C runtime (`runtime/core/ceesafe_rt_core.c`, `tools/gen_runtime.py`), c-backend.md's runtime section,
  testing.md (the hooks). No language, compiler or library change.

## 1. Context

Every allocation through `ceesafe_alloc` and `ceesafe_alloc_zeroed` makes two atomic additions. One is
`alloc_attempts` in `alloc_refused`; the other is `alloc_live` on success. A reallocation makes one, for the
attempt, and every free makes one, subtracting from `alloc_live` (`runtime/core/ceesafe_rt_core.c:250–303`). They are atomic because a threaded program allocates from
every thread (type-system.md §38).

They have three readers, and every one is switched on when the program starts:

- `CEESAFE_ALLOC_REPORT`, read by `ceesafe_platform_config` at the start of `main`, prints both counters at the end:
  the corpus's `--check-leaks` and `tools/frame_stack_check.py` use it;
- `CEESAFE_FAIL_ALLOC_AT` fails the n-th attempt: the fault sweep (`tools/fault_injection.py`) and the collection
  properties use it;
- `ceesafe_test_begin` (ADR 0049) turns on the report, the n-th failure and cancellation, for `ceesafec test`.

`FailingAllocator`'s arming (`failing_armed`, `failing_left`) is separate, test code that is single-threaded by its
contract.

So a release build that sets none of them pays three atomic read-modify-writes per allocation and free, for
counts nothing reads. On x86-64 each is a `lock`-prefixed instruction.

**Measured** with the benchmark's `binary-trees` (depth 18, millions of small boxes). The runtime was edited by hand
for this, in a copy of the generated C, with `CEE_COUNT` as a plain addition:

| Machine | atomic counters | plain counters | Rust |
|---|---|---|---|
| Ryzen 5 3600, Debian, gcc 12 `-O2`, pinned, the box loaded (≈7), median of 5 | 2.13 s | 1.72 s | — |
| Apple M1, clang 21 `-O2`, best of 3 | 1.29 s | 1.28 s | 1.16 s |
| Xeon VM (the benchmark's author) | 3.2 s | 1.9 s | 2.0 s |

On arm64 with LSE, uncontended atomics cost almost nothing. On x86-64 they cost a fifth to two fifths of an
allocation-heavy program.

## 2. Options

| | A. Count only when switched on (proposed) | B. Per-thread counters, summed at exit | C. As today |
|---|---|---|---|
| What | one flag, set before the first allocation, decides whether to count at all | each thread counts in its own counters, which are added to the totals when it ends | atomic counters always |
| Release cost | one load and a predictable branch per allocation and free | a thread-local access per allocation | three atomics |
| Fault injection | unchanged: when counting is on, it counts atomically as today | the n-th attempt needs one order across threads, which per-thread counts do not give | unchanged |
| Complexity | a few lines | thread exit hooks on every platform, and freestanding | none |

**A is proposed.** It removes the cost where nothing reads the counters, and keeps today's exact behaviour where
something does.

## 3. Proposed decision

- The runtime gets one flag, `alloc_counting`. It is true when `CEESAFE_ALLOC_REPORT` or `CEESAFE_FAIL_ALLOC_AT` is
  set, or when `ceesafe_test_begin` runs. It is decided before the program's first allocation and never changes
  afterwards. So a free never subtracts from a count its allocation did not add.
- When the flag is set, everything is as today: atomic counters, the n-th attempt failing, the report at the end.
- When it is clear, `ceesafe_alloc`, `ceesafe_alloc_zeroed`, `ceesafe_realloc` and `ceesafe_free` touch no counter. `FailingAllocator` still fails when armed,
  because its check does not depend on the counters.
- The interpreter writes its own report line (`src/interpreter/runtime.cee`) and is unchanged.
- If an allocation could ever happen before the configuration is read, for example from a constructor of a linked C
  library, it is simply not counted. The implementation checks that ceesafec's generated `main` reads the
  configuration first.

## 4. Consequences

- An allocation-heavy release program on x86-64 runs a fifth to two fifths faster: on the benchmark's `binary-trees`,
  at or below Rust's time.
- Every leak check and fault sweep behaves as before, because each one sets a flag that turns counting on.
- New diagnostics: none.

## 5. Questions for the owner

1. Counting only when a report, a fault-injection setting or `ceesafec test` asks for it (option A)? Recommended: yes.
2. Leave per-thread counters (B) unless a threaded test shows the atomic counters' contention matters under a
   report? Recommended: yes.

## 6. Implementation plan (once accepted)

1. `runtime/core/ceesafe_rt_core.c`: the flag, set in the configuration step and in `ceesafe_test_begin`; the counters
   behind it; `tools/gen_runtime.py`. c-backend.md's runtime section and testing.md.
2. Check that the generated `main` and the test driver read the configuration before their first allocation, hosted
   and freestanding.
3. Tests: the corpus with `--check-leaks`, the fault and cancellation sweeps, `frame_stack_check`,
   `test_command_check`, all unchanged in outcome. A run-pass program with no environment variable set, whose C is
   checked to make no atomic call in `ceesafe_alloc`'s path (`tools/` beside the contract suite).
4. `binary-trees` on Debian (gcc, pinned) and the M1, recorded in `benchmarks/independent/README.md`.
5. ROADMAP §10.

## 7. Implementation notes (2026-11-03)

- **The flag** is `alloc_counting` in `runtime/core/ceesafe_rt_core.c`. `ceesafe_rt_start` sets it from the two
  settings, and `ceesafe_test_begin` sets it too. Each `CEE_COUNT` (the attempt in `alloc_refused`, `alloc_live` on a
  successful allocation, the subtraction in `ceesafe_free`) is behind it.
- **Before the first allocation:** the generated `main` calls `ceesafe_rt_start` first (`src/backend/c_interop.cee`,
  `emitMainWrapper`). The generated test harness reads only argument views and compares strings before
  `TestRun.begin` (`src/host/test_runner.cee`).
- **The check** is `tools/alloc_counting_check.py`, a contract-suite step. It runs a program that allocates 1000 boxes
  with no setting, with the report and with a failure at the 500th attempt. It also checks that every `CEE_COUNT` in
  the emitted runtime is guarded. That source check stands in for §6's look at the assembly, which would depend on each
  compiler's output. It fails against 0.9.3's runtime, at the three unguarded counters.
- **Measured:** `binary-trees` at depth 18 on the Debian box (Ryzen 5 3600, gcc 12 `-O2`, pinned to one CPU, the box
  loaded at about 7.5, 7 runs): median 1.87 s before, 1.59 s after.
