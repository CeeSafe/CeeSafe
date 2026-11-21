# ADR 0200 — A host program's window on iOS and Android

- Status: **Accepted** by the owner, 2026-11-21 ("I accept ADR 0200"); implemented (§5). Asked for by ClayWind's
  roadmap (P6: its C API's Swift and Kotlin bindings on phones).
- Scope: `ceesafe/embed.h` (two functions and one documented entry), the hosted runtime's iOS and Android window
  code (names only: the behaviour exists), type-system.md §28.6 and §36's window section, embed_check. No language or
  library change.

## 1. Context

A host program (C, Swift, Kotlin through JNI, Rust) links a CeeSafe library made by `ceesafec build --emit lib` and
calls it after `ceesafe_init` (ADR 0088). On macOS, Windows and Linux such a library can open a `Window`: ClayWind's
C API (`claywind_run`) does, and its Swift, Rust and Kotlin bindings run on macOS. On phones it cannot:

- **iOS.** A window lives inside `UIApplicationMain`, which never returns. A windowed CeeSafe build's generated
  `main` first calls `ceesafe_rt_windowed()`: its first call runs `UIApplicationMain`, and once the application has
  launched, the runtime's delegate calls the process's `main` again from the run loop, where `ceesafe_rt_windowed()`
  returns at once and the program goes on (ADR 0154 §2.1). The function is internal (`rt.h`), so a host's `main`
  cannot do the same, and without it `Window.open` is `NoDisplay` (type-system.md §36).
- **Android.** The runtime is a NativeActivity's library: `ANativeActivity_onCreate` starts a thread that calls the
  library's `main` (ADR 0154 §2.1). A host library can supply that `main`, but a Kotlin host then needs its own
  classes from that thread, and a thread attached to the VM finds only the system's classes: it needs the activity's
  class loader. The runtime holds the activity (`ceesafe_android_vm`, used by the Bluetooth runtime, ADR 0177), but
  not for hosts.

## 2. Decision

1. **`void ceesafe_window_start(void)`** in `ceesafe/embed.h`: what a windowed build's `main` does first, for a host's
   `main`. On iOS its first call runs the application and does not return; the launched application calls the
   process's `main` again, where it returns at once. On Windows it gives a windowed program's standard streams a
   console or `NUL` (as `--windowed`, ADR 0151). Elsewhere it does nothing. A host calls it before `ceesafe_init`,
   first in `main` (for Swift, the top-level code of `main.swift`). `ceesafe_rt_windowed` becomes its internal name.
2. **Android: the host's library is the NativeActivity's.** As for a CeeSafe program, the activity's library defines
   `main`; the runtime calls it on its thread after the activity is created, and the process ends when it returns.
   `ceesafe/android.h` (new, included by nothing else) declares, for that thread and threads the host attaches:
   - `JavaVM *ceesafe_android_vm(void)`: the activity's VM (the existing function, now public);
   - `jobject ceesafe_android_activity(void)`: the activity's Java object, a global reference the runtime keeps, so a
     host reaches the activity's class loader (`getClassLoader`) and from it its own classes.
3. **Documented together** in type-system.md §28.6 (a host program), with the order of calls on each system, and in
   §36 (a window on a phone from a host).

## 3. Consequences

- ClayWind's Swift binding runs on iOS: its `main.swift` calls `ceesafe_window_start()`, then `ClayWind.run`. Its
  Kotlin binding runs on Android: the app's manifest names `android.app.NativeActivity` with the binding's JNI library,
  whose `main` attaches to the VM and calls the Kotlin entry through the activity's class loader.
- The internal `ceesafe_rt_windowed` keeps working for windowed builds; nothing they generate changes.
- A Kotlin host's UI runs in the NativeActivity, not in an Activity of its own: a host that wants a ClayWind view
  inside its own Android views (a `SurfaceView`) is not covered, nor one inside a UIKit view hierarchy of its own;
  either would be a later ADR.

## 4. Implementation

- `runtime/include/ceesafe/embed.h`: `ceesafe_window_start`; `runtime/include/ceesafe/android.h`: the two functions.
- `runtime/hosted`: `ceesafe_window_start` is `ceesafe_rt_windowed`'s body (the latter calls it);
  `ceesafe_android_activity` returns a global reference made once.
- `--emit lib` writes `android.h` beside `embed.h` for Android targets.
- Tests: embed_check, a host's `main` calling `ceesafe_window_start` then opening a window, in the iOS simulator
  (`window_check` has the simulator already) and the Android emulator; ClayWind's Swift and Kotlin bindings on the
  simulator and the emulator, and on the iPhone and the moto by the owner.

## 5. As built

- `ceesafe_window_start` (`embed.h`, defined in `ceesafe_rt_hosted.c`) holds what `ceesafe_rt_windowed` did, which now
  calls it. `ceesafe/android.h` is runtime file 11; every hosted build writes it (its declarations are under
  `#if defined(__ANDROID__)`, so it is empty elsewhere), and `--emit lib` and `--emit obj` beside `embed.h`.
  `ceesafe_android_activity` returns the `ANativeActivity`'s `clazz`, the global reference NativeActivity holds.
- Found on the way and fixed: `--emit lib`'s archiver was not found beside the NDK's clang, whose name has the target
  before `clang` (`aarch64-linux-android25-clang`): the system's `ar` was used and, on a Mac, wrote an empty archive of
  the ELF objects (96 bytes). `locateArchiver` takes `llvm-ar` beside a clang named for its target too (tooling.md).
- Tests: embed_check's `window-start` (the call before `ceesafe_init`, then exports; 85 passed) and the header beside
  the archive and the object. ClayWind's bindings: the Swift counter on the iOS 26 simulator (iPhone 17 Pro), built with
  `swiftc` against the archive for `arm64-apple-ios17.0-simulator`, laid out inside the safe area (input not driven:
  no Simulator app on the machine); the Kotlin counter on the Android emulator (API 37, arm64), its activity a
  NativeActivity whose library's `main` calls `claywindMain()`: taps counted, text typed by `adb input text` reached the
  Kotlin `String`, a checkbox and the theme switched.
