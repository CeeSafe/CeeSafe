---
title: CeeSafe inside a host program
summary: What ceesafe_init gives a host beyond a panic hook - its own allocator under every CeeSafe allocation, the allocation report and ceesafe_shutdown, the rule for memory that crosses between the two sides, and the smaller freestanding form of the interface.
assumes: [ch14-03-libraries-for-other-programs, ch22-01-the-freestanding-profile, ch15-01-box]
keywords: [host, embedding, "--emit lib", "ceesafe/embed.h", ceesafe_init, ceesafe_config, ceesafe_allocator, ceesafe_shutdown, alloc_report, fail_alloc_at, "panic hook", "Box.intoRaw", "Box.fromRaw", "*mut void", handle, CEESAFE_INIT_AGAIN, CEESAFE_INIT_LATE]
diagnostics: [N0291]
specification: ["type-system.md §28.3", "type-system.md §28.6", "type-system.md §34.2", "tooling.md §8.3", "profiles.md §4.2"]
---

# CeeSafe inside a host program

**This section teaches** how a CeeSafe library lives in a program that it does not own: where its memory comes
from, how the host learns what it allocated, who frees a value that crosses between the two, and what changes when
the host is a kernel. It assumes
[Libraries and objects for other programs](./ch14-03-libraries-for-other-programs), which builds a library with
`--emit lib`, exports functions and installs a panic hook, and [Box](./ch15-01-box). It also assumes
[The freestanding profile](./ch22-01-the-freestanding-profile) for its last part.

## Two ways to be a guest

The first two sections of this chapter and section 14.3 show the same arrangement from two sides: a `main` that
is not CeeSafe's, and CeeSafe code entered through exports. What the **host** provides differs with the profile.

| | Hosted library (14.3, this section) | Freestanding object (22.1) |
|---|---|---|
| Built with | `ceesafec build --emit lib` | `--profile freestanding`, `--emit obj` or `--emit lib` |
| The language | all of it: `Vector`, `String`, threads, files | no heap types, no operating-system services |
| Memory | the C library's `malloc`, or an allocator the host passes to `ceesafe_init` | what the code is given; `Box.createIn` with your allocator |
| A panic | a hook passed to `ceesafe_init`, at run time | the function `ceesafe_panic`, at link time |
| Started by | `ceesafe_init`, once | nothing to start |

Section 14.3 used `ceesafe_init` for one thing, the panic hook. A large host, such as a database or a language
runtime, wants more: every byte in the process should come from its allocator, and its tests should see what a
library leaked. The same `ceesafe_config` carries both.

## A library that keeps memory between calls

The package `journal` (in `book/projects/ch22/`) is a hosted library. The host opens a journal, adds lines to it
over several calls, and closes it. The journal lives on CeeSafe's heap in between, and the host holds a pointer
to it.

<span class="listing-caption"><strong>Listing 22-13</strong>, <code>journal/src/lib.cee</code>: A value that CeeSafe allocates, the host keeps as a handle, and CeeSafe frees.</span>

<!-- ceesafe: skip -->
```ceesafe
/// The lines a host has added, end to end, and how many there are.
struct Journal {
    text: Vector<u8>;
    lines: u64;
}

/// A new, empty journal for the host to keep, or null when there is no memory.
@export("journal_open")
fn open() -> *mut void {
    match (Box.create(Journal { text: Vector<u8>.create(), lines: 0 })) {
        Ok(journal) => {
            return Box.intoRaw(journal) as *mut void;
        }
        Err(e) => {
            return 0 as *mut void;
        }
    }
}

/// Adds the `length` bytes at `line` to `journal`; false when there is no memory.
/// Safety: `journal` came from `journal_open` and is not closed; `line` has `length` readable bytes.
@export("journal_add")
fn add(journal: *mut void, line: *u8, length: usize) -> bool {
    let entry = unsafe { Ptr.toBorrowMut(journal as *mut Journal) };
    if (entry.text.extend(unsafe { Slice.fromRaw(line, length) }).isErr()) {
        return false;
    }
    entry.lines += 1;
    return true;
}

/// The number of bytes `journal` holds.
/// Safety: `journal` came from `journal_open` and is not closed.
@export("journal_size")
fn size(journal: *mut void) -> u64 {
    let entry = unsafe { Ptr.toBorrow(journal as *mut Journal) };
    return entry.text.length as u64;
}

/// Gives `journal` back: its memory is freed here, by the code that allocated it.
/// Safety: `journal` came from `journal_open`, and the host does not use it again.
@export("journal_close")
fn close(journal: *mut void) {
    let owned: Box<Journal> = unsafe { Box.fromRaw(journal as *mut Journal) };
}
```

