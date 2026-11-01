# P7 plan: optimisations and production C

Status: **complete** (2026-10-30; pinned to the reference compiler 0.6.0). The C-prefix switch moved to P8 (Decision 0004).

P7 ports what the reference compiler does by default and P5 left out (reference `v0.6.0`):

| Part | Reference | Switch | Report |
|---|---|---|---|
| Closure-call specialisation (optimisations.md §5, ADR 0019) | `Nir/ClosureSpecialisation.cs` (314 lines) | `--no-closure-specialisation` | `--explain-closure-calls` |
| Placement: boxes on the stack (memory-model.md §5, optimisations.md §4) | `Analysis/PlacementAnalysis.cs` (360) | `--no-placement` | `--explain-allocations` |
| Bounds- and overflow-check elimination (optimisations.md §2–§3) | `Analysis/CheckElimination.cs` (1,088) | `--no-bce`, `--no-oce` | `--explain-bounds-checks`, `--explain-overflow-checks` |
| Structured C (c-backend.md §3) | `Backend.C/CStructurer.cs` (636) | `--flat-c` | — |
| The emitter's hooks for all four | `Backend.C/CEmitter.cs` | | |

Each part lands default-on, as in the reference compiler, once its C is identical to the reference compiler's. The comparison tools pass every switch
explicitly, so a part not yet ported never changes what they compare.

## How it is compared

- **C:** `tools/compare_c.py` over every program the reference compiler builds, with each part on in turn and with all on (the reference compiler's
  default), in the flat and split forms and with `-g`: byte-identical but the banner.
- **Reports:** the four `--explain-*` reports byte-identical to the reference compiler's over the corpus; the optimisation families
  (`placement`, `bounds-checks`, `overflow-checks`, `closure-calls`) pass the runner's snapshot checks; `emit-c`
  snapshots all match.
- **Behaviour:** the run corpus, fault sweep and collection properties in the default (all-on) form, under the C matrix.
- **Fixed point** with production defaults and with `--split`.

## Slices

