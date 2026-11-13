---
title: Shared state
summary: One value that several threads change in place - a Mutex and its guard, Arc to own a value from several spawned threads, atomics for counters and flags, how long a lock is held, and deadlock, which the compiler does not prevent.
assumes: [ch16-01-starting-threads, ch04-02-borrowing, ch09-02-result]
keywords: [Mutex, MutexGuard, lock, tryLock, Arc, clone, AtomicU64, AtomicBool, fetchAdd, compareExchange, deadlock, OnceLock]
diagnostics: [N0248]
specification: ["type-system.md §38.3", "ownership.md T2", "ownership.md T3"]
---

# Shared state

**This section teaches** how several threads read and change one value: with a lock, or with an atomic. It assumes
`Thread.spawn` and `Thread.both` ([Starting threads](./ch16-01-starting-threads)), the borrowing rule
([Borrowing](./ch04-02-borrowing)) and `Result` ([Result](./ch09-02-result)).

The borrowing rule says many readers or one writer. Two threads that both add to one counter are two writers, and
the compiler refuses them (Listing 16-10 in [Starting threads](./ch16-01-starting-threads)). Sometimes that is still
what the program needs: a total that every thread adds to, a table that every thread fills. The library has two
kinds of type for it. Both let a value change through a *shared* borrow, and both make sure that two threads never
change it at the same moment.

## A lock: `Mutex`

A `Mutex<T>` holds a value of type `T` and a lock. To reach the value, a thread takes the lock. While it holds the
lock, any other thread that asks for it waits.

<span class="listing-caption"><strong>Listing 16-18</strong>, <code>main.cee</code>: Two threads add to one total, each taking the lock for every addition.</span>

```ceesafe
fn main() {
    let total = Mutex<u64>.create(0)!;
    Thread.both(() => {
        for (k in 0..1000) {
            var guard = total.lock();
            let data = guard.data();
            data += 1;
        }
    }, () => {
        for (k in 0..1000) {
            var guard = total.lock();
            let data = guard.data();
            data += 2;
        }
    });
    var guard = total.lock();
    Console.writeLine(guard.data());
}
```

```text
3000
```

- `Mutex<u64>.create(0)` puts the value 0 in a new mutex. The lock is allocated, so `create` returns a `Result`.
- `total` is a `let`, and both closures only borrow it shared. That is allowed: both are readers of the *mutex*.
- `total.lock()` waits until the lock is free, takes it, and returns a **guard**, a `MutexGuard<u64>`.
- `guard.data()` lends the value as a `borrow mut u64`. Bind it to a name, then read or change the value through
  that name. `guard.data() += 1` is not an assignment the language accepts (N0104).
- The guard is declared with `var`, because `data()` borrows it mutably.
- When the guard is destroyed, at the end of the loop body here, the lock is released.

There is no way to reach the value without a guard, and no call to forget: the lock is held exactly as long as the
guard lives.

::: tip Rule
`m.lock()` returns a guard. Only the guard's `data()` lends the mutex's value, as `borrow mut T`, and destroying the
guard unlocks. So at most one thread at a time has the value.

*Specification: `type-system.md` §38.3; `ownership.md` T3.*
:::

## How long the lock is held

A guard lives to the end of the block it is declared in. Other threads wait all that time, so keep that block
small: put the guard in a block of its own when the function goes on to other work.

<span class="listing-caption"><strong>Listing 16-19</strong>, <code>main.cee</code>: The inner block ends the first guard, so the lock is free when it is taken again.</span>

```ceesafe
fn main() {
    let total = Mutex<u64>.create(0)!;
    {
        var guard = total.lock();
        let data = guard.data();
        data += 5;
    }
    var again = total.lock();
    Console.writeLine(again.data());
}
```

```text
5
```

## Owning from several threads: `Arc`

`Thread.both` lets its closures borrow the mutex. A thread started with `Thread.spawn` cannot borrow, and a mutex
moved into one thread is gone for the others. For that, `Arc<T>` gives a value several owners. Each `clone()` is
another owner of the *same* value, and the value is destroyed when its last owner is.

<span class="listing-caption"><strong>Listing 16-20</strong>, <code>main.cee</code>: Four spawned threads and <code>main</code> each own the same tally, through an <code>Arc</code>, and change it under its lock.</span>

```ceesafe
struct Tally {
    count: u64;
    largest: u64;
}

fn main() {
    let tally = Arc<Mutex<Tally>>.create(Mutex<Tally>.create(Tally { count: 0, largest: 0 })!)!;
    var workers = Vector<Thread<void>>.create();
    for (id in 1..5u64) {
        let mine = tally.clone();
        workers.push(Thread.spawn(move () => {
            var guard = mine.get().lock();
            let data = guard.data();
            data.count += 1;
            if (id > data.largest) {
                data.largest = id;
            }
        })!)!;
    }
    while (workers.pop() is Some(worker)) {
        worker.join();
    }
    var guard = tally.get().lock();
    let data = guard.data();
    Console.writeLine(data.count);
    Console.writeLine(data.largest);
}
```

```text
4
4
```

