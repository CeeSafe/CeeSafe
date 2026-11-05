# Type System and Static Semantics — M1 subset

Status: normative for Milestone 1 (increment 1b implements it). Later milestones extend these
rules. Where `language-spec.md` describes a later feature, it does not apply until then. Error
codes are listed in `diagnostics.md`.

## 1. Types

| Type | Values | Notes |
|---|---|---|
| `i8 i16 i32 i64` | two's complement, −2^(n−1) … 2^(n−1)−1 | |
| `u8 u16 u32 u64` | 0 … 2^n−1 | |
| `isize` / `usize` | pointer width of the **target** (64 on every M1 target) | from `TargetInfo`, never the host |
| `bool` | `true`, `false` | |
| `void` | no value | function return type only |
| `never` | no value; the expression does not complete | a function's return type only (§15.3, ADR 0074 §2.2) |

Not in M1 (error N0216): floats, `char`, strings as values, user-defined types, references. Floats came in M11
(`f32`, `f64`, §35); character literals are integers (§4).
In M1 a string literal was allowed **only** as a direct argument of `Console.write`/`writeLine`;
since M6c it is a `StringView` value (§22.2).

`void` may appear only as a return type (writing `-> void` is the same as omitting the
return type). A *never* expression is accepted wherever any type is expected.

`never` (ADR 0074 §2.2) names the bottom type, and only as the return type of a function or of a struct's method: a
call of one is a call of type *never* (§9), as `Process.exit` is. Anywhere else (a parameter, a field, a local, a type
argument, a function type, an `async` function, an interface method, an `extern` function) it is N0226: "'never' can
only be a function's return type". A `never` function does not return: the end of its body must be unreachable
(N0213), and `return` in it is N0215. A `let` whose initializer has type *never* has no value to bind (N0224). In C,
the function is `_Noreturn void` (`c-backend.md`); in NIR, a call of one ends its block with `Unreachable`. `never` is
a type name, not a keyword: a value named `never` is unaffected.

## 2. Names and scopes

A **package** is the unit of compilation and of naming (ADR 0013, `tooling.md` §4): its files, its dependencies, and
`lib/core`. A build checks the package being built together with every package it depends on, directly or not.

- **Package scope.** All files of a package share one item scope. Items are referenced by simple name. Two items of
  one package with the same name are an error (N0201), even in different files. `namespace` declarations are accepted
  and have no effect (retired by ADR 0013; they will become an error).
- **Dependencies** (M10 T5). A package names its dependencies in its manifest (`tooling.md` §4). A dependency's
  **public** items are reached by qualifying them with the dependency's name:
  - in a type, `geometry.Point`, `geometry.Pair<u8>`;
  - in an expression, `geometry.area(p)`, `geometry.LIMIT`, `geometry.Shape.Circle(2)`, a struct literal
    `geometry.Point { x: 1, y: 2 }`; in a pattern, `geometry.Shape.Circle(r)`.

  A static function of a generic type of a dependency is called through an import (`Pair<u8>.make()`), since
  `geometry.Pair<u8>.make()` does not parse (grammar.md §2.1: in an expression, generic arguments follow a simple
  name only).
- **Dependency names are reserved.** In a package, nothing else may have the name of one of its dependencies: no
  item, parameter, local, loop variable or pattern binding (N0315). So `geometry.x`, with `geometry` a dependency,
  always means the dependency's item, and the compiler resolves it before type checking (`PackageNames`).
- **Imports.** `import geometry;` at the top of a file brings every public item of the dependency `geometry` into
  that file's scope; `import geometry.Point;` brings one (N0311 if it has none of that name, N0312 if it is not
  public). An import applies to its file only. The name before the `.` must be a dependency of the package (N0310).
- **Visibility between packages.** Only `public` items are visible to other packages, and of a public struct only its
  `public` fields and methods; `internal`, the default, keeps an item within its package, and `private` within its
  file (§12). Naming another package's non-public item is N0312. A public function, constant, field or struct whose
  signature names a type its dependents cannot see (a public function returning an internal struct) is N0314. An
  `impl I for T` applies wherever `I` and `T` are both visible.
- **Built-in scope** (outside the package scope, so a user item hides a built-in): `Console.write`,
  `Console.writeLine`, `Process.exit`, `truncate`, and the primitive type names.
- **`lib/core`** is in scope in every package, unqualified, and no package may redeclare one of its names (N0201,
  §29.2).
- **Function scope.** Parameters, then nested block scopes. A local is visible from the end of
  its declaration to the end of its block.
- **No shadowing of locals.** Declaring a local or parameter whose name is already a local or
  parameter in scope is an error (N0203), as in C#. Shadowing an *item* with a local is allowed.
- **Lookup order** for a simple name: locals (innermost first), the package's items, the file's imports, `lib/core`,
  built-ins. Two imports that both provide a name are an error where the name is used (N0313), not where it is
  imported. For `a.b`: if `a` is a local or an item of the package or of `lib/core`, `.b` is a member as before;
  otherwise, `a` being a dependency (and so not a local or item, N0315), `a.b` is that dependency's item `b`.
- **Messages and C names** write an item of a dependency with its package's name, `geometry.Point`; the C backend
  gives it a prefixed name, `ceeT_geometry_Point` (`c-backend.md` §2.1).

## 3. Bindings and mutability

- `let x = e;` declares an immutable local and **requires** an initialiser (N0220).
- `let _ = e;` (ADR 0072) binds nothing: `e`, any expression, is evaluated and its value destroyed at the end of the
  statement (a non-Copy value is consumed). `_` may be used any number of times; `var _` and `let _: T` are N0100.
- `var x = e;` / `var x: T = e;` / `var x: T;` declare a mutable local. Without an initialiser
  the type is required (N0221), and the variable must be definitely assigned before every read (§7).
- Without an annotation, a local's type is the type of its initialiser. An initialiser of type
  `void` is an error (N0224).
- Parameters and `const` items are immutable. Assigning to them, or to a `let`, is N0211.
- Assignment targets in M1 are locals only. Member targets (`a.b = …`) are N0216.

## 4. Integer literals

- A suffixed literal (`7u8`) has the suffix's type.
- An unsuffixed literal takes the **expected type** from context (§5) if that type is an
  integer type. With no integer expectation it defaults to `i32`.
- The value must be representable in the literal's type (N0206). A literal directly under
  unary minus is checked as a negative value, so `-128i8` and `-9223372036854775808` (as `i64`) are valid.
- **Character literals** (M11, G4a). `'k'` is an integer literal whose value is the character's Unicode scalar value
  (`grammar.md` §1.6, escapes included: `'\n'` is 10). It takes its type from context like an unsuffixed literal,
  so `c == 'k'` with `c: u8` compares bytes, and `'0'..'9'` is a range. With no integer expectation it is a `u32`.
  In an 8-bit type it must be ASCII (at most `0x7F`), since a larger scalar value is more than one byte in UTF-8 and
  would not equal any byte of the text (N0206). It is a pattern too (§15.2). CeeSafe has no `char` type: text is UTF-8
  bytes (§22.2), and a character type is left for when text gains character iteration.

## 5. Expected types (bidirectional checking)

Each expression is checked against an optional expected type:

- local initialiser → the annotated type; assignment value → the target's type;
  argument → the parameter type; `return` value → the return type; `const` value → its
  declared type; `if`/`while` condition → `bool`; each value branch of an `if` or `match` expression → the
  expression's expected type (§15.3).
- An expression's *actual* type must equal the expected type, unless it is *never* (N0205).
- **Flexible operands.** An expression is *flexible* if it consists only of unsuffixed integer
  literals combined with unary `-`, parentheses and the arithmetic/bitwise binary operators, or (M11) if it is an
  `if` or `match` expression whose expression branches are all flexible (§15.3). Since M11 (G7), unsuffixed float
  literals combined the same way are flexible too; they take an expected `f32` or `f64` instead (§35.2).
  For a binary operator whose operands must have the same type:
  1. if the operator's result type is the operand type and an integer type is expected, both
     operands are checked against it;
  2. otherwise, if exactly one operand is flexible, the other operand is checked first
     (without expectation) and its type becomes the flexible operand's expected type;
  3. otherwise both are checked without expectation (flexible ones default to `i32`).

  So `x + 1` with `x: u8` types `1` as `u8`, and `let y: u64 = 2 * 3;` types both literals as `u64`.

## 6. Operators

All binary operators require both operands to have the **same type**. There are no implicit
conversions.

| Operators | Operand types | Result | Runtime behaviour (implemented in 1c) |
|---|---|---|---|
| `+ - *` | integer | same | overflow → panic |
| `/ %` | integer | same | zero divisor → panic. `MIN / -1` → panic (overflow). `/` truncates toward zero, `%` has the sign of the dividend, and `MIN % -1` is `0` |
| `& \| ^` | **unsigned** integer | same | |
| `<< >>` | left: **unsigned** integer; right: any integer type | left type | amount `< 0` or `≥ bit width` → panic. `>>` is logical |
| `== !=` | integer or `bool` | `bool` | |
| `< <= > >=` | integer | `bool` | |
| `&& \|\|` | `bool` | `bool` | short-circuit |
| unary `-` | **signed** integer | same | `-MIN` → panic |
| unary `!` | `bool` | `bool` | |

**Explicit overflow behaviour** (M11, G4b; OD-6). The operators panic on overflow. Where a program wants another
behaviour, every integer type has nine methods. Each takes one argument of the receiver's type, and a flexible
literal adapts:

| Method | Result | On overflow |
|---|---|---|
| `x.wrappingAdd(y)`, `x.wrappingSub(y)`, `x.wrappingMul(y)` | `T` | the result modulo 2^bits, in two's complement for signed types |
| `x.checkedAdd(y)`, `x.checkedSub(y)`, `x.checkedMul(y)` | `T?` | `null` (otherwise `Some` of the result) |
| `x.saturatingAdd(y)`, `x.saturatingSub(y)`, `x.saturatingMul(y)` | `T` | the nearest of `T`'s minimum and maximum |

Two more take one argument the same way and cannot overflow (ADR 0063): `x.min(y)` and `x.max(y)`, of result `T`,
the smaller and the larger operand (`x` when they are equal).

On signed types (ADR 0063 §2.5), `x.abs()` is the magnitude as a `T`. It panics at `T`'s minimum, whose magnitude
`T` cannot hold, exactly as `-x` does, reported at the call. `x.unsignedAbs()` is the magnitude in the unsigned type
of the same width and never panics. Neither exists on unsigned types (N0234).

