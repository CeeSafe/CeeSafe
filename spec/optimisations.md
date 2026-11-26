# Optimisations — preconditions and checks

Status: **normative** from M9c (2026-10-25); §3 from M9d; §4 from M9e. Master prompt §16: an optimisation needs safety preconditions, tests,
correctness checks, debugging support and measurements, against a correct baseline. This document states each
optimisation's preconditions, which must hold for every program the checker accepts, and how the toolchain checks
that the optimisation changes nothing a program can observe. Results are in `milestone-9.md`.

## 1. Rules for every optimisation

- It must not change what a program does: its output, exit status, panics (which one, and where), and its
  allocations as counted by the runtime. A removed check must be one that cannot fail. The one exception is
  placement (§4), which removes allocations of a placement-transparent allocator by design.
- It has a switch that turns it off (`--no-…`), so that the unoptimised build stays available as an oracle and for
  measuring the gain.
- It is decided from facts the compiler already proves (types, loans, checked arithmetic), never from heuristics
  about what a program "probably" does, and never by weakening a check of `ownership.md`.
- The interpreter checks it on every program it runs, where it can (below), and the run corpus and the fuzzer run
  with it both on and off.
- Its output is deterministic, like the rest of the compiler.

## 2. Bounds-check elimination (M9c)

### 2.1 What is removed
A `BoundsCheck(i < L)` (`intermediate-representation.md`; the index check of `type-system.md` §16.3) whose condition
holds every time the statement is reached. The C backend then emits no `cee_check_index` for it. Nothing else is
removed: range checks (`a[lo..hi]`), length checks (`copyFrom`), overflow checks and every other panic stay. Because a
removed check never fails, which panic a program reaches, and where, does not change.

The analysis is `src/backend/check_elimination.cee`, which also decides overflow checks (§3). It runs on each function's final NIR (after drop
elaboration), and its result is the set of checks it proved. The C backend and the interpreter consult that set.
The NIR itself is not changed.

### 2.2 Terms and facts
A **term** is an integer the analysis can name at a program point:
- the current value of a tracked local (§2.3);
- an integer constant;
- the length of a tracked slice, string view, vector or string local.

A **fact** relates two terms: `a = b`, `a ≤ b`, `a < b`, `a ≠ b` (ADR 0100). Terms are mathematical integers and
facts are integer relations. That is sound because CeeSafe's arithmetic is checked: `+`, `-`, `*` panic rather than wrap (§6), and `as`
is lossless (§7). A **definition** `x = a op b` records how a local was computed, for `op` among `+`, `-`, `/`, `%`,
`&` and `>>` on unsigned integers. A **condition** `c = a cmp b` records the comparison that produced a `bool` local.

### 2.3 Tracked locals
A fact may mention only a tracked local:
- an integer, `bool`, slice or `StringView` local, for its value (and a slice's or view's length);
- a `Vector` or `String` local, for its length.

A tracked local changes only through an assignment to it, which the analysis sees, or through a mutable loan of its
own storage: `borrow mut i`, or `push`, `pop`, `clear` and `append`, which take `borrow mut` of a vector. While such a
loan is live, every other use of the local is rejected (B1–B2), so no new fact about it can be made. After the loan,
nothing may write the local through a pointer made from it (U2, ADR 0010). A shared loan cannot change it. A cell
(`RawCell`, `Cell`, `RefCell`, type-system.md §41) is a struct, so never a tracked local, and nothing inside one is a
fact: its contents change through shared loans. So facts

about a local end at each mutable `Ref` or `Slice` of a place rooted at it without a dereference, and a value or
length read after that is a new fact.

A tracked local's facts end at:
- an assignment to a place rooted at the local without a dereference;
- a mutable borrow of such a place, as above;
- being a target of `Unpack`;
- its `StorageDead`.

A local that is moved out is uninitialised until it is assigned again, and that assignment ends its facts.

A fact that ends with a local but also holds of a term known to be equal to it is kept, rewritten to that term.
So `t = length(s); m = t - 1; StorageDead(t)` still knows `m < len(s)`.

