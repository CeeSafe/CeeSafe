# Testing

Status: describes CeeSafe's tests. Tests that were run are recorded, with dates and results, in the repository's
`ROADMAP.md` §10 and the plans under `docs/`; they are never mixed with proposals.

Sanitizer results are supporting evidence, never proof of the language's safety.

## 1. Test kinds

| Kind | Where | Oracle |
|---|---|---|
| Corpus (§2) | `tests/reference/tests/corpus/*`, `tools/run_corpus.py` | the files' own annotations and snapshots |
| Support library | `tests/support`, `tools/fault_sweep.py` | independent oracles (Python, .NET) for each structure, and an allocation-failure sweep |
| Fixed point | `tools/fixed_point.py`, `bootstrap/fixed-point/` | ceesafec built by itself emits byte-identical C, generation after generation |
| Mutation suite (M2) | `tools/mutation_suite.py` | mutants of seed programs; the expected (code, line) diagnostics come from an independent oracle, not from the checker |
| Fault-injection sweep (M3) | `tools/fault_injection.py` | every run-pass program that allocates, built with ASan+UBSan, run once per allocation attempt *k* with attempt *k* failing; each run returns from `main` with `live=0` or panics with "unwrapped an Err value", with no sanitizer report |
| Collection properties (M6) | `tools/collection_properties.py` | a model of each built-in collection predicts the exact output of random operation programs (values, lengths, the growth rule of `type-system.md` §23.1, stale handles, drop order), also under the fault sweep |
| Freestanding | `tools/freestanding_check.py` | the freestanding programs build with `-ffreestanding -nostdinc` for `x86_64-` and `aarch64-unknown-none-elf` and run against their C harnesses |
| Debugging | `tools/debugger_check.py` | a `-g` build under lldb or gdb with `editors/debug`'s commands (`tooling.md` §3) |
| CosmoOS (C1–C9, K3) | `integrations/cosmoos/c*.py`, `k3.py` | ceesafec's C under CosmoOS's own flags, host tests, fuzz targets and module loader rules, and in its QEMU boots; CosmoOS's tree is only read |
| Same C on another host | `tools/host_c_check.py` | ceesafec built on Windows (MSVC, clang) emits the same C as on macOS and Linux |
| Specification (§4) | `tools/spec_check.py`, `tools/guide_check.py` | every diagnostic code is explained; the guide's examples do what the pages say; links resolve |
| Frame reports (L1 A3) | `tools/run_corpus.py`, family `frames` | `--explain-frames` against its `.frames` snapshot; every build asserts each frame's size in C |
| Interpreter agreement (L1 A1) | `tools/run_corpus.py --interp` | every run-pass and run-fail program run with `ceesafec run --interp` meets the same expectations (output, exit, panic, leak check) as its C build |
| Library diagnostics (Decision 0008) | `ceesafec check --library-diagnostics` on an empty program | `lib/` reports no diagnostic: since lib/ is CeeSafe's, its errors are no longer assumed to be porting gaps |
| Cancellation sweep (L1 A2b) | `tools/cancel_sweep.py` | every async run-pass program, built with ASan+UBSan, run once per suspension point *k* with `CEESAFE_CANCEL_AT=k`; each run exits cleanly with `live=0` and no sanitizer report |

`tools/contract_tests.py --ceesafec PATH` runs all of these but CosmoOS and the host check, against one ceesafec.

