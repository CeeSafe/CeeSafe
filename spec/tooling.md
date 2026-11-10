# Tooling (M10)

The commands of `ceesafec` beside `build`, `check` and `run`. The formatter has its own document (`formatting.md`).

## 1. Documentation generator (T2)

```sh
ceesafec doc file.cee...                 # Markdown for the files' public items, on standard output
ceesafec doc lib/core -o spec/api/core.md --title "lib/core"
ceesafec doc --missing file.cee...       # list the public items without a doc comment; exit 1 if there are any
```

The files are one package, as for `check` (a directory means every `.cee` file under it, in ordinal path order).
`lib/core` is documented only when it is named. A file with a syntax error stops the command: its diagnostics are
printed and nothing is written. The package does not have to type-check: documentation is made from the syntax, so it
can be written for code that does not build yet.

### 1.1 What is documented

- Every item declared `public`: a function, a constant, a struct, an enum, an interface, and a function in an
  `extern "C"` block.
- Inside a public struct: its `public` fields, its `public` methods, and whether it has a `drop` block. Inside a public
  enum: every variant. Inside a public interface: every associated type, constant and method (ADR 0086 §2.4,
  §2.5), in source order.
- An `impl I for T` block, listed under `T` as "Implements `I`" when `T` is a public struct or enum of the package.

Nothing else is listed. Items in a `namespace` are listed under their simple names, as the language names them today
(type-system.md §1).

### 1.2 The page

