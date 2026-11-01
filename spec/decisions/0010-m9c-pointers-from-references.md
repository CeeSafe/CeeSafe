# ADR 0010 — M9c: writes through pointers made from references end with the loan

> **Inherited.** ADR 0010 was accepted before CeeSafe's fork (Decision 0006), when the language had its
> earlier name. In this record, "the reference" is that language and project and "the reference compiler" its compiler:
> read them as CeeSafe and `ceesafec`. Links to documents CeeSafe did not keep are given as text.

- Status: **Accepted** (project owner, 2026-10-25, choosing to add the rule so that bounds checks on vectors can be
  eliminated, over limiting M9c to arrays and slices). The wording is the agent's; the owner may revise it.
- Scope: M9c (`milestone-9.md`, `optimisations.md` §2). It amends `unsafe.md` §2 (the U2 contract).

## Context

M9c removes bounds checks that an earlier check or a loop's condition already proves (`optimisations.md` §2). The
proof is about values held in a function's locals: an index, a slice's length, a vector's length. It stays true
until the local changes, and the analysis has to see every change.

A local changes through an assignment, which the analysis sees, or through a reference to it. A slice or scalar
local whose address is never taken cannot change any other way, and needs no new rule. A vector is different:
`push`, `pop` and `clear` take `borrow mut` of it, so every vector has its address taken. The analysis can still
follow it, because a mutable loan is the only way to change the vector's length, and while that loan is live the
borrow checker rejects every other use of the vector (B1–B2). So a length read directly from the vector stays
valid until the next `borrow mut` of it.

That argument fails if the vector can be changed with no live loan. Today's U2 contract allows that: a pointer made
from a reference (`(borrow mut v) as *mut Vector<u8>`; casts are safe, `type-system.md` §27.2) can be kept, by this
function or another, and used later through `Ptr.toBorrowMut(p)` in an `unsafe` block, after the loan it came from
has ended. The write is valid under §2 as written: the storage is live and nothing borrows it at that moment.

## Decision

1. Add a rule to U2 (`unsafe.md` §2): a pointer made from a reference may be used to **write** its target, directly
   or through a reference made from it, only while the reference's loan could still be live. That is, only before the
   target is next used by anything other than that pointer: read, written, borrowed, moved, or its storage ended.
   Pointers that did not come from a reference are not affected: those from the C side, `Slice.fromRaw` of foreign
   memory, MMIO addresses.
2. This is the rule of Rust's raw pointers derived from `&mut` (inferred: Stacked Borrows makes such a later write
   undefined), reduced to writes, which is all M9c relies on.
3. The interpreter does not track loans (`unsafe.md` §5), so the rule is not checked, like the rest of U2's
   "as observed by safe code".
4. Unsafe code in this repository was audited for writes that the rule forbids (`optimisations.md` §2.6).

## Consequences

- M9c may treat a local's value, and a vector local's length, as fixed between two mutable loans of it. So it removes
  `v[i]` checks in loops, and keeps facts about an index that is also passed as `mut i` to a helper (LZ4's
  decompressor).
- Unsafe code that kept a pointer to a local's storage and wrote through it after using the local directly becomes
  invalid. The safe pattern is unchanged: use the pointer within the loan, or keep the data behind the pointer (a
  raw buffer, a box made into a raw pointer) rather than in a local.
