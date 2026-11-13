#!/usr/bin/env python3
"""Checks that `ceesafec fix` and `ceesafec fmt` write each file where they read it (spec/tooling.md §5.3, §9).

Each case makes a tree of files in a temporary directory, runs the command in one of its directories with the
files named by absolute paths, by relative paths with directories (`sub/f.cee`, `../x/f.cee`), through symbolic
links, or as a package (from its root and from a directory below it), and compares the whole tree with what it
was: exactly the files the case names have changed, to the text the case names, and no file has appeared or gone,
whatever its name. A file with a named file's base name stands in the working directory of most cases, so a
write to the base name is seen as a changed file, not only as a new one.

For `fix` the case also names the exit status, the number of `applied` lines on standard error (the rounds that
wrote) and what each says; `fix --dry-run`, `fmt --check` and `fmt --stdout` must change nothing.

usage: python3 tools/fix_paths_check.py --compiler out/ceesafec
"""

import argparse
import os
import subprocess
import sys
import tempfile

# N0211 with a fix (tooling.md §5.2): `let` becomes `var`.
FAULT = "fn {name}() -> i32 {{\n    let x = 1;\n    x = 2;\n    return x;\n}}\n"
FIXED = "fn {name}() -> i32 {{\n    var x = 1;\n    x = 2;\n    return x;\n}}\n"
MAIN_FAULT = "fn main() {\n    let x = 1;\n    x = 2;\n    Console.writeLine(x);\n}\n"
MAIN_FIXED = "fn main() {\n    var x = 1;\n    x = 2;\n    Console.writeLine(x);\n}\n"
UNFORMATTED = "fn main() {\n  Console.writeLine( 1 );\n}\n"
FORMATTED = "fn main() {\n    Console.writeLine(1);\n}\n"
MANIFEST = '[package]\nname = "app"\nversion = "0.1.0"\nkind = "executable"\n'


def run(argv, cwd):
    env = {k: v for k, v in os.environ.items() if not k.startswith("CEESAFE_")}
    return subprocess.run(argv, cwd=cwd, env=env, capture_output=True, text=True, timeout=300,
                          stdin=subprocess.DEVNULL)


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def tree(root):
    """Every file under root: its path from root to its text, or to ('link', target) for a symbolic link."""
    found = {}
    for directory, names, files in os.walk(root):
        for name in list(names) + files:
            path = os.path.join(directory, name)
            key = os.path.relpath(path, root)
            if os.path.islink(path):
                found[key] = ("link", os.readlink(path))
            elif os.path.isfile(path):
                with open(path, encoding="utf-8", newline="") as f:
                    found[key] = f.read()
    return found


class Case:
    """One run: the tree before, the directory it runs in, the arguments, and what must have changed."""

    def __init__(self, name, files, cwd, arguments, changed, links=None, status=0, applied=None, stdout=None):
        self.name = name
        self.files = files          # path from the root -> text
        self.links = links or {}    # path from the root -> target
        self.cwd = cwd
        self.arguments = arguments  # '{root}' is replaced by the tree's root
        self.changed = changed      # path from the root -> the text it must have afterwards
        self.status = status
        self.applied = applied      # fix: the `applied` lines expected on standard error
        self.stdout = stdout        # the standard output expected, if the case names it


def check(compiler, work, number, case, problems):
    root = os.path.realpath(os.path.join(work, f"case{number}"))
    for path, text in case.files.items():
        write(os.path.join(root, path), text)
    for path, target in case.links.items():
        os.makedirs(os.path.dirname(os.path.join(root, path)), exist_ok=True)
        os.symlink(target, os.path.join(root, path))
    cwd = os.path.join(root, case.cwd)
    os.makedirs(cwd, exist_ok=True)
    before = tree(root)
    argv = [compiler] + [a.replace("{root}", root) for a in case.arguments]
    p = run(argv, cwd)
    after = tree(root)
    label = f"{case.name} (`ceesafec {' '.join(case.arguments)}` in {case.cwd or '.'})"
    if p.returncode != case.status:
        problems.append(f"{label}: exit status {p.returncode}, expected {case.status}: {p.stderr[-300:]!r}")
    for path in sorted(set(before) | set(after)):
        was, now = before.get(path), after.get(path)
        expected = case.changed.get(path, was)
        if now == expected:
            continue
        if was is None:
            problems.append(f"{label}: a file appeared: {path}")
        elif now is None:
            problems.append(f"{label}: a file is gone: {path}")
        elif now == was:
            problems.append(f"{label}: {path} was not changed")
        elif path in case.changed:
            problems.append(f"{label}: {path} was changed to {now!r}, expected {expected!r}")
        else:
            problems.append(f"{label}: {path} was changed, and is not a file the command was given")
    if case.applied is not None:
        lines = [line for line in p.stderr.splitlines() if "applied" in line]
        if lines != case.applied:
            problems.append(f"{label}: {len(lines)} `applied` line(s) {lines!r}, expected {case.applied!r}")
    if case.stdout is not None and p.stdout != case.stdout:
        problems.append(f"{label}: standard output {p.stdout!r}, expected {case.stdout!r}")


