# ADR 0120 — A package's include directories reach the generated C

- Status: **Accepted** by the owner, 2026-11-13 ("Fix those new gaps too"), implemented on branch `fix/gaps2-d`; not
  yet released. Raised by the book's authors (gaps2 D6): a package that binds C through `@header("clock.h")` named the
  header's directory in `include-dirs`, and every build still needed `--cflag -Ic`.
- Scope: the driver (`src/main.cee`, `collectNativeLinks`; `src/host/toolchain.cee`, the compile commands);
  `spec/tooling.md` §4.1 and §8.3; `guide/packages.md`. It amends ADR 0047 §3.3. No change to the manifest's syntax,
  the checker, the libraries, the runtime or the generated C.

## 1. Context

ADR 0047 §3.3 gave a package `include-dirs` for its own C files, which are compiled apart with them, and said: "The
generated C, and other packages' C, never see those directories." That was decided before `@header` bound C from a
header (type-system.md §31): the generated C now has `#include "meter.h"` for every `@header("meter.h")` of every
package in the build (§31.2), and nothing in a manifest could say where that header is. A library that wraps C could
not be built by its users without a `--cflag -I…` that each of them had to know, which is what `include-dirs` exists
to avoid for the library's own C.

## 2. Decision

The include directories of every package in the build are on the include path of the **generated C**:

- **Which.** Each package's `include-dirs` (with its `[target.OS]` table's), as the manifest gives them: absolute, or
  relative to the package.
- **Order.** After the runtime's own directory and after the `--cflag`s, so neither is shadowed; then the packages'
  directories, dependents first (the root package's first), as library directories are ordered (ADR 0047 §3.2); a
  directory named twice keeps its first place.
- **Where.** Every command that compiles the generated C: `build` (one command or `--split`), `--emit obj`,
  `--emit lib`, `run` and `test`. They are `-I<dir>` (`/I<dir>` with `cl`).
- **A package's own C files** are compiled apart with its own directories only, as before (ADR 0047 §3.3).
- **The C files of packages without `include-dirs`** are compiled in the generated C's command (in one command, or
  with its flags under `--split` and `--emit obj`), so they find these directories too. This is the part of ADR 0047's
  option B that comes with the change: a header in one package's directory can be found by such a file's `#include`
  of that name, if nothing earlier on its path has it (its own directory, copied beside it, comes first for a quoted
  include). A package whose C must not see other packages' headers names `include-dirs` of its own, and is then
  compiled apart.
- **`--emit c`** compiles nothing: whoever compiles its C passes the directories, which `ceesafe_link.txt` lists
  before each separately compiled C file as before.

Rejected: `-iquote`, which would keep the directories from `#include <…>` in other packages' C files, but also from
the headers' own `#include <lib/types.h>` of a neighbour, the usual form inside a library's headers; and a new
manifest field for the generated C's directories, a second way to say where a package's headers are.

## 3. Compatibility

A build that compiled before still compiles: directories are added after everything that was on the path, so an
`#include` that was found is found in the same place. A build that passed `--cflag -I<dir>` for a `@header` may drop
the flag when the package names the directory.

## 4. Tests

- `tools/package_tests.py`, check 33 (`tests/packages/header-include-dirs`): a library with a C file, a header in
  `native/include` named by `include-dirs` and `@header("meter.h")`; a program that depends on it builds in one
  command, with `--split`, as `--emit obj` and as `--emit lib`, and prints 15. Before the change the C compiler
  stopped at `#include "meter.h"`.
- The same check builds `--emit c -o c-out`: the link file is written into a directory named relative to the working
  directory (it was "cannot write the link file", the path having been made absolute from the root).
