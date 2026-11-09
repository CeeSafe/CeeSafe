# Diagnostics

Status: M1 increment 1a. Lists every diagnostic code the compiler can emit. Codes are stable:
a retired code is never reused. Source of truth in code: `DiagnosticCodes`
(`src/diagnostics/catalog.cee`).

## Format

```text
<path>:<line>:<column>: error[N0100]: expected ';', found 'let'
  |
2 |     let x = 1
  |              ^
```

- `<path>` is the path as given on the command line (never made absolute), so output is reproducible. A package's
  files are named by their path from the package's directory, and a dependency's from the current directory
  (`../alpha/src/util.cee`; ADR 0082 §2.1); the JSON form also names the file's `package`.
- Line and column are 1-based. Columns count UTF-16 code units (grammar.md §1.1).
- Diagnostics are emitted in the order they are found. The lexer finishes before the parser starts,
  so all lexical errors of a file come before its syntax errors.
- `--diagnostics-format=json` gives the same diagnostics as one JSON document, with fixes, and `ceesafec explain
  <code>` prints a code's row (M12; `tooling.md` §5).

## Recovery policy

The parser reports **one** diagnostic per broken statement or item, then re-synchronises
(grammar.md §3). The parse-fail corpus enforces this: each test lists exactly the expected
diagnostics, so any cascade fails the test.

## Codes

### Lexer (N0001–N0099)

| Code | Meaning | Example |
|---|---|---|
| N0001 | Unexpected character (including non-ASCII outside literals and comments) | `let a = 1 # 2;` |
| N0002 | *(unassigned)* | |
| N0003 | Unterminated block comment | `/* ...` at EOF |
| N0004 | Integer literal does not fit in 64 bits | `18446744073709551616` |
| N0005 | Unterminated string literal (newline or EOF before `"`) | `"abc` |
| N0006 | Invalid escape sequence, or `\u{...}` that is not a Unicode scalar value | `"\q"`, `"\u{D800}"` |
| N0007 | Invalid char literal: empty, more than one character, or unterminated | `''`, `'ab'` |
| N0008 | Invalid digit for the radix, or no digits after a radix prefix | `0b102`, `0x` |
| N0009 | Unknown literal suffix | `12abc`, `1f32` (float suffix needs a float literal) |
| N0010 | Misplaced digit separator `_` (must sit between two digits) | `1__0`, `0x_FF`, `1_.5` |
| N0011 | A raw `"""` string literal laid out wrongly: text after the opening `"""` on its line, or a content line that is not blank and is indented less than the closing `"""` (`grammar.md` §1.6, ADR 0074 §2.6) | `""" x`, a line left of the closing `"""` |

### Parser (N0100–N0199)

| Code | Meaning | Example |
|---|---|---|
| N0100 | Expected a specific token | missing `;`, `(` after `if`, `}` before `fn` |
| N0101 | Expected an item (`fn` or `const`) at the top level | `let x = 1;`, `struct S {}` (not in M1) |
| N0102 | Expected an expression | `let a = ;` |
| N0103 | Comparison operators chained | `a < b < c` |
| N0104 | Invalid assignment target | `1 = 2;`, `f() = 3;` |
| N0105 | Expected a type, or `Self` where it names no type (outside an interface, an `impl` or a struct's or enum's body, ADR 0086 §2.2) | `fn f(x: ) {}` |
| N0106 | Namespace declaration not first in the file | `fn a() {}` then `namespace X;` |
| N0107 | Expected a field or `fn` inside a struct body; an imported union with no member, or a union inside an anonymous union member (ADR 0041 §3.2) | `struct A { 42; }` |
| N0108 | Expected a `match` pattern (`_`, a literal or range, a name, `name @ P`, `(P)`, or `E.V(…)`) | `match (n) { + => { } }` |
| N0109 | An `import` declaration after an item | `fn a() {}` then `import geometry;` |
| N0110 | Nested more than 1,000 levels deep (grammar.md §2.2: blocks, statements, types, patterns, unary operands, and each link of an operator, call or member chain). The rest of the file is not parsed | 1,000 nested parentheses; a sum of 1,001 terms |

### Names, types and flow (N0200–N0299)

**Suggestions** (ADR 0065). A member that does not exist (N0200, N0230, N0234 on a number) names
up to three it might be: the type's members within a few edits of it, then those whose doc comment mentions it
(`TextWriter.unsigned` gives `signed`, `decimal` or `hex`); for a built-in type (`Process`, `Console`, …) the members
the built-in types page lists for it, the longest shared start first; and for a small type with no close member, all
its members. A reserved word where a name goes (N0100) says so.

Semantic checks run only when every file parsed without errors.

