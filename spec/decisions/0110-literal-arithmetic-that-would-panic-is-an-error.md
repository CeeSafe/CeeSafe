# ADR 0110 — Literal arithmetic that would panic is an error

- Status: **Accepted** by the owner, 2026-11-12 ("We fix all gaps 1-6 first"), implemented on branch
  `fix/let-discard-and-constant-overflow`; released in ceesafec 0.42.0. Raised by the book's authors, who read
  `spec/type-system.md` as promising it: `let big: u8 = 200 + 100;` compiled and panicked when run.
- Scope: the checker (`src/check/expr.cee`, a flag in `checker.cee` and `declare.cee`), which calls the constant
  evaluator it already has (`src/check/consts.cee`, unchanged); `spec/type-system.md` §5, §6, §10;
  `spec/diagnostics.md`'s N0217 row; the guide. No change to the library, the runtime or the generated C.

## 1. Context

The operators panic on overflow, on a zero divisor and on a shift amount out of range (§6). A constant's value is
evaluated when the program is compiled, and there such an operation is N0217 (§10). In a function's body nothing
was evaluated, so

```ceesafe
fn main() {
    let big: u8 = 200 + 100;   // each literal fits u8 (N0206 does not apply); the sum panicked at run time
    Console.writeLine(big);
}
```

compiled, and stopped with "integer overflow" every time it ran. The specification said so, but only by putting
the compile-time rule under "Constants"; a reader of §6 and §10 together took it for a rule about constant
arithmetic. The statement can never do anything but panic, the compiler holds the evaluator that proves it, and a
literal that does not fit is already an error (N0206): the gap is between `300` and `200 + 100`.

Measured 2026-11-12 on ceesafec 0.40.0 (`run --interp`): with `let`, `200 + 100`, `200u8 + 100u8`, `200u8 + 100`,
`1 - 2` and `16 * 16` as `u8`, `2147483647 + 1` as `i32`, `0 - 1` as `u32`, `1 / 0`, `1 << 32` as `u32`,
`-(-128)` as `i8`, and the first nested in `(200 + 100) / 2` and `1 + (200 + 100) - 100`, all compiled and
panicked; each of them as a `const`'s or a `static`'s value was N0217.

## 2. Decision

**Literal arithmetic** is an expression made only of

- integer literals, suffixed or not, and character literals (integer literals by §4),
- unary `-` and parentheses,
- the arithmetic operators `+ - * / %`, the shifts `<< >>` and the bitwise operators `& | ^`.

Wherever literal arithmetic stands in a function's body, the outermost such expression is evaluated by the
constant evaluator once its type is known, with the run-time semantics of §6. If an operation in it would panic
(overflow of `+ - *`, division or remainder by zero, `MIN / -1`, `-MIN`, a shift amount negative or not below the
width), that is a compile-time error: **N0217**, with the message the same expression gets as a constant's value
("constant evaluation failed: arithmetic overflow: the result 300 does not fit in 'u8'"), at the operation that
fails.

- **Nothing else changes.** The rule reports; it does not replace the expression. Its type is what §5 gave it and
  the code generated for it is what it was (the checks are removed, or not, by the passes that did so before).
  The wrapping, checked and saturating methods are untouched: `200u8.wrappingAdd(100)` is `44`. An argument is an
  expression like any other, so `x.wrappingAdd(255 + 1)` with `x: u8` is N0217 for its argument.
- **What is written, not what is known.** An expression with a name, a call, a cast (`as`, `truncate`) or a float
  literal in it is not literal arithmetic, and neither is an `if` or `match` expression, though its branches may
  be. Such an expression is checked when it runs, as before, whatever the compiler could work out. Inside one,
  each literal part is under the rule: `x + (200 + 100)` is N0217 with `x: u8`.
- **Named constants do not count as literals.** `K + 100` with `const K: u8 = 200;` compiles and panics when it
  runs. The evaluator could fold a constant's use at no cost, but the rule would then depend on a declaration
  elsewhere, perhaps in another package: changing a constant's value there would turn a program that compiled
  into one that does not, far from the edit. A rule about the text of one expression has no such reach. It also
  keeps a way to write the operation deliberately (a test of the panic; the corpus's
  `run-pass/dead-constant-overflow.cee`, §3). Widening the rule to constants later only rejects more.
