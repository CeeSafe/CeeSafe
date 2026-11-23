# ADR 0214 — Loan sets finer than a value: what would be sound, and whether to build it now

- Status: **Proposed** 2026-11-23, at the owner's instruction ("3.8 next": the optimization and DX roadmap's Phase 3,
  item 8, which says "this needs an ADR first"). §2 recommends changing nothing in the rule now; §5 is one false
  positive found on the way, fixed on the working tree; not released. §6's questions are the owner's to decide.
- Scope: lifetimes.md §8 ("one loan set per value"), type-system.md §25.5 to §25.7; ADR 0006 §3, ADR 0134 R7;
  `src/nir/borrow.cee`.

## 1. Context

The borrow checker keeps one set of loans for each value (ADR 0006 §3, lifetimes.md §8): everything a struct's
fields hold is held by the struct, and a field read takes all of it. Three things follow, each confirmed with
ceesafec 0.94.4:

```ceesafe
struct Two<region ra, region rb> { a: StringView<ra>; b: StringView<rb>; }

let t = Two { a: first.view(), b: second.view() };
let b = t.b;
first.append("!")!;          // N0241: cannot borrow 'first' as mutable while 'first' is borrowed
Console.writeLine(b);
```

1. **A field read carries the other fields' loans**, with two regions as above, with one region for both fields, and
   for an array of views (`views[1]`). The same two views as two locals are accepted.
2. **Overwriting a field does not release what it held**: after `h.a = borrow y;`, `x` is still borrowed by `h`.
3. Borrows *of* fields are already disjoint (`borrow mut p.a` and `borrow mut p.b`): the limit is on what fields
   *hold*.

The roadmap asks for "loan sets per region argument", and ADR 0134 R7 warns that with them "a bound on a type would
become a real constraint on instances".

### 1.1 Who would gain

Counted on 2026-11-23 over every `.cee` file (declarations matching `struct|enum Name<region …>`):

| Tree | Types with a region | With two or more regions |
|---|---|---|
| lib/ | 21 | 0 |
| src/ (the compiler) | 0 | 0 |
| CeeSafeApps (every application) | 105 | 0 |
| the corpus, run-pass and check-fail | 144 | 18 |

No library, compiler or application type has two regions; the 18 are tests of the region rules themselves. So loan
sets **per region argument** would change nothing in any program outside the corpus. What programs have is case 1's
second form: one region, several fields.

### 1.2 Why a set per field is not sound in general

A callee that holds `borrow mut Two` may move loans between fields of the same region, and nothing in its signature
says so (check-fail/field-to-field-through-reference.cee, `swap`):

```ceesafe
fn swap(t: borrow mut Two) { let old = t.a; t.a = t.b; t.b = old; }
swap(mut t);
let b = t.b;                 // views 'first' now
```

A caller with a set per field would accept `first.clear()` after it. So per-field sets hold only for a value that
is **never lent mutably**, by a `borrow mut`, a `mut` argument, a `borrow mut this` method or a mutable capture.
Regions are the finest thing a signature can say: fields of one region are one set to every callee.

## 2. Decision

**Keep one loan set per value now.** Record what would be sound, so that a finding that asks for more starts from
it:

### 2.1 If a program needs case 1: sets per field, for struct locals that are never lent mutably

