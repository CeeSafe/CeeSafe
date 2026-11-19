# ADR 0175 — Isolated tasks: a task-set child that traps ends alone

- Status: **Accepted** by the owner, 2026-11-19 ("Yes, I accept ADR 0175's eight questions as written."): every
  question of §8 as recommended. Written at the owner's request ("Write the isolated-task ADR addressing those
  three gaps"), from a review of the HTTP server written in CeeSafe against Erlang's model
  (`~/dev/CeeSafeApps/httpserver/CEESAFE_FINDINGS.md`, S3: a trap ends the process, so a task cannot fail alone) and
  from the three things that review's proposal left open: what happens to the tasks under a child that traps, a
  trap with C frames beneath it, and shared state a child was half way through changing. Implemented the same
  day (§10), with the differences §10 lists; released in 0.75.0. Its check passes on macOS, Linux and Windows on
  ARM64 (MSVC). Amendment 1 (§11, 2026-11-19): `trySpawnIsolated`, and three faults CeeSafe.Web met building on it;
  released in 0.77.0. Amendment 2 (§12, 2026-11-19): what a crashed unit leaves, counted from what it can reach;
  accepted by the owner ("I accept amendment 2's five questions as written.") and implemented (§12.7).
- Scope: the hosted runtime (the panic path, `runtime/core/ceesafe_rt_core.c`; the executor,
  `runtime/hosted/ceesafe_rt_async.c`; mutexes, `ceesafe_rt_thread.c`), lib/core (`TaskSet.spawnIsolated`,
  `TaskCrash`, `Critical`), lib/std (`Mutex`, `AsyncMutex`: poisoning; `Isolation`; `AsyncTcpStream.closer`), the
  checker (the child's captures), the interpreter; type-system.md §14.5, §38, §40 (a new §40.10), diagnostics.md.
  No change to the grammar. **What a trap does outside an isolated task does not change**: it ends the process.

## 1. Context

- **The rule today.** A trap (an index out of bounds, an overflow, an unwrapped `Err`, `unreachable`) is a panic,
  and a panic ends the process: no destructor runs, nothing is caught (§14.5, OD-5). It is the right rule for a
  program whose state cannot be trusted after a broken invariant, and it is cheap: a check and a call.
- **What it costs a server.** A server's event loop holds every connection of its process. One request whose
  handler traps ends them all. httpserver's answer is processes: a supervisor, a worker a core, and a worker that
  traps is started again, so a trap costs one worker's connections and not the server's. That is Erlang's model at
  the size of a worker, not of a request.
- **What already has the shape of an isolated process.** A task set's child (`set.spawn(async move () => …)`,
  §40.5): it owns everything it captures and holds no loan of its parent's (N0349), so it shares state only
  through `Arc`, a `Channel` and `Share` types. Its frames come from the heap, not from its parent's frame. CeeSafe.Web
  runs every HTTP/2 and HTTP/3 stream as such a child.
- **What exists to build on.**
  - The executor knows which task runs on a thread, and async frames are explicit structs: nothing of a task
    lives on the C stack across a suspension point.
  - An allocation budget (ADR 0170) already follows a task and what it starts, and counts what they hold.
  - The runtime already knows when CeeSafe is entered from C: an export or a `@callback` entered on a thread that has
    panicked is caught at that entry (ADR 0088 §2.4).
- **The three things a first sketch left open**, which this ADR is written around:
  1. A child may have tasks under it: an `Async.scope`'s children borrow from its frame, and `race`, `all` and
     `shield` embed theirs in it (§40.6). Ending "the child" must say what becomes of them.
  2. Leaving a trap by jumping back to the executor is unsound if C frames lie between: a C library that called a
     `@callback` is then abandoned half way through its own work.
  3. A child may trap half way through changing state it shares. A held `Mutex` is the visible case; an update
     of several atomics, or of words in shared memory, is the invisible one.

## 2. Decision

### 2.1 The API

```ceesafe
let r = await Async.tasks(256, NetError.OutOfMemory, async (set) => {
    // A child that ends alone if it traps. `id` names it in what the set reports.
    let id = await set.spawnIsolated(NetError.Crashed, async move () => await serve(stream))?;
    …
});

set.crashes()                 // u64: how many of its isolated children have trapped
set.watch(crashes)            // a Channel<TaskCrash> (a clone of it): each crash is put there, never waited for

public struct TaskCrash {
    public task: u64;         // what spawnIsolated returned
    public kind: PanicKind;   // Index, Overflow, Unwrap, … (a new enum in lib/core of the runtime's kinds)
    public leakedBytes: u64;  // what its unit held when it trapped (§2.4)
}
```

- **`await set.spawnIsolated(crashed, child)`** is `set.spawn(child)` for a child that is the root of an
  **isolation unit**. Its result is `Result<u64, E>`, the `u64` naming the child. `crashed: E` is what the child's
  result becomes if its unit traps: the set treats it as the child's `Err`, so it cancels no sibling and the set
  keeps the first (§40.5).
