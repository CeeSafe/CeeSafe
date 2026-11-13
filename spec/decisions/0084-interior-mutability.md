# ADR 0084 — Interior mutability: `RawCell`, `Cell` and a checked `RefCell`

- Status: **Accepted** by the owner, 2026-11-09 ("Accept all ADRs as recommended": every question for the owner answered as recommended), the day it was proposed, for ceesafec **0.25.0** (with ADR 0086 and ADR 0085 step 1). Raised by the Bun
  port's M0 (`~/dev/CeeSafeApps/bun/ROADMAP.md` §3, M0): Bun 1.4.4 writes `Cell`/`RefCell` about 1,000 times, and its
  probe `analysis/probes/g3_cell.cee` fails on ceesafec 0.24.2 (N0204). The evidence is a census of Bun's hand-written
  Rust (§1.1, Python scripts run with `python3 -I`), eleven probes compiled and run with ceesafec 0.24.2 (§1.3), and a
  reading of the specification and the compiler's `Send`/`Share` inference (§1.4). Scripts and probes are kept in
  `~/dev/CeeSafeApps/bun/analysis/adr-evidence/0084/`. Probe p4 found a data race in safe code (a `StableVector`
  shared by two threads); commit `b7c9f41` fixed it before this ADR by making `Share` stricter than `Send` (§1.4).
  This ADR builds its cells on that rule.
- Scope: type-system.md §38.1 (`Share`: one more entry), §38.4 (`dyn … + Share`), §39.2 (statics), a new §41
  (cells); ownership.md B2 and T2; unsafe.md §2 (the `p.write` row and the pointers made from references);
  optimisations.md §2 (tracked locals); diagnostics.md (N0351, N0402, a panic kind); lib/core (a new `cells.cee`) and
  lib/std's `Mutex`; the checker (`reachesSharedChange` in `src/types/types.cee`), the interpreter;
  guide/coming-from/rust.md §"Things you will miss"; the corpus. It amends ADR 0010's pointer rule; it keeps ADR 0018
  and ADR 0024 as they are.

## 1. Context

### 1.1 What Bun writes

Counted on 2026-11-09 by `cells.py` (run as `python3 -I cells.py upstream`) over the same files as
`analysis/bun_rust_features.py`: `upstream/src/**/*.rs`, no test directories, no generated files. Unlike that script
it skips comment lines, so it finds 919 `Cell`/`RefCell` types where the feature census found 1,016.

| `Cell<T>` element | Uses | | `RefCell<T>` element | Uses |
|---|---:|---|---|---:|
| integer or float | 243 | | `Vec`, `SmallVec`, `BabyList`, FIFO | 53 |
| `bool` | 219 | | struct or other | 45 |
| enum, flags struct, other `Copy` value | 148 | | map or set | 14 |
| `Option<…>` of a non-pointer | 89 | | `Option<…>` | 11 |
| raw pointer or `NonNull` | 46 | | `String`, `Box` | 5 |
| `Option<pointer>` | 46 | | | |
| **total** | **791** | | **total** | **128** |

Only 18 of the 791 `Cell`s hold a value that is plainly not `Copy` (`noncopy.py`: a `Box`, `RefPtr`, `Vec` or owned
handle, used with `replace`/`take`); 10 cells of either kind hold a Rust reference.

| Where | `Cell` | `RefCell` | `UnsafeCell` |
|---|---:|---:|---:|
| struct field | 623 | 70 | 28 |
| thread-local (`thread_local!` or `#[thread_local] static`) | 57 | 29 | 4 |
| behind `Rc` | 0 | 4 | 0 |
| parameter, local, alias | 111 | 25 | 19 |

