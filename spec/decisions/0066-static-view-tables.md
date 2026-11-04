# ADR 0066 — Static tables of views

- Status: **Accepted** by the owner, 2026-11-04 ("fix the static view table finding in CeeSafe"). Implemented on the
  working tree; not released. Raised by the ClayWind cleanup on 0.11.0 (`~/dev/CeeSafeApps/CEESAFE-FINDINGS.md` §G).
- Scope: the checker (`src/check/statics.cee`). Spec: type-system.md §10 and §39.2. No new diagnostic.

## 1. Context

ADR 0064 §2 allowed constant tables of views, such as `const LEVELS: [StringView; 3] = ["Low", "Medium", "High"];`.

- **A constant is a value, not a place** (§10). It can be indexed and iterated, but slicing it or taking its
  `.length` is N0247 or N0216 until it is bound to a local.
- **The ClayWind port passes tables to the controls as `Slice<StringView>`,** so every such call needed an extra
  line: `let prioItems = PRIO_ITEMS;` and then `prioItems[..]`.
- **A `static` is a place** (§39.2), which is what such a table wants. But `static LEVELS: [StringView; 3]` was
  N0337: "'[StringView; 3]' is not 'Share'".
- **The check makes an exception for a bare `StringView`.** `static NAME: StringView` is accepted, as the spec's own
  example shows, because "a view of static string data … is safe to share". The exception did not extend to an
  array of the same views.

## 2. Decision

- **What a `static` may hold.** In a `static` (not a `static mut`), `T` may also be `[StringView; N]`. This is the
  same set of types a constant view may have, `isConstantView` (`src/check/declare.cee`), so constants and statics
  agree.
- **Its initialiser** is unchanged (§39.4): an array literal or repeat of string literals or constant views.
- **What it allows.** The table is a place, so it is sliced, measured and borrowed where it is named:
  - `f(LEVELS[..])` and `LEVELS.length`;
  - `borrow LEVELS`, as `borrow(static) [StringView; 3]`.
- **What stays rejected:**
  - **Changing the table:** assigning an element (N0211) and borrowing it mutably (N0248), as for every static.
  - **Arrays of arrays of views** (N0337), as for constants.
  - **`static mut` tables of views** (N0337, unchanged). See §3.

## 3. Not taken: `static mut` tables

- **Release 0.11.0 does not check the regions of values written to a `static mut`.** Writing a view of a local
  `String` into `static mut N: StringView` compiles, and reading `N` afterwards reads freed memory.
- **This ADR does not extend `static mut` to arrays of views,** which would widen that hole. Only the immutable
  form is added, and it cannot be written at all.
- **The hole itself is a separate soundness bug.** It needs its own decision: either require the written value to
  have the `static` region (N0243), or forbid views in `static mut` altogether.

## 4. Evidence (macOS arm64)

- **New corpus tests:**
  - `run-pass/static-view-tables` covers slicing, `.length`, iteration, a `borrow(static)` result, a repeat, and a
    constant view as an element. Its expected output comes from a Python model of the same tables. It passes
    compiled and with `--interp`.
  - `check-fail/static-view-tables` covers `static mut`, nesting, a non-`@constant` initialiser, writing and a
    mutable borrow.
  - Both tests fail on 0.11.0, with N0337 on every immutable table: the run-pass test's tables, and the
    check-fail test's `LEVELS`, which should be accepted.
- **The full corpus, the fixed point and the contract suite** are recorded in ROADMAP §10.

## 5. Consequences

- ClayWind's demo tables can be `static`. Its `let x = TABLE;` lines then go, once a release carries this.
- A `const` table still cannot be sliced where it is named. A table that is passed as a slice should be declared
  `static`.
