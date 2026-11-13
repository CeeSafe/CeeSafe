# ADR 0112 — A thread's closure runs once; `Mutex.tryLock`

- Status: **Accepted** by the owner, 2026-11-13 ("Fix those new gaps too"), implemented on branch `fix/gaps2-a`; released in ceesafec
  0.42.0. Raised by the book (Decision 0011): chapter 16 could not write a thread that gives back what it was
  given, and had no way to ask for a lock without waiting; chapter 13 met the same message for any closure.
- Scope: the checker (`src/check/threads.cee`, `src/check/closures.cee`), the move and borrow checks' messages
  (`src/nir/move_check.cee`, `src/nir/borrow.cee`), both backends' thread join (`src/backend/c_dyn.cee`,
  `src/interpreter/io.cee`); `lib/std/sync.cee` and the runtime (`ceesafe_mutex_try_lock`);
  `spec/type-system.md` §36.5, §38.2, §38.3; `spec/diagnostics.md`; the guide.

## 1. Context

**Moving a capture out.**

```ceesafe
let text = String.from("x")!;
let back = Thread.spawn(move () => text)!.join();
```

was N0251, "cannot move a 'String' out of the value 'env' refers to". The rule behind it is right for a closure in
general (§36.5): the value belongs to the closure, which may be called again, and a second call would find it gone.
CeeSafe has no closure type that can be called only once. But the message named `env`, the body's hidden first
parameter, which no program writes; and for this literal the rule's reason does not hold. `Thread.spawn` takes the
literal itself, never a value the program could call, and the new thread calls its body exactly once.

The language already has literals that run once: an `async move` child (ADR 0074 §2.3) is a **run-once
environment**, whose body may move each capture out, once, and drops the rest as it returns.

**The lock.** `Mutex<T>` had `lock()` only, which waits. A thread that would rather do something else than wait, and
async code, which must not block its thread (N0295), had no call to make.

## 2. Decision

2.1 **The literal of `Thread.spawn(move () => …)` is a run-once environment.** Its body may move each capture out,
once; a second move is N0250, a partial move of a capture with a drop block N0252, as for an `async move` child. No
new rule is needed in the move check: the environment's struct is marked as the child's is.

2.2 **Who drops the captures.** The body drops the captures it did not move, as it returns, on the new thread (one
that was moved on some paths only has a drop flag). `join`, and the drop of a `Thread<T>`, free the environment's
memory and drop nothing. If the thread cannot be started, `spawn` drops the whole environment, as before.

This changes when a capture that is *not* moved out is destroyed: when the thread's body returns, where until now it
was when the thread was joined. It is what an `async move` child does, and it is the better time: a `Channel`
handle or an `Arc` a thread captured is released when the thread is done with it, not when somebody gets round to
joining it. The captures are `Send` (N0334), so they may be dropped on the thread.

2.3 **Every other closure keeps the rule**, with a message that names the captured variable and gives the reason:

```text
error[N0251]: cannot move the captured 'text', a 'String', out of this closure: the closure may be called again, so the value stays in it; only a Copy capture can be read out. Lend it ('borrow') or return a copy; the body of 'Thread.spawn(move () => …)' or of an 'async move' child runs once and may move it (type-system.md §36.5)
```

That holds for a boxed closure passed to `Thread.spawn` too: the box is an ordinary owning closure. N0245 for a
closure's result that may borrow from its captures no longer names `env` either.

2.4 **`m.tryLock()`** is `MutexGuard<T>?`: the guard if no thread holds the lock, else null, at once. It never
waits, so it has no effect (`@effects()`) and a task may call it. The lock is not recursive: the thread that holds it
gets null. The guard is scoped to the mutex exactly as `lock`'s is (the optional carries the region). In the runtime
it is `pthread_mutex_trylock`, `TryAcquireSRWLockExclusive` or CosmoOS's `cosmo_mutex_trylock`; the interpreter
takes the lock if its owner is none.

2.5 **Giving a lock up early needs nothing new.** A guard unlocks where it is dropped, and a value is dropped where
it is moved away: `let _ = guard;` (ADR 0072). The specification and the guide now say so.

## 3. Alternatives

- **A message only**, keeping N0251 for `Thread.spawn`. Sound, and the minimum. Rejected because the mechanism for
  run-once literals exists, is already tested by the async children, and makes the natural program correct; the
  working form would otherwise be a channel or an `Arc<Mutex<T?>>` to carry one value out of one thread.
- **A call-once closure type** (`dyn once fn`), so that any closure passed along could give up its captures. A new
  kind of type, with its own coercions, call rule and table: far more than the item needs. Nothing here is in its way.
- **Drop the remaining captures at `join`, as before.** It would need the body to leave its drop flags behind for
  the joiner; and it keeps resources alive after the thread has ended.
- **`tryLock` returning `Result<MutexGuard<T>, WouldBlock>`.** There is one reason for the failure; an optional says it.
- **`guard.unlock()`.** A second way to write `let _ = guard;`.
- **`RwLock<T>`.** Not here: it needs a reader-writer lock in the runtime for each platform (CosmoOS's userland
  among them), a model of blocked readers and writers in the interpreter, and its own `Share` rule (`T: Send + Share`).
  Proposed separately.

## 4. Consequences

- `run-pass/threads.cee` expected a captured value's drop at `join`. Its unjoined thread now returns the value it
  captured (so its "drop 2" still comes from the `Thread<T>`'s drop, at the end of `main`); the expected output is
  unchanged. A program that relied on a capture living until `join` sees it destroyed earlier; it cannot observe
  the value in between, since it gave it to the closure.
- A dyn table for a run-once environment has no drop entry.
- Tests: `run-pass/thread-moves-capture-out.cee`, `run-pass/mutex-try-lock.cee`,
  `check-fail/thread-closure-moves.cee`, `check-fail/thread-messages.cee`.
