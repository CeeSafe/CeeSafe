# Formatting (M10 T1)

`ceesafec fmt` rewrites CeeSafe source into one canonical style. It has no options: two programmers who run it get the same
spacing, indentation, blank lines, brace placement and comment alignment. Where the style leaves a choice to the author
(§3), it is a choice of line breaks, which the formatter keeps.

The formatter changes only whitespace, and the trailing commas of §3.3. A formatted file parses to the same tree as
the original (the same AST, apart from spans) and keeps every comment, in order. Formatting a formatted file changes
nothing.

## 1. Command

```sh
ceesafec fmt file.cee...          # rewrite each file in place (only if it changes)
ceesafec fmt --check file.cee...  # change nothing; list the files that would change, exit 1 if any
ceesafec fmt --stdout file.cee    # print the formatted text of one file
```

A file with a syntax error is not formatted: its diagnostics are printed, the file is left as it is, and the exit
status is 1. A directory argument means every `.cee` file under it.

## 2. Lines and indentation

- The text is UTF-8 with LF line endings; it ends with exactly one newline. No line has trailing whitespace.
- Indentation is 4 spaces per level; there are no tabs.
- A level is opened by a `{` that ends its line (a block, a struct, enum, interface or impl body, a match, a
  multi-line struct or array literal) and by a broken list (§3.3). Its contents are indented one level, and its
  closing delimiter is on its own line at the opening line's indentation.
- **Blank lines:** at most one in a row. None directly after an opening `{` or before a closing `}`, and none at the
  start or end of the file. Elsewhere, between two items, two members or two statements, a blank line is kept if
  the source has one or more, and not added if it has none.
- **Line width:** 120 columns, counting the code and not a trailing comment. A line longer than that is broken by §3.4
  where it can be. A line that cannot be broken (a long string literal, a long name) stays long.
- **Raw literals** (`grammar.md` §1.6, ADR 0074 §2.6): the content lines and the closing line of a `"""` literal move
  with the line its opening `"""` ends, by the change in that line's indentation, and are never reflowed or trimmed:
  their trailing spaces are part of the value. Fewer columns are removed than the change when the closing line's
  indentation is narrower, since removing more would change the value. Their width does not count.

## 3. Line breaks

### 3.1 What the author decides

The formatter keeps these choices and only lays out the result:
- whether a block, struct, enum, interface or impl body, or match is written on one line (`{ return x; }`) or across
  lines (anywhere between its braces);
- whether a list (arguments, parameters, struct fields, array elements) is broken, and, in a broken list, which
  elements start a new line (so a literal can keep several fields per line);
- whether an expression body (`=> expr`) starts on the line of the `=>` or on the next one;
- where a long binary expression or a chain of member calls (a `.` after a call, an index, `!` or `?`) is broken;
- whether an attribute is followed by a line break.

Any other line break in the source is removed: two tokens on different lines that no rule here separates are joined
with their §4 spacing. In particular `else` always follows the `}` before it on the same line.

A one-line form is kept only if it fits in 120 columns and holds no line comment; otherwise it is broken, as if the
author had written it across lines.

### 3.2 One-line forms

- A one-line block has one space inside each brace: `{ x += 1; }`, `{ break; }`. An empty block is `{}`.
- A one-line struct body is `struct Point { x: i32; y: i32; }`; a one-line enum is `enum E { A, B(i32) }`.
- A one-line match is `match (v) { 0 => "zero", _ => "many" }`, and as a statement
  `match (o) { Some(v) => { use(v); } null => {} }`.
- A one-line struct literal is `Point { x: 1, y: 2 }`; an array literal is `[1, 2, 3]`; argument and parameter lists
  are `f(a, b)` and `fn f(a: i32, b: i32)`. One-line lists have no trailing comma.
- **An if/else chain is laid out as a whole:** if one of its blocks is broken, or the author started an `else` on a new
  line, every block of the chain is broken.

```ceesafe
if (c) { a(); } else { b(); }      // fits: kept on one line
if (c) {                           // otherwise: every block broken
    a();
} else {
    b();
}
```

### 3.3 Broken lists

