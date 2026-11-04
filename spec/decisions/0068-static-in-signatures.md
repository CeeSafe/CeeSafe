# ADR 0068 — `static` in signatures: rejected on parameters, enforced on results

- Status: **Accepted** by the owner, 2026-11-04 ("Tackle ADR 0067 §4 too", then "Reject on params, enforce on
  results"). Implemented on the working tree; not released.
- Scope: the checker (`src/check/references.cee`: `writesStatic`, `staticParameterError`, `resolveFlows`;
  `src/check/interfaces.cee`), the borrow checker's N0245 message (`src/nir/borrow.cee`). Spec: type-system.md
  §25.2, §25.6, §39.3; diagnostics.md (N0273).

## 1. Context

Before this ADR, release 0.11.0 accepted `static` written anywhere in a signature and then ignored it:

- **Ignored where flows are computed.** `static` is never a flow name (§25.6), so `v: StringView<static>` had the
  same flows as `v: StringView`.
- **On a parameter, it promised nothing.** `take(s.view())` with a local `s` compiled for
  `fn take(v: StringView<static>)`. This was sound only because the callee could not use the promise either:
  storing `v` anywhere long-lived was N0272 (and since ADR 0067, also into a static).
- **On a result, it promised nothing either.** `-> StringView<static>` fell into elision (§25.6) and borrowed from
  the scoped parameter. So `fn launder(v: StringView) -> StringView<static> => v` compiled, and its result stayed
  tied to the caller's loan.
- **Without a scoped parameter, it was rejected outright.** `fn table() -> Slice<static, StringView>` was N0245:
  "a function without reference parameters cannot return a reference".

ADR 0067 §4 left two ways out:
- **(a)** check the argument at each call;
- **(b)** reject `static` on parameters.

Option (a) has to hold on every path to the callee. A function value has its regions erased (§25.5), and so do a
`dyn` method and a generic bound. None of those calls sees the parameter's `static`, so (a) would need a check on
each, or a stored view would dangle again.

## 2. Decision

- **Parameters: N0273.** `static` written in a parameter's type is N0273. That covers `StringView<static>`,
  `Slice<static, T>`, `S<static>`, `borrow(static) T`, and an optional or array of one. It applies to functions,
  methods, generic functions and interface methods. Without `static`, a string literal still passes.
- **Results: enforced.** A result whose written regions are all `static` has no flow names and is not elided. It
  borrows from no parameter, which has two sides:
  - **In the body.** Returning a parameter's loan is N0245, which now adds "a 'static' result may hold only static
    data". Returning a local's loan is N0244.
  - **At a call.** The result carries no loan. It outlives the arguments, and it may be stored in a static (ADR
    0067).
- **Mixed results are unchanged.** In `borrow(static, a) T` or `Pair<static, a>`, the other regions give the flows,
  as before.
- **`borrow(static) T` results** already had no sources, through their source list. Only the `StringView<static>`,
  `Slice<static, T>` and `S<static>` forms change.
- **Codes:** none new. The parameter case is N0273, the region-misuse code, and the result cases are N0244 and
  N0245, the return-source codes.

## 3. Evidence (macOS arm64)

- **New corpus tests:**
  - `check-fail/static-parameters`: seven N0273 cases (functions, a slice, a reference, an optional, a generic, a
    method, an interface method), with a plain parameter still taking a literal.
  - `check-fail/static-results`: `launder` and a slice version are N0245, and a local's view is N0244.
  - `run-pass/static-results`: a `static` result kept past its argument's scope, stored in a `static mut`, and a
    parameterless `Slice<static, StringView>` result. Expected output comes from Python. It passes compiled and
    with `--interp`.
- **On the previous build (0.11.0 plus ADRs 0066–0067), all three fail:**
  - `find(owned.view())`'s result was tied to `owned` (N0243);
  - `table()` was N0245;
  - the parameters compiled.
- **Expectations that changed, each noted in place:**
  - `tests/check/check-pass/arrays-and-slices.cee` (`staticView` wrote `static` on its parameters; it now writes it
    on its result);
  - `tests/check/check-fail/arrays-and-slices.cee` (lines 13 and 16 also give N0273);
  - `check-fail/static-mut-loans` (its `StringView<static>` case moved here).
- **Code that still builds:** the compiler's own sources, lib/core, lib/std and ClayWind's six packages.
  CosmoStack was being edited during the check, and its two errors (an unknown field and an unknown variant) were
  the same on 0.11.0. The full corpus, the fixed point and the contract suite are recorded in ROADMAP §10.

## 4. Consequences

- **A program that wrote `static` on a parameter** gets N0273 and drops the word. Its callers are unaffected, since
  the word was never checked.
- **A function can now return static data from no parameter.** `fn table() -> Slice<static, StringView>` and
  `-> StringView<static>` helpers are allowed, and their results can go into a `static mut`.
- **Not covered:** `static` in a function type's parameters (`fn(StringView<static>)`) and in closure literals'
  parameters is not rejected. Region erasure makes it as meaningless there, and nothing relies on it.
