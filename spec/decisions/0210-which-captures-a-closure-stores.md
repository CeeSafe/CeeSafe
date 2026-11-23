# ADR 0210 — Which captures a closure stores

- Status: **Accepted** by the owner, 2026-11-23 ("I accept ADR 0210."), with §6's two questions taken as
  recommended; proposed the same day and implemented under the owner's instruction ("continue with the next roadmap
  item"; the optimization and DX roadmap's Phase 3, item 3: diagnose httpserver's S16); not released.
- Scope: type-system.md §36.2 (the closure rule of §25.7); `src/nir/borrow.cee`, `src/hir/hir.cee`; ADR 0131 §2.5's
  rule and its listed limit; ADR 0168 §3 and §6.

## 1. Context

httpserver's finding S16, reduced:

```ceesafe
fn serve(borrow this, out: borrow mut Out) -> u32 {
    var ctx = Ctx { state: borrow this.state, seen: 0 };
    return twice(() => {
        out.n += 1;
        return this.dispatch(mut ctx);      // N0272: stores a value borrowed from 'out.n' into what 'out' refers to
    });
}
```

`dispatch` may store its receiver into the context (they share the type parameter `S`). Nothing is stored in or from
`out`. The same two statements without the closure are accepted. ADR 0168 made the program compile for an `S` bounded
by `Send`, and recorded twice that why the store was "charged to the wrong capture is still not understood".

**The cause.** It is not a fault: it is ADR 0131's rule, as written. A closure's body is checked first and leaves a
list of the captures it stores through (`captureStores`). Where the closure is made, each such capture "receives the
loans of every other capture" (`collectWrites`). So here `ctx` receives the loan of `out.n`, the mutable borrow the
closure captured to count in. A reference to `ctx` then holds a mutable loan that leads to `out`, the store through
it is taken to reach what `out` refers to, and what is stored there includes that same loan of `out.n`: N0272.
ADR 0131 lists exactly this as a limit: "the checker cannot tell which capture is stored".

The true shape in httpserver passes `out` on in the same call
(`async () => await this.dispatch(found, mut ctx, request, exchange)`), so "captures the body only counts in" would
not be enough: the body has to say which captures' values it stores.

## 2. Decision

A closure's body records, for each capture it stores through, **the captures whose loans the stored values may
hold** (`HirFunction.captureStoreSources`), and where the closure is made that capture receives the loans of those
captures only.

Which captures a value may come from is computed in the body, over its statements, and over-approximates:

1. A read of a place under `(*env).capture` has that capture. A read of the environment as a whole has every
   capture.
2. In each statement, every local the statement writes, **or could write through**, takes the captures of every
   place the statement reads. "Could write through" is any local the statement names that holds loans and is not a
   shared reference, and the locals its loans borrow. So `fill(mut p, v)` gives `p` the captures of `v` whether or
   not `fill` declares the flow.
3. The loan edges the borrow checker already has are followed too. A local that the environment's loans reach by
   an edge, and that no statement gave a capture, has every capture.
4. For a store through a capture (a write through a reference, a call's flow, a collection write, an assignment
   into the capture, an inner closure's stores), the captures stored are those of the locals the stored values were
   read from. For a deep flow that is the argument given, not what the checker resolved it into. A stored value
   that holds the environment's loans and has no capture counts as every capture.

"Every capture" is the rule before this ADR, so each step that cannot follow a value falls back to what was
accepted before.

## 3. Options not taken

- **Leave it, and say "bound `S`"** (ADR 0168's answer). It does not help a closure whose stored value is not of a
  type parameter's type, and the limit is one ADR 0131 only listed because the checker could not do better.
- **Only drop captures that give no loan-holding value** (a number counted in). Simpler, and enough for the
  reduction, not for the program it was reduced from (§1).
- **Per-capture placeholder loans in the body**, so the loans themselves say where they came from. Exact, and a
  change to every rule that reads a placeholder; the statement-level sets are separate from the loans and cannot
  make another check accept more.

## 4. Consequences

- Programs accepted that were N0272: a closure that stores one capture through another while it also uses a
  `borrow mut` parameter without storing in or from it. None is refused that was accepted: the new sets are subsets
  of "every other capture".
- No effect outside closures: the sets are computed only for a function with a closure environment.
- ADR 0131's limit and ADR 0168's two notes point here.

## 5. Implementation and tests (2026-11-23)

- `captureOrigins`, `noteCaptureStores` and `storesFromCapture` (src/nir/borrow.cee); `ThroughWrite.origins` keeps
  the locals a write's values were read from; `Borrows.edges` keeps the loan edges after `computeLoans`.
- run-pass/adr0210-capture-store-sources.cee: the reduction, the shape that passes `out` on, and a view stored into
  a local holder while the parameter is counted in. Each was N0272 with the compiler before.
- check-fail/adr0210-capture-stores-kept.cee: ten closures that do store a view of a local through a captured
  `borrow mut` parameter, by a local, a call's result, a struct, a call that fills a local, another capture, an inner
  closure, a branch, a loop, and across two closures: N0272 each, as before.
- The five existing check-fail/closure-capture-stores*.cee files are unchanged and pass. The results are in
  ROADMAP §10.

## 6. Questions for the owner (answered 2026-11-23: accepted as recommended)

1. Rule 2's "could write through" ignores the callee's declared flows, so it merges more than it must (two captures
   passed to one call beside a `mut` argument). Kept: it is simple to state and to trust, and a later ADR can narrow
   it by the flows if a program needs it.
2. httpserver took `S: Send + Share` onto its interfaces to get past this (ADR 0168). Left as it is: the bound is
   right for those interfaces on its own account.
