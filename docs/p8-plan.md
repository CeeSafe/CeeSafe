# P8 plan: CLI, packages and tools

Status: **in progress** (started 2026-10-31; pinned to the reference compiler 0.6.0).

P8 ports the rest of the reference compiler: every command `ceesafec` does not yet have, the package manager and the tools, then
makes the fork point (Decision 0003 §3) with the C prefix switch (Decision 0004). The reference is still the reference compiler 0.6.0
(`bootstrap/reference.md`); its C# is 8,863 lines in the P8 files of `docs/reference-inventory.md` §1, plus
`Program.cs`'s commands.

| Part | Reference | Commands and options |
|---|---|---|
| Embedded docs, explain, fixes | `Documentation/EmbeddedDocs.cs` (80), `Tooling/Fixer.cs` (51), `Program.cs` | `explain`, `fix [--dry-run]` |
| Doc generator | `Documentation/DocGenerator.cs` (337) | `doc [-o] [--title] [--missing]` |
| Formatter | `Formatting/Formatter.cs` (1,220) | `fmt [--check] [--stdout]` |
| Packages | `Packages/` (1,874: Toml, Manifest, Versions, Workspace, Registry, Publishing, AgentFiles) | `new`, `agents`, `fetch [--offline] [--update]`, `publish`, `index-check`; package mode for every command |
| Interpreter | `Interpreter/` (3,856) | `run --interp`, the sandbox |
| Editor protocols | `Tooling/SemanticIndex.cs` (297), `LanguageServer.cs` (476), `McpServer.cs` (824), `DiagnosticJson.cs` (102, done in P2) | `lsp`, `mcp` |
| Host parity | `editors/debug`, `editors/vscode`, `Backend.C/CCompilers.cs`'s MSVC path | — |

## How it is compared

What the reference compiler prints, ceesafec prints, byte for byte, but where the rename contract (ROADMAP §3.4) says otherwise:
`ceesafec` for `ceesafec`, `ceesafe.toml`/`ceesafe.lock`/`.cee`/`$CEESAFE_HOME` for the reference's names, `ceesafe_*` MCP tools,
CeeSafe agent-file markers. ceesafec's output has CeeSafe's names mapped back (`tools/names.py`, one short table) before it is
compared.

- **explain, doc, fmt:** over every corpus file, `lib/`, the programs and benchmarks, and CeeSafe's own sources.
- **fix:** the reference's `FixTests`: every fix the corpus offers, applied, gives the reference compiler's files; `--dry-run` output identical.
- **Packages:** the reference's `PackageManagerTests` and `AgentFilesTests` as fixtures with local git repositories
  and a local index, no network; the files written (`ceesafe.lock`, `AGENTS.md`, …) compared after the mapping.
- **Interpreter:** run-pass and run-fail under `run --interp`, stdout, stderr and exit status identical to the reference compiler's
  interpreter; the deterministic thread schedule identical. Never on benchmark-sized programs (they need tens of GB).
