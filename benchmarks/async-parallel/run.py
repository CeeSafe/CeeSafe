#!/usr/bin/env python3
"""The parallel fan-out benchmark (ADR 0038; spec/c-backend.md §6): fanout.cee's scope of CHILDREN spawnSend
children, CPU only, run with Async.runWorkers(n, …) for each n in --workers. It prints the median time of --runs runs
for each n and the speed-up over one worker, and checks that the sum does not depend on n.

    python3 benchmarks/async-parallel/run.py --ceesafec out/ceesafec [--workers 1,2,4] [--children 32] [--runs 5]
"""
import argparse
import os
import platform
import statistics
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--ceesafec", required=True)
    parser.add_argument("--workers", default="1,2,4")
    parser.add_argument("--children", type=int, default=32)
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--cc", help="the C compiler ceesafec uses (its default otherwise)")
    args = parser.parse_args()
    compiler = os.path.abspath(os.path.expanduser(args.ceesafec))
    with open(os.path.join(HERE, "fanout.cee"), encoding="utf-8") as f:
        source = f.read()
    counts = [int(w) for w in args.workers.split(",")]
    print(f"async parallel: {platform.system()} {platform.machine()}, {os.cpu_count()} cores, "
          f"{args.children} children, median of {args.runs}")
    print(f"{'workers':>8}  {'ms':>8}  {'speed-up':>8}")
    base = None
    sums = set()
    with tempfile.TemporaryDirectory() as work:
        for n in counts:
            path = os.path.join(work, f"fanout{n}.cee")
            with open(path, "w", encoding="utf-8") as f:
                f.write(source.replace("WORKERS", f"{n}u32").replace("CHILDREN", f"{args.children}u32"))
            program = os.path.join(work, f"fanout{n}")
            build = [compiler, "build", path, "-o", program] + (["--cc", args.cc] if args.cc else [])
            built = subprocess.run(build, capture_output=True, text=True)
            if built.returncode != 0:
                sys.exit(f"build failed:\n{built.stdout}{built.stderr}")
            times = []
            for _ in range(args.runs):
                out = subprocess.run([program], capture_output=True, text=True, check=True).stdout.split()
                times.append(int(out[0]))
                sums.add(out[1])
            median = statistics.median(times)
            if base is None:
                base = median
            print(f"{n:>8}  {median:>8.0f}  {base / median if median else 0.0:>7.2f}x")
    if len(sums) != 1:
        sys.exit(f"the sums differ between worker counts: {sorted(sums)}")


if __name__ == "__main__":
    main()
