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

## Default methods and `Self`

An interface method can have a body: its **default**. An impl that leaves the method out inherits the default; one
that writes it overrides it. Inside an interface, `Self` is the implementing type.

```ceesafe
interface Watcher {
    fn onError(borrow mut this, code: u32);

    fn onWatchError(borrow mut this, code: u32) {
        this.onError(code + 100);
    }

    fn label(borrow this) -> StringView => "watcher";

    fn copyOf(borrow this) -> Self;
}

struct Log {
    total: u32;
}

impl Watcher for Log {
    fn onError(borrow mut this, code: u32) {
        this.total += code;
    }

    fn label(borrow this) -> StringView => "log";

    fn copyOf(borrow this) -> Self => Self { total: this.total };
}

fn report<W: Watcher>(w: borrow mut W) {
    w.onWatchError(1);
    Console.writeLine(w.label());
}

fn main() {
    var log = Log { total: 0 };
    log.onWatchError(5);
    report(mut log);
    Console.writeLine(log.copyOf().total);
}
```

```text
log
206
```

- A default is checked once, where the interface is, with `Self` unknown: it can call the interface's methods but
  not use fields. An error in it is reported there, even if no impl inherits it.
- An impl that inherits a default gets its own copy, compiled for the target type: a call on the type, through a
  bound or through `dyn` calls it directly or through the table, as for a method the impl writes.
- A type has one method of each name: an inherited default whose name the type already has is an error at the impl.
- In an `impl` block and in a struct's or enum's body, `Self` is the type itself, with its parameters:
  `Self { … }`, `Self.make()` and `Self.MAX` work as with the type's name.

## Generic impls

An impl can have type parameters of its own. A wrapper then implements an interface for every inner type that has it:

```ceesafe
interface Source {
    fn next(borrow mut this) -> u32;
}

struct Counter {
    value: u32;
}

impl Source for Counter {
    fn next(borrow mut this) -> u32 {
        this.value += 1;
        return this.value;
    }
}

/// Any source, with the calls of `next` counted.
struct Counted<T> {
    inner: T;
    calls: u32;
}

impl<T: Source> Source for Counted<T> {
    fn next(borrow mut this) -> u32 {
        this.calls += 1;
        return this.inner.next();
    }
}

fn take<S: Source>(s: borrow mut S) -> u32 => s.next() + s.next();

fn main() {
    var counted = Counted { inner: Counted { inner: Counter { value: 0 }, calls: 0 }, calls: 0 };
    Console.writeLine(take(mut counted));
    Console.writeLine(counted.calls + counted.inner.calls);
}
```

```text
3
4
```

- The impl is checked once, then compiled for each instance it applies to: `Counted<Counter>` and
  `Counted<Counted<Counter>>` here. An instance whose argument does not meet the bound (`Counted<u8>`) does not
  implement `Source`.
- Each of the impl's type parameters must appear in its target, and the target is a generic struct or enum.
- Beside a generic impl, no other impl of the same interface may be written for the same struct or enum: no
  `impl Source for Counted<Counter>` as well.
- An impl lives in the package that declares the interface or the type (the orphan rule), so two packages never
  both provide one.
- A struct with region parameters implements an interface with its regions named, `impl<region r> Body for
  RequestBody<r> { … }`, and is used through bounds; it does not convert to `dyn Body`, since a `dyn` value holds no
  loans.

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
