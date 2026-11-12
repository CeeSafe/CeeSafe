---
title: Publishing
summary: How a library reaches other people - by path, by a git tag, or through the registry, where ceesafec publish checks the package and prints the index entry that a pull request adds to the index, with what a version promises.
assumes: [ch07-01-package-and-manifest, ch07-04-dependencies]
keywords: [publish, "ceesafec publish", registry, index, "index-check", version, git, tag, CEESAFE_INDEX, yanked]
diagnostics: [N0335]
specification: ["tooling.md §4.6", "tooling.md §4.7", "tooling.md §4.8"]
---

# Publishing

**This section teaches** how to give a library to other people so that their packages can depend on it. It assumes
[A package and its manifest](./ch07-01-package-and-manifest) and
[Dependencies and the lock file](./ch07-04-dependencies), which shows the other side: how a package names and
fetches a dependency.

## What is given

A CeeSafe library is given as source. There is no compiled form to upload and no server that holds packages. A
program that depends on a library compiles the library's `.cee` files with its own. So to publish a library is to
put its source where others can fetch it, and to tell them where.

There are three ways, and they match the three forms of a dependency:

| The other package writes | What you do |
|---|---|
| `tally = { path = "../tally" }` | Nothing. The library is a directory beside the program, in one repository or on one disk. |
| `tally = { git = "https://example.org/tally.git", tag = "v0.1.0" }` | Push the library to a git repository that the other person can read, and tag the commit. |
| `tally = "0.1"` | The same, and then add the version to the registry's **index**. |

The first is for code that belongs to one project. The second needs nothing from CeeSafe: any git repository with
a `ceesafe.toml` at its top is a library others can depend on. The rest of this section is about the third.

## A library that is ready

The library here is `tally`, the one chapter 7 builds, in a git repository of its own.

<span class="listing-caption"><strong>Listing 14-6</strong>, <code>tally/ceesafe.toml</code>: A library with a version, which the registry needs.</span>

<!-- ceesafe: skip -->
```toml
[package]
name = "tally"
version = "0.1.0"
kind = "library"

[dependencies]
```

Before you give a library to anyone, look at it as its users will:

- Only `public` items are theirs to use ([Files and `public`](./ch07-02-files-and-public)). Everything public is
  a promise you will have to keep.
- `ceesafec doc` writes a page of the public items with their documentation comments, and `ceesafec doc --missing`
  lists the public items that have none.
- If the library needs a recent compiler, say so with the `ceesafec` field of `[package]`.

## The registry is an index

The **registry** is not a store of packages. It is an **index**: a git repository with one file for each library,
`packages/<name>.toml`, that lists the library's published versions. Each entry says where that version is: a git
repository, a commit, and a hash of the files. The default index is `https://github.com/CeeSafe/CeeSafe-index`.

When a package depends on `tally = "0.1"`, `ceesafec fetch` reads `packages/tally.toml` in the index, chooses a
version, and fetches that commit from the library's own repository. The index only points.

## `ceesafec publish`

`ceesafec publish` uploads nothing. It checks that the package can be published, and prints the entry for the
index. Commit the library, push it, and run it in the library's directory:

```sh
git push origin main
ceesafec publish
```

```text
[[version]]
version = "0.1.0"
git = "https://example.org/tally.git"
commit = "d8a1d78f22942c26b768061e63b6fe44a15de642"
hash = "sha256:bd78a12152473abe80b12dcfafbcd007665696e1b9e5cdcf0b1b8c5a37fc8de2"
ceesafec: add this entry to packages/<name>.toml in the index, by pull request (tooling.md §4.8)
```

The first five lines are the entry, on standard output. The last line is a note on standard error, so sending
standard output to a file gives the entry alone.

- `git` is the URL of the remote that the commit was pushed to.
- `commit` is the commit you are on. That commit is the version, for ever.
- `hash` is the SHA-256 of the manifest and the source files, the same hash a lock file records.
- A library with dependencies has one more line, `dependencies = { tally = "0.1" }`, with the requirements as its
  manifest writes them.

::: tip Rule
`ceesafec publish` accepts a package that is a library with a `version`, whose dependencies are all registry
dependencies, that checks without errors, whose files are all committed, and whose commit is on a branch of a
remote. Anything else is `N0335`.

*Specification: `tooling.md` §4.8.*
:::

To publish, add the entry to the end of `packages/tally.toml` in the index and open a pull request. The index's
own checks run `ceesafec index-check`, which checks every entry: the commit exists, its manifest has the entry's
name and version, its hash and dependencies are the entry's, and the version is newer than every entry before it.

```text
ceesafec: 1 index entry checked
```

A new name is reviewed by the people who keep the index. Later versions from the same repository are merged when
the checks pass.

