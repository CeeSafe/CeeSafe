---
title: Timeouts, races and cancellation
summary: Async.timeout gives a task a deadline, Async.race keeps the first of two tasks and Async.all needs both, and each cancels a task by making its next wait return an error - with what a cancelled task may still do, Async.shield and Async.checkpoint.
assumes: [ch17-01-tasks-and-await, ch17-02-scopes]
keywords: ["Async.timeout", "Async.deadline", "Async.race", Raced, "Async.all", cancellation, Cancelled, TimedOut, "Async.shield", "Async.checkpoint", "Async.isCancelled", cancellation point]
diagnostics: [N0205]
specification: ["type-system.md §40.4", "type-system.md §40.7"]
---

# Timeouts, races and cancellation

**This section teaches** how to stop waiting: after a time, when another task has the answer, or when a task beside
this one has failed. It assumes async functions ([Tasks and await](./ch17-01-tasks-and-await)) and scopes
([Scopes](./ch17-02-scopes)).

Structured tasks cannot be abandoned: the code that started a task waits for it. So to give up on a task, a
program **cancels** it and then waits for it to return. Three operations do that for you, and the last part of the
section shows what the cancelled task sees.

## A deadline: `Async.timeout`

<span class="listing-caption"><strong>Listing 17-17</strong>, <code>main.cee</code>: The same fetch with a generous deadline and with one it cannot meet.</span>

```ceesafe
enum FetchError {
    TimedOut,
    @from Async(AsyncError),
}

async fn fetch(ms: u64, value: u32) -> Result<u32, FetchError> {
    await Timer.sleep(ms)?;
    return Ok(value);
}

async fn quick() -> Result<u32, FetchError> {
    return await Async.timeout(1000, FetchError.TimedOut, async () => await fetch(5, 7));
}

async fn slow() -> Result<u32, FetchError> {
    return await Async.timeout(20, FetchError.TimedOut, async () => await fetch(5000, 8));
}

fn show(r: Result<u32, FetchError>) {
    match (r) {
        Ok(v) => { Console.writeLine(v); }
        Err(FetchError.TimedOut) => { Console.writeLine("timed out"); }
        Err(FetchError.Async(_)) => { Console.writeLine("failed"); }
    }
}

fn main() {
    show(Async.run(quick()));
    show(Async.run(slow()));
}
```

```text
7
timed out
```

`Async.timeout(ms, timedOut, body)` runs `body`, an async closure that returns `Result<T, E>`, and gives its
result if it returns within `ms` milliseconds. If it does not, the body is cancelled and awaited, and the result is
`Err(timedOut)`. So `timedOut` is a value of the body's own error type: you choose which error means "too late".
The second run above ends after 20 milliseconds, not five seconds.

`Async.deadline(at, timedOut, body)` is the same with a time on the clock of `Timer.now()`, for several steps that
share one deadline: `let at = Timer.now() + 500;`, then each step under `Async.deadline(at, …)`.

## The first of two: `Async.race`

<span class="listing-caption"><strong>Listing 17-18</strong>, <code>main.cee</code>: Two tasks answer the same question. The first answer is kept.</span>

```ceesafe
async fn answer(ms: u64, value: u32) -> Result<u32, AsyncError> {
    await Timer.sleep(ms)?;
    return Ok(value);
}

async fn fastest() -> Result<Raced<u32>, AsyncError> {
    return await Async.race(
        async () => await answer(5000, 10),
        async () => await answer(20, 20),
    );
}

fn main() {
    match (Async.run(fastest())) {
        Ok(raced) => {
            Console.write("child ");
            Console.write(raced.winner);
            Console.write(" won with ");
            Console.writeLine(raced.value);
            if (raced.other.isNull()) {
                Console.writeLine("the other child was cancelled");
            }
        }
        Err(_) => { Console.writeLine("failed"); }
    }
}
```

```text
child 1 won with 20
the other child was cancelled
```

`Async.race(a, b)` runs two async closures of the same result type `Result<T, E>` at once. When the first returns
`Ok`, the other is cancelled and awaited, and the result is a `Raced<T>`: `winner` is 0 or 1, `value` is the
winner's value, and `other` is an optional that holds the loser's value if it also returned `Ok` before it saw the
cancellation ([Optionals](./ch06-03-optionals)). If the first to return is an `Err`, that error is the result.

## Both of two: `Async.all`

<span class="listing-caption"><strong>Listing 17-19</strong>, <code>main.cee</code>: Two loads at once. Both must succeed.</span>

