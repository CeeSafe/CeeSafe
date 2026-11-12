---
title: A package and its manifest
summary: What a package is and what each field of ceesafe.toml means - name, version, kind, sources and the compiler requirement - with executable and library packages, ceesafec new and new --lib, and where a build goes.
assumes: [ch01-03-hello-package]
keywords: [package, manifest, ceesafe.toml, name, version, kind, executable, library, sources, "ceesafec new", "--lib", out]
diagnostics: [N0321]
specification: ["tooling.md §4.1", "tooling.md §4.4"]
---

# A package and its manifest

**This section teaches** what a package is, what each line of its manifest says, the two kinds of package you will
make, and where the compiler puts what it builds. It assumes you have made and run a package once
([Hello, package](./ch01-03-hello-package)).

## A package

A **package** is a directory that holds a file called `ceesafe.toml`. That file is the package's **manifest**: it
names the package and says what kind it is. The package's source is the `.cee` files under its `src/` directory.

`ceesafec new` makes one:

```sh
ceesafec new scores
```

```text
ceesafec: made executable package 'scores' in scores
ceesafec: scores/AGENTS.md: wrote
```

```text
scores/
├── ceesafe.toml      the manifest
├── src/
│   └── main.cee      a program that prints a line
├── AGENTS.md         instructions for AI coding agents
└── .gitignore        one line, out/
```

Every command that takes no file works on the package of the directory you are in. The compiler looks for
`ceesafe.toml` there, then in the directory above, and so on up. So `ceesafec check` does the same thing in
`scores/` and in `scores/src/`.

::: tip Rule
A package is a directory with a `ceesafe.toml`. A command without file arguments uses the nearest `ceesafe.toml` in
the current directory or above it.

*Specification: `tooling.md` §4.1, §4.4.*
:::

## The manifest, field by field

<span class="listing-caption"><strong>Listing 7-1</strong>, <code>scores/ceesafe.toml</code>: The manifest that <code>ceesafec new scores</code> writes.</span>

```toml
[package]
name = "scores"
version = "0.1.0"
kind = "executable"

[dependencies]
```

The manifest is written in TOML: a `[table]` header, then `key = value` lines. A `#` begins a comment. It has two
tables.

`[package]` describes this package:

| Field | What it says | If you leave it out |
|---|---|---|
| `name` | What the package is called. `build` names the executable after it, and other packages use it to name this package's items. | An error: `name` is the one required field. |
| `version` | The package's version, three numbers: `MAJOR.MINOR.PATCH`. | No version. A package needs one only to be published. |
| `kind` | `"executable"` for a program, `"library"` for code that other packages use. | `"executable"`. |
| `sources` | The directories that hold the source, as a list. | `["src"]`. |
| `ceesafec` | The compiler versions that can build this package, such as `">=0.40.0"`. | Any compiler will try. |

`[dependencies]` lists the other packages this one uses. Here it is empty.
[Dependencies and the lock file](./ch07-04-dependencies) fills it, and explains the `ceesafec` field with the other
version requirements.

A name is an identifier, as the name of a function is. It cannot be a keyword, and it cannot be `core`, which is
the name of the built-in library. It becomes part of CeeSafe names such as `tally.pair`, so it cannot have a
hyphen:

```sh
ceesafec new my-app
```

```text
ceesafec: 'my-app' is not a package name: an identifier that is not a keyword or 'core'
```

`kind` has a third value, `"object"`, and `[package]` has more fields: for C code built with the package, for system
libraries to link, and for programs that run without an operating system. This book does not need them. They are in
the reference: https://ceesafe.github.io/CeeSafe/guide/packages.

## Executable and library packages

An **executable** package is a program. It has a `fn main()`, and you run it or build it.

A **library** package is code for other packages to use. It has no `main`. `ceesafec new --lib` makes one:

```sh
ceesafec new --lib tally
```

```text
ceesafec: made library package 'tally' in tally
ceesafec: tally/AGENTS.md: wrote
```

<span class="listing-caption"><strong>Listing 7-2</strong>, <code>tally/ceesafe.toml</code>: A library's manifest. Only <code>kind</code> differs.</span>

```toml
[package]
name = "tally"
version = "0.1.0"
kind = "library"

[dependencies]
```

In place of `src/main.cee` there is a `src/lib.cee`, with one function in it for you to replace:

