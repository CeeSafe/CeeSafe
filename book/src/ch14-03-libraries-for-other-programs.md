---
title: Libraries and objects for other programs
summary: How CeeSafe code becomes part of a program written in C or another language - exported functions, build --emit lib for a static library with its generated headers, a C program that links it and runs, and --emit obj and --emit c.
assumes: [ch07-01-package-and-manifest, ch09-01-panics]
keywords: ["@export", "@repr(C)", "--emit", "static library", archive, "ceesafe_exports.h", "ceesafe/embed.h", ceesafe_init, "panic hook", object, C]
diagnostics: [N0291]
specification: ["type-system.md §28.3", "type-system.md §28.4", "type-system.md §28.6", "tooling.md §8.3", "profiles.md §4.3"]
---

# Libraries and objects for other programs

**This section teaches** how to build CeeSafe code into a program whose `main` is not CeeSafe's: a C program here,
and in the same way a C++ or Rust program, since each of them can call C. It assumes
[A package and its manifest](./ch07-01-package-and-manifest) and [Panics](./ch09-01-panics). You need to read a
little C. One function uses a raw pointer and `unsafe`, which chapter 20 teaches; the text says what they do.

## What `--emit` chooses

`ceesafec build` makes an executable, and for that the package needs a `main`. The option `--emit` makes something
else, and needs no `main`:

| Command | What it writes | For |
|---|---|---|
| `ceesafec build` | `out/<name>`, an executable | running |
| `ceesafec build --emit lib` | `out/<name>.a`, a static library, and two headers | a program that links CeeSafe code |
| `ceesafec build --emit obj` | `out/<name>.o`, one object file | a link that takes objects, such as a kernel's |
| `ceesafec build --emit c` | `out/<name>-c/`, the C source | a build system that compiles the C itself |

In each case the output holds the package, every package it depends on, and CeeSafe's runtime. `-o` gives it
another path.

## Functions that C can call

The other program, the **host**, calls CeeSafe through **exported** functions. The package `measure` (in
`book/projects/ch14/`) is a library with three.

<span class="listing-caption"><strong>Listing 14-7</strong>, <code>measure/src/lib.cee</code>: Three functions a C program can call, and a struct it can pass.</span>

<!-- ceesafe: skip -->
```ceesafe
/// A range of values, from `low` up to `high`.
@repr(C)
public struct Range {
    public low: i64;
    public high: i64;
}

/// How far `range` reaches.
@export("measure_width")
public fn width(range: Range) -> i64 => range.high - range.low;

/// The mean of `count` values that add up to `sum`.
@export("measure_mean")
public fn mean(sum: i64, count: i64) -> i64 => sum / count;

/// The number of words in `length` bytes at `text`: runs of bytes that are not spaces.
/// Safety: the caller passes `length` readable bytes at `text`.
@export("measure_words")
fn words(text: *u8, length: usize) -> u64 {
    let bytes = unsafe { Slice.fromRaw(text, length) };
    var count: u64 = 0;
    var inside = false;
    for (b in bytes) {
        if (b == ' ') {
            inside = false;
        } else if (!inside) {
            inside = true;
            count += 1;
        }
    }
    return count;
}
```

`@export("measure_mean")` before a function gives it that name in C. C has one space of names for the whole
program, so a prefix such as `measure_` keeps yours apart from the host's. The function is still an ordinary
CeeSafe function: `mean` is also `public`, and CeeSafe packages call it as `measure.mean`.

Only types that C has can cross:

| CeeSafe | C |
|---|---|
| `i8` to `i64`, `u8` to `u64` | `int8_t` to `int64_t`, `uint8_t` to `uint64_t` |
| `usize`, `isize` | `size_t`, `ptrdiff_t` |
| `bool` | `bool` |
| a raw pointer, `*u8` or `*mut u8` | `const uint8_t *`, `uint8_t *` |
| a struct marked `@repr(C)` | `struct` of the same name and fields |
| an enum without payloads marked `@repr(u8)` (or another integer type) | that integer type |

