---
title: "Appendix E: The ceesafec command"
summary: Every command and option of ceesafec 0.42.0 with its argument, what it does and its default, then the exit statuses, the environment variables it reads, the keys of ceesafe.toml and the files it reads and writes.
keywords: [ceesafec, command, option, flag, parse, check, build, run, test, fmt, fix, new, agents, fetch, publish, index-check, doc, explain, lsp, mcp, "--emit", "--diagnostics-format", "exit status", "ceesafe.toml", "ceesafe.lock", CEESAFE_HOME, CEESAFE_INDEX]
specification: ["tooling.md §1", "tooling.md §4", "tooling.md §5", "tooling.md §7", "tooling.md §8", "tooling.md §10"]
---

# Appendix E: The `ceesafec` command

This appendix lists every command of `ceesafec` 0.42.0 and every option each one takes, one table for each
command. After the commands come the exit statuses, the environment variables, the keys of `ceesafe.toml`, and the
files `ceesafec` reads and writes. Every row can be read without the rows around it. The chapters that teach a
command are linked from its heading, and `ceesafec --help` prints a summary of the same options.

## The command line

```text
ceesafec <command> [options] <file.cee>... [<file.c>...] [-- <program arguments>]
```

- **With files,** the `.cee` files are checked as one package. A file whose name ends in neither `.cee` nor `.c`
  is a usage error.
- **Without files,** `parse`, `check`, `build`, `run`, `fix`, `fmt` and `doc` use the package of the nearest
  `ceesafe.toml`, in the current directory or a directory above it. `test`, `fetch`, `publish` and `agents` always
  work on that package and take no files.
- An option that takes an argument is followed by it as the next word: `--emit c`. Only `--diagnostics-format` and
  `--agent` may also be joined to it with `=`: `--diagnostics-format=json`.
- An option the command does not know is a usage error: `ceesafec: unknown option '--bogus'`, then the usage text,
  with exit status 2.

| Command | What it does | Taught in |
|---|---|---|
| `ceesafec new <name>` | Makes a new package in the directory `<name>`. | [1.3](./ch01-03-hello-package), [7.1](./ch07-01-package-and-manifest) |
| `ceesafec check` | Parses, type-checks and borrow-checks the files or the package, and builds nothing. | [1.3](./ch01-03-hello-package) |
| `ceesafec run --interp` | Checks the program, then runs it in the compiler's interpreter, without a C compiler. | [1.3](./ch01-03-hello-package) |
| `ceesafec build` | Compiles the program to C, and then with a C compiler to an executable, an object or a library. | [1.3](./ch01-03-hello-package), [14.1](./ch14-01-build-settings), [14.3](./ch14-03-libraries-for-other-programs) |
| `ceesafec test` | Runs the package's `@test` functions, each as a process of its own. | [11.2](./ch11-02-running-tests) |
| `ceesafec fmt` | Rewrites the files in the one canonical style. | [1.3](./ch01-03-hello-package) |
| `ceesafec fix` | Applies the fixes the compiler is sure of, and checks again until none applies. | this appendix |
| `ceesafec explain <code>` | Prints what a diagnostic code such as `N0249` means. | [1.2](./ch01-02-hello-world) |
| `ceesafec fetch` | Resolves the package's dependencies and writes `ceesafe.lock`. | [7.4](./ch07-04-dependencies) |
| `ceesafec publish` | Checks that the package can be published and prints its entry for the package index. | [14.2](./ch14-02-publishing) |
| `ceesafec index-check` | Checks the entries of a package index against their commits. It is for an index's own automated checks. | [14.2](./ch14-02-publishing) |
| `ceesafec doc` | Writes a Markdown page of the public items and their documentation comments. | [3.4](./ch03-04-comments), [14.2](./ch14-02-publishing) |
| `ceesafec parse` | Lexes and parses the files and reports syntax errors only. | this appendix |
| `ceesafec agents` | Writes or refreshes the package's instruction files for AI coding agents. | this appendix |
| `ceesafec lsp` | Serves the Language Server Protocol on standard input and output, for an editor. | this appendix |
| `ceesafec mcp` | Serves the Model Context Protocol on standard input and output, for an AI agent. | this appendix |

## Options without a command

| Option | Argument | What it does | Default |
|---|---|---|---|
| `--version` | none | As the first word after `ceesafec`, prints the compiler's name and version, `ceesafec 0.42.0`, and exits with status 0. | not given |
| `--help` | none | As the first word after `ceesafec`, or after any command and before a `--`, prints the usage text and exits with status 0. `ceesafec test --help` prints the options of `test` instead. | not given |
| `-h` | none | The same as `--help`. | not given |

