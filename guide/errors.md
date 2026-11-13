# Errors

CeeSafe has two kinds of failure, and keeps them apart:

- **Recoverable errors** are values: a function that can fail returns `Result<T, E>`, and the caller decides what to
  do. Allocation failure and I/O errors are of this kind.
- **Bugs** stop the program: an index out of bounds, an integer overflow, dividing by zero, unwrapping an error or a
  `null`. This is a **panic**: the program prints the reason and where it happened, and exits at once. There is no
  unwinding and no catching.

## `Result`

```ceesafe
enum ParseError {
    Empty,
    BadDigit(u8),
    TooLarge,
}

fn parse(text: StringView) -> Result<u32, ParseError> {
    if (text.length == 0) {
        return Err(ParseError.Empty);
    }
    var value: u32 = 0;
    for (c in text.bytes()) {
        if (c < '0' || c > '9') {
            return Err(ParseError.BadDigit(c));
        }
        let next = value.checkedMul(10);
        if (next == null) {
            return Err(ParseError.TooLarge);
        }
        value = next! + (c - '0') as u32;
    }
    return Ok(value);
}

fn main() {
    match (parse("2026")) {
        Ok(n) => { Console.writeLine(n); }
        Err(ParseError.BadDigit(c)) => {
            Console.write("not a digit: ");
            Console.writeLine(c);
        }
        Err(_) => { Console.writeLine("no number"); }
    }
    match (parse("20x6")) {
        Ok(n) => { Console.writeLine(n); }
        Err(ParseError.BadDigit(c)) => {
            Console.write("not a digit: ");
            Console.writeLine(c);
        }
        Err(_) => { Console.writeLine("no number"); }
    }
}
```

```text
2026
not a digit: 120
```

`Result<T, E>` is an enum with two variants, `Ok(T)` and `Err(E)`, matched without a type prefix. `T` may be `void`
(`Result<void, E>`, with `Ok()`); `E` is usually an enum of the ways the function can fail.

## `?`: pass the error on

In a function that returns a `Result` with the same error type, `e?` is the value inside `Ok`, or returns the `Err`
to the caller at once. Every local is destroyed on the way out, as for any `return`.

```ceesafe
enum ParseError {
    Empty,
    BadDigit(u8),
}

fn digit(c: u8) -> Result<u32, ParseError> {
    if (c < '0' || c > '9') {
        return Err(ParseError.BadDigit(c));
    }
    return Ok((c - '0') as u32);
}

fn twoDigits(text: StringView) -> Result<u32, ParseError> {
    if (text.length != 2) {
        return Err(ParseError.Empty);
    }
    return Ok(digit(text[0])? * 10 + digit(text[1])?);
}

fn main() {
    Console.writeLine(twoDigits("42")!);
    match (twoDigits("4x")) {
        Ok(n) => { Console.writeLine(n); }
        Err(_) => { Console.writeLine("not two digits"); }
    }
}
```

```text
42
not two digits
```

The same works on optionals: `o?` returns `null` from a function that returns an optional.

Where the error types differ, `?` converts in one step, if a declaration says how. The function's error enum may hold
the other error in a variant marked `@from`; or the other error's type may have a method marked `@into`, of the form
`fn name(this) -> F`, that makes the function's error of it (one such method per type):

```ceesafe
enum LoadError {
    @from Parse(ParseError), // `?` on a Result<_, ParseError> returns Err(LoadError.Parse(e))
    Missing,
}

enum ParseError {
    Empty,
}

struct Refused {
    line: u32;
    text: String;

    @into
    fn error(this) -> LoadError => LoadError.Missing; // `?` on a Result<_, Refused> returns Err(e.error())
}

fn parse(text: StringView) -> Result<u32, ParseError> => if (text.length == 0) Err(ParseError.Empty) else Ok(1);

fn find(line: u32) -> Result<u32, Refused> => Err(Refused { line: line, text: String.from("no such line")! });

fn load(text: StringView) -> Result<u32, LoadError> => Ok(parse(text)? + find(7)?);

fn main() {
    match (load("x")) {
        Err(LoadError.Missing) => { Console.writeLine("missing"); }
        _ => { Console.writeLine("other"); }
    }
}
```

```text
missing
```

`@from` keeps the error; `@into` is for an error the function's type cannot hold, or should not: the method takes the
error by value, so what it does not keep (the `String` here) is destroyed. Only `?` converts: `return Err(e)` names
the variant or calls the method itself.

## `!`: this cannot fail

`e!` is the value inside `Ok` (or a present optional's value). If it is an `Err` (or `null`), the program panics. Use
it where failure would be a bug, or in small programs and tests where stopping is the right answer:

```ceesafe
fn main() {
    var names = Vector<StringView>.create();
    names.push("ada")!;                  // allocation failure stops this program
    names.push("grace")!;
    Console.writeLine(names.length);
}
```

```text
2
```

## Allocation failure is a value

Every operation that allocates returns a `Result<…, AllocError>`: `Box.create`, `Vector.push`, `String.append`,
`Arena.alloc` and the rest. A program that must survive running out of memory, such as a kernel, handles it; a
program that need not, writes `!` or passes it on with `?`:

```ceesafe
struct Node {
    value: i32;
    next: Box<Node>?;
}

fn push(head: Box<Node>?, value: i32) -> Result<Box<Node>, AllocError> =>
    Box.create(Node { value: value, next: head });

fn build(n: i32) -> Result<Box<Node>?, AllocError> {
    var head: Box<Node>? = null;
    for (i in 0..n) {
        head = push(move head, i)?;
    }
    return Ok(head);
}

fn main() {
    match (build(3)) {
        Ok(list) => { Console.writeLine(list!.value); }
        Err(AllocError.OutOfMemory) => { Console.writeLine("out of memory"); }
    }
}
```

```text
2
```

The test suite runs every allocating program again with each allocation failing in turn, to check that every
failure path cleans up after itself.

## Panics

A panic prints the reason and the source location, and exits with status 134:

<!-- ceesafe: panics -->
```ceesafe
fn main() {
    let values = [1, 2, 3];
    var i: usize = 0;
    while (true) {
        Console.writeLine(values[i]);
        i += 1;
    }
}
```

```text
1
2
3
```

Here the fourth read stops the program with `panic: index out of bounds` and the file and line of `values[i]`.
Panics never run `drop` blocks or deferred statements: the program is in a state its author did not expect, so
nothing more of it runs.

## I/O errors

Files, sockets and processes report `IoError`, a built-in enum (`NotFound`, `PermissionDenied`, `AlreadyExists`, …,
`Other(code)`), whose `message()` is the C library's text:

<!-- ceesafe: exits 1 -->
```ceesafe
fn main() -> i32 {
    match (File.open("/no/such/file")) {
        Ok(file) => {
            Console.writeLine("opened");
            return 0;
        }
        Err(e) => {
            Console.writeLine(e.message());
            return 1;
        }
    }
}
```

```text
No such file or directory
```

## Next

[Memory and allocation](memory.md).