| Code | Meaning | Example |
|---|---|---|
| N0200 | Name not found (variable, function, or built-in member) | `missing`, `Console.print(1)` |
| N0201 | Item defined twice in the package (a library item's name is not a clash since ADR 0075 §5: the package's item shadows it), or a C name declared twice by C functions or exports, even in different packages (`type-system.md` §28.2, ADR 0081 §2.2) | two `fn twice()`; `fn strlen` in two `extern "C"` blocks |
| N0202 | Parameter declared twice | `fn f(a: i32, a: i32)` |
| N0203 | Local redeclares a local or parameter in scope (no shadowing of locals) | `let x = 1; let x = 2;` |
| N0204 | Unknown type name | `fn f(a: int)` |
| N0205 | Type mismatch against the expected type | `let a: bool = 1;`, `if (n) {}` |
| N0206 | Integer literal out of range for its type | `256u8`, `let c: i8 = 128;` |
| N0207 | Operator not applicable to the operand types (includes mixed types, signed bitwise ops, negating unsigned values; `==` on an enum with payloads, or on slices or arrays of floats or structs, ADR 0072) | `a + b` with `u8`/`i32`, `s & s` with `i32` |
| N0208 | `as` conversion that can lose information | `a as u32` with `a: i32` |
| N0209 | Wrong number of arguments | `two(1)` |
| N0210 | Not callable, or a function or built-in used as a value | `local()`, `let f = two;` |
| N0211 | Assignment to an immutable binding (`let`, parameter, constant, function) | `param = 3;` |
| N0212 | Local read before it is definitely assigned | `var x: i32; return x;` |
| N0213 | End of a non-`void` function is reachable (for a `never` function too, type-system §15.3) | `fn f() -> i32 { }` |
| N0214 | `break`/`continue` outside a loop | `break;` at function level |
| N0215 | `return` with or without a value contradicts the return type, or `return` in a `never` function | `return 1;` in a `void` function |
| N0216 | Construct not supported in M1 (string values, member access, qualified types, generic arguments other than `truncate<T>`; floats and `char` until M11) | `truncate<i32, u8>(x)` |
| N0217 | Not a constant expression, or constant evaluation would panic | `const C: u8 = 200 + 100;` |
| N0218 | Constant depends on itself | `const A: i32 = A + 1;` |
| N0219 | Invalid `main` signature | `fn main(args: i32)` |
| N0220 | `let` without initializer | `let a: i32;` |
| N0221 | `var` without type annotation or initializer | `var c;` |
| N0222 | A generic's substituted copy does not check for its type arguments, and no other error explains why (type-system §20.4) | `fn copyOf<M: Metrics + Copy>(m: borrow M) -> M => m;` used with `dyn Metrics`, which no value holds (ADR 0064 §3.1) |
| N0223 | *(unassigned)* | |
| N0224 | Value of type `void` used as a value, or a `let` of a `never` expression | `let d = nothing();` |
| N0225 | Expression statement that is not a call | `1 + 2;` |
| N0226 | `void` used other than as a return type, or `never` other than as a function's return type | `var e: void;`, `fn f(x: never)` |
| N0227 | Struct contains itself by value (infinite size); reported at the field that closes the cycle. Also an interface that is its own supertrait (ADR 0086 §2.6) | `struct Node { next: Node; }` |
| N0228 | Duplicate member (field or function) in a struct, or an associated constant named like a field, method, variant or another constant of its type (ADR 0074 §2.7) | `struct D { a: i32; a: i64; }` |
| N0229 | Struct literal does not initialise every field | `Point { x: 1 }` |
| N0230 | Unknown field (in a literal or an access), or a function named where a field was expected | `p.z`, `p.sum` |
| N0231 | Field initialised twice in a literal | `Point { x: 1, x: 2, y: 3 }` |
| N0232 | `this` outside a method, or a `this` parameter outside a struct | `fn f() -> i32 => this;` |
| N0233 | Method called as a static function, or a static function called on a value | `Point.sum()`, `p.origin()` |
| N0234 | Field access, method call or literal on something that is not a struct (an optional has only `isSome` and `isNull`, ADR 0072) | `n.x` with `n: i32` |
| N0235 | A block branch of an `if` or `match` expression can complete, so it has no value (type-system.md §15.3) | `let a = if (c) { f(); } else 2;` |
| N0236 | A closure whose type cannot be inferred: a parameter without a type, or a block body returning a value, where no function or closure type is expected (type-system.md §36.2) | `let f = (x) => x + 1;` |

### References and borrows (N0240–N0249; `lifetimes.md`)

N0240–N0245 come from the NIR borrow checker. It runs only on programs that type-check, and its
errors carry notes ("borrowed here", "the borrow is used later here").

