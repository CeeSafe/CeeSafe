---
title: "Appendix B: Operators and symbols"
summary: Every operator and punctuation symbol of CeeSafe with its meaning, the types it takes, whether it can panic and the section that teaches it, one precedence table, and the familiar operators that CeeSafe does not have.
keywords: [operator, precedence, associativity, symbol, punctuation, overflow, panic, "??", "?", "!", "=>", "->", "..", "..=", "@", "_", "~", "++", "::", shift, comparison, assignment, N0102, N0103, N0207]
diagnostics: [N0102]
specification: ["grammar.md §1.7", "grammar.md §2.1", "type-system.md §6", "type-system.md §35.3"]
---

# Appendix B: Operators and symbols

This appendix lists every token of CeeSafe that is not a word or a literal: the operators of expressions, the
assignment operators, and the punctuation. For each it gives the meaning, the types it takes, and whether it can
stop the program with a panic. One table gives the precedence of the operators. The last section names the
operators of other languages that CeeSafe does not have, and what to write instead.

The tables are the compiler's own lists for release %ceesafec-version%: the tokens are those of `tokenText` in
`src/syntax/token.cee`, and the precedence levels are those of the parser and of
[the grammar](https://ceesafe.github.io/CeeSafe/reference/grammar), §2.1. The words of the language are in
[Appendix A](./appendix-a-keywords).

Two rules hold for every binary operator. Both operands have the **same type**: nothing is converted or widened
for you, and `i32 + i64` is `N0207`. And an operator works only on the built-in types the table names: a struct
of yours gets `==` from `@derive(Equal)` and no other operator.

## Operators

"Integer" means any of `i8` to `i64`, `u8` to `u64`, `isize` and `usize`. "Float" means `f32` or `f64`. A
character literal is an integer. The checks that panic are in every build, optimised or not.

| Operator | Meaning | Operands | Result | Can it panic? | Taught in |
|---|---|---|---|---|---|
| `+` | Addition. In the value of a `const` or a `static` only, it also joins two constant pieces of text: `const NAME: StringView = PREFIX + "x";`. | two integers or two floats | the operands' type | Integers: yes, `integer overflow`. Floats: no, the result is infinity or NaN. | [3.2](./ch03-02-data-types), [20.4](./ch20-04-arithmetic) |
| `-` between operands | Subtraction. | two integers or two floats | the operands' type | Integers: yes, `integer overflow`. Floats: no. | [3.2](./ch03-02-data-types), [20.4](./ch20-04-arithmetic) |
| `*` between operands | Multiplication. | two integers or two floats | the operands' type | Integers: yes, `integer overflow`. Floats: no. | [3.2](./ch03-02-data-types), [20.4](./ch20-04-arithmetic) |
| `/` | Division. On integers it rounds toward zero. | two integers or two floats | the operands' type | Integers: yes, `division by zero`, and `integer overflow` for the least value divided by -1. Floats: no, `x / 0.0` is infinity or NaN. | [3.2](./ch03-02-data-types), [20.4](./ch20-04-arithmetic) |
| `%` | Remainder, with the sign of the left operand. | two integers; not floats (`N0207`) | the operands' type | Yes: `division by zero`. | [20.4](./ch20-04-arithmetic) |
| `-` before an operand | Negation. | a signed integer or a float; not an unsigned integer (`N0207`) | the operand's type | Integers: yes, `integer overflow` for the least value. Floats: no. | [20.4](./ch20-04-arithmetic) |
| `!` before an operand | Logical not. | `bool`; not an integer | `bool` | No. | [3.2](./ch03-02-data-types) |
| `*` before an operand | Reads the Copy value that a borrow refers to: `*r`. `*r = v;` assigns through the borrow. It is seldom needed, because a borrow is read through wherever its value is expected. | a `borrow T` or `borrow mut T` | `T` | No. | not in the chapters; [the specification](https://ceesafe.github.io/CeeSafe/reference/type-system), §13.3 |
| `&` | Bitwise and. There is no address-of `&x`: a borrow is written `borrow x`. | two unsigned integers; not signed ones, not `bool` (`N0207`) | the operands' type | No. | [20.4](./ch20-04-arithmetic) |
| `\|` between operands | Bitwise or. | two unsigned integers | the operands' type | No. | [20.4](./ch20-04-arithmetic) |
| `^` | Bitwise exclusive or. `x ^ 0xFF`, with as many `F`s as the type is wide, is the complement, since there is no `~`. | two unsigned integers | the operands' type | No. | [20.4](./ch20-04-arithmetic) |
| `<<` | Shift left. Bits shifted out are dropped. | left: an unsigned integer; right: any integer | the left operand's type | Yes: `shift amount out of range` when the amount is negative or not less than the width in bits. | [20.4](./ch20-04-arithmetic) |
| `>>` | Shift right, filling with zeros. The two `>` must touch. | left: an unsigned integer; right: any integer | the left operand's type | Yes: `shift amount out of range`, as for `<<`. | [20.4](./ch20-04-arithmetic) |
| `==` `!=` | Equal, not equal. | two values of one type: integers, floats, `bool`, an enum without payloads, `StringView`, `String`, slices, arrays, raw pointers, or a type with `@derive(Equal)`; also an optional and `null` | `bool` | No. | [3.2](./ch03-02-data-types), [8.2](./ch08-02-strings), [20.5](./ch20-05-derives-and-macros) |
| `<` `<=` `>` `>=` | Less, less or equal, greater, greater or equal. They cannot be chained: `a < b < c` is `N0103`. | two integers or two floats; not `bool`, text or enums (`N0207`) | `bool` | No. | [3.2](./ch03-02-data-types), [3.5](./ch03-05-control-flow) |
| `&&` | Logical and. The right operand is evaluated only if the left is `true`. | two `bool` | `bool` | No. | [3.2](./ch03-02-data-types) |
| `\|\|` | Logical or. The right operand is evaluated only if the left is `false`. | two `bool` | `bool` | No. | [3.2](./ch03-02-data-types) |
| `??` between operands | Fallback: the left operand's value, or the right operand if the left is `null`. The right operand is evaluated only then. Beside `&&` or `\|\|` it needs parentheses (`N0100`). | left: an optional `T?`; right: a `T` or a `T?` | `T`, or `T?` when the right operand is a `T?` | No. | [6.3](./ch06-03-optionals) |
| `?` after an operand | Propagation: the value inside an `Ok` or a `Some`; for an `Err` or `null`, the function returns it at once. | a `Result<T, E>` or a `T?`, in a function that returns one | `T` | No: it returns instead. | [9.2](./ch09-02-result), [6.3](./ch06-03-optionals) |
| `!` after an operand | Unwrap: the value inside an `Ok` or a `Some`. | a `Result<T, E>` or a `T?` | `T` | Yes: `unwrapped an Err value` on an `Err`, `unwrapped null` on `null`. | [9.2](./ch09-02-result), [6.3](./ch06-03-optionals) |
| `.` | Member access: a field, a method, a variant of an enum, a constant or static function of a type, an item of an imported package. | a value, a type or a package, then a name | the member | No. | [5.1](./ch05-01-defining-structs), [5.3](./ch05-03-methods), [6.1](./ch06-01-defining-an-enum), [7.3](./ch07-03-imports) |
| `(` `)` after an operand | A call: `f(x, mut y)`. | a function, a method or a closure, and its arguments | the function's result | Only if the function does. | [3.3](./ch03-03-functions) |
| `[` `]` after an operand | An element, `a[i]`, or a slice, `a[lo..hi]`, `a[..hi]`, `a[lo..]`, `a[..]`. An index is a `usize`. | an array, a slice, a vector or a view | the element, or a `Slice` | Yes: `index out of bounds`, `slice range out of bounds`. | [4.3](./ch04-03-slices-and-views), [9.1](./ch09-01-panics) |

Arithmetic made only of literals is done by the compiler, and an operation in it that would panic is the compile
error `N0217` instead. Where overflow should wrap, saturate or be tested, every integer type has methods:
`wrappingAdd`, `checkedAdd`, `saturatingAdd` and their like, which [20.4](./ch20-04-arithmetic) teaches.

Five operators are words. They are reserved words or contextual words, and [Appendix A](./appendix-a-keywords)
lists them as such.

| Operator | Meaning | Can it panic? | Taught in |
|---|---|---|---|
| `as` | `e as T` converts to a type that holds every value of `e`'s type. Any other conversion is a compile error, `N0208`. | No. | [3.2](./ch03-02-data-types), [20.4](./ch20-04-arithmetic) |
| `is` | `e is PATTERN` is `true` if the value fits the pattern, and binds the pattern's names. | No. | [6.3](./ch06-03-optionals), [19.1](./ch19-01-where-patterns-go) |
| `borrow`, `borrow mut` | `borrow x` lends `x` for reading, `borrow mut x` for changing. | No. | [4.2](./ch04-02-borrowing) |
| `move` | `move x` moves the value out of `x`. | No. | [15.2](./ch15-02-destruction) |
| `await` | `await task` runs a task to its result. | No. | [17.1](./ch17-01-tasks-and-await) |

## Precedence

Level 12 binds tightest. An operator of a higher level takes its operands before one of a lower level, so
`a + b * c` is `a + (b * c)`. The level numbers are the parser's.

| Level | Operators | Associativity |
|---|---|---|
| 12 | postfix: call `f(x)`, member `x.name`, index and slice `a[i]`, `?`, `!` | left to right |
| 11 | prefix: `-`, `!`, `*`, `borrow`, `borrow mut`, `move`, `await` | right to left: `- -x` is `-(-x)` |
| 10 | `as` | left to right: `x as u32 as u64` |
| 9 | `*` `/` `%` | left to right |
| 8 | `+` `-` | left to right |
| 7 | `<<` `>>` | left to right |
| 6 | `&` | left to right |
| 5 | `^` | left to right |
| 4 | `\|` | left to right |
| 3 | `==` `!=` `<` `<=` `>` `>=` `is` | none: two of them in a row are `N0103` |
| 2 | `&&` | left to right |
| 1 | `\|\|` | left to right |
| 0 | `??` | right to left: `a ?? b ?? c` is `a ?? (b ?? c)` |

- **The bit operators bind tighter than the comparisons,** unlike in C and C#: `x & 1 == 0` is `(x & 1) == 0`.
- **Assignment is not in the table,** because it is a statement and not an expression.
- **`await` ends at the first `?` or `!`:** `await f(x)?` is `(await f(x))?`.
- **After `is`, the pattern reads as far as it can,** so a `|` there is part of the pattern: write
  `(c is 'a') | flag`.
- **A prefix operator applies after the postfix ones:** `-x.abs()` is `-(x.abs())`, and `!ready()` negates the
  call's result.

This program shows three of the rules:

```ceesafe
fn main() {
    let flags: u8 = 0b0000_0110;
    Console.writeLine(flags & 1 == 0);
    Console.writeLine(flags ^ 0xFF);
    let limit: i32? = null;
    Console.writeLine(limit ?? 10 + 1);
}
```

```text
true
249
11
```

## Assignment operators

An assignment is a statement: it has no value, so `if (x = 1)` and `a = b = 0` do not compile. Its left side is
a `var`, a field, an element, `this` in a `borrow mut this` method, or a `borrow mut`, which is written through.

| Operator | Meaning | Can it panic? | Taught in |
|---|---|---|---|
| `=` | Assigns the value on the right. It is also the `=` of a declaration: `let x = 1;`, `const`, `static`, `using`. | No. | [3.1](./ch03-01-variables) |
| `+=` | `x += y;` is `x = x + y;`. | As `+`. | [3.1](./ch03-01-variables), [20.4](./ch20-04-arithmetic) |
| `-=` | `x -= y;` is `x = x - y;`. | As `-`. | [20.4](./ch20-04-arithmetic) |
| `*=` | `x *= y;` is `x = x * y;`. | As `*`. | [20.4](./ch20-04-arithmetic) |
| `/=` | `x /= y;` is `x = x / y;`. | As `/`. | [20.4](./ch20-04-arithmetic) |
| `%=` | `x %= y;` is `x = x % y;`. Integers only. | As `%`. | [20.4](./ch20-04-arithmetic) |
| `&=` | `x &= y;` is `x = x & y;`. Unsigned integers only. | No. | [20.4](./ch20-04-arithmetic) |
| `\|=` | `x \|= y;` is `x = x \| y;`. Unsigned integers only. | No. | [20.4](./ch20-04-arithmetic) |
| `^=` | `x ^= y;` is `x = x ^ y;`. Unsigned integers only. | No. | [20.4](./ch20-04-arithmetic) |
| `<<=` | `x <<= n;` is `x = x << n;`. Unsigned integers only. | As `<<`. | [20.4](./ch20-04-arithmetic) |
| `>>=` | `x >>= n;` is `x = x >> n;`. Unsigned integers only. The `>` and the `>=` must touch. | As `>>`. | [20.4](./ch20-04-arithmetic) |

## Punctuation and other symbols

These tokens are not operators. Some of them are also operators, in the first table, when they stand between or
beside operands.

| Symbol | What it is for | Taught in |
|---|---|---|
| `(` `)` | Group an expression. Enclose the parameters of a function or closure, the condition of `if`, `while` and `match`, the head of `for`, the payload of a variant, the arguments of an attribute, and the sources of a borrowed result, `borrow(a, b) T`. | [3.3](./ch03-03-functions), [3.5](./ch03-05-control-flow) |
| `{` `}` | Enclose a block, and the body of a function, struct, enum, interface, `impl` or `match`. After a type's name they enclose a struct literal: `Point { x: 1, y: 2 }`. An `if`, `while`, `loop` or `for` statement needs its braces. | [3.3](./ch03-03-functions), [3.5](./ch03-05-control-flow), [5.1](./ch05-01-defining-structs) |
| `[` `]` | Enclose an array type, `[u8; 4]`, an array literal, `[1, 2, 3]` or `[0u8; 4]`, and a slice pattern. `[]` is the empty slice. | [3.2](./ch03-02-data-types), [19.3](./ch19-03-the-syntax-whole) |
| `,` | Separates parameters, arguments, fields of a literal, variants, elements and `match` arms. One may follow the last item. | [3.3](./ch03-03-functions) |
| `;` | Ends a statement, a field and a declaration. In `[T; n]` and `[v; n]` it separates the element from the length. A block that is a value ends with an expression without `;`. | [3.3](./ch03-03-functions), [3.2](./ch03-02-data-types) |
| `:` | Stands before a type: of a local, `let x: u8 = 1;`, of a parameter, of a field. Before a value in a struct literal, `x: 1`. Before the bounds of a type parameter, `T: Shape`. | [3.1](./ch03-01-variables), [5.1](./ch05-01-defining-structs), [10.1](./ch10-01-generic-types) |
| `..` | A half-open range, which leaves its end out. In `for (i in 0..n)`, in a slice `a[lo..hi]`, and in a pattern, `0..10`. A range is not a value: `let r = 0..4;` does not compile. | [3.5](./ch03-05-control-flow), [4.3](./ch04-03-slices-and-views), [19.3](./ch19-03-the-syntax-whole) |
| `..=` | An inclusive range, in a pattern only: `'a'..='z'`. In an expression it is `N0100`. | [19.3](./ch19-03-the-syntax-whole) |
| `@` | Begins an attribute, `@test`. In a pattern, `name @ pattern` binds the whole value that the pattern fits. | [Appendix A](./appendix-a-keywords#attributes), [19.3](./ch19-03-the-syntax-whole) |
| `?` after a type | An optional type: `T?` holds a `T` or `null`. | [6.3](./ch06-03-optionals) |
| `??` after a type | Two `?`: `T??` is an optional of an optional. | [6.3](./ch06-03-optionals) |
| `->` | Stands before the result type of a function, `fn f() -> i32`, and of a function type, `fn(i32) -> bool`. It is not member access. | [3.3](./ch03-03-functions), [13.1](./ch13-01-closures) |
| `=>` | Stands before the result of a `match` arm, before the expression that is a function's whole body, `fn two() -> i32 => 2;`, and before the body of a closure, `(x) => x + 1`. | [6.2](./ch06-02-match), [3.3](./ch03-03-functions), [13.1](./ch13-01-closures) |
| `_` | The pattern that fits any value and binds nothing. `let _ = e;` evaluates `e` and discards its value. It is not an expression and not a parameter's name. | [6.2](./ch06-02-match), [9.2](./ch09-02-result) |
| `<` `>` around types | Enclose type parameters, `struct Pair<T>`, and type arguments, `Vector<u8>`, `truncate<u8>(x)`. | [10.1](./ch10-01-generic-types) |
| `+` between bounds | Joins the bounds of a type parameter, `T: Shape + Copy`, and adds a marker to a `dyn` type, `dyn Job + Send`. | [10.1](./ch10-01-generic-types), [18.1](./ch18-01-dynamic-dispatch) |
| `*` before a type | A raw pointer type: `*T` reads, `*mut T` reads and writes. | [20.1](./ch20-01-unsafe-code) |
| `\|` between patterns | Alternatives: `1 \| 2 \| 3` fits any of the three. | [19.3](./ch19-03-the-syntax-whole) |
| `-` before a literal in a pattern | A negative number as a pattern: `-1`. | [19.3](./ch19-03-the-syntax-whole) |
| `~` | Nothing. The compiler reads it as a token, and no rule of the grammar uses it: `~x` is `N0102`. | the last section of this appendix |

## Symbols inside literals and comments

These are not tokens of their own: the compiler reads them as part of a literal or a comment.

| Symbol | What it is for | Taught in |
|---|---|---|
| `"…"` | A string literal, a `StringView`, on one line. | [3.2](./ch03-02-data-types), [8.2](./ch08-02-strings) |
| `"""` | Begins and ends a raw string literal of several lines, with no escapes. The indentation of the closing line is removed from each line. | not in the chapters; [the grammar](https://ceesafe.github.io/CeeSafe/reference/grammar), §1.6 |
| `c"…"` | A C string literal: the compiler adds the zero byte that ends it. | [20.2](./ch20-02-calling-c) |
| `'c'` | A character literal: one character, whose value is its number as an integer. | [3.2](./ch03-02-data-types) |
| `\` | Begins an escape in a string or character literal: `\n`, `\r`, `\t`, `\\`, `\"`, `\'`, `\0`, `\{`, `\}`, and `\u{…}` with one to six hexadecimal digits. Any other is `N0006`. Outside a literal `\` is `N0001`. | not in the chapters; [the grammar](https://ceesafe.github.io/CeeSafe/reference/grammar), §1.6 |
| `{` `}` in a string | The braces themselves. There is no string interpolation: `"x = {x}"` prints `{x}` and warns with `N0400`. `\{` says that the brace is meant. | [1.2](./ch01-02-hello-world), [8.2](./ch08-02-strings) |
| `//` | A comment to the end of the line. | [3.4](./ch03-04-comments) |
| `///` | A documentation comment for the declaration that follows it. | [3.4](./ch03-04-comments) |
| `/*` `*/` | A comment that may span lines. It does not nest. | [3.4](./ch03-04-comments) |
| `0x` `0b` `0o` | Begin a hexadecimal, a binary and an octal integer literal. | [3.2](./ch03-02-data-types) |
| `_` between digits | Separates digits for the reader: `1_000_000`. | [3.2](./ch03-02-data-types) |
| `.` between digits | The point of a float literal: `1.5`. `1.` and `.5` are not literals. | [3.2](./ch03-02-data-types) |
| `...` | After the last parameter of a function in an `extern "C"` block: the further arguments of a C function such as `printf`. | not in the chapters; [the specification](https://ceesafe.github.io/CeeSafe/reference/type-system), §28.2 |

## What CeeSafe does not have

Each row is something that another language has and CeeSafe %ceesafec-version% rejects, with what to write instead.

| Not in CeeSafe | Write instead | Taught in |
|---|---|---|
| `~x`, the bitwise complement | `x ^ 0xFF` for a `u8`, `x ^ 0xFFFFFFFF` for a `u32`: as many `F`s as the type is wide. | [20.4](./ch20-04-arithmetic) |
| `x++`, `x--` | `x += 1;`, `x -= 1;` | [3.1](./ch03-01-variables) |
| `+x`, a prefix plus | `x` | [3.2](./ch03-02-data-types) |
| `a ** b`, a power operator | A loop that multiplies. | [3.5](./ch03-05-control-flow) |
| `c ? a : b`, the conditional operator | `if (c) a else b`, which is an expression. | [3.5](./ch03-05-control-flow) |
| `a?.b`, optional chaining | `match`, `o is Some(v)`, or `o?` in a function that returns an optional. | [6.3](./ch06-03-optionals) |
| `a::b` | `a.b`: the `.` is the only path separator. | [7.3](./ch07-03-imports) |
| `p->field` | `p.field`: a borrow is read through with `.`. | [4.2](./ch04-02-borrowing) |
| `&x`, `&mut x`, the address of a value | `borrow x`, `borrow mut x`; as an argument, `x` and `mut x`. | [4.2](./ch04-02-borrowing) |
| `x >>> n`, an unsigned shift | `x >> n`: shifts take unsigned integers only and always fill with zeros. | [20.4](./ch20-04-arithmetic) |
| `&&=`, `\|\|=`, `??=` | `b = b && c;`, `o = o ?? v;` | [3.2](./ch03-02-data-types), [6.3](./ch06-03-optionals) |
| `a = b = 0`, `if (x = 1)`: assignment as an expression | One assignment in each statement. | [3.1](./ch03-01-variables) |
| `a < b < c`, chained comparisons | `a < b && b < c` | [3.2](./ch03-02-data-types) |
| `not`, `and`, `or` as words | `!`, `&&`, `\|\|` | [3.2](./ch03-02-data-types) |
| `0..=n` in an expression | `0..n + 1`: ranges in `for` and in slices are half-open. `..=` is for patterns. | [3.5](./ch03-05-control-flow) |
| `a + b` on text when the program runs | A `String` and its `append` method. `+` joins text only in the value of a `const` or a `static`. | [8.2](./ch08-02-strings) |
| `"x = {x}"`, `$"…"`, string interpolation | One call for each piece of text. | [8.2](./ch08-02-strings) |
| `==` on a struct of yours | `@derive(Equal)` on the struct, or compare the fields. | [20.5](./ch20-05-derives-and-macros) |
| `#`, `$`, a backtick, as tokens | Nothing: outside a literal or a comment each is `N0001`, an unexpected character. | [the grammar](https://ceesafe.github.io/CeeSafe/reference/grammar), §1.7 |

The compiler reads `~` as a token and then finds no rule for it, so the message is that an expression was
expected:

<!-- ceesafe: fail N0102 -->
```ceesafe
fn main() {
    let flags: u8 = 6;
    Console.writeLine(~flags);
}
```

```text
main.cee:3:23: error[N0102]: expected expression, found '~'
  |
3 |     Console.writeLine(~flags);
  |                       ^
```
