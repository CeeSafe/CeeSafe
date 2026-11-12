---
title: What ownership is
summary: Every value has one owner - copy types are copied and move types are moved by assignment, by a call and by return, a moved variable cannot be used (N0250), and a value is destroyed when its owner goes away.
assumes: [ch03-01-variables, ch03-02-data-types, ch03-03-functions]
keywords: [owner, ownership, move, copy, Copy type, move type, drop, destruction, String, stack, heap, clone]
diagnostics: [N0250]
specification: ["ownership.md O1, O2, O4", "type-system.md §14"]
---

# What ownership is

**This section teaches** what it means for a variable to own a value: which values are copied and which are moved,
what you may do with a variable after a move, and when a value is destroyed. It assumes
[variables](./ch03-01-variables), [data types](./ch03-02-data-types) and [functions](./ch03-03-functions). The
chapter's [opening page](./ch04-00-ownership) says what problem ownership solves.

::: tip Rule
Every value has exactly one owner. When the owner goes away, the value is destroyed.

*Specification: `ownership.md`, invariant O1.*
:::

The owner is most often a variable. What happens when you assign that variable to another depends on the type of
the value.

## Copy types

<span class="listing-caption"><strong>Listing 4-1</strong>, <code>main.cee</code>: An integer is copied. Both variables can be used.</span>

```ceesafe
fn main() {
    let a = 5;
    let b = a;
    Console.writeLine(a);
    Console.writeLine(b);
}
```

```text
5
5
```

`let b = a;` copies the 5. Now there are two values with two owners, and neither knows about the other. Integers,
floats, `bool` and arrays of them are **Copy types**: small values of a fixed size, for which a copy is cheap and
complete. A struct is Copy when all its fields are Copy and it has no `drop` block (shown below);
[chapter 5](./ch05-00-structs) covers structs.

## Move types

A `String` is text that can grow. It cannot be a value of a fixed size, so it has two parts: a small part in the
variable, which records where the text is and how long it is, and the text itself in memory that the `String`
asks for while the program runs. The variable's part lives on the **stack**, the memory of the running function,
which is given back when the function returns. The text lives on the **heap**, memory that stays until it is freed.

<span class="listing-caption"><strong>Listing 4-2</strong>, <code>main.cee</code>: A <code>String</code> is moved from <code>s</code> to <code>t</code>.</span>

```ceesafe
fn main() {
    var s = String.create();
    s.append("hello")!;
    let t = s;
    Console.writeLine(t);
}
```

```text
hello
```

`String.create()` makes an empty `String` and `append` adds text to it. Appending may need more memory, which can
fail; the `!` stops the program if it does ([chapter 9](./ch09-00-error-handling) covers errors).

`let t = s;` does not copy the text. It **moves** the `String`: `t` is the owner now, and `s` is empty. If both
variables owned the same text, both would free it. A move keeps the rule of one owner, and costs the same however
long the text is.

::: tip Rule
Assigning a value of a move type, passing it to a function, returning it, or storing it in a struct moves it. The
variable it came from is then empty, and using it is an error until it is assigned again.

*Specification: `type-system.md` §14.3.*
:::

**Move types** are the types that own something: `String`, `Vector<T>` (a list that grows) and `Box<T>` (one value
on the heap), any struct with a field of a move type, and any struct with a `drop` block.

## Functions take and give ownership

A parameter is a variable of the function. Passing a `String` to a `String` parameter moves it in. Returning one
moves it out to the caller.

<span class="listing-caption"><strong>Listing 4-3</strong>, <code>main.cee</code>: A <code>String</code> is made in one function, moved through a second, and ends in a third.</span>

```ceesafe
fn make() -> String {
    var s = String.create();
    s.append("hello")!;
    return s;
}

fn addWorld(text: String) -> String {
    var longer = text;
    longer.append(", world")!;
    return longer;
}

fn shout(text: String) {
    Console.write(text);
    Console.writeLine("!");
}

fn main() {
    let a = make();
    let b = addWorld(a);
    shout(b);
}
```

```text
hello, world!
```

- `make` returns `s`: the `String` moves to `a` in `main`. It is not destroyed when `make` ends, because `s` no
  longer owns it.
- `addWorld(a)` moves the `String` into `text`, and the result moves to `b`. From here `a` is empty.
- `shout(b)` moves it into `shout`'s `text`. Nothing moves it on, so it is destroyed when `shout` returns.

There is one `String` in this program, and at each line exactly one variable owns it.

## Destruction

To see when values are destroyed, give a struct a `drop` block. The block runs when a value of the struct is
destroyed; inside it, `this` is the value.

<span class="listing-caption"><strong>Listing 4-4</strong>, <code>main.cee</code>: Each <code>Ticket</code> prints a line when it is destroyed.</span>

```ceesafe
struct Ticket {
    id: i32;
    drop {
        Console.write("destroy ticket ");
        Console.writeLine(this.id);
    }
}

fn stamp(t: Ticket) {
    Console.writeLine("stamping");
}

fn main() {
    let a = Ticket { id: 1 };
    let b = Ticket { id: 2 };
    {
        let c = Ticket { id: 3 };
        Console.writeLine("end of the inner block");
    }
    stamp(a);
    var d = Ticket { id: 4 };
    d = Ticket { id: 5 };
    Console.writeLine("end of main");
}
```

```text
end of the inner block
destroy ticket 3
stamping
destroy ticket 1
destroy ticket 4
end of main
destroy ticket 5
destroy ticket 2
```

