# P4 plan: NIR and the safety passes

Status: **complete** (2026-10-30; pinned to the reference compiler 0.6.0): the P4 gate passed (`ROADMAP.md` §P4, `docs/p4-evidence.md`).

P4 ports what runs after the type checker in `Compilation.CreateOnCompilerStack` (reference `v0.6.0`, `Compilation.cs`):

1. `NirBuilder.Build(hir, types)`: the HIR lowered to NIR, a control-flow graph per function (`Nir/Nir.cs`, 348
   lines; `Nir/NirBuilder.cs`, 1,630).
2. For every function that is not an instantiation, in order: `InPlaceChecker` (143), `MoveChecker` (213, with
   `MovePaths`, 383), `BorrowChecker` (705).
3. If nothing failed: N0222 for each instance whose body did not check (the `InstanceError` P3 records).
4. If nothing failed: `DropElaborator` (201) on every function that is not a generic version, then `EffectChecker`
   (654) over the program.

Not P4: closure-call specialisation, placement and check elimination (`ClosureSpecialisation.cs`,
`PlacementAnalysis.cs`, `CheckElimination.cs`) are P7's; the C backend is P5's.

## How it is compared

- **Diagnostics:** `ceesafec check` against `ceesafec check`, as in P3 (`tools/compare_parse.py --command check`): the
  55 check programs that differ today differ only by these passes' diagnostics (N0212, N0222, N0240–N0245,
  N0250–N0253, N0272, N0294's in-place cases, N0295) or by the reference compiler's three printer crashes.
- **NIR:** `ceesafec build --emit c -o DIR --dump-nir --no-closure-specialisation --no-placement --no-bce --no-oce` prints
  the compilation's NIR after drop elaboration (`NirPrinter`; lib/core's functions left out). `ceesafec build
  --dump-nir` prints the same and stops: the C backend is P5. A new `tools/compare_nir.py` compares them over every
  program the reference compiler builds (run-pass, check-pass, emit-c, ffi, freestanding, the probes), with the corpus's options.
- **Expectations:** `MutationTests.cs` and `FaultInjectionTests.cs` (the M2 mutation suite and the M3 fault sweep)
  are ported as fixtures in the last slice.

The reference's NIR is records with object identity (blocks, locals); CeeSafe keeps arenas with indices, as the HIR
does (`src/hir/hir.cee`). The printer's text is the contract.

## Slices

