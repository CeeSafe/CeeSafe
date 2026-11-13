---
title: Iteration
summary: Everything a for loop accepts - ranges, arrays, slices, vectors and iterators - how to write an iterator with a next method, iterators in generic code, and the slice and vector methods that take a closure, since there is no adapter library.
assumes: [ch03-05-control-flow, ch06-03-optionals, ch08-01-vectors, ch10-02-interfaces, ch13-01-closures]
keywords: [for, iteration, iterator, next, Iterator, range, split, sortBy, forEach, any, all, partitionPoint, retain]
diagnostics: [N0200, N0205, N0241, N0251]
specification: ["type-system.md §16.4", "type-system.md §36.4"]
---

# Iteration

**This section teaches** what a `for` loop can run over, how to make a type of your own work in a `for` loop,
and which library methods visit a sequence with a closure. It assumes the `for` loop of
[Control flow](./ch03-05-control-flow), optionals ([Optionals](./ch06-03-optionals)), vectors
([Vectors](./ch08-01-vectors)), interfaces and bounds ([Interfaces](./ch10-02-interfaces)) and
[Closures](./ch13-01-closures).

## What `for` accepts

`for (x in e) { … }` runs its body once for each value of `e`. There are three kinds of `e`: a range, a
sequence, and an iterator.

<span class="listing-caption"><strong>Listing 13-10</strong>, <code>main.cee</code>: One loop each over a range, a slice, a vector, the pieces of a text, and the positions of a map.</span>

```ceesafe
fn main() {
    for (i in 0..2) {
        Console.writeLine(i);
    }

    let scores = [70, 85, 92];
    for (s in scores[1..]) {
        Console.writeLine(s);
    }

    var names = Vector<String>.create();
    names.push(String.from("ada")!)!;
    names.push(String.from("grace")!)!;
    for (name in names) {
        Console.writeLine(name.view());
    }

    for (word in "red,green".split(",")) {
        Console.writeLine(word);
    }

    var stock = HashMap<u32, i32>.create();
    stock.put(7, 10)!;
    stock.put(3, 4)!;
    for (i in 0..stock.length()) {
        Console.write(stock.keyAt(i));
        Console.write(" = ");
        Console.writeLine(stock.valueAt(i));
    }
}
```

```text
0
1
85
92
ada
grace
red
green
7 = 10
3 = 4
```

- **A range** `lo..hi` gives each integer from `lo` up to but not including `hi`. Both ends are evaluated
  once, before the first pass. If `lo` is not below `hi` the body never runs: a range does not count down.
- **A sequence** is an array, a slice or a vector. `x` is each element from index 0 upward. If the element
  type is Copy, `x` is a copy. If it is not, as with `String` above, `x` is a shared borrow of the element in
  its place, so the loop does not empty the vector.
- **An iterator** is the third kind, and the next part explains it. `text.split(",")` returns one.
- A `HashMap` is none of the three. Visit it by position, with a range up to `length()`.

The loop variable cannot be assigned in the body, and `break` and `continue` work as in `while`.

::: tip Rule
`for (x in e)` accepts a range `a..b`, an array, a slice, a vector, and an iterator. While the loop runs over
a sequence or an iterator, what it borrows cannot be changed.

*Specification: `type-system.md` §16.4.*
:::

## Writing an iterator

An iterator is a struct with a method of this shape:

```text
fn next(borrow mut this) -> T?
```

Each call returns the next value as `Some`, or `null` when there are no more. The `for` loop calls `next`
before each pass and stops at `null`. No interface is needed; the shape of the method is enough.

<span class="listing-caption"><strong>Listing 13-11</strong>, <code>main.cee</code>: A struct with a <code>next</code> method runs in a <code>for</code> loop, by value and through a <code>borrow mut</code>.</span>

```ceesafe
/// Counts down from a number to 1.
struct Countdown {
    left: u32;
}

impl Countdown {
    fn next(borrow mut this) -> u32? {
        if (this.left == 0) {
            return null;
        }
        let value = this.left;
        this.left -= 1;
        return value;
    }
}

fn main() {
    for (n in Countdown { left: 3 }) {
        Console.writeLine(n);
    }
    var rest = Countdown { left: 5 };
    for (n in borrow mut rest) {
        if (n == 3) {
            break;
        }
        Console.writeLine(n);
    }
    Console.writeLine(rest.next() ?? 0);
}
```

```text
3
2
1
5
4
2
```

The first loop takes the iterator by value: it moves into the loop and is destroyed when the loop ends. The
second loop takes `borrow mut rest`, so `rest` keeps its state: after the `break` at 3, the next value is 2.

An iterator does its work when `next` is called and not before. `Countdown { left: 1000000 }` holds one
number, not a million.

## Iterators in generic code

The library declares the shape as an interface, `Iterator<T>`, with the one method `next`. A type that says
`impl Iterator<T> for …` can be passed to generic code with the bound `I: Iterator<T>`, and a `for` loop
accepts a value of such a type parameter. An iterator may hold another iterator, and that is how you build
what other languages call an adapter.

