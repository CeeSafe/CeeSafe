# ADR 0228 — Diagnostics that say what to write: a box's contents, a result that holds a slice, float patterns

- Status: **Proposed** 2026-11-24, under the owner's instruction to fix the type-checker findings of the SQLite port
  (`CeeSafeApps/sqlite/CEESAFE_FINDINGS.md` findings 8, 31 and 7). Implemented 2026-11-24 on `fix/findings-types`.
- Scope: type-system.md §15.2 (patterns), §18.3 (moving out of a box), §25.6 (elision and N0245); diagnostics.md
  N0108, N0245; grammar: a float literal is parsed where a pattern is expected, then refused by the checker.

## 1. Context

- **Finding 8.** `*boxed` on a `Box<Expr>` is N0205 ("'*' reads through a reference"); `let inner: Expr = boxed;`
  (§18.3) is the way, and nothing said so.
- **Finding 31.** For `fn span(borrow mut this, bytes: Slice<u8>, n: usize) -> Result<Slice<u8>, E>` returning a slice
  of `bytes`, N0245 told a static function to write `borrow(a, b) T`; written on this result, the source list makes
  the whole `Result` a reference (N0205). A region parameter is what works, and no message named it.
- **Finding 7.** `value is SqliteValue.Real(1000.0)` was a parse error ("expected identifier, found float literal").

## 2. Decision

### 2.1 `*b` of a box

N0205's message for `*` on a `Box<T>` adds that a box is read through without `*` (`b.f`, `b.m()`), and that its
contents move out where `T` is expected: `let inner: T = b;` (§18.3). No `Box.unwrap` method is added: a second
spelling of the same move would be one more thing to learn, and the message now teaches the one there is.

### 2.2 N0245 for a result that holds a reference

Where the result's type is not itself a reference but holds one (`Result<Slice<u8>, E>`, `StringView?` in a struct,
any scoped non-reference type), N0245 names a region parameter, not a source list:
`fn f<region r>(a: Slice<r, u8>, b: Slice<u8>) -> Result<Slice<r, u8>, E>`, and says that `borrow(a) T` would make the
whole result a reference. Both N0245s are covered: the checker's, for a function with several scoped parameters and
no receiver to default to, and the borrow checker's, for a method whose result is derived from a parameter other than
the receiver (it names the parameter).

### 2.3 Float literals are not patterns, and say why

A float literal (or a negated one) where a pattern is expected is parsed, so that the checker can report it: N0108,
"a float literal is not a pattern: floats are compared with '==' (exact, 'NaN' equals nothing, '-0.0 == 0.0'), so bind
the value and compare it", with `V.Real(r) if r == 1.5` and `v is V.Real(r) && r == 1.5` as the forms to write. A
`match` on a float scrutinee stays N0207, whose message now says to write an `if` chain.

Float patterns themselves were considered and not added. A pattern reads as identity, a float `==` is not one: `NaN`
matches nothing (not even a `NaN` pattern), `-0.0` and `0.0` would match each other's arms, and a decimal literal is
rarely the exact value a computation produces, so an arm such as `0.3 =>` would be silently dead for `0.1 + 0.2`.
Exhaustiveness has nothing to say about floats either (every float `match` needs `_`). An explicit comparison makes
each of these visible where it is written; the guard form costs one name.

## 3. Options not taken

- **`Box.unwrap(b)`** (§2.1).
- **Float patterns with `==` semantics** (§2.3): possible, and unambiguous if specified as `==`, but each property
  above would be a surprise in a `match`, where none is in an `if`.

## 4. Implementation and tests (2026-11-24)

- `synthDeref` (src/check/references.cee); `resolveReturnSources`'s message (src/check/references.cee) and the
  unlisted-parameter message (src/nir/borrow.cee); `parsePatternLevel` and `startsNestedPattern`
  (src/syntax/parser.cee), the literal case of `checkPattern` and the scrutinee message of `checkMatch`
  (src/check/patterns.cee).
- `check-fail/adr0228-diagnostics.cee`: `*b` of a box (N0205), a static function returning `Result<Slice<u8>, E>`
  with two slice parameters (N0245), `is V.Real(1000.0)` and `V.Real(-0.5) =>` (N0108), a `match` on an `f64`
  (N0207). `check-fail/adr0228-region-hint.cee`: the finding's method (N0245 from the borrow checker) beside the same
  method with a region parameter, which is valid. The messages were read in the compiler's output (ROADMAP §10).
