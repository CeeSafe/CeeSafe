# ADR 0131 — A value of a type parameter's type is followed as a scoped value is

- Status: **Accepted** by the owner, 2026-11-13 ("Ship 0.42.1 sooner. Follow with 0.42.2"); released in ceesafec 0.42.1.
  Proposed by the integrator the same day after an independent soundness review found the hole. Its first
  form (flows through interfaces, and a loan "of the function's own storage" kept by such a value) was broken by a
  second independent review the same day; §2 is the second form, which replaces it. A third review broke the
  second form's implementation in three ways, one of them a regression against 0.42.0: §2.9 to §2.11 and §4 are the
  third round's. The third review also found three families of programs this ADR does not cover: §10.
- Released with it: the rule the principle rests on, that a type parameter bounded by `Send` or `Share` holds no loan
  (`spec/type-system.md` §25.7 rule 5 and §38.1; it has no ADR of its own). 0.42.0 treated every type parameter as a
  possible reference whatever its bounds, so without the rule a program could not say "this `T` holds no borrow"
  (§2.4, §5), and `AsyncTasksIn.run` (§2.6) would not check.
- Scope: the checker (`src/check/references.cee`, `interfaces.cee`, `generics.cee`, `declare.cee`, `members.cee`,
  `closures.cee`), the borrow checker (`src/nir/borrow.cee`, `passes.cee`), `src/types/types.cee`, the NIR builder
  (one guard), three fields of a HIR function; `lib/core/async.cee` (one bound); `spec/type-system.md` §21.1, §21.2,
  §25.6, §25.7, §36.2, §40; `spec/diagnostics.md`; the guide's async page. No new diagnostic code, no change to the
  backends or the interpreter.

## 1. Context

A program without `unsafe` that passes the checker must not use freed memory. This one passed `ceesafec check` in
0.42.0 and did:

```ceesafe
interface Source<T> {
    fn get(borrow this) -> T;
}
struct Owner {
    text: String;
}
impl Source<StringView> for Owner {
    fn get(borrow this) -> StringView => this.text.view();
}
fn drain<T, S: Source<T>>(s: S) -> T => s.get();
```

Generic code is checked once, in its generic version, and never again for an instance (§20.3). The checker
followed loans through values of scoped types only. For a value of a type parameter's type it relied on an
argument it never stated: *what such a value holds, its type says; it can only have come from something whose type
mentions the parameter, and every place it can go shares a flow name with that*. The argument is false. A call
makes a `T` from an argument whose type does not mention `T`:

- a bound's method or static function whose interface is given a scoped argument:
  `S.build(text.view())` under `S: Build<StringView>`; `s.get()` under an impl that lends its storage;
- a function value: `f(o.text.view())` for `f: fn(StringView) -> U`;
- a closure, from what it captured.

