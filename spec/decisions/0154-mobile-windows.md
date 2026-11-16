# ADR 0154 — Windows on iOS and Android

- Status: **Accepted** by the owner, 2026-11-16 ("continue", after the proposal of a window on iOS and Android behind
  the same `Window` API, the program's `main` inside `UIApplicationMain` and on a NativeActivity's thread). ClayWind's
  roadmap phase P4 (mobile) step 2. Implemented on branch `adr/0154-mobile-windows`, iOS and Android (§5).
- Scope: `runtime/hosted/ceesafe_rt_window.c`, `runtime/hosted/ceesafe_rt_hosted.c`, `lib/std/window.cee`
  (`WindowHandle`), type-system.md §26.13, `spec/api/std.md` (generated), `tools/window_check.py`. No compiler change:
  the generated C stays target-independent, and the runtime chooses by the C compiler's target.

## 1. Context

- **ceesafec already builds for iOS and Android** (P4 step 1): `--cc` with the platform's clang and `--cflag` for the
  target and SDK; the hosted runtime runs unchanged on the iOS simulator and on Android from API level 28.
- **But a window program owns its main loop** (ADR 0069): `main` opens a window and polls it. Neither system allows
  that. On iOS, `UIApplicationMain` must own the main thread, and a UIWindow exists only after the application has
  launched; an app that does not finish launching in time is killed. On Android, an app is a shared library that the
  system's activity loads; its code runs in the activity's callbacks or on threads it starts, and its window appears
  and disappears with the activity.
- **The window runtime has no iOS or Android backend**: on iOS the macOS path loads AppKit, which is not there
  (`NoDisplay`), and Android has no X11.

## 2. Decision

### 2.1 The program's start

- **A window program on iOS or Android is a windowed build** (`windowed = true` or `--windowed`, ADR 0151), whose
  generated `main` already calls `ceesafe_rt_windowed()` first. On Windows that call sets up the standard streams; on
  macOS, Linux and FreeBSD it does nothing, as before.
- **iOS.** The first call starts the application: `UIApplicationMain(argc, argv, nil, CeeSafeAppDelegate)`, which never
  returns. When the application has finished launching, the delegate's `application:didFinishLaunchingWithOptions:`
  returns at once and schedules (`performSelector:withObject:afterDelay:0`) a call of the program's `main` with the
  same arguments, on the main thread, from the run loop. That second call's `ceesafe_rt_windowed()` returns, and the
  program runs: it is on the program's first thread, as `Window` requires. When `main` returns, the process exits with
  its code. While the program runs, the run loop runs only when the program waits or polls (`nextEvent`, `waitEvent`,
  `waitFrame`, `waitAny`), which run it (`CFRunLoopRunInMode`), as SDL does on iOS. C's `main` may be called again
  (unlike C++'s).
- **Without a windowed build**, `Window.open` on iOS is `NoDisplay`: there is no application to put a window in.
- **Android**: the window runtime provides `ANativeActivity_onCreate`, the entry of a NativeActivity's
  library, with its own glue rather than the NDK's `android_native_app_glue` (Apache-2.0, and a thread model the
  runtime would wrap anyway). It starts the program's `main` on a thread of its own, which counts as the program's
  first thread for `Window`; the activity's callbacks queue what happens (the window created and destroyed, input,
  focus, size) for that thread, and `Window.open` waits for the activity's window.

### 2.2 The window on iOS

- **One window per program** (a phone app has one screen; iPad scenes are not covered): `Window.open`'s size is
  ignored, and the window fills the screen. `size()` is the view's size in points and `scale()` the screen's scale.
  A rotation gives `Resized`.