```ceesafe
/// The answer tally gives.
public fn answer() -> i32 => 42;
```

`public` is what lets another package call the function; [Files and `public`](./ch07-02-files-and-public) is about
it. The line beginning `///` is a documentation comment.

The file names `main.cee` and `lib.cee` are what `ceesafec new` chooses. The compiler reads every `.cee` file under
`src/` and does not care what they are called.

::: tip Rule
An executable package has a `fn main()` and can be run and built. A library package has none, and can only be
checked by itself. Only a library can be a dependency.
:::

## What you can do with each

| Command | In an executable package | In a library package |
|---|---|---|
| `ceesafec check` | checks the package and its dependencies | the same |
| `ceesafec fmt` | formats the package's own files | the same |
| `ceesafec run --interp` | runs `main` | an error: there is no `main` |
| `ceesafec build` | writes `out/<name>` | an error: there is no `main` |

A library is not built to a file of its own. When you build a program, the compiler takes the program's package
and every package it depends on and compiles them together into one executable.

## Where a build goes

```sh
ceesafec build
./out/scores
```

`build` writes the executable into `out/` inside the package, under the package's name: `out/scores`, or
`out\scores.exe` on Windows. `out/` belongs to the compiler. Do not keep files of your own there and do not commit
it: the `.gitignore` that `ceesafec new` writes holds the one line `out/` for that reason.

`check` and `run --interp` write nothing in the package.

## Mistakes

::: warning Mistake: a field the manifest does not have
The manifest has a fixed set of fields. A key that is not one of them is an error, not a note the compiler skips.
:::

<span class="listing-caption"><strong>Listing 7-3</strong>, <code>scores/ceesafe.toml</code>: <code>licence</code> is not a field of <code>[package]</code>. No command accepts this manifest.</span>

```toml
[package]
name = "scores"
licence = "MIT"
kind = "executable"

[dependencies]
```

```text
ceesafe.toml:3:1: error[N0321]: unknown field 'licence' in [package]
  |
3 | licence = "MIT"
  | ^^^^^^^
```

Every command reads the manifest first, so `check`, `run` and `build` all stop here. Remove the line, or keep the
information in a `#` comment. `N0321` is the code for every wrong field or value: a `kind` that is not one of the
three says `'kind' is "executable", "library" or "object"`.

::: warning Mistake: running a library
`ceesafec run --interp` and `ceesafec build` in a library package fail, because there is no `main` to start at:

```text
src/internal/quotient.cee:1:1: error[N0300]: an executable needs a 'main' function: fn main() -> i32 { ... }
  |
1 | fn quotient(sum: i64, count: i64) -> i64 => if (count == 0) 0 else sum / count;
  | ^
```

The message points at the first line of the package's first file, which is not where anything is wrong. Check a
library with `ceesafec check`. To run its code, write an executable package that depends on it, or tests, which
chapter 11 covers.
:::

::: details Coming from Rust
`ceesafe.toml` is `Cargo.toml` with fewer parts. A package is either a program or a library, never both, and there
are no `[[bin]]` or `[lib]` tables, no features, no workspaces and no build scripts. `kind = "library"` replaces the
choice between `src/main.rs` and `src/lib.rs`: the file names mean nothing to `ceesafec`.
:::

::: details Coming from C# or Java
The manifest is the project file (`.csproj`, `pom.xml`), and `kind` is the output type. There is no separate
compiled library to reference: a dependency is source, compiled together with the program that uses it.
:::

::: details Coming from Go or TypeScript
`ceesafe.toml` is `go.mod` or `package.json`. A package is the whole directory tree under the manifest, not one
directory of it, and nothing in the manifest runs: there are no scripts.
:::

## Summary

- A package is a directory with a `ceesafe.toml`; its source is the `.cee` files under `src/`.
- `[package]` needs only `name`. `kind` is `"executable"` unless you say `"library"`; `sources` is `["src"]`.
- A name is an identifier that is not a keyword or `core`.
- `ceesafec new <name>` makes an executable package and `ceesafec new --lib <name>` a library.
- An executable has a `main` and builds to `out/<name>`. A library has none: it is checked alone and built only as
  part of a program.
- An unknown field or a wrong value in the manifest is `N0321`.

Next: [Files and `public`](./ch07-02-files-and-public), on what goes in `src/`.
