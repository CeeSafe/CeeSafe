# ADR 0221 — Creating a file for reading and writing, the wrong-mode error, and whether a stream is a terminal

- Status: **Proposed**, 2026-11-24, and implemented on the branch `fix/findings-os` (not merged, not released) at the
  owner's request to close the SQLite port's findings 27 ("A file cannot be created for reading and writing") and 29
  ("No way to tell whether standard input is a terminal"). The names in §2 are this ADR's.
- Scope: type-system.md §26.2 (`IoError`) and §26.3 (files and the console); the runtime; the interpreter.

## 1. Context

**Finding 27.** The port's WAL writer needed `O_RDWR | O_CREAT`. That open exists since ADR 0075 §2.1 as
`File.openOrCreate(path)` (with `File.createNew(path)` for `O_EXCL` and `File.openReadWrite(path)` for an existing
file), but the port read only `File.create` (write-only, truncated) and `File.openReadWrite` (fails when missing),
then created, dropped and reopened the file. The finding asked for an open-options builder (read, write, create,
truncate, exclusive). Reading from `File.create`'s write-only handle failed with "Input/output error": the runtime
maps `EBADF` to `IoError.Other(9)`, whose message is `EIO`'s, so nothing said the handle had the wrong mode.

The opens there are, with their flags:

| Function | Flags |
|---|---|
| `File.open` | `O_RDONLY` |
| `File.openRegular` | `O_RDONLY`, a regular file only |
| `File.create` | `O_WRONLY \| O_CREAT \| O_TRUNC` |
| `File.openAppend` | `O_WRONLY \| O_CREAT \| O_APPEND` |
| `File.openReadWrite` | `O_RDWR` |
| `File.openOrCreate` | `O_RDWR \| O_CREAT` |
| `File.createNew` | `O_RDWR \| O_CREAT \| O_EXCL` |

The one combination a program that reads back what it writes is missing is a fresh, empty file for reading and
writing whether or not one was there: `O_RDWR | O_CREAT | O_TRUNC` (a journal or log reset; SQLite's rollback
journal is opened so).

**Finding 29.** A shell prompts and prints a banner when a person types at it and reads a script silently from a
pipe or a file; without `isatty` the port's `cli/` always reads silently.

## 2. Decision

### 2.1 `File.createReadWrite(path)`

`Result<File, IoError>`: the file created if missing, emptied if present, open for reading and writing
(`O_RDWR | O_CREAT | O_TRUNC`, mode 0666 & ~umask; `_O_RDWR | _O_CREAT | _O_TRUNC` on Windows). It completes the
table above: every open with read and write access now has a name, and each name says what happens to a file that is
there and one that is not. The interpreter checks and empties the file here and reopens it for each operation, as
for the other read-write opens (§26.3).

### 2.2 `IoError.BadDescriptor`

A new variant after `TooManyOpenFiles` (so the others keep their tags, as ADR 0044, 0060, 0077 and 0104 did):
`EBADF`, Windows' `ERROR_INVALID_HANDLE` (which the runtime already maps to `EBADF`). Its message is glibc's,
"Bad file descriptor". It is what reading a `File` opened only for writing (`File.create`, `File.openAppend`) or
writing one opened only for reading (`File.open`) gives, on every host and in the interpreter; and §26.3's lock of
the wrong kind (ADR 0220 §2.1). §26.2 says so, so the message is found where the error is.

A `match` that names the first twenty-two variants and has no `_` arm no longer covers `IoError` (N0256): a source
change, as for the variants before it.

### 2.3 `Console.isTerminal(stream)`

`Console.isTerminal(stream: ConsoleStream) -> bool`, with `public enum ConsoleStream { Input, Output, Error }` in
lib/std (hosted; lib/std's `HostServices.isTerminal`, as `Console.flush` is `HostServices.flush`):

- POSIX: `isatty` on descriptor 0, 1 or 2.
- Windows: `GetConsoleMode` on the stream's handle succeeds. Unlike the C runtime's `_isatty`, which is true for any
  character device, this is false for `NUL` and serial ports, which nobody types at.
- It has no effect and cannot fail: a closed or redirected stream is `false`.
- The interpreter answers for its own streams (`isatty`); when it runs with its output captured or its input given
  (`ceesafe_run`, the corpus runner), it answers `false` for every stream, as a native program run under a pipe does.

## 3. Options not taken

- **An open-options builder** (`File.options().read().write().create().open(path)`). It would be a second way to
  spell the seven opens above, and the combinations that have no name (write-only without truncation, `O_EXCL`
  without read access, truncation without creation) are ones nothing has asked for. `File` is a built-in type whose
  opens are each one runtime mode; a builder would need the checker to read a library struct's fields for the mode
  word. If a program needs one of those, a named open can be added as this one was.
- **`Other(9)` with a better message.** `Other`'s message is fixed ("Input/output error") for every number, so a
  program that prints `e.message()` would still see it. A variant also lets a program match the case.
- **Three functions** (`Console.inputIsTerminal()` …). One function and an enum keep the three in one place in the
  documentation.

## 4. Tests

- `tests/reference/tests/corpus/run-pass/file-create-read-write.cee`: `createReadWrite` creates, empties an existing
  file, reads back what it wrote; `File.create`'s handle read is `BadDescriptor` with its message; `File.open`'s
  handle written is `BadDescriptor`. The same natively and in the interpreter.
- `tests/reference/tests/corpus/run-pass/console-is-terminal.cee`: under the corpus runner every stream is a pipe:
  `false` three times, natively and in the interpreter.
- `tools/terminal_check.py` (POSIX, native and interpreted): under a pseudo-terminal (Python's `pty`) each stream
  that is the terminal is `true`, and a redirected one `false`.
