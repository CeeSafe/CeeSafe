---
title: Methods
summary: Functions that belong to a struct - methods written in the struct's body or in an impl block, the receivers borrow this, borrow mut this and this by value, static functions that make a value, and how each is called.
assumes: [ch05-01-defining-structs, ch04-02-borrowing, ch03-03-functions]
keywords: [method, this, "borrow this", "borrow mut this", receiver, static function, constructor, impl]
diagnostics: [N0200, N0228, N0233, N0248, N0250]
specification: ["type-system.md §12", "type-system.md §21.1"]
---

# Methods

**This section teaches** how to write functions that belong to a struct, the three ways such a function receives
the struct's value, and functions of a struct that need no value. It assumes
[Defining and making structs](./ch05-01-defining-structs), [Functions](./ch03-03-functions) and
[Borrowing](./ch04-02-borrowing).

A **method** is a function declared inside a struct whose first parameter is `this`. It is called on a value of the
struct, with a dot: `order.total()`.

## A method that reads

<span class="listing-caption"><strong>Listing 5-15</strong>, <code>main.cee</code>: A method in the struct's body, and a call of it.</span>

```ceesafe
struct Order {
    quantity: u32;
    unitPrice: u32;

    fn total(borrow this) -> u32 {
        return this.quantity * this.unitPrice;
    }
}

fn main() {
    let order = Order { quantity: 3, unitPrice: 250 };
    Console.writeLine(order.total());
}
```

```text
750
```

- The method is written in the struct's braces, after the fields.
- Its first parameter, `borrow this`, is the **receiver**. It has no type written: its type is the struct. Here
  `this` is a shared borrow of an `Order`.
- The body reaches the fields through it: `this.quantity`. A field's name alone is not in scope.
- In the call `order.total()`, `order` becomes `this`. The other arguments, if there are any, go in the
  parentheses.

## The three receivers

The receiver says what the method does with the value it is called on.

| Receiver | The method | The call `v.m()` |
|---|---|---|
| `borrow this` | reads the value | borrows `v` for the call |
| `borrow mut this` | reads and changes the value | borrows `v` exclusively for the call; `v` must be a `var` |
| `this` | gets the value itself | copies `v` if the struct is Copy, and moves `v` if it is not |

These are the three ways any function takes a parameter: `borrow T`, `borrow mut T` and `T`. Most methods are
`borrow this`.

### `borrow mut this`

<span class="listing-caption"><strong>Listing 5-16</strong>, <code>main.cee</code>: A method that changes the value it is called on.</span>

```ceesafe
struct Order {
    quantity: u32;
    unitPrice: u32;

    fn total(borrow this) -> u32 {
        return this.quantity * this.unitPrice;
    }

    fn add(borrow mut this, more: u32) {
        this.quantity += more;
    }
}

fn main() {
    var order = Order { quantity: 3, unitPrice: 250 };
    order.add(2);
    Console.writeLine(order.quantity);
    Console.writeLine(order.total());
}
```

```text
5
1250
```

`add` changes `order` in place, so `order` is a `var`. The call is `order.add(2)`: a method call does not write
`mut` before the value, as a call of a plain function with a `borrow mut` parameter does.

### `this` by value, and static functions

A function in a struct with no `this` is a **static function**. It is not called on a value. It is called on the
struct's name: `Order.create("lamp", 250)`. Its usual job is to make a value, so that the code that needs an order
does not repeat how one is built. CeeSafe has no special constructor: a static function that returns the struct
does that work, and you name it as you like.

<span class="listing-caption"><strong>Listing 5-17</strong>, <code>main.cee</code>: A static function makes an order, and a method that takes <code>this</code> by value ends it.</span>

```ceesafe
struct Order {
    item: String;
    quantity: u32;
    unitPrice: u32;

    fn create(name: StringView, unitPrice: u32) -> Order {
        let item = String.from(name)!;
        return Order { item: item, quantity: 1, unitPrice: unitPrice };
    }

    fn cancel(this) -> String {
        return this.item;
    }
}

fn main() {
    let order = Order.create("lamp", 250);
    Console.writeLine(order.quantity);
    let item = order.cancel();
    Console.writeLine(item.view());
}
```

