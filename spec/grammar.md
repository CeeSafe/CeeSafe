# CeeSafe Grammar — M1 subset

Status: normative for Milestone 1 (increment 1a implements the lexer and parser for exactly
this grammar). Later milestones extend it. Any syntax not listed here is rejected
by the parser, even if `language-spec.md` shows it for a later milestone.

Notation: EBNF. `'x'` is a literal token, `X?` optional, `X*` zero or more, `X+` one or more.

## 1. Lexical grammar

### 1.1 Source text
- UTF-8. A leading BOM is ignored. Line terminators: `\n`, `\r\n`, `\r`.
- Columns in diagnostics are 1-based and count UTF-16 code units. Lines are 1-based.
- Non-ASCII characters may appear only inside comments, string literals and char
  literals.

### 1.2 Whitespace and comments
- Whitespace: space, tab, line terminators, form feed.
- `// ...` to end of line. `/* ... */` block comments do **not** nest. An unterminated block
  comment is error N0003.
- Comments and whitespace are discarded by the parser, except doc comments (M8a). The lexer records every comment's span
  for the formatter (M10 T1, formatting.md).
- **Doc comments** (M8a): a line comment that starts with exactly `///` (not `////`). A run of doc
  comment lines, with only whitespace between them, is attached to the declaration whose first token
  follows the run directly. Its text is each line after `///` with leading spaces removed. Doc
  comments elsewhere are ordinary comments. Used by the `Safety:` rule (type-system.md §27.4).

### 1.3 Identifiers and keywords
```
Identifier = IdentStart IdentContinue*      (not a keyword)
IdentStart = 'A'..'Z' | 'a'..'z' | '_'
IdentContinue = IdentStart | '0'..'9'
```
A lone `_` is the token `Underscore` (reserved; no meaning in M1).

Keywords (all reserved, whether or not M1 uses them):
```
as borrow break const continue defer drop else enum extern false fn for if impl
import in interface internal let loop match move mut namespace null own private
public return Self static struct this true type unsafe using var where while
class async await yield macro
```
Primitive type names (`i32`, `u64`, `bool`, `void`, …) are **identifiers**,
resolved by name resolution, not keywords.

### 1.4 Integer literals
```
IntLiteral = (Decimal | '0x' HexDigits | '0b' BinDigits | '0o' OctDigits) IntSuffix?
IntSuffix  = 'i8'|'i16'|'i32'|'i64'|'isize'|'u8'|'u16'|'u32'|'u64'|'usize'
```
- `_` separators are allowed only *between* digits (`1_000`, `0xFF_FF`). A separator
  at the start or end of the digit sequence, or doubled, is error N0010.
- The value must fit in 64 bits (unsigned), otherwise N0004. Whether it fits the
  *type* is checked later (type checking).
- A digit invalid for the radix is N0008. A missing digit after a prefix is N0008. An unknown suffix is N0009.
- Decimal literals have no leading-zero restriction, and there is no octal-by-leading-zero:
  `010` is ten.

### 1.5 Float literals (typed since M11 G7: type-system.md §35.2)
```
FloatLiteral = Decimal '.' Decimal Exponent? FloatSuffix? | Decimal Exponent FloatSuffix?
Exponent = ('e'|'E') ('+'|'-')? Decimal
FloatSuffix = 'f32' | 'f64'
```
`1.` and `.5` are not float literals. `1..2` lexes as `1` `..` `2`.

### 1.6 String and char literals
- `"..."` on one line. Escapes: `\n \r \t \\ \" \' \0 \{ \} \u{H..H}` (1–6 hex digits,
  a Unicode scalar value). `\{` and `\}` are the braces themselves; `\{` marks braces that are meant as text, so
  that N0400 does not warn (ADR 0073 §2.4). Other escapes are N0006. A newline or EOF before the closing quote is N0005.
