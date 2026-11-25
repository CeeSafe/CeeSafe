# ADR 0253 — Text of either kind: `TextView` and `TextBuffer`

- Status: **Proposed — implemented** 2026-11-25 on the branch `fix/g5-text` (from 0.95.10), for the SQLite port's
  `CEESAFE_FINDINGS.md` finding 41. Not released. The owner answered §8's six questions the same day (§8):
  §2.6 to §2.8 and the change of `TextBuffer.append` (§2.1) follow from them.
- Scope: lib/core `text_view.cee` (new: `TextView`, `TextBuffer`, `TextPair`, `TextPieces`, `TextChars`,
  `TextChar`) and `bytestring.cee` (`ByteStringView`'s and `ByteString`'s members through them); the checker
  (`src/check/interfaces.cee`, `members.cee`, `same_name_methods.cee`, `references.cee`, `text_view.cee` (new),
  `equality.cee`, `arrays.cee`, `patterns.cee`, `bytes_text.cee`) and the C names of methods
  (`src/backend/c_names.cee`); type-system.md §21.1, §22.4, §22.5 (new), §25.6; guide/builtins.md;
  spec/api/core.md; the book's Appendix C. Amends ADR 0231 §2.4 (the searches' needles and results).

## 1. Context

Finding 41: code written for `StringView` cannot take a `ByteStringView`. The SQLite engine's tokenizer, parser,
catalog and messages are written against `String` and `StringView` (`==` with a literal, `equalsIgnoreAsciiCase`,
`split`, `splitOnce`, slicing, `append`); `ByteStringView` has a subset of those, its searches take a `Slice<u8>`
(`m.startsWith("UNIQUE")` is N0205), and no interface covers both views, so a function cannot be written once for
either. The port therefore keeps SQL text in a `String`, each byte of a sequence that is not UTF-8 standing as a
private-use code point U+10FF80..U+10FFFF (and a real one of those escaped the same way), in `src/sqltext.cee`:
36 call sites translate text in and out at every place a name becomes a value or a message.

Three things stood in the way, each in the language rather than the library:

1. **A view cannot implement an interface.** `StringView` and `Slice<u8>` are references (type-system.md §22.2,
   §16.1), and "no other scoped type implements an interface" (§21.1, N0267).
2. **Generic code cannot take a view through `borrow this`.** `borrow T` of a view is a reference to a reference,
   so a generic function whose bound's methods take `borrow this` is N0274 at `T = StringView` (§25.4). A view's
   methods take it by value, as views are Copy.
3. **A method that takes `this` by value cannot return a borrow of it in an interface.** `fn bytes(this) ->
   Slice<u8>` is N0245, "a function without reference parameters cannot return a reference": `this: Self` is
   opaque, and elision (§25.6) looks only at parameters of scoped type. Interfaces have no region parameters
   (N0216), so the result's region cannot be named either. The only borrowing results expressible from a
   by-value opaque value were `Self` and types built from it.

## 2. Decision

### 2.1 The interfaces (lib/core `text_view.cee`)

```ceesafe
public interface TextView: Copy {
    fn bytes(this) -> Slice<u8>;                       // required
    fn slice(this, start: usize, end: usize) -> Self;  // required
    fn isCharBoundary(this, at: usize) -> bool => at <= this.bytes().length;   // StringView overrides it
    // defaults, written once over the two above:
    fn length(this) -> usize;  fn isEmpty(this) -> bool;  fn byteAt(this, index: usize) -> u8;
    fn sliceFrom(this, start: usize) -> Self;  fn sliceTo(this, end: usize) -> Self;
    fn toUtf8(this) -> StringView?;  fn isUtf8(this) -> bool;
    fn equals<N: TextView>(this, other: N) -> bool;   fn compare<N: TextView>(this, other: N) -> i32;
    fn equalsIgnoreAsciiCase<N: TextView>(this, other: N) -> bool;
    fn compareIgnoreAsciiCase<N: TextView>(this, other: N) -> i32;
    fn indexOf<N: TextView>(this, needle: N) -> usize?;   fn lastIndexOf<N: TextView>(this, needle: N) -> usize?;
    fn indexOfByte(this, b: u8) -> usize?;
    fn indexOfIgnoreAsciiCase<N: TextView>(this, needle: N) -> usize?;
    fn contains<N: TextView>(this, needle: N) -> bool;
    fn startsWith<N: TextView>(this, prefix: N) -> bool;  fn endsWith<N: TextView>(this, suffix: N) -> bool;
    fn startsWithIgnoreAsciiCase<N: TextView>(this, prefix: N) -> bool;
    fn endsWithIgnoreAsciiCase<N: TextView>(this, suffix: N) -> bool;
    fn trim(this) -> Self;  fn trimStart(this) -> Self;  fn trimEnd(this) -> Self;
    fn splitOnce<N: TextView>(this, separator: N) -> TextPair<Self>?;
    fn split<N: TextView>(this, separator: N) -> TextPieces<Self, N>;
    fn chars(this) -> TextChars<Self>;
}

public interface TextBuffer {
    fn append<N: TextView>(borrow mut this, text: N) -> Result<bool, AllocError>;
    fn appendLossy<N: TextView>(borrow mut this, text: N) -> Result<void, AllocError>;
    fn bytes(borrow this) -> Slice<u8>;
    fn clear(borrow mut this);
}
```

