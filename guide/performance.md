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
faster. Measured 2026-11-12 on an Apple M-series Mac (arm64, macOS), with ceesafec 0.37.0 and ADRs 0099 to 0101,
rustc 1.99.1 and Apple clang 21. The machine was shared with other work, so a spread of several percent between runs
is normal here (the C column of `heap-alloc` and `arena` shows it): compare ratios, not single milliseconds, and
measure again before deciding anything on a small difference.

Peak memory is the same as Rust's or lower everywhere (`arena`: 3.1 MiB against 4.6 MiB). Binaries are about 94 KiB
against Rust's 460 KiB. ceesafec compiles each program in about half a second, against rustc's 0.4 s on these
single-file programs, since it runs the C compiler after its own work.

The same suite with ceesafec 0.37.0 as released took 0.98× Rust's time. What moved it:

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
