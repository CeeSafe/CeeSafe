# ADR 0102 — An index updated in place keeps its bounds

- Status: **Accepted** by the owner, 2026-11-12 ("I accept the ADR 0102 as proposed"), implemented on branch
  `impl/0102`, on 0.38.0, and released in ceesafec 0.39.0. Raised while taking up
  `docs/performance-investigation-2026-11.md` §4.2 (its §11): the sort of the tsrs trial kept 39 of its 47 index
  checks with ceesafec 0.38.0.
- Scope: `src/backend/check_elimination.cee`; `spec/optimisations.md` §2.4 and §2.8. The analysis decides more
  checks; nothing else changes.

## 1. Context

```ceesafe
var n = s.length;
while (n > 0) {
    n -= 1;
    total += s[n];     // index check kept
}
```

`n -= 1` is `n = n - 1`: an assignment to `n`, which ends every fact about `n` (`optimisations.md` §2.3), and a
definition whose operand is the local itself is not recorded, since the old value is gone. So `n ≤ len(s)` is lost
at the one statement that makes it `n < len(s)`. The same holds of `i += 1` and what was below `i`. Loops that walk
an index down, or two indexes towards each other (a partition), keep all their checks for this reason.

A second gap shows in the same loops: `s.swap(i, j)` on a `SliceMut` local reborrows it, `t = &mut (*s)`, and
checks `i` and `j` against the length of `t`, of which nothing is known.

## 2. Decision

### 2.1 An update in place

For an assignment `x = x op b` to an integer local, the comparisons of `x` with other terms that the update cannot
have made false are kept, about the new value:

- **Down:** `x = x - b`, and for an unsigned `x`, `x = x / b` and `x = x >> b`. The new value is at most the old
  one, so each `x ≤ u`, `x < u` and `x = u` gives `x ≤ u` (or `x < u`, if it was).
- **Up:** `x = x + b` or `x = b + x`. The new value is at least the old one, so each `l ≤ x`, `l < x` and `l = x`
  gives `l ≤ x` (or `l < x`).
- **Strictly,** when `b` is a constant of at least 1 and the operation is `+` or `-`: `x ≤ u` gives `x < u`, and
  `l ≤ x` gives `l < x`.

Sound because the arithmetic is exact: `+` and `-` panic rather than wrap, an unsigned quotient or shift cannot
exceed its dividend, and `b` is not negative: any unsigned operand, or for a signed `x` a constant of at least 0.
The other term `u` or `l` does not mention `x`, so the assignment does not change it. A product is not included
(`x * 0`), nor a signed operand that is not a constant.

That half of a number of at least 1 is less than it (ADR 0100 §2.2) is not applied to an update in place: `n /= 2`
keeps `n ≤ u`, not `n < u`.

### 2.2 A reborrowed slice

`t = &(*s)` or `t = &mut (*s)`, where `s` and `t` are slice locals and the place is `s` dereferenced and nothing
more, gives `len(t) = len(s)`: the reborrow is of the whole slice. The fact ends with `t` or `s`, as any fact does.

## 3. What it gains

`--explain-bounds-checks` and `--explain-overflow-checks`, kept of all, in the sort of the tsrs trial:

| | Index checks | Overflow checks |
|---|---:|---:|
| 0.38.0 | 39 of 47 | 54 of 84 |
| With this ADR | 25 of 45 | 47 of 84 |
| With this ADR, each helper given `data[a..b]` | 12 of 44 | 33 of 71 |

No time: on data that differs each round the sort takes the same time with 39 checks or 12, and the same as Rust's
(the investigation's §11). The gain is in what a program can be shown not to do, and in loops whose checks do cost.

Over the compiler's own sources, against 0.38.0: 27 more index checks and 11 more overflow checks removed, and no
check that 0.38.0 removed is kept. The compiler emitting its own C: 8.2 s before, 8.5 s after (one run each).

## 4. Tests

- `tests/corpus/bounds-checks/updated.cee` and `.bce`: seven index checks removed (a reverse loop, an insertion
  sort with `swap`, a two-ended scan, a halved index), five near misses kept (an index stepped up, an amount that
  may be 0, a product, a halving that needs strictness, a slice of part of the slice).
- The interpreter evaluates every removed check on every run (`optimisations.md` §2.8).
- Run 2026-11-12, macOS arm64: the corpus (0 failures), `tools/spec_check.py` (0 problems), the fixed point by hand
  (two generations, identical C). On the commit `c62fd52`: `tools/contract_tests.py`, 40 ok, 2 skipped (Linux
  only), 0 failed; `tools/bootstrap_release.sh` from ceesafec 0.3.2.

