---
title: Box
summary: A Box owns one value in allocated memory - Box.create and its Result, reaching the value through the box, moving it out again, and building recursive types such as lists and trees, with the diagnostics N0227, N0211 and N0246.
assumes: [ch04-01-what-ownership-is, ch04-02-borrowing, ch06-02-match, ch06-03-optionals, ch09-03-allocation-failure]
keywords: [Box, Box.create, heap, recursive type, list, tree, unbox, "infinite size", "Box.intoRaw", "Box.fromRaw"]
diagnostics: [N0227, N0211, N0246]
specification: ["type-system.md §18.2", "type-system.md §18.3", "type-system.md §18.4", "type-system.md §18.5", "type-system.md §34.2"]
---

# Box

**This section teaches** `Box<T>`: how to put one value in allocated memory, how to use it there, how to take it
out, and why recursive types need it. It assumes [ownership](./ch04-01-what-ownership-is),
[borrowing](./ch04-02-borrowing), [`match`](./ch06-02-match), [optionals](./ch06-03-optionals) and
[allocation failure](./ch09-03-allocation-failure).

A local variable holds its value in the function's own memory, the stack. A `Box<T>` holds its value somewhere
else: in memory that the program asks the system for, the heap. The box itself is one address. It owns the value,
and when the box is destroyed the value is destroyed and the memory is given back.

## Making and using a box

`Box.create(value)` moves `value` into new memory. Asking for memory can fail, so it returns
`Result<Box<T>, AllocError>`, like `push` on a vector.

<span class="listing-caption"><strong>Listing 15-1</strong>, <code>main.cee</code>: An <code>Account</code> in a box is used as if it were the account.</span>

```ceesafe
struct Account {
    id: u64;
    balance: i64;

    fn deposit(borrow mut this, amount: i64) {
        this.balance += amount;
    }
}

fn show(account: borrow Account) {
    Console.write(account.id);
    Console.write(": ");
    Console.writeLine(account.balance);
}

fn open(id: u64) -> Result<Box<Account>, AllocError> {
    return Box.create(Account { id: id, balance: 0 });
}

fn main() {
    var account = open(7)!;
    account.deposit(100);
    account.balance -= 30;
    show(account);
    let moved = account;
    show(moved);
}
```

```text
7: 70
7: 70
```

- `open` returns the `Result` of `Box.create` as it is. `main` ends it with `!`, which stops the program if there
  is no memory.
- `account.deposit(100)` and `account.balance` reach the `Account` through the box. Nothing is written to follow
  the address.
- `show` takes a `borrow Account`, and a `Box<Account>` is accepted for it: the call borrows the contents.
- `let moved = account;` moves the box. One address is copied, the account stays where it is, and `account` is
  empty afterwards.
- At the end of `main`, `moved` is destroyed: the `Account` first, then its memory.

The contents can be changed when the box can: `account` is a `var`. Through a `let` box they can only be read.

::: tip Rule
`Box.create(v)` moves `v` to the heap and returns `Result<Box<T>, AllocError>`. A `Box<T>` is a move type of one
address. Fields, elements and methods of `T` are reached through it, and it is accepted where a `borrow T` is
wanted. Destroying the box destroys the `T` and frees its memory.

*Specification: `type-system.md` §18.2.*
:::

A box of a small value on its own is rarely useful: an `Account` in a local variable is cheaper. A box is for a
value that must not live inside its owner. The main case is a type that contains itself.

## Recursive types

A list of numbers is either empty, or a number followed by a list. Written directly, that enum would contain
itself, and a value of it would have no size. With a box, the rest of the list is one address.

<span class="listing-caption"><strong>Listing 15-2</strong>, <code>main.cee</code>: A linked list. Each <code>Cons</code> owns the rest of the list through a box.</span>

```ceesafe
enum List {
    Nil,
    Cons(i32, Box<List>),
}

fn sum(list: borrow List) -> i32 => match (list) {
    List.Nil => 0,
    List.Cons(value, rest) => value + sum(rest),
};

fn prepend(value: i32, list: List) -> Result<List, AllocError> {
    return Ok(List.Cons(value, Box.create(list)?));
}

fn main() {
    var list = List.Nil;
    for (i in 1..4) {
        list = prepend(i, list)!;
    }
    Console.writeLine(sum(list));
}
```

```text
6
```

`prepend` takes the old list by value, moves it into a box, and returns a `Cons` that owns the box. `sum` borrows
the list: in its `match`, `rest` is a borrow of the box, and `sum(rest)` borrows the list inside it.

When `main` ends, `list` is destroyed, and each `Cons` destroys its box in turn. The compiler does this with a
loop, so a list of two million elements is destroyed without using two million levels of stack.

A tree has two children, and a child may be missing. `Box<Node>?` is an optional box: `null`, or a box.