<span class="listing-caption"><strong>Listing 13-12</strong>, <code>main.cee</code>: <code>sum</code> adds up any iterator of <code>u32</code>, and <code>Filter</code> is an iterator over another one.</span>

```ceesafe
struct Countdown {
    left: u32;
}

impl Iterator<u32> for Countdown {
    fn next(borrow mut this) -> u32? {
        if (this.left == 0) {
            return null;
        }
        let value = this.left;
        this.left -= 1;
        return value;
    }
}

fn sum<I: Iterator<u32>>(values: I) -> u32 {
    var total: u32 = 0;
    for (v in values) {
        total += v;
    }
    return total;
}

/// The values of `inner` that pass `keep`.
struct Filter<region r, I: Iterator<u32>> {
    inner: I;
    keep: borrow(r) fn(u32) -> bool;
}

impl<region r, I: Iterator<u32>> Iterator<u32> for Filter<r, I> {
    fn next(borrow mut this) -> u32? {
        loop {
            match (this.inner.next()) {
                Some(v) => {
                    if (this.keep(v)) {
                        return v;
                    }
                }
                null => { return null; }
            }
        }
    }
}

fn main() {
    Console.writeLine(sum(Countdown { left: 4 }));
    let odd = Filter { inner: Countdown { left: 6 }, keep: (v) => v % 2 == 1 };
    Console.writeLine(sum(odd));
}
```

```text
10
9
```

`Filter.next` asks `inner` for values until one passes the closure in its `keep` field. The library has two
iterators, both made by `split`: `TextSplit` for a `StringView` and `ByteSplit` for a `Slice<u8>`. It has no
adapters such as `Filter`: you write the ones you need, or you write the loop.

## Methods that take a closure

Slices and vectors have six methods that visit their elements with a closure. An array has them through its
slice, `a[..]`.

<span class="listing-caption"><strong>Listing 13-13</strong>, <code>main.cee</code>: A vector of structs is sorted, visited, tested, searched and filtered.</span>

```ceesafe
struct Reading {
    sensor: u32;
    level: i32;
}

fn main() {
    var readings = Vector<Reading>.create();
    let levels = [62, 17, 88, 45, 30];
    for (i in 0..levels.length) {
        readings.push(Reading { sensor: truncate<u32>(i), level: levels[i] })!;
    }

    readings.sortBy((a, b) => a.level - b.level);
    readings.forEach((r) => { Console.writeLine(r.level); });

    Console.writeLine(readings.any((r) => r.level > 80));
    Console.writeLine(readings.all((r) => r.level > 20));
    Console.writeLine(readings.partitionPoint((r) => r.level < 50));

    readings.retain((r) => r.sensor % 2 == 0);
    Console.writeLine(readings.length);
}
```

```text
17
30
45
62
88
true
false
3
3
```

| Method | The closure | What it does |
|---|---|---|
| `s.sortBy(cmp)` | `(a, b) => i32` | sorts in place; `cmp` is negative, zero or positive as `a` goes before, with or after `b`; not stable |
| `s.forEach(visit)` | `(x) => void` | calls `visit` on each element in order |
| `s.any(test)` | `(x) => bool` | `true` at the first element that passes; `false` if none or empty |
| `s.all(test)` | `(x) => bool` | `false` at the first element that fails; `true` if none or empty |
| `s.partitionPoint(test)` | `(x) => bool` | a binary search: the index of the first element that fails, in a sequence where all that pass come first |
| `v.retain(keep)` | `(x) => bool` | vectors only: keeps the elements that pass, in order, and destroys the others |

The closure's parameters are shared borrows of the elements, so the element type can be any type. Each
closure parameter is a `borrow mut fn`, so the closure may change what it captures, as a `forEach` that adds
to a total does. The receiver is lent to the method for the whole call, so the closure cannot use the same
vector.

::: tip Rule
The library's closure-taking methods are `sortBy`, `forEach`, `any`, `all` and `partitionPoint` on slices and
vectors, and `retain` on vectors. There are no others: no `map`, `filter`, `fold` or `sum`.

*Specification: `type-system.md` §36.4.*
:::

## Mistakes

::: warning Mistake: changing a vector inside a loop over it
The loop borrows the vector until it ends.
:::

<span class="listing-caption"><strong>Listing 13-14</strong>, <code>main.cee</code>: The body pushes onto the vector the loop is visiting. This does not compile.</span>

<!-- ceesafe: fail N0241 -->
```ceesafe
fn main() {
    var v = Vector<i32>.create();
    v.push(1)!;
    v.push(2)!;
    for (x in v) {
        if (x == 1) {
            v.push(3)!;
        }
    }
    Console.writeLine(v.length);
}
```

```text
main.cee:7:13: error[N0241]: cannot borrow 'v' as mutable while 'v' is borrowed
  |
7 |             v.push(3)!;
  |             ^
  = note: 'v' is borrowed here (5:15)
  = note: the borrow is used later here (5:5)
```

