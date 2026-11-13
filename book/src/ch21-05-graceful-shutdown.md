---
title: Stopping gracefully
summary: The last stage of the web server - Ctrl+C is caught with ProcessSignals and closes a channel that asks the server to stop, a race cancels the accepting loop, and a shield lets each open connection finish its answer within a time limit, with a test that stops the server in the middle of a request.
assumes: [ch21-04-async-tasks, ch17-03-timeouts-races-cancellation, ch17-05-tasks-and-threads, ch16-02-channels]
keywords: [graceful shutdown, cancellation, "Async.shield", "Async.race", Channel, close, recvAsync, ProcessSignals, ProcessSignal, waitAsync, "Ctrl+C", SIGINT, SIGTERM, "TcpListener.shutdown", "Process.exit", in flight]
diagnostics: [N0295]
specification: ["type-system.md §40.7", "type-system.md §40.5"]
---

# Stopping gracefully

**This section teaches** how to stop a server without breaking the answers it is in the middle of. It finishes
the server of [section 21.4](./ch21-04-async-tasks). It assumes cancellation and `Async.shield`
([Timeouts, races and cancellation](./ch17-03-timeouts-races-cancellation)), channels between threads and tasks
([Tasks and threads together](./ch17-05-tasks-and-threads)) and closing a channel ([Channels](./ch16-02-channels)).

A server is stopped many times in its life: for a new version, for a new configuration, to move it. A stop is
**graceful** when it has two parts:

1. The server accepts no new connections.
2. Every connection that is open, **in flight**, gets its answer, within some limit of time.

The pool of [section 21.3](./ch21-03-thread-pool) already stops this way when its `finish` is called: closing the
channel is part 1, and joining the workers is part 2. This section does the same for the tasks.

## What cancellation does to a connection

Cancelling `serve`, as the test of section 21.4 does, is part 1 and the opposite of part 2. The accepting loop
stops, and so does every connection's task: its next `read` or `write` returns `Err(AsyncError.Cancelled)`, the
task returns, and its stream is closed with the answer half sent or not sent at all.

A **shield** changes that. Inside `Async.shield(ms, body)` the waits do not see the cancellation, for at most
`ms` milliseconds counted from the start of the shield.

<span class="listing-caption"><strong>Listing 21-28</strong>, <code>main.cee</code>: An answer that takes 50 milliseconds is cancelled after 10, without a shield and with one.</span>

```ceesafe
async fn answer(answered: borrow mut bool) -> Result<bool, AsyncError> {
    await Timer.sleep(50)?;
    answered = true;
    return Ok(false);
}

async fn shielded(answered: borrow mut bool) -> Result<bool, AsyncError> {
    return await Async.shield(1000, async () => await answer(mut answered));
}

async fn stop() -> Result<bool, AsyncError> {
    await Timer.sleep(10)?;
    return Ok(true);
}

async fn run() -> Result<void, AsyncError> {
    var plain = false;
    var kept = false;
    await Async.race(async () => await answer(mut plain), async () => await stop())?;
    await Async.race(async () => await shielded(mut kept), async () => await stop())?;
    await AsyncConsole.writeLine(if (plain) "without a shield: answered" else "without a shield: cancelled")?;
    await AsyncConsole.writeLine(if (kept) "with a shield: answered" else "with a shield: cancelled")?;
    return Ok();
}

fn main() {
    match (Async.run(run())) {
        Ok() => {}
        Err(_) => { Console.writeLine("failed"); }
    }
}
```

```text
without a shield: cancelled
with a shield: answered
```

In both races `stop` wins after 10 milliseconds and the other side is cancelled. The race then waits for the
cancelled side to return, as it always does. Without a shield, the sleep returns `Cancelled` at once. With one,
the sleep runs to its end, the answer is given, and the race returns 40 milliseconds later.

::: tip Rule
Within `Async.shield(ms, body)`, the library's waits ignore a cancellation. Once `ms` milliseconds have passed
since the shield began, they return `Err(AsyncError.TimedOut)`.

