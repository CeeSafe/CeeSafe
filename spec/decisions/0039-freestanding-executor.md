# ADR 0039 — L1 A4c: an executor for freestanding code, and a CosmoOS boot test

- Status: **Accepted** by the owner, 2026-11-01 (every question as proposed). Phase 5, the last of what is left of
  L1 A4 (ROADMAP, the owner's "in a multiphased way").
- Scope: running async tasks where there is no operating system below, as in a kernel, through hooks the environment
  provides. It covers what the freestanding profile allows of §40, and a CosmoOS self-test that runs tasks at boot.

## 1. Context

In the freestanding profile (profiles.md §4) async fns compile, but nothing can run them: `Async` is N0293 ("needs
the hosted runtime"). The hosted executor (`runtime/hosted/ceesafe_rt_async.c`) mixes two kinds of code:
- portable bookkeeping: the ready queue, the deadline heap, the wake list, scopes, cancellation, shields, joins;
- platform services: `poll`, kqueue, epoll, IOCP, io_uring, pipes, `clock_gettime`, threads, `malloc`.

CosmoOS, surveyed read-only on 2026-11-01:
- **CeeSafe is already in its build.** `scripts/cee-gen.py:183` runs `ceesafec build --profile freestanding
  --no-float --kernel cosmoos`, and CeeSafe 0.2.0 is pinned at `:64`.
- **A boot self-test exists.** `kernel/ceesafe/kcore_test.cee` is registered as `cee-kcore` in
  `kernel/core/selftest.c:583`. `make test` (Makefile:243) runs every self-test under QEMU
  (`tests/boot/run_boot_test.py`), keyed on `SELFTEST: <name> ... ok`.
- **The kernel has what an executor needs:**
  - a monotonic clock, `clock_now_ns` (`kernel/include/kernel/timer.h:63`, lock-free);
  - wait queues: `wait_event_timeout` (`wait.h:182`), with wakers safe in IRQ context (`wait.h:10-11`);
  - `complete()`, callable from IRQ (`completion.h:4`);
  - timers whose callbacks run in IRQ context (`timer.h:255-271`).
- No `cosmoos-kernel` or `cosmoos-user` profile exists yet in CeeSafe, so the kernel builds with `freestanding`.

## 2. Options

| | A. The executor in the core runtime, with environment hooks (proposed) | B. Frames only: the environment drives `ceeAR_` itself | C. Keep N0293 until a cosmoos profile |
|---|---|---|---|
| Runtime | the portable part of the executor moves to `runtime/core/ceesafe_rt_exec.c`, compiled in every profile; the hosted file keeps the reactors | none | — |
| The environment gives | `now`, `idle(deadline)`, `wake()`; and memory, through `Async.scopeIn`'s allocator | a loop calling resume functions, with its own timers and scopes | — |
| Language in freestanding | `Async.run`, `scopeIn`, `spawn`, `race`, `all`, `shield`, cancellation, `Timer.sleep`, an IRQ-safe `AsyncSignal` | `async fn` and `await` only: no scopes, no timers | async fns compile; nothing runs them |
| Cost | splitting the executor, a hook ABI to keep stable | each environment rewrites scheduling, cancellation, scopes | — |

**A is proposed.** Scheduling, scopes and cancellation are the hard, tested part (cancel sweep, TSan), and an
environment should not rewrite them. The hooks are the three things only the environment knows: the time, how to idle,
and how an interrupt or another thread ends the idling.

## 3. Proposed decision

1. **The split.** The executor's portable part moves to `runtime/core/ceesafe_rt_exec.c`: tasks, the ready queue, the
   deadline heap, wakes, scopes, joins, shields and cancellation. It compiles in every profile. In the hosted profile
   `runtime/hosted/ceesafe_rt_async.c` implements the hooks with its reactors, and adds descriptors, operations
   (ADR 0035), workers (ADR 0038) and `Async.blocking`. Nothing hosted changes behaviour, and the hosted contract suite
   is the check.
2. **The environment's hooks**, declared in `rt.h`, defined by a freestanding environment:
   - `uint64_t ceesafe_env_now_ms(void)`: a monotonic clock in milliseconds;
   - `void ceesafe_env_idle(void *executor, uint64_t deadline_ms)`: block the calling thread until the deadline
     (`UINT64_MAX`: none) or a wake;
   - `void ceesafe_env_wake(void *executor)`: end that idling. It must be safe from an interrupt handler and from
     another thread, and a wake before the idle must not be lost.
   A missing hook is a link error, as for `ceesafe_panic` (profiles.md §4.2).
3. **What freestanding code may use of §40:**
   - `Async.run`, `await`, `Async.scopeIn(alloc, …)` (the system heap does not exist, so `Async.scope` stays N0293),
     `spawn`, `race`, `all`, `shield`, `checkpoint`, `isCancelled`, and `Async.boxed` with a user allocator;
   - `Timer.sleep` and `Timer.now`, moving from lib/std to lib/core and implemented over the hooks;
   - **`AsyncSignal`**, a new lib/core type. `await signal.wait()` parks the task until `signal.raise()`, which is
     `@effects()`, so an `@context(irq)` handler may call it. It is a flag plus the waiting task, and the hook wakes
     the executor. It is how a driver's interrupt resumes a task.
   - Still hosted only (N0293): sockets, console and file I/O, `Async.blocking`, `spawnSend` and `runWorkers`,
     channels.
4. **Contexts.** An async fn's body forbids `sleep` (§40.1), so a kernel task cannot call a blocking kernel function:
   the kernel's own blocking primitives are `sleep` (CosmoOS's bindings say so), and the task waits through
   `AsyncSignal` instead.
5. **The CosmoOS boot test.** A self-test `cee-async`:
   - `kernel/ceesafe/async_test.cee` runs a scope of tasks that sleep, race, are cancelled, and wait on an
     `AsyncSignal` that a kernel timer callback (IRQ context) raises;
   - C glue implements the three hooks with `clock_now_ns`, a wait queue and `wait_event_timeout`;
   - it is registered in `kernel/core/selftest.c` beside `cee-kcore`.
   Gate: `make test` prints `SELFTEST: cee-async ... ok` and `SELFTEST: PASS`. Where the change is made in CosmoOS is
   question 3.
6. **Tests in CeeSafe:**
   - a freestanding harness in `tools/freestanding_check.py`: a host C file implements the hooks with
     `nanosleep`/`clock_gettime` and a thread raising a signal, and links a freestanding object, so the freestanding
     executor runs on every contract host;
   - the cancel sweep over it;
   - check-fail for N0293 on what stays hosted (`Async.scope`, a socket, `spawnSend`) and for `AsyncSignal.wait` in a
     sync fn.

## 4. Consequences

- Runtime: the split, the hooks, `AsyncSignal`'s runtime part. lib/core gains `Timer` and `AsyncSignal`, and lib/std
  keeps the rest of `async.cee`.
- The checker's N0293 list (profiles.md §4.1) gets shorter by exactly §3.3's list. The C backend links
  `ceesafe_rt_exec.c` whenever the program uses `ceesafe_async_`.
- Spec: profiles.md §1 and §4.1–4.2, type-system.md §40.4 and §40.8, c-backend.md §6.
- CosmoOS needs a `ceesafec` that has this, so its pin (`scripts/cee-gen.py:64`, 0.2.0) moves: question 4.

## 5. Questions for the owner

1. The approach, A (the executor in the core runtime, with three environment hooks)? **Yes.**
2. Freestanding's share of §40, as §3.3 lists it, with `AsyncSignal` as the interrupt-to-task bridge? **Yes.**
3. **Where the CosmoOS change is made.**
   - (a) On a new branch in a separate git worktree of `~/dev/CosmoOS`, leaving your working tree and its untracked
     files alone; you review and merge. (Proposed.)
   - (b) As a patch kept in CeeSafe (`tests/cosmoos/`), which you apply.
   - (c) Not in this phase: the CeeSafe-side harness only.

   **(a).**
4. CosmoOS pins ceesafec 0.2.0. On that branch, should the pin move to this tree's ceesafec (0.3.0-dev, built from
   the commit that adds this)? Or do you want a release first? **Move it to this tree.**

