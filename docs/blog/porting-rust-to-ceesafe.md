# Porting Rust to CeeSafe: Bun and the TypeScript checker

*Two trials that replaced crates of large Rust programs with CeeSafe, behind unchanged Rust APIs: what passed,
how fast it was, and what the ports changed in the language.*

CeeSafe has Rust's safety model (single ownership, moves, shared and exclusive borrows, no garbage collector) and
compiles to C. A claim that it can stand where Rust stands needs more than small examples, so we tried it on two
Rust code bases that other people wrote for production use:

- **[Bun](https://github.com/oven-sh/bun)**, the JavaScript runtime, 1.79 million lines of Rust at the commit we
  pinned, not counting blank lines and comments.
- **[tsrs](https://github.com/oxc-project/tsrs)**, a Rust port of the TypeScript 7 type checker, 24 MB of Rust in 38
  crates.

Neither was ported whole. Each trial is a proof of concept: a few crates swapped for CeeSafe inside the real program,
with the program's own tests as the judge. This post reports what they showed, including where CeeSafe was slower
and what it could not yet express.

## The method: one crate at a time, behind the same API

A Rust crate is replaced without touching the crates that use it:

1. The crate's code is ported to a CeeSafe package, file by file, with the same data structures.
2. The package is compiled with `ceesafec build --emit lib` to a static archive: the generated C and CeeSafe's
   runtime ([ADR 0088](../../spec/decisions/0088-embedding-ceesafe-in-a-host-program.md)).
3. The Rust crate keeps its public API and becomes a thin shim that calls the CeeSafe exports.
4. Rust and CeeSafe are linked into one binary, and the program's own test suite runs.

The interesting part is the boundary. Slices are lent for the length of a call, as pointer and length. An object
that outlives a call is a CeeSafe box that Rust holds by its address and gives back to be dropped. Each side frees
what it allocated. The one exception is a buffer that changes owner: Bun's zstd crate hands a `Vec<u8>` to CeeSafe,
which grows it and hands it back with no copy, because both sides were given the same allocator
([ADR 0090](../../spec/decisions/0090-buffers-that-cross-a-c-boundary.md)).

## Bun: two crates inside the runtime

Two of Bun's crates run as CeeSafe inside Bun: `bun_csrf` (HMAC tokens and base64 over BoringSSL) and `bun_zstd`
(compression and a streaming decoder over the zstd C library). CeeSafe starts without a `main` of its own, reports
panics through Bun's crash handler and allocates from Bun's allocator.

- **Bun's own tests for both crates pass,** in release builds and in debug builds under AddressSanitizer.
- **No measurable cost.** Release timings of the zstd paths equal stock Bun's within noise. The binary is 16.5 KB
  larger.

Those two crates are mostly calls into C libraries, so they say little about the speed of CeeSafe code itself. For
that we ported the one-shot path of Bun's wyhash, the function behind `Bun.hash()`, and timed it against Bun's Rust
in one program:

| Input | Rust | CeeSafe, safe code | CeeSafe, short keys unchecked |
|---|---|---|---|
| 8 bytes | 2.82 ns | 3.15 ns (+12%) | 2.88 ns (+2%) |
| 32 bytes | 3.20 ns | 3.73 ns (+16%) | |
| 1 KB | 38.97 ns | 39.20 ns (+0.6%) | |
| 1 MB | 43,017 ns | 43,215 ns (+0.5%) | |

Bun's Rust here is not safe Rust: it reads through raw pointers to avoid bounds checks. The CeeSafe in the middle
column has no unsafe code in the hash. It is equal from 1 KB up and 12 to 16% slower on short keys, where four reads
at length-dependent offsets keep their checks. Written with the same unchecked reads as the Rust, short keys come
within 2 to 5%.

This is the result we expected, and it is worth saying plainly: **on this hash, CeeSafe was not faster than Rust
at any size.** Rust
compiles through LLVM; CeeSafe compiles to C that clang compiles through the same LLVM. For arithmetic and memory
code the two meet at the same machine code, and parity is the ceiling.

## tsrs: data structures in the language itself

The second trial took code that is algorithms and data structures written in Rust, not wrappers around C:

- **`tsrs_spanmap`**, the map between a content mapper's virtual text and the original: binary searches, an interval
  tree, lookups in both directions.
- **Go's pdqsort** from `tsrs_core`, which the span map sorts with. tsrs ports Go's exact algorithm so that equal
  elements end in Go's order; the CeeSafe port is generic over the element and the comparison, and each sort is
  compiled for its comparison, as Rust's is.
- **The mappings encoder** of `tsrs_sourcemap`'s generator: the state machine and base64 VLQ behind every `.js.map`.

Besides the crates' own tests (31 and 40, all passing), each port has a differential test that holds the original
Rust file and asks both sides the same generated questions:

- The span map gave the same answers as the Rust to **909,360** questions, on maps of up to 5,000 segments, including
  unsorted and invalid ones.
- The generator produced the same results and the same source maps for **87,607** generated calls.

The `tsrs` binary built with the CeeSafe code type-checks a file and writes its source map, and the content-mapper
tests of the crates that use the span map pass.

Speed, in a release build against the original Rust compiled into the same test:

| Work | Rust | CeeSafe |
|---|---|---|
| Build the index of 2,000 segments (a sort and a tree) | 66 µs | 66 µs |
| A lookup, per call (four kinds) | 40–213 ns | 46–290 ns (13–37% slower) |
| Add a mapping to a source map | 17.5 ns | 24.5 ns (40% slower) |
| Build a map of 2,000 segments (512 different maps in turn) | 85 µs | 91 µs (5–9% slower) |

Where the work is inside CeeSafe, as in building the index, it is equal. Where a small function is called across
the boundary, it pays for
the call: the Rust is inlined into its caller by link-time optimisation, and the CeeSafe archive does not take part
in rustc's LTO. That is a cost of a hybrid build, not of the language, and a port of the calling crate would remove
it.

Building a map is the row to read with care. Timed as the others were, by building the same 2,000 segments
again and again, it was about twice as slow (27 µs against 50 to 59 µs), and when this was first written we had
not established why. We have since (2026-11-12, ceesafec 0.38.0): that timing measured the processor's branch
predictor more than the code. A sort of one input repeated thousands of times is a sequence of branches the
predictor can learn, how well depends on where the code happens to be placed in the binary, and the same machine
code took 24 to 33 µs in one binary and 42 to 50 µs in another; Rust's sort swings the same way, from 21 to 41 µs.
Given 512 different maps in turn, both take their real time, and the difference is small and steady: 85 µs for
the Rust and 91 µs for the CeeSafe, 5 to 9% slower, which is the two extra copies at the boundary. The sort alone,
on data that differs each time, takes the same time in both languages, within 2%.

Two rows of the report went the other way, and are not in the table because neither is a result about the
language: validating a 2,000-segment map was 17 to 21% faster in CeeSafe, which we did not investigate, and the
base64 data URL of a source map 25 to 31% faster, because the Rust pushes `char`s through an iterator into a
`String` where the port writes bytes into a slice.

## What the ports changed in CeeSafe

A gap found during a port is fixed in the compiler, through a decision record, and not worked around at every use.
These came out of the two trials:

| Found in | Gap | Now |
|---|---|---|
| Bun | No way to run inside a host program with its allocator and crash handler | `--emit lib` and the embedding interface ([ADR 0088](../../spec/decisions/0088-embedding-ceesafe-in-a-host-program.md)) |
| Bun | A vector's buffer could not change sides | `Vector.intoForeign` and `fromForeign` ([ADR 0090](../../spec/decisions/0090-buffers-that-cross-a-c-boundary.md)) |
| Bun, wyhash | No high half of a 64-bit product. The hand-written form was 2.5 to 3.8 times slower than Rust with Apple clang 21 | `x.mulHigh(y)`, one instruction with any C compiler ([ADR 0092](../../spec/decisions/0092-the-high-half-of-a-product.md)) |
| Bun, wyhash | No read through an unaligned raw pointer | `p.readUnaligned()` ([ADR 0093](../../spec/decisions/0093-unaligned-reads-and-writes-through-a-raw-pointer.md)) |
| tsrs | Nothing like Rust's `OnceLock`, so an index built on first use had to stay in the Rust shim | `OnceCell` and `OnceLock` ([ADR 0096](../../spec/decisions/0096-values-initialised-once.md)) |
| tsrs | `impl dep.Interface for T` was refused | accepted ([ADR 0097](../../spec/decisions/0097-a-dependencys-interface-named-in-an-impl.md)) |

The last two shipped in ceesafec 0.35.0, and the tsrs trial was then changed to use them: the span map's index is
built once inside CeeSafe, and the Rust shim lost its own `OnceLock`.

Some differences are by design and stayed. Arithmetic is checked, so an overflow panics where Rust's release builds
wrap. Shifts take unsigned operands. There are no tuples, so Rust's pairs became small structs. And one accepted
design did not fit: a method cannot return `Result<borrow T, E>` when it has two reference parameters, so
`OnceLock` has `tryInit` where Rust has `get_or_try_init`.

## What this does not show

- **A whole port.** Six pieces of two programs are not 1.79 million lines. Bun's core (the JavaScriptCore
  bindings, the event loop, the bundler) and tsrs's checker are not ported, and the traits, generics, macros and
  async code they are built from were not exercised at volume. The compiler was tested on generated programs of up
  to 707,000 lines; a run at 1.5 million was not completed.
- **Windows and WebAssembly inside these programs.** Both trials build for Unix only.
- **The full suites.** Bun's tests for the two crates ran; tsrs's conformance suite needs a TypeScript checkout the
  trial did not have.
- **Faster than Rust.** Nothing here supports that, and we do not expect it: of the two faster rows of the tsrs
  report, one comes from how that Rust was written and the other was not investigated.

## What it does show

A crate of a production Rust program can be replaced by CeeSafe without its users noticing: the same API, the
program's own tests passing, the same answers on about a million generated cases, and, where the work is done
inside CeeSafe, Rust's speed or close to it: equal for the hash from 1 KB up and for the index
build and for the sort, and 12 to 16% behind on short keys. What the host program receives is C, compiled by the C compiler it already uses.

The reports, with the numbers above and how they were measured, are in the
[CeeBun repository](https://github.com/CeeSafe/CeeBun) under `analysis/trials/` (the compiler's scale test under
`analysis/scale/`, the line count in its `ROADMAP.md`); the map build and the sort are measured in
[this repository's investigation](../performance-investigation-2026-11.md), §10. The Bun trial's code is at
[CeeSafe/ceebun-trial](https://github.com/CeeSafe/ceebun-trial). For the language itself, start with
[CeeSafe for Rust programmers](../../guide/coming-from/rust.md).
