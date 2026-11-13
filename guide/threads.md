# Threads

A program can run work on several threads. The compiler checks that safe code never has a data race: a value moves to
another thread only if it is safe there, and threads share memory only through types made for it.

## Spawning and joining

`Thread.spawn` starts a thread running a `move` closure, which owns what it captures. It returns a `Thread<T>`, and
`join` waits for the thread and gives back its result.

```ceesafe
fn main() {
    var numbers = Vector<u64>.create();
    for (i in 1..101u64) {
        numbers.push(i)!;
    }
    let worker = Thread.spawn(move () => {
        var sum: u64 = 0;
        for (n in numbers[..]) {
            sum += n;
        }
        return sum;
    })!;
    Console.writeLine(worker.join());
}
```

```text
5050
```

- The vector is moved into the thread. The spawner cannot use it afterwards, so nothing is shared.
- Spawning can fail (no memory, no thread): `Thread.spawn` returns a `Result`, here unwrapped with `!`.
- A `Thread<T>` that is destroyed without `join` is joined then: every thread ends before `main` returns.
- A panic in any thread ends the whole program.

The thread runs the closure's body once, so the body may move a captured value out, for instance to give it back.
What the body does not move out, it destroys when it returns, on its thread:

```ceesafe
fn main() {
    var line = String.from("made by main")!;
    let worker = Thread.spawn(move () => {
        line.append(", finished by a thread")!;
        return line;
    })!;
    let finished = worker.join();
    Console.writeLine(finished);
}
```

```text
made by main, finished by a thread
```

A value whose type is Copy (a number, a `bool`) is copied into the closure, and the closure changes its copy. If the
function reads the variable afterwards, the compiler warns (N0403), since the read sees the old value:

<!-- ceesafe: warns N0403 -->
```ceesafe
fn main() {
    var hits = 0;
    let t = Thread.spawn(move () => { hits += 1; })!;
    t.join();
    Console.writeLine(hits);
}
```

```text
main.cee:3:26: warning[N0403]: this 'move' closure changes its own copy of 'hits': the type is Copy, so the closure took a copy, and the 'hits' read after it is unchanged; return the value from the closure, or share it through an atomic, a 'Mutex' or a 'Channel'
  |
3 |     let t = Thread.spawn(move () => { hits += 1; })!;
  |                          ^^^^^^^^^^^^^^^^^^^^^^^^^
```

Return the count from the closure (`join` gives it back), or share it as the next sections show.

## What may cross: `Send`

A value may move to another thread if it is `Send`. Plain data, strings, vectors and boxes are. Borrows are not,
nor anything that holds one, nor raw pointers. The compiler infers it, and reports N0334 where it fails:

<!-- ceesafe: fail N0334 -->
```ceesafe
fn main() {
    var n: u64 = 5;
    let address = borrow n as *u64;
    let t = Thread.spawn(move () => address.isNull())!;
}
```

```text
main.cee:4:26: error[N0334]: this thread's closure captures 'address', a '*u64', which is not Send: it cannot move to another thread (type-system.md §38.1)
  |
4 |     let t = Thread.spawn(move () => address.isNull())!;
  |                          ^^^^^^^^^^^^^^^^^^^^^^^^^^^
```

A value that two threads *read* at once must also be `Share`. Most `Send` types are, but not a `StableVector`, whose
`push` changes it through a shared borrow, nor a `Box<dyn I + Send>`, whose contents the compiler cannot see, nor
anything that holds one. Put such a value behind a `Mutex` to share it.

`Send` and `Share` can also bound a generic function: `fn run<T: Send>(value: T)`. Such a `T` holds no borrow, so
a thread's closure may give it back: `Thread.spawn(move () => value)!.join()`.

## Sharing: `Arc`, `Mutex` and atomics

To share a value, give each thread an owner of it: `Arc<T>` counts its owners, and the last one to go frees it. An
`Arc` gives only shared access, so a value that threads change goes in a `Mutex`, whose guard lends it to one thread
at a time.

```ceesafe
struct Tally {
    count: u64;
}

fn main() {
    let tally = Arc<Mutex<Tally>>.create(Mutex<Tally>.create(Tally { count: 0 })!)!;
    var threads = Vector<Thread<void>>.create();
    for (t in 0..4u64) {
        let mine = tally.clone();
        threads.push(Thread.spawn(move () => {
            for (k in 0..250u64) {
                var guard = mine.get().lock();
                let data = guard.data();
                data.count += 1;
            }
        })!)!;
    }
    threads.clear(); // destroying the threads joins them
    var guard = tally.get().lock();
    Console.writeLine(guard.data().count);
}
```

