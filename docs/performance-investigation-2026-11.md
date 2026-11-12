# Performance against Rust: findings of 2026-11-11, for a later session

**Status:** an investigation, not a plan that anyone accepted. §9 records what a session of 2026-11-12 implemented
from it (ADRs 0099 to 0101, released in ceesafec 0.38.0) and what it measured and left; §1 to §8 are
as written on 2026-11-11. Each finding says whether
it was **measured**, **read in the source** or is a **hypothesis**. Written so that a session with no other context
can pick an item, reproduce its number and fix it.

**Why it was done.** The blog post `docs/blog/porting-rust-to-ceesafe.md` reports two ports of Rust code (Bun and
tsrs) in which CeeSafe was level with Rust or behind it, never ahead. The owner asked how CeeSafe could be made to
outperform Rust.

**The short answer.** CeeSafe 0.35.0 is level with Rust on its own benchmark suite (0.99× Rust's time, geometric
mean of 17 workloads). Both compile through LLVM, so equal code gives equal speed; CeeSafe can get ahead only where
it knows something LLVM does not (its own check elimination, placement of boxes on the stack) or where the Rust
does avoidable work. It is behind on nine workloads, and most of those have a specific cause.

## 1. The machine and the tools

- Apple M-series MacBook Pro (arm64, macOS), Apple clang 21, rustc 1.99.1, ceesafec 0.35.0 (commit `8fa5921`).
- **The machine was shared with other work the whole time** (load average 4 to 12). Every time below is the lowest
  of 5 to 8 runs. Differences under about 5% are noise; §4.2 has one run where the noise was larger than that.
  Re-measure on a quiet machine before deciding anything on a small difference.
- `ceesafec build` compiles the generated C with `-O2` (`src/host/build.cee:300`, `src/host/toolchain.cee:146`).

## 2. Where CeeSafe stands: the benchmark suite with 0.35.0

`python3 benchmarks/run.py --runs 5` (needs `out/ceesafec`, `rustc`, `clang`; `benchmarks/README.md`). Each number is
the language's median wall time divided by Rust's; lower is faster. `rust-checked` is Rust built with overflow
checks, which is what CeeSafe always does.

| Workload | CeeSafe | Rust | Rust, overflow checks | C |
|---|---:|---:|---:|---:|
| heap-alloc | 0.29 | 1.00 | 0.93 | 0.97 |
| nbody | 0.80 | 1.00 | 0.99 | 0.80 |
| stack-alloc | 0.85 | 1.00 | 1.73 | 0.90 |
| vector-growth | 0.87 | 1.00 | 1.12 | 0.93 |
| arena | 0.92 | 1.00 | 0.91 | 1.03 |
| string-building | 1.01 | 1.00 | 1.29 | 0.76 |
| large-objects | 1.02 | 1.00 | 1.06 | 1.03 |
| const-flags | 1.05 | 1.00 | 1.04 | 1.13 |
| memory-copy | 1.09 | 1.00 | 1.00 | 1.00 |
| numeric | 1.12 | 1.00 | 1.00 | 1.05 |
| parsing | 1.13 | 1.00 | 1.10 | 1.05 |
| linked-list | 1.13 | 1.00 | 0.97 | 0.88 |
| small-objects | 1.15 | 1.00 | 0.99 | 0.86 |
| data-oriented | 1.15 | 1.00 | 1.04 | 0.97 |
| binary-trees | 1.23 | 1.00 | 1.02 | 0.91 |
| recursion | 1.28 | 1.00 | 1.24 | 0.98 |
| hash-map | 1.43 | 1.00 | 1.19 | 0.99 |
| **geometric mean** | **0.99** | 1.00 | 1.08 | 0.95 |

`guide/performance.md` has the same table from ceesafec 0.2.0-dev (2026-10-31): 0.96 overall. The order of the
workloads is the same now; several of its explanations are out of date (§5).

## 3. What the two ports measured

From `~/dev/CeeSafeApps/bun/analysis/trials/` (the public repository CeeSafe/CeeBun), reports `wyhash/REPORT.md` and
`tsrs/REPORT.md`:

- **Bun's wyhash, one function:** safe CeeSafe equals Bun's Rust from 1 KB up and is 12 to 16% slower on keys of 8
  to 32 bytes, where four reads at length-dependent offsets keep their bounds checks. With unchecked reads, as the
  Rust has, 2 to 5% slower.
