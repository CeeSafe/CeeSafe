# ADR 0251 — `==` on optional borrows

- Status: **Proposed**, 2026-11-25, at the owner's instruction to close the SQLite port's finding 38
  (`CeeSafeApps/sqlite/CEESAFE_FINDINGS.md`, "An optional borrow cannot be compared with `Some(…)`"). Implemented on
  branch `fix/g5-compiler` (§6); not released.
- Scope: type-system.md §22.1 (comparisons of optionals); the checker (`src/check/expr.cee`, `src/check/options.cee`);
  the corpus.

## 1. Context

ADR 0138 §3 gave `==` and `!=` to two optionals of one Copy type `T?`, so `o == Some(3)` compiles, and left out
"comparing through borrows", as "a larger rule than this finding needs". The SQLite port met that rule at once: a map's
`get` gives an optional borrow, `fn get(borrow this, key: borrow K) -> borrow(this) V?`, and with ceesafec 0.95.10

```ceesafe
m.get(k) == Some(10)        // N0205: expected 'borrow i64', found 'i64'
m.get(k) == Some(x)         // N0205, with "write 'borrow' before the value to lend it"
m.get(a) == m.get(b)        // N0207: operator '==' cannot be applied to type 'borrow i64'
s.get(k) == Some(name)      // N0205 again, and `borrow name` would be the only way to write it
```

The port's tests match instead (`match (m.get(k)) { Some(v) => v == 10, null => false }`). Yet `==` already reads
through any reference (type-system.md §13.3: "`==` compares the referents (via auto-deref)"), so `r == 10` with
`r: borrow i64` compiles, and so does `rs == rs` with `rs: borrow String`. The optional borrow is the one place where
that rule stops.

## 2. Decision

### 2.1 The rule

An **optional borrow**, `(borrow T)?` or `(borrow mut T)?` (written `borrow T?`, ADR 0074 §2.5), is compared through
its references: its values are its referents. `a == b`, where at least one operand is an optional borrow of `T` and
the other is an optional of `T` in any of the forms of §2.2, is

- `true` when both are `null`;
- `false` when exactly one is `null`;
- otherwise `T`'s own `==` on the two referents (the rules of §22.1 for `T`'s values: integers, floats, `bool`,
  `StringView`, fieldless enums, derived equality, strings, an optional again).

`a != b` is its negation. `T` need not be Copy: nothing is copied out of a referent, as `rs == rs` copies nothing. `T`
must have `==` (N0207 otherwise, as for `T` itself). Neither operand is consumed.

### 2.2 The other operand

With an optional borrow of `T` on either side, the other operand may be:

| Other operand | How it is compared |
|---|---|
| `null` | unchanged (§22.1): a test for absence |
| `Some(p)`, `p` a place of type `T` | `p` is lent as an argument to a `borrow T` parameter is (§13.2): `Some(name)` leaves `name` usable |
| `Some(e)`, `e` a value of type `T` | an optional `T?` of the comparison's own, which ends with it: `Some(10)`, `Some(String.from("b")!)` |
| another optional borrow of `T`, shared or mutable | through both references: `m.get(a) == m.get(b)` |
| a `T?`, `T` Copy | copied, as ADR 0138 §3 copies it: `m.get(k) == o`, `o == m.get(k)` |
| a place of a `T?`, `T` not Copy | lent as `asBorrow()` lends it (ADR 0074 §2.5): `s.get(k) == names` |
| a `borrow (T?)` | lent as `asBorrow()` lends it, as an argument is (ADR 0195 §2.4.1) |

The right operand of `o == Some(5)` is still checked against the left's type, so a literal needs no annotation:
`m.get(k) == Some(10)` with `V = i64` makes `10` an `i64`.

### 2.3 A reference to an optional

`r == Some(x)` with `r: borrow (T?)` already compares when `T` is Copy (the referent is read through `r`, a `T?`). When
`T` is not Copy, the referent is now lent as `r.asBorrow()` lends it and §2.1 applies; ADR 0138 §3 made it N0207.

