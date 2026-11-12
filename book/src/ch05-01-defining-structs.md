---
title: Defining and making structs
summary: Declaring a struct and its fields, making a value with a struct literal, reading and changing fields, when a struct is copied and when it moves, and why two structs do not compare with == until the struct derives Equal.
assumes: [ch03-01-variables, ch03-02-data-types, ch04-01-what-ownership-is]
keywords: [struct, field, struct literal, var, Copy, move, "@derive(Equal)", private]
diagnostics: [N0100, N0207, N0211, N0229, N0250]
specification: ["type-system.md §12", "type-system.md §14"]
---

# Defining and making structs

**This section teaches** how to declare a struct, make a value of it, and read and change its fields; when
assigning a struct copies it and when it moves it; and how to compare two structs. It assumes
[Variables](./ch03-01-variables) and [Data types](./ch03-02-data-types), and for the part on moving,
[What ownership is](./ch04-01-what-ownership-is).

A **struct** is a type that groups named values. Each one is a **field**.

## Declaring a struct and making a value

<span class="listing-caption"><strong>Listing 5-1</strong>, <code>main.cee</code>: A struct with three fields, a value of it, and its fields read.</span>

```ceesafe
struct Player {
    score: u32;
    lives: u8;
    active: bool;
}

fn main() {
    let p = Player { score: 0, lives: 3, active: true };
    Console.writeLine(p.score);
    Console.writeLine(p.lives);
    Console.writeLine(p.active);
}
```

```text
0
3
true
```

- `struct Player { … }` declares the type. It is written at the top level of a file, beside the functions.
- Each field is a name, a colon and a type, and ends with `;`. A field's type can be any type, another struct
  included.
- `Player { score: 0, lives: 3, active: true }` is a **struct literal**. It makes a value. Inside it, each field is
  a name, a colon and a value, and the fields are separated by commas.
- `p.score` reads a field.

The type of `p` is `Player`. You can write it, as for any variable: `let p: Player = …`.

::: tip Rule
A struct literal gives every field a value, exactly once. The fields may be in any order. There are no default
values and no field is left unset.

*Specification: `type-system.md` §12, Literals.*
:::

## Changing a field

To change a field, the variable that holds the struct must be a `var`. The whole struct is mutable or the whole
struct is not: a single field cannot be marked.

<span class="listing-caption"><strong>Listing 5-2</strong>, <code>main.cee</code>: The fields of a <code>var</code> are assigned.</span>

```ceesafe
struct Player {
    score: u32;
    lives: u8;
    active: bool;
}

fn main() {
    var p = Player { score: 0, lives: 3, active: true };
    p.score = 50;
    p.lives -= 1;
    Console.writeLine(p.score);
    Console.writeLine(p.lives);
}
```

```text
50
2
```

## Structs that are copied

`Player` holds two numbers and a boolean. Numbers and booleans are Copy types: assigning one copies it. A struct
made only of Copy fields is Copy too.

<span class="listing-caption"><strong>Listing 5-3</strong>, <code>main.cee</code>: Assigning a Copy struct makes a second, separate value.</span>

```ceesafe
struct Player {
    score: u32;
    lives: u8;
    active: bool;
}

fn main() {
    var first = Player { score: 0, lives: 3, active: true };
    var second = first;
    second.score = 120;
    first.lives -= 1;
    Console.writeLine(first.score);
    Console.writeLine(first.lives);
    Console.writeLine(second.score);
    Console.writeLine(second.lives);
}
```

```text
0
2
120
3
```

After `var second = first;` there are two players. Changing one does not change the other.

## Structs that own their fields

A `String` is text that owns its memory, so it is not Copy: assigning it moves it. A struct with a `String` field
owns that string, and the struct moves as well.

The next listing makes a `String` in two lines. `String.create()` makes an empty one, and `append` adds text to it;
the `!` after the call stops the program if memory runs out. [Strings](./ch08-02-strings) covers `String`, and
chapter 9 covers errors.

<span class="listing-caption"><strong>Listing 5-4</strong>, <code>main.cee</code>: A struct that owns a <code>String</code> moves when it is assigned. This does not compile.</span>

<!-- ceesafe: fail N0250 -->
```ceesafe
struct Player {
    name: String;
    score: u32;
}

fn main() {
    var name = String.create();
    name.append("Ada")!;
    let first = Player { name: name, score: 0 };
    let second = first;
    Console.writeLine(second.name.view());
    Console.writeLine(first.score);
}
```

```text
main.cee:12:5: error[N0250]: 'first.score' is used here after it was moved (on at least one path)
   |
12 |     Console.writeLine(first.score);
   |     ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
```

`let second = first;` moves the whole player into `second`. `first` is then empty, and that includes `first.score`,
though `score` is only a number: a struct moves as one value. Two lines earlier the same thing happens to `name`.
The literal `Player { name: name, score: 0 }` moves the string into the struct, and the variable `name` is empty
after it.

Without the last line the program compiles and prints `Ada`. To read a struct without moving it, borrow it, as
[An example program](./ch05-02-example-structs) does.

::: tip Rule
A struct is Copy when every field is Copy. Otherwise assigning it, passing it or returning it moves it, and the old
variable cannot be used again.

*Specification: `type-system.md` §14.*
:::

## Comparing two structs

A struct has no `==` until you ask for one.

<span class="listing-caption"><strong>Listing 5-5</strong>, <code>main.cee</code>: Two structs compared with <code>==</code>. This does not compile.</span>

