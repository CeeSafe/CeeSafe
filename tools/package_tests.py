#!/usr/bin/env python3
"""Package tests for system libraries in the manifest (ADR 0047), on a copy of tests/packages in a temporary directory:

1. native-links: a C static library is built here (cc and ar, or cl and lib on Windows) into a directory named by
   $CEESAFE_TEST_LIBDIR; `wrap` links it, with its header from `include-dirs`, `mid` uses `wrap`, `app` uses `mid`.
   `ceesafec fetch` then `build` in app: the program prints 42 (2 × (20 + 1), the scale from the header).
2. `--emit c` writes ceesafe_link.txt with the include directory, the C file, `-L` and `-l` (cl's spelling with cl).
3. Without $CEESAFE_TEST_LIBDIR, `build` fails with N0321 naming it.
4. The package hash (what a git or registry dependency is locked by) changes when wrap's `links` does.
5. native-errors: `check` reports N0321 for each of its five invalid values.
6. impl-files: a method merged from an `impl` block in another file (ADR 0064 §1) panics at its own position.
7. impl-files: diagnostics in that method name its own file and line.
8. dependency-tests: `test` in app lists and runs app's test only, not the `@test` its dependency keeps in src/.
9. borrowed-listener: `test` of a library that only borrows AsyncTcpListener builds and passes (the C backend once
   panicked on it, then left the listener's drop undefined).
10. dependency-warnings: `check` in app reports app's N0400 and not its dependency's, and succeeds; with
    `--deny-warnings` it fails (ADR 0073 §2.4).
11. shadowing: a parameter, local, loop variable, closure parameter and binding named like the dependency `tls`
    (ADR 0074 §2.10) beside `tls.X` in types and outside their scopes, and associated constants through the package
    (`tls.TlsConfig.DEFAULT_PORT`, `tls.Version.COUNT`, ADR 0074 §2.7): `check` reports the two N0401 warnings only,
    and the program prints what it should, in the interpreter and compiled.
12. shadowing/item: an item named like the dependency is still N0315; shadowing/errors: another package's associated
    constant that is not public is N0312.
13. A `ceesafec` requirement in the manifest this ceesafec does not meet is N0321 (ADR 0076).
14. qualified-generics: `dep.f<T>(…)` and `dep.S<T>.m(…)` keep their type arguments.
15. c-names (ADR 0081 §2.2, §5): one C name declared by `sys` and again by a dependant is N0201 at the dependant's
    declaration, with a note at sys's and the hint to use `sys.strlen` (owned), or to make sys's declaration `public`
    (hidden); two files of one package give N0201 at the later file (two-files); `ceesafec fix` removes the
    declaration identical to sys's, with its doc lines and trailing comment, and an extern block it was alone in, and
    leaves one with another parameter type, result type or `@cname` (fix); with cc, mm and zz declaring one name and
    zz depending on cc, cc owns it for every order of app's `[dependencies]` (order, ADR 0081 §7); a dependant calls
    sys's `public` externs and a wrapper with a `borrow` parameter, in the interpreter and compiled (run).
16. same-names (ADR 0082 §2.1, §2.5): app, alpha and beta each have src/util.cee, and alpha has src/a/mod.cee and
    src/b/mod.cee. `build --split --emit c` writes one unit per file at c/<package>/<path>.c, each with its own
    functions, and names the dependencies' files by package in the location table; `build --split` with `--jobs 1`
    and `--jobs 4` makes programs that print the same, from the same C; an error in alpha's util.cee is reported as
    `../alpha/src/util.cee` with `"package": "alpha"`; two files whose units differ only in letter case are N0363
    (through two files outside the working directory, named by their file names, so that it runs on a file system that
    ignores case too; and in a package where the file system keeps `Util.cee` and `util.cee` apart), while two files
    whose units would be identical (two `util.cee` outside the working directory; `../x.cee` and `__/x.cee`) get
    `util-2.c` and build (ADR 0082 §7). Through a C compiler that logs its commands (POSIX), `--jobs 4` compiles each
    unit alone to its own object and links them; a failing compile exits 1 with the output in the units' order; a
    bad `--cflag`'s message is printed once with a count; `build --jobs` without `--split` is a usage error.
17. doc: `ceesafec doc` lists an interface's associated types and constants with its methods, in source order
    (ADR 0086 §2.4, §2.5).

usage: python3 tools/package_tests.py --compiler out/ceesafec
"""