*Specification: `type-system.md` §40.7.*
:::

## A shield around each connection

<span class="listing-caption"><strong>Listing 21-29</strong>, <code>src/main.cee</code>: The task of a connection, with a shield in place of the timeout.</span>

<!-- ceesafe: skip -->
```ceesafe
/// The task of one connection. It owns the stream. The shield gives the client a limited time, and lets the
/// answer finish when the server is stopping. A connection's own failure stays here.
async fn connection(stream: AsyncTcpStream) -> Result<void, ServeError> {
    let result = await Async.shield(PATIENCE, async () => await handle(borrow stream));
    if (result.isErr()) {
        await AsyncConsole.writeError("a connection failed\n")?;
    }
    return Ok();
}
```

The shield does two jobs. Its time limit is the client's five seconds, which `Async.timeout` gave before: a
silent client's `read` returns `TimedOut` when the time is up, whether the server is stopping or not. And while
the time lasts, a cancellation does not reach `handle`. So when the server is cancelled, each open connection has
what is left of its five seconds to finish, and no stop takes longer than that.

`handle` can no longer return `Cancelled`, so the `match` of section 21.4 is gone, and with it the variant
`TooSlow`. The line that reports a failure is outside the shield: if the server is stopping, `writeError` returns
`Cancelled` and `?` passes it on.

`acceptLoop` and `serve` do not change. A cancelled `serve` now behaves as a graceful stop needs: `accept` returns
`Cancelled`, which ends the loop, and `Async.tasks` then waits for the connections' tasks, which finish their
answers behind their shields.

## Asking the server to stop

Something must cancel `serve`, at a moment that the program chooses. The request to stop is a channel that is
closed. A closed channel is a good signal: any thread or task with a handle can close it, closing twice does no
harm, and every receiver sees it.

<span class="listing-caption"><strong>Listing 21-30</strong>, <code>src/main.cee</code>: The server races against the request to stop.</span>

<!-- ceesafe: skip -->
```ceesafe
/// One side of a race: the server, which never finishes first.
async fn serverSide(listener: borrow AsyncTcpListener) -> Result<bool, ServeError> {
    await serve(listener)?;
    return Ok(false);
}

/// The other side: waits until `stop` is closed.
async fn stopAsked(stop: borrow Channel<bool>) -> Result<bool, ServeError> {
    while ((await stop.recvAsync()?).isSome()) {}
    return Ok(true);
}

/// Serves until `stop` is closed, and then until the connections in flight are answered.
async fn serveUntilStopped(listener: borrow AsyncTcpListener, stop: borrow Channel<bool>) -> Result<void, ServeError> {
    await Async.race(async () => await serverSide(listener), async () => await stopAsked(stop))?;
    return Ok();
}
```

- `stopAsked` receives until `recvAsync` gives `null`, which it does when the channel is closed and empty. Nobody
  sends on this channel, so the loop's body is empty.
- When `stopAsked` returns `Ok`, the race cancels `serverSide`, and waits for it. That wait is the graceful part:
  `serveUntilStopped` returns when the last connection in flight has been answered or has run out of time.
- `serverSide` moved here from the test of section 21.4. As there, the race needs a value from each side, and
  nothing reads the `bool`.

In `main`, the request to stop comes from outside the program. Ctrl+C in the terminal sends the process the signal
`SIGINT`, and the command `kill` sends `SIGTERM`. By default either one ends the process at once: nothing is
destroyed, and an answer in flight is cut. `ProcessSignals.subscribe()` changes that for as long as its value
lives. The request is then kept for the program, and `await signals.waitAsync()` returns it, as
`ProcessSignal.Interrupt` or `ProcessSignal.Terminate`.

<span class="listing-caption"><strong>Listing 21-31</strong>, <code>src/main.cee</code>: A task waits for Ctrl+C and closes the channel, beside the server.</span>

