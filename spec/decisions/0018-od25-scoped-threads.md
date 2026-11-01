# ADR 0018 — OD-25: scoped threads by fork-join

> **Inherited.** ADR 0018 was accepted before CeeSafe's fork (Decision 0006), when the language had its
> earlier name. In this record, "the reference" is that language and project and "the reference compiler" its compiler:
> read them as CeeSafe and `ceesafec`. Links to documents CeeSafe did not keep are given as text.

- Status: **Accepted** (project owner, 2026-10-28, taking every recommendation in §5). The wording is the agent's. The
  decisions are in `type-system.md` §38.6 and `ownership.md` T4. §38.6 makes one point precise: the arguments must be
  closure literals or `fn()` values, since the capture rule is about captures the checker can see.
- Scope: OD-25 (`open-decisions.md`), deferred by ADR 0016 §5 question 5: threads that borrow their spawner's data.

## 1. Context

- **What v1 threads cannot do.** `Thread.spawn` takes only a move literal, or a `Box<dyn fn() -> T + Send>`, and
  every capture must be `Send` (type-system §38.2). References, slices and views are never `Send` (§38.1). So a
  program cannot hand two threads the two halves of a vector it owns. It must move the data into an `Arc` (an
  allocation, and the vector is no longer its own), copy it, or, as `benchmarks/parallel` does, give each thread
  only integers to compute from.
- **Why references are not `Send` today.** A spawned thread can outlive the block that made it: the `Thread<T>` is a
  value that can be stored, returned or dropped later. A reference it carried could then outlive its referent.
