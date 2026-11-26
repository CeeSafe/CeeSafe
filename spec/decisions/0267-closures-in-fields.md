# ADR 0267 — Closure specialisation reaches closures held in fields, so chains run as loops do

- Status: **Accepted** by the owner, 2026-11-26 ("Accept all": §6's five questions as recommended). Proposed the same
  day, at the owner's choice of "Chains as fast as loops" after ADR 0264 (released in 0.95.19). **Implemented** on the
  branch `adr/0267-chain-closures` (§7), not released.
- Scope: `src/nir/closure_specialisation.cee` (the pass of optimisations.md §5, ADR 0019); optimisations.md §5.1 and
  §5.3; the corpus's `closure-calls` family and a benchmark; ADR 0264 §2.6 and type-system.md §36.4a's "Cost".
  No change to the language, the libraries or what a program can observe (optimisations.md §5.2).

## 1. Context

ADR 0264's chains are 3.3 times a loop where the closures are the work: `Chain.of(Count64 { left: n }).map((x) => x *
3).filter((y) => y % 2 == 0).fold(0i64, (a, y) => a + y)` takes 433–456 ms where the loop takes 132–139 ms (2×10^8
values, arm64, `-O2`, 0.95.19).

`--explain-closure-calls` on that program says why. The pass makes variants `Chain<…>.map#1`, `filter#1` and
`fold#1`, each with its closure parameter bound, so `fold`'s own call of its closure is direct. But `map#1` and
`filter#1` only store their closure in a stage (`Mapped { inner: this.inner, f: f }`), and the calls that matter,
in `Mapped.next` and `Filtered.next`, read it back: `(*this).f`, a place with projections. optimisations.md §5.1:
"Not specialised: a closure read from a field". So two calls per value stay indirect, and the C compiler can inline
neither.

The facts that would make them direct are all in the program: in the variant `map#1` the returned chain's
`inner.f` is the bound closure; in `filter#1`, given that its receiver's `inner.f` is known, the returned chain's
`inner.inner.f` and `inner.keep` are; and `fold` passes `borrow mut it` of that chain to `next`.

## 2. Decision

### 2.1 Facts about fields

The pass's facts, today "local `l` holds closure `K`", become "the place `l.p` holds `K`" for a path `p` of field
projections and dereferences, at most 8 long. A local's facts come from:

