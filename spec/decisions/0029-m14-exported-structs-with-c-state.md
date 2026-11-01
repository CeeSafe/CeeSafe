# ADR 0029 — M14: exported structs that hold C state

> **Inherited.** ADR 0029 was accepted before CeeSafe's fork (Decision 0006), when the language had its
> earlier name. In this record, "the reference" is that language and project and "the reference compiler" its compiler:
> read them as CeeSafe and `ceesafec`. Links to documents CeeSafe did not keep are given as text.

- Status: **Accepted** (project owner, 2026-10-30), both questions as recommended (§5): option A, with arrays in this ADR. The wording is the agent's.
- Scope: a struct defined in the reference and used by C by value, whose fields include C state (a `spinlock_t`) and fixed
  arrays. CosmoOS's roadmap 2.0 SA2 needs it for the handle table; the owner chose the reference-owned types for SA2
  (2026-10-30), and SA3 and later conversions need it for every C struct that embeds a lock.

## 1. Context

### 1.1 The handle table

- **The C.** `struct handle_table` (CosmoOS `kernel/include/kernel/handle.h:86`) is a `spinlock_t`, an array of 64
  `struct handle_entry { struct kobject *obj; unsigned rights; unsigned flags; }`, two counts and a `bool`.
  `handle.c` (266 lines) is its only code apart from three writes of `limit` in `process.c` (`:965`, `:1211`,
  `:2415`).
- **C makes the values.** The table is embedded by value in `struct process` (`process.h:180`), and self-tests put
  it in statics and on the stack (`epolltest.c:112`, `:447`, `:530`, `:627`, `:660`; `memtest.c:994`). It is
  initialised in place by `handle_table_init(t)` and torn down by `handle_table_destroy(t)`. CeeSafe code would only
  ever see it through a `*mut handle_table` from C.
- **SA2 so far.** The kobject layer is the reference over C's `struct kobject` (CosmoOS #493): an *imported* struct, which
  works today. The owner's choice for SA2 was that the types themselves become the reference's, so the definition
  `struct handle_table { … }` must come from the reference's generated header, not from `handle.h`.

### 1.2 What the reference allows today

- **`@repr(C)` structs** (type-system §28.5) cross to C and are defined in `ceesafe_exports.h`, but their fields may only
  be integers, `bool`, raw pointers and other `@repr(C)` structs. A `spinlock_t` field is N0291 (checked with the reference compiler
  0.4.0, 2026-10-30: "a @repr(C) struct cannot hold a 'spinlock_t'"), and so is an array field (§28.3: arrays do not
  cross).
- **C state by value** (§33, C7b) is allowed in a *the reference* struct, which then becomes C-laid-out (§33.1): the C
  compiler lays it out, it is never moved (§33.2), and it is made only by `Box.create` with a `@placed`
  initialiser (§33.3). Such a struct is `struct ceeT_S` in `main.c` only: C code cannot name it.
- **Arrays in imported structs** are allowed (ADR 0014, §31): `f: [E; N]` is seen as the reference's array struct over the C
  member, with no copy.

So each half exists: C-laid-out structs inside the reference, and C-visible structs without C state. Nothing joins them.

## 2. Options

| | A. `@repr(C)` may hold C state and arrays | B. A new `@cstruct` kind | C. Keep the type C's |
|---|---|---|---|
| Language | lift two N0291 cases of §28.5; §33's rules apply as they stand | a third struct kind with its own rules | none |
| Who defines `struct handle_table` | The reference (`ceesafe_exports.h`) | The reference | C (`handle.h`), imported by the reference |
| SA2's "the reference-owned types" | yes | yes | no: the code is the reference, the type C's |
| New rules to learn | none: a `@repr(C)` struct with C state is C-laid-out, as any struct with C state is | a kind that is both | none |
| Later conversions (SA3: `struct mutex`, wait queues, `struct process`) | the same rule | the same | each type stays C's |

## 3. Decision: A

### 3.1 Fields

A `@repr(C)` struct's fields may also be:

- **an imported C type by value** (§31), such as `spinlock_t`. The struct is then C-laid-out (§33.1);
- **an array `[E; N]`** whose element is an integer, `bool`, a raw pointer as in §28.3, a `@repr(C)` struct, or an
  imported type. Inside the reference the field is the reference's array; in C it is the member `CE f[N]`.

Every other N0291 case stays: references, slices, boxes, vectors, optionals, results, non-`@repr` structs and
enums, and `@repr` enums as fields. None of the allowed field types needs drop; in addition, a `@repr(C)` struct
with C state may not declare a `drop` method (N0291, new): C makes, embeds and discards these values without calling
The reference, so a drop would silently not run.

### 3.2 A `@repr(C)` struct with C state

It is C-laid-out (§33.1), with every rule of §33 unchanged:

