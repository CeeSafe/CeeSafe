# ADR 0094 — A thread pool for `Async.blocking`

- Status: **Accepted** by the owner, 2026-11-11 (every question of §6 answered as proposed: a pool per
  `Async.run`, a 10-second idle timeout without a cap, released alone; ADRs 0092, 0091, 0095 and 0093 with 0096 and
  0097 took 0.32.0 to 0.35.0 the same day, so this is 0.36.0). Released with ceesafec 0.36.0. Raised by
  CosmoStore's CEESAFE_GAPS G28 (`~/dev/CeeSafeApps/cosmostore/CEESAFE_GAPS.md`) and its profile of 1 MiB GETs
  (`~/dev/CeeSafeApps/cosmostore/docs/benchmarks/2026-11-11-get-1mib-profile.md`).
- Scope: `runtime/hosted/ceesafe_rt_async.c` (`ceesafe_async_blocking_*`, `ceesafe_async_run`, the workers of
  `Async.runWorkers`) and the comment of `ceesafe_blocking` in `runtime/include/ceesafe/rt.h`. No language, library,
  checker or emitter change; type-system.md §40.4 says "a worker thread" and stays true.

## 1. Context

- **Each `Async.blocking` call starts a thread and joins it.**
  - `ceesafe_async_blocking_start` calls `ceesafe_thread_start(body, arg)`;
  - the body ends with `ceesafe_async_blocking_finish`, which sets `done` and wakes the task;
  - `ceesafe_async_blocking_ready` joins the thread once `done` is set.
- **N0295 sends every blocking step of async code through it.** In a server that is every file read and write, and
  every store call.
- **What it costs, measured with ceesafec 0.30.0** (a loop of `await Async.blocking(() => 1u64)`, 20,000 calls):

  | Host | Sequential | 32 tasks at once |
  |---|---:|---:|
  | Debian 13, Xeon E3-1245 V2, two pinned cores | 20 to 21 µs a call | 14 µs |
  | M1, macOS (load average 26 to 154, so indicative) | 18 to 44 µs | 22 to 26 µs |

- **In CosmoStore:**
  - at concurrency 32, starting threads (`clone`, `pthread_create`) was 15% of the event loop's samples during
    1 MiB GETs on Linux;
  - the event loop was never idle, so that 15% bounds the throughput;
  - on macOS, a sample of 0-byte PUTs showed 579 threads in 4 seconds.
- **What others do:** Tokio's `spawn_blocking` and libuv's thread pool both reuse threads.

## 2. Proposal

### 2.1 A pool per run

- **Who owns it:** each `Async.run` has a pool of worker threads for its `Async.blocking` calls, shared by the
  run's workers when it has several (`Async.runWorkers`, ADR 0038).
- **Where it lives:** in `ceesafe_async_run`'s frame, next to its `cee_run`. Its lock and condition variable are
  initialised in place (`pthread_mutex_init`, `InitializeSRWLock`, `COSMO_MUTEX_INIT`), which allocates nothing
  from `ceesafe_alloc`. A run that never blocks allocates nothing for it, so the allocation points the
  fault-injection sweep counts do not move.
- **How executors reach it:** each executor gets a pointer to it, as it gets `run`.
- **When it ends:** before `ceesafe_async_run` returns. Every blocking call has finished by then (a wait is not
  cancelled, §40.4). Its idle threads are told to stop and joined, and their records freed, so a run leaves no
  threads and no allocations behind.
- **Nesting:** a nested `Async.run`, including one inside a blocking body, has its own pool.

### 2.2 A call

- **Start** (`ceesafe_async_blocking_start`):
  - an idle worker takes the call: `body`, `arg` and the `ceesafe_blocking`;
  - with none idle, a new worker is started, whose record comes from `ceesafe_alloc` (`CEE_ALLOC_SYSTEM`);
  - if neither works, the body runs on the calling thread through `ceesafe_scratch_aside`, as today.
