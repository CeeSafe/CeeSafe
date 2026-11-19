# ADR 0176 — Bitwise complement: unary `~` on unsigned integers

- Status: **Accepted** by the owner, 2026-11-19 (asked how to add a complement after ClayWind's finding P6: "Unary
  ~"). Implemented the same day (§5); released in ceesafec 0.76.0.
- Scope: the grammar (precedence level 11), type-system.md §6 (the operator table and literal arithmetic), the
  checker, constant evaluation, the interpreter, the C backend, the guide, the book and the agents template.

## 1. Context

- **The gap.** `&`, `|`, `^`, `<<` and `>>` are defined on unsigned integers (§6); the complement was not. The lexer
  read `~` and the parser refused it (N0102, "expected expression"), and `!x` on a `u32` is N0205, since `!` is for
  `bool`. CeeSafeReporting's MD5 (a PDF/A trailer's ID) needs `~x` and wrote `x ^ 0xFFFFFFFF`: it says less, and the
  width has to be spelt out in the mask, once per type.
- **Why it was absent.** Not a decision: signed bit operations were left out (M1's reasoning: no sign bit masked or
  shifted by accident), and the complement went with them. On unsigned types it has no such hazard.

## 2. Decision

- **`~x`** is a prefix operator at level 11, with `-` and `!`: `~a & b` is `(~a) & b`, `-~x` and `~~x` parse.
- **Operand:** an unsigned integer (`u8` to `u64`, `usize`). The result has its type, every bit flipped. It cannot
  fail. A signed integer, a float, a `bool` or anything else is N0207, the code `&` gives; on a signed integer the
  message adds the bit operators' hint (`truncate<uN>(x)`), on a `bool` that `!` negates it.
- **Literals** adapt as for the other bitwise operators: `let m: u16 = ~0x00FF;` is `0xFF00`. An unsuffixed literal
  with no expected type is `i32`, so `~0` alone is N0207, as `0 & 1` is.
- **Literal arithmetic and constants:** `~` is one of literal arithmetic's operators (ADR 0110) and is evaluated in
  a constant's value (§10): `const ALL: u64 = ~0;`.
- **C:** `((T)~(x))`. The cast is needed: C promotes a `uint8_t` or `uint16_t` to `int` before `~`.

## 3. Options not taken

- **A method, `x.complement()`.** No parser change, but it reads unlike the operators beside it, and every C-family
  programmer writes `~` first.
- **Both.** Two spellings of one operation.
- **`!x` on integers.** One operator for two meanings, logical and bitwise, and a `bool`'s `!` and an integer's
  would differ in what they flip.

## 4. Consequences

- `x ^ 0xFFFFFFFF` is still valid; nothing changes meaning. No program that compiled before compiles differently.
- The agents template's line "There is no `~`" now says that `~` and the bit operators take unsigned integers.

## 5. Implementation (2026-11-19)

- `src/syntax/ast.cee` (`UnaryOp.Complement`), `src/syntax/parser.cee` (`~` beside `-` and `!`), the AST, HIR and
  NIR printers (`compl`, `Complement`).
- `src/check/expr.cee` (`synthComplement`; `~` in `isFlexible` and `isLiteralArithmetic`), `src/check/consts.cee`
  (folding, masked to the type's width), `src/check/const_generics.cee` (not a const argument).
- `src/interpreter/interpreter.cee` and `src/backend/c_body.cee`.
- Tests: `parse-pass/complement`, `run-pass/complement` (every unsigned width, constants, literal arithmetic, a
  compound assignment), `check-fail/complement-types`.
