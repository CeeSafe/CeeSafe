# ADR 0193 — With a slice, the bound counts the time a resume ran, measured at each pause

- Status: **Accepted** by the owner, 2026-11-21 ("Fix runtime, release"), after httpserver's CI failed on macOS with
  its default slice lowered to 2 ms; implemented, released in 0.84.1.
- Scope: the hosted runtime's slow path for a sliced resume (`runtime/hosted/ceesafe_rt_async.c`, `cee_slice_poll`
  and `cee_slice_resume`), `tools/slice_check.py` (one case), type-system.md §40.12, `Async.runSlice`'s doc comment.
  No language or library change. It amends ADR 0189 §2.3 step 2 and answers its question 6 the other way.

## 1. Context

ADR 0189 counted a sliced resume's time as the slices it had used times the slice, so that the slow path would not
read the clock (its question 6: "the bound is late by at most a slice"). That held for a slice of 10 ms on an idle
machine, and not otherwise: the monitor wakes every quarter of the smallest slice (at least 1 ms) and counts a resume
from the wake that first sees it, so each slice runs its length plus the monitor's lateness, and on a loaded machine
`nanosleep` oversleeps by milliseconds. The lateness is added once a slice, and the bound counts none of it.

- `slice_check`'s new case on 0.83.0 and 0.84.0: a unit alone under a 1 ms slice and a 200 ms bound was ended after
  500 ms, 199 pauses, with the panic line "a task ran 200 ms without waiting".
- httpserver's `supervisor_check` on GitHub's macOS runner, with its default slice lowered from 10 to 2 ms: a request
  under a 250 ms bound was ended after 1.52 to 1.59 s, about 12 ms a nominal 2 ms slice, past the supervisor's
  1.5 s stall; with 10 ms slices, 0.72 s.

## 2. Decision

A sliced resume's time is measured: the clock is read when its stack is switched to (`cee_slice_resume`) and at each
poll past its slice (`cee_slice_poll`), and what it ran is added up on its stack (`cee_stack.ran`). The bound is
compared with that sum, and the panic line names it. A poll that may only end its task (it calls C, it holds a
`Mutex`) adds its slice's time the same way and starts the next. The time a task waits paused is not counted, as
before: the bound is how long one resume runs, not how long it lasts.

One clock read is added at each switch to a sliced resume. The slow path already read it on its end-only branch.

## 3. Consequences

- The bound means what it says under any slice: a unit is ended once it has run the bound, late by up to one
  slice and the monitor's lateness, once, not once a slice.
- A slice can be as small as httpserver's 2 ms without stretching the bound.
- `Async.pauses()` is unchanged. A unit ended after pauses names the time it ran, which is now never less than the
  wall time it ran.

## 4. Implementation and checks

`runtime/hosted/ceesafe_rt_async.c`: `cee_stack.ran` and `since`; `src/library/runtime.cee` regenerated
(`tools/gen_runtime.py`). `slice_check` case `measured`: ended with Overran after 200 to 300 ms of wall time and
20 or more pauses. It fails on 0.84.0 (500 ms) and passes with this change (15 cases, 0 problems, three runs).
`bound_check` 33 cases and `debugger_check` 6 cases, 0 problems. httpserver's `supervisor_check` with a 2 ms slice
on macOS arm64: the two requests past a 250 ms bound are ended after 0.26 s, where 0.84.0 took 0.39 s.
