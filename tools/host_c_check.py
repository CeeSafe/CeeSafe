#!/usr/bin/env python3
"""The same C from ceesafec on another host (docs/p8-plan.md S6): the gate "ceesafec built by MSVC emits the same C".

`run` makes, for every source file under the corpus root's tests/corpus, benchmarks and programs
(--root tests/reference), the C of
`ceesafec build --emit c` in the default form, with `--split` and with `-g` (the paths relative, as the comparison
tools give them), and records each build's exit status, standard output and standard error. `compare` then checks two
such trees byte for byte: every C file, and every status and output.

usage: python3 tools/host_c_check.py run --ceesafec EXE --root tests/reference --out DIR [--jobs N]
       python3 tools/host_c_check.py compare DIR_A DIR_B
"""
import argparse
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

FORMS = {"default": [], "split": ["--split"], "g": ["-g"]}


def inputs(root):
    found = []
    for top in ("tests/corpus", "benchmarks", "programs"):
        for directory, _, names in os.walk(os.path.join(root, top)):
            for name in names:
                if name.endswith(".cee"):
                    found.append(os.path.relpath(os.path.join(directory, name), root).replace(os.sep, "/"))
    return sorted(found)


def build(cee, root, out, path, form):
    case = os.path.join(out, form, path.replace("/", "__"))
    os.makedirs(case, exist_ok=True)
    p = subprocess.run([cee, "build", "--emit", "c", *FORMS[form], "-o", os.path.join(case, "c"), path], cwd=root,
                       capture_output=True, timeout=600)
    with open(os.path.join(case, "result"), "wb") as f:
        f.write(b"status %d\n--- stdout\n" % p.returncode + p.stdout + b"\n--- stderr\n" + p.stderr)


def files(tree):
    found = {}
    for directory, _, names in os.walk(tree):
        for name in names:
            full = os.path.join(directory, name)
            found[os.path.relpath(full, tree).replace(os.sep, "/")] = full
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="action", required=True)
    run = sub.add_parser("run")
    run.add_argument("--ceesafec", required=True)
    run.add_argument("--root", required=True)
    run.add_argument("--out", required=True)
    run.add_argument("--jobs", type=int, default=os.cpu_count())
    compare = sub.add_parser("compare")
    compare.add_argument("a")
    compare.add_argument("b")
    options = parser.parse_args()
    if options.action == "run":
        cee, root, out = os.path.abspath(options.ceesafec), os.path.abspath(options.root), os.path.abspath(options.out)
        jobs = [(path, form) for path in inputs(root) for form in FORMS]
        with ThreadPoolExecutor(options.jobs) as pool:
            list(pool.map(lambda j: build(cee, root, out, *j), jobs))
        print(f"host_c_check run: {len(jobs) // len(FORMS)} files x {len(FORMS)} forms")
        return 0
    a, b = files(options.a), files(options.b)
    differ = sorted(k for k in a.keys() & b.keys() if open(a[k], "rb").read() != open(b[k], "rb").read())
    only = sorted(a.keys() ^ b.keys())
    for k in (differ + only)[:10]:
        print(("differ: " if k in differ else "only in one: ") + k)
    cases = sum(1 for k in a if k.endswith("/result"))
    built = sum(1 for k, v in a.items() if k.endswith("/result") and open(v, "rb").read().startswith(b"status 0\n"))
    print(f"host_c_check compare: {cases} builds ({built} emitted C), {len(a)} files: {len(differ)} differ, "
          f"{len(only)} in one tree only")
    return 1 if differ or only else 0


if __name__ == "__main__":
    sys.exit(main())
