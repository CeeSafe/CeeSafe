---
title: The project improved
summary: The lookfor program of chapter 12 improved in two steps - one search function that takes its test as a closure instead of two copies, then an iterator that gives the matching lines one at a time instead of a vector, with the temporary-closure mistake N0243.
assumes: [ch12-05-environment, ch12-06-standard-error, ch13-01-closures, ch13-02-iteration]
keywords: [lookfor, refactoring, closure parameter, iterator, TextSplit, next, temporary, lazy, test]
diagnostics: [N0243]
specification: ["type-system.md §36.2", "type-system.md §16.4"]
---

# The project improved

**This section teaches** how to use a closure to remove a duplicated function, and an iterator to stop
building a collection that is only read once. It changes `lookfor`, the program finished in
[Errors to standard error](./ch12-06-standard-error), and assumes [Closures](./ch13-01-closures) and
[Iteration](./ch13-02-iteration).

`lookfor <text> <file>` prints the lines of a file that contain a text. With the environment variable
`LOOKFOR_IGNORE_CASE` set, a letter matches in either case. The packages of this section are in
`book/projects/ch13/`: copy chapter 12's `lookfor` to start, and change only `src/look.cee`. `src/main.cee`
stays as it is.

## What is wrong

[Working with the environment](./ch12-05-environment) added the second way of searching as a second function:

<span class="listing-caption"><strong>Listing 13-18</strong>, <code>src/look.cee</code>: Chapter 12's <code>searchIgnoreCase</code>. Its <code>search</code> is the same function with <code>line.contains(query)</code> in the <code>if</code>.</span>

<!-- ceesafe: skip -->
```ceesafe
/// As `search`, but a letter matches in either case.
fn searchIgnoreCase<region r>(query: StringView, text: StringView<r>) -> Result<Vector<StringView<r>>, AllocError> {
    var found = Vector<StringView<r>>.create();
    for (line in text.split("\n")) {
        if (containsIgnoreCase(line, query)) {
            found.push(line)?;
        }
    }
    return Ok(found);
}
```

Two things can be better:

- The two functions differ in one expression. A third way of matching (whole words, say) would be a third
  copy of the loop.
- Both build a vector of every matching line, and `run` reads that vector once to print it. The vector costs
  memory, and its `push` is the only reason searching can fail with `AllocError`.

## Step 1: the test becomes a closure

The part that differs is a decision about one line: does it match? Make it a parameter. `search` takes a
closure from a line to a `bool` and no longer knows what a query is.

<span class="listing-caption"><strong>Listing 13-19</strong>, <code>src/look.cee</code>: One <code>search</code>, which asks its caller's closure about each line.</span>

<!-- ceesafe: skip -->
```ceesafe
/// The lines of `text` that `matches` accepts, in order.
fn search<region r>(
    text: StringView<r>,
    matches: borrow fn(StringView) -> bool,
) -> Result<Vector<StringView<r>>, AllocError> {
    var found = Vector<StringView<r>>.create();
    for (line in text.split("\n")) {
        if (matches(line)) {
            found.push(line)?;
        }
    }
    return Ok(found);
}
```

`searchIgnoreCase` is deleted. `run` chooses the test:

<span class="listing-caption"><strong>Listing 13-20</strong>, <code>src/look.cee</code>: <code>run</code> passes one of two closures. Each captures <code>config</code> to read the query.</span>

<!-- ceesafe: skip -->
```ceesafe
/// Reads the file, searches it and prints the lines found. The result is how many lines it printed.
fn run(config: borrow Config) -> Result<usize, LookError> {
    let bytes = readAll(config.path)?;
    let text = match (StringView.fromUtf8(bytes.view())) {
        Some(text) => text,
        null => { return Err(LookError.NotUtf8); }
    };
    let found = if (config.ignoreCase) {
        search(text, (line) => containsIgnoreCase(line, config.query))?
    } else {
        search(text, (line) => line.contains(config.query))?
    };
    for (line in found) {
        Console.writeLine(line);
    }
    return Ok(found.length);
}
```

The closures only read `config`, so each is a `borrow fn`, which is what `search` asks for. The tests change
in one line each. They no longer need a configuration or a query argument: the closure says what to look for.

