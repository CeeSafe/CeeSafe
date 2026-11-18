# ADR 0165 — An impl for every argument of its interface

- Status: **Accepted** by the owner, 2026-11-18 ("I accept all the ADRs as written"): every question of the
  "Questions for the owner" section as recommended. Written at the owner's request ("go ahead and write those ADRs")
  from the findings of the HTTP server written in CeeSafe (`~/dev/CeeSafeApps/httpserver/CEESAFE_FINDINGS.md`,
  S7). **Not implemented, and not implementable as written** (§7, 2026-11-18): the owner decides how to go on.
- Scope: the checker's impls (`src/check/interfaces.cee`: `processImpl`, `implements`, `implMethodFor`), the lookup
  of a method on a concrete type, the `dyn` tables; type-system.md §21 and §20.4, diagnostics.md (N0265, N0267), the
  guide's generics page. No change to the grammar, to the backends, to a library or to the runtime.

## 1. Context

- **What is refused.** An impl's type parameters must each appear in its target type (ADR 0054 §3.1):

  ```ceesafe
  public interface Middleware<S> {
      async fn before(borrow this, ctx: borrow mut Context<S>, request: borrow Request) -> Result<Flow, HandlerError>;
  }

  public struct Cors { … }                     // reads nothing of an application's state

  impl<S> Middleware<S> for Cors { … }         // N0267: type parameter 'S' does not appear in 'Cors',
                                               //        so no instance could choose it (ADR 0054)
  ```

  The reason given is the one ADR 0054 had: an impl template is attached to each instance of its target's type
  constructor (`attachGenericImpls`), and the instance's arguments choose the template's parameters. `Cors` has no
  arguments, so nothing chooses `S`.
- **What it costs.** CORS, security headers, a rate limit and authentication are the same for every application,
  whatever its state type. httpserver could not write them once for `Middleware<S>`. It has a second interface
  without the parameter (`Filter`), a second context type that leaves the state out (`Scope`), two lists in the
  application and one chain of tagged indexes to keep their order, and a stock handler that is a function a handler
  calls, since `impl<S> Handler<S> for Health` is refused the same way. A user of the library meets two kinds of
  middleware where there is one idea.
- **The parameter is chosen, only not by the target.** Wherever such an impl would be used, the interface is named
  with its arguments: a bound `M: Middleware<St>`, a coercion to `dyn Middleware<St>`, a call whose argument is a
  `Context<St>`. The interface's arguments choose `S`.
- **What exists to build on.** Monomorphisation of impl methods per type argument list (ADR 0054 §3.1); declaring an
  instance's functions at its first use (`src/check/lazy_instances.cee`, type-system.md §20.4); inference of a
  generic function's type arguments from its arguments' types (§20).

## 2. Decision

### 2.1 The rule

**An impl's type parameter may appear in the interface's arguments instead of the target type.**

```ceesafe
impl<S> Middleware<S> for Cors { … }                    // for every S
impl<S: Send + Share> Handler<S> for Health { … }       // for every S that meets the bounds
impl<T, S> Middleware<S> for Timed<T> { … }             // T from the target, S from the interface
```

- Each parameter must appear in the target type **or in the interface's arguments** (N0267 otherwise, with the
  message "type parameter 'S' appears neither in 'Cors' nor in 'Middleware<…>'"). A parameter may appear in both;
  then the two must agree, as any unification must.
- Region parameters are as before: they are named for the target (`impl<region r> I for S<r>`) and are erased.
- Bounds on the parameters are a generic function's (§20), checked when the impl is used for given arguments.

### 2.2 Matching

`C<A…>` implements `I<B…>` when an impl's target unifies with `C<A…>`, its interface with `I<B…>`, and the bounds
hold for the arguments found. This is ADR 0054's rule with the interface's side added: for an impl whose parameters
all appear in the target nothing changes.

- **On demand.** An impl with a parameter that the target does not choose cannot be attached when the target's
  instance is made: the interface instances it will be asked for are not known then. It is attached when
  `implements` or `implMethodFor` is asked for the pair (type, interface instance) and finds no exact impl: the
  template is unified, its bounds checked quietly, and its methods declared for that pair, once. This is the step
  an instance's functions already take at their first use (§20.4), and it counts against the same depth limit
  (N0266, 64).
- **`dyn`.** A coercion to `dyn I<B…>` names the pair, so its table is that instance's, as for any impl.
- **Through a bound.** Inside `fn run<S, M: Middleware<S>>(m: borrow M, …)` nothing changes: `M` is opaque, and the
  call that instantiates `run` with `M = Cors`, `S = St` asks `implements(Cors, Middleware<St>)`.

### 2.3 A method called on the concrete type

`cors.before(mut ctx, request)` on a value of type `Cors` names no interface. The impl's parameters that the target
did not choose are then inferred from the call's arguments, **as a generic function's are** (§20): `ctx` is a
`Context<St>`, so `S = St`. If they are not determined, the call is N0265, and its message names the way
out: call the method through a bound or a `dyn` reference of the wanted instance (`let m: borrow dyn Middleware<St>
= cors;`).

- A method that does not mention the parameter in its signature (`fn name(borrow this) -> StringView` in
  `interface Middleware<S>`) cannot be called on the concrete type: every instance has one, and they need not be
  the same function. It is N0265. Through a bound or `dyn` it is called as any method is.

### 2.4 Overlap and the orphan rule

Both stand as ADR 0054 has them.

- **Overlap:** at most one impl of an interface (with arguments that could be equal) for a type constructor.
  `impl Middleware<u8> for Cors` beside `impl<S> Middleware<S> for Cors` is N0267, as `impl I for Wrap<u8>` beside
  `impl<T> I for Wrap<T>` is. There is no specialisation.
