# ADR 0091 — Read-only C objects: `static` tables of C functions and C structs

- Status: **§2.1 and §2.2 accepted** by the owner, 2026-11-11 ("I accept parts 1 and 2 of ADR 0091"); §2.3 (array
  literals by index) is proposed and not decided. Implementation on branch `impl/0091`. Raised by CosmoOS's roadmap 1.3 (its row C2, `docs/roadmap-1.3.md`): the last C in
  `kernel/syscall/` is a table of functions and the record that points at it, and CeeSafe cannot write either as C
  declares them.
- Scope: `spec/type-system.md` §38.1, §39.2, §39.4 and §28.4; the checker's `isSend` (`src/types/types.cee`); the C
  emitter's statics; one new form of array literal (§2.3, separable). No runtime or library change.

## 1. Context

- **What a kernel writes.** C kernels describe behaviour with read-only tables: a system-call table, an operations
  struct per file system or object type, a personality record. CosmoOS's C has 60 such definitions
  (`static const … _ops`, `_type`, tables; counted 2026-11-11 with `grep` over `kernel`, `kernel-services`, `drivers`
  and `compat`). The headers declare the shared ones `const`:

  ```c
  typedef int64_t (*syscall_fn)(struct syscall_args *a);
  extern const struct personality personality_native;          /* process.h */
  static const syscall_fn native_table[SYS_COUNT] = { [SYS_exit] = sys_exit, [SYS_write] = sys_write, … };
  const struct personality personality_native = { .name = "native", .table = native_table, .count = SYS_COUNT, … };
  ```

- **What CeeSafe accepts today** (probes of 2026-11-11, `ceesafec check --profile freestanding --no-float` and
  `build --emit c`, the same results on 0.24.2 and 0.29.0):

  | Source | Result |
  |---|---|
  | `static T: [extern "C" fn(*mut args) -> i64; 3] = [one, sys_c, 0 as …];` | N0337: the type is not `Share` |
  | the same as `static mut` | accepted; emitted without `const` |
  | `@export("personality_native") static P: personality = personality { … };` (an imported struct) | accepted; emitted `const struct personality personality_native = {…}` (ADR 0080 §3.2) |
  | the same without `@export` | N0337: "C writes a C object, so declare it 'static mut'" |

  So ADR 0080 already made the exported record C's `const` object. What is missing is the table it points at, and
  any read-only C object that is not exported: an operations table private to its file, an array of them.

  (This ADR's first draft said the exported record was refused. That was wrong: the probe behind it had a second
  error on an earlier line. The row above is the corrected result, found while reading `src/check/statics.cee`.)

- **Why `static mut` is not enough for the table.**
  1. The table lands in writable data. A system-call table an attacker can write is a classic kernel target; `const`
     puts it in `.rodata`, where CosmoOS's C has it today (`static const syscall_fn native_table[]`).
  2. Every use of a `static mut` is an unsafe operation (§39.3), for data nothing ever writes.
  3. A file-local operations struct has the same two costs, and C's `static const struct … _ops` is the commonest
     shape of all.

- **What it costs.** `kernel/syscall/native.c` (266 lines) and the Linux personality's table stay C for this reason
  alone, and so will each operations table the conversion reaches.

## 2. Proposal

### 2.1 A C function pointer is `Send` and `Share`

`extern "C" fn(…) -> R` joins `fn(…)` in §38.1: it is a code address, the same value in every thread, and calling it
is governed by the function's own contract, not by the pointer's. In `isSendCore`, `TypeKind.FunctionPointer` returns
true, as `TypeKind.Function` does. `ReachesSharedChange` does not walk into it.

With this alone, the first row of the table above is accepted, and so is any `Share` struct that holds C function
pointers.

### 2.2 A `static` may be a read-only C object

A plain `static` (not `static mut`) may have a **C-object type**: an imported C struct (§31), a raw pointer, an
`extern "C" fn`, or an array of these, exported or not. This widens ADR 0080 §3.2, which allowed it for an exported
imported struct. Section 39.2's `Share` requirement does not apply to it, for the reason it does not apply to views
of static string data: it cannot change and it outlives everything.

- **Emitted `const`.** The definition is `const T name = …;` (`static const` when not exported), so it agrees with a
  header that declares it `const`, and the linker places it in read-only data. A header that declares it without
  `const` is a C error at the definition, which is the check that C does not intend to write it.
- **What CeeSafe may do with it.** Read its `Copy` fields and elements, borrow it, take its address as `*T` (never
  `*mut T`: N0211, as for any `static`). Following a raw pointer read from it is an unsafe operation as always.
- **Its initialiser** is §39.4's for C objects, unchanged: a struct literal of the imported type, function names,
  addresses of statics, null.
