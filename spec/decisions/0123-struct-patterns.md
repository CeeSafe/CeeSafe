# ADR 0123 — Struct patterns: `P { x, y: 0, .. }`

- Status: **Accepted** by the owner, 2026-11-13 ("Go ahead with the first three"; for a struct with a `drop`
  block, "I go with your suggestions": a destructuring that suppresses the drop), implemented on branch
  `impl/0123`; not yet released. Raised while writing the language's book (chapter 19; gaps C18 and B9 of the
  second round): patterns took apart enums, optionals and slices only, two values could not be matched at once,
  and a consuming method of a struct with a `drop` block could not hand out its fields.
- Scope: grammar.md §2 (`LocalDecl`, notes); type-system.md §15.2 (the pattern, usefulness), §15.4 and §15.5
  (irrefutable patterns, `let` without `else`), §14.2 and §18.4 (moves, a struct taken apart);
  diagnostics.md (N0229, N0230, N0231, N0250, N0252, N0256, N0404: wording and new causes; no new code);
  formatting.md §4 (the pattern is a list in braces); the parser, the AST and HIR (one pattern each), the checker
  and its usefulness algorithm, the NIR lowering (`Unpack` of a struct), the interpreter and the C backend (that one
  statement), the formatter; the corpus, the package tests, the guide. No change to the borrow, move and drop passes.

## 1. Context

A pattern named a variant, a literal, a range, a constant, a string or a slice (ADR 0085). A struct was read field
by field, so a test on two of its fields was an `if` chain, outside the exhaustiveness check; and two values could
not be matched together, since CeeSafe has no tuples and is not getting them. With struct patterns, two values are
matched through a small struct:

```ceesafe
struct Move { from: Square; to: Square; }

match (Move { from: a, to: b }) {
    Move { from: Square.Empty, .. } => …,
    Move { from: Square.Piece(p), to: Square.Empty } => …,
    Move { from: Square.Piece(p), to: Square.Piece(q) } => …,
}
```

Separately, a field cannot be moved out of a value whose type has a `drop` block (N0252): the block needs the
whole value. So `fn finish(this) -> Parts` of a builder with a `drop` block could not be written without `Option`
fields or a flag.

## 2. Decision

### 2.1 The pattern

```text
PrimPattern  = … | QualifiedName '{' (FieldPattern ',')* (FieldPattern | '..' ','?)? '}'
FieldPattern = Identifier ':' Pattern | 'var'? Identifier
```

`P { x, var y, z: 0, .. }` matches a value of the struct type `P`, or a reference to one, when every field
pattern matches its field. `P` is the struct's name, as a struct literal writes it without type arguments (they are
the value's): `Point`, `Pair`, `geometry.Point`, `Self`. A field is named at most once (N0231), must exist
(N0230), and must be one the code could read as `v.f`: a `private` field from another file is N0296, one that is
not `public` from another package N0312 (ADR 0109). A field written alone, `x`, is `x: x`: a binding of that name,
or, by ADR 0085 §2.6, the constant of that name if one is in scope; `var x` is `x: var x`.

**Every field is named, or the pattern ends with `..`** (N0229 otherwise, naming the fields left out). Reasons: a
variant pattern with parentheses lists every payload (N0209) and a slice pattern says `..` where it skips; a
struct literal names every field; and §2.6 needs "every field" to be something the reader sees. `P { .. }`
matches any value of `P`.

There is no ambiguity. In a pattern, a name followed by `{` had no meaning: an arm's block follows `=>`, and the
conditions that take `is` are parenthesised (`if (p is P { x: 0, .. }) { … }`), as a struct literal in a
condition already relies on. After `let`, a name followed by `{` starts a pattern.

### 2.2 Types

The value's type must be a struct declared in CeeSafe (not a C struct or union, N0205), and the name must name
it (N0205). Field types are the instance's: on a `Pair<i32>`, `Pair { first, second }` binds two `i32`. A field
of a reference type takes a binding or `_`; another pattern there is N0205 ("bind it and match on it separately"),
as for a payload, except a string or slice pattern on a view. `match` and `is` accept a struct scrutinee.

### 2.3 Exhaustiveness and usefulness

A struct type has one constructor, whose arguments are its fields in declared order (a field the pattern does
not name is `_`). The algorithm of §15.2 is unchanged: specialising a column by that constructor replaces it with
one column per field. So

```ceesafe
match (p) {
    Point { x: 0, y } => …,
    Point { x, y: 0 } => …,
}   // N0256: this match does not cover Point { x: -2147483648, y: -2147483648 }
```