The first review found six programs of the first kind (the one above, a default method, an impl storing a
parameter that its non-generic interface does not mention, a `T` stored through an out-parameter, a scoped
implementation behind `borrow mut dyn`, `S.build(local.view())` returned). The first form of this ADR repaired the
interface side and kept the argument for everything else, following only loans of locals. The second review
defeated it with one more generic call (F1: `fn make<S: Build<StringView>>(hint: StringView) -> S => S.build(hint)`
called with a local's view), with no interface at all (F2: `fn apply<U>(o: borrow Owner, f: fn(StringView) -> U)
-> U => f(o.text.view())` called by a generic function that drops `o`), through the exception it made for closures
(F3: `let g = () => s.get(); return g();`), and with a hole in closures that needs no generics (F4:
`let g = () => into.keep(v); g();`, a view of a local stored through a captured `borrow mut` parameter).

## 2. Decision

**The principle.** A value whose type mentions a type parameter that is not known to hold no loan (one not bounded
by `Send` or `Share`) is treated for loans exactly as a value of a scoped type would be in the same position.
Nothing about its loans is taken from its type. It is enforced in one place, the borrow checker's flow of loans
(`computeLoans`, `src/nir/borrow.cee`): the test for "this operand carries its loans to what is made from it" is
`mayHoldLoans` (§25.7 rule 5), no longer "is of a scoped type". Everything below is what must then be true of
signatures for the bodies of sound programs to check.

2.1 **In a body** (§25.7 rule 5). Such a value carries its loans through every copy, move, aggregate and payload.
A call's result carries what the callee's flows give it, whatever the result's type; a call through a function
value gives its result the loans of every argument; a call of a closure, those and what the closure's captures
hold (2.5). A built-in that gives such a value (an element, a payload, what `pop`, an index or a pointer reads)
gives what its operand holds, not a borrow of the operand's storage: the value is not a reference, slice or view
into it, which are the only things a built-in borrows into. Returning the value and storing it through a parameter
are checked as for a scoped value (N0244, N0245, N0272). A raw pointer is no loan (§27.1).

2.2 **Signatures: a result flows from the parameters it shares a flow name with** (§25.6), as before, and the body
is held to that (2.1), so a function cannot return, in a `T`, a loan its signature does not declare. What gives a
parameter a result's name:
- its type mentions the parameter (`v: T`, `v: borrow mut Vector<T>`); a projection `P.Item` counts as `P`;
- a bound: under `S: Source<T>` a parameter of type `S` has the name `T`, and so on through bounds of bounds;
- an interface's method: the receiver has the interface's parameters and `Self`; `Self` has them too. So
  `fn get(borrow this) -> T` has a deep result of `this`, as a generic struct's method has;
- `Self` in a struct's or an enum's own function has the type's parameters and regions;
- a function type has its result's names: `f: fn(StringView) -> U` flows to a result `U`, and a `borrow fn` or a
  closure is a deep source (what its captures hold);
- what a `borrow dyn I<T>` refers to has `I`'s arguments' names, and a `borrow mut dyn I` is a target of flows.

Two additions, so that signatures say what bodies do:
- **What a function value may be given.** A function that takes a function value whose result may hold a loan
  (not a `dyn fn`) can hand it anything and return what comes back, so its result may borrow from every scoped
  parameter, as a scoped result of a function with one scoped parameter does by elision: `apply` above checks, and
  `apply(owner, same)` borrows `owner` for a generic caller as it already did for a concrete one (N0240 in the
  reviewer's `h2`; N0244 now in `h3`).
- **Through a stored value.** Where the result flows from parameter `i` and parameter `j` may be stored through
  `i`, the result flows from `j`: one loan set stands for what a reference refers to, so a `V` taken out of a map
  holds what a `K` put in.

A flow between two parameters that only a bound gives is deep when its source is a reference (the callee may store
what the source's referent holds, not a loan of the source).

2.3 **An impl's method has the interface's flows** (§21.1). Its own flows are narrowed to those of the interface's
method as generic code sees it, and its body is checked against them: a result that borrows from the receiver
itself where the interface's result is declared with a type parameter (a deep result), a result or a store that
takes a parameter's loan where the interface declares no such flow, is N0245 or N0272 in the body, where it
happens. A signature that merely could do so is no error.

2.4 **Bounds are checked.** An interface's own parameters' bounds are checked wherever the interface is given
arguments (N0268, N0334). `Send` and `Share` on a type that a declaration names are decided once every struct and
enum has its fields. The principle makes `T: Send` (or `Share`) the way a program says "this `T` holds no borrow";
that is sound only because no scoped type is Send and the bound cannot be evaded.

2.5 **Closures: a closure's statements are the enclosing function's, for loans** (§36.2). No exception for a
result of a type parameter's type.
- A closure's result may hold what its captures hold, so a call gives its result the loans the closure's
  environment holds (as a deep result: a borrow of a capture itself is N0245 in the closure's body, the existing
  rule) and its arguments' loans.
- A closure whose body stores what it captures through one of its captures is a store, where the closure is made,
  of every other capture's loans through that capture. The closure's body is borrow-checked before the function
  it is written in (an inner closure before its outer one), and records the captures it stores through
  (`HirFunction.captureStores`): a call whose flows write through a place reached through a capture, a `push` or
  `insert`, an assignment to such a place, an inner closure that does. Whether the closure is ever called does not
  matter. This closes F4, which needs no type parameter.

2.6 **`Async.tasks` and `Async.tasksIn`: `E: Copy + Send`.** `AsyncTasksIn<E, A>.run` calls its body, a function
value, with a borrow of the set it made, and returns the body's `E`. By 2.1 that is a loan of a local unless `E`
holds none. `Copy` was already required; `+ Send` is the new restriction.

2.7 **A cell of loans is refused where the instance is written.** `Cell<StringView<r>>` as a field's type was
accepted, and each use then failed in the library's body with ceesafec's internal error. A struct that keeps a
parameter in a cell (`Cell<T>`, `RefCell<T>`, `OnceCell<T>`, a user's wrapper of one) now refuses a scoped argument
with N0351 at the field, parameter or expression that names the instance.

2.8 **Messages.** Every N0244, N0245 and N0272 that the principle causes for a value of a type parameter's type
ends "a value of type 'T' may hold a borrow: if 'T' never holds one, bound it: 'T: Send'". In an impl's method the
message names the interface's rule and blames only what the body does.

2.9 **Flows are the declaration's** (third round). The second form computed a function's flows again for each
instance, from the instance's types, and two of its rules look at types: the widening of 2.2 ("takes a function
value whose result may hold a loan") and the elision it switched off for an interface's method. So a generic body
was checked with flows that its instance did not have: `interface Run<U> { fn run(borrow this, o: borrow Owner, f:
fn(StringView) -> U) -> U => f(o.text.view()); }` checks with the result flowing from `o`, and `Run<StringView>`'s
`run` had no such flow, and no elision either (0.42.0 refused it by elision: a regression). The same with a result
that names a region, `-> Pair<StringView<r>, U>`.

The rule: **a function's flows are computed once, for its declaration, with every type parameter in scope opaque,
and every instance of it has at least those**: an instance of a generic function or method, a generic struct's
method for an instance of the struct, an interface's method for a bound, for a `dyn` type and for the type that
inherits a default, an instance of a generic impl's method. An instance's own computation (names, and elision
where its result is scoped) may add flows; it never removes one. Two gates remain, which cannot lose a loan: a
result whose instance type can hold no loan has no sources, and a `borrow mut` parameter whose instance referent
can hold none is no target. An impl's method's declaration flows are those narrowed to its interface's (2.3).
ceesafec checks this of itself: after the declarations, every function whose declaration's flows are known is
compared with them, and one that lacks a flow is N0216, "ceesafec's internal check", not a silent pass.

2.10 **A function type carries no flows between its arguments** (third round). A call through a function value
gives its result the loans of every argument (2.1); it models no store. That was assumed and not enforced:
`fn pushT<T>(out: borrow mut Vector<T>, v: T) { out.push(v)!; }` taken as a value
`let f: fn(borrow mut Vector<StringView>, StringView) = pushT<StringView>;` stores `v` through `out` at a call the
checker reads as storing nothing. N0298 already refused a function with region parameters as a value, with this
very reason. It now refuses, the same way, **any function whose flows let one argument's loans be stored through
another** (a `borrow mut` parameter that some other parameter flows into, by a region, a type parameter or a
bound): as a named value, as an argument where a function type is expected, in generic code and out of it.
`pushT<i32>` has no such flow (a `Vector<i32>` holds no loan) and is a value as before. The other answer, a call
through a function value storing every argument's loans through every `borrow mut` argument, was rejected: it is
sound too, but it makes every callback that takes a `borrow mut` state and a view (`visit(mut state, name)`) a
store of the view, in code whose callee only reads it, and N0272 wherever the state is a parameter; the functions
it would save, `Vector.push`-like ones taken as values, are few: a closure's body cannot store its parameter
through another either (N0272 in the closure's body, the existing rule), so a store through a callback is already
written inline. Measured: nothing in the corpus, the libraries, the guide, the book or the compiler takes such
a function as a value.

2.11 **`impl<T: Send> I for S<T>`** (third round). Whether an instance has a generic impl was decided by
"`checkBounds` reported nothing", and since 2.4 a Send or Share bound met before the fields are known puts itself
off and reports later: the impl was attached, and N0334 came afterwards with no position. A check that puts itself
off is now "not yet": the instance gets the impl when the question is asked again, once the
fields are known. A message never suggests a bound that cannot be written (`C.Out: Send`).

## 3. Why each program is refused

| Program | Refused by | With |
|---|---|---|
| §1, and the default method `fn last(borrow this, other: Self) -> T => other.get()` | 2.3: `get`'s body lends `this.text` where the result is deep | N0245 in the impl |
| an impl storing a parameter its interface does not mention | 2.3 | N0272 in the impl |
| `out.push(it.next())` with a local's text in the iterator | 2.2: `collect`'s flow `out` ← `it` (a bound's name), seen by the caller | N0272 at the call |
| a scoped implementation behind `borrow mut dyn Keep<StringView>` | 2.2: the `dyn` referent is a target | N0272 |
| `return S.build(local.view())` | 2.1: the `S` carries the loan | N0244 |
| F1 `make<S: Build<StringView>>(hint: StringView) -> S => S.build(hint)` | 2.1, 2.2: `hint` has no name of `S` | N0245 in `make` |
| F2 `launder<U>(o: Owner, f) -> U => apply<U>(o, f)` | 2.2: `apply`'s result borrows from `o`, a local of `launder` | N0244 in `launder` |
| F2 with a generic method `mapS<U>` of an interface | 2.3 narrows the impl's method to the interface's flows; its body returns a borrow of `this` | N0245 in the impl |
| F3 `let g = () => s.get(); return g();` and its variants | 2.5: the result holds what `g` captured | N0244; N0272 for `() => out.push(s.get())` |
| F4 `let g = () => into.keep(v);` | 2.5: a store through `into` of `v`'s loan, where `g` is made | N0272 |

## 4. Why it is sound: the assumptions, and what enforces each

A body is checked with every value that may hold a loan followed (2.1); what it returns and stores is held to its
declaration's flows; a caller applies the callee's flows, which are at least the declaration's (2.9). No step
reads a loan off a type. The argument needs each of these, and each has a test that fails without it:

| # | Assumption | Enforced by | Tests |
|---|---|---|---|
| A1 | An instance's flows include its declaration's | 2.9: flows inherited; ceesafec's internal check (N0216) | `check-fail/instance-flows-default-method`, `-default-static`, `-generic-impl`, `-dyn`, `-bound`, `-region-result`, `-region-tagged`, `-region-in-result` |
| A2 | A function type carries no flows between its arguments | 2.10: N0298; a closure's body cannot store a parameter through a parameter or a capture (N0272) | `check-fail/function-value-flows`, `-generic`, `-feed`, `-argument`, `-bound-instance`, `-closures` |
| A3 | No scoped type is Send or Share, and a bound cannot be evaded | §38.1; 2.4 | `check-fail/interface-parameter-bounds`, `generic-send-arguments`; `check-fail/closures-threads-sync-loans`; `run-pass/generic-impl-send-bound`, `-share-bound`, `-send-bound-plain-interface` (2.11) |
| A4 | A boxed closure's result (`dyn fn`) borrows from neither its parameters nor its captures | N0245 in the closure's body (§36.5) | `check-fail/dyn-fn-result-loans` |
| A5 | An instance that boxes a scoped argument, or borrows it, is refused | N0274, N0246 (§25.4) | `check-fail/scoped-instance-paths` |
| A6 | A built-in that gives a value of a type parameter's type gives contents, not a borrow of its operand's storage | by kind, 2.1 | `run-pass/container-element-loans`, `check-fail/container-element-loans` |
| A7 | An impl's body keeps its interface's flows | 2.3 | `check-fail/interface-impl-flows` |
| A8 | A closure's stores through its captures are the enclosing function's, where the closure is made | 2.5 | `check-fail/closure-capture-stores*`, `async-boxed-closure-loans` |
| A9 | A raw pointer carries no loan: what is read through one is `unsafe` code's | §27.1 | none: a statement about `unsafe` code |

Where it is weakest now: A6, which trusts each built-in by kind and not by a list; A4, an existing rule this ADR
relies on and probed but did not redesign; and closures kept in a struct's field or returned in a `Box`, which
rest on A8 plus the existing rules for where a closure may be kept.

## 5. What sound programs it refuses, and how they are written

- **With-resource and mapper functions over an unbounded result.**
  `fn withFile<T>(f: fn(borrow Disk) -> T) -> T { let file = …; return f(file); }` is N0244: `T` could be a view
  of `file`. Written `fn withFile<T: Send>(…)`. This is the commonest cost.
- **A closure that returns a `T` got from a captured local.** `let g = () => s.get(); return g();` is N0244 when
  `s` is a local or a by-value parameter, though with an implementation that holds a view the value would have been
  fine: the closure borrows `s`, and its result may hold what it captured. Written without the closure, or with
  `T: Send`.
- **A closure whose result of a type parameter's type is made from its own parameter**, `(x: StringView) => f(x)`
  for an `f: fn(StringView) -> U`: N0245 in the closure, whose result is not known to flow from `x`. Written
  with `U: Send`, or with the function value passed instead of a closure around it.
- **A helper that makes an `S` from a view by a bound with a scoped argument.**
  `fn make<S: Build<StringView>>(hint: StringView) -> S` is N0245. Written `fn make<T, S: Build<T>>(hint: T) -> S`.
- **A generic function that stores into a parameter something made from another, with no shared name**
  (`fn fill<S: Build<StringView>>(out: borrow mut Vector<S>, hint: StringView)`): N0272; written generically in
  the argument's type as above.
- **A lending implementation** (`impl Stream<StringView> for Lines`, which owns its text): N0245. It implements an
  interface whose method returns the borrowing type (`fn next(borrow mut this) -> StringView?;`).
- **A closure that stores a capture through another capture when that is in fact fine but the checker cannot tell
  which capture is stored**: the store is accounted for every other capture, so a closure that captures a
  `borrow mut` parameter, a view of a local it only reads, and stores some third thing through the parameter is
  N0272. Written with the store outside the closure, or the view passed as an argument where it is read.
- **`Async.tasks` with an error type that is not Send** (2.6).
- A result that may hold the loans of more arguments than before, at callers: of every scoped argument of a
  function that takes a function value (2.2), and of what is stored through an argument the result flows from.

## 6. What was newly refused, measured

Measured again on the released line (0.42.0 with this ADR and the `Send` and `Share` rule, without the features
the fix was developed beside): the corpus (953 files), `lib/`, `tests/packages`, the guide's and the book's
listings, and the compiler's own source, which builds itself to a fixed point with the rule:

- `lib/core/async.cee`, `AsyncTasksIn.run`: changed by 2.6.
- Third round: the interface method whose result is written with the interface's parameters is elided again when
  the interface is given scoped arguments, as in 0.42.0 (the second round had switched that off, which let
  `impl Run<StringView> for Unit {}` inherit a default whose result borrows from a parameter the instance did not
  list). Nothing in the corpus, the libraries, the guide, the book or the compiler is refused by §2.9 to §2.11.
  The differential against 0.42.0 (every `check-fail` test and every probe of the three reviews and of the fix's
  branch: what 0.42.0 refuses and 0.42.1 accepts) is three files, none of which faults: two probes that use the
  rule that a `T: Send` holds no loan in a thread's or a closure's result, and the probe that became
  `run-pass/interface-impl-narrowed-flows`, which 2.3 accepts by design (a direct call of an impl's method has the
  interface's flows, against which the method's body is checked).
- Corpus expectations changed, each a function that was already refused, which now reports the return as well as
  the closure: `check-fail/generic-send-loans` (`copyOnly`, `named`: N0244 at `return f();`),
  `check-fail/thread-closure-moves` (`throughClosure`: the same).
- Nothing else: no other library function, corpus test, package test, guide or book listing, and nothing in the
  compiler. Four corpus programs that the first implementation of the principle refused (`run-pass/enum-methods`,
  `interface-self`, `impl-bounds`, `once-lock`) showed gaps in 2.2, which now has `Self`'s names, the deep source
  that a `borrow fn` is, and no flow into a `borrow mut fn`; five library functions (`HashMap.put`,
  `OrderedMap.put` and `exchange`, `OnceCell.tryInit`, `OnceLock.tryInit`) showed the need for "through a stored
  value" and for a function type's result names.

## 7. The alternatives rejected

- **Keeping parametricity and patching each shape** (the first form): two reviews, twelve holes.
- **Letting a result of a type parameter's type borrow from every scoped parameter, always** (the literal reading
  of "as a `StringView` result under elision"): sound, and it makes the result of every lookup with a borrowed key,
  `map.get(borrow key)`, a borrow of the key. 2.2 gives that reading only to functions that take a function value,
  where it is what the body does; elsewhere the body is held to the names.
- **Taking a `T` returned by a bound's method as a borrow of the receiver** (not deep): refuses every adaptor that
  keeps an item while it asks for the next.
- **Accounting a closure's stores at each call**, not where it is made: a closure handed to a function is called
  where the checker does not look.

## 8. Tests

`check-fail`: `interface-impl-flows`, `interface-flows-escape`, `generic-own-storage-loans`,
`interface-parameter-bounds` (first round); `generic-loans-function-value`, `-function-value-concrete`,
`-generic-method`, `-bound-helper`, `-bound-helper-caller`, `-paths`, `-supertraits-async`,
`-function-value-paths`, `-methods-async` (F1, F2, and this round's own probes);
`closure-loans-returned`, `-type-parameter`, `-block-copy` (F3); `closure-capture-stores`, `-bound`, `-vector`,
`-forms`, `-struct`, `-method`, `-calls` (F4); `cell-of-loans-fields`, `cell-of-loans-refcell` (2.7).
`run-pass`, native and `--interp` under `--check-leaks`: `interface-result-flows`, `interface-flows-generic`,
`dyn-scoped-sink`, `interface-send-defaults`, `interface-impl-narrowed-flows` (2.3: a signature that could add a
flow and does not; a lookup whose key dies before the result), `generic-code-that-is-sound` (the second review's sound programs: a
sound iterator, `for`, collect, callbacks, a with-resource function under `T: Send`, a recursive Send type).

## 9. Not in this ADR

- A flow between two `borrow mut` parameters that share a type parameter directly still leaves both arguments
  borrowed after the call, as before.
- A way to write, on a signature, that a result of a type parameter's type borrows from a named parameter (the
  `borrow(a, b) T` of reference results), which would let `make` of §5 be written as it stands.
- A region parameter on an interface.

## 10. Known not to be covered by 0.42.1

The third review found three more families of programs without `unsafe` that pass `check` and read freed memory.
None needs a type parameter, an interface or a closure: they are in plain code, 0.42.0 accepts them too, and
0.42.1 was released without their fix so that the fix above was not held back. 0.42.2 follows with it.

1. **A store by a built-in.** `replace(mut a, v)`, `swap(mut a, mut b)`, `fill`, `copyFrom`, `extend` and `resize`
   put a value into a place and record no store. A program is not refused when it puts a view of a local into a
   place that outlives the local with one of them and reads it afterwards: `swap(mut outer, mut inner)` for two
   structs that hold a view, `keep.viewMut().fill(t.view())`, `keep.extend(views[..])`, `keep.resize(2, t.view())`,
   where `keep` or `outer` outlives `t`. The same store written as an assignment or a `push` is refused (N0243).
2. **What `replace` returns.** The old value that `replace(mut a, v)` gives back carries no loan, so a view of a
   local taken out of `a` by `replace` may be kept after the local is gone.
3. **A `borrow mut` held in a scoped value passed by value.** Only a `borrow mut` parameter is a target of stores
   (§25.6). A function that takes a struct with a `borrow mut(r) Vector<StringView<r>>` field by value and pushes
   or assigns through that field stores where no caller sees it: `fn put(s: Sink, v: StringView) { s.out.push(v)!; }`
   called as `put(Sink { out: borrow mut keep }, t.view())`.

Until 0.42.2: a program that uses none of the six built-ins on values that hold a borrow, and passes no struct that
holds a `borrow mut` by value to a function that stores through it, is not affected.
