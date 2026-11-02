#!/usr/bin/env python3
"""Checks of lib/std's Clock (ADR 0055) that the corpus cannot make, since the corpus runs each program natively and in
the interpreter and the two answer different times:

1. natively, `Clock.now()` is within two seconds of Python's `time.time()` around it, with nanoseconds below 10^9,
   and `Clock.unixMillis()` agrees with it;
2. in the interpreter, outside `Async.run`, it is 2000-01-01T00:00:00Z exactly (946684800 seconds, 0 nanoseconds), and
   inside, the epoch plus the virtual clock: after a sleep of 1500 ms, 946684801 seconds and 500 000 000 nanoseconds.

usage: python3 tools/clock_check.py --compiler out/ceesafec
"""

import argparse
import os
import subprocess
import sys
import tempfile
import time

NOW = """
fn main() {
    let t = Clock.now();
    Console.write(t.seconds);
    Console.write(" ");
    Console.write(t.nanoseconds);
    Console.write(" ");
    Console.writeLine(Clock.unixMillis());
}
"""

VIRTUAL = """
async fn later() -> Result<UnixTime, AsyncError> {
    await Timer.sleep(1500)?;
    return Ok(Clock.now());
}

fn main() {
    let t = Clock.now();
    Console.write(t.seconds);
    Console.write(" ");
    Console.writeLine(t.nanoseconds);
    match (Async.run(later())) {
        Ok(u) => {
            Console.write(u.seconds);
            Console.write(" ");
            Console.writeLine(u.nanoseconds);
        }
        Err(_) => { Console.writeLine("failed"); }
    }
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    problems = []
    with tempfile.TemporaryDirectory(prefix="ceesafe-clock-") as work:
        now = os.path.join(work, "now.cee")
        open(now, "w").write(NOW)
        exe = os.path.join(work, "now")
        b = subprocess.run([compiler, "build", now, "-o", exe], capture_output=True, text=True)
        if b.returncode != 0:
            problems.append(f"build failed: {b.stderr.strip()[-400:]}")
        else:
            if os.name == "nt" and not exe.endswith(".exe"):
                exe += ".exe"
            before = time.time()
            p = subprocess.run([exe], capture_output=True, text=True, timeout=60)
            after = time.time()
            fields = p.stdout.split()
            if p.returncode != 0 or len(fields) != 3:
                problems.append(f"native: status {p.returncode}, printed {p.stdout!r}")
            else:
                seconds, nanoseconds, millis = int(fields[0]), int(fields[1]), int(fields[2])
                value = seconds + nanoseconds / 1e9
                if not (before - 2 <= value <= after + 2):
                    problems.append(f"native: {value} is not between {before} and {after} (two seconds' margin)")
                if not 0 <= nanoseconds < 1_000_000_000:
                    problems.append(f"native: nanoseconds {nanoseconds}")
                if not (seconds * 1000 <= millis <= after * 1000 + 2000):
                    problems.append(f"native: unixMillis {millis} against {seconds} seconds")
        virtual = os.path.join(work, "virtual.cee")
        open(virtual, "w").write(VIRTUAL)
        p = subprocess.run([compiler, "run", "--interp", virtual], capture_output=True, text=True, timeout=120)
        want = ["946684800 0", "946684801 500000000"]
        if p.stdout.strip().splitlines() != want:
            problems.append(f"interpreter: printed {p.stdout.strip().splitlines()!r}, want {want!r}")
    print(f"clock_check: the native clock against Python's, the interpreter's epoch; {len(problems)} problems")
    for problem in problems:
        print("  FAIL " + problem)
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
