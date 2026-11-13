---
title: Dynamic dispatch
summary: The type dyn I is a value of some type that implements I, held in a Box or behind a borrow, with its methods found in a table while the program runs - how values convert to it, what can be called, which interfaces allow it, boxed closures, and Send.
assumes: [ch10-02-interfaces, ch15-01-box, ch13-01-closures]
keywords: [dyn, "Box<dyn I>", "borrow dyn I", "borrow mut dyn I", dynamic dispatch, table, dyn-compatible, "dyn fn", "dyn + Send", "dyn + Share", coercion]
diagnostics: [N0205, N0248, N0268, N0330, N0331]
specification: ["type-system.md §37", "type-system.md §36.5", "type-system.md §38.4"]
---

# Dynamic dispatch

**This section teaches** the type `dyn I`: a value whose own type is not known where it is used, only the interface
`I` that it implements. It assumes [Interfaces](./ch10-02-interfaces) and [Box](./ch15-01-box). The part on closures
assumes [Closures](./ch13-01-closures), and the part on threads names `Send` and `Share`, which
[Send and Share](./ch16-04-send-and-share) teaches.

A bound, `S: Shape`, lets generic code call the methods of an interface, and the compiler makes a copy of the code
for each type. So a `Vector<S>` holds one type. To hold a square and a rectangle in one vector, the element type
must be one type that covers both. That type is `dyn Shape`.

## A vector of different types

<span class="listing-caption"><strong>Listing 18-1</strong>, <code>main.cee</code>: One vector holds squares and rectangles, and each call finds the right method.</span>

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

fn main() {
    var shapes = Vector<Box<dyn Shape>>.create();
    shapes.push(Box.create(Square { side: 3 })!)!;
    shapes.push(Box.create(Rect { w: 4, h: 5 })!)!;
    shapes.push(Box.create(Square { side: 1 })!)!;
    var total: u64 = 0;
    for (shape in shapes) {
        Console.write(shape.name());
        Console.write(": ");
        Console.writeLine(shape.area());
        total += shape.area();
    }
    Console.writeLine(total);
}
```

```text
square: 9
rect: 20
square: 1
30
```

- `dyn Shape` means "a value of some type that implements `Shape`". A `Square` holds one `u64` and a `Rect` two, so
  a `dyn Shape` has no one size. It is always held through something of a known size: here a `Box`, which owns the
  value on the heap.
- `Box.create(Square { side: 3 })!` makes a `Box<Square>`. `push` expects a `Box<dyn Shape>`, so the box is
  converted. The conversion moves the box and allocates nothing.
- `shape.area()` calls `Square`'s method for a square and `Rect`'s for a rectangle. Which one is decided while the
  program runs. That is **dynamic dispatch**.

::: tip Rule
`dyn I` is written only as `Box<dyn I>`, which owns the value, or as `borrow dyn I` and `borrow mut dyn I`, which
borrow it. A box of one may be a local, a field, a result, an element or an optional, as any box may.

*Specification: `type-system.md` §37.1.*
:::

## Borrowing as `dyn`

A function that only uses a value takes a borrow of it. A `borrow dyn Shape` parameter accepts a borrow of any type
that implements `Shape`, and a `Box<dyn Shape>` too.

<span class="listing-caption"><strong>Listing 18-2</strong>, <code>main.cee</code>: A local and a box are lent as <code>borrow dyn Shape</code> and <code>borrow mut dyn Shape</code>.</span>

```ceesafe
interface Shape {
    fn area(borrow this) -> u64;
    fn grow(borrow mut this, by: u64);
}

struct Square {
    side: u64;
}

impl Shape for Square {
    fn area(borrow this) -> u64 => this.side * this.side;

    fn grow(borrow mut this, by: u64) {
        this.side += by;
    }
}

fn show(shape: borrow dyn Shape) {
    Console.writeLine(shape.area());
}

