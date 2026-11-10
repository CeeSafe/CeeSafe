#!/usr/bin/env python3
"""ADR 0088 §5's benchmark of the allocator hook: allocation-heavy programs with a hook that calls the C library's
malloc, against no hook.

Each program of the suite (tests/reference/benchmarks/suite) is built three ways from `build --emit c`:
- `baseline`: by --baseline, a ceesafec without the hook (0.28.0): the platform functions call malloc directly;
- `no hook`: by --compiler, whose platform functions test for a hook and then call malloc;
- `hook`: the same C, with the generated `main` passing an allocator whose functions call malloc, calloc, realloc and
  free: every block through an indirect call.
Every binary is compiled with the same C compiler at -O2 and runs --runs times, interleaved in a rotating order; the
medians are printed,
with each variant's difference from the baseline.

With --aa, a second copy of the baseline binary runs too: an A/A check of how far noise alone moves a median.

usage: python3 benchmarks/alloc_hook.py --compiler out/ceesafec --baseline <ceesafec 0.28.0> [--cc clang] [--runs 11]
       [--aa] [programs...]
"""

import argparse
import os
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
SUITE = os.path.join(ROOT, "tests", "reference", "benchmarks", "suite")
PROGRAMS = ["vector-growth", "string-building", "small-objects", "binary-trees", "heap-alloc"]

HOOK = """
#include <stdlib.h>
static void *cee_bench_alloc(void *context, size_t size) { (void)context; return malloc(size); }
static void *cee_bench_alloc_zeroed(void *context, size_t size) { (void)context; return calloc(1, size); }
static void *cee_bench_realloc(void *context, void *block, size_t size) { (void)context; return realloc(block, size); }
static void cee_bench_free(void *context, void *block) { (void)context; free(block); }
static const ceesafe_allocator cee_bench_allocator = {cee_bench_alloc, cee_bench_alloc_zeroed, cee_bench_realloc,
                                                      cee_bench_free, NULL};
"""
MAIN = "int main(int argc, char **argv) {\n"
CONFIG = "        .flags = CEESAFE_INIT_ENVIRONMENT,\n"


def emit(compiler, source, out):
    subprocess.run([os.path.abspath(compiler), "build", "--emit", "c", source, "-o", out], check=True,
                   capture_output=True)
    return out


def compile_c(cc, c_dir, exe):
    sources = [os.path.join(c_dir, n) for n in sorted(os.listdir(c_dir)) if n.endswith(".c")]
    subprocess.run([cc, "-std=c11", "-O2", "-I", c_dir, *sources, "-o", exe, "-lpthread", "-lm"], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--cc", default="clang" if shutil.which("clang") else "cc")
    parser.add_argument("--runs", type=int, default=11)
    parser.add_argument("--aa", action="store_true", help="also run a copy of the baseline binary")
    parser.add_argument("programs", nargs="*", default=PROGRAMS)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="ceesafe-alloc-hook-") as work:
        for program in args.programs:
            source = os.path.join(SUITE, program, "main.cee")
            base_dir = emit(args.baseline, source, os.path.join(work, program, "baseline"))
            new_dir = emit(args.compiler, source, os.path.join(work, program, "new"))
            hook_dir = os.path.join(work, program, "hook")
            shutil.copytree(new_dir, hook_dir)
            main_c = os.path.join(hook_dir, "main.c")
            text = open(main_c, encoding="utf-8").read()
            if text.count(MAIN) != 1 or text.count(CONFIG) != 1:
                print(f"alloc_hook: {program}: the generated main is not the expected one")
                return 1
            text = text.replace(MAIN, HOOK + MAIN).replace(CONFIG, CONFIG + "        .allocator = &cee_bench_allocator,\n")
            with open(main_c, "w", encoding="utf-8") as handle:
                handle.write(text)
            variants = {"baseline": base_dir, "no hook": new_dir, "hook": hook_dir}
            exes, outputs, times = {}, {}, {}
            for name, c_dir in variants.items():
                exes[name] = os.path.join(work, program, name.replace(" ", "_") + ".exe")
                compile_c(args.cc, c_dir, exes[name])
                times[name] = []
            if args.aa:
                exes["baseline again"] = exes["baseline"] + ".copy"
                shutil.copy2(exes["baseline"], exes["baseline again"])
                times["baseline again"] = []
            names = list(exes)
            for run in range(args.runs):  # interleaved, the order rotated, so drift affects every variant alike
                for name in names[run % len(names):] + names[:run % len(names)]:
                    exe = exes[name]
                    start = time.perf_counter()
                    r = subprocess.run([exe], check=True, capture_output=True, text=True)
                    times[name].append(time.perf_counter() - start)
                    outputs.setdefault(name, r.stdout)
            if len(set(outputs.values())) != 1:
                print(f"alloc_hook: {program}: the variants' outputs differ: {outputs}")
                return 1
            medians = {name: statistics.median(t) for name, t in times.items()}
            print(f"{program}:")
            for name, median in medians.items():
                spread = max(times[name]) - min(times[name])
                change = (median - medians["baseline"]) / medians["baseline"] * 100
                print(f"  {name:14} median {median:.3f} s, spread {spread:.3f} s, {change:+.1f}% against the baseline")
            sys.stdout.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
