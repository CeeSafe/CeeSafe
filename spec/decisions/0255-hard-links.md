# ADR 0255 — `File.link`: a second name for a file

- Status: **Proposed — implemented**, 2026-11-25, on the branch `fix/g5-files` (from 0.95.10, not merged, not
  released), at the owner's request to close the SQLite port's finding 44 ("No hard link", CeeSafeApps/sqlite
  CEESAFE_FINDINGS.md). The names in §2 are this ADR's; the owner may rename them before a release.
- Scope: type-system.md §26.3 (files); lib/std's `HostServices.link` (`File.link`, the checker's name for it); the
  hosted runtime's `ceesafe_host_link` (runtime/include/ceesafe/rt.h, runtime/hosted/ceesafe_rt_io.c); the
  interpreter's model (src/interpreter/io.cee); `tools/file_lock_check.py`.

## 1. Context

A file system's hard link is a second directory entry for the same file: both names open the same data, and the
file lives until its last name is removed and its last descriptor closed. The SQLite port's hot-journal test opens
one database through two names, so that the `sqlite3` shell, which looks for `<name>-journal`, does not see a journal
planted for the port under the other name, while both take their locks on the same file. (os_unix.c's
`verifyDbFile` logs a warning for a database with more than one link, and goes on.) The test runs `ln` through
`Process.run`, which Windows does not have and whose failure is a status code, not an `IoError`. `File` has `rename`
(ADR 0075 §2.1) and `remove`, not `link`.

## 2. Decision

### 2.1 The API (lib/std, hosted)

```ceesafe
File.link(existing: StringView, new: StringView) -> Result<void, IoError>   // HostServices.link
```

- Makes `new` a second name of the file named `existing`: POSIX `link(2)`; Windows
  `CreateHardLinkW(new, existing, NULL)`. Both names then open the same file: a write through one is read through
  the other, and removing one leaves the other.
- **`new` is never replaced**: if it exists (file, directory or link) the call is `AlreadyExists`, unlike `rename`,
  which replaces. Nothing is created on any error.
