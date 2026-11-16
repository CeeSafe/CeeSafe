# ADR 0156 — The safe area of a window

- Status: **Accepted** by the owner, 2026-11-16 ("I accept ADR 0156. Go ahead and release."), for ClayWind's roadmap
  phase P4 (mobile) step 4, "the screen: safe areas, rotation, size classes and the pixel ratio" (the owner's
  "continue" started that step). Implemented on branch `adr/0156-safe-area` (from release 0.57.1): iOS, Android and
  the scripted window (§5).
- Scope: `lib/std/window.cee` (`WindowInsets`, `Window.safeArea`), `runtime/hosted/ceesafe_rt_window.c` (iOS, Android,
  the scripted window), type-system.md §26.13, `spec/api/std.md` (generated), `tools/window_check.py`. No compiler
  change.

## 1. Context

- **A phone's window is the whole screen** (ADR 0154), and the system covers parts of it: the status bar, a sensor
  housing or display cutout, the home indicator or the navigation bar, the screen's rounded corners. A program that
  lays out to the window's edges puts a title under the clock and a button under the home indicator.
- **Of step 4's four parts, three need nothing new:**
  - the pixel ratio is `Window.scale()` (ADR 0137), the screen's scale on iOS and the density's on Android;
  - a rotation is a `Resized` with the new size (ADR 0154), on Android when the activity's manifest keeps the
    activity through it (§2.3);
  - size classes (compact or regular, by the content's width and height in points) are a rule over `size()`, which
    is the UI library's to choose; ClayWind's is its own.
- **The fourth, the safe area, only the system knows.** iOS gives it as a view's `safeAreaInsets`. Android gives it
  as the window's insets, and since Android 15 (API 35, for apps that target it; without exception from API 36) every
  app's window is edge to edge, so an app that ignores the insets draws under the bars.
- **`WindowEvent` is matched exhaustively** by ClayWind's hosts; a new variant breaks every such `match` (N0256).
  ADR 0150 and ADR 0155 kept their news out of `WindowEvent` for that reason.

## 2. Decision

### 2.1 The API

```
let safe = w.safeArea();            // WindowInsets { left, top, right, bottom: f32 }, in points
content.x = safe.left;
content.width = size.width as f32 - safe.left - safe.right;
```

- **`WindowInsets { left: f32, top: f32, right: f32, bottom: f32 }`**: how far the system covers the content from
  each of its edges, in points.
- **`w.safeArea() -> WindowInsets`** (`borrow`, no effect): the window's insets now, as of the last event the window
  took from the system. No inset is negative. All four are zero where the system covers nothing: on the desktop
  systems, and for a scripted window until its script gives a safe area.
- **The content does not shrink.** `size()` is still the whole window, and a frame is still drawn for all of it: a
  program fills its background to the edges and keeps what must be seen or touched inside the insets, which is what
  both systems ask of an app.
- **The soft keyboard is not in it.** It comes and goes with text input and is P4 step 5's.

### 2.2 Changes

- **A new safe area gives `Resized`** with the content's size, even when the size did not change (a phone turned from
  one landscape to the other, a bar that appears), as a new scale has since ADR 0137. A program that lays out again
  at `Resized`, reading `size()`, `scale()` and `safeArea()`, needs nothing else, and no program's `match` changes.
- **A rotation is a `Resized`** with the new size; when the insets change with it, a second `Resized` may follow in
  the same poll.

### 2.3 The systems

- **iOS**: the view's `safeAreaInsets` (a `UIEdgeInsets`), read with the view's bounds at each poll and wait, and at
  `open` after the window is laid out. Which orientations an app takes is its Info.plist's
  (`UISupportedInterfaceOrientations`); the view follows its window.
- **Android**: the insets are the Java side's. On the activity's thread, with the activity's `JNIEnv`, the runtime
  calls `getWindow().getDecorView().getRootWindowInsets()` and takes, from Android 11 (API 30),
  `getInsets(WindowInsets.Type.systemBars() | displayCutout())`; before it, the system window insets and the display
  cutout's safe insets, the larger on each side. They are read when the window is created, resized, redrawn or
  focused, when the content rectangle changes and when the configuration does; the program's pump, woken, takes
  them, in points. A failure (no view attached yet, a method an old system lacks) keeps the last insets, and no Java
  exception is left pending.
  - **The density** is read again at a configuration change, so `scale()` follows a display whose density changes.
  - **A rotation destroys an activity**, which gives `Closed` (ADR 0154), unless its manifest declares
    `android:configChanges` with at least `orientation|screenSize|screenLayout|density`. An app that rotates
    declares it; ClayWind's packaging (P4 step 8) writes it.
- **Scripted windows**: `safe:LEFT,TOP,RIGHT,BOTTOM`, four numbers of points, none negative.
- **Desktop systems**: zero, always (§4).

## 3. Alternatives considered

- **A `WindowEvent.SafeArea` variant.** It breaks every exhaustive `match`, for news a program handles exactly as it
  handles `Resized`.
