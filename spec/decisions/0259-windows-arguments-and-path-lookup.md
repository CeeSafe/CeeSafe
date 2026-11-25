# ADR 0259 — Windows: a program's arguments from its command line, and a program found on `PATH` only

- Status: **Proposed — implemented** 2026-11-25 on the branch `fix/g5-os` (from 0.95.10), at the owner's decisions on
  ADR 0245 §5's questions 2 and 3 ("On Windows a program reads its arguments with `GetCommandLineW` … and gets them
  as OS strings, with UTF-8 text when valid"; "A bare program name is looked up on PATH only (and the system
  directories, as Rust does), never the current directory"). Question 1 is answered "batch files stay refused": no
  change. **Amends ADR 0245** §2.2 and §2.3. §6's questions are the owner's.
- Scope: `runtime/hosted/ceesafe_rt_io.c` (`ceesafe_args_set` on Windows, `cee_win_split_command_line`,
  `cee_win_find_program`, and the four calls that start a program); type-system.md §26.1, §26.5;
  `tools/process_check.py`.

## 1. Context

- **Arguments.** A CeeSafe program's `main` is C's `main(int argc, char **argv)`. On Windows the C runtime makes
  `argv` from the command line in the ANSI code page: a character the code page lacks becomes `?`, and since the code
  page is rarely UTF-8, `Process.argument` was `null` for any non-ASCII argument. A file name given on the command
  line could not be opened.
- **Lookup.** `CreateProcessW(NULL, line, …)` looks for a program named without a directory in the program's own
  directory, **the current directory**, the system directories, and then `PATH`. A file planted in the current
  directory (`git.exe` in a cloned repository) runs instead of the one on `PATH`: the reason Rust (1.58, 2022),
  Go (1.19, `exec.LookPath`) and others stopped searching it.

## 2. Decision

### 2.1 Arguments

On Windows the runtime, at start (`ceesafe_args_set`, before `main`), reads `GetCommandLineW()` and splits it by
`CommandLineToArgvW`'s rules, which `Process.run`, `capture` and `spawn` quote for (§26.5):

- **The program name** (argument 0) is read without escapes: from a `"` to the next `"`, else to the first space or
  tab.
- **Each argument after it**, past the spaces and tabs before it: `2n` backslashes before a `"` are `n` backslashes,
  and the `"` opens or closes a quoted part; `2n + 1` backslashes before a `"` are `n` backslashes and a literal `"`;
  backslashes before anything else are themselves. Outside a quoted part a space or tab ends the argument. Inside one,
  `""` is a literal `"` and the quoted part goes on (`CommandLineToArgvW`'s rule since Windows 7, checked against it by
  `tools/process_check.py`).
- An empty command line is one argument, the program's path (`GetModuleFileNameW`), as `CommandLineToArgvW` gives.

Each argument's UTF-16 units become WTF-8 (ADR 0257 §2.1), kept for the process's life.
`Process.argument(i)` is the argument as text when it is UTF-8, else `null`, as on POSIX; `Process.argumentOs(i)` is
its bytes, so an argument with an unpaired surrogate arrives too. `Process.argumentCount()` counts them. The C
runtime's `argv` is not read, and a program built as a windowed one (ADR 0151) gets its arguments the same way.

### 2.2 Finding a program on Windows

`Process.run`, `capture`, `captureWith`, `runWith`, `spawn` and `spawnWith` find the program themselves and give
`CreateProcessW` its full path (`lpApplicationName`), so Windows' own search is never made:

1. **A name with a directory** (a `\`, a `/` or a `:` in it: `.\tool`, `bin\tool`, `C:\x\tool.exe`) is used as it is,
   relative to the working directory when it is relative.
2. **A bare name** is looked for, in this order, in:
   1. the system directory (`GetSystemDirectoryW`: `C:\Windows\System32`);
   2. the Windows directory (`GetWindowsDirectoryW`: `C:\Windows`);
   3. each directory of this process's `PATH`, in order, an empty entry skipped and the `"`s around an entry's text
      removed (`C:\a;"C:\b;c";D:\`).

   The first that holds a file of that name (not a directory) is the program. **The working directory and the
   program's own directory are not searched**: `.\tool` runs a tool there. A `PATH` entry that is relative (`.`) is
   relative to the working directory, as it is written.
3. In both, a name whose last component has no extension (no `.` after its last `\` or `/`) has `.exe` added, as
   `CreateProcessW` does; one with an extension is used as it is. `PATHEXT` is not read.
4. Nothing found is `NotFound`. A batch file is refused (`InvalidInput`), by the name given and by the name found
   (ADR 0245 §2.4).

The command line's first word is the program as the caller wrote it (quoted as §26.5 says), so the child's
argument 0 is what it was.

### 2.3 POSIX

Unchanged: `posix_spawnp` searches the parent's `PATH`, and a name with a `/` is used as it is. A bare name is never
looked for in the working directory unless `PATH` has an empty entry or `.`, which POSIX defines to mean it.

## 3. Alternatives

- **Call `CommandLineToArgvW` itself.** It is shell32's, a library a console program does not otherwise load, and the
  splitter is twenty lines whose rules §2.1 states; the check compares the two on the same lines.
- **Keep the application's directory in the search** (Rust does, for compatibility). The owner's decision names the
  system directories only.
- **Search `PATH` before the system directories.** Rust searches the system directories first, so a `PATH` entry
  cannot replace `cmd.exe` or `where.exe`.

## 4. Consequences

- A program on Windows receives non-ASCII arguments and file names, and names with unpaired surrogates through
  `argumentOs`.
- A program that relied on Windows starting a tool from the working directory names it `.\tool`.
- `process_check.py`'s CeeSafe child now receives every argument the Python script does, non-ASCII ones included.

## 5. Tests

`tools/process_check.py`: the CeeSafe child is given the non-ASCII arguments too, and on Windows a lone surrogate
(through Python's `subprocess` with a `str` holding `\udc80`), compared in hex; the splitter is run on 40 command
lines (quotes, `""` inside quotes, backslashes before quotes and at the end, tabs, an empty line) against
`CommandLineToArgvW` through `ctypes`. On Windows a `cmd.exe`-named program planted in a temporary working directory
must not run (`cmd /d /c exit 3` still exits 3 from System32), and a planted `tool.exe` runs as `.\tool` and is
`NotFound` as `tool`.

## 6. Questions for the owner

1. The program's own directory is not searched (Rust searches it second). Keep it out?
2. `PATH` is the parent's even when ADR 0258's options change the child's (Rust: the child's first).