```ceesafe
async fn load(ms: u64, value: u32, into: borrow mut u32) -> Result<void, AsyncError> {
    await Timer.sleep(ms)?;
    into = value;
    return Ok();
}

async fn both() -> Result<u32, AsyncError> {
    var width: u32 = 0;
    var height: u32 = 0;
    await Async.all(
        async () => await load(30, 4, mut width),
        async () => await load(10, 5, mut height),
    )?;
    return Ok(width * height);
}

fn main() {
    match (Async.run(both())) {
        Ok(area) => { Console.writeLine(area); }
        Err(_) => { Console.writeLine("failed"); }
    }
}
```

```text
20
```

`Async.all(a, b)` is a scope of exactly two children: both return `Result<void, E>`, results travel through
captures, and the first `Err` cancels the other. It differs from `Async.scope` in one way that matters: the two
frames are inside the caller's frame, so it allocates nothing and needs no out-of-memory error. The same is true of
`timeout`, `deadline` and `race`. Their closures follow the scope's capture rules, and may be `async move`.

## What a cancelled task sees

::: tip Rule
Cancelling a task sets a flag. From then on, each of the task's waits returns `Err(AsyncError.Cancelled)` at once.
Nothing else happens to the task: it runs on from that `Err`, and it ends by returning.

*Specification: `type-system.md` §40.7.*
:::

A task is cancelled when a sibling in its scope fails, when it loses a race, when its timeout passes, or when the
task that awaits it is cancelled. The places where it can notice are its **cancellation points**: the library's
waits (a sleep, a read, a write, a channel receive). Code between two waits is never interrupted.

So a cancelled task cleans up with ordinary code. `?` returns the error, and the locals of each function are
destroyed as it returns, as on any error. A task that must do something first matches on the error:

<span class="listing-caption"><strong>Listing 17-20</strong>, <code>main.cee</code>: A task is cancelled by a timeout during a long sleep, and what it does next.</span>

```ceesafe
async fn worker(log: borrow mut Vector<StringView>) -> Result<void, AsyncError> {
    log.push("worker: started")!;
    match (await Timer.sleep(5000)) {
        Ok() => { log.push("worker: slept")!; }
        Err(AsyncError.Cancelled) => { log.push("worker: cancelled at its sleep")!; }
        Err(_) => { log.push("worker: other error")!; }
    }
    log.push("worker: still runs to its return")!;
    let again = await Timer.sleep(5000);
    if (again.isErr()) {
        log.push("worker: the next wait fails at once")!;
    }
    await Async.shield(1000, async () => {
        await Timer.sleep(10)?;
        log.push("worker: cleanup inside a shield waited")!;
        return Ok();
    })?;
    return Err(AsyncError.Cancelled);
}

async fn run(log: borrow mut Vector<StringView>) -> Result<void, AsyncError> {
    return await Async.timeout(20, AsyncError.TimedOut, async () => await worker(mut log));
}

fn main() {
    var log = Vector<StringView>.create();
    match (Async.run(run(mut log))) {
        Ok() => { log.push("main: ok")!; }
        Err(AsyncError.TimedOut) => { log.push("main: timed out")!; }
        Err(_) => { log.push("main: failed")!; }
    }
    for (line in log) {
        Console.writeLine(line);
    }
}
```

```text
worker: started
worker: cancelled at its sleep
worker: still runs to its return
worker: the next wait fails at once
worker: cleanup inside a shield waited
main: timed out
```

The flag stays set, so every later wait fails too. That is a problem for cleanup that has to wait: sending a
last message, or writing a file. **`Async.shield(ms, body)`** runs an async closure in which waits ignore the flag,
for at most `ms` milliseconds. After that time the waits inside return `Err(AsyncError.TimedOut)`, so a shield
cannot hold a cancelled task for ever. The worker's own error (`Cancelled`) became `TimedOut` in `main`: when the
deadline has passed, `Async.timeout` reports `timedOut` whatever error the body ended with.

A long computation has no waits, and so no cancellation points. It can ask:

<span class="listing-caption"><strong>Listing 17-21</strong>, <code>main.cee</code>: The first child fails at once, so the second is cancelled before it begins and stops at its first checkpoint.</span>

```ceesafe
enum Stop {
    NoMemory,
    Failed,
    Cancelled,
}

async fn fails() -> Result<void, Stop> {
    return Err(Stop.Failed);
}

async fn crunch(rounds: borrow mut u32) -> Result<void, Stop> {
    for (n in 0..1000000u32) {
        if (n % 1000 == 0) {
            Async.checkpoint(Stop.Cancelled)?;
            rounds += 1;
        }
    }
    return Ok();
}

async fn run() -> u32 {
    var rounds: u32 = 0;
    let _ = await Async.scope(Stop.NoMemory, (s) => {
        s.spawn(async () => await fails());
        s.spawn(async () => await crunch(mut rounds));
    });
    return rounds;
}

fn main() {
    Console.writeLine(Async.run(run()));
}
```

```text
0
```

