# ADR 0250 — C array members reached without the array struct

- Status: **Accepted** 2026-11-26. Proposed 2026-11-25 after 0.95.11's 64-bit Linux contract suite, when the owner
  chose "-fno-strict-aliasing now" for the next release (0.95.13 shipped it) and this ADR for the redesign that removes
  the flag. The owner's answers to §6: "Every array borrow" and, for exports, "Change, with a release note".
- Scope: the C backend (`src/backend/c_body.cee`: places, borrows, slices, whole copies; `c_names.cee`: a borrowed
  array's C type), `src/host/toolchain.cee` (the flag, removed), c-backend.md §1 and §2, type-system.md §31.3,
  tooling.md §8, the corpus.

## 1. Context

- **What the C has.** A `@repr(C)` struct's, or an imported struct's, array field is a C array member, `uint8_t
  data[3]` (`CE f[N][M]` for nested arrays, type-system.md §28.5, §31.3, ADR 0029), so C code sees the layout it
  declares.
- **How the backend reached it.** As the reference's array struct over the same storage:
  `(*(ceeA3_u8 *)(uintptr_t)&s.data)`, then `.e[i]` for an element, or the whole struct for a copy, a borrow or an
  argument. A borrowed array was a pointer to the array struct, read as `(*p).e[i]`. The layouts are equal, but the
  types are not: the object is a `uint8_t[3]` inside `struct Grid`, and the access went through `struct ceeA3_u8`,
  which strict aliasing does not allow, for an element as for the whole. (type-system.md §31.3 had called it an access
  through an aggregate containing the member's type, C11 6.5p7; gcc does not read the rule that way.)
- **What it cost.** gcc 14.2 at `-O2` (Debian 13) read and wrote such members as zeros once it could see the object:
  `ffi/exported-c-state.cee` printed 1 and 326 for 71 and 369, in 0.95.10 as in 0.95.11's candidates, and
  `run-pass/repr-c-array-copies.cee` printed 11 for 71 after whole copies were made bytes. Clang and gcc 13 happened
  not to. 0.95.13 copies a whole array as bytes and compiles with `-fno-strict-aliasing`, against c-backend.md §1's
  aim that the C is correct without it; a user who compiles `--emit c` output must pass the flag too.

## 2. Decision

1. **A borrowed array is a pointer to its first innermost element** (§6.1: every array borrow, one rule).
   `borrow [T; N]` is `const T *`, `borrow mut [T; N]` is `T *`, with `T` the innermost element (`u8` for
   `[[u8; 3]; 2]`), whatever the borrow is of: one of CeeSafe's arrays (`((const uint8_t *)&a)`), a C array member, an
   inner array of either. The length is the type's. An element through it is `p[i]`; an inner array is `p + i * M`,
   `M` the inner array's count of innermost elements, and stays a pointer until an index reaches an element that is
   not an array.
2. **A C array member is reached the same way**: from its own address converted to the innermost element's type,
   `((T *)(uintptr_t)&s.data)[i]` (through `uintptr_t`, as the owner may be reached through a `const` pointer). No
   array struct and no cast to one.
3. **An array element of a slice or vector likewise**: a `Slice<[T; M]>` may be a slice of a nested C array member
   (`cells[..]`), so its elements are reached from `((T *)(uintptr_t)s.ptr + i * M)`. The slice value itself is
   unchanged (`ceeSl_A<M>_<T>`, its `ptr` the array struct's type, never accessed through).
4. **A whole array is copied as bytes** wherever it is reached through any of these: read with
   `cee_read_unaligned(A, p)`, written from a typed local of the array struct with `ceesafe_mem_copy` (a value, a
   literal, a repeat or zeros), as 0.95.13 already did for C array members.
5. **The flag goes.** ceesafec passes `-fno-strict-aliasing` no longer; c-backend.md §1 says again that the C is
   correct with or without it.
6. **Exports** (§6.2: "Change, with a release note"). No export signature changes after all: references never cross
   the C boundary (type-system.md §28.3, N0291), so no `@export` function takes `borrow [T; N]`, and the exports
   header is as before. What changes is the C of CeeSafe's own functions in `--emit c` output, which is not an
   interface; the release note says so.

## 3. Consequences

- The generated C is correct under strict aliasing again, and C a user compiles is correct with the compiler's
  defaults. gcc and clang optimise CeeSafe's code with type-based alias analysis.
- A borrowed array's C type changed everywhere it appears: parameters, results, locals, fields and generic instances.
  `Secret.wipe` of a borrowed place now takes the referent's size from its type, since `sizeof *p` is one element.
- `emit-c` snapshots change where a borrowed array appears (`emit-c/adr0197-secret-wipe.c`).
- Not covered: a raw pointer to an array (`*[T; N]`, `p.read()` is `(*p)`) is still read as the array struct. No
  FFI-safe pointer type is a pointer to an array (§28.3), so such a pointer points to CeeSafe's own storage unless a
  program converts one from `*void`.

## 4. Options not taken

- **Keep `-fno-strict-aliasing` for good**: simple and what CosmoOS does, but every user's C build needs it, and the
  optimisation it costs is real for numeric code.
- **Declare the members as the array struct in the program's own C**: `ceeA3_u8 data;` in CeeSafe's definition of a
  `@repr(C)` struct, `uint8_t data[3]` in the header C code includes. Two definitions of one struct type across
  translation units are not compatible types: undefined behaviour of its own once LTO joins them, and an imported
  struct's definition is the C header's, not CeeSafe's.
- **`__attribute__((may_alias))` on the array structs**: gcc and clang only, and it weakens the alias analysis of
  every array in the program, not only those over C storage.
- **Change only the borrows that can reach C storage** (§6.1's other answer): a function is compiled once for every
  caller, so such a borrow would have to pass a typed copy and copy it back after the call, which is wrong for a
  borrow kept past the call or watched by C while it runs.

## 5. Tests

- `run-pass/adr0250-borrowed-c-arrays.cee`: borrowed arrays of a local, of a C array member, of a nested one and of its
  inner arrays, of a static's member, and one returned from a function; elements, inner arrays, whole copies,
  literals and repeats written through them; slices of them, and of a nested one's inner arrays.
- `run-pass/repr-c-array-copies.cee`, `ffi/exported-c-state.cee`, `ffi/c-arrays.cee`, and the ffi, run-pass, emit-c,
  closure-calls and frames families under gcc 14 at `-O2` and `-O3` on Debian 13, without `-fno-strict-aliasing`.
- The contract suite on macOS and 64-bit Linux.

## 6. Questions for the owner (answered)

1. **Borrowed arrays**: change the C of every `borrow [T; N]` (one rule, wider change), or only of borrows that can
   reach C storage? **Every array borrow** (§2.1).
2. **`@export` functions taking `borrow [T; N]`**: a release note, or keep the old signature? **Change, with a
   release note** (§2.6: there are none, since references do not cross exports).
