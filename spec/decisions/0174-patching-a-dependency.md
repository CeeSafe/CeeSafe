# ADR 0174 — Patching a dependency: one source for a package, chosen by the build's root

- Status: **Accepted** by the owner, 2026-11-19 (asked how IPos should depend on both a local ClayWind and
  CeeSafeReporting, which pins ClayWind by git: "Add [patch] to CeeSafe"). Implemented the same day (§5); released
  in ceesafec 0.74.0.
- Scope: the manifest (`[patch]`), the resolver (`src/packages/manifest.cee`, `src/packages/resolve.cee`),
  tooling.md §4.1, §4.2, the diagnostics table, the guide's packages page. No change to the language.

## 1. Context

- **One source per package** (tooling.md §4.2). A package name reached through two sources (two paths, a path and
  a commit, two commits) is N0326. That keeps a build from holding two copies of a library whose types meet.
- **What it stops.** An application developed beside its libraries names them by path (IPos: `claywind = { path
  = "../../Clay/claywind" }`, and claywind_desktop, which names claywind by path in turn). A library it also uses
  that is published names the same library by git (CeeSafeReporting: `claywind = { git = "…", rev = "…" }`), as it
  must: a path to a sibling checkout builds for nobody else. The application cannot have both: N0326, and no way
  to say "my checkout is the one".
- **The ways round it, and what they cost.** The application pins every library by git too (each change in the
  library a push and a bump before the application sees it); or the published library names a path (it no longer
  builds anywhere else). Both make the build's owner unable to choose a source the graph already names.

## 2. Decision

- **`[patch]`** in a manifest: entries written as `[dependencies]` entries are, each naming a package and a source,
  a `path` or a `git` repository (with `rev`, `tag` or `branch`, and `dir`).

  ```toml
  [patch]
  claywind = { path = "../../Clay/claywind" }
  ```

- **Only the root's.** The manifest of the package being built is the only one whose `[patch]` is read: a library
  cannot change its users' sources. A dependency's `[patch]` is parsed (so its errors are its own) and not applied.
- **Everywhere.** Each dependency named like a patch, wherever in the graph it is reached (the root's own, a path
  package's, a git or registry package's), takes the patch's source instead of its own, before anything else is
  done with it. The rest of the dependency (a version requirement, a path, a `dir`) is not read.
- **As the root's own dependency.** A patched package is resolved as if the root had named it: a patch's `path` is
  relative to the root's directory; the package is code the user works on (it may embed from its own `[embed]
  roots`, tooling.md §4.1, as a path dependency of the root may); its errors are reported at the patch.
- **The lock** records the patched source, as it records any other; `check` and `build` without `fetch` find it
  there, and a changed or removed patch is the usual N0324 until `fetch`.
- **Mistakes.** A patch with no source (a version requirement: the registry's choice is not a patch) is N0321 at
  its key; a package patched twice is N0321; a patch that names no package of the build (a name misspelt, a
  dependency since removed) is N0321 at the patch, after resolution.

## 3. Options not taken

- **A patch in any manifest.** Two libraries could patch one package two ways, and a library's patch would reach its
  users' builds unasked. The root owns the build; it alone chooses.
- **Replacing only a source that matches** (patching `git+…` to a path, as some package managers key their
  overrides by the source replaced). A package is one package by its name already (N0326); keying by name is what
  the owner writes and reads.
- **A warning for an unused patch.** Package reports have no warnings; an unused patch is far more often a typo than
  a choice, and costs one line to remove.

## 4. Consequences

- IPos names ClayWind by path in `[patch]`, and CeeSafeReporting keeps its git pin: both build, IPos with one
  ClayWind, its own checkout.
- A published package is unchanged by this ADR: it may carry a `[patch]` for its own development; its users do not
  see it.

## 5. Implementation (2026-11-19)

- `src/packages/manifest.cee`: `[patch]` is a manifest table; its entries are read by `readDependency` and kept in
  `Manifest.patches`, a version requirement or a second entry for a name N0321.
- `src/packages/resolve.cee`: `drain` replaces a dependency named in the root's `patches` by the patch and resolves it
  as the root's (`dependent = none()`: the root's directory for a path, the root's file for errors, the empty
  boundary); `resolveAll` reports a patch no package of the build has, N0321.
- Test: `tools/package_tests.py` 42, dependency-patch: N0326 without the patch; with it one package, from the patch,
  in the lock, `check` clean, the patched values printed in the interpreter and compiled; an unused patch and a
  patch with a version requirement N0321; a dependency's own `[patch]` not read. All 39 checks pass on macOS
  (2026-11-19, `python3 tools/package_tests.py --compiler out/ceesafec`).
