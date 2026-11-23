# ADR 0168 — A type parameter bounded by `Send` or `Share` is no flow name

- Status: **Accepted** by the owner, 2026-11-18 ("I accept all the ADRs as written"): every question of the
  "Questions for the owner" section as recommended. Written at the owner's request ("go ahead and write those ADRs")
  from the findings of the HTTP server written in CeeSafe (`~/dev/CeeSafeApps/httpserver/CEESAFE_FINDINGS.md`,
  S14, its first half). Implemented 2026-11-18 (§6); released in ceesafec 0.67.0.
- Scope: the flows of a signature (`src/check/references.cee`, `resolveFlows`); type-system.md §25.6, the guide's
  ownership page. No change to the grammar, to the borrow checker's rules (§25.7), to the backends, to a library or
  to the runtime.

## 1. Context

- **What is refused** (reduced; ceesafec 0.65.1):

  ```ceesafe
  struct Ctx<region r, S> { state: borrow(r) S; seen: u32; }
  struct Plain<region r> { state: borrow(r) u32; seen: u32; }

  interface Handler<S: Send + Share> {
      fn handle(borrow this, ctx: borrow mut Ctx<S>) -> u32;
      fn plain(borrow this, ctx: borrow mut Plain) -> u32;
  }

  impl<S: Send + Share> Server<S> {
      fn handler(borrow this, index: usize) -> borrow Box<dyn Handler<S> + Send + Share> => borrow this.handlers[index];

      fn dispatch(borrow this, ctx: borrow mut Ctx<S>) -> u32 => this.handler(0).handle(mut ctx);
      //  N0272: this stores a value borrowed from a temporary value into what 'ctx' refers to,
      //         which outlives this call
      fn other(borrow this, ctx: borrow mut Plain) -> u32 => this.handler(0).plain(mut ctx);     // accepted
  }
  ```

- **Why.** The two calls differ in one thing: `Ctx<S>` mentions `S`. In an interface's method the receiver has the
  interface's type parameters as flow names (§25.6, ADR 0131), a `borrow mut` parameter whose referent shares a
  name with another parameter is a target of it, and so `handle` may store a loan of its receiver into `ctx`. The
  receiver here is a borrow of a temporary, and `ctx` is `dispatch`'s own parameter: N0272. The bound
  `S: Send + Share` is on the interface and on the impl, and changes nothing.
- **The checker already knows such a parameter brings no loan.** §25.7 rule 5: "Nor does a type parameter bounded
  by `Send` or `Share` bring a loan: no scoped type is Send (§38.1), so a value of type `T` under `T: Send` holds
  none". And §25.6: an owned `dyn` value and a `dyn I + Send` have no flow names. The flows of a signature do not
  use it: `S` is a name there whatever its bounds.
- **What it costs.** A function cannot be given a context and pass it on to a handler: the function that makes the
  context must be the one that calls the handler. In httpserver that put routing, the middleware chain, the handler
  call and the access log in one method, and it is why a library cannot offer "run this handler with that
  context" as a function of its own.

## 2. Decision

### 2.1 The rule

**A type parameter whose bounds include `Send` or `Share` is not a flow name** (§25.6): of a parameter, of the
receiver, of the result, or of what a bound or an interface gives them.

- The bounds are the declaration's: a function's own, its struct's or enum's, its impl's, and for an interface's
  method the interface's (`interface Handler<S: Send + Share>`). An impl that bounds what its interface does not
  gains nothing: the flows of an interface's method are the interface's (ADR 0131 §2.9).
- It is the name that is dropped, not the parameter's other names. `Ctx<region r, S>` under `S: Send` keeps `r`;
  a receiver keeps `Self` and the interface's unbounded parameters.
- A bound that is reached only through another bound (an interface that itself requires `Send`) counts if rule 5
  counts it today, and not otherwise: the two rules use one test.

### 2.2 Why it is sound

A flow name says "a value of this type may carry a loan from here to there". A value of type `S` under `S: Send`
or `S: Share` holds no loan: that is rule 5's statement (§25.7), which rests on §38.1, and this rule uses it and
adds nothing to it. So nothing can travel under that name:

- **into a target:** the callee could store only into a place of the target whose type can hold a loan. A place of
  type `S` cannot; a place that can (`state: borrow(r) S`) names a region, which is its own flow name and stays.
- **into a result:** a result of type `S` holds none (rule 5 has this already: such a result has no sources per
  instance; the rule makes it so for the declaration).

What the callee can still do with its receiver's loan is what the remaining names allow, as before.

### 2.3 What changes

- The program of §1 is accepted: `handle`'s receiver and `ctx` share no name, so nothing is stored, and
  `dispatch` passes `ctx` on.
- A function that was accepted stays accepted: flows are only removed, and a caller with fewer flows to honour
  honours them.
