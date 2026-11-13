# ADR 0117 — `let PATTERN = e else { … };`: bind or leave

- Status: **Accepted** by the owner, 2026-11-13 ("Fix those new gaps too"), implemented on branch `fix/gaps2-c`;
  released in ceesafec 0.42.0. Raised while writing the language's book: a function that needs a value's payload for the rest
  of its body had to nest that body in an `if`.
- Scope: grammar.md §2 (`LocalDecl`); type-system.md §15.5 (new), §15.4 (N0404); diagnostics.md (N0100 and N0213:
  wording; N0404: a new warning); formatting.md (nothing new: the statement formats by the existing rules); the
  parser, the AST and HIR (one statement each), the checker, the NIR lowering; the corpus, the formatter's
  snapshots and the guide. No change to the NIR, the borrow, move and drop passes, the C backend, the interpreter
  or the runtime.

## 1. Context

`e is P` (ADR 0085 §2.8) binds a pattern's names where the test is known true: in the then-branch of an `if`, the
body of a `while`, the later operands of an `&&` chain. That covers "if it matches, do this". It does not cover
"if it does not match, leave; otherwise go on with the names":

```ceesafe
fn area(s: Shape) -> u32 {
    if (!(s is Shape.Rect(w, h))) { return 0; }     // N0359: a pattern under '!' binds nowhere
    return w * h;
}
```

The forms the language had:

- `let n = match (v) { Some(n) => n, null => { return 0; } };` works for one payload. For a variant with two
  (`Shape.Rect(w, h)`) there is nothing to yield them in: CeeSafe has no tuples.
- `if (s is Shape.Rect(w, h)) { …the rest of the function… } else { return 0; }` nests the rest of the function one
  level for each value it takes apart.

So the early return, the first thing a function does with an optional argument or a `Result`, had no flat form for
the general pattern.

## 2. Decision

### 2.1 The statement

```text
LocalDecl = … | 'let' Pattern '=' Expr 'else' Block ';'
```

```ceesafe
fn area(s: Shape) -> u32 {
    let Shape.Rect(w, h) = s else {
        return 0;
    };
    return w * h;
}
```

`let P = e else { B };` evaluates `e` once and tests it against `P`, as `e is P` does (§15.4: any pattern of §15.2,
the scrutinee types of a `match`, N0207 otherwise).

- **Where it matches**, the pattern's names are bound, and are in scope from the end of the statement to the end of
  the enclosing block, as a `let`'s name is. They are `let` bindings; `var x` inside the pattern makes one mutable
  (§15.2). A name the function already declares is N0203, as for any `let`.
- **Where it does not**, `B` runs. `B` does not see the pattern's names (N0200 for a use), and it must leave: its
  end must not be reachable (N0213). `return`, `break`, `continue`, a call of a function that returns `never`
  (`Process.exit`, `panic`), or an `if`/`match` whose every path does one of these. This is the flow rule a function
  that returns a value already obeys (§15.3), applied to the block.

It is a statement, written with `let` and ended with `;`, as every `let` is.

### 2.2 It is `is`, with the bindings in the enclosing scope

Everything else is §15.4's rule for `if (e is P) { rest of the block } else { B }`, with one difference in where
the temporaries end:

- **Moves happen only on a match.** A pattern that binds a non-Copy payload of a value moves it out and consumes the
  value, as a consuming `match` does (§18.4; unbound payloads go to hidden locals that end with the enclosing
  block). In `B` the value is whole: `let Some(bytes) = v else { return v.isNull(); };` is accepted, and a use of `v`
  after the statement is N0250.
- **A reference scrutinee** binds references into its referent (§15.2), which the borrow checker follows as for any
  `let`.
- **Temporaries** of `e` end at the end of the statement on the matching path, as a `let`'s do (§14.5), and before
  `B` runs on the other. So a `RefCell` guard taken to read the value (`let 1..=9 = cell.read().data().length else
  { … };`) is released before the next statement and before `B`. A binding that would refer into a temporary
  (`let [first, rest @ ..] = make() else { … };`) is therefore N0243, "a temporary value is dropped at the end of
  this statement while still borrowed; bind it to a local first", as `let r = make().view();` is. §15.4 keeps such
  temporaries for the then-branch of an `if`; a `let … else` has no such branch to end them at, and extending them
  to the end of the enclosing block would be a second lifetime rule for `let`.
- **Drop order.** The bindings are locals of the enclosing block, declared at the statement: they are destroyed at
  the block's end, in reverse order of declaration, with the block's other locals.

### 2.3 A pattern that tests nothing (N0404)

`let x = e else { … };` and `let _ = e else { … };` parse (a name and `_` are patterns), and their `else` block can
never run. The same holds for `if (n is _)` and `if (n is m)` with `m` a fresh name, which §15.4 accepted in
silence. Both are now warning **N0404**: "this pattern tests nothing: it matches every value", with what to write
instead (`let name = value;`).