<!-- ceesafe: skip -->
```ceesafe
/// Closes `stop` when the process is asked to stop: Ctrl+C in its terminal, or `kill`.
async fn stopOnSignal(signals: borrow ProcessSignals, stop: borrow Channel<bool>) -> Result<void, ServeError> {
    let _ = await signals.waitAsync()?;
    stop.close();
    return Ok();
}

/// The server, and beside it the wait for a request to stop.
async fn run(listener: borrow AsyncTcpListener, signals: borrow ProcessSignals) -> Result<void, ServeError> {
    let stop = Channel<bool>.create(1)?;
    return await Async.all(
        async () => await serveUntilStopped(listener, borrow stop),
        async () => await stopOnSignal(signals, borrow stop),
    );
}

fn main() {
    let listener = AsyncTcpListener.bind("127.0.0.1", 7878)!;
    let signals = ProcessSignals.subscribe()!;
    Console.writeLine("listening on http://127.0.0.1:7878, press Ctrl+C to stop");
    Console.flush();
    match (Async.run(run(borrow listener, borrow signals))) {
        Ok() => { Console.writeLine("stopped"); }
        Err(_) => {
            Console.writeErrorLine("the server failed");
            Process.exit(1);
        }
    }
}
```

- `subscribe` is called in `main`, before the server starts, and its value lives until `main` ends. A process has
  one subscription at a time: a second `subscribe` returns `Err(IoError.AlreadyExists)`.
- `stopOnSignal` does not care which of the two signals came, so it throws the value away and closes the channel.
  The server's side sees the closed channel as before, and the test below still closes it by hand.
- `Async.all` runs both sides until both have returned. After Ctrl+C, `stopOnSignal` returns at once, and
  `serveUntilStopped` returns when the connections in flight are answered.
- While `signals` lives, a second Ctrl+C does not end the program either: it waits to be read. The shield's time
  limit is what bounds the stop.

Run it, and try it with a client that is slow on purpose. In a second terminal, send half a request, wait two
seconds, and send the rest:

```sh
(printf 'GET /about HT'; sleep 2; printf 'TP/1.1\r\n\r\n') | nc 127.0.0.1 7878 | head -1
```

Press Ctrl+C in the server's terminal during those two seconds. The server prints `stopped` only when the two
seconds are over, and the client has its answer:

```text
HTTP/1.1 200 OK
```

```text
listening on http://127.0.0.1:7878, press Ctrl+C to stop
stopped
```

::: info What a new client sees meanwhile
The listener is a local of `main`, so its socket stays open until `main` ends. A client that connects while the
server finishes its last answers is not accepted: it waits in the system's queue, and its connection is reset
when the listener is destroyed. `curl` reports `Connection reset by peer`.
:::

## A test that stops in the middle of a request

<span class="listing-caption"><strong>Listing 21-32</strong>, <code>src/main.cee</code>: A client is half way through its request when the server is asked to stop.</span>

<!-- ceesafe: skip -->
```ceesafe
/// A client that is half way through its request when the server is asked to stop.
async fn slowClient(port: u16, stop: borrow Channel<bool>, reply: borrow mut String) -> Result<void, ServeError> {
    let slow = await AsyncTcpStream.connect("127.0.0.1", port)?;
    await slow.write("GET /about HT".bytes())?;
    // A whole request on a second connection. When it is answered, the server has accepted the first one too.
    var other = String.create();
    await fetch(port, "GET / HTTP/1.1\r\n\r\n", mut other)?;
    stop.close();
    await Timer.sleep(20)?;
    await slow.write("TP/1.1\r\n\r\n".bytes())?;
    var buffer = [0u8; 256];
    loop {
        let n = await slow.read(mut buffer[..])?;
        if (n == 0) {
            return Ok();
        }
        reply.append(StringView.fromUtf8(buffer[0..n]) ?? "?")?;
    }
}

/// Runs the server and the slow client until both have returned.
async fn stopWithOneInFlight(reply: borrow mut String) -> Result<void, ServeError> {
    let listener = AsyncTcpListener.bind("127.0.0.1", 0)?;
    let port = listener.port()?;
    let stop = Channel<bool>.create(1)?;
    return await Async.all(
        async () => await serveUntilStopped(borrow listener, borrow stop),
        async () => await slowClient(port, borrow stop, mut reply),
    );
}
```

