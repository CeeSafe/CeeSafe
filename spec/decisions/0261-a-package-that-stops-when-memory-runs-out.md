# ADR 0261 — A package that stops when memory runs out: `out-of-memory = "panic"`

- Status: **Accepted** by the owner, 2026-11-26 ("Go ahead I accept your recommendation": §6's six questions as
  recommended). Proposed the same day at the owner's instruction ("Tackle 1": the first gap of `ceesafegaps.md`, the
  `!` or `?` after every call that allocates), in the direction the owner chose: a setting for a package's own code.
  **Implemented** on the branch `adr/0261-out-of-memory` (§7), not released.
- Scope: the manifest (tooling.md §4.1) and a command-line option (§12); type-system.md §17.4 and §17.5; the
  checker's typing of calls (`src/check/expr.cee`, `src/check/packages.cee`); the panic message (NIR, the C runtime,
  the interpreter); `--sweep alloc`'s counts (`src/host/test_runner.cee`); diagnostics.md N0262, N0321 and N0402; the
  book's 9.3 and appendix J; guide/for-agents.md and the `ceesafec agents` template.

## 1. Context

Every call that may allocate returns `Result<T, AllocError>`: `v.push(x)`, `String.from(t)`, `Box.create(v)`,
`s.clone()`, `TextBuffer.append` (guide/builtins.md; type-system.md §17). A program writes `?` to pass the failure
on or `!` to stop, at every such call (book 9.3: "the `!` is the program's decision about memory, in one character,
at each place where memory is asked for"). That is CeeSafe's design for kernels, servers that must stay up and small
devices, which must refuse a request and carry on, and it stays the default.

For a program that would stop anyway, the decision is the same at every call, and writing it every time is noise:
the gap list puts it first among the costs of high-level code. Counted on 2026-11-26 (allocating methods and
constructors followed by `!` or `?`, comments excluded):

| Code | Lines | `!` | `?` | Marked calls per 100 lines |
|---|---|---|---|---|
| CeeSafeApps (1,983 files, worktrees and trials excluded) | 944,139 | 1,744 | 32,248 | 3.6 |
| The book's listings | 9,568 | 228 | 44 | 2.8 |
| The guide's examples | 3,586 | 85 | 15 | 2.8 |
| The compiler (`src/`) | 137,784 | 812 | 2,645 | 2.5 |

Two kinds of code show. Servers and libraries pass the failure on: sqlite 18,025 `?` to 124 `!`, cosmostack 9,009 to
131, mailserver 1,033 to 54. For them the `?` is the design working, and nothing here changes it. Programs that stop
write `!`: CeeUI 278 `!` to 21 `?`, the book 228 to 44. A Rust programmer writes neither, since `Vec::push` aborts.
An agent writing a script meets the rule as N0402 warnings, or as N0205 where it used the `Result` as the value.

## 2. Decision

### 2.1 The setting

```toml
[package]
out-of-memory = "panic"      # or "result", the default
```

`out-of-memory` is a field of `[package]` (not of `[target.OS]`), with the values `"result"` (the default, today's
rule) and `"panic"`. Any other value is N0321. It applies to **the package's own sources**, its tests included, and
to no other package:

- each dependency follows its own manifest, so a library's `v.push(x)` means the same whoever depends on it;
- lib/core, lib/std and lib/kernel are always `"result"`;
- a generic function is checked once, in its own package's setting, whoever instantiates it.

It changes nothing in a signature. A function a package declares as returning `Result<T, AllocError>` returns that,
and its callers see it as their own setting says (§2.2).

`ceesafec check`, `run`, `build`, `fix` and `test` on files without a manifest take `--out-of-memory=panic`, so a
script needs no manifest for it. With a manifest, the option is the root package's setting for that command.

### 2.2 The rule

In a package whose setting is `"panic"`, a call whose type is `Result<T, AllocError>` (any function, method or
type's function, of any package; the error type exactly `AllocError`) **has type `T`** and panics if it fails,
unless one of these keeps it a `Result`:

1. it is the operand of `?` or `!` (`v.push(x)?`, `String.from(t)!`), which then mean what they mean today;
2. its expected type is a `Result` (type-system.md §17.4's expected type): `let r: Result<Box<Big>, AllocError> =
   Box.create(b);`, a `return` in a function that returns one, an argument for a `Result` parameter;
3. it is the operand of `let _ =`, which keeps today's meaning: the value is discarded, whatever it was.

```ceesafe
// out-of-memory = "panic"
fn main() {
    var names = Vector<String>.create();
    names.push(String.from("ada"));            // no ! : stops if memory runs out
    let line = String.from("total: ");
    let big: Result<Vector<u8>, AllocError> = Vector<u8>.withCapacity(1 << 30);
    match (big) {                               // the one allocation the program means to survive
        Ok(cache) => {
            Console.writeLine("cache ready");
        }
        Err(_) => {
            Console.writeLine("not enough memory for the cache; going on without it");
        }
    }
}
```

So existing code means the same after the switch: every call it has is written with `?`, `!` or `let _ =`. A call
whose error type is not `AllocError` (`Result<T, IoError>`, an enum with an `@from` variant for `AllocError`) is not
affected: its error is the program's to handle.

### 2.3 What a failure prints

The implicit unwrap panics as `!` does (type-system.md §17.4: panic is abort, nothing is destroyed, exit status
134), with its own message: `ceesafe panic: out of memory at app.cee:4:11`, the call's position. A written `!` keeps
`unwrapped an Err value`. `--sweep alloc` (ADR 0049) counts the new panics as unwrapped, as it does `!` today.

### 2.4 Diagnostics

- **N0402** (a `Result` nobody reads) is not reported for such a call in a `"panic"` package: it has no `Result`.
- **N0262** (`?` or `!` on a non-`Result`) cannot arise from the rule, since rule 1 keeps the operand a `Result`.
- **N0321**: `out-of-memory = "panic"` with `profile = "freestanding"` (§6 question 2).
- A `!` after a call in a `"panic"` package is redundant but not wrong. It is not warned about, and `ceesafec fix`
  does not remove it (§6 question 4).
- In a `"result"` package, a call used where its `T` is expected (`let s: String = String.from(t);`) stays N0205,
  and its message says that `"panic"` would accept it: the code was probably copied from such a package.

### 2.5 A library that says `"panic"`

The setting changes what calls inside the package's own code mean, never a signature: a public function declared
`-> Result<T, AllocError>` returns that type in either setting, and its callers see it as their own setting says
(§2.2). What it changes is when such a function returns `Err`. In its body, a call written with `?` still passes the
failure on, and the caller gets `Err`; a bare call, which the setting unwraps, stops the program instead, the
caller's too.

So a library that opts in keeps its API, and decides call by call which failures its callers may still handle:
`?` where an app should get the `Result` (a large buffer, a user's input), nothing where stopping is acceptable. The
calls it already writes with `?` keep passing the failure on. Only those written with `!` become stops without the
marker, and they were stops already. A caller in a `"result"` package keeps getting a `Result`, and its own
`?` or `!` decides what to do with it.


- **One switch for the whole build.** It would change what a library's `v.push(x)` means according to who builds it:
  a library written to recover would stop, or one written to stop would not compile.
- **A marker on each function** (`@stopsOnNoMemory`). The choice sits where the code is, but most functions of a
  program that stops would carry it, and a function without it reads differently from its neighbours.
- **Infallible twins** (`v.add(x)` beside `v.push(x)`). Every allocating method twice, and the two can be mixed in one
  function; the book would teach both.
- **Changing the default.** The kernels, servers and devices CeeSafe is for need the failure as a value (book 9.3);
  sqlite and cosmostack pass it on 18,025 and 9,009 times.
- **Tooling only** (`ceesafec fix` adds the `!`). It helps an agent finish, but leaves the markers in the code.

## 4. Consequences

- A program that stops loses its `!`s: in the book, 228 markers in 9,568 listing lines. The book would show both,
  with `"result"` still taught first (§6 question 5).
- A call reads differently in the two settings. Code moved from a `"panic"` package into a `"result"` one gets N0205 or
  N0402 at each call, which say what to add; a sure fix could add `!` (§6 question 4).
- Memory safety is unchanged: the implicit unwrap is `!`, and a panic is abort.
- `ceesafe_check` and the language server need the setting of the file's package, which they have from its manifest;
  MCP snippets without a manifest are `"result"`.

## 5. What is not known

- How many of CeeUI's and cosmostore's 1,075 `!`s would go: the count does not separate allocating calls whose result
  is used as a value from statements.
- Whether agents writing in a `"panic"` package then write a bare `v.push(x);` in a library they also edit. The
  diagnostics of §2.4 are the guard; it would show in the habit and fix checks.

## 6. Questions for the owner

1. **The name and values**: `out-of-memory = "panic"` / `"result"`? Recommended: yes, since "panic" is the word the
   book and the specification use for what happens.
2. **Freestanding packages**: refuse `"panic"` there (N0321)? Recommended: yes; a kernel has no process to end, and
   lib/kernel is written to recover.
3. **Libraries**: allow `"panic"` in a `kind = "library"` package, which then stops its host on a failure?
   Recommended: yes, as `trusted` is allowed, and listed on the package's registry page so a user sees it.
4. **Fixes**: in a `"result"` package, a sure fix that adds `!` to a call used where its `T` is expected, in an
   executable only? And no fix that removes `!` in a `"panic"` package? Recommended: yes to both.
5. **Teaching**: the book's listings stay `"result"`, with 9.3 showing the setting and one listing in it?
   Recommended: yes, so that code from the book compiles in any package.
6. **`ceesafec new`**: does a new executable get `out-of-memory = "panic"`? Recommended: no; the default stays the
   one CeeSafe is designed around, and the setting is one line to add.

## 7. The design as built (2026-11-26)

- **The setting.** `out-of-memory` in `[package]` (`src/packages/manifest.cee`), carried to the checker in
  `PackageInfo.outOfMemoryPanic` with the package's kind (`library`, for §6 question 4). `--out-of-memory=panic` and
  `--out-of-memory=result` set the root package's, or the loose files', in `loadPackage`, `loadWorkspaceData` and
  `compileFiles`. N0321 for another value, and for `"panic"` with `profile = "freestanding"`.
- **The rule** (`src/check/options.cee`, `synthCallStoppingOnNoMemory`). A call is checked as any call; if its type is
  `Result<T, AllocError>` and it is not kept, it is wrapped in the HIR's `Unwrap`, noted in
  `Checker.outOfMemoryUnwraps`. `?` and `!` (`checkResultOperand`) and `let _ =` (`checkLocalDecl`) name the call they
  keep in `Checker.keptCall`, compared before the call's arguments are checked, so a nested `f(g()!)` keeps only
  `g()`. An expected `Result` keeps it too.
- **The hint.** The allocating built-ins that take their type from an expected `Result<T, …>` (`Box.create`,
  `withCapacity`, `String.from`) take it, in such a package, from an expected `T` as well (`fallibleHint`), and an
  expected `Box<T>` is no longer stripped to `T` before a member call there, so `let b: Box<i64> = Box.create(5);`
  holds an `i64` as `Box.create(5)!` would.
- **The panic.** NIR's `NirPanicKind.OutOfMemory`, the runtime's `CEE_PANIC_OUT_OF_MEMORY` (21), "out of memory";
  the interpreter's the same. `ceesafec test --sweep alloc` and `tools/fault_injection.py` count it as an unwrap.
- **The diagnostics.** In a package where a failure is a value, N0205 for a `Result<T, AllocError>` whose `T` is the
  expected type adds that the call may fail to allocate, and names the setting. Its fix inserts `!` after the value
  (tooling.md §5.2), in an executable or files given alone, not in a library or an object, and not where the
  `Result`'s `T` differs from what is expected (`Box<i32>` for a `Box<i64>`), which adding `!` would change.
- **Documents.** type-system.md §17.7; tooling.md §4.1, §5.2 and the options table; diagnostics.md N0402;
  testing.md's corpus option; book 9.3 (and `book/projects/ch09-stop`); guide/for-agents.md; the `ceesafec agents`
  template.
- **Tests.** The corpus's `run-pass`, `run-fail` and `check-fail` `out-of-memory-panic.cee` (`// option:
  out-of-memory panic`, built, interpreted and under the sanitizers; the run-fail one fails its first allocation with
  `CEESAFE_FAIL_ALLOC_AT=1`, since a huge `withCapacity` is stopped by AddressSanitizer before the runtime sees it),
  which `tools/fault_injection.py` also builds with the option; `tools/package_tests.py` §47 (the field, a
  dependency's `Result`, the option both ways, N0321, no fix in a library); `tools/fix_kinds_check.py`'s new fix and
  its no-fix case.