`TextView` is implemented by **`StringView`** (UTF-8), **`ByteStringView<r>`** (any bytes) and **`Slice<u8>`**
(bytes as text). `TextBuffer` is implemented by **`String`** and **`ByteString`**: an owned text is read
generically through its view (`s.view()`), since a borrowing interface method of an owned type returns a view of
one kind (§21.1, ADR 0131), and is written generically through `TextBuffer`.

- **Needles** are any `TextView`: a string literal (a `StringView`), either view, or bytes. `name.startsWith("sqlite_")`
  and `name.equalsIgnoreAsciiCase(other)` read the same for a `StringView` and a `ByteStringView`, whatever the
  other is. An owned text is passed as its view.
- **Matches on UTF-8 are whole characters.** A needle that is not UTF-8 may still occur inside UTF-8 (`A9` is the
  second byte of `é`). A search on a view whose `isCharBoundary` is not every position (a `StringView`) takes only
  a match that starts and ends on character boundaries, and looks on past one that does not; so every `Self` a
  method returns is a valid `slice`, and a `StringView`'s pieces stay UTF-8 without a check. On a `StringView` with
  a `StringView` needle every match is whole already (§16.6), so nothing is lost.
- **ASCII case.** `…IgnoreAsciiCase` fold `A`–`Z` to `a`–`z` only, bytewise, as SQLite's `sqlite3StrICmp` and
  `NOCASE` do; `compareIgnoreAsciiCase` orders as `compare` does after folding (a prefix first).
- **Splitting** gives views of the receiver's type: `splitOnce` a `TextPair<Self>` (`left`, `right`), `split` a
  `TextPieces<Self, N>`, an iterator of `Self` with `TextSplit`'s rules (§16.6: `"a,,b"` by `","` is `"a"`, `""`,
  `"b"`; an empty text one empty piece; an empty separator the text whole).
