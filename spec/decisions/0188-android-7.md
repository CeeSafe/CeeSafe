# ADR 0188 — Android from 7.1 (API level 25): the runtime's Java loaded from a file, processes started without posix_spawn

- Status: **Proposed**, 2026-11-20. Asked for by the owner ("Start the CeeSafe work for the V2"), to run IPos on a
  Sunmi V2: a handheld till with a printer built in, on Android 7.1.1 (API level 25), 32-bit ARM. Its 32-bit half is
  ADR 0187; this one is the Android release.
- Scope: the hosted runtime's Android window code (`runtime/hosted/ceesafe_rt_window.c`: how its Java classes are
  loaded) and its processes (`runtime/hosted/ceesafe_rt_io.c`: `Process.run` and the rest without `posix_spawn`); the
  dex's minimum API (`tools/gen_android_dex.py`); ADR 0154's floor, from 28 to 25; ClayWind's claywind-pack (the
  manifest's `minSdkVersion` and the compiler's API level). No language change, no library API change.

## 1. Context

- **The device.** The Sunmi V2 runs Android 7.1.1, API level 25 (checked over adb: `ro.build.version.sdk=25`). Its
  printer is offered to apps as a paired Bluetooth device ("InnerPrinter", `ro.sunmi.simulate_bluetooth=true`), so
  IPos's Bluetooth printing (ADR 0177) reaches it as it reaches the van's printer, once IPos runs there.
- **The floor today.** ADR 0154 made API level 28 (Android 9) the floor, as the oldest the work was tried on, not for
  any one call. Three things hold it there now:
  1. The runtime's Java (the input view, the file picker, the screen reader's view, Bluetooth, printing) is one dex,
     embedded in the runtime as bytes, loaded with `dalvik.system.InMemoryDexClassLoader`, which Android has from API
     level 26 (Android 8.0). On 25 the class is not there: the window works, but text input, file dialogs, the screen
     reader, Bluetooth and printing do nothing.
  2. `Process.run` and its kin start a program with `posix_spawn`, which bionic has from API level 28. Built for 25,
     the runtime does not compile (the NDK's headers leave the calls out: checked, `posix_spawnp` and six more in
     `ceesafe_rt_io.c`).
  3. claywind-pack writes `minSdkVersion="28"` and compiles with the NDK's `…28-clang`, so the system refuses to
     install the app at all.
- **Nothing else.** Compiled against API level 25's headers, every other runtime file builds (`ceesafe_rt_window.c`,
  `_async`, `_thread`, `_hosted`, `_bluetooth`, `_float`, the core); the Java's calls are all from API levels 19 to 23,
  but for the Bluetooth permissions of API level 31, which are behind a check already.

## 2. Decision

### 2.1 The Java, from a file below API level 26

Where `InMemoryDexClassLoader` is missing (the class is not found), the runtime writes the embedded dex to a file in
the app's code cache (`Context.getCodeCacheDir()`, API level 21; the file `ceesafe-runtime-<hash>.dex`, the hash the
dex's first eight bytes of SHA-256, which the generator records, so an updated app never loads an old file) and loads
it with `dalvik.system.DexClassLoader(path, codeCacheDir, null, activity's loader)`, which every release has. The file
is written once (it is there and the right size: used as it is), made read-only, and kept. Both window paths that load
the classes (`cee_android_classes`, `ceesafe_android_bluetooth`) share one loader, made once. From API level 26 nothing
changes: the dex stays in memory.

### 2.2 Processes without posix_spawn below API level 28

The runtime is compiled for the app's floor (`__ANDROID_API__`), so a call newer than it must be weak and asked for
at run time: `posix_spawn` and the six calls with it are used under `__builtin_available(android 28, *)` (the NDK's
`__ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__`, which the runtime defines before its headers on Android). Below 28 the
same work is done with `vfork` and `execvp`/`execve`: the file actions (stdin, stdout, stderr opened on the files the
call names), the inherited descriptors and the closing of the rest done in the child before `exec`, a failure of
`exec` sent back through a close-on-exec pipe, as `posix_spawn` reports it. What a program sees is the same: a started
process, or the error. (An app rarely starts a program on a phone; the calls must work, not be fast.)

### 2.3 The floor

- API level 25 becomes ADR 0154's floor: the runtime builds and its Android checks run for it; `gen_android_dex.py`
  runs d8 with `--min-api 25` (its lambdas desugared as 25 needs).
- claywind-pack (ClayWind's repository) writes `minSdkVersion="25"` and compiles with `…25-clang`, for every ABI. The
  Bluetooth permissions it writes already cover 25 (`BLUETOOTH`, `BLUETOOTH_ADMIN`, and `ACCESS_FINE_LOCATION` for a
  scan, asked for at run time from API level 23 by the Java that is there).
- Below 25 is not a goal (Android 7.0 and older are a few devices, and NativeActivity's behaviour changes there).

### 2.4 Tests

- Android's checks (the window checks, the Bluetooth script backend, a process started) run on an API level 25
  emulator image (x86_64; the arm64 image where the host is ARM) besides the newest one, in the contract suite's
  Android job.
- A check that the dex path writes, reuses and refuses a wrong-size file.
- On the device, by the owner: IPos on the Sunmi V2 (with ADR 0187), a receipt printed on its own printer.

## 3. Options not taken

- **The Java in the app's own dex** (`classes.dex` in the APK, `hasCode="true"`; the runtime already looks in the
  app's loader first). It would need claywind-pack to compile and dex Java, a JDK and d8 on every build machine, which
  CeeSafe has kept off them (the dex is committed bytes). Still possible for an app that wants it: nothing here stops
  it.
- **Process calls Unsupported below 28.** Simpler, but a program that works on one phone would fail on another for a
  reason it cannot see; the fallback is small.
- **Two runtimes, one per floor.** Twice the testing for no gain: the checks at run time cost one branch.

## 4. Consequences

- Programs built by ceesafec run from Android 7.1. With ADR 0187, IPos runs on the Sunmi V2 and prints on its own
  printer.
- ADR 0154's floor, the book's page on phones and ClayWind's README say API level 25.
- The contract suite gains an API level 25 image to run.

## 5. Order of work

1. The dex from a file (2.1), and d8's `--min-api 25`; checked on an API level 25 emulator.
2. The process fallback (2.2); checked there too.
3. The floor (2.3): the suite's image, the docs; claywind-pack's `minSdkVersion` and `…25-clang`.
4. With ADR 0187: IPos on the Sunmi V2.