- **tsrs, inside CeeSafe:** building the span map's index (a sort by three keys and a tree) equals Rust.
- **tsrs, across the boundary:** each small call from Rust into CeeSafe is 13 to 40% slower than the Rust it
  replaced, because the Rust was inlined by link-time optimisation and the CeeSafe archive is not part of rustc's
  LTO (§4.6).
- **tsrs, building a map of 2,000 segments:** about twice as slow, cause not established then. §4.2 measured the sort
  alone since: it is not the sort.

## 4. Findings

### 4.1 hash-map (1.43): the fill, not the bounds checks — measured

`tests/reference/benchmarks/suite/hash-map/main.cee` fills two tables of 4,194,304 `u64` with `push` in a loop, then
probes them. `guide/performance.md` says the gap is the bounds check on `keys[s & MASK]`. It is not:

| Build | Time |
|---|---:|
| Rust (`benchmarks/hash-map/main.rs`, `-C opt-level=3`) | 0.27 s |
| CeeSafe as it is | 0.39 s |
| CeeSafe, every bounds check compiled out (§6.2) | 0.40 s |
| CeeSafe, the tables made with `keys.resize(4194304, 0)!` in place of the `push` loop | 0.31 s |

- All six index checks of the program are kept (`--explain-bounds-checks`), with `push` or with `resize`, and they
  cost nothing measurable.
- The cost is 8 million `push` calls and touching every page. Rust's `vec![0; n]` asks the allocator for zeroed
  memory, which for 32 MB comes as untouched pages.
- **Hypothesis for the rest (0.31 against 0.27):** `resize` writes the fill value into memory it got from
  `ceesafe_realloc`; it does not use `ceesafe_alloc_zeroed`. Not verified in the emitted helper.

**To do.** Look at the helper `ceeVZ_` (`src/backend/c_helpers.cee:1724` and `:2042`). For a fresh vector and a fill
value that is all zero bits, allocate with `ceesafe_alloc_zeroed` (`runtime/core/ceesafe_rt_core.c:470`). Consider a
constructor that says it (`Vector<T>.filled(n, value)`), which the guide already proposed. Then change the benchmark
to use it, as the Rust and C versions do, and re-measure `hash-map` and `data-oriented` (1.15, the same pattern).

### 4.2 Check elimination is where CeeSafe can pass safe Rust — measured, with noise

A standalone benchmark of Go's pdqsort as ported for tsrs (§6.1): 20,000 sorts of 2,000 24-byte structs by one
`u32` key. The Rust is tsrs's `goslices.rs`, safe code with slice indexing.

| Build | Time (two rounds, lowest of 8 each) |
|---|---:|
| Rust, `-C opt-level=3` | 0.55 s, 0.65 s |
| CeeSafe, checks on, `-O2` | 0.62 s, 0.66 s |
| CeeSafe, checks on, `-O3` | 0.61 s, 0.71 s |
| CeeSafe, overflow checks compiled out | 0.60 s, 0.55 s |
| CeeSafe, bounds checks compiled out | 1.08 s, 1.09 s |
| CeeSafe, both compiled out, `-O2` | 0.46 s, 0.50 s |
| CeeSafe, both compiled out, `-O3` | 0.57 s, 0.56 s |

- **With no checks the CeeSafe is 15 to 25% faster than the Rust,** which keeps its own bounds checks. With its
  checks on it is level or slightly behind. The spread of the Rust row (0.55 to 0.65) shows how noisy the machine
  was; the direction held in both rounds.
- **Not explained:** removing only the bounds checks makes it much slower (1.08 s, stable). Probably a code
  generation effect in the partition loops (branches turned into conditional moves, or the reverse). Worth
  understanding before trusting any single row here: disassemble `partition` in both builds.
- `-O3` was not better than `-O2`. No case for changing the default.

**What the compiler keeps today** (`--explain-bounds-checks`, `--explain-overflow-checks`, on the tsrs packages
`~/dev/CeeSafeApps/tsrs-trial/ceesafe/`): 100 of 123 index checks and 130 of 195 overflow checks; in the sort, 39 of
47 and 56 of 83. Two that are provable and kept, in the span map's `segmentIndexAt`
(`ceesafe/tsrs_spanmap/src/lib.cee`, about line 402):

