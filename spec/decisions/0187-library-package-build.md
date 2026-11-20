# ADR 0187 — `build` in a library package makes a static library

- Status: **Accepted** by the owner, 2026-11-20 (asked how to fix CeeUsb's finding F13: "Default to --emit lib").
- Scope: tooling.md §4.1 (the manifest's `kind`) and §8 (`--emit lib`). Finding F13 of CeeUsb.

## 1. Context

`ceesafec build` with no `--emit` makes an executable. In a package whose manifest says `kind = "library"` that
fails at once: a library has no `main`, so the build stops with N0300 ("an executable needs a 'main' function"), at
the first source file, which has nothing to do with it. CeeUsb met it on every system. A package of kind `object`
already builds what its kind says, `--emit obj`, when no `--emit` is given.

## 2. Decision

- In a package of kind `library`, `ceesafec build` without `--emit` is `--emit lib` (ADR 0088): the static library
  `out/<name>.a` and the host's headers beside it. Every option of `--emit lib` applies (`--ar`, `--lib-main`,
  `--exports-header`, `-o`).
- An `--emit` given on the command line is kept: `--emit exe` in a library package without `main` is still N0300,
  and `--emit c` and `--emit obj` are unchanged.
- Nothing else reads the kind for this: `test`, `check` and `run --interp` are unchanged, and a dependency of kind
  `library` is built into its dependant as before.

## 3. Options not taken

- **Keep the executable and improve N0300** (name the manifest's kind, suggest `--emit lib`): the command would still
  fail where the manifest already says what to make.
- **Check only** (`build` in a library checks and writes nothing): a library's C is useful to a host, which is what
  `--emit lib` is for.

## 4. Implementation and tests

- src/main.cee: the default `--emit` for the root package's kind, beside kind `object`'s.
- `tools/package_tests.py` check 44, `library_build_default`: a library package's `build` writes `out/<name>.a`
  and `ceesafe/embed.h`, exits 0, and reports no N0300; `--emit exe` there is still N0300.