When a local ends that is an operand of the definition of another term `x` (`m = lo + t`, with the temporary `t`
ending), the definition ends, and with it everything derived through it. What the facts derive about `x` at that
point is kept as comparisons (ADR 0100): `x = u`, `x < u`, `x ≤ u`, `u < x` or `u ≤ x` for each **lasting** term `u`
(a constant, a length, or a local the program names) that does not mention the ending local. Only the nearest bounds
are kept: one that another lasting term lies between is left to transitivity, and one that `x`'s type gives anyway
(`0 ≤ x`) is no fact. These are facts derived from facts that held, about terms that have not changed, so they hold.
Nothing is kept past §2.5's budget.

### 2.4 Where facts come from
- **Copies**: `x = y` and `x = y as T` (lossless) give `x = y`. `x = length(s)` gives `x = len(s)`. `x = c` gives
  `x = c`, and `x = c op d` for constants and `+`, `-`, `*` gives `x` its exact value when that is in `x`'s range (M9d).
  `x = e as T` where `e` is not a term (a field, an element) gives `min(E) ≤ x ≤ max(E)` for `e`'s type `E` (M9d).
- **Definitions**: `x = a op b` with unsigned operands, unless `x` is itself one of `a`, `b` (as in `i = i + 1`, where
  the old value is gone). If a definition `y = a op b` of the same terms already holds, `x = y` holds after the
  assignment, even when `a` or `b` is `x`'s old value: `i = i + 1` after a test of `i + 1 < n` gives `i < n`.
- **Comparisons**: a `bool` local `c = a cmp b` comparing integers (`<`, `<=`, `>`, `>=`) gives two implications:
  if `c` is true, the relation holds; if false, its negation (`a < b` false is `b ≤ a`). A copy of `c` or `!c`
  carries them, and a `Branch` on `c` adds the relation of the edge it takes. A `bool` local assigned a constant is
  known to hold it.
- **Equality tests** (ADR 0100): a `bool` local `c = a == b` comparing integers gives: if `c` is true, `a = b`; if
  false, `a ≠ b`. `c = a != b` gives the same with true and false exchanged. So after
  `if (n == 0) { return; }`, `n ≠ 0`. Only for lasting terms (§2.3; a temporary that equals a length counts as the
  length): what is known of two temporaries, as in `f(a) == f(b)`, ends with the statement.
- **A resized vector** (ADR 0100): `r = v.resize(n, fill)`, for a tracked vector local `v` and a term `n`, gives
  "if `r` is `Ok`, then `len(v) = n`": `resize` either makes the length `n` or fails with the vector unchanged
  (`type-system.md` §23). A copy of `r` and its discriminant carry the implication, and the edge of a `Switch` on
  the discriminant that only `Ok` takes adds `len(v) = n`. So after `v.resize(n, 0)?` or `v.resize(n, 0)!`, a
  loop `for (i in 0..n)` indexes `v` with no check. The vector is the one the call's `&mut` temporary was made
  from; any other borrow of `v`, or assignment to it, ends that. The temporary must be the call's alone: a local
  the program does not name, which no other statement reads, copies or reborrows. This is the one fact made about a
  local while a mutable loan of it is still in scope (§2.3), and that condition is what makes it hold: nothing can
  change the vector through the loan after the call. A `resize` through a named borrow (`p.resize(n, 0)`) gives no
  fact.
- **An update in place** (ADR 0102): `x = x op b` on an integer local keeps, about the new value, the comparisons
  the update cannot have made false. After `x = x - b` (and, unsigned, `x / b` and `x >> b`) the new value is at
  most the old one: each `x ≤ u`, `x < u` and `x = u` gives `x ≤ u`, or `x < u` if it was. After `x = x + b` the new
  value is at least the old one: each `l ≤ x`, `l < x` and `l = x` gives `l ≤ x`, or `l < x`. When `b` is a constant
  of at least 1 and the operation is `+` or `-`, the result is strict: `x ≤ u` gives `x < u`. After `x = x + 1`, each `x < u`
  also gives `x ≤ u` (ADR 0238): the old value was at most `u − 1`. `b` must not be
  negative: any unsigned operand, or a constant of at least 0 for a signed `x`. So in
  `var n = s.length; while (n > 0) { n -= 1; … s[n] … }` the check is removed. (§2.3 ends the facts of an assigned
  local because its old value is gone; these are the ones that hold of the new value whatever the old one was.)
