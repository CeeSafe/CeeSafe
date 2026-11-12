---
title: Vectors
summary: Vector<T>, a growable list on the heap - making one, push and its Result, checked indexing, length and capacity, iterating, pop, clear and resize, lending the elements as a slice, the closure-taking methods, and why a vector cannot grow while an element is borrowed.
assumes: [ch04-02-borrowing, ch04-03-slices-and-views, ch06-03-optionals]
keywords: [Vector, push, pop, length, capacity, withCapacity, clear, resize, truncate, view, viewMut, Slice, sortBy, retain, partitionPoint, index out of bounds]
diagnostics: [N0205, N0241, N0251]
specification: ["type-system.md §23.1"]
---

# Vectors

**This section teaches** `Vector<T>`: a list of values of one type that lives on the heap and grows at its end. It
assumes [borrowing](./ch04-02-borrowing), [slices](./ch04-03-slices-and-views) and
[optionals](./ch06-03-optionals).

## Making a vector and adding to it

<span class="listing-caption"><strong>Listing 8-1</strong>, <code>main.cee</code>: A vector is made empty, and three values are pushed onto its end.</span>

```ceesafe
fn main() {
    var scores = Vector<i32>.create();
    scores.push(70)!;
    scores.push(85)!;
    scores.push(92)!;
    Console.writeLine(scores.length);
    Console.writeLine(scores[1]);
}
```

```text
3
85
```

- `Vector<i32>.create()` makes an empty vector of `i32`. It asks for no memory yet.
- `scores` is a `var`, because pushing changes it.
- `scores.push(70)` adds a value after the last one.
- `scores.length` is the number of elements. It is a field, with no `()`, and its type is `usize`.
- `scores[1]` is the second element: indexes start at 0.

## Why `push` returns a `Result`

A push may need more memory, and the system can refuse. So `push` returns a `Result`: success, or an `AllocError`.
The `!` after the call takes the success and stops the program on a failure.
[Chapter 9](./ch09-00-error-handling) covers `Result`, and `?`, which passes the failure to the caller instead.

::: tip Rule
Every method that may grow a vector returns a `Result`: `push`, `resize`, `withCapacity`. Write `!` or `?` after
the call. Methods that only shrink it or read it (`pop`, `clear`, an index) return no `Result`.

*Specification: `type-system.md` §23.1.*
:::

## Length and capacity

A vector keeps room for more elements than it holds. `length` is how many it holds; `capacity` is how many it has
room for. When a push finds the vector full, the vector moves to a piece of memory twice as large.

<span class="listing-caption"><strong>Listing 8-2</strong>, <code>main.cee</code>: The capacity grows in steps, and <code>withCapacity</code> asks for the room at the start.</span>

```ceesafe
fn main() {
    var squares = Vector<i32>.create();
    Console.writeLine(squares.capacity);
    for (i in 0..5) {
        squares.push(i * i)!;
    }
    Console.writeLine(squares.length);
    Console.writeLine(squares.capacity);

    var bytes = Vector<u8>.withCapacity(100)!;
    Console.writeLine(bytes.length);
    Console.writeLine(bytes.capacity);
}
```

```text
0
5
8
0
100
```

`Vector<u8>.withCapacity(100)` allocates at once, so it returns a `Result` too. Use it when you know how many
elements are coming: the vector then never has to move.

## Reading, writing and iterating

<span class="listing-caption"><strong>Listing 8-3</strong>, <code>main.cee</code>: An element is read and written by its index, and a <code>for</code> loop visits every element.</span>

```ceesafe
fn main() {
    var scores = Vector<i32>.create();
    scores.push(70)!;
    scores.push(85)!;
    scores.push(92)!;

    scores[0] = 75;
    for (i in 0..scores.length) {
        scores[i] += 1;
    }

    var total = 0;
    for (s in scores) {
        total += s;
    }
    Console.writeLine(scores[0]);
    Console.writeLine(total);
}
```

```text
76
255
```

`for (s in scores)` gives each element in order. `for (i in 0..scores.length)` gives each index, which you need in
order to change the elements. An index is a `usize`, and `0..scores.length` is a range of `usize`.

Every index is checked when the program runs. An index that is not less than the length stops the program: this
is a panic.

<span class="listing-caption"><strong>Listing 8-4</strong>, <code>main.cee</code>: The vector has two elements, and the program asks for the third. It panics.</span>

<!-- ceesafe: panics -->
```ceesafe
fn main() {
    var scores = Vector<i32>.create();
    scores.push(70)!;
    scores.push(85)!;
    let i = scores.length;
    Console.writeLine(scores[i]);
}
```

The program prints this and ends with a failure status:

```text
ceesafe panic: index out of bounds at main.cee:6:30
```

::: tip Rule
`v[i]` never reads or writes outside the vector. An index out of range is a panic at that line, not a wrong value.
:::

## Taking elements away

<span class="listing-caption"><strong>Listing 8-5</strong>, <code>main.cee</code>: <code>pop</code> takes the last element, <code>resize</code> sets the length, and <code>clear</code> empties the vector.</span>

