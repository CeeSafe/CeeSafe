# P6: the first verified fixed point

Status: **complete** (2026-10-30; CeeSafe `1b1aae4`, stage 0 the reference compiler 0.6.0): the P6 gate passed (`ROADMAP.md` §P6).
Everything below was run on that date; nothing in it is only proposed.

## Procedure

`tools/fixed_point.py` builds the chain from CeeSafe's sources at one commit (`git archive` of `ceesafe.toml` and
`src/`, so nothing outside the tree is read):

| Stage | Built by | Also emits |
|---|---|---|
| gen 1 | the pinned the reference compiler 0.6.0 (default options) | — |
| gen 2 | gen 1 | C2, the C gen 1 emits for CeeSafe |
| gen 3 | gen 2 | C3, the C gen 2 emits; gen 3 then emits C4 |

- Inputs, flags (`build --cc <cc>`, so `-O2` and the strict warning set) and toolchain are the same at every stage.
- Each executable is `gen/<n>/ceesafec`. macOS's ad-hoc code signature records the output's file name, so `gen2` and
  `gen3` as names gave executables that differed in that field only (the first run, since corrected).
- The chain runs twice, from clean directories under two different absolute paths (`…/a` and
  `…/deeper/second-run/b`), and no emitted file may contain either path.
- From gen 1 on, `PATH` holds only a directory of logging shims for `cc` and the tools a compiler driver runs, so
  neither the reference compiler nor dotnet can be reached; `HOME` and `TMPDIR` are inside the run's directory. The shims' log is the
  subprocess audit.
- `tools/contract_tests.py` then runs every contract test of P2–P5 against gen 2 (and so gen 3, which is the same
  file): parse, check, NIR and C comparisons (four forms), package fixtures, the run corpus under `-Werror` and under
  ASan+UBSan with leak checks, the mutation suite, the fault sweep, the collection properties and the freestanding
  checks. It accepts exactly the recorded divergences: the reference compiler's three `check --dump-hir` crashes and the reference #6; and, when
  the C compiler is GCC, the GCC 12 report on `run-pass/nested-patterns` that the reference compiler's own C also gets.

## Results

Manifests: `bootstrap/fixed-point/2026-10-30-macos-arm64-clang.md` and `bootstrap/fixed-point/2026-10-30-linux-x64-gcc.md`
(inputs, toolchain versions, every output's sha256, the audit, the commands).

| | macOS 27 arm64, Apple clang 21 | Debian 12 x86-64 (Ryzen box), GCC 12.2 |
|---|---|---|
| Stage 0 | `the reference compiler 0.6.0` (sha256 `1d8b8f1f…8374`) | the v0.6.0 release `ceesafec-linux-x64`, checked against its `SHA256SUMS` |
| C2 = C3 = C4 | yes, all 7 files | yes, all 7 files |
| C equal across the two directories | yes | yes |
| gen 2 = gen 3 executables | yes (`00bf458c…`) | yes (`96d1b4cb…`) |
| gen 1 reproducible across directories | yes | yes |
| Work paths in output | none | none |
| Processes started from gen 1 on | `cc` only (2) | `gcc` and its `as` and `ld` (16) |
| Contract tests on gen 2 | all pass (check: the 4 recorded divergences) | all pass (check: the same 4; GCC: `nested-patterns`) |

The C of the two hosts is identical file for file (`main.c` `40374dbf…`), so the fixed point does not depend on the host
compiler that built gen 1: a CeeSafe built by Apple clang and one built by GCC emit the same C for themselves.

## What P6 does not show

- MSVC, and Windows hosts (P9).
- The P6R names: the fixed point is on the reference's file and manifest names. P6R reruns it on CeeSafe's.
- That the C is right, beyond what the contract tests show: the fixed point shows that ceesafec compiles itself
  consistently, and P2–P5 that it matches the reference.