- Errors, the same on every host:
  - `NotFound`: `existing` does not exist, or a directory on either path does not;
  - `AlreadyExists`: `new` exists;
  - `IsDirectory`: `existing` is a directory (POSIX says `EPERM`, Windows `ERROR_ACCESS_DENIED`: the runtime asks
    the path's kind after the failure, so a program sees one answer);
  - `PermissionDenied`: the directory of `new` may not be written, or the file system refuses for another reason
    of permission;
  - `Unsupported`: the file system has no hard links where the system says so (Windows `ERROR_INVALID_FUNCTION` or
    `ERROR_NOT_SUPPORTED` on FAT and exFAT; POSIX `EOPNOTSUPP`/`ENOTSUP`). A POSIX file system that answers `EPERM`
    instead (Linux's vfat) is `PermissionDenied`;
  - `Other`: the names are on different volumes (`EXDEV`, `ERROR_NOT_SAME_DEVICE`) or the file has too many links
    (`EMLINK`, `ERROR_TOO_MANY_LINKS`), which have no `IoError` of their own;
  - `InvalidInput`: a path that holds a NUL or is 4096 bytes or longer, as for every path (an empty path names
    nothing: `NotFound`).
- The new name is durable once its directory is synced (`Directory.sync`), as for `rename`.
- Effects: `sleep`, as for every call on a path. In freestanding code it is N0293, as `File` is.
- No symbolic link is added: the finding asks for a hard link only, and a symbolic link has another set of
  questions (a relative target, Windows' privilege for `CreateSymbolicLinkW`).

### 2.2 The runtime

`ceesafe_host_link(existing, existingLength, new, newLength) -> int64_t`, 0 or a negated errno, beside
`ceesafe_host_rename` in runtime/hosted/ceesafe_rt_io.c. Paths are checked and converted as `rename`'s are (UTF-8;
UTF-16 on Windows through `cee_win_path`). After `EPERM` or `EACCES` (Windows: `ERROR_ACCESS_DENIED`), a `stat` of
`existing` that finds a directory makes the answer `EISDIR`; Windows' `ERROR_INVALID_FUNCTION` and
`ERROR_NOT_SUPPORTED` are `EOPNOTSUPP`, `ERROR_TOO_MANY_LINKS` is `EMLINK`, and the rest go through `cee_win_errno`.

### 2.3 The interpreter

The interpreter is built by the release stage 0 (0.94.5, Decision 0009), whose library has no `File.link`, and the
runtime's `ceesafe_host_link` is not a name a program may declare (N0292, type-system.md §28.2), so it links through
the system's own calls, in `extern "C"` blocks configured by target (§28.4): `link` on Unix hosts, `CreateHardLinkW`
and `GetLastError` on Windows. The link it makes is the system's, as a native program's. Its errors:

- first from the paths, with the stage-0 library's `File.kind`, in the runtime's terms: `existing` missing is
  `NotFound`, a directory `IsDirectory`; `new` present is `AlreadyExists`;
- then from the call: on Windows its error code, mapped as the runtime maps it; on Unix, where `errno` is not the
  interpreter's to read (it is a macro over a function of a reserved name), from the paths again: a missing parent
  of `new`, or an empty `new`, is `NotFound`, a parent that is not a directory `NotDirectory`, and anything else
  `PermissionDenied`. So a link across volumes or past the link limit is `Other` natively and `PermissionDenied` in
  the interpreter; the cases a program can foresee answer the same.

On another host it is `Unsupported`. Its sandbox refuses it, as every path service. Nothing about the interpreter's
model of files changes: it reopens a file by its path for each positional operation (§26.3), so a file opened
through one name and written through the other is seen through both. When the release stage 0 has `File.link`, the
interpreter calls it instead.

**A known difference, accepted by the owner (2026-11-25).** Until a release stage 0 has `File.link` (a release after
0.95.13 moved into `bootstrap/reference.md`), a POSIX link that fails for a reason the paths do not show is
`PermissionDenied` in the interpreter where a native program says `Other` (`EXDEV` across volumes, `EMLINK` past
the link limit) or `Unsupported` (a file system without hard links that says `ENOTSUP`). Every error a program can
foresee from its paths (a missing file or directory, an existing `new`, a directory, an empty path) is the same
natively and interpreted, and the corpus tests only those. Windows' interpreted errors are the system's code, mapped
as the runtime maps it, so they do not differ.

## 3. Options not taken

- **Copying the file** in the interpreter, as its `rename` is a copy and a removal: a copy is not a second name (a
  write through one would not be seen through the other), and the only users of a hard link need exactly that.
- **`Unsupported` in the interpreter** until stage 0 has `File.link`, as for `File.setPermissions` (ADR 0077): the
  system's call is one line per host, and a test of the port runs in the interpreter too.
- **A feature macro in rt.h** (`#if CEESAFE_HOST_LINK`) so that the interpreter calls the runtime's function when
  the compiler that built it has one: tried, and refused by the checker, since `ceesafe_` names are the runtime's.
- **Replacing `new`** as `rename` does: `link(2)` and `CreateHardLinkW` both refuse, and a replace could not be
  atomic on Windows.
- **`File.symlink`**: not asked for (§2.1).
- **A method on an open `File`** (`f.link(new)`, Linux's `linkat` with `AT_EMPTY_PATH`): not portable (macOS and
  Windows have no such call), and SQLite links by name.

## 4. Tests

- `tests/reference/tests/corpus/run-pass/file-link.cee`, the same output natively, in the interpreter and with
  `--check-leaks`: a file linked under a second name reads the same through both; a write through one name is read
  through the other; removing the first name leaves the second's data; `AlreadyExists` when `new` exists (and its
  content is unchanged), `NotFound` for a missing `existing` and for a missing directory of `new`, `IsDirectory`
  for a directory, `NotFound` for an empty path.
- `tools/file_lock_check.py` part (4), natively and in the interpreter: Python's `os.stat` of both names reports the
  same `st_ino` and `st_nlink` 2 after the program's link (an independent check that it is a link, not a copy), and
  `st_nlink` 1 after one name is removed; and the other way, a link made by Python's `os.link` is read by the program
  through both names. The program also opens the database through the second name and its lock on it conflicts with
  a lock through the first (the finding's use).
