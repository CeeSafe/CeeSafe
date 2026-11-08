# CeeSafe roadmap: the port of the reference compiler to its own language, as CeeSafe

Date: 2026-10-30 (revises the 2026-10-29 draft; reviewed 2026-11-01). Status: **P0–P8 complete** (CeeSafe names since
`8941c10`; the fork point at P8 S7, `docs/fork-point.md`). **P9 in progress**: ceesafec 0.3.2 is the latest release
(2026-11-01), bootstrapped from ceesafec 0.2.0 (Decision 0009). **Language track L1 (async/await) complete**
(2026-11-01, ADRs 0031–0039).
Repository: `~/dev/CeeSafe`. Reference: the C#/.NET 10 compiler CeeSafe was ported from (retired and private;
Decision 0010). Sections 1–3 and 8 describe the port as planned on 2026-10-30; §4's checkboxes, §9 and §10 are
current.

**Identity** (decision 0001):
- The public language is **CeeSafe**. The compiler is `ceesafec`.
- Sources use `.cee`. Packages use `ceesafe.toml` and `ceesafe.lock`, and `$CEESAFE_HOME`.
- The reference was CeeSafe's stage-0 compiler and behaviour oracle until the fork (P8 S7). It is now retired
  (Decision 0010), and the release's stage 0 is a CeeSafe release (Decision 0009, `bootstrap/reference.md`).
- Until `ceesafec` compiled itself, CeeSafe's own sources used the reference's file names; P6R renamed them in one
  commit (`8941c10`).

**The work resolves the reference's OD-33.** That is "a compiler for the language written in the language", recorded by ADR 0023 as option C.
The reference ADR 0001 chose C# and expected self-hosting only "for a long time".

## 1. Scope and completion criteria

There are three deliverables:

1. **Compiler parity.** This covers:
   - lexing, parsing and package names;
   - types, generic checking and instantiation;
   - HIR and NIR;
   - ownership, borrowing, effects and destruction;
   - every optimisation;
   - portable C emission.
2. **Verified self-hosting.** the reference compiler builds `ceesafec` (generation 1). Generation 1 builds generation 2, and generation 2
   builds generation 3. Generations 2 and 3 must emit byte-identical C and pass the compiler tests.
3. **Product parity and handover.** The complete CLI, interpreter, package manager, formatter and doc generator, the
   LSP and MCP servers, fixes and JSON diagnostics, debugger scripts, runtime bundling and release/install all work
   without .NET.

Self-compilation alone does not complete the port. Accepting the compiler's own sources says nothing about whether the
borrow checker is correct. No reduced language subset will be presented as a replacement.

The C runtime (`runtime/`, 2,898 lines) and the CeeSafe libraries (`lib/core`, `lib/std`, `lib/kernel`) are carried over
with their provenance and licence (GPLv2). The compiler's algorithms stay in CeeSafe. Invoking the C# compiler to parse,
check, emit or run tooling does not count as a port. The C compiler, linker, debugger and git remain external tools,
used by the commands that need them.

## 2. Reference audit (2026-10-30)

The audit read the reference `main` at `07d561f` (0.4.0-dev, clean tree) and the released the reference compiler 0.3.1 (`v0.3.1` = `4481e12`).
Paths below are relative to `the reference checkout`.

### 2.1 Size

Physical lines of `.cs`, excluding `bin/` and `obj/`. These measure scope, not effort.

| Component | Files | Lines | Notes |
|---|---:|---:|---|
| `src/Reference.Compiler` | 58 | 36,559 | |
| `src/Reference.Cli` | 1 | 1,211 | 18 commands |
| `tests/Reference.Compiler.Tests` | 41 | 6,673 | 39 test classes |
| `tests/Reference.Fuzz` | 3 | 1,919 | random program generator |
| `tests/corpus` | 573 files | — | 13 families |

Compiler size by directory:

| Directory | Lines | Notes |
|---|---:|---|
| `Semantics/` | 11,419 | `TypeChecker.cs` alone is 9,100 |
| `Backend.C/` | 4,975 | `CEmitter.cs` 3,840; `CStructurer.cs` 636 |
| `Analysis/` | 3,728 | Borrow, Move, InPlace, Drop, Effect, CheckElimination, Placement |
| `Syntax/` | 3,717 | `Parser.cs` 1,789 |
| `Interpreter/` | 3,640 | |
| `Nir/` | 2,361 | |
| `Packages/` | 1,874 | |
| `Tooling/` | 1,750 | LSP, MCP, SemanticIndex |
| `Formatting/` | 1,220 | |
| `Hir/` | 1,011 | |
| `Documentation/` | 417 | |
| `Diagnostics/` | 242 | |
| root | 205 | |

Corpus `.cee` counts:

| Family | Count |
|---|---:|
| run-pass | 153 |
| check-fail | 128 |
| parse-pass | 40 |
| run-fail | 40 |
| check-pass | 27 |
| parse-fail | 27 |
| emit-c | 24 |
| ffi | 6 |
| bounds-checks | 3 |
| placement | 3 |
| freestanding | 3 |
| overflow-checks | 2 |
| closure-calls | 1 |

### 2.2 Can the reference host a compiler today? Not yet

The 2026-10-29 draft assumed CeeSafe could be written "within the pinned stage-0 language". The audit shows the
language core is adequate, but its library and host support are not.

| Area | Present | Missing | Evidence |
|---|---|---|---|
| Core language | enums with payloads and `match`, recursive `Box`, generics with interface bounds, `dyn`, closures, `T?`/`Result` with `?`, statics, threads, multi-file packages | `From` conversion for `?`; `impl<T> I for G<T>` and `Self`; `i128` | `docs/type-system.md` §15, §17.4, §20–§21.1, §36–§39 |
| Collections | `Vector`, `String`, `SlotMap`/`Handle`, `StableVector`; `sortBy` | **hash map, set and ordered map**; slice search | §23–§24, §16.5; `lib/core/slices.cee` |
| Strings | `append`, `appendNumber`, boundary-checked slicing, `==` | hashing, ordering, clone, char/UTF-8 iteration, integer and float parsing, `find`/`startsWith`; growing only from a `StringView` | §22.2, §23.2 |
| Host services | read/write/remove files, stdin bytes, stdout/stderr, argv, `Process.run` (status only), TCP | environment, cwd, directories, stat, mkdir/rename, temp files, clock, **captured child output**, stdin to a child | §26; `runtime/hosted/ceesafe_rt_io.c` |
| Borrowing | IDs and handles, field-precise and constant-index disjointness, `splitAtMut` | any non-constant index aliases every element, so table-heavy code must copy out or use handles | `docs/lifetimes.md` §4, §8 |
| FFI | `extern "C"`, `.c` files linked by `build`, `@repr(C)` structs | `run --interp` runs no C | §28; `docs/unsafe.md` |
| Scale | The reference compiler checks a 202k-line package in 13.1 s | the largest CeeSafe program in the repository is ~360 lines; no parser had been written in the language | ADR 0022; `lib/core/atomics.cee` |

This is the port's largest risk, and the reason for the **N track** (§4) before and alongside P1–P3. P1's probes
(`docs/p1-probes.md`) narrowed it to three needs:
- **N0**, a compiler crash;
- **N1a**, turning bytes into text;
- **N4**, host services.

Maps, generic impls, `From` and borrow precision have accepted the reference-side patterns.

### 2.3 C# surfaces that need a native design

| Finding | Evidence |
|---|---|
| The pipeline order is fixed | `src/Reference.Compiler/Compilation.cs:76-129`. The order: core libraries, parse, `PackageNames.Rewrite`, `TypeChecker`, `NirBuilder`, then per function `InPlaceChecker`, `MoveChecker`, `BorrowChecker`, then `DropElaborator`, then `EffectChecker` |
| Spans are UTF-16 code units over UTF-8 input | `Syntax/SourceText.cs:3`; `docs/grammar.md` §1.1 |
| Interned names get deterministic first-seen IDs | `Syntax/SymbolTable.cs:3` |
| Layout comes from the target, not the host | `Semantics/Types.cs:22` (`TargetInfo`); `docs/c-backend.md` |
| Exact integers through `BigInteger` | 6 files: `IntegerSemantics`, `FloatSemantics`, `Types`, `TypeChecker`, `CEmitter`, `CheckElimination` |
| Width-specific float evaluation and formatting | `Semantics/FloatSemantics.cs:16`; type-system §35 |
| Reflection walks the trees | `Tooling/SemanticIndex.cs:177-280`; `Semantics/TypeChecker.cs:3981`, `:6833` |
| JSON through `System.Text.Json` | `McpServer.cs`, `LanguageServer.cs`, `DiagnosticJson.cs` |
| Hash collections whose order may reach output | `TypeChecker.cs` (58 `Dictionary`, 40 `HashSet`), `CEmitter.cs` (12, 17), `CheckElimination.cs`, `BorrowChecker.cs`; explicit sorts at `Fixer.cs:35`, `CheckElimination.cs:156` |
| Embedded resources: libraries, runtime, docs, templates | `CoreLibrary.cs`, `Backend.C/CBackend.cs:54-63`, `Documentation/EmbeddedDocs.cs`, `Packages/AgentFiles.cs` |
| The CLI is broad | `src/Reference.Cli/Program.cs:225-335`. Commands: `parse check build run fmt doc new agents fetch publish index-check fix explain mcp lsp` |
| Tests cover mutations, allocation failures and several C configurations | `docs/testing.md` §3–§5; `MutationTests.cs`, `FaultInjectionTests.cs`, `MsvcTests.cs` |

### 2.4 the reference changes since the 2026-10-29 draft

The draft's baseline was `06fd7cd`. The current ADR range is **0001–0027**. Both of these are new:
- **0026**, inline atomics: the atomic types moved to `lib/core`, in every profile.
- **0027**: `here` in static initialisers, `Mutex.constant`, `layoutOf<T>()`, raw `read`/`write` for any `T`, and
  `ObjectCache<T>`.

The reference compiler 0.3.0 and 0.3.1 are released. M14 is still in progress: K2 (typed calls across units) and K6 are open.

## 3. Architecture and porting rules

### 3.1 Preserve semantics, change storage

- Follow the current grammar, ownership invariants and accepted ADRs. A needed language addition went through the
  reference (the N track) until the fork; since then it is a CeeSafe ADR, spec first (Decisions 0006, 0007), never
  compiler-only syntax or intrinsics.
- Replace C# class hierarchies with tagged enums and typed IDs into owned tables: AST, HIR, types, definitions,
  functions, blocks, locals. Parent and cross-reference edges are IDs. Lists are ranges into contiguous tables.
- Keep mutable borrows short: read IDs or `Copy` data, end the borrow, then mutate. The borrow checker's
  index-aliasing rule (§2.2) makes this mandatory. P1 must prove the pattern before bulk translation.
- The compilation owns its inputs. Tokens keep source IDs and spans, and decoded literals own their bytes. Server
  requests release everything from the previous compilation.
- Maps, sets, interners and worklists must iterate deterministically. Hash order never reaches output; output paths
  and declaration order are sorted or follow the specified order.
- Allocation failure ends compilation with a diagnostic and no partial output. Fault injection tests this.
- `unsafe` and raw pointers stay at documented FFI boundaries. There is no tracing GC and no implicit reference counting.

### 3.2 Text, numbers and the host boundary

- Keep source bytes plus a tested conversion to the reference's UTF-16 offsets and columns. Diagnostics, fixes, dumps
  and LSP all depend on it. Test BOMs, line endings, supplementary characters, invalid UTF-8 and EOF.
- Exact integers: a small signed-magnitude limb integer in CeeSafe, sized to the audited `BigInteger` uses. Never use
  unchecked `i64` where the reference is exact.
- Floats: binary32/64 parsing, rounding, NaN, ±0, infinities and decimal formatting must match `FloatSemantics.cs` and
  `runtime/hosted/ceesafe_rt_float.c`. Locale-dependent libc is not an oracle.
- Host services (files, paths, environment, processes, clocks, temp files, tool discovery) sit behind one small layer.
  Ideally it is N-track library API. A C adapter is allowed only with a written contract covering ownership, lengths,
  encoding, errors and cleanup. Processes take argument arrays and never shell strings.

### 3.3 Repository layout

As planned on 2026-10-30; the layout as built is in `AGENTS.md` ("Layout").

```text
CeeSafe/
  ROADMAP.md AGENTS.md CLAUDE.md .mcp.json   (agent files from ceesafec new --agent claude)
  ceesafe.toml                                (the reference's manifest name until P6R)
  src/                                        compiler, in CeeSafe (one package; files share one scope)
    support/ syntax/ diagnostics/ semantics/ hir/ nir/ analysis/
    backend_c/ interpreter/ packages/ formatting/ documentation/ tooling/ host/  (planned)
  runtime/ lib/ resources/                    carried over at P5 with provenance
  bootstrap/reference.md                      stage-0 pin
  docs/decisions/                             CeeSafe decisions
  tests/ tools/ benchmarks/                   (planned)
  out/                                        ignored
```

The package compiles as one whole program, as the reference compiler does today; the reference has no separate compilation (ADR 0013 D4).
Subdirectories under `src/` are only for organisation, since every `.cee` file under `src/` shares one scope.

### 3.4 Rename contract

| Surface | The reference today | CeeSafe | When |
|---|---|---|---|
| Compiler | `ceesafec` | `ceesafec` | skeleton now; parity P8 |
| Source extension | `.cee` (`Packages/Manifest.cs:271`, `Program.cs:697`) | `.cee` | P6R |
| Manifest and lock | `ceesafe.toml`, `ceesafe.lock` (`Manifest.cs:32`, `Workspace.cs:28`) | `ceesafe.toml`, `ceesafe.lock` | P6R; conversion in P8 |
| Home | `$CEESAFE_HOME`, `the reference's install directory` (`Workspace.cs:111`) | `$CEESAFE_HOME`, `~/.ceesafe` | done (P8 S3) |
| Generated C | `cee_`, `ceeMk_`, `CEESAFE_PROGRAM_*_H`, `ceesafe_exports.h` | `cee_`, `cee<X>_`, `ceesafe_exports.h` (Decision 0004) | done (P8 S7) |
| Runtime | `ceesafe_*` (~374), `nv*` (~424), `CEESAFE_*` (10), `runtime/include/ceesafe/rt.h` | `ceesafe_*`, `CEE_*`, `ceesafe/rt.h`, ABI version (Decision 0004) | done (P8 S7) |
| Diagnostics | 126 `N####` codes (`Diagnostics/Diagnostic.cs:14`), used by 159 corpus files | letter open, with a mapping | P6R |
| MCP/LSP | server `ceesafec`, tools `ceesafe_*` | `ceesafec`, `ceesafe_*` | done (P8 S5) |
| Agent files | the reference's markers and rule file | `<!--CEESAFE START/END-->`, `ceesafe.mdc` | done (P8 S3) |
| Editor | the reference's extension and language id | `ceesafe-language`, language id `ceesafe` | done (P8 S6) |
| CosmoOS | pinned the reference compiler | pinned `ceesafec` (`scripts/cee-gen.py`; the owner's migration, P9) | done (2026-10-31) |

The reference's generated-C ABI is kept through the first fixed point. Renaming never changes ownership, lifetime, drop or
safety semantics, and it is never done by text substitution.

## 4. Phases and gates

Every phase delivers working code, positive and negative tests, an updated parity ledger and risk table, and a dated
entry in §10. An unchecked item is planned, not done.

### P0 — Foundation: pin, identity, inventory and runner

Dependencies: none. **Complete (2026-10-30)**: the gate passed on the Ryzen box (§10).

- [x] Scaffold the package with the pinned the reference compiler (`ceesafec new ceesafe --agent claude`), and write a `ceesafec` skeleton
  that handles `--version` and `--help` (2026-10-30).
- [x] Decision 0001 covers identity, the bootstrap route, the upstream N track and the reference continuing. Pin the reference compiler 0.3.1 and
  The reference reference in `bootstrap/reference.md` (2026-10-30).
- [x] Inventory and ledger (2026-10-30). `tools/inventory.py` generates `docs/reference-inventory.md`
  deterministically. It lists 59 C# files, 15 commands, 26 options and 126 diagnostic codes (with their emitting files
  and corpus coverage), plus runtime, library and resource files and the 39 test classes, each with a phase.
  `docs/parity.md` is the hand-kept ledger: evidence per phase, the corpus baseline and known gaps.
- [x] Compare 0.3.1 with `07d561f` (2026-10-30). The only source difference in `src/`, `lib/` and `runtime/` is the
  version string. Across all 457 corpus files the runner found no observable difference (`docs/parity.md` §2).
- [x] Write the black-box runner (2026-10-30). `tools/run_corpus.py` takes any compiler path and covers all 13 corpus
  families. It checks annotations, snapshots, expected output and panics, and in `--reference` mode compares status,
  stdout, stderr and emitted files. It applies two named normalisations: the generated-by banner and the program name.
  It was tested against a deliberately wrong compiler.
- [x] First C configurations (2026-10-30, Ryzen box): GCC 12 (default `cc`), clang 18 `-O2`, and clang 18 `-O1` with
  ASan+UBSan. The rest of the matrix moved to P5 (below).
- [x] Reference dumps (2026-10-30). The corpus's AST (40) and HIR (27) snapshots are the reference, and the runner
  reproduces them with 0.3.1. Exporting token and NIR dumps is deferred to P2 and P4, which decide how to compare them.
- [x] Run the reference suite (2026-10-30). `dotnet test the reference.slnx` at `07d561f` on macOS arm64 passed 3,589 of 3,589
  tests, with 0 skipped, in 12 min 24 s. No real GCC was available (`gcc` is Apple clang), and no MSVC (that needs the
  Windows VM). Whether TSan ran is not shown at default verbosity. A configuration the reference's suite cannot find counts as
  missing, not as passed.
- [x] Gate check (2026-10-30). On the Ryzen box (Debian 12, x86-64): the reference compiler 0.3.1 release download matched the
  release's `SHA256SUMS`; the reference was cloned at `07d561f`, and CeeSafe from a git bundle of `ef3e413`. Results:
  - `tools/inventory.py` regenerated `docs/reference-inventory.md` byte-identical;
  - the runner gave the Mac's results: 454 pass, 0 fail, 3 partial.

Carried into later phases (`docs/parity.md` §3):
- the rest of the C matrix: `-O0`, the overflow fallback, `--split`, TSan (P5);
- freestanding symbol checks (P5);
- CLI fixtures for the 20 codes the corpus does not annotate. N0310–N0315 are needed by P3, the rest by P8.

Gate: another checkout reconstructs the pin and runs a representative fixture set with identical results, and the
ledger has no unmapped items (inventory: 0 unmapped, 2026-10-30).

### N — Upstream the reference: self-hosting prerequisites (the reference milestone M15)

Each item is a reference change approved by the owner and made under the reference's rules: spec and ADR first, then tests, docs,
`roadmap.md` and `risks.md`. It ships in a release of the reference compiler, and CeeSafe re-pins (`bootstrap/reference.md`). P1's probes
(2026-10-30, `docs/p1-probes.md` §4) set this list and its order.

All of N0–N4a shipped in **the reference compiler 0.4.0** (the reference M15, tag `v0.4.0` = `66b6bda`, 2026-10-30), and CeeSafe re-pinned to
it the same day, then to the point release 0.4.1 (N7; `bootstrap/reference.md`). Each former
`probes/upstream/` repro is now a regression probe that must keep passing.

