# ADR 0025 — M14 K3: the kernel library

> **Inherited.** ADR 0025 was accepted before CeeSafe's fork (Decision 0006), when the language had its
> earlier name. In this record, "the reference" is that language and project and "the reference compiler" its compiler:
> read them as CeeSafe and `ceesafec`. Links to documents CeeSafe did not keep are given as text.

- Status: **Accepted** (project owner, 2026-10-29), with option **B**, the library in the reference's repository, instead of the
  recommended A. `trusted` packages (§3.2) and the v1 API (§3.3) were accepted as recommended. §6 records what B
  changes. The wording is the agent's.
- Scope: M14 K3, OD-30. The types CosmoOS's roadmap 2.0 needs for SA2 (allocation and object lifetimes) and SA3 (locks,
  waits and quiescence): where they live, who may declare them thread-safe, and their API.

## 1. Context

- **What exists.** These were written as demonstrations, each with its own C glue and tests:

  | Type | Where | Status |
  |---|---|---|
  | `KernelHeap` | C5, `integrations/cosmoos/c5/kernel.cee` | an `Allocator` over `kmalloc`/`kfree` |
  | `KRef<T>` | C7, `c7/kobject.cee` | a counted kobject whose drop is the release |
  | `SpinLock<T>`, `IrqSpinLock<T>`, `Mutex<T>` and their guards | C7, `c7/sync.cee` | the data only through the guard; a spin guard is an atomic context |
  | `Quiesce` | C7, `c7/quiesce.cee` | quiescence guards |
  | `ObjectCache<T>` | — | not built |
  | `PerCpu<T>`, `PreemptGuard` | — | not built (ADR 0024 §3.3) |

  Each lock is made at run time, in a `KernelHeap` box (`@placed init`). None can be a static yet.
- **Statics** (ADR 0024) now let a lock live in a static, built by a `@constant` constructor and a C constant macro.
  CosmoOS has `SPINLOCK_INIT(name)`. It has no static initialiser for `struct mutex`, only `mutex_init`.
- **`@threadSafe`**, which makes a type `Send` and `Share` by declaration, is accepted only in the reference's own library files
  (N0290). A static's type must be `Share`, and a kernel lock is `Share` only by that declaration.
- **How CosmoOS builds the reference today:** each `.cee` file is compiled alone (`scripts/cee-gen.py`), so files share no the reference
  types.

## 2. Options

| | A. A CeeSafe package in CosmoOS's tree (proposed) | B. A library in the reference's repository (`lib/kernel`) | C. Keep demonstrations |
|---|---|---|---|
| Owner | CosmoOS: it binds CosmoOS's C API | The reference, but it binds one kernel's API | — |
| Changes with the kernel | in the same PR as the C it binds | needs a reference release for each change | — |
| `@threadSafe` | needs a way to trust a package | already allowed | — |

## 3. Proposed decision: A

### 3.1 Home and build

- `kernel/ceesafe/core/` in CosmoOS is a CeeSafe package (`ceesafe.toml`: `name = "kcore"`, `kind = "library"`,
  `profile = "freestanding"`, `float = false`).
- CosmoOS's kernel code written in the reference becomes packages that depend on it by path. `cee-gen.py` builds each package
  with `ceesafec build --emit c --split --program <package>` (ADR 0022), so the kernel's CeeSafe packages share CeeSafe types
  through `kcore`. Packages replace today's one-file ports as each is touched; the four existing ports keep working as
  they are.

### 3.2 Trusted packages

- A package may declare in its manifest that it holds `unsafe` synchronisation code: `trusted = true`. In such a
  package `@threadSafe` is accepted, as in the reference's libraries. Elsewhere it stays N0290. A dependency that is trusted is
  listed in `ceesafec build`'s output, so a reviewer sees which packages carry the argument.
- `trusted` permits nothing else: `unsafe` already works in any package, with its `/// Safety:` comments (U2). What it
  adds is that a type's thread-safety becomes the package's claim.

### 3.3 The API (v1)