and an arm that earlier arms cover is N0257. A witness prints the fields it constrains, and `..` for the others.

### 2.4 Irrefutable patterns, and `let P { … } = e;`

A pattern is **irrefutable** when it is `_`, a binding, `name @ P` with `P` irrefutable, or a struct pattern
whose field patterns are all irrefutable. (A variant pattern never is, even of an enum with one variant: the rule
is structural, so that it does not change when a variant is added.)

- After `is` and in `let … else`, an irrefutable pattern is warning N0404, as `_` and a binding already were: the
  test is always true. For a struct pattern the message says to write the `let` below.
- **`let P { … } = e;`**, a `let` whose pattern is a struct pattern, may stand without `else`. The pattern must be
  irrefutable (N0256 otherwise, saying to add `else` or to test with `is`). It is `let … else` whose block is never
  reached: the same scope for the names, the same moves and temporaries (§15.5). Only a struct pattern may: `let
  Some(v) = x;` stays N0100, with its message. It was allowed because it is the form the statement wants (an
  `else` that cannot run would be noise, and N0404) and costs one branch in the parser and one in the checker.

### 2.5 Moves and borrows

As for a variant's payloads (§15.2, §18.4), field by field:

- Through a reference (`borrow P`, `borrow mut P`), a binding refers to its field, with the reference's
  mutability; `var` is N0246. Nothing moves.
- On a value, a binding of a Copy field copies it. A binding of a field that is not Copy **moves the field out**:
  - after `is` and in `let`, out of the scrutinee where it is, on a match only: the value is then partially
    moved, by the rules of §14.4 and ADR 0114 (its other fields stay readable; the value as a whole is N0250);
  - in a `match`, the scrutinee is consumed when the match starts, as today (§18.4); the fields an arm does not
    bind are destroyed when the match ends.
  A nested variant pattern that binds a non-Copy payload unpacks its field (§18.4): the field moves to a hidden
  local of the scope first, so the rest of the struct stays in place.
- `name @ P { … }` moves the value into `name`, so `P`'s bindings must copy (N0250), as before.

### 2.6 A struct with a `drop` block is taken apart, in its own file

A struct pattern **takes apart** a value of a struct type with a `drop` block when all of these hold:

1. the scrutinee is a value, not a reference;
2. the pattern names every field: it has no `..`;
3. the pattern is in the file that declares the struct.

Where such a pattern matches, the value is consumed, whatever its fields' types: each field moves out, into its
binding or, for `_` or a pattern that is not a binding, into a hidden local of the scope (destroyed at its end,
after being matched in turn); and **the `drop` block does not run**, since no value is left for it. Where the
pattern does not match, nothing moves, and the value is dropped as usual, whole. So:

```ceesafe
struct Builder {
    name: String;
    parts: Vector<Part>;
    drop { log("abandoned"); }

    fn finish(this) -> Vector<Part> {
        let Builder { name: _, parts } = this;      // `this` is taken apart: no "abandoned"
        return parts;
    }
}
```

Any other struct pattern on a value of such a type (with `..`, or in another file) only reads: it may test fields
and bind Copy ones, and the value is dropped later, its block running. If it would move a field out, it is N0252,
whose message states this rule.

The rule holds for a nested pattern as for a whole one. In `Outer { part: Part { id: 8 }, .. }`, with `Part` a
struct with a `drop` block declared in this file, the inner pattern names every field of `Part`, so a match takes
`part` apart, moving it out of the `Outer`: fine when `Outer` has no `drop` block (a partial move) or is itself
taken apart, N0252 otherwise. To test the field and leave it, write `Part { id: 8, .. }`.