- **What the borrow checker already does.** A closure is a reference to an environment in its maker's block, and it
  carries its captures' loans (§36.2). Two closures passed to one call are both live for the whole call, so their
  captures must not conflict. Probes, run 2026-10-28 with the reference compiler 0.2.0-dev:
  - two closure arguments that capture the same local mutably are refused (**N0241**, "cannot borrow 'x' as mutable
    while 'x' is mutably borrowed");
  - one that captures it shared and one mutably are refused too (**N0241**).
- **Where the gap shows.** Divide-and-conquer over data the caller owns: sort the halves of a slice in parallel,
  sum the chunks of a vector, process the rows of an image. `splitAtMut` (M13 G5) already gives two disjoint
  `SliceMut` halves; only the thread is missing.

## 2. Options

| | A. Fork-join: `Thread.both(a, b)` (proposed) | B. Scopes with handles: `Thread.scope((s) => { s.spawn(…) … })` | C. Library-only: `parallelChunks` over slices |
|---|---|---|---|
| Model | Rayon's `join` | Rust's `std::thread::scope` | a fixed data-parallel helper |
| What a thread borrows | anything its closure captures, for the call | anything, for the scope | one chunk of one slice |
| New borrow rule | **none**: two scoped closures used only during one call | closures that leave their maker's block with borrowed captures (heap environments holding references; a move literal with scoped captures, which ADR 0015 §3.6 forbids) | none |
| New `Send` rule | references are `Send` inside the call, under conditions (§3.2) | the same | the same, for one element type |
| Number of threads | two per call; more by recursion | any, in a loop | any |
| Size | small | large | small, narrow |

**A is proposed.** It adds parallelism without adding a borrow rule: everything it needs about lifetimes is the
existing rule for closure arguments. B is the more familiar API, but it needs closures that outlive their maker's
block while holding references, which is a new kind of value (OD-10a's territory). C covers one pattern and can be
written over A in the library later.

## 3. Proposed decision

1. **`Thread.both(a, b)`**, in `lib/std` (hosted and CosmoOS userland, as `Thread`): `a` and `b` are closures with
   no parameters and a `void` result, `borrow fn()` or `borrow mut fn()` (§36.2). It runs `a` on a new thread and
   `b` on the calling thread, waits for both, and returns. Results travel through the closures' captures:

   ```ceesafe
   fn sum(values: Slice<u64>) -> u64 { ... }

   fn parallelSum(values: Slice<u64>) -> u64 {
       let halves = values.splitAt(values.length / 2);
       var low: u64 = 0;
       var high: u64 = 0;
       Thread.both(() => { low = sum(halves.left); }, () => { high = sum(halves.right); });
       return low + high;
   }
   ```

   The reference has no tuples, so a pair-returning form would need a library struct; it is left out of v1 (question 3).
   The example checks and runs today (2026-10-28) with a plain two-closure function in place of `Thread.both`: the
   two closures' captures (`halves` shared by both, `low` and `high` each changed by one) are already accepted.
2. **What may be captured.** Inside the call, `a` runs on another thread while `b` runs here, so each capture of
   `a` and of `b` must be safe to use from another thread at the same time:
   - a capture used shared needs its type to be **`Share`**, where, for this rule only, `borrow T`, `Slice<T>` and
     `StringView` are `Share` when `T` is;
   - a capture used mutably needs its type to be **`Send`**, where, for this rule only, `borrow mut T` and
     `SliceMut<T>` are `Send` when `T` is;
   - a captured closure is refused in v1 (question 4);
   - otherwise the error is **N0334**, as for `Thread.spawn`, naming the capture and the type.
   The conflicts between `a`'s and `b`'s captures are the borrow checker's, unchanged (§1): two closures cannot both
   change a value, or one read what the other changes.
3. **Why this is sound.** Both closures and every loan they carry are live for the whole call (§36.2), and the call
   returns only after the thread running `a` has ended. So nothing `a` reaches can be ended, moved or changed by
   the caller while `a` runs, and the two closures cannot reach one value in conflicting ways. With the capture
   rule, a value reachable from both threads is either read by both (and `Share`) or reached by one only (and
   `Send`). This is T1–T3 of `ownership.md` with "moved in" widened to "borrowed for a `Thread.both` call".
4. **If a thread cannot be started** (the runtime's record cannot be allocated, or the platform refuses), `both`
   runs `a` and then `b` on the calling thread. The result is the same, since each closure's effects are its own, so
   `both` cannot fail and returns `void`. A program cannot tell from its output whether `a` ran in parallel.
5. **Panics.** A panic in either closure aborts the process (OD-5), as in any thread.
6. **Effects** (§32): `sleep` and `alloc`, those of `spawn` and `join` together.
7. **The interpreter** runs `a` as a thread under its baton (`Interpreter/Threads.cs`), then `b`, then waits, so
   `ceesafec run --interp` stays deterministic.
8. **C.** A scoped closure is a pointer to an environment whose first member is its code (c-backend §2). `both`
   starts a thread on a runtime helper that calls `a`'s code with `a`'s environment (`ceesafe_thread_start`), calls `b`
   directly, then `ceesafe_thread_join`s. No allocation beyond the runtime's thread record.
9. **More than two.** Recursion: split, `both`, split again. A data-parallel helper (option C) may follow in
   `lib/std` over `both`, spec first.

## 4. Consequences

- Programs can process data they own on several cores without `Arc`, copies or `unsafe`.
- The `Send` and `Share` rules gain a scoped extension used only by `Thread.both`. Outside it, references stay
  neither, and `Thread.spawn` is unchanged.
- One OS thread per call: `both` suits coarse splits (a few per core). A thread pool, so fine-grained recursion is
  cheap, is not in this decision.
- **Not in this decision:** handles and loops of spawns (option B); results returned as a pair; captured closures;
  a thread pool.
- Benchmarks: `benchmarks/parallel` gains a variant that sums a vector the program owns, split by `both`, against
  C with pthreads over the same split, on the Ryzen box (6 cores), before any performance claim.

## 5. Questions for the owner

1. **Fork-join (A)**, rather than Rust-style scopes (B) or a slice helper only (C)? Recommended: A.
2. **The name.** `Thread.both(a, b)`, or `Thread.join(a, b)` (Rayon's name, but `t.join()` already means joining one
   thread), or `Thread.parallel(a, b)`? Recommended: `both`.
3. **Results through captures only in v1**, with no pair-returning form until the reference has tuples or a library pair?
   Recommended: yes.
4. **No captured closures in v1** (a closure captured by `a` or `b`)? Recommended: yes. Allowing them needs the
   capture rule applied through a closure's own captures, which can follow when a use needs it.
5. **Sequential fallback when a thread cannot start**, so `both` cannot fail? Recommended: yes.

## 6. Implementation plan (once accepted)

Spec first (type-system §38.6, ownership.md T1–T3, c-backend §2, the runtime), then:

- the capture rule (N0334 cases) with check-fail tests: each kind of capture, shared and mutable, accepted and refused;
- `Thread.both` in the checker, C and the interpreter; run-pass tests, including the sequential fallback under fault
  injection;
- the guide's threads page (`guide/threads.md`) and `ceesafec explain` text;
- the fuzzer generating `both` calls;
- the `benchmarks/parallel` variant on the Ryzen box;
- a CosmoOS userland run in QEMU on both arches (`c2.py`, as for M13 G4).

## 7. Implementation and results (2026-10-28)

- Spec: type-system §38.6, ownership T4, c-backend (`ceeTF_`, `ceeBOTH_`), N0334's row (`4e4576d`).
- Code (`ab9c95c`): `CheckThreadBoth` and `TypeTable.IsForkSafe` in the checker; `BuiltinFunction.ThreadBoth` through NIR
  and the effect checker (`sleep`, `alloc`); the C helpers; the interpreter's `ThreadBoth` under its baton, mirroring
  `ceesafe_thread_start`'s one allocation, so the fault-injection sweep exercises the sequential fallback. No new borrow
  rule: the probes of §1 hold for `Thread.both` too (`check-fail/thread-both-borrows.cee`).
- Found on the way, and fixed first (`e6b1a67`): a `SliceMut` field (`halves.left` of `splitAtMut`) was moved by a
  use instead of reborrowed (type-system §13.3), so a closure could not pass it on (N0251), and a second use outside
  one was N0250.
- Tests: `run-pass/thread-both.cee` (halves read and changed in parallel, recursion to four threads, a `Mutex`, an
  `fn()` value), `check-fail/thread-both.cee` (each refused capture, a closure variable, a move literal) and
  `thread-both-borrows.cee`; the guide's threads page; the fuzzer (26 of the first 200 seeds). Run 2026-10-28: the
  full suite (3,424) passes; the fuzzer's seeds 1–300 give no finding; `c2.py` runs the corpus, `thread-both`
  included, in CosmoOS on x86-64 (188 programs, the fault sweep 43 programs and 381 runs) and AArch64 (the same: 188, and 43 and 381).
- Benchmark (`benchmarks/parallel`, the `both` pair; the Ryzen box, a Ryzen 5 PRO 3600 with 6 cores, clang 18.1.8 -O2, 7 runs
  interleaved, at normal priority, load average
  about 1.2 on the shared box): 16 million values in a vector the program owns, split in halves.

  | Threads | The reference `Thread.both` | C, recursive pthreads | The reference/C | Speed-up the reference / C |
  |---|---|---|---|---|
  | 1 | 277.5 ms | 268.8 ms | 1.03 | 1.00 / 1.00 |
  | 2 | 155.1 ms | 149.2 ms | 1.04 | 1.79 / 1.80 |
  | 4 | 89.4 ms | 82.6 ms | 1.08 | 3.10 / 3.26 |

  `Thread.both` scales as the same split does in C. Both stay below linear by the same margin; filling the vector,
  which is serial, is inside the times (inferred).
