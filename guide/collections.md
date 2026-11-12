# Arrays, slices and collections

## Arrays

`[T; N]` is `N` values of `T`, inline: on the stack, or inside the struct that holds it. Arrays are values (copied
when assigned) and their element type must be Copy.

```ceesafe
fn main() {
    var squares = [0u32; 5];               // five zeros
    for (i in 0..squares.length) {
        squares[i] = truncate<u32>(i * i);    // usize to u32 could lose bits: say so
    }
    let primes = [2, 3, 5, 7];
    var sum = 0;
    for (p in primes) {
        sum += p;
    }
    Console.writeLine(squares[4]);
    Console.writeLine(sum);
}
```

```text
16
17
```

Every index is checked: `a[i]` with `i >= a.length` stops the program. The compiler removes the check where it can
prove the index is in range, as in both loops above.

## Slices

`Slice<T>` is a borrowed view of consecutive elements, and `SliceMut<T>` a borrowed view that can change them. They
are a pointer and a length, they follow every rule for references, and they are how functions take "some elements":

```ceesafe
fn total(values: Slice<i64>) -> i64 {
    var sum: i64 = 0;
    for (v in values) {
        sum += v;
    }
    return sum;
}

fn double(values: SliceMut<i64>) {
    for (i in 0..values.length) {
        values[i] *= 2;
    }
}

fn main() {
    var numbers = [1i64, 2, 3, 4, 5];
    double(mut numbers[1..3]);              // elements 1 and 2
    Console.writeLine(total(numbers[..]));
    Console.writeLine(total(numbers[3..]));
}
```

```text
20
9
```

`a[lo..hi]`, `a[lo..]`, `a[..hi]` and `a[..]` take elements `lo` up to `hi`; the range is checked. A slice borrows
the whole array it came from, so the array cannot change while the slice is used.

Slices also copy and fill in one checked operation (`d.copyFrom(s)`, `d.fill(v)`), swap elements, and sort and
search with closures (see [Functions and closures](closures.md)).

## Vectors

`Vector<T>` owns a growable list on the heap. Pushing can fail, so `push` returns a `Result`.

```ceesafe
fn main() {
    var v = Vector<i32>.create();          // allocates nothing yet
    for (i in 0..5) {
        v.push(i * 10)!;
    }
    Console.writeLine(v.length);
    Console.writeLine(v[2]);
    match (v.pop()) {                      // i32?: null when empty
        Some(last) => { Console.writeLine(last); }
        null => {}
    }
    v[0] = 99;
    var sum = 0;
    for (x in v) {
        sum += x;
    }
    Console.writeLine(sum);
}
```

```text
5
20
40
159
```

A vector can be passed where a `Slice<T>` is expected (it lends its elements), and `mut v` where a `SliceMut<T>` is.
`v.view()` and `v.viewMut()` give the slices explicitly. Other members: `capacity`, `clear()`, `truncate(n)`,
`swap(i, j)`, and `Vector<T>.withCapacity(n)`.

`v.clone()` makes a second vector with a copy of the elements, which must be Copy. It allocates, so it returns a
`Result`, and afterwards the two vectors have nothing to do with each other.

```ceesafe
fn main() {
    var first = Vector<i32>.create();
    first.push(1)!;
    first.push(2)!;
    var second = first.clone()!;           // Result<Vector<i32>, AllocError>
    second.push(3)!;
    first[0] = 10;
    Console.writeLine(first.length);
    Console.writeLine(second.length);
    Console.writeLine(second[0]);
}
```

```text
2
3
1
```

**Invalidation is a compile error.** Growing a vector may move its elements, so while any element or view of it is
borrowed, `push`, `pop`, `clear` and writes are rejected. This is the most common thing the borrow checker catches
in code that would crash in C++.

## Text: `StringView` and `String`

`StringView` is a read-only view of UTF-8 text: string literals are views, and so are slices of strings. `String`
owns growable text.

```ceesafe
fn greet(name: StringView) -> Result<String, AllocError> {
    var s = String.withCapacity(name.length + 7)?;
    s.append("Hello, ")?;
    s.append(name)?;
    s.append("! You are visitor ")?;
    s.appendNumber(42)?;
    return Ok(s);
}

fn main() {
    let message = greet("Ada")!;
    Console.writeLine(message);
    let word: StringView = "CeeSafe language";
    Console.writeLine(word[0..7]);
    Console.writeLine(word.length);
    Console.writeLine(word == "CeeSafe language");
}
```

