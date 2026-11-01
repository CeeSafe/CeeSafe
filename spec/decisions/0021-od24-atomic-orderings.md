# ADR 0021 — OD-24: weaker orderings for lib/std's atomics

> **Inherited.** ADR 0021 was accepted before CeeSafe's fork (Decision 0006), when the language had its
> earlier name. In this record, "the reference" is that language and project and "the reference compiler" its compiler:
> read them as CeeSafe and `ceesafec`. Links to documents CeeSafe did not keep are given as text.

- Status: **Accepted** (project owner, 2026-10-29, taking every recommendation in §5). The wording is the agent's.
- Scope: OD-24 (`open-decisions.md`), deferred by ADR 0016 "sequentially consistent only in v1, until a benchmark needs
  more": acquire, release and relaxed orderings for `AtomicU64`, `AtomicU32`, `AtomicUSize` and `AtomicBool`.

## 1. Context

- **Today** (type-system §38.3, `lib/std/sync.cee`): every atomic operation is sequentially consistent. Each is a call
  into `runtime/hosted/ceesafe_rt_thread.c`, which uses `__atomic_*_n(…, __ATOMIC_SEQ_CST)`, or `Interlocked*` under
  MSVC, where even a load is a compare-exchange.
- **The benchmark.** `benchmarks/atomics` passes 50 million values through a single-producer, single-consumer ring of
  1,024 slots between the two threads of `Thread.both`. It is the shape of a work queue or a log buffer. Four C builds
  of the same ring separate the cost of the ordering from the cost of the call:

  | Ring (median of 9, interleaved) | clang 18 | gcc 12 |
  |---|---|---|
  | The reference today (seq_cst, runtime calls) | 2.683 s | 2.629 s |
  | C, seq_cst inline | 2.761 s | 2.170 s |
  | C, seq_cst through calls | 1.607 s | 1.529 s |
  | **C, acquire/release (slots relaxed) through calls** | **0.289 s** | **0.291 s** |
  | C, acquire/release inline | 0.158 s | 0.157 s |

  Ryzen 5 PRO 3600 (x86-64), Debian 12, 2026-10-29, threads pinned to two cores sharing an L3 (CPUs 1 and 2), under
  `nice`, load average 1.0–2.3 on the shared box. Every program printed the same checksum. A seq_cst run varies by up
  to 40% between runs: it is dominated by the ring's two cache lines moving between cores. The acquire/release runs
  vary by 1–2%.

  On the Mac (Apple M-series, clang 21, 20 million values, 5 runs, not pinned), the orderings make no difference: 0.718
  s and 0.712 s for acquire/release, against 0.573 s for seq_cst through calls. arm64's seq_cst loads and stores are
  `ldar`/`stlr`, which acquire/release also uses. x86's seq_cst store is an `xchg`, a full barrier, which is what the
  orderings remove.
- **The need, then:** on x86-64, the commonest server target, the orderings alone make this ring 5–9 times faster,
  without changing how atomics are called.

## 2. Options

| | A. An ordering argument on new methods (proposed) | B. Inline atomics in generated C | C. Both | D. Nothing |
|---|---|---|---|---|
| Change | `loadWith(Acquire)` and friends in lib/std; the runtime gains ordered entry points | atomics become built-in operations the emitter writes as `__atomic_*` | — | — |
| Gain measured | 5–9× on the ring (x86) | none alone: inline seq_cst was no faster than calls (table) | a further 1.8× over A (0.158 s against 0.289 s) | — |
| Cost | library and runtime; no compiler change | the emitter, the interpreter, MSVC's intrinsics, `Send`/`Share` reasoning unchanged | both | — |

B gains nothing without A. C's extra step is a compiler change with its own decision, left open (§4).

## 3. Proposed decision: A

### 3.1 The library

```ceesafe
public enum LoadOrdering { Relaxed, Acquire, SequentiallyConsistent }
public enum StoreOrdering { Relaxed, Release, SequentiallyConsistent }
public enum Ordering { Relaxed, Acquire, Release, AcquireRelease, SequentiallyConsistent }

// On each of AtomicU64, AtomicU32, AtomicUSize, AtomicBool (where the operation exists today):
public fn loadWith(borrow this, order: LoadOrdering) -> u64;
public fn storeWith(borrow this, value: u64, order: StoreOrdering);
public fn swapWith(borrow this, value: u64, order: Ordering) -> u64;
public fn compareExchangeWith(borrow this, expected: u64, desired: u64, order: Ordering) -> …;   // as compareExchange
public fn fetchAddWith(borrow this, value: u64, order: Ordering) -> u64;
public fn fetchSubWith(borrow this, value: u64, order: Ordering) -> u64;
```