- **Refused** (N0337 stays): a type with an atomic member (`AtomicU32`, …), directly, in an embedded struct or in an
  element, since an atomic exists to be changed. The exported imported struct ADR 0080 already accepted keeps its own
  rule (it is refused `const` only when the static's type is itself an atomic or an array of them), so nothing that
  compiles today changes.
- **Exported** (`@export`): §28.4's rule for exported statics, with `const` in the definition.

A pointer inside a read-only object may point at a `static mut`: the pointer is constant, what it points at is not,
exactly as `T *const` in C.

### 2.3 Array literals by index (separable)

A table indexed by a number is written in C with designated initialisers, so that an entry sits beside its number and
a missing one is null. A positional literal of 113 entries loses both: one misplaced line moves every call after it.

In a static initialiser, an array literal may give its elements by index:

```ceesafe
static TABLE: [extern "C" fn(*mut syscall_args) -> i64; SYS_COUNT] = [
    SYS_exit: sys_exit,
    SYS_write: sys_write,
    SYS_read: sys_read,
];
```

- Each index is a constant expression of an integer type, less than the array's length (N0338 otherwise), and no
  index appears twice (N0338).
- An element not named is zero: null for pointers and function pointers, 0 for integers, `false` for `bool`, and for
  an imported struct every member zero. Any other element type must name every index (N0338): CeeSafe has no zero
  for it.
- The form is all or nothing: a literal does not mix `index: value` with positional elements.
- It is emitted as C's `[index] = value`.

This is new syntax, so it can be decided apart from §2.1 and §2.2: without it the tables can still be written, in
order, with a comment per line.

## 3. Alternatives

- **Leave the tables in C** (a floor). It is 266 lines today. But the same shape recurs in every subsystem still to
  convert, and each one left in C is a place where a handler's name and signature are checked by nothing in CeeSafe.
- **Make every imported struct `Share`.** Unsound: C mutates most of them (that is what §39.3 records).
- **Infer `const` for a `static mut` nothing writes.** The header's `const` is a contract with C; an inference that
  one more assignment silently reverses is not one.
- **A `@const` attribute on `static mut`.** A second spelling of what `static` already means.

## 4. What changes for existing code

Nothing that compiles today changes meaning: 2.1 and 2.2 accept programs that were N0337. A `static mut` C object
stays as it is.

## 5. Tests

- Corpus `ffi/c-const-objects.cee` (new): a `static` table of a `@callback`, an `@export` and an `extern "C"`
  function and a null; the exported record that points at it, declared `extern const` by its header; an
  unexported read-only struct; C calls through the table and CeeSafe calls through both.
- Corpus `check-fail/static-c-state.cee` (changed, an intended change of an expectation): its plain static of an
  imported struct was N0337 and is now accepted; it gains a struct with an atomic member and an array of them
  (N0337 each), and an assignment to the read-only static (N0211).
- Not written: §2.3's cases (it is not accepted), and a test that a header declaring the object without `const`
  fails to compile (the corpus has no family for a C compile that must fail).

## 6. Questions for the owner

1. Accept §2.1 and §2.2?
2. Accept §2.3 (index literals) with them, later, or not at all?
3. Release alone, or with the header import (CosmoOS's row C1, a separate ADR)?

## 7. Implementation and evidence (2026-11-11, branch `impl/0091`, on 0.31.0)

### 7.1 Where it is

- `src/types/types.cee`: `isSendCore` answers true for `TypeKind.FunctionPointer` (§2.1); `isCObject` (§2.2).
- `src/check/statics.cee`: a plain static of a C-object type with no atomic member is not asked for `Share`
  (`holdsAtomicMember`); N0337's text for an imported type now names the atomic member.
- `src/backend/c_skeleton.cee`: `staticIsConst` adds the C objects to the exported statics, so they are emitted
  `const`.
- `spec/type-system.md` §38.1, §39.2, §39.3, §39.6; `guide/unsafe-and-c.md` ("Read-only tables", a checked example).

### 7.2 Runs (macOS arm64, Apple clang; the compiler built by the 0.31.0 release binary)

| Command | Result |
|---|---|
| `tools/run_corpus.py --compiler out/ceesafec --corpus tests/reference/tests/corpus` | 811 files: 0 fail (4 freestanding partial, as before) |
| `tools/contract_tests.py --ceesafec out/ceesafec` | every line ok except `spec`, whose one problem was the embedded documents not yet regenerated |
| after `tools/gen_docs.py`, the diagnostic's new text and a rebuild: `tools/spec_check.py` | 0 problems |
| the same build: corpus families `check-fail`, `check-pass`, `ffi` | 251, 38 and 19 pass |
| `tools/guide_check.py` | 155 examples, 0 problems |

Not run: the whole contract suite after the last rebuild (only the three corpus families, the spec check and the
guide check were repeated), the fixed point, Linux, and CosmoOS's tree with this compiler.

### 7.3 What CosmoOS can now write

The probe that started this, with both statics plain (`static TABLE: [extern "C" fn(*mut args) -> i64; 3]` and
`@export("personality_native") static NATIVE: personality`), is emitted as

```c
static CEE_UNUSED const ceeA3_F1MS4argsi64 ceeS_TABLE = { .e = { sys_one, sys_c, ((ceeP_F1MS4argsi64)0) } };
const struct personality personality_native = { .name = ceeSC_0, .table = ((syscall_fn *)…&ceeS_TABLE), … };
```

Without §2.3 the table's entries are positional: CosmoOS's 113-entry table would be written in number order.
