---
title: Slices and views
summary: A slice borrows part of an array or a vector - Slice reads, SliceMut writes, made with a[lo..hi] - and a StringView borrows text. Indexes are checked, the collection cannot change while a slice is live (N0241), and a view of a temporary ends with its statement (N0243).
assumes: [ch04-01-what-ownership-is, ch04-02-borrowing]
keywords: [Slice, SliceMut, StringView, slice, view, range, "a[lo..hi]", "borrow mut", index, bounds check, temporary, invalidation]
diagnostics: [N0241, N0243]
specification: ["type-system.md §16.1", "type-system.md §22.2", "lifetimes.md §6"]
---

# Slices and views

**This section teaches** how to borrow part of an array or a vector, and how to borrow text. It assumes
[What ownership is](./ch04-01-what-ownership-is) and [Borrowing](./ch04-02-borrowing): a slice is a borrow, and
the rule for borrows is the rule for slices.

A function that adds up numbers should work on a whole array, on its first three elements, and on a list that
grows. It needs "some elements, in a row, that belong to someone else". That is a **slice**:

- `Slice<T>` is a shared borrow of consecutive elements. It can read them.
- `SliceMut<T>` is an exclusive borrow of consecutive elements. It can read and write them.

A slice is two numbers: where the first element is, and how many there are. It owns nothing, and making one copies
no element.

## Slicing an array

<span class="listing-caption"><strong>Listing 4-12</strong>, <code>main.cee</code>: One function adds up any run of elements.</span>

```ceesafe
fn sum(values: Slice<i32>) -> i32 {
    var total = 0;
    for (v in values) {
        total += v;
    }
    return total;
}

fn main() {
    let a = [10, 20, 30, 40, 50];
    let middle = a[1..4];
    Console.writeLine(middle.length);
    Console.writeLine(middle[0]);
    Console.writeLine(sum(middle));
    Console.writeLine(sum(a[..2]));
    Console.writeLine(sum(a[3..]));
    Console.writeLine(sum(a[..]));
}
```

```text
3
20
90
30
90
150
```

- `a[1..4]` is a slice of elements 1, 2 and 3: the range includes its start and excludes its end.
- `a[..2]` starts at the beginning, `a[3..]` runs to the end, and `a[..]` is all of `a`.
- A slice has a `length`, is indexed from 0 (`middle[0]` is `a[1]`), and can be walked with `for`.
- An array is not a slice: to pass a whole array to `sum`, write `sum(a[..])`.

## Slices that write

A range of a variable gives a `Slice<T>`. For a `SliceMut<T>`, say so: `mut a[lo..hi]` as an argument, as for any
`borrow mut` parameter, or `borrow mut a[lo..hi]` elsewhere.

<span class="listing-caption"><strong>Listing 4-13</strong>, <code>main.cee</code>: A function sets two elements to zero through a <code>SliceMut</code>, and <code>main</code> writes one through another.</span>

```ceesafe
fn clear(values: SliceMut<i32>) {
    for (i in 0..values.length) {
        values[i] = 0;
    }
}

fn main() {
    var a = [10, 20, 30, 40, 50];
    clear(mut a[1..3]);
    let tail = borrow mut a[3..];
    tail[1] = 99;
    for (x in a) {
        Console.writeLine(x);
    }
}
```

```text
10
0
0
40
99
```

`a` is a `var`, because it changes. The variable that holds a slice is a `let`: a slice is a borrow, and a borrow
is not pointed somewhere else after it is made.

## Slicing a vector

A `Vector<T>` is a list that grows; `push` adds an element, and its `!` stops the program if there is no memory
([chapter 8](./ch08-01-vectors) covers vectors). A vector is sliced as an array is, and it can also be passed
whole where a `Slice<T>` is expected.

<span class="listing-caption"><strong>Listing 4-14</strong>, <code>main.cee</code>: The same <code>sum</code> takes a vector, and part of one.</span>