- **The view** is `CeeSafeTouchView`, a `UIView` subclass registered through the Objective-C runtime, as macOS's view
  is: its layer shows the frames (`present`, as a `CGImage` in the layer's contents), or is the program's own with
  `WindowSurface.External` (a `CAMetalLayer` added by the program).
- **Touches, until P4 step 3:** the first touch is the primary pointer: `PointerButton(Primary, true, …)` when it
  begins, `PointerMoved` while it moves, and `PointerButton(Primary, false, …)` when it ends or is cancelled. Other
  touches are ignored.
- **Focus:** `Focus(false)` when the application resigns active, `Focus(true)` when it becomes active again.
- **Waking** (ADR 0142): the same run loop source as on macOS, whose callback ends a wait.
- **The clipboard:** `UIPasteboard.generalPasteboard`'s string.
- **Not on iOS:** a title, a cursor, menus and file dialogs (`Unsupported`, and the first two do nothing), text input
  (P4 step 5), and closing: iOS never asks an app to close its window, so no `Closed` comes.

### 2.3 The window on Android

- **One window, the activity's**: `open` waits for it (ten seconds at most, else `NoDisplay`). Its size in points is
  its pixels over `dpi / 160`. Frames are copied into the `ANativeWindow`'s buffer (`RGBX_8888`, the frames' byte
  order), under a lock that the activity's thread also takes before it lets the window go.
- **Input**: the activity's `AInputQueue`, attached to the program thread's looper and read only under that lock;
  the activity's callbacks detach it under the lock, and so does the activity's end, since the system may dispose of
  the queue without a callback then. The first touch is the primary pointer, as on iOS.
- **Focus** from `onWindowFocusChanged`; **`Closed`** when the activity ends (`onDestroy`: Back, or the system's), and
  nothing touches the activity after that.
- **The program's end**: when `main` returns, the activity is finished and the process ends with `_exit` and `main`'s
  code, as `main`'s return ends the process elsewhere. Not the thread's own end, whose thread-local destructors would
  run after `ceesafe_shutdown`, and not `exit()`, whose static destructors Android's own threads still use: bionic
  aborts on both (a destroyed mutex). Standard output and error go to logcat (tag `ceesafe`), and the pipe is
  drained first, so the program's last lines are kept.
- **Not yet**: the clipboard (a Java service), a title, a cursor, menus, file dialogs, text input.

### 2.4 Native handles

`WindowHandle` gains `UIKit(window, view)` (the `UIWindow` and the view, whose layer is the content) and
`Android(window, activity)` (the `ANativeWindow`, 0 while the activity has none, and the `ANativeActivity`). A program that
draws with its own GPU code (ADR 0137) uses them as it uses `AppKit`'s.

### 2.5 Building

No new option: the platform's clang through `--cc`, the target and SDK through `--cflag`, and `--windowed`. An app
bundle (iOS) and an APK (Android) are made from the executable or library by the app's own tooling (P4 step 8); a
test makes the minimal bundle itself (§5).

## 3. Alternatives considered

- **A callback API** (`Window.run(frame)`), as UIKit and Android expect. It would split every program into a desktop
  form and a mobile form; the run-loop pump keeps one program for all systems, which is what ClayWind's hosts need.
- **The program on a second thread on iOS**, with the main thread running UIKit. UIKit may be used only on the main
  thread, so every window call would cross threads; the pump keeps the program on the main thread, where
  `Window` already insists it be.
- **`android_native_app_glue`.** It is the usual start, but it is Apache-2.0 C to be compiled with every program, and
  its `android_main` thread is the one the runtime's own glue gives.

## 4. Consequences

- **Two new `WindowHandle` variants** (`UIKit`, `Android`): every exhaustive `match` over `WindowHandle`
  needs an arm for it (N0256), as `WindowEvent.Composition` needed in ADR 0141.
- **iOS programs that open a window are windowed builds.**
- **Not covered:** multi-touch and gestures (step 3), safe areas (step 4), the soft keyboard (step 5), the app's
  lifecycle beyond focus (step 6), signing and devices (step 8).

## 5. Implementation and evidence (2026-11-16)

**Files:** `runtime/hosted/ceesafe_rt_window.c` (the UIKit backend and `ceesafe_window_app_start`; the Android
backend and its activity glue; the program's thread on Android for `Window`'s first-thread check),
`runtime/hosted/ceesafe_rt_hosted.c` (a windowed build's start on iOS), `lib/std/window.cee` (`WindowHandle.UIKit`,
`Android`), type-system.md §26.13, `spec/api/std.md`, `tools/window_check.py` (its exhaustive matches).

**Tests** are recorded in ROADMAP §10 (2026-11-16, ADR 0154): a test program in the iOS 26 simulator (as an app
bundle) and in the Android 16 emulator (as an APK), and the window runtime under strict flags on every system.
