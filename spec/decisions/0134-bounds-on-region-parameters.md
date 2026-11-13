# ADR 0134 — Bounds on region parameters: `region b: a` says that `b` outlives `a`

- Status: **Accepted** by the owner, 2026-11-14 ("implement Region bounds"), implemented on branch
  `impl/region-bounds`; not yet released.
- Scope: what a bound written on a region parameter means, where one may be written, and what it gives a body and
  takes from a caller (`src/check/region_bounds.cee`, and the places that call it in `references.cee`,
  `structs.cee`, `declare.cee`, `inherent.cee`, `interfaces.cee`, `src/nir/borrow.cee`; `--dump-ast`);
  `spec/type-system.md` §25.1, §25.6, §25.8 (new), `spec/grammar.md` (`TypeParam`), `spec/diagnostics.md` (N0216,
  N0267, N0273), `spec/lifetimes.md` §8. No new diagnostic code.
- A breaking change: a program that writes a bound on a region parameter that is not another region parameter in
  scope compiled, and no longer does (§6: none in the tree).

## 1. Context

`fn pick<region a, region b: a>(…)` has always parsed: the parser reads `':' Type ('+' Type)*` after any parameter
that is not `const`, and the formatter keeps it. Nothing else read it (found on 2026-11-14 with release 0.42.1):

- the body gets nothing from it: returning `y: StringView<b>` as a `StringView<a>` is N0245 with no bound, with
  `region b: a` and with `region a: b`;
- no caller is checked against it;
- it is not resolved: `region a: nosuch`, `region a: u32`, `region a: Shape + Vector<u8> + Copy`, `region a: a`,
  `region a: b, region b: a` and `region a: static` all check and run, on a function, a struct, an enum, an inherent
  impl and a method;
- `parse --dump-ast` prints `(region b)` for `region b: a`.

There was no soundness hole, since the bound granted nothing. There was a syntax that reads as a promise, in a
language whose two-region functions cannot otherwise say that one region's borrows may stand in for another's: the
only way to write `prefer` below was one region for both parameters, which ties every result to both arguments,
whichever region its type names.

## 2. Decision

