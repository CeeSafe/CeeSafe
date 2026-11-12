---
title: Defining an enum
summary: An enum is a value that is one of several variants - variants with and without data, making and passing them, comparing with ==, methods through an interface, and choosing between an enum and a struct.
assumes: [ch05-01-defining-structs, ch05-03-methods]
keywords: [enum, variant, payload, "==", interface, impl, struct]
diagnostics: [N0200, N0207, N0234, N0267]
specification: ["type-system.md §15.1", "type-system.md §21.1"]
---

# Defining an enum

**This section teaches** how to declare an enum, make its values, pass them to functions and compare them, and
when an enum is the right type. It assumes you can define a struct ([Defining structs](./ch05-01-defining-structs))
and write a method ([Methods](./ch05-03-methods)).

An **enum** is a type whose value is exactly one of a fixed list of **variants**. Where a struct says "an `x` and a
`y`", an enum says "north or east or south or west".

## Variants without data

<span class="listing-caption"><strong>Listing 6-1</strong>, <code>main.cee</code>: An enum of four variants, made, passed to a function and compared.</span>

```ceesafe
enum Direction {
    North,
    East,
    South,
    West,
}

fn isVertical(d: Direction) -> bool {
    return d == Direction.North || d == Direction.South;
}

fn main() {
    let heading = Direction.East;
    Console.writeLine(isVertical(heading));
    Console.writeLine(isVertical(Direction.South));
    Console.writeLine(heading != Direction.West);
}
```

```text
false
true
true
```

- `enum Direction { … }` declares the type. The variants are separated by commas.
- A value is written with the enum's name, a dot and the variant: `Direction.East`.
- `Direction` is a type like any other: `d: Direction` is a parameter, and a field or a return type can be one too.
- `==` and `!=` compare two values of an enum whose variants carry no data.

::: tip Rule
A variant is always written with its enum's name: `Direction.North`, never `North`.

*Specification: `type-system.md` §15.1.*
:::

## Variants that carry data

A variant can carry values. Write their types in parentheses after the variant's name. These values are the
variant's **payload**.

<span class="listing-caption"><strong>Listing 6-2</strong>, <code>main.cee</code>: Each variant of <code>Shape</code> carries what that shape needs.</span>

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

fn main() {
    let dot = Shape.Empty;
    let wheel = Shape.Circle(2);
    Console.writeLine(area(dot));
    Console.writeLine(area(wheel));
    Console.writeLine(area(Shape.Rect(3, 4)));
}
```

```text
0
12
12
```

- `Circle(i64)` carries one `i64`, `Rect(i64, i64)` carries two, and `Empty` carries nothing.
- `Shape.Circle(2)` makes a circle. It looks like a call, and it takes exactly as many values as the variant
  declares.
- `dot`, `wheel` and `Shape.Rect(3, 4)` all have the one type `Shape`, so one function takes any of them.
- The payload has positions, not names. The only way to read it is `match`, which the function `area` uses:
  `Shape.Circle(r)` means "if `s` is a circle, call its payload `r`". [`match`](./ch06-02-match) is the next
  section.

::: tip Rule
An enum value holds one variant and that variant's payload, and nothing of the other variants. To read a payload,
`match` on the value.
:::

A payload is not limited to numbers: it can be a struct or another enum.

## An enum inside a struct

Enums and structs are used together. A struct holds what every value has; an enum field holds the part that
differs from case to case.

<span class="listing-caption"><strong>Listing 6-3</strong>, <code>main.cee</code>: Every order has an id. Only a shipped order has a tracking number.</span>

```ceesafe
enum Status {
    Pending,
    Shipped(u32),
    Cancelled,
}

struct Order {
    id: u32;
    status: Status;
}

fn report(order: borrow Order) {
    Console.write("order ");
    Console.write(order.id);
    match (order.status) {
        Status.Pending => { Console.writeLine(" is waiting"); }
        Status.Shipped(tracking) => {
            Console.write(" was shipped, tracking number ");
            Console.writeLine(tracking);
        }
        Status.Cancelled => { Console.writeLine(" was cancelled"); }
    }
}

fn main() {
    var order = Order { id: 7, status: Status.Pending };
    report(order);
    order.status = Status.Shipped(40112);
    report(order);
}
```

```text
order 7 is waiting
order 7 was shipped, tracking number 40112
```

## An enum or a struct

Without the enum, `Order` would need a `shipped: bool` field and a `tracking: u32` field. Then a program could
make an order that is not shipped and has a tracking number, or read the tracking number of an order that has none.
With `Status`, the tracking number exists only inside `Shipped`, and the only way to reach it is the arm of a
`match` that has checked for `Shipped`.

- Use a **struct** when a value has all of its parts at once.
- Use an **enum** when a value is one of several cases, and most of all when the cases carry different data.
- When two fields of a struct are only meaningful for some values of a third, make that third field an enum and
  move the two into its variants.

## Methods

A struct's methods are written inside its body. An enum's body holds variants only, so the ordinary way to work
with an enum is a function that takes it, like `area` in Listing 6-2.

To call `shape.area()` with a dot, the enum implements an **interface**: a named list of methods that a type
promises to have. Chapter 10 teaches interfaces; this is the smallest use of one.

<span class="listing-caption"><strong>Listing 6-4</strong>, <code>main.cee</code>: <code>Shape</code> gets a method by implementing an interface.</span>

```ceesafe
enum Shape {
    Circle(i64),
    Rect(i64, i64),
}