`ceesafec` with no command prints the usage text and exits with status 2. There is no `help` or `version` command:
`ceesafec help` is `unknown command 'help'`.

## `ceesafec parse`

`ceesafec parse [options] [file.cee...]` reads the files, or the package's, and reports syntax errors.

| Option | Argument | What it does | Default |
|---|---|---|---|
| `--dump-ast` | none | Prints the syntax tree of each file on standard output. | off |
| `--diagnostics-format` | `text` or `json` | Chooses how diagnostics are printed: as text on standard error, or as one JSON document on standard output. | `text` |
| `--offline` | none | In a package, uses only the dependencies that are already fetched, and never runs `git`. | off |

## `ceesafec check`

`ceesafec check [options] [file.cee...]` checks the files as one package. In a package it checks the package, its
tests, and every package it depends on. Taught in [1.3](./ch01-03-hello-package); `--deny-warnings` in
[14.1](./ch14-01-build-settings).

| Option | Argument | What it does | Default |
|---|---|---|---|
| `--package` | `<name>` | Checks the package of that name from `ceesafe.lock` instead, as if run in its directory. It needs a package and no files. A name the lock does not have is exit status 2. | the package of the current directory |
| `--all` | none | Checks every package of `ceesafe.lock`, each with its tests, and reports the diagnostics by package. It needs a package and no files. | off |
| `--whole-program` | none | Checks every package in full as one program, and reads nothing that an earlier check saved. Plain `check` rejects whatever this rejects and is never slower. | off |
| `--record` | none | Makes the interface record that `check` keeps of the package between runs, and prints it. It needs a package and no files. | off |
| `--deny-warnings` | none | Reports every warning as an error, so a warning makes the command exit with status 1. | off |
| `--profile` | `hosted` or `freestanding` | Chooses the profile. `freestanding` is for code with no C library and no operating system ([22.1](./ch22-01-the-freestanding-profile)). | `hosted`, or the manifest's `profile` |
| `--kernel` | `cosmoos` | With `--profile freestanding`, includes the kernel library ([22.2](./ch22-02-kernel-code)). `cosmoos` is the only value. | off, or the manifest's `kernel` |
| `--no-float` | none | Rejects every float type and float literal (`N0297`), for code built without floating-point registers. | floats are allowed, unless the manifest says `float = false` |
| `--offline` | none | In a package, uses only the dependencies that are already fetched, and never runs `git` ([7.4](./ch07-04-dependencies)). | off |
| `--dump-hir` | none | Prints the checked program in the compiler's typed form on standard output. It shows the code that `@derive` wrote. | off |
| `--diagnostics-format` | `text` or `json` | Chooses how diagnostics are printed: as text on standard error, or as one JSON document on standard output, with each diagnostic's position and fixes. | `text` |

`check` takes no `--jobs`: it is one process.

## `ceesafec build`

`ceesafec build [options] [file.cee...] [file.c...]` checks the program, writes C, and runs the C compiler. `.c`
files named with the `.cee` files are compiled and linked with the program. Taught in
[1.3](./ch01-03-hello-package), [14.1](./ch14-01-build-settings) and
[14.3](./ch14-03-libraries-for-other-programs).