- an aggregate assigned to it, whose operand for a field is a local holding `K`, or holding facts of its own (which
  are carried under that field's name): `Mapped { inner: x, f: f }` gives `.f = K` and every `x.q` as `.inner.q`;
- a copy or move of another local or of a place under one (`var it = this.inner` gives `it.q` for each
  `this.inner.q`);
- a call's result, from the callee variant's **return facts** (§2.3);
- a variant's bindings (§2.2).

As today, a local's facts hold when every assignment to it agrees on them; the analysis is flow-insensitive and
iterated to a fixed point.

### 2.2 Variants bound by field facts

A call whose argument carries facts, a local by value (`filter(chain, keep)`) or a borrow of a place
(`next(borrow mut it)`), goes to a variant of the callee with those facts as bindings of its parameter: `(parameter,
path, body)`. A call through a closure read from a place, `(*this).f`, is direct when that place's fact is known.

### 2.3 Return facts

A variant's return facts are the facts of the value it returns, on every return path. They are computed when a
caller needs them, by analysing the variant with its bindings, and kept; a call met again while its own return facts
are being computed (recursion) gives none.

### 2.4 Only fields that are never written

A fact about a field holds only if nothing can change the field after the aggregate is made. The pass takes as
**write-once** a closure-typed field (of a struct definition) that no statement of the program assigns through a
projection (`p.f = …`) and that no `borrow mut` of the field, or of a place under it, is taken anywhere. A fact is
recorded for write-once fields only. Replacing the whole value (`chain = other`) is an assignment to the local and
is handled as today. lib/core's chain stages are all write-once: their fields are private and set only when made.

### 2.5 Limits

Paths of at most 8 projections; at most 64 variants of one function (beyond, calls stay indirect: the pass stays
linear in practice). Async functions and their instances are still not specialised (§5.1).

### 2.6 What the report says

`--explain-closure-calls` names each direct call as today and lists a variant's field bindings as
`parameter 0.inner.f = the closure at …`; an indirect call through a field says which of §2.4's rules or §2.5's
limits kept it indirect.

## 3. Options not taken

- **Closure types as type arguments** (`struct Mapped<T, U, I, F: fn(T) -> U>`), so that each chain is its own
  instance and the call is direct by construction. It changes the language (a bound that is a function type,
  inference of a closure's own type), and every program's chain types; this ADR changes neither.
- **Inlining the stages in the C we emit**, leaving the C compiler to see the constant function pointer. The
  pointer goes through the stage struct in memory, and at `-O2` clang does not follow it (§1's measurement).
- **Only the chain types**, by name. The rule of §2.4 serves any struct that holds a closure it never changes, the
  book's `Filter` of Listing 13-12 among them.

## 4. Consequences

- Chains whose closures are known where the chain is made compile to direct calls, which the C compiler may inline.
  The target, checked by a benchmark: within 10% of the loop on §1's program. ADR 0264 §2.6 and type-system.md
  §36.4a then state the new cost, or say what remains.
- More variants in the generated C: one per distinct set of closures a chain or struct is made with, as for
  closure parameters today (c-backend §2.1's names).
- Nothing a program observes changes (§5.2); the corpus's interpreter-against-native comparison runs the specialised
  NIR, as it does today.

## 5. What is not known

- Whether the C compiler then inlines the whole chain into one loop, which the 10% target needs, or stops at the
  direct calls. The prototype will tell; if it does not, the remaining cost is stated rather than chased here.
- How many variants real programs grow: §2.5's limit bounds them, and the corpus's C size is compared before and
  after.
- `fold(0, …)` infers its accumulator from the literal (`i32`) rather than from the expected result (`i64`): the
  benchmark needed `0i64`. A separate fix (type-system.md §20.2), not this ADR's.

## 6. Questions for the owner

1. **The design**: field facts, variants bound by them and return facts (§2.1–§2.3)? Recommended: yes.
2. **Soundness by write-once fields** (§2.4), program-wide? Recommended: yes; it is simple to check and true of
   every chain stage.
3. **The limits** of §2.5 (8 projections, 64 variants a function)? Recommended: yes, adjusted if the corpus shows a
   need.
4. **The target**: within 10% of the loop on §1's benchmark, and a `closure-calls` corpus test that the chain's
   calls are direct? Recommended: yes; if the C compiler stops short of it, release what is direct and state the
   rest.
5. **Release**: as a patch release once the target is met and the suite passes? Recommended: yes.

## 7. The design as built (2026-11-26)

- **The analysis** (`closureAnalysis`, `src/nir/closure_specialisation.cee`) computes, per function and bindings, the
  locals' known closures as before and their field facts (`Analysis.facts`, `(path << 32) | body`, paths in
  `Specialiser.paths`). Only whole-local assignments give a local facts (a write through a projection changes no
  write-once field). A copy or cast gives a place's facts (`valueFacts`), a borrow gives them under a dereference,
  an aggregate under each field (the empty path only for a write-once closure field, `stableField`), and a call its
  variant's return facts (`returnFacts`, cached by callee and bindings; none while being computed).
- **Bindings** pack a field fact as `((0x80000000 | path << 8 | parameter) << 32) | body`, sorted, so the C names of
  c-backend §2.1 stay deterministic and need no change; `--explain-closure-calls` prints one as `field (*).inner.f of
  parameter 0`.
- **Write-once fields** (`unstableClosureFields`, once per program): a closure field is unstable if a statement
  assigns through a projection ending at it, lends it mutably, assigns in place a value that holds it (by value:
  fields, payloads, elements, a box's or a vector's contents), or gives a builtin or an extern function a mutable
  reference or pointer through which it is reached.
- **A call through a place.** `CallClosure` takes the place itself (`CallClosure((*this).keep, …)`), so the direct
  call looks the place's fact up; a dereference's step is 0xFFFFFFFF, which is `none()`'s value, so a place's steps
  are told apart by the projection's kind.
- **async_instances.cee** keeps `specialisedClosures(c, n, function, bindings)`, now a wrapper with no field facts.
- **Measured** (the benchmark of §1, built by the branch, arm64, `-O2`, three runs each): the loop 133–139 ms, the
  chain 162–168 ms, **1.2 times** (3.3 before). The three calls are direct (`fold#1`, `Filtered.next#1`,
  `Mapped.next#1`); the rest of the gap is the stages' optional results, which the C compiler does not fold away.
  §4's 10% is not met; as §6 question 4 says, what is direct is released and the cost stated (type-system.md §36.4a).
- **Tests**: the corpus's `closure-calls/fields.cee` and its `.calls` (a struct of the program's that keeps its
  closure, a chain, a field assigned after the struct is made and a struct replaced in place, the last two indirect);
  the whole corpus, interpreted and native, which runs the specialised NIR.
