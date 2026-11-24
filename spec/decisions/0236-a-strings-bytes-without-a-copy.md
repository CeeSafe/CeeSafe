# ADR 0236 — A string's bytes without a copy: `s.intoBytes()`, `String.fromUtf8(v)`, `ByteString.fromString(s)`

- Status: **Proposed — implemented** 2026-11-24 on the branch `fix/findings3-text`, at the owner's instruction (finding
  39 of the SQLite port, `CeeSafeApps/sqlite/CEESAFE_FINDINGS.md`). Not released. §8's questions are the owner's.
- Scope: lib/core `text.cee` (`StringBytes`, new) and `bytestring.cee` (`ByteString.fromString`, `intoString`);
  `src/check/vectors.cee` (the two calls on `String`, and the libraries' `releaseBuffer`/`adoptBuffer` on a string);
  `src/documentation/doc_generator.cee` (`ceesafec doc` of a name); type-system.md §22.4, §23.2; tooling.md §1;
  guide/builtins.md; spec/api/core.md; the book's Appendix C.

## 1. Context

ADR 0231 made `ByteString` over a `Vector<u8>`, and `ByteString.fromBytes(v)` takes a vector without copying. A
`String` is a `Vector<u8>` with an invariant (its bytes are UTF-8, type-system.md §23.2; the layout is the same), but
the language had no way to give a string's buffer to a vector: text built in a `String` (`appendNumber`, `appendFixed`,
an edited SQL text) was copied once more to become a `ByteString` (`ByteString.fromText(s.view())`). ADR 0231 §8
question 2 recommended `String.intoBytes()` as a small ADR of its own. The reverse, a `Vector<u8>` that holds UTF-8
(a file read whole, a network message) into a `String`, was also a copy (`String.from(StringView.fromUtf8(v.view())!)`).

The finding also notes that `ceesafec doc ByteString` is "not a .cee or .c file": the command documents files only, so
a lib/core type's members are found in its source, in spec/api/core.md, or through the MCP server's `ceesafe_api`.

## 2. Decision

### 2.1 A string into its bytes

`s.intoBytes() -> Vector<u8, A>` (for `s: String<A>`) consumes the string and gives its buffer, its length and its
capacity to a vector of the same allocator: no copy, no allocation, nothing freed. It is a move of `s` (N0250 if `s`
is used after; moving out of a loan is N0251, as for any move). It is a method because it consumes its receiver, as
`ByteString.intoBytes()` and `Box.intoRaw` do; "into" is lib/core's word for a conversion that consumes and copies
nothing (`intoBytes`, `intoRaw`, `intoForeign`). Nothing is checked: any UTF-8 is bytes.

### 2.2 Bytes into a string

`String.fromUtf8(v) -> Result<String<A>, Vector<u8, A>>` (for `v: Vector<u8, A>`) consumes the vector and, when its
bytes are UTF-8 (Table 3-7, the check of `StringView.fromUtf8`), gives its buffer to a string of the same allocator:
no copy. Otherwise the result is `Err(v)`, the vector given back unchanged, so the caller keeps its bytes (to convert
lossily, to report, or to keep as a `ByteString`). The name is `StringView.fromUtf8(bytes) -> StringView?`'s, which
checks the same way and views where this one takes: the argument's type says which. A written type
(`String<A>.fromUtf8(v)`) must be the vector's allocator's string (N0205); the argument must be a `Vector<u8, A>`
(N0205 otherwise, a `String` included). `String.fromUtf8` is a built-in function on the type, as `String.from` is.

Rust's `String::from_utf8` returns an error that holds the vector (`into_bytes`); here the vector is the error, since
it is all the error says: its first bad byte is `ByteStringView.fromBytes(v.view()).chunks()`'s first `invalid`.

### 2.3 Byte strings

`ByteString.fromString(s: String) -> ByteString` takes the string's bytes without a copy (`fromBytes(s.intoBytes())`),
beside `fromText(text: StringView)`, which copies a view. `b.intoString() -> Result<String, ByteString>` is
`String.fromUtf8` on the bytes: the string without a copy when they are UTF-8, the byte string back otherwise. Its
copying counterparts are `toUtf8()` (a view) and `toUtf8Lossy()`. A `ByteString` is of the system heap (ADR 0231 §8
question 3), so these two are for `String` alone.

### 2.4 Lowering

