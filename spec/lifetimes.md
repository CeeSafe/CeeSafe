# References and the Borrow Checker (M2b)

Status: normative for increment 2b. This document turns `ownership.md` invariants B1–B5 and E1–E3
into an algorithm on NIR. The surface rules for references are in `type-system.md` §13.
Where this document is stricter than `ownership.md`, it records a v0 simplification that later
milestones may relax.

## 1. What makes the v0 problem small

The algorithm can be simple and **exact for v0** because of four language rules (type-system §13):

1. **References are values of type `borrow T` / `borrow mut T` held only in locals and temporaries.**
   They never appear in struct fields (E3) or constants.
2. **A reference binding is immutable and initialised at its declaration** (`let r = …;`; no
   `var r: borrow T`). Assigning to `r` writes *through* it (`*r = v`) and never re-points it.
   So each reference local is assigned exactly once, which makes the points-to sets below
   flow-insensitive without losing precision.
3. **No nested references** (`borrow borrow T` is rejected).
4. **A use of a `borrow mut T` value is an implicit reborrow** of its referent (§4). Two usable
   mutable references to one place can therefore never coexist.

Together with lexical scoping, (1) and (2) mean a reference cannot outlive its referent *inside*
a function, except through a **temporary** borrowed at a call site, or through **`return`**. Both
are checked explicitly (§6, §7). Conflicting accesses while a loan is live (B1/B2) are the
remaining work, and they need liveness (§5).

## 2. NIR additions

```text
projection = Field(f) | Deref | Downcast(V) | Index(operand)
place      = local projection*                 -- (*r).f is  r Deref Field(f)
rvalue    += Ref(Shared | Mut, place)           -- creates a loan
           | Slice(Shared | Mut, place, lo, hi) -- M2d: also a loan, of the whole array or slice viewed
stmt      += StorageDead(local)                 -- the local's storage ends (scope exit, statement end for temps)
```

Lowering facts the checker relies on:
- Every `Ref` is assigned directly to a local or temporary: `t = Ref(k, p)`.
- A temporary lives until the end of the source statement that created it. At that point the
  builder emits `StorageDead(t)`. The condition of an `if` or `while` counts as its own statement
  (M15 S2): its temporaries end once it is evaluated, before the branch, and each evaluation of a
  `while` condition ends its own. A borrow taken in a condition therefore does not reach the body.
