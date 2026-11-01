# ADR 0016 — M13 G4: userland threads and data-race freedom

> **Inherited.** ADR 0016 was accepted before CeeSafe's fork (Decision 0006), when the language had its
> earlier name. In this record, "the reference" is that language and project and "the reference compiler" its compiler:
> read them as CeeSafe and `ceesafec`. Links to documents CeeSafe did not keep are given as text.

- Status: **Accepted** (project owner, 2026-10-28, taking every recommendation in §5; OD-24 and OD-25 record the
  deferred options). The wording is the agent's; the owner may revise it. Not yet implemented: the decisions go into
  `type-system.md` (a new §38), `ownership.md` (the data-race invariants), `c-backend.md`, `profiles.md` and
  the runtime before any code.
- Scope: M13 G4 (`milestone-13.md`): threads in user programs (hosted and CosmoOS userland), with a compile-time
  guarantee that safe code has no data races. The kernel keeps C7's model (ADR 0009: effects, lock guards, `KRef`).

## 1. Context

- **No threads.** Probe at `b9fa451`: `Thread.spawn(…)` is `cannot find 'Thread' in this scope` (N0200). The reference has
  no mutable globals: the only top-level data are `const` items (grammar.md §2, `ConstDecl`). So today a program's
  state is reachable only from its one thread, apart from C state reached through `unsafe` code.
- **Owning closures exist** (G3, type-system §36.5). A `move` literal owns its captures, which is what a thread body
  needs: it must not borrow its spawner's stack, which may be gone.
- **The platforms have what is needed.** CosmoOS userland libc has `cosmo_thread_start`/`join`, `cosmo_mutex_*` and
  `cosmo_cond_*` over futexes (`libc/include/cosmo/thread.h`). POSIX hosts have pthreads, and Windows has
  `CreateThread` and SRW locks. C11 `<stdatomic.h>` gives atomics on every compiler the reference supports except MSVC
  before 17.5. There, the `Interlocked*` intrinsics are used (inferred; to check on the Windows VM).
- **The kernel already has a model.** C7's lock guards, `KRef<T>` and context effects are kernel-only (§32–§34). A
  userland design should look the same where it can: a lock returns a guard that lends the data.
- **What a guarantee needs.** A data race needs two threads reaching the same memory, at least one writing, without
  synchronisation. Safe the reference already gives one writer or many readers per value, within a thread (B1–B5). Across
  threads, what remains is controlling **what may cross** to another thread, and **what may be shared** between
  threads.

## 2. Options

| | A. Marker properties, owned transfer, `Arc`/`Mutex`/atomics (proposed) | B. Message passing only (channels; no shared memory) | C. Unchecked threads (`unsafe` spawn) |
|---|---|---|---|
| Model | Rust's `Send`/`Sync`, simplified | Erlang/Go-style | C's |
| Shared state | through `Arc<Mutex<T>>`, atomics | none: values are moved through channels | anything |
| Data-race freedom | checked | checked (by construction) | none |
| Cost | two inferred properties per type; a few library types | copies through channels; no shared caches | none |
| Covers | workers, shared counters, caches, pools | pipelines | everything, unsafely |

**A is proposed**, with channels as a later library type over it. B alone cannot express a shared cache or counter
without copying. C contradicts the master prompt's safety rules.

## 3. Proposed decision

1. **Two inferred properties**, never written by users in v1 (no `unsafe impl`), and usable as bounds like `Copy`
   (`<T: Send>`):
   - **`Send`**: a value may be moved to another thread. It holds for integers, floats, `bool`, and owned types made
     only of `Send` types: structs, enums, arrays, `Box<T, A>`, `Vector`, `String`, `SlotMap`, `StableVector` and
     resources. It also holds for `Arc<T>` when `T: Share`, `Mutex<T>` when `T: Send`, and the atomics.
     - It fails for: references and every scoped type; raw pointers; imported C types; closures (`borrow fn`); a box
       or vector with a user allocator; regions; and `Thread<T>`, whose join must happen on the thread that spawned
       it.
     - `Box<dyn I>` and `Box<dyn fn…>` are `Send` only when written `dyn I + Send` or `dyn fn(…) + Send`. Coercing to
       one requires `T: Send`.
   - **`Share`**: shared references to it may be used from several threads at once. It holds for every `Send` type
     with no interior mutability (all of them in v1: the reference has no `Cell`), and for `Mutex<T>` and the atomics.
2. **Spawning.** `Thread.spawn(move () => body)` starts a thread running the move literal's body. It returns
   `Result<Thread<T>, SpawnError>`, where `T` is the body's result type.
   - The literal's captures must be `Send` (new code). Since they are moved in, the spawner keeps nothing it could
     race with.
   - `T` must be `Send` (the same code).
   - Spawning allocates the closure and the thread's stack. It has the `sleep` and `alloc` effects.
   - `Thread.spawn` takes only a move literal, or a `Box<dyn fn() -> T + Send>`. This is the one place besides
     `Box.create` where a move literal may appear (amending §36.5).
