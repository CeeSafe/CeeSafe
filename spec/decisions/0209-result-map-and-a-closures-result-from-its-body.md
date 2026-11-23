# ADR 0209 — `Result.map` and `mapErr`, and a closure's result type from its body

- Status: **Proposed** 2026-11-23, and implemented on the working tree under the owner's instruction ("rebase on
  origin/main and continue with Result.map"; the optimization and DX roadmap's Phase 3, item 2); not released. §2.1 is
  ADR 0075 §2.3, accepted 2026-11-05 and left out in its §7.3; §2.2 is new. §6's questions are the owner's to decide.
- Scope: type-system.md §17.6 and §20.2; `lib/core/results.cee`; `src/check/members.cee`, `src/check/generics.cee`,
  `src/check/closures.cee`; spec/api/core.md; guide/builtins.md; the book's chapter 9.2 and appendix C.

## 1. Context

ADR 0075 §2.3 accepted `r.map(f)` and `r.mapErr(f)` and its §7.3 left them out: a generic method of a generic struct
could not name the struct's type parameters. ADR 0064 has since given structs generic static functions, and the
roadmap's item 3.1 made a function type match part by part in inference, so a user's
`mapOk<T, U: Send, E>(r, twice)` infers `U` (run-pass/infer-through-function-types).

What a user writes, though, is `r.map((v) => v + 1)`, and a closure without parameter types said nothing in
inference (§20.2): the call was N0265, asking for `map<i32>(…)` or `(v: i32) => …`. `v`'s type is not in doubt there:
it is the receiver's `T`.

## 2. Decision

### 2.1 `r.map(f)` and `r.mapErr(f)`

```ceesafe
public fn map<U: Send>(r: Result<T, E>, f: borrow mut fn(T) -> U) -> Result<U, E>      // ResultMapping<T, E>
public fn mapErr<F: Send>(r: Result<T, E>, f: borrow mut fn(E) -> F) -> Result<T, F>   // ResultErrorMapping<T, E>
```

- Both consume `r` and are `@effectsOf(f)`. `f` is a function's name, a function value or a closure, as for the
  slice algorithms (§36.4).
- `U` and `F` are inferred from `f` (§20.2), or written: `r.map<i64>(f)`.
- `U: Send` and `F: Send`: a value that comes out of a call of a function value is `Send` (§25.6, ADR 0168), so a
  result cannot be mapped to a type that borrows, a `StringView` included (N0334). `match` does that.
- `map` on a `Result<void, E>` is N0226: there is no value to give `f`. `mapErr` on one is allowed. `f` may return
  nothing: `r.map((v) => { log(v); })` is a `Result<void, E>`.
- The other four methods take no type arguments (N0209).

They are two structs of their own, not members of `ResultMethods<T, E>`: an instance of that struct for
`Result<void, E>` must keep `isOk`, `isErr` and `err`, and `map`'s body has no meaning with `T` = `void`.

### 2.2 A closure's result type from its body (§20.2)

In type-argument inference, a closure literal whose parameters have no written types is matched **last**. When the
type parameters found by then (from the other arguments, and for a method from the receiver's type) say what every
unwritten parameter of the closure is, the closure is checked with those types, and its result type is matched
against the declared one:

- an expression body gives its type;
- a block gives its first `return`'s type; every other `return` must have it (N0205), and a block that returns no
  value is `void`. This is the rule a spawned body has (§38.2).

Otherwise the closure determines nothing, as before: while one of its parameters is still open, and for a `move`
literal. `id((x) => x)` for `id<T>(f: fn(T) -> T)` stays N0265.

An error in the closure's body is reported as itself (N0200, N0205, …), not as N0265.

The expected type of the call is still not used (§20.2), so this is one direction only: arguments, then the body.

## 3. Options not taken

- **`map` without `U: Send`.** N0245 refuses it soundly: `f`'s result could hold a loan the signature does not show.
  Lifting it needs a way to say "the result borrows what the closure captured", which is ADR 0168's open end.
- **A built-in in the checker**, as `sortBy` once was. The method is three lines of CeeSafe; the work was inference,
  which every generic function now has.
- **Bidirectional inference** (the expected type of the call, closures before other arguments). More programs infer,
  and the order in which things are known stops being readable from the call. Not needed for `map`.

## 4. Consequences

- `apply(21, (x) => x + 1)`, which the 0.93.x specification gave as an example of N0265, now compiles: the corpus
  file check-fail/infer-untyped-closure.cee is edited with this ADR (its N0265 line is now `id((x) => x)`).
- N0234's message for an unknown method of a `Result` lists six methods; the book's chapter 9.3 quotes it.
- A closure in an inferred call is checked twice (once to infer, once as the argument), as a closure with typed
  parameters already was.

## 5. Implementation and tests (2026-11-23)

- `checkClosureGiven` (src/check/closures.cee): a literal checked with given parameter types and an inferred result.
  `inferFromClosureBody` (src/check/generics.cee) is the last step of `typeArgumentsFor`.
- `checkResultMethod` (src/check/members.cee) instantiates `map`/`mapErr` for the call as any generic method
  (`instantiateMethod`), the receiver skipped.
- Corpus: run-pass/result-map.cee (native and `--interp`, and with `--check-leaks`); check-fail/result-map-errors.cee;
  check-fail/infer-untyped-closure.cee, edited. The results are in ROADMAP §10.
- Known and not changed here: a generic call whose bound fails with a closure argument also reports N0236 for the
  closure (`apply<i32, StringView>(4, (x) => "t")` does in 0.93.7), so `r.map((v) => "text")` has three diagnostics
  where N0334 says it all.

## 6. Questions for the owner

1. `T?` has no `map`. Recommended: not in this ADR; `o.okOr(e).map(f)` and `match` cover it, and an optional's
   `map` wants its own look at `U: Send` (an optional of a view is common).
2. Is "first `return`" the right rule for a block (§2.2)? Recommended: yes, it is §38.2's, and one rule is easier to
   teach than two.
3. Should the extra N0236 of §5 be removed for every generic call? Recommended: yes, as a fix of its own.