The test must be sure that the slow connection is in flight, and not still in the system's queue, when it asks
for the stop. A sleep would only make that likely. Instead the client makes a second, whole request and waits for
its answer: connections are accepted in the order they arrive, so when the second is answered, the first has its
task. Then the client closes `stop`, waits a moment for the server to act on it, and sends the rest of the first
request.

`Async.all` needs both sides to return `Ok`. So the test passes only if the server returns by itself after the
stop, and the `@test` function then checks that `reply` is the about page with status 200. With the `connection`
of section 21.4 in place of Listing 21-29, `ceesafec test` fails this test: the reply is empty.

```sh
ceesafec test
```

```text
test answersThreeClientsAtOnce ... ok
test finishesTheConnectionInFlight ... ok
test parsesHeaders ... ok
test parsesTheRequestLine ... ok
test refusesWhatIsNotARequest ... ok
test rendersAResponse ... ok
test routesByPath ... ok

ceesafec test: 7 passed, 0 failed
```

## The same stop for a server of threads

A server of threads, as in [section 21.3](./ch21-03-thread-pool), waits in the blocking `accept`, which no
cancellation reaches. Two calls stop it. `signals.wait()` blocks a thread until the request to stop comes. Then
`listener.shutdown()` makes every `accept` on that listener, the one a thread is waiting in and every later one,
return `Err(IoError.ConnectionAborted)`. Both calls borrow the listener, so the threads share it through an `Arc`
([Shared state](./ch16-03-shared-state)).

<span class="listing-caption"><strong>Listing 21-33</strong>, <code>main.cee</code>: One thread accepts and answers. The main thread waits for Ctrl+C and shuts the listener down.</span>

<!-- ceesafe: check -->
```ceesafe
fn answer(stream: TcpStream) {
    var client = stream;
    let _ = client.write("HTTP/1.1 200 OK\r\n\r\nhello\n".bytes());
}

fn main() {
    let listener = Arc<TcpListener>.create(TcpListener.bind("127.0.0.1", 7878)!)!;
    let signals = ProcessSignals.subscribe()!;
    let accepting = listener.clone();
    let acceptor = Thread.spawn(move () => {
        var answered: u32 = 0;
        loop {
            match (accepting.get().accept()) {
                Ok(stream) => {
                    answer(stream);
                    answered += 1;
                }
                Err(_) => { break; }
            }
        }
        return answered;
    })!;
    Console.writeLine("listening on http://127.0.0.1:7878, press Ctrl+C to stop");
    Console.flush();
    let _ = signals.wait();
    listener.get().shutdown()!;
    Console.writeLine(acceptor.join());
}
```

After two requests and Ctrl+C, the compiled program prints:

```text
listening on http://127.0.0.1:7878, press Ctrl+C to stop
2
```

The accepting thread finishes the answer it is writing, then finds its `accept` refused and leaves the loop, and
`main` joins it. With the pool of section 21.3, the accepting thread gives each stream to `workers.take` and
calls `workers.finish()` after the loop, which answers what the workers hold. `shutdown` does not close the
socket: the listener closes when its last owner is destroyed.

Try a server of threads compiled, with `ceesafec build`. The interpreter runs one thread at a time, so there a
thread that waits in `accept` or in `signals.wait()` cannot be woken by another.

## Mistakes

::: warning Mistake: the blocking wait for a signal in a task
`signals.wait()` blocks its thread until a signal comes. In a task it would stop the server it is meant to watch
over.
:::

<span class="listing-caption"><strong>Listing 21-34</strong>, <code>main.cee</code>: An async function waits with the blocking call. This does not compile.</span>

