---
title: Interfaces
summary: An interface lists methods, impl I for T implements it for a type, and a bound lets generic code call them - with default methods, Self, static functions, the interfaces of the library (Copy, Iterator, OrderKey, HashKey, derived equality) and the orphan rule.
assumes: [ch10-01-generic-types, ch05-03-methods]
keywords: [interface, impl, bound, default method, Self, static function, Copy, Iterator, OrderKey, HashKey, "@derive", orphan rule]
diagnostics: [N0267, N0268]
specification: ["type-system.md §21.1", "type-system.md §21.2"]
---

# Interfaces

**This section teaches** how to say what a type can do, with an interface, and how generic code uses that to call
methods on a type parameter. It assumes [Generic types and functions](./ch10-01-generic-types) and
[Methods](./ch05-03-methods).

Generic code may only move, borrow, store and return a value of type `T`. To call a method on it, the code must
know that every type `T` stands for has that method. An **interface** is a named list of methods, and a bound
`T: I` accepts only the types that implement the interface `I`.

## Declaring and implementing an interface

<span class="listing-caption"><strong>Listing 10-9</strong>, <code>main.cee</code>: An interface, two types that implement it, and a generic function bound by it.</span>

```ceesafe
interface Shape {
    fn area(borrow this) -> u64;
    fn name(borrow this) -> StringView;
}

struct Square {
    side: u64;
}

struct Rect {
    w: u64;
    h: u64;
}

impl Shape for Square {
    fn area(borrow this) -> u64 => this.side * this.side;
    fn name(borrow this) -> StringView => "square";
}

impl Shape for Rect {
    fn area(borrow this) -> u64 => this.w * this.h;
    fn name(borrow this) -> StringView => "rect";
}

fn describe<S: Shape>(shape: borrow S) {
    Console.write(shape.name());
    Console.write(": ");
    Console.writeLine(shape.area());
}

fn main() {
    let square = Square { side: 3 };
    describe(square);
    describe(Rect { w: 4, h: 5 });
    Console.writeLine(square.area());
}
```

```text
square: 9
rect: 20
9
```

- `interface Shape { … }` lists two methods without bodies. Each ends with `;`.
- `impl Shape for Square { … }` implements the interface for `Square`: it gives each method a body, with the same
  parameters and result as the interface says.
- `describe<S: Shape>` accepts a borrow of any type that implements `Shape`. Inside it, `shape.name()` and
  `shape.area()` are allowed because the bound promises them.
- The methods are also methods of the type: `square.area()` works without any generic code.

::: tip Rule
`impl I for T` must give every method of `I` that has no default, with the interface's signature. A type
implements an interface once.

*Specification: `type-system.md` §21.1.*
:::

A call through a bound is resolved when the program is compiled. `describe` is compiled once for `Square` and once
for `Rect`, and each copy calls its type's `area` directly. To hold values of different types in one collection
and choose the method while the program runs, there is `dyn Shape`, which chapter 18 covers.

## Default methods, `Self` and static functions

A method in an interface may have a body. That body is its **default**: an `impl` that leaves the method out gets
the default, and an `impl` that writes the method replaces it. Inside an interface, `Self` is the type that
implements it. A method without `this` is a **static function**: it is called on the type, not on a value.

<span class="listing-caption"><strong>Listing 10-10</strong>, <code>main.cee</code>: A default method, a static function, and <code>Self</code> as a result and a parameter type.</span>

```ceesafe
interface Shape {
    fn area(borrow this) -> u64;

    fn unit() -> Self;

    fn fitsIn(borrow this, other: borrow Self) -> bool => this.area() <= other.area();

    fn describe(borrow this) {
        Console.write("a shape of area ");
        Console.writeLine(this.area());
    }
}

struct Square {
    side: u64;
}

struct Rect {
    w: u64;
    h: u64;
}

impl Shape for Square {
    fn area(borrow this) -> u64 => this.side * this.side;
    fn unit() -> Self => Square { side: 1 };
}

impl Shape for Rect {
    fn area(borrow this) -> u64 => this.w * this.h;
    fn unit() -> Self => Self { w: 1, h: 1 };

    fn describe(borrow this) {
        Console.write(this.w);
        Console.write(" by ");
        Console.writeLine(this.h);
    }
}

fn unitsIn<S: Shape>(shape: borrow S) -> u64 => shape.area() / S.unit().area();

fn main() {
    let square = Square { side: 3 };
    square.describe();
    Rect { w: 4, h: 5 }.describe();
    Console.writeLine(Square.unit().fitsIn(square));
    Console.writeLine(unitsIn(square));
}
```

```text
a shape of area 9
4 by 5
true
9
```

