#!/usr/bin/env python3
"""The M3 fault-injection sweep (the reference's FaultInjectionTests, docs/milestone-3.md criterion 1).

Each run-pass program that allocates (its C calls nova_alloc, nova_region_alloc, nova_scratch_enter_root,
nova_pool_take, nova_vec_grow or nova_svec_slot) is built with ASan+UBSan and run once with NOVA_ALLOC_REPORT=1 to
count its allocation attempts n; then once per failure point k (NOVA_FAIL_ALLOC_AT=k): every k <= n, or for n > 200
the first 50, the last 50 and 100 evenly spaced. Every run must end with a normal return and no live allocations, or
with the panic of `!` on the failed allocation (exit 134); a leak, any other panic or a sanitizer report fails.

With --reference, the reference compiler's build of each program must give the same stdout, stderr and status at
every point. Both compilers build in novac's default form (P7), or in P5's with --flat. The interpreter's agreement (the reference's other half of the test) is P8's.

usage: python3 tools/fault_injection.py --compiler out/ceesafec [--reference ~/.nova/0.6.0/bin/novac]
           [--corpus DIR] [--filter TEXT] [--jobs N]
"""
import argparse
import concurrent.futures
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from names import dialect, for_compiler, map_back  # noqa: E402

ALLOCATION_CALLS = ("nova_alloc(", "nova_alloc_zeroed(", "nova_region_alloc(", "nova_region_alloc_fast(",
                    "nova_scratch_enter_root(", "nova_pool_take(", "nova_vec_grow(", "nova_svec_slot(",  # Decision 0007
                    "nova_async_spawn(")  # a scope's child (type-system.md §40.5): its task and frame
