# Async tasks

A program that waits for many things at once (timers, sockets, files, other tasks) can run them as **tasks** on one
thread instead of one thread each. An `async fn` is a function whose call is a task; `await` runs a task and gives its
result; while it waits, other tasks of the same thread run. CeeSafe's tasks are **structured**: every task a program
starts has returned before `Async.run` returns, so a task may borrow its caller's locals, just as a function call may.

## Async functions and `await`

```ceesafe
async fn add(a: i32, b: i32) -> i32 => a + b;

async fn checked(x: i32) -> Result<i32, StringView> {
    let y = await add(x, x);
    if (y > 100) {
        return Err("too big");
    }
    return Ok(y);
}

async fn total(values: Slice<i32>) -> i32 {
    var sum = 0;
    for (v in values) {
        sum = await add(sum, v);
    }
    return sum;
}

fn main() {
    let values = [1, 2, 3];
    Console.writeLine(Async.run(total(values[..])));
    match (Async.run(checked(20))) {
        Ok(v) => { Console.writeLine(v); }
        Err(e) => { Console.writeLine(e); }
    }
    match (Async.run(checked(60))) {
        Ok(v) => { Console.writeLine(v); }
        Err(e) => { Console.writeLine(e); }
    }
}
```

```text
6
40
too big
```

`Async.run(task)` runs a task on this thread until it returns, from ordinary (sync) code. Inside an async fn, `await`
does the same, and `?` works on an awaited `Result` as on any other. A call of an async fn does nothing by itself: it
must be awaited, run or spawned. There is no `Task` or `Future` value to store or return, so a task never outlives the
code that started it:

<!-- ceesafe: fail N0342 -->
```ceesafe
async fn add(a: i32, b: i32) -> i32 => a + b;

fn main() {
    let pending = add(1, 2);
}
```

```text
main.cee:4:19: error[N0342]: a call of the async fn 'add' is a task: await it, or pass it to 'Async.run' (type-system.md §40.1)
  |
4 |     let pending = add(1, 2);
  |                   ^^^^^^^^^
```

`await` is allowed only in an async fn or an async closure (N0341). An async fn that would await itself is N0343, since
its frame would contain itself; `Async.boxed` gives such a call a frame of its own from an allocator.

## Running tasks at once: scopes

`Async.scope` starts **children** that run while the scope waits, and returns once every child has. A child is an
async closure, and it may borrow the awaiting function's locals, because the scope cannot return before it does:

```ceesafe
enum Failure {
    OutOfMemory,
    Bad,
}

async fn pause(ms: u64) -> Result<void, Failure> {
    match (await Timer.sleep(ms)) {
        Ok() => { return Ok(); }
        Err(_) => { return Err(Failure.Bad); }
    }
}

async fn say(text: StringView) -> Result<void, Failure> {
    match (await AsyncConsole.writeLine(text)) {
        Ok() => { return Ok(); }
        Err(_) => { return Err(Failure.Bad); }
    }
}

async fn gather() -> Result<u32, Failure> {
    var slow: u32 = 0;
    var fast: u32 = 0;
    await Async.scope(Failure.OutOfMemory, (s) => {
        s.spawn(async () => {
            await pause(30)?;
            await say("slow done")?;
            slow = 100;
            return Ok();
        });
        s.spawn(async () => {
            await pause(10)?;
            await say("fast done")?;
            fast = 20;
            return Ok();
        });
    })?;
    return Ok(slow + fast); // both children have returned
}

fn main() {
    match (Async.run(gather())) {
        Ok(v) => { Console.writeLine(v); }
        Err(_) => { Console.writeLine("failed"); }
    }
}
```

```text
fast done
slow done
120
```

The two children wait at the same time, so the whole takes about 30 ms, not 40. They write with `AsyncConsole`:
`Console.writeLine` blocks its thread, which async code may not do (N0295, below). The rules that keep this safe are the
borrow checker's:

- two children cannot both capture a local that either changes (N0241), and the scope's body cannot use a local a
  child has captured until the scope returns;
- a child cannot borrow a local of the scope's body, which ends before the child may (N0344); a loop that spawns a
  child per item copies what each child reads, or moves it in with `async move () => …`;
