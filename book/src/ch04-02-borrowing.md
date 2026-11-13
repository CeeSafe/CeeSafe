---
title: Borrowing
summary: Using a value without taking it - a shared borrow (borrow T) reads, an exclusive borrow (borrow mut T) reads and writes and needs mut at the call. The one rule (many readers or one writer, while a borrow is live), when a borrow ends, the reborrow that passes an exclusive borrow on, and the diagnostics N0240 and N0242.
assumes: [ch01-03-hello-package, ch04-01-what-ownership-is]
keywords: [borrow, "borrow mut", mut, "shared borrow", "exclusive borrow", reborrow, place, reference, live, aliasing]
diagnostics: [N0240, N0242]
specification: ["ownership.md B1", "lifetimes.md §5"]
---

# Borrowing

**This section teaches** how a function uses a value without taking it away from its owner, and the one rule the
compiler enforces about that. It assumes you can write and call a function ([Hello, package](./ch01-03-hello-package))
and know what an owner and a move are ([What ownership is](./ch04-01-what-ownership-is)).

Every value has one owner. Passing a value to a function can hand it over, and then the caller no longer has it.
Most functions do not want that. They want to look at a value, or change it where it is, and leave it with its
owner. That is a **borrow**.

There are two kinds:

- `borrow T` is a **shared** borrow. It can read the value. Any number can exist at once.
- `borrow mut T` is an **exclusive** borrow. It can read and write the value, and while it is in use nothing else
  can touch the value.

## Borrowing for a call

<span class="listing-caption"><strong>Listing 4-8</strong>, <code>main.cee</code>: One function reads a counter and another changes it. Neither takes it.</span>

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

`counter` is declared with `var`, not `let`, because it changes. A `let` cannot be borrowed exclusively.

## The rule

::: tip Rule: many readers or one writer
While a borrow is **live**, the value it borrows may not be:

- changed, exclusively borrowed, moved or destroyed, if the borrow is shared;
- used in any way, if the borrow is exclusive.

*Specification: `ownership.md`, invariant B1.*
:::

A borrow is **live** from where it is made to the last place it, or anything made from it, is used. It is not live
to the end of the block. The compiler checks the rule in each function, on every path through it.

This is what makes a borrow safe to use: while you hold one, the value cannot change under you or go away.

## When a borrow ends

<span class="listing-caption"><strong>Listing 4-9</strong>, <code>main.cee</code>: The borrow <code>r</code> is last used on line 4, so line 5 may change <code>total</code>.</span>

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

What follows `borrow` or `borrow mut` must be a **place**: a variable, a field of one, or what another borrow
leads to. A value without a name, such as the result of a call, is not a place. Bind it to a variable first.

## Passing an exclusive borrow on

A function that has an exclusive borrow often gives it to another function, and wants it back.

<span class="listing-caption"><strong>Listing 4-10</strong>, <code>main.cee</code>: <code>recordTwice</code> lends its exclusive borrow to <code>record</code> twice, and then uses it itself.</span>

```ceesafe
struct Counter {
    hits: u64;
}

fn record(c: borrow mut Counter) {
    c.hits += 1;
}

fn recordTwice(c: borrow mut Counter) {
    record(c);
    record(c);
    c.hits += 10;
}

fn main() {
    var counter = Counter { hits: 0 };
    recordTwice(mut counter);
    Console.writeLine(counter.hits);
}
```

```text
12
```

In `recordTwice`, `c` is an exclusive borrow, and an exclusive borrow is never copied: two copies would be two
writers. Nor does `record(c)` take `c` away. The call makes a **reborrow**: a new exclusive borrow of the same
counter, lent to `record`. While the reborrow is live, `c` cannot be used. It ends when the call returns, and `c`
is usable again, so the second call and the last line compile. The argument is written `c`, because `c` already
is an exclusive borrow.

::: tip Rule
Passing an exclusive borrow to a function, or giving it to a `let`, reborrows it. The original cannot be used
while the new borrow is live, and can be used again afterwards.

*Specification: `type-system.md` §13.3; `ownership.md`, invariant B4.*
:::

## Mistakes

::: warning Mistake: changing a value that is still borrowed
Swap the two middle lines of Listing 4-9, and the borrow is live across the change.
:::

<span class="listing-caption"><strong>Listing 4-11</strong>, <code>main.cee</code>: <code>r</code> is used after <code>total</code> changes. This does not compile.</span>

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
use that keeps it live (5:5). To fix it, finish using the borrow before the change, as Listing 4-9 does, or read
`total` again after it.

::: warning Mistake: reading a value while it is exclusively borrowed
An exclusive borrow shuts out reads too, not only writes.
:::

<span class="listing-caption"><strong>Listing 4-12</strong>, <code>main.cee</code>: <code>total</code> is read while <code>r</code> may still write it. This does not compile.</span>

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

::: info The compiler's words
The compiler's messages call an exclusive borrow *mutably borrowed*, and a borrow a *reference*, as the
specification does. This book says *exclusive borrow* and *borrow*, and *shared* and *exclusive* for the two
kinds, because those words say the rule.
:::

::: details Coming from Rust
Rust's reference `&T` is CeeSafe's shared borrow `borrow T`, and `&mut T` is the exclusive borrow `borrow mut T`,
with the same rule, the same non-lexical lifetimes and the same reborrowing. Three things differ:

- a shared borrow for a parameter is implicit at the call: `show(counter)`, not `show(&counter)`;
- an exclusive one is written `mut counter`, not `&mut counter`;
- a borrow is used as the value it borrows: `r += 1` changes the borrowed value. `*r` is accepted and means the
  same, and is seldom written.
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

- `borrow T`, a shared borrow, reads; `borrow mut T`, an exclusive borrow, reads and writes.
- Call a `borrow mut` parameter with `mut x`; a `borrow` parameter with `x`.
- While a shared borrow is live, the value cannot change (`N0240`). While an exclusive borrow is live, the value
  cannot be used at all (`N0242`).
- A borrow is live until its last use, not until the end of its block.
- Passing an exclusive borrow on reborrows it: it is lent for the call, and usable again afterwards.

Next: [Slices and views](./ch04-03-slices-and-views), which are borrows of part of a collection.
