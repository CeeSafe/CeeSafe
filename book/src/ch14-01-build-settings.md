---
title: Build settings
summary: What changes a build - the options of ceesafec build for debugging, the C compiler and its flags, parallel compilation and warnings as errors, debug assertions, embedded files and their roots, cfg blocks, and the manifest fields that travel with the package.
assumes: [ch07-01-package-and-manifest, ch07-04-dependencies]
keywords: [build, "-g", debugAssert, "--cc", "--cflag", "--split", "--jobs", "--deny-warnings", embed, embedText, cfg, "--offline", manifest]
diagnostics: [N0400, N0362]
specification: ["tooling.md §3.1", "tooling.md §4.1", "tooling.md §8", "type-system.md §8", "type-system.md §10.1", "type-system.md §28.4"]
---

# Build settings

**This section teaches** what you can change about a build, and where each setting lives: on the command line or
in `ceesafe.toml`. It assumes [A package and its manifest](./ch07-01-package-and-manifest) and
[Dependencies and the lock file](./ch07-04-dependencies).

## What a build is

`ceesafec build` does three things. It checks the package, as `ceesafec check` does. It writes the program as C.
Then it runs a C compiler on that C, with optimisation on (`-O2`), and puts the executable in `out/`.

There are no named build modes. There is one build, and options that change it:

| Option | What it changes |
|---|---|
| `-g` | A debug build: no optimisation, debugger information that names CeeSafe lines, debug assertions on. |
| `--debug-assertions`, `--no-debug-assertions` | Whether `debugAssert` is evaluated, whatever `-g` says. |
| `-o <path>` | Where the output goes, in place of `out/<name>`. |
| `--cc <compiler>` | Which C compiler runs. |
| `--cflag <flag>` | One more flag for the C compiler. Repeat it for more. |
| `--split`, `--jobs <n>` | One C file for each CeeSafe file, compiled `n` at a time. |
| `--deny-warnings` | Warnings become errors. Also for `check` and `test`. |
| `--offline` | No network. Also for `check`, `run`, `test` and `fetch`. |

Two more are for other sections. `--emit` chooses what is written;
[Libraries and objects for other programs](./ch14-03-libraries-for-other-programs) covers it. `--profile freestanding`
builds for a machine with no operating system, which chapter 22 covers.

::: tip Rule
A build is optimised unless you pass `-g`. The command line chooses how to build; the manifest says what the
package is.

*Specification: `tooling.md` §8.3.*
:::

## Debug builds and debug assertions

`ceesafec build -g` compiles the C without optimisation and with debugger information, so that a debugger stops at
CeeSafe lines and steps through CeeSafe statements. On macOS it also writes `out/<name>.dSYM`.

A debug build turns on **debug assertions**. `debugAssert(condition, message)` is a check you write for yourself:
when it is on and the condition is false, the program panics ([Panics](./ch09-01-panics)). When it is off, nothing
of the call is compiled.

<span class="listing-caption"><strong>Listing 14-1</strong>, <code>main.cee</code>: A check that only some builds make. The second call breaks it.</span>

<!-- ceesafe: panics -->
```ceesafe
fn percent(part: i64, whole: i64) -> i64 {
    let result = part * 100 / whole;
    debugAssert(result <= 100, "a part larger than the whole");
    return result;
}

fn main() {
    Console.writeLine(percent(3, 12));
    Console.writeLine(percent(15, 12));
}
```

```text
25
```

That is `ceesafec run --interp main.cee`, which stops with exit status 134 and this on standard error:

```text
ceesafe panic: a part larger than the whole at main.cee:3:5
```

`ceesafec build -g` makes a program that does the same. A plain `ceesafec build` makes one that prints both
numbers:

```text
25
125
```

| Where | Debug assertions |
|---|---|
| `ceesafec build` | off |
| `ceesafec build -g` | on |
| `ceesafec run --interp` | on |
| `ceesafec test` | on |

`--debug-assertions` turns them on in an optimised build, and `--no-debug-assertions` turns them off under `-g` or
in the interpreter. `assert` has the same form and is never left out: use it for a check the program needs, and
`debugAssert` for one that costs too much to ship.

::: tip Rule
`debugAssert` is evaluated under `-g`, in the interpreter and in tests, and compiled to nothing otherwise. Because
the call may vanish, its arguments may not change anything.

*Specification: `type-system.md` §8; `tooling.md` §3.1.*
:::

## The C compiler

