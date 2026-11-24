#!/usr/bin/env python3
"""Push tokens (ADR 0244): Push.register, natively and interpreted.

One program is built and run:

1. natively with CEESAFE_PUSH_TOKEN set to each form the runtime gives (apns:HEX, apns-sandbox:HEX, fcm:TOKEN): the
   service, the token and sandbox are read back; a malformed one (no prefix, an empty token) is Failed;
2. natively without it: Unsupported (a command-line program has no window, and only an iOS app registers);
3. under `ceesafec run --interp`: Unsupported, the variable set or not;
4. a notification service extension (ADR 0246), with CEESAFE_PUSH_REQUEST naming a push in a file: next() gives its
   title, body and payload once, then nothing; deliver() writes `deliver TITLE<TAB>BODY` to
   CEESAFE_NOTIFICATIONS_DUMP; without the file, Unsupported (no extension); an App Group's folder is a path on
   macOS and null elsewhere; Secrets.readIn/writeIn/removeIn in a group, with CEESAFE_SECRETS_DIR.

usage: python3 tools/push_check.py --compiler out/ceesafec
"""

import argparse
import os
import subprocess
import sys
import tempfile

PROGRAM = r'''fn main() -> i32 {
    match (Push.register()) {
        Ok(t) => {
            Console.write(match (t.service) {
                PushService.Apns => "apns ",
                PushService.Fcm => "fcm ",
            });
            Console.write(if (t.sandbox) "sandbox " else "production ");
            Console.writeLine(t.token.view());
        }
        Err(PushError.Unsupported) => { Console.writeLine("unsupported"); }
        Err(PushError.Denied) => { Console.writeLine("denied"); }
        Err(PushError.Failed) => { Console.writeLine("failed"); }
    }
    return 0;
}
'''

EXTENSION = r'''fn main() -> i32 {
    match (NotificationExtension.next(100)) {
        Ok(Some(push)) => {
            Console.write("push ");
            Console.write(push.title.view());
            Console.write("|");
            Console.write(push.body.view());
            Console.write("|");
            Console.writeLine(push.payload.view());
            Console.writeLine(if (NotificationExtension.deliver(push, "Bob", "the text") is Ok(_)) "delivered" else "not delivered");
        }
        Ok(null) => { Console.writeLine("none"); }
        Err(PushError.Unsupported) => { Console.writeLine("unsupported"); }
        Err(_) => { Console.writeLine("failed"); }
    }
    match (NotificationExtension.next(10)) {
        Ok(null) => { Console.writeLine("then none"); }
        Ok(Some(_)) => { Console.writeLine("then another"); }
        Err(_) => { Console.writeLine("then an error"); }
    }
    match (AppGroup.folder("group.com.example.ceesafe-check")) {
        Some(path) => { Console.writeLine(if (path.view().endsWith("group.com.example.ceesafe-check")) "group folder" else "group elsewhere"); }
        null => { Console.writeLine("no group folder"); }
    }
    let wrote = Secrets.writeIn("group.com.example.ceesafe-check", "ceesafe-push-check", "shared", "kept".bytes()) is Ok(_);
    let read = match (Secrets.readIn("group.com.example.ceesafe-check", "ceesafe-push-check", "shared")) {
        Ok(Some(v)) => StringView.fromUtf8(v.view()) == Some("kept"),
        _ => false,
    };
    let removed = Secrets.removeIn("group.com.example.ceesafe-check", "ceesafe-push-check", "shared") is Ok(_);
    Console.writeLine(if (wrote && read && removed) "group secret" else "no group secret");
    Console.writeLine(if (Secrets.readIn("", "a", "b") is Err(SecretError.InvalidInput)) "empty group invalid" else "empty group taken");
    return 0;
}
'''

