---
title: Derives and macros
summary: What each of the three derives Equal, HashKey and OrderKey writes for a struct or enum, the other attributes and where each is taught, and why CeeSafe has no macros, with the generics, closures, constants and built-ins that take their place.
assumes: [ch05-01-defining-structs, ch08-03-maps, ch10-02-interfaces]
keywords: ["@derive", Equal, HashKey, OrderKey, compareKey, attribute, "@noncopy", "@align", "@test", "@from", "@export", "@repr", macro, debugAssert, embed, "code generator"]
diagnostics: [N0207, N0210, N0290]
specification: ["type-system.md §28.1", "type-system.md §28.7", "type-system.md §10", "type-system.md §10.1"]
---

# Derives and macros

**This section teaches** the three things `@derive` can write and exactly what each one writes, the other
attributes of the language, and what a CeeSafe program uses where another language would use a macro. It assumes
[Defining structs](./ch05-01-defining-structs) and [Maps](./ch08-03-maps), where `@derive` first appears, and
[Interfaces](./ch10-02-interfaces).

An **attribute** is a name after `@`, written before a declaration: `@derive(Equal)`, `@test`, `@export("name")`.
It tells the compiler something about the item that follows. The set of attributes is fixed by the language. A
program cannot define one, and an unknown name is an error.

## The three derives

`@derive(…)` goes before a struct or an enum, and takes one or more of three names. For each, the compiler writes
an `impl` from the type's fields.

| Derive | What the type gets | It needs |
|---|---|---|
| `Equal` | `a == b` and `a != b` | every field's type to have `==` |
| `HashKey` | it can be a key of a `HashMap` | `Equal` as well, and every field's type to be a `HashKey` |
| `OrderKey` | `a.compareKey(b)`, and it can be a key of an `OrderedMap` | every field's type to be an `OrderKey` |

<span class="listing-caption"><strong>Listing 20-38</strong>, <code>main.cee</code>: A struct with all three derives and an enum with two, compared, ordered and used as a map's key.</span>

```ceesafe
@derive(Equal, HashKey, OrderKey)
struct Version {
    major: u32;
    minor: u32;
}

@derive(Equal, OrderKey)
enum Release {
    Nightly,
    Beta(u32),
    Stable(Version),
}

fn main() {
    let old = Version { major: 1, minor: 9 };
    let new = Version { major: 1, minor: 12 };
    Console.writeLine(old == new);
    Console.writeLine(old != new);
    Console.writeLine(old.compareKey(new));

    Console.writeLine(Release.Beta(2) == Release.Beta(2));
    Console.writeLine(Release.Beta(2).compareKey(Release.Stable(old)));
    Console.writeLine(Release.Stable(new).compareKey(Release.Stable(old)));

    var downloads = HashMap<Version, u64>.create();
    downloads.put(old, 310)!;
    downloads.put(new, 45)!;
    Console.writeLine(downloads.get(Version { major: 1, minor: 12 })!);
}
```

```text
false
true
-1
true
-1
1
45
```

What each derive writes is fixed:

- **`Equal`** compares the fields in the order they are declared, and stops at the first that differ. Two enum
  values are equal when they are the same variant with equal payloads.
- **`OrderKey`** compares the first field; if those are equal, the second; and so on. `compareKey` returns a
  negative number, zero or a positive one. For an enum, a variant declared earlier comes before one declared
  later, so `Beta(2)` is before every `Stable`; two values of one variant compare their payloads.
- **`HashKey`** feeds each field, in order, to the map's hasher, and uses `==` to tell two keys with the same
  hash apart. That is why it needs `Equal`.

So the order of a struct's fields is the order of its values: put the most significant field first. An
`OrderKey` does not give the type `<`. Compare with `a.compareKey(b) < 0`.

A derived `impl` is ordinary code. This is `OrderKey` for a two-field struct, derived and then by hand:

<span class="listing-caption"><strong>Listing 20-39</strong>, <code>main.cee</code>: The <code>impl</code> that <code>@derive(OrderKey)</code> writes, written out for a second struct.</span>

