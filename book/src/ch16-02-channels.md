---
title: Channels
summary: Passing values between threads through a bounded Channel - creating one, cloning a handle for each thread, send and recv and what makes them wait, closing so that receivers see the end, several senders and several receivers, and the calls that never wait.
assumes: [ch16-01-starting-threads, ch06-03-optionals, ch09-02-result]
keywords: [Channel, send, recv, close, clone, trySend, tryRecv, isClosed, capacity, deadlock, N0402]
diagnostics: [N0250, N0402]
specification: ["decisions/0032-channels.md §3", "decisions/0060-web-random-tryrecv-foreign-host.md"]
---

# Channels

**This section teaches** how threads pass values to one another while they run. It assumes you can start and join a
thread ([Starting threads](./ch16-01-starting-threads)), and know optionals ([Optionals](./ch06-03-optionals)) and
`Result` ([Result](./ch09-02-result)).

A thread's result reaches its starter once, at `join`. A **channel** carries values all the time the threads run: a
queue that one thread puts values into and another takes them out of. A value that is sent moves into the channel,
and moves out to whoever receives it. So a channel shares nothing: at every moment each value has one owner.

## Send and receive

<span class="listing-caption"><strong>Listing 16-11</strong>, <code>main.cee</code>: A thread sends four squares and closes the channel. <code>main</code> receives until the end and adds them.</span>

```ceesafe
fn main() {
    let channel = Channel<u64>.create(8)!;
    let sender = channel.clone();
    let worker = Thread.spawn(move () => {
        for (i in 1..5u64) {
            sender.send(i * i)!;
        }
        sender.close();
    })!;
    var total: u64 = 0;
    while (channel.recv() is Some(value)) {
        total += value;
    }
    worker.join();
    Console.writeLine(total);
}
```

```text
30
```

- `Channel<u64>.create(8)` makes a channel that holds up to 8 values of type `u64`. Its buffer is allocated once, so
  `create` returns a `Result`, unwrapped here with `!`.
- `channel.clone()` makes a second **handle** to the same channel. It does not copy the channel. The thread's
  closure takes `sender`, and `main` keeps `channel`.
- `sender.send(v)` puts a value in. It returns a `Result`, explained below.
- `sender.close()` says that no more values will come.
- `channel.recv()` takes the next value out. It returns an optional: `Some(value)`, or `null` when the channel is
  closed and empty. The `while (… is Some(value))` loop therefore ends by itself.

::: tip Rule
A `Channel<T>` is a queue of at most `capacity` values. Every handle may send and receive. `clone` makes another
handle, and the channel lives until its last handle is destroyed; values still in it are destroyed with it.

*Specification: ADR 0032 §3 (`spec/decisions/0032-channels.md`).*
:::

There is one type, not a sending half and a receiving half. Which thread sends and which receives is up to the
program.

## Waiting

A channel is **bounded**: it never holds more than its capacity. That decides when the two calls wait.

| Call | Channel has room / a value | Full / empty | Closed |
|---|---|---|---|
| `send(v)` | puts `v` in, `Ok()` | waits for room | `Err(v)`: the value comes back |
| `recv()` | takes the oldest value, `Some(v)` | waits for a value | `Some(v)` while values remain, then `null` |

A sender that is faster than its receiver waits when the channel is full, so the queue cannot grow without limit.
The capacity must be at least 1: `Channel<T>.create(0)` panics.

::: tip Rule
`close()` makes every later `send` fail with `Err(value)`. Receivers still take what is in the channel, and then get
`null`. Destroying a handle does not close the channel: only `close()` does.

*Specification: ADR 0032 §3.*
:::

::: info Capacity under the interpreter
`ceesafec run --interp` runs one thread at a time. A thread that waits there on a full channel is not resumed, and the
interpreter stops with a deadlock fault. Under the interpreter, give a channel room for every value that crosses it,
as these listings do. A program built with `ceesafec build` has no such limit: the sender waits, and goes on when
the receiver has taken a value.
:::

## Values move through

Sending a value gives it away, as passing it to a function does. The next listing sends strings, and has several
senders.

<span class="listing-caption"><strong>Listing 16-12</strong>, <code>main.cee</code>: Three threads each send one string. <code>main</code> closes the channel when all have finished, and counts what arrived.</span>

