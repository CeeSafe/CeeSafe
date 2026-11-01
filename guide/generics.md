# Generics and interfaces

## Generic functions and types

Functions, structs and enums can take type parameters. Each use with different types compiles to its own copy
(monomorphisation), so generic code is as fast as hand-written code for each type.

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

fn firstOf<T>(pair: Pair<T>) -> T => pair.first;

fn main() {
    let p = Pair { first: 1, second: 2 };           // Pair<i32>, inferred
    let q = p.swapped();
    Console.writeLine(firstOf(q));                  // T = i32, inferred
    let names = Pair<StringView> { first: "x", second: "y" };
    Console.writeLine(firstOf<StringView>(names));  // written out
    let m: Maybe<u8> = Maybe.Just(7);
    match (m) {
        Maybe.Just(v) => { Console.writeLine(v); }
        Maybe.Nothing => {}
    }
}
```

```text
2
x
7
```

Type arguments are inferred from the arguments where possible, and written otherwise: `Pair<u8> { … }`,
`Vector<i32>.create()`, `firstOf<StringView>(names)`.

**Generic code is checked once,** for every possible type argument. Inside it, a value of type `T` can be moved,
borrowed, stored and returned, but nothing else is known about it: no arithmetic, no fields, no `==`. What it can do
comes from its **bounds**.

## Interfaces

An interface lists methods. A type implements it with an `impl` block, and a generic function bound by it can call
those methods.

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

fn total<S: Shape>(shapes: Slice<S>) -> u64 {
    var sum: u64 = 0;
    for (shape in shapes) {
        sum += shape.area();
    }
    return sum;
}

fn describe<S: Shape>(shape: borrow S) {
    Console.write(shape.name());
    Console.write(": ");
    Console.writeLine(shape.area());
}

fn main() {
    let squares = [Square { side: 2 }, Square { side: 3 }];
    var rects = Vector<Rect>.create();
    rects.push(Rect { w: 4, h: 5 })!;
    Console.writeLine(total(squares[..]) + total(rects.view()));
    describe(squares[1]);
}
```

```text
33
square: 9
```

- Calls through a bound are resolved at compile time: there is no virtual dispatch and no cost.
- `Copy` is a built-in bound: a `T: Copy` can be used twice and stored in arrays.
- Several bounds are joined with `+`: `<T: Shape + Copy>`.
- An interface can itself be generic: `interface Comparable<T> { fn compare(borrow this, other: borrow T) -> i32; }`.

## Dynamic dispatch

A bound is resolved at compile time, so a `Slice<S>` holds shapes of one type. For a collection of different types
that implement the same interface, use `dyn Shape`: a value of some type implementing `Shape`, whose methods are
called through a table at run time. Its size is not known, so it lives behind a pointer: `borrow dyn Shape`,
`borrow mut dyn Shape`, or an owning `Box<dyn Shape>`.

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

fn describe(shape: borrow dyn Shape) {
    Console.write(shape.name());
    Console.write(": ");
    Console.writeLine(shape.area());
}

fn main() {
    var shapes = Vector<Box<dyn Shape>>.create();
    shapes.push(Box.create(Square { side: 3 })!)!; // a Box<Square> becomes a Box<dyn Shape>
    shapes.push(Box.create(Rect { w: 2, h: 5 })!)!;
    for (shape in shapes) {
        describe(shape);
    }
    let sq = Square { side: 4 };
    describe(sq); // a Square, lent as a borrow dyn Shape
}
```

```text
square: 9
rect: 10
square: 16
```

- A `borrow T` or a `Box<T>` converts to the `dyn` type where one is expected, if `T` implements the interface.
  There is no way back to `T`.
- Every method of the interface must take `borrow this` or `borrow mut this`. Async methods are allowed:
  `await shape.load()` through `dyn` waits for the value's own method, in room the compiler makes for the largest
  of the types converted to that `dyn` type.
- A call through `dyn` cannot be inlined. Prefer a bound when one type is enough.
- An interface method may declare its effects, `@effects() fn visit(borrow mut this, x: u64);`, which every `impl`
  must keep: kernel code in an atomic context can then call it through `dyn`.

## Next

[Functions and closures](closures.md).
