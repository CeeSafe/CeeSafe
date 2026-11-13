---
title: Starting threads
summary: Starting a thread with Thread.spawn and waiting for its result with join, what a thread's closure may capture (it owns what it uses and cannot borrow), what a panic in a thread does, and Thread.both, which runs two closures that borrow from the caller.
assumes: [ch04-01-what-ownership-is, ch04-02-borrowing, ch08-01-vectors, ch09-02-result]
keywords: [Thread.spawn, join, "Thread<T>", move closure, capture, Thread.both, SpawnError, panic, fork-join]
diagnostics: [N0241, N0250, N0332, N0403]
specification: ["type-system.md §38.2", "type-system.md §38.6", "type-system.md §36.5", "ownership.md T1", "ownership.md T4"]
---

# Starting threads

**This section teaches** how to start a thread, how to wait for it and take its result, what the thread may use
from the function that started it, and what happens when a thread panics. It assumes moves
([What ownership is](./ch04-01-what-ownership-is)), borrows ([Borrowing](./ch04-02-borrowing)), vectors
([Vectors](./ch08-01-vectors)) and the `!` operator on a `Result` ([Result](./ch09-02-result)).

## Spawn and join

`Thread.spawn` starts a new thread. `join` waits for it to finish and returns what it computed.

<span class="listing-caption"><strong>Listing 16-1</strong>, <code>main.cee</code>: A thread adds the numbers from 1 to 100, and <code>main</code> waits for the sum.</span>

```ceesafe
fn main() {
    let worker = Thread.spawn(move () => {
        var sum: u64 = 0;
        for (n in 1..101u64) {
            sum += n;
        }
        return sum;
    })!;
    let total = worker.join();
    Console.writeLine(total);
}
```

```text
5050
```

- The argument of `Thread.spawn` is a **closure**: a function written in place, here with no parameters,
  `() => { … }`. Chapter 13 teaches closures. The word `move` before it says that the closure owns everything it
  uses from the surrounding function.
- The new thread runs the closure's body. `main` goes on at once, at the same time.
- `Thread.spawn` returns a `Result`: starting a thread needs memory and a thread from the operating system, and
  either can be refused. The error type is `SpawnError`. The `!` panics on an error, as it does for any `Result`.
- The value inside is a `Thread<u64>`: a handle to the running thread, whose type argument is the type the body
  returns.
- `worker.join()` waits until the thread has finished and returns its `u64`. `join` consumes the handle, so a thread
  is joined once.

::: tip Rule
`Thread.spawn(move () => body)` returns `Result<Thread<T>, SpawnError>`, where `T` is the type of the body's result.
`t.join()` takes the handle, waits for the thread and returns its `T`.

*Specification: `type-system.md` §38.2.*
:::

## What a thread owns

A thread may still be running after the function that started it has returned. So it cannot borrow that function's
locals: they could be gone while the thread still reads them. A thread's closure **owns** what it uses. Using a
local inside the closure moves it in, or copies it if its type is `Copy`.

<span class="listing-caption"><strong>Listing 16-2</strong>, <code>main.cee</code>: The vector moves into the thread, which counts the letters of its strings.</span>

```ceesafe
fn main() {
    var names = Vector<String>.create();
    names.push(String.from("ada")!)!;
    names.push(String.from("grace")!)!;
    let worker = Thread.spawn(move () => {
        var letters: usize = 0;
        for (name in names.view()) {
            letters += name.length;
        }
        return letters;
    })!;
    Console.writeLine(worker.join());
}
```

```text
8
```

After the call of `Thread.spawn`, `names` belongs to the thread. `main` cannot use it again, so no value is in two
threads at once. When the thread's body returns, it destroys the vector as any owner does.

::: tip Rule
A thread's closure owns its captures: a value it uses is moved into it (or copied, if it is `Copy`), and it captures
no borrow. Only a value that is safe to move to another thread may be captured, and the result must be safe to move
back.

*Specification: `type-system.md` §36.5, §38.2; `ownership.md` T1.*
:::

"Safe to move to another thread" is a property of the type, named `Send`. Numbers, strings, vectors and structs of
them have it. [Send and Share](./ch16-04-send-and-share) gives the whole list.

The thread's body runs once, so it may also move a capture out, for example to give it back as its result:

```ceesafe
fn main() {
    let text = String.from("one two three")!;
    let worker = Thread.spawn(move () => {
        Console.writeLine(text.length);
        return text;
    })!;
    let back = worker.join();
    Console.writeLine(back);
}
```

```text
13
one two three
```

A capture the body does not move out is destroyed when the body returns, on the thread.

## Several threads

A `Thread<T>` is a value like any other, so several can be kept in a vector and joined in turn.

<span class="listing-caption"><strong>Listing 16-3</strong>, <code>main.cee</code>: Four threads each square a number, and <code>main</code> adds the results.</span>