```ceesafe
var lo: usize = 0;
var hi = segments.length;
while (lo < hi) {
    let middle = lo + (hi - lo) / 2;          // overflow check on `lo + …` kept
    if (segments[middle].virtualStart < pos) { // index check kept: lo <= middle < hi <= length
        lo = middle + 1;
    } else {
        hi = middle;
    }
}
let idx = lo;
…
if (idx == 0) {
    return …;
}
let prev = idx - 1;                            // overflow check kept, after `idx == 0` returned
```

**To do.** In `src/backend/check_elimination.cee` (1,793 lines; rules in `spec/optimisations.md` §2 and §3):

1. A fact from a branch that leaves: after `if (x == 0) { return …; }`, `x >= 1`, so `x - 1` cannot overflow.
2. The binary-search invariant: with `lo <= hi <= length` kept by the loop, `lo < hi` gives
   `lo + (hi - lo) / 2 < hi`, so the index is in range and the sum cannot overflow. This is a relation between two
   locals, which the analysis may not track today (`optimisations.md` §2.3 says what it tracks).
3. A masked index against a length known from construction (`s & MASK` with a table of `MASK + 1` elements).
4. A callee's index against a slice whose length only the caller knows (`insertionSort(data, a, b)` with
   `b <= data.length`): either facts across calls, or the source pattern of slicing once (`data[a..b]`) so the
   length is local. Bun's wyhash got its large-input parity exactly that way (one 48-byte slice per round).

Use the benchmark of §6.1 and the suite as gauges, and the counts above as the before figures. Every new rule needs
the soundness argument and tests `optimisations.md` §1 asks for.

### 4.3 Overflow checks: a cost Rust's release builds do not pay — measured

`recursion` is 1.28 for CeeSafe and 1.24 for Rust with overflow checks; `parsing` 1.13 and 1.10. CeeSafe checks
always; Rust's release default wraps. So part of every comparison with default Rust is this difference in what is
checked, not in how well it is compiled.

- More elimination (§4.2) removes the checks that are provable. `fib(n - 1) + fib(n - 2)` is not.
- **A decision for the owner, not taken:** whether to offer wrapping arithmetic as an opt-in (per package or per
  function), as Rust's release profile does by default. It would close `recursion`, `parsing` and probably
  `numeric`, and give up the overflow guarantee where it is used. `wrappingAdd` and friends exist today for single
  operations.

### 4.4 Exclusive borrows carry no aliasing information — read in the source, not measured

`spec/c-backend.md` (the type table, about line 162) says `borrow mut T` becomes `T *`, with `restrict` "only where
the borrow checker proves exclusivity and M9 enables it". The word `restrict` does not occur in `src/backend/`: it is
never emitted. Rust marks every `&mut` and `&` parameter `noalias` for LLVM, which lets it keep values in registers
across stores and vectorise loops that read one slice and write another.

**To do.** Emit `restrict` on parameters that are exclusive borrows and on `SliceMut` data pointers where the
borrow checker's rules make it true, and measure `memory-copy` (1.09), `numeric` (1.12) and `nbody`. Be careful
with what C's `restrict` promises (C11 §6.7.3.1): it is about the whole lifetime of the parameter in the block, and
a wrong `restrict` is undefined behaviour that no test may catch. Reborrows, two-phase `mut` arguments, and raw
pointers made from the borrow need a written argument. Rust itself had `noalias` off for years because of LLVM
bugs; it is on since 1.54.

### 4.5 Allocation-heavy workloads — the guide's cause looks out of date; not measured

`binary-trees` 1.23, `small-objects` 1.15, `linked-list` 1.13. `guide/performance.md` says every allocation goes
through `ceesafe_alloc`, "which counts allocations", and that calling `malloc` directly measured 12% faster.

Read in the source (`runtime/core/ceesafe_rt_core.c:418–468`): counting is now conditional on a configuration word
(`config_u32(&alloc_counting) != 0u`), so a program that did not ask for the report does not count. What remains
on every allocation is a call into `ceesafe_rt_core.c` that is not inlined, two or three loads of configuration
words, and a branch for the failing allocator.

