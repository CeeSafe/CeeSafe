# ADR 0169 — Child processes that run beside their parent

- Status: **Accepted** by the owner, 2026-11-18 ("I accept all the ADRs as written"): every question of the
  "Questions for the owner" section as recommended. Written at the owner's request ("go ahead and write those ADRs")
  from the findings of the HTTP server written in CeeSafe (`~/dev/CeeSafeApps/httpserver/CEESAFE_FINDINGS.md`,
  S4). Not implemented yet: nothing of it is in the compiler.
- Scope: lib/std (`ChildProcess`, `ChildOptions`, `ChildExit`; `Process.spawn`, `Process.id`), the hosted runtime
  (`runtime/hosted/ceesafe_rt_hosted.c`: POSIX, Windows, CosmoOS), the interpreter's host services; type-system.md
  §26 (a new §26.16), `spec/api/std.md`, the guide's processes page. No change to the language or to the backends.

## 1. Context

- **What lib/std has.** `Process.run(program, arguments)` and `Process.capture(program, arguments, input)` start a
  program and wait for it (§26.7): `posix_spawn`, `spawnvp` on CosmoOS, `CreateProcess` on Windows. There is no
  value for a program that is running, no process id, and nothing to stop one.
- **What a supervisor needs.** httpserver runs each event loop in a worker process, so that a trap (panic = abort,
  §14.5) costs one worker's connections and not the server. It has to start workers and go on, learn which one
  ended and how, stop one whose loop no longer turns, and stop all of them when it is told to stop itself. None of
  that can be written with `run` and `capture`.
- **What it did instead.** It declares `fork`, `waitpid`, `kill`, `usleep`, `signal` and `setrlimit` through
  `extern "C"`, with their constants from the system's headers. That works on Linux and macOS, and:
  - it is not CeeSafe's memory safety that holds it up: `fork` copies a process with a runtime in it, and what in the
    runtime survives that is not written down (0.65.1 fixed one such thing, `ProcessSignals`);
  - `sigemptyset` and its kin are macros on macOS, and an imported C struct cannot be a local (N0294), so the
    declarations are not the same on the two systems;
  - there is no Windows form of it at all.
- **`fork` is not what is asked for.** Workers that are copies of the supervisor is one way to get workers. A
  program started again with an argument that says "you are worker 3" is another, it is what Windows and CosmoOS
  can do, and it shares nothing by accident. This ADR is about the second; it does not add `fork`.

## 2. Decision

### 2.1 The API (lib/std)

```ceesafe
public struct ChildOptions {
    /// What the child's standard input, output and error are: the parent's, or nothing (the null device).
    public input: ChildStream;
    public output: ChildStream;
    public error: ChildStream;
}

public enum ChildStream { Inherit, Null }

public enum ChildExit {
    /// It returned from `main` or called `Process.exit`: its status.
    Status(i32),
    /// A signal ended it (POSIX): its number. On Windows a child ended by `kill` is `Status(1)`.
    Signal(i32),
}

@noncopy
public struct ChildProcess {
    /// The system's id of the process.
    public fn id(borrow this) -> u64;
    /// Whether it has ended, without waiting: its exit if so.
    public fn poll(borrow mut this) -> Result<ChildExit?, IoError>;
    /// Waits for it to end, blocking the thread (N0295 in async code).
    public fn wait(borrow mut this) -> Result<ChildExit, IoError>;
    /// Asks it to end: SIGTERM, which a `ProcessSignals` subscription in the child receives as `Terminate`
    /// (§26.15); on Windows, a console control event to its process group.
    public fn terminate(borrow this) -> Result<void, IoError>;
    /// Ends it at once: SIGKILL; `TerminateProcess`.
    public fn kill(borrow this) -> Result<void, IoError>;
}

// in `Process`:
public fn spawn(program: StringView, arguments: borrow Vector<StringView>, options: ChildOptions)
    -> Result<ChildProcess, IoError>;
public fn id() -> u64;                         // this process's
public fn executable() -> Result<String, IoError>;   // the path of the running program, to start it again
```

- `Process.spawn` finds `program` on `PATH` as `Process.run` does, starts it, and returns. The child has the
  parent's environment and working directory.
- **A `ChildProcess` is the parent's hold on the child.** It is a move type that needs drop. Dropping it without
  `wait` or a `poll` that returned an exit does not end the child and does not wait for it: the runtime keeps what
  the system needs to forget the child when it ends (POSIX: it is reaped by a later `poll` of any child or at exit;
  Windows: the handle is closed). A program that wants its children gone ends them.
- **`poll` and `wait` report an exit once**, and afterwards return it again: the value is kept in the
  `ChildProcess`.
- **Several children** are watched by polling each. `Process.waitAny(children: SliceMut<ChildProcess>)`, blocking
  until one of them ends, is left to a later ADR (question 3): it needs `waitpid(-1)` on POSIX to be reconciled with
  `Process.run`'s own children, and `WaitForMultipleObjects`' limit of 64 on Windows.
