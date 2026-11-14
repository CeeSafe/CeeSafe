# ADR 0139 — `??` on a borrowed optional, and no second error after a bad left operand

- Status: **Accepted** by the owner, 2026-11-15 ("fix findings 9 and 10 and cut a new release": entries 9 and 10 of
  `~/dev/CeeSafeApps/sqlclient/CEESAFE_FINDINGS.md`, found while moving that package to 0.47.0's ADR 0138). Implemented
  on branch `fix/sqlclient-9-10` from `main` at `34172648` (release 0.47.0).
- Scope: type-system.md §22.1; the checker's `synthCoalesce`. No new diagnostic code, and no NIR, C backend, interpreter
  or library change.

## 1. Context

- **Finding 9.** `config.tls() ?? { return Err(e); }`, with `fn tls(borrow this) -> borrow (TlsConfig?)`, was N0262
  ("'??' needs an optional value on its left, found 'borrow (TlsConfig?)'"), while `match (config.tls())` binds the
  payload as a `borrow TlsConfig` (ADR 0074 §2.5). A getter that lends a field of type `T?` is common, so ADR 0138's
  `?? { … }` could not replace the `match` there.
- **Finding 10.** When the left of `x ?? { … }` was no optional, the fallback was checked as a plain expression, and a
  block there is N0235 ("a block is an expression only as a branch …"): a second error for the same mistake.

## 2. A borrowed optional on the left

`a ?? b` with `a: borrow (T?)` or `a: borrow mut (T?)` lends the optional first, as `a.asBorrow()` (`asBorrowMut()`) does,
then is the `??` of that `borrow T?` (`(borrow mut T)?`): the value is a `borrow T` (`borrow mut T`), and `b` is checked
against that type. The lowering is `optionalBorrow`'s match, then `??`'s own; nothing moves out of the referent, and
the loans are those of the lending. A value of `T?` itself keeps its rule (it is consumed, as by `match`).

## 3. The fallback after an error on the left

When the left side is an error, or no optional (N0262), the fallback is still checked for its own errors, a block as
the branch it is (type `T` unknown), so the only new diagnostic is the left side's.

## 4. Tests

Corpus: run-pass `adr0139-coalesce-borrowed` (a getter's `borrow (String?)`, and a `borrow mut (u32?)` written through);
check-fail `adr0139-coalesce-left-error` (N0262 alone, and an error inside the block still reported).
