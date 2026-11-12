---
title: Imports
summary: How one package names another's public items - the qualified name with the package first, import of one item or of all, the file an import applies to, name clashes, and why the built-in library needs no import.
assumes: [ch07-02-files-and-public]
keywords: [import, qualified name, dependency, name clash, scope, Console, built-in library]
diagnostics: [N0204, N0313, N0315]
specification: ["type-system.md §2"]
---

# Imports

**This section teaches** the two ways to name an item of another package, the rules of `import`, and what the
compiler does when two names are the same. It assumes [Files and `public`](./ch07-02-files-and-public), whose
library `tally` this section uses.

`tally` is a library with a public struct, `Tally` (methods `empty`, `add` and `mean`, and a public field `count`),
and a public function, `pair(first, second)`, that returns a `Tally` of two values. The program here is a second
package, `report`. Its manifest names `tally` as a **dependency**:

```toml
[dependencies]
tally = { path = "../tally" }
```

[Dependencies and the lock file](./ch07-04-dependencies) is about that table. This section is about the source.

## The qualified name

A dependency's public item is named with the package's name, a dot, and the item's name: `tally.pair`,
`tally.Tally`. That is its **qualified name**. It needs no `import`, and it works wherever a name can stand.

<span class="listing-caption"><strong>Listing 7-13</strong>, <code>report/src/output.cee</code>: A qualified name as a parameter's type. The file has no <code>import</code>.</span>

<!-- ceesafe: skip -->
```ceesafe
fn printMean(label: StringView, t: borrow tally.Tally) {
    Console.write(label);
    Console.write(" mean: ");
    Console.writeLine(t.mean());
}
```

`borrow tally.Tally` is a borrow of a `Tally` from the package `tally`. Once the function has the value, its
methods and public fields are used as any struct's are: `t.mean()`.

::: tip Rule
`package.Item` names a public item of a dependency, in a type, an expression or a pattern. No `import` is needed
for it.

*Specification: `type-system.md` §2.*
:::

## `import`

Writing `tally.` every time is clear, and long. An `import` at the top of a file brings a name into that file, so
the file can use it bare.

<span class="listing-caption"><strong>Listing 7-14</strong>, <code>report/src/main.cee</code>: <code>Tally</code> is imported and used bare. <code>pair</code> is not imported, and is qualified.</span>

<!-- ceesafe: skip -->
```ceesafe
import tally.Tally;

fn main() {
    var week = Tally.empty();
    week.add(18);
    week.add(21);
    week.add(24);
    Console.write("days: ");
    Console.writeLine(week.count);
    printMean("week", week);

    let weekend = tally.pair(30, 34);
    printMean("weekend", weekend);
}
```

```text
days: 3
week mean: 21
weekend mean: 32
```

That is what `ceesafec run --interp` prints in `report/`, with the two files of Listings 7-13 and 7-14 in `src/`.

There are two forms:

- `import tally.Tally;` brings in one item.
- `import tally;` brings in every public item of the package.

<span class="listing-caption"><strong>Listing 7-15</strong>, <code>report/src/main.cee</code>: Another <code>main</code> for the same package. With the whole package imported, <code>pair</code> and <code>Tally</code> are both bare.</span>

<!-- ceesafe: skip -->
```ceesafe
import tally;

fn main() {
    let weekend = pair(30, 34);
    var both = Tally.empty();
    both.add(weekend.mean());
    printMean("both", both);
}
```

```text
both mean: 32
```

After `import tally;` the qualified names still work. `tally.pair(30, 34)` is always right.

::: tip Rule
An `import` names a dependency, or one public item of a dependency. It comes before the file's items, and it
applies to its own file only.

*Specification: `type-system.md` §2.*
:::

Each part of the rule is checked:

- An `import` below a function or a struct is `N0109`: imports come first.
- The package must be in the manifest's `[dependencies]`: `import ledger;` without it is `N0310`.
- The item must exist (`N0311`) and be public (`N0312`).
- The import in `main.cee` does nothing for `output.cee`. That is why Listing 7-13 writes `tally.Tally`. The files
  of a package share their items. They do not share their imports.

There is no way to rename what you import, and no `import` between the files of one package: those already see
each other.

## When names clash

Three kinds of name can meet in a file: your package's own items, the names its imports bring, and the names of
the dependencies themselves.

**Your own item wins over an import.** If `report` declares its own `fn pair`, then `pair(30, 34)` calls it, in a
file that says `import tally;` too. The compiler says nothing, because nothing is unclear: the import offers a name
and the package already has one. `tally.pair` still reaches the library's.

**Two imports that offer one name are an error when you use the name.** Importing both packages is fine. Using the
shared name bare is not. The mistake below shows it.

**A dependency's name is taken.** No item of `report` may be called `tally`, or `tally.pair` could mean two things.

