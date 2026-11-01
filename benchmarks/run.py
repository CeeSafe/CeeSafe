#!/usr/bin/env python3
"""Runs the benchmark suite: each workload in CeeSafe, Rust and C, checks every checksum, and prints wall time,
peak memory, allocations, compile time and binary size. See benchmarks/README.md.

    python3 benchmarks/run.py                               # all workloads, cee,rust,rust-checked,c, 5 runs
    python3 benchmarks/run.py --workloads nbody,hash-map --runs 3 --json out/bench.json
"""
import argparse
import json
import os
import platform
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SUITE = ROOT / "benchmarks"
# The CeeSafe programs are the corpus's (tests/reference/benchmarks/suite); the Rust and C ones are here.
CEE_SUITE = ROOT / "tests" / "reference" / "benchmarks" / "suite"

CHECKSUMS = {
    "small-objects": 512000079999994,
    "large-objects": 101989120,
    "stack-alloc": 319680000000,
    "heap-alloc": 159840000000,
    "arena": 39960000000,
    "vector-growth": 99900000000,
    "string-building": 206669134195,
    "recursion": 102334155,
    "linked-list": 9990000000,
    "binary-trees": 29971806,
    "hash-map": 16000008000000,
    "parsing": 799992000000,
    "memory-copy": 163866560,
    "numeric": 2433654612000,
    "data-oriented": 999040128,
    "nbody": 169075164169077842,
}

LANGS = ["cee", "rust", "rust-checked", "c"]
SOURCE = {"cee": "main.cee", "rust": "main.rs", "rust-checked": "main.rs", "c": "main.c"}


def source_of(workload, lang):
    return (CEE_SUITE if lang == "cee" else SUITE) / workload / SOURCE[lang]


def build_command(lang, source, exe, args):
    rust = [args.rustc, "-C", "opt-level=2", "-C", "codegen-units=1"]
    return {
        "cee": [args.ceesafec, "build", "--cc", args.cc, *args.cee_flag, "-o", exe, source],
        "rust": [*rust, "-o", exe, source],
        "rust-checked": [*rust, "-C", "overflow-checks=on", "-o", exe, source],
        "c": [args.cc, "-O2", "-std=c11", "-o", exe, source, "-lm"],
    }[lang]


def tool_of(lang, args):
    return {"cee": args.ceesafec, "rust": args.rustc, "rust-checked": args.rustc, "c": args.cc}[lang]


def run_once(measure, exe):
    """Runs exe under the measure launcher; returns (stdout, wall seconds, peak RSS bytes)."""
    proc = subprocess.run([measure, exe], capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"{exe} exited with {proc.returncode}: {proc.stderr}")
    fields = proc.stderr.split()
    return proc.stdout.strip(), int(fields[fields.index("wall_ns") + 1]) / 1e9, int(fields[fields.index("maxrss") + 1])


def allocations(exe, preload, work):
    report = work / "alloc.txt"
    report.unlink(missing_ok=True)
    env = dict(os.environ, NVBENCH_ALLOC_OUT=str(report))
    env["DYLD_INSERT_LIBRARIES" if sys.platform == "darwin" else "LD_PRELOAD"] = str(preload)
    subprocess.run([exe], env=env, stdout=subprocess.DEVNULL, check=True)
    fields = report.read_text().split()
    return {fields[i]: int(fields[i + 1]) for i in range(0, len(fields), 2)}


