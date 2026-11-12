---
title: Closures
summary: Functions as values and closures - the literal (x) => body, capturing by shared borrow, by mutable borrow and by move, the types fn, borrow fn and borrow mut fn, closures in fields, and boxed closures that outlive their function.
assumes: [ch03-03-functions, ch04-02-borrowing, ch10-03-regions]
keywords: [closure, function value, fn, "borrow fn", "borrow mut fn", capture, move, "dyn fn", Box, environment]
diagnostics: [N0236, N0240, N0245, N0299]
specification: ["type-system.md §36.1", "type-system.md §36.2", "type-system.md §36.5"]
---

# Closures

**This section teaches** how to pass code to a function: a function's name as a value, a closure written in
place, what a closure may do with the variables around it, and the type each kind has. It assumes functions
([Functions](./ch03-03-functions)) and the rule of borrowing ([Borrowing](./ch04-02-borrowing)); one listing
uses a region parameter ([Regions](./ch10-03-regions)).

## Functions are values

The name of a function is a value. Its type is written `fn(parameter types) -> result`, and a parameter of
that type is called like a function.

<span class="listing-caption"><strong>Listing 13-1</strong>, <code>main.cee</code>: A function is passed to another, kept in a variable, and written in place.</span>

```ceesafe
fn double(x: i32) -> i32 => x * 2;

fn negate(x: i32) -> i32 => -x;

fn applyTwice(f: fn(i32) -> i32, x: i32) -> i32 {
    return f(f(x));
}

fn main() {
    Console.writeLine(applyTwice(double, 5));
    var op: fn(i32) -> i32 = double;
    op = negate;
    Console.writeLine(op(5));
    Console.writeLine(applyTwice((x) => x + 1, 5));
}
```

```text
20
-5
7
```

The last call passes a **closure literal**: `(x) => x + 1`. The parameters are in parentheses, `()` for none,
and the body after `=>` is an expression or a block. The type of `x` is not written, because `applyTwice`
expects a `fn(i32) -> i32` and the compiler takes the types from there.

## Capturing

A closure literal may use the locals and parameters of the function it is written in. It **captures** them.

<span class="listing-caption"><strong>Listing 13-2</strong>, <code>main.cee</code>: One closure reads <code>limit</code>, another changes <code>total</code>, and a named function goes where a closure is expected.</span>

```ceesafe
fn countIf(values: Slice<i32>, keep: borrow fn(i32) -> bool) -> i32 {
    var n = 0;
    for (v in values) {
        if (keep(v)) {
            n += 1;
        }
    }
    return n;
}

fn visitAll(values: Slice<i32>, visit: borrow mut fn(i32)) {
    for (v in values) {
        visit(v);
    }
}

fn isEven(v: i32) -> bool => v % 2 == 0;

fn main() {
    let values = [3, 8, 1, 9, 4];
    let limit = 4;
    Console.writeLine(countIf(values[..], (v) => v > limit));
    Console.writeLine(countIf(values[..], isEven));
    var total = 0;
    visitAll(values[..], (v) => { total += v; });
    Console.writeLine(total);
}
```

```text
2
2
25
```

A closure captures by borrowing. `(v) => v > limit` only reads `limit`, so it holds a shared borrow of it.
`(v) => { total += v; }` changes `total`, so it holds an exclusive borrow. The kind of capture decides the
closure's type:

| The literal | Its type | It may be passed where the parameter is |
|---|---|---|
| captures nothing | `fn(T) -> R`, a plain function value | `fn`, `borrow fn`, `borrow mut fn` |
| only reads its captures | `borrow fn(T) -> R` | `borrow fn`, `borrow mut fn` |
| changes a capture | `borrow mut fn(T) -> R` | `borrow mut fn` |

So a function that takes a `borrow mut fn` accepts every closure, and a function that takes a plain `fn`
accepts only code that captures nothing. A named function such as `isEven` converts to either closure type.

::: tip Rule
A closure literal borrows each variable it names: exclusively if its body changes the variable, shared
otherwise. Its type is `borrow mut fn(…)` if any capture is exclusive, `borrow fn(…)` if all are shared, and
the plain `fn(…)` if there is none.

*Specification: `type-system.md` §36.2.*
:::

## A closure is a borrow

A closure can be kept in a `let`. From then on the rule of borrowing applies to what it captured, until the
closure's last use.

<span class="listing-caption"><strong>Listing 13-3</strong>, <code>main.cee</code>: After the last call of <code>above</code>, <code>limit</code> may change. <code>add</code> changes <code>total</code> each time it is called.</span>

```ceesafe
fn main() {
    var limit = 4;
    let above = (v: i32) => v > limit;
    Console.writeLine(above(9));
    limit = 8;
    Console.writeLine(limit);

    var total = 0;
    let add = (v: i32) => { total += v; };
    add(3);
    add(4);
    Console.writeLine(total);
}
```

