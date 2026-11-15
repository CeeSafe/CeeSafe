# ADR 0148 — Windows: timers that end on time

- Status: **Accepted** by the owner, 2026-11-15: "Go ahead and do the works required in CeeSafe and cut a release",
  for CeeSafe.Web's upstream items, this one U21 (`~/dev/CeeSafe.Web/docs/ceesafe-upstream-work.md`, found 2026-11-05
  with ceesafec 0.11.0 on Windows 11 ARM64). Implemented on branch `ceesafe-0.52.0`.
- Scope: the hosted runtime's executor on Windows (`runtime/hosted/ceesafe_rt_async.c`: `ceesafe_async_now`,
  `cee_wait_windows`, `cee_iocp_drain`, the executor's begin and end); ADR 0055's note. No change to the language,
  lib/, the compiler, the interpreter or any other system.

## 1. Context

- **The clock.** `Timer.now()` is `ceesafe_async_now`, which on Windows read `GetTickCount64()`: it moves once a
  system tick, about 15.6 ms. A 1 ms `Timer.sleep` woke, found the clock unmoved, and waited again.
- **The waits.** The executor waits with the deadline as the wait's own timeout:
  `GetQueuedCompletionStatusEx` on its completion port (IOCP, ADR 0035, the default), `WaitForSingleObject` on its
  event, or `Sleep`. Each of those timeouts ends at a tick too, whatever the clock says.
- **Measured by CeeSafe.Web** (2026-11-05, ceesafec 0.11.0, the Windows VM): 180 `Timer.sleep(1)` took 4,485 and
  4,547 ms (about 25 ms each), and 203 ms on macOS. Two of its tests had assumed millisecond timers and failed on
  Windows until their timing was made robust.

## 2. Decision

- **The clock is the performance counter:** `QueryPerformanceCounter` over `QueryPerformanceFrequency`, in ms, as
  `Clock.monotonicNanos` already reads it (`ceesafe_clock_monotonic`).
- **A deadline ends the wait through a high-resolution waitable timer** (`CreateWaitableTimerExW` with
  `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`, Windows 10 1803 on), which is signalled within about a millisecond.
  Before each wait with a deadline, the executor sets it for that deadline, and cancels it after:
  - with its event: `WaitForMultipleObjects` on the event and the timer;
  - with nothing to wait for but the deadline: `WaitForSingleObject` on the timer, in place of `Sleep`;
  - on its completion port, which cannot be waited on with another handle: the thread pool waits on the timer
    (`CreateThreadpoolWait`, `SetThreadpoolWait`), and its callback posts an empty completion to the port. The port
    reads it as another thread's wake. A post that comes after the wait ended for another reason ends a later wait
    early, and the executor recomputes its deadline, as after any early return; the port is the thread's for the life
    of the thread, so the post never names a closed one.
- **The sockets' polling slices keep the tick.** A socket tied to another thread's port is polled between slices of
  1 ms of the port's own timeout (ADR 0035). The timer is set for the deadline, not for the slice, so a deadline ends
  the wait on time while the slices stay as cheap as they were.
- **The timer and the pool's wait are the executor's:** made at its first deadline, closed when it ends (after
  `WaitForThreadpoolWaitCallbacks`, so no callback runs on a closed wait). Where the timer cannot be made (before
  Windows 10 1803), waits keep the tick.
- **Not taken:**
  - *An APC* (`SetWaitableTimer`'s completion routine, ending an alertable wait). A high-resolution timer takes no
    completion routine: `SetWaitableTimer` with one fails (200 of 200 calls, §4), and a timer without the flag fires
    at the tick.
  - *`timeBeginPeriod(1)`*, which raises the resolution of every wait of the process (and of the whole system before
    Windows 10 2004), at a cost in power, and is not guaranteed for a process Windows 11 deems invisible.
  - *`NtAssociateWaitCompletionPacket`*, which ties a waitable object to a port directly: an undocumented native API.

## 3. Consequences

- On Windows, a short `Timer.sleep`, `Async.timeout` or `Async.deadline` ends about when it says, as on Linux and
  macOS. `Timer.sleep(ms)` still waits at least `ms`.
- A wait with a deadline costs a timer's set and cancel, and on the port a thread-pool wait's set and a post.
- Code that measured Windows' tick (for example a test that allowed 25 ms for a 1 ms sleep) is unaffected: it gets
  less than it allowed.

## 4. Tests

Run on 2026-11-15 on the Windows 11 ARM64 VM (Parallels, Windows build 28000, Visual Studio 2026's MSVC and LLVM's
clang). The VM was under load from its host that day: `Sleep(1)` alone took 40 to 87 ms there, so every figure below is
the VM's, and only figures from the same minutes are compared.

- **A probe of the primitives** (`hrtimer.c`, 200 waits of 1 ms each; kept with the session, not in the repository):
  - `SetWaitableTimer` on a high-resolution timer with a completion routine: 200 of 200 calls failed.
  - The high-resolution timer waited on alone: 653 and 690 ms; with an event (`WaitForMultipleObjects`): 725 ms.
  - The thread pool waiting on it and posting to a port read by `GetQueuedCompletionStatusEx`: 865 ms.
  - In the same runs: `GetQueuedCompletionStatusEx` with a 1 ms timeout, 3,583 to 10,702 ms; a timer without the
    flag, about 10,000 ms.
- **A program of `Timer.sleep`** (200 of 1 ms, 50 of 5 ms, and 200 of 1 ms while another task waits on a UDP read):
  - built by ceesafec 0.46.0 (the tick): 9,794, 1,814 and 10,915 ms; again, 10,460, 1,551 and 12,166 ms;
  - built by this tree's ceesafec, with clang: 712, 536 and 729 ms; with `cl`: 740, 646 and 713 ms;
  - ten more runs of this tree's builds: 692 to 966 ms for the first figure, 708 to 1,331 for the second, 696 to 951
    for the third. One earlier run, while the VM was at its busiest, took 8,762 ms, as long as the tick's;
  - on macOS arm64, the same program: 231, 279 and 228 ms.
- **The corpus's async programs on the VM** (`tools/run_corpus.py --family run-pass --check-leaks --jobs 4` with this
  tree's ceesafec, filtered by `async`, `timer`, `sleep`, `socket`, `udp`, `tcp` and `channel`): every one passed but
  `run-pass/async-move-timeout-race.cee`, which races 1 ms sleeps against 50 ms deadlines and failed once at four jobs
  on the loaded VM; alone it passed twice, and the `async` and `timer` sets run again at four jobs passed whole
  (56 and 1 files, 0 failures).
- Not run on Windows: the whole corpus, the contract suite, x64 (the release workflow builds and smoke-tests win-x64
  and win-arm64 at the tag).
