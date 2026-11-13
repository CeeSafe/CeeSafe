---
title: Tasks and await
summary: An async fn is a function whose call is a task, await runs a task to its result while other tasks may use the thread, and Async.run enters async code from main - with errors through ?, async methods, recursion, and the diagnostics N0341, N0342 and N0402.
assumes: [ch03-03-functions, ch09-02-result]
keywords: [async, "async fn", await, task, "Async.run", "Timer.sleep", AsyncConsole, AsyncError, frame, suspend]
diagnostics: [N0341, N0342, N0402]
specification: ["type-system.md §40.1", "type-system.md §40.2", "type-system.md §40.6"]
---

# Tasks and await

**This section teaches** how to write an async function, how to run one, and what happens at an `await`. It assumes
you can write a function ([Functions](./ch03-03-functions)) and return a `Result` and use `?` on one
([Recoverable errors with `Result`](./ch09-02-result)).

An ordinary function runs from its call to its return and keeps its thread for all of that time. An **async
function** may stop on the way, where it has to wait for something, and let other work use the thread until what it
waits for is there. A call of an async function is a **task**.

## An async function and its call

<span class="listing-caption"><strong>Listing 17-1</strong>, <code>main.cee</code>: Two async functions. <code>main</code> runs the outer one, which awaits the inner one three times.</span>

```ceesafe
async fn double(x: i32) -> i32 {
    return x * 2;
}

async fn sumOfDoubles(values: Slice<i32>) -> i32 {
    var sum = 0;
    for (v in values) {
        sum += await double(v);
    }
    return sum;
}

fn main() {
    let values = [1, 2, 3];
    let total = Async.run(sumOfDoubles(values[..]));
    Console.writeLine(total);
}
```

```text
12
```

- `async fn` before a function makes it async. Its parameters, result type and body are written as for any
  function.
- `await double(v)` runs the task `double(v)` until it returns, and its value is what the task returned: an `i32`
  here. `await` is allowed only inside async code.
- `Async.run(task)` does the same from ordinary code: it runs the task on this thread until it returns, and gives
  its result. `main` is an ordinary function, so a program enters async code through `Async.run`. The compiler
  refuses an `async fn main` (N0219): no task would call it.

::: tip Rule
A call of an async fn is a task. A task runs only when it is awaited, passed to `Async.run`, or spawned in a scope.
There is no type of tasks: a task cannot be put in a variable, passed to a function or returned.

*Specification: `type-system.md` §40.1.*
:::

The second half of the rule is what makes tasks structured. Since nothing can hold a task that is still running,
the code that started a task always waits for it. [Scopes](./ch17-02-scopes) shows how tasks run at the same time
under that rule.

## What `await` does

`await f()` starts `f` and does not go on until `f` has returned. In that it is a function call. The difference
is in what the thread does while `f` waits: it runs other tasks, if there are any. With one task there are none,
and the program reads from top to bottom.

<span class="listing-caption"><strong>Listing 17-2</strong>, <code>main.cee</code>: Two awaits, one after the other. The slow one is first and finishes first.</span>

```ceesafe
async fn brew(what: StringView, ms: u64) -> Result<void, AsyncError> {
    await Timer.sleep(ms)?;
    await AsyncConsole.writeLine(what)?;
    return Ok();
}

async fn breakfast() -> Result<void, AsyncError> {
    await brew("tea", 100)?;
    await brew("toast", 20)?;
    return Ok();
}

fn main() {
    match (Async.run(breakfast())) {
        Ok() => { Console.writeLine("served"); }
        Err(_) => { Console.writeLine("failed"); }
    }
}
```

```text
tea
toast
served
```

`Timer.sleep(ms)` is an async function of the library: its task returns after `ms` milliseconds.
`AsyncConsole.writeLine` writes a line; async code uses it in place of `Console.writeLine`, which may block the
thread ([Async I/O](./ch17-04-async-io) explains). Both return `Result<void, AsyncError>`, because a wait can fail:
the write may fail, and any wait may be cancelled
([Timeouts, races and cancellation](./ch17-03-timeouts-races-cancellation)). `AsyncError` is an enum with the
variants `Cancelled`, `TimedOut` and `Io(IoError)`.

`await` binds tighter than `?`, so `await Timer.sleep(ms)?` awaits the task and then applies `?` to its `Result`.
The whole breakfast takes 120 milliseconds: the toast does not start until the tea is done. Running them at the
same time needs a scope, in the next section.

::: tip Rule
The current task runs an awaited task to its end. While that task waits, other tasks of the same thread may run.
Nothing but an `await` suspends a task: the code between two awaits runs without interruption.

*Specification: `type-system.md` §40.2.*
:::

An `await` is therefore a **suspension point**: a place where the task may stop and be resumed later.

## Borrows, methods and recursion

An async function takes borrows as any function does, and may hold a borrow across an `await`. A method may be
async too.

<span class="listing-caption"><strong>Listing 17-3</strong>, <code>main.cee</code>: An async method that changes its receiver after a wait.</span>

```ceesafe
struct Meter {
    total: u64;
}

impl Meter {
    async fn add(borrow mut this, amount: u64) -> Result<void, AsyncError> {
        await Timer.sleep(1)?;
        this.total += amount;
        return Ok();
    }
}

async fn read() -> Result<u64, AsyncError> {
    var meter = Meter { total: 0 };
    await meter.add(5)?;
    await meter.add(7)?;
    return Ok(meter.total);
}

fn main() {
    match (Async.run(read())) {
        Ok(total) => { Console.writeLine(total); }
        Err(_) => { Console.writeLine("failed"); }
    }
}
```