**Bit methods** (ADR 0071 §2.3). On every unsigned type, `x.rotateLeft(n)`, `x.rotateRight(n)`, `x.wrappingShl(n)` and
`x.wrappingShr(n)` take a `u32` amount (a flexible literal adapts), taken modulo the bit width, and are of type `T`;
`x.leadingZeros()`, `x.trailingZeros()` and `x.countOnes()` are a `u32` (the width for a zero `x`). On a signed type
they are N0234, as the bit operators are restricted to unsigned types. `x.byteSwap()` is on every integer type: the
bytes in the other order, of type `T` (two's complement for signed types; a one-byte type is unchanged).

None of them panics except `abs`. Their names are reserved on integer types: an `impl` for an integer type cannot define a method
with one of these names (N0228). In C they are `static inline` helpers of `rt.h` with no undefined behaviour: a
wrapping operation computes in the unsigned type of the same width and converts back with `cee_as_iN`
(`c-backend.md` §6).

Bitwise operators and shifts are restricted to unsigned types in M1. This avoids the
implementation-defined and undefined parts of C's signed bit operations. Reinterpret
with `truncate<u32>(x)`. Relaxing this later is backward compatible. A shift amount that is a
flexible literal is typed as `u32`. Other mismatches are N0207.

## 7. Conversions

- `e as T` is allowed only when **every** value of the source type is representable in `T`
  (N0208 otherwise):
  - identity;
  - `uN → uM` and `iN → iM` for M ≥ N;
  - `uN → iM` for M > N;
  - `bool →` any integer type (`false`→0, `true`→1).
  `isize`/`usize` use the target width. Floats (M11, §35.4): `f32 → f64`, and the integers that a float type holds
  exactly.
- `truncate<T>(e)`: `e` of any integer type, `T` any integer type. The result is `e`
  modulo 2^bits(T), reinterpreted in `T` (two's complement). It is always defined. Use it for
  narrowing and for signed↔unsigned reinterpretation.

## 8. Calls and statements

- A callee must be a package function, or one of the built-ins below (N0210 otherwise).
  Functions are not values in M1 (since M11 they are, §36.1). The argument count must match (N0209), and each argument is
  checked against its parameter type.
- Built-ins:
  - `Console.write(x)` and `Console.writeLine(x)`: `x` is an integer, a `bool`, or a string
    literal. `writeLine()` with no argument writes a newline. Both return `void`.
  - `Process.exit(code: i32)`: has type *never*.
  - `truncate<T>(e)`: see §7. Exactly one type argument (N0216 for other generic arguments).
- An expression statement must be a call (N0225). `1 + 2;` is rejected; `let _ = 1 + 2;` discards any value (§3).
- `break`/`continue` must be inside `while` or `loop` (N0214).
- `return;` only in a `void` function. `return e;` only in a non-`void` function (N0215), except that `return v;` of a
  unit local is `return;` (§20.4). Neither is allowed in a `never` function (§1), which does not return (N0215).

## 9. Control flow facts

- **Definite assignment.** A local may be read only if it is assigned on every path from its
  declaration to the read (N0212). Compound assignment (`x += 1`) reads `x`. Code that is
  unreachable is not checked. M1 has no moves, so the analysis is a forward must-analysis
  over the structured statements; it moves to NIR in M2.
- **Termination.** A statement *may complete* unless it is a `return`, `break` or
  `continue`, or a call of type *never*. It also may not complete if it is a block containing
  such a statement, an `if` with an `else` whose two branches both cannot complete, or a `loop`
  with no `break` that targets it. A `while` loop may always complete, whatever its condition:
  write `loop` for infinite loops. An `if` or `match` expression (§15.3) cannot complete when none of its
  branches can: its condition or scrutinee does not complete, or every branch is a block that cannot complete.
- **Missing return.** If the end of a non-`void` function body may be reached, that is N0213. For a function
  returning `never` (§1), the end must be unreachable too: every path ends in a call of type *never* or a `loop`
  without `break`.

## 10. Constants

- `const N: T = e;` where `e` is a constant expression: literals, other constants, and unary,
  binary, `as` and `truncate` applied to constant expressions. Calls are not constant (N0217).
- Constants are evaluated at compile time with the runtime semantics of §6. An operation that
  would panic at runtime (overflow, division by zero, bad shift) is a compile error (N0217).
- A constant that depends on itself is N0218.
- Uses of a constant are replaced by its value, typed as the constant's declared type.
- **Constant arrays** (C9, ADR 0014): `T` may be `[E; N]` with `E` an integer type, `bool` or `StringView`, and
  `e` an array literal `[e1, …, eN]` or repeat `[v; N]` of constant expressions. A use is the whole array's value,
  like an array literal: it can be indexed and iterated, its `.length` read (ADR 0072), and bound to a local to be
  sliced or borrowed. A table to
  be sliced where it is named is a `static` instead (§39.2, ADR 0066). Arrays of other types, and of arrays, are
  N0216.
- **Constant views** (ADR 0064 §2): `T` may be `StringView` (or an array of them, above), with string literals as
  the constant expressions. The value is static data, so a constant view holds no loans. Other reference types are
  N0246.
- `@export` on a constant makes it C data (§28.4).

## 11. `main`

If a function named `main` exists, it must take no parameters and return `i32` or
`void` (N0219). Whether `main` is *required* is decided by `ceesafec build` (increment 1c).

## 12. Structs (M2a)

```ceesafe
struct Point {
    x: i64;
    y: i64;

    fn sum(this) -> i64 => this.x + this.y;                    // method: `this` by value
    fn origin() -> Point => Point { x: 0, y: 0 };             // static function
}
```

- **Declaration.** Top level only. Field names and member names (fields and functions) are unique
  within the struct (N0228). Field types are any non-`void` M1 type or another struct. A struct
  must not contain itself by value, directly or through other structs (N0227: infinite size).
  Recursive data needs indirection (`Box`, M3). Empty structs are allowed.
- **Names.** A struct name is a package item (unique with functions and constants, N0201) and a
  type name. In expressions, `S.f(...)` calls the static function `f` of `S`.
- **Copy.** In M2a every struct is a Copy type: its fields are Copy and `drop` does not exist yet.
  Assignment, argument passing and return copy the value. OD-7's rule ("Copy iff all fields Copy
  and no `drop`") takes effect in 2c.
- **Literals.** `S { f1: e1, …, fn: en }` must initialise **every** field exactly once (missing:
  N0229, unknown: N0230, duplicate: N0231), in any order. Each `ei` is checked against the field's
  type. Initialisers are **evaluated in source order** (`intermediate-representation.md`), not in
  declaration order.
- **Field access.** `e.f` where `e` has struct type `S` and `f` is a field of `S` has `f`'s type
  (unknown field: N0230; `e` is not a struct: N0234). An access chain `a.b.c` is a *place* if `a`
  is a local or `this`.
- **Field assignment.** `a.f = e` and `a.f op= e` require the root local `a` to be a `var` (N0211).
  `this` is immutable in a by-value method. The root must be definitely assigned *as a whole*
  first (N0212): M2a has no partial initialisation.
- **Methods.** A function in a struct whose first parameter is `this` is a method. `this` has the
  struct's type and is an immutable binding (a copy for Copy structs). It is called as `e.m(args)`,
  where `e` is evaluated first and passed as `this`. A static function (no `this`) is called as
  `S.f(args)`. Calling a method as `S.m()`, or a static function as `e.f()`, is N0233. `this`
  anywhere other than a method's receiver position or body is N0232.
- **Private fields (C7c).** A field declared `private` (`private data: T;`) is accessible only in the
  source file that declares its struct (`language-spec.md` §5: "visible within the declaring type or
  file"), and in any instance of a generic one there: read, assigned, borrowed and given in a literal.
  Anywhere else, naming it is N0296. A struct with private fields can therefore only be made by code in
  its own file, so types that cooperate, like a lock and its guard, can share their fields. This is the one place where a visibility
  modifier takes effect within a package (§2). It lets a type keep an invariant that safe code cannot break,
  such as "the data is reached only through a guard" (`integrations/cosmoos/c7/sync.cee`).
  `public` and `internal` fields, and the default, stay visible throughout the package.
- **Operators.** No operator applies to struct values in M2a (`==` included: N0207).
  `Console.write` cannot print structs (N0207).
- **Layout.** Fields are laid out in declaration order with C's rules (`@repr(C)` by default,
  `memory-model.md` §2). The compiler computes size, alignment and field offsets from `TargetInfo`.
  The generated C checks them with `_Static_assert` (`c-backend.md` §2), so a disagreement with the
  real C compiler fails the build instead of miscompiling.

## 13. References (M2b)

The algorithm that enforces these rules is in `lifetimes.md`.

### 13.1 Types
- `borrow T` (shared) and `borrow mut T` (exclusive), where `T` is any non-reference, non-`void`
  type. `borrow borrow T` is rejected (N0246). At runtime a reference is a non-null pointer.
- **Where references may appear (v0):**
  - parameter types;
  - `let` locals with an initialiser;
  - return types (with a source list, §13.5).

  They may **not** appear in struct fields (E3), constants, or `var` locals (N0246). Struct
  fields of reference type need lifetime parameters (OD-3).
- `borrow T` is Copy. A `borrow mut T` value is never copied: every use reborrows it (§13.3).

### 13.2 Creating references
- `borrow p` / `borrow mut p`, where `p` is a **place**: a local, `this`, a field path, or a path
  through a reference such as `r.f` (`(*r).f`). Borrowing anything else is N0247; bind the value
  to a local first.
- `borrow mut p` requires `p` to be mutable: rooted at a `var` local, or reached through a
  `borrow mut` reference (N0248). A `let` binding, a parameter, a by-value `this`, and anything
  reached through a shared reference are immutable. That holds through a `borrow mut` or
  `SliceMut` stored where a shared reference leads: in `h.v` with `h: borrow Holder` and
  `v: borrow mut Vector<T>`, the vector cannot be changed, as with Rust's `& &mut T`.
- **Call sites.**
  - A `borrow T` parameter accepts a place or value of type `T`, borrowed implicitly. A value
    becomes a temporary that lives until the end of the statement.
  - A `borrow T` parameter also accepts `borrow T` or `borrow mut T`; the latter is coerced by a
    shared reborrow.
  - A `borrow mut T` parameter accepts `mut p`, where `p` is a mutable place of type `T`, or a
    `borrow mut T` value (reborrowed).

  The `mut` marker is required for a plain place and is rejected where the parameter is not
  `borrow mut` (N0249). It makes mutation visible at the call site, as C#'s `ref` does.
- **Receivers.** `fn m(borrow this)` and `fn m(borrow mut this)` declare reference receivers;
  `this` then has type `borrow S` / `borrow mut S`. In `e.m(…)` the receiver is borrowed
  implicitly, with no marker. For `borrow mut this`, `e` must be a mutable place or a
  `borrow mut S` value (N0248).
- **A temporary as a receiver** (ADR 0073 §2.1). A receiver of a `borrow this` or `borrow mut this` method that is
  not a place, `make().view()` or `text.split(",").next()`, is evaluated into a temporary that lives until the end of
  its statement (§14.5), and the receiver borrows it; so is a field path on one (`holder().name.view()`). A result
  derived from it may be used within the statement; kept in a `let` used after it, it is N0243. A path through a
  reference is not copied: it is borrowed as a place. An explicit `borrow e` or `mut e` of a value is still N0247.

### 13.3 Using references
- **Auto-deref.** Where a value of type `T` is needed (an operand, a by-value argument, an
  initialiser of type `T`, a return of type `T`, `Console.write`), a `borrow T` or
  `borrow mut T` operand is read through: `*r`. Field access `r.f` and method calls
  `r.m()` also look through references.
- **Writes through references.** `r = v`, `r op= v` and `r.f = v` write to the referent. This
  requires `borrow mut` (N0211: "cannot assign through a shared reference"). A reference local
  itself can never be re-pointed.
- **Reborrow of `borrow mut`.** Using a `borrow mut T` value as a call argument, an initialiser
  or a return value creates a new mutable loan of `*r`. The original reference is unusable while
  the new one is live, and usable again afterwards (B4).
  This holds for a field of type `borrow mut T` or `SliceMut<T>` (`s.left`) as for a variable: it is
  reborrowed, not moved out of its struct, and inside a closure it is a mutable use of the struct.
  Its referent must be mutable from where it is used (§13.2): a field reached through a shared
  reference cannot be reborrowed mutably (N0248).
- References support no operators of their own: `==` compares the referents (via auto-deref).

### 13.4 Borrow rules (summary of `lifetimes.md`)
While a loan is live (the reference, or something derived from it, is used later), the borrowed
place may not be:
- written, mutably borrowed, or go out of scope, if the loan is shared;
- accessed at all, if the loan is `borrow mut`.

Diagnostics: N0240 (write), N0241 (conflicting borrow), N0242 (read while mutably borrowed),
N0243 (out of scope or temporary dropped while borrowed).

### 13.5 Returning references
- A function returning `borrow T` / `borrow mut T` with exactly one reference parameter (counting
  a reference receiver) returns something derived from that parameter.
- Otherwise the return type names the sources: `fn longer(a: borrow T, b: borrow T) ->
  borrow(a, b) T`. Missing, unknown or non-reference sources are N0245, and so is a
  `borrow mut` result naming a shared parameter. After `borrow`, a `(` begins a source list only when it holds
  names separated by commas and a type follows the `)`; otherwise it begins a parenthesised type (M11, §22.1), so
  `borrow (T?)` is a reference to an optional and `borrow(a) (T?)` names a source.
- The body may only return references derived from the named parameters (N0245). A reference to
  a local or temporary of the function is N0244.

## 14. Move types, destruction, `using` and `defer` (M2c)

Normative for 2c. It implements `ownership.md` O1–O5 and R3. The NIR analyses are described in
`intermediate-representation.md`.

### 14.1 Copy and move types (OD-7)
- **Copy:** integers, `bool`, and `borrow T`. A struct is Copy iff it has no `drop` block and every
  field is Copy.
- **Move types:** structs with a `drop` block or a move-type field, and `borrow mut T` (whose uses
  are reborrows, §13.3, so in practice it is never moved out of a variable).
- **Needs drop:** a struct with a `drop` block, or one with a field that needs drop. Every type that
  needs drop is a move type. The converse does not hold.

### 14.2 `drop` blocks
```ceesafe
struct File {
    fd: i32;
    drop { Sys.close(this.fd); }      // `this` is `borrow mut File`
}
```
- At most one `drop` block per struct (N0228). Inside it, `this` has type `borrow mut S`. The block
  cannot be called explicitly. It runs exactly once when a value of `S` is destroyed (O4), and the
  fields that need drop are destroyed **after** it, in declaration order.
- Moving a field out of a value whose type has a `drop` block is N0252: the `drop` block needs the
  whole value. Moving out of `this` inside `drop` is impossible anyway (§14.3).

### 14.3 Moves
- **Using a move-type place by value moves it**:
  - an initialiser or right-hand side;
  - a by-value argument or a by-value `this` receiver;
  - a struct-literal field;
  - a `return` value.

  The place is then uninitialised. Reading, borrowing or moving it again before it is reassigned is
  **N0250** ("use of moved value"); this is O2 and O3.
- **`move p`** explicitly moves out of the place `p`, even for a Copy type. Afterwards `p` is
  uninitialised, which ends a value's use early.
- **Partial moves** (O2): moving `p.f` out of a local is allowed if no struct on the path from
  `p` to `f` has a `drop` block. `p` is then partially initialised. `p.f` may be reassigned, after
  which `p` is whole again. Reading `p` as a whole, or borrowing it, needs all of it initialised
  (N0250).
- **Not allowed:**
  - moving out of a place reached through a reference (N0251), because the referent must stay
    valid; this includes `this` in `borrow`/`borrow mut` methods;
  - moving out of a `using` binding (N0253).
- **Moves and borrows:** moving a place while it is borrowed is a conflict, treated like a write
  (N0240).

### 14.4 Initialisation (replaces §9's first bullet)
The analysis runs on NIR move paths (`intermediate-representation.md`):
- A place must be **definitely initialised** to be read, borrowed or moved. If it may never have
  been assigned, that is N0212. If it may have been moved, that is N0250.
- Assigning a field `p.f` requires the rest of `p` to be initialised (no piecewise construction
  of an uninitialised struct). Re-initialising a moved-out field is allowed.
  - If some enclosing struct of `p.f` has a `drop` block, that whole struct must be initialised,
    `f` included: such a struct is never partially moved (N0252), so a missing field means the
    whole value was moved. This also covers a struct whose only field is `f` (found by the
    mutation suite, M2d).

### 14.5 Destruction (O4)
- A value that needs drop is destroyed exactly once, at the first of these:
  - the end of its variable's scope: the block end, a `break`/`continue` leaving the block, or a
    `return`;
  - the end of its statement, for a temporary. An `if` or `while` condition is its own statement:
    its temporaries are destroyed once it is evaluated, before the body runs, on every evaluation
    (M15 S2; before M15 they were never destroyed, #1);
  - being **overwritten** by an assignment, in which case the old value is destroyed first.
- **Order:** locals in reverse declaration order; then a struct's `drop` block; then its fields in
  declaration order. By-value parameters are destroyed when the function returns, after the locals.
- **Moved values are not destroyed.** A value that has been moved on *some* paths gets a runtime
  **drop flag**. A partially moved struct destroys only the fields it still has.
- **`Process.exit` and panics do not run destructors or deferred statements** (panic = abort,
  OD-5).

### 14.6 `using`
`using x = e;` declares a local that cannot be moved out of (N0253) and is destroyed at the end
of its block, like any local (R3). It documents that the value's lifetime *is* the scope: a lock
guard, an arena, a file.

*Since M4b:* a `using` binding is **pinned, not immutable**. Its contents may be changed and it may
be borrowed mutably (`arena.reset()`, `frame.enter()`). It cannot be assigned as a whole (N0211)
or moved (N0253). This also lets scoped values that must be mutably borrowed, such as scratch
frames, live in a `using` binding, since they cannot be `var`.

### 14.7 `defer`
`defer s;` schedules the statement `s` to run when the enclosing block exits on any path:
- the block end;
- `break`/`continue` out of it;
- `return` (after the return value is computed).

Deferred statements and local destructions run in reverse order of their position, so a `defer`
runs before the locals declared before it are destroyed, and after the locals declared after it.
`s` may not contain `return`, `break`, `continue` or another `defer` (N0254).

## 15. Enums and `match` (M2d)

```ceesafe
enum Shape {
    Empty,
    Circle(i64),
    Rect(i64, i64),
}

fn area(s: Shape) -> i64 {
    match (s) {
        Shape.Empty => { return 0; }
        Shape.Circle(r) => { return 3 * r * r; }
        Shape.Rect(w, h) => { return w * h; }
    }
}
```

### 15.1 Declarations and values
- `enum E { V1, V2(T1, …), … }` at the top level. Variants are unique (N0228) and have zero or
  more positional payload types: any non-reference, non-`void` type (E3 applies to payloads too).
  An enum must not contain itself by value (N0227).
- **Values.** `E.V` for a variant without payload; `E.V(e1, …)` with exactly the payload arity
  (N0209). Each `ei` is checked against its payload type.
- **Equality** (ADR 0072). For an enum none of whose variants has a payload (a *fieldless* enum), `a == b` and
  `a != b` compare the variants; both operands have that enum type and are only read. An enum with payloads is
  N0207 (compare with `match`), and so are `<`, `>` and the other orderings on any enum.
- **Copy and drop.** An enum is Copy iff every payload type is Copy. It needs drop iff some payload
  needs drop; destroying it destroys the payloads of the active variant. Enums have no `drop`
  block (v0).
- **Layout.**
  - A tag (`u8` for ≤ 256 variants, else `u16`/`u32`), then a union of the variants' payloads, each
    laid out like a struct.
  - Size and alignment follow C's rules for `struct { tag; union { … }; }`. The generated C
    asserts them.
  - CeeSafe does not reorder or niche-optimise (v0).

### 15.2 `match`
```text
MatchStmt = 'match' '(' Expr ')' '{' Arm* '}'
Arm       = Pattern '=>' Block ','?
Pattern   = '_' | IntLiteral | '-' IntLiteral | 'true' | 'false' | 'null'
          | QualifiedName ('(' Payload (',' Payload)* ')')?        -- E.V, E.V(x, _), Ok(E.A(x)) (M11)
Payload   = Binding | Pattern                                      -- a nested pattern: M11 (G1)
Binding   = Identifier | 'var' Identifier | '_'        -- 'var': M3c (§18.4)
```
A payload that is a single identifier is a binding; `Some(…)`, `Ok(…)`, `Err(…)` and `E.V…` there are patterns.
- This section's `match` is a **statement**; §15.3 has the expression form. Arms are tried in order; the first
  that matches runs its block.
- **Scrutinee types:**
  - an enum: variant patterns;
  - an integer: literal patterns of that type;
  - `bool`: `true`/`false`;
  - or a reference to one of these.
- **Exhaustiveness** (N0256): enum matches must cover every variant or end with `_`. Integer
  matches need `_`. `bool` matches need both values or `_`. An arm that can never match (a
  repeated variant or literal, or anything after `_`) is N0257.
- **Bindings.** A variant pattern binds each named payload to a new immutable local in the arm:
  - **Scrutinee of type `E` (a value):** the binding is a *copy* of the payload. Since M3c a
    non-Copy payload is moved out, and the match consumes its scrutinee (§18.4).
  - **Scrutinee of type `borrow E` / `borrow mut E`:** the binding is a `borrow T` /
    `borrow mut T` to the payload, and the borrow checker treats it like any reborrow of the
    scrutinee.
  - `_` binds nothing.
- The scrutinee is evaluated once. When it is a place it is not moved (only Copy payloads are
  read). A temporary scrutinee lives until the end of the `match` statement.
- **Nested patterns** (M11, G1). In a payload's position, a pattern instead of a binding matches the payload itself:
  a variant pattern on an enum payload (`Ok(E.A(x))`, `Some(Some(_))`), a literal on an integer or `bool` payload
  (`Some(3)`), or `null` on an optional payload (`Ok(null)`). Each is checked against the payload's type exactly as
  a whole `match` is checked against the scrutinee's, at any depth.
  - **Order.** Arms are tried in order. Within an arm, the outer variant is tested first, then each nested pattern,
    left to right and depth first. Testing only reads tags and values: an arm that does not match moves nothing.
  - **Bindings** inside nested patterns follow the rules above. On a value scrutinee they copy, or move (§18.4).
    On a reference scrutinee they are references to the nested payload.
  - **A payload that is itself a reference** (`borrow E`) takes only a binding or `_`, not a nested pattern. Matching
    through it is a separate `match` on that reference (N0205 otherwise).
  - **Exhaustiveness and unreachable arms** are decided over the whole patterns. A match is exhaustive when every
    value of the scrutinee's type matches some arm: N0256, whose message names one value that matches none, such
    as `Ok(E.B)`. An arm is unreachable when every value it matches is matched by an earlier arm: N0257. The decision
    is the usefulness algorithm of pattern-matching compilers (Maranget, *Warnings for pattern matching*, 2007), with
    each enum's variants, `true`/`false`, `null`/`Some` and `Ok`/`Err` as the constructors. Integer literals never
    cover their type: an integer position needs a binding or `_` somewhere.

### 15.3 `if` and `match` expressions (M11, G5)
```ceesafe
let sign = if (x < 0) -1 else if (x == 0) 0 else 1;
let name = match (shape) {
    Shape.Circle(_) => "circle",
    Shape.Square(side) => if (side > 10) "big square" else "square",
    Shape.None => { return; },
};
```
```text
IfExpr    = 'if' '(' Expr ')' Branch 'else' (IfExpr | Branch)
MatchExpr = 'match' '(' Expr ')' '{' (ValueArm (',' ValueArm)* ','?)? '}'
ValueArm  = Pattern '=>' Branch
Branch    = Expr | Block | ValueBlock
ValueBlock = '{' Statement* Expr '}'        (* the last Expr without ';', ADR 0073 §2.3 *)
```
- **Where.** In expression position (an initialiser, an operand, an argument, a `return` value, …). An `if` or
  `match` that begins a statement is the statement form (§9, §15.2), as before. An `if` expression needs its
  `else` (N0100 otherwise).
- **Branches.** A branch is an expression, the value of the whole when that branch is taken, or a block that
  cannot complete (§9): one that ends in `return`, `break`, `continue` or a call of type *never*, such as
  `{ return; }`, `{ Process.exit(1); }` or `{ die("bad number"); }` with `fn die(m: StringView) -> never` (§1). A
  block that can complete has no value: N0235.
- **Value blocks** (ADR 0073 §2.3). A block branch may end with an expression written without `;`: the branch's
  value, typed as an expression branch is. An `if` (with its `else`) or `match` that ends a value block, directly
  before its `}`, is read as an expression. Only branches of `if` and `match` expressions have value blocks: in a
  function body, a statement-form `if` or `match` or a plain block, a final expression without `;` is N0100. The value
  is moved out before the block's locals and temporaries are destroyed; a reference to one of them as the value is
  N0243.
- **Extent.** An expression branch extends as far right as an expression can: `if (c) 1 else 2 + 3` is
  `if (c) 1 else (2 + 3)`. Write parentheses to use an `if` expression inside a larger one: `(if (c) 1 else 2) + 3`.
  Value arms are separated by commas; a block arm needs none.
- **Type.**
  - With an expected type (§5), every expression branch is checked against it, and the whole has that type.
  - Without one, the first expression branch that is neither flexible (§5) nor `null` is checked first, and its
    type is the type of the whole; the other branches are checked against it. If every branch is flexible, the
    type is its literals' default: `f64` if any is a float literal (§35.2), else `i32`. A branch of type *never* (and every block branch) fits any type. If no branch is an
    expression, the whole has type *never*.
- **Evaluation.** The condition (or scrutinee) is evaluated once, then exactly one branch. `match` expressions
  follow §15.2 in everything else: the patterns, exhaustiveness (N0256) and unreachable arms (N0257), bindings by
  copy, move or reference, and consuming a value scrutinee (§18.4).
- **Ownership.** A branch's value is moved (or copied) into the result. A move in one branch leaves the place
  maybe-moved after the whole, as after an `if` statement (§14.4). A reference produced by a branch carries that
  branch's loans, so the result carries the loans of every branch that can produce it (`lifetimes.md`).
- **Temporaries** made while evaluating a branch end when the branch ends, as at the end of a statement
  (`lifetimes.md` §6). A reference to one cannot be the branch's value (N0243).

## 16. Arrays, slices and `for` (M2d)

### 16.1 Types
- `[T; N]` is a fixed array of `N` elements. `N` is an integer literal (no suffix or `usize`) with
  1 ≤ N ≤ 2^48 − 1, and the array is at most 2^31 − 1 bytes. It is laid out as `N` consecutive `T`s:
  size `N × size(T)`, the alignment of `T`.
  - **v0 restriction:** `T` must be a Copy type, so arrays are Copy and never need drop. Arrays of
    move types need element-wise move tracking (future). Slices may have move-type elements
    since M6b (§21.3).
  - Otherwise arrays may appear anywhere a value may: locals, parameters, results, struct fields,
    enum payloads and other arrays. Constants of array type are not supported yet (N0216).
- `Slice<T>` / `SliceMut<T>` are **borrowed views** `(pointer, length)` of a run of `T`s in an array
  or another slice. They follow every rule for references (§13), with the same scoping:
  - only in parameters, `let` locals with an initialiser, and return types with a source list;
  - never in fields, payloads, array elements, constants or `var` locals (N0246);
  - `Slice<T>` is Copy, and each use of a `SliceMut<T>` reborrows it; `SliceMut<T>` coerces to
    `Slice<T>`;
  - a slice holds a loan of the whole array (or slice) it was taken from (`lifetimes.md`);
  - a function returning a slice has no `borrow(...)` source list: it must have exactly one
    reference (or slice) parameter, which the result is derived from (§13.5's default rule);
  - the element type follows the array rules: Copy, never a reference.
- Internally, `Slice<T>` is `borrow [T]` and `SliceMut<T>` is `borrow mut [T]`, with `[T]` an
  unsized array type that only exists behind a slice. That is why every reference rule applies.

### 16.2 Values
- `[e1, …, en]`: all elements have the same type (the expected element type, or the first
  element's), n ≥ 1.
- `[v; N]`: N copies of `v`.
- `a.length` is a `usize`: the constant `N` for arrays, the stored length for slices. For an
  array, `a` must be a place (a variable, field or element), because the constant would skip
  evaluating it (N0216), unless there is nothing to evaluate: a `const` array (§10) or a literal of constants
  (ADR 0072).
- **Equality** (ADR 0072). `a == b` and `a != b` compare two arrays of the same type `[T; N]`, or two slices of the
  same `T` (a `SliceMut<T>`, and a vector's elements, lent as `Slice<T>`): equal lengths and equal elements in order.
  `T` is an integer type, `bool` or a fieldless enum (§15.1); other element types, floats included (`NaN != NaN`),
  are N0207, and so are arrays of different lengths. `Slice<u8> == StringView` is N0205: compare `s == t.bytes()`.
  The operands are only read. The comparison is not constant-time.

### 16.3 Indexing and slicing
- `a[i]`:
  - `a` is an array or slice place, or a reference to an array;
  - `i` is a `usize` (a flexible literal becomes `usize`);
  - `i ≥ a.length` **panics** ("index out of bounds", reported at the index expression);
  - `a[i]` is a place: it can be read (a copy), assigned (if mutable: an array rooted at a `var`
    or reached through `borrow mut`, or a `SliceMut`), and borrowed.
- `a[lo..hi]`, `a[lo..]`, `a[..hi]`, `a[..]` give a `Slice<T>` of elements `lo` up to but not
  including `hi` (default `0` and `a.length`). `lo ≤ hi ≤ a.length` is checked; otherwise the
  program **panics**.
- `borrow mut a[lo..hi]` gives a `SliceMut<T>` and needs `a` to be mutable. As a call argument
  for a `SliceMut<T>` parameter, `mut a[lo..hi]` means the same.
- Slicing a value that is not a place (ADR 0073 §2.1): an array value, or a view or slice value, is held in a
  temporary until the end of the statement and sliced there (`digest()[0..4]`, `s.view()[1..3]`); a slice of an array
  temporary cannot be kept after the statement (N0243), a slice of a view keeps the view's loans. A place reached
  through a reference is sliced as a place. `.length` of an array value is its constant length, the value still
  evaluated. Anything else that is not a place is N0247.
- Evaluation order: the array place (with its own indexes), then `lo`, then `hi`. In an assignment
  `a[i] = v`, the target's indexes are evaluated and checked before `v`.
- The borrow checker treats all elements of an array as **overlapping** (no index reasoning in v0).
  Borrowing `a[0]` and then writing `a[1]` while the borrow is live is rejected.

### 16.4 `for`
The header is parenthesised, like `while` and `match`, so a name before the body is never read as a
struct literal.
- `for (i in lo..hi) { … }`: `i` takes each value from `lo` up to but not including `hi`. Both bounds
  are evaluated once, have the same integer type (flexible literals adapt), and `i` is immutable
  in the body.
  - **Index ranges** (ADR 0063). When both bounds are flexible literals, the range is `usize` if the body indexes
    with `i`, and the literals' default (`i32`, §5) otherwise. The body indexes with `i` where `i` is an index
    (`a[i]`) or a slice bound (`a[lo..i]`), or an operand of `+ - * / %` that is one (`a[i * w + x]`), in the body's
    statements, nested blocks and closures. Inside a call, cast, comparison or condition in an index it does not
    count (`a[truncate<usize>(i)]` keeps `i32`). The body also indexes with `i` where it passes `i` itself as an
    argument for a `usize` parameter (ADR 0072), of a callee known before the body is checked: a function named
    directly, a struct's static function (`Table.at(i)`), or a method on `this`, a parameter or an earlier local
    (`list.host(i)`). The decision is syntactic and made before the body is checked;
    locals cannot be shadowed (N0203), so the name means `i` throughout.
- `for (x in seq) { … }`: `seq` is an array place, a slice, or a reference to an array. `x` is a copy
  of each element from index 0 upward. The element is read when the loop reaches it, and the
  length is read once at the start.
- **Non-Copy elements** (M11, G2): when the element type `T` is not Copy, `x` is a `borrow T` to the element, in
  place, instead of a copy (which could not be made: N0251). This is the rule of a `match` on a reference (§15.2). The
  borrow checker treats it as a shared borrow of that element for as long as `x` is used. While it lives, the
  sequence cannot be changed (§13.4, §23.1). A Copy element is still copied, as before.
- `break` and `continue` behave as in `while`. `continue` proceeds to the next element.

- The loop variable is immutable. It has the bounds' type for a range, and the element type
  for a sequence (or `borrow T`, above). Like every local, it cannot shadow another (N0203).

### 16.5 Bulk operations (M9b)
Two methods write a whole slice in one operation. A program computes the same thing with them as with
the element loop they replace; they exist so that the copy costs one check instead of one per element.

```ceesafe
fn copy(out: SliceMut<u8>, to: usize, src: Slice<u8>, from: usize, n: usize) {
    out[to..to + n].copyFrom(src[from..from + n]); // two range checks, one length check, one copy
}
```

- `d.copyFrom(s)`, with `d: SliceMut<T>` and `s: Slice<T>` (a `SliceMut<T>` coerces): if
  `s.length != d.length` the program **panics** ("slice lengths differ", reported at the call) and
  writes nothing. Otherwise each `d[i]` becomes `s[i]`, as if `s` were first copied to a temporary.
  In safe code the two cannot overlap, because `s`'s loan would conflict with `d`'s (N0241, N0242);
  if `unsafe` code makes them overlap (§27.3, `Slice.fromRaw`), the result is still defined.
- `d.fill(v)`, with `v: T`: every element of `d` becomes `v`. Nothing is checked.
- `T` must be Copy (N0259), because both methods copy elements.
- The receiver is a `SliceMut<T>` (a parameter, a local or a call result; reborrowed, like any use of a
  `SliceMut`), or a slicing expression `a[lo..hi]` in any of its forms, which is then taken as
  `borrow mut a[lo..hi]`. That needs `a` to be mutable (N0248), as `mut a[lo..hi]` does for a
  `SliceMut` argument. A `Slice<T>` receiver is N0211. Arrays and vectors are written through a slice:
  `a[..].fill(0)`, `v.viewMut().fill(0)`.
- Evaluation is left to right: the receiver (its place, bounds and range check), then the argument, then
  the length check, then the writes. The receiver's loan starts before the argument is evaluated: these
  calls are not two-phase (B5 applies only to `borrow mut this` methods of a place). So the argument
  cannot read the destination: `s.fill(s[0])` is N0242, and `let v = s[0]; s.fill(v);` is the fix.
- The result is `void`.
- Not provided in M9b: a copy within one slice whose ranges may overlap, comparisons, searches.
- **`d.swap(i, j)`** (M11 G8c), with `i, j: usize`: exchanges elements `i` and `j` of a `SliceMut<T>` (or of a
  vector, which lends its elements mutably). `T` may be any element type, Copy or not: the elements are moved, not
  copied. Both indices are checked first (panic "index out of bounds", reported at the call), and nothing changes
  if either is out of range. `i == j` changes nothing. Effects: none.

Diagnostics: N0259 (an element type that is not Copy, an invalid length, or a too-large array).
The other errors reuse N0205, N0211, N0216, N0230, N0246, N0247 and N0248 (`diagnostics.md`).

### 16.6 Bytes and text (ADR 0071)
Methods of byte slices (`Slice<u8>`, `SliceMut<u8>`, a `Vector<u8>`'s elements), of `StringView` and of a `String`'s
text, written in lib/core (`bytes.cee`, `text.cee`, `numbers.cee`) and reached like `sortBy` (§36.4).

**Byte order** (§2.2), on byte slices and byte vectors only (a `StringView` reads through `s.bytes()`; N0200 otherwise):
- `b.readBE<T>(at)` and `b.readLE<T>(at)` (`at: usize`) are the `T` in the `size(T)` bytes at `at`, big- or
  little-endian, in two's complement for signed `T`. `T` is a fixed-width integer type, `u8` to `u64` or `i8` to `i64`
  (N0207 otherwise, `usize` and `isize` included); a missing type argument is N0265.
- `d.writeBE(at, x)` and `d.writeLE(at, x)` write `x` there. The receiver is a `SliceMut<u8>`, a mutable vector's
  elements, or a slicing expression taken mutably (N0211 for a `Slice<u8>`). `x`'s type gives the width: a type
  argument (`writeBE<u32>(at, 5)`) or the value's own type; a literal without a suffix and without a type argument
  is N0265. So is a character literal without one: it is a `u32` code point, so `'P'` alone would write four bytes
  (`pushBE<u8>('P')` writes one; 0.15.2). They are not two-phase, like `copyFrom` (§16.5).
- `v.pushBE(x)` and `v.pushLE(x)` on a `Vector<u8, A>` append `x`'s bytes with one growth: `Result<void, AllocError>`,
  the vector unchanged on failure. The width is chosen as for `writeBE`. They are two-phase (B5), like `push`.
- A read or write checks `at < length` and `at + size(T) - 1 < length` at the call, before anything is read or
  written, and **panics** "index out of bounds" there otherwise.

**Searching and trimming** (§2.4). On a `StringView` (or a `String`), with `StringView` arguments; on byte slices with
`Slice<u8>` arguments and results. Results that are views borrow the receiver, like slicing it.
- `s.indexOf(t)` and `s.lastIndexOf(t)`: `usize?`, where `t` first or last starts; an empty `t` is at `0`, and at
  `s.length` for `lastIndexOf`. `s.indexOfByte(b)` (`b: u8`): `usize?`.
- `s.contains(t)`, `s.startsWith(t)`, `s.endsWith(t)` and `s.equalsIgnoreAsciiCase(t)`: `bool`; only `A`–`Z` and
  `a`–`z` fold.
- `s.trim()`, `s.trimStart()`, `s.trimEnd()`: the view without leading and trailing ASCII whitespace (space, `\t`,
  `\n`, `\v`, `\f`, `\r`).
- `s.splitOnce(sep)`: `TextHalves<r>?` (`ByteHalves<r>?` on bytes) with the parts before and after the first `sep`, or
  `null`; an empty `sep` is found at the start.
- `s.split(sep)`: a `TextSplit<r>` (`ByteSplit<r>`) whose `next()` is each piece between separators in order, then
  `null`: `"a,,b"` gives `"a"`, `""`, `"b"`, an empty text one empty piece, and an empty `sep` the text whole.
- **UTF-8.** A match of valid UTF-8 inside valid UTF-8 starts and ends on character boundaries, and ASCII whitespace
  is never part of a longer character, so every `StringView` these return is valid UTF-8 without a further check.
- On other element types they are N0200. None of them panics.

**Numbers from text** (§2.5), on `StringView`, `String` and byte slices:
- `s.parseInt<T>()` and `s.parseIntRadix<T>(radix)` (`radix: u32`, 2 to 36): `T?` for any integer type `T`. The text
  is an optional `+` (or `-` for a signed `T`), then one or more digits `0`–`9`, `a`–`z`, `A`–`Z` below the radix,
  and nothing else. A value outside `T`, any other text, and a radix outside 2–36 are `null`.
- `s.parseFloat<T>()`, `T` `f32` or `f64`: `T?`. The text is `[+-]digits[.digits][(e|E)[+-]digits]` with a digit on
  at least one side of the point, or `inf`, `infinity` or `nan` in any case after an optional sign; anything else is
  `null`. The result is the nearest `T` to the decimal value, ties to even, rounded once (an `f32` is not rounded
  through an `f64`); past the largest finite value it is ±infinity, and below the smallest subnormal ±0. For every
  finite `x`, parsing the text `Console.writeLine(x)` prints gives `x` back (§35.7). It does not use the C library,
  so the locale does not matter, and the interpreter runs the same code.
- A missing type argument is N0265, and a type that is not an integer (or float) type N0207. Under `--no-float`,
  `parseFloat<f64>` names a float type, which is N0297 (§35.9).

The method names are reserved on these types, as the integer methods are (§6). The lib/core structs that hold them
(`ByteOrder`, `VectorBytes`, `TextSearch`, `ByteSearch`, `TextHalves`, `ByteHalves`, `TextSplit`, `ByteSplit`,
`NumberParsing`) are names no program may define (N0201).

## 17. Results and error propagation (M3a)

```ceesafe
enum ParseError { Empty, BadDigit(u8), Overflow }

fn digit(c: u8) -> Result<u32, ParseError> {
    if (c < 48u8 || c > 57u8) { return Err(ParseError.BadDigit(c)); }
    return Ok((c - 48u8) as u32);
}

fn twoDigits(a: u8, b: u8) -> Result<u32, ParseError> {
    return Ok(digit(a)? * 10 + digit(b)?);
}
```

`Result`, `Ok`, `Err` and `AllocError` are built in (ADR 0003). User generics come in M6; the
names below then become library items with the same meaning.

- **Splitting** (M13 G5): `s.splitAt(mid)` gives a `SliceHalves<T>` whose `left` is `s[..mid]` and `right` is
  `s[mid..]`. `s.splitAtMut(mid)`, on a `SliceMut<T>` or a vector, gives a `SliceHalvesMut<T>` of two `SliceMut<T>`
  that do not overlap, so both may be written at once, at a split known only at run time. Both panic ("slice range
  out of bounds") if `mid` is past the end; the panic names lib/core's line, not the caller's. The halves borrow `s`
  for as long as either lives. They are lib/core code (`SliceSplitting<T>`, `lib/core/slices.cee`).

### 17.1 Types
- `Result<T, E>` is a built-in enum with two variants, in this order: `Ok` with payload `T`, and
  `Err` with payload `E`. Everything in §15.1 applies: layout, Copy iff `T` and `E` are Copy,
  needs drop iff either does, and only the active variant's payload is destroyed.
- `T` may be `void`, in which case `Ok` has no payload. `E` must not be `void`. Neither may be a
  reference or slice (N0246, E3); arrays, structs, enums and results are fine.
- A type contains `T` and `E` by value: `struct S { r: Result<S, i32>; }` is recursive (N0227).
- `AllocError` is a built-in enum `{ OutOfMemory }` (Copy). The names `Result`, `Ok`, `Err` and
  `AllocError` cannot be redeclared as items (N0201).
- Two `Result` types are equal iff their `T` and their `E` are equal.

### 17.2 Values
- `Ok(e)`, `Ok()` (for `T = void`) and `Err(e)` need an **expected `Result` type** that fixes
  `T` and `E`. That type comes from a declared variable type, a parameter type, a return
  type (`return Ok(x);`), or a field or payload type. Without one, it is N0260 ("write the
  type").
- The payload is checked against `T` or `E` like a call argument against its parameter (§5).

### 17.3 Matching
- Patterns `Ok(x)`, `Ok(_)`, `Ok` (for `T = void`), `Err(x)` and `Err(_)` are written without a
  type prefix. The bindings and exhaustiveness rules of §15.2 apply unchanged: a non-Copy payload
  is bound by matching on `borrow r` (N0258 otherwise).

### 17.4 `e?` and `e!`
```text
Postfix = Primary ( … | '?' | '!' )*      -- '!' is postfix only when it follows an operand
```
- **`e?` (propagate).**
  - `e` must have a `Result<T, E>` type (N0262). The enclosing function must return
    `Result<U, E>` with the same `E` (N0261); `From` conversions come in M6.
  - If `e` is `Ok(v)`, the value of `e?` is `v` (of type `T`; `void` if `T` is `void`).
  - If `e` is `Err(x)`, the function returns `Err(x)` at once, exactly like a `return`
    statement. The statement's temporaries end, then every scope is left (drops and deferred
    statements), innermost first. Like `return`, `?` is not allowed in a deferred statement
    (N0254).
- **`e!` (unwrap).** `e` must have a `Result<T, E>` type (N0262). The value is `v` for `Ok(v)`;
  `Err(_)` **panics** with "unwrapped an Err value". Panic is abort, so nothing is destroyed.
- In both cases `e` is evaluated once and consumed. The payload moves out of it (or is copied);
  nothing else of `e` remains to be destroyed.
- As an expression statement: `f()?;` and `f()!;` are allowed when the operand is a call, like
  calls themselves (N0225 otherwise).
- Precedence: postfix, like a call or member access: `parse(a)?.value` and `-x?` work as
  expected (`-` applies to `x?`). `!` directly followed by `=` lexes as `!=`. Write `r! == 1`
  with a space.

### 17.5 Discarded values (correction to §14.5)
- An expression statement whose value needs drop (for example a call `make();` returning a
  `Res`) destroys the value at the end of the statement, like any other temporary. Before M3a
  the value was never destroyed: an O4 violation, found while specifying 3a.

Diagnostics: N0260 (no expected `Result` type for `Ok`/`Err`), N0261 (`?` in a function whose
return type is not a `Result` with the same error type), N0262 (`?` or `!` on a non-`Result`).

## 18. Boxes and allocators (M3b)

```ceesafe
struct Account {
    id: u64;
    balance: i64;
    fn deposit(borrow mut this, amount: i64) { this.balance += amount; }
}

fn open(id: u64) -> Result<Box<Account>, AllocError> {
    return Box.create(Account { id: id, balance: 0 });
}

fn main() -> i32 {
    var acct = open(7)!;
    acct.deposit(100);       // through the box
    return 0;
}                            // the box is dropped: Account destroyed, memory freed
```

`Box`, `SystemHeap` and `FailingAllocator` are built in (ADR 0003).

### 18.1 Allocator types
- `SystemHeap` and `FailingAllocator` are built-in, field-less, Copy structs. They are the only
  **allocator types** in M3; user allocators come with interfaces in M6 (`memory-model.md` §3.2).
  Their values are written as struct literals: `SystemHeap {}`.
- `SystemHeap` is the platform heap (`malloc`/`free` in hosted and CosmoOS userland builds).
- `FailingAllocator` allocates from the platform heap, but can be told to fail, for testing
  error paths in the language itself:
  - `FailingAllocator.failAfter(n: u64)`: the next `n` allocations through `FailingAllocator`
    succeed, and every later one fails;
  - `FailingAllocator.reset()`: allocations never fail (the initial state).
  The countdown is process-wide; frees are never affected.
- Their names, and `Box`, cannot be declared as items (N0201).

### 18.2 `Box<T, A>`
- `Box<T, A>` owns exactly one value of type `T` in memory obtained from allocator type `A`
  (H1). `Box<T>` means `Box<T, SystemHeap>`. `A` must be an allocator type (N0263). `T` is any
  value type except `void` and references or slices (N0246).
- A box is a **move type** that needs drop, and is one pointer wide (8 bytes on LP64).
  Destroying it destroys `T` (its drop glue), then returns the memory to `A`.
- A type does **not** contain the target of a `Box` by value, so recursive types are built with
  boxes: `enum List { Nil, Cons(i32, Box<List>) }` is allowed (no N0227).
- **Creation** (built-in functions):
  - `Box.create(v)`: `Result<Box<T>, AllocError>`;
  - `Box.createIn(a, v)` with `a` a value of allocator type `A`: `Result<Box<T, A>, AllocError>`.
  `T` is the type of `v`. An expected type `Result<Box<X, _>, _>` provides the type hint for `v`
  (so `Box.create(5)` can make a `Box<u8>`). `v` is moved into the call. If the allocation
  fails, `v` is destroyed and the result is `Err(AllocError.OutOfMemory)`.
- **Access through a box** works like access through a reference (§13.3), with the box's own
  mutability:
  - fields `b.f`, elements `b[i]` and method calls `b.m()` reach `T` (auto-deref);
  - the contents are mutable iff the box is: the box variable is `var`, or the box itself is
    reached through `borrow mut` or another mutable place. `b.f = 1` needs a `var b`;
  - a `Box<T>` argument for a `borrow T` parameter borrows the contents, and `mut b` lends them
    for a `borrow mut T` parameter;
  - where a Copy `T` is expected, a `Box<T>` is read through (auto-deref);
  - a `borrow Box<T>` converts to `borrow T`, and a `borrow mut Box<T>` to `borrow mut T` or
    `borrow T`: a reborrow of the contents (for example, a payload bound by reference in a match);
  - `borrow b` borrows the box itself (`borrow Box<T>`).
- **Borrowing rules:** a place inside a box is part of the box. Moving, overwriting or dropping
  the box while a loan of its contents is live is rejected (N0240, N0243), as for any place and
  its fields. (Unlike a reference going out of scope, a box going out of scope destroys what it
  points to.)
- **Moving out of a box's contents**: see §18.3 (M3c).
- No null boxes exist. A `Box` is never Copy and has no `==`.

Diagnostics: N0263 (an allocator argument that is not an allocator type).

### 18.3 Moving out of a box (M3c)
- A box held in an **owned place** owns its contents as a part of that place: a local, or a
  field path of one, not reached through a reference. Its fields may be moved out
  (`let l = n.left;`, `move n.left`), exactly like fields of a struct local (§14.3–14.4):
  - the box cannot be used as a whole again (moved, passed, matched, borrowed) until every
    moved-out field is re-initialised by assignment (N0250);
  - a field of a `T` with a `drop` block cannot be moved out (N0252);
  - through a reference, a box's contents are as immovable as anything else (N0251).
- Destroying a partially moved box destroys the fields still initialised (in declaration order),
  then returns the memory to the allocator. `T`'s drop glue does not run, and `T` has no `drop`
  block by the rule above.
- There is no way to move the whole contents out in v0 (`T` would need an unbox operation, M6).

### 18.4 Moving payloads out in `match` (M3c; replaces N0258)
```text
Binding = Identifier | 'var' Identifier | '_'
```
- `var x` makes a mutable binding; `x` an immutable one. Both may bind any payload type.
- **Consuming match.** If the scrutinee is a value (not a reference) and some arm binds a
  non-Copy payload, the match **consumes** its scrutinee:
  - the scrutinee is moved when the match starts. A place scrutinee is left uninitialised, as by
    `move`; a `using` binding cannot be consumed (N0253);
  - in an arm whose pattern lists payloads (`E.V(…)`), every payload of the variant moves out:
    named ones into their bindings, and `_` ones into hidden locals of the arm. Payloads not bound
    by name are therefore destroyed when the arm ends, before the bindings' own scope ends.
    A payload matched by a nested pattern (M11) moves into a hidden local too, and the same rule applies to it:
    if its pattern lists payloads, they move out of it in turn;
  - in any other arm (`_`, `E.V` without a list, a variant without payload), the value is
    destroyed when the match statement ends.
- Otherwise (Copy payloads only, or a reference scrutinee) the rules of §15.2 apply unchanged.
- N0258 is no longer reported (since M3c).

### 18.5 Destroying deep structures (R-9)
- A box's drop glue is recursive in general: destroying a `T` destroys its fields, which may own
  further boxes. For a recursive type that would use stack in proportion to the structure's depth.
- **Tail iteration.** The compiler finds the *tail* of `T`: the last field (in destruction order) that
  needs drop, followed through structs, and through enum variants' last payloads, until it
  reaches a `Box<T>` again. If there is one, `Box<T>`'s glue becomes a loop:
  - destroy everything of the current `T` except the tail, in the normal order;
  - take the tail box's pointer (null for variants that end elsewhere);
  - free the current memory, then continue with the tail's contents.
- Only the moment the memory is freed changes: the current node is freed before the tail is
  destroyed instead of after. No program can observe that difference; every `drop` block runs in
  the order §14.5 defines.
- Consequence: lists (`Cons(x, Box<List>)`) are destroyed in constant stack depth. Trees use
  stack in proportion to the number of non-tail steps on a path (the left children in
  `{ left, right }`). Balanced trees therefore use logarithmic depth, and a left-degenerate tree
  still recurses (R-9 records this).

## 19. Regions (M4)

`Arena`, and in later increments `FixedBufferAllocator`, `ScratchFrame`, `Pool<T>` and
`PoolBox<T>`, are built in (ADR 0004). The safety rules are R1–R4 (`ownership.md`): an
allocation borrows its region.

### 19.1 Scoped types (M4a)
A type is **scoped** if a value of it may hold loans:
- references and slices (§13, §16);
- `Result<T, E>` whose `T` or `E` is scoped (a *scoped result*);
- the scoped region values: `FixedBufferAllocator` (§19.4), `ScratchFrame` (§19.5) and
  `PoolBox<T>` (§19.6).

Every rule for references applies to scoped types (N0246):
- they may appear only in parameters, locals with an initialiser, and return types;
- never in fields, payloads of user enums, array elements, box contents or constants.

References and slices must be `let` locals, because assigning to a reference writes through it.
*Since M4c,* other scoped values (scoped results and region values) may be `var`: assigning one
replaces it, like any value. For the borrow checker, the local then holds the loans of every value
it is assigned (a sound over-approximation).

A function returning a scoped type needs return sources: the default rule (one reference
parameter) or a `borrow(...)` list (§13.5). In this rule, "reference parameter" now means
"parameter of a scoped type", and the receiver `this` counts when it is borrowed.

The borrow checker treats every local of a scoped type as holding loans (`lifetimes.md` §3).
Moving or copying a scoped value moves or copies its loans, as for references. A scoped
result's loans reach the `Ok` payload through `?`, `!` and `match`.

### 19.2 `Arena` (M4a)
```ceesafe
fn process(input: Slice<u8>) -> Result<u64, AllocError> {
    using arena = Arena.create(SystemHeap {}, 65536u64);
    let counts = arena.allocArray(256usize, 0u32)?;   // SliceMut<u32>, a loan of `arena`
    for (b in input) { counts[b as usize] += 1u32; }
    var distinct: u64 = 0;
    for (c in counts) { if (c != 0u32) { distinct += 1; } }
    return Ok(distinct);                              // a u64, not arena memory
}                                                     // the arena frees all its memory here
```
- `Arena` is a built-in move type that needs drop. It is not scoped: it owns its memory, which
  it obtains from a free-standing allocator.
- `Arena.create(a, chunkSize)`, with `a` an allocator value (`SystemHeap {}`, `FailingAllocator {}`)
  and `chunkSize: u64`, makes an empty arena. It cannot fail: memory is obtained in chunks of
  `chunkSize` bytes when needed. An allocation larger than a chunk gets a chunk of its own.
- **Allocation** (R1). Both methods take `borrow this` and return a scoped result that carries a
  **shared loan of the arena**:
  - `arena.alloc(v)`: `Result<borrow mut T, AllocError>`, with `v` moved into fresh arena memory;
  - `arena.allocArray(n, fill)` with `n: usize`: `Result<SliceMut<T>, AllocError>`, `n` copies
    of `fill`. `T` must be Copy. `n` may be 0 (an empty slice); `n × size(T)` beyond the address
    space is `Err(OutOfMemory)`.

  The returned references are mutable: each allocation is fresh memory that nothing else refers to.
- **R2:** `T` must not need drop, and must not be scoped (no references or slices in arena memory,
  E3). Both are N0264.
- `arena.reset()` takes `borrow mut this` and releases all memory. Because it is a mutable borrow,
  it is rejected while any allocation is live (B1/B2). Dropping or moving the arena is likewise
  rejected while allocations are live.
- Destroying an arena releases all its memory at once (bulk release). Since no allocation
  needs drop (R2), this equals destroying each allocation.
- Failure: if a chunk cannot be obtained from the allocator, the allocation returns
  `Err(AllocError.OutOfMemory)`; the arena stays usable.

### 19.3 What cannot escape (M4a, the R1 consequences)
- An allocation cannot be returned from the function that owns the arena: N0244, like any
  reference to a local.
- It cannot be stored in a box, a struct, an enum payload or an array: those cannot hold scoped
  values (N0246).
- It cannot outlive the arena: a `reset`, drop, move or scope end of the arena while the
  allocation is live is N0240–N0243.

Diagnostics: N0264 (a type that a region cannot hold).

### 19.4 `FixedBufferAllocator` (M4b)
```ceesafe
fn countWords(text: Slice<u8>) -> Result<u32, AllocError> {
    var storage = [0u8; 4096];
    let fba = FixedBufferAllocator.over(mut storage);   // a mutable loan of `storage`
    let spans = fba.allocArray(64usize, Span { start: 0u32, len: 0u32 })?;
    ...
}
```
- `FixedBufferAllocator.over(mut s)` makes an allocator over caller-provided bytes. `s` is a
  mutable place of type `[u8; N]`, or a `SliceMut<u8>`. The allocator holds a **mutable loan**
  of the storage, so the storage cannot be read or written while the allocator or any of its
  allocations is live.
- `FixedBufferAllocator` is **scoped** (§19.1): a `let` local, a parameter or a return type with
  sources, never stored. It is a move type without drop; nothing is freed.
- `alloc(v)` and `allocArray(n, fill)` work as for `Arena` (§19.2), including R2 (N0264). Each
  allocation is aligned within the storage. When the remaining bytes are not enough, the result
  is `Err(AllocError.OutOfMemory)`; the storage is never grown. There is no `reset`: to reuse the
  storage, let the allocator end and make a new one.
- An allocation's loans include the allocator's loan of the storage, so the storage stays
  borrowed while any allocation is live, even after the allocator's last use.

### 19.5 Scratch frames (M4b; ADR 0004)
```ceesafe
fn render(n: usize) -> Result<u64, AllocError> {
    using frame = Scratch.enter();               // a root frame
    let buf = frame.allocArray(n, 0u8)?;         // scoped to `frame`
    {
        using inner = frame.enter();             // a nested frame borrows `frame` mutably
        let tmp = inner.alloc(1u64)?;
        ...
    }                                            // `inner`'s memory is released here
    return Ok(buf.length as u64);
}                                                // `frame`'s memory is released here
```
- Scratch memory is a runtime-owned stack of heap chunks (`SystemHeap`); it is not the native stack.
- `Scratch.enter()` makes a **root frame**. Only one root frame may be live at a time (per
  thread; v0 is single-threaded): entering a second one **panics** ("a scratch frame is already
  active"). Code that needs scratch memory below a frame receives the frame
  (`borrow ScratchFrame`) and nests.
- `frame.enter()` makes a **nested frame**. For the borrow checker a frame has two parts: its
  *memory*, which allocations borrow, and its *allocation right*, which every allocation call
  uses briefly. The rules:
  - an allocation result carries a shared loan of the frame's memory;
  - each `alloc`/`allocArray` call needs a shared borrow of the allocation right for the call
    only;
  - `enter()` takes a **mutable** loan of the parent's allocation right, which the nested frame
    carries.

  While a nested frame is live, the parent therefore cannot allocate or nest again (B1), but
  allocations already made from the parent stay usable: they lie below the nested frame's
  memory. Destroying or moving a frame conflicts with both parts. This makes frames strictly LIFO.
- Calling a frame method needs a frame that is a place (`using frame = …`): N0247 otherwise.
- `ScratchFrame` is scoped (§19.1) and needs drop. Destroying a frame releases everything
  allocated from it (and from frames nested in it), and returns chunks to the heap.
- `alloc(v)` and `allocArray(n, fill)` work as for `Arena`, including R2. A failure to obtain a
  chunk is `Err(AllocError.OutOfMemory)`.
- Frames are bound with `using` (R3; §14.6): nesting borrows the frame mutably, and a scoped
  value cannot be a `var`. Since frames are scoped, they cannot be returned from the function
  that entered them.

### 19.6 `Pool<T>` and `PoolBox<T>` (M4c)
```ceesafe
struct Particle { x: i32; v: i32; }

fn simulate() -> Result<i32, AllocError> {
    using pool: Pool<Particle> = Pool.create(SystemHeap {}, 64u32);
    var a = pool.take(Particle { x: 0, v: 2 })?;     // PoolBox<Particle>: a loan of `pool`
    let b = pool.take(Particle { x: 10, v: -1 })?;
    a.x += a.v;                                      // through the box
    return Ok(a.x + b.x);
}                                                    // b, a, then the pool are destroyed
```
- `Pool<T>` is a built-in allocator for up to `capacity` values of one type `T`.
  - `Pool.create(a, capacity)`, with `a` an allocator value and `capacity: u32`, makes an empty
    pool. `T` comes from the expected type (`let pool: Pool<T> = …`); without one it is N0260.
  - It cannot fail: the slots are allocated from `a` at the first `take`.
  - `Pool<T>` is a move type that needs drop; destroying it returns the slots to `a`. It is not
    scoped.
- `pool.take(v)` takes `borrow this` and returns `Result<PoolBox<T>, AllocError>`: `v` moves into
  a free slot. The result is `Err(AllocError.OutOfMemory)` when all slots are taken, or when the
  slots cannot be allocated; `v` is then destroyed.
- `PoolBox<T>` owns one slot and its value, like a `Box` (§18.2):
  - auto-deref, the box's own mutability, borrowing its contents, and moving fields out of it
    (§18.3) all work as for `Box`;
  - it is a move type that needs drop: destroying it destroys the `T` (its drop glue), then
    returns the slot to the pool. Values are destroyed one by one, so **`T` may need drop**: R2
    does not apply to pools;
  - it is **scoped**: it carries the shared loan of the pool from `take`. It cannot be returned
    past the pool, stored in a field or box, or outlive the pool (§19.3);
  - `T` must not be scoped (E3; N0264).
- Destroying the pool while a `PoolBox` from it is live is rejected (B2), so every slot is free
  when the pool is destroyed.

## 20. Generics (M6a)

```ceesafe
struct Pair<T> {
    first: T;
    second: T;
    fn swap(this) -> Pair<T> => Pair { first: this.second, second: this.first };
}

enum Maybe<T> { Nothing, Just(T) }

fn largest<T>(p: borrow Pair<T>, pick: bool) -> borrow T {
    if (pick) { return borrow p.first; }
    return borrow p.second;
}

fn main() {
    let p = Pair { first: 1, second: 2 };        // Pair<i32>, inferred from the fields
    let q = p.swap();
    let m: Maybe<u8> = Maybe.Just(7u8);
    let r = largest(p, true);                    // T = i32, inferred from the argument
    let s = largest<i32>(q, false);              // explicit type argument
}
```

### 20.1 Declarations
```text
TypeParams = '<' Identifier (',' Identifier)* '>'
FunctionDecl = 'fn' Identifier TypeParams? '(' … ')' …
StructDecl   = 'struct' Identifier TypeParams? '{' … '}'
EnumDecl     = 'enum' Identifier TypeParams? '{' … '}'
```
- A top-level function, struct or enum may declare **type parameters**. Inside the declaration,
  each parameter is a type. Methods of a generic struct see the struct's parameters.
- **Generic methods** (ADR 0064 §1). A struct's method or static function may declare type parameters of its own:
  `fn add<S: Shape>(borrow mut this, s: borrow S)`. It sees the struct's parameters too. It is checked once with its
  parameters opaque (§20.3) and instantiated per call (§20.4), as a top-level generic function is. Its type
  arguments are inferred from the arguments after the receiver, or written after the name: `canvas.add<Square>(s)`,
  `Pair<i32>.make<u8>(…)`. Type arguments on a call of a function without type parameters of its own are N0209.
  An interface's methods, and so an `impl I for T` block's, cannot have type parameters (N0216): a `dyn` table
  holds one function per method.
- Names must be distinct, and must not shadow items or built-in types (N0201).

### 20.2 Using generic items
- **Types:** `Pair<i32>`, `Maybe<Pair<u8>>`. A generic struct or enum must always be given all its
  type arguments in a type (N0204).
- **Calls:** `largest<i32>(p, true)` gives the arguments explicitly. `largest(p, true)` infers
  them from the argument types: each parameter type is matched against its argument's type. An
  integer literal argument is `i32` unless the parameter type fixes it. A parameter that the
  arguments do not determine is N0265 ("write the type arguments").
- **Values:** `Pair { first: 1, second: 2 }` infers `T` from the field values, or takes it from the
  expected type. `Pair<u8> { … }` gives it. Likewise `Maybe.Just(7)` infers from the payload, and
  `Maybe.Nothing` needs an expected type or explicit arguments: `Maybe<u8>.Nothing`.
- **Static functions:** `Pair<i32>.make(…)`.

### 20.3 Checking generic code once
- A generic declaration is checked **once, for all type arguments**. Each type parameter is an
  opaque type, about which only what holds for every type is known:
  - it is a **move type** that **may need drop**. A value of it can be moved, borrowed,
    passed, returned, stored in fields of the declaring type, and matched as a payload;
  - nothing else is allowed: no operators, no fields, no methods, no literals, no
    `==` (N0207, N0234, N0205). Interfaces (§21) add operations through bounds.
- Move checking, borrow checking and reachability run on the generic code too. A body that
  would be wrong for some type argument is rejected at the definition, **whether or not it is ever
  instantiated** (for example, using a `T` twice is N0250).
- Generic declarations that are never used produce no C code.

### 20.4 Instantiation (monomorphisation)
- Every distinct use (`largest<i32>`, `Pair<Res>`) is an **instantiation**: a copy of the
  declaration with the type arguments substituted. It is checked again and compiled to C.
- An instantiated struct or enum is an ordinary type for every other rule. It is Copy iff its
  substituted fields or payloads are, and needs drop iff one of them does. `Pair<i32>` is Copy;
  `Pair<Res>` is a move type.
- The layout is that of the substituted declaration. Recursion is checked per instantiation
  (N0227).
- Instantiation is depth-limited: a chain of more than 64 nested instantiations (polymorphic
  recursion such as `f<T>` calling `f<Box<T>>`) is N0266.
- **Unit payloads** (ADR 0074 §2.1). `Result<void, E>.Ok` has no payload (§17.1), but a pattern may bind one name
  to it, and `Ok(e)` may pass one argument of type `void`, so `Ok(v) => Ok(v)` means the same at `T = void` as at
  any other `T`, in a generic body and written out by hand:
  - A name bound so is a **unit local**: it has type `void` and holds nothing. `let v = r?;` (or `r!`) with
    `r: Result<void, E>` declares one too, as does `let w = v;` of one. A `let` of a call of a `void` function is
    still N0224, and a `var` cannot hold a unit.
  - Reading a unit local is a unit value. It may be passed where `void` is expected: as `Ok`'s payload, and as
    `return v;` in a `void` function, which is `return;`. Elsewhere it is a `void` value of the usual rules: N0205
    where another type is expected, N0211 when assigned.
  - `Ok(e)` with any `void` `e` evaluates `e` for its effects, then builds the payload-less `Ok`. Any other type is
    N0209.
  - The checker erases units: a unit local is no HIR local, a unit read no operand, and nothing is captured, moved,
    borrowed or destroyed for one. NIR, the C backend and the interpreter never see them.
  - Not included: user generic enums instantiated at `void` (N0226), and `Some(v)` with `void`.
- **An instantiation that does not check is an error at its use** (M15 S1). §20.3's checking
  covers every type argument; a `void` one's payloads are unit payloads (above). A scoped type argument
  the body cannot take is N0274 (§25.4). Otherwise the copy's errors are usually explained by an
  error at the use (an unmet bound, N0268; a move of C state, N0294). If, after the ownership
  checks, no error explains them, the copy is N0222 at the use, naming the type arguments and its
  first error. Its body is never compiled. Before M15, such a copy reached code generation with its
  errors unreported, and ceesafec crashed (#2).

Diagnostics: N0265 (type arguments that cannot be inferred), N0266 (instantiation too deep), N0222
(the instantiation's body does not check for its type arguments).

## 21. Interfaces (M6b)

```ceesafe
interface Shape {
    fn area(borrow this) -> i64;
}

struct Square { side: i64; }
impl Shape for Square {
    fn area(borrow this) -> i64 => this.side * this.side;
}

fn total<S: Shape>(shapes: Slice<S>) -> i64 {
    var t = 0i64;
    for (i in 0..shapes.length) { t += shapes[i].area(); }
    return t;
}
```

### 21.1 Declarations
```text
InterfaceDecl = 'interface' Identifier TypeParams? '{' MethodSig* '}'
MethodSig     = Attribute* 'fn' Identifier '(' Receiver (',' Param)* ')' ('->' Type)? ';'
ImplDecl      = 'impl' TypeParams? Type 'for' Type '{' FunctionDecl* '}'
InherentImpl  = 'impl' TypeParams? Type '{' FunctionDecl* '}'
```
- **`impl S { … }`** (an inherent impl, ADR 0064 §1) adds methods and static functions to a struct `S`, as if they
  were written in its body: the same visibility, the same "no member named twice" rule (N0228), and generic methods
  (§20.1). It is written in the package that declares `S`. Its target names `S` with `S`'s own parameters, in order:
  `impl<T: B> S<T> { … }` for `struct S<T: B>`, `impl<region r> V<r> { … }` for `struct V<region r>`. Anything else is
  N0267.
- An **interface** lists method signatures. Every method has a receiver (`this`, `borrow this`,
  `borrow mut this`). An interface may be generic (`interface Comparable<T>`). Its methods
  cannot have type parameters of their own, and v0 has no `Self` type.
- **`impl I for T { … }`** implements interface `I` (with its type arguments, `I<X>`) for a
  type `T`: a struct, an enum, or a built-in type such as `i32`. A **generic impl** (ADR 0054),
  `impl<T: B, U> I<…> for C<…T…, …U…> { … }`, implements `I` for every instance of the generic struct or enum `C` that
  its target matches and whose arguments meet the bounds; each of its type parameters appears in the target (N0267),
  and the target is a generic struct or enum (N0267). Its methods are checked once, with the parameters opaque, and
  instantiated per instance like a generic struct's: a call on the concrete type, a call through a bound and a `dyn`
  table all use the instance. A type with region parameters is implemented with them named, `impl<region r> I for
  S<r>` (§25.3), and no other scoped type implements an interface (N0267).
  - The block must define exactly the interface's methods, each with the same receiver kind,
    parameter types and return type, after the interface's type arguments are substituted
    (N0267). It may not define other functions.
  - Each type may implement each interface instantiation at most once (N0267).
  - **Overlap.** Beside a generic impl, no other impl of the same interface may have a target of the same struct or
    enum, generic or not (N0267): `impl I for Wrap<u8>` and `impl<T> I for Wrap<T>` are refused together. There is no
    specialisation.
  - **The orphan rule.** An impl is written in the package that declares the interface or the target's struct or
    enum (N0267). The built-in types, and everything lib/core and lib/std declare, are the libraries': a program
    implements its own interfaces for them, and lib/std's interfaces for its own types.
  - An `impl` block's methods are ordinary methods of `T`. `x.area()` works on any `Square`, and
    their names must not clash with `T`'s other methods (N0228).
- Interfaces are not types: an interface is a bound (§21.2), and since M13 also the `I` of a `dyn I` type behind a
  pointer, which dispatches at run time (§37; ADR 0015).
- A method signature may carry an effect list, `@effects() fn visit(borrow mut this);`, which bounds every `impl` of
  it (§37.3).

### 21.2 Bounds
- A type parameter may have bounds: `<T: Shape>`, `<T: Comparable<T> + Copy>`.
- **Inside generic code**, a value whose type is `T` (or a reference to one) may call the methods
  of `T`'s bounds, with the signatures the interfaces give (after substitution). A call that no
  bound provides is N0234. If two bounds provide the same method name, the call is ambiguous
  (N0267).
- **`Copy`** is a built-in bound: a `T: Copy` is a Copy type in the generic code (it can be used
  twice and stored in arrays) and never needs drop. Only Copy types satisfy it.
- **At every instantiation** (a call, type or value that gives `T`), each bound must hold: the
  argument implements the interface (for a type parameter argument, its own bounds include it),
  or is Copy for `Copy`. Otherwise it is N0268, reported where the instantiation is written.
- Dispatch is static: an instantiation calls `T`'s `impl` method directly (monomorphisation).

### 21.3 Slices of move types (correction to §16.1)
- `Slice<T>` and `SliceMut<T>` may have any non-scoped element type, not just Copy ones. A slice
  only views elements: reading one by value still needs a Copy type (N0251 otherwise), but
  borrowing one (`borrow s[i]`, `s[i].method()`) does not. Arrays still need Copy elements
  (§16.1), since an array owns its elements and v0 tracks no per-element moves.

Diagnostics: N0267 (an `impl` that does not match its interface, a duplicate `impl`, or an
ambiguous bound method), N0268 (a type argument that does not satisfy a bound).

## 22. Optionals and string views (M6c)

### 22.1 Optionals `T?`
```ceesafe
fn find(xs: Slice<i32>, wanted: i32) -> usize? {
    for (i in 0..xs.length) { if (xs[i] == wanted) { return i; } }   // i converts to usize?
    return null;
}

fn firstTwo(xs: Slice<i32>) -> usize? {
    let a = find(xs, 1)?;                  // null propagates
    return a + 1;
}

fn main() {
    let numbers = [3, 1, 2];
    match (find(numbers[..], 2)) {
        Some(i) => { Console.writeLine(i); }
        null => { Console.writeLine("absent"); }
    }
    let i = find(numbers[..], 3)!;         // panics on null
}
```
- `T?` (postfix `?` in a type) is an optional `T`, for any non-`void` `T`. It is a built-in enum with
  two variants, in this order: `null` (no payload) and `Some(T)`. Everything in §15.1 applies:
  it is Copy iff `T` is, needs drop iff `T` does, and is scoped iff `T` is (`borrow T?`).
- **Optional references and references to optionals** (M11, G6). `?` applies to the whole type before it, so
  `borrow T?` is an *optional reference*, `(borrow T)?`. A *reference to an optional* is written with parentheses:
  `borrow (T?)`, `borrow mut (Box<Node>?)`. A type may be parenthesised anywhere a type is written (grammar.md
  §2). Diagnostics print the two as written: `borrow i32?` and `borrow (i32?)`. A reference to an optional is a
  reference like any other (§13): a match on it binds references to the payload, `r == null` reads the referent,
  and `r = v` through a `borrow mut (T?)` replaces the optional, as in
  ```ceesafe
  fn append(slot: borrow mut (Box<Node>?), value: i32) {
      match (slot) {
          Some(node) => { append(mut node.next, value); }
          null => { slot = Some(Box.create(Node { value: value, next: null })!); }
      }
  }
  ```
- **Values.**
  - `null` needs an expected optional type (N0260).
  - `Some(e)` makes a present value; with an expected `T?`, `e` is checked against `T`, otherwise
    `T` is `e`'s type.
  - Where a `T?` is expected (an initialiser, an argument, a return value, a field), a value of
    type `T` converts to `Some(value)` implicitly.
- **Matching.** Patterns `null`, `Some(x)` and `Some(_)` are written without a type prefix; the
  rules of §15.2 and §18.4 apply.
- **Comparisons.** `o == null` and `o != null` test for absence, for any `T` (the value is only
  read, not consumed). So do `o.isNull()` and `o.isSome()` (ADR 0072), `bool`s; any other method on an optional
  is N0234. Other comparisons of optionals are not defined (N0207).
- **`a ?? b`** (ADR 0072) is `a`'s value, or `b` when `a` is `null`. `a` is a `T?` (N0262 otherwise) and `b` is
  checked against `T`; the whole is a `T`. If `b` is itself a `T?` of `a`'s type, the whole is that `T?`, so a
  chain may end in an optional (`find(a) ?? find(b)`). `b` is evaluated only when `a` is `null`, so `?? fail("…")`,
  a call of type *never*, stops with a message. `a` is consumed, as by `match (a)`: a non-Copy payload moves out.
  `??` binds less tightly than every other binary operator, `||` included, and is right-associative
  (grammar.md §2.1); beside an unparenthesised `||` or `&&` it is N0100.
- **`o!`** is the value, or **panics** with "unwrapped null".
- **`o?`** is the value, or returns `null` from the enclosing function, which must return an
  optional (N0261). `?` on an optional in a function returning a `Result` is N0261 in v0
  (`language-spec.md` Ex11).
- **Layout.** In general, like any enum: `{ tag; T }`. Since M9e′, an optional whose payload is a pointer
  that is never null is that pointer alone (the *niche* layout; `language-spec.md` §2): null is `null`, and any
  other value is `Some`. That covers `Box<T, A>?`, except a pool box (two pointers), and `borrow T?` and
  `borrow mut T?`, except slices and string views (a pointer and a length). Such a pointer is never null:
  - allocation returns a non-null pointer or fails (`ceesafe_alloc` gives a zero-sized object one byte);
  - a stack-placed box points to its slot;
  - a reference points to an object, and one made from a raw pointer must be non-null (`unsafe.md` §2, U2).

  An optional of such an optional (`Box<T>??`) has the general layout around the niche one. The layout changes no
  semantics, and optionals do not cross the C boundary (§28.3), so no C code sees it. `Box<Node>?` is 8 bytes
  rather than 16 on a 64-bit target.

### 22.2 `StringView`
```ceesafe
fn greeting(loud: bool) -> StringView {
    if (loud) { return "HELLO"; }
    return "hello";
}

fn main() {
    let s: StringView = "CeeSafe language";
    Console.writeLine(s);
    Console.writeLine(s.length);                 // 13 (bytes)
    let first = s[0..4];                         // "CeeSafe": a view of s
    Console.writeLine(first == "CeeSafe");
    for (b in s.bytes()) { … }                   // Slice<u8>
}
```
- `StringView` is a **shared, read-only view of UTF-8 text** `(pointer, length in bytes)`. It is
  scoped like `Slice<T>` (§16.1), and Copy.
- **String literals** have type `StringView` (their bytes are static data). A literal's view holds
  no loans. A function may therefore return a `StringView` without scoped parameters (like
  `greeting` above). The return check still rejects any view that borrows a local (N0244).
- **Operations.**
  - `s.length` is the length in bytes (`usize`);
  - `s[i]` is the byte at `i` (`u8`, bounds-checked);
  - `s.bytes()` is the same bytes as a `Slice<u8>`;
  - `s[lo..hi]` (any bound may be omitted) is a `StringView` of bytes `lo` up to `hi`. Besides the
    range check of §16.3, both bounds must fall on character boundaries (not inside a UTF-8
    multi-byte sequence). Otherwise it **panics** with "string slice not on a character
    boundary";
  - `==` and `!=` compare the bytes;
  - `Console.write(s)` and `Console.writeLine(s)` print it;
  - searching, trimming, splitting and parsing numbers: §16.6.
- `for (b in s)` is not defined: iterate over `s.bytes()` (characters come with `String`
  methods later).
- **From bytes** (M15 S6): `StringView.fromUtf8(bytes)` (`bytes: Slice<u8>`, or a `Vector<u8>` lent
  as one) is a `StringView?`.
  - If the bytes are well-formed UTF-8 (the Unicode Standard's Table 3-7), it is a view of those same
    bytes; nothing is copied.
  - Otherwise it is `null`. That covers overlong forms, surrogates, values above U+10FFFF and
    truncated sequences.
  - The view borrows the bytes: it holds the argument's loans, so the bytes cannot change while it
    is used.
  - It is how text read with `File.read` or `Console.read` becomes a `StringView`, and through
    `append` a `String`, so the UTF-8 invariant above still holds.
  - It is available in every profile, through the core runtime's `ceesafe_utf8_valid`.

Diagnostics: none new. `null` without an expected type is N0260; `?` and `!` on non-optional,
non-result values are N0262.

## 23. `Vector<T, A>` and `String<A>` (M6d)

### 23.1 `Vector<T, A>`
```ceesafe
fn sum(xs: Slice<i32>) -> i32 {
    var total = 0;
    for (x in xs) { total += x; }
    return total;
}

fn main() -> i32 {
    var v = Vector<i32>.create();          // no allocation until the first push
    v.push(1)!;                             // Result<void, AllocError>
    v.push(v.length as i32)!;               // two-phase borrow (B5)
    let r = borrow v[0];
    // v.push(3)!;                          // error[N0240]: `v` is borrowed by `r`
    Console.writeLine(r);
    v.push(3)!;                             // fine: `r` is dead
    Console.writeLine(sum(v));              // a vector argument for a slice parameter
    return 0;
}
```
- `Vector<T, A>` owns a growable, contiguous buffer of `T` values obtained from allocator type `A`
  (H2). `Vector<T>` means `Vector<T, SystemHeap>`. `A` must be an allocator type (N0263). `T` is
  any value type except `void` and scoped types (N0246; references in aggregates are OD-3).
- A vector is a **move type** that needs drop and is never Copy. It is not scoped. Destroying it
  destroys the elements from index 0 upward, then returns the buffer to `A`.
- **Creation** (built-in functions on the type; the type arguments come from the explicit list or,
  without one, from an expected `Vector<T, A>` or `Result<Vector<T, A>, _>`; N0260 otherwise):
  - `Vector<T>.create()`: an empty vector. It allocates nothing;
  - `Vector<T>.withCapacity(n)` (`n: usize`): `Result<Vector<T, A>, AllocError>`, an empty vector
    with room for `n` elements (`n == 0` allocates nothing).
- **Members:**
  - `v.length` and `v.capacity` (`usize`);
  - `v[i]` is element `i`, bounds-checked (panic "index out of bounds"). It is a place, like an
    element of a slice: it can be read (Copy `T`), borrowed (`borrow v[i]`, `borrow mut v[i]`) and
    assigned (`v[i] = x` destroys the old element). Writing needs a mutable vector: a `var`, or
    one reached through `borrow mut`. Moving an element out is rejected (N0251), as for a slice;
  - `v.push(x)`: `Result<void, AllocError>`. It appends `x`, growing the buffer when it is full
    (new capacity: twice the old, at least 4). If the allocation fails, `x` is destroyed, the
    vector is unchanged, and the result is `Err(AllocError.OutOfMemory)`;
  - `v.pop()`: `T?`, the last element moved out, or `null` if the vector is empty;
  - `v.clear()`: destroys every element; the capacity stays;
  - `v.truncate(n)` (M11 G8c, `n: usize`): if `n < v.length`, destroys the elements from index `n` up, lowest
    first, and the length becomes `n`; otherwise nothing changes. The capacity stays;
  - `v.extend(s)` (`s: Slice<T>`), `v.resize(n, fill)` (`n: usize`, `fill: T`) and `v.reserve(extra)` (`extra: usize`),
    ADR 0071 §2.1: `Result<void, AllocError>`. Each grows the buffer at most once (as `push` grows it, to at least
    the length needed) and leaves the vector unchanged if that fails. `extend` appends a copy of `s`; `resize` appends
    copies of `fill` up to length `n`, or truncates to `n`; `reserve` makes room for `extra` more elements and
    changes nothing else. `extend` and `resize` need a Copy `T` (N0259). They take `borrow mut this` and are
    two-phase (B5), so `v.resize(v.length + 1, 0)` is allowed; `v.extend(v.view())` is not (N0241);
  - for a `Vector<u8, A>`, byte order, searching and parsing (§16.6), on its elements;
  - `v.swap(i, j)`, and the methods taking closures (`sortBy`, `forEach`, `any`, `all`, `retain`): §16.5 and §36.4;
  - `v.view()`: the elements as a `Slice<T>`, and `v.viewMut()` as a `SliceMut<T>`;
  - a `Vector<T>` argument for a `Slice<T>` parameter is borrowed as `v.view()`, and `mut v` for a
    `SliceMut<T>` parameter as `v.viewMut()` (like a box's contents, §18.2);
  - `for (x in v)` is `for (x in v.view())` (§16.4).
- **Calls take `this` like methods:** `push`, `pop`, `clear` and `truncate` take `borrow mut this`, and their
  arguments are evaluated before the mutable loan starts (two-phase, B5, `lifetimes.md` §5).
  `length`, `capacity`, `view` and element reads take `borrow this`.
- **Invalidation (H2, acceptance criterion 1 of M6):** an element borrow, a view, or anything
  derived from them is a loan of the vector. While one is live, `push`, `pop`, `clear`, element
  writes, `viewMut`, moving the vector and destroying it are rejected (N0240, N0241, N0243),
  exactly as for a place and its fields. The vector makes **no address-stability promise**:
  growth moves the elements.
- No `==`, no `Copy`.
- **Layout (v0):** `{ T *ptr; u64 length; u64 capacity }` (24 bytes on LP64). An empty vector's
  pointer is a valid, aligned, never-dereferenced address (not null), so views of it are valid.

### 23.2 `String<A>`
```ceesafe
fn greet(name: StringView) -> Result<String, AllocError> {
    var s = String.withCapacity(name.length + 7)?;
    s.append("Hello, ")?;
    s.append(name)?;
    return Ok(s);
}

fn main() -> i32 {
    let msg = greet("CeeSafe")!;
    Console.writeLine(msg.view());          // a StringView borrowed from msg
    return 0;
}
```
- `String<A>` owns UTF-8 text in a growable buffer (`String` means `String<SystemHeap>`). It is a
  move type that needs drop, like `Vector<u8, A>`, but a distinct type.
- **UTF-8 invariant:** text only enters a string through `append` of a `StringView`, which is
  always valid UTF-8 (§22.2), so a string's bytes are always valid UTF-8. Its bytes cannot be
  written: `s[i] = b` is rejected (N0211), and `borrow mut s[i]` and mutable slices of a string
  (N0248).
- **Creation:** `String.create()` (no allocation) and `String.withCapacity(n)`:
  `Result<String<A>, AllocError>`; `String<A>.create()` for another allocator.
- **Members:**
  - `s.append(text)` (`text: StringView`): `Result<void, AllocError>`, two-phase like `push`.
    Growth is as for vectors. On failure the string is unchanged;
  - `s.appendNumber(n)` (M11, G4c; `n` of any integer type, or a reference to one): appends `n` in decimal, with a
    `-` before a negative value and no leading zeros or `+`: `0`, `-128`, `18446744073709551615`. It is
    `Result<void, AllocError>`, two-phase and unchanged on failure, as for `append`. An integer literal argument has
    its usual default type (§4). Any other argument type is N0207;
  - `s.clear()`, `s.length` (bytes), `s.capacity`;
  - `s.view()`: the text as a `StringView`, a loan of `s`; a `String` argument for a `StringView`
    parameter is borrowed the same way;
  - read-only access as for a view: `s[i]` (`u8`), `s[lo..hi]` (a `StringView`, boundary-checked),
    `s.bytes()`;
  - printing: `Console.writeLine(s)` prints the text of a string place.
- Appending a view of the string itself (`s.append(s.view())`) is rejected (N0240): the view is a
  live shared loan of `s` when the call's mutable loan starts.
- **Layout (v0):** as `Vector<u8, A>`.

Diagnostics: none new. The errors reuse N0205, N0207, N0209, N0211, N0240–N0251, N0260 and N0263.

## 24. `SlotMap<T, A>`, `Handle<T>` and `StableVector<T, A>` (M6e)

### 24.1 `SlotMap<T, A>` and `Handle<T>`
```ceesafe
struct Node { value: i32; next: Handle<Node>?; }

fn main() {
    var nodes = SlotMap<Node>.create();
    let a = nodes.insert(Node { value: 1, next: null })!;
    let b = nodes.insert(Node { value: 2, next: a })!;   // a handle is a plain value, not a loan
    match (nodes.get(b)) {
        Some(node) => { Console.writeLine(node.value); }
        null => { }
    }
    let removed = nodes.remove(a);                       // `a` is now stale
    Console.writeLine(nodes.get(a) == null);              // true: generation mismatch
}
```
- `SlotMap<T, A>` owns values of `T` in reusable slots from allocator type `A` (`SlotMap<T>` means
  `SlotMap<T, SystemHeap>`; N0263). `T` is any value type except `void` and scoped types (N0246).
  A slot map is a move type that needs drop, and is not scoped. Destroying it destroys the values
  it holds, then frees its storage.
- `Handle<T>` names one inserted value: a slot index and the slot's generation. It is a **Copy
  value, not a loan**: it can be stored in structs, vectors and other slot maps (unlike a
  reference, E3), and it outlives inserts, removals and moves of the map. `==` and `!=` compare
  handles. Handles are only made by `insert`.
- **Operations:**
  - `SlotMap<T>.create()`: an empty map; it allocates nothing (type arguments as for vectors,
    §23.1);
  - `m.insert(v)`: `Result<Handle<T>, AllocError>`, taking `borrow mut this` (two-phase, B5). If the
    storage cannot grow, `v` is destroyed and the result is `Err(AllocError.OutOfMemory)`;
  - `m.get(h)`: `borrow T?` (a loan of `m`), `null` if `h` does not name a live value;
  - `m.getMut(h)`: `borrow mut T?` (a mutable loan of `m`; needs a mutable map);
  - `m.remove(h)`: `T?`, the value moved out (or `null`), taking `borrow mut this`;
  - `m.contains(h)`: `bool`; `m.length`: the number of live values (`usize`).
- **Validation:** every access compares the handle's generation with the slot's. A removed value's
  handles never match again: a slot's generation changes on every insert and removal, and a slot
  whose generation would wrap around is retired instead of reused. A handle used with another map
  of the same `T` is memory-safe: it yields that map's value in that slot, or `null` (a logic
  error, as with any index).
- **Invalidation:** references from `get`/`getMut` are loans of the map, so `insert` and `remove`
  are rejected while one is live (N0241). The map may reallocate its slots: it promises stable
  **identity** (handles), not stable addresses.
- **Layout (v0):** `{ slot *ptr; u64 length; u64 capacity; u64 count; u32 free }` (40 bytes), each
  slot `{ u32 generation; u32 nextFree; T value }`; `Handle<T>` is `{ u32 index; u32 generation }`.

### 24.2 `StableVector<T, A>`
```ceesafe
fn main() {
    let names = StableVector<i32>.create();   // `let`: push needs only `borrow this`
    names.push(10)!;
    let first = borrow names[0];
    names.push(20)!;                           // fine: elements never move
    Console.writeLine(first);
}
```
- `StableVector<T, A>` is a **grow-only** sequence whose elements never move (`memory-model.md` §4):
  it allocates chunks of 8, 16, 32, … elements and never reallocates them. Ownership, element
  types, `create()` and destruction are as for `Vector` (§23.1).
- `sv.push(x)`: `Result<usize, AllocError>` (the new element's index). It takes **`borrow this`**:
  appending changes only the length and the chunk table, never an existing element, so element
  borrows stay valid across it (interior mutability; like region allocation, §19.2). On failure
  `x` is destroyed and the vector is unchanged.
- `sv.length` (`usize`) and `sv[i]` (a bounds-checked place, as for vectors: read, `borrow`,
  `borrow mut` and assignment, the last two needing a mutable vector).
- No `pop`, `clear`, views or `for` (the elements are not contiguous): iterate with
  `for (i in 0..sv.length)`. A `borrow mut sv[i]` is a mutable loan of `sv`, so `push` is rejected
  while one is live (N0241).
- Not `Sync` (concurrency is later work). **Layout (v0):** `{ void *chunks[32]; u64 length }` (264
  bytes); element `i` is in chunk `⌊log2(i + 8)⌋ − 3`. At most `8 · (2³² − 1)` elements; beyond that
  `push` fails with `OutOfMemory`.

Diagnostics: none new. The errors reuse N0205, N0209, N0240–N0251, N0260 and N0263.

### 24.3 `HashMap<K, V>` (lib/std, ADR 0052)

- **Keys.** `K: HashKey<K>`, a lib/std interface: `hash(borrow this, h: borrow mut Hasher)` feeds what identifies the
  key, and `sameAs(borrow this, other: borrow K)` compares two keys. lib/std implements it for the integer types,
  `bool`, `String` and `Vector<u8>`; a program implements it for its own types. A key type without it is N0268.
- **The hash.** `Hasher` is SipHash-1-3 keyed by 128 bits. `HashMap.create()` uses a key read once per process from the
  system's random source (`getrandom`, `arc4random_buf`, `rand_s`; fixed in the interpreter and CosmoOS userland), so
  colliding keys cannot be chosen against a map. `HashMap.withSeed(k0, k1)` fixes the key.
- **Operations:**
  - `put(key, value)`: `Result<V?, AllocError>`, the old value if the key was there;
  - `get(borrow key)`, `getMut`: an optional reference;
  - `contains`, `remove(borrow key)` (`V?`), `clear`, `length()`;
  - `keyAt(i)` and `valueAt(i)` for `i` in `0..length()`.
- **Order.** Iteration is in insertion order, except that a removal moves the last entry into the removed one's place.
  So the order never depends on the hash or its key, and a program's output is the same everywhere.
- **Failure.** Only `put` allocates. A failed `put` leaves the map as it was (docs/limits.md §2): a new entry's table
  grows first, and a replaced value goes in at the end before it is swapped into place.
- **Methods of `String` and `Vector`.** A call naming no built-in method reaches an impl's method (ADR 0052 §3.3), so
  `impl I for String` is usable.
- **The allocator.** The system heap.

## 25. Region parameters (M6x; OD-3, ADR 0006)

```ceesafe
enum Kind { Word, Number }
struct Token<region src> { kind: Kind; text: StringView<src>; }

struct Lexer<region src> { input: StringView<src>; position: usize; }

fn next<region src>(lexer: borrow mut Lexer<src>) -> Token<src>? { … }   // the token views the input

fn split<region r>(line: StringView<r>) -> Result<Vector<StringView<r>>, AllocError> { … }
```

### 25.1 Declaring regions
- Structs, enums and functions may declare **region parameters** before their type parameters:
  `struct Lexer<region src, T>`, `fn f<region a, region b, T>(…)`. In a parameter list, `region`
  followed by a name declares a region; `region` alone is still an ordinary name.
- A method may use its struct's regions. `static` is the built-in region of static data (string
  literals); it outlives everything.

### 25.2 Region arguments
- A type with region parameters takes region arguments first, then its type arguments:
  `Token<src>`, `Pair<r, i32>`. The built-in scoped types take one region optionally:
  `StringView<r>`, `Slice<r, T>`, `SliceMut<r, T>`, and references `borrow(r) T`,
  `borrow mut(r) T`.
- Region arguments may be **omitted** (all of them, never some): in function bodies they are
  inferred, in signatures elided (§25.6), and in fields they are simply not checked (regions are
  erased, §25.5).
- A written region must be in scope: a region parameter of the enclosing item or function, or
  `static`. In a function's result, `borrow(p)` may also name a parameter, as in v0 (§13.5).
  Otherwise, or with the wrong number of arguments: N0273. `static` in a parameter's type is N0273 too (§25.6, ADR
  0068).

### 25.3 Scoped types
A type is **scoped** (§19.1) iff it is a reference, slice or `StringView`, a scoped region value,
a `PoolBox`, an optional or result with a scoped part, a struct or enum that declares region
parameters, an instance of a generic struct or enum with a scoped type argument, or a `Vector`,
`StableVector` or `SlotMap` with a scoped element. `Handle<T>` is not scoped (it holds no loan).

A struct or enum with region parameters implements an interface as `impl<region r> I for S<r> { … }` (ADR 0054): the
impl's region names are in scope in its target and in its methods' signatures and bodies, as a struct's are in its own
methods. Such a type satisfies bounds (`fn f<B: Body>(b: borrow mut B)`) and is used statically; it does not coerce to
`dyn I` (§37.2).

### 25.4 Where scoped values may be stored
- **Fields and payloads:** a struct field or enum payload may have a scoped type only if the
  struct or enum declares at least one region parameter, or the type is a type parameter (its
  instances are scoped then, §25.3). Otherwise: N0270.
- **Generic instances:** a user generic type or function may be instantiated with scoped type
  arguments. If the generic needs an unscoped type there (it boxes `T`, makes an array of `T`, …),
  the instantiation is rejected where it is written: N0274.
- **Collections:** `Vector`, `StableVector` and `SlotMap` elements may be scoped; so may the value
  passed to a region's `alloc`, except a bare reference (N0264: region memory holds values).
- **Assignment:** assigning to a reference *local* still writes through it (§13.3). Assigning to a
  field or element of reference type replaces the reference stored there (`token.text = view`).
- **Arrays and slices** (ADR 0064 §2): their elements may be scoped values that are Copy: views (`StringView`,
  `Slice<T>`) and structs or enums holding them, but not bare `borrow T` references. All elements share the element
  type's regions, so `[StringView<r>; N]` and `Slice<a, StringView<r>>` hold loans of `r`. An array of scoped
  elements is itself scoped, like a `Vector` of them (§19.1): its loans are checked as a whole, as its places are
  (§16.3). A slice's elements live at least as long as the slice, as a Vector's view's do.
- **Still rejected (N0246):** `Box`, `Pool`, statics, constants other than views (§10), `borrow T` array and slice
  elements, and written references to references. **Region values and pool boxes** (`FixedBufferAllocator`,
  `ScratchFrame`, `PoolBox<T>`) stay out of every place this section opens up (fields, payloads,
  collection elements, type arguments of user types): they must stay pinned or LIFO in locals.

### 25.5 Regions are erased
Regions do not change types: `Token<a>` and `Token<b>` are the same type, and a value's loans are
one set (ADR 0006). Region arguments matter only for the checks of §25.2–§25.4 and for signatures.

### 25.6 Signatures: flows
Each parameter and the result of a function have **flow names**:
- the regions written in their types; `borrow(p)` in the result contributes `p`'s flow names;
- the type parameters their types mention;
- for a parameter of scoped type without written regions: a fresh name of its own (elision);
- for a result of scoped type without written regions: the flow names of the single scoped
  parameter (v0 elision); with several scoped parameters and none written, N0245 (as in v0).
- for a result whose written regions are all `static` (`StringView<static>`, `Slice<static, T>`, `S<static>`,
  `borrow(static) T`, or an optional or array of one): none, and no elision (ADR 0068). It borrows from no
  parameter, so the body may return only static data (N0245 for a parameter's loan, N0244 for a local's), and a
  caller treats the result as static data: it outlives the arguments and may be stored in a static (§39.3).

`static` is never a flow name. It is written only in results and bodies: in a parameter's type (a function's, a
method's or an interface method's) it is N0273 (ADR 0068), since a call does not check that an argument is static
data, and through a function value, a `dyn` method or a bound it could not. A string literal still passes to a
parameter without `static`. A function's **flows** are then:
- **result ← parameter `i`** iff they share a flow name: the result may carry `i`'s loans (this
  generalises v0's return sources, §13.5);
- **parameter `i` ← parameter `j`**, for `i` of type `borrow mut T` whose `T` shares a flow name
  with `j`: the callee may store `j`'s loans into what `i` refers to.

A v0 signature (no regions) has exactly its v0 return sources and no parameter flows.

### 25.7 Borrow checking (`lifetimes.md` §3)
With one loan set per value:
1. Writing a scoped value into a place of a local (a field, an element, a payload) adds its
   loans to that local.
2. Writing it through a reference adds its loans to the reference and to what the reference was
   borrowed from, followed back through reborrows, calls (by their result flows) and region
   allocation. If that reaches a `borrow mut` parameter `p` of the current function, the value's
   loans must all come from parameters that flow into `p` (§25.6): otherwise N0272.
3. `push`, `insert` and `append` write their argument through their receiver (rule 2).
4. At a call, an argument that flows into a `borrow mut` parameter adds its loans to what that
   argument refers to.
5. In generic code, values whose type mentions a type parameter carry loans like scoped values, unless it mentions
   it only inside a `dyn` type (an interface's arguments, a `dyn fn`'s signature): a `dyn` value carries no loans of
   its own (§37.2), so `Box<dyn Handler<S>>` is stored where `S` is a type parameter as it is for a concrete `S`.
6. Destroying a value that is scoped and needs drop is a use of its loans: its `drop` may read
   them, so they must still be live (N0243 otherwise).

Diagnostics: N0270, N0272, N0273, N0274 (`diagnostics.md`).

## 26. Program arguments, files and sockets (C3)

```ceesafe
fn main() -> i32 {
    let argument = Process.argument(1);
    if (argument == null) { Console.writeLine("usage: size FILE"); return 2; }
    let path = argument!;
    match (File.open(path)) {
        Ok(file) => {
            var buffer = [0u8; 4096];
            var total: u64 = 0;
            loop {
                match (file.read(mut buffer[..])) {
                    Ok(n) => { if (n == 0) { break; } total += n as u64; }
                    Err(e) => { Console.writeLine(e.message()); return 1; }
                }
            }
            Console.writeLine(total);
            return 0;
        }
        Err(e) => { Console.write(path); Console.write(": "); Console.writeLine(e.message()); return 1; }
    }
}
```

### 26.1 Program arguments
- `Process.argumentCount()`: `usize`, the number of command-line arguments including the program
  name (argument 0).
- `Process.argument(i)`: `StringView?`, argument `i` as a view of static text (no loans), or `null`
  if `i` is out of range or the argument's bytes are not valid UTF-8 (the `StringView` invariant,
  §22.2). Arguments are never copied.

### 26.2 `IoError`
A built-in enum, like `AllocError`, with the variants `NotFound`, `PermissionDenied`,
`AlreadyExists`, `IsDirectory`, `NotDirectory`, `InvalidInput`, `BrokenPipe`,
`ConnectionRefused`, `AddressInUse`, `OutOfMemory`, `Other(i32)` (the platform's error number), and
after it (ADR 0044, so the earlier variants keep their tags) the network kinds `ConnectionReset`,
`ConnectionAborted`, `NotConnected`, `TimedOut`, `HostUnreachable`, `NetworkUnreachable`,
`AddressNotAvailable` and `Unsupported`, and after them (ADR 0060) `HostNotFound`, the resolver's "no such host".
`e.message()` is a `StringView` of static text describing it, glibc's `strerror` wording on every
platform: "No such file or directory", "Permission denied", …, "Connection reset by peer",
"Software caused connection abort", "Transport endpoint is not connected", "Connection timed out",
"No route to host", "Network is unreachable", "Cannot assign requested address", "Operation not
supported", and "Host not found", which no errno has; `Other`'s is "Input/output error".

| Variant | POSIX | Winsock |
|---|---|---|
| `ConnectionReset` | `ECONNRESET` | `WSAECONNRESET` |
| `ConnectionAborted` | `ECONNABORTED` | `WSAECONNABORTED` |
| `NotConnected` | `ENOTCONN` | `WSAENOTCONN` |
| `TimedOut` | `ETIMEDOUT` | `WSAETIMEDOUT` |
| `HostUnreachable` | `EHOSTUNREACH` | `WSAEHOSTUNREACH` |
| `NetworkUnreachable` | `ENETUNREACH` | `WSAENETUNREACH` |
| `AddressNotAvailable` | `EADDRNOTAVAIL` | `WSAEADDRNOTAVAIL` |
| `Unsupported` | `EOPNOTSUPP`, `ENOTSUP`, `ENOPROTOOPT`, `EAFNOSUPPORT`, `EPROTONOSUPPORT` | their `WSAE` forms |
| `HostNotFound` | `getaddrinfo`'s `EAI_NONAME`, `EAI_NODATA`, `EAI_ADDRFAMILY` | `WSAHOST_NOT_FOUND`, `WSANO_DATA` |

`IoError.TimedOut` is the kernel's (a connect or keep-alive gave up); `AsyncError.TimedOut` is a
deadline of the program's. A `match` that names the first eleven variants and has no `_` arm does
not cover the network kinds (N0256): a source change of 0.7.0; one that names the first nineteen does not cover
`HostNotFound` (N0256): a source change of ADR 0060's release.

### 26.3 Files and the console
- `File` owns an open file. It is a **move type that needs drop**: destroying it closes the file
  (an error from closing is ignored). It is not scoped and not Copy.
- `File.open(path)`: `Result<File, IoError>` for reading; `File.create(path)`: for writing, created
  or truncated; `File.remove(path)`: `Result<void, IoError>`. `path` is a `StringView`; a path that
  contains a zero byte or is longer than 4095 bytes is `InvalidInput`.
- `f.read(mut buffer)` (`buffer: SliceMut<u8>`, `f` taken as `borrow mut this`): `Result<usize,
  IoError>`, the number of bytes read, `0` at the end of the file. Interrupted reads are retried.
- `f.write(bytes)` (`Slice<u8>`): `Result<void, IoError>`; it writes all the bytes or fails.
- `Console.read(mut buffer)`: reads standard input like `f.read`. `Console.writeBytes(bytes)`
  writes raw bytes to standard output, in order with `Console.write`.
- `Console.writeError(text)` and `Console.writeErrorLine(text)` (`text: StringView`) write to
  standard error; standard output is flushed first, so the two keep their relative order.

### 26.4 TCP sockets
- `TcpListener.bind(address, port)` (`address: StringView`, IPv4 or IPv6 text without a zone such as
  `"127.0.0.1"`, `"0.0.0.0"`, `"::1"` or `"::"`, ADR 0044; `port: u16`, `0` for any free port):
  `Result<TcpListener, IoError>`, listening, with the platform's largest backlog (`SOMAXCONN`),
  `SO_REUSEADDR` on POSIX, and an IPv6 socket dual stack (`IPV6_V6ONLY` off).
  `listener.port()`: `Result<u16, IoError>`, the port it is bound to. `listener.accept()`:
  `Result<TcpStream, IoError>`.
- `TcpStream.connect(address, port)`: `Result<TcpStream, IoError>`. `s.read(mut buffer)` and
  `s.write(bytes)` as for files; `s.shutdownWrite()`: `Result<void, IoError>`, so the peer reads
  the end of the stream; `s.shutdownRead()` likewise ends this side's reading.
- The stream's options (ADR 0044 §3.3), each `Result<void, IoError>` and not blocking:
  `s.setNoDelay(on: bool)` (`TCP_NODELAY`), `s.setKeepAlive(idleSeconds: u32?)` (`SO_KEEPALIVE`
  and the idle time, `0` the system's; null turns it off), `s.setSendBuffer(bytes: u32)` and
  `s.setRecvBuffer(bytes: u32)` (`SO_SNDBUF`, `SO_RCVBUF`; the kernel may round, and Linux doubles),
  `s.setLinger(seconds: u32?)` (`SO_LINGER`: `0` makes the close reset the connection; null
  restores the default). Where the platform has no such option (CosmoOS): `Unsupported`.
- The typed addresses (`SocketAddress`, `bindWith`, `connectTo`, `peerAddress`, `localAddress`)
  are the async types' (§40.4): a built-in cannot name a library type.
- Both are move types that need drop; destroying one closes the socket.

### 26.5 Processes
- `Process.run(program, arguments)` (`program: StringView`, found on `PATH`; `arguments: borrow
  Vector<StringView>`, passed after the program name): `Result<i32, IoError>`. It starts the
  program with the caller's standard input, output and error, waits for it, and returns its exit
  status, or 128 plus the signal number if a signal ended it. Standard output is flushed first, so
  output keeps its order. A program that cannot be started is an error (`NotFound`, …); an argument
  with a zero byte is `InvalidInput`; if copying the arguments for the C library fails, `OutOfMemory`.

### 26.6 Runtime (Profile A and hosted)
Implemented in `runtime/hosted/ceesafe_rt_io.c` over the C library's `open`, `read`, `write`,
`close`, `unlink`, `socket`, `bind`, `listen`, `accept`, `connect`, `getsockname`, `shutdown`,
`waitpid`, and `posix_spawnp` (hosted) or `spawnvp` (CosmoOS) (L1, OD-15: public libc functions
only). CosmoOS's `struct sockaddr` (host-order port, 16 address bytes), its spawn API and its wait
status encoding differ from POSIX; the runtime selects them at compile time.
Generated code only calls functions declared in `ceesafe/rt.h`, so it still compiles freestanding.

Diagnostics: none new.

### 26.7 Running a program with its output captured (M15 S7, ADR 0028)
- `Process.capture(program, arguments, input)`: `Result<ProcessOutput, IoError>`.
  - `program: StringView` is found on `PATH`, as for `Process.run`; `arguments: borrow Vector<StringView>`.
  - `input: Slice<u8>?` is the child's standard input (empty if `null`).
  - It waits for the child.
- `ProcessOutput` (lib/std) is a struct with three fields:
  - `status: i32`: the exit status, or 128 plus the signal that ended it, as `Process.run` reports it;
  - `stdout: Vector<u8>` and `stderr: Vector<u8>`: everything the child wrote to each stream.
- **How.** The three streams go through files in a private temporary directory (§26.10), which is removed before
  `capture` returns. A child that writes much to both streams therefore cannot block, without `poll` or threads,
  which CosmoOS's libc does not offer.
  - POSIX: `posix_spawn` file actions.
  - CosmoOS: `spawnvp`'s handle mapping (its `spawn.h`).
  - Windows: `CreateProcess`, quoting the arguments by the C runtime's rules.
- **Errors.** As for `Process.run`: a program that cannot be started is `NotFound` and the like, and a failed
  allocation is `OutOfMemory`. A mistake in the call's arguments is reported against `HostServices.capture`, lib/std's
  function behind it.

### 26.8 What is at a path
- `File.kind(path)`: `Result<PathKind?, IoError>`, following symbolic links: `PathKind.File`, `PathKind.Directory`
  or `PathKind.Other` (lib/std's enum), or `null` if nothing is there.
- ADR 0028 called this `Path.kind`. It is `File.kind` because `Path` is a common name in programs (the corpus has
  one), and lib/std's names cannot be redeclared in a hosted package (§38.5).

### 26.9 The environment
- `Environment.variable(name)`: `StringView?`, the variable's value, or `null` if it is not set or not valid UTF-8.
- Like `Process.argument` (§26.1), the value is a view of static text with no loans. CeeSafe has no way to change the
  environment, so it stays valid. C code that calls `setenv` breaks that, as it would in C.

### 26.10 Directories
- `Directory.create(path)` makes one directory: `AlreadyExists` if something is there, `NotFound` if its parent is
  missing.
- `Directory.remove(path)` removes an empty directory.
- `Directory.list(path)`: `Result<Vector<String>, IoError>`.
  - It gives the entries' names, without `.` and `..`, **sorted by byte value**, so a program's output never depends
    on the order a file system returns them in.
  - A name that is not valid UTF-8 is `InvalidInput`, not skipped, so a program cannot miss a file without a word.
- `Directory.temporary()`: `Result<String, IoError>`, the path of a new, empty directory that only this user can
  enter.
  - It is created under the platform's temporary directory (`TMPDIR` or `/tmp` on POSIX, `GetTempPath` on
    Windows) by trying names until `mkdir` makes one, which is atomic.
  - The caller removes it.
- `Directory.current()`: `Result<String, IoError>`, the working directory.
- **Common rules for §26.7–§26.10.**
  - Paths are `StringView`s, under `File.open`'s rules: a zero byte or more than 4,095 bytes is `InvalidInput`.
  - All of these are hosted-only. In freestanding code, `Process` and `File` are N0293, and `Environment` and
    `Directory`, being lib/std's, are not defined.
  - They are lib/std code (`lib/std/host.cee`) over the runtime's `ceesafe_host_*` functions
    (`runtime/hosted/ceesafe_rt_io.c`).
  - The interpreter models them, and its sandbox (`ceesafec mcp`, `ceesafe_run`) refuses them all.

### 26.11 The wall clock (ADR 0055)
- `Clock.now()` (lib/std) is a `UnixTime`: `seconds: i64` since 1970-01-01T00:00:00Z, leap seconds not counted (POSIX
  time), negative before it, and `nanoseconds: u32`, 0 to 999,999,999. `t.millis()` is `seconds * 1000 + nanoseconds /
  1,000,000`, rounded toward minus infinity; `Clock.unixMillis()` is `Clock.now().millis()`.
- It is the system's real time (`clock_gettime(CLOCK_REALTIME)`; Windows `GetSystemTimePreciseAsFileTime`), which may
  jump backward or forward: waits and measurements use `Timer`'s monotonic clock (§40.4). Where the system has no
  real-time clock (CosmoOS userland without one) it is 1970.
- Hosted only: in freestanding code `Clock` and `UnixTime`, being lib/std's, are not defined (N0200).
- The interpreter answers 2000-01-01T00:00:00Z (946,684,800 seconds) plus its virtual clock (§40.8), so a run stays
  reproducible. Calendar dates, time zones and formatting are not part of the library.

### 26.12 Random bytes (ADR 0060)
- `Random.fill(into)` (lib/std; `into: SliceMut<u8>`) is `Result<void, IoError>`: it fills `into` from the system's
  cryptographic random source, read on every call: `getrandom(…, 0)` on Linux (retried on `EINTR` and for short reads;
  `/dev/urandom` where `getrandom` is missing or `ENOSYS`), `arc4random_buf` on macOS and the BSDs,
  `BCryptGenRandom(NULL, …, BCRYPT_USE_SYSTEM_PREFERRED_RNG)` on Windows. An empty slice is `Ok`.
- Errors: `Unsupported` where there is no source (CosmoOS userland); the source's error otherwise (`IoError` of its
  errno; `Other(EIO)`'s kind on Windows), with `into` partly written. It never answers predictable bytes.
- It has no effect (`@effects()`): it does not allocate, and waits only for the kernel's first seeding at boot.
- Hosted only: in freestanding code `Random` is not defined (N0200).
- The interpreter answers a fixed sequence (SplitMix64 from the seed `0x2b7e151628aed2a6`, each call taking whole
  64-bit words, low byte first, and dropping what it does not use), so a run stays reproducible (§40.8). Its bytes are
  not secret.

### 26.13 Native windows (ADR 0069)
- **`Window.open(title, width, height)`** (lib/std; `width`, `height: u32` in points) is `Result<Window,
  WindowError>`.
  - **The window.** It is titled, resizable and visible, and on top of the program's other windows.
  - **`WindowError`:**
    - `Unsupported`: no backend for this system, the interpreter, or `width`/`height` 0 or over 16384;
    - `NoDisplay`: no window server, such as no `DISPLAY` or no X11 library;
    - `NotMainThread`: called on any thread but the program's first, on every system, so that programs stay
      portable to macOS, which allows windows only there;
    - `Resources`: the system refused.
  - **Ownership.** `Window` is a move type, neither `Send` nor `Share`. Its drop closes the window.
- **Events.** `w.nextEvent()` is `WindowEvent?`: the next pending event, or null. It never blocks, and the events of
  one window come in order. `WindowEvent`:
  - `Closed`: the user asked to close the window, which stays open until it is dropped;
  - `Resized(WindowSize)`, in points;
  - `Focus(bool)`;
  - `PointerMoved(x, y)` and `PointerButton(WindowButton, down, x, y)`. `WindowButton` is `Primary`, `Secondary`
    or `Middle`. Positions are `f32` points from the content's top left; a pointer outside the window during a
    drag gives coordinates outside the size;
  - `Wheel(dx, dy)`, in lines, positive `dy` away from the user (scrolling up);
  - `Key(WindowKey, down, repeat, WindowModifiers)`. `WindowModifiers` has `shift`, `control`, `alt` and `command`:
    `command` is ⌘ on macOS and the Windows key elsewhere;
  - `Text(u32)`: a Unicode scalar the user typed, after the key's own `Key` event; control characters are not
    reported as `Text`.
- **Keys.** `WindowKey` is one of:
  - `Tab`, `Enter`, `Escape`, `Backspace`, `Delete`, `Left`, `Right`, `Up`, `Down`, `Home`, `End`, `PageUp`,
    `PageDown` and `Space`;
  - `Character(u32)`: a letter, as lowercase, a digit, or the unshifted character of a punctuation key, on the
    layout in use;
  - `Function(u8)`: F1 to F12;
  - `Other(u32)`: the system's code for any other key.
- **Size and drawing.** `w.size()` is `WindowSize { width, height }` in points, and `w.scale()` is pixels per point
  as an `f32`, such as 2.0 on a Retina display.
  - **`w.present(pixels: Slice<u32>, width, height)`** is `Result<void, WindowError>`. It shows a `width × height`
    image, rows top to bottom, scaled to fill the window's content.
  - **Pixels.** Each `u32` is one premultiplied RGBA8 pixel with bytes R, G, B, A in memory order:
    `r | g << 8 | b << 16 | a << 24`. The window is opaque, so alpha does not show through.
  - **Errors.** `pixels.length < width × height`, or a zero dimension, is `Resources`.
  - **Copying.** The pixels are copied during the call. A program draws at `size() × scale()` for one image pixel
    per screen pixel.
  - **`w.presentChanged(pixels, width, height, changed: Slice<WindowBox>)`** (ADR 0070) is `present` with a promise:
    only the pixels inside the `changed` boxes differ from the last frame presented. `WindowBox` is `{ x, y, width,
    height }` in the image's pixels. The window may then show just those pixels.
    - It is a plain `present` when there is no last frame of this size, when a box is not inside the image, or
      when there are more than 64 boxes.
    - An empty `changed` shows nothing new, and a box of zero size is skipped.
    - Pixels outside the boxes may be shown from `pixels` or from the window's copy of the last frame, so a program
      that breaks the promise sees either. A scripted window shows the copy.
- **Settings and waiting.**
  - `w.setCursor(WindowCursor)` takes `Default`, `Pointer`, `Text`, `Crosshair`, `Grab`, `ResizeHorizontal` or
    `ResizeVertical`, shown while the pointer is over the content.
  - `w.setTitle(title)` sets the title.
  - `w.waitFrame()` returns when about 1/60 s has passed since the previous `waitFrame`, or earlier when an event
    arrives, so an idle loop does not spin.
- **Effects.** `open`, `present`, `presentChanged`, `waitFrame` and `setTitle` may sleep (`@effects(sleep)`). `nextEvent`, `size`,
  `scale`, `setCursor` and the drop have no effect.
- **Backends** (`runtime/hosted/ceesafe_rt_window.c`):
  - **macOS:** AppKit through the Objective-C runtime, loaded with `dlopen` at the first `open`, so no program
    links it;
  - **Windows:** Win32 and GDI;
  - **Linux:** X11, `libX11.so.6` loaded with `dlopen`.

  The runtime file is part of a build only when the program calls a window function.
- **Scripted windows** (for tests, ADR 0069 §2.3). When the environment variable `CEESAFE_WINDOW_SCRIPT` names a
  file, `open` makes a window that exists only in memory, on any system:
  - **Size and scale.** Its size is the requested one, until a `resize`, and its scale is 1.
  - **Frames.** Frame *n* is the time after the *n*-th `present`. The script's events for frame *n* are what
    `nextEvent` returns then, in the script's order.
  - **Script grammar.** The file holds `;`-separated `FRAME:WHAT` items:
    - `move:X,Y`;
    - `down:B,X,Y` and `up:B,X,Y`, with B 0 to 2 for primary, secondary and middle;
    - `click:X,Y`: a primary down on FRAME and its up on FRAME+1;
    - `wheel:DX,DY`;
    - `key:NAME` (down then up), `keydown:NAME` and `keyup:NAME`;
    - `type:TEXT`: one `Text` per character;
    - `resize:W,H`, `focus:0|1` and `close`.
  - **Key names:** `tab`, `enter`, `escape`, `backspace`, `delete`, `left`, `right`, `up`, `down`, `home`, `end`,
    `pageup`, `pagedown`, `space`, `f1`…`f12`, or one character.
  - **Frame dump.** When `CEESAFE_WINDOW_DUMP` names a file, each `present` replaces it with the line
    `ceesafe-window W H`, then the W×H pixels as RGBA bytes.
  - **Waiting.** `waitFrame` returns at once.
  - **Errors.** A script that cannot be read or parsed makes `open` `Resources`.
- **Hosted only.** In freestanding code these types are not defined (N0200). The interpreter answers `Unsupported`
  to `open`.

## 27. Raw pointers and `unsafe` (M8a)

The contract that unsafe code must keep, and what the compiler and the interpreter check, is in
`unsafe.md`. This section gives the typing rules.

```ceesafe
/// Safety: `p` points to `n` initialised `u32`s that nothing else writes while this runs.
unsafe fn sum(p: *u32, n: usize) -> u64 {
    var total: u64 = 0;
    for (i in 0..n) { total += p.add(i).read() as u64; }
    return total;
}

/// Safety: the pointer comes from a live array and stays inside it.
fn main() -> i32 {
    var a = [1u32, 2, 3, 4];
    let p = borrow mut a[0] as *mut u32;
    unsafe { p.add(3).write(10); }
    Console.writeLine(unsafe { sum(p, a.length) });   // 16
    return 0;
}
```

### 27.1 Types
- `*T` (read-only) and `*mut T` (writable) are **raw pointers** to `T`. `T` is any value type, or
  `void` for an untyped pointer (`*void`, C's `void *`). It may not be a reference, slice,
  `StringView` or other scoped type (§19.1, §25.3), or `never` (N0281). Since M8d it may be a type
  parameter: an instantiation with a scoped type argument is then N0274, as for a box of `T`.
- A raw pointer is one machine address: 8 bytes, aligned to 8 on LP64. It is **Copy**, needs no
  drop, holds no loans and is not scoped (U3). So it may appear anywhere a Copy value may: locals,
  parameters, results, fields, payloads and array elements. Constants of pointer type are not
  supported (N0216).
- `*mut T` coerces to `*T` wherever a `*T` is expected (like `SliceMut<T>` to `Slice<T>`).
- `==` and `!=` compare two pointers of the same type (after that coercion) by address. Ordering
  comparisons are not defined on pointers (N0207); compare `p as usize` instead.

### 27.2 Making pointers (safe)
Making and converting a pointer is safe; only using one is not (§27.3).
- `r as *T` for a reference `r: borrow T` or `borrow mut T`, and `r as *mut T` for `r: borrow mut T`.
  `borrow x as *T` parses as `(borrow x) as *T` (grammar §2.1). The borrow ends at once: the pointer
  holds no loan, so later uses of `x` are not checked against it.
- `s.ptr` for a slice `s`: `*T` for `Slice<T>`, `*mut T` for `SliceMut<T>`; for a `StringView`, `*u8`.
  It is the address of element 0 (of where it would be, for an empty slice).
- `p as *U` and `p as *mut U` for any raw pointer `p`, whatever its pointee and mutability.
- `p as usize` for any raw pointer, and `n as *T` / `n as *mut T` for `n: usize` (also `*void`).
  An integer literal converts too: `0 as *mut T` is the **null pointer** (there is no `null`
  pointer literal: `null` is the empty optional, §22.1).
- `p.isNull()`: `bool`.

### 27.3 Using pointers (unsafe)
These are allowed only in an unsafe context (§27.4; N0280 otherwise). `p` is a raw pointer
expression, evaluated once.
- `p.read()`: the `T` at `p`, moved out (copied, for a Copy `T`). Afterwards the memory holds no `T`: the
  unsafe code must not read it as one, drop it or let anything else use it as one until it writes a new value
  there (`unsafe.md` U2). A type holding C state (§33) is N0282, since C state never moves. Since ADR 0027; before
  it, `T` had to be Copy.
- `p.write(v)`: moves `v` to `p` (copies, for a Copy `T`) without reading or destroying what was there. `p` must be
  a `*mut T` (N0282). It returns `void`. For a type with `@placed` methods (§33.3) the store is a placement: they
  run at `p` afterwards, as for `Box.create`, and `v` may be the struct literal without its C state that
  `Box.create` takes (§33.2).
- `p.readVolatile()` and `p.writeVolatile(v)` (§29.1) still need a Copy `T`.
- `p.add(n)` and `p.sub(n)` (`n: usize`) and `p.offset(n)` (`n: isize`): the pointer `n` elements
  (`n × size(T)` bytes) after (`sub`: before) `p`, with the type of `p`. The arithmetic wraps modulo
  2^64; it never panics.
- `Ptr.toBorrow(p)`: a `borrow T` to the `T` at `p`; `Ptr.toBorrowMut(p)`: a `borrow mut T`, for
  `p: *mut T` (N0282 for `*T`). The reference holds **no loans** (U3): the borrow checker cannot
  protect it, so the unsafe code that makes it must bound its use, typically by returning it from a
  function whose signature ties it to a parameter (§13.5).
- `Slice.fromRaw(p, n)`: a `Slice<T>` of the `n` elements from `p`; `SliceMut.fromRaw(p, n)`: a
  `SliceMut<T>`, for `p: *mut T`. Like `Ptr.toBorrow`, the slice holds no loans.
- Calling an `unsafe fn` (§27.4).
- None of these is defined for `*void`: cast it to a typed pointer first (N0282).

### 27.4 Unsafe contexts
- `unsafe { statements }` is a statement; `unsafe { e }` is an expression whose value is `e`'s
  (grammar §2). In statement position, `unsafe {` always starts the statement form.
- `unsafe fn` (a modifier after the visibility): calling the function is an unsafe operation, and its
  body is an unsafe context. `main` may not be `unsafe` (N0219). Methods may be `unsafe`; since C5,
  interface methods may be too (§30.1), and an `impl` must match (N0267).
- Everything lexically inside an unsafe block, an unsafe expression or the body of an `unsafe fn`
  is in an unsafe context, including deferred statements. A nested `unsafe` is allowed.
- **The Safety rule** (U2): an `unsafe fn`, and every function whose body contains an `unsafe`
  block or expression, must carry a doc comment (grammar §1.2) with a line that starts with
  `Safety:`. It states the contract: what callers must guarantee (for an `unsafe fn`) or why the
  unsafe operations inside are sound (for a safe function). Without one, N0283.
- Safe code is checked exactly as before. An unsafe context only *permits* the operations of §27.3;
  it never relaxes the type, move, borrow or region rules for anything else.

### 27.5 Lowering
- **NIR**: `p.read()` and `p.write(v)` are a copy from, and an assignment to, the place `(*t)` of a
  temporary `t` holding `p`: the same `Deref` projection as references, through a pointer.
  `Ptr.toBorrow`, `Ptr.toBorrowMut` and the pointer casts are `Cast`s; the arithmetic, `isNull`,
  `.ptr` and `fromRaw` are built-ins. The move and borrow checkers ignore places through raw
  pointers (no loans, no moves, no mutability check beyond §27.3).
- **C** (c-backend §2): see `unsafe.md` §4. Pointer arithmetic is done on `uintptr_t`, so generated
  C never computes an out-of-object pointer.
- **Placement** (M5, `memory-model.md` §5.1 rule 1c): a function that makes a raw pointer from a
  reference or calls `Ptr.toBorrow`/`toBorrowMut` or `fromRaw` keeps all its boxes on the heap
  ("address exposed").

Diagnostics: N0280 (an unsafe operation outside an unsafe context), N0281 (an invalid pointee
type), N0282 (a pointer operation the pointer or its pointee does not allow), N0283 (no `Safety:`
doc comment). Invalid pointer casts are N0208.

## 28. C interoperability (M8b)

```ceesafe
extern "C" {
    fn strlen(s: *CChar) -> usize;
    fn puts(s: *CChar) -> i32;
}

@repr(C)
struct Point { x: i32; y: i32; }

/// The Manhattan length of a point, callable from C as `geo_length(struct Point)`.
@export("geo_length")
fn length(p: Point) -> i32 => abs32(p.x) + abs32(p.y);

fn abs32(v: i32) -> i32 { if (v < 0) { return -v; } return v; }

/// Safety: the literal ends in a zero byte, as `puts` and `strlen` need.
fn main() -> i32 {
    let text = "hello from C\0";
    let p = text.ptr as *CChar;
    unsafe { puts(p); }
    return truncate<i32>(unsafe { strlen(p) });
}
```

### 28.1 Attributes
An item may be preceded by attributes `@name` or `@name(arguments)` (grammar §2). The known ones:

| Attribute | On | Meaning |
|---|---|---|
| `@export`, `@export("symbol")` | a top-level, non-generic function; a constant (C9) | callable from C under `symbol` (default: the function's name); for a constant, C data under that name, §28.4 |
| `@cname("name")` | a field of an imported C struct (C9) | its C member name, when that is not a CeeSafe identifier, §31.1 |
| `@repr(C)` | a non-generic struct | a C-compatible struct, §28.5 |
| `@repr(u8)` … `@repr(i64)` | a non-generic enum without payloads | passed to and from C as that integer type, §28.5 |
| `@effects(…)` | a function, method or `extern` function | the effects it may have (§32.2) |
| `@context(atomic)`, `@context(irq)` | a function or method; `atomic` also a struct | a context that forbids effects (§32.3) |
| `@placed` | a method `fn m(borrow mut this)` | the type's placement initialiser (§33.3) |
| `@callback` | a top-level function | a C address without a symbol, for C callbacks (§34.1) |
| `@align(n)` | a non-generic struct | at least `n`-byte alignment (M8d, §29.4) |
| `@noncopy` | a struct | a move type even if every field is Copy (OD-7; M8d) |
| `@cfg("NAME")` | an `@export` function | its C symbol only in builds where the C macro `NAME` is non-zero (ADR 0043), §28.4 |
| `@test` | a top-level function with no parameters, not generic, not exported, returning `void` or `Result<void, E>` | a test that `ceesafec test` runs (ADR 0049; tooling.md §10); `build` leaves it out, and only the generated harness may call it (**N0348** otherwise) |

An unknown attribute, a known one in the wrong place or with the wrong arguments, or the same
attribute twice is N0290.

### 28.2 `extern "C"` blocks
- `extern "C" { … }` at the top level declares functions defined in C: `fn name(parameters) ->
  Result;`, each optionally preceded by attributes and a visibility, with no body, receiver, type
  parameters or region parameters (N0290). `"C"` is the only ABI (N0290).
- Calling an extern function is an unsafe operation (U1, N0280): CeeSafe cannot check what C does.
- Its C name is its CeeSafe name. It must be a valid C identifier that is not a C keyword (or `bool`,
  `true`, `false`, `NULL`, `main`), not reserved by C (starting with `__`, or `_` and an uppercase
  letter), and not owned by the runtime and generated code (starting with `ceesafe_`, `cee_`, `CEE_`, or
  `cee` and an uppercase letter; c-backend §2.1): N0292.
  Two declarations, or a declaration and an `@export`, with one C name are N0201.
- Parameter and result types must be **FFI-safe** (§28.3), or N0291.
- A function that a `@header` defines `static inline` may be declared and called (ADR 0030): the generated prototype
  has no storage class, so it takes the definition's internal linkage (C11 §6.2.2p4–5). CosmoOS's `list.h` helpers are
  called so (lib/kernel's `List`).
- **Variadic functions** (ADR 0041 §3.3). `fn klog(level: u32, format: *CChar, ...);` declares a C variadic
  function: `...` ends its parameters, which must not be empty (N0290), and it is allowed in an `extern "C"` block
  only (N0100). A call passes the parameters as for any C function and then any number of further arguments, each of
  a type that C's default argument promotions (C11 §6.5.2.2p6–7) leave unchanged: `i32`, `u32`, `i64`, `u64`,
  `isize`, `usize`, `f64` or a raw pointer, `*CChar` among them, passed as its C type (§28.3). A narrower integer,
  `bool`, `f32` or anything else is N0347, whose message says how to widen it with `as`. Fewer arguments than
  parameters is N0209.
  - Like every C call, it is unsafe (N0280), and here the `/// Safety:` comment carries more: whether the further
    arguments agree with what the callee reads (a format string's conversions, a count) is not something CeeSafe
    checks, and C checks only a literal format, while the generated C passes a pointer.
  - It is called directly, with no wrapper function, so it has no C function pointer type and cannot be one (N0291),
    and it cannot return a `@repr` enum, which a wrapper would check (N0291). A unit that calls one disables C's
    warnings about a format that is not a literal (`-Wformat-security`, `-Wformat-nonliteral`), for the reason above.
  - **Formatting without a variadic call** (ADR 0041 §3.3). lib/core's `TextWriter<region r>`
    (`lib/core/format.cee`, every profile) writes into a caller's `SliceMut<u8>` as `snprintf` does, with no
    allocation: `create(buffer)`, then `text(s)`, `bytes(b)`, `byte(b)`, `decimal(u64)`, `signed(i64)` and
    `hex(u64, width)`; what does not fit is cut, the text stays zero-terminated in a buffer of at least one byte, and
    `written()`, `length()`, `wanted()` (every byte asked for, as `snprintf`'s result), `truncated()` and `cText()`
    (a `*CChar` to it) read it. lib/kernel's `KernelLog` (`lib/kernel/log.cee`, `--kernel cosmoos`) sends one:
    `send(level, line)` is `klog(level, "%s", text)`, `write(level, message)` a fixed message, and `panic(line)` is
    CosmoOS's `panic("%s", text)`; `LogLevel` is log.h's levels, `LOG_LINE_MAX` its 256. These are the only variadic
    calls, with a fixed format, so a port's messages are safe code.

### 28.3 FFI-safe types and their C types
| CeeSafe | C, in prototypes and in `ceesafe_exports.h` |
|---|---|
| `i8`…`i64`, `u8`…`u64` | `int8_t`…`int64_t`, `uint8_t`…`uint64_t` |
| `usize`, `isize` | `size_t`, `ptrdiff_t` |
| `bool` | `bool` |
| `*T`, `*mut T` | `T const *`, `T *`, where `T` is an integer, `bool`, a `@repr(C)` struct, or (recursively) such a pointer |
| `*void`, `*mut void` | `const void *`, `void *` |
| `*CChar`, `*mut CChar` | `const char *`, `char *` |
| a `@repr(C)` struct `S` | `struct S` |
| a `@repr(uN)` / `@repr(iN)` enum | that integer type |
| `void` (results only) | `void` |

Nothing else crosses: references, slices, string views, boxes, arrays, optionals, results and other
structs and enums are N0291. (Arrays and imported C types may be fields of a `@repr(C)` struct, §28.5.) (A slice crosses as its `.ptr` and `.length`; a pointer to any other
CeeSafe value crosses as `*void`.) A `@repr` enum crosses by value only, not behind a pointer or as a
field of a `@repr(C)` struct: inside CeeSafe it is not a plain integer.

`CChar` is C's `char`, whose signedness differs between targets. It may only be a pointee
(`*CChar`), never a value (N0281); `p as *u8` and `q as *CChar` convert. It exists so that
declarations of C library functions match the C library's own prototypes exactly.

### 28.4 Exports
- `@export` makes a top-level, non-generic, non-method function callable from C, with the C types of
  §28.3 (N0291 otherwise). `main` cannot be exported (N0290). The symbol must be a valid, unreserved
  C name (N0292) and unique (N0201).
- The function stays an ordinary CeeSafe function too. Its body is checked like any other; an
  exported function is not unsafe to call from CeeSafe.
- **Configured exports** (ADR 0043). `@cfg("NAME")` on an `@export` function puts its C definition, its prototype and
  its line in `ceesafe_exports.h` inside `#if NAME` … `#endif`: the symbol exists in the builds where the C macro `NAME`
  is non-zero, as C's `#if NAME` around a definition would have it, and a header may define a stub of that name in the
  others. The CeeSafe function is compiled and checked in every build, and CeeSafe code may call it in any. `NAME` is
  the macro's name as a string, a valid C identifier (N0292); `@cfg` without it, or on anything but an `@export`
  function, is N0290. Code that differs between builds inside a function reads the switch through a C constant macro
  instead (§39.4): `static DEBUG: bool = CONFIG_DEBUG_ON();`, which the C compiler folds.
- **`callerAddress()`** (ADR 0030), valid only in an `@export` function's own body (N0340; not in a closure inside it),
  is a `usize`: the address the exported symbol's call returns to, as C's `__builtin_return_address(0)` in the
  export's wrapper (MSVC: `_ReturnAddress()`), passed to the body as a hidden argument so that inlining cannot change
  it. When CeeSafe calls the function, or the interpreter runs it, it is 0. CosmoOS's lockdep names call sites with it.
- A C caller must call it with the declared types. A C value for a `@repr` enum parameter that is
  not one of its variants panics (`invalid enum value from C`) before the body runs.
- **Exported constants** (C9, ADR 0014). `@export` or `@export("symbol")` on a constant (§10) of integer or
  `bool` type, or of a constant array type `[E; N]`, defines read-only C data with that symbol (default: the
  constant's name): `const CT symbol = v;` or `const CE symbol[N] = { … };`, declared in `ceesafe_exports.h` as
  `extern const CT symbol;` / `extern const CE symbol[N];`. The symbol follows the rules for functions (N0292,
  N0201). In CeeSafe the constant is used as any other constant.

### 28.5 C-compatible structs and enums
- `@repr(C)` on a struct fixes its layout to C's rules (fields in declaration order, each aligned,
  the size rounded up to the alignment; CeeSafe's layout already is this, and M9 may reorder only
  structs without it). Its fields must be integers, `bool`, raw pointers as in §28.3, `@repr(C)`
  structs, imported C types by value (§31; ADR 0029), arrays `[E; N]` of any of these (ADR 0029), or `AtomicU32`,
  `AtomicU64` and `AtomicUSize` (ADR 0041; not `AtomicBool`, a `u32` where C's `bool` is a byte), and their
  names valid, unreserved C identifiers (N0291, N0292). In C it is `struct S`
  with members named as in CeeSafe, of the types CeeSafe uses inside its own C (`usize` is `uint64_t`,
  pointers have no `const`), so that the generated code and C code that includes `ceesafe_exports.h`
  declare the same struct type. It is otherwise an ordinary struct.
- An atomic field (ADR 0041 §3.1) is its integer member in C: `uint32_t`, `uint64_t`, and `uint64_t` for
  `AtomicUSize`, as for `usize`. CeeSafe reaches it only through the atomic type's methods, which are the
  `__atomic_*` operations on the member's address, so C code that includes `ceesafe_exports.h` shares it by applying
  `__atomic_*` to the member (a non-atomic C access while CeeSafe may access it is a data race in C).
- An array field is the C array member `CE f[N]` (nested arrays `CE f[N][M]`); in CeeSafe it is an ordinary array.
- **C state** (ADR 0029). A `@repr(C)` struct with a field of an imported C type, directly or in an array or a
  `@repr(C)` field, is C-laid-out (§33.1), and every rule of §33 applies: it is never moved, copied or assigned in
  CeeSafe, and crosses to C only behind a pointer (N0294); CeeSafe makes one only in place (`Box.create` and its
  `@placed` initialiser, or a static). C makes one wherever C makes a struct: embedded in another struct, in a
  static, on the stack. CeeSafe code that receives a pointer to one relies on C having initialised it, which the
  exported function's `/// Safety:` comment states (§27.4). It may not have a `drop` (N0291): C makes and discards
  it without calling CeeSafe, so the drop would never run.
- `@repr(uN)` / `@repr(iN)` on an enum without payloads gives each variant its index (0, 1, …) as
  that integer type, which must hold every index (N0290). Across the boundary the enum is that
  integer; a value from C that is not a variant's index panics (`invalid enum value from C`).

### 28.6 Building with C
- `ceesafec build` accepts C source files (`.c`) besides CeeSafe files. They are compiled with the generated
  C and linked into the program, with the same C compiler and flags.
- Every C file of the build has the runtime's headers on its include path. `ceesafe/foreign.h` (ADR 0060) is CeeSafe's
  allocator for C code: `ceesafe_alloc(size, CEE_ALLOC_FOREIGN)`, `ceesafe_realloc(p, size, CEE_ALLOC_FOREIGN)` and
  `ceesafe_free(p, CEE_ALLOC_FOREIGN)`, which follow `malloc`, `realloc` and `free` (`free(NULL)` does nothing,
  `realloc(NULL, n)` allocates; aligned for any C type; a failed realloc keeps the block). Their attempts are counted
  and fault-injected in one sequence with CeeSafe's (`CEESAFE_FAIL_ALLOC_AT`), but a block live at exit is not a
  leak: the report gives such blocks on a line of their own (memory-model.md §3.2). A C library's global state, kept
  until the process ends, may live there, such as OpenSSL's through `CRYPTO_set_mem_functions` in test builds.
  `CEE_ALLOC_FOREIGN_INIT` (ADR 0061) is for a C library's one-time initialisation, which cannot survive a failed
  allocation: never failed and not counted as an attempt, its blocks held with `CEE_ALLOC_FOREIGN`'s (either number
  may reallocate or free them). With them, a program may have
  its `main` in C instead of in CeeSafe; without a CeeSafe `main`, the CeeSafe runtime is not started, so
  allocation-failure injection (`CEESAFE_FAIL_ALLOC_AT`) and program arguments are not available to
  exported functions.
- When a program has `@export` functions or `@repr` types, ceesafec also writes `ceesafe_exports.h`: the
  `@repr` structs and enums, and a prototype for each export. C files include it as
  `#include "ceesafe_exports.h"`.
- `run --interp` runs no C code. Calls to extern functions are interpreted only for a model of a few
  C library functions (`intermediate-representation.md` §5.4); any other is an interpreter fault.

Diagnostics: N0290 (attribute or extern declaration misuse), N0291 (a type that is not FFI-safe),
N0292 (a C name that is invalid, a C keyword, or reserved). Calls to extern functions outside an
unsafe context are N0280.

## 29. Systems capabilities (M8d)

```ceesafe
@repr(C)
struct UartRegs { data: Volatile<u32>; status: Volatile<u32>; }

struct Uart {
    regs: MmioPtr<UartRegs>;

    /// Safety: `base` is the mapped address of a PL011-compatible UART that no other `Uart` aliases.
    public unsafe fn attach(base: usize) -> Uart => Uart { regs: MmioPtr<UartRegs>.at(base) };

    public fn putByte(borrow mut this, b: u8) {
        while ((this.regs.get().status.read() & 0x20) != 0) { Cpu.spinHint(); }
        this.regs.getMut().data.write(b as u32);
    }
}
```

### 29.1 Volatile accesses (unsafe)
`p.readVolatile()` and `p.writeVolatile(v)` are `read` and `write` (§27.3, same rules: unsafe, Copy
pointee, `*mut T` to write) as **volatile** accesses: each is performed exactly once, in program
order with every other volatile access and call, with the access width of `T`, never merged,
split, cached or removed. In C they are `*(volatile T *)p`. They are for memory-mapped registers
and memory shared with hardware; they do not order accesses between processors (that needs
atomics, C7).

### 29.2 `lib/core`: CeeSafe sources bundled with the compiler
Some core types are written in CeeSafe itself, over raw pointers and `unsafe` (ADR 0007 decision 1).
Their sources live in `lib/core/*.cee`, are embedded in `ceesafec`, and are part of every package, in
both profiles. Their names are reserved like built-in names (N0201). Code from `lib/core` is
type-, move- and borrow-checked like user code; functions and types the package does not use are
not emitted, and `--dump-hir`/`--dump-nir` leave them out.

| Type | What it is |
|---|---|
| `Volatile<T: Copy>` | A cell whose value is read and written only by volatile accesses: `v.read()` (`borrow this`), `v.write(x)` (`borrow mut this`). One field of type `T`, so it has `T`'s layout and may be a `@repr(C)` field |
| `MmioPtr<T>` | The address of a memory-mapped `T` (typically a `@repr(C)` struct of `Volatile` registers). `unsafe MmioPtr<T>.at(address: usize)` makes one; `p.get()` is a `borrow T` and `p.getMut()` (`borrow mut this`) a `borrow mut T`, tied to `p` by their signatures (§13.5), so register access needs no `unsafe`; `p.address()` is the `usize` |
| `PhysAddr`, `VirtAddr` | Distinct address types (`cosmoos/kernel-memory-and-lifetimes.md`): `PhysAddr.of(bits: u64)`, `a.bits()`, `a.offset(n: u64)` (checked). No conversion between them, or to and from integers, except through `of` and `bits` |

The contract of `MmioPtr<T>.at` (its `Safety:` line): the address is mapped, aligned for `T`, and no
other `MmioPtr` or reference aliases the registers; the program then reaches them only through this
value.

### 29.3 CPU and target queries
- `Cpu.spinHint()`: a hint inside a busy-wait loop (`pause` on x86-64, `yield` on AArch64, nothing
  elsewhere). `void`, safe, allowed in both profiles.
- `Target.pointerBits()`: `u32`, the target's pointer width (64 on every target CeeSafe supports).
  `Target.isX86_64()` and `Target.isAArch64()`: `bool`, the architecture the generated C is
  compiled for. They are decided by the C compiler, not by `ceesafec` (the generated C is the same for
  every LP64 target), so they are not constants: use them in ordinary conditions. The interpreter
  answers for the machine it runs on.

### 29.4 `@align(n)`
`@align(n)` on a non-generic struct, with `n` a power of two from 1 to 4096, raises the struct's
alignment to at least `n` (its size is rounded up to it). In C, the first member is declared
`_Alignas(n)`. It may be combined with `@repr(C)`. N0290 for other `n` or places.

### 29.5 C function pointers
- `extern "C" fn(T1, …, Tn) -> R` is the type of a pointer to a C function (grammar §2). Its
  parameter and result types must be FFI-safe (§28.3) and not `@repr` enums (N0291). It is one
  address, Copy, holds no loans, and is FFI-safe itself, so C callbacks can be passed to and
  returned from C.
- **Values.** The name of an `extern "C"` function, an `@export` function or a `@callback` function (§34.1) is a value of that
  type where such a type is expected (a typed `let`, an argument, a field, a return value) and the
  signatures are equal. Only those functions have a C address; naming another function there is
  N0291. `0 as extern "C" fn(…) -> R` is the null pointer, `f.isNull()` tests for it, and `f as usize`
  is its address. Both are static initialisers too (§39.4, ADR 0042): an ops table's entries.
- **From an address** (ADR 0043): `n as extern "C" fn(…) -> R` for `n: usize` is the function pointer to that address,
  as `n as *T` is a raw pointer (§27.2), so a hook can be published in an `AtomicUSize` and read back. Making it is safe;
  calling through it is unsafe, as for any C function pointer, and a wrong address is that call's obligation.
- **Calls.** `f(args)`, with `f` a local, parameter or field of such a type, calls through the
  pointer. It is unsafe (N0280): the pointer may be null or point to anything.
- CeeSafe functions that are neither exported nor `@callback` (§34.1, C7d) have no C address. CeeSafe-level function
  values (`fn(…) -> R`) and closures are §36 (M11 G8).

### 29.6 Entry points
A hosted program's entry point is `main`, called by the generated C `main` (§11). A freestanding
package has no `main` (`profiles.md` §4.1): its entry points are its `@export` functions,
which the environment calls (a kernel's `init`, a boot stub's `kmain`, a callback). An export's
symbol may start with one `_` and a lowercase letter (`_start`), the names platforms use for entry
symbols (§28.2 reserves only `__` and `_` with an uppercase letter); other reserved C names stay N0292.

Diagnostics: no new codes. Volatile accesses outside an unsafe context are N0280; a function pointer
type or value that cannot cross to C is N0291; a misused `@align` is N0290.

## 30. User-defined allocators (C5)

```ceesafe
extern "C" {
    fn kmalloc(size: usize, flags: u32) -> *mut void;
    fn kfree(p: *mut void);
}

/// CosmoOS's kernel heap: kmalloc and kfree (integrations/cosmoos/c5/kernel.cee).
struct KernelHeap {}

impl Allocator for KernelHeap {
    /// Safety: kmalloc returns null or a block of `layout.size` bytes, 16-byte aligned.
    fn allocate(borrow this, layout: Layout) -> Result<*mut u8, AllocError> {
        if (layout.align > 16) { return Err(AllocError.OutOfMemory); }
        let p = unsafe { kmalloc(layout.size, 0) } as *mut u8;
        if (p.isNull()) { return Err(AllocError.OutOfMemory); }
        return Ok(p);
    }
    /// Safety: the caller passes a block this allocator returned (Box guarantees it).
    unsafe fn deallocate(borrow this, p: *mut u8, layout: Layout) { kfree(p as *mut void); }
}

fn make() -> Result<Box<u64, KernelHeap>, AllocError> => Box.createIn(KernelHeap {}, 42u64);
```

### 30.1 `Layout` and `Allocator` (`lib/core/alloc.cee`)
- `struct Layout { size: usize; align: usize; }`: a block's size in bytes and its alignment (a power of
  two).
- `interface Allocator`, with two methods:
  - `fn allocate(borrow this, layout: Layout) -> Result<*mut u8, AllocError>`: a block of at least
    `layout.size` bytes aligned to `layout.align`, or `Err` (never a null `Ok`);
  - `unsafe fn deallocate(borrow this, p: *mut u8, layout: Layout)`: returns a block. Its contract:
    `p` came from `allocate` of this allocator type with the same layout, and is returned once.
- Interface methods may be `unsafe` (lifting §27.4's earlier rule); an `impl` must match the
  interface's `unsafe` (N0267).
- **`layoutOf<T>()`** (ADR 0027): `T`'s size and alignment as a `Layout`, from `TargetInfo`. For a type holding
  C state (§33), whose size only the C compiler knows (§31.2), it is C's `sizeof`/`_Alignof` of `T`'s C type, so
  `ceesafec run --interp` cannot evaluate it. It takes one type argument and no arguments (N0209), and is a static
  initialiser (§39.4). ADR 0027 named it `Layout.of<T>()`; CeeSafe has no type arguments on a member call, so it is a
  function, like `truncate<T>`.
- **`offsetOf<T>(field)`** (ADR 0030): the offset in bytes of `field` in the struct type `T`, as `usize`. From
  `TargetInfo` for a CeeSafe layout; for a C-laid-out or imported `T` (§31, §33), C's `offsetof`, so `ceesafec run --interp`
  cannot evaluate it. It takes one type argument and a field's name (N0209); a `T` that is not a struct, or has no such
  field, is N0339. A static initialiser (§39.4). It reads no memory, so it is not unsafe; recovering a struct from a
  member's address with it is raw-pointer arithmetic, which is (lib/kernel's `containerOf`). ADR 0030 wrote it
  `offsetOf(T, field)`; a type is no expression in CeeSafe, so it takes `T` as a type argument, like `layoutOf<T>()`.

### 30.2 Allocator types
- A **user-defined allocator** is a non-generic struct **with no fields** that implements `Allocator`.
  It may be the `A` of `Box<T, A>`, and `Box.createIn(a, v)` takes a value of it. Having no fields, it
  has no state that a box would need to carry: a `Box<T, A>` stays one pointer, and dropping it
  needs no allocator value (the memory-model's free-standing allocators, §3.2 there). State that an
  allocator needs (counters, a C handle) lives on the C side or in global C state.
- The built-in allocators are still `SystemHeap` and `FailingAllocator`. `Vector`, `String`,
  `SlotMap`, `StableVector`, `Arena` and `Pool` take only those in v0 (N0263): their growth is in the C
  runtime, which cannot call CeeSafe methods.
- A type named as a box's allocator that is not a struct without fields implementing `Allocator` is
  N0263.

### 30.3 Semantics
- `Box.createIn(a, v)` with a user allocator calls `allocate(Layout { size: size(T), align: align(T) })`
  once. On `Err`, it destroys `v` and returns `Err(AllocError.OutOfMemory)`; on `Ok(p)`, it moves `v`
  to `p`. Destroying the box destroys the contents, then calls `deallocate(p, the same layout)`
  once (H1). The compiler only calls `deallocate` with blocks `allocate` returned, so the contract
  holds for every safe program.
- A `size(T)` of 0 is never requested: such boxes still allocate one byte (as `SystemHeap` does).
- User allocators are not placement-transparent (M5): their boxes stay on the heap.
- They are allowed in the freestanding profile (`profiles.md` §4.1): `Box` with a user
  allocator needs no hosted runtime. `Box.create` (`SystemHeap`) stays hosted-only.

Diagnostics: N0263 (a box allocator that is not one), N0267 (an `impl` whose `unsafe` differs).

## 31. C struct types imported from headers (C6b; ADR 0008)

```ceesafe
@header("kernel/bootinfo.h")
extern "C" {
    struct cosmoboot_info {      // only the fields CeeSafe uses, by their C names
        fb_phys: u64;
        fb_width: u32;
    }
    struct bootinfo_framebuffer { phys: u64; width: u32; }
    struct cosmoboot_mem_entry;  // opaque: only pointed to
}

fn pixels(info: borrow cosmoboot_info) -> u64 => info.fb_width as u64;

fn copyOut(info: borrow cosmoboot_info, out: borrow mut bootinfo_framebuffer) {
    out.phys = info.fb_phys;
    out.width = info.fb_width;
}
```

### 31.1 Declarations
- Inside an `extern "C"` block, `struct name { field: Type; … }` declares an **imported C type**: a type
  that a C header defines, seen through the fields CeeSafe declares. `struct name;` imports one with no
  fields (opaque). The grammar is grammar.md §2's `ExternStruct`.
- `@header("path", …)` on the block names the headers that define its types, one string per header,
  as C's `#include "path"` would find them (the build's `-I` paths, then the generated file's
  directory). A block that declares a struct needs a `@header` (N0290). A header string must be
  non-empty and must not contain `"`, `\`, or a line break (N0290).
- The type's C spelling is `struct name`. With `@ctype("cname")` on the declaration, it is `cname`
  instead, for a C typedef (`typedef struct {…} cname;`). The name, the `@ctype` name and every field
  name must be valid, unreserved C identifiers (§28.2, N0292).
- Fields are declared with their C member names, in any order, and only those CeeSafe uses. Their types
  must be integers, `bool`, raw pointers whose pointee C can name (§28.3, including imported types),
  (since C7b, §33.1) imported types, for embedded members, (since C9) **arrays** `[E; N]` of integers,
  `bool` or imported types, for C array members `CE name[N]`, or (ADR 0041) `AtomicU32`, `AtomicU64` or
  `AtomicUSize` for a `uint32_t`, `uint64_t` or `size_t` member; otherwise N0291. An embedded member is checked
  with `_Generic` only, since CeeSafe has no size for it. A field's declared type must be exactly the member's C type, under §28.3's mapping (`u32`
  is `uint32_t`, `usize` is `size_t`, `*CChar` is `const char *`, `*mut T` is `T *`, `[u8; 16]` is `uint8_t[16]`).
  This is checked when the C is compiled (§31.3), not by ceesafec. A C `char`, `signed char` or `unsigned char`
  member is declared `u8`, and an array of them `[u8; N]` (ADR 0034): C lets any object be read and written as
  bytes, and a byte's value does not depend on `char`'s signedness. `CChar` itself stays a pointee only (§28.3). The
  address of a `u8` member, when it is borrowed, converts to `uint8_t *` in the C.
- **Unions** (ADR 0041 §3.2). Inside an imported struct, `union { a: T; b: U; }` declares the members of a C11
  anonymous union member, reached as `r.a` and `r.b` (C's `p->a`). In the block, `union name { … }` declares an
  **imported union**, spelled `union name` in C (or its `@ctype`), used as an imported struct is (behind a pointer or
  reference, or embedded as a field) and its members reached the same way; `union name;` imports one opaque. A
  union needs a member, and an anonymous union holds no union of its own (N0107; grammar.md §2's `ExternUnion` and
  `AnonymousUnion`). `union` is a word only there: elsewhere it is an identifier.
  - The members of one union share storage, so a member's type must be one every bit pattern is a value of: an
    integer, a raw pointer, an array of these, or an imported type whose declared fields are such (N0346). Reading a
    member after another was written reinterprets bytes, as C11 allows (C11 §6.5.2.3 note 95), and can never make an
    invalid value; `bool`, function pointers, atomics and imported types holding them are refused.
  - The borrow checker treats every member of one union as **one place** (§31.2).
- **Atomic fields** (ADR 0041 §3.1). A member that C code reads and changes with `__atomic_*` (a statistic, a
  reference count, a flag word shared between CPUs) is declared with the atomic type over its integer. The atomic
  type is laid out exactly as its integer (§38.3), and the `_Generic` check compares the member with that integer
  type. The field is a place of the atomic type, which CeeSafe reaches only through the type's methods: each is the
  `__atomic_*` operation on the member's address. That C, too, accesses it only atomically is the C side's obligation,
  as for any state C shares. `AtomicBool` is refused (N0291): it is a `u32`, and C's `bool` a byte.
- **`@cname("name")`** on a field (C9) gives its C member name when that is a CeeSafe keyword (`type`, `in`,
  `match`, …); CeeSafe code uses the declared name. The C name must be a valid, unreserved C identifier (N0292),
  and no two fields may have the same C name (N0228). `@cname` anywhere else is N0290.
- An imported struct has no methods, `drop` block, type or region parameters, or other attributes
  (N0290). Its visibility is that of a struct.

### 31.2 Use
- CeeSafe never knows an imported type's size, alignment or field offsets: they are the C compiler's,
  under the build's flags. So an imported type `T` appears only behind a pointer or a reference:
  `*T`, `*mut T`, `borrow T`, `borrow mut T`, and (since C7b, §33) by value as a struct field or a box's
  contents. As the type of a local, parameter, result, array element, optional, or type argument, it is
  N0294. No value of it exists in CeeSafe: it is
  neither Copy nor movable, and has no struct literal (N0294) and no drop. The one exception is a `static mut` item's
  initialiser (§39.4, ADR 0042), which is C's designated initialiser for the static's C object, not a value.
- **Field places.** Through a reference, `r.f` is a place of the declared field type, as for any
  struct (§12): read it, assign it through `borrow mut`, or borrow it. The borrow checker treats
  it like any field place. A field of raw pointer type, `usize` or `isize` can be read and assigned
  but not borrowed (N0294): its C member type (`const char *`, `size_t`) is not the C type CeeSafe
  uses for the value (`char *`, `uint64_t`), so a pointer to it would have the wrong type in C.
- **Union members** (ADR 0041 §3.2). The members of one union (§31.1) are one place: borrowing, assigning or using
  any member, or a place inside it, conflicts with a live loan of any other member as it would with a loan of the
  same member (B1–B2; N0240–N0242). Fields outside the union stay separate places.
- **Array fields** (C9) are array places of their declared type, as in any struct (§16): `r.f[i]` is
  bounds-checked against `N`, `r.f[a..b]` is a slice (mutable through `borrow mut`), and when the element is
  an integer or `bool` the field can be read and assigned whole. An array of an imported type is
  C-laid-out (§33.1): its elements are places of the imported type, used through their fields or borrowed, and
  the array itself is never a value.
- **Raw pointers.** Making, casting and comparing pointers to imported types is safe (§27.2), and
  `Ptr.toBorrow` / `Ptr.toBorrowMut` make references from them (unsafe, §27.3). `add`, `sub` and `offset`
  (unsafe, §27.3) step by the C type's size (since C9): C does the arithmetic. What would copy a value or make a
  slice of one is N0294: `read`, `write`, `readVolatile`, `writeVolatile`, and `Slice.fromRaw` /
  `SliceMut.fromRaw`.
- **Across the boundary**, `*T` and `*mut T` are FFI-safe (§28.3): `struct name const *` and
  `struct name *` (or the `@ctype` name) in prototypes. The type itself is not, by value (N0291).

### 31.3 Lowering
- The generated C includes every `@header`, after `ceesafe/rt.h`, once each, in the order the blocks
  appear (files in command-line order); `ceesafe_exports.h` includes them too.
- CeeSafe emits no definition, typedef or layout assertion for an imported type, and names it by its
  C spelling. A field place is C's member access (`(*p).f`). Pointer fields are converted through
  `uintptr_t` when read and written, as at exports (c-backend.md §2.3), so that `const` in the
  header's member type causes no warning.
- For every declared field `f` of type `F` (C type `CF`), the generated C asserts both
  `_Generic(((S *)0)->f, CF: 1, default: 0)` and `sizeof(((S *)0)->f) == size(F)`, with a message
  naming the struct, the field and the declaration's file and line. Neither expression is
  evaluated. A header that lacks a field, or has it with another type in some configuration, fails
  to compile; it cannot miscompile.
- An array field `f: [E; N]` (C9) is asserted with `_Generic(&((S *)0)->f, CE (*)[N]: 1, default: 0)`, exact
  where `_Generic` of the member itself would see a decayed pointer, and, for integer and `bool` elements, with
  the size of CeeSafe's array. Its place is `(*(A *)(uintptr_t)&(*p).f)` (through `uintptr_t`, as `p` may be a `const` pointer), where `A` is CeeSafe's array struct
  (`struct { CE e[N]; }`, c-backend.md §2), which has the array's size and alignment: an access through it is an
  access through an aggregate containing the member's type (C11 6.5p7). No copy is made.

### 31.4 The interpreter
`run --interp` has no headers, so it has no layout for an imported type. A function that accesses
a field of one is an interpreter fault (exit status 70) when it does so. Passing pointers to imported
types along is fine.

### 31.5 Profiles
Imported types are platform-neutral (`profiles.md` §5). They are allowed in every profile.
The same declarations work with CosmoOS's, Linux's or a C library's headers.

Diagnostics: N0290 (a misplaced attribute, a block without `@header`, a bad header string, a member
an imported struct cannot have), N0291 (a field type C and CeeSafe do not both spell), N0292 (a C name),
N0294 (an imported type used where CeeSafe would need its size, or a borrow of a pointer or size field).

## 32. Context effects (C7a; ADR 0009)

```ceesafe
extern "C" {
    @effects(sleep)
    fn mutex_lock(m: *mut mutex);
    @effects()
    fn spin_unlock(l: *mut spinlock_t);
}

/// Holding one is an atomic context: nothing may sleep while it is live.
@context(atomic)
struct SpinGuard { lock: *mut spinlock_t; drop { unsafe { spin_unlock(this.lock); } } }

@context(irq)
fn onInterrupt(count: borrow mut u64) { count += 1; }   // may neither sleep nor allocate
```

### 32.1 Effects
- There are three effects: **`sleep`** (the code may block the thread), **`alloc`** (it may allocate memory from a
  heap), and, since L1 (ADR 0031), **`suspend`** (it may suspend the current task: an `await`, §40.2).
- The effects of a function are those of every operation its body may perform, including everything it
  calls and every value it drops (drop glue, `drop` blocks, a box's deallocation). They are inferred over the
  whole package. Recursion is handled by iterating to a fixed point.
- Operations have these effects:

| Operation | Effects |
|---|---|
| a call of a CeeSafe function | that function's effects |
| a call of an `extern` function | its `@effects` if declared; otherwise `sleep` and `alloc` |
| a call through a C function pointer (§29.5) | `sleep` and `alloc` |
| a call through a function value or closure (§36) | its type's effect list (§36.3); without one, `sleep` and `alloc` |
| `Box.create`, and `Box.createIn` with a built-in allocator | `alloc` |
| `Box.createIn` with a user allocator (§30), dropping such a box | the effects of its `allocate`, or of its `deallocate` |
| allocating from an `Arena`, a `Scratch` frame or a `Pool`, making one; `Vector.withCapacity`, `push`; `String.append`, `withCapacity`; `StableVector.push`; `SlotMap.insert` | `alloc` |
| allocating from a `FixedBufferAllocator`; every other built-in operation on values, pointers and collections | none |
| console, file, socket and process operations (§26) | `sleep` (and `alloc` for `Process.run`) |
| dropping a value | the effects of its drop glue: `drop` blocks, the contents' drops, deallocations |
| `await e` (§40.2), and the library's `Async.park()` | `suspend`, and the effects of starting the awaited task (§40.6) |

### 32.2 Declaring effects
- `@effects(e, …)`, with each `e` one of `sleep`, `alloc` and `suspend`, and `@effects()` for none:
  - on an `extern` function, it **declares** what the C function may do. CeeSafe trusts it, as it trusts a
    prototype: a wrong declaration is a wrong binding (R-2);
  - on a CeeSafe function or method, it is an **upper bound**. The inferred effects must be among those
    declared (N0295, reported at an operation that has the extra effect).
- An unknown effect name, or `@effects` twice, is N0290.

### 32.3 Contexts
- `@context(atomic)` on a function forbids `sleep` and `suspend` in its body. `@context(irq)`, code that may run in a
  hardware interrupt, forbids `sleep`, `alloc` and `suspend` (`cosmoos/kernel-memory-and-lifetimes.md` KQ-1: IRQ
  handlers do not allocate). An operation with a forbidden effect is N0295.
- **An async fn or async closure body (§40) is a context that forbids `sleep`** (ADR 0031 §3.2): blocking would stall
  every task of its thread. `Async.blocking` (§40.4) is the bridge.
- `@context(atomic)` on a struct makes holding a value of it an atomic context. At every point of a function
  where a local that holds such a value (the struct itself, or a type containing it by value) may be live,
  `sleep` and `suspend` are forbidden (N0295): a spin guard may not be held across an `await`. A local is live from its initialisation until it is dropped, moved away or
  its storage ends, and a parameter from the function's entry. "May be live" joins over the control-flow
  paths. Dropping the holder itself is checked as if it were no longer live.
- A context restricts the body it is declared on, not callers: any code may call an `@context(irq)` function.
  A function called from an atomic context is checked through its inferred effects.
- Generic functions and structs are checked in each instantiation. An error is reported once.

### 32.4 What this proves
The checker proves that CeeSafe code performs no operation whose declared or inferred effects its context
forbids. It relies on the `@effects` of extern functions being right, and proves nothing about C code. It
does not prove freedom from deadlock or lock-order inversions. In CosmoOS, `might_sleep()` and lockdep remain
the runtime check behind it (ADR 0009).

Effects are checked in every profile. Only `@context`, `@effects` bounds on CeeSafe functions, and
`@context(atomic)` types create obligations, so code that uses none is unaffected.

Diagnostics: N0295 (an operation with an effect its context or `@effects` bound forbids; the message names the
chain of calls that leads to the operation), N0290 (misused `@effects` or `@context`, or an effect name other than
`sleep`, `alloc` and `suspend`).

### 32.5 Effects of closure arguments: `@effectsOf` (OD-9c; ADR 0020)
```ceesafe
@effectsOf(test)
fn count(values: Slice<i32>, test: borrow fn(i32) -> bool) -> i32 {
    var n = 0;
    for (v in values) { if (test(v)) { n += 1; } }    // not count's own effect: its caller's
    return n;
}

fn isEven(v: i32) -> bool => v % 2 == 0;

@context(atomic)
fn underLock(values: Slice<i32>, limit: i32) -> i32 {
    return count(values, (v) => v > limit) + count(values, isEven);   // allowed: neither closure sleeps
}
```
- **The attribute.** `@effectsOf(p, …)` on a function or method names some of its parameters. Each must be of a
  function or closure type (`fn(…)`, `borrow fn(…)`, `borrow mut fn(…)`, with or without an effect list), or it is
  N0336. It changes no type. With `@effects(…)`, the bound applies to the function's own effects.
- **Inside the function**, a call through such a parameter `p`, or through a local that holds `p` (copied, cast or
  reborrowed from it and assigned nothing else), is not one of the function's own effects. So is passing `p` as the
  argument for another function's `@effectsOf` parameter. Every other use is as before: `p` passed to a function
  without the attribute, stored, or captured has its type's effect list wherever it is called.
- **At a call**, the effects are the callee's own, plus, for each of its `@effectsOf` parameters, the argument's:

  | Argument | Its effects |
  |---|---|
  | a closure literal, or a local known to hold one (optimisations.md §5.1: every assignment to it is that literal, or copies or reborrows of a local that holds it) | the literal's body's inferred effects |
  | a named function (§36.1), adapted to a closure | that function's inferred effects: the adapter calls it directly |
  | the caller's own `@effectsOf` parameter, or a local that holds it | none: the caller's caller counts it |
  | anything else | its type's effect list (both effects without one, §36.3) |

  N0295 names the argument in its chain: "it calls 'count' with the closure at main.cee:12, which calls …".
- **As a value**, an `@effectsOf` function (§36.1) has its own effects plus each named parameter's type list.
- **Why this is sound.** A parameter is never assigned (N0211), and a closure or function value cannot be `var`
  (N0246), so every call through `p` during a call runs the argument the caller passed, whose effects the caller
  counted. Anything made from `p` other than a copy or reborrow is counted by its type, where it is called.
- lib/core's `sortBy`, `forEach`, `any`, `all` and `retain` (§36.4) are `@effectsOf` their closure, and so are
  `sortBy`'s helpers.
- **Async arguments** (ADR 0037). An `@effectsOf` parameter of closure type also takes an async closure literal
  (§40.3) whose task's result is the closure type's, or a named async fn of that signature (§36.1), unless the type's
  effect list lacks `suspend` (N0295). The call is then a task (§40.1): `await f(…)`, `Async.run(f(…))`,
  `Async.boxed(alloc, f(…))` or a spawn's body; N0342 elsewhere. It runs `f`'s **async instance** for those arguments,
  one per (`f`, the closures bound to its parameters), whether `f` is sync or async: a copy of `f` in which each call
  through such a parameter, or a
  local holding it, awaits the closure, and passing it on to another function's `@effectsOf` parameter awaits that
  function's instance.
  - The instance is checked as an async fn (§40.1), and its errors are reported at the call that made it, since its
    code may be the library's: a blocking call is N0295, and an instance that calls itself with its closure is N0343.
    Any other use of the closure (stored, captured, passed to a parameter without `@effectsOf`) is N0342: it would
    be called without an await.
  - The call's effects are `f`'s own, the closure's (`suspend` among them) and the instance's. Cancellation is the
    closure's (§40.7): `f` sees only what its awaits return.
  - `f` sees only values, so lib/core's functions work as they are: `await v.forEach(async (x) => …)`,
    `await v.retain(async (x) => …)`, `await s.sortBy(async (a, b) => …)` (its introsort is iterative, so its
    instance's frame has a size).

## 33. C state embedded by value (C7b; ADR 0009 §2)

```ceesafe
@header("kernel/spinlock.h")
extern "C" {
    @ctype("spinlock_t")
    struct spinlock_t;
    @effects()
    fn spinlock_init(lock: *mut spinlock_t, name: *CChar);
}

/// A lock and the data it protects; the C compiler lays it out, and it never moves.
struct SpinLock<T> {
    raw: spinlock_t;   // by value, zero-filled when made
    name: *CChar;
    data: T;

    /// Runs at the lock's final address, before anything can use it.
    @placed
    fn init(borrow mut this) { unsafe { spinlock_init(borrow mut this.raw as *mut spinlock_t, this.name); } }
}

fn make(name: *CChar) -> Result<Box<SpinLock<u64>, KernelHeap>, AllocError> =>
    Box.createIn(KernelHeap {}, SpinLock { name: name, data: 0 });   // `raw` is left out
```

### 33.1 C-laid-out types
- A struct field may have an imported C type (§31) by value. In a CeeSafe struct, it embeds that C state; in an
  imported struct, it declares an embedded member (`struct mutex { lock: spinlock_t; }`); in a `@repr(C)` struct, it
  is C state that C code may embed too (§28.5, ADR 0029).
- A type is **C-laid-out** if it is an imported type, an array of a C-laid-out type (C9), or a struct with a
  field of a C-laid-out type. Like
  an imported type (§31.2), it has no CeeSafe layout. Generated C defines a C-laid-out CeeSafe struct with its
  real member types, so the C compiler lays it out, and asserts nothing about its size, alignment or offsets.
  Allocation sizes come from C's `sizeof`, as for every type.
- A C-laid-out type appears by value only as a field of another struct, as the contents of a `Box`, or (C9) as
  the element of an array that itself appears so. As a local, parameter, result, optional, enum payload or any
  other type argument, it is N0294.

### 33.2 Made in place, never moved
- C state may point into itself once initialised (an empty list head points to itself), so a C-laid-out
  value is never moved, copied or assigned. Every move of one is N0294.
- The only way to make one is a **struct literal given directly to `Box.create` or `Box.createIn`**, or
  nested as a field of such a literal. Fields of imported type are left out of the literal and are
  **zero-filled** (C's `{0}`). Writing one is N0294: CeeSafe has no values of imported types.
- A C-laid-out value lives in its box until the box is dropped. Its contents are borrowed (`borrow`,
  `borrow mut`, through `Ptr.toBorrow` of a pointer to them) but never moved out. Dropping the box runs the
  contents' drop glue in place, then frees them.

### 33.3 `@placed` methods
- `@placed` on a method `fn m(borrow mut this)` with no other parameters and no result makes it the type's
  **placement initialiser**. A struct has at most one. Misuse is N0290.
- When `Box.create`/`Box.createIn` has placed a C-laid-out value in its box, and before it returns, the
  compiler calls the `@placed` methods of the value's fields' types, depth first, innermost first, in field
  order, and then the type's own. No other code can see the value before then. A `@placed` method runs C's
  initialisers (`spinlock_init`, `mutex_init`, `kobject_init`) at the value's final address.
- A box whose allocation fails runs no `@placed` method: the value is dropped, as in §18.2.
- The effects of `Box.create`/`Box.createIn` (§32.1) include those of the `@placed` methods they run.

### 33.4 The interpreter
`run --interp` has no C layout, so making, allocating or accessing a C-laid-out value is an interpreter
fault (exit status 70), as for imported types (§31.4).

Diagnostics: N0294 (a C-laid-out type by value outside a field or a box; a move of one; a literal of one
outside `Box.create`/`createIn`; an imported field in a literal), N0290 (a misused `@placed`).

## 34. Callbacks into CeeSafe, and raw boxes (C7d)

```ceesafe
@header("kernel/object.h")
extern "C" {
    struct kobject;
    struct kobject_type { name: *CChar; release: extern "C" fn(*mut kobject); }
}

/// The kernel calls this when the last reference goes; one C function per instance.
/// Safety: `obj` is the first field of a `KObject<T>` made by `KRef<T>.create`, and this runs once.
@callback
unsafe fn release<T>(obj: *mut kobject) {
    let object: Box<KObject<T>, KernelHeap> = Box.fromRaw(obj as *mut KObject<T>);
}   // dropping `object` drops the data and frees it

// … this.ktype.release = release<T>;   in KObject<T>'s @placed method
```

### 34.1 `@callback`: C addresses of CeeSafe functions
- `@callback` on a top-level function gives it a C address, as `@export` does (§29.5), without a
  symbol. Where an `extern "C" fn` type is expected, its name is a value of that type if the
  signatures are equal. A generic function is named with its type arguments (`release<T>` inside
  generic code, `release<u64>` outside), and each instance has its own C function.
- Its parameter and result types must be FFI-safe (§28.3) and must not mention its type parameters,
  so every instance has the same C signature (N0291). It may be an `unsafe fn`: its `Safety:` line
  states what the C caller guarantees. `@callback` with `@export` is N0290.
- Generated C gives each instance used as a value a `static` function with the C types, which
  converts its arguments and result as an export does (c-backend.md §2.3). Calling through the pointer
  is unsafe and has the `sleep` and `alloc` effects (§32.1). The callback's own body is checked like any
  other function's.

### 34.2 `Box.intoRaw` and `Box.fromRaw`
- `Box.intoRaw(b)` consumes the box `b: Box<T, A>` and returns `*mut T`, the address of its contents.
  Nothing is dropped or freed: the caller owns the memory, and leaks it unless it gives it back. It is
  safe (a leak is not undefined behaviour). It is for handing ownership to C: a kernel object, a deferred
  free.
- `Box.fromRaw(p)` (unsafe, §27.3) turns `p: *mut T` back into a box. The box type, and with it the
  allocator, comes from the expected type (`let b: Box<T, KernelHeap> = unsafe { Box.fromRaw(p) }`);
  without one it is N0205. Contract: `p` came from `Box.intoRaw` of a box of exactly that type, and is
  given back once.
- Neither applies to pool boxes (N0205). Neither has an effect. In the interpreter both work as in C.

### 34.3 A struct's first field
The fields of a struct, including a C-laid-out one (§33), are laid out in declaration order, so the
address of a struct is the address of its first field (C11 6.7.2.1p15). A pointer to the first field
converted with `as` is a pointer to the struct, which is how a kernel callback that receives the
embedded `struct kobject *` or `struct quiesce_head *` finds its object. Since C7d, a field of an
imported struct may also have a C function pointer type (§29.5), such as `kobject_type.release`.

Diagnostics: N0291 (a callback whose C signature is not FFI-safe or depends on its type
parameters), N0290 (`@callback` misused), N0205 (`Box.fromRaw` without a box type).

## 35. Floating-point types (M11 G7; ADR 0011)

```ceesafe
fn mean(values: Slice<f64>) -> f64 {
    var sum = 0.0;
    for (v in values) { sum += v; }
    return sum / round<f64>(values.length);
}

fn main() {
    let samples = [1.5, 2.25, 4.0];
    let m = mean(samples[..]);              // 2.5833…
    Console.writeLine(truncate<i64>(m * 1000.0));   // 2583: toward zero
    let x: f32 = 0.1;                       // the f32 nearest 0.1
    Console.writeLine(x.bits());            // 1036831949
}
```

### 35.1 Types and values
- `f32` and `f64` are IEEE 754 binary32 and binary64. Their layout comes from `TargetInfo` (size and alignment 4 and
  8 on both supported targets). They are Copy, need no drop, hold no loans, and are FFI-safe (C `float` and
  `double`, §28.3).
- The floating-point environment is the default one, and CeeSafe code never changes it: rounding to nearest, ties to
  even; no traps; status flags are never read.
- Every operation rounds its exact result once, to its type. There is no fused multiply-add and no evaluation in a
  wider type (§35.6).

### 35.2 Literals
- A float literal (grammar.md §1.5: `1.5`, `1e9`, `2.5e-3`, `6.02e23f64`) has type `f32` or `f64`. Its value is the
  decimal value rounded to nearest (ties to even) in that type, once: `0.1f32` is the `f32` nearest 0.1, not the
  `f64` nearest 0.1 rounded again.
- With a suffix, the type is the suffix's. Without one, the literal is *flexible* (§5): it takes an expected `f32`
  or `f64` (looking through an optional, as integer literals do) and otherwise defaults to `f64`. A flexible float
  literal where an integer type is expected, or an integer literal where a float type is expected, is N0205: there
  are no implicit conversions (write `1.0`, or `round<f64>(n)`).
- A literal whose value rounds to infinity in its type is N0206 (`1e39f32`). A literal that rounds to zero is
  zero.
- `-` before a literal is the negation operator (§35.3), so `-0.0` is negative zero.

### 35.3 Operators
| Operators | Operand types | Result | Behaviour |
|---|---|---|---|
| `+ - * /` | the same float type | that type | IEEE 754, rounded to nearest even. **Never panics**: overflow gives ±infinity, `x / 0.0` gives ±infinity, and `0.0 / 0.0`, `inf - inf` and `0.0 * inf` give NaN |
| unary `-` | float | same | flips the sign, also of zeros and NaNs |
| `== != < <= > >=` | the same float type | `bool` | IEEE comparisons: NaN is unequal to everything, itself included, and is neither less nor greater; `-0.0 == 0.0` |
| `+= -= *= /=` | | | as `x = x op y` (§6) |

- Float arithmetic is the one arithmetic that does not panic on an unrepresentable result (OD-6 is about
  integers; ADR 0011 Q2).
- `%`, the bitwise operators and the shifts are not defined on floats (N0207).
- Floats cannot be `match` scrutinees or patterns (N0207, §15.2).
- **NaN bits are not portable.** Which NaN an operation produces is not specified: x86-64 and AArch64 differ in its
  sign (inferred from their default NaNs). A NaN compares, converts and prints alike whatever its bits; only `bits()`
  (§35.5) shows the difference.

### 35.4 Conversions
- **`as`** stays lossless (§7): it converts a value only to a type that holds every value of the source type exactly.
  For floats, these are:
  - `f32 as f64`;
  - `i8`, `i16`, `u8` and `u16` as `f32` or `f64`;
  - `i32` and `u32` as `f64`.

  Any other `as` from or to a float type is N0208.
- **`round<T>(e)`**: `e` of any integer or float type, `T` a float type. The result is `e` rounded to nearest (ties
  to even) in `T`. It is always defined: an `f64` too large for `f32` becomes ±infinity, and NaN stays NaN.
- **`truncate<T>(e)`** with `e` of a float type and `T` an integer type rounds toward zero. It **panics** with
  "float out of range" if `e` is NaN or ±infinity, or if the rounded value does not fit in `T`. (`truncate` between
  integer types is unchanged, §7.) A float `T` is N0207: use `round`.
- **`checkedTruncate<T>(e)`**: as `truncate`, but the result is `T?`, `null` where `truncate` would panic.
- **`saturatingTruncate<T>(e)`**: as `truncate`, but a value below `T`'s range gives its minimum and one above gives
  its maximum. NaN gives 0. It never panics.
- `checkedTruncate` and `saturatingTruncate` need a float `e` and an integer `T`, and `round` a float `T` (N0207
  otherwise). All four take exactly one type argument (N0216).

### 35.5 Representation
- `f.bits()` is the IEEE representation of `f`: a `u32` for `f32`, a `u64` for `f64`. `f32.fromBits(u)` (`u: u32`) and
  `f64.fromBits(u)` (`u: u64`) make a float from a representation; every pattern is a value, some of them NaNs.
- The names `bits` and `fromBits` are reserved on float types, as the overflow methods are on integers (§6).

### 35.6 C and the interpreter
- `f32` and `f64` are C `float` and `double`. A float literal is emitted by its bits (`cee_f64_from_bits`), so its
  value never depends on the C compiler's decimal conversion.
- In a program that uses floats, `rt.h` requires `FLT_EVAL_METHOD == 0` (no wider evaluation) and IEEE formats
  (`FLT_RADIX == 2`, `FLT_MANT_DIG == 24`, `DBL_MANT_DIG == 53`), and disables contraction with
  `#pragma STDC FP_CONTRACT OFF` (clang) and `#pragma fp_contract(off)` (MSVC). The generated `main.c` asks for this
  with `#define CEE_FLOATS` when the program has a float local, parameter, result or constant. A program without
  floats needs none of it. That matters for kernels built with `-mgeneral-regs-only`: x86-64 then has no SSE, and
  `FLT_EVAL_METHOD` is not 0. gcc ignores the standard pragma, but does not contract under `-std=c11`,
  which CeeSafe's builds use. `ceesafec` also passes `-ffp-contract=off` to clang and gcc. Integrations that compile CeeSafe's
  C must not enable contraction or `-ffast-math` (c-backend.md §1).
- Conversions in C have no undefined behaviour:
  - `truncate` and its variants compare the value with the target's exact bounds before converting (C11 6.3.1.4);
  - `round<f32>` of an `f64` outside `f32`'s range gives the infinity directly, not through a C conversion
    (C11 6.3.1.5).
- The interpreter computes in CeeSafe's own `f32` and `f64` (`ceesafec`'s C `float` and `double`), which round the same way. Compiled and interpreted programs
  give the same results, NaN bits aside (§35.3).
- Not in G7 (ADR 0011 §3.8): `%`, a math library beyond §35.8, and other float formats. Parsing floats from text came
  with ADR 0071 (§16.6).

### 35.7 Printing (M11 G7b)
- `Console.write`, `Console.writeLine` and `String.appendNumber` accept `f32` and `f64` (and references to them).
- **The digits** are the shortest decimal that reads back as the same value. Reading back means rounding to nearest,
  ties to even, in the value's own type, so an `f32` prints with `f32`'s shortest digits: `0.1f32` prints as `0.1`.
  When several decimals of that length read back, the nearest to the value is printed, and on a tie the one with an
  even last digit.
- **The layout** is that of ECMAScript's Number::toString (ECMA-262 §6.1.6.1.20). Write the decimal as
  0.d1…dk × 10^n with no trailing zeros. Then:
  - if k ≤ n ≤ 21, the digits and n − k zeros: `100`, `100000000000000000000`;
  - if 0 < n ≤ 21, a point after n digits: `1.5`, `123456.789`;
  - if −6 < n ≤ 0, `0.`, then −n zeros, then the digits: `0.000001`;
  - otherwise one digit, a point and the rest if k > 1, `e`, the sign of n − 1 (`+` or `-`), and |n − 1|:
    `1e+21`, `1.5e-7`, `5e-324`.
- `NaN`, `Infinity` and `-Infinity`. Both zeros print as `0`, as in ECMAScript; `bits()` tells them apart.
- In C, the digits come from Schubfach (R. Giulietti, 2020; `runtime/hosted/ceesafe_rt_float.c`), with integer
  arithmetic only and a table of 617 powers of ten generated exactly. It differs from the paper's Java code in one
  place: a result may have one digit. Java needs at least two, so that code tries one digit fewer only from three
  digits on, and gives `4.9e-324`.

  The file is linked only into programs that print a float. The interpreter finds the same digits by an exact search
  over rationals instead (`src/interpreter/float_format.cee`).

### 35.8 Math methods (M11 G7c; ADR 0011 §5 Q5)
| Method | Result | Behaviour |
|---|---|---|
| `x.sqrt()` | same type | the square root, correctly rounded (IEEE `squareRoot`). `(-0.0).sqrt()` is `-0.0`; a negative operand or NaN gives NaN |
| `x.abs()` | same type | `x` with its sign cleared, NaN included |
| `x.floor()`, `x.ceil()` | same type | the nearest integral value below or above (IEEE `roundToIntegralTowardNegative`/`Positive`). Integral values, infinities and NaN are unchanged, and a zero result keeps `x`'s sign: `(-0.5).ceil()` is `-0.0` |
| `x.min(y)`, `x.max(y)` | same type | IEEE 754-2019 `minimum`/`maximum`: NaN if either is NaN, and `-0.0` is below `0.0`. `y` is checked against `x`'s type, so a flexible literal takes it |

- None of them panics, and none calls a C math library: CosmoOS has no libm.
  - `floor`, `ceil`, `abs`, `min` and `max` are exact portable C over integers and bits in `rt.h`. On baseline x86-64
    there is no instruction for `floor` or `ceil`; `roundsd` needs SSE4.1.
  - `sqrt` is the hardware square root, which is correctly rounded on both targets. It is reached through
    `__builtin_elementwise_sqrt` with clang and `__builtin_sqrt` with gcc, built with `-fno-math-errno` (ceesafec passes
    it), so it never sets `errno` or calls `sqrt`. MSVC uses its C runtime's `sqrt`.
- The interpreter uses CeeSafe's own methods of the same names, so it computes what compiled code does.
- The eight names `bits`, `fromBits`, `sqrt`, `abs`, `floor`, `ceil`, `min` and `max` are reserved on float types: an
  interface method of one of those names cannot be implemented for a float type (N0228). Wrong argument counts are
  N0209.

### 35.9 `--no-float` (M11 G7c; ADR 0011 §3.7)
- `ceesafec build --no-float` and `ceesafec check --no-float` reject every float type, float literal and `f32.fromBits`/
  `f64.fromBits` with N0297. Every float value starts at one of those, so nothing of the program computes with
  floats.
- It is meant for kernels and other code built without FP registers. CosmoOS's kernel builds pass it (`c4.py` to
  `c8.py`), and `check-fpregs.sh` remains the backstop. Floats in the generic freestanding profile are otherwise
  allowed (`profiles.md` §1).
- The libraries are not checked this way: lib/core's float parser (§16.6) is reached only through `parseFloat<f64>` or
  `parseFloat<f32>`, whose type argument the program writes and N0297 rejects, and a function nothing calls is not
  emitted (ADR 0071 §2.5).
- In the corpus, a line `// option: no-float` checks a file this way (testing.md §3).

Diagnostics: none new. Mismatched float types, and float literals where an integer is expected, are N0205.
A literal that overflows is N0206. Operators that are not defined on floats are N0207, and so are float match
scrutinees and wrong argument or target types of the conversions. A lossy `as` is N0208. Wrong type-argument counts
are N0216.

## 36. Function values and closures (M11 G8; ADR 0012)

### 36.1 Function types and values (G8a)
```ceesafe
fn double(x: i32) -> i32 => x * 2;
fn negate(x: i32) -> i32 => -x;

fn applyTwice(f: fn(i32) -> i32, x: i32) -> i32 => f(f(x));

fn main() {
    Console.writeLine(applyTwice(double, 5));       // 20
    let pick: fn(i32) -> i32 = if (true) negate else double;
    Console.writeLine(pick(3));                     // -3
    let id = identity<i64>;                         // a generic function, named with its type arguments
    Console.writeLine(id(7));
}

fn identity<T>(x: T) -> T => x;
```
- **The type** `fn(T1, …, Tn) -> R` (`fn(T1, …, Tn)` when `R` is `void`) is a plain function value: one code
  address. It is Copy, needs no drop, holds no loans (it is not scoped), and is not FFI-safe: C function pointers stay
  `extern "C" fn(…)` (§29.5). Two function types are the same type when their parameter and result types are.
- **Values.** The name of a package function is a value of its function type:
  - wherever a value can be (`let f = double;`), or where a function type is expected, which must then equal the
    function's;
  - a generic function is named with its type arguments (`identity<i64>`), giving that instance.

  Methods are not values: they are only named through their type or receiver. These functions are not values
  either, N0298 (a new code):
  - `extern` and `unsafe` functions, since a call through a value is safe;
  - functions with region parameters (§25), since their signatures can store an argument's loans in another
    argument, which a function type cannot say;
  - `main`.
- **Calls.** `f(args)`, with `f` a local, parameter or field of a function type, calls the function it holds. The
  arguments are checked against the parameter types as in any call (§8, §13.2). The call is safe. Its effects are
  the type's effect list (§36.3), else `sleep` and `alloc` (§32.1), as for a C function pointer.
- **References.** Parameter and result types may be references or other scoped types. A reference result needs
  exactly one reference (or scoped) parameter: a function type has no names to write a source list with (N0245,
  §13.5). The borrow checker gives a call's result the loans of every reference argument. That is sound for every
  function the value can hold, because such a function's result can only come from its parameters (no region
  parameters, above).
- **Not in G8a:** `==` on function values (N0207). Closures are §36.2, and effect lists on function types §36.3.
- **C.** A function type is a C function pointer typedef `ceeN_…` over the functions' C signatures. A value is the
  function's address, and a call is an indirect call.

### 36.2 Closures (G8b)
```ceesafe
fn countIf(values: Slice<i32>, keep: borrow fn(i32) -> bool) -> i32 {
    var n = 0;
    for (v in values) { if (keep(v)) { n += 1; } }
    return n;
}

fn forEach(values: Slice<i32>, visit: borrow mut fn(i32)) {
    for (v in values) { visit(v); }
}

fn main() {
    let values = [3, 8, 1, 9, 4];
    let limit = 4;
    Console.writeLine(countIf(values[..], (v) => v > limit));     // 2: captures `limit`
    var total = 0;
    forEach(values[..], (v) => { total += v; });                   // changes `total`: a borrow mut fn
    Console.writeLine(total);                                       // 25
    let twice = (x: i32) => x * 2;                                  // captures nothing: a plain fn(i32) -> i32
    Console.writeLine(twice(21));
}
```
- **Literals** (grammar.md §2): `(x: i32, y) => expression` or `(x) => { statements }`, with `()` for none. A `(` … `)` followed
  by `=>` is a closure; a parenthesised expression cannot be followed by `=>`. The body is a block or an expression,
  which extends as far right as an expression can (as for branches, §15.3).
- **Parameter types** are written, or taken from an expected function or closure type. The **result type** is the
  expected type's, else the expression body's type, else `void` for a block. A parameter without a type and without an
  expected type, or a block body that returns a value without an expected type, is N0236 (a new code).
- **Captures.** The body may name the enclosing function's locals and parameters, `this` included, also from nested
  closures. Each one named is *captured by reference*:
  - **mutably** if the body assigns to it or to a place rooted at it, passes it as `mut`, borrows it mutably, or calls
    a `borrow mut this` method or mutating collection method on it;
  - **shared** otherwise.
  - **Field-precise** (M13 G5): a captured struct local that the body reaches only through its fields (`s.width`,
    `s.items[s.next]`), never whole and not from a nested closure, is captured as one reference per field it names,
    so the closure borrows those fields and the enclosing code may use the others. The mode (shared or mutable) is
    still decided per local. A local behind a reference (`this`) is captured whole.
- **Types.**
  - A literal that captures nothing is a plain function value, `fn(…) -> R` (§36.1).
  - One that captures is a **closure**: `borrow fn(…) -> R` if every capture is shared, `borrow mut fn(…) -> R` if any is
    mutable.
  - A closure type is a reference (§13): `borrow fn` is Copy, and `borrow mut fn` is unique and reborrowed at each
    call. Both are scoped, and allowed where references are (§13.1, §25): parameters, `let` locals, fields of aggregates
    with region parameters (`borrow(r) fn(…)`), and elements of scoped collections.
  - In source, `borrow fn(…)` always means a closure type: a reference to a plain function value cannot be written.
- **Loans.** A closure's value carries the loans of its captures, shared or mutable, and a loan of its *environment*.
  The environment is a hidden local of the block that contains the literal, holding references to the captures.
  So, by the rules that already apply to references (B1–B5, `lifetimes.md`):
  - a captured variable cannot be changed, moved or ended while a closure that captures it is live;
  - nothing else may use a mutably captured one then;
  - a closure cannot be used after its block, nor returned from its function (N0243, N0244).
- **Conversions.**
  - Where a closure type is expected, a plain function value converts to it (an adapter closure). For a named
    function the adapter calls it directly; for any other value it holds a copy and calls through it.
  - A `borrow fn` converts to a `borrow mut fn`: calling a shared closure changes nothing.
  - A `borrow mut fn` where a `borrow fn` is expected is N0299 (a new code): a shared copy could call it while it is
    changing what it captures. So is a capturing closure where a plain `fn` is expected.
- **Calls** `f(args)` are as for function values (§36.1): safe, with the effects of the type's effect list (§36.3), else `sleep` and `alloc`.
- **In the body**, `return` returns from the closure. `break` and `continue` cannot leave it (N0214, as outside a loop).
  `?` propagates from the closure, whose result type must allow it (N0261). A captured value cannot be moved out:
  the body reaches it through a reference (N0251).
- **C.** The environment is a struct `ceeE_…` whose first member is the code pointer, followed by one pointer per
  capture. The closure's value is a pointer to that first member (C11 6.7.2.1p15). The body is a `static` function
  taking `const void *env` first. A call is `(*f)(f, args…)`. The code pointer's member is `f_0code`, and a
  field-precise capture's `f_0<local>__<field>`: the digit, which starts no identifier, keeps them apart from a
  captured local's `f_<local>`, whatever its name.

### 36.3 Effect lists on function and closure types (G8b′; ADR 0012 §3.7)
```ceesafe
extern "C" { @effects() fn spin_lock(l: *mut spinlock_t); @effects() fn spin_unlock(l: *mut spinlock_t); }

@context(atomic)
fn countUnderLock(lock: *mut spinlock_t, values: Slice<i32>, keep: borrow @effects() fn(i32) -> bool) -> i32 {
    unsafe { spin_lock(lock); }
    var n = 0;
    for (v in values) { if (keep(v)) { n += 1; } }    // allowed: `keep` neither sleeps nor allocates
    unsafe { spin_unlock(lock); }
    return n;
}

fn isEven(v: i32) -> bool => v % 2 == 0;
fn noisy(v: i32) -> bool { Console.writeLine(v); return true; }

fn demo(lock: *mut spinlock_t, values: Slice<i32>) {
    let limit = 4;
    let quiet: @effects() fn(i32) -> bool = isEven;    // checked: `isEven` has no effects
    countUnderLock(lock, values, (v) => v > limit);    // the literal's body is held to @effects()
    countUnderLock(lock, values, noisy);               // N0295: 'noisy' may sleep: it does console I/O
}
```
- **Syntax** (grammar.md §2): `@effects(e, …)` written immediately before `fn` in a function type, with each `e`
  `sleep`, `alloc` or `suspend` as in §32.2: `@effects() fn(i32) -> bool`, and for closures `borrow @effects() fn(i32)` and
  `borrow mut @effects(alloc) fn(i32)`. No other attribute is allowed there (N0290), and only `fn` may follow
  it (a syntax error otherwise): C function pointer types (`extern "C" fn`, §29.5) take no effect list.
- **The type.** The effect list is part of a function or closure type: `@effects() fn(i32)` and `fn(i32)` are different
  types. A type without a list may have `sleep` and `alloc`, so `fn(i32)` is the same type as
  `@effects(sleep, alloc) fn(i32)`. It never has `suspend`: only an async closure's call suspends, and that is a task
  (§40.3), not an effect of a sync call. A list may name `suspend`: an `@effectsOf` parameter of such a type takes an
  async closure (§32.5).
  Everything else (Copy, scoped, loans, C representation) is as in §36.1 and §36.2.
- **Calls** through a value of such a type have the type's effects (§32.1). Without a list, that is `sleep` and `alloc`.
- **Values.** Every value of a type with an effect list has at most those effects:
  - a **named function** used where such a type is expected (§36.1) is checked after effect inference: its inferred
    effects must be among the list (N0295, reported where it is named, with the chain of calls);
  - a **closure literal** used where such a type is expected has the list as its body's upper bound, as `@effects` is
    on a function (§32.2; N0295 in the body). So does the adapter closure of a plain function value (§36.2);
  - otherwise a value converts only to a type whose list includes its own: `@effects() fn(i32)` converts to
    `@effects(alloc) fn(i32)` and to `fn(i32)`, and `borrow @effects() fn(i32)` to `borrow fn(i32)` (with the kind
    conversions of §36.2). The reverse is N0299: a value whose effects are not known to be in the list.
- **Inference does not narrow types.** A named function or closure literal with no expected type gets the type
  without a list, whatever its effects: `let f = isEven;` is a `fn(i32) -> bool`, which cannot be passed where an
  `@effects() fn(i32) -> bool` is expected. Write the type (`let f: @effects() fn(i32) -> bool = isEven;`).
- **C.** The list changes nothing in C: the typedefs of two types that differ only in their lists name the same C
  type, so a conversion between them needs no cast.

### 36.4 Library methods taking closures (G8c; ADR 0012 §3.9)
```ceesafe
fn main() {
    var values = [5, 3, 9, 1, 4];
    values[..].sortBy((a, b) => a - b);                    // 1 3 4 5 9
    var total = 0;
    values[..].forEach((v) => { total += v; });            // 22
    Console.writeLine(values[..].any((v) => v > 8));       // true
    Console.writeLine(values[..].all((v) => v % 2 == 1));  // false
    var names = Vector<String>.create();
    // … push some names …
    names.sortBy((a, b) => a.view().length as i32 - b.view().length as i32);   // move-type elements: fine
    names.retain((n) => n.view().length > 3);
}
```
- **Methods.** Each takes a closure (§36.2); a plain function value converts to one, so `s.sortBy(compare)` works.
  The closure's parameters are references to the elements, so the element type may be any type, Copy or not:

| Method | Receiver | Closure type | Result |
|---|---|---|---|
| `s.sortBy(cmp)` | `SliceMut<T>`, or a vector (lending its elements mutably) | `borrow mut fn(borrow T, borrow T) -> i32` | `void` |
| `s.forEach(visit)` | `Slice<T>` (a `SliceMut<T>` coerces), or a vector | `borrow mut fn(borrow T)` | `void` |
| `s.any(test)`, `s.all(test)` | as `forEach` | `borrow mut fn(borrow T) -> bool` | `bool` |
| `v.retain(keep)` | a `Vector<T, A>` (a mutable place) | `borrow mut fn(borrow T) -> bool` | `void` |

  The closure types are `borrow mut fn`, so a closure that changes what it captures may be passed, and so may a
  shared one (§36.2). A slicing expression `a[lo..hi]` is a receiver as for `fill` (§16.5): `a[..].sortBy(cmp)`.
- **`sortBy`** orders the elements so that `cmp(borrow s[i], borrow s[i + 1]) <= 0` for each `i`, when `cmp` is a
  consistent total order: negative for "less", zero for "equal", positive for "greater".
  - It is **not stable**: equal elements may end in any order. The order chosen is a deterministic function of the
    input and of `cmp`'s results.
  - It allocates nothing, calls `cmp` O(n log n) times in the worst case, and uses O(log n) stack: an introsort
    (ADR 0057): quicksort with a median-of-three pivot and a partition without a branch on `cmp`'s result; the
    elements equal to an earlier pivot set aside in one pass; insertion sort below 17 elements; heapsort past a
    depth of 2·log₂ n. A slice that is one ascending run from the start is left as it is, and one strictly
    descending run is reversed, after at most n − 1 calls of `cmp`.
  - If `cmp` is not a consistent order, or changes what it captures, the elements end in some order: they are
    still a permutation of the input, and nothing panics or misbehaves.
- **`forEach`** calls `visit` on each element in index order. **`any`** calls `test` in index order until it returns
  `true` (then `true`; `false` for an empty slice), and **`all`** until it returns `false` (then `false`; `true` for
  an empty slice).
- **`retain`** calls `keep` once on each element in index order. It keeps the elements for which it returned `true`,
  in their order, and destroys the others after the last call. The vector's capacity is unchanged.
- **Loans.** The receiver is evaluated first and lent for the whole call (mutably for `sortBy`, `swap` and `retain`),
  so the closure cannot capture it: `v.retain((x) => x < v.length)` is N0241, as for any other conflicting loan.
- **Effects.** The methods are `@effectsOf` their closure (§32.5): a call has the closure's effects, plus `retain`'s
  destruction of the elements it removes. So `s.sortBy((a, b) => a - b)` may be used in `@context(atomic)` and
  `@context(irq)` code, and `s.sortBy(compare)` may too if `compare` neither sleeps nor allocates.
- **Implementation.** The methods are CeeSafe code in `lib/core/slices.cee`: static functions of the generic structs
  `SliceAlgorithms<T>` and `VectorAlgorithms<T, A>`, which the method calls name (so `SliceAlgorithms<i32>.sortBy(s,
  cmp)` is the same call). They use only safe CeeSafe and `swap` (§16.5).

### 36.5 Owning closures: `move` literals and `dyn fn` (M13 G3; ADR 0015 §3.6)
```ceesafe
fn adder(n: u64) -> Box<dyn fn(u64) -> u64> {
    return Box.create(move (x: u64) => x + n)!;      // n is moved (here, copied) into the closure
}

fn counter() -> Box<dyn mut fn() -> u64> {
    var count: u64 = 0;
    return Box.create(move () => { count += 1; return count; })!;
}

fn main() {
    let add2 = adder(2);
    Console.writeLine(add2(3));                      // 5
    var next = counter();
    next();
    Console.writeLine(next());                       // 2
    var names = Vector<String>.create();
    names.push(String.create())!;
    let size: Box<dyn fn() -> usize> = Box.create(move () => names.length)!; // a closure owning the vector: Box<dyn fn() -> usize>
    Console.writeLine(size());                       // 1
}
```
- **Types.** `dyn fn(T…) -> R` and `dyn mut fn(T…) -> R` (with an optional effect list, `dyn @effects() fn(…)`) are
  `dyn` types (§37): unsized, so they appear only behind a reference or in a box. `Box<dyn fn(T…) -> R, A>` is an
  **owning closure**: a move type that needs drop, which may be returned, stored in fields and collections, and
  outlive the function that made it. `borrow fn(…)` (§36.2) is unchanged and is not written `borrow dyn fn`.
- **Move literals.** `move (x: T, …) => body` is a closure literal that captures **by move**: each local it names is
  moved into the closure when the literal is evaluated, or copied if its type is Copy. Afterwards the local is moved
  out (§14.3), as after any move. The parameter rules, `return`, `break` and `?` are those of §36.2.
- **Where.** A move literal may only be the value argument of `Box.create(…)` or `Box.createIn(a, …)` whose expected
  result is a `Box<dyn fn…, A>` or a `Box<dyn mut fn…, A>`. Its parameter and result types come from that type.
  Anywhere else it is **N0333** (its own type is unnamed; OD-9a).
- **Captures.** A move literal may not capture a scoped value (§19.1): a reference, a view, a slice, a closure, or a
  value with region parameters. That is **N0332**: the box would outlive the loan.
- **Mutation.** A body that assigns to a capture, borrows it mutably or calls a mutating method on it changes the
  closure's own copy. It makes the literal a `dyn mut fn`: where a `dyn fn` is expected, that is N0299. A captured
  value cannot be moved out of the body (N0251): it belongs to the closure, which may be called again.
- **Calls.** `f(args)` where `f` is a place of type `Box<dyn fn…>`, `borrow dyn fn…`, or a reference to such a box,
  calls the closure. A `dyn mut fn` needs mutable access: a `var` box, or a `borrow mut`, else N0248. A call has the
  effects of the type's effect list, else `sleep` and `alloc` (§36.3).
- **Drop.** Dropping the box destroys the captured values, in capture order, then frees the box.
- **Representation.** The closure's environment, a struct of the captured values after the code pointer, is the box's
  contents. The box is a `dyn` pair (§37.4) whose table has one entry, the body.

Diagnostics: N0332 (a scoped capture), N0333 (a move literal outside `Box.create`).

## 37. Dynamic dispatch: `dyn` (M13 G2; ADR 0015)
```ceesafe
interface Shape {
    fn area(borrow this) -> u64;
    fn grow(borrow mut this, by: u64);
}

struct Square { side: u64; }
impl Shape for Square {
    fn area(borrow this) -> u64 => this.side * this.side;
    fn grow(borrow mut this, by: u64) { this.side += by; }
}

struct Rect { w: u64; h: u64; }
impl Shape for Rect {
    fn area(borrow this) -> u64 => this.w * this.h;
    fn grow(borrow mut this, by: u64) { this.w += by; }
}

fn total(shapes: borrow Vector<Box<dyn Shape>>) -> u64 {
    var t: u64 = 0;
    for (s in shapes) { t += s.area(); }          // a call through the table
    return t;
}

fn main() {
    var shapes = Vector<Box<dyn Shape>>.create();
    shapes.push(Box.create(Square { side: 3 })!)!; // Box<Square> coerces to Box<dyn Shape>
    shapes.push(Box.create(Rect { w: 2, h: 5 })!)!;
    Console.writeLine(total(shapes));              // 19
    var sq = Square { side: 1 };
    let r: borrow mut dyn Shape = borrow mut sq;   // a reference coerces too
    r.grow(2);
    Console.writeLine(sq.side);                    // 3
}
```

### 37.1 The type `dyn I`
- For an interface instantiation `I` (§21: `Shape`, `Comparable<i32>`), **`dyn I`** is the type of "a value of some
  type that implements `I`". Its size is not known, so, like `[T]` (§16), it is **unsized**: it appears only behind
  a pointer.
  - `borrow dyn I` and `borrow mut dyn I` are references (§13). They are scoped, follow every reference rule, and
    `borrow dyn I` is Copy.
  - `Box<dyn I, A>` (and `Box<dyn I>` for `SystemHeap`) owns its value (§18.2). It is a move type that needs drop.
  - Anywhere else, `dyn I` is **N0330**: a local, parameter or result of type `dyn I`, a field, an array or vector
    element, an optional, a type argument (`Vector<dyn I>`; `Vector<Box<dyn I>>` is fine), or an element of a slice
    type.
- **Dyn-compatible interfaces.** `dyn I` is allowed only if every method of `I` takes `borrow this` or
  `borrow mut this`. A method that takes `this` by value makes the interface incompatible: `dyn I` is then
  **N0331**, which names that method. A generic interface is checked once its type arguments are given. An async
  method does not make an interface incompatible (§40.1, ADR 0036).
- `dyn` is a keyword in types only (grammar.md §2). `dyn I` binds tighter than `borrow`, so `borrow dyn I` is a
  reference to a `dyn I`.

### 37.2 Coercions
- A value converts to a `dyn` type only where that type is **expected**: an annotated `let`, an argument, a field
  initialiser, a `return`, or an argument of a collection method such as `push`. Inference never invents a `dyn`
  type.
  - `borrow T` → `borrow dyn I`, and `borrow mut T` → `borrow mut dyn I` (or `borrow dyn I`). A `borrow mut T`
    variable or field converts through a reborrow, as every use of a `borrow mut` does (§13.3): it stays usable
    after the reborrow ends, and is frozen while it lives (B4);
  - `Box<T, A>` → `Box<dyn I, A>`: a move of the box, with no new allocation. The allocator type must be the same.
- `T` must implement `I` (§21.1), else **N0268**, as for an unsatisfied bound.
- **Scoped `T` behind a reference** (ADR 0064 §3.2, OD-10a). A type with region parameters that implements `I`
  (§25.3), such as `View<r>`, coerces to `borrow dyn I` and `borrow mut dyn I`. The `dyn` reference carries the loan
  of the reference it came from. As for any `borrow(a) View<r>`, `r` outlives `a`, so the referent's loans outlive
  the `dyn` reference, and borrowing the value keeps its loans alive (B4). No region is written on the `dyn` type.
  Other scoped types (references, closures, views, slices) are not `dyn` (they implement no interface), and
  `Box<dyn I>` takes no scoped `T` (**N0268**): a box owns its value, and its type keeps no region.
- `T` must be a sized type: a struct, an enum, or a built-in type such as `i32`. A `dyn I` does not coerce to another
  `dyn` type, and there is no conversion back to `T` (no downcasting; OD-10b).
- **`dyn I` implements `I`** (ADR 0064 §3.1). For bounds, `dyn I` (and `dyn I + Send`) implements its own
  interface, with the same type arguments: each method calls through the value's table. So a generic
  `fn layout<M: Metrics>(m: borrow M)` takes a `borrow dyn Metrics` as well as a `borrow Fixed`, with `M = dyn Metrics`.
  `dyn I` is unsized, so it may be a type argument only where the generic holds it behind a reference or a box:
  - A function instance whose parameters, locals or result hold a `dyn` value by value is N0222 at the use, with the
    reason.
  - A user struct or enum instantiated with `dyn I` (`ByRef<dyn I>`, written or inferred) is N0330 if it holds the
    value by value (`struct Wrap<T> { inner: T }`).

### 37.3 Calls
- `x.m(args)`, where `x` is a `borrow dyn I`, a `borrow mut dyn I`, a `Box<dyn I, A>`, or a place of one of these
  types, calls the method `m` of `I` through the value's table. The method is that of `T`'s `impl I for T`.
- The receiver rules are those of a reference to `T` (§13, §21.1):
  - a `borrow this` method may be called through any of the three;
  - a `borrow mut this` method needs mutable access: a `borrow mut dyn I`, or a mutable place of `Box<dyn I, A>`.
    Through a `borrow dyn I` it is N0248, as for any value behind a shared reference.
- Only `I`'s methods can be called. `T`'s other methods and fields are not visible through `dyn I` (N0234).
- **Async methods** (§40.1, ADR 0036). For an async method, `x.m(args)` is a task (N0342 unless awaited, run or
  boxed): `await x.m(args)` has the method's result type, as through `T: I`. The receiver and the arguments are lent
  to the callee's frame until it returns.
- **Effects** (§32). An interface method may carry an effect list, which bounds every `impl` of it:

  ```ceesafe
  interface Visitor {
      @effects() fn visit(borrow mut this, value: u64);
  }
  ```

  An `impl` method whose effects exceed its interface's list is N0295, reported at the `impl` method. A call through
  `dyn I` has the effects of `I`'s list, or `sleep` and `alloc` without one, as for a closure without a list (§36.3);
  an async method without one has `suspend` and `alloc`, since an async body cannot sleep (ADR 0036).
  A call on a generic `T: I` is unchanged: it has the effects of the instantiated method (§21.2).
- **Drop.** Dropping a `Box<dyn I, A>` destroys its value with `T`'s drop glue, then frees it with `A`.

### 37.4 Representation (c-backend.md §2)
- A `borrow dyn I`, a `borrow mut dyn I` and a `Box<dyn I, A>` with a zero-sized `A` are each two words:
  `{ data, vt }`. `data` points at the value, and `vt` points at the table for (`T`, `I`).
- The table is a `static const` struct per pair (`T`, `I`) that a coercion names. It holds the drop glue, the value's
  size and alignment (for a user allocator's `deallocate`), then one function per method of `I`, in declaration
  order. Each function is a generated adapter taking `void *` as its receiver, so no function is called through a
  pointer of another type. An async method's entry starts `T`'s method in the frame of a call through the table, and
  the table also holds the size of that frame for `T` (§40.6, c-backend.md §3a).
- `dyn I` values have no address identity beyond `data`. Two `borrow dyn I` values are not comparable (N0209).

Diagnostics: N0330 (`dyn I` not behind a pointer), N0331 (an interface that is not dyn-compatible).

## 38. Threads (M13 G4; ADR 0016)
```ceesafe
struct Counter { n: u64; }

fn main() {
    let counter = Arc<Mutex<Counter>>.create(Mutex<Counter>.create(Counter { n: 0 })!)!;
    var workers = Vector<Thread<u64>>.create();
    for (i in 0..4u64) {
        let mine = counter.clone();                    // another owner, moved into the thread
        workers.push(Thread.spawn(move () => {
            var guard = mine.get().lock();
            let data = guard.data();
            data.n += i + 1;
            return i * 10;
        })!)!;
    }
    var total: u64 = 0;
    loop {
        match (workers.pop()) {
            Some(t) => { total += t.join(); }
            null => { break; }
        }
    }
    Console.writeLine(total);                          // 60
    var guard = counter.get().lock();
    Console.writeLine(guard.data().n);                 // 10
}
```
- Static functions of the library's generic types take their type arguments, as for any generic struct (§20.2):
  `Arc<Mutex<Counter>>.create(…)`.

### 38.1 `Send` and `Share`
- Two properties of types, **inferred** and never declared by user code (v1), which may also be used as bounds, like
  `Copy` (§21.2): `<T: Send>`, `<T: Send + Share>`.
- **`Send`**: a value may be moved to another thread. It holds for:
  - integers, floats and `bool`;
  - structs and enums whose fields and payloads are all `Send`, and arrays of a `Send` element;
  - `Box<T>`, `Vector<T>`, `String`, `SlotMap<T>`, `StableVector<T>` and `Handle<T>`, with `T: Send` and the system
    heap (or `FailingAllocator`);
  - `File`, `TcpListener`, `TcpStream`;
  - `Box<dyn I + Send>` and `Box<dyn fn(…) -> R + Send>` (§38.4);
  - the library's thread-safe types (§38.3): `Arc<T>`, `Mutex<T>`, the atomics.
- It fails for:
  - references, slices, views and every other scoped type (§19.1);
  - closures (`borrow fn`), function values aside (`fn(…)` is `Send`: a code address);
  - raw pointers and imported C types;
  - a box or collection with a user allocator;
  - regions and pools;
  - `Thread<T>` (§38.2);
  - `Box<dyn I>` and `Box<dyn fn…>` without `+ Send`.
- **`Share`**: shared references to a value may be used from several threads at once. It holds for every `Send` type,
  since no v1 type can be changed through a shared reference except the library's, and those are `Share` by design.
  The exception is `Thread<T>`.
- Inference is structural and recursive. A recursive type is `Send` unless a path through it reaches a type that is
  not. Generic code knows only its bounds: a `T` is `Send` only under a `T: Send` bound.

### 38.2 Spawning and joining
- **`Thread.spawn(move () => body)`** starts a thread that runs the move literal's body (§36.5) and returns
  `Result<Thread<T>, SpawnError>`, where `T` is the body's result type. `SpawnError` is the enum
  `SpawnError.Resources` (the thread or its closure could not be allocated).
  - The literal takes no parameters. Its captures must be `Send` (**N0334**), and so must `T` (**N0334**).
  - `Thread.spawn` also takes a `Box<dyn fn() -> T + Send>`.
  - A move literal is allowed here as well as in `Box.create` (amending §36.5).
- **`Thread<T>`** is a move type that needs drop. `t.join()` consumes it, waits for the thread to finish, and
  returns its `T`. Dropping a `Thread<T>` that was not joined waits for it and destroys its result. There are no
  detached threads: every thread a program starts has ended before `main` returns.
- A panic in any thread aborts the process (OD-5).
- **Effects** (§32): `spawn` has `sleep` and `alloc`. `join`, and dropping a `Thread<T>`, have `sleep`.

### 38.3 Shared state (library types: `lib/std/sync.cee`, and the atomics in `lib/core/atomics.cee`)
- **`Arc<T: Share>`**: shared ownership.
  - `Arc.create(v)` returns `Result<Arc<T>, AllocError>`. `a.clone()` returns another owner of the same value, and
    `a.get()` returns a `borrow T` (shared only).
  - The count is atomic. The last owner's drop destroys the value and frees it.
- **`Mutex<T: Send>`**: mutual exclusion.
  - `Mutex.create(v)` returns `Result<Mutex<T>, AllocError>`; the platform lock is allocated.
  - `m.lock()` waits for the lock and returns a `MutexGuard<T>`, scoped to `m` like C7's guards. Its `data()` lends
    `borrow mut T`, and its drop unlocks.
  - `lock` has the `sleep` effect.
- **Atomics**: `AtomicU32`, `AtomicU64`, `AtomicUSize` and `AtomicBool`, in every profile (ADR 0026). Each operation
  is a static inline function of the runtime header, so it compiles to the machine's atomic instructions.
  - `create(v)`, then `load()`, `store(v)`, `swap(v)` and `compareExchange(expected, new)` (returns
    `Result<T, T>`, `Err` with the current value).
  - The integer types also have `fetchAdd(v)` and `fetchSub(v)`, which wrap, and (ADR 0041) `fetchOr(v)` and
    `fetchAnd(v)`; each returns the value before the change.
  - **Layout.** `AtomicU32` and `AtomicU64` are laid out exactly as `u32` and `u64` (size and alignment, checked by
    `_Static_assert` in every unit), and `AtomicUSize` as `usize`; so an atomic may be a field of a `@repr(C)` or
    imported struct, as its integer member (§28.5, §31.1; ADR 0041). `AtomicBool` is a `u32`.
  - Every operation takes `borrow this`, is sequentially consistent, and has no effect.
  - **With an ordering** (ADR 0021): `loadWith(o: LoadOrdering)`, `storeWith(v, o: StoreOrdering)`, and
    `swapWith`, `compareExchangeWith`, `fetchAddWith`, `fetchSubWith`, `fetchOrWith`, `fetchAndWith` taking
    `o: Ordering` (`AtomicBool` has the first four). The enums are `LoadOrdering { Relaxed, Acquire, SequentiallyConsistent }`, `StoreOrdering { Relaxed, Release,
    SequentiallyConsistent }` and `Ordering { Relaxed, Acquire, Release, AcquireRelease, SequentiallyConsistent }`, with
    C11's meanings (C11 §7.17.3). A load cannot be given release ordering, nor a store acquire, which C11 leaves
    undefined. A compare-exchange that fails orders as a load: `AcquireRelease` as `Acquire`, `Release` as `Relaxed`.
  - An ordering changes which values an operation may observe, never whether two accesses race: every access is
    still atomic, so T1–T3 are unchanged. A too-weak ordering is a logic error that neither the interpreter (which runs
    one thread at a time, a sequentially consistent execution) nor ThreadSanitizer can find.
- These types are `Send` and `Share` by the library's declaration (`@threadSafe`, accepted only in the library's
  own files). Their `unsafe` internals are argued in `ownership.md` T1–T3.

### 38.4 `dyn … + Send`
- `dyn I + Send` and `dyn fn(…) -> R + Send` (and with `mut`) are `dyn` types (§37, §36.5) whose boxes are `Send`. A
  coercion to one needs `T: Send`, which for a move literal means `Send` captures (N0334). A `dyn … + Send` converts to
  the same `dyn` type without `+ Send` (the same value: they share their C representation and tables), never the
  other way. `Thread.spawn(f)` with `f: Box<dyn mut fn() -> T + Send>` runs a closure made elsewhere.

### 38.5 Profiles
- Threads, `Arc` and `Mutex` are in the **hosted profile**: hosts with pthreads or Win32 threads, and CosmoOS
  userland over `cosmo_thread_*` and `cosmo_mutex_*` (`profiles.md`). In a freestanding build their names are
  N0293. The kernel uses lib/kernel's locks (ADR 0025). The atomics and their orderings are in `lib/core`, in every
  profile (ADR 0026).
- `lib/std/` is the hosted library: compiled into hosted packages only. Like `lib/core` (§29.2), its names cannot be
  redeclared, and only what a program uses is emitted.

### 38.6 Fork-join: `Thread.both` (OD-25; ADR 0018)
- **`Thread.both(a, b)`** runs `a` on a new thread and `b` on the calling thread, waits until both have finished, and
  returns `void`. It is in the hosted profile, with `Thread` (§38.5).
  - Each argument is a closure literal with no parameters and a `void` result (§36.2), or a function value of type
    `fn()` (§36.1). A closure that is not a literal (a variable or a parameter of type `borrow fn()`) is refused
    (**N0334**): its captures cannot be seen, and the rule below is about them.
  - Results travel through captures: `Thread.both(() => { low = sum(halves.left); }, () => { high = sum(halves.right); });`
- **Captures.** `a` runs on another thread while `b` runs on this one, so each capture of either closure must be safe
  to use from another thread at the same time:
  - a capture used **shared** needs a **`Share`** type, where, for this rule only, `borrow T`, `Slice<T>` and
    `StringView` are `Share` when `T` is;
  - a capture used **mutably** needs a **`Send`** type, where, for this rule only, `borrow mut T` and `SliceMut<T>`
    are `Send` when `T` is;
  - a captured closure is refused;
  - otherwise **N0334**, naming the capture and its type.
  These extended rules apply here only: outside `Thread.both` references are neither `Send` nor `Share` (§38.1).
- **Loans.** Both closures are arguments of one call, so both, and every loan they carry, are live for the whole call
  (§36.2): the borrow checker refuses captures of one value by both where either changes it (B-rules, N0241), and
  nothing either captures can be changed, moved or ended until `both` returns.
- **No failure.** If the new thread cannot be started (its runtime record cannot be allocated, or the platform
  refuses), `both` runs `a` and then `b` on the calling thread. The result is the same either way, so `both` returns
  `void` and a program cannot tell whether `a` ran in parallel.
- A panic in either closure aborts the process (OD-5).
- **Effects** (§32): `sleep` and `alloc`.

Diagnostics: N0334 (a value that is not `Send` where one must cross to another thread; for `Thread.both`, a capture
that may not be used from two threads at once, or an argument that is not a literal or a function value).

## 39. Static items (M14 K1; ADR 0024)

```ceesafe
static OPENED: AtomicU64 = AtomicU64.constant(0);
static NAME: StringView = "ceesafe";

fn openOne() -> u64 => OPENED.fetchAdd(1) + 1;
fn opened() -> borrow(static) AtomicU64 => borrow OPENED;

static mut BOOT_FLAGS: u32 = 0;

/// Safety: called once on the boot CPU, before anything else reads BOOT_FLAGS.
unsafe fn setBootFlags(flags: u32) { BOOT_FLAGS = flags; }
```

### 39.1 The item
- `static NAME: T = e;` and `static mut NAME: T = e;` at package level, with an optional visibility, like `const`
  (§10). The type is required. A static is never generic, and a function cannot declare one (N0337).
- A static is one value for the life of the program, at a fixed address. It is never dropped: a `T` with a `drop` is
  allowed, and its drop never runs.

### 39.2 `static`
- `T` must be `Share` (§38.1), because every thread may use a static (N0337). Views of static string data are the
  one exception, since they cannot change and outlive everything: `T` may be `StringView`, and in a `static` (not a
  `static mut`) also `[StringView; N]`, whose initialiser is an array of string literals or constant views (ADR 0066).
  Such a table is a place, so unlike a constant table (§10) it is sliced and borrowed where it is named:
  `static LEVELS: [StringView; 3] = ["Low", "Medium", "High"];` then `f(LEVELS[..])`.
- `NAME` in an expression denotes the place `*S`, where `S` is a reference to the static of type `borrow(static) T`.
  `static` is the built-in region of §25.1, which outlives everything. So a static is used exactly as a value behind
  a shared reference is:
  - it can be read when `T` is `Copy`, borrowed (`borrow NAME`, `borrow NAME.field`), and its methods taking
    `borrow this` called;
  - it cannot be assigned or moved, and nothing reached through it can be changed (§13.2: N0211, N0248; a move out of
    it is N0251, as for any place behind a reference);
  - a reference to it, or to anything in it, has the `static` region. It may be returned from a function whose return
    type names `static` (`borrow(static) T`, §13.5), stored, and sent to another thread.
- Mutation goes through `T`'s own synchronisation: an atomic's operations, a lock's guard.

### 39.3 `static mut`
- `T` need not be `Share`.
- `NAME` denotes the place `*S` with `S: borrow(static) mut T`, so it can be read, assigned, borrowed mutably and moved
  out of as such a place can.
- **Every use is an unsafe operation** (§27.3), N0280 outside an unsafe context. The unsafe code's `/// Safety:`
  comment must argue what the checker cannot. Two uses give two references that may alias, and two threads may race
  on it (`unsafe.md` U3).
- **What may be written** (ADR 0067). `S` refers to memory that outlives every borrow, so a value written to it, or
  into it, may carry no loan: N0272 otherwise. Static data carries none: a string literal, a constant view, or a
  view read from another static, and a call's result whose type names only `static` (§25.6, ADR 0068). A view of a
  local or of a parameter is rejected. The unsafe block makes the use unsafe; it does not lift this rule.
- **C objects** (ADR 0042). `T` may be an imported C type (§31), or hold one: a device, a protocol's ops table. C
  code reaches it through the addresses CeeSafe gives it and writes it, which is why it is a `static mut`: a plain
  `static` of an imported type is N0337, since CeeSafe would read it as unchanging. Its initialiser is C's (§39.4).

### 39.4 Static initialisers
`e` is evaluated at compile time to a C constant initialiser. It must be a **static initialiser** (N0338 otherwise):
- a constant expression of an integer, `bool` or float type (§10), or a constant's name;
- `"…".ptr as *T`, a string literal's address, for a C string (`"name\0".ptr as *CChar`; §28);
- a string literal, as a `StringView`;
- a struct literal (§12) whose fields are static initialisers. Private fields may be given only where §12 allows it:
  inside the struct's own functions, so inside its `@constant` constructors (below);
- an array literal or repeat (`[e; N]`) of static initialisers;
- a call of a `@constant` function whose arguments are static initialisers;
- a call of a C constant macro whose arguments are static initialisers or `here` (below);
- `layoutOf<T>()` (§30.1).

- **For C objects** (ADR 0042), where the expected type is an imported C type, an `extern "C" fn` or a raw pointer:
  - a struct literal of an imported type (§31.2's one exception), naming any of its declared fields: the members it
    does not name are zero, as C's designated initialisers leave them (C11 §6.7.9p21), whether CeeSafe declares them
    or not. A `[u8; N]` member (a C `char` array, ADR 0034) may be given a string literal of at most `N` bytes, which
    C zero-pads (longer: N0338);
  - the name of an `extern "C"`, `@export` or `@callback` function: its C address (§29.5);
  - `borrow S as *T`, `borrow mut S as *mut T`, and the same of a field of `S` or an element at a constant index, for
    a static `S` (any static, declared before or after): its address, an address constant (C11 §6.6p9). Taking a
    `static mut`'s address is not an unsafe use. `S`'s value, or a field's, is N0338;
  - `0 as extern "C" fn(…) -> R` and `0 as *T`: null.

Not in v1: enum values (including optionals), references other than the static string data above, closures, and
`dyn`. Each of these is N0338.

**`@constant` functions.** `@constant` on a function or method (generic or not) makes it callable in a static
initialiser. Its body must be a single expression (`=> e`, or `{ return e; }`) that is a static initialiser in which
its parameters may also appear (N0338). It is an ordinary function too, callable at run time with the same result. A
call in an initialiser substitutes the arguments for the parameters, and instantiates generic code as any call does.

**C constant macros.** `@constant` on a function in an `extern "C"` block (§28.2) declares a C macro that expands to a
constant initialiser of its result type: `@constant fn SPINLOCK_INIT(name: *CChar) -> spinlock_t;`. It may be called
only in static initialisers and `@constant` bodies (N0338 elsewhere). CeeSafe trusts the declaration, as it trusts an
extern's `@effects` (R-2).

**`here`** (ADR 0027). As an argument of a C constant macro, `here` is the address of the place the macro's result
initialises, with type `*mut T` for the macro's result type `T` (N0338 if the parameter's pointee is another type).
Through nested `@constant` constructors the place is the whole path: in
`static TABLE: Mutex<Table> = Mutex<Table>.constant(name, data)`, the constructor's `MUTEX_INIT_AT(here, name)` is
given the address of `TABLE`'s field `raw`. So a macro may build C state that points to itself, as CosmoOS's list
heads, wait queues and mutexes do. `here` is a name, not a keyword: a variable or item called `here` hides it, and
outside static construction it is an ordinary unknown name (N0200). Anywhere else in a static initialiser or a
`@constant` body, including as an argument of a `@constant` function, it is N0338. A function that uses it builds C
state, so it is compile-time only (below).

**C state built in place** (§33.3). A type with a `@placed init` may be in a static only through a `@constant`
constructor, whose initialiser must leave the value as `init` would: for CosmoOS's locks, the C macro does. `init`
does not run for a static.

**C state at compile time** (§33.1–§33.2). C state is otherwise only made in place, by `Box.create`, and never held as
a value. A static initialiser is the other way to make it, at the static's fixed address, so three places may hold it:
- a static's type, which may hold C state: a static never moves;
- a C constant macro's result type, an imported C type (not FFI-checked: it is C's initialiser, not a value crossing a
  call; §28.3);
- a `@constant` function's result type, and its struct literal, which may give a C-state field the result of a C
  constant macro.

A `@constant` function whose result holds C state is **compile-time only**: it is evaluated in static initialisers
and other `@constant` bodies, called anywhere else is N0338, and it is never compiled to C.

### 39.5 Lowering
- A static is a C object at file scope named `ceeS_<name>` (with the package's name for a dependency's, as in §2.1 of
  `c-backend.md`), `static` unless `--split` places it in `ceesafe_instances.c` and renames it (c-backend §2.4). It is not
  `const` in C, because its methods may write through the pointers they derive from it.
- Its initialiser is the evaluated value: a designated initialiser for a struct (`{ .f_count = 0 }`), braces for an
  array, the macro call for a C constant macro, `(const char *)"…"` for a string literal's address, and a string
  view literal for a `StringView`. `here` is `(&ceeS_<name>.<field>…)`, an address constant (C11 §6.6p9), and
  `layoutOf<T>()` of a C-laid-out `T` is `{ .f_size = sizeof(…), .f_align = _Alignof(…) }`.
- `S` is `&ceeS_<name>`.
- **C objects** (ADR 0042): an imported type's literal is a designated initialiser over its C member names
  (`{ .name = "lo", .mtu = 65535, .ops = ((struct netif_ops *)&ceeS_LO_OPS) }`); a function's address is its C symbol
  (the extern's name, the export's, or a callback's `ceeW_` function, declared before the statics); a static's address
  is `((T)&ceeS_<name>.<member>…)`. When an initialiser holds a static's address, every static is first declared by a
  tentative definition (C11 §6.9.2), so the order of the items does not matter.
- The interpreter evaluates every initialiser before `main` into its memory. A C constant macro has no value it can
  compute, so a program whose statics use one cannot be interpreted (a fault at start).

Diagnostics: N0337 (a static whose type is not `Share`, or one that is generic or local), N0338 (an initialiser or
`@constant` body that is not a static initialiser, or a C constant macro used elsewhere), N0280 (a use of a
`static mut` outside an unsafe context).

## 40. Async functions and structured concurrency (L1; ADR 0031)

```ceesafe
async fn fetchBoth(net: borrow Net, a: StringView, b: StringView) -> Result<Pair, NetError> {
    var left = Buffer.withCapacity(4096)?;
    var right = Buffer.withCapacity(4096)?;
    await Async.scope(NetError.OutOfMemory, (s) => {
        s.spawn(async () => { await net.get(a, mut left)?; return Ok(); });   // borrows `left` from this frame
        s.spawn(async () => { await net.get(b, mut right)?; return Ok(); });  // capturing `left` here too: N0241
    })?;                                                                         // both children have returned
    return Ok(Pair { left: left, right: right });
}

fn main() {
    let result = Async.run(fetchBoth(Net.system(), "a.example", "b.example"));
}
```
(`Net`, `Buffer` and `Pair` stand for library or user types.)

### 40.1 Async functions and tasks
- **`async fn f(…) -> T`** (grammar.md §2), at package level, as a method, or as an interface method. An `extern`
  function, a `drop` block and a `@constant` function cannot be async (N0290). `async` comes after `unsafe`.
- **Interface methods.** An impl's method is async exactly when the interface's is (N0267). Through a type parameter
  (`T: I`) each instance awaits the concrete method, whose frame is known. Through `dyn I` (§37.3, ADR 0036) the
  awaiter's frame has room for the largest frame of `I.m` among the types coerced to `dyn I` anywhere in the
  program: the program is compiled whole, so that set is known. `dyn I` and `dyn I + Send` have one such frame,
  with room for the types coerced to either, since `+ Send` changes no layout (bug B15). An await through the table that may reach its own
  frame is recursion through `await` (below). `Async.boxed` takes such a call too, with a frame of the receiver's own
  size.
- **A call of an async fn is a task**, not a value: it runs nothing until it is awaited (§40.2), run (§40.4) or spawned
  (§40.5). A task call anywhere else (a `let`, an argument of any other call, a field, a return) is **N0342**. There is
  no type of tasks in v1, so a task cannot be stored, returned or passed on.
- **Arguments** are evaluated at the call, in order, and moved or lent into the task's frame. Borrowed arguments stay
  borrowed until the task has returned (§40.6), by the rules for references (§13, `lifetimes.md`).
- **The body** is checked like a function body with result type `T`, with three differences: it may `await`
  (§40.2); it is a context that forbids `sleep` (§32.3), so a blocking call is N0295 with its chain of calls; and its
  effects (§32.1) are those of its operations, `suspend` among them if it awaits.
- **Recursion through `await`** (an async fn that awaits itself, directly, through other async fns, or through `dyn`)
  would make its frame contain itself. In the hosted profile each await on such a cycle is **stacked** (ADR 0056): its
  callee's frame comes from the task's **frame stack**, and the awaiter holds a pointer to it, so the program is
  written as sync recursion is. The stack grows in 16 KiB chunks (or a frame's size), which a returning call gives
  back to its executor's pool (up to 64) for the next tasks; if no chunk can be had, the program panics (`async frame
  stack: out of memory`), as a sync program whose call stack overflows stops. Awaits not on a cycle are embedded as
  before. `--explain-frames` names the stacked ones. **N0343** remains where there is no frame stack (the freestanding
  profile), for a cycle through a `race` or `all` child, and for an async instance calling itself with its closure
  (§32.5). A recursive *sync* call from an async fn is an ordinary call.
- **`await Async.boxed(alloc, f(…))`** awaits a task whose frame comes from `alloc` (an allocator type, as for
  `Box.createIn`, §30) and is given back when it returns; the awaiting frame holds only a pointer, so recursion through
  it is allowed. Its value is `Result<T, AllocError>`: `Err(AllocError.OutOfMemory)` if the frame cannot be had, and
  then the task does not run. N0343's sure fix is `Async.boxed(SystemHeap {}, f(…))!`; in hosted code it keeps the
  failure as a value where a stacked await would panic.
- A generic async fn is checked once and instantiated like any generic function (§20.3).

### 40.2 `await`
- **`await e`**: `e` must be a task (a call of an async fn or an async closure), or one of the `Async` operations that
  produce one (§40.4); otherwise **N0342**. Its value is the task's result, of the task's result type.
- `await` is allowed only in the body of an async fn or async closure: anywhere else, including a sync closure inside an
  async fn, a `drop` block and a deferred statement, it is **N0341**.
- **What it means.** The current task runs `e` to completion; while `e` waits, other tasks of the same thread may run.
  Every `await` is therefore a point where other tasks run and where cancellation may be observed (§40.7). Nothing else
  suspends a task.
- **Borrows across `await`.** A frame never moves (§40.6), so a reference to a local may be held across an `await` with
  no extra rule. The borrow checker treats `await e` as a call: what `e` borrows is borrowed until it returns.
- **Effects:** `suspend` (§32.1), so an `await` while a `@context(atomic)` value may be live is N0295.

### 40.3 Async closures
- **`async (x, …) => body`** (grammar.md §2.1) is a closure literal (§36.2) whose call is a task. Its captures, their
  modes and their loans are §36.2's. `return` and `?` in the body return from the closure's task.
- It is accepted only as the argument of `spawn`, `race`, `all` and `shield` (§40.4, §40.5), or of an `@effectsOf`
  parameter (§32.5, ADR 0037), the way §38.6 accepts only literals: its captures must be visible where it is used
  (N0342 elsewhere). A named async fn becomes such a closure only for an `@effectsOf` parameter. It is never a
  function or closure value otherwise (N0298).
- Its body is an async body: it may await and forbids `sleep`.

### 40.4 The `Async` operations
`Async` is a built-in type, like `Thread` (§38), in the hosted profile until L1 A4 (§38.5). Every operation below
except `run` and `blocking`'s closure produces a task, so it is written after `await`.

| Operation | Result | Notes |
|---|---|---|
| `Async.run(f(…))` | `T`, the task's result | Runs the task on this thread until it returns. Sync code only: it has `sleep` and `alloc`, so in async code it is N0295. The operand is a call of an async fn: `Async.scope`, `all`, `race`, `timeout` and the others are awaited inside one, so as the operand they are N0342 (0.15.2) |
| `Async.runWorkers(n, f(…))` | `T` | `Async.run` with `n` workers, this thread among them (`n: u32`; ADR 0038): `n − 1` more threads, started for the run and joined when it returns, may take its `spawnSend` children (§40.5). A worker that cannot start is skipped: the result is the same. Hosted only |
| `Async.scope(oom, body)`, `Async.scopeIn(alloc, oom, body)` | `Result<void, E>` | §40.5 |
| `Async.race(a, b)` | `Result<Raced<T>, E>` | Two async closures of result `Result<T, E>`, run at once. The first to return decides: an `Ok` wins and the other is cancelled and awaited; `Raced<T>` (lib/std) holds `winner: u32` (0 or 1), `value: T`, and `other: T?`, the loser's value if it also returned `Ok`. An `Err` first is the result. The children's type comes from an expected `Result<Raced<T>, E>`, else from `a`'s expression body |
| `Async.all(a, b)` | `Result<void, E>` | Two async closures of result `Result<void, E>`, run at once; the first `Err` cancels the other and is the result. Results travel through captures, as in §38.6. Without an expected type or an expression body for `a`, `E` is `AsyncError`. The two may not capture one local where either changes it (N0241) |
| `Async.shield(ms, body)` | the body's result | Runs an async closure with cancellation ignored (§40.7) for at most `ms: u64` milliseconds. The body's result type is the expected one; without one, an expression body's own, and a block body's `Result<void, AsyncError>` |
| `Async.timeout(ms, timedOut, body)`, `Async.deadline(at, timedOut, body)` | `Result<T, E>` | ADR 0046. Runs `body`, an async closure of result `Result<T, E>`, beside a deadline `ms: u64` milliseconds from the call (`timeout`) or at `at: u64` on `Timer.now()`'s clock (`deadline`). If the body returns first, its result is the result. Once the deadline passes first, the body is cancelled and awaited: a late `Ok` is kept, and any `Err` becomes `timedOut: E`, which is evaluated before the body starts and dropped if unused. A cancellation from outside gives the body's own result, never `timedOut`; a tie is decided by which task runs first, as in `race`. The body's type comes from an expected `Result<T, E>`, else from its expression body |
| `Async.checkpoint(cancelled)` | `Result<void, E>` | `Err(cancelled)` if the current task is cancelled and not shielded, else `Ok`: a cancellation point for long computations. A call of lib/std's `AsyncControl<E>.checkpoint` |
| `Async.isCancelled()` | `bool` | Whether the current task is cancelled and not shielded (`AsyncControl<E>.isCancelled`) |
| `Async.blocking(closure)` | `T` | Runs a sync closure literal with no parameters on a worker thread and waits for it without blocking other tasks. Its captures follow §38.6's rule and `T` must be `Send` (N0334). If no worker can be started, it runs on the calling thread: the result is the same. The body's effects are the worker's (the task has `suspend`), and the wait is not cancelled: a cancelled task still waits for its worker |
| `Async.park()`, `Async.waker()` | `void`, `u64` | Library-only primitives (**N0345** elsewhere): suspend the current task until its waker is woken; the current task's waker id |
| `AsyncSignal.create()`, `s.raise()`, `await s.wait()` | `AsyncSignal`, `void`, `Result<void, AsyncError>` | lib/core (ADR 0039): `raise` is `@effects()`, so another thread or an `@context(irq)` handler may call it; `wait` parks the task until a raise, which a raise before the wait satisfies. One task waits at a time; a cancelled wait returns `Err(Cancelled)` |

`race`, `all`, `shield`, `timeout` and `deadline` place their children's frames in the caller's frame, so they never
allocate. The library
(`lib/std/async.cee`, Decision 0008) builds everything that waits for the outside world on `park` and `waker`: timers,
console (`AsyncConsole`), files (`AsyncFiles`: each operation on a worker thread, through `Async.blocking`, since a
regular file is always ready), sockets (`AsyncTcpListener`, `AsyncTcpStream`, on non-blocking descriptors; and
`AsyncSockets`, the same calls on raw descriptors for code with its own wait loop; their addresses below), `AsyncMutex<T>` (a guard that may be
held across `await`, its waiters served first come, first served), and channels (`Channel<T: Send>`, ADR 0032: `create(capacity)`,
`clone`, `close`, blocking `send`/`recv` for threads, `await sendAsync`/`recvAsync` for tasks; `trySend` and
`tryRecv`, which never wait and have no effect, and `isClosed()`, ADR 0060: `tryRecv` is the next buffered value or
null, empty or closed, and a channel is drained when `isClosed()` and then `tryRecv()` is null). Its operations return `Result<T, AsyncError>`,
where `AsyncError` has `Cancelled`, `TimedOut` and `Io(IoError)`.

**Socket addresses** (lib/std's `net.cee`, ADR 0044):

- `SocketAddress` is an enum: `V4([u8; 4], u16)` and `V6([u8; 16], u16, u32)`, the address bytes in network order,
  the port, and an IPv6 scope id (0: none). Through the interface `SocketEndpoint` it has `port()`, `withPort(p)`,
  `isLoopback()`, `isUnspecified()` and `write(w: borrow mut TextWriter)`: `127.0.0.1:8080`, `[::1]:8080`,
  `[fe80::1%2]:8080`, IPv6 in RFC 5952's canonical form (lowercase, the first longest run of two or more zero groups
  as `::`, an IPv4-mapped address's last 32 bits dotted), the scope as its number.
- An enum has no functions of its own (§15.1), so the constructors are `SocketAddresses`': `parse(text)` (`a.b.c.d:port`,
  `[v6]:port`, `[v6%zone]:port`; the zone a number or an interface's name, which the runtime resolves where it can,
  not on Windows), `parseIp(text, port)` (without the port), each `SocketAddress?`; and `v4Any(port)`, `v6Any(port)`,
  `v4Loopback(port)`, `v6Loopback(port)`. IPv4 parts are 1 to 3 digits without leading zeros, at most 255; IPv6 text
  is RFC 4291 §2.2's.
- `ListenOptions` has `backlog: u32` (0: `SOMAXCONN`), `reuseAddress` (`SO_REUSEADDR`; ignored on Windows, whose
  option lets another socket take the port), `reusePort` (`SO_REUSEPORT`; `Unsupported` where the platform has none,
  Windows), and `v6Only` (`IPV6_V6ONLY`, always set explicitly on an IPv6 socket); `ListenOptions.defaults()` is
  `{ backlog: 0, reuseAddress: true, reusePort: false, v6Only: false }`.
- `AsyncTcpListener.bindWith(address, options)` and `listener.localAddress()`, both `Result<_, IoError>`;
  `bind(text, port)` takes IPv4 or IPv6 text and the defaults. `await AsyncTcpStream.connectTo(address)`; on a stream,
  `peerAddress()`, `localAddress()`, `shutdownRead()` and the setters of §26.4, none async. A dual-stack listener's
  IPv4 peer is `V4`, not `::ffff:a.b.c.d`.

**Name resolution** (lib/std, ADR 0048): `await AsyncDns.resolve(host, port)` is `Result<Vector<SocketAddress>,
AsyncError>`: the addresses of both families, each with `port`, in the resolver's order, duplicates removed, never
empty.
- A literal (`SocketAddresses.parseIp`) is itself at once.
- Otherwise `getaddrinfo` (`GetAddrInfoW` on Windows; `AF_UNSPEC`, `SOCK_STREAM`, no flags) runs on one of at most four
  resolver threads, the rest queued. The name is passed as given (an internationalised one in its ASCII form).
- A cancelled task returns `Err(Cancelled)` at once; its lookup is unqueued, or left to its thread, which frees it.
- Errors are `Io` kinds: `HostNotFound` (no such name, or no address; ADR 0060, before which it was `NotFound`),
  `TimedOut` (a temporary failure), `InvalidInput` (empty, over 253 bytes, a NUL), `OutOfMemory`, `Unsupported`
  (CosmoOS userland; the interpreter, which knows only literals, `localhost`, as 127.0.0.1 then ::1, and names under
  the reserved `.invalid`, RFC 6761 §6.4, which are `HostNotFound`).

**Vectored sends** (lib/std, ADR 0045):

- `IoParts<region r>` (`net.cee`) is a list of up to eight byte parts, a value built by copying, since a slice is not an
  array element (§25.4): `IoParts.of(a)`, then `parts.then(b)`, which is `IoParts<r>?`, null when eight are there
  already. `count()`, `part(i)` (empty past the last), `totalLength()`, and `after(n)`: the bytes after the first `n`,
  as parts, whole parts dropped and the one where byte `n` falls cut; no parts from `totalLength()` on. Empty parts are
  allowed and send nothing.
- `await stream.writeVectored(borrow parts)` is one step: the task waits until the socket takes at least one byte, and
  the result, `Result<usize, AsyncError>`, is how many it took, which may stop inside any part. Empty parts return
  `Ok(0)` without a system call. `await stream.writeAllVectored(borrow parts)` sends every byte, as `write` does for
  one slice. `await AsyncSockets.sendVectored(fd, borrow parts)` is the one step on a raw descriptor. Errors and
  cancellation are `write`'s (§40.7): a send the kernel completed before the cancellation reports its count.

**Sending files** (lib/std, ADR 0051):

- `await stream.sendFile(borrow file, offset, length)` (`file: borrow File`, `offset` and `length` `u64`) is one step:
  the task waits until the socket takes at least one byte of the file from `offset`, and the result,
  `Result<u64, AsyncError>`, is how many went; 0 if the file ends at `offset`. A `length` of 0 is `Ok(0)` without a
  system call. The file's own position is neither used nor moved, so one file may be sent on several streams at once.
- `await stream.sendFileAll(borrow file, offset, length)` sends `length` bytes in as many steps as the socket needs; if
  the file ends first, the result is `Err(Io(InvalidInput))`, with the bytes before its end sent.
- Where the system has no file send for the socket and the file (the runtime's `CEE_ASYNC_NO_SENDFILE`), a step reads
  at most 64 KiB at the offset on a worker thread (`Async.blocking`) and writes them: the same results, through a
  copy. Errors and cancellation are `write`'s (§40.7).

**UDP** (lib/std, ADR 0050):

- `AsyncUdpSocket.bind(address, options)` (`UdpOptions`: `reuseAddress`, `reusePort`, `v6Only`, `sendBuffer`,
  `recvBuffer`, `receiveEcn`, `receiveLocal`; `UdpOptions.defaults()` sets none) is `Result<AsyncUdpSocket, IoError>`,
  not async; port 0 lets the system choose. `localAddress()` is the bound address. The socket owns its descriptor and
  closes it when destroyed.
- `await s.sendTo(bytes, to)` sends one datagram, whole or not at all: `Result<void, AsyncError>`. A datagram too long
  for the system is `Io(InvalidInput)`. An empty slice sends an empty datagram.
- `await s.recvFrom(buffer)` waits for one datagram: `Datagram { length, from, truncated }`. A datagram longer than the
  buffer fills it, and `truncated` says the rest is gone.
- `await s.sendMsg(borrow message)` sends a `UdpMessage { parts, to, from, ecn, segmentSize }`: the parts (an
  `IoParts`, §40.4) as one datagram, from the local address `from` if given (packet info), with the ECN bits `ecn`
  (0: not set). With `segmentSize > 0` the bytes are sent as datagrams of that size, the last shorter: in one call
  where the system can (Linux's `UDP_SEGMENT`, up to 64 segments), else one by one. The result is the bytes sent, whole
  segments only: if a segment fails after others went, the bytes before it; if the first fails, its error.
- `await s.recvMsg(buffer)` is `UdpReceived { length, truncated, from, to, ecn }`: `to` is the local address the
  datagram arrived at, with the socket's port (with `receiveLocal`; else null), `ecn` its ECN bits (with `receiveEcn`;
  else 0).
- An IPv4 peer of a dual-stack IPv6 socket is reported as `V4`, and a `V4` destination is sent IPv4-mapped. A port
  unreachable is not reported on a later receive (on Windows `SIO_UDP_CONNRESET` turns its report off). Windows refuses
  a sender's ECN value 3 (CE) as `Io(InvalidInput)`. CosmoOS userland has no UDP yet: `bind` is `Io(Unsupported)`.
- Cancellation is `read`'s and `write`'s (§40.7). `AsyncSockets.udpBind`, `udpSend` and `udpRecv` are the raw calls
  on descriptors, one try each, for code that runs its own wait loop.

### 40.5 Scopes
- **`await Async.scopeIn(alloc, oom, (s) => { … })`**, and `Async.scope(oom, body)` with the system heap (and the
  `alloc` effect). With `scopeIn`, each child's task and frame come from `alloc` and go back to it when the child
returns; the spawn has the effects of its `allocate` and `deallocate`. `body` is a sync closure literal taking the scope `s` and returning `void`. `oom: E` is the error the
  scope reports if a child's frame cannot be allocated (ADR 0031 §3.3, as amended).
- **`s.spawn(async () => …)`** starts a child whose result type must be `Result<void, E>`, the same `E` for every child
  of the scope. It returns `void`. The child's frame comes from the scope's allocator. If it cannot be allocated, the
  child is not started, the scope's other children are cancelled, and the scope's result is `Err(oom)`.
- **`s.spawnSend(async () => …)`** (ADR 0038) is `spawn` for a child that any worker of `Async.runWorkers` may run.
  It may move only before it starts: once started it runs, and everything it awaits or spawns starts, on that worker,
  so frames never cross threads. What it is given does: its captures follow `Thread.both`'s rule (§38.6), so one it
  reads needs `Share` and one it changes `Send`, references counting by their target, and a captured closure is
  refused. Its error type must be `Send`, and its frame comes from the system heap (not `Async.scopeIn`'s allocator).
  Each of these is **N0334**. A child's error cancels its siblings wherever they run. Under `Async.run`, which has
  one worker, `spawnSend` is `spawn`.
- **The scope returns only after every child has returned.** Its result is `Ok()` if every child returned `Ok`;
  otherwise the first `Err` (in the order the children returned), after the first `Err` has cancelled the others.
- **The body is a block** of the awaiting function, not a closure (ADR 0031 §6.6): `s` is not a local, and names the
  scope only in `s.spawn(…)`. `return`, `?`, `break` and `continue` cannot leave the body (**N0214**).
- **The scope-loan rule**, on the children's captures (§36.2), local by local:
  - every child's captures are borrowed from its `spawn` until the scope returns, and the awaiting function is
    suspended from the body's end until then;
  - two children capturing one local where either changes it are **N0241**;
  - a statement of the body that uses a local a child has captured is **N0241**;
  - a child that captures one of the body's own locals, which end when the body does while the child may still run,
    is **N0344**: a child borrows from the function that awaits the scope, never from the body.
  - **Copies** (ADR 0033): a child holds its own copy of each local it reads and does not change whose type is
    `Copy`, whoever owns it, taken when it is spawned. A body local may be captured so, which is how a loop spawns a
    child per iteration (`for (k in 0..v.length) { let r = borrow v[k]; s.spawn(async () => await f(r)); }`). A
    child's frame holds its own copy of its closure's environment, made when it starts.
  - **What a child holds lasts until the scope returns** (§3.2): every loan a child's environment carries, from a
    copied borrow or a borrow moved in, is live from its spawn until the scope's wait, in every iteration of a loop
    that spawns. So the body cannot change what a child borrows (N0240, N0241), and a child cannot borrow a local of
    one iteration of the body (N0243 when it ends, N0240 when the next iteration makes it again). The borrow checker
    holds each spawn's environment until its scope's wait, and `Builtin(AsyncScopeStart, [site])` at the body's start
    ends what the scope's previous run held (intermediate-representation.md).
  - **`async move () => …`** (§3.3) moves every capture into the child's frame, as a `move` literal moves into its
    environment (§36.5): the body cannot use what it moved (N0250), and the child drops what it owns when it returns
    (a cancelled child returns early). A child that cannot be started (no frame) drops it at once. A borrow may be
    moved in, and is held until the scope returns as a copied one is; it makes the environment scoped (§25). Only
    `s.spawn` takes `async move`: a race or all child is the caller's (N0209).
- **No `Send` or `Share` check** applies to children: they run on the scope's own thread (ADR 0031 §3.5).
- There is no detached task: every task a program starts has returned before `Async.run` returns.

**Task sets** (ADR 0053): children started while the set runs, from any function the set is passed to.

- **`await Async.tasks(limit, oom, async (set) => …)`** is `Result<void, E>`, `E` being `oom`'s type, which must be
  `Copy` (N0268). It is a call of lib/core's `AsyncTasks<E>.run` (ADR 0037): the body runs in the awaiting task, as
  the set's owner, with `set: borrow TaskSet<E>`, and may pass the set to the functions it calls. When the body has
  returned, the set waits for every child. Its children's tasks and frames come from the system heap, so it is hosted
  only (N0293). **`await Async.tasksIn(alloc, limit, oom, body)`** takes them from `alloc`'s allocator type instead
  (an allocator, N0263), as `Async.scopeIn` does, in every profile: lib/core's `AsyncTasksIn<E, A>.run` gives the set
  its allocator when it starts, so a spawn in any function uses it.
- **`await set.spawn(async move () => …)`** starts a child of result `Result<void, E>` and is `Result<void, E>`: while
  `limit` children run, the task waits for one to return. **`await set.spawnSend(child)`** is the same, but the child
  may run on another worker of `Async.runWorkers` (ADR 0038): each capture and `E` must be `Send` (N0334); a set from
  `tasksIn` with a user allocator starts it on this worker, since its frame is freed where it returns. Once the set or its task is cancelled, or if the child's
  frame cannot be allocated, it starts nothing and is `Err(oom)`; the set goes on. **`set.trySpawn(child)`** is `bool`,
  false instead of waiting (or once cancelled, or with no frame); it is not awaited. `set` is a local or parameter of
  type `TaskSet<E>` or a reference to one. Not awaiting `spawn` is N0342; a child that is not an `async move` literal
  with no parameters is N0209.
- **A child owns what it captures** (N0349): each capture is moved or copied, and its type holds no loan (no reference,
  slice, `StringView`, or struct with region parameters), since a child may outlive the code that started it. So a
  child cannot capture the set: shared state goes through `Arc` or a `Channel`, or values are passed down.
- **Errors.** A child's `Err` cancels no sibling; the set keeps the first. The body's `Err` cancels every child. The
  set's result, once the body and every child have returned, is the body's `Err`, else the first child's, else `oom`
  if a child's frame could not be allocated, else `Ok`. A cancellation of the set's owner reaches every child (§40.7),
  since the owner is the task that awaits the set.

### 40.6 Frames
- Each async fn or async closure has a **frame**: its parameters, the locals it needs across a suspension point (or
  borrows), storage for what it waits on (the awaited task's frame, shared by every await site), its result and its
  resume state. Its size is known at compile time, and `ceesafec build --explain-frames` reports it (c-backend.md §3a).
- **A frame is placed once and never moves.** The root frame of `Async.run` is in its caller's storage. An awaited
  task's frame is in its awaiter's frame. A child's frame is from its scope's allocator, and `race`, `all` and `shield`
  embed their children's. So every reference into a frame is valid until its task returns.
- **A frame is destroyed only after its task returns.** Cancellation never destroys a frame (§40.7). Its locals are
  dropped as the task returns, in the order of §14.
- Lowering: `intermediate-representation.md` (`StartTask`, `Await`, `Park`) and `c-backend.md` (frame structs and
  resume functions).

### 40.7 Cancellation
- **Cancelling a task sets its flag.** A scope cancels its children when one fails or one cannot start (§40.5), `race`
  and `all` cancel the other child, `timeout` and `deadline` cancel their body at the deadline (ADR 0046), and the
  runtime may cancel a root task (the `CEESAFE_CANCEL_AT` sweep,
  `testing.md`). The flag is inherited: an awaited task shares its awaiter's, and a scope's children are cancelled with
  the task that owns the scope. A cancellation wakes the tasks it reaches that wait on a deadline, a descriptor or a
  cancellable wake, so they see it; a task it does not reach is left parked (bug B5: every waiting task used to be
  woken, to retry and park again, at every cancellation).
- **Only the library's waiting operations observe it,** together with `Async.checkpoint` and `Async.isCancelled`: a
  cancelled task's next such operation returns `Err(AsyncError.Cancelled)` at once, or, for an operation already
  waiting, as soon as it can do so safely (a completion-based read first cancels and waits for the kernel's operation).
  Code between them runs normally, so cancellation is visible only at an `await`.
- Cleanup is ordinary code: `?` returns the error, and the frame's locals are dropped as the task returns.
- **`Async.shield(ms, body)`:** within `body`, the library's operations ignore the flag. Once `ms` milliseconds have
  passed since the shield began, they return `Err(AsyncError.TimedOut)`, never `Cancelled`. After the shield the flag
  is observed again.

### 40.8 The interpreter and determinism
- The interpreter runs tasks on one thread with a virtual clock (timers fire in deadline order without waiting; the wall
  clock, §26.11, is 2000-01-01 plus it) and a
  run order fixed by a seed. A program whose output does not depend on timing prints the same in C and in the
  interpreter. It has one worker: `Async.runWorkers(n, …)` is `Async.run`, and `spawnSend` is `spawn` (ADR 0038). A
  program's output must not depend on which worker ran what.
- **Profiles** (profiles.md §4, ADR 0039): in the freestanding profile the executor runs over its environment's hooks.
  Children need `Async.scopeIn`, and threads and I/O stay hosted.

Diagnostics: N0341 (`await` outside an async fn or async closure), N0342 (a task that is not awaited, run or spawned,
or an operand of `await` that is not a task), N0343 (recursion through `await`), N0344 (a spawned child that captures
the scope body's own locals), N0345 (`Async.park` or `Async.waker` outside the library); N0295 (blocking in async
code, `await` while a `@context(atomic)` value is live, `Async.run` in async code); N0241 (conflicting captures of a
scope's children); N0334 (`Async.blocking`'s captures and result, and `spawnSend`'s captures, error type and allocator).
