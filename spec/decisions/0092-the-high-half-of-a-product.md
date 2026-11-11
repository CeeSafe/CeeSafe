# ADR 0092 — The high half of a product: `x.mulHigh(y)`

- Status: **Accepted** by the owner, 2026-11-11 ("I accept both ADRs"), every question as proposed: the name
  `mulHigh`, signed types too, `usize` and `isize` at the target's width. Not implemented yet. Raised by the Bun port's timing of wyhash against Rust
  (`~/dev/CeeSafeApps/bun/analysis/trials/wyhash/REPORT.md` §4, gap 1).
- Scope: `spec/type-system.md` §6 (one integer method), the checker's integer methods, the C emitter and `rt.h` (one
  inline function per width), the interpreter. No library or runtime-library change.
- Release: the next minor release, in one stage.

## 1. Context

- **What programs need.** Hash functions (wyhash, and the multiply-and-fold family), multi-word arithmetic, fixed-point
  scaling and division by a constant all need the high 64 bits of a 64 × 64 product. Every 64-bit CPU has it as one
  instruction (`umulh` on AArch64, the upper register of `mul` on x86-64).
- **What CeeSafe has.** `x.wrappingMul(y)` gives the low half. There is no 128-bit integer and no way to ask for the
  high half, so a program builds it from four 32-bit products:

  ```ceesafe
  fn mulHigh(a: u64, b: u64) -> u64 {
      let al = a & 0xFFFF_FFFF;
      let ah = a >> 32;
      let bl = b & 0xFFFF_FFFF;
      let bh = b >> 32;
      let ll = al * bl;
      let lh = al * bh;
      let hl = ah * bl;
      let hh = ah * bh;
      let mid = (ll >> 32) + (lh & 0xFFFF_FFFF) + (hl & 0xFFFF_FFFF);
      return hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
  }
  ```

- **What that costs depends on the C compiler.** Measured on an M1, Bun's wyhash ported to CeeSafe against Bun's Rust,
  nanoseconds per call:

  | Input | Rust | CeeSafe, clang 23 | CeeSafe, Apple clang 21 |
  |---|---:|---:|---:|
  | 16 bytes | 2.83 | 3.18 | 7.44 |
  | 1 KB | 38.97 | 39.20 | 149.21 |
  | 1 MB | 43,017 | 43,215 | 143,618 |

  clang 23 recognises the pattern and emits one `umulh`. Apple clang 21 does not: it emits the four multiplies, and the
  hash is 2.5 to 3.8 times slower than Rust's. The same source, the same flags. gcc and MSVC were not measured.
- **Rust** writes `((a as u128) * (b as u128)) >> 64`, which every backend lowers to the instruction.

## 2. Proposal

### 2.1 The method

`x.mulHigh(y)`, on every fixed-width integer type and on `usize` and `isize`: the upper half of the full product of
`x` and `y`, as a `T`. For `T` of `n` bits it is `floor(x × y / 2^n)`, computed exactly; for signed `T` the product
is signed and the result rounds towards negative infinity, as an arithmetic shift does.

- It takes one argument of the receiver's type, and a flexible literal adapts, as `wrappingMul` does.
- It cannot overflow and cannot panic.
- `x.wrappingMul(y)` is the lower half, so the pair is the whole product:
  `mix(a, b) = a.wrappingMul(b) ^ a.mulHigh(b)`.

### 2.2 The C

One `static inline` function per width in `rt.h`, so the generated C does not depend on a pattern being recognised:

| Width | Emitted |
|---|---|
| 8, 16, 32 bits | the product in the next wider type, shifted |
| 64 bits, GCC and clang (`__SIZEOF_INT128__`) | `(uint64_t)(((unsigned __int128)a * b) >> 64)`, `__int128` for signed |
| 64 bits, MSVC x64 and ARM64 | `__umulh(a, b)` and `__mulh(a, b)` (`<intrin.h>`) |
| 64 bits, anything else | the four 32-bit products above |

The freestanding runtime uses the same function: `__int128` multiplication needs no helper library on a 64-bit
target.

### 2.3 The interpreter

The four 32-bit products, for every width, so native and interpreted results agree by construction.

## 3. Alternatives considered

- **Nothing: rely on the C compiler.** It works with clang 23. It costs up to 3.8 times with Apple clang 21, the
  default compiler on macOS, and nothing tells the programmer which they got.
- **A `u128` type.** The general answer, and Rust's. It touches the type system, constants, formatting, parsing,
  `readLE`, the C ABI (MSVC has no `__int128`) and the interpreter. Too large for what is asked; this method does not
  stand in its way.
- **`x.wideningMul(y)` returning both halves.** One call instead of two. It needs a result type (a struct or a
  tuple) for every width, and C compilers already merge `a * b` and the high half into one multiply instruction on
  x86-64. The two existing halves compose.
- **A runtime function in `lib/std`** (as ADR 0089 did for CRC-32C). A function call per multiply defeats the
  purpose; this has to inline.

## 4. Not in this ADR

- Carry and borrow (`carryingAdd`, `borrowingSub`), which multi-word arithmetic also wants.
- `u128`.

## 5. Testing

- **Corpus, run-pass:** every width, signed and unsigned, against values computed independently (the corners: 0, 1,
  the maximum, the minimum, −1; and random pairs checked against the four-product form), native and interpreted.
- **Corpus, check-fail:** a float receiver; an argument of another type.
- **The C:** the fixture compiled with clang, gcc and MSVC (the Windows VM); on AArch64 and x86-64 the 64-bit case
  disassembles to one multiply instruction at `-O2`.
- **Evidence rerun:** the wyhash harness with `mulHigh` in place of the hand-written function, built with Apple
  clang 21: within a few percent of Rust, as with clang 23.

## 6. Questions for the owner

1. **The name `mulHigh`.** It follows `wrappingMul`, `checkedMul`. **Proposed: yes.**
2. **Signed types too.** Division by a constant uses the signed form. **Proposed: yes.**
3. **`usize` and `isize`**, at the target's width. **Proposed: yes.**
