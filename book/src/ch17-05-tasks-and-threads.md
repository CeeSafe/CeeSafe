---
title: Tasks and threads together
summary: Tasks share one thread unless you ask for more - Async.blocking moves a blocking or long call to a worker thread, Async.runWorkers with spawnSend spreads a scope's children over several threads, and channels join threads to tasks, with what must be Send or Share.
assumes: [ch17-02-scopes, ch17-04-async-io]
keywords: ["Async.blocking", "Async.runWorkers", spawnSend, worker, work stealing, Send, Share, Channel, AsyncMutex, Mutex, executor, thread]
diagnostics: [N0295, N0334]
specification: ["type-system.md §40.4", "type-system.md §40.5", "type-system.md §40.8"]
---

# Tasks and threads together

**This section teaches** how async code uses more than one thread: for a call that blocks, for a computation, and
for a scope whose children should run in parallel. It assumes scopes ([Scopes](./ch17-02-scopes)) and the rule
that async code may not block ([Async I/O](./ch17-04-async-io)). Threads, `Send` and `Share`, `Mutex`, atomics and
channels are the subject of chapter 16 ([Threads](./ch16-00-threads)): this section uses them and says in a
sentence what each is.

`Async.run` runs its task, and every task that task starts, on the thread that called it. The part of the library
that does this, choosing which task runs next and waiting on the operating system when none can, is the
**executor**. One thread is enough for work that waits. It is not enough when something must block, or when there
is computing to do: a task that computes for a second keeps the thread for a second, and every other task waits.

## A blocking call: `Async.blocking`

<span class="listing-caption"><strong>Listing 17-30</strong>, <code>main.cee</code>: A long computation runs on a worker thread while a timer task runs beside it.</span>

```ceesafe
fn checksum(limit: u64) -> u64 {
    var total: u64 = 0;
    for (i in 0..limit) {
        total = (total * 31 + i) % 1000003;
    }
    return total;
}

async fn run() -> Result<u64, AsyncError> {
    var ticks: u32 = 0;
    var sum: u64 = 0;
    await Async.all(
        async () => {
            sum = await Async.blocking(() => checksum(2000000));
            return Ok();
        },
        async () => {
            await Timer.sleep(1)?;
            ticks += 1;
            return Ok();
        },
    )?;
    await Async.blocking(() => { Console.writeLine("printed by a worker thread"); });
    return Ok(sum + ticks as u64);
}

fn main() {
    match (Async.run(run())) {
        Ok(v) => { Console.writeLine(v); }
        Err(_) => { Console.writeLine("failed"); }
    }
}
```

```text
printed by a worker thread
235198
```

`await Async.blocking(() => …)` runs an ordinary closure on a **worker thread** and waits for it as for any task:
other tasks run meanwhile. Its value is the closure's result. Inside the closure the code is not async, so it may
block: read a `File`, lock a `Mutex`, call a C library, write with `Console`.

The closure runs on another thread while the task's frame stays where it is, so the rule for threads applies to
what it captures: a value it reads must be `Share`, a value it changes must be `Send`, and its result must be
`Send`. [Send and Share](./ch16-04-send-and-share) teaches the two; plain data such as numbers, strings and vectors is both.

One thing does not carry over. A cancelled task still waits for its worker: a thread in the middle of a call
cannot be stopped from outside.

## A scope on several threads

<span class="listing-caption"><strong>Listing 17-31</strong>, <code>main.cee</code>: Four children share out a vector. The result is the same on one thread and on four.</span>

