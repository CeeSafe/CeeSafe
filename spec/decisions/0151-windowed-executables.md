# ADR 0151 — Windowed executables: no console window on Windows

- Status: **Accepted** by the owner, 2026-11-15. ClayWind's finding M2 (`~/dev/CeeSafeApps/Clay/CEESAFE-FINDINGS.md`
  §M): the owner, running ClayWind's desktop host on the x64 Windows laptop, reported "On Windows a terminal window
  opens beneath the app", and asked for fixes at the source. The design below was the lead's, chosen as defaults, and
  the questions it settles are listed in §5 so that the owner can revisit them. Implemented on branch
  `adr/0151-windowed-executables`, released later.
- Scope: the manifest (`src/packages/manifest.cee`, tooling.md §4.1), the command line (`src/main.cee`, tooling.md
  §12.1), the link (`src/host/toolchain.cee`, `src/host/compilers.cee`, tooling.md §8.3), the generated `main`
  (`src/backend/c_interop.cee`, c-backend.md §6), the hosted runtime (`runtime/core/ceesafe_rt_core.c`,
  `runtime/hosted/ceesafe_rt_hosted.c`, `runtime/include/ceesafe/rt.h`), type-system.md §26.3, the book's appendix E
  and §14.1, `tools/package_tests.py`. No language, checker or interpreter change, and no new diagnostic.

## 1. Context

- **Windows has two kinds of executable.** A PE file's header names its subsystem. A *console* program
  (`IMAGE_SUBSYSTEM_WINDOWS_CUI`) is given a console when it starts: its parent's, if the parent has one, or a new
  one, which Windows shows as a terminal window. A *GUI* program (`IMAGE_SUBSYSTEM_WINDOWS_GUI`) is given none: its
  standard handles are whatever its parent passed (nothing, when Explorer starts it), and a terminal that starts it
  does not wait for it.
