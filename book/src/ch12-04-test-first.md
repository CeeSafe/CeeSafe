---
title: Adding the search, test first
summary: Writing a failing @test for the search function and then the function that passes it - search borrows the text and returns a Vector of views into it, with a region parameter that ties the result to the text.
assumes: [ch12-03-refactoring, ch11-01-writing-tests, ch04-03-slices-and-views, ch10-03-regions]
keywords: ["@test", test first, search, split, contains, StringView, region, views, Vector, Check]
diagnostics: [N0245, N0241]
specification: ["tooling.md §10"]
---

# Adding the search, test first

**This section teaches** how to add a function by writing its test before its body, and how a function returns
parts of a text it was lent without copying them. It adds the search to the program of
[section 12.3](./ch12-03-refactoring), and assumes [tests](./ch11-01-writing-tests),
[views](./ch04-03-slices-and-views) and [regions](./ch10-03-regions).

The method has four steps:

1. Write a test that says what the function must do. Run it and see it fail, for the reason you expect.
2. Write enough code to make it pass.
3. Tidy the code, with the test to tell you that it still works.
4. Do it again for the next thing.

Seeing the test fail first matters. A test that has never failed may be one that cannot.

## What the function is

The search needs the text to look for and the text to look in, and gives the lines that match. Before the
body, decide the types:

```text
search(query, text)  ->  the lines of text that contain query
```

A line of `text` is a part of `text`. CeeSafe has a type for a part of a text that copies nothing: a
`StringView`, which is a borrow of some bytes of the text. So the result is a `Vector` of views into `text`.

The other choices are worse here. A `Vector<String>` would copy every matching line into memory of its own.
A list of positions would copy nothing, but each position is only a number: the caller would need `text` again
to turn it back into a line, and nothing would stop it using the number with the wrong text. A view is the
position and the text in one value, and the compiler checks that the text is still there whenever the view is
used.

## The test, and a body that fails it

Add this to the end of `src/look.cee`. A test may sit in the same file as the code it tests.

<span class="listing-caption"><strong>Listing 12-14</strong>, <code>src/look.cee</code>: A test for <code>search</code>, and a <code>search</code> that finds nothing yet.</span>

```ceesafe
/// The lines of `text` that contain `query`, in order.
fn search<region r>(query: StringView, text: StringView<r>) -> Result<Vector<StringView<r>>, AllocError> {
    return Ok(Vector<StringView<r>>.create());
}

@test
fn findsOneLine() -> Result<void, TestFailure> {
    let text = "one fish\ntwo fish\nred herring\n";
    let found = search("red", text)!;
    Check.equalU64(1, found.length as u64)?;
    Check.equalText("red herring", found[0])?;
    return Ok();
}
```

The test builds a text of three lines, searches it for `red`, and checks two things: that one line was found,
and that it is the right one. Each `Check` function returns a `Result`, and `?` ends the test at the first
check that fails. `Check.equalU64` compares two `u64` values, so the length, a `usize`, is converted with
`as u64`.

`search` has a body that compiles and is wrong: it returns an empty vector. That is on purpose. The test has to
compile before it can fail.

```sh
ceesafec test
```

```text
test findsOneLine ... failed

---- findsOneLine: failed ----
expected 1, got 0

ceesafec test: 0 passed, 1 failed
```

It fails at the first check: one line was expected and none came back. That is the failure the empty body
should give, so the test tests something.

## The signature

```text
fn search<region r>(query: StringView, text: StringView<r>) -> Result<Vector<StringView<r>>, AllocError>
```

- `<region r>` declares a region, `r`.
- `text: StringView<r>` says the text is borrowed for `r`.
- `Vector<StringView<r>>` says the views in the result are borrows for the same `r`. That is: **the result
  borrows from `text`.**
- `query` has no region name. The result does not borrow from it, and the caller may let the query go as soon
  as `search` returns.
- The result is in a `Result` because pushing onto a vector can fail to allocate.

A function that takes one view and returns a view needs none of this: the result can only come from the one
argument. `search` takes two, so it must say which.

::: tip Rule
When a function with several borrowed parameters returns views, name the region: declare `<region r>`, and
write `StringView<r>` on the parameter the result borrows from and in the result type.
:::

## The body that passes

Replace the body of `search`:

<span class="listing-caption"><strong>Listing 12-15</strong>, <code>src/look.cee</code>: <code>search</code> keeps each line that contains the query.</span>

```ceesafe
/// The lines of `text` that contain `query`, in order.
fn search<region r>(query: StringView, text: StringView<r>) -> Result<Vector<StringView<r>>, AllocError> {
    var found = Vector<StringView<r>>.create();
    for (line in text.split("\n")) {
        if (line.contains(query)) {
            found.push(line)?;
        }
    }
    return Ok(found);
}
```

- `text.split("\n")` gives the pieces of `text` between line ends, one at a time. Each piece is a view into
  `text`, with the region `r`.
- `line.contains(query)` is `true` if the query is somewhere in the line.
- `found.push(line)?` keeps the view. It copies the view, which is small, and not the line.

```sh
ceesafec test
```

```text
test findsOneLine ... ok

ceesafec test: 1 passed, 0 failed
```

## Using it in `run`

`run` printed the whole text. Now it prints what `search` finds:

<span class="listing-caption"><strong>Listing 12-16</strong>, <code>src/look.cee</code>: <code>run</code> searches the text and prints the lines found.</span>