```text
true
8
7
```

Here nothing gives the parameter types, so the literals write them: `(v: i32)`. A closure is declared with
`let`, also one that changes its captures: like any borrow, it cannot be pointed at something else.

Because a closure is a borrow, making one allocates nothing, and it cannot outlive what it captured: it
cannot be returned from the function that made it.

## A closure in a field

A struct may hold a function value in a field of type `fn(…)`. It may hold a closure too, and since a closure
is a borrow, the struct then needs a region parameter and the field's type names it: `borrow(r) fn(…)`.

<span class="listing-caption"><strong>Listing 13-4</strong>, <code>main.cee</code>: A struct carries a closure, and a function calls it through the field.</span>

```ceesafe
struct Rule<region r> {
    id: u32;
    test: borrow(r) fn(i32) -> bool;
}

fn count(values: Slice<i32>, rule: borrow Rule) -> i32 {
    var n = 0;
    for (v in values) {
        if (rule.test(v)) {
            n += 1;
        }
    }
    return n;
}

fn main() {
    let values = [3, 8, 1, 9, 4];
    let limit = 4;
    let rule = Rule { id: 1, test: (v) => v > limit };
    Console.writeLine(count(values[..], rule));
}
```

```text
2
```

`rule.test(v)` calls the closure the field holds. A struct cannot have a method with the name of one of its
fields, so the call is never ambiguous.

## Closures that own: `move`

A closure that must outlive its function cannot borrow that function's locals. It has to own them. A **move
literal**, `move (x) => body`, moves each variable it names into the closure, or copies it if its type is
Copy. Such a closure lives on the heap in a `Box`, a value that owns one allocation; chapter 15 covers boxes.
Its type is `Box<dyn fn(T) -> R>`, or `Box<dyn mut fn(T) -> R>` if the body changes what it owns.

<span class="listing-caption"><strong>Listing 13-5</strong>, <code>main.cee</code>: Two functions return closures that own what they captured.</span>

```ceesafe
fn adder(n: i32) -> Box<dyn fn(i32) -> i32> {
    return Box.create(move (x) => x + n)!;
}

fn counter() -> Box<dyn mut fn() -> i32> {
    var count = 0;
    return Box.create(move () => {
        count += 1;
        return count;
    })!;
}

fn main() {
    let addTwo = adder(2);
    Console.writeLine(addTwo(3));
    var next = counter();
    next();
    Console.writeLine(next());
}
```

```text
5
2
```

`Box.create` allocates, so it returns a `Result`, and `!` here stops the program if memory runs out
([Allocation failure](./ch09-03-allocation-failure)). `next` is a `var` because calling a `dyn mut fn`
changes the closure's own `count`.

::: tip Rule
A move literal is written only as the argument of `Box.create(…)` where a `Box<dyn fn…>` or
`Box<dyn mut fn…>` is expected. It may not capture a borrow, a view, a slice or another closure: the box
would outlive what they borrow.

*Specification: `type-system.md` §36.5.*
:::

## Mistakes

::: warning Mistake: changing a captured variable while the closure is still used
The closure holds a borrow of the variable until its last call.
:::

<span class="listing-caption"><strong>Listing 13-6</strong>, <code>main.cee</code>: <code>limit</code> changes between the making of <code>above</code> and its call. This does not compile.</span>

<!-- ceesafe: fail N0240 -->
```ceesafe
fn main() {
    var limit = 4;
    let above = (v: i32) => v > limit;
    limit = 8;
    Console.writeLine(above(5));
}
```

```text
main.cee:4:5: error[N0240]: cannot assign to 'limit' while 'limit' is borrowed
  |
4 |     limit = 8;
  |     ^^^^^^^^^^
  = note: 'limit' is borrowed here (3:17)
  = note: the borrow is used later here (5:23)
```

Change the variable before the closure is made or after its last call, as Listing 13-3 does. If the closure
should have its own value, copy it first: `let fixed = limit;` and capture `fixed`.

::: warning Mistake: a closure that changes a capture, where `borrow fn` is expected
A `borrow fn` may be copied and called from several places, so it must not change anything.
:::

<span class="listing-caption"><strong>Listing 13-7</strong>, <code>main.cee</code>: The closure counts its calls, and <code>countIf</code> takes a <code>borrow fn</code>. This does not compile.</span>

<!-- ceesafe: fail N0299 -->
```ceesafe
fn countIf(values: Slice<i32>, keep: borrow fn(i32) -> bool) -> i32 {
    var n = 0;
    for (v in values) {
        if (keep(v)) {
            n += 1;
        }
    }
    return n;
}

fn main() {
    let values = [3, 8, 1, 9, 4];
    var calls = 0;
    Console.writeLine(countIf(values[..], (v) => {
        calls += 1;
        return v > 4;
    }));
    Console.writeLine(calls);
}
```