The output is deterministic: the same files give the same bytes.
- A title (`--title`, or the first file's name without its extension) as a level-1 heading, then a list of the
  documented items, each a link to its section.
- One level-2 section per item, in the order of the files as given and of the items in each file. Its heading is the
  item's kind and name (`struct Volatile`, `fn sortBy`).
- The item's **signature** in a `ceesafe` code block: the item's attributes and its declaration up to its body (the
  `{ … }` or `=> …` of a function, the members of a struct), as `ceesafec fmt` prints them (formatting.md). A struct's
  block lists its public fields; an enum's, its variants; an interface's, its associated types, constants and
  methods.
- The item's **doc comment** (grammar.md §1.2): its lines as written, which are Markdown. An item without one has
  only its signature.
- A struct's public methods follow as level-3 sections (`StructName.method`), each with its signature and doc
  comment; then "Implements `I`" for each `impl`.

### 1.3 Missing documentation

`--missing` writes nothing but a line per public item without a doc comment, `path:line: kind name`, and exits 1 if
there is any. It is a lint for a package's public surface; nothing requires it to pass.

### 1.4 In the repository

`spec/api/core.md` is generated from `lib/core` with `ceesafec doc lib/core -o spec/api/core.md --title "lib/core"`, run
on the embedded `lib/core` (its sources under CeeSafe's names). `tools/spec_check.py` fails if it is out of date.

## 2. Language server (T3)

```sh
ceesafec lsp        # a Language Server Protocol server on standard input and output
```

The server speaks LSP 3.17 over JSON-RPC 2.0, with `Content-Length` framing, on standard input and output. It is part
of `ceesafec` and has no dependency beyond CeeSafe's own libraries. The editor starts it; `editors/vscode` is a VS Code
extension that does (§2.4).

### 2.1 What a document is checked with

A document that is a source of a package (a `ceesafe.toml` above it whose sources include it) is checked with its
package, as `ceesafec check` in the package would check it (§4.4): the editor's text for the document, the package's
other files from disk, and the dependencies its `ceesafe.lock` names, never fetching; the manifest's profile and `float`
apply. Any other document, or one whose package does not resolve, is checked as a package of its own, with
`lib/core`, as `ceesafec check file.cee` would check it, using the text the editor holds. Its profile is hosted, unless
its first line is `// profile: freestanding` (the corpus convention, `testing.md` §3).

Checking reads the files the document's `embed` and `embedText` calls name (`type-system.md` §10.1, ADR 0087 §2.8),
under the same rules as the command line: inside the package's directory and its `[embed] roots`, or, for a document
checked alone, below the document's own directory. A document whose URI is not a file on disk embeds nothing. An
embedded file is read again at each check, so a change to it shows at the document's next change or save.

### 2.2 Capabilities

| Request or notification | What the server does |
|---|---|
| `initialize` | Answers with the capabilities below: full-text synchronisation, hover, definition, document symbols, formatting, quick fixes |
| `textDocument/didOpen`, `didChange`, `didClose` | Keeps the text; after an open or change, checks the document (§2.1) and publishes its diagnostics. A close clears them |
| `textDocument/publishDiagnostics` (sent) | Every diagnostic of the check: its range, severity, code (`N0205`) and message, with each note as related information |
| `textDocument/hover` | On a name or an expression: its type, as CeeSafe writes it. On a local or parameter, `let x: T` or `var x: T`; on a function or method, its signature; on a field, `name: T` |
| `textDocument/definition` | On a local, parameter, function, method, field, struct, enum or interface name: the location of its declaration, if it is in the document |
| `textDocument/documentSymbol` | The document's items and their members, as a tree: functions, structs (fields and methods), enums (variants), interfaces, constants |
| `textDocument/formatting` | The document formatted by `ceesafec fmt` (`formatting.md`), as one edit; nothing if it has a syntax error |
| `textDocument/codeAction` | The fixes (§5.2) of the diagnostics in the requested range, as `quickfix` actions (§5.5) |
| `shutdown`, `exit` | Stops |

Anything else is answered with the JSON-RPC error `MethodNotFound` (requests) or ignored (notifications).

Positions are LSP positions: zero-based lines, and columns in UTF-16 code units, which the server converts to and from
the documents' UTF-8 bytes. Hover and definition use the typed tree of the last check; if the document does not parse,
they answer null and only the diagnostics are shown.

### 2.3 Errors

The server never stops on a bad document. A request that fails inside the compiler is answered with the JSON-RPC
error `InternalError` and the exception's message, and the server goes on. A message that is not valid JSON-RPC is
answered with `ParseError` when it has an id.

### 2.4 The VS Code extension

`editors/vscode` holds:
- a TextMate grammar for `.cee` files (keywords, literals, comments, attributes), whose keyword list is the
  compiler's (`editors/vscode/test/keywords.test.mjs` compares them);
- a language configuration (comments, brackets, auto-closing pairs);
- the client, which starts `ceesafec lsp` (the `ceesafe.server.path` setting, default `ceesafec` on the `PATH`) through the
  `vscode-languageclient` library, bundled with it into `dist/extension.cjs`.

It is built with [Vite+](https://viteplus.dev/) (`vp`), not with `ceesafec`:

```sh
cd editors/vscode
vp install            # the dependencies, pinned by pnpm-lock.yaml
vp test               # the grammar and the server tests below
vp run check          # lint and formatting
vp run package        # bundle the client (vp pack) and make ceesafe-language-0.1.0.vsix (vsce)
```

Its tests: `test/grammar.test.mjs` tokenizes CeeSafe lines with `vscode-textmate`, as VS Code does, and checks the scope
each token gets; `test/server.test.mjs` drives a built `ceesafec lsp` through `vscode-jsonrpc`, the library under VS
Code's client (diagnostics, hover, definition, an unknown method, shutdown and exit).

## 3. Debugging (T4)

A CeeSafe program is debugged as the C it compiles to, with the C mapped back to CeeSafe lines. No debugger needs to
know about CeeSafe.

### 3.1 A debug build

`ceesafec build -g file.cee -o program` writes `#line` directives into the C (`c-backend.md`, source maps) and compiles
it with `-g -O0`. A debugger then shows CeeSafe files and lines: breakpoints set by CeeSafe line stop there, and stepping
goes from one CeeSafe statement to the next. A line can take more than one step, since one CeeSafe statement can be several
C statements (a checked addition, a temporary).

A debug build also turns debug assertions on (ADR 0087 §2.5): `debugAssert(…)` is evaluated, and a false one panics.
`--no-debug-assertions` turns them off in a `-g` build, `--debug-assertions` on in any `build` or native `run`.
`ceesafec test` and the interpreter (`run --interp`) evaluate them unless given `--no-debug-assertions`. The C names the
choice once, `#define CEE_DEBUG_ASSERTIONS 1` (or `0`), in a program that has a `debugAssert`; each assertion is in an
`#if CEE_DEBUG_ASSERTIONS` block.

| | lldb | gdb |
|---|---|---|
| Stop at a CeeSafe line | `breakpoint set -f demo.cee -l 12` (`b demo.cee:12`) | `break demo.cee:12` |
| Stop when a check fails, before the program aborts | `b ceesafe_panic` | `break ceesafe_panic` |
| Where am I | `bt` | `bt` |

### 3.2 Names

The C names follow `c-backend.md` §2.1, so a CeeSafe name can be found in the debugger:

| CeeSafe | C |
|---|---|
| function `length` | `ceeF_length` |
| method `Counter.bump` | `ceeM_7Counter_bump` (the type's name, prefixed by its length) |
| local or parameter `sum` | `v_sum_1` (a number makes each local's name unique in its function) |
| a temporary the compiler made | `t_3` |
| field `x` | `f_x` |
| struct `Point` | `ceeT_Point` |

### 3.3 CeeSafe names in the debugger

`editors/debug/ceesafe_lldb.py` and `editors/debug/ceesafe_gdb.py` add three commands, which print with CeeSafe names:
- `ceesafe-locals`: the current frame's locals and parameters, `name = value`, without the compiler's temporaries; struct
  values show their fields by their CeeSafe names;
- `ceesafe-bt`: the backtrace, one frame a line, `CeeSafe name at file:line` (`Counter.bump at demo.cee:3`); an async
  fn's resume function shows as the fn (`async closure` for a closure);
- `ceesafe-tasks`: the stopped thread's async tasks (type-system.md §40), one a line, from the runtime's
  `ceesafe_async_tasks()` (c-backend.md §6): `gather at demo.cee:18 (waiting for its scope)`, a scope's children
  indented under it.

Load them with `command script import editors/debug/ceesafe_lldb.py` (lldb) or `source editors/debug/ceesafe_gdb.py` (gdb).

Split C (`--split`, c-backend.md §2.4) renames the program's functions to `ceeF_<program>_…`: set `CEESAFE_PROGRAM` to
the program's name before starting the debugger, and the helpers read them as before.

### 3.4 What is checked

`tools/debugger_check.py` builds a program with `-g` and runs it under lldb (macOS) or gdb (Linux), in batch mode. A breakpoint
at a CeeSafe line must stop at that line, `ceesafe-locals` must show the locals by their CeeSafe names, and a breakpoint on
`ceesafe_panic` must give a backtrace whose frames are CeeSafe functions at CeeSafe lines. In an async program stopped
inside a child task, `ceesafe-tasks` must list the awaiting task, the running child and its waiting sibling at the
lines they wait at. On a machine with neither debugger,
the test does nothing, and the run log says which debugger ran.

## 4. Packages (T6, ADR 0013)

### 4.1 The manifest

A package is a directory with `ceesafe.toml`. Its sources are the `.cee` files under the directories `sources` names
(default `src`), in ordinal path order. The manifest is a subset of TOML: `[table]` headers, `key = value` lines
whose value is a string, a boolean, an array of strings or an inline table of strings, and `#` comments.

```toml
[package]
name = "app"                 # required: an identifier, not a keyword and not "core"
version = "0.1.0"            # optional; required to publish (§4.8), MAJOR.MINOR.PATCH
kind = "executable"          # "executable" (default), "library", or "object" (profiles.md §4.3)
profile = "hosted"           # or "freestanding"
float = true                 # false is --no-float (type-system.md §35.9)
kernel = "cosmoos"           # with profile = "freestanding": include lib/kernel, as --kernel cosmoos (ADR 0025)
trusted = true               # its types may be declared @threadSafe (ADR 0025 §3.2); listed for reviewers
sources = ["src"]
c = ["src/shim.c"]           # C files compiled and linked with an executable (type-system.md §28.6)
tests = ["tests"]            # test directories (§10): default "tests"; their files are sources for check and test only
ceesafec = ">=0.20.0"        # the compiler this package needs (ADR 0076): a requirement, as a dependency's (§4.7)
links = ["ssl", "crypto"]    # system libraries to link (ADR 0047): -lssl -lcrypto; ssl.lib crypto.lib with cl
lib-dirs = ["$OPENSSL_DIR/lib"]          # where they are; absolute, or relative to the package
include-dirs = ["$OPENSSL_DIR/include"]  # headers for this package's own C files only

[target.windows]             # also [target.macos] and [target.linux]: added after [package]'s on that system
links = ["libssl", "libcrypto"]

[embed]                      # where `embed` may read besides the package's directory (ADR 0087 §2.8)
roots = ["../build/codegen"]

[dependencies]
geometry = { path = "../geometry" }
lz4 = { git = "https://example.org/lz4-cee.git", tag = "v1.2.0" }      # or rev = "<commit>", or branch = "main"
shapes = "2.1"               # from the registry (§4.6): a version requirement
json = { version = "0.9", index = "https://example.org/my-index.git" }   # from another index
```

**The compiler requirement** (ADR 0076). When a manifest is read (the root's, and each dependency's, by every command
that reads it), a ceesafec whose version does not satisfy `ceesafec` reports N0321 at the value, before anything is
checked: `this package needs ceesafec >=0.20.0; this is ceesafec 0.19.1`. A development version (`0.20.0-dev`) counts
as its MAJOR.MINOR.PATCH. A compiler older than the field reports it as an unknown field at the same line.

A dependency's key is its package's `name`: a dependency is known by the name its own manifest gives it. A
dependency of kind `executable` is N0321: only a library's items are used. Errors in the manifest are N0320 (its
syntax) and N0321 (a missing, unknown or invalid field), with the line.

**System libraries** (ADR 0047):
- `links` names libraries: letters, digits, `_`, `.`, `+` and `-`, not starting with `-`. `lib-dirs` and `include-dirs`
  are directories, absolute or relative to the package, not starting with `-`.
- A directory may start with `$NAME` or `${NAME}`: the environment variable's value, read when the manifest is. An
  unset one is N0321. Nothing runs (ADR 0013), and the hash (§4.3) is of the manifest's text, not the value.
- A `[target.OS]` table (`linux`, `macos` or `windows`) holds the same three fields. The host's is added after
  `[package]`'s; every one is checked.
- The libraries and library directories of every package in the build reach the executable's link, a library's
  included. Packages are taken dependents first (by their longest path from the root). A library named twice keeps
  its last place, so it follows what uses it; a directory keeps its first.
- A package with `include-dirs` has its own C files compiled apart, with `-c` and those directories, and the objects
  join the build. No other C file sees its directories.
- `--emit c` writes the link line as `ceesafe_link.txt` beside the C, and `--emit obj` and `--emit lib` as
  `<output>.link.txt`, when the
  manifests name any: one argument per line, each separately compiled C file after its include directories, then the
  library directories and the libraries (`cl`'s spelling when `--cc` is `cl`).

**Embed roots** (ADR 0087 §2.8, `type-system.md` §10.1). A package's files may embed files inside the package's
directory (the one holding `ceesafe.toml`), and inside each directory `[embed] roots` lists:
- A root is relative to the package's directory, with `/` separators, and nothing in it is expanded: an absolute path,
  a drive, a `\`, `:`, `$` or `%`, or a leading `~` is N0321 at the array, and so is any other field in `[embed]`.
  It may use `..`. Roots are resolved when a file is embedded, not when the manifest is read: one that does not exist
  (yet: a build's generated directory) reaches nothing, and an `embed` below it is N0362 until it does.
- A root is its real path: every symbolic link in it is followed, so a root that is a symbolic link stands for the
  directory it leads to.
- **Only the root package and the path dependencies it reaches through path dependencies alone may have roots
  outside their directory.** They are code the user works on, whose manifests the user wrote. A git or registry
  dependency, and every package reached through one (its path dependencies, at any depth), embeds only inside that
  checkout: a root counts only when its real path is inside it, so a checkout reaches nothing outside itself, through
  `..`, through a symbolic link it holds, or through a path dependency it names. So does a package whose directory
  lies in the package cache (`$CEESAFE_HOME`, §4.3), even as the root package: the language server going to a
  definition in a checkout, or `check` run there, gives it no more than the build that fetched it, and the path
  dependencies such a root reaches are kept inside its directory, wherever they lie.


From the package being built, every dependency is resolved, and then its dependencies, recursively:
- `path` is a directory relative to the manifest that names it, holding the dependency's `ceesafe.toml`;
- `git` is a repository; `rev` names a commit, `tag` and `branch` are resolved to one (§4.3). The commit is
  checked out into the cache, `$CEESAFE_HOME/git/checkouts/<repository>/<commit>` (default `~/.ceesafe`), and the
  dependency is the package at the checkout's root.

A package name reached by two different sources (two paths, two commits, a path and a commit) is N0326, naming both
dependents. A package that depends on itself, directly or not, is N0327. A missing directory or manifest is N0322.
Nothing runs during resolution except `git` (ADR 0013 D5).

### 4.3 The lock file

`ceesafe.lock`, beside `ceesafe.toml`, records the resolution so that every build uses the same code:

```toml
# Written by ceesafec fetch. Commit it with ceesafe.toml.
[[package]]
name = "lz4"
source = "git+https://example.org/lz4-cee.git"
commit = "3f2a9c0d…"         # the full commit id
hash = "sha256:…"            # of the checkout's manifest and sources

[[package]]
name = "geometry"
source = "path+../geometry"
```

- `ceesafec fetch` resolves every dependency, fetching git repositories and resolving tags and branches (`git
  ls-remote`, `git fetch`), and writes the lock. A `rev` is used as written; a `tag` or `branch` is the commit it names
  at that moment, until the next `ceesafec fetch`.
- Every other command reads the lock. It fails with N0324 if the manifest has a dependency the lock lacks or
  records differently (run `ceesafec fetch`), and with N0325 if a git checkout's manifest and sources no longer have the
  lock's hash. A missing checkout of a locked commit is fetched, unless `--offline` is given (N0323 if it cannot be).
- A path dependency has no hash: it is code being worked on beside the package.
- The hash is SHA-256 over, for the manifest and each source file in ordinal order, the path relative to the package
  root, a zero byte, the file's bytes and a zero byte.

### 4.4 Commands

- `ceesafec new <name>` makes `<name>/ceesafe.toml` and `<name>/src/main.cee` (a `main` that prints a line); `--lib`
  makes a library with `src/lib.cee` instead. It also writes `.gitignore` (`out/`) and the agent instructions of
  §7: `AGENTS.md` by default, others with `--agent`, none with `--no-agent`.
- `ceesafec agents [--agent …]` writes or refreshes the agent instructions of the package in the current directory or
  above it (§7.3).
- `ceesafec fetch [--offline] [--update [name]]` resolves and writes `ceesafe.lock` (§4.7).
- `ceesafec publish` checks that the package can be published and prints its index entry (§4.8).
- `ceesafec test` runs the package's `@test` functions (§10).
- `ceesafec build`, `check`, `run`, `doc` and `fmt` without file arguments use the package of the nearest `ceesafe.toml` in
  the current directory or above it. `build` writes `out/<name>` (an executable, or `out/<name>.o` for `object`), the
  C sources with `--emit c`, or `out/<name>.a` with `--emit lib` (§8.3); the manifest's profile, `float` and C files apply, and command-line options add to
  them. `fmt` formats the package's own sources, not its dependencies'.
- `ceesafec check` in a package (ADR 0082 §2.7) checks that package fully, its tests included, and takes each
  dependency from its **interface record** (§4.4.1): the dependency is parsed and its declarations resolved, but its
  bodies are checked again only where a dependent compiles or reads them (generic code, async functions, functions
  with a closure or function parameter or `@effectsOf`, `@constant` functions); every other body's effects come from
  the record. A dependency without a valid record is checked first, alone, and recorded: without its tests or entry
  function, its own dependencies from their records, up to `--jobs <n>` packages at once (default: the number of
  CPUs). A dependency with errors is one diagnostic, N0364, at its line in the manifest, and the package's check stops
  where the whole program's would. The diagnostics are otherwise those of `check --whole-program`, which checks every
  package as one program (ADR 0013 D4) and reads no record: the oracle `tools/package_tests.py` §23 compares the two
  for every package of its fixtures.
- `ceesafec check --all` checks every package of the lock, as `check` would in each, in dependency order, `--jobs <n>`
  at once, and prints one report (§5.1): its diagnostics sorted by package, file and position, then, as text, a line
  per package with errors or warnings, or as JSON a `packages` summary. It exits with 1 if any package has an error.
- `build`, `run`, `test` and `fix` check every package as one program and read no record: a record is a cache of a
  result `check` proved, and only `check` uses it (ADR 0082 §2.7, §8).
- The language server (§2.1) checks a document that belongs to a package (a `ceesafe.toml` above it, whose sources
  include it) with that package: the open document's text, the package's other files from disk, and its locked
  dependencies. Without a lock, or outside a package, the document is checked alone as before.

### 4.4.1 Embedded files and what is cached

`embed` and `embedText` (`type-system.md` §10.1) make a program depend on files that are not sources. Every command
that checks a package reads them afresh, so a file changed after `check` is what `build` compiles, and the language
server sees a change at its next check. Within one compilation each file is read once, by its real path. Neither the
lock's hash (§4.3), which covers a package's manifest and sources, nor `fetch` looks at embedded files.

**Interface records** (ADR 0082 §2.7) are what `check` keeps of a package between runs, in
`$CEESAFE_HOME/cache/records/<package>-<key>.rec`:
- **The key** is a SHA-256 of the record format, `ceesafec --version`'s banner, the profile (hosted or
  freestanding, `--kernel`, `--no-float`), the package's name, manifest and sources (each with its path in the
  package), and its direct dependencies' interface hashes. Any change to one of them is another key: a miss, and a
  check.
- **The record** says whether the package checked and the stage of its first error; and for each function whose body
  dependents skip, where it is declared, its inferred effects (`type-system.md` §32.1) and the explanation of each
  (the steps an N0295 or N0360 names through it, and their number).
- **The interface hash** covers the package's sources with those skipped bodies left out (only their line breaks
  kept, so that what follows keeps its positions), what the record says, and its dependencies' interface hashes. A
  change inside a skipped body that changes neither the effects nor whether the package checks leaves the hash, and
  so every dependent's record, as it was.
- **Opaque records.** A package that embeds a file, or whose skipped bodies make a `dyn` table of an interface with an
  async method (whose frames, and recursion through `await`, belong to the whole program), has an opaque record: its
  dependents check its bodies, and its own diagnostics, not N0364, say whether it fails.
- **Trust.** A record is a cache of a result this compiler proved, never a promise from outside, and any doubt is a
  miss. `$CEESAFE_HOME/cache` and `cache/records` are made with mode 0700; a record is read only if it and both
  directories belong to the user running `ceesafec` and no one else may write to them, from the descriptor whose
  owner and mode were checked; one that cannot be read, is cut short (its last field is the SHA-256 of the rest), or
  names another key, banner, profile or package is a miss. A record is written to a new name created for it alone
  (mode 0600) and renamed, so agents that check at once share the cache and none reads half a record. Owners and
  modes are read where `ceesafec` knows `fstat`'s layout, macOS and Linux on arm64 and x86-64; elsewhere, Windows
  included, no record is read or written, and every `check` checks every package.

### 4.5 Errors

| Code | Meaning |
|---|---|
| N0320 | `ceesafe.toml` or `ceesafe.lock` is not valid in the TOML subset of §4.1 |
| N0321 | A manifest field is missing, unknown, or has an invalid value |
| N0322 | A path dependency's directory or manifest is missing, or a dependency is named differently from its key |
| N0323 | A git dependency cannot be fetched or resolved, or is needed with `--offline` and absent |
| N0324 | The lock file is missing or does not match the manifest: run `ceesafec fetch` |
| N0325 | A git checkout's manifest and sources do not match the lock's hash |
| N0326 | One package name reached through two different sources |
| N0327 | A dependency cycle |
| N0328 | An invalid version, version requirement, or index entry |
| N0329 | No versions satisfy the requirements: the chain of requirements that conflict |
| N0335 | `ceesafec publish`: the package cannot be published as it is |

### 4.6 The registry (M13 G6, ADR 0017)

A **registry dependency** names a package and a **version requirement**. The package is found in an **index**: a git
repository, by default `https://github.com/CeeSafe/CeeSafe-index`, or `$CEESAFE_INDEX`, or the dependency's `index`.

- **Versions** are `MAJOR.MINOR.PATCH`, three decimal numbers with no leading zeros, compared numerically. There
  are no pre-release or build suffixes in v1.
- **Requirements** are one or more comparators separated by commas, all of which must hold:
  - `"1.4"`, `"^1.4"` and `"^1.4.2"` are caret requirements: at least that version, below the next major version, or
    below the next minor for `0.x` (`"0.3"` is `>=0.3.0, <0.4.0`) and the next patch for `0.0.x`.
  - `"=1.4.2"` means exactly that version.
  - `">=1.4"`, `">1.4"`, `"<2"` and `"<=1.9.3"` compare. A missing minor or patch is 0.
  - `"*"` means any version.
- **The index** holds `packages/<name>.toml`, with one `[[version]]` table per published version, in publishing order:
  ```toml
  [[version]]
  version = "1.4.0"
  git = "https://github.com/someone/geometry.git"
  commit = "3f2a9c0d…"                      # a full commit id
  hash = "sha256:…"                         # of the commit's manifest and sources (§4.3)
  dependencies = { shapes = "2.1" }         # its registry requirements, as its ceesafe.toml writes them
  yanked = false                            # optional; a yanked version is never chosen for a new resolution
  ```
  Entries are only appended. A published package's dependencies are all registry dependencies, and those without
  an `index` come from the index that published the package.

### 4.7 Version solving

`ceesafec fetch` resolves path and git dependencies as before (§4.2), gathering the registry requirements of the root
and of every path and git package. It then chooses one version of each registry package, transitively, from the
index's entries:

- The **newest** version that satisfies every requirement on it is chosen, and so on through its own dependencies.
  On a conflict, the solver backtracks.
- A version the lock already records is preferred while it satisfies the requirements, so `ceesafec fetch` changes
  nothing that needs no change. `--update` prefers the newest for every package, and `--update <name>` for that one.
- **One version per package name** (OD-26). A name reached through a path or git dependency and through the registry
  is N0326.
- If no choice satisfies every requirement, N0329 shows the requirements on the package that could not be chosen,
  and where each came from:
  `no version of 'shapes' satisfies ^2.1 (from geometry 1.4.0) and ^1.0 (from plot 0.3.0)`.
- Each chosen version's commit is checked out as for a git dependency (§4.2). Its hash must be the index's (N0325),
  and its manifest's name and version must be the entry's (N0328).
- The lock records each as `source = "registry+<index>"` with `version`, `repository` (the git URL), `commit` and
  `hash`. Other commands use the lock: an entry whose version no longer satisfies the manifest's requirement is
  N0324.

### 4.8 Publishing

`ceesafec publish` (no network but `git`, and nothing written) checks the package in the current directory:

- it is a library with a valid `version`;
- every dependency is a registry dependency;
- it type-checks with its dependencies;
- its git working tree is clean, and its commit is on a remote branch.

It then prints the index entry for the version: the remote's URL, the commit, the hash and the requirements. To
publish, add that entry to `packages/<name>.toml` in the index by pull request. The index's continuous integration
runs `ceesafec index-check`, which checks every new entry:
- the commit exists;
- its manifest has the entry's name and version;
- its hash and dependencies are the entry's;
- the version is newer than every earlier entry.

A new package name is reviewed by the index's maintainers. Later versions from the same repository merge after the
checks pass.

## 5. Diagnostics for programs (M12 A1)

People read diagnostics as text (`diagnostics.md`, "Format"). Programs, AI agents and editors among them, read the
same diagnostics as JSON, and can apply the fixes the compiler is sure of.

### 5.1 `--diagnostics-format=json`

`check`, `build`, `run` and `parse` accept `--diagnostics-format=json` (or `=text`, the default). With `json`,
diagnostics are not written to standard error; instead one JSON document is written to standard output, as one line,
after the command's own work. `build` still writes its outputs, and `run` still runs the program, if there are no
errors; the program's own output then precedes the document. The exit status is unchanged. `"severity"` is `"error"` or `"warning"`,
and `"warnings"` counts the warnings (`diagnostics.md`, "Warnings"): a successful `build` or `run` writes the document
only when there are warnings, and a warning never changes the exit status unless `--deny-warnings` is given.

```json
{
  "version": 1,
  "errors": 1,
  "warnings": 0,
  "diagnostics": [
    {
      "code": "N0249",
      "severity": "error",
      "file": "src/main.cee",
      "package": "app",
      "start": { "line": 7, "column": 10 },
      "end": { "line": 7, "column": 11 },
      "message": "this parameter is 'borrow mut': write 'mut' before the argument to let the call modify it",
      "summary": "`mut` argument marker missing or misplaced",
      "notes": [ { "file": "src/main.cee", "start": { … }, "end": { … }, "message": "…" } ],
      "fixes": [
        { "title": "write 'mut' before the argument",
          "edits": [ { "file": "src/main.cee", "start": { "line": 7, "column": 10 }, "end": { "line": 7, "column": 10 },
                       "newText": "mut " } ] }
      ]
    }
  ]
}
```

- `version` is the format's version: fields may be added within a version, never removed or changed.
- Positions are 1-based lines and columns, columns in UTF-16 code units, as in the text form. `end` is exclusive.
- `file` is the path as the text form prints it: a path given on the command line, or a file of the root package, as
  given (relative to the current directory when it is under it); a dependency's file as its path from the current
  directory, `../alpha/src/util.cee`, so that it can be opened (ADR 0082 §2.1).
- `package` (added in version 1 by ADR 0082 §2.1) is the name of the package the file belongs to, in package mode
  (`build`, `check` or `run` without file arguments): the root package's or a dependency's. It is left out for a build
  of files, and for lib/core's and lib/std's files.
- `summary` is the code's "Meaning" in `diagnostics.md`, which `ceesafec` embeds.
- Diagnostics are in the order the text form prints them. Keys are written in the order shown.
- A dependency with errors (N0364, §4.4) is reported in the manifest of the package checked: `file` is
  `ceesafe.toml` from the current directory, and `package` that package's name.
- `check --all` (§4.4; added in version 1 by ADR 0082 §2.7) writes one document for every package of the lock. Its
  diagnostics are sorted by `package`, then `file`, then position (a diagnostic's order of discovery breaks ties), and
  `errors` and `warnings` count them all. After `diagnostics` comes `packages`: one entry per package, by name, with
  its `name` and its own `errors` and `warnings`:

```json
  "packages": [ { "name": "app", "errors": 1, "warnings": 0 }, { "name": "mid", "errors": 0, "warnings": 0 } ]
```

### 5.2 Fixes

A fix is a list of edits that the compiler is sure resolves the diagnostic. It is offered only where the
intent is unambiguous; a diagnostic with several plausible repairs offers none. Applying a fix removes that
diagnostic and introduces none at the edited place. The kinds:

| Code | When | Edit |
|---|---|---|
| N0249 | an argument for a `borrow mut` parameter lacks `mut`, and is a place that can be borrowed mutably | insert `mut ` before the argument; if the place is a `let` local, also `let` → `var` at its declaration (one fix) |
| N0249 | an argument for a shared parameter has `mut` | delete the `mut ` |
| N0211 | assignment to a `let` local declared in the function | `let` → `var` at its declaration |
| N0248 | `borrow mut` (or a `borrow mut this` call) of a `let` local declared in the function | `let` → `var` at its declaration |
| N0208 | `e as T` between integer types that can lose information | `truncate<T>(e)` (wrap-around, as the message says) |
| N0201 | an extern declaration whose C name a dependency's `public` extern declaration has, identical to it in CeeSafe (type-system.md §28.2; ADR 0081 §2.2) | remove the declaration, with its doc comment and attributes; its call sites are left as they are |
| N0100 | `expected ';'`, with no earlier error in the file, after a declaration, assignment, `return`, or a call statement (not after an expression that cannot be a statement, N0225) | insert `;` after the previous token |

### 5.3 `ceesafec fix`

`ceesafec fix [FILES]` (or the package, without files) applies every fix, writes the files, checks again, and repeats
until no fix applies or after 8 rounds. It then prints the remaining diagnostics, as text or with
`--diagnostics-format=json`, and exits 1 if errors remain. With `--dry-run` it changes no file and prints the
edits of the first round. Edits to one file never overlap: when two fixes would, the later one waits for the next
round.

### 5.4 `ceesafec explain`

`ceesafec explain N0249` prints the code's row of `diagnostics.md` (its meaning and example) and the paragraphs of
`diagnostics.md` that mention it. An unknown code is exit status 2.

### 5.5 In the language server

`textDocument/codeAction` returns the fixes of the diagnostics that touch the requested range as `quickfix` actions,
each with its diagnostic and a `WorkspaceEdit` for the document (§2.2). A fix that would edit another file is not
offered.

## 6. The MCP server (M12 A2)

`ceesafec mcp` serves the [Model Context Protocol](https://modelcontextprotocol.io) on standard input and output, so an
AI agent's host (Claude Code, an IDE, any MCP client) can use CeeSafe's compiler, interpreter and documentation as
tools. It is part of `ceesafec`, with no other dependency, and serves the documents embedded in that `ceesafec`, so the
answers always match the compiler that gives them.

```json
{ "mcpServers": { "ceesafe": { "command": "ceesafec", "args": ["mcp"] } } }
```

### 6.1 Protocol

- JSON-RPC 2.0, one message per line of UTF-8 on standard input and output; nothing else is written to standard
  output. The server stops at the end of its input.
- `initialize` negotiates the protocol version: the client's, if it is one the server supports (`2025-06-18`,
  `2025-03-26`, `2024-11-05`), otherwise the newest of those. The server declares `tools`, `resources` and `prompts`,
  none of which change while it runs, and gives brief usage `instructions`.
- `ping`, `tools/list`, `tools/call`, `resources/list`, `resources/read`, `prompts/list` and `prompts/get` are
  answered; other requests are `MethodNotFound` (-32601); notifications are ignored.
- With version `2025-06-18`, each tool declares an `outputSchema` and its result carries `structuredContent`, with the
  same data as JSON text in `content`. With earlier versions, only `content`.
- A tool call that the tool cannot carry out (a file it cannot read, a source that does not parse when it needs to)
  is a result with `isError: true` and the reason. Unknown tools and invalid arguments are `InvalidParams` (-32602).
  The server never stops on a bad request.

### 6.2 Sources

Every tool that compiles takes its program as either
- `files`: an array of `{ "path": "main.cee", "text": "…" }`, compiled as one package, with nothing read from disk; or
- `paths`: an array of `.cee` file paths, read from disk relative to the server's working directory;

plus `profile` (`hosted`, the default, or `freestanding`) and `noFloat` (default false). Paths in results are the
paths given.

`embed` and `embedText` (`type-system.md` §10.1) read files relative to the source's file on disk: a `paths` file
embeds below its own directory, and a `files` source, which is no file, embeds nothing (N0362). The server reads
nothing else for them.

### 6.3 Tools

| Tool | Input (besides §6.2) | Result |
|---|---|---|
| `ceesafe_check` | | the diagnostics document of §5.1, with fixes |
| `ceesafe_fix` | | `files`: each file's text with every fix applied, over rounds as `ceesafec fix` does (§5.3); `applied`: the number of fixes; `diagnostics`: what remains. Nothing is written to disk |
| `ceesafe_format` | `text`, `path` (optional) | `text`: formatted (`formatting.md`); or an error with the syntax diagnostics |
| `ceesafe_run` | `stdin` (text), `args`, `maxSteps` (default 10,000,000), `maxOutputBytes` (default 65,536) | `outcome` (`exited`, `panicked`, `fault`, `not-compiled`), `exitCode`, `stdout`, `stderr`, `truncated`, and the diagnostics if it did not compile |
| `ceesafe_emit_c` | `explain`: any of `allocations`, `bounds-checks`, `overflow-checks` | `files`: the generated C (`main.c`, and `ceesafe_exports.h` when there is one; not the runtime); `explanations`: the `--explain-*` text |
| `ceesafe_explain` | `code` | `code`, `meaning`, `example`, `paragraphs` of `diagnostics.md` (§5.4) |
| `ceesafe_spec_search` | `query`, `limit` (default 10) | `results`: the sections whose heading or text contain every word of the query, best first: `document`, `heading`, `snippet`, `uri` |
| `ceesafe_spec_section` | `document` (`type-system`, `grammar`, …), `section` (`31.2`, or heading text) | `text` of that section, with its subsections |
| `ceesafe_symbol` | `path`, `line`, `column` (1-based) | the type or signature under the position, and its declaration's location, as hover and definition give them (§2.2) |
| `ceesafe_api` | `name`: a type (`TextWriter`), a member (`TextWriter.decimal`) or a bare member name (`decimal`) | `results`: the entries of `spec/api/core.md`, `spec/api/std.md` and `guide/builtins.md` that document the name (`document`, `heading`, `text`): a type's whole section, a member's entry, or the built-in table rows naming it. With no entry, `suggestions`: up to eight names, the closest by edit distance, the type's real members for `Type.member`, and the entries whose text mentions the member's name |

`ceesafe_run` runs the program in the interpreter (`run --interp`) **sandboxed**:
- files, sockets and processes are refused (a fault that names the refusal);
- `maxSteps` statements at most (a fault), and `maxOutputBytes` of each output kept (`truncated` says if more was
  written; the program goes on);
- 256 MiB of interpreter memory at most: the heap then reports out of memory, which the program sees as
  `AllocError.OutOfMemory`; a local or static allocation beyond it is a fault;
- standard input is `stdin`, the arguments are `args`, and the run has no access to the server's environment.

A fault's exit code is 70 and a panic's 134, as for `run --interp`.

### 6.4 Resources

Each embedded document is a resource `ceesafe://docs/<path>` (for example `ceesafe://docs/docs/type-system.md`,
`ceesafe://docs/site/guide/basics.md`), `text/markdown`, named by its first heading. `resources/read` returns its text.
An unknown URI is the error -32002 (resource not found).

### 6.5 Prompts

| Prompt | Arguments | What it asks |
|---|---|---|
| `write-ceesafe` | `task` | To write CeeSafe for the task: the agent guide (`guide/for-agents.md`), then the task, with the edit-check loop of `ceesafe_check` and `ceesafe_fix` |
| `port-c-to-ceesafe` | `file`, `api` | To port a C file to CeeSafe as C6 and C9 did: imported C structs from the header, safe code over slices, `unsafe` only at the exported boundary, and the C's own tests, a differential test and a benchmark (`cosmoos/c9-report.md`) |

## 7. Agent instructions in packages (M12 A4)

AI coding agents read a file of instructions at the root of a project. `ceesafec new` writes one, so that an agent
working in a new CeeSafe package knows from the start what CeeSafe is, which commands check its work, and the mistakes to
avoid. The convention follows Vite+'s (`vp create`).

### 7.1 The CeeSafe section

`AGENTS.md` holds the **CeeSafe section**, between the lines `<!--CEESAFE START-->` and `<!--CEESAFE END-->`: what CeeSafe is and
where its guide for agents is; the package's layout; the commands (`check`, `fix`, `run --interp`, `build`, `fmt`,
`explain`, `--diagnostics-format=json`); how to use the MCP server; a checklist before finishing; and the common
mistakes of the agent guide. The text is embedded in `ceesafec` (`templates/AGENTS.md`) and is the same for every
package. Everything outside the markers belongs to the package's authors: ceesafec never changes it.

### 7.2 Agents

`--agent` takes one or more of these names, separated by commas or by repeating the option; the default is `agents`.
Each name writes the files below; `AGENTS.md` is written whenever any agent is chosen, since the others refer to it.

| Name | Agent | Instructions | MCP configuration (`ceesafec mcp`, §6) |
|---|---|---|---|
| `agents` | Codex, Amp, OpenCode and others that read `AGENTS.md` | `AGENTS.md` | — |
| `claude` | Claude Code | `CLAUDE.md`: `@AGENTS.md` (Claude Code's import) | `.mcp.json` |
| `gemini` | Gemini CLI | `GEMINI.md`: `@AGENTS.md` (Gemini CLI's import) | `.gemini/settings.json` |
| `copilot` | GitHub Copilot | `.github/copilot-instructions.md`: a copy of the CeeSafe section | `.vscode/mcp.json` |
| `cursor` | Cursor | `.cursor/rules/ceesafe.mdc`: a copy of the CeeSafe section, applied always | `.cursor/mcp.json` |

An unknown name is a usage error (exit status 2), and so is `--agent` with `--no-agent`. The MCP configurations name
the server `ceesafe`, with command `ceesafec` and argument `mcp`, in each tool's format: `{"mcpServers": {"ceesafe": …}}`, or
`{"servers": {"ceesafe": {"type": "stdio", …}}}` for VS Code.

### 7.3 `ceesafec agents`

In a package, `ceesafec agents [--agent …]` writes the chosen agents' files (default `agents`):
- a missing file is written as `ceesafec new` writes it;
- in an instructions file that has the markers, the text between them is replaced with the current CeeSafe section,
  and the rest is kept; a file without them gets the CeeSafe section appended, after a blank line; a file that only
  imports `AGENTS.md` (`@AGENTS.md`) is left as it is;
- an MCP configuration that exists is left as it is, and the command says so: it may hold the authors' other servers.

It prints what it wrote and what it left. Run it after updating `ceesafec`, so the instructions match the compiler.

## 8. The C compiler (`ceesafec build`)

`ceesafec build` writes C and gives it to a C compiler. It needs no configuration when a usual compiler is installed.

### 8.1 Which compiler

`--cc <compiler>` names it; otherwise `$CC`; otherwise:
- on Linux and macOS, `cc`;
- on Windows, the first of these that exists: `clang` on `PATH`; `%ProgramFiles%\LLVM\bin\clang.exe` (where the LLVM
  installer, `winget install LLVM.LLVM`, puts it); `gcc` on `PATH` (MinGW-w64); `cl` on `PATH`; Visual Studio's `cl`
  (§8.2). clang and gcc are fully supported; MSVC is best effort (`c-backend.md` §1.1).

With none, the error names what was looked for and how to install one.

### 8.2 Visual Studio's environment (Windows)

clang on Windows and `cl` compile against Visual Studio's C library and the Windows SDK, which they find through the
environment that Visual Studio's developer prompt sets (`INCLUDE`, `LIB`, `PATH`). When that environment is missing
(`INCLUDE` unset) and the compiler is clang or `cl`, `ceesafec build` sets it up for the compiler, as the developer prompt
would:
1. find Visual Studio, or its Build Tools, with the C++ tools for this machine: `vswhere -latest -products *
   -requires Microsoft.VisualStudio.Component.VC.Tools.ARM64` on an ARM64 Windows,
   `…VC.Tools.x86.x64` otherwise (other products that `vswhere` knows, such as SQL Server Management Studio, have no
   C++ tools and are not taken);
2. run its `VC\Auxiliary\Build\vcvarsall.bat` for the machine's architecture (`arm64` or `x64`) and take the
   environment it leaves;
3. run the compiler in that environment. The user's own environment is not changed.

Without Visual Studio, clang still works where the environment is already set; otherwise the error says to install
Visual Studio's "Desktop development with C++" workload (or the Build Tools), or MinGW-w64.

### 8.3 Flags

| Compiler | Flags |
|---|---|
| clang, gcc, `cc` | `c-backend.md` §1's: `-std=c11`, the strict warnings, `-ffp-contract=off -fno-math-errno`, `-O2` (or `-g -O0`), `--cflag`s, the C files and the separately compiled packages' objects, the manifests' `-L<dir>` and `-l<name>` (§4.1), `-o <output>`; on Windows also the 8 MiB stack, `-Wl,/STACK:8388608` for clang and `-Wl,--stack,8388608` for gcc. With `--split`, each C file is compiled alone with the same flags and `-c`, at most `--jobs` at once, and the objects are linked with `-g` (for `-g`), the `--cflag`s, the libraries and the stack (`c-backend.md` §2.4) |
| `cl` (MSVC) | `/nologo /std:c11 /W3 /O2` (or `/Zi /Od` with `-g`), `--cflag`s, `/Fo:` the build directory (so no `.obj` lands in the project), `/Fe:<output>`, `/link /STACK:8388608` (`c-backend.md` §1.1), then the manifests' `/LIBPATH:<dir>` and `<name>.lib` (§4.1) |

A compiler is MSVC when its file name is `cl` or `cl.exe`. `--emit obj` (one relocatable object) needs clang or gcc.

**`--emit lib`** (ADR 0088 §2.1) writes a static library for a host program whose `main` is not CeeSafe's
(type-system.md §28.6): each C file of the build compiled with `-c` and `--emit obj`'s flags (`--cflag`s included),
then archived with `<archiver> rcs` into `<output>.partial`, made afresh, and renamed over the output, so a failing
archiver (`ceesafec: the archiver '<path>' failed`) leaves an earlier archive as it was. The members are one per C
file: the runtime's keep their names (`ceesafe_rt_core.o`, …); every other one is `<program>_` and the C file's path
without `.c`, each `_` written `__` and each directory separator `_s` (`main.c` gives `<program>_main.o`;
with `--split`, `c/embed/src/lib.c` gives `<program>_c_sembed_ssrc_slib.o`), so two paths never give one name; a
package's separately compiled C files are `<program>_ceesafe_package_<k>.o`. Two C files whose member names differ
only in case would be one file on macOS and Windows, so such a build fails (status 1) naming the member. The default
output is `<name>.a` (`out/<name>.a` in a package). Unlike `-r`, an archive keeps the host's LTO: with `--cflag
-flto=thin` (or `-flto`) every member is bitcode, which the host's link optimises with its own code (a ThinLTO host
that builds its own C with `-fno-split-lto-unit`, as Bun does, needs `--cflag -fno-split-lto-unit` too). One archive
per host binary: two archives built separately each carry the runtime and clash as two objects do (ADR 0088 §2.7).
- **The C `main`.** A package with a CeeSafe `main` gets no C `main` in its archive, since the host has its own (two
  would not link); `--lib-main` keeps it, for an archive linked into an executable alone.
- **The host's headers**, with `--emit lib` and `--emit obj`, are written beside the output: `ceesafe/embed.h` (its
  freestanding form in a freestanding build), and, when the program has exports, the exports header, named by
  `--exports-header <name>` (default `ceesafe_exports.h`). `-o build/libtext.a` gives `build/ceesafe/embed.h` and
  `build/ceesafe_exports.h`, so a host compiles with `-I build`.
- `--ar <archiver>` names the archiver, a path or a name looked up on `PATH`; by default it is `llvm-ar` beside an
  LLVM clang (`clang-18` beside `llvm-ar-18`; the C compiler found as for `--emit obj`, on `PATH` when it has no
  directory), else `ar` on `PATH` (Apple's `ar` indexes bitcode members).
- Usage errors (status 2): `--ar` or `--lib-main` without `--emit lib`; `--ld` with `--emit lib`, which archives
  instead of linking; `--emit lib` when no archiver is found, or `--ar`'s is not, naming `--ar`.

`--jobs N` (`build --split`, ADR 0082 §2.5) compiles at most `N` C files at once; the default is the number of CPUs
(`getconf _NPROCESSORS_ONLN`, or `NUMBER_OF_PROCESSORS` on Windows), and `--jobs 1` compiles them one at a time in the
bytewise order of their paths. The objects are linked in that order, and the C compiler's output is printed in it,
whatever finishes first, so neither the C nor the output depends on `N`; an output identical to an earlier file's
(a bad `--cflag`'s error, printed by every compile) is printed once, then `ceesafec: the C compiler printed the same
for <n> of the C files`. `--jobs` without `--split` is a usage error (status 2), since the C is then one command.
With `cl`, each file is compiled with `/c`, `/Fo:` its own object and, for `-g`, `/FS`, and the objects are linked by
`cl /Fe:`. On macOS a `-g` build's debug
information is gathered into `<output>.dSYM` by `dsymutil` after the link, as the compiler driver does when it compiles
and links in one command, since the objects are removed with the build directory. A build without `--split` is one
`main.c` and the runtime's files in one compiler command, as before.

### 8.4 The output

The executable is `out/<name>` in a package, or `-o <path>`. On Windows, a path without an extension gets `.exe`, so
`ceesafec build` makes `out\hello.exe`. The output's directory is created if it does not exist.

## 9. Porting C to CeeSafe: `c2cee`

CosmoOS's roadmap 2.0 moves kernel C into CeeSafe one file at a time (SA2, SA3), and
every port repeats the same mechanical steps: map the C types, declare each function the file calls, choose `@header`s
that do not include the port's own header back, find what must stay in a C floor, rename members that are keywords,
wire the build, and run the same checks. `integrations/cosmoos/c2cee.py` does the analysis and the checks, and drafts
the `.cee` file, the floor and the build lines (§9.4). Since 2026-10-31 CosmoOS's kernel language
is CeeSafe: a finished port lands in its tree as a `.cee` file, and its build generates the C with the ceesafec it pins
(its `scripts/cee-gen.py` and `build/cee.mk`; the C is not committed). It reads C through clang's own front end (`-Xclang -ast-dump=json`), so
it needs nothing but clang and Python 3, and never edits a file. It parses with the kernel's include path (CosmoOS's `KERNEL_CFLAGS`), the headers the build generates
from CeeSafe (`out/cee/kernel/include`, written by `make cee`) first.

### 9.1 `c2cee.py analyse <file.c> [--header <h>] [--cosmoos <tree>] [--arch x86_64|aarch64] [--json]`

Parses `<file.c>` twice, as a debug build and as a release one (`CONFIG_DEBUG=1`, `0`), with CosmoOS's include paths
for `--arch` (default x86_64), and reports:

- **Functions** the file defines: their C and CeeSafe signatures, and whether a module export (`EXPORT_SYMBOL`) names them.
- **Calls**, each callee with the header that declares it and what that means for the port:
  - a function: an `extern "C"` declaration;
  - `static inline` in a header: callable through that header (type-system §28.2);
  - an ordinary function in one build and `static inline` in the other (lockdep's hooks): it **must** be declared
    through its header, or the build where it is inline fails to link (CosmoOS #499);
  - variadic (`panic`, `klog`): a C floor;
  - through a function pointer: a C floor when the pointer is a member of an imported struct (§31.1).
- **Builtins**: `__builtin_return_address` (CeeSafe's `callerAddress()` in an export), `__atomic_*` (an atomic view of the
  field; a `bool` field needs a `u32`, since lib/core's `AtomicBool` is four bytes), `__atomic_thread_fence` (a floor:
  CeeSafe has no fence).
- **Macros** expanded in the file's functions, from their expansion locations: `wait_event*` (wait_floor.c's helpers),
  `list_for_each_entry*` (a loop over lib/kernel's `List` or `offsetOf`), `might_sleep` (`wait_floor_check_sleep`),
  `KASSERT`, `WARN`, `EXPORT_SYMBOL` (a floor).
- **Locals of struct type** that hold C state (a wait entry, a timer): a CeeSafe local cannot (§33.1), so a floor helper.
- With `--header <h>`, the **structs it defines**, field by field: the CeeSafe type, members whose names are CeeSafe keywords
  (renamed, or `@cname` on an imported struct), `const` pointer members (a `@repr(C)` struct's carry no `const`),
  fields used atomically, and every other file in the tree that names a member.
- **`@header` candidates**: the headers that declare what the file uses, each marked unusable when it includes `<h>`,
  directly or not, since the generated types' header is included from `<h>` (ADR 0030; `thread.h` → `completion.h` →
  `wait.h`).

The report is text, or JSON with `--json`. It exits 0 unless clang fails.

### 9.2 `c2cee.py verify [--cosmoos <tree>] [--arch x86_64|aarch64|both] [--no-boot] [--max-load <n>] [--ceesafec <path>]`

Runs a port's checks in a CosmoOS tree, in order, with `CEESAFEC` set to `--ceesafec` (default `$CEESAFEC`, else
`ceesafec` on `PATH`, else `~/.ceesafe/bin/ceesafec`: the release CosmoOS pins, which its Make runs on the tree's
CeeSafe sources), and prints one line per step and a summary. A tree without `scripts/cee-gen.py` (from before
CosmoOS's move to CeeSafe) is refused.
1. `make cee-check` (every CeeSafe source compiles with the pinned ceesafec, and the negative sources are refused as
   annotated);
2. `make host-test`, with every recorded trace digest it prints;
3. `make check-docs`;
4. per architecture: the debug and the release build, `make analyze` (and, for the first architecture, `make
   fuzz-build`, whose shims define some of a port's functions over its types), and unless `--no-boot` a debug and a release boot
   (`make test`). Before each boot it waits until the one-minute load average is under `--max-load` (default 8),
   since the boot tests' time bounds fail on a loaded host. A boot that fails after the load average reached the
   bound at any point during it (another job started meanwhile; it is sampled every five seconds) is run once more
   on a quiet host: both lines are printed, and the second
   decides.

It exits 1 if any step failed, naming the first.

### 9.3 Checks of the tool

`c2cee.py selftest` analyses `integrations/cosmoos/c2cee/fixture.c` with `fixture.h` and compares the report with
`fixture.expected.json`, and drafts its port (§9.4) and compares that with `fixture.draft.cee` and
`fixture_floor.draft.c`. The fixture holds one of each case of §9.1.

### 9.4 `c2cee.py port <file.c> --header <h> --out <dir> [--cosmoos <tree>] [--arch …]`

Writes a **draft** of the port into `<dir>`, never into the tree: `<name>.cee`, `<name>_floor.c` and `BUILD.md` (the
`ceesafec check` of the finished port, CosmoOS's `cee-gen.py`, `kernel.mk` and `host.mk` lines, where the `.cee` file goes
and the C it replaces, and what to change in
`<h>`). The draft is unsafe CeeSafe that mirrors the
C, statement by statement, for a person to finish and review; every place it could not translate, or translated with a
decision someone must check, carries `TODO(c2cee)` with the reason, and it never drops a construct silently.

- **Names** stay C's (functions, parameters, locals, statics), so the draft reads beside the original; a name that is a
  CeeSafe keyword gets `_` appended. Each function's body is one `unsafe` block, and its doc comment has a `Safety:` line
  marked `TODO(c2cee)`. A function `EXPORT_SYMBOL` names, or that the header declares, is `@export("name")`; one the
  file passes as a function pointer is `@callback`.
- **Types**: `<h>`'s structs become `@repr(C)` structs (a keyword member renamed, `fn` to `callback`, `class` to
  `lock_class`, otherwise with `_` appended, and listed in `BUILD.md`); every other struct the file uses is imported,
  with the fields it reads; typedefs of integers are their CeeSafe integer, of structs `@ctype` imports.
- **Declarations**: an `extern "C"` block per kind: callees whose headers may be `@header`s (§9.1), and the rest without
  a header; the floor's functions; `@constant` macros used in static initialisers.
- **Statics**: integer, `bool`, pointer and array statics, and those initialised by a C constant macro (`SPINLOCK_INIT`),
  are CeeSafe `static mut`s; a static of a C struct type, or one whose initialiser names the static itself
  (`WAITQUEUE_INIT(name)`), stays in the floor, and the draft reaches it through a floor function that returns its
  address.
- **Statements and expressions**: blocks, declarations, `if`, `while`, `for` (as a `while`, unless its body has a
  `continue`), `do` (as a `loop`), `return`, `break`, `continue`, assignments and compound assignments, `++`/`--` as
  statements, calls, `?:` (an `if` expression), casts (`as` when it cannot lose information, else `truncate`), pointer
  comparisons with null (`isNull()`), `*p` (`read`/`write`), `p->f` (`Ptr.toBorrow`, and for a write a `let` of
  `Ptr.toBorrowMut` first), `&x` (`borrow mut … as *mut T`), `sizeof` (`layoutOf`), string literals, the `__atomic`
  builtins on integers and pointers (an atomic view), `__builtin_return_address` (`callerAddress()`). Anything else,
  `switch`, `goto` and `continue` in a `for` among them, is a `TODO(c2cee)` with the C text.
- **Macros**: `KASSERT(c)` becomes `if (!(c))` and a floor call that panics with the condition's text; `might_sleep()`
  `wait_floor_check_sleep`; `wait_event(q, c)` and `wait_event_timeout(q, c, t)` `wait_floor_until(_timeout)` with the
  condition as a `@callback` over a context struct of the locals it reads; other macros are translated from their
  expansion.
- **The floor**: one C function per call of a variadic function (`panic`, `klog`, `kwarn`), with the format fixed and
  the other arguments as parameters; the statics that stay in C, with their accessors; file-scope `STATIC_ASSERT`s; the
  `EXPORT_SYMBOL` lines.
- **Builds**: a function that differs between the debug and the release build (an `#if CONFIG_DEBUG` inside it, or
  present in one build only) is drafted from the debug build with a `TODO(c2cee)` naming the difference: a draft from
  one build alone would change the other.

## 10. Tests: `ceesafec test` (ADR 0049)

### 10.1 Declaring tests

A test is a top-level function marked `@test` (type-system.md §28.1): no parameters, not generic, not exported,
`async` or not, returning `void` or `Result<void, E>`, where an `Err` is its failure. Any other shape, and a call of a
test from anything but the generated harness, is N0348.

- **Where tests are.** Beside the code in the package's sources, or in its test directories (`tests = ["tests"]` in
  `[package]`, by default `tests`). They are part of the same package and see its private items.
- **Build and check.** `check` checks the test directories too. `build` leaves them out, and emits no `@test`
  function. Only the root package's tests are run; a dependency's are not.
- **Checks** (lib/std's `testing.cee`): `Check.isTrue`, `isFalse`, `equalU64`, `equalI64`, `equalText`, `equalBytes`
  and `fail`, and `CheckResult<T, E>.isOk` and `isErr`. Each returns `Result<void, TestFailure>`, used with `?`; a
  failure holds its message. Generic equality waits for an equality interface.

### 10.2 The command

`ceesafec test [--filter TEXT] [--list] [--interp] [--jobs N] [--timeout SECONDS] [--sweep alloc|cancel]
[--format text|json]`, in a package:

1. It checks the package with its tests and writes the harness and the list of tests to `out/ceesafe-test/`.
2. It builds the harness once, as `out/<name>-tests`.
3. It runs each test whose name contains `--filter` (all by default), in the order of their names (bytewise), `--jobs`
   at a time (default 1), each as its own process. `--list` prints the names instead. A `--filter` that no name
   contains is an error (exit status 1), so a mistyped filter cannot pass. `ceesafec test --help` lists the options.

Each test's outcome:

| Outcome | When |
|---|---|
| passed | it returned `Ok`, or `void` |
| failed | it returned `Err`; its message is shown (`IoError`'s and `AsyncError`'s message, a `TestFailure`'s, or for the test's own enum the variant: `returned Err(Mine.Broke)`), and its captured output |
| panicked | the runtime's panic report, with its location |
| crashed | a signal or exception: `abort`, a fault (a status of 129 to 192 on POSIX, as `Process.capture` reports a signal; an exception's code on Windows) |
| exited | the test ended the process itself with another status (`Process.exit(3)`) |
| timed out | it ran past `--timeout` (default 60 s) |
| leaked | it passed, but allocations were live at the end (`CEESAFE_ALLOC_REPORT`) |
| skipped | with `--interp`, it calls C, which the interpreter cannot run |

- **Exit status.** 1 if any test is not passed or skipped, else 0.
- **JSON.** `--format json` prints one document: each test's name, outcome, milliseconds, message, standard output and
  error; the sweeps' failures and runs; and the counts.
- **`--interp`** runs each test with `ceesafec run --interp` on the harness: the virtual clock and the seeded run order
  of type-system.md §40.8. The timeout holds there too, and so does the leak check: the interpreter counts allocations as
  the runtime does. The sweeps are native only: `--interp --sweep` is a usage error (exit status 2).
- **`--sweep alloc`** runs each passing test again once per allocation attempt, the k-th failing. **`--sweep cancel`**
  runs it once per suspension point, the k-th cancelling its root task, sampled past 200 as `tools/cancel_sweep.py`
  is. A run may fail; it may not panic, crash, time out or leak.

### 10.3 The harness protocol (version 1)

The generated function `ceesafeTestMain` is the harness's entry. An executable package's own `main` is checked and
compiled but not run.

- **Listing.** `--ceesafe-list` prints `ceesafe-test 1` and then one name a line.
- **Running.** `--ceesafe-test <name> <fail-alloc-at> <cancel-at> <timeout-ms>` runs one test (0: none for the last
  three):
  - lib/std's `TestRun.begin` turns on the runtime's allocation and cancellation reports and the two points
    (`ceesafe_test_begin`), and arms a watchdog thread (`ceesafe_thread_watchdog`). The watchdog prints
    `ceesafe-test 1 timeout` and ends the process with status 124.
  - The harness then writes `ceesafe-test 1 passed`, or `ceesafe-test 1 failed`, the message and `ceesafe-test 1 end`,
    to standard error.
  - The runner reads these lines, the panic report, the allocation report (`ceesafe alloc: attempts=… live=…`), the
    cancellation report (`points=…`) and the exit status.
