# ADR 0047 — System libraries in the manifest

- Status: **Accepted** by the owner, 2026-11-02 ("Yes, I accept both ADRs with your recommendations"): every
  question of §5 as recommended. Asked for by the owner for CeeSafe.Web
  (`~/dev/CeeSafe.Web/docs/ceesafe-upstream-work.md`, item U8): CeeSafe.Web links the system's OpenSSL for TLS (its
  M4).
- Scope: `ceesafe.toml` (`src/packages/manifest.cee`, tooling.md §4.1), the build's compile and link commands
  (`src/host/build.cee`, `src/host/toolchain.cee`, `src/host/compilers.cee`), `--emit c` and `--emit obj`.

## 1. Context

A package can list C files (`c = ["src/shim.c"]`), and every package's are compiled into the program. But nothing in a
manifest can name a library to link, or where its headers and archives are. Only `--cflag` on the command line can,
and a dependency cannot pass that on to the programs that use it. A package that wraps OpenSSL therefore cannot be used
by a plain `ceesafec build`.

Three facts constrain the design:

- **Nothing runs at build time** (ADR 0013): no pkg-config and no build scripts. Whatever the manifest names is
  literal.
- **One compile command.** The build compiles every C file, generated and listed, in one invocation of the C compiler
  with one set of flags (`compileExecutable`); `--emit obj` compiles each file with `-c`, then joins them with `-r`.
- **The package hash already covers the manifest's text** (`packageHash`, tooling.md §4.3), so any field added there
  changes the hash with it.

## 2. Options

**Where a package's include directories apply.**

| | A. To its own C files (proposed) | B. To every C file of the build |
|---|---|---|
| What | a package with `include-dirs` has its C files compiled apart (`-c`, with its directories), and the objects go into the build's command | the directories are added to the one command |
| Cost | one more compiler run per such package | a header in one package's directory can shadow another package's, or the runtime's, of the same name |

**A is proposed**: it is what the request asks for, and it keeps one package's headers from changing another's C. A
package without `include-dirs` is compiled as today.

## 3. Proposed decision

### 3.1 The fields

```toml
[package]
links = ["ssl", "crypto"]                 # libraries to link: -lssl -lcrypto; ssl.lib crypto.lib with cl
lib-dirs = ["/opt/homebrew/opt/openssl@3/lib"]
include-dirs = ["$OPENSSL_DIR/include"]   # for this package's own C files

[target.windows]                          # also [target.macos] and [target.linux]
links = ["libssl", "libcrypto"]
lib-dirs = ["C:/vcpkg/installed/x64-windows/lib"]
include-dirs = ["C:/vcpkg/installed/x64-windows/include"]
```

- **Values.**
  - Each field is an array of strings.
  - A name in `links` is letters, digits, `_`, `.`, `+` and `-`, and does not start with `-`. So no entry can become a
    compiler flag.
  - A directory is absolute, or relative to the package's directory. It does not start with `-`.
