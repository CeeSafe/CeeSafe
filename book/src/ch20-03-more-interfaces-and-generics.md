---
title: More on interfaces and generics
summary: What chapter 10 left out - methods with their own type parameters, type arguments inferred from a bound, const parameters, constants and associated types in an interface, supertraits, generic impls, and the features CeeSafe does not have.
assumes: [ch10-01-generic-types, ch10-02-interfaces]
keywords: ["generic method", "const parameter", "const generics", "associated constant", "associated type", supertrait, "generic impl", "interface inheritance", where, specialisation, Self]
diagnostics: [N0267, N0268, N0353]
specification: ["type-system.md §20.1", "type-system.md §20.2", "type-system.md §20.5", "type-system.md §21.1", "type-system.md §21.2"]
---

# More on interfaces and generics

**This section teaches** the parts of generics and interfaces that the course did not need: generic methods,
const parameters, constants and types inside an interface, interfaces built on other interfaces, and generic
impls. It ends with a list of what the language does not have. It assumes
[Generic types and functions](./ch10-01-generic-types) and [Interfaces](./ch10-02-interfaces), which cover type
parameters, bounds with `+`, default methods, `Self` and static functions.

Everything here follows the two rules of chapter 10. Generic code is checked once, knowing of a type parameter
only what its bounds say. Then each use with real types is compiled as its own copy, so nothing is decided while
the program runs.

## Generic methods and inference from a bound

A method may have type parameters of its own, beside those of its struct.

<span class="listing-caption"><strong>Listing 20-21</strong>, <code>main.cee</code>: A struct that is not generic, with two generic functions, and a function whose first type argument comes from the bound of its second.</span>

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

struct Canvas {
    covered: u64;

    fn add<S: Shape>(borrow mut this, shape: borrow S) {
        this.covered += shape.area();
    }

    fn of<S: Shape>(shape: borrow S) -> Canvas => Canvas { covered: shape.area() };
}

fn count<T, I: Iterator<T>>(items: I) -> usize {
    var n: usize = 0;
    for (item in items) {
        n += 1;
    }
    return n;
}

fn main() {
    var canvas = Canvas.of(Square { side: 2 });
    canvas.add(Rect { w: 3, h: 4 });
    canvas.add<Square>(Square { side: 1 });
    Console.writeLine(canvas.covered);
    Console.writeLine(count("a,b,c".split(",")));
}
```

```text
17
3
```

- `canvas.add(…)` infers `S` from the argument. `canvas.add<Square>(…)` writes it, after the method's name.
- `count` has two type parameters and one argument. `I` is the type of the argument. `T` appears in no
  parameter, and is found through the bound `I: Iterator<T>`: the value that `split` returns implements
  `Iterator` for exactly one item type, so `T` is that type.

An interface's methods may have type parameters too. Such an interface cannot be used as a `dyn` type, which
chapter 18 covers.

## Const parameters

A **const parameter** stands for a value, not a type. It is written `const NAME: T` in the parameter list, and
its type is `bool`, an integer type, or an enum without payloads.

<span class="listing-caption"><strong>Listing 20-22</strong>, <code>main.cee</code>: A struct whose array length is a parameter, a function that accepts an array of any length, and a function compiled once for each value of a <code>bool</code>.</span>

```ceesafe
struct Window<const N: usize> {
    values: [u64; N];
    next: usize;

    fn create() -> Self => Self { values: [0; N], next: 0 };

    fn push(borrow mut this, value: u64) {
        this.values[this.next] = value;
        this.next = (this.next + 1) % N;
    }

    fn mean(borrow this) -> u64 {
        var total: u64 = 0;
        for (v in this.values) {
            total += v;
        }
        return total / (N as u64);
    }
}

fn sum<const N: usize>(values: [u8; N]) -> u32 {
    var total: u32 = 0;
    for (v in values) {
        total += v as u32;
    }
    return total;
}

fn label<const LOUD: bool>(text: StringView) {
    if (LOUD) {
        Console.write("!! ");
    }
    Console.writeLine(text);
}