```ceesafe
fn main() {
    var workers = Vector<Thread<u64>>.create();
    for (i in 1..5u64) {
        workers.push(Thread.spawn(move () => i * i)!)!;
    }
    var total: u64 = 0;
    while (workers.pop() is Some(worker)) {
        total += worker.join();
    }
    Console.writeLine(total);
}
```

```text
30
```

Each closure gets its own copy of `i`, since a `u64` is `Copy`. `workers.pop()` takes a handle out of the vector, so
that `join` can consume it; a `for` loop over the vector would only borrow each handle. The order in which the
threads finish does not matter here: the sum is the same.

## A thread is always joined

There are no detached threads. If a `Thread<T>` is destroyed without `join`, its destruction waits for the thread
and throws its result away.

<span class="listing-caption"><strong>Listing 16-4</strong>, <code>main.cee</code>: The handle goes out of scope at the end of the inner block, and the program waits there for the thread.</span>

```ceesafe
fn main() {
    {
        let quiet = Thread.spawn(move () => {
            Console.writeLine("the thread ran");
        })!;
    }
    Console.writeLine("after the block");
}
```

```text
the thread ran
after the block
```

::: tip Rule
Every thread a program starts has ended before `main` returns: a `Thread<T>` that was not joined is joined when it
is destroyed.

*Specification: `type-system.md` §38.2.*
:::

## A panic in a thread

A panic anywhere ends the whole program, not only the thread it happens in ([Panics](./ch09-01-panics)). `join`
does not return an error for it: there is nothing left to return to.

<span class="listing-caption"><strong>Listing 16-5</strong>, <code>main.cee</code>: The thread reads past the end of an array. The program stops there, and <code>done</code> is never printed.</span>

<!-- ceesafe: panics -->
```ceesafe
fn main() {
    Console.writeLine("starting");
    let worker = Thread.spawn(move () => {
        let values: [u64; 3] = [10, 20, 30];
        var i: usize = 3;
        return values[i];
    })!;
    Console.writeLine(worker.join());
    Console.writeLine("done");
}
```

```text
starting
```

Then, on standard error, with exit status 134:

```text
ceesafe panic: index out of bounds at main.cee:6:23
```

A thread that can fail in a way the program should survive returns a `Result` from its body, and the code that
joins it handles the error.

## Two threads that borrow: `Thread.both`

Moving everything into a thread is not always what you want. To add up a large array on two threads, each thread
should read half of it where it is. `Thread.both(a, b)` allows that. It runs the closure `a` on a new thread and `b`
on the calling thread, and returns only when both have finished. Since the caller waits, its locals outlive both
closures, and the closures may borrow them.

<span class="listing-caption"><strong>Listing 16-6</strong>, <code>main.cee</code>: Two threads each add up half of an array that stays in <code>main</code>.</span>

```ceesafe
fn sum(values: Slice<u64>) -> u64 {
    var total: u64 = 0;
    for (v in values) {
        total += v;
    }
    return total;
}

fn main() {
    let values: [u64; 6] = [1, 2, 3, 4, 5, 6];
    let halves = values[..].splitAt(3);
    var low: u64 = 0;
    var high: u64 = 0;
    Thread.both(() => { low = sum(halves.left); }, () => { high = sum(halves.right); });
    Console.writeLine(low + high);
}
```

```text
21
```

- The closures are not `move` closures: they borrow `halves`, `low` and `high`.
- `Thread.both` returns nothing. Each closure stores its result in a variable of the caller.
- The borrowing rule holds between the two closures as it does anywhere. Both may read a value. If one changes a
  value, the other may not use it. Here the first changes only `low` and the second only `high`.
- If a new thread cannot be started, `Thread.both` runs `a` and then `b` on the calling thread. So it cannot fail,
  and there is no `Result`.

::: tip Rule
`Thread.both(a, b)` takes two closures written in place, with no parameters and no result. They may borrow from the
caller, under the usual rule: many readers or one writer.

*Specification: `type-system.md` §38.6; `ownership.md` T4.*
:::

## Mistakes

::: warning Mistake: using a value after it moved into a thread
The value now belongs to the thread.
:::

<span class="listing-caption"><strong>Listing 16-7</strong>, <code>main.cee</code>: <code>names</code> is read after the thread took it. This does not compile.</span>

<!-- ceesafe: fail N0250 -->
```ceesafe
fn main() {
    var names = Vector<String>.create();
    names.push(String.from("ada")!)!;
    let worker = Thread.spawn(move () => names.length)!;
    Console.writeLine(names.length);
    Console.writeLine(worker.join());
}
```

```text
main.cee:5:23: error[N0250]: 'names' is used here after it was moved (on at least one path)
  |
5 |     Console.writeLine(names.length);
  |                       ^^^^^^^^^^^^
  = note: the value was moved here (4:31)
```

Read what you need before the spawn, or have the thread return it. If both threads need the value, see
[Shared state](./ch16-03-shared-state).

::: warning Mistake: capturing a borrow
A view of a string is a borrow of it. The thread could outlive the string.
:::

