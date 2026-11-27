# ADR 0270 — Haptics

- Status: **Accepted** by the owner 2026-11-27, with §6 answered as recommended (Marivil uses a few key moments; where
  there are no haptics a call does nothing), for Marivil's CeeUI port (its phase 3 lists haptics). Implemented the same
  day (§7).
- Scope: `lib/std/window.cee` (`Haptics`, `HapticKind`), `runtime/hosted/ceesafe_rt_window.c`
  (`ceesafe_window_haptic`, `ceesafe_window_has_haptics`), `runtime/android/org/ceesafe/window/Haptic.java` (new),
  type-system.md §26.13, `spec/api/std.md`, `tools/window_check.py`.

## 1. Context

- A messenger gives a short vibration when a long press opens a message's menu, when a reaction is chosen, and when a
  message goes. MAUI has `HapticFeedback.Perform(Click | LongPress)`; CeeSafe has nothing.
- iOS: `UIImpactFeedbackGenerator` (light, medium, heavy), `UISelectionFeedbackGenerator`,
  `UINotificationFeedbackGenerator` (success, warning, error); no permission; an iPad has no Taptic Engine.
- Android: a view's `performHapticFeedback(constant)` on the UI thread follows the user's touch-feedback setting and
  needs no permission (a `Vibrator` needs `VIBRATE`); `CONFIRM` and `REJECT` from API 30.
- macOS: `NSHapticFeedbackManager`, felt only on a Force Touch trackpad while it is touched. Windows, Linux and FreeBSD
  have no common API.

## 2. Decision

```
Haptics.play(mut window, HapticKind.Light);   // a long press opened a menu
Haptics.available(window)                     // a phone, or a Mac
```

- **`Haptics.play(window, kind)`** plays one of seven kinds: `Light`, `Medium`, `Heavy` (impacts), `Selection` (a
  tick), `Success`, `Warning`, `Error`. It returns nothing: haptics are optional feedback, and where there are none
  (Windows, Linux, FreeBSD, a device without a vibrator, a scripted window) it does nothing (§6.2).
- **`Haptics.available(window)`**: an iPhone (not an iPad), an Android device with a vibrator, a Mac (whose trackpad
  may or may not be Force Touch); false elsewhere.
- Android's runs through the window's view, posted to the activity's thread, so the user's setting applies.

## 3. Consequences

- Marivil's port gives a light tap when a message's popup opens on a long press, a tick when a reaction is chosen, and
  a success when a message is sent (§6.1).

## 4. Options not taken

- **An error where there are none** (§6.2): every call site would handle a result that changes nothing.
- **Android's `Vibrator`**: it needs the `VIBRATE` permission and ignores the user's touch-feedback setting.

## 5. Tests

- `tools/window_check.py`: a scripted window has no haptics, and playing one does nothing.
- The Moto and the owner's iPhone, in Marivil's port: felt by hand.

## 6. Questions for the owner (answered 2026-11-27)

1. **Marivil's uses**: a few key moments (recommended: the long press, a reaction, a message sent), or only the long
   press? *A few key moments.*
2. **Where there are none**: nothing, silently (recommended), or an error? *Nothing, silently.*

## 7. Implementation (2026-11-27)

- `lib/std/window.cee`: `HapticKind`, `Haptics.play` and `Haptics.available`, over `ceesafe_window_haptic` and
  `ceesafe_window_has_haptics` (0 where there are none, and in a scripted window).
- iOS: a generator made for each haptic and released; `UIDevice.userInterfaceIdiom` is the phone's for `available`.
- macOS: `performFeedbackPattern:performanceTime:` (alignment for a light tap and a selection, level change otherwise,
  now).
- Android: `Haptic.java` (in the dex): `runOnUiThread` with the decor view's `performHapticFeedback` (`CLOCK_TICK`,
  `VIRTUAL_KEY`, `LONG_PRESS`; `CONFIRM` and `REJECT` from API 30), called from the program's thread attached to the
  VM; `available` asks the `Vibrator` whether there is one.
- The window runtime compiles with `-Wall -Wextra -Wcast-qual -Werror` for macOS, iOS, Android, Linux (gcc 14, clang
  19) and Windows (MinGW-w64 gcc 16).
