#!/usr/bin/env python3
"""
c7.py -- CeeSafe milestone C7: kernel concurrency (docs/cosmoos/c7-report.md). This script covers C7c and C7d, the
bindings: c7/sync.cee (SpinLock<T>, IrqSpinLock<T>, Mutex<T> over CosmoOS's own spinlock_t and struct mutex),
c7/kobject.cee (KRef<T> over its struct kobject) and c7/quiesce.cee (grace periods, deferred frees).
CosmoOS is only read.

Steps (no QEMU):
  1. negative cases: each c7/negative/*.cee, compiled with the bindings, must give exactly the diagnostics its
     `// error[N....]` annotations name (sleeping under a spin guard, allocating in an IRQ handler, reaching the
     data without a guard, a guard outliving its data's use);
  2. host test: the bindings and c7/demo.cee, compiled with CosmoOS's HOST_CFLAGS (ASan, UBSan, -Werror) against
     its own headers, linked with its HOST_COMMON_SRCS (the harness and its host spinlock shim,
     tests/host/shim_spinlock.c) and c7/host_kernel.c, run c7/host_test.c;
  3. kernel flags: the same package compiled with CosmoOS's MODULE_CFLAGS, with c4/ceesafe_rt_kernel.c and
     c7/kernel_glue.c, joined and checked as C4 does (loader ELF rules, relocations, exports, frames, FP registers);
  4. C7e, in the kernel (skipped with --no-boot): CosmoOS's debug build (BUILD=debug: lockdep and might_sleep's
     debug half on) and the module ceesafe_c7 (c7/stress.cee, c7/module.c), built with that build's MODULE_CFLAGS,
     signed and checked like C5's, added to a copy of the boot archive, booted in QEMU (4 CPUs). The stress boot's
     checks must all pass, with no lockdep report and no boot module failing; a second boot, with the module built
     with -DC7_CONTROL=1, takes two spinlocks in both orders, and lockdep must report it.

Usage:
    python3 integrations/cosmoos/c7.py [--arch aarch64|x86_64] [--cosmoos ~/dev/CosmoOS] [--no-boot] [--timeout 900]
"""

import argparse
import re
import shlex
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import c1  # noqa: E402
import c4  # noqa: E402

C7_DIR = Path(__file__).resolve().parent / "c7"
PACKAGE = ["integrations/cosmoos/c5/kernel.cee", "integrations/cosmoos/c7/sync.cee", "integrations/cosmoos/c7/kobject.cee",
           "integrations/cosmoos/c7/quiesce.cee"]
MODULE_PACKAGE = [*PACKAGE, "integrations/cosmoos/c7/demo.cee", "integrations/cosmoos/c7/stress.cee"]
STRESS_CHECKS = ["threads", "spinlock-counter", "irqsave-counter", "mutex-table", "nested-spinlock", "kobject-count",
                 "worker-checks", "deferred-frees", "workers-overlapped", "cpus-used", "control-unlocked-counter-loses-updates"]
ANNOTATION = re.compile(r"// error\[(N\d{4})\]")
DIAGNOSTIC = re.compile(r"^(?P<file>[^:\n]+):(?P<line>\d+):\d+: error\[(?P<code>N\d{4})\]", re.MULTILINE)


def run(cmd, **kwargs):
    return subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kwargs)


def negative_cases(checks):
    for case in sorted((C7_DIR / "negative").glob("*.cee")):
        relative = case.relative_to(c1.CEESAFE_ROOT)
        expected = {(n, m.group(1)) for n, text in enumerate(case.read_text().splitlines(), 1) for m in ANNOTATION.finditer(text)}
        result = run([c1.CEESAFEC, "check", "--profile", "freestanding", "--no-float", *PACKAGE, relative], cwd=c1.CEESAFE_ROOT)
        found = {(int(m.group("line")), m.group("code")) for m in DIAGNOSTIC.finditer(result.stdout + result.stderr)
                 if m.group("file") == str(relative)}
        others = [m.group(0) for m in DIAGNOSTIC.finditer(result.stdout + result.stderr) if m.group("file") != str(relative)]
        ok = found == expected and not others and result.returncode != 0
        details = [] if ok else [f"expected {sorted(expected)}, found {sorted(found)}", *others[:4]]
        checks[f"negative {case.name}: {', '.join(code for _, code in sorted(expected))}"] = (ok, details)