```text
1
lamp
```

`create` takes the name as a `StringView`, the type of text such as `"lamp"`, and copies it into a `String` that
the order owns. `String.from(name)` makes that `String`, and `!` stops the program if memory runs out;
[Strings](./ch08-02-strings) covers both types.

`cancel` takes `this` by value. This `Order` owns a `String`, so it is not Copy, and `order.cancel()` moves the
order into the method. The method moves the item's name out and returns it; the rest of the order is destroyed.
After the call, `order` is empty. A method takes `this` by value when the value should not be used again, or when
it turns the value into something else.

On a Copy struct, a `this` method gets a copy, and the caller's value stays as it was. Inside such a method `this`
cannot be assigned: to change the caller's value, use `borrow mut this`.

::: tip Rule
In a struct, a function whose first parameter is `this`, `borrow this` or `borrow mut this` is a method, called
as `value.name(…)`. A function without one is a static function, called as `Struct.name(…)`.

*Specification: `type-system.md` §12, Methods.*
:::

## `impl` blocks

Methods and static functions can also be written outside the struct's body, in a block that begins `impl` and the
struct's name. They are the same as if they were in the body. Use it to keep a long struct's fields together, or
to put a struct's functions in another file of the package.

<span class="listing-caption"><strong>Listing 5-18</strong>, <code>main.cee</code>: The method of Listing 5-15, in an <code>impl</code> block.</span>

```ceesafe
struct Order {
    quantity: u32;
    unitPrice: u32;
}

impl Order {
    fn total(borrow this) -> u32 {
        return this.quantity * this.unitPrice;
    }
}

fn main() {
    let order = Order { quantity: 3, unitPrice: 250 };
    Console.writeLine(order.total());
}
```

```text
750
```

`impl` has a second use, with interfaces, which [chapter 10](./ch10-02-interfaces) teaches.

A method or a static function can be called from any file of the package, wherever it is written. With `private`
before `fn`, only the file that declares it can call it; [Files and `public`](./ch07-02-files-and-public) covers
that.

## Mistakes

::: warning Mistake: a method with the name of a field
A struct's fields, methods and static functions share one set of names. A method that returns the field
`quantity` cannot be called `quantity`.
:::

<span class="listing-caption"><strong>Listing 5-19</strong>, <code>main.cee</code>: A field and a method have the same name. This does not compile.</span>

<!-- ceesafe: fail N0228 -->
```ceesafe
struct Order {
    quantity: u32;
    unitPrice: u32;

    fn quantity(borrow this) -> u32 {
        return this.quantity;
    }
}

fn main() {
    let order = Order { quantity: 3, unitPrice: 250 };
    Console.writeLine(order.quantity);
}
```

```text
main.cee:5:8: error[N0228]: 'Order' already has a member named 'quantity'
  |
5 |     fn quantity(borrow this) -> u32 {
  |        ^^^^^^^^
```

Read the field (`order.quantity`), or give the method another name, such as `count`.

::: warning Mistake: using a value after a method took it
A method with a plain `this` moves a struct that is not Copy. The call looks like any other.
:::

<span class="listing-caption"><strong>Listing 5-20</strong>, <code>main.cee</code>: <code>order</code> is read after <code>cancel</code> took it. This does not compile.</span>

<!-- ceesafe: fail N0250 -->
```ceesafe
struct Order {
    item: String;
    quantity: u32;

    fn cancel(this) -> String {
        return this.item;
    }
}

fn main() {
    let item = String.from("lamp")!;
    let order = Order { item: item, quantity: 1 };
    let name = order.cancel();
    Console.writeLine(name.view());
    Console.writeLine(order.quantity);
}
```

```text
main.cee:15:5: error[N0250]: 'order.quantity' is used here after it was moved (on at least one path)
   |
15 |     Console.writeLine(order.quantity);
   |     ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
   = note: the value was moved here (13:5)
```

