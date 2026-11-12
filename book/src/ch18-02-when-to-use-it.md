---
title: When to use it
summary: Three ways to write code for several kinds of value - a generic with a bound, an enum with match, and dyn - with what each costs when a kind or an operation is added, what a call costs measured, and rules for choosing.
assumes: [ch18-01-dynamic-dispatch, ch10-02-interfaces, ch06-02-match]
keywords: [dyn, generic, bound, enum, match, static dispatch, dynamic dispatch, trade-off, cost, closed set, open set]
diagnostics: [N0205, N0256, N0267]
specification: ["type-system.md §37", "type-system.md §21.2"]
---

# When to use it

**This section teaches** how to choose between three ways to write code that works on several kinds of value: a
generic with a bound, an enum with `match`, and `dyn`. It assumes [Dynamic dispatch](./ch18-01-dynamic-dispatch),
[Interfaces](./ch10-02-interfaces) and [match](./ch06-02-match).

The example is the same each time: some squares and rectangles, and the sum of their areas.
[Listing 18-1](./ch18-01-dynamic-dispatch#a-vector-of-different-types) is the `dyn` way. Here are the other two.

## An enum: the kinds are listed

<span class="listing-caption"><strong>Listing 18-11</strong>, <code>main.cee</code>: The kinds of shape are the variants of one enum, and <code>area</code> is one function with a <code>match</code>.</span>

```ceesafe
enum Shape {
    Square(u64),
    Rect(u64, u64),
}

impl Shape {
    fn area(borrow this) -> u64 => match (this) {
        Shape.Square(side) => side * side,
        Shape.Rect(w, h) => w * h,
    };
}

fn main() {
    var shapes = Vector<Shape>.create();
    shapes.push(Shape.Square(3))!;
    shapes.push(Shape.Rect(4, 5))!;
    var total: u64 = 0;
    for (shape in shapes) {
        total += shape.area();
    }
    Console.writeLine(total);
}
```

```text
29
```

An enum is one type, so a `Vector<Shape>` holds squares and rectangles side by side, in the vector's own memory,
with no box. Any code can `match` on a shape and reach its data. The price is that the set is **closed**: every
kind is a variant written in this one declaration.

## A generic: the type is known

<span class="listing-caption"><strong>Listing 18-12</strong>, <code>main.cee</code>: <code>total</code> is generic, and each vector holds one type.</span>

```ceesafe
interface Shape {
    fn area(borrow this) -> u64;
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
}

impl Shape for Rect {
    fn area(borrow this) -> u64 => this.w * this.h;
}

fn total<S: Shape>(shapes: borrow Vector<S>) -> u64 {
    var sum: u64 = 0;
    for (shape in shapes) {
        sum += shape.area();
    }
    return sum;
}

fn main() {
    var squares = Vector<Square>.create();
    squares.push(Square { side: 3 })!;
    var rects = Vector<Rect>.create();
    rects.push(Rect { w: 4, h: 5 })!;
    Console.writeLine(total(squares) + total(rects));
}
```

```text
29
```

The compiler makes one `total` for `Square` and one for `Rect`. Each calls its `area` directly. The set of types
is **open**: any type with an `impl Shape` works. But the two kinds cannot share a vector, and the caller must
know, where the code is written, which type each value has.

## What a change costs

Programs grow in two directions: a new kind of value, and a new operation on all of them. The three ways differ
most here.

Add a kind to the enum, and every `match` that does not cover it stops compiling.

<span class="listing-caption"><strong>Listing 18-13</strong>, <code>main.cee</code>: A <code>Circle</code> variant is added and <code>area</code> is not changed. This does not compile.</span>

<!-- ceesafe: fail N0256 -->
```ceesafe
enum Shape {
    Square(u64),
    Rect(u64, u64),
    Circle(u64),
}

impl Shape {
    fn area(borrow this) -> u64 => match (this) {
        Shape.Square(side) => side * side,
        Shape.Rect(w, h) => w * h,
    };
}

fn main() {
    Console.writeLine(Shape.Circle(1).area());
}
```

```text
main.cee:8:43: error[N0256]: this match does not cover Shape.Circle; add the missing arms or a '_' arm
  |
8 |     fn area(borrow this) -> u64 => match (this) {
  |                                           ^^^^
```

That is work, and it is also a list of the places to change, made by the compiler. A new operation on an enum is
the cheap direction: one new function with one `match`, and nothing else is touched.

An interface is the other way round, whether it is used through a bound or through `dyn`. A new kind is a new
type and its `impl`, in any file, and no existing code changes. A new operation is a new method, and every `impl`
must gain it.

<span class="listing-caption"><strong>Listing 18-14</strong>, <code>main.cee</code>: <code>edges</code> is added to the interface and only <code>Square</code> gets it. This does not compile.</span>

<!-- ceesafe: fail N0267 -->
```ceesafe
interface Shape {
    fn area(borrow this) -> u64;
    fn edges(borrow this) -> u64;
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
    fn edges(borrow this) -> u64 => 4;
}

impl Shape for Rect {
    fn area(borrow this) -> u64 => this.w * this.h;
}

fn main() {
    let shape: Box<dyn Shape> = Box.create(Rect { w: 4, h: 5 })!;
    Console.writeLine(shape.edges());
}
```

```text
main.cee:20:6: error[N0267]: 'impl Shape for Rect' is missing method 'edges'
   |
20 | impl Shape for Rect {
   |      ^^^^^
```

A default body for the new method ([Interfaces](./ch10-02-interfaces)) keeps the old `impl`s compiling, when a
default makes sense.

| | Generic with a bound | Enum and `match` | `dyn` |
|---|---|---|---|
| Kinds in one collection | one | the variants | any type with the `impl` |
| A new kind | a type and an `impl`, anywhere | a variant, and every `match` | a type and an `impl`, anywhere |
| A new operation | a method in every `impl` | one function | a method in every `impl` |
| Reaching a kind's own data | yes, the type is known | yes, with `match` | no, only the interface's methods |
| Where a value lives | in place | in place | in a box, or behind a borrow |
| A call | direct | a branch on the variant | through the table |
| Limits on the interface | none | no interface needed | must be dyn-compatible |

## What a call costs

The three programs above were made larger and timed: 1,000 shapes, 506 squares and 494 rectangles in an order
that a small random number generator chooses, and the areas summed 200,000 times. That is 200 million calls of
`area`. They are the packages `book/projects/ch18/cost-generic`, `cost-enum` and `cost-dyn`, built with
`ceesafec build` and run on an Apple M1 with Apple clang 21. Each time is the best of several runs.

| Way | Time | For each call |
|---|---:|---:|
| generic: a `Vector<Square>` and a `Vector<Rect>` | 110 ms | 0.6 ns |
| enum: a `Vector<Shape>` | 240 ms | 1.2 ns |
| `dyn`: a `Vector<Box<dyn Shape>>` | 930 ms | 4.7 ns |

- In the generic program each copy of `total` calls its type's `area` by name, so the C compiler can put the
  multiplication in the loop. The loop walks one array of plain numbers.
- In the enum program the call is direct too, and `area` tests the variant before it multiplies.
- In the `dyn` program each pass reads a pair from the vector, follows one pointer to the table and one to the
  shape, which is its own block of heap memory, and makes a call that cannot be inlined.

`area` is one multiplication, so this is the worst case for `dyn`: the body is far smaller than the call. Four
nanoseconds do not show beside a method that formats text, reads a file or walks a list. They show in the
innermost loop of a program that computes.

## Choosing

::: tip Rule: one type at a time - a generic
If each use of the code works on one type, and that type is known where the code is called, write a generic with
a bound. It is the fastest, it allocates nothing, and it keeps the value's type.
:::

::: tip Rule: several kinds that you can list - an enum
If values of several kinds must sit together, and all the kinds are known when the package is written, write an
enum and `match` on it. Choose it also when code must tell the kinds apart and use their data, or when
operations are added more often than kinds.
:::

::: tip Rule: several kinds that you cannot list - `dyn`
If values of several kinds must sit together, and code that the holder has never seen adds kinds (another file's
feature, another package, a table of handlers filled at run time), write an interface and hold `Box<dyn I>`.
:::

Three more points settle most of the remaining cases:

- **A parameter can be either.** `fn show(shape: borrow dyn Shape)` and `fn show<S: Shape>(shape: borrow S)` both
  take a borrow of any shape. The generic is faster in a hot loop. The `dyn` is compiled once, which keeps a large
  function from being copied for each type. A generic can still be called with a `dyn` value, so start with the
  generic if unsure.
- **The ways combine.** A `dyn` value can hold an enum, an enum variant can hold a `Box<dyn I>`, and a generic
  struct can hold either.
- **For one action, a closure.** A `Box<dyn fn(…) -> R>` is a `dyn` with one method and no interface to declare.

## Mistakes

::: warning Mistake: two types in a generic collection
A bound says what a type can do. It does not make two types one type.
:::

<span class="listing-caption"><strong>Listing 18-15</strong>, <code>main.cee</code>: A <code>Rect</code> is pushed on a <code>Vector&lt;Square&gt;</code>. This does not compile.</span>

<!-- ceesafe: fail N0205 -->
```ceesafe
interface Shape {
    fn area(borrow this) -> u64;
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
}

impl Shape for Rect {
    fn area(borrow this) -> u64 => this.w * this.h;
}

fn main() {
    var shapes = Vector<Square>.create();
    shapes.push(Square { side: 3 })!;
    shapes.push(Rect { w: 4, h: 5 })!;
    Console.writeLine(shapes.length);
}
```

```text
main.cee:25:17: error[N0205]: expected 'Square', found 'Rect'
   |
25 |     shapes.push(Rect { w: 4, h: 5 })!;
   |                 ^^^^^^^^^^^^^^^^^^^
```

Make the element an enum of the two, or a `Box<dyn Shape>`.

::: warning Mistake: `dyn` for a set that is closed
An interface, three structs and a vector of boxes, where the three kinds are all there will ever be, is an enum
written the long way. It allocates for each value, and the code that needs a kind's data cannot reach it. Write
the enum.
:::

::: warning Mistake: an enum that other code must extend
A new variant needs the enum's declaration, so only the enum's own package can add a kind. If users of a library
are meant to bring their own kinds, an enum shuts them out: give them an interface.
:::

::: details Coming from Rust
The choice is Rust's: generics for static dispatch, `enum` for a closed set, `dyn Trait` for an open one. Two
differences shift it a little. There is no `impl Trait` in a parameter or a result: write the type parameter, or
`borrow dyn I`, or return a `Box<dyn I>`. And there is no downcast, so a design that leans on `Any` becomes an
enum here.
:::

::: details Coming from C#, Java, TypeScript or Go
There, an interface type is always dynamic, and it is the tool for every one of these jobs. Here it is the third
choice. Reach first for a generic or an enum, and for `dyn` when the kinds are open and held together. A class
hierarchy with a fixed set of subclasses and `is` tests is an enum with `match`.
:::

## Summary

- A generic with a bound: one type for each use, known when compiled; direct calls; nothing allocated.
- An enum with `match`: several kinds in one type, all listed in one place; values in place; a new operation is
  one function, and a new kind changes every `match` (N0256 finds them).
- `dyn`: several kinds in one type, open to kinds added anywhere; a box or a borrow; only the interface's
  methods; a new operation changes every `impl` (N0267 finds them).
- Mixing types in a `Vector<S>` is N0205.
- Measured on the smallest possible method, a call through `dyn` took about 5 ns, against about 1 ns for an enum
  and less for a generic.

Next: [A design worked through](./ch18-03-a-design-worked-through), which takes one small program through all
three.