- **LSP and MCP:** recorded request transcripts (the reference's `LspTests`, `McpTests`), responses identical after the
  mapping.

## Slices

| Slice | Content | Gate |
|---|---|---|
| **S0** | The CLI shell: the reference compiler's usage text and option errors; `explain` over the embedded documents (`tools/gen_docs.py`: the reference's `docs/`, the guide and the agent template); `fix` and `--dry-run` | `explain` identical for every code; every corpus fix applied identical; `--dry-run` identical |
| **S1** | `fmt`: the formatter, `--check`, `--stdout`; CeeSafe's sources formatted by ceesafec | identical over the corpus, `lib/`, programs and benchmarks; idempotent; `ceesafec fmt` dropped from CeeSafe's workflow |
| **S2** | `doc`: the doc generator (signatures cut from the formatter's output, so after S1), `-o`, `--title`, `--missing` | `docs/api/core.md` regenerated identical; every corpus file's page identical |
| **S3** | Packages: TOML, manifests, version solving, git dependencies and `$CEESAFE_HOME`'s cache, the lock file, the registry index, `new`, `agents` (CeeSafe templates and markers), `fetch`, `publish`, `index-check` | the package fixtures; files written identical after the mapping |
| **S4** | The interpreter: values, memory, numerics, builtins, host I/O, deterministic threads, the sandbox; `run --interp` | run-pass and run-fail identical to the reference compiler's interpreter; threads' traces identical |
| **S5** | `lsp` (the semantic index without reflection) and `mcp` (tools, resources, prompts, the sandbox) | the transcripts identical after the mapping |
| **S6** | Host parity: the MSVC path on the Windows VM, the debugger scripts, the VS Code extension under CeeSafe names; the self-build through package mode, P6's fixed point rerun through it; Decision 0003 §2's question (only `.cee` on the command line) | ceesafec built by MSVC emits the same C; fixed point through package mode on both hosts |
| **S7** (done) | The fork point, when the owner names it: the C prefix and ABI version (Decision 0004), the link-collision test, the regression baseline of the reference compiler's output at the fork | the P8 gate (`ROADMAP.md` §P8) |

Slices S0–S6 keep ceesafec's C byte-identical to the reference compiler's, so every P5–P7 gate keeps running unchanged.

## Names (S3)

The rename contract (ROADMAP §3.4) applied to the package commands: `ceesafe.toml`, `ceesafe.lock`, `.cee`,
`$CEESAFE_HOME` (default `~/.ceesafe`), `$CEESAFE_INDEX`; the agent section between `<!--CEESAFE START-->` and
`<!--CEESAFE END-->`, `.cursor/rules/ceesafe.mdc`, and the MCP server `ceesafe` (`ceesafec mcp`) with tools
`ceesafe_*` in the agent template and configurations. Prose said "the reference" until the fork point (S7), and the default
index is CeeSafe's own, `github.com/vkuttyp/CeeSafe-index` (owner, 2026-10-31): a reference index's packages are the reference
packages, which ceesafec does not read (Decision 0003).

## Upstream gaps (lib/std at v0.6.0)

- No way to set a child process's environment: git runs as `env GIT_TERMINAL_PROMPT=0 git …` (POSIX).
- No rename: a finished git checkout is moved into place by `mv` (POSIX).
- `Process.capture` fails on Windows (`InvalidInput`): `captureIn` keeps the input file open for writing while
  `ceesafe_host_spawn` reopens it with `FILE_SHARE_READ`, which Windows refuses. On Windows ceesafec runs every tool through
  a batch file that redirects its output to files, run by `Process.run` (`runThroughBatch`, `src/host/toolchain.cee`).
- No query for the host system (only `Target.isX86_64()`/`isAArch64()`): ceesafec takes a working directory with a
  drive (`X:`) to mean Windows.

All are host-service items for the reference's N track (type-system.md §26). Until the first two are fixed, git dependencies need
a POSIX shell (`env`, `mv`), so on Windows only packages without git dependencies work.

## Known differences

- Threads in the interpreter: the reference compiler's run each CeeSafe thread on a host thread holding a baton, so a blocked thread can be
  resumed whenever its wait ends. ceesafec's are cooperative on one host thread: a thread runs when the running one
  blocks, the first runnable in spawn order, until it ends; one that blocks again deeper in the host stack cannot be
  resumed out of order, which no corpus program needs (a run that would need it reports a deadlock).
- The reference's in-process interpreter tests that mutate NIR into unsafe code (the dynamic safety oracle) have no
  command-line form, so they are not ported; the faults they check are the same code paths (`Memory`).

- `run --interp` honours CEESAFE_ALLOC_REPORT and CEESAFE_FAIL_ALLOC_AT for the interpreted program, as the reference compiler does; but
  ceesafec is a CeeSafe program, so its own runtime honours them too: with CEESAFE_ALLOC_REPORT it adds its own report line
  after the program's, and CEESAFE_FAIL_ALLOC_AT fails ceesafec's own allocation of that number. `tools/compare_interp.py`
  drops ceesafec's line; the fault sweep under the interpreter is the reference's in-process option, not ported.
  (Since 2026-11-13 ceesafec spends that attempt itself before it starts, so the failure reaches the interpreted
  program only; the second report line stays: `spec/tooling.md` §12.3.)

- A file that cannot be read: the reference compiler's message is .NET's exception text, which names the absolute path
  ("Could not find file '/…'"); ceesafec's is the C library's. Neither the comparison tools nor the reference's tests
  depend on it. `ceesafe_run`'s and the other MCP tools' `paths` are the exception: there the message is part of the
  tool's result, so ceesafec writes .NET's three texts (a missing file, a missing directory, a directory).

- Windows output: the reference compiler's `Console.WriteLine` ends lines with CR LF (and its other writes with LF), and with output
  redirected .NET writes in the console's code page (437: `§` is 0x15, `…` becomes `.`). ceesafec writes UTF-8 with LF on
  every host. `tools/compare_packages.py --windows` folds both before comparing; the C files are byte-identical.

- MCP: `ceesafe_spec_search` and `ceesafe_spec_section` ignore case for ASCII letters only; .NET's OrdinalIgnoreCase
  folds other letters too (`é` finds `É`). A query with a non-ASCII letter may therefore find less.

## Execution record

| Date | Slice | Work | Result |
|---|---|---|---|
| 2026-10-31 | S0 | The reference compiler's usage text under CeeSafe's names, and usage errors that print it (`Program.Fail`); `--diagnostics-format` taken out before any other option, as `Program.Run` does; `explain` over the embedded documents (`tools/gen_docs.py` → `src/library/documents.cee`: the reference's `docs/`, the guide and the agent template at `v0.6.0`, 99 documents; `src/tooling/explain.cee`); `fix` and `fix --dry-run` (`src/tooling/fixes.cee`: Fixer's overlap rule, eight rounds, only the inputs edited); `tools/names.py` (the mapping back), `tools/compare_cli.py` | usage and usage errors 17/17 identical; `explain` 1,009/1,009 (every code N0000–N0999, lower case, malformed); `fix`, `fix --dry-run` and `fix --diagnostics-format=json --dry-run` over the 165 check-fail and parse-fail programs and the 16 FixTests programs: 180/180 identical, the files they leave too |
| 2026-10-31 | S1 | the formatter (`src/formatting/formatter.cee`: containers, continuations, glue, the line-breaking loop, trailing-comment alignment, widths in UTF-16 units; `fmt_command.cee`: the §6 self-check, `--check`, `--stdout`, package mode); CeeSafe's sources formatted by `out/ceesafec fmt`, and the generators format with it; `tools/compare_fmt.py` | `fmt --stdout` (text and JSON diagnostics) identical over the 545 `.cee` files of the reference `v0.6.0` and CeeSafe's 85 sources; idempotent on all; the 211 corpus files without inline comments squashed onto one line identical (the line breaker); `fmt --check` and in-place `fmt` over 45 copied files identical; ceesafec formats its own sources in 0.3 s (the reference compiler 0.7 s) |
| 2026-10-31 | S2 | the doc generator (`src/documentation/doc_generator.cee`: entries from the syntax, signatures cut from the formatted text, doc comments, `impl` lists, GitHub anchors, `--missing`, `-o`, `--title`, package mode); `tools/compare_doc.py` | `doc`, `doc --missing` and `doc -o … --title` identical for each of the 545 `.cee` files, lib/core, lib/std and lib/kernel as pages, and CeeSafe's sources as one page (549/549); lib/core's page equals the reference's `docs/api/core.md` |
| 2026-10-31 | S0–S2 | the contract suite with the `cli`, `fmt` and `doc` steps (`tools/contract_tests.py`) | every step ok on macOS (Apple clang 21): the P2–P7 steps unchanged (C 284/284 ×4 and 308/308 ×4, reports 308/308 ×4, CeeSafe's own C identical, 431,331 lines; 501,249 with `--split -g`), packages 7/7, cli, fmt, doc, run corpus under `-Werror` and ASan+UBSan, mutations, fault sweep 783 points, collections 30/30, freestanding |
| 2026-10-31 | S3 | packages (`src/packages/`): the TOML subset, versions and requirements, manifests, the workspace (path, git and registry dependencies, the lock, SHA-256 package hashes in `src/support/sha256.cee`, the git cache in `$CEESAFE_HOME/git`), the index and the backtracking version solver, `publish` and `index-check`, agent files; the commands `fetch`, `publish`, `index-check`, `new`, `agents`; package mode for `parse` too, and `check`/`build`/`fmt`/`doc`/`fix` through the new resolver; `tools/compare_package_commands.py` | 8 scenarios after PackageManagerTests and AgentFilesTests (31 bad manifests; path dependencies, lock out of date, conflicts, shared and cyclic graphs, missing and executable dependencies; git by tag, branch and rev, unknown tag, offline with an empty cache, a changed checkout; a local index with three versions, dependencies, `--update`, conflicts, a missing and an invalid entry, `index-check`; `publish` refusals and a published entry; agent files written, refreshed and left; `new`), each built with the reference's and CeeSafe's names: 382/382 runs identical after the mapping, the lock and agent files too; ceesafec's git package hashes equal the script's SHA-256 |
| 2026-10-31 | S3 | the contract suite with the `package cmds` step | all 30 steps ok on macOS (Apple clang 21); CeeSafe's own C identical (461,390 lines; 538,065 with `--split -g`) |
| 2026-10-31 | S4a | the interpreter's core (`src/interpreter/`: memory with initialised bits, frames, places, operands, rvalues, statements, statics, the runtime's heap, regions, pools, scratch frames, vectors, stable vectors, slot maps, drop glue, the builtins but I/O, C calls and threads; float printing as ceesafe_format_f64); direct calls run on an explicit stack of activations, so deep recursion needs no deep native stack; `run --interp` with `--`, the optimisation switches, CEESAFE_ALLOC_REPORT and CEESAFE_FAIL_ALLOC_AT; `tools/compare_interp.py` | run-pass and run-fail 188/199 identical to the reference compiler's interpreter (plain, with CEESAFE_ALLOC_REPORT, and with every optimisation off); the other 11 need C calls, I/O or threads (S4b–S4d) |
| 2026-10-31 | S4b–S4d | the C library model (strlen, puts, abs, memcmp, memcpy, memset, malloc, free, the atomics, lib/std's host services: environment, path kinds, directories, temporary and working directories, spawning with captured streams), files and sockets through lib/core's File, TcpListener and TcpStream behind a descriptor table, Process.run, program arguments, Console.read; threads (spawn, spawnBoxed, both, join, mutexes) cooperatively in spawn order (`src/interpreter/io.cee`) | run-pass and run-fail 199/199 identical to the reference compiler's interpreter, plain, with CEESAFE_ALLOC_REPORT and with every optimisation off |
| 2026-10-31 | S4 | the contract suite with the `interp` step | all 31 steps ok on macOS (Apple clang 21); CeeSafe's own C identical (501,898 lines; 584,993 with `--split -g`) |
| 2026-10-31 | S5 | the servers: `lsp` (`src/tooling/language_server.cee`: Content-Length framing, documents checked alone or with their package from disk and the lock, publishDiagnostics, hover, definition, document symbols, quick fixes, formatting, UTF-16 positions, JavaScriptEncoder.Default's escaping) over the semantic index (`src/tooling/semantic_index.cee`, an explicit walk in the reference's reflection order); `mcp` (`src/tooling/mcp_server.cee`: the three protocol versions, nine `ceesafe_*` tools, `ceesafe://docs/` resources, both prompts, .NET's messages for arguments of the wrong kind and unreadable paths); the JSON reader (`src/support/json_reader.cee`), in-memory compilation (`src/tooling/compile.cee`), the interpreter's captured output and given input for `ceesafe_run`; `tools/compare_servers.py`. Fixed on the way: `fmt` of a relative directory listed its files from `/` (`sourceFilesUnder` now joins as Path.Combine does) | LSP: 441/441 sessions identical (every corpus file with symbols, code actions, formatting, hover and definition at each word, a change and a close, plus 11 edge and package sessions); hover and definition at every character of every file 441/441. MCP: 436/436 sessions identical (every corpus file through check, fix, format, emit_c with the three reports, symbol at each word and, for the run families, run with arguments, input and small limits; the protocol, 99 resources, prompts, explain for every code in diagnostics.md, spec_search and spec_section, 42 argument errors) |
| 2026-10-31 | S5 | the contract suite with the `servers` step | all 32 steps ok on macOS (Apple clang 21); CeeSafe's own C identical (530,944 lines; 618,791 with `--split -g`) |
| 2026-10-31 | S6a | the fixed point through package mode at `6ad8ba6` (stage 0 from `tools/bootstrap.sh` on each host: the reference compiler 0.6.0 builds the anchor, which builds HEAD); `editors/debug` under CeeSafe names (`ceesafe_lldb.py`, `ceesafe_gdb.py`, `ceesafe_names.py`: `ceesafe-locals`, `ceesafe-bt`, `CEESAFE_PROGRAM`) and `tools/debugger_check.py` (DebuggerTests' programs and assertions, plus `--split`); `editors/vscode` under CeeSafe names (language `ceesafe`, `.cee`, `source.ceesafe`, `ceesafe.server.path`) with a keyword test against `src/syntax/token.cee`; Decision 0005 proposed (any file on the command line until the fork point) | fixed point in the default form and with `--split` on macOS (Apple clang 21) and Linux (GCC 12.2, Ryzen box): gen 1 = gen 2 = gen 3, C2 = C3 = C4 in two directories, only `cc` (or gcc's `as`, `ld`) started, and the same C on both hosts (7 and 119 files; `bootstrap/fixed-point/2026-10-31-p8s6-*`); debugger_check under lldb 0 problems (Linux box has no gdb: not run there); the extension's `vp test` 20/20 (grammar 16, `ceesafec lsp` through vscode-jsonrpc 3, keywords 1) and `vp run package` makes `ceesafe-language-0.1.0.vsix` |
| 2026-10-31 | S6b | Windows: paths with a drive (`hasDrive`, `rootLength`, `isRooted`, `slashedPath`, `currentDirectory`; `joinPath` keeps `X:/`; `%USERPROFILE%` as the home); the C compiler driver (`src/host/compilers.cee`, CCompilers: `cl` by name, on Windows clang, `%ProgramFiles%\LLVM`, gcc, cl, Visual Studio by vswhere, its vcvarsall.bat, MSVC's flags and 8 MiB stack, `-Wl,/STACK` for clang, cl's progress lines dropped, the freestanding refusal); on Windows tools run through a batch file (lib/std's capture fails there) and compilers take a response file (GNU quoting for clang and gcc, the C runtime's for cl); `tools/host_c_check.py`, `compare_packages.py --windows` | Windows 11 ARM64 VM, MSVC 19.51, clang 23.1.4: gen 1 compiled by `cl /W3` from the Mac's C with no warning; gen 1 built CeeSafe in package mode with the compiler it found (clang, Visual Studio set up by itself) and with `--cc cl`; gen 2 built gen 3. Every generation's C for CeeSafe identical to macOS's (7 files; 120 with `--split`). Over the corpus, benchmarks and programs (498 files × default, `--split`, `-g`) gen 3's C, diagnostics and exit statuses equal macOS ceesafec's: 1,494 builds, 6,817 files, 0 differ. Package fixtures against the reference compiler 0.6.0 for Windows (`ceesafec-win-arm64`, sha256 checked): 7/7 with `--windows`. A program built by `--cc cl` and by clang runs |
| 2026-10-31 | S6 | the contract suite with the `debugger` step, after S6b's changes | all 33 steps ok on macOS (Apple clang 21); CeeSafe's own C identical (535,274 lines; 624,029 with `--split -g`) |
| 2026-10-31 | S7 | the fork point (`docs/fork-point.md`), on the owner's choices: C names, runtime and exports header under CeeSafe's names (Decision 0004, `cee_`), `CEESAFE_ABI_VERSION 1` defined in `ceesafe/rt.h` and checked after its include in every generated unit; only `.cee` and `.c` on the command line (Decision 0005, accepted); CeeSafe in the prose; the generators through `tools/names.py`'s forward map, the compiler's literals rewritten by it, prefix-length code fixed by hand (`ceesafe_host_`, `ceesafe_atomic_`, `ceeF_`, the reserved-name check, the split form's defined-name test); CeeSafe's corpus `tests/reference/` (`tools/fork_corpus.py`); the comparison and run tools speak each compiler's names (`names.for_compiler`, `Dialect`, `map_back` with the ABI lines and the `#line` directives they move); `tools/link_collision.py` | all 34 contract steps ok on macOS (Apple clang 21), the reference compiler 0.6.0 the frozen baseline: CeeSafe's own C identical after mapping back (535,540 lines; 624,339 with `--split -g`), no the reference C name left; CeeSafe's corpus: every run_corpus family passes; servers 441 LSP and 433 MCP sessions; a the reference compiler-built and a ceesafec-built library link and run in one program, two the reference compiler-built ones collide. `map_forward` is the exact inverse of `map_back` on the reference's 618 runtime, library, corpus, catalog and template files |
| 2026-10-31 | S7 | the fixed point and Windows after the fork, at `f9f6d6f` | fixed point in the default form and with `--split` on macOS (clang) and Linux (GCC 12.2): gen 1 = gen 2 = gen 3, C2 = C3 = C4 in two directories, only `cc` (gcc's `as`, `ld`) started, the same C on both hosts (7 and 120 files; `bootstrap/fixed-point/2026-10-31-p8s7-*`); Windows: gen 1 by `cl`, gen 2 by clang and by `--cc cl`, gen 3, each emitting C identical to the fixed point's; CeeSafe's corpus in three forms identical between macOS and Windows (1,494 builds, 6,817 files) |
