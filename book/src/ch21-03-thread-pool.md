---
title: A pool of threads
summary: The third stage of the web server - a fixed number of worker threads that take connections from a bounded Channel, so that a slow client holds one worker and not the server, with a test that closes the channel and joins the workers.
assumes: [ch21-02-requests-and-responses, ch16-01-starting-threads, ch16-02-channels, ch16-04-send-and-share]
keywords: [thread pool, worker, Thread.spawn, Channel, TcpStream, Send, close, join, SpawnError, backpressure, slow client]
diagnostics: [N0250]
specification: ["type-system.md §38", "type-system.md §26.4"]
---

# A pool of threads

**This section teaches** how to answer several connections at the same time with a fixed number of threads. It
changes `src/main.cee` of [section 21.2](./ch21-02-requests-and-responses) and leaves `src/http.cee` alone. It
assumes threads ([Starting threads](./ch16-01-starting-threads)), channels ([Channels](./ch16-02-channels)) and
`Send` ([Send and Share](./ch16-04-send-and-share)).

## One slow client stops everyone

The server of section 21.2 answers one connection, then the next. While it waits in `read` for one client, nobody
else is served. To see it, connect with `nc`, a tool that opens a connection and sends what you type. Here it
sends nothing for three seconds. Then ask for a page:

```sh
(sleep 3 | nc 127.0.0.1 7878) &
curl -s -o /dev/null -w "%{http_code} after %{time_total} s\n" http://127.0.0.1:7878/about
```

```text
200 after 2.689497 s
```

The page took almost three seconds, all of them spent waiting for another client.

The cure is to give each connection to a thread, so that the loop can go back to `accept` at once. A new thread
for every connection would work, and has no limit: ten thousand clients would start ten thousand threads. A
**pool** has a fixed number of threads, started once. The loop puts each connection in a channel, and each thread
takes connections out of it.

## The workers

<span class="listing-caption"><strong>Listing 21-17</strong>, <code>src/main.cee</code>: A struct for the pool, and the function that starts its threads.</span>

<!-- ceesafe: skip -->
```ceesafe
/// A fixed number of threads that answer the connections given to them.
struct Workers {
    connections: Channel<TcpStream>;
    threads: Vector<Thread<u64>>;
}

impl Workers {
    /// Starts `count` threads. Each takes connections from the channel until it is closed and empty, and
    /// returns how many it answered.
    fn start(count: usize) -> Result<Workers, ServeError> {
        let connections = Channel<TcpStream>.create(count * 2)?;
        var threads = Vector<Thread<u64>>.create();
        for (i in 0..count) {
            let mine = connections.clone();
            threads.push(Thread.spawn(move () => {
                var answered: u64 = 0;
                while (mine.recv() is Some(connection)) {
                    var stream = connection;
                    match (handle(mut stream)) {
                        Ok() => { answered += 1; }
                        Err(_) => { Console.writeErrorLine("a connection failed"); }
                    }
                }
                return answered;
            })?)?;
        }
        return Ok(Workers { connections: move connections, threads: move threads });
    }
```

