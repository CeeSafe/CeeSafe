---
title: Generic types and functions
summary: Type parameters on functions, structs and enums - how the compiler infers type arguments and when you write them, what code may do with a value of type T, bounds such as T Copy, and why generic code costs nothing at run time.
assumes: [ch03-03-functions, ch05-01-defining-structs, ch06-01-defining-an-enum, ch06-03-optionals]
keywords: [generic, type parameter, type argument, inference, bound, Copy, monomorphisation, "Pair<T>"]
diagnostics: [N0207, N0250, N0265]
specification: ["type-system.md §20.1", "type-system.md §20.2", "type-system.md §20.3", "type-system.md §20.4"]
---

# Generic types and functions

**This section teaches** how to declare a function, a struct or an enum that works for many types, how the
compiler chooses the types at each use, and what a bound allows. It assumes [Functions](./ch03-03-functions),
[Defining structs](./ch05-01-defining-structs), [Defining an enum](./ch06-01-defining-an-enum) and
[Optionals](./ch06-03-optionals).

## A generic function

A **type parameter** is a name for a type that the caller chooses. It is declared between `<` and `>` after the
function's name, and then used as a type in the parameters, the result and the body.

<span class="listing-caption"><strong>Listing 10-3</strong>, <code>main.cee</code>: A generic function, called with inferred and with written type arguments.</span>

```ceesafe
fn when<T>(condition: bool, value: T) -> T? => if (condition) value else null;

fn main() {
    Console.writeLine(when(true, 5) ?? 0);
    Console.writeLine(when(false, "tea") ?? "nothing");
    Console.writeLine(when<u8>(true, 200) ?? 0);
}
```

```text
5
nothing
200
```

`when` gives back its value if the condition holds, and `null` if not. The type that a call puts in place of `T`
is a **type argument**.

- `when(true, 5)` does not say what `T` is. The compiler **infers** it from the argument: `5` is an `i32`, so `T`
  is `i32` and the result is an `i32?`.
- `when(false, "tea")` infers `T` as `StringView`.
- `when<u8>(true, 200)` writes the type argument. The literal `200` is then a `u8`.

::: tip Rule
A call infers its type arguments from the types of its arguments. When the arguments do not determine a type
parameter, write the type arguments after the name: `when<u8>(…)`.

*Specification: `type-system.md` §20.2.*
:::

## Generic structs and enums

A struct or an enum takes type parameters after its name. Its fields, payloads and methods may use them.

<span class="listing-caption"><strong>Listing 10-4</strong>, <code>main.cee</code>: A generic struct with a method, and a generic enum.</span>

```ceesafe
struct Pair<T> {
    first: T;
    second: T;

    fn swapped(this) -> Pair<T> => Pair { first: this.second, second: this.first };
}

enum Maybe<T> {
    Nothing,
    Just(T),
}

fn main() {
    let p = Pair { first: 1, second: 2 };
    Console.writeLine(p.swapped().first);
    let bytes = Pair<u8> { first: 1, second: 2 };
    Console.writeLine(bytes.second);
    let halves: Pair<f64> = Pair { first: 0.5, second: 1.5 };
    Console.writeLine(halves.first);

    let found = Maybe.Just(7);
    let missing: Maybe<i32> = Maybe.Nothing;
    for (m in [found, missing]) {
        match (m) {
            Maybe.Just(v) => { Console.writeLine(v); }
            Maybe.Nothing => { Console.writeLine("nothing"); }
        }
    }
}
```

```text
2
2
0.5
7
nothing
```

A value of a generic type gets its type arguments in one of three ways:

- from its fields or payload: `Pair { first: 1, second: 2 }` is a `Pair<i32>`, and `Maybe.Just(7)` is a
  `Maybe<i32>`;
- written: `Pair<u8> { … }`, `Maybe<i32>.Nothing`;
- from the type the place expects: `let halves: Pair<f64> = Pair { … }`, `let missing: Maybe<i32> = Maybe.Nothing`.

