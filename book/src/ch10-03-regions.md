---
title: Regions
summary: A struct that holds a borrow or a view declares a region parameter, which ties each value of the struct to what it borrows from - with functions that return a borrow, borrow(a, b), the diagnostics N0243, N0244, N0245 and N0270, and when to own instead.
assumes: [ch04-02-borrowing, ch04-03-slices-and-views, ch05-01-defining-structs, ch10-01-generic-types]
keywords: [region, region parameter, "StringView<r>", "borrow(a, b)", scoped type, lifetime, outlive, static, own]
diagnostics: [N0243, N0244, N0245, N0270]
specification: ["type-system.md §25.1", "type-system.md §25.2", "type-system.md §25.4", "type-system.md §25.6", "lifetimes.md §7"]
---

# Regions

**This section teaches** how a struct holds a borrow, how a function says which parameter its result borrows from,
and what the compiler reports when a borrow would outlive its value. It assumes [Borrowing](./ch04-02-borrowing),
[Slices and views](./ch04-03-slices-and-views), [Defining structs](./ch05-01-defining-structs) and the angle
brackets of [Generic types and functions](./ch10-01-generic-types).

A borrow must not outlive the value it borrows from. For a borrow in a local variable, the compiler sees both in
one function. A borrow stored in a struct can travel: the struct is returned, passed on, put in a vector. So the
struct's type must say that it holds a borrow. It says so with a **region parameter**.

## A struct that holds a view

A `StringView` is a view: a borrow of part of a string. A struct with a `StringView` field declares a region
between `<` and `>`, with the word `region`, and gives it to the field.

<span class="listing-caption"><strong>Listing 10-15</strong>, <code>main.cee</code>: A struct with a region parameter holds a view of a string that <code>main</code> owns.</span>

```ceesafe
struct Word<region src> {
    text: StringView<src>;
    start: usize;

    fn length(borrow this) -> usize => this.text.length;
}

fn secondWord(line: StringView) -> Word {
    var start: usize = 0;
    while (start < line.length && line[start] != ' ') {
        start += 1;
    }
    start += 1;
    var end = start;
    while (end < line.length && line[end] != ' ') {
        end += 1;
    }
    return Word { text: line[start..end], start: start };
}

fn main() {
    let line = String.from("hello wide world")!;
    let word = secondWord(line.view());
    Console.writeLine(word.text);
    Console.writeLine(word.start);
    Console.writeLine(word.length());
}
```

```text
wide
6
4
```

- `struct Word<region src>` declares the region `src`. A region is a name for "the value this borrows from, and
  how long that value lives". It is not a type and holds no data.
- `text: StringView<src>` says that the view borrows from `src`.
- `secondWord` takes one view and returns a `Word`. With one borrowed parameter, the compiler takes the result to
  borrow from it, so the regions need not be written. Written out, the function is
  `fn secondWord<region src>(line: StringView<src>) -> Word<src>`.
- In `main`, `word` borrows from `line`. No text is copied: `word.text` is the four bytes inside `line`.

`String.from` makes a `String` that owns a copy of the text. The `!` after it stops the program if memory runs
out; [chapter 9](./ch09-02-result) covers it.

::: tip Rule
A struct or enum that holds a borrow, a view or a slice declares a region parameter: `struct S<region r>`. The
field names it: `StringView<r>`, `Slice<r, T>`, `borrow(r) T`. Region parameters come before type parameters:
`struct Cursor<region r, T>`.

*Specification: `type-system.md` §25.1, §25.4.*
:::

## What the region says

