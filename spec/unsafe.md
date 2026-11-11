# Unsafe Code — Contract and Checks

Status: **normative** from M8a (2026-10-24). Rules U1–U3 are in `ownership.md`; the typing rules
are in `type-system.md` §27; the decisions are in ADR 0007.

Safe CeeSafe code cannot cause undefined behaviour (master prompt §6, `ownership.md` §1). Unsafe code
can. This document says what unsafe code must guarantee, so that safe code keeps that property, and
what the toolchain checks.

## 1. What unsafe permits

Exactly the operations of `type-system.md` §27.3, and calls to `extern "C"` functions (§28.2;
M8b). Later increments add MMIO accesses:
- reading and writing through a raw pointer;
- pointer arithmetic (`add`, `sub`, `offset`);
- making references and slices from raw parts (`Ptr.toBorrow`, `Ptr.toBorrowMut`, `Slice.fromRaw`,
  `SliceMut.fromRaw`);
- making a box or a vector from raw parts (`Box.fromRaw`, `Vector.fromRaw`), setting a vector's length over
  elements written through a pointer (`v.setLength(n)`), and moving a vector's buffer to or from a host
  (`Vector.intoForeign`, `Vector.fromForeign`): `type-system.md` §34.2 and §23.3;
- calling an `unsafe fn`.

Making, copying, comparing and casting pointers is safe (§27.2): a pointer that is never used
cannot do harm. An unsafe context relaxes nothing else (`type-system.md` §27.4).

## 2. The contract (U2)

Every unsafe operation must be **valid** when it runs:

| Operation | Requirement |
|---|---|
| `p.read()` | `p` is non-null, aligned for `T`, and points to a live, initialised `T` inside one object. For a `T` that is not Copy, the value is moved out (ADR 0027): until the next `p.write`, nothing may read, drop or borrow the `T` at `p`, or the value would have two owners (O1) |
| `p.write(v)` | `p` is non-null, aligned for `T`, and points to live storage for a `T` inside one object, which nothing currently borrows (no live loan of it, shared or mutable; a shared loan of a `RawCell`, or of a value holding it, does not count for the cell's contents, type-system.md §41.1). The old contents are not dropped: if they held a `T` that needs drop, it leaks unless it was read out first |
| `p.add/sub/offset(n)` | always defined (the address wraps); the *result* must meet the requirements above before it is used |
| `Ptr.toBorrow(p)` | as `read`, for as long as the reference is used; nothing writes the `T` meanwhile (except through the reference itself, for `toBorrowMut`), and for `toBorrowMut` nothing else reads it either |
| `Slice.fromRaw(p, n)` | as `Ptr.toBorrow`, for each of the `n` elements; `n × size(T)` ≤ 2^63 − 1 |
| calling an `unsafe fn` | the function's own `Safety:` contract |
| calling an `extern "C"` function | the C function's documented contract, and the declaration's types match its definition; pointers it is given are valid for what it does with them, for as long as it keeps them; values it returns meet CeeSafe's type invariants (`@repr` enums are checked) |

**Pointers made from references** (M9c, ADR 0010): a pointer made from a reference (`r as *T`, `r as *mut T`, or
`s.ptr` of a slice) may be used to **write** its target, directly or through a reference made from it by
`Ptr.toBorrowMut` or `SliceMut.fromRaw`, only while the reference's loan could still be live. That is, only until the
target is next used by anything other than that pointer: read, written, borrowed, moved, or its storage ended. After
that, such a write is not valid even though the storage is live and nothing borrows it. The one exception is a
`RawCell`'s contents, reached through `c.pointer()` (type-system.md §41.1; ADR 0084): they may be written while
shared loans of the cell are live, and a reference made from that pointer by `Ptr.toBorrowMut` is valid while no other
reference made from it is used. Optimisations rely on this

(`optimisations.md` §2): a vector's length, read directly, stays what it was until the next `borrow mut` of the vector.

"Live" means the object's storage has not ended: the local is in scope (between its
`StorageLive` and `StorageDead`), the box or vector buffer has not been freed, the region has not
been reset or destroyed. "Inside one object" means within one local, one heap block or one region
allocation, as laid out by `TargetInfo`.

