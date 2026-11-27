#!/usr/bin/env python3
"""Device keys (ADR 0275): DeviceKey.obtain, publicKey, sign and remove.

One program is built and run, natively:

1. against the system's store, under a key name of this run's own, removed after: on macOS a key is made (no
   attestation asked: an empty challenge), found again unchanged by a second obtain, its public half read back the
   same; a message signed with it verifies against that SPKI (ECDSA P-256, SHA-256, DER; with Python's
   `cryptography` where it is installed, else the signature's DER shape); after a removal there is no key (publicKey
   and sign answer null) and a second removal is Ok; a name the store does not take is InvalidInput. On Linux,
   FreeBSD and Windows every call is Unsupported;
2. with CEESAFE_SECRETS_DIR naming a folder, on every system: plain files hold no key, so every call is Unsupported;
3. under `ceesafec run --interp`: Unsupported.

usage: python3 tools/device_key_check.py --compiler out/ceesafec
"""

import argparse
import os
import subprocess
import sys
import tempfile

PROGRAM = r'''fn kind(e: SecretError) -> StringView => match (e) {
    SecretError.Unsupported => "unsupported",
    SecretError.InvalidInput => "invalid",
    SecretError.Locked => "locked",
    SecretError.Denied => "denied",
    SecretError.Failed => "failed",
};

fn hex(b: Slice<u8>) {
    let digits: StringView = "0123456789abcdef";
    for (x in b) {
        let hi = (x >> 4) as usize;
        let lo = (x & 15) as usize;
        Console.write(digits[hi..hi + 1]);
        Console.write(digits[lo..lo + 1]);
    }
    Console.writeLine("");
}

fn main() -> i32 {
    let app: StringView = "CeeSafeKeyCheck";
    let name = Environment.variable("KEY_NAME") ?? "check";
    let _ = DeviceKey.remove(app, name);
    match (DeviceKey.obtain(app, name, "".bytes())) {
        Ok(k) => {
            Console.write("first ");
            Console.writeLine(if (k.created) "created" else "found");
            Console.write("attestation ");
            Console.writeLine(match (k.attestation) {
                KeyAttestation.None => "none",
                KeyAttestation.AndroidKeyChain => "android",
                KeyAttestation.AppleAppAttest => "apple",
            });
            Console.write("spki ");
            hex(k.publicKey.view());
        }
        Err(e) => {
            Console.write("first ");
            Console.writeLine(kind(e));
            return 0;
        }
    }
    match (DeviceKey.obtain(app, name, "".bytes())) {
        Ok(k) => {
            Console.write("second ");
            Console.writeLine(if (k.created) "created" else "found");
            Console.write("spki2 ");
            hex(k.publicKey.view());
        }
        Err(e) => {
            Console.write("second ");
            Console.writeLine(kind(e));
        }
    }
    match (DeviceKey.publicKey(app, name)) {
        Ok(Some(p)) => {
            Console.write("public ");
            hex(p.view());
        }
        Ok(null) => { Console.writeLine("public none"); }
        Err(e) => {
            Console.write("public ");
            Console.writeLine(kind(e));
        }
    }
    match (DeviceKey.sign(app, name, "marivil-device-assert.v1|nonce|key|aud".bytes())) {
        Ok(Some(s)) => {
            Console.write("signature ");
            hex(s.view());
        }
        Ok(null) => { Console.writeLine("signature none"); }
        Err(e) => {
            Console.write("signature ");
            Console.writeLine(kind(e));
        }
    }
    Console.write("remove ");
    Console.writeLine(match (DeviceKey.remove(app, name)) { Ok() => "ok", Err(e) => kind(e) });
    Console.write("after ");
    Console.writeLine(match (DeviceKey.publicKey(app, name)) { Ok(Some(_)) => "still", Ok(null) => "none", Err(e) => kind(e) });
    Console.write("sign after ");
    Console.writeLine(match (DeviceKey.sign(app, name, "x".bytes())) { Ok(Some(_)) => "signed", Ok(null) => "none", Err(e) => kind(e) });
    Console.write("remove again ");
    Console.writeLine(match (DeviceKey.remove(app, name)) { Ok() => "ok", Err(e) => kind(e) });
    Console.write("bad name ");
    Console.writeLine(match (DeviceKey.publicKey(app, "no/slash")) { Ok(_) => "taken", Err(e) => kind(e) });
    return 0;
}
'''

