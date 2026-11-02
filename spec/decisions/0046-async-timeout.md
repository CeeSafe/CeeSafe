# ADR 0046 — `Async.timeout` and `Async.deadline`

- Status: **Accepted** by the owner, 2026-11-02 ("Yes, I accept each and every ADR's questions"): every question of
  §5 as recommended. Asked for by the owner for CeeSafe.Web
  (`~/dev/CeeSafe.Web/docs/ceesafe-upstream-work.md`, item U4). It is the third of that document's ADRs, after ADR 0044
  (U1, U2) and ADR 0045 (U3).
- Scope: two new built-in `Async` operations (type-system.md §40.4). This covers the checker, the HIR and NIR forms, the
  C backend and the interpreter, plus a lib/core join.

## 1. Context

A deadline on an async operation is written today as a race:

```ceesafe
let r = await Async.race(
    async () => await readRequest(mut stream),
    async () => { await Timer.sleep(30000)?; return Err(WebError.TimedOut); },
);
```

That has three costs:

- **It is verbose, and the result is a `Raced<T>`** to unwrap.
- **The deadline child must return the body's error type.** So that type needs a `TimedOut` variant of its own, and
  the child must turn `Timer.sleep`'s own `Cancelled` into it. When the body wins, the deadline child is cancelled and
  its error is dropped. That is correct, but it is easy to get backwards.
- **It cannot be wrapped in a function.** A helper `withDeadline(ms, body)` takes the body as a closure parameter. A
  closure parameter cannot be awaited inside an `Async.race` child of an `@effectsOf` function (N0342 when awaited,
  N0295 when called, §40.3).
  - The other route, an `@effectsOf` *async* fn called with an async closure, is checked but miscompiles: it is bug
    B1 in the request, a null function call. That bug is fixed separately, not by this ADR.

So a deadline must be one of `Async`'s built-in operations, as `race` and `shield` are.

## 2. Options

| | A. Built-in, lowered as a two-child join (proposed) | B. Built-in, a cancel deadline on one child | C. A library function |
|---|---|---|---|
| What | the body and a deadline child run as `race`'s two children; whichever returns first decides | the body runs as one child whose scope the executor cancels when a deadline passes | `@effectsOf(body) async fn timeout(…)` in lib/core |
| Cost | a new HIR form and checker rule. The frames, cancellation, the C join, the interpreter and the cancel sweep are `race`'s | a new executor mechanism: a deadline that cancels a scope, on every executor (hosted, freestanding, interpreter) | cannot be written today (§1); needs B1 fixed and a language change for closure parameters in race children |

**A is proposed.** It reuses `race`'s tested machinery: frames embedded in the caller's, so no allocation;
cancel-and-await of the loser; the interpreter's join. The deadline child is a `Timer.sleep`, whose frame is small. B
would save that frame, but it adds a second way for a task to be cancelled, on every executor.

## 3. Proposed decision

### 3.1 The operations

| Operation | Result | Notes |
|---|---|---|
| `Async.timeout(ms, timedOut, body)` | `Result<T, E>` | Runs `body`, an async closure of result `Result<T, E>`. If it has not returned `ms: u64` milliseconds after the call, it is cancelled and awaited, and the result is `Err(timedOut)` (`timedOut: E`) |
| `Async.deadline(at, timedOut, body)` | `Result<T, E>` | The same, with an absolute deadline `at: u64` on `Timer.now()`'s clock. A deadline already past cancels the body at its first waiting operation |

Both are written after `await`, as every task-producing `Async` operation is. The body's type comes from an expected
`Result<T, E>`, else from the body's expression, as for `shield`. `timedOut` is evaluated before the body starts. If
it is not used, it is dropped when the operation returns.

### 3.2 Who decides

The first of the two to return decides, as in `race`:

- **The body returns first** with `Ok(v)` or `Err(e)`: that is the result. The deadline child is cancelled and
  awaited.
- **The deadline passes first:** the body is cancelled and awaited (§40.7: its next waiting operation returns
  `Cancelled`, and an operation in the kernel is cancelled and waited for).
  - If the body still returns `Ok(v)`, because it finished before it saw the cancellation, the result is `Ok(v)`. No
    value is lost, as for `race`'s `other`.
  - If it returns any `Err`, the result is `Err(timedOut)` and the body's error is dropped. After the deadline, the
    body's error is almost always its view of the cancellation.
- **The caller is cancelled from outside:** the body is cancelled with it (the flag is inherited, §40.7). The deadline
  child's `Timer.sleep` returns `Err(Cancelled)`, and the deadline child never decides with an error. So the result is
  the body's own, typically its `Cancelled`, never `timedOut`. A timeout reports only its own deadline.
- **A tie:** if the body and the deadline finish at the same moment, whichever task runs first decides, as in `race`.
  Natively that is timing. In the interpreter, the virtual clock and the run-order seed fix it, so a given seed always
  gives the same result.
