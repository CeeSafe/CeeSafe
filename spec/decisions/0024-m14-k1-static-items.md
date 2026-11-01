# ADR 0024 — M14 K1: static items (global state)

> **Inherited.** ADR 0024 was accepted before CeeSafe's fork (Decision 0006), when the language had its
> earlier name. In this record, "the reference" is that language and project and "the reference compiler" its compiler:
> read them as CeeSafe and `ceesafec`. Links to documents CeeSafe did not keep are given as text.

- Status: **Accepted** (project owner, 2026-10-29), with `static mut` added at the owner's request (§3.7). The
  wording is the agent's.
- Scope: M14 K1, OD-28. Global state for a kernel written in the reference (CosmoOS roadmap 2.0's SA2 and SA3), and for hosted
  programs.

## 1. Context

- **Today** the reference has only compile-time `const` items (type-system §10). `static var count: u64 = 0;` and
  `var count: u64 = 0;` at top level are N0101. CosmoOS's four CeeSafe ports keep all their state in C, and C7's kernel
  demonstrations (`integrations/cosmoos/c7`) get every lock from a function argument or a `KernelHeap` box made at run
  time.
- **A kernel needs** tables and singletons protected by locks (the handle table, the module list, lockdep's graph),
  counters, and per-CPU data. CosmoOS keeps per-CPU data in arrays indexed by CPU id (`[CONFIG_MAX_CPUS]`, beside
  `struct percpu`).
- **Kernel locks are C state** (R-11, type-system §33). A spinlock may not be zero-initialised: a zeroed
  `spinlock_t`'s owner is CPU 0. CosmoOS writes a static lock with a C initialiser macro,
  `SPINLOCK_INIT(name)`, which sets the owner to `SPINLOCK_NO_OWNER`. C7's `SpinLock<T>` instead initialises its lock at
  run time, in place (`@placed fn init`).
- **Thread-safety already has a vocabulary.** `Share` (§38.1) says a value may be used from several threads at once.
  Lock and atomic types are `Share` by the library's `@threadSafe` declaration.

## 2. Options

| | A. Immutable statics of `Share` types, built by compile-time constructors (proposed) | B. `static mut` with `unsafe` access (Rust's) | C. Lazily initialised statics (a run-time constructor on first use) |
|---|---|---|---|
| Safe access | `borrow NAME`, like any shared reference; mutation only through the type's own synchronisation | every access is `unsafe` | safe, but needs a once-guard on every access |
| Data races | impossible: the type is `Share` | the user's argument | impossible |
| Initialisation | at compile time, as C static initialisers: no constructor runs, no order to get wrong | the same | at run time, in an order the program must not depend on |
| Kernel fit | yes: a `SpinLock<Table>` in a static, built with `SPINLOCK_INIT` | yes, unsafely | no: a once-guard in interrupt context needs a lock of its own |

A is Rust's `static` with interior mutability, and Linux's `DEFINE_SPINLOCK`. The owner chose A and B together: B
as `static mut`, every use of which is `unsafe` (§3.7), for the floor and for state whose synchronisation the reference cannot
express. C is not taken.

## 3. Proposed decision: A

### 3.1 The item

```ceesafe
static TABLE: SpinLock<HandleTable> = SpinLock.constant("handles", HandleTable.empty());
static OPENED: AtomicU64 = AtomicU64.constant(0);

fn openOne() -> u64 => OPENED.fetchAdd(1) + 1;
```

- `static NAME: T = e;` at package level, `public` or not. `T` must be `Share` (§38.1), or it is a new error,
  N0337: "a static is shared by every thread". This also covers a static used only from one thread, because the
  checker cannot know that.
- A static is **never assigned or moved** (N0337), and `NAME` in an expression is a place of type `T` that only shared
  borrows and copies (for a `Copy` `T`) may use. So all mutation goes through `T`'s own methods taking `borrow this`:
  a lock's guard or an atomic's operation.
- **Region.** `borrow NAME` has the static region, which outlives every other region. A reference to a static may be
  stored and returned anywhere a reference may (E1–E4 hold trivially), and passed to another thread when `T` is
  `Share`.
- A static is never dropped. A `T` with a `drop` is allowed, and its drop never runs: a kernel never exits, and a
  hosted program's statics end with the process, as in C.
- Generic statics, and statics inside functions, are not in v1 (N0337).

### 3.2 Compile-time constructors

`e` must be a **static initialiser**, evaluated by the compiler to a C constant initialiser. It may be:

- a constant expression (§10), a string literal (as `*CChar`), or a struct, enum or array literal of static
  initialisers (fields private to `T` are allowed only inside `T`'s own `@constant` constructor);
- a call of a `@constant` function: one whose body is a single expression that is a static initialiser of its
  parameters, the parameters being static initialisers at each call. `@constant` functions are checked as ordinary
  functions too, so they can be called at run time;
- a call of a **C constant macro**: an `extern "C"` function declared `@constant`, whose name is a C macro that expands
  to a constant initialiser (`@constant fn SPINLOCK_INIT(name: *CChar) -> spinlock_t;`). The reference trusts the declaration,
  as it trusts `@effects` on an extern (R-2).

A type whose values are made at their final address (`@placed init`, §33.3) may be in a static only through a
`@constant` constructor, whose initialiser must leave it as `init` would. For CosmoOS's locks that is the C macro's
job, so `SpinLock.constant` uses `SPINLOCK_INIT` and does not call `init`.

### 3.3 Per-CPU data is a library type

There is no `@percpu` in the language. The kernel library (K3) provides `PerCpu<T>`, an array of `T` per CPU behind a
`Share` type. `PerCpu<T>.local(guard)` lends the calling CPU's element, for as long as a guard that disables
preemption (or interrupts, for data an interrupt handler shares) is borrowed. Its `unsafe` core argues the rest: the
guard pins the CPU, so only this CPU's code, and not its interrupts when the guard masks them, uses the element. So:

```ceesafe
static TICKS: PerCpu<u64> = PerCpu.constant(0);

@context(atomic)
fn tick(preempt: borrow PreemptGuard) { *TICKS.local(preempt) += 1; }
```

That keeps the language change to statics, and puts the CPU model where CosmoOS's own API is (OD-30).

### 3.4 C

A static is a C object definition with internal linkage, or external with `@export` (or across ADR 0022's units),
named `ceeS_<name>`. Its initialiser is the evaluated C initialiser: the `@constant` calls are expanded, and the C macros
are written as calls. It is `const` in C only when `T` contains no C state, no atomic and no lock, so that its methods
can write through the pointers they derive, in their own `unsafe` code.

### 3.5 Unsafe access at the floor

`unsafe` code may still take `borrow NAME as *T as *mut T`, as for any shared reference (`unsafe.md`). That is how
the library types implement their synchronisation. It is not a `static mut`.

### 3.7 `static mut`

```ceesafe
static mut BOOT_INFO: BootInfo = BootInfo.empty();

/// Safety: runs once on the boot CPU, before any other CPU or interrupt can read BOOT_INFO.
unsafe fn recordBoot(info: BootInfo) { BOOT_INFO = info; }
```

- `static mut NAME: T = e;` takes the same static initialisers (§3.2). `T` need not be `Share`.
- **Every use is an `unsafe` operation**: reading, writing, assigning to a field, borrowing, or taking its address. U1
  (`unsafe.md`) gains "use a `static mut`", so outside `unsafe` code each use is N0280, and a function using one needs
  its `/// Safety:` comment (N0283).
- A borrow of a `static mut` has the static region, as for a `static`. Nothing checks that two such borrows do not
  alias, or that two threads do not race: that is the `/// Safety:` argument (U3's rule for references made from
  pointers).
- In C it is a non-`const` object (`ceeS_<name>`), like a static whose type holds C state.
- The interpreter checks accesses through it as it checks raw pointers: an access out of bounds or after the end of a
  value is a fault (`UnsafeTests`).

### 3.6 Hosted programs

The same `static` works in hosted programs for atomics (`AtomicU64.constant`, in lib/std) and any `@threadSafe` type
with a `@constant` constructor. lib/std's `Mutex<T>` allocates its platform lock and so has none in v1. A static mutex
is a later library change: `PTHREAD_MUTEX_INITIALIZER` and `SRWLOCK_INIT` are C constant macros.

## 4. Consequences

- **The language:** `static` and `static mut` items, the static region, `@constant` functions and C constant macros.
  Diagnostics: N0337 for a static that is not `Share`, is assigned or moved, or is generic or local; N0338 for an
  initialiser that is not a static initialiser; N0280 for a use of a `static mut` outside `unsafe`.
- **The interpreter** evaluates static initialisers once, before `main`, into its memory. C macros it cannot evaluate
  (it has no C) fault, so kernel statics are tested natively.
- **Tests (planned):**
  - run-pass: atomics and a lock in statics, from two threads (`Thread.both`) under TSan;
  - check-fail: each N0337/N0338 case;
  - a CosmoOS kernel object table in a static `SpinLock`, as M14 K1's acceptance says.
- **`@threadSafe` outside the reference's libraries.** A static's type must be `Share`, and kernel lock types are `Share` only
  by `@threadSafe`, which today is accepted only in the reference's own library files (N0290). The kernel library (K3, OD-30)
  needs the same permission. ADR 0025 decides it with the library's home: for example, a package that declares itself
  a trusted library in its `ceesafe.toml`.
- **Not decided here:** lazily initialised statics, generic statics.

## 5. Questions for the owner

1. Option A: immutable statics of `Share` types, built at compile time? (Recommended.)
2. `@constant` functions and `@constant` C macros as the way to build them? (Recommended. The alternative is to allow
   only literals and constants, which cannot build a private-field lock or call `SPINLOCK_INIT`.)
3. Per-CPU data as the kernel library's `PerCpu<T>`, not a language attribute? (Recommended.)

## 6. Implementation (2026-10-29)

Specified in type-system §39, grammar `StaticDecl`, diagnostics N0337 and N0338.

- **Uses** need no new borrow rule. A function that uses a static gets a hidden local holding its address, a
  `borrow(static) T` (`borrow(static) mut T` for a `static mut`) made by `HirStaticAddress`/`NirStaticAddress`, which
  holds no loans. `NAME` is `*local`. The existing rules then apply: through a shared reference nothing is changed
  (N0211, N0248) or moved out (N0251), and returned references need a return type that names `static` (N0245
  otherwise). Diagnostics call the place "static 'NAME'". So assigning or moving a static is N0211 or N0251, not N0337
  as §4 planned. N0337 is for a static whose type is not `Share`, and N0338 for an initialiser.
- **One exception to `Share`:** a `StringView`, the only reference a static may hold, viewing static string data.
- **Initialisers** are type-checked before the function bodies, so the instances their `@constant` calls create are
  checked with them. They are folded after the bodies, by substituting arguments into `@constant` bodies
  (`TypeChecker.StaticFold`). A `@constant` body is checked once with its parameters symbolic.
- **C** (`CEmitter.EmitStatics`): `static CEE_UNUSED T ceeS_NAME = { … };`. With `--split`, it is `extern` in the
  header, defined in `ceesafe_instances.c` and renamed like the functions. A C constant macro gets no prototype and no
  wrapper. A run-time call of one, in a `@constant` function called at run time, is a compound literal
  `((T)MACRO(args))`.
- **Interpreter**: each static is a writable allocation, filled before `main` and not counted as a heap allocation; a
  C macro in an initialiser is a fault.
- lib/std's atomics have `@constant` constructors (`AtomicU64.constant`, …).
- **Tests:**
  - `run-pass/statics.cee`, in every C configuration (split and TSan included) and the interpreter: atomics changed
    from two threads, constants, struct, array and `StringView` statics, a `@constant` constructor setting private
    fields, `static mut`, and references returned as `borrow(static)`;
  - check-fail `statics.cee`, `static-moves.cee` and `static-initializers.cee`;
  - `parse-pass/statics.cee`.
- **Not yet:** `@export` statics, enum values in initialisers, and a C constant macro compiled natively. The macro path
  is checked and emitted, but its C needs a header defining the macro: M14 K3's kernel library, with CosmoOS's
  `SPINLOCK_INIT`, is its first real use and test. Per-CPU data (`PerCpu<T>`) is K3 too.
