# ADR 0077 — `Console.flush()`, `Directory.space(path)`, and what diskio needs: `IoError.StorageFull`, `File.openAppend`, `File.modified`, `File.setPermissions`

- Status: **Accepted** by the owner, 2026-11-06 ("While we wait, fix CeeSafe gap you found"). Raised by the CosmoStore
  port (`CeeSafeApps/cosmostore/CEESAFE_GAPS.md` G24 and G25).
- Scope: type-system.md §26.3 and §26.10, lib/std `host.cee`, the hosted runtime, the checker's `Console` members, the
  interpreter, guide/builtins.md, the corpus.

## 1. Context

- **G25.** Standard output is buffered by the C library when it is not a terminal. A program killed with SIGKILL
  loses what it wrote last, and CeeSafe has no way to flush except writing to standard error, which flushes standard
  output first. CosmoStore's soak test lost every line of its killed runs until it moved them to standard error.
- **G24.** A storage engine must stop writing before the volume is full. CeeSafe cannot ask a volume how much room it
  has (`statvfs`, `GetDiskFreeSpaceExW`), so CosmoStore's ADR-0006 takes a configured capacity instead.

## 2. Decision

1. **`Console.flush()`** writes what standard output holds buffered and returns `void`.
   - It is hosted only: in freestanding code it is N0293, as `Process.capture` is.
   - It is lib/std's `HostServices.flush`, over the runtime's `ceesafe_host_console_flush` (`fflush(stdout)`). The checker
     routes it as it routes `File.kind`.
   - Standard error is unbuffered and needs no flush. `AsyncConsole` writes to the descriptor directly, so it needs
     none either.
   - The interpreter writes its buffered output.
2. **`Directory.space(path)`**, in lib/std, returns `Result<VolumeSpace, IoError>`. `VolumeSpace { total, free,
   available }` gives the volume's bytes for the file system that holds `path`:
   - `total` is its size;
   - `free` is what is unused;
   - `available` is what an unprivileged process may use. It is at most `free`, since root may have a reserve.

   On POSIX it is `statvfs`, with fragment size × `f_blocks`, `f_bfree` and `f_bavail`. On Windows it is
   `GetDiskFreeSpaceExA` (available, total, free). A missing path is `NotFound`. The interpreter has no volume of its
   own to describe and answers `IoError.Unsupported`, so that a run stays reproducible.

## 2a. Amendment (2026-11-06): what diskio needs

The owner then asked for the mail server's `diskio` package (CeeSafeLibs) to leave C. Its session said that diskio
"stays in C because CeeSafe lacks a portable errno accessor and packages can't pick sources per OS". Neither is
needed once lib/std covers diskio's calls, because lib/std is portable already. What lib/std still lacked:

3. **`IoError.StorageFull`**, "No space left on device", appended after `HostNotFound` so earlier variants keep
   their tags. It covers `ENOSPC`, `EDQUOT` where the system has it, and Windows' `ERROR_DISK_FULL` and
   `ERROR_HANDLE_DISK_FULL`. diskio's `FileError.NoSpace` needed a portable errno for this.
   - **Source change:** a `match` that names the first twenty variants and has no `_` no longer covers `IoError`
     (N0256), as with ADR 0044 and ADR 0060.
4. **`File.openAppend(path)`**: write-only, created if missing (mode 0666 & ~umask), every write at the end
   (`O_APPEND`; `_O_APPEND` on Windows). This is mode 5 of `ceesafe_file_open`. The interpreter's model writes at
   the file's end each time.
5. **`File.modified(path)`**: `Result<UnixTime, IoError>`, the last modification time with nanoseconds where the
   system keeps them (`stat`; `_stat64` on Windows).
6. **`File.setPermissions(path, mode)`**: `chmod` of the low 12 bits. On Windows it sets the read-only attribute when
   the owner may not write, and does nothing else. It is how a mail store keeps its files 0640 and its directories
   0750 under any umask. It is called on the new, still empty file, so the file never holds data others could read.
   A process-wide umask setter was rejected: diskio's calls run on `Async.blocking` worker threads, and the umask
   would race.
7. The interpreter answers `Unsupported` for 5 and 6, since the stage-0 compiler's library can do neither.

Tested: a 2 MB disk image filled with `writeAt` reports `IoError.StorageFull`, and `Directory.space` then says the
volume is nearly full. A file given 0o640 shows mode 640 (`stat`). Appends from two openings land in order.

## 3. Tests

- run-pass `console-flush` (output written before a flush) and `volume-space` (`available <= free <= total`, a missing
  path is `NotFound`).
- A check by hand that a flushed line survives SIGKILL with standard output redirected.