As the name of a type, a generic struct or enum always has its type arguments: a parameter is a `Pair<i32>`, never
a bare `Pair`. A type may have several parameters: `struct Entry<K, V> { key: K; value: V; }`.

You have used generic types already. `Vector<T>`, `HashMap<K, V>` and `Result<T, E>` are generic types of the
library, and `T?` is an optional of any `T`. A static function of a generic type is called with the type written,
`Vector<i32>.create()`, because `create` has no argument to infer from.

## What generic code may do with a `T`

The compiler checks a generic function once, where it is declared, for every type that `T` could be. So the body
may do only what is possible for every type: move a `T`, borrow it, store it in a field, return it. It may not add
two values of type `T`, compare them, read a field or call a method, because not every type has those. It may not
use the same value twice either, because not every type can be copied.

A **bound** on a type parameter narrows the types it stands for, and in return allows the body more. `T: Copy`
accepts only Copy types (numbers, `bool`, views, and structs and enums made of them), and allows the body to use
a value more than once.

<span class="listing-caption"><strong>Listing 10-5</strong>, <code>main.cee</code>: The bound <code>T: Copy</code> lets the function use <code>value</code> twice.</span>

```ceesafe
struct Pair<T> {
    first: T;
    second: T;
}

fn twice<T: Copy>(value: T) -> Pair<T> {
    return Pair { first: value, second: value };
}

fn main() {
    let p = twice(4);
    Console.writeLine(p.first + p.second);
    let q = twice("ab");
    Console.write(q.first);
    Console.writeLine(q.second);
}
```

```text
8
abab
```

`main` may add `p.first` and `p.second`: there the type is known to be `Pair<i32>`. Only the inside of `twice`
sees an unknown `T`.

::: tip Rule
Inside generic code, a value of type `T` can be moved, borrowed, stored and returned. Anything more comes from a
bound: `T: Copy` allows copies, and an interface allows its methods.

*Specification: `type-system.md` §20.3.*
:::

The other kind of bound is an interface, which says which methods a type has. `<T: Shape>` allows the body to call
the methods of `Shape` on a `T`, and several bounds are joined with `+`: `<T: Shape + Copy>`.
[Interfaces](./ch10-02-interfaces) teaches them.

::: info Two parameters of type `T`, and views
With `ceesafec` 0.40.0, a generic function that takes two or more values of type `T` and returns a `T`, such as
`fn choose<T>(first: bool, a: T, b: T) -> T`, cannot be called with a view such as `StringView`. The compiler
reports `N0274` at the call. A view borrows, and the result does not say which argument it borrows from.
[Regions](./ch10-03-regions) shows how to write such a function for views.
:::

## No cost at run time

For each set of type arguments a program uses, the compiler makes a copy of the generic function or type with the
types filled in, and compiles that copy as if you had written it by hand. Listing 10-3 uses `when` with `i32`,
`StringView` and `u8`, so the built program holds three functions: `ceesafec build --emit c` shows them in the
generated C as `ceeG_4when1i32`, `ceeG_4when1PZ` and `ceeG_4when1u8`. No type is looked up while the program runs,
and nothing is boxed. A generic function that is never called produces no code.

::: tip Rule
Each use of a generic function or type with different type arguments is compiled as its own copy. Generic code
runs as fast as code written for one type.

*Specification: `type-system.md` §20.4.*
:::

## Mistakes

::: warning Mistake: an operator on a `T`
Writing the body as if `T` were a number. Nothing says that every `T` has `>`.
:::

<span class="listing-caption"><strong>Listing 10-6</strong>, <code>main.cee</code>: The body compares two values of an unknown type. This does not compile.</span>

<!-- ceesafe: fail N0207 -->
```ceesafe
fn larger<T>(a: T, b: T) -> T {
    if (a > b) {
        return a;
    }
    return b;
}

fn main() {
    Console.writeLine(larger(3, 7));
}
```

