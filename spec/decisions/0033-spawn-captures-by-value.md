# ADR 0033 — L1 A4d: a scope child per loop iteration, with its own values

- Status: **Accepted** by the owner, 2026-11-01 ("A now, B later"): §3.1–3.2 and 3.4–3.5 now, §3.3 (`async move`)
  as a later step.
- Scope: what a child spawned in an `Async.scope` body may capture (type-system.md §40.5; ADR 0031 §3.3 and §6.6).
  Found by the reactor benchmark (L1 A4b, ROADMAP §10).

## 1. Context

A scope's body is a block of the awaiting function (ADR 0031 §6.6). A child captures by reference, and its
environment lives in the body's block, so a child that captures one of the body's own locals is **N0344**: the local
ends with the body while the child may still run. That rule is sound, but it leaves a common program with no spelling:

```ceesafe
await Async.scope(AsyncError.Cancelled, (s) => {
    for (i in 0..workers) {
        s.spawn(async () => await work(i));          // N0344: i is the body's
    }
    for (c in streams.view()) {
        s.spawn(async () => await hold(c));          // N0344: c is the body's
    }
})?;
```

Checked 2026-11-01 with ceesafec at `844e755`: both loops are N0344, even for a `u32`. The only workaround is
recursion through `Async.all` and `Async.boxed`, one boxed frame per value (`benchmarks/async-reactor/reactor.cee`).

What a child sees through a by-reference capture of a `Copy` local cannot differ from a copy taken at the spawn:
- the body may not use a local after a child captured it (N0241);
- the awaiting function is suspended from the body's end until the scope returns;
- a child that changes a capture captures it mutably, not as a copy.

## 2. Options

| | A. Copies by value, automatically | B. `async move` children | C. A spawn argument |
|---|---|---|---|
| Spelling | `s.spawn(async () => await work(i))` as written | `s.spawn(async move () => await work(i))` | `s.spawn(i, async (i) => …)` |
| `Copy` values (`u32`, `borrow T`) | yes | yes | yes |
| Owned values (a `Vector` built in the body) | no (still N0344) | yes: moved into the child | yes |
| New syntax | none | `async move` (as `move` literals, §36.5) | a second `spawn` form |
| Change to programs that compile today | none (they never capture a body local) | none | none |

## 3. Proposed decision: A, then B

1. **A child's `Copy` captures are held by value in its own frame.** `spawn` copies them when it starts the child,
   whoever owns the captured local, so a `Copy` local of the body is no longer N0344. A child keeps capturing by
   reference what it changes, and what is not `Copy`.
2. **A copied borrow keeps its loan until the scope returns.** A captured `borrow T` (or a `Copy` struct holding one)
   is a value whose loan the scope-loan rule now follows: its referent must outlive the scope. A borrow of a body
   local is N0344 still, with the referent named. A borrow of the awaiting function's locals, or of anything they own,
   is allowed. That is the `for (c in streams.view())` loop.
3. **`async move () => …` children** (B) move non-`Copy` captures into the child's frame, as a `move` literal moves
   into its environment (§36.5). The body cannot use what it moved. The child drops what it owns when it returns or
   is cancelled, and a frame that cannot be allocated drops the moved values with the error.
4. Race and all children (§40.4) follow the same rules. Their frames are the caller's, so a copy lives as long as
   the race.
5. The interpreter already gives each child its own copy of its environment, and keeps doing so.

A first, because it makes the reported program compile as written with no new syntax. B second, as its own step,
because it adds syntax and drop obligations to frames.

## 4. Consequences

- type-system.md §40.5: the scope-loan rule distinguishes copied captures (by value; only their loans are followed)
  from referenced ones (as today). diagnostics.md: N0344's text names the referent of a copied borrow.
- The C backend: a child closure's environment gains the copied values as fields, which `ceeAI_` copies into its
  frame. The frame-size report (`--explain-frames`) counts them.
- Tests:
  - run-pass: a loop of children over indices, and over a vector's elements by borrow;
  - check-fail: a copied borrow of a body local (N0344), and a changed capture in a loop (N0344 still);
  - `benchmarks/async-reactor/reactor.cee` written with the loop, not the recursion.

## 5. Questions for the owner

1. A, copies by value without new syntax? **Yes.**
2. B, `async move` for owned values, as a later step? **Yes, after A.**

## 6. Implementation (2026-11-01)

- **§3.1, done.** While a `spawn` child is checked (`Checker.copyCaptures`), a capture it only reads whose type is
  `Copy` and not scoped becomes a copy (`CaptureMode.Copy`, `src/check/closures.cee`). `checkSpawn` already exempted
  copies from N0344 and N0241. Every async closure's frame holds `cee_env`, its own copy of the environment, which
  `ceeAI_` copies from the closure value (`src/backend/c_async.cee`; the frame report counts it). The closure value is
  one temporary per literal in the awaiting frame, made again by each iteration of a loop, so without the copy every
  child of a loop would read the last iteration's environment. N0344's message now says what a child can copy.
- **§3.2, not yet: a copied borrow.** A capture of a scoped type (`borrow T`, `for (c in v.view())`'s `c`) is still
  by reference, so a body local of that type is N0344. Following its loan until the scope returns needs the borrow
  checker's help: the environment temporary is overwritten by the next iteration, which ends the earlier children's
  loans as the borrow checker sees them, though those children still run. A rule in the type checker alone would not
  be sound. Indexing with a copied loop variable (`borrow v[k]`) covers the case meanwhile. §3.2 goes with §3.3, which
  needs the same tracking of what a child holds until the scope returns.
- **Tests:** run-pass/async-spawn-loop (the loop variable and a body local copied, in C and the interpreter, under
  ASan+UBSan); check-fail/async-spawn-copies (a borrow-holding, a non-Copy and a changed body local: N0344);
  `benchmarks/async-reactor/reactor.cee` spawns its holders in a loop.
