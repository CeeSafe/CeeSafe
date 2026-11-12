# ADR 0099 — `resize` of a vector with no buffer takes zeroed memory

- Status: **Proposed**, 2026-11-12, implemented in the working tree and not released. Raised by
  `docs/performance-investigation-2026-11.md` §4.1: the `hash-map` workload was 1.43 times Rust's time, and the cause
  measured there was filling two tables of 4,194,304 `u64`, not its index checks.
- Scope: the C emitter's `resize` helper (`src/backend/c_helpers.cee`, `ceeVZ_…`), one runtime function
  (`ceesafe_vec_zeroed`, `runtime/core/ceesafe_rt_core.c`) and one inline (`cee_all_zero`, `rt.h`). No change to the
  language, to the checker, to the interpreter or to what a program can observe.

## 1. Context

`v.resize(n, fill)` (ADR 0071 §2.1) grows the buffer once, with `ceesafe_vec_grow`, and writes `fill` into every new
element. For a vector with no buffer that is an allocation followed by a loop over all of it. When the fill is zero
that loop is work the allocator can do better: `ceesafe_alloc_zeroed` is the platform's `calloc`, which for a large
block returns pages the operating system has not touched, and they are zero when first read. Rust's `vec![0; n]` and
C's `calloc` do that, and the Rust and C versions of the workload use them.

## 2. Decision

In the `resize` helper, when the vector has no buffer (`cap == 0`) and every byte of `fill` is zero, the buffer is
allocated zeroed and nothing is written:

```c
if (v->cap == 0u && cee_all_zero(&fill, sizeof fill)) {
    uint64_t cap = 0u;
    void *p = ceesafe_vec_zeroed(&cap, n, sizeof(T), allocator);
    if (p == NULL) return error;
    v->ptr = (T *)p; v->cap = cap; v->len = n;
    return ok;
}
```

`ceesafe_vec_zeroed` gives the capacity `ceesafe_vec_grow` gives a vector with no buffer (at least 4 elements, at
least `n`), refuses the same sizes, and makes one allocation attempt.

The test is on the bytes of the value, at run time, so it holds for every Copy element type without a rule per type:

- an integer, a `bool` or a pointer-free struct of zeros qualifies;
- `-0.0` does not (its sign bit is set), and gets the loop, which keeps its sign;
- a struct whose padding bytes happen not to be zero does not qualify and gets the loop. When every byte is zero,
  every field is zero, so zeroed memory holds the same elements.

With a constant fill the C compiler folds the test away.

## 3. What a program can observe

Nothing (`optimisations.md` §1): the same length, capacity and elements; one allocation attempt and one live block,
so the allocation report, `FailingAllocator` and `CEESAFE_FAIL_ALLOC_AT` count as before; the same `AllocError`,
with the vector unchanged. The interpreter models a vector itself and does not change.

There is no switch: this is not an elimination of a check, and both paths produce the same vector.

## 4. Not decided here

A constructor, `Vector<T>.filled(n, value)`, which `guide/performance.md` proposed. `var v = Vector<T>.create();
v.resize(n, value)?;` now costs what it would, so it would be a convenience only. It needs its own ADR if wanted.

## 5. Tests and measurements

- `tests/corpus/run-pass/vector-resize-zeroed.cee`: zero and non-zero fills, `-0.0`, a struct, a vector that has a
  buffer, lengths 0 and 1.
- `tests/corpus/emit-c/vectors.c`: the snapshot gains the branch above in `ceeVZ_…` (a reviewed edit).
- The `hash-map` and `data-oriented` workloads make their tables with `resize`, as the Rust and C versions make
  theirs with `vec![0; n]` and `calloc`. Measurements: `guide/performance.md`.
