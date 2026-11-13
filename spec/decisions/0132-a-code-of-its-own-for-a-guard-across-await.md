# ADR 0132 — A code of its own for a `RefCell` guard across `await`: N0405

- Status: **Accepted** by the owner, 2026-11-13 ("Go ahead and tackle those still open"), implemented on branch
  `fix/open-messages`; released in ceesafec 0.43.0. Raised by the writer of the book's Appendix F, whose row for N0402 had to
  describe two unrelated warnings.
- Scope: the number one warning is reported with (`src/nir/effect_check.cee`); `spec/diagnostics.md`,
  `spec/type-system.md` §41.3; the test `check-pass/refcell-await.cee`; the book's Appendix F. No change to when
  either warning is given, to its text or to what a program means.
- Numbering: written as ADR 0129 on branch `fix/open-messages` and renumbered before any release (the owner,
  2026-11-13: "go"). 0129 was the number of the withdrawn ADR "which unwraps a sweep sets apart" (branch
  `impl/0129`, reverted the same day: a sweep's unwraps stay set apart by file, ADR 0075 §2.16) and is not used
  again.

## 1. Context

Two warnings were reported as N0402:

- a `RefCell` guard live where a task may suspend (ADR 0084 §2.6, ceesafec 0.25.0);
- a call statement whose `Result` nothing reads (ADR 0105, ceesafec 0.42.0), which was given the same number
  without the first being seen: `diagnostics.md` then had two rows for N0402 in two tables.

The two rows were later made one that began "Two warnings share this code". A code is what a program holds on to: `ceesafec explain N0402`
printed one row for both, a script that reads the JSON document (`tooling.md` §5.1) could not tell a lost error from
a guard held too long, and any later way to allow or deny one warning by its code would have had to take both.

## 2. Decision

2.1 The guard's warning is **N0405**, the next free warning (N0403 is ADR 0113's, N0404 ADR 0117's). Its text,
its place (the statement that may suspend) and its rule (`type-system.md` §41.3) are unchanged.

2.2 **N0402** is the unused `Result` only (ADR 0105). It keeps the number because it is by far the more often seen
of the two: the book's chapters 8, 9, 11, 16, 17 and 21 name it for the unused `Result` (none names it for the
guard), and every program that drops a `Result` meets it, while the guard's warning needs a `RefCell`, a guard in a
local and an `await`.

2.3 `diagnostics.md` has one row for each, and N0405's row says what it was reported as before.

## 3. What a program or script can notice

A warning that was `warning[N0402]: a RefCell guard, … is live where the task may suspend` is now
`warning[N0405]` with the same text and position; in the JSON document its `"code"` is `"N0405"` and its `"summary"`
is its own row's. Nothing else changes: it is a warning as before, so no command's exit status changes, and with
`--deny-warnings` it is an error as before. A script that matched `N0402` to find this warning must match `N0405`.

## 4. Alternatives

- **Move the unused `Result` instead**, since the guard had the number first. Refused for §2.2's reason: it would
  change the code the book teaches in six chapters, to keep one that few programs meet.
- **Keep one code and tell the two apart by the message.** That is what §1 describes; a message is not a contract.

## 5. Tests

`tests/reference/tests/corpus/check-pass/refcell-await.cee` expects `warning[N0405]` at both guards; the tests of
N0402 (`check-pass/unused-result-warning.cee` and the others) are unchanged.
