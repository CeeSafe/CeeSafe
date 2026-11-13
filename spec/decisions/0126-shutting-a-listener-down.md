# ADR 0126 — Shutting a listener down: `listener.shutdown()`

- Status: **Accepted** by the owner, 2026-11-13 ("Go ahead with the first three"), implemented on branch `impl/0125`;
  released in ceesafec 0.43.0. Raised by the book's authors (chapter 21): a thread waiting in `accept` could not be told to
  stop, so a server of threads had no graceful shutdown; they asked for "a way to close a listener early".
- Scope: the built-in `TcpListener` (checker `src/check/io.cee`, `src/hir/builtins.cee`, the C backend, the
  interpreter); the hosted runtime (`runtime/hosted/ceesafe_rt_io.c`, `rt.h`); `spec/type-system.md` §26.4 and
  §40.7; `guide/builtins.md`, `guide/threads.md`, `guide/async.md`; the corpus and `tools/signal_check.py`. No new
  diagnostic and no new variant of `IoError`.

## 1. Context

- **Tasks need nothing new.** `await listener.accept()` on an `AsyncTcpListener` is a wait like any other:
  cancelling its task makes it return `Err(AsyncError.Cancelled)` (§40.7), and `Async.race` is how one task stops
  another. The book's last listing does this. This ADR documents it and tests it with a signal (ADR 0125).
- **Threads had no way.** `TcpListener.accept` blocked in `accept(2)`. Another thread could only end the process.
- **Closing the socket under a waiting thread is not a way.** The descriptor's number is free the moment it is
  closed, and the waiting thread, or one about to wait, may then act on whatever file gets that number next. It is
  also not expressible: the listener's drop needs ownership, which no thread has while another borrows it.
- **What the systems do** when another thread wants a blocked `accept` to return:
  - Linux: `shutdown(fd, SHUT_RD)` on a listening socket wakes it (`EINVAL`).
  - macOS and the BSDs: `shutdown` on a listening socket is `ENOTCONN` and wakes nothing.
  - Windows: only `closesocket` wakes it, with the hazard above.
  So no system call does it everywhere. Waiting for two things does.

## 2. Decision

```ceesafe
listener.shutdown()    // TcpListener, `borrow this`: Result<void, IoError>
```

- **What it does.** Every `accept` on that listener, one a thread is waiting in now and every later one, returns
  `Err(IoError.ConnectionAborted)`. It cannot be undone. Calling it again changes nothing. It does not wait and has
  no effect (in the effect system's sense), so a task or a `drop` block may call it.
- **What it does not do.** It does not close the socket: that is still the listener's drop, when the last owner
  goes. Until then the port stays bound, and a client that connects waits in the system's queue and is reset at the
  drop (as with a cancelled async accept loop, where the book notes the same).
- **Ownership.** `accept` and `shutdown` both take `borrow this`, and `TcpListener` is `Send` and `Share` already:
  two threads share it through an `Arc<TcpListener>` (`a.get().accept()`, `b.get().shutdown()`), or by reference
  under `Thread.both`. No use after the close is possible: nothing is closed while a reference exists.
- **`ConnectionAborted` from `accept` now means exactly this.** The system's own `ECONNABORTED` (a client that
  connected and reset before it was accepted) was passed to the program before; the runtime now passes over such a
  connection and accepts the next. So `Err(IoError.ConnectionAborted)` ends an accepting loop, and no existing
  variant had to change meaning for a program that matches it.
- **Async listeners have no `shutdown`:** cancellation is theirs (§1).

### 2.1 The runtime: one mechanism

A blocking listener no longer waits in `accept(2)`. Its socket does not block, and `ceesafe_tcp_accept` waits for
two things: the socket to have a connection, and the listener's wake.

- **POSIX.** The wake is a pipe made by `ceesafe_tcp_listen`. `accept` is `poll` on the socket and the pipe's
  reading end, then `accept(2)`; `EAGAIN` (another thread took the connection) and `ECONNABORTED` go round again.
  `shutdown` writes one byte, which nothing ever reads: the pipe stays readable, so every later `poll` returns at
  once. The accepted socket is made blocking where the system let it inherit `O_NONBLOCK` (the BSDs).
- **Windows.** The wake is a manual-reset event; the socket signals a second event for `FD_ACCEPT`
  (`WSAEventSelect`, which also makes it nonblocking). `accept` resets that event, tries `accept`, and on
  `WSAEWOULDBLOCK` waits for both with `WaitForMultipleObjects`. `shutdown` sets the first event.
