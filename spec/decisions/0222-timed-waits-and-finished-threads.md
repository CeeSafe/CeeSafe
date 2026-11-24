# ADR 0222 — A thread's sleep, a receive with a time limit, and asking whether a thread has finished

- Status: **Proposed**, 2026-11-24, and implemented on the branch `fix/findings-os` (not merged, not released) at the
  owner's request to close the SQLite port's findings 23 ("No timed wait for threads") and 30 ("A finished thread
  cannot be reaped"). The names in §2 are this ADR's.
- Scope: type-system.md §38.2 (threads) and §40.6 (channels); lib/std's `ThreadServices` and `Channel`; the
  runtime's `ceesafe_thread_sleep`, `ceesafe_thread_finished` and `ceesafe_chan_wait_until`; the interpreter.

## 1. Context

Outside async code a thread can wait for a channel (`recv`), a mutex or another thread (`join`), but never for a
time. A connection waiting for another's transaction (SQLite's busy timeout) or for a free pooled connection must
give up after a while, so the port polled `tryRecv()` against `Timer.now()`, spinning a core (finding 23).

A `Thread<T>` has only `join()`, which waits. A server that starts a thread per client and runs for days can neither
keep the handles (the vector only grows) nor join them as clients leave without blocking on one that is still
serving; `Thread.service` threads are never joined, and nothing tells the program when one is done (finding 30).

## 2. Decision

### 2.1 `Thread.sleep(ms)`

`Thread.sleep(ms: u64)`: the calling thread waits at least `ms` milliseconds (`nanosleep`, resumed after a signal
for the time left; `Sleep` on Windows, in steps below `INFINITE`). `Thread.sleep(0)` returns at once. It is lib/std's
`ThreadServices.sleep` (hosted), named as a member of the built-in `Thread` beside `spawn`. Effect: `sleep`, so an
`async fn` cannot call it (N0295): a task waits with `await Timer.sleep(ms)`, which lets the thread's other tasks run.

### 2.2 `channel.recvTimeout(ms)`

`recvTimeout(ms: u64) -> Result<T?, AsyncError>` on `Channel<T>`: as `recv`, but waiting at most `ms` milliseconds on
the monotonic clock (`Timer.now()`):

- `Ok(Some(v))`: the next value; `Ok(null)`: the channel is closed and empty, as `recv` says;
- `Err(AsyncError.TimedOut)`: neither happened in time. A buffered value is taken even when `ms` is 0, so
  `recvTimeout(0)` is `tryRecv` with the closed case told apart.

The shape is `recvAsync`'s (`Result<T?, AsyncError>`), so code moves between the two; `AsyncError.TimedOut` is
already "a deadline of the program's" (§26.2). The wait is the runtime's: `ceesafe_chan_wait_until(channel, epoch,
deadline)` blocks on the channel's condition variable until the channel changes or the deadline passes
(`pthread_cond_timedwait`, `SleepConditionVariableSRW`, `cosmo_cond_timedwait`), so nothing spins. Effect: `sleep`.

### 2.3 `t.isFinished()`

`t.isFinished() -> bool` on a `Thread<T>`, borrowing it: `true` once the thread's body has returned (its result is
written), so a `join()` after it does not wait. It never waits and has no effect. A server keeps its client threads
in a `Vector<Thread<u64>>` and, now and then, removes and joins the finished ones:

```ceesafe
var k: usize = 0;
while (k < clients.length) {
    if (clients[k].isFinished()) {
        served += clients.swapRemove(k).join();
    } else {
        k += 1;
    }
}
```

(A `Vector<Thread<void>>` cannot `swapRemove`: N0226, a generic method whose type argument holds `void`, a gap found
here and left for its own change; the tests and the guide use a thread with a result.)

The runtime's thread record gains a flag that its own entry function sets, with a sequentially consistent store,
after the body returns; `ceesafe_thread_finished` reads it with a sequentially consistent load, so the result the
body wrote is visible to the `join` that follows.

`Thread.spawn` returns `Result<Thread<T>, SpawnError>` and `Thread.service` `Result<void, SpawnError>` (§38.2,
ADR 0182), unchanged: a program that must know when a thread is done spawns it, and reaps it as above.

### 2.4 The interpreter

The interpreter runs one thread at a time on a virtual clock (§38, §40.8): time passes only when every thread
waits for it.

- `Thread.sleep(ms)` waits as a thread waits for a mutex, until the virtual clock reaches its deadline. Other threads
  run meanwhile; when none can, the clock moves to the earliest deadline of a waiting thread.
- `recvTimeout` waits for the channel to change or for its deadline, whichever comes first, the same way.
- `isFinished()` only answers: a thread runs when the caller waits (a `sleep`, a `recv`, a `join`), so a loop that
  polls `isFinished` and sleeps between polls ends there as it does natively. A loop that polls without ever
  waiting runs no other thread there, and does not end.
- As with every wait there, a thread that started inside another's wait cannot be resumed before it ends (the
  known difference of §38): a program whose new thread waits for its spawner is a deadlock fault there.

## 3. Options not taken

- **`tryJoin(this) -> Result<T, Thread<T>>`.** It would move the handle out and back on every poll; `isFinished`
  and `join` together do the same with a borrow, as Rust's `JoinHandle::is_finished` does.
- **A condition variable.** The cases the port had are a timed sleep and a timed receive; a `Condvar` with a
  `MutexGuard` is a larger design (spurious wakes, the guard's re-lock under poisoning) left for a program that
  needs one.
- **`Duration`.** Times elsewhere (`Timer.sleep`, `Async.timeout`) are `u64` milliseconds.
- **A finished-notification for `Thread.service`.** A service thread is by design not waited for (ADR 0182); a
  program that must know spawns.

## 4. Tests

- `tests/reference/tests/corpus/run-pass/thread-sleep.cee`: `Thread.sleep` waits at least its time (`Timer.now()`
  before and after); `sleep(0)` returns.
- `tests/reference/tests/corpus/run-pass/channel-recv-timeout.cee`: an empty channel times out, after at least the
  time; a value sent by another thread before the deadline arrives; a buffered value with `ms` 0; a closed, empty
  channel is `Ok(null)`.
- `tests/reference/tests/corpus/run-pass/thread-is-finished.cee`: a thread that waits on a channel is not finished
  until it is sent a value (then polled with `Thread.sleep` between polls); a server-style vector of threads reaped
  by `isFinished` and `join`.
- Each prints the same natively and in the interpreter; their bounds are generous (a lower bound on time slept, no
  upper one beyond the corpus's own timeout).
