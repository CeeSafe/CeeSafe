# ADR 0103 — `partitionPoint` on slices

- Status: **Accepted** by the owner, 2026-11-12 ("Go ahead", on the proposal to prototype the closure question and
  then add it), implemented on branch `impl/0103`, on 0.39.0, and released in ceesafec 0.40.0. Raised by
  `docs/performance-investigation-2026-11.md` §12: the tsrs trial's port wrote Rust's `partition_point` out three
  times as a loop, in a form that is slower than the standard one.
- Scope: one function in `lib/core/slices.cee`; the checker's lists of slice and vector methods
  (`src/check/vectors.cee`, `members.cee`); `spec/type-system.md` §36.4; the guide. No change to the language or the
  runtime.

## 1. Context

CeeSafe's library has `sortBy` and no search. A program that needs "the first element not below `k`" writes

```ceesafe
var lo: usize = 0;
var hi = s.length;
while (lo < hi) {
    let middle = lo + (hi - lo) / 2;
    if (s[middle].key < k) { lo = middle + 1; } else { hi = middle; }
}
```

which since ADR 0100 has no check left in it. It is still the slower form of the search. Its loop ends when the
bounds meet, so the processor cannot know how many steps there are before it has compared; Rust's
`partition_point` halves a size, so the number of steps depends on the length alone. Measured 2026-11-12 (Apple
M-series, 40 million searches of 2,000 24-byte elements for random keys, the lowest of 7 runs, the key's generation
included):

| | ns a search |
|---|---:|
| The loop above, in CeeSafe | 16.1 |
| The loop above, in Rust | 18.6 |
| Halving a size, written inline in CeeSafe | 9.2 |
| The same as a function taking the test as a closure, index checks kept | 9.2 |
| The same with unchecked reads | 9.2 |
| Rust's `slice::partition_point` | 10.3 |

So the form is worth 40%, it is not a matter of the language, a closure costs nothing (the call is specialised, ADR
0019), and the two index checks in the loop cost nothing either.

In the tsrs trial the three searches rewritten this way gave the same answers and did not move its lookups by more
than the spread: a search is a small part of each. The case for the method is the isolated figure and that ports
from Rust and Go have a name for it.

## 2. Decision

```ceesafe
s.partitionPoint(test)      // test: borrow mut fn(borrow T) -> bool; the result is a usize
```

on a `Slice<T>` (a `SliceMut<T>` coerces), a slicing expression, and a vector, like `any` and `all`: it is
`SliceAlgorithms<T>.partitionPoint(s, test)` in `lib/core`, `@effectsOf(test)`.

- For a slice **partitioned** by `test` (every element for which it is true before every element for which it is
  false) the result is the index of the first element for which `test` is false; the length if there is none; 0
  for an empty slice.
- `test` is called at most ⌈log₂ n⌉ + 1 times. Nothing is allocated and nothing panics.
- On a slice that is not partitioned the result is some index of it or its length.
- The implementation is safe code. Its loop keeps an index check and an overflow check, which the analysis cannot
  remove (the invariant `base + size ≤ length` relates two locals) and which cost nothing measurable.

Not included: `binarySearch(key)` and a comparator form. `partitionPoint` gives both
(`let at = s.partitionPoint((e) => e.key < k); at < s.length && s[at].key == k`), and the first needs an ordering
interface on `T` that slices' methods do not otherwise ask for.

## 3. Compatibility

A program whose slice or vector method was called `partitionPoint` could not exist: those types take no user
methods. The name joins the methods the checker lists in its "has no method" messages.

## 4. Tests

- `tests/corpus/run-pass/slice-partition-point.cee`: on a slice, a slicing expression, an array and a vector; a
  captured value and a function value; every length from 0 to 40 with every split (the answer is the split, and at
  most 7 calls); a binary search built on it; a slice that is not partitioned.
- Three HIR snapshots (`check-pass/async-timeout`, `effects-of`, `string-braces-warning`) number their closures one
  higher, since the library has one more function: reviewed, nothing else in them differs. `spec/api/core.md` gains
  the function.
- Run 2026-11-12, macOS arm64, on the commit `91f890f`: `tools/contract_tests.py`, 40 ok, 2 skipped (Linux only), 0
  failed; `tools/bootstrap_release.sh` from ceesafec 0.3.2.

