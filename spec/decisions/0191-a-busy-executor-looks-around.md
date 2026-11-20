# ADR 0191 — A busy executor looks around: events and timers while the queue is full, paused tasks in a queue of their own

- Status: **Accepted** by the owner, 2026-11-20 ("yes, fix it in CeeSafe's executor"), after httpserver's benchmark
  against Elixir (httpserver's `docs/elixir-benchmark.md`) found the two faults below; implemented, not released.
- Scope: the hosted runtime's executor (`runtime/hosted/ceesafe_rt_async.c`, `cee_exec_tasks` and `cee_collect`),
  `tools/slice_check.py` (two cases), c-backend.md §3b and §6. No language or library change.
- It amends ADR 0189 §2.3 step 3 (a paused task "goes to the back of the ready queue") and the epoll paragraph of
  c-backend.md §6 (a busy executor collected events only after a wait that ended without its event).

## 1. Context

httpserver's benchmark against Bandit (Elixir), on a 4-core Xeon with 4 workers, found two faults that Bandit does
not have, both of one cause: the executor looks at its reactor, its woken tasks and its timers only when no task is
ready (`cee_wait`), after a task is paused past its slice (`cee_slice_collect`), and under epoll after a wait that
ended without its event (`stale`, every 61 resumes).

1. **Timers never fire under a full queue.** At 128 h2c connections (and at 16 for a 1 MiB stream), a worker's
   HTTP/2 tasks kept its queue full; its heartbeat, a timer, never ran; and the supervisor killed every worker with
   SIGKILL 2 s later while it was serving (gdb: `web_http2_handle`, `send`). The h2c rate fell to a fifth of
   Bandit's.
2. **Every wake behind a paused task costs a slice.** A paused task went to the back of the one ready queue, after
   the tasks its collection readied but before every task those then woke. A request that hands work from task to
   task, as an HTTP/1.1 connection and its isolated request do (Web ADR 0017, three channels), waited a whole slice
   at each hand-off. One worker beside one client computing 100 ms at a time: healthy requests waited 75 ms under a
   10 ms slice and the whole 100 ms with per-request isolation. On the benchmark machine, with four computing
   clients beside 10 000 requests a second, healthy requests waited 3.6 to 10 s; Bandit's p99 was 3.7 ms. The same
   fault made httpserver's CI fail on macOS (`slice: beside /spin, five requests are answered, the median after
   0.232 s (< 0.2)`).

## 2. Decision

### 2.1 A busy executor collects every millisecond

Every 61 resumes (`CEE_POLL_EVERY`, now on every platform) while a task is ready, the executor reads the clock, and if
a millisecond has passed since it last collected, collects without blocking (`cee_collect`, the former
`cee_slice_collect`, now built for every hosted target): the reactor or `poll` with a timeout of 0 (Windows: its
wait with 0; CosmoOS: its own), the woken tasks, and the due timers, all readied behind the tasks already ready. This
replaces the epoll-only `stale` rule, which it covers: a read raced against a deadline sees what came within about a
millisecond.

A collection every 61 resumes, without the clock, cost h2c 15 % of its rate on macOS (a `kevent` every few
requests, since an HTTP/2 request is many resumes); with the millisecond it is within the noise (three alternating
runs each, HTTP/1.1 at 64 connections and h2c at 128: 88 to 89 thousand and 48 thousand requests a second, before and
after).

### 2.2 Paused tasks wait in a queue of their own

A task paused past its slice goes to the back of the executor's **paused queue**, after what came meanwhile is
collected, as before. The executor takes the head of the paused queue

- when no other task is ready, after collecting without blocking (so a socket's data or a timer that came during
  the slice runs first), or
- when a slice has passed since a paused task's resume last ended (`paused_ran`), whatever is ready,

and otherwise takes from the ready queue. A task one of the ready tasks wakes therefore runs before the paused one,
not a slice later; the paused tasks are never starved, since between two of their resumes the others have at most
one slice; and two paused tasks take turns as before. A paused task stays on its thread: the paused queue is the
executor's, which no other worker steals from. The debugger's task list shows its tasks as "paused past its slice".

## 3. Results

- **One worker, one client computing `/spin/100` in a loop, 8 healthy keep-alive connections** (httpserver's bench,
  macOS M1): healthy requests per half second and the worst wait went from 72–80 and 75–83 ms to 328–357 and
  13–15 ms under a 10 ms slice, from 240 and 18 ms to 1 470 and 5–6 ms under 1 ms, and from 41–56 and 100 ms to
  328–360 and 15–23 ms with per-request isolation. The computing client was served as before (40 spins of 100 ms).
- **`tools/slice_check.py`**, two new cases, which fail on 0.82.0 and pass now:
  - `handoff`: twenty round trips between two tasks beside a unit that computes 300 ms under a 10 ms slice took
    288 ms on 0.82.0, under 100 ms now;
  - `busy`: two tasks that pass a number to and fro for 300 ms keep the queue full beside a task waiting on 1 ms
    timers; its longest wait was 300 ms on 0.82.0, with or without a slice, and is under 50 ms now.
- `slice_check` 15 cases and `bound_check` 33: 0 problems. The contract suite: see the ROADMAP's row.

## 4. Alternatives

- **Collect every 61 resumes without the clock**: 15 % of h2c's rate (§2.1).
- **A paused task only when nothing else is ready**: starves it under a load that keeps the queue full, the fault this
  ADR removes from timers.
- **A shorter default slice**: it shortens each hand-off's wait but not their count, and costs every computing task
  more switches; the queue is the fault.
