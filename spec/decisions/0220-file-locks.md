# ADR 0220 — Advisory locks on byte ranges of an open file

- Status: **Proposed**, 2026-11-24, and implemented on the branch `fix/findings-os` (not merged, not released) at the
  owner's request to close the SQLite port's finding 3 ("No advisory file locks"). The names in §2 are this ADR's;
  the owner may rename them before a release.
- Scope: type-system.md §26.3 (files); lib/std's `FileLocks` and `FileLock`; the hosted runtime's
  `ceesafe_host_file_lock`; the interpreter's model of it; `tools/file_lock_check.py`.

## 1. Context

A program that shares a file with other processes needs advisory locks. The SQLite port could not follow SQLite's
locking protocol, so it documented "one process at a time" (CeeSafeApps/sqlite, CEESAFE_FINDINGS.md finding 3).
That protocol does not lock the whole file: it locks bytes far past the data, which no page ever holds (SQLite's
`os_unix.c` and `os_win.c`):

| Lock | Bytes | Kind |
|---|---|---|
| PENDING | 0x40000000, one byte | shared while a reader takes SHARED; exclusive while a writer waits for readers to leave |
| RESERVED | 0x40000001, one byte | exclusive: one writer intends to write |
| SHARED | 0x40000002, 510 bytes | shared by every reader; exclusive for the writer's commit |

To interoperate with the `sqlite3` shell and every other SQLite, the locks must be the ones it takes: POSIX
`fcntl` record locks (`F_SETLK`, `F_RDLCK`/`F_WRLCK`) on Unix, `LockFileEx` on Windows. `flock` locks whole files,
and on Linux and the BSDs it never conflicts with `fcntl` locks, so it cannot serve.

POSIX's process-associated `fcntl` locks have two well-known traps: two descriptors of one process never conflict,
and closing *any* descriptor of the file drops every lock the process holds on it. Linux 3.15 and macOS 13 have
open-file-description locks (`F_OFD_SETLK`, `F_OFD_SETLKW`): owned by the open file, not the process, so two `File`s
of one program conflict as two processes do, and only closing that `File` releases them. They conflict with
classic `fcntl` locks of other processes, so SQLite sees them.

## 2. Decision

### 2.1 The API (lib/std, hosted)

```ceesafe
public enum FileLock { Shared, Exclusive }

f.lock(kind: FileLock, start: u64, length: u64) -> Result<void, IoError>     // waits for the lock
f.tryLock(kind: FileLock, start: u64, length: u64) -> Result<bool, IoError>  // never waits
f.unlock(start: u64, length: u64) -> Result<void, IoError>
```

- Each takes `f` as `borrow this` (`File`'s methods, the checker's names for `FileLocks.lock(f, …)` and the rest,
  as `f.modified()` is `FileMetadata.modified(f)`, ADR 0078 §3). A lock changes nothing a read or write sees, so a
  shared reference to the file may lock it.