None is in a plain `static` or behind an `Arc`: Rust refuses both, since `Cell` is not `Sync`. The related types:
Bun's own `JsCell<T>` (407 uses in 73 files), `RacyCell` (60), `AtomicCell` (16), `ThreadCell` (8), all over
`UnsafeCell`; `OnceCell` 196, `OnceLock` 284, `LazyLock` 92; 55 `thread_local!` blocks and 19 `#[thread_local]`
statics. The calls: `.with_mut(` 841 (mostly `JsCell`), `.borrow_mut()` 168, `.borrow()` 97. For comparison the
atomics appear 1,244 times and `Mutex<` 30. Bun also writes `dyn … + Sync` 58 times (a plain grep, comments
included), 25 of them `dyn Fn(usize) + Sync` and 12 `dyn Any + Send + Sync`.

### 1.2 Why Bun needs them

One `VirtualMachine` per JS thread, reached by shared reference from everywhere, and **re-entrancy**: a host
function calls into JS, which calls another host function that changes the same object. A `&mut` cannot be held
across that call, so the fields are cells.

- `src/jsc/VirtualMachine.rs:389`: `pub is_inside_deferred_task_queue: core::cell::Cell<bool>,` and `:375`:
  `pub(crate) jobs: crate::JsCell<crate::job::JobList>,`.
- `src/runtime/timer/timer_object_internals.rs:242`: "`&Self` (NOT `&mut`) — fields are `Cell`/`JsCell` so
  re-entrant JS touching this object via another `&Self` is sound (no `noalias`)."
- `src/bun_core/output.rs:1044`: "drop the RefCell borrow before calling into the writer so a flush triggered from
  inside a print (or vice-versa) doesn't BorrowMutError." The checked borrow catches a real re-entrancy bug.
- `src/ptr/js_cell.rs:27–33`: `pub struct JsCell<T>(core::cell::UnsafeCell<T>);` then
  `unsafe impl<T> Sync for JsCell<T> {}`, which its own comment calls "a lie to the type system that we discharge by
  the thread-affinity invariant". Its `get_mut` hands out `&mut T` from `&self` unchecked.

Intrusive reference counts are the other common case: `src/http_jsc/websocket_client.rs:159`,
`pub(crate) ref_count: Cell<u32>,`.

### 1.3 What CeeSafe does today (ceesafec 0.24.2)

Probes `p1`–`p10` and Bun's `g3_cell.cee`, checked and run with `out/ceesafec` (`run --interp`, and `build` for p4
and p5):

| Probe | Result |
|---|---|
| g3 `hits: Cell<u32>` in a struct | N0204: `Cell` is not a type |
| p3 `AtomicU32`/`AtomicBool` fields, changed through `borrow Timer` | works: the atomics are today's only safe cell |
| p5 a user `UCell<T: Copy>` whose `set(borrow this)` writes through `borrow this.value as *T as *mut T` | accepted, **and** accepted as a shared capture of both closures of `Thread.both`; built natively, two threads of 200,000 increments print 204,389, 202,460, 200,000. Still accepted after `b7c9f41`: nothing in `UCell` is on §38.1's list |
| p2, p4 **safe code only**: two `Thread.both` closures `push` to one `StableVector<u32>` they capture shared | accepted on 0.24.2; 400,000 pushes leave a length of 214,183, 212,407, 210,627. Since `b7c9f41`: N0334, "a `StableVector<u32>`, which is not Share" |
| p6 a `Reader<region r, T>` guard over a counter, written in user code like `MutexGuard` | works (with `unsafe`): a library `RefCell` needs no new syntax |
| p7 a method named `borrow` | N0100: `borrow` is a reserved word |
| p8 `impl<T: Copy> W<T>` (struct `W<T>` unbounded), `fn get(borrow this) -> T => this.value` | N0251: an inherent impl takes the struct's own bounds (§21.1, ADR 0064 §1), so methods cannot be limited to Copy instances (§21.1 calls the mismatch N0267; the checker reports the body instead) |
| p9, p10 `W<borrow u32>`, and a cell of a reference set to a shorter-lived one | the generic struct is accepted; the cell's `set` is N0222 inside, since a raw pointer may not point to a type holding loans (§27.1) |

### 1.4 What the specification says

