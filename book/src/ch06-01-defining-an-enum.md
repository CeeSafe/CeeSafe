---
title: Defining an enum
summary: An enum is a value that is one of several variants - variants with and without data, making and passing them, comparing with ==, methods and static functions in an impl block, and choosing between an enum and a struct.
assumes: [ch05-01-defining-structs, ch05-03-methods]
keywords: [enum, variant, payload, "==", impl, method, static function, this, struct]
diagnostics: [N0100, N0101, N0200, N0207]
specification: ["type-system.md §15.1", "type-system.md §21.1"]
---

# Defining an enum

**This section teaches** how to declare an enum, make its values, pass them to functions and compare them, how to
give it methods, and when an enum is the right type. It assumes you can define a struct ([Defining structs](./ch05-01-defining-structs))
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

An enum has methods and static functions, as a struct has. A struct's body can hold its methods. An enum's body
holds variants only, so an enum's functions are written in an `impl` block: the word `impl`, the enum's name, and
the functions in braces. It is the block that [Methods](./ch05-03-methods) shows for a struct.

<span class="listing-caption"><strong>Listing 6-4</strong>, <code>main.cee</code>: An <code>impl</code> block gives <code>Shape</code> a static function and a method.</span>

```ceesafe
enum Shape {
    Circle(i64),
    Rect(i64, i64),
}

impl Shape {
    fn square(side: i64) -> Shape {
        return Shape.Rect(side, side);
    }

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
    Console.writeLine(Shape.square(4).area());
}
```

```text
10
3
16
```

- `area` has the receiver `borrow this`, so it is a method, called on a value: `door.area()`. Inside it, `this` is
  the shape, borrowed for reading, and `match (this)` asks which variant it is.
- `square` has no receiver, so it is a static function, called on the type: `Shape.square(4)`.
- The receivers are a struct's: `borrow this` to read, `borrow mut this` to change, `this` to take the value.

A function that takes the enum, like `area(s)` in Listing 6-2, still works. Write a method when the operation
belongs to the type, and `shape.area()` reads better at the call.

## A method that changes the value

A struct's `borrow mut this` method changes fields. An enum has no fields: to go from one variant to another, the
method assigns a whole new value to `this`.

<span class="listing-caption"><strong>Listing 6-5</strong>, <code>main.cee</code>: <code>advance</code> replaces the light with its next variant.</span>

```ceesafe
enum Light {
    Red,
    Green,
    Amber,
}

impl Light {
    fn name(borrow this) -> StringView {
        match (this) {
            Light.Red => { return "red"; }
            Light.Green => { return "green"; }
            Light.Amber => { return "amber"; }
        }
    }

    fn advance(borrow mut this) {
        match (this) {
            Light.Red => { this = Light.Green; }
            Light.Green => { this = Light.Amber; }
            Light.Amber => { this = Light.Red; }
        }
    }
}

fn main() {
    var light = Light.Red;
    Console.writeLine(light.name());
    light.advance();
    Console.writeLine(light.name());
    light.advance();
    light.advance();
    Console.writeLine(light.name());
}
```

```text
red
green
red
```

`this = Light.Green;` replaces the value that the method was called on, so after `light.advance()` the variable
`light` in `main` holds the new variant. The assignment needs the receiver `borrow mut this`, and the variable must
be a `var`. `StringView` is the type of a string literal.

::: tip Rule
An enum's methods and static functions are written in `impl Name { … }`, never in the enum's body. In a
`borrow mut this` method, `this = value;` replaces the value the method was called on.

*Specification: `type-system.md` §21.1.*
:::

## Mistakes

::: warning Mistake: a variant without its enum's name
A variant is not a name on its own. The compiler looks for a variable or a function called `North` and finds none.
:::

<span class="listing-caption"><strong>Listing 6-6</strong>, <code>main.cee</code>: <code>North</code> is written without <code>Direction</code>. This does not compile.</span>

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

<span class="listing-caption"><strong>Listing 6-7</strong>, <code>main.cee</code>: <code>Shape</code> has a variant with a payload, so <code>==</code> is refused. This does not compile.</span>

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

::: warning Mistake: a function inside the enum's body
A struct's body can hold methods. An enum's body is a list of variants, and a `fn` in it is refused.
:::

<span class="listing-caption"><strong>Listing 6-8</strong>, <code>main.cee</code>: <code>isRound</code> is written among the variants. This does not compile.</span>

<!-- ceesafe: fail N0100 N0101 -->
```ceesafe
enum Shape {
    Circle(i64),
    Rect(i64, i64),

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
main.cee:5:5: error[N0100]: an enum's body holds its variants; write its functions in an 'impl' block: 'impl Name { fn … }' (type-system.md §21.1)
  |
5 |     fn isRound(borrow this) -> bool {
  |     ^^

main.cee:11:1: error[N0101]: expected 'fn', 'const', 'struct' or 'enum', found '}'
   |
11 | }
   | ^
```

The first message says what to do: close the enum after `Rect(i64, i64),` and move `isRound` into
`impl Shape { … }`, as Listing 6-4 does. The second message follows from the first. The compiler ends the enum at
the `fn`, so the enum's own `}` is left over. It goes away with the fix.

::: details Coming from Rust
The declaration is the same, with two differences. A variant is `Shape.Circle(2)`, with a dot, not
`Shape::Circle(2)`, and there is no `use Shape::*` to drop the prefix. Payloads are positional only: there is no
`Rect { w: i64, h: i64 }` variant. Methods are in `impl Shape { … }`, as in Rust, and where Rust writes
`*self = …` in a `&mut self` method, CeeSafe writes `this = …`.
:::

::: details Coming from C, C# or Java
A CeeSafe enum is not a set of named integers. A variant has no number you can read or do arithmetic with, and a
variant can carry data, which makes an enum closer to a tagged union in C or a sealed class hierarchy in C# and
Java. A Java enum's methods are written in its body, after the constants; a CeeSafe enum's are in an `impl` block.
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
- An enum's methods and static functions are written in `impl Name { … }`, not in its body (`N0100`).
- In a `borrow mut this` method, `this = value;` replaces the value.
- A struct is all of its fields at once; an enum is one of its variants.

Next: [`match`](./ch06-02-match), which takes an enum value apart.
