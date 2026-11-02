# ADR 0056 — Recursion through `await`: frames from a per-task stack

- Status: **Accepted** by the owner, 2026-11-02 ("I accept ADR 0056 with your recommendations"): every question of §5
  as recommended. Asked for by the owner after CeeSafe.Web's notes on middleware (its v0 limit 3):
  a `Logged` handler holding the next handler as `Box<dyn Handler<S>>` is N0343 once one `Logged` can wrap another,
  and the only way out today is `Async.boxed`, one allocation per middleware layer per request.
- Scope: the coroutine pass's recursion rule (N0343), the C backend's await of a frame held by pointer, the hosted
  runtime's executor, the interpreter (unchanged), the freestanding profile.

## 1. Context

A frame has a size known at compile time and is placed once (type-system.md §40.6): an awaited call's frame is
embedded in its awaiter's. ADR 0036 made a call through `dyn I` take room for the largest frame among the types
coerced to `dyn I` (`S(I, m)`). So if an impl in `S(I, m)` awaits, directly or not, a call that may reach itself, its
frame would contain itself: **N0343** (ADR 0036 §3.5; the same rule for direct recursion, §40.1).

The sure fix is `await Async.boxed(alloc, x.m(…))`: the frame from an allocator, the awaiter holding a pointer, the
value `Result<T, AllocError>`. For middleware that is an allocation and a failure path per layer per request, written
at every call that might recurse.

Three facts make something cheaper possible:

- **Within a task, frames are strictly last in, first out.** An awaited call's frame lives exactly while its awaiter
  waits for it: the awaiter cannot resume, return, or start another await of its own until that call returns, and a
  cancelled call still returns (§40.7: cancellation never destroys a frame). Children (spawned, race, all, sets) are
  other tasks, with frames of their own. So a task's frames that are not embedded form a stack.
- **A task stays on one worker once it has started** (ADR 0038), so its stack is touched by one thread.
- **ceesafec sees every cycle** (ADR 0036 §1, the closed program): the coroutine pass already finds each await edge
  that closes one, to report N0343.

## 2. Options

**Where a recursive await's frame comes from.**

| | A. A per-task frame stack (proposed) | B. `Async.boxed` everywhere, as today | C. A fixed reserve per task |
|---|---|---|---|
| What | an await edge on a cycle takes its frame from its task's stack; the frame is popped when the call returns | each recursive call allocates and frees, and returns `Result` | each task gets a fixed block, as a thread gets a stack |
| Steady cost | none: an executor reuses the stacks' chunks across tasks | an allocation and a free per call | the reserve per task, used or not |
| Source | unchanged: the program that was N0343 compiles | `Async.boxed(…)!` at each such call | unchanged |
| When memory runs out | §3.3 | `Err(OutOfMemory)` at the call | at a depth fixed in advance |

**A is proposed.** It removes the per-call allocation and the source change. Like a thread's call stack, it ties memory
to the depth actually reached, but it grows on demand and gives memory back to the executor, not to the system.
`Async.boxed` stays for code that wants the failure as a value, or a frame from its own allocator.

**Which cycles.** The mechanism is the same for a cycle through `dyn` (ADR 0036) and for direct recursion
(`async fn walk` awaiting `walk`). Proposed: both, so N0343 is gone in the hosted profile, and recursion through
`await` behaves like recursion in sync code (§3.1).

## 3. Proposed decision

### 3.1 Which awaits are stacked

- The coroutine pass finds the **strongly connected components** of the await graph: functions, and through a `dyn I`
  call every impl in `S(I, m)`, as N0343 does today. An await edge **inside** a component is **stacked**: its awaiter
  holds a pointer to the callee's frame, not the frame. Every other await embeds its callee's frame as before, so a
  program without recursion through `await` compiles exactly as today.
- Within one component every edge is stacked, so no frame of the component contains another of it.
- `--explain-frames` marks a stacked await (`stacked: the frame comes from the task's stack, N bytes`).
- **N0343 stays only** where there is no task stack (§3.4), and for an `@effectsOf` function's async instance calling
  itself with its closure (ADR 0037), whose frame is laid out with the closure's.

### 3.2 The task's frame stack

- Each task has a **frame stack**: a list of chunks, empty at first. A stacked await pushes its callee's frame on it
  (aligned for the frame, its size from ceesafec or, through `dyn`, from the table, ADR 0036 §3.6) before the call
  starts, and pops it when the call has returned.
- **Chunks** are 16 KiB, or the frame's size if larger. A chunk the stack no longer uses goes back to its
  **executor's pool** (one per worker thread), which keeps up to 64 for the next tasks; a task that returns gives all
  of its back. So a server whose requests recurse to the same depth allocates in its first requests and then not at
  all.
- A frame on the stack is still placed once and never moves (§40.6): chunks are never moved or resized.
- Drops, cancellation and the debugger's `tasks` command see a stacked frame as an embedded one: the resume chain is
  the same, one pointer deeper.

### 3.3 When memory runs out

A stacked await's value is the callee's, `T`, as for any await. If no chunk can be had:

| | a. Panic (proposed) | b. Cancel the task |
|---|---|---|
| What | `async frame stack: out of memory`, as a sync program whose call stack overflows stops | the task is cancelled, and the failing await returns the cancellation's error |
| Cost | the process stops | only an `Async.*` or `Result<T, AsyncError>` call can carry it; a `T` cannot |