- `"""` begins a raw literal, which may span lines (ADR 0074 §2.6):

  ```ceesafe
  let query = """
      SELECT id, name
      FROM users
      """;
  ```

  - The opening `"""` ends its line: anything but spaces and tabs after it is N0011.
  - The content is the lines that follow, up to the first line holding only spaces or tabs and then `"""`. That
    closing line's `"""` ends the literal; tokens may follow it on the line. A content line may hold `"""` among
    other text.
  - The closing line's indentation is removed from every content line. A content line that is not blank and does not
    start with that indentation is N0011. A blank line (only spaces and tabs) is empty.
  - Content lines are joined by `\n`, also in a CRLF file. There is no final newline: the value above is
    `SELECT id, name\nFROM users`.
  - There are no escapes: `\` is a backslash, and `{` is a brace (N0400 does not apply).
  - EOF before the closing line is N0005.
  - The token is a string literal, the same as `"..."` after the lexer.
- `'c'`: exactly one Unicode scalar value or one escape. Otherwise N0007. Its value is the scalar value, as an
  integer literal (`type-system.md` §4, M11); it may also be a pattern.

### 1.7 Punctuation and operators
```
( ) { } [ ] , ; : . .. @ ? ?? -> =>
+ - * / % ! & | ^ ~ && || << 
= == != < <= > >=
+= -= *= /= %= &= |= ^= <<=
```
The lexer **never** produces `>>` or `>>=`. It emits single `>` (and `>=`) tokens.
The parser forms the shift operator `>>` from two *adjacent* `>` tokens, and `>>=` from `>`
immediately followed by `>=`. That keeps nested generic arguments
(`A<B<C>>`) unambiguous in later milestones.

`??` is one token (ADR 0072): the fallback operator in an expression (§2.1), and two `?` in a type (`T??`, an
optional of an optional). The expression `x??`, `?` applied twice, is written `(x?)?`: a `??` that no operand follows
is N0100 with that hint.

Any other character is N0001 (unexpected character).

## 2. Syntactic grammar (M1)

```
CompilationUnit = NamespaceDecl? ImportDecl* Item* EOF
NamespaceDecl   = 'namespace' QualifiedName ';'     (* retired by ADR 0013: accepted, no effect *)
ImportDecl      = 'import' Identifier ('.' Identifier)? ';'   (* M10 T5: a dependency, or one of its items; type-system.md §2 *)
QualifiedName   = Identifier ('.' Identifier)*

Item       = Attribute* Visibility? (FunctionDecl | ConstDecl | StaticDecl | StructDecl | EnumDecl | ExternBlock)
Attribute  = '@' Identifier ('(' AttributeArg (',' AttributeArg)* ')')?   (* M8b; type-system.md §28.1 *)
AttributeArg = Identifier | StringLiteral | IntLiteral
ExternBlock = 'extern' StringLiteral '{' (ExternFn | ExternStruct | ExternUnion | ExternEnum | ExternConst | ExternStatic)* '}'  (* M8b; §28.2 *)
ExternStruct = Attribute* Visibility? 'struct' Identifier ('{' (ExternField | AnonymousUnion)* '}' | ';')   (* C6b; type-system.md §31 *)
ExternUnion = Attribute* Visibility? 'union' Identifier ('{' ExternField+ '}' | ';')   (* ADR 0041; type-system.md §31.1 *)
ExternEnum = Attribute* Visibility? 'enum' Identifier ':' Type ';'   (* ADR 0079; type-system.md §31.6 *)
ExternConst = Attribute* Visibility? 'const' Identifier ':' Type ('=' Expr)? ';'   (* ADR 0080; type-system.md §28.2 *)
ExternStatic = Attribute* Visibility? 'static' 'mut'? Identifier ':' Type ';'   (* ADR 0080; type-system.md §39.6 *)
AnonymousUnion = 'union' '{' ExternField+ '}'                               (* ADR 0041: a C11 anonymous union member *)
ExternField = Attribute* FieldDecl                                           (* C9: @cname; type-system.md §31.1 *)
ExternFn   = Attribute* Visibility? 'fn' Identifier '(' (Param (',' Param)* (',' '...')? ','?)? ')' ('->' Type)? ';'   (* `...`: ADR 0041 *)
TypeParams = '<' TypeParam (',' TypeParam)* '>'               (* M6a; type-system.md §20 *)
InterfaceDecl = 'interface' Identifier TypeParams? '{' MethodSig* '}'   (* M6b; §21 *)
MethodSig  = Attribute* 'fn' Identifier '(' Receiver (',' Param)* ')' ('->' Type)? ';'   (* attributes: M13 G2, `@effects`; §37.3 *)
ImplDecl   = 'impl' Type 'for' Type '{' (Attribute* Visibility? FunctionDecl)* '}'   (* M6b; §21; attributes C7a *)
           | 'impl' Type '{' (Attribute* Visibility? FunctionDecl | Visibility? ConstDecl)* '}'  (* inherent: ADR 0064 §1; ConstDecl ADR 0074 §2.7 *)
           | 'impl' TypeParams? Type '{' (Attribute* Visibility? FunctionDecl)* '}'   (* inherent: ADR 0064 §1; §21.1 *)
MemberCall = Expr '.' Identifier ('<' Type (',' Type)* '>')? '(' Args? ')'   (* type arguments: ADR 0064 §1 *)
TypeParam  = 'region' Identifier                               (* M6x: a region, before any type parameter; §25.1 *)
           | Identifier (':' Type ('+' Type)*)?                (* bounds: M6b *)
(* `region` is contextual: only `region` followed by a name declares a region. In a type, a region
   argument is a region name or `static`, written first in the argument list (`Token<src>`,
   `StringView<r>`, `Slice<r, T>`) or in `borrow(r) T`; type-system.md §25.2. *)
EnumDecl   = 'enum' Identifier TypeParams? '{' (Variant (',' Variant)* ','?)? '}'        (* M2d *)
Variant    = ('@' 'from')? Identifier ('(' Type (',' Type)* ','? ')')?   (* `@from`: ADR 0075 §2.3 *)
Visibility = 'public' | 'internal' | 'private'

FunctionDecl = 'unsafe'? 'async'? 'fn' Identifier TypeParams? '(' ParamList? ')' ('->' Type)? FunctionBody
                                            (* 'unsafe': M8a; 'async': L1, type-system.md §40, ADR 0031 *)
ParamList    = (Receiver | Param) (',' Param)* ','?
Receiver     = ('borrow' 'mut'?)? 'this'   (* M2a: by value; M2b: reference receivers *)
Param        = Identifier ':' Type

StructDecl   = 'struct' Identifier TypeParams? '{' Member* '}'          (* M2a; TypeParams M6a *)
Member       = Visibility? FieldDecl | Attribute* Visibility? FunctionDecl | DropBlock   (* method attributes: C7a, type-system.md §32 *)
             | Visibility? ConstDecl                                (* ADR 0074 §2.7: an associated constant, type-system.md §10 *)
DropBlock    = 'drop' Block                (* M2c *)
FieldDecl    = Identifier ':' Type ';'
FunctionBody = Block | '=>' Expr ';'

ConstDecl = 'const' Identifier ':' Type '=' Expr ';'
StaticDecl = 'static' 'mut'? Identifier ':' Type '=' Expr ';'   (* M14 K1; type-system.md §39 *)

Type = QualifiedName
     | 'borrow' 'mut'? Sources? Type          (* M2b references; Sources only in return types *)
     | '[' Type ';' IntLiteral ']'             (* M2d fixed array *)
     | ('Slice' | 'SliceMut') '<' Type '>'     (* M2d slices *)
     | 'Result' '<' Type ',' Type '>'          (* M3a; type-system.md §17 *)
     | Type '?'                                (* M6c: optional; `borrow T?` is an optional reference *)
     | '(' Type ')'                            (* M11 (G6): `borrow (T?)` is a reference to an optional *)
     | Attribute* 'fn' '(' (Type (',' Type)*)? ')' ('->' Type)?   (* M11 (G8a): a function value, type-system.md §36.1;
                                               G8b′: `@effects(…)` before `fn`, §36.3 *)
     | Identifier '<' Type (',' Type)* '>'     (* M6a: an instantiation of a generic struct or enum; also Box, Pool, … *)
     | '*' 'mut'? Type                         (* M8a: raw pointer; type-system.md §27 *)
     | 'dyn' Type                              (* M13 G2: an interface instantiation; only behind `borrow` or in
                                                  `Box<…>`; type-system.md §37. `dyn` is a keyword only here *)
     | 'dyn' 'mut'? Attribute* 'fn' '(' (Type (',' Type)*)? ')' ('->' Type)?   (* M13 G3: an owning closure's type, §36.5 *)
Sources = '(' Identifier (',' Identifier)* ')'   (* only if a type follows the ')'; else '(' Type ')' *)

Block = '{' Statement* '}'
Statement = Block
          | LocalDecl
          | IfStmt
          | 'while' '(' Expr ')' Block
          | 'loop' Block
          | 'break' ';'
          | 'continue' ';'
          | 'return' Expr? ';'
          | 'using' Identifier (':' Type)? '=' Expr ';'      (* M2c *)
          | 'defer' Statement                               (* M2c *)
          | MatchStmt                                       (* M2d; type-system.md §15.2 *)
          | 'for' '(' Identifier 'in' Expr ('..' Expr)? ')' Block   (* M2d; type-system.md §16.4 *)
          | 'unsafe' Block                                  (* M8a; type-system.md §27.4 *)
          | Expr AssignOp Expr ';'        (* assignment: a statement, not an expression *)
          | Expr ';'
LocalDecl = ('let' | 'var') Identifier (':' Type)? ('=' Expr)? ';'
          | 'let' '_' '=' Expr ';'          (* ADR 0072: evaluates Expr and destroys its value, as `Expr;` *)
IfStmt    = 'if' '(' Expr ')' Block ('else' (IfStmt | Block))?
AssignOp  = '=' | '+=' | '-=' | '*=' | '/=' | '%=' | '&=' | '|=' | '^=' | '<<=' | '>>='
```

Notes:
- Braces are **mandatory** after `if`, `else`, `while` and `loop`, so there is no dangling-else ambiguity.
- Assignment is a statement, so `if (x = 1)` is a syntax error. The assignment target
  must be a name or member access (N0104 otherwise). Whether it is *assignable*
  (a `var`) is checked later.
- A `LocalDecl` without an initialiser must have a type annotation (enforced by the
  checker, not the parser).

### 2.1 Expressions and precedence

From lowest to highest binding. All binary levels are left-associative except
comparisons, which are **non-associative**: `a < b < c` is error N0103, and `??`, which is right-associative.

| Level | Operators | Notes |
|---|---|---|
| 0 | `??` | right-assoc, short-circuit (ADR 0072; type-system.md §22.1). An unparenthesised `\|\|` or `&&` operand beside it is N0100 |
| 1 | `\|\|` | short-circuit |
| 2 | `&&` | short-circuit |
| 3 | `== != < <= > >=` | non-associative |
| 4 | `\|` | |
| 5 | `^` | |
| 6 | `&` | |
| 7 | `<< >>` | |
| 8 | `+ -` | |
| 9 | `* / %` | |
| 10 | `as` | `Expr 'as' Type`, left-assoc (`x as u32 as u64`). A primitive type name (`u8` … `u64`, `i8` … `i64`, `usize`, `isize`, `f32`, `f64`, `bool`, `char`) takes no type arguments, so `a as u64 < b` is `(a as u64) < b` (ADR 0075 §2.13) |
| 11 | unary `- !`, `*` (ADR 0075 §2.14: reading through a reference), `borrow`, `borrow mut`, `move`; `await` (L1) | prefix, right-assoc (`- -x`); `await`'s operand: below |
| 12 | postfix: call `(...)`, member `.name` | |

The ordering puts bitwise operators **above** comparisons (as in Rust and Swift), so
`x & 1 == 0` parses as `(x & 1) == 0`. This avoids a well-known C/C# pitfall.

```
Primary = IntLiteral | FloatLiteral | StringLiteral | CharLiteral | 'true' | 'false'
        | Identifier GenericArgs? | '(' Expr ')' | 'this'
        | 'unsafe' '{' Expr '}'                              (* M8a: an unsafe expression *)
        | IfExpr | MatchExpr                                 (* M11 (G5): type-system.md §15.3 *)
        | '(' (ClosureParam (',' ClosureParam)*)? ')' '=>' (Block | Expr)   (* M11 (G8b): a closure, §36.2 *)
        | 'move' '(' (ClosureParam (',' ClosureParam)*)? ')' '=>' (Block | Expr)   (* M13 G3: a move literal, §36.5 *)
        | 'async' 'move'? '(' (ClosureParam (',' ClosureParam)*)? ')' '=>' (Block | Expr)
                                                     (* L1: an async closure, §40.3; `move`: a child that owns what it
                                                        captures, §40.5, ADR 0033 *)
        | Identifier GenericArgs? '{' FieldInits? '}'       (* struct literal, M2a; GenericArgs M6a *)
        | '[' Expr (',' Expr)* ','? ']' | '[' Expr ';' IntLiteral ']'   (* array literals, M2d *)
        | '[' ']'                                    (* the empty slice, ADR 0075 §2.9 *)
ClosureParam = Identifier (':' Type)?             (* a '(' … ')' followed by '=>' is a closure *)
IfExpr    = 'if' '(' Expr ')' Branch 'else' (IfExpr | Branch)
MatchExpr = 'match' '(' Expr ')' '{' (ValueArm (',' ValueArm)* ','?)? '}'
ValueArm  = Pattern '=>' Branch
Branch    = Expr | Block | ValueBlock                (* a Block branch must not complete *)
ValueBlock = '{' Statement* Expr '}'                 (* its last Expr has no ';' (ADR 0073 §2.3) *)
FieldInits = FieldInit (',' FieldInit)* ','?
FieldInit  = Identifier ':' Expr
GenericArgs = '<' Type (',' Type)* '>'      (* only when followed by '(', '.' or '{' — see below *)
Postfix = Primary ( '(' ArgList? ')' | '.' Identifier | '[' IndexOrRange ']'
                  | '?' | '!' )*                     (* M3a: propagate / unwrap a Result *)
IndexOrRange = Expr | Expr? '..' Expr?               (* M2d: element or slice *)
ArgList = Arg (',' Arg)* ','?
Arg     = 'mut' Expr | Expr                (* `mut x` for a `borrow mut` parameter, M2b *)
Await   = 'await' AwaitOperand ( '?' | '!' ) PostfixRest*         (* L1; type-system.md §40.2, ADR 0031 §3.1 *)
        | 'await' AwaitOperand
AwaitOperand = Primary ( '(' ArgList? ')' | '.' Identifier | '[' IndexOrRange ']' )*
PostfixRest  = '(' ArgList? ')' | '.' Identifier | '[' IndexOrRange ']' | '?' | '!'
```

**`await`** (L1, ADR 0031 §3.1): its operand is a primary followed by calls, member accesses and indexes, and the first
`?` or `!` ends it. That suffix, and every postfix after it, applies to the awaited value: `await f(x)?` is
`(await f(x))?`, and `await f(x)?.len` is `((await f(x))?).len`. A task is never a `Result`, so a `?` that the
operand would take could never type.

**Generic arguments in expressions** (`truncate<i64>(x)`, and since M6a `id<i32>(x)`,
`Pair<u8>.make()` and `Pair<u8> { … }`): after an identifier, `<` starts generic arguments if the
tokens up to the matching `>` can only form types (identifiers, `.`, `,`, `;`, `*` (M8a), `?` (M9a: optional
types, as in `Vector<Box<Pair>?>.create()`), integer literals, `borrow`, `mut`, and balanced `<…>` and `[…]`) and the `>` is followed by `(`, `.` or `{`, or (since
C7d, for a generic function's instance as a value, `release<T>`, type-system §34.1) by `)`, `,`, `;`
or `]`, which cannot follow a comparison's `>` (it needs an operand). Otherwise `<` is the less-than
operator. Write `(f < T) > (x)` to mean the comparison.

**Struct literals** (M2a): in expression position, an identifier *immediately followed by `{`* is
a struct literal. This is unambiguous in CeeSafe because every `{` that opens a block follows a
keyword (`else`, `loop`), a `)` (conditions are parenthesised) or a type (`-> T {`), never an
expression.

A `Receiver` (`this`) is only meaningful as the first parameter of a function declared inside a
`struct`. The parser accepts it as any first parameter; the checker rejects it elsewhere (N0232).

### 2.2 Nesting limit (M15 S3)

Nesting is limited to **1,000 levels** per file. Each of these is one level:
- a nested block or statement;
- a type (each `Box<…>` inside another is one level);
- a pattern;
- a unary operand;
- each link of a left-associative chain, so `1 + 1 + 1` is two levels deep, as are `a.b.c` and `f()()`. The links
  are binary operators, `as`, calls, members, indexes, `?` and `!`.

Every later pass recurses over the tree the parser builds, so its depth is what is bounded. Past the limit the parser
reports **N0110** once, at the token where the limit was passed, and stops parsing that file. No other diagnostic in
the file follows it. Hand-written code stays far below the limit: none of the repository's 525 files comes near it.
Before M15, 100,000 levels overflowed ceesafec's stack, ending with SIGSEGV and no message (#3). ceesafec runs every command
on its own 256 MiB stack, so input within the limit compiles whatever the operating system's main-thread stack is
(1 MiB on Windows; measured in milestone-15.md S3).

## 3. Error recovery (normative for diagnostics tests)

- Expected-token errors (N0100) are reported at the current token. When the missing
  token is `;`, they are reported at the end of the previous token instead. The missing token is
  *not* consumed.
- After an error, the parser suppresses further diagnostics until it re-synchronises:
  - in a block: at the next `;` (consumed), `}` (not consumed), or statement keyword
    (`let var if while loop return break continue`). Balanced `{ }` groups are skipped
    whole. An item keyword (`fn`, visibility) ends the block, which reports the missing `}`;
  - inside a `struct` body: at the next member start (identifier followed by `:`, `fn`, or a
    visibility keyword) or the closing `}`;
  - at the top level: at the next declaration keyword (`fn const public internal private
    namespace struct class enum interface impl let var static extern import using type`)
    outside any skipped `{ }` group, or at EOF. Each misplaced declaration is therefore reported
    once (N0101).
- Every parse loop consumes at least one token per iteration or terminates, so
  parsing always terminates.
- Past the nesting limit (§2.2), N0110 ends the file's parse: no later diagnostic is reported in
  that file.