- [x] **N0. The compiler crash** (the reference #2) on a generic `Result` rebuild
  with `T = void` is now N0222, an error (`probes/check-fail/void-result-rebuild.cee`). The instance still does not
  check, so `void` results keep their non-generic lifts.
- [x] **N0b. The missing drop** (the reference #1) of a condition temporary is
  fixed, and the reference's run corpus now requires `live=0` (`probes/run-pass/condition-temporaries.cee`).
- [x] N0c. The two false rejections (the reference #4,
  #5) are fixed (`probes/run-pass/truncate-two-phase.cee`,
  `probes/run-pass/generic-param-borrow.cee`); their workarounds in `src/support/` and `tests/support/` are gone.
- [x] **N0d. A nesting limit** (the reference #3): 1,000 levels, N0110, with the
  passes on a 256 MiB stack. CeeSafe's parser must reproduce N0110 at the same depth (P2).
- [x] **N1a. Bytes to text**: `StringView.fromUtf8` (type-system §22.2), then `String.append`.
- [x] **N4a. Host services** (ADR 0028, type-system §26.7–§26.10): `Process.capture` (stdout, stderr, stdin input),
  `Environment.variable`, `Directory.create`/`list`/`remove`/`temporary`/`current` and
  `File.kind`. `probes/run-pass/host-process.cee` uses them to drive `cc`.
- [x] ~~N4b. Stdin by line or length~~: dropped; LSP/MCP framing can be buffered over `Console.read`.
- [x] **N7. lib/std's helper names** (found by the 0.4.0 re-pin): 0.4.0's `lib/std/host.cee` declared top-level
  helpers (`pathIn`, `fromAlloc`, `textOf` and others) that a program could not redeclare (N0201). Fixed in the reference compiler
  **0.4.1** (point release from `release-0.4`, 2026-10-30), CeeSafe's pin since; the probes use those names again.
- [ ] Optional ergonomics, none blocking:
  - N1b, `Vector` bulk append from a slice (performance);
  - N1c, text ordering, `clone`, `find`/`startsWith` and number parsing (until then, byte-wise functions in
    `src/support/`);
  - N1d, arrays of `StringView` (until then, `match` tables).
- [x] ~~N2 library maps~~: not needed upstream. A generic insertion-ordered map works in the language (`generic-map` probe) and
  lives in `src/support/`.
- [x] ~~N3 `impl<T>`, `Self`, `From`~~: not needed. Use one impl per instantiation, `K: Key<K>`, and lift functions
  (`generic-impls`, `error-layers` probes).
- [x] ~~N5 borrow precision~~: not needed. `splitAtMut`, copy-out and separate tables are accepted
  (`aliasing-workarounds` probe).
- [x] N6. The reference `docs/milestone-15.md` (drafted at `34ce40a`, implemented 2026-10-30), citing OD-33. Its items map to
  this list as follows: S1 is N0 (#2), S2 is N0b (#1), S4 and S5 are N0c (#4, #5), S3 is N0d (#3), S6 is N1a and S7
  (ADR 0028) is N4a. All shipped together in 0.4.0; the planned 0.3.2 was not needed.

Gate per item: the reference compiler release passes the reference's full suite, and CeeSafe's affected probes pass after the re-pin.

### P1 — Probes and the support library

Dependencies: P0. Runs alongside the N track.

- [x] Capability probes (2026-10-30; `probes/`, `docs/p1-probes.md`). There are 12, covering:
  - ID tables;
  - owning enums;
  - index aliasing, and the accepted workarounds;
  - a generic map;
  - a lexer and interner over all 525 CeeSafe source files;
  - error layers;
  - dataflow bitsets and worklists;
  - gaps in generic impls and text.

  Results: they pass on macOS and Linux, under GCC and clang, with and without ASan+UBSan. They found one compiler
  crash (N0) and decided the N list.
- [x] Host probe (2026-10-30). Writing a file, running `cc` with an argument vector, exit statuses, stderr and file
  existence all work. Capturing output and the environment are missing (N4a).
- [x] `src/support/` (2026-10-30), each module tested against an independent model (`docs/p1-probes.md` §7):
  - `bytes`;
  - `map`, an insertion-ordered `Map<K: Key<K>, V>`;
  - `interner`, with first-seen IDs;
  - `bits`;
  - `source`, giving the reference's line/UTF-16 positions, ill-formed UTF-8 as U+FFFD per maximal subpart, and up to
    two BOMs;
  - `bigint`, arbitrary precision for constants and float formatting;
  - `json` (the writer), byte-identical to .NET's relaxed escaping. The reader moves to P8.

  The first worklist is a `Vector` plus `Bits`, as in the dataflow probe.
- [x] Limits and allocation failure (`docs/limits.md`). Each limit is recorded with the phase that enforces it.
  `tools/fault_sweep.py` fails each of 573 allocations in turn: every failure comes back as an error, with the
  structures intact and nothing leaked.
- [x] Grow the skeleton (2026-10-30). `ceesafec` knows the reference compiler's 15 commands, loads the files they name into
  `SourceText` (`src/host/files.cee`), reports unreadable files, and names the phase that implements each command.

Gate: probes and the support library pass under ASan and UBSan with the pinned the reference compiler. Every compiler bug found has a
reduced reproducer filed upstream. Missing features block; they are never worked around by weakening checks.
**Passed 2026-10-30 (§10).** The four reproducers in `probes/upstream/` and the nesting crash were filed as the reference #1–#5 (2026-10-30), fixed in
the reference compiler 0.4.0, and the reproducers became regression probes at the re-pin.

### P2 — Source frontend

Dependencies: P1's text and storage, N1.

- [x] Port `SourceText`, the line index, interning, token kinds, the keyword table (`Keywords.Table`), literals,
  trivia and doc comments, and the lexer (2026-10-30; `docs/p2-frontend.md`). Checked by a token-dump oracle built
  from the reference's own `Lexer`: byte-identical over all 540 `.cee` files of the reference `v0.4.1`, 21 edge cases and
  3,000 random inputs. One documented difference: an invalid escape of a character above U+FFFF.
- [x] Diagnostics, their text rendering and the JSON document (`--diagnostics-format=json`), with summaries generated
  from the reference's documents (`tools/gen_catalog.py`).
- [x] Port the AST and the recursive-descent/Pratt parser: imports, attributes, generics and regions, patterns,
  closures, FFI, recovery, and splitting adjacent `>` (2026-10-30). The AST is tables of `Copy` nodes
  (`docs/p2-frontend.md` §5). The `else if` chains and `T???…` are built and printed in loops, so they cannot exhaust
  the stack where the reference does (§6).
- [x] Add `parse` and `--dump-ast` with byte-identical dumps.

Gate: all 40 parse-pass and 28 parse-fail fixtures match, including AST snapshots and exact diagnostics. Every `.cee`
file in the reference's repository and CeeSafe's own sources parse. Fuzzed malformed input ends with a controlled error.
**Passed 2026-10-30** on macOS and the Ryzen box (`docs/p2-frontend.md` §4):
- the parse families match the reference compiler 0.4.1 with no difference in exit status, stdout or stderr;
- all 540 `.cee` files of the reference `v0.4.1` give identical trees and diagnostics;
- so do CeeSafe's own files, except the documented escape span;
- 5,000 mutated and 3,000 random inputs are identical to the reference;
- the deep inputs end normally;
- the sanitizers report nothing, and there are no leaks.

### P3 — Names, types, generics, HIR

Dependencies: P2, N2–N3, P1's exact integers.

- [x] Port `PackageNames` (ADR 0013, N0310–N0315), scopes and visibility, builtins, `TargetInfo`, type interning,
  layouts and type properties.
- [x] Port declarations, expressions, coercions and reborrows, patterns and usefulness (`PatternUsefulness.cs`),
  constant evaluation, HIR construction and `--dump-hir`.
- [x] Port the generics: body checking, bounds, regions, instantiation, closure captures, `dyn`, owning closures,
  `Send`/`Share`, `@effectsOf`.
- [x] Port the M14 and K semantics: statics (ADR 0024), `here`, `layoutOf`, raw `read`/`write` (ADR 0027), inline
  atomics (ADR 0026), `lib/kernel` contracts (ADR 0025), profiles, `unsafe`/FFI, and embedded C state.
- [x] Split `TypeChecker.cs` (9,100 lines) into reviewable files using a source-to-port checklist that preserves
  diagnostic order.

Gate: check-pass and type-focused check-fail fixtures and HIR snapshots match exactly. Ownership negatives are tracked
as pending P4. This is not yet a full `check`. **Passed (2026-10-30, the reference compiler 0.5.0)**: `docs/p3-plan.md` (slices S0–S8,
`src/check/` as the split of `TypeChecker.cs`).

### P4 — NIR and every safety pass

Dependencies: P3.

- [x] Port NIR (`Nir.cs`, `NirBuilder.cs`, `NirPrinter.cs`) and `--dump-nir`.
- [x] Port `MovePaths`, `MoveChecker`, `InPlaceChecker`, `BorrowChecker`, `DropElaborator`, and then `EffectChecker`,
  in the pipeline order of §2.3. v0.6.0 has no `FlowAnalysis` pass: flows are the checker's `ResolveFlows`, which is
  P3's.
- [x] Cover:
  - field and index precision;
  - optional references;
  - captures and `dyn`;
  - threads and `Thread.both`;
  - kernel guards;
  - the sticky shared-deref soundness fix (N0248, the reference `3a99b4a`).
- [x] Port the mutation expectations (`MutationTests.cs` as `tools/mutation_suite.py`). The fault-injection sweep
  (`FaultInjectionTests.cs`) builds and runs programs, so it moved to P5's gate.

Gate: all 128 check-fail and 27 check-pass fixtures and the mutation expectations pass. NIR before and after drops
agrees with the reference. Every invariant in `docs/ownership.md` has mapped positive and negative evidence. `check`
now means the full pipeline. **Passed (2026-10-30, the reference compiler 0.6.0)**: `docs/p4-plan.md` (slices S0–S6) and
`docs/p4-evidence.md`. The results:

- **Fixtures:** all 136 check-fail and 27 check-pass fixtures give the reference's diagnostics. Of the 322 check
  programs, 318 are identical in text, JSON and `--dump-hir`. The other four are recorded divergences:
  - `enums`, `optionals` and `results`: the reference compiler's `--dump-hir` crashes on them;
  - `offset-of`: the reference #6.
- **Mutations:** all 1,152 mutants match the oracle.
- **NIR:** the 284 programs the reference compiler builds have identical NIR after drop elaboration. The reference compiler prints NIR only after
  elaboration, so the comparison is made there.

### P5 — Flat C backend and the native build

Dependencies: P4, N4 (process capture).

- [x] Port naming, type ordering and layout, declarations, aggregates, function/closure/vtable lowering, the runtime
  helpers, constants, drops, source maps, panic sites, FFI and imported structs, callbacks, exports and object mode
  (`src/backend/`, 2026-10-30).
- [x] Bundle `runtime/` (`tools/gen_runtime.py`) and `lib/` with deterministic tables. Construct `cc` and linker
  arguments, and publish output files (`src/host/toolchain.cee`, `src/host/build.cee`).
- [x] Add `build --emit c|exe|obj` with flat C, `-g`, `--line-directives`, `--split`, `--cc`, `--cflag`, `--ld`,
  `--program` and `.c` inputs. The switches of P7's forms (`--flat-c`, `--no-placement`, `--no-bce`, `--no-oce`,
  `--no-closure-specialisation`) are accepted: ceesafec always emits that form.

Gate: run-pass (153), run-fail (40), ffi and freestanding pass under clang -O0/-O2, the overflow fallback and
ASan+UBSan, with GCC where available. The M3 fault-injection sweep (`FaultInjectionTests.cs`, moved from P4) passes on
every run-pass program that allocates, which shows O4 at run time. `emit-c` snapshots match, or each difference is reviewed. Objects for both
CosmoOS architectures pass. **Passed (2026-10-30, the reference compiler 0.6.0)**: `docs/p5-plan.md` (S0–S3). The results:

- **C:** byte-identical to the reference compiler's (`--flat-c` and the optimisations off) for all 284 programs the reference compiler builds, in four
  forms each: flat, `--split`, `-g`, `-g --split`. CeeSafe's own C is identical too (394,094 lines; ceesafec 1.4 s,
  the reference compiler 4.1 s), on macOS and on Linux, and a ceesafec built from it by cc or GCC behaves the same.
- **Behaviour:** run-pass 159/159, run-fail 40/40, ffi 8/8 under Apple clang 21 (`-O2`, `-O0`, the overflow fallback,
  `-O1` ASan+UBSan with leak checks, `--split`, `-g`, all `-Werror`), clang 18 and GCC 12 on the Ryzen box; TSan on the
  three thread programs. One recorded divergence, the reference's: GCC 12 `-O2 -Werror` reports `-Wuninitialized` in
  an enum constructor of `run-pass/nested-patterns`, for the reference compiler's C too, flat and default.
- **Fault sweep:** 50 run-pass programs allocate; all 789 failure points end as accepted, on macOS and Linux, and each
  run equals the run of the reference compiler's build.
- **Collections:** the 30 programs of `CollectionPropertyTests` (`tools/collection_properties.py`) print their models'
  output under every configuration, their C equals the reference compiler's, and their 156 failure points pass the sweep.
- **Freestanding:** 3/3 host objects run under their harnesses with only allowlisted undefined symbols; both CosmoOS
  triples have no forbidden sections, mutable state or COMMON symbols, also joined by `ld.lld`
  (`tools/freestanding_check.py`), as for the reference compiler.
- **emit-c snapshots:** 7 of 24 match. The 17 others are the reference compiler's default form (structured, placement, check
  elimination, closure specialisation: P7). Reviewed: the reference compiler in the flat, unoptimised form fails the same 17.

### P6 — First verified fixed point

Dependencies: P5 and every feature CeeSafe uses.

- [x] Gen 1 is built by the pinned the reference compiler from CeeSafe's `.cee` sources.
- [x] Gen 1 builds gen 2, and gen 2 builds gen 3, with identical inputs, flags and toolchain.
- [x] Gen 2 and gen 3 emitted C and resources are byte-equal. Both pass the compiler contract tests
  (`tools/contract_tests.py`).
- [x] Repeat from clean directories under different absolute paths. Check that no timestamp or path leaks into output.
- [x] Gen 2 onward builds with no `dotnet` or the reference compiler on `PATH`, and the subprocess logs are audited.

Gate: a reproducible fixed point with archived manifests. This is a checkpoint, not release readiness. **Passed
(2026-10-30, CeeSafe `1b1aae4`, stage 0 the reference compiler 0.6.0)**: `docs/p6-fixed-point.md`, manifests in
`bootstrap/fixed-point/`. On macOS (Apple clang 21) and Linux (GCC 12): C2 = C3 = C4 and gen 2 = gen 3 bit for bit,
in two directories each; the C is the same on both hosts; from gen 1 on only the C compiler runs; every contract test
passes on gen 2 but the recorded divergences.

### P6R — Rename cut-over

Dependencies: P6.

- [x] `ceesafec` accepts `.cee`, `ceesafe.toml` (and `ceesafe.lock`). `$CEESAFE_HOME` is named (Decision 0002); nothing
  reads a home directory until P8's git and registry dependencies. `.cee` and `ceesafe.toml` are kept as an explicit
  legacy mode (Decision 0002), then dropped the same day by Decision 0003: the reference and CeeSafe exist in parallel, so
  ceesafec reads only `ceesafe.toml` packages; files named on the command line may have any extension.
- [x] The diagnostic code letter stays **N** (Decision 0002: `C` would collide with MSVC's codes), so no mapping and
  no corpus conversion is needed.
- [x] One commit (`8941c10`) renames CeeSafe's own sources and manifest; `3d6ac40` is the bootstrap anchor that the reference compiler
  still builds (`tools/bootstrap.sh`). The P6 fixed point reruns with ceesafec alone; the reference compiler is no longer needed for
  routine builds (formatting waits for P8's `fmt`).

Gate: the fixed point holds on CeeSafe names, and the legacy mode (if kept) is tested. **Passed (2026-10-30,
`8941c10`)**: from `tools/bootstrap.sh` (the reference compiler 0.6.0 → `3d6ac40` → this tree), the fixed point with that ceesafec as
stage 0 holds on macOS (Apple clang 21) and Linux (GCC 12): gen 1 = gen 2 = gen 3 = stage 0, the same C on both hosts,
which differs from the legacy-named commit's only in the 80 `src/` file names; only the C compiler runs. Legacy mode:
`compare_packages.py` passes the seven fixtures as they are (`ceesafe.toml`), renamed (`ceesafe.toml`, `--renamed all`)
and mixed (a CeeSafe root over legacy dependencies, `--renamed root`); a directory with both manifests is an error,
and a legacy package ignores `.cee` files. `tools/contract_tests.py` passes on both hosts.

### P7 — Optimisations and production C

Dependencies: P6R. Ownership and drop schedules do not change.

- [x] Port placement and escape analysis, `Box.create` built in place, bounds and overflow check elimination, closure
  specialisation, and every `--explain-*` report (S0–S2).
- [x] Port `CStructurer` (structured C), `--flat-c`, `--split` and `--program`, with deterministic unit names (S3).
- [x] ~~Switch to the approved CeeSafe C prefix and ABI version~~: moved to P8 by Decision 0004 (accepted 2026-10-30),
  so the C stays byte-identical to the reference compiler's while CeeSafe tracks the reference.
- [x] Run each switch on and off (`--no-placement`, `--no-bce`, `--no-oce`, `--no-closure-specialisation`) against the
  optimisation snapshots.
- [x] Rerun the fixed point with production defaults and `--split` (S4).

Gate: optimisation tests pass, the on and off runs agree, and production builds reach the fixed point. Any performance
claim needs a benchmark under `benchmarks/`, run on the Ryzen host.

### P8 — CLI, packages and tools

Dependencies: P7. Parser-only tools may start after P2, and semantic tools after P4.

- [x] Packages (S3):
  - manifest/TOML, path and git dependencies, the lock file and cache;
  - the registry index and version solving, `fetch`/`publish`/`index-check`;
  - `$CEESAFE_HOME`;
  - a one-way conversion of a CeeSafe package to a CeeSafe package (`ceesafe.toml`, `ceesafe.lock`, `.cee`), if wanted:
    ceesafec does not read CeeSafe packages (Decision 0003).

  Local fixture repositories, no network.
- [x] Interpreter: values, memory, numerics, builtins, deterministic threads (`Interpreter/Threads.cs`) and the sandbox (S4; threads cooperative, docs/p8-plan.md).
- [x] Formatter: trivia, recovery, `--check`/`--stdout`, idempotence. Format CeeSafe's own sources (S1).
- [x] Docs and resources: `doc`, `explain`, the embedded specs and guide (S0, S2), and `new`/`agents` with CeeSafe
  templates and markers (S3).
- [x] Fixes and JSON: the exact schema, `fix --dry-run`, and every corpus fix applied (`FixTests`) (S0).
- [x] LSP: framing, UTF-16 edits, hover, definition, symbols, formatting, actions. Explicit walkers replace reflection (S5).
- [x] MCP: lifecycle, tools (`ceesafe_*`), resources and prompts, the sandbox (S5).
- [x] Host parity: Windows/MSVC, debugger scripts, the VS Code extension (S6; git dependencies on Windows wait for
  lib/std's rename and child environments).
- [x] The normal self-build uses package mode, and P6 is rerun through it (S6: both hosts, both forms).

- [x] Decide whether the command line accepts only `.cee` files (Decision 0005, accepted: only `.cee` and `.c` since
  the fork, S7).
- [x] With the fork point, switch to the CeeSafe C prefix and ABI version (Decision 0004: `cee_`/`ceesafe_*`,
  `ceesafe/rt.h`, `CEESAFE_ABI_VERSION`), with a link-collision test: one program linking a library built by the reference compiler and one
  built by ceesafec (S7; `tools/link_collision.py`).
- [x] The fork point (Decision 0003 §3): taken at S7 (`docs/fork-point.md`). CeeSafe's corpus starts there
  (`tests/reference/`), and the reference compiler comparisons are regression tests against the reference compiler 0.6.0's output with CeeSafe's
  names mapped back. CeeSafe's own specification followed P8 (Decision 0006, `docs/spec-plan.md`): the reference's `v0.6.0`
  text forked into `spec/` and `guide/`, the inherited ADRs inherited, and ceesafec embeds it.

Gate: every command and flag in the P0 ledger is implemented, or has an accepted decision. The contract suites pass.
No C# fallback.

### P9 — Harden, release, hand over

Dependencies: P8.

- [ ] Run the full acceptance matrix (§6) on both architectures, hosted and freestanding.
- [ ] Run a recorded 24-hour differential fuzz campaign, with optimisations off and on, flat and structured C,
  closures, `dyn`, threads, async and numeric edges. The reference compiler is retired (Decision 0010), so the
  differential is ceesafec's C against its interpreter (`run --interp`) and each optimisation switch on against
  off. Port the random program generator to CeeSafe.
- [ ] Measure and record compiler throughput and peak memory (`benchmarks/`; the last comparison with the reference
  compiler is P5's: CeeSafe's own C in 1.4 s against 4.1 s). Run it on the Ryzen host with `nice`.
- [x] Release `ceesafec` for the six platforms, with checksums, licences, install scripts and macOS notarisation:
  v0.1.0, 2026-10-31, on the owner's word (`docs/release.md`; every platform's binary built from one C seed and
  checked to emit it); v0.2.0 (2026-10-31, the first CeeSafe-only release); v0.3.2 (2026-11-01, the first from
  ceesafec 0.2.0 as stage 0; the tags v0.3.0 and v0.3.1 failed in the workflow and published nothing).
- [x] Ship a source-release C seed (in the spirit of ADR 0023 option A), verified to build with a C toolchain alone
  (2026-11-01: `ceesafec-<version>-seed.tar.gz`, first published with 0.3.2, built by the workflow's seed-check on
  Ubuntu gcc, macOS clang and Windows cl).
- [ ] Build and run `ceesafec` on CosmoOS userland, on both architectures, including self-compilation.
- [ ] Have the safety passes and `unsafe` adapters reviewed independently when a reviewer is available.
- [x] ~~Optional (Decision 0003): migrate CosmoOS to `ceesafec`~~: done by the owner in CosmoOS's tree. Its
  `scripts/cee-gen.py` generates the kernel's CeeSafe code with a pinned ceesafec (`origin/main` `a3feb689`: 0.3.0-dev;
  the pin to 0.3.2 is CosmoOS's own uncommitted work, 2026-11-01), checked from here by
  `integrations/cosmoos/ports.py`; ADR 0039's self-test `cee-async` boots in it on both architectures.
- [x] Document the point at which CeeSafe becomes the supported compiler, and the frozen C# reference: Decision 0007
  (CeeSafe's own runtime and expectations; v0.2.0, the first CeeSafe-only release), Decision 0009 (stage 0 a CeeSafe
  release) and Decision 0010 (the reference retired, named nowhere but the licence).

Gate: parity, fixed point, CLI parity and release evidence are complete. Hosted CosmoOS support, an outside audit and
publication are reported separately.

### Language track L1 — async/await (ADR 0031)

CeeSafe's own language work after the fork (Decisions 0006, 0007). The design is `docs/async-await-proposal.md`; the
decisions are ADR 0031 and ADRs 0032–0039. Each phase is spec first, and each must keep the fixed point and
`tools/contract_tests.py` green. **Complete (2026-11-01)**: A0–A4 below, released in 0.3.2.

- [x] **Library ownership** (Decision 0008, 2026-10-31): `lib/` is CeeSafe's and `tools/gen_library.py` reads it.
- [x] **A0, specification** (2026-10-31; ADR 0031 accepted and amended §6):
  - ADR 0031;
  - `grammar.md`, `type-system.md` (§32 `suspend`, §36.3, a new §40), `ownership.md`, `lifetimes.md`,
    `intermediate-representation.md`, `c-backend.md`, `profiles.md`;
  - `diagnostics.md` rows N0341–N0345.

  Gate: `spec_check` and `guide_check` are clean, and the owner accepts ADR 0031. Passed 2026-10-31.
- [x] **A1, a single task** (2026-10-31):
  - `async fn`, `await`, `Async.run`;
  - the `suspend` effect and its contexts;
  - frames and flat resume functions;
  - `lib/std/async.cee` (timers, console);
  - the interpreter, and `run_corpus.py --interp`.

  Gate: async run-pass programs give the same result in C and in the interpreter; ASan, UBSan and leak checks are
  clean. Passed 2026-10-31.
- [x] **A2, scopes and cancellation**, in four steps, each closed by the contract suite:
  - [x] **A2a, many tasks** (2026-10-31):
    - async closures;
    - `Async.scope` and `spawn`, with the scope-loan rule (`scopeIn` moves to A2b);
    - a run queue and wakers in the runtime;
    - the interpreter scheduler, which gives each task its own activation stack.
  - [x] **A2b, cancellation** (2026-10-31; `scopeIn` moved to A3, the owner's choice):
    - the cancel flag, `checkpoint`, `isCancelled`, `shield`;
    - `race` and `all`;
    - `CEESAFE_CANCEL_AT` and `tools/cancel_sweep.py`.
  - [x] **A2c, threads** (2026-10-31):
    - `Async.blocking`;
    - channels (ADR 0032, accepted 2026-10-31);
    - wakes from other threads.
  - [x] **A2d, I/O** (2026-10-31):
    - async sockets (`AsyncTcpListener`, `AsyncTcpStream`) on `poll()`, readable and writable;
    - async files (`AsyncFiles`, through `Async.blocking`);
    - `AsyncMutex<T>` (lib/std: `Async` is a built-in type, so the library names it `AsyncMutex`);
    - kqueue and epoll move to A4, as performance work against `poll()`, with the Windows path.

  Gate: the cancellation sweep and the determinism check pass (both on macOS, 2026-10-31: 11 async programs, 76
  points; C and the interpreter print the same), and Linux and macOS runs (Linux: A4a, 2026-11-01). Passed.
- [x] **A3, sizes** (2026-11-01):
  - [x] frames shrunk by liveness; `--explain-frames` (2026-10-31);
  - [x] `Async.boxed` and `Async.scopeIn`, frames from an allocator through generated per-allocator adapters; N0343's
    sure fix (2026-10-31);
  - [x] async interface methods, through type parameters; `dyn` with async methods is N0331 until A4 (2026-11-01);
  - [x] the debugger's `ceesafe-tasks` (lldb and gdb): each async fn's frame information (`ceeAN_`, a line per resume
    state), read by the runtime's `ceesafe_async_tasks()` (2026-11-01).
- [x] **A4, platforms** (test hosts from 2026-11-01: macOS arm64; Debian 12 x86-64 with GCC 12, no clang; Windows 11
  arm64 with MSVC 14.51):
  - [x] A4a, portability (2026-11-01): A2's Linux run (the async runtime needed `_POSIX_C_SOURCE` under glibc);
    ceesafec and the async runtime under MSVC (rt.h's atomics, `cee_max_align_t`); Windows' wake path (an event);
    Winsock sockets behind a table of `i32` ids (the owner's choice), sync and async (`WSAEventSelect` and
    `WSAPoll`); the contract suite's sanitizer steps on GCC where there is no clang. Found on the way: MSVC `/O2`
    miscompiles `run-pass/slice-bulk` (`tests/c-compilers/msvc-o2-store-through-copy.c`).
  - [x] A4b, reactors:
    - [x] kqueue and epoll, armed one-shot at each park; deadlines in a heap, descriptor waiters in a table, wakes in a
      list, the parked list doubly linked; `poll()` kept (`CEESAFE_REACTOR_POLL=1`, and a contract step);
      `benchmarks/async-reactor/run.py` (2026-11-01);
    - [x] IOCP on Windows (the default) and io_uring on Linux (opt-in), over ADR 0035's operation records; a
      cancelled operation waits for the kernel (2026-11-01).
  - [x] A4c, freestanding: an executor hook for the kernel profile and a CosmoOS boot (ADR 0039): the executor over
    four environment hooks, AsyncSignal, and CosmoOS's self-test cee-async on a branch (2026-11-01).
  - [x] A4d, language: `dyn` async methods and closures polymorphic in `suspend` (an ADR each, spec first);
    - [x] a child per loop iteration (ADR 0033 §3.1, the owner's choice "A now, B later"): a child holds its own
      copy of each Copy, unscoped value it reads (2026-11-01);
    - [x] copied borrows and `async move` children (ADR 0033 §3.2–3.3): the borrow checker holds each spawn's
      environment until its scope's wait (2026-11-01).
    - [x] `dyn` async methods (ADR 0036): the awaiter's frame has room for the largest frame among the types coerced
      to the dyn type; `Async.boxed` takes the impl's own size from the table (2026-11-01);
    - [x] closures that may or may not suspend (ADR 0037): an `@effectsOf` function called with an async closure runs
      its async instance, which awaits the closure; lib/core's sortBy is iterative (2026-11-01).
  - [x] A4e, an opt-in `Send` work-stealing executor (ADR 0038): `s.spawnSend` children move to an idle worker of
    `Async.runWorkers(n, …)` before they start (2026-11-01).

  **What is left of A4, in phases** (the owner's "in a multiphased way", 2026-11-01). Each phase is an ADR the owner
  accepts, then the implementation, the contract suite on macOS and Debian and the run families on Windows, a §10
  row, and a commit the owner approves:
  1. [x] **Completion I/O** (A4b; ADR 0035; 2026-11-01): IOCP on Windows and io_uring on Linux, over one library protocol in which a
     cancelled operation is cancelled in the kernel and waited for before its task returns (type-system.md §40.7),
     so a lent buffer outlives the kernel's use of it. Gate: a buffer-lifetime test (cancel a read while the kernel
     holds the buffer, under ASan); `benchmarks/async-reactor` flat on Windows.
  2. [x] **`dyn` async methods** (A4d; ADR 0036; 2026-11-01): an interface with async methods made `dyn` (N0331
     before), a call through the table knowing its frame's size.
  3. [x] **Closures that may or may not suspend** (A4d; ADR 0037; 2026-11-01): `@effectsOf` carrying `suspend`, so
     a higher-order function is async when its argument is.
  4. [x] **Work stealing** (A4e; ADR 0038; 2026-11-01): an opt-in executor for `Send` tasks across threads.
  5. [x] **Freestanding** (A4c; ADR 0039; 2026-11-01): an executor hook for the kernel profile, and a CosmoOS boot
     that runs tasks. Last, since it touches CosmoOS's tree, where the owner's work is in progress.

## 5. Bootstrap protocol

The planned interfaces became these tools (`bootstrap/reference.md`, Decision 0009, `docs/p6-fixed-point.md`):

```text
sh tools/bootstrap_release.sh STAGE0 [REVISION] [OUTPUT]     stage 0 = a released ceesafec (now 0.2.0), through its
                                                           recorded chain of commits and bridges to the tree
python3 tools/fixed_point.py --stage0 out/ceesafec --work <dir> --manifest <file>
                                                           gen 1, 2, 3: C and resources byte-equal, manifest archived
                                                           in bootstrap/fixed-point/
python3 tools/contract_tests.py --ceesafec out/ceesafec     the contract suite on the result
release seed: ceesafec-<version>-seed.tar.gz              builds with a C compiler alone (bootstrap/seed/README.md)
```

The primary artefacts are the generated C and the bundled resources. Binary equality also needs controlled linker,
debug and signing metadata, and is reported separately. Generation equality shows a stable translation. It is not a
proof of correctness or of an uncompromised seed: keep the provenance, the independent safety tests and the C# oracle.

## 6. Acceptance matrix

| Area | Required evidence | First gate |
|---|---|---|
| Support library | independent models, collisions, determinism, OOM | P1 |
| Lexer and parser | AST snapshots, recovery, exact errors, UTF-16 positions | P2 |
| Types, generics, HIR | fixtures, constant edges, HIR snapshots, profiles | P3 |
| Ownership, borrows, effects | every negative, mutations, regions, captures, threads | P4 |
| Drops | post-drop NIR, early exits, partial moves, allocation failures | P4–P5 |
| Native semantics | stdout, stderr, status, panic sites against the corpus's expectations and the interpreter | P5 |
| C backend | snapshots, strict C11 warnings, determinism, FFI, source maps | P5–P7 |
| Self-hosting | gen2 equals gen3, no .NET on `PATH`, CeeSafe names | P6, P6R, P8 |
| Optimisations | each switch on and off, explanation reports, fuzz | P7 |
| Tools and protocols | CLI, packages, JSON and fixes, fmt, doc, LSP, MCP, editor | P8 |
| Portability | macOS, Linux, Windows; arm64 and x64; clang, GCC, MSVC | P9 |
| CosmoOS | C and object ABI checks, kernel integrations, both architectures, hosted compiler | P5–P9 |
| Robustness | sanitizers on the compiler and its programs, OOM sweeps, malformed input, fuzz campaign | ongoing |

Comparison policy:
- Byte equality where a contract promises it (dumps, JSON, protocol streams, gen2 against gen3), and C equal to the
  reviewed emit-c snapshots (until the fork: C equal to the reference compiler's where the backend was unchanged).
- Normalisation is narrow and versioned. It never hides a missing diagnostic, check, drop or panic location.
- A known bug of a toolchain or of a stage 0 gets a reproducer, a spec citation and a separate expected outcome. Expected results are never
  regenerated from CeeSafe to make a comparison pass.
- A skipped configuration is reported as skipped, not as a pass.

## 7. Risks

| Risk | Mitigation | Blocks |
|---|---|---|
| ~~The reference lacks library and host support for a compiler~~ (§2.2) | closed: the N track shipped in 0.4.0–0.4.1 | P1–P5 |
| ~~N-track throughput~~ | closed: the N track has no open item, and since the fork language work is CeeSafe's ADRs | P2–P3 |
| Index aliasing makes table-heavy code awkward | P1 ID/copy-out prototypes; N5 only with evidence | P2–P4 |
| No precedent for a large CeeSafe program (~360 lines today) | P1 probes at scale; measure ceesafec check and build time as `src/` grows | P1 on |
| The stage-0 chain breaks: a commit uses library code it adds, or an old compiler stops building on a new host | bridges recorded in `tools/bootstrap_release.sh`; move stage 0 to a newer release (Decision 0009 §5); the C seed builds with a C compiler alone | releases |
| Stage-0 compiler bug exposed by the tree | reduced reproducer; the fixed point is the check (Decision 0009 §3); move stage 0 past it | releases |
| Numeric or text differences silently change acceptance | exact integers, UTF-16 tests, independent boundary oracles | P3 |
| Translation loses safety or drops | invariant-to-test map, mutations, NIR comparison, sanitizers | P4–P9 |
| Generated C differs without an explanation | deterministic tables and order, reviewed diffs, fixed point | P5–P9 |
| Incomplete rename breaks sources, packages or ABI | rename contract (§3.4), P6R legacy decision, reversible CosmoOS PRs | P6R–P9 |
| Host services drift into C | adapter contracts and a dependency inventory | P1, P8 |
| Servers leak compilations | ownership per request, repeated-edit tests | P8 |
| A hidden C# fallback | builds with `PATH` isolated, subprocess audit | P6, P8 |
| Scope grows past reviewable changes | port pass by pass; gate each phase | all |

A change to language semantics, memory architecture, public compatibility, supported targets or the bootstrap trust
policy needs a decision record: `spec/decisions/` for the language, `docs/decisions/` for the project and its
identity.

## 8. Order

```text
P0 foundation ──> P1 probes + support lib ──> P2 frontend ──> P3 types/HIR ──> P4 NIR + safety
                    │  ▲                         ▲               ▲
                    ▼  │                         │               │
                 N track (upstream the reference M15: N1 strings, N2 maps, N3 generics/From, N4 host, N5?)
P4 ──> P5 flat C ──> P6 fixed point ──> P6R rename ──> P7 optimisations ──> P8 tools ──> P9 release
                                                                             │
                                                                             └──> L1 async/await (A0–A4, complete)
```

## 9. Immediate work queue

Reviewed 2026-11-01, after the 0.3.2 release. Done: P0–P8, the language track L1 (A0–A4), and P9's release and seed
items (ceesafec 0.3.2, published by run 41308563209 with all its jobs green). The N track's upstream items (the
reference #6, `offsetOf`; the owner's N0110 proposal) are closed with the reference's retirement (Decision 0010);
ceesafec's own behaviour is recorded in P4 and `docs/p2-frontend.md` §6.

1. ~~Open `main` for the next version~~: done 2026-11-01, `c_program.cee`'s two constants are `0.4.0-dev` (as after
   0.1.0 and 0.2.0, the extension's `package.json` moves at the release).
2. **Move stage 0 forward** (Decision 0009 §5; `bootstrap/reference.md`, "Changing stage 0"): ceesafec 0.3.2 is the
   newest release and likely builds the tree with an empty chain, which retires the 0.2.0 chain's bridge and its
   `-D_GNU_SOURCE`. Gate: the bootstrap and the fixed point on macOS and Linux, recorded in §10.
3. **The next tag** is the first run of Open VSX trusted publishing (`ovsx publish --trusted-publishing`, §10
   2026-11-01): 0.3.2's `stores` job still skipped Open VSX (no `OVSX_PAT`), and the owner published 0.3.2 there by
   hand.
4. **P9's open items**, in this order:
   - the acceptance matrix (§6) on arm64 and x86-64, hosted and freestanding, with the hosts in §10 (macOS arm64,
     Debian 12 x86-64, Windows 11 arm64; x86-64 Windows and arm64 Linux only in the release workflow so far);
   - the compiler's throughput and peak memory, recorded under `benchmarks/`;
   - the 24-hour differential fuzz campaign (C against the interpreter, each optimisation on against off), after
     porting the generator to CeeSafe;
   - `ceesafec` built and run on CosmoOS userland on both architectures, including self-compilation;
   - an independent review of the safety passes and the `unsafe` adapters, when a reviewer is available.
5. **CosmoOS** (its tree, the owner's and another session's work): the pin moves from 0.3.0-dev to 0.3.2 in
   `scripts/cee-gen.py` (uncommitted there on 2026-11-01). Later: the kernel glue of ADR 0039 keeps one executor slot
   (`kernel/ceesafe/async_selftest.c`); a second kernel thread running an executor needs a per-thread slot (a field
   of `struct thread`) behind `ceesafe_env_slot`.

Progress is measured in completed contracts and gates, with no calendar estimate.

## 10. Execution record

| Date | Work actually performed | Result |
|---|---|---|
| 2026-10-29 | Read-only inspection of the reference and an empty CeeSafe; first roadmap draft; owner chose a full rename | planning only |
| 2026-10-30 | Cross-checked the draft against the reference `07d561f` and the `docs/` specs. Owner decisions recorded as decision 0001. Roadmap rewritten. | Draft found stale: baseline, ADRs to 0027, counts, pipeline citation, and the reference's missing self-hosting prerequisites (§2.2) |
| 2026-10-30 | `the reference compiler new ceesafe --agent claude` (0.3.1) in a scratch directory, copied into `~/dev/CeeSafe`; `git init`; skeleton `src/main.cee`; `bootstrap/reference.md` | done |
| 2026-10-30 | P0 inventory, ledger and runner: `tools/inventory.py`, `docs/reference-inventory.md`, `docs/parity.md`, `tools/run_corpus.py`; the reference compiler built from `07d561f` into a scratch directory for comparison | done; findings in `docs/parity.md` §3 |
| 2026-10-30 | P0 gate on the Ryzen box | passed; P0 complete |
| 2026-10-30 | P1 probes (`probes/`), the report (`docs/p1-probes.md`), and an upstream crash reproducer (`probes/upstream/`) | 12/12 pass; N list revised |
| 2026-10-30 | P1 support library, oracles (`tools/oracles/`), fault sweep, limits, skeleton loading; `run_corpus.py --with` and `--check-leaks`; 4 more upstream reproducers | all pass; P1 gate passed |
| 2026-10-30 | Filed the reference issues #1–#5 (the P1 upstream defects; each reproduced again on `origin/main` 07d561f first) | filed |
| 2026-10-30 | Drafted the reference M15 (`docs/milestone-15.md`, the reference `34ce40a`, pushed) | proposed; awaiting the owner |
| 2026-10-30 | The reference M15 S1–S6 implemented and pushed (the reference `4126457`, CI green); issues #1–#5 closed; ADR 0028 (S7) proposed. Checked against this repository with the reference compiler at `4126457`: probes 11/12 (the 12th, `text-gaps`, expects `StringView.fromUtf8` to be missing), support tests 6/6, fault sweep 573/573 | awaiting a release (0.3.2 / 0.4.0) to re-pin |
| 2026-10-30 | The reference M15 S7 implemented (ADR 0028 accepted; the reference `61cb665`, `1d1f091`, CI green on Linux and Windows; `c3.py` passes in a CosmoOS aarch64 guest): `Process.capture`, `File.kind`, `Environment.variable`, `Directory` | M15 complete; re-pin waits for a release |
| 2026-10-30 | Re-pinned to the reference compiler 0.4.0, then 0.4.1 (`80d8d8c`). 0.4.0 reserved its lib/std helper names (N7), fixed in the reference `24106b0` and released as the point release 0.4.1. All P1 workarounds removed; the four upstream reproducers became regression probes. Gates rerun with 0.4.1: macOS (Apple clang 21) and the Ryzen box (GCC 12; clang 18.1.8 -O2; clang 18.1.8 ASan+UBSan+LSan) | reference corpus at `v0.4.1` 467 pass, 3 partial, no leaks, on both; probes 16/16; support 6/6; fault sweep 573/573; skeleton builds; inventory regenerated, 0 unmapped |
| 2026-10-30 | Re-pinned to the reference compiler 0.5.0 (`af071d4`, ADR 0029) at P3 S2b, before any S2b code. lib/ unchanged, the checker change is S7's (FFI). Gates rerun on macOS (Apple clang 21) only; the Ryzen box keeps 0.4.1 until its next gate run | probes 16/16; support 8/8; type table 1/1 with no leaks; reference corpus at `v0.5.0` 468 pass, 3 partial; parse 483/483 identical; check 99/320 identical (as with 0.4.1); fault sweep 573/573 |
| 2026-10-30 | P2: lexer, diagnostics and JSON, AST tables, parser and `--dump-ast` (`src/syntax/`, `src/diagnostics/`); token-dump oracle from the reference's `Lexer`; `compare_tokens.py`, `compare_parse.py`, `lexer_fuzz.py`, `parser_fuzz.py`, `gen_catalog.py` | gate passed on macOS and Linux (`docs/p2-frontend.md` §4): parse families 68/68 with no reference difference, 540/540 reference files identical, 5,000 mutated and 3,000 random inputs identical, sanitizers clean, `live=0` |
| 2026-10-30 | P3: the type checker (`src/check/`, `src/hir/`), slices S0–S8 (`docs/p3-plan.md`), and package mode for `check` (`src/host/packages.cee`, path dependencies) | P3 gate passed: check-pass 27/27 and the 27 HIR snapshots identical; check programs 265/320, the other 55 differ only by P4's diagnostics or the reference compiler's printer crashes; `ffi`, `freestanding`, `emit-c`, `placement`, `bounds-checks`, `overflow-checks`, `closure-calls`, `run-fail` 83/83; package fixtures 7/7; `ceesafec check` of CeeSafe itself identical to the reference compiler (HIR dump, 35,150 lines); ASan+UBSan clean |
| 2026-10-30 | Re-pinned to the reference compiler 0.6.0 (`93e0a46`, ADR 0030) after P3; `offsetOf`/`callerAddress` ported; every completed gate rerun (`bootstrap/reference.md`) | all pass; one upstream bug found (`offsetOf` before layout is 0) |
| 2026-10-30 | P4: NIR (`src/nir/`: model, printer, builder, move paths, drop elaboration, move/in-place/borrow/effect checkers, `passes.cee`), `build --dump-nir`, diagnostic notes, `tools/compare_nir.py`, `tools/mutation_suite.py`, `docs/p4-evidence.md`; P3 gaps found and fixed (type-parameter flow names, `Box.create`'s `@placed` methods, HIR's dead `isGeneric`) | P4 gate passed: check programs 318/322, the 4 recorded divergences; other families 84/84; NIR 284/284 and CeeSafe's own package identical; mutations 1,152/1,152 against the oracle, as with the reference compiler; `check --dump-hir` identical on 186/186 check-pass and run-pass programs; packages 7/7; `ceesafec check` of CeeSafe clean (0.77 s); ceesafec under ASan+UBSan over 295 check-fail and run-pass programs (`build --dump-nir`) and its own package: no report, `live=0` |
| 2026-10-30 | P5: the flat C backend (`src/backend/`), the runtime embedded (`tools/gen_runtime.py`), the toolchain and `build --emit c|exe|obj` with `-g`, `--split`, `--cc`, `--cflag`, `--ld` (`src/host/toolchain.cee`, `src/host/build.cee`); `tools/compare_c.py`, `tools/fault_injection.py`, `tools/collection_properties.py`, `tools/freestanding_check.py`; P3 gaps fixed (built-in items' `Used` marks, HirProgram's Structs and DynSignatures, InHostedLibrary), one NIR span fixed (an exhaustive match's Unreachable) | P5 gate passed: C identical for 284/284 programs in four forms (flat, split, `-g`, both) and for CeeSafe itself; run-pass 159/159, run-fail 40/40, ffi 8/8 under Apple clang 21, clang 18 and GCC 12 (`-O2`, `-O0`, overflow fallback, ASan+UBSan with leak checks, split, `-g`, `-Werror`) but one GCC report that the reference compiler's C shares; TSan 3/3; fault sweep 789 points on macOS and Linux, equal to the reference compiler's runs; collection properties 30/30 with 156 swept points; freestanding host and both triples clean; emit-c differences reviewed (P7's form); ceesafec built from its own C by GCC emits the same C on Linux |
| 2026-10-30 | P6: `tools/fixed_point.py`, `tools/contract_tests.py`, `bootstrap/fixed-point/` | fixed point on macOS and Linux: C2 = C3 = C4, gen 2 = gen 3 bit for bit, in two directories, the same C on both hosts, only `cc` (and GCC's `as`, `ld`) started from gen 1 on; all contract tests pass on gen 2 but the recorded divergences |
| 2026-10-30 | P6R: Decision 0002 (legacy `ceesafe.toml`/`.cee` kept; codes keep N), `ceesafe.toml`/`.cee`/`ceesafe.lock` packages (`3d6ac40`), CeeSafe's own sources renamed (`8941c10`), `tools/bootstrap.sh`, `compare_packages.py --renamed` | gate passed: fixed point on CeeSafe names from a bootstrapped ceesafec on macOS and Linux, same C on both, only `src/` file names differ from the anchor's C; package fixtures legacy, renamed and mixed 7/7 each; contract tests pass on both hosts |
| 2026-10-30 | P7: closure specialisation (`src/nir/closure_specialisation.cee`), placement (`src/nir/control_flow.cee`, `src/backend/placement.cee`), check elimination (`src/backend/check_elimination.cee`), structured C (`src/backend/c_structurer.cee`), all default on with their switches and `--explain-*` reports; `tools/compare_reports.py`, `tools/compare_self.py`, the contract suite and fixed point in the default form; Decision 0004 (C prefix at P8) | P7 gate passed: C identical to the reference compiler's default form over 308 programs (flat, `--split`, `-g`, both) and for CeeSafe itself; the four reports identical; emit-c 24/24; run corpus under Apple clang 21, clang 18 and GCC 12 (the recorded GCC report aside) and ASan+UBSan; fault sweep 783 points and collections 30/30 on both hosts; fixed point with production defaults and `--split` on macOS and Linux, the same C on both |
| 2026-10-31 | Benchmarks and the first CeeSafe-only C (Decision 0007): `benchmarks/` (16 workloads against Rust, Rust with overflow checks and C; `run.py`), an ablation of ceesafec's C finding the costs; `runtime/` CeeSafe's own (`tools/gen_runtime.py` reads it; the regenerated embedding identical but its header); arena bump allocation inline (`ceesafe_region_alloc_fast`); zeroed boxes through `ceesafe_alloc_zeroed` (`ceeBZ_`, `calloc`); `emit-c` snapshots CeeSafe's own; `src/support/` standalone again (`none()` in `src/support/none.cee`), `tests/support` as `.cee`, `tools/fault_sweep.py` with the `CEESAFE_` variables; `guide/performance.md` | macOS arm64, Apple clang 21, rustc 1.99.1, 5 runs: CeeSafe 0.96× Rust's time (geometric mean; 1.01× before), 0.88× Rust with overflow checks; `arena` 1.37× → 0.95×, `large-objects` 1.26× → 1.02×. Fixed point holds; reference corpus all families pass with leak checks (emit-c 24/24 after the reviewed `arena.c` edit); `tests/support` 8/8 with leak checks; fault sweep 573/573; guide_check 92 examples and spec_check 76 documents, 0 problems; `fmt --check` clean |
| 2026-10-31 | CosmoOS's move to CeeSafe, CeeSafe's side: `integrations/cosmoos/ports.py` runs CosmoOS's own `scripts/cee-gen.py` (its `generated`, `refused`) with the ceesafec under test, and with `--pinned` compares every generated file with that compiler's but for the banner's version; `c2cee.py verify` sets `CEESAFEC` and runs `make cee-check` (`--ceesafec`), and refuses a reference-era tree; `c2cee port`'s `BUILD.md` describes the new layout and its analysis uses the kernel's include path (`out/cee/kernel/include`, `boot/protocol`, `drivers/include`); `spec/tooling.md` §9 and the integration README follow | `ports.py` on CosmoOS's local `ceesafe-migration` (`47964c10`) with ceesafec 0.3.0-dev and `--pinned` 0.2.0: 17 ports, 1 negative source, 28 generated files equal, 0 problems; on `origin/main` (still the reference) refused as intended; `c2cee selftest` PASS; `c2cee port kernel/core/bootinfo.c` drafts (it failed before on `cosmoboot.h`); `verify` refuses a reference-era tree; spec_check and guide_check 0 problems |
| 2026-10-31 | L1 async/await, library ownership and A0 drafts: Decision 0008 (`lib/` recovered from `src/library/sources.cee`, equal to `map_forward` of `tests/reference/lib/` for all 13 files; `tools/gen_library.py` reads `lib/`; `spec_check` documents `lib/core`); ADR 0031 proposed; `docs/async-await-proposal.md`; roadmap track L1. `python3 tools/contract_tests.py --ceesafec out/ceesafec` (macOS arm64, Apple clang 21) | regenerated `sources.cee` differs only in its header and one doc comment; all 13 contract steps ok (fixed point, corpus, -Werror, ASan, support, sweeps, mutations, collections, freestanding, spec, guide, debugger); `fmt --check` clean. ADR 0031 awaits the owner |
| 2026-10-31 | L1 A0: ADR 0031 accepted ("I accept it as written."), then amended (§6: the scope's out-of-memory error is named by the caller, the owner's choice; `checkpoint(cancelled)`; `AsyncError`; `Async.race`/`all`; the `await` operand wording). Spec: `grammar.md`, `type-system.md` §32, §36.3, new §40, `ownership.md` A1–A4, `lifetimes.md` §8, `intermediate-representation.md` §3, `c-backend.md` §2.1, §3a, §6, `diagnostics.md` N0341–N0345 with N0290/N0295, `profiles.md`, `testing.md` §1; `gen_catalog.py`, `gen_docs.py`. `python3 tools/contract_tests.py --ceesafec out/ceesafec` | all 13 steps ok (spec_check: 137 codes explained); `fmt --check` clean. A0 gate passed |
| 2026-10-31 | L1 A1: `async fn`, prefix `await`, `Async.run`, `Async.park` (src/syntax, src/check/async.cee); the `suspend` effect and its contexts (src/types, src/check/effects.cee, src/nir/effect_check.cee); awaited calls marked in NIR and N0343 (src/nir/coroutines.cee); frames, flat resume functions and `Async.run` in C (src/backend/c_async.cee); runtime/hosted/ceesafe_rt_async.c (poll, timers); lib/std/async.cee (`Timer`, `AsyncConsole`, `AsyncError`); the interpreter on a virtual clock; `run_corpus.py --interp`; contract steps `run interp` and `library`; corpus files parse-pass/async-syntax, run-pass/async-basics, async-timers, check-fail/async-tasks, async-recursion, async-effects, emit-c/async-frame; ADR 0031 §6.5 (lowering); `check-pass/effects-of.hir` and `emit-c/slice-bulk.c` updated by reviewed edit (library ids and type order only). `python3 tools/contract_tests.py --ceesafec out/ceesafec` (macOS arm64, Apple clang 21) | all 15 steps ok: fixed point; corpus incl. the 7 async files; run-pass 161 and run-fail 40 in C (-Werror, ASan+UBSan, leaks) and in the interpreter; lib/ diagnostics clean; `fmt --check` clean. Found and fixed on the way: an `as` cast in lib/ that the suppressed library diagnostics hid (hence the `library` step). A1 gate passed |
| 2026-10-31 | L1 A2a: ADR 0032 (channels) accepted; ADR 0031 §6.6 (the scope body is a block, the owner's choice; the scope-loan rule checked on the children's captures). Async closures; `await Async.scope(oom, (s) => …)` and `s.spawn(async () => …)` (src/check/async.cee; N0241, N0344, N0214, N0342); NIR builtins AsyncSpawn and AsyncScopeWait; scope structs, spawn and child thunks in C (src/backend/c_async.cee); a multi-task executor (runtime/hosted/ceesafe_rt_async.c: run queue, parked list, per-task deadlines and descriptors, scopes); the interpreter's scheduler (runTasks: a stack per task, the virtual clock); corpus run-pass/async-scope, check-fail/async-scope; fault_injection.py counts `ceesafe_async_spawn`. `python3 tools/contract_tests.py --ceesafec out/ceesafec`; `python3 tools/fault_injection.py --compiler out/ceesafec --corpus tests/reference/tests/corpus` (macOS arm64, Apple clang 21) | all 15 steps ok (run-pass 162 in C, ASan+UBSan and the interpreter); the fault sweep then 52 allocating programs, 798 failure points, 0 fail, so spawn's out-of-memory path is clean. Found on the way: `await` of a scope was wrapped as an awaited call and dropped in NIR; the interpreter freed a child's environment with its statement (it now copies it) |
| 2026-10-31 | L1 A2b, first part: cancellation (a task's flag, its scope's when a child fails or cannot start, its scope owner's, transitively; a cancellation wakes waiting tasks), `Async.isCancelled`, `Async.checkpoint` (lib/std's `AsyncControl<E>`), `Async.shield` (enter/leave around the awaited body; TimedOut past its deadline; a block body's result defaults to `Result<void, AsyncError>`); lib/std's waits observe cancellation; `CEESAFE_CANCEL_AT`/`CEESAFE_CANCEL_REPORT` and `tools/cancel_sweep.py`, a new contract step; the interpreter mirrors all of it; corpus run-pass/async-cancel, async-shield, check-fail additions; lib/std's helpers moved into `AsyncLeaf` (library top-level names are reserved for every program); timers that come due together run earliest deadline first (a race under load: `async-scope` failed once in a parallel corpus run); effects-of.hir and slice-bulk.c updated again by reviewed edit (numbering and type order only). `python3 tools/contract_tests.py --ceesafec out/ceesafec` | all 16 steps ok: run-pass 164 in C, ASan+UBSan and the interpreter; fault sweep 54 programs, 803 points, 0 fail; cancel sweep 5 programs, 29 points, 0 fail. `race`, `all` and `scopeIn` remain |
| 2026-10-31 | L1 A2b, race and all as specified (the owner's choice, option 1): `await Async.race(a, b)` -> `Result<Raced<T>, E>` and `await Async.all(a, b)` -> `Result<void, E>`; both children's tasks and frames in the awaiting frame (`ceesafe_async_start`, `ceesafe_task_slot`: nothing allocated); race cancels the other child when the first returns, all on an Err (`ceesafe_async_cancel`); lib/std's `Raced<T>`, `AsyncRaceJoin<T, E>.settle`, `AsyncAllJoin<E>.settle` make the value in completion order; NIR AsyncJoin/AsyncJoinResult; the interpreter mirrors it; N0343 counts embedded children; corpus run-pass/async-race (allocation attempts=0), check-fail additions; effects-of.hir updated by reviewed edit (closure numbers only). `python3 tools/contract_tests.py --ceesafec out/ceesafec` | all 16 steps ok after the snapshot update (corpus re-run: check-pass 27/27); run-pass 165 in C, ASan+UBSan and the interpreter; cancel sweep 6 programs, 41 points, 0 fail. `scopeIn` remains |
| 2026-10-31 | L1 A2c, first part: `await Async.blocking(() => …)` (a sync closure on a worker thread; captures by Thread.both's rule and a Send result, N0334; the body's effects are the worker's; not cancelled; inline if no thread starts) and waking from another thread (a task's atomic `woken` flag and its executor's wake pipe, watched by `poll`); async programs link the thread runtime; the interpreter runs the body at once; corpus run-pass/async-blocking, check-fail additions. `python3 tools/contract_tests.py --ceesafec out/ceesafec`; the blocking program 5 times under `--cflag -fsanitize=thread` | all 16 steps ok: run-pass 166 in C, ASan+UBSan and the interpreter; fault sweep 55 programs, 809 points (a worker that cannot start runs inline), 0 fail; cancel sweep 7 programs, 47 points, 0 fail; ThreadSanitizer silent. Channels (ADR 0032) remain |
| 2026-10-31 | L1 A2c, channels (ADR 0032): `Channel<T: Send>` in lib/std/channel.cee (`create`, `clone`, `close`, blocking `send`/`recv`, `await sendAsync`/`recvAsync`, `SendError<T>`), over an element-blind runtime ring (`ceesafe_chan_*`: a spin lock, an epoch, task wait nodes in their frames, a condition variable for threads); the interpreter's model (a blocking wait is a thread wait; a stuck run starts a waiting thread); the cancellation sweep's counter made atomic (a race ThreadSanitizer found); corpus run-pass/async-channel, check-fail/async-channel, async-effects addition; effects-of.hir updated by reviewed edit (closure numbers only). `python3 tools/contract_tests.py --ceesafec out/ceesafec`; the channel program (capacities 2-4) 20 times under `--cflag -fsanitize=thread` and 5 under ASan+UBSan | all 16 steps ok after the snapshot update: run-pass 167 in C, ASan+UBSan and the interpreter; fault sweep 56 programs, 820 points, 0 fail; cancel sweep 8 programs, 58 points, 0 fail; ThreadSanitizer silent after the fix. A2c gate passed |
| 2026-10-31 | L1 A2d: `AsyncMutex<T>` (a one-token channel; the guard's drop puts the token back with `Channel.trySend`, which has no effect); `AsyncTcpListener`/`AsyncTcpStream` on non-blocking descriptors and readable/writable waits (`ceesafe_async_tcp_*` in ceesafe_rt_io.c); `AsyncFiles` through `Async.blocking`; `AsyncSockets`, the raw calls, which the interpreter uses on real descriptors with `ceesafe_async_poll`; the executor's poll takes any number of descriptors; woken tasks run in wake order and a channel wakes its oldest waiter first (C took the lock 1, 3, 2 where the interpreter took 1, 2, 3); a connect is complete only once it has a peer (SO_ERROR is 0 while it is in progress); function-type typedefs of unused library code are no longer emitted (they named undeclared types); kqueue, epoll and Windows move to A4. Corpus run-pass/async-mutex, async-tcp, async-files; effects-of.hir updated by reviewed edit. `python3 tools/contract_tests.py --ceesafec out/ceesafec` | all 16 steps ok: run-pass 170 in C, ASan+UBSan and the interpreter; fault sweep 58 programs, 826 points, 0 fail; cancel sweep 11 programs, 76 points, 0 fail |
| 2026-10-31 | L1 A3, frames: a frame keeps only its parameters, its borrowed locals, the locals live just after a suspension point (backward liveness; StorageDead reads nothing) and what a scope's end reads; the rest are C locals of the resume function. What a task needs only while it waits (an awaited child, a join, a worker) shares one union. ceesafec computes each frame's layout and asserts it in C (`_Static_assert`); `build --explain-frames` reports it, with a new corpus family `frames`. emit-c/async-frame.c updated by reviewed edit (the asserts; `i` a C local). `python3 tools/contract_tests.py --ceesafec out/ceesafec` | all 16 steps ok: every async build's frame asserts hold; run-pass 170 in C, ASan+UBSan and the interpreter; cancel sweep 11 programs, 75 points, 0 fail. A3's `Async.boxed`/`scopeIn`, async interface methods and the debugger's `tasks` remain |
| 2026-11-01 | L1 A3, frames from allocators: `await Async.boxed(alloc, f(…))` -> `Result<T, AllocError>` (the frame from the allocator, a pointer in the waiting storage, not an edge of the await graph, so recursion is allowed) and `Async.scopeIn(alloc, oom, body)` (each child's block from the allocator); generated adapters `ceeAA_`/`ceeAD_` per allocator type over its `allocate`/`deallocate`, or the runtime's heap; N0343's sure fix `Async.boxed(SystemHeap {}, …)!` (`ceesafec fix` applies it, and the fixed program runs); the interpreter calls user allocators as C does; the task slot grows to 128 bytes (its static assert caught the task record outgrowing it). Corpus run-pass/async-boxed (a counting allocator: 3 blocks out, 3 back, in C and the interpreter), check-fail additions. `python3 tools/contract_tests.py --ceesafec out/ceesafec` | all 16 steps ok: run-pass 171 in C, ASan+UBSan and the interpreter; fault sweep 59 programs, 844 points, 0 fail; cancel sweep 12 programs, 74 points, 0 fail |
| 2026-11-01 | L1 A3, the rest: async interface methods (an `async` mismatch between an interface and its implementation is reported; called through a type parameter, each instance awaits the concrete method; `dyn` of an interface with an async method is N0331 until A4), and the debugger's `ceesafe-tasks` (lldb, gdb): each async fn emits `ceeAL_`/`ceeAN_`, its name, file and the line of each resume state; every task carries its record (`ceesafe_async_run`, `_spawn`, `_start` take it); `ceesafe_async_tasks()` lists the thread's tasks as a tree of scopes; `ceesafe-bt` names resume functions. `--split` thunks in the header are `CEE_UNUSED` (async programs with scopes failed `-Werror` with `--split` before). Corpus run-pass/check-fail async-interface; emit-c/async-frame.c updated by reviewed edit (the records, the run's third argument); `tools/debugger_check.py` gains the tasks program. `python3 tools/contract_tests.py --ceesafec out/ceesafec`, then `tools/cancel_sweep.py` after fixing the new test | 15 of 16 steps ok (run-pass 172 in C, ASan+UBSan and the interpreter; fault sweep 59 programs, 844 points; debugger under lldb, tasks included); the cancel sweep failed on run-pass/async-interface, which unwrapped a cancelled `Timer.sleep` with `!`: the test now returns on `Err`, and the sweep rerun gives 13 programs, 79 points, 0 fail. gdb not installed: `ceesafe-tasks` under gdb not run |
| 2026-11-01 | L1 A4a, portability, on three hosts (macOS arm64; Debian 12 x86-64, GCC 12, no clang or gdb; Windows 11 arm64, MSVC 19.51). Linux: glibc hides `clock_gettime` under `-std=c11`, so no async program compiled there (A2's Linux run had not been made); the async runtime defines `_POSIX_C_SOURCE` on Linux. MSVC: the runtime's `__atomic` builtins became rt.h's `ceesafe_atomic_*`, `max_align_t` `cee_max_align_t`; Windows' cross-thread wake is an auto-reset event. Winsock 2 sockets behind a table of `i32` ids from `CEE_SOCKET_BASE` (the owner's choice), sync and async; the executor waits with `WSAEventSelect` on its event and `WSAPoll`; `ceesafe_async_poll` too, for the interpreter. Tools: the sanitizer and sweep steps use GCC where there is no clang; freestanding reports `skip`. New run-pass/tcp-loopback. ceesafec on Debian and Windows built from macOS-emitted C, then by itself. `python3 tools/contract_tests.py --ceesafec out/ceesafec` on macOS and Debian (the Debian tree a scratch commit of the change, for the fixed point); on Windows `tools/run_corpus.py --cc cl --family run-pass --family run-fail` and the async and TCP programs with `--interp` | macOS: all 16 steps ok (run-pass 173 in C, ASan+UBSan and the interpreter; fault sweep 844 points; cancel sweep 13 programs, 79 points). Debian: 15 ok and freestanding skipped (no clang): fixed point; run-pass 173 under GCC's ASan+UBSan; fault sweep 844 points; cancel sweep 13 programs, 77 points; `-Werror` 172 of 173, nested-patterns as recorded; debugger only demangling (no gdb). Windows: async 13 of 13 in C and the interpreter, TCP 2 of 2 in both; run-pass 172 of 173, run-fail 40 of 40: `slice-bulk` is an MSVC `/O2` miscompile (every installed MSVC, ARM64 and x64; `tests/c-compilers/msvc-o2-store-through-copy.c`). Not run on Windows: the sanitizers, sweeps and fixed point |
| 2026-11-01 | L1 A4b, reactors: kqueue (macOS) and epoll (Linux), armed one-shot at each park (kqueue's changes sent with the wait); the executor's deadlines a min-heap, descriptor waiters a table by fd, wakes a list, the parked list doubly linked, so no wait walks the parked tasks; `poll()` kept (`CEESAFE_REACTOR_POLL=1`, a new contract step). Windows: a waiting socket selected once per park for all network events (`ceesafe_socket_select`), fixing two tasks on one socket overwriting each other's selection; deselected at the end of `Async.run`. `benchmarks/async-reactor/run.py`. Found by the benchmark: an expression-bodied race/all child capturing under `await` (`async () => await g(x)`) crashed the compiler (`namesInExpr` had no `Await` arm): fixed. New run-pass async-duplex, async-join-captures. `python3 benchmarks/async-reactor/run.py --ceesafec out/ceesafec` on each host | us per round trip at 10 / 100 / 1000 idle connections: macOS kqueue 26.6 / 27.4 / 28.6, poll() 29.6 / 111 / 1063 (before: 1390 at 1000); Debian epoll 17.6 / 17.4 / 18.1, poll() 21.8 / 48.4 / 350; Windows WSAPoll 28.0 / 97.0 / 3200 (before the per-park selection: 159 / 1191 / 14509; still linear until IOCP) |
| 2026-11-01 | MSVC: ceesafec builds with `/O2 /Oi-`. Plain `/O2` (MSVC 19.44, 19.50, 19.51; ARM64 and x64) miscompiles a store loop through a slice copy followed by a `memmove` into the same array (`run-pass/slice-bulk`; `tests/c-compilers/msvc-o2-store-through-copy.c`, valid C11); `/O1`, `/Ob0`, `/Oi-` and `/d2SSAOptimizer-` are right, `/Oi-` is the documented one. `tools/run_corpus.py --cc cl --family run-pass --family run-fail` on the Windows VM | 176 of 176 run-pass and 40 of 40 run-fail with ceesafec's own flags (slice-bulk included) |
| 2026-11-01 | ADR 0034 accepted ("`[u8; N]` matches `char[N]`") and implemented: an imported struct's `[u8; N]` or `u8` member may be C's `char`, `signed char` or `unsigned char` (a nested `_Generic`); a borrow of a `u8` member converts its address to `uint8_t *` (amended from the accepted N0294: ceesafec cannot tell `char` from `uint8_t` members). Reported by the owner from CosmoOS (`cosmofs_internal.h`'s `char pool_name[…]`). New ffi/c-chars. ADR 0033 accepted ("A now, B later"), §3.1 implemented: a spawned child copies each Copy, unscoped capture it only reads, and every async closure's frame holds its own copy of its environment (else a loop's children shared the last iteration's); §3.2 (copied borrows) and §3.3 (`async move`) wait for the borrow checker to follow what a child holds until its scope returns. New run-pass async-spawn-loop, check-fail async-spawn-copies; the reactor benchmark spawns its holders in a loop. `python3 tools/contract_tests.py --ceesafec out/ceesafec` on macOS and Debian (the Debian tree a scratch commit of the change); on Windows the run families, ffi and the async and TCP programs with `--interp` | macOS: all 17 steps ok (run-pass 176, run-fail 40, ffi 9 under -Werror and ASan+UBSan; fault sweep 61 programs, 850 points; cancel sweep 16 programs, 103 points; the poll reactor step). Debian: 16 ok, freestanding skipped (no clang): fixed point; run-pass 176 under GCC's ASan+UBSan; -Werror 175 of 176 (nested-patterns, recorded); fault sweep 850 points; cancel sweep 16 programs, 92 points. Windows: run-pass 176, run-fail 40; async 16 and TCP 2 in the interpreter; ffi 8 of 9 as checked out with LF, and c-chars builds and runs under MSVC, its snapshot differing only by the CRLF `git apply` gave it on the VM |
| 2026-11-01 | ADR 0033 §3.2–3.3 (the owner's "Go with ADR 0033 §3.2–3.3"). §3.2: a spawned child copies scoped Copy captures too (`borrow T`); the borrow checker makes each spawn's environment a holder, used by its scope's wait and not ended by being made again, so its loans (every iteration's) last until the wait; a new no-op `Builtin(AsyncScopeStart, [site])` at the body's start ends them (run-pass/async-scope's `rounds`, a scope in a loop, found that it was needed). §3.3: `async move () => …` children own their captures: the environment moves into the spawn, the child's frame copy is dropped when it returns (and the original when the spawn fails), in C and the interpreter; it may move a borrow in, which makes its environment scoped (`capturesLoans`). Race and all children may not be `async move` (N0209). The NIR printer names the async builtins (it printed `ThreadBoth` for all of them). New run-pass async-spawn-move, check-fail async-spawn-loans, async-spawn-moved, async-spawn-moves; async-spawn-loop and async-spawn-copies updated; parse-pass/async-syntax's snapshot gains `async move` by reviewed edit. `python3 tools/contract_tests.py --ceesafec out/ceesafec` on macOS and Debian (scratch commit); on Windows the run families, ffi, and the async and TCP programs with `--interp` | macOS: all 17 steps ok (fixed point; run-pass 177, run-fail 40, ffi 9 under -Werror and ASan+UBSan; fault sweep 62 programs, 857 points; cancel sweep 17 programs, 128 points). Debian: 16 ok, freestanding skipped (no clang): fixed point; run-pass 177 under GCC's ASan+UBSan; -Werror 176 of 177 (nested-patterns, recorded); fault sweep 857 points; cancel sweep 17 programs, 97 points. Windows: run-pass 177, run-fail 40, ffi 9 (with LF checkouts: `core.autocrlf false` on the VM); async 17 and TCP 2 in the interpreter |
| 2026-11-01 | L1 A4 phase 1, ADR 0035 (accepted as proposed: the operation record, epoll by default with io_uring opt-in, IOCP by default): lib/std's accept, connect, read and write are loops over `ceesafe_async_op_*` steps on an `AsyncOp` record in the frame; a task cancelled with an operation in flight cancels it in the kernel and waits for its completion, keeping a value completed meanwhile. IOCP (Windows' default): a port per thread, `WSARecv`/`WSASend`/`AcceptEx`/`ConnectEx`, a wait in `GetQueuedCompletionStatusEx`, another thread's wake posted to the port, a socket tied to another thread's port served by readiness. io_uring (Linux, `CEESAFE_REACTOR_URING=1`): raw system calls, a 16384-entry completion queue signalling an eventfd in the epoll set, EBUSY retried after a drain, overflow flushed. Found while measuring: operation waiters on the wake list (every wait scanned them; now `on_op`), ConnectEx's socket tied before it had an id (its later association failed, so it ran by readiness), and a completion-queue overflow when 2000 readers were cancelled at once (the cancellations were dropped; readers waited for ever). The interpreter's steps are readiness. New run-pass/async-cancel-read (the buffer-lifetime program), a contract step "reactor uring" (the async programs on io_uring under ASan+UBSan, Linux only), `benchmarks/async-reactor/run.py` with a column per backend; check-pass/effects-of.hir's closure numbers +10 by reviewed edit (lib/std grew). `python3 tools/contract_tests.py --ceesafec out/ceesafec` on macOS and Debian (scratch commit), the corpus step rerun on Debian with the updated snapshot; on Windows the run families, ffi, the async programs on IOCP, WSAPoll and the interpreter, and the benchmark | macOS: 16 ok, reactor uring skipped (not Linux), corpus ok after the snapshot edit (run-pass 178 under ASan+UBSan; fault sweep 859 points; cancel sweep 18 programs, 111 points). Debian: 16 ok and freestanding skipped (no clang), corpus ok on its rerun; reactor uring 18 of 18 under ASan+UBSan; cancel sweep 18 programs, 101 points; benchmark per round trip at 10/100/1000 idle: epoll 17.7/17.6/19.4 us, io_uring 19.6/19.6/19.6, poll() 21.4/50.9/349.6. Windows: run-pass 178, run-fail 40, ffi 9; async 18 on IOCP, on WSAPoll and in the interpreter; IOCP 53.2/28.0/9.4 us, WSAPoll 75.0/84.4/3437 |
| 2026-11-01 | L1 A4 phase 2, ADR 0036 (accepted as proposed: room for the largest frame, `Async.boxed` sized from the table, `suspend, alloc` without a list): an interface with async methods is dyn-compatible. A call through `dyn` awaits the receiver's method in a frame `ceeAF_dyn_<D…>_m<k>` (its resume adapter, its size, the result, then a union of the method's frames for every type coerced to the dyn type) with the usual `ceeAI_`/`ceeAR_`/`ceeAT_`/`ceeAN_`, so `await`, `Async.run` and `Async.boxed` lower as for a direct call; an async table entry starts `T.m` in that frame and the table holds its size `s<k>` for `Async.boxed`. N0343 follows calls through `dyn` to the coerced types' methods; `--explain-frames` reports each dyn await. The interpreter needed no change. New run-pass/async-dyn, check-fail/async-dyn and async-dyn-frames, frames/async-dyn; check-fail/async-interface no longer expects N0331. `python3 tools/contract_tests.py --ceesafec out/ceesafec` on macOS and Debian, the corpus step rerun on both with the updated async-interface test; on Windows the run families, ffi, check-fail, frames, and the async programs on IOCP, WSAPoll and the interpreter | macOS: 16 ok, reactor uring skipped (not Linux), corpus failed only on async-interface and ok on its rerun (check-fail 148, run-pass 179 with leak checks); cancel sweep 19 programs, 114 points; a `--split` build of async-dyn clean under `-Werror`. Debian: 16 ok and freestanding skipped (no clang), corpus failed only on async-interface and ok on its rerun; reactor uring 19 of 19 under ASan+UBSan; cancel sweep 19 programs, 107 points. Windows: run-pass 179, run-fail 40, ffi 9, frames 2, check-fail 147 of 148 before the async-interface fix; async 19 on IOCP, on WSAPoll and in the interpreter |
| 2026-11-01 | L1 A4 phase 3, ADR 0037 (accepted as proposed: async instances of `@effectsOf` functions, literals and named async fns as arguments, introsort made iterative): an `@effectsOf` parameter of closure type takes an async closure literal or a named async fn (an async adapter), and the call is a task. A NIR pass after drop elaboration (`src/nir/async_instances.cee`) makes one async instance per (function, bound closures), a copied HIR function with `asyncInstanceOf`, in which a call through a bound closure awaits it and passing it on to another `@effectsOf` parameter awaits that function's instance; `Async.boxed` and `v.retain` take instances too. An instance's errors are reported at the call that made it: N0295 (blocking, or a list without `suspend`), N0343 (calling itself with its closure), N0342 (the closure used otherwise). An async fn is no function or closure value elsewhere (N0298), which closes an older hole (its address was null in C). `--explain-frames` reports instances at their calls. lib/core's introsort keeps its waiting ranges in three arrays of 64, shorter side first (the recursion's order of comparisons). New run-pass/async-effects-of, check-fail/async-effects-of and async-effects-of-instances, frames/async-effects-of; closure-calls/known.calls' lib/core line numbers +20 by reviewed edit (introsort grew 20 lines). `python3 tools/contract_tests.py --ceesafec out/ceesafec` on macOS and Debian, the corpus and cancel sweep rerun on both after the run-pass program stopped unwrapping a cancelled sleep; on Windows the run families, ffi, check-pass, check-fail, frames, and the async programs on IOCP, WSAPoll and the interpreter; a `sortBy` timing on macOS | macOS: 16 ok, reactor uring skipped; corpus (closure-calls snapshot) and cancel sweep (the test's unwrap) failed, ok on rerun: cancel sweep 20 programs, 147 points. Debian: 15 ok, freestanding skipped, the same two failures, ok on rerun: corpus clean with leak checks, cancel sweep 20 programs, 140 points, the program on io_uring under ASan+UBSan. Windows: run-pass 180, run-fail 40, ffi 9, check-pass 27, check-fail 150, frames 3; async 20 on IOCP, on WSAPoll and in the interpreter. sortBy of 2 000 000 u32 (15 interleaved runs): 126.4 ms recursive, 128.0 ms iterative (+1.3%; noise 0.1%) |
| 2026-11-01 | L1 A4 phase 4, ADR 0038 (accepted as proposed: children that move only before they start, `Async.runWorkers(n, task)`, the name `spawnSend`): `s.spawnSend(child)` checks its captures by `Thread.both`'s rule (a read capture `Share`, a changed one `Send`, references by their target, no closures), a `Send` error type and the system heap (N0334); `Async.runWorkers(n, task)` starts `n − 1` worker threads for the run, joined when it returns. Runtime: the run's queue of published children (taken oldest first by a worker with nothing ready), shared scopes whose count, first error and waiter change under the run's mutex, an atomic cancellation announced to every worker by an epoch, the parent woken across threads by its last child, non-blocking wake pipes; `ceesafe_scope` 24 → 32 bytes, a task 128 → 144. ThreadSanitizer found each `spawnSend` rewriting the scope's shared flag while workers read it; now only the first does. The interpreter has one worker. New run-pass/async-workers, check-fail/async-workers, a contract step "run tsan" (the async programs under ThreadSanitizer, macOS and Linux), `benchmarks/async-parallel`. `python3 tools/contract_tests.py --ceesafec out/ceesafec` on macOS and Debian; on Windows the run families, ffi, check-pass, check-fail, frames, emit-c, and the async programs on IOCP, WSAPoll and the interpreter; the benchmark on all three | macOS: 17 ok, reactor uring skipped; run tsan 21 of 21; cancel sweep 21 programs, 193 points; fault sweep 959 points. Debian: 17 ok, freestanding skipped; run tsan (GCC) 21 of 21; reactor uring 21 of 21; cancel sweep 187 points. Windows: run-pass 181, run-fail 40, ffi 9, check-pass 27, check-fail 151, frames 3, emit-c 25; async 21 on IOCP, WSAPoll and the interpreter. async-parallel, 32 children (ms at 1/2/4/8 workers): macOS 8 cores 412/194/111/70 (5.89x at 8), Debian 12 cores 292/162/87 (3.36x at 4), Windows VM 4 cores 407/203/109 (3.73x at 4) |
| 2026-11-01 | L1 A4 phase 5, ADR 0039 (accepted as proposed: the executor over environment hooks, freestanding's share of §40 with `AsyncSignal`, CosmoOS on a worktree branch, the pin moved to this tree): `runtime/hosted/ceesafe_rt_async.c` compiled with `CEE_FREESTANDING` keeps the queue, scopes, joins, shields, cancellation and `AsyncSignal` over four hooks (`ceesafe_env_now_ms`, `_idle`, `_wake`, and `_slot`, the fourth because the profile has no TLS), with no global state; lib/core's `async.cee` (AsyncError, Timer, AsyncControl, the joins, AsyncSignal) moved from lib/std; in freestanding `Async.scope`, `blocking`, `runWorkers` and `spawnSend` are N0293 and the `IoError` type is allowed. `AsyncSignal`'s raise only pokes (it may be an interrupt's) and never touches a task that may have returned. Found by the CosmoOS boot test: the executor aligned a child's block to `sizeof(max_align_t)` (32 on x86-64) rather than its alignment (16), which `kmalloc` refuses; now `_Alignof` in every profile. `tools/freestanding_check.py` allows the hooks, Darwin's `bzero`, and a program's own statics. New freestanding/async (+ harness), run-pass/async-signal; check-fail/freestanding-hosted-names updated; check-pass/effects-of.hir closure numbers +8 by reviewed edit. CosmoOS branch `ceesafe-async-boot` (worktree `~/dev/CosmoOS-ceesafe-async`, uncommitted): self-test `cee-async`, its kernel glue, cee-gen and kernel.mk entries, pin 0.3.0-dev. `python3 tools/contract_tests.py --ceesafec out/ceesafec` on macOS and Debian; Windows run families, ffi, check-pass, check-fail, frames, emit-c, async on IOCP, WSAPoll and the interpreter; CosmoOS `gmake test` and `gmake ARCH=aarch64 test` | macOS: 18 ok (freestanding 4 programs, host and 2 triples with ld.lld), reactor uring skipped; cancel sweep 22 programs, 200 points. Debian: 18 ok, freestanding skipped (no clang); cancel sweep 194 points. Windows: run-pass 182, run-fail 40, ffi 9, check-pass 27, check-fail 151, frames 3, emit-c 25; async 22 on IOCP, WSAPoll and the interpreter. CosmoOS: x86_64 `SELFTEST: cee-async ... ok (28 ms)`, `SELFTEST: PASS (525 tests)`, boot-test PASS; aarch64 `cee-async ... ok (26 ms)`, 525 tests, PASS |
| 2026-11-01 | P9, release 0.3.0 with a C seed (the owner's choice after L1): the version 0.3.0 (`c_program.cee`, the extension's `package.json`); `tools/seed_archive.sh` packs the seed job's C as `ceesafec-<version>-seed.tar.gz` with `bootstrap/seed/README.md` (build with `cc` or `cl` alone; check by emitting the seed from the tag) and the licences; `.github/workflows/release.yml` gains `seed-check` (Ubuntu gcc, macOS clang, Windows cl: the archive alone, its README's commands, `--version`, and the seed emitted again), publishes the archive in `SHA256SUMS`, and builds generation 1 under `cl` with `/Oi-` as ceesafec's driver does. The seed's README commands run by hand on a test archive, before the version change: macOS clang, Debian gcc 12, Windows 11 ARM64 cl, each building from the archive alone and emitting it again from the tree; `python3 tools/contract_tests.py --ceesafec out/ceesafec` on the 0.3.0 commit | seed: macOS reproduces, Debian gcc reproduces, Windows cl no differences (a first macOS-made archive carried AppleDouble files; the script now writes none). macOS contract suite: 18 ok, reactor uring skipped; cancel sweep 22 programs, 203 points |
| 2026-11-01 | P9, release 0.3.1, Decision 0009 (the owner: "Bootstrap from 0.2.0"): the v0.3.0 tag's run (41302947031) failed in the seed job's first step, the reference compiler 0.6.0's download returning 404 since the reference's repository became private, and published nothing. The release's stage 0 is now ceesafec 0.2.0 from this repository's release (hash from its `SHA256SUMS`); `tools/bootstrap_release.sh` builds the tree through the chain recorded for 0.2.0: 7d2a041 (the newest commit 0.2.0 builds), then 7d2a041 with 291246d's `src/library` (a bridge: 291246d uses the `AsyncSockets` it adds), then the tree, with `-D_GNU_SOURCE` on Linux for the chain's pre-844e755 runtime. `release.yml`, `docs/release.md`, `bootstrap/reference.md` ("Release stage 0") follow; the version is 0.3.1 (`c_program.cee`, the extension's `package.json`), the compiler 0.3.0's otherwise. | From 0.2.0 at 70da046: macOS arm64 and Debian 12 x86-64 reach the fixed point, seed `main.c` `d1ba9faf…`, identical to the C of the reference compiler-bootstrapped 0.3.0 (`out/ceesafec`). Windows 11 ARM64 (`cl`, the seed README's command): the 0.3.1 seed archive builds `ceesafec 0.3.1`, which emits the seed again from the tree. `python3 tools/contract_tests.py --ceesafec out/ceesafec` at 0.3.1 on macOS: 18 ok, reactor uring skipped; fault sweep 964 points, cancel sweep 208 points, 0 fail |
| 2026-11-01 | P9, release 0.3.2: v0.3.1's run (41306957697) built the seed, its three checks and all six platforms from ceesafec 0.2.0, then failed in the `extension` job's packaging: `vpx`, which the extension's `package` script ran, comes with Vite+'s standalone installer and not with the npm package the workflow installs, so nothing was published. The script runs `vp dlx @vscode/vsce` instead; the version is 0.3.2. | `vp run package` with no `vpx` on `PATH` writes the `.vsix`; `python3 tools/contract_tests.py --ceesafec out/ceesafec` at 0.3.2 on macOS: 18 ok, reactor uring skipped |
| 2026-11-01 | Decision 0010 (the owner: remove every mention of the retired predecessor, "Everything but licence"): the corpus's 107 snapshots that were compared through the name mapping replaced by ceesafec's own output, each only where the mapped output equalled the snapshot byte for byte, and the mapping (`tools/names.py`) removed from `run_corpus.py`, `fault_injection.py`, `collection_properties.py`, `freestanding_check.py`, `mutation_suite.py` and `gen_docs.py`; the `compare_*` tools, fork generators, inventory, `tools/bootstrap.sh`, the oracles built from its source, `tests/reference/lib` and `tests/reference/integrations` deleted; 132 fixtures renamed to `.cee`, `ceesafe.toml` and `ceesafe.lock`; the compiler's old-manifest message and its identifiers renamed; `AGENTS.md` from `ceesafec agents`, `.mcp.json` serving `out/ceesafec mcp`; the record's mentions rewritten as "the reference" (the inherited ADRs say so in their header), dead links made text; corpus test data spelling the name now spells `safe` (same length), the two expectations computed from the bytes (an FNV-1a hash, a byte sum) recomputed in Python; `tools/spec_check.py`'s names rule now checks every tracked file's path and text, `COPYING.RUNTIME` alone excepted | `run_corpus.py` on the corpus before and after removing the mapping: 519 files, every family as before (freestanding partial as before). `python3 tools/contract_tests.py --ceesafec out/ceesafec` at 0.3.2 on macOS: 18 ok, reactor uring skipped (a first run caught a byte sum in `string-views`, then recomputed; then the guard caught this decision's own text) |
| 2026-11-01 | Open VSX: the namespace `ceesafe`'s ownership claim (EclipseFdn/open-vsx.org#15060) approved; `ceesafe.ceesafe-language` 0.3.2 published there by the owner with an access token (a trusted publisher needs an existing extension), then its trusted publisher registered (GitHub, `vkuttyp/CeeSafe`, `release.yml`, no environment). The `stores` job publishes to Open VSX with `ovsx publish --trusted-publishing` (ovsx 1.3.0) and no secret; `OVSX_PAT` and its skip are gone (`docs/release.md`) | open-vsx.org API: 0.3.2, published by `vkuttyp`, namespace verified; the registration listed on the namespace's settings page. The next tag is the first workflow run to use it |
| 2026-11-01 | CosmoOS integration checks C1 and C8, brought up to the async work (both failed before Decision 0010, as found by running them at 384f3ce): the hosted I/O runtime builds in CosmoOS userland (`CEE_COSMOOS`), where there is no `fcntl` and no `getpeername`, so opening an async socket fails with `ENOSYS` (`spec/profiles.md` §2.2), and `EWOULDBLOCK` is used only where the host defines it; `c1.py --corpus` skips the 22 async programs (no executor in CosmoOS userland yet: open work) and matches a process by the 31 characters the kernel keeps of its name; C8's binding of `struct timer` is `KernelTimer` (lib/core's `Timer` took the name), and its host test makes `object.cee`'s C, CosmoOS having ported `object.c` | CosmoOS `25742b40`: `c1.py --arch x86_64` and `--arch aarch64`, each also with `--corpus`: every check PASS, 200 corpus programs in the guest; `c8.py --arch x86_64`: negative cases, module built, signed, loaded at boot, timer and thread, 525 self-tests PASS; its host test still fails (c7's host shim against CosmoOS's kernel `signal.h`). `python3 tools/contract_tests.py --ceesafec out/ceesafec` on macOS: 18 ok, reactor uring skipped |
| 2026-11-01 | ADR 0040 (the owner: "tackle the open works"): the hosted executor in CosmoOS userland (`CEE_COSMOOS`), with no reactor: with no task ready it waits on its own `cosmo_cond_t`, timed to the earliest deadline, and a wake sets a pending flag and signals it (replacing the wake pipe); a task parked on a descriptor counts as ready, as consoles and files do on Windows, and `ceesafe_async_poll`/`_write` work without readiness; channels use `cosmo_mutex_t`/`cosmo_cond_t`. Async sockets stay `ENOSYS`; `spec/profiles.md` §2.2 says so. `c1.py`/`c2.py --corpus` skip only the three async socket programs | `c1.py --corpus` against CosmoOS 25742b40, x86_64 and aarch64: 219 programs in the guest (19 async), every check PASS; `c2.py --arch x86_64`: 219 programs and the guest fault-injection sweep (54 programs, 494 runs) PASS; the runtime compiled with CosmoOS's `USER_CFLAGS` (-Werror) clean; on macOS the 22 async programs with leak checks, under TSan and with `CEESAFE_REACTOR_POLL=1`; `python3 tools/contract_tests.py --ceesafec out/ceesafec`: 18 ok, reactor uring skipped |
| 2026-11-01 | CosmoOS integration checks after CosmoOS's own CeeSafe ports: c7's host shim declares `malloc`/`free` rather than including `<stdlib.h>` (whose host `signal.h` clashed with CosmoOS's through the generated mutex types), and follows the generated `struct quiesce_head` (`callback`) and `struct mutex` (`char *name`); c7 and c8 build `object.cee`'s C for their host tests (`c1.object_sources`); c6 and c9 take a ported component's C original from the newest hand-written version in CosmoOS's history (`c6.c_original`: fbvalid at 51880ef2, gpt at 64765a72) and leave out the runtime files the tree generates; k3 uses CosmoOS's own `MUTEX_INIT_AT` (its copy, `k3/mutex_init_at.h`, deleted) | Against CosmoOS 25742b40: c2 (x86_64) 4/4, c3 (x86_64) 5/5, c4 and c5 on x86_64 and aarch64 9/9 and 17/17, c6 14/14, c7 32/32 with boots, c8 12/12 with boots, c9 8/8, k3 37/37, `ports.py` 28 ports 0 problems |
| 2026-11-01 | Release v0.3.2: the tag's run (41308563209, 11 min) with stage 0 ceesafec 0.2.0 | every job succeeded: version, seed, seed-check on Ubuntu, macOS and Windows, the six platform builds, extension, publish, stores. Published at github.com/vkuttyp/CeeSafe/releases/tag/v0.3.2 (2026-11-01 13:03 UTC): six archives, `ceesafec-0.3.2-seed.tar.gz`, `ceesafe-language-0.3.2.vsix`, `SHA256SUMS`. The Marketplace published `ceesafe.ceesafe-language` 0.3.2 by OIDC (its first trusted publish); Open VSX skipped (no `OVSX_PAT`; trusted publishing came after the tag) |
| 2026-11-01 | ROADMAP review after 0.3.2 and Decision 0010: the status and identity notes, §3's rules and layout, §3.4's CosmoOS row, P9 (releases 0.2.0 and 0.3.2, the seed, the fuzz campaign and throughput without the reference compiler, CosmoOS's migration and the supported-compiler point marked done by Decisions 0007, 0009, 0010), L1 marked complete, §5's bootstrap as built, §6's comparison policy, §7's risks after the N track, §8, §9 rewritten; grammar slips of the rename fixed | documentation only; facts checked against `gh run view`, `gh release view`, `bootstrap/reference.md`, Decisions 0009–0010 and CosmoOS's `git log` |
| 2026-11-01 | ADR 0041 (the owner: "Start with the ADR", then "I accept ADR 0041 as proposed"), after a survey of CosmoOS's network stack for roadmap 2.0 SA6: C interop for porting stateful subsystems. (1) Atomic fields: an imported struct's `uint32_t`/`uint64_t`/`size_t` member may be declared `AtomicU32`/`AtomicU64`/`AtomicUSize`, and a `@repr(C)` struct's atomic field is its integer in `ceesafe_exports.h`; the atomics gain `fetchOr`/`fetchAnd` (and `…With`), in the runtime for GCC/Clang and MSVC and in the interpreter; `AtomicBool` is excluded (a `u32`; C's `bool` is a byte). (2) Imported unions: `union { … }` inside an imported struct (a C11 anonymous member) and `union name { … }` in the block; every member of one union is one place for the borrow checker; members must be any-bit-pattern types (N0346). (3) Variadic C functions: `...` ends an extern's parameters; calls are unsafe, further arguments only of types C's promotions leave unchanged (N0347); called directly, with no wrapper, in units that disable C's format-literal warnings. (4) lib/core's `TextWriter` (snprintf semantics, no allocation) and lib/kernel's `KernelLog` (`klog(level, "%s", line)`, `panic("%s", line)`). Spec §28.2, §28.5, §31.1–31.2, §38.3, grammar, diagnostics; ADR 0041 §7 records the implementation. Snapshots re-recorded: `check-pass/effects-of.hir`'s closure-environment ids moved (lib/core's new functions), nothing else; `kernel-library.hir` gains the new log function and lib/kernel's constant; `emit-c/static-here.c` gains lib/kernel's two headers | corpus: `ffi/atomic-fields` (shared with C both ways; ASan/UBSan and TSan), `ffi/unions`, `ffi/variadic` (`-Werror -Wformat=2`, ASan/UBSan), `run-pass/atomic-bitwise`, `run-pass/text-writer` (expected lines from a Python model; native, interpreter, leak check), `check-fail/ffi-atomic-fields`, `ffi-union-members`, `ffi-union-borrows`, `ffi-variadic`, `parse-fail/extern-unions`, `variadic-outside-extern`; `integrations/cosmoos/k3.py --arch both` against CosmoOS e4065ef9: 37 PASS (klog and panic resolved from its headers under its kernel flags; the new atomics in `k3/atomics.cee`); `ports.py --pinned 0.3.2`: 17 generated files differ, each explained (lib/core line numbers in panic locations, renumbered closure environments, one typedef order, ADR 0040's runtime, rt.h's new operations, lib/kernel's two new headers; no port declares `klog` or `panic`); ceesafec 0.3.2 builds the tree at once and reaches the fixed point with the same C as `out/ceesafec`; `python3 tools/contract_tests.py --ceesafec out/ceesafec`: 18 ok, reactor uring skipped (corpus after the snapshots above) |
| 2026-11-01 | Release v0.4.0 (ADR 0041 §5.4): the version set to 0.4.0 in `src/backend/c_program.cee` and `editors/vscode/package.json`; stage 0 stays ceesafec 0.3.2, which builds the tree at once | `python3 tools/contract_tests.py --ceesafec out/ceesafec` with ceesafec 0.4.0: 18 ok, reactor uring skipped; the tag's run 41335261923 built and checked the seed and all six platforms and the extension, but GitHub cancelled the seed-check on `macos-14` (a scheduled brownout before that image's retirement on 2026-11-02), so nothing was published |
| 2026-11-01 | Release v0.4.1: v0.4.0 with the seed-check on `macos-15` instead of the retiring `macos-14` | the tag's run 41336452370 (8 min): every job succeeded, the seed-checks on Ubuntu, macOS 15 and Windows, the six platform builds, the extension, publish and both stores; the downloaded archives match SHA256SUMS and the osx-arm64 binary reports `ceesafec 0.4.1` |
| 2026-11-01 | ADR 0042 (the owner: "I accept ADR 0042 as proposed"), found porting CosmoOS's `loopback.c` for SA6: statics that are C state. A `static mut` may have an imported C type; in its initialiser an imported struct's literal is C's designated initialiser (unnamed members zero; a `[u8; N]` member takes a string of at most N bytes, N0338 beyond), and the static initialisers gain C function addresses (extern, `@export`, `@callback`), `borrow S as *T` of any static or a field or element of one, and null C function and raw pointers. A plain `static` of an imported type is N0337 with a hint; an imported literal elsewhere stays N0294; another static's value in an initialiser is N0338. Lowering: designated initialisers over C member names, `((T)&ceeS_…)` addresses, every static declared first by a tentative definition when an initialiser holds an address, and a `@callback` named only in a static's initialiser gets its `ceeW_` function. Spec §29.5, §31.2, §39.3–§39.5; ADR 0042 §7 | corpus: `ffi/c-statics` (C registers a CeeSafe static device and calls through its CeeSafe ops table; plain, `-Werror -Wall` with ASan/UBSan, and `--split`), `check-fail/static-c-state`; CosmoOS's `loopback.cee` with this compiler gives test_loopback's C digest; `python3 tools/contract_tests.py --ceesafec out/ceesafec`: 18 ok, reactor uring skipped |
| 2026-11-01 | Release v0.5.0 (ADR 0042 §5.4): the version set to 0.5.0 in `src/backend/c_program.cee` and `editors/vscode/package.json`; stage 0 stays ceesafec 0.3.2 | 0.3.2 and 0.4.1 each build the tree at once to the same C as ceesafec 0.5.0, which reaches the fixed point; the contract suite on the ADR 0042 commit: 18 ok; the tag's run 41347093879 (8 min): every job succeeded, the seed-checks on Ubuntu, macOS 15 and Windows, the six platform builds, the extension, publish and both stores; the downloaded archives match SHA256SUMS and the osx-arm64 binary reports `ceesafec 0.5.0` |
| 2026-11-02 | A review found async programs failing to build on Ubuntu: its gcc defines `_FORTIFY_SOURCE` at -O2, glibc marks `read` warn_unused_result, and gcc does not take a `(void)` cast as a use, so the four drains of the wake pipe and the io_uring eventfd in `runtime/hosted/ceesafe_rt_async.c` failed `-Werror`. They now go through `cee_drain`, which keeps the result and drops it. The contract suite gains `run fortify` (Linux): the run families under gcc `-O2 -D_FORTIFY_SOURCE=2 -Wall -Werror` (testing.md) | reproduced on Debian 12 (gcc 12.2) with `-O2 -D_FORTIFY_SOURCE=2 -Werror`: ceesafec 0.5.0 fails 22 async run-pass programs, the fix passes them; the contract suite on Debian: every step ok but the fixed point and spec checks, which need a git checkout (the copy was a tarball); on macOS: every step ok |
| 2026-11-02 | The same build of the corpus found gcc 12 reporting a variant constructor's `r` used uninitialised (`-Wuninitialized` at `-O2 -Wall`) once inlined into a caller whose argument has an enum payload (`run-pass/nested-patterns`), where clang 14 is silent. A constructor (`ceeMk_…`) is now one compound literal, `return (T){ .tag = …, .u.v_X = { … } };`, the rest zero as C initialisers leave it. Measured on the compiler's own C (gcc 12, `-O2 -Wall -D_FORTIFY_SOURCE=2`): the old form 0 warnings there, a fill then stores 1 (`-Wmaybe-uninitialized`), the compound literal 0, and it alone passes the corpus. Seven `emit-c` snapshots re-recorded, each checked to change only its constructors | the corpus under gcc `-O2 -D_FORTIFY_SOURCE=2 -Wall -Werror` on Debian: run-pass 184, run-fail 40, ffi 13, all pass; contract suite as the previous row |
| 2026-11-02 | The owner: the site had not been updated with async/await. Publishing worked (docs.yml deploys on every push to guide/ or spec/); the guide had no chapter on it, so the site did not lead to it, and three comparison pages still said it did not exist. New `guide/async.md` (in the sidebar after Threads): async fns and `await`, scopes, `race`, cancellation, the library's waiting operations, how and where tasks run; the C#/Java/TypeScript, Go and Zig pages corrected; the README gains structured async, threads and the C interop of ADRs 0041–0042 | `tools/guide_check.py`: 97 examples (five new, each run in the interpreter with its output), 0 problems; the site builds with no dead links; `spec_check`: 0 problems |
| 2026-11-02 | Release v0.5.1 (the owner: "push it and release 0.5.1"): 0.5.0 with the runtime's drains for Ubuntu's gcc and the variant constructors as compound literals; stage 0 stays ceesafec 0.3.2 | 0.3.2 builds the tree at once to the same C as ceesafec 0.5.1, which reaches the fixed point; the contract suites of the two fixes' rows; the tag's run 41352419377: every job succeeded, the seed-checks on Ubuntu, macOS 15 and Windows, the six platform builds, the extension, publish and both stores; the downloaded archives match SHA256SUMS and the osx-arm64 binary reports `ceesafec 0.5.1`; docs run on 986916e succeeded, and the site serves `/guide/async` |
| 2026-11-02 | ADR 0043 (the owner: "Yes, I accept your recommendations"), found porting CosmoOS's `netif.c` for SA6: exports that exist only in some builds. `@cfg("NAME")` on an `@export` function wraps its C prototype, its definition (one unit or `--split`'s instances) and its line in `ceesafe_exports.h` in `#if NAME` … `#endif`; the CeeSafe function is checked and callable in every build. `@cfg` elsewhere is N0290, a name that is not a C identifier N0292. The existing `n as extern "C" fn(…)` from a `usize` is now stated (§29.5). Spec §28.1, §28.4, §29.5; ADR 0043 §7 | corpus: `ffi/cfg-exports` (the C harness built with the macro set and unset), `check-fail/cfg-attribute`; `python3 tools/contract_tests.py --ceesafec out/ceesafec`: 18 ok, reactor uring skipped (macOS); on Debian 12 every step ok but `fixed point` and `spec`, which need a git checkout (the tree was copied as a tarball) |
| 2026-11-02 | Release v0.6.0 (ADR 0043 §5.3): the version set to 0.6.0 in `src/backend/c_program.cee` and `editors/vscode/package.json`; stage 0 stays ceesafec 0.3.2 | 0.3.2 builds the tree at once, and that compiler's C is ceesafec 0.6.0's, which reaches the fixed point; the contract suites of the ADR 0043 row; CosmoOS's `netif.cee` built with it reproduces test_netif's C digest 0x83fa866653f4b673 |
| 2026-11-02 | ADR 0044 (the owner: "Yes, I accept the ADR each with your recommendation"), for CeeSafe.Web (its upstream items U1, U2): eight IoError kinds after `Other` (ConnectionReset, ConnectionAborted, NotConnected, TimedOut, HostUnreachable, NetworkUnreachable, AddressNotAvailable, Unsupported), mapped from errno and Winsock, glibc's wording; lib/std's `SocketAddress` (V4, V6 with scope), `SocketAddresses`' parsers and RFC 5952 text, `ListenOptions` (backlog, SOMAXCONN by default; reuse address and port; v6Only set explicitly), `AsyncTcpListener.bindWith`/`localAddress`, `AsyncTcpStream.connectTo`, the setters (no delay, keep-alive, buffers, linger), `peerAddress`/`localAddress`, `shutdownRead`; the blocking `TcpListener`/`TcpStream` take IPv6 text and gain the setters and `shutdownRead`. The runtime's one endpoint form on POSIX, CosmoOS and Winsock; AcceptEx's buffer for IPv6 in a union, so the operation record stays 192 bytes. The bootstrap chain from 0.3.2 is `f7a7d57`, `93c433f` (the interpreter uses library calls and built-ins those commits add). Spec §26.2, §26.4, §40.4, c-backend.md §6; ADR 0044 §7 | corpus: run-pass io-error-network-kinds (messages from Linux's `os.strerror`), socket-address-text (49 texts, Python's `ipaddress` the oracle: `tools/oracles/gen_socket_address_cases.py`), async-tcp-ipv6, async-tcp-backlog (100 connects before any accept), async-tcp-reset (SO_LINGER 0 read as ConnectionReset), tcp-ipv6-options; ffi/socket-options (each option read back by `getsockopt`); check-fail io-error-old-kinds (N0256). `python3 tools/contract_tests.py --ceesafec out/ceesafec`: macOS 18 ok (cancel sweep: 25 async programs, 319 points), reactor uring skipped; Debian 12 after `tools/bootstrap_release.sh` from the released 0.3.2 through the chain: every step ok, the reactor uring among them, once socket-options read the linger time only while it is on (Linux keeps the last one). Windows 11 ARM64 (MSVC, the compiler built from its emitted C): the six socket programs and the messages' under IOCP and `CEESAFE_REACTOR_POLL`, and socket-options compiled by hand, all as expected; `ceesafec build` with a `.c` input fails there before compiling ("cannot write the C: Invalid argument", ffi/callbacks too): not this change's, not yet investigated |
| 2026-11-02 | ADR 0045 and ADR 0046 (the owner: "Yes, I accept each and every ADR's questions"), for CeeSafe.Web (its upstream items U3, U4, U6's record, T1, and bugs B1–B3). ADR 0046: `Async.timeout(ms, timedOut, body)` and `Async.deadline(at, timedOut, body)`, lowered to `race`'s join with lib/core's `AsyncTimeoutJoin<T, E>.until` as the second child; the join learns a function child with an argument and a second result of its own type. ADR 0045: lib/std's `IoParts<region r>` (eight parts, built by value), `writeVectored`, `writeAllVectored`, `AsyncSockets.sendVectored`; the runtime's 64-word message record (`ceesafe_async_msg_op`, the operation header first) with room for U6's datagrams; `sendmsg`/`writev`/`WSASend`/`IORING_OP_SENDMSG`. T1: `cancel_sweep.py` and `fault_sweep.py --package DIR`. Bugs: B3 (a mutable slice of a captured array left the capture shared: a const pointer in C), B2 (a package struct's library field types and drops not emitted), B1 (async instances only for sync `@effectsOf` callees: a null call for an async one), B4 (kqueue: a cancellation's walk of the parked tasks followed the ready list after a full change queue's flush readied its next task, spinning for ever), and the interpreter's errno numbers on Windows (ADR 0044's kinds read as `Other` there). Spec §32.5, §40.4, §40.7, intermediate-representation.md, c-backend.md §3a and §6, testing.md §1; ADR 0045 §7, ADR 0046 §7 | corpus: run-pass async-timeout (13 cases), async-tcp-vectored (Python oracle: `tools/oracles/gen_vectored_cases.py`, each layout's bytes and FNV-1a), async-tcp-vectored-reset, async-cancel-sendv (cancelled in flight; the peer reads an exact prefix), async-race-mut-capture, unused-socket-holder, async-effects-of-async, async-wake-waiters (hung 5 of 5 runs before the fix); check-fail async-timeout; check-pass async-timeout.hir (new); effects-of.hir: closure ids only (checked with ids normalised). `python3 tools/contract_tests.py --ceesafec out/ceesafec` on macOS (the compiler built by itself): every step ok but the new snapshot's ids, refreshed; cancel sweep 30 async programs, 570 points; fault sweep 1123 points. Debian 12 (the compiler built from its emitted C with gcc), the tree at ADR 0045: every step ok, the reactor uring among them; the corpus again at the B4 fix, and `tools/bootstrap_release.sh` from the released 0.3.2 through the chain to it: ok. Windows 11 ARM64 (MSVC, the compiler built from its emitted C): async-timeout, async-tcp-vectored, async-cancel-sendv, async-tcp-vectored-reset, async-race-mut-capture, async-tcp, async-tcp-reset under IOCP, `CEESAFE_REACTOR_POLL` and the interpreter, all as expected, once async-cancel-sendv kept sending (Windows' loopback took the whole megabyte) and the interpreter knew MSVC's errno numbers |
| 2026-11-02 | Release v0.7.0 (ADR 0044 §5.5): ADR 0044's addresses, IPv6, options and network error kinds, ADR 0045's vectored sends, ADR 0046's `Async.timeout`, T1 and bugs B1–B4, for CeeSafe.Web; the version set to 0.7.0 in `src/backend/c_program.cee` and `editors/vscode/package.json`; stage 0 stays ceesafec 0.3.2, through the chain `f7a7d57`, `93c433f` | the contract suites of the two rows above; `tools/bootstrap_release.sh` from the released 0.3.2 on Debian 12 reaches the tree |
| 2026-11-02 | ADR 0047 and ADR 0048 (the owner: "Yes, I accept both ADRs with your recommendations"), for CeeSafe.Web (U8, U5), and bugs B5, B6 found by it. ADR 0047: `links`, `lib-dirs`, `include-dirs` in `[package]` and `[target.linux|macos|windows]`; `$NAME` in directories; every package's libraries on the executable's link, dependents first; a package with include directories has its own C compiled apart; `--emit c`/`obj` write the link line. ADR 0048: `AsyncDns.resolve` on at most four detached resolver threads, cancellable at once, errors as `IoError` kinds; the interpreter knows literals and `localhost`. B5: a cancellation wakes only the tasks it reaches (it woke every parked socket waiter). B6: `Async.timeout`/`deadline` over `Result<void, E>` (lib/core's settle bound a void payload; the printers crashed on its library-file diagnostic). Also on Windows: a `.c` input's name stops at `\` (`build` with a `.c` file failed, "cannot write the C"), and an output without an extension gets `.exe` with clang and gcc too. tooling.md §4.1, §8.3; spec §40.4, §40.7; c-backend.md §6; testing.md; ADR 0046 §7, 0047 §7, 0048 §7 | tools/package_tests.py (a C static library built by the test, two dependency levels up; the link file; an unset variable; the locked hash of a git dependency follows `links`; five N0321s), tools/dns_check.py (cancellation of running and queued lookups, the four-thread bound by time, 100 lookups; ASan, `live=0`), tools/wake_check.py (200 races beside 100 idle sockets: 20000 tasks woken before, 0 after), corpus run-pass async-dns and async-timeout-void, all in the contract suite. `python3 tools/contract_tests.py`: macOS every step ok (cancel sweep 34 async programs, 781 points; fault sweep 1192 points); Debian 12 (the compiler built from its emitted C with gcc) every step ok, the reactor uring among them (cancel sweep 629 points). Windows 11 ARM64 (MSVC, the compiler built from its emitted C; clang there too): the package tests with `cl`, dns_check (`--no-sanitizers`) under IOCP and `WSAPoll`, wake_check, and nine async programs under IOCP, `WSAPoll` and the interpreter, all as expected |
| 2026-11-02 | ADR 0049 (the owner: "Yes, I accept ADR 0049 with your recommendations"), from the owner's testing-framework research: `@test` on parameterless free functions (sync or async; `void` or `Result<void, E>`) in the sources and test directories, N0348 for other shapes and for calls of a test; lib/std's `testing.cee` (`TestFailure`, `Check`, `CheckResult<T, E>`, the harness's `TestRun`); `ceesafec test` checks the package with its tests, writes a generated harness (`ceesafeTestMain`, the checker's entry a setting), builds it once, and runs each test as its own process: passed, failed, panicked, crashed, timed out (a watchdog thread in the test process; the interpreter keeps the same deadline), leaked, skipped (C under `--interp`); `--filter`, `--list`, `--jobs` (Thread.both), `--timeout`, `--interp`, `--sweep alloc|cancel` (`ceesafe_test_begin`), `--format json`. tooling.md §10, type-system.md §28.1, diagnostics.md, testing.md, guide/testing.md; ADR 0049 §8 | check-fail/test-shapes; tools/test_command_check.py (in the contract suite) on tests/packages/test-command, 14 tests of every outcome against expected.tsv (reviewed by hand), natively and with `--interp`, `--list`, both sweeps against the harness's own counts, `build` without tests. `python3 tools/contract_tests.py`: macOS every step ok; Debian 12 every step ok but two run while a second copy of the suite ran beside it by mistake (the io_uring and TSan async runs), each then passing when rerun alone (TSan twice); a first run found `nanosleep` undeclared under glibc's strict headers (fixed: `_GNU_SOURCE`); Windows 11 ARM64 test_command_check with clang and with cl (where a counting loop's overflow panic came at once: the fixture's endless test now has no exit) |
| 2026-11-02 | Release v0.8.0 (ADR 0048 §5.5): ADR 0047's system libraries in the manifest, ADR 0048's DNS, ADR 0049's `ceesafec test`, bugs B5 and B6, and two Windows fixes (a `.c` input's name, `.exe` with every compiler); the version set to 0.8.0 in `src/backend/c_program.cee` and `editors/vscode/package.json`; stage 0 stays ceesafec 0.3.2, through the chain `f7a7d57`, `93c433f` | the contract suites of the three rows above; `tools/bootstrap_release.sh` from the released 0.3.2 on Debian 12 reaches the tree |
| 2026-11-02 | ADR 0052 (the owner: "Yes, I accept all five ADRs with your recommendations"): lib/std's `HashKey<K>`, `Hasher` (SipHash-1-3) and `HashMap<K: HashKey<K>, V>`, insertion-ordered, keyed per process by `ceesafe_random_key`; impl methods on `String` and `Vector` reachable. Spec §24.3, guide/collections.md, ADR 0052 §7 (commit 33fb373) | run-pass/hash-map from tools/oracles/gen_hashmap_cases.py (SipHash checked through SipHash-2-4's published vectors); the hashmap sweep (79 points) in the contract suite; check-fail/hash-map-key; macOS, Debian (gcc -Werror), Windows 11 ARM64 (clang, cl), the interpreter |
| 2026-11-02 | B7: a closure capturing a local named `code` gave its C environment two `f_code` members (the code pointer's and the capture's), and a local `p__x` clashed with a field-precise capture of `p.x`. Both environment names now start with a digit (`f_0code`, `f_0p__x`), which no identifier does (`src/check/closures.cee`, type-system.md §36.2). Found writing tools/sendfile_check.py; in 0.8.0 too | run-pass/closure-capture-names (fails to compile with 0.8.0); macOS, Debian, Windows 11 ARM64 natively and in the interpreter |
| 2026-11-02 | ADR 0051 (accepted with ADR 0052's row): `AsyncTcpStream.sendFile` (one step) and `sendFileAll`, 64-bit offsets, the file's position untouched; `ceesafe_async_op_sendfile` (Linux `sendfile`, io_uring through the readiness step; macOS `sendfile`; IOCP `TransmitFile` on a handle of its own from `ReOpenFile`, the count clamped to the file's end) and `ceesafe_file_read_at` (`pread`; Windows `ReadFile` at an offset on its own handle), with lib/std's 64 KiB copy where the runtime returns `CEE_ASYNC_NO_SENDFILE` (WSAPoll, CosmoOS, a refused file, the interpreter, whose model reads at an offset by reopening the file's path). `_GNU_SOURCE` on Linux in `ceesafe_rt_io.c` for `pread`. Spec §40.4, c-backend.md §6, guide/async.md, ADR 0051 §7 | run-pass/async-send-file from tools/oracles/gen_sendfile_cases.py (ranges of a 100001-byte file to a slow reader, one step, a step at the end, two streams at once, the position); tools/sendfile_check.py in the contract suite (a sparse file of 2^32 + 2^20 + 12345 bytes: across 4 GiB, its end, past it); the cancel sweep on it (179 points, 0 failures). `python3 tools/contract_tests.py` on macOS: every step ok but spec (documents.cee not regenerated) and two check-pass snapshots (closure ids only, checked normalised), both then fixed and rerun ok. Debian 12: epoll, io_uring, poll, the interpreter, sendfile_check under epoll and io_uring (a first build found `pread` undeclared under strict C11). Windows 11 ARM64: IOCP, WSAPoll, the interpreter, sendfile_check (a first run found TransmitFile refusing a count past the end and moving the file pointer, and ReadFile at an offset moving it too: fixed) |
| 2026-11-02 | ADR 0054 (accepted with ADR 0052's row; done before ADR 0050 at the owner's word, "Do ADR 0054 first", for CeeSafe.Web's M2): `impl<T: B> I for C<T>` and `impl<region r> I for S<r>`. The parser takes the impl's parameters; the checker checks a generic impl once for its target and implements it for each matching instance whose arguments meet the bounds, as generic structs' methods are; overlap with a generic impl and the orphan rule are N0267; a scoped type as `dyn I` is N0268. Spec §21.1, §25.3, §37.2, diagnostics.md, guide/generics.md, ADR 0054 §7 | run-pass/impl-generic, run-pass/impl-generic-async (CeeSafe.Web's probes, passing), check-fail/impl-generic; the corpus, package tests and every CeeSafe.Web package check as before (the orphan rule's search); macOS contract suite; Debian 12 (gcc -Werror -std=c11, io_uring, the interpreter) and Windows 11 ARM64 (IOCP, WSAPoll, the interpreter) on the new tests |
| 2026-11-02 | ADR 0050 (accepted with ADR 0052's row): `AsyncUdpSocket` (`bind` with `UdpOptions`; `sendTo`, `recvFrom`; `sendMsg`/`recvMsg` with the local address, ECN bits, truncation and segments), `UdpMessage`, `Datagram`, `UdpReceived`; the runtime's datagram steps on the message record on every reactor (sendmsg/recvmsg, io_uring, IOCP's WSASendMsg/WSARecvMsg, WSAPoll), `UDP_SEGMENT` on Linux, `SIO_UDP_CONNRESET` off on Windows, EMSGSIZE as InvalidInput; `AsyncSockets.udpBind`/`udpSend`/`udpRecv`; the interpreter's models. Two commits, `71d5c13` a new bootstrap chain step. Spec §40.4, c-backend.md §6, guide/async.md, ADR 0050 §7 | run-pass/async-udp (`tools/oracles/gen_udp_cases.py`), natively and in the interpreter; `tools/udp_check.py` (ECN v4 and v6, a source address, 100 segments, a cancelled receive; ASan) in the contract suite; macOS contract suite (the first commit's); Debian 12 (epoll, io_uring, poll, gcc -Werror -std=c11) and Windows 11 ARM64 (IOCP, WSAPoll; CE refused as InvalidInput) |
| 2026-11-02 | ADR 0053, first part (accepted with ADR 0052's row): `Async.tasks(limit, oom, async (set) => …)` (lib/core's `AsyncTasks<E>.run`, an `@effectsOf` async fn, ADR 0037), `TaskSet<E>` with `await set.spawn(async move () => …)` and `set.trySpawn(…)`, from any function the set is passed to; N0349 for a child's capture holding a loan; the HIR form `SetSpawn` and builtin `AsyncSetSpawn`; the runtime's `ceesafe_task_set` (no sibling cancelled by a child's error, a failed allocation stopping nothing, one task waiting for room); the interpreter's model. `E` must be Copy; a spawn on a cancelled set is `Err(oom)`. `Async.tasksIn` and `set.spawnSend` follow. Spec §40.5, intermediate-representation.md, c-backend.md §3a, diagnostics.md, guide/async.md, spec/api/core.md, ADR 0053 §7 | run-pass/async-task-set, check-fail/async-task-set, the guide's example; the cancellation sweep on it (0 failures); macOS contract suite; Debian 12 (gcc -Werror -std=c11, io_uring, the interpreter) and Windows 11 ARM64 (IOCP, WSAPoll, the interpreter) on the new test. check-pass snapshots: closure ids only (checked normalised) |
| 2026-11-02 | ADR 0053, second part: `Async.tasksIn(alloc, limit, oom, body)` (lib/core's `AsyncTasksIn<E, A>.run`, which gives the set its allocator's adapters through the library-only `Async.setAllocator`, builtin `AsyncSetAllocator`; `Async.tasks` is it with `SystemHeap`), `set.spawnSend` (ADR 0038's rule on captures and `E`; the runtime's `ceesafe_async_set_spawn_send`; a shared set admits under the run's lock); the interpreter's allocator and FailingAllocator models. Spec §40.5, c-backend.md §3a, ADR 0053 §7 | run-pass/async-task-set-in (a counting allocator's blocks handed out and taken back; FailingAllocator making spawn `Err(oom)`, trySpawn false and the set `oom`; spawnSend's sum on one worker and four), check-fail/async-task-set (N0334, N0263, N0345 added); natively and in the interpreter; macOS contract suite; Debian 12 (gcc -Werror -std=c11, io_uring, TSan) and Windows 11 ARM64 (IOCP, WSAPoll) on the new tests |
| 2026-11-02 | Release v0.9.0 (the owner: "Release 0.9.0 next"): ADR 0050's UDP, ADR 0051's file sends, ADR 0052's HashMap, ADR 0053's task sets, ADR 0054's generic impls, and bug B7; the version set to 0.9.0 in `src/backend/c_program.cee` and `editors/vscode/package.json`; the README and the site's home page; stage 0 stays ceesafec 0.3.2, through the chain `f7a7d57`, `93c433f`, `71d5c13` | the contract suites of the rows above, and once more on the release commit; `tools/bootstrap_release.sh` from the released 0.3.2 reaches the tree |
| 2026-11-02 | B8 (found by CeeSafe.Web, its probe `tests/probes/generic-dyn-box-flow-bug.cee`): a `Box<dyn I<S>>` parameter stored through `borrow mut` was N0272 when `S` is a type parameter. The loan rule (§25.7 rule 5) counted a type parameter met only inside a `dyn` type, which carries no loans of its own (§37.2): `mayHoldLoans` now uses `TypeTable.mentionsParameterOutsideDyn` (`src/check/references.cee`, `src/types/types.cee`); spec §25.7 rule 5 | run-pass/generic-dyn-box-store (the probe, and a free function, natively and in the interpreter; 0.9.0 reports three N0272); the corpus (every check-fail as before); macOS contract suite, every step ok but spec, which failed only because ADR 0055's file was written during the run (rerun ok after) |
| 2026-11-02 | ADR 0055 (the owner: "Yes, I accept ADR 0055 with your recommendations"), for CeeSafe.Web's U12: lib/std's `Clock.now()` and `UnixTime` (seconds and nanoseconds since 1970; `millis()`), `Clock.unixMillis()`; the runtime's `ceesafe_clock_realtime` (`CLOCK_REALTIME`, `GetSystemTimePreciseAsFileTime`); the interpreter's 2000-01-01 epoch plus its virtual clock. Spec §26.11, §40.8, guide/async.md, ADR 0055 §7 | run-pass/clock, check-fail/freestanding-hosted-names (N0200); `tools/clock_check.py` in the contract suite; macOS contract suite (every step ok but two check-pass snapshots, closure ids only, checked normalised and updated); Debian 12 and Windows 11 ARM64: clock_check, run-pass/clock natively and in the interpreter |
| 2026-11-02 | Release v0.9.1 (the owner: "Go ahead and publish"): bug B8 (a `Box<dyn I<S>>` parameter stored where `S` is a type parameter) and ADR 0055's wall clock, for CeeSafe.Web's M2; the version set to 0.9.1 in `src/backend/c_program.cee` and `editors/vscode/package.json`; stage 0 stays ceesafec 0.3.2, through the chain `f7a7d57`, `93c433f`, `71d5c13` | the contract suite on the release commit |
| 2026-11-02 | B9 (found by CeeSafe.Web on 0.9.1): a generic struct's own method could not coerce `this` to `dyn I` when `I` is implemented by a generic impl (N0268): the impl was attached to the struct's instances but not to its generic version, in which its methods are checked. `attachToInstances` attaches to both (`src/check/interfaces.cee`) | run-pass/generic-impl-dyn-own-method (0.9.1 reports N0268), natively and in the interpreter |
| 2026-11-02 | B10 (found by CeeSafe.Web; since 0.8.0): a type boxed as `dyn I`, `I` with an async method never called through `dyn`, gave C that did not compile: the table's entry names its dyn frame, emitted only for a dyn call. The frames a table's async entries need are emitted too (`src/backend/c_async.cee`) | run-pass/async-dyn-uncalled (0.9.1: ten C errors) |
| 2026-11-02 | B11 (found by CeeSafe.Web; since 0.8.0): a method called through a returned `borrow Box<dyn I>` panicked the compiler (`c_body.cee`, the dyn branch of `Ref`): the receiver, reached through a box, was copied to a temporary of type `dyn I`. A path through a box reached through a reference is borrowed in place (`isPathThroughReference`, `src/check/places.cee`) | run-pass/borrowed-box-dyn-call (0.9.1 panics), natively and in the interpreter |
| 2026-11-02 | B12 (found by CeeSafe.Web on 0.9.0): a generic impl's method passing `this` as `dyn I` named the uninstantiated type in C: generic code's coercion recorded a dyn table for the generic type. Generic code records none; each instance records its own (`src/check/dyn.cee`) | run-pass/generic-impl-this-dyn (0.9.1: nine C errors), natively and in the interpreter |
| 2026-11-02 | Release v0.9.2 (the owner: "Yes, go ahead and release"): bugs B9–B12, for CeeSafe.Web's `Server<S>` (M2); the version set to 0.9.2 in `src/backend/c_program.cee` and `editors/vscode/package.json`; stage 0 stays ceesafec 0.3.2, through the chain `f7a7d57`, `93c433f`, `71d5c13` | the contract suite on the release commit |
| 2026-11-02 | `ceesafec test` after CeeSafe.Web's report (ADR 0049 §8): `--interp` finds leaks (the interpreter's `ceesafe_test_begin` turns on its allocation report); `--interp --sweep` is a usage error; a `--filter` matching nothing is exit status 1; tests run in the names' order; a test's own enum error names its variant; the outcome `exited` for `Process.exit`, `crashed` kept for signals; `ceesafec test --help`. tooling.md §10.2, guide/testing.md | `tools/test_command_check.py` with the fixture's `leaksSafely` and `exits` (expected.tsv reordered by name and extended, reviewed by hand) and the new checks; macOS contract suite; Debian 12 and Windows 11 ARM64 test_command_check |
| 2026-11-02 | ADR 0056 (the owner: "I accept ADR 0056 with your recommendations"), after CeeSafe.Web's notes on middleware: recursion through `await` (direct, mutual, through `dyn`) compiles in the hosted profile, each await on a cycle stacked (the NIR role `taskStacked`): its callee's frame from the task's frame stack (`ceesafe_async_stack_push`/`pop`, 16 KiB chunks, a pool of 64 per executor, a panic when none can be had); `--explain-frames` names stacked awaits; the task record and `ceesafe_task_slot` 160 bytes. N0343 remains for freestanding code, race/all children and ADR 0037's instances. Spec §40.1, c-backend.md §3a, diagnostics.md, guide/async.md, ADR 0056 §7 | run-pass/async-recursion-stacked, natively and in the interpreter; check-fail/async-recursion (freestanding) and async-dyn-frames; `tools/frame_stack_check.py` in the contract suite; macOS contract suite; Debian 12 and Windows 11 ARM64 |
| 2026-11-02 | Release v0.9.3 (the owner: "push and release"): ADR 0056's frame stacks (recursion through `await`, middleware through `dyn`) and the `ceesafec test` fixes after CeeSafe.Web's report; the version set to 0.9.3 in `src/backend/c_program.cee` and `editors/vscode/package.json`; stage 0 stays ceesafec 0.3.2, through the chain `f7a7d57`, `93c433f`, `71d5c13` | the contract suite on the release commit |
| 2026-11-02 | An independent benchmark imported (the owner: "import it into the repo and run it"): `benchmarks/independent/`, 13 workloads in CeeSafe and Rust from an outside author, with their Linux run on 0.9.0 (`results-linux-0.9.0.json`); `run.py` made portable to macOS (CPU name, `ru_maxrss` in bytes, binaries in `out/`), the programs unchanged | run on 0.9.3, Apple M1, 9 rounds (`results-macos-0.9.3.json`): every checksum equal; geometric mean against `rust-checked` 1.43 at the default `-O2` [1.41, 1.45], 1.24 at `-O3` [1.22, 1.25]; largest gaps `sort` 4.3× (library sort), `matmul` 3.5× at `-O2` (1.02× at `-O3`), `hashmap` 1.9× |
| 2026-11-03 | ADR 0057 (the owner: "Yes to all ADRs. I accept."): a faster `sortBy` in lib/core: a partition without a branch on the comparison, pdqsort's equal-key partition, a run check at the start; heapsort and every guarantee of type-system.md §36.4 kept; spec §36.4, the API page, the guide; run-pass/sort-patterns from `tools/oracles/gen_sort_cases.py` (Python's `sorted`), McIlroy's adversary in run-pass/closure-methods for the heapsort path; `closure-calls/known.calls` updated (the new helpers, all direct), two `.hir` snapshots with closure ids only | the contract suite (every step but the corpus, whose `.hir`/`.calls` snapshots were then updated; the corpus rerun: 0 fail); sort-patterns and closure-methods with gcc 12 and ASan on Debian and MSVC on Windows; partition loops branch-free in clang, gcc and MSVC assembly; `benchmarks/independent` `sort` 4.30× → 1.69× `rust-checked` (`results-macos-adr0057.json`) |
| 2026-11-03 | ADR 0059 (the owner: "I accept both ADRs with your recommendations"): the allocation counters only when something reads them: one flag in `runtime/core/ceesafe_rt_core.c`, set by `CEESAFE_ALLOC_REPORT`, `CEESAFE_FAIL_ALLOC_AT` or `ceesafe_test_begin` before the first allocation; c-backend.md and memory-model.md; `tools/alloc_counting_check.py`, a new contract step | the contract suite (every step; `spec` rerun after the ADR 0058 text below changed the embedded documents: 0 problems); the check fails against 0.9.3's runtime at its three unguarded counters; `binary-trees` depth 18 on the Debian box (gcc 12, pinned, load ≈7.5, 7 runs): median 1.87 s → 1.59 s |
| 2026-11-03 | ADR 0058 (the same acceptance): `-O3` for the generated C, **held at its gate** (§3.2): `dyn-dispatch`'s slowdown on the M1 is layout, not `-O3` — the same `-O2` code takes 0.44–1.04 s across builds that differ only in where code lands, and the hot loop and thunks are identical at both levels; not steerable from the C; back to the owner (§7) before `src/host` changes | measurements only, recorded in ADR 0058 §7 |
| 2026-11-03 | ADR 0058 withdrawn by the owner at its gate ("Lets stop it there. I don't want to land slow."): `-O2` stays the default; the ADR's status and §8 record it | none: no code changed |
| 2026-11-03 | CeeSafe.Web's B15, B14, B13 and T3 (`~/dev/CeeSafe.Web/docs/ceesafe-upstream-work.md`): B15, `dyn I` and `dyn I + Send` share one dyn frame per slot (they mangle alike), sized over both types' impls, emitted once (`src/backend/c_async.cee`, `sameDynFamily`, `dynFrameMethod`; spec §40 notes it); B14, `+ Send` after `dyn` in the type-argument lookahead (`src/syntax/parser.cee`); B13, the drop glue of a scope child's error declared before its adapter; T3, `(long)FIONBIO` in the runtime; run-pass/async-dyn-send-frames, dyn-send-type-argument, async-scope-droppable-error | run: the three tests natively with `--check-leaks` and in the interpreter (0.9.3 fails them: 18 and 5 C errors, and the parse error); with gcc 12 plus ASan/UBSan on Debian (live=0) and MSVC on Windows; clang 23 `-Wsign-conversion` on Windows: 0.9.3's runtime warns 4 times, the fix none; CeeSafe.Web `m5-client`'s `web_client` with `ceesafec test`: 11 passed natively (0.9.3: 20 C errors). Pending at commit: the full contract suite |
| 2026-11-03 | HashMap after a reviewer's prototypes (ADR 0052 §8): slots carry the hash's low 32 bits (home and fingerprint), no `hashes` vector, inserts into the probe's empty slot, at most 2^32 − 1 entries (else OutOfMemory); `Hasher.u64` and `Hasher.bytes` word paths, same output; run-pass/hash-map gains mixed-feed SipHash cases from the generator's oracle. Also B13 in the interpreter: a later child's error and a scope's unused error argument are dropped, as in C (the B13 test leaked 3 allocations under `run interp`, found by the contract suite on `992b1eb`) | run: hash-map natively with `--check-leaks` and in the interpreter; the hashmap sweep, 64 of 64; per phase (4 M keys): M1 clang `-O2` insert 320–366 → 264 ms, hit 227–236 → 160–171, miss 249–280 → 216, remove ≈ 205 → 210; Ryzen gcc 12 `-O2` insert, hit and miss ≈ 1.7× faster, remove the same; the B13 test in the interpreter: live 3 → 0; hash-map on Debian (gcc `-Werror`, ASan/UBSan, live=0); the contract suite: every step but the corpus, whose two `.hir` snapshots differ in closure ids only (verified normalised) and were updated, then the corpus: 0 fail |
| 2026-11-03 | Release v0.9.4 (the owner: "commit and release when the suite passes"): ADR 0057's `sortBy`, ADR 0059's allocation counters, CeeSafe.Web's B13 (C and interpreter), B14, B15 and T3, and the HashMap of ADR 0052 §8; the version set to 0.9.4 in `src/backend/c_program.cee` and `editors/vscode/package.json`; stage 0 stays ceesafec 0.3.2 | the contract suite on `59fd0f0` (above); the release build: the corpus and the spec check |
| 2026-11-03 | ADR 0060 (the owner: "Lets go back to CeeSafe once again to implement the following request by CeeSafe.Web"), CeeSafe.Web's U16, U13, U14 and U15: lib/std's `Random.fill(into)` over the runtime's `ceesafe_random_fill` (`getrandom` or `/dev/urandom`, `arc4random_buf`, `BCryptGenRandom`; `Unsupported` on CosmoOS; HashMap's seed through it), the interpreter's SplitMix64 sequence; `Channel.tryRecv()` and `isClosed()` (`ceesafe_chan_closed`); `CEE_ALLOC_FOREIGN` and `ceesafe/foreign.h`, written into every build, its blocks reported apart (`ceesafe alloc: foreign held=<k>`) and never a leak; `IoError.HostNotFound` (variant 19, "Host not found") for the resolver's "no such name" and "no address" (runtime number 65536), the interpreter's `.invalid` names. Spec §26.2, §26.12, §28.6, §40.4, memory-model.md §3.2, ADR 0048's amendment, guide/async.md and unsafe-and-c.md | run-pass/random-fill, channel-try-recv, io-error-host-not-found, async-dns (updated), check-fail/freestanding-hosted-names (N0200); `tools/random_check.py` and `tools/foreign_alloc_check.py`, new contract steps; macOS contract suite (two check-pass snapshots, closure ids only, checked normalised and updated; the alloc-counting guard check, fixed); Debian 12 (gcc `-Werror`, ASan + UBSan) and Windows 11 ARM64 (clang `-Werror -Wsign-conversion`): the new tests natively and in the interpreter with leak checks, random_check, foreign_alloc_check, dns_check |
| 2026-11-03 | Release v0.10.0 (the owner: "Yes, commit and release it"): ADR 0060's `Random.fill`, `Channel.tryRecv` and `isClosed`, `CEE_ALLOC_FOREIGN` and `ceesafe/foreign.h`, and `IoError.HostNotFound` (CeeSafe.Web's U16, U13, U14, U15); a minor version for `HostNotFound`'s source change, as 0.7.0 was for ADR 0044's; the version set to 0.10.0 in `src/backend/c_program.cee` and `editors/vscode/package.json`; stage 0 stays ceesafec 0.3.2 | the contract suite on `8ac40d8` (above); the release build: the corpus and the spec check |
| 2026-11-03 | ADR 0061 (the owner: "You can take over CeeSafe session and tackle U17 then release as per the worfklow v* tag"), CeeSafe.Web's U17: `CEE_ALLOC_FOREIGN_INIT` (3) in `ceesafe/foreign.h`, for a C library's one-time initialisation, which cannot survive a failed allocation (OpenSSL 3.6's re-enters the `pthread_once` running it): never failed (`CEESAFE_FAIL_ALLOC_AT`, `FailingAllocator`) and not counted as an attempt, its blocks held with `CEE_ALLOC_FOREIGN`'s (`foreign held`), either number may reallocate or free them. Spec §28.6, memory-model.md §3.2, guide/unsafe-and-c.md | `tools/foreign_alloc_check.py` extended (an initialisation: no attempt added, held=2, never failed at any of the sweep's points), and a mutant runtime counting and failing the new number fails it (8 problems); macOS contract suite, the fixed point included; CeeSafe.Web's `web_tls` with OpenSSL's initialisation under the number: the allocation sweep passes the points that crashed before |
| 2026-11-03 | Release v0.10.1 (the owner, as above): ADR 0061's `CEE_ALLOC_FOREIGN_INIT` (CeeSafe.Web's U17); a patch version, since it only adds; the version set to 0.10.1 in `src/backend/c_program.cee` and `editors/vscode/package.json`; stage 0 stays ceesafec 0.3.2 | the contract suite on the ADR 0061 commit (above); the release build: the corpus and the spec check |
| 2026-11-03 | Toward GitHub Linguist (the owner: "do the draft and the usage check; grammar under MIT"): `editors/linguist/` (the `languages.yml` entry, a `.cee` heuristic, two samples, the steps), the TextMate grammar under the MIT licence (`editors/vscode/syntaxes/LICENSE`), `tools/linguist_usage.py`. Not submittable yet: Linguist wants 2000 files indexed in a year from others' repositories | `tools/linguist_usage.py`: 1084 `.cee` files, 130 outside `vkuttyp` and CeeSafe, 0 of them CeeSafe-like (the query finds 61 in the owner's); the heuristic matches both samples and none of three other `.cee` languages' files; spec_check: 0 problems |
| 2026-11-03 | The repositories moved to the `CeeSafe` organisation (the owner: "Move CeeSafe-index, too. Yes, update the references and push"): `vkuttyp/CeeSafe` and `vkuttyp/CeeSafe-index` transferred to `CeeSafe/CeeSafe` and `CeeSafe/CeeSafe-index`; the site is `ceesafe.github.io/CeeSafe`. References updated: the default registry index (`src/packages/workspace.cee`, spec/tooling.md, spec/README.md, ADR 0017 §3), the install scripts and the site's config, the guide, the README, the extension's manifest and README, release.yml's stage 0 URL (same SHA-256), docs/release.md's store policies, the agents template and this repository's AGENTS.md (`ceesafec agents`). CosmoOS links stay at `vkuttyp/CosmoOS`; history (this table, docs/p8-plan.md, ADR 0017's question) keeps the old names. The stores' trusted publishers name the owner and must be changed in each store before the next release | spec_check, package_tests, `fmt --check`, `sh -n site/public/install.sh` |
| 2026-11-03 | CeeSafe.Web's B16 (the owner: "Take B16 into the CeeSafe repo now and fix it there first"): on kqueue, a full change queue (64 entries) sent early by `cee_kqueue_change` drained the wake pipe, so the next `cee_wait` blocked in `kevent` until a deadline with a woken task on its wake list (HTTP/2 streams 1 s late past ~30 connections). `cee_kqueue_call` now records a drained wake (`pipe_drained`), and `cee_wait` does not block while one is recorded. Found by tracing the emitted runtime under `h2load`: a task parked on a channel, on the wake and parked lists with `woken` set, ran 1 003 ms later, after the executor went idle | corpus run-pass async-drained-wake (80 readers wake, the first sends to a parked receiver, all park on sockets): with 0.10.1, prints 0 after its 2 s guard, 3 of 3 runs; with the fix, prints 1 in 0.06 s, 5 of 5, and in the interpreter. `python3 tools/run_corpus.py` (581 files) and `python3 tools/contract_tests.py --ceesafec out/ceesafec` on macOS: every step ok (ASan, TSan, -Werror, reactor poll, fault sweep 1717 points, cancel sweep 1196 points, wake_check). CeeSafe.Web's `examples/server` with `h2load -c 30 -m 1`: 150 → 10 023 req/s, longest request 1.01 s → 5.2 ms. Found beside it, open: every finished race walks every parked task (CeeSafe.Web's B17) |
| 2026-11-03 | Release v0.10.2 (the owner: "Merge and release 0.10.2"): CeeSafe.Web's B16 (a wake lost when a full kqueue change queue drains the wake pipe); a patch version, since it only fixes; the version set to 0.10.2 in `src/backend/c_program.cee` and `editors/vscode/package.json`; stage 0 stays ceesafec 0.3.2 | `python3 tools/contract_tests.py --ceesafec out/ceesafec` on the release commit, macOS: every step ok, the fixed point among them (`spec` re-run alone after an uncommitted draft ADR was set aside: 109 documents, 0 problems) |
| 2026-11-03 | ADR 0062 (the owner: "I accept ADR 0062 with your recommendations"), CeeSafe.Web's B17: a cancellation visits only the cancelled scope's tasks. Each scope lists its children that run on its worker, and each task the scopes it owns; `cee_cancel_visit` walks them depth first through their own links, and a shared scope falls back to the full walk. `cee_task` is 192 bytes, `ceesafe_scope` 48 (`taskSlotLayout`, `scopeLayout`, the interpreter's task head). On the way, `cee_wait` takes woken tasks in one pass and heap-sorts them by wake, where it walked the wake list once per woken task. `CEESAFE_CANCEL_REPORT` adds `visited=`. Spec c-backend.md (the task record, cancellation, the report) | corpus run-pass async-cancel-nested (three levels of scopes, 9 parked tasks see an outer race's cancellation, 0.24 s; interpreter too); `tools/wake_check.py` now also requires at most 400 tasks looked at by 200 cancellations (400 on the reactor and poll). A benchmark of 20 000 races beside 4 000 idle tasks: 594 → 56 ms. CeeSafe.Web's HTTP/2 server under `h2load`: 50 × 100 streams 3 562 → 70 956 req/s, 10 × 100 21 944 → 75 812 (ADR 0062 §7). `python3 tools/contract_tests.py --ceesafec out/ceesafec` on macOS: every step ok, the fixed point among them (fault sweep 1727 points, cancel sweep 1226) |
| 2026-11-03 | Release v0.10.3 (the owner, accepting ADR 0062's question 4: a patch release when it lands): ADR 0062's cancellation that visits only its scope, and the one-pass wake list (CeeSafe.Web's B17); the version set to 0.10.3 in `src/backend/c_program.cee` and `editors/vscode/package.json`; stage 0 stays ceesafec 0.3.2 | `python3 tools/contract_tests.py --ceesafec out/ceesafec` on the release commit, macOS: every step ok, the fixed point among them; CeeSafe.Web's `tools/run_tests.py --sanitize --alloc-sweep --cancel-sweep` with it: 39 runs passed, 1 skipped |
| 2026-11-04 | Release v0.11.0 (the owner: "Now it's time to release", then "0.11.0"): ADRs 0063 (index ranges, integer `min`/`max`/`abs`/`unsignedAbs`, reborrowing into `dyn`), 0064 (generic methods and inherent `impl`, arrays and slices of views and constant views, `dyn I` as a bound, scoped `T` behind `borrow dyn I`) and 0065 (the agent-facing reference: `spec/api/std.md`, `guide/builtins.md`, `ceesafe_api`, "did you mean"); a minor version, since it adds language features and breaks no program that compiled on 0.10.3; the version set to 0.11.0 in `src/backend/c_program.cee` and `editors/vscode/package.json`; stage 0 stays ceesafec 0.3.2. The compiler's own sources keep to what stage 0 accepts: `src/check/suggest.cee` first used `usize.min` and was rewritten before the release commit | `sh tools/bootstrap_release.sh` with v0.3.2's osx-arm64 binary (its `SHA256SUMS` hash checked) builds the release commit through the recorded chain (0.3.2, 0.6.0, 0.6.0, 0.8.0); that compiler's C equals its rebuild's and the local build's. `python3 tools/contract_tests.py --ceesafec out/ceesafec` on the release commit, macOS: all 31 contracts ok, the fixed point and `mcp api` among them |
| 2026-11-04 | ADRs 0066 and 0067 (the owner: "fix the static view table finding in CeeSafe", then "fix the static mut hole too"; uncommitted, not released). 0066: a `static` (not `static mut`) may be `[StringView; N]`, the constant views' set (`isConstantView`), so a table is a place, sliced and borrowed where it is named (`src/check/statics.cee`; type-system.md §10, §39.2). 0067: a write to a static may carry no loan, N0272 (`checkStaticWrites`, `src/nir/borrow.cee`; §39.3): 0.11.0 compiled `LAST = s.view()` into a `static mut` and read freed memory afterwards. Rejected too: a `StringView<static>` parameter, since a call does not check that region (left open, ADR 0067 §4) | New corpus tests `run-pass/static-view-tables`, `check-fail/static-view-tables`, `run-pass/static-mut-views`, `check-fail/static-mut-loans`: expected output from Python, run-pass passing compiled and `--interp`, all four failing on 0.11.0. Full corpus 600 tests, 0 failures. The working tree's gen 1 (built by 0.11.0) and gen 2 emit identical C. `python3 tools/contract_tests.py --ceesafec out/ceesafec`: all 31 ok (run fortify and reactor uring skipped, Linux only). ClayWind (six packages) and CosmoStack check |
| 2026-11-04 | ADR 0068 (the owner: "Tackle ADR 0067 §4 too", then "Reject on params, enforce on results"; uncommitted, not released): `static` written in a parameter's type is N0273, since no call path (direct, function value, `dyn`, bound) could check it. A result whose regions are all `static` borrows from no parameter: the body may return only static data (N0245 with a `static` hint, or N0244), and the caller's result carries no loan and may be stored in a static. Before, `-> StringView<static>` was elided to the parameter (`launder(v) => v` compiled) or N0245 without one (`writesStatic`, `src/check/references.cee`, `src/check/interfaces.cee`; type-system.md §25.2, §25.6, §39.3) | New corpus tests `check-fail/static-parameters`, `check-fail/static-results`, `run-pass/static-results` (expected output from Python; compiled and `--interp`), all failing on the previous build. Changed in place, with notes: `tests/check/check-pass/arrays-and-slices.cee`, `tests/check/check-fail/arrays-and-slices.cee`, and `check-fail/static-mut-loans` (its `StringView<static>` case moved). Full corpus 602, 0 failures. Gen 1 and gen 2 emit identical C. `contract_tests.py`: all 31 ok (2 Linux-only skipped). The compiler, lib/core, lib/std and ClayWind check. CosmoStack's two errors come from edits made during the check and are the same on 0.11.0 |
| 2026-11-04 | Release v0.12.0 (the owner: "Commit them one per ADR and release 0.12.0"): ADRs 0066 (static tables of views), 0067 (a write to a `static mut` carries no loan: a soundness fix) and 0068 (`static` rejected on parameters, enforced on results); a minor version, since ADR 0068 rejects `static` written on a parameter, which compiled on 0.11.0; the version set to 0.12.0 in `src/backend/c_program.cee` and `editors/vscode/package.json`; stage 0 stays ceesafec 0.3.2 | `sh tools/bootstrap_release.sh` with v0.3.2's osx-arm64 binary builds the release commit through the recorded chain; `python3 tools/contract_tests.py --ceesafec out/ceesafec` on the release commit, macOS |
| 2026-11-04 | ADR 0069 (the owner: a native app demo of the ClayWind port, then "Add windowing to CeeSafe first" and "Accept, implement it"): lib/std's `Window` (type-system.md §26.13): `open`, `nextEvent`, `present` of premultiplied RGBA8 `u32`s, `size`, `scale`, `setCursor`, `setTitle`, `waitFrame`. The backends in `runtime/hosted/ceesafe_rt_window.c` are AppKit through the Objective-C runtime (loaded with dlopen), Win32 and GDI, and X11 (libX11 loaded with dlopen); `CEESAFE_WINDOW_SCRIPT` gives an in-memory window for tests; the interpreter answers `Unsupported` | `tools/window_check.py` (new contract): 7 checks against a Python model of §26.13. `check-fail/window-not-send`. Real windows with injected input on macOS arm64, Windows 11 arm64 (MSVC; x64 built too) and Ubuntu 22.04 on Xvfb (gcc, strict warnings). ClayWind's desktop host runs on all three. Full corpus passes; `check-pass/async-timeout.hir` and `effects-of.hir` regenerated (closure numbers only). Gen 1 and gen 2 emit identical C |
| 2026-11-04 | Release v0.13.0 (the owner accepted ADR 0069 with "release as 0.13.0"): ADR 0069, native windows in lib/std; a minor version for a new library API, which also takes the names `Window` and `Window*` from programs; the version set to 0.13.0 in `src/backend/c_program.cee` and `editors/vscode/package.json`; stage 0 stays ceesafec 0.3.2 | `sh tools/bootstrap_release.sh` with v0.3.2's osx-arm64 binary builds the release commit through the recorded chain; `python3 tools/contract_tests.py --ceesafec out/ceesafec` on the release commit, macOS |
| 2026-11-04 | ADR 0069 §8: the macOS window is sRGB (`setColorSpace:`), so Core Animation no longer converts every frame's colours on the CPU (profiled in ClayWind's desktop host on a Retina display: vImage about 500 samples a frame, against about 230 for rendering) | The host's idle CPU went from 47% to 31%; a screenshot's colours are within 1 of before; strict warnings clean for arm64 and x86_64 macOS; `window_check.py --display` passes |
| 2026-11-04. Not published: GitHub returned a server error on the tag push and never started the release workflow, and the owner chose to skip 0.13.1 ("Skip 0.13.1, 0.14.0 covers it"); the tag v0.13.1 stays at ae26d98 with no release | Release v0.13.1 (the owner: "Commit the sRGB fix and release 0.13.1"): ADR 0069 §8, the sRGB window on macOS; a patch version, since it only changes the runtime's macOS backend; the version set to 0.13.1 in `src/backend/c_program.cee` and `editors/vscode/package.json`; stage 0 stays ceesafec 0.3.2 | `sh tools/bootstrap_release.sh` with v0.3.2's osx-arm64 binary builds the release commit through the recorded chain; `python3 tools/contract_tests.py --ceesafec out/ceesafec` on the release commit, macOS |
| 2026-11-04 | ADR 0070 (the owner: "Add a present that takes the changed boxes", then "Accept, implement and release 0.14.0"): `Window.presentChanged(pixels, width, height, changed: Slice<WindowBox>)` shows only the boxes that changed since the last frame (type-system.md §26.13); a plain `present` without a last frame of the size, for a box outside the image, or past 64 boxes. Backends: on macOS two sRGB IOSurfaces shown in turn, each updated only by the boxes it lacks; on Windows `StretchDIBits` of each box at 1:1 (its source y counts from the bottom); on X11 `XPutImage` of each box at 1:1; scaled windows draw whole; scripted windows apply the boxes to their kept frame | `tools/window_check.py`: 16 checks, 8 of them `presentChanged` sequences against a Python model, `--display` included. A moving square through `presentChanged` on real windows (macOS Retina, Windows 11 ARM64 with MSVC, Ubuntu on Xvfb with gcc's strict warnings): one square, no trail, at 1:1 and scaled. ClayWind's desktop host on a Retina display: idle CPU from 31% to 16%, at 55 FPS |
| 2026-11-04 | Release v0.14.0 (the owner accepted ADR 0070 with "Accept, implement and release 0.14.0"): ADR 0070, `Window.presentChanged`; it also carries ADR 0069 §8 (the sRGB macOS window), whose v0.13.1 tag reached GitHub without starting the release workflow; a minor version for a new library API, which also takes the name `WindowBox`; the version set to 0.14.0 in `src/backend/c_program.cee` and `editors/vscode/package.json`; stage 0 stays ceesafec 0.3.2 | `sh tools/bootstrap_release.sh` with v0.3.2's osx-arm64 binary builds the release commit through the recorded chain; `python3 tools/contract_tests.py --ceesafec out/ceesafec` on the release commit, macOS |
| 2026-11-05 | ADR 0071 (the owner: "Accept as proposed, implement and release 0.15.0"), from a Postgres wire client's needs: `v.extend(s)`, `v.resize(n, fill)` and `v.reserve(extra)` on vectors of Copy elements; byte order on byte slices and vectors (`readBE<T>`, `readLE<T>`, `writeBE`, `writeLE`, `pushBE`, `pushLE`), range-checked at the call; the unsigned bit methods (`rotateLeft`, `rotateRight`, `wrappingShl`, `wrappingShr`, `leadingZeros`, `trailingZeros`, `countOnes`) and `byteSwap`; searching, trimming and splitting `StringView`, `String` and `Slice<u8>`; `parseInt<T>`, `parseIntRadix<T>` and correctly rounded `parseFloat<T>` (type-system.md §6, §16.6, §23.1). lib/core's `bytes.cee`, `text.cee` and `numbers.cee`; the float parser in CeeSafe rather than the C runtime (ADR 0071 §7.2) | 3.1 million float inputs against exact references (Python's `float`, exact rationals for `f32`) with no difference, the interpreter on a subset; 4 million print-then-parse round-trips; the corpus's five new run-pass, one run-fail and six check-fail tests, compiled and interpreted; `tools/guide_check.py` |
| 2026-11-05 | Release v0.15.0 (the owner: "Accept as proposed, implement and release 0.15.0"): ADR 0071; a minor version for new library methods, which also reserve the lib/core names `ByteOrder`, `VectorBytes`, `TextSearch`, `ByteSearch`, `TextHalves`, `ByteHalves`, `TextSplit`, `ByteSplit` and `NumberParsing`; the version set to 0.15.0 in `src/backend/c_program.cee` and `editors/vscode/package.json`; stage 0 stays ceesafec 0.3.2 | `sh tools/bootstrap_release.sh` with v0.3.2's osx-arm64 binary builds the release commit through the recorded chain; `python3 tools/contract_tests.py --ceesafec out/ceesafec` on the release commit, macOS |
| 2026-11-05 | Fix: a method merged from an `impl S { … }` block (ADR 0064 §1) took the struct's file, so its offsets were read against the wrong source: `check` (a diagnostic in it), `build` and `run` (its runtime checks) panicked with an index out of bounds in `src/support/source.cee` once an offset passed the struct file's end; found splitting a 21k-line struct of CosmoStack into `impl` files. `mergeInherentImpls` records each merged method's file (`Checker.methodFiles`); `resolveStructMembers` and `processMethodAttributes` set it per method. Fixture `tests/packages/impl-files`, checks 6–7 of `tools/package_tests.py` | on macOS: package tests 7/7 (checks 6–7 fail with 0.15.0); corpus 620 files, 0 fail, 4 partial (freestanding, as before); `fmt --check` clean |
| 2026-11-05 | Release v0.15.1 (the owner: "Once the compiler fix succeeds commit and release it with new version number"): the `impl`-file fix above; a patch version for a bug fix | `python3 tools/contract_tests.py --ceesafec out/ceesafec.rel2` (0.15.1) on macOS: every suite ok, fixed point, corpus, sanitizers, sweeps, packages 7/7; Linux-only checks skipped (fortify, io_uring) |
| 2026-11-05 | Fix (0.15.2), four bugs from the libraries' notes on 0.14–0.15 (`CeeSafeLibs/NOTES-ceesafe-0.14.md` §1, §4, §5, §7; the owner: "do A"): (1) `ceesafec test` of a library that only borrows an `AsyncTcpListener` panicked in the C backend (`types.cee:311`: a vector helper's `Result<void, AllocError>` asked for only by lib/std, now added to the used types with any vector), then (2) called `AsyncTcpListener`'s `drop` from drop glue without emitting it: the used library functions and the used types are now computed until each used type's drops are reached (`setUpEmitter`); (3) `Async.run(Async.scope(…))` (or `all`, `race`, `timeout`) in sync code compiled to nothing, so its result was never assigned: an operand of `Async.run`/`runWorkers` that is not a call of an async fn is N0342 (type-system.md §40.4); (4) the `test` harness listed a dependency's `@test` functions kept in its `src/` and could not call them (N0312): it lists the root package's only. Also: a character literal in `pushBE`/`writeBE` without a type argument is N0265 (it wrote four bytes; §16.6), and a string literal over 4095 bytes is emitted as a `ceeLS_<k>` byte array (c-backend.md, the `StringView` row), so `-Wpedantic` no longer warns. Fixtures `tests/packages/borrowed-listener` and `dependency-tests` (checks 8–9 of `tools/package_tests.py`); corpus `check-fail/async-run-operand`, `check-fail/byte-order-char-literal`, `run-pass/long-string-literals` | on macOS: both repros in `CeeSafeLibs/repros/` pass `ceesafec test` (0.15.1: the panic, then the C compiler's undeclared `ceeM_16AsyncTcpListener_drop`); package tests 9/9 (8–9 fail with 0.15.1); the long-literal program builds with no `-Woverlength-strings` warning (0.15.1: 3) and prints the same as the interpreter; corpus: every family passes, freestanding partial as before |
| 2026-11-05 | Release v0.15.2 (the owner: "do A", the bug fixes above as a patch release): the version set to 0.15.2 in `src/backend/c_program.cee` and `editors/vscode/package.json`; stage 0 stays ceesafec 0.3.2 | `sh tools/bootstrap_release.sh` with v0.3.2's osx-arm64 binary builds the release commit through the recorded chain, and its `build --emit c` equals the local compiler's; `python3 tools/contract_tests.py --ceesafec out/ceesafec` on the release commit, macOS: every suite ok (fixed point, corpus, sanitizers, sweeps, packages 9/9, spec, guide) except `reactor poll`, where one of 45 async programs failed once; seven reruns of that suite (four in turn, three at once) passed, so it is recorded as a timing flake; Linux-only checks skipped (fortify, io_uring) |
| 2026-11-05 | ADR 0072 (the owner: "Accept ADRs"), from the libraries' notes on 0.14–0.15: `a ?? b` (one token; right-associative, below `\|\|`; `b` evaluated only when `a` is null; lowered to a temporary and a null test), `o.isSome()`/`o.isNull()`, `==`/`!=` on fieldless enums and on arrays and slices of integers, `bool` and fieldless enums (a vector lent as its elements), a literal `for` range passed to a `usize` parameter typed `usize`, `.length` of a `const` array, and `let _ = e;` (type-system.md §3, §15.1, §16.2, §16.4, §22.1; grammar.md §2.1). Guide: `@effects` on C calls from async code, reserved words and built-in names, `Console.writeBytes`. Changes from the text in ADR 0072 §7.2 | on macOS: the corpus's five new run-pass programs (expected output checked by hand; compiled and interpreted agree), one check-fail and one parse-fail; results below the release row |
| 2026-11-05 | Release v0.16.0 (the owner: "Accept ADRs", ADR 0072 as one release, 0.16.0; "Commit and release"): the version set to 0.16.0 in `src/backend/c_program.cee` and `editors/vscode/package.json`; stage 0 stays ceesafec 0.3.2. `guide/for-agents.md`'s pitfalls "Arrays are not compared with `==`" and "A temporary is not sliced or measured" no longer failed under ADR 0072, so they now show arrays of structs (N0207) and a call's result (N0216) | `python3 tools/contract_tests.py --ceesafec out/ceesafec` (0.16.0) on macOS: every suite ok (fixed point, corpus, sanitizers, sweeps, packages 9/9, spec) except `guide`, two failures from the pitfalls above; after the fix `guide_check` and `spec_check` report 0 problems, and gen1/gen2 `build --emit c` of the release tree are identical; Linux-only checks skipped (fortify, io_uring) |
| 2026-11-05 | Fix (0.16.1), a stale wake from `Async.blocking` (`mailserver/CEESAFE_FINDINGS.md` §1.4, its `runtime-fix.patch`): the blocking thread sets `done`, then wakes the task, so a task that saw `done` first was woken after it moved on; the run loop readied it on that wake at its next suspension while its real registrations stayed, so as a scope's waiter it was readied twice when a child returned (the ready queue looped, or a finished task ran again), and a stale descriptor aborted a later wait. `cee_exec_loop` now takes a wake only for a task `on_wake` in that resume, and resets its deadline and descriptor as `cee_unpark` does; every cross-thread waker (signal, blocking, shared scope, channel, DNS) targets such a task. Corpus `run-pass/async-blocking-stale-wake` | the repro: 0.16.0 SIGSEGV in 4 runs of 4, the fix `ok` in 8 of 8; the corpus test fails with 0.16.0 (6 of 6 crash) and passes with the fix, under ThreadSanitizer and in the interpreter; the mail server's relay load test (`tests/conformance/run.sh --relay-load 1000`): 3 of 3 deliver all 1,000 (about 2.6 s), 0.16.0 stalls with 984 queued |
| 2026-11-05 | Release v0.16.1 (the owner: "Yes", to the stale-wake fix as a patch release): the version set to 0.16.1 in `src/backend/c_program.cee` and `editors/vscode/package.json`; stage 0 stays ceesafec 0.3.2 | `python3 tools/contract_tests.py --ceesafec out/ceesafec` (0.16.1) on the fix's commit, macOS: every suite ok (fixed point, corpus, sanitizers, `reactor poll` and `run tsan` 46/46, sweeps, packages, spec, guide); Linux-only checks skipped (fortify, io_uring) |
| 2026-11-05 | ADR 0073 (the owner: "Accept the ADR as proposed", then "Yes, implement it"), from the mail server port's findings (`mailserver/CEESAFE_FINDINGS.md` §2.2, §3.2, §3.3, §3.6): a temporary as a receiver, a slice base or a `.length` operand lives until the end of its statement (`make().view()`, `text.split(",").next()`, `digest()[0..4]`, `s.view()[1..3]`; a view kept after it is N0243); every `mut p` argument is two-phase, reserved for reads only when a later argument does more than read, and B5 finds the receiver's place left to right (`ps[i].set(bump(mut i))` borrowed the element after `bump`); a block branch of an `if` or `match` expression may end with its value; the first warning, N0400 for `{name}` in a string literal with `name` in scope, `\{`/`\}` escapes, `--deny-warnings`, warnings reported for the root package only. Changes from the accepted text in ADR §7.2 (receivers keep B5's any-use rule; reservations only before arguments that do more than read; the compiler's own `"^{commit}"` split in two) | corpus: every family passes with the new run-pass, check-pass, check-fail and parse-fail programs (compiled, `--interp`, `--check-leaks`), the emit-c and frames snapshots unchanged; package tests 10/10; guide 123 examples; the fixed point |
| 2026-11-05 | Release v0.17.0 (the owner: "Yes once pass, release this as 0.17.0"): ADR 0073; the version set to 0.17.0 in `src/backend/c_program.cee` and `editors/vscode/package.json`; stage 0 stays ceesafec 0.3.2. Release notes: `ps[i].set(bump(mut i))` now borrows the element named before `bump` runs (left to right, as language-spec.md requires; 0.16.1 borrowed it after), and N0400 is the compiler's first warning | `python3 tools/contract_tests.py --ceesafec out/ceesafec` on ADR 0073's tree, macOS: every suite ok (fixed point, corpus, sanitizers, sweeps, packages 10/10, spec, guide); Linux-only checks skipped (fortify, io_uring); gen1/gen2 `build --emit c` identical at 0.17.0; `ceesafec check` clean with no warnings, and `ceesafec test` passing, in all 38 packages and 19 test suites of CeeSafeLibs, the mail server, PostgresClient and Clay |
| 2026-11-05 | ADR 0074 (the owner: "Take over ~/dev/CeeSafe session and fix all those"; §6 answered as proposed), from the mail server port's remaining findings (`mailserver/CEESAFE_FINDINGS.md` §3.7, §3.9–§3.14, §4, §4.1): `Ok(v)` binds a unit payload, so generics work at `void`; `never` as a function's return type (`_Noreturn` in C); run-once environments, so an `async move` child of `s.spawn`, `set.spawn` or `Async.timeout`/`deadline`/`race`/`all` may move its captures out (the rest dropped when it returns); method region parameters (not in interfaces), and deep results, a result naming only a reference parameter's pointee regions not keeping its loan (callee rule N0245); `asBorrow`/`asBorrowMut` and `T?` places passed to `borrow T?`; raw `"""` literals (N0011); associated constants (`Type.NAME`); `for` over a value with `next(borrow mut this) -> T?` (`HirStmt.ForNext`); integer `checkedTruncate`/`saturatingTruncate`; a local may shadow a dependency (N0401). Built as five branches and merged; changes from the accepted text in ADR §7.2 | `python3 tools/contract_tests.py --ceesafec out/ceesafec-new2` on the merged tree, macOS: every suite ok (fixed point, corpus, -Werror, asan, tsan, fault/cancel/support sweeps, mutations, packages 12/12, guide 127 examples) except spec, then ok after regenerating `documents.cee`; Linux-only checks skipped (fortify, io_uring); corpus again after the N0208 hint: every family passes; `ceesafec check` with no diagnostics in all 35 packages of CeeSafeLibs and CeeSafeApps, and `ceesafec test` passing in their 17 suites (291 tests); the new callee rule rejected no code in the compiler, lib/ or those packages |
| 2026-11-05 | Release v0.18.0 (the owner: "Yes, merge and release 0.18.0"): ADR 0074; the version set to 0.18.0 in `src/backend/c_program.cee` and `editors/vscode/package.json`; stage 0 stays ceesafec 0.3.2. Release notes: a function returning a view of its reference parameter's own storage under a pointee region (`fn f(borrow mut this) -> Slice<r, u8> => this.buf[0..2]`) is now N0245 (none found in the three repositories); `let x = die();` with a `never` callee is N0224; N0315 no longer applies to locals, parameters and bindings | `python3 tools/contract_tests.py --ceesafec out/ceesafec` on the release tree (0.18.0 built from 0.17.0, then by itself), macOS: every suite ok (fixed point, corpus, -Werror, asan, tsan, sweeps, mutations, packages 12/12, spec, guide 127 examples); Linux-only checks skipped (fortify, io_uring); `ceesafec check` clean in all 35 packages of CeeSafeLibs and CeeSafeApps and their 17 test suites passing (on the merged tree before the version change) |
| 2026-11-05 | B18 (found by CeeSafe.Web on 0.18.0, its `docs/ceesafe-upstream-work.md` and probe `tests/probes/deep-result-borrow-bug.cee`; the owner: "Go fix B18 in the CeeSafe repo"), ADR 0074 §8: `fn headers(borrow this) -> borrow HeaderList<r> => borrow this.fields` was N0245, because §25.6 elided no region of a result naming one inside, so the result was a deep result of `this`; and `o.inner().first()` kept `o`'s own loan, because a deep result's argument contributed less only as `&p` or a reference parameter. Now a result reference with nothing written on it takes the single scoped parameter's elided region (`resolveFlows`, `src/check/references.cee`), and a call whose `borrow T` result passes through one reference parameter (`resultThrough`, `src/hir/hir.cee`) is followed to its argument (`throughDefinitions`, `deepSourceEdge`, `src/nir/borrow.cee`); a pointee naming no region that may hold loans is not followed. Spec type-system.md §25.6, lifetimes.md §7; corpus run-pass and check-fail `deep-results-through` | the probe prints 1 (0.18.0: N0245); the new run-pass case passes and 0.18.0 refuses it (three N0245), the check-fail case's annotations match (0.18.0: an N0245 at line 13, no N0240); negative cases unchanged (ADR 0074's `this.buf`, `borrow mut` from a shared `this`, a returned local, a slice of a returned `borrow Vector<u8>`, a move or a push while the result lives); `python3 tools/contract_tests.py --ceesafec out/ceesafec-b18b` on macOS: every suite ok (fixed point, corpus, -Werror, asan, tsan, sweeps, mutations, packages 12/12, spec, guide 127 examples), Linux-only checks skipped (fortify, io_uring); CeeSafeLibs and CeeSafeApps: 39 manifests check with no diagnostic, as with 0.18.0; CeeSafe.Web at `92f23fb` (unchanged): every package, example and fuzzer checks with no diagnostic and `fmt --check` clean |
| 2026-11-05 | Release v0.18.1 (the owner: "Commit and release it as 0.18.1"): B18 above, a patch release; the version set to 0.18.1 in `src/backend/c_program.cee` and `editors/vscode/package.json`; stage 0 stays ceesafec 0.3.2 | `sh tools/bootstrap_release.sh` with v0.3.2's osx-arm64 binary (`SHA256SUMS` checked) builds the fix commit and the release commit through the recorded chain (0.3.2, 0.6.0, 0.8.0), reporting `ceesafec 0.18.1`, and its `build --emit c` equals the local compiler's; `python3 tools/contract_tests.py --ceesafec out/ceesafec` on the release tree, macOS: every suite ok (corpus, -Werror, asan, tsan, sweeps, mutations, packages 12/12, spec, guide 127 examples) but the fixed point, whose two runs differed because the release commit was made while the suite ran (`fixed_point.py` archives `HEAD` once for each run, so run a built the fix commit's 0.18.0 and run b the release commit); `tools/fixed_point.py --commit c1982a2` alone afterwards: FIXED POINT; Linux-only checks skipped (fortify, io_uring); CeeSafe.Web at `92f23fb`, unchanged, with the fix's compiler: 43 runs passed in every mode (sanitizers, both sweeps), web_tls's interpreter run skipped |
| 2026-11-05 | Temporaries out of the debug info (the owner, after CosmoOS's release kernel outgrew its 64 MiB ESP: "Fix the compiler side too, in a worktree"): every NIR temporary was a function-wide C local, so an optimised `-g` build gave each one a debug variable with a location list over the whole function, repeated at every inlined call; CosmoOS's 83 generated files carried 4.8 bytes of `.debug_loclists` per byte of code, against 0.6 for its 218 C files, and most of its release kernel's 12.2 MB. `rt.h` gains `CEE_TEMP` (clang: `unused, nodebug`; GCC: `unused`, having no `nodebug` for variables; MSVC: nothing), and the C backend (`emitFunction`, `src/backend/c_body.cee`; an async fn's resume function, `src/backend/c_async.cee`) declares a temporary with it unless `#line` directives are on (`ceesafec build -g`), where it stays `CEE_UNUSED` for debuggers. Spec c-backend.md §7, §7a; the 24 `emit-c` snapshots edited by that one rule (an indented `CEE_UNUSED <type> t_<n>` declaration becomes `CEE_TEMP`), not regenerated | CosmoOS's `blk.cee` built as its release kernel builds it (clang, `-O2 -g`): `.debug_info` 59.2 KB to 38.5 KB, `.debug_loclists` 77.9 KB to 28.0 KB, `.text` unchanged (18.6 KB); `python3 tools/run_corpus.py` on the corpus: every family passes; `python3 tools/contract_tests.py --ceesafec out/ceesafec`, macOS: exit 0 |
| 2026-11-06 | Release v0.18.2 (the owner: "Go ahead and release"): the temporaries' `CEE_TEMP` above, a patch release; the version set to 0.18.2 in `src/backend/c_program.cee` and `editors/vscode/package.json`; stage 0 stays ceesafec 0.3.2 | `python3 tools/contract_tests.py --ceesafec out/ceesafec` on the release commit, macOS: every suite ok (fixed point, corpus, -Werror, asan, tsan, sweeps, mutations, packages 12/12, spec, guide), Linux-only checks skipped (fortify, io_uring); `sh tools/bootstrap_release.sh` with v0.3.2's osx-arm64 binary (`SHA256SUMS` checked) builds it through the recorded chain (0.3.2, 0.6.0, 0.8.0), reporting `ceesafec 0.18.2`, and its `build --emit c` equals the local compiler's |
| 2026-11-06 | ADR 0075 (the owner: "I want you to take over ~/dev/CeeSafe session and fix all the findings"), from the CosmoStore port's `CEESAFE_GAPS.md` G1–G22 (G8's `HashKey` half was already fixed): durable files (`File.openReadWrite`/`openOrCreate`/`createNew`, `readAt`/`writeAt`, `syncData`/`syncAll` with `F_FULLFSYNC`, `length`/`setLength`, `File.rename`, `Directory.sync`) and their `AsyncFiles` forms; `@from` variants for `?`, `Result.isOk/isErr/ok/err`, `okOr`; lib/std `OrderedMap` (B-tree, cursors) and `OrderKey`; `Crc32c`, `Crc64Nvme`, `Md5`, `Sha256`, `HmacSha256`; `Clock.monotonicNanos`; a field of a returned reference (through a vector) is a place; `assert`/`panic`; `[]`; an array's slice methods; `Vector.insert/remove/swapRemove`; `replace`/`swap`; `as T <`; `*r`; static arrays of atomics; `--sweep alloc` counting test files' unwraps apart; headers beside package C files; no N0265 after an argument's error. Also: a package's item may shadow a library item's name (C names `…_lib_…`), and a library static is emitted only where used. Changes from the accepted text and two compiler bugs found (N0272 with generics, N0212 across `?` under the kernel profile) in ADR §7 | corpus with `out/ceesafec-new`, macOS: every family passes (freestanding partial as before), the 9 new programs also under `--interp`; four snapshots regenerated where only library growth renumbered or reordered (ADR §7.11); fixed point gen2 → gen3 `--emit c` identical; CosmoStore's `checkpointDamageFailsOpen` under `--sweep alloc`: 63 sweep failures in 200 runs with 0.18.2, 0 failures with 63 counted as the tests' own; `python3 tools/contract_tests.py --ceesafec out/ceesafec` (gen2): every suite ok (fixed point, -Werror, asan, tsan, interp, support/hashmap/fault/cancel sweeps, mutations, packages, spec, guide 127 examples, debugger, mcp api) except the corpus, whose one failure, check-fail/systems-capabilities.cee's N0201 on shadowing `PhysAddr`, is ADR §7.9's intended change (expectation edited, rerun ok); Linux-only checks skipped (fortify, io_uring); `ceesafec check` clean in all 44 packages of CeeSafeLibs and CeeSafeApps (as with 0.18.2), and their 24 test suites plus CosmoStore's (54 tests) passing |
| 2026-11-06 | Release v0.19.0 (the owner: "Go ahead release 0.19.0"): ADR 0075; the version set to 0.19.0 in `src/backend/c_program.cee` and `editors/vscode/package.json`; stage 0 stays ceesafec 0.3.2. Release notes: a package's item may now shadow a lib/core or lib/std name (was N0201; check-fail/systems-capabilities.cee's expectation edited); `* 2` is a dereference (parse-fail/expression-errors.cee now uses `/ 2`); lib/std gains `OrderedMap`, `OrderKey`, `Crc32c`, `Crc64Nvme`, `Md5`, `Sha256`, `HmacSha256`, and lib/core `ResultMethods`, `OptionalResults`, `VectorEditing`; `assert`, `panic`, `replace` and `swap` are built-in functions a package's own items shadow. Before tagging, the seed build from 0.3.2 found two constructs 0.8.0 does not accept in the interpreter and one in the parser (a block value, `let _`, an array of `StringView`s), rewritten in ADR 0075's commit | `python3 tools/contract_tests.py --ceesafec out/ceesafec` on the release tree (0.19.0 built by ADR 0075's compiler, then by itself), macOS: every suite ok (fixed point, corpus, -Werror, asan, tsan, interp, sweeps, mutations, packages, spec, guide 127 examples, debugger, mcp api); Linux-only checks skipped (fortify, io_uring); `sh tools/bootstrap_release.sh` with v0.3.2's osx-arm64 binary (SHA256SUMS checked) at ADR 0075's commit: ok, and that compiler's `--emit c` equals its own rebuild's |
| 2026-11-06 | ADR 0075 §8 (the owner: "Fix the two bugs first, then migrate CosmoStore"): N0272 no longer rejects a referent's own contents going back into it (a `-> T` result is a deep result; a `borrow mut` parameter's deep placeholder may flow into itself), and library functions that do not check in a freestanding build are left out of the NIR checks (the N0212 across `?`); `File.readAt`/`writeAt`/`syncData`/`syncAll`/`length`/`setLength` take `borrow this`, which CosmoStore's migration needed (its shim, `unsafe`, adapters and own checksums are gone; CosmoStore f5a3d9f) | corpus every family passes with the two new programs (run-pass `generic-values-back-into-this`, which 0.19.0 rejects; check-fail `values-into-this-still-checked`); contract suite on the bug-fix tree: every suite ok; on the final tree every suite ok but `run interp`, whose failure did not reproduce when the family was run alone with the machine idle (316 of 316: a timeout under the load of other sessions' VMs); fixed point; `tools/bootstrap_release.sh` from v0.3.2 at the commit, and the seed's own rebuild emits the same C; CosmoStore: check, fmt and its 53 tests pass, and its benchmark matches ADR-0005's (100k objects: GET+verify 392k/s, recovery from a checkpoint 52 ms) |
| 2026-11-06 | Release v0.19.1 (the owner: "Commit and release 0.19.1"): ADR 0075 §8 above, a patch release; the version set to 0.19.1 in `src/backend/c_program.cee` and `editors/vscode/package.json`; stage 0 stays ceesafec 0.3.2. Release notes: N0272 accepts storing values taken out of a `borrow mut` parameter back into it; File's durable methods take `borrow this` (code for 0.19.0 still checks) | as the row above |
| 2026-11-06 | ADR 0076 (the owner: "yes", to a manifest key so CosmoStore can pin its compiler; CosmoStore's CEESAFE_GAPS G23): `[package] ceesafec = "<requirement>"`, the dependencies' requirement syntax; a ceesafec that does not satisfy it reports N0321 at the value (`this package needs ceesafec >=0.19.2; this is ceesafec 0.19.1`), for the root and every dependency, before anything is checked; a `-dev` version counts as its MAJOR.MINOR.PATCH; a compiler older than the field reports it as unknown at the same line | package_tests check 13 (unsatisfied: N0321 naming both; satisfied: checks); by hand: `>=`, a range, caret, `*`, a malformed requirement, and a dependency's requirement at `fetch`; contract suite: every suite ok; `src/` builds with 0.8.0; fixed point |
| 2026-11-06 | Release v0.20.0 (the owner: "Yes", to commit ADR 0076, release it as 0.20.0 and pin CosmoStore to it): ADR 0076, a minor version for a new manifest field; the version set to 0.20.0 in `src/backend/c_program.cee` and `editors/vscode/package.json`; stage 0 stays ceesafec 0.3.2. Release notes: `[package] ceesafec = ">=0.20.0"` states the compiler a package needs; older compilers report the field as unknown | `tools/bootstrap_release.sh` from v0.3.2 at ADR 0076's commit, and the seed's rebuild emits the same C; on the release tree: corpus, spec check and package tests pass; the contract suite passed on ADR 0076's tree (the release commit changes only the version) |
| 2026-11-06 | ADR 0077 (the owner: "While we wait, fix CeeSafe gap you found", then the mail server's diskio): `Console.flush()`; `Directory.space(path)` (`statvfs`/`GetDiskFreeSpaceExA`); `IoError.StorageFull` (ENOSPC, EDQUOT, ERROR_DISK_FULL), appended after HostNotFound; `File.openAppend`; `File.modified`; `File.setPermissions` (chmod). With them CeeSafeLibs' diskio 0.2.0 is plain CeeSafe with its 0.1.0 API (its C shim and errno constants are gone). Interpreter: `Directory.space`, `File.modified` and `File.setPermissions` answer Unsupported | corpus: new run-pass `console-flush`, `volume-space`, `file-append-metadata` (natively and with --interp); three HIR snapshots regenerated where only closure numbers moved; by hand: a flushed line survives SIGKILL with stdout redirected and an unflushed one does not; `Directory.space` matches `df`; a filled 2 MB disk image gives StorageFull; a 0o640 file is mode 640; diskio under umask 000 makes 0750 directories and a 0640 file; the 24 diskio-dependent mail server packages (main, -dav, -eas, -ews) check clean and the main tree's 17 suites pass (151 tests); `src/` builds with 0.8.0; fixed point; contract suite on the tree: every suite ok |
| 2026-11-06 | Release v0.21.0 (the owner: "commit and release 0.21.0"): ADR 0077; the version set to 0.21.0 in `src/backend/c_program.cee` and `editors/vscode/package.json`; stage 0 stays ceesafec 0.3.2. Release notes: `IoError.StorageFull` follows `HostNotFound`, so a `match` naming the first twenty variants with no `_` is N0256; new: `Console.flush`, `Directory.space`, `File.openAppend`, `File.modified`, `File.setPermissions` | `tools/bootstrap_release.sh` from v0.3.2 at ADR 0077's commit, and the seed's rebuild emits the same C; on the release tree: corpus, spec check and package tests pass; the contract suite passed on ADR 0077's tree (the release commit changes only the version) |
| 2026-11-08 | ADR 0078 (CeeSafe.Web's B19 and U22; the owner: "Fix B19 and U22 in CeeSafe", then "Go ahead and release CeeSafe"): on Linux, each `sendfile` blocks SIGPIPE on its thread and takes one it raised (after a failed call, or with a partial count), so `sendFile` to a reset connection is an error, not exit 141; `f.modified()` and `f.kind()` from the open file (`fstat`, lib/std's `FileMetadata`); `File.openRegular(path)` (mode 6: `O_NONBLOCK | O_NOCTTY`, `fstat`, cleared; `IsDirectory`, `InvalidInput`); `AsyncFiles.openRegular`, `modified`, `kind`. Interpreter: `modified` Unsupported, `kind` and `openRegular` from the path | corpus: new run-pass `async-send-file-reset` (0.21.0 exits 141 on Linux under epoll and io_uring; so does a runtime that took the signal only after a failed call, 6 of 6) and `file-open-regular` (a mutant without `O_NONBLOCK` times out on the FIFO); both natively and with --interp on macOS arm64, Debian 12 (Linux 6.1; epoll, io_uring) and Windows 11 ARM64 (IOCP, WSAPoll); fault sweep of the new test (85 points); three HIR snapshots regenerated where only closure numbers moved; by hand: CeeSafe.Web's benchmarks/http built with it survived 80 clients dropping mid-file and h2load's resets and then served 1 GiB whole, both reactors; `tools/bootstrap_release.sh` from 0.3.2 (the chain's 0.8.0 builds `src/`); fixed point; corpus native and interpreted; contract suite: every suite ok |
| 2026-11-08 | Release v0.22.0 (the owner: "Go ahead and release CeeSafe"): ADR 0078; the version set to 0.22.0 in `src/backend/c_program.cee` and `editors/vscode/package.json`; stage 0 stays ceesafec 0.3.2. Release notes: on Linux, `sendFile` to a reset connection is an error, never SIGPIPE; new: `File.openRegular`, `f.modified()`, `f.kind()`, `AsyncFiles.openRegular`, `AsyncFiles.modified`, `AsyncFiles.kind` | ADR 0078's tree: bootstrap from 0.3.2, fixed point, corpus, contract suite (above); the release commit changes only the version and this record |

**Tests run for the skeleton** (2026-10-30, the reference compiler 0.3.1, Apple clang 21.0.0):
- `ceesafec check` and `ceesafec fmt --check`: clean.
- `ceesafec build`, then `./out/ceesafe --version`: printed `ceesafec 0.0.0-dev`, exit 0.
- No arguments: exit 2. An unknown command: exit 2.
- `ceesafec run --interp -- --version` printed the same version.

**Tests run for P0** (2026-10-30, macOS arm64, Apple clang 21.0.0, Python 3.9.6):
- `tools/run_corpus.py --compiler the reference compiler` (0.3.1), all 457 files: 454 passed, 0 failed, 3 partial
  (freestanding: object builds only).
- The same run with the reference compiler `07d561f` against `--reference` 0.3.1: 0 differences.
- Against a deliberately wrong compiler: every affected file failed, as expected.
- `tools/inventory.py` run twice gave byte-identical output, with no host paths.
- The reference's `dotnet test the reference.slnx` at `07d561f`: 3,589 passed, 0 failed, 0 skipped.

**P0 gate on the Ryzen box** (2026-10-30, Debian 12 x86-64, Python 3.11.2; the reference compiler 0.3.1 linux-x64, sha256
`8bd8d81b…d973`; the reference `07d561f`; CeeSafe `ef3e413`). Each run used `nice -n 15` and 6 jobs:

| Run | Result |
|---|---|
| inventory | byte-identical to the committed file |
| all 457 files, default `cc` (GCC 12.2.0) | 454 pass, 0 fail, 3 partial |
| run-pass, run-fail, ffi, freestanding with clang 18.1.8 `-O2` | 199 pass, 0 fail, 3 partial |
| run-pass, run-fail, ffi with clang 18.1.8 `-O1` and ASan+UBSan (`-fno-sanitize-recover=all`) | 199 pass, 0 fail |

A probe build confirmed the flags took effect: the sanitizer runtime was linked in, and the clang build came from
clang 18.1.8.

**P1 probes** (2026-10-30): the 12 probes and the lexer's 525-file run, on macOS and on the Ryzen box (Debian 12),
under GCC 12, clang 18 and Apple clang, with and without ASan+UBSan. All passed. The commands and results are in
`docs/p1-probes.md` §1.

**P1 support library and gate** (2026-10-30; `docs/p1-probes.md` §7):
- macOS:
  - 6 support tests, all matching their oracles, with and without ASan+UBSan;
  - the fault sweep, 573/573;
  - a planted rollback bug and a planted leak were both caught.
- Ryzen box (Debian 12, CeeSafe `28fdb5b`), all with `--check-leaks`:
  - the support tests under GCC 12 and under clang 18 with ASan+UBSan+LSan: 6/6 each;
  - the probes under both: 8/8 each;
  - The reference's run-pass, run-fail and ffi corpus: 199/199 with no leak;
  - the fault sweep under clang ASan+LSan: 573/573.
- The earlier gate run found the missing-drop bug as a LeakSanitizer report.

**P5 flat C backend** (2026-10-30; `docs/p5-plan.md`): C identical to the reference compiler's for 284/284 programs in four forms and
for CeeSafe itself; run-pass, run-fail and ffi pass under Apple clang 21, clang 18 and GCC 12 in every configuration
(one GCC report that the reference compiler's C shares); the fault sweep (789 points), the collection properties and the freestanding
checks pass on macOS and Linux.

**P6 fixed point** (2026-10-30; `docs/p6-fixed-point.md`): gen 1 by the reference compiler 0.6.0, gen 2 and gen 3 by CeeSafe; C2 = C3
and gen 2 = gen 3 on macOS (Apple clang 21) and Linux (GCC 12), in two directories each, with the same C on both
hosts; only the C compiler runs from gen 1 on; `tools/contract_tests.py` passes on gen 2 on both hosts.

**P6R rename** (2026-10-30; Decision 0002): CeeSafe's sources renamed (`8941c10`), bootstrap anchor `3d6ac40`; the
fixed point on CeeSafe names with a bootstrapped ceesafec as stage 0 on macOS and Linux, the same C on both; legacy,
renamed and mixed package fixtures pass; contract tests pass on both hosts.

**P7 production C** (2026-10-30; `docs/p7-plan.md`): closure specialisation, placement, check elimination and the
structured form, on by default; C identical to the reference compiler 0.6.0's default form over 308 programs in four shapes and for
CeeSafe itself, the four reports identical; the contract suite in the default form on macOS and Linux; the fixed
point in the default form and with `--split` on both hosts, the same C on both. Decision 0004: the C prefix switches
at P8.

**P8 tools and the fork point** (2026-10-31; `docs/p8-plan.md`, `docs/fork-point.md`): recorded per slice in
`docs/p8-plan.md`; the fixed point after the fork on macOS, Linux and Windows (`bootstrap/fixed-point/2026-10-31-p8s7-*`).

**CeeSafe's specification** (2026-10-31; Decision 0006, `docs/spec-plan.md`, commit `e401024`, macOS, Apple clang 21):
the contract suite, 36 steps, all ok (`spec_check`: 75 documents, 132 codes, 100 links, 0 problems; `guide_check`: 92
examples, 0 problems; the CLI and servers through `tools/reference_docs.py`: `fix` 177/177, MCP 433/433, LSP 438/438;
CeeSafe's own C identical after mapping back, 535,154 lines and 623,749 with `--split -g`); the fixed point from a
bootstrapped stage 0 in the default form and with `--split` (`bootstrap/fixed-point/2026-10-31-spec-*`). Linux and
Windows not rerun for this commit.

**CosmoOS integrations** (2026-10-31; `docs/cosmoos-integrations.md`, macOS arm64, QEMU, CosmoOS `9c0d0f84`): the reference's
C1–C9 and K3 checks forked into `integrations/cosmoos/` and run with ceesafec: C1 (with the 199-program corpus in the
guest), C2 (412 sweep runs), C3, C5, C7 and C8 booted on x86_64; C4 on both arches; C6, C9 and K3. All pass. C9 found
a ceesafec bug (a closure with an `unsafe { e }` body crashed the capture scan), fixed with a regression program in
`tests/check/`; the contract suite passes after the fix (36/36). Not run: aarch64 boots, C8 `--cycles`.

**Release v0.1.0** (2026-10-31; `docs/release.md`): the contract suite on the 0.1.0 build (36/36); two dry runs of
`release.yml` (the first found `cl`'s objects written into the seed directory), then the tag: six platforms, each
binary built from the Linux seed and emitting it byte for byte (osx-x64 checked under Rosetta), macOS signed and
notarized (Accepted), published with SHA256SUMS at github.com/vkuttyp/CeeSafe/releases/tag/v0.1.0;
`install.sh` from the site installs it and builds a program. `main` is 0.2.0-dev.

**Release v0.2.0** (2026-10-31; `docs/release.md`; Decision 0007): the first CeeSafe-only release. The contract suite,
now CeeSafe-only (13 steps), on the commit before the version change: every step ok but `spec`, which needed the
embedded documents regenerated (then 0 problems); the fixed point at 0.2.0; one dry run of `release.yml`, then the tag:
six platforms from one seed, macOS signed and notarized, published at github.com/vkuttyp/CeeSafe/releases/tag/v0.2.0.
The released osx-arm64 archive matches `SHA256SUMS`, reports `ceesafec 0.2.0`, is accepted by Gatekeeper as notarized,
and builds and runs a program. The VS Code extension `ceesafe-language-0.2.0.vsix` (tested with 20/20 tests against
0.2.0) was attached by hand, with its line added to `SHA256SUMS`; from the next tag the workflow's `extension` job
builds and attaches it. Since this release the workflow runs only on a pushed `v*` tag. `main` is 0.3.0-dev.

**The VS Code extension on the Marketplace** (2026-10-31): the publisher `ceesafe` (created by the owner), with the
logo; `ceesafe.ceesafe-language` 0.2.0 published by the owner with `vsce` 4.1.0 from the build with the icon and the
store README, which also replaced the `.vsix` on the v0.2.0 release (its `SHA256SUMS` line updated); the gallery
lists it public with its icon. The secret `VSCE_PAT` is an all-organizations token, which Azure DevOps retires on
2026-12-01: so the `stores` job now publishes by Marketplace Trusted Publishing (`vsce publish --oidc`, the job with
`id-token: write`), under the publisher's policy "Release workflow" (owner `vkuttyp` #146238, repository `CeeSafe`
#1440301724, workflow `release.yml`; Active, created by the owner 2026-10-31), so no Marketplace secret is stored; it is
first exercised by the next tag. Open VSX: the namespace `ceesafe` exists, its ownership claim is
EclipseFdn/open-vsx.org#15060; until then its step skips.

**Release v0.3.2** (2026-11-01; Decision 0009): the first release bootstrapped from ceesafec 0.2.0 and the first with
a C seed; v0.3.0 (the reference compiler's download refused) and v0.3.1 (the extension's packaging) failed in the
workflow and published nothing. Six platforms from one seed, the seed checked on three systems, the extension on
the Marketplace by trusted publishing (§10).

**ClayWind port findings, ADR 0063 (proposed) and ADR 0064 (draft)** (2026-11-03; uncommitted, not released). This
work was asked for by the owner ("explore ~/dev/CeeSafe and tackle those findings") after the pure CeeSafe port of
ClayWind (`~/dev/CeeSafeApps/CEESAFE-FINDINGS.md`).
- **ADR 0063, implemented on the working tree:**
  - a `borrow mut` variable or field is reborrowed into `borrow mut dyn I`. Before, it was moved (N0250), and a field
    was N0251;
  - `for` ranges of flexible literals are `usize` when the body indexes with the variable (§16.4,
    `src/check/range_index.cee`);
  - integer `min`/`max` (§6; `bfIntMin`/`bfIntMax`, `CEE_MINMAX` in `rt.h`);
  - N0246 suggests a copy, with a fix;
  - effect explanations name an interface method without `@effects`.
- **ADR 0064:** a draft of the larger proposals (generic methods and inherent `impl`, arrays and slices of views,
  `dyn I` as a bound, OD-10a), for the owner to decide; none of it is implemented.
- **Run with `out/ceesafec.cand4`, built from the tree, macOS arm64:**
  - `tools/contract_tests.py`: every contract ok except `spec`, whose one problem was stale embedded documents.
    After `tools/gen_docs.py` and a rebuild, `spec_check` reports 0 problems.
  - `tools/run_corpus.py`: 580 tests, 0 failures. Five tests are new; the `dyn-reborrow` pair fails with 0.10.3.
  - The fixed point: the changed compiler rebuilds itself to byte-identical C (`--emit c`, `diff -rq`).
  - `guide_check`: 103 examples, 0 problems. The guide's for-agents range example now compiles; its failing example
    is a mixed `usize`/`i32` use.

**ADR 0063 and ADR 0064 accepted and implemented** (2026-11-04; uncommitted, not released). The owner wrote "I
accept the ADR", then chose "Both 0063 and 0064" and, for `abs`, "Both".
- **ADR 0063 §2.5:** `abs()`, which panics at the minimum as `-x` does, and `unsignedAbs()`.
- **ADR 0064, all of it:**
  - §1: generic methods, `x.m<T>(…)`, and inherent `impl S { … }`;
  - §2: arrays and slices of views, and constant views;
  - §3.1: `dyn I` implements `I`, used only behind references;
  - §3.2 (OD-10a): a scoped `T` behind `borrow dyn I`.
- **Run with `out/ceesafec`, built from the tree, macOS arm64:**
  - `tools/contract_tests.py`: 29 contracts ok. The one failure, `spec`, was stale embedded documents; after
    `gen_docs.py` and a rebuild, `spec_check`, `guide_check` and `debugger_check` report 0 problems.
  - `run_corpus.py`: 596 tests, 0 failures. Sixteen tests are new across both ADRs. Six expectations that encoded
    the old rules were changed in place, each with a note (ADR 0064 §5.5).
  - The fixed point: byte-identical C.
  - ClayWind's seven packages and CosmoStack build.

**ADR 0065, the agent-facing reference** (2026-11-04; uncommitted, not released). The owner wrote "Go ahead implement
1-4".
- **The library reference for agents:**
  - `spec/api/std.md`, checked by `spec_check` like `core.md`;
  - `guide/builtins.md`, built-in types and functions, whose every row a type-checked example verifies;
  - both API pages in `llms-full.txt`.
- **`ceesafe_api`:** an MCP lookup by name, with suggestions; `tools/mcp_api_check.py` is a new contract.
- **"Did you mean" in N0200, N0230 and N0234,** and reserved words named in N0100.
- **`guide/for-agents.md` and `templates/AGENTS.md`:** five new checked sections and four lines.
- **Run with `out/ceesafec`, macOS arm64:**
  - `guide_check`: 117 examples, 0 problems. `spec_check`: 0 problems. `mcp_api_check`: 15 lookups, 0 problems.
  - Corpus: 596 tests, 0 failures. The fixed point: byte-identical C. `tools/contract_tests.py`: all 31 contracts ok, the
    new `mcp api` among them.

`main` is 0.8.0.

Not yet run: the rest of P9 (§9 item 4).
