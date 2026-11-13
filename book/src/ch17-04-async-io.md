---
title: Async I/O
summary: The waits the library gives tasks - Timer, AsyncConsole in place of Console, AsyncFiles, TCP sockets with an echo server and its client in one program, and channels between tasks - each returning a Result with an AsyncError, and why blocking calls are refused with N0295.
assumes: [ch17-01-tasks-and-await, ch17-02-scopes, ch09-02-result]
keywords: [Timer, "Timer.sleep", "Timer.now", AsyncConsole, AsyncFiles, AsyncTcpListener, AsyncTcpStream, AsyncError, IoError, Channel, sendAsync, recvAsync, loopback, echo, blocking]
diagnostics: [N0261, N0295]
specification: ["type-system.md §40.4", "type-system.md §40.1"]
---

# Async I/O

**This section teaches** the operations a task waits on: time, the console, files, TCP sockets and channels. It
assumes async functions ([Tasks and await](./ch17-01-tasks-and-await)), scopes ([Scopes](./ch17-02-scopes)) and
error enums with `@from` ([Recoverable errors with `Result`](./ch09-02-result)).

A task must never **block**: keep its thread while it waits. A blocked thread runs no other task, so one blocking
read would stop every task of the program. The library therefore has a second set of I/O operations for tasks.
Each is an async function: it starts the operation, lets other tasks run until the operating system says the
operation can go on, and returns a `Result`.

| What | Operations |
|---|---|
| Time | `await Timer.sleep(ms)`; `Timer.now()`, milliseconds on a clock that only goes forward (not async) |
| Console | `await AsyncConsole.write(text)`, `writeLine(text)`, `writeError(text)` |
| Files | `await AsyncFiles.open(path)`, `create(path)`, `read(mut file, buffer)`, `write(mut file, bytes)` |
| TCP | `AsyncTcpListener.bind(address, port)` (not async), `await listener.accept()`; `await AsyncTcpStream.connect(address, port)`, `await stream.read(buffer)`, `await stream.write(bytes)` |
| Tasks to tasks | `Channel<T>`: `await ch.sendAsync(value)`, `await ch.recvAsync()` |

The library has more of each kind, and UDP sockets (`AsyncUdpSocket`) and name lookup (`AsyncDns.resolve`)
besides: the reference is https://ceesafe.github.io/CeeSafe/guide/async.

::: tip Rule
The async operations return `Result<T, AsyncError>`. `AsyncError` is `Cancelled`, `TimedOut`, `Io(IoError)` or
`Closed`: the task was cancelled, a shield's time ran out, the operating system refused, or the channel was closed.

*Specification: `type-system.md` §40.4.*
:::

## Time and the console

<span class="listing-caption"><strong>Listing 17-24</strong>, <code>main.cee</code>: Three short sleeps, a dot after each, and the time they took.</span>

```ceesafe
async fn report() -> Result<void, AsyncError> {
    let start = Timer.now();
    await AsyncConsole.write("waiting")?;
    for (i in 0..3) {
        await Timer.sleep(10)?;
        await AsyncConsole.write(".")?;
    }
    await AsyncConsole.writeLine(" done")?;
    if (Timer.now() - start >= 30) {
        await AsyncConsole.writeLine("at least 30 ms passed")?;
    }
    return Ok();
}

fn main() {
    match (Async.run(report())) {
        Ok() => {}
        Err(_) => { Console.writeLine("failed"); }
    }
}
```

```text
waiting... done
at least 30 ms passed
```

`AsyncConsole` writes text only. To write a number, build the line in a `String` first
([Strings](./ch08-02-strings)) and write its view. `Console` is still right in `main` and in any function that is
not async.

## Files

<span class="listing-caption"><strong>Listing 17-25</strong>, <code>main.cee</code>: A task writes a file and reads it back.</span>

```ceesafe
async fn roundTrip(path: StringView) -> Result<usize, AsyncError> {
    var out = await AsyncFiles.create(path)?;
    await AsyncFiles.write(mut out, "written by a task\n".bytes())?;
    var back = await AsyncFiles.open(path)?;
    var buffer = [0u8; 64];
    var total: usize = 0;
    loop {
        let n = await AsyncFiles.read(mut back, mut buffer[..])?;
        if (n == 0) {
            return Ok(total);
        }
        total += n;
    }
}

fn main() {
    let path = "notes.tmp";
    match (Async.run(roundTrip(path))) {
        Ok(n) => {
            Console.write(n);
            Console.writeLine(" bytes read back");
        }
        Err(AsyncError.Io(e)) => { Console.writeLine(e.message()); }
        Err(_) => { Console.writeLine("failed"); }
    }
    let _ = File.remove(path);
}
```

```text
18 bytes read back
```

The file is the same `File` that chapter 12 reads with ([Reading a file](./ch12-02-reading-a-file)). `AsyncFiles`
does each operation on another thread while the task waits. A read returns how many bytes it put in the buffer,
and 0 at the end of the file.

## Sockets: an echo server and its client