- User locals get `StorageDead` at the end of their block, and on `break`/`continue` for the
  blocks being left. (`return` ends every local; references escaping through `return` are §7's job.)
- A use of a `borrow mut T` operand at a call argument, an initialiser or a return is lowered as
  `Ref(Mut, (*r))`, a reborrow. A coercion of `borrow mut T` to `borrow T` is `Ref(Shared, (*r))`.

## 3. Loans and points-to sets

- Each `t = Ref(k, p)` at program point P creates a **loan** `L = (k, p, P)`.
- Each reference local `r` has a set `loans(r)` of the loans it may refer to (directly or through
  reborrows). It is the least solution of:

| NIR | Constraint |
|---|---|
| `t = Ref(k, p)` | `L ∈ loans(t)`; and if `p` contains `Deref` of a reference local `q`, then `loans(q) ⊆ loans(t)` (a reborrow keeps the original loan alive) |
| `t = Slice(k, p, lo, hi)` | as `Ref(k, p)`: a slice is a reference to `[T]`, and its loan covers all of `p`, whatever the range |
| `t = Use(Place(r))` with `r` of reference type | `loans(r) ⊆ loans(t)` |
| `t = Call(f, args)` where `f` returns a reference | `loans(arg_i) ⊆ loans(t)` for every parameter `i` that `f`'s signature says the result is derived from (§7) |
| reference **parameter** `p_i` | `Param_i ∈ loans(p_i)`, a placeholder loan for "whatever the caller lent" |

Because every reference local is assigned once (§1), solving these subset constraints by
fixpoint iteration gives the same result as a flow-sensitive analysis.

**Loan-holding locals (M4a).** Since M4a, every local of a *scoped* type holds loans
(`type-system.md` §19.1), not only reference locals. The constraints generalise:
- **Copies and moves.** When an assignment's rvalue reads a place rooted at a loan-holding local
  `q` (`Use`, aggregates, enum values, built-in calls), then `loans(q) ⊆ loans(t)`. This covers a
  payload read such as `t = (q as Ok).0`.
- **`Unpack`.** `Unpack(q, …)` gives each target `loans(q)`.
- **Borrowing a loan holder.** `Ref(k, p)` where `p` is rooted at a loan-holding local `q` and does
  not go through a deref: `loans(q) ⊆ loans(t)`. A reference to a scoped value keeps that
  value's loans alive.
- Liveness (§5) and the return check (§7) consider all loan-holding locals.
- **A value read through a local reference (ADR 0114 §2.3).** A copy `t = Use((*x)…)` through a reference local
  `x` whose one definition is `x = Ref(k, p)`, with no other reference on the way, reads what is stored in `p`: it
  gets `loans(q)` for `p`'s root `q` (less the outer placeholder when `p` is reached through a reference parameter,
  ADR 0074 §2.4), and not the loan of `x`'s own borrow, since the value does not point into `p`. If `p` is itself
  `(*y)…` for such a local `y`, the rule applies to `y`. A deep call result (ADR 0074 §2.4) whose argument is
  `Ref(k, (*x)…)` gets the same. Any other reference local gives all its loans, as above, and so does a `Builtin`
  rvalue given such a place (its result may point into the place: a region's allocation). A `for` over a field's
  elements or over an iterator field reads its items this way, so an item can be returned with a region of the
  field's contents, as the same read through the parameter can.

## 4. Accesses

Each NIR statement and terminator performs **accesses** to places, in evaluation order:

| Construct | Access |
|---|---|
| operand `Place(p)` | **Read** p |
| `Ref(Shared, p)` | **BorrowShared** p |
| `Ref(Mut, p)` | **BorrowMut** p |
| `Assign(p, …)` | the rvalue's accesses, then **Write** p |
| `StorageDead(l)` | **Dead** l |

Two places **overlap** if they have the same root local and one projection list is a prefix of
the other. `a` overlaps `a.f`, `a.f` does not overlap `a.g`, and `(*r)` overlaps `(*r).f`.
Two `Index` projections always compare equal (M2d): `a[i]` overlaps `a[j]`, because v0 does no
index reasoning. Two mutable slices of one array therefore cannot be live at once, even if
their ranges are disjoint.
Places with different roots never overlap. That is correct because aliasing between different
roots only arises through loans, and every loan is checked when it is created.

**Conflicts** (B1, B2):

| Live loan \ access | Read | Write | BorrowShared | BorrowMut | Dead |
|---|---|---|---|---|---|
| Shared | ok | **conflict** | ok | **conflict** | **conflict** |
| Mut | **conflict** | **conflict** | **conflict** | **conflict** | **conflict** |

## 5. Liveness (NLL)

- A reference local is **live** at a point if some path from that point reads it (as an operand
  or as the root of a place) before the function ends. Reference locals are never redefined
  (§1), so there is no kill except at the definition. This is standard backward dataflow over the
  NIR CFG.
- The **live loans after point P** are the union of `loans(r)` over every reference local `r` live
  after P.
- **Check:** for every access `a` at point P to place `q`, and every loan `L` live after P with
  `L` not created at P, it is an error if `L.place` overlaps `q` and the table in §4 says
  conflict. The errors are N0240 (write), N0241 (borrow), N0242 (read) and N0243 (Dead: the
  place stops existing while borrowed).

This accepts `let r = borrow x; print(r); x = 5;`, because `r` is dead after `print`. It rejects
`let r = borrow x; x = 5; print(r);`.

**Overwriting a reference is not a use of its referent (M11, G3).** A **Write** or **Dead** of a local `r` whose
type is a reference does not conflict with a loan whose place goes through `*r`, such as `(*r).next`. The write
replaces which object `r` points to; it does not change, move or end the object it pointed to. That object stays
protected by the loans the new reference inherited from `r` when it was made (§3: a reborrow `Ref(k, (*r)…)` gets
`loans(r)`). A box is not a reference: overwriting a box destroys its contents, so a loan through `*b` still conflicts.
This accepts a cursor walking a list, `node = cursor!; … cursor = next_of(node);`, and a temporary that is reassigned
in each iteration of a loop while a reference made through the previous value lives on.

**Writes happen after the value (M2d).** In `Assign(p, rvalue)` the rvalue is evaluated first and
`p` is written last. The **Write** access is therefore checked against the loans live *after*
the statement; the rvalue's own accesses use the loans live before it. This accepts
`a[0] = sum(a[..])`: the slice is dead once `sum` has returned, before `a[0]` is written.
Rust's MIR gets the same result by writing a call's result in a separate step.

**B5, two-phase borrows (as implemented; ADR 0073 §2.2).** For a method call `e.m(args)` whose receiver is
`borrow mut this` and `e` is a place, the receiver's loan is created **after** `args` are
evaluated, right before the call. Arguments may therefore read `e` (`v.push(v.len())`), but
they cannot hold a borrow of `e` across the call. The place itself, with its index values and their bounds
checks, is found where the receiver stands, left to right: `ps[i].set(bump(mut i))` borrows the element `i` named
before `bump` ran (before ADR 0073, after). The loan exists only for the call, which is exactly the interval in
which it must be exclusive.

Every other argument `mut p` (`p` a place) and every `borrow mut` local passed on to a `borrow mut` parameter (B4) is
two-phase too, in calls of functions, methods, closures and function values: its place is found where it stands, and
its loan made at the call. Unlike a receiver it is **reserved** in between: when a later argument does more than
read (a call, a borrow, a move), a shared loan of the place is held from the argument to the call, so a later
argument may read the place (`fail(mut s, s.count)`) but a write, a move or a mutable borrow of it conflicts (N0240,
N0241), and so does a shared borrow still live at the call (`f(mut s, s.name.view())`). Reads in later arguments
of what a reserved loan may cover are taken where they stand, before the loans are made.

## 6. Temporaries

`f(makeValue())` with a `borrow T` parameter borrows a temporary that dies at the end of the
statement (`StorageDead`). If the result of `f` is derived from that argument and is stored in a
reference local used after the statement, the `Dead` access conflicts with the live loan: N0243
("temporary dropped while still borrowed"). The fix is to bind the value to a local first. The same holds for a
temporary made for a receiver or a slice (ADR 0073 §2.1): `make().view()` is valid within its statement, and N0243
once the view is used after it.

## 7. Returning references (E1, E2)

- **Signature.** A function whose return type is a reference must name the parameters the result
  may be derived from. With exactly one reference parameter (a `borrow`/`borrow mut` receiver
  counts), that parameter is implied (elision). Otherwise the return type must be written
  `borrow(a, b) T` or `borrow mut(a) T`, naming reference parameters (N0245 if missing or wrong).
  `borrow mut` results may be derived from `borrow mut` parameters only.
- **Body check at each `Return(op)`.** For every loan `L ∈ loans(op)`:
  - `L = Param_i`: `i` must be among the named parameters (N0245);
  - `L` is a real loan whose place starts with `Deref` of reference parameter `p_i`: allowed if
    `i` is named (N0245 otherwise). Examples: `borrow this.items` with `this: borrow Self`, or
    `borrow (*a).f`;
  - any other real loan borrows a local or temporary of this function: N0244 ("returns a reference
    to local data").
- **Deep results** (type-system.md §25.6, ADR 0074 §2.4). A reference parameter that is not a view has two
  placeholder loans: `Outer_i`, the loan of the reference itself, and `Param_i`, what its referent holds. A value read
  through `*p_i` carries `Param_i` and not `Outer_i`; so does a borrow of a place beyond another reference stored in
  the referent. If the result is a deep result of `i`, the body check also rejects (N0245) `Outer_i`, and a real loan
  whose place starts with `Deref` of `p_i` and passes no other reference: the referent's own storage.
- **Callers** use the signature as the `Call` constraint in §3. For a deep result of parameter `i` whose argument is
  `t = &p` (`&mut p`), the result takes `p`'s local's loans, less `Outer` placeholders of a parameter `p` is reached
  through, instead of `t`'s; for an argument that is a reference parameter, its loans less its `Outer`; for an
  argument `t = f(a)`, the only assignment of `t`, where `f`'s result passes through its parameter `a`
  (type-system.md §25.6, B18), what `a` contributes as that argument, followed again if `a` is defined the same way.

## 8. What the v0 checker deliberately does not do

- No references in aggregates, and no reference re-pointing — **relaxed in M6x** (OD-3,
  `type-system.md` §25): aggregates with region parameters, scoped collections and re-pointable
  reference fields. The checker keeps one loan set per value; writes into fields, elements and
  through references add the written loans to the holder and to what its mutable loans point to
  (iterated to a fixpoint); calls apply their signature flows; and a loan conflicts only at points
  reachable from where it was created (holders such as a vector receive loans after their own
  definition). Destroying a value whose `drop` may read its loans is a use of them.
  A bound on a region parameter (`region b: a`, `type-system.md` §25.8, ADR 0134) is such a signature flow and
  nothing more: the checker compares no two regions at a call.
- Index-sensitive disjointness (`a[i]` versus `a[j]`) came in 2d with arrays, conservatively: all elements
  overlap. Since M13 G5, two different **constant** indices are different places, so `borrow mut a[0]` and
  `borrow mut a[2]` may be live together. That is sound because a constant index names one element whatever the
  array's length. Any index that is not a constant still overlaps every element.
- Moves and drops are 2c. The `StorageDead` points emitted now are where drops will go.
- **`await` adds no rule** (L1, type-system.md §40.2). An awaited call is a call in NIR
  (`intermediate-representation.md` §3), so its arguments' loans last until the task returns, as for any call. A frame
  never moves, so a loan of a local across an `await` needs nothing more. A scope's child is an element of a scoped
  collection (§40.5), so its loans last until the scope returns.

## 9. Diagnostics

Every borrow error names the conflicting access (primary span) and carries notes for the loan's
creation point and for a later use of the reference that keeps it live, when one exists.