```ceesafe
fn sum(values: Slice<i32>) -> i32 {
    var total = 0;
    for (v in values) {
        total += v;
    }
    return total;
}

fn main() {
    var v = Vector<i32>.create();
    v.push(1)!;
    v.push(2)!;
    v.push(3)!;
    Console.writeLine(sum(v));
    Console.writeLine(sum(v[1..]));
    v.push(4)!;
    Console.writeLine(sum(v));
}
```

```text
6
5
10
```

Each slice here lasts for one call. When the call returns the borrow is over, and `v` may grow again.

::: tip Rule
A slice borrows the whole array or vector it was taken from, not only the elements in its range. While a
`Slice<T>` is live the collection cannot change; while a `SliceMut<T>` is live the collection cannot be used
except through it.

*Specification: `type-system.md` §16.1.*
:::

## Views of text

A `StringView` is a shared borrow of text. A string literal is one, a range of a `StringView` is another, and a
`String` lends its text where a `StringView` is expected.

<span class="listing-caption"><strong>Listing 4-15</strong>, <code>main.cee</code>: <code>firstWord</code> returns a view of part of the text it was given.</span>

```ceesafe
fn firstWord(text: StringView) -> StringView {
    for (i in 0..text.length) {
        if (text[i] == ' ') {
            return text[..i];
        }
    }
    return text;
}

fn main() {
    let line = "hello wide world";
    Console.writeLine(firstWord(line));
    Console.writeLine(line[6..10]);

    let s = String.from("owned text")!;
    let word = firstWord(s);
    Console.writeLine(word);
    Console.writeLine(word.length);
}
```

```text
hello
wide
owned
5
```

