# Parity ledger

Hand-maintained. It says what the port must cover, how each part is checked, and where it stands. The facts it
summarises are generated: `docs/reference-inventory.md` (by `tools/inventory.py`) lists every C# file, command,
option, diagnostic code, runtime file and test class with its phase. `tools/run_corpus.py` checks the corpus against
any compiler. Regenerate the inventory after every rebaseline and review its diff.

Reference: the reference `07d561f` (0.4.0-dev), stage-0 the reference compiler 0.3.1 (`bootstrap/reference.md`).

## 1. By phase

Lines are C# lines from the inventory. A file shared between phases counts under its combined label (`P1+P3`, etc.).

| Phase | C# scope (lines) | Black-box evidence | Other evidence still to build | Status |
|---|---|---|---|---|
| P1 support | `IntegerSemantics`, `FloatSemantics` (297), the version constant | `tests/support` (6 programs, oracles in `tools/oracles/`), fault sweep | `FloatFormatTests`, `RuntimeSemanticsTests` edge values as fixtures (with P3/P5) | **done** (support library; the semantics files themselves port in P3) |
| P2 frontend | `Syntax/`, `Diagnostics/` (3,959) | parse-pass 40 (`.ast`), parse-fail 27 | `LexerTests`, `ParserTests` cases as fixtures; token dump decision | not started |
| P3 types/HIR | `Semantics/`, `Hir/` (12,133, plus `Compilation.cs` 131 shared with P4) | check-pass 27 (`.hir`), check-fail 128 (type-focused part) | `PackageTests`, `PrivateFieldTests`: two-package fixtures for N0310–N0315 | not started |
| P4 NIR/safety | `Nir/` (not `ClosureSpecialisation`), `Analysis/` safety passes (4,346) | check-fail 128 (all) | `MutationTests` (411 lines), `UnsafeTests`; NIR dumps (`--dump-nir`) as a reference export | not started |
| P5 flat C | `Backend.C/` (not `CStructurer`), `CoreLibrary` (4,406) | run-pass 153, run-fail 40, emit-c 24, ffi 6, freestanding 3 (partial) | C configurations (§3); `FaultInjectionTests`, `CollectionPropertyTests`, `SourceMapTests`, `NicheLayoutTests`, freestanding symbol checks | **done** (`docs/p5-plan.md`): C identical 284/284 in four forms; the C matrix, the fault sweep, the collection properties and the freestanding checks ported and passing. `SourceMapTests` and `NicheLayoutTests` read the C, which is the reference compiler's byte for byte with and without `-g` (inferred covered; not ported) |
| P6/P6R | — | gen2 = gen3 comparison (`tools/fixed_point.py`) | rename fixtures | P6 **done** (`docs/p6-fixed-point.md`); P6R **done** (Decision 0002; `compare_packages.py --renamed all|root`) |
| P7 optimisations | `CheckElimination`, `PlacementAnalysis`, `ClosureSpecialisation`, `CStructurer` (2,379) | bounds-checks 3, overflow-checks 2, placement 3, closure-calls 1 | `StructuredCTests`; on/off runs of each switch | not started |
| P8 tools | `Interpreter/`, `Packages/`, `Formatting/`, `Documentation/`, `Tooling/` (8,627 + `Manifest.cs` 274) | — | protocol and CLI fixtures for every test class marked P8 in the inventory | in progress (`docs/p8-plan.md`): `explain`, `fix` and `fmt` identical (S0, S1) |
| CLI | `Program.cs` (1,211) | — | the 15 commands and 26 listed options, plus the unlisted ones (inventory §2) | `parse`, `check`, `build` with every option, `fix`, `explain`, `fmt`; the reference compiler's usage text and usage errors (`tools/compare_cli.py`) |

## 2. Corpus baseline (run 2026-10-30)