| Code | Meaning | Example |
|---|---|---|
| N0240 | Assignment to a place while it is borrowed (shared or mutable) | `let r = borrow x; x = 2; use(r)` |
| N0241 | Borrow conflicting with a live borrow | two live `borrow mut x`, or `borrow mut x` while `borrow x` is live |
| N0242 | Read of a place while it is mutably borrowed | `let m = borrow mut x; print(x); m = 1;` |
| N0243 | A place or temporary goes out of scope while borrowed | `let r = same(five()); use(r)` |
| N0244 | Returning a reference to a local or temporary (E1) | `return borrow x;` with `x` local |
| N0245 | Return-source errors (E2): missing or invalid `borrow(...)` list, a result derived from an unlisted parameter, or a deep result (one naming only regions of a parameter's referent, ADR 0074 §2.4) that borrows the referent's own storage | `fn f(a: borrow T, b: borrow T) -> borrow T`; `fn f(borrow this) -> Slice<r, u8> => this.buf[0..2]` |
| N0246 | Reference in a disallowed position: struct field, constant, `var` local, reference to reference, source list outside a return type. Since M4a this covers every scoped type, scoped results included (`type-system.md` §19.1) | `struct S { r: borrow i64; }` |
| N0247 | `borrow` of something that is not a place | `borrow 5`, `take(mut five())` |
| N0248 | `borrow mut` (or a `borrow mut this` call) on an immutable place | `borrow mut param`, `shared.grow()` |
| N0249 | `mut` argument marker missing or misplaced | `take(x)` for `borrow mut`, `look(mut x)` for `borrow` |

### Moves and destruction (N0250–N0259; `type-system.md` §14)

N0212 and N0250–N0253 come from the NIR move checker (since M2c), which also runs only on
programs that type-check. A move while borrowed is reported by the borrow checker as N0240.

| Code | Meaning | Example |
|---|---|---|
| N0250 | Use of a value that was moved (on at least one path), including reading a partially moved struct as a whole; a `name @ P` pattern that moves its value while a binding under `P` moves part of it (ADR 0085 §2.5) | `consume(r); consume(r);` |
| N0251 | Move out of a place reached through a reference; since ADR 0075 `*r` of a referent that is not Copy | `let a = r.a;` with `r: borrow Pair` and `Pair.a` a move type |
| N0252 | Move of a field out of a value whose type has a `drop` block | `consume(o.res)` with `struct Owner { res: Res; drop {} }` |
| N0253 | Move out of a `using` binding | `using u = make(); consume(u);` |
| N0254 | `return`, `break`, `continue` or `defer` inside a deferred statement | `defer return 1;` |
| N0256 | `match` that does not cover every value (add the missing variants or `_`); arms with a guard do not count (ADR 0085 §2.2) | `match (s) { Shape.Empty => { } }` |
| N0257 | Unreachable `match` arm: a duplicate pattern, or any arm after `_`; a guarded arm too, when earlier unguarded arms match all it does | `1 => { } 1 => { }` |
| N0258 | *Retired in M3c:* a non-Copy payload bound from a value scrutinee now moves out, and the match consumes the scrutinee (`type-system.md` §18.4) | — |
| N0259 | Invalid array (`type-system.md` §16): a length that is not an integer literal from 1 to 2^48 − 1, an element type that is not Copy, or an array larger than 2^31 − 1 bytes; since M9b also `copyFrom` or `fill` on a slice whose element type is not Copy (§16.5), and since ADR 0071 a vector's `extend` or `resize`; since ADR 0075 `[]` where no slice is expected (a static's array may hold non-Copy elements without a `drop` block) | `[Res; 2]`, `[i32; 0]`, `let e = [];` |

Arrays and slices otherwise reuse codes: indexing or slicing a non-array, a non-`usize` index,
or a bad `for` sequence or range is N0205; writing an element of a `let` array or through a
`Slice` is N0211; a slice in a field, a `var` local or an array element is N0246;
`borrow mut a[..]` of an immutable array is N0248; slicing a non-place is N0247. Since M9b, `copyFrom` or
`fill` on a `Slice` is N0211, on a slice of an immutable array N0248, and a source that is not a slice of the
element type N0205. Slices are
references, so the borrow checker reports their conflicts as N0240–N0245.

### Region parameters (N0270–N0279; `type-system.md` §25, M6x)

