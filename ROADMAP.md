# CeeSafe roadmap: port the Nova compiler to Nova, as CeeSafe

Date: 2026-10-30 (revises the 2026-10-29 draft). Status: **P0–P7 complete (CeeSafe names since `8941c10`; P7 production C 2026-10-30; stage 0 novac 0.6.0 via the `3d6ac40` anchor); P8 complete (2026-10-31; the fork point taken at S7, `docs/fork-point.md`)**.
The probes, `src/support/`, the limits, the fault sweep and the skeleton exist (`docs/p1-probes.md`). No compiler code has been ported.
Repository: `~/dev/CeeSafe`. Reference: `vkuttyp/Nova` (C#/.NET 10), pinned in `bootstrap/reference.md`.

**Identity** (decision 0001):
- The public language is **CeeSafe**. The compiler is `ceesafec`.
- Sources use `.cee`. Packages use `ceesafe.toml` and `ceesafe.lock`, and `$CEESAFE_HOME`.
- Nova remains a living language. It is CeeSafe's stage-0 compiler and its behaviour oracle.
- **Until `ceesafec` compiles itself, CeeSafe's own sources are `.nova` files with a `nova.toml`**, because novac reads
  only those. P6R renames them in one commit.

**The work resolves Nova's OD-33.** That is "a Nova compiler written in Nova", recorded by Nova ADR 0023 as option C.
Nova ADR 0001 chose C# and expected self-hosting only "for a long time".

## 1. Scope and completion criteria

There are three deliverables:

1. **Compiler parity.** This covers:
   - lexing, parsing and package names;
   - types, generic checking and instantiation;
   - HIR and NIR;
   - ownership, borrowing, effects and destruction;
   - every optimisation;
   - portable C emission.
2. **Verified self-hosting.** novac builds `ceesafec` (generation 1). Generation 1 builds generation 2, and generation 2
   builds generation 3. Generations 2 and 3 must emit byte-identical C and pass the compiler tests.
3. **Product parity and handover.** The complete CLI, interpreter, package manager, formatter and doc generator, the
   LSP and MCP servers, fixes and JSON diagnostics, debugger scripts, runtime bundling and release/install all work
   without .NET.

Self-compilation alone does not complete the port. Accepting the compiler's own sources says nothing about whether the
borrow checker is correct. No reduced language subset will be presented as a replacement.

The C runtime (`runtime/`, 2,898 lines) and the Nova libraries (`lib/core`, `lib/std`, `lib/kernel`) are carried over
with their provenance and licence (GPLv2). The compiler's algorithms stay in Nova. Invoking the C# compiler to parse,
check, emit or run tooling does not count as a port. The C compiler, linker, debugger and git remain external tools,
used by the commands that need them.

## 2. Reference audit (2026-10-30)

The audit read Nova `main` at `07d561f` (0.4.0-dev, clean tree) and the released novac 0.3.1 (`v0.3.1` = `4481e12`).
Paths below are relative to `~/Nova`.

### 2.1 Size

Physical lines of `.cs`, excluding `bin/` and `obj/`. These measure scope, not effort.

| Component | Files | Lines | Notes |
|---|---:|---:|---|
| `src/Nova.Compiler` | 58 | 36,559 | |
| `src/Nova.Cli` | 1 | 1,211 | 18 commands |
| `tests/Nova.Compiler.Tests` | 41 | 6,673 | 39 test classes |
| `tests/Nova.Fuzz` | 3 | 1,919 | random program generator |
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

Corpus `.nova` counts:

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

### 2.2 Can Nova host a compiler today? Not yet

The 2026-10-29 draft assumed CeeSafe could be written "within the pinned stage-0 language". The audit shows the
language core is adequate, but its library and host support are not.

| Area | Present | Missing | Evidence |
|---|---|---|---|
| Core language | enums with payloads and `match`, recursive `Box`, generics with interface bounds, `dyn`, closures, `T?`/`Result` with `?`, statics, threads, multi-file packages | `From` conversion for `?`; `impl<T> I for G<T>` and `Self`; `i128` | `docs/type-system.md` §15, §17.4, §20–§21.1, §36–§39 |
| Collections | `Vector`, `String`, `SlotMap`/`Handle`, `StableVector`; `sortBy` | **hash map, set and ordered map**; slice search | §23–§24, §16.5; `lib/core/slices.nova` |
| Strings | `append`, `appendNumber`, boundary-checked slicing, `==` | hashing, ordering, clone, char/UTF-8 iteration, integer and float parsing, `find`/`startsWith`; growing only from a `StringView` | §22.2, §23.2 |
| Host services | read/write/remove files, stdin bytes, stdout/stderr, argv, `Process.run` (status only), TCP | environment, cwd, directories, stat, mkdir/rename, temp files, clock, **captured child output**, stdin to a child | §26; `runtime/hosted/nova_rt_io.c` |
| Borrowing | IDs and handles, field-precise and constant-index disjointness, `splitAtMut` | any non-constant index aliases every element, so table-heavy code must copy out or use handles | `docs/lifetimes.md` §4, §8 |
| FFI | `extern "C"`, `.c` files linked by `build`, `@repr(C)` structs | `run --interp` runs no C | §28; `docs/unsafe.md` |
| Scale | novac checks a 202k-line package in 13.1 s | the largest Nova program in the repository is ~360 lines; no parser has been written in Nova | ADR 0022; `lib/core/atomics.nova` |

This is the port's largest risk, and the reason for the **N track** (§4) before and alongside P1–P3. P1's probes
(`docs/p1-probes.md`) narrowed it to three needs:
- **N0**, a compiler crash;
- **N1a**, turning bytes into text;
- **N4**, host services.

Maps, generic impls, `From` and borrow precision have accepted Nova-side patterns.

### 2.3 C# surfaces that need a native design

| Finding | Evidence |
|---|---|
| The pipeline order is fixed | `src/Nova.Compiler/Compilation.cs:76-129`. The order: core libraries, parse, `PackageNames.Rewrite`, `TypeChecker`, `NirBuilder`, then per function `InPlaceChecker`, `MoveChecker`, `BorrowChecker`, then `DropElaborator`, then `EffectChecker` |
| Spans are UTF-16 code units over UTF-8 input | `Syntax/SourceText.cs:3`; `docs/grammar.md` §1.1 |
| Interned names get deterministic first-seen IDs | `Syntax/SymbolTable.cs:3` |
| Layout comes from the target, not the host | `Semantics/Types.cs:22` (`TargetInfo`); `docs/c-backend.md` |
| Exact integers through `BigInteger` | 6 files: `IntegerSemantics`, `FloatSemantics`, `Types`, `TypeChecker`, `CEmitter`, `CheckElimination` |
| Width-specific float evaluation and formatting | `Semantics/FloatSemantics.cs:16`; type-system §35 |
| Reflection walks the trees | `Tooling/SemanticIndex.cs:177-280`; `Semantics/TypeChecker.cs:3981`, `:6833` |
| JSON through `System.Text.Json` | `McpServer.cs`, `LanguageServer.cs`, `DiagnosticJson.cs` |
| Hash collections whose order may reach output | `TypeChecker.cs` (58 `Dictionary`, 40 `HashSet`), `CEmitter.cs` (12, 17), `CheckElimination.cs`, `BorrowChecker.cs`; explicit sorts at `Fixer.cs:35`, `CheckElimination.cs:156` |
| Embedded resources: libraries, runtime, docs, templates | `CoreLibrary.cs`, `Backend.C/CBackend.cs:54-63`, `Documentation/EmbeddedDocs.cs`, `Packages/AgentFiles.cs` |
| The CLI is broad | `src/Nova.Cli/Program.cs:225-335`. Commands: `parse check build run fmt doc new agents fetch publish index-check fix explain mcp lsp` |
| Tests cover mutations, allocation failures and several C configurations | `docs/testing.md` §3–§5; `MutationTests.cs`, `FaultInjectionTests.cs`, `MsvcTests.cs` |

### 2.4 Nova changes since the 2026-10-29 draft

The draft's baseline was `06fd7cd`. The current ADR range is **0001–0027**. Both of these are new:
- **0026**, inline atomics: the atomic types moved to `lib/core`, in every profile.
- **0027**: `here` in static initialisers, `Mutex.constant`, `layoutOf<T>()`, raw `read`/`write` for any `T`, and
  `ObjectCache<T>`.

novac 0.3.0 and 0.3.1 are released. M14 is still in progress: K2 (typed calls across units) and K6 are open.

## 3. Architecture and porting rules

### 3.1 Preserve semantics, change storage

- Follow the current grammar, ownership invariants and accepted ADRs. A needed language addition goes through Nova
  (the N track), never into compiler-only syntax or intrinsics.
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
- Exact integers: a small signed-magnitude limb integer in Nova, sized to the audited `BigInteger` uses. Never use
  unchecked `i64` where the reference is exact.
- Floats: binary32/64 parsing, rounding, NaN, ±0, infinities and decimal formatting must match `FloatSemantics.cs` and
  `runtime/hosted/nova_rt_float.c`. Locale-dependent libc is not an oracle.
- Host services (files, paths, environment, processes, clocks, temp files, tool discovery) sit behind one small layer.
  Ideally it is N-track library API. A C adapter is allowed only with a written contract covering ownership, lengths,
  encoding, errors and cleanup. Processes take argument arrays and never shell strings.

### 3.3 Repository layout

```text
CeeSafe/
  ROADMAP.md AGENTS.md CLAUDE.md .mcp.json   (agent files from novac new --agent claude)
  nova.toml                                  -> ceesafe.toml at P6R
  src/                                        compiler, in Nova (one package; files share one scope)
    support/ syntax/ diagnostics/ semantics/ hir/ nir/ analysis/
    backend_c/ interpreter/ packages/ formatting/ documentation/ tooling/ host/  (planned)
  runtime/ lib/ resources/                    carried over at P5 with provenance
  bootstrap/reference.md                      stage-0 pin
  docs/decisions/                             CeeSafe decisions
  tests/ tools/ benchmarks/                   (planned)
  out/                                        ignored
```

The package compiles as one whole program, as novac does today; Nova has no separate compilation (ADR 0013 D4).
Subdirectories under `src/` are only for organisation, since every `.nova` file under `src/` shares one scope.

### 3.4 Rename contract

| Surface | Nova today | CeeSafe | When |
|---|---|---|---|
| Compiler | `novac` | `ceesafec` | skeleton now; parity P8 |
| Source extension | `.nova` (`Packages/Manifest.cs:271`, `Program.cs:697`) | `.cee` | P6R |
| Manifest and lock | `nova.toml`, `nova.lock` (`Manifest.cs:32`, `Workspace.cs:28`) | `ceesafe.toml`, `ceesafe.lock` | P6R; conversion in P8 |
| Home | `$NOVA_HOME`, `~/.nova` (`Workspace.cs:111`) | `$CEESAFE_HOME`, `~/.ceesafe` | done (P8 S3) |
| Generated C | `nv_`, `nvMk_`, `NOVA_PROGRAM_*_H`, `nova_exports.h` | `cee_`, `cee<X>_`, `ceesafe_exports.h` (Decision 0004) | done (P8 S7) |
| Runtime | `nova_*` (~374), `nv*` (~424), `NOVA_*` (10), `runtime/include/nova/rt.h` | `ceesafe_*`, `CEE_*`, `ceesafe/rt.h`, ABI version (Decision 0004) | done (P8 S7) |
| Diagnostics | 126 `N####` codes (`Diagnostics/Diagnostic.cs:14`), used by 159 corpus files | letter open, with a mapping | P6R |
| MCP/LSP | server `novac`, tools `nova_*` | `ceesafec`, `ceesafe_*` | done (P8 S5) |
| Agent files | `<!--NOVA START/END-->`, `nova.mdc` | `<!--CEESAFE START/END-->`, `ceesafe.mdc` | done (P8 S3) |
| Editor | `nova-language`, language id `nova` | `ceesafe-language`, language id `ceesafe` | done (P8 S6) |
| CosmoOS | `.nova`, pinned novac | stays on Nova (Decision 0003); a migration is the owner's option | — |

Nova's generated-C ABI is kept through the first fixed point. Renaming never changes ownership, lifetime, drop or
safety semantics, and it is never done by text substitution.

## 4. Phases and gates

Every phase delivers working code, positive and negative tests, an updated parity ledger and risk table, and a dated
entry in §10. An unchecked item is planned, not done.

### P0 — Foundation: pin, identity, inventory and runner

Dependencies: none. **Complete (2026-10-30)**: the gate passed on the Ryzen box (§10).

- [x] Scaffold the package with the pinned novac (`novac new ceesafe --agent claude`), and write a `ceesafec` skeleton
  that handles `--version` and `--help` (2026-10-30).
- [x] Decision 0001 covers identity, the bootstrap route, the upstream N track and Nova continuing. Pin novac 0.3.1 and
  the Nova reference in `bootstrap/reference.md` (2026-10-30).
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
- [x] Run the reference suite (2026-10-30). `dotnet test Nova.slnx` at `07d561f` on macOS arm64 passed 3,589 of 3,589
  tests, with 0 skipped, in 12 min 24 s. No real GCC was available (`gcc` is Apple clang), and no MSVC (that needs the
  Windows VM). Whether TSan ran is not shown at default verbosity. A configuration Nova's suite cannot find counts as
  missing, not as passed.
- [x] Gate check (2026-10-30). On the Ryzen box (Debian 12, x86-64): the novac 0.3.1 release download matched the
  release's `SHA256SUMS`; Nova was cloned at `07d561f`, and CeeSafe from a git bundle of `ef3e413`. Results:
  - `tools/inventory.py` regenerated `docs/reference-inventory.md` byte-identical;
  - the runner gave the Mac's results: 454 pass, 0 fail, 3 partial.

Carried into later phases (`docs/parity.md` §3):
- the rest of the C matrix: `-O0`, the overflow fallback, `--split`, TSan (P5);
- freestanding symbol checks (P5);
- CLI fixtures for the 20 codes the corpus does not annotate. N0310–N0315 are needed by P3, the rest by P8.

Gate: another checkout reconstructs the pin and runs a representative fixture set with identical results, and the
ledger has no unmapped items (inventory: 0 unmapped, 2026-10-30).

### N — Upstream Nova: self-hosting prerequisites (Nova milestone M15)

Each item is a Nova change approved by the owner and made under Nova's rules: spec and ADR first, then tests, docs,
`roadmap.md` and `risks.md`. It ships in a novac release, and CeeSafe re-pins (`bootstrap/reference.md`). P1's probes
(2026-10-30, `docs/p1-probes.md` §4) set this list and its order.

All of N0–N4a shipped in **novac 0.4.0** (Nova M15, tag `v0.4.0` = `66b6bda`, 2026-10-30), and CeeSafe re-pinned to
it the same day, then to the point release 0.4.1 (N7; `bootstrap/reference.md`). Each former
`probes/upstream/` repro is now a regression probe that must keep passing.

- [x] **N0. The compiler crash** ([Nova #2](https://github.com/vkuttyp/Nova/issues/2)) on a generic `Result` rebuild
  with `T = void` is now N0222, an error (`probes/check-fail/void-result-rebuild.nova`). The instance still does not
  check, so `void` results keep their non-generic lifts.
- [x] **N0b. The missing drop** ([Nova #1](https://github.com/vkuttyp/Nova/issues/1)) of a condition temporary is
  fixed, and Nova's run corpus now requires `live=0` (`probes/run-pass/condition-temporaries.nova`).
- [x] N0c. The two false rejections ([Nova #4](https://github.com/vkuttyp/Nova/issues/4),
  [#5](https://github.com/vkuttyp/Nova/issues/5)) are fixed (`probes/run-pass/truncate-two-phase.nova`,
  `probes/run-pass/generic-param-borrow.nova`); their workarounds in `src/support/` and `tests/support/` are gone.
- [x] **N0d. A nesting limit** ([Nova #3](https://github.com/vkuttyp/Nova/issues/3)): 1,000 levels, N0110, with the
  passes on a 256 MiB stack. CeeSafe's parser must reproduce N0110 at the same depth (P2).
- [x] **N1a. Bytes to text**: `StringView.fromUtf8` (type-system §22.2), then `String.append`.
- [x] **N4a. Host services** (ADR 0028, type-system §26.7–§26.10): `Process.capture` (stdout, stderr, stdin input),
  `Environment.variable`, `Directory.create`/`list`/`remove`/`temporary`/`current` and
  `File.kind`. `probes/run-pass/host-process.nova` uses them to drive `cc`.
- [x] ~~N4b. Stdin by line or length~~: dropped; LSP/MCP framing can be buffered over `Console.read`.
- [x] **N7. lib/std's helper names** (found by the 0.4.0 re-pin): 0.4.0's `lib/std/host.nova` declared top-level
  helpers (`pathIn`, `fromAlloc`, `textOf` and others) that a program could not redeclare (N0201). Fixed in novac
  **0.4.1** (point release from `release-0.4`, 2026-10-30), CeeSafe's pin since; the probes use those names again.
- [ ] Optional ergonomics, none blocking:
  - N1b, `Vector` bulk append from a slice (performance);
  - N1c, text ordering, `clone`, `find`/`startsWith` and number parsing (until then, byte-wise functions in
    `src/support/`);
  - N1d, arrays of `StringView` (until then, `match` tables).
- [x] ~~N2 library maps~~: not needed upstream. A generic insertion-ordered map works in Nova (`generic-map` probe) and
  lives in `src/support/`.
- [x] ~~N3 `impl<T>`, `Self`, `From`~~: not needed. Use one impl per instantiation, `K: Key<K>`, and lift functions
  (`generic-impls`, `error-layers` probes).
- [x] ~~N5 borrow precision~~: not needed. `splitAtMut`, copy-out and separate tables are accepted
  (`aliasing-workarounds` probe).
- [x] N6. Nova `docs/milestone-15.md` (drafted at `34ce40a`, implemented 2026-10-30), citing OD-33. Its items map to
  this list as follows: S1 is N0 (#2), S2 is N0b (#1), S4 and S5 are N0c (#4, #5), S3 is N0d (#3), S6 is N1a and S7
  (ADR 0028) is N4a. All shipped together in 0.4.0; the planned 0.3.2 was not needed.

Gate per item: the novac release passes Nova's full suite, and CeeSafe's affected probes pass after the re-pin.

### P1 — Probes and the support library

Dependencies: P0. Runs alongside the N track.

- [x] Capability probes (2026-10-30; `probes/`, `docs/p1-probes.md`). There are 12, covering:
  - ID tables;
  - owning enums;
  - index aliasing, and the accepted workarounds;
  - a generic map;
  - a lexer and interner over all 525 Nova source files;
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
- [x] Grow the skeleton (2026-10-30). `ceesafec` knows novac's 15 commands, loads the files they name into
  `SourceText` (`src/host/files.nova`), reports unreadable files, and names the phase that implements each command.

Gate: probes and the support library pass under ASan and UBSan with the pinned novac. Every compiler bug found has a
reduced reproducer filed upstream. Missing features block; they are never worked around by weakening checks.
**Passed 2026-10-30 (§10).** The four reproducers in `probes/upstream/` and the nesting crash were filed as Nova #1–#5 (2026-10-30), fixed in
novac 0.4.0, and the reproducers became regression probes at the re-pin.

### P2 — Source frontend

Dependencies: P1's text and storage, N1.

- [x] Port `SourceText`, the line index, interning, token kinds, the keyword table (`Keywords.Table`), literals,
  trivia and doc comments, and the lexer (2026-10-30; `docs/p2-frontend.md`). Checked by a token-dump oracle built
  from the reference's own `Lexer`: byte-identical over all 540 `.nova` files of Nova `v0.4.1`, 21 edge cases and
  3,000 random inputs. One documented difference: an invalid escape of a character above U+FFFF.
- [x] Diagnostics, their text rendering and the JSON document (`--diagnostics-format=json`), with summaries generated
  from the reference's documents (`tools/gen_catalog.py`).
- [x] Port the AST and the recursive-descent/Pratt parser: imports, attributes, generics and regions, patterns,
  closures, FFI, recovery, and splitting adjacent `>` (2026-10-30). The AST is tables of `Copy` nodes
  (`docs/p2-frontend.md` §5). The `else if` chains and `T???…` are built and printed in loops, so they cannot exhaust
  the stack where the reference does (§6).
- [x] Add `parse` and `--dump-ast` with byte-identical dumps.

Gate: all 40 parse-pass and 28 parse-fail fixtures match, including AST snapshots and exact diagnostics. Every `.nova`
file in Nova's repository and CeeSafe's own sources parse. Fuzzed malformed input ends with a controlled error.
**Passed 2026-10-30** on macOS and the Ryzen box (`docs/p2-frontend.md` §4):
- the parse families match novac 0.4.1 with no difference in exit status, stdout or stderr;
- all 540 `.nova` files of Nova `v0.4.1` give identical trees and diagnostics;
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
as pending P4. This is not yet a full `check`. **Passed (2026-10-30, novac 0.5.0)**: `docs/p3-plan.md` (slices S0–S8,
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
  - the sticky shared-deref soundness fix (N0248, Nova `3a99b4a`).
- [x] Port the mutation expectations (`MutationTests.cs` as `tools/mutation_suite.py`). The fault-injection sweep
  (`FaultInjectionTests.cs`) builds and runs programs, so it moved to P5's gate.

Gate: all 128 check-fail and 27 check-pass fixtures and the mutation expectations pass. NIR before and after drops
agrees with the reference. Every invariant in `docs/ownership.md` has mapped positive and negative evidence. `check`
now means the full pipeline. **Passed (2026-10-30, novac 0.6.0)**: `docs/p4-plan.md` (slices S0–S6) and
`docs/p4-evidence.md`. The results:

- **Fixtures:** all 136 check-fail and 27 check-pass fixtures give the reference's diagnostics. Of the 322 check
  programs, 318 are identical in text, JSON and `--dump-hir`. The other four are recorded divergences:
  - `enums`, `optionals` and `results`: novac's `--dump-hir` crashes on them;
  - `offset-of`: Nova #6.
- **Mutations:** all 1,152 mutants match the oracle.
- **NIR:** the 284 programs novac builds have identical NIR after drop elaboration. novac prints NIR only after
  elaboration, so the comparison is made there.

### P5 — Flat C backend and the native build

Dependencies: P4, N4 (process capture).

- [x] Port naming, type ordering and layout, declarations, aggregates, function/closure/vtable lowering, the runtime
  helpers, constants, drops, source maps, panic sites, FFI and imported structs, callbacks, exports and object mode
  (`src/backend/`, 2026-10-30).
- [x] Bundle `runtime/` (`tools/gen_runtime.py`) and `lib/` with deterministic tables. Construct `cc` and linker
  arguments, and publish output files (`src/host/toolchain.nova`, `src/host/build.nova`).
- [x] Add `build --emit c|exe|obj` with flat C, `-g`, `--line-directives`, `--split`, `--cc`, `--cflag`, `--ld`,
  `--program` and `.c` inputs. The switches of P7's forms (`--flat-c`, `--no-placement`, `--no-bce`, `--no-oce`,
  `--no-closure-specialisation`) are accepted: ceesafec always emits that form.

Gate: run-pass (153), run-fail (40), ffi and freestanding pass under clang -O0/-O2, the overflow fallback and
ASan+UBSan, with GCC where available. The M3 fault-injection sweep (`FaultInjectionTests.cs`, moved from P4) passes on
every run-pass program that allocates, which shows O4 at run time. `emit-c` snapshots match, or each difference is reviewed. Objects for both
CosmoOS architectures pass. **Passed (2026-10-30, novac 0.6.0)**: `docs/p5-plan.md` (S0–S3). The results:

- **C:** byte-identical to novac's (`--flat-c` and the optimisations off) for all 284 programs novac builds, in four
  forms each: flat, `--split`, `-g`, `-g --split`. CeeSafe's own C is identical too (394,094 lines; ceesafec 1.4 s,
  novac 4.1 s), on macOS and on Linux, and a ceesafec built from it by cc or GCC behaves the same.
- **Behaviour:** run-pass 159/159, run-fail 40/40, ffi 8/8 under Apple clang 21 (`-O2`, `-O0`, the overflow fallback,
  `-O1` ASan+UBSan with leak checks, `--split`, `-g`, all `-Werror`), clang 18 and GCC 12 on the Ryzen box; TSan on the
  three thread programs. One recorded divergence, the reference's: GCC 12 `-O2 -Werror` reports `-Wuninitialized` in
  an enum constructor of `run-pass/nested-patterns`, for novac's C too, flat and default.
- **Fault sweep:** 50 run-pass programs allocate; all 789 failure points end as accepted, on macOS and Linux, and each
  run equals the run of novac's build.
- **Collections:** the 30 programs of `CollectionPropertyTests` (`tools/collection_properties.py`) print their models'
  output under every configuration, their C equals novac's, and their 156 failure points pass the sweep.
- **Freestanding:** 3/3 host objects run under their harnesses with only allowlisted undefined symbols; both CosmoOS
  triples have no forbidden sections, mutable state or COMMON symbols, also joined by `ld.lld`
  (`tools/freestanding_check.py`), as for novac.
- **emit-c snapshots:** 7 of 24 match. The 17 others are novac's default form (structured, placement, check
  elimination, closure specialisation: P7). Reviewed: novac in the flat, unoptimised form fails the same 17.

### P6 — First verified fixed point

Dependencies: P5 and every feature CeeSafe uses.

- [x] Gen 1 is built by the pinned novac from CeeSafe's `.nova` sources.
- [x] Gen 1 builds gen 2, and gen 2 builds gen 3, with identical inputs, flags and toolchain.
- [x] Gen 2 and gen 3 emitted C and resources are byte-equal. Both pass the compiler contract tests
  (`tools/contract_tests.py`).
- [x] Repeat from clean directories under different absolute paths. Check that no timestamp or path leaks into output.
- [x] Gen 2 onward builds with no `dotnet` or novac on `PATH`, and the subprocess logs are audited.

Gate: a reproducible fixed point with archived manifests. This is a checkpoint, not release readiness. **Passed
(2026-10-30, CeeSafe `1b1aae4`, stage 0 novac 0.6.0)**: `docs/p6-fixed-point.md`, manifests in
`bootstrap/fixed-point/`. On macOS (Apple clang 21) and Linux (GCC 12): C2 = C3 = C4 and gen 2 = gen 3 bit for bit,
in two directories each; the C is the same on both hosts; from gen 1 on only the C compiler runs; every contract test
passes on gen 2 but the recorded divergences.

### P6R — Rename cut-over

Dependencies: P6.

- [x] `ceesafec` accepts `.cee`, `ceesafe.toml` (and `ceesafe.lock`). `$CEESAFE_HOME` is named (Decision 0002); nothing
  reads a home directory until P8's git and registry dependencies. `.nova` and `nova.toml` are kept as an explicit
  legacy mode (Decision 0002), then dropped the same day by Decision 0003: Nova and CeeSafe exist in parallel, so
  ceesafec reads only `ceesafe.toml` packages; files named on the command line may have any extension.
- [x] The diagnostic code letter stays **N** (Decision 0002: `C` would collide with MSVC's codes), so no mapping and
  no corpus conversion is needed.
- [x] One commit (`8941c10`) renames CeeSafe's own sources and manifest; `3d6ac40` is the bootstrap anchor that novac
  still builds (`tools/bootstrap.sh`). The P6 fixed point reruns with ceesafec alone; novac is no longer needed for
  routine builds (formatting waits for P8's `fmt`).

Gate: the fixed point holds on CeeSafe names, and the legacy mode (if kept) is tested. **Passed (2026-10-30,
`8941c10`)**: from `tools/bootstrap.sh` (novac 0.6.0 → `3d6ac40` → this tree), the fixed point with that ceesafec as
stage 0 holds on macOS (Apple clang 21) and Linux (GCC 12): gen 1 = gen 2 = gen 3 = stage 0, the same C on both hosts,
which differs from the legacy-named commit's only in the 80 `src/` file names; only the C compiler runs. Legacy mode:
`compare_packages.py` passes the seven fixtures as they are (`nova.toml`), renamed (`ceesafe.toml`, `--renamed all`)
and mixed (a CeeSafe root over legacy dependencies, `--renamed root`); a directory with both manifests is an error,
and a legacy package ignores `.cee` files. `tools/contract_tests.py` passes on both hosts.

### P7 — Optimisations and production C

Dependencies: P6R. Ownership and drop schedules do not change.

- [x] Port placement and escape analysis, `Box.create` built in place, bounds and overflow check elimination, closure
  specialisation, and every `--explain-*` report (S0–S2).
- [x] Port `CStructurer` (structured C), `--flat-c`, `--split` and `--program`, with deterministic unit names (S3).
- [x] ~~Switch to the approved CeeSafe C prefix and ABI version~~: moved to P8 by Decision 0004 (accepted 2026-10-30),
  so the C stays byte-identical to novac's while CeeSafe tracks Nova.
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
  - a one-way conversion of a Nova package to a CeeSafe package (`nova.toml`, `nova.lock`, `.nova`), if wanted:
    ceesafec does not read Nova packages (Decision 0003).

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
  `ceesafe/rt.h`, `CEESAFE_ABI_VERSION`), with a link-collision test: one program linking a novac-built and a
  ceesafec-built library (S7; `tools/link_collision.py`).
- [x] The fork point (Decision 0003 §3): taken at S7 (`docs/fork-point.md`). CeeSafe's corpus starts there
  (`tests/reference/`), and the novac comparisons are regression tests against novac 0.6.0's output with CeeSafe's
  names mapped back. CeeSafe's own specification followed P8 (Decision 0006, `docs/spec-plan.md`): Nova's `v0.6.0`
  text forked into `spec/` and `guide/`, Nova's ADRs inherited, and ceesafec embeds it.

Gate: every command and flag in the P0 ledger is implemented, or has an accepted decision. The contract suites pass.
No C# fallback.

### P9 — Harden, release, hand over

Dependencies: P8.

- [ ] Run the full acceptance matrix (§6) on both architectures, hosted and freestanding.
- [ ] Run a recorded 24-hour differential fuzz campaign against novac, with optimisations off and on, flat and
  structured C, closures, `dyn`, threads and numeric edges. Port the generator to Nova.
- [ ] Measure compiler throughput and peak memory against novac. Run it on the Ryzen host with `nice`.
- [x] Release `ceesafec` for the six platforms, with checksums, licences, install scripts and macOS notarisation:
  v0.1.0, 2026-10-31, on the owner's word (`docs/release.md`; every platform's binary built from one C seed and
  checked to emit it).
- [ ] Ship a source-release C seed (in the spirit of ADR 0023 option A), verified to build with a C toolchain alone.
- [ ] Build and run `ceesafec` on CosmoOS userland, on both architectures, including self-compilation.
- [ ] Have the safety passes and `unsafe` adapters reviewed independently when a reviewer is available.
- [ ] Optional (Decision 0003: CosmoOS stays on Nova): migrate CosmoOS (`.nova` to `.cee`, the pin to `ceesafec`) one
  component per reviewed, reversible PR, if the owner takes it up.
- [ ] Document the point at which CeeSafe becomes the supported compiler, and the frozen C# reference.

Gate: parity, fixed point, CLI parity and release evidence are complete. Hosted CosmoOS support, an outside audit and
publication are reported separately.

### Language track L1 — async/await (ADR 0031)

CeeSafe's own language work after the fork (Decisions 0006, 0007). The design is `docs/async-await-proposal.md`; the
decision will be ADR 0031. Each phase is spec first, and each must keep the fixed point and `tools/contract_tests.py`
green.

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
- [ ] **A2, scopes and cancellation**, in four steps, each closed by the contract suite:
  - [x] **A2a, many tasks** (2026-10-31):
    - async closures;
    - `Async.scope` and `spawn`, with the scope-loan rule (`scopeIn` moves to A2b);
    - a run queue and wakers in the runtime;
    - the interpreter scheduler, which gives each task its own activation stack.
  - [x] **A2b, cancellation** (2026-10-31; `scopeIn` moved to A3, the owner's choice):
    - the cancel flag, `checkpoint`, `isCancelled`, `shield`;
    - `race` and `all`;
    - `CEESAFE_CANCEL_AT` and `tools/cancel_sweep.py`.
  - [ ] **A2c, threads:**
    - `Async.blocking`;
    - channels (ADR 0032, accepted 2026-10-31);
    - wakes from other threads.
  - [ ] **A2d, I/O:**
    - kqueue and epoll beside `poll()`;
    - async files and sockets;
    - `Async.Mutex`.

  Gate: the cancellation sweep and the determinism check pass.
- [ ] **A3, sizes:** frames shrunk by liveness; `--explain-frames`; `Async.boxed` and `Async.scopeIn` (both allocate
  frames from a user allocator, through generated per-allocator adapters); async interface methods; the debugger
  `tasks` command.
- [ ] **A4, platforms:** io_uring and IOCP; a freestanding executor hook and a CosmoOS boot; `Send` work stealing.

## 5. Bootstrap protocol (planned interfaces; none exist yet)

```text
gen1: ~/.nova/0.4.1/bin/novac build              (pinned stage-0, CeeSafe .nova sources)
gen2: out/gen1/ceesafec build -o out/gen2/ceesafec  --emit c kept in out/gen2/c/
gen3: out/gen2/ceesafec build -o out/gen3/ceesafec  --emit c kept in out/gen3/c/
tools/compare-stages out/gen2 out/gen3            (byte equality of C and resources)
tools/run-fixtures --compiler out/gen2/ceesafec --reference ~/.nova/0.4.1/bin/novac
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
| Native semantics | stdout, stderr, status, panic sites against novac and the interpreter | P5 |
| C backend | snapshots, strict C11 warnings, determinism, FFI, source maps | P5–P7 |
| Self-hosting | gen2 equals gen3, no .NET on `PATH`, CeeSafe names | P6, P6R, P8 |
| Optimisations | each switch on and off, explanation reports, fuzz | P7 |
| Tools and protocols | CLI, packages, JSON and fixes, fmt, doc, LSP, MCP, editor | P8 |
| Portability | macOS, Linux, Windows; arm64 and x64; clang, GCC, MSVC | P9 |
| CosmoOS | C and object ABI checks, kernel integrations, both architectures, hosted compiler | P5–P9 |
| Robustness | sanitizers on the compiler and its programs, OOM sweeps, malformed input, fuzz campaign | ongoing |

Comparison policy:
- Byte equality where a contract promises it (dumps, JSON, protocol streams, gen2 against gen3), and novac-equal C
  where the backend is unchanged.
- Normalisation is narrow and versioned. It never hides a missing diagnostic, check, drop or panic location.
- A known reference bug gets a reproducer, a spec citation and a separate expected outcome. Expected results are never
  regenerated from CeeSafe to make a comparison pass.
- A skipped configuration is reported as skipped, not as a pass.

## 7. Risks

| Risk | Mitigation | Blocks |
|---|---|---|
| **Nova lacks library and host support for a compiler** (§2.2) | N track upstream; P1 probes decide the order | P1–P5 |
| N-track throughput: each item is spec, ADR, implementation and release in Nova | batch items per novac release; start with N1/N2, which the frontend needs first | P2–P3 |
| Index aliasing makes table-heavy code awkward | P1 ID/copy-out prototypes; N5 only with evidence | P2–P4 |
| No precedent for a large Nova program (~360 lines today) | P1 probes at scale; measure novac check and build time as `src/` grows | P1 on |
| Stage-0 drift as Nova evolves | pin a release; rebaseline only at gates (`bootstrap/reference.md`) | all |
| Stage-0 compiler bug exposed by CeeSafe | reduced reproducers, fixed upstream, re-pin | dependent phase |
| Numeric or text differences silently change acceptance | exact integers, UTF-16 tests, independent boundary oracles | P3 |
| Translation loses safety or drops | invariant-to-test map, mutations, NIR comparison, sanitizers | P4–P9 |
| Generated C differs without an explanation | deterministic tables and order, reviewed diffs, fixed point | P5–P9 |
| Incomplete rename breaks sources, packages or ABI | rename contract (§3.4), P6R legacy decision, reversible CosmoOS PRs | P6R–P9 |
| Host services drift into C | adapter contracts and a dependency inventory | P1, P8 |
| Servers leak compilations | ownership per request, repeated-edit tests | P8 |
| A hidden C# fallback | builds with `PATH` isolated, subprocess audit | P6, P8 |
| Scope grows past reviewable changes | port pass by pass; gate each phase | all |

A change to language semantics, memory architecture, public compatibility, supported targets or the bootstrap trust
policy needs a decision record: in Nova for the language, in CeeSafe for the compiler and its identity.

## 8. Order

```text
P0 foundation ──> P1 probes + support lib ──> P2 frontend ──> P3 types/HIR ──> P4 NIR + safety
                    │  ▲                         ▲               ▲
                    ▼  │                         │               │
                 N track (upstream Nova M15: N1 strings, N2 maps, N3 generics/From, N4 host, N5?)
P4 ──> P5 flat C ──> P6 fixed point ──> P6R rename ──> P7 optimisations ──> P8 tools ──> P9 release
```

## 9. Immediate work queue

1. **Pinned to novac 0.5.0** (2026-10-30; 0.4.1 before it): M15 S1–S7 and the N7 fix shipped, every P1 workaround removed. The N
   track has no open blocking item.
2. P2–P5 are done (`docs/p2-frontend.md`, `docs/p3-plan.md`, `docs/p4-plan.md`, `docs/p5-plan.md`), pinned to
   novac 0.6.0; the C matrix is complete (P5). P6, the first verified fixed point, holds on macOS and Linux
   (`docs/p6-fixed-point.md`). P6R is done (Decision 0002): CeeSafe's sources are `.cee` with `ceesafe.toml`,
   legacy `.nova` packages still build, codes keep N, and `tools/bootstrap.sh` builds from novac. P7 is done
   (`docs/p7-plan.md`): ceesafec emits novac's production C (structured, placement, check elimination, closure
   specialisation) byte for byte, and reaches the fixed point in that form. P8 is in progress (`docs/p8-plan.md`):
   `explain`, `fix`, `fmt`, `doc`, the package commands, `run --interp` and the `lsp` and `mcp` servers are identical to
   novac's (S0–S5); S6 adds the package-mode fixed point on macOS and Linux, Windows with MSVC and clang (the same
   C), the debugger scripts and the VS Code extension. S7 took the fork point: CeeSafe's C names and ABI version,
   `.cee` only, its own corpus, novac 0.6.0 the frozen baseline (`docs/fork-point.md`). P8 is complete. CeeSafe's
   own specification is written (Decision 0006: `spec/`, `guide/`, `docs/spec-plan.md`); next, P9.
4. Upstream (Nova #6, filed 2026-10-30): novac 0.6.0's `offsetOf<T>(field)` is 0 unless `T`'s layout was computed before (CheckOffsetOf reads
   `member.Offset` without `TypeTable.LayoutOf(measured)`); ceesafec gives the true offset.
5. Owner's call: propose upstream that N0110 count `else if` links and `?` suffixes. novac's `--dump-ast` aborts on a
   20,000-link `else if` chain (`docs/p2-frontend.md` §6).

Progress is measured in completed contracts and gates. There is no calendar estimate before P1 produces evidence.

## 10. Execution record

| Date | Work actually performed | Result |
|---|---|---|
| 2026-10-29 | Read-only inspection of Nova and an empty CeeSafe; first roadmap draft; owner chose a full rename | planning only |
| 2026-10-30 | Cross-checked the draft against Nova `07d561f` and the `docs/` specs. Owner decisions recorded as decision 0001. Roadmap rewritten. | Draft found stale: baseline, ADRs to 0027, counts, pipeline citation, and Nova's missing self-hosting prerequisites (§2.2) |
| 2026-10-30 | `~/.nova/bin/novac new ceesafe --agent claude` (0.3.1) in a scratch directory, copied into `~/dev/CeeSafe`; `git init`; skeleton `src/main.nova`; `bootstrap/reference.md` | done |
| 2026-10-30 | P0 inventory, ledger and runner: `tools/inventory.py`, `docs/reference-inventory.md`, `docs/parity.md`, `tools/run_corpus.py`; novac built from `07d561f` into a scratch directory for comparison | done; findings in `docs/parity.md` §3 |
| 2026-10-30 | P0 gate on the Ryzen box | passed; P0 complete |
| 2026-10-30 | P1 probes (`probes/`), the report (`docs/p1-probes.md`), and an upstream crash reproducer (`probes/upstream/`) | 12/12 pass; N list revised |
| 2026-10-30 | P1 support library, oracles (`tools/oracles/`), fault sweep, limits, skeleton loading; `run_corpus.py --with` and `--check-leaks`; 4 more upstream reproducers | all pass; P1 gate passed |
| 2026-10-30 | Filed Nova issues #1–#5 (the P1 upstream defects; each reproduced again on `origin/main` 07d561f first) | filed |
| 2026-10-30 | Drafted Nova M15 (`docs/milestone-15.md`, Nova `34ce40a`, pushed) | proposed; awaiting the owner |
| 2026-10-30 | Nova M15 S1–S6 implemented and pushed (Nova `4126457`, CI green); issues #1–#5 closed; ADR 0028 (S7) proposed. Checked against this repository with novac at `4126457`: probes 11/12 (the 12th, `text-gaps`, expects `StringView.fromUtf8` to be missing), support tests 6/6, fault sweep 573/573 | awaiting a release (0.3.2 / 0.4.0) to re-pin |
| 2026-10-30 | Nova M15 S7 implemented (ADR 0028 accepted; Nova `61cb665`, `1d1f091`, CI green on Linux and Windows; `c3.py` passes in a CosmoOS aarch64 guest): `Process.capture`, `File.kind`, `Environment.variable`, `Directory` | M15 complete; re-pin waits for a release |
| 2026-10-30 | Re-pinned to novac 0.4.0, then 0.4.1 (`80d8d8c`). 0.4.0 reserved its lib/std helper names (N7), fixed in Nova `24106b0` and released as the point release 0.4.1. All P1 workarounds removed; the four upstream reproducers became regression probes. Gates rerun with 0.4.1: macOS (Apple clang 21) and the Ryzen box (GCC 12; clang 18.1.8 -O2; clang 18.1.8 ASan+UBSan+LSan) | reference corpus at `v0.4.1` 467 pass, 3 partial, no leaks, on both; probes 16/16; support 6/6; fault sweep 573/573; skeleton builds; inventory regenerated, 0 unmapped |
| 2026-10-30 | Re-pinned to novac 0.5.0 (`af071d4`, ADR 0029) at P3 S2b, before any S2b code. lib/ unchanged, the checker change is S7's (FFI). Gates rerun on macOS (Apple clang 21) only; the Ryzen box keeps 0.4.1 until its next gate run | probes 16/16; support 8/8; type table 1/1 with no leaks; reference corpus at `v0.5.0` 468 pass, 3 partial; parse 483/483 identical; check 99/320 identical (as with 0.4.1); fault sweep 573/573 |
| 2026-10-30 | P2: lexer, diagnostics and JSON, AST tables, parser and `--dump-ast` (`src/syntax/`, `src/diagnostics/`); token-dump oracle from the reference's `Lexer`; `compare_tokens.py`, `compare_parse.py`, `lexer_fuzz.py`, `parser_fuzz.py`, `gen_catalog.py` | gate passed on macOS and Linux (`docs/p2-frontend.md` §4): parse families 68/68 with no reference difference, 540/540 reference files identical, 5,000 mutated and 3,000 random inputs identical, sanitizers clean, `live=0` |
| 2026-10-30 | P3: the type checker (`src/check/`, `src/hir/`), slices S0–S8 (`docs/p3-plan.md`), and package mode for `check` (`src/host/packages.nova`, path dependencies) | P3 gate passed: check-pass 27/27 and the 27 HIR snapshots identical; check programs 265/320, the other 55 differ only by P4's diagnostics or novac's printer crashes; `ffi`, `freestanding`, `emit-c`, `placement`, `bounds-checks`, `overflow-checks`, `closure-calls`, `run-fail` 83/83; package fixtures 7/7; `ceesafec check` of CeeSafe itself identical to novac (HIR dump, 35,150 lines); ASan+UBSan clean |
| 2026-10-30 | Re-pinned to novac 0.6.0 (`93e0a46`, ADR 0030) after P3; `offsetOf`/`callerAddress` ported; every completed gate rerun (`bootstrap/reference.md`) | all pass; one upstream bug found (`offsetOf` before layout is 0) |
| 2026-10-30 | P4: NIR (`src/nir/`: model, printer, builder, move paths, drop elaboration, move/in-place/borrow/effect checkers, `passes.nova`), `build --dump-nir`, diagnostic notes, `tools/compare_nir.py`, `tools/mutation_suite.py`, `docs/p4-evidence.md`; P3 gaps found and fixed (type-parameter flow names, `Box.create`'s `@placed` methods, HIR's dead `isGeneric`) | P4 gate passed: check programs 318/322, the 4 recorded divergences; other families 84/84; NIR 284/284 and CeeSafe's own package identical; mutations 1,152/1,152 against the oracle, as with novac; `check --dump-hir` identical on 186/186 check-pass and run-pass programs; packages 7/7; `ceesafec check` of CeeSafe clean (0.77 s); ceesafec under ASan+UBSan over 295 check-fail and run-pass programs (`build --dump-nir`) and its own package: no report, `live=0` |
| 2026-10-30 | P5: the flat C backend (`src/backend/`), the runtime embedded (`tools/gen_runtime.py`), the toolchain and `build --emit c|exe|obj` with `-g`, `--split`, `--cc`, `--cflag`, `--ld` (`src/host/toolchain.nova`, `src/host/build.nova`); `tools/compare_c.py`, `tools/fault_injection.py`, `tools/collection_properties.py`, `tools/freestanding_check.py`; P3 gaps fixed (built-in items' `Used` marks, HirProgram's Structs and DynSignatures, InHostedLibrary), one NIR span fixed (an exhaustive match's Unreachable) | P5 gate passed: C identical for 284/284 programs in four forms (flat, split, `-g`, both) and for CeeSafe itself; run-pass 159/159, run-fail 40/40, ffi 8/8 under Apple clang 21, clang 18 and GCC 12 (`-O2`, `-O0`, overflow fallback, ASan+UBSan with leak checks, split, `-g`, `-Werror`) but one GCC report that novac's C shares; TSan 3/3; fault sweep 789 points on macOS and Linux, equal to novac's runs; collection properties 30/30 with 156 swept points; freestanding host and both triples clean; emit-c differences reviewed (P7's form); ceesafec built from its own C by GCC emits the same C on Linux |
| 2026-10-30 | P6: `tools/fixed_point.py`, `tools/contract_tests.py`, `bootstrap/fixed-point/` | fixed point on macOS and Linux: C2 = C3 = C4, gen 2 = gen 3 bit for bit, in two directories, the same C on both hosts, only `cc` (and GCC's `as`, `ld`) started from gen 1 on; all contract tests pass on gen 2 but the recorded divergences |
| 2026-10-30 | P6R: Decision 0002 (legacy `nova.toml`/`.nova` kept; codes keep N), `ceesafe.toml`/`.cee`/`ceesafe.lock` packages (`3d6ac40`), CeeSafe's own sources renamed (`8941c10`), `tools/bootstrap.sh`, `compare_packages.py --renamed` | gate passed: fixed point on CeeSafe names from a bootstrapped ceesafec on macOS and Linux, same C on both, only `src/` file names differ from the anchor's C; package fixtures legacy, renamed and mixed 7/7 each; contract tests pass on both hosts |
| 2026-10-30 | P7: closure specialisation (`src/nir/closure_specialisation.cee`), placement (`src/nir/control_flow.cee`, `src/backend/placement.cee`), check elimination (`src/backend/check_elimination.cee`), structured C (`src/backend/c_structurer.cee`), all default on with their switches and `--explain-*` reports; `tools/compare_reports.py`, `tools/compare_self.py`, the contract suite and fixed point in the default form; Decision 0004 (C prefix at P8) | P7 gate passed: C identical to novac's default form over 308 programs (flat, `--split`, `-g`, both) and for CeeSafe itself; the four reports identical; emit-c 24/24; run corpus under Apple clang 21, clang 18 and GCC 12 (the recorded GCC report aside) and ASan+UBSan; fault sweep 783 points and collections 30/30 on both hosts; fixed point with production defaults and `--split` on macOS and Linux, the same C on both |
| 2026-10-31 | Benchmarks and the first CeeSafe-only C (Decision 0007): `benchmarks/` (16 workloads against Rust, Rust with overflow checks and C; `run.py`), an ablation of ceesafec's C finding the costs; `runtime/` CeeSafe's own (`tools/gen_runtime.py` reads it; the regenerated embedding identical but its header); arena bump allocation inline (`ceesafe_region_alloc_fast`); zeroed boxes through `ceesafe_alloc_zeroed` (`ceeBZ_`, `calloc`); `emit-c` snapshots CeeSafe's own; `src/support/` standalone again (`none()` in `src/support/none.cee`), `tests/support` as `.cee`, `tools/fault_sweep.py` with the `CEESAFE_` variables; `guide/performance.md` | macOS arm64, Apple clang 21, rustc 1.99.1, 5 runs: CeeSafe 0.96× Rust's time (geometric mean; 1.01× before), 0.88× Rust with overflow checks; `arena` 1.37× → 0.95×, `large-objects` 1.26× → 1.02×. Fixed point holds; reference corpus all families pass with leak checks (emit-c 24/24 after the reviewed `arena.c` edit); `tests/support` 8/8 with leak checks; fault sweep 573/573; guide_check 92 examples and spec_check 76 documents, 0 problems; `fmt --check` clean |
| 2026-10-31 | CosmoOS's move to CeeSafe, CeeSafe's side: `integrations/cosmoos/ports.py` runs CosmoOS's own `scripts/cee-gen.py` (its `generated`, `refused`) with the ceesafec under test, and with `--pinned` compares every generated file with that compiler's but for the banner's version; `c2cee.py verify` sets `CEESAFEC` and runs `make cee-check` (`--ceesafec`), and refuses a Nova-era tree; `c2cee port`'s `BUILD.md` describes the new layout and its analysis uses the kernel's include path (`out/cee/kernel/include`, `boot/protocol`, `drivers/include`); `spec/tooling.md` §9 and the integration README follow | `ports.py` on CosmoOS's local `ceesafe-migration` (`47964c10`) with ceesafec 0.3.0-dev and `--pinned` 0.2.0: 17 ports, 1 negative source, 28 generated files equal, 0 problems; on `origin/main` (still Nova) refused as intended; `c2cee selftest` PASS; `c2cee port kernel/core/bootinfo.c` drafts (it failed before on `cosmoboot.h`); `verify` refuses a Nova-era tree; spec_check and guide_check 0 problems |
| 2026-10-31 | L1 async/await, library ownership and A0 drafts: Decision 0008 (`lib/` recovered from `src/library/sources.cee`, equal to `map_forward` of `tests/reference/lib/` for all 13 files; `tools/gen_library.py` reads `lib/`; `spec_check` documents `lib/core`); ADR 0031 proposed; `docs/async-await-proposal.md`; roadmap track L1. `python3 tools/contract_tests.py --ceesafec out/ceesafec` (macOS arm64, Apple clang 21) | regenerated `sources.cee` differs only in its header and one doc comment; all 13 contract steps ok (fixed point, corpus, -Werror, ASan, support, sweeps, mutations, collections, freestanding, spec, guide, debugger); `fmt --check` clean. ADR 0031 awaits the owner |
| 2026-10-31 | L1 A0: ADR 0031 accepted ("I accept it as written."), then amended (§6: the scope's out-of-memory error is named by the caller, the owner's choice; `checkpoint(cancelled)`; `AsyncError`; `Async.race`/`all`; the `await` operand wording). Spec: `grammar.md`, `type-system.md` §32, §36.3, new §40, `ownership.md` A1–A4, `lifetimes.md` §8, `intermediate-representation.md` §3, `c-backend.md` §2.1, §3a, §6, `diagnostics.md` N0341–N0345 with N0290/N0295, `profiles.md`, `testing.md` §1; `gen_catalog.py`, `gen_docs.py`. `python3 tools/contract_tests.py --ceesafec out/ceesafec` | all 13 steps ok (spec_check: 137 codes explained); `fmt --check` clean. A0 gate passed |
| 2026-10-31 | L1 A1: `async fn`, prefix `await`, `Async.run`, `Async.park` (src/syntax, src/check/async.cee); the `suspend` effect and its contexts (src/types, src/check/effects.cee, src/nir/effect_check.cee); awaited calls marked in NIR and N0343 (src/nir/coroutines.cee); frames, flat resume functions and `Async.run` in C (src/backend/c_async.cee); runtime/hosted/ceesafe_rt_async.c (poll, timers); lib/std/async.cee (`Timer`, `AsyncConsole`, `AsyncError`); the interpreter on a virtual clock; `run_corpus.py --interp`; contract steps `run interp` and `library`; corpus files parse-pass/async-syntax, run-pass/async-basics, async-timers, check-fail/async-tasks, async-recursion, async-effects, emit-c/async-frame; ADR 0031 §6.5 (lowering); `check-pass/effects-of.hir` and `emit-c/slice-bulk.c` updated by reviewed edit (library ids and type order only). `python3 tools/contract_tests.py --ceesafec out/ceesafec` (macOS arm64, Apple clang 21) | all 15 steps ok: fixed point; corpus incl. the 7 async files; run-pass 161 and run-fail 40 in C (-Werror, ASan+UBSan, leaks) and in the interpreter; lib/ diagnostics clean; `fmt --check` clean. Found and fixed on the way: an `as` cast in lib/ that the suppressed library diagnostics hid (hence the `library` step). A1 gate passed |
| 2026-10-31 | L1 A2a: ADR 0032 (channels) accepted; ADR 0031 §6.6 (the scope body is a block, the owner's choice; the scope-loan rule checked on the children's captures). Async closures; `await Async.scope(oom, (s) => …)` and `s.spawn(async () => …)` (src/check/async.cee; N0241, N0344, N0214, N0342); NIR builtins AsyncSpawn and AsyncScopeWait; scope structs, spawn and child thunks in C (src/backend/c_async.cee); a multi-task executor (runtime/hosted/ceesafe_rt_async.c: run queue, parked list, per-task deadlines and descriptors, scopes); the interpreter's scheduler (runTasks: a stack per task, the virtual clock); corpus run-pass/async-scope, check-fail/async-scope; fault_injection.py counts `ceesafe_async_spawn`. `python3 tools/contract_tests.py --ceesafec out/ceesafec`; `python3 tools/fault_injection.py --compiler out/ceesafec --corpus tests/reference/tests/corpus` (macOS arm64, Apple clang 21) | all 15 steps ok (run-pass 162 in C, ASan+UBSan and the interpreter); the fault sweep then 52 allocating programs, 798 failure points, 0 fail, so spawn's out-of-memory path is clean. Found on the way: `await` of a scope was wrapped as an awaited call and dropped in NIR; the interpreter freed a child's environment with its statement (it now copies it) |
| 2026-10-31 | L1 A2b, first part: cancellation (a task's flag, its scope's when a child fails or cannot start, its scope owner's, transitively; a cancellation wakes waiting tasks), `Async.isCancelled`, `Async.checkpoint` (lib/std's `AsyncControl<E>`), `Async.shield` (enter/leave around the awaited body; TimedOut past its deadline; a block body's result defaults to `Result<void, AsyncError>`); lib/std's waits observe cancellation; `CEESAFE_CANCEL_AT`/`CEESAFE_CANCEL_REPORT` and `tools/cancel_sweep.py`, a new contract step; the interpreter mirrors all of it; corpus run-pass/async-cancel, async-shield, check-fail additions; lib/std's helpers moved into `AsyncLeaf` (library top-level names are reserved for every program); timers that come due together run earliest deadline first (a race under load: `async-scope` failed once in a parallel corpus run); effects-of.hir and slice-bulk.c updated again by reviewed edit (numbering and type order only). `python3 tools/contract_tests.py --ceesafec out/ceesafec` | all 16 steps ok: run-pass 164 in C, ASan+UBSan and the interpreter; fault sweep 54 programs, 803 points, 0 fail; cancel sweep 5 programs, 29 points, 0 fail. `race`, `all` and `scopeIn` remain |
| 2026-10-31 | L1 A2b, race and all as specified (the owner's choice, option 1): `await Async.race(a, b)` -> `Result<Raced<T>, E>` and `await Async.all(a, b)` -> `Result<void, E>`; both children's tasks and frames in the awaiting frame (`ceesafe_async_start`, `ceesafe_task_slot`: nothing allocated); race cancels the other child when the first returns, all on an Err (`ceesafe_async_cancel`); lib/std's `Raced<T>`, `AsyncRaceJoin<T, E>.settle`, `AsyncAllJoin<E>.settle` make the value in completion order; NIR AsyncJoin/AsyncJoinResult; the interpreter mirrors it; N0343 counts embedded children; corpus run-pass/async-race (allocation attempts=0), check-fail additions; effects-of.hir updated by reviewed edit (closure numbers only). `python3 tools/contract_tests.py --ceesafec out/ceesafec` | all 16 steps ok after the snapshot update (corpus re-run: check-pass 27/27); run-pass 165 in C, ASan+UBSan and the interpreter; cancel sweep 6 programs, 41 points, 0 fail. `scopeIn` remains |
| 2026-10-31 | L1 A2c, first part: `await Async.blocking(() => …)` (a sync closure on a worker thread; captures by Thread.both's rule and a Send result, N0334; the body's effects are the worker's; not cancelled; inline if no thread starts) and waking from another thread (a task's atomic `woken` flag and its executor's wake pipe, watched by `poll`); async programs link the thread runtime; the interpreter runs the body at once; corpus run-pass/async-blocking, check-fail additions. `python3 tools/contract_tests.py --ceesafec out/ceesafec`; the blocking program 5 times under `--cflag -fsanitize=thread` | all 16 steps ok: run-pass 166 in C, ASan+UBSan and the interpreter; fault sweep 55 programs, 809 points (a worker that cannot start runs inline), 0 fail; cancel sweep 7 programs, 47 points, 0 fail; ThreadSanitizer silent. Channels (ADR 0032) remain |

**Tests run for the skeleton** (2026-10-30, novac 0.3.1, Apple clang 21.0.0):
- `novac check` and `novac fmt --check`: clean.
- `novac build`, then `./out/ceesafe --version`: printed `ceesafec 0.0.0-dev`, exit 0.
- No arguments: exit 2. An unknown command: exit 2.
- `novac run --interp -- --version` printed the same version.

**Tests run for P0** (2026-10-30, macOS arm64, Apple clang 21.0.0, Python 3.9.6):
- `tools/run_corpus.py --compiler ~/.nova/bin/novac` (0.3.1), all 457 files: 454 passed, 0 failed, 3 partial
  (freestanding: object builds only).
- The same run with novac `07d561f` against `--reference` 0.3.1: 0 differences.
- Against a deliberately wrong compiler: every affected file failed, as expected.
- `tools/inventory.py` run twice gave byte-identical output, with no host paths.
- Nova's `dotnet test Nova.slnx` at `07d561f`: 3,589 passed, 0 failed, 0 skipped.

**P0 gate on the Ryzen box** (2026-10-30, Debian 12 x86-64, Python 3.11.2; novac 0.3.1 linux-x64, sha256
`8bd8d81b…d973`; Nova `07d561f`; CeeSafe `ef3e413`). Each run used `nice -n 15` and 6 jobs:

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
  - Nova's run-pass, run-fail and ffi corpus: 199/199 with no leak;
  - the fault sweep under clang ASan+LSan: 573/573.
- The earlier gate run found the missing-drop bug as a LeakSanitizer report.

**P5 flat C backend** (2026-10-30; `docs/p5-plan.md`): C identical to novac's for 284/284 programs in four forms and
for CeeSafe itself; run-pass, run-fail and ffi pass under Apple clang 21, clang 18 and GCC 12 in every configuration
(one GCC report that novac's C shares); the fault sweep (789 points), the collection properties and the freestanding
checks pass on macOS and Linux.

**P6 fixed point** (2026-10-30; `docs/p6-fixed-point.md`): gen 1 by novac 0.6.0, gen 2 and gen 3 by CeeSafe; C2 = C3
and gen 2 = gen 3 on macOS (Apple clang 21) and Linux (GCC 12), in two directories each, with the same C on both
hosts; only the C compiler runs from gen 1 on; `tools/contract_tests.py` passes on gen 2 on both hosts.

**P6R rename** (2026-10-30; Decision 0002): CeeSafe's sources renamed (`8941c10`), bootstrap anchor `3d6ac40`; the
fixed point on CeeSafe names with a bootstrapped ceesafec as stage 0 on macOS and Linux, the same C on both; legacy,
renamed and mixed package fixtures pass; contract tests pass on both hosts.

**P7 production C** (2026-10-30; `docs/p7-plan.md`): closure specialisation, placement, check elimination and the
structured form, on by default; C identical to novac 0.6.0's default form over 308 programs in four shapes and for
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

**CosmoOS integrations** (2026-10-31; `docs/cosmoos-integrations.md`, macOS arm64, QEMU, CosmoOS `9c0d0f84`): Nova's
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

Not yet run: the rest of P9.
