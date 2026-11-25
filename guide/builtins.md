# Built-in types and functions

The types and functions below are part of the compiler, not of a library, so they have no page in the library
reference. The libraries' own types, such as `TextWriter`, `Timer`, `Environment`, `HashMap` and the async
sockets, are in [lib/core](../spec/api/core.md) and [lib/std](../spec/api/std.md); one of lib/std's, its
compression codecs, is summarised here too. Every example here is compiled and run by the guide's checks, so the
names and signatures are the compiler's. With the MCP server, `ceesafe_api` looks any of these names up.

## `Console`

| Function | Type |
|---|---|
| `Console.write(x)`, `Console.writeLine(x)` | `x` an integer, float, `bool`, `StringView` or `String` |
| `Console.writeBytes(bytes)` | `bytes: Slice<u8>`, raw bytes to standard output, UTF-8 or not |
| `Console.writeError(text)`, `Console.writeErrorLine(text)` | `text: StringView`, to standard error |
| `Console.read(mut buffer)` | `Result<usize, IoError>`; `0` at the end of input |
| `Console.readLine(mut line)` | `Result<bool, IoError>`: the next line of input in `line`, a `String`, without its `\n` or `\r\n`; `false` at the end of input |
| `Console.isTerminal(ConsoleStream.Input)` | `bool`: whether a person types at the stream (`Input`, `Output` or `Error`); `false` for a pipe or a file |

A shell prompts only when its input is a terminal, and reads a script silently otherwise:

```ceesafe
fn main() {
    if (Console.isTerminal(ConsoleStream.Input)) {
        Console.write("> ");
    }
    Console.writeLine(Console.isTerminal(ConsoleStream.Output) || true);
}
```

```text
true
```

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
| `File.createReadWrite(path)` | `Result<File, IoError>` for reading and writing, created if missing, emptied if present (ADR 0221) |
| `IoError.BadDescriptor` | "Bad file descriptor" (`EBADF`): reading a file opened only for writing (`File.create`), or writing one opened only for reading |
| `f.lock(FileLock.Exclusive, start, length)`, `f.tryLock(…)`, `f.unlock(start, length)` | advisory locks on bytes of the file, which other programs (SQLite among them) see; `tryLock` is `Result<bool, IoError>`, `false` when another holds a conflicting lock (ADR 0220) |
| `f.testLock(kind, start, length)` | `Result<FileLock?, IoError>`: null if the lock could be taken now, else the kind of another holder's conflicting lock; takes nothing on POSIX (`F_GETLK`), tries and releases at once on Windows (ADR 0234) |
| `f.mapShared(offset, length)` | `Result<SharedMapping, IoError>`: a region of the file mapped shared with every other mapping of it, in any process (SQLite's `-shm`); read and written by copies (`readAt`, `writeAt`) and atomics (`load32`, `store32`, `compareExchange32`, `fetchAdd32` and the `16` and `64` forms, with lib/core's orderings), with `SharedMapping.fence()`; the file is extended to cover it (ADR 0232, ADR 0235) |
| `f.mapSharedReadOnly(offset, length)` | `Result<ReadOnlyMapping, IoError>`: the same, for reading only, from a `File` open for reading (`File.open` too); `readAt`, `load16`, `load32`, and no method that writes; the file must already hold the range (ADR 0235) |
| `ProcessLimits.openFiles()`, `ProcessLimits.raiseOpenFiles(n)` | the open-file limit's `current` and `maximum`; raise the soft limit, once, at a server's start (ADR 0104) |
| `TcpListener.bind(address, port)` | `Result<TcpListener, IoError>`; then `port()`, `accept()` |
| `listener.shutdown()` | every `accept` on the listener, one another thread waits in and every later one, returns `Err(IoError.ConnectionAborted)`; the socket closes when the listener is destroyed ([Threads](threads.md), ADR 0126) |
| `ProcessSignals.subscribe()` | `Result<ProcessSignals, IoError>`; then `poll()`, `wait()` and `await waitAsync()` give Ctrl+C (`SIGINT`), `SIGTERM` and `SIGHUP` as `ProcessSignal.Interrupt`, `Terminate` and `Hangup` instead of ending the process, until the value is dropped (ADRs 0125 and 0140) |
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

