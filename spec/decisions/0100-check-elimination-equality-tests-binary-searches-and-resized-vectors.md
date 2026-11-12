# ADR 0100 — Check elimination: equality tests, binary searches and resized vectors

- Status: **Proposed**, 2026-11-12, implemented in the working tree and not released. Raised by
  `docs/performance-investigation-2026-11.md` §4.2, items 1 and 2: in the tsrs trial's span map, a binary search
  kept its index check and the overflow check of its midpoint, and `idx - 1` after `if (idx == 0) { return …; }`
  kept its overflow check. All three are provable.
- Scope: `src/backend/check_elimination.cee`; `spec/optimisations.md` §2.2 to §2.5, §2.8, §3.2 and §3.5. The
  analysis decides more checks; nothing else changes. Of item 3 (a length known from construction) it takes the
  case that was measured, a vector made with `resize` (§2.5); a masked index against such a length in another
  function, and item 4 (facts across calls), are not part of this.

## 1. Context

```ceesafe
var lo: usize = 0;
var hi = segments.length;
while (lo < hi) {
    let middle = lo + (hi - lo) / 2;          // overflow check kept
    if (segments[middle].virtualStart < pos) { // index check kept
        lo = middle + 1;
    } else {
        hi = middle;
    }
}
let idx = lo;
if (idx == 0) {
    return …;
}
let prev = idx - 1;                            // overflow check kept
```

Four things were missing from the analysis (`optimisations.md` §2):

1. `==` and `!=` gave no fact: only `<`, `<=`, `>`, `>=` did.
2. `/` was not a definition, and nothing said that half of a positive number is less than it.
3. The temporaries of `lo + (hi - lo) / 2` end at the end of the statement, and what was derived through them
   (`middle < hi`) ended with them, before `segments[middle]`.
4. At a join, a fact was kept only if both paths held it in the same words. The loop enters with
   `hi = segments.length` and comes round with `hi = middle`, `middle < segments.length`: no common fact, so
   `hi ≤ segments.length` was not an invariant.

## 2. Decision

Each rule is stated in `optimisations.md`; this is the list, with why each is sound. Terms are mathematical
integers (§2.2 there), which stays true: the analysis removes a check only where the exact result is in range.

### 2.1 Unequal terms (§2.2, §2.4, §3.2)

A new relation, `a ≠ b`. `c = a == b` on integer terms gives "if `c`, then `a = b`" and "if not `c`, then `a ≠ b`";
`!=` gives the same with the edges exchanged. Used in two places:

- in the closure, `a ≠ b` with `a ≤ b` derived gives `a < b`: integers;
- in a term's bounds, a bound that is a constant `k` with `a ≠ k` derived moves by one, repeatedly: `a ≥ k` and
  `a ≠ k` give `a ≥ k + 1`.

A pair that the facts also make equal is ignored: such facts contradict, the code never runs, and deriving `a < a`
there would gain nothing. The test gives facts only when both sides are lasting terms (a constant, a length, a named
local): a comparison of two temporaries (`f(a) == f(b)`) would leave implications that no later check can use.

### 2.2 Halving (§2.2, §2.4)

- `x = a / b`, unsigned, is a definition, and gives `x ≤ a`: a division that completed had `b ≥ 1`.
- `t = a − b` with `b < a` derived gives `1 ≤ t`.
- `x = a / k` for a constant `k ≥ 2`, or `x = a >> k` for a constant `k ≥ 1`, with the lower bound of `a` at
  least 1, gives `x < a`: `⌊a / 2⌋ < a` for `a ≥ 1`.

With the offset rule that was there (`x = o + i`, `i < t`, `t = L − o` give `x < L`), `middle < hi`.

### 2.3 What a dying operand's definitions derived (§2.3)

When a local ends that is an operand of the definition of another term `x`, the comparisons the facts derive
between `x` and the lasting terms (constants, lengths, named locals) are added as facts, the nearest bounds only.
They are consequences of facts that held, about terms the kill does not change. Bounded: at most one closure for
each such kill, none past the budget of 128 facts, and each fact added is between `x` and a lasting term.

