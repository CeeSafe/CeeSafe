# ADR 0131 — Flows through interfaces, and the loans of a value of a type parameter's type

- Status: **Proposed** by the integrator on 2026-11-13 after an independent soundness review found the hole;
  implemented on branch `fix/interface-result-loans`; awaits the owner's acceptance; not yet released.
- Scope: the checker (`src/check/references.cee`, `interfaces.cee`, `generics.cee`, `declare.cee`, `members.cee`,
  `closures.cee`), the borrow checker (`src/nir/borrow.cee`), the NIR builder (one guard), one field of a HIR
  function; `lib/core/async.cee` (one bound); `spec/type-system.md` §21.1, §21.2, §25.6, §25.7, §40;
  `spec/diagnostics.md`; the guide's async page. No new diagnostic code, no change to the backends or the
  interpreter.

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

`drain<StringView, Owner>(owner)` returns a view of a string that `drain` has dropped. Generic code is checked once,
in its generic version, and never again for an instance (§20.3); so what it may assume of a bound's method must
hold for every implementation. It assumed nothing and was told nothing: an interface's method had no flows at all
when its result was a type parameter (`ResolveFlows` gave flow names to a struct's type parameters, not to an
interface's), and an impl's method was compared with the interface's by its types alone.

Looking at the neighbours with that shape found the same fault five more times, each a use of freed memory in
0.42.0 (`tests/reference/tests/corpus/check-fail/interface-impl-flows`, `interface-flows-escape`,
`generic-own-storage-loans`):

1. a default method `fn last(borrow this, other: Self) -> T => other.get();`;
2. an impl that stores a parameter where the interface says nothing of it: `fn put(borrow mut this, v:
   StringView<r>)` for a non-generic `fn put(borrow mut this, v: StringView)`, called through a bound or `dyn`;
3. a `T` from a bound's method stored through an out-parameter (`out.push(it.next())` with `it: borrow mut I`,
   `I: Stream<T>`, `out: borrow mut Vector<T>`): the function's signature had no flow from `it` to `out`;
4. a scoped implementation behind `borrow mut dyn Keep<StringView>`, whose method stores its argument: a `dyn`
   referent was taken to hold no loan;
5. a value of a type parameter's type that holds a loan of a local, given by a call: `S.build(text.view())` under
   `S: Build<StringView>`, or `f(borrow buffer)` for `f: fn(borrow Buffer) -> T`. The borrow checker carried a loan
   from copy to copy only for a scoped type, so `let r = f(buffer); return r;` returned it unseen.

## 2. Decision

2.1 **An interface's method has the interface's flow names** (§25.6), whatever it is declared for: a bound's type
parameter, `Self`, a `dyn` type, the type that inherits a default. Its receiver has the interface's type parameters
and `Self`, as a struct's method's receiver has the struct's; `Self` elsewhere in the signature has the interface's
parameters; a projection `P.Item` counts as `P`. So `fn get(borrow this) -> T` has a **deep result** of `this`, by
the rule a generic struct's `fn get(borrow this) -> T` already has (ADR 0074 §2.4, ADR 0075 §8): it carries what
the receiver holds, not a loan of the receiver. `fn put(borrow mut this, v: T)` has the flow `this` ← `v`.

2.2 **A bound shares its names.** The flow names of a type parameter include those of its bounds' arguments, and of
theirs in turn: under `S: Source<T>` a parameter of type `S` has the name `T`. So `drain`'s result flows from `s`,
and a caller in generic code knows it. A flow between two parameters that only a bound gives is a **deep flow** when
its source is a reference that is not a view: only what the source's referent holds may be stored (N0272 for a loan
of the reference itself or of its referent's own storage), and a caller's argument contributes what it refers to
holds. `collect(mut words, mut all)` then leaves neither argument borrowed.

2.3 **An impl's method keeps the interface's flows** (§21.1). It may have no flow that the interface's method,
declared for `Self` with the interface's parameters opaque, lacks: a result that borrows from a parameter the
interface's result does not flow from, a result that borrows from a reference parameter itself where the
interface's is a deep result of it, a parameter stored through another where the interface has no such flow. N0267,
on the method. `impl Source<StringView> for Owner` above is refused there: "its result may borrow from 'this'
itself, but the interface's result may hold only what that parameter's referent holds".

What remains possible: a type that holds a view gives it (`impl<region r> Source<StringView<r>> for Holder<r>`,
`-> StringView<r> => this.text`); static data is `-> StringView<static>`; a type that lends its own storage
implements an interface whose method returns the borrowing type itself, `fn get(borrow this) -> StringView;`, which
every caller then treats as a borrow of the receiver.

2.4 **`borrow dyn I<T>`.** What a `borrow dyn I<T>` or `borrow mut dyn I<T>` refers to may be a scoped
implementation (§37.2): it has the flow names of `I`'s arguments, and a `borrow mut dyn I` parameter is a target of
flows as any `borrow mut` of a type that may hold loans. An owned `dyn` value (`Box<dyn I<T>>`) still holds none
(§25.7 rule 5), nor does a `dyn I + Send`.

