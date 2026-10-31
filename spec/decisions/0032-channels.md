# ADR 0032 — L1: channels between threads and between tasks

- Status: **Accepted** by the owner, 2026-10-31 ("Accept as written"): every question as recommended (§5).
- Scope: `Channel<T>` in `lib/std/channel.cee`. It is the increment ADR 0016 §3.4 deferred ("`Condvar` and channels
  follow as a separate increment, each spec-first"), and the transport ADR 0031 §3.5 needs between executors.

## 1. Context

Threads share state today only through `Arc<Mutex<T>>` and the atomics (§38.3). ADR 0031 runs one executor per
thread and never moves a task between threads, so tasks on different threads need a way to pass values that wakes a
waiting task without blocking its thread.

Constraints from the tree:

- **No tuples** (`AGENTS.md`), so `let (tx, rx) = channel()` cannot be written.
- **`Mutex.lock` has `sleep`** (§38.3), so async code cannot use one (N0295, type-system.md §40.1).
- **Allocation failure is a value** (OD-6 and `ownership.md`), so a send must not allocate behind the caller's back.
- **The interpreter runs one thread at a time** (ADR 0016 §3.8).

## 2. Options

| | A. One cloneable handle, bounded buffer (proposed) | B. Separate `Sender<T>`/`Receiver<T>` | C. Unbounded |
|---|---|---|---|
| Creation | `Channel<T>.create(n)` | needs a pair: no tuples, so an out-parameter or a struct | as A or B |
| Who may send and receive | every handle | senders and receivers | as A or B |
| Allocation | once, at `create` | once | on every send that grows the buffer |
| Back-pressure | a full channel waits | as A | none |

**A is proposed.** B adds two types and a pair struct for what close() and documentation can say. C breaks the rule
that allocation failure is a value at the place it happens.

## 3. Proposed decision

1. **`Channel<T: Send>`** is a library type, `@threadSafe`: a handle to a shared, reference-counted buffer of
   `capacity` values. It is `Send` and `Share`.
   - `Channel<T>.create(capacity: usize) -> Result<Channel<T>, AllocError>` allocates the buffer once. A capacity of 0
     panics: a rendezvous channel is not in v1.
   - `c.clone()` returns another handle to the same channel. Dropping the last handle drops the values still
     buffered, in order, and frees the buffer.
   - `c.close()` makes every later send fail. Receivers still take what is buffered, then see the end.
2. **Blocking operations**, for threads, with the `sleep` effect:
   - `c.send(v: T) -> Result<void, T>` waits while the channel is full, and returns `Err(v)` if it is closed.
   - `c.recv() -> T?` waits while it is empty, and returns `null` once it is closed and empty.
3. **Async operations**, for tasks (type-system.md §40), with the `suspend` effect. While they wait, the task is parked
   and its thread runs other tasks:
   - `await c.sendAsync(v: T) -> Result<void, SendError<T>>`. `SendError<T>` is `Closed(T)` or `Cancelled(T)`, so a
     value is never lost: a cancelled or refused send hands it back.
   - `await c.recvAsync() -> Result<T?, AsyncError>`. It returns `Err(AsyncError.Cancelled)` only while it has taken
     nothing, so no value is lost to cancellation (ADR 0031 §3.4).
4. **Waking.** A parked task registers its waker (`Async.waker()`) with the channel. A send wakes the oldest waiting
   receiver, and a receive wakes the oldest waiting sender. Waking a task of another thread goes through that
   thread's executor (`ceesafe_async_wake`, safe from any thread), so a channel connects executors as well as tasks
   on one executor. Waiters are served first come, first served.
5. **The lock.** The channel's state is guarded by a spin lock over `lib/core`'s atomics. Its critical sections are a
   constant number of steps and never wait, allocate or call user code. Taking it therefore has no effect, and async
   code may use the channel. The blocking operations wait outside the lock, on a runtime condition variable
   (`ceesafe_cond_*`, `@effects(sleep)`).
6. **Drops run outside the lock.** A value is moved out of the buffer under the lock and dropped by the caller, so a
   `drop` block never runs while the lock is held.
7. **The interpreter.** Threads run one at a time and tasks on the virtual clock. A thread or task waiting on a
   channel that no one else can serve is a deadlock, reported as an interpreter fault (as ADR 0016 §3.8).
8. **Profiles.** Hosted only, with threads (§38.5): `Channel` is N0293 elsewhere.

## 4. Consequences

- Three runtime additions: `ceesafe_cond_*` (a condition variable beside `ceesafe_mutex_*`), thread-safe wakers, and
  each executor's wake descriptor (a self-pipe, or eventfd on Linux) that its `poll` waits on with its other
  registrations.
- The spin lock's `unsafe` code keeps `ownership.md` T1–T3. Only `Send` values cross (T1), and the buffer is reached
  only under the lock (T2).
- The tests to add:
  - run-pass: one thread to another, task to task on one executor, and a task on one thread to a task on another;
  - check-fail: a value that is not `Send` (N0334), and a blocking `recv` in async code (N0295);
  - the fault sweep reaches `create`, the only allocation.

## 5. Questions for the owner

1. One handle type rather than a sender/receiver pair? **Yes** (§3.1).
2. Bounded only, with a capacity of at least 1? **Yes**: unbounded and rendezvous channels can come later.
3. A spin lock inside the channel, so async code can use it? **Yes** (§3.5).
