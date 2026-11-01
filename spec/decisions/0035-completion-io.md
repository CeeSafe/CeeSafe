# ADR 0035 — L1 A4b: completion-based I/O (IOCP, io_uring)

- Status: **Accepted** by the owner, 2026-11-01 (every question as proposed). Phase 1 of what is left of L1 A4 (ROADMAP,
  the owner's "in a multiphased way").
- Scope: how lib/std's async socket operations use completion-based kernel interfaces, IOCP on Windows and io_uring
  on Linux, beside the readiness reactors of L1 A4b (kqueue, epoll, `poll()`, `WSAPoll`); and which backend each host
  uses. The language does not change.

## 1. Context

Each socket operation in `lib/std/async.cee` is a readiness loop: try the call, and on `CEE_ASYNC_WOULD_BLOCK`
register a wait (`ceesafe_async_wait_readable`/`_writable`) and park. A cancelled task leaves at the top of the loop
(`AsyncLeaf.status()`). Nothing is in flight while the task waits, so leaving is always safe.

Measured 2026-11-01 (`benchmarks/async-reactor`, ROADMAP §10): epoll and kqueue stay flat as idle connections grow,
but Windows does not: `WSAPoll` sees every waiting socket, 28 µs per round trip at 10 idle connections and 3200 µs
at 1000.

With IOCP or io_uring the kernel is given the operation and its buffer, and completes it later. That is sound only
if the buffer outlives the kernel's use of it. ADR 0031 provides the basis: a frame never moves and is never freed
while its task waits. type-system.md §40.7 states the rule: a cancelled completion-based read first cancels the
kernel's operation and waits for it. What is missing is the protocol between lib/std and the runtime, and the choice
of backend.

## 2. Options for the protocol

| | A. An operation record in the frame (proposed) | B. Completion behind the readiness calls |
|---|---|---|
| Library | one loop per operation over `ceesafe_async_op_*`, the record (`AsyncOp`) a local of the async fn | unchanged |
| In flight | the record: OVERLAPPED or io_uring's user data, the result | hidden runtime buffers, copied |
| Buffers | the caller's, lent to the kernel | the runtime's own, then a copy |
| Cancellation | explicit: cancel in the kernel, wait, then return | the runtime would own buffers that outlive the task |

**A is proposed.** B gives up the reason to use completion I/O (no copy) and needs buffers the runtime allocates
behind the program's back, against "allocation failure is a value" (ADR 0032 §1).

## 3. Proposed decision

1. **An operation record.** lib/std's `AsyncOp` is opaque runtime storage in the awaiting frame, like `ChannelWait`
   (ADR 0032 §5). It holds what the backend keeps in flight (an `OVERLAPPED` and its `WSABUF`, or the io_uring
   submission's user data), the state and the result.
2. **One call per step.** `ceesafe_async_op_read(op, fd, buffer, length)`, `_send`, `_accept` and `_connect` return
   the result (a value ≥ 0 or a negated errno) once the operation is complete, or `CEE_ASYNC_PENDING` when the task
   must park and call again. On a readiness backend a step is today's attempt (try; if it would block, register the
   wait and say pending). On a completion backend the first step submits and later steps read the completion. The
   library's loop is the same on every host.
3. **Cancellation waits for the kernel.** When the task is cancelled with an operation in flight,
   `ceesafe_async_op_cancel(op)` asks the kernel to cancel it (`CancelIoEx`, `IORING_OP_ASYNC_CANCEL`) and returns
   whether it is still in flight. The task then parks without being cancellable until the completion arrives. Its
   frame, and so the buffer, outlives the kernel's use. The test is a buffer-lifetime program: cancel a read while
   the kernel holds the buffer, under ASan.
4. **No value is lost** (§40.7, as `recvAsync`): a read that completed before the cancellation reached the kernel
   returns its bytes (`Ok(n)`), and the next operation sees the cancellation. A send that completed reports what it
   sent. Only an operation that did nothing returns `Err(Cancelled)`.
5. **Backends.**
   - **Windows: IOCP**, the executor's port, with each socket associated once. `WSARecv`, `WSASend`, `AcceptEx` and
     `ConnectEx` replace `WSAPoll` for sockets. Consoles and files are unchanged.
   - **Linux: io_uring**, with its rings mapped through the raw system calls (no liburing). It adds `IORING_OP_RECV`,
     `_SEND`, `_ACCEPT`, `_CONNECT` and `_ASYNC_CANCEL`, and the wait pipe and deadlines through the ring.
   - epoll, kqueue, `poll()` and `WSAPoll` remain, as readiness backends of the same protocol.
6. **The interpreter** implements `ceesafe_async_op_*` as readiness steps over its real sockets, as today.
7. Every executor keeps its structures of L1 A4b. An operation in flight is another reason for a task to wait.

## 4. Consequences

- `runtime/hosted/ceesafe_rt_async.c` and `ceesafe_rt_io.c` gain the op calls and the two backends; `rt.h` declares
  `ceesafe_async_op` (size asserted against lib/std's `AsyncOp`) and `CEE_ASYNC_PENDING`. c-backend.md §6 describes
  them. lib/std's accept, connect, read and write become op loops.
- `benchmarks/async-reactor/run.py` gains a column per backend. Tests: run-pass async-cancel-read (the
  buffer-lifetime program), the existing async socket programs on every backend, and the cancel sweep, which cancels
  at every suspension, so also with operations in flight.

## 5. Questions for the owner

1. The protocol, A? **Yes.**
2. **Linux's default:** io_uring where the kernel allows it, or epoll by default with io_uring opt-in
   (`CEESAFE_REACTOR=uring`) until the benchmark shows it faster? (Proposed: opt-in first. epoll is already flat at
   18 µs per round trip, and io_uring is often disabled in containers.) **epoll by default, io_uring opt-in.**
3. **Windows' default:** IOCP, with `WSAPoll` kept behind `CEESAFE_REACTOR_POLL=1`? (Proposed: yes; `WSAPoll` is
   linear.) **Yes.**

## 6. Implementation (2026-11-01)

- **The protocol** (§3.1–3.4): `AsyncOp` in `lib/std/async.cee` (the runtime's 192-byte record, then the reason the
  task is leaving). `check()` before each step leaves at once if nothing is in flight, and otherwise asks for the
  cancellation and steps on. `result()` keeps a completed value and makes an error the cancellation's. accept,
  connect, read and write are op loops. The runtime's record (`cee_op`) is one layout for every backend, so a
  cancellation knows what it holds.
- **IOCP**, Windows' default (§3.5): as c-backend.md §6 describes. Found while measuring:
  - a task waiting on an operation was first a wake waiter, and every wait scanned them all, so the cost stayed
    linear; such tasks now have their own state, `on_op`;
  - `ConnectEx`'s socket was tied to the port before it had an id, so the table did not know, and its next
    association failed, which sent it to readiness. `ceesafe_socket_adopt` now records the port.
- **io_uring**, opt-in (§3.5): the variable is `CEESAFE_REACTOR_URING=1`, numeric like `CEESAFE_REACTOR_POLL`, not
  the `CEESAFE_REACTOR=uring` written above. Found while measuring: at the end of the benchmark, cancelling 2000
  readers at once overflowed the 512-entry completion queue. The kernel then refused the cancellations' submissions
  (EBUSY) and kept completions back, so readers waited for ever. Now the queue has 16384 entries, a refused
  submission is retried after a drain, and an overflow is flushed.
- The async runtime defines `_GNU_SOURCE` on Linux, for `syscall()`; it still gives `clock_gettime`.
- **Tests:** run-pass/async-cancel-read (the buffer-lifetime program: a child's read cancelled in flight, its frame
  freed, the peer writing afterwards; the next read gets every byte); a contract step, "reactor uring", running the
  async programs on io_uring under ASan+UBSan on Linux; `benchmarks/async-reactor/run.py`, a column per backend.