- **No limit on concurrent calls.** Every call gets a thread at once, as today. A bound could deadlock bodies
  that wait on each other, for instance through a channel. §40.4 does not promise that such bodies run at once
  (a body runs on the calling thread when no thread can be had, and the interpreter has one thread), but today
  they do, and a pool should not take that away.
- **The body is unchanged.** It runs on a thread whose scratch stack is empty. The previous body dropped every frame
  it entered (§19.5), and type-system.md line 1396's "as on a thread of its own" holds.
- **One thread-local is reset:** `cee_next_workers`, which a body could set without running. `cee_exec` and
  `cee_cancel_root` are already saved and restored by a nested run.

### 2.3 Ending a call without a join

Today the join is what keeps the task's frame alive while the worker leaves it: `finish` sets `done`, then reads
`b->executor` and `b->task` to wake the task. Without the join, a task woken for another reason could see `done`,
end and free its frame while the worker is still in `cee_wake_task`.

- **`done` gets three values:**
  - 0: running;
  - 1: the result is written and the worker is waking the task;
  - 2: the worker no longer touches `b`.
- **The worker** stores 1, wakes the task, stores 2, and returns to the pool.
- **`ready`** is true only at 2. At 1 it yields until 2 instead of parking. The wake has already happened, and the
  worker is only leaving `cee_wake_task`. This is the wait the join made before, a shorter one.
- **Memory order:** the stores are release and the loads acquire (`ceesafe_atomic_*` are sequentially consistent),
  so the result written before 1 is seen after 2.

### 2.4 Idle workers

- **Parking:** a worker with no call waits on its own condition variable, under the pool's mutex.
- **The idle timeout:** a worker idle for 10 seconds leaves the pool and ends. Its record goes on a list that the
  next call's start, or the end of the run, joins and frees; a thread that has ended joins at once.
- **So the pool follows the load:** a burst of 128 calls leaves 128 threads, and 10 seconds later none. A server
  under steady load keeps about as many threads as it has calls in flight.
- **Platforms:**
  - POSIX: `pthread_cond_timedwait`;
  - Windows: `SleepConditionVariableSRW` with a timeout;
  - CosmoOS: `cosmo_cond_timedwait` (ADR 0040).
- **Freestanding** keeps no blocking threads (profiles.md §4.4), as today.

## 3. Alternatives considered

- **A. A process-wide pool.** Threads would outlive runs, so `ceesafec test`'s leak check would see their records
  at exit. It would also need global state and a shutdown hook. Per run, a test's run ends clean.
