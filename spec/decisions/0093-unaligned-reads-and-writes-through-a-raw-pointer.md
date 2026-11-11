# ADR 0093 — Unaligned reads and writes through a raw pointer

- Status: **Accepted** by the owner, 2026-11-11 ("I accept both ADRs"), every question as proposed: done at low
  priority, with the names `readUnaligned` and `writeUnaligned`. Deferred after 0.32.0 and taken up again on
  2026-11-11 ("implement all three"). **Implemented** (§6). Raised by the Bun port's timing of wyhash against Rust
  (`~/dev/CeeSafeApps/bun/analysis/trials/wyhash/REPORT.md` §4, gap 2).
- Scope: `spec/unsafe.md` §1 and its table of requirements, `spec/type-system.md` §27.3, the checker's pointer
  methods, the C emitter, the interpreter. No library or runtime change.
- Release: the next minor release, in one stage. **Small, and optional:** §1 says what works today.

## 1. Context

- **The rule today.** `p.read()` and `p.write(v)` need `p` aligned for `T` (`spec/unsafe.md`). A `u32` or a `u64` at
  an arbitrary byte offset, which is what a parser, a hash function or a packed C struct has, cannot be read through
  a `*u32` or `*u64`.
- **What works today, for integers.** `readLE<T>` and `readBE<T>` on a byte slice read at any offset, with a bounds
  check. Without the check, unsafe code makes a slice of exactly the width first:

  ```ceesafe
  /// Safety: the caller proves `at + 4 <= input.length`.
  unsafe fn read4Unchecked(input: Slice<u8>, at: usize) -> u64 =>
      Slice.fromRaw(input.ptr.add(at), 4).readLE<u32>(0) as u64;
  ```

  The slice's length is a constant, so the C compiler removes the check and emits one load. In the wyhash port this
  brought 8-byte keys from 12% slower than Rust to 2% (clang 23).
- **What does not work.** The idiom is indirect: the reader has to know that a four-byte slice read at 0 is a load.
  It covers integers only, in a named byte order. A `f32`, a pointer or a `@repr(C)` struct at an unaligned address
  (a packed wire format, a C library's unaligned field) has no read at all, short of copying bytes into a local
  through a second pointer.
- **Rust** has `ptr::read_unaligned` and `ptr::write_unaligned`; C has `memcpy` into a local.

## 2. Proposal

Two unsafe methods on raw pointers, beside `read` and `write`:

| Method | Does | Requirement |
|---|---|---|
| `p.readUnaligned()` on `*T` or `*mut T` | the `T` at `p`, in the machine's byte order | as `p.read()`, without the alignment; `T` is Copy (N0282) |
| `p.writeUnaligned(v)` on `*mut T` | stores `v` at `p` | as `p.write(v)`, without the alignment; `T` is Copy (N0282) |

- **Copy only.** A value with drop glue read this way would have two owners, as with `p.read()`, and an unaligned
  place cannot be borrowed afterwards to repair it.
- **The C:** `memcpy` between `p` and a local of type `T`, which compilers lower to one load or store and which is
  valid under strict aliasing. What the freestanding build uses for it is settled in the implementation.
- **The interpreter:** reads and writes the value's bytes at the address, whatever its alignment.
- **Byte order.** The machine's, as `p.read()`. A format with a fixed order keeps `readLE`/`readBE`, or applies
  `byteSwap`.

## 3. Alternatives considered

- **Nothing.** The slice idiom costs nothing at run time for integers, and CeeSafe code rarely holds unaligned
  structs. This is a real option: the ADR adds convenience and the non-integer cases, not speed.
- **Drop the alignment requirement from `p.read()` and `p.write()`.** Every read through a pointer would become a
  `memcpy`, which compilers lower well, but an aligned read is a promise the optimiser and the sanitizers use, and
  `Ptr.toBorrow` would still need alignment.
- **`readLE`/`readBE` on raw pointers.** Covers the integer case by name, and not floats, pointers or structs.

## 4. Testing

- **Corpus, run-pass:** each integer width, `f32`, `f64`, a raw pointer and a `@repr(C)` struct, read and written at
  every offset 0 to 7 of a byte buffer, native and interpreted, compared with the bytes.
- **Corpus, check-fail:** outside `unsafe`; a type that is not Copy (N0282); `writeUnaligned` through a `*T`.
- **Sanitizers:** the run-pass file under UBSan (`-fsanitize=alignment`) reports nothing, where the same reads with
  `p.read()` do.
- **The C:** at `-O2` a `u64` read disassembles to one load on AArch64 and x86-64.

## 5. Questions for the owner

1. **Do it at all?** The integer case has a zero-cost idiom today. **Proposed: yes, at low priority**, for the
   non-integer cases and for code that reads as what it does.
2. **The names `readUnaligned` and `writeUnaligned`.** Rust's, in CeeSafe's spelling. **Proposed: yes.**

## 6. Implementation notes (2026-11-11)

On branch `impl/0093`, on 0.33.0.

- **The built-ins** are `PtrReadUnaligned` (139) and `PtrWriteUnaligned` (140), checked beside `readVolatile` and
  `writeVolatile` (`src/check/unsafe.cee`): unsafe (N0280), a Copy pointee (N0282), a `*mut T` to write through
  (N0282). §2 named N0259 for the Copy rule; the code is N0282, the one the volatile forms report.
- **`rt.h`:** `cee_read_unaligned(T, p)` and `cee_write_unaligned(T, p, v)`, expressions. Each copies `sizeof(T)`
  bytes with `ceesafe_mem_copy` (`__builtin_memcpy` on GCC and clang, `memcpy` elsewhere, which every profile
  provides) between `p` and the element of a compound literal `(T[1]){…}`. An array of one takes an initialiser of
  type `T` whether `T` is a scalar or a struct. `p` is cast to a `char` pointer first: clang's alignment sanitizer
  checks a `memcpy` argument by its static type, and reported the first form.
- **The interpreter** loads and stores at the address without `checkAligned`; every other check of a raw access
  stays.
- **Effects:** `writeUnaligned` has `write`'s effect.
