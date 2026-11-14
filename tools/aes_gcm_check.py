#!/usr/bin/env python3
"""Checks of lib/std's Aes256Gcm (ADR 0135) that the corpus cannot make: the corpus runs each program in the
interpreter too, where lib/std's bit-sliced AES takes seconds for a kilobyte, so its program is short.

Here the same program (tools/oracles/gen_aes_gcm_cases.py's `program`) is built with the long list: the four AES-256
test cases of the GCM specification, and associated data of 0 to 257 bytes with texts of every length around one,
four and eight blocks, then of 4 KiB and 64 KiB and a byte either side. Each case is sealed, opened and tampered
with, and what it prints is compared with the oracle's lines (tools/oracles/aes_gcm_model.py). It is built and run
compiled only:

1. for this machine: the CPU's instructions where it has them, else lib/std's own code;
2. on macOS on Apple silicon, for x86-64 too (`--cflag -arch --cflag x86_64`), run by Rosetta: AES-NI and PCLMULQDQ.

usage: python3 tools/aes_gcm_check.py --compiler out/ceesafec
"""

import argparse
import os
import platform
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "oracles"))
import gen_aes_gcm_cases  # noqa: E402
from aes_gcm_model import self_check  # noqa: E402


def run(compiler, source, work, name, flags):
    """Builds `source` with `flags` and returns what the program prints, or None with the reason printed."""
    exe = os.path.join(work, name)
    built = subprocess.run([compiler, "build", source, "-o", exe] + flags, capture_output=True, text=True)
    if built.returncode != 0:
        print(f"aes_gcm_check: {name}: the build failed:\n{built.stdout}{built.stderr}")
        return None
    ran = subprocess.run([exe], capture_output=True, text=True, timeout=300)
    if ran.returncode != 0:
        print(f"aes_gcm_check: {name}: exit status {ran.returncode}:\n{ran.stderr}")
        return None
    return ran.stdout.splitlines()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    model = self_check()
    source_text, lines = gen_aes_gcm_cases.program(gen_aes_gcm_cases.LONG_CASES, gen_aes_gcm_cases.LONG_ROUNDS)
    targets = [("native", [])]
    if sys.platform == "darwin" and platform.machine() == "arm64":
        targets.append(("x86-64", ["--cflag", "-arch", "--cflag", "x86_64"]))
    problems = 0
    with tempfile.TemporaryDirectory(prefix="ceesafe-aes-gcm-") as work:
        source = os.path.join(work, "aes_gcm.cee")
        with open(source, "w", encoding="utf-8", newline="\n") as f:
            f.write(source_text)
        for name, flags in targets:
            printed = run(compiler, source, work, name, flags)
            if printed is None:
                problems += 1
            elif printed != lines:
                problems += 1
                wrong = [k for k in range(max(len(printed), len(lines)))
                         if k >= len(printed) or k >= len(lines) or printed[k] != lines[k]]
                k = wrong[0]
                print(f"aes_gcm_check: {name}: {len(wrong)} of {len(lines)} lines differ; the first, line {k + 1}:\n"
                      f"  expected: {lines[k] if k < len(lines) else '(nothing)'}\n"
                      f"  printed:  {printed[k] if k < len(printed) else '(nothing)'}")
    rounds = len(gen_aes_gcm_cases.LONG_ROUNDS)
    print(f"aes_gcm_check: the model: {model}; {len(gen_aes_gcm_cases.LONG_CASES)} specification cases and {rounds} "
          f"rounds (sealed, opened, tampered with), {len(lines)} lines, compiled for {' and '.join(n for n, _ in targets)}"
          f": {problems} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