- **Dead code and generic bodies.** The error is reported in code that cannot run (`if (false) { … }`, after a
  `return`) and in the body of a generic function, instantiated or not, once: it is a property of the text, like
  a type error, and the checker reports those there too.
- **Where a constant is already evaluated** (a `const`'s value, a `static`'s initialiser) nothing changes: those
  are folded whole by their own rules, which report the same error once.
- **Floats** are not part of the rule: float arithmetic does not panic (§35).

Not chosen: N0206 as the code ("a literal that does not fit"). The failure is an operation's, division by zero
and shifts have no literal to blame, and N0217 with its message already says exactly this for a constant. Not
chosen: a warning. The statement cannot succeed, and CeeSafe has no reason to compile it.

### Implementation

`synth` (`src/check/expr.cee`) checks a unary or binary expression as before; when the syntax is literal
arithmetic and no enclosing expression is (a flag on the checker, also off inside a constant's value and a
static's initialiser), it then calls `fold` on the checked expression and discards the folded value. An
expression whose type is not an integer (it has a type error already) is not folded.

## 3. Compatibility

A program with literal arithmetic that would panic stops compiling. It could not run past that statement, so what
is lost is a program whose such statement was never reached. Measured 2026-11-12, before the rule was turned on
for the corpus, by checking every `.cee` file with the compiler before and after and counting the new N0217:

| Tree | `.cee` files | New errors |
|---|---:|---:|
| `src/` (also built whole by the new compiler) | 143 | 0 |
| `lib/` | 34 | 0 |
| `tests/reference/` | 860 | 4, in 2 files |
| `tests/support/` | 10 | 0 |
| `tests/` elsewhere (`check`, `types`, `packages`, `p7`, `embedding`, `cbind`, `formatting`, `syntax`) | 185 | 0 |
| `benchmarks/` | 15 | 0 |
| `probes/`, `integrations/`, `editors/` | 56 | 0 |
| `guide/` (its 157 examples, by `tools/guide_check.py`) | | 0 |

The two files, each a reviewed edit of an expectation and not a regeneration:

- `tests/reference/tests/corpus/run-pass/dead-constant-overflow.cee` (3 sites: `1i32 + 2147483647i32`,
  `-2147483648i32 - 1i32`, `65536i32 * 65536i32` under a condition that is never true). The test is about the C:
  an overflowing operation on two constants must keep its check and not become a C constant expression. Its
  operands are now three named constants, whose uses are their values: the generated C for the three statements
  is byte for byte what the literals gave, so the test tests what it did. Its expected output is unchanged.
- `tests/reference/tests/corpus/run-fail/literal-sum-overflow-u8.cee`, added on this branch one commit earlier to
  pin the old behaviour: removed. Its case is the first line of `check-fail/literal-arithmetic.cee`.

No other `run-fail` test panics through literal arithmetic: they already compute with a local or a parameter.
The book (`book/`) was not measured here.

## 4. Tests

- `tests/corpus/check-fail/literal-arithmetic.cee`: `u8`, `i32`, `u64`; typed by an annotation, by a suffix on
  one or both literals, and by default; subtraction below zero and below `i32`'s minimum, multiplication,
  division and remainder by zero, `MIN / -1`, both shifts, `-(-128)`, a character literal; nested in a larger
  literal expression and beside a name, under a comparison, in an optional's initialiser, a call's argument, an
  array literal, an assignment and a compound assignment; in a branch that never runs, in a function's
  expression body and in a generic function. Each is reported once.
- `tests/corpus/run-pass/literal-arithmetic.cee`: literal arithmetic that reaches each type's limits without
  passing them, nested, with shifts and bitwise operators, suffixed, negated, with character literals, as an
  argument and an optional's value; float division by zero; the wrapping, checked and saturating methods on
  literals that would overflow as operators.
- `tests/corpus/run-fail/named-operand-overflow-u8.cee`: a parameter's sum that fits, then a named constant's
  that does not, which panics at run time ("integer overflow").
- `tests/corpus/check-fail/constants.cee` (unchanged) holds the constant's side.
