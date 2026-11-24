# ADR 0242 — The system's secret store

- Status: **Proposed — implemented** 2026-11-24 on the branch `feat/secret-store`, released in 0.95.6 (the owner: "Release"), asked for by Marivil's CeeUI port
  (phase 3, phones: "secure storage for the account and the keys", PORT-PLAN.md), on the owner's "continue" after
  that step was named as the port's next. Its questions for the owner are §6.
- Scope: `lib/std/secrets.cee` (`Secrets`, `SecretError`), `runtime/hosted/ceesafe_rt_secrets.c` (new),
  `runtime/android/org/ceesafe/window/Secrets.java` (new) and the embedded dex, `ceesafe_rt_window.c` (the class for
  a thread), `src/backend/c_backend.cee` (the file in a build that calls it), `tools/gen_runtime.py`, the
  interpreter's model (`src/interpreter/io.cee`), type-system.md §26.21, `spec/api/std.md` (generated),
  `tools/secrets_check.py` (new; contract step `secrets`).

## 1. Context

- **An app's secrets have no place in CeeSafe today.** A messenger keeps a session token and the key its stored
  account is encrypted with; a mail client, a password; an API client, a key. The only place a CeeSafe program has
  is a file (`Directory.appData`, ADR 0159), which anything running as the user reads, which a backup copies, and
  which leaves the device with it.
- **Every system CeeSafe ships apps on has a store for them**, kept encrypted under a key the system holds, and not
  readable by other apps: Apple's keychain (macOS and iOS, `SecItem*`), Android's Keystore (a key in the secure
  hardware where there is one; the app seals its values with it), Windows' DPAPI (`CryptProtectData`, under the
  user's logon secret). Linux has the Secret Service (D-Bus; GNOME Keyring, KWallet) where a desktop session runs
  one, and nothing on a server.
- **What MAUI does** (the app being ported): `SecureStorage`, the keychain on iOS and macOS, a Keystore key sealing
  `SharedPreferences` on Android, DPAPI on Windows. The port keeps the token and the pickle key in a file until
  CeeSafe has this.

## 2. Decision