```ceesafe
@derive(OrderKey)
struct Version {
    major: u32;
    minor: u32;
}

struct Byhand {
    major: u32;
    minor: u32;
}

impl OrderKey<Byhand> for Byhand {
    fn compareKey(borrow this, other: borrow Byhand) -> i32 {
        let first = this.major.compareKey(other.major);
        if (first != 0) {
            return first;
        }
        return this.minor.compareKey(other.minor);
    }
}

fn main() {
    Console.writeLine(Version { major: 1, minor: 9 }.compareKey(Version { major: 1, minor: 12 }));
    Console.writeLine(Byhand { major: 1, minor: 9 }.compareKey(Byhand { major: 1, minor: 12 }));
    Console.writeLine(Byhand { major: 2, minor: 0 }.compareKey(Byhand { major: 1, minor: 12 }));
}
```

```text
-1
-1
1
```

Write `OrderKey` or `HashKey` by hand when the derived order is not the one you want, such as an order that
ignores a field. `Equal` is different: `==` on a struct or enum comes only from the derive, so it always means
"every field is equal". An equality that means something else is a method with a name, such as `sameName`.

::: tip Rule
`@derive` takes `Equal`, `HashKey` and `OrderKey`, each once, on a struct or an enum. `HashKey` needs `Equal`.
The `impl` is written from the fields in declaration order and checked like code you wrote.

*Specification: `type-system.md` §28.7.*
:::

There is no derive for copying, printing or default values:

- a struct whose fields are all Copy is Copy by itself, with nothing to write;
- a deep copy is a method: `s.clone()` on a `String` or `Vector`, and a `clone` method of your own for a struct
  that holds them;
- printing is a method that writes the fields, and a default value is a static function such as `create()`.

## The other attributes

| Attribute | On | What it does | Taught in |
|---|---|---|---|
| `@test` | a function | `ceesafec test` runs it | chapter 11 |
| `@from` | an enum variant | `?` converts the payload's error type into the enum | chapter 9 |
| `@export("name")` | a function, a constant | callable from C under that name | chapters 14 and 20 |
| `@repr(C)`, `@repr(u8)` | a struct, an enum | C's layout; one integer in C | chapters 14 and 20 |
| `@callback` | a function | a C address, for C's function pointers | [Calling C](./ch20-02-calling-c) |
| `@header`, `@cname`, `@ctype`, `@cvalue` | `extern "C"` blocks and their items | which header, and C's own names | [Calling C](./ch20-02-calling-c) |
| `@cfg("NAME")` | an `@export` function, a C declaration | exists in C only where the C macro `NAME` is not zero | `type-system.md` §28.4 |
| `@noncopy` | a struct | a move type, though every field is Copy | below |
| `@align(n)` | a struct | its address is a multiple of `n` bytes | below |
| `@effects(…)`, `@context(…)`, `@placed`, `@constant` | functions and methods | rules for kernel code: what may sleep or allocate, values made in place | `type-system.md` §32, §33, §39 |

<span class="listing-caption"><strong>Listing 20-40</strong>, <code>main.cee</code>: A struct of one integer that moves instead of copying, and a struct aligned to 64 bytes.</span>

```ceesafe
@noncopy
struct Ticket {
    number: u32;
}

@align(64)
struct Counter {
    hits: u64;
}

fn redeem(ticket: Ticket) -> u32 => ticket.number;

fn main() {
    let ticket = Ticket { number: 7 };
    Console.writeLine(redeem(ticket));
    let counter = Counter { hits: 3 };
    Console.writeLine((borrow counter as *Counter as usize) % 64);
}
```

```text
7
0
```

`@noncopy` is for a value that stands for something that must be used once: a ticket, a permission, a handle.
After `redeem(ticket)` the ticket is gone, and a second `redeem(ticket)` is the error of a value used after a
move. `@align(64)` puts each `Counter` on its own cache line, so that two threads updating two counters do not
slow each other down.

## No macros

CeeSafe has no macros: no pattern macros, no procedural macros, no preprocessor. The word `macro` is reserved and
means nothing. The reason is that everything the compiler checks, the formatter formats and an editor resolves
is in the file as you read it. A call is a call, and no name hides a second language.

What a macro does elsewhere is done here by a feature that is checked like any other code:

| A macro is used for | In CeeSafe |
|---|---|
| the same code for several types | a generic function or type (chapter 10) |
| the same code with a piece that varies | a closure parameter (chapter 13) |
| code chosen by a compile-time flag | a const parameter; `if (cfg("NAME"))` for a C macro (chapter 14) |
| `==`, hashing and ordering for a type | `@derive` |
| printing with a format string | `Console.write` once for each value; `append…` methods on a `String` |
| joining constant text | `+` between constant `StringView`s in a constant |
| a file's contents as a constant | `embed("path")` and `embedText("path")` (chapter 14) |
| a C string literal | `c"…"` |
| an assertion only in debug builds | `debugAssert(condition, message)` |
| tables and repeated declarations | a program that writes a `.cee` file, run before the build |

<span class="listing-caption"><strong>Listing 20-41</strong>, <code>main.cee</code>: Four of these: joined constants, a closure for the piece that varies, a const parameter, and a debug assertion.</span>

```ceesafe
const HOME: StringView = "/home/ada";
const CONFIG: StringView = HOME + "/.config";

fn largest<T: Copy>(values: Slice<T>, before: borrow fn(T, T) -> bool) -> T {
    var best = values[0];
    for (v in values) {
        if (before(best, v)) {
            best = v;
        }
    }
    return best;
}

fn show<const WIDTH: usize>(name: StringView, value: u64) {
    var line = String.create();
    line.appendPadded(name, WIDTH, TextAlign.Left, ' ')!;
    line.appendNumber(value)!;
    Console.writeLine(line.view());
}

fn main() {
    Console.writeLine(CONFIG);
    let sizes = [40u64, 7, 310];
    debugAssert(sizes.length > 0, "largest needs a value");
    show<8>("largest", largest(sizes[..], (a, b) => a < b));
    show<8>("least", largest(sizes[..], (a, b) => a > b));
}
```

```text
/home/ada/.config
largest 310
least   7
```

- `HOME + "/.config"` joins two constant views when the program is compiled. `+` on views is allowed only in a
  constant: text built while the program runs is appended to a `String`.
- `largest` is written once. The comparison, the piece a macro would paste in, is a closure.
- `debugAssert` is checked in every build and evaluated only when debug assertions are on: under
  `ceesafec build -g`, under `ceesafec test`, and in the interpreter. Because the call may vanish, its condition
  may not change anything. `assert` is always evaluated.

When code really must be generated, as a table of a thousand entries or declarations from a C header are,
generate it in the open: a script writes a `.cee` file whose first line says it is generated, and the file is
checked, formatted and read like any other. `tools/cbind.py` in [Calling C](./ch20-02-calling-c) is one such
generator, and the compiler's own sources use several.

## Mistakes

::: warning Mistake: a derive that does not exist
Only three names are accepted.
:::

<span class="listing-caption"><strong>Listing 20-42</strong>, <code>main.cee</code>: Two derives from another language. This does not compile.</span>

<!-- ceesafe: fail N0290 -->
```ceesafe
@derive(Debug, Clone)
struct Version {
    major: u32;
    minor: u32;
}

fn main() {
    let v = Version { major: 1, minor: 9 };
    Console.writeLine(v.major);
}
```

```text
main.cee:1:9: error[N0290]: '@derive' takes Equal, HashKey and OrderKey; Clone, Default and Debug are written as methods (ADR 0087 §2.3)
  |
1 | @derive(Debug, Clone)
  |         ^^^^^

main.cee:1:16: error[N0290]: '@derive' takes Equal, HashKey and OrderKey; Clone, Default and Debug are written as methods (ADR 0087 §2.3)
  |
1 | @derive(Debug, Clone)
  |                ^^^^^
```

This `Version` is Copy already, so it needs no `Clone`. To print it, write the two fields. The same code,
`N0290`, is reported for `@derive(HashKey)` without `Equal`, for a derive named twice, and for an attribute the
language does not have, such as `@inline`.

::: warning Mistake: deriving `Equal` over a field that has no `==`
The derive compares every field, so every field's type needs `==`.
:::