def build_module(cosmo, work, gen, control):
    """ceesafe_c7.ko for `cosmo`'s build, as build/module.mk builds a module; the stress, or with `control` the inversion."""
    work.mkdir(parents=True, exist_ok=True)
    cflags, ldflags = shlex.split(cosmo.var("MODULE_CFLAGS")), shlex.split(cosmo.var("MODULE_LDFLAGS"))
    objects, usage, errors = [], [], []
    sources = [gen / "main.c", gen / "ceesafe_rt_core.c", c4.C4_DIR / "ceesafe_rt_kernel.c", C7_DIR / "kernel_glue.c", C7_DIR / "module.c"]
    for source in sources:
        obj = work / f"{source.stem}.o"
        extra = ["-fstack-usage"] + (["-DC7_CONTROL=1"] if control and source.name == "module.c" else [])
        compiled = c4.compile_c(cosmo.var("CC"), cflags, source, obj, gen, extra)
        if compiled.returncode != 0:
            errors.append(compiled.stderr.strip())
        objects.append(obj)
        usage.append(obj.with_suffix(".su"))
    if errors:
        return None, [line for e in errors for line in e.splitlines()][:10]
    unsigned, module = work / "ceesafe_c7.ko.unsigned", work / "ceesafe_c7.ko"
    c1.run([cosmo.var("LD"), *ldflags, "-o", unsigned, *objects])
    c1.run([sys.executable, cosmo.root / "scripts" / "modsign.py", "sign", "--key", cosmo.var("MODSIGN_KEY"), "--in", unsigned, "--out", module])
    elf_check = run([sys.executable, cosmo.root / "scripts" / "check-module-elf.py", module, cosmo.arch])
    problems, kinds, undefined = c4.all_problems(unsigned, cosmo.arch, c4.kernel_exports(), cosmo.var("OBJDUMP"))
    problems = [p for p in problems if "mutable global state" not in p]  # module.c's and kernel_glue.c's counters
    frames, largest = c4.frame_problems(usage, int(cosmo.var("KERNEL_FRAME_LIMIT")))
    problems += frames + ([] if elf_check.returncode == 0 else elf_check.stderr.splitlines())
    return module, problems


def boot_log(cosmo, work, module, timeout):
    image = c1.make_image(cosmo, work, [], extra=[("modules/ceesafe_c7.ko", module)])
    try:
        serial, status = c1.boot(cosmo, work, image, [], timeout)
    except SystemExit:  # no shell prompt: the kernel stopped (the control's lockdep panic) or hung
        serial, status = (work / "serial.log").read_bytes().decode("utf-8", "replace"), None
    return [line.strip() for line in serial.replace("\r", "\n").split("\n")], status


