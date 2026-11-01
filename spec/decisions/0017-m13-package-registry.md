# ADR 0017 — M13 G6: a package registry and version solving

> **Inherited.** ADR 0017 was accepted before CeeSafe's fork (Decision 0006), when the language had its
> earlier name. In this record, "the reference" is that language and project and "the reference compiler" its compiler:
> read them as CeeSafe and `ceesafec`. Links to documents CeeSafe did not keep are given as text.

- Status: **Accepted** (project owner, 2026-10-28, taking every recommendation in §5; OD-26 records the deferred
  option). The wording is the agent's; the owner may revise it. Not yet implemented: the decisions go into
  `tooling.md` §4 and `diagnostics.md`, with an amendment to ADR 0013 D6, before any code.
- Scope: M13 G6 (`milestone-13.md`): finding packages by name and version, and choosing compatible versions, on top
  of ADR 0013's path and git dependencies, lock file and hashes.

## 1. Context

- **Today** (ADR 0013, `tooling.md` §4): a dependency is a path or a git repository pinned to a commit by `ceesafe.lock`
  with a SHA-256 of its sources. `version` in `ceesafe.toml` "is recorded but not compared" (D6). Two packages that need
  the same dependency must name the same source and commit, or it is N0326. There is no way to find a package by name,
  and no way to say "any 1.x".
- **What G6 must not lose:** reproducible builds (the lock and its hashes), `--offline`, nothing executed at build
  time except `git` (D5), and the kernel profile's rule that a build needs only CeeSafe source and C.
- **Hosting.** A registry needs somewhere to live. The reference has a GitHub organisation and a documentation site on GitHub
  Pages. It has no server, and the project should not have to run one.

## 2. Options

| | A. A git index (proposed) | B. A registry server with an HTTP API | C. No registry: a curated list in the docs |
|---|---|---|---|
| What it is | a git repository (`ceesafe-lang/index`) with one small TOML file per package, listing each version's git URL, commit and source hash | a service storing package archives, with publish and download endpoints | links |
| Hosting | GitHub; anyone can mirror it with `git clone` | a server, storage, accounts, uptime | none |
| Publishing | a pull request adding a version line, checked by CI (the commit exists, the hash matches, the manifest's name and version agree) | `ceesafec publish` with a token | edit a page |
| Integrity | the index records each version's hash, and the lock checks it as today | the server's archive hashes | none |
| Offline and mirrors | the index is a git repository like any dependency | needs a mirror protocol | — |
| Precedent | crates.io-index, Homebrew taps, Go's sum database (in part) | npm, PyPI | — |

**A is proposed.** It adds names and versions without a service to run. The code still comes from each package's
own git repository, pinned to a commit and a hash exactly as today. B can come later behind the same manifest
syntax, if the index outgrows git.

## 3. Proposed decision

1. **The index** is a git repository: by default `https://github.com/vkuttyp/CeeSafe-index`, or `$CEESAFE_INDEX`.
   - One file per package, `packages/<name>.toml`, with one entry per published version: `version`, `git` (a URL),
     `commit` (a full id), `hash` (the SHA-256 of `tooling.md` §4.3), and `dependencies` (the version requirements of
     that version's `ceesafe.toml`, so solving needs no checkouts).
   - Entries are only appended. A version can be marked `yanked = true`, which keeps old locks working and stops new
     resolutions from choosing it.
   - The index's CI checks each pull request: the commit exists, its manifest's name and version match the entry,
     the hash matches, and the version is new.
2. **Registry dependencies** in `ceesafe.toml`:
   ```toml
   [dependencies]
   geometry = "1.4"           # a version requirement: >=1.4.0, <2.0.0
   lz4 = "=0.3.1"             # exactly
   json = { version = "0.9", index = "https://example.org/my-index.git" }   # another index
   ```
   - Requirements follow Semantic Versioning's caret rule: `"1.4"` means `>=1.4.0, <2.0.0`, and `"0.3"` means
     `>=0.3.0, <0.4.0`.
   - Also `=`, `>=`, `<`, and comma-separated combinations.
   - Path and git dependencies keep their meaning, and override the index for their name.
3. **Version solving** (`ceesafec fetch`): the newest version of each package that satisfies every requirement on it,
   found by a PubGrub-style solver.
   - A **failure is explained**, as a chain of requirements (`app needs geometry ^1.4; geometry 1.4 needs shapes ^2;
     plot needs shapes ^1`), as a new code.
   - **One version per package name** in a build, as today. Allowing several would need distinct C names per version
     (`c-backend.md` §2.1) and is left open (OD-26).
   - The **result goes into `ceesafe.lock`**: the source is `registry+<index URL>`, with the version, commit and hash.
     From then on a build is exactly as reproducible as with git dependencies today. `ceesafec fetch --update [name]`
     re-solves; otherwise a lock that still satisfies the manifest is kept.
4. **Publishing** is a pull request to the index.
   - `ceesafec publish` (no network) checks the package: it builds, the manifest's version is new, and the working tree
     is a clean, pushed commit.
   - It then prints the entry to add.
   - No tokens and no accounts. The index's maintainers review the first version of each new package name, and later
     versions from the same repository merge automatically after CI.
5. **Unchanged:**
   - D5: nothing runs at fetch or build time but `git`.
   - `--offline`.
   - The hashes.
   - N0326, for a name reached through a path or git dependency and the registry at once.
   - The kernel profile, which needs nothing new.

## 4. Consequences

- Packages can be found by name and depended on by a range, and builds stay pinned and hashed.
- The index is a public repository that someone must review: the first version of each package name, by hand.
- Solving adds about one module to `Packages/` and new tests:
  - unit tests of the solver over synthetic indexes, including the failure explanations;
  - an end-to-end test against a local index repository made in the test, so it needs no network.
- Not decided here: several versions of one package in a build (OD-26), private-index authentication (plain git
  credentials work already), and a search command.

## 5. Questions for the owner (decided 2026-10-28: each as recommended)

1. **A git index (A)** rather than a registry server? Recommended: A.
2. **Where it lives:** a new public repository, `vkuttyp/CeeSafe-index`? Or an organisation, which would need creating?
   Recommended: `vkuttyp/CeeSafe-index` now, moved later if an organisation is made. Creating it needs your account.
3. **Caret requirements** (`"1.4"` means `^1.4`)? Recommended: yes, as Cargo does.
4. **One version per package name in a build** (OD-26 for more)? Recommended: yes.
5. **Review:** should the first version of each package name be reviewed by hand, and later versions merge after CI?
   Recommended: yes.

## 6. Implementation plan (once accepted)

Spec first (`tooling.md` §4.6 the index, §4.7 solving, §4.8 publishing; diagnostics), then:

- **G6a.** Version requirements and the index format, with a local test index; the solver and its failure
  explanations.
- **G6b.** `ceesafec fetch` with registry dependencies, and the lock entries.
- **G6c.** `ceesafec publish`'s checks.
- **G6d.** The index repository and its CI, once the owner has created it.
