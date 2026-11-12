---
title: The freestanding profile
summary: What CeeSafe keeps and removes when there is no operating system, how build --profile freestanding --emit obj makes one object, and the few lines of C - a panic function above all - that an environment must supply to link and run it.
assumes: [ch14-03-libraries-for-other-programs, ch09-01-panics, ch15-03-allocators]
keywords: [freestanding, hosted, profile, "--profile freestanding", "--emit obj", "kind = object", ceesafe_panic, "ceesafe/rt.h", FixedBufferAllocator, "Box.createIn", Allocator, "--cflag", "--target", "--ld", cross-compile, N0293]
diagnostics: [N0293]
specification: ["profiles.md §4.1", "profiles.md §4.2", "profiles.md §4.3", "profiles.md §4.4", "type-system.md §29.6", "type-system.md §30"]
---

# The freestanding profile

**This section teaches** how to build CeeSafe code for an environment with no operating system and no C library:
what the language still offers there, what you must supply, and how to run such code on your own machine. It
assumes [Libraries and objects for other programs](./ch14-03-libraries-for-other-programs) (`@export`, the types
that cross to C, `--emit`), [Panics](./ch09-01-panics) and
[Allocators, arenas and scratch frames](./ch15-03-allocators). It uses raw pointers and `unsafe`, which chapter 20
teaches, and says what each does.

## Two profiles

A **profile** says which services the compiled code may rely on. There are two:

- **hosted**, the default: an operating system and a C library are there. Every earlier chapter uses it.
- **freestanding**: nothing is there but the processor and memory. The code that surrounds yours, a kernel, a boot
  loader, a board's start-up file, is called the **environment** in this chapter.

You choose the profile for a whole build, with `--profile freestanding` on the command line or one line of the
manifest. The package `gauge` (in `book/projects/ch22/`) uses the manifest:

```toml
[package]
name = "gauge"
version = "0.1.0"
kind = "object"
profile = "freestanding"
```

`kind = "object"` makes a plain `ceesafec build` write one object file, `out/gauge.o`, as `--emit obj` does.

## What remains and what goes

Everything that needs no service remains: integers and `bool`, structs, enums, arrays, slices, `StringView`,
optionals, `Result`, borrows, generics, interfaces, closures, and every check the compiler and the generated code
make. The library `lib/core` is there too, since it is written in CeeSafe over those things.

What needs an operating system is removed, and naming it is an error, `N0293`:

| Not available | Why |
|---|---|
| `Console`, `Process` | output, arguments and exit are services |
| `File`, `TcpListener`, `TcpStream` | files and sockets |
| `SystemHeap`, `Box.create`, `Vector`, `String`, `Arena`, `Pool`, `Scratch`, `SlotMap`, `StableVector` | each takes memory from the system's heap |
| threads, `Async.scope`, `Async.blocking`, the async I/O of chapter 17 | there are no threads or descriptors |
| `fn main()` | nothing calls it |

::: tip Rule
A freestanding package has no `main`, and is entered through its `@export` functions. A name that needs the
hosted runtime is `N0293` wherever it appears.

*Specification: `profiles.md` §4.1; `type-system.md` §29.6.*
:::

## A freestanding package

<span class="listing-caption"><strong>Listing 22-1</strong>, <code>gauge/src/lib.cee</code>: Three exported functions and one C function the package expects from its environment.</span>

<!-- ceesafe: skip -->
```ceesafe
extern "C" {
    fn board_log(value: u64);
}

/// The sum of `length` bytes at `data`.
/// Safety: the caller passes `length` readable bytes at `data`.
@export("gauge_sum")
fn sum(data: *u8, length: usize) -> u32 {
    let bytes = unsafe { Slice.fromRaw(data, length) };
    var total: u32 = 0;
    for (b in bytes) {
        total += b as u32;
    }
    return total;
}

/// `part` as a percentage of `whole`.
@export("gauge_percent")
fn percent(part: u32, whole: u32) -> u32 => part * 100 / whole;

/// The squares of 1 to `count`, built in memory from a local array, then added up.
/// Safety: board_log only reads its argument.
@export("gauge_squares")
fn squares(count: u32) -> u32 {
    var storage: [u8; 64] = [0; 64];
    using memory = FixedBufferAllocator.over(mut storage);
    match (memory.allocArray(count as usize, 0u32)) {
        Ok(cells) => {
            var total: u32 = 0;
            for (i in 0..cells.length) {
                let n = truncate<u32>(i) + 1;
                cells[i] = n * n;
                total += cells[i];
            }
            unsafe { board_log(total as u64); }
            return total;
        }
        Err(e) => {
            return 0;
        }
    }
}
```

