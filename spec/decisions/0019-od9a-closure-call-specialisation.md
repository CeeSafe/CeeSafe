# ADR 0019 — OD-9a: direct closure calls by specialisation

> **Inherited.** ADR 0019 was accepted before CeeSafe's fork (Decision 0006), when the language had its
> earlier name. In this record, "the reference" is that language and project and "the reference compiler" its compiler:
> read them as CeeSafe and `ceesafec`. Links to documents CeeSafe did not keep are given as text.

- Status: **Accepted** (project owner, 2026-10-28, taking every recommendation in §5). The wording is the agent's. The
  decision is in `optimisations.md` §5, `intermediate-representation.md` §3 and `c-backend.md` §2.1. A remains open
  (OD-9a), as a language question.
- Scope: OD-9a (`open-decisions.md`), deferred by ADR 0012 §5 question 5 "until a performance need appears, with a
  benchmark": monomorphised closures, so a call through a closure can be a direct call the C compiler inlines.

## 1. Context

- **How a closure call compiles today.** A closure is a pointer to an environment whose first member is its code
  (type-system §36.2). A call through one is `(*c)(c, a, b)` in C (c-backend §2). A library method such as `sortBy`
  takes `borrow mut fn(borrow T, borrow T) -> i32`, one type for every comparator of that signature, so its body cannot
  know which function it calls. The C compiler does not specialise lib/core's introsort for the one comparator either.
- **The need, measured** (2026-10-28, the Ryzen box: Ryzen 5 PRO 3600, Debian 12; `benchmarks/sort`, 10 rounds of
  sorting 1,000,000 values; sort time with the data generation subtracted; median of 7 runs, interleaved; load average
  1.0–2.1 on the shared box). "Direct" is the reference compiler's C for the benchmark with the seven calls through the comparator
  replaced by direct calls to its function, `ceeCB_92(t, a, b)`, by hand: what a specialisation would produce. Every
  program printed the same checksum.

  | Compiler | The reference today | The reference, direct calls | C `qsort` | the same introsort in C | the same, comparator opaque |
  |---|---|---|---|---|---|
  | clang 18.1.8 -O2 | 1.062 s | **0.671 s** | 1.171 s | 0.617 s | 0.907 s |
  | gcc 12.2.0 -O2 | 1.076 s | **0.797 s** | 1.195 s | 0.659 s | 0.941 s |

  Direct calls take 37% (clang) and 26% (gcc) off the sort, bringing the reference from 1.72× the C introsort to 1.09× under
  clang. The rest is the reference's checks (M11 G8c found the same split).
- **Where the closures come from.** Every call of lib/core's closure-taking methods (`sortBy`, `retain`, `forEach`,
  `any`, `all`) in the corpus, the benchmarks and the guide passes a closure literal written at the call, or a named
  function adapted to a closure (§36.2: `sortBy(compare)` in `closure-methods.cee`), and the methods pass it on
  unchanged to their helpers (checked 2026-10-28 by searching for the calls). So at those calls the code that will
  run is known. User functions with closure parameters were not surveyed.

## 2. Options

| | A. Closure types and `Fn` bounds (OD-9a as written) | B. Specialisation in the compiler (proposed) | C. Nothing |
|---|---|---|---|
| Model | Rust's: each literal its own type; `fn sortBy<F: Fn(borrow T, borrow T) -> i32>(…, cmp: borrow mut F)` | the call graph: a callee cloned per closure code it is called with | — |
| Language change | yes: closure types, an `Fn` bound family, inference of `F`, its effects and loans in bounds; lib/core's signatures change | **none**: `sortBy` and every user function keep their signatures | none |
| Which calls become direct | only through functions written with an `F` bound | every call through a parameter whose closure is known at the call, in any function | none |
| Cost | a large language feature | a NIR-to-NIR pass, like M9's; code size: one clone per (function, closure) pair | the measured 26–37% stays |

**B is proposed.** It gives the measured gain to code as it is written, lib/core's included, without adding to the
language. A remains open as a language question (expressiveness: owning closures in generic code without a box),
not as a performance one.

## 3. Proposed decision

1. **Closure-call specialisation**, an optimisation pass on NIR before C (`optimisations.md` §5), on by default with
   `--no-closure-specialisation`, as M9's are. It changes no program's behaviour, only which C function is called.
2. **What is known.** At a call `f(…, c, …)`, an argument `c` is *known* when it is a closure made in the caller from a
   literal, or an `fn` value's adapter, or a known parameter of a function being specialised, or a reborrow or copy
   of a known value. Its *code* is that literal's or adapter's function.
3. **What is cloned.** When `f`'s parameter `p` has a closure type and the argument is known with code `k`, the call
   goes to `f[p := k]`: a copy of `f` in which every call through `p` (or a reborrow or copy of `p`) is a direct call
   to `k`, and every call that passes `p` on is specialised the same way. Clones are made once per (function,
   parameter, code), so recursion (`introsort` calling itself) calls its own clone. A function with several closure
   parameters is specialised for each known one.
4. **What is not.** A closure stored in a field, a `dyn fn` (§37), a `Box<dyn fn…>`, or one whose code depends on a
   branch is called as today. Generic functions are specialised after monomorphisation, as one more instance.
