# Kernel memory safety without a second toolchain

*How CeeSafe's approach to kernel safety differs from Rust's: the same ownership model, delivered as warning-clean
C11 that the kernel's own build compiles.*

Rust has shown that ownership and borrowing can remove whole classes of memory bugs from kernel code, and Rust in
the Linux kernel is no longer an experiment. This post takes that result as given. The question it asks is
narrower: what does a C kernel have to take on to get it?

With Rust, the answer includes a second compiler. Rust code is compiled by rustc through LLVM, beside the kernel's C
compiler, which for most Linux builds is GCC, and the kernel's C headers reach Rust through bindings that bindgen
generates with libclang. The kernel now has two toolchains to pin, two sets of target and code-generation options
to keep in agreement, a copy of its struct layouts that a tool derives from the headers, and Rust code only on the
architectures LLVM targets. A GCC front end for Rust and a GCC backend for rustc are being built to close that gap.
That is a great deal of work spent on the toolchain question alone.

CeeSafe is a memory-safe systems language that makes a different trade. Its safety model is Rust's: single
ownership, moves, shared and exclusive borrows, deterministic destruction, no garbage collector. Its compiler,
`ceesafec`, does not produce machine code. It produces C11, and the kernel's existing C compiler, with the kernel's
existing flags and headers, does the rest.

This post describes what that buys, what it costs, and what we have measured so far.

## What the kernel build receives

`ceesafec build --profile freestanding --emit obj` writes one relocatable object. From the kernel build's point of
view, CeeSafe code is C files compiled with its own `CFLAGS`. The C backend's contract
([spec/c-backend.md](../../spec/c-backend.md) §1) says what those files are:

- **C11**, also valid as C17, including only freestanding headers (`<stdint.h>`, `<stddef.h>`, `<stdbool.h>`,
  `<stdalign.h>`, `<limits.h>`, `<stdnoreturn.h>`) and CeeSafe's `rt.h`.
- **Warning-free** under `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror`, and under the
  host kernel's own warning set as well. For CosmoOS, the kernel CeeSafe's kernel support was developed against,
  that adds `-Wcast-qual -Wmissing-prototypes -Wstrict-prototypes -Wvla -Wundef -Wimplicit-fallthrough` and
  `-Wframe-larger-than=2048`.
- **No compiler support library.** The generated code never needs compiler-rt or libgcc, so there is no 128-bit
  division, no `long double` and no outline-atomic helper.
- **Correct with and without `-fno-strict-aliasing`.** Kernels pass it; the generated C does not depend on it.

"Warning-clean" is not decoration. A kernel that builds with `-Werror` and a strict warning set will not accept
generated code that is merely valid, and generated code that needs its own relaxed flags is a second toolchain in
disguise.

The object's undefined symbols are a short, fixed list ([spec/profiles.md](../../spec/profiles.md) §4.4):
`ceesafe_panic`, `memcpy`, `memmove`, `memset`, `memcmp`, and the functions the package itself declares in
`extern "C"` blocks. On the kernel targets the object has no constructor sections, no thread-local storage, no
unwind tables, no COMMON symbols, and no mutable global state other than the program's own statics. A panic is a
call to `ceesafe_panic(kind, location)`, which the kernel defines; the CosmoOS binding passes it to the kernel's
`panic`. The freestanding core runtime is a header of about 300 lines and a few C functions.

The compiler has the same property. `ceesafec` is written in CeeSafe, and every release carries a C seed, the
compiler's own generated C, which one `cc` command turns into a working compiler on a platform that has no release
build.

## Struct layouts come from the kernel's headers

A kernel's structs change with its configuration. `CONFIG_*` options add and remove fields, and a debug build's
`spinlock_t` is not a release build's.

CeeSafe never writes those layouts down. An `extern "C"` block names the header and the fields the CeeSafe code
uses:

```ceesafe
@header("kernel/timer.h")
extern "C" {
    @ctype("struct timer")
    struct timer;
    @effects()
    fn timer_start(t: *mut timer, delay_ns: u64);
}
```

The generated C `#include`s that header and accesses the real members, compiled with the build's own flags. The
layout is therefore always the one of this configuration and architecture, because the same C compiler computed it
from the same header. Each declared field is checked at C compile time: it exists and has exactly the declared
type. A configuration that changes a field fails the build and names the field, where a stale binding would
miscompile.

This is possible only because the output is C. A compiler that emits machine code must know the layout itself, so
it needs a tool that reads the headers the way the C compiler would and a copy of the result.

## Context rules are checked by the compiler

Memory safety is not the only rule kernel code lives under. Code holding a spin lock must not sleep. An interrupt
handler must not sleep or allocate. Ownership does not express either.

CeeSafe checks them as effects. Every function's effects, `sleep` and `alloc`, are inferred from what it does and
calls, destructors included. `@effects(…)` declares what a C function may do, and `@context(atomic)` or
`@context(irq)` says what a function, or the holder of a guard, must not do:

```ceesafe
@header("kernel/sched.h")
extern "C" {
    @effects(sleep)
    fn cosmo_sleep_ms(ms: u32);
}

/// Holding a spin-lock guard makes the code that runs meanwhile atomic: it must not sleep.
@context(atomic)
struct SpinGuard { cpu: u32; drop {} }

/// Safety: cosmo_sleep_ms has no preconditions; it may sleep.
fn backOff() { unsafe { cosmo_sleep_ms(1); } }

fn update(counter: borrow mut u64) {
    let guard = SpinGuard { cpu: 0 };
    counter += 1;
    backOff(); // sleeps while the guard is held
}
```

```text
main.cee:17:5: error[N0295]: a 'SpinGuard' is live here, which makes this an atomic context (type-system.md §32.3), but this may sleep: it calls 'backOff', which calls 'cosmo_sleep_ms', declared @effects(sleep)
```

