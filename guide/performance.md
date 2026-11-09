# Performance

CeeSafe compiles through C, with every safety check on: array bounds, integer overflow, lossy conversions. This page
measures what that costs against Rust and plain C on sixteen workloads, and says where CeeSafe is ahead or behind and
why.

## The result

Over the sixteen workloads CeeSafe takes **0.96× Rust's time** (geometric mean), and **0.88×** that of Rust built with
overflow checks, which is the comparison that checks the same things CeeSafe does. Hand-written C without any check takes
0.94×.

| Workload | CeeSafe | Rust | Rust, overflow checks | C | CeeSafe (ms) | Rust (ms) |
|---|---:|---:|---:|---:|---:|---:|
| `heap-alloc` | **0.28** | 1.00 | 0.92 | 0.95 | 121 | 428 |
| `nbody` | **0.82** | 1.00 | 1.08 | 0.82 | 500 | 611 |
| `stack-alloc` | **0.84** | 1.00 | 1.70 | 0.85 | 244 | 289 |
| `vector-growth` | **0.87** | 1.00 | 1.03 | 0.80 | 385 | 443 |
| `arena` | **0.95** | 1.00 | 0.91 | 0.98 | 241 | 254 |
| `memory-copy` | 0.99 | 1.00 | 1.06 | 1.13 | 595 | 602 |
| `linked-list` | 1.01 | 1.00 | 0.99 | 0.90 | 366 | 361 |
| `large-objects` | 1.02 | 1.00 | 0.97 | 1.02 | 254 | 250 |
| `small-objects` | 1.03 | 1.00 | 1.00 | 0.89 | 510 | 493 |
| `string-building` | 1.03 | 1.00 | 1.31 | 0.78 | 299 | 292 |
| `numeric` | 1.12 | 1.00 | 1.00 | 1.14 | 240 | 214 |
| `binary-trees` | 1.13 | 1.00 | 1.02 | 0.91 | 561 | 494 |
| `parsing` | 1.16 | 1.00 | 1.10 | 1.05 | 161 | 139 |
| `data-oriented` | 1.18 | 1.00 | 1.05 | 1.00 | 381 | 323 |
| `recursion` | 1.26 | 1.00 | 1.26 | 1.00 | 407 | 323 |
| `hash-map` | 1.41 | 1.00 | 1.17 | 0.94 | 453 | 321 |
| **geometric mean** | **0.96** | 1.00 | 1.09 | 0.94 | | |

Times are each language's median wall time over five runs (after one discarded run) divided by Rust's; lower is
faster. Measured 2026-10-31 on an Apple M-series Mac (arm64, macOS), with ceesafec 0.2.0-dev, rustc 1.99.1 and Apple
clang 21. A run-to-run spread of a few percent is normal: compare ratios, not single milliseconds.

Peak memory is the same as Rust's or lower everywhere (`arena`: 7.8 MiB against 17.7 MiB). Binaries are about 40–56 KiB
against Rust's 460 KiB. ceesafec compiles each program in about a quarter of a second, roughly twice rustc's time on these
single-file programs, since it runs the C compiler after its own work.

## Where CeeSafe is ahead

- **`heap-alloc`, 3.5× faster than Rust.** ceesafec's placement optimisation keeps a box that never escapes its
  function on the stack, so the program's per-iteration heap array costs no allocation at all. It beats hand-written C,
  which calls `malloc` and `free` each time.
- **`stack-alloc`, `nbody`, `vector-growth`.** CeeSafe matches C. Its overflow and bounds checks are removed where
  ceesafec proves they cannot fail (loop indexes within a length, sums of bounded values), which Rust with overflow
  checks does not manage on `stack-alloc` (1.70×).
- **`arena`.** A region's bump allocation is inlined into the program (Decision 0007), and the arena's chunks take less
  than half the memory of Rust's.

## Where CeeSafe is behind, and why

- **`hash-map` (1.41×).** Rust builds its tables with `vec![0; n]`, so LLVM knows their length and removes the bounds
  checks on `keys[s & MASK]`. A CeeSafe `Vector` can only be filled by `push`, so the length is unknown and every probe
  keeps its check: with the checks compiled out the program runs at Rust's speed. A filled-vector constructor would
  close this gap.
- **`recursion` (1.26×) and `parsing` (1.16×).** The overflow check on `fib(n - 1) + fib(n - 2)` and on accumulating
  digits cannot be proven unnecessary; Rust pays the same when its overflow checks are on (1.26× and 1.10×).
- **`data-oriented` (1.18×).** As for `hash-map`, the particle arrays are filled by `push`; the bounds checks cost about
  11%.
- **`binary-trees` (1.13×) and `small-objects` (1.03×).** Every heap allocation goes through the runtime's
  `ceesafe_alloc`, which counts allocations (for `CEESAFE_ALLOC_REPORT` and fault injection) before calling `malloc`.
  Calling `malloc` directly measured 12% faster on both.

## Const parameters

`const-flags` (ADR 0083 §1.3) is a recursive scanner over 10 MB of pseudo-random bytes, run 100 times, with three
flag tests per byte; its flags are const parameters (`fn scan<const TS: bool, const JSX: bool>`), so each instance
keeps only its own tests. It is a workload of `run.py`, measured on its own and not in the table above, whose sixteen
workloads were measured with an older ceesafec. Measured 2026-11-09 on the same kind of machine (Apple arm64, Apple
clang 21, rustc 1.99.1, ceesafec 0.26.0-dev), on a machine shared with other work, so the spread is wider than usual:

| Version | Time |
|---|---:|
| CeeSafe, const parameters (`main.cee`) | 1.05× Rust (2.67 s against 2.55 s, `run.py`, median of 5) |
| Rust, const generics (`main.rs`) | 1.00 |
| CeeSafe, the same flags as `bool` parameters (`runtime.cee`) | 1.19× CeeSafe's const version (3.68 s against 3.10 s user time, 3 runs each) |

At `-O2` clang does not clone a recursive function for a constant argument, so a run-time flag keeps every test;
the const parameter removes them, as Rust's const generics do. `main.c` takes the flags as arguments (C has no const
parameters), and in the same three runs took 3.27–3.67 s.

## Run it yourself

```sh
python3 benchmarks/run.py                                    # every workload: cee, rust, rust-checked, c; 5 runs
python3 benchmarks/run.py --workloads hash-map,nbody --runs 3
python3 benchmarks/run.py --cee-flag --no-bce                # with bounds-check elimination off
```

Each workload is the same algorithm with the same data structures in every language, and prints one checksum, which
the runner checks. `benchmarks/README.md` describes the workloads and the build flags. To see which checks ceesafec kept
in a program, build it with `--explain-bounds-checks`, `--explain-overflow-checks` or `--explain-allocations`
([The ceesafec command](ceesafec.md)).