**a is proposed**, and stated where recursion is: memory for recursion through `await` is the call stack's kind, not a
value. Code that wants the failure as a value keeps `Async.boxed`. The fault-injection sweep reaches the chunk
allocation (it is the runtime's `ceesafe_alloc`), so a test sees the panic where memory runs out.

### 3.4 Profiles and threads

- **Hosted:** chunks from the system heap, pooled per executor.
- **Freestanding:** no heap, so no stack: N0343 stays, and `Async.boxed(alloc, …)` remains the way (ADR 0039). An
  environment hook for chunks can come later, when a kernel asks.
- **Threads** (`spawnSend`, `Async.runWorkers`): a task's stack is used only by the worker running it, and its pool is
  that worker's.

### 3.5 The interpreter

Its activations are already dynamic: it runs the program that was N0343 as it is, with no stack to model. Its frames do
not count toward `CEESAFE_ALLOC_REPORT` today and still do not.

## 4. Consequences

- CeeSafe.Web's middleware stacks without `Async.boxed` and without an allocation per request in the steady state:
  `await this.inner.handle(…)` compiles as written.
- Recursive async code (a tree walk, a recursive descent parser) is written as sync code is.
- **The diagnostic changes meaning:** N0343 is the freestanding profile's and ADR 0037's case only. Programs that use
  `Async.boxed` to break a cycle keep working, with their own allocation.
- **Memory is no longer fully known at compile time** for a program with recursion through `await`: its depth decides
  it, as for sync recursion. `--explain-frames` says which awaits are stacked.
- New diagnostics: none.

## 5. Questions for the owner

1. Frames of recursive awaits from a per-task stack of pooled chunks (option A), with no source change? Recommended:
   yes.
2. Both kinds of cycle: through `dyn` and direct recursion, so N0343 remains only in the freestanding profile and for
   ADR 0037's case? Recommended: yes.
3. Running out of memory for the stack panics (3.3 a), with `Async.boxed` kept for failure as a value? Recommended:
   yes.
4. 16 KiB chunks and a pool of 64 per executor, as constants in the runtime for now? Recommended: yes.

## 6. Implementation plan (once accepted)

1. Spec: §40.1 and §40.6 (stacked awaits; N0343's remaining cases), §40.8 (the interpreter), c-backend.md §3a (the
   stacked await, the stack and the pool), diagnostics.md N0343, the guide's async page.
2. `src/nir/coroutines.cee`: the components of the await graph; stacked edges instead of N0343, where the profile has a
   stack.
3. C backend: a stacked await site holds a pointer in the awaiter's frame, pushes the frame (`ceesafe_async_stack_push(
   size, align)`), starts and resumes it through the pointer, and pops it (`ceesafe_async_stack_pop(frame)`) when it
   returns; `--explain-frames`.
4. Runtime: the task's chunk list, push and pop, the executor's pool, the panic; `tools/gen_runtime.py`.
5. Tests:
   - run-pass: middleware stacked three deep through `Box<dyn Handler>` (CeeSafe.Web's shape), a recursive tree walk
     through `dyn`, direct recursion (a recursive `async fn` 10 000 deep, past one chunk), a cancellation deep in a
     recursion, natively and in the interpreter;
   - `CEESAFE_ALLOC_REPORT`: after a warm-up, a thousand requests through stacked middleware allocate no chunk (a
     native tool beside the contract suite);
   - the fault sweep on the recursive programs (the panic where the chunk fails), the cancellation sweep, ASan, TSan
     with `Async.runWorkers`;
   - frames: a `.frames` snapshot showing stacked awaits;
   - check-fail: N0343 in the freestanding profile and for ADR 0037's case.
6. ROADMAP §10.

## 7. Implementation notes (2026-11-02)

- **The role.** The coroutine pass gives each await on a cycle the NIR task role `taskStacked` (it finds them as it
  found N0343's: an await whose callee reaches its caller, through `dyn` tables too), so every await of a component is
  stacked. The C backend gives a stacked await a pointer member, pushes and pops the frame around it, and does not wait
  for the callee's struct before the awaiter's (a pointer needs only its name). The dyn frame of a stacked call through
  `dyn` is emitted after every impl's, as one only run or boxed already was.
- **The size comes from the C struct,** not the table: the program is closed, so `sizeof(struct ceeAF_dyn_…)` is the
  union of every impl's frame, as ADR 0036 lays it out.
- **The task record** gains the top chunk (`cee_task.stack`), so it and `ceesafe_task_slot` are 160 bytes (were 144):
  the backend's `taskSlotLayout`, and the interpreter's model of a task's head under `scopeIn` and `tasksIn`.
- **The task record is aligned to 16** (`_Alignas(16)` on its first member): at 152 bytes, a spawned child's frame,
  which follows its record rounded to `max_align_t`'s alignment, was 8-aligned where that alignment is 8 (macOS arm64,
  MSVC), and UBSan reported a frame struct requiring 16 (run-pass/async-wake-waiters). A static assertion keeps it.
- **The fault sweep** (`tools/fault_injection.py`) counts `ceesafe_async_stack_push` as an allocation and accepts its
  panic, which §3.3 makes the outcome of a failed chunk.
- **N0343 stays** for a cycle through a `race` or `all` child (no single call holds the frame) and for ADR 0037's
  async instance, as §3.1 said, and in the freestanding profile.
- **Effects:** the frame stack's allocation is not an `alloc` effect of the awaiting function, as a sync call's stack
  is not: the coroutine pass runs after the effect checker, and §3.3 makes the failure a panic of the call stack's
  kind.
- **Tests:** run-pass/async-recursion-stacked (direct and mutual recursion, recursion 5 000 deep, a tree of
  `Box<dyn Node>`, middleware three deep, a timeout cancelling a recursion 500 deep), natively and in the interpreter;
  check-fail/async-recursion now freestanding (N0343 there), check-fail/async-dyn-frames keeps its N0295;
  `tools/frame_stack_check.py` in the contract suite (a thousand requests through stacked middleware allocate no chunk
  after the first; a recursion 20 000 deep leaves nothing live; a failed chunk panics, under ASan).