Read what you need before the call, or make the method `borrow this` if it does not need to own the value.

::: warning Mistake: a `borrow mut this` method on a `let`
The method changes the value, so the variable must be a `var`.
:::

<span class="listing-caption"><strong>Listing 5-21</strong>, <code>main.cee</code>: <code>add</code> is called on a <code>let</code>. This does not compile.</span>

<!-- ceesafe: fail N0248 -->
```ceesafe
struct Order {
    quantity: u32;
    unitPrice: u32;

    fn add(borrow mut this, more: u32) {
        this.quantity += more;
    }
}

fn main() {
    let order = Order { quantity: 3, unitPrice: 250 };
    order.add(2);
    Console.writeLine(order.quantity);
}
```

```text
main.cee:12:5: error[N0248]: cannot borrow 'order' as mutable: 'order' was declared with 'let'; use 'var' for a mutable variable
   |
12 |     order.add(2);
   |     ^^^^^
```

::: warning Mistake: a static function called on a value
A static function has no `this`, so there is nothing for a value to become. Call it on the struct's name.
:::

<span class="listing-caption"><strong>Listing 5-22</strong>, <code>main.cee</code>: <code>single</code> is static and is called on <code>order</code>. This does not compile.</span>

<!-- ceesafe: fail N0233 -->
```ceesafe
struct Order {
    quantity: u32;
    unitPrice: u32;

    fn single(unitPrice: u32) -> Order {
        return Order { quantity: 1, unitPrice: unitPrice };
    }
}

fn main() {
    let order = Order { quantity: 3, unitPrice: 250 };
    let other = order.single(100);
    Console.writeLine(other.quantity);
}
```

```text
main.cee:12:23: error[N0233]: 'Order.single' is a static function; call it as Order.single(...)
   |
12 |     let other = order.single(100);
   |                       ^^^^^^
```

The same code reports the opposite mistake, a method called on the struct's name.

::: warning Mistake: a field without `this.`
In a method, a field is `this.quantity`. The bare name is looked up as a variable, and there is none.
:::

<span class="listing-caption"><strong>Listing 5-23</strong>, <code>main.cee</code>: The method names a field without <code>this</code>. This does not compile.</span>

<!-- ceesafe: fail N0200 -->
```ceesafe
struct Order {
    quantity: u32;
    unitPrice: u32;

    fn doubled(borrow this) -> u32 {
        return quantity * 2;
    }
}

fn main() {
    let order = Order { quantity: 3, unitPrice: 250 };
    Console.writeLine(order.doubled());
}
```

```text
main.cee:6:16: error[N0200]: cannot find 'quantity' in this scope
  |
6 |         return quantity * 2;
  |                ^^^^^^^^
```

::: details Coming from Rust
`this` is `self`: `borrow this` is `&self`, `borrow mut this` is `&mut self`, and `this` is `self`. Methods may be
written in the struct's body as well as in an `impl` block. A static function is called with a dot,
`Order.create(…)`, not `Order::create(…)`. A method and a field cannot share a name, so there is no getter named
after its field.
:::

::: details Coming from C#, Java or TypeScript
`this` is declared, as the first parameter, and how it is declared matters: a method that changes a field must say
`borrow mut this`. `this.` is never optional. There is no constructor and no `new`: write a static function that
returns the struct. A struct's functions are not `static` by a keyword; they are static by having no `this`.
:::

## Summary

- A method is a function in a struct, or in an `impl` block for it, whose first parameter is the receiver. It is
  called as `value.name(…)`.
- `borrow this` reads; `borrow mut this` changes the caller's value and needs a `var` (`N0248`); `this` takes the
  value, copying a Copy struct and moving any other (`N0250` if it is used afterwards).
- A function with no receiver is a static function, called as `Struct.name(…)` (`N0233` otherwise). Constructors
  are static functions.
- Fields are reached as `this.field` (`N0200` for the bare name), and a method cannot have the name of a field
  (`N0228`).

Next: [chapter 6](./ch06-00-enums), on enums, the types whose value is one of several alternatives.
