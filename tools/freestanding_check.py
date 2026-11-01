#!/usr/bin/env python3
"""The reference's FreestandingTests (cosmoos/profiles.md §4.4), run against any compiler.

For each freestanding corpus program:
- host: `build --profile freestanding --emit obj`; its undefined symbols are only the allowlist (nova_panic, memcpy,
  memmove, memset, memcmp) and its own extern "C" functions; linked with the program's C harness it runs with the
  expected output, status and panic;
- each CosmoOS triple (x86_64-unknown-none-elf, aarch64-unknown-none-elf), cross-compiled with CosmoOS's flags: the
  same allowlist; no constructors, TLS or unwind tables, no mutable global state, no COMMON symbols; and with ld.lld,
  the one object `--emit obj --ld ld.lld` makes has the same allowlist.

usage: python3 tools/freestanding_check.py --compiler PATH [--corpus DIR] [--nm llvm-nm] [--objdump llvm-objdump]
           [--lld ld.lld]
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run_corpus import expectations, read  # noqa: E402
from names import dialect  # noqa: E402

ALLOWLIST = {"nova_panic", "memcpy", "memmove", "memset", "memcmp"}
# The async executor's environment (ADR 0039; profiles.md §4.2), which a freestanding program using async needs.
ENVIRONMENT = {"ceesafe_env_now_ms", "ceesafe_env_idle", "ceesafe_env_wake", "ceesafe_env_slot"}
# Darwin's LLVM lowers a zeroing memset to bzero whatever the flags; the CosmoOS triples keep memset.
HOST_ONLY = {"bzero"} if sys.platform == "darwin" else set()
FORBIDDEN = (".ctors", ".dtors", ".init_array", ".fini_array", ".preinit_array", ".tdata", ".tbss", ".eh_frame",
             ".eh_frame_hdr", ".gcc_except_table")
TRIPLES = ("x86_64-unknown-none-elf", "aarch64-unknown-none-elf")
EXTERN_BLOCK = re.compile(r'extern\s+"C"\s*\{(.*?)\n\}', re.S)
FN = re.compile(r"\bfn\s+([A-Za-z_][A-Za-z0-9_]*)")
STATIC_ITEM = re.compile(r"^static\s", re.M)


def run(argv, **kw):
    return subprocess.run(argv, capture_output=True, text=True, **kw)


def externs_of(text):
    names = set()
    for block in EXTERN_BLOCK.findall(text):
        names.update(FN.findall(block))
    return names


def undefined(nm, objects):
    undefined, defined = set(), set()
    for obj in objects:
        for line in run([nm, obj]).stdout.splitlines():
            parts = line.split()
            if len(parts) >= 2 and parts[-2] == "U":
                undefined.add(parts[-1].lstrip("_") if sys.platform == "darwin" else parts[-1])
            elif len(parts) >= 3:
                defined.add(parts[-1].lstrip("_") if sys.platform == "darwin" else parts[-1])
    return undefined - defined


def sections(objdump, obj):
    result = []
    for line in run([objdump, "-h", obj]).stdout.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[0].isdigit():
            result.append((parts[1], int(parts[2], 16)))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--corpus", default=os.path.expanduser("~/Nova/tests/corpus"))
    parser.add_argument("--nm", default=shutil.which("llvm-nm") or "nm")
    parser.add_argument("--objdump", default=shutil.which("llvm-objdump") or "objdump")
    parser.add_argument("--lld", default=shutil.which("ld.lld"))
    args = parser.parse_args()
    compiler = os.path.abspath(os.path.expanduser(args.compiler))
    d = dialect(compiler)
    directory = os.path.join(args.corpus, "freestanding")
    files = sorted(f for f in os.listdir(directory) if f.endswith((".nova", ".cee")))
    if shutil.which("clang") is None:
        print("freestanding_check: no clang here (the targets are clang's): not run")
        return 2
    resource = run(["clang", "-print-resource-dir"]).stdout.strip()
    cross = ["-nostdinc", "-isystem", os.path.join(resource, "include"), "-fno-pic", "-fno-pie", "-O2", "-Werror",
             "-mgeneral-regs-only"]
    problems = []
    for name in files:
        relative = "freestanding/" + name
        text = read(os.path.join(directory, name))
        allowed = {d.c(n) for n in ALLOWLIST} | ENVIRONMENT | externs_of(text)
        with tempfile.TemporaryDirectory() as work:
            obj = os.path.join(work, "package.o")
            built = run([compiler, "build", "--profile", "freestanding", "--emit", "obj", "--cc", "clang",
                         "--cflag", "-O2", "--cflag", "-Werror", "-o", obj, relative], cwd=args.corpus)
            if built.returncode != 0:
                problems.append(f"{relative} (host): {built.stderr.strip()[:500]}")
                continue
            extra = undefined(args.nm, [obj]) - allowed - HOST_ONLY
            if extra:
                problems.append(f"{relative} (host) leaves undefined: {', '.join(sorted(extra))}")
            harness = os.path.join(directory, os.path.splitext(name)[0] + ".c")
            exe = os.path.join(work, "environment")
            host_c = os.path.join(work, "host-c")  # the harness includes the generated rt.h
            run([compiler, "build", "--profile", "freestanding", "--emit", "c", "-o", host_c, relative], cwd=args.corpus)
            linked = run(["clang", "-std=c11", "-Wall", "-Werror", "-I", host_c, harness, obj, "-o", exe, "-lpthread"])
            if linked.returncode != 0:
                problems.append(f"{relative} (harness): {linked.stderr.strip()[:500]}")
                continue
            ran = run([exe], timeout=30)
            stdout, status, panic, panic_line = expectations(text)
            if ran.stdout != stdout or ran.returncode != status:
                problems.append(f"{relative}: stdout {ran.stdout!r} status {ran.returncode}, expected {stdout!r} {status}")
            if panic is not None and not ran.stderr.startswith(f"{d.panic}{panic} at {relative}:{panic_line}:"):
                problems.append(f"{relative}: panic {ran.stderr.strip()!r}")
            for triple in TRIPLES:
                flags = [f"--target={triple}"] + cross
                objects = []
                c_dir = os.path.join(work, triple)
                emitted = run([compiler, "build", "--profile", "freestanding", "--emit", "c", "-o", c_dir, relative],
                              cwd=args.corpus)
                if emitted.returncode != 0:
                    problems.append(f"{relative} ({triple}): {emitted.stderr.strip()[:500]}")
                    continue
                warnings = ["-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Wshadow", "-Wconversion",
                            "-Wsign-conversion", "-Wmissing-prototypes", "-Wstrict-prototypes", "-Wvla",
                            "-Wcast-qual", "-Wundef", "-Wimplicit-fallthrough"]
                profile = ["-ffreestanding", "-fno-stack-protector", "-fno-asynchronous-unwind-tables",
                           "-fno-unwind-tables"]
                for source in sorted(f for f in os.listdir(c_dir) if f.endswith(".c")):
                    o = os.path.join(c_dir, source + ".o")
                    compiled = run(["clang"] + warnings + profile + flags + ["-I", c_dir, "-c",
                                                                          os.path.join(c_dir, source), "-o", o])
                    if compiled.returncode != 0:
                        problems.append(f"{relative} ({triple}) {source}: {compiled.stderr.strip()[:500]}")
                        continue
                    objects.append(o)
                extra = undefined(args.nm, objects) - allowed
                if extra:
                    problems.append(f"{relative} ({triple}) leaves undefined: {', '.join(sorted(extra))}")
                for o in objects:
                    found = sections(args.objdump, o)
                    if not any(n == ".text" or n.startswith(".text.") for n, _ in found):
                        problems.append(f"{relative} ({triple}) {os.path.basename(o)} has no .text")
                    bad = [n for n, _ in found if any(n == f or n.startswith(f + ".") for f in FORBIDDEN)]
                    state = [n for n, size in found if size > 0 and (n in (".data", ".bss") or
                                                                    n.startswith(".data.") or n.startswith(".bss."))]
                    if state and os.path.basename(o) == "main.c.o" and STATIC_ITEM.search(text):
                        state = []  # the program's own static items (profiles.md §4.4); the runtime may have none
                    if bad or state:
                        problems.append(f"{relative} ({triple}) {os.path.basename(o)}: {bad + state}")
                    for line in run([args.nm, o]).stdout.splitlines():
                        if len(line.split()) >= 2 and line.split()[-2] == "C":
                            problems.append(f"{relative} ({triple}) COMMON symbol: {line}")
                if args.lld:
                    joined = os.path.join(work, triple + ".o")
                    built = run([compiler, "build", "--profile", "freestanding", "--emit", "obj", "--cc", "clang",
                                 "--ld", args.lld] + [x for f in flags for x in ("--cflag", f)] + ["-o", joined, relative],
                                cwd=args.corpus)
                    if built.returncode != 0:
                        problems.append(f"{relative} ({triple} joined): {built.stderr.strip()[:500]}")
                    else:
                        extra = undefined(args.nm, [joined]) - allowed
                        if extra:
                            problems.append(f"{relative} ({triple} joined) leaves undefined: {', '.join(sorted(extra))}")
    for p in problems:
        print("FAIL", p)
    print(f"freestanding_check: {len(files)} programs, host and {len(TRIPLES)} triples"
          f"{' (with ld.lld)' if args.lld else ''}: {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
