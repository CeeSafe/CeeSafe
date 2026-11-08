# ADR 0078 — `sendFile` never raises SIGPIPE; an open file's time and kind, and `File.openRegular`

- Status: **Proposed**, 2026-11-08, for the owner. Raised by CeeSafe.Web: bug B19 and request U22
  (`CeeSafe.Web/docs/ceesafe-upstream-work.md`), for its M9 milestone. The owner asked for both to be fixed here.
- Scope: the hosted runtime (`ceesafe_rt_async.c`, `ceesafe_rt_io.c`, `rt.h`), lib/std (`host.cee`, `async.cee`), the
  checker's File members, the C backend, the interpreter, type-system.md §26.3 and §40's sending files,
  guide/builtins.md, the corpus.

## 1. Context

- **B19.** On Linux, a program sending a file over TCP with lib/std's `sendFile` or `sendFileAll` ends with exit status
  141 (SIGPIPE) when the peer resets the connection during the send. Every other send in the runtime passes
  `MSG_NOSIGNAL`. `sendfile(2)` has no flags argument, and nothing in the runtime blocks or ignores SIGPIPE.
  - macOS is not affected: its sockets get `SO_NOSIGPIPE`. Windows has no SIGPIPE.
  - For a server, one client that disconnects in the middle of a download ends the process, and every connection with
    it. CeeSafe.Web found it with h2load, which resets its connections when its time is up.
  - Two ways in, both seen on Linux 6.1:
    1. **A failed call.** Linux reports a reset once, as ECONNRESET, to whichever call on the socket comes first. A
       server with a read pending (any HTTP/1.1 server) takes it there, and its next `sendfile` fails with EPIPE.
    2. **A partial count.** `sendfile` moves a file in pieces. A piece after the reset raises SIGPIPE, while the call
       returns the bytes of the pieces before it.
- **U22.** A static-file server answers with validators (`ETag`, `Last-Modified`, 304, `If-Range`; RFC 9110 §8.8, §13),
  and must refuse what is not a regular file. ADR 0077's `File.modified(path)` and §26.8's `File.kind(path)` read the
  path, which can name another file by the time it is opened: a check-then-use race. Opening a FIFO for reading waits
  for a writer, so a request for one would hold its worker.

## 2. Decision: `sendFile` never raises SIGPIPE

