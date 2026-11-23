# Performance

CeeSafe compiles through C, with every safety check on: array bounds, integer overflow, lossy conversions. This page
measures what that costs against Rust and plain C on seventeen workloads, and says where CeeSafe is ahead or behind
and why.

## The result

Over the seventeen workloads CeeSafe takes **0.92× Rust's time** (geometric mean), and **0.85×** that of Rust built
with overflow checks, which is the comparison that checks the same things CeeSafe does. Hand-written C without any
check takes 0.95×.

| Workload | CeeSafe | Rust | Rust, overflow checks | C | CeeSafe (ms) | Rust (ms) |
|---|---:|---:|---:|---:|---:|---:|
| `heap-alloc` | **0.29** | 1.00 | 0.93 | 1.25 | 122 | 419 |
| `nbody` | **0.82** | 1.00 | 0.99 | 0.81 | 506 | 619 |
| `linked-list` | **0.84** | 1.00 | 0.99 | 0.91 | 323 | 384 |
| `stack-alloc` | **0.84** | 1.00 | 1.70 | 0.85 | 242 | 288 |
| `vector-growth` | **0.86** | 1.00 | 1.07 | 0.69 | 469 | 544 |
| `small-objects` | **0.94** | 1.00 | 1.01 | 0.89 | 462 | 494 |
| `arena` | **0.94** | 1.00 | 0.92 | 1.14 | 251 | 266 |
| `binary-trees` | 0.98 | 1.00 | 1.00 | 0.83 | 515 | 523 |
| `memory-copy` | 0.99 | 1.00 | 0.99 | 1.07 | 626 | 630 |
| `large-objects` | 1.01 | 1.00 | 1.00 | 0.97 | 262 | 260 |
| `string-building` | 1.03 | 1.00 | 1.31 | 0.78 | 297 | 289 |
| `hash-map` | 1.03 | 1.00 | 1.20 | 0.98 | 328 | 317 |
| `data-oriented` | 1.05 | 1.00 | 1.04 | 1.01 | 340 | 325 |
| `const-flags` | 1.07 | 1.00 | 1.05 | 1.11 | 2650 | 2473 |
| `numeric` | 1.08 | 1.00 | 0.99 | 1.05 | 232 | 214 |
| `parsing` | 1.17 | 1.00 | 1.11 | 1.03 | 163 | 139 |
| `recursion` | 1.26 | 1.00 | 1.27 | 1.02 | 401 | 317 |
| **geometric mean** | **0.92** | 1.00 | 1.08 | 0.95 | | |

Times are each language's median wall time over five runs (after one discarded run) divided by Rust's; lower is
faster. Measured 2026-11-12 on an Apple M-series Mac (arm64, macOS), with ceesafec 0.38.0, rustc 1.99.1 and Apple clang 21. The machine was shared with other work, so a spread of several percent between runs
is normal here (the C column of `heap-alloc` and `arena` shows it): compare ratios, not single milliseconds, and
measure again before deciding anything on a small difference.

Peak memory is the same as Rust's or lower everywhere (`arena`: 3.1 MiB against 4.6 MiB). Binaries are about 94 KiB
against Rust's 460 KiB. ceesafec compiles each program in about half a second, against rustc's 0.4 s on these
single-file programs, since it runs the C compiler after its own work.

The same suite with ceesafec 0.37.0 took 0.98× Rust's time. What moved it (ADRs 0099 to 0101):

| Workload | Before | Now | What changed |
|---|---:|---:|---|
| `binary-trees` | 1.28 | 0.98 | an allocation is `malloc` behind one test, not two calls into the runtime (ADR 0101) |
| `small-objects` | 1.16 | 0.94 | the same |
| `linked-list` | 1.13 | 0.84 | the same |
| `hash-map` | 1.36 | 1.03 | the tables are made with `resize`, which takes zeroed memory (ADR 0099), not 8 million `push` calls |
| `memory-copy` | 1.16 | 0.99 | the buffers are made with `resize`, not 32 million `push` calls |
| `data-oriented` | 1.18 | 1.05 | the arrays are made with `resize`, whose length ceesafec then knows: no index check in the loop (ADR 0100) |
| `numeric` | 1.11 | 1.08 | the sieve is made with `resize`; its index checks are removed |

