---
title: "Appendix D: Derives"
summary: The three derives Equal, HashKey and OrderKey - the impl each one writes, what every field's type must have, the diagnostic for each way a derive can be wrong, and the language's other attributes with where each is taught.
keywords: ["@derive", Equal, HashKey, OrderKey, DerivedEqual, derivedEqual, hash, sameAs, compareKey, deriveHash, deriveCompare, attribute, "@test", "@from", "@export", "@repr", "@noncopy", "@align"]
diagnostics: [N0207, N0222, N0268, N0290]
specification: ["type-system.md §28.1", "type-system.md §28.7"]
---

# Appendix D: Derives

This appendix lists what `@derive` can write for a struct or an enum: the three derives, the `impl` each one
writes, what each needs of the type's fields, and the diagnostic for each way to get it wrong. Its last table lists
the language's other attributes. To learn derives, read [20.5](./ch20-05-derives-and-macros). Use this page to look
one up: every row of every table can be read without the rows around it.

`@derive(…)` is written before a `struct` or an `enum`, with one or more of the names `Equal`, `HashKey` and
`OrderKey`, each at most once: `@derive(Equal, HashKey)`. The set is fixed. A program cannot define a derive, and
there is none for copying, cloning, printing or default values.

## The derives

| Derive | What the type gets | It also needs | Taught in |
|---|---|---|---|
| `@derive(Equal)` | `a == b` and `a != b` on two values of the type. Two structs are equal when every field is equal. Two enum values are equal when they are the same variant and every payload is equal. | nothing else | [5.1](./ch05-01-defining-structs), [20.5](./ch20-05-derives-and-macros) |
| `@derive(Equal, HashKey)` | The type can be the key of a `HashMap`. Two keys are the same key when they are `==`. | `Equal` in the same `@derive`, since the map tells two keys apart with `==` | [8.3](./ch08-03-maps), [20.5](./ch20-05-derives-and-macros) |
| `@derive(OrderKey)` | `a.compareKey(b)`, which is negative when `a` comes first, zero when they are the same and positive when `a` comes after, and the type can be the key of an `OrderedMap`. It does not give the type `<` or `>`. | nothing else | [8.3](./ch08-03-maps), [20.5](./ch20-05-derives-and-macros) |

## What each derive writes

Each derive writes one `impl` for the type, here called `S`. The `impl` is ordinary code: the compiler checks it
as it checks code you wrote, and `ceesafec check --dump-hir` prints it with the rest of the program.

| Derive | The `impl` it writes | The functions in it | What they do |
|---|---|---|---|
| `Equal` | `impl DerivedEqual<S> for S`. `DerivedEqual` is an interface of the core library that only `@derive(Equal)` may implement. | `fn derivedEqual(borrow this, other: borrow S) -> bool` | Compares the fields in the order they are declared, joined by `&&`, so it stops at the first pair that differs. For an enum it compares the variant, then the payloads in order. `a == b` calls it, and `a != b` is its negation. Neither moves its operands. |
| `HashKey` | `impl HashKey<S> for S`. `HashKey` is the standard library's interface for a `HashMap` key ([8.3](./ch08-03-maps)). | `fn hash(borrow this, h: borrow mut Hasher)` and `fn sameAs(borrow this, other: borrow S) -> bool` | `hash` passes each field, in declaration order, to the library function `deriveHash(field, mut h)`. For an enum it first hashes the variant's index, counted from 0 in declaration order, as a `u32`, then the payloads. `sameAs` returns `this.derivedEqual(other)`. |
| `OrderKey` | `impl OrderKey<S> for S`. `OrderKey` is the standard library's interface for an `OrderedMap` key ([8.3](./ch08-03-maps)). | `fn compareKey(borrow this, other: borrow S) -> i32` | Compares the first field with the library function `deriveCompare(a, b)` and returns the result if it is not zero, then the second field, and so on, and returns 0 when every field is the same. For an enum a variant declared earlier comes before one declared later, and two values of one variant compare their payloads in order. |

So the order of the fields is the order of the values: put the most significant field first.

A program can write `HashKey` and `OrderKey` by hand, and does when the derived order or hash is not the one it
wants. This program derives `Equal` and writes by hand exactly what `@derive(HashKey, OrderKey)` would have written
for the same struct.

