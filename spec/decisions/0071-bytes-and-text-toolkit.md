# ADR 0071 — Bytes and text: bulk appends, byte order, bit rotation, searching and number parsing

- Status: **Accepted** by the owner, 2026-11-05 ("Accept as proposed, implement and release 0.15.0": §6 as proposed:
  generic byte-order methods, the bit counts included, one release). Implemented with the changes listed in §7, and
  released as 0.15.0. Raised by the work on a Postgres wire client, later SMTP/IMAP/MIME (notes from that session,
  passed on by the owner).
- Scope: type-system.md §6 (integer methods), §16.5 (slice bulk operations), §22.2 (`StringView`), §23.1
  (`Vector<T, A>`), §35 (floats); `lib/core/` (new `bytes.cee`, `text.cee`, `numbers.cee`); `src/check/vectors.cee`
  and `src/check/library.cee` (the method tables); the interpreter; `runtime/hosted/ceesafe_rt_float.c` (float
  parsing); the corpus.

## 1. Context

A wire-protocol client spends its time turning values into bytes and bytes back into values. Today, every one of
those steps is hand-written:

- **Appending bytes.** `Vector<T>` has `push` and nothing bulk. An encoder pushes byte by byte, or pushes zeros and
  then `copyFrom`s over them. This is the hottest path in such a client.
- **Byte order.** Every Postgres message has big-endian lengths and fields. Each read or write is four shifts,
  four index checks and a `truncate`.
- **Bit rotation.** SHA-256 and MD5 (for SCRAM and the md5 login) need `rotateLeft`/`rotateRight`. `ownership.md`
  §0 says "`wrappingShl` masks", but no such method exists: the integer methods are the nine of §6 plus `min`, `max`,
  `abs` and `unsignedAbs`.
- **Searching.** `StringView` and `Slice<u8>` have no `indexOf`, `startsWith`, `endsWith`, `split` or `trim`. SCRAM
  messages (`r=…,s=…,i=…`), command tags (`INSERT 0 1`), and all of SMTP, IMAP and MIME need them.
- **Numbers from text.** Postgres sends results as text. Integers are easy to hand-roll, but parsing a float with
  correct rounding is not. The runtime already formats floats exactly (§35.7); nothing reads them back.

What stays out (§4): hashes, HMAC, PBKDF2, hex and base64 belong in packages, and TLS is a separate decision.

## 2. Proposal

Every addition is a method on a built-in type, like `sortBy` and `splitAt` (§36.4, `lib/core/slices.cee`). The
compiler's method tables map each name to a `lib/core` function written in CeeSafe. Only float parsing is in the C
runtime, linked only when used, next to the float formatter. The names become reserved on their types, as the
integer methods are (N0228).

### 2.1 Bulk appends on `Vector<T, A>` (§23.1)

For a Copy `T` (N0259 otherwise):

| Method | Result | What it does |
|---|---|---|
| `v.extend(s)` (`s: Slice<T>`) | `Result<void, AllocError>` | Appends all of `s`, with at most one growth. On failure `v` is unchanged |
| `v.resize(n, fill)` | `Result<void, AllocError>` | Grows to `n` with copies of `fill`, or truncates to `n` |
| `v.reserve(extra)` | `Result<void, AllocError>` | Makes room for `extra` more elements without changing the length |

- They take `borrow mut this` and are two-phase (B5), like `push`.
- `v.extend(v.view())` is rejected (N0240), as `s.append(s.view())` is for strings.
- `String` keeps `append`. It gets no `extend`, because its bytes must stay UTF-8.

### 2.2 Byte order

There are generic forms over the integer types, so the names don't multiply:

| Method | On | Result |
|---|---|---|
| `s.readBE<T>(at)`, `s.readLE<T>(at)` | `Slice<u8>`, `StringView` bytes, a vector | `T` |
| `s.writeBE(at, x)`, `s.writeLE(at, x)` | `SliceMut<u8>`, a mutable vector | `void` |
| `v.pushBE(x)`, `v.pushLE(x)` | `Vector<u8, A>` | `Result<void, AllocError>` |
| `x.byteSwap()` | every integer type | `T` |

