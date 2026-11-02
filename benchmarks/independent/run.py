#!/usr/bin/env python3
"""CeeSafe vs Rust: an independent benchmark harness.

Each workload has main.cee and main.rs implementing the same algorithm with the same data structures. The size
comes from argv (nothing can be constant-folded) and each program prints a checksum; every configuration must print
the same checksum or the run aborts.

Configurations:
  rust          rustc -C opt-level=3 -C codegen-units=1 -C panic=abort   (best-tuned release; overflow unchecked)
  rust-checked  the same plus -C overflow-checks=on                   (overflow checks in the program's own code,
                as CeeSafe always has; Rust's precompiled std, e.g. HashMap and sort, stays unchecked)
  cee-gcc       ceesafec build                                         (the default: the host cc, here gcc -O2)
  cee-clang     ceesafec build --cc clang                              (clang 18 -O2; rustc bundles a newer LLVM)
  cee-clang-O3  ceesafec build --cc clang --cflag -O3                  (what one flag gives: Rust's release is -O3)

Measurement: one discarded warm-up round, then ROUNDS rounds; inside each round the configurations run in a
shuffled order (fixed seed), so slow drift of the machine falls on all of them alike. Wall time by perf_counter around
fork/exec/wait; peak RSS from wait4. Reported: median, min, and the interquartile spread as a percentage of the median.
"""
import argparse, json, math, os, random, resource, shutil, statistics, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
# Imported into CeeSafe as benchmarks/independent: binaries go to the repository's out/, which is not committed.
BIN = os.path.join(HERE, "..", "..", "out", "benchmarks", "independent")
# ru_maxrss is KiB on Linux and bytes on macOS.
RSS_PER_MIB = 1024.0 * 1024.0 if sys.platform == "darwin" else 1024.0


def cpu_name():
    if os.path.exists("/proc/cpuinfo"):
        return next((l.split(":", 1)[1].strip() for l in open("/proc/cpuinfo") if l.startswith("model name")), "?")
    r = subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"], capture_output=True, text=True)
    return r.stdout.strip() or "?"

WORKLOADS = {  # name: size argument
    "nbody": 20000000,
    "spectral-norm": 4000,
    "fannkuch": 11,
    "binary-trees": 18,
    "mandelbrot": 4000,
    "hashmap": 4000000,
    "sort": 25000000,
    "text-parse": 40000000,
    "sieve": 100000000,
    "matmul": 1200,
    "dyn-dispatch": 200000,
    "sort-same": 10000000,      # the same hand-written quicksort in both: codegen, not library
    "hashmap-same": 8000000,    # the same hand-written linear-probing table in both
}

RUST_FLAGS = ["-C", "opt-level=3", "-C", "codegen-units=1", "-C", "panic=abort"]


def configs(ceesafec):
    return {
        "rust": lambda src, out: ["rustc", *RUST_FLAGS, src, "-o", out],
        "rust-checked": lambda src, out: ["rustc", *RUST_FLAGS, "-C", "overflow-checks=on", src, "-o", out],
        "cee-gcc": lambda src, out: [ceesafec, "build", src, "-o", out],
        "cee-clang": lambda src, out: [ceesafec, "build", "--cc", "clang", src, "-o", out],
        "cee-clang-O3": lambda src, out: [ceesafec, "build", "--cc", "clang", "--cflag", "-O3", src, "-o", out],
    }


