# ADR 0096 — Values initialised once: `OnceCell` and `OnceLock`

- Status: **Accepted** by the owner, 2026-11-11 ("I accept both ADRs"), every question as proposed: both types, the names `OnceCell` and `OnceLock`, `OnceLock<T>.constant()` for
  statics, no kernel variant now. **Implemented, with two differences** (§6): `tryInit` in place of `getOrTryInit`,
  and no `constant()` yet. Raised by the port of two tsrs crates
  (`~/dev/CeeSafeApps/bun/analysis/trials/tsrs/REPORT.md` §4, gap 1).
- Scope: `lib/core/cells.cee`, `lib/std/sync.cee`, `spec/type-system.md` §38 and §41, the guide (`threads.md`,
  `builtins.md`, `coming-from/rust.md`). No change to the checker, the emitter or the grammar.
- Release: the next minor release, in one stage.

## 1. Context

- **What the port met.** tsrs's span map builds an index on its first original-to-virtual lookup and never again:
  `original_index: OnceLock<OriginalIndex>`, read through `&self` from any thread. Most maps are never asked, so the
  index is not built with the map.
- **What CeeSafe offers today, and why none fits:**
  - **Build it with the value.** Changes the cost: every map pays for an index few use.
  - **`Mutex<T?>`.** Every read takes the lock, and the value is reached only through the guard, so a method cannot
    return a `borrow T` that lasts as long as its `borrow this`.
  - **`RefCell<T?>`.** The same guard problem, and a cell is never `Share` (§41), so the struct cannot be shared
    between threads at all.
  - **Atomics and a `RawCell`, by hand.** Not available to user code: a type that reaches a `RawCell` is `Share`
    only through a library `@threadSafe` type (§38.1, §41.1).
- **What the port did.** The `OnceLock` stayed in the Rust shim, and the index became a second CeeSafe object. A
  port of all of the crate could not have done that.
- **Rust** has `std::cell::OnceCell` (one thread) and `std::sync::OnceLock` (shared). They are common in Rust code
  bases: lazily built indexes, caches, and globals.

## 2. Proposal

Two library types. Each holds nothing or one `T`, is written at most once through a shared reference, and from then
on lends `borrow T` for as long as the cell is borrowed.

| Type | Where | Shared between threads |
|---|---|---|
| `OnceCell<T>` | lib/core, every profile | no: a cell, as `Cell` and `RefCell` (§41) |
| `OnceLock<T: Send + Share>` | lib/std, hosted | yes: `@threadSafe`, as `Mutex` and `Arc` |

The same members on both:

| Member | Type | Does |
|---|---|---|
| `OnceCell<T>.create()` | the cell | an empty cell |
| `c.get()` | `borrow T?` | the value, or `null` while the cell is empty |
| `c.getOrInit(init)` | `borrow T`; `init: borrow mut fn() -> T` | the value; when the cell is empty, runs `init` and stores its result first |
| `c.getOrTryInit(init)` | `Result<borrow T, E>`; `init: borrow mut fn() -> Result<T, E>` | as `getOrInit`; on `Err` the cell stays empty and the error is returned |
| `c.set(value)` | `Result<void, T>` | stores `value` in an empty cell; a full cell gives the value back |
| `c.getMut()` | `(borrow mut T)?` | the value, through an exclusive borrow of the cell |
| `c.take()` | `T?` | the value, leaving the cell empty; through an exclusive borrow |
| `c.intoInner()` | `T?` | the value, the cell consumed |

- **Why a value never changes under a `borrow T`.** No method writes a full cell through a shared reference: `set`
  refuses, and `take` and `getMut` need the cell exclusively, when no `borrow T` from it is live.
- **`getOrTryInit` is not optional here.** Allocation is fallible in CeeSafe, so most initialisers return a
  `Result`.
- **`OnceLock`, between threads.** One thread runs `init`; the others that arrive meanwhile wait and then read the
  value. Once the cell is full a read is one atomic load and takes no lock. `getOrInit` and `getOrTryInit` may sleep,
  so an `async fn` cannot call them (N0295, §40.1); `get` has no effects.
- **An initialiser that asks its own cell.** `OnceCell`: a panic, "cell initialised recursively". `OnceLock`: the
  same panic when the asking thread is the one running `init`.
- **In a `static`.** `OnceLock<T>.constant()`, an empty cell in a static's initialiser, as `AtomicU64.constant`
  (§39.4): the lazily built global. `OnceCell` is not `Share`, so it is not allowed in a `static` (N0337).
- **Drop.** The cell drops its value, if it has one.
- **How they are built.** `OnceCell`: a `RawCell<T?>` and a `Cell` for the running state. `OnceLock`: a `RawCell<T?>`,
  an `AtomicU8` state (empty, running, full) and the runtime's mutex for the waiters. Library code only.