1. **On Linux, each `sendfile` call blocks SIGPIPE on its thread** (`pthread_sigmask`).
   - SIGPIPE from a send is directed at the calling thread, so it stays pending there.
   - If one became pending during the call and none was before it, it is taken with a zero-timeout `sigtimedwait`.
     That holds whatever the call returned: an error, or a partial count (§1's second way).
   - The thread's mask is then restored. The process's disposition of SIGPIPE is never changed.
   - A thread that already blocks SIGPIPE keeps whatever the call leaves pending, as after any write of its own.
2. **The result is the call's own.** A partial count is returned as sent, and the next step's EPIPE is
   `IoError.BrokenPipe`. So `sendFileAll` fails with an error, as a `write` would.
3. **Both reactors use the same step.** epoll and io_uring share it (`cee_ready_sendfile`). macOS and Windows are
   unchanged.
4. **Rejected:**
   - **Ignoring SIGPIPE process-wide at startup** (nginx, Go). It changes every program, including one writing to a
     closed pipe on standard output, which today ends quietly.
   - **`splice` through a pipe** instead of `sendfile`. It takes no flag that suppresses the signal either.

## 3. Decision: an open file's time and kind, and `File.openRegular`

1. **`f.modified()`** returns `Result<UnixTime, IoError>`: the open file's last modification time, from `fstat`
   (`_fstat64` on Windows), with nanoseconds where the system keeps them.
2. **`f.kind()`** returns `Result<PathKind, IoError>`, from `fstat`:
   - `File` for a regular file;
   - `Directory` for a directory, where the system lets one be opened;
   - `Other` for a device, FIFO or socket.
3. **How they are built.** Both are lib/std's `FileMetadata.modified(f)` and `FileMetadata.kind(f)`, over the
   runtime's `ceesafe_host_file_modified` and `ceesafe_host_file_kind`, which read the File's descriptor. The checker
   routes the methods as it routes slice algorithms, with `f` borrowed. They take `borrow this`, so a file shared by
   reference can be asked. They are hosted only.
4. **`File.openRegular(path)`** opens `path` for reading if it is a regular file:
   - **POSIX:** `open` with `O_RDONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC`, so a FIFO's open never waits and a
     terminal never becomes the controlling one. Then `fstat` the descriptor. A regular file has `O_NONBLOCK`
     cleared and is returned. Anything else is closed and refused.
   - **Windows:** `_open`, then `_fstat64`. Windows refuses to open a directory (EACCES); a stat of the path, after
     the failed open, makes that `IsDirectory`. No file is read, so this races with nothing.
   - **Errors:** a directory is `IoError.IsDirectory`. Anything else that is not a regular file is `InvalidInput`,
     whichever device it is. A missing path is `NotFound`, as for `File.open`.
   - **Encoding:** it is mode 6 of `ceesafe_file_open`.
5. **`AsyncFiles.openRegular(path)`, `AsyncFiles.modified(file)` and `AsyncFiles.kind(file)`** run these on a worker
   thread, as `AsyncFiles.length` does, and take `borrow mut File` as it does.
6. **The interpreter:**
   - `f.modified()` answers `Unsupported`, as `File.modified(path)` does (the stage-0 compiler's library cannot
     stat).
   - `f.kind()` and `File.openRegular` use the path's kind. Its files are reopened by path for each operation
     (ADR 0075 §2.1), and nothing else runs between the check and the use.
   - A file whose path was removed since it was opened is `NotFound` there. Natively it still answers.
7. **Not changed:** `File.open` keeps its behaviour. A program that opens a FIFO to wait for a writer still can.

## 4. Consequences

- A Linux server that sends files survives clients that disconnect. CeeSafe.Web's benchmark server, built with this
  runtime, served a whole 1 GiB response after 80 clients dropped in the middle of one and h2load's resets, under epoll
  and io_uring.
- A static-file handler can take a file's validators from the file it sends. It can refuse FIFOs and devices without
  blocking a worker.
- Each Linux `sendfile` costs two `pthread_sigmask` calls and two `sigpending` calls, and a `sigtimedwait` after a
  reset. Each call moves up to the socket's free space, so this is small next to the copy.
- `IoError` gains no variant, so no `match` over it changes.

## 5. Tests

Run on 2026-11-08 (macOS arm64; Debian 12, Linux 6.1, x86-64, under epoll and io_uring; Windows 11 ARM64, under IOCP
and WSAPoll; each natively and in the interpreter):

- **`run-pass/async-send-file-reset`:** §1's two ways in, in rounds whose sends must fail or finish, never signal.
  - With ceesafec 0.21.0 on Linux it exits 141 under both reactors.
  - So it does with a runtime that took the signal only after a failed call (exit 141, 6 runs of 6, in the
    mid-send rounds).
  - With this runtime it passes everywhere.
  - It passes the allocation fault sweep (85 failure points).
- **`run-pass/file-open-regular`:** checks, in order:
  - a regular file opens, reads, is `File`, and its `modified()` equals `File.modified(path)` (the path-based oracle);
  - after its path is removed, it still answers `kind()` and `modified()` (natively);
  - a directory is `IsDirectory`;
  - `NUL` or `/dev/null` is `InvalidInput`;
  - a missing path is `NotFound`;
  - a FIFO made with `mkfifo` is `InvalidInput` (POSIX);
  - the `AsyncFiles` forms give the same answers.

  A mutant runtime without `O_NONBLOCK` fails it: the FIFO's open waits until the run times out.
- **On Windows,** `/dev/null` was a regular file on the test machine (`C:\dev\null`), and `openRegular` rightly opened
  it. The test tries `NUL`, a device in every directory, first.