- `T` is any integer type, and `x`'s type gives the width for writes and pushes. A flexible literal is N0260 (no
  width), so the code writes `v.pushBE(5u32)` or `v.pushBE(length)`.
- Each call checks bounds once (`at + size(T) ≤ length`), and panics "index out of bounds" at the call otherwise.
- Signed types read and write two's complement.
- The generated C is a `memcpy` and a byte swap where the host's order differs, with no undefined behaviour for
  unaligned access.

A Postgres message header is then:

```ceesafe
out.pushBE(0x51u8)?;                          // 'Q'
let at = out.length;
out.pushBE(0u32)?;                            // length, patched below
out.extend(sql.bytes())?;
out.pushBE(0u8)?;
let length = truncate<u32>(out.length - at);
out.viewMut().writeBE(at, length);
```

### 2.3 Bits (§6)

On every **unsigned** integer type (N0234 on signed, as with the bit operators):

| Method | Result |
|---|---|
| `x.rotateLeft(n)`, `x.rotateRight(n)` | `T`; `n: u32`, taken modulo the bit width. Never panics |
| `x.wrappingShl(n)`, `x.wrappingShr(n)` | `T`; `n` modulo the bit width. This makes `ownership.md` §0 true |
| `x.leadingZeros()`, `x.trailingZeros()`, `x.countOnes()` | `u32` (the width for `0`) |

### 2.4 Searching and trimming (§22.2, §16.5)

On `StringView`, with the same methods on `Slice<u8>` (taking and giving `Slice<u8>`):

| Method | Result |
|---|---|
| `s.indexOf(needle)`, `s.lastIndexOf(needle)` | `usize?` (an empty needle is found at `0`, and at `length` by `lastIndexOf`) |
| `s.indexOfByte(b)` | `usize?` |
| `s.contains(needle)`, `s.startsWith(p)`, `s.endsWith(p)` | `bool` |
| `s.equalsIgnoreAsciiCase(t)` | `bool`; only `A–Z`/`a–z` fold. SMTP, IMAP and HTTP headers need it |
| `s.trim()`, `s.trimStart()`, `s.trimEnd()` | the view without ASCII whitespace (space, `\t`, `\n`, `\v`, `\f`, `\r`) |
| `s.splitOnce(sep)` | `TextHalves?`: the parts before and after the first `sep` |
| `s.split(sep)` | `TextSplit`: `next(borrow mut this) -> StringView?`, the pieces in order |

- `needle`, `p` and `sep` are `StringView` (or `Slice<u8>`). An empty `sep` is a panic ("empty separator").
- **UTF-8.** A match of valid UTF-8 inside valid UTF-8 always starts and ends on character boundaries. So every
  view these methods return is a valid `StringView`, without further checks.
- **Loans.** A result borrows `s`, like `s[lo..hi]`. `TextSplit<r>` and `TextHalves<r>` are region structs, like
  `SliceHalves` in `lib/core/slices.cee`.
- **Speed.** `indexOfByte` is a word-at-a-time scan. `indexOf` uses `indexOfByte` on the needle's first byte, then
  compares, which is enough for protocol text. A two-way search can come later, with the same results.

```ceesafe
var fields = message.split(",");
while (true) {
    let field = match (fields.next()) { Some(f) => f, null => { break; } };
    match (field.splitOnce("=")) {
        Some(kv) => { if (kv.left == "i") { iterations = kv.right.parseInt<u32>()?; } }
        null => {}
    }
}
```

### 2.5 Numbers from text (§6, §35)

| Method | Result |
|---|---|
| `s.parseInt<T>()` | `T?`, for any integer type `T` |
| `s.parseIntRadix<T>(radix)` | `T?`; `radix: u32` from 2 to 36 (panics otherwise); digits `0–9`, `a–z`, `A–Z` |
| `s.parseFloat<T>()` | `T?`, for `f32` or `f64` |

These are on `StringView` and on `Slice<u8>`.

- **Integers.** An optional `-` (signed `T` only) or `+`, then one or more digits, with nothing else: no
  whitespace, no `_`, no `0x`. Overflow is `null`.
