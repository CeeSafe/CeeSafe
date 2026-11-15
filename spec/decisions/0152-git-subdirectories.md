# ADR 0152 — Packages in a repository's subdirectories: `dir` for git dependencies

- Status: **Accepted** by the owner, 2026-11-15. ClayWind's finding M4 (`~/dev/CeeSafeApps/Clay/CEESAFE-FINDINGS.md`
  §M): ClayWind's roadmap gate is a chat client in a repository of its own built on ClayWind's released packages
  only, and the owner said "Go ahead with step 8" (the gate). The design below was the lead's, chosen as defaults,
  and the questions it settles are listed in §5 so that the owner can revisit them. Implemented on branch
  `adr/0152-git-subdirectories`, released later.
- Scope: the manifest (`src/packages/manifest.cee`, tooling.md §4.1), resolution and the lock
  (`src/packages/resolve.cee`, `src/packages/workspace.cee`, tooling.md §4.2, §4.3), the registry's entries and
  publishing (`src/packages/registry.cee`, `src/packages/publishing.cee`, tooling.md §4.6–§4.8), the embed boundary
  of a package in the package cache (`src/host/embedding.cee`, tooling.md §4.1), the book's §7.4, the guide's
  packages page, `tools/package_tests.py`. No language, checker, backend or runtime change, and no new diagnostic.

## 1. Context

- **A git dependency is the package at its checkout's root** (tooling.md §4.2 before this ADR). ClayWind
  (`https://github.com/CeeSafe/Clay`) is one repository holding several packages in subdirectories (`claywind/`,
  `claywind-fonts/`, `claywind-tasks/`, and `claywind-desktop/` soon), each a `ceesafe.toml` whose dependencies on
  its siblings are `path = "../claywind"`. An application in another repository cannot depend on any of them by git.
- **A path dependency of a git package was a path.** Resolved from the checkout's directory in the package cache, it
  was locked as `path+` and the path from the root package to the cache (`path+../../.ceesafe/git/checkouts/…`). So
  an application that named `claywind` by git, and `claywind_tasks`, whose `claywind` is `path = "../claywind"`,
  would reach one package through two sources (N0326), even at one commit. And a path that left the checkout reached
  whatever lay beside it in the cache, or anywhere on the disk: a lock that depends on one machine's directories.
- **Other package managers.** Cargo resolves a git dependency by package name among the workspace members the
  repository holds; npm and Go name a subdirectory in the URL or module path. CeeSafe's dependency key is already
  the package's name (§4.1), but a name search would read every manifest of a repository and make the result depend
  on which directories exist; a named directory says where the package is.

## 2. Decision

### 2.1 The field

```toml
[dependencies]
claywind = { git = "https://github.com/CeeSafe/Clay.git", tag = "v0.1.0", dir = "claywind" }
claywind_desktop = { git = "https://github.com/CeeSafe/Clay.git", tag = "v0.1.0", dir = "claywind-desktop" }
```

- **`dir`** is a directory inside the repository's checkout, relative to its root, that holds the package's
  `ceesafe.toml`. It goes with `git` and any one of `rev`, `tag` and `branch`; without it the package is at the
  checkout's root, as before. On a path or registry dependency it is N0321, like any other field they do not take.