| Option | Argument | What it does | Default |
|---|---|---|---|
| `-o` | `<path>` | Names the output file, or with `--emit c` the output directory. A directory that does not exist is made. | in a package `out/<name>`; with files, the first file's name without `.cee`, in the current directory |
| `--emit` | `exe`, `c`, `obj` or `lib` | Chooses what is written: `exe` an executable, `c` the generated C sources in a directory, `obj` one relocatable object, `lib` a static library with the headers a host program needs ([14.3](./ch14-03-libraries-for-other-programs)). | `exe` |
| `--cc` | `<compiler>` | Names the C compiler. | `$CC`; else `cc`; on Windows the first found of clang, gcc and Visual Studio's `cl` |
| `--cflag` | `<flag>` | Passes one more flag to the C compiler. It can be given several times ([14.1](./ch14-01-build-settings)). | none; the C is compiled with `-O2` |
| `--ld` | `<linker>` | With `--emit obj`, joins the objects with `<linker> -r` instead of the C compiler, as `ld.lld` does for another processor ([22.1](./ch22-01-the-freestanding-profile)). With `--emit lib` it is a usage error. | the C compiler joins them |
| `--ar` | `<archiver>` | With `--emit lib`, names the archiver that makes the library. Without `--emit lib` it is a usage error. | `llvm-ar` beside an LLVM clang, else `ar` |
| `--lib-main` | none | With `--emit lib`, keeps the C `main` of a package that has a CeeSafe `main`. Without `--emit lib` it is a usage error. | off: a library has no C `main`, since its host has one |
| `--exports-header` | `<name>` | Names the header that declares the program's `@export` items for C. | `ceesafe_exports.h` |
| `-g` | none | Makes a debug build: `#line` directives, the C compiled with `-g -O0`, and debug assertions on ([14.1](./ch14-01-build-settings)). | off |
| `--line-directives` | none | Maps the generated C to CeeSafe lines with `#line`, without changing the optimisation level. | off; on with `-g` |
| `--debug-assertions` | none | Evaluates `debugAssert` calls in this build. | off; on with `-g` |
| `--no-debug-assertions` | none | Leaves `debugAssert` calls out of this build, even with `-g`. | off |
| `--split` | none | Writes the C as one file for each CeeSafe file, so that the C compiler runs on several at once ([14.1](./ch14-01-build-settings)). | off: one C file |
| `--jobs` | `<n>`, a number above 0 | With `--split`, compiles at most `n` C files at once. Without `--split` it is a usage error. | the number of processors |
| `--program` | `<name>` | With `--split`, sets the name that the program's C names carry. | the package's name, or the first file's |
| `--flat-c` | none | Writes the C in its flat form, one label for each block, without rebuilding `if` and loops. | off |
| `--profile` | `hosted` or `freestanding` | Chooses the profile. `freestanding` builds without a C library or an operating system ([22.1](./ch22-01-the-freestanding-profile)). | `hosted`, or the manifest's `profile` |
| `--kernel` | `cosmoos` | With `--profile freestanding`, includes the kernel library ([22.2](./ch22-02-kernel-code)). `cosmoos` is the only value. | off, or the manifest's `kernel` |
| `--no-float` | none | Rejects every float type and float literal (`N0297`). | floats are allowed, unless the manifest says `float = false` |
| `--deny-warnings` | none | Reports every warning as an error, so a warning stops the build with status 1 ([14.1](./ch14-01-build-settings)). | off |
| `--offline` | none | In a package, uses only the dependencies that are already fetched, and never runs `git`. | off |
| `--diagnostics-format` | `text` or `json` | Chooses how diagnostics are printed: as text on standard error, or as one JSON document on standard output. | `text` |
| `--no-placement` | none | Keeps every `Box` on the heap. Placement, which is on by default, lets a box that never leaves its function live on the stack. | off |
| `--no-bce` | none | Keeps every index check. By default a check the compiler proves cannot fail is removed. | off |
| `--no-oce` | none | Keeps every overflow check. By default a check the compiler proves cannot fail is removed. | off |
| `--no-closure-specialisation` | none | Keeps every call through a closure indirect. By default a function called with a known closure gets a variant that calls it directly ([13.4](./ch13-04-what-they-cost)). | off |
| `--explain-allocations` | none | Prints, for every allocation site, where its memory comes from and why ([15.1](./ch15-01-box)). | off |
| `--explain-bounds-checks` | none | Prints each index check and whether it was removed ([13.4](./ch13-04-what-they-cost)). | off |
| `--explain-overflow-checks` | none | Prints each checked `+`, `-`, `*` and negation and whether its check was removed ([20.4](./ch20-04-arithmetic)). | off |
| `--explain-closure-calls` | none | Prints each call through a closure, whether it became a direct call, and each variant made ([13.4](./ch13-04-what-they-cost)). | off |
| `--explain-frames` | none | Prints each `async fn`'s frame size and what makes it large. | off |
| `--dump-nir` | none | Prints the program in the compiler's intermediate form on standard output. | off |

## `ceesafec run`

`ceesafec run --interp [options] [file.cee...] [-- <program arguments>]` checks the program and runs it in the
interpreter. The words after `--` are the program's arguments. The command's exit status is the program's. Taught
in [1.3](./ch01-03-hello-package).