2.1 **Meaning and direction.** `region b: a` says that **`b` outlives `a`**: *a value that holds what is borrowed
for `b` may be used where one borrowed for `a` is required*; never the reverse. This is the direction of a type
parameter's bound (`T: I`: a `T` may be used where an `I` is required) and of the same bound in other languages
(Rust's `'b: 'a`).

"Outlives" is defined in the terms §25 already has, not by a comparison of two lifetimes, which the checker does not
have: regions are erased (§25.5), a value has one loan set, and a region in a signature is a **flow name** (§25.6).
A bound adds flows:

> With `region b: a` in scope, a target of flows that names `a` (the result, a store target) takes the loans of
> every parameter that names `b`, as if the target named `b` too.

The relation is reflexive (every flow name flows to itself: the existing rule) and transitive (`region c: b` and
`region b: a`: a target that names `a` takes `c`'s). It is not symmetric: a target that names `b` gains nothing
from `region b: a`.

2.2 **What it grants in a body.** Exactly the flows of 2.1, in the one record a body is held to
(`HirFunction.returnSources`, `deepSources`, `mutableFlows`): returning a parameter that names `b` from a result
that names `a` is no longer N0245, and storing it through a target that names `a` no longer N0272. Nothing else
changes in a body: a local's written regions are not checked, as before (§25.2).

In the terms of §25.6: before the result's sources and each store target's flows are computed, the target's names
are **closed downward**: every region parameter in scope one of whose bounds is a name in the set is added, until
nothing is added. The parameters' names, the sources, are left as written. Then "share a flow name" is read as
before. Consequences that follow without a rule of their own:

- a reference's own region counts: `source: borrow(b) String` with `region b: a` flows into a result
  `StringView<a>` as a plain source, not a deep one, so the body may return `source.view()` and the caller records
  a borrow of the argument itself. Without the bound the result is not a source at all (or, where the referent
  names `a`, a deep one);
- `borrow(p)` in a result contributes `p`'s names (§25.6), and those are closed like any other;
- elision is untouched: an elided region is no region parameter and has no bound;
- the flows are the declaration's (ADR 0131 §2.9): the closure is part of computing a signature's flows, so the
  declaration's record has the bound's flows and every instance inherits them by the existing mechanism. There is
  no second mechanism.

2.3 **What it demands of a caller: the flow is recorded.** A caller does not compare the lifetimes of the `a`
arguments and the `b` arguments: at a call the checker knows loans and where they are live, not named lifetimes. It
applies the callee's flows, as for every call (§25.7 rule 4, lifetimes.md §3): the result takes the loans of the `b`
arguments, and what a store target refers to takes them. From then on the ordinary rules hold the caller to the
bound: the value typed with `a` cannot be used after a `b` argument's referent is destroyed (N0243), nor while it is
written (N0240 to N0242).

That is the whole demand, and it is the sound one in this model. What the callee may do with the bound is exactly:
let a `b` loan be reachable from an `a`-typed result or store target. The caller's record puts every `b` loan there
whether or not the callee does. Any use the caller then makes of that value is checked against those loans. A
separate outlives check at the call ("the `b` arguments' referents live at least as long as the `a` arguments'")
would refuse `prefer(short.view(), long.view())` whose result is used while both are alive only if it were stricter
than the flow, and admit nothing the flow refuses: it would have to be decided from the same loans and live ranges,
and what it could add is the refusal of programs that never use the result too late.

So a call that "violates" the bound in the lexical sense is not an error in itself:

```ceesafe
fn pick<region a, region b: a>(x: StringView<a>, y: StringView<b>) -> StringView<a> => if (x.length > 0) x else y;

fn main() {
    let long = String.from("long")!;
    var kept = Vector<StringView>.create();
    {
        let short = String.from("short")!;
        Console.writeLine(pick(long.view(), short.view()));   // accepted: the result dies before `short`
        kept.push(pick(long.view(), short.view()))!;
    }                                                          // error[N0243]
    Console.writeLine(kept[0]);
}
```

```text
error[N0243]: 'short' goes out of scope while still borrowed
  = note: 'short' is borrowed here
  = note: the borrow is used later here
  = note: 'pick' declares 'region b: a' ('b' outlives 'a'), so its result may hold the borrow passed here
```

The third note is new. A conflict with a loan (N0240 to N0243) names each call in the function that was given a
value holding the loan and whose callee has, for that parameter, a flow that a bound alone gives: "… so its result
may hold the borrow passed here", or "… so it may store the borrow passed here through another argument". The note
explains; it decides nothing.

2.4 **Where a bound is written.**

- **Functions and methods**: on the function's own region parameters, naming them or, in a method, the regions of
  its struct, enum or inherent impl (`fn label<region l: scratch>(…)` in `struct Parser<region scratch, …>`). The
  enclosing type's regions cannot be bounded by a method's: there is no place to write that.
- **Structs and enums**: `struct Parser<region scratch, region input: scratch>`. The bound is in scope in every
  function of the type, in its body and in its inherent impls, where it means what a function's own bound means:
  a parameter that names `input` flows where `scratch` is required. For a method the receiver has all the type's
  regions already, so the bound matters for the other parameters and for functions without a receiver.
  **An instance is not checked against the bound, and need not be**: a value has one loan set (§25.5), so an
  instance holds the borrows of all its fields and is not used after any of them is gone, whichever region a field
  is written with. A field of type `StringView<input>` may be read as a `StringView<scratch>` with or without the
  bound, since fields' regions are not checked (§25.2). The bound on a type is therefore a statement for its
  functions and for its readers, not a constraint on its values.
- **Inherent impls**: an `impl<region a, region b: a> S<a, b>` block names its struct's or enum's parameters in
  order (N0267, ADR 0064); it **restates the region bounds exactly**: the same bounds on the same parameters, no
  more and no fewer (N0267). A block cannot be conditional on a region bound, as it can on a type bound (ADR 0130):
  regions are erased, so there is nothing to select an instance by.
- **Impls of an interface**: `impl<region r> I for S<r>` declares the regions again as its own names (ADR 0054). It
  **writes no bound** (N0216), and its methods see none: their flows are their interface's (§21.1, ADR 0131 §2.3),
  and an interface has no regions (N0216), so a bound could give a caller through the interface nothing.
- **Interfaces**: an interface with region parameters stays N0216, and an interface's method, or a method of an
  impl of one, with regions of its own stays N0216. Unchanged.
- **Function types and closures**: a function type has no region parameters and so no bound, and a closure
  declares none. A function with region parameters is no function value (N0298, §36.1: a function type cannot say
  how loans flow between its arguments), with a bound as without one, so no call through a function value has a
  bound's flow to apply. Unchanged, and tested for a function with a bound.
- **`static`**: `region a: static` is refused (N0273). It would say that `a`'s borrows are static data, and no call
  checks that an argument is static data (ADR 0068: `static` in a parameter's type is N0273 for this reason). The
  converse needs no writing: static data holds no loan and passes where any region is required.
- **Several bounds**: `region c: a + b`, `c` outlives both.

2.5 **Errors.** A region's bound is another region parameter in scope. Each of these is N0273, at the bound:

| Written | Message |
|---|---|
| `region a: nosuch`, `region a: u32`, `region a: Shape`, `region a: T` | `'nosuch' is not a region parameter in scope: a region is bounded by a region, 'region b: a' for "'b' outlives 'a'"` |
| `region a: Vector<u8>` (not a name) | `a region is bounded by a region parameter, not by a type: 'region b: a' says that 'b' outlives 'a'` |
| `region a: static` | `'static' cannot bound a region: no call checks that an argument is static data, and static data may be used for any region as it is` |
| `region a: a` | `the region 'a' is bounded by itself: every region outlives itself` |
| `region b: a + a` | `the bound 'a' is written twice` |
| `region a: b, region b: a`, or a longer cycle (at the first region of the cycle) | `the bounds of region 'a' lead back to it through 'b': regions that outlive each other are one region, so declare one` |

A cycle is refused, not read as "the regions are equal": the program that means one region writes one, and a
relation without cycles keeps "outlives" a partial order that a reader can follow. A cycle lies within one
parameter list (an enclosing type's regions cannot name a method's), so it is found there.

And, with existing codes for existing meanings:

- an inherent impl that does not restate its type's region bounds: N0267;
- an impl of an interface with a bound on a region parameter: N0216;
- a type parameter bounded by a region (`T: a`, `T: static`) was and is N0204 ("'a' is not an interface"), and a
  const parameter's type that is a region (`const N: a`) N0204 ("unknown type 'a'").

No new code is taken: N0273 is "a region written where none can be", which is what each bound error is.

2.6 **`--dump-ast`** prints a region's bounds after its name: `(region b a)` for `region b: a`, `(region c a b)`
for `region c: a + b`. A region without bounds prints as before, so no snapshot changed.

## 3. Why each program is refused

| Program | Refused by | With |
|---|---|---|
| a body that returns or stores a `b` value as an `a` without the bound, or with `region a: b` | 2.2: the target's names are closed downward only | N0245, N0272 in the body |
| `fn forward<region a, region b>(x, y) -> StringView<a> => pick(x, y)` | 2.3 applied to `forward`'s own body: `pick`'s result holds `y`'s loan, which `forward`'s signature does not let its result hold | N0245 in `forward` |
| a caller that keeps `pick(long, short)` after `short` is destroyed | 2.3: the result holds `short`'s loan | N0243, with the note |
| the same through a struct's function, a method's own bound, a generic function's instance, a generic struct's function, another package | 2.2: the bound's flows are the declaration's | N0243 |
| `let f = pick;`, `call(keep, …)` | N0298, unchanged | N0298 |
| a closure `(out, word) => keep(out, word)` | a closure cannot store one parameter through another, unchanged | N0272 in the closure |
| every bound of 2.5 | 2.5 | N0273, N0267, N0216, N0204 |

## 4. Why it is sound: the assumptions, and what enforces each

A bound adds flows to a declaration's record and does nothing else. The argument is ADR 0131's (§4 there): a body
is held to its declaration's flows, and a caller applies the callee's flows, which are at least the declaration's.
An extra flow in that record can make no program unsound: it lets the body do more, and makes every caller assume
that it does. What must hold is that the two sides read **one** record and that nothing reaches a function around
it. Each assumption has a test that fails without it (the mutations are in §8):

| # | Assumption | Enforced by | Tests |
|---|---|---|---|
| R1 | The flows a bound gives are in the record that callers apply, not only in what the body is allowed | one computation (`resolveFlows` closes the target's names, then computes sources and flows from them) | `check-fail/region-bounds-callers` (every function of it), `tests/packages/region-bounds/misuse` |
| R2 | Every instance has them (ADR 0131 A1) | the declaration's flows are inherited; ceesafec's internal check (N0216) | `check-fail/region-bounds-callers`: `generic`, `genericStructFunction`, `genericStructMethod` |
| R3 | Where the bound makes a reference's own loan a source, the caller records the loan of the argument itself, not only what it refers to | deep and plain are decided from the closed names, once, for both sides | `check-fail/region-bounds-callers`: `reference`, `ownStorage`, `referenceStore` |
| R4 | A by-value store target whose flows are computed late (ADR 0131 §2.13) has them too | `resolveHeldTargets` closes the target's names the same way | `check-fail/region-bounds-callers`: `byValueTarget`; `run-pass/region-bounds`: `send` |
| R5 | No function value or closure carries a bound's flow (ADR 0131 A2) | N0298 for a function with region parameters; N0272 in a closure's body | `check-fail/region-bounds-function-values`, `check-fail/region-bounds-body` (the closure) |
| R6 | An impl block cannot change the bounds its functions are checked with | N0267 (inherent), N0216 (of an interface) | `check-fail/region-bounds-errors` |
| R7 | A value has one loan set, so an instance of a type with a bound needs no check | §25.5, unchanged | `check-fail/region-bounds-callers`: `instance` |

Not soundness, but the meaning: the closure goes downward only (`check-fail/region-bounds-body`: `preferBack`,
`keepBack`, `siblings`, `upward`, `Backwards`), and a bound names a region (`check-fail/region-bounds-errors`). A
closure in the wrong direction would give a different language, not an unsound one: its callers would record the
flows it has.

Where it is weakest:

- **R7.** Nothing checks an instance of a struct with a bound, and the claim that nothing needs to rests entirely on
  "one loan set per value" (§25.5) and on fields' regions not being checked. If loans were ever tracked per field or
  per region argument, a bound on a type would become a real constraint on instances, with a check at every
  literal, and this ADR would have to be revisited.
- **Region names are symbols.** A region is identified by its name (`regionFlowName(symbol)`), in this ADR as in
  §25.6 before it. Names cannot repeat within what a function sees (N0202), and an impl of an interface, the one
  place that renames a type's regions, sees no bounds (2.4). The receiver of such an impl's method still has the
  struct's own names, an existing looseness this ADR leaves as it found it (§9).
- **The note** (2.3) is found by looking, after the fact, for a call that was given a value holding the loan: it
  can be missing (a borrow that reached the call through another call's store) and decides nothing.

## 5. What sound programs it refuses, and how they are written

- **A bound cannot be asked for after the fact.** A function with two unrelated regions cannot be called "as if"
  bounded: the bound is the callee's to declare.
- **A bounded function ties its `a` results to its `b` arguments at every call**, also where the body would not
  have used the `b` argument. That is what a flow is; a function that needs both behaviours is two functions.
- **No bound through an interface.** A method of an interface cannot take two regions and relate them (N0216). It
  takes type parameters, whose bounds give flows (ADR 0131).
- **A cycle is not an equation**: `region a: b, region b: a` is refused; write one region.

## 6. What was newly refused, measured

The breaking change is 2.5: a bound that is not a region parameter in scope, `static`, the region itself, written
twice or part of a cycle; an inherent impl that does not restate its type's bounds; a bound on an impl of an
interface. Each compiled before, and was ignored.

Measured on 2026-11-14 by searching for `region NAME :` in every `.cee`, `.md`, `.py`, `.txt`, `.html` and `.json`
file of the tree (the corpus, `lib/`, `tests/packages`, `probes/`, `benchmarks/`, `templates/`, the guide's and the
book's listings, the specification's examples, the compiler's own source), before this branch's own files: **no
program in the tree writes a bound on a region parameter**, valid or not. Nothing is newly refused, and the corpus
passes unchanged (§8). Outside the tree the only known programs are the eight reproductions of the finding
(`r1` to `r8`): of those, `r5` (the nonsense bounds) and the enum of `r6` (`region b: a + nosuch`) are now refused,
as intended; `r2` and the rest of `r6` are now given what they asked for.

Programs with a *valid* bound that compiled before still compile, with one difference a caller can notice: the
callee's `a` results now hold the `b` arguments' loans. `r4`'s two calls are still accepted (the results are printed
at once). A caller that kept such a result past a `b` argument's referent was accepted before, soundly, because
the body could not then have returned it; it is N0243 now. None in the tree.

## 7. The alternatives rejected

- **A real outlives check at the call**, comparing the live ranges of the two groups of arguments. It needs a
  notion of "how long an argument's referents live" that the checker does not have for a value built from several
  loans, it refuses calls whose results are never used late, and it adds nothing that recording the flow does not
  already refuse (2.3).
- **The other direction** (`region b: a` for "`a` outlives `b`"). It reads against a type parameter's bound and
  against every language that has the notation.
- **A cycle as equality.** One more way to say what one region parameter says.
- **Refusing every bound** (the syntax error it could have been). It leaves the language without a way to return
  the longer-lived of two views as the shorter region except by merging the regions.
- **A bound on a struct as a constraint on its instances**, checked at each literal. With one loan set per value
  there is nothing for the check to compare; see R7 for when that would change.
- **Bounds on an impl of an interface.** Sound by the same argument, since such a method's flows are narrowed to
  its interface's; but it would be a second place where a type's bounds can be said differently, for flows a caller
  through the interface cannot see.
- **A new diagnostic code** for a malformed bound. N0273 already means a region written where none can be.

## 8. Tests

- `run-pass/region-bounds` (native and `--interp`, `--check-leaks`): the longer-lived of two views as the shorter
  region; a store; transitive and several bounds; a reference's own region; a forwarding function; a parser struct
  with an input region that outlives its scratch region, with a function, an inherent impl and a method's own
  bounded region; an iterator over two slices of different regions; an enum; a by-value store target; a generic
  struct; an impl of an interface for a struct with a bound.
- `check-fail/region-bounds-body`: each of those bodies without its bound (N0245, N0272), the bound the wrong way
  round, siblings, upward, forwarding without restating, a closure.
- `check-fail/region-bounds-callers`: a caller that keeps the result or the store past the referent, for a
  function, a store, a transitive bound, a reference's own region, a generic function, a forwarding function, a
  struct's function, a method's own region, a generic struct's method and function, an enum's function, a by-value
  target, the holder's own storage, an instance; and a write while the result is in use.
- `check-fail/region-bounds-errors`: every error of 2.5.
- `check-fail/region-bounds-function-values`: N0298, for a result with two sources, a store, and a function whose
  one flow the bound gives.
- `tests/packages/region-bounds` (`tools/package_tests.py`, check 35): a dependency's bounded functions, struct and
  method: used well (check, `run --interp`, built), and misused (N0245, four N0243 with the note's text).
- `parse-pass/region-bounds` (its `.ast` snapshot) and `tests/formatting/region-bounds`
  (`tools/fmt_snapshots.py`): `--dump-ast` and the formatter.
- The guide (`guide/ownership.md`, "One region that outlives another") holds the exact text of the N0245 without
  the bound and of the N0243 with its note; the book (Listing 10-18) the N0245 and the program with the bound.

Mutations, each applied to a copy of the implementation, built, and run against these tests (2026-11-14):

| # | Mutation | Assumption | Result |
|---|---|---|---|
| M1 | the result's names are not closed | the feature | fails `run-pass/region-bounds` (N0245), `check-fail/region-bounds-body` and `-callers`, `tests/packages/region-bounds` |
| M2 | a store target's names are not closed | the feature | fails `run-pass/region-bounds` (N0272), `check-fail/region-bounds-body` and `-callers` |
| M3 | the closure goes upward (the bounds of a name are added, not the regions bounded by it) | direction | fails `check-fail/region-bounds-body` (`preferBack`, `keepBack`, `upward` accepted), `run-pass/region-bounds` |
| M4 | a caller skips the result sources that a bound gives (`computeLoans`) | R1 | fails `check-fail/region-bounds-callers` (N0243 missing), `-body` (`forward` accepted), `tests/packages/region-bounds/misuse` |
| M5 | a caller skips the stores that a bound gives (`collectWrites`) | R1 | fails `check-fail/region-bounds-callers` (`store`, `referenceStore`), `-body` (`forwardStore`, the closure), `tests/packages/region-bounds/misuse` |
| M6 | the closure is skipped for an instance (a non-empty type environment) | R2 | **passes**: the instance inherits the declaration's flows (ADR 0131 §2.9), which is the mechanism R2 relies on |
| M6b | M6, and an instance does not inherit | R2 | fails: N0216, ceesafec's internal check ("this instance of 'choose' lacks a flow of its declaration"), on `run-pass/region-bounds`; `check-fail/region-bounds-callers` (`generic`, `genericStructFunction`, `genericStructMethod`) |
| M7 | a bound never makes a deep source plain | R3 | fails `run-pass/region-bounds` (`viewOf`: N0245), `check-fail/region-bounds-callers` (`reference`, `ownStorage`) |
| M8 | `resolveHeldTargets` does not close the target's names | R4 | fails `run-pass/region-bounds` (`send`: N0272), `check-fail/region-bounds-callers` (`byValueTarget`) |
| M9 | N0298 not given for having region parameters | R5 | fails `check-fail/region-bounds-function-values` on `one` alone: `pick` and `keep` are still N0298, by the rules for a result's sources and for a store through an argument, and `one` as a value would be sound (a call through a function value gives its result every argument's loans) |
| M10 | `checkRegionBounds`, `sameRegionBounds`, `refuseRegionBounds` report nothing | R6, 2.5 | fails `check-fail/region-bounds-errors` |

## 9. Not in this ADR

- **Regions on interfaces** (N0216), and with them a bound a caller through a bound or a `dyn` type could rely on.
- **An impl of an interface names its regions afresh, and its receiver has the struct's own names**: with
  `impl<region r> Put<StringView<r>> for Bag<r>` for `struct Bag<region a>`, a method's parameter of type
  `StringView<r>` shares no flow name with the receiver unless `r` is spelled `a`. Found while designing 2.4,
  present in 0.42.1, reported and not changed here.
- **Regions written in a body** (`let v: StringView<a> = …`) are not checked, with or without bounds (§25.2).
- **A `where` clause** that relates two regions declared elsewhere (a struct's by a method's).
