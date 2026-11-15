# ADR 0145 — Waiting for any of a program's windows

- Status: **Accepted** by the owner, 2026-11-15. ClayWind's roadmap step P3.4 opens several windows, and a host that
  sleeps on one window's `waitEvent` (ADR 0142) leaves the input of the others queued until that window has an event.
  The owner chose "Window.waitAny (Recommended)": "New ADR 0145: a static Window.waitAny(timeoutMs) that returns when
  any open window of the program has an event or was woken. waitEvent keeps its meaning; the host then drains each
  window's nextEvent. Built on ADR 0142's registry of open windows. Release as 0.51.0." Implemented on branch
  `adr/0145-window-wait-any`, released later.
- Scope: `lib/std/window.cee`, `runtime/hosted/ceesafe_rt_window.c`, the interpreter's model of the new call
  (`src/interpreter/io.cee`), type-system.md §26.13, `spec/api/std.md` (generated), `tools/window_check.py`, the
  corpus. No language or emitter change.

## 1. Context

- **One window's wait.** `w.waitEvent(timeoutMs)` (ADR 0142) returns when `w` has an event or was woken. With two
  windows open, a host that waits on the first sleeps through the second's clicks and keys, and through a wake meant
  for it; a host that waits on each in turn with a short timeout ticks again, which ADR 0142 removed.
- **One window's pump takes every window's events (macOS).** AppKit has one event queue per application. The pump
  that `nextEvent`, `waitEvent` and `waitFrame` run for a window `w` dequeues every event and queued only those whose
  `NSWindow` is `w`'s; a pointer, wheel or key event of another window of the program was handed to `sendEvent:`,
  which gives it to that window's view, which ignores it. So with two windows, the input of one was lost whenever the
  other was polled first: a bug since the first window could have a second one, and the multi-window case ADR 0145
  makes ordinary would meet it at once.
  - Windows is not affected: `PeekMessageW(&message, NULL, …)` dispatches every message of the thread to its window's
    procedure, which finds its `cee_window` from the `HWND` (`GWLP_USERDATA`). X11 is not affected: each window has
    its own connection (`XOpenDisplay` at each `open`), so one window's pump never reads another's events.
  - The macOS delegate's callbacks (close, resize, focus) and the content view's text input methods already found
    their window from the `NSWindow` (`cee_mac_owner`). The pump's check of the backing scale after its loop looked
    at `w` only.

## 2. Decision

### 2.1 The API (lib/std)

```ceesafe
var main = Window.open("Main", 800, 600)!;
var tools = Window.open("Tools", 240, 600)!;
loop {
    Window.waitAny(1000);                     // until either window has an event, either is woken, or a second
    loop {
        match (main.nextEvent()) {
            Some(event) => { /* main's event */ }
            null => { break; }
        }
    }
    loop {
        match (tools.nextEvent()) {
            Some(event) => { /* tools' event */ }
            null => { break; }
        }
    }
    // results from channels, then redraw what changed
}
```

- **`Window.waitAny(timeoutMs: u32)`**, a static function, returns when any window the program has open has an event
  queued (its `nextEvent` would return one), when any of them was woken (`WindowWaker.wake`) since a wait on it last
  returned, or after `timeoutMs` milliseconds, whichever comes first; 0 returns at once. Like `waitEvent` it does
  not tick, and while it sleeps every window takes the system's events (repaints, the clipboard's requests, AppKit),
  so a system event that gives the program none does not end it. It says nothing about which window is ready: the
  host drains each window's `nextEvent`, and reads its channels, after every return.
- **Wakes.** When it returns, it takes the wake of every open window, as `waitEvent` takes its window's. ADR 0142's
  lost-wake rule holds for each window: a wake that comes while the program is busy is kept until the next wait that
  includes the window (`waitAny`, or that window's `waitEvent` or `waitFrame`). `waitEvent` and `waitFrame` keep their
  meaning: they take their own window's wake only, so a wake of another window is still pending for the next
  `waitAny`.
- **No window open.** `waitAny` sleeps for `timeoutMs`, and nothing ends that sleep early: a waker of a dropped window
  does nothing (ADR 0142). A sleep, not an immediate return, because `waitAny` is the bound of a loop's rate: a host
  that closed its last window and loops on `waitAny(16)` until it notices would otherwise spin a core, and the call's
  one rule ("returns at an event, a wake or the timeout") stays true with no window.
- **Another thread.** No window can be open on any thread but the program's first (`open` is `NotMainThread`, §26.13),
  so on another thread `waitAny` is the case of no window: it sleeps for `timeoutMs`, touches no window, and takes no
  wake. That keeps the function total (no `Result` for a case that has one meaning), and the main thread's windows
  are never pumped off it, which AppKit forbids.
- **Effects.** `@effects(sleep)`, as `waitEvent`.

### 2.2 Routing the events (macOS)

- The pump finds the window each event is for: `cee_mac_owner` of the event's `NSWindow` (the registry of the
  program's AppKit windows the delegate uses). An event without a window (a pointer move outside every window, the
  runtime's own wake event) goes to the window being pumped, as before. An event of a window that is not the
  program's (a panel of the system's) goes to `sendEvent:`, as before.
- So every window's pump takes every window's events into their own queues, and the window that was polled first no
  longer swallows the other's input. After its loop, the pump checks the backing scale of every open window, not
  only its own.
- Windows' pump also checks every open window's scale (the window procedure already routes the messages).

### 2.3 The runtime