| Option | Argument | What it does | Default |
|---|---|---|---|
| `--interp` | none | Runs the program in the interpreter. It is required: without it the command stops with `'run' needs --interp` and status 2. | required |
| `--` | the program's arguments | Ends the options of `ceesafec`. Every word after it is passed to the program, where `Process.argument(1)` is the first. | no arguments |
| `--no-debug-assertions` | none | Skips `debugAssert` calls in this run. | off: the interpreter evaluates them |
| `--debug-assertions` | none | Evaluates `debugAssert` calls, which the interpreter does anyway. | on |
| `--no-placement` | none | Keeps every `Box` on the heap. | off |
| `--no-bce` | none | Keeps every index check. | off |
| `--no-oce` | none | Keeps every overflow check. | off |
| `--no-closure-specialisation` | none | Keeps every call through a closure indirect. | off |
| `--offline` | none | In a package, uses only the dependencies that are already fetched, and never runs `git`. | off |
| `--diagnostics-format` | `text` or `json` | Chooses how the check's diagnostics are printed. With `json` the document comes on standard output after the program's own output. | `text` |

## `ceesafec test`

`ceesafec test [options]` runs in a package and takes no files. It checks the package with its tests, builds one
test program, and runs each test whose name matches, in the order of their names. Taught in
[11.2](./ch11-02-running-tests); the sweeps in [11.3](./ch11-03-organising-tests).

| Option | Argument | What it does | Default |
|---|---|---|---|
| `--filter` | `<text>` | Runs only the tests whose name contains the text. If no name contains it, that is an error with status 1. | every test |
| `--list` | none | Prints the tests' names, one on a line, and runs none. | off |
| `--interp` | none | Runs each test in the interpreter. A test that calls C is skipped. It cannot be combined with `--sweep`. | off: the tests run as a built program |
| `--jobs` | `<n>`, a number above 0 | Runs `n` tests at once. | 1 |
| `--timeout` | `<seconds>`, a number above 0 | Ends a test that runs longer and reports it as timed out. | 60 |
| `--sweep` | `alloc` or `cancel` | Runs each passing test again, once for each allocation with that allocation failing (`alloc`), or once for each point where a task can be cancelled (`cancel`) ([11.3](./ch11-03-organising-tests)). | off |
| `--sweep-strict` | none | With `--sweep alloc`, counts a panic from an unwrap in a test's own file as a sweep failure too ([15.3](./ch15-03-allocators)). | off |
| `--format` | `text` or `json` | Chooses the form of the report. `json` is one document with each test's name, outcome, time and output. | `text` |
| `--deny-warnings` | none | Reports every warning of the check as an error. With a warning, no test runs and the status is 1. | off |

