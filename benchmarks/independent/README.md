# CeeSafe vs Rust: an independent benchmark

Thirteen single-threaded workloads, each written twice, in CeeSafe (`main.cee`) and in Rust (`main.rs`), with the
same algorithm and the same data structures. Every program takes its size from the command line, so nothing can
be constant-folded, and prints a checksum. `run.py` checks that every configuration prints the same checksum.

```sh
python3 run.py --ceesafec /path/to/ceesafec                      # all workloads, 9 timed rounds
python3 run.py --ceesafec /path/to/ceesafec --workloads nbody,matmul --rounds 5
```

It needs `rustc`, `gcc`, `clang`, `strip` and, optionally, `taskset` (runs are pinned to CPU 1 when it is present).

## Configurations

| Name | Build |
|---|---|
| `rust` | `rustc -C opt-level=3 -C codegen-units=1 -C panic=abort` (release, tuned; overflow unchecked) |
| `rust-checked` | the same plus `-C overflow-checks=on`: the baseline, since CeeSafe always checks overflow |
| `cee-gcc` | `ceesafec build`, the default: the host `cc` (gcc here) at `-O2` |
| `cee-clang` | `ceesafec build --cc clang` (clang 18, `-O2`) |
| `cee-clang-O3` | `ceesafec build --cc clang --cflag -O3` |

## Workloads

| Workload | What it stresses | Note |
|---|---|---|
| `nbody` | float arithmetic over a fixed `[Body; 5]` | both sides know the count is 5 |
| `spectral-norm` | float, a small function called n² times | |
| `fannkuch` | small-array permutations, data-dependent indexes | |
| `binary-trees` | millions of small heap allocations, recursion | both use glibc malloc |
| `mandelbrot` | pure float loops | |
| `hashmap` | the standard library maps: insert, hit, miss, remove | library vs library: SipHash-1-3 on both, but Rust's table is hashbrown (SwissTable) |
| `sort` | the standard library unstable sort with a comparator | library vs library: Rust's ipnsort vs CeeSafe's introsort |
| `text-parse` | writing then parsing 40M decimal numbers as bytes | |
| `sieve` | Eratosthenes over 100M byte flags | CeeSafe has no `vec![x; n]`, so it fills with `push` |
| `matmul` | 1200×1200 float matrix product, flat row-major, index arithmetic | |
| `dyn-dispatch` | 200M calls through `Box<dyn Shape>` | |
| `sort-same` | the same hand-written quicksort in both | the compilers, without the libraries |
| `hashmap-same` | the same hand-written linear-probing table in both | the compilers, without the libraries |

## Method

- One discarded warm-up round, then 9 rounds. Inside each round the five configurations run in a shuffled order
  (fixed seed), so slow drift of the machine falls on all of them alike.
- Wall time of the whole process (including start-up) by `perf_counter` around fork/exec/wait; peak RSS from `wait4`.
- Reported: the median, the minimum, and the interquartile spread as a percentage of the median. The geometric mean
  over workloads comes with a 95% bootstrap interval (runs resampled within each workload).
- Binary sizes are of stripped copies.
- Results go to `results.json`.

## Known limits

- One machine: a 2-vCPU cloud VM with no control over frequency or neighbours. Spreads of 5–30% occur; compare
  ratios of medians, not single runs.
- `rust-checked` checks overflow in the program's own code only. Rust's precompiled standard library (`HashMap`,
  `sort`) stays unchecked, while CeeSafe's library is checked.
- rustc bundles LLVM 22; the C compilers are gcc 13 and clang 18.
- Neither side targets the native CPU.

## In this repository

Imported on 2026-11-02 from an independent author, unchanged except for `run.py`. That file was changed so it runs on macOS too: the CPU name comes from `sysctl` when there is no
`/proc/cpuinfo`, `ru_maxrss` is read as bytes on macOS, and binaries go to `out/benchmarks/independent/` instead of
`bin/`. The programs are the author's: keep them as written, so the comparison stays theirs.

| Run | Machine | ceesafec | Results |
|---|---|---|---|
| the author's | a 2-vCPU Xeon VM, Ubuntu 24.04, gcc 13, clang 18, rustc 1.97, pinned | 0.9.0 | `results-linux-0.9.0.json`, `run-linux-0.9.0.log` |
| 2026-11-02 | Apple M1 (8 cores), macOS, Apple clang 21 (the `gcc` there), rustc 1.99.1, not pinned | 0.9.3 | `results-macos-0.9.3.json`, `run-macos-0.9.3.log` |

```sh
python3 benchmarks/independent/run.py --ceesafec out/ceesafec --json benchmarks/independent/results-<os>-<version>.json
```
