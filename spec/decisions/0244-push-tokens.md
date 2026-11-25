# ADR 0244 — Push tokens

- Status: **Proposed — implemented** 2026-11-25 on the branch `feat/notifications`, asked for by Marivil's CeeUI port
  (phase 3: notifications while the app is not running; the owner: "Yes" to the iOS half first, then the App ID with
  push, and the server's change deployed to marivil.com). Its questions for the owner are §6.
- Scope: `lib/std/push.cee` (`Push`, `PushToken`, `PushService`, `PushError`), `runtime/hosted/ceesafe_rt_push.c`
  (new), `ceesafe_rt_window.c` (iOS: the app delegate's answers, `ceesafe_ios_push_token`), `src/backend/c_backend.cee`,
  `tools/gen_runtime.py`, the interpreter, type-system.md §26.23, `spec/api/std.md`, `tools/push_check.py` (new; contract
  step `push`).

## 1. Context

- **A phone ends what is not on its screen.** iOS suspends an app within seconds of the home screen, and stops it
  later; Android does the same to an app in the background after a while. A messenger's connection to its server ends
  with it, and a local notification (ADR 0243) can only come from a running app.
- **The systems' answer is push**: the app registers with the system's push service (Apple's APNs, Google's FCM) and
  is given a token; its server sends to the token, and the system shows the notification, or wakes the app, whether
  the app runs or not.
- On iOS the token comes to the app's delegate (`application:didRegisterForRemoteNotificationsWithDeviceToken:`),
  which CeeSafe's runtime makes (ADR 0154), so a program cannot have it without the runtime.

## 2. Decision

1. **`Push.register() -> Result<PushToken, PushError>`** in lib/std: registers with the system's push service and
   returns the device's token, blocking until it comes (30 seconds at most), from any thread but the window's.
   A `PushToken` is its `service` (`PushService.Apns` or `Fcm`), its `token` (APNs: the device token's bytes in hex),
   and `sandbox`: whether the build is a development one, whose pushes go to APNs's sandbox
   (`api.sandbox.push.apple.com`). A program registers each run (a token can change) and sends the token to its server.
2. **`PushError`**: `Unsupported`, `Denied` (the app is not signed for push, or the service refused), `Failed` (no
   answer in time, anything else).
3. **Where:**
   - **iOS** (a windowed app): `UIApplication.registerForRemoteNotifications` on the main thread, and the app
     delegate's answer, waited for. The environment is the build's: a development build carries
     `embedded.mobileprovision`, whose `aps-environment` says `development`; an App Store build carries none and is
     production. The app's signing must grant `aps-environment` (CeeUI's packer's `--push`), which only a profile of
     an explicit App ID with Push Notifications has.
   - **Android** (amended 2026-11-25): FCM, from the app's own Firebase, a later decision (ADR 0249).
   - **macOS**, Windows, Linux, FreeBSD, CosmoOS and `ceesafec run --interp`: `Unsupported` for now (§6.1).
4. **What is shown**: what the server sends. An alert push is shown by the system as it comes; a program that must
   change it before it is shown (a messenger's text is end-to-end encrypted) needs iOS's notification service
   extension, a later step (§6.2).
5. **Testing**: `CEESAFE_PUSH_TOKEN=<text>` is the token instead, natively on every system: `apns:HEX`,
   `apns-sandbox:HEX` or `fcm:TOKEN`, as the runtime gives it to lib/std.

## 3. Consequences

- A CeeSafe app on iOS can be told of what comes while it is not running; with Marivil's server, a message from Bob
  reached the owner's iPhone as a notification with the app suspended (§5).
- The token's text crosses from the runtime to lib/std in one form (`service:token`), which the test variable uses
  too.

## 4. Options not taken

- **Receiving the push's payload in the program**: an alert push needs nothing of the program to be shown; a silent
  push that wakes the app, and the notification extension, are later steps with their own decisions.
- **The token from a delegate the program installs**: the runtime owns the app delegate (ADR 0154), and a program
  should not need Objective-C to have a token.

## 5. Tests

- `tools/push_check.py` (contract step `push`): with `CEESAFE_PUSH_TOKEN` each form is read back (service, token,
  sandbox) and a malformed one is `Failed`; without it, `Unsupported` here (no window); interpreted, `Unsupported`.
- By hand (2026-11-25), Marivil's port on the owner's iPhone 14 Plus, signed with the "Marivil CeeUI Dev" profile
  (App ID `com.murshisoft.marivil.ceeui`, Push Notifications): `Push.register` gave a 32-byte token, `sandbox`;
  the port registered it with marivil.com (server 0.2.105, the topic per device); a message from a second account with
  the app suspended was pushed (`[push:apns] sent alert`) and shown on the iPhone.

## 6. Questions for the owner

1. **Android**: FCM, with Firebase's libraries in the app (CeeUI's packer, ADR 0243's companion), next? Implemented:
   `Unsupported`.
2. **The notification service extension** (iOS): CeeUI's packer builds an extension beside the app, and CeeSafe runs
   a program in it, so the text is decrypted before it is shown, as MAUI's extension does? Implemented: not yet.
