# ADR 0263 — A notification extension on Android: the push's real content while the app is not running

- Status: **Accepted** by the owner 2026-11-26, with §6 answered as recommended (a process of its own, 8 seconds, the
  extension kept running), for Marivil's CeeUI port (phase 3), answering ADR 0249 §6.1.
- Scope: `runtime/android/org/ceesafe/window/` (a new `Extension` class in the embedded dex),
  `runtime/hosted/ceesafe_rt_push.c` and `ceesafe_rt_window.c` (`NotificationExtension` on Android; the program's
  presence), type-system.md §26 (`NotificationExtension`, `AppGroup` on Android); CeeUI's packer (`ceeui-pack
  android --extension DIR`, its `PushService`). Amends ADR 0249 §2.2 (how the service knows the program runs) and
  ADR 0246 §6.2.

## 1. Context

- On iOS a push is shown with its real text: the app's notification service extension, a second CeeSafe program
  (ADR 0246), reads the waiting envelopes, decrypts them and delivers "Bob" and his words. On Android the push is shown
  as the server sent it ("Sent you a message"): the APK's Firebase service (ADR 0249 §2.3) is Java and runs no
  program code (ADR 0249 §3, §6.1).
- Android needs no second bundle: the system starts the app's own `FirebaseMessagingService` for a push, in the app's
  sandbox, with its files and its Keystore. What is missing is a way for that service to run CeeSafe code and wait for
  its answer.
- **Time.** `onMessageReceived` runs on a background thread the library gives it; Firebase documents about 20 seconds
  for it to finish (less is safer: a high-priority message may lose its priority if no notification follows soon).
- **Two programs, one store.** The extension opens the app's database. sqlite-ea (the engine's owner) confirms two
  connections exclude each other only with open-file-description locks (ADR 0220 §2.2, `F_OFD_SETLK`), which a 64-bit
  Android build has and a 32-bit one (`armeabi-v7a`, `x86`) does not: there the runtime falls back to process locks,
  which do not separate two connections of one process. `ceeui-pack` builds `arm64-v8a` by default and
  `armeabi-v7a` on request.
- **Two runtimes, one process.** Each CeeSafe program carries its own runtime; two in one process are safe only if
  neither resolves the other's symbols and they never both own the window, the activity's callbacks or a JavaVM
  attachment the other expects.

## 2. Decision

1. **The extension is a CeeSafe program packed beside the app's**: `ceeui-pack android --extension DIR` builds the
   package in DIR (Marivil's `marivil_host/nse`, the same program as on iOS) into `lib/<abi>/libextension.so`, beside
   the app's `libmain.so`. The two are never loaded in one process (§2.2), so neither can bind to the other's
   runtime.
2. **It runs in a process of its own**, `android:process=":push"`, the service with it. One answer for every ABI: the
   app's program and the extension never share a process, so the store's locks work whatever the build (§1), a
   crash or a leak in the extension does not touch the app, and only one runtime is ever in a process.
3. **The service starts it and hands it the push.** For a push while the app's program is not running (§2.4), the
   service loads `libextension` once in its process, starts the program's `main` on a thread of its own (once: it
   then waits in `NotificationExtension.next` for the next push, as on iOS), and gives it the push: `title`, `body`
   and the data payload as a JSON object. `NotificationExtension.next(ms)` returns it; `deliver(request, title, body)`
   shows it on the channel `Notifications` use (an empty title keeps the push's), tagged as the push would have been.
   A push not delivered within **8 seconds** is shown as it came, as iOS does when its time runs out.
4. **The program's presence across processes.** ADR 0249's system property is seen only in its own process. Instead,
   while the app's program runs, its runtime holds an exclusive lock on the file `ceesafe-program.lock` in the app's
   files folder (a process lock is enough between processes, on every ABI). The service tests it
   (`FileChannel.tryLock`, a moment's lock): held means the program has the message over its connection and shows it
   itself (ADR 0249 §2.2, unchanged in effect). The property stays, for a service an app keeps in its own process.
5. **What the extension may use.** It is a hosted program in the app's sandbox: `Directory.appData`, `Secrets` (the
   app's own Keystore key) and the network, as the app. `AppGroup.folder(group)` gives the app's files folder on
   Android (both processes have it), so a program written for iOS's group finds the same store; `Secrets.readIn`
   with a group reads the app's own items there. The runtime's Java classes take the service's `Context` in place of
   the activity (`Notify.permission` alone needs an activity: in an extension it answers Denied).
6. **Testing.** `CEESAFE_PUSH_REQUEST` (ADR 0246 §2.5) is unchanged and works for the Android build's logic on a
   desktop; a device test sends a push to the Moto with the app not running (§5).

## 3. Consequences

- Marivil on Android shows "Bob" and his words for a push while the app is not running, from the same extension
  program as the iPhone's; nothing in Marivil's code changes but its packing.
- A push costs a process start the first time (the `:push` process stays while Android keeps it).
- The app's program and the extension may both run, in two processes, against one store, as on iOS.

## 4. Options not taken

- **The extension in the app's process.** No second process and ADR 0249's property as it is; but on a 32-bit build
  the store's locks would not separate the two programs (§1, sqlite-ea), two runtimes would share one process, and the
  extension's failures would be the app's. Possible as an option for 64-bit-only builds, not as the rule.
- **Starting the app's own program for a push** (one program, a mode): its `main` builds a window and a UI; the
  extension's is small, and on iOS it is a separate program anyway.
- **Decrypting in Java**: the app's encryption, store and server code are CeeSafe and Rust; a second copy in Java is
  what the port exists to avoid.
- **WorkManager for the work**: a later, slower path, for a fetch and decrypt that 8 seconds within
  `onMessageReceived` would not cover; to be measured on the Moto (§5) before it is needed.

## 5. Tests (proposed)

- `tools/push_check.py`: `AppGroup.folder` on an Android build is the files folder; the presence lock is held while a
  program runs and free after.
- On the owner's Moto with Marivil's port against marivil.com: a message from Bob while the app is not running is
  shown as "Bob" and his text; the same with the app open shows the app's own notification only; the extension's log
  (`nse.log`, as on iOS) says what it did.

## 6. Questions for the owner (answered 2026-11-26)

1. **Process**: the extension in a process of its own (`:push`, recommended), or in the app's process for 64-bit-only
   builds? *A process of its own.*
2. **Time**: deliver within 8 seconds, then show the push as it came (recommended)? *8 seconds.*
3. **Lifetime**: the extension's program keeps running in `:push` after a push, ready for the next, until Android ends
   the process (recommended), or ends after each push? *Kept running.*

## 7. Implementation (2026-11-26)

- `runtime/hosted/ceesafe_rt_window.c` (Android): `JNI_OnLoad` registers `start` and `push` on the APK's class
  `org.ceesafe.Extension` when the library is loaded by `System.loadLibrary`; `start` keeps the service's application
  context (a global reference) and runs `main` on a thread of its own, its output to logcat as an activity's;
  `push` queues the push (title, body, the payload's JSON, the notification's tag) and waits for a delivery or the
  time; `ceesafe_android_push_extension_next` and `_deliver` serve `NotificationExtension` (a delivery is
  `Notify.show` under the push's tag). The accessors of the runtime's Java classes and `ceesafe_android_vm` use the
  extension's context and VM where there is no activity. At the activity's first start the runtime holds
  `files/ceesafe-program.lock` (§2.4).
- `ceesafe_android_string`: a Java String from standard UTF-8 (`new String(bytes, "UTF-8")`), now used for
  notifications' texts too: `NewStringUTF` takes modified UTF-8, in which an emoji's four bytes are not valid
  (CheckJNI aborts on them), a bug of ADR 0243's Android notifications found here.
- `runtime/hosted/ceesafe_rt_push.c`: `next` and `deliver` reach the window runtime's (weak); `AppGroup.folder` is the
  app's files folder (`ceesafe_host_android_files`, `ceesafe_rt_io.c`).
- `runtime/hosted/ceesafe_rt_io.c`: an app's folders are found from a process named `<package>:<name>` too (before,
  `Directory.appData` failed in any service process of an app).
- `runtime/android/org/ceesafe/window/` `Notify`, `Secrets` and `Push` take a `Context`; the dex regenerated.
- CeeUI's packer (`ceeui-pack android --firebase … --extension DIR`): `libextension.so`, the class
  `org.ceesafe.Extension`, and `PushService` in `:push`.
