---
title: Variables
summary: Naming values with let, var and const - a let cannot be assigned, a var can, a const is fixed at compile time. Type inference and annotations, scope, and the rule that a name in scope cannot be declared again.
assumes: [ch01-02-hello-world]
keywords: [let, var, const, immutable, mutable, type annotation, type inference, scope, shadowing]
diagnostics: [N0200, N0203, N0211, N0212]
specification: ["type-system.md §2", "type-system.md §3", "type-system.md §10"]
---

# Variables

**This section teaches** how to give a value a name with `let`, `var` and `const`, how the compiler finds a
variable's type, and where a name can be used. It assumes you can write and run a one-file program
([Hello, world](./ch01-02-hello-world)).

## `let` and `var`

A variable is a name for a value. CeeSafe has two ways to declare one in a function:

- `let` declares a variable that keeps its first value;
- `var` declares a variable that can be assigned again.

<span class="listing-caption"><strong>Listing 3-1</strong>, <code>main.cee</code>: A <code>let</code> keeps its value, and a <code>var</code> changes.</span>

```ceesafe
fn main() {
    let width = 4;
    var count = 0;
    count = count + 1;
    count += width;
    Console.writeLine(width);
    Console.writeLine(count);
}
```

```text
4
5
```

`count = count + 1;` assigns a new value to `count`. `count += width;` is a shorter way to write
`count = count + width;`, and `-=`, `*=`, `/=` and `%=` work the same way.

::: tip Rule
A `let` cannot be assigned after its declaration. A variable that changes is a `var`.

*Specification: `type-system.md` §3.*
:::

Use `let` unless the variable must change. Then a reader who sees `let` knows the value is the same everywhere
below it, and the compiler holds you to that.

## Types: inferred or written

Every variable has one type, fixed when the program is compiled. Most of the time you do not write it: the
compiler takes the type of the first value. To choose the type yourself, write it after a colon.

<span class="listing-caption"><strong>Listing 3-2</strong>, <code>main.cee</code>: An inferred type, a written type, and a <code>var</code> that gets its value later.</span>

```ceesafe
fn main() {
    let name = "Ada";
    let year: u16 = 2026;
    var total: u64;
    total = 40 + 2;
    Console.writeLine(name);
    Console.writeLine(year);
    Console.writeLine(total);
}
```

```text
Ada
2026
42
```

- `name` has the type of `"Ada"`, which is text.
- `year: u16` is a 16-bit unsigned integer, because the annotation says so. [Data types](./ch03-02-data-types)
  lists the types.
- `var total: u64;` has no first value, so the type must be written. The compiler checks that `total` is assigned
  before it is read.

A `let` always needs its value in the declaration. A variable never changes its type: assigning text to a `var`
that holds a number does not compile.

## Constants

A `const` is a value that is known when the program is compiled. It is declared outside any function, and every
function of the package can use it.

<span class="listing-caption"><strong>Listing 3-3</strong>, <code>main.cee</code>: Two constants, used in <code>main</code>.</span>

```ceesafe
const SECONDS_PER_HOUR: u32 = 60 * 60;
const GREETING: StringView = "Hello";

fn main() {
    let hours: u32 = 3;
    Console.writeLine(GREETING);
    Console.writeLine(hours * SECONDS_PER_HOUR);
}
```

```text
Hello
10800
```

A constant differs from a `let` in three ways:

- its type is always written (`StringView` is the type of text);
- its value is computed by the compiler, so it can use literals, operators and other constants, but cannot call
  your functions;
- it is declared at the top level of a file, not inside a function.

The names in capitals are a convention, not a rule.

## Scope

A variable can be used from the end of its declaration to the end of the block it is declared in. A block is the
code between `{` and `}`. That part of the program is the variable's **scope**.

<span class="listing-caption"><strong>Listing 3-4</strong>, <code>main.cee</code>: Each <code>inner</code> exists only in its own block, and <code>outer</code> is visible in both.</span>

```ceesafe
fn main() {
    let outer = 10;
    {
        let inner = 5;
        Console.writeLine(outer + inner);
    }
    {
        let inner = 7;
        Console.writeLine(outer + inner);
    }
    Console.writeLine(outer);
}
```

```text
15
17
10
```

The two blocks do not overlap, so each may have its own `inner`. Inside either block, `outer` is still in scope.

::: tip Rule
A name that is in scope cannot be declared again. This is true in the same block and in a block inside it.
CeeSafe has no shadowing.

*Specification: `type-system.md` §2.*
:::

