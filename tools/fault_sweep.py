#!/usr/bin/env python3
"""Allocation-failure sweep (ROADMAP P1; tools/fault_injection.py uses the same runtime hooks).

Builds a program with the given compiler, runs it once with CEESAFE_ALLOC_REPORT=1 to count allocation attempts, then
once per attempt k with CEESAFE_FAIL_ALLOC_AT=k. Each run must exit 0, print a line starting with "consistent" (the
program's own check that the failure came back as an error and left its structure intact), and end with no live
allocations (the runtime's report: live=0). A panic, a crash, an inconsistency or a leak is a failure.

usage: tools/fault_sweep.py --compiler PATH [--cflag=FLAG]... SOURCE...
       tools/fault_sweep.py --compiler PATH [--cflag=FLAG]... --package DIR   (an executable package, built in DIR)
example: tools/fault_sweep.py --compiler out/ceesafec tests/support/fault/alloc-sweep.cee src/support/*.cee
"""

import argparse
import collections
import os
import re
import subprocess
import sys
import tempfile

REPORT = re.compile(r"ceesafe alloc: attempts=(\d+) live=(\d+)")


def run(exe, env_extra, cwd=None):
    env = dict(os.environ)
    env.update(env_extra)
    p = subprocess.run([exe], cwd=cwd, capture_output=True, text=True, env=env, timeout=60)
    return p.returncode, p.stdout, p.stderr


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--cflag", action="append", default=[])
    parser.add_argument("--package", help="an executable package's directory, built with its manifest")
    parser.add_argument("sources", nargs="*")
    args = parser.parse_args()
    if bool(args.package) == bool(args.sources):
        parser.error("give either SOURCE files or --package DIR")
    compiler = os.path.abspath(os.path.expanduser(args.compiler))
    cwd = os.path.abspath(os.path.expanduser(args.package)) if args.package else None

    with tempfile.TemporaryDirectory(prefix="ceesafe-sweep-") as work:
        exe = os.path.join(work, "program")
        flags = [x for f in args.cflag for x in ("--cflag", f)]
        build = subprocess.run([compiler, "build"] + flags + args.sources + ["-o", exe],
                               cwd=cwd, capture_output=True, text=True)
        if build.returncode != 0:
            sys.exit(f"fault_sweep: build failed:\n{build.stderr}")
        status, out, err = run(exe, {"CEESAFE_ALLOC_REPORT": "1"}, cwd)
        m = REPORT.search(err)
        if status != 0 or not m:
            sys.exit(f"fault_sweep: the unfailed run failed (status {status}):\n{out}{err}")
        attempts, live = int(m.group(1)), int(m.group(2))
        print(f"fault_sweep: unfailed run: {out.strip()}; {attempts} allocation attempts, live={live}")
        failures, stops = [], collections.Counter()
        for k in range(1, attempts + 1):
            status, out, err = run(exe, {"CEESAFE_ALLOC_REPORT": "1", "CEESAFE_FAIL_ALLOC_AT": str(k)}, cwd)
            m = REPORT.search(err)
            line = out.strip()
            if status != 0 or not line.startswith("consistent") or not m or m.group(2) != "0":
                failures.append(f"attempt {k}: status {status}, stdout {line!r}, stderr {err.strip()[:300]!r}")
            else:
                stops[line] += 1
        for line, n in sorted(stops.items()):
            print(f"  {n:5} x {line}")
        if live != 0:
            failures.append(f"the unfailed run leaks: live={live}")
        print(f"fault_sweep: {attempts - len(failures)} of {attempts} failure points handled")
        for f in failures[:20]:
            print("  FAIL " + f)
        sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
