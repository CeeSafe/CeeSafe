---
title: Async tasks
summary: The fourth stage of the web server - the same server on one thread, with AsyncTcpListener, a task for each connection in a task set that bounds how many run, a time limit for each client, and a test that runs server and clients as tasks.
assumes: [ch21-03-thread-pool, ch17-01-tasks-and-await, ch17-02-scopes, ch17-03-timeouts-races-cancellation, ch17-04-async-io]
keywords: [async, AsyncTcpListener, AsyncTcpStream, task set, "Async.tasks", "set.spawn", async move, "Async.timeout", "Async.race", AsyncError, blocking, N0295]
diagnostics: [N0295, N0209]
specification: ["type-system.md §40.5", "type-system.md §40.1"]
---

# Async tasks

**This section teaches** how to serve many connections on one thread, with a task for each. It rewrites the edges
of the server of [section 21.3](./ch21-03-thread-pool) and again leaves `src/http.cee` alone. It assumes async
functions ([Tasks and await](./ch17-01-tasks-and-await)), task sets ([Scopes](./ch17-02-scopes)), timeouts and
races ([Timeouts, races and cancellation](./ch17-03-timeouts-races-cancellation)) and the async sockets
([Async I/O](./ch17-04-async-io)).

A worker thread that waits in `read` is a whole thread doing nothing. A task that waits costs only its frame, a
piece of memory with its locals, and the thread goes on with other tasks. A server spends most of its time waiting
for clients, so tasks fit it well.

## The edges, awaited

The async sockets have the names of the blocking ones with `Async` in front, and their waits are awaited.

<span class="listing-caption"><strong>Listing 21-22</strong>, <code>src/main.cee</code>: <code>readHead</code> and <code>handle</code> as async functions.</span>

<!-- ceesafe: skip -->
```ceesafe
/// Reads from `stream` until the empty line that ends a request's head, the end of the stream, or the limit.
async fn readHead(stream: borrow AsyncTcpStream) -> Result<Vector<u8>, ServeError> {
    var head = Vector<u8>.create();
    var buffer = [0u8; 512];
    while (head.length < HEAD_LIMIT) {
        let n = await stream.read(mut buffer[..])?;
        if (n == 0) {
            break;
        }
        head.extend(buffer[0..n])?;
        if (head.view().contains("\r\n\r\n".bytes())) {
            break;
        }
    }
    return Ok(head);
}

/// Answers one connection: reads the request, chooses the response and sends it.
async fn handle(stream: borrow AsyncTcpStream) -> Result<void, ServeError> {
    let head = await readHead(stream)?;
    let response = match (StringView.fromUtf8(head.view())) {
        Some(text) => answer(text),
        null => badRequest(),
    };
    let text = render(borrow response)?;
    await stream.write(text.view().bytes())?;
    stream.shutdownWrite()?;
    return Ok();
}
```

Compare with section 21.2. Three things changed: `async fn`, the type `AsyncTcpStream`, and `await` before `read`,
`write` and the call of `readHead`. The stream is a plain `borrow`, not `borrow mut`: the async stream's methods
take a shared borrow. `answer`, `badRequest` and `render` are the same functions as before. They do not wait, so
they are not async, and a task calls them as any code does.

The waits return an `AsyncError`, and `shutdownWrite`, which does not wait, an `IoError`. The error type has an
`@from` variant for each, and two plain variants that the next listings use:

```text
enum ServeError {
    NoMemory,
    TooSlow,
    @from Io(IoError),
    @from Async(AsyncError),
    @from Memory(AllocError),
}
```

## A task for each connection

<span class="listing-caption"><strong>Listing 21-23</strong>, <code>src/main.cee</code>: The task of one connection.</span>

