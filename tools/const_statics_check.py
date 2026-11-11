#!/usr/bin/env python3
"""A plain static of scalar data is C's `const` object (ADR 0098): the C that run-pass/static-scalar-data.cee emits.

The corpus runs the program; this reads its C. Each plain static must be defined `static ... const`, the `static mut`
beside it must not be, and the C must compile with -Wcast-qual as an error, since a `const` object's address reaches
CeeSafe's pointer types, which carry no `const`.

usage: python3 tools/const_statics_check.py --ceesafec out/ceesafec
"""
import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parent / "tests" / "reference" / "tests" / "corpus" / "run-pass" / "static-scalar-data.cee"
CONST = ["LIMIT", "ARMED", "ALLOWED", "GRID"]
MUTABLE = ["HITS"]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ceesafec", required=True)
    args = ap.parse_args()
    work = Path(tempfile.mkdtemp(prefix="const-statics-"))
    failures = []
    try:
        run = subprocess.run([args.ceesafec, "build", "--emit", "c", "-o", str(work / "c"), str(SOURCE)],
                             capture_output=True, text=True)
        if run.returncode != 0:
            print(run.stderr, file=sys.stderr)
            print("const statics: FAIL (the source did not build)")
            return 1
        text = (work / "c" / "main.c").read_text()
        for name, const in [(n, True) for n in CONST] + [(n, False) for n in MUTABLE]:
            lines = [line for line in text.splitlines() if re.match(rf"static\b.*\bceeS_{name} = ", line)]
            if len(lines) != 1:
                failures.append(f"{name}: {len(lines)} definitions")
            elif (re.search(r"\bconst\b", lines[0].split("=")[0]) is not None) != const:
                failures.append(f"{name}: {'not ' if const else ''}const: {lines[0][:100]}")
        run = subprocess.run([args.ceesafec, "build", "--cflag", "-Werror=cast-qual", "-o", str(work / "program"),
                              str(SOURCE)], capture_output=True, text=True)
        if run.returncode != 0:
            failures.append("the C does not compile with -Werror=cast-qual: " + run.stderr.strip()[:400])
        elif subprocess.run([str(work / "program")], capture_output=True).returncode != 0:
            failures.append("the program failed")
    finally:
        shutil.rmtree(work, ignore_errors=True)
    for failure in failures:
        print(f"  {failure}")
    print(f"const statics: {'FAIL' if failures else 'PASS'} ({len(CONST)} const, {len(MUTABLE)} not)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
