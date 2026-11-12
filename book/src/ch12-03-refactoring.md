---
title: Refactoring for clarity and errors
summary: Splitting the program into a configuration struct with a region parameter, an error enum whose @from variants let ? convert library errors, a run function that returns a Result, and a short main in a file of its own.
assumes: [ch12-02-reading-a-file, ch05-01-defining-structs, ch06-01-defining-an-enum, ch07-02-files-and-public, ch09-02-result]
keywords: [refactoring, Config, region, static, "@from", LookError, run, Result, Process.exit, package, files]
diagnostics: [N0270, N0261]
specification: ["type-system.md §25.4"]
---

# Refactoring for clarity and errors

**This section teaches** how to give a growing program a shape: the settings in a struct, the failures in an
enum, the work in a function that returns a `Result`, and a `main` that only reports. It changes the program of
[section 12.2](./ch12-02-reading-a-file) without changing what it does. It assumes
[structs](./ch05-01-defining-structs), [enums](./ch06-01-defining-an-enum),
[`Result`](./ch09-02-result) and [packages of several files](./ch07-02-files-and-public).

## What is wrong with the program

The `main` of section 12.2 works, and has four faults that will grow with it:

1. It does four jobs: it reads arguments, reads a file, checks the text and prints.
2. `query` and `path` are two loose variables. They belong together: they are what the user asked for.
3. Each failure is handled where it happens, with its own message and its own `Process.exit`. There are four
   exits already.
4. Nothing in it can be tested, because everything is inside `main`.

The cure is the same for most command-line programs. `main` stays small: it gets a configuration, calls one
function, and reports an error if there is one. Everything else moves into functions that return a `Result`,
in another file.

## The logic, in a file of its own

Make a second file, `src/look.cee`. Every `.cee` file under `src/` belongs to the package, and the files of one
package share one scope: `main.cee` can use what `look.cee` declares without importing it.

<span class="listing-caption"><strong>Listing 12-10</strong>, <code>src/look.cee</code>: The configuration, the errors, and the functions that can fail.</span>

<!-- ceesafe: skip -->
```ceesafe
/// What the program was asked to do.
struct Config<region r> {
    query: StringView<r>;
    path: StringView<r>;
}

/// The ways a run can fail.
enum LookError {
    MissingQuery,
    MissingPath,
    NotUtf8,
    @from Io(IoError),
    @from Memory(AllocError),
}

/// The configuration, from the program's arguments.
fn configure() -> Result<Config<static>, LookError> {
    let query = match (Process.argument(1)) {
        Some(text) => text,
        null => { return Err(LookError.MissingQuery); }
    };
    let path = match (Process.argument(2)) {
        Some(text) => text,
        null => { return Err(LookError.MissingPath); }
    };
    return Ok(Config { query: query, path: path });
}

/// The whole file's bytes.
fn readAll(path: StringView) -> Result<Vector<u8>, LookError> {
    var file = File.open(path)?;
    var bytes = Vector<u8>.create();
    var buffer = [0u8; 4096];
    loop {
        let n = file.read(mut buffer[..])?;
        if (n == 0) {
            break;
        }
        bytes.extend(buffer[0..n])?;
    }
    return Ok(bytes);
}

/// Reads the file and prints it.
fn run(config: borrow Config) -> Result<void, LookError> {
    let bytes = readAll(config.path)?;
    let text = match (StringView.fromUtf8(bytes.view())) {
        Some(text) => text,
        null => { return Err(LookError.NotUtf8); }
    };
    Console.write(text);
    return Ok();
}
```

There are three new things in it.

### A struct that holds views

`Config` groups the two settings. Its fields are views, and a view is a borrow of text that lives somewhere
else. A struct that stores borrows must say so, with a **region parameter**: `struct Config<region r>` reads
"a `Config` holds borrows that are valid for some region `r`", and each field names it, `StringView<r>`.
[Section 10.3](./ch10-03-regions) covers regions.

`configure` returns a `Config<static>`. `static` is the region of text that lasts for the whole run of the
program. The arguments are such text, which is why `Process.argument` can hand out views of them with nothing
to keep alive. `run` takes a `borrow Config` and does not name the region: for a parameter the compiler fills
it in.

::: tip Rule
A struct with a `StringView` field needs a region parameter: `struct Config<region r>` with fields of type
`StringView<r>`.

*Specification: `type-system.md` §25.4.*
:::

### One error type for the program

`LookError` lists every way a run can fail. Three variants are the program's own. The other two carry an error
from the library, and are marked `@from`.

`@from` is what lets `?` work in `readAll` now. `File.open(path)?` has an `IoError` to return, and `readAll`
returns `LookError`. Because `LookError` has the variant `@from Io(IoError)`, `?` wraps the error for you and
returns `Err(LookError.Io(e))`. In the same way `bytes.extend(buffer[0..n])?` returns
`Err(LookError.Memory(e))`, and the `match` that section 12.2 needed there is gone.

::: tip Rule
In a function that returns `Result<T, F>`, `?` on a `Result` whose error type is `E` needs `F` to be `E`, or
an enum with a variant `@from Name(E)`.
:::

### `run` returns a `Result`

`run` does the work and prints nothing about failure. It returns `Result<void, LookError>`: `Ok()` with no
value, or the error. Whoever calls it decides what an error means. Here that is `main`; in a test it is the
test.

## A `main` that reports

<span class="listing-caption"><strong>Listing 12-11</strong>, <code>src/main.cee</code>: <code>main</code> gets the configuration, runs, and reports any error in one place.</span>

