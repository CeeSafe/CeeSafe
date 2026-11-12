---
title: Scopes
summary: Async.scope starts child tasks that run at the same time and returns when all have returned, so children may borrow the caller's locals - with the rules for their captures, async move, how one error cancels the rest, and task sets for children started later.
assumes: [ch17-01-tasks-and-await, ch04-02-borrowing]
keywords: ["Async.scope", spawn, child, structured concurrency, capture, "async move", "Async.tasks", TaskSet, trySpawn, "Async.scopeIn"]
diagnostics: [N0241, N0344, N0349]
specification: ["type-system.md §40.3", "type-system.md §40.5"]
---

# Scopes

**This section teaches** how to run several tasks at the same time. It assumes you can write and await an async
function ([Tasks and await](./ch17-01-tasks-and-await)) and know the borrowing rule
([Borrowing](./ch04-02-borrowing)). It uses closures ([Closures](./ch13-01-closures)): `(x) => body` is a function
written in place, which can use the locals around it. Using a local so is called **capturing** it.

`await` runs one task and waits for it. A **scope** starts several tasks, its **children**, and waits for all of
them. While one child waits, the others run.

## A scope and its children

<span class="listing-caption"><strong>Listing 17-8</strong>, <code>main.cee</code>: Two children of one scope. Both wait at the same time, so the quick one finishes first.</span>

```ceesafe
async fn brew(what: StringView, ms: u64) -> Result<void, AsyncError> {
    await Timer.sleep(ms)?;
    await AsyncConsole.writeLine(what)?;
    return Ok();
}

async fn breakfast() -> Result<void, AsyncError> {
    await Async.scope(AsyncError.Cancelled, (s) => {
        s.spawn(async () => await brew("tea", 100));
        s.spawn(async () => await brew("toast", 20));
    })?;
    await AsyncConsole.writeLine("served")?;
    return Ok();
}

fn main() {
    match (Async.run(breakfast())) {
        Ok() => { Console.writeLine("done"); }
        Err(_) => { Console.writeLine("failed"); }
    }
}
```

```text
toast
tea
served
done
```

- `Async.scope(oom, (s) => { … })` is a task, so it is awaited. Its second argument is the scope's **body**, which
  receives the scope as `s`.
- `s.spawn(async () => …)` starts a child. `async () => body` is an **async closure**: a closure whose call is a
  task. It is written only where a child is wanted, as here.
- The body ends as soon as it has spawned the children. The scope then waits. Its `await` returns when every child
  has returned, so `served` comes after both.
- The whole takes about 100 milliseconds, not 120.

Every child returns `Result<void, E>`, with one error type `E` for all children of the scope, and the scope's
result is `Result<void, E>`. The first argument, `oom`, is the error the scope reports if it has no memory for a
child's frame: children's frames come from the heap, since their number is not known when the program is compiled.
Here `E` is `AsyncError`, and `AsyncError.Cancelled` serves as that value.

::: tip Rule
A scope returns only after every child it started has returned. Its result is `Ok()` if every child returned `Ok`,
and otherwise the first `Err` a child returned.

*Specification: `type-system.md` §40.5.*
:::

## Children borrow from the function that awaits

A child returns no value, so its results travel through what it captures. Because the scope does not return while
a child runs, a child may borrow the locals of the function that awaits the scope.

<span class="listing-caption"><strong>Listing 17-9</strong>, <code>main.cee</code>: Each child writes its result to a local of <code>both</code>.</span>

```ceesafe
async fn measure(text: StringView, ms: u64) -> Result<usize, AsyncError> {
    await Timer.sleep(ms)?;
    return Ok(text.length);
}

async fn both() -> Result<usize, AsyncError> {
    var left: usize = 0;
    var right: usize = 0;
    await Async.scope(AsyncError.Cancelled, (s) => {
        s.spawn(async () => {
            left = await measure("structured", 30)?;
            return Ok();
        });
        s.spawn(async () => {
            right = await measure("tasks", 10)?;
            return Ok();
        });
    })?;
    return Ok(left + right);
}

fn main() {
    match (Async.run(both())) {
        Ok(total) => { Console.writeLine(total); }
        Err(_) => { Console.writeLine("failed"); }
    }
}
```

```text
15
```