Statements, members, enum variants and match arms of a broken body are one per line. A list that is broken (by its
author, or by §3.4) is laid out as a block:
- the opening delimiter ends its line;
- the elements are indented one level; each element that the author started on a new line starts a new line, and
  the others follow the element before them after `, `; a list broken by §3.4 has one element per line;
- the closing delimiter starts its own line at the opening line's indentation;
- the last element is followed by a comma, where the grammar allows one (grammar.md §2: parameters, arguments, struct
  field initialisers, array elements, enum variants, and the arms of a match expression, except an arm whose value is
  a block). The commas between the arms of a match statement are left as written. Type parameters, generic arguments,
  attribute arguments, closure parameters, function-type parameters and interface method parameters are never broken.

```ceesafe
let bodies = [
    Body { x: 0.0, y: 0.0, z: 0.0, vx: 0.0, vy: 0.0, vz: 0.0, mass: SOLAR_MASS },
    Body {
        x: 4.84143144246472090e+00, y: -1.16032004402742839e+00, z: -1.03622044471123109e-01,
        vx: 1.66007664274403694e-03 * DAYS_PER_YEAR, vy: 7.69901118419740425e-03 * DAYS_PER_YEAR,
    },
];
```

### 3.4 Breaking a line that is too long

When a line does not fit, the formatter breaks one construct that starts on it and lays the file out again, until
the line fits or nothing on it is left to break. It takes the first kind in this order, and of that kind the
outermost:
1. a function's expression body (`=> e`): the expression moves to the next line, indented one level;
2. a list or a one-line block or body (§3.3);
3. the value of a match arm or a closure (`=> e`), moved to the next line;
4. an initialiser or an assigned value (`= e`), moved to the next line;
5. a binary expression: after its operator, with the right operand indented one level;
6. a chain of member calls: before a `.`, indented one level.

The author's breaks of the same kinds (§3.1) are laid out by the same rules.

## 4. Spaces

- One space around binary operators (`a + b`, `x == y`, `a && b`), around `=` and the compound assignments, around
  `->` and `=>`, after `,`, `;` (inside a one-line form) and `:` (`x: i32`, `x: 1`).
- No space around `..` (`0..n`, `a[i..j]`, `a[..]`), `.`, inside parentheses, brackets and generic arguments
  (`f(a)`, `[1, 2]`, `Vector<i32>`), before `(` in a call or declaration, after a unary operator (`-x`, `!b`,
  `*p`), before `?` and `!` (`f()?`, `r!`), or before `,` and `;`.
- Keywords: one space after `if`, `while`, `for`, `match`, `return` (with a value), `fn` (in a declaration), `let`,
  `var`, `borrow`, `mut`, `move`, `as` and the visibility keywords; `if (c) {`, `} else {`, `} else if (d) {`.
- One space before an opening `{` that follows code on its line.
- Attributes are followed by one space or a line break, as written: `@context(atomic) fn f()`.

## 5. Comments

- Comments keep their text and order. A comment on a line of its own is indented like the code that follows it (or,
  at the end of a block, like the code before the `}`).
- A comment after code on the same line (a trailing comment) follows it after one space. Trailing comments on
  consecutive lines are aligned: each starts one space after the longest code among those lines. A line without a
  trailing comment, or a blank line, ends the group.
- Doc comments (`///`) stay directly before their item; a blank line between a doc comment and its item is removed,
  since it would detach the comment (grammar.md §1.2).
- A block comment (`/* … */`) inside a line keeps one space on each side.

## 6. Guarantees and their tests

- **Same tree:** for every file of the repository that parses (the corpus included), the formatted text parses
  without error to an AST equal to the original's, spans aside.
- **Comments:** the comments of the formatted text are those of the original, in order.
- **Same tokens:** apart from commas, the tokens are the original's; a raw literal is compared by its value, which its
  re-indentation keeps.
- **Fixed point:** formatting the formatted text changes nothing. `tools/fmt_snapshots.py` checks the reviewed
  snapshots in `tests/formatting/` (`NAME.cee` formats to `NAME.formatted.cee`, which formats to itself).
- **The repository's own code** (`lib/`, `programs/`, `benchmarks/`, `integrations/`, the README's examples) is kept
  formatted: `ceesafec fmt --check` runs over it in the test suite. The test corpus is not reformatted, since its
  annotations name lines and some files test unusual layout; it is the formatter's test input.