- **A trap inside a unit ends the unit and nothing else** (§2.2), unless one of §2.3's conditions holds, in which
  case it ends the process as today.
- **The child's captures must be `Send`** (N0334, as `spawnSend`'s), besides holding no loan (N0349). That keeps
  out a value that is shared with the parent without synchronisation (a type that is not `Send` is one that may
  be): a `RefCell` reached from both sides through a raw pointer, say, whose borrow flag a trap would leave set
  for the parent to trip over.
- **`set.watch(crashes)`** is the monitor: a `TaskCrash` is put in the channel for each crash with `trySend`, so
  nothing waits; one that does not fit is counted (`set.missedCrashes()`).
- `spawn`, `spawnSend` and `trySpawn` are unchanged: their children are not isolated, and a trap in one ends the
  process.

### 2.2 What ending a unit means

- **The unit is the child and every task started under it** (gap 1): the tasks it awaits, the children of its
  `race`, `all`, `shield` and `timeout`, the children of every `Async.scope` and task set opened in it, and theirs.
  A task belongs to the unit of the task that made it, as it belongs to its maker's budget (ADR 0170). An isolated
  child started inside a unit is a unit of its own while it runs, and is ended with the outer unit if that one
  traps: nothing is left that could wait for it.
- **When any task of the unit traps:**
  1. the panic's line is written and the panic hook is called, as for any panic, with the unit named (§2.5);
  2. control returns to the executor (§2.3), and **no task of the unit ever runs again**: those that were ready
     are taken off the queues, those that were parked are not woken (a wake-up for one is dropped), their timers
     are cancelled;
  3. every `Mutex` and `AsyncMutex` a task of the unit holds is poisoned and released (§2.6);
  4. the set counts the child as returned with `Err(crashed)`, frees its place under the limit, and sends the
     `TaskCrash`.
- **Nothing the unit held is freed, dropped or closed** (§2.4).

### 2.3 When a trap still ends the process

A trap in a unit ends the process, exactly as today, when any of these holds:

- **C frames lie between the executor and the trap** (gap 2). The runtime counts, for each thread, the entries
  into CeeSafe from C: an export, a `@callback`. The executor notes the count when it resumes a task; a trap at a
  higher count is under a C frame (a comparison function called by a C sort, a callback of a TLS library), and
  jumping over it would abandon that library half way. The same holds for a trap on a thread that is not an
  executor's worker running the unit's task (the closure of `Async.blocking`, a thread the unit started).
- **The task is in a critical section** (gap 3, §2.6): `Critical.enter()` is held.
- **The panic is the runtime's own**: the executor's bookkeeping failed (`CEE_PANIC_ASYNC`), or a second panic
  arrives while the first is being handled.

Otherwise the jump is sound: between the executor's resume call and the trap there are only CeeSafe's frames, whose
values are abandoned with the unit's.

- **Mechanism.** The executor sets a jump point (`setjmp`) before it resumes a task that belongs to a unit, and
  only then. The panic path, which today reports and aborts, asks first whether the thread's current task has a
  unit and none of the conditions above holds, and if so jumps there. Programs with no isolated child run the
  code they run today.

### 2.4 What is left behind

- **Everything the unit held is leaked**: its frames, every block it allocated, every value in them. No `drop`
  runs. This is the rule for a panic (§14.5), kept; what changes is that the process goes on with the leak.
- **Why not free it.** A block the unit allocated may be reachable from elsewhere: through an `Arc` the parent
  also holds, a value sent through a channel. A frame may be written by the kernel after the trap: a read that
  was in flight completes into its buffer. Freeing either would leave live code holding freed memory. Leaking is
  the one thing that is sound without knowing who else can reach what.
- **It is counted.** Each unit has a budget record (ADR 0170) of its own, unbounded unless the program gave it a
  bound, so `leakedBytes` is what the unit held when it trapped. `Isolation.report()` (lib/std) gives the
  process's totals: `crashes` and `leakedBytes`. A program that goes on for long decides from them when to stop
  and be started again: httpserver's worker drains past a threshold, and its supervisor starts another. A crash
  is expected to be rare; a program in which it is not has a fault to fix, and will be recycled often until it is.
- **Descriptors, channel ends and child processes the unit held are leaked too**, and are not counted: a socket
  stays open, a channel whose only sender was in the unit is never closed, a `ChildProcess` is never waited for.
  **The owner is told, and acts** (§2.7). The runtime does not close them, for the reason it does not free
  memory, and a stronger one: it cannot know that a descriptor number or a channel reference the unit once held
  was not moved out and released elsewhere, and closing a number that another socket now has is worse than
  leaking one.

### 2.5 What is reported

