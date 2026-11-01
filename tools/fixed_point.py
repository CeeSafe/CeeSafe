#!/usr/bin/env python3
"""The first verified fixed point (ROADMAP P6).

From CeeSafe's sources at a commit (`git archive`, so nothing outside the tree leaks in):

  gen 1 = stage 0's build of CeeSafe (a ceesafec: a release, or one from tools/bootstrap_release.sh)
  gen 2 = gen 1's build of CeeSafe, and C2 the C gen 1 emits for it
  gen 3 = gen 2's build of CeeSafe, and C3 the C gen 2 emits for it

C2 and C3 (every file: the program's C and the embedded runtime) must be byte-equal, and so must the gen 2 and gen 3
executables (same C, flags and toolchain). The chain runs twice, from clean directories under two different absolute
paths, and the C of both runs must be equal; no output may contain a work directory's path.

Gen 1 onward runs with PATH holding only a directory of logging shims for the tools a build may start (cc and the
tools cc itself runs), so no other compiler is reachable; the shims' log is the subprocess audit.

A manifest (inputs, toolchain versions, sha256 of every output, the audit) is written to --manifest.

Every build is in the compiler's default form (since P7, the production defaults); --extra adds a switch to every
build, e.g. --extra=--split.

usage: python3 tools/fixed_point.py --stage0 CEESAFEC [--commit HEAD] [--cc cc] [--extra=--split]
           --work DIR --manifest FILE
"""
import argparse
import hashlib
import os
import platform
import shutil
import subprocess
import sys

SHIM = """#!/bin/sh
printf '%s' "${{0##*/}}" >> "{log}"
for a in "$@"; do printf ' %s' "$a" >> "{log}"; done
printf '\\n' >> "{log}"
exec "{real}" "$@"
"""


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def tree_hashes(directory):
    result = {}
    for base, _, names in os.walk(directory):
        for name in names:
            full = os.path.join(base, name)
            result[os.path.relpath(full, directory)] = sha256(full)
    return dict(sorted(result.items()))


def run(argv, cwd, env, log):
    log.append("$ " + " ".join(argv) + f"   (in {os.path.basename(cwd)})")
    p = subprocess.run(argv, cwd=cwd, env=env, capture_output=True, text=True)
    if p.returncode != 0:
        sys.exit(f"fixed_point: {' '.join(argv)} failed ({p.returncode}):\n{p.stdout}\n{p.stderr}")
    return p


def shims(directory, log_path, tools):
    os.makedirs(directory)
    for tool in tools:
        real = shutil.which(tool)
        if real is None:
            continue
        path = os.path.join(directory, tool)
        with open(path, "w") as f:
            f.write(SHIM.format(log=log_path, real=real))
        os.chmod(path, 0o755)