`Async.checkpoint(error)` is `Err(error)` if the current task is cancelled, and `Ok()` otherwise.
`Async.isCancelled()` gives the flag as a `bool`. Neither waits, so neither is awaited. A checkpoint does not let
other tasks run: a task that computes for a long time keeps its thread, and belongs on another one
([Tasks and threads together](./ch17-05-tasks-and-threads)).

## Mistakes

::: warning Mistake: swallowing the error of a wait
A cancelled task that ignores `Cancelled` and goes on is a task that cannot be stopped.
:::

<span class="listing-caption"><strong>Listing 17-22</strong>, <code>main.cee</code>: A retry loop discards the result of its sleep. The timeout cancels it, and nothing stops. This compiles and misbehaves.</span>

```ceesafe
async fn worker(log: borrow mut Vector<StringView>) -> Result<void, AsyncError> {
    for (attempt in 0..3) {
        let _ = await Timer.sleep(1000);
        log.push("worker: tries again")!;
    }
    return Ok();
}

async fn run(log: borrow mut Vector<StringView>) -> Result<void, AsyncError> {
    return await Async.timeout(20, AsyncError.TimedOut, async () => await worker(mut log));
}

fn main() {
    var log = Vector<StringView>.create();
    match (Async.run(run(mut log))) {
        Ok() => { log.push("main: finished in time")!; }
        Err(_) => { log.push("main: timed out")!; }
    }
    for (line in log) {
        Console.writeLine(line);
    }
}
```

```text
worker: tries again
worker: tries again
worker: tries again
main: finished in time
```

After 20 milliseconds the worker is cancelled. Its first sleep returns `Err`, which `let _ =` throws away, and the
two sleeps after it fail at once. The worker "tries again" three times with no pause between, and returns `Ok`,
which the timeout keeps: a body that returns `Ok` keeps its value even when it is late. Write
`await Timer.sleep(1000)?;` and the worker stops at the first sleep, with `main: timed out`. In a loop that retries
on errors, test for `AsyncError.Cancelled` first and return it.

::: warning Mistake: a timeout value of another type
The second argument of `Async.timeout` is the error to report, not a message.
:::

<span class="listing-caption"><strong>Listing 17-23</strong>, <code>main.cee</code>: The body's error type is <code>AsyncError</code>, and the timeout is given a text. This does not compile.</span>

<!-- ceesafe: fail N0205 -->
```ceesafe
async fn fetch() -> Result<u32, AsyncError> {
    await Timer.sleep(5000)?;
    return Ok(1);
}

async fn run() -> Result<u32, AsyncError> {
    return await Async.timeout(20, "too slow", async () => await fetch());
}

fn main() {
    match (Async.run(run())) {
        Ok(v) => { Console.writeLine(v); }
        Err(_) => { Console.writeLine("failed"); }
    }
}
```

```text
main.cee:7:36: error[N0205]: expected 'AsyncError', found 'StringView'
  |
7 |     return await Async.timeout(20, "too slow", async () => await fetch());
  |                                    ^^^^^^^^^^
```

Pass `AsyncError.TimedOut`, or a variant of your own error enum as Listing 17-17 does.

::: details Coming from Rust
In Rust a future is cancelled by dropping it, at whatever `.await` it had reached, and the code after that point
never runs. Here a cancelled task is told through an `Err`, runs its own error path, and returns. There is no
"cancel safety" to reason about for a wait: a wait either gave its value or returned an error. The price is the
mistake of Listing 17-22, which Rust does not have.
:::

::: details Coming from C# or Go
There is no `CancellationToken` or `context.Context` to pass down: the flag belongs to the task and is inherited
by everything it awaits and spawns. `Async.timeout` is `context.WithTimeout` around one call.
:::

::: details Coming from JavaScript or Python
`Async.race` is `Promise.race`, except that the loser is stopped and has returned before the race does.
Python's `asyncio` raises `CancelledError` inside a task at its `await`; here the wait returns the error as a
value.
:::

## Summary

- `Async.timeout(ms, timedOut, body)` gives the body's result, or `Err(timedOut)` after `ms` milliseconds.
  `Async.deadline` takes a time on `Timer.now()`'s clock.
- `Async.race(a, b)` gives a `Raced<T>` for the first `Ok` and cancels the other. `Async.all(a, b)` needs both
  and stops at the first `Err`. None of the four allocates.
- Cancelling sets a flag. Each later wait of the task returns `Err(AsyncError.Cancelled)` at once, and the task
  returns by its own code. Every cancelled task is awaited.
- `Async.shield(ms, body)` lets cleanup wait for up to `ms` milliseconds. `Async.checkpoint(error)` and
  `Async.isCancelled()` let a computation see the flag.
- Pass the error of a wait on. A task that swallows it does not stop.

Next: [Async I/O](./ch17-04-async-io), the waits the library provides.