A lock is advisory: it keeps out only programs that ask for one too. Lock bytes past the data (SQLite locks bytes
from 0x40000000), take a range the `File` does not already hold, and unlock exactly the ranges taken: that is the
same on every system. Dropping the `File` releases its locks. `testLock` asks whether a lock could be taken
without taking it (SQLite's "is a writer here?"), and names the kind of lock in the way.

```ceesafe
fn main() {
    let dir = Directory.temporary()!;
    var path = String.create();
    path.append(dir.view())!;
    path.append("/data")!;
    {
        let f = File.createReadWrite(path.view())!;
        let mine = f.tryLock(FileLock.Exclusive, 0x40000000, 1)!;
        Console.writeLine(mine);
        f.unlock(0x40000000, 1)!;
        Console.writeLine(f.testLock(FileLock.Exclusive, 0x40000001, 1)!.isNull());
    }
    File.remove(path.view())!;
    Directory.remove(dir.view())!;
}
```

```text
true
true
```

A shared mapping is memory that other processes may write at any moment, so it is never a slice: bytes are copied
out and in, and integers are atomics, which are atomic between processes too. An access outside the region, or an
atomic not aligned to its size in the file, panics. To reach more of the file, map another region; regions of the
same bytes are one memory. A file the program may only read is mapped with `mapSharedReadOnly`, whose
`ReadOnlyMapping` has copies out and loads but nothing that writes.

```ceesafe
fn main() {
    let dir = Directory.temporary()!;
    var path = String.create();
    path.append(dir.view())!;
    path.append("/index")!;
    {
        let f = File.createReadWrite(path.view())!;
        let a = f.mapShared(0, 4096)!;
        let b = f.mapShared(0, 64)!;
        a.writeAt(8, "header".bytes());
        let _ = a.compareExchange32(0, 0, 41, Ordering.AcquireRelease);
        let _ = b.fetchAdd32(0, 1, Ordering.AcquireRelease);
        SharedMapping.fence();
        var copy = [0u8; 6];
        b.readAt(8, mut copy[..]);
        Console.writeLine(StringView.fromUtf8(copy[..])!);
        Console.writeLine(a.load32(0, LoadOrdering.Acquire));
        Console.writeLine(f.length()!);
    }
    File.remove(path.view())!;
    Directory.remove(dir.view())!;
}
```

```text
header
42
4096
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
| `t.intoBytes()`, `String.fromUtf8(v)` | `Vector<u8>`; `Result<String, Vector<u8>>`: the buffer moves, no copy; `Err(v)`, unchanged, for bytes that are not UTF-8 |
| `t.append(view)`, `t.appendNumber(n)` | `Result<void, AllocError>` |
| `t.add()`, then `.text(view)`, `.char(c)`, `.number(n)`, `.signed(n)`, `.hex(n, minWidth, upper)`, `.padded(view, width, align, fill)`, `.fixed(f, digits)`, `.done()` | an appender whose steps chain; `done()` is `Result<void, AllocError>`, an `Err` if any step could not allocate (ADR 0217; also on a `Vector<u8>`) |
| `t.appendHex(n, minWidth, upper)`, `t.appendOctal(n)`, `t.appendFixed(f, digits)`, `t.appendPadded(view, width, align, fill)` | `Result<void, AllocError>`: Rust's `{:08x}`, `{:o}`, `{:.3}`, `{:>8}` (also on a `Vector<u8>`, and as `VectorBytes.appendHex(mut v, …)`) |
| `t.appendChar(c)`, `t.appendUtf8(bytes)` | `Result<bool, AllocError>`: a character (`c: u32`) in UTF-8, or bytes that are UTF-8 (checked once, not copied first); `false`, appending nothing, otherwise (also on a `Vector<u8>`) |
| `t.view()`, `t.clear()`, `t.length`, `t.capacity` | a `StringView` borrowed from `t`; … |
| `s.indexOf(t)`, `s.lastIndexOf(t)`, `s.indexOfByte(b)` | `usize?` |
| `s.contains(t)`, `s.startsWith(t)`, `s.endsWith(t)`, `s.equalsIgnoreAsciiCase(t)` | `bool` |
| `s.trim()`, `s.trimStart()`, `s.trimEnd()` | the `StringView` without ASCII whitespace |
| `s.splitOnce(sep)`, `s.split(sep)` | `TextHalves?` (`left`, `right`); a `TextSplit`, whose `next()` gives each piece, then `null` |
| `s.parseInt<T>()`, `s.parseIntRadix<T>(radix)`, `s.parseFloat<T>()` | `T?`: `null` for text that is not a `T` |

The searches and parsers work on a `String` too, and on bytes: a `Slice<u8>` (or a `Vector<u8>`) takes and gives
`Slice<u8>` where a view takes and gives `StringView`; a string literal there is its bytes, `path.startsWith("/api")`. Every view they return stays valid UTF-8. Floats parse
correctly rounded, and accept `inf`, `infinity` and `nan` in any case.

A `StringView` is a borrow: a variable holding one is a `let`, and a struct or enum holding one needs a region
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

### Text that may not be UTF-8: `ByteString` and `ByteStringView`

Text from C, a file name, or a database's TEXT column may hold bytes that are not UTF-8, which a `String` cannot.
lib/core's `ByteString` (owned) and `ByteStringView` (borrowed, a region parameter in a struct as a `StringView`)
keep any bytes as they are, and convert to UTF-8 when asked (ADR 0231, type-system.md §22.4):

| Member | Type |
|---|---|
| `ByteStringView.fromText(text)`, `ByteStringView.fromBytes(bytes)` | a view of the bytes, no copy |
| `ByteString.create()`, `ByteString.fromBytes(v)`, `b.intoBytes()` | a byte string; the `Vector<u8>` taken or given back, no copy |
| `ByteString.fromString(t)`, `b.intoString()` | a byte string, the `String`'s bytes taken; `Result<String, ByteString>`, the byte string back when it is not UTF-8; no copy |
| `ByteString.fromText(text)`, `ByteString.copyOf(bytes)`, `b.clone()`, `v.toByteString()` | `Result<ByteString, AllocError>`, a copy |
| `b.view()`, `b.bytes()`, `length()`, `isEmpty()`, `byteAt(i)`, `slice(lo, hi)`, `sliceFrom(lo)`, `sliceTo(hi)` | a `ByteStringView`; `Slice<u8>`; … |
| `toUtf8()`, `isUtf8()` | `StringView?`, a view when the bytes are UTF-8; `bool` |
| `toUtf8Lossy()`, `v.appendLossyTo(mut s)` | a `String` with U+FFFD for each maximal subpart of a bad sequence, as browsers and Python decode |
| `v.chunks()` | the `Utf8Chunk`s: each a `valid` `StringView` then an `invalid` `Slice<u8>`, for printing with escapes |
| `==`, `v.equals(w)`, `v.compare(w)` | the bytes compared; bytewise order. A `ByteString` is a `HashMap` and `OrderedMap` key |
| `v.indexOf(t)`, `startsWith`, `endsWith`, `contains`, `trim`, `splitOnce`, `split`, `chars`, … | `TextView`'s (below): any text as the needle, a literal included |
| `b.append(text)`, `appendBytes(bytes)`, `appendByte(b)`, `appendNumber(n)`, `appendUnsigned(n)`, `appendHex`, `appendChar`, … | `Result<…, AllocError>`, as a `String`'s |

`Console.write` prints text only; write a byte string's bytes with `Console.writeBytes(b.bytes())`. Text built in a
`String` becomes a byte string with `ByteString.fromString(t)`, which takes its buffer, and a byte string that is
UTF-8 becomes a `String` with `b.intoString()` (ADR 0236).

```ceesafe
fn main() {
    var b = ByteString.create();
    b.append("id=")!;
    b.appendNumber(7)!;
    b.appendByte(0xFF)!;
    Console.writeLine(b.length());
    Console.writeLine(b.toUtf8().isNull());
    Console.writeLine(b.toUtf8Lossy()!.view());
    for (chunk in b.view().chunks()) {
        Console.write(chunk.valid);
        Console.write(" then ");
        Console.writeLine(chunk.invalid.length);
    }
    Console.writeLine(b.view().startsWith("id=7"));
    var t = String.create();
    t.appendFixed(0.5, 2)!;
    let c = ByteString.fromString(t); // no copy
    match (c.intoString()) {
        Ok(text) => { Console.writeLine(text.view()); }
        Err(_) => { Console.writeLine("not UTF-8"); }
    }
}
```

```text
5
true
id=7�
id=7 then 1
true
0.50
```

### Code for either kind of text: `TextView` and `TextBuffer`

lib/core's interface `TextView` is implemented by `StringView`, `ByteStringView` and `Slice<u8>`, and `TextBuffer` by
`String` and `ByteString` (ADR 0253, type-system.md §22.5). A function bounded by one is written once and compiled
for each kind of text it is given, with direct calls, so a `StringView` loses nothing. Needles are any text: a
literal, either view, or bytes; an owned text is passed as its `view()`.

| Member of `TextView` | Type |
|---|---|
| `bytes()`, `length()`, `isEmpty()`, `byteAt(i)` | the bytes; counts and indexes are in bytes |
| `slice(lo, hi)`, `sliceFrom(lo)`, `sliceTo(hi)`, `trim()`, `trimStart()`, `trimEnd()` | the same kind of text |
| `equals(t)`, `compare(t)`, `equalsIgnoreAsciiCase(t)`, `compareIgnoreAsciiCase(t)` | `bool`, `i32`; ASCII case only |
| `indexOf(t)`, `lastIndexOf(t)`, `indexOfByte(b)`, `indexOfIgnoreAsciiCase(t)` | `usize?` |
| `contains(t)`, `startsWith(t)`, `endsWith(t)`, `startsWithIgnoreAsciiCase(t)`, `endsWithIgnoreAsciiCase(t)` | `bool` |
| `splitOnce(t)`, `split(t)` | `TextPair` (`left`, `right`) or null; an iterator of pieces |
| `chars()` | an iterator of `TextChar` (`value`, `offset`, `length`, `valid`): U+FFFD for each maximal subpart of a bad sequence |
| `toUtf8()`, `isUtf8()`, `isCharBoundary(i)` | `StringView?`; `bool`; `bool` |

On a `StringView` a match must be whole characters, so a needle that is not UTF-8 is never found inside one, and
every piece stays UTF-8. A `TextBuffer` has `append(t)`, `bytes()` and `clear()`; a `String` replaces what is not
UTF-8 with U+FFFD as it appends. A `StringView` keeps its built-in methods (`s.indexOf(t)` takes a `StringView`);
`TextView.indexOf(s, t)` and a bound reach the interface's.

```ceesafe
fn keyword<T: TextView>(word: T) -> bool => word.trim().equalsIgnoreAsciiCase("select");

fn quoted<B: TextBuffer, T: TextView>(out: borrow mut B, name: T) -> Result<void, AllocError> {
    out.append("\"")?;
    out.append(name)?;
    return out.append("\"");
}

fn main() {
    let raw = [0x53u8, 0x65, 0x6C, 0x65, 0x63, 0x74];
    Console.writeLine(keyword(" SELECT "));
    Console.writeLine(keyword(ByteStringView.fromBytes(raw[..])));
    let bad = [0x61u8, 0xFF];
    var s = String.create();
    quoted(mut s, ByteStringView.fromBytes(bad[..]))!;
    Console.writeLine(s);
    for (c in ByteStringView.fromBytes(bad[..]).chars()) {
        Console.write(c.value);
        Console.writeLine(if (c.valid) "" else " (not UTF-8)");
    }
}
```

```text
true
true
"a�"
97
65533 (not UTF-8)
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
| `v.drain()` | an iterator that moves the elements out first to last, `for (x in v.drain())`; `v` is borrowed until it ends, then empty with its capacity |
| `s.sortBy(cmp)`, `s.sortStableBy(cmp)` | on a `SliceMut<T>` or a vector: `cmp(a, b)` is negative, zero or positive; `sortStableBy` keeps equal elements in their order |
| `[]` | the empty `Slice<T>` or `SliceMut<T>`, where one is expected |
| `a.writeLE(0, x)` on an array | an array's methods are its slice's: `a[..].writeLE(0, x)` |
| `v.view()`, `v.viewMut()` | `Slice<T>`, `SliceMut<T>` |
| `[x; N]`, `[a, b, c]`, `a[lo..hi]`, `borrow mut a[lo..hi]` | an array; a `Slice<T>`; a `SliceMut<T>` |
| `d.copyFrom(s)`, `d.fill(v)` | bulk writes into a `SliceMut<T>` |
| `d.copyWithin(from, to, count)` | C's `memmove` inside one `SliceMut<T>` (or a vector, or a mutable array): the ranges may overlap |
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
hold views (`[StringView; 3]`, `Slice<StringView>`), but not `borrow T` borrows. Slice a variable, not a call's
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
| `& \| ^ << >>`, unary `~` | unsigned types only |
| `x.wrappingAdd(y)`, `x.checkedAdd(y)`, `x.saturatingAdd(y)` | also `Sub`, `Mul`; `checked…` is a `T?` |
| `x.mulHigh(y)` | every integer type: the upper half of the full product (`wrappingMul` is the lower); never overflows |
| `x.min(y)`, `x.max(y)` | every integer type |
| `x.abs()`, `x.unsignedAbs()` | signed types: `abs` panics at the minimum, `unsignedAbs` is the unsigned type of the width |
| `x.rotateLeft(n)`, `x.rotateRight(n)` | unsigned types; `n: u32`, taken modulo the width |
| `x.wrappingShl(n)`, `x.wrappingShr(n)` | every integer type; `n: u32`, taken modulo the width; on a signed type `wrappingShr` is arithmetic (`(-7i32).wrappingShr(1)` is `-4`) |
| `x.leadingZeros()`, `x.trailingZeros()`, `x.countOnes()` | unsigned types; a `u32` (the width for 0) |
| `x.byteSwap()` | every integer type: the bytes in the other order |
| `x as T` | only conversions that never lose information; `a as u64 < b` compares |
| `*r` | the Copy value `r` refers to; `*r = v` writes through a `borrow mut` |
| `replace(mut place, v)`, `swap(mut a, mut b)` | the old value, which nothing is left without; exchange two places |
| `arrayFrom(f)` | an array `[T; N]` whose element `i` is `f(i)`, called for `i` from 0 to `N - 1`; `T` and `N` come from the expected type or `arrayFrom<T, N>(f)` ([Arrays](collections.md#arrays-of-atomics-and-other-move-types)) |
| `assert(cond)`, `assert(cond, message)`, `panic(message)` | panic when false; always (`never`) |
| `debugAssert(cond)`, `debugAssert(cond, message)` | as `assert`, but only in debug builds, tests and the interpreter; may change nothing that exists before it ([Debugging](debugging.md#debug-assertions)) |
| `embed("path")`, `embedText("path")` | a file's bytes as a `Slice<u8>`, or as UTF-8 text, read when the package is checked; the path is relative to the source file and stays inside the package ([Constants](basics.md#embedded-files)) |
| `r.isOk()`, `r.isErr()`, `r.ok()`, `r.err()`, `o.okOr(e)` | a `Result`'s tests and conversions; an optional as a `Result` |
| `r.map(f)`, `r.mapErr(f)` | a `Result` with `f` applied to its `Ok` value, or to its `Err` value; the new type is `Send` (no `StringView`) |
| `enum E { @from Io(IoError), … }` | `?` turns an `IoError` into `E.Io(e)` in a function returning `Result<_, E>` |
| `@into fn name(this) -> F` (a method, one per type) | `?` turns an error of the method's type into an `F` by calling it, in a function returning `Result<_, F>`; lib/std's `SendError<T>.reason()` is one |
| `truncate<T>(x)` | wraps an integer; a float to an integer rounds toward zero and panics out of range or on NaN |
| `checkedTruncate<T>(x)`, `saturatingTruncate<T>(x)` | an integer or a float to an integer `T`: a `T?` that is `null` where `T` cannot hold the value (where `truncate` would wrap an integer or panic on a float); the nearest bound of `T` (NaN gives 0) |
| `round<T>(x)` | to a float type, rounding to nearest |
| `f.sqrt()`, `f.abs()`, `f.floor()`, `f.ceil()`, `f.min(g)`, `f.max(g)`, `f.bits()` | floats |
| `f.trunc()`, `f.round()`, `f.roundEven()`, `f.copySign(g)` | floats: toward zero, half away from zero, half to even; `f`'s magnitude with `g`'s sign |
| `f.exp()`, `f.exp2()`, `f.expM1()`, `f.ln()`, `f.log2()`, `f.log10()`, `f.ln1p()`, `f.pow(g)`, `f.sin()`, `f.cos()`, `f.tan()`, `f.asin()`, `f.acos()`, `f.atan()`, `f.atan2(g)`, `f.sinh()`, `f.cosh()`, `f.tanh()`, `f.asinh()`, `f.acosh()`, `f.atanh()`, `f.cbrt()`, `f.hypot(g)`, `f.fma(g, h)`, `f.remainder(g)`, `f.truncatingRemainder(g)`, `f.scaleB(n)`, `f.logB()` | floats: the C library's functions, hosted only ([Math methods](closures.md#math-methods)) |

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
| `SecretHeap` | the allocator for keys: its blocks are wiped before they are freed, its growth copies and wipes, its containers wipe the slots they give up (see [memory](memory.md#secrets)) |
| `Secret.wipe(mut place)` | sets a zeroable place, or `mut bytes[a..b]`, to zero bytes in a way the C compiler keeps: for keys (see [memory](memory.md#secrets)) |
| `FixedBufferAllocator.over(mut bytes)` | an allocator over a caller's buffer, for freestanding code |
| `Arena`, `Scratch`, `Pool<T>`, `SlotMap<T>`, `StableVector<T>` | see [memory](memory.md) and [collections](collections.md) |
| `Thread.cpuCount()` | `u32`, at least 1: the CPUs the process may run on (on Linux its affinity mask) |
| `Thread.spawn`, `Thread.both`, `Async.run`, `Async.scope` | see [threads](threads.md) and [async](async.md) |
| `OnceLock<T>.constant()`, `Mutex<T>.constant(v)`, `Vector<T>.empty()` | static initialisers: an empty cell, a mutex whose lock is made at its first use, an empty vector ([In a static](threads.md#in-a-static)) |
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

## Compression: `Deflate`, `Zlib`, `Gzip`, `Snappy` and `Zstd`

lib/std's deflate (RFC 1951), and its zlib (RFC 1950) and gzip (RFC 1952) framings, and snappy's raw block format,
both ways, and Zstandard (RFC 8878) both ways, in pure CeeSafe ([ADR 0196](../spec/decisions/0196-compression-codecs.md); the [lib/std page](../spec/api/std.md) has every
signature):

| Function or type | Type |
|---|---|
| `Deflate.decompress(input, limit, mut out)`, and `Zlib.…`, `Gzip.…` | `Result<void, DecodeError>`: all of `input` onto the end of `out`, at most `limit` bytes; on an error `out` is as it was |
| `Deflate.compress(input, level, mut out)`, and `Zlib.…`, `Gzip.…` | `Result<void, AllocError>`; `level` 0 (stored) to 9, 6 the usual default |
| `DecodeError` | `Malformed` (not a stream, cut short, or bytes after it), `TooLarge` (past the limit, or a zstd window past the window limit), `Unsupported` (a zlib preset dictionary, a zstd dictionary), `ChecksumMismatch`, `OutOfMemory(AllocError)` |
| `Inflater.create(format, limit)`, `i.step(input, mut out)` | a decoder that streams into the caller's `SliceMut<u8>`: `Result<Progress, DecodeError>`, with `read`, `written` and `done`; `format` is a `DeflateFormat` (`Raw`, `Zlib`, `Gzip`) |
| `i.setWindow(history)` | a preset dictionary, before the first step |
| `Deflater.create(format, level)`, `d.update(input, mut out)`, `d.flush(mut out)`, `d.finish(mut out)` | an encoder that appends to a `Vector<u8>`; `flush` is a sync flush (all so far decodes) |
| `Adler32` | zlib's checksum, with `create`, `update`, `finish` and `of`, as `Crc32` (gzip's) |
| `Snappy.decompress(input, limit, mut out)`, `Snappy.compress(input, level, mut out)` | snappy's raw block (Parquet's `SNAPPY` pages; Avro's `snappy` blocks, which add the big-endian `Crc32` of the bytes), as `Deflate`'s calls; snappy has one level, so `level` is ignored |
| `Snappy.length(input)` | `usize?`: the length a block's preamble states, read alone, or `null`; `decompress` refuses one above its limit before allocating |
| `Zstd.decompress(input, limit, mut out)` | Zstandard frames (Parquet's `ZSTD` pages, PyIceberg's default; Avro's `zstandard` blocks), as `Deflate`'s call; skippable frames are skipped, a stated content size above `limit` is refused before allocating |
| `Zstd.compress(input, level, mut out)` | `Result<void, AllocError>`: one frame that states its size, without a checksum; libzstd's levels 1 to 9 in meaning and parameters, 0 its default (3), above 9 taken as 9 |
| `ZstdDecoder.create(limit, windowLimit)`, `z.step(input, mut out)` | a zstd decoder that streams, as `Inflater`; it allocates a window of `windowLimit` bytes at `create` (libzstd's default limit is 2^27) and refuses a frame whose window is larger |
| `ZstdEncoder.create(level, checksum)`, `e.update(input, mut out)`, `e.flush(mut out)`, `e.finish(mut out)` | a zstd encoder that streams, as `Deflater`: `flush` writes what it holds as blocks (all so far decodes), `finish` ends the frame, with its XXH64 if `checksum`; its memory is the level's tables and up to twice its window (2 MiB at level 3) |
| `Xxh64` | XXH64 with a seed, zstd's checksum, with `create(seed)`, `update`, `finish` and `of(bytes, seed)` |

Every decoder takes a limit, so a decompression bomb costs at most `limit` bytes, and no input panics one: every
refusal is a `DecodeError`. `Inflater.step` and `ZstdDecoder.step` allocate nothing after `create` and has no effect (`@effects()`), so
async code may call it, but its CPU time is in proportion to the slices it is given: async code decodes some 64 KiB
at a time, or hands a whole buffer to `Async.blocking`. The encoders allocate (`@effects(alloc)`) and take time in
proportion to their input, so async code compresses through `Async.blocking` too. Compression leaks content through the output's length (CRIME,
BREACH): do not compress a secret together with bytes an attacker chooses.

```ceesafe
fn main() {
    let text = "to be, or not to be, that is the question: to be, or not to be".bytes();
    var packed = Vector<u8>.create();
    Gzip.compress(text, 6, mut packed)!;
    var back = Vector<u8>.create();
    match (Gzip.decompress(packed.view(), 1 << 20, mut back)) {
        Ok() => { Console.writeLine(StringView.fromUtf8(back.view()) ?? ""); }
        Err(_) => { Console.writeLine("refused"); }
    }
    var inflater = Inflater.create(DeflateFormat.Gzip, 1 << 20)!;
    var buffer = [0u8; 16];
    var at: usize = 0;
    var total: usize = 0;
    while (true) {
        let rest = packed[at..];
        let p = inflater.step(rest, mut buffer[..])!;
        at += p.read;
        total += p.written;
        if (p.done) { break; }
    }
    Console.writeLine(total);
    let cut = packed[0..packed.length - 1];
    match (Gzip.decompress(cut, 1 << 20, mut back)) {
        Ok() => { Console.writeLine("decoded"); }
        Err(e) => {
            match (e) {
                DecodeError.Malformed => { Console.writeLine("cut short"); }
                _ => { Console.writeLine("refused"); }
            }
        }
    }
    Console.writeLine(Adler32.of("123456789".bytes()));
    var block = Vector<u8>.create();
    Snappy.compress(text, 0, mut block)!;
    Console.writeLine(Snappy.length(block.view()) ?? 0);
    match (Snappy.decompress(block.view(), 10, mut back)) {
        Ok() => { Console.writeLine("decoded"); }
        Err(e) => {
            match (e) {
                DecodeError.TooLarge => { Console.writeLine("more than 10 bytes"); }
                _ => { Console.writeLine("refused"); }
            }
        }
    }
    // A frame written by the `zstd` tool (level 19, with a checksum).
    let frame = [
        0x28u8, 0xB5, 0x2F, 0xFD, 0x24, 0x3E, 0x55, 0x01, 0x00, 0x42, 0xC2, 0x07, 0x0C, 0xC0, 0x3D, 0x24, 0x87, 0x60,
        0x8A, 0xBF, 0x1A, 0x65, 0xAE, 0xE5, 0x04, 0x8F, 0x43, 0xE3, 0xA5, 0xCE, 0x53, 0xF1, 0xD7, 0xCE, 0x52, 0x7C,
        0xC6, 0xBB, 0xE2, 0x81, 0xE6, 0xC8, 0x01, 0x02, 0x00, 0x9C, 0x4A, 0x4D, 0x64, 0x53, 0x09, 0x42, 0xFF, 0xBC,
        0x60,
    ];
    var unpacked = Vector<u8>.create();
    Zstd.decompress(frame[..], 1 << 20, mut unpacked)!;
    Console.writeLine(StringView.fromUtf8(unpacked.view()) ?? "");
    var decoder = ZstdDecoder.create(1 << 20, 1 << 20)!;
    let p = decoder.step(frame[..], mut buffer[..])!;
    Console.writeLine(p.written);
    Console.writeLine(Xxh64.of("123456789".bytes(), 0));
    var z = Vector<u8>.create();
    Zstd.compress(text, 3, mut z)!;
    var zback = Vector<u8>.create();
    Zstd.decompress(z.view(), 1 << 20, mut zback)!;
    Console.writeLine(zback.view() == text);
    var encoder = ZstdEncoder.create(1, true)!;
    var streamed = Vector<u8>.create();
    encoder.update(text[0..20], mut streamed)!;
    encoder.flush(mut streamed)!;
    encoder.update(text[20..], mut streamed)!;
    encoder.finish(mut streamed)!;
    var sback = Vector<u8>.create();
    Zstd.decompress(streamed.view(), 1 << 20, mut sback)!;
    Console.writeLine(sback.view() == text);
}
```

```text
to be, or not to be, that is the question: to be, or not to be
62
cut short
152961502
62
more than 10 bytes
to be, or not to be, that is the question: to be, or not to be
16
10139926970967174787
true
true
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
    v.sortStableBy((a, b) => if (a < b) -1 else if (a > b) 1 else 0);
    for (moved in v.drain()) {
        let kept: u32 = moved;
    }
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
    bytes.copyWithin(0, 2, 2);
    var text = String.create();
    let added: bool = text.appendChar(0x263A)?;
    let valid: bool = text.appendUtf8(src[..])?;
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
    let halved: i64 = x.wrappingShr(1);
    let doubled: i64 = x.wrappingShl(1);
    let counts: u32 = w.leadingZeros() + w.trailingZeros() + w.countOnes();
    let swapped: i16 = (-2i16).byteSwap();
}
fn memory() {
    var storage = [0u8; 64];
    let arena = FixedBufferAllocator.over(mut storage);
}
fn main() {}
```