```text
Hello, Ada! You are visitor 42
CeeSafe
16
true
```

- `length` is in bytes; `s[i]` is a byte (`u8`); `s.bytes()` is a `Slice<u8>`.
- Slicing (`s[lo..hi]`) checks that both ends fall on character boundaries.
- A `String` can only receive valid UTF-8 (`append` takes a view, which always is), so its contents are always valid
  text.
- `==` compares bytes, on views and on `String`s.

`String.from(text)` makes an owned string from a view in one call, and `s.clone()` copies a string. Both allocate,
so both return a `Result`; neither touches the text it copies.

```ceesafe
fn main() {
    var name = String.from("Ada")!;        // Result<String, AllocError>
    var other = name.clone()!;
    other.append(" Lovelace")!;
    name.append("!")!;
    Console.writeLine(name);
    Console.writeLine(other);
    let first = String.from(other[0..3])!; // from a part of another string
    Console.writeLine(first);
}
```

```text
Ada!
Ada Lovelace
Ada
```

Numbers in other forms go through lib/core's helpers, which a `String` and a `Vector<u8>` both have (and `VectorBytes`
as static functions): `appendHex(n, minWidth, upper)`, `appendOctal(n)`, `appendFixed(f, digits)`, rounded half to
even as Rust's `{:.3}` is, and `appendPadded(text, width, align, fill)` for a field of `width` characters.

```ceesafe
fn main() {
    var s = String.create();
    s.appendHex(48879, 8, true)!;
    s.append(" ")!;
    s.appendOctal(8)!;
    s.append(" ")!;
    s.appendFixed(2.675, 2)!;
    s.append(" [")!;
    s.appendPadded("id", 6, TextAlign.Right, ' ')!;
    s.append("]")!;
    Console.writeLine(s.view());
}
```

```text
0000BEEF 10 2.67 [    id]
```

`2.675` is `2.67499999…` in binary, so it rounds down, as it does in Rust and C.

## Stable references: `SlotMap` and `StableVector`

Sometimes values must refer to each other, as in a graph. A reference stored in a value would borrow the collection
for as long as the value lives. A `SlotMap` gives out **handles** instead: plain values that name an element, can be
stored anywhere, and are checked on every use, so a handle to a removed element finds nothing instead of something
else.

```ceesafe
struct Node {
    value: i32;
    next: Handle<Node>?;
}

fn main() {
    var nodes = SlotMap<Node>.create();
    let a = nodes.insert(Node { value: 1, next: null })!;
    let b = nodes.insert(Node { value: 2, next: a })!;
    match (nodes.get(b)) {
        Some(node) => { Console.writeLine(node.value); }
        null => {}
    }
    let removed = nodes.remove(a);
    Console.writeLine(nodes.get(a) == null);             // the handle is stale now
    Console.writeLine(nodes.length);
}
```

```text
2
true
1
```

`StableVector<T>` is a grow-only sequence whose elements never move, so an element can stay borrowed while more are
pushed.

## Maps: `HashMap`

```ceesafe
fn key(text: StringView) -> String {
    var s = String.create();
    s.append(text)!;
    return s;
}

fn main() {
    var ages = HashMap<String, u32>.create();
    ages.put(key("ada"), 36)!;
    ages.put(key("alan"), 41)!;
    let previous = ages.put(key("ada"), 37)!; // the old value: Some(36)
    let ada = key("ada");
    match (ages.get(borrow ada)) {
        Some(age) => { Console.writeLine(age); }
        null => { Console.writeLine("unknown"); }
    }
    for (i in 0..ages.length()) {
        let name = ages.keyAt(i);
        Console.writeLine(name.view());
    }
}
```

```text
37
ada
alan
```

- **Keys.** A key type implements `HashKey`; lib/std does for integers, `bool`, `String` and `Vector<u8>`.
- **The hash.** It is keyed by a random value chosen once per process, so a client cannot send keys that all collide.
- **Order.** Iteration follows insertion, so a program's output never depends on the hash. A removal moves the last
  entry into the removed one's place.

## Next

[Generics and interfaces](generics.md).
