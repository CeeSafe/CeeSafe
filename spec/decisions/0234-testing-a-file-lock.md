# ADR 0234 — Testing a file lock without taking it

- Status: **Proposed — implemented**, 2026-11-24, on the branch `fix/findings3-io` (not merged, not released), at the
  owner's request to close the SQLite port's finding 36 ("A lock cannot be tested without taking it",
  CeeSafeApps/sqlite CEESAFE_FINDINGS.md). It extends ADR 0220. The names in §2 are this ADR's; the owner may rename
  them before a release.
- Scope: type-system.md §26.3 (files); lib/std's `FileLocks.testLock`; the hosted runtime's
  `ceesafe_host_file_test_lock` (runtime/hosted/ceesafe_rt_io.c, rt.h); the checker's name `f.testLock(…)`; the
  interpreter's model of it; `tools/file_lock_check.py`.

## 1. Context

SQLite asks two questions about locks that change nothing:

- **Is a hot journal being written?** `unixCheckReservedLock` (os_unix.c) asks whether another connection holds
  RESERVED (byte 0x40000001) with `fcntl(F_GETLK)` for a write lock on that byte. Before it trusts a hot journal it
  must know that no writer is alive.
- **Am I the first connection to the wal-index?** `unixLockSharedMemory` asks with `F_GETLK` whether anyone holds the
  `-shm`'s DMS byte; if no one does, it truncates the `-shm` and takes the byte exclusively.

ADR 0220 gave `lock`, `tryLock` and `unlock`. The port tests with `tryLock` then `unlock`, which **takes** the lock
for an instant: another connection's `tryLock` of RESERVED in that instant fails once and goes to its busy handler.
POSIX's `F_GETLK` and Linux's and macOS's `F_OFD_GETLK` answer without side effect, and also say which kind of lock
conflicts (`l_type`), which the DMS test uses (a shared holder means "others are here", not "a writer is here").

The finding also asks about ADR 0220 §2.2's portable rule against converting a held lock in place: os_unix.c goes
from SHARED to EXCLUSIVE by an `F_SETLK` of a write lock over the shared one; os_win.c cannot, and does not.

## 2. Decision

### 2.1 The API (lib/std, hosted)

```ceesafe
f.testLock(kind: FileLock, start: u64, length: u64) -> Result<FileLock?, IoError>   // FileLocks.testLock(f, …)
```