## 6. Implementation (2026-11-01)

- **The split** (§3.1) is by `CEE_FREESTANDING` within `runtime/hosted/ceesafe_rt_async.c`, not a file of its own in
  `runtime/core`. The file was already one executor with platform branches (POSIX, Windows, kqueue, epoll,
  io_uring), and freestanding is one more. The portable part is what every branch keeps: the queue, scopes, joins,
  shields, cancellation and `AsyncSignal`. The backend embeds the file, with the define, in a freestanding program
  that uses async. Built that way, its object has no `.data`, `.bss` or TLS. Its undefined symbols are the hooks,
  `ceesafe_panic` and `memset`.
- **Hooks** (§3.2): there are **four**, not three. `void **ceesafe_env_slot(void)` names the calling thread's
  executor, since the profile has no thread-local storage and no mutable globals (profiles.md §4.4). `Async.run`
  keeps the executor in its own frame and puts it in the slot. The wake order and the root the sweep cancels became
  the executor's fields. With no heap for its arrays, the freestanding executor finds deadlines by walking its parked
  tasks. A stuck executor, or one asked for a system-heap child, panics with the new `CEE_PANIC_ASYNC`.
- **Library** (§3.3): lib/core's `async.cee` holds `AsyncError`, `Timer`, `AsyncControl`, `AsyncLeaf.status`,
  `Raced` and the joins, moved from lib/std, and `AsyncSignal` (`@noncopy`, four words: raised, the waiting task, its
  executor, a raise in progress). lib/std keeps the console, sockets and files, its own steps as `AsyncIo`. The
  `IoError` type is allowed in freestanding code, since `AsyncError.Io` names it, but making one stays N0293.
  `Async.scope`, `Async.blocking`, `Async.runWorkers` and `spawnSend` are N0293 there.