- **`ms = 0`:** the body starts and runs until its first waiting operation, which returns `Cancelled`. A body that
  never waits returns its value.

### 3.3 Shields and nesting

- A timeout inside `Async.shield` still cancels its own body when its deadline passes. The body is the timeout's
  child task, with its own flag, as a `race` child inside a shield is. This is checked in the implementation, not
  assumed.
- Timeouts nest. An inner deadline later than the outer one never fires: the outer one cancels the inner body first,
  and the inner timeout returns the body's error.

### 3.4 Lowering

- **Checker** (`src/check/async.cee`, beside `race` and `shield`): it checks the arguments and the body's result
  type. It builds the deadline child as a closure over the computed deadline that awaits lib/core's `Timer.sleep`.
  Its result is `Result<void, AsyncError>`, which is never the user's `E`.
- **HIR:** a new form `AsyncTimeout(site, body, deadline child, timedOut, settle)`.
- **NIR and the C backend:** lowered as `race`'s join: both frames are embedded in the caller's, and the first to
  return cancels the other.
- **Settle:** lib/core's `AsyncTimeoutJoin<T, E>.settle(first, body, deadline, timedOut)` gives §3.2's result.
- **Interpreter:** the same join, with `isRace` and a timeout flag.

The compiler's own sources do not use the operations, so the bootstrap from 0.3.2 needs no chain step.

## 4. Consequences

- CeeSafe.Web's connection deadlines (header read, idle keep-alive, body read) are one expression each, with
  CeeSafe.Web's own error type and no `TimedOut` plumbing.
- A generic `withDeadline` helper is still not writable as a function (§1). `Async.timeout` takes its place.
- New diagnostics: none. A `timedOut` that is not `E` is the usual type mismatch, and a body that is not an async
  closure is N0342, as for `race`.

## 5. Questions for the owner

1. A built-in operation lowered through `race`'s two-child join (option A), rather than a new executor deadline
   (option B)? Recommended: yes.
2. Add `Async.deadline(at, timedOut, body)` beside `Async.timeout`? Recommended: yes. It is the same lowering with a
   different first argument.
3. After the deadline, keep a body's late `Ok`, and turn any `Err` into `timedOut` (§3.2)? Recommended: yes.
4. A cancellation from outside gives the body's error, never `timedOut`? Recommended: yes.
5. Ties decided by run order, as in `race`, rather than a "body wins" rule? Recommended: yes. Such a rule would need
   the executor to compare the two tasks' deadlines, which no other operation does.

## 6. Implementation plan (once accepted)

1. Spec: §40.4 (the table), §40.7 (cancellation: the timeout's own deadline), `intermediate-representation.md` and
   `c-backend.md` (the join), the API page.
2. `src/`:
   - checker: argument types and inference, the deadline child;
   - HIR: `AsyncTimeout` and its printer form;
   - NIR: the join;
   - the C backend: `c_async.cee`'s join;
   - the interpreter's join;
   - `effect_check` (the operation has `suspend`, as `race` has).
3. lib/core: `AsyncTimeoutJoin<T, E>`; then `tools/gen_library.py`.
4. Tests:
   - corpus run-pass, each in the interpreter (virtual clock) and natively. The timeout fires during a socket read, a
     sleep, a channel receive and a nested scope. A body that finishes first. A late `Ok` that is kept. An outer
     cancellation that is not reported as `timedOut`. A timeout inside a shield. Nested timeouts. `ms = 0`.
     `Async.deadline` with a past deadline;
   - check-fail: a `timedOut` of the wrong type; a body that is not an async closure;
   - `.hir` snapshot of the new form;
   - `tools/cancel_sweep.py` over the new tests, under ASan with `--check-leaks`. The contract suite on macOS, on
     Debian (with "reactor uring") and on Windows.
5. The guide (`guide/async.md`, cancellation and deadlines), ROADMAP §10.

## 7. Implementation notes (2026-11-02)

- **The join.** The HIR form is `AsyncTimeout(site, body, until, at, timedOut, settle)`. It lowers to the join `race`
  uses, with kind 2: `Builtin(AsyncJoin, [site, 2, body, its closure, until, at])`. Two things about a join changed,
  each for every kind:
  - a child that is a function rather than a closure, with one parameter, is given the operand as its argument (in
    C, to its initialiser; in the interpreter, as its first value);
  - the second result has its own child's type.
- **Library walk.** The C backend's walk of the library functions a program uses now follows a join's children.
  `AsyncTimeoutJoin<T, E>.until` is reached only that way.
- **Shields.** A timeout inside a shield cancels its body: a join's children are tasks of their own, with no shield
  (corpus `async-timeout`, case i).
- **Bug B3, fixed here.** `async-timeout`'s socket case found it again: a closure that slices a captured array as
  `mut a[..]` captured it shared, so its environment held a `const` pointer, which `-Werror` rejects. `synthSlice`
  now notes the mutable use, as `borrow mut` does (corpus `async-race-mut-capture`, the CeeSafe.Web probe).
