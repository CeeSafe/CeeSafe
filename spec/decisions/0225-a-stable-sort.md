# ADR 0225 — A stable sort: `sortStableBy`

- Status: **Proposed** 2026-11-24 and implemented on the branch `fix/findings-core`, at the owner's instruction (the
  SQLite port's finding 32: "a stable sort for Vector/slices"). Not released. §6's question is the owner's.
- Scope: type-system.md §36.4; lib/core `slices.cee` (SliceAlgorithms); `src/check/library.cee`, `vectors.cee`,
  `members.cee`, `bytes_text.cee` (the method-name lists); guide/collections.md, guide/builtins.md; book Appendix C;
  tools/oracles/gen_stable_sort_cases.py.

## 1. Context

`s.sortBy(cmp)` is an introsort (ADR 0057) and the spec says it is not stable (§36.4). SQLite's sorter keeps rows with
equal ORDER BY keys in the order they arrived, and a port that must print the same rows in the same order has to
sort stably. The port appended each row's place as a last key and compared it after the ORDER BY terms. Any program
that sorts records by one field after another (sort by name, then stably by department) needs the same.

## 2. Decision

1. `s.sortStableBy(cmp)` on a `SliceMut<T>`, a vector, a mutable array or a slicing expression, with `sortBy`'s
   closure type `borrow mut fn(borrow T, borrow T) -> i32` and result `void`. It orders as `sortBy` does, and
   elements for which `cmp` returns 0 keep the order they had.
2. `sortBy` stays the unstable introsort and the spec keeps saying so: it is the faster sort, and callers that do
   not need stability keep its speed.
3. **Algorithm.** It allocates nothing, so it keeps `sortBy`'s infallible `void` result and works on elements of any
   type (moved by `swap`): insertion sort of blocks of 20 elements, then the blocks merged pairwise, bottom up, by
   SymMerge (Pok-Son Kim and Arne Kutzner, "Stable Minimum Storage Merging by Symmetric Comparisons", 2004), the
   algorithm of Go's `sort.Stable`: each merge splits at the middle, finds the cut by binary search, rotates the two
   middle blocks (by swaps of equal blocks), and merges the two halves the same way. O(n log n) calls of `cmp`,
   O(n log² n) swaps.
4. Like `introsort`, the merge is iterative, so that an async instance has a frame of fixed size (ADR 0037 §3.7):
   each step's range is split at its middle, so at most one range per depth waits and the depth is at most log₂ of
   the length; 64 waiting ranges suffice for any slice.
5. It is `@effectsOf(cmp)`, like `sortBy`. With an inconsistent `cmp` the result is some permutation of the input,
   and nothing panics.

## 3. Alternatives not taken

- **Making `sortBy` stable.** A stable sort that allocates nothing is several times slower than the introsort on
  long inputs (O(n log² n) swaps), and one that is as fast needs a buffer of n/2 elements: an allocation, so a
  `Result`, and moving elements of any type into a buffer is unsafe code. Neither fits the existing `sortBy`
  signature or its users.
- **A merge sort with a buffer** (`sortStableBy` returning `Result<void, AllocError>`): faster, but fallible and
  unsafe inside. It can be added as a second function if a measurement asks for it.

## 4. Tests

- `run-pass/sort-stable-patterns.cee`, written by `tools/oracles/gen_stable_sort_cases.py` from Python's `sorted`
  (stable) as the oracle: gen_sort_cases.py's ten input patterns at 14 lengths from 0 to 1000 (around the blocks of
  20 and the merges that join them), each sorted ascending and descending by key and by key modulo 4 (many equal
  keys), the FNV-1a of the original places checked; then a random, inconsistent comparator followed by a proper
  sort, checked to be a permutation. Built (with `--check-leaks`) and with `run --interp`.
- `tools/collection_properties.py`: the vector model sorts stably among its random operations, against Python's
  `list.sort` (stable), on elements with a printing drop.
- The guide's example (guide/collections.md) sorts jobs by priority and prints them in their stable order.

## 5. Performance

Not measured against `sortBy` in benchmarks/ yet: the spec states the complexity and that it is slower. For the
port's use (ORDER BY on result rows) the alternative was an extra key compared on every call.

## 6. Question for the owner

Keep `sortBy` unstable and `sortStableBy` allocation-free as §2 has them, or add a buffered stable sort?