def one(n, files):
    return [f"ceesafec fix: applied {n} fix(es) to {files} file(s)"]


def fix_cases():
    fault, fixed = MAIN_FAULT, MAIN_FIXED
    helper, helped = FAULT.format(name="helper"), FIXED.format(name="helper")
    decoy = "// not a file of the command\n" + fault
    package = {"pkg/ceesafe.toml": MANIFEST, "pkg/src/main.cee": fault, "pkg/src/util/helper.cee": helper,
               "pkg/docs/main.cee": decoy, "pkg/main.cee": decoy}
    packaged = {"pkg/src/main.cee": fixed, "pkg/src/util/helper.cee": helped}
    return [
        Case("fix: an absolute path outside the working directory",
             {"there/f.cee": fault, "here/f.cee": decoy}, "here", ["fix", "{root}/there/f.cee"],
             {"there/f.cee": fixed}, applied=one(1, 1)),
        Case("fix: an absolute path, nothing of that name here",
             {"there/f.cee": fault}, "here", ["fix", "{root}/there/f.cee"], {"there/f.cee": fixed},
             applied=one(1, 1)),
        Case("fix: an absolute path below the working directory",
             {"here/sub/f.cee": fault, "here/f.cee": decoy}, "here", ["fix", "{root}/here/sub/f.cee"],
             {"here/sub/f.cee": fixed}, applied=one(1, 1)),
        Case("fix: a relative path through the parent",
             {"there/f.cee": fault, "here/f.cee": decoy}, "here", ["fix", "../there/f.cee"],
             {"there/f.cee": fixed}, applied=one(1, 1)),
        Case("fix: a relative path with a directory",
             {"here/sub/f.cee": fault, "here/f.cee": decoy}, "here", ["fix", "sub/f.cee"],
             {"here/sub/f.cee": fixed}, applied=one(1, 1)),
        Case("fix: a file of the working directory",
             {"here/f.cee": fault}, "here", ["fix", "f.cee"], {"here/f.cee": fixed}, applied=one(1, 1)),
        Case("fix: two files of one base name, absolute",
             {"a/f.cee": fault, "b/f.cee": helper, "here/f.cee": decoy}, "here",
             ["fix", "{root}/a/f.cee", "{root}/b/f.cee"], {"a/f.cee": fixed, "b/f.cee": helped},
             applied=one(2, 2)),
        Case("fix: two files of one base name, relative",
             {"a/f.cee": fault, "b/f.cee": helper, "here/f.cee": decoy}, "here",
             ["fix", "../a/f.cee", "../b/f.cee"], {"a/f.cee": fixed, "b/f.cee": helped}, applied=one(2, 2)),
        Case("fix: a directory that is a symbolic link",
             {"real/deep/f.cee": fault, "here/f.cee": decoy}, "here", ["fix", "link/f.cee"],
             {"real/deep/f.cee": fixed}, links={"here/link": "../real/deep"}, applied=one(1, 1)),
        Case("fix: the parent of a symbolic link is the parent of what it names",
             {"real/deep/x.cee": "", "real/f.cee": fault, "here/f.cee": decoy}, "here", ["fix", "link/../f.cee"],
             {"real/f.cee": fixed}, links={"here/link": "../real/deep"}, applied=one(1, 1)),
        Case("fix: a file that is a symbolic link stays one",
             {"real/g.cee": fault, "here/g.cee": decoy}, "here", ["fix", "{root}/links/f.cee"],
             {"real/g.cee": fixed}, links={"links/f.cee": "../real/g.cee"}, applied=one(1, 1)),
        Case("fix: a package from its root", package, "pkg", ["fix"], packaged, applied=one(2, 2)),
        Case("fix: a package from a directory below its root", package, "pkg/docs", ["fix"], packaged,
             applied=one(2, 2)),
        Case("fix: a package from its source directory", package, "pkg/src/util", ["fix"], packaged,
             applied=one(2, 2)),
        Case("fix --dry-run: an absolute path", {"there/f.cee": fault, "here/f.cee": decoy}, "here",
             ["fix", "--dry-run", "{root}/there/f.cee"], {}, status=1, applied=[]),
        Case("fix --dry-run: a relative path", {"here/sub/f.cee": fault}, "here",
             ["fix", "--dry-run", "sub/f.cee"], {}, status=1, applied=[],
             stdout="sub/f.cee:2:5: declare 'x' with 'var': 'let' -> 'var'\n"),
        Case("fix --dry-run: a package from below its root", package, "pkg/docs", ["fix", "--dry-run"], {},
             status=1, applied=[]),
        Case("fix: nothing to fix writes nothing", {"there/f.cee": fixed}, "here", ["fix", "{root}/there/f.cee"],
             {}, applied=[]),
    ]