```text
12
```

This is safe because of where a task's locals live. Each task has a **frame**: a block of memory, of a size the
compiler knows, for its parameters and the locals it needs across a wait. The frame of an awaited task is inside
the frame of the task that awaits it, and a frame never moves. So `await` allocates nothing, and a borrow of a
local stays valid while its task waits. The borrow checker treats `await f(x)` as it treats the call `f(x)`.

An interface may declare an async method, and then every implementation's method is async
([Interfaces](./ch10-02-interfaces)). An async function may also await itself:

<span class="listing-caption"><strong>Listing 17-4</strong>, <code>main.cee</code>: Recursion through <code>await</code>, written as recursion always is.</span>

```ceesafe
async fn depth(n: u32) -> Result<u32, AsyncError> {
    if (n == 0) {
        return Ok(0);
    }
    await Timer.sleep(1)?;
    return Ok(1 + await depth(n - 1)?);
}

fn main() {
    match (Async.run(depth(5))) {
        Ok(d) => { Console.writeLine(d); }
        Err(_) => { Console.writeLine("failed"); }
    }
}
```

```text
5
```

A frame cannot contain itself, so for a recursive `await` the callee's frame comes from a stack of frames that the
task keeps, its **frame stack**, which grows as a call stack does.

## Mistakes

::: warning Mistake: calling an async function without `await`
In JavaScript, C# or Rust the call gives a value to await later. In CeeSafe there is no such value.
:::

<span class="listing-caption"><strong>Listing 17-5</strong>, <code>main.cee</code>: A task is put in a variable. This does not compile.</span>

<!-- ceesafe: fail N0342 -->
```ceesafe
async fn double(x: i32) -> i32 => x * 2;

fn main() {
    let pending = double(21);
    Console.writeLine("nothing ran");
}
```

```text
main.cee:4:19: error[N0342]: a call of the async fn 'double' is a task: await it, or pass it to 'Async.run' (type-system.md §40.1)
  |
4 |     let pending = double(21);
  |                   ^^^^^^^^^^
```

In `main`, write `let result = Async.run(double(21));`. In async code, write `let result = await double(21);`.

::: warning Mistake: `await` in an ordinary function
Only async code can wait, so only async code can `await`.
:::

<span class="listing-caption"><strong>Listing 17-6</strong>, <code>main.cee</code>: <code>show</code> is not async and awaits. This does not compile.</span>

<!-- ceesafe: fail N0341 -->
```ceesafe
async fn double(x: i32) -> i32 => x * 2;

fn show(x: i32) {
    Console.writeLine(await double(x));
}

fn main() {
    show(21);
}
```

```text
main.cee:4:23: error[N0341]: 'await' is allowed only in the body of an async fn or async closure (type-system.md §40.2)
  |
4 |     Console.writeLine(await double(x));
  |                       ^^^^^
```

Make `show` async, and then its callers must await it or run it. That is how async spreads up a program: from the
function that waits to `main`, where one `Async.run` ends it. The other way round needs no change: async code calls
ordinary functions freely, as long as they do not block.

::: warning Mistake: discarding the `Result` of a wait
A wait that fails says so in its `Result`. A statement that ignores it loses a cancellation.
:::

<span class="listing-caption"><strong>Listing 17-7</strong>, <code>main.cee</code>: The sleep's <code>Result</code> is not used. The compiler warns.</span>

<!-- ceesafe: warns N0402 -->
```ceesafe
async fn pause() {
    await Timer.sleep(1);
}

fn main() {
    Async.run(pause());
    Console.writeLine("done");
}
```

```text
main.cee:2:5: warning[N0402]: the 'Result' of this call is not used, so an error it reports is lost: handle it with '?', '!' or 'match', or write 'let _ = ...;' to discard it
  |
2 |     await Timer.sleep(1);
  |     ^^^^^^^^^^^^^^^^^^^^
```

Return a `Result` from the async function and write `await Timer.sleep(1)?;`, as Listing 17-2 does. This matters
more in async code than elsewhere: the next section but one shows a task that ignores these errors and so cannot be
stopped.

::: details Coming from Rust
An `async fn` call is not a `Future` value: there is nothing to store, `join!` or drop. `await` is a prefix
(`await f()`), not `f().await`. There is no executor to choose and no `Pin`: frames never move, so a borrow across
an `await` needs no extra rule. Cancellation is not a drop at an `.await`: it is an `Err` that the wait returns.
:::

::: details Coming from JavaScript, TypeScript, C# or Python
A call without `await` does not start the work "in the background" and give a promise or a `Task`. It is a compile
error (N0342). To run two things at once, use a scope. There is no `async` `main` (N0219): `main` calls `Async.run`.
:::

::: details Coming from Go
A task is like a goroutine that its caller always waits for. There is no `go f()` that returns at once: the nearest
thing is a child of a scope, which the scope waits for.
:::

## Summary

- `async fn` makes a function whose call is a task. `await task` runs it and gives its result, in async code only
  (`N0341`). `Async.run(task)` does so from ordinary code such as `main`.
- A task is not a value. A call of an async fn that is not awaited, run or spawned is `N0342`.
- While an awaited task waits, other tasks of the thread may run. Only an `await` suspends a task.
- The library's waits return `Result<T, AsyncError>`. Pass the error on with `?`; ignoring it is warning `N0402`.
- A task's locals are in its frame, which never moves, so borrows may be held across an `await`.

Next: [Scopes](./ch17-02-scopes), where several tasks run at the same time.
