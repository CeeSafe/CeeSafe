# ADR 0262 — A borrowed local is not zeroed

- Status: **Accepted** by the owner, 2026-11-26 ("Go ahead with real fix for finding 55", the SQLite port's
  `CEESAFE_FINDINGS.md` finding 55, after its benchmark's profile). **Implemented** on the branch `perf/f55-locals`
  (from 0.95.17, with 0.95.18 and 0.95.19 merged), released in 0.95.20.
- Scope: the C backend's rule for which aggregate locals start zeroed (`partlyWritten`, `src/backend/c_body.cee`,
  which sync and async functions share); c-backend.md §5; 16 `emit-c` snapshots. No language, library or runtime
  change.

## 1. Context

Since M9d an aggregate local (a struct, enum, array, slice, vector, …) is declared without an initializer when it is
assigned whole before every read; ADR 0204 took the same rule to resume functions and dropped the zeroing of a local
whose variant payload is read. Three reasons still kept `= {0}`: a write to a part (a field, an element, a payload), an
`Unpack` target, and a borrow of the local or of a part (`&x`, `x.e + i`), shared or mutable.

The SQLite port's benchmark found the last one costly. Its expression evaluator is a `match` over 30 kinds, and most
arms bind a value and lend it (`let v = evaluate(…)?; return Ok(negate(borrow v));`); every such local was zeroed on
every call, about 800 bytes in fifty 16-byte stores before the `match` looked at its expression, a sixth of a table
scan's time. The nested-loop function, entered once for each row, cleared 2 KB. Of the 2 469 zeroed locals in the
engine's C, 2 073 were zeroed only because they are borrowed.

A borrow cannot be the reason a local is read before it is written. Definite assignment (the checker's, and the
interpreter's on every run) allows a borrow, shared or mutable, only of a local assigned whole, so whatever the borrow
reads was written, and what it writes is written over a whole value.

## 2. Decision

- A borrow of a local, or of a part of it, no longer keeps its zero initializer. A local is zeroed when a part of it
  is written other than by assigning it whole, or it is an `Unpack` target; else it is declared without one.
- References and scalars keep their zero initializer, as before: the C compilers remove those stores.

## 3. Consequences

- No program's behaviour changes: a borrowed local was assigned whole before its borrow.
- In the SQLite port's C (bench/, 37 000 lines of CeeSafe), zeroed aggregate locals fall from 2 469 to 396.
- 16 `emit-c` snapshots lose 24 initializers (`= {0};` becomes `;`), a reviewed edit (§4).

## 4. Tests

- The `emit-c` family: each differing line of the 16 snapshots was checked to differ only by declarations whose
  ` = {0}` (or ` = ceeZ_…()`) the new compiler leaves out, declaration by declaration after the runner's
  normalisations, and the snapshots were edited by deleting those initializers from their own text, not by copying
  the compiler's output. Every one of the 24 is a local assigned whole before its first borrow (an arena from
  `Arena.create`, a closure's environment from one literal, the rest from a value).
- `run-pass/adr0262-borrowed-locals.cee`: locals of every aggregate kind lent in arms, loops and early returns,
  natively (gcc and clang, -Werror, AddressSanitizer and UndefinedBehaviorSanitizer), in the interpreter and with
  `--check-leaks`, and the run-pass family under Valgrind's memcheck, which reports a read of a byte never written.
- The contract suite on Linux x86-64 (§5).
