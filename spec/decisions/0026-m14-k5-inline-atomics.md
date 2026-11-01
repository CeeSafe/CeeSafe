# ADR 0026 — M14 K5: inline atomics, and atomics in every profile

> **Inherited.** ADR 0026 was accepted before CeeSafe's fork (Decision 0006), when the language had its
> earlier name. In this record, "the reference" is that language and project and "the reference compiler" its compiler:
> read them as CeeSafe and `ceesafec`. Links to documents CeeSafe did not keep are given as text.

- Status: **Accepted** (project owner, 2026-10-29), both questions as recommended (§5). The wording is the agent's.
- Scope: OD-27 (M14 K5's first item). How the atomic types' operations reach the machine, and which profiles have
  them.

## 1. Context

- **Today** (ADR 0016, ADR 0021): `AtomicU64`, `AtomicU32`, `AtomicUSize`, `AtomicBool` and `Ordering` are in
  `lib/std`, a hosted-only library. Each operation calls a runtime function in `runtime/hosted/ceesafe_rt_thread.c`
  (`ceesafe_atomic_add_u64`, `ceesafe_atomic_load_ord_u32`, …). The ordered ones take the ordering as an `int` and switch
  on it.
- **The cost of the call** (ADR 0021 §1, `benchmarks/atomics`, Ryzen box, x86-64): the ring with acquire/release takes
  0.289 s through the runtime calls and 0.158 s written in C with inline `__atomic_*`, a further 1.8×. Sequentially
  consistent inline C was no faster than the calls (2.761 s against 2.170 s), because the fence dominates.
- **The kernel has no the reference atomics.** `lib/std` is not in a freestanding build, and `lib/kernel` (ADR 0025) has none.
  Roadmap 2.0's SA track converts CosmoOS's C, which uses the GCC builtins directly: 1,233 `__atomic_*` calls in 98
  files (`__atomic_load_n` 593, `__atomic_store_n` 393, `__atomic_fetch_add` 141, …; counted 2026-10-29 in
  `kernel/` and `drivers/`). A converted subsystem needs atomics of its own.
- **No library calls.** CosmoOS builds AArch64 code with `-mno-outline-atomics`, and so does the reference's freestanding
  profile (`profiles.md` §4.3). With it, gcc and clang expand every 8-, 16-, 32- and 64-bit `__atomic_*` on
  both architectures inline, with no `libatomic` or `__aarch64_*` helper (inferred from CosmoOS's own use; checked
  per architecture by the implementation, below). MSVC has the `_Interlocked*` intrinsics, already used by the
  runtime.

## 2. Options

| | A. Keep the calls | B. Inline in the runtime header | C. Built-in operations in the emitter |
|---|---|---|---|
| How | as today | the `ceesafe_atomic_*` functions become `static inline` in `rt.h`; the ordering switch folds away where the ordering is a constant | the emitter writes `__atomic_*` with the ordering constant at each call site |
| Compiler change | none | none: the runtime and its header only | a new intrinsic kind in NIR and the emitter |
| An ordering not known at compile time | works | works: the switch stays | needs a fallback call |
| `-O0` | a call | a call to the inline function, with its switch | inline |
| Gain at `-O2` | — | as C (expected; to be measured) | as C |

B gives what C gives at `-O2`, where the ordering at a call site is a constant (it is in every `…With` call written
with an enum constant, after inlining), with no change to the compiler.

## 3. Decision

1. **B: inline atomics in the runtime header.** `rt.h` defines the `ceesafe_atomic_*` functions as `static inline`, with
   `__atomic_*` (gcc and clang) or `_Interlocked*` (MSVC), and `ceesafe_rt_thread.c` loses them. Nothing else in the
   generated C changes.
2. **Atomics in every profile.** `AtomicU64`, `AtomicU32`, `AtomicUSize`, `AtomicBool` and `Ordering` move from
   `lib/std` to `lib/core`, so freestanding and kernel code has them, with the same API (type-system §38.3). Their
   `Send`/`Share` argument (`@threadSafe`) is unchanged. `Arc`, `Mutex` and `Thread` stay in `lib/std`: they need
   the platform's threads and allocator.
3. **The freestanding profile's rule is checked.** A test compiles a program using every atomic operation with the
   freestanding profile's flags for both CosmoOS architectures and requires no undefined symbol (as C4's checks do,
   `c4.py`).

