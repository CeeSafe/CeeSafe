---
title: Cells
summary: A cell lets a value change through a shared borrow - Cell for Copy values, RefCell with its guards CellRef and CellRefMut and the panic that replaces a compile-time check, and OnceCell for a value computed once.
assumes: [ch04-02-borrowing, ch05-03-methods, ch06-03-optionals, ch10-03-regions, ch15-02-destruction]
keywords: [Cell, RefCell, OnceCell, RawCell, CellRef, CellRefMut, "interior mutability", read, write, tryRead, tryWrite, getOrInit, "cell already borrowed"]
diagnostics: [N0211, N0268, N0351]
specification: ["type-system.md §41.1", "type-system.md §41.2", "type-system.md §41.3", "type-system.md §41.4"]
---

# Cells

**This section teaches** the cell types `Cell`, `RefCell` and `OnceCell`: what they allow, which of them moves a
check from the compiler to the running program (`RefCell` and `OnceCell` do; `Cell` needs none), and when that
trade is worth making. It assumes
[borrowing](./ch04-02-borrowing), [methods](./ch05-03-methods), [optionals](./ch06-03-optionals),
[regions](./ch10-03-regions) and [destruction](./ch15-02-destruction).

The rule of borrowing is many readers or one writer. A value that is borrowed with `borrow` cannot change, and
the compiler holds every program to that. Sometimes the rule is stricter than the program needs. A method that
only reads may want to count how often it was called. Two parts of a program may both need to add to one log.

A **cell** is a value that can be changed through a shared borrow. The many-readers-or-one-writer rule still
holds for what is inside, but the cell keeps it in another way: by never lending the inside, or by checking while
the program runs.

## `Cell<T>`: get and set

`Cell<T>` holds a Copy value. `get()` returns a copy of it and `set(v)` stores a new one. Both take `borrow this`.

<span class="listing-caption"><strong>Listing 15-28</strong>, <code>main.cee</code>: <code>read</code> borrows the sensor to read it, and still counts its calls.</span>

```ceesafe
struct Sensor {
    value: i32;
    reads: Cell<u32>;

    fn read(borrow this) -> i32 {
        this.reads.set(this.reads.get() + 1);
        return this.value;
    }
}

fn main() {
    let sensor = Sensor { value: 21, reads: Cell<u32>.create(0) };
    let first = borrow sensor;
    let second = borrow sensor;
    Console.writeLine(first.read() + second.read());
    Console.writeLine(sensor.reads.get());
    let before = sensor.reads.replace(0);
    Console.writeLine(before);
}
```

```text
42
2
2
```

`sensor` is a `let`, and it is borrowed twice at once. The count changes all the same. `replace(v)` stores `v` and
returns what was there.

A `Cell` is safe without any check because it never lends its contents. `get` hands out a copy, so no borrow of
the inside exists that a `set` could change under its holder. That is also why `T` must be Copy.

::: tip Rule
`Cell<T>` holds a Copy value that `get()`, `set(v)` and `replace(v)` read and write through a shared borrow. It
costs nothing at run time: a `get` is one read and a `set` is one write.

*Specification: `type-system.md` §41.2.*
:::

## `RefCell<T>`: borrowing checked at run time

A `String` or a `Vector` is not Copy, and using one means borrowing it. `RefCell<T>` lends its contents through
two methods, and counts the loans while the program runs.

- `cell.read()` returns a `CellRef<T>`. Its `data()` lends a `borrow T`.
- `cell.write()` returns a `CellRefMut<T>`. Its `data()` lends a `borrow mut T`.

A `CellRef` or `CellRefMut` is a **guard**: a value that stands for the access. While it lives, the cell knows that
it is being read or written. When the guard is destroyed, the access is given back.

<span class="listing-caption"><strong>Listing 15-29</strong>, <code>main.cee</code>: Two workers hold a shared borrow of one log, and both add to it.</span>

```ceesafe
struct Worker<region r> {
    name: StringView<r>;
    log: borrow(r) RefCell<Vector<String>>;

    fn run(borrow this) {
        var line = String.from(this.name)!;
        line.append(" ran")!;
        var guard = this.log.write();
        guard.data().push(line)!;
    }
}

fn main() {
    let log = RefCell<Vector<String>>.create(Vector<String>.create());
    let a = Worker { name: "first", log: borrow log };
    let b = Worker { name: "second", log: borrow log };
    a.run();
    b.run();
    a.run();
    let guard = log.read();
    for (line in guard.data()) {
        Console.writeLine(line);
    }
}
```

```text
first ran
second ran
first ran
```

Without the cell this program has no good form. Each worker would need a `borrow mut` of the vector, and two
exclusive borrows of one value cannot be live together.

In `run`, `guard` is a `var` because `data()` on a `CellRefMut` takes `borrow mut this`. The guard is destroyed at
the end of `run`, so the next call finds the cell free.