- **Every executable `ceesafec build` makes is a console program** (the linkers' default, with `main` the entry). A
  program with a `Window` (type-system.md §26.13) started from Explorer, a shortcut, a scheduled task or another GUI
  program shows a terminal window beneath its window, which closes the program when it is closed. ClayWind's desktop
  host is such a program, and so is every CeeSafe program with a window.
- **Nothing can ask for the other kind.** The manifest (tooling.md §4.1) has no field for it and `ceesafec build` no
  option; `--cflag -mwindows` reaches the compile but not every link (`--split`), is gcc's spelling only, and leaves
  the program with no standard streams at all (below).
- **A GUI program's standard streams.** With no handles from its parent, the C runtime's `stdout`, `stderr` and
  `stdin` have no descriptor (`_fileno` is -2 in the UCRT): a write fails, `fflush` fails, and `_setmode` on the
  descriptor, which the runtime calls before its first write, is an invalid parameter (a crash under the UCRT's
  default handler, which clang and `cl` builds have; MinGW's returns -1). On the laptop (WinLibs gcc 16, UCRT), a
  `-mwindows` program started by a scheduled task: `fwrite` to `stdout` 12, `fflush` -1, `_write(1)` -1, `stderr`'s
  `fwrite` 0, `_read(0)` -1. Started from cmd with its output redirected (`app > log.txt`, or a parent's pipes), the
  handles are valid and everything works.
- **macOS, Linux and FreeBSD have one kind.** A program with a window there is an ordinary executable; a terminal appears
  only when the user starts it from one. (A macOS application bundle, `.app`, is another question: §6.)

## 2. Decision

### 2.1 The manifest field and the option

```toml
[package]
name = "desktop"
windowed = true              # on Windows, no console window (ADR 0151); nothing changes elsewhere
```

- **`windowed`** is a boolean in `[package]`, default `false`. `true` says that the executable is a windowed program:
  on Windows it is linked for the Windows subsystem (§2.2) and starts as §2.3 says. On macOS, Linux and FreeBSD
  (ADR 0149) it changes nothing: the executable is the same as without it.
- **Only an executable of the hosted profile.** `windowed = true` with `kind = "library"` or `"object"`, or with
  `profile = "freestanding"`, is N0321 at the value: "'windowed' is for an executable of the hosted profile: a library,
  an object or a freestanding program is linked by its host (ADR 0151)". A value that is not a boolean is N0321
  ("'windowed' is true or false"). `windowed = false` is allowed everywhere.
- **Only `[package]`.** A `[target.OS]` table keeps its three link fields (ADR 0047): `windowed` there is N0321, as
  any other field is. Since the field does nothing on macOS, Linux and FreeBSD, a per-system place would add nothing.
- **`--windowed`** (`build`, and `test`, which accepts `build`'s options) is the same for one build, as `--no-float`
  is `float = false`. It is a usage error (status 2) with `--profile freestanding` (given, or the manifest's) and with
  `--emit obj` or `--emit lib`: "--windowed is for an executable of the hosted profile: an object or a library is
  linked by its host (ADR 0151)".
- **Where the field applies.** The manifest's `windowed = true` applies to `build --emit exe` (the default) and
  `build --emit c`. It does nothing for `--emit obj` and `--emit lib` asked on the command line (a host program's
  link chooses its subsystem), for `--profile freestanding` given on the command line, for `test` (the test program is
  a console program, whose output ceesafec reads) and for `run --interp` and `check`, which link nothing. Doing
  nothing there is not reported: the field describes the package's executable, and those commands do not make it.
- **`--emit c`** writes the same C as an executable's build (the generated `main` calls the windowed start, §2.3),
  and whoever links it passes the subsystem flags of §2.2.

### 2.2 The link on Windows

`main` stays the entry, so the generated C `main` is unchanged and no `WinMain` is written:

| Compiler | Flags, after the stack's |
|---|---|
| gcc (MinGW-w64) | `-mwindows`: the GUI subsystem; MinGW-w64's start code calls `main` in a GUI program as in a console one |
| clang (MSVC target) | `-Wl,/SUBSYSTEM:WINDOWS -Wl,/ENTRY:mainCRTStartup` |
| `cl` | `/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup`, after `/link` |

`/ENTRY:mainCRTStartup` keeps the C runtime's console start code, which calls `main`; without it the MSVC linker
looks for `WinMain` in a GUI program. Both link paths (one compile, and `--split`'s link) pass them.

### 2.3 The start: the standard streams of a windowed program

The generated `main` of a windowed build calls `ceesafe_rt_windowed()` (`rt.h`, an internal entry point like
`ceesafe_rt_init_main`, not in `embed.h`) before `ceesafe_rt_init_main`, so before anything is written. Elsewhere it
does nothing. On Windows, for standard input (0), output (1) and error (2):
1. **A stream whose handle is valid is kept** (`GetStdHandle` neither NULL nor `INVALID_HANDLE_VALUE`, and
   `GetFileType` not `FILE_TYPE_UNKNOWN`): output redirected to a file (`app > log.txt`), a parent's pipes (ssh, a
   build tool, `Process.run`) and a console handle the parent passed all keep working. When all three are valid,
   nothing else is done.
2. **Otherwise the program attaches to its parent's console**, if it has one: `AttachConsole(ATTACH_PARENT_PROCESS)`,
   once. A stream that was valid gets its handle back (`SetStdHandle`), in case attaching replaced it.
3. **Each stream that had no handle is opened**: on the console, `CONOUT$` for output and error and `CONIN$` for
   input, when attaching succeeded; on `NUL` otherwise. The C runtime's stream is reopened (`freopen`), its
   descriptor made 0, 1 or 2 again when the reopening gave another (`_dup2`), and the system's standard handle set to
   it (`SetStdHandle`), so `Console`, `AsyncConsole`, the descriptor-level reads and a child's inherited handles all
   see it. A step that fails leaves that stream as it was; the start never fails the program.

So, on Windows, a windowed program:
- **started from Explorer, a shortcut or a scheduled task** has no console and no terminal window. Its standard output
  and error go nowhere and its standard input is empty: `Console.write` and its kin succeed, `Console.flush`
  writes what is buffered to `NUL`, `Console.read` is 0 and `Console.readLine` `Ok(false)`. A panic's message goes nowhere too; the exit status
  (134) is still the process's.
- **started from a terminal** (cmd, PowerShell, Windows Terminal) writes there and reads the terminal's input. The
  terminal does not wait for it, as for any GUI program: cmd's prompt comes back at once, and the program's output
  appears as it is written (`start /wait`, `cmd /c`, a batch file and PowerShell's `Start-Process -Wait` wait). Being
  one of the console's processes, it gets that console's Ctrl+C (`ProcessSignals`, type-system.md §26.15).
- **started with its streams redirected** writes and reads them as a console program does.

## 3. Alternatives considered

- **`WinMain` as the entry.** The generated C would need a second entry and `__argc`/`__wargv` for the arguments,
  and gains nothing over keeping `main` with `/ENTRY:mainCRTStartup` (MSVC, clang) and MinGW's start code.
- **A console program that hides or frees its console at start** (`FreeConsole`, `ShowWindow(GetConsoleWindow(),
  SW_HIDE)`): the terminal window still flashes when the program starts, and a program started from a terminal would
  close or hide that terminal's window.
- **Every program with a `Window` is windowed, automatically.** The compiler cannot tell a desktop program from a
  command-line tool that sometimes opens a window (a viewer called from a script, `window_check.py`'s programs, which
  print their results), and the second must stay a console program. It is the package's choice.
- **An attribute on `main`** (`@windowed fn main`): how a program is linked is the build's setting, like `profile`
  and `kind`, not the source's; a test build of the same package must stay a console program.
- **`subsystem = "windows"`, `gui = true` or `console = false`.** `subsystem` names Windows' mechanism in a field
  meant for every system; `gui` is jargon; `console = false` reads as "no standard output", which is not so from a
  terminal. `windowed` says what the program is.
- **Leave the streams without handles** (no `NUL`): every write would fail and `_setmode` would crash a clang or `cl`
  build at its first write (§1). A program must not need to know how it was started to print.
- **Standard input from `NUL` even when attached:** an interactive cmd does not wait for the program and reads the
  same console, so input typed there goes to one or the other. But a parent that waits (`cmd /c`, `start /wait`, a
  batch file) expects the program to read the terminal, as the lead's design said; kept as `CONIN$`.
- **A message box for a panic with no console:** a window the program did not ask for, at a panic, on one system
  only. Not now (§5).

## 4. Consequences

- A package with a window adds `windowed = true`, and on Windows it starts as a desktop program does. Its output still
  reaches a terminal that starts it, a redirected file and a parent's pipes.
- A compiler older than this one reports `windowed` as an unknown field (N0321), so a package that uses it should say
  so with `ceesafec = ">=…"` (ADR 0076), the release that brings it.
- The C of a windowed build differs from a console build's by one call in `main`, on every system; the runtime's
  start function is a no-op outside Windows.
- Console programs, and every build without the field or option, are unchanged.

## 5. The questions this ADR settles (the owner may revisit them)

1. **The name:** `windowed` (not `gui`, `subsystem` or `console`).
2. **The place:** `[package]` only, not `[target.windows]`.
3. **An option:** `--windowed` for `build` (accepted by `test`, where it does nothing), as `--no-float` is for
   `float`.
4. **Elsewhere:** the field on a library, an object or a freestanding package is N0321; the option with `--emit
   obj`, `--emit lib` or the freestanding profile is a usage error; the field does nothing, silently, for those
   `--emit`s and the profile given on the command line, for `test`, `check` and `run --interp`.
5. **macOS, Linux and FreeBSD:** nothing changes, and no application bundle is made (§6).
6. **gcc's flag:** `-mwindows` (which also links gdi32 and comdlg32), not `-Wl,--subsystem,windows`.
7. **Attaching:** only when a standard stream has no handle, so redirection is never replaced; a valid stream keeps
   its handle even if attaching changed it.
8. **Standard input when attached:** `CONIN$`, the terminal's input.
9. **Without a console:** the streams on `NUL`, so writes succeed and reads are empty; a panic's message is lost.
10. **`--emit c`:** carries the windowed start; the link flags are the linker's caller's to pass.
11. **`test`:** always a console program.

## 6. Not in this ADR

- A macOS application bundle (`.app`, `Info.plist`, an icon) and a Linux `.desktop` file.
- A Windows icon, version resource or manifest (`.rc`), DPI awareness in the executable's manifest (the runtime sets
  it at run time, ADR 0137), and `WinMain`.
- Showing a panic without a console (a message box, the event log).

## 7. Implementation and evidence

See ROADMAP §10's row for the tests run.