<span class="listing-caption"><strong>Listing 20-43</strong>, <code>main.cee</code>: <code>Build</code> has no <code>==</code>, and <code>Version</code> has a <code>Build</code>. This does not compile.</span>

<!-- ceesafe: fail N0207 -->
```ceesafe
struct Build {
    number: u32;
}

@derive(Equal)
struct Version {
    major: u32;
    build: Build;
}

fn main() {
    let v = Version { major: 1, build: Build { number: 7 } };
    Console.writeLine(v.major);
}
```

```text
main.cee:5:9: error[N0207]: '@derive(Equal)' compares field 'build', whose type 'Build' has no '==': derive Equal for it too, or compare the type in a method of its own (ADR 0087 §2.3)
  |
5 | @derive(Equal)
  |         ^^^^^
```

Add `@derive(Equal)` to `Build`. The message points at the derive, not at the field, because the comparison is
in the code the derive writes.

::: warning Mistake: calling a macro
`name!(…)` is not a macro call. `!` after an expression unwraps a `Result` or an optional, so the compiler
reads `println!` as a value and `(…)` as a call of it.
:::

<span class="listing-caption"><strong>Listing 20-44</strong>, <code>main.cee</code>: Rust's <code>println!</code>. This does not compile.</span>

<!-- ceesafe: fail N0210 -->
```ceesafe
fn main() {
    println!("total");
}
```

```text
main.cee:2:5: error[N0210]: this expression cannot be called
  |
2 |     println!("total");
  |     ^^^^^^^^
```

The message does not mention macros. Write `Console.writeLine("total");`. For text with values in it, there is
no format string either: call `Console.write` once for each piece, and `Console.writeLine` for the last.

::: details Coming from Rust
- `@derive(Equal)` is `#[derive(PartialEq, Eq)]`, `@derive(Equal, HashKey)` is `Hash`, and `@derive(OrderKey)`
  is `Ord`, without the operators `<` and `>`. There is no `Clone`, `Copy`, `Debug` or `Default` derive, and no
  way to define a derive.
- There is no `macro_rules!` and there are no procedural macros. `println!` and `format!` are `Console.write`
  calls and `String` appends; `vec![…]` is an array literal or pushes; `assert!` is `assert`; `debug_assert!`
  is `debugAssert`; `include_bytes!` and `include_str!` are `embed` and `embedText`; `concat!` is `+` in a
  constant; `cfg!` is `cfg("NAME")`.
- `#[inline]`, `#[must_use]` and `#[allow(…)]` have no counterpart. A `Result` that nothing reads is a warning
  without any attribute.
:::

::: details Coming from C
There is no preprocessor: no `#define`, no `#include`, no `#ifdef`. A constant is `const`, a function-like macro
is a function (generic if it must take several types), a file is part of its package without being included,
and conditional code is `if (cfg("NAME"))`, where both branches are checked in every build.
:::

::: details Coming from C#, Java, TypeScript or Go
Attributes look like C#'s attributes, Java's annotations and TypeScript's decorators, but a program cannot
declare one and nothing reads them while the program runs. There is no reflection. `@derive(Equal, HashKey)`
does what overriding `Equals` and `GetHashCode`, or `equals` and `hashCode`, does. Go's `go generate` is the
nearest thing to the generators described above.
:::

## Summary

- `@derive(Equal)` gives `==` and `!=`; `@derive(Equal, HashKey)` makes a type a `HashMap` key;
  `@derive(OrderKey)` gives `compareKey` and makes it an `OrderedMap` key. They work on structs and enums.
- Each derive goes through the fields in declaration order. A field whose type lacks what the derive needs is
  an error at the derive (`N0207` for `Equal`).
- No other derive exists (`N0290`). Copy is automatic; cloning, printing and defaults are methods.
- Attributes are fixed by the language: `@test`, `@from`, `@export`, `@repr`, `@callback`, `@noncopy`, `@align`
  and those for C declarations and kernel code.
- CeeSafe has no macros. Generics, closures, const parameters, constants joined with `+`, `embed`, `c"…"`,
  `debugAssert` and code generators do their work, and `name!(…)` is an error (`N0210`).

This is the end of the chapter. The appendices list the derives and the operators for reference.
