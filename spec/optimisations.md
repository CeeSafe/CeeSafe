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

A **fact** relates two terms: `a = b`, `a ≤ b`, `a < b`. Terms are mathematical integers and facts are integer
relations. That is sound because CeeSafe's arithmetic is checked: `+`, `-`, `*` panic rather than wrap (§6), and `as`
is lossless (§7). A **definition** `x = a op b` records how a local was computed, for `op` among `+`, `-`, `%`, `&`
and `>>` on unsigned integers. A **condition** `c = a cmp b` records the comparison that produced a `bool` local.

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
- **Passed checks**: after `BoundsCheck(i < L)`, `i < L`; after `RangeCheck(lo, hi, L)`, `lo ≤ hi` and `hi ≤ L`;
  after `LengthCheck(a, b)`, `a = b`.

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

A check `BoundsCheck(i < L)` is removed when `i < L` is derived from the facts that hold before it.

### 2.5 The analysis
Forward dataflow over the control-flow graph. The facts at a block's entry are those holding on every incoming edge;
a block not yet reached holds everything. At a join of two paths the analysis keeps:
- the facts of both paths;
- an implication "if `c` is `w`, then F" of one path, if the other path knows `c` is not `w` (it holds vacuously
  there) or knows F itself;
- a comparison or definition F of one path as "if `c` is not `w`, then F", if the other path knows `c` is `w`.

`a && b` and `a || b` lower to joins of a path that computed `b` and a path that assigned a constant, so
`while (i < s.length && s[i] != 0)` and `if (k == 0 || n - k < 4) { return; }` keep what their tests showed.

Every rule keeps only facts that hold on both paths, so each fact in a block's entry holds on every execution that
reaches it. Loops are iterated until no entry changes. The join is not a plain intersection, so the iteration is
bounded (64 visits per block on average). A function that reaches the bound has nothing removed. Implications use a
length rather than a compiler temporary known to equal it, since the temporary may be gone where the implication is
used. Derived facts are computed by saturation, which does not depend on the order facts were found, so the result
is deterministic.

A proof considers at most **128 facts** (M15 S3); where more hold, the check stays. The cost of saturation grows with
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
  `removed` or `kept`.

### 2.8 How it is checked
- **The interpreter** evaluates every bounds check, removed or not. If a removed one would fail, it stops with an
  interpreter fault (exit status 70, "an eliminated bounds check fails"): a compiler bug. Every interpreter run checks
  this: the corpus agreement tests, the fault-injection sweep, the property tests and the fuzzer's oracle.
- **The run corpus** builds every program with the analysis (every configuration) and without it (`clang -O2 no-elim`),
  so output and panics are compared both ways. So does the fuzzer's oracle.
- **`tests/corpus/bounds-checks/*.cee`**: snapshots of `--explain-bounds-checks`
  (`.bce`) for loops that must be proved, and for near misses that must keep their check (a reassigned index, a
  mutated vector, a borrowed index, `<=` bounds, a loop bounded by another slice's length, a stable vector, whose
`push` takes only `borrow this`).

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
  order, with `removed` or `kept`.

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
- **Variants.** One per (function, bindings), where the bindings map each bound parameter index to a body. They are
  made from a worklist in the order the program's functions and their calls are visited, so the result is
  deterministic. A recursive call with the same known argument goes to the same variant. Generic functions are
  specialised as their instances (§20.4); a function with no NIR (an `extern`) is never specialised.
- **Not specialised:** a closure read from a field, an element or a `dyn fn` (§37), a `Box<dyn fn…>`, a closure whose
  value depends on a branch, and a `Thread.both` argument (called through the runtime, c-backend §2).
- A variant is the same NIR with call targets fixed: the same statements, checks and frame. Borrow and effect
  checking run before the pass and are not repeated.
- **Async instances are not variants** (type-system.md §32.5, ADR 0037). A call with an async closure for an
  `@effectsOf` parameter needs its callee's instance, which awaits the closure, so it is made always, after drop
  elaboration and before the effect checker, whatever `--no-closure-specialisation` says. Async functions, instances
  among them, are not specialised (their frames are laid out once).

### 5.2 What a program can observe
Nothing: output, exit status, panics and allocations are unchanged. Only the C function a call goes to changes, and the
generated C has one more function per variant (`ceeXS<n>_<name>`, c-backend §2.1).

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