def fmt_cases():
    raw, done = UNFORMATTED, FORMATTED
    decoy = "// not a file of the command\n" + raw
    package = {"pkg/ceesafe.toml": MANIFEST, "pkg/src/main.cee": raw, "pkg/src/util/main.cee": "fn  helper( ) { }\n",
               "pkg/docs/main.cee": decoy, "pkg/main.cee": decoy}
    packaged = {"pkg/src/main.cee": done, "pkg/src/util/main.cee": "fn helper() {}\n"}
    return [
        Case("fmt: an absolute path outside the working directory",
             {"there/f.cee": raw, "here/f.cee": decoy}, "here", ["fmt", "{root}/there/f.cee"], {"there/f.cee": done}),
        Case("fmt: a relative path through the parent",
             {"there/f.cee": raw, "here/f.cee": decoy}, "here", ["fmt", "../there/f.cee"], {"there/f.cee": done}),
        Case("fmt: a relative path with a directory",
             {"here/sub/f.cee": raw, "here/f.cee": decoy}, "here", ["fmt", "sub/f.cee"], {"here/sub/f.cee": done}),
        Case("fmt: a directory, absolute",
             {"there/a/f.cee": raw, "there/b/f.cee": raw, "here/f.cee": decoy}, "here", ["fmt", "{root}/there"],
             {"there/a/f.cee": done, "there/b/f.cee": done}),
        Case("fmt: a directory that is a symbolic link",
             {"real/deep/f.cee": raw, "here/f.cee": decoy}, "here", ["fmt", "link/f.cee"],
             {"real/deep/f.cee": done}, links={"here/link": "../real/deep"}),
        Case("fmt: a file that is a symbolic link stays one",
             {"real/g.cee": raw, "here/g.cee": decoy}, "here", ["fmt", "{root}/links/f.cee"],
             {"real/g.cee": done}, links={"links/f.cee": "../real/g.cee"}),
        Case("fmt: a package from its root", package, "pkg", ["fmt"], packaged),
        Case("fmt: a package from a directory below its root", package, "pkg/docs", ["fmt"], packaged),
        Case("fmt: a package from its source directory", package, "pkg/src/util", ["fmt"], packaged),
        Case("fmt --check: an absolute path", {"there/f.cee": raw, "here/f.cee": decoy}, "here",
             ["fmt", "--check", "{root}/there/f.cee"], {}, status=1),
        Case("fmt --check: a package from below its root", package, "pkg/docs", ["fmt", "--check"], {}, status=1,
             stdout="../src/main.cee\n../src/util/main.cee\n"),
        Case("fmt --stdout: an absolute path", {"there/f.cee": raw, "here/f.cee": decoy}, "here",
             ["fmt", "--stdout", "{root}/there/f.cee"], {}, stdout=done),
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    problems = []
    fixes, formats = fix_cases(), fmt_cases()
    with tempfile.TemporaryDirectory(prefix="ceesafe-fix-paths-") as work:
        for number, case in enumerate(fixes + formats):
            check(compiler, work, number, case, problems)
    print(f"fix_paths_check: {len(fixes)} runs of fix and {len(formats)} of fmt, each tree compared with what it "
          f"was; {len(problems)} problems")
    for problem in problems:
        print("  FAIL " + problem)
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