<!-- ceesafe: skip -->
```ceesafe
/// The task of one connection. It owns the stream and gives the client a limited time. A connection's own
/// failure stays here. Only a cancellation is passed on.
async fn connection(stream: AsyncTcpStream) -> Result<void, ServeError> {
    let result = await Async.timeout(PATIENCE, ServeError.TooSlow, async () => await handle(borrow stream));
    match (result) {
        Ok() => { return Ok(); }
        Err(ServeError.Async(AsyncError.Cancelled)) => { return Err(ServeError.Async(AsyncError.Cancelled)); }
        Err(_) => {
            await AsyncConsole.writeError("a connection failed\n")?;
            return Ok();
        }
    }
}
```

- `connection` takes the stream by value. The task owns its connection, and closes it when it returns.
- `Async.timeout` gives the client `PATIENCE` milliseconds, a constant set to 5000, for the whole exchange. A
  client that connects and says nothing is cut off after five seconds, and its task ends. The pool of section
  21.3 had no such limit, and a blocking `read` cannot be given one from outside.
- A client's failure is reported and becomes `Ok()`: it is not the server's failure. A cancellation is different.
  It means that the server is stopping, and a task must pass it on
  ([what a cancelled task sees](./ch17-03-timeouts-races-cancellation#what-a-cancelled-task-sees)).
- A task writes with `AsyncConsole`, not `Console`.

<span class="listing-caption"><strong>Listing 21-24</strong>, <code>src/main.cee</code>: The accepting loop starts a task in a set for each connection.</span>

<!-- ceesafe: skip -->
```ceesafe
/// Accepts connections and starts a task in `set` for each. It ends only by an error.
async fn acceptLoop(listener: borrow AsyncTcpListener, set: borrow TaskSet<ServeError>) -> Result<void, ServeError> {
    loop {
        let stream = await listener.accept()?;
        await set.spawn(async move () => await connection(stream))?;
    }
}

/// Serves the listener's connections, at most `CONNECTIONS` at a time.
async fn serve(listener: borrow AsyncTcpListener) -> Result<void, ServeError> {
    return await Async.tasks(CONNECTIONS, ServeError.NoMemory, async (set) => await acceptLoop(listener, set));
}

fn main() {
    let listener = AsyncTcpListener.bind("127.0.0.1", 7878)!;
    Console.writeLine("listening on http://127.0.0.1:7878");
    Console.flush();
    match (Async.run(serve(borrow listener))) {
        Ok() => {}
        Err(_) => { Console.writeErrorLine("the server failed"); }
    }
}
```

The task set does the work of the pool and its channel:

| The pool of section 21.3 | The task set |
|---|---|
| `Workers.start(THREADS)`: four threads | `Async.tasks(CONNECTIONS, …)`: up to 64 tasks, on one thread |
| `workers.take(stream)` waits while the channel is full | `await set.spawn(…)` waits while 64 tasks run |
| the connection moves through the channel | the connection moves into the `async move` closure |
| `finish` joins the threads | `Async.tasks` returns when every task has returned |

`async move () => await connection(stream)` owns `stream`: a set's task may outlive the turn of the loop that
started it, so it cannot borrow from it. `ServeError.NoMemory` is the error the set gives if a task's frame cannot
be allocated.

::: tip Rule
`await set.spawn(async move () => …)` starts a task that owns what it captures. While `limit` tasks run, the
spawn waits. The set's function returns when its body and all its tasks have returned.

*Specification: `type-system.md` §40.5.*
:::

Build it, run it, and send it the four silent clients that stopped the pool:

```sh
for i in 1 2 3 4; do (sleep 3 | nc 127.0.0.1 7878) & done
curl -s -o /dev/null -w "%{http_code} after %{time_total} s\n" http://127.0.0.1:7878/about
```

```text
200 after 0.000493 s
```

Four tasks wait, and the thread is free.

## A test of tasks

The test runs the server and three clients as tasks of one thread. The clients are three children of a scope, so
their requests are open at the same time. Something must then stop the server, which would accept for ever. The
test uses a race: when the clients' side returns `Ok`, the other side, the server, is cancelled.

<span class="listing-caption"><strong>Listing 21-25</strong>, <code>src/main.cee</code>: The two sides of the test's race, and the function that runs it.</span>

<!-- ceesafe: skip -->
```ceesafe
/// One side of the test's race: the server, which never finishes first.
async fn serverSide(listener: borrow AsyncTcpListener) -> Result<bool, ServeError> {
    await serve(listener)?;
    return Ok(false);
}

/// The other side: three requests at the same time, each on its own connection.
async fn clientSide(
    port: u16,
    home: borrow mut String,
    about: borrow mut String,
    missing: borrow mut String,
) -> Result<bool, ServeError> {
    await Async.scope(ServeError.NoMemory, (s) => {
        s.spawn(async () => await fetch(port, "GET / HTTP/1.1\r\n\r\n", mut home));
        s.spawn(async () => await fetch(port, "GET /about HTTP/1.1\r\n\r\n", mut about));
        s.spawn(async () => await fetch(port, "GET /nowhere HTTP/1.1\r\n\r\n", mut missing));
    })?;
    return Ok(true);
}

/// Runs the server until the three clients have their answers. Their side wins the race, which cancels the server.
async fn serveThreeClients(
    home: borrow mut String,
    about: borrow mut String,
    missing: borrow mut String,
) -> Result<void, ServeError> {
    let listener = AsyncTcpListener.bind("127.0.0.1", 0)?;
    let port = listener.port()?;
    await Async.race(
        async () => await serverSide(borrow listener),
        async () => await clientSide(port, mut home, mut about, mut missing),
    )?;
    return Ok();
}
```

- `fetch`, in the package, connects, writes a request and appends everything it reads to a `String`.
- Each child of the scope changes a different string. Two children may not change the same variable, which is why
  there are three parameters and not one array.
- `Async.race` keeps the value of the side that wins, so both sides return one: a `bool` that nothing reads.
- When the server's side is cancelled, its `accept` returns `Err(AsyncError.Cancelled)`, `acceptLoop` returns that
  with `?`, the set cancels its tasks and waits for them, and `serve` returns. Nothing is left running when the
  race returns.

The `@test` function calls `Async.run(serveThreeClients(…))` and checks the three replies, as the test of section
21.3 does.

```sh
ceesafec test
```

```text
test answersThreeClientsAtOnce ... ok
test parsesHeaders ... ok
test parsesTheRequestLine ... ok
test refusesWhatIsNotARequest ... ok
test rendersAResponse ... ok
test routesByPath ... ok

ceesafec test: 6 passed, 0 failed
```

## Threads or tasks

| | The pool of threads | Tasks |
|---|---|---|
| A waiting client costs | a thread | a frame |
| The code | ordinary functions | `async fn` and `await` through every function that waits |
| A time limit for a client | not from outside a blocking call | `Async.timeout` |
| Long computation in a request | uses its own thread | holds up every task of the thread |
| Stopping | close the channel and join | cancellation |

A server whose requests compute for long wants threads, or both: `Async.blocking` and `Async.runWorkers`
([Tasks and threads together](./ch17-05-tasks-and-threads)). This server only waits and copies, so one thread is
enough.

## Mistakes

::: warning Mistake: a blocking socket in a task
`TcpListener` and `TcpStream` block their thread while they wait. In a task that would stop every other task, so
the compiler refuses.
:::

<span class="listing-caption"><strong>Listing 21-26</strong>, <code>main.cee</code>: An async function with the blocking listener and stream. This does not compile.</span>

<!-- ceesafe: fail N0295 -->
```ceesafe
async fn serveOne(listener: borrow TcpListener) -> Result<void, IoError> {
    var stream = listener.accept()?;
    stream.write("HTTP/1.1 200 OK\r\n\r\n".bytes())?;
    return Ok();
}

fn main() {
    let listener = TcpListener.bind("127.0.0.1", 0)!;
    match (Async.run(serveOne(borrow listener))) {
        Ok() => { Console.writeLine("answered"); }
        Err(e) => { Console.writeLine(e.message()); }
    }
}
```

```text
main.cee:2:18: error[N0295]: 'serveOne' is an async fn, which must not block its thread (type-system.md §40.1), but this may sleep: it does file or socket I/O
  |
2 |     var stream = listener.accept()?;
  |                  ^^^^^^^^^^^^^^^^^

main.cee:3:5: error[N0295]: 'serveOne' is an async fn, which must not block its thread (type-system.md §40.1), but this may sleep: it does file or socket I/O
  |
3 |     stream.write("HTTP/1.1 200 OK\r\n\r\n".bytes())?;
  |     ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
```

Use `AsyncTcpListener` and `AsyncTcpStream`, and await their waits. This is the error you meet when you turn the
server of section 21.3 into this one a function at a time, and it tells you which calls are left.

::: warning Mistake: a set's task that borrows
A task in a set is started with an `async move` closure. A closure without `move` would borrow `stream` from a
turn of the loop that ends before the task does.
:::

<span class="listing-caption"><strong>Listing 21-27</strong>, <code>main.cee</code>: The closure given to <code>set.spawn</code> is not <code>async move</code>. This does not compile.</span>

<!-- ceesafe: fail N0209 -->
```ceesafe
enum ServeError {
    NoMemory,
    @from Async(AsyncError),
}

async fn connection(stream: borrow AsyncTcpStream) -> Result<void, ServeError> {
    await stream.write("HTTP/1.1 200 OK\r\n\r\n".bytes())?;
    return Ok();
}

async fn acceptLoop(listener: borrow AsyncTcpListener, set: borrow TaskSet<ServeError>) -> Result<void, ServeError> {
    loop {
        let stream = await listener.accept()?;
        await set.spawn(async () => await connection(borrow stream))?;
    }
}

async fn serve(listener: borrow AsyncTcpListener) -> Result<void, ServeError> {
    return await Async.tasks(8, ServeError.NoMemory, async (set) => await acceptLoop(listener, set));
}

fn main() {
    let listener = AsyncTcpListener.bind("127.0.0.1", 0)!;
    match (Async.run(serve(borrow listener))) {
        Ok() => {}
        Err(_) => { Console.writeLine("failed"); }
    }
}
```

```text
main.cee:14:15: error[N0209]: a task set's child is an async move closure literal with no parameters: 'set.spawn(async move () => …)' (ADR 0053 §3.2)
   |
14 |         await set.spawn(async () => await connection(borrow stream))?;
   |               ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
```

Give the task the stream itself, as Listing 21-24 does: `connection` takes an `AsyncTcpStream` by value, and the
closure is `async move () => await connection(stream)`.

::: warning Mistake: turning a cancellation into `Ok`
`connection` swallows a client's errors. If it swallowed `Cancelled` too, a task that was told to stop would look
like one that finished, and code after it would run on. Match the cancellation and return it.
:::

::: details Coming from Rust
There is no runtime to choose and no `tokio::spawn` that needs `'static + Send`: a set's task owns its captures
(`async move`), and stays on its thread unless you ask otherwise. `Async.tasks` is close to a `JoinSet` with a
semaphore in front of it.
:::

::: details Coming from Go
A goroutine for each connection reads like the blocking code and is scheduled like these tasks. Here the two are
different functions: `async fn` with `await` for tasks, and the compiler keeps blocking calls out of them (N0295).
:::

## Summary

- `AsyncTcpListener` and `AsyncTcpStream` are the sockets for tasks. `accept`, `read` and `write` are awaited.
- The pure code of `src/http.cee` is called from tasks unchanged.
- `Async.tasks(limit, oom, body)` with `await set.spawn(async move () => …)` gives each connection a task that
  owns it, and never runs more than `limit` (`N0209` for a closure that is not `async move`).
- `Async.timeout` limits how long a client may take.
- A connection's failure stays in its task. A cancellation is passed on.
- Blocking sockets in async code are `N0295`.
- Cancelling `serve` stops the accepting loop and every connection's task.

That last point is a stop, and not yet a graceful one. Next:
[stopping gracefully](./ch21-05-graceful-shutdown).
