#!/usr/bin/env python3
"""Local notifications (ADR 0243): Notifications.requestPermission, show and clear, natively and interpreted.

One program is built and run three ways:

1. natively with CEESAFE_NOTIFICATIONS_DUMP naming a file, on every system: each call is Ok and appends its line
   (a text's tab, newline and backslash escaped; UTF-8 kept whole), and each id, title or body the system does not take
   is InvalidInput, with nothing appended;
2. natively without it: Unsupported on Linux, FreeBSD and Windows, and on macOS too, where the program has no bundle
   (the id and text checks are InvalidInput everywhere);
3. under `ceesafec run --interp`: Unsupported for every call the system is asked, the variable set or not.

usage: python3 tools/notify_check.py --compiler out/ceesafec
"""

import argparse
import os
import subprocess
import sys
import tempfile

PROGRAM = r'''fn kind(r: Result<void, NotificationError>) -> StringView => match (r) {
    Ok() => "ok",
    Err(NotificationError.Unsupported) => "unsupported",
    Err(NotificationError.InvalidInput) => "invalid",
    Err(NotificationError.Denied) => "denied",
    Err(NotificationError.Failed) => "failed",
};

fn say(label: StringView, r: Result<void, NotificationError>) {
    Console.write(label);
    Console.write(" ");
    Console.writeLine(kind(r));
}

fn main() -> i32 {
    say("permission", Notifications.requestPermission());
    say("show", Notifications.show("chat:42", "Bob", "Are you there?"));
    say("replace", Notifications.show("chat:42", "Bob", "Two messages"));
    say("escapes", Notifications.show("chat.43-x_y", "A\tB", "line one\nline two \\ back"));
    say("utf8", Notifications.show("u", "Zoë", "مرحبا 👋"));
    say("empty texts", Notifications.show("e", "", ""));
    say("clear", Notifications.clear("chat:42"));
    say("clear absent", Notifications.clear("nothing"));
    var longId = String.create();
    for (i in 0..65) {
        let _ = longId.append("i");
    }
    var longTitle = String.create();
    for (i in 0..257) {
        let _ = longTitle.append("t");
    }
    var longBody = String.create();
    for (i in 0..4097) {
        let _ = longBody.append("b");
    }
    say("id empty", Notifications.show("", "t", "b"));
    say("id long", Notifications.show(longId.view(), "t", "b"));
    say("id 64", Notifications.show(longId.view()[..64], "t", "b"));
    say("id space", Notifications.show("a b", "t", "b"));
    say("id slash", Notifications.clear("a/b"));
    say("title long", Notifications.show("t", longTitle.view(), "b"));
    say("title 256", Notifications.show("t", longTitle.view()[..256], "b"));
    say("body long", Notifications.show("b", "t", longBody.view()));
    say("body 4096", Notifications.show("b", "t", longBody.view()[..4096]));
    say("zero", Notifications.show("z", "a\0b", "c"));
    return 0;
}
'''

# (label, what a working system prints, whether the system is asked)
LINES = [
    ("permission", "ok", True),
    ("show", "ok", True),
    ("replace", "ok", True),
    ("escapes", "ok", True),
    ("utf8", "ok", True),
    ("empty texts", "ok", True),
    ("clear", "ok", True),
    ("clear absent", "ok", True),
    ("id empty", "invalid", False),
    ("id long", "invalid", False),
    ("id 64", "ok", True),
    ("id space", "invalid", False),
    ("id slash", "invalid", False),
    ("title long", "invalid", False),
    ("title 256", "ok", True),
    ("body long", "invalid", False),
    ("body 4096", "ok", True),
    ("zero", "invalid", False),
]

DUMP = ("permission\n"
        "show chat:42\tBob\tAre you there?\n"
        "show chat:42\tBob\tTwo messages\n"
        "show chat.43-x_y\tA\\tB\tline one\\nline two \\\\ back\n"
        "show u\tZoë\tمرحبا \U0001f44b\n"
        "show e\t\t\n"
        "clear chat:42\n"
        "clear nothing\n"
        f"show {'i' * 64}\tt\tb\n"
        f"show t\t{'t' * 256}\tb\n"
        f"show b\tt\t{'b' * 4096}\n")


def expected(working):
    return "".join(f"{label} {answer if working or not asked else 'unsupported'}\n" for label, answer, asked in LINES)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    problems = []
    cases = 0
    with tempfile.TemporaryDirectory() as work:
        source = os.path.join(work, "main.cee")
        exe = os.path.join(work, "notify.exe" if os.name == "nt" else "notify")
        with open(source, "w", encoding="utf-8") as f:
            f.write(PROGRAM)
        built = subprocess.run([compiler, "build", source, "-o", exe], capture_output=True, text=True)
        if built.returncode != 0:
            print(f"FAIL build: {built.stderr.strip()[:800]}")
            print("notify_check: 0 cases: 1 problems")
            return 1

        def run(dump, working, interp=False, label=""):
            nonlocal cases
            cases += 1
            env = dict(os.environ)
            env.pop("CEESAFE_NOTIFICATIONS_DUMP", None)
            if dump is not None:
                env["CEESAFE_NOTIFICATIONS_DUMP"] = dump
            command = [compiler, "run", "--interp", source] if interp else [exe]
            result = subprocess.run(command, capture_output=True, env=env)
            out = result.stdout.decode("utf-8", "replace").replace("\r\n", "\n")
            want = expected(working)
            if result.returncode != 0:
                problems.append(f"{label}: exit {result.returncode}: {result.stderr.decode('utf-8', 'replace')[:400]}")
            elif out != want:
                diff = [f"  {a!r} not {b!r}" for a, b in zip(out.splitlines(), want.splitlines()) if a != b]
                problems.append(f"{label}: printed other lines:\n" + "\n".join(diff[:10]))

        dump = os.path.join(work, "dump.txt")
        run(dump, True, label="CEESAFE_NOTIFICATIONS_DUMP")
        text = open(dump, encoding="utf-8").read() if os.path.exists(dump) else ""
        if text != DUMP:
            got, want = text.splitlines(), DUMP.splitlines()
            diff = [f"  {a[:80]!r} not {b[:80]!r}" for a, b in zip(got, want) if a != b]
            problems.append("CEESAFE_NOTIFICATIONS_DUMP: the file holds other lines:\n" + "\n".join(diff[:10]) +
                            (f"\n  ({len(got)} lines, not {len(want)})" if len(got) != len(want) else ""))
        run(None, False, label="the system's notifications (no bundle, or no system)")
        os.remove(dump)
        run(dump, False, interp=True, label="interpreter")
        if os.path.exists(dump):
            problems.append("interpreter: wrote the dump file")
    for p in problems:
        print(f"FAIL {p}")
    print(f"notify_check: {cases} cases: {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
