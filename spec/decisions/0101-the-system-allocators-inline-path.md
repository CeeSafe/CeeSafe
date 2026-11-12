# ADR 0101 — The system allocator's inline path

- Status: **Proposed**, 2026-11-12, implemented in the working tree and not released. Raised by
  `docs/performance-investigation-2026-11.md` §4.5: `binary-trees`, `small-objects` and `linked-list` were 1.13 to
  1.28 times Rust's time, and the investigation asked for the allocation path to be measured before anything else.
- Scope: the runtime (`runtime/include/ceesafe/rt.h`, `runtime/core/ceesafe_rt_core.c`,
  `runtime/hosted/ceesafe_rt_hosted.c`), hosted profile only. No change to the language, to the compiler's output
  (the generated C calls `ceesafe_alloc` and `ceesafe_free` as before) or to what a program can observe.

## 1. Context

Every allocation of the system allocator went `ceesafe_alloc` (in `ceesafe_rt_core.c`: whether to count, whether to
fail) → `ceesafe_platform_alloc` (in `ceesafe_rt_hosted.c`: the host's hook or the C library, and the first block
noted for `CEESAFE_INIT_LATE`) → `malloc`. Two calls across C files, neither inlined, and five loads of
configuration, before the allocator; the same for a free. Counting itself is off unless asked for (ADR 0059), so in
an ordinary run all of that decides nothing.

Measured 2026-11-12 (Apple M-series, clang 21, `-O2`, the lowest of 7 runs, the generated C of each workload built
twice from the same directory):

| Workload | Through the runtime | `malloc`/`free` called directly | One word tested, then `malloc`/`free` |
|---|---:|---:|---:|
| binary-trees | 0.658 s | 0.512 s | 0.523 s |
| small-objects | 0.594 s | 0.475 s | 0.474 s |
| linked-list | 0.423 s | 0.336 s | 0.345 s |

So the path costs about 20% of these programs' time, and a test of one word keeps nearly all of what calling the C
library directly gives. Declaring `ceesafe_alloc` with `__attribute__((malloc, alloc_size(1)))` alone changed nothing.

## 2. Decision

One word, `ceesafe_alloc_watched`, is zero exactly when an allocation of the system allocator is the C library's
and nothing watches it. It has a bit per reason to take the runtime's path:

- `CEE_ALLOC_WATCH_COUNTED`: allocations are counted (the allocation report, `CEESAFE_FAIL_ALLOC_AT`,
  `ceesafec test`). Set and cleared with `alloc_counting`, by the one function that writes it.
- `CEE_ALLOC_WATCH_PLATFORM`: the platform functions are not the bare C library's. Set at the start and whenever
  `ceesafe_platform_use_allocator` runs (a host's hook, ADR 0088 §2.5, or its removal); cleared when
  `ceesafe_platform_alloc`, `_alloc_zeroed` or `_realloc` returns a block from the C library with no hook installed.
  So the first allocation of a program takes the runtime's path, which notes it (`ceesafe_rt_allocated`), and the
  later ones need not.

In `rt.h`, hosted profile, `ceesafe_alloc(size, allocator)` and `ceesafe_free(p, allocator)` are macros for two
inline functions: for `CEE_ALLOC_SYSTEM` with the word zero, `malloc(size == 0 ? 1 : size)` and `free(p)`; otherwise
the functions of `ceesafe_rt_core.c`, unchanged. The cost beside the C library's call is one relaxed load and an
untaken branch. `ceesafe_rt_core.c`, which defines the functions, removes the macros for itself.

What does not take the inline path, on purpose:

- every other allocator (`FailingAllocator`, the foreign allocators of ADRs 0060 and 0061): their identity is
  what the runtime's function acts on;
- `ceesafe_alloc_zeroed`, `ceesafe_realloc` and the vector's growth: they are in the runtime already, one call
  from the generated code;
- a package's C files, which include `ceesafe/foreign.h` alone and call the functions;
- the freestanding profile, which has no C library.

## 3. Why it changes nothing a program can observe

- **Counting and injected failures.** The word is non-zero from before `alloc_counting` is set until after it is
  cleared, so a counted run takes the runtime's path for every allocation and free, as before. An attempt fails only
  under counting (`CEESAFE_FAIL_ALLOC_AT` needs the attempt's number) or for `FailingAllocator`, which is another
  allocator.
- **A host's allocator.** While a hook is installed the platform bit is never cleared, so every block comes from and
  goes to the hook.
- **`CEESAFE_INIT_LATE`.** `ceesafe_rt_allocated` is set by the allocation that clears the platform bit, so it is
  set before any inline allocation.
- **Blocks.** Both paths use the C library's heap when no hook is installed, so a block allocated on one may be
  freed on the other. `malloc` is given at least one byte on both.
- **Threads.** The word is read relaxed and changed with an atomic or/and. A thread that a C constructor started
  may allocate inline while `ceesafe_init` turns counting on; that allocation is not counted, which ADR 0088 §8
  already says of the runtime's own path ("which only the report would notice").

A C compiler that sees `malloc` and `free` may remove a pair it proves unneeded. That is the C compiler's
optimisation of a path on which nothing counts allocations; a counted run does not take it.

## 4. Tests and measurements

- The contract suite and the run corpus with `--check-leaks` (counted runs: the runtime's path) and without (the
  inline path), and `tools/fault_sweep.py`, whose injected failures need the runtime's path.
- `benchmarks/run.py`, before and after: `guide/performance.md`.
