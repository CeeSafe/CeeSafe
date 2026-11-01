# ADR 0040 — The hosted executor in CosmoOS userland

- Status: **Accepted** by the owner, 2026-11-01 ("tackle the open works", of which this was one); after review, the
  three choices of §5 accepted as proposed ("I accept ADR 0040's choices as proposed").
- Scope: running async programs (type-system.md §40) as CosmoOS user programs, the target `integrations/cosmoos/c1.py`
  builds for (`CEE_COSMOOS`; profiles.md §2).

## 1. Context

A CeeSafe program built for CosmoOS userland compiles the hosted runtime with `-DCEE_COSMOOS` against CosmoOS's own
libc. Since the async work (ADRs 0031–0038) the hosted executor, `runtime/hosted/ceesafe_rt_async.c`, needs:

- `poll()`, or kqueue, epoll or io_uring, to wait for descriptors and deadlines;
- a pipe made non-blocking with `fcntl`, which another thread writes to end that wait;
- `pthread` mutexes and condition variables for channels (ADR 0032) and `sched_yield` while spinning.

CosmoOS's libc, surveyed read-only at CosmoOS `25742b40`, has none of these: no `poll.h`, `pthread.h`, `sched.h` or
`fcntl()`. A descriptor there waits or not per call (`MSG_DONTWAIT`) or through CosmoOS's own completion ring
(`SYS_aio_*`), never by a flag set on it. It does have, as public libc functions:

- threads, `cosmo_thread_start`/`join` (`libc/include/cosmo/thread.h`), which `ceesafe_rt_thread.c` already uses;
- `cosmo_mutex_t` and `cosmo_cond_t`, with `cosmo_cond_timedwait(c, m, timeout_ns)`;
- `clock_gettime(CLOCK_MONOTONIC)` and `nanosleep`.

So no async program builds for CosmoOS userland (`c1.py --corpus` skips all 22), and since commit `ea582b7` opening
an async socket there is `ENOSYS` (`ceesafe_rt_io.c`).

## 2. Options

| | A. A CosmoOS backend in the hosted executor (proposed) | B. The freestanding executor (ADR 0039) with userland hooks | C. A backend on CosmoOS's completion ring |
|---|---|---|---|
| What runs | everything hosted async does but sockets: timers, scopes, cancellation, shields, channels, `Async.blocking`, `AsyncFiles`, `runWorkers`, `spawnSend`, `AsyncConsole`, `AsyncSignal` | only what freestanding allows: no `blocking`, `runWorkers`, `spawnSend` or channels between threads (§40.5, N0293) | as A, and async sockets |
| Waiting | the executor's own condition variable, timed to the earliest deadline; another thread's wake signals it | the environment's `idle` and `wake` hooks | the ring: completions, timeouts, and a posted wake |
| Descriptors | count as ready, as consoles and files do on Windows (ADR 0035): the task retries at once | none | completions |
| Cost | one `#if` per platform service in one file | a second set of hooks for userland, and programs lose the hosted API | the ring's ABI is CosmoOS's, not libc's: raw syscalls, which L1 (OD-15) forbids until the CosmoOS owner rules |

**A is proposed.** It gives CosmoOS programs the whole hosted API but sockets, through public libc only, and keeps
the executor's tested bookkeeping unchanged. C is the way to sockets later, once OD-15 allows the ring.

## 3. Decision

1. **`CEE_COSMOOS` has no reactor.** The executor's descriptor table, kqueue, epoll, io_uring and `poll()` paths are
   not compiled there.
2. **Waiting and waking.** With no task ready, the executor waits on its own `cosmo_cond_t` under its own
   `cosmo_mutex_t`, timed to the earliest deadline (or untimed if there is none), until a wake: another thread, a
   finished `Async.blocking` call, a channel, a `runWorkers` peer or an `AsyncSignal` raise, signals it after
   setting a pending flag, so a wake that comes before the wait is not lost. This replaces the wake pipe.
3. **Descriptors count as ready.** A task parked on a descriptor (console output, `AsyncConsole`) is readied by the
   next wait, as Windows does for consoles and files; its operation then runs, and may block the worker for as long
   as a console write or read takes. `ceesafe_async_poll` reports every descriptor ready in the directions asked,
   and sleeps for the timeout when there is none.
4. **Channels** use `cosmo_mutex_t` and `cosmo_cond_t` where other hosts use pthreads, and spin with plain retries
   where they call `sched_yield`.
5. **Async sockets stay `ENOSYS`** (ADR text in `ceesafe_rt_io.c`; profiles.md §2.2).

## 4. Consequences

- Async programs build and run as CosmoOS user programs, and the c1 corpus run covers them, but for the three that use
  async sockets.
- A console read through `AsyncConsole` blocks its worker until a line comes, where a host with readiness would run
  other tasks meanwhile. Programs that need both wait in `Async.blocking`.
- Linux, macOS, Windows and freestanding builds are unchanged.

## 5. Choices the owner reviewed (accepted as proposed)

1. A rather than C: sockets wait for OD-15's ruling on raw syscalls (the ring has no libc wrapper).
2. Descriptors count as ready, as on Windows, rather than being refused.
3. A condition variable per executor, rather than a futex word (`cosmo_futex_*` are raw syscalls under OD-15).