fn main() {
    var window = Window<3>.create();
    window.push(10);
    window.push(20);
    window.push(60);
    window.push(40);
    Console.writeLine(window.mean());
    Console.writeLine(sum([1u8, 2, 3, 4]));
    label<true>("disk full");
    label<false>("saved");
}
```

```text
40
10
!! disk full
saved
```

- `Window<3>` and `Window<5>` are different types, as `Vector<u8>` and `Vector<i32>` are. The array is inside
  the struct, with no allocation.
- Inside the generic code, `N` is read like any value: `% N`, `N as u64`, `[0; N]`.
- `sum([1u8, 2, 3, 4])` infers `N` as 4 from the argument's type.
- `label<true>` and `label<false>` are two functions. In each, `LOUD` is a constant, so the C compiler removes
  the `if`.

An argument is a literal, `true` or `false`, the name of a constant, an enum variant, or one of the enclosing
item's own const parameters.

::: tip Rule
A const parameter's argument is known when the program is compiled. A value computed while the program runs
cannot be one, and neither can an expression over a const parameter, such as `[u8; N + 1]`.

*Specification: `type-system.md` §20.5.*
:::

## Constants and types in an interface

An interface may declare constants, with or without a default value, and **associated types**, which each `impl`
chooses.

<span class="listing-caption"><strong>Listing 20-23</strong>, <code>main.cee</code>: Two encodings. Each <code>impl</code> gives a name, may give a width, and chooses the type of one unit.</span>

```ceesafe
interface Encoding {
    const NAME: StringView;
    const WIDTH: usize = 1;
    type Unit: Copy;

    fn decode(bytes: Slice<u8>) -> Self.Unit?;
}

struct Latin1 {
    unused: u8;
}

struct Ucs2 {
    unused: u8;
}

impl Encoding for Latin1 {
    const NAME: StringView = "latin-1";
    type Unit = u8;

    fn decode(bytes: Slice<u8>) -> u8? => if (bytes.length > 0) bytes[0] else null;
}

impl Encoding for Ucs2 {
    const NAME: StringView = "ucs-2";
    const WIDTH: usize = 2;
    type Unit = u16;

    fn decode(bytes: Slice<u8>) -> u16? {
        if (bytes.length < 2) {
            return null;
        }
        return (bytes[0] as u16) * 256 + (bytes[1] as u16);
    }
}

fn first<E: Encoding>(bytes: Slice<u8>) -> E.Unit? {
    Console.write(E.NAME);
    Console.write(" reads ");
    Console.write(E.WIDTH);
    Console.writeLine(" at a time");
    return E.decode(bytes);
}

fn main() {
    let bytes = [1u8, 2];
    Console.writeLine(first<Latin1>(bytes[..]) ?? 0);
    Console.writeLine(first<Ucs2>(bytes[..]) ?? 0);
    Console.writeLine(Ucs2.WIDTH);
}
```

```text
latin-1 reads 1 at a time
1
ucs-2 reads 2 at a time
258
2
```

- `Latin1` leaves `WIDTH` out and gets the default. `Ucs2` gives its own.
- `type Unit: Copy;` says that each `impl` names a type, and that the type is Copy. Generic code knows only that
  bound about it.
- In the interface the type is `Self.Unit`. Through a bound it is `E.Unit`, and the constants are `E.NAME` and
  `E.WIDTH`. On a type they are `Ucs2.WIDTH`.
- `first` takes no value of type `E`, so the call writes the type argument: `first<Latin1>(…)`.

Through a type parameter, `E.WIDTH` is a value of each copy and not a constant expression: it cannot be an array
length or a const parameter's argument. `Ucs2.WIDTH`, on a named type, can.

Use an associated type when the implementing type decides it and generic code only passes it along. When
generic code must say which type it wants, make it a parameter of the interface, as `Iterator<T>` does: a bound
can then name it, `I: Iterator<u8>`.

## Supertraits

An interface can require another. `interface Shape: Named` says that every type implementing `Shape` also
implements `Named`. `Named` is a **supertrait** of `Shape`.

<span class="listing-caption"><strong>Listing 20-24</strong>, <code>main.cee</code>: <code>Shape</code> builds on <code>Named</code>, so a bound of <code>Shape</code> gives <code>name</code> too.</span>

```ceesafe
interface Named {
    fn name(borrow this) -> StringView;
}

interface Shape: Named {
    fn area(borrow this) -> u64;

    fn describe(borrow this) {
        Console.write(this.name());
        Console.write(" of area ");
        Console.writeLine(this.area());
    }
}