import argparse
import itertools
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

        # 6. A method merged from an `impl` block in another file keeps that file (ADR 0064 §1): its bounds check
        #    reports the method's own position, not an offset into the struct's file.
        impl_files = os.path.join(work, "impl-files")
        shutil.copytree(os.path.join(ROOT, "tests", "packages", "impl-files"), impl_files)
        p = run([compiler, "build"], impl_files)
        if p.returncode != 0:
            problems.append(f"impl-files: build failed: {p.stdout[:400]}{p.stderr[:400]}")
        else:
            exe = os.path.join(impl_files, "out", "impl_files" + (".exe" if WINDOWS else ""))
            p = run([exe], impl_files)
            if p.returncode == 0 or p.stdout.split() != ["20"] or "src/more/cells.cee:24:" not in p.stderr.replace("\\", "/"):
                problems.append(f"impl-files: expected 20 then a bounds panic at src/more/cells.cee:24: "
                                f"{p.returncode} {p.stdout[:200]!r} {p.stderr[:200]!r}")

        # 7. A diagnostic in such a method names its own file and line.
        cells = os.path.join(impl_files, "src", "more", "cells.cee")
        with open(cells) as f:
            text = f.read()
        with open(cells, "w") as f:
            f.write(text.replace("let n = this.base;", "let n: bool = this.base;"))
        p = run([compiler, "check", "--diagnostics-format=json"], impl_files)
        try:
            diagnostics = json.loads(p.stdout)["diagnostics"]
            places = [(d["file"].replace("\\", "/"), d["start"]["line"]) for d in diagnostics]
        except (ValueError, KeyError):
            places = []
        if not places or places[0] != ("src/more/cells.cee", 23) or any(f != "src/more/cells.cee" for f, _ in places):
            problems.append(f"impl-files: expected diagnostics in src/more/cells.cee, the first at line 23: {places} "
                            f"{p.stdout[:300]}{p.stderr[:300]}")

        # 8. A dependency's `@test` in its src/ is private to it: the dependent's harness lists only its own tests.
        dependency_tests = os.path.join(work, "dependency-tests")
        shutil.copytree(os.path.join(ROOT, "tests", "packages", "dependency-tests"), dependency_tests)
        app = os.path.join(dependency_tests, "app")
        p = run([compiler, "test", "--list"], app)
        q = run([compiler, "test"], app)
        if p.returncode != 0 or p.stdout.split() != ["quadrupleOfThree"] or q.returncode != 0:
            problems.append(f"dependency-tests: expected app's one test to be listed and pass: {p.returncode} "
                            f"{p.stdout[:200]!r} {q.returncode} {q.stdout[-300:]!r}{q.stderr[-300:]!r}")

        # 9. A library that only borrows a listener: its test build has the listener's drop and the vector helpers'
        #    result type.
        listener = os.path.join(work, "borrowed-listener")
        shutil.copytree(os.path.join(ROOT, "tests", "packages", "borrowed-listener"), listener)
        p = run([compiler, "test"], listener)
        if p.returncode != 0:
            problems.append(f"borrowed-listener: test failed: {p.stdout[-400:]}{p.stderr[-400:]}")

        # 10. Warnings are the root package's only, and never fail a command unless --deny-warnings is given.
        warnings = os.path.join(work, "dependency-warnings")
        shutil.copytree(os.path.join(ROOT, "tests", "packages", "dependency-warnings"), warnings)
        app = os.path.join(warnings, "app")
        p = run([compiler, "check", "--diagnostics-format=json"], app)
        q = run([compiler, "check", "--deny-warnings"], app)
        try:
            document = json.loads(p.stdout)
            found = [(d["code"], d["severity"], d["file"].replace("\\", "/")) for d in document["diagnostics"]]
        except (ValueError, KeyError):
            found = []
        if p.returncode != 0 or found != [("N0400", "warning", "src/main.cee")] or q.returncode != 1:
            problems.append(f"dependency-warnings: expected app's one warning, status 0, and 1 with --deny-warnings: "
                            f"{p.returncode} {found} {q.returncode} {p.stdout[:300]}{p.stderr[:300]}")

        # 11. A local may shadow a dependency (ADR 0074 §2.10); associated constants through a package (§2.7).
        shadowing = os.path.join(work, "shadowing")
        shutil.copytree(os.path.join(ROOT, "tests", "packages", "shadowing"), shadowing)
        app = os.path.join(shadowing, "app")
        expected = "466\n993\n2\n7\ntrue\n465\n0\n1\n5\n587"
        p = run([compiler, "check", "--diagnostics-format=json"], app)
        try:
            found = [(d["code"], d["severity"], d["start"]["line"]) for d in json.loads(p.stdout)["diagnostics"]]
        except (ValueError, KeyError):
            found = []
        if p.returncode != 0 or found != [("N0401", "warning", 9), ("N0401", "warning", 33)]:
            problems.append(f"shadowing: expected two N0401 warnings (lines 9 and 33) and status 0: "
                            f"{p.returncode} {found} {p.stdout[:300]}{p.stderr[:300]}")
        p = run([compiler, "run", "--interp"], app)
        if p.returncode != 0 or p.stdout.strip() != expected:
            problems.append(f"shadowing: run --interp printed {p.stdout.strip()!r} ({p.returncode}) {p.stderr[-300:]}")
        p = run([compiler, "build"], app)
        exe = os.path.join(app, "out", "app.exe" if WINDOWS else "app")
        if p.returncode != 0:
            problems.append(f"shadowing: build failed: {p.stderr.strip()[-600:]}")
        else:
            out = run([exe], app).stdout.strip()
            if out != expected:
                problems.append(f"shadowing: the program printed {out!r}")

        # 12. N0315 for an item; N0312 for a constant that is not public.
        for name, code in (("item", "N0315"), ("errors", "N0312")):
            p = run([compiler, "check", "--diagnostics-format=json"], os.path.join(shadowing, name))
            try:
                found = [d["code"] for d in json.loads(p.stdout)["diagnostics"]]
            except (ValueError, KeyError):
                found = []
            if p.returncode != 1 or found != [code]:
                problems.append(f"shadowing/{name}: expected {code} alone: {p.returncode} {found} {p.stderr[:300]}")

        # 13. A compiler requirement (ADR 0076): one this ceesafec does not meet is N0321 naming both versions; one it
        #     meets checks.
        pinned = os.path.join(work, "compiler-requirement")
        os.makedirs(os.path.join(pinned, "src"))
        with open(os.path.join(pinned, "src", "main.cee"), "w") as f:
            f.write("fn main() {}\n")
        for requirement, accepted in ((">=999.0.0", False), (">=0.0.1, <999.0.0", True)):
            with open(os.path.join(pinned, "ceesafe.toml"), "w") as f:
                f.write(f'[package]\nname = "pinned"\nceesafec = "{requirement}"\n')
            p = run([compiler, "check"], pinned)
            needs = f"this package needs ceesafec {requirement}; this is ceesafec "
            if accepted and p.returncode != 0:
                problems.append(f"compiler-requirement: {requirement!r} was refused: {p.stderr[:300]}")
            if not accepted and (p.returncode == 0 or "N0321" not in p.stderr or needs not in p.stderr):
                problems.append(f"compiler-requirement: {requirement!r} was not N0321 naming it: {p.stderr[:300]}")

        # 14. A dependency's generic items called with explicit type arguments, `dep.f<T>(…)` and `dep.S<T>.m(…)`: the
        #     arguments go with the joined name (they were dropped until 2026-11-09, so `T` was inferred as i32).
        qualified = os.path.join(work, "qualified-generics")
        shutil.copytree(os.path.join(ROOT, "tests", "packages", "qualified-generics"), qualified)
        app = os.path.join(qualified, "app")
        expected = "29\ntrue"
        p = run([compiler, "check"], app)
        if p.returncode != 0:
            problems.append(f"qualified-generics: check failed: {p.stderr[:600]}")
        p = run([compiler, "run", "--interp"], app)
        if p.returncode != 0 or p.stdout.strip() != expected:
            problems.append(f"qualified-generics: run --interp printed {p.stdout.strip()!r} ({p.returncode}) "
                            f"{p.stderr[-300:]}")
        p = run([compiler, "build"], app)
        exe = os.path.join(app, "out", "app.exe" if WINDOWS else "app")
        if p.returncode != 0:
            problems.append(f"qualified-generics: build failed: {p.stderr.strip()[-600:]}")
        else:
            out = run([exe], app).stdout.strip()
            if out != expected:
                problems.append(f"qualified-generics: the program printed {out!r}")

        # 15. One owner per C name (ADR 0081): N0201 at the declaration that clashes, its note and hint, its fix.
        #     A note in a dependency names its file as the path from the package (ADR 0082 §2.1): ../sys/src/lib.cee.
        c_names = os.path.join(work, "c-names")
        shutil.copytree(os.path.join(ROOT, "tests", "packages", "c-names"), c_names)

        def n0201s(directory, command="check"):
            p = run([compiler, command, "--diagnostics-format=json"], directory)
            try:
                return p.returncode, [d for d in json.loads(p.stdout)["diagnostics"] if d["code"] == "N0201"]
            except (ValueError, KeyError):
                return p.returncode, [{"unparsed": p.stdout[:300] + p.stderr[:300]}]

        def where(d):
            return (d.get("file", "").replace("\\", "/"), d.get("start", {}).get("line"))

        hint = "use `sys.strlen`, which package 'sys' declares; remove this declaration"
        status, found = n0201s(os.path.join(c_names, "owned"))
        notes = found[0].get("notes", []) if len(found) == 1 else []
        if (status != 1 or len(found) != 1 or where(found[0]) != ("src/main.cee", 4)
                or not found[0]["message"].endswith(hint) or len(found[0].get("fixes", [])) != 1
                or [(where(n), n["message"]) for n in notes] != [(("../sys/src/lib.cee", 3), "package 'sys' declares it here")]):
            problems.append(f"c-names/owned: expected N0201 at src/main.cee:4 with sys's note, hint and fix: "
                            f"{status} {found}")
        status, found = n0201s(os.path.join(c_names, "hidden"))
        if (status != 1 or len(found) != 1 or where(found[0]) != ("src/main.cee", 4) or found[0].get("fixes")
                or "make its declaration `public` in package 'sys', then use `sys.abs`" not in found[0]["message"]
                or [where(n) for n in found[0].get("notes", [])] != [("../sys/src/lib.cee", 5)]):
            problems.append(f"c-names/hidden: expected N0201 at src/main.cee:4 with the hint to make sys's abs "
                            f"public: {status} {found}")
        status, found = n0201s(os.path.join(c_names, "two-files"))
        if (status != 1 or len(found) != 1 or where(found[0]) != ("src/main.cee", 5)
                or [where(n) for n in found[0].get("notes", [])] != [("src/a.cee", 3)]):
            problems.append(f"c-names/two-files: expected N0201 at src/main.cee:5, the later file, with a note at "
                            f"src/a.cee:3: {status} {found}")
        fix = os.path.join(c_names, "fix")
        status, found = n0201s(fix, "fix")
        with open(os.path.join(fix, "src", "main.cee")) as f:
            fixed = f.read()
        if (status != 1 or sorted(where(d) for d in found) != [("src/main.cee", 5), ("src/main.cee", 8),
                                                                ("src/main.cee", 10)]
                or "fn strlen(" in fixed or "Identical to sys's" in fixed or "with its doc comment" in fixed
                or "// c" in fixed or "strcmp" in fixed or "extern \"C\" {  }" in fixed
                or "fn memcmp(a: *u8, b: *u8, n: usize) -> i32;" not in fixed or '@cname("strlen")' not in fixed
                or "fn atoi(s: *CChar) -> i64;" not in fixed):
            problems.append(f"c-names/fix: expected strlen's declaration (its doc lines and comment too) and strcmp's "
                            f"block removed, and memcmp's, length's and atoi's kept: {status} {found} {fixed!r}")
        # The owner does not depend on the order of `[dependencies]` (ADR 0081 §7): cc, mm and zz declare strlen,
        # zz depends on cc; cc owns it, and mm and zz get N0201 with a note at cc's, in every order.
        order = os.path.join(c_names, "order", "app")
        for names in itertools.permutations(["cc", "mm", "zz"]):
            with open(os.path.join(order, "ceesafe.toml"), "w") as f:
                f.write('[package]\nname = "app"\nversion = "0.1.0"\n\n[dependencies]\n'
                        + "".join(f'{n} = {{ path = "../{n}" }}\n' for n in names))
            status, found = n0201s(order)
            got = sorted((where(d), [where(n) for n in d.get("notes", [])]) for d in found)
            want = [(("../mm/src/lib.cee", 3), [("../cc/src/lib.cee", 3)]),
                    (("../zz/src/lib.cee", 3), [("../cc/src/lib.cee", 3)])]
            if status != 1 or got != want:
                problems.append(f"c-names/order {', '.join(names)}: expected N0201 in mm and zz, owned by cc: "
                                f"{status} {got}")
        app = os.path.join(c_names, "run")
        expected = "5\ntrue\n2\n7"
        p = run([compiler, "check"], app)
        if p.returncode != 0:
            problems.append(f"c-names/run: check failed: {p.stderr[:600]}")
        p = run([compiler, "run", "--interp"], app)
        if p.returncode != 0 or p.stdout.strip() != expected:
            problems.append(f"c-names/run: run --interp printed {p.stdout.strip()!r} ({p.returncode}) {p.stderr[-300:]}")
        p = run([compiler, "build"], app)
        exe = os.path.join(app, "out", "run.exe" if WINDOWS else "run")
        if p.returncode != 0:
            problems.append(f"c-names/run: build failed: {p.stderr.strip()[-600:]}")
        else:
            out = run([exe], app).stdout.strip()
            if out != expected:
                problems.append(f"c-names/run: the program printed {out!r}")

        # 16. Units that never merge, and parallel C compiles (ADR 0082 §2.1, §2.5).
        same = os.path.join(work, "same-names")
        shutil.copytree(os.path.join(ROOT, "tests", "packages", "same-names"), same)
        app = os.path.join(same, "app")
        units = ["c/alpha/src/a/mod.c", "c/alpha/src/b/mod.c", "c/alpha/src/util.c", "c/app/src/main.c",
                 "c/app/src/util.c", "c/beta/src/util.c"]
        homes = {"c/alpha/src/util.c": ["ceeF_alpha_helper", "ceeF_alpha_scale"], "c/beta/src/util.c": ["ceeF_beta_helper"],
                 "c/alpha/src/a/mod.c": ["ceeF_alpha_left"], "c/alpha/src/b/mod.c": ["ceeF_alpha_right"],
                 "c/app/src/util.c": ["ceeF_helper"], "c/app/src/main.c": ["ceeF_main"]}

        def tree(directory):
            found = {}
            for d, _, names in os.walk(directory):
                for n in names:
                    path = os.path.join(d, n)
                    with open(path, "rb") as f:
                        found[os.path.relpath(path, directory).replace(os.sep, "/")] = f.read()
            return found

        emitted = []
        for jobs in ("1", "4"):
            out = os.path.join(same, "c-jobs" + jobs)
            p = run([compiler, "build", "--split", "--jobs", jobs, "--emit", "c", "-o", out], app)
            if p.returncode != 0:
                problems.append(f"same-names: build --split --emit c failed: {p.stderr[:600]}")
                break
            emitted.append(tree(out))
        if len(emitted) == 2:
            c = emitted[0]
            found = sorted(n for n in c if n.startswith("c/"))
            if found != units:
                problems.append(f"same-names: expected the units {units}, found {found}")
            for unit, functions in homes.items():
                text = c.get(unit, b"").decode()
                defined = [line.split("(")[0].split()[-1] for line in text.splitlines()
                           if line.startswith(("int32_t ceeF_", "void ceeF_")) and line.endswith(") {")]
                if sorted(defined) != sorted(functions):
                    problems.append(f"same-names: {unit} defines {defined}, not {functions}")
            header = c.get("ceesafe_program.h", b"").decode()
            for name in ("src/main.cee", "src/util.cee", "alpha/src/util.cee", "alpha/src/a/mod.cee", "beta/src/util.cee"):
                if f'[] = "{name}";' not in header:
                    problems.append(f"same-names: the location table does not name {name!r}")
            if emitted[0] != emitted[1]:
                problems.append("same-names: the C of --jobs 1 and --jobs 4 differ")
        printed = []
        for jobs in ("1", "4"):
            exe = os.path.join(same, "app-jobs" + jobs + (".exe" if WINDOWS else ""))
            p = run([compiler, "build", "--split", "--jobs", jobs, "-o", exe], app)
            if p.returncode != 0:
                problems.append(f"same-names: build --split --jobs {jobs} failed: {p.stderr[-600:]}")
                continue
            printed.append(run([exe], app).stdout.strip())
        if printed != ["11\n22\n300\n21"] * 2:
            problems.append(f"same-names: the programs printed {printed}")
        # What --jobs runs (ADR 0082 §2.5, §7), seen through a C compiler that logs its commands (POSIX sh): each C
        # file compiled alone with -c to its own object, the objects linked. A compile that fails exits 1, with the
        # compilers' output in the order of the files' paths although the first finish last; one output printed by
        # every compile (a bad flag's) is printed once, with a count.
        if not WINDOWS:
            wrapper = os.path.join(same, "cc-log")
            log = os.path.join(same, "cc.log")
            with open(wrapper, "w") as f:
                f.write('#!/bin/sh\n'
                        'printf "%s\\n" "$*" >> "$CEESAFE_TEST_CC_LOG"\n'
                        'if [ -n "$CEESAFE_TEST_CC_FAIL" ]; then\n'
                        '  for a in "$@"; do case "$a" in\n'
                        '    */c/alpha/*.c) sleep 1; echo "failed: $a" >&2; exit 1;;\n'
                        '    *.c) echo "failed: $a" >&2; exit 1;;\n'
                        '  esac; done\n'
                        'fi\n'
                        'exec ${CC:-cc} "$@"\n')
            os.chmod(wrapper, 0o755)
            exe = os.path.join(same, "app-logged")
            p = run([compiler, "build", "--split", "--jobs", "4", "--cc", wrapper, "-o", exe], app,
                    {"CEESAFE_TEST_CC_LOG": log})
            commands = open(log).read().splitlines() if os.path.exists(log) else []
            compiles = [c.split() for c in commands if " -c " in f" {c} "]
            sources = [next((a for a in c if a.endswith(".c")), "") for c in compiles]
            objects = [c[c.index("-o") + 1] for c in compiles if "-o" in c]
            unit_sources = sorted(s[s.rindex("/c/") + 1:] for s in sources if "/c/" in s)
            links = [c for c in commands if " -c " not in f" {c} " and all(o in c.split() for o in objects)]
            out = run([exe], app).stdout.strip() if p.returncode == 0 else ""
            if (p.returncode != 0 or unit_sources != units or len(set(objects)) != len(compiles)
                    or any(not o.endswith(".o") for o in objects) or len(links) != 1 or out != "11\n22\n300\n21"):
                problems.append(f"same-names: build --split --jobs 4 should compile each unit alone to its own object "
                                f"and link them: {p.returncode} {unit_sources} {len(objects)} objects, "
                                f"{len(links)} links, printed {out!r} {p.stderr[-300:]}")
            p = run([compiler, "build", "--split", "--jobs", "4", "--cc", wrapper, "-o", exe], app,
                    {"CEESAFE_TEST_CC_LOG": log, "CEESAFE_TEST_CC_FAIL": "1"})
            failed = [line[len("failed: "):] for line in p.stderr.splitlines() if line.startswith("failed: ")]
            names = [os.path.relpath(f, os.path.commonpath(failed)).replace(os.sep, "/") for f in failed] if failed else []
            if p.returncode != 1 or len(failed) < len(units) or names != sorted(names):
                problems.append(f"same-names: a failing split build should exit 1 with the compilers' output in the "
                                f"order of the C files' paths: {p.returncode} {names} {p.stderr[-300:]}")
            p = run([compiler, "build", "--split", "--jobs", "4", "--cflag", "-fno-such-flag-ceesafe", "-o", exe], app)
            if p.returncode != 1 or "the C compiler printed the same for " not in p.stderr:
                problems.append(f"same-names: a bad --cflag should fail the build (1) and its message be printed "
                                f"once, with a count: {p.returncode} {p.stderr[-400:]}")
        # build --jobs without --split is a usage error (ADR 0082 §2.5).
        p = run([compiler, "build", "--jobs", "4", "--emit", "c", "-o", os.path.join(same, "c-nosplit")], app)
        if p.returncode != 2 or "--jobs needs --split" not in p.stderr:
            problems.append(f"same-names: build --jobs without --split should be a usage error (2): {p.returncode} "
                            f"{p.stderr[:200]}")
        util = os.path.join(same, "alpha", "src", "util.cee")
        with open(util) as f:
            text = f.read()
        with open(util, "w") as f:
            f.write(text.replace("x + 10;", "x + true;"))
        p = run([compiler, "check", "--diagnostics-format=json"], app)
        try:
            found = [(d["code"], d["file"], d.get("package")) for d in json.loads(p.stdout)["diagnostics"]]
        except (ValueError, KeyError):
            found = []
        if found != [("N0205", "../alpha/src/util.cee", "alpha")] or not os.path.isfile(os.path.join(app, found[0][1])):
            problems.append(f"same-names: expected N0205 in ../alpha/src/util.cee of package alpha: {found} "
                            f"{p.stdout[:300]}{p.stderr[:300]}")
        with open(util, "w") as f:
            f.write(text)
        clash = os.path.join(work, "case-clash")
        for d, name, body in (("x", "Util.cee", "fn main() { Console.writeLine(twice(2)); }\n"),
                              ("y", "util.cee", "fn twice(n: i32) -> i32 => n * 2;\n")):
            os.makedirs(os.path.join(clash, d))
            with open(os.path.join(clash, d, name), "w") as f:
                f.write(body)
        os.makedirs(os.path.join(clash, "here"))
        files = [os.path.join(clash, "x", "Util.cee"), os.path.join(clash, "y", "util.cee")]
        p = run([compiler, "build", "--split", "--emit", "c", "--diagnostics-format=json", "-o",
                 os.path.join(clash, "c"), *files], os.path.join(clash, "here"))
        try:
            found = [(d["code"], d["file"]) for d in json.loads(p.stdout)["diagnostics"]]
        except (ValueError, KeyError):
            found = []
        if p.returncode != 1 or found != [("N0363", "util.cee")]:
            problems.append(f"same-names: expected N0363 at util.cee: {p.returncode} {found} {p.stderr[:300]}")
        p = run([compiler, "build", "--emit", "c", "-o", os.path.join(clash, "one"), *files], os.path.join(clash, "here"))
        if p.returncode != 0:
            problems.append(f"same-names: the same files without --split failed: {p.stderr[:300]}")
        # Files whose units would have one path are not N0363 (ADR 0082 §7): two files outside the working directory
        # with one name, and `../x.cee` beside `__/x.cee`; the later gets `-2`, and the program runs.
        same_path = os.path.join(work, "same-path")
        for d, name, body in (("p", "util.cee", "fn helperP() -> i32 => 1;\n"),
                              ("q", "util.cee", "fn helperQ() -> i32 => 2;\n"),
                              ("w/sub", "main.cee", "fn main() { Console.writeLine(helperP() + helperQ()); }\n"),
                              (".", "x.cee", "fn helperA() -> i32 => 3;\n"),
                              ("w/__", "x.cee", "fn helperB() -> i32 => 4;\n"),
                              ("w", "main.cee", "fn main() { Console.writeLine(helperA() + helperB()); }\n")):
            os.makedirs(os.path.join(same_path, d), exist_ok=True)
            with open(os.path.join(same_path, d, name), "w") as f:
                f.write(body)
        w = os.path.join(same_path, "w")
        for files, want_units, want_out in (
                ([os.path.join(same_path, "p", "util.cee"), os.path.join(same_path, "q", "util.cee"), "sub/main.cee"],
                 ["c/util/sub/main.c", "c/util/util-2.c", "c/util/util.c"], "3"),
                (["main.cee", "../x.cee", "__/x.cee"], ["c/main/__/x-2.c", "c/main/__/x.c", "c/main/main.c"], "7")):
            cdir = os.path.join(same_path, "c-" + want_out)
            p = run([compiler, "build", "--split", "--emit", "c", "-o", cdir, *files], w)
            found = sorted(n for n in tree(cdir) if n.startswith("c/")) if p.returncode == 0 else []
            exe = os.path.join(same_path, "prog-" + want_out + (".exe" if WINDOWS else ""))
            q = run([compiler, "build", "--split", "--jobs", "4", "-o", exe, *files], w)
            out = run([exe], w).stdout.strip() if q.returncode == 0 else ""
            if found != want_units or out != want_out:
                problems.append(f"same-names: {files} should make the units {want_units} and print {want_out}: "
                                f"{found} {out!r} {p.stderr[:300]}{q.stderr[:300]}")
        # On a file system that keeps letter case apart (Linux's), the same in a package.
        with open(os.path.join(app, "src", "Util.cee"), "w") as f:
            f.write("fn unused() -> i32 => 1;\n")
        if sorted(n for n in os.listdir(os.path.join(app, "src")) if n.lower() == "util.cee") == ["Util.cee", "util.cee"]:
            p = run([compiler, "build", "--split", "--emit", "c", "--diagnostics-format=json", "-o",
                     os.path.join(same, "c-clash")], app)
            try:
                found = [(d["code"], d["file"], d.get("package")) for d in json.loads(p.stdout)["diagnostics"]]
            except (ValueError, KeyError):
                found = []
            if p.returncode != 1 or found != [("N0363", "src/util.cee", "app")]:
                problems.append(f"same-names: expected N0363 at src/util.cee: {p.returncode} {found} {p.stderr[:300]}")

        # 17. An interface's associated types and constants in `ceesafec doc`.
        docs = os.path.join(work, "doc-interface")
        os.makedirs(docs)
        with open(os.path.join(docs, "kind.cee"), "w") as f:
            f.write("/// A kind.\npublic interface Kind {\n    const CODE: u32;\n    type Item: Copy;\n"
                    "    const FLAG: bool = false;\n    fn id(borrow this) -> u32;\n}\n")
        p = run([compiler, "doc", "kind.cee"], docs)
        want = ("public interface Kind {\n    const CODE: u32;\n    type Item: Copy;\n    const FLAG: bool = false;\n"
                "    fn id(borrow this) -> u32;\n}\n")
        if p.returncode != 0 or want not in p.stdout:
            problems.append(f"doc: expected the interface with its constants and types:\n{p.stdout}{p.stderr[:300]}")

    print(f"package_tests: 17 checks; {len(problems)} problems")
    for problem in problems:
        print("  FAIL " + problem)
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