Borrows, slices, `StringView`, `String`, `Vector`, optionals and `Result` do not cross. So `words` takes text the
way C passes it, as a raw pointer `*u8` and a length. `Slice.fromRaw` makes a slice of them. The compiler cannot
check that the pointer leads to that many bytes, so the call must be inside `unsafe { … }`, and the function must
say in a `/// Safety:` comment what it relies on.

::: tip Rule
`@export` goes on a top-level function that is not generic. Its parameters and result are integers, `bool`, raw
pointers, `@repr(C)` structs and `@repr` enums, and nothing else.

*Specification: `type-system.md` §28.3, §28.4.*
:::

## The library and its headers

```sh
ceesafec build --emit lib
```

```text
out/
├── measure.a             the static library
├── ceesafe_exports.h     the exported functions and types, for C
└── ceesafe/
    └── embed.h           how a host starts CeeSafe's runtime
```

`ceesafec` writes `ceesafe_exports.h` from the package. For `measure` it declares:

```c
struct Range;

struct Range {
    int64_t low;
    int64_t high;
};

int64_t measure_width(struct Range);
int64_t measure_mean(int64_t, int64_t);
uint64_t measure_words(uint8_t const *, size_t);
```

`--exports-header measure.h` gives the header another name. Both headers are generated at each build: do not edit
them.

## A C program that uses it

<span class="listing-caption"><strong>Listing 14-8</strong>, <code>measure/host/main.c</code>: A C program that starts the runtime, calls the three functions, and then makes one of them panic.</span>

<!-- ceesafe: skip -->
```c
#include "ceesafe/embed.h"
#include "ceesafe_exports.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Called when CeeSafe code panics. It must end the process. */
static void on_panic(void *context, const ceesafe_panic_info *info) {
    (void)context;
    fprintf(stderr, "host: CeeSafe stopped: %s at %s:%u\n", info->reason, info->file, info->line);
    exit(3);
}

int main(void) {
    ceesafe_config config = {0};
    config.size = sizeof config;
    config.panic_hook = on_panic;
    if (ceesafe_init(&config) != 0) {
        return 1;
    }

    const char *text = "one archive for one program";
    struct Range range = {3, 10};
    printf("words: %llu\n", (unsigned long long)measure_words((const uint8_t *)text, strlen(text)));
    printf("width: %lld\n", (long long)measure_width(range));
    printf("mean: %lld\n", (long long)measure_mean(64, 2));
    printf("mean: %lld\n", (long long)measure_mean(64, 0));
    return 0;
}
```

Compile it with the system's C compiler. `-I out` is where the headers are, and the library goes on the command
line as a file:

```sh
cc -I out host/main.c out/measure.a -o out/host
./out/host
```

```text
words: 5
width: 7
mean: 32
host: CeeSafe stopped: division by zero at src/lib.cee:14
```

The exit status is 3. The first three lines are standard output and the last is standard error.

**Starting the runtime.** `ceesafe_init` is called once, before any exported function, with a `ceesafe_config`
whose `size` is set. After it, any thread of the host may call the exports. The config can also carry the
program's arguments, an allocator of the host's, and a **panic hook**.

**Panics.** The last call divides by zero, and CeeSafe panics. A panic cannot be caught and continued, in a host
as anywhere else ([Panics](./ch09-01-panics)). The hook is how the host learns of it: it gets the reason and the
CeeSafe file and line, and it must end the process. It must not return, call CeeSafe again, or jump out. Without a
hook, and in a host that never calls `ceesafe_init`, the runtime prints its usual line and aborts:

```text
ceesafe panic: division by zero at src/lib.cee:14:48
```

So an exported function should not panic on what the host can pass. A function that can fail returns a value that
says so, such as a `bool` or a status number, because a `Result` does not cross.

::: tip Rule
A host calls `ceesafe_init` once before it calls CeeSafe. One program links one CeeSafe library: the library
holds the runtime, and two of them clash.

*Specification: `type-system.md` §28.6; `tooling.md` §8.3.*
:::

