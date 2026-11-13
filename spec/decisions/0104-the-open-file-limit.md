# ADR 0104 — The open-file limit

- Status: **Accepted** by the owner, 2026-11-12 (every question of §6 answered as recommended: §2.1 and §2.2 accepted,
  `IoError.TooManyOpenFiles` in the same release, no raise at start by the runtime, released alone as soon as it
  passes; the owner said 0.39.0, which went to ADR 0102, an index updated in place keeps its bounds, and 0.40.0 to
  ADR 0103, `partitionPoint`, the same day, so this ADR is 0104), implemented on branch `impl/0104`, on 0.40.0, and released in ceesafec 0.41.0. Raised by CosmoStore's CEESAFE_GAPS G32
  (`~/dev/CeeSafeApps/cosmostore/CEESAFE_GAPS.md`) and its ADR-0039
  (`~/dev/CeeSafeApps/cosmostore/docs/adr/0039-out-of-descriptors-the-journal-keeps-going.md`).
- Scope: lib/std `host.cee` (a new `ProcessLimits`), the hosted runtime (`ceesafe_rt_io.c`, `rt.h`), the interpreter,
  type-system.md §26 (a new §26.14), guide/builtins.md, the corpus. §2.3, if accepted, also the built-in `IoError`
  (checker, emitter, interpreter, §26.2).

## 1. Context

- **A server holds a descriptor per connection and per open file,** and the soft limit a process starts with is far
  below what a busy one needs:
  - macOS: 256 (`launchctl limit maxfiles`), with an unlimited hard limit; the kernel's real cap is
    `kern.maxfilesperproc` (61,440 on an M1, 122,880 on an Intel Mac mini);
  - Linux (Debian 13 in a container): 1,024, with a hard limit of 524,288;
  - Windows: CeeSafe's runtime opens files and sockets as Win32 handles (`CreateFileW`), which have no per-process
    count limit.
- **What servers do:** raise the soft limit to the hard one at start. Go's runtime has done so on every Unix since
  1.19. nginx has `worker_rlimit_nofile`.
- **CeeSafe cannot read or set it.** lib/std has no `getrlimit`/`setrlimit`, and `EMFILE` reaches a program only as
  `IoError.Other(24)`.
- **What it cost CosmoStore:** on an Intel Mac mini started over ssh (soft limit 256), 128 concurrent 256 KiB PUTs ran
  out of descriptors (128 sockets plus an extent file per upload). An open in its journal failed, and the journal
  fail-stopped until a restart (fixed on its side by ADR-0039, which now answers 503 instead). With `ulimit -n 10240`
  the same load ran clean. The operator has to know to raise the limit, and the failure only shows under load.
- **Measured with a C probe** (`setrlimit(RLIMIT_NOFILE)`, soft limit only):
  - macOS 27, M1: soft 256, hard unlimited. Setting the soft limit to unlimited, to 61,441 or to 61,440 all succeed.
    The value read back is what was set, though `kern.maxfilesperproc` still caps what can be opened.
  - Debian 13: soft 1,024, hard 524,288. 524,288 succeeds; 524,289 is `EINVAL`.
  - Not yet measured: macOS 15. Apple's `setrlimit(2)` says it "no longer accepts `rlim_cur = RLIM_INFINITY`" for
    `RLIMIT_NOFILE` and recommends `min(OPEN_MAX, rlim_max)` (`OPEN_MAX` is 10,240), which macOS 27 no longer
    enforces. The implementation clamps on Apple systems to `kern.maxfilesperproc`, and retries with `OPEN_MAX` if
    that is refused (§2.2).

## 2. Proposal

### 2.1 Reading the limit

`ProcessLimits.openFiles()`, in lib/std, returns `Result<OpenFileLimit, IoError>`:

```cee
public struct OpenFileLimit {
    /// The soft limit: the most descriptors the process may hold now.
    public current: u64;
    /// The hard limit: the most `current` may be raised to without privilege. u64's maximum when unlimited.
    public maximum: u64;
}
```

- POSIX: `getrlimit(RLIMIT_NOFILE)`, with `RLIM_INFINITY` as u64's maximum.
- Windows: both u64's maximum (no count limit on handles).
- The interpreter: `Unsupported`.

### 2.2 Raising it

`ProcessLimits.raiseOpenFiles(to: u64)` returns `Result<u64, IoError>`, the soft limit afterwards.

- It sets the soft limit to `min(to, maximum)`, and on Apple systems also to at most `kern.maxfilesperproc`; if
  that is refused (`EINVAL`), it retries with `min(to, maximum, OPEN_MAX)`, so the call succeeds on every macOS
  release.
- It never lowers the limit: a `to` at or below `current` changes nothing and returns `current`.
- Raising the hard limit needs privilege and is not offered.
- Windows returns u64's maximum and changes nothing; the interpreter returns `Unsupported`.
- A server's start is then `ProcessLimits.raiseOpenFiles(u64.max)` (or a configured number), once.

### 2.3 A named error for "too many open files" (optional; question 2)

A new built-in `IoError` variant, `TooManyOpenFiles`, after `StorageFull`: `EMFILE` and `ENFILE`, and Windows'
`ERROR_TOO_MANY_OPEN_FILES` (which the runtime already maps to `EMFILE`). `message()` is "Too many open files".

