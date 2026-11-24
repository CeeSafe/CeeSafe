# ADR 0220 — The MCP server's `ceesafe_test` and `ceesafe_doc`

- Status: **Proposed**, 2026-11-24, under the optimization and DX roadmap's Phase 1, item 2 (the owner: "Continue to the
  remaining in the roadmap"). No code yet: §6's questions are the owner's.
- Scope: tooling.md §6.3 (two tools), §10.2 (the outcomes, shared); `src/tooling/mcp_server.cee`,
  `src/host/test_runner.cee` (its harness, made in memory as well as on disk); `tools/mcp_api_check.py`;
  guide/for-agents.md and the agents template.

## 1. Context

The MCP server has ten tools (tooling.md §6.3). An agent can check, fix, format, run and look things up, but to run a
package's tests it leaves the server for the shell: `ceesafec test`, which builds the harness with a C compiler, writes
`out/ceesafe-test/` and `out/<name>-tests`, and runs native code. For documentation it runs `ceesafec doc`.

Two promises constrain any new tool (tooling.md §6, `mcp_server.cee:1-4`):
- the server reads only its input, the paths a tool names and, for a `package` source (ADR 0208), that package's
  manifests, lock and sources; it writes nothing;
- it runs code only in the sandboxed interpreter: no files, sockets or processes, a step budget, 256 MiB.

`ceesafec test --interp` already runs tests in the interpreter (§10.2): each test through `ceesafec run --interp` on the
harness, a child process, with the virtual clock and seeded order of type-system.md §40.8. A test that calls C is
`skipped`. The sweeps (`--sweep alloc|cancel`) are native only.

`ceesafe_run` already interprets in the server's own process (`interpretCaptured`, with `sandboxed`, `stepLimit`,
`memoryLimit` and captured output), so no child process is needed for that part.

## 2. Decision

### 2.1 `ceesafe_test`

- **Input:** a source of §6.2, normally `{"package": "."}` (tests live in packages: beside the code, or in `tests`
  directories); `files` or `paths` work too, their `@test` functions being the tests. Also `filter` (as `--filter`;
  one that no name contains is a tool error, as it is an error for the command), `maxSteps` per test (default
  10,000,000) and `maxOutputBytes` per test and stream (default 65,536).
- **How it runs:** in the sandboxed interpreter, in the server's process. The server checks the package with its
  tests, makes the harness **in memory** (the source `ceesafec test` writes to `out/ceesafe-test/`, never written
  here), and interprets it once per test, in the order of their names, each with a fresh interpreter: its own
  memory, its own step budget, the virtual clock and seeded order of §40.8. Nothing is built, written or started.
- **Outcomes:** §10.2's, as the interpreter can give them: `passed`, `failed`, `panicked`, `leaked`, `skipped` (it
  calls C), and two of its own:
  - `fault`: the interpreter stopped it (exit status 70): a sandbox refusal (a file, socket or process, named in the
    message), use of uninitialised memory, or the step budget spent. A test that reads a fixture file is a `fault`,
    not a failure: the message says the sandbox refused the file, and that `ceesafec test` runs it.
  - `exited`: it called `Process.exit` with another status.
  `crashed` and `timed out` cannot happen: there is no native code, and the budget is in steps.
- **Result:** the document of `ceesafec test --format json` (§10.2), with the same field names, so one parser reads
  both: `tests` (each `name`, `outcome`, `steps` in place of `milliseconds`, `message`, `stdout`, `stderr`,
  `truncated`), `passed`, `failed`, `skipped`. If the package does not compile, `outcome: "not-compiled"` and the
  diagnostics, as `ceesafe_run` gives them. No sweep fields: the sweeps are native (§4).

### 2.2 `ceesafe_doc`

- **A tool, not a resource.** Resources are the embedded documents, fixed for a given `ceesafec` (§6.4); a package's
  documentation depends on its sources, which a resource URI cannot carry.
- **Input:** a source of §6.2 and `missing` (default false).
- **Result:** `markdown`, the page `ceesafec doc` writes for the root package's public items (§1.2); with `missing`,
  `missing`: the public items without a doc comment, each `path`, `line`, `kind`, `name`, as `doc --missing`'s
  `path:line: kind name` lines list them (§1.3). A
  dependency's items are not documented: they are its own package's.

### 2.3 What changes elsewhere

- `test_runner.cee`: the harness's source is made by a function that returns it, which `ceesafec test` writes to
  `out/ceesafe-test/` and the server compiles from memory; the outcome names and the JSON writer are shared.
- The server's `instructions` and guide/for-agents.md: "after an edit that changes behaviour, `ceesafe_test` with
  `{"package": "."}`; `ceesafec test` in a shell for tests that touch files or C, and for the sweeps".
- `tools/mcp_api_check.py`: a package with a passing test, a failing one, a panicking one, one that reads a file
  (`fault`, the refusal named) and one that calls C (`skipped`); `ceesafe_doc` against `ceesafec doc`'s output for the
  same package, byte for byte, and `missing` against `doc --missing`.

## 3. Alternatives considered

- **Run the tests natively** (`ceesafec test` behind the tool): the real C build, the sweeps, tests that touch files.
  It breaks both promises: the server would write `out/`, start a C compiler and run native code an agent wrote, with
  the server's rights. A client that wants that already has the shell, where its own permission rules apply.
- **A child `ceesafec run --interp` per test**, as `test --interp` does: the same sandbox, but a process per test and
  a harness written to disk. In-process is what `ceesafe_run` does already.
- **`ceesafe_doc` as a resource template** (`ceesafe://package-docs/{path}`): templates are not in every client the
  server supports (§6.1's three protocol versions), and a resource has no `missing` switch.

## 4. Not in this ADR

- The sweeps through MCP. They need the native runtime's allocation and cancellation hooks (ADR 0075 §2.16).
- Time limits: the interpreter's budget is steps, which are deterministic; a test's milliseconds would not be.

## 5. Testing

As §2.3: `tools/mcp_api_check.py`'s new cases, in the contract suite; the corpus and the fixed point unchanged (no
language change).

## 6. Questions for the owner

1. **Where tests run:** in the sandboxed interpreter only, in the server's process, never natively? **Proposed: yes.**
2. **The result:** `ceesafec test --format json`'s document, with `steps` for `milliseconds`, `truncated`, and the
   interpreter's `fault`, rather than a format of its own? **Proposed: yes.**
3. **`ceesafe_doc`:** a tool with `missing`, rather than a resource? **Proposed: a tool.**