```ceesafe
fn main() {
    var stack = Vector<i32>.create();
    stack.push(1)!;
    stack.push(2)!;

    match (stack.pop()) {
        Some(top) => { Console.writeLine(top); }
        null => { Console.writeLine("empty"); }
    }

    stack.resize(4, 9)!;
    Console.writeLine(stack.length);
    Console.writeLine(stack[3]);

    stack.clear();
    Console.writeLine(stack.length);
    Console.writeLine(stack.capacity);
    Console.writeLine(stack.pop() ?? -1);
}
```

```text
2
4
9
0
4
-1
```

- `pop()` removes the last element and returns it as an [optional](./ch06-03-optionals), `i32?` here: `null` when
  the vector is empty. `??` gives a value to use in that case.
- `resize(n, fill)` makes the length `n`. It adds copies of `fill`, or removes elements from the end. It may grow
  the vector, so it returns a `Result`. `truncate(n)` only removes, and returns nothing.
- `clear()` removes every element. The capacity stays, ready for the next pushes.

There are also `insert(at, x)`, `remove(at)` and `swapRemove(at)` for the middle of a vector, and `swap(i, j)`.

## Lending the elements as a slice

A function that works on some elements takes a `Slice<T>`, or a `SliceMut<T>` to change them
([Slices and views](./ch04-03-slices-and-views)). A vector lends its elements as either.

<span class="listing-caption"><strong>Listing 8-6</strong>, <code>main.cee</code>: Two functions take slices. The vector lends all its elements, or a range of them.</span>

```ceesafe
fn total(values: Slice<i32>) -> i32 {
    var sum = 0;
    for (v in values) {
        sum += v;
    }
    return sum;
}

fn double(values: SliceMut<i32>) {
    for (i in 0..values.length) {
        values[i] *= 2;
    }
}

fn main() {
    var v = Vector<i32>.create();
    for (i in 1..6) {
        v.push(i)!;
    }
    double(mut v);
    Console.writeLine(total(v));
    Console.writeLine(total(v[1..3]));
    let all = v.view();
    Console.writeLine(all.length);
}
```

```text
30
10
5
```

- `total(v)` passes the vector where a `Slice<i32>` is expected: the vector is borrowed, not moved.
- `double(mut v)` does the same for a `SliceMut<i32>`. The `mut` shows that the call may change `v`.
- `v[1..3]` is a slice of elements 1 and 2.
- `v.view()` and `v.viewMut()` give the two slices as values.

## Methods that take a closure

A closure is a function written where it is used: `(x) => x > 15`. Chapter 13 covers closures. A vector has six
methods that take one.

<span class="listing-caption"><strong>Listing 8-7</strong>, <code>main.cee</code>: Sorting, testing, visiting, filtering and searching a vector with closures.</span>

```ceesafe
fn main() {
    var v = Vector<i32>.create();
    for (i in 1..9) {
        v.push((i * 7) % 10)!;
    }
    v.sortBy((a, b) => a - b);
    Console.writeLine(v[0]);
    Console.writeLine(v.any((x) => x > 8));
    Console.writeLine(v.all((x) => x > 8));
    var sum = 0;
    v.forEach((x) => { sum += x; });
    Console.writeLine(sum);
    v.retain((x) => x % 2 == 0);
    Console.writeLine(v.length);
    Console.writeLine(v.partitionPoint((x) => x < 5));
}
```

```text
1
true
false
42
4
2
```

| Method | What it does |
|---|---|
| `v.sortBy((a, b) => …)` | sorts in place; the closure returns a negative number, zero or a positive number as `a` comes before, with or after `b` |
| `v.any((x) => …)`, `v.all((x) => …)` | whether some element, or every element, passes the test |
| `v.forEach((x) => { … })` | calls the closure on each element in order |
| `v.retain((x) => …)` | keeps the elements that pass the test and removes the others |
| `v.partitionPoint((x) => …)` | a binary search: in a vector whose elements pass a test and then fail it, the index of the first that fails |

After `retain` the vector above is 2, 4, 6, 8, and the first element that is not less than 5 is at index 2.

## A vector cannot grow while an element is borrowed

A push may move every element to a larger piece of memory. A borrow of an element made before the push would then
point at memory that the vector has given back. The compiler does not let that program exist.

<span class="listing-caption"><strong>Listing 8-8</strong>, <code>main.cee</code>: <code>first</code> borrows an element, and is used after a push. This does not compile.</span>

<!-- ceesafe: fail N0241 -->
```ceesafe
fn main() {
    var scores = Vector<i32>.create();
    scores.push(70)!;
    let first = borrow scores[0];
    scores.push(85)!;
    Console.writeLine(first);
}
```

```text
main.cee:5:5: error[N0241]: cannot borrow 'scores' as mutable while 'scores[0]' is borrowed
  |
5 |     scores.push(85)!;
  |     ^^^^^^
  = note: 'scores[0]' is borrowed here (4:5)
  = note: the borrow is used later here (6:5)
```

