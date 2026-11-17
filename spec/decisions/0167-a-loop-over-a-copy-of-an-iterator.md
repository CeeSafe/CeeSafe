# ADR 0167 — A warning for a loop that walks a copy of an iterator read afterwards

- Status: **Accepted** by the owner, 2026-11-18 ("I accept all the ADRs as written"): every question of the
  "Questions for the owner" section as recommended. Written at the owner's request ("go ahead and write those ADRs")
  from the findings of the HTTP server written in CeeSafe (`~/dev/CeeSafeApps/httpserver/CEESAFE_FINDINGS.md`,
  S11). Not implemented yet: nothing of it is in the compiler.
- Scope: a new warning, N0407, reported where uses after a move are (`src/nir/move_check.cee`); type-system.md
  §16.4 (iterators), diagnostics.md, the guide's loops page. No change to what a program means, to the grammar, to
  the backends, to a library or to the runtime.

## 1. Context

- **The rule as it is** (type-system.md §16.4, ADR 0074 §2.8). `for (x in e)` over an iterator evaluates `e` once
  into a hidden local: "a value `e` moves into the loop; a `borrow mut` `e` is reborrowed, and the iterator keeps
  its state after the loop". For an iterator that is Copy, "moves" is a copy.
- **What went wrong.**

  ```ceesafe
  var parts = Multipart.of(body, boundary);   // every field is Copy, so the struct is
  for (part in parts) { … }                    // walks a copy of `parts`
  if (parts.failed()) { … }                    // the original: never advanced, never failed
  ```

  A body cut short was reported as well formed (httpserver, tests/form.cee): `failed()` was asked of a value the
  loop had not touched. Nothing was reported, the program ran, and the answer was wrong.
- **The two neighbours are both told.**
  - With a type that is not Copy the same program is an error: N0250, "`parts` is used here after it was moved".
    The reader is sent to `for (part in borrow mut parts)`.
  - With `for (part in borrow mut parts)` the variable itself advances, for a Copy type too (checked with ceesafec
    0.65.1: a counter read after such a loop has its last value; after `for (x in c)` it has its first).
  Only the Copy case is silent, and whether a struct is Copy is not written on it: it follows from its fields, so
  adding a `Vector` to an iterator changes this loop from silently wrong to refused.
- **Why not make it an error or change the meaning.** Walking a copy is useful and is what the rule says: lib/core's
  `TextSplit` is Copy, and `let words = text.split(","); for (w in words) { … } for (w in words) { … }` walks the
  words twice (run with ceesafec 0.65.1). Making `for` over a variable advance the variable would change programs that rely on the copy, and
  would make a `let` iterator unusable in a loop.

## 2. Decision

### 2.1 The warning

**N0407.** `for (x in e)` where

1. `e` is a place: a local, a parameter, or a field path of one (`this.parts`), not a call or another expression;
2. `e`'s type is Copy and is walked by `next` (the iterator rule; arrays, slices and ranges are not iterators);
3. the place is read after the loop, on some path, **other than as the `e` of another `for`**;

is reported at `e`:

```text
warning[N0407]: the loop walks a copy of 'parts': 'parts' itself does not advance, and is read after the loop
  = note: 'parts' is read here (12:9)
  = help: write 'for (part in borrow mut parts)' to walk 'parts' itself
```

- **Read after the loop** is the test the move checker makes for a moved value (N0250), asked of a value that was
  copied where a type that is not Copy would have been moved. An assignment to the whole place (`parts = …`) ends
  it, as it ends a move.
- **Another `for` over the same place is not such a read** (§1's last point): walking a Copy iterator again from
  its start is the common use of the copy.
- **Inside the loop's body** a read of the place is not "after the loop", and is not reported: the body sees the
  place as it was, which a reader of `for (w in words) { use(words) }` can see.

### 2.2 Silencing it

A program that means the copy says so by making it: `let first = parts; for (part in first) { … }`, after which
`parts` read later is plainly the untouched one. No attribute or comment form is added.

### 2.3 `ceesafec fix`

No fix is applied: the compiler cannot know which of the two was meant (§2.2). The help line is the suggestion.

## 3. Consequences

- The program of §1 is told at the loop, with the line that reads the stale value.
- **False positives** are possible where a program reads a Copy iterator's own field after walking a copy on
  purpose (`for (x in counter) { … } log(counter.end)`). §2.2 is the way out, one line.
- **The libraries and the corpus** are searched before the warning lands; each report is either a fault or gets
  §2.2's line, and the count goes in ROADMAP §10. The compiler's own sources are warning-free today and stay so.
- A warning does not fail a build; `--deny-warnings`, where a build uses it, does.

## 4. Questions for the owner

1. A warning, not an error and not a change of meaning (§1, last point)? Recommended: yes.
2. Another `for` over the same place does not count as a read (§2.1)? Recommended: yes; without it the warning fires
   on the idiom the copy exists for.
3. Fields of `this` and of other places are places for rule 1, not only locals? Recommended: yes: an iterator kept
   in a struct is where the stale read is hardest to see.

## 5. Implementation plan (once accepted)

1. Spec: §16.4 (a sentence and the example), diagnostics.md (N0407, with the three conditions), the guide's loops
   page ("walking a variable itself").
2. `src/nir/`: the loop's header marks the copy of an iterator place as "would be a move"; the move check's
   use-after query runs for it, skipping reads that are another loop header's copy; N0407 with the note.
3. Tests:
   - check-pass with the warning annotated: the program of §1; a field of `this`; a read on one path only;
   - check-pass with no warning: a second `for` over the place; the place assigned before it is read; a read
     inside the body; `for (x in borrow mut it)`; a temporary (`for (w in text.split(","))`); a `let` copy (§2.2);
     arrays, slices and ranges;
   - the corpus, the libraries and the compiler's sources without new warnings, or each one listed and decided.
4. ROADMAP §10.