Both calls on `String` are lib/core's `StringBytes` (`text.cee`), as `String.from` is `StringCopying` (ADR 0108):
`intoBytes`, `fromUtf8` and their `…Failing` and `…Secret` copies for `FailingAllocator` and `SecretHeap`, chosen by
the checker from the allocator. They are written over the two built-ins only the libraries may call, `releaseBuffer`
and `adoptBuffer` (ADR 0090, type-system.md §23.3), which the checker now allows on a `String` too, in library code
only: the string's or vector's buffer is given up without being freed and adopted by the other value, with the
allocation report unchanged (`foreign` is `false`: the block stays CeeSafe's, so `live` does not move).

- No `unsafe` in user code and no weaker check: `StringBytes` is safe to call directly (`StringBytes.intoBytes(s)`);
  `fromUtf8` checks every byte before the buffer becomes a string's, so the UTF-8 invariant of §23.2 stands. The only
  code that could break it, `adoptBuffer` on a string, is the libraries' alone (N0200 for a program, as before).
- A `SecretHeap` buffer stays a `SecretHeap` buffer (wiped when freed, ADR 0197), and a `FailingAllocator` one stays
  counted by it: the allocator is part of both types, so a block never changes allocators.
- An empty string (capacity 0) gives the empty vector, and back: nothing is adopted.
- The interpreter and the C backend already lower `releaseBuffer`/`adoptBuffer` from the vector's three words, which a
  string has at the same places (§23.2, "Layout"): no backend change.

### 2.5 `ceesafec doc NAME`

`ceesafec doc` with one argument that is a name — an identifier or `Type.member`, not a path that exists, with no `/`
and no `.cee` or `.c` — prints what `ceesafe_api` (tooling.md §6.3) finds for it in the embedded reference
(spec/api/core.md, spec/api/std.md, guide/builtins.md): a type's whole section, or a member's entries, as Markdown, each
after a line naming its document and heading. With nothing found, it prints `no entry for 'NAME'` and the suggestions
on standard error and exits 1. So `ceesafec doc ByteString`, `ceesafec doc ByteString.fromString` and `ceesafec doc
appendHex` answer from the compiler, as the MCP tool does, with no server.

## 3. Consequences

- `ByteString.fromString(text)` replaces `ByteString.fromText(text.view())` where the text was built in a `String`;
  a vector read from a file becomes a `String` with no copy and one check.
- lib/core has one more public type (`StringBytes`) and `ByteString` two more members (spec/api/core.md).
- `String<A>` gains one method and one function; its invariant and layout are unchanged.

## 4. Tests

- `run-pass/adr0236-string-bytes.cee`: the round trip `String` → `Vector<u8>` → `String` and through `ByteString`,
  with the buffer's address and capacity kept (no copy observable), an empty string, a byte string or vector that is
  not UTF-8 given back unchanged (its capacity too), `String<FailingAllocator>` and `String<SecretHeap>`, and lib/core's
  `StringBytes` called directly. Native with `--check-leaks` and `--interp`; expectations written by hand.
- `check-fail/adr0236-string-bytes-errors.cee`: `String.fromUtf8` of a `String`, a `Slice<u8>` or a `Vector<i32>`
  (N0205), a written allocator that is not the vector's (N0205), `intoBytes` with an argument and `fromUtf8` with
  none (N0209), and a program's `adoptBuffer` on a string (N0200).
- `check-fail/adr0236-string-bytes-moves.cee`: the string used after `intoBytes` (N0250), `intoBytes` through a
  `borrow` and a `borrow mut` (N0251), `String.fromUtf8` of a borrowed vector (N0240).
- tooling: `tools/cli_check.py`'s `doc` cases for a type, a member and an unknown name.

## 5. Alternatives not taken

- **`ByteString.fromString` alone**, with no `String.intoBytes`: the conversion is the string's to give, and a
  `Vector<u8>` is wanted as often (a writer's buffer, a hash's input) as a `ByteString`.
- **`s.intoByteString()`**: lib/core makes a type from another by the target's `from…` (`ByteString.fromBytes`,
  `String.from`), and a method of `String` naming a lib/core type would couple the built-in to the library.
- **`String.fromUtf8(v) -> String?`**, as `StringView.fromUtf8` is: the vector would be lost when it is not UTF-8,
  and with it the bytes a program wanted to keep or report.
- **`String.fromUtf8Unchecked`** (`unsafe`): `fromUtf8`'s check is one pass over bytes already in cache, and an
  `unsafe` door to a broken invariant is what CeeSafe does not offer where a safe design exists.
- **A reinterpretation in the backends** (a `BuiltinCall` that changes the type of the three words): two backends and
  the NIR to change, where the existing library built-ins already move a buffer between two values.

## 6. Not done

- `ceesafec doc NAME` for a package's own items (the reference is the embedded one, as for `ceesafe_api`).
- `ByteString<A>` (ADR 0231 §8 question 3), and so `fromString` of a `String<FailingAllocator>`.

## 7. Evidence

Run on 2026-11-24 with the tree's `out/ceesafec`; ROADMAP §10 has the runs.

## 8. Questions for the owner

1. `String.fromUtf8(v)`'s error is the vector itself, not a `FromUtf8Error { bytes, validUpTo }`. Recommended: keep;
   the position is one call away (§2.2).
2. `ceesafec doc NAME` prints the embedded reference only. Should a name also be looked up in the current package's
   public items? Recommended: later, with the LSP's index, which knows them.