A TCP server **binds** a listener to an address and a port, and **accepts** connections from it. Each connection
is a stream of bytes in both directions. The program below is both sides: a server that sends back whatever it
receives, and the one client it serves, as two children of a scope. It uses the loopback address `127.0.0.1`, which
never leaves the machine, and port 0, which asks the system for any free port.

<span class="listing-caption"><strong>Listing 17-26</strong>, <code>main.cee</code>: An echo server and a client on one thread, over the loopback address.</span>

```ceesafe
enum EchoError {
    @from Io(IoError),
    @from Async(AsyncError),
}

async fn serve(listener: borrow AsyncTcpListener) -> Result<void, EchoError> {
    let client = await listener.accept()?;
    var buffer = [0u8; 64];
    loop {
        let n = await client.read(mut buffer[..])?;
        if (n == 0) {
            break;
        }
        await client.write(buffer[0..n])?;
    }
    client.shutdownWrite()?;
    return Ok();
}

async fn ask(port: u16, reply: borrow mut String) -> Result<void, EchoError> {
    let stream = await AsyncTcpStream.connect("127.0.0.1", port)?;
    await stream.write("hello, ".bytes())?;
    await stream.write("echo".bytes())?;
    stream.shutdownWrite()?;
    var buffer = [0u8; 64];
    loop {
        let n = await stream.read(mut buffer[..])?;
        if (n == 0) {
            return Ok();
        }
        match (StringView.fromUtf8(buffer[0..n])) {
            Some(text) => { reply.append(text)!; }
            null => {}
        }
    }
}

async fn run() -> Result<String, EchoError> {
    let listener = AsyncTcpListener.bind("127.0.0.1", 0)?;
    let port = listener.port()?;
    var reply = String.create();
    await Async.scope(EchoError.Async(AsyncError.Cancelled), (s) => {
        s.spawn(async () => await serve(borrow listener));
        s.spawn(async () => await ask(port, mut reply));
    })?;
    return Ok(reply);
}

fn main() {
    match (Async.run(run())) {
        Ok(reply) => { Console.writeLine(reply.view()); }
        Err(EchoError.Io(e)) => { Console.writeLine(e.message()); }
        Err(EchoError.Async(_)) => { Console.writeLine("cancelled or timed out"); }
    }
}
```

```text
hello, echo
```

Read it from `run`:

- `bind` makes the listener and `listener.port()` asks which port the system chose. Neither waits, so neither is
  async, and their error is an `IoError`. The listener exists before either child starts, so the client cannot
  connect too early.
- `serve` waits in `accept` for a connection. It then reads until `read` returns 0, which means the other side
  has finished sending, and writes each piece back. `write` sends all the bytes it is given.
- `ask` connects, writes two pieces, and calls `shutdownWrite()` to say it has finished sending. That is what
  ends the server's loop. It then reads the reply until the server finishes too.
- The two children take turns at their awaits: while the client waits for the echo, the server runs, and the
  other way round. Neither blocks the one thread they share.
- The streams and the listener close their sockets when they are destroyed, at the end of `serve`, `ask` and `run`.

