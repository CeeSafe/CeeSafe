# ADR 0233 — The system's Back

- Status: **Accepted** by the owner, 2026-11-24 ("Continue", after this change was named as the next step: "Android's
  own Back button: it currently closes the app, because CeeSafe's runtime leaves it to the system. Fixing that is a
  CeeSafe runtime change"). Asked for by Marivil's CeeUI port (phase 3, phones). Implemented on branch
  `feat/android-back`: Android and the scripted window (§4).
- Scope: `lib/std/window.cee` (`WindowKey.Back`, `Window.setBackWanted`), `runtime/hosted/ceesafe_rt_window.c`
  (Android, the scripted window), type-system.md §26.13, `spec/api/std.md` (generated), `tools/window_check.py`. No
  compiler change.

## 1. Context

- **Android's Back is the system's today.** The runtime gives every key event of a NativeActivity back to the system
  (`cee_android_event` returns 0), so Back finishes the activity: the program gets `Closed` and ends. An app with
  pages of its own (Marivil's port: the chats, then a chat, Status, Settings) cannot go back a page with it; its user
  leaves the app instead, losing what was open.
- **Android's own model** (`OnBackPressedDispatcher`, and the predictive back gesture since Android 13): an app asks
  for Back while it has somewhere to go back to, by enabling a callback before the press, and the system keeps Back
  otherwise, so at an app's first page Back leaves it. The decision cannot be made after the press: by then the
  system must know whether to animate leaving the app.
- **No other system** CeeSafe opens windows on has a Back key. iOS goes back by a swipe from the screen's edge, which
  an app draws itself (UIKit's navigation controller); a desktop has Escape.

## 2. Decision

1. **`WindowKey.Back`**, appended to `WindowKey` after `Other` (so the earlier variants keep their tags, as
   ADR 0044 did for `IoError`): the system's Back.
2. **`Window.setBackWanted(on: bool) -> bool`** (`borrow mut`): whether the program wants Back. Wanted, Back is a
   `Key(WindowKey.Back, down, repeat, …)` event, its press and release, and the system does nothing with it; not
   wanted, the default, it is the system's, which ends the window (`Closed`). It returns whether the window has a
   Back: true on Android and for a scripted window, false elsewhere, where it changes nothing. A program sets it as
   its pages change (each frame is cheap: the runtime keeps a flag).
3. **While the soft keyboard shows**, its input method takes Back first and hides the keyboard, as the system
   already does (`AInputQueue_preDispatchEvent`): the program sees no Back then.
4. **The scripted window** has Back as the key name `back` (`key:back`, `keydown:back`, `keyup:back`). As on a
   phone, when the item is delivered and the program does not want Back, its press is `Closed` and its release
   nothing.

## 3. Consequences

- A program that matches every `WindowKey` needs an arm for `Back` (or `_`): a program that compiled before may not,
  until it does. Only `tools/window_check.py` among CeeSafe's own matched them all; CeeUI's key mapping is updated
  with it.
- Back keeps working as it does today for every program that does not ask for it.
- The predictive back gesture's animation (Android 14's preview of the page behind) is not drawn: the program is
  told at the press, as Android's non-predictive callbacks are. A later ADR may give the progress of the gesture.
- iOS's edge swipe stays the program's own gesture (ADR 0155's touches).

## 4. Implementation

- `runtime/hosted/ceesafe_rt_window.c`: `CEE_KEY_BACK` (15); the window's `back_wanted`; `ceesafe_window_set_back`;
  `cee_android_event` takes `AKEYCODE_BACK` while wanted and queues its down (with the system's repeat) and up,
  else returns it to the system as before; the script's key names have `back`, and `cee_window_script_pump` makes an
  unwanted Back `Closed`.
- `lib/std/window.cee`: `WindowKey.Back`, `keyOf`'s 15, `Window.setBackWanted`.
- type-system.md §26.13: the key, the method and the script's name; `spec/api/std.md` regenerated.
- Tests: `tools/window_check.py`'s new check, a program that wants Back, gets the script's press and release, lets it
  go, and is `Closed` by the next Back; in the interpreter the open is `Unsupported`, as for every window. The
  Android path by hand on the owner's Moto with Marivil's port (§5).

## 5. Evidence

Released in 0.95.3 (the owner: "Release CeeSafe 0.95.2 once the contracts pass"; 0.95.2 had been taken meanwhile by
ADRs 0229 to 0232, so this is the next patch, rebased onto that release). ROADMAP §10's row has the runs.

- macOS arm64: `tools/window_check.py`, 68 checks, 0 problems, the Back check native and interpreted. The contract
  suite on the rebased branch: every step ok, two skipped as Linux-only. `emit-c/arrays-of-atomics.c`'s closure id
  1748 is 1750 (lib/std's two new functions; a reviewed edit, the id only).
- Motorola, Android, with Marivil's CeeUI port built by this change: Back on Settings returns to the chats, Back on
  the chats leaves the app. Back while the soft keyboard shows (§2.3) was not tried.