<span class="listing-caption"><strong>Listing 13-21</strong>, <code>src/look.cee</code>: The first test, with a closure for the test of a line.</span>

<!-- ceesafe: skip -->
```ceesafe
@test
fn findsOneLine() -> Result<void, TestFailure> {
    let text = "one fish\ntwo fish\nred herring\n";
    let found = search(text, (line) => line.contains("red"))!;
    Check.equalU64(1, found.length as u64)?;
    Check.equalText("red herring", found[0])?;
    return Ok();
}
```

```sh
ceesafec test
```

```text
test caseIgnored ... ok
test caseMatters ... ok
test findsOneLine ... ok

ceesafec test: 3 passed, 0 failed
```

This is the package `book/projects/ch13/stage1`.

## Step 2: the lines come from an iterator

`text.split("\n")` is an iterator: it finds each line when the loop asks for it. `search` then copies the
matching ones into a vector. Instead, `search` can return an iterator of its own that gives the next
matching line on demand. It needs two fields: the `TextSplit` it reads lines from, and the closure.

<span class="listing-caption"><strong>Listing 13-22</strong>, <code>src/look.cee</code>: <code>Matches</code> is an iterator over the matching lines, and <code>search</code> makes one.</span>

<!-- ceesafe: skip -->
```ceesafe
/// The lines of a text that a test accepts, one at a time.
struct Matches<region r> {
    lines: TextSplit<r>;
    matches: borrow(r) fn(StringView) -> bool;
}

impl<region r> Matches<r> {
    /// The next line that the test accepts, or null when the text has no more.
    fn next(borrow mut this) -> StringView<r>? {
        loop {
            match (this.lines.next()) {
                Some(line) => {
                    if (this.matches(line)) {
                        return line;
                    }
                }
                null => { return null; }
            }
        }
    }
}

/// The lines of `text` that `matches` accepts, in order.
fn search<region r>(text: StringView<r>, matches: borrow(r) fn(StringView) -> bool) -> Matches<r> {
    return Matches { lines: text.split("\n"), matches: matches };
}
```

- `Matches` holds a view of the text (inside `TextSplit<r>`) and a closure, both borrows, so it has a region
  parameter `r` and both fields name it.
- `next` has the shape a `for` loop needs. It calls `this.lines.next()` until a line passes
  `this.matches(line)` or the lines run out.
- `search` does no searching now. It returns at once, cannot fail, and has no `Result`.

## The closure must live as long as the iterator

In step 1, `search` finished its work before its statement ended. Now it returns a value that still holds
the closure. Keeping step 1's shape of `run` no longer compiles (the package
`book/projects/ch13/stage2-temporary`):

<span class="listing-caption"><strong>Listing 13-23</strong>, <code>src/look.cee</code>: Each closure is made inside a branch, and <code>found</code> is used after it. This does not compile.</span>

<!-- ceesafe: skip -->
```ceesafe
    let found = if (config.ignoreCase) {
        search(text, (line) => containsIgnoreCase(line, config.query))
    } else {
        search(text, (line) => line.contains(config.query))
    };
    var count: usize = 0;
    for (line in found) {
        Console.writeLine(line);
        count += 1;
    }
    return Ok(count);
```

```text
src/look.cee:96:6: error[N0243]: a temporary value is dropped at the end of this statement while still borrowed; bind it to a local first
   |
96 |     } else {
   |      ^
   = note: a temporary value is borrowed here (95:22)
   = note: the borrow is used later here (94:5)

src/look.cee:98:6: error[N0243]: a temporary value is dropped at the end of this statement while still borrowed; bind it to a local first
   |
98 |     };
   |      ^
   = note: a temporary value is borrowed here (97:22)
   = note: the borrow is used later here (94:5)
```

A closure literal is a borrow of a hidden value, its **environment**, which holds the borrows of what it
captures. The environment belongs to the block the literal is written in. Here that block is a branch of the
`if`, and `found` carries the closure out of it. The message says what to do: bind the closure to a local.

::: tip Rule
A closure cannot be used after the block its literal is written in. To keep one for several statements, bind
it with `let` in a block that lasts long enough.

*Specification: `type-system.md` §36.2, "Loans".*
:::