MESSAGE = b"marivil-device-assert.v1|nonce|key|aud"


def lines_of(text):
    out = {}
    for line in text.splitlines():
        if line.startswith("sign after ") or line.startswith("remove again ") or line.startswith("bad name "):
            key, _, value = line.rpartition(" ")
            out[key] = value
        elif " " in line:
            key, value = line.split(" ", 1)
            out[key] = value
    return out


def verify(spki_hex, signature_hex):
    """'verified', 'shape' (no cryptography here: the DER's shape only), or why it failed."""
    spki = bytes.fromhex(spki_hex)
    signature = bytes.fromhex(signature_hex)
    try:
        from cryptography.hazmat.primitives import hashes
        from cryptography.hazmat.primitives.asymmetric import ec
        from cryptography.hazmat.primitives.serialization import load_der_public_key
    except ImportError:
        ok = len(spki) == 91 and spki[0] == 0x30 and len(signature) >= 8 and signature[0] == 0x30
        return "shape" if ok else "not DER"
    try:
        key = load_der_public_key(spki)
        if key.curve.name != "secp256r1":
            return f"curve {key.curve.name}"
        key.verify(signature, MESSAGE, ec.ECDSA(hashes.SHA256()))
        return "verified"
    except Exception as e:  # noqa: BLE001 - any failure is a problem to report
        return f"does not verify: {e}"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    problems = []
    cases = 0
    with tempfile.TemporaryDirectory() as work:
        source = os.path.join(work, "key.cee")
        with open(source, "w", encoding="utf-8") as f:
            f.write(PROGRAM)
        exe = os.path.join(work, "key" + (".exe" if os.name == "nt" else ""))
        r = subprocess.run([compiler, "build", source, "-o", exe], capture_output=True, text=True, cwd=work)
        if r.returncode != 0:
            sys.exit(f"device_key_check: building failed:\n{r.stderr}")
        env = dict(os.environ)
        env.pop("CEESAFE_SECRETS_DIR", None)
        env["KEY_NAME"] = f"check-{os.getpid()}"
        r = subprocess.run([exe], capture_output=True, text=True, env=env, timeout=120)
        got = lines_of(r.stdout)
        cases += 1
        if sys.platform == "darwin":
            want = {"first": "created", "attestation": "none", "second": "found", "remove": "ok", "after": "none",
                    "sign after": "none", "remove again": "ok", "bad name": "invalid"}
            for key, value in want.items():
                if got.get(key) != value:
                    problems.append(f"macOS: {key}: expected {value!r}, got {got.get(key)!r}")
            spki = got.get("spki", "")
            if len(spki) != 182 or got.get("spki2") != spki or got.get("public") != spki:
                problems.append("macOS: the public half is not one 91-byte SPKI, the same each time")
            elif "signature" not in got or len(got["signature"]) < 16:
                problems.append(f"macOS: no signature: {got.get('signature')!r}")
            else:
                result = verify(spki, got["signature"])
                if result not in ("verified", "shape"):
                    problems.append(f"macOS: the signature {result}")
                cases += 1
        else:
            if got.get("first") != "unsupported":
                problems.append(f"{sys.platform}: expected Unsupported, got {got.get('first')!r}")
        # Plain files hold no key.
        folder = os.path.join(work, "secrets")
        os.mkdir(folder)
        env["CEESAFE_SECRETS_DIR"] = folder
        r = subprocess.run([exe], capture_output=True, text=True, env=env, timeout=120)
        cases += 1
        if lines_of(r.stdout).get("first") != "unsupported":
            problems.append(f"CEESAFE_SECRETS_DIR: expected Unsupported, got {lines_of(r.stdout).get('first')!r}")
        # The interpreter.
        env.pop("CEESAFE_SECRETS_DIR", None)
        r = subprocess.run([compiler, "run", "--interp", source], capture_output=True, text=True, env=env,
                           cwd=work, timeout=300)
        cases += 1
        if lines_of(r.stdout).get("first") != "unsupported":
            problems.append(f"--interp: expected Unsupported, got {lines_of(r.stdout).get('first')!r} "
                            f"(status {r.returncode}) {r.stderr.strip()[-200:]}")
    for p in problems:
        print(f"  FAIL {p}")
    print(f"device_key_check: {cases} cases: {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
