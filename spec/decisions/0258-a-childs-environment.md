# ADR 0258 — A child's environment: set, remove, clear

- Status: **Proposed — implemented** 2026-11-25 on the branch `fix/g5-os` (from 0.95.10), for the SQLite port's
  `CEESAFE_FINDINGS.md` finding 51, at the owner's instruction ("Add environment control to `ChildOptions` and to
  `Process.capture`/`run`/`spawn`: set, remove, and clear-and-start-empty, as OS strings"). §6's questions are the
  owner's.
- Scope: lib/std `host.cee` (`ChildOptions`, `HostServices.spawn`, `spawnWith`, `capture`, and the new `runWith`,
  `captureWith` and their `…Os` forms); the checker (`src/check/expr.cee`: the process calls take a
  `Vector<StringView>` or a `Vector<OsStringView>`); the runtime (`ceesafe_host_spawn`, `ceesafe_host_child_start_with`:
  the environment's changes); the interpreter's models of the two; type-system.md §26.5, §26.7, §26.16, §26.18;
  `tools/child_check.py`, `tools/process_check.py`.

## 1. Context

A child had its parent's environment and nothing else: `Process.run`, `capture` and `spawn` take a program and
arguments, and `ChildOptions` only the three streams. The SQLite port's tests must start the sqlite3 shell with HOME
(USERPROFILE on Windows) naming a directory of their own, whose `.sqliterc` holds `.crlf off`; they did it through a
Python script (`tests/capture.py`) started by `Process.spawn`.

## 2. Decision

### 2.1 `ChildOptions` holds the environment's changes

- `ChildOptions` keeps its public `input`, `output` and `error`, and gains a private list of changes to the
  environment, so it is no longer Copy and can no longer be written as a literal outside lib/std (N0296):
  `ChildOptions.streams(input, output, error)` makes the three-stream options a literal made. `inherit()` and
  `quiet()` are as before.
- `options.setEnvironment(name: OsStringView, value: OsStringView)`: the child has `name` with `value`.
- `options.removeEnvironment(name)`: the child does not have `name`.
- `options.clearEnvironment()`: the child starts with an empty environment, and the changes made before are
  forgotten; the changes made after are its only variables.
- Each records the change and is `Result<void, AllocError>` (`clearEnvironment` is `void`). The changes are applied in
  order to the parent's environment (or to an empty one after `clearEnvironment`) when a child is started: a later
  change of a name replaces an earlier one. With no changes, the child has the parent's environment, as before.
- Names and values are OS strings (ADR 0257), so a literal or a `StringView` is accepted (ADR 0257 §2.3).
- **Checked at the start**, not when recorded: an empty name, a name with `=` or a zero byte, or a value with a zero
  byte is `InvalidInput`, and nothing is started. On Windows, bytes that are not well-formed WTF-8 are too.
- **Names are compared as the system compares them**: bytes on POSIX; on Windows without case (ordinal, as
  `CompareStringOrdinal` with `bIgnoreCase`), so `removeEnvironment("path")` removes `Path`.
- `CEESAFE_HANDOVER`, the runtime's word to a child that is handed things (ADR 0173), is the runtime's: it is never
  passed on from the parent, and a change naming it is `InvalidInput`.

### 2.2 The calls that take options

- `Process.spawn(program, arguments, options)` and `Process.spawnWith(program, arguments, options, handover)` take
  `options: borrow ChildOptions` (they took it by value): every call that compiled still does, since an argument is
  lent to a `borrow` parameter, and a program can start several children with one `ChildOptions`.
- **`Process.runWith(program, arguments, options: borrow ChildOptions)`** (new, lib/std): `Process.run` with the
  options' streams (`Inherit` or `Null`) and environment: it starts the child, waits for it, and returns its exit
  status, or 128 plus the signal that ended it. Standard output is flushed first.
- **`Process.captureWith(program, arguments, input, options: borrow ChildOptions)`** (new, lib/std):
  `Process.capture` with the options' environment. Its streams are capture's own (the files of §26.7): the options'
  `input`, `output` and `error` are not used.
- `Process.run` and `Process.capture` are unchanged: the parent's environment.

### 2.3 The runtime

- `ceesafe_host_spawn` (capture) and `ceesafe_host_child_start_with` (spawn, spawnWith, runWith) take the changes as
  an array of name and value texts, a kind for each (set or remove) and whether to clear.
- POSIX: the runtime builds the child's `envp` from `environ` (or nothing, when cleared), without
  `CEESAFE_HANDOVER`, applies the changes in order, adds its own `CEESAFE_HANDOVER` word when things are handed over,
  and passes it to `posix_spawnp`. The program is still looked up on the **parent's** `PATH`, whatever the options say
  (§6 question 1).
- Windows: the same on the UTF-16 block of `GetEnvironmentStringsW` (`CREATE_UNICODE_ENVIRONMENT`), each name and
  value from WTF-8 (ADR 0257 §2.6). An empty environment is a block of two terminators.

### 2.4 OS-string arguments

`capture`, `captureWith`, `runWith`, `spawn` and `spawnWith` take `arguments` as a `borrow Vector<StringView>` (as
before) or a `borrow Vector<OsStringView>`: the checker reads the argument's element type and calls lib/std's
`HostServices` function of that name, or its `…Os` form, which differ only in the element type. The program is an
`OsStringView` in both (a literal or a `StringView` is accepted, ADR 0257 §2.3).

### 2.5 Where

- Hosted, on Linux, macOS, FreeBSD and Windows. CosmoOS's `spawnvp` takes no environment: a change there is
  `Unsupported` (none is the parent's environment, as before).
- The interpreter starts no child (`spawn`, `runWith`: `Unsupported`, as for `spawn`), and its `capture` runs on the
  compiler's own, which the release's stage 0 builds: `captureWith` with changes is `Unsupported` there, and without
  any is `capture`.

## 3. Alternatives

- **A builder, `Command`** (Rust's), with the program, arguments, environment and streams. It would be a fourth way to
  start a program beside `run`, `capture` and `spawn`, which the owner asked to extend.
- **A public `environment` field.** `ChildOptions { input, output, error }` breaks either way; a private list keeps
  the representation lib/std's, and methods can check what a field cannot.
- **Check names when recorded.** A setter that can fail on a name would need two error types (`AllocError`,
  `IoError`); the start reports `InvalidInput` already for a zero byte in an argument.

## 4. Migration

- `ChildOptions { input: …, output: …, error: … }` outside lib/std is N0296 (a private field): write
  `ChildOptions.streams(…, …, …)`. Two of CeeSafe's own checks (`tools/process_check.py`, `tools/child_check.py`)
  did; none of CeeSafeLibs and CeeSafeApps does.
- A `ChildOptions` is no longer Copy: a program that copied one into two variables and changed one moves it now.

## 5. Tests

- `tools/child_check.py` (contract step `children`): a CeeSafe parent starts itself with `spawn`, `runWith` and
  `captureWith`; the child prints its environment, which Python compares with the parent's as changed: a set
  (an ASCII value, a non-ASCII one, an empty one), a set that replaces an existing variable, a remove, a clear with
  and without later sets, the same name set twice (the last wins); on Windows a name set in another case replaces
  the variable; an `=` in a name and an empty name are `InvalidInput`.
- `tools/process_check.py`: `captureWith` gives the sqlite3-like case: HOME set for the child alone.

## 6. Questions for the owner

1. The program is looked up on the parent's `PATH` even when the options set the child's (Rust searches the child's
   first). Search the child's?
2. `runWith` and `captureWith` are new names; should `run` and `capture` take options instead (a source change)?
