#!/usr/bin/env python3
"""Cancellation sweep (spec/testing.md §1; ADR 0031 §3.4): cancellation must destroy nothing and leak nothing.

Every run-pass program of the corpus that calls `Async.run` is built with clang (GCC without clang) under ASan+UBSan and run once with
CEESAFE_CANCEL_REPORT=1 to count its suspensions n (the runtime prints `ceesafe cancel: points=N` after each
Async.run; the last line counts the whole program). Then once per suspension k <= n (sampled past 200, like
fault_injection.py) with CEESAFE_CANCEL_AT=k, which cancels the running root task at the program's k-th suspension.
Each run must exit 0 with no sanitizer report and no live allocation (CEESAFE_ALLOC_REPORT: live=0). Its output may
differ from the expected one: a cancelled task returns early by ordinary code.

With --package DIR (repeatable), each directory is an executable package instead: it is built there with its manifest
and swept the same way, from that directory.

usage: python3 tools/cancel_sweep.py --compiler out/ceesafec [--corpus tests/reference/tests/corpus] [--filter TEXT]
       python3 tools/cancel_sweep.py --compiler out/ceesafec --package DIR [--package DIR]...
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fault_injection import SANITIZER_CC  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
POINTS = re.compile(r"ceesafe cancel: points=(\d+)")
ALLOC = re.compile(r"ceesafe alloc: attempts=(\d+) live=(\d+)")
SANITIZERS = ["-O1", "-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
MAX_POINTS = 200


def sample(n):
    if n <= MAX_POINTS:
        return list(range(1, n + 1))
    picked = set(range(1, 51)) | {n - k for k in range(50)} | {k * n // 101 for k in range(1, 101)}
    return sorted(p for p in picked if 1 <= p <= n)


def run(exe, cwd, extra):
    env = dict(os.environ)
    env.update({"CEESAFE_ALLOC_REPORT": "1"})
    env.update(extra)
    try:
        p = subprocess.run([exe], cwd=cwd, capture_output=True, text=True, env=env, timeout=60)
    except subprocess.TimeoutExpired:
        return "timeout", "", ""
    return p.returncode, p.stdout, p.stderr


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--corpus", default=os.path.join(ROOT, "tests", "reference", "tests", "corpus"))
    parser.add_argument("--filter")
    parser.add_argument("--package", action="append", default=[], help="an executable package's directory")
    args = parser.parse_args()
    compiler = os.path.abspath(os.path.expanduser(args.compiler))
    # (name, the directory it is built and run in, the build's inputs: none for a package, which has a manifest)
    if args.package:
        programs = [(os.path.basename(os.path.normpath(p)), os.path.abspath(os.path.expanduser(p)), [])
                    for p in args.package]
    else:
        directory = os.path.join(args.corpus, "run-pass")
        programs = [(name, args.corpus, ["run-pass/" + name]) for name in sorted(os.listdir(directory))
                    if name.endswith(".cee") and
                    "Async.run" in open(os.path.join(directory, name), encoding="utf-8").read() and
                    (not args.filter or args.filter in name)]
    failures, total = [], 0
    with tempfile.TemporaryDirectory(prefix="ceesafe-cancel-") as work:
        for name, cwd, inputs in programs:
            exe = os.path.join(work, name.removesuffix(".cee"))
            flags = [x for f in SANITIZERS for x in ("--cflag", f)]
            build = subprocess.run([compiler, "build", "--cc", SANITIZER_CC] + flags + inputs + ["-o", exe],
                                   cwd=cwd, capture_output=True, text=True)
            if build.returncode != 0:
                failures.append(f"{name}: build failed: {build.stderr.strip()[:300]}")
                continue
            status, out, err = run(exe, cwd, {"CEESAFE_CANCEL_REPORT": "1"})
            counts = POINTS.findall(err)
            if status != 0 or not counts:
                failures.append(f"{name}: the uncancelled run failed (status {status}): {err.strip()[:300]}")
                continue
            points = int(counts[-1])
            for k in sample(points):
                total += 1
                status, out, err = run(exe, cwd, {"CEESAFE_CANCEL_AT": str(k)})
                m = ALLOC.search(err)
                if status != 0 or not m or m.group(2) != "0":
                    failures.append(f"{name}: cancelled at {k}: status {status}, stderr {err.strip()[:300]!r}")
    print(f"cancel_sweep: {len(programs)} async {'packages' if args.package else 'programs'}, {total} cancellation points; {len(failures)} fail")
    for f in failures[:20]:
        print("  FAIL " + f)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
