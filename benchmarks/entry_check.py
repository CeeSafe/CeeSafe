#!/usr/bin/env python3
"""ADR 0088 §5's benchmark of the panicked check: a call-heavy export loop with and without ceesafe_entry_check.

An export that does almost nothing, called from a C host in a loop (no LTO, so every call is a real call into the
export's wrapper). The same generated C is compiled twice: as ceesafec writes it, and with the wrapper's
`ceesafe_entry_check();` line removed. Each binary runs --runs times; the medians are printed, with the difference
per call.

usage: python3 benchmarks/entry_check.py --compiler out/ceesafec [--cc clang] [--calls 400000000] [--runs 7]
"""

import argparse
import os
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

PROGRAM = """// The export the loop calls.
@export("bench_step")
fn step(x: u64, k: u64) -> u64 => (x ^ k) + 1;
"""

HOST = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "ceesafe/embed.h"
uint64_t bench_step(uint64_t, uint64_t);
int main(int argc, char **argv) {
    ceesafe_config config = {0};
    config.size = (uint32_t)sizeof config;
    if (ceesafe_init(&config) != 0) return 1;
    uint64_t calls = strtoull(argv[1], NULL, 10), x = (uint64_t)argc;
    for (uint64_t k = 0; k < calls; k++) x = bench_step(x, k);
    printf("%llu\n", (unsigned long long)x);
    return 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--cc", default="clang" if shutil.which("clang") else "cc")
    parser.add_argument("--calls", type=int, default=400_000_000)
    parser.add_argument("--runs", type=int, default=7)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="ceesafe-entry-") as work:
        source = os.path.join(work, "bench.cee")
        with open(source, "w", encoding="utf-8") as handle:
            handle.write(PROGRAM)
        c_dir = os.path.join(work, "c")
        subprocess.run([os.path.abspath(args.compiler), "build", "--emit", "c", source, "-o", c_dir], check=True)
        with open(os.path.join(work, "host.c"), "w", encoding="utf-8") as handle:
            handle.write(HOST)
        main_c = os.path.join(c_dir, "main.c")
        text = open(main_c, encoding="utf-8").read()
        if "    ceesafe_entry_check();\n" not in text:
            print("entry_check: the generated export has no ceesafe_entry_check call")
            return 1
        variants = {"with the check": text, "without": text.replace("    ceesafe_entry_check();\n", "")}
        runtime = [os.path.join(c_dir, n) for n in sorted(os.listdir(c_dir)) if n.startswith("ceesafe_rt_")]
        times = {}
        for name, generated in variants.items():
            variant_c = os.path.join(c_dir, "main.c")
            with open(variant_c, "w", encoding="utf-8") as handle:
                handle.write(generated)
            exe = os.path.join(work, name.replace(" ", "_"))
            subprocess.run([args.cc, "-std=c11", "-O2", "-I", c_dir, os.path.join(work, "host.c"), variant_c,
                            *runtime, "-o", exe, "-lpthread", "-lm"], check=True)
            times[name] = []
        for _ in range(args.runs):  # interleaved, so drift affects both alike
            for name in variants:
                exe = os.path.join(work, name.replace(" ", "_"))
                start = time.perf_counter()
                subprocess.run([exe, str(args.calls)], check=True, capture_output=True)
                times[name].append(time.perf_counter() - start)
        medians = {name: statistics.median(t) for name, t in times.items()}
        for name, median in medians.items():
            spread = max(times[name]) - min(times[name])
            print(f"{name:15} median {median:.3f} s ({median / args.calls * 1e9:.3f} ns per call), "
                  f"spread {spread:.3f} s over {args.runs} runs")
        difference = medians["with the check"] - medians["without"]
        print(f"difference: {difference / args.calls * 1e9:+.3f} ns per call "
              f"({difference / medians['without'] * 100:+.1f}%)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