```text
main.cee:2:9: error[N0207]: operator '>' cannot be applied to type 'T'
  |
2 |     if (a > b) {
  |         ^^^^^
```

The error is in the declaration, not at the call: `larger` is wrong for some `T`, so it is wrong, even though
`main` only uses it with numbers. No bound gives a `T` the operator `>`. Bound `T` by an interface with a method
that compares, and call that method: [Interfaces](./ch10-02-interfaces#the-interfaces-of-the-library) shows
`OrderKey`.

::: warning Mistake: using a `T` twice without `Copy`
A `T` may be a type that moves, such as a `String`, and a value that has moved cannot be used again.
:::

<span class="listing-caption"><strong>Listing 10-7</strong>, <code>main.cee</code>: Listing 10-5 without the bound. This does not compile.</span>

<!-- ceesafe: fail N0250 -->
```ceesafe
struct Pair<T> {
    first: T;
    second: T;
}

fn twice<T>(value: T) -> Pair<T> {
    return Pair { first: value, second: value };
}

fn main() {
    let p = twice(4);
    Console.writeLine(p.first + p.second);
}
```

```text
main.cee:7:12: error[N0250]: 'value' is used here after it was moved (on at least one path)
  |
7 |     return Pair { first: value, second: value };
  |            ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
```

`first: value` moves `value` into the pair, and `second: value` uses it again. Add the bound, `<T: Copy>`, as
Listing 10-5 does.

::: warning Mistake: a type argument that nothing determines
A call infers from its arguments only. The type of the variable that receives the result does not help.
:::

<span class="listing-caption"><strong>Listing 10-8</strong>, <code>main.cee</code>: No argument of <code>nothing</code> mentions <code>T</code>. This does not compile.</span>

<!-- ceesafe: fail N0265 -->
```ceesafe
fn nothing<T>() -> T? {
    return null;
}

fn main() {
    let a: i32? = nothing();
    Console.writeLine(a ?? 1);
}
```

```text
main.cee:6:19: error[N0265]: cannot infer 'T' for 'nothing' here; write the type arguments: 'nothing<…>'
  |
6 |     let a: i32? = nothing();
  |                   ^^^^^^^^^
```

Write the type argument as the message says: `let a = nothing<i32>();`. The same code is reported for
`let m = Maybe.Nothing;` and for `var v = Vector.create();`, where no payload or argument gives the type.

::: details Coming from Rust
Type parameters, bounds and one compiled copy for each set of type arguments work as in Rust. Four things differ:

- type arguments at a call are written `f<u8>(x)`, not `f::<u8>(x)`;
- a call does not infer from the expected type of its result, so `let a: i32? = nothing();` needs `nothing<i32>()`;
- there is no `where` clause for bounds: they are written in the parameter list, `<T: Shape + Copy>`;
- there are no operator interfaces such as `Add` or `PartialOrd`: an operator cannot be applied to a `T`.
:::

::: details Coming from C#, Java, TypeScript or Go
A CeeSafe generic is not checked against each call, as a C++ template is, and is not erased, as a Java or
TypeScript generic is. The body is checked once against its bounds, as in C# or Go, and then compiled separately
for each set of type arguments, for value types and all others alike. Nothing is boxed.
:::

## Summary

- `fn f<T>(…)`, `struct S<T>` and `enum E<T>` declare type parameters.
- A call infers type arguments from its arguments; a struct or enum value infers them from its fields or payload,
  or from the type its place expects. Otherwise write them: `f<u8>(…)`, `Pair<u8> { … }`, `Vector<i32>.create()`
  (`N0265` if they are missing).
- Generic code is checked once. A `T` can be moved, borrowed, stored and returned, and nothing else: no operators
  (`N0207`), no second use (`N0250`).
- A bound allows more: `T: Copy` allows copies, an interface allows its methods.
- Each use with different types is compiled as its own copy, with no cost at run time.

Next: [Interfaces](./ch10-02-interfaces), which let generic code call methods on a `T`.
