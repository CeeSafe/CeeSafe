# Loop versioning: a prototype

Status: a prototype on `proto/loop-versioning`, not an accepted decision. It has no ADR number: if it is adopted, this
text becomes the ADR and `spec/optimisations.md` gains a section. Measured on Linux x86-64 on 2026-11-12, on top of
0.37.0 (`e1ed24a`).

## 1. What it does

A counted loop whose index checks check elimination (optimisations.md §2) cannot prove is emitted twice. Before the
loop, a guard tests, without panicking, that the largest index the loop can use is in range. If it is, a copy of the
loop without those checks runs; otherwise the loop runs as it was written.

```c
{ uint64_t ceeG0, ceeG1;
  g = j < n && cee_try_mul_u64(i, n, &ceeG0) && cee_try_add_u64(ceeG0, n - 1, &ceeG1) && ceeG1 < c.len; }
if (g) {
    for (;;) { … c.ptr[i * n + j] … }            /* no index check, no overflow check on i * n + j */
} else {
    for (;;) { … cee_check_index(…) … }           /* the loop as it was */
}
```

Behaviour does not change. The guard has no effect of its own, and it passes only when none of the checks it covers
could fail in any iteration, so the copy without them does what the checked loop would. A run in which one of them
would fail takes the checked loop and panics at the same operation, with the same message and location, after the
same work.

It is the same result check elimination aims at, reached without a proof: the question "is `i * n + j` below the
length for every `j`" is asked once at run time instead of being decided at compile time. It does not depend on the
C compiler's optimisation level or version, which is what `-O3` was needed for (ADR 0058).

## 2. Which loops

A loop is versioned when all of this holds (`src/backend/loop_versioning.cee`, `planLoop`):

1. It is an innermost natural loop of at most 12 blocks, entered from one block that ends in a plain jump.
2. Its header is `t = counter < bound; branch t ? inside : outside`, with `counter` and `bound` of type `usize`,
   `bound` a constant or a local the loop does not assign.
3. `counter` is assigned once in the loop, `counter = counter + 1`, in a block whose only successor is the header.
   This is what `for (j in lo..hi)` lowers to.
4. Every statement in the loop is a StorageDead, a BoundsCheck, or an assignment of a plain computation: a use, a
   binary or unary operation, a cast, a truncation, a length, or wrapping, saturating, min, max or mul-high
   arithmetic. No calls, borrows, drops, boxes or awaits. (The first cut; see §6.)
5. At least one BoundsCheck that check elimination kept has an index and a length of the forms below.

An index is one of:

- the counter, or a local assigned once in the loop from it (`j = counter`);
- a sum of that and a value that does not change in the loop, in either order, nested (`lo + j + 1`);
- a value that does not change in the loop.