- **Where the wakes live.** A `TcpListener` is a descriptor, and stays one: its wakes are in a small table of the
  runtime, by descriptor, from `listen` to `ceesafe_fd_drop`, which removes the entry before the number can be
  reused. The table has a spin lock for its short walks; no system call is made under it.
- **Cost.** Two more descriptors for each blocking listener (one event pair on Windows), and one more system call
  for each `accept`. A server has a few listeners.
- **CosmoOS** has neither `poll` nor a descriptor that does not block: its listeners wait as before, and `shutdown`
  is `Err(IoError.Unsupported)`.

### 2.2 The interpreter

The interpreter marks the listener; `accept` on a marked listener is `ConnectionAborted`. It runs one thread at a
time (§38), and a thread that waits in the host's `accept` keeps the others from running, so none can shut the
listener down meanwhile: a known difference of the interpreter's threads (`docs/p8-plan.md`), not a new one. A
program in which the listener is shut down before the accepting thread comes to run behaves as when compiled.

Tried and withdrawn: letting `accept` first run the threads that have not started, so that one of them could call
`shutdown`. A thread started that way and then blocked on the accepting thread (a pool's worker waiting for its
first connection, as in the book's chapter 21) cannot give the baton back, and the interpreter reported a deadlock
in a program that ran before.

## 3. Alternatives considered

- **A handle type** (`listener.closer()` giving a `Send + Share` value): a second type that can only do what a shared
  reference to the listener can. `borrow this` and `Arc` already give exactly the sharing wanted.
- **A new `IoError.Closed`:** it would say it a little better, and break every exhaustive `match` on `IoError`
  (N0256), as ADR 0077 and ADR 0104 did. `ConnectionAborted` says it once the runtime keeps the kernel's own use of
  it away from programs. Left to the owner (see the report of the branch).
- **`shutdown(2)` on Linux and something else elsewhere:** two behaviours to keep alike, one of them (the socket
  stops listening at once) visible to clients on one system only.
- **A timeout on `accept`** and a flag the loop looks at: a server that wakes many times a second to do nothing, or
  one that stops late.
- **Connecting to the listener's own port to wake it:** fails with a full queue, a firewall, or a bound address the
  process cannot reach.
- **One wake pipe for the process:** every listener's waiting thread would wake for any listener's shutdown and the
  byte would need an owner. Two descriptors for each listener are simpler to be sure of.

## 4. Compatibility

- A new method on a built-in type; no program has a `shutdown` on `TcpListener` today (it was N0200).
- `accept` no longer returns `ConnectionAborted` for a connection the client reset early: it accepts the next one.
  A program that treated that error as fatal now carries on; one that looped on it sees no difference.
- A blocking listener holds two more descriptors. A program at its descriptor limit has `ProcessLimits`.

## 5. Tests

- Corpus `run-pass/tcp-listener-shutdown.cee` (compiled, interpreted, `--check-leaks`): a thread shuts the listener
  down that another accepts on; `accept` after it; twice; a connection made before the shutdown is still
  accepted before it, and the stream it gave still works after it; a second listener is not disturbed.
- `check-fail/tcp-listener-shutdown-stream.cee`: `shutdown` on a `TcpStream` is still N0200 (it has `shutdownRead`
  and `shutdownWrite`), and `shutdown` takes no argument.
- `tools/signal_check.py`: the server of threads, with a request in flight when the signal comes.

## 6. Implementation and evidence

- **Where:** `runtime/hosted/ceesafe_rt_io.c` (the table of wakes before `ceesafe_fd_drop`; `ceesafe_tcp_listen`,
  `ceesafe_tcp_accept` and `ceesafe_tcp_listener_shutdown`, POSIX and Windows) and `rt.h`; the builtin
  `TcpListenerShutdown` (`src/hir/builtins.cee`, `src/check/io.cee`, `src/backend/c_body.cee`), with no effect in
  `src/nir/effect_check.cee`; `src/interpreter/io.cee`.
- **macOS 27, arm64 (2026-11-13):** the corpus, compiled and interpreted (`run-pass/tcp-listener-shutdown.cee` with
  `--check-leaks`; every other socket test unchanged); `tools/fault_injection.py --filter tcp-listener-shutdown`:
  4 failure points, 0 fail; `tools/signal_check.py`: the server of threads answers its request in flight, answers no
  connection made after the signal, and exits 0.
- **Linux (Debian 12 in a container, aarch64, gcc 12, 2026-11-13):** the corpus's `tcp` tests compiled and
  interpreted, and `tools/signal_check.py`.
- **Not run:** Windows, whose side (`WSAEventSelect`, the two events) is compiled by nothing here; Linux on x86-64;
  CosmoOS (unchanged: its listeners wait in `accept`, and `shutdown` is `Unsupported`).