So one name means one variable for as long as you can see it. When you read a name, there is only one declaration
it can refer to.

## Mistakes

::: warning Mistake: assigning to a `let`
A variable that changes must be declared with `var`.
:::

<span class="listing-caption"><strong>Listing 3-5</strong>, <code>main.cee</code>: <code>count</code> is a <code>let</code>, and line 3 assigns to it. This does not compile.</span>

<!-- ceesafe: fail N0211 -->
```ceesafe
fn main() {
    let count = 0;
    count = count + 1;
    Console.writeLine(count);
}
```

```text
main.cee:3:5: error[N0211]: cannot assign to 'count': 'count' was declared with 'let'; use 'var' for a mutable variable
  |
3 |     count = count + 1;
  |     ^^^^^
```

The message says what to do: write `var count = 0;`.

::: warning Mistake: declaring a name again
In some languages a second `let` with the same name replaces the first. In CeeSafe it is an error.
:::

<span class="listing-caption"><strong>Listing 3-6</strong>, <code>main.cee</code>: <code>text</code> is declared twice. This does not compile.</span>

<!-- ceesafe: fail N0203 -->
```ceesafe
fn main() {
    let text = "42";
    let text = text.length;
    Console.writeLine(text);
}
```

```text
main.cee:3:9: error[N0203]: 'text' is already declared in this function; locals cannot be shadowed
  |
3 |     let text = text.length;
  |         ^^^^
```

Give the second value a name of its own. It is a different value, here even of a different type, so a different
name describes it better:

<span class="listing-caption"><strong>Listing 3-7</strong>, <code>main.cee</code>: Two values, two names.</span>

```ceesafe
fn main() {
    let text = "42";
    let length = text.length;
    Console.writeLine(length);
}
```

```text
2
```

If the two values are the same thing at different times, such as a running total, use one `var` and assign to it.

::: warning Mistake: reading a `var` before it has a value
A `var` declared without a value must be assigned on every path before it is read. The check is called
**definite initialisation**.
:::

<span class="listing-caption"><strong>Listing 3-8</strong>, <code>main.cee</code>: <code>total</code> has no value when it is printed. This does not compile.</span>

<!-- ceesafe: fail N0212 -->
```ceesafe
fn main() {
    var total: i32;
    Console.writeLine(total);
}
```

```text
main.cee:3:5: error[N0212]: 'total' is read here but may not have been assigned a value
  |
3 |     Console.writeLine(total);
  |     ^^^^^^^^^^^^^^^^^^^^^^^^^
```

Give the variable a value in its declaration, `var total = 0;`, or assign to it before the read.

::: warning Mistake: using a variable outside its scope
A variable declared in a block is gone at the block's `}`.
:::

<span class="listing-caption"><strong>Listing 3-9</strong>, <code>main.cee</code>: <code>inner</code> is used after its block ends. This does not compile.</span>

<!-- ceesafe: fail N0200 -->
```ceesafe
fn main() {
    let outer = 10;
    {
        let inner = 5;
        Console.writeLine(outer + inner);
    }
    Console.writeLine(inner);
}
```

```text
main.cee:7:23: error[N0200]: cannot find 'inner' in this scope
  |
7 |     Console.writeLine(inner);
  |                       ^^^^^
```

Declare the variable in the block where all its uses are, here the body of `main`.

::: details Coming from Rust
`let` is Rust's `let`, and `var` is Rust's `let mut`. There is no shadowing: `let x = x + 1;` is `N0203`. A
`const` needs its type, as in Rust, but cannot be declared inside a function.
:::

::: details Coming from JavaScript or TypeScript
`let` is closest to `const`, and `var` to `let`. CeeSafe's `var` is scoped to its block, not to its function, and
a name cannot be declared again in an inner block.
:::

::: details Coming from C, C# or Java
`let` is a local that is `const` or `final`, and that is the default to reach for. As in C# and Java, and unlike
C, an inner block cannot declare a name that the enclosing block already has.
:::

## Summary

- `let` names a value that does not change; `var` names one that can be assigned again (`N0211` if you assign to a
  `let`).
- The type is inferred from the first value, or written after a colon. It never changes.
- A `var` may be declared without a value if its type is written; it must be assigned before it is read (`N0212`).
- `const NAME: Type = value;` at the top level of a file is a value computed at compile time.
- A variable's scope runs from its declaration to the end of its block (`N0200` outside it).
- A name in scope cannot be declared again (`N0203`).

Next: [data types](./ch03-02-data-types), the types a variable can have.
