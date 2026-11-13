#!/usr/bin/env python3
"""`ceesafec test` (ADR 0049) on tests/packages/test-command, copied to a temporary directory:

1. `check` checks the package with its tests.
2. `test --format json` natively and with --interp: each test's outcome, and a part of its message, as
   tests/packages/test-command/expected.tsv says (reviewed by hand).
3. `test --list --filter fails`: the matching names, in the names' order; a filter that matches nothing is exit status 1;
   `--interp --sweep` is a usage error (2), since the sweeps are native only; `test --help` prints the options.
4. `test --sweep alloc` and `--sweep cancel`: no sweep failure, and as many runs as the harness reports allocation
   attempts or suspension points when run directly (the counts tools/fault_sweep.py and tools/cancel_sweep.py sweep).
5. `build`: the program runs its own main, and `--emit c` holds no test function.
6. Which unwraps `--sweep alloc` sets apart (ADR 0129), on tests/packages/sweep-unwraps: a `!` in test-only code (an
   `@test` function, a closure in one, a file of the test directory) is counted apart; a `!` in production code is a
   sweep failure at its own position, whichever test reached it and though its file holds tests; `--sweep-strict`
   fails them all; `--sweep cancel` sets nothing apart.

usage: python3 tools/test_command_check.py --compiler out/ceesafec
"""

