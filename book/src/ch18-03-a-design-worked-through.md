---
title: A design worked through
summary: One small program, a page of blocks rendered as text, written three ways - with an enum, with a generic struct, and with boxed dyn values - showing what each way allows, what stops it, and how the tests follow the design.
assumes: [ch18-02-when-to-use-it, ch11-01-writing-tests, ch08-02-strings]
keywords: [dyn, enum, generic, interface, design, "Vector<Box<dyn I>>", plug-in, open set, tests]
specification: ["type-system.md §37"]
---

# A design worked through

**This section teaches** how the choice between an enum, a generic and `dyn` plays out in one program, by writing
it three times. It assumes [When to use it](./ch18-02-when-to-use-it), and that you can write a test
([Writing tests](./ch11-01-writing-tests)) and append to a `String` ([Strings](./ch08-02-strings)).

The program holds a **page**: a list of blocks, each a heading, a paragraph or a rule. It does two things with a
page: it renders it as plain text, a line for each block, and it counts the words. Each version is a package
under `book/projects/ch18/`, with its tests in the source files.

## First: an enum

There are three kinds of block and the program knows them all. That is the case for an enum.

<span class="listing-caption"><strong>Listing 18-16</strong>, <code>page-enum/src/page.cee</code>: The kinds of block are variants, and each operation is one <code>match</code>.</span>

<!-- ceesafe: skip -->
```ceesafe
/// One block of a page. Every kind of block there is, is listed here.
enum Block {
    Heading(String),
    Paragraph(String),
    Rule,
}

impl Block {
    /// Appends the block to `out` as a line of plain text.
    fn render(borrow this, out: borrow mut String) -> Result<void, AllocError> {
        match (this) {
            Block.Heading(text) => {
                out.append("# ")?;
                out.append(text.view())?;
            }
            Block.Paragraph(text) => { out.append(text.view())?; }
            Block.Rule => { out.append("----")?; }
        }
        return out.append("\n");
    }

    /// The number of words in the block.
    fn words(borrow this) -> usize => match (this) {
        Block.Heading(text) => countWords(text.view()),
        Block.Paragraph(text) => countWords(text.view()),
        Block.Rule => 0,
    };
}
```

A page is a `Vector<Block>`. The rest of the file has `renderPage` and `wordsOnPage`, which loop over the vector,
a `sample` page, and two tests. `ceesafec run --interp` in the package prints:

```text
# Tides
The tide comes in twice a day.
----
It goes out twice too.
13 words
```

This is the shortest version and nothing in it allocates for a block. Everything about a kind of block is spread
over the `match`es, though, and all of them are in this file. Then the program grows: another part of it wants a
quotation block, and later there may be tables and lists, each written by whoever needs it. With the enum, each
of them edits `Block` and every `match`. The set of kinds has to open.

## Second: an interface and a generic page

An interface opens the set. Each kind becomes a struct with an `impl Block`, and a new kind is a new struct in any
file. The page must then say what it holds. The first thing to try is a type parameter.

<span class="listing-caption"><strong>Listing 18-17</strong>, <code>page-generic/src/page.cee</code>: A page generic in the type of its title and the type of its body blocks.</span>

<!-- ceesafe: skip -->
```ceesafe
/// A page of one shape: a title, then a body whose blocks are all of one type.
struct Page<T: Block, B: Block> {
    title: T;
    body: Vector<B>;
}

/// The whole page as text, a line for each block.
fn renderPage<T: Block, B: Block>(page: borrow Page<T, B>) -> Result<String, AllocError> {
    var out = String.create();
    page.title.render(mut out)?;
    for (block in page.body) {
        block.render(mut out)?;
    }
    return Ok(out);
}
```

<span class="listing-caption"><strong>Listing 18-18</strong>, <code>page-generic/src/page.cee</code>: The sample page is a <code>Page&lt;Heading, Paragraph&gt;</code>.</span>

<!-- ceesafe: skip -->
```ceesafe
/// A page to show and to test with.
fn sample() -> Result<Page<Heading, Paragraph>, AllocError> {
    var body = Vector<Paragraph>.create();
    body.push(Paragraph { text: String.from("The tide comes in twice a day.")? })?;
    body.push(Paragraph { text: String.from("It goes out twice too.")? })?;
    return Ok(Page { title: Heading { text: String.from("Tides")? }, body: body });
}
```