Three things in it are new or matter more here than in a hosted program.

**The `extern "C"` block** declares a function that is written in C and linked later. Calling it is `unsafe`,
because the compiler cannot see what it does. It is how freestanding code reaches anything outside itself: here
the board's way of showing a number.

**Memory without a heap.** `squares` needs an array whose length it learns at run time. A
`FixedBufferAllocator` hands out pieces of the 64 bytes in `storage`, a local on the stack, and gives an `Err`
when they run out. Nothing asks an operating system for anything.

**The checks stay.** `total += b as u32` is checked for overflow and `part * 100 / whole` for division by zero,
as in any CeeSafe program. A failed check is a panic.

## The panic function is yours

In a hosted program a panic prints a line and aborts the process. A freestanding object can do neither, so every
panic calls a C function that the object does not contain:

```c
_Noreturn void ceesafe_panic(cee_panic_kind kind, cee_loc loc);
```

The environment defines it: a kernel calls its own `panic`, firmware may write to a serial port and reset the
board. `kind` says which check failed, `ceesafe_panic_reason(kind)` gives the usual text for it, and `loc` holds
the CeeSafe file, line and column.

::: tip Rule
The code that links a freestanding object defines `ceesafe_panic`, and that function never returns.

*Specification: `profiles.md` §4.2.*
:::

An object built for this profile leaves only these names undefined: `ceesafe_panic`; the functions of its own
`extern "C"` blocks; and `memcpy`, `memmove`, `memset` and `memcmp`, which a C compiler may call for copies and
zeroing in any freestanding code. A program that uses async tasks also needs four `ceesafe_env_` functions for
a clock and for waiting; `profiles.md` §4.2 lists them.

## Running it on your machine

A C program can play the environment. It defines `board_log` and `ceesafe_panic`, and its `main` does what a boot
stub would do: call the exports.

<span class="listing-caption"><strong>Listing 22-2</strong>, <code>gauge/host/main.c</code>: A stand-in for the environment, which supplies the two functions the object needs and calls its exports.</span>

<!-- ceesafe: skip -->
```c
#include "ceesafe/rt.h"
#include "ceesafe_exports.h"
#include <stdio.h>
#include <stdlib.h>

/* The function the package declares in its extern "C" block. */
void board_log(uint64_t value);

void board_log(uint64_t value) {
    printf("log: %llu\n", (unsigned long long)value);
}

/* Every panic of the CeeSafe code arrives here. It must not return. */
_Noreturn void ceesafe_panic(cee_panic_kind kind, cee_loc loc) {
    fflush(stdout);
    fprintf(stderr, "board: stopped: %s at %s:%u:%u\n", ceesafe_panic_reason(kind), loc.file, (unsigned)loc.line,
        (unsigned)loc.column);
    exit(3);
}

int main(void) {
    const uint8_t readings[4] = {10, 20, 30, 40};
    printf("sum: %u\n", (unsigned)gauge_sum(readings, 4));
    printf("squares: %u\n", (unsigned)gauge_squares(5));
    printf("squares: %u\n", (unsigned)gauge_squares(100));
    printf("percent: %u\n", (unsigned)gauge_percent(3, 12));
    printf("percent: %u\n", (unsigned)gauge_percent(3, 0));
    return 0;
}
```

Build the object, then the C source as well, because the object comes without headers and `ceesafe/rt.h` (the
types of `ceesafe_panic`) and `ceesafe_exports.h` are written beside the C:

