---
title: Comments
summary: The three kinds of comment - a line comment from // to the end of the line, a block comment between /* and */ that does not nest, and a documentation comment written with /// before a declaration.
assumes: [ch03-03-functions]
keywords: [comment, "//", "/* */", "///", documentation comment, "ceesafec doc"]
diagnostics: [N0100]
specification: ["grammar.md §1.2"]
---

# Comments

**This section teaches** how to write notes in a program for the people who read it. It assumes
[Functions](./ch03-03-functions).

The compiler ignores a comment. CeeSafe has three kinds:

- `//` starts a **line comment**, which runs to the end of the line.
- `/*` starts a **block comment**, which runs to the next `*/` and can cover several lines or part of one.
- `///` starts a **documentation comment**, which describes the declaration that follows it.

<span class="listing-caption"><strong>Listing 3-31</strong>, <code>main.cee</code>: The three kinds of comment.</span>

```ceesafe
/// Returns the larger of two numbers.
///
/// When they are equal, the result is `a`.
public fn larger(a: i32, b: i32) -> i32 {
    // A comment on a line of its own.
    if (a >= b) {
        return a; // A comment after code.
    }
    /* A block comment
       can cover several lines. */
    return b;
}

fn main() {
    Console.writeLine(larger(3, 8));
}
```

```text
8
```

Most comments in CeeSafe code are line comments. To comment several lines, start each with `//`.

## Documentation comments

A documentation comment is one or more lines that start with exactly `///`, directly before a declaration such as
a function. It says what the declaration is for, to someone who will use it without reading its body. Its text is
Markdown.

The command `ceesafec doc` collects these comments into a page that lists the public items of a package. `public`
before `fn` makes a function visible to other packages; a later chapter covers packages and visibility. For
Listing 3-31, `ceesafec doc main.cee` prints a Markdown page with a heading `## fn larger`, then the function's
first line, `public fn larger(a: i32, b: i32) -> i32`, then the two sentences of the comment. A function without
`public`, such as `main`, is not on the page.

::: tip Rule
`///` documents the declaration that follows it. Anywhere else, and with any other number of slashes, the line is
an ordinary comment.

*Specification: `grammar.md` §1.2.*
:::

A good documentation comment says what a function returns and what it needs from its arguments. A good line
comment says why the code does something, when the code cannot say it.

## Mistakes

::: warning Mistake: a block comment inside a block comment
Block comments do not nest. The first `*/` ends the comment, wherever it is.
:::

<span class="listing-caption"><strong>Listing 3-32</strong>, <code>main.cee</code>: The comment ends after <code>inner</code>, and the rest of the line is read as code. This does not compile.</span>

<!-- ceesafe: fail N0100 -->
```ceesafe
fn main() {
    /* outer /* inner */ still outer? */
    Console.writeLine("done");
}
```

```text
main.cee:2:31: error[N0100]: expected ';', found identifier
  |
2 |     /* outer /* inner */ still outer? */
  |                               ^
```

To switch off code that already contains a block comment, put `//` before each of its lines.

## Summary

- `//` comments to the end of the line.
- `/* … */` comments a span and does not nest (`N0100`, or another error, from what is left over).
- `///` before a declaration is its documentation, in Markdown; `ceesafec doc` collects it for public items.

Next: [control flow](./ch03-05-control-flow).