CASES = [
    ("apns:00ff10ab", "apns production 00ff10ab\n"),
    ("apns-sandbox:737d0e3f", "apns sandbox 737d0e3f\n"),
    ("fcm:dGVzdA:APA91b-x_y", "fcm production dGVzdA:APA91b-x_y\n"),
    ("gcm:abc", "failed\n"),
    ("apns:", "failed\n"),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    problems = []
    cases = 0
    with tempfile.TemporaryDirectory() as work:
        source = os.path.join(work, "main.cee")
        exe = os.path.join(work, "push.exe" if os.name == "nt" else "push")
        with open(source, "w", encoding="utf-8") as f:
            f.write(PROGRAM)
        built = subprocess.run([compiler, "build", source, "-o", exe], capture_output=True, text=True)
        if built.returncode != 0:
            print(f"FAIL build: {built.stderr.strip()[:800]}")
            print("push_check: 0 cases: 1 problems")
            return 1

        def run(token, want, interp=False):
            nonlocal cases
            cases += 1
            env = dict(os.environ)
            env.pop("CEESAFE_PUSH_TOKEN", None)
            if token is not None:
                env["CEESAFE_PUSH_TOKEN"] = token
            command = [compiler, "run", "--interp", source] if interp else [exe]
            result = subprocess.run(command, capture_output=True, env=env)
            out = result.stdout.decode("utf-8", "replace").replace("\r\n", "\n")
            label = f"{'interpreted ' if interp else ''}{token!r}"
            if result.returncode != 0:
                problems.append(f"{label}: exit {result.returncode}: {result.stderr.decode('utf-8', 'replace')[:300]}")
            elif out != want:
                problems.append(f"{label}: printed {out!r}, not {want!r}")

        for token, want in CASES:
            run(token, want)
        run(None, "unsupported\n")
        run("apns:00ff10ab", "unsupported\n", interp=True)

        # The extension (ADR 0246).
        ext_source = os.path.join(work, "extension.cee")
        ext_exe = os.path.join(work, "extension.exe" if os.name == "nt" else "extension")
        with open(ext_source, "w", encoding="utf-8") as f:
            f.write(EXTENSION)
        built = subprocess.run([compiler, "build", ext_source, "-o", ext_exe], capture_output=True, text=True)
        if built.returncode != 0:
            problems.append(f"extension build: {built.stderr.strip()[:800]}")
        else:
            request = os.path.join(work, "push.txt")
            with open(request, "w", encoding="utf-8") as f:
                f.write('Marivil\nSent you a message\n{"kind":"message","from":"u1"}\n')
            dump = os.path.join(work, "dump.txt")
            store = os.path.join(work, "store")
            os.mkdir(store)
            group = "group folder" if sys.platform == "darwin" else "no group folder"
            for label, extra, want in [
                ("a push from a file", {"CEESAFE_PUSH_REQUEST": request},
                 'push Marivil|Sent you a message|{"kind":"message","from":"u1"}\ndelivered\nthen none\n'),
                ("no extension", {}, "unsupported\nthen an error\n"),
            ]:
                cases += 1
                env = dict(os.environ)
                for k in ("CEESAFE_PUSH_REQUEST", "CEESAFE_NOTIFICATIONS_DUMP", "CEESAFE_SECRETS_DIR"):
                    env.pop(k, None)
                env.update(extra)
                env["CEESAFE_NOTIFICATIONS_DUMP"] = dump
                env["CEESAFE_SECRETS_DIR"] = store
                result = subprocess.run([ext_exe], capture_output=True, env=env)
                out = result.stdout.decode("utf-8", "replace").replace("\r\n", "\n")
                want_all = want + f"{group}\ngroup secret\nempty group invalid\n"
                if result.returncode != 0 or out != want_all:
                    problems.append(f"extension, {label}: printed {out!r}, not {want_all!r} "
                                    f"(exit {result.returncode}: {result.stderr.decode('utf-8', 'replace')[:300]})")
            text = open(dump, encoding="utf-8").read() if os.path.exists(dump) else ""
            if text != "deliver Bob\tthe text\n":
                problems.append(f"extension: the delivery wrote {text!r}")
    for p in problems:
        print(f"FAIL {p}")
    print(f"push_check: {cases} cases: {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
