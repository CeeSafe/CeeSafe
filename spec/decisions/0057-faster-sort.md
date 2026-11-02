# ADR 0057 — A faster `sortBy`: branchless partitioning, equal keys, sorted runs

- Status: **Accepted** by the owner, 2026-11-02 ("Yes to all ADRs. I accept."): every question of §5 as
  recommended. Asked for by the owner ("draft an ADR for a faster sortBy") after the independent benchmark
  (`benchmarks/independent/`, imported in `b9ec7ed`): its `sort` workload is CeeSafe's slowest, 4.3× Rust on an
  Apple M1 at every C optimisation level (`results-macos-0.9.3.json`) and 4.2× on the author's Linux VM
  (`results-linux-0.9.0.json`).
- Scope: lib/core's `SliceAlgorithms<T>.sortBy` and its helpers (`lib/core/slices.cee`), type-system.md §36.4, the API
  page, the guide. No language, compiler, runtime or interpreter change.

## 1. Context

`sortBy` (type-system.md §36.4; `lib/core/slices.cee:46`) is an introsort: a Lomuto partition around a median of three,
insertion sort by adjacent swaps below 17 elements, heapsort past a depth of 2·log₂ n. It is generic over any `T`,
takes a `borrow mut fn(borrow T, borrow T) -> i32`, allocates nothing, and uses only safe CeeSafe and `swap`.

**The comparator is not the cost.** `--explain-closure-calls` on the benchmark shows every call of `cmp` in
`partition`, `insertion` and `siftDown` made direct to the closure (ADR 0019), so the C compiler sees and inlines it.

**The algorithm is.** The benchmark's `sort-same` workload, the same hand-written Hoare quicksort in both languages, is
1.00× Rust; Rust's own `sort-same` is about four times slower than Rust's `sort_unstable_by`. Rust's library sort
(ipnsort) wins by what it does, not by how it is compiled:

- On random keys, a partition whose loop branches on the comparison mispredicts about half the time. ipnsort's
  partition has no data-dependent branch: it moves every element and advances its boundary by the comparison's
  outcome.
- With many equal keys, it partitions the elements equal to the pivot off and does not sort them again. Our Lomuto
  partition sends every equal key to one side, so all-equal input runs to the depth limit and finishes in heapsort.
- A slice that is already one ascending or descending run is detected in one pass and finished (reversed if
  descending).

### 1.1 The prototype (run 2026-11-02, Apple M1, ceesafec 0.9.3 at its default `-O2`, best of 3)

User-level CeeSafe code with the same signature as the library, through the same comparator closure:
`Branchless<T>.sortBy` = the iterative quicksort of today with (a) a branchless Lomuto partition, (b) pdqsort's
equal-key partition, (c) a run check at the start. Rust is `sort_unstable_by` (rustc 1.99.1, `-C opt-level=3`). Each
time is the whole process: generating the keys, sorting them, and a checksum pass. All three sorts printed equal
checksums.

| 10 M `u64` keys | Rust | today | prototype |
|---|---|---|---|
| random | 0.19 s | 0.75 s | **0.31 s** |
| ascending | 0.04 s | 0.15 s | **0.04 s** |
| descending | 0.04 s | 0.39 s | **0.04 s** |
| all equal | 0.04 s | 0.25 s | **0.04 s** |
| 16 distinct values | 0.07 s | 0.35 s | **0.08 s** |

| Larger elements, random keys | today | prototype |
|---|---|---|
| 32-byte struct, 10 M | 1.57 s | **0.95 s** |
| 128-byte struct, 4 M | 1.06 s | **0.73 s** |

The branchless partition alone (without (b) and (c)) took random keys from 0.75 s to 0.28 s, but went quadratic on
all-equal and 16-value input (no result within 120 s): (b) is not optional. A Hoare partition that still branches
gained only 4% (0.73 s), and an insertion sort that shifts instead of swapping (possible only for `T: Copy`) gained
nothing measurable over it.

## 2. Options