- **The interval of a checked result** (ADR 0238): after `x = a * b`, a signed `x = a + b` or `x = a - b`, or
  `x = -a`, `low ≤ x` and `x ≤ high`, where `low` and `high` are the least and greatest values of the operation at
  the corners of its operands' bounds (§3.2), each only where it is narrower than `x`'s type range; for `*`, none
  when a corner lies outside the type. Execution goes past the operation only if its exact result is in the type, so
  this holds whether its check is removed or kept. So under `i < 1000`, `7 * i` is at most 6993 and the `+` of
  `7 * i + 1` cannot overflow. Unsigned `+` and `-` have their definitions instead.
- **A reborrowed slice** (ADR 0102): `t = &(*s)` or `t = &mut (*s)` for slice locals `s` and `t`, the place being
  `s` dereferenced and nothing more, gives `len(t) = len(s)`. `s.swap(i, j)` on a `SliceMut` local makes one.
- **Passed checks**: after `BoundsCheck(i < L)`, `i < L`; after `RangeCheck(lo, hi, L)`, `lo ≤ hi` and `hi ≤ L`;
  after `LengthCheck(a, b)`, `a = b`. Where `i` or `L` is a compiler temporary known to equal a lasting term (§2.3),
  `BoundsCheck` also gives the fact about that term (`m < len(s)` for `t = m; u = length(s); BoundsCheck(t < u)`),
  which is the same fact on every path that made the check, whatever its temporaries were.

Derived facts:
- a < b implies a ≤ b;
- ≤ and < are transitive (and a ≤ b < c gives a < c);
- constants compare as integers;
- equal definitions (the same operation on equal terms) have equal results;
- x = a − b gives x ≤ a and b ≤ a (the subtraction did not panic), and x < a when b is a constant ≥ 1;
- t = L − o with a constant c ≥ 1 and c ≤ t gives o < L (some length remains after o);
- x = a % n gives x < n;
- x = a & m gives x ≤ m and x ≤ a;
- x = a >> k gives x ≤ a;
- x = a + b gives a ≤ x and b ≤ x, and a < x when b is a constant ≥ 1;
- x = o + i, with i < t (or i ≤ t) and t = L − o for the same `o`, gives x < L (or x ≤ L): an offset plus an index
  below the remaining length stays below the length.
- x = a + m and y = b + m (the same `m`) with a ≤ b give x ≤ y, and with a < b, x < y.

And, from ADR 0100:
- a ≠ b with a ≤ b gives a < b;
- t = a − b with b < a gives 1 ≤ t;
- x = a / b gives x ≤ a: the unsigned division did not panic, so b ≥ 1;
- x = a / k for a constant k ≥ 2, or x = a >> k for a constant k ≥ 1, with a's lower bound (§3.2) at least 1, gives
  x < a.

Together with the offset rule these prove a binary search: in `while (lo < hi) { let m = lo + (hi - lo) / 2; … }`,
`t = hi − lo` is at least 1, `t / 2 < t`, so `m < hi`, and with `hi ≤ len(s)` kept by the loop (§2.5), `s[m]` is in
range and `lo + t / 2` cannot overflow (§3.3).

A check `BoundsCheck(i < L)` is removed when `i < L` is derived from the facts that hold before it.

### 2.5 The analysis
Forward dataflow over the control-flow graph. The facts at a block's entry are those holding on every incoming edge;
a block not yet reached holds everything. At a join of two paths the analysis keeps:
- the facts of both paths;
- an implication "if `c` is `w`, then F" of one path, if the other path knows `c` is not `w` (it holds vacuously
  there) or knows F itself;
- a comparison or definition F of one path as "if `c` is not `w`, then F", if the other path knows `c` is `w`.

- a comparison of one path, in the form the other path states it (ADR 0100): `a < b` if both state it, else
  `a ≤ b` if each path states one of `a = b`, `a < b`, `a ≤ b`; and for `a = b` on one path, `b ≤ a` likewise. A
  path states a relation directly, or through one equality of `a` or of `b` with another term (`hi = m` and
  `m < len` state `hi < len`). So a loop that starts with `hi = len(s)` and assigns `hi = m` with `m < len(s)`
  keeps `hi ≤ len(s)`. This is a reading of the facts as they stand, with no saturation: the join stays cheap.

`a && b` and `a || b` lower to joins of a path that computed `b` and a path that assigned a constant, so
`while (i < s.length && s[i] != 0)` and `if (k == 0 || n - k < 4) { return; }` keep what their tests showed.

