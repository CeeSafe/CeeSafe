# ADR 0190 — Polls asked for by the manifest

- Status: **Accepted** by the owner, 2026-11-20 ("I accept all the above ADRs as written": §8's five questions as
  recommended); implemented, not released (§10). Proposed the same day. Written at the owner's request ("Draft ADRs for all gaps"), from the review of
  httpserver against Elixir's process isolation that followed ceesafec 0.80.0. Of the five places where Elixir is
  still ahead, this is the compiler's part of the third, "isolation is opt-in": a server package cannot make the
  bound of ADR 0186 its default, because polls exist only in a build whose command line says
  `--cflag -DCEE_POLLS=1`. ADR 0186 §8 question 1 foresaw it: "A manifest setting (`[build] polls = true`) could
  set the macro later". The other gaps are ADR 0189 (a task past its slice paused, not ended), CeeSafe.Web's ADR
  0017 (a request isolated apart from its connection) and httpserver's plan (`docs/elixir-gaps.md`: isolation as
  its default, a supervisor for tasks, recycling with no refused connection).
- Scope: the manifest (`src/packages/manifest.cee`, tooling.md §4.1), the command line (`--polls`, `--no-polls`,
  tooling.md §12.1), the C files the build writes (`src/host/build.cee`, `src/host/toolchain.cee`), c-backend.md
  §3b, type-system.md §40.11's first item. No change to the language, to NIR, to the runtime or to where polls go:
  ADR 0186 §2.2 places them, and `CEE_POLLS` still decides whether they compile to anything.

## 1. Context

- **What a build with polls needs today.** ADR 0186 writes `CEE_POLL(…)` into every program's C, empty unless the
  C compiler sees `CEE_POLLS` defined (`ceesafe/rt.h`; c-backend.md §3b). The runtime's monitor thread and
  executor registry (`runtime/hosted/ceesafe_rt_async.c`, `#if defined(CEE_POLLS)`) and lib/core's
  `Async.runBound` (`cfg("CEE_POLLS")`, lowered to `#if CEE_POLLS`) follow the same macro. The one way to define it
  is the command line: `ceesafec build --cflag -DCEE_POLLS=1` (`/DCEE_POLLS=1` with `cl`), which
  `pushCompileFlags` (`src/host/toolchain.cee:434`) passes to every C file of the build, the runtime's included.
