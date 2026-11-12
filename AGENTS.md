<!--CEESAFE START-->

# Using CeeSafe

This package is written in CeeSafe, a memory-safe systems programming language with modern syntax that
`ceesafec` compiles ahead of time to C. Ownership and borrowing are checked at compile time; there is no garbage
collector. CeeSafe is new: code that looks right from C#, TypeScript or Rust often is not CeeSafe. Check every edit with the
compiler, and read the guide for agents: https://ceesafe.github.io/CeeSafe/guide/for-agents (the whole guide and
specification for a model's context: https://ceesafe.github.io/CeeSafe/llms-full.txt).

## The package

- `ceesafe.toml` names the package, its kind (`executable` or `library`) and its dependencies; `ceesafe.lock` pins them.
- The sources are the `.cee` files under `src/`. A program starts at `fn main()`.
- `ceesafec build` writes to `out/`, which is not committed.

## Commands

Run them in the package's directory; without file arguments they use the package.

| Command | What it does |
|---|---|
| `ceesafec check` | parse, type-check and borrow-check; the fastest way to find errors |
| `ceesafec fix` | apply the fixes the compiler is sure of, then check again |
| `ceesafec explain N0249` | what a diagnostic code means |
| `ceesafec run --interp` | run the program in the interpreter |
| `ceesafec build` | compile through C to `out/<name>` |
| `ceesafec fmt` | format in the one canonical style; `ceesafec fmt --check` lists files that would change |
| `ceesafec fetch` | resolve the dependencies and write `ceesafe.lock` |

Add `--diagnostics-format=json` to `check`, `build`, `run` or `fix` for one JSON document with every diagnostic, its
position and its fixes.

## The MCP server

If you have the `ceesafe` MCP server (`ceesafec mcp`), use its tools: `ceesafe_check` after every edit, `ceesafe_fix` for the
fixes it offers, `ceesafe_explain` for a code, `ceesafe_spec_search` and `ceesafe_spec_section` for the rules, `ceesafe_run` to
run code in a sandbox. The specification is embedded in `ceesafec`, so its answers match the compiler.

## Before you finish

- [ ] `ceesafec check` reports no errors (run `ceesafec fix` first).
- [ ] `ceesafec fmt --check` lists no file.
- [ ] The program does what was asked: `ceesafec run --interp`, or `ceesafec build` and run it.

## Common mistakes

- There is no `~`: complement with `x ^ 0xFFFFFFFF`. There are no type aliases or tuples, and no `==` on structs.
- An optional's fallback is `o ?? value`; `o.isSome()` and `o.isNull()` test it. `let _ = f();` discards a value.
- Indexes are `usize`. `0..4` is a `usize` range when the body indexes with its variable, else `i32`; `0..values.length` is `usize`.
- `if` and `match` expressions take expressions, `let s = if (x < 0) -1 else 1;`, or blocks that end with their value
  without `;`; a statement's block has no value.
- A variable that changes is `var`; a `let` cannot be assigned. A `borrow mut` argument is written `mut x`.
- `as` converts only without loss; `truncate<u8>(x)` wraps; `checkedTruncate<i64>(n)` (an optional) and
  `saturatingTruncate<u8>(n)` check or clamp, for integers and floats.
- `for (part in text.split(","))` loops over any value with `fn next(borrow mut this) -> T?`.
- A multi-line string is a raw `"""` literal: the closing `"""` line's indentation is removed, and there are no escapes.
- A function that always exits returns `never`: `fn die(m: StringView) -> never { …; Process.exit(1); }`.
- An `async move` child (`s.spawn`, `set.spawn`, `Async.timeout`/`race`/`all`) owns its captures and may move them.
- A view of a temporary, `make().view()`, lasts until the end of its statement: to keep it, bind the value to a local.
- There is no string interpolation: `"x = {x}"` prints `{x}` (warning N0400); write the value in a call of its own.
- A function or closure in a field is called as `c.step(n)`; a struct that borrows a closure has a region parameter.
- A `StringView` inside an enum or struct needs a region parameter; for fixed messages, use an enum of reasons.
- A method cannot have the name of a field.
- Look a name up before deciding it does not exist: MCP `ceesafe_api` (`TextWriter.decimal`, `Process.argument`), or
  the guide's built-in types page and the lib/core and lib/std API pages. A "did you mean" in an error names the real one.
- A lookup that may find nothing is an optional (`Process.argument(i)`, `StringView.fromUtf8(b)`): `Some`/`null`.
- Some keywords read like names (`own`, `type`, `move`, `drop`, `where`); async code uses `AsyncConsole`, not `Console`.

<!--CEESAFE END-->

# CeeSafe project rules

CeeSafe is a memory-safe systems language, and `ceesafec`, its compiler, is written in CeeSafe. The section above,
between the `CEESAFE` markers, is written by `ceesafec agents` and is refreshed by it: put project rules here, below
the markers, and never edit between them. While working on this repository, "ceesafec" means `out/ceesafec`, the
compiler built from the tree (`ceesafec` is not on `PATH` here).

## Governing documents (in order of authority)

