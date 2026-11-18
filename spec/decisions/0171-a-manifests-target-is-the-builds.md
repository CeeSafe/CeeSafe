# ADR 0171 — A manifest's target is the build's, and phones are targets

- Status: **Proposed**, 2026-11-18. Asked for by the owner for ClayWind ("The most important gate I see is HTTPS on
  phones. We need to tackle it urgently"): an app that uses CeeSafe.Web's `web_tls` cannot be built for a phone.
- Scope: `ceesafe.toml`'s target tables (`src/packages/manifest.cee`, tooling.md §4.1; ADR 0047 §3.1), a `--target`
  option of `build`, `run`, `test` and `check` (`src/main.cee`, `src/host/build.cee`). No change to the language, to
  code generation or to the runtime.

## 1. Context

ADR 0047 gave a manifest `[target.linux]`, `[target.macos]`, `[target.windows]` and `[target.freebsd]`, each with
`links`, `lib-dirs` and `include-dirs`, and said the table that applies is the host's: the system `ceesafec` runs on.
That was right while every build was for the host.

ADR 0154 made iOS and Android targets. A build for them runs on a Mac with another C compiler (`--cc`, `--cflag`):
`ceesafec` itself does not know what the build is for, and applies `[target.macos]`.

What that does today, measured with ceesafec 0.66.0 on an app that uses `web_tls` (whose `[target.macos]` names
Homebrew's OpenSSL):

- **Android.** `claywind-pack android` reports success. The app's `libmain.so` has 88 undefined OpenSSL symbols
  (`OPENSSL_init_ssl`, `SSL_*`, `X509_*`) and needs no library that has them: the linker took the `-L` and `-l` of the
  Mac's OpenSSL, found nothing for Android there, and a shared library may have undefined symbols. The app cannot
  load on a phone.
- **iOS.** The link is given the Mac's `libssl` and `libcrypto`, which are not built for iOS.

So a package cannot say "on Android, link this; on iOS, that", and a package that says what a Mac needs breaks every
phone build of the programs that use it.

## 2. Options

| | A. `--target` names the system (proposed) | B. Guess it from `--cc` and `--cflag` | C. Leave it to the packer |
|---|---|---|---|
| What | `ceesafec build --target android` applies `[target.android]`; without it, the host's, as today | look for `-target …-android…`, `-isysroot …iPhoneOS…` in the flags | `claywind-pack` passes `-L` and `-l` itself |
| Cost | one option; packers pass it | a guess that is silently wrong for a flag it does not know | every packer must know every dependency's libraries, which is what ADR 0047 took away; and the host's table still applies |

**A is proposed.** B turns a compiler's flags into meaning; C does not stop the host's table from applying.

## 3. Proposed decision

### 3.1 The targets

A manifest may have `[target.ios]` and `[target.android]`, beside the four of ADR 0047, with the same three fields
and the same rules (ADR 0047 §3.1 to §3.4): names and directories are literal, a directory may start with `$NAME` or
`${NAME}`, and a field that is not one of the three is an error.

`[target.ios]` is for a device and for the simulator alike: they differ in the archive, which the directory chooses
(`lib-dirs = ["$OPENSSL_IOS_DIR/lib"]`, set by who builds).

### 3.2 `--target`

`ceesafec build --target NAME` (and `run`, `test`, `check`, which read manifests the same way), where NAME is one of
`linux`, `macos`, `windows`, `freebsd`, `ios`, `android`:

- the manifest table that applies, in the package and in every dependency, is `[target.NAME]` and no other;
- without `--target`, it is the host's, as today: no existing build changes;
- any other NAME is an error that lists the six.

`--target` names the system and nothing else. It does not choose a C compiler, a sysroot or an architecture: `--cc`
and `--cflag` still do, as ADR 0154 has it. It is not recorded in `ceesafe.lock`, and it does not change a package's
hash (the manifest's text does, and that is unchanged).

`--target ios` or `android` with `run` or `test` is an error: the host cannot run what it built.

### 3.3 A target with no table

A package with no table for the build's target adds nothing, as today. That is what makes a phone build of a package
that only names a Mac's libraries stop linking them: the link then fails for want of the symbols, and says which,
where the linker reports undefined symbols. For Android's shared library it does not by default; that is the
packer's to ask for (`-Wl,--no-undefined`), outside this ADR.

## 4. What it is for, outside this repository

These are not part of the decision; they are why it is asked for, and each is its owner's to accept.

- **CeeSafe.Web, `web_tls`:** `[target.android]` and `[target.ios]` naming a static OpenSSL 3 built for each
  (`links = ["ssl", "crypto"]`, `lib-dirs = ["$OPENSSL_ANDROID_DIR/lib"]`), a script that builds those archives from
  OpenSSL's sources with the NDK and Xcode, and the system's roots there: Android keeps them as PEM files in
  `/apex/com.android.conscrypt/cacerts` and `/system/etc/security/cacerts`; iOS gives no files, so the server's chain
  is given to the system to judge (`SecTrustEvaluateWithError`).
- **ClayWind, `claywind-pack`:** passes `--target ios` or `--target android`, sets the variables the tables name for
  each Android ABI, and asks Android's link to refuse undefined symbols.

## 5. Questions for the owner

1. **The option's name.** `--target` (proposed), or `--os`, since it names a system and not a triple? Recommended:
   `--target`, the word the manifest's tables already use.
2. **Android's ABIs.** One `[target.android]` whose directory comes from a variable the packer sets for each ABI
   (proposed), or a table for each (`[target.android-arm64]`)? Recommended: one table; a table for each ABI multiplies
   the manifest for what is one directory's difference.
3. **`check --target`.** Accepted and used to choose the table (proposed), so that a manifest's phone tables are
   checked on a desktop, or refused as meaningless? Recommended: accepted.

## 6. Tests

- A manifest with `[target.ios]` and `[target.android]` is accepted; an unknown field in one is the error ADR 0047
  gives; `[target.haiku]` is still the error that lists the tables, now six.
- A package with different `links` under `[target.macos]` and `[target.android]`: `--emit c` with `--target android`
  on a Mac lists Android's and none of the Mac's; without `--target`, the Mac's.
- A dependency's tables follow the build's `--target`.
- `--target plan9` is an error; `run --target android` is an error.