def kernel_boots(root, arch, timeout, checks):
    cosmo = c1.Cosmo(root, arch, c1.CEESAFE_ROOT / "out" / "cosmoos" / f"{arch}-debug", build="debug")
    cosmo.make("image")
    work = c1.CEESAFE_ROOT / "out" / "cosmoos" / f"c7-boot-{arch}"
    gen = work / "gen"
    gen.mkdir(parents=True, exist_ok=True)
    c1.run([c1.CEESAFEC, "build", "--profile", "freestanding", "--no-float", "--emit", "c", "-o", gen, *MODULE_PACKAGE], cwd=c1.CEESAFE_ROOT)

    module, problems = build_module(cosmo, work / "stress", gen, control=False)
    checks[f"{arch} debug build: ceesafe_c7 built, signed, checked (check-module-elf.py, C4's checks)"] = (module is not None and not problems, problems)
    if module is not None:
        lines, status = boot_log(cosmo, work / "stress", module, timeout)
        results = {}
        for line in lines:
            match = re.search(r"ceesafe_c7: (\S+) (PASS|FAIL) \((-?\d+)\)", line)
            if match:
                results[match.group(1)] = (match.group(2) == "PASS", match.group(3))
        for name in STRESS_CHECKS:
            passed, detail = results.get(name, (False, "no result"))
            checks[f"{arch} in-kernel {name} ({detail})"] = (passed, [])
        checks[f"{arch} module init: all checks passed"] = (any("ceesafe_c7: all checks passed" in l for l in lines), [])
        # A report is a bare "lockdep: <kind>" line and a panic (kernel/core/lockdep.c:313-328); "[DEBUG] lockdep: ..."
        # and "[ INFO] lockdep: ..." lines are its statistics.
        reports = [l for l in lines if l.startswith("lockdep:") or "panic" in l.lower()]
        checks[f"{arch} no lockdep report, no panic"] = (not reports, reports[:6])
        notes = [l for l in lines if "lockdep: class table" in l]
        if notes:
            checks[f"{arch} (note) {notes[-1]}"] = (True, [])
        loaded = next((l for l in lines if re.search(r"module: \d+ boot module\(s\) loaded", l)), "")
        checks[f"{arch} boot modules: {loaded.split('module: ')[-1] if loaded else 'no line'}"] = (loaded.endswith(", 0 failed"), [])
        checks[f"{arch} CosmoOS shut down cleanly (QEMU exit status {status})"] = (status == 33, [])

    module, problems = build_module(cosmo, work / "control", gen, control=True)
    if module is None:
        checks[f"{arch} control module builds"] = (False, problems)
        return
    lines, status = boot_log(cosmo, work / "control", module, min(timeout, 300))
    report = [l for l in lines if "lockdep" in l or "ceesafe.c7.order" in l]
    found = any("lockdep:" in l for l in lines) and any("ceesafe.c7.order" in l for l in lines)
    checks[f"{arch} negative control: lockdep reports the order inversion of ceesafe.c7.order.a/b"] = (found, report[:8] if not found else report[:3])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cosmoos", default=str(Path.home() / "dev" / "CosmoOS"))
    parser.add_argument("--arch", choices=["x86_64", "aarch64"], default="aarch64", help="for the kernel-flag checks and the boots")
    parser.add_argument("--no-boot", action="store_true", help="skip C7e's boots in the debug build")
    parser.add_argument("--timeout", type=int, default=900)
    args = parser.parse_args()
    root = Path(args.cosmoos).expanduser().resolve()
    c4.COSMO = root
    status_before = c1.git_status(root)
    cosmo = c1.Cosmo(root, args.arch, c1.CEESAFE_ROOT / "out" / "cosmoos" / f"{args.arch}-release")
    work = c1.CEESAFE_ROOT / "out" / "cosmoos" / "c7"
    gen = work / "gen"
    gen.mkdir(parents=True, exist_ok=True)
    c1.require_compiler()
    checks = {}

    negative_cases(checks)

    c1.run([c1.CEESAFEC, "build", "--profile", "freestanding", "--no-float", "--emit", "c", "-o", gen, *PACKAGE,
            "integrations/cosmoos/c7/demo.cee"], cwd=c1.CEESAFE_ROOT)
    ceesafe_flags = ["-I", gen, "-ffreestanding", "-fno-stack-protector"]  # the freestanding profile's flags (§4.3)

    # Host: CosmoOS's host-test flags, headers, spinlock shim and harness.
    cc = cosmo.var("HOST_CC")
    host_cflags, host_ldflags = shlex.split(cosmo.var("HOST_CFLAGS")), shlex.split(cosmo.var("HOST_LDFLAGS"))
    # HOST_COMMON_SRCS: the harness, the spinlock shim, and what the harness needs (tests/host/host.mk).
    host_srcs = [gen / "main.c", gen / "ceesafe_rt_core.c", c4.C4_DIR / "ceesafe_rt_kernel.c", C7_DIR / "host_kernel.c",
                 C7_DIR / "host_test.c", *c1.object_sources(root, work),  # CosmoOS's own kobjects
                 *[root / s for s in shlex.split(cosmo.var("HOST_COMMON_SRCS"))]]
    exe = work / "host_test"
    built = run([cc, *host_cflags, *ceesafe_flags, "-I", root / "tests" / "host", *host_srcs, *host_ldflags, "-o", exe])
    if built.returncode != 0:
        checks["host test builds with CosmoOS's HOST_CFLAGS"] = (False, built.stderr.strip().splitlines()[:10])
    else:
        ran = run([exe], timeout=600, env={"ASAN_OPTIONS": f"detect_leaks={0 if sys.platform == 'darwin' else 1}", "PATH": "/usr/bin:/bin"})  # no LSan on macOS
        lines = (ran.stdout + ran.stderr).strip().splitlines()
        checks[f"host test c7/host_test.c: {lines[-1] if lines else 'no output'}"] = (ran.returncode == 0, lines[-10:] if ran.returncode else [])

    # Kernel: CosmoOS's module flags, and C4's checks on the joined object.
    cflags = shlex.split(cosmo.var("MODULE_CFLAGS"))
    ld, objdump, limit = cosmo.var("LD"), cosmo.var("OBJDUMP"), int(cosmo.var("KERNEL_FRAME_LIMIT"))
    objects, usage, errors = [], [], []
    for source in [gen / "main.c", gen / "ceesafe_rt_core.c", c4.C4_DIR / "ceesafe_rt_kernel.c", C7_DIR / "kernel_glue.c"]:
        obj = work / f"kernel-{source.stem}.o"
        compiled = c4.compile_c(cosmo.var("CC"), cflags, source, obj, gen, ["-fstack-usage"])
        if compiled.returncode != 0:
            errors.append(compiled.stderr.strip())
        objects.append(obj)
        usage.append(obj.with_suffix(".su"))
    if not errors:
        joined = work / "ceesafe_c7.o"
        subprocess.run([ld, *shlex.split(cosmo.var("MODULE_LDFLAGS")), "-o", joined, *objects], check=True)
        problems, kinds, undefined = c4.all_problems(joined, args.arch, c4.kernel_exports(), objdump)
        problems = [p for p in problems if "mutable global state" not in p]  # kernel_glue.c's counter is the test's (as in C5)
        frames, largest = c4.frame_problems(usage, limit)
        errors = problems + frames
        checks[f"{args.arch} object under MODULE_CFLAGS meets C4's checks (relocations {', '.join(kinds)}; undefined "
               f"{', '.join(undefined)}; largest frame {largest[0]} {largest[1]} B)"] = (not errors, errors)
    else:
        checks["compiles with CosmoOS's MODULE_CFLAGS"] = (False, [line for e in errors for line in e.splitlines()][:10])

    if not args.no_boot:
        kernel_boots(root, args.arch, args.timeout, checks)

    checks["CosmoOS working tree unchanged"] = (c1.git_status(root) == status_before, [])
    for name, (ok, details) in checks.items():
        print(f"[c7] {'PASS' if ok else 'FAIL'}  {name}")
        for detail in details[:10]:
            print(f"[c7]        {detail}")
    return 0 if all(ok for ok, _ in checks.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
