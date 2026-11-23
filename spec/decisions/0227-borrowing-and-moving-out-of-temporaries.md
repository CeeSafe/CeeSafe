# ADR 0227 — Borrowing a temporary, and moving a field out of an unwrapped one

- Status: **Proposed** 2026-11-24, under the owner's instruction to fix the type-checker findings of the SQLite port
  (`CeeSafeApps/sqlite/CEESAFE_FINDINGS.md` findings 28 and 33). Implemented 2026-11-24 on `fix/findings-types`.
- Scope: type-system.md §13.2 (creating references), §14.3 (moves); diagnostics.md N0247.

## 1. Context

- **Finding 28.** `map.contains(borrow 1u32)` was N0247, so a lookup by a constant key needed a local first. A
  receiver that is a temporary was already lent for its statement (ADR 0073 §2.1: `make().view()`), and so was a value
  passed for a `borrow T` parameter without the word; only the explicit `borrow e` refused.
- **Finding 33.** `var seek = replace(mut best, null)!.seek;` was N0251 ("cannot move … out of an element or enum
  payload of 'a temporary'"): the move checker sees a field of the optional's payload, a projection it does not track
  for partial moves, so it refuses a move from it.

## 2. Decision

### 2.1 `borrow e` of a value lends a temporary for the statement

`borrow e`, where `e` is no place and is not reached through a reference (a literal, a call's result, a field or
element path from one), is a shared borrow of a temporary that lives until the end of its statement, the borrow a
receiver gets (BorrowTemp; ADR 0073 §2.1). Using the loan after the statement is N0243, as for `make().view()` kept in
a `let`. `borrow mut e` and `mut e` of a value stay N0247: what the callee changes would be dropped unseen, which is
more likely a mistake than an intent.

### 2.2 A field of an unwrapped temporary moves out of a hidden local

`e?.f` and `e!.f`, where `e` is no place and `f` is not Copy, are checked as `{ let unwrapped = e?; unwrapped.f }`:
the unwrapped value is held in a hidden local of the block, `f` is moved out of it as from any struct local (a
partial move, so a `drop` block on the path is N0252), and the rest is destroyed at the block's end, which is the end
of the expression. The move checker and the drop elaboration see an ordinary local and an ordinary partial move;
nothing about temporaries changes. For a place, `opt!.f` stays N0251: `opt!` consumes `opt`, and taking one field of
it that way is more likely a mistake than writing `let p = opt!;` first.

## 3. Options not taken

- **`borrow mut` of a temporary** (Rust's `&mut 1`): see §2.1.
- **Tracking payload projections as move paths** (so the NIR temporary of `e!` itself is partly moved): a change to
  move paths and drop elaboration for every enum, where the hidden local gives the same result from the parts that
  already exist.

## 4. Implementation and tests (2026-11-24)

- `synthBorrow` (src/check/references.cee) lends a temporary through `borrowReceiver`; the field case of
  `synthMember` (src/check/expr.cee) builds the block. The interpreter runs the same HIR, so it needed no change.
- `run-pass/adr0227-temporaries.cee`: `borrow 1u32` and `borrow forty()` passed; `make()!.kept`,
  `replace(mut slot, null)!.other` and `tried()?.kept` with fields whose `drop` blocks print, so the output shows each
  value destroyed exactly once and the rest of the unwrapped value at the end of its expression (the order worked by
  hand from §14.5). `check-fail/adr0227-temporaries.cee` (N0247 for `borrow mut five()` and `mut five()`),
  `check-fail/adr0227-kept-temporaries.cee` (N0243 for a kept loan, N0251 for `opt!.name` of a place).
- A reviewed change to an expectation: `check-fail/borrow-expressions.cee` had `let a = borrow 5;` and
  `let b = borrow five();` as N0247; they are valid now, so those lines borrow mutably (still N0247), and a shared
  `look(borrow five())` was added beside them.
