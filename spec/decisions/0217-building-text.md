# ADR 0217 — Building text: a chainable appender, and why not interpolation

- Status: **Accepted** by the owner, 2026-11-24 ("I accept ADRs 0215–0217"), §6's questions as recommended: the
  appender with the proposed names (`add()`, `text`, `char`, `number`, `signed`, `hex`, `padded`, `fixed`, `done()`),
  for `String` and `Vector<u8>` (both in lib/std, §7.1); a warning for an appender left without `done()`; no
  interpolation and no format strings. **Implemented** the same day (§7), not released. Proposed 2026-11-23 at the owner's instruction ("continue
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

## 7. The design as built (2026-11-24)

### 7.1 The appender

- `s.add()`, on a `String` or on a `Vector<u8>` whose allocator is the system heap, takes no
  arguments and returns a `TextAppender<r>` or a `ByteAppender<r>` that holds `s` borrowed mutably. It is checked as
  the call `TextAppend.add(mut s)` or `ByteAppend.add(mut s)`, as `s.split(…)` is TextSearch's (ADR 0071 §2.4).
- Each method appends as the method it names does, unless an earlier step failed, and returns the appender (`borrow
  mut`, its region elided), so the next step chains. A step whose allocation fails appends nothing more and marks the
  appender; what earlier steps appended stays.

| Step | Appends as |
|---|---|
| `text(s: StringView)` | `append(s)` |
| `char(c: u32)` | `appendChar(c)`: nothing for a value that is not a Unicode scalar value, which is not a failure |
| `number(n: u64)`, `signed(n: i64)` | `appendNumber(n)` |
| `hex(value: u64, minWidth: u32, upper: bool)` | `appendHex` |
| `padded(text: StringView, width: usize, align: TextAlign, fill: u32)` | `appendPadded` |
| `fixed(value: f64, digits: u32)` | `appendFixed` |
| `done() -> Result<void, AllocError>` | nothing: `Err(AllocError.OutOfMemory)` if a step failed, else `Ok()` |

- The parameters are the existing appends' own, so a chain and a run of appends say the same things.
- **Both appenders are in lib/std**, not the `Vector<u8>` one in lib/core as §6's third question had it: a `Vector`
  is hosted only (profiles.md §4.1), so in lib/core, which freestanding builds check too, its methods could not be
  checked, and no freestanding program could make one. A hosted program sees no difference.

### 7.2 N0408: an appender left without `done()`

A statement made of a chain of method calls whose first call is `x.add()` with no arguments, and whose value is an
appender (or a reference to one), is warning N0408: the appender is made in the statement and dropped at its end
without `done()`, so a failed allocation in the chain is lost. The fix is to end the chain with `.done()?` (or `!`).

- Only a chain that starts with `add()` is reported. An appender kept in a variable and fed a step at a time
  (`let a = s.add(); a.text(x); … a.done()?;`) has statements whose value is the appender, and is not: its `done()`
  is in another statement, and the warning would be wrong for every one of them.
- A chain that ends in `done()` without `?`, `!` or `match` leaves a `Result` unread: N0402 reports it, as for any
  call.
- A warning, like N0402 (ADR 0073 §2.4), reported once per statement for the root package's files.

### 7.3 Implementation notes

- lib/std's `format.cee`: `TextAppend<r>.add` and `TextAppender<r>` for a `String`, `ByteAppend<r>.add` and
  `ByteAppender<r>` for a `Vector<u8>` (whose numbers go through lib/core's `TextWriter`, with no allocation of
  their own). The checker routes `s.add()` (`checkAppenderStart`, src/check/bytes_text.cee) as it routes `pushBE`;
  `add` is among a `String`'s and a byte vector's method names, so a close name finds it. N0408 is noted in
  `checkExpressionStatement` and reported with N0402 (`warnUnfinishedAppenders`).
- **Verified** (Mac, 2026-11-24): the corpus, 1,149 files, every family passing, with `run-pass/text-appender.cee`
  (its expected lines are Python's formatting of the same values) and `check-pass/unfinished-appender-warning.cee`
  (its `// warning[N0408]` annotations written by hand; its `.hir` snapshot is the compiler's dump, reviewed); the
  new test in the interpreter with `--check-leaks`; `tools/fault_injection.py` on it, 18 failure points, 0 failing;
  `guide_check` (195 examples), `spec_check`, `cli_check`, `mcp_api_check`, 0 problems; `book_appendix_check`, no
  problem but the six of the tree before the change (release rows for tags made on main since); the fixed point.

