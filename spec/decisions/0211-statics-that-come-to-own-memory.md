# ADR 0211 — Statics that come to own memory: `null` and an empty vector as static initialisers, `OnceLock.constant`, `Mutex.constant`

- Status: **Accepted** by the owner, 2026-11-23 ("I accept ADR 0211."), with §6's three questions taken as
  recommended; proposed the same day and implemented under the owner's instruction ("continue", the optimization and
  DX roadmap's Phase 3, item 5: ADR 0075's leftover, G21); not released. Asked which to build, a constant
  `OnceLock`, `Mutex<T>.constant` as ADR 0075 §2.15 wrote it, or both, with the constant mutex's out-of-memory panic
  stated, the owner answered "Both".
- Scope: type-system.md §39.4; `src/check/statics.cee`, `src/check/vectors.cee`; the C backend's static initialisers
  and generated `main`; the interpreter's; `runtime/core` (one accessor); `lib/core/cells.cee`, `lib/std/sync.cee`;
  spec/api; guide/threads.md and guide/builtins.md.

## 1. Context

ADR 0075 §2.15 accepted `Mutex<T>.constant(value)` and `Vector<T>.empty()`, so that a program can write
`static TABLE: Mutex<Vector<u64>> = Mutex<Vector<u64>>.constant(Vector<u64>.empty());`, and its §7.5 left them out:
"`Vector` is a built-in type, and making it a static initialiser is its own change". With 0.94.1 a static can hold
atomics and plain data and nothing that grows: a registry, a cache or a set of seen ids has to be passed to every
function that uses it. Tried: `OnceLock<u64>.create()` in a static is N0338 ("'create' is not '@constant'"),
`Vector<u8>.create()` is N0338, and so is a struct literal with a `null` field ("an enum value").

Two things §2.15 did not settle:

- A `Mutex` holds a platform lock that `create` allocates and reports the failure of. A constant has no lock yet.
- "The leak checker does not count a static's allocations": nothing did that.

## 2. Decision

### 2.1 An optional's `null` is a static initialiser

Its bytes are zero: a null pointer for a niche optional, the tag 0 otherwise. `Some(e)` and other enum values are
not (N0338, whose message now says that `null` is the one that is).

### 2.2 An empty vector or string is one

`Vector<T>.create()` and `String.create()` allocate nothing: pointer, length and capacity are zero.
`Vector<T>.empty()` is `create` under the name ADR 0075 gave it; either is accepted. `withCapacity` is not.

### 2.3 `OnceLock<T>.constant()` and `RawCell<T>.constant(value)`

`@constant` twins of `create`. A `OnceLock` in a static is filled once, with `getOrInit` or with `tryInit`, whose
initialiser may fail and return its error.

### 2.4 `Mutex<T>.constant(value)`

`value` is a static initialiser. The mutex's lock field becomes an `AtomicUSize`: the lock's address, or 0 for a
constant mutex not yet used. Every use reads it; at 0 it makes the lock and stores it with one compare-and-exchange,
and a thread that loses the exchange destroys the lock it made and uses the winner's. `drop` destroys a lock that
exists.

**Running out of memory there is a panic** ("out of memory making a constant Mutex's lock"): `lock()` returns a
guard and has no error to return. `Mutex<T>.create` still reports it. A program that must not panic there keeps the
mutex in a `OnceLock` and fills it with `tryInit(() => Mutex<T>.create(v))`.

### 2.5 The leak report and statics

A static is never destroyed (§39), so what it comes to own is live when the process ends, and a run with
`CEESAFE_ALLOC_REPORT`, an injected failure or `ceesafec test` would report it. **When allocations are counted, and
only then, the generated `main` destroys the statics that need drop after the program's `main` returns, the last
declared first, before `ceesafe_shutdown`'s report.** The interpreter does the same. `Process.exit` and a panic do
not, as they run no destructor. A run that does not count allocations never destroys a static.

## 3. Options not taken

- **A lock that needs no allocation** (C's `PTHREAD_MUTEX_INITIALIZER`, a zero `SRWLOCK`). The lock would be inside
  the `Mutex`, whose size would then be the platform's: 40 bytes on glibc x86-64, 64 on macOS, 8 on Windows. Layout
  comes from `TargetInfo` only, and a `Mutex` that C state lives in could not move.
- **`OnceLock` only.** No panic anywhere, and one more line at every use. The owner chose both.
- **The runtime finds what statics reach** (as it does for isolated units that trapped, ADR 0175) and prints
  `statics held=<k>` for the tools to subtract. Nothing would be destroyed, and every tool that reads the report
  would have to learn the line.
- **Destroying statics at every exit of `main`.** A language rule, with C++'s hazards (a thread still running, a
  static used by another's `drop`), for the sake of a report.

## 4. Consequences

- `Mutex` grows no field; each `lock`, `tryLock` and unlock reads one more atomic.
- A static whose type has a `drop` block now has that block run at the end of a run that counts allocations. Such a
  static could exist before (its initialiser being plain data); none is in the corpus or the libraries.
- A host that embeds the runtime calls `ceesafe_shutdown` itself and gets no such destruction: its report counts
  what its statics own.

## 5. Implementation and tests (2026-11-23)

- `staticFold` (src/check/statics.cee) lets through an optional's `null` and `bfVectorCreate` of a vector; the C
  backend writes `{ 0 }` for both, the interpreter a zero value.
- `ceesafe_rt_alloc_counted` (runtime/core) and `emitStaticsRelease` (the generated `main`); the interpreter's
  `runMainSteps`.
- lib/std's `Mutex.lockHandle`; lib/core's `RawCell.constant`; lib/std's `OnceLock.constant`, `Mutex.constant`.
- run-pass/static-locks.cee: a `null` field, a `OnceLock` filled by a fallible initialiser, a constant mutex whose
  lock three threads make at once (300 pushes, their sum), a mutex of a string and of a number; with `--check-leaks`
  nothing is live, natively (also `--split`) and in the interpreter. check-fail/static-locks-errors.cee: five
  initialisers that are still N0338. The results are in ROADMAP §10.
- **The allocation sweeps** (found by the contract suite before 0.94.5): `tools/fault_injection.py` fails every
  allocation of run-pass/static-locks.cee in turn, and where the failed one is a constant mutex's lock the run ends in
  §2.4's panic, "out of memory making a constant Mutex's lock". That is this ADR's defined outcome, as ADR 0056
  §3.3's "async frame stack: out of memory" is, so the sweep accepts it as it accepts that one (exit 134, no leak).
  `ceesafec test --sweep alloc` still counts it as a failure; setting it apart there, as ADR 0147 does the frame
  stack's, is a change to the test command that is not made here.

## 6. Questions for the owner (answered 2026-11-23: accepted as recommended)

1. Is destroying statics only in a run that counts allocations (§2.5) acceptable? Yes; the other two ways are in §3.
2. Should `Vector<T>.create()` stay accepted in a static beside `empty()` (§2.2)? Yes; it is the same value, and
   refusing one spelling of it would need a rule of its own.
3. Should lib/kernel's `Mutex` and `SpinLock` get `constant` too? A separate change; they hold no allocated lock,
   and a kernel's statics are reviewed with CosmoOS.