- **What forgets it.** Every command that builds: `build`, `run`, `test` (natively, its default), and every
  script, CI file and editor task that calls them. A build without it is a working program with no bound:
  `Async.runBound` answers false and nothing else says so. httpserver's benchmark server prints its own line for
  that case (`bench: CW_RUN_BOUND_MS needs a build with --cflag -DCEE_POLLS=1; running unbounded`), and its README
  and `tools/supervisor_check.py` carry the flag by hand. Windows spells it differently for `cl` (ADR 0186 §10, the
  Windows run, where `/DCEE_POLLS=1` given to MinGW's gcc was taken as a file to link).
- **What a manifest can say about the C today** (tooling.md §4.1): `links`, `lib-dirs`, `include-dirs`, and in
  `[target.OS]` tables the same and `frameworks`. Directories and library names, checked for shape, never a flag:
  no manifest key reaches the C compiler's command line as text. Build-wide settings come from the **root**
  manifest only: `profile`, `float` (`workspace.root.floatAllowed`, `src/main.cee:1092`), `kernel`, `windowed`,
  `[patch]` ("A dependency's own `[patch]` is not applied"). The command line adds to them (`--profile`,
  `--no-float`, `--windowed`).
- **What polls cost** (ADR 0186 §5.1 and §10): geometric means over the benchmarks of +10.2% instructions and
  +5.3% cycles; `parsing` +24%, `const-flags` +27%, `hash-map` +11%, most others within 2%. On httpserver's
  benchmark server, `/small` and `/json` within noise and `/gz/16384` 6% fewer requests a second, deflate's inner
  loops being where it goes. That is why question 1 kept them off by default, and why a server wants them while a
  numeric program does not.

## 2. Decision

### 2.1 The key

```toml
[package]
name = "server"
polls = true        # this program is built with polls (ADR 0186): Async.runBound can bound its tasks
```

- **`polls`** is a boolean of `[package]`. `true` builds the program with polls, as `--cflag -DCEE_POLLS=1` does
  today; `false`, the default, without.
- **The root's decides** (question 1), as for `float` and `windowed`. A dependency's `polls` is read and checked,
  and has no effect on a build whose root it is not: a library cannot turn on a cost in its users' loops, nor turn
  off a bound they asked for. A library sets it for its own tests and examples, whose root it is.
- It is N0321 when it is not a boolean, in a `[target.OS]` table (polls do not depend on the system), and with
  `profile = "freestanding"` (no monitor thread: ADR 0186 §3; today the C compiler's `#error`).

### 2.2 How it reaches the C

**Every C file the build writes defines the macro itself** (question 2): with polls on, the generated C files and
the runtime's `ceesafe_rt_*.c` begin with

```c
#define CEE_POLLS 1  /* polls: the root manifest's polls = true, or --polls (ADR 0190) */
```

before their first `#include`. Not a `-D` on the command line, for three reasons:
- **`--emit c` stays complete.** Its C builds the same program with any compiler and no flags; a `-D` would have to
  be written to `ceesafe_link.txt` or remembered by whoever compiles it.
- **One spelling.** `cl`, gcc and clang read the same line; ADR 0186's Windows run met the other way's trap.
- **`--split` and `--emit obj`/`--emit lib`** compile every file apart; each carries its own line, so no file can
  miss it.

A `--cflag -DCEE_POLLS=1` on top is an identical redefinition, which C allows: today's scripts keep working.
C files of packages (`c = [...]`) and their separately compiled objects are the package's own C and do not get it.

### 2.3 The command line

- **`--polls`** and **`--no-polls`** (`build`, `run`, `test`) override the root's key for one build, as `--profile`
  overrides `profile`. Both given is a usage error (status 2). `--no-polls` with `--cflag -DCEE_POLLS=1` is a usage
  error too: the two contradict, and the flag would win in the C.
- `--cflag -DCEE_POLLS=1` stays what it is, and the spelling the ADR 0186 tools use; tooling.md §12.1 names
  `--polls` as the way to say it.

### 2.4 The commands

- **`build` and `run`** (natively): as §2.2. **`test`** builds its harness natively by default (tooling.md §10):
  with polls on, the harness has them, so a package's tests of its own handling of an overrun run as the program
  does. `test --interp`, like `run --interp`, is §2.5's.
- **`check`, `fix`, `fmt`, `doc`, the language server**: no C, nothing changes. `cfg("CEE_POLLS")` is a build's
  question, and checking answers it as it does today.
- **Nothing built is cached** today (the record cache, ADR 0082 §2.7, holds checks, which polls do not change).
  An object cache keyed by the C's text (ADR 0082 §2.6) would see §2.2's line in it, so a build with polls and one
  without could not share an object.

### 2.5 The interpreter

Unchanged: it bounds a task by fuel whatever the key says (ADR 0186 §2.5), since its `cfg` cannot see the macro.
`run --interp` and `test --interp` of a package with `polls = false` still honour `Async.runBound`. That
difference is ADR 0186's and stays: the interpreter has no cost to save.

### 2.6 What is reported

- **Nothing new at build time.** A successful `build` prints nothing today, and this adds no line. `--emit c`'s
  files show it in their first line (§2.2).
- **A program that asks for a bound it cannot have** is still told at run time, by `Async.runBound` answering
  false (ADR 0186 §2.1), which a server reports as httpserver's benchmark server does. A compile-time warning for a
  call of `Async.runBound` in a build without polls is left out (question 3): the call is usually in a library
  (httpserver's `App`), under a setting the program reads at run time, and a library's warnings are not the user's
  to fix.

## 3. What it does not do

- **No generic C flags in manifests** (§4). Only this one, named, checked setting.
- **Polls stay off by default.** Turning them on for every program is ADR 0186's question 1 again, with the same
  numbers (§1).
- **No polls per package.** The key is the root's; a dependency cannot exempt its loops (`@noPoll` on a function
  is the way, ADR 0186 §2.3: what it exempts is then in the code, where a reviewer finds it).
- **Nothing for `cfg` names in general** (httpserver's `HTTPSERVER_TRAP_HOOKS`): a root's `[cfg]` table could set
  them the same way (§2.2's line, one per name), but that is its own question and ADR.

## 4. Options that were weighed

- **A generic `cflags` key in manifests.** It would serve this and every other macro, and is what C build systems
  have. Rejected: a flag is the C compiler's language, not CeeSafe's, and different for each compiler (`-D` and
  `/D`); a dependency's flags would apply to the whole program's C, which is one translation unit for all
  packages, and could undo its checks (`-DNDEBUG`-like switches of the runtime's own macros), change its ABI, or
  run code at build time (`-fplugin=`, `-include`), against ADR 0013's "nothing runs". Root-only would answer the
  last, not the first two.
- **Polls on by default, with `polls = false` to opt out.** The bound would be every server's without asking.
  Rejected for now on ADR 0186 §5.1's costs: every program pays +5% cycles on the mean and up to +27% in tight
  scanning loops for a limit only programs that serve requests use. To be asked again when ADR 0189's pause
  (which needs the same polls) is measured.
- **Polls inferred from use**: a program whose code calls `Async.runBound` or `spawnIsolatedBound` is built with
  polls. No key, and the footgun of a bound that answers false goes. Rejected: the call is in a library (httpserver
  calls it from `App` whenever a setting is given), so a dependency would turn on a cost in the application's own
  loops without the application saying so; and whether a call is reached is the program's run-time choice.
- **Per package**: polls in the functions of the packages that ask for them, none in others'. The backend knows
  each function's package, so it is possible (an implicit `@noPoll` on every function of a package without the
  key). It is sound: a missing poll only weakens the bound, a loop without one is not caught, and the monitor's
  word waits for the next poll there is. Rejected: it inverts who decides. The application owns the bound; a
  dependency that opts out hides its loops from it, which is the case the bound is for (a quadratic parse of a
  hostile input in a library). A dependency that measured a hot loop has `@noPoll`.
- **A `[build]` table** for the key (ADR 0186 §8's guess). The manifest has none; `[package]` already holds the
  build-wide settings the root decides (`profile`, `float`, `windowed`). A table for one key would be the first of
  a kind, so it waits for a second.
- **`-D` on the command line from the key** (in place of §2.2's line). Simpler to build, and what `--cflag` does.
  Rejected for `--emit c`'s sake and the two spellings (§2.2).

## 5. Consequences

- A server package says `polls = true` once, and every build of it, by any command, script or editor, has the
  bound it asks for. httpserver's plan makes isolation and a bound its defaults on this (`docs/elixir-gaps.md`).
- The flag in scripts (httpserver's README, `tools/bound_check.py`, `windows.yml`) keeps working, and can become
  `--polls`.
- A library's `polls = true` changes only its own tests and examples. Its README says what it needs.
- `--emit c` output of a package with polls builds with polls everywhere, with no flag.

## 6. Why it is sound

Polls are not a safety mechanism: they decide only whether a task past its bound is seen (ADR 0186 §6). The key
changes which C is compiled, never what a program means: a program with polls and one without compute the same
results, unless a bound ends a task. The root deciding keeps the trade (a bound for speed) with the one who owns
the program.

## 7. What it costs to get wrong

- A manifest key is public: once released, packages write it. The name and its place in `[package]` are the hard
  part to change (question 1); its meaning can grow (`polls = "slices"` if ADR 0189 needs a mode) without breaking
  `true` and `false`.
- If a later release turns polls on by default, `polls = false` already means what the opt-out needs.

## 8. Questions for the owner

1. `polls = true` in `[package]`, the root's deciding as for `float` and `windowed`, a dependency's having no effect
   on its users (§2.1)? Recommended: yes.
2. The macro defined by a line at the top of every C file the build writes, not a `-D` (§2.2)? Recommended: yes:
   `--emit c` stays complete and `cl` needs no other spelling.
3. No compile-time warning for `Async.runBound` in a build without polls, `Async.runBound` answering false being
   the report (§2.6)? Recommended: yes.
4. `--polls` and `--no-polls` for one build (§2.3)? Recommended: yes.
5. Polls still off by default, to be asked again with ADR 0189's measurements (§3, §4)? Recommended: yes.

## 9. Implementation plan (once accepted)

1. **Manifest**: `polls` in `[package]` (`src/packages/manifest.cee`, beside `float`); N0321 for a value that is not
   a boolean, in `[target.OS]`, with `profile = "freestanding"`; `workspace.root.polls`. tooling.md §4.1.
2. **Options**: `--polls`, `--no-polls` (`src/main.cee`, beside `--windowed`), the root's value when neither is
   given; the usage errors of §2.3; tooling.md §12.1 and `ceesafec --help`.
3. **The C**: with polls on, the line of §2.2 at the top of every generated file and every runtime file the build
   writes (`src/host/build.cee`, where `writeFilesUnder` gets them; `--emit c` and `--split` included). c-backend.md
   §3b, type-system.md §40.11's first item ("A build with `polls = true`, `--polls`, or `--cflag -DCEE_POLLS=1`").
4. **Tests**:
   - `tools/package_tests.py`: a package with `polls = true` whose program spins and whose `Async.runBound` must
     answer true and end the spin (from `tools/bound_check.py`'s programs); the same with `--no-polls` (answers
     false); a dependency with `polls = true` under a root without (answers false); `ceesafec test` in a package with
     `polls = true` (a test of an overrun passes natively); `--emit c` of it compiled by plain `cc` with no flag
     (polls present: `bound_check`'s listing comparison, the poll's load found); the N0321 cases.
   - `tools/cli_check.py`: the two options against the help texts and tooling.md §12.1; the two usage errors.
   - `tools/bound_check.py` and `windows.yml` move to `--polls` (and one case keeps `--cflag -DCEE_POLLS=1`).
   - The contract suite's `bound` step unchanged otherwise; the fixed point (the compiler's own manifest has no key).
5. httpserver then sets `polls = true` in `bench/ceesafe.toml` and, per its plan, says what its users' manifests
   need.

## 10. Implementation notes (2026-11-20)

Implemented on `adr/0190-polls-from-manifest`, from `main` at 0.80.0 (`33e43bda`), §9's steps 1 to 4; step 5 is
httpserver's own.

- **Manifest** (`src/packages/manifest.cee`): `polls` in `[package]`, beside `windowed`. N0321 for a value that is
  not a boolean ("'polls' is true or false (ADR 0190)"), for `polls = true` with `profile = "freestanding"` (at the
  value), and for `polls` in any `[target.OS]` table ("'polls' belongs in [package], not [target.linux]: polls do not
  depend on the system"), each one diagnostic. `polls = false` with the freestanding profile is accepted.
- **Options** (`src/main.cee`): `--polls` and `--no-polls`, read where `build`'s options are, so `build` and `test`
  take them (`test` builds its harness with `build`'s reader). `run` is the interpreter's alone (`'run' needs
  --interp`), so it does not take them: §2.3's "`build`, `run`, `test`" is `build` and `test`, and `run --interp`
  is §2.5's. Neither given, the root's key decides, and a freestanding build (`--profile freestanding` over a
  hosted manifest) has no polls whatever its key. Usage errors (status 2): both options; `--no-polls` with a
  `--cflag` that defines the macro (`-DCEE_POLLS`, `-DCEE_POLLS=…`, or `/D` for `cl`); and, beyond §2.3, `--polls`
  with `--profile freestanding`, which the C compiler's `#error` would otherwise report.
- **The C**: with polls on, every `.c` file the backend returns (the program's and the runtime's) gets the line of
  §2.2 before its first byte, in `compileFiles` before the build writes them, so `--emit c`, `--split`, `--emit
  obj` and `--emit lib` all have it. The program's own C files (`c = [...]`, files given on the command line) and the
  packages' separately compiled ones are added after and do not get it; headers do not need it.
- **Documents**: tooling.md §4.1 (the example manifest and a **Polls** paragraph), §12.1's table; c-backend.md §3b;
  type-system.md §40.11's first item; `ceesafec --help`; the book's §14.1 table and appendix E (both option tables
  and the manifest table).
- **Tests**: `tools/package_tests.py` check 43 (§9 step 4's list: the key, `false`, no key, `--polls`, `--no-polls`,
  `--split`; a dependency's key under a root without it; `test` with the key and with `--no-polls`; `--emit c`'s six
  files each starting with the line, and compiled by `cc` with no flag, the spinning child ended; the three N0321
  and the three usage errors). Against 0.80.0 it reports 11 problems. `tools/bound_check.py` builds `survive` with
  `--polls` and keeps `ends` on `--cflag -DCEE_POLLS=1`; `windows.yml` runs `bound_check` and needed no change.
- **Results** (macOS, Apple clang 21, a load of 40 to 150 from other work): the fixed point; `package_tests` 41
  checks, 0 problems; `bound_check` 33 cases, 0 problems; `cli_check` 54 options against the help texts and
  tooling.md §12.1, 0 problems; the corpus's check-fail 383, check-pass 48, emit-c 31, no failure; `spec_check` 0
  problems; `book_appendix_check` and `book_check` one problem each, appendix J's missing row for 0.80.1, a release
  tagged after this branch's base (not this change's).

