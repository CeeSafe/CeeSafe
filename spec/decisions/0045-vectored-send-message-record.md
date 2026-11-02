# ADR 0045 — Vectored send, and the message operation record

- Status: **Accepted** by the owner, 2026-11-02 ("Yes, I accept each and every ADR's questions"): every question of
  §5 as recommended. Asked for by the owner for CeeSafe.Web
  (`~/dev/CeeSafe.Web/docs/ceesafe-upstream-work.md`, item U3, and U6's question about the operation record's size).
  The second of that document's ADRs, after ADR 0044. It decides the record that vectored sends and, later, datagram
  messages (U6) use. The UDP API itself is a later ADR.
- Scope: `lib/std`'s `AsyncTcpStream` and `AsyncSockets`, a new `lib/std` type for a list of byte parts, the hosted
  runtime's operation records and steps on every reactor, and the interpreter's model.

## 1. Context

An HTTP response is a head and a body in two buffers. Today `AsyncTcpStream.write` takes one slice, so a server
either sends twice (two system calls, and a small first segment unless `TCP_NODELAY` is set) or copies both into one
buffer. The kernels have the answer in one call: `writev`/`sendmsg` (POSIX), `WSASend` with an array of `WSABUF`
(Windows), `IORING_OP_SENDMSG` (io_uring).

Three facts constrain the design:

- **The operation record.** A socket operation is steps over a record in the awaiting task's frame (ADR 0035):
  `ceesafe_async_op`, 24 words (`rt.h`), lib/std's `AsyncOp`. It never moves, so what the kernel uses outlives that
  use. On a completion backend the kernel reads a vectored send's array of parts after the first step returns, so the
  array must be in the record too, or somewhere else that never moves. U6's datagram messages need more: a
  `msghdr`/`WSAMSG`, the peer's address and a control buffer for ancillary data (packet info, ECN, GSO).
- **Every waiting operation has a record in its frame.** A server's idle keep-alive connection is a task parked in
  `read`, so the record's size is paid once per open connection. CeeSafe.Web's worker frame is 920 bytes today, and
  the record is 192 of them.
- **CeeSafe has no slices of slices** (N0246: a slice is never an array or slice element, type-system.md §25.4).
  `writeVectored(parts: Slice<Slice<u8>>)`, as the request wrote it, cannot be declared. A struct with region
  parameters may hold slices in fields, and CeeSafe.Web's `IoParts<region r>` already does that with four named slots
  (`packages/web_core/src/transport.cee`).

## 2. Options

**Where the parts live while a send is in flight.**

| | A. A second, larger record (proposed) | B. One larger record | C. The caller's frame |
|---|---|---|---|
| What | `ceesafe_async_msg_op` (64 words) for vectored and datagram operations; `ceesafe_async_op` stays 24 words | `ceesafe_async_op` grows to hold the parts, the message header and the control buffer | the record points at an array in the calling task's frame |
| Cost | two record types in the runtime; one shared header, so cancellation is unchanged | about 320 more bytes in every waiting read's frame, idle connections included | a pointer into another frame; correct only while that frame outlives the step, which ADR 0035 §3.3 proves only for the record itself |

**A is proposed.** Only a task in a vectored or datagram operation pays for the larger record, and the larger record is
still in a frame that never moves. C would be correct with today's frames, but it adds a second thing that must
outlive the kernel's use, which nothing checks.

**How a list of parts is written.**

| | D. A lib/std type with fixed slots (proposed) | E. Allow arrays of slices |
|---|---|---|
| What | `IoParts<region r>`: up to eight parts, built by value: `IoParts.of(head).then(body)` | lift N0246 for array elements, so `[Slice<r, u8>; N]` and a borrow of it as a list exist |
| Cost | lib/std only; eight private slot fields | a language change: element loans in the borrow checker, the flows of §25.6, both backends, the interpreter |

**D is proposed** for 0.7.0. It has the same shape as CeeSafe.Web's `IoParts`, so CeeSafe.Web maps onto it or adopts
it. E is a better language. It is not needed for this, and it is worth its own ADR if more code wants lists of
borrowed slices.

## 3. Proposed decision

### 3.1 `IoParts<region r>` (lib/std, `net.cee`)

```ceesafe
public struct IoParts<region r> {
    // eight private Slice<r, u8> slots and a count
    public fn of(part: Slice<r, u8>) -> IoParts<r>;
    public fn then(borrow this, part: Slice<r, u8>) -> IoParts<r>?;   // null when it holds eight already
    public fn count(borrow this) -> u32;
    public fn part(borrow this, i: u32) -> Slice<r, u8>;              // empty past the last part
    public fn totalLength(borrow this) -> usize;
    public fn after(borrow this, n: usize) -> IoParts<r>;             // the bytes after the first n, as parts
}
```