```ceesafe
@derive(Equal)
struct Point {
    x: i32;
    y: i32;
}

impl HashKey<Point> for Point {
    fn hash(borrow this, h: borrow mut Hasher) {
        deriveHash(this.x, mut h);
        deriveHash(this.y, mut h);
    }

    fn sameAs(borrow this, other: borrow Point) -> bool => this == other;
}

impl OrderKey<Point> for Point {
    fn compareKey(borrow this, other: borrow Point) -> i32 {
        let c0 = deriveCompare(this.x, other.x);
        if (c0 != 0) {
            return c0;
        }
        let c1 = deriveCompare(this.y, other.y);
        if (c1 != 0) {
            return c1;
        }
        return 0;
    }
}

fn main() {
    let a = Point { x: 1, y: 9 };
    let b = Point { x: 1, y: 12 };
    Console.writeLine(a == b);
    Console.writeLine(a.compareKey(b));
    var seen = HashMap<Point, bool>.create();
    seen.put(a, true)!;
    Console.writeLine(seen.contains(Point { x: 1, y: 9 }));
}
```

```text
false
-1
true
```

`Equal` is the exception. `==` on a struct or an enum with payloads comes only from the derive, so that `==` has
one meaning: an `impl DerivedEqual` written in a program is an error (`N0290`).

## What a field's type must have

A derive goes through every field of a struct and every payload of an enum's variants, so each of their types must
have what the derive uses: `==` for `Equal`, an `impl` of `HashKey` for `HashKey`, an `impl` of `OrderKey` for
`OrderKey`. "Yes" in this table means that a struct with a field of that type checks with that derive. A code
means that the compiler reports that diagnostic at the derive's name.

| A field or payload of type | `Equal` | `HashKey` | `OrderKey` |
|---|---|---|---|
| `i8`, `i16`, `i32`, `i64`, `u8`, `u16`, `u32`, `u64`, `usize` | yes | yes | yes |
| `isize` | yes | `N0268`: the library has no `HashKey` for `isize` | yes |
| `bool` | yes | yes | yes: `false` comes before `true` |
| `f32`, `f64` | yes, by the IEEE rule, so a NaN is not equal to itself | `N0268` | `N0268` |
| an enum whose variants have no payloads, without any derive of its own | yes | yes: its variant's index is hashed | yes: its variant's index is compared |
| a struct or enum that has the same derive | yes | yes | yes |
| a struct, or an enum with payloads, that does not have the derive | `N0207` | `N0268` | `N0268` |
| `String` | yes: the same text | yes | yes: by its bytes, and a prefix comes first |
| `Vector<u8>` | yes: the same length and bytes | yes | yes: by its bytes, and a prefix comes first |
| `Vector<T>` for any other `T` that has `==` | yes: the same length and equal elements in order | `N0268` | `N0268` |
| an array `[T; N]` of a `T` that has `==` | yes: equal elements in order | `N0268` | `N0268` |
| `StringView` (the struct then has a region parameter, [10.3](./ch10-03-regions)) | yes: the same text | `N0268` | `N0268` |
| `Slice<r, T>` of a `T` that has `==` | yes: the same length and equal elements in order | `N0268` | `N0268` |
| an optional `T?` of a `T` that has `==` | yes: two `null`s are equal, and two values are compared | `N0268` | `N0268` |
| `Box<T>` | yes when `T` has `==`: the boxed values are compared | `N0268` | `N0268` |
| a borrow, `borrow(r) T` | yes when `T` has `==`: the borrowed values are compared | yes when `T` is a `HashKey` | yes when `T` is an `OrderKey` |
| a raw pointer, `*T` | yes: the same address | `N0268` | `N0268` |
| `Cell<T>` of a `T` that has `==` | yes: the contents are compared | `N0268` | `N0268` |
| a `HashMap`, an `OrderedMap`, a closure or function type | `N0207` | `N0268` | `N0268` |

Where the table says `N0268` for a field you need in a key, write that interface's `impl` for the type by hand, as
the program above does, and leave the field out of it or handle it there.

**Generic types.** A derive on a generic struct or enum writes a generic `impl` with the type's own parameters. For
`HashKey` and `OrderKey`, each type parameter also gets that interface as a bound. For `Equal`, the fields are
checked for each instance, at the first `==` on a value of that instance: `Wrap<u32>` can be compared, and
`Wrap<Plain>`, where `Plain` has no `==`, is `N0222` at the comparison. Generic types are taught in
[10.1](./ch10-01-generic-types).

## Diagnostics

Each message below is the compiler's own, from release 0.42.0. A diagnostic about a derive points at the derive's
name inside the attribute, not at the field, because the fault is in the code the derive writes.

