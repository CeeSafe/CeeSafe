#!/usr/bin/env python3
"""Native checks of task frame stacks (ADR 0056) that the corpus cannot make: they read the runtime's allocation report
and make an allocation fail. Each program is built with clang (GCC without clang) under ASan+UBSan.

1. Middleware three deep through `Box<dyn Handler>`, stacked: after one warm-up request, a thousand more requests
   allocate no chunk. The program counts allocation attempts around them with CEESAFE_ALLOC_REPORT's counter, read
   twice; the difference must be the requests' own allocations and nothing for frames, i.e. equal for 10 and for 1000
   requests (the expected values are those two counts' equality and live=0 at exit).
2. A recursion 20 000 deep (several chunks) returns its depth, with live=0 at exit (the executor's pool is given back).
3. With the chunk allocation made to fail (CEESAFE_FAIL_ALLOC_AT at the first chunk), the program panics with "async
   frame stack: out of memory" and no sanitizer report.

usage: python3 tools/frame_stack_check.py --compiler out/ceesafec [--no-sanitizers]
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fault_injection import SANITIZER_CC  # noqa: E402

ALLOC = re.compile(r"ceesafe alloc: attempts=(\d+) live=(\d+)")

MIDDLEWARE = """
interface Handler {
    async fn handle(borrow this, n: u32) -> u32;
}

struct Base {}

impl Handler for Base {
    async fn handle(borrow this, n: u32) -> u32 => n + 1;
}

struct Scaled {
    inner: Box<dyn Handler>;
    factor: u32;
}

impl Handler for Scaled {
    async fn handle(borrow this, n: u32) -> u32 => await this.inner.handle(n) * this.factor;
}

async fn serve(requests: u32) -> u32 {
    let base: Box<dyn Handler> = Box.create(Base {})!;
    let two: Box<dyn Handler> = Box.create(Scaled { inner: base, factor: 2 })!;
    let app = Scaled { inner: two, factor: 3 };
    var total: u32 = 0;
    for (k in 0..requests) {
        total = total.wrappingAdd(await app.handle(k));
    }
    return total;
}

fn main() {
    let requests = match (Environment.variable("REQUESTS")) {
        Some(_) => 1000u32,
        null => 10u32,
    };
    Console.writeLine(Async.run(serve(requests)));
}
"""

DEEP = """
async fn depth(n: u32) -> u64 {
    if (n == 0) {
        return 0;
    }
    return await depth(n - 1) + 1;
}

fn main() {
    Console.writeLine(Async.run(depth(20000)));
}
"""


def build(compiler, work, name, source, sanitizers):
    path = os.path.join(work, name + ".cee")
    open(path, "w").write(source)
    exe = os.path.join(work, name)
    flags = [] if not sanitizers else ["--cc", SANITIZER_CC, "--cflag", "-fsanitize=address,undefined", "--cflag",
                                       "-fno-sanitize-recover=all", "--cflag", "-O1"]
    b = subprocess.run([compiler, "build"] + flags + [path, "-o", exe], capture_output=True, text=True)
    if b.returncode != 0:
        return None, b.stderr.strip()[-400:]
    if os.name == "nt" and not exe.endswith(".exe"):
        exe += ".exe"
    return exe, None


def run(exe, extra):
    env = dict(os.environ, CEESAFE_ALLOC_REPORT="1", **extra)
    p = subprocess.run([exe], capture_output=True, text=True, env=env, timeout=120)
    m = ALLOC.search(p.stderr)
    return p, (int(m.group(1)), int(m.group(2))) if m else None


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--no-sanitizers", action="store_true",
                        help="build with the default C compiler, without ASan (a host without clang: Windows with cl)")
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    sanitizers = not args.no_sanitizers
    problems = []
    with tempfile.TemporaryDirectory(prefix="ceesafe-frame-stack-") as work:
        exe, error = build(compiler, work, "middleware", MIDDLEWARE, sanitizers)
        if exe is None:
            problems.append(f"middleware: build failed: {error}")
        else:
            few, few_counts = run(exe, {})
            many, many_counts = run(exe, {"REQUESTS": "1"})
            # sum over k of ((k + 1) * 2) * 3, wrapping at 2^32
            want_few = sum(((k + 1) * 6) for k in range(10)) % (1 << 32)
            want_many = sum(((k + 1) * 6) for k in range(1000)) % (1 << 32)
            if few.returncode != 0 or few.stdout.strip() != str(want_few) or few_counts is None:
                problems.append(f"middleware (10): status {few.returncode}, {few.stdout.strip()!r}, {few.stderr[-300:]!r}")
            elif many.returncode != 0 or many.stdout.strip() != str(want_many) or many_counts is None:
                problems.append(f"middleware (1000): status {many.returncode}, {many.stdout.strip()!r}")
            elif few_counts[0] != many_counts[0] or few_counts[1] != 0 or many_counts[1] != 0:
                problems.append(f"middleware: {few_counts} allocation attempts and live for 10 requests, "
                                f"{many_counts} for 1000: the frame stack allocated per request")
        exe, error = build(compiler, work, "deep", DEEP, sanitizers)
        if exe is None:
            problems.append(f"deep: build failed: {error}")
        else:
            p, counts = run(exe, {})
            if p.returncode != 0 or p.stdout.strip() != "20000" or counts is None or counts[1] != 0:
                problems.append(f"deep: status {p.returncode}, {p.stdout.strip()!r}, counts {counts}")
            else:
                # The first allocation attempt is the first chunk (the program allocates nothing else).
                failed, _ = run(exe, {"CEESAFE_FAIL_ALLOC_AT": "1"})
                if failed.returncode == 0 or "async frame stack: out of memory" not in failed.stderr or \
                        "ERROR: AddressSanitizer" in failed.stderr:
                    problems.append(f"deep with no chunk: status {failed.returncode}, {failed.stderr[-300:]!r}")
    print(f"frame_stack_check: stacked middleware, a deep recursion, a failed chunk; {len(problems)} problems")
    for problem in problems:
        print("  FAIL " + problem)
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
