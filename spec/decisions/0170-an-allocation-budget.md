# ADR 0170 — An allocation budget for a task and what it starts

- Status: **Accepted** by the owner, 2026-11-18 ("I accept all the ADRs as written"): every question of the
  "Questions for the owner" section as recommended. Written at the owner's request ("go ahead and write those ADRs")
  from the findings of the HTTP server written in CeeSafe (`~/dev/CeeSafeApps/httpserver/CEESAFE_FINDINGS.md`,
  S5). Implemented 2026-11-18 (§6), with another API than §2.1's; released in ceesafec 0.70.0. **Not run on Windows.**
- Scope: the hosted runtime (the allocator's slow path, `runtime/core/ceesafe_rt_core.c`; the executor's switch of
  tasks, `runtime/hosted/ceesafe_rt_async.c`), lib/core (`Async.budget`, `Budget`), the interpreter; type-system.md
  §40 (a new §40.x), `spec/api/core.md`, the guide's async page. No change to the language, to the generated C's
  calls of the allocator, or to the freestanding profile.

## 1. Context

- **What happens today.** Every allocation in CeeSafe can fail and says so (`Result<…, AllocError>`), but only the
  system decides when: `Vector`, `String` and the other collections take `SystemHeap` or `FailingAllocator` (§30.2),
  and nothing counts what one task has allocated. A request that grows without bound takes the process's memory, and
  the first allocation to fail may be another request's.
- **What a server can do about it now.** httpserver limits a worker process's address space (`RLIMIT_AS`). That
  keeps the failure inside one worker, and since allocations return errors the request that asked is answered
  instead of the process ending (measured: a handler that takes memory until it fails got 495 MiB of a 512 MiB
  limit, answered, and the worker went on). But the limit is the worker's: every request in it shares the 512 MiB,
  one request can take all of it, and the others then fail. It is Linux's, and coarse.
- **What is wanted.** "This request may hold 8 MiB", enforced where the request allocates, so the request that
  grows is the one whose allocation fails, and its handler answers 413 or 503 as it would for any failed
  allocation.
- **What exists to build on.**
  - The system allocator's inline path tests one word before `malloc` and `free` (ADR 0101): counting (ADR 0059)
    and fault injection (`ceesafec test --sweep alloc`) hang off it, on the slow path.
  - The executor knows which task runs on a thread, and `Async.timeout`, `Async.race` and the shield are scopes it
    already keeps for a task and the children it starts (§40.7).
  - The C library tells a block's size (`malloc_usable_size`, `malloc_size`, `_msize`), so a free can be taken off
    a count without a header on every block.

## 2. Decision

### 2.1 The API (lib/core)

```ceesafe
// Runs `body` with at most `bytes` held at once by it and by the tasks it starts.
let r = await Async.budget(8 * 1048576, async () => await this.dispatch(request, exchange));

// The same for code that is not async: a thread, a test, a parser.
let parsed = Budget.run(1048576, () => Json.parse(text));

Budget.held()        // usize?: what the current budget holds now; null outside one
Budget.limit()       // usize?: its limit
```

- **Inside a budget, an allocation that would take what is held over the limit fails**, as an allocation fails
  when the system has no memory: `Err(AllocError.OutOfMemory)` from `push`, `Box.create`, `String.append` and the
  rest. Nothing else about the program changes: the code that handles a failed allocation handles this one.
- **What is held** is the sum of the sizes of the blocks the body's code allocated and has not freed, as the C
  library sizes them. A block freed inside the budget is taken off, whoever allocated it; the count does not go
  below zero.
