# ADR 0231 — Text of any bytes: `ByteString` and `ByteStringView`

- Status: **Proposed — implemented** 2026-11-24 on the branch `fix/findings2-text`, at the owner's instruction
  ("Implement fixes for findings 6, 15, 19, 26 and 35 in CeeSafe": this is finding 19 of the SQLite port). Not
  released. §8's questions are the owner's.
- Scope: lib/core `bytestring.cee` (new); lib/std `collections.cee` (`HashKey`) and `ordered_map.cee`
  (`OrderKey`); `src/check/expr.cee` (N0207's message for `Console.write`); type-system.md §22.4; guide/builtins.md;
  spec/api/core.md.

## 1. Context

`String` and `StringView` hold valid UTF-8 only (type-system.md §22.2, §23.2), and that invariant is what lets a view
be sliced on character boundaries, printed and compared as text. There is no type for text that is *mostly* UTF-8:
C's `char *`, Rust's `OsString` and `bstr`, a file name on Linux, and SQLite's TEXT, which may hold any bytes. SQLite's
JSON functions decode `"\ud800"` to `ED A0 80`, an unpaired surrogate that UTF-8 forbids, and `CAST(x'ff' AS TEXT)`
keeps the byte `FF`. The port's value type was built on `String`, so it could not carry those bytes: its workaround
turns each byte of an invalid sequence into U+FFFD, and the file it writes then differs from SQLite's.

A `Vector<u8>` holds the bytes, but it is not text: it has no text appends next to its byte ones that read as a
string's, no lossy conversion, no way to print with escapes, and nothing says which vectors are text.

## 2. Decision

Two library types in lib/core, written in safe CeeSafe over `Vector<u8>` and `Slice<r, u8>`, with **no language
change**: the only compiler change is N0207's message (§2.6).

### 2.1 The types

- `ByteString`: owned, a move type, its bytes in a private `Vector<u8>` (the system heap's: §8 question 3).
- `ByteStringView<region r>`: borrowed and Copy, a private `Slice<r, u8>`, valid for `r` as a `StringView<r>` is
  (§25); a struct field of the type needs a region parameter (N0270), as a view's does.

The names follow `String`/`StringView`; Rust's bstr says `BString`/`BStr`. Both fields are private (N0296) though
they guard no invariant: any bytes are a byte string. That keeps the representation lib/core's to change (an
inline buffer for short texts, an allocator parameter).

### 2.2 Making one, without copying where the bytes allow

| Call | Copies |
|---|---|
| `ByteStringView.fromText(text)` (a literal, a `StringView`, a `String` lent) | no |
| `ByteStringView.fromBytes(bytes)` (a `Slice<u8>`) | no |
| `ByteString.fromBytes(v)` (takes a `Vector<u8>`) | no |
| `b.view()`, `b.bytes()` | no: loans of `b` |
| `b.intoBytes()` (gives the `Vector<u8>` back) | no |
| `ByteString.fromText(text)`, `ByteString.copyOf(bytes)`, `b.clone()`, `v.toByteString()` | one allocation, none for no bytes |

A `String` cannot become a `ByteString` without a copy: the language has no conversion of a `String` into its
`Vector<u8>` (§8 question 2).

### 2.3 UTF-8

- `v.toUtf8() -> StringView<r>?` and `b.toUtf8() -> StringView?`: a view, with no copy, when the bytes are UTF-8
  (Table 3-7, as `StringView.fromUtf8` checks); null otherwise. `v.isUtf8()`.
- `v.toUtf8Lossy()` and `b.toUtf8Lossy()`: `Result<String, AllocError>`, a new string with one U+FFFD for each
  **maximal subpart** of an ill-formed sequence, the Unicode Standard's §3.9 practice, which the WHATWG Encoding
  Standard's decoder and Python's `bytes.decode('utf-8', 'replace')` follow. `v.appendLossyTo(mut s)` appends the
  same text to a `String`. A maximal subpart is the longest prefix of a well-formed sequence at a position where none
  is (`E2 82` before `62`: one), or the one byte that starts none (`FF`, `C0`, a lone `80`): `ED A0 80` is three,
  since no sequence starting `ED A0` is well-formed.
- `v.chunks()`: an iterator (§16.4) of `Utf8Chunk<r>`, each `valid: StringView<r>` (a run of UTF-8, perhaps empty)
  then `invalid: Slice<r, u8>` (one maximal subpart, 1 to 3 bytes; empty only in the last chunk). A printer writes
  `valid` and escapes `invalid`; the lossy conversion is the chunks with U+FFFD for each `invalid`. Empty bytes give
  no chunk. This is Rust's `Utf8Chunks`.

### 2.4 Bytes as text

- `length()`, `isEmpty()`, `byteAt(i)`, `slice(start, end)`, `sliceFrom(start)`, `sliceTo(end)` (methods, as a
  library struct has no read-only fields: `v.length()`, not `v.length`). A slice may cut a sequence; the bytes are
  any bytes. Out of range panics, as `s[lo..hi]` does.
- The searches of §16.6, on bytes, with a `Slice<u8>` needle (so a literal's `"…".bytes()`, a `StringView`'s, a
  vector's and another byte string's all serve): `indexOf`, `lastIndexOf`, `indexOfByte`, `contains`, `startsWith`,
  `endsWith`, `equalsIgnoreAsciiCase`, `trim`, `trimStart`, `trimEnd`, `splitOnce` (a `ByteStringHalves<r>?`) and
  `split` (a `ByteStringSplit<r>`, an iterator of views). They are lib/core's `ByteSearch`.
- `==` and `!=` compare the bytes: both types are `@derive(Equal)` (§28.7). `v.equals(w)` does the same across two
  regions, `v.compare(w) -> i32` orders bytewise, a prefix first (for UTF-8, code point order, as `String`'s).
- `ByteString` is a `HashKey` (lib/std; its bytes then a terminator, as a `String`'s) and an `OrderKey` (lib/std;
  `compare`), so it keys a `HashMap` and an `OrderedMap`. A view does not: a map's key cannot be a scoped type
  (N0274).

### 2.5 Appending

String's family, on a `ByteString`, each `Result<…, AllocError>`, growing once and unchanged on failure:
`append(text: StringView)` (so `b.append("…")` takes a literal), `appendBytes(bytes)`, `appendByte(b)`,
`appendNumber(n: i64)` and `appendUnsigned(n: u64)` (decimal, as `String.appendNumber`), `appendHex`, `appendOctal`,
`appendFixed`, `appendPadded` (lib/core's `AppendFormatted`, through `Formatted`) and `appendChar(c)`,
`appendUtf8(bytes)` (`AppendText`: `Result<bool, …>`, `false` for what is not a scalar value or not UTF-8). A
library function cannot take every integer type as `String.appendNumber` does, which is the compiler's: hence two.

### 2.6 Output

The bytes are written as they are: `Console.writeBytes(b.bytes())` (§26) and `v.writeTo(mut w)` into a
`TextWriter`. `Console.write(b)` stays N0207, since `Console.write` prints text, and its message now says what to
write: "`'Console.writeLine' can print integers, floats, bools and string views, not 'ByteString'; write its bytes
with 'Console.writeBytes(text.bytes())'`". Formatting follows the library's conventions, without format strings
(ADR 0217 §2.2): append the pieces.

### 2.7 Literals

A string literal is a `StringView`, not a `ByteStringView`. `ByteStringView.fromText("…")` is one call and copies
nothing, and `append` and the searches take the literal (or its `.bytes()`) directly. Making a literal one where a
`ByteStringView` is expected would be the first conversion of a built-in value into a library struct, in the checker,
the interpreter and the C backend, for a saving of one call: not done (§8 question 1).

## 3. Consequences

- A program that keeps foreign text keeps it exactly, and converts at the edge it chooses: strictly (`toUtf8`), with
  replacement (`toUtf8Lossy`), or with its own escapes (`chunks`). The SQLite port's value type can hold TEXT as a
  `ByteString` and give `StringView`s where the bytes are UTF-8.
- lib/core has 6 more public types (spec/api/core.md); emit-c snapshots that print type ids may move.
- `String` and `StringView` are unchanged: their invariant stands.

## 4. Tests

- `tests/reference/tests/corpus/run-pass/byte-string-utf8.cee`, written by `tools/oracles/gen_byte_string_cases.py`
  from Python's decoder: 58 fixed cases (Table 3-7's edges, the Standard's Tables 3-8 to 3-11, overlong forms,
  surrogates, truncated sequences, SQLite's two cases), each `isUtf8()`, the lossy bytes, and the chunks (from the
  decoder's error spans, through `codecs.register_error`); then 3,000 seeded strings summarised, with the order of
  neighbours against Python's `bytes` order.
- `run-pass/adr0231-byte-strings.cee`: every member once; expectations written by hand.
- `check-fail/adr0231-byte-string-errors.cee`: N0207 (printing), N0205 (a byte string is no `StringView`, and a literal no `ByteStringView`), N0270 (a
  view field without a region parameter).
- `tests/support/run-pass/byte-string-keys.cee`: `ByteString` keys of `HashMap` and `OrderedMap` against a model, its
  summary pinned by `tools/oracles/support_models.py` (Python's dict, `sorted`, `decode('utf-8', 'replace')`), run with
  `--check-leaks`.

## 5. Alternatives not taken

- **A flag on `String` that it may hold any bytes**, or a `String<NotUtf8>` parameter: every view, slice, print and
  comparison would test it, and `StringView`'s guarantee would become a convention.
- **`Vector<u8>` alone**, with free functions: works today, but says nothing about which bytes are text and gives no
  text appends or printing.
- **An encoding parameter (`ByteString<Latin1>`)**: SQLite's TEXT has no encoding beyond "probably UTF-8"; a program
  that knows an encoding converts once to a `String`.
- **Replacement with one U+FFFD per byte** (the port's workaround) or per run of bad bytes: neither is the standard
  practice, and each disagrees with Python, browsers and Rust's `from_utf8_lossy`.

## 6. Not done

- `Console.write(b)` printing the bytes (§2.6), a literal as a `ByteStringView` (§2.7), a `String` into a
  `ByteString` without a copy (§2.2), a `ByteStringView` as a map key (§2.4), allocators other than the system heap.

## 7. Evidence

Run on 2026-11-24 with the tree's `out/ceesafec`: the three corpus files above (native, `--check-leaks`, and
`--interp`) and the support test with `--check-leaks`, all passing; ROADMAP §10 has the full runs.

## 8. Questions for the owner

1. A string literal where a `ByteStringView` is expected (§2.7)? Recommended: not now; `fromText` is one call.
2. `String.intoBytes() -> Vector<u8>` (a `String` gives up its buffer, no copy), so `ByteString.fromString(s)`
   copies nothing? Recommended: yes, as a separate small ADR; the reverse needs a check and exists as `toUtf8`.
3. `ByteString<A>` for `FailingAllocator` and `SecretHeap`, as `String<A>` has? Recommended: when a program asks.
4. `Console.write(b)` and `writeLine(b)` writing a byte string's bytes, lowered by the checker to `writeBytes`?
   Recommended: no: `Console.write` prints text, and the message names the call that writes bytes.