fn enlarge(shape: borrow mut dyn Shape) {
    shape.grow(1);
}

fn main() {
    var square = Square { side: 3 };
    show(square);
    enlarge(mut square);
    show(square);

    var boxed: Box<dyn Shape> = Box.create(Square { side: 1 })!;
    boxed.grow(4);
    show(boxed);
    enlarge(mut boxed);
    show(boxed);
}
```

```text
9
16
25
36
```

`show` is compiled once, whatever is passed to it. `square` stays an ordinary local on the stack: nothing is
allocated to lend it. The rules for the receiver are those of any borrow: a `borrow mut this` method needs a
`borrow mut dyn Shape`, or a box in a `var`.

::: tip Rule
A value becomes a `dyn` only where the `dyn` type is written or expected: a `let` with the type, an argument, a field,
a `return`, an argument of `push`. `borrow T` converts to `borrow dyn I`, and `Box<T>` to `Box<dyn I>`, when `T`
implements `I`. The compiler never chooses a `dyn` type by inference.

*Specification: `type-system.md` §37.2.*
:::

Only the interface's methods can be called through a `dyn`. The fields and other methods of the value are out of
reach (`shape.side` is N0234, "type 'dyn Shape' has no fields"), and nothing converts a `dyn Shape` back to a
`Square`. Two `dyn` values cannot be compared with `==`.

## A `dyn` where a bound is asked for

`dyn Shape` implements `Shape`. So generic code written for a bound also takes a `dyn` value, and there is no need
to write a function twice.

<span class="listing-caption"><strong>Listing 18-3</strong>, <code>main.cee</code>: The generic <code>doubled</code> is called with a <code>Square</code> and with a <code>dyn Shape</code>.</span>

```ceesafe
interface Shape {
    fn area(borrow this) -> u64;
}

struct Square {
    side: u64;
}

impl Shape for Square {
    fn area(borrow this) -> u64 => this.side * this.side;
}

fn doubled<S: Shape>(shape: borrow S) -> u64 => shape.area() * 2;

fn main() {
    let square = Square { side: 3 };
    Console.writeLine(doubled(square));
    let boxed: Box<dyn Shape> = Box.create(Square { side: 5 })!;
    let any: borrow dyn Shape = borrow boxed;
    Console.writeLine(doubled(any));
}
```

```text
18
50
```

In the second call `S` is `dyn Shape`, and that copy of `doubled` calls `area` through the table.

## Which interfaces have a `dyn` type

A call through a `dyn` has a value and nothing else: the type is not known. So every member of the interface must
be a method that can be called on a borrowed value.

::: tip Rule
An interface is **dyn-compatible** if each of its methods takes `borrow this` or `borrow mut this`, has no type
parameters of its own, and names `Self` only as its receiver, and if the interface has no static function, no
associated type, no constant and does not require `Copy`. Default methods are allowed. `dyn I` for any other
interface is N0331.

*Specification: `type-system.md` §37.1.*
:::

An interface with type arguments is fine once they are given: `Box<dyn Iterator<u32>>` is a type. If `Shape` extends
another interface, `interface Shape: Named`, a `dyn Shape` has `Named`'s methods too, but it does not convert to a
`dyn Named`.

## Boxed closures

`Box<dyn fn(T) -> R>`, the owning closure that [Closures](./ch13-01-closures) teaches, is the same idea with one method, the call. So closures with different bodies and different captures fit in one
vector.

<span class="listing-caption"><strong>Listing 18-4</strong>, <code>main.cee</code>: Three closures of one <code>dyn fn</code> type are run in order.</span>

```ceesafe
fn main() {
    var steps = Vector<Box<dyn fn(i32) -> i32>>.create();
    let offset = 10;
    steps.push(Box.create(move (n) => n + offset)!)!;
    steps.push(Box.create(move (n) => n * 2)!)!;
    steps.push(Box.create(move (n) => n - 1)!)!;
    var value = 1;
    for (step in steps) {
        value = step(value);
    }
    Console.writeLine(value);
}
```

```text
21
```

Use a `dyn fn` when the thing to store is one action. Use an interface when it has several methods or a name worth
giving.

## `+ Send` and `+ Share`

The compiler cannot see what a `Box<dyn Shape>` holds, so it does not let one move to another thread. A bound in
the type makes the promise: `dyn Shape + Send` accepts only values that are `Send`, and `dyn Shape + Share` only
values that are `Share`. The same is written on a closure type: `Box<dyn fn() -> u64 + Send>`.

<span class="listing-caption"><strong>Listing 18-5</strong>, <code>main.cee</code>: A <code>Box&lt;dyn Shape + Send&gt;</code> moves into a thread, and another becomes a plain <code>Box&lt;dyn Shape&gt;</code>.</span>

```ceesafe
interface Shape {
    fn area(borrow this) -> u64;
}

