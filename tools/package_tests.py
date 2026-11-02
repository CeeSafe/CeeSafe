#!/usr/bin/env python3
"""Package tests for system libraries in the manifest (ADR 0047), on a copy of tests/packages in a temporary directory:

1. native-links: a C static library is built here (cc and ar, or cl and lib on Windows) into a directory named by
   $CEESAFE_TEST_LIBDIR; `wrap` links it, with its header from `include-dirs`, `mid` uses `wrap`, `app` uses `mid`.
   `ceesafec fetch` then `build` in app: the program prints 42 (2 × (20 + 1), the scale from the header).
2. `--emit c` writes ceesafe_link.txt with the include directory, the C file, `-L` and `-l` (cl's spelling with cl).
3. Without $CEESAFE_TEST_LIBDIR, `build` fails with N0321 naming it.
4. The package hash (what a git or registry dependency is locked by) changes when wrap's `links` does.
5. native-errors: `check` reports N0321 for each of its five invalid values.

usage: python3 tools/package_tests.py --compiler out/ceesafec
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
WINDOWS = os.name == "nt"


def run(argv, cwd, env=None):
    e = dict(os.environ)
    e.pop("CEESAFE_TEST_LIBDIR", None)
    if env:
        e.update(env)
    return subprocess.run(argv, cwd=cwd, env=e, capture_output=True, text=True)


def build_library(work):
    lib = os.path.join(work, "lib")
    os.makedirs(lib)
    source = os.path.join(work, "native-links", "clib", "mylib.c")
    include = os.path.join(work, "native-links", "clib", "include")
    if WINDOWS:
        steps = [["cl", "/nologo", "/c", "/I", include, source, "/Fo:" + os.path.join(lib, "mylib.obj")],
                 ["lib", "/nologo", "/OUT:" + os.path.join(lib, "mylib.lib"), os.path.join(lib, "mylib.obj")]]
    else:
        cc = os.environ.get("CC", "cc")
        steps = [[cc, "-c", "-I", include, source, "-o", os.path.join(lib, "mylib.o")],
                 ["ar", "rcs", os.path.join(lib, "libmylib.a"), os.path.join(lib, "mylib.o")]]
    for step in steps:
        p = run(step, work)
        if p.returncode != 0:
            sys.exit(f"package_tests: cannot build the C library: {' '.join(step)}\n{p.stdout}{p.stderr}")
    return lib


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    problems = []
    with tempfile.TemporaryDirectory(prefix="ceesafe-packages-") as work:
        os.environ["CEESAFE_HOME"] = os.path.join(work, "home")  # git checkouts here, not in the user's ~/.ceesafe
        for name in ("native-links", "native-errors"):
            shutil.copytree(os.path.join(ROOT, "tests", "packages", name), os.path.join(work, name))
        lib = build_library(work)
        env = {"CEESAFE_TEST_LIBDIR": lib}
        app = os.path.join(work, "native-links", "app")

        # 1. Two levels of dependency above the library.
        p = run([compiler, "fetch"], app, env)
        if p.returncode != 0:
            problems.append(f"fetch failed: {p.stderr.strip()}")
        p = run([compiler, "build"], app, env)
        exe = os.path.join(app, "out", "app.exe" if WINDOWS else "app")
        if p.returncode != 0:
            problems.append(f"build failed: {p.stderr.strip()[-600:]}")
        else:
            out = run([exe], app).stdout.strip()
            if out != "42":
                problems.append(f"the program printed {out!r}, not 42")

        # 2. The link file of --emit c.
        cdir = os.path.join(work, "app-c")
        p = run([compiler, "build", "--emit", "c", "-o", cdir], app, env)
        link = os.path.join(cdir, "ceesafe_link.txt")
        if p.returncode != 0 or not os.path.exists(link):
            problems.append(f"--emit c wrote no ceesafe_link.txt: {p.stderr.strip()[-300:]}")
        else:
            lines = open(link).read().splitlines()
            msvc = "cl" in os.path.basename(os.environ.get("CC", "")).lower() and WINDOWS
            def same(a, b):
                return os.path.normcase(os.path.realpath(a)) == os.path.normcase(os.path.realpath(b))
            def flagged(prefix, path):
                return any(l.startswith(prefix) and same(l[len(prefix):], path) for l in lines)
            if msvc:
                ok = flagged("/LIBPATH:", lib) and "mylib.lib" in lines
            else:
                ok = flagged("-L", lib) and "-lmylib" in lines
            ok = ok and any(same(l, os.path.join(work, "native-links", "wrap", "c", "shim.c")) for l in lines
                            if not l.startswith(("-", "/I", "/L")))
            if not ok:
                problems.append(f"ceesafe_link.txt lacks the library, its directory or wrap's C file: {lines}")
            if not any(l.startswith(("-I", "/I")) and l.rstrip("/\\").endswith("include") for l in lines):
                problems.append(f"ceesafe_link.txt lacks wrap's include directory: {lines}")

        # 3. An unset variable.
        p = run([compiler, "build"], app)
        if p.returncode == 0 or "N0321" not in p.stderr or "CEESAFE_TEST_LIBDIR" not in p.stderr:
            problems.append(f"an unset variable was not N0321 naming it: {p.stderr.strip()[:300]}")

        # 4. The package hash (tooling.md §4.3: a git or registry dependency's, which `publish` prints) follows
        #    `links`: wrap in a git repository of its own, published before and after the change.
        wrap = os.path.join(work, "native-links", "wrap")
        git = ["git", "-c", "user.name=test", "-c", "user.email=test@localhost"]
        run(git + ["init", "-q"], wrap)
        run(git + ["add", "-A"], wrap)
        run(git + ["commit", "-qm", "one"], wrap)
        user = os.path.join(work, "hash-user")
        os.makedirs(os.path.join(user, "src"))
        open(os.path.join(user, "src", "main.cee"), "w").write("fn main() {}\n")

        def locked_hash():
            commit = run(["git", "rev-parse", "HEAD"], wrap).stdout.strip()
            source = wrap.replace("\\", "/")
            open(os.path.join(user, "ceesafe.toml"), "w").write(
                f'[package]\nname = "user"\nversion = "0.1.0"\n\n[dependencies]\n'
                f'wrap = {{ git = "{source}", rev = "{commit}" }}\n')
            p = run([compiler, "fetch"], user, env)
            lock = os.path.join(user, "ceesafe.lock")
            text = open(lock).read() if os.path.exists(lock) else p.stderr
            return [l for l in text.splitlines() if l.startswith("hash")]

        before = locked_hash()
        toml = os.path.join(wrap, "ceesafe.toml")
        text = open(toml).read()
        open(toml, "w").write(text.replace('links = ["mylib"]', 'links = ["mylib", "m"]'))
        run(git + ["commit", "-qam", "two"], wrap)
        after = locked_hash()
        if not before or before == after:
            problems.append(f"the locked hash did not follow links: {before} {after}")

        # 5. Invalid values.
        p = run([compiler, "check", "--diagnostics-format=json"], os.path.join(work, "native-errors"))
        try:
            found = [d["code"] for d in json.loads(p.stdout)["diagnostics"]]
        except (ValueError, KeyError):
            found = [line for line in p.stdout.splitlines() + p.stderr.splitlines() if "N0321" in line]
        count = sum(1 for c in found if "N0321" in str(c))
        if count != 5:
            problems.append(f"native-errors: {count} N0321 diagnostics, not 5: {p.stdout[:400]}{p.stderr[:400]}")

    print(f"package_tests: 5 checks; {len(problems)} problems")
    for problem in problems:
        print("  FAIL " + problem)
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