In the last four the CeeSafe program changed as well as the compiler: it now makes its arrays filled, as the Rust
(`vec![0; n]`) and C (`calloc`) versions always did.

## Where CeeSafe is ahead

- **`heap-alloc`, 3.4× faster than Rust.** ceesafec's placement optimisation keeps a box that never escapes its
  function on the stack, so the program's per-iteration heap array costs no allocation at all. It beats hand-written C,
  which calls `malloc` and `free` each time.
- **`stack-alloc`, `nbody`, `vector-growth`.** CeeSafe matches C. Its overflow and bounds checks are removed where
  ceesafec proves they cannot fail (loop indexes within a length, sums of bounded values), which Rust with overflow
  checks does not manage on `stack-alloc` (1.70×).
- **`linked-list`, `small-objects`.** A box is allocated and freed by the C library directly: in a program that did
  not ask for the allocation report, the runtime's part is one load and an untaken branch (ADR 0101).
- **`arena`.** A region's bump allocation is inlined into the program (Decision 0007), and the arena's chunks take less
  memory than Rust's.

## Where CeeSafe is behind, and why

- **`recursion` (1.26×) and `parsing` (1.17×).** The overflow check on `fib(n - 1) + fib(n - 2)` and on accumulating
  digits cannot be proven unnecessary; Rust pays the same when its overflow checks are on (1.27× and 1.11×). CeeSafe
  always checks; Rust's release builds wrap.
- **`hash-map` (1.03×).** What is left is the overflow check on the hash's multiplication: Rust with overflow checks
  takes 1.20×. Its index checks are kept, in `find`, which sees the table through a borrow, and cost nothing
  measurable.
- **`data-oriented` (1.05×).** Its overflow checks: the position and velocity sums are of values read from the
  arrays, so they stay (15 of 23), and with them compiled out the program runs at Rust's speed. Rust with overflow
  checks takes 1.04×.
- **`numeric` (1.08×).** Partly established. It keeps the four index checks of its matrix product (`a[i * M + k]`:
  nothing bounds a product) and 11 overflow checks; compiling either kind out gains about 3%, and the rest is
  within the spread.
- **`const-flags` (1.07×).** See below: within the spread of this machine.

## Making a filled vector

A vector that starts filled is made with `resize`: `var v = Vector<u64>.create(); v.resize(n, 0)?;`. When the vector
has no buffer and the fill value is all zero bits, the memory comes from the allocator already zeroed, which for a
large table is memory the operating system has not touched yet (ADR 0099). After the `?` (or `!`), ceesafec knows
that `v.length` is `n`, so `for (i in 0..n) { v[i] = …; }` has no index check (ADR 0100). A loop of `push` calls
costs a call and a capacity test for each element, and leaves the length unknown to the compiler.

## Const parameters

`const-flags` (ADR 0083 §1.3) is a recursive scanner over 10 MB of pseudo-random bytes, run 100 times, with three
flag tests per byte; its flags are const parameters (`fn scan<const TS: bool, const JSX: bool>`), so each instance
keeps only its own tests. Its row in the table above is the first line below, measured again. The comparison with
run-time flags was measured 2026-11-09 on the same kind of machine (Apple arm64, Apple clang 21, rustc 1.99.1,
ceesafec 0.26.0-dev), on a machine shared with other work, so the spread is wider than usual:

| Version | Time |
|---|---:|
| CeeSafe, const parameters (`main.cee`) | 1.05× Rust (2.67 s against 2.55 s, `run.py`, median of 5) |
| Rust, const generics (`main.rs`) | 1.00 |
| CeeSafe, the same flags as `bool` parameters (`runtime.cee`) | 1.19× CeeSafe's const version (3.68 s against 3.10 s user time, 3 runs each) |

At `-O2` clang does not clone a recursive function for a constant argument, so a run-time flag keeps every test;
the const parameter removes them, as Rust's const generics do. `main.c` takes the flags as arguments (C has no const
parameters), and in the same three runs took 3.27–3.67 s.

## A call through `dyn`

One measurement of what a method call costs in each of the three ways to write code for several kinds of value: a
generic with a bound, an enum with `match`, and `dyn` ([Generics](generics.md)). The programs are the book's
`book/projects/ch18/cost-generic`, `cost-enum` and `cost-dyn`: 1,000 shapes, squares and rectangles in an order a
small random number generator chooses, whose `area` (one multiplication) is summed 200,000 times, so 200 million
calls. Measured 2026-11-13 on an Apple M1 with Apple clang 21 and ceesafec 0.40.0, `ceesafec build`, the lowest
wall-clock time of the whole program in 30 runs of each, on a machine shared with other work:

| Way | Time | For each call |
|---|---:|---:|
| generic: a `Vector<Square>` and a `Vector<Rect>`, each summed by its own instance | 107 ms | 0.5 ns |
| enum: a `Vector<Shape>`, `area` a `match` | 215 ms | 1.1 ns |
| `dyn`: a `Vector<Box<dyn Shape>>` | 908 ms | 4.5 ns |

The three print the same sum. This is the smallest possible method, so it shows the cost of the call alone at its
largest: a method that does real work hides it. It is one machine and one workload, not a rule.

## An `OrderedMap` loaded in key order

`OrderedMap` is a B-tree whose nodes hold up to 31 entries. A put past the last key or before the first splits the
full nodes on its path 29 entries to 1, the one on the key's side, not in the middle (ADR 0194 §2.4), so the nodes a
load in key order leaves behind are nearly full, not half empty. Measured 2026-11-21 on an Apple M1 with Apple clang
21, `ceesafec build`, 1M puts of a `u64` key and a `[u8; 64]` value, peak footprint by `/usr/bin/time -l`, times the
best of three on a machine shared with other work:

| 1M puts of `u64` → `[u8; 64]` | peak footprint | 1M puts | 1M lookups after them |
|---|---:|---:|---:|
| `OrderedMap`, keys ascending or descending, middle splits only (0.85.1) | 184 MB | 168 ms | 101 ms |
| `OrderedMap`, keys ascending or descending, edge splits | **97 MB** | **127 ms** | 106 ms |
| `OrderedMap`, keys scrambled (`i * 2654435761 % 4294967311`), either | 128 MB | 0.9 to 1.0 s | 0.37 to 0.54 s |
| `HashMap`, for comparison | 74 MB | | |

(The times are for ascending keys.) A map built from a sorted source, such as a checkpoint, a snapshot or an import,
takes about 1.3 times a `HashMap`'s memory instead of 2.5 times.

## Deflate, zlib and gzip

lib/std's codecs (ADR 0196) are pure CeeSafe; `benchmarks/codecs/run.py` compares them with the system's zlib, called
from C, on the same machine. Measured 2026-11-21 on an Apple M1 with Apple clang, `ceesafec build`, zlib 1.2.12, on
32 MiB inputs: an inventory-shaped CSV with a random 128-bit hex column, a column of 64-bit timestamps, and text. Both
decoders read the streams zlib wrote at each level; MB/s of uncompressed data, the best of three, on a machine shared
with other work (load average 4 to 6):

| input | level | zlib size | lib/std size | inflate zlib | inflate lib/std | deflate zlib | deflate lib/std |
|---|---:|---:|---:|---:|---:|---:|---:|
| CSV | 1 | 12,770,130 | 13,108,634 | 903 | 341 | 138 | 166 |
| CSV | 6 | 11,474,314 | 11,491,975 | 680 | 269 | 45 | 39 |
| CSV | 9 | 11,339,843 | 11,357,561 | 726 | 281 | 26 | 20 |
| integers | 1 | 11,442,696 | 11,460,690 | 732 | 325 | 134 | 147 |
| integers | 6 | 10,518,876 | 10,535,653 | 767 | 338 | 33 | 31 |
| integers | 9 | 10,544,785 | 10,563,024 | 757 | 332 | 4 | 3 |
| text | 1 | 8,264,300 | 8,640,522 | 1,093 | 315 | 171 | 190 |
| text | 6 | 5,937,534 | 5,969,432 | 1,778 | 477 | 28 | 26 |
| text | 9 | 5,713,119 | 5,745,255 | 1,795 | 474 | 10 | 8 |