A value of type `Word` carries the borrow of `line` wherever it goes, and the rule of
[Borrowing](./ch04-02-borrowing#the-rule) applies to it as to any borrow: while `word` is in use, `line` cannot be
changed, moved or destroyed.

<span class="listing-caption"><strong>Listing 10-16</strong>, <code>main.cee</code>: <code>word</code> is used after the string it borrows from is gone. This does not compile.</span>

<!-- ceesafe: fail N0243 -->
```ceesafe
struct Word<region src> {
    text: StringView<src>;
}

fn main() {
    var word = Word { text: "none" };
    {
        let line = String.from("hello wide world")!;
        word = Word { text: line.view() };
    }
    Console.writeLine(word.text);
}
```

```text
main.cee:10:6: error[N0243]: 'line' goes out of scope while still borrowed
   |
10 |     }
   |      ^
   = note: 'line' is borrowed here (9:29)
   = note: the borrow is used later here (11:5)
```

`line` is destroyed at the `}` on line 10, and `word` still holds a view of it on line 11. Without the check, that
line would read freed memory. The fix is to keep `line` alive as long as `word`: declare it in the outer block.

On line 6, `"none"` is a string literal. Literals are part of the program and live as long as it runs, so a `Word`
of a literal can go anywhere. Their region has a name, `static`.

::: tip Rule
A value whose type has a region cannot outlive what it borrows from, and while it is live the rule of borrowing
protects that value. Regions exist only for the compiler: they cost nothing when the program runs.

*Specification: `type-system.md` §25.5.*
:::

## Functions that return a borrow

A function that returns a borrow, a view, or a struct with a region returns something borrowed from its
parameters. The caller needs to know from which, to know how long the result is good for.

- With **one** borrowed parameter (a `borrow this` counts), the result borrows from it. Nothing is written.
- With **several**, the result names them: `borrow(a, b) T` for a borrow, and a shared region for views and
  structs.

<span class="listing-caption"><strong>Listing 10-17</strong>, <code>main.cee</code>: Three functions that return a borrow, with the source implied, named by parameter, and named by region.</span>

```ceesafe
fn firstOf(values: Slice<i64>) -> borrow i64 {
    return borrow values[0];
}

fn larger(a: borrow i64, b: borrow i64) -> borrow(a, b) i64 => if (a > b) a else b;

fn longer<region r>(a: StringView<r>, b: StringView<r>) -> StringView<r> {
    return if (a.length >= b.length) a else b;
}

fn main() {
    let numbers = [4i64, 9, 2];
    Console.writeLine(firstOf(numbers[..]));
    let x: i64 = 3;
    let y: i64 = 7;
    Console.writeLine(larger(x, y));

    let pear = String.from("pear")!;
    let apple = String.from("apple")!;
    Console.writeLine(longer(pear.view(), apple.view()));
}
```

```text
4
7
apple
```

- `firstOf` has one borrowed parameter, so `-> borrow i64` borrows from `values`.
- `larger` may return either parameter, and says so: `borrow(a, b) i64`. The caller then keeps both `x` and `y`
  unchanged while it uses the result. A function that only ever returns `a` writes `borrow(a) i64`, and the caller
  is free to change `y`.
- `longer` returns a view. Both parameters and the result name the same region `r`, which says the result may
  borrow from either.

::: tip Rule
A function with several borrowed parameters names the ones its result borrows from: `borrow(a, b) T`, or one
region shared by the parameters and the result. With one borrowed parameter, it is implied.

*Specification: `lifetimes.md` §7; `type-system.md` §25.6.*
:::

The diagnostics say "reference" for a borrow and "scoped type" for a type that holds one.

## When to own instead

A struct with a region is cheap, because it copies nothing, and short-lived, because it is tied to another value.
That suits a value used and dropped while its source stays put: a token of a line being parsed, a field of a
record being read. It does not suit a value that must outlive its source: one returned from the function that
made the text, stored for later, or kept after the text changes. Such a struct **owns** its data: a `String`
where the other holds a `StringView`, a `Vector<T>` for a `Slice<T>`.

<span class="listing-caption"><strong>Listing 10-18</strong>, <code>main.cee</code>: A struct that owns its text needs no region and can be returned from the function that builds the text.</span>

```ceesafe
struct Greeting {
    text: String;
}

fn greet(name: StringView) -> Greeting {
    var text = String.create();
    text.append("Hello, ")!;
    text.append(name)!;
    return Greeting { text: text };
}

fn main() {
    let greeting = greet("Ada");
    Console.writeLine(greeting.text);
}
```

```text
Hello, Ada
```

`greet` moves `text` into the struct and the struct to the caller. Nothing is borrowed, so nothing limits how long
`greeting` lives. The price is the memory for the text and the work of building it.

## Mistakes

::: warning Mistake: a view in a struct without a region
A field of type `StringView`, `Slice<T>` or `borrow T` needs the struct to declare a region.
:::

<span class="listing-caption"><strong>Listing 10-19</strong>, <code>main.cee</code>: The struct holds a view and declares no region. This does not compile.</span>

<!-- ceesafe: fail N0270 -->
```ceesafe
struct Word {
    text: StringView;
    start: usize;
}

fn main() {
    let w = Word { text: "hello", start: 0 };
    Console.writeLine(w.text);
}
```

```text
main.cee:2:11: error[N0270]: a field of scoped type 'StringView' needs a region parameter on the struct: 'struct Word<region r>' (type-system.md §25.4)
  |
2 |     text: StringView;
  |           ^^^^^^^^^^
```

Declare the region and name it in the field, as Listing 10-15 does, or make the field a `String` if the struct
should own its text.

::: warning Mistake: returning a borrow of a local
A function's locals are destroyed when it returns, so the result cannot borrow from one.
:::

<span class="listing-caption"><strong>Listing 10-20</strong>, <code>main.cee</code>: The <code>Word</code> would view a string that the function is about to destroy. This does not compile.</span>

<!-- ceesafe: fail N0244 -->
```ceesafe
struct Word<region src> {
    text: StringView<src>;
}

fn make(prefix: StringView) -> Word {
    var line = String.create();
    line.append(prefix)!;
    line.append(" wide world")!;
    return Word { text: line.view() };
}

fn main() {
    let word = make("hello");
    Console.writeLine(word.text);
}
```

```text
main.cee:9:5: error[N0244]: cannot return a reference to 'line', which is local to 'make'
  |
9 |     return Word { text: line.view() };
  |     ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
  = note: borrowed here (9:25)
```

This is the case for owning: return a struct with a `String` field, as Listing 10-18 does, and move `line` into
it.

::: warning Mistake: several borrowed parameters and an unnamed result
With two borrowed parameters, the compiler does not guess which one the result borrows from.
:::

<span class="listing-caption"><strong>Listing 10-21</strong>, <code>main.cee</code>: The result does not say whether it borrows from <code>a</code> or <code>b</code>. This does not compile.</span>

<!-- ceesafe: fail N0245 -->
```ceesafe
fn larger(a: borrow i64, b: borrow i64) -> borrow i64 => if (a > b) a else b;

fn main() {
    let x: i64 = 3;
    let y: i64 = 7;
    Console.writeLine(larger(x, y));
}
```

```text
main.cee:1:44: error[N0245]: this function has several reference parameters: name the ones the result is borrowed from, e.g. 'borrow(a, b) T'
  |
1 | fn larger(a: borrow i64, b: borrow i64) -> borrow i64 => if (a > b) a else b;
  |                                            ^^^^^^^^^^
```

Write `-> borrow(a, b) i64`. The compiler gives the same message for two `StringView` parameters and a
`StringView` result, but there `borrow(a, b) StringView` is not accepted: a view is already a borrow. Use a region,
as `longer` does in Listing 10-17.

::: details Coming from Rust
A region is a lifetime: `struct Word<region src>` is `struct Word<'src>`, `StringView<src>` is `&'src str`, and
`static` is `'static`. What differs:

- a result names parameters, `borrow(a, b) T`, where Rust would declare a lifetime `'a` and write it three times;
- a region can be left out of a type in a signature (`-> Word`, not `-> Word<'_>`), and the elision rule is one
  line: one borrowed parameter, or name the sources;
- there are no bounds between regions (`'a: 'b`), and region arguments do not make different types: `Word<a>` and
  `Word<b>` are one type.
:::

::: details Coming from C#, Java, TypeScript or Go
In those languages a field that refers to a string keeps the string alive, through the garbage collector. CeeSafe
has none, so a field either owns its data (`String`) or borrows it (`StringView<r>`), and a borrow is checked. If
you do not want to think about regions, own: it is what a field does in those languages, without the sharing.
:::

::: details Coming from C
`Word` is a struct with a `const char *` and a length that point into someone else's buffer. The region is the
comment "valid only while the line buffer is" turned into something the compiler checks.
:::

## Summary

- A struct or enum that holds a borrow, a view or a slice declares a region parameter, `struct S<region r>`, and
  its fields name it (`N0270` if it is missing).
- A value of such a type cannot outlive what it borrows from (`N0243`), and a function cannot return one that
  borrows from its own locals (`N0244`).
- A function with one borrowed parameter returns a borrow of it. With several, the result names its sources:
  `borrow(a, b) T`, or a shared region (`N0245` if it does not).
- Regions cost nothing at run time.
- A struct that must outlive its source owns its data: `String`, `Vector<T>`.

Next: chapter 11, on testing.
