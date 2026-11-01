# ADR 0030 — M14: what SA3 needs from the reference (one definition, `offsetOf`, the caller's address)

> **Inherited.** ADR 0030 was accepted before CeeSafe's fork (Decision 0006), when the language had its
> earlier name. In this record, "the reference" is that language and project and "the reference compiler" its compiler:
> read them as CeeSafe and `ceesafec`. Links to documents CeeSafe did not keep are given as text.

- Status: **Accepted** (project owner, 2026-10-30), every question as recommended (§5): A, A, A. The wording is the agent's.
- Scope: what CosmoOS's roadmap 2.0 SA3 needs to rewrite `wait.c`, `completion.c`, `semaphore.c`, `mutex.c` and
  `quiesce.c` in the reference with the reference-owned types (the owner's choice, 2026-10-30). SA3's host test, `tests/host/test_sync.c`
  (CosmoOS #496), records the behaviour the reference versions must reproduce.

## 1. Context

What the C uses, and where the reference stands. Each item was checked against the reference compiler 0.5.0 on 2026-10-30.

### 1.1 The same struct, defined twice in one translation unit (blocks SA3)

- **The include graph.** `thread.h` includes `completion.h`, which includes `wait.h`; `mutex.h`, `completion.h` and
  `semaphore.h` include `wait.h`. Once `struct waitqueue` is the reference's (ADR 0029), `wait.h` gets it by including the
  generated header, as `handle.h` does since SA2 (CosmoOS #494).
- **The collision.** A CeeSafe port that reads a thread's fields (`wait.cee` sets `thread->waiting_on`; the mutex reads
  priorities) names `kernel/thread.h` in its `@header`. Its `main.c` then sees `struct waitqueue` twice: once through
  `thread.h` → `completion.h` → `wait.h` → the generated header, and once in its own definition (c-backend §2.3:
  `main.c` defines every `@repr(C)` struct). A second definition of a struct in one translation unit is an error
  (C11 §6.7.2.3p1).
- **Why SA2 did not meet it.** `handle.cee` names `spinlock.h` and `object.h`, and neither includes `handle.h`.
- **The guard.** CosmoOS's `cee-gen.py` renames each generated header's guard `CEESAFE_EXPORTS_H` after the file's path,
  so that two generated headers in one file do not hide each other. The guard is not the problem; the second
  definition in `main.c` is.

### 1.2 A struct from a pointer to its member (`container_of`)

- **The C.** `list_for_each_entry(e, head, link)` and `container_of` recover a struct from its embedded `list_node`:
  a wait entry from its `link`, and a mutex from its `pi_link` in the owner's `pi_held` list. The second member is not
  at offset 0.
- **The reference.** ADR 0009 §2 proposed `Ptr.containerOf` (unsafe). Its implementation note moved it to C7d, and C7d's
  generated callbacks never needed it, so **it does not exist** (no match in `src/` or `lib/`). Nothing gives a field's
  offset as a value either. `layoutOf<T>()` (ADR 0027) gives only size and alignment.

### 1.3 The caller's address (lockdep)

- **The C.** `mutex_lock`, `mutex_trylock`, `mutex_unlock`, `complete` and the completion waits pass
  `(uintptr_t)__builtin_return_address(0)` to lockdep, which names the acquiring and releasing call sites in its
  reports (`docs/kernel/lockdep/`, CosmoOS L21).
- **The reference.** Nothing gives it. A CeeSafe function's C is `static`, and the C compiler may inline it into its exported
  wrapper, so even a raw `__builtin_return_address` in its body would name different frames depending on the
  optimiser.

### 1.4 What is not a gap (checked)

- **Static inline functions of a header.** An `extern "C"` declaration of `list_init`, which `list.h` defines
  `static inline`, compiles warning-free under CosmoOS's flags. The generated prototype `void list_init(struct
  list_node *);` follows the inline definition, has no storage class, and so takes the definition's internal linkage
  (C11 §6.2.2p4–5). `raw_this_cpu`, `list_*`, `wait_entry_init` and `thread_current` are all callable already. The
  spec does not say so; §3.4 adds it.
- **Function pointers in a `@repr(C)` struct** (`wait_entry.fn`) and calls through them: type-system §29.5. The
  generated typedef and C's `wait_callback_fn` name compatible types.
- **Macros that are code** (`might_sleep`, `list_for_each_entry`, `wait_event`): `might_sleep` is three lines over
  `raw_this_cpu()` and `lockdep_might_sleep`, which a port writes in the reference. A list walk is a loop over `next` once
  §1.2 is solved. `wait_event` stays a C macro for C callers, over the exported functions.
- **`panic`'s format** stays in a floor, as in SA2.

## 2. Options

### 2.1 One definition (§1.1)

| | A. `main.c` includes its exports header | B. `@cdefined` structs | C. No the reference-owned types where the graph loops |
|---|---|---|---|
| What changes | a `@repr(C)` struct is defined only in the exports header, which `main.c` includes; `--exports-header <path>` names the header (the `#include` and the guard) | an attribute marks a struct whose definition a `@header` brings, and `main.c` asserts its members instead of defining it | the type stays C's, imported (§31) |
| The definition | one, generated, used by both | The reference's, but written in C's header by hand or by a script | C's |
| `cee-gen.py` | passes `--exports-header kernel/include/kernel/wait_types.h`; no guard renaming | unchanged | unchanged |
| SA3's choice of the reference-owned types | kept | kept, with two copies to keep in step | given up for waitqueue, completion and mutex |

### 2.2 `container_of` (§1.2)

| | A. `offsetOf(T, field)` | B. `Ptr.containerOf(p, T.field)` only | C. Keep list walks in a C floor |
|---|---|---|---|
| Form | a built-in, `usize`, C's `offsetof` | an unsafe built-in for the one use | none |
| Reach | `container_of`, and any other offset (an embedded member's address from its struct's) | `container_of` | — |

### 2.3 The caller's address (§1.3)

| | A. `callerAddress()` in an export | B. Floor wrappers | C. Report no address |
|---|---|---|---|
| Form | a built-in, valid only in an `@export` function: the exported symbol's return address | each exported lock function is C that captures the address and calls a CeeSafe function with it | lockdep gets 0 |
| Lockdep reports | as today | as today | lose the call sites |
| C per primitive | none | one wrapper per entry point (about a dozen) | none |

## 3. Decision: A, A, A

### 3.1 One definition

- `ceesafec build --exports-header <path>` names the exports header: `<path>` is the name C code includes, and the
  guard is derived from it (`kernel/include/kernel/wait_types.h` → `KERNEL_INCLUDE_KERNEL_WAIT_TYPES_H`). Without it
  the header is `ceesafe_exports.h`, as today.
- `main.c` (and, with `--split`, every unit) includes the exports header, after the `@header`s, and does not define
  the `@repr(C)` structs itself. Their layout assertions follow the include, in `main.c`. A `@repr(C)` struct is then
  defined once in every translation unit that sees it, however many headers bring it in, so a kernel header may
  include the generated header and a CeeSafe port may include that kernel header.
- The header's `@header` includes come before its own definitions, as today (c-backend §2.3).

### 3.2 `offsetOf(T, field)`

- `offsetOf(T, f)` is a built-in whose value is the offset in bytes of field `f` of the struct type `T`, as `usize`.
  `T` is a struct with a field `f` (a non-struct `T` or a missing field is N0339, new). For a C-laid-out `T`, or an
  imported one, it is C's `offsetof`, which the C compiler computes (§33.1); otherwise it is the layout's offset, a
  constant.
- It is a static initialiser (§39.4), like `layoutOf<T>()`.
- It is not `unsafe`: it reads no memory. Using it to recover a struct, as `(p as *mut u8).sub(offsetOf(T, f)) as
  *mut T`, is raw-pointer arithmetic, which is (`unsafe.md` U2). lib/kernel adds `containerOf` and a list walk over
  `list_node` on it (§3.5).

### 3.3 `callerAddress()`

- `callerAddress()` is a built-in of type `usize`, valid only in the body of an `@export` function (N0340 elsewhere,
  new). Its value is the address the exported symbol's call returns to: C's `__builtin_return_address(0)` taken in
  the export's wrapper, which passes it to the function's body as a hidden argument. So it does not depend on
  inlining. The reference calls of the same function get 0, since no C frame called them.
- MSVC: `_ReturnAddress()` (c-backend §1.1); the interpreter gives 0.
- It is not `unsafe`: it is a number, not a pointer one may follow.

### 3.4 Static inline functions (spec only)

type-system §28.2 says what §1.4 found: an `extern "C"` function that a `@header` defines `static inline` may be
declared and called. The generated prototype takes the definition's internal linkage (C11 §6.2.2p4).

### 3.5 lib/kernel

`lib/kernel/list.cee` binds CosmoOS's `struct list_node` and its inline helpers (`list_init`, `list_push_back`,
`list_remove`, `list_empty`) and adds `containerOf` over `offsetOf`, with unsafe iteration that may remove the
current entry, as `list_for_each_entry_safe` does. `integrations/cosmoos/k3.py` checks it against CosmoOS's headers
with the rest.

