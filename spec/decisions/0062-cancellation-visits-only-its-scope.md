# ADR 0062 — A cancellation visits only the cancelled scope's tasks

- Status: **Accepted** by the owner, 2026-11-03 ("I accept ADR 0062 with your recommendations"): every question of §5
  as recommended. Asked for by the owner ("Merge and release 0.10.2, then draft the B17 ADR") after CeeSafe.Web's item
  B17 (`~/dev/CeeSafe.Web/docs/ceesafe-upstream-work.md`).
- Scope: the hosted runtime's cancellation (`cee_cancel_scope`, `cee_wake_waiters`), the task and scope records in
  `ceesafe/rt.h`, the backend's layouts for them (`taskSlotLayout`, `scopeLayout` in `src/backend/c_async.cee`), and
  the interpreter's model of a task's head. No language or library change.

## 1. Context

A cancellation must reach every task it cancels, wherever that task waits (type-system.md §40.7). Today that is
`cee_wake_waiters` (`runtime/hosted/ceesafe_rt_async.c`), which walks **every task parked on the executor**. For each
task it climbs the task's scopes in `cee_cancelled` and unparks the ones that are cancelled. B5 (CeeSafe.Web, fixed in
0.8.0) made the walk wake only those tasks. The walk itself still visits all of them.

It runs:

- on each scope cancellation: `cee_cancel_scope`, called when a race is won, when a scope's child fails, or when a
  spawn cannot be allocated;
- on the cancellation sweep's cancellation of the root;
- on another worker's cancellation of a shared scope (`cancel_epoch`, ADR 0038).

The first is the common case. Every `Async.race` that a child wins cancels the others, so one race costs O(parked
tasks on the executor), whatever the race's own size. A server keeps a task parked per connection, per request and per
wait, and races to bound waits (a read against a deadline, a handler against a reset). So the cost grows with the
square of its concurrency.

CeeSafe.Web measured it with HTTP/2. There, a task per stream and a race per connection wake and per stream (handler
against reset) make it plain. `examples/server` with `h2load -n 20000`, built by ceesafec with B16's fix (the code
released as 0.10.2), macOS arm64:

| connections × streams | req/s | mean request |
|---|---|---|
| 10 × 100 | 21 944 | 45 ms |
| 50 × 10 | 22 308 | 22 ms |
| 50 × 30 | 15 860 | 93 ms |
| 20 × 100 | 13 871 | 144 ms |
| 50 × 50 | 11 782 | 212 ms |
| 50 × 100 (`-n 100000`) | 3 562 | 1.40 s |

`sample` of the server at 50 × 100 puts 895 of about 2 400 samples in `cee_cancel_scope` and 1 393 in
`cee_exec_loop`, into which the walk's helpers are inlined. HTTP/1.1 on the same server, with one task per connection,
does not show it at 100 connections (42 076 req/s).

## 2. Options

