# ADR 0253 — Text of either kind: `TextView` and `TextBuffer`

- Status: **Proposed — implemented** 2026-11-25 on the branch `fix/g5-text` (from 0.95.10), for the SQLite port's
  `CEESAFE_FINDINGS.md` finding 41. Not released. §8's questions are the owner's.
- Scope: lib/core `text_view.cee` (new: `TextView`, `TextBuffer`, `TextPair`, `TextPieces`, `TextChars`,
  `TextChar`) and `bytestring.cee` (`ByteStringView`'s and `ByteString`'s members through them); the checker
  (`src/check/interfaces.cee`, `members.cee`, `same_name_methods.cee`, `references.cee`) and the C names of methods
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
    fn append<N: TextView>(borrow mut this, text: N) -> Result<void, AllocError>;
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
- **`TextBuffer.append`** appends the bytes. A `String` holds UTF-8 only (§23.2), so into a `String` a text that is
  not UTF-8 is appended as `toUtf8Lossy` gives it, each maximal subpart as U+FFFD; a `ByteString` takes every byte.

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
  outside lib/core (the SQLite port, cee-proxy and this repository were searched).

`ByteString`'s `append`, `bytes` and `clear` come from `TextBuffer`; `append` takes any `TextView` (a literal, a
`StringView` or a `ByteStringView`), so `b.append(other.view())` appends another byte string's bytes.

### 2.5 Dispatch and cost

Generic code is monomorphised (§20.4, §21.2): `fn f<T: TextView>(t: T)` is compiled once per text type, and its
calls are direct calls of that type's methods, which C inlines. `TextView` has generic methods, so it has no `dyn`
type (N0331), and needs none: there are three kinds of text, all known where the code is compiled. The defaults
are written over `bytes()` and lib/core's byte searches (`ByteScan`), which `TextSearch` and `ByteSearch` use too;
a `StringView`'s concrete calls are unchanged (§2.2).

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
- `check-fail/adr0253-text-view-errors.cee`: a `borrow this` method of an interface used at a view (N0274), an impl
  for `SliceMut<u8>` (N0267), and a `ByteStringView` given to a concrete `StringView`'s built-in `indexOf` (N0205,
  unchanged) beside `TextView.indexOf`, which takes it.
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

A scratch copy of the port, not committed there, was tried against this branch: §7 of the report in ROADMAP §10
says what it showed. In short, `src/sqltext.cee`'s escaping can go once SQL stays a `ByteString`/`ByteStringView`;
the tokenizer and the name comparisons become generic or byte-typed with the same calls; slicing (`s[a..b]`) and
`==` with a literal are the remaining rewrites, since a library type has neither operator.

## 7. Evidence

Run on 2026-11-25 with the tree's `out/ceesafec`; ROADMAP §10 has the runs.

## 8. Questions for the owner

1. The names `TextView`, `TextBuffer`, `TextPair`, `TextPieces`, `TextChars`, `TextChar`.
2. Removing `ByteStringHalves` and `ByteStringSplit` (§2.4) rather than keeping them unused. Recommended: remove.
3. `==` between a `ByteStringView` and a literal, and `v[a..b]` on a `ByteStringView`: operators on a library type
   are a language question of their own (§6). Recommended: a later ADR if the port's move asks for it.
4. `TextBuffer.append` into a `String` replaces what is not UTF-8 (§2.1). Recommended: keep; a `Result<bool, …>`
   would make every `ByteString` append answer a question it cannot fail.
