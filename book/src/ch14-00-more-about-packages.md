---
title: More about packages
summary: What a package can do beyond being checked and run - the settings of a build, how a library is published for other people's packages, and how CeeSafe code is built as a library or object for a program written in C.
assumes: [ch07-00-packages]
keywords: [package, build, "ceesafec build", publish, registry, index, "static library", "--emit", export]
---

# More about packages

**This chapter teaches** the parts of a package's life that come after the code works: how it is built, how it is
given to other CeeSafe programmers, and how it is given to programs that are not CeeSafe at all. It continues
[chapter 7](./ch07-00-packages), which covers the manifest, the files of a package, imports, and dependencies with
their lock file.

Chapter 7 treats a build as one command, `ceesafec build`, that writes `out/<name>`. That is enough for a long
time. It stops being enough at three points:

- **You want a different build.** One with debugger information, or built by another C compiler, or with a file
  of data inside the executable, or one that refuses to finish while there is a warning. CeeSafe has no build
  script and no table of profiles: a handful of options on the command line and a few fields of the manifest do
  this.
- **Someone else wants your library.** A dependency is always source that the other package fetches. Publishing is
  making that source reachable: by a git repository alone, or by an entry in the registry's index that
  `ceesafec publish` prints for you.
- **A program in another language wants your code.** `ceesafec` compiles through C, so its output can be a static
  library with a C header as readily as an executable. A C program links it and calls the functions you export.

The chapter uses three small packages, which are in the book's repository under `book/projects/ch14/`:

- `notice`, a program with an embedded file and a block that only some builds have;
- `tally`, the library of chapter 7, published;
- `measure`, a library with exported functions, and a C program that calls them.

## The sections

1. [Build settings](./ch14-01-build-settings): debug builds and `debugAssert`, the C compiler and its flags,
   `--split` and `--jobs`, `--deny-warnings`, `embed` and `[embed] roots`, `cfg` blocks, and what the manifest
   holds.
2. [Publishing](./ch14-02-publishing): the three ways to give a library, the index, `ceesafec publish` and
   `ceesafec index-check`, and what a version promises.
3. [Libraries and objects for other programs](./ch14-03-libraries-for-other-programs): `@export`, `--emit lib`,
   the generated headers, a C program that links the library and runs, and `--emit obj` and `--emit c`.

The sections do not depend on each other. Read the one you need.