`String.from("owned text")!` makes a `String` that owns a copy of the literal's text
([What ownership is](./ch04-01-what-ownership-is#move-types)). `firstWord` copies no text. Its result is a view into the text of its argument, and the compiler knows that:
`word` borrows `s`, so `s` cannot be changed or destroyed while `word` is used.

The `length` of a view counts bytes, and `text[i]` is a byte. Text is UTF-8, in which a character can take more
than one byte; a range must begin and end between characters. [Chapter 8](./ch08-02-strings) covers text.

A function that only reads text should take `StringView`, not `borrow String`: it then accepts literals, `String`s
and parts of either.

## Indexes are checked

A slice knows its length, and every index and every range is checked against it when the program runs. An index
outside the slice stops the program with a panic. It never reads the memory beside the slice.

<span class="listing-caption"><strong>Listing 4-16</strong>, <code>main.cee</code>: The slice has two elements. The second call asks for a third, and the program panics.</span>

<!-- ceesafe: panics -->
```ceesafe
fn at(values: Slice<i32>, i: usize) -> i32 {
    return values[i];
}

fn main() {
    let a = [10, 20, 30];
    Console.writeLine(at(a[..2], 1));
    Console.writeLine(at(a[..2], 2));
}
```

```text
20
```

After the `20`, the program writes this to standard error and exits with status 134:

```text
ceesafe panic: index out of bounds at main.cee:2:19
```

The array has an element 2, but the slice `a[..2]` does not: the check is against the slice. A range is checked in
the same way (`a[2..9]` panics with `slice range out of bounds`), and so is a range of text that would cut a
character in two.

::: tip Rule
Indexes are `usize`. `s[i]` with `i >= s.length` panics, and so does a range that does not fit.
:::

## Mistakes

::: warning Mistake: changing a collection while a slice of it is live
A vector that grows may move its elements to a larger piece of memory. A slice made before would then point at the
old one. In C++ this compiles and fails at run time; in CeeSafe the borrow rule rejects it.
:::

<span class="listing-caption"><strong>Listing 4-17</strong>, <code>main.cee</code>: <code>s</code> is used after <code>v</code> grows. This does not compile.</span>

<!-- ceesafe: fail N0241 -->
```ceesafe
fn main() {
    var v = Vector<i32>.create();
    v.push(1)!;
    v.push(2)!;
    let s = v[..];
    v.push(3)!;
    Console.writeLine(s[0]);
}
```

```text
main.cee:6:5: error[N0241]: cannot borrow 'v' as mutable while 'v' is borrowed
  |
6 |     v.push(3)!;
  |     ^
  = note: 'v' is borrowed here (5:5)
  = note: the borrow is used later here (7:25)
```

`push` changes `v`, so it borrows `v` exclusively, and the shared borrow `s` is still live: it is used on line 7.
The notes give the slice (5:5) and that later use (7:25). To fix it, take the slice after the last change, or copy
out what you need before it: `let first = v[0];` copies an `i32` and borrows nothing. Assigning to an element
while a slice is live is refused in the same way, with `N0240`.

::: warning Mistake: a view of a temporary
A value that is made in an expression and not given a name is a temporary. It is destroyed at the end of its
statement, so a view of it cannot be kept longer than that.
:::

<span class="listing-caption"><strong>Listing 4-18</strong>, <code>main.cee</code>: <code>v</code> would be a view of a <code>String</code> that is already destroyed. This does not compile.</span>

<!-- ceesafe: fail N0243 -->
```ceesafe
fn make() -> String {
    return String.from("made for a moment")!;
}

fn main() {
    let v = make().view();
    Console.writeLine(v);
}
```

```text
main.cee:6:5: error[N0243]: a temporary value is dropped at the end of this statement while still borrowed; bind it to a local first
  |
6 |     let v = make().view();
  |     ^^^^^^^^^^^^^^^^^^^^^^
  = note: a temporary value is borrowed here (6:5)
  = note: the borrow is used later here (7:5)
```

`view()` gives the `StringView` of a `String`. Nothing owns the `String` that `make()` returns, so it is destroyed
at the `;` of line 6. Give it an owner, as the message says, or use the view inside the one statement:

<span class="listing-caption"><strong>Listing 4-19</strong>, <code>main.cee</code>: A view used within its statement, and a view of a value that has an owner.</span>

```ceesafe
fn make() -> String {
    return String.from("made for a moment")!;
}

fn main() {
    Console.writeLine(make().view());
    let kept = make();
    let v = kept.view();
    Console.writeLine(v);
}
```

```text
made for a moment
made for a moment
```

::: details Coming from Rust
`Slice<T>` is `&[T]`, `SliceMut<T>` is `&mut [T]` and `StringView` is `&str`. `a[1..4]` is already the slice: there
is no `&a[1..4]`. The exclusive one is `borrow mut a[1..4]`, or `mut a[1..4]` as an argument. An array does not
turn into a slice by itself (`sum(a[..])`), but a vector and a `String` do.
:::

::: details Coming from Go, Python, C# or TypeScript
A Go slice or a C# `Span<T>` is the nearest thing. A Python or JavaScript `a[1:4]` or `a.slice(1, 4)` makes a new
list with copies; a CeeSafe slice copies nothing and is only valid while the collection stays as it is. A string
literal has the type `StringView`, not `String`.
:::

::: details Coming from C
A slice is a pointer and a length that travel together, and every index is checked against that length. It cannot
outlive the array, and the array cannot be reallocated while the slice is used.
:::

## Summary

- `Slice<T>` is a shared borrow of consecutive elements; `SliceMut<T>` is an exclusive one. `StringView` is a
  shared borrow of text.
- `a[lo..hi]`, `a[..hi]`, `a[lo..]` and `a[..]` make a `Slice<T>`; `borrow mut a[lo..hi]`, or `mut a[lo..hi]` as an
  argument, makes a `SliceMut<T>`.
- A vector or a `String` can be passed where a slice or a view is expected; an array is passed as `a[..]`.
- Indexes and ranges are checked at run time; one that does not fit panics.
- A slice borrows its whole collection, which cannot change while the slice is live (`N0241`, `N0240`).
- A view of a temporary lasts to the end of its statement (`N0243`): give the value a name to keep the view.

Next: [chapter 5](./ch05-00-structs), on structs, which group values under one owner.