- **A body that relied on such a flow is refused**: one that stores a loan of one parameter into another, with
  `S: Send` as the only name they share. No such body can exist without `unsafe`: there is no place of a `Send`
  type to store a loan in. The corpus, the libraries, ClayWind, CeeSafe.Web and the two HTTP packages are checked
  before the rule lands, and the count goes in ROADMAP §10.

## 3. What it does not do

- It does not touch an unbounded parameter, or one bounded by `Copy` or by an interface that is not Send: "a `T`
  with other bounds only, `Copy` included, may be a reference" (rule 5).
- It does not lift the second half of httpserver's S14: a closure that captures a context and a `borrow mut`
  parameter and stores into the context is refused as if it stored into the parameter. That is reduced
  (httpserver's CEESAFE_FINDINGS.md, S16) and looks like a fault of the checker, not a rule: it is not for an ADR
  until it is understood.

## 4. Questions for the owner

1. A type parameter bounded by `Send` or `Share` is no flow name (§2.1)? Recommended: yes.
2. `Share` alone, as well as `Send`? Recommended: yes: rule 5 already treats the two alike. If §38.1 turns out to allow
   a `Share` type that holds a loan, the rule is `Send` alone and rule 5 has a fault to be fixed with it.
3. Should an impl's stricter bound count for its own methods called on the concrete type (not through the
   interface)? Recommended: no, in this ADR: the flows are the declaration's (ADR 0131 §2.9), and one rule is
   easier to hold than two.

## 5. Implementation plan (once accepted)

1. Spec: §25.6 (the list of flow names: "a type parameter is one unless its bounds include `Send` or `Share`"),
   §25.7 rule 5 (a reference to it), the guide's ownership page with the handler example.
2. `src/check/references.cee`: `resolveFlows` leaves out such parameters when it collects the names of a
   parameter's, the receiver's and the result's types, and those a bound's arguments add; the self-check that a
   function has its declaration's flows (N0216) uses the same list.
3. Tests:
   - check-pass: the program of §1; a `Share`-only bound; a generic function that
     passes a `borrow mut Holder<T>` to a callee under `T: Send`;
   - check-fail: the same programs without the bound (N0272, as today); a parameter bounded by `Copy` only; a
     store through a region the two parameters do share;
   - run-pass: a dispatcher that is handed a context and calls a boxed handler with it;
   - the contract suite, and the packages of §2.3.
4. ROADMAP §10.

## 6. Implementation notes (2026-11-18)

- `resolveFlows` collects a signature's type-parameter names with `pushFlowTypeNames`, which leaves out a parameter
  whose written bounds include `Send` or `Share`: the test rule 5 makes (`sendBound`, `shareBound`, set from the
  same syntax in `src/check/generics.cee`). It is used for the function's own parameters, its struct's or enum's,
  and its interface's, so `Self`'s names lose them too. Nothing else changed: the flows, the self-check of
  declared flows (N0216) and the borrow checker read the names as before.
- **§2.3 was wrong about what stays accepted**, found when the packages it lists were checked. It said that a
  function that was accepted stays accepted and that no body can rely on a flow under a `Send` name. One can: a
  method of `struct App<S: Send + Share>` that passes its `ctx: borrow mut Context<S>` to a method of
  `interface Middleware<S>`, **whose `S` has no bound**. The callee still declares that it may store its receiver
  into the context, and the caller's own signature, which allowed that under the name `S` before, no longer does:
  N0272 (httpserver, `App.enter`; one place). CeeSafe.Web's packages, httpclient, the libraries and the corpus are
  unchanged. The program is accepted again when the interface declares the bound its users have
  (`interface Middleware<S: Send + Share>`), which then has to be written on the structs that hold such a
  handler (`Router<S>`): mechanical, and what the rule is for, but it is a program that compiled and no longer
  does. **This is for the owner to decide before a release** (ROADMAP §10): keep the rule as accepted and say so
  in the release notes, or narrow a callee's flows at each call by what its type arguments are there (an `S`
  whose argument can hold no loan carries nothing), which accepts both and is more work in the checker.
- **More than §2.3 said.** The closure that httpserver could not write
  (`Async.timeout(ms, late, async () => await this.dispatch(found, mut ctx, request, exchange))`, its findings'
  S16) is accepted too once the interface bounds `S`: what was charged to the other capture was the store that the
  shared name allowed, and there is none now. Without the bound it is refused as before, and why that store is
  charged to the wrong capture is still not understood.
- **Tests:** run-pass/adr0168-send-parameter-no-flow (a dispatcher handed a context calls two boxed handlers with
  it; the output is the program's arithmetic) and check-fail/adr0168-unbounded-parameter-flows (no bound, and
  `Copy` alone: N0272 as before). The corpus: 0 failures.
