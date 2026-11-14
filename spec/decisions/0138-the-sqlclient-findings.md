# ADR 0138 — sqlclient's findings: `??` blocks, `==` on optionals, views in `var`, `?`/`!` statements, text and slice helpers, `void` instances

- Status: **Accepted** by the owner, 2026-11-14 ("Fix all CeeSafe findings and cut a new release": the eight entries of
  `~/dev/CeeSafeApps/sqlclient/CEESAFE_FINDINGS.md`, written while porting CosmoSQLClient's SQL Server driver to
  CeeSafe). Implemented on branch `fix/sqlclient-findings` from `main` at `7c20ac51` (release 0.45.1), rebased on
  `8c2d8182` (release 0.46.1; release 0.46.0's ADR 0137 is a window's native handles).
- Scope: type-system.md §2, §13.1, §16.1, §16.5, §17.4, §19.1, §20.3, §22.1, §23.2; grammar.md §2.1; diagnostics.md
  (N0225's wording); lib/core (`AppendText`, `SliceCopying<T: Copy>`), lib/std (`AppendText` for `String`); the
  parser, the checker; Appendix C. No new diagnostic code, and no NIR, C backend or interpreter change.

## 1. Context

The port is about 5,600 lines of library code: TDS packets, a token decoder, TLS through a sans-I/O engine, a pool.
Its findings, by number in that file:

| # | What | Kind |
|---|---|---|
| 1 | No way to append UTF-8 bytes, or one character, to a `String` without a temporary and a second check | library gap |
| 2 | `x ?? { return …; }` is a parse error, so "unwrap or leave" takes a four-line `match` | language gap |
| 3 | A `var` cannot hold a `StringView`/`Slice<T>`, so a view cannot be narrowed step by step | language gap |
| 4 | `Slice<u8, r>` says "'Slice' takes one type argument", not that the region comes first | diagnostic |
| 5 | No overlapping copy inside one buffer (C's `memmove`) | library gap |
| 6 | `r.ok()` on a `Result<void, E>` checks, then the interpreter panics and the C does not compile | **bug** |
| 7 | `x!;` and `f()!!;` are N0225: only `?` and `!` directly on a call were statements | language gap |
| 8 | `o == Some(1)` is N0207: optionals compare only with `null` | language gap |

Finding 6 is a fault of the checker, not of the backends: type-system.md §2 allows `void` only as a return type and a
`Result`'s success type, and §20.3's unit payloads exclude `Some(v)` with `void` by name, but an instance's signature
is not checked again (§20.3: "the instantiation's would only repeat it"), so `ResultMethods<void, E>.ok`, whose
signature is `fn ok(r: Result<void, E>) -> void?`, reached the backends with a `void?`.

## 2. `a ?? { … }`

- **Rule.** The right operand of `??` may be a block. It is checked as a branch of an `if` expression (§15.3, ADR
  0073 §2.3): it ends in its value, written without `;`, of type `T`, or it cannot complete (`return`, `break`,
  `continue`, a *never* call). A block that can complete without a value is N0235, as for a branch.
- **Grammar.** After `??`, a `{` begins the block (the parser's `parseBranch`); nothing else changes: `??` stays the
  lowest, right-associative level, and a block operand is never itself a chain.
- **Lowering.** As before, `{ let t = a; if (t == null) <block> else t! }`: the block is the `if`'s branch.
- **Why a block and not `let … else`.** `??` is the expression CeeSafe already has for the fallback, and a branch block
  is a form the language already has, with its rules and diagnostics. `let Some(p) = … else { … };` would be a second
  way to say the same with a new statement.

## 3. `==` and `!=` on optionals

- **Rule.** `a == b` and `a != b` where both are optionals of one type `T?`, `T?` is Copy, and `T`'s values compare
  with `==` (integers, floats, `bool`, `StringView`, fieldless enums, derived equality, an optional again): equal when
  both are `null`, or both hold equal values. The right operand is checked against the left's type, so `Some(5)`
  needs no annotation. `o == null` keeps its own rule (§22.1).
- **Lowering** (the checker, as `??` is): `{ let l = a; let r = b; if (l == null) r == null else (r != null && l! == r!) }`,
  and for `!=` `if (l == null) r != null else (r == null || l! != r!)`; `&&` and `||` short-circuit, so `r!` is read
  only when `r` holds a value.
- **Not Copy.** An optional of a type that is not Copy (`String?`) is N0207 with a hint: the lowering copies its
  operands, and comparing through borrows is a larger rule than this finding needs.

## 4. Shared views in `var`

- **Rule.** A `var` may hold a `StringView` or a `Slice<T>`. Assigning to it re-binds it to another view; for
  `borrow T`, `borrow mut T` and `SliceMut<T>` it stays N0246, because assigning to those writes through them.
- **Why this is sound.** §19.1 already lets other scoped values (scoped results, region values) be `var`, with the
  local holding the loans of every value it is assigned. A shared view gets the same treatment: the place is the
  local itself, not its referent, and the borrow checker's flows are those of a scoped result. Checked: a view of a
  string that goes out of scope (N0243), returned from its function (N0244), or whose owner changes while it is held
  (N0241), each after a re-binding.
- **Why only shared views.** Through a `SliceMut<T>` an assignment `m = other` could mean writing the elements, as it
  means writing the referent through a `borrow mut T`; keeping that meaning and refusing `var` is the existing rule.

## 5. `?` and `!` as statements

- **Rule.** An expression statement may be `?` or `!` on any operand, not only on a call: `x!;` checks that `x` holds a
  value (or is `Ok`), `x?;` returns when it does not, and `f()!!;` unwraps twice. The operand is consumed as by
  `match`; a value the statement does not keep is destroyed at its end, as a call statement's is.
- NIR already lowered such statements (`lowerExpressionStmt`); only the checker refused them.

## 6. Library: `AppendText` and `SliceCopying`

- **`AppendText`** (lib/core), implemented for `Vector<u8>` in lib/core and for `String` in lib/std:
  - `appendChar(c: u32) -> Result<bool, AllocError>`: the character in UTF-8; `false`, appending nothing, for a value
    that is not a Unicode scalar value (a surrogate, or past 0x10FFFF);
  - `appendUtf8(bytes: Slice<u8>) -> Result<bool, AllocError>`: the bytes when they are UTF-8, checked once, with no
    copy first; `false`, appending nothing, when they are not.

  A `String` stays UTF-8, so a safe append without the check is not offered: what the finding wanted is the single
  check and no temporary, which this gives.
- **`SliceCopying<T: Copy>.copyWithin(s, from, to, count)`**, called `d.copyWithin(from, to, count)` on a `SliceMut<T>`,
  a mutable array or a vector: the `count` elements from `from` are copied to `to`, as `memmove` copies them. Both
  ranges are checked before anything is copied ("slice range out of bounds"). Written in CeeSafe (forward when the
  target is before the source, backward otherwise), so the backends are unchanged.

## 7. Diagnostics

- **`void` instances** (finding 6). A call of a function whose signature has `void` where §2 allows none (a parameter,
  an optional's value, a `Result`'s error) is N0226 at the call. Written signatures never do (they are N0226 where
  written), so only an instance at `void` can: `r.ok()` on a `Result<void, E>`, or a generic `fn wrap<T>(v: T)` given
  a `void` call. The message names the function and, for `ok` and `err`, says to test with `isOk()`. The instance's
  other functions (`isOk`, `err`) are unaffected, and the refused one is never compiled. This makes the checker keep
  what §2 and §20.3 already said; `void?` as a type stays undefined.
- **A slice's region last** (finding 4): `Slice<u8, r>` and `SliceMut<u8, r>`, whose second argument is a region in
  scope, are N0204 "a slice's region comes first: write 'Slice<r, u8>'".

## 8. Tests

Corpus: run-pass `adr0138-coalesce-block`, `adr0138-optional-equality`, `adr0138-unwrap-statements`,
`adr0138-view-variables`, `adr0138-copy-within`, `adr0138-append-text`; check-fail `adr0138-coalesce-block`,
`adr0138-optional-equality`, `adr0138-view-loans`, `adr0138-mutable-views`, `adr0138-void-instances`,
`adr0138-slice-region-order`. The run-pass programs' output was checked in the interpreter and natively. sqlclient
itself (34 tests, 13 of them against SQL Server 2025) is the outside check that the port's workarounds can go.
