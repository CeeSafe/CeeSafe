# Built-in types and functions

The types and functions below are part of the compiler, not of a library, so they have no page in the library
reference. The libraries' own types, such as `TextWriter`, `Timer`, `Environment`, `HashMap` and the async
sockets, are in [lib/core](../spec/api/core.md) and [lib/std](../spec/api/std.md). Every example here is compiled
and run by the guide's checks, so the names and signatures are the compiler's. With the MCP server,
`ceesafe_api` looks any of these names up.

## `Console`

| Function | Type |
|---|---|
| `Console.write(x)`, `Console.writeLine(x)` | `x` an integer, float, `bool`, `StringView` or `String` |
| `Console.writeBytes(bytes)` | `bytes: Slice<u8>`, raw bytes to standard output |
| `Console.writeError(text)`, `Console.writeErrorLine(text)` | `text: StringView`, to standard error |
| `Console.read(mut buffer)` | `Result<usize, IoError>`; `0` at the end of input |

`Console` blocks its thread: in an `async fn`, use lib/std's `AsyncConsole` (N0295 otherwise).

```ceesafe
fn main() {
    Console.write("answer: ");
    Console.writeLine(42);
    let bytes = "raw\n";
    Console.writeBytes(bytes.bytes());
    Console.writeLine(2.5);
    Console.writeLine(true);
}
```

```text
answer: 42
raw
2.5
true
```

## `Process` and program arguments

| Function | Type |
|---|---|
| `Process.argumentCount()` | `usize`, including the program name |
| `Process.argument(i)` | `StringView?`: `null` out of range or for bytes that are not UTF-8 |
| `Process.exit(status)` | `status: i32`; never returns |
| `Process.run(program, arguments)` | `Result<i32, IoError>`; `arguments: borrow Vector<StringView>` |
| `Process.capture(program, arguments, input)` | `Result<ProcessOutput, IoError>` (lib/std) |

Environment variables are lib/std's `Environment.variable(name)`, a `StringView?`.

```ceesafe
fn main() {
    Console.writeLine(Process.argumentCount() >= 1);
    match (Process.argument(1000)) {
        Some(a) => { Console.writeLine(a); }
        null => { Console.writeLine("no argument 1000"); }
    }
}
```

```text
true
no argument 1000
```

## `File`, `TcpListener` and `TcpStream`

| Function | Type |
|---|---|
| `File.open(path)`, `File.create(path)` | `Result<File, IoError>` (reading; writing, created or truncated) |
| `File.remove(path)` | `Result<void, IoError>` |
| `f.read(mut buffer)` | `Result<usize, IoError>`, `0` at the end; `buffer: SliceMut<u8>` |
| `f.write(bytes)` | `Result<void, IoError>`: all of them or an error |
| `TcpListener.bind(address, port)` | `Result<TcpListener, IoError>`; then `port()`, `accept()` |
| `TcpStream.connect(address, port)` | `Result<TcpStream, IoError>`; `read`, `write`, `shutdownWrite`, `shutdownRead` |
| `e.message()` | the `IoError`'s text, a `StringView` |

A `File` closes when it is destroyed. These block their thread: async code uses lib/std's `AsyncFiles`,
`AsyncTcpListener` and `AsyncTcpStream`.

```ceesafe
fn main() {
    match (File.open("/no/such/file")) {
        Ok(_) => { Console.writeLine("opened"); }
        Err(e) => { Console.writeLine(e.message()); }
    }
}
```

```text
No such file or directory
```

## `StringView` and `String`

| Member | Type |
|---|---|
| `s.length` | `usize`, bytes |
| `s[i]`, `s[lo..hi]`, `s.bytes()` | `u8`; a `StringView` (character boundaries checked); `Slice<u8>` |
| `StringView.fromUtf8(bytes)` | `StringView?`: `null` for bytes that are not UTF-8 |
| `==`, `!=` | compare the bytes |
| `String.create()`, `String.withCapacity(n)` | `String`; `Result<String, AllocError>` |
| `t.append(view)`, `t.appendNumber(n)` | `Result<void, AllocError>` |
| `t.view()`, `t.clear()`, `t.length`, `t.capacity` | a `StringView` borrowed from `t`; … |