3. **Joining.** `t.join()` consumes the handle, waits, and returns the body's `T`. Dropping an unjoined `Thread<T>`
   joins it and destroys the result. There are no detached threads in v1, so a program ends only after every thread
   it started. A panic in any thread aborts the process (OD-5).
4. **Shared state** (library types, safe to use; their implementation is `unsafe` over the runtime):
   - `Arc<T>` (`T: Share`): shared ownership with an atomic count. `Arc.create(v)` returns a `Result`. `a.clone()`
     gives another owner. `borrow` of the contents is shared only. The last drop destroys `T`.
   - `Mutex<T>` (`T: Send`): `m.lock()` returns a guard lending `borrow mut T`, scoped like C7's guards (type-system
     §32.4), and the unlock is the guard's drop. `Arc<Mutex<T>>` is the usual shared, mutable state.
   - `AtomicU32`, `AtomicU64`, `AtomicBool`, `AtomicUSize`: `load`, `store`, `swap`, `compareExchange`, `fetchAdd`,
     `fetchSub`, all sequentially consistent in v1. Weaker orderings stay open (OD-24).
   - `Condvar` and channels follow as a separate increment, each spec-first.
5. **Why this is enough.** Every value is reachable from one thread (moved in) or through an `Arc` of a `Share`
   type. A `Share` type without a `Mutex` or an atomic has no way to be written through a shared reference. So two
   threads can write the same memory only through a `Mutex` or an atomic. `unsafe` code may break this, as it may
   break B1–B5. `ownership.md` gains invariants T1–T3 with this argument, and the mutation suite gains thread cases.
6. **Profiles.** Hosted (pthreads, or Win32 on Windows) and CosmoOS userland (`cosmo_thread_*`, `cosmo_mutex_*`). The
   kernel and freestanding profiles reject `Thread`, `Arc` and `Mutex` as hosted-only names (N0293). The kernel keeps C7.
7. **Effects** (§32). `spawn`, `join`, `lock` and a `Thread` drop have `sleep`. Atomic operations have none.
8. **The interpreter** runs threads deterministically: one at a time, switching only when a thread blocks (`join`,
   a contended `lock`) or ends, in spawn order. A deadlock, where every thread is blocked, is an interpreter fault.
   This keeps `ceesafec run --interp` reproducible. It means the interpreter cannot find races, but safe code has none.
9. **C.**
   - The runtime gains `ceesafe_thread_start`, `ceesafe_thread_join`, `ceesafe_mutex_*` and atomics over `<stdatomic.h>`, in
     `runtime/hosted` and as platform hooks for CosmoOS.
   - A thread's body is the boxed closure, called through its table as for `dyn fn` (§37.4).
   - `Arc<T>` is `struct { atomic count; T value; } *`. `Mutex<T>` embeds the platform mutex and `T`.

## 4. Consequences

- Programs can use several cores safely, with the same ownership rules plus two inferred properties. Nothing changes
  for single-threaded code: a type's `Send` and `Share` matter only at `spawn`, `Arc` and `Mutex`.
- **Not in v1:**
  - scoped threads that borrow the spawner's locals (`Thread.scope`; OD-25);
  - user `unsafe impl Send`;
  - weaker atomic orderings (OD-24);
  - thread-locals;
  - detached threads.
- Benchmarks: a parallel workload (for example `numeric`'s sieve over several threads) joins `benchmarks/` before any
  performance claim, on the Ryzen box (6 cores).

## 5. Questions for the owner (decided 2026-10-28: each as recommended)

1. **Marker properties with `Arc`/`Mutex`/atomics (A)**, rather than message passing only (B)? Recommended: A.
2. **Names.** Should they be `Send` and `Share` (Rust's `Sync` renamed to say what it means), or `Send` and `Sync`?
   Recommended: `Send` and `Share`.
3. **Joining on drop**, with no detached threads in v1? Recommended: yes. Nothing outlives `main`, and no handle
   leaks a thread.
4. **Sequentially consistent atomics only in v1?** Recommended: yes. Weaker orderings would wait for a benchmark that
   needs them (OD-24).
5. **Scoped threads later** (OD-25)? Recommended: yes. They need a rule for borrows that outlive a closure's call,
   which the other parts do not.
6. **CosmoOS userland in v1, with a test in QEMU** (`integrations/cosmoos/`, as for C3)? Recommended: yes.

## 6. Implementation plan (once accepted)

Spec first (type-system §38, ownership.md T1–T3, c-backend, profiles), then:

- **G4a.** `Send` and `Share` inference and bounds, with check-fail tests over every kind of type.
- **G4b.** `Thread.spawn`/`join`, the hosted runtime, the interpreter's scheduler; run-pass and fault-injection tests.
- **G4c.** `AtomicU32`/`U64`/`Bool`/`USize`, `Arc<T>`, `Mutex<T>` with guards, run under ThreadSanitizer as an extra
  C configuration.
- **G4d.** CosmoOS userland: the platform hooks, and a QEMU test on both arches.
- A parallel benchmark on the Ryzen box.
