---
title: Working with the environment
summary: Reading an environment variable with Environment.variable, an optional view, to switch on a search that ignores the case of ASCII letters, written test first and carried to run in the configuration struct.
assumes: [ch12-04-test-first, ch06-03-optionals, ch11-01-writing-tests]
keywords: [Environment.variable, environment variable, isSome, equalsIgnoreAsciiCase, bytes, case-insensitive, usize, overflow]
diagnostics: [N0205]
specification: ["type-system.md §26.9"]
---

# Working with the environment

**This section teaches** how a program reads an environment variable, and uses one to give `lookfor` a search
that ignores case. It continues from [section 12.4](./ch12-04-test-first), and assumes
[optionals](./ch06-03-optionals) and [tests](./ch11-01-writing-tests).

An **environment variable** is a named text that a program inherits from whatever started it. A user sets one
once, in a shell or its start-up file, and every later run sees it. That suits an option that a person wants
always on, more than an argument typed each time. `lookfor` will ignore case when the variable
`LOOKFOR_IGNORE_CASE` is set.

## Reading a variable

<span class="listing-caption"><strong>Listing 12-19</strong>, <code>main.cee</code>: A variable that is set gives its text. One that is not gives <code>null</code>.</span>

```ceesafe
fn main() {
    match (Environment.variable("LOOKFOR_IGNORE_CASE")) {
        Some(value) => {
            Console.write("set to: ");
            Console.writeLine(value);
        }
        null => { Console.writeLine("not set"); }
    }
    Console.writeLine(Environment.variable("LOOKFOR_IGNORE_CASE").isSome());
}
```

```text
not set
false
```

`Environment.variable(name)` is a `StringView?`: the variable's value, or `null` if there is no such variable.
Like an argument, the value is text that lasts for the whole run, and nothing is copied. Set the variable for
one command by writing it first on the line, in a POSIX shell:

```sh
LOOKFOR_IGNORE_CASE=1 ceesafec run --interp main.cee
```

```text
set to: 1
true
```

`lookfor` asks only whether the variable is set, with `isSome()`. Its value does not matter: `1`, `yes` and
even an empty value all switch the option on.

::: tip Rule
`Environment.variable(name)` is a `StringView?`: `null` if the variable is not set, or its value is not UTF-8.
A program can read its environment and cannot change it.

*Specification: `type-system.md` §26.9.*
:::

## The tests first

Two tests, added to the end of `src/look.cee`. The first pins down what `search` already does, so that the new
work cannot change it unnoticed. The second is for a function that does not exist yet.

<span class="listing-caption"><strong>Listing 12-20</strong>, <code>src/look.cee</code>: A test that <code>search</code> is exact about case, and one for a search that is not.</span>

<!-- ceesafe: skip -->
```ceesafe
@test
fn caseMatters() -> Result<void, TestFailure> {
    let text = "Red sky\nred herring\nblue moon\n";
    let found = search("red", text)!;
    Check.equalU64(1, found.length as u64)?;
    Check.equalText("red herring", found[0])?;
    return Ok();
}

@test
fn caseIgnored() -> Result<void, TestFailure> {
    let text = "Red sky\nred herring\nblue moon\n";
    let found = searchIgnoreCase("rEd", text)!;
    Check.equalU64(2, found.length as u64)?;
    Check.equalText("Red sky", found[0])?;
    Check.equalText("red herring", found[1])?;
    return Ok();
}
```

The package does not compile at this point: the compiler cannot find `searchIgnoreCase`. That is this step's
failing test.

## The search that ignores case

The library can compare two texts without regard to case: `a.equalsIgnoreAsciiCase(b)`. It has no "contains"
of that kind, and no function that makes a lower-case copy of a text. So `containsIgnoreCase` tries the
comparison at each position of the line where the query could start.

<span class="listing-caption"><strong>Listing 12-21</strong>, <code>src/look.cee</code>: A <code>contains</code> that ignores case, and the search that uses it.</span>

```ceesafe
/// Whether `line` contains `query`, taking `A` to `Z` and `a` to `z` as the same letters.
fn containsIgnoreCase(line: StringView, query: StringView) -> bool {
    if (query.length > line.length) {
        return false;
    }
    let bytes = line.bytes();
    for (start in 0..line.length - query.length + 1) {
        if (bytes[start..start + query.length].equalsIgnoreAsciiCase(query.bytes())) {
            return true;
        }
    }
    return false;
}

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

- `line.bytes()` is the line as a slice of bytes, and `bytes[start..start + query.length]` is the window of it
  that is as long as the query. The function works on bytes because a window may begin or end in the middle of
  a character that takes several bytes. A slice of bytes allows that. A `StringView` must hold whole
  characters, so slicing the view itself there would panic.
- `equalsIgnoreAsciiCase` exists for byte slices as well as for views. It treats the 26 letters `A` to `Z` as
  equal to `a` to `z`, and every other byte as itself. `É` and `é` are different to it.
- The first `if` is needed, not only quick. The mistake below shows why.

`searchIgnoreCase` has the shape of `search`, with the same region in its signature for the same reason.

```sh
ceesafec test
```

```text
test caseIgnored ... ok
test caseMatters ... ok
test findsOneLine ... ok

