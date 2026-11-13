---
title: Send and Share
summary: The two properties of types behind every thread check - Send for a value that may move to another thread and Share for one that several threads may read at once, which types have them, how to require them in generic code, and the diagnostic N0334.
assumes: [ch16-01-starting-threads, ch16-03-shared-state, ch10-01-generic-types, ch10-02-interfaces]
keywords: [Send, Share, N0334, bound, "dyn + Send", Cell, Arc, data race, thread safety]
diagnostics: [N0334]
specification: ["type-system.md §38.1", "type-system.md §38.4", "type-system.md §38.6", "ownership.md T1", "ownership.md T2"]
---

# Send and Share

**This section teaches** the two properties the compiler checks whenever a value meets a second thread, which types
have them, and how to read the diagnostic when one is missing. It assumes
[Starting threads](./ch16-01-starting-threads) and [Shared state](./ch16-03-shared-state), and generic bounds and
interfaces ([Generic types and functions](./ch10-01-generic-types), [Interfaces](./ch10-02-interfaces)).

The earlier sections said "safe to move to another thread" and "made for sharing". Those are precise:

- A type is **`Send`** if a value of it may be *moved* to another thread.
- A type is **`Share`** if several threads may *read one value* of it at the same time, through shared borrows.

You do not declare either. The compiler works them out from what a type is made of, and checks them where a thread
starts and where a value is shared.

## Send

A struct is `Send` when all its fields are. Numbers, `bool`, `String`, `Vector<T>` and `Box<T>` of `Send` values are
`Send`, so most types you write are, without a word from you.

<span class="listing-caption"><strong>Listing 16-26</strong>, <code>main.cee</code>: A struct of a string and a vector is <code>Send</code>, so a thread's closure may capture it.</span>

```ceesafe
struct Reading {
    sensor: String;
    values: Vector<u64>;
}

fn main() {
    var values = Vector<u64>.create();
    values.push(20)!;
    values.push(22)!;
    let reading = Reading { sensor: String.from("north")!, values: values };
    let worker = Thread.spawn(move () => {
        var sum: u64 = 0;
        for (v in reading.values.view()) {
            sum += v;
        }
        return sum + reading.sensor.length as u64;
    })!;
    Console.writeLine(worker.join());
}
```

```text
47
```

A type is not `Send` when a value of it is tied to the thread that made it, or to something that thread owns:

- a borrow, a slice, a view, or a struct that holds one: what it borrows from stays behind;
- a raw pointer (the chapter on unsafe code);
- a `Box<dyn I>`, a box holding some value that implements the interface `I` (chapter 18): the compiler cannot see
  what is inside, unless the type says `Box<dyn I + Send>`;
- a `Thread<T>` handle.

::: tip Rule
`Thread.spawn` requires every capture of its closure, and the closure's result, to be `Send`. A `Channel<T>` and a
`Mutex<T>` require `T: Send`.

*Specification: `type-system.md` §38.1, §38.2; `ownership.md` T1.*
:::

## Share

`Share` is the stronger property: every `Share` type is `Send`. Two threads reading one `u64`, one `String` or one
`Vector<u64>` do no harm, since reading changes nothing. So nearly every `Send` type is also `Share`.

The exceptions are the types that *change through a shared borrow*. A `Cell<u64>` (chapter 15) is one: `set` takes a
shared borrow of the cell and replaces its value, with no lock. On one thread that is safe. From two threads it is a
data race. So a `Cell` is `Send`, and may move to another thread, but it is not `Share`, and neither is a struct that
has one as a field.

The library's thread-safe types also change through a shared borrow: `lock`, `fetchAdd` and `send` all take one.
They are `Share` because each makes its changes safe, with a lock or an atomic instruction. That is the whole
difference between a `Cell<u64>` and an `AtomicU64`.

| Type | `Send` | `Share` |
|---|---|---|
| integers, floats, `bool` | yes | yes |
| a struct, enum or array | if every part is | if every part is |
| `String`, `Vector<T>`, `Box<T>` | if `T` is | if `T` is |
| `Arc<T>`, `Mutex<T>`, `Channel<T>`, the atomics | yes | yes |
| `Cell<T>`, `RefCell<T>`, `StableVector<T>` | if `T` is | no |
| `Box<dyn I>` | no | no |
| `Box<dyn I + Send>` | yes | no |
| `Box<dyn I + Share>` | yes | yes |
| `borrow T`, `Slice<T>`, `StringView` | no | no |
| `Thread<T>`, raw pointers | no | no |

