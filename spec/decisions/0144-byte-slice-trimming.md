# ADR 0144 — Trimming byte slices: the methods exist; N0200 names them

- Status: **Accepted** by the owner, 2026-11-15: asked about ClayWind's findings, the answer was "0.49.1 now, K1/K2
  too". Reported by ClayWind (`~/dev/CeeSafeApps/Clay/CEESAFE-FINDINGS.md` K2). Implemented on branch
  `adr/0143-0144-k1-k2`.
- Scope: the checker's N0200 messages for a method that a slice, a vector, a `String` or a `StringView` does not have
  (`src/check/vectors.cee`, `src/check/bytes_text.cee`); guide/collections.md; the book's §13.2 (a message it
  prints); the corpus. No new method, no change to lib/core, the C backend, the interpreter or the runtime.

## 1. Context

- **The finding.** "`Slice<u8>` has no `trim`": parsing Unicode's test files (fields like ` 2 ` between semicolons)
  needs one, and ClayWind's tests skip spaces by hand. Proposed: `trimmed()` (ASCII whitespace) on `Slice<u8>`.
- **What CeeSafe has** (ADR 0071 §2.4, type-system.md §16.6). Byte slices (`Slice<u8>`, `SliceMut<u8>`, an array of
  `u8` through its slice, a `Vector<u8>`'s elements) have `trim()`, `trimStart()` and `trimEnd()`, written in lib/core
  (`lib/core/text.cee`, `ByteSearch<r>`) and reached as methods like `sortBy`. Each returns a `Slice<r, u8>` of the
  receiver's region, so the result borrows what the receiver borrows, and drops space, `\t`, `\n`, `\v`, `\f` and
  `\r`. They are `StringView`'s `trim`, `trimStart` and `trimEnd` (`TextSearch<r>`), with the same names, and come
  with `indexOf`, `splitOnce`, `split`, `parseInt` and the rest. ceesafec 0.49.0 runs them: on 2026-11-15, a program
  calling `s.trim()`, `s.trimStart()` and `s.trimEnd()` on a `Slice<u8>`, an array, a `SliceMut<u8>`, a
  `Vector<u8>` and through a function returning `line.trim()` printed the expected lengths (`ceesafec run --interp`).
- **Why the finding was made.** The message for a method a slice does not have lists only the built-in slice methods:

  ```text
  error[N0200]: 'Slice<u8>' has no method 'trimmed'; slices have 'copyFrom', 'fill', 'swap', 'sortBy', 'forEach',
  'any', 'all', 'partitionPoint', 'splitAt' and 'splitAtMut'
  ```

  So it reads as the whole API of a byte slice, and it leaves out `copyWithin` (ADR 0138 §6) as well. The same holds
  for a `Vector<u8>` (whose list has no byte methods) and a `String` (none of its searches), and a `StringView`'s says
  only "the searches and parsers of ADR 0071". None of them can answer "did you mean 'trim'?", because `trim` is in
  no list the suggestion searches (`src/check/suggest.cee`, `appendEveryMethodChoice`).

## 2. Decision

- **No new method.** Byte slices keep `trim()`, `trimStart()` and `trimEnd()`, the names `StringView` has. A second
  spelling (`trimmed`) for the same operation, on bytes only, would make the two APIs differ for no gain.
- **N0200 lists the methods a receiver has, and suggests a close one.** For a method a receiver does not have, the
  message lists, in this order:
  - a slice: `copyFrom`, `fill`, `copyWithin`, `swap`, `sortBy`, `forEach`, `any`, `all`, `partitionPoint`,
    `splitAt`, `splitAtMut`; a byte slice, then also `readBE`, `readLE`, `writeBE`, `writeLE`, `indexOf`,
    `lastIndexOf`, `indexOfByte`, `contains`, `startsWith`, `endsWith`, `equalsIgnoreAsciiCase`, `trim`,
    `trimStart`, `trimEnd`, `splitOnce`, `split`, `parseInt`, `parseIntRadix`, `parseFloat`;
  - a vector: its built-in methods as before; a `Vector<u8>`, then also `pushBE`, `pushLE` and the byte methods;
  - a `String`: `append`, `appendNumber`, `clear`, `view`, `bytes`, `clone`, then the searches, trims, splits and
    parsers;
  - a `StringView`: `bytes`, then the searches, trims, splits and parsers;

  then lib/core's impl methods on the type, as before. When a name in the list is close (the "did you mean" rule of
  `isCloseName`), the message names up to three of them instead of the list:

  ```text
  error[N0200]: 'Slice<u8>' has no method 'trimmed'; did you mean 'trim' or 'trimEnd'?
  error[N0200]: 'Slice<i32>' has no method 'map'; it has 'copyFrom', 'fill', 'copyWithin', 'swap', 'sortBy', 'forEach',
  'any', 'all', 'partitionPoint', 'splitAt' and 'splitAtMut'
  ```

- The lists live in `src/check/bytes_text.cee`, beside the names the method dispatch tests (`isSearchMethod`,
  `isParseMethod`), so that a method added to one is added to the other.
- guide/collections.md says what byte slices have, with an example that trims and parses fields.

## 3. Alternatives considered

- **Add `trimmed()`** (the finding's proposal), or `trimmed`/`trimmedStart`/`trimmedEnd` beside `trim`: two names for
  one operation, and `StringView` would then need them too to stay consistent.
- **A built-in slice method instead of lib/core's:** the byte methods are lib/core functions on purpose (ADR 0071
  §2.4: one implementation for bytes and text, read by the interpreter as by the C backend); nothing about trimming
  needs the checker.
- **Only fix the documentation:** the guide's slice section did not mention the byte methods, but the message is
  what a programmer reads at the moment of the mistake, and it said the method set was complete.

## 4. Compatibility

Only N0200's wording changes, for a method a slice, vector, `String` or `StringView` does not have. The code, the span
and which programs compile are unchanged.

## 5. Tests

- Corpus `run-pass/adr0144-byte-trimming.cee`: `trim`, `trimStart` and `trimEnd` on a `Slice<u8>`, a `SliceMut<u8>`,
  an array, a `Vector<u8>`, an all-whitespace slice and an empty one, each of the six ASCII whitespace bytes and a
  non-ASCII byte that is kept, and a function that returns `line.trim()` of its parameter (the result's region).
- Corpus `check-fail/adr0144-method-lists.cee`: `trimmed` on a `Slice<u8>`, a `Vector<u8>`, a `String` and a
  `StringView` (N0200), and `trim` on a `Slice<u32>` (N0200, "a method of 'Slice<u8>' and 'StringView'").
- The book's Listing 13-16 prints the new message (tools/book_check.py).

## 6. Implementation and evidence

See ROADMAP §10's row for the tests run.