Every rule keeps only facts that hold on both paths, so each fact in a block's entry holds on every execution that
reaches it. Loops are iterated until no entry changes. The join is not a plain intersection, so the iteration is
bounded (64 visits per block on average). A function that reaches the bound has nothing removed. Implications use a
length rather than a compiler temporary known to equal it, since the temporary may be gone where the implication is
used. Derived facts are computed by saturation, which does not depend on the order facts were found, so the result
is deterministic.

A proof considers at most **128 facts** (M15 S3); where more hold, the check stays. The facts counted are the ones a
proof reads, comparisons and definitions (ADR 0100): implications, conditions and known values are not read by the
saturation, and in a function with many tests they are most of what holds. The cost of saturation grows with
the number of facts, cubically, and a long straight-line function adds one at each step, so a sum of 300 terms took
17 s to compile before the budget. That covers both this section's index checks and §3's overflow checks. The most
facts any proof needs in the repository's programs, benchmarks and libraries, or in CosmoOS's CeeSafe sources, is 44
(measured 2026-10-30). Their generated C was byte-identical with and without the budget.

### 2.6 Unsafe code
The rule of ADR 0010 is the only assumption about `unsafe` code. The unsafe code in this repository (`lib/core`,
`integrations/cosmoos/**`, `tests/corpus/**`, `programs/`, `benchmarks/`) was audited for writes through a pointer
made from a reference after its target was used directly (2026-10-25): none, as recorded in `milestone-9.md` (9c).

### 2.7 Switch and diagnostics
- On by default. `ceesafec build --no-bce` and `ceesafec run --no-bce` keep every check (`CEmitOptions.BoundsCheckElimination`,
  `InterpreterOptions.BoundsCheckElimination`).
- `ceesafec build --explain-bounds-checks` prints each index check of the package, in source order, with
  `removed` or `kept`: what that build does, so with `--no-bce` every line says `kept`.

### 2.8 How it is checked
- **The interpreter** evaluates every bounds check, removed or not. If a removed one would fail, it stops with an
  interpreter fault (exit status 70, "an eliminated bounds check fails"): a compiler bug. Every interpreter run checks
  this: the corpus agreement tests, the fault-injection sweep, the property tests and the fuzzer's oracle.
- **The run corpus** builds every program with the analysis (every configuration) and without it (`clang -O2 no-elim`),
  so output and panics are compared both ways. So does the fuzzer's oracle.
- **`tests/corpus/bounds-checks/*.cee`**: snapshots of `--explain-bounds-checks`
  (`.bce`) for loops that must be proved, and for near misses that must keep their check (a reassigned index, a
  mutated vector, a borrowed index, `<=` bounds, a loop bounded by another slice's length, a stable vector, whose
`push` takes only `borrow this`). `search.cee` (ADR 0100) has the binary searches that must be proved and their near
misses: a midpoint of `(lo + hi) / 2`, an upper end that may pass the length, a search of another slice.
`updated.cee` (ADR 0102) has the loops whose index is updated in place and their near misses: an index stepped up
past its bound, an amount subtracted that may be 0, a product, a slice of part of the slice.
`resized.cee` has the loops over a vector after `resize` and their near misses: a `pop` after it, a result that was
not tested, the length of another vector, a resize through a named borrow.

Stable vectors (`type-system.md` §24.2) are not tracked: their length grows through a shared borrow.

### 2.9 Character boundaries in ASCII text (after M13)
A slice of a `StringView` (`v[lo..hi]`, type-system §22.2) checks the range and that `lo` and `hi` are character
boundaries. In text whose bytes are all ASCII (each below 0x80), every index is a boundary, so the boundary check
cannot fail. When the text is a local every assignment of which is a string literal of ASCII bytes, or a copy of
such a local, the C backend checks the range only (`cee_check_range` for `cee_check_str_range`). The range check stays.
- The analysis is per function and flow-insensitive, like §5.1's; it is part of `CheckElimination` and runs under the
  same switch (`--no-bce` keeps the boundary check too). `--explain-bounds-checks` lists each slice of a view with
  `boundary removed` or `boundary kept`.
- The interpreter checks boundaries on every slice; if one whose check was removed is not a boundary, it stops with
  an interpreter fault ("an eliminated boundary check fails"), as for §2.8.
- The run corpus's and the fuzzer's `clang -O2 no-elim` configuration builds without it.

## 3. Overflow-check elimination (M9d)