- **Targets.** A `[target.OS]` table's fields are added after `[package]`'s when the build is for that system (`linux`,
  `macos`, `windows`; the host's, as for every hosted build). Other names are rejected.
- **Environment variables.** A directory may start with `$NAME` or `${NAME}`, which is replaced by that variable's value
  when ceesafec reads the manifest. An unset variable is an error naming it (N0321). This reads the environment and
  runs nothing (ADR 0013). The hash is of the manifest's text, so the variable's value is not part of it.
- **Errors.** A bad value, an unknown field in a target table, or an unknown target is N0321, as for every manifest
  field.

### 3.2 The link line

- `links` and `lib-dirs` from every package in the build reach the executable's link, whatever its kind: a library's
  are its users'.
- **Order.** Packages are taken dependents before their dependencies, which is what static archives need. In a
  package, `[package]`'s entries come before its target's.
- **Duplicates.** A library named twice keeps its last place, so it follows everything that uses it. A directory named
  twice keeps its first.
- **Flags.**
  - GCC and clang: `-L dir` for each directory, `-l name` for each library, after the C files.
  - MSVC (`cl`): `/I dir` for headers; after `/link`, `/LIBPATH:dir` and `name.lib`.
  - clang on Windows: GCC's flags, which its driver maps to `name.lib`.

### 3.3 Include directories

A package with `include-dirs` has its own C files compiled apart:
- with `-c` (or `cl /c`), the build's flags and its directories (`-I`, `/I`), into the build's work directory;
- the objects then go into the build's command in place of those C files.

The generated C, and other packages' C, never see those directories. (Amended by ADR 0120: the generated C, which
includes every package's `@header`s, has them on its include path, and so have the C files compiled with it.)

### 3.4 Building by hand

- `--emit c` writes the link line next to the C: `ceesafe_link.txt` in the output directory. It holds one argument per
  line, in the C compiler's spelling (GCC/clang's unless `--cc` names `cl`), and it includes the include directories
  each package's C files need.
- `--emit obj` writes the same as `<output>.link.txt` beside the object. An object's libraries are linked by whoever
  links the object.

## 4. Consequences

- CeeSafe.Web's TLS package names OpenSSL once, and every program that depends on it links it.
- **A dependency can now add libraries and library directories to your link.** That trusts it as far as its C files
  already are: a dependency's C runs in your program anyway.
- New diagnostics: none. Bad fields are N0321.

## 5. Questions for the owner

1. The fields `links`, `lib-dirs` and `include-dirs`, in `[package]` and in `[target.linux|macos|windows]` tables added
   after it? Recommended: yes.
2. A package's `include-dirs` apply only to its own C files, compiled apart (option A)? Recommended: yes.
3. Directories may start with `$NAME` or `${NAME}`, read from the environment, with an unset variable an error?
   Recommended: yes.
4. Link order dependents first, a repeated library keeping its last place? Recommended: yes.
5. `--emit c` writes `ceesafe_link.txt`, and `--emit obj` writes `<output>.link.txt`? Recommended: yes.

## 6. Implementation plan (once accepted)

1. Spec: tooling.md §4.1 (the fields), §4.3 (the hash already covers them), §6 and §8 (the link line, the files for
   building by hand).
2. `src/packages/manifest.cee`: the fields and target tables, their checks, the variables.
3. `src/main.cee` and `src/host/`:
   - the workspace's link list in dependency order;
   - the separate compile of packages with `include-dirs`;
   - the flags for GCC/clang and for `cl`;
   - the link files.
4. Tests:
   - a package test that links a small C static library, built by the test, from `lib-dirs`, with its header from
     `include-dirs`;
   - the same through a two-level dependency, where the library's user is the dependency's dependency;
   - an include directory whose header shadows a name the root package also uses, so it must stay its package's;
   - N0321 for each bad value, an unknown target, and an unset variable;
   - the lock: the package hash changes when `links` changes;
   - `--emit c`'s link file;
   - Windows with `cl` and with clang, on the Windows 11 ARM64 test host.
5. The guide (`guide/packages.md`, C libraries), ROADMAP §10.

## 7. Implementation notes (2026-11-02)

- **Link files.** They are written only when the manifests name a library, a library directory or an include
  directory, so existing `--emit c` outputs do not change.
- **Separate C files.** A package with `include-dirs` has its C files compiled from where they are, not copied into
  the build's directory as other C files are. So a `#include "local.h"` beside the file also works.
- **The hash.** A path dependency has no hash (tooling.md §4.3). `tools/package_tests.py` checks the hash through a git
  dependency instead: its locked hash changes with `links`.
- **Windows file names.** `addCFile` took a C file's name after the last `/` only, so a Windows path (`C:\…\x.c`)
  became its whole name, and writing it into the build directory failed ("cannot write the C: Invalid argument"; the
  ROADMAP's open Windows item). It now also stops at `\`.
