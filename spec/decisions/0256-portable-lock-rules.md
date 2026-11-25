# ADR 0256 — Portable lock rules, checked on any host: `CEESAFE_LOCK_RULES`

- Status: **Proposed — implemented**, 2026-11-25, on the branch `fix/g5-files` (from 0.95.10, not merged, not
  released), at the owner's request to close the SQLite port's finding 48 ("Windows' lock rules cannot be checked off
  Windows", CeeSafeApps/sqlite CEESAFE_FINDINGS.md). It extends ADR 0220 and ADR 0234. The names in §2 are this ADR's;
  the owner may rename them before a release.
- Scope: type-system.md §26.3 (files); lib/std's `FileLocks` (the panics of §2.3); the hosted runtime's
  `ceesafe_host_file_lock`, `ceesafe_host_file_test_lock` and `ceesafe_fd_drop` (runtime/hosted/ceesafe_rt_io.c);
  the interpreter's model of locks (src/interpreter/io.cee); `tools/file_lock_check.py`; `.github/workflows/windows.yml`.

## 1. Context

ADR 0220 §2.2 and ADR 0234 §2.3 give the portable contract for advisory locks: **lock a range the `File` does not
hold, and unlock exactly the ranges it locked**. POSIX hosts allow more: locking a range the `File` holds converts or
merges its lock there, and an unlock may name any bytes. Windows does not (`LockFileEx` never converts or merges, and
`UnlockFileEx` releases only a range exactly as it was locked), and the interpreter models POSIX's rules. So code
meant for Windows that breaks the contract passes every test on macOS and Linux and fails only on a Windows machine:

- a recovery that locks the checkpointer's byte again while a checkpoint holds it: on Windows a `tryLock` is `false`
  (read as "busy") and a `lock` waits for ever, on its own lock;
- an unlock of PENDING and RESERVED as one range, which were locked one by one: on Windows nothing is released (the
  runtime reports `ERROR_NOT_LOCKED` as success, ADR 0220 §2.1), so the bytes stay locked and the next connection
  waits.

The hosts also differ the other way: POSIX refuses a shared lock on a `File` not open for reading and an exclusive
one on a `File` not open for writing (`BadDescriptor`), Windows grants both, and the interpreter does not check
(ADR 0220 §2.4). A program that takes an exclusive lock on a read-only `-shm` (os_win.c's DMS byte) works on Windows
only.

The finding asks for a way to check a portable program on any host. The SQLite port's tests should be able to run
with it on macOS and Linux CI and fail where a Windows run would.

## 2. Decision

### 2.1 The switch: an environment variable

`CEESAFE_LOCK_RULES` in a program's environment chooses the rules its `File` locks follow, natively and under
`ceesafec run --interp`:

| Value | Rules |
|---|---|
| unset, or empty | the host's (ADR 0220, ADR 0234): no change |
| `portable` | §2.2's rules; a lock over **exactly** a range the `File` holds, of the other kind, converts it (POSIX's step from SHARED to EXCLUSIVE, os_unix.c's scheme) |
| `strict` | §2.2's rules, and no conversion either: every lock is on bytes the `File` does not hold, which is Windows' rule |
| anything else | every lock call panics, naming the variable, so a misspelt value never turns the checks off |

It is read once, at the program's first lock call. It is an environment variable, not an option of `File` or of
the compiler, so a test suite runs with it without a change to the program or its build (`CEESAFE_LOCK_RULES=strict
ceesafec test`, a CI job's `env:`), and a program cannot turn it off for itself. The runtime already reads test hooks
this way (`CEESAFE_DNS_DELAY_MS`, ADR 0048).

### 2.2 What the rules refuse

The rules concern the `File`'s own locks only; a conflict with another holder is the host's, unchanged (`tryLock`
`false`, `lock` waits, `testLock` names it). With `portable` or `strict`, on every host:

1. **A lock over a range the `File` holds**: `lock` or `tryLock` of a range that overlaps any range the `File` has
   locked (and not unlocked) **panics**, before the host is asked, unless the value is `portable`, the range is
   exactly one held range (the same start and length) and the kind differs: that is a conversion, done as on POSIX
   (ADR 0234 §2.3). A lock of the same range and kind again is refused under both values: it is the finding's lock
   taken twice.
2. **An unlock of a range not locked as given**: `unlock` of a range that is not exactly one held range (a part of
   one, two adjacent ones as one, bytes never locked, a range unlocked already) **panics**.
3. **A test over a range the `File` holds**: `testLock` of a range that overlaps one the `File` holds **panics**:
   on POSIX the `File`'s own locks are not counted and on Windows they are (ADR 0234 §2.2), so the answer differs by
   host.
4. **A lock the `File`'s access does not allow**: a shared lock on a `File` not open for reading, or an exclusive one
   on a `File` not open for writing, is `Err(IoError.BadDescriptor)`, POSIX's answer, on Windows and in the
   interpreter too. (Not a panic: it is POSIX's ordinary error, which a portable program already handles.)

The panics are bugs of the program, as an index out of bounds is: Windows' own answers (a `false` that means "you
hold it", a wait that never ends, an unlock that does nothing) cannot be told from a busy database by the program,
and a panic shows where the rule was broken. The messages name the rule and the variable:

```text
FileLocks.lock: the File already holds a lock on bytes of this range, which Windows refuses (CEESAFE_LOCK_RULES)
FileLocks.unlock: the range is not one the File locked, and Windows releases only those (CEESAFE_LOCK_RULES)
FileLocks.testLock: the File holds a lock on bytes of this range, which Windows counts and POSIX does not (CEESAFE_LOCK_RULES)
CEESAFE_LOCK_RULES is neither portable nor strict
```

**What `strict` refuses, against Windows.** Every request Windows refuses to a handle because of the handle's own
locks (`ERROR_LOCK_VIOLATION`, `ERROR_NOT_LOCKED`) is refused by `strict`. `strict` also refuses the requests Windows
grants on a handle that holds an overlapping lock — a shared lock over its own shared lock, and over its own
exclusive one — because Windows then **stacks** the two locks, each released by its own unlock, where a POSIX host
replaces one with the other: the program's state after the call differs by host, so it is not portable either.
`tools/file_lock_check.py` checks both statements against `LockFileEx` itself (§4). Under `portable`, the conversion it
allows behaves on Windows as Windows decides: shared to exclusive is refused (`tryLock` `false`, `lock` waits for
ever), and exclusive to shared is granted by stacking, so the bytes stay locked exclusively until both are unlocked:
`portable` is for checking a POSIX scheme, `strict` for a scheme that must run on Windows.

**What the rules do not check.** Windows' locks are mandatory (a byte locked exclusively cannot be read or written
through another handle, ADR 0220 §2.2): the rules do not model that, and the contract (lock bytes the program does
not read) stays the program's. Nor do they make the program's `File`s of one file conflict on the BSDs, whose locks
are the process's (ADR 0220 §2.2).

### 2.3 Where it is done

- **lib/std** (`FileLocks`): the runtime's and the interpreter's answers gain one code, 3, "refused by the rules"
  (for `lock`, `tryLock` and `unlock`, beside 0 and 1; for `testLock` beside 0, 1 and 2), and 4, "an unknown
  value"; lib/std panics with §2.2's messages. Without the variable neither code is ever returned, and the calls are
  as before.
- **The runtime** keeps, only while the rules are on, a table of the ranges each descriptor holds (descriptor, start,
  end, kind), behind a lock of its own, so several threads may lock through one `File`: a request is checked and its
  range reserved in the table before the host call, and the reservation is dropped if the host refuses or reports a
  conflict; a conversion changes the entry's kind once the host grants it. `ceesafe_fd_drop` (a `File`'s drop) removes
  the descriptor's entries, so a descriptor number reused by a later `File` starts with none. The access check is
  `fcntl(F_GETFL)` on POSIX and `NtQueryInformationFile(FileAccessInformation)` on Windows (`FILE_READ_DATA`;
  `FILE_WRITE_DATA` or `FILE_APPEND_DATA`), looked up in ntdll at run time; if it cannot be asked, the lock is left to
  the host.
- **The interpreter** applies the same rules to its model (ADR 0220 §2.4), whose ranges per descriptor are the
  table above, and takes the access from its handle (`File.open` and `openRegular` read, `File.create` and
  `openAppend` write, the read-and-write opens both). It reads the variable from its own environment, which is the
  program's.

### 2.4 Using it

The SQLite port runs its lock tests a second time with `CEESAFE_LOCK_RULES=strict` and its Windows scheme forced on
(os_win.c's steps, which take no lock twice and convert nothing), on macOS and Linux CI, and with `portable` and its
POSIX scheme (os_unix.c's SHARED-to-EXCLUSIVE conversion, which `portable` allows). A lock mistake of the Windows
scheme then panics on any host, at the call that makes it.

### 2.5 The owner's decisions (2026-11-25)

- **Both values stay.** `portable` is for code that follows os_unix.c, whose SHARED-to-EXCLUSIVE step converts a held
  range in place: it checks everything else of the contract (no lock taken twice, no overlap, unlocks as locked)
  while letting that step through, so a POSIX scheme can be run under it on every POSIX host. `strict` is for
  checking Windows' rules, for example the engine's Windows path forced on on macOS or Linux: it refuses the
  conversion too, which Windows refuses (shared to exclusive) or stacks (exclusive to shared, §2.2), so it is the
  value whose refusals are Windows' (§4's oracle). One value would serve only one of the port's two schemes.
- **An exclusive lock on a `File` not open for writing is refused on every host, under both values**
  (`BadDescriptor`), and a shared one on a `File` not open for reading likewise. It is the portable answer: POSIX
  refuses it (`EBADF`), so code that relies on Windows granting it (os_win.c's exclusive try of a read-only `-shm`'s
  DMS byte) breaks on POSIX hosts, and the rules make it break on Windows too, where it would otherwise pass. A
  program that must take such a lock opens the file for writing, or takes the lock only where the host grants it,
  outside the rules.

## 3. Options not taken

- **Emulating Windows' answers** (a `tryLock` that is `false` over the `File`'s own lock, a `lock` that never returns,
  an unlock that does nothing): faithful, but a test would then fail as a hang or a busy error far from the mistake.
- **An option of `File`** (`File.openReadWrite(path, rules)`) or of `FileLocks` (`FileLocks.setRules`): the program
  would have to be changed to be checked, and a library inside it could turn the checks off for the rest.
- **A ceesafec flag** (`--lock-rules=strict`): it reaches `run` and `test` but not a program started by a test (the
  SQLite port's tests start child processes), and natively it would have to become an environment variable anyway.
- **Always on in the interpreter**: it would change what released programs print under `run --interp` (a POSIX
  program that converts in place), and the native build would still need its own switch.
- **Only the owner's first list, conversions allowed** (`portable` alone): it would miss a Windows scheme that
  converts a range in place, which Windows refuses; `strict` catches it. Both are offered because the port has a
  scheme of each kind.
- **Panicking for the access mode** (§2.2, 4): `BadDescriptor` is what POSIX hosts already return, so a program that
  handles it on Linux handles it here.

## 4. Tests

- Corpus, each natively, with `--interp` and with `--check-leaks`, the variable set by a new directive of the corpus
  format, `// env: NAME=value` (`tools/run_corpus.py`, spec/testing.md §1, beside `// stdin:`):
  - `run-pass/file-lock-rules.cee` (`strict`): locks and unlocks that keep the contract (os_win.c's steps from
    SHARED to EXCLUSIVE, unlocking and locking again; adjacent ranges unlocked one by one; a lock after its unlock;
    a shared lock beside another `File`'s; a test of a range not held), and `BadDescriptor` for an exclusive lock on
    a `File.open` file and a shared one on a `File.create` file;
  - `run-fail/file-lock-rules-twice.cee` (`strict`: the same range locked twice),
    `file-lock-rules-overlap.cee` (`strict`: a shared lock over part of a held one),
    `file-lock-rules-convert.cee` (`strict`: a conversion in place, which `portable` allows),
    `file-lock-rules-unlock-joined.cee` (`portable`: two ranges unlocked as one),
    `file-lock-rules-unlock-part.cee` (`strict`: part of a range),
    `file-lock-rules-test.cee` (`strict`: `testLock` over a held range) and
    `file-lock-rules-unknown.cee` (the value `Strict`): each panics with §2.2's message at the line that breaks the
    rule.
  - A conversion under `portable` is not in the corpus, since Windows itself refuses it and the corpus prints the
    same on every host: `tools/file_lock_check.py` checks it on POSIX hosts.
- `tools/file_lock_check.py` part (5): 22 sequences of `tryLock` and `unlock` steps on one handle (same, overlapping,
  inner, enclosing, adjacent and disjoint ranges of each kind; exact, partial, larger, joined, unheld and repeated
  unlocks; os_win.c's and os_unix.c's steps), run by one program under `strict` and under `portable`, natively and in
  the interpreter, stop (panic) exactly where §2.2's model says. Against Windows: each sequence is run on one handle
  with `LockFileEx` and `UnlockFileEx` through Python's `ctypes` (independent of CeeSafe's runtime) — live on
  Windows (the windows workflow runs the script), and on other hosts from the answers recorded there
  (`WINDOWS_RECORDED`, from the windows workflow's run 45577525614 on windows-2022, 2026-11-25) — and every step
  `strict` lets through must be granted by
  Windows, and the step `strict` refuses must be refused by Windows or granted by stacking (one unlock of it leaves
  the bytes locked against another handle's exclusive try).
- Without the variable every existing lock test (`file-locks.cee`, `file-lock-test.cee`, `tools/file_lock_check.py`
  parts 1 to 3) is unchanged.