<!-- ceesafe: skip -->
```ceesafe
fn report(error: LookError) {
    Console.write("lookfor: ");
    match (error) {
        LookError.MissingQuery => { Console.writeLine("no text to look for"); }
        LookError.MissingPath => { Console.writeLine("no file to look in"); }
        LookError.NotUtf8 => { Console.writeLine("the file is not UTF-8 text"); }
        LookError.Io(e) => { Console.writeLine(e.message()); }
        LookError.Memory(_) => { Console.writeLine("out of memory"); }
    }
}

fn main() {
    let config = match (configure()) {
        Ok(config) => config,
        Err(error) => {
            report(error);
            Console.writeLine("usage: lookfor <text> <file>");
            Process.exit(2);
        }
    };
    match (run(config)) {
        Ok() => {}
        Err(error) => {
            report(error);
            Process.exit(2);
        }
    }
}
```

`report` is the only place that turns an error into words. Its `match` has no `_` arm, so when a later section
adds a variant to `LookError`, the compiler points at this `match` until the new variant has a message.

`main` exits with 2 after any error. A configuration error also prints the usage line, because the user typed
the command wrongly; a file that cannot be read does not, because the command was right.

Check the package and try each path through it:

```sh
ceesafec check
ceesafec run --interp
```

```text
lookfor: no text to look for
usage: lookfor <text> <file>
```

```sh
ceesafec run --interp -- tide
```

```text
lookfor: no file to look in
usage: lookfor <text> <file>
```

```sh
ceesafec run --interp -- tide nothere.txt
```

```text
lookfor: No such file or directory
```

With `tide harbour.txt` it prints the six lines of the file, as before. The line `looking for: tide` is gone:
it was there to show the argument had arrived.

## Mistakes

::: warning Mistake: a view in a struct without a region
This is the struct most people write first.
:::

<span class="listing-caption"><strong>Listing 12-12</strong>, <code>main.cee</code>: The struct holds views and has no region parameter. This does not compile.</span>

<!-- ceesafe: fail N0270 -->
```ceesafe
struct Config {
    query: StringView;
    path: StringView;
}

fn main() {
    let config = Config { query: "tide", path: "harbour.txt" };
    Console.writeLine(config.query);
}
```

```text
main.cee:2:12: error[N0270]: a field of scoped type 'StringView' needs a region parameter on the struct: 'struct Config<region r>' (type-system.md §25.4)
  |
2 |     query: StringView;
  |            ^^^^^^^^^^

main.cee:3:11: error[N0270]: a field of scoped type 'StringView' needs a region parameter on the struct: 'struct Config<region r>' (type-system.md §25.4)
  |
3 |     path: StringView;
  |           ^^^^^^^^^^
```

The message gives the fix: `struct Config<region r>`, and `StringView<r>` for each field, as in Listing 12-10.

::: warning Mistake: `?` on an error of another type
A variant that carries the library's error is not enough. Without `@from`, `?` does not use it.
:::

<span class="listing-caption"><strong>Listing 12-13</strong>, <code>main.cee</code>: <code>Io</code> is not marked <code>@from</code>, so <code>?</code> cannot turn an <code>IoError</code> into a <code>LookError</code>. This does not compile.</span>

<!-- ceesafe: fail N0261 -->
```ceesafe
enum LookError {
    MissingPath,
    Io(IoError),
}

fn firstByte(path: StringView) -> Result<u8, LookError> {
    var file = File.open(path)?;
    var buffer = [0u8; 1];
    let _ = file.read(mut buffer[..])?;
    return Ok(buffer[0]);
}

fn main() {
    match (firstByte("/no/such/file")) {
        Ok(b) => { Console.writeLine(b); }
        Err(_) => { Console.writeLine("failed"); }
    }
}
```

```text
main.cee:7:16: error[N0261]: '?' would return 'Err' of type 'IoError', but 'firstByte' returns errors of type 'LookError'; mark a variant of the function's error enum '@from' to convert, as in '@from Variant(IoError)'
  |
7 |     var file = File.open(path)?;
  |                ^^^^^^^^^^^^^^^^

main.cee:9:13: error[N0261]: '?' would return 'Err' of type 'IoError', but 'firstByte' returns errors of type 'LookError'; mark a variant of the function's error enum '@from' to convert, as in '@from Variant(IoError)'
  |
9 |     let _ = file.read(mut buffer[..])?;
  |             ^^^^^^^^^^^^^^^^^^^^^^^^^^
```

Write `@from Io(IoError),` and it compiles.

::: details Coming from Rust
`@from` on a variant does the work of `impl From<io::Error> for LookError`, or of `#[from]` in the `thiserror`
crate. A region parameter is a lifetime parameter: `struct Config<region r>` is `struct Config<'a>`, and
`static` is `'static`. There is no `lib.rs` and `main.rs` split and no `mod` line: the files under `src/` are
one package and one scope, and tests can go beside the code in either.
:::

::: details Coming from Go, C# or Python
There are no exceptions to throw from deep inside and catch in `main`. The error travels as the function's
return value, and `?` is the short way to write "if this failed, return its error". The chain from `File.open`
to `main` is visible in every signature on the way.
:::

## Summary

- `main` gets a configuration, calls `run`, and reports; the logic lives in functions that return a `Result`.
- The files under `src/` share one scope, so moving code to a new file needs no imports.
- A struct that holds views has a region parameter (`N0270`); views of arguments are `static`.
- One error enum for the program, with `@from` variants so that `?` converts the library's errors (`N0261`).
- One function turns errors into messages, with a `match` that has no `_` arm.

Next: [the search itself](./ch12-04-test-first), written test first, now that there is a place a test can reach.