- A program can then tell a shortage it may wait out from a failed disk, without a platform's errno number.
- As with `StorageFull` (ADR 0077), a `match` that names the first twenty-one variants and has no `_` arm no longer
  covers `IoError` (N0256): a source change.

## 3. Alternatives considered

- **Raising the limit in the runtime at start, for every program,** as Go does: no API needed. But a CeeSafe program
  is also a library's host or a short tool, and raising a process-wide limit unasked is a policy. Kept for the owner
  (question 3).
- **`setrlimit` in general** (`ProcessLimits.set(kind, …)`): more than anything has asked for. Other limits can join
  `ProcessLimits` when a program needs them.
- **Only the named error (§2.3), no limit API:** a server could then answer 503 cleanly, but would still run out at 256.

## 4. Not in this ADR

- Raising the hard limit, or other `RLIMIT_*` kinds.
- An async form: both calls are a system call or two, not I/O, and a server makes them once at start.

## 5. Testing

- Corpus run-pass test: read the limit; raise it to `current + 1` where `current < maximum`, and read it back; raising
  to a lower value leaves it; on Windows, both are u64's maximum.
- A test that opens files past the soft limit: lower it first (from the test, in a child process run with
  `ulimit -Sn 64`), open 100 files after `raiseOpenFiles(128)`, and expect them to open.
- With §2.3: an open past the limit is `IoError.TooManyOpenFiles`, `message()` included, in the compiled program and
  the interpreter's error mapping.
- Contract tests on the Mac (macOS 27), the Debian container, and macOS 15 on the Intel Mac mini; the release
  workflow's Windows jobs.

## 6. Questions for the owner

1. **Accept §2.1 and §2.2** (`ProcessLimits.openFiles` and `raiseOpenFiles`)? Recommended.
2. **Add `IoError.TooManyOpenFiles` (§2.3)?** It is a source change for exhaustive matches, like `StorageFull`.
   Recommended: yes, in the same release.
3. **Should the runtime raise the limit itself at start** (Go's choice), instead of leaving it to the program?
   Recommended: no, leave it to the program, which may know a number it wants.
4. **Release:** alone as 0.41.0, or with whatever else is ready? Recommended: as soon as it passes, alone if nothing
   else is ready.

## 7. Implementation and evidence

- **Where:**
  - `runtime/hosted/ceesafe_rt_io.c`: `ceesafe_host_open_files` and `ceesafe_host_raise_open_files` after
    `ceesafe_host_space`, with `<sys/resource.h>` where the system has it (`CEE_HAVE_RLIMIT`; else `Unsupported`) and
    `<sys/sysctl.h>`, `<limits.h>` on Apple systems; `ceesafe_io_kind` maps `EMFILE` and `ENFILE`.
  - `runtime/include/ceesafe/rt.h`: `CEE_IO_TOO_MANY_OPEN_FILES` (21) and the two declarations;
    `runtime/core/ceesafe_rt_core.c`: the message.
  - `lib/std/host.cee`: `OpenFileLimit`, `ProcessLimits`, the externs, and kind 21 in `HostServices.hostError`.
  - The checker's variant list (`src/check/structs.cee`); the interpreter (`src/interpreter/io.cee`): the message,
    `errnoOfMessage`, `ioKindOf` (`EMFILE`, `ENFILE`), and `Unsupported` for both host calls. The compiler's own
    sources name no new variant, so the bootstrap chain (0.8.0) builds them.
  - type-system.md §26.2 and §26.14, the guide, `spec/api/std.md` (`ceesafec doc lib/std`); corpus
    `run-pass/process-limits.cee`.
- **Measured before writing it** (a C probe of `setrlimit`, soft limit only): macOS 27 on an M1 accepts any value,
  unlimited included, with `kern.maxfilesperproc` 61,440 the real cap; Debian 13 accepts its hard limit (524,288) and
  refuses one more with `EINVAL`. macOS 15 was not reachable for this release, so the `OPEN_MAX` fallback is untested
  there; on macOS 27 it is never taken.
- **The new test:** natively with the shell's limit (1,048,576: too large to exhaust, so that part prints its line
  without opening), under `ulimit -Sn 256` (raised to 272, then opens until one is `TooManyOpenFiles`, with its
  message), and in the interpreter (`Unsupported`): the same four lines.
- **Checks (Debian 13, x86-64, in the CosmoStore benchmark container, 0.40.0's main with and without the branch, both
  as git checkouts):** `tools/contract_tests.py` passes the same 34 suites on both and fails the same 7 on both
  (corpus's partial status, `run fortify` and `run split` on two ffi programs, `run interp` on run-fail, packages,
  embedding, cbind). Those 7 fail on 0.39.0's main in that container too: the container's, not this ADR's. The
  corpus's run-pass family: 328 on the branch, 327 without.
- **Checks (macOS 27, M1, load average 9 to 27 from two virtual machines):** `tools/contract_tests.py` passes every
  suite but the corpus's partial status, as on main; run-pass 328 with `-Werror`. `tools/spec_check.py`: 0 problems.
- **Found on the way:** the interpreter's `ioKindOf` does not map `ENOSPC` to `StorageFull` (ADR 0077); an
  interpreted program gets `Other(28)`. Not changed here.
- **Not run:** Windows (the release workflow builds and smoke-tests it), CosmoOS, macOS 15.
