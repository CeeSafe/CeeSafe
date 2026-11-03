#!/usr/bin/env python3
"""CeeSafe's allocator for C code (ADR 0060, CEE_ALLOC_FOREIGN), on a package written here whose C file includes
ceesafe/foreign.h and keeps one block until the process exits, as a C library's global state does. Once with the C
file compiled with the program, and once compiled apart (`include-dirs`, ADR 0047 §3.3):

1. with CEESAFE_ALLOC_REPORT=1 the program prints its results, the report has live=0 (C's block is not a leak) and a
   second line `ceesafe alloc: foreign held=1`;
2. with CEESAFE_FAIL_ALLOC_AT=n for every attempt n of that run: each run exits 0 with live=0, and the failed attempt
   reaches C's allocations too: some run sees C's first allocation fail (hold=0) and some its realloc (cycle=0);
3. `ceesafec test`: the package's test, which keeps a C block, passes rather than leaking;
4. CEE_ALLOC_FOREIGN_INIT (ADR 0061), with CW_INIT=1 the program first runs C's one-time initialisation: two blocks
   under that number, one grown with realloc and kept, the other freed under CEE_ALLOC_FOREIGN. It adds no attempt
   (`attempts` equals the run without it), its kept block is held (foreign held=2), and at every point of the sweep it
   still succeeds (init=1).

usage: python3 tools/foreign_alloc_check.py --compiler out/ceesafec
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

SHIM = r"""/* Allocates through CeeSafe's allocator for C (ADR 0060). */
#include "ceesafe/foreign.h"

int32_t shim_hold(uint64_t n);
int32_t shim_cycle(void);
int32_t shim_init(void);

static void *held; /* kept until exit, as a library's global state */

int32_t shim_hold(uint64_t n) {
    held = ceesafe_alloc((size_t)n, CEE_ALLOC_FOREIGN);
    return held != NULL ? 1 : 0;
}

static void *state; /* the initialisation's, kept until exit */

/* A library's one-time initialisation (ADR 0061): never failed, so it never returns 0 under the sweep. */
int32_t shim_init(void) {
    void *scratch = ceesafe_alloc(64u, CEE_ALLOC_FOREIGN_INIT);
    state = ceesafe_alloc(16u, CEE_ALLOC_FOREIGN_INIT);
    if (scratch == NULL || state == NULL) return 0;
    void *grown = ceesafe_realloc(state, 256u, CEE_ALLOC_FOREIGN_INIT);
    if (grown == NULL) return 0;
    state = grown;
    ceesafe_free(scratch, CEE_ALLOC_FOREIGN); /* the other foreign number frees it */
    return 1;
}

/* realloc(NULL, …) allocates, a failed realloc keeps the block, free(NULL) does nothing. */
int32_t shim_cycle(void) {
    void *p = ceesafe_realloc(NULL, 16u, CEE_ALLOC_FOREIGN);
    if (p == NULL) return 0;
    void *q = ceesafe_realloc(p, 4096u, CEE_ALLOC_FOREIGN);
    if (q == NULL) {
        ceesafe_free(p, CEE_ALLOC_FOREIGN);
        return 0;
    }
    ceesafe_free(q, CEE_ALLOC_FOREIGN);
    ceesafe_free(NULL, CEE_ALLOC_FOREIGN);
    return 1;
}
"""

MAIN = """extern "C" {
    fn shim_hold(n: u64) -> i32;
    fn shim_cycle() -> i32;
    fn shim_init() -> i32;
}