- **Floats.** The grammar is that of the formatter's output plus the common forms:
  - `[+-]digits[.digits][(e|E)[+-]digits]`, with digits on at least one side of the `.`;
  - `inf`, `infinity` and `nan` in any case, with an optional sign. Postgres writes `Infinity`, `-Infinity` and `NaN`.
- **Rounding.** The result is the nearest `T` to the decimal value, with ties to even. Overflow gives ±∞, as IEEE
  754 and every correct `strtod` do. Underflow gives the nearest subnormal or ±0.
- **How floats parse.**
  - A fast path (the Clinger/Eisel-Lemire method) handles nearly every input.
  - A big-decimal fallback handles the rest, so every input is exact.
  - It is in `ceesafe_rt_float.c`, independent of the locale (never `strtod`), and linked only if the program
    parses a float.
  - The interpreter does the same over rationals, as it does for formatting (`float_format.cee`).
  - Under `--no-float`, `parseFloat<f64>` is rejected with N0297, like any other use of a float type (§35.9).
- **Round-trip.** For every finite `x`, parsing what `Console.writeLine(x)` prints gives `x` back. The tests check
  this.

## 3. Alternatives considered

- **Named byte-order methods** (`readU32BE`, `writeI16LE`, …): 8 types × 2 orders × read, write and push is 48
  names, against 6 generic ones. A program that wants the explicit names can wrap the generic ones. **(Question 1.)**
- **Free functions in a `Bytes` struct** (`Bytes.readBE<u32>(s, at)`) instead of methods: this needs no compiler
  tables, but it reads worse, and it doesn't match `copyFrom` and `fill`.
- **A general `Iterator` interface for `split`:** CeeSafe has none yet. `TextSplit.next()` has the shape a future
  iterator protocol would adopt.
- **`strtod` for floats:** it depends on the locale, varies by libc, and isn't in the freestanding profiles.

## 4. Not in this ADR

- **Hashes and HMAC, PBKDF2, MD5:** a package (PostgresClient first), in pure CeeSafe. 2.3 gives them what they need.
- **Hex and base64:** a package. Encoding fits in a few lines on top of 2.1 and 2.2.
- **TLS:** later, as its own ADR. The one runtime feature it will need is upgrading an existing `AsyncTcpStream` to
  TLS in place, for STARTTLS. Without that, STARTTLS needs a memory-BIO shim.

## 5. Testing

- **The corpus:** run-pass programs for each method, in the compiled program and in the interpreter. Check-fail
  programs for:
  - the signed-type and flexible-literal errors;
  - `v.extend(v.view())`;
  - a `Slice<u8>` receiver for `writeBE`.
- **Integer parsing:** every boundary (`MIN`, `MAX`, `MAX + 1`) for every width, and every malformed form.
- **Float parsing:**
  - the round-trip over a few million random `f64`s and every `f32` exponent;
  - the known-hard inputs, such as halfway cases and the longest exact decimals (the `parse-number-fxx-test-data`
    set);
  - the special values.
- **A compiler check:** reimplementing the compiler's own hand-rolled helpers on the new methods, where it already
  has them (byte search in the lexer, decimal parsing), is a good check, but not part of this ADR.

## 6. Questions for the owner

1. **Byte order:** generic `readBE<T>` / `writeBE` / `pushBE` (proposed), or named `readU32BE`-style methods?
2. **Bits:** include `leadingZeros`/`trailingZeros`/`countOnes` (cheap, and wanted by bitsets and allocators), or
   only rotation and the masking shifts?
3. **Float parsing in the runtime C:** in `ceesafe_rt_float.c` next to the formatter (proposed, and fastest), or in
   lib/core CeeSafe (slower to write and run, but no C)?
4. **Release:** all of it as 0.15.0, or 2.1–2.3 first (the encoder path) and 2.4–2.5 next?

## 7. Implementation and evidence (2026-11-05)

### 7.1 Where it is

- **lib/core:**
  - `bytes.cee`: `ByteOrder` and `VectorBytes`;
  - `text.cee`: `TextSearch`, `ByteSearch`, the halves and splits, and a private `ByteScan`;
  - `numbers.cee`: `NumberParsing` and a private `FloatDecimal`.