| | A. Branchless partition, equal keys, runs (proposed) | B. A port of ipnsort / driftsort | C. Keep the introsort, tune it |
|---|---|---|---|
| What | §3: three changes to the introsort we have, all safe CeeSafe | block partition, sorting networks for small slices, a merge of runs, scratch memory | Hoare partition, a better insertion sort |
| Random keys (prototype) | 2.4× faster; 1.6× Rust | close to 1× Rust | 1.03× faster |
| Patterns | sorted, reversed, equal keys at Rust's speed | the same | unchanged |
| Cost | about 60 lines changed in `slices.cee` | thousands of lines; scratch memory breaks "allocates nothing", or needs `unsafe` copies | small |
| Guarantees of §36.4 | all kept (§3.4) | at risk | kept |

**A is proposed.** It removes most of the gap with what the language already has: no `unsafe`, no allocation, no new
API. B's remaining step from 1.6× to about 1× (small-sort networks, a cyclic permutation that halves the writes) can
come later, measured on top of A.

## 3. Proposed decision

### 3.1 The partition

`partition(s, lo, hi, cmp)` keeps the median-of-three pivot at `s[hi - 1]`, and partitions without a branch on the
comparison:

```ceesafe
var store = lo;
for (i in lo..last) {
    let below: usize = if (cmp(borrow s[i], borrow s[last]) < 0) 1 else 0;
    s.swap(i, store);
    store += below;
}
s.swap(store, last);
```

The swap is unconditional: an element not below the pivot is swapped with itself or with one not below it, so
`[lo, store)` is below the pivot and `[store, i]` is not. The C compilers turn the comparison into a conditional set
(`cset`, `setcc`); the implementation checks this on gcc and clang (§6).

### 3.2 Equal keys

