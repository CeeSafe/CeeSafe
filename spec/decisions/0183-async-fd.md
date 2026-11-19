# ADR 0183 — `AsyncFd`: a task waits for its own descriptor

- Status: **Accepted** by the owner, 2026-11-19 (asked about CeeUsb's findings F3 and F5: "Yes, both as described"):
  a public, POSIX-only `AsyncFd.readable(fd)` / `writable(fd)` in lib/std over the executor's existing wait.
  Implemented the same day released in 0.79.0.
- Scope: type-system.md §40.4; lib/std `async.cee`; the runtime (c-backend.md §6). Finding F5.

## 1. Context

usbfs reports finished transfers by making its descriptor writable (`POLLOUT`). The executor already waits on
descriptors (lib/std's private `ceesafe_async_wait_readable` / `_writable`), but a package could not reach them: a
second declaration of the C name was N0201. So CeeUsb ran its own epoll thread and woke tasks through `AsyncSignal`,
an extra thread hop per completion.

## 2. Decision

- **`AsyncFd.readable(fd: i32)`** and **`writable(fd: i32)`**, `async`, `Result<void, AsyncError>`: the task parks
  until the executor's reactor (kqueue, epoll edge-triggered or one-shot, or `poll`) says the descriptor may be
  ready. `Err(Cancelled)` or `Err(TimedOut)` before or after the wait, as other waits (§40.7).
- **The contract** is the reactor's: call them only after an operation on the descriptor would have blocked (EAGAIN),
  and retry the operation when they return, which may be early. Edge-triggered epoll learns readiness from edges, so
  a wait without a failed attempt first may sleep through data that is already there.
- **`AsyncFd.closing(fd)`** before the program closes a descriptor with its own call: the runtime's close epoch for
  the number moves, so the reactor registers a later descriptor of that number again (epoll forgets a closed one by
  itself, and the runtime's closes already move the epoch).
- **POSIX only.** On Windows the executor waits for sockets alone and counts any other descriptor ready, so
  `readable` and `writable` return `Err(Io(Unsupported))` there (and on CosmoOS, which has no reactor), rather than
  spin. They are not compiled out: the program decides with `cfg("target.unix")` (ADR 0179).

## 3. Options not taken

- **A raw `Waker` for a descriptor**: more general, and needs the reactor's registration API to be public.
- **A thread per descriptor**: the hop this removes.

## 4. Implementation and tests (2026-11-19)

- `AsyncFd` in lib/std/async.cee; `ceesafe_async_fd_waits` and `ceesafe_async_fd_closing` in
  runtime/hosted/ceesafe_rt_async.c (rt.h).
- `ffi/adr0183-async-fd.cee` (an ffi test so that interpreted runs, which run no C, skip it): a task reads a
  non-blocking pipe, parks with `readable`, and wakes when another task writes 50 ms later; on Windows it checks
  `Unsupported`. Passed on macOS (kqueue), and on Ubuntu 24.04 (x86-64) with edge-triggered epoll,
  `CEESAFE_REACTOR_ONESHOT=1` and `CEESAFE_REACTOR_POLL=1`.