- **Its spelling, checked when the manifest is read** (N0321 at the value): relative, with `/` separators, and
  nothing in it is expanded, as an `[embed]` root (§4.1): an empty string, an absolute path, a drive, a `\`, `:`, `$`
  or `%`, a NUL and a leading `~` are refused, and so is a `..` that would leave the checkout (`../x`, `a/../..`).
  `.` and empty parts are dropped and `a/../b` is `b`: the directory is recorded in that normal form, and `.` is the
  checkout's root.
- **Where it leads, checked when the package is loaded** (N0322 at the dependency): the directory must hold a
  `ceesafe.toml`, and its real path must lie inside the checkout's, so a symbolic link in the repository cannot lead
  the package out of it.

### 2.2 Identity: repository, commit, directory

- **A git package's source is its repository, its commit and its directory.** Two dependencies on one name are one
  package when the three are the same, whatever `rev`, `tag` or `branch` each wrote; otherwise N0326, which names
  both, each with its directory: `'claywind' is reached through two sources: git+https://…/Clay.git at
  3f2a9c0d1b2e, directory 'claywind' (from 'app') and git+https://…/Clay.git at 9e8d7c6b5a41, directory 'claywind'
  (from 'claywind_tasks')`.
- **A path dependency of a git package is a directory of the same checkout.** It is resolved to the same repository
  and commit, and to the directory of the dependent's joined with the path, in normal form. So ClayWind's
  `claywind_tasks` (directory `claywind-tasks`) naming `claywind = { path = "../claywind" }`, and the application's
  own `claywind = { git = …, tag = "v0.1.0", dir = "claywind" }`, are one package when both tags name one commit.
  The rule applies again to the path dependencies of such a package, at any depth.
- **A path that leaves the checkout** (`..` past its root, an absolute path), from a git package, is N0322 at the
  dependency: `'outer': the path '../../outside' of 'evilout' leads out of its checkout of <repository>: a git
  package's path dependencies are directories of the same checkout`. Before this ADR it was resolved on the disk;
  no published package can rely on that, and the lock it gave held one machine's directories.
- **One fetch of a repository per command.** `ceesafec fetch` brings each repository's clone up to date once, so
  the same tag or branch written by two dependencies names one commit, and a repository is fetched once however many
  of its packages the build uses.

### 2.3 The lock and the cache

- **The lock records the directory** (§4.3): `dir = "claywind"` in the entry, after `commit`, for a git or registry
  package not at its checkout's root; an entry without `dir` is at the root, so every earlier lock reads as it did.
  A path dependency inside a checkout is locked as the git package it is (`source = "git+…"`, its `commit`, `dir`
  and `hash`). Every command other than `fetch` checks a git dependency's `dir` against the lock's (N0324 when it
  differs), as it checks the repository and `rev`.
- **The hash** (§4.3) is of the package's manifest and sources, with paths relative to the package's directory, as
  before: a package in a subdirectory is hashed as if it were at a root.
- **The cache's layout is unchanged.** A checkout is per repository and commit,
  `$CEESAFE_HOME/git/checkouts/<repository>/<commit>`; the packages of its directories share it. `--offline` uses it
  as before, and `fetch --update` changes nothing here (it concerns registry versions).
- **Interface records** (ADR 0082, §4.4.1) need no change: a record's key holds the package's name and its manifest
  and sources, and one build has one package per name, so two packages of one checkout never share a key; one
  package at two directories with the same files is the same record, which is right, since a record is a cache of
  what those files check to.
- **The language server**, given a file of a package in a checkout's directory (an editor that went to a dependency's
  definition opens it there), checks it with that package, found by the nearest `ceesafe.toml` as for any document
  (§2.1 of tooling.md), and bounds its embedding by the whole checkout (§2.4). (Its own definitions are answered
  within the open document, so nothing else of it changes.)

### 2.4 Embedding

The rule of §4.1 (ADR 0087 §2.8) is kept, with "the checkout" meaning the whole checkout of the commit, not the
package's directory: a git or registry package in a subdirectory, and every package reached through it, may embed
from its own directory and from its `[embed] roots` only where their real paths lie inside that checkout. So a
package may embed files a sibling directory of its repository holds (a shared `assets/`), and nothing outside the
repository. A root package whose directory lies in the package cache is kept inside the checkout that holds it, when
it lies in one (`$CEESAFE_HOME/git/checkouts/<repository>/<commit>`), and inside its own directory otherwise: the
language server or `check` run there gives it no more than the build that fetched it.

### 2.5 The registry

An index entry (§4.6) may carry `dir` beside `git` and `commit`, with the same spelling and meaning, and the lock
records it for the chosen version. `ceesafec publish` in a package that is not at its repository's root writes it
(the package's directory from the repository's top, `git rev-parse --show-prefix`), and `ceesafec index-check` reads
the manifest and computes the hash there. An entry whose `dir` is not valid is N0328, as any other invalid field of an
entry. Before this ADR, `publish` in a subdirectory printed an entry whose hash was its package's but whose commit's
root held another manifest, which `index-check` then refused.

