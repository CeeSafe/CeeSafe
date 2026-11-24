# ADR 0241 — `cbind` writes C's `long` and `unsigned long` as `CLong` and `CULong`

- Status: **Accepted** by the owner, 2026-11-24 ("Yes", to "a short ADR making `cbind` write `CLong` and `CULong`, so
  a zlib binding can be published that also works on Windows"). Raised by the optimization and DX roadmap's Phase 4,
  item 1: a binding to be published in the package index (`CeeSafe/CeeSafe-index`) must hold on every target.
- Number: written as ADR 0221 on branch `dx/roadmap-phase0` (its commits name it so); renumbered 0241 when the
  branch was merged with `main`, which had given 0221 to another ADR.
- Scope: tooling.md §11.2's types; `src/tooling/cbind.cee` and its oracle `tools/cbind.py`; `tools/cbind_check.py`
  and its fixture; `tests/cbind/zlib`. No language change: `CLong` and `CULong` exist since ADR 0080 §3.5.

## 1. Context

`ceesafec cbind` (ADR 0095, ADR 0127) maps C's types by type-system.md §28.3's table, and tooling.md §11.2 fixes two
of them by the width they have on Linux and macOS: `unsigned long` is `usize` and `long` is `i64`. Both are 64 bits
there (LP64) and 32 on Windows (LLP64), where `usize` and `i64` stay 64.

Found by `tools/cbind_zlib_check.py`: zlib's API is in `uLong` (`typedef unsigned long uLong`), so its binding reads
`fn crc32(crc: usize, buf: *u8, len: u32) -> usize;`. On Windows that declaration does not match the header's, and a
program over the binding passes and receives values of the wrong width. A binding written on Linux therefore cannot be
published for every target, and one written on Windows, if cbind ran there, would differ from it.

Since ADR 0080 §3.5 CeeSafe has `CLong` and `CULong`: C's `long` and `unsigned long`, of the target's width
(type-system.md §28.3 says how they convert). cbind's table was written without them.

## 2. Decision

- `long` is written `CLong` and `unsigned long` `CULong` (clang's AST spells `long int` and `signed long` as `long`). In
  every position: a parameter, a result, a struct member, extern data, a pointer's or an array's element, a function
  pointer's types, an integer macro whose C type is one of them.
- An `_Atomic long` or `_Atomic unsigned long`, which was `AtomicU64` or `AtomicUSize` (wrong on Windows too), is
  left out with a comment, as any declaration CeeSafe cannot spell is: there is no atomic of `CLong` or `CULong`.
- `long long` stays `i64` and `unsigned long long` `u64` (64 bits on every target C has), and `size_t`, `uintptr_t`,
  `intptr_t`, `ptrdiff_t` and `ssize_t` stay `usize` and `isize`.
- Both implementations, the command and `tools/cbind.py`, change together: `tools/cbind_check.py` requires the same
  bytes from both.

## 3. Consequences

- A program over such a binding converts at the boundary: `n as CULong` where that is lossless on the target it is
  checked for, else `checkedTruncate<CULong>(n)` (type-system.md §28.3; a program's validity can depend on the host,
  as ADR 0080 §3.5 says). `CULong as u64` and `CLong as i64` are lossless everywhere.
- A binding written before this ADR keeps compiling where it did; cbind writes the new spelling from now on.

## 4. Testing

- `tests/cbind/fixture.h` gains a function of `long` and `unsigned long`, and `tools/cbind_check.py` expects
  `CLong` and `CULong` for it, written from this ADR; the program over the fixture's binding calls it, so the C
  compiler checks the declaration against the header.
- `tools/cbind_zlib_check.py` builds and runs zlib's binding as before, with the program converting its sizes.

## 5. Implementation notes (2026-11-24)

- `cbScalar` (src/tooling/cbind.cee) and `SCALARS` (tools/cbind.py) map `long` and `unsigned long` to `CLong` and
  `CULong`; an integer macro of either type is a constant of it (`#define FX_LONG 7L` is `const FX_LONG: CLong = 7;`),
  in both. An enum whose fixed underlying type is `long` is sized by its values, as one with no fixed type is.
- `tests/cbind/fixture.h` gained `fx_span(long, unsigned long) -> unsigned long` and the macros `FX_LONG` (`7L`) and
  `FX_ULONG` (`9UL`); `tests/cbind/use.cee` calls `fx_span(FX_LONG, FX_ULONG)`, so the C compiler checks the
  declaration against the header (its exit status is still 47). The zlib program converts its sizes to `CULong` with
  `checkedTruncate` and back with `as`.
- **Verified** (Mac, 2026-11-24): `tools/cbind_check.py`, 66 declarations, 0 problems (9 with the compiler before the
  change); `tools/cbind_zlib_check.py`, 0 problems; `spec_check`, 0 problems; the fixed point. Not run on Windows,
  where the change matters most: no Windows host in this session.

