# ADR 0249 — Push on Android: Firebase Cloud Messaging, from the app's own copy

- Status: **Proposed — implemented** 2026-11-25 on the branch `feat/notifications`, asked for by Marivil's CeeUI port
  (phase 3: push on Android, the owner's choice of a Gradle build for an app with Firebase; the app's entry in the
  Firebase project made with the owner). Its questions for the owner are §6.
- Scope: `runtime/android/org/ceesafe/window/Push.java` (new, in the embedded dex), `runtime/hosted/ceesafe_rt_push.c`
  (Android's token), `runtime/hosted/ceesafe_rt_window.c` (the class; the property `ceesafe.program`),
  `tools/gen_android_dex.py`, type-system.md §26.23. Amends ADR 0244 §2.3, which left Android Unsupported.

## 1. Context

- **Android's push service is Firebase Cloud Messaging (FCM)**, a library each app carries (Firebase Messaging and
  some ninety AndroidX, Play services and Kotlin libraries), configured by the app's `google-services.json`; there is
  no system call for a token as iOS's `registerForRemoteNotifications`.
- **CeeSafe's runtime builds without Gradle or a JDK** (ADR 0157: its Java is a committed dex), and most programs need
  no push: the runtime cannot carry Firebase.
- **A push comes while the program is not running**: the system starts the app's `FirebaseMessagingService` for it,
  in the app's process, before any native code is loaded. Marivil's server sends data-only messages (title, body,
  conversation) and leaves showing them to the app.

## 2. Decision

1. **The app carries Firebase; the runtime finds it.** `Push.register()` on Android asks
   `FirebaseMessaging.getInstance().getToken()` through reflection on the app's class loader (the runtime's
   `org.ceesafe.window.Push`), waits up to 30 seconds on the program's thread, and gives `fcm:TOKEN`. An app without
   Firebase Messaging is `Unsupported`, as before; no token in time (no Google Play services, no network, no
   configuration) is `Failed`.
2. **The program's presence is a property.** From the activity's first start until the process ends with `main`, the
   Java system property `ceesafe.program` is `running`. An app's own Java in the same process (its messaging service)
   leaves a push to a running program, which has the message over its own connection and shows it itself.
3. **Building is the packer's.** CeeUI's `ceeui-pack --firebase google-services.json` builds such an APK with Gradle
   (a generated project and a pinned wrapper) and adds a messaging service that shows a data push on the channel
   `Notifications` use while the program is not running (§2.2).

## 3. Consequences

- A CeeSafe app on Android receives pushes while it is not running: Marivil's port shows "Bob / Sent you a message".
- Showing a push's real content needs the program: on Android the service would start the program's code in the same
  process (as iOS's extension, ADR 0246); not yet (§6.1).

## 4. Options not taken

- **The runtime links Firebase**: every program would carry it, and the runtime's build would need Gradle.
- **ceeui-pack merges the AARs itself** (manifests, resources, R classes, dex): no Gradle, but a large piece of build
  machinery to keep up with Firebase's (the owner chose Gradle).
- **FCM's HTTP API without the library**: an app cannot get a token without Google Play services' client.

## 5. Tests

- `tools/push_check.py`: unchanged on the desktop (the token's text, `CEESAFE_PUSH_TOKEN`).
- By hand (2026-11-25), the owner's Moto with Marivil's port against marivil.com: Firebase initialized in the APK and
  the token registered with the server; with the app's process ended, a message from Bob started the service, which
  showed it (tag `push:<conversation>`, channel `ceesafe`, heads-up); with the app open, the service left the next one
  to the program, which showed it decrypted (tag `u:<sender>`), and no second notification came.

## 6. Questions for the owner

1. **The real content on Android**: the messaging service could start the program's code in its process and hand it
   the push (`NotificationExtension.next`, ADR 0246), so the notification shows the decrypted text, as on the iPhone?
2. **A token that changes** (`onNewToken`): registered at the app's next start; a way to tell a running program?