Why this is sound: every field is accounted for by the pattern (moved to a binding, or to a hidden local that is
destroyed), so nothing leaks and nothing is destroyed twice; the value's own destruction is off on exactly the
paths where the pattern matched, which the move checker already tracks for a consumed value (the lowering is
§18.4's `Unpack`, of a struct). Why it is explicit: the pattern lists every field, so adding a field to the struct
makes each such pattern an error (N0229) until it says what happens to the new field. Why the file: skipping a
destructor is the type author's decision. A `drop` block may release what a Copy field names (a file descriptor,
a lock's index); outside the file, code could otherwise leak that or, worse for an invariant, keep using it. The
file is the unit `private` already uses (ADR 0109), and needs no new keyword; a type that wants others to take it
apart offers a consuming method.

`name @ P { … }` with a `P` that takes the value apart is N0250: the value moves once.

## 3. Alternatives

- **Fields may simply be left out** (`P { x }` matching any `y`). Shorter, and what a reader of struct literals
  least expects; it makes §2.6's "every field" invisible, and a new field silently ignored.
- **Positional patterns** `P(x, y)`. Structs have no positional form anywhere else.
- **Tuples**, for matching two values. Declined by the owner; a small struct names its parts.
- **Usefulness-based irrefutability** (an enum with one variant counts). Then adding a variant elsewhere turns a
  `let` into an error far away; the structural rule is local. It can be widened later without breaking programs.
- **`let` without `else` for every irrefutable pattern.** Only struct patterns need it: a binding is a `let`
  already, and `let _ = e;` exists.
- **Partial moves in `match` too** (the scrutinee left partly in place). `match` consumes its scrutinee today;
  keeping that makes one rule for all arms, and `is`/`let` give the finer one.
- For the `drop` struct: **keep N0252** (builders then need `Option` fields); **a `mut this` receiver** (does not
  stop the block from running); **an attribute or a `forget` function** (new surface, and `forget` leaks fields
  unless it is this pattern again); **allow it in any file** (lets any code skip a destructor); **only when a
  non-Copy field is bound** (then `let Fd { fd } = this;` would copy `fd` and still close it).

## 4. Implementation

- Parser: `PatternNode.Struct(name, fields, hasRest)`; a field is an `Ast.bindings` entry (name, `var`, pattern or
  none). `finishLetElse` takes a struct pattern without `else` (an empty block of no width).
- Checker: `checkStructPattern` (patterns.cee) resolves the fields with `resolveField` (so N0230, N0296, N0312
  and configured fields behave as in `v.f`), builds `HirPattern.Struct(type, a pattern per field, takesApart)`;
  `isIrrefutable`, `takesApart`; `bindsNonCopy` (the "consumes" test) counts a pattern that takes apart.
  usefulness.cee: a `Constructor` for a struct, in `constructorOf`, `allConstructors`, `pushArguments`,
  `pushArgumentTypes`, `renderConstructor`.
- NIR: `testPattern`, `bindPattern`, `guardBindPattern`, `expandAlternatives` and `patternBinds` recurse into the
  fields. `movePattern` is what a consuming test now calls: it unpacks a variant (`unpackPattern`), moves a
  struct's bound fields out of their places, or takes a struct apart with `Unpack(place, none, targets)`, the
  statement §18.4 uses, here with a target for each field. What is unpacked is always a whole local: a field or
  payload moves into a hidden local first.
- `Unpack` without a variant: the interpreter and the C backend read the struct's fields; the move and drop
  passes already treat an `Unpack`'s source as moved, so the `drop` block's call is never emitted on that path.
- Formatter: the pattern's braces are a list, as a struct literal's; `{ .. }` is spaced.

## 5. Compatibility

No program that compiled changes meaning: a name followed by `{` in a pattern was a syntax error, and a `match`
or `is` on a struct was N0207. One existing behaviour changes, a fix: `if (s.o is Some(v))` on a field of a local,
with a payload that is not Copy, leaked `s`'s other fields (the unpack marked all of `s` moved); the field now
moves to a hidden local first, and the rest of `s` is destroyed as usual.

## 6. Tests

`parse-pass/struct-patterns`, `parse-fail/struct-patterns`; `check-pass/struct-patterns` (HIR);
`check-fail/struct-patterns` (unknown field, a field twice, a field left out, another type, not a struct, a
refutable `let`, N0256 and N0257 by usefulness, N0252, N0250 for `@`, `var` through a reference),
`check-fail/struct-pattern-moves` (use after a partial move, after a consuming match, after a value is taken
apart, a field of a `drop` struct, in a loop); `run-pass/struct-patterns` (Copy and non-Copy fields, nested both
ways, generic, regions, `Self`, constants, ranges, or-patterns, slices, `match`/`is`/`let … else`/`let`, loops),
`run-pass/struct-pattern-drop` (a builder that finishes into its parts, in `let`, `match` with a guard, `is`,
under `Some`, nested `drop` structs; what runs and what does not), `run-pass/is-field-payload` (the fix of §5):
natively, with `--interp` and with `--check-leaks`; the formatter's snapshot; `tests/packages/private-items` (a
private field and a private struct in a pattern: N0296; a `drop` struct of another file: N0252).