It compiles, its three tests pass, and every call of `render` is direct. But look at what `sample` lost: the rule
between the two paragraphs. `body` is a `Vector<Paragraph>`, and a `Rule` is not a `Paragraph`
([Listing 18-15](./ch18-02-when-to-use-it#mistakes) is this error). The shape of a page is now part of its type.
That suits a program whose pages all have one shape, such as a report with a title and rows. It does not suit a
page that is any blocks in any order.

## Third: a page of `dyn Block`

The interface and the three structs stay as they are. Only the page changes: it holds boxes of `dyn Block`.

<span class="listing-caption"><strong>Listing 18-19</strong>, <code>page-dyn/src/page.cee</code>: A page is a vector of boxed blocks of any types.</span>

<!-- ceesafe: skip -->
```ceesafe
/// A page: any number of blocks, of any types that implement `Block`, in order.
struct Page {
    blocks: Vector<Box<dyn Block>>;

    fn create() -> Page => Page { blocks: Vector<Box<dyn Block>>.create() };

    /// Adds a block to the end of the page.
    fn add(borrow mut this, block: Box<dyn Block>) -> Result<void, AllocError> => this.blocks.push(block);

    /// The whole page as text, a line for each block.
    fn render(borrow this) -> Result<String, AllocError> {
        var out = String.create();
        for (block in this.blocks) {
            block.render(mut out)?;
        }
        return Ok(out);
    }

    /// The number of words on the page.
    fn words(borrow this) -> usize {
        var total: usize = 0;
        for (block in this.blocks) {
            total += block.words();
        }
        return total;
    }
}
```

`Page` has no type parameters, so nothing that uses a page is generic either. `add` takes a `Box<dyn Block>`, and
a caller writes `page.add(Box.create(Rule {})?)?`: the `Box<Rule>` converts at the call.

Now the quotation. It is a file of its own, and no line of `blocks.cee` or `page.cee` changes.

<span class="listing-caption"><strong>Listing 18-20</strong>, <code>page-dyn/src/quote.cee</code>: A new kind of block is a struct and an <code>impl</code>.</span>

<!-- ceesafe: skip -->
```ceesafe
/// A kind of block added later. Nothing in `blocks.cee` or `page.cee` names it.
struct Quote {
    text: String;
    by: String;
}

impl Block for Quote {
    fn render(borrow this, out: borrow mut String) -> Result<void, AllocError> {
        out.append("> ")?;
        out.append(this.text.view())?;
        out.append(" (")?;
        out.append(this.by.view())?;
        return out.append(")\n");
    }

    fn words(borrow this) -> usize => countWords(this.text.view());
}
```

<span class="listing-caption"><strong>Listing 18-21</strong>, <code>page-dyn/src/main.cee</code>: The sample page, with a quotation added to it.</span>

<!-- ceesafe: skip -->
```ceesafe
fn main() {
    var page = sample()!;
    page.add(Box.create(Quote { text: String.from("Time and tide wait for no man.")!, by: String.from("proverb")! })!)!;
    let text = page.render()!;
    Console.write(text.view());
    Console.write(page.words());
    Console.writeLine(" words");
}
```

```text
# Tides
The tide comes in twice a day.
----
It goes out twice too.
> Time and tide wait for no man. (proverb)
20 words
```

The generic code of the second version is not wasted. `dyn Block` implements `Block`, so a function with a bound
serves a block whose type is known and one whose type is not.

<span class="listing-caption"><strong>Listing 18-22</strong>, <code>page-dyn/src/blocks.cee</code>: One generic function, tested with a <code>Heading</code> and with a <code>borrow dyn Block</code>.</span>

<!-- ceesafe: skip -->
```ceesafe
/// One block as text, for any type of block, known or not.
fn renderOne<B: Block>(block: borrow B) -> Result<String, AllocError> {
    var out = String.create();
    block.render(mut out)?;
    return Ok(out);
}

@test
fn rendersAKnownBlock() -> Result<void, TestFailure> {
    let heading = Heading { text: String.from("Tides")! };
    return Check.equalText("# Tides\n", renderOne(heading)!.view());
}

@test
fn rendersAnUnknownBlock() -> Result<void, TestFailure> {
    let rule = Rule {};
    let block: borrow dyn Block = borrow rule;
    return Check.equalText("----\n", renderOne(block)!.view());
}
```

The tests sit with the code they test: the page's in `page.cee`, the quotation's in `quote.cee`. `ceesafec test`
in `page-dyn` prints:

```text
test aQuoteOnAPage ... ok
test countsWords ... ok
test rendersAKnownBlock ... ok
test rendersAnUnknownBlock ... ok
test rendersEachKind ... ok

ceesafec test: 5 passed, 0 failed
```

## What each step bought and cost

| | Enum | Generic page | `dyn` page |
|---|---|---|---|
| Any blocks in any order | yes | no | yes |
| A new kind without editing the page's file | no | yes | yes |
| A block is allocated | no | no | yes, one box each |
| A call of `render` | direct, then a `match` | direct | through the table |
| Code can ask "is this a heading?" | yes, with `match` | yes, the type is known | only through a method of `Block` |

The last row is what the third version gave up. In the first, a function that finds the page's first heading is
a `match` away. In the third, a block is only what `Block` says, so the question must become a method of the
interface, perhaps with a default body, and every such question makes the interface larger. If a program asks
many questions of that sort about a few kinds, it wants the enum back.

For this program the third version is the right one: rendering a line of text costs far more than a call through
a table, and the kinds are meant to grow.

## Mistakes

::: warning Mistake: starting with `dyn`
The first version is about half the code of the last and was correct for the program as it stood. Open the set
when a second place needs to add a kind, not before. Moving from the enum to the interface is mechanical: each
arm of each `match` becomes a method of one `impl`.
:::

::: warning Mistake: making everything that touches the page generic
The second version pushes `T` and `B` into `renderPage`, `wordsOnPage`, `sample` and every function that will
ever take a page. When a type parameter spreads like that and stands for "whatever is in there", the value wanted
is a `dyn`.
:::

::: details Coming from C#, Java, TypeScript or Go
The third version is the design you would write first in those languages: an interface and a list of objects. In
CeeSafe it is the version you arrive at, because the first two are cheaper and the compiler checks more of them.
:::

## Summary

- An enum fits a set of kinds that one file can list. It keeps values in place and lets code `match` on them.
- A generic struct fits values whose types are fixed by the shape of the data, such as a title of one type and a
  body of another.
- `Vector<Box<dyn I>>` fits any kinds in any order, with kinds added in files that the holder never names.
- Generic functions with a bound keep working when the design moves to `dyn`.
- With `dyn`, what code may ask of a value is exactly the interface.

Next: chapter 19, Patterns.
