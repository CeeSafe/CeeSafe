# ADR 0055 — Wall-clock time

- Status: **Accepted** by the owner, 2026-11-02 ("Yes, I accept ADR 0055 with your recommendations"): every question
  of §5 as recommended. Asked for by CeeSafe.Web (`~/dev/CeeSafe.Web/docs/ceesafe-upstream-work.md`, item
  U12): an HTTP server with a clock must send `Date` (RFC 9110 §6.6.1), and cookies' `Expires`, logs and certificate
  checks need the real time. CeeSafe.Web's `HttpDate.format(unixSeconds, …)` is written and waits for this.
- Scope: lib/std, the hosted runtime, the interpreter.

## 1. Context

CeeSafe has one clock, the monotonic one: `Timer.now()` (lib/core, `ceesafe_async_now`: `CLOCK_MONOTONIC`,
`GetTickCount64`, or the freestanding environment's `ceesafe_env_now_ms`), in milliseconds from an arbitrary start. It
orders events and measures time, and it never jumps, but it says nothing of the date. Nothing in `runtime/` or `lib/`
reads the system's real time.

The two clocks answer different questions and must stay apart: the wall clock jumps (NTP, the user), so a deadline or
a timeout on it would fire early or never. `Timer` keeps the monotonic clock for every wait.

The interpreter runs tasks on a virtual clock (type-system.md §40.8): its `Timer.now()` starts at 0 and moves only
when no task can run, so its runs are reproducible.

## 2. Options

**What the interpreter answers.**

| | A. A fixed epoch on the virtual clock (proposed) | B. The host's real time |
|---|---|---|
| What | 2000-01-01T00:00:00Z plus the virtual clock's milliseconds | what the native program would read |
| Cost | the interpreter's date is not today's | a program's output under `run --interp` and the corpus differs from run to run |

**A is proposed**: the interpreter is the reference implementation and the corpus's second runner, and its runs are
reproducible now. A program that prints today's date runs natively.

**Precision.** Seconds and nanoseconds (as `timespec`) cost nothing more than milliseconds and lose nothing; the
milliseconds are one call on top.

## 3. Proposed decision

```ceesafe
// lib/std, hosted
public struct UnixTime {
    public seconds: i64;     // since 1970-01-01T00:00:00Z, leap seconds not counted (POSIX time); negative before
    public nanoseconds: u32; // 0 to 999 999 999

    public fn millis(borrow this) -> i64;  // seconds * 1000 + nanoseconds / 1 000 000, rounded toward minus infinity
}

public struct Clock {
    /// The system's real time now. It may jump backward or forward (NTP, the user): measure and wait with Timer.
    public fn now() -> UnixTime;
    public fn unixMillis() -> i64;   // Clock.now().millis()
}
```

- **The runtime:** `ceesafe_clock_realtime(int64_t *seconds, uint32_t *nanoseconds)`:
  - POSIX (Linux, macOS, the BSDs): `clock_gettime(CLOCK_REALTIME)`;
  - Windows: `GetSystemTimePreciseAsFileTime`, from 1601 in 100 ns steps, converted;
  - CosmoOS userland: its `clock_gettime(CLOCK_REALTIME)` if its libc has one, else 0 (1970), which the guide states.
- **Hosted only:** `Clock` is lib/std's (N0293 in the freestanding profile, as for files). A freestanding environment
  hook for the real time is left for when a kernel asks.
- **The interpreter** answers 2000-01-01T00:00:00Z (946 684 800 s) plus its virtual clock (option A).
- **Infallible:** every hosted system above has the call; a failure (none is documented for these clocks) reads as 0.
- **Not in this ADR:** calendar dates, time zones and formatting. CeeSafe.Web formats `Date` itself from Unix seconds,
  and a calendar library is a separate design.

## 4. Consequences

- CeeSafe.Web turns `Date` on; cookies and logs get times.
- Two clocks, named for what they answer: `Timer` (monotonic, for waits and measurements) and `Clock` (the date).
- New diagnostics: none.

## 5. Questions for the owner

1. `Clock.now() -> UnixTime { seconds: i64, nanoseconds: u32 }` and `Clock.unixMillis() -> i64`, in lib/std, hosted
   only, infallible? Recommended: yes.
2. The interpreter answers a fixed epoch (2000-01-01T00:00:00Z) plus its virtual clock, so its runs stay reproducible
   (option A)? Recommended: yes.
3. Calendar dates, time zones and formatting left out, for a later design? Recommended: yes.

## 6. Implementation plan (once accepted)

1. Spec: §26 (host services) or §40.4 beside `Timer`; tooling.md's interpreter section (the epoch); the guide.
2. Runtime: `ceesafe_clock_realtime` in `ceesafe_rt_io.c` (every hosted program links it); `tools/gen_runtime.py`.
3. lib/std: `UnixTime`, `Clock`; `tools/gen_library.py`.
4. The interpreter's model: the epoch plus the virtual clock.
5. Tests:
   - corpus run-pass: under the interpreter the epoch, and `millis()` of negative and positive times against values
     worked out by hand (the rounding toward minus infinity);
   - a native-only check beside the contract suite: `Clock.now()` within a second of Python's `time.time()` on macOS,
     Debian and Windows, and nanoseconds below 10⁹;
   - check-fail: `Clock` in the freestanding profile (N0293).
6. ROADMAP §10.

## 7. Implementation notes (2026-11-02)

- **Freestanding:** lib/std is not loaded in the freestanding profile, so `Clock` there is an unknown name (N0200), as
  `Directory` and every lib/std type are, not N0293 as §3 said; check-fail/freestanding-hosted-names shows it.
- **The runtime** is `ceesafe_clock_realtime` in `ceesafe_rt_io.c`; on Windows it counts from 1601 in 100 ns steps
  and gives 1970 for an earlier time, which no system reports.
- **The interpreter** writes 946684800 plus its virtual milliseconds' seconds, and their remainder as nanoseconds.
- **Tests:** run-pass/clock (`millis()` rounding toward minus infinity, worked out by hand; `Clock.now()` after 2000,
  natively and in the interpreter); `tools/clock_check.py` in the contract suite (the native clock within two seconds
  of Python's `time.time()`, the interpreter's epoch exactly, and after a 1500 ms virtual sleep).