<span class="listing-caption"><strong>Listing 15-3</strong>, <code>main.cee</code>: A binary search tree. <code>insert</code> fills the empty slot it arrives at.</span>

```ceesafe
struct Node {
    value: i32;
    left: Box<Node>?;
    right: Box<Node>?;
}

fn insert(slot: borrow mut (Box<Node>?), value: i32) -> Result<void, AllocError> {
    match (slot) {
        Some(node) => {
            if (value < node.value) {
                insert(mut node.left, value)?;
            } else {
                insert(mut node.right, value)?;
            }
        }
        null => {
            slot = Box.create(Node { value: value, left: null, right: null })?;
        }
    }
    return Ok();
}

fn walk(slot: borrow (Box<Node>?)) {
    match (slot) {
        Some(node) => {
            walk(node.left);
            Console.writeLine(node.value);
            walk(node.right);
        }
        null => {}
    }
}

fn main() {
    var root: Box<Node>? = null;
    for (v in [5, 2, 8, 1, 9, 3]) {
        insert(mut root, v)!;
    }
    walk(root);
}
```

```text
1
2
3
5
8
9
```

`insert` takes an exclusive borrow of a slot: the root, or the `left` or `right` field of a node. If the slot holds
a node, it goes down one side. If the slot is `null`, `slot = …` writes the new box through the borrow.

The parentheses in `borrow mut (Box<Node>?)` matter. They say "a borrow of an optional box". Without them the type
is an optional borrow, which the mistakes below show.

::: tip Rule
A type cannot contain itself by value. It can contain a `Box` of itself, because a box is one address whatever it
points to.

*Specification: `type-system.md` §18.2, §18.5.*
:::

## Taking the value out

A box in a local variable owns its contents as a struct owns its fields, so the contents can be moved out. There
are two ways.

- Where a `T` is expected, a `Box<T>` gives its contents. For a Copy type that is a copy. For a move type it moves
  the value out and frees the box.
- A `match` on an enum value moves the payloads into its bindings. [Section 6.2](./ch06-02-match) covers `match`.

<span class="listing-caption"><strong>Listing 15-4</strong>, <code>main.cee</code>: The loop takes the list apart. Each pass moves one text out and unboxes the rest.</span>

```ceesafe
enum List {
    Nil,
    Cons(String, Box<List>),
}

fn main() {
    let tail = Box.create(List.Cons(String.from("b")!, Box.create(List.Nil)!))!;
    var list = List.Cons(String.from("a")!, tail);
    while (true) {
        match (list) {
            List.Cons(text, rest) => {
                Console.writeLine(text);
                list = rest;
            }
            List.Nil => { break; }
        }
    }
    let count = Box.create(41)!;
    let next: i32 = count + 1;
    Console.writeLine(next);
}
```

```text
a
b
42
```

`match (list)` consumes `list`: `text` now owns the string and `rest` owns the box. `list = rest;` assigns a
`Box<List>` where a `List` is expected, so the contents move out of the box into `list`, and the box's memory is
freed. `count + 1` reads the `i32` through its box.

A field of a boxed struct moves out in the same way as a field of a struct in a local: `let name = person.name;`.
After that the box is partly empty and cannot be used as a whole.

::: tip Rule
The contents of a box that a local owns can be moved out: the whole value where a `T` is expected, a field by
naming it, an enum's payloads by `match`. Through a borrow of a box nothing can be moved out.

*Specification: `type-system.md` §18.3, §18.4.*
:::

## What else a box does

- `Box<T>` is short for `Box<T, SystemHeap>`. The second parameter is where the memory comes from, and
  `Box.createIn` chooses it; [section 15.3](./ch15-03-allocators) covers allocators.
- A box can hold a value known only by an interface, `Box<dyn Shape>`. Chapter 18 covers it.
- `Box.intoRaw(b)` gives up the box and returns the address of its contents as a raw pointer, and
  `Box.fromRaw(p)` takes one back. They exist to hand a value to C code. Nothing frees the value in between, so a
  box that is never taken back is a leak. `fromRaw` is unsafe. Chapter 20 covers both.
- When the compiler can prove that a box never leaves its function, it keeps the value on the stack. The program
  behaves the same. `ceesafec build --explain-allocations` prints the decision for each box.

## Mistakes

::: warning Mistake: a recursive type without a box
An enum or struct that holds itself directly has no finite size.
:::

<span class="listing-caption"><strong>Listing 15-5</strong>, <code>main.cee</code>: <code>Cons</code> holds a <code>List</code> by value. This does not compile.</span>

<!-- ceesafe: fail N0227 -->
```ceesafe
enum List {
    Nil,
    Cons(i32, List),
}

fn main() {
    let list = List.Nil;
}
```

