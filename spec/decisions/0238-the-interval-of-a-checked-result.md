# ADR 0238 — The interval of a checked result, a constant sum's exact value, and an upper bound after `x += 1`

- Status: **Accepted** by the owner, 2026-11-24 ("I accept ADR 0238."); proposed 2026-11-23, under the owner's instruction ("Lets go ahead with those remaing in the
  roadmap": the optimization and DX roadmap's Phase 2, items 1, 2 and 3); implemented, not released.
- Number: written as ADR 0218 on branch `dx/roadmap-phase0` (its commits name it so); renumbered 0238 when the
  branch was merged with `main`, which had given 0218 to another ADR.
- Scope: optimisations.md §2.4 ("Where facts come from", "An update in place") and §3.2; ADR 0102; `src/backend/check_elimination.cee`
  (`intervalFacts`); tests/corpus/overflow-checks.

## 1. Context

A checked `+`, `-` or `*` whose result is in its type's range on every run loses its check (§3.3). What the analysis
then knows of the **result** depends on the operation:

- an unsigned `+` or `-` is a definition (§2.4), from which bounds of the result follow;
- a `*`, a signed `+` or `-` and a negation give no fact at all: the result has its type's whole range.

So in `t ^= 7 * i + 1`, under `i < 1000`, the `*` loses its check and the `+` keeps it: `7 * i` is a `u32` that may
be anything. The benchmarks' `numeric` (`i * M + j`, `k * M + j`, two in its innermost loop) and `hash-map`
(`7 * i + 1`, `7 * i + 3`) keep checks for this reason alone.

## 2. Decision

### 2.1 The interval of a checked result

After `x = a op b` for `op` among `*`, signed `+` and signed `-`, and after `x = -a`, the analysis records the
**interval** of the result:

- from the operands' bounds before the operation (§3.2), the least and greatest values of the operation at their
  corners: `lo(a) + lo(b)` to `hi(a) + hi(b)` for `+`, `lo(a) − hi(b)` to `hi(a) − lo(b)` for `-`, the least and
  greatest of the four products for `*`, `−hi(a)` to `−lo(a)` for a negation;
- as the facts `low ≤ x` and `x ≤ high` with constant terms, each only when it is narrower than `x`'s type range
  (one the type gives is no fact, as in §2.3);
- for `*`, nothing when a corner lies outside the type.

It holds whether the operation's check is removed or kept. Arithmetic is checked (type-system.md §6): execution goes
past `x = a op b` only when the exact result is in the type, and the exact result of an operation lies between its
values at the corners of the operands' intervals (`+` and `-` are monotonic in each operand, a product's extremes are
at the corners). So the fact needs no proof of its own, only the operands' bounds, which already hold.

Unsigned `+` and `-` keep their definitions and get no interval: what the interval would say follows from the
definition and the operands' bounds, and two facts for one temporary would only fill §2.5's budget.

### 2.2 A constant sum has its exact value (a fix)

§2.4 already says that `x = c op d`, for constants and `+`, `-`, `*`, gives `x` its exact value. For unsigned `+`
and `-` the analysis made a definition instead, since constants are terms, and a definition gives a sum only lower
bounds. So `for (i in 2..LIMIT + 1) { i * i }` (the benchmarks' sieve) kept the `*`'s check, which `2..LIMIT` and a
literal did not: the loop's end `_2 = Add(const 1000, const 1)` had no upper bound. (The roadmap guessed at the
loop's join; the NIR shows the end temporary never had the fact.) Two constants are now decided first, as §2.4 says;
the exact value says everything the definition did.

### 2.3 An upper bound after `x += 1`

An update in place (ADR 0102) keeps, after `x = x + b`, only the lower bounds of the old value: the new one is at
least as large. One upper bound holds as well: after `x = x + 1`, each `x < u` of the old value gives `x ≤ u` of the
new one, since the old value was at most `u − 1` and the sum is exact. Only for the constant 1: `x < u` says nothing of
`x + 2`.

That is a counter that writes below a length and then steps: in the benchmarks' `parsing`, `digits[n] = …; n += 1;`
leaves `n ≤ 20` after the loop, so the countdown `while (n > 0) { n -= 1; … digits[n] … }` that follows needs no
check.

## 3. Options not taken

- **Only for an operation whose check was removed** (the roadmap's first wording). Narrower and no more sound: the
  interval holds after a kept check too.
- **A definition for `*`**, as for unsigned `+`. The derived-fact rules (§2.4) have nothing to say of a product, and
  the closure would carry a term no rule reads. The interval is what the next operation needs.

## 4. Consequences

- More checks are removed; the output of a program does not change (§1). The interpreter still evaluates every
  operation exactly and stops with exit 70 if a removed check would fail (§3.5).
- Up to two more facts per such temporary, which end with it (§2.3) unless kept as comparisons to constants.

## 5. Implementation and tests

- `intervalFacts` (src/backend/check_elimination.cee), called by `assignFacts` with the other facts of an
  assignment; in `newFacts`, the constants' branch before the definitions' (§2.2); in `updateFacts`, `plusOne`
  (§2.3).
- tests/corpus/overflow-checks/interval.cee with its `.oce`: the products of the benchmarks and near misses that keep
  their check (a product whose operand is unbounded, a sum past the type, a negation of a value that may be `MIN`), and the loop over `2..LIMIT + 1` (§2.2).
- tests/corpus/bounds-checks/incremented.cee with its `.bce` (§2.3): the countdown after a counted fill, and near
  misses (a step of 2, a counter tested with `<=`).
- tests/corpus/bounds-checks/updated.cee: its near miss `doubled` multiplied by 1, which §2.1 now proves (`i` is
  exactly 1); it multiplies by 2, so the near miss stays one, and its expected output is 5 (was 3).
- Runs recorded in ROADMAP §10.