## Versions

To publish the next version, change `version` in the manifest, commit, push, and run `ceesafec publish` again. It
prints a new entry with the new commit, which goes after the first.

Entries are only added. A published version is never changed, because lock files everywhere record its commit and
its hash. If a version should not be used, its entry gets the line `yanked = true`: packages that have locked it
keep building, and no new `fetch` chooses it.

A version number is a promise to the packages that depend on yours. A requirement such as `"1.4"` accepts every
later `1.x`, and `"0.3"` accepts every later `0.3.x`. So:

| You changed | Raise |
|---|---|
| Only the inside: a fix, a faster body | `PATCH`: 0.1.0 to 0.1.1 |
| Added public items, and broke nothing | `MINOR` from 1.0.0 on: 1.4.0 to 1.5.0. Below 1.0.0, `PATCH`. |
| Removed or changed a public item | `MAJOR` from 1.0.0 on: 1.4.0 to 2.0.0. Below 1.0.0, `MINOR`: 0.1.1 to 0.2.0. |

A build holds one version of a library, never two. If two libraries in a program need versions of yours that no
single version satisfies, the program's `fetch` fails. A breaking change costs your users more here than in a
language that can load two versions side by side, so make them rarely.

A package that already depends on `tally = "0.1"` keeps the version in its lock file when 0.1.1 appears.
`ceesafec fetch --update` moves it.

## An index of your own

An index is an ordinary git repository, so a team can keep one for libraries that are not public: a repository
with a `packages/` directory, entries added as above, and `ceesafec index-check` run in it before each merge. A
dependency names the index it comes from:

```toml
[dependencies]
tally = { version = "0.1", index = "https://example.org/our-index.git" }
```

The environment variable `CEESAFE_INDEX` replaces the default index for every dependency that names none. It must
then be set for every command, not only for `fetch`, because the lock file records which index each library came
from. The `index` key in the manifest needs nothing set.

## Mistakes

::: warning Mistake: publishing before pushing
`publish` prints an entry only for a commit that others can fetch. With a change that is not committed:

```text
ceesafe.toml:1:1: error[N0335]: the package has uncommitted changes: commit them, so the published commit is what you checked
  |
1 | [package]
  | ^
```

And with a commit that is not pushed:

```text
ceesafe.toml:1:1: error[N0335]: commit caa8037e132b is on no remote branch: push it first
  |
1 | [package]
  | ^
```

Commit, push, and run `publish` again.
:::

::: warning Mistake: a published library with a path dependency
A library `summary` that depends on `tally = { path = "../tally" }` works on your disk and nowhere else.

```text
ceesafe.toml:1:1: error[N0335]: 'tally' is a path or git dependency: a published package depends only on the registry (tooling.md §4.8)
  |
1 | [package]
  | ^
```

Publish `tally` first, change the dependency to `tally = "0.1"`, run `ceesafec fetch`, and commit both files.
:::

::: warning Mistake: publishing a program
Only a library can be a dependency, so only a library can be published:
`only a library can be published: 'exe' has kind = "executable"`. A library without a `version` gets
`[package] needs a version to publish: version = "MAJOR.MINOR.PATCH"`. Both are `N0335`. To give people a program,
give them its repository to build, or the executable from `out/`.
:::

::: details Coming from Rust
`ceesafec publish` is not `cargo publish`. Nothing is uploaded and there is no account or token: the code stays in
your git repository, and the index holds a pointer to a commit. There is no `cargo yank` either: yanking is an edit
to the index, by pull request. Path and git dependencies must be gone before publishing, as with crates.io.
:::

::: details Coming from Go, TypeScript or Python
This is closest to Go, where a module is a git repository and a version is a tag. The difference is the index: a
short name and a version requirement are looked up there, in place of the URL being the name. Unlike `npm publish`
or uploading to PyPI, there is no package file and nothing to build first.
:::

## Summary

- A library is given as source: by path, by a git repository and tag, or by an entry in the registry's index.
- The index is a git repository of `packages/<name>.toml` files. Each entry names a version, a repository, a
  commit and a hash.
- `ceesafec publish` checks the package and prints its entry. You add the entry to the index by pull request, and
  `ceesafec index-check` checks it there.
- It needs a library with a `version`, registry dependencies only, no errors, everything committed and pushed
  (`N0335` otherwise).
- A published version never changes. A bad one is marked `yanked = true`.
- Raise `PATCH` for a fix and `MAJOR` for a break; below 1.0.0, `MINOR` is the breaking number.
- `index = "…"` in a dependency, or `CEESAFE_INDEX`, names another index.

Next: [Libraries and objects for other programs](./ch14-03-libraries-for-other-programs), on giving CeeSafe code
to a program that is not written in CeeSafe.