- it is never moved, copied or assigned in the reference (N0294), and crosses to C only behind a pointer (`*T`, `*mut T`;
  §28.3's by-value crossing is for `@repr(C)` structs without C state);
- The reference makes one only through `Box.create` with its `@placed` initialiser (§33.2–§33.3), or in a static (§39.4);
- C makes one wherever C makes a struct (embedded, static, on the stack). CeeSafe code that receives a pointer to
  one from C relies on C having initialised it, as with any pointer from C: the exported function that takes it
  says so in its `/// Safety:` comment (§27.4).

### 3.3 C

- **`ceesafe_exports.h`** defines the struct as `struct S { … }` with members named as in the reference, after the `@header`
  includes that declare its imported member types (it includes them already, c-backend §2.3), and after the
  `@repr(C)` structs it contains. There is no layout assertion, as for every C-laid-out type: the C compiler lays
  it out, and `layoutOf<T>()` gives C's `sizeof`/`_Alignof` (ADR 0027).
- **Array members** are `CE f[N];` with `N` written as a number. A C header that also names the length (CosmoOS's
  `HANDLE_TABLE_SIZE`) checks it with a `_Static_assert` beside its `#include`. In `main.c` a field place is the
  member seen as the reference's array struct, `(*(ceeA<N>_<E> *)(uintptr_t)&p->f)`, as for ADR 0014's imported arrays, after
  an assertion that the member and the array struct have one size and alignment (the member's type needs no
  `_Generic` check: `main.c` defines it).
- **`main.c`** uses `struct S` for it, as for every `@repr(C)` struct, so both translation units name one type
  (C11 6.2.7).

### 3.4 The interpreter

C-laid-out types are already outside the interpreter (§33.4), and a `@repr(C)` struct with C state is one of
them. Arrays of `@repr(C)` elements in a `@repr(C)` struct without C state run in the interpreter as CeeSafe arrays.

### 3.5 For the handle table (CosmoOS, after a release)

```ceesafe
@header("kernel/spinlock.h", "kernel/object.h")
extern "C" {
    @ctype("spinlock_t")
    struct spinlock_t;
    struct kobject;
}

const HANDLE_TABLE_SIZE: usize = 64;

@repr(C)
struct handle_entry {
    obj: *mut kobject; // null: free
    rights: u32;
    flags: u32;
}

@repr(C)
struct handle_table {
    lock: spinlock_t;
    entries: [handle_entry; 64];
    count: u32;
    limit: u32;
    exiting: bool;
}

/// Safety: `t` points to storage for a table that nothing uses yet.
@export("handle_table_init")
fn tableInit(t: *mut handle_table) { … }
```

`make cee` writes the generated header (`kernel/include/kernel/handle_types.h`, say), and `handle.h` includes it
in place of its own definitions, with `_Static_assert(HANDLE_TABLE_SIZE == 64)`. `process.c` and the self-tests are
unchanged: `unsigned` and `uint32_t` are the same type on both of CosmoOS's targets, and the C sees the same
members. The entries hold `*mut kobject` and count their references by hand, in unsafe code, as the C does: a
`KRef<kobject>` field would need drop glue, which C-made values never run (§3.1).

## 4. Consequences

- SA2's handle table, and later SA3's locks and wait queues and `struct process` itself, can be the reference's types
  without changing how C embeds them.
- C code may write a reference-owned struct's fields (`process.c` writes `limit`). Such a struct is plain data with C
  state: the reference keeps no invariant in it that C could break without `unsafe` on the reference side noticing, which is
  already the rule for anything behind a pointer from C (unsafe.md U2).
- No layout is checked for these structs in the reference; the C compiler is the authority, as for every C-laid-out type.
  The member types are checked in both translation units by the shared definition.
- **Tests.**
  - The run corpus gets a C file that embeds a C-state `@repr(C)` struct by value in a C struct, initialises it
    through an export, and uses its array.
  - Compile-fail tests cover a move of one (N0294), a by-value parameter (N0294), a `drop` method on one (N0291),
    and an array of a non-FFI type (N0291).
  - `k3.py` compiles CosmoOS's header against the generated one.
- **Release.** It needs a the reference compiler release (0.5.0) and a CosmoOS pin move before SA2's third PR.

## 5. Questions for the owner

1. **Which option?** A (recommended), B or C (§2).
2. **Arrays in the same ADR?** Recommended: yes, since the handle table needs both. The alternative is a separate
   ADR for array fields of `@repr(C)` structs.

## 6. Implementation (2026-10-30)

- **Checker** (`TypeChecker.IsFfiSafe`, the `@repr(C)` field loop): a field may be an imported type or an array of an
  allowed element; a `drop` on a C-laid-out `@repr(C)` struct is N0291.
- **C** (`CEmitter`): a `@repr(C)` struct's array field is the C array member in `main.c` and `ceesafe_exports.h`, with
  an assertion that it has the layout of the reference's array struct; places use it through the cast that ADR 0014's imported
  arrays use (`IsCArrayMember`); a struct literal assigns its arrays after the compound literal
  (`EmitConstruction`); a static's initialiser writes them as plain braces (`CArrayInitializer`). The header names an
  imported member by its C spelling and defines no imported struct.
- **Tests.**
  - `ffi/exported-c-state.cee` with `.c` and `.types.h`, under every C configuration: C embeds `xs_table` (a lock, a
    self-pointing list head, an array of `@repr(C)` entries) in its own struct, a static and a stack frame, writes a
    field, and uses it through the reference's exports. `Grid`, a plain `@repr(C)` struct with nested arrays, is a value and a
    static in the reference. The reviewed header is `ffi/exported-c-state.h`.
  - `check-fail/c-state-types.cee`: a `drop` (N0291) and a by-value parameter (N0294).
    `check-fail/ffi-declarations.cee`: array fields are allowed, and arrays of a non-`@repr` struct or a `@repr`
    enum are N0291.