In a child, `?` and `return` return from the child, not from `both`. After the scope, `left` and `right` belong
to `both` alone again.

The borrowing rule, many readers or one writer, holds between children as it holds everywhere:

::: tip Rule
What a child captures is borrowed from its `spawn` until the scope returns. Two children may both read a local.
If one child changes a local, no other child and no later statement of the body may use it.

*Specification: `type-system.md` §40.5, the scope-loan rule.*
:::

Children run on the scope's own thread, one at a time, and swap only at an `await`. So there is no data race to
fear, and nothing a child captures needs to be `Send` or `Share` ([Send and Share](./ch16-04-send-and-share)). The rule is there for the same
reason as in one function: a child that holds a borrow across a wait must find the value unchanged after it.

A loop may spawn a child for each item. A child keeps its own copy of a `Copy` local that it only reads, so each
child below has its own `x` and `i`, and each changes a different element of `results`.

<span class="listing-caption"><strong>Listing 17-10</strong>, <code>main.cee</code>: Four children, started in a loop.</span>

```ceesafe
async fn square(x: u64, into: borrow mut u64) -> Result<void, AsyncError> {
    await Timer.sleep(x)?;
    into = x * x;
    return Ok();
}

async fn squares() -> Result<u64, AsyncError> {
    var results = [0u64; 4];
    await Async.scope(AsyncError.Cancelled, (s) => {
        for (i in 0..4) {
            let x = (i as u64) + 1;
            s.spawn(async () => await square(x, mut results[i]));
        }
    })?;
    return Ok(results[0] + results[1] + results[2] + results[3]);
}

fn main() {
    match (Async.run(squares())) {
        Ok(sum) => { Console.writeLine(sum); }
        Err(_) => { Console.writeLine("failed"); }
    }
}
```

```text
30
```

## When a child fails

The first child to return `Err` **cancels** its siblings: their next wait returns `Err(AsyncError.Cancelled)`, and
they return. The scope still waits for all of them, and then its result is that first error.

<span class="listing-caption"><strong>Listing 17-11</strong>, <code>main.cee</code>: The second job fails after 50 milliseconds. The third, which would sleep for five seconds, is cancelled.</span>

```ceesafe
enum JobError {
    NoMemory,
    BadInput(u32),
    @from Async(AsyncError),
}

async fn job(id: u32, ms: u64) -> Result<void, JobError> {
    await Timer.sleep(ms)?;
    if (id == 2) {
        return Err(JobError.BadInput(id));
    }
    await AsyncConsole.writeLine(if (id == 1) "job 1 done" else "job 3 done")?;
    return Ok();
}

async fn runAll() -> Result<void, JobError> {
    return await Async.scope(JobError.NoMemory, (s) => {
        s.spawn(async () => await job(1, 10));
        s.spawn(async () => await job(2, 50));
        s.spawn(async () => await job(3, 5000));
    });
}

fn main() {
    match (Async.run(runAll())) {
        Ok() => { Console.writeLine("all jobs done"); }
        Err(JobError.BadInput(id)) => {
            Console.write("job ");
            Console.write(id);
            Console.writeLine(" had bad input");
        }
        Err(_) => { Console.writeLine("failed"); }
    }
}
```

```text
job 1 done
job 2 had bad input
```

The `@from` variant lets `?` turn an `AsyncError` into a `JobError`
([Recoverable errors with `Result`](./ch09-02-result)). Job 3's sleep returns `Cancelled`, its `?` returns that as
`JobError.Async(…)`, and the scope, which already has job 2's error, drops it. The program ends after 50
milliseconds. [Timeouts, races and cancellation](./ch17-03-timeouts-races-cancellation) says more about what a
cancelled task may do.

## Task sets: children started later

A scope's children are spawned in its body. A server wants something else: a task for each connection, started
when the connection arrives, from a loop that may be in another function. That is a **task set**.

<span class="listing-caption"><strong>Listing 17-12</strong>, <code>main.cee</code>: Five tasks started through a set, at most two at a time. One fails and the others finish.</span>

