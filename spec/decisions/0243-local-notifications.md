# ADR 0243 — Local notifications

- Status: **Proposed — implemented** 2026-11-24 on the branch `feat/notifications`, asked for by Marivil's CeeUI port
  (phase 3: notifications; the owner: "Phase 3 next", then the order local notifications, push tokens, the iOS
  extension). Its questions for the owner are §6.
- Scope: `lib/std/notifications.cee` (`Notifications`, `NotificationError`), `runtime/hosted/ceesafe_rt_notify.c`
  (new), `runtime/android/org/ceesafe/window/Notify.java` (new) and the embedded dex, `ceesafe_rt_window.c` (the class
  for a thread), `src/backend/c_backend.cee`, `tools/gen_runtime.py`, the interpreter, type-system.md §26.22,
  `spec/api/std.md`, `tools/notify_check.py` (new; contract step `notifications`).

## 1. Context

- **A program cannot tell its user anything while it is not on the screen.** A messenger has a message, a mail
  client a mail, a timer its end; each system shows these as notifications, which a CeeSafe program cannot post.
- **The systems' notifications**: Android's `NotificationManager` (a channel since Android 8; the runtime permission
  `POST_NOTIFICATIONS` since Android 13), Apple's `UNUserNotificationCenter` (iOS, and macOS for a bundled app; the
  user grants the app's permission once), Windows' toasts (WinRT, an app identity) and the freedesktop
  notification service on Linux (D-Bus).
- **Push** (a server waking an app that is not running, ADR 0244) delivers into the same notifications: this is the
  half that shows them.

## 2. Decision

1. **`Notifications`** in lib/std, three calls, each blocking and callable from any thread but the window's (an
   answer from the user needs it):
   - `Notifications.requestPermission() -> Result<void, NotificationError>`: asks the user, once, whether the app
     may notify; `Ok` when it may (already, or now), `Denied` when not. Where the system needs no permission, `Ok`.
   - `Notifications.show(id, title, body) -> Result<void, NotificationError>`: shows a notification, replacing the
     one shown with the same `id`. Tapping it opens the app.
   - `Notifications.clear(id) -> Result<void, NotificationError>`: removes it; `Ok` when there is none.
   `id` is 1 to 64 ASCII letters, digits, `.`, `-`, `_` and `:`; `title` at most 256 bytes and `body` at most 4,096
   bytes of UTF-8, and neither holds a zero byte. Anything else is `InvalidInput`, before the system is asked.
2. **`NotificationError`**: `Unsupported` (no notifications here), `InvalidInput`, `Denied` (the user refused, or
   turned the app's notifications off), `Failed`.
3. **Where:**
   - **Android** (a program with an activity): the runtime's Java class `org.ceesafe.window.Notify`, on the calling
     thread attached to the VM. One channel, `ceesafe`, named after the app, high importance (it shows at the top of
     the screen, with the system's sound). The small icon is the app's. A tap starts the app's launch activity, or
     brings the running one forward. The permission is asked as Bluetooth's are (ADR 0177): the system's dialog,
     waited for while the activity has lost the focus. An app declares `POST_NOTIFICATIONS` in its manifest
     (CeeUI's packer does, for an app that notifies); without it Android 13 and later refuse (`Denied`).
   - **iOS, and macOS in a bundled app**: `UNUserNotificationCenter` (UserNotifications.framework, loaded with
     `dlopen`): alert, sound and badge asked for; a request with the id, no trigger (shown at once), the default
     sound. A program with no bundle identifier (every command-line program on macOS) is `Unsupported`, as the
     framework refuses it. While the app is in the foreground iOS shows nothing (the system's rule without a
     delegate): a program notifies when it is not on the screen.
   - **Windows, Linux, FreeBSD, CosmoOS** and `ceesafec run --interp`: `Unsupported` (§6.1).
4. **Testing**: `CEESAFE_NOTIFICATIONS_DUMP=<file>`, natively on every system, appends what the calls would do to the
   file instead (`permission`, `show ID<TAB>TITLE<TAB>BODY`, `clear ID`, one a line, a tab, a newline and a backslash in a
   text written `\t`, `\n` and `\\`), and every call is then `Ok`.

## 3. Consequences

- A program can tell its user something while it is off the screen, with one API on the phones and on macOS.
- Which notification was tapped is not reported to the program yet: the tap opens the app as it was (§6.2).
- The contract suite's `notifications` step checks the API through `CEESAFE_NOTIFICATIONS_DUMP` on every system,
  and `Unsupported` where there is no store of notifications; the systems' own by hand (the owner's Moto and iPhone).

## 4. Options not taken

- **Notifications in CeeUI only**: a program without CeeUI (a tray tool, a service with a window) needs them too,
  and the Android and Apple calls belong with the runtime's other system calls.
- **A rich notification now** (images, actions, reply fields, grouping): each is a system-specific decision; the
  three calls are what every system has.

## 5. Tests

- `tools/notify_check.py` (contract step `notifications`): a program that asks permission, shows, replaces and
  clears notifications and checks each `InvalidInput`; with `CEESAFE_NOTIFICATIONS_DUMP` natively on every system
  (the file's lines compared); without it, `Unsupported` on Linux, FreeBSD, Windows and in a command-line program on
  macOS; interpreted, `Unsupported`.
- By hand, with Marivil's port: on the Moto and the iPhone, a message that arrives while the app is in the
  background is a notification, and a tap opens the app.

## 6. Questions for the owner

1. **Windows and Linux**: toasts (an app identity registered by the installer) and the freedesktop service, in a
   later step? Implemented: `Unsupported`.
2. **The tapped notification**: report its id to the program (`Notifications.opened()`, or a window event), so the
   app opens the chat? Implemented: not yet.