- **Orphans:** the impl is written in the package of the interface or of the target's type constructor. A
  library's `Cors` implements the library's `Middleware<S>`: both are its own.

### 2.5 Flows

Nothing is added. A method of an interface has its declared flows whatever implements it (§25.6, ADR 0131): the
receiver has the interface's type parameters as flow names. For `impl<S> Middleware<S> for Cors` the receiver holds
no `S`, and the signature still says it may; a caller through the interface could not know otherwise.

## 3. What it does not do

- **No specialisation**, so no impl "for every `S` but this one".
- **No impl for a bare type parameter** (`impl<T> I for T`): the target is still a struct or an enum (N0267).
- **No associated type chosen by the interface's argument beyond what an instance has**: an interface with
  associated types is implemented per instance, and `P.Item` under `P: I<A>` is that instance's.
- **No method type parameters of the impl's own** beyond what interface methods have (ADR 0086 §2.7).

## 4. Consequences

- A library writes middleware, a codec or a stock handler once for every argument of a generic interface.
  httpserver's `Filter` and `Scope` become unnecessary: `Cors`, `SecurityHeaders`, `RateLimit`, `BearerAuth` and
  `Trace` implement `Middleware<S>`, and `Health` is a `Handler<S>`.
- **Compile time and code size.** An impl is instantiated once for each distinct argument list it is used with,
  as generic impls are. A program with one application state has one instance of each.
- **Nothing that compiles today changes meaning.** The rule accepts programs that were N0267; the on-demand lookup
  runs only where the exact lookup found nothing.
- **N0265 gains a case** (§2.3). No new diagnostic code.

## 5. Questions for the owner

1. A parameter of an impl may be chosen by the interface's arguments instead of the target (§2.1)? Recommended: yes.
2. A direct call on the concrete type infers such a parameter from its arguments, and is N0265 when they do not
   determine it (§2.3)? Recommended: yes. The alternative, refusing every direct call of such an impl's methods, is
   simpler to state and makes a test of a filter go through a bound for no reason.
3. Overlap stays "one impl per interface and type constructor", so a concrete impl beside the general one is
   refused (§2.4)? Recommended: yes, until specialisation is designed for generic impls as a whole.

## 6. Implementation plan (once accepted)

1. Spec: §21 (the rule, matching on demand, direct calls), §20.4 (the on-demand step beside an instance's functions),
   diagnostics.md (N0265's and N0267's cases), the guide's generics page with the middleware example.
2. `src/check/interfaces.cee`:
   - `processImpl`: a parameter is accepted if it appears in the target or in the interface's arguments; the
     template records which side chooses each;
   - `implements`, `implMethodFor`: on a miss, the templates of the target's constructor for the interface's
     constructor are unified with the pair, their bounds checked, and their methods declared for it (cached by
     template and argument list);
   - the overlap check compares interface arguments "that could be equal", which it does already;
   - a method lookup on the concrete type finds the template's method and infers what the target did not choose.
3. Tests:
   - run-pass: one filter used by applications of two state types, through a bound, as `dyn`, and called directly
     with its parameter inferred; an impl with one parameter from each side; bounds on the interface's side; native
     and interpreted, the output given by the program's specification (which state each instance saw);
   - check-fail: a parameter in neither place; a concrete impl beside the general one; an unmet bound, reported at
     the use; a direct call that does not determine the parameter; the orphan rule;
   - the contract suite; httpserver with `Filter` removed, as the program that asked.
4. ROADMAP §10.

## 7. What was found before implementing (2026-11-18)

§1 listed what exists to build on and missed what does not: **a type cannot implement one generic interface for two
argument lists at all.** A type's methods are one set of names (`Checker.implMethods` is keyed by type and name,
`implMethodFor(typ, name)`), so the second impl's methods collide with the first's (ceesafec 0.67.0):

```ceesafe
interface Conv<T> { fn make(borrow this, seed: T) -> u64; }
struct X { k: u64; }
impl Conv<u8> for X { fn make(borrow this, seed: u8) -> u64 => this.k + seed as u64; }
impl Conv<u16> for X { fn make(borrow this, seed: u16) -> u64 => this.k * 1000 + seed as u64; }
//  N0228: 'X' already has a method named 'make'
//  N0267: 'impl Conv<u16> for X' is missing method 'make'
```

§2.2 has `Cors` implement `Middleware<A>` and `Middleware<B>` as two instances with methods of the same names on
the one type `Cors`: the same collision, made by the compiler. A call through a bound in instantiated code, a `dyn`
table and a direct call all find a method by the type and the name alone, so with two instances they would find
the first. Nothing of §2 was implemented.

Two ways on, for the owner:

- **A. First let a type implement a generic interface for several argument lists.** A method is then found by the
  type, the interface instance and the name: through a bound and through `dyn` the instance is known; on the
  concrete type a name that several instances have is chosen by the arguments (§2.3's rule, needed here already).
  It touches `implMethods`, `findMethod`, `implMethodFor`, the `dyn` tables and N0228, and is an ADR of its own;
  this one follows from it unchanged.
- **B. This ADR for one argument list a type.** The impl is instantiated on demand, as §2.2 says, for the first
  `Middleware<A>` the program asks of `Cors`, and a second, different one is an error that says so. A program has
  one application state type, so httpserver's `Filter` could still go; a test that builds applications of two
  state types with one filter could not be written. It needs nothing the compiler lacks, and A can lift the
  limit later without changing a program that compiles under B.
