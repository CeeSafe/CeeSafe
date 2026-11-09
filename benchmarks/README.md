# Benchmarks: CeeSafe against Rust

The latest results and what they mean: [`guide/performance.md`](../guide/performance.md).

Seventeen workloads, each written in CeeSafe (`main.cee`), Rust (`main.rs`) and C (`main.c`, the baseline). Every
version implements the **same algorithm with the same data structures** and prints one checksum line, which must be
identical across languages (`run.py` checks it). The CeeSafe programs are built from the corpus,
`tests/reference/benchmarks/suite/<workload>/main.cee`, and the Rust and C ones are here,
`benchmarks/<workload>/main.rs` and `main.c`.

```sh
python3 benchmarks/run.py                                   # every workload: cee, rust, rust-checked, c; 5 runs
python3 benchmarks/run.py --workloads nbody,hash-map --runs 3
python3 benchmarks/run.py --cee-flag --no-bce --json out/bench.json
```

It needs `out/ceesafec` (or `--ceesafec <path>`), `rustc` and `clang` (`--cc`). Binaries go to `out/benchmarks/`.
Each row reports the median wall time over the runs (after one discarded run), peak RSS, heap allocations and peak
heap (counted by `allocount.c`, an interposer every language allocates through), compile time and binary size. The
last table divides each language's time by Rust's, with the geometric mean over the workloads.

| Workload | What it stresses |
|---|---|
| `small-objects`, `large-objects` | many small / large heap objects allocated and freed |
| `stack-alloc`, `heap-alloc` | a local array, and the same array on the heap each time |
| `arena` | bump allocation in 64 KiB chunks, then reset |
| `vector-growth`, `string-building` | the growable vector and string |
| `recursion` | naive fib(40): calls |
| `linked-list`, `binary-trees` | pointer-heavy heap structures |
| `hash-map` | an open-addressing table, linear probing |
| `parsing` | byte-by-byte parsing of a 2,000,000-line buffer |
| `memory-copy` | 4 KiB block copies |
| `numeric` | a sieve and an integer matrix product |
| `data-oriented` | struct-of-arrays particle updates |
| `nbody` | the Benchmarks Game's n-body, in `f64` |
| `const-flags` | a recursive scanner whose language flags are const parameters (ADR 0083): Rust's const generics; C's run-time flags |

Builds:

| Language | Build |
|---|---|
| `cee` | `ceesafec build --cc clang` (its C at `-O2`, every safety check on: bounds, overflow, narrowing) |
| `rust` | `rustc -C opt-level=2 -C codegen-units=1` (bounds checks on, overflow checks off) |
| `rust-checked` | the same with `-C overflow-checks=on`: it checks what CeeSafe checks, the fair comparison |
| `c` | `clang -O2 -std=c11 -lm` (no checks) |