The check that the compiler no longer makes is made by `read` and `write`:

- `write()` panics if any guard of the cell is live;
- `read()` panics if a `CellRefMut` is live. Any number of `CellRef` guards may be live together.

<span class="listing-caption"><strong>Listing 15-30</strong>, <code>main.cee</code>: A writer is asked for while a reader is live. The program panics.</span>

<!-- ceesafe: panics -->
```ceesafe
fn main() {
    let numbers = RefCell<Vector<i32>>.create(Vector<i32>.create());
    let reader = numbers.read();
    Console.writeLine(reader.data().length);
    var writer = numbers.write();
    writer.data().push(1)!;
    Console.writeLine(reader.data().length);
}
```

```text
0
```

Then, on standard error:

```text
ceesafe panic: cell already borrowed at main.cee:5:18
```

With plain borrows the compiler would have refused this program. With a `RefCell` it compiles, and the mistake is
found when that line runs, if a test runs it.

::: tip Rule
`RefCell<T>` lends its contents through guards: `read()` for shared access and `write()` for exclusive access.
Many readers or one writer is checked when the guard is made, and a conflict is a panic, "cell already borrowed".

*Specification: `type-system.md` §41.3.*
:::

### Guards last to the end of their block

A plain borrow ends at its last use. A guard is a value with a `drop` block, so it lasts until its block ends,
like any local. A guard that is never named is a temporary and ends with its statement.

`tryRead()` and `tryWrite()` return the guard as an optional, `null` where `read` and `write` would panic.

<span class="listing-caption"><strong>Listing 15-31</strong>, <code>main.cee</code>: The reader lives in an inner block. Inside it a writer is refused. After it a writer is free to work.</span>

```ceesafe
fn main() {
    let numbers = RefCell<Vector<i32>>.create(Vector<i32>.create());
    {
        let reader = numbers.read();
        match (numbers.tryWrite()) {
            Some(var writer) => { writer.data().push(1)!; }
            null => { Console.writeLine("busy: someone is reading"); }
        }
        Console.writeLine(reader.data().length);
    }
    numbers.write().data().push(2)!;
    Console.writeLine(numbers.read().data().length);
}
```

```text
busy: someone is reading
0
1
```

The last two lines each make a guard and use it, and the guard is destroyed at the `;`. Keeping guards that short is the way to
avoid the panic: take the guard, do the work, and let it go before calling code that may use the cell.

`RefCell` has three more operations that need no guard in your hands. `cell.replace(v)` stores `v` and returns
the old contents. `cell.take()`, on a `RefCell<T?>`, returns the contents and leaves `null`. Both panic as
`write` does. With an exclusive borrow of the cell itself, `cell.getMut()` lends the contents with no check, and
`cell.intoInner()` ends the cell and returns them.

## `OnceCell<T>`: written once

A `OnceCell<T>` starts empty and is filled at most once, through a shared borrow. After that it lends a plain
`borrow T` with no guard and no check, because the value can no longer change. It suits a value that is costly
to compute and is wanted only sometimes.

<span class="listing-caption"><strong>Listing 15-32</strong>, <code>main.cee</code>: The total is computed at the first call of <code>sum</code> and kept for the second.</span>

```ceesafe
struct Report {
    lines: Vector<i32>;
    total: OnceCell<i32>;

    fn sum(borrow this) -> i32 {
        return this.total.getOrInit(() => {
            Console.writeLine("adding up");
            var t = 0;
            for (n in this.lines) {
                t += n;
            }
            return t;
        });
    }
}

fn main() {
    var lines = Vector<i32>.create();
    lines.push(3)!;
    lines.push(4)!;
    let report = Report { lines: lines, total: OnceCell<i32>.create() };
    Console.writeLine(report.sum());
    Console.writeLine(report.sum());
}
```

```text
adding up
7
7
```

The argument of `getOrInit` is a closure: a function written in place, which runs only if the cell is empty.
Chapter 13 covers closures. `cell.get()` returns the value as an optional borrow without filling the cell, and
`cell.set(v)` fills an empty cell and returns `Err(v)` for a full one.

## Choosing

| Type | Holds | Access | Cost of the check |
|---|---|---|---|
| `Cell<T>` | a Copy value | `get`, `set`, `replace` | none |
| `RefCell<T>` | any owned value | guards from `read` and `write` | a counter, and a panic on conflict |
| `OnceCell<T>` | nothing, then one value | `getOrInit`, `get`, `set` | a test of whether it is full |

All three are built on `RawCell<T>`, the one type whose contents may change under a shared borrow. Using it
directly needs unsafe code, which chapter 20 covers.

A cell is for one thread. A cell, and any struct that holds one, cannot be shared between threads; chapter 16
covers what can.

Use a cell when the design needs it, not to quiet the compiler. A `borrow mut` parameter is checked before the
program runs and costs nothing. A `RefCell` moves the same check to run time, where it is found only on the paths
that tests reach.