<!-- ceesafe: fail N0207 -->
```ceesafe
struct Player {
    score: u32;
    lives: u8;
}

fn main() {
    let a = Player { score: 10, lives: 3 };
    let b = Player { score: 10, lives: 3 };
    Console.writeLine(a == b);
}
```

```text
main.cee:9:23: error[N0207]: operator '==' cannot be applied to type 'Player'
  |
9 |     Console.writeLine(a == b);
  |                       ^^^^^^
```

There are two things to do instead. The first is to compare the fields that matter:
`a.score == b.score && a.lives == b.lives`. The second is to write `@derive(Equal)` on the line before the struct.
It tells the compiler to write the comparison: two values are equal when all their fields are equal.

<span class="listing-caption"><strong>Listing 5-6</strong>, <code>main.cee</code>: With <code>@derive(Equal)</code>, the struct has <code>==</code> and <code>!=</code>.</span>

```ceesafe
@derive(Equal)
struct Player {
    score: u32;
    lives: u8;
}

fn main() {
    let a = Player { score: 10, lives: 3 };
    let b = Player { score: 10, lives: 3 };
    Console.writeLine(a == b);
    Console.writeLine(a != b);
}
```

```text
true
false
```

::: tip Rule
Structs compare with `==` and `!=` only when the struct is declared with `@derive(Equal)`. The other operators,
such as `+` and `<`, do not apply to structs.
:::

## Who can see a field

A field can be read and assigned by any code in the same package. That is enough for a program in one file, and for
this chapter. Two words change it. `private` before a field (`private score: u32;`) limits the field to the file
that declares the struct. `public` makes a struct or a field visible to other packages;
[Files and `public`](./ch07-02-files-and-public) covers it.

## Mistakes

::: warning Mistake: leaving a field out of a literal
Every field needs a value. The message names the one that is missing.
:::

<span class="listing-caption"><strong>Listing 5-7</strong>, <code>main.cee</code>: The literal has no <code>active</code>. This does not compile.</span>

<!-- ceesafe: fail N0229 -->
```ceesafe
struct Player {
    score: u32;
    lives: u8;
    active: bool;
}

fn main() {
    let p = Player { score: 0, lives: 3 };
    Console.writeLine(p.lives);
}
```

```text
main.cee:8:13: error[N0229]: missing field 'active' in 'Player' literal
  |
8 |     let p = Player { score: 0, lives: 3 };
  |             ^^^^^^
```

Add `active: true` or `active: false` to the literal.

::: warning Mistake: assigning a field of a `let`
A `let` cannot change, and neither can its fields.
:::

<span class="listing-caption"><strong>Listing 5-8</strong>, <code>main.cee</code>: <code>p</code> is a <code>let</code> and one of its fields is assigned. This does not compile.</span>

<!-- ceesafe: fail N0211 -->
```ceesafe
struct Player {
    score: u32;
    lives: u8;
    active: bool;
}

fn main() {
    let p = Player { score: 0, lives: 3, active: true };
    p.score = 50;
    Console.writeLine(p.score);
}
```

```text
main.cee:9:5: error[N0211]: cannot assign to 'p.score': 'p' was declared with 'let'; use 'var' for a mutable variable
  |
9 |     p.score = 50;
  |     ^^^^^^^
```

Declare it with `var p`, as Listing 5-2 does.

::: warning Mistake: a field written without its value
When a variable has the same name as a field, the literal still says both: `score: score`. There is no short form.
:::

<span class="listing-caption"><strong>Listing 5-9</strong>, <code>main.cee</code>: The literal names the fields and gives no values. This does not compile.</span>

<!-- ceesafe: fail N0100 -->
```ceesafe
struct Player {
    score: u32;
    lives: u8;
}

fn main() {
    let score: u32 = 10;
    let lives: u8 = 3;
    let p = Player { score, lives };
    Console.writeLine(p.score);
}
```

```text
main.cee:9:27: error[N0100]: expected ':', found ','
  |
9 |     let p = Player { score, lives };
  |                           ^
```

Write `Player { score: score, lives: lives }`.

::: details Coming from Rust
A struct is declared and made much as in Rust, with these differences:

- fields end with `;` in the declaration, not `,`;
- there is no field shorthand (`Player { score }`): write `score: score`;
- Copy is not asked for with a derive: a struct whose fields are all Copy is Copy;
- `==` comes from `@derive(Equal)`, written where Rust writes `#[derive(PartialEq)]`.
:::

::: details Coming from C#, Java, TypeScript or Python
A struct value is not a reference to an object. `var second = first;` never makes two names for one object: it
copies the value, or moves it and leaves `first` empty. There is no `new`: a struct literal makes the value.
:::

::: details Coming from C
The declaration reads like C's with the name before the type. A value is always made whole: there is no declared
but uninitialised struct, and no literal that leaves fields at zero.
:::

## Summary

- `struct Name { field: Type; … }` declares a struct; `Name { field: value, … }` makes a value and names every
  field (`N0229` if one is missing).
- `p.field` reads a field. Assigning one needs a `var` (`N0211`).
- A struct whose fields are all Copy is Copy. A struct that owns something, such as a `String`, moves as one value
  (`N0250` if it is used after the move).
- Structs have no `==` (`N0207`) unless declared with `@derive(Equal)`; or compare the fields.
- Fields are visible throughout the package; `private` limits one to its file.

Next: [an example program](./ch05-02-example-structs) that grows from loose variables into a struct.
