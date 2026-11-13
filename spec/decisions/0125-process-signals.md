# ADR 0125 — Process signals: `ProcessSignals`

- Status: **Accepted** by the owner, 2026-11-13 ("Go ahead with the first three"), implemented on branch `impl/0125`;
  not yet released. Raised by the book's authors (chapter 21, "a final project: a web server"): a program could not
  notice Ctrl+C or `SIGTERM`, so the book's server stops when Enter is pressed on standard input.
- Scope: lib/std (`lib/std/host.cee`: `ProcessSignal`, `ProcessSignals`; `lib/std/channel.cee`: one accessor for the
  library's own use); the hosted runtime (`runtime/hosted/ceesafe_rt_async.c`, `rt.h`); the interpreter
  (`src/interpreter/signals.cee`, `io.cee`); `spec/type-system.md` §26.15; `guide/builtins.md`, `guide/async.md`,
  `guide/threads.md`; `tools/signal_check.py` in the contract suite; the corpus. No change to the checker, no new
  diagnostic, and no new variant of `IoError` or `AsyncError`.

## 1. Context

- **What a server needs.** "Stop accepting, answer what is in flight, exit 0" starts with a request to end the
  process: `SIGINT` (Ctrl+C in a terminal) and `SIGTERM` (`kill`, systemd, a container's stop) on POSIX; Ctrl+C,
  Ctrl+Break and the console's close on Windows. By default each ends the process at once, with nothing dropped.
- **What CeeSafe had.** Nothing: no signal reached a program. The book's server reads a line on a thread instead.
- **What must not happen.** A signal handler runs between any two instructions of any thread. Code of the program
  there could see a half-made value, take a lock its own thread holds, or allocate inside the allocator. So nothing of
  the program may run in a handler, and the runtime's own handler may only do what POSIX calls async-signal-safe.
- **What the library has for waiting.** Channels (ADR 0032): a thread blocks in `recv`, a task parks in `recvAsync`
  and is cancellable there, `tryRecv` never waits, any thread may send, and the interpreter already models them.

## 2. Decision

```ceesafe
public enum ProcessSignal { Interrupt, Terminate }

@noncopy
public struct ProcessSignals {
    public fn subscribe() -> Result<ProcessSignals, IoError>;
    public fn poll(borrow this) -> ProcessSignal?;                                  // never waits
    public fn wait(borrow this) -> ProcessSignal;                                   // blocks the thread
    public async fn waitAsync(borrow this) -> Result<ProcessSignal, AsyncError>;    // parks the task
}   // its drop ends the subscription
```

- **Which requests.** `Interrupt`: `SIGINT`; on Windows `CTRL_C_EVENT` and `CTRL_BREAK_EVENT`. `Terminate`: `SIGTERM`;
  on Windows `CTRL_CLOSE_EVENT`, `CTRL_LOGOFF_EVENT` and `CTRL_SHUTDOWN_EVENT`. Nothing else is touched: `SIGHUP`,
  `SIGQUIT`, `SIGPIPE` (which the runtime already avoids per socket) and the rest do what they did.
- **Before a program subscribes, nothing changes.** The runtime installs no handler at start. A program that never
  calls `subscribe` is ended by Ctrl+C exactly as before this ADR.
- **One subscription for the process.** `subscribe` installs the handlers; a second call while the first value lives
  is `Err(IoError.AlreadyExists)`. `Err(IoError.Unsupported)` where there are no such requests (CosmoOS, the
  interpreter's sandbox). The value is `Send` and `Share` (its one field is a channel): threads and tasks use it by
  reference, or through an `Arc`.
- **While it lives,** a request does not end the process. It is queued: `poll` takes the oldest or returns `null` at
  once; `wait` blocks its thread (N0295 in async code, as any blocking wait); `waitAsync` parks its task and returns
  `Err(AsyncError.Cancelled)` if the task is cancelled first, losing nothing. So a server's shutdown is
  `Async.race(server, async () => await signals.waitAsync())`. Up to eight requests are kept; more that nobody takes
  are merged into those (a signal is a request, not a count).
- **Its drop restores what was there before:** the next request ends the process again. That is the answer to the
  second Ctrl+C: a program that wants "once to stop in good order, twice to stop now" drops the subscription when it
  begins to stop. While it keeps it, further requests only queue.
- **Hosted only.** lib/std is not there in freestanding code (N0200 for the names, as for `ProcessLimits`).

### 2.1 The runtime

`ceesafe_signal_subscribe(channel)` and `ceesafe_signal_unsubscribe()` (`rt.h`). The subscription's queue is a
`Channel<u32>` the library makes; the runtime takes a count of it and sends 1 or 2 into it, without waiting: a full
channel drops the value.

- **POSIX.** `sigaction` for `SIGINT` and `SIGTERM` (`SA_RESTART`), the old actions kept. The handler does two
  things, both async-signal-safe: an atomic or of the signal's bit into a word, and `write` of one byte to a pipe that
  does not block. A thread of the runtime, started by `subscribe`, waits in `poll` on the pipe, empties it, takes the
  bits and sends. `unsubscribe` restores the old actions, tells the thread to end, joins it and gives the channel's
  count back. The pipe is made by the first subscription and never closed: a handler still running in another thread
  when a subscription ends writes to a descriptor that is still the pipe. The next subscription empties it first.
- **Windows.** `SetConsoleCtrlHandler`. The console calls the handler on a thread of its own, so it sends at once.
  For the close, logoff and shutdown events the system ends the process when the handler returns: it does not return
  (`Sleep(INFINITE)`), so the program has until it returns from `main`, or the system's limit (5 s for a close). With
  no subscription the handler returns `FALSE`: the default.
- **Why a thread, and not the reactor.** A thread that sends into a channel serves all three ways of waiting on every
  reactor (kqueue, epoll, io_uring, `poll`, IOCP) with code that is already there and tested, and Windows needs no
  second design. The thread exists only while a subscription does.

### 2.2 The interpreter

`run --interp` and `test --interp` implement it for the interpreter's own process. The compiler that builds
`ceesafec` may be older than this ADR, so the interpreter does not call the runtime's functions: it asks the C
library (`signal()`), with a `@callback` handler that sets a bit in a static `AtomicU32` and sets itself again. The
bits are looked at where the program asks or waits: `tryRecv`/`recv`/`recvAsync` on the subscription's channel, the
task loop's top, and its waits (a poll of sockets is cut to 20 ms while a task waits for a signal; with nothing else
to wait for, 10 ms pauses). On Windows the C library gives the interpreter Ctrl+C and Ctrl+Break only. The sandbox
(the MCP server's `run`) answers `Unsupported`.

## 3. Alternatives considered

- **A user's function as the handler** (C's `signal`): unsound, §1. No language with a safe subset offers it.
- **Handlers installed by the runtime at start, for every program:** a program that does not care would have to
  learn to exit. Refused for the same reason as ADR 0104's question 3: a process-wide policy nobody asked for.
- **The reactor waits for the signal** (kqueue `EVFILT_SIGNAL`, `signalfd`, a self-pipe in the wait set): three
  mechanisms plus IOCP's, each to be made to work for a blocked thread too. More code in the least testable place.
- **A global function, `Process.signalled()`:** no owner, so no moment at which the default comes back.
- **More signals** (`SIGHUP` to reload, `SIGUSR1`): nothing has asked; `ProcessSignal` can grow, at the cost of
  users' exhaustive matches, which is why it starts with the two that every platform has.
- **`Process.raise(signal)` for tests:** the library has no place for test-only process control, and a signal a
  process sends itself is not the path a terminal or `kill` takes. `tools/signal_check.py` sends real ones.

## 4. Compatibility

New names in lib/std (`ProcessSignal`, `ProcessSignals`): a program with its own type of either name keeps its own
(a program's names shadow the library's). No existing program changes: without a subscription no handler exists.

## 5. Tests

- `tools/signal_check.py` (contract suite, POSIX), built and in the interpreter, `SIGINT` and `SIGTERM` each:
  a server of tasks with a request in flight (also with `CEESAFE_REACTOR_POLL=1`, and io_uring on Linux), a server of
  threads (ADR 0126), one signal taken and then the default after the drop, and no subscription at all.
- Corpus `run-pass/process-signals.cee` (compiled, interpreted, `--check-leaks`): no request yet, a second
  subscription refused, a subscription after a drop, a cancelled `waitAsync`.
- `check-fail/process-signals-wait-in-async.cee`: `wait` in an async fn is N0295.

## 6. Not in this ADR

- Sending signals, other signals, a child process's signals, signal masks.
- Windows services' control requests.
