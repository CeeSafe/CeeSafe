# ADR 0085 — Pattern guards and richer patterns: `if (…)` guards, `|`, ranges, `@`, constants, strings, slices, `is`

- Status: **Proposed**, 2026-11-09. Raised by the Bun port's M0 (`~/dev/CeeSafeApps/bun/ROADMAP.md` §3, M0): the probe
  `analysis/probes/g2_guard.cee` (`Some(n) if n > 3 => …`) fails on ceesafec 0.24.2 with N0100 "expected '=>', found
  'if'". Rests on 25 probe programs compiled with ceesafec 0.24.2 on 2026-11-09 (§1.1), and on counts of Bun 1.4.4's
  Rust (`upstream/src`, 3,238 `.rs` files, 1,743,082 lines) made by four Python scripts run with `python3 -I`
  (§1.2), kept in `~/dev/CeeSafeApps/bun/analysis/adr-evidence/0085/`.
- Scope: type-system.md §15.2 (patterns, scrutinee types, bindings, exhaustiveness), §15.3 (value arms), §18.4
  (moving payloads out), §4 (literals in patterns), §9 (`if` and `while` conditions); grammar.md §1.3 (the contextual
  word `is`), §1.7 (the token `..=`), §2.1 (`MatchExpr`, `ValueArm`, precedence); diagnostics.md (N0108, N0207, N0256,
  N0257 and new codes); the parser, the pattern checker (`src/check/patterns.cee`), the usefulness check
  (`src/check/usefulness.cee`), NIR lowering (`src/nir/nir_builder.cee`), the C backend, the formatter; the interpreter
  only through NIR; guide/structs-and-enums.md; the corpus.

## 1. Context

### 1.1 What CeeSafe has today

The pattern grammar (type-system.md §15.2) is `_`, an integer, character or `bool` literal (an integer one may be
negated), `null`, and a variant `E.V(…)` / `Some(…)` / `Ok(…)` / `Err(…)` whose payloads are bindings (`x`, `var x`,
`_`) or nested patterns (M11, G1). The parser has exactly these four forms: `PatternNode` is `Wildcard`, `Null`,
`Literal`, `Variant` (`src/syntax/ast.cee:185`), read by `parsePatternLevel` (`src/syntax/parser.cee:1931`).
Exhaustiveness and unreachable arms use Maranget's usefulness algorithm (`useful`, `src/check/usefulness.cee:161`), and
integer literals never cover their type. NIR lowers a flat match to a `Switch` (`lowerMatch`,
`src/nir/nir_builder.cee:740`) and a nested one to sequential tests that fall through to the next arm
(`lowerNestedMatch`, `:909`, with `testPattern`, `:958`). The interpreter runs NIR, so it follows the lowering.

Probes (`~/dev/CeeSafeApps/bun/analysis/adr-evidence/0085/*.cee`, ceesafec 0.24.2):

| Form | Example | Today |
|---|---|---|
| guard | `Some(n) if n > 3 => …` | N0100 "expected '=>', found 'if'" |
| or-pattern | `' ' \| '\t' => …` | N0100 "expected '=>', found '\|'" |
| range | `'a'..='z'`, `'a'..'z'` | N0100 "expected '=>', found '..'" |
| `@` binding | `Some(n @ 5)` | N0100 "expected ')', found '@'" |
| named constant | `LF => …` (`const LF: u8 = 10`) | N0205 "a variant pattern needs an enum" |
| top-level binding | `n => …` on a `u8` | N0205, same message: a bare name is read as a variant |
| struct pattern | `P { x: 1, y } => …` | N0100 |
| slice pattern | `[first, ..] => …` | N0108 "expected a pattern, found '['" |
| string literal | `"get" => …` | N0108; and `match` on a `StringView` is N0207 |
| byte string | `b"abc" => …` | N0100: there is no byte-string literal at all |
| `let … else` | `let Some(n) = x else { return 0; };` | N0100 |
| `if let`, `while let` | `if (let Some(n) = x)`, `while (let …)` | N0102 "expected expression, found 'let'" |
| `matches!` | `matches(c, 'a')` | N0200 |
| works | `Tok.C(4)`, `K.A(1, _)`, `Some(true)`, a `bool` `match` expression, a duplicate literal (N0257) | |

