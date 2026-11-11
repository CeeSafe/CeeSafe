# Performance against Rust: findings of 2026-11-11, for a later session

**Status:** an investigation, not a plan that anyone accepted. Nothing here is implemented. Each finding says whether
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