**To do.** Measure first: build `binary-trees` with the generated C patched to call `malloc`/`free` directly (the
method of §6.2) and see whether 12% is still there. If it is, make the non-counting path a static inline in `rt.h`
that tests one word and calls the platform allocator, as the arena's bump allocation was inlined (Decision 0007).
Do not start from the guide's sentence: it describes an older runtime.

### 4.6 Hybrid builds pay at the boundary — measured in the tsrs trial

In `tsrs/REPORT.md` §3 each lookup that crosses from Rust into CeeSafe costs 6 to 17 ns more than the inlined Rust,
and a list-returning call allocates on both sides. This is a property of the build, not of the language: the
CeeSafe archive holds native objects.

**To do,** if hybrid builds matter: cross-language LTO. The Bun trial already links CeeSafe as ThinLTO bitcode,
because Bun builds its Rust with a clang of the same LLVM (`ceebun-trial/scripts/build/ceesafe.ts`). For a cargo
build it means compiling the generated C with `-flto` by a clang whose LLVM matches rustc's, and
`-C linker-plugin-lto`. tsrs's release profile is fat LTO, so the pieces are there; the trial's
`crates/tsrs_ceesafe/build.rs` does not do it.

### 4.7 Not investigated

`numeric` (1.12), `memory-copy` (1.09), `parsing` beyond its overflow checks, `data-oriented` beyond the fill.
Start each with `--explain-bounds-checks` and `--explain-overflow-checks`, then the method of §6.2.

## 5. Corrections owed to `guide/performance.md`

- Its table is ceesafec 0.2.0-dev; §2 above is 0.35.0.
- **hash-map:** the stated cause (bounds checks from an unknown length) is wrong for the current compiler (§4.1).
- **"A CeeSafe `Vector` can only be filled by `push`"** is no longer true: `v.resize(n, fill)` exists.
- **Allocation counting** (§4.5): the runtime now counts only when asked.

Any change to the guide needs `tools/gen_docs.py`, a rebuilt compiler and `tools/spec_check.py`, and its examples
are compiled by `tools/guide_check.py`.

## 6. How to reproduce

### 6.1 The sort benchmark

The CeeSafe side is a package that depends on the trial's port of the sort
(`~/dev/CeeSafeApps/tsrs-trial/ceesafe/tsrs_core`, `src/goslices.cee`); the Rust side is one file beside a copy of
`~/dev/CeeSafeApps/tsrs-trial/crates/tsrs_core/src/goslices.rs`. Both print `237213930`.

`sortbench/ceesafe.toml`:

```toml
[package]
name = "sortbench"
version = "0.1.0"
kind = "executable"

[dependencies]
tsrs_core = { path = "/Users/kutty/dev/CeeSafeApps/tsrs-trial/ceesafe/tsrs_core" }
```

`sortbench/src/main.cee`:

```ceesafe
struct Segment {
    virtualStart: u32;
    virtualEnd: u32;
    originalStart: u32;
    originalEnd: u32;
    kind: i32;
    features: u32;
}

struct ByVirtualStart {
    unused: u8;
}

impl tsrs_core.Order<Segment> for ByVirtualStart {
    public fn less(a: Segment, b: Segment) -> bool => a.virtualStart < b.virtualStart;
}

fn next(state: borrow mut u64) -> u64 {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

fn main() {
    var state: u64 = 0x2545F4914F6CDD1D;
    var source = Vector<Segment>.create();
    for (i in 0..2000) {
        let start = truncate<u32>(next(mut state) % 8000);
        source.push(Segment {
            virtualStart: start,
            virtualEnd: start + 3,
            originalStart: truncate<u32>(next(mut state) % 8000),
            originalEnd: 0,
            kind: 0,
            features: 1,
        })!;
    }
    var work = Vector<Segment>.withCapacity(2000)!;
    var checksum: u64 = 0;
    for (round in 0..20000) {
        work.clear();
        work.extend(source.view())!;
        tsrs_core.sortFunc<Segment, ByVirtualStart>(work.viewMut());
        checksum = checksum.wrappingAdd((work[round % 2000].originalStart as u64) + (work[1999].virtualStart as u64));
    }
    Console.writeLine(checksum);
}
```

The Rust `main.rs` is the same program: `mod goslices;`, a `#[derive(Clone, Copy)] struct Segment` with the same six
fields, the same xorshift, `work.extend_from_slice(&source)`, and
`goslices::sort_func(&mut work, |a, b| a.virtual_start.cmp(&b.virtual_start))`. Built with
`rustc -C opt-level=3 -C codegen-units=1 main.rs`.