## 4. Consequences

- `lib/core`'s names are reserved in every package: a freestanding package that declares its own `AtomicU64` stops
  compiling (none of CosmoOS's four ports does).
- The kernel's CeeSafe code uses the same atomics as userland CeeSafe code, compiled as CosmoOS's C compiles its own.
  `lib/kernel`'s future reference counts and flags use them.
- `benchmarks/atomics` is rerun on the Ryzen box after the change: the reference acquire/release row against the C inline
  row, and the sequentially consistent rows, under clang and gcc. The result goes into §6 and `docs/benchmarks.md`.
  If the gap to C does not close, C (built-in operations) is reconsidered.
- **Not decided here:** atomics on pointers and on 8- and 16-bit integers (add as converted code needs them), and
  fences as a library function (`atomicFence(order)`), which CosmoOS's C uses 22 times.

## 5. Questions for the owner

1. Inline atomics through the runtime header (B)? (Recommended.)
2. Move the atomic types and `Ordering` to `lib/core`, so kernel code has them? (Recommended.)

## 6. Implementation and results (2026-10-29)

- `ceesafe/rt.h` defines the `ceesafe_atomic_*` functions as `static inline`: `__atomic_*` with constant orderings in a
  switch (gcc, clang), `_Interlocked*` intrinsics (MSVC). `ceesafe_rt_thread.c` no longer has them, and a program that
  uses only atomics no longer links it. The interpreter is unchanged (`CLibrary.cs`).
- `lib/core/atomics.cee` has `AtomicU64`, `AtomicU32`, `AtomicUSize`, `AtomicBool`, `LoadOrdering`, `StoreOrdering`
  and `Ordering`, moved from `lib/std/sync.cee` unchanged (doc comments added where lib/core requires them).
  Type-system §38.3 and §38.5 say so.
- The atomics' `Result` types would have been emitted in every program, because a generic instance made by lib/core's
  own code counted as the program's. Now an instance made only by a library's code (lib/core, as lib/std and
  lib/kernel already were) is emitted only where the program uses it. The `emit-c` snapshots did not change.
- **Freestanding** (§3.3): `integrations/cosmoos/k3/atomics.cee` calls every operation of every type, with every
  ordering kind, from statics. `k3.py` compiles it with CosmoOS's module flags on both architectures, one file and
  split: the only undefined symbol is the kernel's `panic`, so no operation calls a helper. On AArch64 the object
  holds the expected instructions: `ldaxr`/`stlxr` for acquire-release and sequentially consistent read-modify-writes,
  `ldxr`/`stxr` for relaxed ones, `ldar` and `stlr` for loads and stores.
- **Results.** `benchmarks/atomics` on the Ryzen box (as ADR 0021 §6: pinned to CPUs 1 and 2, median of 9,
  interleaved; load average 0.7–2.7):

  | | clang 18 | gcc 12 |
  |---|---|---|
  | The reference, acquire/release, through calls (ADR 0021 §6) | 0.366 s | 0.442 s |
  | **The reference, acquire/release, inline** | **0.215 s** | **0.195 s** |
  | C, acquire/release through calls | 0.296 s | 0.297 s |
  | C, acquire/release inline | 0.160 s | 0.157 s |
  | The reference, sequentially consistent | 2.508 s | 2.584 s |
  | C, sequentially consistent inline | 2.816 s | 2.116 s |

  The ordered ring is 1.7× (clang) and 2.3× (gcc) faster than through calls, and within 1.34× and 1.24× of inline C.
  The reference's ring is now faster than C's through calls. Sequentially consistent rings are dominated by their fences, as
  before, and vary by tens of percent between runs. Option C (built-in operations) is not needed for this gap: what
  remains is the ring's own code (inferred: the indexing and its checks around the atomics), not the atomics.

