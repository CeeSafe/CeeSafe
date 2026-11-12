---
title: An example program
summary: One small program that prices an order, written four times - with loose variables, with a struct, with a struct that owns a String and so moves, and with functions that borrow the struct and print its fields.
assumes: [ch05-01-defining-structs, ch04-02-borrowing, ch03-03-functions]
keywords: [struct, borrow, move, refactoring, printing a struct, Console.writeLine]
diagnostics: [N0207, N0250]
specification: ["type-system.md §12"]
---

# An example program

**This section teaches** when a struct is worth having, by writing one program four times. It assumes
[Defining and making structs](./ch05-01-defining-structs), [Functions](./ch03-03-functions) and
[Borrowing](./ch04-02-borrowing).

The program works out the price of an order: a quantity of one item, each at the same unit price. Prices are whole
numbers of cents.

## With loose variables

<span class="listing-caption"><strong>Listing 5-10</strong>, <code>main.cee</code>: The order is two variables, and the function takes two parameters.</span>

```ceesafe
fn total(quantity: u32, unitPrice: u32) -> u32 {
    return quantity * unitPrice;
}

fn main() {
    let quantity: u32 = 3;
    let unitPrice: u32 = 250;
    Console.write("total in cents: ");
    Console.writeLine(total(quantity, unitPrice));
}
```

```text
total in cents: 750
```

The program is correct. What is wrong with it is what it does not say. `total` takes two `u32` values, and nothing
tells the reader, or the compiler, that they are two parts of one order. A call that gives them in the other order,
`total(unitPrice, quantity)`, compiles. Here it even gives the same answer; for most functions it does not. And
each new fact about the order is one more variable and one more parameter in every function that needs it.

## With a struct

<span class="listing-caption"><strong>Listing 5-11</strong>, <code>main.cee</code>: The order is one value, and the function takes one parameter.</span>

```ceesafe
struct Order {
    quantity: u32;
    unitPrice: u32;
}

fn total(order: Order) -> u32 {
    return order.quantity * order.unitPrice;
}

fn main() {
    let order = Order { quantity: 3, unitPrice: 250 };
    Console.write("total in cents: ");
    Console.writeLine(total(order));
}
```

```text
total in cents: 750
```

Now the signature `fn total(order: Order) -> u32` says what the function is about. The body names the two fields,
so they cannot be given in the wrong order. The literal names them too: `Order { quantity: 3, unitPrice: 250 }` is
easier to read than `total(3, 250)`.

`total` takes its `Order` by value. `Order` has two number fields, so it is a Copy type: the call copies the order,
and `main` could go on using `order` after it.

## The order gets a name