```sh
ceesafec build
ceesafec build --emit c
cc -I out/gauge-c host/main.c out/gauge.o -o out/host
./out/host
```

```text
sum: 100
log: 55
squares: 55
squares: 0
percent: 25
board: stopped: division by zero at src/lib.cee:19:45
```

The exit status is 3, the one the stand-in chose. `gauge_squares(100)` asks for 400 bytes from a buffer of 64,
gets an `Err`, and returns 0 without touching `board_log`. The last call divides by zero, and the panic arrives
in the C function with its place in the CeeSafe source.

`nm -u out/gauge.o` shows what the object still needs. On macOS, where C names carry a leading `_`:

```text
_board_log
_ceesafe_panic
```

## Memory from the environment

A fixed buffer is enough for a function's working space. For values that live longer, an environment usually has
an allocator of its own, and you make it a CeeSafe allocator: a struct with no fields that implements the
interface `Allocator`. `Box.createIn` then takes it.

<span class="listing-caption"><strong>Listing 22-3</strong>, <code>main.cee</code>: An allocator over two functions of the environment, and a box made in it.</span>

<!-- ceesafe: freestanding -->
```ceesafe
extern "C" {
    fn board_alloc(size: usize, align: usize) -> *mut void;
    fn board_free(block: *mut void, size: usize);
}

/// The board's memory, reached through two C functions.
struct BoardHeap {}

impl Allocator for BoardHeap {
    /// Safety: board_alloc returns null or `size` bytes aligned to `align`.
    fn allocate(borrow this, layout: Layout) -> Result<*mut u8, AllocError> {
        let block = unsafe { board_alloc(layout.size, layout.align) } as *mut u8;
        if (block.isNull()) {
            return Err(AllocError.OutOfMemory);
        }
        return Ok(block);
    }

    /// Safety: a box returns each block once, with the layout it asked for.
    unsafe fn deallocate(borrow this, block: *mut u8, layout: Layout) {
        board_free(block as *mut void, layout.size);
    }
}

struct Reading {
    channel: u32;
    value: u64;
}

/// Keeps one reading in the board's memory for a moment, and returns its value: 0 when there is no memory.
@export("gauge_keep")
fn keep(value: u64) -> u64 {
    match (Box.createIn(BoardHeap {}, Reading { channel: 1, value: value })) {
        Ok(reading) => {
            return reading.value;
        }
        Err(e) => {
            return 0;
        }
    }
}
```

The box is freed through `deallocate` when `reading` goes out of scope, as any box is. This listing is checked
with the freestanding profile but not run here: it needs a `board_alloc`.

Only `Box` takes such an allocator. `Vector` and `String` grow inside the hosted runtime and are not available
in this profile at all: a freestanding package keeps sequences in arrays, in slices from a
`FixedBufferAllocator`, or in structures it builds from boxes.

::: tip Rule
In a freestanding package, memory is the stack, storage you are given, a `FixedBufferAllocator` over either, and
`Box.createIn` with an allocator you define. There is no default allocator.

*Specification: `profiles.md` §4.1; `type-system.md` §30.*
:::

## Building for another machine

`ceesafec` writes C, and the C compiler decides the processor. Pass it a target with `--cflag`, and name a linker
that can join objects for that target with `--ld`:

```sh
ceesafec build --cc clang --ld ld.lld --cflag --target=aarch64-unknown-none-elf -o out/gauge-arm.o
```