```ceesafe
fn main() {
    let lines = Channel<String>.create(8)!;
    var workers = Vector<Thread<void>>.create();
    for (id in 1..4u64) {
        let mine = lines.clone();
        workers.push(Thread.spawn(move () => {
            var line = String.from("report from thread ")!;
            line.appendNumber(id)!;
            mine.send(line)!;
        })!)!;
    }
    while (workers.pop() is Some(worker)) {
        worker.join();
    }
    lines.close();
    var count = 0;
    var letters: usize = 0;
    while (lines.recv() is Some(line)) {
        count += 1;
        letters += line.length;
    }
    Console.writeLine(count);
    Console.writeLine(letters);
}
```

```text
3
60
```

- Each thread gets its own handle, made by `clone` inside the loop and moved into that thread's closure.
- After `mine.send(line)`, the thread no longer has `line`. Using it there is the error N0250, as after any move.
- With several senders, no single sender knows when the others are done. Here `main` knows: it joins the three
  threads and then closes the channel.
- The strings arrive in an order that depends on timing. The listing prints only their number and total length,
  which do not.

## Several receivers

Any handle may receive, so a channel can also hand out work: each value goes to exactly one of the threads that
call `recv`.

<span class="listing-caption"><strong>Listing 16-13</strong>, <code>main.cee</code>: Two threads take jobs from one channel and send their results back on another.</span>

```ceesafe
struct Job {
    name: String;
    copies: u64;
}

fn main() {
    let jobs = Channel<Job>.create(4)!;
    let results = Channel<u64>.create(4)!;
    var workers = Vector<Thread<void>>.create();
    for (w in 0..2) {
        let myJobs = jobs.clone();
        let myResults = results.clone();
        workers.push(Thread.spawn(move () => {
            while (myJobs.recv() is Some(job)) {
                myResults.send(job.copies * job.name.length as u64)!;
            }
        })!)!;
    }
    jobs.send(Job { name: String.from("ab")!, copies: 10 })!;
    jobs.send(Job { name: String.from("abc")!, copies: 100 })!;
    jobs.close();
    while (workers.pop() is Some(worker)) {
        worker.join();
    }
    results.close();
    var total: u64 = 0;
    while (results.recv() is Some(value)) {
        total += value;
    }
    Console.writeLine(total);
}
```

```text
320
```

Closing `jobs` is what ends the workers: each one's `recv` returns `null` when the jobs are gone, its loop ends, and
the thread finishes. Then `main` can join them, close `results`, and read what they sent.

## Without waiting

Three more calls never wait:

- `trySend(v)` sends if there is room now, and returns `Err(v)` if the channel is full or closed;
- `tryRecv()` returns a value if one is there now, and `null` if the channel is empty, closed or not;
- `isClosed()` says whether `close` has been called. Values may still be in the channel then.

<span class="listing-caption"><strong>Listing 16-14</strong>, <code>main.cee</code>: The calls that do not wait, and a send to a closed channel, on one thread.</span>

```ceesafe
fn main() {
    let channel = Channel<u64>.create(2)!;
    Console.writeLine(channel.tryRecv() ?? 0);
    channel.send(1)!;
    channel.send(2)!;
    match (channel.trySend(3)) {
        Ok() => { Console.writeLine("sent 3"); }
        Err(value) => { Console.writeLine("full"); }
    }
    channel.close();
    Console.writeLine(channel.isClosed());
    match (channel.send(4)) {
        Ok() => { Console.writeLine("sent 4"); }
        Err(value) => { Console.writeLine(value); }
    }
    Console.writeLine(channel.recv() ?? 0);
    Console.writeLine(channel.recv() ?? 0);
    Console.writeLine(channel.recv() ?? 0);
}
```

```text
0
full
true
4
1
2
0
```

The first `tryRecv` finds nothing, and `?? 0` prints 0 for its `null`. The third value does not fit. After `close`,
`send(4)` gives the 4 back in `Err`. The two values sent before the close are still received, in the order they were
sent, and the last `recv` returns `null`.

`send` returns the value in its error because the value was moved into the call: if the channel cannot take it, the
caller gets it back and decides what to do with it.

A task (chapter 17) uses `sendAsync` and `recvAsync` instead of `send` and `recv`: they let the thread's other tasks
run while one waits.

## Mistakes

::: warning Mistake: never closing the channel
A receiver's loop ends only at `null`, and `null` comes only after `close()`. A sender that finishes, or a handle
that is destroyed, does not close the channel.
:::

<span class="listing-caption"><strong>Listing 16-15</strong>, <code>main.cee</code>: The thread sends two values and ends without closing. <code>main</code> waits for a third value that never comes.</span>

