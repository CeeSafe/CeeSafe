# ADR 0159 — An app's life on iOS and Android: background, foreground, memory, and where its data lives

- Status: **Proposed**, 2026-11-16, for ClayWind's roadmap phase P4 (mobile) step 6, "the app lifecycle: background
  and foreground, memory warnings, and state saved on the way out" (the owner's standing "continue"). Not
  implemented.
- Scope: `lib/std/window.cee` (`WindowAppEvent`, `Window.nextAppEvent`), `lib/std/host.cee` (`Directory.appData`),
  `runtime/hosted/ceesafe_rt_window.c` (iOS, Android, the scripted window), `runtime/hosted/ceesafe_rt_io.c`,
  type-system.md §26.13 and the section of `Directory`, `spec/api/std.md` (generated), `tools/window_check.py`. No
  compiler change.

## 1. Context

- **A phone's app does not choose when it stops.** The user leaves it and the system takes it off the screen; soon
  after, the system may stop its threads (iOS suspends an app seconds after it leaves the foreground) and may end
  its process at any time to make room, without another word. An app that wants to come back as it was must save
  what matters when it leaves the foreground, not when it ends.
- **What a program is told today** (ADR 0154): `Focus(false)` when the window's scene is no longer active or the
  activity's window loses focus, which also happens for a notification shade, a permission prompt, a call banner and
  the app switcher; and on Android `Closed` when the activity is destroyed with the process alive. Neither says
  "you are off the screen now: save".
- **Memory.** Both systems warn an app when memory is short (`didReceiveMemoryWarning`; `onLowMemory`,
  `onTrimMemory`) and end the apps that hold the most. A UI library holds caches it can drop: glyphs, images, layers.
