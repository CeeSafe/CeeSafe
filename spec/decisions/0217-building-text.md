# ADR 0217 — Building text: a chainable appender, and why not interpolation

- Status: **Accepted** by the owner, 2026-11-24 ("I accept ADRs 0215–0217"), §6's questions as recommended: the
  appender with the proposed names (`add()`, `text`, `char`, `number`, `signed`, `hex`, `padded`, `fixed`, `done()`),
  in lib/std for `String` and in lib/core for `Vector<u8>`; a warning for an appender left without `done()`; no
  interpolation and no format strings. Not implemented yet. Proposed 2026-11-23 at the owner's instruction ("continue
  with 3.9": the optimization and DX roadmap's Phase 3, item 9).
- Scope: lib/std's `String` and lib/core's `Vector<u8>` text helpers (ADR 0087 §2.9, ADR 0138 §6); diagnostics.md
  N0400; the guide's collections page, where strings are.

## 1. Context

There is no string interpolation: `"x = {x}"` prints the braces, with warning N0400. Text is built one call at a
time, each with its own `?` or `!`:

```ceesafe
line.append(if (response.isHttp2()) "HTTP/2 " else "HTTP/1.1 ")!;
line.appendNumber(response.status() as u64)!;
line.append(", ")!;
line.appendNumber(response.body().length as u64)!;
line.append(" bytes")!;
```

(httpclient's example; nine such lines for one line of output.) Counted on 2026-11-23 over the applications (1,651
files, 674,164 lines):

| | Count |
|---|---|
| `.append(` calls | 8,007 |
| runs of four or more consecutive append or write lines | 446 |
| the longest run | 33 lines |
| uses of `TextWriter` (a fixed buffer, no allocation) | 207 |

This is the one ergonomic gap of the three in the roadmap's item 3.9 that programs pay for on most pages.

## 2. Decision

### 2.1 A chainable appender in the library

A value made from a `String` (or a `Vector<u8>`) whose methods append and return it, and which remembers a failed
allocation instead of returning it at every step:

```ceesafe
line.add().text(if (http2) "HTTP/2 " else "HTTP/1.1 ").number(status).text(", ").number(length).text(" bytes").done()?;
```

- One `?` (or `!`) for the line, at `done()`. After a failed step the later steps do nothing; `done()` returns
  `Err(AllocError.OutOfMemory)`. What was appended before the failure stays, as it does today when a later
  `append` fails.
- Its methods are the appends that exist: text, a character, a number (decimal, signed, hex, padded), a float with
  digits. Nothing is formatted by a format string.
- **No language change.** Tried with ceesafec 0.94.4 as ordinary code: a struct holding `borrow mut(r) String` and a
  flag, methods `fn text(borrow mut this, s: StringView) -> borrow mut Appender`, and
  `fn add<region r>(out: borrow mut(r) String) -> Appender<r>`. The chain above compiles, runs in the interpreter
  and natively, and leaves no allocation live.
- A value that is not used to its `done()` loses an error silently. N0402 (an unused `Result`) does not see it,
  since the chain's value is the appender. The appender's `drop` cannot report. This is the design's one cost, and
  question 2.

### 2.2 No interpolation

`"x = {x}"` stays text, and N0400 stays a warning:

1. **Where the text goes and what a failure does have no place in a literal.** Interpolation makes a `String`
   (which allocator? what if it fails?) or needs a sink (then it is §2.1 with different punctuation). Every
   existing append says both.
2. **A format mini-language** (`{x:>8}`, `{n:#x}`) is a second syntax inside string literals, checked by a second
   set of rules. The appender's methods are ordinary calls, completed and documented as calls are.
3. `Console.writeLine` takes one value; the cases where interpolation is most missed (a line of log) are served by
   §2.1 into a reused `String`, or by `TextWriter` where nothing may allocate.

## 3. Options not taken

- **`String.format(template, …)`** with a checked template. It needs variadic arguments or an array of a formatting
  interface (`dyn`), and the mini-language of §2.2's second reason.
- **Interpolation that lowers to §2.1** (`line.add("status {status}, {length} bytes")?`). The sink and the failure
  are then said; the mini-language question remains. It can be added on top of §2.1 later without changing it.
- **Methods that return `Result<borrow mut String, AllocError>`** so that `a.append(x)?.append(y)?` chains. A `?`
  for each step is what there is now, on one line.

## 4. Consequences

- lib/std gains one struct and one method on `String`; lib/core the same for `Vector<u8>` if question 3 says so.
- The guide's section on strings (guide/collections.md) shows the chain first; N0400's explanation points to it.

## 5. What is not known

- The cost of the chain against the separate appends: each step tests the flag. Not measured.
- Whether `done()` should be required by the checker (question 2).

## 6. Questions for the owner

1. Add the appender (§2.1)? Recommended: yes. Names: `add()`, `text`, `char`, `number`, `signed`, `hex`, `padded`,
   `fixed`, `done()` are a proposal; the owner's choice.
2. An appender dropped without `done()`: accept the silent loss, warn (a new warning for a value of a type marked
   as one that must be finished), or make `drop` panic when a step failed? Recommended: a warning, as N0402 is for
   a `Result`.
3. For `Vector<u8>` too, in lib/core? Recommended: yes, the same names.
4. No interpolation and no format strings (§2.2)? Recommended: yes for now; §3's second option stays open.
