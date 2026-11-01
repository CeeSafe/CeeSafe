#!/usr/bin/env python3
"""The async reactor benchmark (L1 A4b; spec/c-backend.md §6): reactor.cee with IDLE idle connections (both ends a
task parked on a read) and ROUNDS one-byte round trips on one more. It prints the median time per round trip for each
size, on each of the host's backends (ADR 0035): kqueue or epoll, and io_uring on Linux (CEESAFE_REACTOR_URING=1), or
IOCP on Windows; and poll(), or WSAPoll on Windows (CEESAFE_REACTOR_POLL=1). A backend that scales keeps the time per
round trip flat as IDLE grows.

    python3 benchmarks/async-reactor/run.py --ceesafec out/ceesafec [--idle 10,100,1000] [--rounds 20000] [--runs 5]
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
    parser.add_argument("--idle", default="10,100,1000")
    parser.add_argument("--rounds", type=int, default=20000)
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--cc", help="the C compiler ceesafec uses (its default otherwise)")
    args = parser.parse_args()
    compiler = os.path.abspath(os.path.expanduser(args.ceesafec))
    with open(os.path.join(HERE, "reactor.cee"), encoding="utf-8") as f:
        source = f.read()
    system = platform.system()
    if system == "Linux":
        backends = [("epoll", {}), ("io_uring", {"CEESAFE_REACTOR_URING": "1"}), ("poll()", {"CEESAFE_REACTOR_POLL": "1"})]
    elif system == "Windows":
        backends = [("IOCP", {}), ("WSAPoll", {"CEESAFE_REACTOR_POLL": "1"})]
    else:
        backends = [("kqueue", {}), ("poll()", {"CEESAFE_REACTOR_POLL": "1"})]
    print(f"async reactor: {system} {platform.machine()}, {args.rounds} round trips, median of {args.runs}")
    print(f"{'idle':>6}" + "".join(f"  {name + ' us/rt':>15}" for name, _ in backends))
    with tempfile.TemporaryDirectory(prefix="ceesafe-reactor-") as work:
        for idle in [int(x) for x in args.idle.split(",")]:
            text = source.replace("const IDLE: usize = 1000;", f"const IDLE: usize = {idle};", 1)
            text = text.replace("const ROUNDS: u32 = 20000;", f"const ROUNDS: u32 = {args.rounds};", 1)
            path = os.path.join(work, f"reactor_{idle}.cee")
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)
            exe = os.path.join(work, f"reactor_{idle}" + (".exe" if os.name == "nt" else ""))
            build = [compiler, "build"] + (["--cc", args.cc] if args.cc else []) + [path, "-o", exe]
            built = subprocess.run(build, capture_output=True, text=True)
            if built.returncode != 0:
                sys.exit(f"build failed:\n{built.stderr}")
            row = []
            for _, extra in backends:
                env = dict(os.environ)
                env.pop("CEESAFE_REACTOR_POLL", None)
                env.pop("CEESAFE_REACTOR_URING", None)
                env.update(extra)
                times = []
                for _ in range(args.runs):
                    ran = subprocess.run([exe], capture_output=True, text=True, env=env, timeout=600)
                    out = ran.stdout.strip()
                    if ran.returncode != 0 or not out.isdigit():
                        sys.exit(f"idle {idle}: run failed ({ran.returncode}): {out} {ran.stderr.strip()}")
                    times.append(int(out))
                row.append(statistics.median(times) * 1000.0 / args.rounds)
            print(f"{idle:>6}" + "".join(f"  {t:>15.1f}" for t in row))


if __name__ == "__main__":
    main()