- **The call:** `ceesafe_window_wait_any(timeout_ms)`. On another thread, or with no window open, it sleeps for the
  timeout. Otherwise it loops until its deadline, over ADR 0142's list of open windows (only the main thread changes
  it, so the main thread walks it without the lock; the flags are read under the lock):
  1. it returns if any window has an event queued, or was woken;
  2. it pumps every window (a scripted one moves its script's due events into its queue, a system one takes the
     system's pending events), and returns if any window has an event then, or a scripted window's script still holds
     events (as `waitEvent`: they arrive only with `present`);
  3. under the lock, it returns if a window was woken meanwhile, and otherwise clears the process's signal (below);
  4. it waits for the time left, on the system's events and the process's signal together.

  It then takes every window's wake, and clears the process's signal.
- **The process's signal.** Besides each window's signal (ADR 0142), the process has one, made at the first `open`
  (the main thread) and kept until the process exits: a non-blocking, close-on-exec pipe on POSIX systems, a
  manual-reset event on Windows. `wake` sets it with the window's, under the lock. `waitAny` waits on that one signal
  however many windows are open, so Windows' `MsgWaitForMultipleObjectsEx` has one handle, not one per window, and
  there is no limit of 63 windows. A `waitEvent` that took its window's wake leaves the process's signal set; step 3
  clears it when no window is woken, so a stale signal costs one more pass of the loop, never a lost wake.
- **The wait, per system:**
  - **macOS:** `nextEventMatchingMask:untilDate:` with `dequeue:NO`, as `waitEvent`: the application's queue holds
    every window's events, and the wake source (ADR 0142 §2.4) is the application's. `wake` now signals the wake
    source for a scripted window too (it is only made at the first system window's `open`), so a program with both
    kinds wakes from AppKit's wait.
  - **Windows:** `MsgWaitForMultipleObjectsEx` on the process's event, with `QS_ALLINPUT` and `MWMO_INPUTAVAILABLE`:
    the thread's message queue holds every window's messages.
  - **X11:** if any window's connection has queued events (`XPending`), there is no wait; otherwise `poll` on every
    window's connection and the process's pipe.
  - **Only scripted windows:** the process's signal alone, for the time left.
  - **Both kinds:** the system's wait, which the process's signal ends too (on macOS through the wake source).

### 2.4 The interpreter

- `open` stays `Unsupported` there, so no window is ever open, and `waitAny` is reachable only as the case of no
  window: it advances the interpreter's virtual clock (ADR 0075 §2.6) by `timeoutMs`, as a sleep, so a run stays
  reproducible.

## 3. Alternatives considered

- **`waitEvent` waiting for every window.** It would change a call that ClayWind's single-window hosts use, and a
  program that waits on one window on purpose (a modal dialog's loop) would return for the others. The owner kept
  `waitEvent`'s meaning.
- **A set of windows, `Window.waitAll(windows)`.** A `Window` is a move type, neither `Send` nor `Share`; a slice of
  borrows of several windows, some of them borrowed `mut` elsewhere in the same loop, is a burden for every host,
  and the runtime already knows the open windows (ADR 0142's list).
- **Returning which window is ready** (an index, or the window's waker id). The host must drain every window and its
  channels after a wait anyway (more than one may be ready, and a wake is not an event, ADR 0142 §2.1), so it adds
  nothing a host can rely on.
- **Waiting on every window's own signal.** On Windows `MsgWaitForMultipleObjectsEx` takes at most 63 handles, so a
  64th window would need slices or a thread; one signal for the process has no limit and one handle on every system.
- **Returning at once with no window open.** A host's loop would spin while it has no window (§2.1).
- **A `Result` with `NotMainThread`** off the main thread. That thread has no window by construction, so the call has
  one meaning there, a sleep; an error the program can only ignore adds a `!` or a `match` to every call.
- **Routing in `sendEvent:`** (an `NSWindow` subclass whose `sendEvent:` queues). It would need a runtime-made class
  per window kind and still the pump's lookup for events without a window; the pump already sees every event.

## 4. Consequences

- **New call:** `Window.waitAny`. No new type or event.
- **Fixed:** on macOS, a window's input is no longer lost when another window of the program is polled first; a
  window's scale change is seen by any window's pump.
- **`wake`** also sets the process's signal, and on macOS signals the wake source for a scripted window too; a
  `waitEvent` may then see one spurious return from AppKit's wait, which its loop absorbs.
- **Every process with a window** has one more signal: two descriptors on POSIX systems, an event handle on Windows.
  An `open` that cannot make it is `Resources`.
- **Not covered:**
  - which window a wait returned for (§3);
  - an executor on the UI thread, as ADR 0142 §2.2;
  - Wayland, as before.
- **The spec** is type-system.md §26.13.

## 5. Implementation and evidence (2026-11-15)

**Files:**
- `lib/std/window.cee`: `Window.waitAny` (§2.1).
- `runtime/hosted/ceesafe_rt_window.c`: §2.2 and §2.3.
- `src/interpreter/io.cee`: §2.4.
- `tests/reference/tests/corpus/run-pass/window-wait-any.cee`: `waitAny` with no window, on the main thread and on
  another, sleeps its timeout, and 0 returns at once (it runs headless, natively and in the interpreter);
  `check-pass/window-wait-any.cee`: a host loop over two windows, one of them woken by a task on another thread.
- `tools/window_check.py`: two scripted windows, each one's queued event and a thread's wake of each ending a wait of
  20 s, the timeout with nothing, and a wake that `waitEvent` on the other window leaves for `waitAny`; with
  `--display`, two real windows, a task's wake of the second ending a `waitAny`, and a click posted to the second
  through the system while only the first window's `waitEvent` and `nextEvent` run (the routing fix; built as a
  package with a C helper, since only C posts a system event).

**Tests** are recorded in ROADMAP §10 (2026-11-15, ADR 0145), with the commands and results on each system.