### 2.4 The join (§2.5)

A comparison of one path is kept in the form the other path states: `a < b` if both state it, `a ≤ b` if each
states one of `a = b`, `a < b`, `a ≤ b`. "States" is read from the facts as they stand, directly or through one
equality of `a` or of `b`, with no saturation, so a join costs what it did, in order of growth. Each kept fact holds
on both paths. A passed `BoundsCheck` also records its fact about the lasting terms its temporaries equal, so that
the paths of a loop state the same fact.

### 2.5 The length of a resized vector (§2.4)

`r = v.resize(n, fill)` gives "if `r` is `Ok`, then `len(v) = n`", carried by copies of `r` and by its discriminant
to the edge of the `Switch` that only `Ok` takes. Sound because `resize` makes the length exactly `n` or fails and
leaves the vector unchanged (ADR 0071 §2.1), and because the facts about `v` ended at the `&mut` the call was given:
the arguments are evaluated before that borrow is taken, so no fact about `v` is made between the borrow and the
call. Which vector the call resizes is read from the `&mut` temporary's own assignment, recorded as a fact that ends
with the temporary or the vector and is never rewritten to an equal term.

`optimisations.md` §2.3 rests on no fact being made about a local while a mutable loan of it is live. This fact is
made while the call's temporary is still in scope, so the rule requires that the temporary be the call's alone: not
a local the program names, and mentioned by no other statement or terminator of the function (counted once per
function that has a `resize`). Then nothing can change the vector through it after the call. A `resize` through a
named borrow reborrows it (`&mut *p`), which is not a borrow of the vector local, and gives no fact.

Why: `data-oriented` kept four index checks in each iteration of its loop over four vectors of a constant length,
11% of its time (measured 2026-11-12 with the checks compiled out). The Rust version makes the vectors with
`vec![0; N]`, whose length LLVM sees.

### 2.6 The budget counts what a proof reads (§2.5)

A proof is abandoned when more than 128 facts hold. All facts were counted, though the saturation reads only
comparisons and definitions; in a function with many tests most facts are implications, and with more comparisons
kept (§2.3, §2.4) such functions passed 128 and lost checks they had: 51 in the compiler's own sources with the
first form of these rules. The budget now counts comparisons and definitions only. The closure's size, which is what
the budget bounds, depends on those alone, so its worst case is what it was.

## 3. What it costs

The fixed point now builds a closure at some kills (§2.3), where before it built none until the checks were
decided. Measured 2026-11-12 on the compiler's own sources (`ceesafec build --emit c`, user time, two runs each):
8.1 s before, 8.45 s after; with `--no-bce --no-oce`, 4.5 s and 4.6 s.

## 4. What it does not do

- A vector reached through a borrow (`v: borrow Vector<T>`) is not a tracked local, as before: `v[middle]` there
  keeps its check. A slice parameter is tracked.
- `lo ≤ hi` is not kept by the loop (it needs `middle + 1 ≤ hi` from `middle < hi`), so `s[lo]` after the loop is
  not proved.
- `(lo + hi) / 2` is not proved: nothing relates the quotient of a sum to its terms. It can also overflow.

## 5. Tests and measurements

- `tests/corpus/bounds-checks/search.cee` and `.bce`: four searches proved, five near misses kept.
- `tests/corpus/bounds-checks/resized.cee` and `.bce`: six index checks after a `resize` removed, five near
  misses kept.
- `tests/corpus/overflow-checks/tested.cee` and `.oce`: four subtractions after an equality test removed, four near
  misses kept.
- The interpreter evaluates every removed check on every run (§2.8, §3.5): the run corpus, the property tests and
  the fuzzer's oracle run with these rules.
- No check the previous compiler removed is kept by this one, over the compiler's own sources, the benchmark suite
  and the run corpus; the counts are in ROADMAP §10.