- The panic's line goes to standard error as today, followed by the unit: `ceesafe panic: index out of bounds at
  serve.cee:214:9 (isolated task 5731: ended alone)`.
- The panic hook (ADR 0088) is called with the same information and a flag that says the process goes on.
- `TaskCrash` to the set's watcher, `set.crashes()`, `Isolation.report()`.

### 2.6 Shared state half way through a change (gap 3)

- **Locks: poisoned and released.** A lock held by a task of the unit would otherwise be held for ever. Each is
  marked poisoned and unlocked. `lock()` on a poisoned `Mutex` or `AsyncMutex` **panics** (kind `Poisoned`): the
  data may break its invariant, and the next holder would act on it. In another isolated unit that is another
  crash, and in ordinary code it ends the process, which is what the first trap would have done before this ADR.
  Code that can repair the data asks: `mutex.recover()` takes the lock whether or not it is poisoned and says
  which, and `guard.heal()` clears the mark. A guard cannot leave its task (it holds a loan), so the runtime's
  list of a task's locks is exact.
- **Everything else: a critical section, declared.** An update of two atomics that must agree, of words in
  shared memory (`SharedMemory.word`), or through `unsafe` and a raw pointer has no lock to poison and nothing
  the runtime can see.

  ```ceesafe
  {
      let critical = Critical.enter();    // a trap in here ends the process, as it always did
      table.word(slot).store(key);
      table.word(slot + 1).store(value);
  }
  ```

  While a `Critical` lives in a task, a trap in that task ends the process (§2.3). It costs a counter in the task.
  It is the program's statement that the state it is changing has no other protection; where it is missing, a
  trap half way leaves that state as it was at the trap, for every other task to read.
- **lib/core and lib/std are audited** for such updates in their own safe API, and each either has none (a single
  atomic operation, a runtime call that cannot trap), takes a lock, or enters a critical section. The audit's
  list goes in the implementation notes.
- **What stays the program's to know.** A unit that traps between two steps that are each safe (a row written to
  one channel and not yet to the other, a counter raised and its work not done) leaves a state no check can
  call broken. Erlang has the same property, and the same answer: the owner is told.

### 2.7 What an owner does

- **A way to shut a socket that was given away.** `stream.closer()` (lib/std, on `AsyncTcpStream`) is a
  `StreamCloser`: `Send` and `Share`, a second hold on the same socket (a duplicate, as a `Handover`'s, ADR 0173).
  `closer.shutdown()` ends the connection for the peer whoever holds the stream. A server keeps the closer of a
  connection it hands to an isolated child, and shuts it when the child crashes: the peer is not left waiting on
  a socket nobody will ever read.
- A channel the unit was to close is closed by its owner (`Channel.close()` on a clone it kept), or its waiters
  are cancelled.
- The pattern is Erlang's: the monitor is told, and it is the monitor that cleans up.

## 3. What it does not do

- **No preemption.** A task that computes without awaiting still holds its worker, isolated or not. A bound on a
  task's computing time (a check on loop back-edges that traps a task past its bound, which this ADR would then
  end alone) is its own ADR.
- **No protection from `unsafe` code** that has already written where it should not: the process's memory is
  shared, and an isolated task is isolated by ownership, not by an address space. A program that must survive
  that uses processes (ADR 0169, ADR 0173).
- **No restart.** The set is told; starting the work again, and how often, is the owner's policy (a supervisor
  for tasks can be a library over `watch`).
- **No isolation for `spawn`, `scope`, `race`, `all` or `timeout` children**: the first is a choice not made, and
  the others' frames are in their parent's, with loans both ways.
- **Not on `Async.runWorkers`' other workers yet** (`spawnSend`): question 7.

## 4. Options that were weighed

- **Free what the unit allocated** (its budget knows the blocks). Unsound: §2.4.
- **Unwind, running destructors.** It is what Rust does, and it would close sockets and channels. It needs
  landing pads or a second return path in every function, changes every program's code size, and must define a
  trap inside a destructor; and a destructor that runs on state a broken invariant left behind is the thing
  OD-5 was decided against.
- **A ledger of descriptors for each unit, closed on a crash.** Unsound where a descriptor was moved out of the
  unit and closed elsewhere: its number may by then be another socket's (§2.4). `closer()` gives the owner the
  same effect for the socket that matters, with ownership it actually has.
- **Isolate by process only** (the status quo, with ADR 0173 making it cheaper to write). Kept: it is the answer
  for code that is `unsafe` or foreign. It costs a process for each unit of failure, which a request cannot afford.

## 5. Consequences

- **A server can lose one request to a trap.** httpserver over CeeSafe.Web: an HTTP/2 or HTTP/3 stream is already
  a task-set child that talks to its connection through a channel, so it becomes isolated by spawning it with
  `spawnIsolated`, and the connection resets the one stream. An HTTP/1.1 connection is served by one of a fixed
  number of worker tasks that are scope children (CeeSafe.Web, `web_transport/src/server.cee`): isolating it means
  serving each connection as a task-set child that owns its socket, with the server keeping its closer. That is
  CeeSafe.Web's change, and a handler's `borrow mut Exchange` stays inside the child.
- **A trap is no longer always the end.** Code that was written knowing "after a panic nothing runs" is still
  right outside a unit. Inside one, what it shared may be read after it trapped: poisoning and `Critical` are
  how it says what must not be.
- **A leak is a normal state** of a long-running program that isolates, and a number to watch.
- **Cost.** Programs without an isolated child: one test in the panic path, which is not a hot path. With one
  alive: the allocator's slow path (ADR 0170 measured no difference on httpserver's benchmark server), a jump
  point for each resume of a unit's task, and a list entry for each lock a unit's task takes. Each is measured
  on httpserver's benchmark server before this lands (§9).
- **The interpreter does the same**, so a program's tests of its crash handling run under `ceesafec test`: it ends
  the unit's tasks and reports the crash; what is leaked there is the interpreter's own, and `leakedBytes` is 0.

## 6. Why it is sound

A unit's tasks never run again, so nothing reads the values a trap left half made in the unit's own frames and
blocks. Those are never freed, so whatever else can reach them reaches memory that is still what it was. What the
unit shared, it shared through `Send` values it owned, `Arc`, channels and `Share` types: the first are the
unit's alone, a channel's operations are the runtime's and complete or do not, an `Arc`'s count is one atomic
step, and a `Share` type is either a lock (poisoned), or atomics and `unsafe` (the program's `Critical`). No C
frame is jumped over. The stack between the executor and the trap is CeeSafe's own, and holds nothing a suspended
task depends on: a suspended task's state is in its frame.

## 7. What it costs to get wrong

- A task of a dead unit that runs once more reads abandoned state. The executor's rule (§2.2, step 2) is one
  flag tested where a task is taken to be resumed, and the tests of §9 wake a dead unit's tasks every way a task
  can be woken.
- A jump over a C frame corrupts a C library. The count of §2.3 is kept where CeeSafe is entered from C, which is
  generated code in one place.
- A missing `Critical` is a logic fault in a program, not memory unsafety: the state it leaves is one the
  program could always have observed from another thread between the two steps.

## 8. Questions for the owner

1. An isolated child of a task set (`spawnIsolated`), whose unit is the child and everything started under it
   (§2.1, §2.2)? Recommended: yes.
2. Leak everything the unit held, memory, descriptors and channel ends, tell the owner, and give it
   `stream.closer()` for the socket (§2.4, §2.7), with no per-unit ledger that closes things? Recommended: yes.
   The ledger is unsound for anything that can be moved (§4), and the owner is the one party that knows.
3. `lock()` on a poisoned mutex panics, with `recover()` and `heal()` for code that can repair (§2.6)?
   Recommended: yes. Returning a `Result` from `lock()` would change every caller for a case most cannot handle.
4. A trap under a C frame, or in a `Critical`, ends the process as today (§2.3)? Recommended: yes.
5. An isolated child's captures must be `Send` (§2.1)? Recommended: yes.
6. The panic line and the panic hook for a crash that the process survives (§2.5)? Recommended: yes: a crash that
   is not written down is a fault nobody will find.
7. Leave isolated children on `Async.runWorkers`' other workers (`spawnSendIsolated`) to a later ADR? Recommended:
   yes. The jump is per thread and would work, but a unit's tasks could then be running on two workers when one
   traps, and stopping the other needs a design of its own.
8. Should the name be `spawnIsolated`, or should isolation be a property of the set (`Async.isolatedTasks`)?
   Recommended: `spawnIsolated`: a set often has both kinds (a connection's reader and its streams), and the
   crash value is the child's.

## 9. Implementation plan (once accepted)

1. Spec: §40.10; §14.5 (a panic in an isolated unit); §38 (poisoning); diagnostics.md (N0334's new use, the
   `Poisoned` kind); `spec/api`; the guide's async and error-handling pages.
2. Runtime: the unit record (its budget, its tasks, its locks, a dead flag) and a task's pointer to it; the
   executor's jump point, and the test where a task is taken to run; the panic path's test; the count of entries
   from C; poisoning in the mutexes; the set's crash count and watcher. lib/core and lib/std over them; the audit
   of §2.6. The checker: `spawnIsolated`'s child. The interpreter.
3. Tests:
   - run-pass (and so in the interpreter): a child that traps by each kind of panic while its siblings finish and
     the set reports `crashed` and the count; a `TaskCrash` received; a unit with a scope's children, a `race`,
     an `all` and a nested task set, each parked on a timer, a channel and a socket when the root traps, and
     none runs again (each would set a flag); a nested isolated child ended with its outer unit; a mutex held at
     the trap is poisoned, `lock` panics, `recover` and `heal` work; an `AsyncMutex` the same;
   - a check that needs the process's end (as `tools/child_check.py`): a trap in a `Critical`, under a `@callback`
     called from C, in `Async.blocking`'s closure, and in a child started with `spawn`, each ends the process
     with the panic's line; a trap in a unit does not, and the line names the unit;
   - leaks: `leakedBytes` equals what the child allocated, by the budget's arithmetic; a read in flight at the
     trap completes with the sanitizers on and reports nothing; 10 000 crashes in one process, with its memory
     growing by what they held and no more;
   - `stream.closer()`: the peer of a crashed child's socket sees the end when the owner shuts it;
   - the fault and cancellation sweeps over the run-pass tests;
   - the cost of §5, on httpserver's benchmark server, in ROADMAP §10.
4. CeeSafe.Web and httpserver: HTTP/2 and HTTP/3 streams isolated; HTTP/1.1 connections as task-set children;
   `app.isolate`; the worker's drain on `Isolation.report()`; a route that traps, in the supervisor check, costs
   one request.
5. ROADMAP §10.

## 10. Implementation notes (2026-11-19)

What is there is §2 on the hosted runtime and in the interpreter. It differs from what was accepted in these
places.

- **`Isolation.watch(set, crashes)`, not `set.watch(crashes)`** (§2.1). `Channel` is lib/std's and `TaskSet` is
  lib/core's, which cannot name it; the function is in lib/std's `Isolation`, beside `Isolation.report()`.
- **`spawnIsolated` returns 0 where there is no memory to isolate the child**, which is then an ordinary one: a
  trap in it ends the process. §2.1 did not say; failing the spawn would have left a started child to stop.
- **A `Mutex` is taken in async code with `tryLock`** (`lock()` may block, N0295), so `tryRecover()` was added
  beside `recover()` (§2.6). `tryLock()` on a poisoned mutex panics as `lock()` does.
- **The timers of a dead unit's tasks are not cancelled** (§2.2 step 2 said they are). A task of a dead unit
  stays where it waits, and is dropped when it is next taken to run: one test, in one place, for every way a task
  can be woken, which is what §7 asked for. Its timer fires once into that. The task that trapped is parked like
  any other if the resume that trapped had registered a wait, so that whatever wakes it finds it.
- **A unit's tasks stay on the worker that started it**: `spawnSend` inside a unit is `spawn` (§3 left
  `spawnSendIsolated` out; this is the rest of that).
- **Which `Critical` counts.** The innermost unit's: a `Critical` held by a task of an outer unit does not stop a
  unit inside it from ending alone, since the outer unit's state is not what the inner one leaves.
- **The hook's flag** (§2.5) is a new last field of `ceesafe_panic_info`, `isolated_task`: 0, or the task.
- **The panic for a poisoned lock has no source position** in the compiled program: it is raised by the runtime
  (`ceesafe_panic_poisoned`), as `CEE_PANIC_POISONED`. The interpreter gives the position of the `lock()`.
- **A dead unit that had live units inside it leaves the count of units above zero** for the life of the
  process, so entries from C go on being counted. It costs a call an entry, and nothing else.
- **The interpreter** ends a unit as the runtime does (its scheduler drops a dead unit's tasks, tells the set,
  poisons and releases). Its `leakedBytes` is 0, the kind of a panic is read from its reason, and `closer()` is
  `Unsupported` there, as a `Handover`'s holds are.

How it is built:

- **The panic path** (`runtime/core/ceesafe_rt_core.c`): `panic_report` asks `ceesafe_panic_isolate`, a pointer the
  executor sets when the first unit is made, before anything else. The runtime's own aborts
  (`ceesafe_panic_runtime`, `CEE_PANIC_ASYNC`, `CEE_PANIC_ALLOCATOR`) never ask.
- **The executor** (`runtime/hosted/ceesafe_rt_async.c`): a task has a `unit` (its maker's, as its budget; its
  own if `spawnIsolated` made it). The loop drops a task of a dead unit where it takes one to run, and sets a
  `setjmp` point before it resumes a task of a live one. `cee_isolate_panic` jumps there if the thread's count of
  entries from C is what it was at the resume and the unit holds no `Critical`. `cee_unit_crash` then marks the
  unit dead, poisons and releases its locks, gives the set the error and the count, sends the `TaskCrash`, and
  tells the set's scope that the root is gone, without freeing it.
- **What a unit holds** is a budget record of its own (ADR 0170), unbounded, inside its maker's: `leakedBytes` is
  what it holds at the trap. Under a host's allocator hook there is none, and `leakedBytes` is 0.
- **Entries from C** (`src/backend/c_interop.cee`): each export's and `@callback`'s wrapper calls
  `ceesafe_entry_enter()` and, when its function has returned, `ceesafe_entry_leave()`. Both test one global first.
- **`set.spawnIsolated`** (`src/check/async.cee`): `set.spawn`'s steps with one more between the wait for room
  and the start, `TaskSet.isolating(set, crashed, admitted)`, which leaves the crash value for the start that
  follows on the same thread with no await between; and `spawnedIsolated` for the result.
- **The audit of §2.6**, of what lib/core and lib/std share between threads in their safe API: the atomics (one
  operation each); `Channel` (the runtime's, whose operations complete or do not); `Arc` (its count is one atomic
  step; a value whose last owner traps while dropping it is leaked, and nobody else has it); `Mutex` and
  `AsyncMutex` (poisoned); `OnceLock` (its initialiser now runs in a `Critical`: the process's once-lock is held
  and the cell is half made); `SharedMemory.word` (one atomic). Nothing else in them is `Share`.

What was measured (Linux x86-64, Xeon E3-1270 v6, ceesafec built there from its own C):

- **A program with no isolated child**: httpserver's benchmark server built by the compiler before and after,
  `h2load --h1 -c 64 -t 4`, three runs of 5 s each a route: `/small` 92 859 to 100 210 requests a second before
  and 100 149 to 102 293 after; `/json` 92 719 to 97 982 and 92 441 to 93 954; a cached 4 KiB response (a `Mutex`
  for each) 83 124 to 84 799 and 81 307 to 85 200. No difference beyond the runs' own.
- **An isolated child** that allocates, waits once and returns: 255 to 273 ns, against 138 to 156 ns for an
  ordinary one (300 000 children, three passes).
- **Ten thousand crashes** in one process, each holding 64 KiB: `Isolation.report()` counts all of it, and the
  process's resident size ended at 9 MB (the blocks were never written).

What was not done:

- **§9 step 4**: CeeSafe.Web and httpserver do not use it yet. HTTP/2 and HTTP/3 streams can be started with
  `spawnIsolated`; an HTTP/1.1 connection has first to become a task-set child.
- **The fault and cancellation sweeps** over these tests (§9): the sweeps run corpus tests, and these are not.
- **Windows x64 with MSVC**: the machine was off. The check passes on Windows on ARM64 built with MSVC, on Linux
  and on macOS.
- **A unit under `Async.runWorkers`** with other workers running is not tested: its tasks stay on one worker by
  construction.
- **Tests:** `tools/isolated_check.py`, in the contract suite as `isolated`: 14 cases.

## 11. Amendment 1 (2026-11-19): `trySpawnIsolated`, and what CeeSafe.Web met

From CeeSafe.Web's Web ADR 0016 (isolated connections and streams), accepted by the owner the same day, and its
`docs/ceesafe-upstream-work.md` (U25, B21, B22, B23). The owner, asked whether to do them in CeeSafe for a release:
"Yes, go ahead."

1. **`set.trySpawnIsolated(crashed, async move () => …)`** (U25): `spawnIsolated` without the wait, as `trySpawn`
   is `spawn` without it. Its result is `u64?`: the child's number (0 if there was no memory to isolate it, as for
   `spawnIsolated`), or `null` when the set is full or cancelled, and the child is then not started. Its child
   follows `spawnIsolated`'s rules (N0349, N0334, hosted only). It is for a task that cannot wait for room: an
   HTTP/2 or HTTP/3 connection's frame loop starts each stream's handler and refuses the stream when the set is
   full, and the streams it would wait for wait on that loop to send their data. The checker joins `tryRoom`,
   `isolating` and the start as for `trySpawn`, and `TaskSet.startedIsolated` gives the result.
2. **`StreamCloser.shutdown()` ends a connection whose peer has half-closed** (B21). It was `shutdown(SHUT_RDWR)`,
   which macOS (and the BSDs) refuse with `ENOTCONN` once the peer's FIN has arrived, sending nothing: the peer of a
   crashed connection that had ended its side waited for its own timeout. On `ENOTCONN` each direction is now shut
   on its own; `SHUT_WR` is taken there, and the peer reads the end. Linux took `SHUT_RDWR` in that state already.
3. **`ceesafec test` reports an isolated child's trap as neither the test's panic nor its leak** (B22). The runner
   called any test whose standard error held `ceesafe panic` panicked, and an isolated task that ends alone writes
   that line too (§2.5). It now looks for a panic line that does not say `ended alone)`, and reports that one. What
   the crashed units left was counted as live allocations at the end: each budget record (ADR 0170) now counts its
   blocks beside its bytes, a crash adds its unit's blocks (and its root's frame, allocated before the unit was
   made) to a process total, and `ceesafe_shutdown`'s report gives it as `ceesafe alloc: isolated leaked=<k>`, which
   the runner takes off `live`. The interpreter counts the same per unit. A block allocated in a unit and freed
   outside it stays in the unit's count, as its bytes do, so a test whose unit crashed may have that many of its
   own leaks unreported; a block of nested units that both crash is counted twice, and the runner never takes off
   more than `live`.
4. **The interpreter frees a watched channel at its set's end** when the set held the last count (B23): a channel
   made in the set's body and given to `Isolation.watch` was left live, which `ceesafec test --interp` reported as
   a leak. The runtime did this already.
5. **The executor's jump point is a function of its own** (`cee_resume_isolated`), so that the executor's loop keeps
   no local across a `setjmp`: gcc 13.3 warned (`-Wclobbered`) of two (`t`, `outer`) when a program used isolated
   tasks. They were set before the `setjmp` and not changed, which C11 §7.13.2.1 says keeps them; the warning is
   gone without a `volatile`.

Checked (2026-11-19, macOS arm64, the tree's compiler): `tools/isolated_check.py` 14 cases, with `trySpawnIsolated`
in its first program and a peer that half-closes before the owner's closer in its sockets case (which 0.75.0
fails: "the peer saw no end"); `tools/test_command_check.py`, whose fixture gains four tests: an isolated child's
trap that leaves the test passed, one followed by the test's own panic (reported at the test's line) and by the
test's own leak, and a watcher made in the set's body.

## 12. Amendment 2 (2026-11-19): what a crashed unit leaves, counted from what it can reach

### 12.1 Context

Found by CeeSafe.Web isolating HTTP/2 and HTTP/3 streams with 0.77.0 (Web ADR 0016 §2.3; its upstream list, B24).
Amendment 1 (§11 item 3) has `ceesafec test` take off `live` the blocks a crashed unit left, counted as the blocks
allocated under the unit's budget and not freed under it, plus its root's frame. That misses what the unit
**captured**. An isolated child owns its captures (N0349), so a crash leaks them by §2.4's rule. But its parent
allocated them, before the unit existed: a stream's kit of buffers, its request head, its channel handles, its
clone of the application's `Arc`. The HTTP/2 test that traps one stream ends with `live=17` and `isolated
leaked=3`; the other 14 blocks are the stream task's captures, and the test is reported "leaked". The HTTP/3 test
is the same (10 blocks). Every isolated child of a real program captures something, so 0.77.0's discount works only
for a child that captures nothing allocated.

What the count has to say: the blocks still live at the end that are there **because** a unit crashed. Those are
exactly the blocks the dead unit can still reach. It holds no loan (N0349), so what it reaches it owns, or holds a
count of (`Arc`, `Channel`, a `Handover`'s hold): an owned block is never dropped, and a counted one never sees its
last count go. A block the unit cannot reach, it cannot be what keeps alive.

### 12.2 Options weighed

- **A. Count glue at the spawn.** The compiler emits for each captured type a walk like its drop glue that counts
  the blocks it owns, and the unit takes them over when it starts. It cannot see through a type whose `drop` is
  the program's or the library's own (`Arc`, `Channel`, `HashMap`, `String` are library structs with `drop`
  blocks), and whether an `Arc`'s block leaks depends on the counts at the crash, not at the spawn. A capture the
  child drops, moves out or sends away before it traps stays counted, and that hides a real leak of the test's.
- **B. Reachability at the end, in counting runs only (recommended).** While the allocation counters run
  (`ceesafec test`, `CEESAFE_ALLOC_REPORT`, `CEESAFE_FAIL_ALLOC_AT`: memory-model.md), the runtime keeps a table of live blocks
  (address and size). At a crash it records the dead unit's task blocks. At `ceesafe_shutdown` it walks
  conservatively from them: every aligned word of a block that holds the address of a live block in the table
  adds that block, transitively. The blocks found are the ones reported as `isolated leaked=<k>`. They are taken
  as a set, so two units that reach one block count it once, and a block freed since the crash is not in the table.
- **C. No leak check for a test whose isolated task trapped.** Simple and honest, but a test that checks a server's
  crash handling is then never checked for the leaks its own code has, which is the test that most needs it.

### 12.3 Decision (B)

1. **The table.** Only where `alloc_counting` is on: every `ceesafe_alloc`, `ceesafe_alloc_zeroed` and
   `ceesafe_realloc` of the system's or the failing allocator puts the block in an open-addressed table (address,
   size), and `ceesafe_free` takes it out. Under a host's allocator hook, and on CosmoOS, there are no block sizes:
   the table keeps the sizes asked for. Without counting, nothing changes: no table, no cost.
2. **The roots.** At a crash (§2.2 step 4), the dead unit's tasks are the root's block and, through each task's
   scopes and their lists of children (ADR 0062), every task block under it, the units inside it included. Their
   addresses are kept in a list of the process's dead roots, in counting runs only.
3. **The walk,** at `ceesafe_shutdown`, before the report: from the roots, every 8-byte-aligned word of a reached
   block that equals the address of a block in the table, or points inside one, reaches that block. Blocks are
   visited once. The count of reached blocks that are still live is `isolated leaked`, in place of amendment 1's
   budget count. The bytes stay as they are (`TaskCrash.leakedBytes`, `Isolation.report()`): those are what the
   unit held at the trap, by its budget.
4. **The interpreter** does the same over its own allocations: from the dead unit's task frames, every
   pointer-sized value that holds an allocation's address. Its memory model knows each allocation's extent, so its
   walk is exact where the native one is conservative.
5. **The runner** is unchanged from amendment 1: it takes `isolated leaked` off `live`, never below zero.

### 12.4 What it costs and where it can be wrong

- **Cost.** Nothing in a program that does not count allocations. In a counting run: a table insert and a removal
  per allocation (tests already count each one), and a walk once, at the end, only if a unit crashed.
- **Conservative, so it can over-count**, never under-count what the unit reaches. A word in a dead frame that
  happens to equal a live block's address (a stale copy of a pointer the child moved out before it trapped, or an
  integer) adds that block, which hides a leak of the test's own of that block. Over-counting is the safe side:
  a test is never reported leaked for what a crash left.
- **The interpreter's walk is exact**, so the two can differ only in a test with a stale pointer in a dead
  frame, and only in the native run's favour.
- **Amendment 1's budget block count goes.** The bytes it gave and the budget records stay.

### 12.5 Questions for the owner

1. Option B, a conservative walk from the dead units' tasks at the end, in counting runs only? *Recommended: yes.*
   **Accepted.**
2. Keep the table only while allocations are counted, so a program that does not count pays nothing? *Recommended:
   yes.* **Accepted.**
3. Accept that the native walk may over-count from a stale word in a dead frame, hiding a test's own leak of that
   block, rather than under-count and report a crash's leak as the test's? *Recommended: yes.* **Accepted.**
4. Replace amendment 1's budget block count with the walk, keeping `leakedBytes` from the budget? *Recommended:
   yes.* **Accepted.**
5. Release it as a patch release once it is in, and raise CeeSafe.Web's minimum to it for the streams' part of Web
   ADR 0016? *Recommended: yes.* **Accepted.**

### 12.6 Tests (proposed)

- `tests/packages/test-command`: an isolated child that captures a `Vector`, a `Box`, a `String`, a `Channel`
  handle and an `Arc` clone, and traps: passed, natively and in the interpreter; the same, then the test's own
  leak: leaked; a child that sent its captured vector away before it trapped, and the receiver freed it: passed.
- CeeSafe.Web's two stream tests (`web_http2` and `web_http3`, each a stream whose handler traps): passed, and
  the HTTP/2 one in the interpreter too.
- The fault and cancellation sweeps over those, and the contract suite.

### 12.7 Implementation notes (2026-11-19)

- **Native** (`runtime/core/ceesafe_rt_core.c`): the table is open-addressed by address, under a spin lock (blocks are
  allocated on every thread), in the platform heap, only while `alloc_counting` is on; `ceesafe_realloc` moves an
  entry with its block. The roots are the dead unit's root task block and the task that trapped
  (`ceesafe_alloc_dead_root`, from `cee_unit_crash`). The other task blocks are reached from them: a scope's record
  is in its owner's frame and lists its children, and a task names its frame-stack chunks. The walk sorts the
  table (a heap sort, with no C library sort) and searches it for the block that holds each word. **The table and
  the roots take the C library's heap**, not `ceesafe_platform_alloc`: under a host's allocator hook (ADR 0088
  §2.5) the platform functions are the host's, which then saw a block it never got back (`tools/embed_check.py`,
  five of its cases), and are given back at the end. `live_untrack` reads the table's size under the lock: read
  before it, it raced with a constructor's thread that allocated while counting was turned on (ThreadSanitizer, in
  the same check).
- **Interpreter** (`src/interpreter/runtime.cee`, `io.cee`): the roots are the heap blocks the unit's tasks allocated,
  and, for every task of a dead unit, its environment and its frames' locals and slots. **A channel's handle is now
  tagged** (0x7C4A in its high 16 bits, `chanHandle`), so the walk follows a handle the unit holds to the channel's
  buffer while a count of it is held. Before, a handle was the channel's index plus one, a small number the walk
  could not tell from any other. An allocation's address was already distinctive (`(i + 1) << 32`).
- **Amendment 1's block count is gone** (the budget's `blocks`, `ceesafe_alloc_isolated_leaked`); the report line
  and the runner are as amendment 1 made them.

Checked (2026-11-19, macOS arm64, the tree's compiler):
- `tools/test_command_check.py`: the fixture has seven isolated tests, natively and in the interpreter. Two capture
  a `Vector`, a `Box`, a `String`, a channel's handle and an `Arc`'s count and trap: passed, and leaked with the
  test's own leak after. A child that sent its captured vector back before it trapped: passed.
- CeeSafe.Web's two stream tests (B24): `web_http2`'s, natively and in the interpreter, and `web_http3`'s, natively,
  passed. Under 0.77.0 they were leaked: 17 live against 3 counted, and 10 against 0. The interpreter found 11 of 16
  before channel handles were tagged.

