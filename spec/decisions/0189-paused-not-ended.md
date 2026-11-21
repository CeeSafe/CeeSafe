# ADR 0189 — Paused, not ended: an isolated unit past its slice yields on a stack of its own

- Status: **Accepted** by the owner, 2026-11-20 ("I accept all the above ADRs as written": §8's nine questions as
  recommended); implemented, not released (§10). Proposed the same day. Asked for by the owner ("We need to tackle where
  Elixir is ahead", then "Draft ADRs for all gaps"), after httpserver's step 7 of ADR 0186 compared httpserver's
  isolation with Elixir's. This is the first gap: Erlang's scheduler takes the CPU from a process and gives it back
  later; ADR 0186 can only end the task. The other gaps are other documents (a CeeSafe.Web ADR for a request's own unit
  and a 500, ADR 0190 for polls from the manifest, httpserver's plan for supervision and recycling).
- Scope: the hosted runtime's executor (`runtime/hosted/ceesafe_rt_async.c`), a small stack-switching layer with
  one routine per architecture, lib/core's `Async` (one call and one counter), the checker's list of async calls,
  the C backend's polls (a second kind, question 3), the interpreter (one call, no pauses), the specification and
  the checks. No new syntax. Nothing changes for a program that does not ask for a slice, or a build without polls.
- It revisits ADR 0186 §3's "No suspension" and its question 2, which the owner answered "never suspend" on
  2026-11-20. That answer stands for a task in no unit and for a build without a slice; this ADR asks to pause
  isolated units only (§8, question 1).

## 1. Context

- **What ADR 0186 gives.** In a build with `-DCEE_POLLS=1`, `Async.runBound(ms, mode)` bounds one resume of a task.
  A monitor thread sets the overdue thread's poll word (`cee_bound_watch`, `ceesafe_rt_async.c:2264`), the next poll
  takes the slow path (`cee_bound_poll_slow`, `:2396`), and a task of an isolated unit traps with
  `PanicKind.Overran`, which ADR 0175 turns into the end of its unit alone (`ceesafe_panic_with`, `:2424`; the jump
  back to the executor, `cee_isolate_panic`, `:2051`). httpserver's step 7 measured it: a request that spins is
  ended in 0.27 to 0.33 s under a 250 ms bound, and its worker answers the others meanwhile (ADR 0186 §10).
- **What it does not give.** The neighbours of a spinning request wait for the whole bound: 0.27 s in that check.
  A request that legitimately computes for longer than the bound is ended, so the bound must be set above the
  longest honest request, which is also how long every neighbour may be held. Erlang does neither: a process is
  descheduled after a few thousand reductions and resumed later, so a long computation finishes and its
  neighbours are delayed by a slice, not by the computation.
- **Why ADR 0186 did not pause.** Its §1: "a CeeSafe task can only be suspended at an `await`: between two awaits
  its state is on the C stack, in the frames of the sync functions it called, and those frames cannot be moved
  aside". Its §4 weighed making every loop a suspension point (every looping function a coroutine, loans across a
  loop becoming loans across a suspension point, N0349) and rejected it. Both are right about moving frames. Neither
  considered **not moving them**: if the sync frames of a resume are on a stack of their own, a pause leaves them
  where they are, and the executor goes on on its own stack. Nothing in the language changes: no frame moves, no
  loan crosses an `await`, and the code the compiler emits is the same.
- **Why only isolated units.** Two tasks of one executor today interleave only at awaits, and a task's sync code
  may rely on that: a `borrow mut` of a value a sibling task also reaches between its awaits, an invariant broken
  for the length of a loop. Pausing such a task in the middle would let the sibling see it. An isolated unit cannot
  share that way: its root's captures must be `Send` and hold no loan (N0334, N0349; ADR 0175 §2.1), so what it
  shares with the rest of the program is reached through `Send` values it owns and `Share` values (`Arc`, `Mutex`,
  atomics) that are safe against another thread. Pausing it at a poll is what an operating system does to a thread
  at any instruction, and the program is already correct against that.
- **What exists to build on**, as checked against `main` at `33e43bda`:
  - The resume of a task of a unit goes through `cee_resume_isolated` (`:3449`), which sets the jump point a trap
    returns to (`cee_exec.jump`, `jump_depth`) and calls `cee_resume` (`:3439`), the task's state machine. The loop
    around it (`cee_exec_tasks`, `:3461`) swaps the task's budget (ADR 0170), starts the bound's epoch
    (`cee_bound_resume`) and, after it, clears the word (`cee_bound_waited`).
  - The slow path already refuses to end a unit where ending it would be unsound, and leaves the word set for a
    later poll: in a `Critical` (`t->unit->critical`, kept by `ceesafe_async_critical_enter`, `:2502`), or with C's
    frames between the executor and the poll (`ceesafe_entry_depth() != cee_exec.jump_depth`, `:2419`).
  - A unit records the locks its tasks hold (`ceesafe_async_lock_held`, `:2520`): kind 1 is lib/std's `Mutex`, a
    blocking lock (`lib/std/sync.cee:86`; `pthread_mutex_lock`, `ceesafe_rt_thread.c:205`), kind 2 `AsyncMutex`
    (`lib/std/channel.cee:336`). `OnceLock`'s initialisation runs inside a `Critical` (`lib/std/sync.cee:218`).
  - The monitor is one runtime thread that wakes every quarter of the smallest bound (`:2266`) and compares each
    executor's epoch with the one it last saw.
- **No stack switching exists today.** The runtime has no `ucontext`, fiber or hand-written switch; a stack
  overflow is a guard page and the end of the process (ownership.md, "Stack overflow"). The C backend's stack is the
  thread's: 8 MiB on Windows by the link flags (c-backend.md §1.1), the system's elsewhere.

## 2. Decision

A program may give isolated units a **slice**. A task of a unit whose resume has run past the slice is **paused** at
its next poll that may pause: its sync frames stay on the stack they run on, the executor returns to its own stack,
and the task goes to the back of the ready queue. It goes on from the poll when its turn comes again. ADR 0186's
bound still ends a unit, and with a slice it counts the slices of one resume, so a runaway still ends while an
honest long computation shares the thread.

### 2.1 The API

```ceesafe
// The slice: how long one resume of a task of an isolated unit may run before it is paused at its next poll, in
// milliseconds. 0 (the default) is none. False, and nothing started, in a native build without polls, or on a
// target without stack switching (§2.6); the interpreter takes the call and answers true (§2.7).
let sliced: bool = Async.runSlice(10);

// ADR 0186's bound, unchanged in form. With a slice in force it is the time of one resume summed over its slices.
let bounded: bool = Async.runBound(2000, RunBound.Count);

Async.pauses()   // u64: how many times a task has been paused (§2.4)
```

- **A slice and a bound together** are the Erlang-like setting: `runSlice(10)` and `runBound(2000)` let a request
  compute for up to two seconds of its own time, ten milliseconds at a time, and end it with `Overran` past that.
  A slice alone lets a unit compute forever, sharing the thread: Erlang's behaviour, and a program's choice.
- **Only isolated units are paused.** A task in no unit is never paused; ADR 0186 §2.4 applies to it as before
  (counted, or ended with `RunBound.Panic`). An isolated unit's own bound (`spawnIsolatedBound`) is unchanged and
  applies over its slices.
- The names follow ADR 0186 §2.1's: a `run` call for the program's setting, a counter for what happened.

### 2.2 How a resume runs

1. **On a stack of its own.** With a slice in force, `cee_resume_isolated` takes a stack from the executor's pool
   (§2.5) and switches to it. On that stack a trampoline sets the jump point a trap returns to, calls
   `cee_resume`, and switches back with its result: 1 returned, 0 waits, −1 trapped. The executor's loop is
   otherwise as it is. Without a slice, nothing changes: the resume runs on the executor's stack.
2. **The jump point is on the task's stack**, not the executor's. A trap's `longjmp` therefore never crosses
   stacks. That matters on Linux, where glibc's checked `longjmp` (`_FORTIFY_SOURCE`, which the contract suite's
   `run fortify` step builds with) aborts a jump to a stack below the current one, and on Windows, where `longjmp`
   unwinds frame by frame.