2.5 **A value of a type parameter's type keeps a loan of the function's own storage** (§25.7 rule 5). What such a
value holds of the function's parameters is in its type: every place it can go shares a flow name with where it
came from, which is why the checker never followed those loans from copy to copy, and still does not. A loan of a
local is not in its type. It is now carried through copies, moves and aggregates: returning the value is N0244,
storing it through a parameter N0272. In a closure's body a borrow of a capture is such a loan (N0245, the existing
rule of §36.2 that a closure's result cannot borrow from the closure); and a call of a closure gives a result of
such a type its arguments' loans, not the closure's own, so `let g = () => s.get(); return g();` stays accepted.

2.6 **An interface's parameters' bounds are checked** wherever the interface is given arguments: an impl's header,
a bound, a `dyn` type, a supertrait (§21.2; N0268, N0334). `interface Source<T: Send>` was a bound in name only,
and with rule 5's "a `T: Send` holds no loan" a default's body trusted it. `Send` and `Share` are decided by a
type's fields, so for a type that a declaration names they are now checked once every struct and enum has its
fields: before, `struct Wrap<T: Share>` accepted a field or a parameter of type `Wrap<Cell<i32>>`.

2.7 **`Async.tasks` and `Async.tasksIn`: `E: Copy + Send`.** `AsyncTasksIn<E, A>.run` calls its body, a function
value, with a borrow of the set it made, and returns the body's `E`. By 2.5 that is refused unless `E` holds no
loan, which `Send` says.

## 3. Why it is sound

A generic body is checked against the flows of what it calls. For a bound's or a `dyn` type's method those are now
the interface's (2.1), and 2.3 makes every implementation stay within them, so the body's check holds for every
instance. A caller of an instance uses the instance's own signature, whose flow names are those of the same
declaration (2.2 adds the bound's names to both) plus the elision of §25.6 for a scoped result, so it sees at least
the flows the body was allowed. Loans of parameters in values of a type parameter's type are covered by the names
(parametricity: a `T` can only come from something whose type mentions `T`, or, by 2.2, a type bounded by an
interface over `T`); loans of locals, which the names cannot cover, are followed (2.5).

## 4. The alternative rejected

To take a `T` returned by a bound's method as a borrow of the call's borrowed arguments, the receiver included
(the conservative reading of rule 5, as a non-generic `fn get(borrow mut this) -> StringView` is read), and leave
impls alone. It is sound, and refuses the generic code people write: `let a = it.next(); let b = it.next();` would
hold two mutable borrows of `it`, so no adaptor that keeps an item while it asks for the next (collect, zip, a
peeking parser) would check, unless `T: Send`. The rule chosen refuses instead the implementation that lends its
own storage through a result declared `T`, which generic callers cannot use safely in any case, and which has two
other spellings (2.3). It is also the rule a generic struct's method already has.

Following every loan in values of a type parameter's type (2.5 without "own storage") was tried: it refuses
lib/core's `Vector` helpers, `Cell`, `Volatile` and `Lazy` (ten sites), each a value read through a parameter that
its type already accounts for.

## 5. What is newly refused

Measured on the corpus (927 files), `lib/`, `tests/packages`, the guide's and the book's listings, and the
compiler's own source, which builds itself to a fixed point with the rule:

- `lib/core/async.cee:289`, `AsyncTasksIn.run`: `return Err(e)` with the body's `e` (2.5). Changed by 2.7. No test,
  guide or book listing gives `Async.tasks` an error type that is not Send.
- Nothing else. The corpus tests `check-fail/generic-send-loans` and `check-fail/thread-closure-moves` keep their
  expectations.

Programs outside those that are refused where they were accepted: an impl that lends its own storage through a
result the interface declares with a type parameter (an owning "lending iterator" as `impl Stream<StringView> for
Lines`); an impl with a flow its non-generic interface lacks; an argument that does not meet an interface's
parameter's bound; a `Wrap<T: Share>` or `<T: Send>` named in a declaration with an argument whose fields make it
neither; generic code that returns or stores a `T` that a function value or a bound's function made from a borrow
of a local, without `T: Send`.

## 6. Tests

`check-fail/interface-impl-flows` (2.3: the program of §1, the default method, an async method, `T?`,
`Result<T, E>`, a struct of `T`, a vector of `T`, a slice, `borrow mut this`, a stored parameter, a static
function, an associated type); `check-fail/interface-flows-escape` (2.1, 2.2, 2.4: thirteen callers that would
outlive what they borrow); `check-fail/generic-own-storage-loans` (2.4, 2.5); `check-fail/interface-parameter-bounds`
(2.6); `run-pass/interface-result-flows`, `run-pass/interface-flows-generic`, `run-pass/dyn-scoped-sink`,
`run-pass/interface-send-defaults` (what must keep working: bounds, `dyn`, defaults, associated types, static
functions, a generic struct, `for`, threads, closures), native and `--interp`, under `--check-leaks`.

## 7. Not in this ADR

- A flow between two `borrow mut` parameters that share a type parameter directly (`fn pour<T>(from: borrow mut
  Vector<T>, to: borrow mut Vector<T>)`) still leaves both arguments borrowed after the call, as before: making
  every such flow deep would refuse callees that store a reborrow, and was not needed here.
- An interface's method whose result is a type parameter and which has two reference parameters cannot be given a
  scoped argument (`Pass<StringView>` for `fn pass(borrow this, v: T) -> T` is N0245, by elision), as before.
- A region parameter on an interface, which would let a non-generic interface declare a stored loan.