```ceesafe
fn weigh(values: borrow Vector<u64>, part: usize, parts: usize) -> u64 {
    var total: u64 = 0;
    var i = part;
    while (i < values.length) {
        total += values[i] % 7;
        i += parts;
    }
    return total;
}

async fn fanOut(values: borrow Vector<u64>) -> Result<u64, AsyncError> {
    let total = AtomicU64.create(0);
    await Async.scope(AsyncError.Cancelled, (s) => {
        for (part in 0..4) {
            s.spawnSend(async () => {
                total.fetchAdd(weigh(values, part, 4));
                return Ok();
            });
        }
    })?;
    return Ok(total.load());
}

fn main() {
    var values = Vector<u64>.create();
    for (i in 0..1000u64) {
        values.push(i * i)!;
    }
    Console.writeLine(Async.run(fanOut(borrow values))!);
    Console.writeLine(Async.runWorkers(4, fanOut(borrow values))!);
}
```

```text
2001
2001
```

- `Async.runWorkers(n, task)` is `Async.run` with `n` threads: the calling thread and `n - 1` more, started for
  the run and joined when it returns. Each is a **worker** with its own queue of tasks.
- `s.spawnSend(async () => …)` is `s.spawn` for a child that any worker may run. A worker with nothing to do
  takes a child that another worker has not started yet: this is **work stealing**.
- A child moves only before it starts. Once a worker starts it, the child and everything it awaits and spawns
  stay on that worker, so a frame never crosses threads and borrows inside a task stay as simple as before.
- The children still borrow from `fanOut`: they read `values` and add to `total`. Because they may now run at the
  same time, what they read must be `Share` and what they change must be `Send`. A `Vector<u64>` that is only read
  is `Share`. An `AtomicU64` can be changed through a shared borrow from several threads at once.

::: tip Rule
A `spawn` child runs on its scope's thread, and nothing it captures needs `Send` or `Share`. A `spawnSend` child
may run on another worker: what it reads must be `Share`, what it changes must be `Send`, and its error type must
be `Send`.

*Specification: `type-system.md` §40.5.*
:::

Under `Async.run`, which has one worker, `spawnSend` does what `spawn` does. The same is true in
`ceesafec run --interp`, which runs every program on one thread. So write programs whose output does not depend on
which worker ran what, as this one's does not: a sum is the same in any order. A task set has the same pair,
`set.spawn` and `set.spawnSend`.

## Threads and tasks through a channel

A thread and a task can also work side by side with nothing shared but a channel. The thread uses the channel's
blocking calls, `send` and `recv`. The task uses `sendAsync` and `recvAsync`.

<span class="listing-caption"><strong>Listing 17-32</strong>, <code>main.cee</code>: A thread computes squares and sends them. A task on the main thread receives them.</span>

```ceesafe
fn square(n: u64) -> u64 {
    return n * n;
}

async fn collect(results: borrow Channel<u64>) -> Result<u64, AsyncError> {
    var total: u64 = 0;
    loop {
        match (await results.recvAsync()?) {
            Some(v) => { total += v; }
            null => { return Ok(total); }
        }
    }
}

fn main() {
    let results = Channel<u64>.create(16)!;
    let sender = results.clone();
    let worker = Thread.spawn(move () => {
        for (n in 1..11u64) {
            let _ = sender.send(square(n));
        }
        sender.close();
        return 0u64;
    })!;
    Console.writeLine(Async.run(collect(borrow results))!);
    let _ = worker.join();
}
```

```text
385
```

`Thread.spawn` starts a thread that owns a clone of the channel. The task waits in `recvAsync` without blocking, so
it could serve sockets and timers in the same scope while the thread computes.

## Mistakes

::: warning Mistake: a `spawnSend` child that shares a single-thread value
A `Cell` (chapter 15) lets one thread change a value through a shared borrow. It is not `Share`.
:::

<span class="listing-caption"><strong>Listing 17-33</strong>, <code>main.cee</code>: Four children that may run on four threads count in a <code>Cell</code>. This does not compile.</span>

<!-- ceesafe: fail N0334 -->
```ceesafe
async fn fanOut() -> Result<u32, AsyncError> {
    let hits = Cell<u32>.create(0);
    await Async.scope(AsyncError.Cancelled, (s) => {
        for (i in 0..4) {
            s.spawnSend(async () => {
                hits.set(hits.get() + 1);
                return Ok();
            });
        }
    })?;
    return Ok(hits.get());
}

fn main() {
    Console.writeLine(Async.runWorkers(4, fanOut())!);
}
```