A `StringView` is a reference: a variable holding one is a `let`, and a struct or enum holding one needs a region
parameter. For formatting into a fixed buffer without allocating, use lib/core's `TextWriter` (`text`, `byte`,
`bytes`, `decimal` for a `u64`, `signed` for an `i64`, `hex`, then `written`).

```ceesafe
fn main() {
    let s: StringView = "CeeSafe";
    Console.writeLine(s.length);
    Console.writeLine(s[0..3]);
    var t = String.create();
    t.append("x = ")!;
    t.appendNumber(-7)!;
    Console.writeLine(t.view());
    let bytes = [104u8, 105];
    match (StringView.fromUtf8(bytes[..])) {
        Some(v) => { Console.writeLine(v); }
        null => { Console.writeLine("not UTF-8"); }
    }
}
```

```text
7
Cee
x = -7
hi
```

## `Vector`, arrays and slices

| Member | Type |
|---|---|
| `Vector<T>.create()`, `Vector<T>.withCapacity(n)` | `Vector<T>`; `Result<Vector<T>, AllocError>` |
| `v.push(x)` | `Result<void, AllocError>` |
| `v.pop()` | `T?` |
| `v.length`, `v.capacity`, `v[i]` | `usize`; an element place |
| `v.clear()`, `v.truncate(n)`, `v.swap(i, j)` | |
| `v.view()`, `v.viewMut()` | `Slice<T>`, `SliceMut<T>` |
| `[x; N]`, `[a, b, c]`, `a[lo..hi]`, `borrow mut a[lo..hi]` | an array; a `Slice<T>`; a `SliceMut<T>` |
| `d.copyFrom(s)`, `d.fill(v)` | bulk writes into a `SliceMut<T>` |

Indexes are `usize`. A range of two literals that the body indexes with is a `usize` range. Arrays and slices may
hold views (`[StringView; 3]`, `Slice<StringView>`), but not `borrow T` references. Slice a variable, not a call's
result: bind it to a local first.

```ceesafe
fn total(xs: Slice<u32>) -> u32 {
    var n: u32 = 0;
    for (x in xs) {
        n += x;
    }
    return n;
}

fn main() {
    var v = Vector<u32>.create();
    for (i in 0u32..4) {
        v.push(i * 10)!;
    }
    Console.writeLine(total(v.view()));
    var buffer = [0u32; 4];
    buffer[1..3].fill(5);
    Console.writeLine(total(buffer[..]));
    let names = ["Low", "Medium", "High"];
    Console.writeLine(names[1]);
    match (v.pop()) {
        Some(last) => { Console.writeLine(last); }
        null => {}
    }
}
```

```text
60
10
Medium
30
```

## Integers, floats and conversions

| Operation | Notes |
|---|---|
| `+ - * / %`, unary `-` | panic on overflow, and on dividing by zero |
| `& \| ^ << >>` | unsigned types only; no `~` (use `x ^ 0xFFFFFFFF`) |
| `x.wrappingAdd(y)`, `x.checkedAdd(y)`, `x.saturatingAdd(y)` | also `Sub`, `Mul`; `checked…` is a `T?` |
| `x.min(y)`, `x.max(y)` | every integer type |
| `x.abs()`, `x.unsignedAbs()` | signed types: `abs` panics at the minimum, `unsignedAbs` is the unsigned type of the width |
| `x as T` | only conversions that never lose information |
| `truncate<T>(x)` | wraps an integer; a float to an integer rounds toward zero and panics out of range or on NaN |
| `checkedTruncate<T>(f)`, `saturatingTruncate<T>(f)` | a float to an integer: `T?` (`null` where `truncate` would panic); the nearest bound (NaN gives 0) |
| `round<T>(x)` | to a float type, rounding to nearest |
| `f.sqrt()`, `f.abs()`, `f.floor()`, `f.ceil()`, `f.min(g)`, `f.max(g)`, `f.bits()` | floats |