import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
WINDOWS = os.name == "nt"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    problems = []
    fixture = os.path.join(ROOT, "tests", "packages", "test-command")
    expected = []
    for line in open(os.path.join(fixture, "expected.tsv"), encoding="utf-8"):
        if line.startswith("#") or not line.strip():
            continue
        name, native, interp, message = line.rstrip("\n").split("\t")
        expected.append((name, native, interp, message))

    def run(argv, cwd):
        return subprocess.run([compiler] + argv, cwd=cwd, capture_output=True, text=True, timeout=600)

    with tempfile.TemporaryDirectory(prefix="ceesafe-test-command-") as work:
        package = os.path.join(work, "test-command")
        shutil.copytree(fixture, package, ignore=shutil.ignore_patterns("out"))

        # 1. check
        p = run(["check"], package)
        if p.returncode != 0:
            problems.append(f"check failed: {p.stderr[-400:]}")

        # 2. outcomes, natively and in the interpreter
        for column, flags in ((1, []), (2, ["--interp"])):
            p = run(["test", "--format", "json", "--timeout", "3", "--jobs", "4"] + flags, package)
            try:
                document = json.loads(p.stdout)
            except ValueError:
                problems.append(f"test {' '.join(flags)}: no JSON document: {p.stdout[:300]}{p.stderr[-300:]}")
                continue
            got = {t["name"]: t for t in document["tests"]}
            names = [t["name"] for t in document["tests"]]
            if names != [e[0] for e in expected]:
                problems.append(f"test {' '.join(flags)}: names {names}")
            for entry in expected:
                test = got.get(entry[0])
                if test is None:
                    continue
                if test["outcome"] != entry[column]:
                    problems.append(f"test {' '.join(flags)}: {entry[0]} was {test['outcome']!r}, not {entry[column]!r}: "
                                    f"{test['message'][:200]!r}")
                elif entry[3] != "-" and entry[column] not in ("skipped",) and entry[3] not in test["message"]:
                    problems.append(f"test {' '.join(flags)}: {entry[0]}'s message {test['message']!r} lacks {entry[3]!r}")
            if p.returncode != 1:
                problems.append(f"test {' '.join(flags)}: exit status {p.returncode}, not 1 (tests fail)")
            if column == 1:
                shown = got.get("printsAndFails", {}).get("stdout", "")
                if "shown because it fails" not in shown:
                    problems.append(f"printsAndFails's output was not captured: {shown!r}")

        # 3. list, an empty filter, --interp --sweep, --help
        p = run(["test", "--list", "--filter", "fails"], package)
        want = sorted((e[0] for e in expected if "fails" in e[0]), key=lambda n: n.encode())
        if p.stdout.split() != want:
            problems.append(f"test --list --filter fails printed {p.stdout.split()}, not {want}")
        p = run(["test", "--filter", "noSuchTest"], package)
        if p.returncode != 1 or "noSuchTest" not in p.stderr:
            problems.append(f"a filter matching nothing: exit status {p.returncode}, {p.stderr[-200:]!r}")
        p = run(["test", "--interp", "--sweep", "alloc"], package)
        if p.returncode != 2 or "--interp" not in p.stderr:
            problems.append(f"--interp --sweep: exit status {p.returncode}, not 2: {p.stderr[-200:]!r}")
        p = run(["test", "--help"], package)
        if p.returncode != 0 or "--sweep" not in p.stdout or "exited" not in p.stdout:
            problems.append(f"test --help: exit status {p.returncode}, {p.stdout[:200]!r}")

        # 4. sweeps, against the harness's own counts
        harness = os.path.join(package, "out", "tc-tests" + (".exe" if WINDOWS else ""))
        for sweep, test, key in (("alloc", "allocates", "ceesafe alloc: attempts="),
                                 ("cancel", "sleeps", "ceesafe cancel: points=")):
            direct = subprocess.run([harness, "--ceesafe-test", test, "0", "0", "0"], capture_output=True, text=True)
            counts = re.findall(re.escape(key) + r"(\d+)", direct.stderr)
            if direct.returncode != 0 or not counts:
                problems.append(f"the harness run of {test} reported no count: {direct.stderr[:300]!r}")
                continue
            count = int(counts[-1])
            p = run(["test", "--sweep", sweep, "--filter", test, "--format", "json"], package)
            try:
                document = json.loads(p.stdout)
            except ValueError:
                problems.append(f"--sweep {sweep}: no JSON document: {p.stderr[-300:]}")
                continue
            runs = document["sweepRuns"]
            wanted = count if count <= 200 else None
            if document["sweepFailures"] or (wanted is not None and runs != wanted) or runs == 0:
                problems.append(f"--sweep {sweep} on {test}: {runs} runs (the harness counted {count}), "
                                f"failures {document['sweepFailures']}")

        # 5. build: no test in the program
        cdir = os.path.join(work, "c")
        p = run(["build", "--emit", "c", "-o", cdir], package)
        text = "".join(open(f, encoding="utf-8", errors="replace").read() for f in glob.glob(os.path.join(cdir, "*.c")))
        if p.returncode != 0:
            problems.append(f"build --emit c failed: {p.stderr[-300:]}")
        for name, _, _, _ in expected:
            if re.search(r"\bceeF_" + name + r"\b", text):
                problems.append(f"the program's C defines the test {name}")
        p = run(["build"], package)
        exe = os.path.join(package, "out", "tc" + (".exe" if WINDOWS else ""))
        out = subprocess.run([exe], capture_output=True, text=True).stdout.strip() if p.returncode == 0 else ""
        if out != "the program":
            problems.append(f"build: the program printed {out!r}: {p.stderr[-300:]}")

        # 6. the sweep's unwraps: test-only code's are set apart, production code's are failures (ADR 0129)
        unwraps = os.path.join(work, "sweep-unwraps")
        shutil.copytree(os.path.join(ROOT, "tests", "packages", "sweep-unwraps"), unwraps,
                        ignore=shutil.ignore_patterns("out"))
        marks = {}
        source = open(os.path.join(unwraps, "src", "main.cee"), encoding="utf-8").read().split("\n")
        for number, text in enumerate(source, 1):
            if "// sweep: " in text:
                marks[text.split("// sweep: ")[1].strip()] = f"src/main.cee:{number}:"
        # test: the production position its sweep failure names, or None when its own unwrap is set apart
        wanted = {
            "returnsIt": "handled",
            "unwrapsItself": None,
            "unwrapsInItsClosure": None,
            "unwrapsInExpressionBody": None,
            "unwrapsInTestDirectory": None,
            "callsHelperInTests": None,
            "callsProduction": marks.get("production"),
            "callsProductionFromTestDirectory": marks.get("production"),
            "callsGeneric": marks.get("generic"),
            "callsHelperBeside": marks.get("helper beside"),
        }
        apart = sorted(n for n, w in wanted.items() if w is None)
        failing = {n: w for n, w in wanted.items() if w not in (None, "handled")}

        def sweep(flags):
            p = run(["test", "--sweep"] + flags + ["--format", "json"], unwraps)
            try:
                return p, json.loads(p.stdout)
            except ValueError:
                problems.append(f"sweep-unwraps {' '.join(flags)}: no JSON document: {p.stdout[:300]}{p.stderr[-300:]}")
                return p, None

        p, document = sweep(["alloc"])
        if document is not None:
            got = {}
            for failure in document["sweepFailures"]:
                got.setdefault(failure.split(":")[0], []).append(failure)
            if len(marks) != 3 or sorted(got) != sorted(failing) or p.returncode != 1:
                problems.append(f"sweep-unwraps: failures {document['sweepFailures']}, exit status {p.returncode}; "
                                f"wanted one each for {sorted(failing)}")
            for name, position in failing.items():
                for failure in got.get(name, []):
                    if f"unwrapped an Err value at {position}" not in failure:
                        problems.append(f"sweep-unwraps: {name}'s failure is not at {position}: {failure!r}")
            if document["sweepUnwrappedByTest"] != len(apart) or document["sweepRuns"] != len(wanted):
                problems.append(f"sweep-unwraps: {document['sweepUnwrappedByTest']} set apart in "
                                f"{document['sweepRuns']} runs, not {len(apart)} in {len(wanted)}")
        # a test that unwraps only in test-only code passes the sweep, and fails it under --sweep-strict
        for name in apart:
            p, document = sweep(["alloc", "--filter", name])
            if document is not None and (p.returncode != 0 or document["sweepFailures"]
                                         or document["sweepUnwrappedByTest"] != 1):
                problems.append(f"sweep-unwraps: {name} alone: exit status {p.returncode}, {document['sweepFailures']}, "
                                f"{document['sweepUnwrappedByTest']} set apart")
            p, document = sweep(["alloc", "--sweep-strict", "--filter", name])
            if document is not None and (p.returncode != 1 or len(document["sweepFailures"]) != 1
                                         or document["sweepUnwrappedByTest"] != 0):
                problems.append(f"sweep-unwraps: {name} under --sweep-strict: exit status {p.returncode}, "
                                f"{document['sweepFailures']}")
        p = run(["test", "--sweep", "alloc", "--filter", "unwrapsItself"], unwraps)
        if "0 sweep failure(s) in 1 sweep run(s) (1 unwrapped by the test itself)" not in p.stdout:
            problems.append(f"sweep-unwraps: the summary's words: {p.stdout[-200:]!r}")
        p, document = sweep(["cancel"])
        if document is not None and (p.returncode != 0 or document["sweepUnwrappedByTest"] != 0):
            problems.append(f"sweep-unwraps --sweep cancel: exit status {p.returncode}, {document}")
        p = run(["build"], unwraps)
        if p.returncode != 0:
            problems.append(f"sweep-unwraps: build failed: {p.stderr[-300:]}")

    print(f"test_command_check: {len(expected)} tests, 6 checks; {len(problems)} problems")
    for problem in problems:
        print("  FAIL " + problem)
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