interface Measure {
    fn area(borrow this) -> i64;
}

impl Measure for Shape {
    fn area(borrow this) -> i64 {
        match (this) {
            Shape.Circle(r) => { return 3 * r * r; }
            Shape.Rect(w, h) => { return w * h; }
        }
    }
}

fn main() {
    let door = Shape.Rect(2, 5);
    Console.writeLine(door.area());
    Console.writeLine(Shape.Circle(1).area());
}
```

```text
10
3
```

`interface Measure` says that a type with `Measure` has a method `area`. `impl Measure for Shape` writes that
method for `Shape`. Inside it, `this` is the shape, borrowed for reading.

::: tip Rule
An enum has no methods of its own. Write a function that takes the enum, or implement an interface for it.

*Specification: `type-system.md` §21.1.*
:::

## Mistakes

::: warning Mistake: a variant without its enum's name
A variant is not a name on its own. The compiler looks for a variable or a function called `North` and finds none.
:::

<span class="listing-caption"><strong>Listing 6-5</strong>, <code>main.cee</code>: <code>North</code> is written without <code>Direction</code>. This does not compile.</span>

<!-- ceesafe: fail N0200 -->
```ceesafe
enum Direction {
    North,
    South,
}

fn main() {
    let heading = North;
}
```

```text
main.cee:7:19: error[N0200]: cannot find 'North' in this scope
  |
7 |     let heading = North;
  |                   ^^^^^
```

Write `Direction.North`.

::: warning Mistake: `==` on an enum that carries data
`==` compares only enums whose variants all have no payload. If any variant carries data, `==` is not defined for
the type, even between two variants that carry nothing.
:::

<span class="listing-caption"><strong>Listing 6-6</strong>, <code>main.cee</code>: <code>Shape</code> has a variant with a payload, so <code>==</code> is refused. This does not compile.</span>

<!-- ceesafe: fail N0207 -->
```ceesafe
enum Shape {
    Empty,
    Circle(i64),
}

fn main() {
    let s = Shape.Circle(2);
    Console.writeLine(s == Shape.Empty);
}
```

```text
main.cee:8:23: error[N0207]: '==' compares enums whose variants have no payloads; 'Shape' has payloads: compare with 'match' (type-system.md §15.1)
  |
8 |     Console.writeLine(s == Shape.Empty);
  |                       ^^^^^^^^^^^^^^^^
```

Ask the question with `match`, as the message says. The next section also shows a shorter form, `s is Shape.Empty`.

::: warning Mistake: methods in an `impl` block for an enum
`impl Shape { … }` with functions in it works for a struct. For an enum it does not.
:::

<span class="listing-caption"><strong>Listing 6-7</strong>, <code>main.cee</code>: An <code>impl</code> without <code>for</code> cannot give an enum a method. This does not compile.</span>

<!-- ceesafe: fail N0234 N0267 -->
```ceesafe
enum Shape {
    Circle(i64),
    Rect(i64, i64),
}

impl Shape {
    fn isRound(borrow this) -> bool {
        match (this) {
            Shape.Circle(_) => { return true; }
            Shape.Rect(_, _) => { return false; }
        }
    }
}

fn main() {
    Console.writeLine(Shape.Circle(1).isRound());
}
```

```text
main.cee:6:6: error[N0267]: an 'impl' without 'for' of an enum holds constants only, not functions (ADR 0074 §2.7)
  |
6 | impl Shape {
  |      ^^^^^

main.cee:16:39: error[N0234]: type 'Shape' has no methods; 'isRound' cannot be called on it
   |
16 |     Console.writeLine(Shape.Circle(1).isRound());
   |                                       ^^^^^^^
```

Make it a function, `fn isRound(s: borrow Shape) -> bool`, called as `isRound(shape)`, or implement an interface
as Listing 6-4 does.

::: details Coming from Rust
The declaration is the same, with three differences. A variant is `Shape.Circle(2)`, with a dot, not
`Shape::Circle(2)`, and there is no `use Shape::*` to drop the prefix. Payloads are positional only: there is no
`Rect { w: i64, h: i64 }` variant. There is no `impl Shape { fn … }`: an enum gets methods from `impl Interface for
Shape`.
:::

::: details Coming from C, C# or Java
A CeeSafe enum is not a set of named integers. A variant has no number you can read or do arithmetic with, and a
variant can carry data, which makes an enum closer to a tagged union in C or a sealed class hierarchy in C# and
Java.
:::

::: details Coming from TypeScript
An enum with payloads does the work of a discriminated union, `{ kind: "circle", r: number } | { kind: "rect", … }`.
The variant is the `kind`, and `match` is the `switch (shape.kind)` that the compiler checks for completeness.
:::

## Summary

- `enum Name { A, B(T), C(T, U) }` declares a type whose value is one variant.
- A variant is written `Name.A` or `Name.B(value)`, always with the enum's name (`N0200` without it).
- A payload is read with `match`, not with a field name.
- `==` and `!=` work on enums whose variants carry no data (`N0207` otherwise).
- An enum has no methods of its own (`N0267`): write a function, or implement an interface.
- A struct is all of its fields at once; an enum is one of its variants.

Next: [`match`](./ch06-02-match), which takes an enum value apart.