- every child returns `Result<void, E>` with one `E`: the first `Err` cancels the other children and is the scope's
  result, after all of them have returned. `oom`, the scope's first argument, is the error if a child's frame cannot
  be allocated.

`Async.scopeIn(alloc, oom, body)` takes the children's frames from an allocator of your choice; `Async.scope` uses the
system heap.

## Racing and joining two tasks

`Async.race(a, b)` runs two async closures and keeps the first to finish; `Async.all(a, b)` runs both and fails with
the first error. Neither allocates: their children's frames are in the caller's.

```ceesafe
async fn after(ms: u64, v: u32) -> Result<u32, AsyncError> {
    await Timer.sleep(ms)?;
    return Ok(v);
}

async fn fastest() -> Result<u32, AsyncError> {
    let r: Result<Raced<u32>, AsyncError> = await Async.race(
        async () => await after(30, 1),
        async () => await after(5, 2),
    );
    let raced = r?;
    return Ok(raced.winner * 10 + raced.value); // the second child (winner 1) returned 2
}

fn main() {
    match (Async.run(fastest())) {
        Ok(v) => { Console.writeLine(v); }
        Err(_) => { Console.writeLine("failed"); }
    }
}
```

```text
12
```

The loser is cancelled and awaited before `race` returns; if it also returned `Ok`, `raced.other` holds its value.

## Cancellation

A task is cancelled when a sibling fails, when it loses a race, or when the task that owns its scope is cancelled.
Cancelling only sets a flag: the task's next waiting operation (a sleep, a read, a channel receive) returns
`Err(AsyncError.Cancelled)` at once, and the task returns through its own `?`s, dropping its locals as any function
does. Code between two `await`s is never interrupted. A long computation can look with `Async.isCancelled()` or
`Async.checkpoint(error)`.

```ceesafe
async fn work() -> Result<u32, AsyncError> {
    var saw: u32 = 0;
    let outcome = await Async.scope(AsyncError.Cancelled, (s) => {
        s.spawn(async () => {
            await Timer.sleep(5)?;
            return Err(AsyncError.TimedOut); // fails first, so its sibling is cancelled
        });
        s.spawn(async () => {
            match (await Timer.sleep(1000)) {
                Ok() => { saw = 1; }
                Err(AsyncError.Cancelled) => { saw = 2; }
                Err(_) => { saw = 3; }
            }
            return Ok();
        });
    });
    match (outcome) {
        Err(AsyncError.TimedOut) => { return Ok(saw); }
        _ => { return Ok(0); }
    }
}

fn main() {
    match (Async.run(work())) {
        Ok(v) => { Console.writeLine(v); }
        Err(_) => { Console.writeLine("failed"); }
    }
}
```

```text
2
```

`Async.shield(ms, body)` runs cleanup that must finish: inside it the flag is ignored, for at most `ms` milliseconds.

## Deadlines

`Async.timeout(ms, timedOut, body)` runs `body` with a deadline. If the body has not returned after `ms`
milliseconds, it is cancelled and awaited, and the result is `Err(timedOut)`, an error of the body's own type.
`Async.deadline(at, timedOut, body)` is the same with a time on `Timer.now()`'s clock. Neither allocates.

```ceesafe
enum FetchError {
    TimedOut,
    Async(AsyncError),
}

async fn after(ms: u64, v: u32) -> Result<u32, FetchError> {
    match (await Timer.sleep(ms)) {
        Ok() => { return Ok(v); }
        Err(e) => { return Err(FetchError.Async(e)); }
    }
}

async fn fetch() -> Result<u32, FetchError> {
    let quick = await Async.timeout(50, FetchError.TimedOut, async () => await after(5, 1))?;
    match (await Async.timeout(5, FetchError.TimedOut, async () => await after(50, 2))) {
        Ok(v) => { return Ok(quick * 10 + v); }
        Err(FetchError.TimedOut) => { return Ok(quick * 10 + 9); } // the slow one ran out of time
        Err(e) => { return Err(e); }
    }
}

fn main() {
    match (Async.run(fetch())) {
        Ok(v) => { Console.writeLine(v); }
        Err(_) => { Console.writeLine("failed"); }
    }
}
```

```text
19
```

A body that returned `Ok` before it saw the cancellation keeps its value. If the caller itself is cancelled, the
result is the body's own error, never `timedOut`: a timeout reports only its own deadline.