The error follows the call chain to the C function that sleeps. The same mechanism covers destructors: `KRef<T>`
is a reference-counted kernel object whose final release may sleep, so dropping one under a spin lock does not
compile.

Rust has no notion of "may sleep" in the language. For kernel Rust the same rule is checked by klint, a separate
lint tool over annotated code. The difference is where the check lives: in CeeSafe it is part of the type checker
that every build runs, with the effects inferred, and it is stated in the language specification.

The limit is stated in the guide and belongs here too: the check is only as good as the `@effects` written on the
C declarations, and it says nothing about deadlocks or lock ordering. The kernel's lockdep remains the check for
those.

## Other decisions that differ from Rust

- **Allocation is always explicit and always fallible.** Every allocating type takes its allocator as a type
  parameter, and every allocation returns a `Result`. There is no infallible `push` to forbid. In a kernel,
  `Box.createIn(KernelHeap {}, value)` allocates over `kmalloc` and returns `Err` when the kernel has no memory.
- **Panics abort, and overflow is checked in every build.** There is no unwinding, so no unwind tables and no
  unwinder in the kernel, and no build mode in which integer overflow wraps silently.
- **The unsafe contract is required.** An `unsafe fn`, and any function containing `unsafe`, must carry a
  `Safety:` doc line or it does not compile (N0283). The compiler checks that the line exists, not that it is true.
- **Less annotation, a simpler checker.** A function returning a reference names the parameters it borrows from,
  `-> borrow(a) T`, and declares no lifetime parameters. The borrow checker is intraprocedural and keeps one set of
  loans per value.

## What this costs

The last point above is also the first cost.

- **The checker rejects programs Rust accepts.** The usual fixes are to end a borrow earlier, copy a value out, or
  use an index or a handle. There are no macros, no iterator adapters, no blanket impls and no `impl Trait`.
- **The C compiler is in the trusted base.** Emitting C means emitting C with no undefined behaviour, by
  construction: checked arithmetic, checked shifts and division, explicit widths for narrow arithmetic, no plain
  `char` for data. The backend keeps a list of these hazards and the corpus runs under ASan and UBSan, but this is
  a discipline to maintain, where rustc controls its IR directly.
- **No soundness proof.** Rust's model has years of production use and formal study behind it. CeeSafe has a
  specification, a reference interpreter that runs the same programs, and tests.
- **Narrower than "any C compiler" in the kernel today.** Hosted programs build with clang, gcc and, best effort,
  MSVC. The kernel profile has been built and tested with clang and lld only, because that is what CosmoOS uses.
- **No Linux integration yet.** The mechanisms are not specific to CosmoOS: flags come from the codebase's build,
  types from its headers, symbols from its exports. A Linux kernel-module profile is a future integration and is not
  scheduled.
- **A young language and a small ecosystem.** There is no crates.io to draw on.

## What has been measured

The record is in [docs/cosmoos-integrations.md](../cosmoos-integrations.md), from runs on 2026-10-31 against an
unchanged CosmoOS tree:

- On x86-64 and AArch64, CeeSafe objects pass nine of nine module checks under the kernel's `MODULE_CFLAGS`: the
  loader's ELF rules, relocation types, kernel symbols only, no constructors or mutable state, stack frames of at
  most 2,048 bytes, and no floating-point registers.
- A signed CeeSafe module boots in the x86-64 kernel and passes its in-kernel tests with no block left allocated.
- Ports of existing C components (an LZ4 decoder, a framebuffer validator, a GPT parser) pass CosmoOS's own host
  tests and fuzz targets, and differential tests against the C originals: 20,000 cases each, no mismatches.
- The lock and kernel-object bindings ran a stress test on four CPUs in the debug kernel with no lockdep report,
  and a deliberate lock inversion was reported, as a control.

AArch64 boots were not part of that record.

On speed, with every check on, CeeSafe runs sixteen user-space workloads in 0.96× Rust's time (geometric mean) and
0.88× that of Rust built with overflow checks; hand-written C without checks takes 0.94×. CeeSafe is ahead where
its compiler removes checks or allocations and behind by up to 1.41× on a hash map whose bounds checks it cannot
yet remove. [guide/performance.md](../../guide/performance.md) has the table, the method and the losses. These are
not kernel benchmarks.

## Summary

Rust brings a kernel memory safety and asks it to adopt a compiler. CeeSafe brings the same ownership model, a
weaker borrow checker and a much smaller ecosystem, and asks the kernel to compile some more C: C that passes its
warning set, reads its headers, links against a five-symbol runtime, and maps back to source lines in its
debugger. For a kernel whose build, targets and review process are organised around a C compiler, that is a
different adoption cost, and it is the one CeeSafe was designed to minimise.

## Earlier discussion

The Rust side of this comparison has been argued at length on Hacker News, and those threads are better sources
for it than this post:

- [Rust in the kernel is no longer experimental](https://news.ycombinator.com/item?id=46213585)
- [Rust kernel policy](https://news.ycombinator.com/item?id=42998375), with the version and unstable-feature policies
- [Klint: compile-time detection of atomic context violations for kernel Rust code](https://news.ycombinator.com/item?id=35207157)
- [Supporting Linux kernel development in Rust](https://news.ycombinator.com/item?id=24334731), an early thread on
  bindgen, GCC and architecture coverage
- [Ask HN: C with a borrow checker](https://news.ycombinator.com/item?id=26511830)

CeeSafe is GPL-2.0-only, with an exception for the runtime and library code compiled into programs. The
[guide's kernel chapter](../../guide/kernel.md), the [profiles specification](../../spec/profiles.md) and
[CeeSafe for Rust programmers](../../guide/coming-from/rust.md) go further than this post.