Both sweeps also take an executable package instead, built in its directory with its manifest:
`tools/cancel_sweep.py --compiler PATH --package DIR` (repeatable) and `tools/fault_sweep.py --compiler PATH
--package DIR`. That is how a package outside this repository (CeeSafe.Web's gates) uses them.

## 2. Corpus format

Runner: `tools/run_corpus.py --compiler out/ceesafec --corpus tests/reference/tests/corpus`.

- `parse-pass/*.cee`: must parse with **no** diagnostics. The AST dump (`ceesafec parse --dump-ast`) must equal the
  sibling `.ast` snapshot. **Every new or changed snapshot is reviewed by hand before it is committed.** A snapshot is
  an expected value, not a recording.
- `parse-fail/*.cee`: annotate each expected diagnostic on the line where it is reported, as `// error[N0100]`, or
  `// error[N0100]@14` to also pin the column. A missing **or extra** diagnostic fails the test, so it also checks
  error recovery.
- `check-pass/*.cee`: must type-check with no diagnostics. The HIR dump (`ceesafec check --dump-hir`) must equal the
  sibling `.hir` snapshot.
- `check-fail/*.cee`: must **parse** cleanly and produce exactly the annotated semantic diagnostics.
- A file with a line `// profile: freestanding` is checked in the freestanding profile (`profiles.md` §4), and one with
  `// option: no-float` as if built with `--no-float` (`type-system.md` §35.9).
- `run-pass/*.cee` and `run-fail/*.cee` are compiled to C, built with strict warnings and `-Werror`, and run.
  Directives:
  - `// expect-stdout: text`: one per output line, in order; `// expect-stdout:` alone is an empty line.
  - `// expect-exit: N`: the exit status (default 0).
  - `// expect-panic: reason`: placed on the line where the panic must be reported. The test requires exit status
    134 (SIGABRT) and `ceesafe panic: <reason> at <file>:<line>:` on standard error.
- `ffi/` programs are linked with their C harness (`NAME.c`, and `NAME.types.h` for imported C types,
  `type-system.md` §31); `freestanding/` programs with their environment.
- `emit-c/*.cee` have reviewed snapshots of the generated `main.c`; `placement/`, `bounds-checks/`,
  `overflow-checks/` and `closure-calls/` have snapshots of the `--explain-…` reports (`optimisations.md`).
- The C configurations: the contract suite builds the run families under the host's `cc` with `-Werror`, and under
  clang `-O1` with ASan+UBSan, `-fno-sanitize-recover=all` and the runtime's allocation report (`--check-leaks`: a
  program that exits normally with live allocations fails). On Linux, also under gcc with `-O2 -D_FORTIFY_SOURCE=2
  -Wall -Werror`, the flags Ubuntu's gcc applies by default: glibc's `warn_unused_result` and gcc's own flow warnings
  then reach the runtime and the generated C. Every configuration must meet the expectations, so the run corpus is
  also a differential test.

A change to the language adds corpus files, with snapshots written by ceesafec run from the corpus root. Adding
library code (`lib/`, Decision 0008) can renumber closures in `.hir` snapshots and reorder types in `.c` ones, since
library items are declared first: such an update is a reviewed edit that changes nothing else (L1 A1: `check-pass/effects-of.hir`,
`emit-c/slice-bulk.c`).

## 3. Snapshots

The corpus's snapshots are CeeSafe's own: an intended change to what ceesafec emits updates them by a reviewed edit,
recorded with the change, and never by regenerating them to make a run pass. `tools/run_corpus.py --reference PATH`
also requires a second compiler to give identical observable outcomes.

## 4. The specification's own checks

- `tools/spec_check.py`: every diagnostic code `ceesafec` reports has a row that `ceesafec explain` finds; every
  relative link in `spec/` and `guide/` resolves; `spec/api/core.md` is what `ceesafec doc` writes for `lib/core`; no
  tracked file uses a retired name.
- `tools/guide_check.py`: every `ceesafe` code block of `guide/` is compiled as a file named `main.cee`:
  - a block must type-check; with a `main`, it is also run in the interpreter, and a `text` block right after it must be
    exactly its output;
  - an HTML comment directly before the block changes that:
    - `<!-- ceesafe: fail N0241 -->`: the block must fail with exactly these codes, and a `text` block after it must be
      exactly the compiler's messages;
    - `<!-- ceesafe: freestanding -->`: checked with the freestanding profile, not run;
    - `<!-- ceesafe: check -->`: type-checked, not run (it calls C);
    - `<!-- ceesafe: panics -->`, `<!-- ceesafe: exits 1 -->`: the run's exit status;
    - `<!-- ceesafe: skip -->`: not checked, for a block that cannot stand alone (a second package's file, a fragment).
      Use it sparingly, and check the example some other way.

## 5. Not yet done

A random program generator (M7d, K7) and a 24-hour differential campaign with it are ROADMAP P9 items.