- **B. A fixed number of workers** (libuv's default is 4). It can deadlock bodies that wait on each other, and it
  queues work that today runs at once.
- **C. A cap on idle workers, without a timeout.** It is simpler, but a burst above the cap pays thread creation
  again, and a cap below the steady load does too.
- **D. Moving blocking work into the reactor** (io_uring, IOCP) for files: a much larger change, and only for I/O.
  The pool helps every blocking body.

## 4. Not in this ADR

- **Thread affinity, priorities and stack sizes:** workers get the platform's default stack, as threads do today.
- **A setting for the timeout or a cap:** none is proposed. 10 seconds is Tokio's `thread_keep_alive` default.

## 5. Testing

- **The corpus as it is:**
  - `async-blocking`, `async-blocking-stale-wake` (a task woken before its worker is done);
  - the cancellation sweeps (`CEESAFE_CANCEL_AT`);
  - the fault-injection sweep, where a worker record that cannot be allocated runs the body on the calling thread;
  - the leak checks.
- **New run-pass tests:**
  - many calls in sequence, and from many tasks at once, with results checked;
  - a blocking body that runs a nested `Async.run`, which itself blocks;
  - calls from `Async.runWorkers(4, …)`.
- **The timeout:** `CEESAFE_BLOCKING_IDLE_MS` (read by `ceesafe_platform_config`, as `CEESAFE_REACTOR_POLL` is)
  shortens it for tests. The run-pass corpus runs again with 1 ms, so that workers leave and are joined between
  nearly every two calls, with the leak check.
- **Sanitizers:** ThreadSanitizer and AddressSanitizer runs of those tests on Linux, as for ADR 0038.
- **Measurements:** the loop of §1 before and after, and CosmoStore's 1 MiB GETs at concurrency 32 on pinned cores.

## 6. Questions for the owner

1. **Scope of the pool:** per `Async.run` (proposed), or one for the process (A)?
2. **Idle workers:** a 10-second timeout with no cap (proposed), or a cap (C)? Or both?
3. **Release:** 0.32.0 with this alone (proposed, so CosmoStore can take it), or with whatever is next (ADRs 0091
   to 0093 are under way in other sessions)?

## 7. Implementation and evidence

- **Where:** `runtime/hosted/ceesafe_rt_async.c`, the section `cee_bpool` before the end of `Async.blocking`'s
  part: the pool (`cee_bpool_init`, `_give`, `_main`, `_end`), the three-state `done` in
  `ceesafe_async_blocking_finish` and `_ready`, the pool in `ceesafe_async_run`'s frame and a pointer to it in each
  executor (`cee_executor.pool`, passed to the workers of `Async.runWorkers`). The name is `cee_bpool` because
  `rt.h` already has a `cee_pool` (the region pool). `rt.h`: the comment of `ceesafe_blocking`. No emitter change:
  the thunk already ends with `ceesafe_async_blocking_finish`.
- **A test removed while writing it:** a first version of `run-pass/async-blocking-pool` had two bodies hand a
  value to each other through a channel. It deadlocked in the interpreter, and in the fault sweep when a worker record
  could not be allocated and the body ran on the calling thread. §40.4 does not promise that two bodies run at once
  (§2.2), so the case is not a test; it is why §2.2 keeps no bound.
- **Checks (macOS, M1):**
  - `tools/contract_tests.py` on the branch, rebased on 0.32.0, 0.34.0 and 0.35.0: every check passes
    (`spec_check` after `gen_docs.py` and a rebuild: 0 problems);
  - the corpus with `--check-leaks`, compiled and interpreted: run-pass 320, run-fail 51, ffi 18;
  - the run-pass corpus with `CEESAFE_BLOCKING_IDLE_MS=1`: 319 of 319, with the leak check;
  - `tools/fault_injection.py`: 319 run-pass programs, 3065 failure points, 0 failures.
- **Checks (Debian 13, x86-64, clang and gcc 14, on 0.34.0 with and without the branch):**
  - the run-pass corpus under AddressSanitizer and UBSan with `-Werror` and the leak check: 321 of 321 (320 of 320
    without);
  - the async programs on the poll reactor with the leak check: 51 of 51, and under ThreadSanitizer: 51 of 51, three
    runs with gcc, one with clang;
  - the fault sweep of the blocking programs: 171 points, 0 failures;
  - the gcc `-Werror` checks fail on 0.34.0 as on the branch: `rt.h`'s `cee_mul_high_u64` (ADR 0092) uses
    `__int128`, which `-pedantic` rejects. Not this ADR's.
- **Measurements** (the loop of §1, 20,000 calls; ceesafec without and with the pool):

  | Host | Sequential | 32 tasks at once |
  |---|---:|---:|
  | Debian 13, two pinned cores | 20 to 27 → 7 to 9 µs a call | 14 to 17 → 4 to 6 µs |
  | M1, macOS | 16 → 5 µs | |

- **CosmoStore** (b712ef8 built with 0.31.0 and with the pool, server on two pinned cores, `tools/perf_gate.sh`,
  5 alternating rounds): the gate passed; small GETs at concurrency 32 1.43 times the operations a second (4 KiB:
  10,331 → 14,421), 1 MiB PUTs at 32 1.28 times (p99 0.82 times), small writes 1.07 to 1.10 times, the rest within
  noise. An alternating A/B of 1 MiB GETs at 32: about 839 → 1,073 a second; at 128 unchanged (about 740).
- **Not run:** Windows, CosmoOS.