ceesafec test: 3 passed, 0 failed
```

## From the variable to `run`

The option is part of what the user asked for, so it goes in `Config` as a third field, after `path`:
`ignoreCase: bool;`. `configure` fills it:

<span class="listing-caption"><strong>Listing 12-22</strong>, <code>src/look.cee</code>: <code>configure</code> reads the environment as well as the arguments.</span>

<!-- ceesafe: skip -->
```ceesafe
/// The configuration, from the program's arguments and its environment.
fn configure() -> Result<Config<static>, LookError> {
    let query = match (Process.argument(1)) {
        Some(text) => text,
        null => { return Err(LookError.MissingQuery); }
    };
    let path = match (Process.argument(2)) {
        Some(text) => text,
        null => { return Err(LookError.MissingPath); }
    };
    let ignoreCase = Environment.variable("LOOKFOR_IGNORE_CASE").isSome();
    return Ok(Config { query: query, path: path, ignoreCase: ignoreCase });
}
```

And `run` chooses the search:

<span class="listing-caption"><strong>Listing 12-23</strong>, <code>src/look.cee</code>: <code>run</code> picks the search that the configuration asks for.</span>

<!-- ceesafe: skip -->
```ceesafe
/// Reads the file, searches it and prints the lines found.
fn run(config: borrow Config) -> Result<void, LookError> {
    let bytes = readAll(config.path)?;
    let text = match (StringView.fromUtf8(bytes.view())) {
        Some(text) => text,
        null => { return Err(LookError.NotUtf8); }
    };
    let found = if (config.ignoreCase) searchIgnoreCase(config.query, text)? else search(config.query, text)?;
    for (line in found) {
        Console.writeLine(line);
    }
    return Ok();
}
```

The `if` is an expression: `found` is the result of one search or the other.

Only `configure` knows about the environment. The two search functions take plain values, which is why the
tests can call them without setting any variable. Keep the reading of the outside world at the edge of a
program, and the logic stays testable.

```sh
ceesafec run --interp -- tide harbour.txt
```

```text
Nets dry on the wall, and the tide is low.
```

```sh
LOOKFOR_IGNORE_CASE=1 ceesafec run --interp -- tide harbour.txt
```

```text
Nets dry on the wall, and the tide is low.
The Tide turns at noon.
```

## Mistakes

::: warning Mistake: subtracting lengths that may be the wrong way round
Lengths are `usize`, which has no negative values. If the query is longer than the line,
`line.length - query.length` has no answer, and CeeSafe panics where another language would wrap to a huge
number and read past the end of the line.
:::

<span class="listing-caption"><strong>Listing 12-24</strong>, <code>main.cee</code>: Without the check on the lengths, a query longer than the line panics.</span>

<!-- ceesafe: panics -->
```ceesafe
fn containsIgnoreCase(line: StringView, query: StringView) -> bool {
    let bytes = line.bytes();
    for (start in 0..line.length - query.length + 1) {
        if (bytes[start..start + query.length].equalsIgnoreAsciiCase(query.bytes())) {
            return true;
        }
    }
    return false;
}

fn main() {
    Console.writeLine(containsIgnoreCase("The Tide turns", "tide"));
    Console.writeLine(containsIgnoreCase("low", "tide"));
}
```

```text
true
```

The first call prints `true`. The second stops the program:

```text
ceesafe panic: integer overflow at main.cee:3:22
```

Compare the lengths before subtracting, as Listing 12-21 does. A test with a short line would have found this;
the test `caseIgnored` does not have one, and adding it is a good exercise.

::: warning Mistake: an optional as a condition
An optional is not a `bool`. "Is it set?" is a question you ask of it.
:::

<span class="listing-caption"><strong>Listing 12-25</strong>, <code>main.cee</code>: An optional where a condition is wanted. This does not compile.</span>

<!-- ceesafe: fail N0205 -->
```ceesafe
fn main() {
    if (Environment.variable("LOOKFOR_IGNORE_CASE")) {
        Console.writeLine("ignoring case");
    }
}
```

```text
main.cee:2:9: error[N0205]: expected 'bool', found 'StringView?'
  |
2 |     if (Environment.variable("LOOKFOR_IGNORE_CASE")) {
  |         ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
```

Write `Environment.variable("LOOKFOR_IGNORE_CASE").isSome()`.

::: details Coming from Rust
`Environment.variable(name)` is `std::env::var(name).ok()`, and `.isSome()` is `.is_ok()` there. There is no
`to_lowercase()` that returns a new `String`: the Rust Book's version lowers the query and every line, which
allocates for each line; the version here compares in place and allocates nothing. It is ASCII only, where
`to_lowercase` knows Unicode.
:::

::: details Coming from C, Go or Python
`Environment.variable` is `getenv`, `os.LookupEnv` and `os.environ.get`. As with `getenv`, the text is not
copied; unlike `getenv`, the result cannot be used before it is tested for `null`.
:::

## Summary

- `Environment.variable(name)` is a `StringView?`; `isSome()` asks whether the variable is set.
- Read the environment in one place, at the edge, and pass the answer on as a field of the configuration.
- `a.equalsIgnoreAsciiCase(b)` compares views or byte slices, ignoring the case of ASCII letters only.
- Slice the bytes of a text, not the view, when a position may fall inside a character.
- Subtracting a larger `usize` from a smaller one panics: compare first.

Next: [errors to standard error](./ch12-06-standard-error), so that the output of `lookfor` can go to a file.