## 3. Alternatives considered

- **Resolve by name** (Cargo's way): look for a manifest named like the key anywhere in the checkout. It reads every
  directory of a repository, a renamed or duplicated directory changes the result, and the lock would still need the
  directory. A named directory costs one field and says what it means.
- **A URL suffix** (`git = "https://…/Clay.git#claywind"`, npm's and Go's way): mixes two things in one string that
  git itself does not understand, and a `#` is part of some URLs. A field of its own is checked on its own.
- **`path` beside `git`** (`{ git = …, path = "claywind" }`): `path` already means a directory on the disk relative to
  the manifest; giving it a second meaning beside `git` would make `{ path = "x" }` and `{ git = …, path = "x" }`
  read alike and mean different things. `subdir` and `directory` were the other names: `dir` is short and plain.
- **A path dependency of a git package as a path** (the earlier rule): N0326 between it and the same package named
  by git, and a lock that holds the cache's directories. Rejected: the package is the same commit's file.
- **Bound embedding by the package's directory**, not the checkout: a package in a subdirectory could not embed
  shared files of its own repository, which the root-level package could. The checkout is the unit that was fetched,
  locked and hashed together, and nothing outside it is reachable either way.
- **Treat a path out of a git package's checkout as before** (resolved on the disk): kept no meaning that a published
  package can rely on; refused instead (N0322).

## 4. Consequences

- ClayWind's applications write `{ git, tag, dir }` per package, and its packages keep their sibling `path`
  dependencies: a released ClayWind is one tag of one repository.
- A git package with a path dependency that leaves its checkout no longer resolves (N0322). Such a package could
  only be used where the disk had the same layout as its author's.
- Locks of builds whose git packages have path dependencies change at the next `ceesafec fetch`: those packages are
  locked as `git+` with `dir` instead of `path+` into the cache. A lock written before this ADR with such a `path+`
  entry is N0324 to every command but `fetch` (run `ceesafec fetch`).
- A compiler older than this one reports `dir` as an unknown field (N0321): a package that uses it should say so with
  `ceesafec = ">=…"` (ADR 0076), the release that brings it.

## 5. The questions this ADR settles (the owner may revisit them)

1. **The name:** `dir` (not `subdir`, `directory`, `path` or a URL suffix), with any of `rev`, `tag` and `branch`.
2. **Spelling errors** (absolute, drive, `\`, `:`, `$`, `%`, NUL, leading `~`, empty, `..` out of the checkout) are
   N0321 when the manifest is read; a directory without a `ceesafe.toml`, or whose real path leaves the checkout
   through a symbolic link, is N0322 when the package is loaded.
3. **Identity** is (repository, commit, normal directory); the written `rev`/`tag`/`branch` is not part of it.
4. **A path dependency of a git package** is the same repository and commit at the joined directory, at any depth;
   one that leaves the checkout is N0322 (before: resolved on the disk).
5. **One fetch per repository per command**, so one tag spelled twice is one commit.
6. **The lock** writes `dir` after `commit`, only when not at the root; a missing `dir` is the root.
7. **Embedding** is bounded by the whole checkout, for a package in a subdirectory and what it reaches, and for a
   root package that lies in a checkout of the package cache.
8. **The registry** carries `dir` too, written by `publish` and checked by `index-check`.
9. **Two directories under one name** (two dirs, or one dir at two commits) are N0326, naming both with their
   directories.
10. **Directories are compared as written, in normal form**: two spellings that a symbolic link inside the checkout
    makes one directory are two sources.

## 6. Not in this ADR

- Finding a package in a repository by its name (a workspace's member list).
- Sparse or partial checkouts: the whole commit is checked out once, as before.
- A path dependency of a registry package (still N0328: a published package's dependencies are registry ones).

## 7. Implementation and evidence

See ROADMAP §10's row for the tests run.
