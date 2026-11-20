# ADR 0186 — A bound on how long a task runs without waiting

- Status: **Accepted** by the owner, 2026-11-20 ("I accept all nine §8 questions as recommended"); implemented,
  released in ceesafec 0.80.0 (§10). Proposed the same day. Written at the owner's request ("Yes", to drafting the preemption ADR), from
  the review of httpserver against Erlang's model that led to ADR 0175. ADR 0175 §3 left this open: "A task that
  computes without awaiting still holds its worker, isolated or not. A bound on a task's computing time (a check
  on loop back-edges that traps a task past its bound, which this ADR would then end alone) is its own ADR."
  This is that ADR. Drafted as ADR 0179 and renumbered, that number and the six after it having been given to
  CeeUsb's findings (0.79.0) meanwhile. Its claims about the compiler and the runtime were then checked against
  `main` at 0.79.0 (`0056e9d0`) and corrected where they were wrong (§1), and its design revised where the owner
  left that open ("Feel free to modify the given for best implementation to reach our goal"): where polls go
  (§2.2), the poll word, the monitor's registry, the interpreter's rule and the unit's name. A probe with polls
  added by hand to generated C (§5.1) gave the first costs; the measurements of §9 replace it.
- Scope: NIR and the C backend (polls at loops and at the entry of functions that may recurse, §2.2), the hosted
  runtime (a monitor thread and the executors it watches; the executor's resume count and poll word,
  `runtime/hosted/ceesafe_rt_async.c`; the poll macro in `ceesafe/rt.h`; the panic path's new kind), lib/core
  (`Async.runBound`, `PanicKind.Overran`) and the checker's isolated spawns (`src/check/async.cee`), the interpreter;
  type-system.md §40 (a new §40.11), §14.5, diagnostics.md, `spec/api/core.md`, the guide's async page. No change to
  the grammar. **A program built without `CEE_POLLS` does not change at all**: not its code, not its speed (§5.1
  checked this for the macro: its empty form compiles to the same instructions).

## 1. Context

- **The rule today.** Scheduling is cooperative (§40): a task runs until it awaits. A task that computes without
  awaiting, a loop that never ends or a parse of a hostile input that is quadratic, holds its thread, and every
  task of that thread waits for it.
- **What it costs a server.** httpserver runs one event loop a worker process. A request that spins holds every
  connection of its worker. The only remedy is the supervisor's: a worker whose heartbeat stops for `stallMs`
  (5 s by default; the benchmark server's `CW_STALL_MS` is 2 s) is killed and started again, with all its
  connections. ADR 0175 made a request that *traps* end alone. A request that *spins* still costs the worker. This
  is now the largest gap between httpserver and Erlang, whose scheduler takes the CPU from a process after a count
  of reductions.
- **Why CeeSafe cannot simply switch tasks.** Erlang's scheduler can stop a process anywhere because a process's
  state is always on its own heap. A CeeSafe task can only be suspended at an `await`: between two awaits its
  state is on the C stack, in the frames of the sync functions it called, and those frames cannot be moved
  aside. To make every loop a point where the task may be suspended, the compiler would have to turn every
  function that loops into a coroutine, and every loan held across a loop would become a loan across a
  suspension point (N0349's rules). That changes what programs mean, not just what they cost.
- **What exists to build on**, as checked against the code:
  - ADR 0175: a task of an isolated unit that traps ends alone, its locks poisoned, its set told. A trap is
    therefore something a server can survive, if the task that is trapped is in a unit.
  - **The executor is thread-local.** Each thread's executor is `static _Thread_local cee_executor cee_exec`
    (`ceesafe_rt_async.c:261`; freestanding, its environment's slot). It knows which task runs (`cee_exec.current`,
    set and cleared around each resume in `cee_exec_loop`, `:3187`). **Nothing lists the executors of the
    process.** The one list there is, `cee_run.executors[CEE_MAX_WORKERS]` (`:301`), holds the workers of one
    `Async.runWorkers` run, under that run's lock, so that a worker can poke the others; a plain `Async.run`, which
    is also what each `Thread` that runs async code has, is in no list at all (`cee_exec.run` is NULL). The monitor
    therefore needs a registry of its own (§2.2, step 3).
  - **NIR computes dominators and natural loops** (`controlFlowOf`, `src/nir/control_flow.cee`), before C is
    written, and the C structurer reads its loop headers (`c_structurer.cee:289`). Two things the draft missed:
    - A natural loop exists only in a reducible graph. `controlFlowOf` marks an irreducible one
      (`isReducible = false`, on a retreating edge that is not a back edge) and the structurer then emits the
      function flat (`c_structurer.cee:183`). In such a graph a cycle can have no header in `loopHeaders`, so "a
      poll at each natural loop's header" could miss it. The rule that holds for both is **a poll at the target of
      every retreating edge** in the reverse postorder `controlFlowOf` already computes: in a reducible graph those
      targets are exactly the natural loops' headers.
    - An async fn's resume function is always flat, and enters through a `switch` on its state that jumps to each
      resume label (`c_async.cee:1025`). Those jumps are C's, not NIR's edges, so they make no loop that NIR does
      not see: a loop that awaits is resumed in its middle, and passes its header again on its next iteration. A
      `Poll` that is a NIR statement at the head of a header block is therefore in the right place in structured C
      (inside the `for (;;)`), in flat C (after the block's label) and in a resume function alike.
  - **The panic path already declines** to end a unit alone inside a `Critical` or with C frames between the
    executor and the trap (`cee_isolate_panic`, `ceesafe_rt_async.c:2045`: `t->unit->critical != 0u ||
    ceesafe_entry_depth() != cee_exec.jump_depth`; ADR 0175 §2.3).
  - **How a build is configured by a macro.** The draft named `CEE_LSAN` as a macro "the program is built with". It
    is not: the runtime derives it itself from the C compiler's sanitizer macros (`ceesafe_rt_async.c:32`). The
    precedents are two, and they apply to different code:
    - CeeSafe source is configured with `cfg("NAME")` (ADR 0080 §3.3, ADR 0179), lowered to `#if NAME`, and set with
      `ceesafec build --cflag -DNAME=1`: httpserver's `HTTPSERVER_TRAP_HOOKS` (`if (cfg("HTTPSERVER_TRAP_HOOKS"))`
      in its `src/cache.cee` and `src/gzip.cee`).
    - The runtime is C, and is configured with C's own `#if`. It is emitted with every build (`--emit c` writes the
      `ceesafe_rt_*.c` beside `main.c`) and compiled with the program's flags (`pushCompileFlags`,
      `src/host/toolchain.cee:433`), so it sees a `--cflag -DCEE_POLLS=1`, as it sees `-fsanitize=address`.

    A poll is not CeeSafe source: the backend writes it. So `CEE_POLL` is defined in `ceesafe/rt.h` under a plain
    `#if defined(CEE_POLLS)`, empty otherwise, and `cfg("…")` has no part in it. `cfg("CEE_POLLS")` does fit one
    place: lib/core's `Async.runBound`, which is CeeSafe and can tell with it that the build has no polls and say
    so (§2.1). The interpreter answers every `cfg` that is not a target condition as false (ADR 0179 §2.2), so its
    rule (§2.5) must not depend on the macro.
  - **Nothing in the runtime does what the monitor would do.** The runtime's own threads are `Async.runWorkers`'
    workers (`ceesafe_rt_async.c:3377`), `Async.blocking`'s pool (`:1801`), the signal watcher (`:3885`), the DNS
    thread (`:5904`), and on Windows the thread pool's wait on a deadline timer (`:2496`); there are also service
    threads (`Thread.service`, ADR 0182), which run program code. None reads another thread's executor. The nearest
    is `ceesafec test`'s timeout (ADR 0049 §8, `ceesafe_thread_watchdog`, `runtime/hosted/ceesafe_rt_thread.c:286`):
    a detached platform thread that sleeps once and ends the process with status 124. It is the precedent for the
    monitor's form, a platform thread of the runtime's own that the allocation report does not count, but it
    watches nothing.

## 2. Decision

**Not preemption: a bound.** A task may run for at most a bound, set for the program or for an isolated unit,
between being resumed and waiting again. A task of an isolated unit that runs past it traps with a new kind,
`Overran`, at its next poll, and ADR 0175 ends its unit alone. Nothing is suspended and resumed later: a request
that spins is ended, as Erlang would end a process that a supervisor's timeout gave up on, not paused.

### 2.1 The API

```ceesafe
// The program's bound: how long one resume of a task may last, in milliseconds. 0 (the default) is none.
// The second argument says what happens to a task in no isolated unit (§2.4). False, and nothing started, in a
// native build without polls (`cfg("CEE_POLLS")` false), so a program can tell that it asked for a bound it cannot
// have. The interpreter, whose `cfg` cannot see the macro, takes the call itself and answers true (§2.5).
let bounded: bool = Async.runBound(250, RunBound.Count);

// A unit's own bound, for a child known to compute more (or less) than the program's (question 7).
let id = await set.spawnIsolatedBound(2000, NetError.Crashed, async move () => await serve(stream))?;

Async.overruns()   // u64: how many tasks ran past the bound, whether they were ended or not (§2.4)
```

- `PanicKind.Overran` is the new kind a `TaskCrash` carries (the runtime's `CEE_PANIC_OVERRAN`, 19). The panic line
  names the bound, the time the task had run, and the place of the poll that ended it.
- The bound measures **one resume**: the time from the executor taking the task to the task waiting or
  returning. A request that computes for 100 ms, awaits, and computes for 100 ms again is never past a 250 ms
  bound. A bound on all the CPU time a request may take, over all its resumes, is a different limit (§3).
- **The unit's name.** The draft had `spawnIsolatedWithin`. Its shape fits lib/core, which has no named arguments
  and names a variant's extra leading argument with a suffix: `Async.scopeIn(alloc, oom, body)`,
  `Async.tasksIn(alloc, …)`, `set.spawnIsolated(crashed, child)`. Its word does not: the one time-taking "within" in
  CeeSafe's libraries, CeeSafeLibs' `TlsStream.acceptWithin(stream, config, timeoutMs)`, is a deadline on the whole
  call, as `Async.timeout` is, and `spawnIsolatedWithin(2000, …)` would read as "this child must finish within
  2 s", which is the limit this ADR does not set (§3). `spawnIsolatedBound` and `trySpawnIsolatedBound` use the word
  the program's bound already has (`Async.runBound`, `RunBound`). They are the checker's calls, as `spawnIsolated`
  is (`src/check/async.cee:582`), not only lib/core methods.

### 2.2 How a task is caught

1. **Polls.** In a program built with `-DCEE_POLLS=1`, the generated C has polls placed so that **every cycle a
   task can run passes a poll**, of the control flow graph and of the call graph, with as few polls in hot code as
   that allows. The draft put one at every loop header and at the entry of every function that calls; the probe
   (§5.1) showed what that costs in short loops, so the rules are:
   - **Loops.** A poll at the target of every retreating edge of a function's graph (in a reducible graph, every
     natural loop's header; §1), except:
   - **An innermost counted loop**, a `for` over an integer range or over the elements of a slice, vector or array
     (its end is fixed when the loop starts, and its body cannot change its variable), whose body has no loop of its
     own: one poll before it starts and one every 1 024 iterations, not one an iteration. The backend writes it as
     chunks (`end = min(i + 1024, n)`, the chunk's loop with no poll), so a loop of at most 1 024 iterations polls
     once. The chunks are written under `#if defined(CEE_POLLS)`, so that a build without the macro compiles the
     loop as it does today. Its body may call: a call that can recurse polls itself (below). This is what removed
     `nbody`'s cost in the probe (+9% to −1%, §5.1).
   - **A loop whose every iteration passes another poll** (each path from its header to a back edge goes through
     a poll of an inner loop or a call that polls) needs none of its own. This only saves code; outer loops are
     not where the cost is.
   - **Calls.** A poll at the entry of every function on a cycle of the call graph (a recursion, direct or through
     others), an indirect call (a function pointer, a closure, a `dyn` method) counting as a call to every function
     that may be its target. The draft polled every function that calls. A function on no cycle, whose loops poll,
     reaches a poll or returns within a number of steps its call tree bounds: bounded, if not always small (a tree
     of k levels that each call the next twice runs 2^k calls), which the bound accepts as it accepts a long
     straight-line function.

   A poll is one load and one branch, and passes the place it is at to the slow path only, as every trap does
   (`ceesafe_panic(kind, cee_loc)`):

   ```c
   #if defined(CEE_POLLS)
   extern _Thread_local uint32_t ceesafe_poll_flag;
   #define CEE_POLL(loc) do { if (__builtin_expect(__atomic_load_n(&ceesafe_poll_flag, __ATOMIC_RELAXED) != 0u, 0)) \
       ceesafe_poll_slow(&(loc)); } while (0)
   #else
   #define CEE_POLL(loc) do { } while (0)
   #endif
   ```

   The poll word is a thread-local word, not (as the draft had it) a thread-local pointer to the executor's word:
   that is two dependent loads for the same answer, and the probe found it no cheaper (§5.1). A thread whose
   executor is not watched keeps its word at 0. The monitor writes the word through the address the executor
   registered (step 3); the address of another thread's thread-local is valid while that thread lives. On
   Mach-O the address of a thread-local is fetched through its descriptor once per function call, then kept in a
   register (§5.1); ELF executables use the initial- or local-exec model, which needs no call. Without `CEE_POLLS`
   the macro is empty and a counted loop's chunks are not compiled: after the preprocessor, the C is what it is
   today, and the C compiler has nothing to remove. A freestanding build with `CEE_POLLS` is refused (`#error`,
   §3).
2. **The executor's count.** Each executor keeps a count of the resumes it has started, raised before each resume,
   and the task it is running (`cee_exec.current` already). It reads no clock. A nested `Async.run` reuses its
   thread's executor: its resumes raise the count, so a task that blocks in one is not caught while the inner run
   makes progress, only once a task of the inner run spins.
3. **The monitor.** One thread per process, a platform thread of the runtime's own as `ceesafec test`'s watchdog
   is, started by the first `Async.runBound` with a bound above 0. It reads a **registry of executors** that this
   ADR adds, since none exists (§1): an executor joins it when its `cee_exec_loop` starts while a bound is in force
   (and every running loop joins at the first `Async.runBound`), and leaves it when the loop ends, under the
   registry's lock. `cee_run.executors` stays what it is, a run's list for pokes. The monitor wakes every quarter of
   the smallest bound in force. For each executor it compares the count with the one it saw last time. If the count
   is the same, and a task is running, that task has run without waiting since at least the monitor's previous
   wake. Once the time since the count last changed passes the task's bound, the monitor sets the executor's poll
   word. This is how Go's `sysmon` finds a goroutine to preempt (its `schedtick`), and it costs the executor nothing
   but the count. A task is caught between its bound and its bound plus a quarter of it.
4. **The slow path** (`ceesafe_poll_slow`, on the thread that polled):
   - If the task is in an isolated unit, holds no `Critical` and has no C frame between it and the executor (the
     tests of ADR 0175 §2.3, `cee_isolate_panic`): the poll word is cleared, `Async.overruns()` is raised, and the
     task panics with `Overran`. ADR 0175 does the rest: the unit ends alone, its locks are poisoned and released,
     its set is told.
   - If it is in a `Critical`, or under a `@callback` from C: the word is left set, and the next poll that is
     outside both ends the unit. A task that never leaves the `Critical` is the supervisor's (§3).
   - If the task is in no unit: §2.4.
5. A task that waits or returns clears its executor's poll word, so a word set just as the task waited does not
   end the next one.

### 2.3 Where there are no polls

- **C code** and `extern "C"` calls: a task spinning in a C library, or blocked in a system call, is not caught.
  The poll after the call returns sees the word.
- **`Async.blocking`'s closures**, which run on other threads: they are not on the executor, and hold nothing of
  it.
- **The runtime itself** (channels, the allocator), whose operations are bounded, and the C helpers the backend
  writes for a vector's fill and copy (`for (uint64_t i = …)`), which are bounded by a length.
- **A function marked `@noPoll`** (question 5), for a hot loop that the program's owner has measured and bounds by
  other means. It is not `unsafe`: the worst it can do is hold the thread, as every function does today.

### 2.4 A task in no unit

A task outside any isolated unit cannot be ended alone: its trap ends the process. By default the slow path only
counts it (`Async.overruns()`), writes one line to the panic hook (the task, its bound, the place), and clears
the word for the rest of that resume. The process goes on as it does today, and the supervisor's watchdog is
still the remedy. `Async.runBound(ms, RunBound.Panic)` makes such a task panic, for a program that
would rather end than spin (question 3).

### 2.5 The interpreter

The draft had the interpreter read "the clock" every 1 024 polls. Its clock is virtual (§40.8): timers fire in
deadline order without waiting, and the clock does not move while a task computes, so a task that spins would
never be past its bound. The interpreter instead counts the loop iterations and calls a resume executes (all of
them: §2.2's savings are the C backend's) and ends the task once the count passes a fuel given by the bound: a
fixed number of steps a millisecond, which §40.11 states (question 9). That is deterministic, as §40.8 requires,
and needs no monitor. It does not depend on `CEE_POLLS`, which the interpreter cannot see (§1); `Async.runBound`
is the interpreter's own there (§2.1).

`ceesafec test` builds its harness natively unless given `--interp` (tooling.md §10), so a test of a program's
handling of an overrun is run with `--interp`, or natively with `--cflag -DCEE_POLLS=1`.

## 3. What it does not do

- **No suspension.** A task past its bound is ended, not paused. A way to pause it would need a frame for every
  loop: §1 and §4.
- **No bound on total CPU time** for a request over all its resumes. The same poll could serve it (a unit's time
  added up at each wait, and checked by the slow path), but it needs a clock read at each resume and its own
  questions. It is left to an amendment once this one is measured.
- **Nothing in C code or system calls.** The supervisor's heartbeat and `stallMs` stay, and now cover only that.
- **Not on `Async.runWorkers`' other workers**, whose children ADR 0175 does not isolate: its question 7 left
  isolated children there (`spawnSendIsolated`) to a later ADR. Each worker's executor would register and have its
  own poll word and count, so the monitor would work unchanged; what is missing is an isolated unit to end.
- **Not freestanding** (CosmoOS): there is no monitor thread and no thread-local storage (profiles.md §4.4), and
  a build with `CEE_POLLS` is refused (`#error` in `ceesafe/rt.h`). A kernel could set a poll word from a timer
  interrupt, which is exactly what the word is for, but that is the freestanding runtime's own ADR.

## 4. Options that were weighed

- **Make every loop a suspension point**, so the scheduler switches tasks as Erlang does. Rejected: every function
  that loops becomes a coroutine, frames grow, and loans across a loop become loans across a suspension point
  (N0349). It changes the language for a fault that a bound and isolation already contain.
- **A signal and a jump from its handler** (SIGALRM, or a per-thread timer). Unsound: the jump can land in the
  middle of the allocator, a `Critical`, or a C library, and a handler may call almost nothing (async-signal-
  safety). It also has no Windows equivalent.
- **Go's asynchronous preemption** (a signal and a resume at any instruction, with the compiler's stack maps). Far
  more machinery than CeeSafe's C backend has, for suspension that §1 says the language cannot use anyway.
- **A fuel counter** (decrement at each poll, check at zero) instead of a word the monitor sets. It needs no thread
  and is deterministic, but it is a read, a write and a branch at each poll instead of a read and a branch, and
  fuel is not time: the bound would mean something different on every machine. Kept for the interpreter (§2.5),
  which has no clock that moves while a task computes.
- **One process-wide word** ("some executor is overdue") in place of a thread-local one, the slow path finding
  whose it is. It would need no thread-local storage, and so could serve a freestanding build; but every thread's
  polls would take the slow path while any one task is overdue, and the probe found it no cheaper (§5.1).
- **A poll at every loop header and every function that calls** (the draft). Every cycle passes a poll, but so
  does every iteration of a five-element loop: `nbody` +9%, where §2.2's counted-loop rule gives −1% (§5.1).
- **A countdown in a register** (`if (--tick == 0) { tick = 4096; poll(); }`) in place of the load, so most
  iterations touch no memory. Not measured: it is the same two instructions in the loop as a load and a branch, and
  keeps a register live across the loop; the counted-loop rule gets its benefit where the trip count is known, and
  costs nothing where it is small.
- **Polls always on.** Rejected for now: CeeSafe's performance against Rust
  (`docs/performance-investigation-2026-11.md`, `benchmarks/run.py`) is measured on tight loops. The draft's fear
  was that a load of a word another thread can write stops the C compiler vectorising a loop. The probe found
  that is not where the cost is today: none of the program loops of the six workloads it compiled is vectorised,
  with polls or without. The cost it found is the poll's own load and branch in a short loop that is not counted
  (`parsing`'s byte scans, `while (b[i] != '=') i += 1;`, +22 to +27%; the server's gzip, −12% of its requests a
  second) and, smaller, functions that are no longer inlined (§5.1). A program that wants the bound pays for it;
  one that does not, does not. Question 1 asks whether that should change once §9's numbers exist.

## 5. Consequences

- **A server can lose one request to a spin**, not a worker. httpserver: the worker calls `Async.runBound` when it
  starts (`CW_RUN_BOUND_MS`, say 250 ms under `CW_ISOLATED=1`), and is built with `-DCEE_POLLS=1`. A request that
  loops is reset (HTTP/2, HTTP/3) or loses its connection (HTTP/1.1) after 250 to about 310 ms. Its siblings never
  notice more than that delay, and the supervisor's watchdog is left for C and system calls.
- **A request's latency has a ceiling** that a cooperative scheduler never had: no request waits more than one
  bound behind another that spins.
- **A trap that is a timeout.** Code inside a unit may now trap where it never did before: at any loop or call.
  Everything ADR 0175 says about state half way through a change holds; nothing new is needed, because a poll is a
  place where a trap of any other kind could already happen (an index, an overflow).
- **Cost.** With `CEE_POLLS`, one load and one predictable branch per iteration of a loop that is not counted,
  per 1 024 iterations of one that is, and per call of a function that may recurse; and code that is larger (5.5%
  for httpserver's benchmark server, with a poll in every loop); without it, none. The monitor wakes a few times a
  bound and reads one word per executor. The executor's count is one add per resume. §5.1 has the first numbers:
  nothing measurable on httpserver's small requests, 12% fewer requests a second when it compresses, and up to 27%
  on a byte scan.

### 5.1 The probe (2026-11-20; throwaway, not committed)

**Not this ADR's implementation**, which does not exist: polls added by hand to the C that ceesafec 0.78.1 wrote
(`ceesafec build --emit c`), then compiled with the flags `ceesafec build` uses (`-O2 -ffp-contract=off
-fno-math-errno`), on an Apple M1 (macOS, Apple clang 21.0.0). A poll was put at every `for (;;) {` (a structured
loop's header) and, for the server, also after every label that a later `goto` in the same function jumps back to
(the loops of flat functions, async resume functions among them). There were no polls at function entries, so the
cost of those (§2.2, step 1) is not measured. The slow path was a cold, never-inlined function that was never
called. Variants: **A**, the word is a thread-local `uint32_t` (§2.2); **B**, the draft's thread-local pointer to a
word; **C**, one process-wide word (§4).

- With the macro empty, the assembly of `numeric`'s `main.c` was the same, instruction for instruction, as without
  the patch.
- On Mach-O the thread-local's address was fetched once at each function's entry (a call through its descriptor)
  and kept in a register, and each poll was `ldr w8, [x20]` and a branch.

CeeSafe's benchmarks (`tests/reference/benchmarks/suite`), the median wall time of 15 runs, the builds run in turn;
a rebuild that compiled to the same instructions (only linked differently) moved by up to 2%, so differences of a
few percent are noise:

| Workload | Polls | Without | A (thread-local word) | B (draft's pointer) | C (process-wide) |
|---|---:|---:|---:|---:|---:|
| `numeric` (a sieve, a matrix product) | 7 | 253–271 ms | −4 to −6% | −4 to −6% | −6% |
| `nbody` | 7 | 542–604 ms | +9 to +11% | +7 to +9% | +12% |
| `parsing` (byte scans) | 7 | 177–196 ms | +22 to +27% | +28 to +32% | +27% |

- In `parsing` and `nbody` a function that was inlined without polls (`parsing`'s `number`) was not with them.
  Built with `-mllvm -inline-threshold=2000`, with everything inlined again, the cost was +18% (`parsing`) and +10%
  (`nbody`): most of it is the poll's load and branch in loops of a few instructions (`while (b[i] != '=') i += 1;`).
- C cost no less than A, so the cost is not the thread-local access.
- **§2.2's counted-loop rule**, applied by hand to the innermost counted loops of at most 1 024 iterations (the poll
  moved to before the loop, which is what the rule writes for them): `nbody`'s four loops over its five bodies,
  `numeric`'s two of 320. `nbody`'s loop of 10 million steps, which calls `advance`, kept a poll an iteration (the
  rule would poll once every 1 024), and `numeric`'s sieve `while` its poll. 15 runs:

  | Workload | Without | A, a poll in every loop | A, counted loops by §2.2 |
  |---|---:|---:|---:|
  | `nbody` | 560.9 ms | +9.3% | −0.8% |
  | `numeric` | 266.1 ms | −6.0% | −7.8% |

  `parsing`'s loops are `while` and `loop`, which the rule leaves as they are: its +22 to +27% is what a poll costs
  in a short loop that is not counted.

httpserver's benchmark server (`bench/`, CeeSafe.Web at `79ae138`), one process (no supervisor), a poll in every
loop (492 structured and 407 flat loop headers; not §2.2's counted-loop rule), `h2load --h1 -c 64 -t 4 -D 8`, 5
rounds of 8 s with the builds in turn, all 45 runs with no failed request. The median of requests a second, and its
range:

| Route | Without | The same instructions, relinked | A, a poll in every loop |
|---|---:|---:|---:|
| `/small` | 68 642 (58 083–72 552) | +9.2% | +3.4% |
| `/json` | 72 985 (58 720–80 530) | −1.9% | +2.4% |
| `/gz/16384`, gzip | 4 304 (4 100–4 499) | −3.8% | −12.1% |

- On `/small` and `/json` the relinked build, whose code is the baseline's, differs from it by as much as the polled
  one does: a poll costs nothing that this load can see where a request does little. With gzip, where the time is
  in CeeSafe's deflate (httpserver's `src/gzip.cee`), the polled server answers 12% fewer requests (9% fewer than
  the relinked build). That is the same cost as `parsing`'s: short loops that are not counted.
- **Not measured:** polls at function entries; the counted-loop rule on the server; Linux (ELF's thread-locals,
  and the server's `reusePort` workers); HTTP/2.

## 6. Why it is sound

A poll ends a task only through the panic path, and only where ADR 0175 already ends a unit alone: no `Critical`
held, no C frame jumped over. Everywhere else the word stays set and the task goes on exactly as it would have
without polls. The monitor writes one word and reads two, all atomically; it never touches a task. A poll word that
is set late, or seen late, makes the bound longer, never shorter, so a task is never ended before its bound. A word
set just as a task waits is cleared when it waits (§2.2, step 5), so it cannot end the next task. A task with
`@noPoll` code is only ever slower to be caught. An executor that leaves the registry is no longer read, and its
thread's word stays 0.

## 7. What it costs to get wrong

- **A poll missing from a loop**, so a spin that is not caught: the plan's tests (§9) spin in every kind of loop and
  in recursion, sync and async, in a function whose graph is irreducible too, and each must be caught. §2.2's rules
  that leave polls out are where a miss would hide: a loop taken for counted whose end or variable can change, a
  recursion through an indirect call the call graph did not see. Each rule has tests of its own, and a debug check
  (§9, step 2) for the counted loop.
- **A task ended before its bound**, which would make a correct but slow request fail: the monitor compares counts,
  so it can only be late; the tests check the shortest time a task is ended after.
- **A word left set for the next task**: the test of step 5, with a task that waits on the instant the word is set.
- **The monitor reading an executor whose thread has ended**: an executor leaves the registry, under its lock,
  before its loop returns; the tests end threads and runs while a bound is in force.
- **Polls that cost more than they should**, in a program that turned them on: measured on httpserver's benchmark
  server and on CeeSafe's own benchmarks before this is accepted (§9). §5.1 already shows +22% on a byte scan.

## 8. Questions for the owner

1. Polls only in a program built with `-DCEE_POLLS=1`, with no change otherwise (§2.2)? Recommended: yes, for now,
   and to be asked again with §9's numbers. §5.1's say always-on polls would cost too much as they stand (+22 to
   +27% on `parsing`, 12% of the server's compressed requests), and that a server that turns them on pays little
   unless it compresses. A manifest setting (`[build] polls = true`) could set the macro later, as it could for any
   of the C toolchain's macros.
2. End a task past its bound (an `Overran` trap, ADR 0175 does the rest), and never suspend it (§2, §4)?
   Recommended: yes.
3. A task in no unit is only counted and reported by default, with `RunBound.Panic` for a program that would rather
   end (§2.4)? Recommended: yes: a program that did not ask for isolation did not ask to be ended either.
4. The bound measures one resume, not a request's total CPU time (§2.1, §3)? Recommended: yes, with total time left
   to an amendment.
5. `@noPoll` on a function, for a measured hot loop (§2.3)? Recommended: yes. §5.1 has the loops that need it: a
   deflate's inner loops, bounded by the input that a request's size limit bounds, are where httpserver's 12% goes,
   and they are bounded by other means. It stays a function's attribute, not a loop's, so that what it exempts is
   easy to find.
6. Polls placed by §2.2's rules (a counted innermost loop once every 1 024 iterations; a function's entry only if
   it may recurse), instead of the draft's poll at every loop header and every function that calls? Recommended:
   yes: `nbody`'s +9% became −1%, and every cycle still passes a poll.
7. A unit's own bound, inherited by every task under the unit's root, as `set.spawnIsolatedBound(ms, crashed,
   child)` and `set.trySpawnIsolatedBound(ms, crashed, child)` (the draft's `spawnIsolatedWithin`, §2.1)?
   Recommended: yes, with that name: a connection that serves file downloads computes more between waits than one
   that serves JSON, and "within" reads as a deadline on the whole child.
8. A monitor that is a thread of the runtime, with a registry of executors (§2.2, step 3)? Recommended: yes. The
   other choice, the executor reading a clock at each resume and each poll, puts a system call or a vDSO read on
   every resume of every task.
9. In the interpreter, the bound as fuel, a fixed number of polls a millisecond, because its clock does not move
   while a task computes (§2.5)? Recommended: yes; the number to be stated in §40.11.

## 9. Implementation plan (once accepted)

1. Spec: §40.11 (the bound, the poll, what it ends; the interpreter's fuel); §14.5 (`Overran`); diagnostics.md (if
   `@noPoll` is accepted); `spec/api/core.md`; the guide's async and error-handling pages.
2. NIR and the C backend, by §2.2's rules: a `Poll` statement at the head of the block that each retreating edge
   targets (the natural loops' headers in a reducible graph, from `controlFlowOf` in `src/nir/control_flow.cee`),
   but for innermost counted loops, which HIR's `for` over a range or a slice marks and NIR keeps on the loop's
   header, and which the backend writes in chunks of 1 024 under `#if defined(CEE_POLLS)`; a loop whose every
   iteration passes another poll has none. A `Poll` at the entry of each function on a cycle of the call graph, which
   the backend builds once for the program, an indirect call an edge to every function whose address is taken, every
   closure body of its type and every `dyn` implementation of its method. Written as `CEE_POLL(ceeL_n);` with the
   place's `cee_loc`. `ceesafe/rt.h` defines the macro under `#if defined(CEE_POLLS)`, to nothing otherwise, and an
   `#error` for a freestanding build with it. With debug assertions, a counted loop checks at each chunk that its end
   has not changed.
3. Runtime: the thread-local poll word, the executor's resume count, the registry of executors (joined and left by
   `cee_exec_loop`), the monitor thread; `ceesafe_poll_slow` and its three cases; clearing the word at each wait and
   return; `CEE_PANIC_OVERRAN` and `PanicKind.Overran`; `Async.overruns()`.
4. lib/core: `Async.runBound` (false without `cfg("CEE_POLLS")`), `RunBound`; the checker (`src/check/async.cee`)
   and lib/core: `spawnIsolatedBound` and `trySpawnIsolatedBound`.
5. The interpreter: the count of polls a resume executes, against the bound's fuel (§2.5).
6. Tests:
   - run-pass (and so in the interpreter): an isolated child that spins in a `while`, a `for`, a `loop`, a recursion
     with no loop, a sync helper and an async body, each ended with `Overran` while a sibling that awaits a timer
     every millisecond finishes on time; a child that computes for less than its bound in many resumes is never
     ended; a unit's own bound beats the program's;
   - a spin in a function whose graph is irreducible, if CeeSafe source can make one (an emit-c test that the poll
     is at each retreating edge's target otherwise);
   - §2.2's rules: a counted loop of 10^9 iterations is ended (its chunks poll); one of 5 has one poll (emit-c); a
     `while` that walks a vector is not taken for counted; mutual recursion with no loop, a recursion through a
     function pointer, a closure and a `dyn` method, each ended; a function on no cycle has no entry poll (emit-c);
   - a `Critical` held past the bound: the unit is ended at the first poll after it; a spin under a `@callback` from
     C: ended once back in CeeSafe's frames; a spin in a task of no unit: counted, reported, not ended; with
     `RunBound.Panic`, the process ends with the line;
   - the word set on the instant a task waits does not end the next task (a test that forces the order);
   - threads and runs that end while a bound is in force (the registry);
   - a program built without `CEE_POLLS` is, preprocessed (`cc -E`), the same C as before this ADR, for the corpus's
     run-pass programs and the benchmarks;
   - cost: httpserver's benchmark server, with and without `CEE_POLLS`, and CeeSafe's benchmarks against Rust
     (`benchmarks/run.py`; results in `guide/performance.md` and ROADMAP §10), with every rule of §2.2 (§5.1 measured
     only a poll in every loop, and the counted-loop rule on two benchmarks), and on Linux (ELF's thread-locals) as
     well as macOS. The ADR is accepted, or question 1 changed, on those numbers.
7. httpserver: `CW_RUN_BOUND_MS`, the bench built with `-DCEE_POLLS=1`, a `/spin/{ms}` route, and a check in
   `supervisor_check.py --isolated` that a spinning request ends alone while every worker stays up and other
   requests are answered within the bound. `CEESAFE_FINDINGS.md`: the spin moves from the supervisor to the unit.

## 10. Implementation notes (2026-11-20)

What is there is §2 with §2.2's rules, on the hosted runtime and in the interpreter; §9's steps 1 to 6, released in
0.80.0. Step 7 is httpserver's own (below). It differs from what was accepted in these places.

- **No `Poll` statement in NIR** (§9, step 2). The backend places the polls as it writes C
  (`src/backend/c_polls.cee`), from `controlFlowOf` and a call graph it builds once a program, so that no NIR pass
  (the borrow, move and effect checks, the printer, the interpreter) has a statement to learn. The rules are §2.2's.
- **The counted-loop rule depends on the loop's bounds** (§2.2). A loop counts by its shape in NIR, wherever it came
  from: a header whose one statement is `c = i < end`, `i` raised by one and written nowhere else in the loop, `end`
  never, neither borrowed, no loop inside, one way in; and only in a structured function (an async fn's resume
  function, and every function under `--flat-c`, poll at each header). Then, measured on CeeSafe's benchmarks
  (below):
  - **an end known only when it starts** runs in chunks, as §2.2 has it: an outer loop around it polls and sets the
    chunk's end, the loop tests against that, an end that does not change while it runs, and the outer loop goes
    on only if the chunk ran out first (c-backend.md §3b). A first form, which rewrote the test as
    `i < chunk || helper(…)` with no outer loop, hid the trip count from the C compiler, which stopped unrolling
    and vectorising;
  - **constant bounds at most 1 024 apart** need no poll at all: the loop ends by itself (`stack-alloc` and
    `heap-alloc` +135% with the first form, +1% now);
  - **constant bounds further apart** keep the header's poll: chunked, the C compiler no longer knew the counter's
    range, and checks and divisions it had taken out of the loop came back (`arena` +36% chunked, +2% polled).
- **Not done:** "a loop whose every iteration passes another poll needs none" (§2.2), which only saves code; and the
  debug check that a counted loop's end has not changed (§9, step 2), since a loop counts only once the backend has
  seen that nothing in it writes or borrows its end.
- **A poll reads a process-wide count, not a thread-local word** (§2.2, step 1; §4 had weighed one process-wide
  word and set it aside as no cheaper, which held in loops). On Mach-O a function that touches a thread-local fetches
  its address with a call once each time it runs, so a poll at a recursive function's entry cost a call a call:
  `recursion` +48%. `ceesafe_poll_any` counts the threads whose own word the monitor set; the poll loads it, and the
  slow path reads the thread's word and returns at once if it is 0. While a task is overdue in a `Critical`, or under
  C's frames, every thread's polls take the slow path, which is a call and a load.
- **A counted loop whose bounds are constants at most 1 024 apart has no poll** (§2.2): with the test rewritten,
  the C compiler no longer knew how often `for (j in 0..32)` ran and stopped unrolling it (`stack-alloc` and
  `heap-alloc` +135%). A loop of so few iterations of a body with no loop ends by itself.
- **The poll word holds the epoch of the resume it ends** (§2.2, step 5, made exact). The monitor writes the
  resume's epoch, never 0; the slow path ends the task only if it is the running resume's, and clears it otherwise.
  A write that lands after the task waited, and after the executor cleared the word, so ends nothing.
- **The registry is a slot of each thread's, not a field of the executor** (§2.2, step 3), since a nested
  `Async.run` saves and restores the executor by value. A thread joins it when its first executor loop starts and
  leaves it when that loop ends; a nested run starts a new epoch for the resume around it when it ends.
- **The poll word and `ceesafe_poll_slow` are the core runtime's** (`runtime/core/ceesafe_rt_core.c`), which every
  hosted program links, so that a program with loops and no tasks links too; the async runtime installs its slow
  path (`ceesafe_poll_hook`) before its monitor can set a word.
- **`Async.runBound` is `false` by the runtime's answer** (§2.1, §9 step 4), not by `cfg("CEE_POLLS")` in lib/core:
  the runtime is compiled with the program's flags, and the interpreter, whose `cfg` cannot see the macro, takes the
  call and answers `true`. lib/core has `RunBound` and `AsyncBound` (`lib/core/isolation.cee`); the checker makes
  `Async.runBound` and `Async.overruns` calls of `AsyncBound`'s, as it makes `Async.isCancelled` one of
  `AsyncControl`'s.
- **A task in no unit is reported on standard error, not to the panic hook** (§2.4): the hook must end the process
  or never return (ADR 0088), and this report does neither.
- **The interpreter's fuel** is 10 000 steps a millisecond of the bound (§2.5, question 9), a step being a block
  transition or a call outside a `@noPoll` function. It ends a task at its first step past the fuel, which may be a
  little before the first poll the compiled program would reach (in a `Critical`'s drop, say): `bound_check`'s
  programs are written so that both give the same lines.
- **The panic's text** names the time from the monitor's first sight of the resume, which may be up to a quarter of
  the bound after its start: it is never more than the time the task ran.
- **Tests that are not as §9 listed them.** An irreducible graph: CeeSafe's structured source makes none that I
  found (an async resume function's dispatch is C's, not NIR's), so the rule for it has no test that exercises it.
  "The same C as before, byte for byte" (§9, step 6) cannot be: a poll's place is in the C either way, as
  `CEE_POLL(&ceeL_<n>)` and its location constant. What is checked is the claim behind it, that the code is the
  same: each benchmark's C, compiled without `CEE_POLLS`, gives the same assembly as that C with every poll taken
  out (`tools/bound_check.py`). Seven emit-c snapshots changed, by their new poll lines and the location constants
  numbered after them, nothing else (`arrays`, `async-frame`, `control-flow`, `flat`, `gcd`, `line-directives`,
  `placed-boxes`).

How it was tested (macOS arm64, Apple clang 21.0.0, this branch's compiler):

- `tools/bound_check.py`, new, in the contract suite: 33 cases, 0 problems, three runs.
- The corpus: every family as before (parse-pass 54, parse-fail 45, check-pass 48, check-fail 383, run-pass 418,
  run-fail 55, emit-c 31, ffi 22, frames 4, and the rest), with two new check-fail programs and one emit-c; run-pass
  418 and run-fail 55 again built with `--cflag=-DCEE_POLLS=1` and leak checks; run-pass 418 interpreted.
- **The cost**, built with `--cflag -DCEE_POLLS=1` against the same build without it (§9, step 6). The machine was
  loaded by other programs (load averages from 5 to 117) and `benchmarks/run.py`'s wall times moved by up to 90%
  between runs of the same binaries, so what is recorded is the instructions retired and the cycles of each
  benchmark's process (`/usr/bin/time -l`, the run of the fewest cycles of three), which the load moves much less.
  Geometric means: instructions **+10.2%**, cycles **+5.3%**. The rest is the poll's own load and branch in `while`
  loops (`parsing`'s byte scans, `hash-map`'s probes) and at the entry of recursive functions (`const-flags`'s
  scanner). Built without the macro, the code is the same (`bound_check`).

| Workload | Instructions | Cycles | | Workload | Instructions | Cycles |
|---|---:|---:|---|---|---:|---:|
| `arena` | +13.9% | +2.2% | | `memory-copy` | +10.8% | +10.7% |
| `binary-trees` | +2.5% | +0.6% | | `nbody` | +8.2% | +1.1% |
| `const-flags` | +20.7% | +26.9% | | `numeric` | +1.1% | −8.5% |
| `data-oriented` | +27.0% | +4.2% | | `parsing` | +23.7% | +24.3% |
| `hash-map` | +24.7% | +11.2% | | `recursion` | +34.6% | −0.1% |
| `heap-alloc` | +1.0% | +0.8% | | `small-objects` | +1.3% | +1.4% |
| `large-objects` | +0.6% | +3.3% | | `stack-alloc` | +1.0% | +0.8% |
| `linked-list` | +1.7% | +2.4% | | `string-building` | +5.2% | +15.4% |
| | | | | `vector-growth` | +4.6% | −0.6% |

  httpserver's benchmark server (CeeSafe.Web at `79ae138`, one process, `h2load --h1 -c 64 -t 4 -D 8`, 5 rounds, the
  median of requests a second), with and without polls: `/small` −1.6%, `/json` −1.5% (within the runs' range),
  `/gz/16384` with gzip −6.3% (§5.1's probe, a poll at every loop: −12%).
- **Sanitizers** (macOS): `bound_check`'s `survive` and its `count`, `critical`, `callback`, `stale` and `registry`
  cases built with AddressSanitizer and UndefinedBehaviorSanitizer, and `survive`, `registry` and `latency` with
  ThreadSanitizer: the same lines, no report.
- **Linux** (Ubuntu 24.04 in Docker, arm64, gcc 13.3; the C this compiler emits on macOS, built there with
  `-DCEE_POLLS=1`): `survive`'s seven lines, and `count`, `critical`, `callback`, `early`, `stale`, `registry`,
  `latency` and `panic` as on macOS.
- **Windows** (Windows 11 x64, an i7-4700HQ; `ceesafec` built from the C it emits with `cl` 19.44 and CI's command
  line, warning-free): `bound_check`'s 33 cases pass with `CC=cl`, and again with WinLibs gcc 16.2 (MinGW-w64, UCRT),
  so the monitor's `CreateThread` and SRW lock, MSVC's volatile poll read and `_Thread_local` under both are exercised;
  the same-code check compares `cl /O2 /FA` listings. The corpus's run-pass and run-fail give the same results with and
  without polls, under `cl` (`/DCEE_POLLS=1`) and under gcc (`-DCEE_POLLS=1`). The failures they share predate this
  ADR, as release 0.78.0 shows: `run-pass/slice-bulk` prints wrong values under `cl` (not under gcc),
  `run-pass/io-error-network-kinds` does not finish, and `emit-c/extern-array-parameter`'s snapshot assumes LP64.
  `tools/bound_check.py` runs in `.github/workflows/windows.yml` from now on.
- **httpserver, step 7** (its branch `adr0186-run-bound`, built with this compiler): the benchmark server calls
  `Async.runBound(CW_RUN_BOUND_MS, RunBound.Count)` and has `/spin/{ms}`. `supervisor_check.py --isolated --inherit
  --budget-mb 16` (250 ms, three workers, macOS) passed three times: `/spin` and `/spin/2000` were ended alone after
  0.27 to 0.33 s, where the stall watchdog takes 1.5 s; `/spin/50` was answered; the report counted no crash, stall or
  kill. With one worker, a request on a second connection waited 0.27 to 0.28 s for each of three spins and was
  answered by the same worker, started once. Built without `CEE_POLLS` the same check fails as it should: the spins
  are caught by the watchdog after 1.4 to 1.5 s and the worker is replaced.