- `Arc<Mutex<Tally>>.create(…)` moves the mutex to the heap with a count of owners. It allocates, so it returns a
  `Result`: the line has one `!` for the mutex and one for the `Arc`.
- `tally.clone()` does not copy the tally. It adds an owner. Each thread's closure takes its own `mine`.
- `mine.get()` lends the value inside as a shared borrow, `borrow Mutex<Tally>`. An `Arc` never lends `borrow mut`:
  it cannot know that the other owners are not looking.
- The two changes to the tally happen under one guard, so no other thread sees `count` raised and `largest` not yet
  updated. A lock protects whatever you do while you hold it, not only one operation.

::: tip Rule
`Arc<T>.create(v)` returns `Result<Arc<T>, AllocError>`. `a.clone()` is another owner of the same value, and
`a.get()` lends it as `borrow T`, never `borrow mut T`. To change a value in an `Arc`, put a `Mutex` or an atomic
inside it.

*Specification: `type-system.md` §38.3; `ownership.md` T2.*
:::

## Atomics

For one number or one flag, a lock is more than is needed. An **atomic** is an integer or a `bool` whose every
operation happens as one step that no other thread can interrupt. The types are `AtomicU32`, `AtomicU64`,
`AtomicUSize` and `AtomicBool`. Their operations take a shared borrow, so an atomic needs no `var` and no guard.

<span class="listing-caption"><strong>Listing 16-21</strong>, <code>main.cee</code>: Two threads count on one atomic, and one of them sets a flag.</span>

```ceesafe
fn main() {
    let hits = AtomicU64.create(0);
    let done = AtomicBool.create(false);
    Thread.both(() => {
        for (k in 0..1000) {
            let _ = hits.fetchAdd(1);
        }
    }, () => {
        for (k in 0..1000) {
            let _ = hits.fetchAdd(1);
        }
        done.store(true);
    });
    Console.writeLine(hits.load());
    Console.writeLine(done.load());
}
```

```text
2000
true
```

`fetchAdd(1)` adds 1 and returns the value from before the addition, which the listing discards with `let _ =`.
No addition is lost, whatever the timing. For spawned threads, share the atomic as `Arc<AtomicU64>`.

| Operation | What it does | Returns |
|---|---|---|
| `load()` | reads | the value |
| `store(v)` | writes `v` | nothing |
| `swap(v)` | writes `v` | the value before |
| `compareExchange(expected, new)` | writes `new` only if the value is `expected` | `Ok(before)`, or `Err(current)` and no change |
| `fetchAdd(v)`, `fetchSub(v)` | adds or subtracts, wrapping around | the value before |
| `fetchOr(v)`, `fetchAnd(v)` | sets or keeps bits | the value before |

`AtomicBool` has the first four.

<span class="listing-caption"><strong>Listing 16-22</strong>, <code>main.cee</code>: The operations on one thread. The first <code>compareExchange</code> expects 7, finds 9, and changes nothing.</span>

```ceesafe
fn main() {
    let turn = AtomicU32.create(7);
    Console.writeLine(turn.swap(9));
    match (turn.compareExchange(7, 1)) {
        Ok(before) => { Console.writeLine("changed"); }
        Err(current) => { Console.writeLine(current); }
    }
    match (turn.compareExchange(9, 1)) {
        Ok(before) => { Console.writeLine("changed"); }
        Err(current) => { Console.writeLine(current); }
    }
    Console.writeLine(turn.fetchAdd(5));
    Console.writeLine(turn.load());
}
```

```text
7
9
changed
1
6
```

An atomic makes *one* operation safe. Two operations in a row, a `load` and then a `store` of the result, are two
steps, and another thread can run between them. That is no data race, and the compiler accepts it, but an update
can still be lost. Use `fetchAdd` or `compareExchange` for a change that depends on the old value, and a `Mutex`
when several values must change together.

