# ADR 0031 — L1: async/await (stackless frames, structured scopes, cancellation as a flag)

- Status: **Accepted** by the owner, 2026-10-31 ("I accept it as written."): every question as recommended (§5).
  **Amended** the same day while writing `type-system.md` §40 (§6): a scope's allocation failure is an error the caller
  names ("Caller names the OOM error", the owner's choice), and the changes that follow from `?` needing one error
  type (§17.4).
- Scope: `async fn`, async closures, `await`, the `suspend` effect, `Async.run`, structured scopes, cancellation, and the
  frame ABI between ceesafec, the runtime and `lib/std`. The first CeeSafe language decision after the fork (Decisions
  0006, 0007). The design and its rationale are `docs/async-await-proposal.md` (revised after a cross-check against the tree,
  2026-10-31); this record fixes the choices the implementation depends on.

## 1. Context

CeeSafe has no concurrency other than threads (§38: `Thread.spawn`, `Thread.both`, `Arc`, `Mutex`, the atomics).
`async`, `await` and `yield` are reserved (`language-spec.md` §1) and lexed as keywords
(`src/syntax/token.cee`) but used nowhere.

Rust's async shows ten failure modes (the proposal, "What goes wrong in Rust and others"). Most come from three
choices: futures that move (hence `Pin`), tasks that outlive their spawner (hence `'static + Send`), and cancellation
by dropping a suspended future. CeeSafe can avoid all three because of rules it already has:

- a panic aborts (OD-5), so a suspended frame is never unwound;
- context effects exist (§32), so blocking in async and a spin guard across `await` can be refused;
- fork-join borrowing exists (§38.6, ADR 0018);
- scoped collections of closures exist (§25, §36.2);
- allocators are explicit (§18, §30).

Facts from the tree that constrain the design:

- NIR has no call terminator: a call is an rvalue (`intermediate-representation.md`).
- The C structurer falls back to flat form on an irreducible graph (`c-backend.md` §3).
- The interpreter runs NIR on an explicit activation stack.
- Every console, file and socket operation has `sleep` (§32.1).
- There are no channels yet (ADR 0016 left them for a later increment).

## 2. Options

| Model | Verdict |
|---|---|
| **A. Stackless frames that never move, structured scopes, cancellation as a flag** | Proposed |
| B. Stackful coroutines or green threads | Needs stack switching per architecture, which C11 cannot express; kernel stacks are small |
| C. Effect handlers (OCaml 5) | The same stack switching, and a large new language feature |
| D. Rust's model (`poll`, `Pin`, cancel by drop, detached tasks) | The source of the problems A avoids |

## 3. Decision: A

### 3.1 The language

1. **`async fn f(…) -> T`.** A call to it is a *task*. In v1 a task must be awaited at once (`await f(x)`), passed
   to `Async.run`, or spawned into a scope (§3.3). Anything else is **N0342**. There is no first-class task value.
2. **`await e`** is a prefix operator. It is allowed only in an async fn or async closure body (**N0341**: a sync
   function, a `drop` block or a sync closure). Its operand is a primary followed by calls, member accesses and
   indexes; the first `?` or `!` ends the operand and applies to the awaited value, so `await f(x)?` is
   `(await f(x))?` and `await f(x)?.len` is `((await f(x))?).len` (a task is never a `Result`). Its type is the
   task's result. (Wording corrected 2026-10-31 while writing `grammar.md`: the accepted text said "binds looser than
   postfix operators", which would group `await (f(x)?)`, contrary to its own example; the example is the decision.)
3. **Async closures**, `async (…) => …`, are closure literals whose call is a task (§36.2's capture rules apply
   unchanged). They are accepted where a task-producing closure is expected: `spawn`, `race`, `all`, `shield`.
4. **Recursion through `await`** would make a frame infinitely large, so it is **N0343**. Its sure fix,
   `await Async.boxed(alloc, f(…))`, comes with L1 A3.

### 3.2 Effects (amending §32 and §36.3)

1. **A third effect, `suspend`:** the code may await. `@effects(…)` and effect lists on function types accept it
   (N0290 as before for other names). `suspend` is not in the default effect list of a function type written
   without one, because sync code cannot await.
2. **New contexts and restrictions:**

   | Context | Forbids | Diagnostic |
   |---|---|---|
   | An async fn or async closure body | `sleep` | N0295, with the call chain; `Async.blocking` is the bridge |
   | A live `@context(atomic)` value (§32.3's liveness rule) | `suspend` | N0295 |
   | `@context(irq)` | `suspend`, in addition to `sleep` and `alloc` | N0295 |

3. **Calls through closures.** `Async.scope`, `spawn`, `race`, `all` and `shield` are `@effectsOf` their closure
   arguments (§32.5).

### 3.3 Structured concurrency

1. **Scopes.** `Async.scopeIn(alloc, oom, (s) => …)` runs its body, which may call `s.spawn(async () => …)`. It
   returns only after every child has returned. Child frames are placed with `alloc`. `Async.scope(oom, body)` uses the
   system heap and has `alloc`. `spawn` returns `void`; a child whose frame cannot be allocated is not started, the
   others are cancelled, and the scope returns `Err(oom)` (amended, §6).
2. **The scope-loan rule.** `s` is typed as a scoped collection of closures whose region is the body's environment,
   and `s.spawn(c)` checks like a `push(c)`. Hence:
   - captures that conflict across spawns are **N0241**, by the existing borrow rules;
   - nothing a child captures can be changed, moved or ended by the body before the scope ends;
   - a child that captures one of the body's own locals, which end before the children do, is **N0344**.
3. **No `Send` or `Share` check** on children's captures, because children run on the scope's own thread.
   `Async.blocking(closure)` runs its closure on a worker thread, so its captures follow §38.6's rule (**N0334**).
4. **The result.** The scope's result is the first error a child returns. The other children are then cancelled
   (§3.4), and the scope still waits for them.
5. **Combinators** (amended, §6).
   - `Async.race(a, b)` returns the winner and hands back the loser's value if it also finished (`Raced<T>`).
   - `Async.all(a, b)` runs both and returns the first error, with results through captures.
   - Both take two async closures and embed their frames in the caller's, so they never allocate.
   - There is no detached spawn in v1.

### 3.4 Cancellation

1. **Cancelling a task sets a flag.** It never destroys a frame. A frame is freed only after its task returns.
2. **Who sees the flag.** Only the library's operations that would block observe it: timers, I/O, channel receives,
   plus `Async.checkpoint(cancelled)` and `Async.isCancelled()`. The library's operations return
   `Err(AsyncError.Cancelled)`, where `AsyncError` (`lib/std`) has `Cancelled`, `TimedOut` and `Io(IoError)`.
   `checkpoint(cancelled: E)` returns `Result<void, E>` (amended, §6).
3. **Cleanup** is ordinary code on the `?` path, and destructors run in the normal order.
4. **Shields.** `Async.shield(deadline, body)` makes those operations ignore the flag. Once the deadline passes they
   return `Err(TimedOut)`, never `Cancelled`.
5. **`drop` blocks stay synchronous** (§3.1, N0341).

### 3.5 Threads, executors and the library

1. **One executor per thread; a task never moves between threads.** Tasks need not be `Send`. Several cores are used by
   several threads, each with `Async.run`, connected by channels (ADR 0032, L1 A2).
2. **Built in and library.** The compiler builds in only what needs frame layout or control flow:
   - `async fn`, async closures, `await`;
   - `Async.run`, `scope`/`scopeIn`, `spawn`, `race`, `all`, `shield`, `checkpoint`, `blocking`;
   - two primitives, `Async.park()` (suspend the current task until woken) and `Async.waker()` (its wake handle).
     They are accepted only in library files (**N0345**).

   Timers, console, files, sockets, `Async.Mutex` and `Channel` are library code in `lib/std` over runtime functions
   with honest `@effects` (Decision 0008).
3. **Console output from async code** waits for the descriptor to be writable (`poll`) and writes at most `PIPE_BUF`
   bytes per step. POSIX guarantees such a write does not block, so the runtime function declares `@effects()`.
4. **Profiles.** Async is in the hosted profile until L1 A4. The frame ABI is in every profile.

### 3.6 Lowering (amending `intermediate-representation.md` and `c-backend.md`)

1. **NIR** (amended, §6.5).
   - An awaited call is an ordinary `Call`, marked as awaited; `Async.run`'s task call is marked too; `Async.park()` is
     a builtin. So the borrow, move and drop analyses see calls and need no rule of their own.
   - A coroutine pass after the effect checker builds the await graph (N0343).
2. **Frames.** Async fn `F` becomes four pieces of C (`c-backend.md` §3a):
   - `struct ceeAF_<F>` holds the parameters, every local, one child-frame field per awaited call, the result and the
     state;
   - `ceeAI_<F>` fills in the arguments;
   - `ceeAR_<F>(frame)` returns false while the task is pending, and `ceeAT_<F>` is it as the runtime calls it.

   A frame is placed once (on `Async.run`'s caller's stack, in its parent's frame, or from a scope's allocator) and
   never moves, so a borrow of a local across `await` stays valid. In v1 every local lives in the frame; L1 A3 shrinks
   frames by liveness and adds `--explain-frames`.
3. **Resume functions are always emitted in flat form.** The state `switch` makes the graph irreducible for an `await`
   inside a loop.
4. **The runtime.** `ceesafe_rt_async.c` holds the executor, the timers and the reactor (`poll` in A1, kqueue and epoll
   in A2), plus wakers by id. `ceesafe/rt.h` declares the task header: the resume function, root frame, cancel flag,
   shield depth, scheduler links and waker.
5. **Determinism.** The interpreter runs async code on a virtual clock with a seeded run order. A
   `CEESAFE_CANCEL_AT=k` sweep cancels at every suspension point, modelled on `CEESAFE_FAIL_ALLOC_AT`.

### 3.7 Diagnostics

| Code | Meaning |
|---|---|
| N0341 | `await` outside an async fn or async closure |
| N0342 | A call of an async fn that is not awaited, run or spawned |
| N0343 | Recursion through `await` (a frame that contains itself) |
| N0344 | A spawned child that captures the scope body's own locals |
| N0345 | `Async.park` or `Async.waker` outside the library |

N0290 and N0295 extend to `suspend`.

## 4. Consequences

- Eight of the ten Rust failure modes become compile errors or cannot arise. Colouring is kept on purpose, and the
  debugger's task tree is a tooling task (L1 A3).
- The compiler itself does not use async, so the fixed point is unaffected until a later decision makes it do so.
- `lib/std` gains `async.cee` (A1) and `channel.cee` (A2, with ADR 0032).
- The interpreter gains a task scheduler (A2). Scopes and their bodies must run as activations, never through the
  interpreter's nested calls.

## 5. Questions for the owner

Each has a recommended answer, which §3 assumes.

1. First-class `Task<T>` values in v1? **No** (§3.1.1).
2. How `Cancelled` is typed: **a case of every async I/O error enum**, with `checkpoint()` returning
   `Result<void, Cancelled>`. Amended (§6): one library enum, `AsyncError`, and `checkpoint(cancelled: E)` (§3.4.2).
3. `race` and a loser that also finished: **hand back its value** (§3.3.5).
4. `await` and `?`: **`await f(x)?` is `(await f(x))?`** (§3.1.2).
5. Awaiting in a `defer` or `drop` inside an implicit shield: **not in v1** (§3.4.5).
6. Resume functions: **flat C in v1** (§3.6.3).
7. Scope storage: **an explicit allocator**, `scopeIn`, with `scope` on the system heap (§3.3.1).

## 6. Amendments (2026-10-31)

`?` propagates only an error of the same type (`type-system.md` §17.4: there are no `From` conversions). Three
accepted choices did not respect that, and were changed while §40 was written:

1. **A scope's allocation failure.** As accepted, `spawn` returned `Result<void, AllocError>`, which cannot become the
   scope's `Result<void, E>`. Asked, the owner chose the caller-named error: `Async.scope(oom, body)` and
   `scopeIn(alloc, oom, body)`, with `spawn` returning `void` (§3.3.1).
2. **`checkpoint`.** For the same reason, `checkpoint()` returning `Result<void, Cancelled>` could not be propagated
   with `?` from a function with its own error type. It takes the error to return: `checkpoint(cancelled: E)`.
   `isCancelled()` is added for code that decides for itself (§3.4.2).
3. **The I/O error type.** Adding `Cancelled` to the built-in `IoError` would break every exhaustive `match` on it.
   The library's async operations return `AsyncError` instead, which wraps `IoError` (§3.4.2).
4. **`race` and `all`** are `Async.race(a, b)` and `Async.all(a, b)` rather than scope methods, with two children whose
   frames are embedded. They then need no allocator and no out-of-memory error (§3.3.5).
5. **Lowering, found while implementing A1.** New NIR terminators (`StartTask`, `Await`, `Park`) would have needed an
   arm in every analysis and in the interpreter for what is, to all of them, a call. Instead an awaited call stays a
   `Call` marked in a side table, and the C backend alone turns marked calls into suspension points (§3.6.1). The C
   names follow the C function name, `ceeAF_<F>` and so on, and the runtime's entry is `ceesafe_async_run(frame,
   resume)`; the task header arrives with A2's scheduler.
6. **The scope body is a block, not a closure** (2026-10-31, the owner's choice "Inline block"). Checked as a closure,
   a spawned child's environment would live in the body's block, which ends while the child still runs. So
   `Async.scope(oom, (s) => { … })`'s body is checked as a block of the awaiting function; `s` names the scope only in
   `s.spawn(…)`. The scope-loan rule is then checked on the children's capture lists (`src/check/async.cee`),
   conservatively, per local:
   - two children capturing one local where either changes it are N0241;
   - a body statement that uses a local a child has captured is N0241, since the child may run until the scope
     returns (the awaiting function itself is suspended until then);
   - a child that captures one of the body's own locals outlives it, which is N0344.

   `return`, `?`, `break` and `continue` cannot leave the body, the same limit a closure body has (§3.3.2).

## 7. Implementation

None yet. `ROADMAP.md` (language track L1) records each phase as it is run.