## 4. Consequences

- SA3's ports can be written with the reference-owned types and only the format of `panic` in C. Each primitive's exported
  functions keep their C names and types, so the 669 call sites and the `*_INIT` macros are unchanged.
- **`main.c` no longer defines `@repr(C)` structs.** A build with `--emit c` must keep the exports header beside
  `main.c`, as it already writes it. FFI tests check that every C configuration still compiles.
- **Tests.**
  - The run corpus gets a program whose `@header` includes its own exports header, which used to be a redefinition
    error.
  - `offsetOf` gets tests on a CeeSafe struct, a C-laid-out one and an imported one, against C's `offsetof`.
  - `callerAddress()` gets a test in an export called from C, comparing against the C caller's own
    `__builtin_return_address`.
  - Check-fail tests cover N0339 and N0340.
- **Release.** It needs a the reference compiler release (0.6.0) before SA3's conversion PRs. The host test (#496) does not wait.

## 5. Questions for the owner

1. **One definition (§2.1):** A (recommended), B or C?
2. **Offsets (§2.2):** A, `offsetOf(T, field)` (recommended), B or C?
3. **The caller's address (§2.3):** A, `callerAddress()` (recommended), B or C?

## 6. Implementation (2026-10-30)

Two forms differ from §3, for the reason `layoutOf<T>()` differs from ADR 0027's `Layout.of<T>()`:
- **`offsetOf<T>(field)`**, not `offsetOf(T, field)`: a type is no expression in the reference, so `T` is a type argument.
- **`containerOf(node, offset)` returns `*mut void`**, which the caller casts, and is not generic: the struct that
  embeds a node is usually C-laid-out, and no type argument may be (type-system §33.1). It is a top-level function of
  lib/kernel, since the reference has no type arguments on a member call.