::: tip Rule
`Arc<T>` requires `T: Share`, since every owner may read the value at once. A `Mutex<T>` is `Share` whenever `T` is
`Send`: the lock lets one thread in at a time, so what is inside never needs to be `Share`.

*Specification: `type-system.md` §38.1, §38.3; `ownership.md` T2.*
:::

That is why the answer to "this type is not `Share`" is nearly always "put it in a `Mutex`".

`Thread.both` uses both properties, and for it alone a borrow counts as the value it borrows. A value that either
closure only reads must be `Share`. A value that a closure changes must be `Send`.

## In generic code

Inside a generic function the compiler knows about a type parameter only what its bounds say
([Generic types and functions](./ch10-01-generic-types)). `Send` and `Share` are written as bounds like any
interface: `T: Send`, `T: Send + Share`.

<span class="listing-caption"><strong>Listing 16-27</strong>, <code>main.cee</code>: The bound <code>Send</code> lets the function move a value of any such type into a thread.</span>

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

fn areaElsewhere<T: Shape + Send>(shape: T) -> u64 {
    let worker = Thread.spawn(move () => shape.area())!;
    return worker.join();
}

fn main() {
    Console.writeLine(areaElsewhere(Square { side: 5 }));
}
```

```text
25
```

The bound is checked at each call: `areaElsewhere(Square { side: 5 })` is accepted because `Square`, a struct of one
`u64`, is `Send`.

A boxed interface value says the same thing in its type. `Box<dyn Shape + Send>` holds some `Shape` that is known to
be `Send`, so the box may cross to another thread.

<span class="listing-caption"><strong>Listing 16-28</strong>, <code>main.cee</code>: With <code>+ Send</code> in its type, a boxed interface value moves into a thread.</span>

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
    let worker = Thread.spawn(move () => shape.area())!;
    Console.writeLine(worker.join());
}
```

```text
9
```

## Mistakes

Every failure of these checks is the diagnostic N0334. Its message names the value, its type, the property that is
missing, and what the value was about to do.

::: warning Mistake: a boxed interface value without `+ Send`
A plain `Box<dyn Shape>` may hold anything that has an `area`, including a value that must stay on its thread.
:::

<span class="listing-caption"><strong>Listing 16-29</strong>, <code>main.cee</code>: Listing 16-28 without <code>+ Send</code>. This does not compile.</span>

<!-- ceesafe: fail N0334 -->
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
    let worker = Thread.spawn(move () => shape.area())!;
    Console.writeLine(worker.join());
}
```

```text
main.cee:15:31: error[N0334]: this thread's closure captures 'shape', a 'Box<dyn Shape>', which is not Send: it cannot move to another thread (type-system.md §38.1)
   |
15 |     let worker = Thread.spawn(move () => shape.area())!;
   |                               ^^^^^^^^^^^^^^^^^^^^^^^
```

Declare the box as `Box<dyn Shape + Send>`.

::: warning Mistake: a type parameter without the bound
In generic code, `T` is `Send` only if a bound says so.
:::

<span class="listing-caption"><strong>Listing 16-30</strong>, <code>main.cee</code>: Listing 16-27 with the bound <code>Send</code> left out. This does not compile.</span>

<!-- ceesafe: fail N0334 -->
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

fn areaElsewhere<T: Shape>(shape: T) -> u64 {
    let worker = Thread.spawn(move () => shape.area())!;
    return worker.join();
}

fn main() {
    Console.writeLine(areaElsewhere(Square { side: 5 }));
}
```

```text
main.cee:14:31: error[N0334]: this thread's closure captures 'shape', a 'T', which is not Send: it cannot move to another thread (type-system.md §38.1)
   |
14 |     let worker = Thread.spawn(move () => shape.area())!;
   |                               ^^^^^^^^^^^^^^^^^^^^^^^
```

The call in `main` passes a `Square`, which is `Send`, but the function is checked once, for every `T` its bounds
allow. Add `+ Send` to the bound.

::: warning Mistake: sharing a cell between threads
A cell is for one thread. The message says `Share`, not `Send`, because the closures read the same cell.
:::

<span class="listing-caption"><strong>Listing 16-31</strong>, <code>main.cee</code>: Both closures of <code>Thread.both</code> use one <code>Cell</code>. This does not compile.</span>

