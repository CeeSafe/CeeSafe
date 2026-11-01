# ADR 0038 — L1 A4e: an opt-in work-stealing executor

- Status: **Accepted** by the owner, 2026-11-01 (every question as proposed). Phase 4 of what is left of L1 A4
  (ROADMAP, the owner's "in a multiphased way").
- Scope: letting one `Async.run` use several threads, by moving tasks from a busy thread to an idle one. It covers
  which tasks may move, what the compiler checks, and the runtime's shape.

## 1. Context

ADR 0031 §3.7 chose one executor per thread, where a task never moves. So tasks need not be `Send`, and children
borrow from their parent's frame with no thread rule (§40.5). To use several cores today, a program starts threads,
each with its own `Async.run`, and connects them with channels (ADR 0032).

That leaves a common case awkward: a scope that fans out CPU-heavy children (parse, hash, compress per item). They all
run on one thread, and splitting them across threads by hand means channels and owned copies of the input.

CeeSafe already has the rule that makes borrowing across threads safe: **`Thread.both`'s capture rule** (§38.6,
ADR 0018). A capture used shared needs `Share`, a capture used mutably needs `Send`, with references counting as
`Share`/`Send` when their target is. Both closures end before `both` returns, so their loans are scoped. A scope's
children end before the scope's wait returns (§40.5), the same structure.

The proposal (docs/async-await-proposal.md §6) noted that "a later opt-in work-stealing executor can require frames to
be `Send`; the compiler knows which locals live across which `await`".

## 2. Options

### 2.1 What may move

| | A. Marked children, before they start (proposed) | B. Every task, at any suspension | C. Marked children, at any suspension |
|---|---|---|---|
| Opt-in | `s.spawnSend(child)` | `Async.runWorkers(n, task)` makes every task movable | `s.spawnSend(child)` |
| Checked | the child's captures, by §38.6's rule, and its result | every frame of every task: each local live across an `await` must be `Send` | the child's captures and every frame it awaits |
| Diagnostic | N0334 naming the capture (as `Thread.both`) | N0334 naming the local and the await | as B, for the child's tree |
| Runtime | a shared queue of unstarted children; each worker is today's executor | tasks, timers and I/O registrations move between reactors | as B |
| I/O, timers | the worker that started the task owns them, as today | a registration must move with the task | as B |

**A is proposed.** A started task stays on the worker that first ran it, so its frame never crosses threads and need not
be `Send`. Only what the child is given crosses: its captures, as `Thread.both`'s rule checks them, and its result. Each
worker is the executor that exists today, with its own reactor, timers and wake list. The new runtime piece is a queue
of unstarted children that idle workers take from.

The cost is balance. A long child cannot be split once it has started. Fan-out workloads have many children, and those
spread well. B and C would also need every reactor to hand over registrations, which none of the four backends does.

### 2.2 Where the workers come from

| | D. `Async.runWorkers(n, task)` (proposed) | E. A process-wide pool, sized to the cores |
|---|---|---|
| Threads | `n − 1` started for this run and joined when it returns; the caller is the n-th | started on the first run that needs one, never joined |
| Without it | `spawnSend` children run on the scope's thread, as `spawn`'s | — |
| Failure | a worker that cannot start leaves fewer workers; the result is the same | as D |

**D is proposed.** It is explicit, scoped like everything else in the model, and needs no global state. A library that
calls `spawnSend` works unchanged under plain `Async.run`.

## 3. Proposed decision

1. **`s.spawnSend(child)`** in an `Async.scope` body, alongside `spawn` (§40.5). It takes an async closure literal and
   returns `Result<void, AllocError>`, like `spawn`. Its captures follow §38.6's rule: a shared capture needs `Share`
   and a mutable one `Send`, references counting by their target, and a captured closure is refused (**N0334**,
   naming the capture). Its result's error type must be `Send` (**N0334**). The scope-loan rule (§40.5, ADR 0033)
   applies unchanged, so two children may not both change one value (N0241).
2. **A child may move only before it starts.** An idle worker of the same run may take it from the scope's thread.
   Once started, a task runs only on the worker that started it, and every task it awaits or spawns starts there.
   Nothing about frames is checked, because frames do not move.
3. **`Async.runWorkers(n, task)`** runs `task` like `Async.run` (§40.4), with `n − 1` more worker threads for the run
   (`n ≥ 1`). It returns when the task has returned and every worker is idle. The workers are joined then.
   `Async.run(task)` is `Async.runWorkers(1, task)`. A worker that cannot be started is skipped: the result is the
   same, and a program cannot tell how many ran (as `Thread.both`).
4. **Cancellation and the scope** work across threads. A child's error cancels its siblings wherever they run: the
   flag is atomic, and a worker's task is woken through that worker's wake pipe (ADR 0032 §5). The scope's count of
   live children is atomic. The child that finishes last wakes the parent across threads.
5. **Runtime.** One run shares a queue of unstarted `spawnSend` children (a mutex-protected deque in v1, which the
   benchmark may later replace). Each worker is a `ceesafe_executor` as today. A worker takes from the queue only when
   its ready queue is empty and nothing it waits for is due. The `tasks` command of the debugger (L1 A3) lists each
   worker's tasks.