`test` builds its test program as `build` does, so it also accepts the options of `build`, such as `--cc`,
`--cflag` and `-g`. The table [Which command accepts which option](#which-command-accepts-which-option) lists
them.

## `ceesafec fmt`

`ceesafec fmt [options] [file or directory...]` rewrites each file in the canonical style. A directory means every
`.cee` file under it. Without arguments, in a package, it formats the package's own sources. Taught in
[1.3](./ch01-03-hello-package).

| Option | Argument | What it does | Default |
|---|---|---|---|
| `--check` | none | Changes nothing. Prints the name of each file that would change, and exits with status 1 if there is one. It cannot be combined with `--stdout`. | off: the files are rewritten |
| `--stdout` | none | Changes nothing. Prints the formatted text of the one file given on standard output. | off |
| `--diagnostics-format` | `text` or `json` | Chooses how syntax errors are printed. A file with a syntax error is not formatted. | `text` |

## `ceesafec fix`

`ceesafec fix [options] [file.cee...]` checks, applies every fix the compiler offers, writes the files, and checks
again, for at most 8 rounds. Then it prints the diagnostics that remain and exits with status 1 if any is an
error. It edits only the files given, or the package's own sources.

| Option | Argument | What it does | Default |
|---|---|---|---|
| `--dry-run` | none | Changes no file. Prints the edits of the first round instead. | off |
| `--diagnostics-format` | `text` or `json` | Chooses how the remaining diagnostics are printed. | `text` |
| `--offline` | none | In a package, uses only the dependencies that are already fetched, and never runs `git`. | off |

## `ceesafec explain`

`ceesafec explain <code>` takes exactly one diagnostic code and no options. It prints the code's meaning, an
example, and the paragraphs of the specification's list of diagnostics that mention it. Capitals do not matter: `n0249` is `N0249`. An
unknown code is exit status 2. Taught in [1.2](./ch01-02-hello-world).

## `ceesafec new`

`ceesafec new <name> [options]` makes the directory `<name>` with `ceesafe.toml`, `src/main.cee`, `.gitignore`
and `AGENTS.md`. The name must be an identifier that is not a keyword and not `core`, and the directory must not
exist with files in it. Taught in [1.3](./ch01-03-hello-package) and [7.1](./ch07-01-package-and-manifest).

| Option | Argument | What it does | Default |
|---|---|---|---|
| `--lib` | none | Makes a library package: `kind = "library"` and `src/lib.cee` instead of `src/main.cee`. | off: an executable package |
| `--agent` | `<names>`: one or more of `agents`, `claude`, `gemini`, `copilot`, `cursor`, separated by commas | Writes the instruction files of those AI coding agents. It can be given several times, and as `--agent=<names>`. An unknown name is a usage error. | `agents`, which writes `AGENTS.md` |
| `--no-agent` | none | Writes no instruction file. It cannot be combined with `--agent`. | off |

## `ceesafec agents`

`ceesafec agents [options]` runs in a package. It writes the instruction files that are missing, replaces the
part of an existing one that `ceesafec` wrote, and leaves the rest. It prints what it wrote and what it left.

| Option | Argument | What it does | Default |
|---|---|---|---|
| `--agent` | `<names>`: one or more of `agents`, `claude`, `gemini`, `copilot`, `cursor`, separated by commas | Chooses the agents whose files are written or refreshed. It can be given several times, and as `--agent=<names>`. | `agents`, which writes `AGENTS.md` |

Each name writes these files. `AGENTS.md` is written whenever any agent is chosen.

| Name | The agent | Files |
|---|---|---|
| `agents` | Codex, Amp, OpenCode and others that read `AGENTS.md` | `AGENTS.md` |
| `claude` | Claude Code | `AGENTS.md`, `CLAUDE.md`, `.mcp.json` |
| `gemini` | Gemini CLI | `AGENTS.md`, `GEMINI.md`, `.gemini/settings.json` |
| `copilot` | GitHub Copilot | `AGENTS.md`, `.github/copilot-instructions.md`, `.vscode/mcp.json` |
| `cursor` | Cursor | `AGENTS.md`, `.cursor/rules/ceesafe.mdc`, `.cursor/mcp.json` |

## `ceesafec fetch`

`ceesafec fetch [options]` runs in a package. It resolves every dependency, fetches git repositories, and writes
`ceesafe.lock`. Taught in [7.4](./ch07-04-dependencies).

| Option | Argument | What it does | Default |
|---|---|---|---|
| `--offline` | none | Never runs `git`: it resolves from what is already fetched. | off |
| `--update` | none, or one or more package names | Chooses the newest version that satisfies the requirements for every registry package, or only for the packages named, instead of keeping the version the lock records. | off: a locked version that still satisfies the requirements is kept |

## `ceesafec publish`

`ceesafec publish` runs in a package and takes no options. It checks that the package is a library with a version,
that every dependency comes from the registry, that it type-checks, and that its git working tree is clean with
its commit on a remote branch. Then it prints the package's entry for the index. It writes nothing and publishes
nothing by itself. A package that fails a check is `N0335` and exit status 1. Taught in
[14.2](./ch14-02-publishing).

## `ceesafec index-check`

`ceesafec index-check [--offline] [directory]` checks the entries of the package index in the directory. It is run
by an index's automated checks ([14.2](./ch14-02-publishing)).

| Option | Argument | What it does | Default |
|---|---|---|---|
| `--offline` | none | Never runs `git`: an entry is checked only against a commit that is already fetched. | off |
| a directory | the path of an index | Names the index to check. At most one may be given. | the current directory |

## `ceesafec doc`

`ceesafec doc [options] [file or directory...]` writes one Markdown page of the public items of the files, or of
the package, with their documentation comments. The code does not have to type-check, but a syntax error stops the
command. Taught in [3.4](./ch03-04-comments) and [14.2](./ch14-02-publishing).

| Option | Argument | What it does | Default |
|---|---|---|---|
| `-o` | `<file>` | Writes the page to the file. | standard output |
| `--title` | `<text>` | Sets the page's title, its first heading. | the first file's name without `.cee`; in a package, the package's name |
| `--missing` | none | Writes no page. Prints a line `path:line: kind name` for each public item without a documentation comment, and exits with status 1 if there is one. | off |

## `ceesafec lsp` and `ceesafec mcp`

Both take no options and serve one client on standard input and output until the input ends.

- `ceesafec lsp` is the language server. An editor starts it. Its exit status is 0 after the client's `shutdown`
  request and 1 if the input ends without one.
- `ceesafec mcp` is the server for AI agents. Its tools check, fix, explain and run CeeSafe code and search the
  specification. `ceesafec new --agent claude` and the other agent names write a configuration that starts it.

## Which command accepts which option

`parse`, `check`, `build`, `run`, `fix` and `test` read their options in one way, so some of them accept an option
that the tables above list only for another command. This table is made by running each of the six with each
option: a command "accepts" an option when it does not answer `unknown option`. Accepting an option does not mean
that it does anything there. The options of `fmt`, `new`, `agents`, `fetch`, `index-check` and `doc` are only
those of their own tables. Those commands, `publish` and `explain` also accept `--diagnostics-format`.

| Option | Accepted by |
|---|---|
| `--all` | `check` |
| `--ar` | `build`, `test` |
| `--cc` | `build`, `test` |
| `--ceesafe-harness` | `run` |
| `--cflag` | `build`, `test` |
| `--debug-assertions` | `build`, `run`, `test` |
| `--deny-warnings` | `parse`, `check`, `build`, `fix`, `test` |
| `--diagnostics-format` | `parse`, `check`, `build`, `run`, `fix`, `test` |
| `--dry-run` | `fix` |
| `--dump-ast` | `parse`, `check`, `build`, `run`, `fix`, `test` |
| `--dump-hir` | `parse`, `check`, `build`, `run`, `fix`, `test` |
| `--dump-nir` | `build`, `test` |
| `--dump-tokens` | `parse`, `check`, `build`, `run`, `fix`, `test` |
| `--emit` | `build`, `test` |
| `--explain-allocations` | `build`, `test` |
| `--explain-bounds-checks` | `build`, `test` |
| `--explain-closure-calls` | `build`, `test` |
| `--explain-frames` | `build`, `test` |
| `--explain-overflow-checks` | `build`, `test` |
| `--exports-header` | `build`, `test` |
| `--filter` | `test` |
| `--flat-c` | `build`, `test` |
| `--format` | `test` |
| `--interp` | `run`, `test` |
| `--jobs` | `build`, `test` |
| `--kernel` | `parse`, `check`, `build`, `run`, `fix`, `test` |
| `--ld` | `build`, `test` |
| `--lib-main` | `build`, `test` |
| `--library-diagnostics` | `parse`, `check`, `build`, `run`, `fix`, `test` |
| `--line-directives` | `build`, `test` |
| `--list` | `test` |
| `--no-bce` | `build`, `run`, `test` |
| `--no-closure-specialisation` | `build`, `run`, `test` |
| `--no-debug-assertions` | `build`, `run`, `test` |
| `--no-float` | `parse`, `check`, `build`, `run`, `fix`, `test` |
| `--no-oce` | `build`, `run`, `test` |
| `--no-placement` | `build`, `run`, `test` |
| `--offline` | `parse`, `check`, `build`, `run`, `fix`, `test` |
| `--package` | `check` |
| `--profile` | `parse`, `check`, `build`, `run`, `fix`, `test` |
| `--program` | `build`, `test` |
| `--record` | `check` |
| `--split` | `build`, `test` |
| `--sweep` | `test` |
| `--sweep-strict` | `test` |
| `--timeout` | `test` |
| `--whole-program` | `check` |
| `-g` | `build`, `test` |
| `-o` | `build`, `test` |

Three options in that table are for the compiler's own tests and tools, and `ceesafec --help` does not list them.
A program's build has no use for them.

| Option | Argument | What it does |
|---|---|---|
| `--dump-tokens` | none | With `parse`, prints each file's tokens on standard output. |
| `--library-diagnostics` | none | Reports diagnostics in the files of the built-in libraries too. |
| `--ceesafe-harness` | `<file>` | With `run`, adds the test program that `ceesafec test` generated. `ceesafec test --interp` uses it to run one test. |

## Exit statuses

| Status | When |
|---|---|
| 0 | The command did what it was asked. Warnings do not change the status unless `--deny-warnings` is given. `--help` and `--version` exit with 0. |
| 1 | The program has an error: `parse`, `check`, `build`, `run`, `fix` or `test` reported a diagnostic that is an error, or `fmt` or `doc` met a syntax error. |
| 1 | The command's own check failed: a test did not pass (`test`), no test's name contains the `--filter` text (`test`), a file would change (`fmt --check`), a public item has no documentation comment (`doc --missing`), the package cannot be published (`publish`), a dependency cannot be resolved (`fetch`). |
| 1 | The C compiler or the archiver ran and reported an error (`build`, `test`). |
| 1 | `lsp` only: the input ended before the client's `shutdown` request. |
| 2 | A usage error: no command, an unknown command or option, an option without its argument or with a wrong one, a file that cannot be read or is not a `.cee` or `.c` file, `run` without `--interp`. The message is followed by the usage text on standard error. |
| 2 | `explain` with a code that does not exist, `check --package` with a name the lock does not have, or a C compiler that cannot be started. The message is printed alone. |
| 70 | `run --interp` only: the interpreter stopped the program at a use of freed, out-of-bounds or uninitialised memory, or a bad free. |
| 134 | `run --interp` only: the program panicked. A built program that panics ends with the same status. |
| any other | `run --interp` only: the program's own exit status, such as 3 after `Process.exit(3)`. |

## Environment variables

`ceesafec` reads these variables. None is required.

| Variable | Read by | What it sets | When it is not set |
|---|---|---|---|
| `CC` | `build`, `test` | The C compiler, when `--cc` is not given. | `cc`; on Windows the first found of clang, gcc and Visual Studio's `cl` |
| `CEESAFE_HOME` | every command that reads a package | The directory where `ceesafec` keeps fetched git dependencies (`git/checkouts/`) and what `check` saves between runs (`cache/records/`) ([7.4](./ch07-04-dependencies)). | `.ceesafe` in the user's home directory |
| `HOME`, or `USERPROFILE` on Windows | every command that reads a package | The user's home directory, where `.ceesafe` is when `CEESAFE_HOME` is not set. | `.ceesafe` is taken in the current directory |
| `CEESAFE_INDEX` | `fetch`, and every command that reads `ceesafe.lock` | The URL of the package index for registry dependencies ([14.2](./ch14-02-publishing)). A lock written with one index does not match another. | `https://github.com/CeeSafe/CeeSafe-index` |
| `PATH` | `build`, `test` | Where the C compiler and the archiver are looked up when they are named without a directory. | no default |
| a variable named in the manifest, as `$NAME` or `${NAME}` at the start of a `lib-dirs` or `include-dirs` entry | every command that reads the manifest | The start of that directory's path ([14.1](./ch14-01-build-settings)). | `N0321`: the variable must be set |
| `INCLUDE` | `build` on Windows | Says that Visual Studio's environment is already set up. When it is not set and the C compiler is clang or `cl`, `ceesafec` sets that environment up for the compiler. | `ceesafec` finds Visual Studio and sets it up |
| `ProgramFiles`, `ProgramFiles(x86)` | `build` on Windows | Where LLVM's clang and Visual Studio are looked for. | those places are not searched |
| `NUMBER_OF_PROCESSORS` | `build --split` on Windows | The default of `--jobs`. | 1 |

A program that `ceesafec build` made reads two variables of its own when it starts. `ceesafec test` finds leaks
from the same report.

| Variable | Read by | What it does |
|---|---|---|
| `CEESAFE_ALLOC_REPORT` | a built program | With the value `1`, the program prints `ceesafe alloc: attempts=<n> live=<m>` on standard error when `main` returns: how many allocations it asked for, and how many were still live ([22.3](./ch22-03-inside-a-host-program)). |
| `CEESAFE_FAIL_ALLOC_AT` | a built program | With the value `k`, the program's allocation number `k` fails, so the code that handles `AllocError` runs ([22.3](./ch22-03-inside-a-host-program)). |

## The manifest: `ceesafe.toml`

A package is a directory with `ceesafe.toml`. The file is a subset of TOML: `[table]` headers, `key = value` lines
whose value is a string, a boolean, an array of strings or an inline table of strings, and `#` comments. An
unknown key is `N0321`. Taught in [7.1](./ch07-01-package-and-manifest), [7.4](./ch07-04-dependencies) and
[14.1](./ch14-01-build-settings).

| Table | Key | Value | What it sets | Default |
|---|---|---|---|---|
| `[package]` | `name` | a string | The package's name: an identifier that is not a keyword and not `core`. It is required. | none: required |
| `[package]` | `version` | a string, `MAJOR.MINOR.PATCH` | The package's version. It is required to publish. | none |
| `[package]` | `kind` | `"executable"`, `"library"` or `"object"` | What the package is: a program with a `main`, a library for other packages, or one relocatable object. | `"executable"` |
| `[package]` | `profile` | `"hosted"` or `"freestanding"` | The profile, as `--profile`. | `"hosted"` |
| `[package]` | `float` | `true` or `false` | `false` rejects float types and literals, as `--no-float`. | `true` |
| `[package]` | `kernel` | `"cosmoos"` | With `profile = "freestanding"`, includes the kernel library, as `--kernel cosmoos`. | none |
| `[package]` | `trusted` | `true` or `false` | `true` lets the package declare its own types `@threadSafe`. | `false` |
| `[package]` | `sources` | an array of directories | Where the package's `.cee` files are. | `["src"]` |
| `[package]` | `tests` | an array of directories | The test directories. Their files are sources for `check` and `test` only. | `["tests"]` |
| `[package]` | `c` | an array of files | C files compiled and linked with the executable. | none |
| `[package]` | `ceesafec` | a string, a version requirement such as `">=0.42.0"` | The compiler the package needs. A compiler whose version does not satisfy it reports `N0321` before it checks anything. | any compiler |
| `[package]` | `links` | an array of library names | System libraries to link: `["ssl"]` is `-lssl`. | none |
| `[package]` | `lib-dirs` | an array of directories | Where those libraries are. Absolute, or relative to the package. | none |
| `[package]` | `include-dirs` | an array of directories | Where the headers for the package's C files and `@header`s are. | none |
| `[target.linux]`, `[target.macos]`, `[target.windows]` | `links`, `lib-dirs`, `include-dirs` | arrays, as in `[package]` | More libraries and directories on that system, added after those of `[package]`. | none |
| `[embed]` | `roots` | an array of directories relative to the package | Directories outside the package from which `embed` and `embedText` may read. | none: only the package's directory |
| `[dependencies]` | a package's name | a string, a version requirement such as `"2.1"` | A dependency from the registry. | none |
| `[dependencies]` | a package's name | `{ path = "../dir" }` | A dependency in a directory, relative to this manifest. | none |
| `[dependencies]` | a package's name | `{ git = "url" }` with one of `rev = "commit"`, `tag = "name"`, `branch = "name"` | A dependency in a git repository, at that commit, tag or branch. | none |
| `[dependencies]` | a package's name | `{ version = "0.9", index = "url" }` | A dependency from the registry, from another index than the default one. | none |

## The files `ceesafec` reads and writes

| File or directory | Written by | What it is |
|---|---|---|
| `ceesafe.toml` | `new`; then you | The manifest. Every command that works on a package reads it. |
| `ceesafe.lock` | `fetch` | The resolved dependencies: for each a `[[package]]` with `name` and `source` (`path+…`, `git+…` or `registry+…`), and for git and registry packages `commit` and `hash`, with `version` and `repository` for a registry package. Every other command reads it and reports `N0324` when it is missing or does not match the manifest. Commit it with `ceesafe.toml`. A package without dependencies needs none. |
| `src/`, or the directories of `sources` | you; `fmt` and `fix` rewrite them | The package's `.cee` files. |
| `tests/`, or the directories of `tests` | you; `fmt` and `fix` rewrite them | The test files, read by `check` and `test`. |
| `out/` | `build`, `test` | Everything `ceesafec` builds in a package. It is not committed: `new` writes a `.gitignore` with `out/`. |
| `out/<name>` | `build` | The executable, named after the package. On Windows it is `out\<name>.exe`. |
| `out/<name>.o` | `build --emit obj`, or `build` of a package of kind `object` | One relocatable object. |
| `out/<name>.a` | `build --emit lib` | A static library. The host's headers are written beside it: `out/ceesafe/embed.h` and, when the program has exports, `out/ceesafe_exports.h`. |
| `out/<name>-c/` | `build --emit c` | The generated C: `main.c`, the runtime's C files, and the `ceesafe/` headers. |
| `out/<name>.dSYM` | `build -g` on macOS | The debug information of a debug build. |
| `out/<name>-tests` | `test` | The test program. |
| `out/ceesafe-test/` | `test` | The generated test harness `harness.cee` and the lists of tests. |
| `<file>`, `<file>.o`, `<file>.a`, `<file>-c/` in the current directory | `build` with files, outside a package | The same outputs, named after the first `.cee` file without its extension. `-o` names another place. |
| `AGENTS.md` and the other agents' files | `new`, `agents` | Instructions for AI coding agents. `ceesafec` rewrites only the part between its own markers. |
| `$CEESAFE_HOME/git/checkouts/` | `fetch`; another command when a locked checkout is missing, unless `--offline` | The git dependencies, each at its locked commit. |
| `$CEESAFE_HOME/cache/records/` | `check` | What `check` keeps of each package between runs, so that the next check of a package that depends on it is faster. Deleting it loses nothing. |

The specification of the command is [Tooling](https://ceesafe.github.io/CeeSafe/reference/tooling), and the
guide's shorter page is [The ceesafec command](https://ceesafe.github.io/CeeSafe/guide/ceesafec).