<span class="listing-caption"><strong>Listing 16-8</strong>, <code>main.cee</code>: The closure captures a <code>StringView</code>. This does not compile.</span>

<!-- ceesafe: fail N0332 -->
```ceesafe
fn main() {
    let text = String.from("one two three")!;
    let view = text.view();
    let worker = Thread.spawn(move () => view.length)!;
    Console.writeLine(worker.join());
}
```

```text
main.cee:4:31: error[N0332]: this 'move' closure captures 'view', a 'StringView', which holds a loan: the thread could outlive it; move the value itself in, or share it through an 'Arc' (type-system.md §38.2)
  |
4 |     let worker = Thread.spawn(move () => view.length)!;
  |                               ^^^^^^^^^^^^^^^^^^^^^^
```

Capture `text` itself, so the string moves
into the thread and the closure calls `text.view()` there, or use `Thread.both`. Leaving out `move` is no way
around it: `Thread.spawn` accepts only a `move` closure (N0333).

::: warning Mistake: expecting a thread to change a captured number
A `Copy` value is copied into the closure. The thread changes its copy.
:::

<span class="listing-caption"><strong>Listing 16-9</strong>, <code>main.cee</code>: The thread adds 1 to its own copy of <code>hits</code>. The compiler warns.</span>

<!-- ceesafe: warns N0403 -->
```ceesafe
fn main() {
    var hits: u64 = 0;
    let worker = Thread.spawn(move () => {
        hits += 1;
    })!;
    worker.join();
    Console.writeLine(hits);
}
```

```text
main.cee:3:31: warning[N0403]: this 'move' closure changes its own copy of 'hits': the type is Copy, so the closure took a copy, and the 'hits' read after it is unchanged; return the value from the closure, or share it through an atomic, a 'Mutex' or a 'Channel'
  |
3 |     let worker = Thread.spawn(move () => {
  |                               ^^^^^^^^^^^^
```

It is a warning, so the program still runs, and prints 0. The compiler warns only when the variable is used again
after the closure, as `hits` is in the last line. Return the value from the thread and take it from `join`, or use `Thread.both`, whose closures change the caller's
variables.

::: warning Mistake: two closures of `Thread.both` changing one variable
That is two writers at once, which the borrowing rule refuses.
:::

<span class="listing-caption"><strong>Listing 16-10</strong>, <code>main.cee</code>: Both closures add to <code>hits</code>. This does not compile.</span>

<!-- ceesafe: fail N0241 -->
```ceesafe
fn main() {
    var hits: u64 = 0;
    Thread.both(() => { hits += 1; }, () => { hits += 1; });
    Console.writeLine(hits);
}
```

```text
main.cee:3:39: error[N0241]: cannot borrow 'hits' as mutable while 'hits' is mutably borrowed
  |
3 |     Thread.both(() => { hits += 1; }, () => { hits += 1; });
  |                                       ^^^^^^^^^^^^^^^^^^^^
  = note: 'hits' is mutably borrowed here (3:17)
  = note: the borrow is used later here (3:5)
```

Without this check the two additions could interleave and lose one: a data race. Give each closure its own variable
and add them afterwards, as Listing 16-6 does, or use an atomic ([Shared state](./ch16-03-shared-state)).

::: details Coming from Rust
`Thread.spawn(move () => …)` is `std::thread::spawn(move || …)`, and `Thread<T>` is `JoinHandle<T>`. What differs:

- `spawn` returns a `Result`, since CeeSafe reports a failed allocation instead of aborting;
- `join` returns `T`, not `Result<T, _>`: a panic in a thread ends the process, so there is no panic to carry back;
- dropping the handle joins the thread; it does not detach it;
- `Thread.both` is a small `std::thread::scope` for exactly two closures, one of which runs on the calling thread.
:::

::: details Coming from Go, Java or C#
A goroutine or a `Thread` in those languages may read and write any variable it can see, and the garbage collector
keeps the variable alive. Here a spawned thread owns what it uses, and the compiler refuses a capture that another
thread could also reach. A program cannot end while a thread is still running: `main` waits for every thread.
:::

::: details Coming from C
`Thread.spawn` is `pthread_create` with the argument block built and typed by the compiler, and `join` is
`pthread_join` returning a typed result. You never cast a `void *`, and the block cannot point at a caller's stack
frame that has gone.
:::

## Summary

- `Thread.spawn(move () => body)!` starts a thread and gives a `Thread<T>`; `join()` waits for it and returns the
  body's result.
- A spawned thread owns its captures: values move in (`N0250` if you use them afterwards), `Copy` values are
  copied (`N0403` if the thread changes its copy and the variable is used later), and borrows are refused
  (`N0332`). The body runs once, so it may move a capture out, as its result for example.
- A handle that is not joined is joined when it is destroyed. No thread outlives `main`.
- A panic in any thread ends the program.
- `Thread.both(a, b)` runs two closures that may borrow from the caller, under the usual borrowing rule (`N0241`).

Next: [Channels](./ch16-02-channels), for threads that pass values to one another while they run.