### 3.1 What is removed
The overflow check of an integer `+`, `-` or `*` (`type-system.md` §6) or of a negation, when the exact result is in
the range of its type every time the operation runs. The C backend then writes the operation without a check
(`(T)(a + b)`, with no undefined behaviour, since the exact result fits). Nothing else is removed: division and
remainder keep their zero-divisor and `MIN / -1` checks, shifts their amount checks, and conversions their range
checks. A removed check never fails, so which panic a program reaches, and where, does not change.

It is part of the same analysis as §2 (`src/backend/check_elimination.cee`), with the same facts at the same program
points, and it adds no new kind of fact. §2's facts treat checked arithmetic as exact; that stays true, since an
operation whose check is removed was proved not to overflow.

### 3.2 Bounds of a term
Every term has a range that holds whatever the facts say: a local's is its type's (`i32`: −2³¹ to 2³¹ − 1), a
length's is that of `usize`, and a constant's is itself. From the facts before the operation, a term `a` has:
- an **upper bound**, the least of `max(u)` over the terms `u` with `a ≤ u` derived (§2.4), and of `max(u) − 1`
  over those with `a < u` derived;
- a **lower bound**, the greatest of `min(l)` over the terms `l` with `l ≤ a`, and of `min(l) + 1` over those with
  `l < a`.

A bound that is a constant `k` with `a ≠ k` derived moves by one (ADR 0100): an unsigned `n` with `n ≠ 0` has the
lower bound 1, so `n - 1` cannot overflow.

An operand that is not a term (an element read from memory, a field) has its type's range.

### 3.3 When an operation cannot overflow
With the operands' bounds `[lo(a), hi(a)]` and `[lo(b), hi(b)]` and the result type's range `[MIN, MAX]`, the check is
removed when one of these holds:
- **by interval**: every combination of the bounds stays in `[MIN, MAX]`: for `a + b`, `lo(a) + lo(b) ≥ MIN` and
  `hi(a) + hi(b) ≤ MAX`; for `a - b`, `lo(a) − hi(b) ≥ MIN` and `hi(a) − lo(b) ≤ MAX`; for `a * b`, the four products of
  the bounds; for `-a`, `−hi(a) ≥ MIN` and `−lo(a) ≤ MAX`;
- **an unsigned subtraction** `a - b` with `b ≤ a` derived;
- **an unsigned addition** `a + b` whose sum `s`, added to the facts as the definition `s = a + b` (exact: §2.4's
  rules for `+` hold of the mathematical sum), has an upper bound of at most `MAX`. That covers an offset plus an
  index below the remaining length (`to + i < L`, §2.4), which the interval rule does not.

An operation whose operands are all constants is decided by its exact result alone, whatever the facts (M11, found by
the G8c fuzz campaign). Under a condition that is never true, such as `if (1u16 < 0u16)`, the facts contradict and
every rule above would hold vacuously. The check would then be removed from code that never runs, which is sound,
but the C would hold an overflowing constant expression such as `1 + 2147483647`, which C compilers reject under
`-Werror`.

The bound rule is what makes a counter safe: in `for (i in 0..n)` the increment `i + 1` runs after `i < n`, so
`hi(i) ≤ max(n) − 1` and `i + 1` does not overflow. A loop over a bound held in a constant (`for (s in 0..100)`) is
the same.

### 3.4 Switch and diagnostics
- On by default. `ceesafec build --no-oce` and `ceesafec run --no-oce` keep every overflow check
  (`CEmitOptions.OverflowCheckElimination`, `InterpreterOptions.OverflowCheckElimination`). `--no-bce` and `--no-oce`
  are independent.
- `ceesafec build --explain-overflow-checks` prints each checked `+`, `-`, `*` and negation of the package, in source
  order, with `removed` or `kept`: what that build does, so with `--no-oce` every line says `kept`.

### 3.5 How it is checked
- **The interpreter** evaluates every operation exactly, removed or not. If a removed one would overflow, it stops
  with an interpreter fault (exit status 70, "an eliminated overflow check fails"). As for §2, every interpreter run
  checks this.
- **The run corpus and the fuzzer** build every program without either elimination (`clang -O2 no-elim`) as well as
  with both, and compare.