/// Safety: the shim's functions take and keep nothing of the caller's.
fn main() {
    match (Environment.variable("CW_INIT")) {
        Some(_) => {
            Console.write("init=");
            Console.write(unsafe { shim_init() });
            Console.write(" ");
        }
        null => {}
    }
    var v = Vector<u8>.create();
    let pushed = match (v.push(1)) {
        Ok() => 1,
        Err(_) => 0,
    };
    let hold = unsafe { shim_hold(32) };
    let cycle = unsafe { shim_cycle() };
    var w = Vector<u8>.create();
    let after = match (w.push(2)) {
        Ok() => 1,
        Err(_) => 0,
    };
    Console.write("hold=");
    Console.write(hold);
    Console.write(" cycle=");
    Console.write(cycle);
    Console.write(" vectors=");
    Console.writeLine(pushed + after);
}
"""

TEST = """/// Safety: as main's.
@test
fn keepsCState() -> Result<void, TestFailure> {
    Check.equalU64(1, truncate<u64>(unsafe { shim_hold(64) }))?;
    return Ok();
}
"""

REPORT = re.compile(r"ceesafe alloc: attempts=(\d+) live=(\d+)")
FOREIGN = re.compile(r"ceesafe alloc: foreign held=(\d+)")


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", newline="\n") as f:
        f.write(text)


def check(compiler, work, separate, problems):
    shape = "apart" if separate else "with the program"
    root = os.path.join(work, "separate" if separate else "together")
    manifest = '[package]\nname = "foreign"\nversion = "0.1.0"\nc = ["c/shim.c"]\n'
    if separate:
        manifest += 'include-dirs = ["c"]\n'
    write(os.path.join(root, "ceesafe.toml"), manifest)
    write(os.path.join(root, "c", "shim.c"), SHIM)
    write(os.path.join(root, "src", "main.cee"), MAIN)
    write(os.path.join(root, "tests", "held.cee"), TEST)
    exe = os.path.join(work, "foreign-" + ("separate" if separate else "together"))
    b = subprocess.run([compiler, "build", "-o", exe], cwd=root, capture_output=True, text=True)
    if b.returncode != 0:
        problems.append(f"{shape}: build failed: {(b.stdout + b.stderr).strip()[-600:]}")
        return
    if os.name == "nt" and not exe.endswith(".exe"):
        exe += ".exe"

    def run(fail_at, init=False):
        env = dict(os.environ, CEESAFE_ALLOC_REPORT="1")
        env.pop("CW_INIT", None)
        if init:
            env["CW_INIT"] = "1"
        if fail_at:
            env["CEESAFE_FAIL_ALLOC_AT"] = str(fail_at)
        return subprocess.run([exe], capture_output=True, text=True, timeout=60, env=env)

    p = run(0)
    report, foreign = REPORT.search(p.stderr), FOREIGN.search(p.stderr)
    if p.returncode != 0 or p.stdout.strip() != "hold=1 cycle=1 vectors=2":
        problems.append(f"{shape}: status {p.returncode}, printed {p.stdout.strip()!r}")
    if not report or report.group(2) != "0":
        problems.append(f"{shape}: the report {p.stderr.strip()!r} does not say live=0")
        return
    if not foreign or foreign.group(1) != "1":
        problems.append(f"{shape}: the report {p.stderr.strip()!r} does not say foreign held=1")
    attempts = int(report.group(1))
    seen = set()
    for n in range(1, attempts + 1):
        q = run(n)
        r = REPORT.search(q.stderr)
        if q.returncode != 0 or not r or r.group(2) != "0":
            problems.append(f"{shape}: CEESAFE_FAIL_ALLOC_AT={n}: status {q.returncode}, {q.stderr.strip()!r}")
        seen.add(q.stdout.strip())
    for want in ("hold=0", "cycle=0"):
        if not any(want in s for s in seen):
            problems.append(f"{shape}: no failed attempt reached C ({want}); outputs {sorted(seen)!r}")
    # CEE_ALLOC_FOREIGN_INIT: no attempt, held, never failed.
    p = run(0, init=True)
    report, foreign = REPORT.search(p.stderr), FOREIGN.search(p.stderr)
    if p.returncode != 0 or p.stdout.strip() != "init=1 hold=1 cycle=1 vectors=2":
        problems.append(f"{shape}: with the initialisation: status {p.returncode}, printed {p.stdout.strip()!r}")
    if not report or int(report.group(1)) != attempts or report.group(2) != "0":
        problems.append(f"{shape}: with the initialisation the report is {p.stderr.strip()!r}, not attempts={attempts} "
                        f"live=0")
    if not foreign or foreign.group(1) != "2":
        problems.append(f"{shape}: with the initialisation the report {p.stderr.strip()!r} does not say foreign held=2")
    for n in range(1, attempts + 1):
        q = run(n, init=True)
        if q.returncode != 0 or not q.stdout.startswith("init=1 "):
            problems.append(f"{shape}: CEESAFE_FAIL_ALLOC_AT={n} failed the initialisation: {q.stdout.strip()!r}")
    t = subprocess.run([compiler, "test"], cwd=root, capture_output=True, text=True, timeout=600)
    if t.returncode != 0:
        problems.append(f"{shape}: `ceesafec test` failed: {(t.stdout + t.stderr).strip()[-600:]}")
    return attempts


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    problems = []
    counts = []
    with tempfile.TemporaryDirectory(prefix="ceesafe-foreign-") as work:
        for separate in (False, True):
            counts.append(check(compiler, work, separate, problems))
    print(f"foreign_alloc_check: C's allocations counted, failed and not leaked, compiled with the program and apart "
          f"({counts} attempts swept), and an initialisation never failed; {len(problems)} problems")
    for problem in problems:
        print("  FAIL " + problem)
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