struct Square {
    side: u64;
}

impl Shape for Square {
    fn area(borrow this) -> u64 => this.side * this.side;
}

fn main() {
    let shape: Box<dyn Shape + Send> = Box.create(Square { side: 3 })!;
    let other: Box<dyn Shape + Send> = Box.create(Square { side: 4 })!;
    let worker = Thread.spawn(move () => shape.area())!;
    Console.writeLine(worker.join());
    let plain: Box<dyn Shape> = other;
    Console.writeLine(plain.area());
}
```

```text
9
16
```

A `+ Share` box converts to `+ Send`, and either to the plain type. Nothing converts the other way: once the
promise is dropped from the type, it is gone.

## How a call is dispatched

`ceesafec build --emit c -o <directory>` writes the C for a program. For Listing 18-1 it has this (the long
generated names are shortened here to `ShapeRef` and `ShapeTable`):

```c
typedef struct { void *data; const ShapeTable *vt; } ShapeRef; /* dyn Shape */

struct ShapeTable {
    void (*drop)(void *);
    uint64_t size;
    uint64_t align;
    uint64_t (*m0)(void *);
    cee_str (*m1)(void *);
};
```

- A `Box<dyn Shape>` is two pointers: one to the value and one to a **table**. A `borrow dyn Shape` is the same
  pair. A `Vector<Box<dyn Shape>>` is an array of these pairs.
- There is one constant table for each pair of a type and an interface that the program converts: one for
  `Square` as `Shape`, one for `Rect` as `Shape`. It holds how to destroy the value, its size and alignment, and
  one function for each method of the interface, in the order the interface declares them.
- The conversion `Box<Square>` to `Box<dyn Shape>` writes the pair: the box's pointer and the address of
  `Square`'s table.
- `shape.area()` in the loop is `t.vt->m0(t.data)`: read the function from the table, and call it with the pointer
  to the value. `m0` for `Square` is a small function that calls `Square`'s `area`.
- Destroying the box calls the table's `drop` function, if the type has one, and frees the memory.

The call cannot be inlined, because the C compiler does not know which function it reaches.
[When to use it](./ch18-02-when-to-use-it) measures what that costs.

## Mistakes

::: warning Mistake: a `dyn` that is not in a box or behind a borrow
A vector's elements all have one size, and a `dyn Shape` has none.
:::

<span class="listing-caption"><strong>Listing 18-6</strong>, <code>main.cee</code>: A vector of <code>dyn Shape</code> itself. This does not compile.</span>

<!-- ceesafe: fail N0330 -->
```ceesafe
interface Shape {
    fn area(borrow this) -> u64;
}

struct Square {
    side: u64;
}

impl Shape for Square {
    fn area(borrow this) -> u64 => this.side * this.side;
}

fn main() {
    var shapes = Vector<dyn Shape>.create();
    shapes.push(Square { side: 3 })!;
}
```

```text
main.cee:14:25: error[N0330]: 'dyn Shape' has no size the compiler knows: use it behind a reference or a box ('borrow dyn Shape', 'Box<dyn Shape>'; type-system.md §37.1)
   |