On the machine this chapter was written on (macOS on ARM, Apple clang 21 and LLVM's `ld.lld`) that writes an ELF
object for a 64-bit ARM processor with the same two undefined names, and `--target=x86_64-unknown-none-elf` does
the same for x86-64. Neither object was run: that takes the board or kernel it is for. Every freestanding build
adds `-ffreestanding -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables` by itself. A real
kernel has more flags of its own, and the rule is to take them from the kernel's build, each as a `--cflag`,
and not to copy them by hand.

An object for a Windows target is refused, since the linkers for Windows cannot join objects into one:

```text
ceesafec: --emit obj joins the program's objects with a relocatable link ('-r'), which the linkers for a Windows target do not have: give an ELF target, as in '--cflag --target=aarch64-unknown-none-elf', or build with '--emit lib'
```

What comes after the object is the environment's work: a linker script that places the code, start-up code that
sets a stack and calls an export, and the kernel's own `ceesafe_panic`. `profiles.md` §4.4 lists what such an
object is guaranteed not to contain (constructors, thread-local storage, unwind tables), which is what a kernel's
loader needs to know.

## Mistakes

::: warning Mistake: writing a freestanding package as a hosted program
`Console`, a `Vector` and a `main` are habits from every earlier chapter. Each is refused, at the place it is
named.
:::

<span class="listing-caption"><strong>Listing 22-4</strong>, <code>variants/hosted-names/src/lib.cee</code>: Three names that need an operating system, in a package whose manifest says <code>profile = "freestanding"</code>.</span>

<!-- ceesafe: skip -->
```ceesafe
/// Prints the readings above `limit`, and returns how many there are.
@export("report_above")
fn above(limit: u32) -> usize {
    var high = Vector<u32>.create();
    for (reading in [3u32, 9, 27]) {
        if (reading > limit) {
            Console.writeLine(reading);
            high.push(reading)!;
        }
    }
    return high.length;
}

fn main() {
    let _ = above(5);
}
```

```text
src/lib.cee:4:16: error[N0293]: 'Vector' needs the hosted runtime and is not available with --profile freestanding (profiles.md §4.1)
  |
4 |     var high = Vector<u32>.create();
  |                ^^^^^^^^^^^
src/lib.cee:7:13: error[N0293]: 'Console' needs the hosted runtime and is not available with --profile freestanding (profiles.md §4.1)
  |
7 |             Console.writeLine(reading);
  |             ^^^^^^^
src/lib.cee:14:4: error[N0293]: a freestanding package has no 'main': nothing calls it; enter it through @export functions (profiles.md §4.1)
   |
14 | fn main() {
   |    ^^^^
```

Count instead of collecting, report through an `extern "C"` function of the environment, and let the environment
call the export.

::: warning Mistake: linking without `ceesafe_panic`
The object compiles without it and the link fails, naming `ceesafe_panic` as an undefined symbol. Define it in
the environment, in C, with the signature above. A version that returns is worse than none: the compiled code
relies on the call never coming back, so what runs after it is undefined.
:::

::: warning Mistake: testing only as a hosted program
`ceesafec check` without the profile accepts a `Vector` that the freestanding build refuses. Put
`profile = "freestanding"` in the manifest, so that every command and your editor check the package as it is
built.
:::

::: details Coming from Rust
The profile is `#![no_std]` without `alloc`, and `ceesafe_panic` is the `#[panic_handler]`, written in C and
found at link time. There is no `#![no_main]`: a freestanding package never has a `main`. There is no
`GlobalAlloc` to register either, because the allocator is part of a box's type: `Box.createIn(BoardHeap {}, v)`.
:::

::: details Coming from C
This is `-ffreestanding` with the checks kept. The object is ordinary: one relocatable file, no constructors,
calls to `memcpy` and its three relatives at most. What you add to a C kernel's build is one function,
`ceesafe_panic`.
:::

## Summary

- `--profile freestanding`, or `profile = "freestanding"` in the manifest, builds for an environment with no
  operating system. `kind = "object"` or `--emit obj` makes the output one object file.
- The language and `lib/core` remain. `Console`, files, threads, the system heap and every type that allocates
  from it are `N0293`, and so is a `main`.
- The package is entered through `@export` functions and reaches out through `extern "C"` functions.
- The environment defines `ceesafe_panic`, which never returns.
- Memory is the stack, a `FixedBufferAllocator`, or `Box.createIn` with an allocator you define.
- `--cflag --target=…` with `--ld` builds for another processor. A Windows target is refused for objects.

Next, [Kernel code](./ch22-02-kernel-code) uses the profile for what it is meant for: a driver.