- **A smaller content**: `size()` as the safe rectangle only, with the frames placed inside it. Simple for a program
  that knows nothing of insets, but the app's background then stops short of the screen's edges, with the system's
  color around it, which neither system's apps do; and a GPU surface (ADR 0137) is the whole window anyway.
- **A flag that only wakes a wait**, as a drag's changes do (ADR 0150). A program would have to compare the insets
  after every wait; `Resized` is the event it already lays out on.
- **Android's content rectangle** (`onContentRectChanged`), which needs no JNI. Under edge to edge it is the whole
  window, so it says nothing on the systems where the answer matters most.
- **Reading the insets on the program's thread.** A view's getters belong to the activity's thread; the activity's
  callbacks are already there.

## 4. Consequences

- **No break**: `WindowEvent` and `WindowHandle` are unchanged. A program that never asks sees one more `Resized` at
  the same size when the insets change, which it already handles.
- **The runtime uses JNI on Android**, for the first time, through the activity's own `JNIEnv`; no Java code is
  added to an app.
- **ClayWind** gives the insets to its layout (a root padding, and per-edge choices for bars that reach the edge)
  and picks its size classes from `size()`.
- **Not covered**, each for a later ADR if a program needs it:
  - the soft keyboard's inset (P4 step 5);
  - the insets of system gestures (Android's back gesture at the side edges), which do not cover content;
  - a notch on a Mac's built-in display, where a full-screen window is kept below it by the system;
  - locking a window's orientation from the program, which stays the app's manifest's or Info.plist's;
  - the rectangles of the cutouts themselves and the corners' radii.

## 5. Implementation and evidence (2026-11-16)

- **One helper for every backend** (`cee_window_safe_area`): it keeps the four insets, never negative, and queues
  `Resized` at the current size when they change, as `cee_window_rescaled` does.
- **iOS**: `safeAreaInsets` through `objc_msgSend` (`objc_msgSend_stret` on x86_64, as for a `CGRect`), in the pump
  beside the bounds and the scale, and at `open` after `layoutIfNeeded`.
- **Android**: `cee_android_read_insets` (JNI, in a local frame, every exception cleared) from the callbacks
  `onNativeWindowCreated`, `onNativeWindowResized`, `onNativeWindowRedrawNeeded`, `onWindowFocusChanged`,
  `onContentRectChanged` and `onConfigurationChanged`, the last four of which are new and also wake the program, so
  a rotation ends a wait; the density read again at `onConfigurationChanged`.
- **`lib/std/window.cee`**: `WindowInsets`, `Window.safeArea`; `spec/type-system.md` §26.13 and its script grammar.
- **Evidence:**
  - `tools/window_check.py`: 50 checks, 0 problems, on macOS arm64; with `--display`, 60 checks, 0 problems. Its
    five new ones: a trace (zero at first; a new safe area with a `Resized` at the same size; the same one again
    gives nothing; one given with a resize comes after the resize's `Resized`; zero again), safe areas of three and
    five numbers and with a negative inset as malformed scripts, and the interpreter's `Unsupported`. With
    `--display`, a real macOS window answers `0 0 0 0`.
  - Android 17 emulator (API 37, arm64, 1080×2400 at density 420; the test built for API 28, its manifest targeting
    API 36, so edge to edge), a test that prints the size, scale and safe area at `open` and at each `Resized` and
    draws the safe area green in a red content: portrait 411×914 at 2.625, safe 0 24 0 24 (the status bar and the
    gesture bar); rotated, `Resized` 914×411 with safe 0 24 0 24; back, `Resized` 411×914. With an emulated tall
    cutout and three-button navigation: portrait safe 0 48 0 48; landscape 48 24 48 0 (the cutout on one side, the
    navigation bar on the other, the status bar above), the same in the other landscape. The screenshots show the
    red bands exactly under the system's bars in each case.
  - iOS 27 simulator (iPhone 18 Pro), the same test in two app bundles: portrait only, 402×874 at 3, safe 0 62 0 34;
    landscape only, 874×402, safe 62 0 62 20. The screenshots show the red bands over the sensor housing, the
    corners' sides and the home indicator.
  - The window runtime compiles with ceesafec's own warning flags and `-Werror` for macOS (arm64, x86_64), the iOS
    simulator (arm64, x86_64), iOS devices (arm64), Android API 28 (arm64, x86_64, armv7; NDK 28.2), Linux (Ubuntu
    24.04, gcc 13) and Windows (MinGW-w64 gcc 13).
  - Not verified: a rotation while the app runs on iOS (the Simulator is not driven here; the two bundles show each
    orientation's values, and the size already followed a rotation in ADR 0154's code), Android before API 30 (the
    older insets' path, compiled but not run), a change of density while the app runs, and devices.
  - `tools/contract_tests.py` with `out/ceesafec` built from the branch: every check ok but two, both then fixed and
    run again alone. Seven check-pass HIR dumps number their closures after lib/std's, which has two more
    (regenerated; they differ in those numbers only). `console_input_check.py` failed one run in four on an
    interpreted program: it opened its FIFO's writing end, without blocking, before the program had opened the
    reading end (ENXIO); it now tries again until the program has. Eight runs, 0 problems.