Every range the sort works on, except one starting at 0, starts just after an earlier pivot, and that pivot is at
most every element of the range. So, after choosing a pivot for `s[lo..hi]` with `lo > 0`: if `cmp(s[lo - 1],
pivot) >= 0`, the pivot equals that lower bound, and the elements not above it are all equal to it. They are
partitioned to the left (the same branchless loop with `cmp(pivot, s[i]) >= 0`), and the sort continues with the
elements above the pivot only (pdqsort's `partition_left`). Each such step finishes at least the pivot, so a range of
k distinct values takes O(n·k) comparisons at most, and the depth limit still bounds the rest.

### 3.3 Runs

Before partitioning, `sortBy` scans from the start while the slice is ascending (`cmp(s[i], s[i - 1]) >= 0`) or
strictly descending. A slice that is all one ascending run is done; all one strictly descending run is reversed
(strictly, so no two of its elements are equal, and reversing it is its sort). The scan stops at
the first element out of order, so it costs at most n − 1 comparisons, wasted only when the slice is nearly sorted
but not quite.

### 3.4 What §36.4 promises, kept

- **Not stable; deterministic.** The order chosen is still a function of the input and of `cmp`'s results: no
  randomness, the same in the interpreter and natively. It changes from 0.9.3's for equal keys (§4).
- **O(n log n) comparisons in the worst case, O(log n) stack, no allocation.** The heapsort fallback past a depth of
  2·log₂ n stays, and so does the explicit stack of at most 64 waiting ranges (ADR 0037 §3.7: an async instance's frame
  has a size).
- **An inconsistent `cmp` leaves a permutation, and nothing panics.** Every index the new code touches is inside
  `[lo, hi)` whatever `cmp` returns: `store ≤ i < last`, and the equal-key step makes `lo` grow by at least one. A
  `cmp` that lies may leave the slice unsorted but cannot make the loop run past its range or forever.
- **`@effectsOf(cmp)`, the async instance, `@context(atomic)` and `irq`:** unchanged; the new helpers are
  `@effectsOf(cmp)` as the old ones are.

## 4. Consequences

- The benchmark's `sort` should go from 4.3× Rust to about 1.6× (§1.1); programs sorting sorted, reversed or
  duplicate-heavy data gain most.
- **Observable changes.** Equal elements may end in a different order than with 0.9.3, and `cmp` is called a
  different number of times and in a different sequence. §36.4 never promised either, but a program that printed
  them sees the difference. The corpus's `run-pass/closure-methods` counts comparisons only against a bound
  (`comparisons < 2000 * 40`).
- Heapsort is reached more rarely: all-equal keys no longer reach it (§3.2), so the corpus needs an input that
  exhausts the depth limit on purpose (§6).
- New diagnostics: none.

## 5. Questions for the owner

1. The three changes of option A, keeping §36.4's guarantees, with B's further work left for a later ADR?
   Recommended: yes.
2. The run check of §3.3, which costs up to n − 1 comparisons on input that is nearly but not quite sorted?
   Recommended: yes; Rust's sort does the same.
3. Equal elements may end in a different order than 0.9.3's (unstable both ways): a note in the release, no
   compatibility switch? Recommended: yes.

## 6. Implementation plan (once accepted)

1. Spec: type-system.md §36.4's `sortBy` paragraph (the algorithm named: introsort with a branchless partition,
   pdqsort's equal-key partition, a run check), the API page through `tools/gen_docs.py`, guide/closures.md.
2. `lib/core/slices.cee`: `partition` as §3.1, a `partitionEqual`, the run check in `sortBy`, the equal-key step in
   `introsort`; `tools/gen_library.py`.
3. Check the generated C: the partition loop has no conditional branch on the comparison under gcc and clang at `-O2`
   (macOS, the Debian box's gcc 12, MSVC on the Windows VM).
4. Tests:
   - run-pass/closure-methods: its comment no longer says all-equal keys reach heapsort; add an input that reaches it
     (a median-of-three killer sequence), and patterns of every kind (runs, reversed, all equal, few distinct values,
     a run followed by one out-of-order element), each checked against Python's `sorted` as an oracle in
     `tests/support` (the expected values from outside the code under test);
   - an inconsistent comparator (random results from a seeded generator) on 10 000 elements: a permutation, no
     panic, natively and in the interpreter;
   - the async instance (run-pass/async-effects-of) unchanged;
   - the fault and cancellation sweeps, ASan, the collection properties.
5. Benchmarks: `benchmarks/independent/` `sort` before and after, recorded in its README; `benchmarks/run.py` unchanged.
6. ROADMAP §10.

## 7. Implementation notes (2026-11-02)

- **The code** is §3 as written: `sortBy` checks the run at the start, `introsort` sets equal keys aside when
  `low > 0` and the pivot is not above `s[low - 1]`, and the new helpers `pivot` and `partitionEqual` sit beside
  `partition`, all `@effectsOf(cmp)` (`lib/core/slices.cee`).
- **No branch on `cmp`'s result** in either partition loop, checked in the assembly of the benchmark's `sort`: clang
  21 on arm64 (`cinc`), gcc 12 on x86-64 (`setb`, `setnb`, then `add`), MSVC on arm64 (`csetlo`, `cseths`, then `add`).
  The loop's remaining branches are its index checks, never taken.
- **Every call of `cmp` stays direct** (ADR 0019): `closure-calls/known.calls` gains the run check's calls in `sortBy`
  and the new helpers' lines, and the library's line numbers moved. The user code's lines are unchanged. This is a
  reviewed edit of the snapshot.
- **Heapsort** is reached on purpose by McIlroy's adversary in run-pass/closure-methods. Its comparator fixes values
  lazily, and the first two values are fixed in descending order so that the run check does not finish the sort
  first. With the depth limit, n = 2000 takes 74 752 comparisons; the prototype without one took 1 001 938 (n²/4),
  and 256 M for n = 32 000.
- **The oracle test** is run-pass/sort-patterns, from `tools/oracles/gen_sort_cases.py`. It covers 10 patterns at 11
  lengths, ascending and descending, (key, index) entries checked as a permutation, and an inconsistent comparator
  followed by a proper sort, all against Python's `sorted`. Its lengths stop at 700 so that the interpreter runs it in
  about 6 s; at 5000 it took 52 s.
- **Measured** with the benchmark's `sort` (25 M random `u64`, Apple M1, `-O2`, best of 3): 1.96 s before, 0.77 s
  after; Rust takes 0.46 s.
