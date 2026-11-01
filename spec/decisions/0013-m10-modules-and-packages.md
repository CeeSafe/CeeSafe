# ADR 0013 — M10 T5–T6: modules and packages

> **Inherited.** ADR 0013 was accepted before CeeSafe's fork (Decision 0006), when the language had its
> earlier name. In this record, "the reference" is that language and project and "the reference compiler" its compiler:
> read them as CeeSafe and `ceesafec`. Links to documents CeeSafe did not keep are given as text.

- Status: **Accepted** (project owner, 2026-10-26). The owner took every recommendation of §4: package-level names
  (option A) with `namespace` retired, `src/` as the default sources, git `rev`, `tag` or `branch` pinned in the lock,
  and dependencies' C names prefixed with their package's name. The wording is the agent's; the owner may revise it.
  Not yet implemented: the decisions go into `language-spec.md` §5, `type-system.md` §1 (scopes), `grammar.md`,
  `c-backend.md` §2.1 and `tooling.md` (a new §4) before any code.
- Scope: M10 T5 (modules) and T6 (packages and building), `milestone-10.md`. It settles OD-11 (`open-decisions.md`:
  "`ceesafe.toml` with name, version, profile, dependencies (path-only in v0)"). The owner has already chosen git
  dependencies as well as path ones, with a lock file (2026-10-26).

## 1. Context

- **One compilation is one package.** All its files share one item scope; names are simple. `namespace X;` is parsed
  and recorded but qualifies nothing, and two items of the same name are N0201 even in different namespaces
  (`type-system.md` §1). `import` is a reserved keyword, but the grammar has no import declaration.
- **Visibility today:** `public`, `internal` (the default) and `private` are parsed. Only `private` on struct fields
  has an effect, and it means visible within the file (§12, C7c).
- **`lib/core`** is part of every package: its items are in scope unqualified, with no import (§29.2).
- **The compiler is whole-program.** Generics are monomorphised (§20), and the ownership analyses, placement and the
  optimisations (`optimisations.md`) run over the whole NIR. The C backend writes one `main.c` per program, with
  names such as `ceeF_area` and `ceeT_Point` (`c-backend.md` §2.1), unique only within one package.
- **Programs that would use packages:** CosmoOS integration code that today shares bindings by copying files
  (`integrations/cosmoos/c5/kernel.cee` into c7 and c8), the benchmark programs, `nwc`, and code outside this
  repository (ADR 0008: the reference also serves Linux and other C codebases, not only CosmoOS).
- **The kernel profile** needs nothing to run at build time that is not CeeSafe source or C, and builds must be
  reproducible.

## 2. Decisions proposed

### D1. A package is a directory with a manifest

`ceesafe.toml` at the package's root; its sources are every `.cee` file under the directory `src/` (or the
directories the manifest's `sources` lists), in ordinal path order. The manifest is a small subset of TOML: tables,
strings, booleans, arrays of strings and inline tables. `ceesafec` parses it itself, with no new dependency.

```toml
[package]
name = "geometry"            # an identifier: what dependents call the package
version = "0.3.1"            # recorded; not used to choose versions (D6)
kind = "library"             # "executable" (has main), "library", or "object" (one .o, profiles.md §4.3)
profile = "hosted"           # or "freestanding"
float = true                 # false is --no-float
c = ["src/shim.c"]           # C sources built with it (type-system.md §28.6)

[dependencies]
shapes = { path = "../shapes" }
lz4 = { git = "https://example.org/lz4-cee.git", rev = "3f2a9c0d…" }    # a full commit id
util = { git = "https://example.org/util.git", tag = "v1.2.0" }          # resolved to a commit in ceesafe.lock
```

### D2. Names: one scope per package, qualified across packages

Recommended (option **A** in §3):
- Within a package nothing changes: its files share one item scope, and names are simple.
- A dependency's **public** items are reached through the dependency's name as the manifest writes it:
  `geometry.Point`, `geometry.area(p)`, as types and in expressions.
- `import geometry;` brings every public name of `geometry` into the file's scope; `import geometry.Point;` brings
  one. An imported name never hides a name of the package itself or of `lib/core`. Two imports giving the same name
  are an error where the name is used, not where it is imported.
- `namespace` declarations are retired: N0106 for any `namespace` after a grace period in which it is accepted and
  ignored. (Alternatively, kept as documentation with no effect.)
