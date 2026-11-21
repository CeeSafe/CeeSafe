# ADR 0203 — Readiness ends a slice: a task that computes is paused when its thread has I/O to serve

- Status: **Accepted** by the owner, 2026-11-21 ("Go ahead", to the fix proposed after an independent benchmark of
  httpserver against Bandit and Cowboy); implemented, released in 0.89.0.
- Scope: the hosted runtime's bound monitor (`runtime/hosted/ceesafe_rt_async.c`: `cee_bound_wait_io`, the slot's
  `io_reactor`, `io_epoch`, `io_flagged` and `io_seen`, `cee_bound_resume`, `cee_bound_waited`), `tools/slice_check.py`
  (one case), type-system.md §40.12, c-backend.md §3b. No language or library change. It amends ADR 0189 §2.3 (the
  word is set when a resume has run past its slice) and its §5 (a neighbour waits up to a slice).

## 1. Context

Under `Async.runSlice` (ADR 0189) a task of an isolated unit that computes is paused once it has run its slice, and
its neighbours run. A request that arrives while such a task computes waits for the rest of the slice, plus the
monitor's lateness: the monitor wakes every quarter of the slice, at least every millisecond, and sets the word from
the wake that first saw the resume. So with httpserver's default 2 ms slice a healthy request beside one that
computes waited 1.5 to 2 ms on average whatever the slice: Erlang's scheduler, which switches every few thousand
reductions, waits a fraction of that.

An independent benchmark ran httpserver against Bandit on one Linux core under 3 000 healthy requests a second beside
two requests computing 200 ms at a time: healthy median 2.4 ms against Bandit's 0.8. Reproduced on a Xeon D-2123IT
(Linux 6.8, epoll), httpserver's `bench` with one worker pinned to one core: p50 1.81 ms, p99 3.41 ms with a 2 ms slice,
1.29 and 3.66 ms with 1 ms, 0.15 and 0.27 ms with no task computing. `slice_check`'s new case on 0.88.0: a reader
beside a unit computing under a 20 ms slice waited 14 to 17 ms on average for each write (worst 25 to 30 ms).

## 2. Decision

The monitor ends a slice when the thread running it has something to serve.

- A resume that runs on a stack of its own (a sliced resume: the only kind a set word pauses rather than ends)
  stores its thread's reactor (the epoll or kqueue descriptor) and its epoch in its slot as it begins, and clears the
  epoch as it ends. Nothing else is added on the executor's thread.
- The monitor, instead of sleeping its period, waits that long with `poll` on the reactors of the slots whose sliced
  resume it has already seen on an earlier look. `poll` on an epoll or kqueue descriptor reports that it has events
  and takes none of them (checked on macOS and Linux). A reactor with events gets its thread's word set for that
  resume, once (`io_flagged`); the task pauses at its next poll that may pause, the executor collects what came
  (ADR 0191) and serves it, and the task goes on after.
- **Only resumes that last.** A resume is watched from the monitor's second look at it, 200 µs or more after it
  began. Most resumes (a request read, answered and waiting again) end sooner and are never paused for readiness,
  which under load is nearly always there. Without this, `/small` at 64 connections on one core served 8.6 % less.
- **The monitor's pace.** While some resume has run past a look in the last 50 ms (a task computes), the monitor
  looks every 200 µs, so a computing task resumed after the request it paused for is watched again at once. Otherwise
  (idle, or only short resumes) it sleeps its period, as before: an idle worker's CPU is unchanged.
- The time still ends a slice: a resume with nothing to serve is paused at its slice, and the bound (ADR 0193) still
  counts what it ran. Windows (IOCP) and freestanding builds keep the time alone.

## 3. Consequences

- A request beside a task that computes waits about the time the monitor takes to see its reactor and the task to
  reach a poll that may pause: a fraction of a millisecond, not most of a slice.
- The monitor thread wakes more often while tasks compute: every 200 µs, or at once on readiness. On a machine where
  it shares the computing thread's core, the kernel's scheduling of it bounds the tail (§4: p99 1.4 to 1.6 ms on one
  core against 0.6 ms on two).
- A task that computes is paused more often while requests come, each pause a switch of stacks: it took 201 ms for
  200 ms of work beside 3 000 requests a second (200 to 205 ms before).

## 4. Implementation and measurements

`runtime/hosted/ceesafe_rt_async.c`; `src/library/runtime.cee` regenerated. `slice_check` case `readiness`: a unit
computes 500 ms under a 20 ms slice beside a task reading what another thread (`Thread.spawn`, its own `Async.run`)
writes over TCP every 13 ms; each write is read within a few ms (mean <= 3, longest <= 8). macOS arm64: mean 0,
longest 0 to 1 ms in five runs, against 14 to 17 and 29 to 30 on 0.88.0; Linux x86-64: passes, and fails on 0.88.0
(mean 15, longest 25). `slice_check` 15 cases and `bound_check` 33, 0 problems, on both.

httpserver's `bench`, one worker, on the Xeon, 3 000 healthy requests a second over 30 connections beside two clients
computing `/spin/200` one after another, 2 ms slice (two runs each):

| | p50 | p90 | p99 | p99.9 |
|---|---|---|---|---|
| 0.88.0, one core | 1.81 ms | 3.02 ms | 3.41 ms | 5.4 ms |
| ADR 0203, one core | 0.30 to 0.32 ms | 0.50 to 0.53 ms | 1.35 to 1.56 ms | 3.0 to 3.3 ms |
| ADR 0203, two cores | 0.29 to 0.34 ms | 0.51 ms | 0.60 to 0.62 ms | 1.9 to 3.0 ms |

`/small` over HTTP/1.1 at 64 connections, one core, five alternating runs: 71 881 requests a second against 72 178 on
0.88.0 (0.4 %, within the runs' spread). An idle worker's CPU over 10 s: 12 ticks against 11. The crash probe of Web
ADR 0021 at 200 crashes a second: p99 4.8 ms, no request over 100 ms.

Tried and left: a pipe the executor wrote to when a sliced resume began, to wake the monitor at once (p50 0.19 ms,
but p99 4.6 ms on one core, the monitor's wakes taking the core); a 200 µs nap whenever sliced resumes began at all
(p50 0.13 ms, but 8.6 % less throughput, every short resume paused).
