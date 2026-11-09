# The ceesafec command

`ceesafec` is the compiler and every tool around it. `ceesafec --help` prints the same summary as this page.

Inside a [package](packages.md), commands take no file names: they use the nearest `ceesafe.toml` in the current
directory or above. Outside one, give the files.

## Commands

| Command | Does |
|---|---|
| `ceesafec new <name> [--lib] [--agent <names>] [--no-agent]` | make a package: `ceesafe.toml`, `src/main.cee` (or `src/lib.cee`), and `AGENTS.md` for [AI agents](for-agents.md) |
| `ceesafec agents [--agent <names>]` | write or refresh the package's agent instructions |
| `ceesafec fetch [--offline]` | resolve the package's dependencies and write `ceesafe.lock` |
| `ceesafec check` | parse, type-check and borrow-check; the fastest way to find errors |
| `ceesafec build` | compile to C, then with the C compiler to `out/<name>` (or `-o`) |
| `ceesafec run --interp` | run the program in the reference interpreter |
| `ceesafec test` | run the package's `@test` functions, each as its own process ([Testing](testing.md)) |
| `ceesafec parse` | parse only, reporting syntax errors |
| `ceesafec fmt [--check \| --stdout]` | format in the canonical style ([Formatting](formatting-and-docs.md)) |
| `ceesafec doc [-o file] [--title t] [--missing]` | a Markdown page of the public items ([Documentation](formatting-and-docs.md#documentation)) |
| `ceesafec lsp` | the language server, for [editors](editors.md) |
| `ceesafec fix [--dry-run]` | apply the fixes the compiler is sure of, and check again until none applies |
| `ceesafec explain <code>` | what a diagnostic code (`N0249`) means |
| `ceesafec mcp` | the MCP server, for [AI agents](for-agents.md) |

## Building

| Option | Does |
|---|---|
| `-o <path>` | the output file (or directory, with `--emit c`) |
| `--emit exe` | an executable (the default) |
| `--emit c` | the generated C sources, to look at or to build yourself |
| `--emit obj` | one relocatable object, for a kernel or another program's link (`--ld ld.lld` joins objects for ELF cross builds) |
| `--cc <compiler>` | the C compiler; default `$CC`, else `cc`, or on Windows clang, gcc or Visual Studio's `cl`, found and set up ([tooling](../spec/tooling.md) §8) |
| `--cflag <flag>` | an extra C compiler flag (repeatable); the default is `-O2` |
| `-g` | a debug build: `#line` directives and `-g -O0` (see [Debugging](debugging.md)) |
| `--line-directives` | `#line` directives without changing the optimisation level |
| `--split` | the C in one file per CeeSafe source, `c/<package>/<path>.c`, so it compiles in parallel ([C backend](../spec/c-backend.md) §2.4) |
| `--jobs <n>` | with `--split`: compile at most `n` C files at once; the default is the number of CPUs, and the output is the same for any `n` |
| `--profile hosted \| freestanding` | the [profile](kernel.md#the-freestanding-profile) |
| `--no-float` | reject every float type and literal |
| `--offline` | in a package: never run `git` |

C files given with the CeeSafe files (`ceesafec build main.cee shim.c`) are compiled and linked with the program.

## Seeing what the compiler decided

| Option | Prints |
|---|---|
| `--explain-allocations` | each allocation site, and whether its box lives on the stack or the heap, and why |
| `--explain-bounds-checks` | each index check, and whether it was removed |
| `--explain-overflow-checks` | each checked `+`, `-`, `*` and negation, and whether its check was removed |
| `--no-placement`, `--no-bce`, `--no-oce` | turn off box placement, bounds-check elimination, overflow-check elimination |
| `--dump-ast`, `--dump-hir`, `--dump-nir` | the compiler's intermediate forms (`parse`, `check`, `build`) |

The optimisations never remove a check that could fail; their preconditions are in
[Optimisations](../spec/optimisations.md).

## Exit status and errors

`ceesafec` exits with 0 on success, 1 when the program has errors, and 2 for a usage error. Diagnostics go to standard
error as `file:line:column: error[code]: message`, followed by the source line, notes, and a hint where there is one.
The codes are listed in [Diagnostics](../spec/diagnostics.md).

With `--diagnostics-format=json` (on `parse`, `check`, `build`, `run`, `fmt` and `fix`), they go to standard output
instead, as one JSON document with each diagnostic's position, message and fixes, for programs and
[AI agents](for-agents.md) ([tooling](../spec/tooling.md) §5).

## The interpreter

`ceesafec run --interp` runs a program without a C compiler, in an interpreter that executes the compiler's own
intermediate form. It stops with an exit status of 70 on any use of freed, out-of-bounds or uninitialised memory, or
a bad free, which the C build would not detect. The test suite runs every program both ways and compares the results.