- **`tests/corpus/overflow-checks/*.cee`**: snapshots of `--explain-overflow-checks`
  (`.oce`) for operations that must be proved (counters, constant bounds, `b ≤ a` subtractions, offsets below a
  length, widened operands) and near misses that must keep their check (a counter bounded by `<=` the type's
  maximum, a sum of two unbounded values, a subtraction tested the wrong way, a signed negation that may be `MIN`).
  `tested.cee` (ADR 0100) has the subtractions after an equality test that left, and their near misses: the test of
  another local, a signed operand, a local changed after the test.
  `interval.cee` (ADR 0238) has the operations whose operands are bounded by the interval of a checked result (a
  product, a signed sum, difference and negation), and their near misses: an unbounded operand, a product past the
  type, a negation that may be of `MIN`.

## 4. Placement (M5; on by default from M9e)

### 4.1 What changes
A `Box.create` site that `PlacementAnalysis` places (`memory-model.md` §5) keeps its value in a slot of the
function's frame instead of the heap. The rules and preconditions are `memory-model.md` §5 and §5.1; they are not
repeated here. Since M9e a site in a loop may be placed when its box ends within the iteration (§5.1).

### 4.2 What a program can observe
Output, exit status, panics and drop timing are unchanged. Allocation counts are not. A placed box is no allocation
attempt, so the allocation report (`CEESAFE_ALLOC_REPORT`) shows fewer attempts, and fault injection
(`CEESAFE_FAIL_ALLOC_AT`) cannot make a placed box fail. That is allowed only for an allocator whose contract says
allocation "may spuriously succeed": `SystemHeap` in userland (memory-model.md §5, rule 1d). `FailingAllocator`,
`KernelHeap` and user-defined allocators are never placed, so counting and failure injection stay exact for them.
A test that needs every `SystemHeap` attempt counted builds with `--no-placement`.

### 4.3 Switch and diagnostics
- On by default (M9e). `ceesafec build --no-placement` and `ceesafec run --no-placement` keep every box on the heap
  (`CEmitOptions.Placement`, `InterpreterOptions.Placement`). `--placement` is still accepted.
- `ceesafec build --explain-allocations` prints every allocation site of the package with its decision and reason. The
  libraries' sites are left out, as in §2.7's listings, since the libraries are part of every package.

### 4.4 How it is checked
- **The interpreter** applies the same decisions. A placed box lives in its own allocation, which ends at the box's
  drop, so a use after it is a fault. A site whose previous box is still alive when it runs again is a fault ("a
  stack-placed box's slot is reused while its previous box is alive"), since natively the site has one slot.
- **The run corpus and the fuzzer** build every program with placement (every configuration) and without it
  (`clang -O2 no-placement`), and compare.
- **What must hold**: the same output, panics and drop trace with and without placement, never more allocation
  attempts, and native code that agrees with the interpreter on everything, the allocation report included.
  `tests/corpus/placement/*.alloc` are reviewed snapshots of `--explain-allocations`.

## 5. Closure-call specialisation (OD-9a; ADR 0019)

### 5.1 What changes
A call through a closure (`CallClosure`, type-system §36.2) whose closure is *known* becomes a direct call to the
closure's body function, which the C compiler may inline. A call that passes a known closure to a function's
closure-typed parameter goes to a **variant** of that function: a copy in which that parameter is bound to the
closure's body, so the calls through it inside are direct too.

- **Known.** In a function, a local holds a known closure with body `K` when every assignment to it is one of:
  a reference to field 0 of an environment local every assignment of which is an aggregate whose field 0 is
  `K`'s address (a closure literal, or an `fn` value's adapter, made in this function); a copy, cast or reborrow of a
  local holding a known closure with body `K`; or, in a variant, the parameter bound to `K`. Any other assignment
  makes the local unknown. The analysis is per function, flow-insensitive, and iterated to a fixed point.
- **Closures held in fields** (ADR 0267). A local's facts say which places under it hold a known closure, by a path of
  field projections and dereferences (at most 8): an aggregate gives each field the operand's closure or facts, a copy
  or move of a place gives that place's facts, a borrow gives them under a dereference, and a call's result gives
  its variant's return facts (the facts every `return` of the variant gives). A call through a closure read from a
  place (`(*this).f`) is direct when that place's fact names it, and a call whose argument has facts goes to a
  variant bound by them. A fact names only a **write-once** closure field: one that no statement assigns through a
  projection or lends mutably, and that no value assigned in place (`*p = s`, `x.inner = s`) holds, nor any value a
  builtin or a C function is given a mutable reference or pointer to. A function has at most 64 variants bound by
  facts; past that its calls pass plain bindings only.
