# ADR 0246 — A notification service extension, and what an app shares with it

- Status: **Proposed — implemented** 2026-11-25 on the branch `feat/notifications`, asked for by Marivil's CeeUI port
  (phase 3, step 3: "iOS extension", the owner's choice; the App ID, the App Group and the profiles made with the
  owner). Its questions for the owner are §6.
- Scope: `lib/std/push.cee` (`NotificationExtension`, `PushRequest`, `AppGroup`), `lib/std/secrets.cee`
  (`Secrets.readIn`, `writeIn`, `removeIn`), `runtime/hosted/ceesafe_rt_window.c` (iOS: an extension's start, its
  principal class, the queue of pushes), `runtime/hosted/ceesafe_rt_push.c`, `runtime/hosted/ceesafe_rt_secrets.c` (a
  keychain group; the app's own group named when none is), the interpreter, type-system.md §26.23, `spec/api/std.md`,
  `tools/push_check.py`.

## 1. Context

- **A push carries no text a messenger can show**: the server cannot read an end-to-end encrypted message, so it sends
  "Sent you a message". iOS lets an app change a push before it is shown, in a **notification service extension**: a
  second program in the app's bundle (`PlugIns/<name>.appex`), started by the system for a push with `mutable-content`,
  given about 30 seconds and 24 MB.
- **The extension needs what the app keeps**: its account and keys and its store. A separate process with its own
  sandbox reaches the app's data only through an **App Group** (a folder both may use) and a **keychain group**.
- MAUI's extension (`MarivilNotificationService`) does this with `group.com.murshisoft.messenger`.

## 2. Decision

1. **An extension's program is a windowed CeeSafe program** whose bundle is an `.appex`: at its start the runtime
   defines `CeeSafeNotificationService` (a `UNNotificationServiceExtension`, the principal class the extension's
   Info.plist names), runs the program's `main` on a thread of its own, and gives the main thread to `NSExtensionMain`.
2. **`NotificationExtension.next(milliseconds) -> Result<PushRequest?, PushError>`**: the next push iOS handed the
   extension (its `title`, `body`, and its payload, `userInfo`, as JSON), or null within the time.
   **`NotificationExtension.deliver(request, title, body)`** shows it so (an empty title keeps the push's). A push not
   delivered when iOS's time runs out is shown as it came (`serviceExtensionTimeWillExpire`). Elsewhere, and in an app,
   `Unsupported`.
3. **`AppGroup.folder(group) -> String?`**: the App Group's folder (Apple's systems,
   `containerURLForSecurityApplicationGroupIdentifier:`), null where there is none.
4. **`Secrets.readIn/writeIn/removeIn(group, app, name)`**: the secret store in the keychain group `group` (an App Group,
   which iOS takes as a keychain access group, or a team-prefixed one), on Apple's systems; elsewhere the group changes
   nothing. And **a call without a group names the app's own group**: without one, a keychain delete takes the item of
   the same service and account in every group the app reaches, the shared group's too (found on the owner's iPhone,
   §5). The runtime learns the app's own group once, from a probe item the system places.
5. **Testing**: `CEESAFE_PUSH_REQUEST=<file>` is one push, on every system (title, body, payload on three lines), and a
   delivery is appended to `CEESAFE_NOTIFICATIONS_DUMP` as `deliver TITLE<TAB>BODY`.
6. **Building**: CeeUI's packer builds the extension (`--extension DIR`, `--group ID`): the package in DIR into
   `PlugIns/<app>Notifications.appex`, identifier `<app's>.NotificationService`, its Info.plist with the extension
   point and the principal class, signed with its own profile and the group.

## 3. Consequences

- A CeeSafe app on iOS can show a push's real content when it is not running: Marivil's iPhone shows "Bob" and his
  words, decrypted in the extension.
- Whatever an app shares with its extension goes in the group: Marivil's store moves to the group's folder, and its
  account to the group's keychain, once.

## 4. Options not taken

- **An extension in Swift or Objective-C linked with the CeeSafe program**: a second language and toolchain in every
  app's build; the runtime already drives Objective-C classes (ADR 0154).
- **The program called on the main thread for each push**: the program's `main` owns its thread, as an app's does; the
  pushes are queued for it.

## 5. Tests

- `tools/push_check.py`: the extension through `CEESAFE_PUSH_REQUEST` (a push read and delivered, then none), no
  extension `Unsupported`, the App Group's folder (a path on macOS), `Secrets` in a group.
- By hand (2026-11-25), the owner's iPhone 14 Plus with Marivil's port against marivil.com: a message from Bob, sent
  while the app was not running, was shown as "Bob" and his text. On the way, the iPhone's log showed the account lost
  where the app moved it into the group and then removed its old copy (§2.4, fixed).

## 6. Questions for the owner

1. **Other extensions** (a share extension, a widget): the same start, with their own principal classes, when an app
   needs them?
2. **Android**: its pushes reach the app's own process (FCM's service), so no extension is needed there; with ADR
   0244's FCM step.