- Ticket 3 is destroyed at the `}` of the inner block, where its owner `c` ends.
- Ticket 1 was moved into `stamp`, so it is destroyed when `stamp` returns, not at the end of `main`.
- Ticket 4 is destroyed by the assignment that replaces it.
- At the end of `main`, `d` (holding 5) and then `b` are destroyed: the reverse of the order they were declared in.
  `a` is empty and nothing happens for it.

::: tip Rule
A value is destroyed exactly once: when its owner's block ends, when its owner is assigned a new value, or, for a
value that was never given a name, at the end of its statement. Variables of a block are destroyed in the reverse
order of their declaration.

*Specification: `type-system.md` §14.5.*
:::

A `String` or a `Vector<T>` gives its heap memory back in the same way and at the same places. You write no call to
free anything.

## Keeping a value and giving one away

A move leaves you without the value. When you still need it, there are three answers.

**Borrow it.** Most functions only need to read a value or change it where it is. The next section,
[Borrowing](./ch04-02-borrowing), shows how a function does that and leaves the value with its owner. This is
the usual answer.

**Get it back.** A function that takes a value can return it, as `addWorld` does in Listing 4-3.

**Make a second value.** CeeSafe never copies a move type for you, and `String` and `Vector<T>` have no `clone`
method: a copy asks for memory, so you write it where it happens.

<span class="listing-caption"><strong>Listing 4-5</strong>, <code>main.cee</code>: <code>copyOf</code> makes a second <code>String</code> with the same text. <code>shout</code> takes the copy, and <code>s</code> is kept.</span>

```ceesafe
fn copyOf(text: StringView) -> String {
    var copy = String.create();
    copy.append(text)!;
    return copy;
}

fn shout(text: String) {
    Console.write(text);
    Console.writeLine("!");
}

fn main() {
    var s = String.create();
    s.append("hello")!;
    shout(copyOf(s));
    Console.writeLine(s);
}
```

```text
hello!
hello
```

`StringView` is a borrow of text, which [Slices and views](./ch04-03-slices-and-views) explains: `copyOf` reads
the text of `s` and does not take `s`.

## Mistakes

::: warning Mistake: using a variable after its value moved
After `let t = s;` the `String` belongs to `t`. A reader from a garbage-collected language expects two names for
one object. There is one name, and the other is empty.
:::

<span class="listing-caption"><strong>Listing 4-6</strong>, <code>main.cee</code>: <code>s</code> is used after its value moved to <code>t</code>. This does not compile.</span>

<!-- ceesafe: fail N0250 -->
```ceesafe
fn main() {
    var s = String.create();
    s.append("hello")!;
    let t = s;
    Console.writeLine(s);
    Console.writeLine(t);
}
```

```text
main.cee:5:23: error[N0250]: 's' is used here after it was moved (on at least one path)
  |
5 |     Console.writeLine(s);
  |                       ^
```

The message points at the use, not at the move: look upwards from it for the place where `s` was assigned, passed
or returned. "On at least one path" means the compiler also reports a move that happens only in one branch of an
`if`, or in an earlier turn of a loop. To fix it, use `t`, or do not move: borrow, or make a copy as Listing 4-5
does.

::: warning Mistake: passing a value to a function and using it afterwards
A call moves as an assignment does. Nothing at the call marks it; the parameter's type, `String`, is what says the
function takes the value.
:::

<span class="listing-caption"><strong>Listing 4-7</strong>, <code>main.cee</code>: The first call takes <code>s</code>, so the second has nothing to pass. This does not compile.</span>

<!-- ceesafe: fail N0250 -->
```ceesafe
fn shout(text: String) {
    Console.write(text);
    Console.writeLine("!");
}

fn main() {
    var s = String.create();
    s.append("hello")!;
    shout(s);
    shout(s);
}
```

```text
main.cee:10:5: error[N0250]: 's' is used here after it was moved (on at least one path)
   |
10 |     shout(s);
   |     ^^^^^^^^^
```

`shout` only prints the text, so it should not take the `String` at all. Declare the parameter as a borrow,
`text: borrow String`, and both calls compile unchanged: that is the subject of [Borrowing](./ch04-02-borrowing).

::: details Coming from C#, Java, Python, Go or TypeScript
In those languages `let t = s` on an object gives a second name for the same object, and the garbage collector
frees it when no name is left. In CeeSafe a value has one name that owns it. Sharing is done by borrowing, for a
limited time that the compiler checks.
:::

::: details Coming from Rust
The model is Rust's: Copy and move types, moves by assignment, call and return, destruction at the end of scope in
reverse order. A `drop` block in the struct is what `impl Drop` is. Two things differ: there is no `Clone` trait
and no `.clone()` on `String` or `Vector<T>`, so a copy is written out; and a panic stops the program without
running `drop` blocks.
:::

::: details Coming from C or C++
A move is not a copy that leaves the source in some valid state, as `std::move` does: the compiler forbids any use
of the source. Destruction is what a C++ destructor gives you, with the compiler checking that no value is
destroyed twice or used afterwards.
:::

## Summary

- Every value has exactly one owner.
- Copy types (integers, floats, `bool`, arrays, structs of them without a `drop` block) are copied by assignment.
- Move types (`String`, `Vector<T>`, `Box<T>`, structs that hold one or have a `drop` block) are moved by
  assignment, by a call and by `return`. The variable they came from is then empty.
- Using an empty variable is `N0250`.
- A value is destroyed once, when its owner goes away; a block's variables are destroyed in reverse order of
  declaration.
- To keep a value, borrow it, get it back from the function, or make a second value by hand.

Next: [Borrowing](./ch04-02-borrowing), which is how most code uses a value without owning it.
