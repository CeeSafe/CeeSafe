---
title: "Appendix H: The grammar"
summary: The grammar of CeeSafe as the specification states it, production by production, grouped as lexical forms, items, types, statements, expressions and patterns, with the sections that teach each group.
keywords: [grammar, EBNF, production, syntax, precedence, token, literal, item, type, statement, expression, pattern]
specification: ["grammar.md", "type-system.md §15.2"]
---

# Appendix H: The grammar

This appendix lists the grammar of CeeSafe 0.42.0: what the parser of `ceesafec` accepts, as productions. Use it to
settle a question of form, such as where a comma may go or which operator binds tighter, and follow the links
beside each group to the section that teaches the form with examples.

The productions are copied from the specification, character for character, and are not rewritten here: all but
the last block are the productions of
[the grammar](https://ceesafe.github.io/CeeSafe/reference/grammar) (`spec/grammar.md`), in its order, and the
block under [Patterns](#patterns) is from
[the type system, §15.2](https://ceesafe.github.io/CeeSafe/reference/type-system) (`spec/type-system.md`). A form
that no production here derives is rejected by the parser. A form that a production derives may still be rejected
by a later check, such as type checking or the borrow rule, which the chapters teach.

## How to read this notation

The notation is EBNF. One production defines one name:

| Notation | Meaning |
|---|---|
| `Name = …` | The production that defines `Name`. A name with a capital first letter is defined by a production, or is a token of the [lexical grammar](#lexical-grammar). |
| `'x'` | The token `x`, written as it is in a source file. `'fn'` is the keyword `fn`, and `'('` is an opening parenthesis. |
| `A B` | `A`, then `B`. |
| `A \| B` | `A` or `B`. A line that begins with `\|` continues the production above it with one more alternative. |
| `X?` | `X` or nothing. |
| `X*` | `X` zero or more times. |
| `X+` | `X` one or more times. |
| `( … )` | A group, so that `?`, `*`, `+` or `\|` applies to all of it. `('->' Type)?` is an optional result type. |
| `'a'..'z'` | In the lexical grammar, any one character from `a` to `z`. |
| `(* … *)` | A comment. In the patterns block a comment begins with `--` and runs to the end of the line. |

Three things in the comments are not part of the form:

- A tag such as `M2d`, `L1`, `C7a` or `G5` names the step of the compiler's history that added the form. It has
  no meaning for a program.
- `ADR 0085 §2.3` names a decision of the specification, a page under
  [decisions](https://ceesafe.github.io/CeeSafe/reference/decisions/).
- `type-system.md §36.1`, or a bare `§21`, names the section of the type system that gives the form its rules.

`EOF` is the end of the file. `StringLiteral` and `CharLiteral` are tokens: their forms are described in prose
under [Strings and characters](#strings-and-characters).

## Lexical grammar

A source file is UTF-8 text. The lexer turns it into tokens. Whitespace and comments separate tokens and are then
dropped, except that a `///` comment is kept as documentation for the declaration that follows it.
[Comments](./ch03-04-comments) teaches both kinds of comment.

### Identifiers and keywords

@@IDENTIFIERS@@

A lone `_` is a token of its own, not an identifier. The keywords are all reserved: none can be a name, whether
or not the language uses it yet.

@@KEYWORDS@@

The names of the primitive types, such as `i32`, `u64`, `bool` and `void`, are identifiers and not keywords. Four
more words have a meaning in one place only and are identifiers everywhere else: `is` after a complete operand,
`region` before a name in a list of type parameters, `dyn` in a type, and `union` in an `extern` block.
[Variables](./ch03-01-variables) teaches names, and [Data types](./ch03-02-data-types) the primitive types.

### Numbers

@@INTEGERS@@

@@FLOATS@@

`1.` and `.5` are not float literals, and `1..2` is the three tokens `1`, `..` and `2`. A decimal literal may
begin with zeros, and `010` is ten. [Data types](./ch03-02-data-types) teaches the literals and their suffixes.

### Strings and characters

The specification describes these in prose, not in productions:

| Form | What it is |
|---|---|
| `"text"` | A string literal, on one line. Its escapes are `\n`, `\r`, `\t`, `\\`, `\"`, `\'`, `\0`, `\{`, `\}` and `\u{…}` with one to six hexadecimal digits. |
| `"""` to `"""` | A raw string literal of several lines. The opening `"""` ends its line. The content is the lines that follow, up to a line that holds only spaces or tabs and then `"""`. That closing line's indentation is removed from every line. There are no escapes. |
| `c"text"` | A C string literal, with the escapes of `"text"` except `\0`. The compiler adds the NUL byte at its end. |
| `'c'` | A character literal: one Unicode scalar value or one escape. Its value is an integer. |

[Strings and string views](./ch08-02-strings) teaches string literals, and
[Calling C and being called by it](./ch20-02-calling-c) the C string literal.

### Punctuation and operators

@@PUNCTUATION@@

The lexer never makes a `>>` or `>>=` token. The parser reads two `>` that touch as the shift operator, and a
`>` that touches a `>=` as `>>=`, so that the end of `Vector<Box<Point>>` is two closing brackets. `..=` is a token
of patterns only, and `??` is two `?` when it is read in a type. Any character that starts no token is the error
N0001.

## Items

A file is a list of items: the declarations at its top level. There is no statement outside a function.

### A file

@@UNIT@@

[Imports](./ch07-03-imports) teaches `import`. A `namespace` line is accepted and has no effect.

### Items, attributes and the declarations with type parameters

@@ITEMS@@

| Form | Taught in |
|---|---|
| `Visibility`: `public`, `internal`, `private` | [Files and public](./ch07-02-files-and-public) |
| `Attribute`: `@test`, `@derive(…)`, `@export("name")` | [How to write tests](./ch11-01-writing-tests), [Derives and macros](./ch20-05-derives-and-macros) |
| `ExternBlock` and what it declares | [Calling C and being called by it](./ch20-02-calling-c) |
| `TypeParams`, `TypeParam` with bounds | [Generic types and functions](./ch10-01-generic-types) |
| `TypeParam` that begins `region` | [Regions](./ch10-03-regions) |
| `TypeParam` that begins `const` | [More on interfaces and generics](./ch20-03-more-interfaces-and-generics) |
| `InterfaceDecl`, `MethodSig`, `ImplDecl` | [Interfaces](./ch10-02-interfaces) |
| `InterfaceConst`, `AssocTypeDecl`, `AssocTypeDef`, an interface after `:` | [More on interfaces and generics](./ch20-03-more-interfaces-and-generics) |
| `impl` with no interface | [Methods](./ch05-03-methods) |
| `EnumDecl`, `Variant` | [Defining an enum](./ch06-01-defining-an-enum) |
| `Variant` with `@from` | [Result, ? and !](./ch09-02-result) |

### Functions

@@FUNCTIONS@@

[Functions](./ch03-03-functions) teaches the declaration, and [Methods](./ch05-03-methods) the receiver `this`.
[Unsafe code](./ch20-01-unsafe-code) teaches `unsafe fn`, and [Tasks and await](./ch17-01-tasks-and-await)
`async fn`.

### Structs

@@STRUCTS@@

[Defining and making structs](./ch05-01-defining-structs) teaches fields, [Methods](./ch05-03-methods) the
functions in a struct's body, and [Destruction](./ch15-02-destruction) the `drop` block. A `FunctionBody` that
is `=>` and an expression is taught in [Functions](./ch03-03-functions).

### Constants and statics

@@CONSTANTS@@

[Variables](./ch03-01-variables) teaches `const`, and [Kernel code](./ch22-02-kernel-code) teaches `static`
and `static mut`.

## Types

@@TYPES@@

The alternative that begins `'extern'` is `extern "C" fn(A) -> R`, a pointer to a C function, which
[Calling C and being called by it](./ch20-02-calling-c) teaches.

| Form | Taught in |
|---|---|
| A name: `i32`, `Point`, `Self.Item` | [Data types](./ch03-02-data-types) |
| `borrow T`, `borrow mut T` | [Borrowing](./ch04-02-borrowing) |
| `borrow(a, b) T`, with `Sources` | [Regions](./ch10-03-regions) |
| `[T; n]` | [Data types](./ch03-02-data-types) |
| `Slice<T>`, `SliceMut<T>` | [Slices and views](./ch04-03-slices-and-views) |
| `Result<T, E>` | [Result, ? and !](./ch09-02-result) |
| `T?` | [Optionals](./ch06-03-optionals) |
| `fn(A) -> R`, `dyn fn(A) -> R`, `dyn mut fn()` | [Closures](./ch13-01-closures) |
| `Vector<T>`, with `GenericArg` | [Generic types and functions](./ch10-01-generic-types) |
| `ConstArg` | [More on interfaces and generics](./ch20-03-more-interfaces-and-generics) |
| `*T`, `*mut T` | [Unsafe code](./ch20-01-unsafe-code) |
| `dyn I` | [Dynamic dispatch](./ch18-01-dynamic-dispatch) |

## Statements

@@STATEMENTS@@

Braces are required after `if`, `else`, `while` and `loop`. An assignment is a statement and not an expression,
so `if (x = 1)` is a syntax error. `MatchStmt` is defined with the [patterns](#patterns).

| Form | Taught in |
|---|---|
| `LocalDecl`: `let`, `var`, `let _ =` | [Variables](./ch03-01-variables) |
| `let Pattern = Expr else Block;` | [Where patterns go](./ch19-01-where-patterns-go) |
| `IfStmt`, `while`, `loop`, `break`, `continue`, `for` | [Control flow](./ch03-05-control-flow) |
| `for` over a collection or an iterator | [Iteration](./ch13-02-iteration) |
| `return` | [Functions](./ch03-03-functions) |
| `MatchStmt` | [match](./ch06-02-match) |
| `using`, `defer` | [Destruction](./ch15-02-destruction) |
| `unsafe` block | [Unsafe code](./ch20-01-unsafe-code) |
| `AssignOp` | [Variables](./ch03-01-variables), [Arithmetic](./ch20-04-arithmetic) |

## Expressions

### Precedence

The binary and prefix operators are not in the productions: this table gives them, from the loosest binding to
the tightest. Every binary level associates to the left, except that the comparisons do not associate at all
(`a < b < c` is the error N0103) and `??` associates to the right.

@@PRECEDENCE@@

The bit operators bind tighter than the comparisons, so `x & 1 == 0` is `(x & 1) == 0`.
[Data types](./ch03-02-data-types) teaches the arithmetic and comparison operators,
[Arithmetic](./ch20-04-arithmetic) the bit operators, the shifts and `as`, [Optionals](./ch06-03-optionals) `??`,
and [Where patterns go](./ch19-01-where-patterns-go) `is`.

### Primary and postfix expressions

@@EXPRESSIONS@@

`Operand`, in `IsTest`, is the expression to the left of `is`, which is an operator of level 3 in the table.
After a name, `<` begins generic
arguments only when the tokens up to the matching `>` can form types or const arguments and the `>` is followed
by `(`, `.`, `{`, `)`, `,`, `;` or `]`. Otherwise it is the less-than operator. A name followed directly by `{`
is a struct literal.

| Form | Taught in |
|---|---|
| Literals, array literals `[a, b]` and `[x; n]` | [Data types](./ch03-02-data-types) |
| `IfExpr`, `MatchExpr`, `Branch`, `ValueBlock` | [Control flow](./ch03-05-control-flow), [match](./ch06-02-match) |
| Struct literal, `FieldInits` | [Defining and making structs](./ch05-01-defining-structs) |
| A closure, `move` closure, `ClosureParam` | [Closures](./ch13-01-closures) |
| `async` closure | [Scopes](./ch17-02-scopes) |
| `Await` | [Tasks and await](./ch17-01-tasks-and-await) |
| Call, `ArgList`, `mut x` as an argument | [Functions](./ch03-03-functions), [Borrowing](./ch04-02-borrowing) |
| `GenericArgs` at a call | [Generic types and functions](./ch10-01-generic-types) |
| `IndexOrRange`: `a[i]`, `a[1..4]`, `a[..]` | [Slices and views](./ch04-03-slices-and-views) |
| Postfix `?` and `!` | [Result, ? and !](./ch09-02-result), [Optionals](./ch06-03-optionals) |
| `IsTest`, `Guard` | [Where patterns go](./ch19-01-where-patterns-go) |
| `unsafe { e }` | [Unsafe code](./ch20-01-unsafe-code) |

## Patterns

The grammar names `Pattern` and `MatchStmt` and leaves their productions to the type system, §15.2. This block
is that section's.

@@PATTERNS@@

Every line of `PrimPattern` that begins with `|` is one of its alternatives. A payload that is a single identifier
is a binding, unless the name is a constant in scope.

| Form | Taught in |
|---|---|
| `MatchStmt`, `Arm`, variant patterns, `_` | [match](./ch06-02-match) |
| `Some(x)` and `null` | [Optionals](./ch06-03-optionals) |
| Where a pattern may be written: `match`, `is`, `let … else` | [Where patterns go](./ch19-01-where-patterns-go) |
| Which patterns can fail, and what a `match` must cover | [Patterns that can fail](./ch19-02-patterns-that-can-fail) |
| Every form: literals, ranges, `\|`, `name @ P`, strings, slices, `Guard` | [The syntax, whole](./ch19-03-the-syntax-whole) |

## Limits

Nesting is limited to 1,000 levels in a file: blocks, types, patterns, unary operands and each link of a chain
such as `a.b.c` or `1 + 1 + 1` count as levels. Past the limit the parser reports N0110 once and stops reading
that file.