- **Where to save.** A phone's app may write only inside its own container, in folders the system names. CeeSafe has
  `Directory.temporary()` (ADR 0158 gave it Android's cache folder), which the system may empty; there is no call
  for the folder whose contents are kept.
- **`WindowEvent` is matched exhaustively** by ClayWind's hosts; ADRs 0150, 0155, 0156 and 0157 kept their news out
  of it.

## 2. Decision

### 2.1 The API

```
loop {
    match (w.nextAppEvent()) {
        Some(WindowAppEvent.Background) => { save(state); }        // off the screen: save now, stop drawing
        Some(WindowAppEvent.Foreground) => { redraw = true; }      // back on it
        Some(WindowAppEvent.MemoryWarning) => { caches.clear(); }
        None => break,
    }
}
let folder = Directory.appData("claywind-chat")!;                   // kept from one run to the next
```

- **`WindowAppEvent`**: `Background`, `Foreground`, `MemoryWarning`.
- **`w.nextAppEvent() -> WindowAppEvent?`** (`borrow mut`): the next of them, in order, or null; it never blocks.
  They are queued beside the events, as touches and drops are, and a queued one ends `waitEvent`, `waitFrame` and
  `Window.waitAny`. A program that never asks loses nothing it has today.
- **`Directory.appData(name: StringView) -> Result<String, IoError>`**: the folder for this app's own data, kept from
  one run to the next, created if it is missing. `name` is the app's folder name where the system gives none
  (the desktop systems); `InvalidInput` when it is empty or holds a separator.

### 2.2 Promises

- **`Background` and `Foreground` alternate**, and a window opens in the foreground: the first is a `Background`.
  `Background` means the app is no longer on the screen (not merely covered or without focus, which is still
  `Focus(false)`); `Foreground` that it is on the screen again.
- **Time to save.** After `Background` is queued, the program is given time to take it and save:
  - iOS: the runtime asks the system for background time (`beginBackgroundTaskWithExpirationHandler:`) and gives it
    back when the program next waits for events, which is how the program says it has done what it had to; the
    system allows some seconds, and the runtime ends the task when the system says the time is up.
  - Android: the activity's `onStop` does not return until the program has taken `Background` and waited or polled
    again, two seconds at most, as `android_native_app_glue` holds its callbacks; the system does not end a process
    inside `onStop`.
  A program that saves within that, at `Background`, has saved before the system can stop it. What it writes after
  is not promised to be written.
- **In the background** `present` shows nothing and returns `Ok` (there is no surface on Android, and iOS forbids
  drawing work there), and waits keep working, so a program's loop needs no special case: it sleeps in `waitEvent`.
- **`MemoryWarning`** may come at any time, in either state, and more than once.
- **The process's end is not an event.** The system ends a background app's process without telling it. On Android
  the activity destroyed with the process alive is still `Closed`.
- **`Directory.appData`** names: macOS `~/Library/Application Support/<name>`; Windows `%APPDATA%\<name>`; Linux and
  FreeBSD `$XDG_DATA_HOME/<name>`, else `~/.local/share/<name>`; iOS the container's `Library/Application
  Support/<name>` (backed up with the device, not shown in Files); Android the app's files folder,
  `/data/data/<package>/files/<name>`.

### 2.3 The systems

- **iOS**: the scene delegate's `sceneDidEnterBackground:` and `sceneWillEnterForeground:` (ADR 0154's
  `CeeSafeSceneDelegate`), and `UIApplicationDidReceiveMemoryWarningNotification`.
- **Android**: the activity's `onStop` and `onStart` (`onPause` is not the background: a split screen keeps a paused
  activity visible), and `onLowMemory`; all three are `ANativeActivity` callbacks, with no Java.
- **Scripted windows**: `app:background`, `app:foreground` and `app:memory`.
- **Desktop systems**: no app events (a minimized or hidden window is still the program's to draw, and nothing ends
  its process for memory). `Directory.appData` works everywhere.

## 3. Alternatives considered

- **`WindowEvent` variants.** They break every exhaustive `match`.
- **A callback** the system calls at the moment (`Window.onBackground(fn)`), which could save before the callback
  returns. It runs on the system's thread while the program's loop is elsewhere, in the middle of a frame or holding
  a borrow: the program's state is not safe to read there. The queue with time held (§2.2) gives the same guarantee
  on the program's own thread.
- **`Focus(false)` as the signal to save.** It comes for every banner and prompt, several times a minute for some
  apps; saving at each is wasteful, and it does not come at all when an app goes straight to the background on some
  paths.
- **State saved by the system** (iOS state restoration, Android's `onSaveInstanceState` bundle). Both keep a small
  blob for the system to hand back; they add a second place for state with its own size limits and lifetime, and
  Android's needs Java to read on the way back in. A file in `Directory.appData` is one mechanism for every system.
- **Levels of memory warning** (Android's `onTrimMemory` levels). iOS has one; a program that drops its caches at
  any warning does the right thing on both.

## 4. Consequences

- **No break**: no event or handle is added; a program that never calls `nextAppEvent` behaves as before, except
  that `present` in the background on iOS now shows nothing instead of drawing into a layer no one sees.
- **ClayWind** saves its snapshot (the one hot reload uses) to `Directory.appData` at `Background`, loads it at
  start, stops its frame loop in the background, and drops its glyph and image caches at `MemoryWarning`.
- **Not covered**, each for a later ADR if a program needs it: work that continues in the background (downloads,
  audio, location), notifications, a URL or file that launches the app, several scenes or windows on an iPad, and
  the split-screen and picture-in-picture states between foreground and background.

## 5. Plan of implementation and evidence

1. `Directory.appData` on every system; `nextAppEvent`, the queue and the scripted `app:` items;
   `tools/window_check.py` cases; type-system.md.
2. iOS: the scene delegate's two methods, the memory notification, the background task. Evidence: the simulator
   (`simctl` can send an app to the background by launching another, and a memory warning through its menu's
   private call if reachable) and the owner's iPhone: Home and back, with the saved file's time.
3. Android: `onStop`, `onStart`, `onLowMemory`, the held callback. Evidence: the emulator (`input keyevent HOME`,
   `am start`, `am send-trim-memory`) and the owner's moto g35 5G.
4. The window runtime under ceesafec's own warning flags for every target; the contract suite; ClayWind's demo and
   chat client coming back as they were left, after `am kill` and after iOS ends the app in the background.