| Slice | Content | Gate |
|---|---|---|
| **S0** | `src/nir/nir.cee` (the model), `src/nir/nir_printer.cee`, `ceesafec build --dump-nir`, `tools/compare_nir.py`; NirBuilder's skeleton: functions, locals, blocks, statements, operators, calls, constants, returns, `if`/`while`/`loop`/`for`, StorageDead | NIR identical for the S1-level run-pass programs (no drops yet: types without drop) |
| **S1** | MovePaths and DropElaborator (drop flags, partial moves, box frees) | NIR identical for every program whose constructs S0 lowers, drops included |
| **S2** | NirBuilder for the rest of the HIR, in P3's order: structs, enums and `match`, optionals and results (`?`, `!`), references, arrays and slices, boxes, vectors, strings, containers, regions, generics, interfaces, `dyn`, closures, threads, statics, `unsafe` and FFI (split like P3's slices as it goes) | NIR identical over every program the reference compiler builds |
| **S3** | InPlaceChecker and MoveChecker (N0250–N0253, N0294's in-place cases, N0212 if it is theirs) | the move families of check-fail identical |
| **S4** | BorrowChecker (N0240–N0245, N0272; the sticky shared deref, N0248) and N0222 for failed instances | check-fail identical except the reference compiler's printer crashes and `offset-of` |
| **S5** | EffectChecker (N0295) | every check program identical but those three and `offset-of`; `check` is the full pipeline |
| **S6** | The mutation and fault-injection expectations; every invariant of `docs/ownership.md` mapped to positive and negative evidence | the P4 gate (`ROADMAP.md` §P4) |

## Execution record

| Date | Slice | Work | Result |
|---|---|---|---|
| 2026-10-30 | S0, S2 | `src/nir/nir.cee` (arenas), `nir_printer.cee`, `nir_builder.cee` (the whole of NirBuilder, so S2 came with S0), `build --dump-nir`, `tools/compare_nir.py`; `gen_builtins.py` emits `builtinFunctionName` | before drops: 174 of 270 built programs identical, every difference a drop |
| 2026-10-30 | S1 | `src/nir/move_paths.cee` (MovePath, InitState as bitsets, MoveData's FIFO dataflow), `src/nir/drops.cee` (DropElaborator; flags kept in creation order, as the reference's dictionary); HIR's never-set `isGeneric`/`isInstantiation` fields removed for `functionIsGeneric` (statics' constant-function loop too) | `compare_nir.py` over every corpus family: 474 files, 284 built by the reference compiler, **284 identical**; CeeSafe's own NIR (package mode) identical, 429,006 lines, ceesafec 0.30 s, 79 MB. Unchanged: check 266/322 and 84/84, parse 487/487, packages 7/7, `fmt --check` clean |
| 2026-10-30 | S3 | `src/nir/move_check.cee` (InPlaceChecker, MoveChecker), `src/nir/passes.cee` (the pipeline after the checker: NIR, in-place and move checks on functions that are not instantiations, N0222, drops); `check` now runs it | check-pass/check-fail/run-pass **275**/322 (was 266); no N0212, N0222, N0250–N0253 or N0294 line differs in the 47 left (borrow checking, effects, the reference compiler's printer crashes, `offset-of`); other families 84/84; NIR 284/284; packages 7/7 |
| 2026-10-30 | S4 | `src/nir/borrow.cee` (BorrowChecker: loans, points-to, through-writes and N0272, liveness, conflicts N0240–N0243 with notes, returns N0244/N0245; live sets enumerate as .NET's HashSet, free slots reused LIFO, since the reference's report order follows them); diagnostics gained notes (text `= note:` lines and JSON); P3 gap fixed: flow names for type parameters (`t:`, the function's and the receiver struct's), so generic functions' return sources and mutable flows match | check-pass/check-fail/run-pass **308**/322: the 14 left are N0295 (S5: 11), the reference compiler's printer crashes (enums, optionals, results) and `offset-of`; other families 84/84; NIR 284/284; packages 7/7; `ceesafec check` of CeeSafe itself clean, as with the reference compiler (0.88 s, 71 MB), HIR identical |
| 2026-10-30 | S5 | `src/nir/effect_check.cee` (EffectChecker: inference to a fixed point, drop effects per round, KnownClosures and own `@effectsOf` origins, live atomic holders, the three contexts and named-function values; an operation's description kept as parts and spelled out only for a message); P3 gap fixed: `Box.create` now records the `@placed` methods it runs (EnsurePlaced) | check-pass/check-fail/run-pass **318**/322: the 4 left are the recorded divergences (the reference compiler's `check --dump-hir` printer crashes on the check-fail enums, optionals and results, and `offset-of`); other families 84/84; NIR 284/284; `check --dump-hir` identical on all 186 check-pass and run-pass programs; packages 7/7; CeeSafe's self-check clean (0.77 s), HIR identical |
| 2026-10-30 | S6 | `tools/mutation_suite.py` (MutationTests: .NET's seeded `Random` reproduced, checked draw for draw against .NET 10; the oracle ported), `docs/p4-evidence.md` (every ownership.md invariant mapped to must-fail and near-miss programs, codes checked against the files); the fault-injection sweep moved to P5's gate, since it runs compiled programs | mutations: 1,152 mutants over 5 seeds (120 resolving per seed, 54–83 rejected), ceesafec and the reference compiler both match the oracle on every one; P4 gate passed |
