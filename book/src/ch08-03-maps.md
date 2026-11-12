---
title: Hash maps and ordered maps
summary: HashMap and OrderedMap find a value by its key - put, get as an optional borrow, getMut to update, remove, iterating by position or by cursor, what a key type needs, and when to choose a hash map or an ordered map.
assumes: [ch04-02-borrowing, ch06-02-match, ch06-03-optionals, ch08-01-vectors, ch08-02-strings]
keywords: [HashMap, OrderedMap, put, get, getMut, remove, contains, keyAt, valueAt, cursor, seek, HashKey, OrderKey, derive]
diagnostics: [N0205, N0241, N0268]
---

# Hash maps and ordered maps

**This section teaches** the two maps of the standard library. A map keeps pairs of a key and a value, and finds
the value from the key. It assumes [match](./ch06-02-match), [optionals](./ch06-03-optionals),
[vectors](./ch08-01-vectors) and [strings](./ch08-02-strings).

- `HashMap<K, V>` finds a key by its hash. It keeps the pairs in the order they were put in.
- `OrderedMap<K, V>` keeps the pairs sorted by key.

Both are in lib/std, and a program uses them without an import. Their methods have the same names: `put`, `get`,
`getMut`, `contains`, `remove`, `clear`, `length()`.

## Putting and getting

<span class="listing-caption"><strong>Listing 8-23</strong>, <code>main.cee</code>: A map from a product number to the number in stock.</span>

```ceesafe
fn main() {
    var stock = HashMap<u32, i32>.create();
    stock.put(7, 10)!;
    stock.put(3, 4)!;
    Console.writeLine(stock.length());
    Console.writeLine(stock.contains(3));

    match (stock.get(7)) {
        Some(count) => { Console.writeLine(count); }
        null => { Console.writeLine("not stocked"); }
    }
    match (stock.get(8)) {
        Some(count) => { Console.writeLine(count); }
        null => { Console.writeLine("not stocked"); }
    }
}
```

```text
2
true
10
not stocked
```

- `HashMap<u32, i32>.create()` makes an empty map with `u32` keys and `i32` values.
- `put(key, value)` moves the key and the value into the map. It may allocate, so it returns a `Result`, as a
  vector's `push` does.
- `get(key)` returns an [optional](./ch06-03-optionals): a borrow of the value, or `null` if the key is not in
  the map. The `match` handles both.
- `contains(key)` is a `bool`.
- `length()` is the number of pairs. It is a method here, with `()`, where a vector's `length` is a field.

::: tip Rule
A lookup may find nothing, so `get` returns an optional. The compiler does not let you use the value without
saying what happens when it is `null`.
:::

When a missing key is an error in the program itself, `stock.get(7)!` takes the value and panics on `null`.

## Updating

<span class="listing-caption"><strong>Listing 8-24</strong>, <code>main.cee</code>: <code>put</code> replaces a value and returns the old one. <code>getMut</code> changes a value where it is.</span>

```ceesafe
fn main() {
    var stock = HashMap<u32, i32>.create();
    stock.put(7, 10)!;

    let before = stock.put(7, 12)!;
    Console.writeLine(before ?? -1);
    let fresh = stock.put(9, 1)!;
    Console.writeLine(fresh ?? -1);

    match (stock.getMut(7)) {
        Some(count) => { count -= 2; }
        null => {}
    }
    Console.writeLine(stock.get(7)!);
}
```

```text
10
-1
10
```

- `put` for a key that is already there replaces its value. After the `!`, its result is an optional: the old
  value, or `null` if the key is new.
- `getMut(key)` returns an optional exclusive borrow of the value. Writing to `count` changes the value in the
  map.

The usual "add one, or start at one" is a `getMut` with a `put` in its `null` arm; Listing 8-26 does that.

## Removing and iterating

<span class="listing-caption"><strong>Listing 8-25</strong>, <code>main.cee</code>: A pair is removed, and the others are visited by position.</span>

```ceesafe
fn main() {
    var stock = HashMap<u32, i32>.create();
    stock.put(7, 10)!;
    stock.put(3, 4)!;
    stock.put(9, 0)!;
    stock.put(5, 2)!;

    let removed = stock.remove(7);
    Console.writeLine(removed ?? -1);

    for (i in 0..stock.length()) {
        Console.write(stock.keyAt(i));
        Console.write(" -> ");
        Console.writeLine(stock.valueAt(i));
    }
}
```

```text
10
5 -> 2
3 -> 4
9 -> 0
```

- `remove(key)` takes the pair out and returns the value as an optional, `null` if the key was not there.
- `keyAt(i)` and `valueAt(i)` borrow the key and the value at position `i`, from 0 up to `length()`.