<!-- ceesafe: fail N0334 -->
```ceesafe
fn main() {
    let hits = Cell<u64>.create(0);
    var seen: u64 = 0;
    Thread.both(() => { hits.set(hits.get() + 1); }, () => { seen = hits.get(); });
    Console.writeLine(seen);
}
```

```text
main.cee:4:17: error[N0334]: this closure reads 'hits', a 'Cell<u64>', which is not Share: it cannot be read from another thread (type-system.md §38.6)
  |
4 |     Thread.both(() => { hits.set(hits.get() + 1); }, () => { seen = hits.get(); });
  |                 ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

main.cee:4:54: error[N0334]: this closure reads 'hits', a 'Cell<u64>', which is not Share: it cannot be read from another thread (type-system.md §38.6)
  |
4 |     Thread.both(() => { hits.set(hits.get() + 1); }, () => { seen = hits.get(); });
  |                                                      ^^^^^^^^^^^^^^^^^^^^^^^^^^^^
```

"Reads" means "uses through a shared borrow": `set` takes one. The borrowing rule did not object, since both
closures are readers in its eyes. `Share` is the check that catches a change behind a shared borrow. Use an
`AtomicU64`, as Listing 16-21 in [Shared state](./ch16-03-shared-state) does.

::: warning Mistake: a struct with a cell in an `Arc`
One field that is not `Share` makes the whole struct not `Share`.
:::

<span class="listing-caption"><strong>Listing 16-32</strong>, <code>main.cee</code>: A <code>Stats</code> moves into a thread, which is allowed. A second one goes into an <code>Arc</code>, which is not. This does not compile.</span>

<!-- ceesafe: fail N0334 -->
```ceesafe
struct Stats {
    name: String;
    reads: Cell<u64>;
}

fn main() {
    let stats = Stats { name: String.from("disk")!, reads: Cell<u64>.create(0) };
    let worker = Thread.spawn(move () => {
        stats.reads.set(stats.reads.get() + 1);
        return stats.reads.get();
    })!;
    Console.writeLine(worker.join());
    let again = Stats { name: String.from("disk")!, reads: Cell<u64>.create(0) };
    let shared = Arc<Stats>.create(again)!;
    Console.writeLine(shared.get().name.view());
}
```

```text
main.cee:14:18: error[N0334]: 'Stats' is not Share, but 'T' must be ('T: Share'; type-system.md §38.1); its field 'reads' is a 'Cell<u64>', which is not Share
   |
14 |     let shared = Arc<Stats>.create(again)!;
   |                  ^^^^^^^^^^
```

There is one error, on line 14. The spawn on line 8 is accepted: `Stats` is `Send`, and after the move only one
thread has it. The message names the field that is the cause. Make it an `AtomicU64`, or share an
`Arc<Mutex<Stats>>`.

::: details Coming from Rust
`Send` is `Send` and `Share` is `Sync`, with the same meaning, inferred the same way. What differs:

- user code cannot declare them: there is no `unsafe impl Send`. Only the library marks its own types;
- `Share` implies `Send`, so `T: Share` alone is enough for `Arc<T>` (Rust's `Arc<T>` wants `T: Send + Sync`);
- a `dyn` type takes the bound in the same place, `Box<dyn Shape + Send>`;
- one code, N0334, covers both properties; read the message for which one.
:::

::: details Coming from Java, C#, Go or C
In those languages "thread-safe" is a sentence in a type's documentation, and passing a type that is not
thread-safe to another thread compiles. Here it is two properties that the compiler computes for every type and
checks at every place where a value meets another thread. A type that is not thread-safe cannot get there.
:::

## Summary

- `Send`: a value may move to another thread. `Share`: several threads may read one value at once. `Share` implies
  `Send`.
- Both are inferred from a type's parts. Plain data, strings, vectors and boxes of them have both.
- Borrows, raw pointers, `Thread<T>` and a plain `Box<dyn I>` are not `Send`. A type that changes through a shared
  borrow without protection, such as a `Cell`, is `Send` but not `Share`.
- `Thread.spawn` needs `Send` captures and a `Send` result; `Arc<T>` needs `T: Share`; `Mutex<T>` needs `T: Send`
  and is itself `Share`.
- In generic code, write the bound: `T: Send`, `T: Send + Share`. For a boxed interface value, write
  `Box<dyn I + Send>`.
- A missing property is N0334, which names the value, its type and the property.

Next: chapter 17, on async, where many tasks share one thread, and the same two properties decide what a task may
carry to another.