| | A. Each scope lists its children (proposed) | B. A parked list per scope | C. Keep the walk; make it cheaper |
|---|---|---|---|
| What a cancellation visits | the cancelled scope's live tasks, and the scopes they own, recursively | the parked tasks of the cancelled scope and its descendants | every parked task |
| Cost per race | O(the race's children and what they own) | the same | O(parked tasks), with a smaller constant |
| Record size | two links and a list head per task, a list head and a link per scope | the same, and a second membership to keep when a task parks and unparks | none |
| Other workers' tasks | a shared scope's children may run on other workers: those keep today's epoch walk (§3.3) | the same | the same |

**A is proposed.** It visits exactly what the cancellation reaches, its links change only when a task starts or
returns and when a scope starts or ends, never on each park. B would also keep a membership on every park and unpark.
That is the hot path, so it costs more and gains nothing. C leaves the quadratic cost in place.

## 3. Decision

### 3.1 The records

- A task (`cee_task`) gains `sibling_next` and `sibling_prev`, linking it into its scope's list of live children, and
  `owned`: the innermost scope it owns that has not ended. A task owns several scopes at once when, for example, a
  task set's body awaits a race. Those scopes nest, last in first out, as frames do (ADR 0056 §1).
- A scope (`ceesafe_scope`) gains `children`, the head of its live children's list, and `outer`: the scope its owner
  owned before this one, so that `owned` is a stack.
- A child joins its scope's list when it is spawned, started (`ceesafe_async_start`), or embedded (race, all). It
  leaves when it returns, before its slot or block is released. A scope is pushed on its owner's `owned` when its first
  child is added, and popped when its last child has returned. Both happen where `live` already changes.

### 3.2 A cancellation

`cee_cancel_scope(scope)` sets `scope->cancelled` (as now), then visits:

- **each child in `scope->children`:**
  - If it is parked, waiting on a deadline, a descriptor, or a cancellable wake (the condition `cee_wake_waiters` uses
    today), it is unparked to see the cancellation.
  - A task parked `on_scope` stays parked: its own children are visited, and the last to return readies it, as now.
- **each scope the child owns** (its `owned` stack), with that scope's children visited the same way, depth first.

A task already ready or running is passed over: it sees the cancellation at its next check (§40.7), as now. The visit
uses an explicit stack in the executor, sized by the deepest nesting met, so a deep tree does not recurse in C.

### 3.3 What stays as it is

- **Shared scopes** (`spawnSend` children, ADR 0038). Their children may be parked on other workers, whose lists this
  thread cannot walk. A visit that reaches a shared scope does today's: it increments the run's `cancel_epoch` and
  pokes the workers, which walk their parked tasks (`cee_wait`). Only programs that use `spawnSend` with several
  workers pay for it, and only when a shared scope is cancelled.
- **The cancellation sweep** (`CEESAFE_CANCEL_AT`) cancels the root, which reaches every task. It keeps the full walk.
  It is a test, and its cost is the walk's.
- `cee_cancelled`, shields, and what a cancelled wait returns do not change.

### 3.4 Layout

- `cee_task` grows by 24 bytes. `ceesafe_task_slot` goes from 160 to 192 bytes (kept a multiple of 16). The
  backend's `taskSlotLayout` and the interpreter's model of a task's head follow, as in ADR 0056 §5.
- `ceesafe_scope` grows by 16 bytes: `scopeLayout` goes from 32 to 48. Generated code zero-initialises scopes, and a
  zero list is an empty one.
- A frame with a race or a scope grows accordingly. The C size checks (`_Static_assert`) catch any disagreement.

## 4. Consequences

- A race's cost no longer depends on how many tasks wait elsewhere on the executor. For CeeSafe.Web's HTTP/2 server,
  the 50 × 100 run should match the 10 × 100 run per stream (to be measured, §6).
- Each spawn and return does two more pointer writes. Each scope's first child and its end do one more.
- B5's check (`tools/wake_check.py`: 200 races beside 100 idle sockets, tasks woken) still holds. It gains a count of
  tasks *visited*, which must not grow with the idle tasks.

## 5. Questions for the owner

1. Option A, each scope listing its children, with the visit of §3.2? Recommended: yes.
2. Shared scopes and the sweep keep the full walk (§3.3), for now? Recommended: yes. A per-worker list for shared
   scopes can come when a program needs it.
3. `ceesafe_task_slot` at 192 bytes and `ceesafe_scope` at 48 (§3.4)? Recommended: yes.
4. A patch release (0.10.3) when it lands, since only performance changes? Recommended: yes.

## 6. Plan and tests (once accepted)

1. Runtime: the links and their upkeep (spawn, start, embed, return, a scope's first child and end), the visit, and
   the fallbacks of §3.3; `tools/gen_runtime.py`.
2. Backend and interpreter: the two layouts.
3. Tests:
   - `tools/wake_check.py` counts tasks visited per cancellation. With 1 000 idle tasks parked beside 1 000 races,
     each race must visit only its own children. Today each visits about 1 000 (to be confirmed by the check before
     the change).
   - The corpus's cancellation programs, the cancel and fault sweeps, ASan and TSan, `run interp`, and
     `reactor poll`: unchanged results.
   - Nested cancellation: a race inside a task set inside a race, cancelled from the outside. Every level's waiting
     tasks must see it, checked by their results in the corpus.
   - A shared scope with several workers is cancelled while its children are parked on other workers
     (ADR 0038's tests, extended).
   - CeeSafe.Web's `h2load` table of §1 again, before and after.

## 7. Implementation notes (2026-11-03)

- **The records:**
  - `cee_task` gains `listed`, `sibling_next`, `sibling_prev` and `owned`, and is 192 bytes.
  - `ceesafe_scope` gains `listed`, `children` and `outer`, and is 48 bytes; `TaskSetRecord`'s 96 bytes still hold a
    set (88).
  - `taskSlotLayout`, `scopeLayout` and the interpreter's `CEE_TASK_HEAD` (`src/interpreter/io.cee`) follow.
- **The upkeep:**
  - `cee_spawn` and `ceesafe_async_start` list the child, unless it is a published `spawnSend` child, and list the
    scope on its owner.
  - `cee_child_done` unlists the child before its block is released.
  - `ceesafe_async_scope_done` unlists the scope when it returns true. That is every scope's end, a task set's
    included: a set's live count may reach zero and rise again while a race nested in its body is still listed.
  - A scope that another task spawns into moves to that task's list, as `scope->owner` does.
- **The visit** (`cee_cancel_visit`) walks down, across and up through the records' own links (owner, scope, sibling,
  `outer`), so it keeps no stack. A shared scope met on the way ends it with `cee_wake_waiters`.
- **`CEESAFE_CANCEL_REPORT`** adds `visited=V`, the tasks cancellations looked at. The prefix parsers
  (`cancel_sweep.py`, `ceesafec test`) are unchanged.
- **The wake list, found on the way (§1's server):**
  - With the visit in place, a profile of the HTTP/2 server built without inlining put about 40 % of the time in
    `cee_wait`: it found woken tasks one at a time, walking the whole wake list for each. That is quadratic, since
    every HTTP/2 connection and stream waits on a channel.
  - `cee_ready_woken` now takes them in one pass into the executor's `woken` buffer and heap-sorts them by wake
    sequence, so they still run earliest wake first. If the buffer cannot grow, the old walk remains.
  - No layout or behaviour changes, so it ships with this ADR.
- **Measured (2026-11-03, macOS arm64):**
  - `tools/wake_check.py`: 200 cancellations looked at 400 tasks, on the reactor and on poll. The check now requires
    at most 400.
  - A benchmark of 20 000 races whose losers park, beside idle tasks parked on a channel:

    | idle tasks | 0.10.2 | this ADR |
    |---|---|---|
    | 0 | 2 ms | 3 ms |
    | 1 000 | 144 ms | 7 ms |
    | 2 000 | 287 ms | 16 ms |
    | 4 000 | 594 ms | 56 ms |

  - CeeSafe.Web's `examples/server`, `h2load -n 20000` (`-n 100000` for 50 × 100), req/s:

    | connections × streams | 0.10.2 | the visit | the visit and the wake list |
    |---|---|---|---|
    | 10 × 100 | 21 944 | 41 061 | 75 812 |
    | 50 × 10 | 22 308 | 43 242 | 73 456 |
    | 20 × 100 | 13 871 | 29 708 | 73 829 |
    | 50 × 50 | 11 782 | 23 833 | 68 880 |
    | 50 × 100 | 3 562 (mean 1.40 s) | 7 880 | 70 956 (mean 70 ms) |
    | 100 × 1 | 10 698 | 30 380 | 37 297 |

- **Tests:**
  - corpus `run-pass/async-cancel-nested.cee`: an outer race's loser owns a task set. The set's 8 children and its
    body each wait in a race against a 100 s sleep. All 9 see the cancellation, and it ends in 0.24 s natively and in
    the interpreter. It also passes on 0.10.2, whose full walk was correct, only slow.