<!-- ceesafe: fail N0295 -->
```ceesafe
async fn stopAsked(signals: borrow ProcessSignals) -> Result<void, AsyncError> {
    let _ = signals.wait();
    return Ok();
}

fn main() {
    let signals = ProcessSignals.subscribe()!;
    match (Async.run(stopAsked(borrow signals))) {
        Ok() => { Console.writeLine("stopped"); }
        Err(_) => { Console.writeLine("failed"); }
    }
}
```

```text
main.cee:2:5: error[N0295]: 'stopAsked' is an async fn, which must not block its thread (type-system.md §40.1), but this may sleep: it calls 'ProcessSignals.wait', which calls 'Channel<u32>.recv', which calls 'ceesafe_chan_wait', declared @effects(sleep)
  |
2 |     let _ = signals.wait();
  |     ^^^^^^^^^^^^^^^^^^^^^^^
```

Write `await signals.waitAsync()?` in a task, as Listing 21-31 does. `signals.wait()` is for a thread.

::: warning Mistake: a shield with no end
A shield's time is the longest a stop can take. A very large number brings back the client that connects, says
nothing, and keeps the server from ever stopping. Choose the time you are willing to wait.
:::

::: warning Mistake: an accepting loop that survives its cancellation
A loop that matches the error of `accept` and goes round again never ends: once the task is cancelled, every
`accept` returns `Cancelled` at once, and the loop spins. Pass the error on with `?`, as `acceptLoop` does.
:::

::: details Coming from Rust
The Rust Book stops its pool in `Drop`: dropping the sender ends the workers' loops, and the pool joins them.
`Workers.finish` of section 21.3 is that, called by name. For tasks, `Async.shield` has no direct counterpart in
Tokio, where graceful shutdown is usually a `CancellationToken` that each connection's task chooses when to look
at.
:::

::: details Coming from Go
`http.Server.Shutdown(ctx)` is these two parts: it closes the listeners, then waits for the open connections
until the context's deadline. Here the deadline is the shield's time, set for each connection.
:::

## Summary

- A graceful stop accepts nothing new, and answers what is in flight, within a time limit.
- Cancelling a task makes its waits fail. `Async.shield(ms, body)` lets the waits in `body` go on for at most `ms`
  milliseconds from the shield's start.
- A shield around each connection is both the client's time limit and what lets an answer finish during a stop.
- A closed channel is a request to stop that threads and tasks can both make and see. `Async.race` turns it into
  the cancellation of the server.
- `ProcessSignals.subscribe()` keeps Ctrl+C and `SIGTERM` for the program. A task waits for one with
  `await signals.waitAsync()`, and a thread with `signals.wait()`, which is `N0295` in a task.
- `listener.shutdown()` ends the blocking `accept` of a `TcpListener` from another thread: every `accept` returns
  `Err(IoError.ConnectionAborted)`.
- Test the stop itself: a request in flight, the stop, and the answer.

## Where to go from here

The finished server is about 200 lines in `src/main.cee`, half of them tests, and 190 in `src/http.cee`. It leaves
out much of HTTP, and each missing part is an exercise that the same tools reach:

- **Files from disk.** Serve a directory with `AsyncFiles` in place of `embedText`. Check the path first: refuse
  any that contains `..`.
- **Several requests on one connection.** Loop in `handle` until the client closes or sends `Connection: close`,
  and leave `Connection: close` out of the response.
- **Requests with a body.** Read the `Content-Length` header with `request.header`, then that many more bytes.
- **Several threads of tasks.** `Async.runWorkers` and `set.spawnSend` spread the connections over the processor's
  cores ([Tasks and threads together](./ch17-05-tasks-and-threads)).

This is the last project of the book. It used ownership to decide who closes a connection, regions to parse
without copying, a channel to move work between threads, tasks to wait for many clients on one thread, and
cancellation to stop. None of it needed `unsafe`, and the compiler checked all of it.
