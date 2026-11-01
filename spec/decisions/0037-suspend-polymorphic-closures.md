# ADR 0037 — L1 A4: higher-order functions that suspend when their closure does

- Status: **Accepted** by the owner, 2026-11-01 (every question as proposed). Phase 3 of what is left of L1 A4
  (ROADMAP, the owner's "in a multiphased way").
- Scope: letting a function that takes a closure (`forEach`, `any`, `retain`, a user's `retry(op)`) be called with an
  async closure, so that the call is a task that suspends where the closure does. It extends `@effectsOf` (§32.5,
  ADR 0020) and async closures (§40.3).

## 1. Context

Today a closure type never has `suspend` (§36.3): "only an async closure's call suspends, and that is a task". So
`values.forEach(async (v) => await send(v))` is rejected: `forEach` is a sync function and cannot await its argument.
Each higher-order function would need an async twin (`forEachAsync`, `anyAsync`, …). Rust has the same split, and its
keyword generics are not settled.

CeeSafe already has the two pieces a third way needs:

- **`@effectsOf(p)`** (§32.5) makes a function's effects depend on its closure argument's: a call has the callee's own
  effects plus the argument's. `suspend` is an effect (§32.1), so "the call suspends when the argument does" is the
  same rule, one effect further.
- **The program is compiled whole**, and calls of closures that are known are already rewritten into a variant of the
  callee bound to the literal's body (optimisations.md §5). An async argument needs that variant always, since the
  callee's frame must embed the literal's.

## 2. Options

| | A. Async instances of `@effectsOf` functions (proposed) | B. An `async?` marker on functions | C. Async twins in the library |
|---|---|---|---|
| Written | `await values.forEach(async (v) => …)` | `async? fn forEach(…)`, then as A | `await values.forEachAsync(async (v) => …)` |
| New syntax | none | a marker, its rules for the body | none |
| Which functions | every `@effectsOf` function | those marked | those written twice |
| Library | `forEach`, `any`, `all`, `retain` work as they are | each marked | each duplicated |
| Body checked | per instance, as an async body | per instance | once each |

**A is proposed.** `@effectsOf` already says "my effects are my argument's", and `suspend` is an effect. B adds
syntax for what the attribute already states. C doubles the library and every user's higher-order code.

## 3. Proposed decision

1. **An async argument.** An `@effectsOf` parameter `p` of type `fn(A…) -> R` (by reference or not) accepts an
   async closure literal whose task's result is `R`, or a named async fn of that signature (§36.1), unless its type
   has an effect list without `suspend` (N0295 then, as for any argument exceeding its list). In such a parameter's
   type alone, an effect list may name `suspend` (N0290 elsewhere, as today).
2. **The call is a task.** A call that passes an async argument for any `@effectsOf` parameter is a task (§40.1):
   `await f(…)`, `Async.run(f(…))` or a spawn's body, N0342 elsewhere. Its value is `f`'s result.
3. **Its async instance.** The task runs `f`'s *async instance* for those arguments. It is a copy of `f` in which
   each call through `p`, or through a local that holds `p` (§32.5's rule), awaits the argument, and passing `p` on
   to another function's `@effectsOf` parameter awaits that function's async instance. There is one instance per
   (`f`, the literals or functions bound to its parameters), as for closure-call variants (optimisations.md §5.1). It
   is made whether or not that optimisation is on, because it is semantics, not an optimisation.
4. **The instance is checked as an async body** (§40.1) after effect inference, with its chain naming the call that
   made it ("in the async instance of 'forEach' for the closure at main.cee:12"):
   - a context that forbids `sleep`: a blocking call in `f` is N0295 at the caller's call, naming it;
   - `suspend` while a `@context(atomic)` value may be live: N0295;
   - recursion through the instance (`f` calling itself with `p`): N0343, at the caller's call. There is no sure fix
     there, since the recursion is in `f`.
   Borrow checking is not repeated: an await is checked as a call (§40.2), and `f`'s calls through `p` were checked
   as calls.
5. **Effects** of the call: `f`'s own, the argument's (with `suspend`), and the instance's (§32.5's table, unchanged
   otherwise).
6. **Cancellation** is the argument's (§40.7). `f` sees only the values its awaits return. A cancelled
   `forEach` keeps calling its closure, whose leaf operations return `Err` at once. A function that should stop
   early must look at the results, as for any closure.
7. **lib/core.** `forEach`, `any`, `all` and `retain` work with async closures as they are. `sortBy`'s introsort is
   recursive, so its async instance would be N0343. It becomes iterative (an explicit stack of at most
   `2 · log2(length)` ranges, in the frame), so that `await s.sortBy(async (a, b) => …)` works too (question 3).
8. **The interpreter** runs an instance like any async fn. **The C backend** emits it like any async fn, named
   `ceeAS<n>_<f>`, with its frame embedding the literal's.

## 4. Consequences

- The checker: an `@effectsOf` parameter accepts async arguments; such a call is a task. NIR: a pass after
  `buildNir`, before effect checking, makes the async instances (from a worklist, deterministic as §5.1's) and turns
  calls through bound parameters into awaits. The effect checker and the coroutine pass treat instances as async
  fns with their chain. The backend and the interpreter take instances where they take async fns.
- lib/core's introsort becomes iterative; `benchmarks/sort` must not regress (guide/performance.md).
- Spec: type-system.md §32.5, §36.3, §40.1, §40.3; optimisations.md §5.1 (instances are not variants);
  c-backend.md §3a; diagnostics.md N0295, N0342 and N0343.
- Tests:
  - run-pass: `forEach`, `any`, `retain` and `sortBy` with async closures; a user's `retry(op)`; a named async fn
    as the argument; `p` passed on to another `@effectsOf` function; an instance cancelled midway; C and the
    interpreter agreeing;
  - check-fail: N0342 for an unawaited call; N0295 for a blocking call in the instance and for a list without
    `suspend`; N0343 for a recursive instance; N0290 for `suspend` in a non-`@effectsOf` type;
  - frames: an instance's frame;
  - the cancel sweep over the run-pass programs.

## 5. Questions for the owner

1. The approach, A (async instances of every `@effectsOf` function, no new syntax)? **Yes.**
2. Async arguments: closure literals and named async fns only (there is no type of async closure values to store or
   pass otherwise)? **Yes.**
3. `sortBy`: make introsort iterative, so it accepts an async comparator? Or leave it recursive, with N0343 at an
   async call? **Iterative.**

## 6. Implementation (2026-11-01)

- **Checking** (§3.1–3.2): `synthCall` (`src/check/expr.cee`) checks an async closure literal for a parameter that
  `@effectsOf` names (`namesEffectsOf`, before `finishFunctionFacts` resolves the attribute), and a named async fn
  becomes an async adapter there (`adapterClosure`, whose body awaits it). Such a call is a task
  (`checkSuspendingCall`). `v.retain(async …)`, a builtin, does the same, and marks its call of lib/core's retain as
  awaited. An async fn is a function or closure value nowhere else: N0298. This also closes a hole that predates
  this ADR: `let g: fn(u32) -> u32 = asyncFn;` was accepted, and its address was null in C.
- **Instances** (§3.3): `src/nir/async_instances.cee`, after drop elaboration and before the effect checker. An
  instance is a HIR function copied from `f` with `asyncInstanceOf` set (so `isAsyncFunction` holds), its call site in
  `siteFile`/`siteStart`/`siteEnd`, and the bound parameters dropped from its `@effectsOf` list. Its NIR is `f`'s with
  new statements where calls change. The known-closure analysis is closure specialisation's (`specialisedClosures`).
  `Async.boxed(alloc, f(…))` takes the instance too.
- **Errors** (§3.4) are reported at the making call: the effect checker reports an instance's own operations there
  and not again in its caller, and the coroutine pass reports a recursive instance there.
- **Effect lists:** §36.3 already lets a type's list name `suspend`, so there is no new N0290. §3.1's "N0290
  elsewhere, as today" was wrong; an `@effects(suspend)` type outside `@effectsOf` simply has that effect.
- **`--explain-frames`** reports an instance at its call: "the async instance of 'twice' for this call".
- **sortBy** (§3.7): introsort keeps its pending ranges in three arrays of 64 in its frame, the shorter side first, so
  the comparator is called in the order the recursion called it. Measured 2026-11-01 on macOS (Apple clang, `-O2`, a
  `sortBy` of 2 000 000 `u32`, 15 interleaved runs of each build): 126.4 ms recursive, 128.0 ms iterative (+1.3%; two
  copies of one build differ by 0.1%). One array of a range struct measured 128.8 ms, so it is not used.
- **Tests:** run-pass/async-effects-of (lib/core's forEach, any, all, retain and sortBy; a user's functions, one
  passing its closure on; a named async fn; Async.boxed; an instance in a spawned child, cancelled midway);
  check-fail/async-effects-of (N0342, N0298) and async-effects-of-instances (N0295 twice, N0343, N0342);
  frames/async-effects-of.
