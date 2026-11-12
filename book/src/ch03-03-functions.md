---
title: Functions
summary: Declaring and calling functions - parameters and their types, return types and return, expression-bodied functions written with =>, the difference between statements and expressions, and functions that never return.
assumes: [ch03-01-variables, ch03-02-data-types]
keywords: [fn, parameter, argument, return, "=>", expression body, statement, expression, never, Process.exit]
diagnostics: [N0100, N0211, N0213, N0225]
specification: ["type-system.md §1", "type-system.md §8", "type-system.md §9"]
---

# Functions

**This section teaches** how to declare a function, pass values to it and get a value back. It assumes
[Variables](./ch03-01-variables) and [Data types](./ch03-02-data-types).

A function is a named piece of code. `main` is one: it is where a program starts. You declare others with `fn` in
the same way and call them by name.

## Parameters and return types

<span class="listing-caption"><strong>Listing 3-23</strong>, <code>main.cee</code>: One function that returns nothing and one that returns an <code>i32</code>.</span>

```ceesafe
fn main() {
    greet("Ada");
    let area = rectangleArea(3, 4);
    Console.writeLine(area);
}

fn greet(name: StringView) {
    Console.write("Hello, ");
    Console.writeLine(name);
}

fn rectangleArea(width: i32, height: i32) -> i32 {
    return width * height;
}
```

```text
Hello, Ada
12
```

- A **parameter** is a name and a type: `name: StringView`, `width: i32`. The type is always written. Parameters
  are separated by commas.
- The **return type** comes after `->`. `greet` has none, so it returns nothing.
- `return` ends the function and gives its value to the caller. In a function that returns nothing, `return;`
  alone leaves early.
- A call gives one **argument** for each parameter, in order: `rectangleArea(3, 4)`.
- Functions can be declared in any order. `main` calls `greet`, which is declared below it.

::: tip Rule
Every parameter has a written type, and each argument must have exactly that type. A function with a return type
must return a value on every path.

*Specification: `type-system.md` §8 and §9.*
:::

The compiler never infers a function's types from its body or its callers. A function's first line says all that
a caller needs to know.

Two more facts. A parameter cannot be assigned in the body: it behaves like a `let`. And `main` may return an
`i32`, which becomes the program's exit status: `fn main() -> i32`.

## Expression bodies

When a function only computes a value, write `=>` and the expression in place of a block.

<span class="listing-caption"><strong>Listing 3-24</strong>, <code>main.cee</code>: Two functions with expression bodies.</span>

```ceesafe
fn square(x: i64) -> i64 => x * x;

fn isEven(n: i32) -> bool => n % 2 == 0;

fn main() {
    Console.writeLine(square(12));
    Console.writeLine(isEven(7));
}
```

```text
144
false
```

`fn square(x: i64) -> i64 => x * x;` means the same as a block with `return x * x;`. The line ends with a
semicolon.

## Statements and expressions

A function body is a list of **statements**. A statement does something and has no value: a `let` or `var`
declaration, an assignment, a `return`, a loop.

An **expression** computes a value: `3`, `width * height`, `a > b`, a call such as `larger(3, 8)`. Expressions
appear inside statements, wherever a value is needed.

<span class="listing-caption"><strong>Listing 3-25</strong>, <code>main.cee</code>: Statements that contain expressions.</span>

```ceesafe
fn larger(a: i32, b: i32) -> i32 {
    let result = if (a > b) a else b;
    return result;
}

fn main() {
    let x = larger(3, 8);
    Console.writeLine(x);
    Console.writeLine(larger(x, 2) + 1);
}
```

```text
8
9
```

In `larger`, the line `let result = if (a > b) a else b;` is a statement. The part after `=` is an expression: an
`if` that chooses between two values, which [Control flow](./ch03-05-control-flow) teaches. In `main`,
`larger(x, 2) + 1` is an expression made of a call, an operator and a literal.

::: tip Rule
The block of a function body has no value. A function gives a value back with `return`, or with an `=>` body.
:::

The difference matters in two places. A block does not end in a bare value, so `{ 5 }` is not a function body that
returns 5. And an expression alone is not a statement unless it is a call: `x + 1;` computes a value that nothing
uses, and the compiler refuses it. Both are in the mistakes below.

## Functions that never return

Some functions never come back to their caller: they end the program, or loop forever. Such a function has the
return type `never`.

<span class="listing-caption"><strong>Listing 3-26</strong>, <code>main.cee</code>: <code>fail</code> prints a message and ends the program with exit status 2.</span>