P5_SWITCHES = ["--flat-c", "--no-closure-specialisation", "--no-placement", "--no-bce", "--no-oce"]
SWITCHES = []  # the default form; --flat sets P5_SWITCHES
# The sanitizers are clang's, or GCC's where there is no clang (the Linux box: ROADMAP L1 A2's gate).
SANITIZER_CC = "clang" if shutil.which("clang") else "gcc"
SANITIZERS = ["-O1", "-Werror", "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
def report_of(d):
    """The allocation report line of a program built by a compiler of dialect `d`."""
    return re.compile(re.escape(d.alloc) + r"attempts=(\d+) live=(\d+)")
MAX_ATTEMPTS = 200


def failure_points(attempts):
    if attempts <= MAX_ATTEMPTS:
        return list(range(1, attempts + 1))
    points = list(range(1, 51)) + [attempts - k for k in range(50)] + [k * attempts // 101 for k in range(1, 101)]
    seen, result = set(), []
    for k in points:
        if k not in seen:
            seen.add(k)
            result.append(k)
    return result


def run(exe, fail_at, d):
    """The program's status, stdout and stderr; a CeeSafe-built program's stderr with Nova's names, for comparing."""
    env = dict(os.environ, **{d.alloc_report: "1", d.fail_alloc_at: str(fail_at)})
    p = subprocess.run([exe], capture_output=True, env=env, timeout=30)
    status = 128 - p.returncode if p.returncode < 0 else p.returncode  # a signal, as .NET reports it (SIGABRT: 134)
    return status, p.stdout, map_back(p.stderr.decode("utf-8", "replace")) if d.cee else p.stderr.decode("utf-8", "replace")


def problem(outcome):
    status, _, stderr = outcome
    if "Sanitizer" in stderr or "runtime error" in stderr:
        return "sanitizer report"
    if "nova panic: " in stderr:  # run() gives Nova's names
        return None if "nova panic: unwrapped an Err value" in stderr and status == 134 else "unexpected panic"
    m = report_of(dialect("novac")).search(stderr)
    if not m:
        return f"no allocation report (exit code {status})"
    return None if m.group(2) == "0" else f"{m.group(2)} allocation(s) leaked"


def build(compiler, corpus, path, out, reference):
    switches = SWITCHES
    c_dir = out + "-c"
    d = dialect(compiler)
    cwd, argv = for_compiler(compiler, corpus, ["build", "--emit", "c", "-o", c_dir] + switches + [path])
    emitted = subprocess.run([compiler] + argv, cwd=cwd, capture_output=True)
    if emitted.returncode != 0:
        return None, "C emission failed: " + emitted.stderr.decode("utf-8", "replace")[:500]
    source = ""
    for name in os.listdir(c_dir):
        if name.endswith(".c") and not name.startswith(d.runtime_prefix):
            with open(os.path.join(c_dir, name), encoding="utf-8") as f:
                source += f.read()
    if not any(d.c(call) in source for call in ALLOCATION_CALLS):
        return False, None  # the program never allocates
    flags = [x for f in SANITIZERS for x in ("--cflag", f)]
    cwd, argv = for_compiler(compiler, corpus, ["build", "--cc", SANITIZER_CC] + flags + switches + [path, "-o", out])
    built = subprocess.run([compiler] + argv, cwd=cwd, capture_output=True)
    if built.returncode != 0:
        return None, "build failed: " + built.stderr.decode("utf-8", "replace")[:500]
    return True, None


def sweep(arguments):
    compiler, reference, corpus, path = arguments
    with tempfile.TemporaryDirectory() as work:
        exe = os.path.join(work, "program")
        allocates, error = build(compiler, corpus, path, exe, False)
        if allocates is None:
            return path, 0, [error]
        if not allocates:
            return path, None, []
        reference_exe = None
        if reference:
            reference_exe = os.path.join(work, "reference")
            ok, error = build(reference, corpus, path, reference_exe, True)
            if not ok:
                return path, 0, [f"reference: {error}"]
        d, rd = dialect(compiler), dialect(reference or "novac")
        baseline = run(exe, 0, d)
        m = report_of(dialect("novac")).search(baseline[2])
        if not m:
            return path, 0, [f"baseline: no allocation report; exit code {baseline[0]}\n{baseline[2][:500]}"]
        attempts, live = int(m.group(1)), int(m.group(2))
        failures = []
        if live != 0:
            failures.append(f"{live} allocation(s) leaked without fault injection")
        if reference_exe and run(reference_exe, 0, rd) != baseline:
            failures.append("baseline: the reference's run differs")
        for k in failure_points(attempts) if attempts > 0 else []:
            outcome = run(exe, k, d)
            p = problem(outcome)
            if p:
                failures.append(f"allocation {k} of {attempts} failing: {p}\nstderr:\n{outcome[2][:500]}")
            if reference_exe and run(reference_exe, k, rd) != outcome:
                failures.append(f"allocation {k} of {attempts} failing: the reference's run differs")
        return path, attempts, failures


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--reference")
    parser.add_argument("--corpus", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests",
                                                               "reference", "tests", "corpus"))
    parser.add_argument("--filter")
    parser.add_argument("--jobs", type=int, default=os.cpu_count())
    parser.add_argument("--flat", action="store_true", help="build in P5's form (everything off)")
    args = parser.parse_args()
    if args.flat:
        SWITCHES[:] = P5_SWITCHES
    compiler = os.path.abspath(os.path.expanduser(args.compiler))
    reference = os.path.abspath(os.path.expanduser(args.reference)) if args.reference else None
    directory = os.path.join(args.corpus, "run-pass")
    paths = ["run-pass/" + f for f in sorted(os.listdir(directory)) if f.endswith((".nova", ".cee"))]
    if args.filter:
        paths = [p for p in paths if args.filter in p]
    work = [(compiler, reference, args.corpus, p) for p in paths]
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        results = list(pool.map(sweep, work))
    swept = [(p, n, f) for p, n, f in results if n is not None]
    failed = [(p, f) for p, _, f in swept if f]
    points = sum(len(failure_points(n)) for _, n, _ in swept if n)
    for p, f in failed:
        print(f"FAIL {p}:\n  " + "\n  ".join(f[:5]))
    print(f"fault_injection: {len(results)} run-pass programs, {len(swept)} allocate, {points} failure points"
          f"{' (each also run as built by the reference)' if reference else ''}; {len(failed)} fail")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