The warning is for a pattern that is `_` or a binding (through `name @`). A pattern that takes a value apart and
happens to cover its type, `arr is [first, rest @ ..]` on an array or `w is Wrapper.Only(x)` on a one-variant
enum, is not warned about: `is` (and now `let … else`) is the only way to take such a value apart, since `let`
takes no pattern without `else`. (An irrefutable `let P = e;` is a possible later step; it is not needed for this
gap and is not part of this ADR.)

### 2.4 What is not allowed, and the messages

All N0100, each reported once, at the token:

- `let Some(v) = x;` — "expected 'else', found ';': 'let' with a pattern that can fail is
  'let PATTERN = e else { … };', whose block leaves (return, break, continue, or a call that does not return) when
  the value does not match (ADR 0117); to test and go on, write 'if (e is PATTERN) { … }' or a 'match'". (Before
  this ADR: "expected ';', found '('".)
- `var Some(v) = x else { … };` — a pattern is written after `let`; `var x` inside it makes a binding mutable.
- `let n: T = x else { … };` — a `let … else` takes no type: the pattern says what is expected.

What follows `let` is a pattern when it starts as one that cannot be a declared name: a literal, `null`, `-1`, `(`,
`[`, a string, `|`, or a name followed by `(`, `.`, `@` or `|`. A single name followed by `=` is a `let` as before;
if `else` follows its value, it is a `let … else` whose pattern is that name (a constant in scope, §15.2, or the
binding N0404 reports). No program that parsed before changes meaning: `else` could not follow a `let`'s value.

## 3. Alternatives

- **`e ?? { return; }`**, a diverging block as `??`'s fallback. Only for optionals, one payload; `Result` and enum
  variants stay nested. A second meaning for `??`'s right side.
- **Bindings of `!(e is P)` flow to what follows an `if` that leaves** (flow typing: `if (!(s is Shape.Rect(w, h)))
  { return 0; }` then `w` in scope). It reads well, but scope would then depend on flow analysis of the `if`'s
  block, and N0359's rule ("binds where the test is known true, lexically") would no longer be lexical.
- **`guard (e is P) else { … }`**: a new keyword for the same statement.
- **Tuples**, so that `match` can yield two payloads: a much larger feature, and the `match` form stays three lines
  of ceremony.

`let … else` is the known form (Rust 1.65, Swift's `guard let`), one production, and its meaning is already in the
language as `is`.

## 4. Implementation

- Parser: `parseLocalDecl` reads a pattern after `let` when one starts there (`startsLetPattern`), then `=`, the
  value, `else`, a block, `;`. AST: `StmtNode.LetElse(pattern, value, block)`.
- Checker (`src/check/is_patterns.cee`, `checkLetElse`): the test is checked by the function that checks `e is P`,
  with its bindings going into the enclosing block's scope; they are taken out of scope while the `else` block is
  checked; the block must not complete (`completes`, the flow analysis N0213 uses). HIR:
  `HirStmt.LetElse(test, block)`, the test being the `HirExpr.Is` an `if` would hold.
- NIR (`src/nir/is_lowering.cee`, `lowerLetElse`): `lowerIsTest`, the lowering of `is` in a condition, in a scope
  of its own; the false edge ends the statement's temporaries and goes to the block; on the true edge the
  temporaries end and the scope's locals join the enclosing block's scope. The move, borrow, effect and drop
  passes, the C backend and the interpreter see blocks, branches and locals they already handle.
- Formatter: the value breaks as a `let`'s initialiser does; the block is a block; `else` and `;` are spaced by the
  token rules. `tests/formatting/let-else.cee`.

## 5. Compatibility

None lost: every new form was a syntax error. N0404 is a warning: it stops nothing, except under
`--deny-warnings`. The catalogue gains N0404 (group A of the same round may add N0403; N0404 was taken here to
avoid a clash).

## 6. Tests

`tests/reference/tests/corpus/`: `run-pass/let-else.cee` (optionals, `Result`, a variant with two payloads, a
nested pattern, ranges and or-patterns, `var` bindings, `continue` and `break` in a loop, a non-Copy payload moved
out and the value whole in the `else` block, a `RefCell` guard released at the statement's end, a `never` call in
the block; native, `--interp` and `--check-leaks`), `check-fail/let-else.cee` (N0213 twice, N0200, N0404 three
times, N0207, N0205, N0214), `check-fail/let-else-moves.cee` (N0250, N0243 twice), `parse-fail/let-else.cee`,
`parse-pass/let-else.cee` with its `.ast`, `check-pass/always-true-is.cee` (N0404 on `is`), and
`tests/formatting/let-else.cee` with its fixed point.