A `HashMap` keeps its pairs in the order they were put in, so the output of a program does not depend on the
hash. A removal changes that order in one way: the last pair moves into the place of the removed one. That is why
5 comes first above. `clear()` removes every pair.

## An ordered map, and `String` keys

A map owns its keys and its values. Text as a key is therefore a `String`, not a `StringView`: a view borrows
text, and the map would outlive it.

<span class="listing-caption"><strong>Listing 8-26</strong>, <code>main.cee</code>: Counting words. The ordered map gives them back sorted.</span>

```ceesafe
fn owned(text: StringView) -> String {
    var s = String.create();
    s.append(text)!;
    return s;
}

fn main() {
    var counts = OrderedMap<String, u32>.create();
    for (word in "the quick fox and the lazy dog and the end".split(" ")) {
        let key = owned(word);
        match (counts.getMut(key)) {
            Some(n) => { n += 1; }
            null => { counts.put(key, 1)!; }
        }
    }

    var at = counts.first();
    while (at.isSome()) {
        let entry = at!;
        Console.write(counts.keyAt(entry).view());
        Console.write(" ");
        Console.writeLine(counts.valueAt(entry));
        at = counts.next(entry);
    }
}
```

```text
and 2
dog 1
end 1
fox 1
lazy 1
quick 1
the 3
```

- `owned` makes a `String` from a view, because the map needs a key that it can own.
- `counts.getMut(key)` lends `key` for the lookup. In the `null` arm, `put` moves `key` into the map.
- An ordered map is visited with a **cursor**, a small value that names a position. `first()` and `last()` return
  an optional cursor, `null` for an empty map. `next(cursor)` and `previous(cursor)` return the one after or
  before, `null` at the end.
- `keyAt(cursor)` and `valueAt(cursor)` borrow the key and the value there. The key is a `String`, and
  `Console.write` prints its `view()`.

The `while` loop is the whole pattern for visiting an ordered map. Strings are ordered by their bytes, so capital
letters come before small ones.

A cursor is for reading a map that stays as it is. After a `put`, a `remove` or a `clear`, a cursor made before
it panics when it is used.

Because the keys are sorted, an ordered map can also start in the middle. `seek(key)` is a cursor at the first
key that is not before `key`, and `seekAfter(key)` at the first key after it.

<span class="listing-caption"><strong>Listing 8-27</strong>, <code>main.cee</code>: The readings from day 10 onward, in order.</span>

```ceesafe
fn main() {
    var readings = OrderedMap<u32, i32>.create();
    readings.put(20, 5)!;
    readings.put(3, 1)!;
    readings.put(11, 9)!;

    var at = readings.seek(10);
    while (at.isSome()) {
        let entry = at!;
        Console.write(readings.keyAt(entry));
        Console.write(": ");
        Console.writeLine(readings.valueAt(entry));
        at = readings.next(entry);
    }
}
```

```text
11: 9
20: 5
```

## What a key needs

A `HashMap` key must be hashed and compared for equality: its type implements the interface `HashKey`. An
`OrderedMap` key must be compared for order: its type implements `OrderKey`. lib/std implements both for the
integer types, `bool`, `String` and `Vector<u8>`. Floats are not keys.

For a struct of your own, `@derive` writes the implementations from the fields.
[Chapter 10](./ch10-02-interfaces) covers interfaces, and chapter 20 covers derives.

<span class="listing-caption"><strong>Listing 8-28</strong>, <code>main.cee</code>: A struct becomes a key of both maps.</span>

```ceesafe
@derive(Equal, HashKey, OrderKey)
struct Point {
    x: i32;
    y: i32;
}

fn main() {
    var visits = HashMap<Point, u32>.create();
    visits.put(Point { x: 1, y: 2 }, 5)!;
    Console.writeLine(visits.get(Point { x: 1, y: 2 })!);

    var sorted = OrderedMap<Point, u32>.create();
    sorted.put(Point { x: 1, y: 2 }, 5)!;
    sorted.put(Point { x: 0, y: 9 }, 7)!;
    Console.writeLine(sorted.valueAt(sorted.first()!));
}
```

```text
5
7
```

`Equal` gives the struct `==`, which `HashKey` needs. `OrderKey` orders by the fields in the order they are
declared: `x` first, then `y`.

## Which map

| | `HashMap` | `OrderedMap` |
|---|---|---|
| A key needs | `HashKey` | `OrderKey` |
| Order of iteration | as put in (a removal moves the last pair) | sorted by key |
| Iterating | `keyAt(i)`, `valueAt(i)` for `0..length()` | cursors: `first`, `next`, `seek` |
| Smallest, largest, a range of keys | no | `first`, `last`, `seek`, `seekAfter` |
| How it finds a key | one hash, then a few comparisons | a search down a tree |

