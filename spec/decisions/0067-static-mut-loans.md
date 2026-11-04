# ADR 0067 — A write to a `static mut` carries no loan

- Status: **Accepted** by the owner, 2026-11-04 ("fix the static mut hole too"). Implemented on the working tree;
  not released. Found while testing ADR 0066.
- Scope: the borrow checker (`src/nir/borrow.cee`, `checkStaticWrites`). Spec: type-system.md §39.3, diagnostics.md
  (N0272). No new code.

## 1. Context

- **A `static mut` is the place `*S`, with `S: borrow(static) mut T`** (§39.3). Every use is unsafe, but the unsafe
  block only makes the use unsafe (two uses may alias, two threads may race). It is not meant to switch off the
  borrow checker.
- **Release 0.11.0 accepts a dangling view in a static.** It compiles, and printing `LAST` afterwards reads freed
  memory:

  ```ceesafe
  static mut LAST: StringView = "none";
  /// Safety: …
  fn keep(s: borrow String) { unsafe { LAST = s.view(); } }
  ```

- **Why the checker misses it.** The borrow checker records the write as a store through the reference `S`
  (`collectWrites`, §25.7 rule 2). N0272 then checks such stores only against the parameters a reference was
  borrowed from (`checkParameterWrites`). `S` comes from `StaticAddress`, so it points to no loan, and the store
  was never checked.
- **Only `StringView` is affected.** It is the one scoped type a `static mut` may hold: N0337 rejects every other
  scoped type, and ADR 0066 keeps arrays of views out of `static mut`. References to references are not supported,
  so a view in a static cannot be reached any other way.

## 2. Decision

- **The rule.** A value written to a static, through its address local `S` (`S = StaticAddress(s)`), may carry no
  loan. Any loan in what it stores is N0272: "this stores a value borrowed from 's' into static 'LAST', which
  outlives every borrow; a static may hold only static data, such as a string literal". When the loan is of a place,
  a note points at the borrow.
- **Still accepted: static data,** which carries no loan:
  - a string literal and a constant view;
  - a view read from another static or from a static table (`LEVELS[1]`);
  - a call's result when its arguments are static data.
- **Rejected:** a view of a local, of a parameter (including through a call or a subview), and of a parameter
  declared `StringView<static>`.
- **Why `StringView<static>` parameters are rejected.** A call does not check that the argument of a
  `StringView<static>` parameter is static data: `take(s.view())` with a local `s` compiles. Trusting that region
  here would reopen the hole through the caller. The unchecked call is a separate gap, recorded in §4.
- **Why N0272 and not a new code.** N0272 is "storing loans where the signature does not let them flow". A static
  is the extreme case: nothing may flow there.

## 3. Evidence (macOS arm64)

- **`check-fail/static-mut-loans`:** five writes rejected (a local, a parameter, through a call, a subview, a
  `StringView<static>` parameter).
- **`run-pass/static-mut-views`:** six accepted writes of static data, and a `static mut` integer, alongside them.
  Its expected output comes from a Python model. It passes compiled and with `--interp`.
- **On 0.11.0, both tests fail:** the check-fail test because the hole compiles, and the run-pass test because it
  uses ADR 0066's static table.
- **The full corpus, the fixed point and the contract suite** are recorded in ROADMAP §10.

## 4. Consequences

- **Programs this change breaks.** None of the repository's packages writes a `static mut` view. A program that
  stored a non-static view this way was already unsound, and now gets N0272.
- **Open: `StringView<static>` parameters** (§2). Either a call checks that the argument is static data (N0243
  then, as for a returned local), or a written `static` region on a parameter becomes N0273. Until then, a value
  that must be stored in a static has to be static data at the point of the write.