```sh
cd sortbench && ceesafec fetch --offline && ceesafec build -o ../sort_default
ceesafec build --emit c -o ../sortc          # the C, for the variants of §6.2
```

### 6.2 Compiling the checks out, to see what they cost

There is no compiler switch for it (and there should not be one for users). For a measurement, emit the C and edit
its copy of the runtime header:

```sh
ceesafec build --emit c -o work main.cee     # writes work/main.c, work/ceesafe/rt.h and the runtime's C files
```

In `work/ceesafe/rt.h`:

- **bounds:** in `cee_check_index`, replace the body `if (index >= length) ceesafe_panic(CEE_PANIC_INDEX, *loc);`
  with `(void)index; (void)length; (void)loc;`;
- **overflow:** in the `CEE_ARITH` macro, replace each
  `if (__builtin_add_overflow(a, b, &r)) ceesafe_panic(CEE_PANIC_OVERFLOW, *loc);` (and the `sub` and `mul` forms)
  with `(void)__builtin_add_overflow(a, b, &r); (void)loc;`.

Then `cc -O2 -I work work/*.c -o program`. Compare against the same directory unedited, built the same way.

### 6.3 Which checks were kept

```sh
ceesafec build main.cee --explain-bounds-checks -o /dev/null     # one line per index check: kept or removed
ceesafec build main.cee --explain-overflow-checks -o /dev/null
```

For a package with dependencies, run it in the package (`ceesafec build --emit lib --offline --explain-… -o x.a`).

## 7. A suggested order

1. **§4.1, the fill** (zeroed allocation in `resize`, perhaps `Vector.filled`): the clearest evidence, a small
   change, two workloads.
2. **§4.5, measure the allocation path**, and inline it if the cost is still there: three workloads.
3. **§4.2, check elimination**, starting with the early-return fact and the binary search: the only item that can
   put CeeSafe ahead of safe Rust, and the largest piece of work.
4. **§4.4, `restrict`**: unmeasured, and the one with a soundness risk.
5. **§5, the guide**, once the numbers have moved.
6. **§4.6** only if hybrid builds are a goal; **§4.3** only if the owner decides for a wrapping mode.

Each compiler or library change goes through an ADR in `spec/decisions/`, as usual. The gauge for all of them is
§2's table: rerun `benchmarks/run.py` before and after, on a quiet machine.

## 8. What not to expect

Parity is the ceiling for code that is the same in both languages. The items above remove costs CeeSafe has that
Rust does not (the fill, the allocator call, missing `restrict`) and add knowledge Rust does not have (check
elimination). They can move the mean below 1.00 and turn several of the "behind" rows; they cannot make CeeSafe
faster than Rust on everything.

## 9. What was done on 2026-11-12

Same machine and tools as §1, ceesafec 0.37.0 (commit `4f0ed22`) as the base. The machine was shared again (load 2
to 7). `python3 benchmarks/run.py --runs 5`, before and after, each language's median over Rust's:

| Workload | 0.37.0 | With ADRs 0099–0101 | Item |
|---|---:|---:|---|
| binary-trees | 1.28 | 0.98 | §4.5 |
| small-objects | 1.16 | 0.94 | §4.5 |
| linked-list | 1.13 | 0.84 | §4.5 |
| hash-map | 1.36 | 1.03 | §4.1 |
| memory-copy | 1.16 | 0.99 | §4.1's pattern |
| data-oriented | 1.18 | 1.05 | §4.1's pattern and §4.2 |
| numeric | 1.11 | 1.08 | §4.1's pattern and §4.2 |
| **geometric mean, 17 workloads** | **0.98** | **0.92** | |

The other ten rows moved by less than the spread. Against Rust with overflow checks the mean is 0.85.
`guide/performance.md` has the whole table and is corrected as §5 asked.

### 9.1 §4.1, the fill — done (ADR 0099)

- The hypothesis was right: `resize` grew the buffer and then wrote the fill. The helper now takes
  `ceesafe_alloc_zeroed` for a vector with no buffer and a fill of zero bytes (tested at run time on the value's
  bytes, so `-0.0` and a struct with a non-zero field keep the loop).