So a guard today is a nested `if` inside the arm with the fall-through arm's body copied into its `else`, and the rest
are chains of literal arms or `if` tests.

### 1.2 What Bun's Rust uses

Counted over all 3,238 `.rs` files under `upstream/src`, test directories included, by Python scripts of regular
expressions (`patterns.py`, `patterns2.py`, `bytestr.py`, `recount.py`, kept with the probes), run with `python3 -I`.
An *arm* is a line `PATTERN => …` (rustfmt puts each arm head on one line); its *pattern* is the text before a
top-level `if`. Line-based counts are close, not exact: a multi-line or-pattern counts once; `macro_rules!` arms are
excluded. Without the `test`, `tests` and `testing` directories there are 95,017 arms and 6,778 guards. `matches!`
is counted by `recount.py` per call, its parentheses balanced across lines. `src/lint` (a linter, 28,129
arms) is shown apart because it is the densest user.

| Form | Arm lines (all) | outside `src/lint` | Needs |
|---|---|---|---|
| match arms in total | 95,140 | 67,011 | — |
| guard `P if g =>` | 6,780 (7.1%) | 3,617 | this ADR |
| or-pattern `A \| B` | 6,362 | 4,296 | this ADR |
| byte literal `b'x'` (inside any pattern) | 2,652 | 2,050 | works: `'x'` is an integer pattern |
| byte-string `b"…"` (not counting 4,379 `comptime_string_map!` entries) | 2,546 | — | this ADR |
| string literal `"…"` | 414 | — | this ADR |
| integer or byte range `..=` | 352 arms; 526 lines anywhere | 270 | this ADR |
| named constant (`UPPER_CASE` in a pattern) | 890 | 816 | this ADR |
| slice pattern `[…]` | 764 arms + 383 `let`/`if let` | 437 | this ADR |
| `x @ P` | 380 | 194 | this ADR |
| tuple `(a, b) =>` | 4,058 | 2,140 | not here (§4): no tuples |
| struct-like `V { f, .. }` | 4,446 | 2,790 | not here (§4): no named payloads |
| `if let` / `while let` / `&& let` chains | 14,903 / 1,670 / 1,881 | — | this ADR (`is`) |
| `matches!(…)` calls (1,667 with a guard inside) | 8,125 | — | this ADR (`is`) |
| `let … else` | 7,806 | — | translation rule (§2.9) |

What the 6,780 guards test: a method test (`is_*`, `contains`, `starts_with`, `len()`) 2,574; `==`/`!=` 1,719; another
call 1,013; a flag or other 968; a nested `matches!` 297; an ordering 209. 1,183 guard a bare binding or `_` (an
`if`/`else` chain written as a `match`). Only 4 take `&mut` of anything, none of a binding; 3 have a visible side
effect (`self.visited.insert(…)`); none awaits. Rust already gives a guard shared access only.

Examples:

```rust
Some(wrote) if wrote == bytes.len() => {}                       // src/http_jsc/websocket_client.rs:1117
0x5F | 0x24 | 0x61..=0x7A | 0x41..=0x5A => {                    // src/js_parser/lexer.rs:1742
b'a'..=b'z' | b'A'..=b'Z' | b'0'..=b'9' | b'_' | b'-' => self.advance(1),   // src/css/css_parser.rs:4584
[ref rest @ .., 0x09..=0x0D | b' '] => rest,                    // src/bun_core/string/immutable/whitespace.rs:87
c if c == QUOTE => {     // src/js_parser/lexer.rs:815: "const-generic param can't be a pattern; guard is fine"
```

The lexers and parsers (`js_parser`, `css`, `parsers/{json,json5,yaml,toml,xml}`, `shell_parser`) are almost all
byte `match`es with or-patterns and ranges. Ported today, each becomes an `if` chain, and the jump table is lost.