## Mistakes

::: warning Mistake: changing a field through a shared borrow
A `borrow this` method cannot assign to a field.
:::

<span class="listing-caption"><strong>Listing 15-33</strong>, <code>main.cee</code>: <code>read</code> borrows the sensor to read it and tries to count. This does not compile.</span>

<!-- ceesafe: fail N0211 -->
```ceesafe
struct Sensor {
    value: i32;
    reads: u32;

    fn read(borrow this) -> i32 {
        this.reads += 1;
        return this.value;
    }
}

fn main() {
    let sensor = Sensor { value: 21, reads: 0 };
    Console.writeLine(sensor.read());
}
```

```text
main.cee:6:9: error[N0211]: cannot assign to 'this.reads': it is behind a shared reference ('borrow'); a 'borrow mut' reference is needed to modify it
  |
6 |         this.reads += 1;
  |         ^^^^^^^^^^
```

Make the method `borrow mut this` if its callers can give an exclusive borrow. If they cannot, make the field a
`Cell<u32>`, as Listing 15-28 does.

::: warning Mistake: a `Cell` of a value that is not Copy
`Cell` works by copying the value in and out.
:::

<span class="listing-caption"><strong>Listing 15-34</strong>, <code>main.cee</code>: A <code>Cell</code> of a <code>String</code>. This does not compile.</span>

<!-- ceesafe: fail N0268 -->
```ceesafe
fn main() {
    let name = Cell<String>.create(String.from("x")!);
}
```

```text
main.cee:2:16: error[N0268]: 'String' is not Copy, but 'T' must be ('T: Copy')
  |
2 |     let name = Cell<String>.create(String.from("x")!);
  |                ^^^^^^^^^^^^
```

Use `RefCell<String>`, or `RefCell<String?>` if the text is to be taken out with `take()`.

::: warning Mistake: a cell of a borrow
A cell cannot hold a view, a slice or a borrow.
:::

<span class="listing-caption"><strong>Listing 15-35</strong>, <code>main.cee</code>: A <code>RefCell</code> of a <code>StringView</code>. This does not compile.</span>

<!-- ceesafe: fail N0351 -->
```ceesafe
fn main() {
    let text = String.from("hello")!;
    let view = RefCell<StringView>.create(text.view());
}
```

```text
main.cee:3:16: error[N0351]: a cell cannot hold 'StringView', which holds loans; hold a raw pointer or an owned value instead (type-system.md §41.1; ADR 0084 §2.1)
  |
3 |     let view = RefCell<StringView>.create(text.view());
  |                ^^^^^^^^^^^^^^^^^^^
```

Put an owned value in the cell, here a `String`. A struct may hold a borrow of a cell, as `Worker` does in
Listing 15-29: that is the usual arrangement.

::: warning Mistake: holding a guard across a call that uses the cell
The second `write()` or `read()` panics, as in Listing 15-30. Nothing warns of it at compile time. Keep each guard
in the smallest block that does the work, or use it as a temporary in one statement.
:::

::: details Coming from Rust
`Cell`, `RefCell` and `OnceCell` are Rust's, and `RawCell` is `UnsafeCell`. `borrow` is a keyword here, so
`borrow()` and `borrow_mut()` are `read()` and `write()`, and `try_borrow` is `tryRead`. A guard is used through
`data()`, not by dereferencing. There is no `Rc`, so there is no `Rc<RefCell<T>>`: several parts of a program
share a cell by borrowing it, as Listing 15-29 does, and the cell's owner outlives them.
:::

::: details Coming from C#, Java, Go, Python or TypeScript
In those languages every object can be changed through every reference to it, and nothing checks that two parts
of a program do not change it under each other. A `RefCell` is the nearest thing to that, with the check added. In
CeeSafe it is the exception: start with `borrow mut`.
:::

::: details Coming from C or C++
A `Cell` field is a `mutable` member. A `RefCell` is a `mutable` member with a counter that turns a read during a
write into a stop at a known line, where C++ has undefined behaviour.
:::

## Summary

- A field cannot be assigned through a shared borrow (`N0211`). A cell is the exception.
- `Cell<T>` holds a Copy value (`N0268` otherwise): `get`, `set`, `replace`. It has no run-time check and needs
  none.
- `RefCell<T>` lends its contents through guards: `read()` gives a `CellRef`, `write()` a `CellRefMut`, and
  `data()` gives the borrow. A conflict panics with "cell already borrowed". `tryRead` and `tryWrite` return
  `null` in its place.
- A guard lasts to the end of its block, or of its statement if it has no name.
- `OnceCell<T>` is filled once, by `getOrInit` or `set`, and then lends a plain borrow.
- A cell cannot hold a borrow or a view (`N0351`), and cannot be shared between threads.

Next: [shared ownership](./ch15-05-shared-ownership), for a value that has no single owner.