- The range is the bytes `start` to `start + length - 1`. A range may lie past the end of the file (SQLite's do).
  `length` 0, or a range that ends past 2^63 - 1 (POSIX's `off_t`), is `InvalidInput`: there is no "to the end of
  the file and beyond" length, which Windows does not have.
- `lock` waits until the lock is granted. `tryLock` is `Ok(true)` if it took the lock and `Ok(false)` if another
  holder's lock conflicts (`EAGAIN`, `EACCES`, Windows' `ERROR_LOCK_VIOLATION`); a conflict is not an error.
- Shared locks of different holders coexist; an exclusive lock conflicts with every other holder's lock on any byte
  of its range.
- `unlock` releases the `File`'s locks on the range; unlocking bytes it does not hold succeeds and does nothing.
- Dropping (closing) the `File` releases its locks; so does the process's end.
- POSIX needs the file open for reading for a shared lock and for writing for an exclusive one: otherwise
  `BadDescriptor` (ADR 0221 §2.2). Windows needs neither.
- Effects: `lock` has `sleep`; `tryLock` and `unlock` have none.

### 2.2 Whose lock: the open `File`

Where the host has open-file-description locks, a lock belongs to the `File` that took it:

- Two `File`s of one program conflict with each other exactly as two processes do, so a lock also serves between
  threads that each opened the file.
- Locking a range the `File` already holds replaces its lock there: a shared lock becomes exclusive (when no other
  holder has the range) and back, and unlocking part of a range splits it. This is `fcntl`'s rule.
- Another `File` of the program closing does not release this one's locks.

Where the host has only process-associated locks (a C library without `F_OFD_SETLK`, as on FreeBSD, NetBSD and
OpenBSD; a Linux kernel before 3.15, which refuses it with `EINVAL`, after which the runtime falls back; a 32-bit
Linux build, whose `off_t` the open-file-description calls do not take), the lock is the process's: `File`s of one program do not conflict, and closing any `File`
of the file releases them all. Processes still exclude each other, which is what interoperating with SQLite needs.
A program that keeps one `File` open per file, as SQLite itself does, behaves the same on both.

On Windows (`LockFileEx`, `UnlockFileEx` on the descriptor's handle), a lock belongs to the handle too, but:

- a lock never converts or merges: locking a range that overlaps one the same `File` holds conflicts with it, so
  `tryLock` is `false` and `lock` waits for ever. Unlock, then lock again (SQLite's own Windows code does this);
- `unlock` releases exactly a range that was locked, by its start and length;
- the locks are mandatory: a byte locked exclusively cannot be read or written through another handle. SQLite's
  bytes lie past any page for this reason; a program should lock bytes it does not read.

The portable contract is therefore: lock a range the `File` does not already hold, and unlock exactly the ranges it
locked. On POSIX hosts the conversions above also work.

### 2.3 The runtime

`ceesafe_host_file_lock(file, kind, wait, start, length) -> int64_t` (runtime/hosted/ceesafe_rt_io.c): `kind` 0
unlocks, 1 is shared, 2 exclusive; `wait` 1 waits. It returns 0 when done, 1 when a `tryLock` met a conflict, or a
negated errno. Interrupted waits (`EINTR`) are retried. lib/std checks the range before the call.

### 2.4 The interpreter

The interpreter is built by the release stage 0 (Decision 0009), whose library has no file locks, and its files
reopen for each positional operation (§26.3), so it cannot hold a host lock. It models the locks among the program's
own `File`s, with §2.2's open-file rules (conversion, splitting, conflicts between `File`s, release on close): the
same answers as a native run for anything the program does to itself. A `lock` that conflicts with another `File`
of the program waits as a thread waits for a mutex: other threads run, and if none can, it is a deadlock fault.
**Other processes do not see the interpreter's locks**, nor it theirs; a test of interoperation runs natively
(`tools/file_lock_check.py`). It does not check the `File`'s access mode (`BadDescriptor` above), which its table of
descriptors does not keep. When the release stage 0 has `File` locks the interpreter can take host locks too.

## 3. Options not taken

- **`flock` (whole file).** It does not interoperate with SQLite, and has no ranges.
- **Process-associated `fcntl` locks everywhere.** They are SQLite's, but their two traps (§1) make them unsafe for
  a library to hand out: any `File` of the file dropped anywhere in the program releases another part's locks.
  Open-file-description locks are the same locks to other processes.
- **A guard value that unlocks on drop.** The lock is a property of the `File` and of byte ranges, which SQLite
  upgrades and downgrades piecemeal while holding others; a guard per range would borrow the `File` for as long as
  the lock is held. Dropping the `File` already releases everything.
- **`length` 0 for "to the end and beyond" (POSIX's `l_len` 0).** Windows has no such range; a program that wants
  the whole file passes `0, 0x7FFFFFFFFFFFFFFF`.
- **A `WouldBlock` error for `tryLock`.** A held lock is an expected answer, not a failure; `bool` keeps `?` for
  real errors.

## 4. Tests

- `tests/reference/tests/corpus/run-pass/file-locks.cee`, the portable contract only, since the corpus runs on every
  host: SQLite's steps from SHARED to EXCLUSIVE by unlocking and locking again; unlocking bytes not held; a range
  past the end; `InvalidInput` for length 0 and for a range past 2^63 - 1; a lock that waits for another thread's
  `File` to unlock (where locks are the process's nothing waits, and the output is the same). The same output
  natively and in the interpreter.
- `tools/file_lock_check.py`: (1) conflicts between two `File`s of one program, conversion, splitting and release on
  close, natively and in the interpreter, on Linux and macOS (skipped, and said so, elsewhere); (2) against Python's
  `fcntl.lockf` in another process (an independent implementation of classic record locks), both ways; (3) where
  `sqlite3` is installed, a database whose SHARED range the program holds exclusively makes a `sqlite3` read report
  "database is locked", one it holds shared makes a write report it, and while `sqlite3` holds `BEGIN EXCLUSIVE` the
  program's `tryLock` of the SHARED range is `false`, and `true` after `COMMIT`. Without `sqlite3` that part is
  skipped and said so.
