#!/usr/bin/env python3
"""The freed-block scan (ADR 0197 §4.3, type-system.md §42.4) on tests/packages/scan-freed, copied to a temporary
directory:

1. `test --scan-freed`, natively and with --interp: every test passes. Two show what the scan finds (the system heap's
   old buffers keep CosmoStore's G42 keys; a freed system block keeps its bytes); the others, one for each guarantee
   of ADR 0197 §3, that no freed SecretHeap block holds its secret: growth, drop, the slots given up, strings, boxes,
   slot maps, stable vectors, and lib/std's Aes256Gcm and HmacSha256 in a SecretHeap box.
2. `test` without the scan: those two fail, since `Testing.freedHolds` is false while the scan is off, and the
   others pass.
3. `build`: the program prints `true` and `false` (the system heap's keys found, SecretHeap's not) with
   CEESAFE_SCAN_FREED=1, and `false` twice without it.

usage: python3 tools/scan_freed_check.py --compiler out/ceesafec
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
FOUND = {"systemHeapGrowthLeavesKeys", "systemHeapFreeKeepsBytes"}


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    problems = []

    def run(argv, cwd, env=None):
        environment = dict(os.environ)
        environment.update(env or {})
        return subprocess.run([compiler] + argv, cwd=cwd, capture_output=True, text=True, timeout=900,
                              env=environment)

    with tempfile.TemporaryDirectory(prefix="ceesafe-scan-freed-") as work:
        package = os.path.join(work, "scan-freed")
        shutil.copytree(os.path.join(ROOT, "tests", "packages", "scan-freed"), package,
                        ignore=shutil.ignore_patterns("out"))
        counted = 0
        for label, flags, scanning in (("native", ["--scan-freed"], True), ("interp", ["--scan-freed", "--interp"], True),
                                       ("without the scan", [], False)):
            p = run(["test", "--format", "json", "--jobs", "4"] + flags, package)
            try:
                tests = json.loads(p.stdout)["tests"]
            except (ValueError, KeyError):
                problems.append(f"{label}: no JSON report: {p.stdout[:300]}{p.stderr[-600:]}")
                continue
            if len(tests) < 8:
                problems.append(f"{label}: {len(tests)} tests, expected 8")
            for t in tests:
                counted += 1
                want = "passed" if scanning or t["name"] not in FOUND else "failed"
                if t["outcome"] != want:
                    problems.append(f"{label}: {t['name']}: {t['outcome']}, expected {want}: {t.get('message', '')}")
        exe = os.path.join(work, "scan")
        p = run(["build", "-o", exe], package)
        if p.returncode != 0:
            problems.append(f"build failed: {p.stderr[-600:]}")
        else:
            for env, want in (({"CEESAFE_SCAN_FREED": "1"}, ["true", "false"]), ({}, ["false", "false"])):
                environment = dict(os.environ)
                environment.pop("CEESAFE_SCAN_FREED", None)
                environment.update(env)
                ran = subprocess.run([exe], capture_output=True, text=True, timeout=60, env=environment)
                if ran.returncode != 0 or ran.stdout.split() != want:
                    problems.append(f"the program with {env or 'no scan'}: status {ran.returncode}, printed "
                                    f"{ran.stdout.split()}, expected {want}")
    for problem in problems:
        print(f"scan_freed_check: {problem}")
    print(f"scan_freed_check: {counted} test outcomes and 2 runs of the built program: {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
