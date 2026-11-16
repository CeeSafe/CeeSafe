# ADR 0155 — Touches on a window

- Status: **Proposed**, 2026-11-16, for ClayWind's roadmap phase P4 (mobile) step 3, "touch events and gestures" (the
  owner: "Go ahead with 1-3", whose second item was to start that step with this ADR). Branch `adr/0155-touches`.
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
  `Moved` of the same finger (a program that reads late sees the latest position, which is what it draws), and failing
  that the oldest queued `Moved` of any finger is dropped. `Began`, `Ended` and `Cancelled` are never dropped; the
  fingers that can be down at once (ten on most screens) bound them.

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
  name for a finger (the runtime numbers it as above), delivered only while touches are on, so `tools/window_check.py`
  checks the promises on every host.

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