- **Characters** (`chars()`): a `TextChars<Self>`, an iterator of `TextChar { value, offset, length, valid }`.
  Over UTF-8 each is a scalar value and its bytes. Where the bytes are not UTF-8, each **maximal subpart** of an
  ill-formed sequence (the Unicode Standard's §3.9, as `toUtf8Lossy` and `chunks` use, ADR 0231 §2.3) is one
  `TextChar` with `value` U+FFFD, `valid` false, and its `offset` and `length` (1 to 3) in bytes, so nothing is
  lost: `text.slice(c.offset, c.offset + c.length)` is the bytes. Bytes one at a time were not taken: they would
  split a valid character of a `ByteStringView` into pieces that are not characters, and differ from the lossy
  conversion that a printer of the same text uses.
- **`TextBuffer.append`** appends the bytes and answers whether it did. A `ByteString` takes every byte (`true`). A
  `String` holds UTF-8 only (§23.2): a text that is not UTF-8 is not appended, and the answer is `false`, as
  `appendUtf8` answers (lib/core's `AppendText`). Nothing is replaced silently: **`appendLossy`** is the explicit
  form, which appends the text with U+FFFD for each maximal subpart, as `toUtf8Lossy` gives it, into either buffer
  (so a `ByteString` given `appendLossy` holds UTF-8 after it). `?` and `!` on `append` read as before; a function
  that returned `b.append(t)` as its own `Result<void, …>` now writes `b.append(t)?; return Ok();`.

### 2.2 A view implements an interface

`impl I for StringView { … }` and `impl I for Slice<T> { … }` (a shared slice of a non-scoped element type) are
allowed, with no region written: regions are erased in types (§25.5), and each method that returns a view of
the receiver has it by elision (§25.6) from `this`, its single scoped parameter. The orphan rule is unchanged (the
built-in types are the libraries', so a program implements its own interfaces for them). A view's methods take
`this` by value; a `borrow this` or `borrow mut this` method makes the generic code that calls it N0274 at the
view (§25.4), as before.

- **Calls on the concrete view.** A view's built-in methods (§16.6, §22.2) keep their names and their lowering:
  `s.indexOf("x")` on a `StringView` is `TextSearch.indexOf` as before, so UTF-8 code is neither slower nor
  different. A method of an interface the view implements is called on it when the view has no built-in of the
  name (`s.slice(0, 2)`, `s.chars()`), and always through a bound, through `I.m(view, …)`, and in an instance of
  generic code whose call went through a bound (ADR 0229 §2.2's record): there, `t.indexOf(n)` with `T =
  StringView` is `TextView`'s, which takes any needle, not the built-in.
- **C names.** A method of a slice is named from its mangled type (`ceeM_…Slice…`), as a vector's is (ADR 0052 §3.3).
- The same holds for `String` and the vector built-ins with `TextBuffer`: a built-in name stays the built-in on the
  concrete type, and the interface's in an instance whose call went through a bound.

### 2.3 Elision from a type parameter

A function with **no** parameter of scoped type, whose result is scoped and names no region, now takes its result's
loans from its **single** parameter whose declared type names a type parameter that may hold loans (one not bounded
by `Send` or `Share`, so with a flow name of its own, §25.6; `Self` for the receiver of an interface's method),
where it was N0245. So `fn bytes(this) -> Slice<u8>` in an interface, and `fn raw<T: TextView>(t: T) -> Slice<u8>
=> t.bytes()`, borrow what `this` or `t` holds, as they would for a view. It is sound for every instance: an instance
at a scoped `T` has the elision it would have had written for the view; at an unscoped type (`impl Raw for Owner`)
the result borrows from the by-value local `this`, so the body can return only what `this` holds (nothing, for an
owner) or static data, and `this.data[..]` is N0244 in the impl, as for any function. The one signature that
compiled before and now reads differently is a result of static views only (`fn name<T>(t: T) -> StringView`),
which was taken to borrow from nothing and now borrows what `t` holds: stricter for a caller whose `t` is a view,
the same for any other `t`. The whole corpus is unchanged by it (§7).

**Why it is sound, and only stricter** (kept at the owner's word, §8 answer 3). The rule adds a flow, result ←
`t`, and removes none, so it can only turn an accepted program into a refused one, never the reverse:
- *In the callee*, the body is checked against the flows its signature declares. With the new flow the body may
  return what `t` holds. `t` is a by-value parameter, a local of the callee: a loan of `t` itself (a slice of an
  owned field) is N0244, as for any local; what `t` holds was lent to it by the caller, so it outlives the call.
  Static data was allowed before and is still.
- *In the caller*, the result now holds the loans of the argument for `t`. For an argument that holds none (an
  owned value, an integer) nothing changes. For a view, the result is kept no longer than the view's own loans,
  which is what the callee's body may have returned, so a program that relied on the result outliving them was
  accepted only because the old signature said too little; it is refused now, with N0243 or N0244 where it would
  have read freed memory had the callee returned from `t`.
- *In an instance*, flows are the declaration's (§25.6, ADR 0131 §2.9): an instance at a scoped type has the
  elision it would have had for the view written out, and an instance at an unscoped type has a result that
  borrows from nothing it was given, which its body can produce only from static data.
- Where elision was ambiguous (two such parameters, or a scoped parameter beside one), nothing changes: N0245 as
  before.

**Elision in an instance.** An instance of `fn mk<T, N>(t: T, n: N) -> Pair<T, N>` at two views had two scoped
parameters and a scoped result with no region written, so elision failed in the instance (N0245, reported as
N0274 at the use) though the declaration's flows (result ← `t`, `n`) already said everything. A parameter scoped
only by its type argument is now no candidate for elision when the result's written type names a type parameter;
`TextView.split` (`-> TextPieces<Self, N>`) needs it.

### 2.4 `ByteStringView` and `ByteString`

`ByteStringView`'s members of §2.1 come from `TextView` and are no longer its own: `bytes`, `length`, `isEmpty`,
`byteAt`, `slice`, `sliceFrom`, `sliceTo`, `toUtf8`, `isUtf8`, `equals`, `compare`, the searches, `trim…`,
`splitOnce` and `split`. Calls read the same; what changes:

- the needles of `indexOf`, `lastIndexOf`, `contains`, `startsWith`, `endsWith`, `equalsIgnoreAsciiCase`, `equals`,
  `compare`, `splitOnce` and `split` are any `TextView`, so a literal is taken as itself (`v.startsWith("UNIQUE")`;
  ADR 0237 §5 question 2) and a `Slice<u8>` as before;
- `splitOnce` is a `TextPair<ByteStringView<r>>?` (fields `left`, `right`, as `ByteStringHalves`'s were) and `split`
  a `TextPieces<ByteStringView<r>, N>`. `ByteStringHalves` and `ByteStringSplit` are removed: nothing names them
  outside lib/core (the SQLite port, cee-proxy and this repository were searched). The owner accepted the removal
  (§8 answer 2): both shipped in 0.95.2, have no users, and CeeSafe is before 1.0.

**Source changes, for the release notes:**
- `ByteStringHalves` and `ByteStringSplit` are gone: name `TextPair<ByteStringView>` and `TextPieces` instead (or let
  `let` infer them); `left` and `right` keep their names.
- `ByteString.append` and `TextBuffer.append` answer `Result<bool, AllocError>`: `?` and `!` are unchanged, a
  `return b.append(t);` from a `Result<void, …>` function becomes `b.append(t)?; return Ok();`.
- Through `TextBuffer`, a `String` no longer takes text that is not UTF-8: `append` answers `false`; `appendLossy`
  replaces.
- A function whose only possibly-scoped parameter is a type parameter and whose result is a static view
  (`fn name<T>(t: T) -> StringView`) now ties the result to `t` (§2.3).

`ByteString`'s `append`, `appendLossy`, `bytes` and `clear` come from `TextBuffer`; `append` takes any `TextView`
(a literal, a `StringView` or a `ByteStringView`), so `b.append(other.view())` appends another byte string's bytes.

### 2.5 Dispatch and cost

Generic code is monomorphised (§20.4, §21.2): `fn f<T: TextView>(t: T)` is compiled once per text type, and its
calls are direct calls of that type's methods, which C inlines. `TextView` has generic methods, so it has no `dyn`
type (N0331), and needs none: there are three kinds of text, all known where the code is compiled. The defaults
are written over `bytes()` and lib/core's byte searches (`ByteScan`), which `TextSearch` and `ByteSearch` use too;
a `StringView`'s concrete calls are unchanged (§2.2).

### 2.6 `==` between a `ByteStringView` and text

`v == t` and `v != t`, with the `ByteStringView` on either side and a `StringView` (a string literal included) or
another `ByteStringView` on the other, compare the bytes, as `v.equals(t)` does. The checker lowers it to the two
operands' bytes (`v.bytes()`, the `StringView` as its `Slice<u8>`) compared as slices are (§16.2): no new HIR, no
call per byte. Any other operand (`v == 3`) and the orderings (`v < "b"`) stay N0207. A `ByteString` compares with
`==` as before (`@derive(Equal)`, with another `ByteString`); with text, through its `view()`.

Added for 0.95.12 (the owner, 2026-11-25): a `String` is text on the other side too. `s == v`, `v == s`, `s != v`
and `v != s` compare the string's text, lent as `s.view()` is (the `String` is not consumed), with the view's
bytes. `s < v` and the other orderings stay N0207. Optionals of these compare by ADR 0251 §2.6.

### 2.7 `v[i]` and `match` on a `ByteStringView`

- `v[i]`, `i: usize`, is the byte at `i`, a `u8`: `v.byteAt(i)`, which panics past the end as `s[i]` does. It is a
  value, not a place (`v[i] = b` is refused: a view is read-only). A range (`v[a..b]`) is not added: `v.slice(a, b)`.
- `match (v)` where an arm's pattern is a string literal (or alternatives of them, or one under `@`) matches the
  view's bytes as a `Slice<u8>` is matched (ADR 0085 §2.7): the literal's UTF-8 bytes, compared with the view's,
  and coverage as for a byte slice. A binding arm then binds the bytes, a `Slice<u8>` (`x => x.length`). A match
  on a `ByteStringView` without string patterns is a struct's match, as before.
- Added for 0.95.12 (the owner, 2026-11-25): a string pattern may also stand where a `ByteStringView` is matched
  inside another pattern: a payload (`Some("abc")` on a `ByteStringView?`, `Token.Name("from")`), a field
  (`Word { text: "hi", .. }`), alternatives of them (`Some("a" | "b")`) and `name @ "abc"`, which binds the view
  itself, a `ByteStringView`, as `Some(s @ "zz")` on a `StringView?` binds the view. It matches the view's bytes as
  on a `Slice<u8>`, lowered to a struct pattern on the view's one field (`ByteStringView { data: "abc" }`): no new
  HIR. A pattern of another type there (`Some(3)`, `Some([0x61, ..])`) stays N0205.
- A match on a type parameter bounded by `TextView` takes no string pattern (N0207, "cannot match on a value of type
  'T'", as before): generic code matches `t.bytes()`. Making it work would need a match that dispatches through a
  bound, which nothing else does; `.bytes()` is one call and the same code.

### 2.8 Any needle on a concrete `StringView` or `String`

The searches of a `StringView` and of a `String`'s text that take one text argument (`indexOf`, `lastIndexOf`,
`contains`, `startsWith`, `endsWith`, `equalsIgnoreAsciiCase`, `splitOnce`, `split`) take any `TextView`, not only
through a bound:
- a string literal or a `StringView` takes the built-in (`TextSearch`), unchanged in code and speed;
- any other `TextView` (`ByteStringView`, `Slice<u8>`) takes `TextView`'s method of the name on the `StringView`,
  with the needle's type: a match is whole characters, so `"café".indexOf(tail)` with the byte `A9` is null, and
  `splitOnce` gives a `TextPair<StringView>?`, `split` a `TextPieces<StringView, N>` (their fields and pieces read
  the same as `TextHalves`' and `TextSplit`'s);
- another type is N0205, as before (`expected 'StringView'`).

The argument is checked once, before the choice. A `Slice<u8>` receiver's built-in searches are unchanged (a literal
needle, or a `Slice<u8>`).

## 3. Consequences

- One function serves both kinds of text: `fn keyword<T: TextView>(word: T) -> bool =>
  word.equalsIgnoreAsciiCase("select")`. The SQLite engine can keep SQL as bytes and still compare names with
  literals and slice them; §6 reports what moving it would take.
- lib/core has 2 more interfaces and 4 more public structs, and 2 fewer (`ByteStringHalves`, `ByteStringSplit`);
  emit-c snapshots that print type ids may move.
- A program may implement its own interfaces for `StringView` and `Slice<T>`.

## 4. Tests

- `run-pass/adr0253-text-view.cee`: every member of both interfaces on each of the three views and two buffers,
  generic functions over them, a needle of each kind, case folding, a needle that is not UTF-8 inside a
  `StringView` (not found), the built-ins on the concrete `StringView` and `Slice<u8>` unchanged, `I.m(view)`;
  expectations by hand. Native with `--check-leaks`, and `--interp`.
- `run-pass/adr0253-text-chars.cee`, written by `tools/oracles/gen_text_view_cases.py` from Python: the characters
  of fixed and 2,000 seeded byte strings (maximal subparts from Python's decoder error spans), and
  `compareIgnoreAsciiCase`, `indexOf` and `split` against Python's on the same strings.
  Its buffers: `append` of bytes that are not UTF-8 into a `String` answers `false` and appends nothing,
  `appendLossy` replaces, into a `String` and a `ByteString`.
- `run-pass/adr0253-text-operators.cee` (§2.6 to §2.8): `==` and `!=` both ways with a literal, a `StringView` and a
  `ByteStringView`; `v[i]` in a loop; a `match` with string patterns and alternatives, and a binding arm; a concrete
  `StringView`'s and `String`'s searches with `ByteStringView` and `Slice<u8>` needles (a byte inside `é` not found,
  `splitOnce`, `split`, `lastIndexOf`, case-insensitive equality) beside literal and `StringView` ones. By hand;
  native with `--check-leaks`, and `--interp`.
- `check-fail/adr0253-text-view-errors.cee`: a `borrow this` method of an interface used at a view (N0274), an impl
  for `SliceMut<u8>` (N0267), and a `bool` needle (N0205) beside a `ByteStringView` one, which a concrete
  `StringView` and `TextView.indexOf` take.
- `check-fail/adr0253-text-operators-errors.cee`: `v == 3` and `v < "b"` (N0207), a number as a needle (N0205), a
  `match` on a `T: TextView` (N0207).
- `check-fail/adr0253-text-view-loans.cee` (borrow checking runs on a program that type-checks, so apart): an impl
  for an owning type returning a loan of its by-value `this` (N0244), and bytes from a generic `bytes()` outliving
  the text (N0243).

## 5. Alternatives not taken

- **Overloads of the searches on `ByteStringView`** (one for `StringView`, one for `Slice<u8>`): CeeSafe has no
  overloading, and it would not let a function take either view.
- **A literal converted to a `ByteStringView` where one is expected** (ADR 0231 §2.7, question 1): one conversion
  for one type; generic needles take every kind of text with none.
- **`borrow this` methods**: refused at every view (§1, item 2).
- **Regions on interfaces** (`interface TextView<region r>`): a larger change to bounds and instances, for what
  elision from a type parameter (§2.3) gives without new syntax.
- **One interface for views and owned texts**: an owned text's view borrows the owner, so its methods would take
  `borrow this` and return the concrete view type; the two shapes do not meet in one signature.
- **Bytes as characters** in `chars()`: §2.1.

## 6. The SQLite port

A scratch copy of the port, not committed there, was tried against this branch. It checks unchanged (its
`ByteStringView` calls pass `Slice<u8>` needles, which `TextView` still takes), but for one line after §2.1's
change of `append`: `src/valueops.cee:87` returns a `ByteString.append(…)` as its `Result<void, …>` (N0205; written
`…?; return Ok();`). Its tokenizer, made generic over
`T: TextView` with a `ByteString` as each token's text, checks with the same calls (`byteAt`, `slice`, `length()`,
`equalsIgnoreAsciiCase`, `append`), for a `StringView` and a `ByteStringView` alike. The first layer of what that
breaks is 51 errors: 20 `match`es on a token's text with string patterns (a `match` takes a `StringView` or a
`Slice<u8>`, so `match (t.text.bytes())`), 3 indexings and 1 `==` with a literal (`byteAt`, `equals`), and 27 places
where a token's text becomes a `String` name in the AST. Past them, names stay `String` in 144 fields and vectors
(catalog, AST, plans, messages); when those are `ByteString`, `src/sqltext.cee`'s escaping goes: `sqlTextOf` and
`sqlBytes` become copies or nothing, `hasEscapes` and the U+10FF80 mapping disappear, and its
`startsWithIgnoreAsciiCase` is `TextView`'s. With §2.6 and §2.7 (the owner's answer 4), the 20 `match`es, 3
indexings and 1 `==` of that first layer compile as they are written (measured: 27 errors left, all
`String`/`ByteString` mismatches where names enter the AST), which remain the port's work.

## 7. Evidence

Run on 2026-11-25 with the tree's `out/ceesafec`; ROADMAP §10 has the runs.

## 8. Questions for the owner, and the answers

Asked in the first report; the owner answered on 2026-11-25, and the branch carries the answers out.

1. The names `TextView`, `TextBuffer`, `TextPair`, `TextPieces`, `TextChars`, `TextChar`? **Kept.**
2. Removing `ByteStringHalves` and `ByteStringSplit` (§2.4)? **Accepted**: they shipped in 0.95.2, have no users
   outside lib/core, and CeeSafe is before 1.0. Listed among the source changes for the release notes (§2.4).
3. The elision rule of §2.3, which makes `fn f<T>(t: T) -> StringView` borrow from `t`? **Kept**; §2.3 says why it
   is sound and only stricter.
4. `match` with string patterns, `==` with text, and indexing on a `ByteStringView`? **Implemented** (§2.6, §2.7);
   on a generic `T: TextView` the `match` does not fall out (§2.7).
5. A concrete `StringView`'s searches with any `TextView` needle, not only through a bound? **Implemented**, keeping
   the whole-character rule (§2.8).
6. `TextBuffer.append` into a `String` replacing what is not UTF-8? **Changed**: it answers `false` and appends
   nothing, as `appendUtf8` does, and `appendLossy` replaces explicitly (§2.1).