## 3. Alternatives considered

- **`OnceLock` only.** One type, usable everywhere. It needs the hosted runtime's lock, so freestanding and kernel
  code would have none, and single-threaded code would pay an atomic load.
- **`Lazy<T>`: the initialiser stored in the cell.** Rust's `LazyLock`. The initialiser cannot then use the value
  that holds the cell (the span map's index is built from the map's own segments), so `getOrInit` is the form the
  port needs. `Lazy` can be added later on top of these.
- **A language feature: `lazy` fields or statics.** More than the case needs; the library types cover it with rules
  the language already has.
- **Nothing.** The Rust shim keeps the `OnceLock`. That is a workaround only while a Rust side exists.

## 4. Testing

- **Corpus, run-pass:** each member on both types, native and interpreted; `getOrInit` runs `init` once over many
  calls; `getOrTryInit` leaves the cell empty after an `Err` and fills it on the next `Ok`; a value with a `drop`
  block is dropped once, with the cell, and `take` leaves nothing to drop.
- **Threads:** many threads call `getOrInit` on one `OnceLock`; `init` runs once and every thread reads the same
  value. Under ThreadSanitizer: no report.
- **Corpus, check-fail:** a `OnceCell` shared between threads (N0334) and in a `static` (N0337); `take` through a
  shared reference; `getOrInit` on a `OnceLock` in an `async fn` (N0295).
- **Panics:** the recursive initialiser, on both types.
- **The port:** `tsrs_spanmap`'s index moves into the CeeSafe `SpanMap`, the shim's `OnceLock` goes, and the trial's
  tests and timings are rerun.

## 5. Questions for the owner

1. **Both types, or `OnceLock` only?** **Proposed: both**, so freestanding code has one and single-threaded code
   pays nothing.
2. **The names `OnceCell` and `OnceLock`.** Rust's. **Proposed: yes**: a port keeps the names its source uses.
3. **`OnceLock<T>.constant()` for statics in this ADR?** **Proposed: yes**: it is one `@constant` function, and
   lazily built globals are the other common use.
4. **A kernel `OnceLock`** over lib/kernel's lock. **Proposed: not now**: no port has asked for it.

## 6. Implementation notes (2026-11-11)

On branch `impl/0096`, on 0.34.0 with ADRs 0093 and 0097.

- **`OnceCell<T>`** is in `lib/core/cells.cee`: a `Cell<u8>` state (empty, running, full) and a `RawCell<T?>`.
  **`OnceLock<T: Send + Share>`** is in `lib/std/sync.cee`, `@threadSafe`: an `AtomicU8` state and a `RawCell<T?>`.
- **Different from §2: `tryInit`, not `getOrTryInit`.** `Result<borrow T, E>` cannot be written for a method with two
  reference parameters, the cell and the closure: the result must say which it is borrowed from, and a source list
  is allowed only on a reference that is the whole return type (N0245, N0246). The receiver cannot be given a region
  of its own either. So `tryInit<E>(init)` returns `Result<void, E>`: it fills the cell or returns the error, and
  `get()!` or `getOrInit` then lends the value. `getOrInit` is as proposed, with `borrow(this) T`.
- **Not done: `OnceLock<T>.constant()`** (question 3). The storage is a `T?`, and an optional is not a static
  initialiser (§39.4, "not in v1"). A static `OnceLock` needs either that rule extended to `null` or storage that is
  not an optional; neither is in this ADR. Found on the way: an N0338 inside a library `@constant` body made the
  diagnostic printer panic (`src/support/source.cee:27`), which is not fixed here.
- **The lock.** A `OnceLock` holds none, so `create()` neither allocates nor fails. Initialisers run under one lock
  for the process, new in the runtime: `ceesafe_mutex_once_lock` and `ceesafe_mutex_once_unlock`
  (`runtime/hosted/ceesafe_rt_thread.c`), a static mutex made recursive by its owner's identity, so an initialiser
  may fill another `OnceLock`. Waiters block on it. The cost: initialisers of different cells do not run at the same
  time. A lock or a wait queue per cell would lift that, and can replace this without changing the type. CosmoOS
  gives the runtime no thread identity, so there the lock is not recursive.
- **The interpreter** models the lock as a mutex at a fixed address, with a depth for its owner.
- **Inference.** `E` of `tryInit<E>` is not inferred from a closure that returns `Err(…)` alone, or from a call's
  result in some positions: the call writes it.
- **Not done from §4:** the port's `tsrs_spanmap` index has not been moved into the CeeSafe `SpanMap`.
