---
title: Packages and files
summary: How a CeeSafe program grows past one file - a package and its manifest, the files of a package and the public keyword, imports from another package, and dependencies pinned by a lock file.
assumes: [ch01-03-hello-package, ch05-00-structs]
keywords: [package, ceesafe.toml, manifest, public, import, dependency, ceesafe.lock, "ceesafec fetch", library]
---

# Packages and files

**This chapter teaches** how a program is divided: into files, and into packages that use each other. It assumes
you have made a package with `ceesafec new` ([Hello, package](./ch01-03-hello-package)) and can write a struct with
methods ([chapter 5](./ch05-00-structs)).

A **package** is a directory with a file called `ceesafe.toml` and a `src/` directory of `.cee` files. It is the
unit the compiler works on: `ceesafec check`, `run` and `build` take a whole package at a time. It is also the unit
of naming. CeeSafe has two levels of names and no more:

- **Inside a package** every file sees every item of every other file. There is nothing to declare and nothing to
  import. Splitting a package into files and directories is for the people who read it. A file keeps an item to
  itself only by marking it `private`.
- **Between packages** nothing is visible unless it is marked `public`, and the other package's name comes first:
  `tally.pair(30, 34)`.

So the questions other languages answer with modules, namespaces, headers and export lists have short answers
here. To share code between two files, put them in one package. To share code between two programs, put it in a
**library** package and make both programs depend on it.

The chapter builds three small packages, which are in the book's repository under `book/projects/ch07/`:

- `scores`, a program of three files;
- `tally`, a library;
- `report`, a program that depends on `tally`.

## The sections

1. [A package and its manifest](./ch07-01-package-and-manifest): what a package is, each field of `ceesafe.toml`,
   executable and library packages, `ceesafec new`, and where a build goes.
2. [Files and `public`](./ch07-02-files-and-public): what the files of a package see of each other, what
   directories under `src/` mean (nothing), what `public` opens to other packages, and what `private` keeps to
   one file.
3. [Imports](./ch07-03-imports): naming another package's items with its name in front, or with `import`, and what
   happens when two names clash.
4. [Dependencies and the lock file](./ch07-04-dependencies): `[dependencies]` by path, by git and from the registry,
   `ceesafec fetch`, `ceesafe.lock`, `--offline`, and version requirements.

Read them in order the first time: each uses the packages of the one before.