- A value, built by copying: a scoped type cannot be a `var` local (N0246), so `then` returns a new value instead of
  appending.
- Empty parts are allowed and cost nothing: they are skipped when the record is filled.
- `after(n)` drops whole parts and cuts into the part where byte `n` falls. It is what a write loop needs after a
  partial send. Past `totalLength()` it is empty.
- The capacity is **eight**. It is at most `IOV_MAX`'s POSIX minimum (`_XOPEN_IOV_MAX`, 16) on every host, and an HTTP
  response needs two (head, body) or three per chunk (size line, data, CRLF).

### 3.2 The streams

```ceesafe
// AsyncTcpStream
public async fn writeVectored(borrow this, parts: borrow IoParts) -> Result<usize, AsyncError>;  // one step
public async fn writeAllVectored(borrow this, parts: borrow IoParts) -> Result<void, AsyncError>; // every byte
// AsyncSockets (raw descriptors)
public async fn sendVectored(fd: i32, parts: borrow IoParts) -> Result<usize, AsyncError>;
```

- `writeVectored` waits until the socket takes at least one byte, then returns how many it took, which may stop in
  the middle of any part. A partial count is normal, not an error. Empty `parts` (total 0) returns `Ok(0)` without a
  system call.
- `writeAllVectored` loops `writeVectored` over `parts.after(sent)` until every byte is sent, as `write` does for one
  slice.
- Errors and cancellation are `write`'s:
  - `BrokenPipe` and `ConnectionReset` are `Io(…)`. No signal is raised: the runtime already sends with
    `MSG_NOSIGNAL` and `SO_NOSIGPIPE`.
  - A cancellation with a send in flight on a completion backend follows ADR 0035 §3.4. The task asks the kernel to
    cancel and waits for the completion. A send the kernel completed anyway reports its count to `writeVectored`.
    `writeAllVectored` returns `Err(Cancelled)` at its next step, as `write` does.
- The blocking `TcpStream` gets no vectored write: it is a compiler built-in (type-system.md §26.5), and CeeSafe.Web
  does not need one.

### 3.3 The message record

- **Size.** `rt.h` adds `typedef struct ceesafe_async_msg_op { uint64_t words[64]; } ceesafe_async_msg_op;`, with a
  `_Static_assert` that each backend's record fits. lib/std adds `AsyncMsgOp` (`words: [u64; 64]`, plus `leaving`, as
  `AsyncOp` has), with the same `check` and `result`.
- **Layout.** The runtime's record starts with today's `cee_op`, so `ceesafe_async_op_cancel` and the completion paths
  take either record. After that header comes the message area:
  - the parts as the kernel reads them: `struct iovec[8]` (POSIX, io_uring) or `WSABUF[8]` (Windows);
  - a `struct msghdr` (POSIX, io_uring) or `WSAMSG` (Windows);
  - a peer address (`struct sockaddr_in6`, which also holds IPv4);
  - a control buffer of 128 bytes for U6's ancillary data: packet info, traffic class and segment size each fit with
    room to spare.

  U3 uses the parts and, on io_uring, the `msghdr`. The rest is reserved for U6, so the UDP ADR adds operations and
  leaves the layout alone.
- **Filling it.** lib/std cannot build an array of C pointers. It hands the parts to the record one at a time, before
  the first step: `ceesafe_async_msg_part(op, index, bytes, length)`. The runtime reads them only during that call.
  The step is `int64_t ceesafe_async_op_sendv(void *op, int32_t fd, uint32_t count)`, with the usual result: a count
  ≥ 0, a negated errno, or `CEE_ASYNC_PENDING`.

### 3.4 The runtime, on every reactor

- **Readiness (epoll, kqueue, `WSAPoll`):**
  - Linux: `sendmsg(fd, &msg, MSG_NOSIGNAL)`.
  - macOS and the BSDs: `writev`. The socket already has `SO_NOSIGPIPE`.
  - `WSAPoll`: a non-overlapped `WSASend` with the `WSABUF` array.
  - On `EAGAIN` the step registers for writability, as `ceesafe_async_op_send` does.
- **IOCP:** an overlapped `WSASend` with the record's `WSABUF` array and `OVERLAPPED`, completed like `send`.
- **io_uring:** `IORING_OP_SENDMSG` with `MSG_NOSIGNAL` and the record's `msghdr`.
- **CosmoOS userland** (`CEE_COSMOOS`): one `send` of the first non-empty part, a legal partial write. The runtime's
  CosmoOS socket calls take one buffer today; a vectored call there can come later without an API change.