1. `ROADMAP.md`: scope, phases, gates and the execution record (§10).
2. `docs/decisions/*.md`: CeeSafe's project decisions.
3. `spec/`: the specification (type system, ownership, C backend, tooling) and its ADRs (`spec/decisions/`). Spec
   before code, safety first, determinism, target layout from `TargetInfo` only.
4. `bootstrap/reference.md`: the release stage 0 and the recorded fixed points.

## Building ceesafec

- Sources are `.cee` files under `src/`, the manifest is `ceesafe.toml`: `out/ceesafec check`,
  `out/ceesafec build -o out/ceesafec`.
- From a fresh checkout, get a first ceesafec with `tools/bootstrap_release.sh STAGE0`, where STAGE0 is a released
  ceesafec (`bootstrap/reference.md`, "Release stage 0"; Decision 0009). Keep a known good `out/ceesafec` before
  changing the compiler, so a broken build can be rebuilt by the previous one.
- Format with ceesafec: `out/ceesafec fmt` formats the package's `.cee` sources, `out/ceesafec fmt --check` lists the
  ones that would change. The generators (`tools/gen_*.py`) format their output with it too ($CEESAFEC).
- A missing language or library feature is designed here: spec and ADR first (`spec/decisions/0031-…` onward), then
  the compiler, tests and guide. The libraries are `lib/` (Decision 0008; `tools/gen_library.py` embeds them), the
  runtime `runtime/` (`tools/gen_runtime.py`). Never work around a gap with `unsafe`, C, or a weaker check where a
  safe design exists.
- Small C adapters for host services are allowed only behind a documented contract (ownership, lengths, encoding,
  errors); compiler algorithms stay in CeeSafe.
- CeeSafe has no predecessor to consult or compare with: `tools/spec_check.py` (in the contract suite) fails on any
  mention of the retired language it began from, outside the licence notice (Decision 0010).

## Tools

```sh
python3 tools/contract_tests.py --ceesafec out/ceesafec                # every contract test, one command (spec/testing.md)
python3 tools/run_corpus.py --compiler out/ceesafec --corpus tests/reference/tests/corpus      # CeeSafe's corpus
W=""; for f in src/support/*.cee; do W="$W --with $f"; done                # zsh: then ${=W}
python3 tools/run_corpus.py --compiler out/ceesafec --corpus tests/support --family run-pass $W --check-leaks
python3 tools/fault_sweep.py --compiler out/ceesafec tests/support/fault/alloc-sweep.cee src/support/*.cee
python3 tools/oracles/support_models.py                                  # oracle summaries for tests/support
python3 tools/fixed_point.py --stage0 out/ceesafec --work <dir> --manifest <file>
                                                                        # the fixed point (docs/p6-fixed-point.md)
sh tools/bootstrap_release.sh STAGE0 [REVISION] [OUTPUT]                 # a ceesafec from a released one (Decision 0009)
python3 benchmarks/run.py                                                # CeeSafe against Rust and C (guide/performance.md)
python3 tools/gen_runtime.py                                             # regenerate src/library/runtime.cee from runtime/
python3 tools/gen_library.py                                             # regenerate src/library/sources.cee from lib/
python3 tools/gen_docs.py                                                # regenerate src/library/documents.cee
```

Generated files say so in their first line; edit the generator, never the output. Expected values in `tests/support`
come from oracles outside the code under test (Python, .NET). Use `--check-leaks` for anything new.

`run_corpus.py` checks the corpus's own expectations (annotations, snapshots, expected output); `--reference`
also requires identical observable outcomes from a second ceesafec. Its normalisations are listed in the script: never
widen one to make a comparison pass.

## Evidence discipline

- Cite sources: `file:line` for code, sections for docs.
- Keep tests that were run (date, command, result) apart from tests that are only proposed. Record work in ROADMAP §10.
- Never regenerate expected results from CeeSafe to make a comparison pass; an intended change to an expectation is a
  reviewed edit, recorded where the change is.

## Layout

```text
ROADMAP.md  AGENTS.md  ceesafe.toml
src/                                    the compiler, in CeeSafe
docs/decisions/                         CeeSafe decisions
bootstrap/                              the release stage 0, the seed's README, the recorded fixed points
tools/                                  run_corpus.py (corpus runner), contract_tests.py, the generators and checks
probes/                                 P1 capability probes in corpus format
src/support/ src/host/                  the support library (P1) and host services
src/syntax/ src/diagnostics/            the frontend (P2): tokens, lexer, AST, parser, dumps; diagnostics and their JSON
tests/reference/                        CeeSafe's corpus (tests/corpus), benchmarks and programs
tests/support/                          support-library tests (run-pass, fault/)
tools/oracles/                          independent oracles and generators for tests
runtime/                                the C runtime ceesafec embeds (tools/gen_runtime.py; Decision 0007)
lib/                                    the libraries ceesafec embeds: core, std, kernel (tools/gen_library.py; Decision 0008)
spec/                                   the specification and its ADRs
guide/                                  the guide (site/ publishes it)
benchmarks/                             CeeSafe against Rust and C (run.py; results in guide/performance.md)
out/                                    build output, never committed
```

## Git

Commit only when asked, in small focused commits; never commit `out/`.
