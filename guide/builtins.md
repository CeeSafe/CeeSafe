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
| `Console.writeBytes(bytes)` | `bytes: Slice<u8>`, raw bytes to standard output, UTF-8 or not |
| `Console.writeError(text)`, `Console.writeErrorLine(text)` | `text: StringView`, to standard error |
| `Console.read(mut buffer)` | `Result<usize, IoError>`; `0` at the end of input |
| `Console.readLine(mut line)` | `Result<bool, IoError>`: the next line of input in `line`, a `String`, without its `\n` or `\r\n`; `false` at the end of input |

`Console` blocks its thread: in an `async fn`, use lib/std's `AsyncConsole` (N0295 otherwise).

`Console.write` takes text, which is always UTF-8. Bytes from a file or the network may not be:
`StringView.fromUtf8(bytes)!` stops the program on them. To pass them through as they are, write them with
`Console.writeBytes(bytes)`.

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

`Console.readLine(mut line)` reads standard input a line at a time into one `String`, which it empties first. The
input's last line needs no line ending; a line that is not UTF-8 is `Err(InvalidInput)`. What the program wrote is
on the screen before it waits, so a prompt needs no `Console.flush()`. `Random.below(n)` (lib/std) is a random
integer from 0 to `n - 1`, each as likely as the others, as a `Result<u64, IoError>`; in the interpreter, the same
numbers every run.

This program picks a number from 1 to 100, then adds up the numbers it is given, one a line. The guide's checks run
it with no input, and its number may be any, so this is all they can show:

```ceesafe
fn main() {
    let secret = Random.below(100)! + 1;
    Console.writeLine(secret >= 1 && secret <= 100);
    var line = String.create();
    var lines = 0;
    var total: i64 = 0;
    Console.writeLine("numbers, one a line:");
    while (Console.readLine(mut line)!) {
        lines += 1;
        match (line.view().trim().parseInt<i64>()) {
            Some(n) => { total += n; }
            null => { Console.writeLine("not a number"); }
        }
    }
    Console.write(lines);
    Console.write(" lines, total ");
    Console.writeLine(total);
}
```