### 1.3 Prior decisions

This ADR **extends** §15.2's pattern grammar and its M11 nested patterns, and **amends** one rule there: integer
literals never cover their type (§2.4 below lets ranges cover it). It **extends** §15.2's scrutinee types to
`StringView`, `Slice<T>` and arrays. It keeps ADR 0072's ruling that `Slice<u8> == StringView` is N0205: a string
pattern is typed by its scrutinee, as `'k'` is (§4), and is not an `==`. Value arms (§15.3, ADR 0073 §2.3), moves out
of a consuming match (§18.4) and pattern bindings that shadow a dependency (ADR 0074 §2.10) are unchanged.

## 2. Proposal

### 2.1 Grammar

```text
Arm         = Pattern Guard? '=>' Block ','?               (* §15.2 *)
ValueArm    = Pattern Guard? '=>' Branch                   (* §15.3 *)
Guard       = 'if' '(' Expr ')'
Pattern     = '|'? AltPattern ('|' AltPattern)*            (* or-pattern *)
AltPattern  = (Identifier '@')? PrimPattern
PrimPattern = '_' | 'null' | Literal | Literal '..=' Literal | Literal '..' Literal
            | StringLiteral
            | QualifiedName ('(' Payload (',' Payload)* ')')?   (* a variant, or an associated constant (§2.6) *)
            | 'var'? Identifier                            (* a constant if one is in scope, else a binding *)
            | '[' (SliceItem (',' SliceItem)* ','?)? ']'
            | '(' Pattern ')'
SliceItem   = Pattern | '..' | Identifier '@' '..'          (* at most one '..' per slice pattern *)
Literal     = IntLiteral | '-' IntLiteral | CharLiteral | 'true' | 'false'
IsTest      = Operand 'is' Pattern                         (* §2.8: at the precedence of '=='; 'is' is contextual *)
```

`..=` becomes one token. It is a pattern operator only; `0..=9` in an expression is N0100 with the hint "ranges in
expressions are half-open: `0..10`".

### 2.2 Guards

```ceesafe
match (x) {
    Some(n) if (n > 3) => { Console.writeLine("big"); }
    _ => { Console.writeLine("small"); }
}
```