| What is wrong | The compiler reports | The fix |
|---|---|---|
| A name that is not one of the three, as in `@derive(Clone)` or `@derive(Debug)` | `error[N0290]: '@derive' takes Equal, HashKey and OrderKey; Clone, Default and Debug are written as methods (ADR 0087 §2.3)` | Remove the name. Write a method instead, such as `clone` or one that prints the fields. |
| One derive named twice, as in `@derive(Equal, Equal)` | `error[N0290]: this derive is named twice (ADR 0087 §2.3)` | Name it once. |
| `@derive` or `@derive()` with no name | `error[N0290]: '@derive' names what it derives: Equal, HashKey or OrderKey (ADR 0087 §2.3)` | Name what to derive. |
| `@derive(HashKey)` without `Equal` | `error[N0290]: '@derive(HashKey)' needs Equal too, since 'sameAs' is '==': write '@derive(Equal, HashKey)' (ADR 0087 §2.3)` | Write `@derive(Equal, HashKey)`. |
| Two `@derive` attributes on one struct or enum | `error[N0290]: '@derive' is given twice (type-system.md §28.1)` | Put every name in one attribute, as in `@derive(Equal, OrderKey)`. |
| `@derive` before a function | `error[N0290]: '@derive' cannot be used on a function (type-system.md §28.1)` | Derive only on a struct or an enum. |
| `@derive` before an interface or another declaration that is not a struct or an enum | `error[N0290]: '@derive' cannot be used on this declaration (type-system.md §28.1)` | Derive only on a struct or an enum. |
| An `impl DerivedEqual<S> for S` written by hand | `error[N0290]: 'DerivedEqual' is implemented only by '@derive(Equal)': '==' has one meaning, so it cannot be written by hand (ADR 0087 §2.3)` | Use `@derive(Equal)`. For an equality that means something else, write a method with a name. |
| `@derive(Equal)` on a type with a field whose type has no `==` | `error[N0207]: '@derive(Equal)' compares field 'plain', whose type 'Plain' has no '==': derive Equal for it too, or compare the type in a method of its own (ADR 0087 §2.3)` | Add `@derive(Equal)` to the field's type. |
| `@derive(Equal, HashKey)` on a type with a field whose type is not a `HashKey` | `error[N0268]: 'f64' does not implement 'HashKey<f64>', which 'K' requires` | Derive `HashKey` for the field's type, or write `impl HashKey` for the outer type by hand. |
| `@derive(OrderKey)` on a type with a field whose type is not an `OrderKey` | `error[N0268]: 'Plain' does not implement 'OrderKey<Plain>', which 'K' requires` | Derive `OrderKey` for the field's type, or write `impl OrderKey` for the outer type by hand. |
| `==` on an instance of a generic type with `@derive(Equal)` whose type argument has no `==` | `error[N0222]: 'Wrap<Plain>.derivedEqual' cannot be used: for these arguments its body has an error: '@derive(Equal)' compares field 'value', whose type 'Plain' has no '==': derive Equal for it too, or compare the type in a method of its own (ADR 0087 §2.3) (type-system.md §20.4)` | Add `@derive(Equal)` to the type argument's type. |

These are what the compiler reports when a derive is missing. Each is fixed by adding the derive.

| What the program does | The compiler reports | The derive that is missing |
|---|---|---|
| `a == b` on a struct without `@derive(Equal)` | `error[N0207]: operator '==' cannot be applied to type 'Plain'` | `@derive(Equal)` on the struct |
| `a == b` on an enum with payloads and without `@derive(Equal)` | `error[N0207]: '==' compares enums whose variants have no payloads; 'Shape' has payloads: compare with 'match' (type-system.md §15.1)` | `@derive(Equal)` on the enum, or a `match` ([6.2](./ch06-02-match)) |
| `HashMap<V, u32>` where `V` is not a `HashKey` | `error[N0268]: 'V' does not implement 'HashKey<V>', which 'K' requires` | `@derive(Equal, HashKey)` on `V` |
| `OrderedMap<V, u32>` where `V` is not an `OrderKey` | `error[N0268]: 'V' does not implement 'OrderKey<V>', which 'K' requires` | `@derive(OrderKey)` on `V` |
| `a < b` on a type with `@derive(OrderKey)` | `error[N0207]: operator '<' cannot be applied to type 'V'` | none: write `a.compareKey(b) < 0` |

