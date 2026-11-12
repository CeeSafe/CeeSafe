---
title: Reading a file
summary: Reading a whole file into a Vector of bytes with File.open and a read loop, the IoError each step can give, and turning the bytes into text with StringView.fromUtf8, which is null when they are not UTF-8.
assumes: [ch12-01-arguments, ch08-01-vectors, ch09-02-result]
keywords: [File.open, read, IoError, Vector, extend, StringView.fromUtf8, UTF-8, bytes, message]
diagnostics: [N0261, N0207]
specification: ["type-system.md §26.2", "type-system.md §26.3"]
---

# Reading a file

**This section teaches** how to read a whole file into memory, which errors that can give, and how the bytes
become text. It continues the program of [section 12.1](./ch12-01-arguments), and assumes
[vectors](./ch08-01-vectors) and [`Result` with `?`](./ch09-02-result).

## A file is bytes, read a piece at a time

The library has no single call that returns a file's contents. It gives you the two steps such a call would be
made of, and you write the loop:

- `File.open(path)` is a `Result<File, IoError>`: the open file, or the reason it could not be opened.
- `file.read(mut buffer)` fills as much of `buffer` as it can and is a `Result<usize, IoError>`: how many bytes
  it read. It is `0` at the end of the file.

The function below puts the two together. It is the first function of the project that can fail, so it returns
a `Result`.

<span class="listing-caption"><strong>Listing 12-5</strong>, <code>src/main.cee</code>: <code>readAll</code> reads the file that the second argument names, and <code>main</code> prints it.</span>

<!-- ceesafe: exits 2 -->
```ceesafe
fn readAll(path: StringView) -> Result<Vector<u8>, IoError> {
    var file = File.open(path)?;
    var bytes = Vector<u8>.create();
    var buffer = [0u8; 4096];
    loop {
        let n = file.read(mut buffer[..])?;
        if (n == 0) {
            break;
        }
        match (bytes.extend(buffer[0..n])) {
            Ok() => {}
            Err(_) => { return Err(IoError.OutOfMemory); }
        }
    }
    return Ok(bytes);
}

fn main() {
    let query = match (Process.argument(1)) {
        Some(text) => text,
        null => {
            Console.writeLine("usage: lookfor <text> <file>");
            Process.exit(2);
        }
    };
    let path = match (Process.argument(2)) {
        Some(text) => text,
        null => {
            Console.writeLine("usage: lookfor <text> <file>");
            Process.exit(2);
        }
    };
    Console.write("looking for: ");
    Console.writeLine(query);
    let bytes = match (readAll(path)) {
        Ok(bytes) => bytes,
        Err(error) => {
            Console.write("lookfor: ");
            Console.writeLine(error.message());
            Process.exit(2);
        }
    };
    match (StringView.fromUtf8(bytes.view())) {
        Some(text) => { Console.write(text); }
        null => {
            Console.writeLine("lookfor: the file is not UTF-8 text");
            Process.exit(2);
        }
    }
}
```

```text
usage: lookfor <text> <file>
```

With no arguments it still prints the usage line. With the two arguments it prints the file:

```sh
ceesafec run --interp -- tide harbour.txt
```

```text
looking for: tide
The harbour wakes before the town.
Nets dry on the wall, and the tide is low.
A boat goes out; the harbour bell rings twice.
Nobody on the quay looks up.
The Tide turns at noon.
By evening the boats are home.
```

## What `readAll` does

- `var file = File.open(path)?;` opens the file for reading. `?` gives the `File` if the result is `Ok`, and
  returns the `Err` from `readAll` at once if it is not. The file closes by itself when `file` goes out of
  scope, on every path out of the function.
- `var buffer = [0u8; 4096];` is an array of 4096 zero bytes, on the stack. It is where each piece lands.
- `file.read(mut buffer[..])` lends the whole array to `read` as a slice it may write. `mut` at the call shows
  that the call changes `buffer`.
- `bytes.extend(buffer[0..n])` appends the `n` bytes just read to the vector. Only the first `n` bytes of the
  buffer are new; the rest is left from earlier turns of the loop.
- The loop ends when `read` gives `0`.

`extend` may have to grow the vector, and growing can fail: it returns a `Result<void, AllocError>`, as
[chapter 9](./ch09-03-allocation-failure) explains. `readAll` returns `IoError`, a different type, so `?` does
not fit there. The `match` turns the failure into `IoError.OutOfMemory`, one of the variants of `IoError`. Section 12.3 replaces the `match` with something shorter.

::: tip Rule
Reading a file is a loop: `file.read(mut buffer)` until it gives `0`. Every step returns a `Result`, and a `File`
closes when it is destroyed.

*Specification: `type-system.md` §26.3.*
:::

## The errors

`IoError` is a built-in enum. Its variants name what went wrong, among them `NotFound`, `PermissionDenied` and
`IsDirectory`, and `error.message()` gives a line of text for any of them. The program prints that text:

```sh
ceesafec run --interp -- tide nothere.txt
```

```text
looking for: tide
lookfor: No such file or directory
```

```sh
ceesafec run --interp -- tide src
```

```text
looking for: tide
lookfor: Is a directory
```

Both runs end with the status 2. Because `IoError` is an enum, a program that wants to treat one failure
specially matches on it:

<span class="listing-caption"><strong>Listing 12-6</strong>, <code>main.cee</code>: One variant of <code>IoError</code> gets its own arm, and the others share the built-in message.</span>

```ceesafe
fn main() {
    match (File.open("/no/such/file")) {
        Ok(_) => { Console.writeLine("opened"); }
        Err(IoError.NotFound) => { Console.writeLine("there is no such file"); }
        Err(error) => { Console.writeLine(error.message()); }
    }
}
```

```text
there is no such file
```

## Bytes are not yet text

`readAll` returns a `Vector<u8>`: bytes. A file can hold anything. Text in CeeSafe, a `StringView` or a
`String`, is always valid UTF-8, so the bytes must be checked before they are treated as text.
`StringView.fromUtf8(bytes)` does the check and gives an optional: a view of the same bytes as text, or `null`.

<span class="listing-caption"><strong>Listing 12-7</strong>, <code>main.cee</code>: Three bytes that are not UTF-8, then the first two of them, which are.</span>

```ceesafe
fn main() {
    let bytes = [72u8, 105, 0xFF];
    match (StringView.fromUtf8(bytes[..])) {
        Some(text) => { Console.writeLine(text); }
        null => { Console.writeLine("not UTF-8"); }
    }
    Console.writeLine(StringView.fromUtf8(bytes[0..2]) ?? "not UTF-8");
}
```

```text
not UTF-8
Hi
```

The view copies nothing. It borrows the bytes, so in Listing 12-5 the text is a view into the vector `bytes`,
and the vector must stay alive as long as the text is used. The compiler checks that.

A file in an older encoding shows the `null` arm of Listing 12-5. This makes one, with the byte `E9`, which is
`é` in Latin-1 and is not valid UTF-8 on its own:

```sh
printf 'caf\351\n' > latin1.txt
ceesafec run --interp -- caf latin1.txt
```

```text
looking for: caf
lookfor: the file is not UTF-8 text
```

::: tip Rule
`StringView.fromUtf8(bytes)` is a `StringView?`: the bytes as text, or `null` if they are not valid UTF-8. It
borrows the bytes and copies nothing.
:::

## Mistakes

::: warning Mistake: `?` in `main`
`?` returns the error from the function it is in, so that function must return a `Result`. `main` does not.
:::

<span class="listing-caption"><strong>Listing 12-8</strong>, <code>main.cee</code>: <code>?</code> has nowhere to return the error to. This does not compile.</span>

<!-- ceesafe: fail N0261 -->
```ceesafe
fn main() {
    var file = File.open("harbour.txt")?;
    var buffer = [0u8; 64];
    let n = file.read(mut buffer[..])?;
    Console.writeLine(n);
}
```

```text
main.cee:2:16: error[N0261]: '?' returns 'Err' from the function, so 'main' must return 'Result<_, IoError>'
  |
2 |     var file = File.open("harbour.txt")?;
  |                ^^^^^^^^^^^^^^^^^^^^^^^^^

main.cee:4:13: error[N0261]: '?' returns 'Err' from the function, so 'main' must return 'Result<_, IoError>'
  |
4 |     let n = file.read(mut buffer[..])?;
  |             ^^^^^^^^^^^^^^^^^^^^^^^^^^
```

Do not follow the message's advice for `main`: `main` returns nothing, or an `i32`, and cannot return a
`Result`. Put the work in a function that returns a `Result`, as `readAll` does, and let `main` `match` on what
it returns.

::: warning Mistake: printing bytes
`Console.writeLine` prints text and numbers. A slice of bytes is neither.
:::

<span class="listing-caption"><strong>Listing 12-9</strong>, <code>main.cee</code>: Bytes given to a function that prints text. This does not compile.</span>

<!-- ceesafe: fail N0207 -->
```ceesafe
fn main() {
    var bytes = Vector<u8>.create();
    bytes.push(104)!;
    bytes.push(105)!;
    Console.writeLine(bytes.view());
}
```

```text
main.cee:5:23: error[N0207]: 'Console.writeLine' can print integers, floats, bools and string views, not '[u8]'
  |
5 |     Console.writeLine(bytes.view());
  |                       ^^^^^^^^^^^^
```

Check the bytes with `StringView.fromUtf8` and print the view it gives. To write bytes as they are, checked or
not, there is `Console.writeBytes(bytes.view())`.

::: details Coming from Rust, Python or Go
There is no `fs::read_to_string(path)`, `open(path).read()` or `os.ReadFile(path)`: you write the loop of
Listing 12-5 once and keep it. The two failures that `read_to_string` folds into one error, an I/O error and
invalid UTF-8, are separate steps here: an `IoError` from `File`, then a `null` from `StringView.fromUtf8`.
:::

## Summary

- `File.open(path)` gives a `Result<File, IoError>`; `file.read(mut buffer[..])` gives how many bytes it read,
  `0` at the end.
- Read a whole file with a loop that appends each piece to a `Vector<u8>` with `extend`.
- `error.message()` is an `IoError`'s text; match on a variant such as `IoError.NotFound` to treat it specially.
- `StringView.fromUtf8(bytes)` checks the bytes and gives a `StringView?` that borrows them.
- `?` needs a function that returns a `Result` (`N0261`), so `main` uses `match`.

Next: [refactoring](./ch12-03-refactoring), because `main` is now doing four jobs.