| Slice | Content | Gate |
|---|---|---|
| **S0** | Closure-call specialisation: NIR variants (`NirFunction.variant`, a call's variant), `ceeXS<n>_` names, the library reach by (function, variant), `--explain-closure-calls`; default on | C identical with it on; `closure-calls` family; reports identical |
| **S1** | Placement: the analysis, stack slots, placed boxes' drops and frees, `--explain-allocations`; default on | C identical with it on; `placement` family; reports identical |
| **S2** | Check elimination: the dataflow, removed index and range checks, unchecked arithmetic, the two reports; default on | C identical with each on; the two families; reports identical |
| **S3** | CStructurer: the statement tree, `if`/loops/`switch`, break/continue, labels, `#line` with it; default on | C identical in the reference compiler's default form (all on), flat and split, with and without `-g`; `emit-c` 24/24 |
| **S4** | Production defaults everywhere: the run corpus, sweep and properties in the default form, the C matrix; the fixed point with production defaults and `--split` | the P7 gate (`ROADMAP.md` §P7) |

The C-prefix and ABI switch (ROADMAP P7's third item) was proposed at S4 as Decision 0004; the owner accepted its
option 2 (2026-10-30): the switch is made at P8's fork point, so P7's C stays byte-identical to the reference compiler's.

## Execution record

| Date | Slice | Work | Result |
|---|---|---|---|
| 2026-10-30 | S0 | `src/nir/closure_specialisation.cee` (Rewrite, VariantOf, KnownClosures, the report); a call's variant (`NirProgram.rvalueVariants`) and a function's (`NirFunction.variant`, `bindings`); the emitter's `ceeXS<n>_` names and the library reach by (function, variant); `CBuildOptions`; `--no-closure-specialisation`, `--explain-closure-calls` (and `--dump-nir` now prints the NIR the C was made from); `tools/compare_reports.py`, `tools/compare_self.py`, `compare_c.py`/`compare_nir.py --on`; `tests/p7/closures.cee` | with specialisation on: C 284/284, NIR 284/284, reports 284/284 (8 non-empty) over the corpus; the 24 benchmark and program sources C and reports identical; `tests/p7/closures.cee` (5 functions' variants, mutual recursion, generic callees, 4 lib/core methods, unknown closures) C in three forms, NIR and report identical; CeeSafe's own C identical (no closure calls); run-pass, run-fail, ffi, closure-calls pass with it on by default |
| 2026-10-30 | S1 | `src/nir/control_flow.cee` (the reference's ControlFlow: reverse postorder, dominators, natural loops, reducibility), `src/backend/placement.cee` (PlacementAnalysis: the flow set, escapes, the loop rule, error paths, the stack limits; the report), the emitter's slots, placed sites and placed boxes' drops and frees; `--no-placement`, `--explain-allocations` | with placement on: C 308/308 over the corpus, benchmarks and programs, with and without specialisation, `-g` and `--split`; reports 308/308 (94 non-empty); `placement` family 3/3; CeeSafe's own C identical with both on (417,937 lines), its 1,318-line report identical; run-pass, run-fail, ffi pass under ASan+UBSan with leak checks with both on by default |
| 2026-10-30 | S2 | `src/backend/check_elimination.cee` (CheckElimination: terms and facts, the forward dataflow with the reference's budget and smallest-block-first order, Join with implications, Kill's rewriting through equal terms, the closure's equality classes and saturation rules, Upper/Lower over 128-bit bounds, exact products by BigInt, the ASCII boundaries, both listings); the emitter's removed index checks, `cee_check_range` for ASCII text and unchecked `((T)(a op b))`/`((T)(-x))`; `--no-bce`, `--no-oce`, `--explain-bounds-checks`, `--explain-overflow-checks` | with BCE on, OCE on, and everything but structuring on: C 308/308 over the corpus, benchmarks and programs, also with `-g` and `--split`; bounds-check reports 308/308 (75 non-empty), overflow-check reports 308/308 (229 non-empty); CeeSafe's own C identical (429,983 lines; 492,067 with `--split -g`), its reports identical (3,658 and 2,099 lines), also from a ceesafec built with the checks eliminated; ceesafec emits its own C in 2.7 s, 213 MB; families bounds-checks 3/3, overflow-checks 2/2, placement 3/3, closure-calls 1/1; run-pass 159, run-fail 40, ffi 8 under ASan+UBSan with leak checks, all on by default; emit-c 7/24 (waits for S3's structured C) |
| 2026-10-30 | S3 | `src/backend/c_structurer.cee` (CStructurer over the existing ControlFlow: PlacementOf with the inline-exit limits, the tree in node and list arenas, Branch, transfer elimination to a fixpoint, labels, guard-clause hoisting, Negate, the printer with `else if` chains, switches and `-g` labels); the emitter captures statement and exit lines for the tree; `--flat-c` | in the reference compiler's default form (everything on): C 308/308 over the corpus, benchmarks and programs, flat, `-g`, `--split` and `-g --split`; structured alone, and with `-g`, 308/308; CeeSafe's own C identical (404,458 lines; 469,474 with `--split -g`) also from a ceesafec built in the default form, its allocation report identical; families emit-c 24/24, bounds-checks 3/3, overflow-checks 2/2, placement 3/3, closure-calls 1/1; run-pass 159, run-fail 40, ffi 8 under ASan+UBSan with leak checks; ceesafec emits its own C in 3.1 s, 234 MB |
| 2026-10-30 | S4 | production defaults everywhere: `tools/contract_tests.py` adds the default form (C in four shapes over the corpus, benchmarks and programs; the four reports; CeeSafe's own C flat and `--split -g`; the optimisation and emit-c families); the fault sweep and collection properties build in the default form (`--flat` for P5's); `tools/fixed_point.py --extra`; Decision 0004 proposed and accepted (prefix at P8) | **P7 gate passed.** macOS (Apple clang 21): every contract step ok (C 308/308 ×4, reports 308/308 ×4, self identical, run-pass 159, run-fail 40, ffi 8 under `-Werror` and ASan+UBSan with leak checks, emit-c 24/24 and the optimisation families, mutations, fault sweep 783 points, collections 30/30 with 156 points, freestanding). Linux (GCC 12, clang 18): the same (the corpus checkout there has no benchmarks: C and reports 284/284), clang 18 `-O2 -Werror` and ASan+UBSan 207/207, GCC 12 207/207 but the recorded `nested-patterns`. Fixed point in the default form and with `--split` on both hosts (`bootstrap/fixed-point/2026-10-30-p7*.md`), the same C on both |