1. **`Secrets`** in lib/std, three calls, each blocking and callable from any thread:
   - `Secrets.read(app, name) -> Result<Vector<u8>?, SecretError>`: the value, or `null` when there is none;
   - `Secrets.write(app, name, value: Slice<u8>) -> Result<void, SecretError>`: makes or replaces it;
   - `Secrets.remove(app, name) -> Result<void, SecretError>`: removes it; `Ok` when there was none.
   `app` names the app's own set of secrets, as `Directory.appData`'s name names its folder: not empty, no `/` or
   `\`, not `.` or `..`, at most 128 bytes. `name` is 1 to 128 bytes of ASCII letters, digits, `.`, `-` and `_`, not
   `.` or `..`.
   A value is at most 65,536 bytes (a store for keys and tokens, not documents). A name or value outside these is
   `SecretError.InvalidInput`, before the system is asked.
2. **`SecretError`**: `Unsupported` (this system has no store CeeSafe uses), `InvalidInput`, `Locked` (the store
   cannot be read until the user unlocks the device or signs in), `Denied` (the system refused this program: an iOS
   app without its keychain entitlement, the user's refusal of a macOS prompt), `Failed` (anything else: the
   Keystore's key was lost, a value no longer opens, an error of the system's).
3. **Where they are kept:**
   - **macOS and iOS**: a generic password item, service `app`, account `name`, accessible after the first unlock,
     this device only (`kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly`: a backup does not carry it, and a
     notification extension can read it while the phone is locked). macOS uses the data protection keychain (iOS's
     kind) where the program is entitled to it, and the user's login keychain otherwise (a program not signed with a
     team's keychain group: every command-line program and every unsigned build). Security.framework is loaded with
     `dlopen`, as Bluetooth's frameworks are: no program links it.
   - **Android**: an AES-256-GCM key in the Android Keystore, alias `ceesafe.secrets.<app>`, made at the first
     write (in StrongBox or the TEE where the device has one; never exportable). Each value is sealed with it
     (a random 12-byte nonce; the app and name as associated data, so one value cannot be passed off as another)
     and kept in the app's no-backup folder, `ceesafe-secrets/<app>/<name>`. The work is the runtime's Java class
     `org.ceesafe.window.Secrets`, from the embedded dex (ADR 0157), on the calling thread attached to the
     activity's VM. A program with no activity (none on Android today) gets `Unsupported`.
   - **Windows**: DPAPI for the current user (`CryptProtectData`, no prompt, the app and name as its entropy), kept
     in `%LOCALAPPDATA%\<app>\secrets\<name>`. crypt32 is loaded with `LoadLibrary`.
   - **Linux, FreeBSD, CosmoOS** and `ceesafec run --interp`: `Unsupported` (§6.1). A program falls back as it
     chooses: Marivil keeps its file there.
4. **Testing**: `CEESAFE_SECRETS_DIR=<folder>`, natively on every system, keeps the values as plain files
   `<folder>/<app>/<name>` instead of the system's store, so a program's own tests run anywhere and touch no keychain.
   Never for a real app's use: the values are not protected. The interpreter answers `Unsupported` to every call,
   with the variable or without, as it does Bluetooth's: it has no system store, and a run stays reproducible.
5. **Writes are whole**: a reader sees the old value or the new, never part of one (the keychain's update is one call;
   Android and Windows write a temporary file, then rename it over the old).

## 3. Consequences

- A program can keep a secret where the system protects it, with one API on every phone and desktop it ships on.
- A value written on one device does not move to another (by design: the Keystore's and DPAPI's keys do not leave
  the device, and the keychain items are "this device only"). A program that moves its data to a new phone moves
  its secrets by its own means.
- **macOS and unsigned builds**: an item in the login keychain belongs to the program that wrote it. A rebuilt
  program is another program to the keychain, so its first read of the item makes macOS ask the user once ("…wants
  to use your confidential information"), and `Denied` if refused. A signed app with a keychain group asks nothing.
- The contract suite's `secrets` step checks the API on every system through `CEESAFE_SECRETS_DIR`, and on macOS and
  Windows the system's own store too (the step's own app name, removed after).

## 4. Options not taken

- **A file encrypted with a key the program holds**: the key would sit beside the file. No protection.
- **Android's `EncryptedSharedPreferences`** (Jetpack Security, MAUI's choice on Android): a library the runtime's dex
  would have to carry, and deprecated by Google in 2025. The Keystore with a file per value is the same protection
  in 150 lines of Java.
- **The Secret Service on Linux now**: a D-Bus client in the runtime (libdbus or sd-bus loaded with `dlopen`) and the
  Secret Service's session encryption, for desktops that run a keyring, for an app no one ships on Linux yet. §6.1.

## 5. Tests

- `tools/secrets_check.py` (contract step `secrets`): a program that writes, reads, replaces and removes values (empty,
  binary, 65,536 bytes), reads one that is not there, removes one that is not there, and checks each `InvalidInput`;
  natively with `CEESAFE_SECRETS_DIR`; then natively without it, against the system's store, on macOS and Windows
  (`Unsupported` checked on Linux and FreeBSD); and interpreted, `Unsupported`.
- Android and iOS by hand, with Marivil's port on the owner's Moto and iPhone: the account kept there, the app
  restarted, still signed in.

As run on the branch (2026-11-24): `secrets_check.py` 3 cases, 0 problems on macOS arm64 (the login keychain, the
values up to 65,536 bytes), on Debian 13 x86_64 (gcc; `Unsupported`) and on Windows 11 arm64 under Parallels (DPAPI;
the compiler from the C seed by `cl`; the program with `-Werror` under clang and `/WX` under cl, after clang's
`-Wcast-qual` found a cast of DPAPI's input); windows.yml and checks.yml on GitHub, success. The Moto g35 (Android
15): Marivil's account moved from `account.json` into the Keystore (the key made in the TEE, Trusty, after StrongBox
answered `HARDWARE_TYPE_UNAVAILABLE`), and a cold start read it back, signed in. The iPhone 14 Plus: the port signed in
on this Mac's server and, started again, opened signed in, its account read from the keychain (signed with the
application identifier and keychain group of a development profile).

## 6. Questions for the owner

1. **Linux**: the Secret Service (GNOME Keyring, KWallet) when a desktop session offers it, `Unsupported` without?
   Implemented: `Unsupported` everywhere on Linux.
2. **macOS without a keychain group**: the login keychain (one prompt after each rebuild for an item an older build
   wrote, §3), or `Unsupported` there, so that only a signed app uses the keychain? Implemented: the login keychain.
