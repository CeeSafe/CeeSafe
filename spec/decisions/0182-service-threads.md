# ADR 0182 — Service threads

- Status: **Accepted** by the owner, 2026-11-19 (asked about CeeUsb's finding F4: "Service threads"): a thread that
  runs for the process, never joined, captures owned, the "no detached threads" rule restated with that one named
  exception. Implemented the same day released in 0.79.0. One difference from the question
  as asked, for the owner to confirm: it was put as "a lib/std `Thread.service`"; it is a member of the built-in
  `Thread`, beside `spawn` (§3).
- Scope: type-system.md §38.2; c-backend.md §6 (threads); the runtime. Finding F4.

## 1. Context

A `Thread<T>` that is dropped is joined, and every thread ends before `main` returns. A USB library's event loop
(epoll on Linux, a CFRunLoop on macOS, a completion port on Windows) lives for the process and is started lazily
from library code, which has nowhere to keep a handle and must not make `main` wait for it at exit. CeeUsb started it
with `pthread_create` and `pthread_detach` (`CreateThread` on Windows) through an `@callback` entry point.

## 2. Decision

- **`Thread.service(name, move () => body) -> Result<void, SpawnError>`**: `name` a `StringView`; the literal as
  `Thread.spawn`'s (no parameters, captures `Send` and owned, N0333, N0334, N0332) with a `void` body.
- The thread is never joined. When `main` returns the process ends with it wherever it is, and what it holds is not
  dropped, as nothing of the process is. A body that returns drops what it did not move and frees its environment on
  its own thread.
- It is named: the first 15 bytes, set from the thread itself (`pthread_setname_np` on Linux and macOS,
  `SetThreadDescription` where Windows has it), for debuggers and `ps`/`top`.
- Effects: `sleep` and `alloc`, as `spawn`.
- §38.2's "There are no detached threads" now names this exception.

## 3. Options not taken

- **A lib/std function** (as the question put it): a library function cannot take a move literal and check its
  captures as `spawn` does; it could take a `Box<dyn mut fn() + Send>`, and still needs the runtime's detach, which no
  library type can reach. So it is a member of the built-in `Thread`, beside `spawn` and `both`, with the same checks.
- **`spawnDetached` returning a handle**, or a daemon flag on `Thread<T>`: a handle that may not be joined is a trap;
  a thread that is never joined needs no handle.
- **Joining services at exit**: an event loop does not end on its own.

## 4. Consequences

- The interpreter runs threads one at a time, a thread when another waits for it (ADR 0016 §3.8). A service runs there
  when `main` waits on something, to its end; one that waits for `main` in turn is the interpreter's deadlock, as a
  spawned thread that does is. Programs whose service answers `main` back and forth run natively only.
- A service still running when the process ends holds its environment and what it captured, which the allocation
  report counts as live: `ceesafec test` reports such a test `leaked`, and the corpus's `--check-leaks` fails it. Even
  a service that returns frees its environment on its own thread, which races `main`'s exit. The corpus test ends with
  `Process.exit(0)`, which prints no report, as a program whose services run for its life ends, and is an ffi test,
  since the fault sweep over run-pass needs a report. **Open, for the owner:**
  whether the report should leave out what live services hold (the runtime would track service environments and what
  they reach, as ADR 0175's isolated tasks' leaks are told apart).

## 5. Implementation and tests (2026-11-19)

- `checkThreadService` (src/check/threads.cee), `bfThreadService`, `ceeTSS_` and `ceeTRS_` (src/backend/c_dyn.cee),
  `ceesafe_thread_service` (runtime/hosted/ceesafe_rt_thread.c), the interpreter's service threads (io.cee).
- `ffi/adr0182-service-threads.cee`: a service squares four requests and returns when its channel closes; a second
  waits on a channel no one closes and `main` returns past it. It was a run-pass program; it is in the ffi family,
  which the fault sweep does not run, because it can give no allocation report (§4). Interpreted, it printed the same;
  the guide's `Thread.service` example runs in the interpreter on every guide check.
  `check-fail/adr0182-service-threads.cee`: N0333, N0225 (a value body), N0334, N0332.
- A program whose service answers requests forever exited 0 five times out of five on macOS (arm64).