`tools/run_corpus.py --compiler the reference compiler`, macOS arm64, Apple clang 21.0.0, default C flags (the reference compiler's `cc`):

| Family | Pass | Fail | Partial |
|---|---:|---:|---:|
| parse-pass | 40 | 0 | 0 |
| parse-fail | 27 | 0 | 0 |
| check-pass | 27 | 0 | 0 |
| check-fail | 128 | 0 | 0 |
| run-pass | 153 | 0 | 0 |
| run-fail | 40 | 0 | 0 |
| emit-c | 24 | 0 | 0 |
| ffi | 6 | 0 | 0 |
| freestanding | 0 | 0 | 3 |
| bounds-checks | 3 | 0 | 0 |
| overflow-checks | 2 | 0 | 0 |
| placement | 3 | 0 | 0 |
| closure-calls | 1 | 0 | 0 |

The same run with `--compiler` set to the reference compiler built from `07d561f` and `--reference the reference compiler` found **no
observable difference** in any of the 457 files (status, stdout, stderr, emitted files) under the two listed
normalisations. The only source difference between `v0.3.1` and `07d561f` in `src/`, `lib/` and `runtime/` is the
version string (`git diff --stat v0.3.1 07d561f`); the other commits change tests and docs. So 0.3.1 is an
unambiguous oracle for `07d561f`.

The runner was checked against a deliberately wrong compiler (dropping the first diagnostic of every JSON check and
the last line of every AST dump): it failed all 40 parse-pass and 128 check-fail files, and reported the reference
difference in `--reference` mode.

## 3. Known gaps in the black-box evidence

- **The C matrix** is complete since P5 (2026-10-30): `-O2`, `-O0`, the portable overflow fallback
  (`-DNV_NO_BUILTIN_OVERFLOW`), ASan+UBSan with leak checks, `--split`, `-g`, all with `-Werror`, under Apple clang 21,
  clang 18 and GCC 12, and TSan for the thread programs (`run_corpus.py --cflag=… --build-option=…`). MSVC is not
  run (P9). GCC 12 `-O2 -Werror` rejects `run-pass/nested-patterns` (`-Wuninitialized` in an enum constructor) for
  The reference compiler's C as for ceesafec's: the reference's, recorded.
- **Freestanding**: the runner checks only that the object builds; `tools/freestanding_check.py` ports
  `FreestandingTests` (symbols, sections, harness runs, both CosmoOS triples, `ld.lld`).
- **20 codes have no corpus annotation** (inventory §3): N0109, N0258, N0300, N0310–N0315, N0320–N0329, N0335.
  Most are package and build errors tested by C# unit tests (`PackageTests`, `PackageManagerTests`, `CliTests`).
  They need black-box package fixtures before P3 (N0310–N0315) and P8 (the rest).
- **N0258 (`NonCopyBinding`) is retired** (diagnostics.md: since M3c, type-system §18.4) but still declared in
  `DiagnosticCodes`, which is the reference's rule for never reusing a code. CeeSafe keeps the reservation and emits nothing.
  Upstream nit: `type-system.md:702` still says "N0258 otherwise".
- **Unit-test-only behaviour.** 39 C# test classes (inventory §5) test through the C# API rather than the CLI. Each
  needs CLI-level fixtures before its phase gate. The largest are `PackageManagerTests` (528 lines),
  `CollectionPropertyTests` (461) and `MutationTests` (411).
- **No token or NIR snapshots exist.** AST and HIR snapshots cover P2 and P3. Whether P4 compares NIR dumps byte for
  byte, or through a mapping, is a P4 decision; `--dump-nir` works today.

- **Leaks are not checked by the reference's run corpus.** `run_corpus.py --check-leaks` requires the runtime's `live=0` on a
  normal exit. The reference compiler 0.3.1 skipped drops of `if`/`while` condition temporaries (the reference #1), which its corpus did not
  notice. Since 0.4.0 the reference's `RunCorpusTests` requires `live=0` itself, and `probes/run-pass/condition-temporaries.cee`
  keeps the case. CeeSafe's runs still use `--check-leaks`.

## 4. Reference test suite

`dotnet test the reference.slnx` at `07d561f` (2026-10-30, macOS arm64): 3,589 passed, 0 failed, 0 skipped, 12 min 24 s.
This host has no real GCC and no MSVC, so those configurations did not run here; the Ryzen box (GCC) and the
Windows VM (MSVC) cover them.