- `lib/core` stays unqualified, in every package.

### D3. Visibility between packages

- Only `public` items, and a public struct's `public` fields and methods, are visible to dependents. `internal`, the
  default, stays within the package; `private` stays within the file (fields: §12).
- A public item's signature must not name a type its dependents cannot see (a public function returning an internal
  struct): a new error. Implementations of an interface (`impl I for T`) are visible wherever both `I` and `T` are.

### D4. Building stays whole-program

`ceesafec build` (no file arguments, in a package directory) checks and compiles the package with all its
dependencies, as one program, as today. Generated C names of items from a package other than the one being built
gain the package's name (`ceeF_geometry_area`, `ceeT_geometry_Point`), so two packages may declare the same name. No
separate compilation and no cache in this ADR: they would need generic code checked once per package and a stable
interface format, and would be decided after measuring build times (the M10 plan's "cache of generated C per
package" is deferred until then).

### D5. Sources: path and git dependencies

- `path` is relative to the manifest. `git` takes `rev` (a full commit id), or `tag` or `branch`, which `ceesafec fetch`
  resolves to a commit and records.
- Git sources are fetched with the `git` executable into a cache (`$CEESAFE_HOME/git`, default `the reference's install directory/git`), one
  checkout per commit.
- **Nothing runs at fetch or build time** but `git` and the C compiler: a package has no build scripts.

### D6. The lock file

- `ceesafe.lock`, written by `ceesafec fetch` and committed with the package. Each dependency, transitively: its name,
  source, commit for git, and a SHA-256 of its manifest and sources.
- `ceesafec build` uses the lock and fails if a package's sources no longer match their hash, or if `ceesafe.toml` names a
  dependency the lock lacks (run `ceesafec fetch`). `--offline` never runs `git`.
- **No version solving.** Two packages that depend on a package of the same name must agree on its source and commit;
  otherwise it is an error that names both. `version` is recorded but not compared. *Amended by
  [ADR 0017](0017-m13-package-registry.md) (2026-10-28): registry dependencies are version requirements solved
  against an index; path and git dependencies keep this rule.*

### D7. Tools follow the package

`check`, `build`, `run`, `doc` and `fmt` without file arguments act on the package of the current directory. The
language server checks a document with its package when there is one (`tooling.md` §2.1). `ceesafec new <name>`
writes a manifest and `src/main.cee`.

## 3. Options for names (D2)

| | A. Package-level names (proposed) | B. Hierarchical namespaces | C. File modules |
|---|---|---|---|
| Inside a package | one scope, as today | `namespace X.Y;` qualifies every item | each file its own module |
| Across packages | `geometry.Point`, `import geometry;` | `import geometry.shapes;` | `import geometry/shapes;` |
| Existing code | unchanged | every file needs a namespace or moves | every multi-file package needs imports |
| Compiler work | qualified names through dependencies, imports, visibility | the above, plus namespaces within a package | the above, plus per-file scopes |

A keeps everything written so far valid, and qualifies only what crosses a package boundary, where names can collide.
B and C give finer structure inside a large package; nothing in this repository needs it yet, and A does not prevent
adding namespaces later.

## 4. Questions for the owner

1. Names: A (recommended), B or C (§3)? And under A, retire `namespace` (recommended) or keep it as a comment?
2. Sources: `src/` by default (recommended), or every `.cee` file under the package directory?
3. Git references: `rev`, `tag` and `branch`, each resolved to a commit in the lock (recommended), or `rev` only?
4. C names: prefix items of dependencies with the package name (recommended, D4)?
5. `ceesafec new`: wanted now, or later?

## 5. Consequences

- New: a manifest parser, a dependency resolver and fetcher (`git`), the lock file, qualified names and imports in
  the type checker, visibility errors, package-prefixed C names, and package-aware commands and language server.
- New diagnostics (codes assigned when specified): manifest errors, an unknown dependency, an unresolved or
  ambiguous imported name, a non-public item used from another package, a public signature naming a non-public
  type, a hash mismatch, a dependency conflict.
- Tests: packages in `tests/packages/` (path dependencies, a diamond, a conflict, visibility, imports); git sources
  from a local bare repository made by the test, so the suite needs no network.
- Risks: R-12 (new) for dependencies: a fetched package is code the build trusts. Mitigated by commits pinned in the
  lock, verified hashes, and no build scripts; not by review, which stays the user's.