- That alone is small (the pages are touched by the probes anyway). The larger part of `hash-map`'s gap was the 8
  million `push` calls, which §4.1 measured: the benchmark now uses `resize`, as the Rust and C use `vec!` and
  `calloc`. `memory-copy`, `data-oriented` and `numeric` had the same `push` loops and were changed the same way.
- `Vector<T>.filled(n, value)` was not added: `create` and `resize` now cost what it would.
- **What is left of `hash-map` (1.03):** the overflow check on the hash's multiplication. Rust with overflow checks
  takes 1.20. Measured with the checks compiled out (§6.2): no overflow checks 0.423 s against 0.429 s and Rust's
  0.381 s under load, so not all of it; within the spread on a quiet run.

### 9.2 §4.5, the allocation path — measured, then done (ADR 0101)

Measured first, as asked: the generated C of each workload built twice, lowest of 7 runs.

| Workload | Through the runtime | `malloc`/`free` directly | One word tested, then `malloc`/`free` |
|---|---:|---:|---:|
| binary-trees | 0.658 s | 0.512 s | 0.523 s |
| small-objects | 0.594 s | 0.475 s | 0.474 s |
| linked-list | 0.423 s | 0.336 s | 0.345 s |

About 20%, more than the guide's old 12%. `__attribute__((malloc))` on the declaration alone gained nothing. The
runtime now has the one-word test inline in `rt.h`; a counted run, an injected failure and a host's allocator still
see every allocation. The allocation counts of the three workloads under `benchmarks/allocount.c` are unchanged
(the C compiler removed no pair).

### 9.3 §4.2, check elimination — items 1 and 2 done, item 3 in part (ADR 0100)

- **Item 1** (a fact from a branch that leaves): `==` and `!=` give facts; `n ≠ 0` moves an unsigned bound to 1.
- **Item 2** (the binary search): `/` is a definition; half of a value that is at least 1 is less than it; what
  the temporaries of `lo + (hi - lo) / 2` derived is kept when they end; the join keeps `hi ≤ length` across
  `hi = middle`. The three checks of §4.2's example are removed (`tests/corpus/bounds-checks/search.cee`,
  `overflow-checks/tested.cee`).
- **Item 3, in part:** after `v.resize(n, fill)?` (or `!`), `v.length` is `n`. That removed the four index checks
  per iteration of `data-oriented`, which cost 11% (measured with the checks compiled out, 0.400 s against 0.355 s).
  A masked index in another function (`find` in `hash-map`) is not covered: it needs facts across calls.
- **Item 4:** not done.
- **A finding on the way:** the budget of 128 facts counted implications, which no proof reads. With more
  comparisons kept, 51 checks in the compiler's own sources that 0.37.0 removed were kept. The budget now counts
  comparisons and definitions only.
- **Counts** (`--explain-bounds-checks`, `--explain-overflow-checks`, 0.37.0 against this tree) are in ROADMAP §10.
  The tsrs packages of §4.2 were not re-measured: do that with §6.3 and compare with "100 of 123" and "130 of 195".
- **Not explained, still:** §4.2's row where compiling the bounds checks out made the sort slower. The same shape
  showed in `hash-map` (0.429 s with checks, 0.592 s without) and slightly in `numeric`. Disassemble before
  trusting a "checks compiled out" row.
- Compile time: the compiler emitting its own C, 8.1 s before and 8.45 s after.

### 9.4 §4.4, `restrict` — measured by hand, not implemented

Only two workloads of the suite pass an exclusive borrow to a function: `const-flags` (`s: borrow mut Scan`) and
`nbody` (`SliceMut<Body>`). `memory-copy` and `numeric`, which §4.4 names, do all their work in `main`. With
`restrict` written into the generated C by hand (lowest of 5):

| Workload | As generated | With `restrict` |
|---|---:|---:|
| const-flags (the `Scan *` parameter) | 2.666 s | 2.663 s |
| nbody (the `ptr` member of the `SliceMut` struct) | 0.533 s | 0.514 s |

Nothing on `const-flags`; 3.5% on `nbody`, inside the spread of that run, and `nbody` is already at C's speed.
With no measured gain and the soundness risk §4.4 describes, it was left. A workload that reads one slice and writes
another in a called function would be the place to look again.

