# ADR 0155 — Touches on a window

- Status: **Accepted** by the owner, 2026-11-16 ("Continue", after this proposal), for ClayWind's roadmap phase P4
  (mobile) step 3, "touch events and gestures" (the owner's "Go ahead with 1-3" started that step with this ADR).
  Implemented on branch `adr/0155-touches`: iOS, Android and the scripted window (§5).
- Scope: `lib/std/window.cee` (`WindowTouch`, `WindowTouchPhase`, `Window.setTouches`, `Window.nextTouch`),
  `runtime/hosted/ceesafe_rt_window.c` (iOS, Android, the scripted window), type-system.md §26.13, `spec/api/std.md`
  (generated), `tools/window_check.py`. No compiler change.

## 1. Context

- **ADR 0154 gives a touch screen one pointer**: the first finger down is the primary button and its moves are pointer
  moves, and every other finger is ignored. That is enough for buttons and lists, not for what a touch UI needs:
  pinching, two-finger pans, a second finger that cancels a tap, and a velocity for a fling.
- **ClayWind recognizes gestures itself.** Its widgets are nested in one native view, and which widget gets a gesture
  is decided among them (a list scrolls, a button inside it is tapped), so the system's recognizers, attached to the
  whole view, cannot decide it. Tap, double tap, long press, pan, pinch and momentum scrolling are ClayWind's, from
  raw touches. CeeSafe's part is to give the touches faithfully, the same on every system.
- **The systems agree on the raw model**: each finger has an identity from down to up, a position on every change, and
  a time; the system may take touches away (iOS `touchesCancelled:withEvent:` when a system gesture wins, Android's
  `AMOTION_EVENT_ACTION_CANCEL`).
- **`WindowEvent` is matched exhaustively** by ClayWind's hosts. A new variant breaks every such `match` (N0256), as
  `Composition` did in ADR 0141 and the handles did in ADR 0154. ADR 0150 kept drops out of `WindowEvent` for that
  reason, with a queue beside the events.

## 2. Decision

### 2.1 The API

```
w.setTouches(true);                 // bool: whether this window gives touches here
loop {
    match (w.nextTouch()) {
        Some(t) => track(t.id, t.phase, t.x, t.y, t.time),
        None => break,
    }
}
```

- **`WindowTouchPhase`**: `Began`, `Moved`, `Ended`, `Cancelled`.
- **`WindowTouch { id: u32, phase: WindowTouchPhase, x: f32, y: f32, time: u64 }`**:
  - `id`: the finger, from its `Began` to its `Ended` or `Cancelled`. The runtime numbers touches itself, from 1, per
    window, and never reuses a number in a window's life, so a program's table of fingers never confuses an old one
    with a new one. (The systems reuse theirs: Android's pointer ids restart at 0, and iOS gives object addresses.)
  - `x`, `y`: points from the content's top left, as pointer events are.
  - `time`: nanoseconds on the system's clock of input events (iOS `UITouch.timestamp`, Android
    `AMotionEvent_getEventTime`), for velocities. Only differences between touches of one window mean anything.
- **`w.setTouches(on: bool) -> bool`** (`borrow mut`): with `on`, touches are queued for `nextTouch` and are no longer
  given as the pointer, so a program does not see a finger twice. It returns whether the system gives touches to this
  window: true on iOS and Android and for a scripted window, false elsewhere for now (§4). Off, the default, keeps
  ADR 0154's pointer and queues no touch, so a program that never asks pays nothing and its queue cannot fill.
  Turning it off cancels the touches that are down (below), then gives the pointer from the next finger down.
- **`w.nextTouch() -> WindowTouch?`** (`borrow mut`): the next touch, in order, or null; it never blocks. With none
  queued it takes the system's events first, as `nextEvent` and `nextDrop` do. A queued touch ends `waitEvent`,
  `waitFrame` and `Window.waitAny`, as an event does. It has no effects beyond the window's own queue, and runs on the
  program's first thread, as every `Window` call does.

### 2.2 Promises

- **Every `Began` gets exactly one end**, `Ended` or `Cancelled`, with the same id, and nothing for that id after it.
  When the system cancels, when the window loses focus or is closed, when the app goes to the background, and when
  touches are turned off, the runtime queues `Cancelled` for each touch still down. A gesture recognizer can therefore
  hold state per finger without a timeout.
- **`Moved` only for a change of position**, after the finger's `Began`. A `Moved` and the `Ended` that a system
  reports together come as a `Moved`, then the `Ended`, at the same place.
- **One finger's touches keep their order; different fingers' touches keep the system's order.** Touches and events
  are separate queues: a program that needs a key's order relative to a finger reads both after each wait, as for
  drops.
- **A bounded queue.** A window keeps at most 256 touches not taken. When full, a new `Moved` replaces the last queued
  touch of the same finger if that is a `Moved` (a program that reads late sees the latest position, which is what it
  draws); failing that, the oldest queued `Moved` of any finger is dropped; failing that (a queue of taps only), the
  oldest finger whose `Began` and end are both queued is dropped whole, so a program that reads late loses a tap it
  never saw, never half a finger. One such finger is always there: the queue's other fingers are those down now or
  down at the last read, and a window follows at most 32 fingers at once (a 33rd is not followed).

### 2.3 The systems

- **iOS.** `CeeSafeTouchView` (ADR 0154) already receives `touchesBegan`, `touchesMoved`, `touchesEnded` and
  `touchesCancelled`; with touches on, it queues each touch of the event's set, its `locationInView:` in points, and
  its `timestamp` × 10⁹. The view sets `multipleTouchEnabled`. A `UITouch` is mapped to the window's id for it in a
  small table, cleared at its end.
- **Android.** The input queue's `AMotionEvent`s: `ACTION_DOWN` and `ACTION_POINTER_DOWN` give `Began` for the action's
  pointer, `ACTION_MOVE` a `Moved` for each pointer whose position changed (with the batched historical samples first,
  `AMotionEvent_getHistorical*`, so a fling's velocity is not undersampled), `ACTION_UP` and `ACTION_POINTER_UP`
  `Ended`, `ACTION_CANCEL` `Cancelled` for every pointer down. Positions are divided by the density's scale, as ADR
  0154's are. Only `AINPUT_SOURCE_TOUCHSCREEN` gives touches; a mouse on Android stays the pointer.
- **Scripted windows**: `touch:ID,PHASE,X,Y`, with PHASE `began`, `moved`, `ended` or `cancelled` and ID the script's
  name for a finger (the runtime numbers it as above), given as a system's finger would be: the pointer while touches
  are off, a touch while they are on. Its time is its frame's at 60 frames a second. `tools/window_check.py` checks
  the promises with them on every host.
- **Desktop systems**: `setTouches` returns false and changes nothing (§4).

## 3. Alternatives considered

- **A `WindowEvent.Touch` variant.** One queue keeps touches in order with keys, but it breaks every exhaustive
  `match` on `WindowEvent`, again, and a program that does not want touches must still match them. ADR 0150 chose the
  queue beside the events for the same reason.
- **The system's gesture recognizers** (`UIPinchGestureRecognizer` and the like; Android's `GestureDetector`, which is
  Java's and out of a native activity's reach). They differ by system in thresholds and in what they report, and they
  decide for the whole view, where ClayWind must decide among its widgets (§1).
- **Touches and the pointer together**, a pointer emulated from the first finger while touches are given. A program
  that handles both sees one tap twice, and must guess which pointer events came from a finger. `setTouches` makes the
  choice the program's, once.
- **The system's touch ids.** They are reused, on Android at once, so an id that ended and a new one look alike to a
  program that missed the end; the runtime's numbers never repeat.

## 4. Consequences

- **No break**: `WindowEvent` and `WindowHandle` are unchanged, and a program that does not call `setTouches` behaves
  as before.
- **ClayWind** turns touches on where `setTouches` returns true and builds its gestures from them, with the pointer
  and the wheel on the desktop.
- **Not covered**, each for a later ADR if a program needs it:
  - touch screens on the desktop: Windows (`WM_POINTER` with `PT_TOUCH`) and X11 (XInput 2.2 touch events). There
    `setTouches` returns false and the system's own emulation stays the pointer;
  - the trackpad's gestures on macOS (`magnifyWithEvent:`, `rotateWithEvent:`), which are not touches on the screen;
  - pressure, the touch's size, a stylus and hovering (Apple Pencil, S Pen).

## 5. Implementation and evidence (2026-11-16)

- **One touch layer for every backend** (`runtime/hosted/ceesafe_rt_window.c`): a backend gives each finger to
  `cee_touch_began`, `cee_touch_moved` and `cee_touch_ended` under a key of its own (a `UITouch`'s address, an
  Android pointer id + 1, a script's ID + 1). The layer keeps the fingers down, numbers them, queues touches, and,
  with touches off, does ADR 0154's pointer emulation, which moved there from the two backends. Queuing `Closed`
  (`cee_window_push`) and losing focus (`cee_window_focus`) cancel the fingers down.
- **iOS**: the view's four touch methods (cancel now its own), `multipleTouchEnabled`, `UITouch.timestamp`.
- **Android**: every motion action of a touch screen, with `AMotionEvent_getHistorical*` for batched moves; other
  sources (a mouse) are the pointer for their event.
- **`lib/std/window.cee`**: `WindowTouchPhase`, `WindowTouch`, `Window.setTouches`, `Window.nextTouch`;
  `spec/type-system.md` §26.13 and its script grammar.
- **Evidence:**
  - `tools/window_check.py`: 45 checks, 0 problems, on macOS arm64. Its eight new ones cover a trace of touches off
    and on (the pointer from one finger, a second ignored, the pointer lifted when touches turn on, numbers from 1
    never reused, a move to the same place dropped, a move before an end that moved, cancels by the script, by focus
    loss and by turning touches off), a full queue (300 moves of one finger merged to 254 and its end; 200 taps
    keeping the newest 128 whole), a touch ending `waitEvent` and `waitAny` of 20 s, four malformed `touch` items,
    and the interpreter's `Unsupported`. With `--display`, a real desktop window answers false; its program run alone
    on macOS printed `touches false`.
  - Android 16 emulator (arm64, API 28 build), touches injected through the emulator's console
    (`adb emu event send`, multi-touch protocol B; `sendevent` is refused by SELinux): with touches off, a tap is the
    pointer at 205.7, 457.1 (540, 1200 pixels at 2.625) and of two fingers only the first is; on, a pinch gives
    touch 1 at 152.4 and touch 2 at 259.0, 20 moves, touch 2's end at 335.2 and touch 1's at 72.4, 460.9, in order,
    at increasing times; a finger held while Home is pressed is `Cancelled` (touch 3); Back gives `Closed` and the
    program returns 0.
  - iOS 27 simulator (iPhone 18 Pro), launched headless: the window opens at 402×874, gets its focus, and
    `setTouches(true)` is true; the app runs on.
  - The window runtime and the hosted runtime compile with ceesafec's own warning flags (`-Wall -Wextra -Wpedantic`
    … `-Wcast-qual -Wmissing-prototypes`, `-Werror`) for macOS, the iOS simulator and devices, Android (API 28), Linux
    (gcc 13, X11) and Windows (MinGW-w64). This found two warnings in ADR 0154's iOS code, released in 0.56.0, that
    every iOS build printed: `ceesafe_window_app_start` had no prototype (now declared in `ceesafe/rt.h`), and the
    layer's gravity constant was cast from `const` (now held as `cee_id *`, as on macOS). Fixed here.
  - Not verified: touches on iOS (the Simulator app was not open and is not driven here; a person's touches in the
    simulator or on the iPhone are to come), Android's historical samples (the emulator's console sends one sample
    per event), and a stylus.