Four pieces of this are the pattern for any value a host keeps:

- **`Box.intoRaw(b)`** takes the box and returns the address of its contents as a raw pointer. Nothing is freed.
  From here the compiler no longer tracks the value: the host owns it.
- **`*mut void`** is how a pointer to a CeeSafe struct crosses to C. A `Journal` is not a `@repr(C)` struct, so C
  may not see inside it, and the header declares the handle as `void *`.
- **`Ptr.toBorrowMut(p)`** and **`Ptr.toBorrow(p)`** make a borrow from a raw pointer for the length of one call.
  They are `unsafe`: the `/// Safety:` line says what the host must keep true.
- **`Box.fromRaw(p)`** makes the box again. In `close`, `owned` goes out of scope at once, so the journal and its
  vector are destroyed and freed in the ordinary way.

Each failure is a value the host can test, null or `false`, since a `Result` does not cross and a panic would end
the host.

::: tip Rule
Each side frees what it allocated, through its own code. A value CeeSafe hands out with `Box.intoRaw` comes back
through an export that calls `Box.fromRaw`. The host never passes it to its own `free`.

*Specification: `type-system.md` §28.6, §34.2.*
:::

## The host's allocator, and the report

<span class="listing-caption"><strong>Listing 22-14</strong>, <code>journal/host/main.c</code>: A host that gives CeeSafe its allocator, asks for the allocation report, and checks for a leak.</span>

<!-- ceesafe: skip -->
```c
#include "ceesafe/embed.h"
#include "ceesafe_exports.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The host's allocator: the C library's, with a count of the calls CeeSafe makes. */
static unsigned long requests, releases;

static void *host_alloc(void *context, size_t size) {
    (void)context;
    requests++;
    return malloc(size);
}

static void *host_alloc_zeroed(void *context, size_t size) {
    (void)context;
    requests++;
    return calloc(1, size);
}

static void *host_realloc(void *context, void *block, size_t size) {
    (void)context;
    requests++;
    return realloc(block, size);
}

static void host_free(void *context, void *block) {
    (void)context;
    releases++;
    free(block);
}

static const ceesafe_allocator host_allocator = {host_alloc, host_alloc_zeroed, host_realloc, host_free, NULL};

static void on_panic(void *context, const ceesafe_panic_info *info) {
    (void)context;
    fprintf(stderr, "host: CeeSafe stopped: %s at %s:%u\n", info->reason, info->file, info->line);
    exit(3);
}

static void add(void *journal, const char *line) {
    if (!journal_add(journal, (const uint8_t *)line, strlen(line))) {
        fprintf(stderr, "host: no memory for a line\n");
    }
}

int main(void) {
    ceesafe_config config = {0};
    config.size = sizeof config;
    config.allocator = &host_allocator;
    config.alloc_report = true;
    config.panic_hook = on_panic;
    int32_t started = ceesafe_init(&config);
    if (started != 0) {
        fprintf(stderr, "host: ceesafe_init failed: %d\n", (int)started);
        return 1;
    }
    printf("second init: %d\n", (int)ceesafe_init(&config));

    void *journal = journal_open();
    if (journal == NULL) {
        return 1;
    }
    add(journal, "the host owns main");
    add(journal, "CeeSafe owns the journal");
    printf("size: %llu\n", (unsigned long long)journal_size(journal));
    printf("live before close: %llu\n", (unsigned long long)ceesafe_shutdown());
    journal_close(journal);
    printf("live after close: %llu\n", (unsigned long long)ceesafe_shutdown());
    printf("host allocator: %lu requests, %lu releases\n", requests, releases);
    return 0;
}
```