| Code | Meaning | Example |
|---|---|---|
| N0270 | A scoped field or payload in a struct or enum without region parameters | `struct S { v: StringView; }` |
| N0272 | Storing a value through a `borrow mut` parameter whose loans the signature does not let flow into it, or storing a value with any loan into a `static mut` (ADR 0067) | `fn put(v: borrow mut Vector<StringView>, s: StringView) { v.push(s)!; }` |
| N0273 | An unknown region name, region arguments on a type without region parameters, the wrong number of them, or `static` in a parameter's type (ADR 0068) | `StringView<nowhere>`, `Plain<r>` |
| N0274 | A generic used with a scoped type argument where it needs an unscoped one (it boxes `T`, makes an array of `T`, …) | `boxed<StringView>(s)` with `fn boxed<T>(v: T) -> Box<T>` |

N0271 is unassigned: destroying a value that holds loans while their referents are gone reports N0243.

### Unsafe and raw pointers (N0280–N0289; `type-system.md` §27, `unsafe.md`, M8a)

| Code | Meaning | Example |
|---|---|---|
| N0280 | An unsafe operation outside an unsafe context: a read, write or arithmetic through a raw pointer, `Ptr.toBorrow`/`toBorrowMut`, `Slice.fromRaw`/`SliceMut.fromRaw`, a call to an `unsafe fn`, or a use of a `static mut` (U1, type-system.md §39.3) | `let v = p.read();` in a safe function |
| N0281 | An invalid raw-pointer pointee: a reference, slice, `StringView` or other scoped type, or `never` (a type parameter is allowed since M8d; a scoped argument for it is N0274) | `*borrow i32`, `*Slice<u8>` |
| N0282 | A pointer operation that the pointer or its pointee does not allow: reading or writing a non-Copy type, writing through a `*T`, `toBorrowMut` or `SliceMut.fromRaw` from a `*T`, any access through `*void` | `p.write(1)` with `p: *i32`; `q.read()` with `q: *void` |
| N0283 | An `unsafe fn`, or a function containing `unsafe`, without a doc comment with a `Safety:` line (U2) | `unsafe fn f() {}` with no `/// Safety: …` above it |

### C interoperability and profiles (N0290–N0299; `type-system.md` §28, M8b; `profiles.md` §4, M8c)

| Code | Meaning | Example |
|---|---|---|
| N0290 | An unknown attribute, an attribute in the wrong place or with wrong arguments, a repeated attribute, an ABI other than `"C"`, or an extern declaration with a body, receiver or type parameters; `@export` on `main`; an effect name other than `sleep`, `alloc` and `suspend`; `async` on an extern, interface or `@constant` function (`type-system.md` §40.1) | `@inline fn f() {}`, `@repr(C) enum E { A }`, `extern "Rust" { … }` |
| N0291 | A type that cannot cross the C boundary in an extern declaration, an export, or a `@repr(C)` struct's field; a `drop` on a `@repr(C)` struct that holds C state (ADR 0029) | `fn f(s: Slice<u8>);` in `extern "C"`, `@export fn g() -> Box<i32>` |
| N0292 | A C name that is not a valid C identifier, is a C keyword (or `bool`, `true`, `false`, `NULL`, `main`), is reserved by C, or starts with `ceesafe_`, `cee_`, `CEE_` or `cee` and an uppercase letter | `@export("int")`, `fn ceesafe_alloc();` in `extern "C"`, `@repr(C) struct S { default: i32; }` |
| N0293 | A hosted-only name (`Console`, `Process`, `File`, the heap allocators and every collection or box that needs them, …) or a `main` function in a package built with `--profile freestanding` (`profiles.md` §4.1) | `Console.writeLine(1);`, `let b = Box.create(5)!;` |
| N0294 | An imported C type (type-system.md §31), or a C-laid-out type (§33), used where CeeSafe would need its size or would move it: as a local, parameter, result, field, element, optional or type argument, in a struct literal, or with `read`, `write`, `add`, `sub`, `offset` or `fromRaw` through a pointer; or a borrow of a pointer, `usize` or `isize` field of one | `fn f(i: cosmoboot_info) {}`, `p.add(1)` with `p: *cosmoboot_info` |
| N0295 | An operation whose effect (`sleep`, `alloc`, `suspend`) the context forbids: in an `@context(atomic)` or `@context(irq)` function, in an async fn or async closure (`sleep`; type-system.md §40.1), while an `@context(atomic)` value (a spin-lock guard) is live (`sleep`, `suspend`), or beyond a CeeSafe function's `@effects` bound (type-system.md §32); a closure body beyond its type's effect list, or a named function with effects its expected function type's list lacks (§36.3) | `mutex_lock(m)` while a `SpinGuard` is live |
| N0296 | A `private` field named outside the source file that declares its struct: read, assigned, borrowed or given in a literal (type-system.md §12) | `lock.data` outside `sync.cee` |
| N0297 | A float type, literal or `fromBits` in a build with `--no-float` (type-system.md §35.9) | `let x = 1.5;` under `ceesafec build --no-float` |
| N0298 | A function that cannot be a function value: an `extern` or `unsafe` function, one with region parameters, `main`, or an async fn other than as the argument of an `@effectsOf` parameter (type-system.md §36.1, §32.5) | `let f = puts;` with `puts` declared in an `extern "C"` block |
| N0299 | A closure of the wrong kind: a `borrow mut fn` where a `borrow fn` is expected, or a capturing closure where a plain `fn` is expected (type-system.md §36.2); a function or closure value whose type's effects are not in the expected type's effect list (§36.3) | `let f: fn(i32) -> i32 = (x) => x + k;` |

