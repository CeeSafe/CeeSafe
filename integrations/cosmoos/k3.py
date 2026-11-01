#!/usr/bin/env python3
"""
k3.py -- M14 K3's native check of lib/kernel (ADR 0025) against a CosmoOS checkout. Two programs, each compiled to C
with --kernel cosmoos, once in one file and once split (--split, ADR 0022), then each C file with CosmoOS's
MODULE_CFLAGS and the freestanding profile's flags, against CosmoOS's own headers, with CeeSafe's kernel runtime shim
(c4/ceesafe_rt_kernel.c):

  - k3/module.cee, what a loadable module may use (static and boxed locks, a kernel object): the joined object must
    meet C4's checks for a module (profiles.md §4.4), undefined symbols only kernel exports;
  - tests/reference/tests/corpus/check-pass/kernel-library.cee, the whole library, as code built into the kernel: the same checks,
    except that it may also call the kernel's own inline primitives (KERNEL_INTERNAL), which CosmoOS does not export.
  - k3/atomics.cee, every atomic operation of lib/core (ADR 0026), as a module: the same checks, so each operation
    is inline and calls no helper.

Each object's .data and .bss hold its statics (type-system.md §39), which §4.4 allows since ADR 0024.

Leaves CosmoOS's working tree unchanged (checked). Usage:
    python3 integrations/cosmoos/k3.py [--cosmoos ~/dev/CosmoOS] [--arch aarch64|x86_64|both]
"""

import argparse
import shlex
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import c1  # noqa: E402
import c4  # noqa: E402

PROGRAMS = [("module", "integrations/cosmoos/k3/module.cee", False),
            ("kernel image", "tests/reference/tests/corpus/check-pass/kernel-library.cee", True),
            # Every atomic operation (ADR 0026): inline, so it may call nothing the kernel does not export (no
            # libatomic, no outline-atomics helper).
            ("atomics", "integrations/cosmoos/k3/atomics.cee", False),
            # A static Mutex (`here`, MUTEX_INIT_AT) and a static ObjectCache (ADR 0027).
            ("statics", "integrations/cosmoos/k3/statics.cee", False)]
# What IrqGuard's arch_irq_save/restore inline to: defined in the kernel, not exported to modules.
KERNEL_INTERNAL = {"arch_irq_save_hw", "arch_irq_restore_hw"}


def check_arch(root, arch, split, name, program, built_in, checks):
    cosmo = c1.Cosmo(root, arch, c1.CEESAFE_ROOT / "out" / "cosmoos" / f"{arch}-release")
    work = c1.CEESAFE_ROOT / "out" / "cosmoos" / f"k3-{arch}-{name.replace(' ', '-')}{'-split' if split else ''}"
    gen = work / "gen"
    gen.mkdir(parents=True, exist_ok=True)
    built = subprocess.run([c1.CEESAFEC, "build", "--profile", "freestanding", "--kernel", "cosmoos", "--no-float", "--emit", "c",
                            *(["--split", "--program", "kl"] if split else []), "-o", gen, program],
                           cwd=c1.CEESAFE_ROOT, capture_output=True, text=True)
    label = f"{arch} {name}{' split' if split else ''}"
    if built.returncode != 0:
        checks[f"{label}: ceesafec builds the program"] = (False, built.stderr.splitlines()[:10])
        return
    cflags, ldflags = shlex.split(cosmo.var("MODULE_CFLAGS")), shlex.split(cosmo.var("MODULE_LDFLAGS"))
    objects, usage, errors = [], [], []
    for source in [*sorted(gen.glob("*.c")), c4.C4_DIR / "ceesafe_rt_kernel.c"]:
        obj = work / f"{source.stem}.o"
        # The freestanding profile's flags (profiles.md §4.3), as the build of a CeeSafe package passes them.
        compiled = c4.compile_c(cosmo.var("CC"), cflags, source, obj, gen, ["-fstack-usage", "-ffreestanding", "-fno-stack-protector"])
        # MUTEX_INIT_AT is CosmoOS's own (kernel/include/kernel/mutex.h, ADR 0027 §3.1).
        if compiled.returncode != 0:
            errors.append(compiled.stderr.strip())
        objects.append(obj)
        usage.append(obj.with_suffix(".su"))
    if errors:
        checks[f"{label}: compiles with CosmoOS's MODULE_CFLAGS"] = (False, [line for e in errors for line in e.splitlines()][:12])
        return
    joined = work / "kl.o"
    subprocess.run([cosmo.var("LD"), *ldflags, "-o", joined, *objects], check=True)
    problems, kinds, undefined = c4.all_problems(joined, arch, c4.kernel_exports(), cosmo.var("OBJDUMP"))
    state = [p for p in problems if "mutable global state" in p]
    internal = sorted(set(undefined) & KERNEL_INTERNAL)
    problems = [p for p in problems if "mutable global state" not in p
                and not (built_in and p.split(":")[0] in KERNEL_INTERNAL)]
    frames, largest = c4.frame_problems(usage, int(cosmo.var("KERNEL_FRAME_LIMIT")))
    checks[f"{label}: {len(objects)} C file(s) compile under MODULE_CFLAGS; the object meets C4's checks (relocations "
           f"{', '.join(kinds)}; undefined {', '.join(undefined)}; largest frame {largest[0]} {largest[1]} B)"] = (not problems + frames, problems + frames)
    checks[f"{label}: (note) the statics' state: {'; '.join(state) or 'none'}"] = (True, [])
    if internal:
        checks[f"{label}: (note) kernel-internal, for built-in code only: {', '.join(internal)}"] = (True, [])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cosmoos", default=str(Path.home() / "dev" / "CosmoOS"))
    parser.add_argument("--arch", choices=["x86_64", "aarch64", "both"], default="both")
    args = parser.parse_args()
    root = Path(args.cosmoos).expanduser().resolve()
    c4.COSMO = root
    status_before = c1.git_status(root)
    c1.require_compiler()
    checks = {}
    for arch in (["aarch64", "x86_64"] if args.arch == "both" else [args.arch]):
        for name, program, built_in in PROGRAMS:
            for split in (False, True):
                check_arch(root, arch, split, name, program, built_in, checks)
    checks["CosmoOS working tree unchanged"] = (c1.git_status(root) == status_before, [])
    for name, (ok, details) in checks.items():
        print(f"[k3] {'PASS' if ok else 'FAIL'}  {name}")
        for detail in details[:12]:
            print(f"[k3]        {detail}")
    return 0 if all(ok for ok, _ in checks.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
