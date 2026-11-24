#!/usr/bin/env python3
"""A binding of the system's zlib, written afresh by `ceesafec cbind`, works (the optimization and DX roadmap's Phase 4,
item 1, its local half): in a copy of tests/cbind/zlib, `cbind --header zlib.h --only …` writes src/zlib.cee from the
header the system has, then `ceesafec build` compiles the program over it and links `-lz`, and it runs. The C compiler
checks each declaration against the header (type-system.md §31), so a binding that drifted from the installed zlib
fails to build. The oracle is Python's zlib: the program prints that its round trip gave the bytes back, that the
compressed form is smaller, and the CRC-32 and Adler-32 of its data, which Python computes from the same bytes.

Exit status 2, with the reason, when the host has no clang (cbind reads clang's AST) or no zlib.h: not a pass.

usage: python3 tools/cbind_zlib_check.py --ceesafec out/ceesafec
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIXTURE = os.path.join(ROOT, "tests", "cbind", "zlib")
NAMES = "compress,uncompress,compressBound,crc32,adler32,zlibVersion,Z_OK,Z_BUF_ERROR"


def include_directory():
    """Where zlib.h is: the macOS SDK's include directory, else the usual Unix ones."""
    candidates = []
    if sys.platform == "darwin":
        sdk = subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True, text=True).stdout.strip()
        if sdk:
            candidates.append(os.path.join(sdk, "usr", "include"))
    candidates += ["/usr/include", "/usr/local/include", "/opt/homebrew/include"]
    return next((d for d in candidates if os.path.isfile(os.path.join(d, "zlib.h"))), None)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ceesafec", required=True)
    args = parser.parse_args()
    compiler = os.path.abspath(args.ceesafec)
    include = include_directory()
    if include is None:
        print("cbind_zlib_check: skipped: no zlib.h on this host")
        sys.exit(2)
    if shutil.which("clang") is None:
        print("cbind_zlib_check: skipped: no clang on this host (cbind reads clang's AST)")
        sys.exit(2)
    data = bytes(k * 7 % 251 for k in range(100000))  # what src/main.cee builds
    expected = ["true", "true", str(zlib.crc32(data)), str(zlib.adler32(data))]
    problems = []
    work = tempfile.mkdtemp(prefix="cbind-zlib-")
    try:
        package = os.path.join(work, "zlib")
        shutil.copytree(FIXTURE, package)
        made = subprocess.run([compiler, "cbind", "--header", "zlib.h", "-I", include, "--cc", "clang", "--only", NAMES,
                               "-o", os.path.join(package, "src", "zlib.cee")], capture_output=True, text=True)
        if made.returncode != 0:
            problems.append(f"cbind failed ({made.returncode}): {made.stderr[-600:]}")
        else:
            binding = open(os.path.join(package, "src", "zlib.cee")).read()
            for name in NAMES.split(","):
                if name not in binding:
                    problems.append(f"the binding lacks {name}")
            built = subprocess.run([compiler, "build"], cwd=package, capture_output=True, text=True)
            if built.returncode != 0:
                problems.append(f"the program over the binding does not build: {built.stderr[-1200:]}")
            else:
                ran = subprocess.run([os.path.join(package, "out", "zlibdemo")], capture_output=True, text=True,
                                     timeout=60)
                lines = ran.stdout.split()
                if ran.returncode != 0 or lines != expected:
                    problems.append(f"the program printed {lines} (exit {ran.returncode}); Python's zlib: {expected}")
    finally:
        shutil.rmtree(work, ignore_errors=True)
    for p in problems:
        print("FAIL", p)
    print(f"cbind_zlib_check: zlib.h from {include}, crc32 {expected[2]}, adler32 {expected[3]}: "
          f"{len(problems)} problems")
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