struct Square {
    side: u64;
}

impl Named for Square {
    fn name(borrow this) -> StringView => "square";
}

impl Shape for Square {
    fn area(borrow this) -> u64 => this.side * this.side;
}

fn greet<N: Named>(thing: borrow N) {
    Console.write("hello, ");
    Console.writeLine(thing.name());
}

fn show<S: Shape>(shape: borrow S) {
    greet(shape);
    shape.describe();
}

fn main() {
    show(Square { side: 3 });
}
```

```text
hello, square
square of area 9
```

The two interfaces are implemented in two `impl` blocks: `impl Shape` does not contain `name`. In return, the
default method `describe` may call `this.name()`, and inside `show` an `S: Shape` is accepted where a `Named` is
wanted. Several supertraits are joined with `+`, and `Copy` may be one: `interface Token: Named + Copy`.

## Generic impls

An `impl` may have type parameters, to implement an interface for every instance of a generic type at once.

<span class="listing-caption"><strong>Listing 20-25</strong>, <code>main.cee</code>: A pair of shapes is a shape, whatever the shapes are.</span>

```ceesafe
interface Shape {
    fn area(borrow this) -> u64;
}

struct Square {
    side: u64;
}

struct Pair<T> {
    first: T;
    second: T;
}

impl Shape for Square {
    fn area(borrow this) -> u64 => this.side * this.side;
}

impl<T: Shape> Shape for Pair<T> {
    fn area(borrow this) -> u64 => this.first.area() + this.second.area();
}

fn main() {
    let two = Pair { first: Square { side: 1 }, second: Square { side: 2 } };
    Console.writeLine(two.area());
    let four = Pair { first: two, second: Pair { first: Square { side: 3 }, second: Square { side: 4 } } };
    Console.writeLine(four.area());
}
```

```text
5
30
```

`impl<T: Shape> Shape for Pair<T>` reads: for every `T` that implements `Shape`, `Pair<T>` implements `Shape`.
So `Pair<Square>` is a shape, and then `Pair<Pair<Square>>` is one. A `Pair<u8>` is still a valid type; it is
not a `Shape`.

::: tip Rule
Beside a generic impl, no other impl of the same interface may have a target of the same struct or enum.
`impl Shape for Pair<u8>` and `impl<T> Shape for Pair<T>` are refused together: there is no specialisation.

*Specification: `type-system.md` §21.1.*
:::

## What CeeSafe does not have

Some features of other languages' generics are absent, by decision. Knowing the list saves looking for them.

| Not in CeeSafe | What to write |
|---|---|
| `where` clauses | Bounds go in the parameter list: `<T: Shape + Copy>`. |
| Default values for type or const parameters | Write the argument, or a second function that passes it. |
| Expressions over a const parameter (`[u8; N + 1]`) | A second const parameter, or a constant. |
| Specialisation and overlapping impls | One impl for each type and interface. |
| Defaults for associated types, and bounds that fix one (`E: Encoding<Unit = u16>`) | A type parameter on the interface. |
| Associated types with parameters of their own | A type parameter on the interface. |
| Operator overloading through an interface | `==` comes from `@derive(Equal)`; the rest are named methods. |
| An interface as a type by itself | A bound `T: I`, or `dyn I` behind a pointer (chapter 18). |

## Mistakes

::: warning Mistake: a run-time value as a const argument
`label<LOUD>` is chosen when the program is compiled. A parameter's value is not known then.
:::

<span class="listing-caption"><strong>Listing 20-26</strong>, <code>main.cee</code>: A <code>bool</code> parameter used as a const argument. This does not compile.</span>

<!-- ceesafe: fail N0353 -->
```ceesafe
fn label<const LOUD: bool>(text: StringView) {
    if (LOUD) {
        Console.write("!! ");
    }
    Console.writeLine(text);
}

fn report(urgent: bool) {
    label<urgent>("disk full");
}

fn main() {
    report(true);
}
```

```text
main.cee:9:11: error[N0353]: the argument for the const parameter 'LOUD' of 'label' must be a constant: 'urgent' is a run-time value; choose the instance at run time: `if (urgent) label<true>(…) else label<false>(…)` (type-system.md §20.5)
  |
