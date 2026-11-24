# ADR 0245 — Windows command lines, captured input, and batch files

- Status: **Proposed — implemented** 2026-11-24 on the branch `fix/windows-processes`, for 0.95.8 (after another
  session's 0.95.7), found by the SQLite port (its CEESAFE_FINDINGS.md, findings 45 and 46). Its questions for the
  owner are §5.
- Scope: `runtime/hosted/ceesafe_rt_io.c` (`ceesafe_process_run`, `ceesafe_host_spawn`,
  `ceesafe_host_child_start_with`, and the shared `cee_win_command_line`, `ceesafe_host_quote`,
  `ceesafe_host_quote_program`, `cee_win_batch`), `lib/std/host.cee` (`HostServices.captureIn`),
  `src/host/toolchain.cee` (`runThroughBatch`), type-system.md §26.5, §26.7, §26.16, c-backend.md (Windows),
  testing.md, `tools/process_check.py` (new; contract step `processes`, and windows.yml).

## 1. Context

- **`Process.capture` failed for every program on Windows.** `captureIn` created the input file with `File.create`
  and kept that handle, which writes, open while `ceesafe_host_spawn` opened the same file with
  `CreateFileW(…, GENERIC_READ, FILE_SHARE_READ, …)`. An open that does not share with writers fails while a handle
  that writes is open: a sharing violation. ceesafec never saw it, since on Windows it runs its tools through a
  batch file (`runThroughBatch`).
- **`Process.run` split arguments on Windows.** `ceesafe_process_run` gave the arguments to `_wspawnvp`, which joins
  them with spaces into one command line without quoting; the child splits that line again, so
  `Process.run("sqlite3", [path, "CREATE TABLE t(a, b)"])` gave sqlite3 four arguments, and a `"` was lost.
  `Process.spawn` and `capture` already quoted for `CommandLineToArgvW` (`ceesafe_host_quote`).
- **Batch files.** `CreateProcessW` runs a `.bat` or `.cmd` file through cmd.exe, which splits the line by its own
  rules and expands `%name%`, so no quoting for `CommandLineToArgvW` keeps an argument from being read as commands:
  BatBadBut (CVE-2024-24576; Rust, Node, Python and others fixed or documented it in 2024). `_wspawnvp` also ran
  batch files, found on `PATH` with `.bat` and `.cmd` added, without quoting at all.

## 2. Decision

1. **capture:** lib/std writes the input file and closes it (a block drops it) before the child is started, and
   the runtime opens it for the child sharing with readers and writers. The output and error files are created by
   the runtime for the child (`CREATE_ALWAYS`, shared with readers) and nothing else holds them; lib/std reads them
   after the child's handles are closed. POSIX is unchanged (closing first changes nothing there).
2. **One command line for all four** (`run`, `capture`, `spawn`, `spawnWith`), built by `cee_win_command_line`:
   - the program is the line's first word, read by `CreateProcessW` and `CommandLineToArgvW` without escapes; it is
     quoted as it is when empty or with a space or tab, and a `"` in it, which no Windows file name has, is
     `InvalidInput`;
   - an argument is written as it is when it is not empty and has no space, tab or `"`; otherwise in quotes, a `"`
     as `\"`, the backslashes before a `"` or the closing quote doubled, other backslashes as they are. The line
     never holds `""` inside quotes, where the UCRT's parser and `CommandLineToArgvW` differ;
   - more than 32,767 units is `E2BIG` (`IoError.Other`).
3. **`Process.run` uses `CreateProcessW`** with this process's standard handles (`STARTF_USESTDHANDLES`,
   inherited), no new process group, and waits. Its program is found as `spawn`'s and `capture`'s are
   (`CreateProcessW`'s search, `.exe` added to a name without an extension) rather than `_wspawnvp`'s.
4. **Batch files are refused** with `InvalidInput`, before anything is started, by all four: a program whose name's
   last component ends in `.bat` or `.cmd`, in any case, where each part between colons (a drive, an alternate
   stream) is tested, without the dots and spaces Windows drops from the end of a name (`T.CMD`, `t.bat.`,
   `t.cmd. .`, `t.bat::$DATA`, `C:x.bat`). A name without an extension cannot be one: `CreateProcessW` adds `.exe`.
   A program that means to run a batch file runs `cmd.exe` itself and quotes for cmd.exe itself; ceesafec's
   `runThroughBatch` does so (its script path is now passed unquoted, as one argument the runtime quotes).

## 3. Alternatives

- **Escape for cmd.exe instead of refusing** (Rust's choice: `^`-escapes and `%%cd:~,%` for `%`). The rules are
  cmd.exe's undocumented parser, and some texts (a newline) cannot be passed at all; Rust refuses those. Refusing
  everything is smaller and cannot be wrong; escaping can come later if a program needs it.
- **Keep `_wspawnvp` and quote each argument first.** It would keep `_wspawnvp`'s `PATH` search with `.bat` and
  `.cmd`, which is the batch problem again, and two lookups for one family of calls.

## 4. Tests

`tools/process_check.py`: a Python script and the program itself receive 22 arguments (spaces, quotes, backslashes
before quotes and at the end, empty arguments, tabs, newlines, `%PATH%`, `^&|<>()!`, non-ASCII text, which only the
script gets) through `run`, `capture` and `spawn`, each writing what it received in hex, compared with the list the
check made; `capture`'s input is every byte value, `\r\n`, `0x1A` and a zero byte included. A missing program is
`NotFound`. On Windows the six batch spellings above are refused by all three, and `cmd.exe /d /c "exit 3"` runs.

## 5. Questions for the owner

1. Refuse batch files (this ADR), or escape for cmd.exe as Rust does?
2. A CeeSafe program's own arguments on Windows come from `main`'s `char **argv`, in the ANSI code page, so
   non-ASCII arguments are lost or refused (`Process.argument` is `null` for bytes that are not UTF-8). The check
   gives the CeeSafe child ASCII arguments only. Should the runtime take `GetCommandLineW` and split it
   (`CommandLineToArgvW`) into UTF-8?
3. `CreateProcessW`'s search looks in the working directory before `PATH`, as `spawn` and `capture` already did;
   `_wspawnvp` did too. Should a program named without a directory be looked up on `PATH` only?