Choose a `HashMap` unless you need the keys in order. Choose an `OrderedMap` for sorted output, for the smallest
or largest key, or for "every key from here to there".

## Mistakes

::: warning Mistake: a plain value as the fallback of `get`
`get` returns an optional borrow of the value, not an optional value. `?? -1` offers a value where a borrow is
expected.
:::

<span class="listing-caption"><strong>Listing 8-29</strong>, <code>main.cee</code>: The fallback is an <code>i32</code>, and the optional holds a borrow. This does not compile.</span>

<!-- ceesafe: fail N0205 -->
```ceesafe
fn main() {
    var stock = HashMap<u32, i32>.create();
    stock.put(7, 10)!;
    Console.writeLine(stock.get(8) ?? -1);
}
```

```text
main.cee:4:39: error[N0205]: expected 'borrow i32', found 'i32'
  |
4 |     Console.writeLine(stock.get(8) ?? -1);
  |                                       ^^
```

Use a `match`, as Listing 8-23 does. `remove` and `put` return the value itself, so `??` works on their results.

::: warning Mistake: a struct as a key without its derives
A map does not know how to hash or order a type of yours until you say so.
:::

<span class="listing-caption"><strong>Listing 8-30</strong>, <code>main.cee</code>: <code>Point</code> has no <code>HashKey</code>. This does not compile.</span>

<!-- ceesafe: fail N0268 -->
```ceesafe
struct Point {
    x: i32;
    y: i32;
}

fn main() {
    var visits = HashMap<Point, u32>.create();
    visits.put(Point { x: 1, y: 2 }, 5)!;
}
```

```text
main.cee:7:18: error[N0268]: 'Point' does not implement 'HashKey<Point>', which 'K' requires
  |
7 |     var visits = HashMap<Point, u32>.create();
  |                  ^^^^^^^^^^^^^^^^^^^
```

Add `@derive(Equal, HashKey)` above the struct, as in Listing 8-28.

::: warning Mistake: keeping a borrowed value across a `put`
A `put` may move every pair, as a vector's `push` moves its elements. A value borrowed with `get` cannot be used
after it.
:::

<span class="listing-caption"><strong>Listing 8-31</strong>, <code>main.cee</code>: <code>seven</code> borrows from the map, and is used after a <code>put</code>. This does not compile.</span>

<!-- ceesafe: fail N0241 -->
```ceesafe
fn main() {
    var stock = HashMap<u32, i32>.create();
    stock.put(7, 10)!;
    let seven = stock.get(7);
    stock.put(8, 1)!;
    Console.writeLine(seven!);
}
```

```text
main.cee:5:5: error[N0241]: cannot borrow 'stock' as mutable while 'stock' is borrowed
  |
5 |     stock.put(8, 1)!;
  |     ^^^^^
  = note: 'stock' is borrowed here (4:17)
  = note: the borrow is used later here (6:23)
```

Look the value up again after the `put`, or copy it out before it by giving the variable the value's type:
`let seven: i32 = stock.get(7)!;`. Without the type, `seven` is still a borrow.

::: details Coming from Rust
`HashMap` and `OrderedMap` are `HashMap` and `BTreeMap`. `put` is `insert` and returns the old value, inside a
`Result`. There is no `entry` API: use `getMut`, and `put` in the `null` arm. A `HashMap` iterates in insertion
order, as `IndexMap` does, and its lookups take `borrow K`, so a `String` map is looked up with a `String`.
:::

::: details Coming from C#, Java, TypeScript, Go or Python
`map[key]` is not written with brackets: use `get`, `getMut` and `put`. A missing key is `null` in an optional
that the compiler makes you handle, never an exception or a zero value. The map owns its keys and values: putting
a `String` in moves it.
:::

## Summary

- `HashMap<K, V>.create()` and `OrderedMap<K, V>.create()` make empty maps. `length()` is a method.
- `put(key, value)!` adds or replaces, and returns the old value as an optional.
- `get(key)` is an optional borrow of the value (`N0205` if you give it a plain fallback); `getMut(key)` lets you
  change it; `contains(key)` is a `bool`.
- `remove(key)` returns the value as an optional.
- A `HashMap` iterates by position in insertion order; an `OrderedMap` iterates by cursor in key order.
- A key type implements `HashKey` or `OrderKey` (`N0268`): integers, `bool`, `String`, `Vector<u8>`, and structs
  with `@derive(Equal, HashKey, OrderKey)`.
- While a value is borrowed from a map, the map cannot change (`N0241`).

Next: [chapter 9](./ch09-00-error-handling), which explains the `Result` behind every `!` in this chapter.
