#!/usr/bin/env python3
"""Push tokens (ADR 0244): Push.register, natively and interpreted.

One program is built and run:

1. natively with CEESAFE_PUSH_TOKEN set to each form the runtime gives (apns:HEX, apns-sandbox:HEX, fcm:TOKEN): the
   service, the token and sandbox are read back; a malformed one (no prefix, an empty token) is Failed;
2. natively without it: Unsupported (a command-line program has no window, and only an iOS app registers);
3. under `ceesafec run --interp`: Unsupported, the variable set or not.

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
    for p in problems:
        print(f"FAIL {p}")
    print(f"push_check: {cases} cases: {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
