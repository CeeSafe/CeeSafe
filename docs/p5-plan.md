# P5 plan: the flat C backend and the native build

Status: **complete** (2026-10-30; pinned to the reference compiler 0.6.0): the P5 gate passed (`ROADMAP.md` §P5).

P5 ports what runs after the safety passes when the reference compiler builds (reference `v0.6.0`):

1. `CBackend.Generate` (`Backend.C/CBackend.cs`, 308 lines): N0300 for a missing `main`, the emitter, the exports
   header, and the runtime files the program needs (`rt.h`, the core, hosted, I/O, float and thread runtimes).
2. `CEmitter` (`Backend.C/CEmitter.cs`, 3,944 lines) in its flat form: one label per NIR block, no structuring.
3. The toolchain (`Toolchain`, `CCompilers.cs`, 191 lines): the strict warning set, the semantic flags, `-O2` or
   `-g -O0`, objects joined with `-r`, and the build command of `Reference.Cli/Program.cs`.

Not P5 (P7's): the structured form (`CStructurer.cs`), placement (`PlacementAnalysis`), bounds- and overflow-check
elimination (`CheckElimination`) and closure-call specialisation. `ceesafec` accepts the switches that turn them off
(`--flat-c`, `--no-placement`, `--no-bce`, `--no-oce`, `--no-closure-specialisation`) and always behaves as if they
were given. `--split` and `-g`/`--line-directives` are in P5 (the C matrix carried from P0, `docs/parity.md` §3).

## How it is compared

- **C:** `ceesafec build --emit c -o DIR --flat-c --no-placement --no-bce --no-oce --no-closure-specialisation` against
  `ceesafec build --emit c -o DIR`: every file of the output directory byte-identical, except the generated-by
  banner's compiler name and version (`tools/compare_c.py`). Over every program the reference compiler builds, with the corpus's
  options.
- **Behaviour:** `tools/run_corpus.py --compiler ceesafec` over run-pass, run-fail, ffi and freestanding, under the C
  matrix (clang `-O0`/`-O2`, the overflow fallback, ASan+UBSan, GCC on the Ryzen box).
- **Fault sweep:** `FaultInjectionTests.cs` ported: every run-pass program that allocates, under ASan+UBSan, failing
  each allocation in turn; ceesafec's outcomes must equal the reference compiler's built with the same switches.

## Slices

| Slice | Content | Gate |
|---|---|---|
| **S0** | `src/backend/`: names, C types and mangling, the program skeleton, structs and enums, functions in flat form, statements, rvalues and terminators for the S0-level programs; `tools/gen_runtime.py` (the runtime embedded as `src/library/runtime.cee`); `build --emit c`; `tools/compare_c.py` | C identical for the programs whose types S0 emits |
| **S1** | The rest of CEmitter: drop glue, boxes and allocators, vectors, strings, containers, I/O, arenas, pools, `dyn`, closures, threads and forks, statics, FFI, imported structs, exports, callbacks, the exports header, `#line` | C identical over every program the reference compiler builds |
| **S2** | `--emit exe` and `--emit obj`: the toolchain, temporary directories, `Process.capture`; `-g`, `--cc`, `--cflag`, `--ld`; `--split` | run-pass, run-fail, ffi and freestanding pass with ceesafec |
| **S3** | The C matrix, the fault sweep, objects for both CosmoOS architectures, CeeSafe built by ceesafec's C | the P5 gate (`ROADMAP.md` §P5) |

## Execution record

| Date | Slice | Work | Result |
|---|---|---|---|
| 2026-10-30 | S0, S1 | `src/backend/` (the whole flat CEmitter: `c_names`, `c_emitter`, `c_program`, `c_skeleton`, `c_interop`, `c_helpers`, `c_dyn`, `c_body`, `c_backend`), `tools/gen_runtime.py` (`src/library/runtime.cee` from `v0.6.0`), `build --emit c [-o DIR] [--exports-header NAME]`, `tools/compare_c.py`. P3 gaps closed: built-in items' `Used` marks (`src/check/program_tables.cee`), HirProgram's Structs and DynSignatures computed where the reference does, and types made in library code not marked the program's (`Checker.setFile`, TypeTable.InHostedLibrary). Layouts are computed once before emitting, so the emitter reads the checker immutably | **C identical for all 284 programs the reference compiler builds** (every corpus family: run-pass, run-fail, emit-c, ffi, freestanding, placement, bounds-, overflow- and closure-check families), banner aside; CeeSafe's own C identical (394,094 lines; ceesafec 1.4 s, 143 MB, the reference compiler 4.1 s); that C, compiled by cc -O2, gives a ceesafec whose output is identical. Unchanged: check 318/322, other families 84/84, NIR 284/284, packages 7/7, self-check clean |
| 2026-10-30 | S2 | `src/host/toolchain.cee` (StrictWarnings, SemanticFlags, ProfileFlags; CompileExecutable and CompileObject in the GNU dialect, through `Process.capture`), `src/host/build.cee` (Program.Build: default outputs, a temporary directory removed after, the compiler's output relayed, the reference compiler's messages), `--emit exe` (the default) and `--emit obj`, `--cc` (else `$CC`, else `cc`), `--cflag`, `--ld`, `-g`, `--line-directives`, `--split` and `--program` (`src/backend/c_split.cee`: NameUnits, UnitOf, SplitFiles), `.c` inputs and the manifest's `kind` and `c` (type-system.md §28.6); `tools/freestanding_check.py` (FreestandingTests); `run_corpus.py --build-option`. NIR fix: an exhaustive match's trailing Unreachable takes the current span, as the reference's Terminate gives it (P4's NIR dump does not show spans; `-g` did) | C identical in all four forms (flat, `--split`, `-g`, `-g --split`): 284/284 each. run-pass 159/159, run-fail 40/40, ffi 8/8 under clang `-O2`, `-O0`, `-DNV_NO_BUILTIN_OVERFLOW`, `-O1` ASan+UBSan with leak checks, `--split` and `-g`; TSan on the 3 thread programs; freestanding 3/3 for host (objects run under their harnesses) and both CosmoOS triples with ld.lld, the reference compiler likewise. emit-c snapshots: 7 of 24 match; the rest are the reference compiler's structured, optimised form (P7), whose flat form is identical (compare_c) |
| 2026-10-30 | S3 | `tools/fault_injection.py` (FaultInjectionTests; with `--reference`, each run equals the run of the reference compiler's build), `tools/collection_properties.py` (CollectionPropertyTests' generators over the reproduced System.Random, their models, the configurations and the sweep), the C matrix with `-Werror`, and the Ryzen box (Debian 12): ceesafec built by GCC 12 from its own C (`-Werror`, the strict set), the matrix under GCC 12 and clang 18, the sweep; `run_corpus.py`'s emit-c family takes `--build-option` | fault sweep: 50 allocating programs, 789 failure points, 0 failures, equal to the reference compiler's runs (macOS and Linux). Collections: 30/30 match their models under `-O2`, `-O0`, the overflow fallback and ASan+UBSan; C equal to the reference compiler's; 156 swept points. Matrix: all pass but GCC 12 `-O2 -Werror` on `nested-patterns`, which rejects the reference compiler's C, flat and default, too. ceesafec built by GCC emits CeeSafe's C identical to the Mac's. emit-c: the reference compiler in the same form fails the same 17 snapshots. **P5 gate passed** |