- **Async code** polls on a timer, as it would any state that has no readiness to wait on. A child's end as an
  awaitable (a pidfd on Linux, `kqueue`'s `EVFILT_PROC`, a wait on the handle's thread pool) is also left
  (question 3).

### 2.2 What the child shares with its parent

Nothing but what the system gives every new program: the environment, the working directory, the three standard
streams as `ChildOptions` says, and no other open file: the implementation checks that every descriptor the
runtime opens is close-on-exec (the signal pipe is; the others are to be verified, and fixed where not). Memory to
share is asked for by name and is not part of this ADR.

### 2.3 Systems

| | start | `poll` / `wait` | `terminate` | `kill` |
|---|---|---|---|---|
| POSIX | `posix_spawnp` | `waitpid(pid, WNOHANG)` / `waitpid(pid, 0)` | `kill(pid, SIGTERM)` | `kill(pid, SIGKILL)` |
| Windows | `CreateProcess`, a new process group | `WaitForSingleObject(h, 0)` / `INFINITE`, `GetExitCodeProcess` | `GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, group)` | `TerminateProcess(h, 1)` |
| CosmoOS | `spawnvp`, if it returns without waiting; else `Unsupported` | as its libc allows; else `Unsupported` | `Unsupported` | `Unsupported` |
| the interpreter | as the host, unless sandboxed: `Unsupported` | | | |

- A process id that the system has reused is never signalled: `terminate` and `kill` after the exit was reported
  return `Ok` and do nothing.
- Freestanding: none of it (`profiles.md` §4).

## 3. What it does not do

- **No `fork`.** A program that wants a copy of itself uses `extern "C"`, as today, and takes on what that means.
- **No pipes to the child.** `Process.capture` remains the way to read what a program writes; a child with pipes
  that the parent reads while it runs needs async pipes, which lib/std does not have.
- **No resource limits** (`setrlimit`, job objects) and **no change of user or directory** for the child: each is
  its own design, and ADR 0170 is the proposal for memory.

## 4. Consequences

- A supervisor is written in CeeSafe alone: it starts itself again as each worker (`Process.executable()`, an
  argument for the worker's number), polls them, and ends them. httpserver's `extern "C"` block goes, except for
  the shared memory its workers map, and it gains Windows.
- **Workers no longer inherit a listening socket or a mapping by being copies.** Each binds the port with
  `reusePort` (Linux spreads connections; macOS does not, which httpserver's inherit mode exists for), or is
  handed what it needs by a name: a shared-memory object, a socket passed over a Unix socket. Those are not here,
  so on macOS the supervisor's inherit mode still needs `fork` until they are. This is the cost of §1's last
  point, and question 2 asks whether it is acceptable.
- The runtime gains a table of children it has started and not yet reported, on POSIX, so that a dropped
  `ChildProcess` does not leave a zombie for the life of the parent.

## 5. Questions for the owner

1. A `ChildProcess` value from `Process.spawn`, with `id`, `poll`, `wait`, `terminate` and `kill` (§2.1)?
   Recommended: yes.
2. No `fork` in lib/std, knowing that a worker which must inherit a socket or a mapping still needs it (§4)?
   Recommended: yes, and a later ADR for named shared memory and for passing a descriptor, which are what the
   supervisor actually inherits.
3. Leave `waitAny` and an awaitable exit out of this ADR (§2.1)? Recommended: yes: polling four workers every
   20 ms is what httpserver's supervisor does now, and both additions touch the reactor.
4. Dropping a `ChildProcess` neither ends nor waits for the child (§2.1)? Recommended: yes. Ending it would make a
   moved or forgotten value kill a process; waiting would make a drop block.

## 6. Implementation plan (once accepted)

1. Spec: §26.16, `spec/api/std.md`, the guide; profiles.md (hosted only).
2. Runtime: `ceesafe_host_spawn`, `ceesafe_host_child_poll`, `_wait`, `_signal`, `_forget`, `ceesafe_host_pid`,
   `ceesafe_host_executable`, for the three systems; the POSIX table of forgotten children. lib/std over them; the
   interpreter's host services, sandboxed as `Process.run` is.
3. Tests:
   - a check like `tools/signal_check.py` (the corpus runner starts no helper programs): a parent that spawns three
     children, sees each one's status, ends one with `terminate` (the child subscribed and exits 0) and one with
     `kill` (`Signal(9)`), and drops the third and exits while it runs; a program that does not exist
     (`NotFound`); `poll` and `wait` after the exit; a reused id never signalled (by construction, and by review);
   - on Windows the same programs by the release workflow's smoke job; CosmoOS reports `Unsupported`;
   - httpserver's supervisor on `Process.spawn`, with `tools/supervisor_check.py`, as the program that asked.
4. ROADMAP §10.
