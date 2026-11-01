# ADR 0020 — OD-9c: functions whose effects are their closure arguments'

> **Inherited.** ADR 0020 was accepted before CeeSafe's fork (Decision 0006), when the language had its
> earlier name. In this record, "the reference" is that language and project and "the reference compiler" its compiler:
> read them as CeeSafe and `ceesafec`. Links to documents CeeSafe did not keep are given as text.

- Status: **Accepted** (project owner, 2026-10-29, taking every recommendation in §5). The wording is the agent's.
- Scope: OD-9c (`open-decisions.md`), deferred by M11 G8c (type-system §36.4): a function whose effects are its own plus
  those of a closure argument, so that `sortBy` with an `@effects()` comparator may run in an atomic context.

## 1. Context

- **Effects today** (type-system §32, §36.3). Each function's effects (`sleep`, `alloc`) are inferred from its body. A
  call through a closure or function value has its *type's* effect list, and a type without one has both effects.
  `@context(atomic)` forbids `sleep`, and `@context(irq)` forbids both.
- **The gap.** lib/core's closure-taking methods (`sortBy`, `forEach`, `any`, `all`, `retain`; §36.4) take
  `borrow mut fn(…)` with no list, so each of them has both effects, whatever closure it is given. None of them can be
  used in atomic or interrupt code, even with a comparator that does nothing but compare. Giving the parameter a list
  (`borrow mut @effects() fn(…)`) would forbid every comparator that allocates, everywhere. What is wanted is Swift's
  `rethrows`: the method has the effects of what it is given.
- **Who needs it.** CosmoOS kernel code holds spinlocks (atomic context) and runs interrupt handlers. Sorting or
  scanning a small table under a lock, with `sortBy` or `any`, is today impossible in safe the reference. It must be written out by
  hand (inferred: no CosmoOS port does this yet; `kernel/block/gpt.cee` and the A5 ports have no closures).
- **What compiles the same either way.** Nothing in C or the interpreter changes. Effects are only checked.

## 2. Options

| | A. `@effectsOf(p)` on a function (proposed) | B. Effect variables (`fn sortBy<effects E>(…, cmp: … @effects(E) fn …) @effects(E)`) | C. Check effects after closure specialisation (ADR 0019) | D. Nothing |
|---|---|---|---|---|
| Model | Swift's `rethrows`, for two effects | Koka's effect polymorphism | the variant `sortBy` for a known comparator has that comparator's effects | — |
| Syntax | one attribute naming parameters | effect parameters, bounds and arguments | none | none |
| Inference at calls | the argument's own effects (a literal's body, a named function's inference) | unification of effect variables | precise, per variant | — |
| Soundness argument | short (§3.3) | the general one | ties acceptance to an optimisation (`--no-closure-specialisation` would change which programs compile) | — |
| Cost | the checker, one attribute | a new kind of generic parameter | none in the checker, but wrong in principle | the gap stays |

C is ruled out because a program's acceptance must not depend on an optimisation. B can say more, for example a
function whose effects are the union of two closures stored in a struct. But nothing needs that, and it is a new kind
of generics. A covers the library methods and any user function of the same shape.

## 3. Proposed decision: A

### 3.1 The attribute

```ceesafe
@effectsOf(cmp)
public fn sortBy(s: SliceMut<T>, cmp: borrow mut fn(borrow T, borrow T) -> i32) { … }

@context(atomic)
fn sortUnderLock(lock: *mut spinlock_t, table: SliceMut<i32>) {
    unsafe { spin_lock(lock); }
    table.sortBy((a, b) => a - b);   // allowed: sortBy's own effects are none, and the comparator's are none
    unsafe { spin_unlock(lock); }
}
```

- `@effectsOf(p, …)` on a function or method names one or more of its parameters. Each must be a function or closure
  type: `fn(…)`, `borrow fn(…)` or `borrow mut fn(…)`, with or without an effect list. Otherwise it is a new error code
  (N0336). It may be combined with `@effects(…)`, which then bounds the function's *own* effects (§3.2).
- It changes no type. The function's type and value are as before (§3.4).

### 3.2 Inside the function

- A call whose callee is the parameter `p` itself is not one of the function's own effects: the caller accounts for it.
- Every other use is checked as today:
  - a call through a copy or a reborrow of `p` (`let c = p; c(x)`) has its type's list;
  - `p` passed to a function that is not `@effectsOf` for it: that function's inferred effects, which include
    calls through its parameter by type;
  - `p` stored in a field or captured by a closure: the type's list wherever it is called.