```text
true
numbers, one a line:
0 lines, total 0
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
| `File.openReadWrite(path)`, `File.openOrCreate(path)`, `File.createNew(path)` | `Result<File, IoError>` for reading and writing, never truncated: an existing file; created if missing; a new one (`AlreadyExists` otherwise) |
| `f.readAt(mut buffer, offset)`, `f.writeAt(bytes, offset)` | positional: `usize` (0 past the end); every byte or an error |
| `f.syncData()`, `f.syncAll()` | durable on disk (`F_FULLFSYNC` on Apple systems); after a failed sync the file's durable contents are unknown |
| `f.length()`, `f.setLength(n)` | `u64`; truncate or zero-extend |
| `File.rename(from, to)`, `Directory.sync(path)` | atomic replace; make a directory's entries durable |
| `Directory.space(path)` | the volume's `total`, `free` and `available` bytes (ADR 0077) |
| `Console.flush()` | write standard output's buffer now, so it survives a kill |
| `File.openAppend(path)` | write-only, created if missing, every write at the end |
| `File.modified(path)`, `File.setPermissions(path, 0o640)` | a `UnixTime`; `chmod` |
| `File.openRegular(path)` | read-only, only a regular file: never waits on a FIFO; `IsDirectory`, or `InvalidInput` for a device (ADR 0078) |
| `f.modified()`, `f.kind()` | the open file's time and `PathKind`, from the file, not its path |
| `IoError.StorageFull` | the volume is full (`ENOSPC`, `EDQUOT`) |
| `IoError.TooManyOpenFiles` | out of descriptors (`EMFILE`, `ENFILE`): a shortage, not a failed disk (ADR 0104) |
| `ProcessLimits.openFiles()`, `ProcessLimits.raiseOpenFiles(n)` | the open-file limit's `current` and `maximum`; raise the soft limit, once, at a server's start (ADR 0104) |
| `TcpListener.bind(address, port)` | `Result<TcpListener, IoError>`; then `port()`, `accept()` |
| `listener.shutdown()` | every `accept` on the listener, one another thread waits in and every later one, returns `Err(IoError.ConnectionAborted)`; the socket closes when the listener is destroyed ([Threads](threads.md), ADR 0126) |
| `ProcessSignals.subscribe()` | `Result<ProcessSignals, IoError>`; then `poll()`, `wait()` and `await waitAsync()` give Ctrl+C (`SIGINT`) and `SIGTERM` as `ProcessSignal.Interrupt` and `Terminate` instead of ending the process, until the value is dropped (ADR 0125) |
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
| `String.from(view)`, `t.clone()` | `Result<String, AllocError>`: a new string with a copy of the text |
| `t.append(view)`, `t.appendNumber(n)` | `Result<void, AllocError>` |
| `t.appendHex(n, minWidth, upper)`, `t.appendOctal(n)`, `t.appendFixed(f, digits)`, `t.appendPadded(view, width, align, fill)` | `Result<void, AllocError>`: Rust's `{:08x}`, `{:o}`, `{:.3}`, `{:>8}` (also on a `Vector<u8>`, and as `VectorBytes.appendHex(mut v, …)`) |
| `t.view()`, `t.clear()`, `t.length`, `t.capacity` | a `StringView` borrowed from `t`; … |
| `s.indexOf(t)`, `s.lastIndexOf(t)`, `s.indexOfByte(b)` | `usize?` |
| `s.contains(t)`, `s.startsWith(t)`, `s.endsWith(t)`, `s.equalsIgnoreAsciiCase(t)` | `bool` |
| `s.trim()`, `s.trimStart()`, `s.trimEnd()` | the `StringView` without ASCII whitespace |
| `s.splitOnce(sep)`, `s.split(sep)` | `TextHalves?` (`left`, `right`); a `TextSplit`, whose `next()` gives each piece, then `null` |
| `s.parseInt<T>()`, `s.parseIntRadix<T>(radix)`, `s.parseFloat<T>()` | `T?`: `null` for text that is not a `T` |

The searches and parsers work on a `String` too, and on bytes: a `Slice<u8>` (or a `Vector<u8>`) takes and gives
`Slice<u8>` where a view takes and gives `StringView`. Every view they return stays valid UTF-8. Floats parse
correctly rounded, and accept `inf`, `infinity` and `nan` in any case.

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
| `v.insert(at, x)`, `v.remove(at)`, `v.swapRemove(at)` | `Result<void, AllocError>`; `T`; `T` (the last element fills the gap); the index is checked at the call |
| `[]` | the empty `Slice<T>` or `SliceMut<T>`, where one is expected |
| `a.writeLE(0, x)` on an array | an array's methods are its slice's: `a[..].writeLE(0, x)` |
| `v.view()`, `v.viewMut()` | `Slice<T>`, `SliceMut<T>` |
| `[x; N]`, `[a, b, c]`, `a[lo..hi]`, `borrow mut a[lo..hi]` | an array; a `Slice<T>`; a `SliceMut<T>` |
| `d.copyFrom(s)`, `d.fill(v)` | bulk writes into a `SliceMut<T>` |
| `v.extend(s)`, `v.resize(n, fill)`, `v.reserve(extra)` | `Result<void, AllocError>`; one growth at most; `extend` and `resize` need Copy elements |
| `v.clone()` | `Result<Vector<T>, AllocError>`: a new vector with a copy of the elements, which must be Copy |
| `b.readBE<T>(at)`, `b.readLE<T>(at)` | `T`, an integer from bytes, big- or little-endian |
| `b.writeBE(at, x)`, `b.writeLE(at, x)` | into a `SliceMut<u8>` (or a mutable `Vector<u8>`) |
| `v.pushBE(x)`, `v.pushLE(x)` | `Result<void, AllocError>`: `x`'s bytes appended to a `Vector<u8>` |
| `Vector.intoRaw(v)`, `Vector.fromRaw(p, n, c)` | `VectorParts<T>` (`pointer`, `length`, `capacity`), nothing freed; a vector again, its type written or expected. `fromRaw` is unsafe |
| `v.setLength(n)` | unsafe: the length, over elements written through `v.viewMut().ptr`; panics above the capacity |
| `Vector.intoForeign(v)`, `Vector.fromForeign(p, n, c)` | unsafe: a buffer of Copy values a host adopts, or gives up ([Unsafe code and C](unsafe-and-c.md#buffers-that-change-sides)) |

Byte order works on fixed-width integers (`u8` to `u64`, `i8` to `i64`); the value's type gives the width, so a
literal needs a suffix (`v.pushBE(5u32)`). A read or write past the end panics at the call, and writes nothing.

```ceesafe
fn main() {
    var message = Vector<u8>.create();
    message.pushBE(0x51u8)!; // 'Q'
    message.pushBE(0u32)!; // the length, patched below
    message.extend("SELECT 1".bytes())!;
    message.pushBE(0u8)!;
    let length = truncate<u32>(message.length - 1);
    message.writeBE(1, length);
    Console.writeLine(message.readBE<u32>(1));
    var fields = "r=abc,s=xyz,i=4096".split(",");
    while (true) {
        let field = match (fields.next()) {
            Some(f) => f,
            null => { break; }
        };
        let pair = field.splitOnce("=")!;
        if (pair.left == "i") {
            Console.writeLine(pair.right.parseInt<u32>()! * 2);
        }
    }
    Console.writeLine(" 2.5e-3 ".trim().parseFloat<f64>()!);
}
```

```text
13
8192
0.0025
```

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
| `x.mulHigh(y)` | every integer type: the upper half of the full product (`wrappingMul` is the lower); never overflows |
| `x.min(y)`, `x.max(y)` | every integer type |
| `x.abs()`, `x.unsignedAbs()` | signed types: `abs` panics at the minimum, `unsignedAbs` is the unsigned type of the width |
| `x.rotateLeft(n)`, `x.rotateRight(n)`, `x.wrappingShl(n)`, `x.wrappingShr(n)` | unsigned types; `n: u32`, taken modulo the width |
| `x.leadingZeros()`, `x.trailingZeros()`, `x.countOnes()` | unsigned types; a `u32` (the width for 0) |
| `x.byteSwap()` | every integer type: the bytes in the other order |
| `x as T` | only conversions that never lose information; `a as u64 < b` compares |
| `*r` | the Copy value `r` refers to; `*r = v` writes through a `borrow mut` |
| `replace(mut place, v)`, `swap(mut a, mut b)` | the old value, which nothing is left without; exchange two places |
| `assert(cond)`, `assert(cond, message)`, `panic(message)` | panic when false; always (`never`) |
| `debugAssert(cond)`, `debugAssert(cond, message)` | as `assert`, but only in debug builds, tests and the interpreter; may change nothing that exists before it ([Debugging](debugging.md#debug-assertions)) |
| `embed("path")`, `embedText("path")` | a file's bytes as a `Slice<u8>`, or as UTF-8 text, read when the package is checked; the path is relative to the source file and stays inside the package ([Constants](basics.md#embedded-files)) |
| `r.isOk()`, `r.isErr()`, `r.ok()`, `r.err()`, `o.okOr(e)` | a `Result`'s tests and conversions; an optional as a `Result` |
| `enum E { @from Io(IoError), … }` | `?` turns an `IoError` into `E.Io(e)` in a function returning `Result<_, E>` |
| `@into fn name(this) -> F` (a method, one per type) | `?` turns an error of the method's type into an `F` by calling it, in a function returning `Result<_, F>`; lib/std's `SendError<T>.reason()` is one |
| `truncate<T>(x)` | wraps an integer; a float to an integer rounds toward zero and panics out of range or on NaN |
| `checkedTruncate<T>(x)`, `saturatingTruncate<T>(x)` | an integer or a float to an integer `T`: a `T?` that is `null` where `T` cannot hold the value (where `truncate` would wrap an integer or panic on a float); the nearest bound of `T` (NaN gives 0) |
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
    Console.writeLine(checkedTruncate<u8>(wide).isNull());
    Console.writeLine(saturatingTruncate<u8>(wide));
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
true
255
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
| `OnceCell<T>`, `OnceLock<T>` | a value written once and then lent as `borrow T`: `getOrInit(init)`, `tryInit<E>(init)`, `get()`, `set(v)`; `OnceLock` is the one threads share ([threads](threads.md#built-once-oncelock)) |

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
    var line = String.create();
    let more: Result<bool, IoError> = Console.readLine(mut line);
    let below: Result<u64, IoError> = Random.below(100);
    Console.writeError("e");
    Console.writeErrorLine("e");
    var listener = TcpListener.bind("127.0.0.1", 0)?;
    let port: Result<u16, IoError> = listener.port();
    let accepted: Result<TcpStream, IoError> = listener.accept();
    let shut: Result<void, IoError> = listener.shutdown();
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
    var bytes = Vector<u8>.create();
    bytes.extend(src[..])?;
    bytes.resize(8, 0)?;
    bytes.reserve(8)?;
    bytes.pushBE(1u16)?;
    bytes.pushLE(-1i32)?;
    let word: u32 = bytes.readBE<u32>(0);
    let back: i16 = bytes.view().readLE<i16>(2);
    bytes.writeLE(0, 7u64);
    dst[..].writeBE(0, 3u16);
    return Ok();
}
fn text() {
    let s: StringView = " a,b ";
    let at: usize? = s.indexOf(",");
    let last: usize? = s.lastIndexOf("b");
    let byte: usize? = s.indexOfByte(',');
    let has: bool = s.contains("a") && s.startsWith(" ") && s.endsWith(" ") && s.equalsIgnoreAsciiCase(" A,B ");
    let t: StringView = s.trim().trimStart().trimEnd();
    let halves: TextHalves? = t.splitOnce(",");
    var pieces: TextSplit = t.split(",");
    let piece: StringView? = pieces.next();
    let n: i64? = "-12".parseInt<i64>();
    let h: u8? = "ff".parseIntRadix<u8>(16);
    let f: f32? = "1.5".parseFloat<f32>();
    let raw: Slice<u8> = s.bytes();
    let rawAt: usize? = raw.indexOf(",".bytes());
    let rawTrim: Slice<u8> = raw.trim();
    let rawHalves: ByteHalves? = raw.splitOnce(",".bytes());
    var rawPieces: ByteSplit = raw.split(",".bytes());
    let rawN: u32? = raw.parseInt<u32>();
}
fn numbers() {
    let a: u8 = 250;
    let s: u8 = a.wrappingSub(1);
    let m: u8? = a.checkedMul(2);
    let high: u64 = (0xFFFF_FFFF_FFFF_FFFFu64).mulHigh(2);
    let x: i64 = -5;
    let u: u64 = x.unsignedAbs();
    let f: f64 = 2.0;
    let ct: u8? = checkedTruncate<u8>(f);
    let st: u8 = saturatingTruncate<u8>(f);
    let ci: i8? = checkedTruncate<i8>(x);
    let si: u16 = saturatingTruncate<u16>(x);
    let r: f64 = f.sqrt().abs().ceil().min(1.0).max(0.5);
    let bits: u64 = f.bits();
    let w: u32 = 0x80000001;
    let rotated: u32 = w.rotateLeft(1).rotateRight(2).wrappingShl(33).wrappingShr(1);
    let counts: u32 = w.leadingZeros() + w.trailingZeros() + w.countOnes();
    let swapped: i16 = (-2i16).byteSwap();
}
fn memory() {
    var storage = [0u8; 64];
    let arena = FixedBufferAllocator.over(mut storage);
}
fn main() {}
```
