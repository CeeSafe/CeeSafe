# ADR 0053 — Task sets: spawning while a task runs

- Status: **Accepted** by the owner, 2026-11-02 ("Yes, I accept all five ADRs with your recommendations"): every
  question of §5 as recommended. Asked for by the owner for CeeSafe.Web
  (`~/dev/CeeSafe.Web/docs/ceesafe-upstream-work.md`, item U11). A server starts a task per accepted connection, and
  an HTTP/2 connection starts a task per stream as streams open.
- Scope: a new `Async` operation and a lib/std type, the checker and the borrow checker's view of it, both backends,
  the runtime's scope record, the interpreter.

## 1. Context

A scope's children are spawned only in its synchronous body (type-system.md §40.5: "`s` is not a local, and names the
scope only in `s.spawn(…)`"). Everything is built on that:

- **The checker** resolves `s` only while the body is checked, in the same function (`src/check/async.cee`,
  `scopeNamed`). A child closure is another function, so `s` is unknown there.
- **The borrow checker** keeps each child's captured loans until the scope's wait, in the same NIR function
  (`src/nir/borrow.cee`, the spawn holders).
- **The C** names the scope as a field of the awaiting frame (`cee_frame->cee_s<site>`), which only that function's
  code can reach.
- **The runtime** sets the scope's owner at each spawn (`cee_spawn`: `scope->owner = cee_exec.current`). A child
  spawning into its parent's scope would make the owner chain, which cancellation walks, wrong.