def run_once(exe, arg, pin):
    argv = ([*pin] if pin else []) + [exe, str(arg)]
    start = time.perf_counter()
    p = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    out = p.stdout.read()
    err = p.stderr.read()
    _, status, usage = os.wait4(p.pid, 0)
    wall = time.perf_counter() - start
    if status != 0:
        sys.exit(f"{exe} {arg} failed with status {status}: {err.decode()[:300]}")
    return wall, usage.ru_maxrss / RSS_PER_MIB, out.decode()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ceesafec", required=True)
    ap.add_argument("--rounds", type=int, default=9)
    ap.add_argument("--workloads", default=",".join(WORKLOADS))
    ap.add_argument("--json", default=os.path.join(HERE, "results.json"))
    args = ap.parse_args()
    ceesafec = os.path.abspath(args.ceesafec)
    names = args.workloads.split(",")
    cfgs = configs(ceesafec)
    pin = ["taskset", "-c", "1"] if shutil.which("taskset") and os.cpu_count() and os.cpu_count() > 1 else None
    bindir = os.path.abspath(BIN)
    os.makedirs(bindir, exist_ok=True)
    rng = random.Random(20261102)
    results = {"meta": {
        "rustc": subprocess.run(["rustc", "-vV"], capture_output=True, text=True).stdout.strip(),
        "ceesafec": subprocess.run([ceesafec, "--version"], capture_output=True, text=True).stdout.strip(),
        "gcc": subprocess.run(["gcc", "--version"], capture_output=True, text=True).stdout.splitlines()[0],
        "clang": subprocess.run(["clang", "--version"], capture_output=True, text=True).stdout.splitlines()[0],
        "cpu": cpu_name(), "platform": sys.platform,
        "cpus": os.cpu_count(), "rounds": args.rounds, "pinned": bool(pin),
        "rust_flags": " ".join(RUST_FLAGS)}, "workloads": {}}

    for name in names:
        arg = WORKLOADS[name]
        exes, build = {}, {}
        for cfg, cmd in cfgs.items():
            src = os.path.join(HERE, name, "main.rs" if cfg.startswith("rust") else "main.cee")
            out = os.path.join(bindir, f"{name}-{cfg}")
            t0 = time.perf_counter()
            r = subprocess.run(cmd(src, out), capture_output=True, text=True)
            secs = time.perf_counter() - t0
            if r.returncode != 0:
                sys.exit(f"build {name} {cfg} failed:\n{r.stderr[-2000:]}")
            stripped = out + ".stripped"
            subprocess.run(["strip", "-o", stripped, out], check=True)
            build[cfg] = {"seconds": secs, "bytes": os.path.getsize(stripped)}
            exes[cfg] = out
        samples = {c: [] for c in cfgs}
        rss = {c: 0.0 for c in cfgs}
        outputs = {}
        for rnd in range(args.rounds + 1):
            order = list(cfgs)
            rng.shuffle(order)
            for cfg in order:
                wall, peak, out = run_once(exes[cfg], arg, pin)
                outputs.setdefault(cfg, out)
                if out != outputs[cfg]:
                    sys.exit(f"{name} {cfg}: output changed between runs")
                if rnd > 0:  # round 0 is the warm-up
                    samples[cfg].append(wall)
                    rss[cfg] = max(rss[cfg], peak)
        if len(set(outputs.values())) != 1:
            sys.exit(f"{name}: outputs differ between configurations: {outputs}")
        row = {"arg": arg, "checksum": outputs["rust"].strip().replace("\n", " "), "configs": {}}
        for cfg in cfgs:
            s = sorted(samples[cfg])
            q = statistics.quantiles(s, n=4)
            row["configs"][cfg] = {
                "median_ms": statistics.median(s) * 1000, "min_ms": s[0] * 1000,
                "spread_pct": (q[2] - q[0]) / statistics.median(s) * 100,
                "samples_ms": [x * 1000 for x in samples[cfg]], "peak_rss_mib": rss[cfg],
                "binary_kib": build[cfg]["bytes"] / 1024, "compile_s": build[cfg]["seconds"]}
        results["workloads"][name] = row
        c = row["configs"]
        print(f"{name:14} rust {c['rust']['median_ms']:8.1f}  rust-chk {c['rust-checked']['median_ms']:8.1f}  "
              f"cee-gcc {c['cee-gcc']['median_ms']:8.1f}  cee-clang {c['cee-clang']['median_ms']:8.1f} ms   "
              f"cee-O3 {c['cee-clang-O3']['median_ms']:8.1f}  (cee-gcc/rust-chk {c['cee-gcc']['median_ms'] / c['rust-checked']['median_ms']:.2f})", flush=True)
        json.dump(results, open(args.json, "w"), indent=1)

    # geometric means of the ratios
    def geo(xs):
        return math.exp(sum(math.log(x) for x in xs) / len(xs))
    summary = {}
    for base in ("rust", "rust-checked"):
        for cfg in cfgs:
            ratios = [w["configs"][cfg]["median_ms"] / w["configs"][base]["median_ms"] for w in results["workloads"].values()]
            summary[f"{cfg}/{base}"] = geo(ratios)
    results["geomean"] = summary
    # 95% bootstrap interval of each geometric mean: resample each workload's runs, recompute medians and the mean
    boot = random.Random(7)
    ci = {}
    for base in ("rust", "rust-checked"):
        for cfg in cfgs:
            if cfg == base:
                continue
            vals = []
            for _ in range(2000):
                ratios = []
                for w in results["workloads"].values():
                    a = [boot.choice(w["configs"][cfg]["samples_ms"]) for _ in w["configs"][cfg]["samples_ms"]]
                    b = [boot.choice(w["configs"][base]["samples_ms"]) for _ in w["configs"][base]["samples_ms"]]
                    ratios.append(statistics.median(a) / statistics.median(b))
                vals.append(geo(ratios))
            vals.sort()
            ci[f"{cfg}/{base}"] = [vals[49], vals[1949]]
    results["geomean_ci95"] = ci
    json.dump(results, open(args.json, "w"), indent=1)
    print("geometric means:", json.dumps(summary, indent=1))
    print("95% intervals:", json.dumps(ci, indent=1))


if __name__ == "__main__":
    main()
