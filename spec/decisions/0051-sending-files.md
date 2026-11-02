# ADR 0051 — Sending a file on a stream without copying it

- Status: **Accepted** by the owner, 2026-11-02 ("Yes, I accept all five ADRs with your recommendations"): every
  question of §5 as recommended. Asked for by the owner for CeeSafe.Web
  (`~/dev/CeeSafe.Web/docs/ceesafe-upstream-work.md`, item U7). Its static files (M9) are sent this way.
- Scope: lib/std's `AsyncTcpStream`, the hosted runtime on every reactor, the interpreter's model.

## 1. Context

A static file is sent today by reading it into a buffer (`AsyncFiles.read`, on a worker thread) and writing the buffer
to the socket. Every byte crosses into the process and back out. The kernels can send a file's pages to a socket
directly:
- Linux `sendfile`;
- macOS and the BSDs `sendfile`, with a different signature;
- Windows `TransmitFile`.

Three facts shape the design:

- **`File` is a compiler built-in, and its C form is its descriptor**, an `int32_t` (rt.h's `ceesafe_fd_drop(int32_t
  *)`). lib/std can pass the runtime a pointer to it, under the same contract. The borrow of the `File` keeps it open
  for as long as the operation is in flight: the frame that holds the borrow outlives the operation (ADR 0035).
- **A file read can block on the disk.** `sendfile` on a non-blocking socket waits for the socket without blocking,
  but it still reads pages the cache does not hold. nginx's default accepts that; its `aio threads` avoid it.
- **Windows' `TransmitFile` sends at most 2³¹ − 1 bytes a call,** and takes the file's offset in the `OVERLAPPED`.

## 2. Options

**io_uring.** `IORING_OP_SPLICE` sends a file without blocking on the disk, but through a pipe: a pipe per stream, and
two linked submissions per step. **Proposed:** on io_uring, the readiness path's `sendfile`, until a measurement shows
the disk wait matters. The splice path can come later without an API change.

**Where a step reads the disk.** On the executor's thread (proposed, as nginx's default), or always on a worker thread.
A worker per step adds a thread hand-off to every chunk, the common case being a cached file.

## 3. Proposed decision

### 3.1 The API (lib/std)

```ceesafe
// AsyncTcpStream
public async fn sendFile(borrow this, file: borrow File, offset: u64, length: u64) -> Result<u64, AsyncError>;
public async fn sendFileAll(borrow this, file: borrow File, offset: u64, length: u64) -> Result<void, AsyncError>;
```

- **`sendFile` is one step.** The task waits until the socket takes at least one byte, and the result is how many the
  kernel sent, from `offset`. Fewer than `length` is normal; 0 means the file ends at `offset`. A `length` of 0 is
  `Ok(0)` without a system call.
- **`sendFileAll`** loops until `length` bytes are sent. If the file ends first, the result is `Err(Io(InvalidInput))`,
  with the bytes before it sent.
- **Offsets and lengths are 64-bit.** The runtime splits a step to the platform's limit, so files over 4 GiB work.
- **The file's own position is not used or moved:** the offset is explicit. So one file may be sent on many streams at
  once.
- **Cancellation** follows ADR 0035: on IOCP an operation in flight is cancelled and waited for, and a step completed
  meanwhile reports its count.

### 3.2 The runtime, on every reactor

- **Linux** (epoll, and io_uring per §2): `sendfile(socket, file, &offset, count)`. On `EAGAIN` the step registers for
  writability.
- **macOS and the BSDs:** `sendfile(file, socket, offset, &len, NULL, 0)`. On `EAGAIN`, `len` holds what was sent:
  a partial count, or the wait.
- **Windows, IOCP:** an overlapped `TransmitFile` with the offset in the `OVERLAPPED`, at most 2³¹ − 1 bytes. The
  handle is the CRT descriptor's (`_get_osfhandle`).
- **Windows `WSAPoll`, CosmoOS userland, and any system call that refuses** (`EINVAL` or `ENOSYS` from `sendfile`, a
  file that is not a regular file): the runtime returns `Unsupported`. lib/std then falls back to a bounded copy: up to
  64 KiB read with `pread` semantics on a worker thread (`Async.blocking`), and sent with `write`. Same results, with a
  copy.
- **The record** is the plain operation record (`AsyncOp`, ADR 0035): no buffers are lent to the kernel.

### 3.3 The interpreter

It models a step with the host's file read at an offset and the host's socket send: at most 64 KiB, a legal partial
send. The host lib/std has both, so no chain step is needed.

## 4. Consequences

- CeeSafe.Web's static files go to the socket without passing through the process, where the system can.
- A step may block the executor's thread on a disk read (§2). A server for files that are rarely cached should know it.
  A later option can move steps to a worker.
- New diagnostics: none.

## 5. Questions for the owner

1. `sendFile` (one step, a partial count) and `sendFileAll`, with an explicit 64-bit offset and the file's position
   untouched? Recommended: yes.
2. `sendfile` on the executor's thread, as nginx's default, rather than a worker per step? Recommended: yes.
3. io_uring uses the readiness path's `sendfile` for now, not splice through a pipe? Recommended: yes.
4. Where the system cannot, a bounded 64 KiB copy in lib/std, with the same results? Recommended: yes.

## 6. Implementation plan (once accepted)

1. Spec: §40.4, c-backend.md §6, the guide's sockets section.
2. Runtime: `ceesafe_async_op_sendfile(op, socket, file, offset, length)` on each backend; `tools/gen_runtime.py`.
3. lib/std: `sendFile`, `sendFileAll` and the fallback; `tools/gen_library.py`.
4. The interpreter's model.
5. Tests:
   - corpus run-pass: files of 0 and 1 bytes, a file larger than the socket buffers to a slow reader (partial steps),
     an offset in the middle, a length past the end (`sendFileAll`'s `InvalidInput`), the same file on two streams at
     once. The received bytes are checked against the file's (Python writes the file and its digest);
   - a file larger than 4 GiB, sparse, so it takes no disk space: native only, in a tool beside the contract suite,
     since the corpus runs in the interpreter too;
   - a step cancelled in flight under ASan, and `tools/cancel_sweep.py`;
   - every reactor, Debian, and Windows 11 ARM64 (IOCP's `TransmitFile`; `WSAPoll`'s fallback).
6. ROADMAP §10.