So a server today runs a fixed set of worker children, each looping accept-then-serve on a shared listener
(CeeSafe.Web's `packages/web_transport/tests/workers.cee`). The worker count is the connection limit, and every
worker's frame exists from the start.

There are no detached tasks, and this ADR keeps it so (§40.5: "There is no detached task"). A task set is a scope
whose children are started while it runs, and it waits for all of them before it returns.

## 2. Options

**What dynamically started children may capture.**

| | A. Owned values only (proposed) | B. Loans, as a scope's children |
|---|---|---|
| What | `async move` closures whose captures are owned or `Copy` and hold no loan; shared state through `Arc` | children borrow from the task that started them, as a scope's do |
| Cost | a shared listener or configuration goes in an `Arc`, or is passed down by value | a child may outlive the task that started it, so its loans would have to reach past that task's frame: a borrow-checker rule across functions and tasks that does not exist |

**A is proposed.** It is sound with the rules that exist: a child holds nothing that can end before it does. A
connection's task owns its stream, and what it shares it shares through `Arc`.

## 3. Proposed decision

### 3.1 The operation

```ceesafe
await Async.tasks(limit, oom, async (set) => {
    loop {
        let stream = await listener.accept()?;
        await set.spawn(async move () => await serve(stream))?;   // waits while `limit` children run
    }
})
```

- **The operation.** `await Async.tasks(limit, oom, body)` is `Result<void, E>`:
  - `limit: u32` is the most children live at once;
  - `oom: E` is the result if a child's frame cannot be allocated;
  - `body` is an async closure taking `set: borrow TaskSet<E>` and returning `Result<void, E>`.
- **The body** runs as the set's owner, like any awaited call. When it returns, the set waits for every child.
- **`Async.tasksIn(alloc, limit, oom, body)`** takes the children's frames from `alloc`, as `Async.scopeIn` does.
- **The handle.** `set` is an ordinary `borrow` parameter. The body may pass it to the functions it calls and awaits,
  so an accept loop can live in its own function. A spawned child cannot capture it: by §3.2, it holds no loan.

### 3.2 Spawning

- **`await set.spawn(child)`** starts `child`, an `async move` closure literal of result `Result<void, E>`, and is
  `Result<void, E>`:
  - while `limit` children are live, the spawning task waits for one to return (admission, as backpressure);
  - once the set is cancelled, it returns the cancellation's error and starts nothing;
  - if the frame cannot be allocated, it returns `Err(oom)`, and the set goes on.
- **`set.trySpawn(child)`** is `bool`: false when `limit` children are live or the set is cancelled, without waiting.
  It is not a task, so it needs no `await`.
- **Captures** (option A): each capture of `child` is moved or copied into it, and its type holds no loan: no
  reference, slice or `StringView`, and no struct with region parameters. Otherwise **N0349**, naming the capture.
  `spawnSend`'s thread rule (§40.5, ADR 0038) applies to a `set.spawnSend`, which may run on another worker.

### 3.3 Errors and cancellation

- **A child's `Err` does not cancel its siblings.** One failed connection must not end the others. The set keeps the
  first child error.
- **The set's result,** once the body and every child have returned:
  - the body's `Err` if it returned one;
  - else the first child error, or `oom` if a frame could not be allocated;
  - else `Ok`.
- **The body's `Err`, or a cancellation of the set's owner, cancels every child** (§40.7's flag), and the set waits for
  them.
- **The owner is fixed when the set starts.** A child spawned by any task of the set has the set's owner in its chain,
  so cancellation reaches it.

### 3.4 Lowering and the runtime

- **The set's record** is a `ceesafe_scope` with a limit and a count, in the frame of the task that awaits
  `Async.tasks`, as a scope's is. `TaskSet<E>` is lib/std's handle to it: a reference to the record, which the body
  takes by `borrow` and so cannot outlive. The C reaches it through the handle, from any function.
- **`ceesafe_async_spawn` learns about sets:**
  - the owner is set once, at the set's start;
  - a full set parks the spawning task until a child returns (a wait list on the record);
  - a returning child wakes one waiting spawner.
- **The interpreter** looks a set up by its handle, not by the current task and site as it does a scope today. So a
  spawn from any task of the set reaches the set.

## 4. Consequences

- CeeSafe.Web's server starts a task per connection, up to a limit, with no worker frames allocated in advance, and
  HTTP/2 a task per stream.
- **Sharing goes through `Arc`** (or values passed down). That is the price of children that may outlive the task that
  started them.
- **A scope's `s.spawn` is unchanged.** Children that borrow from their caller stay the static form.
- **New diagnostic:** N0349, a set child's capture that holds a loan.

## 5. Questions for the owner

1. `await Async.tasks(limit, oom, async (set) => …)` and `Async.tasksIn(alloc, …)`, the handle a `borrow` parameter
   the body may pass down but no child may capture? Recommended: yes.
2. Children capture owned or `Copy` values only, with no loans, sharing through `Arc` (option A)? Recommended: yes.
3. `await set.spawn` waits while `limit` children are live; `set.trySpawn` returns false instead? Recommended: yes.
4. A child's `Err` does not cancel its siblings; the set reports the body's error, else the first child's? Recommended:
   yes.

## 6. Implementation plan (once accepted)

1. Spec:
   - §40.5 (task sets beside scopes, the capture rule and N0349);
   - §40.7 (cancellation through the set's owner);
   - c-backend.md §3a;
   - intermediate-representation.md;
   - diagnostics.md;
   - the guide's async page.
2. `src/`:
   - the checker (the operation, the handle's type, `spawn`/`trySpawn` on it, N0349);
   - HIR and NIR forms;
   - the C backend (the record in the frame, spawn through the handle);
   - the interpreter's sets.
3. Runtime: the record's limit, count, waiting spawners and fixed owner; `tools/gen_runtime.py`.
4. lib/std: `TaskSet<E>`; `tools/gen_library.py`.
5. Tests:
   - corpus run-pass: an accept loop starting a task per connection, with more clients than the limit, all served; a
     full set making the spawner wait; `trySpawn` refused when full; a child's error not cancelling siblings; the
     body's error and an outer cancellation cancelling every child; `oom` from `tasksIn` with a failing allocator; the
     handle passed to a function that spawns;
   - check-fail: N0349 for a borrowed capture, a slice, a `StringView`; the handle captured by a child;
   - `tools/cancel_sweep.py`, the allocation-failure sweep with `--check-leaks`, TSan for `spawnSend` (ADR 0038);
   - every reactor and Windows.
6. ROADMAP §10.

## 7. Implementation notes (2026-11-02)

- **The body through ADR 0037.** `Async.tasks(limit, oom, body)` is checked as a call of lib/core's
  `AsyncTasks<E>.run`, an `@effectsOf(body)` async fn whose async instance awaits the body in the awaiting task: the
  set's owner, as §3.1 says. Its frame holds the record (`TaskSetRecord<E>`: the runtime's `ceesafe_task_set`, then
  the first child error), and after the body it waits for the children with `ceesafe_async_set_done` and
  `Async.park`. So the compiler needs no form for the body or the wait, and the borrow checker no rule: only the
  start is the compiler's.
- **The start.** `await set.spawn(child)` is checked as `await TaskSet.spawned(set, SetSpawn(set.recordOf(), await
  set.room(), child))`, and `trySpawn` with `tryRoom` and `started`. `SetSpawn` is a HIR form and the NIR builtin
  `AsyncSetSpawn`, emitted in any function (a sync one may call `trySpawn`). `set` must be a local or parameter of
  type `TaskSet<E>` or a reference to one, since it is read once per step.
- **Readings of §3.2 and §3.4.**
  - `spawn`'s `Err(oom)` may come many times, so `E` must be `Copy` (N0268 at `Async.tasks`).
  - A spawn on a cancelled set has no error of `E` that means cancellation, so it starts nothing and is `Err(oom)`;
    its task is being cancelled, and so is its set's result.
  - Only the owner can spawn (a child cannot capture the set), so the runtime keeps one task waiting for room, and
    the owner never changes.
  - A `limit` of 0 is 1.
- **`Async.tasksIn` (the second commit).** A spawn in another function does not know the allocator's type, so the set
  is given its adapters when it starts: lib/core's `AsyncTasksIn<E, A>.run` calls `Async.setAllocator(record, alloc)`,
  a library-only primitive (N0345), whose builtin `AsyncSetAllocator` carries `A` and emits
  `ceesafe_async_set_allocator(record, ceeAA_<A>, ceeAD_<A>)`; the runtime's set spawns take their blocks from them.
  `Async.tasks` is `AsyncTasksIn<E, SystemHeap>` (its frames are the system heap's either way). `tasksIn` works in
  the freestanding profile; `tasks` is hosted only.
- **`set.spawnSend`** checks ADR 0038's rule on the child's captures (each moved in, so each must be `Send`) and on
  `E`, and calls `ceesafe_async_set_spawn_send` and `ceesafe_async_publish`. A set with `spawnSend` children is shared:
  admission and its wait for room take the run's lock, as `scope_done` does. With a user allocator (tasksIn) the
  child runs on this worker, since a frame from it is freed where the child returns.
- **The interpreter** takes a user allocator's blocks for a set's children as for `scopeIn`, and models
  FailingAllocator's attempts (`failAfter`).
- **The interpreter** models the record (its InterpScope, flagged as a set), the room and done waits (parked until a
  child returns, through `asyncWoken`), the body's cancellation, and the start.
- **Tests:** run-pass/async-task-set (ten children under a limit of three, started from a function the set is passed
  to; a child's error that cancels no sibling; the body's error cancelling every child; `trySpawn` refused while full;
  an outer timeout reaching every child), natively, in the interpreter, under the cancellation sweep; check-fail/
  async-task-set (N0349 for a reference, a slice, a `StringView` and the set itself; N0209; N0342; N0268); the guide's
  example.