What changed:
- **One definition** (`CEmitter`): `HeaderStructs` is the set the exports header defines (the `@repr(C)` structs and the
  CeeSafe structs they hold); `main.c` includes the header after the `@header`s and skips their definitions, keeping the
  layout assertions; `--exports-header` (`CEmitOptions.ExportsHeader`) names the header and its guard.
- **`offsetOf`**: `TypeChecker.CheckOffsetOf`, a literal for a reference layout, `BuiltinFunction.OffsetOf` (C's
  `offsetof`) for a C-laid-out or imported type, in static initialisers too; an interpreter fault for the latter.
- **`callerAddress()`**: `TypeChecker.CheckCallerAddress` marks the export (`HirFunction.UsesCallerAddress`); its C
  gets a hidden last parameter `cee_caller`, which the export's wrapper fills with `cee_return_address()` (rt.h) and
  The reference's calls and callback wrappers with 0. The interpreter gives 0.
- **lib/kernel/list.cee**: `list_node`, `List.init`/`isEmpty`/`pushBack`/`pushFront`/`remove`/`first`/`next` over
  list.h's inline helpers, and `containerOf`.
- **Tests:**
  - `ffi/includes-exports.cee` (a `@header` that includes the program's own exports header; `callerAddress()` from
    two C call sites at `-O0` and `-O2`, and 0 from the reference);
  - `ffi/exported-c-state.cee` (`offsetOf` on a reference layout, a C-laid-out struct, an imported one and in a static,
    each equal to C's `offsetof`);
  - `check-fail/offset-of.cee` and `check-fail/caller-address.cee`.

  A walk over a list with removals was checked against CosmoOS's `list.h` on the host (2026-10-30: sum 15, the odd
  nodes unlinked).