If a host needs the code of several CeeSafe packages, make one package that depends on them all and build that.

## Objects and C source

`ceesafec build --emit obj` writes the same code as one object file, `out/measure.o`, with the same two headers beside it. The host
links it as it links the library: `cc -I out host/main.c out/measure.o -o out/host`. A library lets the linker
leave out what the program does not use, so prefer `--emit lib` unless the host's link needs an object. A package
whose manifest says `kind = "object"` builds its object with a plain `ceesafec build`.

`ceesafec build --emit c` stops before the C compiler:

```text
out/measure-c/
├── main.c                 the package, as C
├── ceesafe_exports.h
├── ceesafe_rt_core.c      the runtime
├── ceesafe_rt_hosted.c
├── ceesafe_rt_io.c
└── ceesafe/
    ├── embed.h
    ├── foreign.h
    └── rt.h
```

Give this directory to any build system that compiles C11. It is also how you read what the compiler made of your
code.

A package with a `main` can be built with `--emit lib` too. The library then leaves the C `main` out, since the
host has its own, and `--lib-main` keeps it.

## Mistakes

::: warning Mistake: exporting a function that takes a CeeSafe type
A `StringView` is a pointer and a length with a rule about how long it is valid. C has no such type.
:::

<span class="listing-caption"><strong>Listing 14-9</strong>, <code>main.cee</code>: An exported function cannot take a <code>StringView</code>.</span>

<!-- ceesafe: fail N0291 -->
```ceesafe
/// The number of bytes in `text`.
@export("measure_length")
fn length(text: StringView) -> usize => text.length;

fn main() {
    Console.writeLine(length("four"));
}
```

```text
main.cee:3:17: error[N0291]: exported function 'length' cannot take a 'StringView': only integers, bool, raw pointers, @repr(C) structs and @repr enums cross to C (type-system.md §28.3)
  |
3 | fn length(text: StringView) -> usize => text.length;
  |                 ^^^^^^^^^^
```

Take a `*u8` and a `usize`, as `words` in Listing 14-7 does. A method cannot be exported either: write a top-level
function that calls it.

::: warning Mistake: `ceesafec build` in a library package
Without `--emit`, `build` makes an executable and reports that there is no `main`. A library for a host is
`ceesafec build --emit lib`.
:::

::: warning Mistake: two CeeSafe libraries in one program
Each `.a` holds the runtime. Linking two into one program gives the linker two copies of every runtime function,
and it stops with duplicate symbols. Build one library from one package that depends on the others.
:::

::: details Coming from Rust
`--emit lib` is `crate-type = ["staticlib"]`. `@export("name")` is `#[no_mangle] pub extern "C" fn name`, and
`@repr(C)` is `#[repr(C)]`. The header is written by the compiler, with no `cbindgen`. There is no
`catch_unwind`: a panic never unwinds, so it cannot be turned into an error code at the boundary.
:::

::: details Coming from C or C++
The library is an ordinary static archive and the header an ordinary C header, with `extern "C"` guards for C++.
What is new is the start: call `ceesafe_init` before the first export. Memory that CeeSafe allocates is freed by
CeeSafe: if an export hands out a pointer, another export takes it back.
:::

## Summary

- `ceesafec build --emit lib` writes `out/<name>.a` with `ceesafe_exports.h` and `ceesafe/embed.h`; `--emit obj`
  writes one object; `--emit c` writes the C source. None needs a `main`.
- `@export("symbol")` on a top-level function makes it callable from C. Only integers, `bool`, raw pointers,
  `@repr(C)` structs and `@repr` enums cross (`N0291` for anything else).
- The host includes the two headers, calls `ceesafe_init` once, and then calls the exports.
- A panic ends the process. A panic hook lets the host report it first.
- One program links one CeeSafe library, which holds every package it needs and the runtime.

This is the end of chapter 14. Chapter 20 returns to C from the other side, with CeeSafe calling C functions, and
chapter 22 builds CeeSafe for a machine with no operating system.