The output does not depend on how the bytes are cut into pieces on the way. The client may get the reply in one
read or in several, and it appends them in order. A real server would accept in a loop and start a task for each
connection through a task set ([Scopes](./ch17-02-scopes#task-sets-children-started-later)).

The error enum has two `@from` variants because the program meets two error types: `IoError` from the calls that
do not wait, and `AsyncError` from those that do. With them, every `?` in the listing converts by itself. In a
function whose error type is `AsyncError`, `?` on an `IoError` converts too, to `AsyncError.Io`.

## Channels between tasks

A `Channel<T>` ([Channels](./ch16-02-channels) teaches it for threads) also carries values from task to task. Tasks use
`sendAsync` and `recvAsync`, which wait without blocking.

<span class="listing-caption"><strong>Listing 17-27</strong>, <code>main.cee</code>: One task sends three numbers through a channel with room for one. The other adds them up.</span>

```ceesafe
async fn produce(ch: borrow Channel<u32>) -> Result<void, AsyncError> {
    for (n in 1..4u32) {
        await ch.sendAsync(n)?;
    }
    ch.close();
    return Ok();
}

async fn consume(ch: borrow Channel<u32>, total: borrow mut u32) -> Result<void, AsyncError> {
    loop {
        match (await ch.recvAsync()?) {
            Some(n) => { total += n; }
            null => { return Ok(); }
        }
    }
}

async fn run(ch: borrow Channel<u32>) -> Result<u32, AsyncError> {
    var total: u32 = 0;
    await Async.all(async () => await produce(ch), async () => await consume(ch, mut total))?;
    return Ok(total);
}

fn main() {
    let ch = Channel<u32>.create(1)!;
    match (Async.run(run(borrow ch))) {
        Ok(total) => { Console.writeLine(total); }
        Err(_) => { Console.writeLine("failed"); }
    }
}
```

```text
6
```

`sendAsync` waits while the channel is full, so a fast producer cannot run ahead of a slow consumer. Its error is
a `SendError<T>`, which holds the value that was not sent: `SendError.Closed(value)` when the channel was closed,
`SendError.Cancelled(value)` when the task was cancelled. Match on it to get the value back. `?` drops the value
and returns `Err(AsyncError.Closed)` or `Err(AsyncError.Cancelled)`: `SendError<T>` has a method, `reason()`,
marked `@into`, and `?` converts an error through such a method of the error's own type as it does through an
`@from` variant of the function's error enum. `recvAsync` gives `Some(value)`, or `null` once the channel is
closed and empty.

## Mistakes

::: warning Mistake: `Console` in async code
`Console.writeLine` may block: a terminal or a pipe that is full makes it wait. Async code may not call it.
:::

<span class="listing-caption"><strong>Listing 17-28</strong>, <code>main.cee</code>: An async function writes with <code>Console</code>. This does not compile.</span>

<!-- ceesafe: fail N0295 -->
```ceesafe
async fn greet() -> Result<void, AsyncError> {
    Console.writeLine("hello");
    return Ok();
}

fn main() {
    match (Async.run(greet())) {
        Ok() => {}
        Err(_) => { Console.writeLine("failed"); }
    }
}
```

```text
main.cee:2:5: error[N0295]: 'greet' is an async fn, which must not block its thread (type-system.md §40.1), but this may sleep: it does console I/O
  |
2 |     Console.writeLine("hello");
  |     ^^^^^^^^^^^^^^^^^^^^^^^^^^^
```

Write `await AsyncConsole.writeLine("hello")?;`. The compiler knows of each function whether it may block, through
every call it makes, so the same error stops a `File` read, a `Mutex` lock or `Async.run` inside async code,
however deep in ordinary functions the blocking call is.
[Tasks and threads together](./ch17-05-tasks-and-threads) shows what to do with a call that must block.

::: tip Rule
Async code cannot make a call that may block its thread.

*Specification: `type-system.md` §40.1.*
:::

::: warning Mistake: `?` on an `IoError` with an error enum that converts only `AsyncError`
The calls that do not wait (`bind`, `port`, `shutdownWrite`) return `IoError`, not `AsyncError`. `?` converts an
error in one step, to a variant that holds exactly its type.
:::

<span class="listing-caption"><strong>Listing 17-29</strong>, <code>main.cee</code>: <code>bind</code>'s error is an <code>IoError</code>, and <code>ServeError</code> has no variant for one. This does not compile.</span>

<!-- ceesafe: fail N0261 -->
```ceesafe
enum ServeError {
    @from Async(AsyncError),
}

async fn listen() -> Result<AsyncTcpListener, ServeError> {
    let listener = AsyncTcpListener.bind("127.0.0.1", 0)?;
    await AsyncConsole.writeLine("bound")?;
    return Ok(listener);
}

fn main() {
    match (Async.run(listen())) {
        Ok(_) => { Console.writeLine("listening"); }
        Err(_) => { Console.writeLine("failed"); }
    }
}
```

```text
main.cee:6:20: error[N0261]: '?' would return 'Err' of type 'IoError', but 'listen' returns errors of type 'ServeError'; mark a variant of the function's error enum '@from' to convert, as in '@from Variant(IoError)'
  |
6 |     let listener = AsyncTcpListener.bind("127.0.0.1", 0)?;
  |                    ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
```

`AsyncError` has a variant for an `IoError`, but `?` does not go through it to reach `ServeError`. Add
`@from Io(IoError)` to the enum, as Listing 17-26 does. A function that returns `AsyncError` itself needs nothing:
`AsyncError.Io` is an `@from` variant, so there the same `?` compiles and returns `Err(AsyncError.Io(e))`.

::: details Coming from Rust
There is no `AsyncRead` trait to import and no runtime crate to choose: the async I/O types are in the standard
library and work with the one executor. A buffer is lent to `read` for the length of the `await`, as a borrow.
:::

::: details Coming from JavaScript, C# or Python
There, blocking calls in async code compile and stall the program at run time. Here the compiler refuses them
(N0295), so a program that compiles has no hidden blocking call on its tasks' thread.
:::

## Summary

- A task must not block its thread. Blocking calls in async code are `N0295`.
- Tasks wait with `Timer.sleep`, and write with `AsyncConsole`, not `Console`.
- `AsyncFiles` opens, reads and writes files; `AsyncTcpListener` and `AsyncTcpStream` are TCP; `Channel<T>` with
  `sendAsync` and `recvAsync` carries values between tasks.
- Each returns `Result<T, AsyncError>`: `Cancelled`, `TimedOut`, `Io(IoError)` or `Closed`. `sendAsync` returns a
  `SendError<T>` with the unsent value, which `?` turns into `Closed` or `Cancelled`. The calls that do not wait
  return `IoError`. `?` converts one to `AsyncError.Io`, or to an `@from` variant of the program's own error enum,
  which needs one for each of the two types (`N0261` otherwise).
- A `read` that returns 0 means the end: of the file, or of what the other side sends.

Next: [Tasks and threads together](./ch17-05-tasks-and-threads).
