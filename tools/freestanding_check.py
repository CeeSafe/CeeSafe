#!/usr/bin/env python3
"""The freestanding profile's checks (spec/profiles.md §4.4), run against a ceesafec.

For each freestanding corpus program:
- host: `build --profile freestanding --emit obj`; its undefined symbols are only the allowlist (ceesafe_panic, memcpy,
  memmove, memset, memcmp) and its own extern "C" functions; linked with the program's C harness it runs with the
  expected output, status and panic;
- a Windows (COFF) target: the runtime's freestanding form compiles, and `--emit obj` is refused in one line of
  ceesafec's own, since the linkers for that target have no relocatable link;
- each CosmoOS triple (x86_64-unknown-none-elf, aarch64-unknown-none-elf), cross-compiled with CosmoOS's flags: the
  same allowlist; no constructors, TLS or unwind tables, no mutable global state, no COMMON symbols; and with ld.lld,
  the one object `--emit obj --ld ld.lld` makes has the same allowlist.

And the embedded build (ADR 0088 §5), for demo.cee: `--emit lib` writes the archive with ceesafe/embed.h's
freestanding form beside it; the archive's undefined symbols are the allowlist's; a C embedder including that header
compiles with the profile's flags and warnings for each triple (with llvm-ar), and on the host links the archive,
whose ceesafe_init takes the config's size and refuses another.

usage: python3 tools/freestanding_check.py --compiler PATH [--corpus DIR] [--nm llvm-nm] [--objdump llvm-objdump]
           [--lld ld.lld] [--ar llvm-ar]
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

ALLOWLIST = {"ceesafe_panic", "memcpy", "memmove", "memset", "memcmp"}
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
    parser.add_argument("--corpus", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests",
                                                         "reference", "tests", "corpus"))
    parser.add_argument("--nm", default=shutil.which("llvm-nm") or "nm")
    parser.add_argument("--objdump", default=shutil.which("llvm-objdump") or "objdump")
    parser.add_argument("--lld", default=shutil.which("ld.lld"))
    parser.add_argument("--ar", default=shutil.which("llvm-ar"), help="the triples' archiver (an ELF one)")
    args = parser.parse_args()
    compiler = os.path.abspath(os.path.expanduser(args.compiler))
    directory = os.path.join(args.corpus, "freestanding")
    files = sorted(f for f in os.listdir(directory) if f.endswith(".cee"))
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
        allowed = ALLOWLIST | ENVIRONMENT | externs_of(text)
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
            if panic is not None and not ran.stderr.startswith(f"ceesafe panic: {panic} at {relative}:{panic_line}:"):
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
    # A Windows target has no relocatable link (profiles.md §4.3): refused by ceesafec, in one line of its own, before
    # the linker is asked. The runtime's freestanding form compiles for that target all the same (no <windows.h>).
    with tempfile.TemporaryDirectory() as refused:
        for name in files:
            built = subprocess.run([compiler, "build", "--profile", "freestanding", "--emit", "obj", "--cc", "clang",
                                    "--cflag", "--target=aarch64-pc-windows-msvc", os.path.join(directory, name),
                                    "-o", os.path.join(refused, "program.o")], capture_output=True, text=True)
            said = built.stderr.strip().splitlines()
            if built.returncode == 0 or len(said) != 1 or "relocatable link" not in said[0]:
                problems.append(f"freestanding/{name} (a Windows target): expected ceesafec's one-line refusal: "
                                f"{built.returncode} {built.stderr.strip()[:400]}")
    # On macOS Apple's clang links with Apple's ld, which has no ELF link: a cross build without `--ld` fails in the
    # linker, and ceesafec says after the linker's words that the target needs `--ld ld.lld` (profiles.md §4.3).
    if sys.platform == "darwin" and "Apple" in run(["clang", "--version"]).stdout:
        with tempfile.TemporaryDirectory() as cross_build:
            built = subprocess.run([compiler, "build", "--profile", "freestanding", "--emit", "obj", "--cc", "clang",
                                    "--cflag", "--target=aarch64-unknown-none-elf",
                                    os.path.join(directory, files[0]), "-o", os.path.join(cross_build, "program.o")],
                                   capture_output=True, text=True)
            said = built.stderr.strip().splitlines()
            if (built.returncode == 0 or not said or not said[-1].startswith("ceesafec: --emit obj joins")
                    or "'--ld ld.lld'" not in said[-1] or "aarch64-unknown-none-elf" not in said[-1]):
                problems.append(f"freestanding/{files[0]} (an ELF target on macOS, no --ld): expected ceesafec's "
                                f"advice last: {built.returncode} {built.stderr.strip()[-400:]}")
    problems += embedded(compiler, args, cross)
    for p in problems:
        print("FAIL", p)
    print(f"freestanding_check: {len(files)} programs, host and {len(TRIPLES)} triples"
          f"{' (with ld.lld)' if args.lld else ''}, and the embedded build: {len(problems)} problems")
    return 1 if problems else 0


EMBEDDER = r"""
#include "ceesafe/embed.h"
#if !defined(CEESAFE_EMBED_FREESTANDING)
#error "the freestanding form of ceesafe/embed.h"
#endif
int embedder(void);
int embedder(void) {
    ceesafe_config config = {(uint32_t)sizeof(ceesafe_config), 0u};
    ceesafe_config other = {1u, 0u};
    int32_t ok = ceesafe_init(&config);
    int32_t size = ceesafe_init(&other);
    return ok == 0 && size == CEESAFE_INIT_SIZE && ceesafe_shutdown() == 0u ? 0 : 1;
}
"""

HOST_MAIN = r"""
#include <stdio.h>
#include <stdlib.h>
#include "ceesafe/rt.h"
int embedder(void);
_Noreturn void ceesafe_panic(cee_panic_kind kind, cee_loc loc) {
    (void)kind;
    (void)loc;
    abort();
}
int main(void) {
    int r = embedder();
    printf("embedder=%d\n", r);
    return r;
}
"""


def embedded(compiler, args, cross):
    """ADR 0088 §5: the freestanding profile's embedded build, demo.cee as `--emit lib`."""
    problems = []
    relative = "freestanding/demo.cee"
    allowed = ALLOWLIST | ENVIRONMENT | externs_of(read(os.path.join(args.corpus, relative)))
    warnings = ["-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Wmissing-prototypes", "-Wstrict-prototypes",
                "-Werror"]
    profile = ["-ffreestanding", "-fno-stack-protector"]
    with tempfile.TemporaryDirectory() as work:
        embedder = os.path.join(work, "embedder.c")
        with open(embedder, "w", encoding="utf-8") as handle:
            handle.write(EMBEDDER)
        # The host: the archive linked by an environment's stand-in, which runs ceesafe_init.
        archive = os.path.join(work, "host", "libdemo.a")
        built = run([compiler, "build", "--profile", "freestanding", "--emit", "lib", "--cc", "clang", "--cflag",
                     "-O2", "-o", archive, relative], cwd=args.corpus)
        header = os.path.join(work, "host", "ceesafe", "embed.h")
        if built.returncode != 0 or not os.path.isfile(header):
            problems.append(f"{relative} (embedded, host): {built.stderr.strip()[:500]}")
        else:
            extra = undefined(args.nm, [archive]) - allowed - HOST_ONLY
            if extra:
                problems.append(f"{relative} (embedded, host) leaves undefined: {', '.join(sorted(extra))}")
            host_c = os.path.join(work, "host-c")  # rt.h, for the stand-in's ceesafe_panic
            run([compiler, "build", "--profile", "freestanding", "--emit", "c", "-o", host_c, relative],
                cwd=args.corpus)
            main_c = os.path.join(work, "main.c")
            with open(main_c, "w", encoding="utf-8") as handle:
                handle.write(HOST_MAIN)
            exe = os.path.join(work, "embedded")
            linked = run(["clang", *warnings, "-I", os.path.join(work, "host"), "-I", host_c, embedder, main_c,
                          archive, "-o", exe])
            ran = run([exe], timeout=30) if linked.returncode == 0 else linked
            if ran.returncode != 0 or ran.stdout != "embedder=0\n":
                problems.append(f"{relative} (embedded, host): {ran.stdout.strip()} {ran.stderr.strip()[:500]}")
        if args.ar is None:
            print("freestanding_check: the embedded build's triples skipped: no llvm-ar (--ar)")
            return problems
        for triple in TRIPLES:
            flags = [f"--target={triple}"] + cross
            directory = os.path.join(work, triple)
            archive = os.path.join(directory, "libdemo.a")
            built = run([compiler, "build", "--profile", "freestanding", "--emit", "lib", "--cc", "clang", "--ar",
                         args.ar] + [x for f in flags for x in ("--cflag", f)] + ["-o", archive, relative],
                        cwd=args.corpus)
            if built.returncode != 0:
                problems.append(f"{relative} (embedded, {triple}): {built.stderr.strip()[:500]}")
                continue
            extra = undefined(args.nm, [archive]) - allowed
            if extra:
                problems.append(f"{relative} (embedded, {triple}) leaves undefined: {', '.join(sorted(extra))}")
            compiled = run(["clang", *warnings, *profile, *flags, "-I", directory, "-c", embedder, "-o",
                            os.path.join(directory, "embedder.o")])
            if compiled.returncode != 0:
                problems.append(f"{relative} (embedded, {triple}) embed.h: {compiled.stderr.strip()[:500]}")
    return problems


if __name__ == "__main__":
    sys.exit(main())