- **Syntax.** `if (…)`, parenthesised like every CeeSafe condition (§9). The guard is a `bool` (N0205 otherwise).
- **Order.** Arms are tried in order. For one arm: test the pattern (as §15.2 says: outer variant first, then nested
  patterns left to right, depth first); if it matches, bind, then evaluate the guard; if the guard is `false`, go on to
  the next arm. For an or-pattern with a guard, each alternative that matches, left to right, binds and runs the guard;
  the first `true` wins (Rust's rule, so a mechanical port keeps its meaning).
- **What the guard sees.** The arm's bindings, read-only. For a Copy payload the binding is the value. For a non-Copy
  payload in a consuming match (§18.4), the guard reads the payload in place: the move into the binding happens only
  after the guard is `true`. An arm whose guard fails has moved nothing.
- **What the guard may not do (N0355).** From the start of the pattern test to the end of the guard, the scrutinee
  carries a shared loan. The guard may read the scrutinee and the bindings, call functions, and read and write other
  places. It may not move, assign or `mut`-lend the scrutinee, a place inside it or a binding. It may not `await`:
  the loan would cross a suspension (no Bun guard does). The borrow checker reports these with N0355, whose message
  names the guard.
- **Exhaustiveness.** A guarded arm never counts toward coverage: its row is not added to the usefulness matrix. Its
  pattern is still checked for reachability: if earlier unguarded arms cover it, it is N0257. When N0256's witness
  would match a guarded arm, the message says "arms with a guard do not count; add `_` or an arm without a guard".

### 2.3 Or-patterns

`A | B` matches when either matches; alternatives are tested left to right. A leading `|` is allowed (rustfmt's
layout). An or-pattern may appear anywhere a pattern may, `Some('a' | 'b')` included. Every alternative binds the
same names, each with the same type and the same `var`-ness (N0356 otherwise). For usefulness each alternative is a
row of its own; an alternative already covered by earlier rows or earlier alternatives is N0257, pointed at that
alternative.

### 2.4 Ranges

`lo..=hi` (inclusive) and `lo..hi` (half-open, as range expressions are) on an integer scrutinee or payload. Both
bounds are integer or character literals, typed by the scrutinee as §4 types a literal (so `'a'..='z'` on a `u8`).
An empty range (`'z'..='a'`, `5..5`) is N0357. Floats stay N0207.

Coverage **changes**: integer literals and ranges cover an integer type when together they cover every value. The
usefulness check splits the type into the intervals the arms' bounds define (as Rust has since 1.33), so
`0x00..=0x7F` and `0x80..=0xFF` cover a `u8`, and N0256's witness names an uncovered value (`'{'`, `128`). A lone
literal still needs `_` beside it, as today.

### 2.5 `name @ pattern`

Binds the value the pattern matched, as a payload binding would (copy, move or reference, §15.2), and tests the
pattern. `d @ '0'..='9'`, `quote @ ('"' | '\'')`, `Err(e @ IoError.NotFound)`. It does not change coverage.

A value is moved at most once. When `name` binds by move (a non-Copy value in a consuming match, §18.4, or an `is`
that moves, §2.8), no binding inside its pattern may also bind by move: `e @ Err(inner)` with a non-Copy payload would
move the payload into `inner` and again, inside the whole value, into `e`, so it is N0250 (use of a moved value), with
the message "`e @ …` moves the value that `inner` also moves; bind one of them". Bindings under it that copy (Copy
payloads) are allowed, and so is everything on a reference scrutinee, where each binding is a reference. This is
Rust's rule for `@` (since 1.56).

### 2.6 Constants and top-level bindings

A bare identifier (no `.`, no parentheses) in pattern position is:

- a **constant pattern** when it names a `const` in scope of an integer, `bool` or fieldless-enum type. It matches
  that value and counts as that literal for coverage. A `const` of another type is N0358.
- a **const generic parameter** (ADR 0083) is N0358 ("a const generic parameter cannot be a pattern; test it in a
  guard: `c if (c == QUOTE)`"), never a binding, until a later ADR; ADR 0083 §4 states the same. Otherwise `QUOTE =>`
  would silently become a catch-all binding, and as a last arm only an unused-binding warning would show it.
- otherwise a **binding** of the value at that position, at the top level too: `n => …` binds the scrutinee (copy,
  move or reference, §15.2), and `var n` a mutable one.

This is Rust's rule. Its known hazard is a misspelt constant becoming a catch-all binding; the arms after it are then
N0257, and the binding warns as unused, so the mistake does not compile silently. It changes the meaning of existing
code in one case: a payload binding whose name is also a `const` in scope (`Some(MAX)`). The release scans the
repositories' corpus for it first (§5).

A qualified name without parentheses (`Type.NAME`, `pkg.Type.NAME`) is a variant pattern when it names a variant, and
a constant pattern when it names an associated constant (ADR 0074 §2.7) of a type allowed above. Read through a type
parameter (`T.NAME`, or `Self.NAME` in an interface default, ADR 0086) it is a value known only per instance, not a
constant, so it is N0358 there too.

### 2.7 Strings and slices

- **String literal patterns.** On a `StringView` scrutinee, `"get"` matches equal bytes, as `==` does (§22.2). On a
  `Slice<u8>` scrutinee, the same literal matches its UTF-8 bytes: the literal is typed by the scrutinee, as `'k'` is.
  Bun's `b"cjs"` is written `"cjs"`. A byte string that is not UTF-8 is a slice pattern of integers (below). String
  patterns never cover their type; a duplicate is N0257. `match` on a `StringView` is no longer N0207.
- **Slice patterns** on `Slice<T>`, `[T; N]` and references to them: `[]`, `[a, b]`, `[first, ..]`, `[.., last]`,
  `[x, rest @ ..]`, `['"', inner @ .., '"']`. Each item is a pattern on one element; `..` matches any number of
  elements, at most once (N0100 otherwise); `rest @ ..` binds them as a `Slice<T>` of the same region.
  - **Bindings.** A slice is a view, so it never moves: a Copy element binds a copy, a non-Copy one a `borrow T`. On
    an array by value, an element binding needs a Copy element type; otherwise N0205 with the hint "match on
    `a[..]`".
  - **Coverage.** A slice's constructors are its lengths: `[p1, …, pk]` covers length k, a pattern with `..` and k
    items covers every length ≥ k. The check splits lengths at 0 … L and "L or more", L being the largest k in the
    arms (Rust's algorithm). An array has one length, N.

### 2.8 `e is P`: patterns as conditions

One boolean operator replaces Rust's `matches!`, `if let`, `while let` and let chains:

```ceesafe
let isDigit = c is '0'..='9';
if (cache.find(key) is Some(entry) && entry.fresh) { use(entry); }
while (parts.next() is Some(part)) { handle(part); }
```

- **`is` is contextual.** It is the operator only where a complete operand is followed by `is`; everywhere else it is
  an identifier. A keyword would break `c.types.is(t, …)`, which `src/` uses throughout.
- **Precedence.** That of `==`, non-associative. After `is` the pattern grammar reads as far as it can, so `|` there is
  an or-pattern: `(c is 'a') | flag` needs parentheses.
- **Meaning.** The test of `e is P` is that of `match (e) { P => true, _ => false }`: `e` is evaluated once; tests
  only read.
- **Bindings** are in scope only where the test is known true: in the later operands of the same top-level `&&`
  chain, and in the `if`'s then-branch or the `while`'s body whose condition is that chain. A pattern with bindings
  anywhere else (under `||` or `!`, in a `let` initialiser, as an argument) is N0359. Bindings copy, move or
  reference as in §15.2.
- **Moves happen only on success.** Unlike a consuming `match` (§18.4), `is` moves nothing when the test fails. When
  it succeeds and binds a non-Copy payload of a value scrutinee, the scrutinee is consumed at that point, as §18.4
  consumes it (unbound payloads go to hidden locals). The move checker sees a place scrutinee as moved on the true
  path only, so `if (e is Some(v)) { A } else { B }` leaves `e` whole in `B`, as Rust's `if let` does.
- **Drop points.** Bindings and hidden locals live to the end of the then-branch, or of the `while` body for that
  iteration. When a later operand of the `&&` chain is `false`, those made so far are destroyed at once, in reverse
  order, before the `else` branch runs or the loop exits: in `x is Some(v) && g`, a false `g` destroys `v` there.
- **Temporaries.** A temporary scrutinee is destroyed at the end of the condition, unless a binding refers into it
  (a reference, or a slice or view of it, §2.7); then it lives to the end of the then-branch or of this iteration's
  body. It never lives into the `else` branch or the next iteration (Rust 2024's `if let` scope, applied to `while`
  too). So a `RefCell` guard (ADR 0084) used to copy a value out is gone before the body, and a `.write()` in the
  body or in `else` does not panic; one that the body still reads through is visible in the condition. A `while`
  evaluates the scrutinee again each iteration.
- `matches!(x, P if g)` is `x is P && g`.

### 2.9 Translation rules (no language change)

| Rust | CeeSafe |
|---|---|
| `let Some(n) = x else { return 0; };` | `let n = match (x) { Some(n) => n, _ => { return 0; } };` (works today) |
| `b'x'` in a pattern | `'x'` |
| `b"abc"` in a pattern | `"abc"` (§2.7) |
| `ref x`, `ref mut x` | match on `borrow` / `borrow mut` (§15.2) |
| `if let P = e { A } else { B }` | `if (e is P) { A } else { B }` (`e` moves only on success, §2.8) |
| `match (a, b) { … }` | nested `match`, or `is` chains (§4) |
| `comptime_string_map!` / `phf_map!` | a `static` view table (ADR 0066) searched by a function |

### 2.10 Lowering

- **Switch where it can be.** A match whose scrutinee is an integer, `bool` or fieldless enum, with no guard, and whose
  arms are literals, constants, ranges and or-patterns of them (and a final `_` or binding), lowers through
  `lowerMatch` to one NIR `Switch`. Ranges and or-patterns expand into cases when the expanded count is at most 1,024
  (a fixed number, so the output is deterministic); past it, the match takes the sequential path. Bun's lexer arms
  become a C `switch`, which C compilers turn into a jump table.
- **Sequential tests otherwise.** Guards, strings, slices, `@` and or-patterns with bindings use
  `lowerNestedMatch`'s per-arm test and fall-through. `testPattern` gains: a range (two comparisons), a string (length,
  then `cee_slice_eq`, ADR 0072), a slice (a length test, `==` k or `>=` k, then element tests at index i or
  `len - k + j`; the indexes are proven by the length test and emitted without bounds checks), an or-pattern (each
  alternative's test jumps to its own bind block, since its bindings sit in other places, then to the shared body), a
  guard (a branch after binding, to the body or to the next arm).
- **No decision trees.** Compiling to Maranget's decision trees would test each position once but duplicate arm
  bodies or need join blocks for bindings and moves. The C compiler already merges chains of tests on one value. This
  can be an optimisation later, behind the same semantics.
- **The interpreter** runs NIR and needs nothing new: ranges lower to comparisons, slice tests to `.length` and
  indexing. **The formatter** prints `a | b`, `'a'..='z'`, `n @ P`, `P if (g) =>`, `e is P`, one space around each.

### 2.11 Diagnostics

Codes N0355–N0359 are this ADR's share of the set's numbering (ADRs 0081–0087). A double move through `@` (§2.5) is
the existing N0250 with its own message.

| Code | Meaning | Example |
|---|---|---|
| N0355 | a guard moves, assigns or `mut`-lends the scrutinee or a binding, or awaits | `Some(v) if (consume(v)) =>` on a non-Copy `v` |
| N0356 | alternatives of an or-pattern bind different names, types or `var`-ness | `Some(x) \| null =>` |
| N0357 | an empty range pattern | `'z'..='a'` |
| N0358 | a constant of a type that cannot be a pattern, a const generic parameter, or `T.NAME` through a type parameter | a `const` struct or `f64`; `QUOTE =>` with `<const QUOTE: u8>` |
| N0359 | an `is` pattern with bindings where the test is not known true | `if (!(x is Some(n))) …` |

N0108's text lists the new forms; N0207 no longer applies to `StringView`, `Slice<T>` or arrays; N0256 and N0257 gain
the guard wording of §2.2.

## 3. Alternatives considered

- **Guards as `when g` (C#) or `if g` without parentheses (Rust).** `when` is a new word; a bare `if g` would be the
  one condition in CeeSafe without parentheses. `if (g)` reuses both the keyword and the rule.
- **`if (let P = e)` and `while (let P = e)`, plus a `matches(e, P)` built-in.** Closer to Rust's text, but two
  constructs for one idea, and a built-in that takes a pattern as an argument is a new kind of call. `is` is one
  operator, reads as C#'s does, and composes with `&&`.
- **`is` as a keyword.** Simpler to parse, but it breaks the identifier `is`, used across `src/`.
- **Keep "integer literals never cover their type".** A port then adds `_ => unreachable` to every split byte match.
  An unreachable panic is weaker than a checked proof, and interval splitting is a known, small extension.
- **Constants only when qualified (`Bytes.LF`, an associated constant, ADR 0074 §2.7), bare names always
  bindings.** No meaning change and no misspelling hazard, but 890 Bun patterns would need renaming into namespaces,
  and a top-level `LF =>` would bind silently. The N0257 and unused-binding backstop makes Rust's rule safe enough.
- **A `b"…"` byte-string literal.** The set's rule for literal prefixes: a letter written directly before `"` is a
  prefix, reserved (N0100) unless an ADR defines it. ADR 0087 defines `c"…"`; `b` is not added here, because a string
  pattern is typed by its scrutinee (§2.7), so patterns do not need it. Byte strings in expressions are §4's.
- **Guards translated to nested `if`s.** That is today's rule: it copies the fall-through arm's body into each `else`,
  6,780 times, and loses exhaustiveness checking of what follows.

## 4. Not in this ADR

- **Tuple patterns and `match (a, b)`** (4,058 arm lines): CeeSafe has no tuples. Translation: a nested `match`, or
  `is` chains. If tuples come, their patterns follow §15.2's payload rules.
- **Struct patterns and named variant fields** (`V { f, .. }`, 4,446): CeeSafe enum payloads are positional.
  Translation: positional payloads, or field reads in a guard.
- **`let P = e else { … };`**: a translation rule (§2.9). It may come later as `let`'s own form.
- **Byte-string literals in expressions**, float patterns, `box`/deref patterns, patterns on `String` or `Vector`
  values (match on their view).

## 5. Testing

- **The corpus, run-pass**, each also in the interpreter, outputs compared: guards (order, fall-through, a guard on a
  consuming match that fails and moves nothing, an or-pattern whose guard runs twice), or-patterns at the top and
  nested, ranges both ways and an exhaustive split `u8` match, `@`, constants and top-level bindings, string patterns
  on `StringView` and `Slice<u8>`, slice patterns of every shape including `rest @ ..`, `is` in `let`, `if`, `while`
  and `&&` chains, `matches!`-style `is` with a following `&& g`, `if (e is P) … else` using `e` in `else`, a false
  `&&` operand destroying a binding before `else` (a `drop` that prints), a temporary destroyed at the end of the
  condition (a `RefCell` written in the body).
- **Check-fail**: each of N0355–N0359 (a const generic parameter and `Self.NAME` as patterns included); N0250 for
  `e @ Err(inner)` with a non-Copy payload; N0256 with only guarded arms; N0257 for a guarded arm and for an
  or-alternative; `..=` in an expression; two `..` in a slice.
- **Usefulness unit tests**: witnesses for interval splits (`'{'`, `128`, `-1`), for slice lengths (`[]`, `[_, _, ..]`).
- **Parsing and formatting**: parse-pass snapshots for `is` precedence and `|` after `is`; `ceesafec fmt` round-trips.
- **C backend**: a snapshot that a lexer-style range match emits one `switch`, and one past 1,024 cases emits tests.
- **The Bun probe**: `analysis/probes/g2_guard.cee` (with `if (n > 3)`) compiles and prints `big`; the five §1.2
  examples, ported by the rules above, compile and agree with Rust's results.
- **Meaning change scan** (§2.6): before release, the checker lists every payload binding named like a `const` in
  scope across CeeSafe, CeeSafeLibs and CeeSafeApps; each is renamed first.

## 6. Questions for the owner

1. **Guard syntax `P if (g) =>`,** with parentheses. **Recommended: yes**: it is CeeSafe's condition syntax.
2. **`e is P` as the one form for `matches!`, `if let`, `while let` and let chains,** with `is` contextual.
   **Recommended: yes** (§3: one operator; the identifier `is` keeps working).
3. **Integer literals and ranges cover their type when together they cover every value.** This amends §15.2.
   **Recommended: yes**.
4. **Bare identifiers in patterns: a constant if one is in scope, otherwise a binding, at every depth** (Rust's rule,
   with the §5 scan). **Recommended: yes**.
5. **String literal patterns typed by the scrutinee** (`StringView` or `Slice<u8>`), no `b"…"` token here.
   **Recommended: yes**.
6. **An or-pattern with a guard runs the guard once per matching alternative**, as Rust does. **Recommended: yes**, for
   a faithful port; the order is still fixed.
7. **Release in two steps:** 0.25.0 with guards, or-patterns, ranges, `@`, constants and top-level bindings (§2.2–§2.6,
   what the lexers need); 0.26.0 with strings, slices and `is` (§2.7–§2.8). **Recommended: yes**: the first step is
   mostly parser and usefulness work, the second touches scoping and new scrutinee types.