Calling an extern function outside an unsafe context is N0280; two extern declarations or exports
with one C name are N0201, even in different packages. It is reported at each declaration but the one that owns the
name (type-system.md §28.2 says which: never a dependant's when its dependency declares it too), with a note at the
owner's, which names its package; for a direct dependency's extern, the message says to call it through the package (``use `sys.strlen`, which package 'sys'
declares; remove this declaration``) or to make it `public` there, and an identical declaration has a fix that
removes it (type-system.md §28.2; ADR 0081 §2.2). (`CChar` as a value type was N0281 until ADR 0080 §3.5,
type-system.md §28.3.)

Raw pointers reuse codes: an invalid pointer cast is N0208, ordering comparisons on pointers are
N0207, a constant of
pointer type is N0216, and an `unsafe main` is N0219. (An `unsafe` interface method was N0216 until C5.)

### Results and the heap (N0260–N0269; `type-system.md` §17–§18)

| Code | Meaning | Example |
|---|---|---|
| N0260 | `Ok(...)` / `Err(...)` (or, since M4c, `Pool.create(...)`; since M6c, `null`) without an expected type that fixes its type arguments, or where another type is expected | `let a = Ok(1);`, `let p = Pool.create(SystemHeap {}, 4u32);`, `let n = null;` |
| N0261 | `?` in a function that does not return a `Result` with the same error type, or an enum with an `@from` variant for it (ADR 0075 §2.3; the message names the variant to add); `?` on an optional in a function that does not return an optional (M6c) | `?` on `Result<i32, F>` in a function returning `Result<i32, E>` |
| N0262 | `?` or `!` on something that is not a `Result` or optional value (a reference to one included), or `??` with a left operand that is not optional; an `@from` variant without exactly one payload, or two `@from` variants of one payload type (ADR 0075 §2.3) | `n?` with `n: i32`, `@from A` |
| N0263 | An allocator argument that is not an allocator type (`SystemHeap`, `FailingAllocator`, or for `Box` since C5 a struct without fields implementing `Allocator`) | `Box<i32, Point>`, `Box.createIn(point, 1)` |
| N0265 | Type arguments that cannot be inferred: a generic call, struct literal or enum value whose arguments, fields, expected type or unique bound instantiations (ADR 0086 §2.7) do not determine every type parameter. Not reported when an argument has an error of its own (ADR 0075 §2.16) | `let d = Maybe.Nothing;`, `let e = make();` |
| N0266 | Generic instantiation nested more than 64 deep (polymorphic recursion) | `fn deep<T>(x: Maybe<T>) { deep(Maybe.Just(x)); }` |
| N0267 | An `impl` that does not match its interface (receiver, parameter or return type, a missing method without a default, or an extra method), a duplicate `impl`, an `impl` for a type parameter or a scoped type other than `S<r>` under `impl<region r>`, a generic impl whose target is not a generic struct or enum or does not name each of its type parameters, overlapping impls, an impl outside the package of its interface and of its type (the orphan rule, ADR 0054), an impl method whose receiver or number of type parameters differs from the interface's (ADR 0086 §2.3), a missing or undeclared constant or associated type, a constant of another type (ADR 0086 §2.4, §2.5), or a method call, constant or associated type `T.Name` that two bounds both provide | `impl Shape for Square { fn area(this) -> i64 … }` |
| N0268 | A type argument that does not satisfy a bound of its type parameter (an interface it does not implement, or `Copy`), an impl's associated type that does not meet its declared bounds (ADR 0086 §2.5), an impl's target that does not implement a supertrait of its interface (ADR 0086 §2.6), or a type with region parameters coerced to `dyn I` (ADR 0054) | `total(plain)` with `fn total<S: Shape>(…)` |
| N0264 | A type a region cannot hold (R2): one that needs drop, a scoped type, or a non-Copy fill value for `allocArray` | `arena.alloc(Res { id: 1 })`, `arena.alloc(someReference)` |

Generic code (M6a) reuses codes:
- an operator on a type parameter is N0207, a field or method on one is N0234, and a literal for
  one is N0205;
- a generic type without its arguments, or with the wrong number of them, is N0204;
- a duplicate or shadowing type parameter is N0201;
- an interface method's type parameters are compared with its impl's by number and position (N0267, ADR 0086
  §2.3);
- an interface's method, or a method of an `impl` of an interface, with region parameters of its own is N0216 (ADR
  0074 §2.4);