An enum whose variants have no payloads has `==` and `!=` without any derive ([6.1](./ch06-01-defining-an-enum)).

## The other attributes

`@derive` is one of the language's attributes: a name after `@`, written before a declaration. The set is fixed by
the language, and a name that is not in it is `error[N0290]: unknown attribute '@inline' (type-system.md §28.1)`.
An attribute in the wrong place, with the wrong arguments, or given twice is `N0290` too. Only `@derive` writes
code. The others change how one declaration is checked or compiled.

| Attribute | It goes before | What it does | Taught in |
|---|---|---|---|
| `@test` | a top-level function with no parameters that returns `void` or `Result<void, E>` | Makes the function a test that `ceesafec test` runs. `ceesafec build` leaves it out. | [11.1](./ch11-01-writing-tests) |
| `@from` | a variant of an enum that has exactly one payload | Lets `?` convert an error of the payload's type into this variant. | [9.2](./ch09-02-result) |
| `@export`, `@export("symbol")` | a top-level function that is not generic, a constant, a static | Makes it callable or readable from C under that name. Without an argument the name is the item's own. | [14.3](./ch14-03-libraries-for-other-programs), [20.2](./ch20-02-calling-c) |
| `@cfg("NAME")` | a function that has `@export` | Gives the function its C symbol only in builds where the C macro `NAME` is not zero. | `type-system.md` §28.4 |
| `@repr(C)` | a struct that is not generic | Lays the struct out as C lays out a struct with the same fields. | [14.3](./ch14-03-libraries-for-other-programs), [20.2](./ch20-02-calling-c) |
| `@repr(u8)` and the other integer types up to `@repr(i64)` | an enum whose variants have no payloads | Passes the enum to and from C as that integer type. | [14.3](./ch14-03-libraries-for-other-programs), [20.2](./ch20-02-calling-c) |
| `@callback` | a top-level function | Makes the function a C address without a symbol, to give to C as a function pointer. | [20.2](./ch20-02-calling-c) |
| `@header("path", …)` | an `extern "C"` block | Names the C headers that define the block's types, constants and inline functions. | [20.2](./ch20-02-calling-c) |
| `@cname("name")` | a function or static of an `extern "C"` block, a field of a C struct declared there | Gives the item's name in C when it differs from its CeeSafe name. | [20.2](./ch20-02-calling-c) |
| `@ctype("cname")` | a struct, union or enum of an `extern "C"` block | Gives the type's spelling in C, for a typedef, instead of `struct name`. | `type-system.md` §31.1 |
| `@cvalue("expression")` | a constant of an `extern "C"` block | Names the C constant expression whose value the constant states, which the C compiler then checks. | `type-system.md` §28.2 |
| `@noncopy` | a struct | Makes the struct a move type even when every field is Copy. | [20.5](./ch20-05-derives-and-macros#the-other-attributes) |
| `@align(n)` | a struct that is not generic | Gives the struct an alignment of at least `n` bytes. | [20.5](./ch20-05-derives-and-macros#the-other-attributes) |
| `@effects(…)` | a function, a method, a function of an `extern "C"` block | States the effects the function may have: `sleep`, `alloc`, `suspend`, or none with `@effects()`. | [22.2](./ch22-02-kernel-code) |
| `@effectsOf(p, …)` | a function or method with closure or function parameters | Says that the function's effects at a call include those of the closure passed for each named parameter. | `type-system.md` §32.5 |
| `@context(atomic)`, `@context(irq)` | a function or method; `atomic` also a struct | Puts the code in a context that forbids some effects, such as sleeping. | [22.2](./ch22-02-kernel-code) |
| `@constant` | a function or method whose body is one expression; a function of an `extern "C"` block | Lets the function be called in the initialiser of a static. | [22.2](./ch22-02-kernel-code) |
| `@placed` | a method `fn m(borrow mut this)` | Makes the method the type's placement initialiser, which builds the value where it will live. | `type-system.md` §33.3 |
| `@threadSafe` | a struct of the libraries, or of a package whose manifest says `trusted = true` | Declares the type `Send` and `Share` on the author's word. Anywhere else it is `N0290`. | `type-system.md` §38.3 |
| `@interiorMutable` | the core library's `RawCell` only | Marks the one type whose contents may change through a shared borrow. Anywhere else it is `N0290`. | `type-system.md` §41.1 |

The specification's own table of attributes is in
[the type system, §28.1](https://ceesafe.github.io/CeeSafe/reference/type-system), and `@derive` is §28.7 of the
same page.