- A struct local that is never the place of a mutable borrow (§1.2's four ways), whose every write is a literal, a
  whole assignment or `p.f = v` in the same function, has one set per first-level field.
- `p.f` and a shared borrow of a place under `p.f` take field `f`'s set; `p.f = v` adds to it; a literal gives each
  field its operand's; any use of `p` as a whole (a move, a copy, an argument, a capture, a `return`) takes the
  union, and a whole assignment from a value that is not a literal gives every field everything.
- A field's set is live while that field can still be read: liveness per field, with a use of the whole value a use
  of every field.
- Everything else (a value lent mutably once, enums, arrays, elements, what a reference refers to, values of a type
  parameter's type) keeps one set.

This needs no region and no bound: a value that is never lent mutably is one no callee can apply a bound to, so
ADR 0134 R7 does not arise. In the checker it is a set of virtual locals per such struct local, for the points-to
sets and for liveness: an estimated 400 lines in `src/nir/borrow.cee`, the file every soundness argument of §25
rests on.

### 2.2 If a program needs two regions told apart across a call: sets per region parameter

For a value that *is* lent mutably, the finest sound grain is the declared region parameter. Then:

- a field's loans go to the sets of the regions its type names; a callee's flows already name regions (§25.6), so
  a call with `borrow mut p` adds what flows into region `ra` to that set only;
- **R7's answer**: a bound `region b: a` on the type is the inclusion "set(a) ⊇ set(b)" on every instance, applied
  where the instance is made and after every mutable loan of it. No check at a literal is needed, and no program is
  refused for a bound: the instance only keeps its `b` loans alive as long as its `a` ones;
- regions are erased from types (ADR 0006 §4), so the sets are indexed by the declaration's parameters, not by
  arguments.

Nothing outside the corpus has such a type today (§1.1).

### 2.3 Case 2, a field overwritten, is a different change

Points-to sets only grow (lifetimes.md §3): they are computed once for the function, and a loan's end is found by
liveness, not by removal. Releasing a field's old loan on overwrite needs sets that differ by program point, which
neither §2.1 nor §2.2 gives. It is not proposed.

## 3. Options not taken

- **Build §2.1 now.** It would accept case 1 for locals. No finding found asks for it (the applications' findings files, searched for "loan set" and
  "per field", have none), and its cost is in the one file where a mistake is unsoundness rather than a refused program.
- **Build §2.2 now**, as the roadmap's item is worded. It serves no program (§1.1) and carries R7.
- **Sets per field for every value.** Unsound (§1.2).

## 4. Consequences

- Nothing changes for programs. lifetimes.md §8's sentence stands, and ADR 0134 R7's condition ("if loans were ever
  tracked per field or per region argument") has an answer written down for the day it is met.
- The roadmap's item 3.8 is closed as "designed, not built", to be reopened by a finding.

## 5. Found on the way: `t.a = t.b` through a `borrow mut` was N0272

```ceesafe
fn direct(t: borrow mut Two) { t.a = t.b; }                      // N0272 before
fn viaLocal(t: borrow mut Two) { let old = t.b; t.a = old; }     // accepted before and now
```

A value read through a reference parameter is a copy of what the parameter's referent holds, not the parameter's own
loan (ADR 0074 §2.4), and what a referent already holds may be stored back into it (ADR 0075 §8). `let old = t.b;`
was lowered that way; an operand read through the parameter and stored directly was given the parameter's own loan
too, and refused. `recordStore` (src/nir/borrow.cee) now treats both alike. A parameter's view, another parameter's
field and a parameter stored as a reference are N0272 as before.

The same rule reaches two more shapes, and four expected diagnostics in three corpus files are edited for it:

- **Between two parameters that share a region.** `this.text = other.text;` in
  `fn swapWith(borrow mut this, other: borrow mut Holder<r>)` was N0272, and so was the next line by what the first
  had stored (check-fail/closure-capture-stores-calls.cee, two lines). It is what ADR 0131 allows through a deep
  flow, "what the source's referent holds". A caller is still refused where that carries a local's view into a
  `borrow mut` parameter: the file's `x3`, four N0272, unchanged, and `leaks` in the new check-fail file.
- **In a closure's body.** `into.text = v;` was reported in the body and again where the closure is made
  (check-fail/closure-capture-stores.cee and closure-capture-stores-forms.cee, one line each). The body's report came
  from the same extra loan; the one where the closure is made, which is the rule (§36.2), stands in both.

Every function those three files require to be refused is still refused.

Tests: run-pass/field-to-field-through-reference.cee (a copy and a swap, three lines); check-fail of the same name
(three stores still refused, `leaks`, and §1.2's caller, N0241). The results are in ROADMAP §10.

## 6. Questions for the owner

1. Keep one loan set per value now, with §2.1 and §2.2 as the recorded designs? Recommended: yes.
2. If a finding asks, take §2.1 (per field, locals never lent mutably) before §2.2 (per region)? Recommended: yes;
   §2.1 serves the types programs have.
