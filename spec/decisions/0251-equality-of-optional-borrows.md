# ADR 0251 — `==` on optional borrows

- Status: **Accepted** by the owner, 2026-11-25, with §5's questions answered: owned optionals of a type that is not
  Copy are lent too (§2.5), and `String == StringView` is byte equality (§2.6). Proposed the same day, at the owner's instruction to close the SQLite port's finding 38
  (`CeeSafeApps/sqlite/CEESAFE_FINDINGS.md`, "An optional borrow cannot be compared with `Some(…)`"). Implemented on
  branch `fix/g5-compiler` (§6); not released.
- Scope: type-system.md §22.1 (comparisons of optionals) and §22.2 (`String` and `StringView`); the checker (`src/check/expr.cee`, `src/check/options.cee`);
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

### 2.5 Owned optionals of a type that is not Copy (Question 1: yes)

`a == b` and `a != b` on two owned optionals `T?` whose `T` is not Copy and has `==` (`String?`, a `@derive(Equal)`
struct's) compare as §2.1 says, both lent: a place as `asBorrow()` lends it, a temporary (`a == make()`) held by the
comparison and dropped with it. Neither operand is consumed, so `a` and `b` stay usable. With an owned optional of a
type that is not Copy on the left, the right operand is checked as §2.2's table says (`a == Some(name)` lends `name`).
This replaces ADR 0138 §3's N0207 for such optionals.

### 2.6 `String` and `StringView` (Question 2: yes)

`s == v` and `s != v`, with one operand a `String` and the other a `StringView` (in either order, a string literal
included: `s == "abc"`, `"abc" == s`), compare the string's text with the view byte for byte, as `s.view() == v`
does; a `String` is not consumed. So do an optional's values: `m.get(k) == Some("a")` with `V = String`, and
`names == Some("a")` with `names: String?`; `Some(e)` of a value against a `String` referent may then be a view's.

### 2.7 What is unchanged

- Values of different types are not compared: `i64? == i32?` is N0205, as `i64 == i32` is.
- A `T` without `==` has none in an optional either: `p == q` with `Plain?` is N0207.
- `Some(e) == m.get(k)` with `e` a place of a type that is not Copy: the left `Some(e)` is an owned value (it is
  checked first, without an expected type), so `e` moves into it, and a later use of `e` is N0250. Write the
  optional borrow on the left, where `Some(e)` lends `e`.

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

## 5. Questions for the owner (answered 2026-11-25)

1. Should `==` lend two owned optionals of a type that is not Copy (`String? == String?`) as §2.2 lends one of them?
   Answered yes: §2.5.
2. `String == StringView` was N0207 (`s == "a"`), so `m.get(k) == Some("a")` with `V = String` was refused. Should
   `String` compare with `StringView` (the bytes)? Answered yes, both orders and with literals: §2.6.

## 6. Implementation (2026-11-25)

- `synthBinary` (`src/check/expr.cee`): the left operand of `==` and `!=` is read with `lentOptional` (a `borrow (T?)`
  of a non-Copy `T` is lent); a left optional borrow checks its right operand with `checkBorrowedOptionalOperand`, a
  left `T?` with `checkOptionalOperand` (an optional borrow of `T` on the right is kept as it is); `comparableOptionals`
  admits an optional borrow and an optional of the same referent.
- `synthOptionalEquality` (`src/check/options.cee`): `lentOperand` lends places and holds temporaries; `comparedValue`
  binds a reference payload to a local.
- Corpus: run-pass `adr0251-optional-borrow-equality`; check-fail `adr0251-optional-borrow-equality`,
  `adr0251-moved-into-some`.
- After the owner's answers: `lentOperand` lends every optional that is not Copy, `comparedByLoan` sends an owned one's
  right operand through `checkBorrowedOptionalOperand`, `comparableOptionals` admits `String` and `StringView` values,
  and `derivedEquality` (`src/check/derives.cee`) compares a `String` with a `StringView` through `stringText`. Corpus:
  run-pass `adr0251-owned-and-string-equality`; check-fail `adr0251-optional-borrow-equality` rewritten for §2.7.
