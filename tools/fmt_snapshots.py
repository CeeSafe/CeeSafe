#!/usr/bin/env python3
"""Formatter snapshots (formatting.md): each tests/formatting/NAME.cee, formatted with `ceesafec fmt --stdout`, must
equal the reviewed NAME.formatted.cee, and that snapshot must format to itself (a fixed point).

usage: python3 tools/fmt_snapshots.py --compiler out/ceesafec
"""

import argparse
import glob
import os
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))


def fmt(compiler, path):
    p = subprocess.run([compiler, "fmt", "--stdout", path], capture_output=True)
    return p.returncode, p.stdout, p.stderr.decode("utf-8", "replace")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    problems = []
    inputs = sorted(p for p in glob.glob(os.path.join(ROOT, "tests", "formatting", "*.cee"))
                    if not p.endswith(".formatted.cee"))
    for source in inputs:
        name = os.path.relpath(source, ROOT)
        snapshot = source[:-len(".cee")] + ".formatted.cee"
        with open(snapshot, "rb") as f:
            expected = f.read()
        status, out, err = fmt(compiler, source)
        if status != 0 or out != expected:
            problems.append(f"{name}: differs from its .formatted.cee (status {status}) {err[:300]}")
        status, out, err = fmt(compiler, snapshot)
        if status != 0 or out != expected:
            problems.append(f"{name}: its .formatted.cee is not a fixed point (status {status}) {err[:300]}")
    print(f"fmt_snapshots: {len(inputs)} files; {len(problems)} problems")
    for problem in problems:
        print("  FAIL " + problem)
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