## Waiting for the outside world

The library's waiting operations are async functions, in the hosted profile:

| What | Operations |
|---|---|
| Time | `Timer.sleep(ms)`; `Timer.now()` (not async) |
| Console | `AsyncConsole.write(text)`, `writeLine(text)`, `writeError(text)` |
| Files | `AsyncFiles.open`, `create`, `read`, `write` (each on a worker thread while the task waits) |
| Sockets | `AsyncTcpListener.bind` or `bindWith(address, options)` (not async), `accept`; `AsyncTcpStream.connect` or `connectTo(address)`, `read`, `write`, `writeVectored(parts)` and `writeAllVectored(parts)` (an `IoParts`, up to eight byte parts sent in one call); `sendFile(file, offset, length)` and `sendFileAll(file, offset, length)`, a file's bytes sent by the kernel where it can (`sendfile`, `TransmitFile`), the file's position untouched; `AsyncUdpSocket.bind(address, options)`, `sendTo`, `recvFrom`, and `sendMsg`/`recvMsg` with the local address, ECN bits, truncation and segments; `AsyncDns.resolve(host, port)`, a name's addresses, cancellable; the options (`setNoDelay`, `setKeepAlive`, buffers, `setLinger`) and `peerAddress`/`localAddress`, not async |
| Tasks to tasks | `Channel<T>`: `sendAsync`, `recvAsync`, `close` (`send` and `recv` block, for threads); `AsyncMutex<T>`: `lock`, whose guard may be held across `await` |
| Blocking calls | `Async.blocking(() => …)`: any sync code on a worker thread while the task waits |
| Signals | `AsyncSignal`: one task waits, any thread or interrupt handler raises |

Each returns `Result<T, AsyncError>`, whose cases are `Cancelled`, `TimedOut` and `Io(IoError)`. A socket's address is
a `SocketAddress`, `V4` or `V6`, made from text by `SocketAddresses.parse("[::1]:8080")` or
`SocketAddresses.parseIp("::1", 8080)`, and written back with `a.write(mut w)`. A listener on `SocketAddresses.v6Any(port)`
also accepts IPv4 clients, whose `peerAddress()` is `V4`. Network failures have their own `IoError` kinds,
`ConnectionReset`, `TimedOut`, `HostUnreachable` and the rest, the same on every platform. A blocking call made
directly in async code would stop every task of the thread, so it is a compile error (N0295): a `File` read or a
`Mutex` lock belongs in `Async.blocking`.

A static file goes to a socket with `await stream.sendFileAll(borrow file, offset, length)`: the kernel copies it from
the page cache (`sendfile`, or `TransmitFile` on Windows), and where it cannot, lib/std reads 64 KiB at a time on a
worker thread and writes them, with the same result. The offset is explicit and the file's position stays where it was,
so one open file can be sent on many connections at once. If the file is shorter than `offset + length`, the result
is `Io(InvalidInput)`. `sendFile` is one step and returns how many bytes went.

## How it runs

Each async fn has a **frame** holding its locals and what it is waiting for. Its size is known at compile time
(`ceesafec build --explain-frames` prints it), and it never moves, which is why a reference to a local may be held
across an `await`. An awaited call's frame is inside its caller's, so `await` itself allocates nothing; children of a
scope get theirs from the scope's allocator.

The executor waits with kqueue on macOS and the BSDs, epoll on Linux (io_uring when asked for), and the console and
file APIs on Windows. `Async.run` uses one thread; `Async.runWorkers(n, task)` and `s.spawnSend(…)` spread a scope's
children over `n` threads ([Threads](threads.md#async-tasks-on-several-threads)). In the interpreter tasks run in a
fixed order with a virtual clock, so the examples above print the same every time, and quickly.

Async code also runs in CosmoOS user programs (without async sockets) and in kernel code, over an executor the kernel
provides ([Kernel code](kernel.md#async-tasks-in-kernel-code)). The debugger lists a program's tasks and where each
waits ([Debugging](debugging.md)). The rules in full are the specification's §40
([type-system.md](../spec/type-system.md)) and ADRs 0031 to 0040.

## Next

[Unsafe code and C](unsafe-and-c.md).
