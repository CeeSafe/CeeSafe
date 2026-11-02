#!/usr/bin/env python3
"""Checks ADR 0059: the runtime counts allocations only when something reads the counts.

1. A program that allocates 1000 boxes, run three ways:
   - with no setting: its output, and no "ceesafe alloc:" report;
   - with CEESAFE_ALLOC_REPORT=1: the report, with at least 1000 attempts and live=0;
   - with CEESAFE_FAIL_ALLOC_AT=500: the 500th allocation fails, so the program stops with an error status.
2. The runtime's C, as ceesafec emits it: every use of CEE_COUNT (an atomic read-modify-write) is guarded by
   `alloc_counting`, on its own line or the line before. So a release run, which sets none of the settings, makes no
   atomic operation per allocation.

usage: python3 tools/alloc_counting_check.py --compiler out/ceesafec
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

PROGRAM = """
fn main() {
    var boxes = Vector<Box<u64>>.withCapacity(1000)!;
    for (i in 0..1000u64) {
        boxes.push(Box.create(i)!)!;
    }
    var total: u64 = 0;
    for (b in boxes) {
        total += b;
    }
    Console.writeLine(total);
}
"""

REPORT = re.compile(r"ceesafe alloc: attempts=(\d+) live=(\d+)")


def run(exe, extra):
    env = {k: v for k, v in os.environ.items() if not k.startswith("CEESAFE_")}
    env.update(extra)
    return subprocess.run([exe], capture_output=True, text=True, env=env, timeout=60)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    problems = []
    with tempfile.TemporaryDirectory(prefix="ceesafe-alloc-counting-") as work:
        source = os.path.join(work, "boxes.cee")
        with open(source, "w") as f:
            f.write(PROGRAM)
        exe = os.path.join(work, "boxes")
        b = subprocess.run([compiler, "build", source, "-o", exe], capture_output=True, text=True)
        if b.returncode != 0:
            problems.append(f"build failed: {b.stderr.strip()[-400:]}")
        else:
            if os.name == "nt":
                exe += ".exe"
            plain = run(exe, {})
            if plain.returncode != 0 or plain.stdout.strip() != "499500" or "ceesafe alloc:" in plain.stderr:
                problems.append(f"no setting: status {plain.returncode}, {plain.stdout.strip()!r}, {plain.stderr[-200:]!r}")
            reported = run(exe, {"CEESAFE_ALLOC_REPORT": "1"})
            m = REPORT.search(reported.stderr)
            if reported.returncode != 0 or m is None or int(m.group(1)) < 1000 or int(m.group(2)) != 0:
                problems.append(f"report: status {reported.returncode}, {reported.stderr[-200:]!r}")
            failed = run(exe, {"CEESAFE_FAIL_ALLOC_AT": "500"})
            if failed.returncode == 0:
                problems.append("CEESAFE_FAIL_ALLOC_AT=500: the program did not fail")
        emitted = os.path.join(work, "c")
        e = subprocess.run([compiler, "build", "--emit", "c", source, "-o", emitted], capture_output=True, text=True)
        core = os.path.join(emitted, "ceesafe_rt_core.c")
        if e.returncode != 0 or not os.path.exists(core):
            problems.append(f"--emit c: status {e.returncode}, no ceesafe_rt_core.c")
        else:
            lines = open(core).read().splitlines()
            uses = 0
            for k, line in enumerate(lines):
                if "CEE_COUNT(" in line and not line.lstrip().startswith("#define"):
                    uses += 1
                    if "alloc_counting" not in line and (k == 0 or "alloc_counting" not in lines[k - 1]):
                        problems.append(f"ceesafe_rt_core.c:{k + 1}: CEE_COUNT without alloc_counting: {line.strip()}")
            if uses == 0:
                problems.append("ceesafe_rt_core.c: no use of CEE_COUNT found (the check is out of date)")
    print(f"alloc_counting_check: no setting, the report, a failed allocation, the runtime's guards; "
          f"{len(problems)} problems")
    for problem in problems:
        print("  FAIL " + problem)
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