### 9.5 §4.7, the workloads not investigated

- **memory-copy:** the fill (two `push` calls for each of 16,777,216 bytes). Now 0.99.
- **data-oriented:** the fill and the index checks (§9.3). What is left (1.05) is its overflow checks: with them
  compiled out, 0.326 s against 0.339 s and Rust's 0.328 s. Rust with overflow checks takes 1.04.
- **numeric (1.08):** partly. Four index checks of the matrix product (`a[i * M + k]`: nothing bounds a product)
  and 11 overflow checks stay; compiling either out gains about 3% (0.234 s, 0.228 s, 0.226 s; Rust 0.212 s).
- **parsing (1.17):** not looked at beyond §4.3. Compiling the overflow checks out brought it to Rust's time
  (0.143 s against 0.171 s; Rust 0.145 s), so it is §4.3's cost.

### 9.6 Not touched

§4.3 (a wrapping mode: the owner's decision; it is now the main cost left in `recursion`, `parsing`, `hash-map` and
`data-oriented`), §4.6 (cross-language LTO), §4.2 item 4.

## 10. The tsrs map build: code placement (2026-11-12, after 0.38.0)

**Superseded in part by §11**, the same day: the placement effect below is real, but it exists only because the
benchmark sorts one input again and again. Read §11 before using any figure here; "what is left" and the case for
loop alignment do not stand.

§3's last item, "building a map of 2,000 segments: about twice as slow, cause not established". Measured with
ceesafec 0.38.0, the trial at `878c330f`, on the shared machine (load 2 to 8; figures move by about ±5 µs).

- **It is the sort's placement in the test binary.** `tsrs_spanmap__new` called directly, with the benchmark's exact
  input and a drop after each call: 42 to 50 µs inside the differential test binary, 24 to 33 µs in a C program and
  in a plain Rust binary linked with the same archive. The sort function's machine code is identical in both
  binaries (624 instructions, compared after disassembly); its address is 0x08 modulo 64 in the slow one and 0x04
  in the fast one.
- **The scan.** The sort moved 4 bytes at a time by a padding object linked before the archive, lowest of 5 runs:

  | Start, modulo 64 | CeeSafe, as built (`-O3`) | `-falign-loops=16` | `=32` | `=64` |
  |---|---:|---:|---:|---:|
  | 0x08, 0x0c, 0x10 | 46–52 µs | | | |
  | the other 13 | 28–41 µs | | | |
  | all 16 placements | | 24–41 µs, and 49–52 µs at one start (0x2c) | 25–36 µs | 24–33 µs |

  Rust's sort (`goslices.rs`, tsrs's release profile), moved the same way: 21–28 µs at 15 placements, 41 µs at one.
- **What is left.** At a good placement, CeeSafe 24–28 µs against Rust 21–22 µs for the copy and the sort; the
  shim's copy into a `Vec<u32>` is 3.5 µs more per call.
- **Not identified:** which loop. Three adjacent slow offsets would fit a four-instruction loop split by a line
  boundary, but the disassembly has none: the sort's smallest loops are six and seven instructions, and none starts
  at the offset that guess predicts. The cause inside the function is not known.
- **§4.2's unexplained row** (the sort slower with its bounds checks compiled out, 1.08 s) and §9.3's (`hash-map`)
  are probably the same effect: a different build moves the loops. Not verified.

**Loop alignment as a default, measured and not adopted.** The whole suite built three ways and run interleaved, 9
runs each (load 2 to 4):

| | `-falign-loops=32` | `-falign-loops=64` |
|---|---:|---:|
| Geometric mean of 17 workloads, lowest run | +0.4% | +0.6% |
| `string-building` | +3% | +6% |
| `numeric` | +2% | +2% |
| `const-flags` | +1% | +2% |
| The other 14 | within ±1% | within ±2% |

`string-building` was scanned over the 16 placements too: it has no slow placement as built (about 297 ms at each),
and aligned loops cost it the same 3% and 6% at every one. So the flag removes a rare large loss (a tight loop at an
unlucky offset) and adds a small one everywhere. Executables grow by 16 bytes.

## 11. The sort on data that differs: equal to Rust, and what §10 and §4.2 measured (2026-11-12)