This is the shape of the workers in [Channels](./ch16-02-channels#several-receivers), with connections as the
jobs.

- `Channel<TcpStream>` carries the connections themselves. Sending one moves it: after the send, the loop that
  accepted it no longer has it, and the worker that receives it is its only owner. A channel's values must be
  `Send`, and a `TcpStream` is.
- Each thread gets its own handle to the channel, `mine`, made by `clone` and moved into the thread's closure.
- `mine.recv()` waits for a connection, and returns `null` when the channel is closed and empty. That ends the
  loop and the thread.
- `var stream = connection;` moves the connection into a variable that can be changed, because `handle` takes a
  `borrow mut TcpStream`. The stream is destroyed at the end of each turn of the loop, which closes the connection.
  It is closed by the thread that answered it, and nothing in the program says so: ownership does.
- `handle` is the function of section 21.2, unchanged. It runs on whichever thread calls it.
- A thread returns how many connections it answered, so its handle is a `Thread<u64>`.
- The channel holds up to `count * 2` connections that no worker has taken yet.

`Thread.spawn` can fail, with a `SpawnError`, so the error type gets a third variant for `?` to use:

```text
enum ServeError {
    @from Io(IoError),
    @from Memory(AllocError),
    @from Spawn(SpawnError),
}
```

The name `Pool` is taken: it is a built-in allocator type ([Allocators](./ch15-03-allocators)). Hence `Workers`.

## Giving work, and finishing

<span class="listing-caption"><strong>Listing 21-18</strong>, <code>src/main.cee</code>: The other two methods of <code>Workers</code>.</span>

<!-- ceesafe: skip -->
```ceesafe
    /// Gives a connection to the workers. It waits while the channel is full, and returns `false` after `finish`.
    fn take(borrow this, stream: TcpStream) -> bool => this.connections.send(stream).isOk();

    /// Closes the channel and waits for the threads. Every connection taken before is answered first.
    /// Returns how many were answered.
    fn finish(this) -> u64 {
        this.connections.close();
        var threads = this.threads;
        var answered: u64 = 0;
        while (threads.pop() is Some(thread)) {
            answered += thread.join();
        }
        return answered;
    }
}
```

`take` sends the connection. If every worker is busy and the channel is full, `send` waits, and so does the loop
that called it. That is what you want: the server accepts no faster than it answers, and connections that come
meanwhile wait in the system's queue. A queue without a limit would hide the overload until memory ran out.

`finish` takes `this` by value, so it uses the pool up: after `workers.finish()`, the variable `workers` is gone
and nothing more can be given to it. It closes the channel, and then joins each thread. A closed channel still
hands out what is in it, so each worker answers the connections that were already taken, then gets `null`, and
returns. This is a **graceful** stop: no new work, and all the work in hand finished.

::: tip Rule
To stop workers that receive from a channel, close the channel and join the threads. Values sent before the close
are still received.

*Specification: ADR 0032 §3 (`spec/decisions/0032-channels.md`).*
:::

## The loop

<span class="listing-caption"><strong>Listing 21-19</strong>, <code>src/main.cee</code>: <code>main</code> accepts, and hands each connection to the workers.</span>

<!-- ceesafe: skip -->
```ceesafe
fn main() {
    let listener = TcpListener.bind("127.0.0.1", 7878)!;
    let workers = Workers.start(THREADS)!;
    Console.writeLine("listening on http://127.0.0.1:7878");
    Console.flush();
    loop {
        let _ = workers.take(listener.accept()!);
    }
}
```

`THREADS` is a constant, `const THREADS: usize = 4;`. Build the server, run it, and try the slow client again:

```sh
(sleep 3 | nc 127.0.0.1 7878) &
curl -s -o /dev/null -w "%{http_code} after %{time_total} s\n" http://127.0.0.1:7878/about
```

```text
200 after 0.002027 s
```

The silent client holds one worker, and another worker answers.

This `main` never calls `finish`: it runs until the program is stopped from outside. The test does call it.

## A test that stops the workers

<span class="listing-caption"><strong>Listing 21-20</strong>, <code>src/main.cee</code>: Three connections, two workers, and a stop.</span>

<!-- ceesafe: skip -->
```ceesafe
@test
fn answersThreeConnectionsAndStops() -> Result<void, TestFailure> {
    let listener = TcpListener.bind("127.0.0.1", 0)!;
    let port = listener.port()!;
    let workers = Workers.start(2)!;
    var first = TcpStream.connect("127.0.0.1", port)!;
    var second = TcpStream.connect("127.0.0.1", port)!;
    var third = TcpStream.connect("127.0.0.1", port)!;
    first.write("GET / HTTP/1.1\r\n\r\n".bytes())!;
    second.write("GET /about HTTP/1.1\r\n\r\n".bytes())!;
    third.write("GET /nowhere HTTP/1.1\r\n\r\n".bytes())!;
    for (i in 0..3) {
        Check.isTrue(workers.take(listener.accept()!), "the workers take a connection")?;
    }
    Check.equalU64(3, workers.finish())?;
    Check.isTrue(readReply(mut first)!.view().startsWith("HTTP/1.1 200 OK\r\n"), "/ is 200")?;
    Check.isTrue(readReply(mut second)!.view().endsWith(ABOUT), "/about is the about page")?;
    Check.isTrue(readReply(mut third)!.view().startsWith("HTTP/1.1 404 Not Found\r\n"), "/nowhere is 404")?;
    return Ok();
}
```

The test is the three clients and the accepting loop in one. `readReply`, in the package, is the reading loop of
the earlier tests as a function. `workers.finish()` returns when both threads have ended, and its result says that
three connections were answered between them. Only then does the test read the replies, which have been waiting in
each client's connection.

```sh
ceesafec test
```

```text
test answersThreeConnectionsAndStops ... ok
test parsesHeaders ... ok
test parsesTheRequestLine ... ok
test refusesWhatIsNotARequest ... ok
test rendersAResponse ... ok
test routesByPath ... ok

ceesafec test: 6 passed, 0 failed
```

::: info Under the interpreter
`ceesafec test --interp` runs threads one at a time, and a thread that waits on a full channel is not resumed
there ([Channels](./ch16-02-channels#waiting)). The test passes under it because the channel of two workers has
room for four connections, and the test sends three.
:::

## What the pool does not solve

Four silent clients hold all four workers:

```sh
for i in 1 2 3 4; do (sleep 3 | nc 127.0.0.1 7878) & done
curl -s -o /dev/null -w "%{http_code} after %{time_total} s\n" http://127.0.0.1:7878/about
```

```text
200 after 2.690891 s
```

A thread that waits in `read` does nothing else, and a thread is not cheap: it has a stack of its own, and the
system must switch between them. More threads move the limit and do not remove it. The next section keeps one
thread and lets it wait for many connections at once.

## Mistakes

::: warning Mistake: using a connection after giving it away
`send` moves the stream into the channel. The loop that accepted it cannot write to it afterwards: a worker may be
writing to it at that moment, on another thread.
:::

<span class="listing-caption"><strong>Listing 21-21</strong>, <code>main.cee</code>: A stream is sent to a channel and then written to. This does not compile.</span>

<!-- ceesafe: fail N0250 -->
```ceesafe
fn main() {
    let listener = TcpListener.bind("127.0.0.1", 0)!;
    let connections = Channel<TcpStream>.create(1)!;
    var client = TcpStream.connect("127.0.0.1", listener.port()!)!;
    var stream = listener.accept()!;
    let _ = connections.send(stream);
    stream.write("HTTP/1.1 200 OK\r\n\r\n".bytes())!;
}
```

```text
main.cee:7:5: error[N0250]: 'stream' is used here after it was moved (on at least one path)
  |
7 |     stream.write("HTTP/1.1 200 OK\r\n\r\n".bytes())!;
  |     ^^^^^^
  = note: the value was moved here (6:5)
```

In a language without this check, two threads would write to one connection, and the client would get the two
answers mixed. Here one thread owns a connection at a time.

::: warning Mistake: joining before closing
`finish` closes the channel first. Joining first would wait for threads that wait in `recv` for a close that never
comes. The compiler does not check that a wait ends ([Channels](./ch16-02-channels#mistakes)).
:::

::: warning Mistake: a thread for every connection
`Thread.spawn` in the accepting loop is shorter than a pool, and lets clients decide how many threads the
server has. Fix the number, and let the channel's capacity make the loop wait.
:::

::: details Coming from Rust
The Rust Book's pool sends boxed closures through an `mpsc` channel that the workers share behind
`Arc<Mutex<Receiver>>`. A CeeSafe `Channel<T>` may be received from by several threads, so each worker has a clone
of the handle and there is no mutex. This pool sends the connections, not closures, because every job is the same.
:::

::: details Coming from Go
`Channel<TcpStream>.create(n)` is `make(chan net.Conn, n)`, and the workers' loop is `for conn := range ch`. The
difference is the move: after `ch <- conn`, Go still lets the sender use `conn`. Here that is N0250.
:::

## Summary

- A server that answers one connection at a time waits with its slowest client.
- A pool is a fixed number of threads that take work from one bounded channel. `Channel<TcpStream>` moves each
  connection to the worker that will answer it, and that worker closes it.
- A full channel makes the accepting loop wait, so the server accepts no faster than it answers.
- To stop: close the channel, then join. What was sent before the close is still answered.
- A value that was sent is gone from the sender (`N0250`).
- Each waiting client still holds a whole thread.

Next: [async tasks](./ch21-04-async-tasks), where waiting costs a task and not a thread.
