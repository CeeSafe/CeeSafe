#!/usr/bin/env python3
"""Every contract test of ceesafec, against CeeSafe's own expectations only (Decision 0007: CeeSafe has left Nova, and
nothing here runs novac or reads a Nova checkout):
- the fixed point at HEAD, with ceesafec as stage 0 (tools/fixed_point.py);
- CeeSafe's corpus (tests/reference/tests/corpus) in every family: annotations, AST/HIR/C snapshots, expected output,
  the optimisation reports; run-pass and run-fail again in the interpreter; run-pass, run-fail and ffi again under
  -Werror, and under ASan+UBSan with leak checks; lib/'s own diagnostics (Decision 0008);
- the support library's tests with leak checks, and its allocation-failure sweep;
- the mutation suite, the fault sweep over the corpus, the cancellation sweep over its async programs, the collection
  properties, the freestanding checks;
- CeeSafe's specification and guide (tools/spec_check.py, tools/guide_check.py) and the debugger scripts.
Each step's last line is printed.

usage: python3 tools/contract_tests.py --ceesafec PATH
"""

import argparse
import glob
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))

# GCC 12 -O2 -Werror rejects this program's C (-Wuninitialized in an enum constructor; ROADMAP §P5).
KNOWN_GCC_RUN_FAILURES = {"run-pass/nested-patterns.cee"}


def is_gcc():
    """Whether the default C compiler ($CC, else cc) is GCC."""
    cc = os.environ.get("CC") or "cc"
    version = subprocess.run([cc, "--version"], capture_output=True, text=True).stdout
    return "Free Software Foundation" in version or version.startswith("gcc")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--ceesafec", required=True)
    args = parser.parse_args()
    cee = os.path.abspath(os.path.expanduser(args.ceesafec))
    corpus = os.path.join(ROOT, "tests", "reference", "tests", "corpus")
    support = sorted(glob.glob(os.path.join(ROOT, "src", "support", "*.cee")))
    py = [sys.executable, "-B"]
    run_corpus = py + [f"{HERE}/run_corpus.py", "--compiler", cee]
    run_families = ["--family", "run-pass", "--family", "run-fail", "--family", "ffi"]
    failed = 0
    with tempfile.TemporaryDirectory(prefix="ceesafe-contract-") as work:
        # lib/ is CeeSafe's (Decision 0008): an empty program checked with the library's own diagnostics on.
        empty = os.path.join(work, "empty.cee")
        with open(empty, "w") as f:
            f.write("fn main() {}\n")
        steps = [
            ("fixed point", py + [f"{HERE}/fixed_point.py", "--stage0", cee, "--work", os.path.join(work, "fp"),
                                  "--manifest", os.path.join(work, "fixed-point.json")]),
            ("corpus", run_corpus + ["--corpus", corpus, "--check-leaks"]),
            ("run -Werror", run_corpus + ["--corpus", corpus] + run_families + ["--cflag=-Werror"]),
            ("run interp", run_corpus + ["--corpus", corpus, "--family", "run-pass", "--family", "run-fail",
                                         "--interp", "--check-leaks"]),
            ("library", [cee, "check", "--library-diagnostics", empty]),
            ("run asan", run_corpus + ["--corpus", corpus] + run_families +
             ["--cc", "clang", "--cflag=-O1", "--cflag=-Werror", "--cflag=-fsanitize=address,undefined",
              "--cflag=-fno-sanitize-recover=all", "--check-leaks"]),
            ("support", run_corpus + ["--corpus", os.path.join(ROOT, "tests", "support"), "--family", "run-pass",
                                      "--check-leaks"] + [x for f in support for x in ("--with", f)]),
            ("support sweep", py + [f"{HERE}/fault_sweep.py", "--compiler", cee,
                                    os.path.join(ROOT, "tests", "support", "fault", "alloc-sweep.cee")] + support),
            ("mutations", py + [f"{HERE}/mutation_suite.py", "--compiler", cee]),
            ("fault sweep", py + [f"{HERE}/fault_injection.py", "--compiler", cee, "--corpus", corpus]),
            ("cancel sweep", py + [f"{HERE}/cancel_sweep.py", "--compiler", cee, "--corpus", corpus]),
            ("collections", py + [f"{HERE}/collection_properties.py", "--compiler", cee, "--sweep"]),
            ("freestanding", py + [f"{HERE}/freestanding_check.py", "--compiler", cee, "--corpus", corpus]),
            ("spec", py + [f"{HERE}/spec_check.py", "--ceesafec", cee]),
            ("guide", py + [f"{HERE}/guide_check.py", "--ceesafec", cee]),
            ("debugger", py + [f"{HERE}/debugger_check.py", "--ceesafec", cee]),
        ]
        for name, argv in steps:
            p = subprocess.run(argv, cwd=ROOT, capture_output=True, text=True)
            lines = [line for line in (p.stdout + p.stderr).splitlines() if line.strip()]
            summary = " | ".join(lines[-3:]) if name in ("corpus", "run -Werror", "run asan", "support") else (
                lines[-1] if lines else "(no output)")
            ok = p.returncode == 0
            failures = {line.split()[1] for line in lines if line.startswith("FAIL ")}
            if name == "run -Werror" and not ok and failures and failures <= KNOWN_GCC_RUN_FAILURES and is_gcc():
                ok = True
                summary += f" (GCC: {', '.join(sorted(failures))}, recorded)"
            print(f"{name:14} {'ok  ' if ok else 'FAIL'} {summary}")
            sys.stdout.flush()
            failed += not ok
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