```ceesafe
fn main() {
    let big: u8 = 250;
    Console.writeLine(big.wrappingAdd(10));
    Console.writeLine(big.saturatingAdd(10));
    let n: i32 = -7;
    Console.writeLine(n.abs());
    Console.writeLine(n.max(3));
    let wide: i64 = 300;
    Console.writeLine(truncate<u8>(wide));
    let f: f64 = 2.75;
    Console.writeLine(truncate<i32>(f));
    Console.writeLine(round<f32>(wide));
    Console.writeLine(f.floor());
}
```

```text
4
255
7
3
44
2
300
2
```

## `Box` and memory

| Type or function | Notes |
|---|---|
| `Box.create(value)` | `Result<Box<T>, AllocError>`, heap-owned; `Box<dyn I>` for dynamic dispatch |
| `FixedBufferAllocator.over(mut bytes)` | an allocator over a caller's buffer, for freestanding code |
| `Arena`, `Scratch`, `Pool<T>`, `SlotMap<T>`, `StableVector<T>` | see [memory](memory.md) and [collections](collections.md) |
| `Thread.spawn`, `Thread.both`, `Async.run`, `Async.scope` | see [threads](threads.md) and [async](async.md) |

```ceesafe
fn main() {
    let b = Box.create(41)!;
    Console.writeLine(b + 1);
}
```

```text
42
```

## Every signature above

This block is type-checked, not run: each call is annotated with the type the tables give, so a table that drifts
from the compiler fails the guide's checks.

<!-- ceesafe: check -->
```ceesafe
fn everything() -> Result<void, IoError> {
    let created: Result<File, IoError> = File.create("/tmp/x");
    var f = created?;
    let wrote: Result<void, IoError> = f.write("hi".bytes());
    var buffer = [0u8; 8];
    let got: Result<usize, IoError> = f.read(mut buffer[..]);
    let removed: Result<void, IoError> = File.remove("/tmp/x");
    let input: Result<usize, IoError> = Console.read(mut buffer[..]);
    Console.writeError("e");
    Console.writeErrorLine("e");
    var listener = TcpListener.bind("127.0.0.1", 0)?;
    let port: Result<u16, IoError> = listener.port();
    let accepted: Result<TcpStream, IoError> = listener.accept();
    var stream = TcpStream.connect("127.0.0.1", 1)?;
    let n: Result<usize, IoError> = stream.read(mut buffer[..]);
    let w: Result<void, IoError> = stream.write(buffer[..]);
    let sw: Result<void, IoError> = stream.shutdownWrite();
    let sr: Result<void, IoError> = stream.shutdownRead();
    var arguments = Vector<StringView>.create();
    let status: Result<i32, IoError> = Process.run("true", borrow arguments);
    let captured: Result<ProcessOutput, IoError> = Process.capture("true", borrow arguments, "".bytes());
    let home: StringView? = Environment.variable("HOME");
    return Ok();
}
fn collections() -> Result<void, AllocError> {
    var v: Vector<u32> = Vector<u32>.withCapacity(4)?;
    v.push(1)?;
    v.push(2)?;
    let c: usize = v.capacity;
    v.swap(0, 1);
    v.truncate(1);
    let m: SliceMut<u32> = v.viewMut();
    v.clear();
    var t: String = String.withCapacity(8)?;
    t.clear();
    let tc: usize = t.capacity;
    var dst = [0u8; 2];
    let src = [1u8, 2];
    dst[..].copyFrom(src[..]);
    return Ok();
}
fn numbers() {
    let a: u8 = 250;
    let s: u8 = a.wrappingSub(1);
    let m: u8? = a.checkedMul(2);
    let x: i64 = -5;
    let u: u64 = x.unsignedAbs();
    let f: f64 = 2.0;
    let ct: u8? = checkedTruncate<u8>(f);
    let st: u8 = saturatingTruncate<u8>(f);
    let r: f64 = f.sqrt().abs().ceil().min(1.0).max(0.5);
    let bits: u64 = f.bits();
}
fn memory() {
    var storage = [0u8; 64];
    let arena = FixedBufferAllocator.over(mut storage);
}
fn main() {}
```