- `p` passed directly as the argument for another function's `@effectsOf` parameter contributes nothing either:
  `sortBy` may hand `cmp` to an `@effectsOf` helper.
- The function's own effects are inferred from everything else. They are bounded by `@effects(…)` if given, and checked
  against `@context` as today.

### 3.3 At a call

The effects of a call to an `@effectsOf` function are its own effects, plus, for each named parameter, the effects of
the argument:

| Argument | Its effects |
|---|---|
| a closure literal | its body's inferred effects |
| a named function (adapted as in §36.1–§36.2) | that function's inferred effects |
| the caller's own `@effectsOf` parameter, passed on directly | none (the caller's caller accounts for it) |
| anything else | its type's effect list (both effects without one) |

A diagnostic for such a call names the argument as the witness, for example "calls sortBy with a closure that does
console I/O (line 12)", following the chain as N0295 does today.

**Why it is sound.** A parameter is immutable (N0211), and closures and function values cannot be `var` (N0246). So
during the call, every call through `p` itself runs exactly the argument the caller passed, and that argument's effects
are counted at the call. Calls through anything else made from `p` are counted by their types, as before. Effects are a
property of a call's dynamic extent: a closure stored by the function and called after it returns is counted where it
is called, by its type.

### 3.4 As a value

Naming an `@effectsOf` function as a value (`let f: @effects() fn(…) = sortBy`) gives it no special treatment. Its
effects as a value are its own effects plus each named parameter's *type* list. The check of §36.3 then applies.

### 3.5 lib/core

- `sortBy`, `forEach`, `any` and `all` become `@effectsOf` their closure parameter, and so does `retain`. `retain` still
  has the effects of destroying a `T` (its own, inferred per instantiation).
- The spec's sentence "these methods cannot be used in `@context(atomic)` … code" (§36.4) changes accordingly.

## 4. Consequences

- **Kernel code** can use the library's algorithms under a lock with a comparator that neither sleeps nor allocates.
- **One new attribute** and one diagnostic code (N0336 for a misplaced or misnamed parameter). N0295 gains a witness
  kind (the argument of a call).
- **Nothing changes** for code that does not use the attribute, or in C, the interpreter, or the fuzzer's programs.
- **Tests (planned):**
  - check-pass: `sortBy`/`any` under `@context(atomic)` with literal and named comparators, and an `@effectsOf` helper
    that passes its parameter on;
  - check-fail: a sleeping comparator in atomic code (N0295 naming the argument), a copy of `p` called inside (counted
    by type), a misnamed parameter or one that is not a closure (N0336), and `@effectsOf` with an `@effects()` bound
    violated by the function's own body.
- **Not decided here:** effect variables (B), which stay open in OD-9c if a need appears.

## 5. Questions for the owner

1. Option A, `@effectsOf(p)`, as above? (Recommended.)
2. The name: `@effectsOf(p)` (proposed), or `@effects(…, of p)` inside the existing attribute?
3. lib/core's five methods change in the same step (§3.5)? (Recommended.)

## 6. Implementation (2026-10-29)

The decision is in type-system §32.5, with §36.2 and §36.4 changed to match. Three points refine §3 without changing
what it decides. Each follows from the same argument (§3.3: a parameter is never assigned):

- **Copies of `p`.** §3.2 counted a copy of `p` by its type. A local that is assigned nothing but copies, casts or
  reborrows of `p` holds the same argument, so a call through it is exempt as well. Anything else still counts by type.
- **Arguments.** Besides a literal written in the call, a local *known* to hold a literal counts as that literal. That
  is closure specialisation's analysis (optimisations.md §5.1), which the effect checker runs whatever the
  optimisation switches are, so which programs compile does not depend on them. So does a local holding the caller's
  own `@effectsOf` parameter.
- **Named functions.** A named function adapted to a closure (§36.2) used to be held in the adapter's environment and
  called through. Its effects were then the closure type's, both effects without a list, whatever the function did.
  The adapter now calls the function directly. Its effects are the function's own, and the C has one indirect call
  fewer.

The diagnostic code is N0336: N0335 was already `ceesafec publish`'s. `EffectChecker.CallEffects` computes a call's
effects, and `OriginsOf` finds what each local holds. Tests: `check-pass/effects-of.cee`, `check-fail/effects-of.cee`
(N0295 for a sleeping literal, a sleeping named function, an argument without an effect list, a parameter handed to a
function without the attribute, and the function as a value), `check-fail/effects-of-attribute.cee` (N0336), and
`check-fail/closure-method-effects.cee` (lib/core's methods with a comparator that does I/O).
