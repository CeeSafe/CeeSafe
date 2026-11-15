# ADR 0143 — An array length may be a constant expression

- Status: **Accepted** by the owner, 2026-11-15: asked about ClayWind's findings, the answer was "0.49.1 now, K1/K2
  too". Reported by ClayWind (`~/dev/CeeSafeApps/Clay/CEESAFE-FINDINGS.md` K1). Implemented on branch
  `adr/0143-0144-k1-k2`.
- Scope: the checker (`src/check/const_generics.cee`, `arrayLengthValue` and its helpers; `src/check/arrays.cee`,
  N0259's message); type-system.md §10, §16.1; diagnostics.md N0217, N0259, N0353; the guide and the book where array
  types are described; the corpus. No new diagnostic, no change to the C backend, the interpreter or the runtime. It
  amends ADR 0083 §7's "any other expression that names no const parameter stays N0259".

## 1. Context

- **What is accepted today** (type-system.md §16.1, ADR 0083 §2.7): the `N` of `[T; N]` and of a repeat `[v; N]` is
  an integer literal, a `usize` constant (`LENGTH`, `Oid.LENGTH`) or a `usize` const parameter. Anything else is
  N0259, "an array length must be an integer literal, a 'usize' constant or a const parameter".
- **What asked.** ClayWind's text shaping sizes a buffer as "8 `u32`s for each of 1,024 characters". `[0u32; 256 * 8]`
  and `[u8; CAPACITY * 4]` are N0259, so the program writes `[0u32; 8192]` (or a second constant whose value is
  computed by hand), and the two numbers can drift apart.
- **What the checker already has.** A constant's value (§10) is checked against its type and folded by the constant
  evaluator (`src/check/consts.cee`, `fold`): literals, other constants, unary and binary operators, `as` and
  `truncate`, exactly, with overflow, division by zero and a bad shift reported as N0217 ("constant evaluation
  failed: …"). A const argument in a type position already takes a parenthesised constant expression
  (`Grouped<(StmtTag.COUNT + 1)>`, ADR 0083 §2.4). So `const TOTAL: usize = 256 * 8;` then `[0u32; TOTAL]` is
  accepted, and only the expression written in place is not.

## 2. Decision

An array length, in an array type `[T; N]` and in a repeat `[v; N]`, wherever a literal or a constant is accepted
now, may be a **constant expression of type `usize`**: §10's constant expressions, checked against `usize` and
evaluated when the program is compiled.

```ceesafe
const CHARS: usize = 1024;

struct Shaping {
    glyphs: [u32; CHARS * 8];
    levels: [u8; (CHARS + 1) / 2];
}

let table = [0u32; 256 * 8];
let flags: [u8; 1 << 4] = [0u8; 16];
let words = [0u64; SMALL as usize * 2]; // `const SMALL: u32 = 4;`
```

- **The expression** is an operator applied to operands (a unary or binary operator or `as`, also in parentheses)
  whose leaves are integer literals and constants (top-level, associated, or of another package). It is checked as a
  constant's value is (§10): against `usize`, so an unsuffixed literal is a `usize`, and then folded by the constant
  evaluator. The forms accepted before (a literal, a constant's name, a bare const parameter) are unchanged.
- **The value** is the length: the type `[u8; 256 * 8]` is `[u8; 2048]`, the same type as one written with the
  literal. It must be from 1 to 2^48 − 1, as before (N0259 otherwise: `[u8; 4 - 4]`, `[u8; 1 << 50]`).
- **Diagnostics.**
  - An operation that would panic at run time is N0217, as in a constant: `[u8; BIG + 1]` is "constant evaluation
    failed: arithmetic overflow: the result 18446744073709551616 does not fit in 'usize'"; `2 - 3` is the same with
    `-1`; `4 / 0` is "division by zero"; a shift past 63, "shift amount … is out of range".
  - A result that is not a `usize` is N0205 at the operand: `[u8; SMALL * 2]` with `const SMALL: u32` is "expected
    'usize', found 'u32'". `SMALL as usize * 2` is the fix. Floats are N0205 too.
  - A call is N0217, "constant expressions cannot contain calls", as in a constant.
  - A local in the expression is N0353, as a bare local is: "'n' is a run-time value; use a 'Vector' for a length
    known only at run time".
  - Any other form (a block, an `if`, a call written bare) stays N0259, whose message now reads "an array length must
    be a constant expression of type 'usize' (literals, 'usize' constants and operators) or a const parameter, from 1
    to 281474976710655".
- **Const parameters** stay as ADR 0083 §2.3 decided: a bare `N` is a length, and an expression over one
  (`[T; N * 2]`, `[u8; N + 1]`) stays **N0353**, "an array length cannot be an expression over the const parameter
  'N'". In the generic body `[T; N]` equals only `[T; N]`; `[T; N * 2]` would need type equality modulo arithmetic
  (is `[T; N * 2]` the type of a `[T; N + N]`? can `N` be inferred from a `[u8; 8]`?), and §20.4's termination rule
  relies on arguments in generic bodies being bare. Evaluating the expression only in each instance would make the
  generic body's types unknown where they are checked. The program gives the item a second const parameter
  (`struct Grouped<const N: usize, const TWICE: usize>`). An interface constant read through a type parameter
  (`H.SIZE * 2`) is N0353 as `H.SIZE` alone is (ADR 0086 §2.4); on a concrete type (`Ts.SIZE * 2`) it is a constant.

## 3. Alternatives considered

- **Keep the rule; write a constant.** `const TOTAL: usize = CHARS * 8;` works today, but every derived size needs a
  name, and the length is no longer next to its meaning. The checker already evaluates the same expressions for
  constants and for const arguments in types, so refusing them here is an exception without a reason.
- **Allow expressions over const parameters, evaluated per instance** (Rust's `generic_const_exprs`): out of scope,
  for the reasons in §2. ADR 0083 §1.1 found no such use in the Bun port; a later ADR can lift N0353 there.
- **Accept any expression the evaluator can fold** (blocks, `if`): §10 does not make them constant, and an array length
  is no place to start.

## 4. Compatibility

Only programs that were refused (N0259) are accepted. N0259's message changes its wording; its code, span and
conditions for every other case are unchanged. A local in an operator expression (`[u8; n * 2]`) was N0259 and is
N0353 now, as `[u8; n]` already was.

## 5. Tests

- Corpus `run-pass/adr0143-constant-array-lengths.cee`: lengths written as products, quotients, shifts, `|`, `as` and
  parenthesised sums of literals and constants (top-level and associated), in struct fields, locals, parameters,
  repeats and a generic struct's field; the types equal to the literal's (`[u8; 2 * 8]` assigned from a `[u8; 16]`);
  `.length` of each.
- Corpus `check-fail/adr0143-constant-array-lengths.cee`: overflow, a negative result, division by zero, a `u32`
  constant, a float, a call, a local, an expression over a const parameter, 0 and 2^50.

## 6. Not in this ADR

- Expressions over const parameters (above), `const fn`, and constant arrays of other element types (§10).
- Array lengths in other positions (there are none: a length appears only in `[T; N]` and `[v; N]`).

## 7. Implementation and evidence

`arrayLengthValue` (`src/check/const_generics.cee`) sends an operator expression that names no const parameter and
no local to `constantLength`, which checks it against `usize` with `inConstantInitializer` set (so literal arithmetic
is not folded twice) and folds it with `consts.cee`'s `fold`; `boundAccessorIn` refuses an interface constant read
through a bound inside it. See ROADMAP §10's row for the tests run.