```text
main.cee:5:25: error[N0334]: this child reads 'hits', a 'Cell<u32>', which is not Share: a 'spawnSend' child may read it from another thread (type-system.md §38.6, ADR 0038)
  |
5 |             s.spawnSend(async () => {
  |                         ^^^^^^^^^^^^^
```

With `s.spawn` this program compiles and prints 4: the children then run one at a time on one thread, and a `Cell`
is enough. For children on several threads, use an atomic, as Listing 17-31 does.

::: warning Mistake: locking a `Mutex` in async code
A `Mutex` ([Shared state](./ch16-03-shared-state)) makes its thread wait for the lock. That is blocking.
:::

<span class="listing-caption"><strong>Listing 17-34</strong>, <code>main.cee</code>: An async function locks a <code>Mutex</code>. This does not compile.</span>

<!-- ceesafe: fail N0295 -->
```ceesafe
async fn bump(counter: borrow Mutex<u32>) -> Result<void, AsyncError> {
    var guard = counter.lock();
    await Timer.sleep(1)?;
    return Ok();
}

fn main() {
    let counter = Mutex<u32>.create(0)!;
    match (Async.run(bump(borrow counter))) {
        Ok() => {}
        Err(_) => { Console.writeLine("failed"); }
    }
}
```

```text
main.cee:2:5: error[N0295]: 'bump' is an async fn, which must not block its thread (type-system.md §40.1), but this may sleep: it calls 'Mutex<u32>.lock', which calls 'ceesafe_mutex_lock', declared @effects(sleep)
  |
2 |     var guard = counter.lock();
  |     ^^^^^^^^^^^^^^^^^^^^^^^^^^^
```

The message gives the chain of calls that ends in the blocking one. There are three ways out, by what the lock is
for:

- Between tasks, use an `AsyncMutex<T>`: `var guard = await m.lock()?;`. A task that finds it locked waits without
  blocking, and the guard may be held across an `await`. Waiting tasks get the lock in the order they asked.
- For a short update shared with threads, lock the `Mutex` inside `Async.blocking(() => …)`, or use an atomic. A
  task may also call `tryLock()`, which never waits and returns `null` when the lock is held.
- Or keep the state in one task and send it messages through a channel.

::: details Coming from Rust
There are no `Send` bounds on ordinary async code, because a task never changes thread after it starts. Only what
crosses at a `spawnSend` or an `Async.blocking` is checked, where it crosses. `Async.blocking` is
`spawn_blocking` that may borrow.
:::

::: details Coming from Go
Go's scheduler moves goroutines between threads at any time and lets any of them block. Here a task stays on its
worker and may not block, and you say which children may be placed on other workers (`spawnSend`) and which calls
block (`Async.blocking`).
:::

::: details Coming from JavaScript or Python
`Async.run` is the single event loop you know. `Async.blocking` is a thread pool for calls that would stall it, and
`Async.runWorkers` is several loops that share out the children of a scope.
:::

## Summary

- `Async.run` uses one thread for all its tasks. A task that blocks or computes for long holds up the others.
- `await Async.blocking(() => …)` runs ordinary code on a worker thread while the task waits. It reads `Share`
  values, changes `Send` ones, and is not cut short by cancellation.
- `Async.runWorkers(n, task)` runs with `n` workers, and `s.spawnSend` children may start on any of them
  (`N0334` if what they capture may not cross threads). A started task stays on its worker.
- A `Channel<T>` joins threads (`send`, `recv`) and tasks (`sendAsync`, `recvAsync`).
- A blocking lock in async code is `N0295`: use `AsyncMutex<T>`, `Async.blocking`, an atomic or a channel.

This is the end of the chapter. Chapter 18 turns to interfaces as values.
