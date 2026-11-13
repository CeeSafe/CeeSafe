---
title: "Appendix A: Keywords"
summary: Every reserved word of CeeSafe with its meaning and the section that teaches it, the words that are special only in one place and stay usable as names, and every attribute.
keywords: [keyword, reserved word, contextual word, identifier, attribute, is, region, dyn, union, own, type, move, drop, where, "@test", "@from", "@derive", N0100, N0290]
diagnostics: [N0100]
specification: ["grammar.md §1.3", "grammar.md §2", "type-system.md §28.1"]
---

# Appendix A: Keywords

This appendix lists three sets of words. The **reserved words** are the 46 words that the compiler never reads as a
name. The **contextual words** have a meaning in one place of the grammar and are ordinary names everywhere else.
The **attributes** are the names that may follow `@` before a declaration. Look a word up in the first column; the
last column names the section that teaches it.

The tables are the compiler's own lists for release %ceesafec-version%: the reserved words are those of `keywordText` in
`src/syntax/token.cee`, the contextual words are those the parser compares a name with, and the attributes are
those the checker accepts. The grammar is in
[the specification](https://ceesafe.github.io/CeeSafe/reference/grammar).

## Reserved words

A reserved word cannot name anything: not a local, a parameter, a field, a function, a type or a package. That
holds for every word of this table, also for the six that have no meaning yet. The names of the built-in types
(`i32`, `u8`, `bool`, `void`, `never`, `StringView`, `Vector`, …) are not here because they are not reserved: they
are names that the compiler declares, found like any other name.

| Word | What it means | Taught in |
|---|---|---|
| `as` | Converts a value to another type when no value can be lost: `x as i64`. A conversion that can lose is a call, `truncate<u8>(x)`. | [3.2](./ch03-02-data-types), [20.4](./ch20-04-arithmetic) |
| `async` | Before `fn`, declares a function whose call is a task. Before a closure, `async () => …`, makes a task to give to a scope. | [17.1](./ch17-01-tasks-and-await), [17.2](./ch17-02-scopes) |
| `await` | Runs a task to its result, inside async code: `await read(path)?`. | [17.1](./ch17-01-tasks-and-await) |
| `borrow` | Lends a value without moving it. In a type, `borrow T` reads and `borrow mut T` may change. As an expression, `borrow x` makes the borrow. | [4.2](./ch04-02-borrowing) |
| `break` | Leaves the nearest `while`, `loop` or `for`. It takes no label and no value. | [3.5](./ch03-05-control-flow) |
| `class` | Reserved, with no meaning. CeeSafe has structs, enums and interfaces, and no classes. | [5.1](./ch05-01-defining-structs) for structs |
| `const` | Declares a constant, a value fixed when the program is compiled: `const LIMIT: u32 = 10;`. Also declares a const parameter of a generic item, and a constant of an interface. | [3.1](./ch03-01-variables), [20.3](./ch20-03-more-interfaces-and-generics) |
| `continue` | Starts the next turn of the nearest `while`, `loop` or `for`. | [3.5](./ch03-05-control-flow) |
| `defer` | Runs a statement when the enclosing block ends, however it ends. | [15.2](./ch15-02-destruction) |
| `drop` | Begins a `drop` block in a struct: the code that runs when a value of the struct is destroyed. | [15.2](./ch15-02-destruction) |
| `else` | The other branch of an `if`. Also the block of `let PATTERN = e else { … };`, which runs when the pattern does not fit. | [3.5](./ch03-05-control-flow), [19.1](./ch19-01-where-patterns-go) |
| `enum` | Declares an enum, a type whose value is one of several variants. | [6.1](./ch06-01-defining-an-enum) |
| `extern` | Begins an `extern "C" { … }` block, which declares functions and types that C defines. | [20.2](./ch20-02-calling-c) |
| `false` | The `bool` value that is not `true`. | [3.2](./ch03-02-data-types) |
| `fn` | Declares a function or a method. In a type, `fn(i32) -> bool` is the type of a function value. | [3.3](./ch03-03-functions), [13.1](./ch13-01-closures) |
| `for` | Loops over a range or over a value with a `next` method: `for (x in items)`. Also joins the two types of `impl Interface for Type`. | [3.5](./ch03-05-control-flow), [13.2](./ch13-02-iteration), [10.2](./ch10-02-interfaces) |
| `if` | Chooses between blocks by a `bool` condition, as a statement or as an expression. After a pattern in a `match` arm, `if (…)` is a guard. | [3.5](./ch03-05-control-flow), [6.2](./ch06-02-match) |
| `impl` | Begins a block of methods for a type, `impl Type { … }`, or the implementation of an interface, `impl Interface for Type { … }`. | [5.3](./ch05-03-methods), [10.2](./ch10-02-interfaces) |
| `import` | Makes another package, or one item of it, usable by name in this file: `import tally;`. | [7.3](./ch07-03-imports) |
| `in` | Separates the variable of a `for` loop from what it loops over: `for (i in 0..4)`. | [3.5](./ch03-05-control-flow) |
| `interface` | Declares an interface, a set of methods that a type can implement. | [10.2](./ch10-02-interfaces) |
| `internal` | A visibility that says what writing nothing says: the item is visible in every file of its own package. | [7.2](./ch07-02-files-and-public) |
| `let` | Declares a local that cannot be assigned again. `let _ = e;` evaluates `e` and discards its value. | [3.1](./ch03-01-variables), [9.2](./ch09-02-result) |
| `loop` | A loop with no condition. It ends with `break` or `return`. | [3.5](./ch03-05-control-flow) |
| `macro` | Reserved, with no meaning. CeeSafe has no macros. | [20.5](./ch20-05-derives-and-macros) |
| `match` | Chooses an arm by the pattern that fits a value, as a statement or as an expression. Every value must be covered. | [6.2](./ch06-02-match), [19.2](./ch19-02-patterns-that-can-fail) |
| `move` | Before a closure, `move (x) => …`, makes a closure that owns what it captures; `async move` does so for a task. As an expression, `move x` moves the value out of `x`, also when its type is Copy; `x` cannot be used afterwards. | [13.1](./ch13-01-closures), [17.2](./ch17-02-scopes), [15.2](./ch15-02-destruction) |
| `mut` | Marks what may be changed through a borrow or a pointer: the type `borrow mut T`, the argument `mut x` for a `borrow mut` parameter, the receiver `borrow mut this`, the raw pointer `*mut T`, a `static mut`, and `dyn mut fn`. A local that changes is a `var`, not a `let mut`. | [4.2](./ch04-02-borrowing), [5.3](./ch05-03-methods), [20.1](./ch20-01-unsafe-code) |
| `namespace` | Reserved. A line `namespace a.b;` at the top of a file is accepted and has no effect. CeeSafe has packages, and no namespaces. | [7.2](./ch07-02-files-and-public) |
| `null` | The optional value that holds nothing. Only a value of an optional type `T?` can be `null`. | [6.3](./ch06-03-optionals) |
| `own` | Reserved, with no meaning. | not used by the language |
| `private` | A visibility: the item, field or method is visible only in the file that declares it. | [7.2](./ch07-02-files-and-public), [5.1](./ch05-01-defining-structs) |
| `public` | A visibility: the item is visible to the packages that depend on this one. | [7.2](./ch07-02-files-and-public) |
| `return` | Leaves the function, with its result if it has one. | [3.3](./ch03-03-functions) |
| `Self` | Names a type: in an interface, the type that implements it; in an `impl` block or a struct's or enum's body, the type itself. | [10.2](./ch10-02-interfaces) |
| `static` | Declares a static, a variable that lives as long as the program: `static COUNT: AtomicU64 = …;`. Also the name of the region that lasts for the whole program, as in `StringView<static>`. | [22.2](./ch22-02-kernel-code), [10.3](./ch10-03-regions) |
| `struct` | Declares a struct, a type with named fields. | [5.1](./ch05-01-defining-structs) |
| `this` | The value a method is called on. It is the first parameter of a method: `this`, `borrow this` or `borrow mut this`. | [5.3](./ch05-03-methods) |
| `true` | The `bool` value that is not `false`. | [3.2](./ch03-02-data-types) |
| `type` | Declares an associated type in an interface, `type Unit;`, and defines it in an `impl`, `type Unit = u8;`. There are no type aliases. | [20.3](./ch20-03-more-interfaces-and-generics) |
| `unsafe` | Allows the operations the compiler cannot check, such as reading through a raw pointer or calling C: an `unsafe { … }` block or an `unsafe fn`. | [20.1](./ch20-01-unsafe-code) |
| `using` | Declares a local that stays in place until its block ends: `using file = open(path)?;`. Its contents may change, but it cannot be moved or assigned as a whole. | [15.2](./ch15-02-destruction) |
| `var` | Declares a local that can be assigned again. In a pattern, `var n` binds a name that can be assigned. | [3.1](./ch03-01-variables), [19.3](./ch19-03-the-syntax-whole) |
| `where` | Reserved, with no meaning. A bound is written with the type parameter: `fn f<T: Copy>(x: T)`. | [10.1](./ch10-01-generic-types) for bounds |
| `while` | Loops as long as a `bool` condition holds. | [3.5](./ch03-05-control-flow) |
| `yield` | Reserved, with no meaning. CeeSafe has no generators: an iterator is a struct with a `next` method. | [13.2](./ch13-02-iteration) |

A reserved word where a name is expected is `N0100`, and the message says that the word is reserved:

<!-- ceesafe: fail N0100 -->
```ceesafe
fn main() {
    let type = 1;
}
```

```text
main.cee:2:9: error[N0100]: expected identifier, found 'type', which is a reserved word: choose another name (grammar.md §1.3)
  |
2 |     let type = 1;
  |         ^^^^
```

## Contextual words

A contextual word is not reserved. The parser gives it a meaning only in the place the table names, and
everywhere else it is a name you may declare and use.

| Word | Where it is special | What it means there | Taught in |
|---|---|---|---|
| `is` | After a complete operand, in an expression | `e is PATTERN` tests whether the value fits the pattern, and binds the pattern's names. | [6.3](./ch06-03-optionals), [19.1](./ch19-01-where-patterns-go) |
| `region` | Before a name, in the parameter list of a generic item: `struct Token<region r>` | Declares a region parameter, which says how long the borrows inside the type last. | [10.3](./ch10-03-regions) |
| `dyn` | In a type, before an interface, `fn` or `mut fn`: `Box<dyn Shape>` | A value of some type that implements the interface, with its methods found when the program runs. `dyn fn` is the type of a closure that owns what it captures. | [18.1](./ch18-01-dynamic-dispatch), [13.1](./ch13-01-closures) |
| `union` | In an `extern "C"` block, before a name or `{` | Declares a C union, or an anonymous union member of a C struct. | not in the chapters; [the specification](https://ceesafe.github.io/CeeSafe/reference/type-system), §31.1 |
| `from` | After `@`, before a variant of an enum | The attribute `@from`, listed below. | [9.2](./ch09-02-result) |
| `Send` | After `+` in a `dyn` type: `dyn Job + Send` | The value behind the `dyn` may move to another thread. Elsewhere `Send` is the library's interface of that name. | [18.1](./ch18-01-dynamic-dispatch), [16.4](./ch16-04-send-and-share) |
| `Share` | After `+` in a `dyn` type: `dyn Job + Share` | The value behind the `dyn` may be used from several threads at once. Elsewhere `Share` is the library's interface of that name. | [18.1](./ch18-01-dynamic-dispatch), [16.4](./ch16-04-send-and-share) |

The parser also knows the names of the primitive types (`u8`, `u16`, `u32`, `u64`, `i8`, `i16`, `i32`, `i64`,
`usize`, `isize`, `f32`, `f64`, `bool`, `char`) for one purpose: such a type takes no type arguments, so in
`a as u64 < b` the `<` is a comparison.

This program uses two contextual words as names, and `is` once as the operator:

```ceesafe
fn main() {
    let region = 3;
    let is = region is 3;
    Console.writeLine(is);
}
```

```text
true
```

## Attributes

An attribute is `@name` or `@name(arguments)`, written before a declaration. Attribute names are not reserved
words: after `@` the parser reads a name, and the checker accepts the names of this table, each only on the kind
of declaration the table says. Anything else is `N0290`.

| Attribute | Goes on | What it means | Taught in |
|---|---|---|---|
| `@test` | a top-level function with no parameters | `ceesafec test` runs the function as a test. `ceesafec build` leaves it out. | [11.1](./ch11-01-writing-tests) |
| `@from` | a variant of an enum that has exactly one payload | `?` converts an error of the payload's type into this variant. | [9.2](./ch09-02-result) |
| `@into` | a method `fn m(this) -> F` of a struct or of an enum's `impl` block, one for each type | `?` converts an error of the method's type into `F` by calling the method. `SendError<T>.reason()` of lib/std is one. | [17.4](./ch17-04-async-io) |
| `@derive` | a struct or an enum | `@derive(Equal, HashKey, OrderKey)` writes one or more of these for the type: `==` and `!=`, a `HashKey` implementation, an `OrderKey` implementation. | [20.5](./ch20-05-derives-and-macros) |
| `@noncopy` | a struct | The struct is a move type even if every field is Copy. | [20.5](./ch20-05-derives-and-macros) |
| `@align` | a struct | `@align(n)` gives the struct an alignment of at least `n` bytes. | [20.5](./ch20-05-derives-and-macros) |
| `@export` | a top-level function, a constant or a static | C can call the function, or read the data, under its name or under the symbol given: `@export("tally_add")`. | [14.3](./ch14-03-libraries-for-other-programs), [20.2](./ch20-02-calling-c) |
| `@repr` | a struct, or an enum without payloads | `@repr(C)` lays a struct out as C does. `@repr(u8)` to `@repr(i64)` passes an enum to and from C as that integer type. | [20.2](./ch20-02-calling-c), [14.3](./ch14-03-libraries-for-other-programs) |
| `@callback` | a top-level function | The function has a C address and no C symbol, to pass to C as a function pointer. | [20.2](./ch20-02-calling-c) |
| `@header` | an `extern "C"` block | `@header("path", …)` names the C headers that define the block's types, constants and inline functions. | [20.2](./ch20-02-calling-c) |
| `@cname` | a field of a C struct, or a function or static of an `extern "C"` block | `@cname("name")` gives the name C uses, when the declaration has another name in CeeSafe. | [20.2](./ch20-02-calling-c) |
| `@ctype` | a struct, union or enum of an `extern "C"` block | `@ctype("name")` gives the type's spelling in C when it is a typedef, instead of `struct name`. | named in [20.5](./ch20-05-derives-and-macros); [the specification](https://ceesafe.github.io/CeeSafe/reference/type-system), §31.1 |
| `@cvalue` | a constant of an `extern "C"` block | `@cvalue("expression")` gives the C constant expression whose value the C compiler asserts. | named in [20.5](./ch20-05-derives-and-macros); [the specification](https://ceesafe.github.io/CeeSafe/reference/type-system), §28.2 |
| `@cfg` | an `@export` function, or a function or field of an `extern "C"` block | `@cfg("NAME")` says the declaration exists in C only in builds where the C macro `NAME` is not zero. | named in [20.5](./ch20-05-derives-and-macros); [the specification](https://ceesafe.github.io/CeeSafe/reference/type-system), §28.4 |
| `@constant` | a function or a method; a function of an `extern "C"` block | The function can be called in the initial value of a static. | [22.2](./ch22-02-kernel-code) |
| `@context` | a function or a method; `@context(atomic)` also a struct | `@context(atomic)` and `@context(irq)` name a context that forbids effects: code in it must not sleep. | [22.2](./ch22-02-kernel-code) |
| `@effects` | a function, a method, a function type or an `extern "C"` function | `@effects(…)` lists the effects the function may have, of `sleep`, `alloc` and `suspend`. | [22.2](./ch22-02-kernel-code) |
| `@effectsOf` | a function or a method | `@effectsOf(p, …)` says that a call has, besides its own effects, those of the function passed for each parameter named. | not in the chapters; [the specification](https://ceesafe.github.io/CeeSafe/reference/type-system), §32.5 |
| `@placed` | a method `fn m(borrow mut this)` | The method is its type's placement initialiser, which builds a value where it will stay. | named in [20.5](./ch20-05-derives-and-macros); [the specification](https://ceesafe.github.io/CeeSafe/reference/type-system), §33.3 |
| `@threadSafe` | a struct, in the libraries and in a package whose `ceesafe.toml` says `trusted = true` | Declares the struct `Send` and `Share` on the author's word. In any other package it is `N0290`. | not in the chapters; [the specification](https://ceesafe.github.io/CeeSafe/reference/type-system), §38.3 |
| `@interiorMutable` | a struct, in the core library only | Marks the library's `RawCell`, the type on which `Cell` and `RefCell` are built. In any package it is `N0290`. | not in the chapters; [15.4](./ch15-04-cells) teaches the cells built on it |
