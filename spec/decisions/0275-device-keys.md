# ADR 0275 — Device keys: a hardware-held signing key, and its attestation

- Status: **Accepted** by the owner 2026-11-28, with §6 answered as recommended (attestation as the MAUI app sends it;
  the phones and macOS; no user presence asked), for Marivil's CeeUI port (its phase 3: "secure storage and the device
  key"). Implemented the same day (§7). The owner chose then, for marivil.com's gateway taking App Attest from the
  MAUI app only, that the port enrolls its iPhone key unattested for now (§6.4).
- Scope: `lib/std/secrets.cee` (`DeviceKey`, `DeviceKeyInfo`, `KeyAttestation`), `runtime/hosted/ceesafe_rt_secrets.c`
  (`ceesafe_secrets_key`), `runtime/android/org/ceesafe/window/Secrets.java`, `src/interpreter/io.cee`,
  type-system.md §26.21, `spec/api/std.md`, `tools/device_key_check.py` (in `tools/contract_tests.py`).

## 1. Context

- The MAUI app signs in silently with a key of the device's: one P-256 key a device, made and kept in hardware (the
  Secure Enclave; the Android Keystore, StrongBox first), its private half never exportable. Its public half enrolls
  with the server once (with the hardware's attestation when the key is new); after that, a session is refreshed by
  signing the server's nonce, in place of a code. The key is not bound to the user's presence: background refreshes
  failed when it was.
- `Secrets` (ADR 0242) keeps bytes; a key that never leaves the hardware is another thing: it signs.

## 2. Decision

```
let key = DeviceKey.obtain("Marivil", "device", nonce.bytes())?;   // made when there is none, attested then
let signature = DeviceKey.sign("Marivil", "device", message)?;       // ECDSA P-256, SHA-256, DER; null: no key
let half = DeviceKey.publicKey("Marivil", "device")?;                 // SPKI DER; null: no key; never makes one
DeviceKey.remove("Marivil", "device")?;
```

- **`obtain(app, name, challenge)`** gives a `DeviceKeyInfo`: the SPKI (91 bytes), whether this call made the key,
  whether hardware holds it (null when the system was not asked: an existing key on Apple's systems), and a new key's
  attestation: `AndroidKeyChain` (the Keystore's certificate chain, the challenge in its attestation extension) or
  `AppleAppAttest` (App Attest's object for a new App Attest key, over SHA-256(challenge || SPKI)), else `None`. An
  empty challenge asks for none.
- **Where**: macOS and iOS, the Secure Enclave, tagged "<app>.<name>", usable after the first unlock, this device
  only; a key in the keychain where the Enclave cannot hold it (an unsigned macOS program, in the login keychain, as
  `Secrets` falls back; a simulator). Android, the Keystore, alias "ceesafe.key.<app>.<name>", StrongBox where the
  device has it, else the TEE. Windows, Linux, FreeBSD, `CEESAFE_SECRETS_DIR` and the interpreter: `Unsupported`.
- Errors are `SecretError`'s; names as `Secrets` takes them.

## 3. Consequences

- Marivil's port enrolls the device's key after a sign-in and refreshes its session with it, as the MAUI app does,
  through marivil.com's `auth/device/challenge`, `enroll` and `assert`.

## 4. Options not taken

- **Keys in `Secrets`' bytes**: a key the program can read can be copied off the device; the point is that it cannot.
- **User presence for every signature**: background refreshes would fail (the MAUI app's experience on the Moto).
- **Windows' TPM and Linux's**: no app of CeeSafe's needs them yet.

## 5. Tests

- `tools/device_key_check.py` (in the contract suite): on macOS a key made, found again, its public half the same,
  a signature that verifies against it (Python's `cryptography`), removed, no key after; Unsupported elsewhere, with
  the test folder, and interpreted.
- The Moto against marivil.com (2026-11-28): enrolled with its attestation chain (the server: "android,
  hardware-tee"); the next start's session came from the key ("assert ok"); the start after kept it.
- The owner's iPhone: §7.

## 6. Questions for the owner (answered 2026-11-28)

1. **Attestation**: both, as the MAUI app (recommended); Android only; none? *Both.*
2. **Systems**: the phones and macOS (recommended), or the phones only? *The phones and macOS.*
3. **The user's presence**: none, silent (recommended), or each signature? *None.*
4. **App Attest at marivil.com**, which takes it from the MAUI app only (`AppAttest:AppId`): enroll the port's iPhone
   key unattested for now (recommended), change the gateway, or try? *Unattested for now.* The App Attest capability
   cannot be added to an App ID through App Store Connect's API ("APP_ATTEST is not a valid value"); the developer
   portal adds it, with the profile made again, when the server takes the port's App ID.

## 7. Implementation (2026-11-28)

- `ceesafe_secrets_key(op, app, name, input, out)`: 0 obtain, 1 the public half, 2 sign, 3 remove; one runtime file
  with `Secrets`, linked by its prefix.
- Apple: Security's `SecKeyCreateRandomKey` (`kSecAttrTokenIDSecureEnclave`, `kSecAccessControlPrivateKeyUsage`,
  `kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly`), `SecItemCopyMatching` by tag, `SecKeyCopyExternalRepresentation`
  (the X9.63 point, after the P-256 SPKI header), `SecKeyCreateSignature`
  (`kSecKeyAlgorithmECDSASignatureMessageX962SHA256`); App Attest through the Objective-C runtime
  (`DCAppAttestService`, its answers from blocks without captures, waited for), `CC_SHA256`.
- Android: `Secrets.keyObtain`, `keyPublic`, `keySign`, `keyRemove` (KeyPairGenerator in "AndroidKeyStore",
  `setAttestationChallenge`, `setIsStrongBoxBacked` with the TEE after; `KeyInfo.isInsideSecureHardware`;
  SHA256withECDSA), one array each way.
- On the way: `Secrets` on Windows stored DPAPI's function addresses through a type-punned pointer, which gcc refuses at
  -O2; copied into the typed pointers now.
- The runtime compiles with `-Wall -Wextra -Wcast-qual -Werror` for macOS (arm64, x86_64), iOS, Android, Linux (gcc 14,
  clang 19) and Windows (MinGW-w64 gcc 16).