<span class="listing-caption"><strong>Listing 13-24</strong>, <code>src/look.cee</code>: The final <code>run</code>. Both closures are locals of the function, and the loop prints each line as it is found.</span>

<!-- ceesafe: skip -->
```ceesafe
/// Reads the file, searches it and prints the lines found. The result is how many lines it printed.
fn run(config: borrow Config) -> Result<usize, LookError> {
    let bytes = readAll(config.path)?;
    let text = match (StringView.fromUtf8(bytes.view())) {
        Some(text) => text,
        null => { return Err(LookError.NotUtf8); }
    };
    let exact = (line: StringView) => line.contains(config.query);
    let anyCase = (line: StringView) => containsIgnoreCase(line, config.query);
    var count: usize = 0;
    for (line in search(text, if (config.ignoreCase) anyCase else exact)) {
        Console.writeLine(line);
        count += 1;
    }
    return Ok(count);
}
```

The closures are in a `let`, so their parameter type is written. Making a closure runs nothing: the one that
is not chosen costs two borrows on the stack. The `for` loop takes the iterator that `search` returns, calls
its `next`, and prints each line when it is found. No vector is built, so `run` counts the lines itself.

The tests call `next` themselves. A test is a good place to see an iterator work one step at a time:

<span class="listing-caption"><strong>Listing 13-25</strong>, <code>src/look.cee</code>: The first test asks for the first match, then checks that there is no second.</span>

<!-- ceesafe: skip -->
```ceesafe
@test
fn findsOneLine() -> Result<void, TestFailure> {
    let text = "one fish\ntwo fish\nred herring\n";
    var found = search(text, (line) => line.contains("red"));
    Check.equalText("red herring", found.next() ?? "")?;
    Check.isTrue(found.next().isNull(), "one line only")?;
    return Ok();
}
```

`found` is a `var` because `next` changes it. Here the closure literal is an argument in the `let` statement
of the function's own block, so it lives to the end of the test.

```sh
ceesafec test
ceesafec build
./out/lookfor tide harbour.txt
LOOKFOR_IGNORE_CASE=1 ./out/lookfor tide harbour.txt
```

```text
test caseIgnored ... ok
test caseMatters ... ok
test findsOneLine ... ok

ceesafec test: 3 passed, 0 failed
Nets dry on the wall, and the tide is low.
Nets dry on the wall, and the tide is low.
The Tide turns at noon.
```

This is the package `book/projects/ch13/lookfor`.

## What changed

| | Chapter 12 | Now |
|---|---|---|
| Ways to match | a function for each | a closure for each, one `search` |
| Matching lines | collected in a `Vector`, then printed | printed as each is found |
| `search` can fail | yes, with `AllocError` | no |
| A new way to match | copy a function | write a closure |

The program's behaviour, its messages and its exit statuses are the same.
[What they cost](./ch13-04-what-they-cost) measures the three versions.

## Mistakes

::: warning Mistake: a closure literal in a branch, kept by an iterator
This is Listing 13-23. The closure ends with the branch it is written in, and the iterator is used after it
(`N0243`). Bind the closure with `let` before the `if`, as Listing 13-24 does.
:::

::: warning Mistake: an iterator where the values are needed more than once
An iterator gives each value once, in order.
:::

If the program needed the matches twice, or sorted, or their count before the first is printed, the vector
of step 1 would be the right design. Choose the iterator when the caller reads the values once.

::: details Coming from Rust
The Rust book's version of this step is `contents.lines().filter(|line| line.contains(query))`. CeeSafe has
no `filter`, so `Matches` is that adapter written out, and its closure field has a named type,
`borrow(r) fn(StringView) -> bool`, where Rust would have a type parameter.
:::

## Summary

- When two functions differ in one decision, make the decision a closure parameter.
- When a function builds a collection that its caller reads once, return an iterator: a struct with the
  state and a `next` method.
- A struct that holds a closure or a view has a region parameter, and so has the function that returns it.
- A closure that an iterator keeps must be bound with `let` in a block that outlives the iterator (`N0243`).
- Test an iterator by calling `next` and checking each value, then `null`.

Next: [What they cost](./ch13-04-what-they-cost).