In addition, unsafe code must leave every **safe-code invariant** intact *as observed by safe
code* (U2): ownership (O1–O5: a value has one owner, is destroyed once), borrowing (B1–B5: no
write to a place while a shared loan of it is live, no second access while a mutable loan is live),
regions (R1–R4), the heap (H1–H3), and the type invariants (a `bool` is 0 or 1, an enum's tag is
one of its variants, a `StringView` is UTF-8). For example, writing `2` through a `*mut u8` that
points at a `bool` breaks safe code later, even though the write itself is valid.

**Unconstrained references** (U3): a reference made by `Ptr.toBorrow`, `Ptr.toBorrowMut` or `fromRaw`
holds no loans, so the borrow checker cannot see what it refers to. The unsafe code must bound it.
The usual way is a safe function whose signature ties the result to a parameter, so that safe
callers are checked against that parameter:

```ceesafe
struct Buffer {
    data: *mut u8;
    length: usize;

    /// Safety: `data` points to `length` live bytes owned by this buffer (the constructor's contract).
    public fn bytes(borrow this) -> Slice<u8> => unsafe { Slice.fromRaw(this.data, this.length) };
}
```

A caller's slice from `b.bytes()` borrows `b`, so `b` cannot be changed or destroyed while it is
used. Without such a bound, the reference could outlive its target, and nothing would report it.

## 3. The `Safety:` rule

Each `unsafe fn` and each function containing `unsafe` carries a doc comment with a `Safety:`
line (N0283; `type-system.md` §27.4):
- for an `unsafe fn`, it states what the **caller** must guarantee;
- for a safe function, it explains why the unsafe operations inside meet §2 **for every safe
  caller**. If that argument depends on something the caller could break, the function should be
  `unsafe` instead.

The compiler checks only that the comment exists. Reviews check that it is true.

## 4. Lowering to C (c-backend §2)

| CeeSafe | C |
|---|---|
| `*T`, `*mut T` | `T *` (C's type of `T`); `*void` is `void *`. No `const`: read-only is a CeeSafe rule, and CosmoOS's `-Wcast-qual` would reject casting it away |
| `p.read()` / `p.write(v)` | `(*p)` / `(*p) = v` |
| `p.add(n)` | `(T *)(uintptr_t)((uintptr_t)p + (uintptr_t)n * sizeof(T))`; `sub` subtracts, and `offset` converts the signed `n` to `uintptr_t` first (modulo 2^64) |
| `r as *T` | `(T *)(uintptr_t)r` |
| `p as *U` | `(U *)(uintptr_t)p` (no pointer-to-pointer cast, so no `-Wcast-align` or `-Wcast-qual`) |
| `p as usize` / `n as *T` | `(uint64_t)(uintptr_t)p` / `(T *)(uintptr_t)n` |
| `0 as *T`, `p.isNull()` | `(T *)(uintptr_t)0`, `p == NULL` |
| `Ptr.toBorrow(p)` | `p` (a C pointer, as references are) |
| `Slice.fromRaw(p, n)` | the slice struct `{ .ptr = p, .length = n }` |

C itself makes pointer arithmetic outside an object, and comparisons of unrelated pointers with
`<`, undefined. CeeSafe's lowering avoids both: arithmetic goes through `uintptr_t`, and pointers have
no ordering (only `==`/`!=`, which C defines for any two pointers). A conversion between a pointer
and `uintptr_t` is implementation-defined in C11 (6.3.2.3); clang and GCC define it as the address
on every target CeeSafe supports (inferred from their documentation; checked on the corpus by the
sanitizers and the interpreter).

## 5. What is checked

- **Statically**: the capability rule (U1, N0280), pointee and operation rules (N0281, N0282),
  the `Safety:` comment (N0283). Safe code is checked as before (`type-system.md` §27.4).
- **The interpreter** (`intermediate-representation.md` §5) runs unsafe code with its model of
  memory, and reports as an interpreter fault (exit status 70): a null pointer; an address in no
  live object, or in an object whose storage has ended; an access that does not fit inside the
  object; a misaligned access; reading uninitialised bytes. It does not track loans (no Stacked
  Borrows), so it does not detect a write through a pointer while safe code holds a loan of the
  target.
- **Natively**, the run corpus also runs with ASan and UBSan.
- **Not checked**: that the `Safety:` comment is true; the type invariants of §2 (a bad `bool`
  written through a pointer is caught only when it misbehaves); the rule for pointers made from references (§2),
  since the interpreter does not track loans.