- **Children.** A task started inside the body (`Async.race`, `all`, a scope's `spawn`) is inside the budget. A
  thread started inside it (`Thread.spawn`) is not: it outlives nothing by rule, and question 3 asks.
- **Nesting.** A budget inside a budget counts against both, and fails at whichever limit is reached first.
- `Async.budget` returns what `body` returns. It does not fail by itself: a body that never handles a failed
  allocation never sees one unless it allocates past the limit.

### 2.2 What it is not

- **Not a sandbox.** It counts the CeeSafe allocator's blocks: `SystemHeap`. A user allocator's boxes (§30), a
  region's or a pool's own growth (they ask `SystemHeap`, and that is counted), memory a C library allocates for
  itself, mapped files and stacks are counted only where they come through `ceesafe_alloc`.
- **Not exact about who holds what.** A block that the body allocates and hands to a task outside the budget
  (through a channel, into a shared cache) stays counted until it is freed, by whoever frees it, and if it is freed
  outside any budget it is never taken off: the budget's count is then too high until the budget ends, which makes
  it stricter, never laxer. A block from outside that the body frees takes the count down by its size (not below
  zero), which makes it laxer by at most what the body was given. For a request's memory, which lives and dies
  with the request, the count is exact.
- **Not a way to recover from a panic.** Code that unwraps an allocation (`push(x)!`) panics on a failure, and a
  panic ends the process (§14.5), inside a budget as outside. A budget is for code that returns its allocation
  errors, which lib/core and lib/std do.

### 2.3 How

- **The current budget is the executor's.** A task carries a pointer to its budget (null for none), inherited by
  the tasks it starts. When the executor resumes a task it sets the thread's current budget to the task's, and
  clears it when the task parks or ends. `Budget.run` sets and restores it around a call.
- **The allocator's word.** While any budget exists in the process, the word the inline path tests (ADR 0101) is
  set, and every allocation and free takes the slow path, which reads the thread's current budget: none, and it is
  `malloc` or `free` as before; one, and the block's size is added or taken off, up the chain of nested budgets,
  with the limit checked before the block is asked for. When the last budget ends, the word is cleared.
- **Cost.** A program that never makes a budget runs the inline path it runs today: nothing is added to it. A
  program with a budget live anywhere pays the slow path for every allocation in the process while it is live
  (ADR 0101 measured that path at about 20% of the time of programs that do little but allocate), plus one
  `malloc_usable_size` for each block counted. A server that budgets every request is such a program. Question 2
  asks whether that is the right trade; the alternative is a per-thread word, which costs the inline path a
  thread-local load for everyone.
- **Counts are atomic words**: a block may be freed on another thread than it was allocated on.
- **The interpreter** counts its own allocations for the program the same way, so a test of a budget runs under
  `ceesafec run --interp` and `ceesafec test`.
- **Freestanding and CosmoOS:** not there. The API is hosted (`profiles.md` §4); a kernel has its own allocator
  and its own accounting.

## 3. Consequences

- A server gives each request a budget and answers the one that outgrows it. The process limit stays as the
  backstop for what a budget does not count (§2.2).
- `ceesafec test --sweep alloc` already runs every test under each failed allocation, so code that is correct
  under the sweep is correct under a budget: the failure is the same one, at a place the sweep visited.
- **A new way for allocation to fail that is not the system's.** A library that treated `OutOfMemory` as "the
  machine is dying" (and, say, stopped serving) would now do that for one greedy request. None of lib/std does;
  the guide says so for programs.

## 4. Questions for the owner

1. A budget as a scope, `Async.budget(bytes, body)` and `Budget.run(bytes, body)`, whose excess is an ordinary
   failed allocation (§2.1)? Recommended: yes.
2. One process-wide word that sends every allocation to the slow path while any budget is live (§2.3)?
   Recommended: yes, measured first on CeeSafe.Web's benchmark server with a budget for each request; if it costs
   more than a few percent there, the per-thread word is measured against it before either lands.
3. A thread started inside a budget is outside it (§2.1)? Recommended: yes. A thread's memory has no scope to end
   with, and a budget that a detached thread could exhaust after its request is gone would fail the next request
   that reuses nothing of it.
4. Live bytes as the C library sizes them, not the sizes asked for (§2.1)? Recommended: yes: it is what a free can
   tell, and it is what the process actually holds.

## 5. Implementation plan (once accepted)

1. Spec: §40 (the scope, beside `timeout`), §30 (a sentence: the collections' failures include a budget's),
   `spec/api/core.md`, profiles.md, the guide.
2. Runtime: the budget record (limit, held, parent), the task's and the thread's pointer, the executor's set and
   clear, the slow path's accounting, the process-wide count of live budgets that sets the inline word. lib/core
   over it. The interpreter's allocator.
3. Tests:
   - run-pass: a body that grows a vector past its budget gets `OutOfMemory` at the expected element count (the
     oracle is arithmetic on the element size and the C library's growth is not assumed: the test asserts a range
     and that `held` never exceeded `limit`); the same after frees; nested budgets; a child task counted; a thread
     not counted; `held` back to zero when the body's values are dropped; outside any budget, `null`;
   - the fault sweep over those tests;
   - the benchmark of question 2, recorded in ROADMAP §10 with its machine and commands;
   - httpserver with a budget for each request, and its supervisor check's "takes memory until it fails" case
     answered by the budget instead of the process limit.
4. ROADMAP §10.

## 6. Implementation notes (2026-11-18)

- **The API is a guard, not a scope with a body.** §2.1 had `Async.budget(bytes, body)` and
  `Budget.run(bytes, body)`. What is there is `Budget.enter(bytes) -> Result<Budget, AllocError>`: the budget is
  current while the value lives, in a task or outside one, and its drop ends it. `Async` is the compiler's own
  (its operations are lowered by the checker and both backends), so a new one is compiler work in four places; a
  guard is the runtime and lib/std alone, covers async and sync code with one form, and nests by scope as a body
  would. `Budget.held()` and `Budget.limit()` are as §2.1 has them. **This is not what was accepted**: if the scope
  form is wanted it can be added over the same runtime.
- **In lib/std**, not lib/core (`lib/std/budget.cee`): lib/core is the freestanding profile's too, and a budget is
  hosted.
- **The runtime** (`runtime/core/ceesafe_rt_core.c`, hosted part): a record of a limit, the bytes held and the
  budget it is inside, with a count of what names it (its guard, a budget inside it, a task made under it), so a
  task that outlives the guard still has somewhere to count. The thread's current budget is a `_Thread_local`.
  `CEE_ALLOC_WATCH_BUDGET` is ADR 0101's word's third bit, set while any record exists. `ceesafe_alloc`,
  `ceesafe_alloc_zeroed`, `ceesafe_realloc` and `ceesafe_free` count `CEE_ALLOC_SYSTEM`'s and `CEE_ALLOC_FAILING`'s
  blocks by `ceesafe_platform_block_size` (`malloc_size`, `malloc_usable_size`, `_msize`); a `realloc` takes what is
  asked beyond the block's size first, so a refusal leaves the block as it was. A new block that does not fit is
  freed again and the allocation fails.
- **The executor** (`runtime/hosted/ceesafe_rt_async.c`): a task has a budget, the one current where it is made
  (`cee_task_init`); the run loop swaps it in around each resume, keeps what the task left current when it parks,
  and frees a finished child's block under it before the task's name for it is dropped.
- **Not there:** under a host's allocator hook (ADR 0088) and on CosmoOS `Budget.enter` fails, since a block's size
  cannot be told. The interpreter enters and leaves a budget and holds nothing back (§2.3 said it would count):
  its memory is its own model.
- **Question 2's measurement** (Linux x86-64, Xeon E3-1270 v6, one process of httpserver's benchmark server, the
  compiler built there from its own C; `h2load --h1 -c 64 -t 4`, two runs of 5 s each): with a budget of 1 MiB
  entered for every request in `App.handle`, against none: `/small` 98 356 and 99 602 against 94 314 and 100 475
  requests a second; `/json` 95 744 and 101 033 against 93 793 and 95 101; 4 KiB of text built for each request
  33 033 and 35 732 against 32 674 and 35 221; 64 KiB, 3 575 and 3 874 against 3 587 and 3 635. No difference
  beyond what two runs of the same thing differ by, so the process-wide word stays and the per-thread word was not
  tried.
- **Tests:** `tools/budget_check.py`, in the contract suite as `budgets`: the two programs natively and with the
  sanitizers, and one in the interpreter; 5 cases on macOS and on Linux. They are not corpus tests: the corpus's
  programs also run in the interpreter, which holds nothing back. httpserver does not use a budget yet.