`push` needs the whole vector exclusively, and `first` still borrows part of it. This is
[the borrowing rule](./ch04-02-borrowing#the-rule), applied to a vector and its elements. Three fixes:

- finish with `first` before the push (move line 6 above line 5);
- copy the value, `let first = scores[0];`, which borrows nothing after that line;
- keep the index, and read `scores[0]` again after the push.

::: tip Rule
A borrow of an element, a slice or a view borrows the whole vector. While it is live, the vector cannot be pushed
to, popped, cleared, written, moved or destroyed.

*Specification: `type-system.md` §23.1, "Invalidation".*
:::

## When the vector goes away

A vector owns its elements. When its owner's block ends, the vector destroys each element, from index 0 upward,
and then gives its memory back. The next listing shows it with a `drop` block, which is code that runs when a
value of the struct is destroyed (chapter 15 covers destruction).

<span class="listing-caption"><strong>Listing 8-9</strong>, <code>main.cee</code>: The elements are destroyed when the vector is, at the end of <code>main</code>.</span>

```ceesafe
struct Ticket {
    id: i32;

    drop {
        Console.write("destroyed ");
        Console.writeLine(this.id);
    }
}

fn main() {
    var queue = Vector<Ticket>.create();
    queue.push(Ticket { id: 1 })!;
    queue.push(Ticket { id: 2 })!;
    queue[0] = Ticket { id: 3 };
    Console.writeLine("end of main");
}
```

```text
destroyed 1
end of main
destroyed 3
destroyed 2
```

Assigning to `queue[0]` destroys the element that was there. `clear`, `truncate` and `resize` destroy the elements
they remove. `pop` does not: it moves the element out to you.

## Mistakes

::: warning Mistake: an index that is not a `usize`
An index is a `usize`. An `i32` is not converted for you, because a negative index has no meaning.
:::

<span class="listing-caption"><strong>Listing 8-10</strong>, <code>main.cee</code>: <code>i</code> is an <code>i32</code>. This does not compile.</span>

<!-- ceesafe: fail N0205 -->
```ceesafe
fn main() {
    var scores = Vector<i32>.create();
    scores.push(70)!;
    let i = 0;
    Console.writeLine(scores[i]);
}
```

```text
main.cee:5:30: error[N0205]: expected 'usize', found 'i32'
  |
5 |     Console.writeLine(scores[i]);
  |                              ^
```

Give the index its type, `let i: usize = 0;`. A loop variable over `0..scores.length` is a `usize` already.

::: warning Mistake: moving an element out by its index
`v[i]` reads a copy of an element whose type is Copy, such as a number. A `String` is not Copy, and moving it out
would leave a hole in the vector.
:::

<span class="listing-caption"><strong>Listing 8-11</strong>, <code>main.cee</code>: A <code>String</code> cannot be moved out of the vector. This does not compile.</span>

<!-- ceesafe: fail N0251 -->
```ceesafe
fn main() {
    var names = Vector<String>.create();
    var name = String.create();
    name.append("Ada")!;
    names.push(name)!;
    let first = names[0];
    Console.writeLine(first);
}
```

```text
main.cee:6:5: error[N0251]: cannot move a 'String' out of an element of the vector 'names'; only Copy values can be read from there (use 'pop()')
  |
6 |     let first = names[0];
  |     ^^^^^^^^^^^^^^^^^^^^^
```

Borrow the element, `let first = borrow names[0];`, or take it out with `pop()`, `remove(at)` or `swapRemove(at)`.

::: details Coming from Rust
`Vector<T>` is `Vec<T>`. `push` returns a `Result`, where Rust aborts on a failed allocation. `v[i]` panics out of
range as in Rust; there is no `v.get(i)` that returns an optional. `length` and `capacity` are fields, not methods.
`for (x in v)` borrows the vector and does not consume it.
:::

::: details Coming from C#, Java, TypeScript or Go
A vector is `List<T>`, `ArrayList`, an array or a slice, with one owner. Assigning a vector to another variable
moves it: it does not make a second name for the same list. An element borrowed from it is checked at compile
time, so a list cannot change under a loop or a borrow that is using it.
:::

## Summary

- `Vector<T>.create()` makes an empty vector; `Vector<T>.withCapacity(n)!` makes one with room for `n` elements.
- `push`, `resize` and `withCapacity` may allocate and return a `Result`: write `!` or `?`.
- `v.length` and `v.capacity` are `usize` fields. `v[i]` takes a `usize` (`N0205`) and panics out of range.
- `pop()` returns an optional. `clear()`, `truncate(n)` and `resize(n, fill)` change the length.
- A vector lends its elements to a `Slice<T>` parameter, and with `mut v` to a `SliceMut<T>` one.
- `sortBy`, `any`, `all`, `forEach`, `retain` and `partitionPoint` take a closure.
- While an element or a slice of a vector is borrowed, the vector cannot change (`N0241`).
- Only a Copy element can be read out by its index (`N0251`). A vector destroys its elements when it is destroyed.

Next: [strings and string views](./ch08-02-strings), which are text kept the same way.