5. **Equivalence.** The clone is the same NIR with a call target fixed, so its checks, effects and loans are the
   original's; borrow and effect checking run before the pass and are not repeated. The interpreter runs the program
   unspecialised, and the corpus already compares it with native runs. The pass is checked by: the run-pass corpus with
   and without it (outputs equal), the fuzzer's configurations (one more: `--no-closure-specialisation`), a test that
   counts direct and indirect closure calls in the C of `benchmarks/sort`, and `--explain-closure-calls`, which lists
   each call through a closure and whether it became direct.
6. **Kernel and freestanding profiles** get it too: it adds no allocation and changes no frame size (a clone's frame
   is the original's).

## 4. Consequences

- `sortBy`, `retain`, the closure-taking slice methods (§36.4) and user functions with closure parameters call
  their closures directly, so the C compiler can inline them. No source changes.
- Code size grows by one function per (function, closure) pair actually reached; `--explain-closure-calls` reports
  the clones.
- A remains open, as a language question.
- **Not in this decision:** closure types or `Fn` bounds (A); specialising `dyn` calls (devirtualisation); inlining
  in the reference compiler itself (the C compiler does it once the call is direct).
- Benchmarks: `benchmarks/sort` is measured again on the Ryzen box after the pass, with clang and gcc, before any
  claim, and `benchmarks/suite`'s closure workload (`nbody`) with it.

## 5. Questions for the owner

1. **Specialisation in the compiler (B)**, rather than closure types and `Fn` bounds (A) now? Recommended: B, with
   A left open as a language question.
2. **On by default**, with `--no-closure-specialisation` and `--explain-closure-calls`? Recommended: yes, as for M9's
   bounds-check and overflow-check elimination.
3. **No limit on clones** in v1, the count reported by `--explain-closure-calls`? Recommended: yes; the count is
   bounded by the closure literals a program writes times the functions they reach.

## 6. Implementation plan (once accepted)

Spec first (`optimisations.md` §5, `intermediate-representation.md`, `c-backend.md`), then:

- the pass (`Optimisation/ClosureSpecialisation.cs`): known closures, the clone worklist, the switch and the
  explanation;
- tests: the corpus with and without it, a C-shape test on `benchmarks/sort` (no indirect comparator calls remain),
  explanation snapshots, the fuzzer configuration;
- `benchmarks/sort` and `benchmarks/suite` re-measured on the Ryzen box.

## 7. Implementation and results (2026-10-28)

- Spec: `optimisations.md` §5, `intermediate-representation.md` §3, `c-backend.md` §2.1 (`5bae3d6`).
- Code (`9ec6aa1`): `Nir/ClosureSpecialisation.cs`; `NirFunction.Variant` and `NirCall.Variant`; the C names
  `ceeXS<n>_<name>`; the emitter's reachability now follows variants, so a library function that only its variants
  replace is not emitted; the interpreter runs the specialised NIR; `--no-closure-specialisation` and
  `--explain-closure-calls`.
- In `benchmarks/sort` every one of the seven calls through the comparator is direct, through six variants of lib/core's
  introsort (`sortBy`, `introsort`, `insertion`, `heapsort`, `partition`, `siftDown`).
- Found on the way: `ceesafec run` gave the interpreter the C backend's NIR, so with the pass on it was specialised twice,
  which replaced originals by variants (an interpreter fault on the first variant call). The pass now leaves a
  specialised program unchanged, and `run` passes its switch to the backend.
- Tests, run 2026-10-28: the full suite passes (3,431), with the run corpus built in one more C configuration
  (`clang -O2 no-closure-specialisation`); `ClosureSpecialisationTests` (the `--explain-closure-calls` snapshot of
  `closure-calls/known.cee`, whose closure chosen by a branch stays indirect; its output with and without the pass;
  no indirect comparator call in `benchmarks/sort`'s C, seven without the pass); the fuzzer's seeds 1–300 give no
  finding in six configurations and the interpreter; `c2.py` runs the corpus in CosmoOS on x86-64 and AArch64 (188 programs each; the fault sweep 43 programs and 381 runs each).
- Measured on the Ryzen box (clang 18.1.8 and gcc 12.2.0, -O2, 7 runs, the same session; load average 1.3–2.8 on the
  shared box, normal priority). `benchmarks/sort`, sort time:

  | Compiler | The reference, pass on | The reference, `--no-closure-specialisation` | the same introsort in C | C `qsort` |
  |---|---|---|---|---|
  | clang | **0.691 s** | 1.048 s | 0.610 s | 1.162 s |
  | gcc | **0.791 s** | 1.100 s | 0.657 s | 1.188 s |

  The pass takes 34% (clang) and 28% (gcc) off the sort, which is what §1's hand edit showed (0.671 s and 0.797 s).
  The reference goes from 1.72× and 1.68× the C introsort to 1.13× and 1.20×.
- `benchmarks/suite` with the pass on and off: `nbody`, `parsing` and `data-oriented` move within ±8% in both
  directions, which is noise on that box. None of them calls through a closure: `nbody` has no closure literal any
  more, so §4's reference to it as the suite's closure workload was out of date. The sort is the measured case.