::: tip Rule
For a bare name the compiler looks at the package's own items first, then at the file's imports, then at the
built-in library. A qualified name, `tally.pair`, always means the dependency's item.

*Specification: `type-system.md` §2.*
:::

## The built-in library needs nothing

`Console`, `StringView`, `i64`, and the rest of what CeeSafe gives every program, are in scope in every file of
every package. There is no `import` for them and no line in `[dependencies]`. Listing 7-13 calls `Console.write`
in a file with no `import` at all.

So an `import` in a CeeSafe file always tells you one thing: this file uses a package that the manifest lists.

## Mistakes

::: warning Mistake: a bare name with no import
The item is public and the package is a dependency, but this file has not imported the name. The compiler does not
search dependencies for a bare name.
:::

<span class="listing-caption"><strong>Listing 7-16</strong>, <code>report/src/output.cee</code>: Listing 7-13 with <code>Tally</code> in place of <code>tally.Tally</code>. This does not compile, though <code>main.cee</code> imports <code>Tally</code>.</span>

<!-- ceesafe: skip -->
```ceesafe
fn printMean(label: StringView, t: borrow Tally) {
    Console.write(label);
    Console.write(" mean: ");
    Console.writeLine(t.mean());
}
```

```text
src/output.cee:1:43: error[N0204]: unknown type 'Tally'
  |
1 | fn printMean(label: StringView, t: borrow Tally) {
  |                                           ^^^^^
```

Write `tally.Tally`, or add `import tally.Tally;` at the top of this file. In an expression the same mistake is
`N0200`, `cannot find 'Tally' in this scope`.

::: warning Mistake: one name from two imports
Suppose `report` also depends on a package `ledger`, which has its own public `pair`, and one file imports both
packages whole.
:::

<span class="listing-caption"><strong>Listing 7-17</strong>, <code>report/src/main.cee</code>: Both imports offer <code>pair</code>. This does not compile.</span>

<!-- ceesafe: skip -->
```ceesafe
import tally;
import ledger;

fn main() {
    let weekend = pair(30, 34);
    printMean("weekend", weekend);
}
```

```text
src/main.cee:5:19: error[N0313]: 'pair' is imported from both 'tally' and 'ledger': write 'tally.pair' or import only one
  |
5 |     let weekend = pair(30, 34);
  |                   ^^^^
```

The two `import` lines are not the error: the use on line 5 is. Qualify it, as the message says. With
`tally.pair(30, 34)` on that line the program runs, and `ledger.pair` is there for the other one.

::: warning Mistake: an item named as a dependency is
A package cannot declare a function, a struct or any other item with the name of one of its dependencies.
:::

<span class="listing-caption"><strong>Listing 7-18</strong>, <code>report/src/main.cee</code>: A function called <code>tally</code> in a package that depends on <code>tally</code>. This does not compile.</span>

<!-- ceesafe: skip -->
```ceesafe
fn tally(first: i64, second: i64) -> i64 => first + second;

fn main() {
    Console.writeLine(tally(30, 34));
}
```

```text
src/main.cee:1:4: error[N0315]: 'tally' is the name of a dependency and cannot also name an item
  |
1 | fn tally(first: i64, second: i64) -> i64 => first + second;
  |    ^^^^^
```

Rename the function.

::: details Coming from Rust
`import tally.Tally;` is `use tally::Tally;` and `import tally;` is `use tally::*;`. The separator is a dot, not
`::`. There is no `as`, no nested `{…}` list, and no `use` for your own package's items. `tally.pair` works with no
`import`, as `tally::pair` does with no `use`.
:::

::: details Coming from C# or Java
`import tally;` reads like `using Tally;` or `import tally.*;`, but it names a package from the manifest, not a
namespace. An import is for its file, as a `using` is. There are no global usings.
:::

::: details Coming from Go, TypeScript or Python
Go requires the package name on every use, and CeeSafe allows it: `tally.pair`. Unlike Go, an import can make the
name bare. Unlike TypeScript and Python, the thing imported is a package from the manifest, never a file or a
path, and there is no `import … as …`.
:::

## Summary

- `tally.pair` and `tally.Tally` name a dependency's public items anywhere, with no `import`.
- `import tally.Tally;` brings one name into the file; `import tally;` brings all the package's public names.
- Imports come before the file's items and apply to that file only.
- A bare name the file has not imported is not found (`N0204` for a type).
- The package's own items win over imports. A name two imports offer is an error where it is used (`N0313`).
- No item may have a dependency's name (`N0315`).
- The built-in library is in scope everywhere, with no `import`.

Next: [Dependencies and the lock file](./ch07-04-dependencies), on how `tally` got into `report`'s manifest and
what `ceesafec fetch` does with it.