Taken up from §4.2 ("check elimination is where CeeSafe can pass safe Rust"), with ceesafec 0.38.0 and the sort
benchmark of §6.1 rebuilt. Shared machine, load 1 to 4.

**The checks.** `--explain-bounds-checks` and `--explain-overflow-checks` of the sort (`goslices.cee`, through the
benchmark that instantiates it), kept of all:

| | Index checks | Overflow checks |
|---|---:|---:|
| The trial's port, 0.37.0 | 39 of 47 | 56 of 83 |
| The trial's port, 0.38.0 | 39 of 47 | 54 of 84 |
| The trial's port, with the two rules below | 25 of 45 | 47 of 84 |
| Sliced once (each helper given `data[a..b]`), 0.38.0 | 28 of 47 | 43 of 71 |
| Sliced once, with the two rules below | 12 of 44 | 33 of 71 |

The two rules are in the working tree, not released, and need an ADR: an index updated in place keeps its bounds
(`n -= 1` keeps what was above `n`, `i += 1` what was below; today even `while (n > 0) { n -= 1; … s[n] }` keeps
its check), and a reborrowed slice (`&mut (*s)`, which `s.swap` makes) has the length of the slice it came from.
The sliced sort gives the same result as the port (the benchmark's checksum).

**They change nothing measurable here, and neither does the language.** The benchmark of §6.1 copies the same
2,000 segments and sorts them, 20,000 times. Each variant below was linked at 16 placements (a padding object of 0
to 60 bytes before it), three runs each, microseconds per sort:

| | Best | Median | Worst |
|---|---:|---:|---:|
| The trial's port, 0.38.0 | 26.0 | 33.5 | 52.3 |
| The trial's port, with the rules | 27.8 | 33.5 | 51.6 |
| Sliced, 0.38.0 | 22.4 | 31.5 | 51.4 |
| Sliced, with the rules | 24.8 | 38.6 | 49.8 |
| Rust (`rustc -C opt-level=3`) | 21.2 | 30.1 | 36.8 |

Every variant, Rust too, swings by a factor of up to two with its placement, more than the variants differ. With
the benchmark changed to fill the 2,000 segments with new random values each round (both languages; the checksums
agree), the same 16 placements:

| | Best | Median | Worst |
|---|---:|---:|---:|
| The trial's port, 0.38.0 | 87.1 | 87.4 | 93.1 |
| The trial's port, with the rules | 85.5 | 86.3 | 90.7 |
| Sliced, 0.38.0 | 86.0 | 87.3 | 90.9 |
| Sliced, with the rules | 86.5 | 88.1 | 90.3 |
| Rust | 86.3 | 88.1 | 90.5 |

(That includes filling the segments, the same work in both.) No placement is slow, and the five are within 2% of
each other.

**What that means.**

- **A sort of one input, repeated, is a benchmark of the branch predictor.** It takes a third of the time of a
  sort of new data, because the predictor learns the branches of that input; how well depends on the addresses of
  the branches, which is the placement effect of §10. This reading fits every measurement here but was not tested
  by counting mispredictions.
- **On data that differs, the CeeSafe sort is as fast as Rust's,** with 39 index checks or 12. So §10's "what is
  left" (24–28 µs against 21–22 µs) is not a gap, and loop alignment, which §10 found cost the suite 0.4 to 0.6%,
  buys nothing real.
- **§4.2's table was taken with the repeated input** (§6.1's program): its rows, including "with no checks the
  CeeSafe is 15 to 25% faster than the Rust" and the unexplained 1.08 s, differ by less than one build differs from
  itself across placements. None of them should be relied on. Whether check elimination can put CeeSafe ahead of
  safe Rust is not shown by this sort either way.
- **The tsrs trial's map build**, timed in the trial's own test on 512 different maps in turn (three runs, either
  order): Rust 84 to 86 µs, CeeSafe 90 to 92 µs, 5 to 9% slower. The shim's copy of the segments is 3.5 µs of
  that. On one repeated map the same test gave 37 to 56 µs for Rust and 51 to 56 µs for CeeSafe across three runs.

**For later benchmarks:** never time a data-dependent algorithm on one input in a loop. Five workloads of
`benchmarks/` loop over the same data in rounds (`hash-map`, `arena`, `linked-list`, `vector-growth`,
`string-building`); their rounds are millions of operations long, far more than a predictor holds, but they were
not checked for this effect.