<!-- ceesafe: exits 2 -->
```ceesafe
fn fail(message: StringView) -> never {
    Console.writeLine(message);
    Process.exit(2);
}

fn half(n: i32) -> i32 {
    if (n % 2 != 0) {
        fail("half: the number is odd");
    }
    return n / 2;
}

fn main() {
    Console.writeLine(half(10));
    Console.writeLine(half(7));
    Console.writeLine("not reached");
}
```

```text
5
half: the number is odd
```

`Process.exit(2)` ends the program at once with exit status 2. Because `fail` returns `never`, the compiler knows
that nothing runs after a call of it. It also checks the promise: if the end of `fail`'s body could be reached,
the function would not compile.

The built-in `panic("message")` is such a function too. It stops the program with a message, for a situation that
should be impossible. [Chapter 9](./ch09-01-panics) teaches when to use it.

## Mistakes

::: warning Mistake: a path that returns nothing
Every way through a function with a return type must reach a `return`.
:::

<span class="listing-caption"><strong>Listing 3-27</strong>, <code>main.cee</code>: When <code>n</code> is 0, <code>sign</code> reaches its end without a value. This does not compile.</span>

<!-- ceesafe: fail N0213 -->
```ceesafe
fn sign(n: i32) -> i32 {
    if (n < 0) {
        return -1;
    }
    if (n > 0) {
        return 1;
    }
}

fn main() {
    Console.writeLine(sign(-4));
}
```

```text
main.cee:1:4: error[N0213]: 'sign' must return a 'i32' on every path; the end of its body is reachable
  |
1 | fn sign(n: i32) -> i32 {
  |    ^^^^
```

Add `return 0;` as the last statement.

::: warning Mistake: ending a body with a bare value
A block is not an expression. The last line of a function body is a statement like the others.
:::

<span class="listing-caption"><strong>Listing 3-28</strong>, <code>main.cee</code>: The body of <code>five</code> has a value where a statement is expected. This does not compile.</span>

<!-- ceesafe: fail N0100 -->
```ceesafe
fn five() -> i32 {
    5
}

fn main() {
    Console.writeLine(five());
}
```

```text
main.cee:2:6: error[N0100]: expected ';', found '}': a block ends in a value only as a branch of an 'if' or 'match' expression (let x = match …)
  |
2 |     5
  |      ^
```

Write `return 5;`, or `fn five() -> i32 => 5;`.

::: warning Mistake: an expression as a statement
Only a call can stand alone as a statement.
:::

<span class="listing-caption"><strong>Listing 3-29</strong>, <code>main.cee</code>: The sum on line 3 is not used. This does not compile.</span>

<!-- ceesafe: fail N0225 -->
```ceesafe
fn main() {
    let x = 3;
    x + 1;
}
```

```text
main.cee:3:5: error[N0225]: only calls can be used as statements
  |
3 |     x + 1;
  |     ^^^^^
```

Give the value a name, `let y = x + 1;`, or pass it to a function.

::: warning Mistake: assigning to a parameter
A parameter keeps the value the caller gave.
:::

<span class="listing-caption"><strong>Listing 3-30</strong>, <code>main.cee</code>: <code>n</code> is a parameter, and line 2 changes it. This does not compile.</span>

<!-- ceesafe: fail N0211 -->
```ceesafe
fn next(n: i32) -> i32 {
    n += 1;
    return n;
}

fn main() {
    Console.writeLine(next(1));
}
```

```text
main.cee:2:5: error[N0211]: cannot assign to 'n': parameters are immutable
  |
2 |     n += 1;
  |     ^
```

Copy it into a `var` with another name, `var result = n;`, and change that. Here `return n + 1;` is enough.

::: details Coming from Rust
A block's last expression is not its value: `fn five() -> i32 { 5 }` does not compile. Use `return`, or `=>` for
a one-expression body. `-> never` is Rust's `-> !`.
:::

::: details Coming from C#, Java, TypeScript or Python
There are no overloads: two functions cannot share a name. There are no default values for parameters and no
named arguments. `=>` is C#'s expression-bodied member, not a lambda.
:::

## Summary

- `fn name(parameter: Type, …) -> ReturnType { … }` declares a function; without `->` it returns nothing.
- Every parameter's type is written, and arguments must match it exactly. Parameters cannot be assigned (`N0211`).
- A function with a return type returns on every path (`N0213`).
- `fn f(x: T) -> U => expression;` is a function whose body is one expression.
- Statements have no value and a block does not end in one (`N0100`); an expression is a statement only if it is a
  call (`N0225`).
- A function that does not come back returns `never`.

Next: [comments](./ch03-04-comments).