- `Square` does not write `describe` or `fitsIn`, so it has the defaults. `Rect` writes its own `describe`.
- `fn unit() -> Self;` has no `this`. It is called as `Square.unit()` on a type, and as `S.unit()` through a
  bound.
- In `fitsIn`, `other: borrow Self` means "another value of the same implementing type": a `Square` is compared
  with a `Square`.
- In an `impl`, `Self` is the type being implemented, so `Self { w: 1, h: 1 }` is a `Rect`.

A default is checked once, in the interface, where `Self` is unknown. It may call the interface's other methods,
as `describe` calls `this.area()`. It may not read a field, because it does not know which type it is in.

::: tip Rule
An interface method with a body is a default: an `impl` may leave it out. `Self` in an interface is the
implementing type. A method without `this` is a static function, called as `T.name(…)`.
:::

## The interfaces of the library

The language and its library define a few interfaces that the compiler or the collections look for.

| Interface | What a type gets from it | How a type gets it |
|---|---|---|
| `Copy` | Its values are copied, not moved. As a bound, `T: Copy`. | Automatically: numbers, `bool`, views, shared borrows, and structs and enums whose parts are all Copy. There is no `impl Copy`. |
| `Iterator<T>` | It gives its values one at a time, and meets a bound `I: Iterator<T>`. | `impl Iterator<T> for …` with `fn next(borrow mut this) -> T?`. |
| `OrderKey<K>` | An order, `a.compareKey(b)`: negative, zero or positive. It can be a key of an `OrderedMap`. | `@derive(OrderKey)` on the type, or an `impl`. Integers, `bool` and `String` have it. |
| `HashKey<K>` | It can be a key of a `HashMap`. | `@derive(HashKey)` on the type, or an `impl`. Integers, `bool` and `String` have it. |
| `DerivedEqual<T>` | `==` and `!=` on a struct or enum. | Only `@derive(Equal)` on the type. |

`@derive(…)` before a struct or enum asks the compiler to write the `impl`, field by field. `OrderKey<K>` and
`HashKey<K>` take the type itself as their argument: the bound for "a type with an order" is `K: OrderKey<K>`.
A `for` loop accepts any value with a `next` method of the right shape; the `impl Iterator<T>` is what lets the
type meet a bound.

<span class="listing-caption"><strong>Listing 10-11</strong>, <code>main.cee</code>: A type implements the library's <code>Iterator</code>, another derives <code>OrderKey</code>, and generic functions use both through bounds.</span>

```ceesafe
struct Countdown {
    left: u32;
}

impl Iterator<u32> for Countdown {
    fn next(borrow mut this) -> u32? {
        if (this.left == 0) {
            return null;
        }
        this.left -= 1;
        return this.left + 1;
    }
}

@derive(OrderKey)
struct Version {
    major: u32;
    minor: u32;
}

fn sum<I: Iterator<u32>>(source: borrow mut I) -> u32 {
    var total: u32 = 0;
    for (n in source) {
        total += n;
    }
    return total;
}

fn smallest<K: OrderKey<K> + Copy>(keys: Slice<K>) -> K {
    var best = keys[0];
    for (k in keys) {
        if (k.compareKey(best) < 0) {
            best = k;
        }
    }
    return best;
}

fn main() {
    for (n in Countdown { left: 3 }) {
        Console.writeLine(n);
    }
    var four = Countdown { left: 4 };
    Console.writeLine(sum(mut four));

    let numbers = [5, 2, 9];
    Console.writeLine(smallest(numbers[..]));
    let versions = [Version { major: 1, minor: 4 }, Version { major: 1, minor: 2 }];
    Console.writeLine(smallest(versions[..]).minor);
}
```

```text
3
2
1
10
2
2
```

`Countdown` is declared in this program and `Iterator` in the library. That is allowed, and so is the opposite, an
interface of yours implemented for a type of the library: `impl Shape for u64 { … }`.

::: tip Rule: the orphan rule
An `impl I for T` is written in the package that declares `I` or in the package that declares `T`. So two packages
can never both implement the same interface for the same type.

*Specification: `type-system.md` §21.1.*
:::

To give a method to a type of another package through an interface of another package, wrap the type in a struct of
your own and implement the interface for the struct.

## Mistakes

::: warning Mistake: an `impl` that leaves out a method
Every method without a default must be written.
:::

<span class="listing-caption"><strong>Listing 10-12</strong>, <code>main.cee</code>: The <code>impl</code> has no <code>name</code>. This does not compile.</span>

<!-- ceesafe: fail N0267 -->
```ceesafe
interface Shape {
    fn area(borrow this) -> u64;
    fn name(borrow this) -> StringView;
}

struct Square {
    side: u64;
}

impl Shape for Square {
    fn area(borrow this) -> u64 => this.side * this.side;
}

fn main() {
    let s = Square { side: 2 };
    Console.writeLine(s.area());
}
```