- **The checker:**
  - `src/check/bytes_text.cee` chooses the lib/core function by name and type;
  - `vectors.cee` (`extend`, `resize`, `reserve`) and `io.cee` (the bit methods) handle the new methods;
  - `members.cee` routes them.
- **Built-ins:** eleven new ones, `bfIntRotateLeft` to `bfVectorReserve`, in the interpreter, the C backend and `rt.h`.
- **NIR:**
  - two-phase borrowing for `VectorBytes` calls;
  - the call-site range checks of byte order (`checkByteRange`).

### 7.2 Changes from the text above

1. **Float parsing is in lib/core, in CeeSafe, not in the C runtime** (question 3's proposed answer).
   - **Why.** The interpreter needed the same algorithm in CeeSafe anyway (§2.5). One implementation now serves
     both, with no runtime file to link and no extern call.
   - **The algorithm.** Clinger's fast path, then the simple decimal conversion of Go's strconv and Wuffs, both in
     exact integer arithmetic.
   - **Accuracy.** Correctly rounded on every test below.
   - **Speed.** About 50 ns for a short input (`3.14159`) and about 210 ns for a 17-digit one
     (`0.30000000000000004`), which takes the decimal path. Exponents far from zero cost a few µs. A faster path
     (Eisel-Lemire) can come later without changing a result.
2. **No panics in the parsers.** lib/core has no way to panic with a message of its own, so these give a defined
   result instead:
   - a radix outside 2–36 is `null`;
   - an empty separator is found at the start by `splitOnce`, and gives the text whole from `split`.
3. **Byte order is on fixed-width types only:** not `usize` or `isize` (N0207), since a wire format has a fixed width.
   - **Receivers.** Byte slices and byte vectors; a `StringView` reads through `.bytes()`.
   - **Where it panics.** Each read and write checks its range at the call (two NIR `BoundsCheck`s), so the panic is
     "index out of bounds" on the program's line, as §2.2 says. A check inside the library would have named lib/core.
4. **`VectorBytes` has no allocator parameter.** A vector's allocator is `SystemHeap` or `FailingAllocator` (§23.1),
   and a generic `Vector<u8, A>` is not a type (N0263). So it has a function for each, and the checker picks one.
   - **Two-phase.** Its calls take their argument before the vector's loan, as `push` does. The NIR builder
     recognises the struct by its name, which only lib/core may use.
5. **`--no-float` does not apply to library files** (type-system.md §35.9). Programs are unaffected:
   - a program reaches the float parser only through `parseFloat<f32>` or `parseFloat<f64>`, which N0297 still
     rejects;
   - unused library functions are not emitted.
6. **Additions:**
   - a `String` has the search and parse methods, on its text;
   - `Vector<u8>` has every byte-slice method, on its elements.

### 7.3 Evidence

- **Float parsing** against exact references:
  - 3.1 million inputs, from six seeds, with no difference:
    - random doubles in three formats;
    - random mantissas of up to 30 digits with exponents from −340 to 320;
    - exact halfway points and their neighbours;
    - random `f32`s;
    - the boundaries (largest subnormal, smallest normal, `f64` and `f32` overflow midpoints, `2^53 + 1`, 400-digit
      inputs);
    - malformed text.
  - **The references:** Python's `float` for `f64`, and exact rational rounding for `f32`.
  - **The interpreter:** gives the same results on a 3,000-input subset.
- **Round-trip:** 2 million random `f64`s and 2 million `f32`s, each printed with `appendNumber`, parse back to the
  same bits.
- **The corpus:**
  - **New tests:** five run-pass (`int-bits`, `vector-bulk`, `byte-order`, `text-search`, `number-parsing`), one
    run-fail (`byte-order-past-end`) and six check-fail. The run-pass tests run compiled and interpreted.
  - **Changed tests:** `string-views.cee` no longer uses `trim` as its missing method. Four snapshots moved:
    - the HIR of `async-timeout` and `effects-of`, where only closure numbers changed;
    - the C of `vectors` and `slot-maps`, which now include the new vector helpers.
- **Speed** (Apple M-series, `-O2`):
  - `readBE<u32>`: 52 million calls in a loop take about 40 ms in total, about 1 ns each;
  - `indexOfByte` scans about 10 GB/s;
  - `extend` is a `memcpy` after one capacity check.