```text
main.cee:14:43: error[N0299]: expected 'borrow fn(i32) -> bool', but this closure changes 'calls', so it is a 'borrow mut fn' (type-system.md §36.2)
   |
14 |     Console.writeLine(countIf(values[..], (v) => {
   |                                           ^^^^^^^^
```

Declare the parameter as `keep: borrow mut fn(i32) -> bool`. When you write a function that takes a closure,
prefer `borrow mut fn`: it accepts both kinds. The same code reports a closure that captures where a plain
`fn(…)` is expected.

::: warning Mistake: parameters without types, where nothing gives them
The compiler does not work the types out from the body.
:::

<span class="listing-caption"><strong>Listing 13-8</strong>, <code>main.cee</code>: A literal in a <code>let</code> has no expected type. This does not compile.</span>

<!-- ceesafe: fail N0236 -->
```ceesafe
fn main() {
    let add = (a, b) => a + b;
}
```

```text
main.cee:2:16: error[N0236]: parameter 'a' needs a type: write 'a: T', or use the closure where a function or closure type is expected (type-system.md §36.2)
  |
2 |     let add = (a, b) => a + b;
  |                ^

main.cee:2:19: error[N0236]: parameter 'b' needs a type: write 'b: T', or use the closure where a function or closure type is expected (type-system.md §36.2)
  |
2 |     let add = (a, b) => a + b;
  |                   ^
```

Write `(a: i32, b: i32) => a + b`.

::: warning Mistake: returning a closure that borrows
The locals it captured end with the function.
:::

<span class="listing-caption"><strong>Listing 13-9</strong>, <code>main.cee</code>: A function tries to return a <code>borrow fn</code>. This does not compile.</span>

<!-- ceesafe: fail N0245 -->
```ceesafe
fn above(limit: i32) -> borrow fn(i32) -> bool {
    return (v) => v > limit;
}

fn main() {
    let test = above(4);
    Console.writeLine(test(5));
}
```

```text
main.cee:1:25: error[N0245]: a function without reference parameters cannot return a reference: there is nothing it could borrow from
  |
1 | fn above(limit: i32) -> borrow fn(i32) -> bool {
  |                         ^^^^^^^^^^^^^^^^^^^^^^
```

Return an owning closure, as `adder` does in Listing 13-5, or take the closure's work as a parameter of the
caller.

In a closure's body, `return` returns from the closure, not from the function around it, and `break` and
`continue` cannot leave it. To stop a loop early, write the loop yourself and pass the test as a closure.

::: details Coming from Rust
- `fn(T) -> R` is Rust's `fn` pointer. `borrow fn` is `&dyn Fn`, `borrow mut fn` is `&mut dyn FnMut`, and
  `Box<dyn fn>` is `Box<dyn Fn>`. There are no `Fn` traits and no `impl Fn` parameter: a closure parameter has
  one of these types, and the compiler specialises the call
  ([What they cost](./ch13-04-what-they-cost)).
- A literal without `move` always borrows. With `move` it must go straight into `Box.create`.
- There is no `FnOnce`: a closure cannot move a captured value out.
:::

::: details Coming from C#, Java, TypeScript or Go
A closure there keeps its captured variables alive on the heap for as long as the closure exists. Here a
closure borrows them, on the stack, and the compiler rejects a closure that would outlive them. Keeping a
closure in a list or returning it needs `move` and a `Box`, and from then on the closure has its own copies:
changing the original variable does not change what the closure sees.
:::

::: details Coming from C
`fn(T) -> R` is a function pointer. A closure is a function pointer and its context argument in one value,
with the context made and passed for you.
:::

## Summary

- A function's name is a value of type `fn(T) -> R`. A closure literal is `(x) => expression` or
  `(x) => { … }`.
- A literal borrows what it names: `borrow fn` if it only reads, `borrow mut fn` if it changes something,
  plain `fn` if it captures nothing. Parameter types come from the expected type, or are written.
- While a closure is in use, its captures follow the rule of borrowing (`N0240`), and it cannot leave its
  function (`N0245`).
- A `borrow mut fn` parameter accepts every closure; a `borrow fn` parameter rejects one that changes a
  capture (`N0299`).
- A field of type `fn(…)` or `borrow(r) fn(…)` holds one, and `value.field(arguments)` calls it.
- `Box.create(move (x) => …)` makes a closure that owns its captures and may be returned or stored.

Next: [Iteration](./ch13-02-iteration), where closures meet the `for` loop and the methods of slices and
vectors.
