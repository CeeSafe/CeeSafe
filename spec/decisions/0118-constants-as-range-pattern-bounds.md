# ADR 0118 — A constant's name as a range pattern's bound

- Status: **Accepted** by the owner, 2026-11-13 ("Fix those new gaps too"), implemented on branch `fix/gaps2-c`;
  not yet released. Raised while writing the language's book: a range whose limits the program already names had
  to repeat them as literals.
- Scope: type-system.md §15.2 (`PrimPattern`, "Ranges"); diagnostics.md (N0358's row; N0108's wording); the parser
  (`rangePattern`, `nameBound`) and the checker (`rangeBound` in `src/check/patterns.cee`); the corpus and the
  guide. No change to the AST or HIR (a range pattern holds two expressions and, once checked, two values, as
  before), the usefulness check, the NIR, the C backend or the interpreter.

## 1. Context

A range pattern's bounds were integer or character literals only (ADR 0085 §2.4). A bare name is already a pattern
when it names a constant (ADR 0085 §2.6): `LIMIT => …` matches the constant's value. The two did not meet:

```ceesafe
const LOW: u8 = 10;
const HIGH: u8 = 20;

fn inBand(n: u8) -> bool => match (n) {
    LOW..=HIGH => true,      // N0100: expected '=>', found '..='
    _ => false,
};
```

The program had to write `10..=20` beside the constants, or a guard (`v if (v >= LOW && v <= HIGH)`), which does
not count toward exhaustiveness (§15.2).

## 2. Decision

```text
PrimPattern = … | Bound '..=' Bound | Bound '..' Bound
Bound       = Literal | Identifier
```

A range pattern's bound is an integer or character literal, or the bare name of a constant. Either bound may be
either: `LOW..=HIGH`, `0..LOW`, `HIGH..=255`.

- **The name is a constant in scope** (a `const` item of the file, the package, or the library, found as the bare
  name of a constant pattern is, §15.2; a private constant of another file is N0296, ADR 0109). Its value is its
  bound, evaluated when the program is compiled, as every constant is (§10). Anything else of that name, or
  nothing, is N0358: a local, a parameter, a const parameter (whose value differs for each instance, as for a
  constant pattern), a function. A value that is not constant is tested in a guard.
- **The constant's type is the scrutinee's (or payload's) type**, exactly: N0205 otherwise. A literal bound is
  typed by the scrutinee (§4); a constant has its declared type, and no conversion is applied, as none is for a
  constant pattern.
- **Everything else is the range's existing rule**, applied to the values: an empty range is N0357, the range
  counts toward exhaustiveness and reachability with its values (so `0..LOW` and `LOW..=255` cover a `u8`), and it
  may stand wherever a range may: in a `match`, after `is`, in a `let … else` (ADR 0117), under `name @` and in an
  or-pattern.
- Only a bare name: `T.NAME..=9` and a constant of a dependency (`pkg.NAME`) are not bounds (N0100), as a
  qualified name in a pattern is a variant. An expression (`LOW + 1`) is not a bound either: name it.

No program that compiled changes: a name before `..=` or `..` in a pattern, and a name after them, were syntax
errors.

## 3. Alternatives

- **Only a better message** ("a range's bounds are literals"). It keeps the duplication the item reports, and
  the language already reads a constant's name as a pattern.
- **Any constant expression as a bound** (`LOW + 1..=HIGH * 2`). The pattern grammar would then hold expressions,
  and `a..b` in a pattern would compete with the range expression's operands. A named constant says the same.
- **Qualified constants** (`Limits.LOW`). A qualified name in a pattern is a variant path; telling the two apart
  needs resolution in the parser or a pattern node that is decided later. Left until it is asked for.

## 4. Implementation

- Parser: `parsePattern`, at a name followed by `..=` or `..`, reads the name as the lower bound (`nameBound`, a
  name expression) and continues in `rangePattern`, which `literalPattern` shares; the upper bound may be a name.
  After `let`, a name followed by `..=` or `..` starts a pattern (`startsLetPattern`, ADR 0117).
- Checker: `rangeBound` checks a literal bound as before; a name is looked up as a constant pattern's name is
  (`constantNamed`), its type compared with the scrutinee's, and its value taken (`namedConstantValue`).
  `checkRangePattern` then has two integer values, as it had.

## 5. Tests

`tests/reference/tests/corpus/run-pass/range-pattern-constants.cee` (constants as either bound and both, a
character constant, negative values, a cover of `u8` without `_`, after `is`, in a `let … else`; native and
`--interp`), `check-fail/range-pattern-constants.cee` (N0357, N0358 for a local, a misspelt name and a const
parameter, N0205, N0256, N0257), `parse-fail/range-pattern-bound.cee` (N0108).