```sh
ceesafec build --emit lib
cc -I out host/main.c out/journal.a -o out/host
./out/host
```

```text
second init: 1
size: 42
ceesafe alloc: attempts=3 live=2
live before close: 2
ceesafe alloc: attempts=3 live=0
live after close: 0
host allocator: 3 requests, 2 releases
```

The two lines that begin `ceesafe alloc:` are the report, on standard error. The rest is standard output.

**The allocator** is four functions with the contracts of `malloc`, `calloc(1, size)`, `realloc` and `free`, and
a context pointer that is passed back to each. Once it is installed, every block the runtime needs comes from
it: the box, the vector's buffer, and the runtime's own buffers. Two things are stricter than `malloc`'s
contract. Blocks must be aligned to 16 bytes at least, and the functions must be safe to call from any thread
and stay valid until the process ends. Here they received three requests (the box, and two for the vector as it
grew) and two releases.

**The report.** `alloc_report = true` makes the runtime count. `ceesafe_shutdown()` prints
`ceesafe alloc: attempts=… live=…` and returns the number of live blocks. It frees nothing and stops nothing:
exports can still be called after it, as `journal_close` is here. Before the close two blocks are live, after it
none. A host's test calls it at the end and fails when the result is not 0.

**Failing on purpose.** `config.fail_alloc_at = 3` makes the third allocation fail, as `--sweep alloc` does for
CeeSafe's own tests ([Allocators, arenas and scratch frames](./ch15-03-allocators#testing-every-failure)). Run
that way, this host prints `host: no memory for a line` and `size: 18`: the second line was refused whole, and
the failure was made by the runtime without asking the host's allocator, which saw two requests.

**The results of `ceesafe_init`.** It returns 0, or one of the constants in `ceesafe/embed.h`, having changed
nothing:

| Result | Value | When |
|---|---|---|
| `CEESAFE_INIT_AGAIN` | 1 | it was called before, as the second call above shows |
| `CEESAFE_INIT_LATE` | 2 | the config asks for an allocator or for counting, and CeeSafe has already allocated |
| `CEESAFE_INIT_SIZE` | 3 | the config is null, or its `size` is not one this runtime knows |
| `CEESAFE_INIT_ALLOCATOR` | 4 | an allocator with a null function |
| `CEESAFE_INIT_MEMORY` | 5 | there was no memory to copy the arguments |

`LATE` exists because an allocator cannot change under live blocks: a block from `malloc` would later be given
to the host's `free`. The config has three more fields. `argc` and `argv` give the program's arguments to
`Process.argument`, and `flags` set to `CEESAFE_INIT_ENVIRONMENT` lets the environment variables
`CEESAFE_ALLOC_REPORT` and `CEESAFE_FAIL_ALLOC_AT` switch the report and the failure on. This book did not run
those three.

::: tip Rule
`ceesafe_init` happens once, before any call into CeeSafe on any thread, and an allocator can only be given
then. After it, exports may be called from any thread of the host.

*Specification: `type-system.md` §28.6; `tooling.md` §8.3.*
:::

## When the host is a kernel

A freestanding package can be built as an archive too: `ceesafec build --emit lib` in the package `gauge` of
section 22.1 writes `out/gauge.a` with `ceesafe_exports.h` and `ceesafe/embed.h` beside it. The header is then
in its **freestanding form**, which begins with `#define CEESAFE_EMBED_FREESTANDING 1` and whose config is two
fields:

```c
typedef struct ceesafe_config {
    uint32_t size;                      /* sizeof(ceesafe_config) */
    uint32_t flags;                     /* none are defined: pass 0 (this version ignores the field) */
} ceesafe_config;
```

There is nothing else to configure. There are no arguments and no environment. There is no heap for an allocator
to stand under, and the panic hook is the `ceesafe_panic` that the environment links. `ceesafe_init` only checks
`size`, so that a kernel can call the same function in both profiles. Linked with a C stand-in on the machine
this chapter was written on, it returned 0 for the right size, 0 again when called twice (this form keeps no
state, so there is no `AGAIN`), and `CEESAFE_INIT_SIZE` for a wrong one. `ceesafe_shutdown` returned 0.

