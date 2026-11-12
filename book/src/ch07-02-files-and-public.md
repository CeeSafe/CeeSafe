---
title: Files and public
summary: A package's source is every .cee file under src/, and all its files share one scope whatever directory they are in. The public keyword on items, fields and methods decides what another package can use.
assumes: [ch07-01-package-and-manifest, ch05-01-defining-structs, ch05-03-methods]
keywords: [file, src, directory, scope, public, internal, private, field, method, library, visibility]
diagnostics: [N0201, N0312, N0314]
specification: ["type-system.md §2", "type-system.md §12", "tooling.md §4.1"]
---

# Files and `public`

**This section teaches** how a package's source is divided into files, what those files see of each other, and
what the keyword `public` changes. It assumes [A package and its manifest](./ch07-01-package-and-manifest), and
structs with methods ([Defining structs](./ch05-01-defining-structs), [Methods](./ch05-03-methods)).

An **item** is something declared at the top of a file: a function, a struct, an enum, a constant.

## The files of one package

A package's source is every `.cee` file under `src/`. Here is a program, `scores`, in three files:

```text
scores/
├── ceesafe.toml
└── src/
    ├── main.cee
    ├── scorecard.cee
    └── output/
        └── lines.cee
```

<span class="listing-caption"><strong>Listing 7-4</strong>, <code>scores/src/main.cee</code>: <code>main</code> uses a struct and a function that other files declare.</span>

<!-- ceesafe: skip -->
```ceesafe
fn main() {
    var card = Scorecard.empty();
    card.record(72);
    card.record(85);
    card.record(92);
    printLine("rounds", card.rounds);
    printLine("mean", card.mean());
}
```

```text
rounds: 3
mean: 83
```

<span class="listing-caption"><strong>Listing 7-5</strong>, <code>scores/src/scorecard.cee</code>: The struct, in a file of its own.</span>

```ceesafe
struct Scorecard {
    rounds: i64;
    total: i64;

    fn empty() -> Scorecard => Scorecard { rounds: 0, total: 0 };

    fn record(borrow mut this, score: i64) {
        this.rounds += 1;
        this.total += score;
    }

    fn mean(borrow this) -> i64 => this.total / this.rounds;
}
```

<span class="listing-caption"><strong>Listing 7-6</strong>, <code>scores/src/output/lines.cee</code>: A function, in a file in a directory.</span>

```ceesafe
fn printLine(label: StringView, value: i64) {
    Console.write(label);
    Console.write(": ");
    Console.writeLine(value);
}
```

`ceesafec run --interp` in `scores/` prints the two lines under Listing 7-4. Three things are missing from these
files, and none is needed:

- `main.cee` does not say where `Scorecard` and `printLine` come from. There is no import and no declaration.
- `scorecard.cee` and `lines.cee` do not say that they share anything. Nothing is marked for export.
- Nothing mentions the directory `output`.

::: tip Rule
All the files of a package share one scope. An item declared in any file is visible, by its plain name, in every
file of the package. The order of the files and of the items in a file does not matter.

*Specification: `type-system.md` §2.*
:::

## Directories mean nothing

A file's path inside `src/` has no meaning in the language. `src/output/lines.cee` is one more file of the package:
`printLine` is not `output.printLine`, and moving the file to `src/lines.cee` changes nothing in the program. Use
directories to keep a large package in order for its readers.

A `.cee` file outside `src/`, beside `ceesafe.toml` for example, is not part of the package. The manifest's
`sources` field says which directories are read, and `src` is its default.

Because there is one scope, a name can be declared once in the whole package, not once in each file. The first
mistake below shows what happens otherwise.

## `public`: what another package sees

Inside its package every item is visible. To another package, nothing is, until you write `public`.

Here is a library, `tally`, in two files:

<span class="listing-caption"><strong>Listing 7-7</strong>, <code>tally/src/lib.cee</code>: A public struct with one public field and one that is not, and a public function.</span>

<!-- ceesafe: skip -->
```ceesafe
public struct Tally {
    public count: i64;
    total: i64;

    public fn empty() -> Tally => Tally { count: 0, total: 0 };

    public fn add(borrow mut this, value: i64) {
        this.count += 1;
        this.total += value;
    }

    public fn mean(borrow this) -> i64 => quotient(this.total, this.count);
}

public fn pair(first: i64, second: i64) -> Tally {
    var t = Tally.empty();
    t.add(first);
    t.add(second);
    return t;
}
```

<span class="listing-caption"><strong>Listing 7-8</strong>, <code>tally/src/internal/quotient.cee</code>: A function with no <code>public</code>. Only <code>tally</code> can call it.</span>

```ceesafe
fn quotient(sum: i64, count: i64) -> i64 => if (count == 0) 0 else sum / count;
```

`ceesafec check` in `tally/` reports nothing. A package that depends on `tally` (the next two sections show how)
can use:

- the type `Tally`, because the struct is `public`;
- the field `count`, and the methods `empty`, `add` and `mean`, because each is `public`;
- the function `pair`.

It cannot use the field `total` or the function `quotient`. They have no `public`, so they are **internal**: they
belong to `tally`, which is free to rename or remove them without breaking any other package. The directory name
`internal` is only a name. What keeps `quotient` inside the package is the missing keyword.