- a generic `main` is N0219;
- naming something that is not an interface as a bound or in an `impl` is N0204;
- a method on a type parameter that no bound declares is N0234;
- an `impl` method clashing with a type's own method is N0228, and so is an inherited interface default whose name
  the target already has, reported at the impl (ADR 0086 §2.1);
- an interface default is checked at the interface, with `Self` opaque: a field of `Self` is N0234, and an inherited
  instance that does not check for its target is N0222 at the impl (ADR 0086 §2.1);
- `Self` outside an interface, an `impl` block or a struct's or enum's body is N0105 in a type and N0200 in an
  expression (ADR 0086 §2.2);
- a projection `T.Name` whose bounds declare no associated type `Name` is N0200, and a generic impl's own constant
  N0216 (ADR 0086 §2.4, §2.5).

Region values reuse codes:
- assigning a `using` binding as a whole is N0211;
- nesting a frame that is not a `using` binding is N0248;
- a method call on a frame that is not a local is N0247;
- a storage that is not `[u8; N]` or `SliceMut<u8>` is N0205;
- returning a frame or fixed-buffer allocator without sources is N0245.

`?` inside a deferred statement is N0254, and `Ok`/`Err` with the wrong number of values is
N0209. The names `Result`, `Ok`, `Err`, `AllocError`, `Box`, `SystemHeap` and `FailingAllocator`
cannot be declared (N0201). Moving out of a box's contents is N0251 until M3c.

### Build (N0300–N0399)

| Code | Meaning | Example |
|---|---|---|
| N0300 | `ceesafec build --emit exe` without a `main` function | a library-only file |

Names across packages (N0310–N0319; `type-system.md` §2, ADR 0013):

| Code | Meaning | Example |
|---|---|---|
| N0310 | The package named in an import, or in a struct literal `p.T { … }`, is not a dependency of the package (elsewhere an unknown `p` in `p.x` is N0200) | `import geometri;` |
| N0311 | A dependency has no item of that name | `geometry.Pointt` |
| N0312 | The item exists but is not `public`, or a field or method of a public struct is not | `geometry.helper()` with `fn helper` internal |
| N0313 | A simple name is provided by two imports | `import a; import b;` both with `Point`, then `Point` |
| N0314 | A public item's signature names a type that other packages cannot see | `public fn make() -> Hidden` with `struct Hidden` internal |
| N0315 | An item or a type parameter has the name of one of the package's dependencies (a local, parameter, loop variable or binding may, ADR 0074 §2.10) | `struct geometry { … }` |

Packages (N0320–N0329; `tooling.md` §4, ADR 0013):

| Code | Meaning | Example |
|---|---|---|
| N0320 | `ceesafe.toml` or `ceesafe.lock` is not valid in the TOML subset | `name = app` (no quotes) |
| N0321 | A manifest field is missing, unknown or invalid, or a dependency is an executable | `kind = "binary"` |
| N0322 | A path dependency's directory or manifest is missing, or its name differs from its key | `geometry = { path = "../geo" }` with no `../geo/ceesafe.toml` |
| N0323 | A git dependency cannot be fetched or resolved, or is absent with `--offline` | an unknown tag |
| N0324 | The lock file is missing or does not match the manifest | a dependency added without `ceesafec fetch` |
| N0325 | A git checkout's manifest and sources do not match the lock's hash | a file edited in the cache |
| N0326 | One package name reached through two different sources | two paths to two `geometry` packages |
| N0327 | A dependency cycle | `a` depends on `b`, `b` on `a` |
| N0328 | An invalid version, version requirement, or index entry (`tooling.md` §4.6) | `shapes = "1.x"` |
| N0329 | No versions satisfy the requirements: the conflicting requirements and where each came from (§4.7) | `^2.1 (from geometry 1.4.0) and ^1.0 (from plot 0.3.0)` |
| N0335 | `ceesafec publish`: the package cannot be published as it is (§4.8) | a path dependency, or uncommitted changes |

Dynamic dispatch and owning closures (N0330–N0339; `type-system.md` §37, §36.5, ADR 0015):