`--cc` names the C compiler. Without it, `ceesafec` uses the environment variable `CC`, and without that `cc` (on
Windows it looks for clang, gcc, then Visual Studio's compiler).

`--cflag` adds one flag after the compiler's own, so a later flag wins over an earlier one:

```sh
ceesafec build --cc clang --cflag -Os -o out/small
ceesafec build --cflag -fsanitize=address
```

The first builds for size in place of speed: for Listing 14-1 on macOS, 80,400 bytes where the default gives
96,560. The second adds the C compiler's address checker to the whole program. A flag is passed as it is, and a
flag the C compiler does not know is the C compiler's error or warning, not `ceesafec`'s.

By default the whole program is one C file. `--split` writes one C file for each CeeSafe file and compiles them in
parallel, as many at once as the machine has processors, or as `--jobs` says:

```sh
ceesafec build --split --jobs 4
```

The executable does the same thing either way. `--split` is for large programs, where compiling one file of C
takes long.

## Warnings as errors

A warning does not stop a build.

<span class="listing-caption"><strong>Listing 14-2</strong>, <code>main.cee</code>: A program that checks, with a warning. It prints the braces as written.</span>

<!-- ceesafe: warns N0400 -->
```ceesafe
fn main() {
    let count = 3;
    Console.writeLine("count = {count}");
}
```

```text
main.cee:3:32: warning[N0400]: this string prints '{count}' as written (there is no string interpolation): write the value in a call of its own, or '\{' to mean the brace
  |
3 |     Console.writeLine("count = {count}");
  |                                ^^^^^^^
```

`ceesafec check --deny-warnings main.cee` reports the same line as `error[N0400]` and exits with status 1, and
`build --deny-warnings` writes no executable. Use it where a build runs unattended, so that a warning cannot pass
unread.

## Files built into the program

`embedText(path)` is the text of a file as a `StringView`, and `embed(path)` its bytes as a `Slice<u8>`. The
compiler reads the file when it checks the package, and the bytes become part of the executable. The program opens
nothing when it runs.

The package `notice` (in `book/projects/ch14/`) has a file `data/notice.txt` with two lines, `notice 1.0` and
`Built from one package.`

<span class="listing-caption"><strong>Listing 14-3</strong>, <code>notice/src/main.cee</code>: A file built into the program, and a block that only some builds have.</span>

<!-- ceesafe: skip -->
```ceesafe
const NOTICE: StringView = embedText("../data/notice.txt");

fn main() {
    Console.write(NOTICE);
    if (cfg("NOTICE_LOUD")) {
        Console.writeLine("LOUD");
    } else {
        Console.writeLine("quiet");
    }
}
```

```text
notice 1.0
Built from one package.
quiet
```

That is what `ceesafec run --interp` prints. The path is relative to the directory of the source file that names it, with `/` on every system. It must be a
string literal or a constant, and the file must be 16 MiB or less.

::: tip Rule
An embedded file must lie inside the package's directory, or inside a directory that `[embed] roots` in the
manifest lists. Every check reads the file again.

*Specification: `type-system.md` §10.1; `tooling.md` §4.1.*
:::

## Blocks for some builds

Line 5 of Listing 14-3 is a **`cfg` block**. `if (cfg("NAME")) { … } else { … }` chooses between two blocks when
the C is compiled, by the C macro `NAME`: the first block where the macro is not zero, the second otherwise. Both
blocks are checked in every build. Set the macro with `--cflag`:

```sh
ceesafec build --cflag -DNOTICE_LOUD=1
./out/notice
```

```text
notice 1.0
Built from one package.
LOUD
```

The interpreter takes every macro as 0, which is why Listing 14-3 printed `quiet`. `cfg("!NAME")` is the opposite
condition. A `cfg` block is a statement, not a value: `let x = if (cfg("X")) 1 else 2;` does not compile.

Build with `--cflag -DNOTICE_LOUD=0` for the quiet program. With no definition at all the build still succeeds and
is quiet, but the C compiler prints a warning of its own: `'NOTICE_LOUD' is not defined, evaluates to 0`.

## What the manifest sets

The options above are given at each build. Nothing in `ceesafe.toml` sets the optimisation, the C compiler or its
flags. The manifest holds what is true of the package in every build. Beside the fields of
[A package and its manifest](./ch07-01-package-and-manifest), `[package]` has:

| Field | What it says |
|---|---|
| `tests = ["tests"]` | The directories of test files, which `check` and `test` read and `build` leaves out ([Running tests](./ch11-02-running-tests)). |
| `float = false` | No floating-point types in this package. The option `--no-float` says the same. |
| `profile = "freestanding"` | The package is for a machine with no operating system. The option `--profile` says the same. |
| `c = ["src/shim.c"]` | C files compiled and linked with the program. Chapter 19 covers calling C. |
| `links`, `lib-dirs`, `include-dirs` | System libraries to link, and where they and their headers are. |

A table `[target.windows]`, `[target.macos]` or `[target.linux]` holds `links`, `lib-dirs` and `include-dirs` for
one system. `[embed]` holds `roots`, which the first mistake below uses.

The lock file is a build setting too: `check`, `run`, `build` and `test` use exactly the dependencies that
`ceesafe.lock` records, and `--offline` keeps each of them from the network
([Dependencies and the lock file](./ch07-04-dependencies)).

## Mistakes

::: warning Mistake: embedding a file outside the package
A package can embed its own files. Here `shared/terms.txt` is two directories above the package, beside it and
others.
:::

<span class="listing-caption"><strong>Listing 14-4</strong>, <code>terms/src/main.cee</code>: The file exists, and the package may not read it.</span>

<!-- ceesafe: skip -->
```ceesafe
const TERMS: StringView = embedText("../../../shared/terms.txt");

fn main() {
    Console.write(TERMS);
}
```

```text
src/main.cee:1:37: error[N0362]: cannot embed '../../../shared/terms.txt': no file is there, or it is outside the package's directory and its [embed] roots
  |
1 | const TERMS: StringView = embedText("../../../shared/terms.txt");
  |                                     ^^^^^^^^^^^^^^^^^^^^^^^^^^^
```

The message is the same for a file that is missing and one that is outside, on purpose: a package cannot use it to
find out what is on your disk. Name the directory in the manifest, relative to the package:

<span class="listing-caption"><strong>Listing 14-5</strong>, <code>terms/ceesafe.toml</code>: A root lets the package embed from one more directory.</span>

<!-- ceesafe: skip -->
```toml
[package]
name = "terms"
kind = "executable"

[embed]
roots = ["../../shared"]

[dependencies]
```

With that manifest, `ceesafec run --interp` prints the file:

```text
Free to use, change and share.
```

Roots count only in packages you work on. A dependency fetched by git or from the registry embeds only its own
files, whatever its manifest says.

::: warning Mistake: `--jobs` without `--split`
`ceesafec build --jobs 4` is refused: `build's --jobs needs --split: without it, the C is compiled in one command`.
Without `--split` there is one C file and nothing to run in parallel.
:::

::: warning Mistake: work inside a `debugAssert`
A `debugAssert` is not in the program you ship. Do not put a check there that the program's users need, such as a
check of its input: that is `assert`, or a `Result` ([Result](./ch09-02-result)).
:::

::: details Coming from Rust
There are no `[profile.dev]` and `[profile.release]`. A plain `ceesafec build` is optimised, like
`cargo build --release`, and `-g` is the debug build. `debugAssert` is `debug_assert!`, and `assert` is `assert!`.
Integer overflow is checked in every build, not only in debug builds. `embed` and `embedText` are `include_bytes!`
and `include_str!`. A `cfg` block is not `#[cfg]`: both blocks are always type-checked, and the condition is a C
macro, not a feature or a target.
:::

::: details Coming from C
`--cflag` is where `CFLAGS` goes, and a `cfg` block is `#if NAME` … `#else` … `#endif`, except that the compiler
checks both halves in every build. `debugAssert` is `assert` under `NDEBUG`, and CeeSafe's `assert` has no off
switch.
:::

## Summary

- `ceesafec build` checks, writes C and compiles it with `-O2`. `-g` makes a debug build.
- `debugAssert` is evaluated under `-g`, in the interpreter and in tests, and compiled to nothing otherwise;
  `--debug-assertions` and `--no-debug-assertions` override that.
- `--cc` (or `CC`) chooses the C compiler, and each `--cflag` adds a flag after `ceesafec`'s own.
- `--split` compiles one C file for each CeeSafe file in parallel, and `--jobs` limits how many at once.
- `--deny-warnings` makes `check`, `build` and `test` fail on a warning.
- `embed` and `embedText` build a file into the program. It must be inside the package or an `[embed]` root
  (`N0362` if not).
- `if (cfg("NAME"))` chooses a block by a C macro, which `--cflag -DNAME=1` sets. The interpreter takes it as 0.
- The manifest has no fields for optimisation or C flags. It has `tests`, `float`, `profile`, `c`, the link fields
  and `[embed]`.

Next: [Publishing](./ch14-02-publishing), on giving a library to other people.