::: tip Rule
An item without `public` is internal: only its own package can use it. `public` on an item lets other packages use
it. Each field and each method of a public struct needs its own `public` to be seen from outside.

*Specification: `type-system.md` §2.*
:::

`public` is written once for each thing. A public struct does not make its fields public, and that is useful: with
`total` internal, another package cannot write `Tally { count: 2, total: 40 }`. It has to call `Tally.empty()` and
`add`, so a `Tally` it holds always has a `total` that matches its `count`.

What is public must also make sense from outside. A public function whose signature names an internal type could
not be called by anyone who can see it, so the compiler refuses it:

<span class="listing-caption"><strong>Listing 7-9</strong>, <code>main.cee</code>: A public function returns a struct that is not public. This does not compile.</span>

<!-- ceesafe: fail N0314 -->
```ceesafe
struct Range {
    low: i64;
    high: i64;
}

public fn range() -> Range => Range { low: 0, high: 0 };
```

```text
main.cee:6:11: error[N0314]: 'range' is public, but its signature names 'Range', which is not public: make 'Range' public, or 'range' not (type-system.md §2)
  |
6 | public fn range() -> Range => Range { low: 0, high: 0 };
  |           ^^^^^
```

::: info Two more words
`internal` can be written where `public` goes. It says what leaving the word out says. A field can also be
`private`: then only code in the file that declares the struct can reach it, and other files of the same package
get the error `N0296`.
:::

## Mistakes

::: warning Mistake: the same name in two files
Two files of one package cannot both declare `printLine`, even in different directories. A file is not a scope.
:::

<span class="listing-caption"><strong>Listing 7-10</strong>, <code>scores/src/extra.cee</code>: A second <code>printLine</code>, added beside the files of Listings 7-4 to 7-6. The package does not compile.</span>

<!-- ceesafe: skip -->
```ceesafe
fn printLine(label: StringView, value: i64) {
    Console.writeLine(value);
}
```

```text
src/output/lines.cee:1:4: error[N0201]: 'printLine' is already defined
  |
1 | fn printLine(label: StringView, value: i64) {
  |    ^^^^^^^^^
```

The compiler reads the files in the order of their paths, so `src/extra.cee` comes first and the message points
at the other one. Give one of the two functions another name.

::: warning Mistake: calling another package's internal function
The function exists, and the message says so. It is not yours to call.
:::

<span class="listing-caption"><strong>Listing 7-11</strong>, <code>report/src/main.cee</code>: A package that depends on <code>tally</code> calls <code>quotient</code>. This does not compile.</span>

<!-- ceesafe: skip -->
```ceesafe
fn main() {
    Console.writeLine(tally.quotient(7, 2));
}
```

```text
src/main.cee:2:23: error[N0312]: 'tally.quotient' is not public: only its own package can use it (type-system.md §2)
  |
2 |     Console.writeLine(tally.quotient(7, 2));
  |                       ^^^^^^^^^^^^^^
```

If `tally` is your package and the function is meant to be used, add `public` to it. If it is someone else's, use
what the package offers: here `Tally.mean()`.

::: warning Mistake: reading a field that is not public
A public struct can keep fields to itself. From outside they cannot be read, written, or given in a struct literal.
:::

<span class="listing-caption"><strong>Listing 7-12</strong>, <code>report/src/main.cee</code>: The same package reads <code>total</code>. This does not compile.</span>

<!-- ceesafe: skip -->
```ceesafe
import tally.Tally;

fn main() {
    var week = Tally.empty();
    week.add(18);
    Console.writeLine(week.total);
}
```

```text
src/main.cee:6:28: error[N0312]: field 'total' of 'tally.Tally' is not public: only its own package can reach it (type-system.md §2)
  |
6 |     Console.writeLine(week.total);
  |                            ^^^^^
```

::: details Coming from Rust
There is no `mod`, no `use crate::…` and no `pub(crate)`. A package is one flat scope, which is what a crate would
be if every item were `pub(crate)` and glob-imported everywhere. `public` is `pub`, and it has one meaning: visible
to other packages.
:::

::: details Coming from C# or Java
There are no namespaces and no `using` between files of a package. The default is C#'s `internal`: visible in the
whole package, in every file. The boundary of a `private` field is the file, not the type.
:::

::: details Coming from Go
A directory is not a package: the whole tree under `src/` is one. A capital first letter exports nothing. Write
`public`.
:::

::: details Coming from TypeScript or Python
A file is not a module. Two files of a package do not `import` each other and do not `export` anything: they are
already one scope. Top-level names must therefore be unique in the package.
:::

::: details Coming from C
There are no headers and no forward declarations. A function can be called from any file of the package, above or
below where it is declared.
:::

## Summary

- A package's source is every `.cee` file under `src/` (or the directories `sources` names).
- All files of a package share one scope: no imports, no exports, no order.
- Directories and file names under `src/` mean nothing to the compiler.
- A name is declared once in a package (`N0201`).
- Without `public` an item, a field or a method is internal to its package; using it from another is `N0312`.
- A public item's signature may name only public types (`N0314`).

Next: [Imports](./ch07-03-imports), on how a package names the public items of another.