14 |     var shapes = Vector<dyn Shape>.create();
   |                         ^^^^^^^^^
```

Write `Vector<Box<dyn Shape>>` and push `Box.create(…)!`, as Listing 18-1 does.

::: warning Mistake: `dyn` of an interface with a static function
`unit` has no `this`, so a call of it has no value in which to find a table.
:::

<span class="listing-caption"><strong>Listing 18-7</strong>, <code>main.cee</code>: <code>Shape</code> has a static function, so there is no <code>dyn Shape</code>. This does not compile.</span>

<!-- ceesafe: fail N0331 -->
```ceesafe
interface Shape {
    fn area(borrow this) -> u64;
    fn unit() -> Self;
}

struct Square {
    side: u64;
}

impl Shape for Square {
    fn area(borrow this) -> u64 => this.side * this.side;
    fn unit() -> Self => Square { side: 1 };
}

fn main() {
    let shape: Box<dyn Shape> = Box.create(Square { side: 3 })!;
    Console.writeLine(shape.area());
}
```

```text
main.cee:16:20: error[N0331]: 'Shape' cannot be used as 'dyn Shape': its method 'unit' is a static function, which has no value to find a table in (type-system.md §37.1)
   |
16 |     let shape: Box<dyn Shape> = Box.create(Square { side: 3 })!;
   |                    ^^^^^^^^^
```

The message names the member that stands in the way. Move it to a second interface that generic code uses with a
bound, and keep the interface for `dyn` to methods on a borrowed value.

::: warning Mistake: a changing method through a shared borrow
`borrow dyn Shape` is a shared borrow, as `borrow Square` is.
:::

<span class="listing-caption"><strong>Listing 18-8</strong>, <code>main.cee</code>: <code>grow</code> needs <code>borrow mut this</code> and the parameter is a shared borrow. This does not compile.</span>

<!-- ceesafe: fail N0248 -->
```ceesafe
interface Shape {
    fn area(borrow this) -> u64;
    fn grow(borrow mut this, by: u64);
}

struct Square {
    side: u64;
}

impl Shape for Square {
    fn area(borrow this) -> u64 => this.side * this.side;

    fn grow(borrow mut this, by: u64) {
        this.side += by;
    }
}

fn enlarge(shape: borrow dyn Shape) {
    shape.grow(1);
}

fn main() {
    var square = Square { side: 3 };
    enlarge(square);
    Console.writeLine(square.area());
}
```

```text
main.cee:19:5: error[N0248]: 'grow' takes 'borrow mut this', but this value is behind a shared reference
   |
19 |     shape.grow(1);
   |     ^^^^^
```

Take `borrow mut dyn Shape` and call `enlarge(mut square)`, as Listing 18-2 does.

::: warning Mistake: asking for the value's own type back
A `Box<dyn Shape>` has forgotten that it holds a `Square`. There is no cast that remembers.
:::

<span class="listing-caption"><strong>Listing 18-9</strong>, <code>main.cee</code>: A <code>Box&lt;dyn Shape&gt;</code> is not a <code>Box&lt;Square&gt;</code>. This does not compile.</span>

<!-- ceesafe: fail N0205 -->
```ceesafe
interface Shape {
    fn area(borrow this) -> u64;
}

struct Square {
    side: u64;
}

impl Shape for Square {
    fn area(borrow this) -> u64 => this.side * this.side;
}

fn main() {
    let shape: Box<dyn Shape> = Box.create(Square { side: 3 })!;
    let square: Box<Square> = shape;
    Console.writeLine(square.side);
}
```

```text
main.cee:15:31: error[N0205]: expected 'Box<Square>', found 'Box<dyn Shape>'
   |
15 |     let square: Box<Square> = shape;
   |                               ^^^^^