def summary(results, langs):
    """Prints each language's time relative to rust, and the geometric mean over the workloads."""
    if "rust" not in langs:
        return
    by = {(r["workload"], r["lang"]): r["wall"] for r in results}
    workloads = list(dict.fromkeys(r["workload"] for r in results))
    print("\ntime relative to rust (lower is faster)")
    print(f"{'workload':16}" + "".join(f"{lang:>14}" for lang in langs))
    ratios = {lang: [] for lang in langs}
    for w in workloads:
        cells = []
        for lang in langs:
            ratio = by[(w, lang)] / by[(w, "rust")]
            ratios[lang].append(ratio)
            cells.append(f"{ratio:14.2f}")
        print(f"{w:16}" + "".join(cells))
    geo = [statistics.geometric_mean(ratios[lang]) for lang in langs]
    print(f"{'geometric mean':16}" + "".join(f"{g:14.2f}" for g in geo))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--langs", default=",".join(LANGS))
    parser.add_argument("--workloads", default=",".join(CHECKSUMS))
    parser.add_argument("--ceesafec", default=str(ROOT / "out" / "ceesafec"))
    parser.add_argument("--cee-flag", action="append", default=[],
                        help="an extra ceesafec build option, such as --no-bce (repeatable)")
    parser.add_argument("--rustc", default="rustc")
    parser.add_argument("--json")
    args = parser.parse_args()

    work = ROOT / "out" / "benchmarks" / Path(args.cc).name
    work.mkdir(parents=True, exist_ok=True)
    preload = work / ("allocount.dylib" if sys.platform == "darwin" else "allocount.so")
    subprocess.run([args.cc, "-O2", "-dynamiclib" if sys.platform == "darwin" else "-shared", "-fPIC", "-o", preload,
                    SUITE / "allocount.c"], check=True)
    measure = work / "measure"
    subprocess.run([args.cc, "-O2", "-o", measure, SUITE / "measure.c"], check=True)

    langs = []
    for lang in args.langs.split(","):
        if shutil.which(tool_of(lang, args)) is None:
            print(f"skipping {lang}: {tool_of(lang, args)} not found", file=sys.stderr)
        else:
            langs.append(lang)
    cc_version = subprocess.run([args.cc, "--version"], capture_output=True, text=True).stdout.splitlines()[0]
    tools = {"ceesafec": args.ceesafec, "rustc": args.rustc}
    versions = {name: subprocess.run([path, "--version"], capture_output=True, text=True).stdout.strip()
                for name, path in tools.items() if shutil.which(path)}
    print(f"machine: {platform.machine()} {platform.system()} {platform.release()}; C compiler: {cc_version}; "
          f"{'; '.join(versions.values())}; runs: {args.runs}")

    # One discarded build of each language first, so compile times are measured with global caches warm.
    for lang in langs:
        subprocess.run(build_command(lang, source_of("recursion", lang), work / f"warm.{lang}", args),
                       capture_output=True)

    results = []
    for workload in args.workloads.split(","):
        for lang in langs:
            source = source_of(workload, lang)
            exe = work / f"{workload}.{lang}"
            start = time.perf_counter()
            built = subprocess.run(build_command(lang, source, exe, args), capture_output=True, text=True)
            compile_time = time.perf_counter() - start
            if built.returncode != 0:
                print(f"{workload} {lang}: build failed\n{built.stdout}{built.stderr}", file=sys.stderr)
                return 1
            walls, rss = [], 0
            run_once(measure, exe)  # discarded: the first run of a new binary pays for loading and checking it
            for _ in range(args.runs):
                out, wall, peak = run_once(measure, exe)
                if out != str(CHECKSUMS[workload]):
                    print(f"{workload} {lang}: checksum {out}, expected {CHECKSUMS[workload]}", file=sys.stderr)
                    return 1
                walls.append(wall)
                rss = max(rss, peak)
            row = {"workload": workload, "lang": lang, "wall": statistics.median(walls), "wall_min": min(walls),
                   "wall_max": max(walls), "rss": rss, "compile": compile_time, "size": exe.stat().st_size,
                   **allocations(exe, preload, work)}
            results.append(row)
            print(f"{workload:16} {lang:13} {row['wall'] * 1000:9.1f} ms  rss {rss / 2**20:8.1f} MiB  "
                  f"allocs {row['allocations']:>9}  peak heap {row['peak'] / 2**20:8.1f} MiB  "
                  f"compile {compile_time:6.2f} s  size {row['size'] / 1024:7.1f} KiB")
            sys.stdout.flush()

    summary(results, langs)
    if args.json:
        Path(args.json).write_text(json.dumps({"machine": platform.machine(), "system": platform.system(),
                                               "cc": cc_version, "versions": versions, "runs": args.runs,
                                               "results": results}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