3. **The return switch hands the stack back** to the pool when the resume returned or waits, and the task's
   budget goes back as it does today.

### 2.3 How a task is paused

The monitor sets the word when a resume has run past the slice, as it does for the bound (its period becomes a
quarter of the smallest slice or bound). *ADR 0203 also sets it when the resume's thread has I/O to serve.* `cee_bound_poll_slow` then, for a task of a unit:

1. **May it pause here?** Only if all of these hold, else the word stays set and a later poll asks again:
   - the unit is in no `Critical` (`t->unit->critical == 0`), as for ending it;
   - no C frames are between the executor and the poll (`ceesafe_entry_depth() == cee_exec.jump_depth`), as for
     ending it: a C library's frames are not paused in the middle;
   - the unit records no kind-1 lock (a `Mutex`): a task paused holding a blocking lock would block any task of
     the same thread that asks for it, and that thread is the one that would resume the holder (§6). An
     `AsyncMutex` held across a pause is a held async lock: its waiters park, as they do while the holder awaits;
   - the poll is not inside a nested `Async.run` (the bound's `depth` is 1): an inner executor's tasks may borrow
     the sync frames of the function that called it, and the inner loop is not the one that would requeue;
   - the poll is one the compiler marked as one that may pause (question 3: not in a function that calls a C
     function itself, whose thread-local state a task of the same thread could then see half used).
2. **Is the bound past?** If a bound applies and the slices this resume has used, times the slice, reach it, the
   task traps with `Overran` exactly as under ADR 0186. The count is kept on the task and cleared when it waits.
3. **Pause.** The task is marked paused and pinned to this thread, `Async.pauses()` is counted, the word is
   cleared, its budget is swapped back, and the task switches to the executor's stack. The executor puts it at the
   back of the ready queue and goes on.
4. **Resume.** When the executor takes a paused task, it starts a new epoch for the monitor, swaps the budget in,
   and switches to the task's stack instead of calling `cee_resume`; the task returns from `cee_bound_poll_slow`
   and goes on from the poll.

A wake that arrives for a paused task is stale: it registered no wait, and the loop drops such wakes today
(`cee_exec_tasks`, `:3545`). A cancellation is seen at the task's next await, as for a task
that is running. A unit that dies while one of its tasks is paused (another task of it trapped) never resumes that
task: it is dropped from the queue as a task of a dead unit is (`cee_unit_dead`, `:2042`), and its stack goes back
to the pool, since nothing outside the task points into its sync frames (§6).

### 2.4 What is reported

- `Async.pauses()`, process-wide, as `Async.overruns()` is.
- Nothing on standard error: a pause is the normal working of a slice, not a fault.
- A unit ended by its bound after pauses has the same panic line as under ADR 0186; the time named is the slices'
  sum.

### 2.5 The stacks

- **Size.** Each stack reserves 8 MiB of address space, the C backend's thread stack on Windows and the usual one
  elsewhere, with a guard page below it, so a resume can recurse as deep as it can today and an overflow ends the
  process as it does today. Pages are committed as they are touched; a resume that does not recurse touches one or
  two.
- **The pool** is the executor's own (thread-local, like `cee_exec`): a stack taken at a resume is given back at the
  wait, so an executor that pauses nothing uses one stack. A paused task keeps its stack until it waits. The pool
  keeps at most 16 free stacks and unmaps the rest; a stack given back after a deep resume has its pages released
  (`madvise(MADV_DONTNEED)`, `VirtualAlloc(MEM_RESET)`) past its first 64 KiB.
- **Memory per paused task:** one stack, of which the pages touched are resident (16 KiB pages on Apple's arm64,
  4 KiB on most others).

### 2.6 Where it is built

- **The switch** saves the callee-saved registers on the current stack, stores the stack pointer, loads the other
  and restores its registers: x86-64 System V (`rbx`, `rbp`, `r12`–`r15`, and the x87 and SSE control words),
  arm64 (`x19`–`x28`, `fp`, `lr`, `d8`–`d15`; on Apple, `x18` is left alone). It is a `.S` file of the runtime's,
  one routine per architecture, as the probe's (§5.1).
- **Windows** uses fibers (`ConvertThreadToFiber` once per executor thread, `CreateFiberEx`, `SwitchToFiber`), which
  keep the thread information block's stack bounds that structured exceptions, `__chkstk` and `longjmp` read, and
  stay compatible with hardware-enforced stack protection.
- **Linux, macOS, FreeBSD, Android and iOS** on x86-64 and arm64 use the switch. An object built from the `.S` file
  carries no shadow-stack property, so a Linux binary with it is not run with Intel's shadow stack.
- **No slice** (`Async.runSlice` returns false): 32-bit targets (ADR 0187's `armeabi-v7a` and i686), where 8 MiB of
  address space a stack is too much to promise, until a measured need; freestanding builds, which have no polls
  (ADR 0186 §3).
- **Sanitizers.** Each switch is told to AddressSanitizer (`__sanitizer_start_switch_fiber`,
  `__sanitizer_finish_switch_fiber`) and ThreadSanitizer (`__tsan_create_fiber`, `__tsan_switch_to_fiber`,
  `__tsan_destroy_fiber`) when they are on, since the contract suite runs both.
- **The debugger.** A running task's stack ends in the trampoline, whose frame record is set to the executor's
  frame at each switch, so `bt` walks from the task into the executor. A paused task's frames are on no thread;
  the lldb script (`tools/debugger_check.py`'s "tasks") lists paused tasks with their saved stack, frame and
  program counters.

### 2.7 The interpreter

The interpreter's clock is virtual and does not move while a task computes (type-system.md §40.8), so no task
delays another's timers, and pausing would change nothing a program can observe. `Async.runSlice` is the
interpreter's own: it answers true and records the slice, and nothing is paused. ADR 0186 §2.5's fuel still ends a
unit past its bound. A test of pausing is run natively, with `--cflag -DCEE_POLLS=1`.

## 3. What it does not do

- **No pause for a task in no unit**, or in a unit inside a `Critical`, holding a `Mutex`, under C's frames or in a
  nested `Async.run` (§2.3). Those keep ADR 0186's behaviour: counted or ended at the bound.
- **Nothing for C code or system calls.** A computation in C still holds the thread; the supervisor's heartbeat is
  still the remedy (ADR 0186 §3).
- **No migration.** A paused task resumes on the thread that paused it. Its sync frames may hold the addresses of
  thread-local values the C compiler computed once; moving them would be unsound. Isolated units are not on
  `Async.runWorkers`' other workers yet anyway (ADR 0175 question 7).
- **No total CPU time of a request** over all its resumes: the bound is still one resume's (ADR 0186 §3), now summed
  over that resume's slices.
- **No priority.** The ready queue stays first in, first out; a paused task goes to its back.

## 4. Options that were weighed

- **Every loop a suspension point** (ADR 0186 §4). Rejected there and here: it changes what programs mean.
- **Keep ADR 0186 and raise the bound.** No new machinery, but neighbours wait for the whole bound, and an honest
  request longer than it is ended. It is what this ADR replaces for units, and what remains for everything else.
- **`ucontext` (`getcontext`, `makecontext`, `swapcontext`).** Portable on POSIX, but `swapcontext` saves and
  restores the signal mask, a system call: 1.8 µs for a switch pair against 29 ns hand-written in the probe (§5.1).
  It is deprecated on macOS and absent from Android's libc before API 28.
- **Resume every task on the executor's stack, and on a pause hand the executor a new stack** ("handoff"). The
  paused task keeps the stack it ran on, executor frames under it, and the loop goes on on a pooled stack; when the
  task's resume returns into the old loop's frames, those see they are stale and switch to the live loop. Nothing
  is paid by a resume that is not paused. It was weighed against the measured cost of the switch per resume (about
  27 ns, §5.1) and set aside for its complexity: two instances of the loop, a stale one that must hand its result
  over, the first pause leaving the thread's own stack (and `main`'s frames) under a task, and a run that ends on a
  pooled stack having to switch back to return through them. Question 2 keeps it open if the server's numbers ask.
- **A thread for each paused task.** Moving the executor's other tasks to a new thread when one overruns, as Go does
  for a blocked system call, would cover C code too, but tasks are pinned to their executor's thread-local state
  and the program's non-isolated tasks may not move. It is a different executor.
- **Signals and a jump at any instruction**, Go's asynchronous preemption: rejected by ADR 0186 §4 for the same
  reasons (async-signal-safety, the allocator, no Windows equivalent).
- **Pausing at every poll, C or not.** Simpler for the compiler, but a loop that calls into a C library between
  polls could be paused with that library's thread-local state half used (OpenSSL's error queue is per thread), and
  another task of the thread would see it. Question 3.

## 5. Consequences

- A request that computes no longer holds its neighbours for the bound: they wait for one slice, plus the monitor's
  lateness (a quarter of the slice) and the poll's distance. The bound can be set for runaways (seconds), not for
  latency.
- An isolated resume costs one switch pair and a pool take and give back, about 27 ns on this machine (§5.1). On
  httpserver's `/small`, 68 642 requests a second on one process (ADR 0186 §5.1), a request takes about 14.6 µs and
  a few resumes; a few switch pairs are under 1% of it. That is arithmetic, not a measurement of the server.
- Memory: one stack per paused task and one per executor, mostly address space.
- The runtime gains its first assembly: two routines and a Windows path, each tested under the sanitizers.

### 5.1 The probe (2026-11-20; throwaway, not committed)

A C program on this machine (Apple M1, macOS, Apple clang 21, `-O2`), timed with each thread's CPU time
(`CLOCK_THREAD_CPUTIME_ID`) since the machine's load average was about 140 on 8 cores from other work; medians of
seven rounds, two runs agreeing within 3%:

| What | Cost |
|---|---:|
| Hand-written arm64 switch, there and back (§2.6's registers) | 29.2–29.3 ns |
| `swapcontext`, there and back | 1 822–1 863 ns |
| A resume of an empty body: direct call / on a pooled stack | 2.3–2.7 ns / 29.1–29.5 ns |
| A 100-iteration body: direct / on a pooled stack | 50.7–51.8 ns / 90.2–91.9 ns |
| A 1 000-iteration body: direct / on a pooled stack | 552–555 ns / 581–603 ns |
| Making a 256 KiB stack with a guard page, touching its top and unmapping it | 3.1–3.8 µs |

So a resume on a pooled stack costs about 27 to 40 ns more than a direct call, and a stack must be pooled, not made
for each resume (3 µs). The 100-iteration body's larger difference is the switch's mispredicted returns, which the
next resume's work absorbs. Not measured: x86-64, Windows fibers, the sanitizers' annotations, and the server.

## 6. Why it is sound

- **Nothing moves.** A paused task's frames stay at their addresses, so every pointer into them, which the
  language allows only from within the task, stays good.
- **Nothing outside the task points into its sync frames.** A task cannot be spawned from sync code; the frames of
  async functions are the task's heap frames or its frame stack (ADR 0056), not the C stack; a nested `Async.run`,
  whose tasks could borrow a sync frame, is excluded (§2.3). So another task running while this one is paused
  cannot reach those frames, and a stack given back after the unit died has no reader.
- **What runs meanwhile cannot see a broken invariant of the unit.** The unit shares only `Send` values it owns and
  `Share` values (§1). The program is already correct against the unit running on another thread, interleaved at
  any instruction; a pause at a poll is a special case of that. Tasks in no unit are never paused, so their
  reliance on interleaving only at awaits holds.
- **No deadlock on the thread.** A blocking lock is never held across a pause (§2.3). A `Critical` is never paused
  in, so the half-made state it guards (`OnceLock`'s initialisation, the runtime's own) is never seen.
- **The runtime's thread-local state** is the executor's (`cee_exec.current`, the budget, the entry depth, the poll
  word), and a pause saves and restores it as a wait does. C's thread-local state is excluded by question 3's rule;
  `errno` is read by the runtime's C right after its system calls, where no poll is.
- **A trap after a pause** jumps to the point on its own stack (§2.2), as a trap before one does.
- **The kernel writes into no sync frame after a pause.** With readiness (epoll, kqueue, poll) a read completes
  inside its call; with io_uring (ADR 0035) and Windows' completions the buffers are an async frame's, waited for at
  an `await`, never across a sync stretch.

## 7. What it costs to get wrong

- A register the switch forgets to save corrupts the task or the executor at some later point, far from the
  cause. The switch is two short routines, checked by a test that fills every callee-saved register with a pattern
  and checks it after thousands of switches, on each architecture in CI, and under both sanitizers.
- A missing condition in §2.3 would pause a task where another task could see its state: the checks list each
  condition and test it (a `Critical`, a held `Mutex`, a C callback, a nested run), as ADR 0186's bound_check does.
- If the cost per resume were larger on some host than the probe's, every isolated resume would pay it while a
  slice is in force; question 2's handoff removes it.
- Misplacing the requeue (a paused task stolen by another worker) would resume it on a foreign thread; pinning is
  checked by a test with `Async.runWorkers`.

## 8. Questions for the owner

1. Pause isolated units past a slice, revisiting ADR 0186's question 2 ("never suspend") for them only, with tasks
   in no unit and builds without a slice unchanged? Recommended: yes.
2. Run every resume of an isolated task on a pooled stack while a slice is in force (§2.2), rather than the handoff
   that pays only when a task is paused (§4)? Recommended: yes, and revisit if httpserver's numbers with a slice
   show the switch in its profile.
3. Pause only at polls in functions that call no C function themselves; a poll in one that does may only end the
   task (`CEE_POLL_END`, ADR 0186's behaviour)? Recommended: yes. A compute loop calls no C itself; a loop that
   does is the one whose C thread-local state could be seen half used.
4. 8 MiB of address space a stack, a guard page, at most 16 free stacks kept per executor (§2.5)? Recommended: yes.
5. Targets: x86-64 and arm64 on Linux, macOS, FreeBSD, Android and iOS with the hand-written switch; Windows with
   fibers; `Async.runSlice` false on 32-bit targets and freestanding builds (§2.6)? Recommended: yes.
6. With a slice, the bound counts the slices one resume used, times the slice, rather than reading the clock at each
   pause and resume (§2.3, step 2)? Recommended: yes: the slow path stays the only place that does anything, and
   the bound is late by at most a slice. *Reversed by ADR 0193: the monitor's lateness is added once a slice, so the
   time is now measured at each pause.*
7. The interpreter accepts a slice and pauses nothing (§2.7)? Recommended: yes.
8. A paused task is pinned to its thread and never stolen (§3)? Recommended: yes.
9. No slice by default; httpserver sets its own (`CW_RUN_SLICE_MS`, 10 ms in the benchmark server, with
   `CW_RUN_BOUND_MS` raised to seconds)? Recommended: yes.

## 9. Implementation plan (once accepted)

1. **The switch and the pool** (`runtime/hosted/ceesafe_rt_stack.S`, `ceesafe_rt_stack.c`): the two routines, the
   Windows fibers, the pool, the sanitizer annotations. A C test of the registers and of a trap's jump on a pooled
   stack, built in each CI job, and under ASan, UBSan and TSan.
2. **The executor:** `cee_resume_isolated` on a pooled stack with the jump point on it; the pause in
   `cee_bound_poll_slow` with §2.3's conditions; the paused task's requeue, pinning and resume; the bound counted in
   slices; the monitor's period from the slice.
3. **lib/core and the checker:** `Async.runSlice` and `Async.pauses`, in the checker's async calls
   (`src/check/async.cee`) as `runBound` is.
4. **The backend:** the second kind of poll (`CEE_POLL_END`) in functions that call a C function, if question 3 is
   accepted; the emit-c snapshot.
5. **The interpreter:** `Async.runSlice` taken and recorded; no pauses.
6. **Checks:** `tools/bound_check.py` gains a paused unit whose neighbours are answered within the slice and the
   monitor's lateness, a unit that computes past the bound over many slices and is ended, and one case each for a
   `Critical`, a held `Mutex`, a C callback, a nested run, a trap after a pause, a cancellation of a paused task and
   a unit that dies while one of its tasks is paused; the same under `run asan` and `run tsan`; the Windows
   workflow runs it with `cl`.
7. **The specification:** type-system.md §40.11 (the slice, the pause and its conditions), c-backend.md (the stacks
   and the switch), testing.md, and the book's async chapter.
8. **httpserver** (its own step): `CW_RUN_SLICE_MS`, and `supervisor_check.py --isolated` measuring the neighbour's
   wait against the slice instead of the bound.

## 10. Implementation notes (2026-11-20)

Implemented on `adr/0189-preemption` from `main` at `33e43bda` (0.80.0), §9's steps 1 to 7; step 8 is httpserver's.

- **What it is.** `Async.runSlice(ms)` and `Async.pauses()` (lib/core's `AsyncBound`, the checker's async calls,
  `spec/api/core.md`); the runtime's slices in `ceesafe_rt_async.c` (the stack pool, the switch, the pause in the
  slow path, the requeue); `CEE_POLL_END` and `ceesafe_poll_slow_end` (`rt.h`, the core runtime), which the backend
  writes in a function that calls an `extern "C"` function or a C function pointer itself (`c_polls.cee`'s
  `callsForeign`); the interpreter's two calls (a slice recorded, nothing paused); type-system.md §40.12, c-backend.md
  §3b, testing.md, the guide's async page; `tools/slice_check.py`, in the contract suite natively, under ASan with
  UBSan and under TSan, and in the Windows workflow with `cl`; the emit-c snapshot `adr0189-poll-end`.
- **Deviations from §2, each for a reason found while building it:**
  1. **The switch is file-scope assembly in `ceesafe_rt_async.c`, not a `.S` file** (§2.6, §9 step 1). The compiler
     writes and compiles C files only; an assembler step would be new machinery for every C compiler, and `cl` takes
     no `.S`. It is compiled only under `CEE_POLLS`, so a build without polls has no assembly at all.
  2. **A paused task's stack is kept with its unit, not on the task** (`cee_unit.paused`). The task's record is full
     (192 bytes, the size `ceesafe_task_slot` and the interpreter's `CEE_TASK_HEAD` both assume); a unit has a list of
     its paused tasks' stacks, which the executor looks in only when it is not empty.
  3. **A pause collects what came meanwhile** before the task is requeued (`cee_slice_collect`: descriptors, wakes
     and due timers, without blocking). The executor otherwise looks only when nothing is ready, and two units that
     compute keep its queue full: in the first build the neighbour on a 1 ms timer still waited the whole 300 ms.
  4. **No release of a deep stack's pages when it is given back** (§2.5's `madvise` past 64 KiB): telling a deep resume
     from a shallow one costs a system call at every give-back. The pool keeps at most 16 stacks, so what deep
     resumes touched is bounded by that.
  5. **A nested `Async.run`** cannot be reached from a unit's code: a sync function that runs one may not be called
     from async code (N0295). It is reached only through a C callback, under C's frames, where §2.3 already forbids a
     pause; the depth condition stays as a second guard, and there is no case for it in the check.
  6. **A `Mutex`** can be held by a unit only through `tryLock` (`lock` may block: N0295); the check takes it so.
  7. **Linux x86-64 with the shadow stack on:** `Async.runSlice` is `false` there (`arch_prctl(ARCH_SHSTK_STATUS)`),
     since the switch keeps no shadow stack. Not run: no machine here has it on.
  8. **Windows:** the monitor sleeps on a high-resolution waitable timer, not `Sleep`, whose 15.6 ms tick made every
     slice at least that long (a 1 ms slice paused 12 times in 444 ms of computing). It shortens ADR 0186's lateness
     there too.
  9. **Found on the way, not this ADR's:** `cee_unit_begin` wrote `ceesafe_panic_isolate` at every unit's start, a
     data race between units begun on two workers of `Async.runWorkers` (ThreadSanitizer, in `slice_check`'s
     `workers` case with or without a slice). It is written once now, under an atomic flag, and the other threads wait
     until it is.
- **The debugger** (§2.6's lldb listing of paused tasks) is not done: `tools/debugger_check.py` is unchanged.
- **Checked** with the compiler built by itself (`out/ceesafec`, fixed point):
  - **macOS arm64** (Apple clang 21): every corpus family (run-pass 419, run-fail 55, check-fail 383, emit-c 32 with
    the new snapshot, ffi 22, the rest); run-pass and run-fail with `-DCEE_POLLS=1` and leak checks, 419 and 55; the
    async run-pass programs with polls under ASan with UBSan and leak checks, 60, and under TSan, 60; run-pass and
    run-fail in the interpreter with leak checks, 418 and 55, the one failure `run-pass/async-race-read-loses`, 0.79.2's
    test that times out in the interpreter on `main` too (fixed on `fix/interp-socket-deadline`); `bound_check` 33 of
    33, `isolated_check` 14 of 14, `slice_check` 15 of 15 natively, built for x86-64 under Rosetta, under ASan with
    UBSan, and under TSan; `guide_check` 190 examples, 0 problems.
  - **Linux** (Ubuntu 24.04 in Docker, gcc 13.3 and clang 18, the compiler built there from this compiler's C):
    arm64, `slice_check` 15 of 15 with gcc, with gcc at `-O2 -D_FORTIFY_SOURCE=2 -Wall -Werror` (glibc's checked
    `longjmp`), under ASan with UBSan (LeakSanitizer on) and under TSan; x86-64 (emulated), 15 of 15 with gcc, with
    fortify and under ASan with UBSan. TSan does not run under that emulation (an empty program built with it exits
    66): not run on x86-64 Linux.
  - **Windows 11** (`cl` 19.44, the compiler built from this compiler's C with the workflow's command, no warning):
    `slice_check` 15 of 15 on fibers and `bound_check` 33 of 33, both with `--cc cl`.
- **What it costs** (httpserver's benchmark server, CeeSafe.Web at `79ae138`, built with `-DCEE_POLLS=1`, one process,
  `CW_ISOLATED=1`, `h2load --h1 -c 64 -t 4 -D 6`, 3 rounds with the settings in turn, the machine's load about 6, no
  failed request; the median and its range): `/small` 80 756 (79 806–82 518) requests a second with no slice, 78 934
  (78 018–79 302) with a 10 ms slice, −2.3%; `/gz/16384` with gzip 4 393 (4 221–4 421) and 4 325 (4 303–4 351), −1.5%.
  Each isolated resume runs on a pooled stack there; the difference is near the runs' spread.
- **What a neighbour waits** beside `/spin/300` (a request computing 300 ms) on the same server, its `/small` timed by
  curl: 247 ms with no slice; 87 to 149 ms with a 10 ms slice, 39 to 42 ms with 2 ms, 29 to 30 ms with 1 ms. A
  request is several resumes (accepted, read, handled, written), and each comes after a slice of the one that
  computes, so its wait is a few slices, not one: a server that wants short waits sets a short slice. Throughput was
  measured with a 10 ms slice only; a 1 ms slice pauses ten times as often, and is not measured.