- **`KernelHeap`**, from C5, unchanged.
- **`ObjectCache<T>`**, a typed slab cache over `kmem_cache`. A static or boxed cache made by `ObjectCache<T>.create(name)`
  (`kmem_cache_create` with `T`'s size and alignment). `cache.alloc(value)` returns `Result<CacheBox<T>, AllocError>`.
  A `CacheBox<T>` is one pointer whose drop drops the value and returns the memory to the cache. Because `Box<T, A>`
  needs a stateless allocator (C5), `CacheBox<T>` is its own type, scoped to the cache it came from.
- **`SpinLock<T>`, `IrqSpinLock<T>`, `Mutex<T>` and their guards**, from C7, plus `@constant` constructors:
  `SpinLock.constant(name, data)` with `SPINLOCK_INIT`, and `Mutex.constant(name, data)` with a new `MUTEX_INIT(name)`
  macro, which CosmoOS adds to `mutex.h` in the same change.
- **`PreemptGuard`** (preemption off until dropped; an atomic context) and **`IrqGuard`** (interrupts off; also
  atomic), and **`PerCpu<T>`**: `CONFIG_MAX_CPUS` values, `PerCpu.constant(v)`, and `p.local(borrow guard)`, which
  lends this CPU's value while the guard is borrowed. Its `unsafe` core is argued once: a guard pins the CPU, and an
  `IrqGuard` also keeps this CPU's interrupt handlers out.
- **`KRef<T>`** and **`Quiesce`**, from C7.

### 3.4 Tests

The C5 and C7 self-tests and stress tests move into CosmoOS as kernel self-tests of `kcore`. They run in every debug
boot, on both architectures, under lockdep. The reference's `c5.py` and `c7.py` stay as they are, against their own copies, until
`kcore` replaces them.

### 3.5 What the reference changes

- `trusted = true` in the manifest, and `@threadSafe` in trusted packages (tooling §4, type-system §38.1).
- Nothing else. The API is CeeSafe code in CosmoOS.

## 4. Consequences

- **Order:**
  1. The reference: `trusted`, then the 0.2.0 release with statics and `--split`.
  2. CosmoOS: the pin moves to 0.2.0, `kcore` lands with its self-tests, and `MUTEX_INIT` is added.
  3. M14 K1's acceptance, a kernel object table in a static `SpinLock`, is then CosmoOS's SA2 proving conversion.
- `kcore`'s `unsafe` code is a new place unsoundness can hide (R-2, R-13). Its `/// Safety:` arguments go in the
  safety-audit pack (`safety-audit.md` §3) for the outside audit (K6).
- **Not decided here:** a stateful `Box` allocator, which would let `ObjectCache` be an `Allocator`, and wait queues and
  completions (SA3's own design, on top of these types).

## 5. Questions for the owner

1. The library as a CeeSafe package in CosmoOS's tree, `kernel/ceesafe/core/` ("kcore")? (Recommended.)
2. `trusted = true` in `ceesafe.toml` to allow `@threadSafe` there? (Recommended; the alternative is to keep the kernel
   library's thread-safe types in the reference's repository.)
3. The v1 API in §3.3, including the new `MUTEX_INIT` macro in CosmoOS? (Recommended.)

## 6. The decision: B, and what it changes

The owner chose B: the kernel library is part of the reference's repository and ships inside `ceesafec`, like lib/std.

- **`lib/kernel`**, a library for a kernel with CosmoOS's C API. It is not compiled into every freestanding build, as
  lib/core is, because it names CosmoOS's headers. A build selects it with `kernel = "cosmoos"` in `ceesafe.toml`'s
  `[package]`, or `ceesafec --kernel cosmoos` for a build of files. That is valid only with the freestanding profile
  (N0320 otherwise). Its names are global in the build, as lib/std's are in a hosted one (§38.5).
- **`@threadSafe`** is accepted in lib/kernel as in the reference's other libraries. `trusted = true` (§3.2) is still accepted,
  for packages outside the reference that hold synchronisation code of their own, such as a CosmoOS driver's.
- **A change to the library is a reference change**, released and then picked up by CosmoOS's pin. §3.1's "in the same PR as
  the C it binds" becomes two changes, the reference's first. `MUTEX_INIT` was to be added to CosmoOS's `mutex.h` (not
  done; §7).
- **Testing.**
  - The reference's suite type-checks programs that use lib/kernel (`--kernel cosmoos`, freestanding) and the misuses it
    refuses. It does not compile their C, because CI has no CosmoOS headers.
  - `integrations/cosmoos/k3.py` builds and runs the library's tests natively against a CosmoOS checkout, as `c7.py`
    does (its host harness and shims).
  - Once the pin moves, CosmoOS's own self-tests, in debug boots on both architectures, are the library's acceptance.

## 7. Implementation (2026-10-29)

- **`lib/kernel`** (`heap.cee`, `sync.cee`, `kobject.cee`, `quiesce.cee`), embedded in the reference compiler. It is selected by
  `--kernel cosmoos`, `kernel = "cosmoos"` in `ceesafe.toml`, or `// option: kernel cosmoos` in a corpus file.
  - From C5 and C7: `KernelHeap`, `SpinLock`/`IrqSpinLock`/`Mutex` and their guards, now `@threadSafe` with `T: Send`;
    `KRef<T>`; `Quiesce`, `Deferred<T>`, `deferDrop`.
  - New: the `@constant` constructors (`SpinLock<T>.constant` and `IrqSpinLock<T>.constant`, with `SPINLOCK_INIT`);
    `PreemptGuard`; `IrqGuard`; and `PerCpu<T: Copy + Send>`, whose `local(IrqGuard)` returns a
    `PerCpuRef` that owns the guard and lends the value, a second loan on one CPU panicking.
- **What this needed in the compiler** (type-system §39.4, "C state at compile time"):
  - C state allowed in a static's type, a C constant macro's result and a `@constant` constructor's literal;
  - `@constant` functions whose result holds C state compile-time only;
  - each static C string emitted as a `char` array of its own, because CosmoOS builds with `-Wwrite-strings`;
  - the emitter now counts a box's construction (allocator, `@placed` methods) and a taken function address as
    reaching library code, which lib/kernel's boxed locks and `KRef` need;
  - lib/kernel's own composite types are emitted only when used, as lib/std's are.
- **Trusted packages:** `trusted = true` in `ceesafe.toml` (`PackageInfo.Trusted`) admits `@threadSafe`.
- **Found in the implementation:**
  - CosmoOS exports to modules neither `arch_irq_save`/`restore` (they inline to `arch_irq_*_hw`) nor
    `preempt_enable`. So `IrqGuard`, `PreemptGuard` and `PerCpu` are for code built into the kernel. Locks, kernel
    objects and the heap work in modules too.
  - `ObjectCache<T>` is **not built**. Moving a value that is not `Copy` into raw memory has no primitive (`p.write`
    needs `Copy`, §27.3), so a cache box cannot be filled. It needs a language addition (a move into raw memory, or a
    stateful `Box` allocator) and is left open. **Resolved by [ADR 0027](0027-m14-static-mutex-and-object-cache.md)**, with a static `Mutex` below.
  - **No static `Mutex`.** A CosmoOS mutex holds list heads that point to themselves. CosmoOS's static initialisers
    take the variable's own name for that (`WAITQUEUE_INIT(name)`, `LIST_HEAD_INIT(name)`), and a C constant macro
    is not given it. So §3.3's `MUTEX_INIT(name)` could not initialise one correctly, and it was not added to
    CosmoOS. A static mutex needs initialisers that know their own address (a §39 extension), or a lock made at
    start-up. It is left open. Static spinlocks are not affected: `SPINLOCK_INIT` has no self-pointers.
- **Tests:**
  - The reference's suite: `check-pass/kernel-library.cee`, `check-fail/kernel-library.cee` (sleeping under a static spin
    guard, with interrupts off), `check-fail/kernel-library-constant.cee` (N0338), and `KernelLibraryTests` (trusted
    packages, the manifest keys, the library absent unless selected).
  - `integrations/cosmoos/k3.py` (2026-10-29, both architectures, one file and split): the module subset
    (`k3/module.cee`) meets C4's module checks against CosmoOS's headers and exports. The whole library compiles as
    kernel code; its only kernel-internal references are `arch_irq_save_hw`/`arch_irq_restore_hw`.
  - **In a booted kernel** (2026-10-29, CosmoOS #489): the self-test `cee-kcore` (CosmoOS
    `kernel/ceesafe/kcore_test.cee`, built with the reference compiler 0.2.0) uses every type but `ObjectCache` as code built into the
    kernel: a table under a static `SpinLock`, a static `IrqSpinLock`, a `PerCpu` counter, boxed locks, a `KRef`'s
    clone and release, a grace period and a deferred free. It passes in debug boots (lockdep on) on AArch64 and
    x86-64. It needed CosmoOS's lockdep class table raised from 512 to 640: main was at about 500, and the test's
    classes overflowed it later in an x86-64 boot.
  - **Static analysis.** CosmoOS's analyzer gate (`make analyze`, clang `--analyze`) reports "Potential memory leak"
    for each `Box.create` in a `KernelHeap`. They are false: each box frees its block on drop. CosmoOS #489 baselines
    the four the test makes. The cause, reduced in plain C (2026-10-29, Apple clang 21): `KernelHeap.allocate`
    returns `Ok(p)`, which the emitter writes as a compound literal with a union member (`(R){ .tag = 0,
    .u.v_Ok = { .f__0 = p } }`), and the analyzer loses the pointer read back from that union. The same value built
    by member stores (`r.u.v_Ok.f__0 = p; return r;`) is tracked, and so is a plain struct.
  - **Fixed in the emitter** (2026-10-29, after 0.2.0; c-backend §2): a variant whose payload may hold a pointer is
    built by a `static inline` constructor of member stores (`ceeMk_<E>_v_<variant>`); scalar payloads keep the
    compound literal. CosmoOS's `kcore_test.cee` analyzes clean on both architectures with it (4 reports with
    0.2.0). A first version used constructors for every payload: gcc then ran `string-building` 1.8× slower, with
    a constructor only on its cold growth path (inferred: inlining). With constructors for pointer payloads only,
    the suite on the Ryzen box (the reference, both compilers, interleaved, two rounds) moves within its noise (±6% where the
    C did not change at all), except memory-copy under clang, 1.10×, whose only change runs once outside the loop
    and which is 1.03× with `-falign-loops=32` (code placement, inferred). CosmoOS's baseline entries can go once
    its pin moves past 0.2.0.

