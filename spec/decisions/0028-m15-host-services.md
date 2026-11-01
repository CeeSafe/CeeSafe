# ADR 0028 — M15 S7: the host services a compiler needs

> **Inherited.** ADR 0028 was accepted before CeeSafe's fork (Decision 0006), when the language had its
> earlier name. In this record, "the reference" is that language and project and "the reference compiler" its compiler:
> read them as CeeSafe and `ceesafec`. Links to documents CeeSafe did not keep are given as text.

- Status: **Accepted** (project owner, 2026-10-30), every question as recommended (§5). The wording is the agent's. Implemented the same day (§6).
- Scope: M15 S7 (OD-35). These are the operating-system services `ceesafec build` and its package commands use, which
  The reference's hosted library (type-system §26) lacks: a compiler written in the reference (OD-33) cannot do without them.

## 1. Context

The services the reference compiler itself uses (an audit of `src/`, 2026-10-30):

| Service | Where the reference compiler uses it | The reference today (§26) |
|---|---|---|
| Run a program and capture its stdout and stderr, optionally with stdin | the C compiler and linker (`Backend.C/CBackend.cs:252-290`); tool discovery (`CCompilers.cs:165`) | `Process.run`: inherited stdio, exit status only |
| Read an environment variable | `CC`, `PATH`, `CEESAFE_HOME`, `CEESAFE_INDEX`, `INCLUDE` | none |
| Create and list directories | `-o DIR` for `--emit c` (`Program.cs:586`, `:603`, `CBackend.cs:42`); `src/` of a package (`Manifest.cs:271`) | none |
| Whether a path exists, and what it is | manifests, the cache, `fmt` on a directory | none (`File.open` and look at the error) |
| A fresh temporary directory, removed afterwards | every `build` (`Program.cs:614`) | none |
| The working directory | package lookup (`Manifest.FindDirectory`), source paths (`SourceText.SourcePath`) | none |

The CeeSafe port (P1, `host-process` probe) confirmed the rest works: writing a C file, running `cc` with an argument
vector, its exit status, and reading the object back.

## 2. Options

| | A. Built-in library API in `lib/std` and §26 (proposed) | B. C adapters in each program | C. A generic `extern "C"` POSIX binding |
|---|---|---|---|
| Safety | safe API; the runtime owns the unsafe parts | every program writes unsafe FFI | unsafe everywhere, and POSIX only |
| Portability | the runtime selects the platform (macOS, Linux, Windows, CosmoOS), as `Process.run` does (§26.6) | per program | none on Windows or CosmoOS |
| Interpreter | modelled, and refused in the sandbox | runs no C (§28.6) | runs no C |

## 3. Proposed decision: A

Hosted-only functions (N0293 in freestanding code), blocking, with errors as `IoError`:

- `Process.capture(program, arguments, input) -> Result<ProcessOutput, IoError>`.
  - `program: StringView` is found on `PATH`; `arguments: borrow Vector<StringView>`; `input: Slice<u8>?` is written to
    the child's stdin, which is then closed (`null`: an empty stdin).
  - `ProcessOutput` is a built-in struct with `status: i32`, as `Process.run` reports it, plus `stdout: Vector<u8>`
    and `stderr: Vector<u8>`.
  - The runtime reads both pipes and writes stdin concurrently (`poll` on POSIX, overlapped pipes on Windows), so a
    child that fills one pipe cannot deadlock it.
  - Failures: allocation is `OutOfMemory`; the program not found is `NotFound`; arguments as for `Process.run`.
- `Environment.variable(name: StringView) -> StringView?`: the value as static text, like `Process.argument` (§26.1).
  It is `null` if absent or not valid UTF-8. The reference has no way to set a variable, so the text never changes under the
  program.
- `Directory.create(path) -> Result<void, IoError>` (one level; `AlreadyExists` if it exists) and
  `Directory.remove(path)` (an empty directory).