```ceesafe
enum WorkError {
    NoMemory,
    Failed(u32),
    @from Async(AsyncError),
}

async fn work(n: u32, done: Arc<AtomicU32>) -> Result<void, WorkError> {
    await Timer.sleep(10)?;
    if (n == 3) {
        return Err(WorkError.Failed(n));
    }
    done.get().fetchAdd(1);
    return Ok();
}

async fn startAll(set: borrow TaskSet<WorkError>, done: borrow Arc<AtomicU32>) -> Result<void, WorkError> {
    for (n in 0..5u32) {
        let mine = done.clone();
        await set.spawn(async move () => await work(n, mine))?;
    }
    return Ok();
}

async fn run(done: borrow Arc<AtomicU32>) -> Result<void, WorkError> {
    return await Async.tasks(2, WorkError.NoMemory, async (set) => await startAll(set, done));
}

fn main() {
    let done = Arc<AtomicU32>.create(AtomicU32.create(0))!;
    match (Async.run(run(borrow done))) {
        Ok() => { Console.writeLine("all done"); }
        Err(WorkError.Failed(n)) => {
            Console.write("task ");
            Console.write(n);
            Console.writeLine(" failed");
        }
        Err(_) => { Console.writeLine("no memory, or cancelled"); }
    }
    Console.write(done.get().load());
    Console.writeLine(" tasks finished");
}
```

```text
task 3 failed
4 tasks finished
```

- `Async.tasks(limit, oom, async (set) => …)` runs its body with the set, then waits for every task the set
  started. The error type is `oom`'s, and must be `Copy`.
- `await set.spawn(async move () => …)` starts a task. While `limit` tasks run, it waits for one to return, which
  is how a server keeps its load bounded. `set.trySpawn(…)` does not wait: it returns `false` instead.
- A task's error does **not** cancel the others. The set keeps the first error and reports it at the end. An error
  of the body cancels them all.
- A set's task may outlive the function that spawned it (`startAll` here), so it cannot borrow. It is an
  `async move` closure, which **owns** what it captures: each capture is moved or copied in. Tasks share through
  an `Arc` (chapter 15) or a channel ([Channels](./ch16-02-channels)). Here each task owns a clone of an `Arc`
  of an atomic counter.

## Mistakes

::: warning Mistake: two children changing one local
Each child needs its own place to write.
:::

<span class="listing-caption"><strong>Listing 17-13</strong>, <code>main.cee</code>: Both children borrow <code>hits</code> mutably. This does not compile.</span>

<!-- ceesafe: fail N0241 -->
```ceesafe
async fn count(ms: u64, hits: borrow mut u32) -> Result<void, AsyncError> {
    await Timer.sleep(ms)?;
    hits += 1;
    return Ok();
}

async fn both() -> Result<u32, AsyncError> {
    var hits: u32 = 0;
    await Async.scope(AsyncError.Cancelled, (s) => {
        s.spawn(async () => await count(10, mut hits));
        s.spawn(async () => await count(20, mut hits));
    })?;
    return Ok(hits);
}

fn main() {
    match (Async.run(both())) {
        Ok(hits) => { Console.writeLine(hits); }
        Err(_) => { Console.writeLine("failed"); }
    }
}
```

```text
main.cee:11:17: error[N0241]: this child captures 'hits' mutably, but an earlier child of this scope captures it too (type-system.md §40.5)
   |
11 |         s.spawn(async () => await count(20, mut hits));
   |                 ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
```

Give each child a local of its own and add them after the scope, as Listing 17-9 does, or share one value through
a type made for sharing: a `Cell` (chapter 15) is enough between children of one thread.

::: warning Mistake: a child that borrows a local of the scope's body
The body ends when it has spawned the children. Its locals end with it, while the children still run.
:::

<span class="listing-caption"><strong>Listing 17-14</strong>, <code>main.cee</code>: <code>name</code> belongs to the body, and the child borrows it. This does not compile.</span>

<!-- ceesafe: fail N0344 -->
```ceesafe
async fn show(text: StringView) -> Result<void, AsyncError> {
    return await AsyncConsole.writeLine(text);
}

async fn run() -> Result<void, AsyncError> {
    await Async.scope(AsyncError.Cancelled, (s) => {
        var name = String.from("local to the body")!;
        s.spawn(async () => await show(name.view()));
    })?;
    return Ok();
}

fn main() {
    Async.run(run())!;
}
```