An archive suits a kernel whose linker should drop unused code, and it is the form the compiler names when it
refuses an object for a Windows target (that build was not run for this book). One program still links one CeeSafe archive or object: two of them each carry the
runtime.

## Mistakes

::: warning Mistake: passing a pointer to a CeeSafe struct as itself
The message lists raw pointers among the types that cross. It means pointers to types C has: integers, `bool`
and `@repr(C)` structs.
:::

<span class="listing-caption"><strong>Listing 22-15</strong>, <code>main.cee</code>: An export that takes a pointer to a struct C cannot describe.</span>

<!-- ceesafe: fail N0291 -->
```ceesafe
struct Journal {
    lines: u64;
}

/// The number of lines in `journal`.
/// Safety: `journal` points to a live `Journal`.
@export("journal_lines")
fn lines(journal: *Journal) -> u64 {
    let entry = unsafe { Ptr.toBorrow(journal) };
    return entry.lines;
}
```

```text
main.cee:8:19: error[N0291]: exported function 'lines' cannot take a '*Journal': only integers, bool, raw pointers, @repr(C) structs and @repr enums cross to C (type-system.md §28.3)
  |
8 | fn lines(journal: *Journal) -> u64 {
  |                   ^^^^^^^^
```

Take a `*void` (or `*mut void`) and convert inside: `Ptr.toBorrow(journal as *Journal)`. Mark the struct
`@repr(C)` only if the host is meant to read its fields.

::: warning Mistake: freeing a CeeSafe value with the host's `free`
`free(journal)` in the host compiles, and even with one shared allocator it is wrong: the vector inside the
journal is never freed, the report counts a block as live for ever, and nothing runs the value's `drop`. Call the
export that closes it.
:::

::: warning Mistake: calling `ceesafe_init` after the first export
A host that calls `journal_open` and then `ceesafe_init` with an allocator gets `CEESAFE_INIT_LATE` (2), and the
runtime goes on with `malloc`. Check the result of `ceesafe_init`, and call it where the host starts, before it
makes the threads that call CeeSafe.
:::

::: warning Mistake: using a handle after closing it
`journal_close(journal)` twice, or `journal_add` after it, is the host breaking the `/// Safety:` contract. The
compiler checks neither side of that line: on the CeeSafe side it is inside `unsafe`, and the host is C. Keep
such contracts short, and set the host's pointer to null when it closes the handle.
:::

::: details Coming from Rust
`Box.intoRaw` and `Box.fromRaw` are `Box::into_raw` and `Box::from_raw`, and the handle is the usual opaque
pointer of a C API. The allocator is not a `#[global_allocator]` chosen at compile time: the host passes it at
run time, once, and it sits under the runtime's own counting.
:::

::: details Coming from C
`ceesafe_allocator` is the shape of the hooks many C libraries take, such as a `set_mem_functions` call, with
two differences. It must be installed before the first allocation, and it reports through `ceesafe_shutdown`
what is still live.
:::

## Summary

- A hosted library gets its memory from `malloc`, or from the `allocator` in the config of `ceesafe_init`: four
  functions with malloc's contracts, 16-byte alignment, usable from any thread.
- `alloc_report` and `ceesafe_shutdown()` tell the host how many blocks CeeSafe still holds. `fail_alloc_at`
  makes one allocation fail.
- `ceesafe_init` returns 0 or a `CEESAFE_INIT_` constant: `AGAIN` for a second call, `LATE` for an allocator
  after the first allocation.
- A value the host keeps is a box turned into a `*mut void` with `Box.intoRaw`, borrowed in each export with
  `Ptr.toBorrow` or `Ptr.toBorrowMut`, and given back to an export that calls `Box.fromRaw`.
- Each side frees what it allocated.
- In the freestanding profile `ceesafe/embed.h` has a config of `size` and `flags` only: the panic function is
  linked and there is no heap to configure.

This is the end of chapter 22. A real kernel or board is the next step, and its documents are the
specification's: `profiles.md` for what an object contains, and `type-system.md` §27 to §34 for everything this
chapter used at the boundary.