- `null` when a `kind` lock on bytes `start` to `start + length - 1` could be taken now: no **other** holder's lock
  conflicts with it. `Some(k)` when one does, `k` being that lock's kind: for a shared request the conflict is always
  `Exclusive`; for an exclusive request it is `Shared` or `Exclusive`, and when several holders conflict, which one
  is named is unspecified (POSIX's `F_GETLK` reports "one of" them). A caller that wants to know whether *any*
  writer holds a range tests a `Shared` lock: `Some(Exclusive)` exactly when one does.
- **It takes nothing on POSIX hosts**: no lock is set, waited for or released, so no other holder sees the test.
- The range, its checks (`InvalidInput` for `length` 0 or a range past 2^63 - 1) and the holders are ADR 0220's:
  where locks are the `File`'s (open-file-description locks) the program's other `File`s are other holders and the
  `File`'s own locks are not; where they are the process's (FreeBSD and the other BSDs, the fallbacks of §2.2 of
  ADR 0220) the process's own locks are not counted, through any `File`. Other processes' locks always are, of
  either kind (classic `fcntl` and open-file-description locks conflict with each other).
- It is `borrow this`, as the other lock methods, and has no effect (it never waits). Any open `File` may test,
  whatever its access mode: `F_GETLK` checks none, and neither does the Windows emulation below.
- The answer is a snapshot: by the time the caller acts, the holder may have gone or another may have come. A
  protocol that acts on it confirms with the lock it takes next, as SQLite does (a hot journal is rolled back only
  under EXCLUSIVE, and the `-shm` is truncated only under the DMS byte's exclusive lock).

### 2.2 Windows: a try and an immediate release

Windows has no `F_GETLK`. `LockFileEx` with `LOCKFILE_FAIL_IMMEDIATELY` is tried, and on success released at once
with `UnlockFileEx` of the same range; for an exclusive request that fails, a shared try tells a shared holder from
an exclusive one. This is what SQLite's own os_win.c does (`winCheckReservedLock` locks RESERVED and unlocks it), so
it is the behaviour SQLite's Windows connections already expect of each other. Its documented differences:

- **The test holds the lock for an instant.** Another holder's `tryLock` of an overlapping range in that instant is
  `false`, and its `lock` waits for the instant to pass; since Windows' locks are mandatory, another handle's read or
  write of those bytes in that instant fails. (Lock only bytes that are not read, ADR 0220 §2.2, and this never
  matters.)
- **The `File`'s own locks count.** A lock never merges or converts on Windows, so a range the `File` already holds
  conflicts with the try; `testLock` then answers as if another holder had it. A caller knows its own locks.

The alternative, `Unsupported` on Windows, was rejected: a program written for every host could then not test a lock
at all, and would fall back to `tryLock` and `unlock`, which is the same side effect with less information.

### 2.3 Converting a held lock (ADR 0220 §2.2, settled)

ADR 0220's portable contract stays: **lock a range the `File` does not hold, and unlock exactly the ranges it
locked.** It is made precise for POSIX hosts, which is what a program following os_unix.c needs:

- **POSIX hosts** (open-file-description or process locks alike): locking a range the `File` holds converts its lock
  there, shared to exclusive or back, **atomically**: a `tryLock` that meets another holder's conflicting lock is
  `false` and leaves the held lock as it was (`fcntl`'s rule), and a `lock` waits with the held lock kept. That is
  os_unix.c's SHARED → EXCLUSIVE step, and the interpreter models it (ADR 0220 §2.4).
- **Windows:** never converts. A program that must run there takes os_win.c's steps, which also work on POSIX hosts
  and interoperate with os_unix.c's: a reader's SHARED is a shared lock on the SHARED range (`winGetReadLock`); a
  writer that holds PENDING exclusively, so that no new reader starts, unlocks it (`winUnlockReadLock`), then
  `tryLock`s the whole SHARED range exclusively; if that fails it locks the range shared again and reports busy.
  The step is not atomic, and PENDING is what makes that safe: between the unlock and the exclusive lock no reader
  can enter, and the writer's RESERVED keeps other writers out. The locks themselves are the same bytes and kinds
  as os_unix.c's, so the two schemes exclude each other.

A program chooses one scheme for all its hosts or by its build; this ADR adds no query for "converts in place"
(§3).

### 2.4 The runtime

`ceesafe_host_file_test_lock(file, kind, start, length) -> int64_t` (runtime/hosted/ceesafe_rt_io.c): `kind` 1 shared,
2 exclusive; 0 no conflict, 1 a shared lock conflicts, 2 an exclusive one, or a negated errno. POSIX: `F_OFD_GETLK`
where `ceesafe_host_file_lock` takes open-file-description locks (`F_OFD_SETLK` defined, a 64-bit `off_t`), falling
back to `F_GETLK` on `EINVAL` as it does; elsewhere `F_GETLK`. Windows: §2.2. lib/std checks the range first.

### 2.5 The interpreter

The interpreter answers from its model of the program's own locks (ADR 0220 §2.4): a lock of another `File` of the
program that overlaps the range and conflicts (either is exclusive) is reported with its kind, the first one found;
the `File`'s own locks are not. As for its locks, **other processes' locks are not seen**; a test of interoperation
runs natively (`tools/file_lock_check.py`).

## 3. Options not taken

- **`Result<bool, IoError>`.** It answers the hot-journal question but not the DMS one (shared holder or exclusive),
  which `F_GETLK` answers in the same call.
- **The holder's process id** (`F_GETLK`'s `l_pid`). An open-file-description lock has none (`F_OFD_GETLK` reports
  -1), Windows has none, and a process id is stale by the time it is read. SQLite does not use it.
- **`Unsupported` on Windows** (§2.2).
- **A query whether locks convert in place** (`FileLocks.convertsInPlace()`). A program that runs on Windows takes
  os_win.c's steps everywhere, which need no query; one that is POSIX only takes os_unix.c's. If a port wants both
  schemes chosen at run time, the query is a one-line addition.
- **Allowing conversion on Windows** by unlocking and relocking inside `lock`. It would not be atomic: between the
  two calls another holder may take the range, so a failed "conversion" would have lost the lock it had. Hiding that
  would break the property os_unix.c relies on.

## 4. Tests

- `tests/reference/tests/corpus/run-pass/file-lock-test.cee`, the same output natively and in the interpreter, on
  every host (it tests only ranges the testing `File` does not hold, since Windows counts its own): a range no one
  holds tests `null` for both kinds, and still does after a lock and its unlock; another `File`'s exclusive lock is
  reported `Exclusive` for both kinds of request, and its shared lock `Shared` for an exclusive request and `null` for
  a shared one. Where two `File`s of one program do not conflict (process locks, the BSDs) there is nothing to
  report, so the program prints whether `testLock` agrees with `tryLock` and `unlock` from the same `File`, which it
  must on every host, and the kind only where the `File`s conflict; `InvalidInput` for length 0 and a range past
  2^63 - 1; a `File` open only for reading may test an exclusive lock.
- `tools/file_lock_check.py`: (1) two `File`s of one program, natively and in the interpreter (Linux, macOS): a
  conversion in place that fails while the other `File` shares, and keeps the shared lock (§2.3), the `File`'s own
  lock never reported to it, the converted lock reported `Exclusive` and, converted back, `Shared`; (2) Python's
  `fcntl.lockf` (classic locks, in the checker's process) holds a shared, then an exclusive lock, and the program's
  `testLock` reports `Shared`/`null` and `Exclusive`/`Exclusive` for exclusive/shared requests, and `null` once it
  is gone; while the program tests a byte 200 000 times, Python takes and releases it without waiting as fast as it
  can, and none of its attempts is refused (nothing was taken; the same loop with `tryLock` and `unlock`, tried by
  hand on macOS arm64, had 20 939 of 120 857 refused); (3) while `sqlite3` holds `BEGIN IMMEDIATE` (RESERVED), the
  program's `testLock` of the RESERVED byte is `Exclusive`, and `null` after `COMMIT`: SQLite's hot-journal question.
- Windows: the code is written for §2.2 and compiled by the release workflow; it has not been run here.