- The existing methods are unchanged and stay sequentially consistent, so no program changes meaning.
- **No undefined orderings.** C11 leaves a store with acquire ordering, and a load with release ordering, undefined.
  Separate enums for loads and stores make those impossible to write. A compare-exchange takes one ordering, and its
  failure ordering is derived as C11 requires: `AcquireRelease` fails with `Acquire`, `Release` fails with `Relaxed`,
  and the others fail with themselves.
- **The meaning** is C11's (the C standard §7.17.3), which the documentation states for each enum value. The reference adds no
  memory model of its own.

### 3.2 The runtime and the C

- `ceesafe_rt_thread.c` gains `ceesafe_atomic_<op>_<width>_ord` entry points taking the ordering as an `int`. Each switches on
  it, with constant orderings in the `__atomic_*` builtins, so the C compiler emits each case's own instructions.
- Under MSVC, every `Interlocked*` function is a full barrier, which is a correct (stronger) implementation of every
  ordering. So MSVC changes nothing.

### 3.3 Safety

- **Memory safety and data-race freedom are unchanged.** Every access to an atomic is still atomic, and `Send`/`Share`
  and T1–T3 (`ownership.md`) are untouched. A weaker ordering changes which values a load may return, never whether an
  access is a race.
- **What it does allow is logic bugs.** A program that publishes data with `Relaxed` instead of `Release` may read
  stale values. That was OD-24's stated safety impact, and the API keeps it opt-in.
- **Testing limits.** The interpreter runs threads one at a time, in sequentially consistent order, which is one legal
  execution of a weaker program. So it cannot find an ordering bug, and neither can ThreadSanitizer, which sees no race.
  The guide says so.

## 4. Consequences

- lib/std gains three enums and six methods per atomic type. The runtime gains the ordered entry points. Neither the
  compiler nor the language changes.
- `benchmarks/atomics` gains a reference build with acquire/release, expected near the "through calls" row.
- **Not decided here:**
  - inline atomics (B), whose further 1.8× is its own question;
  - `fence(order)`;
  - weaker orderings inside `Arc`'s count, which are a change to `unsafe` library code with its own argument.

## 5. Questions for the owner

1. Option A as above: ordered `…With` methods, existing methods unchanged? (Recommended.)
2. Separate `LoadOrdering`/`StoreOrdering` enums, so undefined orderings cannot be written? (Recommended; the alternative
   is one enum and a panic on an invalid ordering.)
3. Record inline atomics (B) as a new open decision, OD-27? (Recommended.)

## 6. Implementation and results (2026-10-29)

- lib/std (`sync.cee`) has the three enums and the `…With` methods. The runtime has `ceesafe_atomic_<op>_ord_<width>`
  (`ceesafe_rt_thread.c`, declared in `ceesafe/rt.h`), which switch to constant orderings. The interpreter treats each as
  sequentially consistent (`CLibrary.cs`). Type-system §38.3 and the threads guide describe them.
- Tests: `run-pass/atomic-orderings.cee` checks each method's result and a release/acquire message between the two
  threads of `Thread.both`, in every C configuration, ThreadSanitizer included, and in the interpreter.
- **Results.** `benchmarks/atomics` gained `spsc-ordered.cee`, the same ring with acquire/release indices and relaxed
  slots. On the Ryzen box (as in §1: pinned to CPUs 1 and 2, median of 9, interleaved, load average 1.1–2.9):

  | | clang 18 | gcc 12 |
  |---|---|---|
  | The reference, sequentially consistent | 1.663 s | 2.634 s |
  | **The reference, acquire/release** | **0.366 s** | **0.442 s** |
  | C, acquire/release through calls | 0.302 s | 0.293 s |
  | C, acquire/release inline | 0.161 s | 0.156 s |

  The reference's ring is 4.5× (clang) and 6.0× (gcc) faster with orderings, within 1.2–1.5× of C's through calls. The rest of
  the gap to inline C is OD-27.
- **The interpreter and spinning.** The interpreter switches threads only when one blocks or ends (ADR 0016 §3.8). So a
  thread that spins on an atomic, waiting for a thread that has not yet run, spins forever under `ceesafec run --interp`.
  This was already true with sequentially consistent atomics. The test puts its spinning receiver on the new thread
  for that reason.