- **Variants.** One per (function, bindings), where the bindings map each bound parameter index (and, ADR 0267, each
  fact about a parameter's field) to a body. They are
  made from a worklist in the order the program's functions and their calls are visited, so the result is
  deterministic. A recursive call with the same known argument goes to the same variant. Generic functions are
  specialised as their instances (§20.4); a function with no NIR (an `extern`) is never specialised.
- **Not specialised:** a closure read from a field that is not write-once or whose struct was not made with a known
  closure, an element or a `dyn fn` (§37), a `Box<dyn fn…>`, a closure whose
  value depends on a branch, and a `Thread.both` argument (called through the runtime, c-backend §2).
- A variant is the same NIR with call targets fixed: the same statements, checks and frame. Borrow and effect
  checking run before the pass and are not repeated.
- **Async instances are not variants** (type-system.md §32.5, ADR 0037). A call with an async closure for an
  `@effectsOf` parameter needs its callee's instance, which awaits the closure, so it is made always, after drop
  elaboration and before the effect checker, whatever `--no-closure-specialisation` says. Async functions, instances
  among them, are not specialised (their frames are laid out once).

### 5.2 What a program can observe
Nothing: output, exit status, panics and allocations are unchanged. Only the C function a call goes to changes, and the
generated C has one more function per variant (`ceeXS_<len><name>B<k>_<len><body>…`, named from what it binds,
c-backend §2.1).

### 5.3 Switch and diagnostics
- On by default. `ceesafec build --no-closure-specialisation` and `ceesafec run --no-closure-specialisation` keep every
  closure call indirect (`CEmitOptions.ClosureSpecialisation`, `InterpreterOptions.ClosureSpecialisation`).
- `ceesafec build --explain-closure-calls` prints each call through a closure with its function and line, and whether it
  became direct (and to which body) or why not, then each variant made.

### 5.4 How it is checked
- **The interpreter** runs the specialised NIR when the pass is on, so the corpus's comparison of interpreter and
  native output covers it.
- **The run corpus and the fuzzer** build every program with the pass (every configuration) and without it
  (`clang -O2 no-closure-specialisation`), and compare.
- **What must hold**: the C of `benchmarks/sort` has no indirect comparator call left, and the run corpus
  gives the same output with and without the pass. `tests/corpus/closure-calls/*.calls` are reviewed snapshots of
  `--explain-closure-calls`.

## 6. Temporary forwarding (ADR 0162)

### 6.1 What changes
A value is not copied through a temporary it only passes through. Where a compiler temporary `T` is only assigned
and then read once, each `T = R` becomes `D = R`, and the read `D = use(T)` goes; a `T = use(S)` whose only read is
`return T` goes, and the block returns `S`. `T` is then no local of the C function. All of these must hold:

1. **`T` is a compiler temporary**: a NIR local without a name, not a parameter, not a `using` local, and not one of
   a `debugAssert`'s own temporaries (ADR 0087 §2.5).
2. **Every mention of `T` but one is a whole assignment of it** (`T = R`, no projection), and that one is the whole
   operand of a `Use`: `D = use(T)`, or a `return T` when `T` has one assignment. `StorageDead(T)` statements are
   not mentions, and go with it.
3. **Each `R` is a `Use`** (a place, moved or copied, or a constant) **or a `Call`** with no task role (not awaited,
   run or spawned). For `return T`, a `Use`.
4. **No `R` reads `D`'s local.**
5. **`T`, each `R` and the read have one type.**
6. **The way from each assignment to the read is straight and empty**: the rest of the assignment's block, then,
   through each `goto`, the next block from its start, up to the read, with nothing on it but `StorageDead`
   statements, none of a local that `D`'s place mentions (for `return T`: that `S`'s place mentions). Each block the
   ways pass into is entered only by them.

It runs to the end of a chain: `T1 = R; T2 = T1; D = T2` becomes `D = R`. So the value of a `match` or an `if` used
as an expression is written by each arm where it is going (`let x = if (c) a else b;` assigns `x` in each branch).
Named locals are never removed.

### 6.2 What a program can observe
Nothing: `R` is evaluated where it was, and `D` is written before anything can read it (rule 6). Output, exit status,
panics, drop order and allocation counts are unchanged. A function's frame holds fewer values, and an async
function's task frame fewer fields.