```

Put what the code needs into the interface as a method. If the code must tell the kinds apart and reach their
data, the design wants an enum: [When to use it](./ch18-02-when-to-use-it).

::: warning Mistake: passing the box where a bound is asked for
`dyn Shape` implements `Shape`. The box around it does not.
:::

<span class="listing-caption"><strong>Listing 18-10</strong>, <code>main.cee</code>: <code>doubled</code> is given the box, so <code>S</code> would be <code>Box&lt;dyn Shape&gt;</code>. This does not compile.</span>

<!-- ceesafe: fail N0268 -->
```ceesafe
interface Shape {
    fn area(borrow this) -> u64;
}

struct Square {
    side: u64;
}

impl Shape for Square {
    fn area(borrow this) -> u64 => this.side * this.side;
}

fn doubled<S: Shape>(shape: borrow S) -> u64 => shape.area() * 2;

fn main() {
    let boxed: Box<dyn Shape> = Box.create(Square { side: 5 })!;
    Console.writeLine(doubled(boxed));
}
```

```text
main.cee:17:23: error[N0268]: 'Box<dyn Shape>' does not implement 'Shape', which 'S' requires; the box's contents, 'dyn Shape', implement it: pass a 'borrow dyn Shape' (as in 'let r: borrow dyn Shape = borrow box;'), or declare the parameter with that type
   |
17 |     Console.writeLine(doubled(boxed));
   |                       ^^^^^^^^^^^^^^
```

Borrow the contents as a `dyn` first, as Listing 18-3 does: `let any: borrow dyn Shape = borrow boxed;`.

::: details Coming from Rust
`dyn Shape` is Rust's `dyn Shape`, `Box<dyn Shape>` is the same, and `borrow dyn Shape` is `&dyn Shape`. The
differences:

- an interface with a static function, an associated type or a generic method has no `dyn` type at all. Rust's
  `where Self: Sized` on one method, to keep the rest usable, does not exist;
- there is no `Any` and no downcast;
- `dyn Shape + Share` is Rust's `dyn Shape + Sync`. `Share` implies `Send`;
- no lifetime is written on a `dyn` type, and a `Box<dyn I>` cannot hold a type that has a region parameter.
:::

::: details Coming from C#, Java, TypeScript or Go
In those languages every value of an interface type is already a pointer to an object with a table, so a
`List<Shape>` is what `Vector<Box<dyn Shape>>` is here. CeeSafe makes you write the box, because the allocation is
real and can fail, and write `dyn`, because the default, a bound, has no table and no indirect call. There is no
`is`, `instanceof` or type switch on a `dyn` value: use an enum and `match` for that.
:::

::: details Coming from C
A `dyn Shape` is the struct of function pointers you would write by hand, with a `void *` beside it. The compiler
writes the table, fills it, and checks that the pointer and the table always belong together.
:::

## Summary

- `dyn I` is a value of some type that implements `I`. It lives in a `Box<dyn I>` or behind `borrow dyn I` or
  `borrow mut dyn I`; anywhere else it is N0330.
- `Box<T>` and `borrow T` convert to the `dyn` forms where the `dyn` type is expected, never by inference, and
  never back (N0205).
- Only the interface's methods can be called. A `borrow mut this` method needs an exclusive borrow of the value (N0248).
- An interface has a `dyn` type only if every member is a method on a borrowed `this` without type parameters or
  `Self` elsewhere (N0331).
- `dyn I` implements `I`, so generic code with a bound takes it; the box around it does not (N0268).
- `Box<dyn fn(…) -> R>` is the same for one action. `+ Send` and `+ Share` in a `dyn` type say what may cross
  threads.
- A `dyn` value is two pointers, to the value and to a table of its type's functions. A call reads the function
  from the table.

Next: [When to use it](./ch18-02-when-to-use-it), which sets `dyn` beside a generic and an enum.
