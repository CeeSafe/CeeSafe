---
title: Borrowing
summary: Using a value without taking it - borrow T reads, borrow mut T reads and writes and needs mut at the call. The one rule (many readers or one writer, while a borrow is live), when a borrow ends, and the diagnostics N0240 and N0242.
assumes: [ch01-03-hello-package]
keywords: [borrow, "borrow mut", mut, reference, live, shared, exclusive, aliasing]
diagnostics: [N0240, N0242]
specification: ["ownership.md B1", "lifetimes.md §5"]
---

# Borrowing

**This section teaches** how a function uses a value without taking it away from its owner, and the one rule the
compiler enforces about that. It assumes you can write and call a function ([Hello, package](./ch01-03-hello-package));
section 4.1, on what an owner is, will come before it.

Every value has one owner. Passing a value to a function can hand it over, and then the caller no longer has it.
Most functions do not want that. They want to look at a value, or change it where it is, and leave it with its
owner. That is a **borrow**.

There are two kinds:

- `borrow T` is a **shared** borrow. It can read the value. Any number can exist at once.
- `borrow mut T` is an **exclusive** borrow. It can read and write the value, and while it is in use nothing else
  can touch the value.

## Borrowing for a call

<span class="listing-caption"><strong>Listing 4-5</strong>, <code>main.cee</code>: One function reads a counter and another changes it. Neither takes it.</span>

```ceesafe
struct Counter {
    hits: u64;
}

fn show(c: borrow Counter) {
    Console.writeLine(c.hits);
}

fn record(c: borrow mut Counter) {
    c.hits += 1;
}

fn main() {
    var counter = Counter { hits: 0 };
    record(mut counter);
    record(mut counter);
    show(counter);
}
```

```text
2
```

- `show` takes `borrow Counter`: it may read `c.hits`, and `main` still owns `counter` afterwards.
- `record` takes `borrow mut Counter`: it may change `c.hits`.
- At the call, `record(mut counter)` says `mut`. `show(counter)` says nothing.

::: tip Rule
An argument for a `borrow mut` parameter is written `mut x`. An argument for a `borrow` parameter is written `x`.
So every place where a call may change a value shows it.
:::

`counter` is declared with `var`, not `let`, because it changes. A `let` cannot be borrowed mutably.

## The rule

::: tip Rule: many readers or one writer
While a borrow is **live**, the value it borrows may not be:

- changed, mutably borrowed, moved or destroyed, if the borrow is shared;
- used in any way, if the borrow is exclusive.

*Specification: `ownership.md`, invariant B1.*
:::

A borrow is **live** from where it is made to the last place it, or anything made from it, is used. It is not live
to the end of the block. The compiler checks the rule in each function, on every path through it.

This is what makes a borrow safe to use: while you hold one, the value cannot change under you or go away.

## When a borrow ends

<span class="listing-caption"><strong>Listing 4-6</strong>, <code>main.cee</code>: The borrow <code>r</code> is last used on line 4, so line 5 may change <code>total</code>.</span>

```ceesafe
fn main() {
    var total = 0;
    let r = borrow total;
    Console.writeLine(r);
    total += 1;
    Console.writeLine(total);
}
```

```text
0
1
```

`borrow total` makes a shared borrow by hand and `r` holds it. After line 4 nothing uses `r` again, so the borrow
is over and `total` is free to change.

## Mistakes

::: warning Mistake: changing a value that is still borrowed
Swap the two middle lines of Listing 4-6, and the borrow is live across the change.
:::

<span class="listing-caption"><strong>Listing 4-7</strong>, <code>main.cee</code>: <code>r</code> is used after <code>total</code> changes. This does not compile.</span>

<!-- ceesafe: fail N0240 -->
```ceesafe
fn main() {
    var total = 0;
    let r = borrow total;
    total += 1;
    Console.writeLine(r);
}
```

```text
main.cee:4:5: error[N0240]: cannot assign to 'total' while 'total' is borrowed
  |
4 |     total += 1;
  |     ^^^^^^^^^^^
  = note: 'total' is borrowed here (3:5)
  = note: the borrow is used later here (5:5)
```

The message gives the three places that matter: the change (line 4), where the borrow was made (3:5), and the later
use that keeps it live (5:5). To fix it, finish using the borrow before the change, as Listing 4-6 does, or read
`total` again after it.

::: warning Mistake: reading a value while it is exclusively borrowed
An exclusive borrow shuts out reads too, not only writes.
:::

<span class="listing-caption"><strong>Listing 4-8</strong>, <code>main.cee</code>: <code>total</code> is read while <code>r</code> may still write it. This does not compile.</span>

<!-- ceesafe: fail N0242 -->
```ceesafe
fn main() {
    var total = 0;
    let r = borrow mut total;
    Console.writeLine(total);
    r += 1;
}
```

```text
main.cee:4:5: error[N0242]: cannot use 'total' while 'total' is mutably borrowed
  |
4 |     Console.writeLine(total);
  |     ^^^^^^^^^^^^^^^^^^^^^^^^^
  = note: 'total' is mutably borrowed here (3:5)
  = note: the borrow is used later here (5:5)
```

Read through the borrow instead (`Console.writeLine(r)`), or read `total` after the last use of `r`.

::: details Coming from Rust
`borrow T` is `&T` and `borrow mut T` is `&mut T`, with the same rule and the same non-lexical lifetimes. Three
things differ:

- a shared borrow for a parameter is implicit at the call: `show(counter)`, not `show(&counter)`;
- an exclusive one is written `mut counter`, not `&mut counter`;
- there is no `*r` to read or write through a borrow: `r += 1` changes the borrowed value.
:::

::: details Coming from C#, Java, TypeScript or Go
A `borrow` is not a reference to an object that the garbage collector keeps alive. It is a short loan, checked at
compile time, that cannot outlive the value. `borrow mut` at a call is like C#'s `ref`: the caller writes `mut` as
C# writes `ref`.
:::

::: details Coming from C
A borrow is a pointer that cannot be null, cannot dangle, and cannot be used while something else changes what it
points to. Those three guarantees are what the rule buys.
:::

## Summary

- `borrow T` reads; `borrow mut T` reads and writes.
- Call a `borrow mut` parameter with `mut x`; a `borrow` parameter with `x`.
- While a shared borrow is live, the value cannot change (`N0240`). While an exclusive borrow is live, the value
  cannot be used at all (`N0242`).
- A borrow is live until its last use, not until the end of its block.

Next, section 4.3: slices and views, which are borrows of part of a collection.
