#!/usr/bin/env python3
"""The system's secret store (ADR 0242): Secrets.read, write and remove, natively and interpreted.

One program is built and run three ways:

1. natively with CEESAFE_SECRETS_DIR naming an empty folder, on every system: a value that is not there is `null` and
   removing it is `Ok`; values are written, read back whole and replaced (text, empty, every byte value, the largest,
   65,536 bytes), one over the limit and each name the store does not take are `InvalidInput`; two apps' values of one
   name are apart; a removed value is gone; the folder then holds only the apps' (now empty) folders;
2. natively without it, against the system's store: on macOS (the keychain) and Windows (DPAPI) the same as 1, under
   an app name of this run's own, removed after; elsewhere every call the system is asked is `Unsupported`;
3. under `ceesafec run --interp`: every call the system is asked is `Unsupported`, the variable set or not.

usage: python3 tools/secrets_check.py --compiler out/ceesafec
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

/// A value's length and a hash of its bytes, or what the read answered.
fn read(label: StringView, app: StringView, name: StringView) {
    Console.write(label);
    Console.write(" ");
    match (Secrets.read(app, name)) {
        Ok(Some(v)) => {
            var h: u64 = 7;
            for (b in v.view()) {
                h = (h * 31 + b as u64) % 1000000007;
            }
            Console.write(v.length);
            Console.write(" ");
            Console.writeLine(h);
        }
        Ok(null) => { Console.writeLine("none"); }
        Err(e) => { Console.writeLine(kind(e)); }
    }
}

fn done(label: StringView, r: Result<void, SecretError>) {
    Console.write(label);
    Console.write(" ");
    match (r) {
        Ok() => { Console.writeLine("ok"); }
        Err(e) => { Console.writeLine(kind(e)); }
    }
}

fn main() -> i32 {
    let app = Environment.variable("SECRETS_CHECK_APP") ?? "ceesafe-secrets-check";
    var other = String.create();
    let _ = other.append(app);
    let _ = other.append("-other");
    read("missing", app, "token");
    done("remove missing", Secrets.remove(app, "token"));
    done("write text", Secrets.write(app, "token", "hello, secret".bytes()));
    read("text", app, "token");
    done("replace", Secrets.write(app, "token", "replaced".bytes()));
    read("replaced", app, "token");
    done("write empty", Secrets.write(app, "empty", ""[..].bytes()));
    read("empty", app, "empty");
    var every = Vector<u8>.create();
    for (i in 0..256) {
        let _ = every.push(truncate<u8>(i));
    }
    done("write bytes", Secrets.write(app, "bytes", every.view()));
    read("bytes", app, "bytes");
    var large = Vector<u8>.create();
    for (i in 0..65536) {
        let _ = large.push(truncate<u8>(i * 7 + i / 256));
    }
    done("write largest", Secrets.write(app, "large_1.x-y", large.view()));
    read("largest", app, "large_1.x-y");
    let _ = large.push(1);
    done("write too large", Secrets.write(app, "large_1.x-y", large.view()));
    read("largest kept", app, "large_1.x-y");
    done("write other app", Secrets.write(other.view(), "token", "other".bytes()));
    read("other app", other.view(), "token");
    read("this app", app, "token");
    read("name empty", app, "");
    read("name slash", app, "a/b");
    read("name space", app, "a b");
    read("name dot", app, ".");
    read("name dots", app, "..");
    var long = String.create();
    for (i in 0..129) {
        let _ = long.append("n");
    }
    read("name long", app, long.view());
    read("name 128", app, long.view()[..128]);
    read("app empty", "", "token");
    read("app slash", "a/b", "token");
    read("app backslash", "a\\b", "token");
    read("app dots", "..", "token");
    done("remove", Secrets.remove(app, "token"));
    read("removed", app, "token");
    done("remove again", Secrets.remove(app, "token"));
    done("clean empty", Secrets.remove(app, "empty"));
    done("clean bytes", Secrets.remove(app, "bytes"));
    done("clean largest", Secrets.remove(app, "large_1.x-y"));
    done("clean other", Secrets.remove(other.view(), "token"));
    read("cleaned", other.view(), "token");
    return 0;
}
'''


def hash_of(data):
    h = 7
    for b in data:
        h = (h * 31 + b) % 1000000007
    return f"{len(data)} {h}"


LARGE = bytes(((i * 7 + i // 256) & 0xFF) for i in range(65536))

# (label, what a working store prints, whether the system is asked)
LINES = [
    ("missing", "none", True),
    ("remove missing", "ok", True),
    ("write text", "ok", True),
    ("text", hash_of(b"hello, secret"), True),
    ("replace", "ok", True),
    ("replaced", hash_of(b"replaced"), True),
    ("write empty", "ok", True),
    ("empty", hash_of(b""), True),
    ("write bytes", "ok", True),
    ("bytes", hash_of(bytes(range(256))), True),
    ("write largest", "ok", True),
    ("largest", hash_of(LARGE), True),
    ("write too large", "invalid", False),
    ("largest kept", hash_of(LARGE), True),
    ("write other app", "ok", True),
    ("other app", hash_of(b"other"), True),
    ("this app", hash_of(b"replaced"), True),
    ("name empty", "invalid", False),
    ("name slash", "invalid", False),
    ("name space", "invalid", False),
    ("name dot", "invalid", False),
    ("name dots", "invalid", False),
    ("name long", "invalid", False),
    ("name 128", "none", True),
    ("app empty", "invalid", False),
    ("app slash", "invalid", False),
    ("app backslash", "invalid", False),
    ("app dots", "invalid", False),
    ("remove", "ok", True),
    ("removed", "none", True),
    ("remove again", "ok", True),
    ("clean empty", "ok", True),
    ("clean bytes", "ok", True),
    ("clean largest", "ok", True),
    ("clean other", "ok", True),
    ("cleaned", "none", True),
]


def expected(working):
    return "".join(f"{label} {answer if working or not asked else 'unsupported'}\n"
                   for label, answer, asked in LINES)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    problems = []
    cases = 0
    with tempfile.TemporaryDirectory() as work:
        source = os.path.join(work, "main.cee")
        exe = os.path.join(work, "secrets.exe" if os.name == "nt" else "secrets")
        with open(source, "w", encoding="utf-8") as f:
            f.write(PROGRAM)
        built = subprocess.run([compiler, "build", source, "-o", exe], capture_output=True, text=True)
        if built.returncode != 0:
            print(f"FAIL build: {built.stderr.strip()[:800]}")
            print("secrets_check: 0 cases: 1 problems")
            return 1
        app = f"ceesafe-secrets-check-{os.getpid()}"

        def run(folder, working, interp=False, label=""):
            nonlocal cases
            cases += 1
            env = dict(os.environ)
            env.pop("CEESAFE_SECRETS_DIR", None)
            env["SECRETS_CHECK_APP"] = app
            if folder is not None:
                env["CEESAFE_SECRETS_DIR"] = folder
            command = [compiler, "run", "--interp", source] if interp else [exe]
            result = subprocess.run(command, capture_output=True, env=env)
            out = result.stdout.decode("utf-8", "replace").replace("\r\n", "\n")
            want = expected(working)
            if result.returncode != 0:
                problems.append(f"{label}: exit {result.returncode}: {result.stderr.decode('utf-8', 'replace')[:400]}")
            elif out != want:
                diff = [f"  {a!r} not {b!r}" for a, b in zip(out.splitlines(), want.splitlines()) if a != b]
                problems.append(f"{label}: printed other lines:\n" + "\n".join(diff[:10]) +
                                (f"\n  ({len(out.splitlines())} lines, not {len(want.splitlines())})"
                                 if len(out.splitlines()) != len(want.splitlines()) else ""))

        folder = os.path.join(work, "store")
        os.mkdir(folder)
        run(folder, True, label="CEESAFE_SECRETS_DIR")
        left = sorted(os.path.relpath(os.path.join(d, n), folder) for d, _, names in os.walk(folder) for n in names)
        if left:
            problems.append(f"CEESAFE_SECRETS_DIR: files left after their removal: {left}")
        kept = sorted(os.listdir(folder))
        if kept != sorted([app, app + "-other"]):
            problems.append(f"CEESAFE_SECRETS_DIR: the folder holds {kept}, not the two apps' folders")
        run(None, sys.platform in ("darwin", "win32"), label="the system's store")
        run(folder, False, interp=True, label="interpreter")
    for p in problems:
        print(f"FAIL {p}")
    print(f"secrets_check: {cases} cases: {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