- **`Share` is no longer every `Send` type** (commit `b7c9f41`, type-system.md §38.1). Until then `isShare` returned
  `isSend`, resting on ADR 0016 §3.1's "no interior mutability (all of them in v1: the reference has no `Cell`)".
  But `StableVector.push` takes `borrow this` (§24.2), and a `dyn` box may hide anything: p4 was a data race in safe
  code. Now `Share` is `Send` without a path (fields, payloads, elements, box contents) to a value changed through a
  shared reference (`reachesSharedChange`, `types.cee:1730`): a `StableVector`, a `dyn` box (`Box<dyn I + Send>`,
  `Box<dyn fn… + Send>`), or a type parameter without a `Share` bound. The library's `@threadSafe` types stop the
  walk, so `Mutex<StableVector<T>>` is `Share`. The list is the hook this ADR needs; it also means no `dyn` box can
  be shared across threads at all, where Bun writes `dyn … + Sync` 58 times.
- **Writing behind a shared loan is invalid, even in `unsafe`.** ownership.md B2: "If L is shared, P must also not be
  mutated". unsafe.md §2: `p.write(v)` needs storage "which nothing currently borrows (no live loan of it, shared or
  mutable)", and ADR 0010's rule lets a pointer made from a reference write only while that loan could be live. So
  p5's `UCell` breaks U2: **no correct cell can be written in CeeSafe today**. `MutexGuard.data`
  (`lib/std/sync.cee:51–52`, `borrow this.lock.data as *T as *mut T`) relies on a library-only argument (T3).