6. **The interpreter** has one thread: `spawnSend` is `spawn`, and `runWorkers(n, …)` is `run`. A program's output must
   not depend on which worker ran what (§40.8), which the corpus checks by comparing C with the interpreter.
7. **Profiles.** Hosted only, like `Thread` (§38.5). The freestanding executor (phase 5) has one worker.

## 4. Consequences

- Checker: `spawnSend` beside `spawn` (`src/check/async.cee`), with §38.6's capture rule (`checkThreadBoth`'s,
  `src/check/threads.cee`); `runWorkers` beside `run`. NIR and backend: `spawnSend` lowers like `spawn` with a flag,
  `runWorkers` like `run` with a worker count. Runtime: the run's shared queue, atomic scope counts and cancellation,
  cross-thread wakes of a parent, workers started and joined by `ceesafe_async_run_workers`.
- Spec: type-system.md §40.4, §40.5 and §40.8; c-backend.md §3a and §6; diagnostics.md N0334; profiles.md.
- Tests:
  - run-pass: a scope fanning out `spawnSend` children over a borrowed `Vector` (the same output under `run` and
    `runWorkers(4, …)`, and in the interpreter); cancellation across workers; children doing I/O and timers on
    their own worker;
  - check-fail: N0334 for a non-`Share` shared capture, a non-`Send` mutable capture, a captured closure, and a
    non-`Send` error type;
  - TSan on Linux for the runtime (a new contract step if clang has it), plus the cancel sweep;
  - `benchmarks/async-parallel`: a CPU-bound fan-out with its speed-up at 1, 2 and 4 workers on Debian and macOS.

## 5. Questions for the owner

1. What may move: **A**, marked children before they start (no frame checks), rather than B or C (every task, or
   marked trees, at any suspension, with `Send` frames)? **A.**
2. Workers: **D**, `Async.runWorkers(n, task)` per run, rather than a process-wide pool? **D.**
3. The name `spawnSend`? (Alternatives: `spawnAnywhere`, `spawnParallel`.) **`spawnSend`.**

## 6. Implementation (2026-11-01)

- **Checking** (§3.1, §3.3): `checkSpawn` (`src/check/async.cee`) takes `send`. `checkSendChild` applies §38.6's rule
  through `isForkSafe`, refuses captured closures, and requires a `Send` error type and the system heap. These are
  the messages of `Thread.both`, naming `spawnSend`. A send child's HIR `Spawn` is listed in `c.sendSpawns`, and NIR
  marks its `Builtin(AsyncSpawn)` with the role `taskSend`. `Async.runWorkers(n, task)` is
  `BuiltinCall(AsyncRunWorkers, [n, RunTask(task)])`, lowered to `Builtin(AsyncWorkers, [n])` then the run.
  `spawn` returns `void` (a failed spawn is recorded in the scope, §40.5), so `spawnSend` does too, not the
  `Result<void, AllocError>` that §3.1 says.
- **Runtime** (§3.4–3.5), `runtime/hosted/ceesafe_rt_async.c`:
  - The loop of `ceesafe_async_run` is shared by the caller and `cee_worker` threads (`cee_exec_begin`,
    `cee_exec_loop`, `cee_exec_end`).
  - `cee_run` holds a mutex, the queue of published children with an atomic count, an atomic `done`, a
    `cancel_epoch`, and the workers' executors.
  - A child is queued only by `ceesafe_async_publish`, after its frame is initialised.
  - A shared scope's `live`, `failed` and `waiter` change under the mutex. `cancelled` is atomic, and so is a task's.
  - The waiting parent parks `on_wake`, which cancellation does not wake. The child that finishes last wakes it
    through its executor.
  - Wake pipes are non-blocking: a busy worker drains its pipe only when it waits.
  - `ceesafe_scope` grew from 24 to 32 bytes. A task grew from 128 to 144 (its executor `home`, an atomic
    `cancelled`, `unpublished`), and `ceesafe_task_slot` with it. The frame layouts follow (`scopeLayout`,
    `taskSlotLayout`), and so does the interpreter's model of a child's block.
- **Found by ThreadSanitizer:** each `spawnSend` rewrote the scope's `shared` flag while workers read it. Now only the
  first does, before any child is published.
- **Not done:** §3.5 said the debugger's `tasks` command lists every worker's tasks. It lists the stopped thread's
  executor, as before.
- **Tests:**
  - run-pass/async-workers: a fan-out with 1 and 4 workers, cancellation across workers, children with scopes of
    their own;
  - check-fail/async-workers: a capture read that is not `Share`, one changed that is not `Send`, a captured closure,
    an error type that is not `Send`, `Async.scopeIn` with a user allocator;
  - a contract step "run tsan": the async programs under ThreadSanitizer, on macOS and Linux;
  - the cancel and fault sweeps reach the new program (47 and 63 points on macOS, none failing);
  - `benchmarks/async-parallel` (macOS, 8 cores, 32 children): 412 ms with 1 worker, 194 with 2 (2.12x), 111 with 4
    (3.71x), 70 with 8 (5.89x).