```text
1000
```

`lock` waits until the lock is free. `tryLock` never waits: it returns the guard, or null if a thread holds the lock
(the calling thread included). A guard unlocks where it is destroyed: at the end of its block, or earlier where it is
moved away, as by `let _ = guard;`.

```ceesafe
fn main() {
    let m = Mutex<u64>.create(1)!;
    var guard = m.lock();
    let data = guard.data();
    data += 1;
    Console.writeLine(m.tryLock().isSome()); // held: no guard
    let _ = guard;                           // unlocks here
    match (m.tryLock()) {
        Some(free) => {
            var second = free;
            Console.writeLine(second.data());
        }
        null => { Console.writeLine("busy"); }
    }
}
```

```text
false
2
```

For a counter or a flag, an atomic is simpler: `AtomicU64`, `AtomicU32`, `AtomicUSize` and `AtomicBool` change
through a shared borrow with `load`, `store`, `swap`, `compareExchange`, `fetchAdd` and `fetchSub`, and the
integer ones with `fetchOr` and `fetchAnd`.

Those are sequentially consistent: every thread sees every atomic operation in one order. That is the simple rule,
and on x86-64 it costs a full barrier on each store. When one thread publishes data and another reads it, release
and acquire are enough, and `storeWith` and `loadWith` say so:

```ceesafe
struct Message {
    data: AtomicU64;
    ready: AtomicBool;
}

fn main() {
    let m = Message { data: AtomicU64.create(0), ready: AtomicBool.create(false) };
    var got: u64 = 0;
    Thread.both(() => {
        while (!m.ready.loadWith(LoadOrdering.Acquire)) {}
        got = m.data.loadWith(LoadOrdering.Relaxed);
    }, () => {
        m.data.storeWith(42, StoreOrdering.Relaxed);
        m.ready.storeWith(true, StoreOrdering.Release);
    });
    Console.writeLine(got);
}
```

```text
42
```

The release store makes the data written before it visible to a thread whose acquire load reads `true`. The
`…With` methods take C11's orderings (`Relaxed`, `Acquire`, `Release`, `AcquireRelease`, `SequentiallyConsistent`). A
weaker ordering never makes a data race, but a wrong one can make a thread read a stale value, and neither
`ceesafec run --interp` nor ThreadSanitizer will notice. Use the plain methods unless a measurement says otherwise.
`benchmarks/atomics` has one: a ring between two threads, 5 to 9 times faster on x86-64 with release and acquire.

## Built once: `OnceLock`

A value that is built on first use and then only read, such as an index or a cache, goes in a `OnceLock`. The first
thread to ask runs the initialiser; any thread that arrives meanwhile waits, and every later read is one atomic load:

```ceesafe
struct Catalog {
    prices: Vector<u64>;
    total: OnceLock<u64>;

    fn sum(borrow this) -> u64 => this.total.getOrInit(() => {
        var sum: u64 = 0;
        for (p in this.prices.view()) {
            sum += p;
        }
        return sum;
    });
}

fn main() {
    var prices = Vector<u64>.create();
    prices.push(40)!;
    prices.push(2)!;
    let catalog = Arc<Catalog>.create(Catalog { prices: prices, total: OnceLock<u64>.create() })!;
    let other = catalog.clone();
    let worker = Thread.spawn(move () => other.get().sum())!;
    Console.writeLine(catalog.get().sum());
    Console.writeLine(worker.join());
}
```

```text
42
42
```

- `get()` gives the value or `null`, `set(v)` fills an empty cell, and `tryInit<E>(init)` takes an initialiser that
  returns a `Result` and leaves the cell empty on an error.
- `take()` and `getMut()` need the cell exclusively (`var` and no other borrow).
- For one thread, lib/core's `OnceCell<T>` has the same members and no lock.

## Borrowing: `Thread.both`

A spawned thread cannot borrow, since it may outlive what it borrowed. `Thread.both(a, b)` can: it runs `a` on a new
thread and `b` on this one, and returns only when both have finished. So the two closures may use the caller's data,
for example the two halves of a slice:

