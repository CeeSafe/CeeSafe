#!/usr/bin/env python3
"""The async reactor benchmark (L1 A4b; spec/c-backend.md §6): reactor.cee with IDLE idle connections (both ends a
task parked on a read) and ROUNDS one-byte round trips on one more. It prints the median time per round trip for each
size, with the host's reactor (kqueue, epoll; Windows: WSAPoll) and with poll() (CEESAFE_REACTOR_POLL=1). A reactor
that scales keeps the time per round trip flat as IDLE grows.

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
    native = "kqueue" if platform.system() in ("Darwin", "FreeBSD") else (
        "epoll" if platform.system() == "Linux" else "WSAPoll")
    print(f"async reactor: {platform.system()} {platform.machine()}, {args.rounds} round trips, median of {args.runs}")
    print(f"{'idle':>6}  {native + ' us/rt':>14}  {'poll() us/rt':>14}")
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
            for poll in (False, True):
                env = dict(os.environ)
                if poll:
                    env["CEESAFE_REACTOR_POLL"] = "1"
                times = []
                for _ in range(args.runs):
                    ran = subprocess.run([exe], capture_output=True, text=True, env=env, timeout=600)
                    out = ran.stdout.strip()
                    if ran.returncode != 0 or not out.isdigit():
                        sys.exit(f"idle {idle}: run failed ({ran.returncode}): {out} {ran.stderr.strip()}")
                    times.append(int(out))
                row.append(statistics.median(times) * 1000.0 / args.rounds)
            print(f"{idle:>6}  {row[0]:>14.1f}  {row[1]:>14.1f}")


if __name__ == "__main__":
    main()
