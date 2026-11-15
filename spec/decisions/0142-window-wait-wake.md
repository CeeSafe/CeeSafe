# ADR 0142 — Waiting for a window's events, and waking a window from another thread

- Status: **Accepted** by the owner, 2026-11-15. ClayWind's roadmap phase P3 needs windows that idle until something
  changes, and async work that wakes the UI: its step 3 is "CeeSafe async tasks on the UI thread, their results
  delivered to the UI, the window woken when they finish". The owner approved it as "ADR 0142 + state layer":
  implemented on branch `adr/0142-window-wait-wake`, released later.
- Scope: `lib/std/window.cee`, `runtime/hosted/ceesafe_rt_window.c`, type-system.md §26.13, `spec/api/std.md`
  (generated), `tools/window_check.py`, the corpus. No language, emitter or interpreter change.

## 1. Context

- **A window cannot idle.** `waitFrame` (ADR 0069) returns about 1/60 s after the previous call, or earlier when an
  event arrives. A program that redraws only when something changed still wakes 60 times a second to find nothing:
  a little CPU, and a laptop's timer coalescing defeated, for as long as the window is open.
- **Nothing outside the window can wake it.** `Window` is neither `Send` nor `Share` (§26.13; the corpus test
  `check-fail/window-not-send`), so a thread or a task that finishes some work has no way to tell the UI thread.
  The UI must poll its channels at the next tick, which is what the 60 Hz tick is then for.
- **Async tasks run under an executor that owns its thread.** `Async.run` and `Async.runWorkers` (§40.4) run a task
  on the calling thread until it returns, and an async body is a context without `sleep` (§32.3): a UI loop that
  waits for window events cannot be a task. What a UI needs from the tasks is their results on the UI thread, and a
  wake when they arrive.

## 2. Decision

### 2.1 The API (lib/std)

```ceesafe
let waker = window.waker();                       // WindowWaker: Copy and Send
let results = Channel<Answer>.create(16)!;        // between threads and tasks (ADR 0032)
let sender = results.clone();
let worker = Thread.spawn(move () => Async.run(fetch(sender, waker)))!;  // the tasks, on their own thread
loop {
    window.waitEvent(1000);                       // sleeps until an event, a wake or a second
    loop {
        match (window.nextEvent()) {
            Some(event) => { /* handle it */ }
            null => { break; }
        }
    }
    loop {
        match (results.tryRecv()) {
            Some(answer) => { /* apply it */ }
            null => { break; }
        }
    }
    // redraw if something changed
}

async fn fetch(out: Channel<Answer>, waker: WindowWaker) -> Result<void, AsyncError> {
    let answer = await compute();
    await out.sendAsync(answer)?;
    waker.wake();                                 // the UI's waitEvent returns
    return Ok();
}
```

- **`Window.waitEvent(borrow mut this, timeoutMs: u32)`** returns when an event is queued (`nextEvent` would return
  one), when the window was woken (below) since the last `waitEvent` or `waitFrame` returned, or after `timeoutMs`
  milliseconds, whichever comes first; 0 returns at once. It does not tick: an idle window that waits with a long
  timeout uses no CPU. It keeps the window alive while it waits: it takes the system's events, so a Win32 window
  repaints, an X11 window answers the clipboard's requests (ADR 0141) and AppKit runs, and returns only when one of
  them gave the program an event. `waitFrame` keeps its meaning.
- **`Window.waker(borrow this) -> WindowWaker`**: a handle that wakes this window. It is a struct with one private
  `u64`, the window's waker id, so it is **Copy and `Send`** (§38.1) where `Window` is neither: a thread or a task
  takes a copy. Ids are given out in order and never reused in a process, so a waker never reaches a later window.
- **`WindowWaker.wake(borrow this)`**, from any thread or task: the window's current `waitEvent` or `waitFrame`
  returns promptly, or its next one at once. Wakes before the window waits again are one. After the window was
  dropped it does nothing. It has **no effects** (`@effects()`), like `AsyncSignal.raise` (ADR 0039), so an async
  task calls it directly.
- **No new event.** A wake is not a `WindowEvent`: `waitEvent`'s return is the signal, and what the wake was for is
  in the program's own channels or state, which it reads after each return. Every exhaustive `match` over
  `WindowEvent` stays as it is. A program that needs to know a wake happened sets its own flag before waking, or
  sends on a channel, as above.
- **The lost-wake rule.** A program reads what it is woken for after `waitEvent` returns and before it waits again.
  A wake that comes while it is busy is kept until the next wait, so the order "send, then wake" on the other side
  never loses a result.

### 2.2 Async tasks and the UI (ClayWind's P3 step 3)

- The tasks run under `Async.run` (or `Async.runWorkers`) on a thread the UI starts, not on the UI thread: an async
  body may not sleep, and the UI thread sleeps in `waitEvent`. Their results reach the UI thread over a `Channel`
  (`await sendAsync` in the task, `tryRecv` in the UI loop; ADR 0032), and a `waker.wake()` after the send makes the
  UI's `waitEvent` return. The UI thread never blocks on the tasks, and a task never touches the `Window`.
- `wake` has no effects, and `WindowWaker` is Copy and `Send`, so it is captured by a task, a `spawnSend` child
  (ADR 0038) or `Async.blocking`'s closure as any integer is.
- **Not decided here:** an executor on the UI thread, with `await window.event()`. It needs the executor to wait on
  the system's event queue too (AppKit's has no descriptor for kqueue), and its own ADR (§4).

### 2.3 The runtime