9 |     label<urgent>("disk full");
  |           ^^^^^^
```

The message gives the fix. Both copies exist, and an `if` picks one.

::: warning Mistake: implementing an interface without its supertrait
`impl Shape for T` needs `impl Named for T` to exist.
:::

<span class="listing-caption"><strong>Listing 20-27</strong>, <code>main.cee</code>: <code>Square</code> implements <code>Shape</code> and not <code>Named</code>. This does not compile.</span>

<!-- ceesafe: fail N0268 -->
```ceesafe
interface Named {
    fn name(borrow this) -> StringView;
}

interface Shape: Named {
    fn area(borrow this) -> u64;
}

struct Square {
    side: u64;
}

impl Shape for Square {
    fn area(borrow this) -> u64 => this.side * this.side;
}

fn main() {
    Console.writeLine(Square { side: 3 }.area());
}
```

```text
main.cee:13:16: error[N0268]: 'Square' does not implement 'Named', which 'Shape' requires as a supertrait (ADR 0086 §2.6)
   |
13 | impl Shape for Square {
   |                ^^^^^^
```

Add `impl Named for Square { … }`. Writing `name` inside `impl Shape` is not the fix: an `impl` holds only its
own interface's methods.

::: warning Mistake: two bounds with a method of the same name
Through a bound, a method is found by its name alone.
:::

<span class="listing-caption"><strong>Listing 20-28</strong>, <code>main.cee</code>: Both interfaces of the bound declare <code>show</code>. This does not compile.</span>

<!-- ceesafe: fail N0267 -->
```ceesafe
interface Printer {
    fn show(borrow this);
}

interface Screen {
    fn show(borrow this);
}

fn both<T: Printer + Screen>(thing: borrow T) {
    thing.show();
}

fn main() {
    Console.writeLine(1);
}
```

```text
main.cee:10:11: error[N0267]: 'show' is ambiguous: both 'Printer' and 'Screen' declare it
   |
10 |     thing.show();
   |           ^^^^
```

There is no syntax that names the interface at the call. Give the methods different names. A type could not
implement both interfaces in any case: an interface's methods are ordinary methods of the type, and a type has
one method of each name.

::: details Coming from Rust
- Const parameters are const generics, limited to `bool`, integers and fieldless enums, with no `{ N + 1 }`.
- Associated constants and associated types are as in Rust, but there is no `T: Trait<Item = X>`, no default
  for an associated type and no generic associated type. `Iterator` takes its item as a parameter.
- Supertraits are written the same way. `Copy` may be one; `Sized`, `Send` and `Sync` are not.
- There are no `where` clauses, no blanket impl over a bare `T` (`impl<T: Shape> Named for T`): a generic impl's
  target is a generic struct or enum. There is no `impl Trait` in argument or result position.
- There is no `<T as Trait>::method` to choose between two bounds.
:::

::: details Coming from C#, Java, TypeScript or Go
- A const parameter has no counterpart in these languages: the nearest is a constructor argument, which costs a
  field and a test while the program runs. Here each value makes its own compiled copy.
- An interface that builds on another is interface inheritance, `interface Shape : Named` in C#. The type still
  writes one `impl` block for each.
- A constant or a static function in an interface is C#'s static abstract member. Java and Go have none.
:::

## Summary

- A method may declare type parameters of its own: `canvas.add<Square>(s)`, or inferred from the arguments.
- A type parameter that no argument fixes is inferred from a bound such as `I: Iterator<T>`.
- `const N: T` is a parameter that stands for a value of a `bool`, integer or fieldless enum type. Its argument
  is a compile-time constant (`N0353`), and each value is a separate copy.
- An interface may hold constants (`const NAME: T;`, read as `E.NAME`) and associated types (`type Unit;`, read
  as `E.Unit`), which each `impl` gives.
- `interface B: A` makes `A` a supertrait: implementing `B` needs `A` (`N0268`), and a bound `T: B` gives `A`'s
  methods.
- `impl<T: B> I for S<T>` implements `I` for every instance at once. Impls never overlap.
- Two bounds that declare the same method name make the call ambiguous (`N0267`).
- There are no `where` clauses, default parameter values, specialisation or operator overloading.

Next: [Arithmetic: checked, wrapping, conversions](./ch20-04-arithmetic).