- **More parts than the host's `IOV_MAX`** cannot happen: the record holds eight.

### 3.5 The interpreter

The interpreter calls its host's lib/std (ADR 0044 §3.7), and stage 0 (0.3.2) has no vectored send. A vectored step
in the interpreter therefore sends the first non-empty part with the host's `send` and returns its count. That is a
legal partial write, so programs see the same bytes in the same order, and the bootstrap needs no chain step.

## 4. Consequences

- CeeSafe.Web sends a response's head and body in one system call, without a copy.
- A task in a vectored send has a 512-byte record in its frame, instead of 192 bytes. Every other operation is
  unchanged.
- U6 (UDP) gets its record now; its ADR designs only the API.
- New diagnostics: none.

## 5. Questions for the owner

1. A second record, `ceesafe_async_msg_op` (64 words), for vectored and datagram operations, leaving the plain record
   at 24 words (option A)? Recommended: yes.
2. Write the list of parts as lib/std's `IoParts<region r>`, eight fixed slots built by value (option D), and leave
   arrays of slices (option E) to a later language ADR? Recommended: yes.
3. A capacity of eight parts? Recommended: yes. (Sixteen is the most every host allows in one call. Eight keeps
   `IoParts` at 136 bytes.)
4. No vectored write on the blocking built-in `TcpStream`? Recommended: yes, none.

## 6. Implementation plan (once accepted)

1. Spec: §40.4 (the stream table and `IoParts`), `c-backend.md` §6 (the message record and its calls), the API pages.
2. Runtime:
   - `rt.h`: `ceesafe_async_msg_op`, `ceesafe_async_msg_part` and `ceesafe_async_op_sendv`;
   - `ceesafe_rt_async.c`: the record on each backend, its static asserts, the steps;
   - then `tools/gen_runtime.py`.
3. lib/std: `IoParts` (`net.cee`); `AsyncMsgOp`, `writeVectored`, `writeAllVectored` and `AsyncSockets.sendVectored`
   (`async.cee`); then `tools/gen_library.py`.
4. The interpreter's model of `ceesafe_async_msg_part` and `ceesafe_async_op_sendv` (§3.5).
5. Tests. Expected bytes come from a Python oracle that generates the parts and the concatenation each side must see:
   - corpus run-pass: parts of 0, 1 and more than the socket's send buffer; eight parts; `after` at every boundary;
   - a slow reader (a small `SO_RCVBUF` and a reader that sleeps), forcing partial sends across part boundaries;
   - a peer that resets mid-send gives `ConnectionReset` or `BrokenPipe`;
   - cancellation in flight under ASan, where the frame is freed only after the completion; `tools/cancel_sweep.py`
     over the new tests;
   - every reactor: the contract suite, its "reactor uring" step, and Windows (IOCP and `WSAPoll`) on the Windows 11
     ARM64 test host;
   - the allocation-failure sweep with `--check-leaks`.
6. The guide (`guide/async.md`, the sockets section), ROADMAP §10.

## 7. Implementation notes (2026-11-02)

- **Record sizes** (each checked by a `_Static_assert` in `ceesafe_rt_async.c`, and on every host the tests ran on):
  - the header, then the count, the eight parts and the message header;
  - a 32-byte peer address;
  - a control buffer of 128 bytes, or 96 on Windows, whose header is larger (an `OVERLAPPED` and three sockets). 96
    bytes still holds Windows' packet info, ECN and segment-size messages.
- **`IoParts`' receivers.** `then`, `part` and `after` take `this` by value. With `borrow this`, a result of type
  `IoParts<r>` was taken to borrow the receiver's own local (N0244), since regions are erased (ADR 0006), so a helper
  could not return a list it had built. `IoParts` holds only slices, so the copy is a few words.
- **The interpreter.** It keeps each part's address and length in the record's words 8 to 23. A step sends at most
  64 KiB of the first non-empty part, which is a legal partial send.
- **Windows' interpreter errors.** The interpreter's errno mapping (ADR 0044) had Linux's and macOS's numbers only.
  On Windows the runtime gives MSVC's `errno.h` values, read with `cl` on the test host: ECONNRESET 108, ENOTCONN 126,
  ETIMEDOUT 138, EOPNOTSUPP 130, and the rest. So a reset read as `Other`, and both reset tests failed there in the
  interpreter until the mapping gained a third column.
- **T1, done with this ADR.** `tools/cancel_sweep.py` and `tools/fault_sweep.py` take `--package DIR`
  (spec/testing.md §1).