```text
main.cee:10:6: error[N0267]: 'impl Shape for Square' is missing method 'name'
   |
10 | impl Shape for Square {
   |      ^^^^^
```

Write the method in the `impl`, or give it a default in the interface.

::: warning Mistake: a type that does not meet the bound
Having a method of the right name is not enough. The type needs an `impl` of the interface.
:::

<span class="listing-caption"><strong>Listing 10-13</strong>, <code>main.cee</code>: <code>Circle</code> has an <code>area</code> method and no <code>impl Shape</code>. This does not compile.</span>

<!-- ceesafe: fail N0268 -->
```ceesafe
interface Shape {
    fn area(borrow this) -> u64;
}

struct Circle {
    radius: u64;

    fn area(borrow this) -> u64 => 3 * this.radius * this.radius;
}

fn show<S: Shape>(shape: borrow S) {
    Console.writeLine(shape.area());
}

fn main() {
    show(Circle { radius: 1 });
}
```

```text
main.cee:16:5: error[N0268]: 'Circle' does not implement 'Shape', which 'S' requires
   |
16 |     show(Circle { radius: 1 });
   |     ^^^^^^^^^^^^^^^^^^^^^^^^^^
```

Move the method into an `impl Shape for Circle { … }`. The same code is reported for a bound of `Copy` that a
type does not meet, for example a `Vector<u8>` passed where `T: Copy` is wanted.

::: warning Mistake: an `impl` where neither the interface nor the type is yours
The orphan rule refuses it.
:::

<span class="listing-caption"><strong>Listing 10-14</strong>, <code>main.cee</code>: <code>Iterator</code> and <code>Vector</code> both belong to the library. This does not compile.</span>

<!-- ceesafe: fail N0267 -->
```ceesafe
impl Iterator<u8> for Vector<u8> {
    fn next(borrow mut this) -> u8? => this.pop();
}

fn main() {
    let bytes = Vector<u8>.create();
    Console.writeLine(bytes.length);
}
```

```text
main.cee:1:1: error[N0267]: 'impl Iterator<u8> for Vector<u8>' must be written in the package that declares 'Iterator<u8>' or 'Vector<u8>' (the orphan rule, ADR 0054)
  |
1 | impl Iterator<u8> for Vector<u8> {
  | ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
```

Declare `struct Bytes { items: Vector<u8>; }` and write `impl Iterator<u8> for Bytes`.

::: details Coming from Rust
An interface is a trait: `impl I for T`, defaults, `Self`, static functions, bounds with `+`, and the orphan rule
are as in Rust. What differs:

- `Copy` is never implemented or derived: a struct whose fields are all Copy, and that has no `drop`, is Copy;
- there is no `PartialEq`, `Ord` or `Hash` to implement, and no operator traits. `==` comes only from
  `@derive(Equal)`; order and hashing are `OrderKey<K>` and `HashKey<K>`, which take the type as an argument;
- an iterator's item is a parameter of the interface, `Iterator<u32>`, not an associated type;
- an interface method is also an ordinary method of the type, so a type cannot have a method and an interface
  method of one name.
:::

::: details Coming from C#, Java, TypeScript or Go
- A type does not list its interfaces where it is declared. Each `impl I for T` is a separate block, and it can be
  written after the type exists, for a type you did not write.
- A bound is not met by shape: a type with the right methods and no `impl`, which is enough in Go or TypeScript,
  does not meet it.
- A parameter of type `S` with `S: Shape` is not a reference to "some shape" found at run time. Each call is
  compiled for the one type it is given.
:::

## Summary

- `interface I { … }` lists methods; `impl I for T { … }` implements them for a type (`N0267` if one is missing).
- A bound `T: I` lets generic code call the methods of `I` on a `T`, with no cost at run time. A type without the
  `impl` does not meet the bound (`N0268`).
- A method with a body is a default. `Self` is the implementing type. A method without `this` is a static
  function, `T.name(…)`.
- The library's interfaces are `Copy`, `Iterator<T>`, `OrderKey<K>`, `HashKey<K>` and the equality that
  `@derive(Equal)` gives.
- An `impl` lives in the package of its interface or of its type (`N0267`).
- Where an interface's method returns a type parameter, as `fn get(borrow this) -> T` does, an `impl` may return
  what the value holds, and it may not lend the value's own storage: `this.text.view()` as that result is `N0245`.
  Generic code is checked against the interface, which does not say that the result borrows from the value.
- A bound `T: Send` says that a `T` holds no borrow, so generic code may return such a `T` from a closure or keep
  it after the value it came from is gone. [Send and Share](./ch16-04-send-and-share) covers `Send`.

Next: [Regions](./ch10-03-regions), for structs that hold a borrow.