- `Directory.list(path) -> Result<Vector<String>, IoError>`: the entries' names, without `.` and `..`, **sorted by
  byte value**, so programs stay deterministic (c-backend §1's rule applied to I/O).
- `Path.kind(path) -> Result<PathKind?, IoError>`: `File`, `Directory` or `Other`, or `null` if nothing is there. It
  follows symbolic links.
- `Directory.temporary() -> Result<String, IoError>`: a new, empty, private directory, created atomically (`mkdtemp`)
  under the platform's temporary directory. The caller removes it.
- `Directory.current() -> Result<String, IoError>`.

Paths are `StringView`s, under the same rules as `File.open`: zero bytes and paths over 4,095 bytes are `InvalidInput`.
The interpreter models these with .NET's APIs on macOS and Linux, and the sandbox (`ceesafec mcp`, `ceesafe_run`) refuses
them all, as it refuses processes today.

## 4. Consequences

- Spec: type-system §26.7–§26.10, diagnostics (none new expected), and the runtime in `runtime/hosted/ceesafe_rt_io.c`
  (POSIX and Windows) with CosmoOS's variants selected at compile time.
- Tests:
  - run-pass and run-fail programs for each function (a child writing more than a pipe's buffer to both streams;
    absent variables; sorted listing; temporary-directory cleanup);
  - `InterpreterTests` and `McpTests` for the sandbox;
  - the MSVC configuration for Windows;
  - `c3.py` in a CosmoOS guest for the spawn path.
- A compiler written in the reference (the CeeSafe port) can then implement `build` and package mode without C adapters.

## 5. Questions for the owner

1. Option A, with this API? (Recommended.)
2. `Environment.variable` as a `StringView?` of static text, rather than a copied `String`? (Recommended: it matches
   `Process.argument`, and nothing can change the environment.)
3. `Directory.list`: should a name that is not valid UTF-8 make the call fail with `InvalidInput`, or be skipped?
   (Recommended: fail. Skipping could make a compiler silently miss a source file.)
4. CosmoOS: implement `Process.capture` there in 0.4.0 (its spawn API needs pipe redirection; to be audited in
   CosmoOS's code), or report `Other` until a later release? (Recommended: audit first, and decide with the evidence.)
5. Windows: full parity in the same release? (Recommended: yes. The reference compiler's own Windows builds depend on these services.)

## 6. Implementation (2026-10-30)

- **Q4 (CosmoOS), from the audit.** CosmoOS's libc (`libc/include/spawn.h`, `unistd.h`, `dirent.h`, `sys/stat.h`,
  `stdlib.h`) has `spawnvp` with `spawn_handle` mappings, plus `pipe`, `getenv`, `getcwd`, `opendir`/`readdir`, `stat`,
  `mkdir` and `rmdir`, but no `poll` and no `mkdtemp`. So `Process.capture` is implemented there in this release.
  On every platform it routes the child's streams through files in a private temporary directory, not pipes, so
  nothing needs `poll` or threads to avoid a deadlock. The temporary directory is made by trying names until `mkdir`
  succeeds, which is atomic on every platform.
- **One name changed.** `Path.kind` is `File.kind`. A lib/std `Path` would break programs that declare their own:
  lib/std names cannot be redeclared in a hosted package (§38.5), and the corpus's `run-pass/struct-nested.cee` has
  one. `Environment`, `Directory`, `PathKind`, `ProcessOutput` and `HostServices` matched nothing in the repository,
  CeeSafe or the site's guide.
- **Where it lives:**
  - `lib/std/host.cee`, with its unsafe code behind documented contracts;
  - ten `ceesafe_host_*` functions in `runtime/hosted/ceesafe_rt_io.c`, for POSIX, CosmoOS and Windows, declared in `rt.h`;
  - two checker hooks, so `Process.capture` and `File.kind` call lib/std's `HostServices`;
  - the interpreter's models in `Interpreter/HostServices.cs`.
- **Tests:**
  - `run-pass/host-services.cee`, run in every C configuration and in the interpreter;
  - `check-fail/host-services.cee`;
  - `HostServicesTests`, where capture runs natively and in the interpreter with a child writing 300,000 bytes to
    each stream, and the sandbox refuses.
- **CosmoOS, in a guest.** `c3.py --ssh` builds `integrations/cosmoos/c3/c3-host.cee` for CosmoOS and runs it.
  The first run found that CosmoOS's `S_ISDIR`/`S_ISREG` test the stat record's `st_type` (a `DT_*` value), not
  `st_mode` (its `sys/stat.h`). The runtime selects `st_type` there.
- **Not yet run:** the Windows `CreateProcess` path. The CI's Windows job builds and runs the run corpus with MSVC,
  which covers everything but capture; `HostServicesTests` covers capture, but not on Windows.