<!-- ceesafe: exits 70 -->
```ceesafe
fn main() {
    let channel = Channel<u64>.create(4)!;
    let sender = channel.clone();
    let worker = Thread.spawn(move () => {
        sender.send(1)!;
        sender.send(2)!;
    })!;
    var total: u64 = 0;
    while (channel.recv() is Some(value)) {
        total += value;
    }
    worker.join();
    Console.writeLine(total);
}
```

This compiles: the compiler does not check that a wait ends. The interpreter sees that no thread can go on, and
stops with exit status 70:

```text
ceesafe interp: fault: deadlock: every thread is waiting for another (type-system.md §38) at lib/std/channel.cee:156:22
```

A built program has no such check. It waits for ever. Add `sender.close();` as the thread's last statement, as in
Listing 16-11.

::: warning Mistake: moving the only handle into the thread
The closure takes the handle it uses. If that is the only one, the starter has none left.
:::

<span class="listing-caption"><strong>Listing 16-16</strong>, <code>main.cee</code>: The closure captures <code>channel</code>, and <code>main</code> then uses it. This does not compile.</span>

<!-- ceesafe: fail N0250 -->
```ceesafe
fn main() {
    let channel = Channel<u64>.create(4)!;
    let worker = Thread.spawn(move () => {
        channel.send(7)!;
        channel.close();
    })!;
    worker.join();
    Console.writeLine(channel.recv() ?? 0);
}
```

```text
main.cee:8:23: error[N0250]: 'channel' is used here after it was moved (on at least one path)
  |
8 |     Console.writeLine(channel.recv() ?? 0);
  |                       ^^^^^^^
```

Clone a handle for the thread before the spawn (`let sender = channel.clone();`) and use `sender` in the closure.

::: warning Mistake: ignoring what `send` returns
A `send` to a closed channel does not send. If nothing reads its `Result`, the value is lost without a word.
:::

<span class="listing-caption"><strong>Listing 16-17</strong>, <code>main.cee</code>: The result of <code>send</code> is dropped. The compiler warns.</span>

<!-- ceesafe: warns N0402 -->
```ceesafe
fn main() {
    let channel = Channel<u64>.create(4)!;
    channel.close();
    channel.send(7);
    Console.writeLine(channel.recv() ?? 0);
}
```

```text
main.cee:4:5: warning[N0402]: the 'Result' of this call is not used, so an error it reports is lost: handle it with '?', '!' or 'match', or write 'let _ = ...;' to discard it
  |
4 |     channel.send(7);
  |     ^^^^^^^^^^^^^^^
```

The program still runs, and prints 0: the 7 never entered the channel. Write `channel.send(7)!` where a closed
channel would be a bug, or `match` on the result where it can happen.

::: details Coming from Rust
`Channel<T>` is closest to `std::sync::mpsc::sync_channel(n)`, a bounded channel. What differs:

- there is one handle type, not a `Sender` and a `Receiver`, and any handle may do both, so a channel has many
  senders and many receivers;
- the channel does not close when the last sender is dropped, because no handle is only a sender: call `close()`;
- `recv()` returns `T?`, not `Result<T, RecvError>`, and `send` returns the value in `Err` as Rust's does;
- a capacity of 0 (a rendezvous channel) is not available.
:::

::: details Coming from Go
`Channel<T>.create(n)` is `make(chan T, n)` with `n` at least 1, `send` is `ch <- v`, and `recv()` is `v, ok := <-ch`
with `null` for `ok == false`. Sending on a closed channel returns an error instead of panicking. There is no
`select` over several channels for threads; tasks have `Async.race` (chapter 17).
:::

::: details Coming from Java or C#
A `Channel<T>` is a bounded blocking queue (`ArrayBlockingQueue`, `BlockingCollection`) that can be closed. What it
adds is the move: after `send(v)` the sender cannot touch `v`, so the receiver is its only user.
:::

## Summary

- `Channel<T>.create(capacity)!` makes a bounded queue; `clone()` makes another handle to it for another thread.
- `send(v)` moves `v` in and waits while the channel is full; on a closed channel it returns `Err(v)`.
- `recv()` waits while the channel is empty and returns `null` once it is closed and empty.
- Only `close()` closes a channel. A receiver that loops until `null` needs someone to call it.
- `trySend`, `tryRecv` and `isClosed` never wait.
- The compiler does not check that a wait ends: a channel that is never closed is a program that never finishes.

Next: [Shared state](./ch16-03-shared-state), for a value that several threads change in place.