```text
main.cee:3:15: error[N0227]: 'List' contains 'List' by value, which contains 'List': the type would have infinite size
  |
3 |     Cons(i32, List),
  |               ^^^^
```

Write `Cons(i32, Box<List>)`, as Listing 15-2 does.

::: warning Mistake: changing the contents of a `let` box
The contents of a box are as mutable as the box. A box declared with `let` is read-only all the way through.
:::

<span class="listing-caption"><strong>Listing 15-6</strong>, <code>main.cee</code>: <code>p</code> is a <code>let</code>, so <code>p.x</code> cannot be assigned. This does not compile.</span>

<!-- ceesafe: fail N0211 -->
```ceesafe
struct Point {
    x: i32;
    y: i32;
}

fn main() {
    let p = Box.create(Point { x: 1, y: 2 })!;
    p.x = 10;
    Console.writeLine(p.x);
}
```

```text
main.cee:8:5: error[N0211]: cannot assign to 'p.x': 'p' was declared with 'let'; use 'var' for a mutable variable
  |
8 |     p.x = 10;
  |     ^^^
```

Declare the box with `var`.

::: warning Mistake: `borrow mut Box<Node>?` for a slot
`?` binds to the whole type before it, so `borrow mut Box<Node>?` is "an optional exclusive borrow of a box", not
"an exclusive borrow of an optional box". The parameter is then an optional, and assigning to it is assigning to a
parameter.
:::

<span class="listing-caption"><strong>Listing 15-7</strong>, <code>main.cee</code>: The slot's type lacks its parentheses. This does not compile.</span>

<!-- ceesafe: fail N0211 -->
```ceesafe
struct Node {
    value: i32;
    next: Box<Node>?;
}

fn fill(slot: borrow mut Box<Node>?, value: i32) {
    slot = Box.create(Node { value: value, next: null })!;
}

fn main() {
    var first: Box<Node>? = null;
    fill(mut first, 1);
}
```

```text
main.cee:7:5: error[N0211]: cannot assign to 'slot': parameters are immutable; its type is an optional 'borrow mut' reference, not a reference to an optional: write 'borrow mut (T?)' to change the caller's optional through it
  |
7 |     slot = Box.create(Node { value: value, next: null })!;
  |     ^^^^
```

Write `slot: borrow mut (Box<Node>?)`, as Listing 15-3 does.

::: warning Mistake: a box of a borrow
A box owns its contents, and can be moved anywhere and kept for any time. A view or a borrow is tied to another
value, so a box cannot hold one.
:::

<span class="listing-caption"><strong>Listing 15-8</strong>, <code>main.cee</code>: A box of a <code>StringView</code>. This does not compile.</span>

<!-- ceesafe: fail N0246 -->
```ceesafe
fn main() {
    let text = String.from("hello")!;
    let b = Box.create(text.view())!;
    Console.writeLine(b);
}
```

```text
main.cee:3:24: error[N0246]: a 'Box' cannot hold a reference or slice in v0
  |
3 |     let b = Box.create(text.view())!;
  |                        ^^^^^^^^^^^
```

Box the `String` itself, or keep the view in a local. The diagnostics say "reference" for a borrow
([Borrowing](./ch04-02-borrowing) says why).

::: details Coming from Rust
`Box<T>` is `Box<T>`, with three differences. `Box.create` returns a `Result`, where `Box::new` aborts when memory
runs out. There is no `*b`: a box is read through wherever a `T` is expected, and `let t: T = b;` is `*b` by
value. And the allocator is a type parameter in stable use: `Box<T, FailingAllocator>`.
:::

::: details Coming from C#, Java, Go, Python or TypeScript
Every object of a class is what CeeSafe calls a box: a value on the heap, reached by an address. The difference is
that a `Box` has exactly one owner and is freed when that owner ends, with no garbage collector. A struct in
CeeSafe is not boxed unless you box it.
:::

::: details Coming from C or C++
`Box.create(v)` is `malloc` plus the move of `v` into the block, with the null check in the return type. A
`Box<T>` is a `T *` that is never null and is freed exactly once, as `std::unique_ptr<T>` is.
:::

## Summary

- `Box.create(v)` returns `Result<Box<T>, AllocError>`: one value on the heap, owned by the box.
- A box is one address and a move type. Its contents are reached through it, and are mutable when the box is
  (`N0211`).
- A type cannot contain itself (`N0227`), but it can contain a `Box` of itself: lists and trees are built this way.
- The contents of a box in a local can be moved out: the whole value where a `T` is expected, fields by name,
  payloads by `match`.
- A box cannot hold a borrow or a view (`N0246`).
- `Box.intoRaw` and `Box.fromRaw` hand a box to C and take it back. Chapter 20 covers them.

Next: [destruction](./ch15-02-destruction), on what happens when a value's owner ends.