| Code | Meaning | Example |
|---|---|---|
| N0330 | `dyn I` where it is not behind a reference or a `Box`: a local, parameter, field, element, optional or type argument | `fn f(s: dyn Shape)`, `Vector<dyn Shape>` |
| N0331 | `dyn I` for an interface that is not dyn-compatible (type-system.md §37.1, ADR 0086 §2.3): one of its methods takes `this` by value, has no receiver, has type parameters of its own or names `Self` outside its receiver, or it has an associated type or a constant, or requires `Copy`; or the same holds for a supertrait | `interface Sink { fn close(this); }` then `borrow dyn Sink` |
| N0332 | A `move` literal captures a scoped value: a reference, view, slice, closure, or a value with region parameters (`type-system.md` §36.5) | `let v = s.view(); Box.create(move () => v.length)` |
| N0333 | A `move` literal anywhere but as the value of `Box.create`/`createIn` with an expected `Box<dyn fn…>` | `let f = move (x: u64) => x;` |
| N0334 | A value that is not `Send` where it must cross to another thread: a spawned closure's capture or result, a `dyn … + Send` coercion's source, a `Send` bound's argument, or, for `Thread.both`, a capture that may not be used from two threads at once or an argument that is not a closure literal or `fn()` value; for `s.spawnSend`, the same rule on its captures, an error type that is not `Send`, or a scope with a user allocator (`type-system.md` §38, §38.6, §40.5, ADR 0038) | `Thread.spawn(move () => view.length)` with `view: StringView` |
| N0336 | `@effectsOf` that names no parameter, or a parameter that is not of a function or closure type, or has no names (`type-system.md` §32.5) | `@effectsOf(n)` with `n: i32`; `@effectsOf()` |
| N0337 | A static that cannot be one: a `static` whose type is not `Share`, or a generic or local one (`type-system.md` §39) | `static C: Cell = …` with `Cell` not `Share` |
| N0338 | Not a static initialiser: an initialiser or `@constant` body outside §39.4, or a C constant macro called elsewhere | `static N: u64 = compute();` |
| N0339 | `offsetOf<T>(field)` whose `T` is not a struct, or has no field of that name (`type-system.md` §30.1, ADR 0030) | `offsetOf<u32>(x)`; `offsetOf<Pair>(c)` |
| N0340 | `callerAddress()` outside the body of an `@export` function, including in a closure inside one (`type-system.md` §28.4, ADR 0030) | `fn f() -> usize => callerAddress();` |
| N0341 | `await` outside the body of an async fn or async closure: in a sync function or closure, a `drop` block or a deferred statement (`type-system.md` §40.2, ADR 0031) | `fn f() { await g(); }` |
| N0342 | A task that is not awaited, run or spawned: a call of an async fn, an async closure, or a call with an async closure for an `@effectsOf` parameter, anywhere else; an `await` whose operand is not a task; an `Async.run` or `Async.runWorkers` operand that is not a call of an async fn (`Async.run(Async.scope(…))`); or an `@effectsOf` function that uses such a closure other than by calling it or passing it on to an `@effectsOf` parameter (`type-system.md` §40.1, §40.3, §32.5) | `let t = fetch(url);` |
| N0343 | Recursion through `await` where no frame stack takes it (ADR 0056): in the freestanding profile, an async fn whose frame would contain itself, directly or through a call through `dyn` that may reach it; in any profile, a cycle through a `race` or `all` child, or an `@effectsOf` function's async instance calling itself with its closure (`type-system.md` §40.1, §32.5, ADRs 0036, 0037) | `async fn walk(n: u32) { if (n > 0) { await walk(n - 1); } }` |
| N0344 | A scope's child that captures one of the scope body's own locals, which end before the child does, other than a `Copy` value it only reads, which it copies, or what an `async move` child moves (`type-system.md` §40.5, ADR 0033) | `Async.scope(E.Oom, (s) => { var n = 0; s.spawn(async () => { n += 1; return Ok(); }); })` |
| N0345 | `Async.park()` or `Async.waker()` outside the library (`type-system.md` §40.4) | `await Async.park();` in a program |
| N0346 | A member of an imported union whose type has bit patterns that are not values: anything but an integer, a raw pointer, an array of these, or an imported type whose fields are such (`type-system.md` §31.1, ADR 0041 §3.2) | `union u { on: bool; n: u32; }` in `extern "C"` |
| N0348 | A `@test` function of the wrong shape: a method, with parameters or arguments, generic, exported, or returning anything but `void` or `Result<void, E>`; or a call of a test from anything but the generated harness (`build` leaves tests out) (`tooling.md` §10, ADR 0049 §3.1) | `@test fn t(x: u32) {}` |
| N0349 | A task set's child that captures a value holding a loan (a reference, a slice, a `StringView`, a struct with region parameters, the set itself): it may outlive the code that started it (`type-system.md` §40.5, ADR 0053 §3.2) | `await set.spawn(async move () => { let k = n + 1; return Ok(); })` with `n: borrow u32` |
| N0347 | A further argument of a C variadic function whose type C's default argument promotions would change or that cannot pass through `...`: an integer narrower than 32 bits, `bool`, `f32`, or anything but `i32`, `u32`, `i64`, `u64`, `isize`, `usize`, `f64` and raw pointers (`type-system.md` §28.2, ADR 0041 §3.3) | `printf(f, b)` with `b: u8` |
| N0351 | A cell of a loan-holding type: `RawCell<T>`, `Cell<T>` or `RefCell<T>`, directly or inside a generic struct, with a `T` that holds loans (a reference, view, closure or other scoped type), whose contents could then outlive what they borrow (`type-system.md` §41.1, ADR 0084 §2.1) | `RefCell<StringView>`, `Cell<borrow u32>` |
| N0355 | A `match` guard that moves, assigns or `mut`-lends the scrutinee, a place inside it or one of the arm's bindings, or that awaits: the guard only reads them (`type-system.md` §15.2, ADR 0085 §2.2) | `Slot.Full(r) if (consume(r)) =>` with `r` not Copy |
| N0356 | The alternatives of an or-pattern bind different names, or one name with different types or `var`-ness (`type-system.md` §15.2, ADR 0085 §2.3) | `Some(x) \| null =>` |
| N0357 | An empty range pattern: `lo..=hi` with `lo > hi`, or `lo..hi` with `lo >= hi` (`type-system.md` §15.2, ADR 0085 §2.4) | `'z'..='a' =>` |
| N0358 | A constant pattern of a type that cannot be a pattern (anything but an integer or `bool`), or `T.NAME` read through a type parameter, or `Self.NAME` in an interface's default method (`type-system.md` §15.2, ADR 0085 §2.6) | `HALF =>` with `const HALF: f64` |
| N0360 | A `debugAssert` whose condition or message may change what exists before the call, which release builds would not do: an assignment, `mut` loan, move or drop of a place declared outside the arguments, a write through a raw pointer or to a `static mut`, I/O or another `sleep` or `suspend` operation, a call of a C function (even one declared `@effects()`) or through a closure, function value or `dyn` method, an early exit (`?`), or a call or drop that does one of these at any depth; the message names the chain of calls (`type-system.md` §8, ADR 0087 §2.5) | `debugAssert(events.next() != null)` with `events` declared before the call |
| N0363 | Two CeeSafe files of a `build --split` whose C units' paths, `c/<package>/<path in the package>.c`, differ only in letter case: on macOS's and Windows's file systems they would be one file (`c-backend.md` §2.4, ADR 0082 §2.1). Reported at the later file | `src/Util.cee` and `src/util.cee` in one package |