- **AsyncSignal's protocol:** `raise` marks itself `busy`, sets `raised`, takes the waiting task by swapping the
  `task` word to 0, wakes it, and clears `busy`. A take that loses that race waits until `busy` clears, then keeps
  the raise for its next take. So no raise touches a task that may have returned. A raise only sets the task's woken
  flag and pokes its executor, never its queues, as it may be an interrupt's. `AsyncSignal` works in the hosted
  profile too (another thread, `Async.blocking`'s worker) and in the interpreter.
- **Found by the CosmoOS boot test:** the executor asked for a child's block with `sizeof(cee_max_align_t)` as the
  alignment, and rounded the task head the same way. On x86-64 that is 32, but the type's alignment is 16, which
  CosmoOS's `kmalloc` gives and a host's `posix_memalign` hid. It now uses `_Alignof`, on every profile.
- **The freestanding tool:** `tools/freestanding_check.py` allows the four hooks. It allows `bzero` on a Darwin host,
  where LLVM makes a zeroing `memset` a call of `bzero` whatever the flags. It now implements profiles.md §4.4's
  exception for a program's own static items, so `main.c` may have `.bss`, while the runtime may not.
- **CosmoOS** (§3.5, question 3: a worktree branch): branch `ceesafe-async-boot` of `~/dev/CosmoOS`, in
  `~/dev/CosmoOS-ceesafe-async`. It adds:
  - `kernel/ceesafe/async_test.cee` and `async_selftest.c`, the hooks over `clock_now_ns`, a wait queue with a
    pending flag, and `waitqueue_wake_all`. A kernel timer's callback raises the test's `AsyncSignal` in interrupt
    context.
  - the self-test `cee-async`, registered after `cee-kcore`;
  - the async runtime file in `scripts/cee-gen.py` (emitted only by ports that use async) and in `kernel/kernel.mk`;
  - the pin moved to `0.3.0-dev` (question 4).
  The glue keeps one slot for the kernel, enough for the self-test. It panics if a second thread runs tasks while one
  does; a per-thread slot comes with the first driver that runs tasks.
- **Tests:**
  - freestanding/async with its harness `async.c`: the hooks over a mutex and a condition variable, and a thread
    raising the signal as an interrupt would. It runs on the host, and compiles for both CosmoOS triples with the
    strict warnings and with `ld.lld`;
  - run-pass/async-signal, in C, in the interpreter and under ThreadSanitizer;
  - check-fail/freestanding-hosted-names: N0293 for `Async.scope`, `Async.blocking` and `Async.runWorkers`, and the
    `IoError` type now accepted;
  - check-pass/effects-of.hir: closure numbers +8 by a reviewed edit, because lib/core gained closures;
  - CosmoOS `gmake test` on x86_64: `SELFTEST: cee-async ... ok (28 ms)`, `SELFTEST: PASS (525 tests)`, `boot-test:
    PASS`.