An order is for something. Add the item's name as a `String` field. The line that makes the string is
explained in [Defining and making structs](./ch05-01-defining-structs#structs-that-own-their-fields): `String.from`
copies text into a new `String`, and `!` stops the program if memory runs out.

<span class="listing-caption"><strong>Listing 5-12</strong>, <code>main.cee</code>: The struct owns a <code>String</code>, and <code>total</code> still takes it by value. This does not compile.</span>

<!-- ceesafe: fail N0250 -->
```ceesafe
struct Order {
    item: String;
    quantity: u32;
    unitPrice: u32;
}

fn total(order: Order) -> u32 {
    return order.quantity * order.unitPrice;
}

fn main() {
    let item = String.from("lamp")!;
    let order = Order { item: item, quantity: 3, unitPrice: 250 };
    Console.write("total in cents: ");
    Console.writeLine(total(order));
    Console.writeLine(order.item.view());
}
```

```text
main.cee:16:23: error[N0250]: 'order' is used here after it was moved (on at least one path)
   |
16 |     Console.writeLine(order.item.view());
   |                       ^^^^^^^^^^^^^^^^^
```

Nothing in `total` changed, but its meaning did. With a `String` field, `Order` is no longer Copy. Passing it by
value now moves it: `total(order)` hands the order to the function, the function works out a number and destroys
the order when it returns, and `main` has nothing left to print.

`total` only reads two fields. It has no reason to take the order.

## Borrowing the struct

<span class="listing-caption"><strong>Listing 5-13</strong>, <code>main.cee</code>: Both functions borrow the order, and <code>main</code> keeps it.</span>

```ceesafe
struct Order {
    item: String;
    quantity: u32;
    unitPrice: u32;
}

fn total(order: borrow Order) -> u32 {
    return order.quantity * order.unitPrice;
}

fn show(order: borrow Order) {
    Console.write("item: ");
    Console.writeLine(order.item.view());
    Console.write("quantity: ");
    Console.writeLine(order.quantity);
    Console.write("unit price in cents: ");
    Console.writeLine(order.unitPrice);
}

fn main() {
    let item = String.from("lamp")!;
    let order = Order { item: item, quantity: 3, unitPrice: 250 };
    show(order);
    Console.write("total in cents: ");
    Console.writeLine(total(order));
}
```

```text
item: lamp
quantity: 3
unit price in cents: 250
total in cents: 750
```

- The parameter type is `borrow Order`. The function may read the order, and `main` still owns it afterwards.
- The calls do not change: `total(order)` and `show(order)`. A shared borrow for a parameter is made at the call
  without a word.
- Inside the functions, fields are read through the borrow in the same way, `order.quantity`.
- `main` uses `order` twice, and could use it again.

::: tip Rule
A function that only reads a struct takes `borrow S`. Then it works the same whether the struct is Copy or not,
and it keeps working when the struct later gets a field that owns something.
:::

## Printing a struct

`show` prints the order one field at a time, because that is the only way. `Console.writeLine` prints numbers,
booleans and text. `order.item.view()` gives the text of the `String` in a form it can print. CeeSafe has no
built-in way to print a whole struct for debugging, and no attribute that makes one. A function like `show` is how
a program prints its own types, and you choose what it prints.

## Mistakes

::: warning Mistake: printing a struct
Passing the struct itself to `Console.writeLine` does not compile. The message lists what can be printed.
:::

<span class="listing-caption"><strong>Listing 5-14</strong>, <code>main.cee</code>: A whole struct is given to <code>Console.writeLine</code>. This does not compile.</span>

<!-- ceesafe: fail N0207 -->
```ceesafe
struct Order {
    quantity: u32;
    unitPrice: u32;
}

fn main() {
    let order = Order { quantity: 3, unitPrice: 250 };
    Console.writeLine(order);
}
```

```text
main.cee:8:23: error[N0207]: 'Console.writeLine' can print integers, floats, bools and string views, not 'Order'
  |
8 |     Console.writeLine(order);
  |                       ^^^^^
```

Print the fields, each with its own call, as `show` does in Listing 5-13.

::: warning Mistake: taking a struct by value to read it
This is Listing 5-12. It compiles while the struct is Copy, and stops compiling at every caller on the day the
struct gets a field that owns something. Write `borrow Order` from the start.
:::

::: details Coming from Rust
There is no `#[derive(Debug)]`, no `{:?}` and no `dbg!`. Write a function that prints the fields. The borrow at
the call is implicit: `total(order)`, not `total(&order)`.
:::

::: details Coming from C#, Java, TypeScript or Python
There is no `ToString()`, `toString()` or `__repr__` that the console calls for you, and `Console.writeLine` does
not take an object. Passing a struct to a function does not share it: it copies it or moves it, unless the
parameter is a `borrow`.
:::

## Summary

- Values that belong together are one struct, and a function about them takes one parameter.
- A struct with only Copy fields is copied when passed by value. Give it a field that owns something, and passing
  it by value moves it (`N0250` if it is used afterwards).
- A function that only reads a struct takes `borrow S`; the call is written the same.
- A struct is printed field by field. `Console.writeLine` does not take a struct (`N0207`), and there is no debug
  printing.

Next: [methods](./ch05-03-methods), which put functions like `total` and `show` inside the struct.
