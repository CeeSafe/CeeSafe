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
- A method without a receiver is a static function: `fn create(options: borrow Options) -> Self;` is called as
  `T.create(o)` through a bound `T`, as `Self.create(o)` in a default and as `Rule.create(o)` on a type. A method may
  also have type parameters of its own, `fn visit<T: Copy>(borrow mut this, value: T)`, instantiated per call.

## Constants and associated types

An interface can declare constants, with or without a default, and associated types, which each impl chooses:

```ceesafe
interface Encoding {
    const NAME: StringView;
    const WIDTH: usize = 1;
    type Unit: Copy;

    fn decode(borrow this, bytes: Slice<u8>) -> Self.Unit?;
}

struct Latin1 {
    unused: u8;
}

impl Encoding for Latin1 {
    const NAME: StringView = "latin-1";
    type Unit = u8;

    fn decode(borrow this, bytes: Slice<u8>) -> u8? => if (bytes.length > 0) bytes[0] else null;
}

struct Ucs2 {
    unused: u8;
}

impl Encoding for Ucs2 {
    const NAME: StringView = "ucs-2";
    const WIDTH: usize = 2;
    type Unit = u16;

    fn decode(borrow this, bytes: Slice<u8>) -> u16? {
        if (bytes.length < 2) {
            return null;
        }
        return (bytes[0] as u16) * 256 + (bytes[1] as u16);
    }
}

fn first<E: Encoding>(e: borrow E, bytes: Slice<u8>) -> E.Unit? {
    Console.write(E.NAME);
    Console.write(" reads ");
    Console.write(E.WIDTH);
    Console.writeLine(" byte(s)");
    return e.decode(bytes);
}

fn main() {
    let bytes = [1u8, 2u8];
    Console.writeLine(first(Latin1 { unused: 0 }, bytes[..]) ?? 0);
    Console.writeLine(first(Ucs2 { unused: 0 }, bytes[..]) ?? 0);
    Console.writeLine(Ucs2.WIDTH);
}
```

```text
latin-1 reads 1 byte(s)
1
ucs-2 reads 2 byte(s)
258
2
```

- `E.NAME` and `E.Unit` name the constant and the type of whatever `E` is; each instance reads its impl's, so a test
  of a constant such as `if (H.HAS_ON_DATA)` costs nothing at run time.
- An associated type's bounds (`Copy` here) are checked at each impl, and are all that generic code knows of it.
- When callers need to fix a type (an iterator's item, say), make it an interface parameter instead:
  `Iterator<u8>` rather than an associated type.
- An interface with constants, associated types or static functions has no `dyn` type.

An interface can build on others, its **supertraits**: `interface Shape: Named { … }`. Every `impl Shape` needs an
`impl Named` for the same type, and a bound `S: Shape` gives `Named`'s methods too: inside a generic function over
`S: Shape`, `s.name()` works, and `s` can be passed where a `Named` is wanted. `dyn Shape` holds both interfaces'
methods. `Copy` can be a supertrait as well, making every bound by the interface a Copy one.

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
- A block of a struct's or enum's own functions takes bounds in the same way: `impl<T: Copy> Pair<T> { fn
  first(borrow this) -> T => this.left; }` gives `first` to the pairs of a Copy type, where reading the field is a
  copy, and to no other. `Pair<String>` does not have it (N0268 where it is called), and the other functions of
  `Pair<T>` cannot call it.

## Const parameters

A generic item may also take **values**: `const N: T`, with `T` a `bool`, an integer type or a fieldless enum. Each
distinct value compiles to its own copy, so a flag or a size known at compile time costs nothing at run time.

```ceesafe
enum Mode { Plain, Shout }

struct Buffer<const N: usize> {
    bytes: [u8; N];
    used: usize;
}

fn render<const UPPER: bool>(c: u8) -> u8 {
    if (UPPER && c >= 'a' && c <= 'z') {
        return c - 32;
    }
    return c;
}

fn greet<const M: Mode>() -> StringView => match (M) { Mode.Plain => "hello", Mode.Shout => "HELLO" };

fn sum<const N: usize>(values: [u32; N]) -> u32 {
    var total: u32 = 0;
    for (v in values) {
        total += v;
    }
    return total;
}

fn main() {
    let b = Buffer<16> { bytes: [0u8; 16], used: 0 };
    Console.writeLine(b.bytes.length);
    let shout = true;
    let c = if (shout) render<true>('q') else render<false>('q'); // the instance is chosen at run time
    Console.writeLine(c);
    Console.writeLine(greet<Mode.Shout>());
    Console.writeLine(sum([1u32, 2, 3]));                         // N = 3, inferred from the argument
}
```

```text
16
81
HELLO
6
```

- Inside the generic item, a const parameter is a value of its type: `if (UPPER)`, `match (M)`, `[u8; N]`. Both
  branches of `if (UPPER)` are checked, and each copy keeps only its own.
- An argument is `true`, `false`, an integer literal, a constant (`Buffer<SIZE>`), an enum variant
  (`greet<Mode.Shout>()`) or the caller's own const parameter. A run-time value is not one: choose the instance with
  an `if`, as above.
- An array length may also name a `usize` constant: `const SIZE: usize = 64;` then `[u8; SIZE]`.
- Expressions over a parameter (`[u8; N + 1]`, `render<!UPPER>`) are refused; name a constant, or add a parameter.

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
- Every method of the interface must take `borrow this` or `borrow mut this`, have no type or const parameters of
  its own and not name `Self` except as its receiver: a static function or a generic method has no place in a table.
  Defaults are fine. Async methods are allowed:
  `await shape.load()` through `dyn` waits for the value's own method, in room the compiler makes for the largest
  of the types converted to that `dyn` type.
- A call through `dyn` cannot be inlined. Prefer a bound when one type is enough.
- An interface method may declare its effects, `@effects() fn visit(borrow mut this, x: u64);`, which every `impl`
  must keep: kernel code in an atomic context can then call it through `dyn`.

## Next

[Functions and closures](closures.md).