### Warnings (N0400–N0499)

A warning is reported like an error, `warning[N0400]` in the text format and `"severity": "warning"` in the JSON
document (counted in `"warnings"`), but it never stops `check`, `build`, `run`, `test` or `fix`, and never changes
the exit status. `check` reports it with the errors; `build`, `run` and `test` write it to standard error before their
work, or as the JSON document after it. Warnings are reported for the root package's files only, never for a
dependency's, lib/core's or lib/std's. `--deny-warnings` (`check`, `build`, `test`) reports every warning as an
error, and the command fails (ADR 0073 §2.4).

| Code | Meaning | Example |
|---|---|---|
| N0350 | A configured item used where its condition may not hold: a C function, an imported field or an export declared `@cfg("X")` used outside `if (cfg("X")) { … }` and outside an export configured so (type-system.md §28.4, ADR 0080 §3.3) | `fields.debugOnly` outside `if (cfg("CONFIG_DEBUG"))` |
| N0400 | A string literal's text has `{name}`, `name` bound where the literal stands (a local, a parameter, `this`, a constant or a static): CeeSafe has no string interpolation, so it is printed as written. `\{` means the brace and does not warn (`grammar.md` §1.6) | `let x = 3; Console.writeLine("x = {x}");` |
| N0401 | `name.x` where `name` is a local, parameter, loop variable or binding with the name of a dependency that has a public item `x`: the local hides the dependency there, so this is the local's member and not `name.x`, the item (`type-system.md` §2, ADR 0074 §2.10) | `fn f(tls: Conn) { tls.connect(); }` with a dependency `tls` that has `public fn connect` |
| N0402 | A `RefCell` guard (`CellRef`, `CellRefMut`, or a value holding one) live at an `await` or another point where an async fn or async closure may suspend: another task resumed meanwhile panics on the same cell (`type-system.md` §41.3, ADR 0084 §2.6) | `let g = cell.read(); await pause();` |