- **The optimiser trusts shared loans** for its tracked locals (optimisations.md §2: "A shared loan cannot change
  it", integers, `bool`, slices, views, `Vector`/`String` lengths) and already excludes `StableVector` (§2, "their
  length grows through a shared borrow"). A `borrow T` is a C `const T *` (c-backend.md, the `borrow T` row), which C
  allows to be written through when the object itself is not `const`; a `borrow mut T` may become `T *restrict`.
- **Copy.** A struct with no `drop` whose fields are all Copy is Copy (type-system.md §14.1); `@noncopy` makes it a
  move type anyway. A generic `W<T>` is Copy at `W<u32>`.
- **Statics** must be `Share` (§39.2, ADR 0024 option A); diagnostics.md's N0337 already uses `static C: Cell = …` as
  its example. Thread-locals are "not in v1" (ADR 0016 §4). A panic is an abort, with no unwinding and no destructors
  (ownership.md, OD-5).

## 2. Proposal

Library cells over one compiler-known primitive (option A of §3).

### 2.1 `RawCell<T>`: the primitive

- A lib/core struct marked `@interiorMutable` and `@noncopy`; `@interiorMutable` is accepted only in lib/core (others:
  N0290, as for `@threadSafe`). It has `T`'s size and alignment (one field), so a cell may be a `@repr(C)` field like
  an atomic.
- **Never Copy.** Without `@noncopy`, `RawCell<u32>`, hence `Cell<u32>` and `RefCell<u32>`, would be Copy (§14.1):
  `let snap = rc;` would read the whole `RefCell` while a `CellRefMut`'s `borrow mut T` is live, a read no guard sees,
  of storage C may treat as `restrict`. So a cell, and every struct or enum holding one, is a move type, as in Rust
  (`Cell<T>` is not `Copy` there either). Nothing is lost: `c.get()` copies the value out, and a copy of a cell is
  `Cell.create(c.get())`. A struct with a cell field (§2.3's `Timer`) is moved, not copied.
- `RawCell.create(v: T) -> RawCell<T>`; `c.pointer(borrow this) -> *mut T` (safe to take, `unsafe` to use);
  `c.getMut(borrow mut this) -> borrow mut T` (safe: the borrow is exclusive); `c.intoInner(this) -> T`.
- **The one rule it changes** (amending ownership.md B2, unsafe.md §2's `p.write` and `Ptr.toBorrowMut` rows, and
  ADR 0010): the `T` inside a `RawCell` may be read and written through `c.pointer()` while shared loans of the cell,
  or of a value that holds it, are live. A shared loan freezes everything **except** the contents of a `RawCell`. A
  `borrow mut` of the cell still excludes every other access (B1 is unchanged), and the unsafe code still must not
  make two live `borrow mut T` from the pointer, nor write while a `borrow T` it made is live.
- `T` may not hold loans (a reference, view or other scoped type): N0351, "a cell of a loan-holding type". The
  raw-pointer rule of §27.1 already forbids the pointer; refusing at the type avoids the variance problem of p10 that
  Rust solves with invariance. Bun has 10 such cells; the port gives them raw pointers.
- User code may use `RawCell` (Bun's `RacyCell`, `ThreadCell`, `AtomicCell` port over it). It may not claim
  `@threadSafe`.

### 2.2 `RawCell` joins §38.1's list

- `reachesSharedChange` gains one case: a `RawCell<T>` is a value changed through a shared reference. So a type is
  **not `Share`** when a path through its fields, payloads, elements or box contents reaches a `RawCell`, unless the
  path first passes through a `@threadSafe` type (whose own `unsafe` code argues T3). It is still `Send` when `T` is: a
  cell may move to another thread, not be used from two at once. No other change to `Share` is needed: `b7c9f41`
  already made it stricter than `Send`.
- `Mutex` and `Arc` stay `Share` by `@threadSafe`. `Mutex`'s `data` field becomes a `RawCell<T>`, so
  `MutexGuard.data` meets U2 instead of a special argument. This refuses nothing new: `Mutex<T: Send>` already
  excludes every loan-holding `T` (N0334 and N0274 on `Mutex<borrow u32>`), so N0351 cannot fire there.
- Consequences through existing rules, with no new check: `Thread.both` refuses a cell captured shared (N0334, §38.6);
  so does `Async.blocking`; `Arc<Cell<u32>>` fails `Arc`'s `T: Share` bound; `static X: Cell<u32>` is N0337 (§39.2,
  as diagnostics.md already shows). ownership.md T2's sentence "A v1 type can be changed through a shared reference
  only if it is one of the library's `@threadSafe` types" gains "or holds a `RawCell` or a `StableVector`, and such a
  type is not `Share`".

### 2.3 `Cell<T>`

```ceesafe
struct Timer {
    flags: Cell<u32>;
    inCallback: Cell<bool>;
}

fn fire(t: borrow Timer) {
    t.inCallback.set(true);
    callIntoScript(t);                 // may call fire(t) or cancel(t) again: both take `borrow Timer`
    t.inCallback.set(false);
    t.flags.set(t.flags.get() | 1);
}
```

- `Cell<T: Copy>`: `Cell.create(v)`, `c.get()`, `c.set(v)`, `c.replace(v) -> T`, `c.getMut()` (`borrow mut this`),
  `c.intoInner()`. Every method but the last two takes `borrow this`.
- `T` is bounded `Copy` on the struct, because an inherent impl cannot add a bound of its own (§21.1, probe p8). The 18
  non-Copy cells of §1.1 (`Cell<Option<Box<…>>>` used with `replace`/`take`) port to `RefCell<T?>`, whose `replace`
  and `take` (§2.4) never find a guard live at those sites. Conditional methods would let one `Cell` serve both, but
  they change §21.1 for every library; they are not needed for 2% of the cells.
- No method lends a reference to the inside, so nothing can observe a change through a stale reference, and a
  re-entrant call between two methods is harmless. A Copy `T` has no `drop`, so `set` runs no user code.
- **`==`.** `Cell<T>` has `==` and `!=` when `T` has them, comparing `a.get() == b.get()`, so ADR 0087's
  `@derive(Equal)` includes `Cell` fields, as Rust derives `PartialEq` through `Cell`. `RefCell` has no `==`: it would
  take a read guard and could panic.
- Plain library code over `RawCell`, with methods small enough to inline: `get` is a load, `set` a store. Cells are
  never tracked locals (optimisations.md §2 is restated to say so), and `borrow` stays `const T *` in C.

### 2.4 `RefCell<T>`: checked shared and exclusive access

- `RefCell.create(v)`. Its state is one `Cell<isize>`: 0 free, `n > 0` readers, −1 writing.
- `c.read()` returns a `CellRef<T>` and `c.write()` a `CellRefMut<T>`. (`borrow` is a reserved word, p7, so the port
  maps `.borrow()` to `.read()` and `.borrow_mut()` to `.write()`.) Each guard is a scoped struct over a region of the
  cell, written like `MutexGuard` (p6 shows the shape): `data(borrow this) -> borrow T`, and
  `data(borrow mut this) -> borrow mut T` on the exclusive one. Its drop gives the access back.
- **On conflict it panics**: `write` while any guard is live, or `read` while a `CellRefMut` is, panics with "cell
  already borrowed" (a new panic kind, `CEE_PANIC_CELL_BORROWED`, for freestanding programs). Panic is abort (OD-5):
  deterministic, reported at the call, like an index out of bounds. A reader count that would overflow panics too.
- `c.tryRead()` and `c.tryWrite()` return the guard or `null`, for code that can wait or fall back (Bun's
  `output.rs:1044` case). `c.replace(v)` and `c.take()` (on `RefCell<T?>`) panic as `write` does; they move the old
  value out before returning it, so its `drop` runs in the caller with no guard live, and may re-enter the cell or
  free its owner. `getMut` and `intoInner` need no check.
- **Guards made in a condition.** A guard that is a temporary of an `if` or `while` condition, or of an `e is P`
  scrutinee (ADR 0085 §2.8), must end where Rust 2024 ends it: at the end of the condition, or, when a binding of `P`
  borrows from it, at the end of the block that uses the binding (the `if`'s block, one pass of the `while` body),
  never in the `else` block nor across the next test. Otherwise `if (c.read().data().isEmpty()) {…} else
  { c.write()… }` panics on a program that reads as correct. ADR 0085's temporaries rule should say so; if it keeps
  "until the end of the statement", a guard temporary in a condition is a warning beside N0402.
- **Why it is sound.** A guard is scoped to the cell (§25.4), so it cannot outlive it, be stored past it or cross a
  thread (scoped types are not `Send`). The reference `data` lends is tied to the guard (§13.5), so the B-rules end it
  before the guard's drop resets the state: the argument T3 makes for `MutexGuard`, with a counter for the lock. The
  cell is never copied behind a guard's back (§2.1, `@noncopy`).
- **Cost.** One word beside `T`; acquiring is a test and a store, releasing a store; the panic path is cold.

### 2.5 `dyn … + Share`

Since `b7c9f41`, no `dyn` box is `Share`, which is right for a box that may hold a cell but refuses Bun's 58
`dyn … + Sync`. `dyn I + Share` and `dyn fn(…) -> R + Share` (§38.4) are `dyn` types whose boxes are `Send` and
`Share`: `+ Share` implies `+ Send`, as `Share` implies `Send` (§38.1). A coercion to one needs `T: Share` (N0334
otherwise: a closure capturing a `Cell` cannot become one); `reachesSharedChange` stops at such a box. It converts
to `dyn I + Send` and to `dyn I` (the same value and tables), never the other way.

### 2.6 With the rest of the language

| Feature | Rule |
|---|---|
| Borrow checker | unchanged for safe code: `set`, `read`, `write` take `borrow this`, so any number of shared loans coexist; `borrow mut` of a cell is exclusive as ever |
| Copy | a cell and everything holding one is a move type (§2.1) |
| Closures | a `borrow fn` may capture a cell shared and change it while the caller holds another shared loan (Bun's `&Cell<bool>` drop guards, `VirtualMachine.rs:6610`) |
| `dyn` | a `borrow this` method behind `borrow dyn I` may change the object's cells; `dyn I + Send` needs only `Send`, `dyn I + Share` needs `Share` (§2.5) |
| Conditions | a guard temporary ends with the condition or the block that uses its bindings (§2.4) |
| Async | a cell in a task frame is fine (frames never move, A1). A `CellRef`/`CellRefMut` live across an `await` is warning N0402 (N0405 since ADR 0132): another task resumed meanwhile panics on the same cell. Tasks that cross threads already need `Send` captures |
| Statics | refused (N0337), as in Rust. Bun's 86 thread-local cells wait for thread-locals (§4) |
| Profiles | all three cells are in lib/core, so in every profile, the kernel included; they have no effect |
| Interpreter | runs them as ordinary memory; the conflict panic is the same in both back ends |

## 3. Alternatives considered

| | A. Library cells over one compiler-known primitive | B. Translation rule, no new types | C. Atomics as cells |
|---|---|---|---|
| What | `RawCell<T>` (the primitive), `Cell<T>`, `RefCell<T>` in lib/core | re-shape to `borrow mut`, or raw pointers in `unsafe` | `AtomicU32`, `AtomicBool`, … in place of `Cell` |
| Safe code | yes | `borrow mut` cannot cross re-entrant JS calls; raw pointers are `unsafe` at ~1,000 sites | yes, for integers and `bool` (462 of 791 cells) |
| Threads | cells are not `Share`, checked | the user's argument | atomic, so correct but paid for |
| Cost | `Cell`: a plain load or store; `RefCell`: a flag test and an update per guard | none | sequentially consistent operations unless every site names an ordering |
| Coverage | all of §1.1 | all, unsafely | no pointers, enums, structs, `Option`, collections |
| Soundness | gives p5 a correct form, not `Share` | p5's `UCell` stays inferred `Share` and breaks U2 | as B |

**A is proposed** (§2). B gives up the port's safety claim for the most common pattern in Bun and still leaves CeeSafe
unable to state the rule its own `Mutex` relies on. C covers half the cases at a cost the rest of the program does
not need. For `dyn`, the alternative to §2.5 is to keep every box un-`Share` and port Bun's `dyn … + Sync` to
`Arc<Mutex<…>>`: a lock per call where Bun has none.

## 4. Not in this ADR

- **Thread-locals** (ADR 0016 §4, "not in v1"; profiles.md lists `@threadLocal` as a profile capability). Bun's 86
  thread-local cells and its static `VM` pointer (`VirtualMachine.rs:692`) need them; a cell is what one would hold.
- **`OnceCell`, `OnceLock`, `LazyLock`** (572 uses). A single-thread `OnceCell` is a small library type over `RawCell`;
  the thread-safe ones are lazy statics, ADR 0024's rejected option C. Both want their own decision.
- **`Rc<T>`.** CeeSafe has only `Arc`. Bun's 4 `Rc<RefCell<…>>` and its intrusive counts (`ref_count: Cell<u32>`) port
  with this ADR's `Cell`; a non-atomic `Rc` is a separate question.
- **Implicit dereference of guards.** Guards are reached through `data()`, as `MutexGuard` is; a guard ADR may change
  both together.
- **An unchecked `JsCell`.** The port maps `JsCell` to `RefCell` (or `Cell` where `T` is Copy). If the checks show up
  in Bun's benchmarks, a `RawCell` wrapper with an `unsafe` accessor is what Bun has today, and needs nothing new.
- Weaker orderings for cells, `Cell` of a reference (§2.1), and `Share` declared by user code.

## 5. Testing

- **Run tests** (corpus, interpreter and native, compared): every `Cell` and `RefCell` method; a re-entrant callback
  that sets a cell through a second shared reference (§2.3's `fire`); `RefCell<Box<…>?>`'s `take` and `replace`,
  with a `drop` of the old value that reads the same cell; `==` on two `Cell<u32>`; `if (c.read()…) {…} else
  { c.write()… }` does not panic; a `Box<dyn I + Share>` read from both `Thread.both` children.
- **Panic tests**: `write` under a live `read`, `read` under a live `write`, `replace` under a guard; each with its
  message and location, in both back ends; `tryRead`/`tryWrite` return `null` in the same cases.
- **Check-fail tests**: a cell captured by `Thread.both` (N0334); `Arc<Cell<u32>>`; `static C: Cell<u32>` (N0337); a
  struct holding a cell three levels deep in a `Thread.both` capture; a struct holding a cell coerced to
  `dyn I + Share` (N0334); `let a = rc; let b = rc;` on a `RefCell<u32>` and on a struct with a `Cell<u32>` field
  (N0250); a guard returned past its cell (N0243); `RefCell<StringView>` (N0351); `@interiorMutable` outside lib/core
  (N0290); `Cell<Box<u32>>` (the `Copy` bound). A warning test for a guard across `await` (N0402). The existing
  `tests/reference/tests/corpus/check-fail/share-shared-change.cee` (p4) stays.
- **Mutation suite**: thread cases that drop the `RawCell` case from `reachesSharedChange`, or `@noncopy` from
  `RawCell`, must be caught by the tests above.
- **C output**: `emit_c` snapshots showing `Cell.get`/`set` as one load or store and no call; a bounds-check snapshot
  showing that a cell's value is never a tracked fact.
- **ThreadSanitizer**: p5 rewritten with `Mutex`/atomics, built with `-fsanitize=thread`, reports nothing.
- **Bun**: `analysis/probes/g3_cell.cee` passes (it becomes a non-`g` probe) with `Cell.create(0)`.

## 6. Questions for the owner

1. Option A: one `@interiorMutable` primitive, `RawCell<T>`, and `Cell<T>`/`RefCell<T>` as library code in lib/core,
   shipped in 0.25.0? Recommended: yes. It is the only option that keeps the port safe and lets CeeSafe state the
   rule its own `Mutex` depends on.
2. `RawCell` joins `reachesSharedChange`'s list (§2.2), so a cell is `Send` but not `Share`? Recommended: yes. It
   adds one case to the rule `b7c9f41` already shipped, and no existing program holds a `RawCell`.
3. `RawCell` is `@noncopy`, so cells and their holders are move types (§2.1)? Recommended: yes. A Copy cell can be
   read behind a live `CellRefMut`; Rust's cells are not `Copy` either.
4. `dyn I + Share` (§2.5), so a `dyn` over data with no cell can be shared again? Recommended: yes, in this ADR: Bun
   writes `dyn … + Sync` 58 times, and without it each becomes a lock.
5. A `RefCell` conflict panics (abort), with `tryRead`/`tryWrite` beside? Recommended: yes. It matches every other
   run-time check in CeeSafe, and Bun's `BorrowMutError` is a bug to report, not a value to handle.
6. Names: `RawCell`, `Cell`, `RefCell`, and `read()`/`write()` returning `CellRef`/`CellRefMut` with `data()`?
   Recommended: yes. They keep Rust's names where a port can keep them, and `borrow` is reserved.
7. A warning (N0402, not an error) for a `RefCell` guard live across `await`, and guards in conditions ended as Rust
   2024 does (§2.4, for ADR 0085)? Recommended: yes. Across `await` it may be deliberate in a single-task program,
   but it is usually a panic waiting for a second task.
8. `Cell<T: Copy>` bounded on the struct, with the 18 non-Copy cells ported to `RefCell<T?>`, rather than amending
   §21.1 so an inherent impl may add bounds (p8)? Recommended: yes. Conditional methods are a wider change than 2% of
   the cells justify; if another port needs them, they get their own ADR and `Cell` gains `replace`/`take` for all `T`.
9. Cells of references refused (N0351) in v1, rather than an invariance rule for regions? Recommended: yes. Ten Bun
   sites, all portable with raw pointers; variance can be its own ADR if a second port needs it.
