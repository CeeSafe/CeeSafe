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
- A `Thread<T>` that is dropped without `join` is joined then: every thread ends before `main` returns.
- A panic in any thread ends the whole program.

## What may cross: `Send`

A value may move to another thread if it is `Send`. Plain data, strings, vectors and boxes are. References are not,
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

`Send` and `Share` can also bound a generic function: `fn run<T: Send>(value: T)`.

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
    threads.clear(); // dropping the threads joins them
    var guard = tally.get().lock();
    Console.writeLine(guard.data().count);
}
```

```text
1000
```

For a counter or a flag, an atomic is simpler: `AtomicU64`, `AtomicU32`, `AtomicUSize` and `AtomicBool` change
through a shared reference with `load`, `store`, `swap`, `compareExchange`, `fetchAdd` and `fetchSub`.

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

[Unsafe code and C](unsafe-and-c.md).
