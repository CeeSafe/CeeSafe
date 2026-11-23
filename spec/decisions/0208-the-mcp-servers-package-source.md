# ADR 0208 — The MCP server's package source

- Status: **Accepted** by the owner, 2026-11-23 ("I accept ADR 0208"), with §6's three questions taken as
  recommended; proposed the same day and implemented under the owner's instruction on the optimization and DX roadmap
  ("Go ahead and start implementing"; its Phase 1, item 1); not released.
- Scope: tooling.md §6.2 and §6.3; `src/tooling/mcp_server.cee`, `src/tooling/compile.cee`,
  `src/tooling/language_server.cee`; `tools/mcp_api_check.py`; the agents template and guide/for-agents.md.

## 1. Context

The agents template tells an agent to call `ceesafe_check` after every edit. The tool takes its program as `files`
(texts) or `paths` (files read from disk), and compiles them as one package of their own (tooling.md §6.2): it reads no
manifest and no lock. In a package that has a dependency, the answer is wrong. Tried with ceesafec 0.93.0: a package
`app` with `geometry = { path = "../geometry" }`, where `ceesafec check` reports nothing, and
`ceesafe_check {"paths": ["src/main.cee"]}` reports `N0200 cannot find 'geometry' in this scope`. An agent then
"repairs" code that was right, or stops trusting the tool and goes back to the shell.

The language server already checks a document with its package: the other sources from disk, the locked dependencies,
never fetching (tooling.md §2.1; `checkInPackage`). The MCP server has no such source.

## 2. Decision

### 2.1 A third source: `package`

Every tool that compiles (`ceesafe_check`, `ceesafe_fix`, `ceesafe_run`, `ceesafe_emit_c`, `ceesafe_symbol`) takes,
instead of `paths`:

- `package`: a directory, relative to the server's working directory (`"."` is that directory). The program is the
  package of the nearest `ceesafe.toml` at or above it: its sources (`[package] sources`, §4.2) and every package of its
  lock, read from disk, **never fetching**, as `check --offline` and the language server resolve it (§4.4, §2.1).

`package` with `paths` is `InvalidParams`: one says which files, the other says all of them.

### 2.2 Unsaved text: `files` over a package

With `package`, a `files` entry whose path names a source of the root package replaces that file's text for the call:
an agent checks an edit before it writes it. A `files` entry that names no source of the root package is a tool error
that says so (a new file is on disk before the manifest's `sources` can find it). The path is taken from the server's
working directory, as a `paths` entry is.

### 2.3 Settings

The profile, `no-float` and the kernel come from the root manifest, as for `check`. `profile` or `noFloat` given with
`package` is `InvalidParams`: the manifest decides, and a silent override would make the tool disagree with the
command.

### 2.4 Paths in results

A file of the program is named by its path relative to the server's working directory, with `/` (`src/main.cee`,
`../geometry/src/lib.cee`), which is what a `paths` call would have been given and what `ceesafe_symbol`'s `path`
takes.

### 2.5 What each tool does with it

- `ceesafe_check`: the diagnostics of the root package and of its dependencies, as `check` reports them.
- `ceesafe_fix`: fixes are applied to the root package's files only (a dependency is not the caller's to edit), and
  `files` in the result lists **only the files a fix changed**; with `files` or `paths` it lists every file, as
  before. Nothing is written.
- `ceesafe_run`: an executable package runs in the sandbox, unchanged otherwise. The sandbox refuses files, so a
  package that reads its own data fails there as any program does.
- `ceesafe_emit_c`, `ceesafe_symbol`: unchanged otherwise.

### 2.6 Failures

No manifest at or above the directory, a manifest or lock that does not resolve (a dependency not fetched, N0324), or
a `files` entry outside the root package: a result with `isError: true` and the reason, the manifest's or lock's
first diagnostic included. The server never fetches and never writes.

### 2.7 What the server reads

§6's promise stands, widened by one clause: the server reads its input, the paths a tool names, and for `package` the
manifests, the lock and the sources of the packages the lock lists. It still writes nothing.

## 3. Options not taken

- **`paths` finds the package by itself** when its files are sources of one. No new parameter, and the call agents
  make today would start to work. But `paths` would then mean two things, chosen by what is on disk: a file checked
  alone today (a corpus program inside a package's tree, a scratch file) would change meaning when a manifest lists it.
  The explicit source is the one the template can name. §6 question 1.
- **Checking through the interface records** (ADR 0082 §2.7), as `check` does. Faster for large locks, but the
  server would write the cache; it compiles in memory, as the language server does. A later change can read valid
  records without writing.
- **Fetching** when the lock is missing. A check that touches the network is not what "check" says, and the server
  promises to read only.

## 4. Consequences

- The template's and the guide's advice becomes `ceesafe_check {"package": "."}` in a package.
- A large lock is compiled whole on every call (§3, second option); not measured yet (§5).
- `checkInPackage` (the language server) and the MCP server share one function that lists a package's files, so the
  two cannot drift.

## 5. Implementation and tests (2026-11-23)

- `packageProgram` (src/tooling/compile.cee) lists a package's files and its lock's, with the root manifest's
  settings; the language server's `checkInPackage` and the MCP server's `packageSourcesOf` both use it.
- `McpSources` (src/tooling/mcp_server.cee) carries the files, their packages and the settings to every tool that
  compiles; `ceesafe_fix` applies fixes to the root package's files only and, for a package, returns the changed ones.
- `tools/mcp_package_check.py`, new and in the contract suite, on tests/packages' `imports` and `missing-lock`: the
  tool's diagnostics against `ceesafec check --offline`'s for the same package, the names from the directory above,
  an overlay (and the file on disk unchanged), a fix, a symbol, and the five refusals. 12 checks, 0 problems, on macOS
  arm64 and on Ubuntu 24.04 x86-64. With the compiler before this change the tool has no `package` and the check
  fails.
- The language server's package checks (`tools/package_tests.py`) are unchanged: 42 checks, 0 problems, on macOS.
- Not measured: the time of a call on a large lock (§4).

## 6. Questions for the owner (answered 2026-11-23: accepted as recommended)

1. Should `paths` also find its package by itself (§3, first option)? No; revisit if agents keep calling `paths` in
   packages.
2. `ceesafe_fix` with `package` lists only changed files (§2.5). Yes; the alternative is every file of the root
   package on every call.
3. `profile`/`noFloat` with `package` are refused (§2.3). Yes.