<!-- ceesafe: skip -->
```ceesafe
/// Reads the file, searches it and prints the lines found.
fn run(config: borrow Config) -> Result<void, LookError> {
    let bytes = readAll(config.path)?;
    let text = match (StringView.fromUtf8(bytes.view())) {
        Some(text) => text,
        null => { return Err(LookError.NotUtf8); }
    };
    let found = search(config.query, text)?;
    for (line in found) {
        Console.writeLine(line);
    }
    return Ok();
}
```

`search` returns an `AllocError` and `run` returns a `LookError`. The `?` converts one to the other through the
variant `@from Memory(AllocError)` of section 12.3.

Follow the borrows from the bottom up: each `line` is a view into `text`, `text` is a view of `bytes`, and
`bytes` is the vector that owns the file's contents. The file is in memory once, and nothing is copied out of
it. `bytes` is destroyed when `run` returns, after the last line is printed.

```sh
ceesafec run --interp -- tide harbour.txt
```

```text
Nets dry on the wall, and the tide is low.
```

```sh
ceesafec run --interp -- harbour harbour.txt
```

```text
The harbour wakes before the town.
A boat goes out; the harbour bell rings twice.
```

The search is exact about case: line 5, `The Tide turns at noon.`, is not found by `tide`. Section 12.5 makes
that a choice.

::: info Line ends
`split("\n")` gives one more piece than the file has lines when the text ends with a line end: the empty piece
after it. An empty piece contains no query but the empty one, so it shows only if you look for `""`. A file
with Windows line ends leaves a `\r` at the end of each piece; `line.trimEnd()` removes it.
:::

## Mistakes

::: warning Mistake: not saying where the result comes from
Without a region, the compiler cannot tell whether the views in the result point into `query` or into `text`,
and so cannot check their use.
:::

<span class="listing-caption"><strong>Listing 12-17</strong>, <code>main.cee</code>: Two view parameters, views in the result, and no region. This does not compile.</span>

<!-- ceesafe: fail N0245 -->
```ceesafe
fn search(query: StringView, text: StringView) -> Result<Vector<StringView>, AllocError> {
    var found = Vector<StringView>.create();
    for (line in text.split("\n")) {
        if (line.contains(query)) {
            found.push(line)?;
        }
    }
    return Ok(found);
}

fn main() {
    let found = search("red", "red herring\nblue moon\n")!;
    Console.writeLine(found[0]);
}
```

```text
main.cee:1:51: error[N0245]: this function has several reference parameters: name the ones the result is borrowed from, e.g. 'borrow(a, b) T'
  |
1 | fn search(query: StringView, text: StringView) -> Result<Vector<StringView>, AllocError> {
  |                                                   ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
```

The form the message suggests, `borrow(text) T`, is for a function whose result is itself a borrow. Here the
result is a vector that the caller owns, and the borrows are inside it. For that, use the region parameter of
Listing 12-15.

::: warning Mistake: changing the text while the lines are in use
The lines are borrows of the text. While any of them is still used, the text may not change. This is the rule
of [borrowing](./ch04-02-borrowing#the-rule), reaching through `search` by way of the region.
:::

<span class="listing-caption"><strong>Listing 12-18</strong>, <code>main.cee</code>: The text is cleared between the search and the use of its result. This does not compile.</span>

<!-- ceesafe: fail N0241 -->
```ceesafe
fn search<region r>(query: StringView, text: StringView<r>) -> Result<Vector<StringView<r>>, AllocError> {
    var found = Vector<StringView<r>>.create();
    for (line in text.split("\n")) {
        if (line.contains(query)) {
            found.push(line)?;
        }
    }
    return Ok(found);
}

fn main() {
    var text = String.from("red herring\nblue moon\n")!;
    let found = search("red", text.view())!;
    text.clear();
    Console.writeLine(found[0]);
}
```

```text
main.cee:14:5: error[N0241]: cannot borrow 'text' as mutable while 'text' is borrowed
   |
14 |     text.clear();
   |     ^^^^
   = note: 'text' is borrowed here (13:31)
   = note: the borrow is used later here (15:29)
```

In a language without this check, `found[0]` would read memory that `clear` had emptied. Use the lines first,
then change the text.

::: details Coming from Rust
`fn search<region r>(query: StringView, text: StringView<r>) -> … Vector<StringView<r>>` is
`fn search<'a>(query: &str, contents: &'a str) -> Vec<&'a str>`. There is no `text.lines()`: use
`text.split("\n")`. There is no `#[cfg(test)] mod tests`: a `@test` function is left out of a build by itself.
:::

::: details Coming from Go, C# or Java
In those languages a substring is a new string, or shares memory that the garbage collector keeps alive. A
`StringView` shares the memory with no collector: the compiler proves, from the region in the signature, that
the text outlives every view of it.
:::

## Summary

- Write the test first, with a body that compiles and fails; see the failure; then write the body.
- `ceesafec test` runs every `@test` function and shows what a failed check expected and got.
- `search` returns views into the text it was lent: no copies, and the compiler checks the text outlives them.
- `<region r>` on the function, `StringView<r>` on the parameter and in the result, say which argument the
  result borrows from (`N0245` without it).
- While the lines are in use the text cannot change (`N0241`).

Next: [an option from the environment](./ch12-05-environment), for a search that ignores case.