Two more tools are in [the guide's page on threads](https://ceesafe.github.io/CeeSafe/guide/threads):
`OnceLock<T>`, a value that the first thread to ask for it builds and every thread then reads, and the `…With`
forms of the atomic operations (`loadWith`, `storeWith`), which take a weaker memory ordering for speed.

## Deadlock

The compiler proves that two threads never touch one value at once. It does not prove that a thread which waits
will ever go on. A **deadlock** is a wait that cannot end. The smallest one needs only one thread.

<span class="listing-caption"><strong>Listing 16-23</strong>, <code>main.cee</code>: The first guard is still alive when the same thread asks for the lock again.</span>

<!-- ceesafe: exits 70 -->
```ceesafe
fn main() {
    let total = Mutex<u64>.create(0)!;
    var first = total.lock();
    let data = first.data();
    data += 5;
    var second = total.lock();
    Console.writeLine(second.data());
}
```

This compiles. Under `ceesafec run --interp` it stops with exit status 70:

```text
ceesafe interp: fault: deadlock: a thread locks a mutex it already holds at lib/std/sync.cee:40:18
```

A built program has no such check: it waits for ever. Listing 16-19 is the fix, a block that ends the first guard.

A thread that would rather do something else than wait calls `tryLock()`. It never waits: it returns the guard if
the lock is free, and `null` if any thread holds it, the calling thread included.

```ceesafe
fn main() {
    let total = Mutex<u64>.create(0)!;
    var first = total.lock();
    let data = first.data();
    data += 5;
    if (total.tryLock() is Some(second)) {
        Console.writeLine("free");
    } else {
        Console.writeLine("held");
    }
}
```

```text
held
```

The classic deadlock takes two threads and two locks. One thread locks `a` and then asks for `b`, while the other
has locked `b` and asks for `a`. Each waits for the other, for ever, and only when the timing is unlucky, so tests
can pass for a long time. The defences are habits, not checks:

- hold one lock at a time where you can;
- where a thread needs two, take them in the same order everywhere in the program;
- keep guards short-lived, and do not call code you do not know while holding one.

The interpreter runs threads one at a time, so it does not find a deadlock that depends on timing.

::: tip Rule
Safe code has no data race. It can still deadlock: neither the compiler nor a built program detects a wait that
never ends.
:::

## Mistakes

::: warning Mistake: a guard declared with `let`
`data()` borrows the guard mutably, so the guard must be mutable.
:::

<span class="listing-caption"><strong>Listing 16-24</strong>, <code>main.cee</code>: The guard is a <code>let</code>. This does not compile.</span>

<!-- ceesafe: fail N0248 -->
```ceesafe
fn main() {
    let total = Mutex<u64>.create(0)!;
    let guard = total.lock();
    Console.writeLine(guard.data());
}
```

```text
main.cee:4:23: error[N0248]: cannot borrow 'guard' as mutable: 'guard' was declared with 'let'; use 'var' for a mutable variable, or bind it with 'using'
  |
4 |     Console.writeLine(guard.data());
  |                       ^^^^^
```

Write `var guard = total.lock();`, as the listings of this section do. The `using guard = total.lock();` that the
message proposes compiles too.

::: warning Mistake: changing a value that is only in an `Arc`
An `Arc` makes a value shared. It does not make it changeable.
:::

<span class="listing-caption"><strong>Listing 16-25</strong>, <code>main.cee</code>: A thread pushes to a vector it reaches through an <code>Arc</code>. This does not compile.</span>

<!-- ceesafe: fail N0248 -->
```ceesafe
fn main() {
    let list = Arc<Vector<u64>>.create(Vector<u64>.create())!;
    let other = list.clone();
    let worker = Thread.spawn(move () => {
        let mine = other.get();
        mine.push(1)!;
    })!;
    worker.join();
    Console.writeLine(list.get().length);
}
```

```text
main.cee:6:9: error[N0248]: cannot borrow the value 'mine' refers to as mutable: it is behind a shared reference ('borrow'); a 'borrow mut' reference is needed to modify it
  |
6 |         mine.push(1)!;
  |         ^^^^
```

`push` needs `borrow mut`, and `get()` lends `borrow`. Make it an `Arc<Mutex<Vector<u64>>>` and push through a
guard, as Listing 16-20 changes its tally.

::: details Coming from Rust
`Arc<Mutex<T>>` is the same pair with the same roles. What differs:

- `lock()` returns the guard, not a `Result`: a panic ends the process, so a mutex is never poisoned;
- the guard does not dereference to the value: call `guard.data()`, and declare the guard `var`;
- `Mutex.create` and `Arc.create` return a `Result`, since both allocate;
- an `Arc` is read with `a.get()`, not by dereferencing it, and the library has no `RwLock`;
- the atomic operations are sequentially consistent unless you call a `…With` form with an ordering.
:::

::: details Coming from Java, C# or Go
In those languages a lock and the data it protects are two things, and nothing stops code from touching the data
without the lock (`synchronized`, `lock`, `sync.Mutex`). Here the data is *inside* the mutex and comes out only
through a guard, so "forgot to lock" does not compile. Unlocking is not a statement either: the guard's end does it,
on every path out of the block.
:::

::: details Coming from C
`Mutex<T>` is the platform's lock (a pthread mutex on Linux and macOS) kept with the data it protects, and the
unlock call is made for you where the guard ends. The atomics are C11's `_Atomic` integers, with `memory_order_seq_cst` by default.
:::

## Summary

- A `Mutex<T>` owns its value. `lock()` gives a guard, `guard.data()` lends the value as `borrow mut T`, and the end
  of the guard unlocks. Declare the guard with `var`.
- A lock is held as long as its guard lives: use a block to end it early. `tryLock()` asks for the lock without
  waiting, and returns `null` if it is held.
- `Arc<T>` gives a value several owners for spawned threads: `clone()` adds one, `get()` lends `borrow T` only.
  Shared and changeable is `Arc<Mutex<T>>`, or an `Arc` of an atomic.
- Atomics (`AtomicU32`, `AtomicU64`, `AtomicUSize`, `AtomicBool`) change through a shared borrow, one indivisible
  operation at a time.
- The compiler prevents data races, not deadlocks: do not lock a mutex you hold, and take several locks in one
  order.

Next: [Send and Share](./ch16-04-send-and-share), the two properties of types that all these checks rest on.