lib/std's decoder runs at 27 to 45% of zlib's speed (ADR 0196 §4.4's target is half), above 300 MB/s everywhere but
the CSV at levels 6 and 9 (270 to 280); its encoder is faster than zlib's at level 1, where it probes the hash once,
and 75 to 95% of zlib's speed above, with output within 0.6% of zlib's size at levels 6 and 9 and up to 4.6% larger
at level 1. Of the decoder's time, a profile gives about a quarter to the copies of matches and a sixth to zlib's
Adler-32 (1.8 GB/s alone), which raw deflate and gzip skip.

## Snappy

lib/std's `Snappy` (ADR 0196 phase 2), against the system's libsnappy 1.2 called from C (`benchmarks/codecs/snappy.c`),
on the same three 32 MiB inputs. Both decoders read libsnappy's block of each input; MB/s of uncompressed data, the best
of three, measured 2026-11-23 on the Apple M1 with Apple clang, `ceesafec build`, on a machine shared with other work
(load average 28 to 32):

| input | libsnappy size | lib/std size | decode libsnappy | decode lib/std | encode libsnappy | encode lib/std |
|---|---:|---:|---:|---:|---:|---:|
| CSV | 18,336,266 | 18,519,478 | 2,933 | 1,187 | 753 | 510 |
| integers | 18,634,155 | 18,635,253 | 1,582 | 1,324 | 693 | 580 |
| text | 10,517,475 | 10,522,728 | 2,655 | 1,072 | 938 | 707 |

lib/std's decoder runs at 1,070 to 1,320 MB/s (ADR 0196 §4.4's target is 1,000), 40 to 84% of libsnappy's: it sizes
the output once from the preamble, copies a match eight bytes at a time and a literal of up to 64 bytes a word at a
time, and checks each element's bounds, where libsnappy's fast path reads ahead without them. Its encoder, libsnappy's
algorithm, runs at 68 to 84% of libsnappy's speed, with output within 1% of its size (the same bytes on most inputs of
`tools/codec_check.py`).

## Zstandard

lib/std's `Zstd.decompress` (ADR 0196 phase 3; decoding only), against the system's libzstd 1.5.7 called from C
(`benchmarks/codecs/zstd.c`, `ZSTD_decompressDCtx`), on the same three 32 MiB inputs. Both decoders read libzstd's
frame of each input at levels 1, 3 and 19. Figures are MB/s of uncompressed data, the best of three, measured 2026-11-23
on the Apple M1 with Apple clang, `ceesafec build`, on a machine shared with other work (load average 7 to 9):

| input | level | size | decode libzstd | decode lib/std | lib/std / libzstd |
|---|---:|---:|---:|---:|---:|
| CSV | 1 | 10,484,267 | 1,528 | 614 | 40% |
| CSV | 3 | 10,958,702 | 1,401 | 529 | 38% |
| CSV | 19 | 9,706,712 | 1,163 | 516 | 44% |
| integers | 1 | 8,732,993 | 1,123 | 516 | 46% |
| integers | 3 | 8,732,322 | 1,191 | 545 | 46% |
| integers | 19 | 8,728,417 | 1,166 | 529 | 45% |
| text | 1 | 7,192,671 | 1,177 | 447 | 38% |
| text | 3 | 6,940,874 | 1,352 | 499 | 37% |
| text | 19 | 4,550,212 | 2,156 | 823 | 38% |

lib/std's decoder runs at 450 to 820 MB/s, 37 to 46% of libzstd's. ADR 0196 §4.4's target is 500 MB/s, met on seven
of the nine inputs (under a load average of 70 to 145 the same runs gave 280 to 525). It decodes the four Huffman
streams of a block's literals in turn and runs each sequence as it decodes it, with the bit reader in locals: clang
does not inline the reader's methods into the large block function, and keeping them out of line cost a third. Its
other costs are the bounds checks of each table lookup and copy, and the checked shifts. libzstd's loops have neither, and prefetch matches.

## Run it yourself

```sh
python3 benchmarks/run.py                                    # every workload: cee, rust, rust-checked, c; 5 runs
python3 benchmarks/run.py --workloads hash-map,nbody --runs 3
python3 benchmarks/run.py --cee-flag --no-bce                # with bounds-check elimination off
python3 benchmarks/codecs/run.py                             # deflate, zlib and gzip against the system's zlib, then
                                                             # snappy and zstd against libsnappy and libzstd
```

Each workload is the same algorithm with the same data structures in every language, and prints one checksum, which
the runner checks. `benchmarks/README.md` describes the workloads and the build flags. To see which checks ceesafec kept
in a program, build it with `--explain-bounds-checks`, `--explain-overflow-checks` or `--explain-allocations`
([The ceesafec command](ceesafec.md)).