```ceesafe
fn sum(values: Slice<u64>) -> u64 {
    var total: u64 = 0;
    for (v in values) {
        total = total + v;
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

The usual borrowing rules hold between the two closures: both may read a value, but if one changes it the other may
not use it. A value both read must be `Share`, and one that a closure changes must be `Send`; a reference counts as
its referent here. `splitAtMut` gives two halves that each closure may change. If a thread cannot be started, `both`
runs `a` and then `b` on this thread, so it never fails.

## Passing values: `Channel`

A `Channel<T>` (lib/std) carries values of a `Send` type from one thread to another through a buffer of fixed size.
Every handle may send and receive, and `clone` makes another handle to the same channel, so each thread owns one.

```ceesafe
fn main() {
    let channel = Channel<u64>.create(16)!;
    let sender = channel.clone();
    let producer = Thread.spawn(move () => {
        for (i in 1..11u64) {
            match (sender.send(i * i)) {
                Ok() => {}
                Err(_) => { return; }
            }
        }
        sender.close();
    })!;
    var sum: u64 = 0;
    while (true) {
        match (channel.recv()) {
            Some(v) => { sum += v; }
            null => { break; }
        }
    }
    producer.join();
    Console.writeLine(sum);
}
```

```text
385
```

- `Channel<T>.create(capacity)` allocates the buffer once, so it returns a `Result`. The capacity is at least 1.
- `send(value)` waits while the channel is full. On a closed channel it gives the value back as `Err(value)`.
- `recv()` waits while the channel is empty, and is `null` once the channel is closed and empty.
- `trySend` and `tryRecv` never wait; `close()` ends the channel for every handle, and `isClosed()` asks.
- The values still buffered are destroyed with the last handle.
- Tasks use the same channel with `sendAsync` and `recvAsync` ([Async tasks](async.md)).
- The interpreter runs threads one at a time, and reports a thread that waits to send on a full channel as a
  deadlock: there, give the channel room for what is sent before the receiver runs, as above.

## Stopping a thread that waits in `accept`

`listener.accept()` blocks its thread until a client connects. Another thread ends that wait with
`listener.shutdown()`: every `accept` on the listener, the one being waited in and every later one, returns
`Err(IoError.ConnectionAborted)`. Both calls borrow the listener, so the threads share it through an `Arc`.

```ceesafe
fn main() {
    let listener = Arc<TcpListener>.create(TcpListener.bind("127.0.0.1", 0)!)!;
    let accepting = listener.clone();
    let acceptor = Thread.spawn(move () => {
        var clients: u32 = 0;
        loop {
            match (accepting.get().accept()) {
                Ok(_) => { clients += 1; }
                Err(IoError.ConnectionAborted) => { break; } // the listener was shut down
                Err(e) => {
                    Console.writeLine(e.message());
                    break;
                }
            }
        }
        return clients;
    })!;
    listener.get().shutdown()!;
    Console.writeLine(acceptor.join());
}
```

```text
0
```

- `shutdown` does not close the socket. The listener closes when its last owner drops it, so no thread can use it
  after the close.
- `accept` returns `ConnectionAborted` for no other reason: match it to leave the accepting loop.
- With `ProcessSignals` this is a server's graceful stop: the main thread calls `signals.wait()`, which blocks until
  Ctrl+C (`SIGINT`) or `SIGTERM`, then `shutdown()`; the accepting thread leaves its loop and joins the threads
  that are still answering. `ProcessSignals` is described in [Async tasks](async.md#stopping-at-ctrl-c).
- The interpreter runs threads one at a time. A thread that waits there, in `accept` or in `signals.wait()`, keeps
  the others from running, so no other thread can wake it: a server of threads is tried compiled, and one of tasks
  in either.

## Async tasks on several threads

An `Async.run` uses one thread. To spread a scope's children over several, run it with `Async.runWorkers(4, task)`
and spawn them with `s.spawnSend(async () => …)` instead of `s.spawn`. Another worker may take such a child before it
starts. Once started, the child stays on that worker. What the child captures follows the rule of `Thread.both`: a
value it reads must be `Share`, and one it changes must be `Send`. Two children cannot change the same value, so they
report results through atomics, or through a value per child. Under a plain `Async.run`, `spawnSend` is `spawn`.

## Where threads run

Threads are part of the hosted profile: Linux, macOS, Windows and CosmoOS user programs. Kernel code uses the
kernel's own locks instead ([Kernel code](kernel.md)). The interpreter (`ceesafec run --interp`) runs threads one at a
time, in a fixed order, so its output is always the same.

## Next

[Async tasks](async.md): many waits on one thread.