- **Calls:**
  - `ceesafe_window_wait_event(window, timeout_ms)`;
  - `ceesafe_window_waker(window) -> u64`: the window's id, from 1;
  - `ceesafe_window_wake(id)`, from any thread.
- **The registry.** The open windows are in one list, under a spin lock (the channels' kind, ADR 0032: no `sleep`):
  `open` adds a window after it opened, with the next id, and `close` takes it out before it frees anything. `wake`
  looks the id up under the lock and, if the window is there and not already woken, sets its `woken` flag and its
  **signal**; a window that is gone is not found. Nothing but the flag and the signal's writing end is touched from
  another thread, and only under the lock.
- **The signal**, made at `open` for every window: on POSIX systems a pipe, both ends non-blocking and close-on-exec
  (`wake` writes one byte); on Windows a manual-reset event (`SetEvent`). Taking the wake (when a wait returns)
  clears the flag and drains the pipe or resets the event, under the lock, so flag and signal agree.
- **`waitEvent`** loops until its deadline: it returns if an event is queued or the window is woken; otherwise it
  takes the system's pending events (the backend's pump, as `nextEvent` does), checks again, and waits on the system
  and the signal for the time left. It then takes the wake. `waitFrame` takes the wake too, returns at once when the
  window was woken, and its wait includes the signal.

### 2.4 The backends

- **macOS.** The wait is `nextEventMatchingMask:untilDate:inMode:dequeue:` with `dequeue:NO`, as for `waitFrame`.
  A wake from another thread does not call AppKit. At the first `open`, the runtime adds a version 0 `CFRunLoopSource`
  to the main run loop's common modes. `wake` calls `CFRunLoopSourceSignal` and `CFRunLoopWakeUp`, which are
  thread-safe and allocate nothing. The source's callback runs on the main thread, inside the wait's run loop, and
  posts an `NSEventTypeApplicationDefined` event with the runtime's subtype (`postEvent:atStart:`). That ends the wait,
  and the pump drops it, as winit's event loop proxy does. The pipe is the scripted window's signal there.
- **Windows.** The wait is `MsgWaitForMultipleObjectsEx` on the window's event, with `QS_ALLINPUT` and
  `MWMO_INPUTAVAILABLE`, so a message already in the queue ends it too. `waitFrame` uses the same wait.
- **Linux (X11).** If Xlib has queued events (`XPending`, which also flushes), there is no wait. Otherwise `poll` waits
  on the connection's descriptor and the pipe. It replaces `waitFrame`'s `select`, which had only the connection.
- **The scripted window.** `waitEvent` returns at once while the script still holds events, since they arrive only
  with `present` (frames), and while an event is queued or the window is woken. Once the script has given all its
  events, it waits on the signal for the timeout, as an idle real window does. A test then sees a wake from another
  thread end a long wait, on any system and without a display (`tools/window_check.py`). `waitFrame` still returns at
  once.
- **The interpreter.** `open` is still `Unsupported`, so no window, and no waker, is ever made, and no other call is
  reached or modelled.

## 3. Alternatives considered

- **`WindowEvent.Woken`.** It would break every exhaustive `match` over `WindowEvent`, for information the program
  already has where it put its results. Rejected (§2.1).
- **A reference-counted waker** (an `Arc` of the window's signal). It needs a drop and a clone, and a raw pointer
  that is not `Send`. An id looked up in a registry is a plain value: Copy, `Send`, and safe after the window is gone
  without any shared ownership.
- **Posting the `NSEvent` from the waking thread** (GLFW's `glfwPostEmptyEvent`). `postEvent:atStart:` is documented
  as callable from other threads, but creating the event allocates Objective-C objects on a thread that may have no
  autorelease pool, and `wake` would have `alloc`. The run loop source keeps every AppKit call on the main thread.
- **`waitEvent` returning when this thread's executor has work.** The UI thread runs no executor while it waits
  (§1), so there is none to ask.
- **A wait on `nextEvent`** (`nextEvent(timeout)`). It would change a call that never blocks, and that ClayWind's
  hosts drain in a loop.
- **A timeout of "forever".** `u32` milliseconds reach 49 days; a program that wants no timeout loops.

## 4. Consequences

- **New calls**: `Window.waitEvent` and `Window.waker`, and the type `WindowWaker` with `wake`. No new event, so no
  program's `match` changes.
- **Effects.** `waitEvent` may sleep (`@effects(sleep)`). `waker` and `wake` have none.
- **Every window has a signal**: two descriptors on POSIX systems, an event handle on Windows. An `open` that cannot
  make it is `Resources`.
- **`waitFrame`** also returns early on a wake. Programs that never wake see no change, apart from X11's `select`
  becoming `poll`, and Windows' wait also ending for a message already queued.
- **Not covered:**
  - an executor on the UI thread, and awaitable window events (§2.2);
  - a wake from a signal handler: the registry's lock is not async-signal-safe;
  - Wayland, as before.
- **The spec** is type-system.md §26.13.

## 5. Implementation and evidence (2026-11-15)

**Files:**
- `lib/std/window.cee`: `Window.waitEvent`, `Window.waker`, `WindowWaker` (§2.1).
- `runtime/hosted/ceesafe_rt_window.c`: §2.3 and §2.4.
- `tests/reference/tests/corpus/check-pass/window-waker-send.cee`: a waker moved into a thread, used again (Copy),
  and passed to an async task run on another thread.
- `tools/window_check.py`: a scripted program whose second thread wakes a window that waits 20 s, and the checks
  of §26.13; type-system.md §26.13 and `spec/api/std.md`.

**Tests** are recorded in ROADMAP §10 (2026-11-15, ADR 0142), with the commands and results on each system.
