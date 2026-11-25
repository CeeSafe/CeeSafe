# ADR 0260 — Byte views as text: the rest of ADR 0253

- Status: **Proposed — implemented** 2026-11-26 on the branch `fix/g6-text-views` (from 0.95.13), at the owner's
  request to fix the SQLite port's `CEESAFE_FINDINGS.md` findings 52, 53 and 54, met while moving the port's SQL
  text from `String` to `ByteString` (its finding 41, on ADR 0253). Not released.
- Scope: the checker (`src/check/members.cee`, `vectors.cee`, `expr.cee`, `arrays.cee`, `references.cee`,
  `text_view.cee`); lib/std `testing.cee` (`Check.equalTextOf`); type-system.md §13.3 and §22.5;
  guide/builtins.md; spec/api/std.md; the book's Appendix C. Extends ADR 0253 (§2.6–§2.8 there).

## 1. Context

ADR 0253 made `StringView` and `ByteStringView` two `TextView`s, so text code can be written once for either. A
port that then moved some 37,000 lines from `String` names to `ByteString` ones met the places where a byte view is
still not used as a text view is (the port's findings 52–54):

1. a method call through two references, `name.view()` with `name: borrow borrow ByteString` (a pattern's binding
   through a borrowed optional), is N0200, "'borrow borrow ByteString' has no method named 'view'"; on a `String` it
   works, because the built-ins read through every reference, while a struct's methods were looked up through one;
2. `s.append(v)` with `s: String` and `v: ByteStringView` is N0205: `String`'s built-in `append(StringView)` is
   found first, and `TextBuffer`'s `append<N: TextView>` (ADR 0253 §2.1) is never tried;
3. a string literal is not a `ByteStringView`: `"main"` passed, returned or bound where one is expected is N0205;
4. `v[a..b]` slices a `StringView` but not a `ByteStringView` ("only arrays and slices can be sliced");
5. `v.length` is a field of `StringView` and `String`, but a method of `ByteStringView` and `ByteString`;
6. `parseInt<T>()` (and `parseIntRadix`, `parseFloat`) is on `StringView` and `Slice<u8>`, not on `ByteStringView`;
7. lib/std's `Check.equalText` takes `StringView`s only.

Each meant a few lines of churn at every use. None is a question of safety: each is a form that the same values
already had under another name.

## 2. Decision

### 2.1 A method call through more than one reference

A receiver of type `borrow borrow T` (any depth) reaches `T`'s methods as a `borrow T` does: the outer references
are read through, and the reference left is `borrow mut` only if every one was. So `borrow (borrow mut T)` calls
`T`'s `borrow this` methods and refuses its `borrow mut this` ones (N0248, "behind a shared reference"), as Rust's
`&&mut T` does. Slices, string views and closures, which are references themselves, are not read through. Writing
the type `borrow borrow T` stays N0246 (type-system.md §13.1): such a type only arises from a binding.

### 2.2 `String.append` of any text

On a concrete `String`, `s.append(t)` with `t` a `StringView`, a string literal or a lent `String` is the built-in,
unchanged (`Result<void, AllocError>`). With `t` any other `TextView` (a `ByteStringView`, a `Slice<u8>`) it is
`TextBuffer`'s `append`, with `t`'s type: `Result<bool, AllocError>`, `false` (and nothing appended) for text that
is not UTF-8, as ADR 0253 §2.1 says for a `String`. This is ADR 0253 §2.8's rule for a search's needle, applied to
`append`. The receiver stays two-phase: `t` is checked before `s` is borrowed mutably.

### 2.3 A string literal where a `ByteStringView` is expected

A string literal checked against `ByteStringView` or `ByteStringView?` (an argument, a return, a typed `let`, a
field, an element) is `ByteStringView.fromText("…")`: the literal's bytes, nothing copied. Only a literal converts:
a `StringView` value does not, so an expected type never changes which generic instance a call picks (a literal
already has the one type it can have there). `ByteStringView.fromText(s)` remains the conversion of a value.

### 2.4 `v[a..b]` on a `ByteStringView`

`v[a..b]`, `v[a..]`, `v[..b]` and `v[..]` on a `ByteStringView` are `v.slice(a, b)`, `v.sliceFrom(a)`,
`v.sliceTo(b)` and `v`: a `ByteStringView` of the same bytes, panicking out of bounds as a slice's range does. A
byte view is any bytes, so every position is a boundary (a `StringView`'s range must end on characters, §22.2). A
mutable range of one (`mut v[..]`) stays refused: the view is read only.

### 2.5 `v.length` on a `ByteStringView` and a `ByteString`

`v.length` (no call) on a `ByteStringView` or a `ByteString` is `v.length()`, the count of bytes, as `s.length` is on
a `StringView` and a `String`; the `ByteString` is lent, not moved. It is not a place: `v.length = n` stays refused.
`v.length()` keeps working.

### 2.6 Parsing a `ByteStringView`

`v.parseInt<T>()`, `v.parseIntRadix<T>(radix)` and `v.parseFloat<F>()` on a `ByteStringView` parse `v.bytes()`, as on a
`Slice<u8>` (ADR 0071 §2.5): the same results, null for anything that is not a number. In code generic over
`T: TextView`, `t.bytes().parseInt<U>()` is the form: `TextView` gets no generic-over-integers method, as the
parsers are the checker's, chosen by their type argument.

### 2.7 `Check.equalTextOf`

lib/std's `Check.equalTextOf<E: TextView, A: TextView>(expected, actual)` compares two texts of any kinds by their
bytes; a failure shows both, as `Check.equalText` does, with each byte past ASCII of text that is not UTF-8 written
`\xHH`. `Check.equalText(StringView, StringView)` is unchanged: a generic parameter would refuse a `String` argument,
which a `StringView` parameter lends, and the repositories hold 583 calls of it.

## 3. Consequences

- No program changes meaning: each form was an error before, except `s.append(t)` for a `TextView` `t` other than
  a `StringView`, which was N0205.
- No new HIR or C: each form is lowered to calls of lib/core's existing methods, or to a dereference.
- The interpreter and native builds agree: both run the same lowered calls.

## 4. Tests

- `run-pass/adr0260-byte-views-as-text.cee`, natively, in the interpreter and with `--check-leaks`: §2.1 to §2.6,
  each against values worked out by hand (`"key=1234"` sliced and parsed, `0x1234` = 4660; a `String` given a
  `ByteStringView` that is UTF-8 and one that is not). A function with no reference parameter returns a literal as a
  `ByteStringView<static>`, as it would a `StringView`.
- `check-fail/adr0260-byte-views-as-text-errors.cee`: what stays refused: a `borrow mut this` method through a
  shared outer reference (N0248); `mut v[..]` of a byte view (N0205); `v.length = 1` (N0216); a `StringView` value
  (not a literal) where a `ByteStringView` is expected (N0205).
- `tests/packages/test-command` (`tools/test_command_check.py`): `passesTextOf` and `failsTextOf`, natively and in
  the interpreter; the failure's message, reviewed by hand in `expected.tsv`, is `expected "abc", got "ab\xff"`.

## 5. Alternatives not taken

- **Every `StringView` converts to a `ByteStringView`** (§2.3): lossless, but a conversion of values by expected type
  could make a generic call's instance depend on its context. Literals have one type there either way.
- **`Check.equalText` made generic** (§2.7): it would refuse the `String` arguments existing tests pass.
- **`ByteStringView` given a `length` field** (§2.5): a second copy of what its slice holds.
- **`parseInt` in `TextView`** (§2.6): the parsers are not library functions generic over integers, and a method
  of the interface would need one; `t.bytes()` reaches `Slice<u8>`'s.

## 6. The SQLite port

With these, the port's helpers `parseUsize`, `parseI64` and `parseI32` (src/text.cee), most `ByteStringView.fromText`
calls for literals, the `.length()` churn and the tests' `equalName` can go, and `executeAnalyze`'s match can bind
through the borrow again.