def chain(args, root, log):
    """One chain, from a clean directory: the sources, gen 1, 2, 3; returns the outputs' hashes."""
    if os.path.exists(root):
        shutil.rmtree(root)
    source = os.path.join(root, "ceesafe")
    os.makedirs(source)
    archive = subprocess.run(["git", "-C", args.repo, "archive", args.commit, "ceesafe.toml", "src"], capture_output=True,
                             check=True).stdout
    subprocess.run(["tar", "-x", "-C", source], input=archive, check=True)
    audit = os.path.join(root, "subprocesses.log")
    bin_dir = os.path.join(root, "path")
    # cc and what cc may run (the driver's linker, assembler and SDK tools on macOS).
    shims(bin_dir, audit, [args.cc, "ld", "as", "clang", "xcrun", "dsymutil", "ar", "lipo", "collect2"])
    env = {"PATH": bin_dir, "HOME": root, "TMPDIR": os.path.join(root, "tmp"), "LANG": "C", "LC_ALL": "C"}
    os.makedirs(env["TMPDIR"])
    gen = os.path.join(root, "gen")
    os.makedirs(gen)
    # Stage 0, with the full environment; its output is gen 1.
    stage0_env = dict(os.environ)
    # Each generation is gen/<n>/ceesafec: one file name, since macOS's ad-hoc signature records it.
    def exe(n):
        return os.path.join(gen, str(n), "ceesafec")

    run([args.stage0, "build", "--cc", args.cc, "-o", exe(1)] + args.extra, source, stage0_env, log)
    hashes = {"gen1": sha256(exe(1))}
    with open(audit, "w"):
        pass  # the audit starts with gen 1
    for n in (1, 2):
        c_dir = os.path.join(gen, f"c{n + 1}")
        run([exe(n), "build", "--emit", "c", "-o", c_dir] + args.extra, source, env, log)
        run([exe(n), "build", "--cc", args.cc, "-o", exe(n + 1)] + args.extra, source, env, log)
        hashes[f"c{n + 1}"] = tree_hashes(c_dir)
        hashes[f"gen{n + 1}"] = sha256(exe(n + 1))
    # gen 3 emits C once more: gen 3 is gen 2 rebuilt, so this must equal C3 too.
    c4 = os.path.join(gen, "c4")
    run([exe(3), "build", "--emit", "c", "-o", c4] + args.extra, source, env, log)
    hashes["c4"] = tree_hashes(c4)
    # No output names the work directory.
    for c in ("c2", "c3", "c4"):
        for base, _, names in os.walk(os.path.join(gen, c)):
            for name in names:
                with open(os.path.join(base, name), "rb") as f:
                    if root.encode() in f.read():
                        sys.exit(f"fixed_point: {c}/{name} contains the work directory's path")
    with open(audit) as f:
        commands = [line.split()[0] for line in f if line.strip()]
    return hashes, sorted(set(commands)), len(commands)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--stage0", required=True,
                        help="the stage-0 compiler: a ceesafec (a release, or tools/bootstrap_release.sh's)")
    parser.add_argument("--repo", default=os.path.normpath(os.path.join(os.path.dirname(__file__), "..")))
    parser.add_argument("--commit", default="HEAD")
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--work", required=True)
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--extra", action="append", default=[], help="another switch for every build")
    args = parser.parse_args()
    args.stage0 = os.path.abspath(os.path.expanduser(args.stage0))
    commit = subprocess.run(["git", "-C", args.repo, "rev-parse", args.commit], capture_output=True, text=True,
                            check=True).stdout.strip()
    work = os.path.abspath(args.work)
    log = []
    first = chain(args, os.path.join(work, "a"), log)
    second = chain(args, os.path.join(work, "deeper", "second-run", "b"), log)
    problems = []
    for hashes, where in ((first[0], "run a"), (second[0], "run b")):
        if hashes["c2"] != hashes["c3"]:
            problems.append(f"{where}: C2 and C3 differ")
        if hashes["c3"] != hashes["c4"]:
            problems.append(f"{where}: C3 and gen 3's C differ")
        if hashes["gen2"] != hashes["gen3"]:
            problems.append(f"{where}: the gen 2 and gen 3 executables differ")
    if first[0]["c3"] != second[0]["c3"]:
        problems.append("the two runs' C differs")
    if first[0]["gen3"] != second[0]["gen3"]:
        problems.append("the two runs' gen 3 executables differ")
    stage0_version = subprocess.run([args.stage0, "--version"], capture_output=True, text=True).stdout.strip()
    cc_version = subprocess.run([args.cc, "--version"], capture_output=True, text=True).stdout.splitlines()[0]
    lines = [
        "# CeeSafe fixed point (ROADMAP P6)",
        "",
        f"commit: {commit}",
        f"host: {platform.system()} {platform.machine()} {platform.release()}",
        f"stage 0: {stage0_version} ({args.stage0}, sha256 {sha256(args.stage0)})",
        f"cc: {args.cc}: {cc_version}",
        f"switches: {' '.join(args.extra) or '(none: the default form)'}",
        f"runs: {os.path.join(work, 'a')} and {os.path.join(work, 'deeper', 'second-run', 'b')}",
        f"result: {'FIXED POINT' if not problems else 'FAILED: ' + '; '.join(problems)}",
        "",
        "## Executables (sha256)",
    ]
    for name in ("gen1", "gen2", "gen3"):
        lines.append(f"- {name}: run a {first[0][name]}, run b {second[0][name]}")
    lines += ["", "## C emitted by gen 1, 2 and 3 (sha256; equal in both runs)"]
    for path, digest in first[0]["c3"].items():
        same = "c2 = c3 = c4" if first[0]["c2"].get(path) == digest == first[0]["c4"].get(path) else "DIFFERS"
        lines.append(f"- {path}: {digest} ({same})")
    lines += ["", "## Subprocess audit (gen 1 onward; PATH = the shims only)",
              f"- run a: {first[2]} processes, programs: {', '.join(first[1])}",
              f"- run b: {second[2]} processes, programs: {', '.join(second[1])}",
              "", "## Commands", ""] + [f"    {line}" for line in log]
    os.makedirs(os.path.dirname(os.path.abspath(args.manifest)), exist_ok=True)
    with open(args.manifest, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines[:9]))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