Loop over the indexes instead: `for (i in 0..v.length)` reads the length once, before the first pass, and
borrows nothing between passes, so the body may push. To remove elements, use `retain`.

::: warning Mistake: a `next` of another shape
The method must take `borrow mut this` and return an optional.
:::

<span class="listing-caption"><strong>Listing 13-15</strong>, <code>main.cee</code>: This <code>next</code> takes a shared borrow and returns a plain <code>u32</code>. This does not compile.</span>

<!-- ceesafe: fail N0205 -->
```ceesafe
struct Countdown {
    left: u32;
}

impl Countdown {
    fn next(borrow this) -> u32 {
        return this.left;
    }
}

fn main() {
    for (n in Countdown { left: 3 }) {
        Console.writeLine(n);
    }
}
```

```text
main.cee:12:15: error[N0205]: 'for' iterates over an array, a slice, a vector, a range 'a..b' or an iterator; the method 'next' of 'Countdown' has the wrong shape; an iterator has 'fn next(borrow mut this) -> T?'
   |
12 |     for (n in Countdown { left: 3 }) {
   |               ^^^^^^^^^^^^^^^^^^^^^
```

The same code, with "has no method 'next'", reports a `for` over a value that is not iterable, such as a
`HashMap`.

::: warning Mistake: a chain of adapters
`map`, `filter` and `sum` do not exist.
:::

<span class="listing-caption"><strong>Listing 13-16</strong>, <code>main.cee</code>: A chain as another language writes it. This does not compile.</span>

<!-- ceesafe: fail N0200 -->
```ceesafe
fn main() {
    let total = [1, 2, 3].map((x) => x * 2).filter((x) => x > 2).sum();
    Console.writeLine(total);
}
```

```text
main.cee:2:27: error[N0200]: 'Slice<i32>' has no method 'map'; slices have 'copyFrom', 'fill', 'swap', 'sortBy', 'forEach', 'any', 'all', 'partitionPoint', 'splitAt' and 'splitAtMut'
  |
2 |     let total = [1, 2, 3].map((x) => x * 2).filter((x) => x > 2).sum();
  |                           ^^^
```

Write the loop. The steps of the chain become the lines of its body:

```ceesafe
fn main() {
    var total = 0;
    for (x in [1, 2, 3]) {
        if (x * 2 > 2) {
            total += x * 2;
        }
    }
    Console.writeLine(total);
}
```

```text
10
```

::: warning Mistake: taking a non-Copy element out of the loop variable
For a `String` element the loop variable is a borrow, and a borrow cannot give the value away.
:::

<span class="listing-caption"><strong>Listing 13-17</strong>, <code>main.cee</code>: The body tries to move each name into another vector. This does not compile.</span>

<!-- ceesafe: fail N0251 -->
```ceesafe
fn main() {
    var names = Vector<String>.create();
    names.push(String.from("ada")!)!;
    names.push(String.from("grace")!)!;
    var kept = Vector<String>.create();
    for (name in names) {
        kept.push(name)!;
    }
    Console.writeLine(kept.length);
}
```

```text
main.cee:7:9: error[N0251]: cannot move a 'String' out of the value 'name' refers to; only Copy values can be read through a reference
  |
7 |         kept.push(name)!;
  |         ^^^^^^^^^^^^^^^
```

Push a copy, `kept.push(name.clone()!)!`. An iterator is different: the value its `next` returns belongs to
the loop variable, also when its type is not Copy.

::: details Coming from Rust
- There is no `IntoIterator`, no `iter()`, `iter_mut()` or `into_iter()`: `for (x in v)` borrows the vector,
  and gives copies of Copy elements and shared borrows of others.
- `Iterator<T>` has its item as a type parameter, not an associated type, and only the method `next`. There
  are no provided methods: no `map`, `filter`, `enumerate`, `zip`, `collect`.
- `a..b` is not a value: it is part of the `for` syntax. There is no `..=`, `rev()` or `step_by()`.
:::

::: details Coming from Python, C#, Java or TypeScript
A `for` loop over a collection does not make an iterator object: it indexes the elements in place. There are
no generators and no `yield`: an iterator is a struct that keeps its own position in its fields, as
`Countdown` does.
:::

## Summary

- `for (x in e)` runs over a range, an array, a slice, a vector, or an iterator. A `HashMap` is visited by
  position.
- Over a sequence, `x` is a copy of a Copy element and a shared borrow of any other (`N0251` if you try to
  move it out). The sequence cannot change during the loop (`N0241`).
- An iterator is a struct with `fn next(borrow mut this) -> T?` (`N0205` for another shape). Passed by value
  it is used up; passed as `borrow mut` it keeps its state.
- `impl Iterator<T> for …` lets generic code take it with a bound `I: Iterator<T>`.
- The closure-taking methods are `sortBy`, `forEach`, `any`, `all`, `partitionPoint` and `retain`. There is
  no adapter library (`N0200`): write the loop, or an iterator of your own.

Next: [The project improved](./ch13-03-project-improved) uses a closure and an iterator in `lookfor`.
