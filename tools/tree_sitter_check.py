#!/usr/bin/env python3
"""The Tree-sitter grammar (editors/tree-sitter-ceesafe) against the sources the compiler accepts: every `.cee` file
of the compiler (src/), the libraries (lib/) and the corpus programs that compile (run-pass, run-fail, check-pass) is
parsed, and a file whose tree has an ERROR or MISSING node is a problem. The compiler's own parser is the reference:
what it accepts, an editor must not mark as broken.

With --highlights, the queries in editors/zed/languages/ceesafe are also compiled against the grammar (a query that
names a node or field the grammar does not have is an error).

usage: python3 tools/tree_sitter_check.py --tree-sitter PATH [--highlights] [--show N]
"""
import argparse
import glob
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
GRAMMAR = os.path.join(ROOT, "editors", "tree-sitter-ceesafe")
QUERIES = os.path.join(ROOT, "editors", "zed", "languages", "ceesafe")
CORPUS = os.path.join(ROOT, "tests", "reference", "tests", "corpus")


def sources():
    found = []
    for pattern in ("src/**/*.cee", "lib/**/*.cee"):
        found += glob.glob(os.path.join(ROOT, pattern), recursive=True)
    for family in ("run-pass", "run-fail", "check-pass"):
        found += glob.glob(os.path.join(CORPUS, family, "*.cee"))
    return sorted(found)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--tree-sitter", required=True, help="the tree-sitter CLI")
    parser.add_argument("--highlights", action="store_true")
    parser.add_argument("--show", type=int, default=10, help="how many failing files to name")
    args = parser.parse_args()
    files = sources()
    problems = []
    failing = []
    chunk = 200
    for start in range(0, len(files), chunk):
        batch = files[start:start + chunk]
        run = subprocess.run([args.tree_sitter, "parse", "--quiet", "--stat"] + batch, cwd=GRAMMAR,
                             capture_output=True, text=True, timeout=600)
        for line in run.stdout.splitlines():
            if "(ERROR" in line or "(MISSING" in line:
                failing.append(line.split()[0])
    for name in failing[:args.show]:
        print("FAIL", os.path.relpath(name, ROOT) if os.path.isabs(name) else name)
    if failing:
        problems.append(f"{len(failing)} of {len(files)} files have a parse error")
    if args.highlights:
        for query in sorted(glob.glob(os.path.join(QUERIES, "*.scm"))):
            run = subprocess.run([args.tree_sitter, "query", query, files[0]], cwd=GRAMMAR, capture_output=True,
                                 text=True, timeout=120)
            if run.returncode != 0:
                problems.append(f"{os.path.basename(query)}: {run.stderr.strip()[:300]}")
    for p in problems:
        print("FAIL", p)
    print(f"tree_sitter_check: {len(files)} files: {len(problems)} problems")
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