### 2.4 Mutable optional borrows

A `(borrow mut T)?` is compared through a shared borrow: a place holding one (`let g = m.getMut(k);`) is lent, not
moved, so `g` is usable after `g == Some(p)`; a temporary one (`m.getMut(k) == Some(p)`) is held by the comparison,
which ends with it. Comparing reads the referent only, so a shared view of the mutable reference is all it needs.

### 2.5 What is unchanged

- Two owned optionals of a type that is not Copy, `String? == String?`, stay N0207 (ADR 0138 §3). The message now
  names the fix this ADR gives: "compare their borrows, 'a.asBorrow() == Some(v)' or 'a.asBorrow() == b.asBorrow()'".
- The referents must have one type: `m.get(k) == Some("a")` with `V = String` is N0205 ("expected 'String', found
  'StringView'"), as `s == "a"` with `s: String` is N0207 (Question 2).
- An owned temporary that is not Copy, compared with an owned optional, is not lent: `make() == Some(s)` with
  `String?` stays N0207.
- `Some(e) == m.get(k)` with `e` a place of a type that is not Copy: the left `Some(e)` is an owned value (it is
  checked first, without an expected type), so `e` would move into it; this is N0207 with the hint. Write the
  optional borrow on the left.

## 3. Lowering

As ADR 0138 §3, in the checker: `{ let l = a; let r = b; if (l == null) r == null else (r != null && l! == r!) }`,
and for `!=` `if (l == null) r != null else (r == null || l! != r!)`, where

- an operand that is lent (§2.2, §2.4) is `borrow p` passed through `asBorrow()`'s lowering (`optionalBorrow`), and a
  temporary that is lent is first held in a local of the block (`held`), declared where the operand stands, so the
  evaluation stays left to right;
- an optional borrow's value `l!` is bound to a local of its own (`referent`) inside the branch that reads it, and
  the referent is compared as the place `*referent`, as `rs == rs` compares two places through references.

NIR, the C backend and the interpreter see only `if`, `match`, locals and `==` on the referents: nothing new.

## 4. Options not taken

- **`Some(x)` as `Some(borrow x)` only when written so.** It would keep §2.2's table shorter, but `borrow` is written
  in CeeSafe only where a borrow is stored, and an argument is lent without it; the comparison is an argument-like use.
- **Copying the referents when `T` is Copy.** It is what `*r` does, but it would make `T`'s Copy-ness decide which
  operands compile, which §13.3's rule for references does not.
- **`==` on all non-Copy optionals.** With the lending of §2.2 in place it is a small step (lend both), but it changes
  ADR 0138 §3's decision, which the owner took; Question 1.

## 5. Questions for the owner

1. Should `==` lend two owned optionals of a type that is not Copy (`String? == String?`) as §2.2 lends one of them?
   Recommended: yes, in a later ADR; nothing in this one depends on it.
2. `String == StringView` is N0207 (`s == "a"`), so `m.get(k) == Some("a")` with `V = String` is refused. Should
   `String` compare with `StringView` (the bytes)? Recommended: yes, separately; it is the next thing the port would
   write.

## 6. Implementation (2026-11-25)

- `synthBinary` (`src/check/expr.cee`): the left operand of `==` and `!=` is read with `lentOptional` (a `borrow (T?)`
  of a non-Copy `T` is lent); a left optional borrow checks its right operand with `checkBorrowedOptionalOperand`, a
  left `T?` with `checkOptionalOperand` (an optional borrow of `T` on the right is kept as it is); `comparableOptionals`
  admits an optional borrow and an optional of the same referent.
- `synthOptionalEquality` (`src/check/options.cee`): `lentOperand` lends places and holds temporaries; `comparedValue`
  binds a reference payload to a local.
- Corpus: run-pass `adr0251-optional-borrow-equality`; check-fail `adr0251-optional-borrow-equality`.