A value that does not change in the loop is a `usize` constant, a `usize` local the loop does not assign, or a local
assigned once in the loop as a sum or product of such values (`i * n`, where `i` is an outer loop's variable). A local
assigned in the loop counts only where its assignment runs before the use in every iteration: it is earlier in the
same block, or its block dominates the use's.

A length is a constant, a local the loop does not assign, or `length(v)` for a local `v` the loop does not assign
(the rule of §2.3: an assignment through a dereference writes elements, never `v`'s own storage), or
`length(*r)` for a reference local `r` that the loop neither assigns nor assigns through as a whole (`*r = …`).

## 3. The guard

With the counter's value before the loop `lo` and the bound `hi`, the indexes of a covered check are `B + j` for
`lo ≤ j < hi`, where `B` is the base (absent, or the value that does not change). The guard is the conjunction of:

- `lo < hi`: otherwise the loop does not run, and either copy does the same nothing;
- for each covered check, `B` computed with `cee_try_add_u64` and `cee_try_mul_u64` (rt.h's non-panicking
  arithmetic) without overflow, `B + (hi − 1)` without overflow, and that sum below the length. For an index that
  is the counter alone: `hi ≤ length`. For an index that does not change: index `< length`.

In the copy, the pass removes exactly: the covered BoundsChecks; the overflow checks of the additions that form a
covered index (`B + j ≤ B + hi − 1`, which did not overflow); and the overflow checks of the operations inside `B`
(the guard computed the same operations on the same values). Every other check stays in both copies.

Why the counter is below `hi` at each covered check: the header tested it in this iteration, and it changes only in
the block that jumps back to the header. Covered checks are never taken from that block or from the header.

## 4. Where it sits

`generateC` (`src/backend/c_backend.cee`) runs it after `analyzeChecks` and before placement and emission, so both
emitter passes see the same NIR. It needs `--bce` and `--oce` on (it marks its results in the same `CheckResults`),
and `--no-loop-versioning` turns it off.

The pass changes the NIR: it copies the loop's blocks, adds one guard block, and retargets the loop's entry. In the
NIR the guard is `g = false`, so any other reader of the NIR sees a program that always takes the checked loop. The
C backend writes the real test in place of that statement (`emitLoopGuard`). The structurer needs no change: it
prints the result as `if (g) { for … } else { for … }`.

Not versioned: async functions, generic functions, functions with a `debugAssert` (ADR 0087 names its blocks by id
range), blocks under `cfg`.

The interpreter does not run the pass. Its results cannot differ, since the pass does not change behaviour.

## 5. Evidence

All on one 2-vCPU x86-64 VM; gcc 13.3, clang 22.1.6; rustc 1.97.0. Timings interleaved, pinned, medians.

### Behaviour

- The compiler built by itself with the pass reaches a fixed point: the second and third generations are
  byte-identical. 40 loops of the compiler are versioned; the binary is 0.4% larger.
- 58 cases of small loops (in range, empty, past the end by one, overflowing bases, early exits, writes through
  references, slices, arrays), each built with the pass on, off, and on under ASan and UBSan: 25 of them panic,
  and no case differs in output, exit status or panic message.
- The corpus (`run-pass`, `run-fail`, `ffi`: 400 programs) with clang 18 and `-Werror`, and with gcc under
  ASan and UBSan with `--check-leaks`: three failures, the IPv6 and UDP tests, which fail the same way with the
  0.37.0 compiler on this machine (it has no IPv6).
- Three corpus tests are new: `run-pass/loop-versioning.cee`, whose expected output is that of
  `tools/oracles/loop_versioning_model.py`, and `run-fail/loop-versioning-index.cee` and
  `loop-versioning-overflow.cee`. They pass with the pass on, with `--no-loop-versioning`, and with `--interp`.

### Speed

`benchmarks/independent`, time as a multiple of Rust built with overflow checks (lower is better; 1.00 is equal).
`-O2` in every column. Outputs are identical in every row.

| Workload | Loops versioned | gcc 13 off | gcc 13 on | clang 22 off | clang 22 on |
|---|---|---|---|---|---|
| matmul | 2 | 3.33 | 1.35 | 1.72 | 1.01 |
| fannkuch | 2 | 0.90 | 0.97 | 0.96 | 1.04 |
| spectral-norm | 1 | 0.91 | 0.88 | 2.87 | 2.96 |
| sieve | 1 | 0.95 | 0.94 | 0.94 | 0.94 |
| nbody | 0 | 1.37 | 1.37 | 1.36 | 1.40 |
| mandelbrot | 0 | 1.21 | 1.20 | 1.17 | 1.19 |
| binary-trees | 0 | 1.09 | 1.03 | 1.09 | 1.09 |
| hashmap | 0 | 1.45 | 1.41 | 1.08 | 1.01 |
| hashmap-same | 0 | 1.20 | 1.24 | 1.08 | 1.05 |
| sort | 0 | 1.62 | 1.64 | 1.42 | 1.43 |
| sort-same | 0 | 1.00 | 0.96 | 0.94 | 0.93 |
| text-parse | 0 | 1.53 | 1.50 | 1.08 | 1.02 |
| dyn-dispatch | 0 | 0.81 | 0.83 | 1.02 | 1.00 |

- **matmul** is the shape the pass was written for: `c[i * n + j]` in the inner loop. With gcc it goes from 3.3
  times Rust's time to 1.35, with clang 22 from 1.7 to equal.
- **fannkuch is 4 to 8% slower with the pass**, in both runs and with both C compilers. Its two versioned loops run
  for a handful of iterations each (`for (i in 0..r)` with `r` mostly 1 or 2) and are entered millions of times,
  so the guard costs more than the checks it removes. This is the pass's known weakness, and a release would need
  an answer to it: see §6, "Short loops".
- Every other difference is inside this machine's noise (about 3% between runs of one binary). Workloads with no
  versioned loop are the control: their on and off columns are the same C.
- clang 22's 2.9 on spectral-norm is not from this pass; it is there with the pass off.

## 6. Limits, and what an ADR would need

- **Short loops.** The guard runs each time the loop is entered. A loop of two iterations entered millions of times
  pays more for the guard than it saves (fannkuch, §5). Candidates: version only when the bound is not provably
  small and skip loops nested in a loop of the same function unless the guard can be hoisted to the outer loop
  (its operands often do not change there); or hoist first and version second. Not tried.
- **Calls in the loop.** A call makes the loop ineligible, so `sum += f(v[j])` is not versioned, nor is any loop
  that pushes or prints. Calls cannot change a local's value or a local vector's length unless the loop borrows it
  mutably, so rule 4 can be widened to calls whose arguments are not such borrows. The copy must then carry the
  per-rvalue tables (callee variants, task roles, placements) with it.
- **Only `usize`, only step 1, only `<`.** `while (i < n) { …; i += 1; }` with the step in the body, steps other
  than 1, `<=`, and signed or narrower counters are left alone.
- **Lengths behind fields.** `self.items[j]` reads `length((*self).items)`, which is not covered. It needs the rule
  for which assignments can change a field's vector.
- **Iterators.** `for (x in v)` is a different lowering and is not covered; check elimination handles most of it.
- **Nested loops.** Only the innermost loop is versioned. Versioning the outer loops too would hoist the guard
  further, at the cost of more copies.
- **Code size.** Each versioned loop is emitted twice. The compiler itself grew by 0.4%.
- **A report.** `--explain-bounds-checks` should say "hoisted" for a covered check, with the same stable format
  as "removed" and "kept".
- **The spec.** optimisations.md needs the rules of §2 and §3 as normative text, and the corpus a
  `loop-versioning` family that pins which loops are versioned, as `bounds-checks` does for elimination.
- **Not measured:** arm64, macOS, and the compile-time cost of the pass (it runs once per function).