### 6.3 Switch and diagnostics
- On by default. `ceesafec build --no-temporary-forwarding` and `ceesafec run --no-temporary-forwarding` keep every
  temporary (`CBuildOptions.temporaryForwarding`).
- It runs on the NIR that is compiled and the NIR that is interpreted, after closure-call specialisation (§5).
  Checks, diagnostics and `--dump-nir` see the NIR before it.

### 6.4 How it is checked
- **The interpreter** runs the forwarded NIR: a read of a local whose storage has ended is a fault there, so a
  value forwarded past the end of what it reads would be caught.
- **The run corpus** runs with it and without it (`--no-temporary-forwarding`), natively and interpreted, and the
  fault sweep takes the switch among its others.
- **`tests/corpus/emit-c/temporary-forwarding.c`** is a reviewed snapshot: a forwarded call result, a chain, an
  `if` value, a `return`, and each rule as a case that is not forwarded.

## 7. Fill elision (ADR 0239)

### 7.1 What changes
An array literal is not written when the loop after it overwrites every element before anything reads one. An
assignment `a = [e; N]` is elided when all of these hold:

1. **The literal**: `a` is a local of type `[T; N]` with `N ≥ 1`, assigned whole (no projection), and `T` needs no
   drop (§16.2 of type-system.md already refuses a repeat of such a `T`, N0259).
2. **The way to the loop**: from the assignment, the rest of its block, then through each `goto` the next block from
   its start, through blocks entered only by the way, up to a block entered twice, the loop's header. No statement
   on it mentions `a`, but `x = Length(a)`, which is the constant `N`.
3. **The loop counts from 0 to `N`**: the header holds `c = Less(t, n)` alone and ends in `branch c ? body : exit`; on
   the way `t` is assigned 0, and `n` is `N` (a constant, or a local assigned the constant `N` or `Length(a)`). The
   loop is the blocks reached from `body` without passing the header: none is the exit or on the way, none returns or
   ends the process, and exactly one, the latch, goes back to the header, by a `goto`. In the loop `t` is assigned
   only by `t = Add(t, 1)` in the latch and read only by whole copies, and `n` is not mentioned.
4. **One store each iteration, at the index**: one statement of the loop mentions `a`, once: `a[x] = v`, where `x` is
   `t` or a chain of whole copies of it, each assigned once in the loop, in the store's block or the body's first
   block, before the store and the increment. The store's block is on every way from `body` to the latch, and the
   store comes before the increment.
5. **Unconditional code**: no block on the way or in the loop is compiled under a condition (`cfg`, ADR 0080) or
   belongs to a `debugAssert` (ADR 0087 §2.5), which a build may leave out.

Then iteration `k` stores `a[k]` for every `k` below `N`, and nothing reads `a` before the loop ends.

### 7.2 What a program can observe
Nothing: no element of the literal is read. A panic during the loop ends the process, or, in a task of an isolated
unit (type-system.md §40.10), the unit, whose frames are never resumed or read; while the loop runs its store needs
`a` exclusively, so nothing else can reach `a`. Output, exit status, panics, drop order and allocation counts are
unchanged. In C the array is declared, initialised, at the top of its function, so an elided literal leaves what it
held, never indeterminate memory.

### 7.3 Switch and diagnostics
- On by default. `ceesafec build --no-fill-elision` and `ceesafec run --no-fill-elision` write every literal
  (`CBuildOptions.fillElision`).
- It runs on the NIR that is compiled and the NIR that is interpreted, after temporary forwarding (§6)
  (`elideFills`, src/nir/fill_elision.cee; `NirProgram.elidedFills`). Checks, diagnostics and `--dump-nir` see the
  NIR before it.

### 7.4 How it is checked
- **The interpreter** marks an elided literal's bytes uninitialised instead of writing them, so reading an element
  before its store is a fault ("use of uninitialised memory", exit status 70) on every program it runs.
- **The run corpus** runs it natively and interpreted, and the fault sweep takes `--no-fill-elision` among its
  switches.
- **`tests/corpus/emit-c/fill-elision.c`** is a reviewed snapshot: the fill, the fill in an outer loop, the loop to
  `a.length`, and near misses that keep their literal (a loop to `N − 1`, a store at `j + 1`, a store under an `if`,
  a read of `a` in the loop, a `break`). `run-pass/fill-elision.cee` runs the same cases.