```text
main.cee:8:17: error[N0344]: this child captures 'name', which ends with the scope's body while the child may still run; a child holds its own copy only of a Copy value it does not change and that holds no borrow (type-system.md §40.5)
  |
8 |         s.spawn(async () => await show(name.view()));
  |                 ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
```

Declare the local before the scope, so that it outlives the children. Or give it to the child: an `async move`
closure moves what it captures into the child's frame, and the child destroys it when it returns.

<span class="listing-caption"><strong>Listing 17-15</strong>, <code>main.cee</code>: Each child owns the <code>String</code> made for it.</span>

```ceesafe
async fn show(text: StringView) -> Result<void, AsyncError> {
    return await AsyncConsole.writeLine(text);
}

async fn run() -> Result<void, AsyncError> {
    await Async.scope(AsyncError.Cancelled, (s) => {
        for (n in 1..4) {
            var line = String.from("child ")!;
            line.appendNumber(n)!;
            s.spawn(async move () => await show(line.view()));
        }
    })?;
    return Ok();
}

fn main() {
    Async.run(run())!;
}
```

```text
child 1
child 2
child 3
```

::: warning Mistake: a task of a set that borrows
A scope's child may borrow from the function that awaits the scope. A set's task may not borrow at all.
:::

<span class="listing-caption"><strong>Listing 17-16</strong>, <code>main.cee</code>: The task captures a <code>StringView</code>, which is a borrow of <code>greeting</code>. This does not compile.</span>

<!-- ceesafe: fail N0349 -->
```ceesafe
async fn show(text: StringView) -> Result<void, AsyncError> {
    return await AsyncConsole.writeLine(text);
}

async fn run() -> Result<void, AsyncError> {
    let greeting = String.from("hello")!;
    return await Async.tasks(4, AsyncError.Cancelled, async (set) => {
        let view = greeting.view();
        await set.spawn(async move () => await show(view))?;
        return Ok();
    });
}

fn main() {
    Async.run(run())!;
}
```

```text
main.cee:9:25: error[N0349]: this child captures 'view', of type 'StringView', which holds a loan; a task set's child may outlive the code that starts it, so it captures owned or Copy values only: share through Arc, or pass values down (ADR 0053 §3.2)
  |
9 |         await set.spawn(async move () => await show(view))?;
  |                         ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
```

Move an owned `String` into the task, as Listing 17-15 does for a scope's child. When the tasks can all be spawned
in one place, use a scope: its children may borrow.

Two smaller edges. `return`, `?`, `break` and `continue` cannot leave a scope's body (N0214): the children would
still be running. And `Async.scopeIn(alloc, oom, body)` is `Async.scope` with the children's frames taken from an
allocator of your choice (chapter 15).

::: details Coming from Rust
`Async.scope` is what `tokio::spawn` cannot be: children borrow from the caller with no `'static` bound and no
`Arc`, because the scope outlives them, as `std::thread::scope` does for threads. `Async.tasks` is the nearest thing
to a `JoinSet`, and its tasks own their captures, as spawned futures do.
:::

::: details Coming from JavaScript, C# or Python
A scope is `Promise.all`, `Task.WhenAll` or a `TaskGroup` with the tasks started inside it. There is no way to
start a task and forget it: every task has an owner that waits for it.
:::

::: details Coming from Go
A scope is a `sync.WaitGroup` and an `errgroup` that the compiler will not let you forget to wait on, and a task
set with a limit is a group with a semaphore.
:::

## Summary

- `await Async.scope(oom, (s) => { s.spawn(async () => …); … })` starts children that run at the same time, and
  returns when all have returned.
- A child returns `Result<void, E>`. The first `Err` cancels the other children and is the scope's result.
- A child borrows from the function that awaits the scope, never from the scope's body (`N0344`). Two children
  cannot share a local that one of them changes (`N0241`).
- A child keeps its own copy of a `Copy` local it only reads. `async move () => …` moves its captures into the
  child.
- `Async.tasks(limit, oom, async (set) => …)` gives a set whose tasks are started later with
  `await set.spawn(async move () => …)`. They own what they capture (`N0349`), and one's error does not stop the
  others.

Next: [Timeouts, races and cancellation](./ch17-03-timeouts-races-cancellation).
