# ADR 0239 — Fill elision: an array literal that a counted loop overwrites in full is not written

- Status: **Accepted** by the owner, 2026-11-24 ("I accept your recommendation. So yes to 1-3."), with §5's three
  questions taken as recommended; proposed the same day at the owner's instruction ("Go ahead and write ADR.", after
  the profiling of the benchmarks' slowest workloads against C, ROADMAP §10, 2026-11-24). Implemented (§6); not
  released.
- Number: written as ADR 0219 on branch `dx/roadmap-phase0` (its commits name it so); renumbered 0239 when the
  branch was merged with `main`, with the branch's other three, whose numbers `main` had given to other ADRs.
- Scope: optimisations.md, a new §7; `src/nir/` (the analysis), `src/backend/` (the C), `src/interpreter/` (the
  oracle), `src/main.cee` (the switch); tests/corpus/emit-c and run-pass.

## 1. Context

An array must be initialised before it is used (type-system.md), so a buffer is written as a literal and then
filled:

```ceesafe
var a = [0u64; 32];
for (j in 0..32usize) { a[j] = (i + j as u64) % 1000; }
```

The C backend writes the literal as a loop of stores, and clang does not remove it: it cannot see that the next loop
writes every element before any is read. In the benchmarks' `stack-alloc`, where this runs 20 million times, the
literal's stores cost about 120 ms of 1,077 (11%), measured on the Ubuntu box rq with ceesafec's own clang flags, the
emitted C with and without the store loop, five runs each (ROADMAP §10, 2026-11-24). The C version of the benchmark
declares the array without initialising it, which CeeSafe cannot do, and should not have to.

The profiling found no other change of this size that the compiler could make by itself: the rest of the gap to C is
overflow checks that can fail (`sum += a[j]`), page faults both versions take, and code alignment.

## 2. Decision

### 2.1 What changes

An assignment `a = [e; N]` is **elided** (not executed) when every element of `a` is written before any can be read.
The proof is one pattern, the one a fill takes; all of these must hold:

1. **The literal.** `a` is a local of array type `[T; N]` with `N ≥ 1`, the assignment is to the whole local (no
   projection), and `T` needs no drop (no drop glue, type-system.md §14.5, so an element overwritten without being
   dropped is the same as one dropped). `e` is an operand: reading it has no effect.
2. **The way to the loop.** From the assignment, the rest of its block, then through each `goto` the next block from
   its start, up to the loop's header, the way that temporary forwarding uses (§6.1 rule 6): every block it passes
   into is entered only by it, and no statement on it mentions `a`.
3. **The loop is counted from 0 to N.** The header is `c = Less(t, n)` and `branch c ? B : X`, where on the way `t`
   is assigned the constant 0 and `n` the constant `N` (or `n` is that constant); in the loop, `t` is assigned only by
   `t = Add(t, 1)` in the block that goes back to the header, and `n` not at all. The loop is the blocks reached from
   `B` without passing the header; every one of them leads back to the header, and none leaves the loop in another
   way (`return`, a `break`'s `goto` to outside). A panic may (§2.2).
4. **One store, every iteration, at the index.** In the loop exactly one statement mentions `a`: `a[x] = v`, where `x`
   is `t`'s value of this iteration (a copy of `t`, or a copy of a copy, made in the loop before the store and not
   assigned again), and `v` does not mention `a`. Its block is on every way from `B` to the increment of `t`, before
   it.

Then iteration `k` writes `a[k]`, for every `k` from 0 to `N − 1`, before the loop reads anything of `a` (it reads
nothing) and before anything after the loop can (the loop leaves only through the header, after iteration `N − 1`).

### 2.2 What a program can observe

Nothing: no element of the literal is ever read. A panic during the loop ends the process (OD-5), or, in a task of
an isolated unit (type-system.md §40.10, ADR 0175), ends the unit, whose frames are kept and never resumed, freed or
dropped (§14.5). Either way the half-written array is not read again: while the loop runs, its store needs `a`
exclusively (B1–B2), so no reference to `a` exists through which another task, or the panic hook, could read it.
Output, exit status, panics (which, and where), drop order and allocation counts are unchanged.

In C, the array is declared, initialised, once at the top of the function (as now); an elided literal leaves what the
array held, so its memory is never indeterminate and no behaviour is undefined.

### 2.3 Switch and diagnostics

- On by default. `ceesafec build --no-fill-elision` and `ceesafec run --no-fill-elision` write every literal
  (`CBuildOptions.fillElision`, `InterpreterOptions.fillElision`).
- It runs on the NIR that is compiled and the NIR that is interpreted, after temporary forwarding (§6). Checks,
  diagnostics and `--dump-nir` see the NIR before it.

### 2.4 How it is checked

- **The interpreter** is the oracle. For an elided literal it marks the bytes of `a` uninitialised, as it does a
  place moved out of, instead of writing them; reading an element before its store is then its fault "use of
  uninitialised memory", exit status 70. So a wrong proof is caught on every program the interpreter runs.
- **The run corpus** runs with it and without it (`--no-fill-elision`), natively and interpreted, and the fault sweep
  takes the switch among its others.
- **tests/corpus/emit-c/fill-elision.c** is a reviewed snapshot: the fill, the fill inside an outer loop, the loop
  to `a.length`, and each rule as a near miss that keeps its literal (a loop to `N − 1`, a store at `j + 1`, a store
  under an `if`, a read of `a` in the loop, a `break`).
- **run-pass/fill-elision.cee**: the same cases with their output, with `--check-leaks`, natively and interpreted.

## 3. Options not taken

- **A general analysis of definite writes** (each element written on every way before a read). It covers more —
  partial fills, two loops, a fill through a helper — at the cost of an array-section analysis. The pattern covers the
  one measured case; a later ADR can widen it if a program shows a need.
- **Uninitialised arrays in the language** (`var a: [u64; 32];` with a definite-assignment rule for elements). A
  language change, with the same proof moved into the checker; it would make the user write what the compiler can
  see.
- **`memset` instead of the store loop.** Clang treats it as the same stores; it does not remove them either.

## 4. Consequences

- `stack-alloc`: about 11% faster on rq (1,077 → about 955 ms). The digits buffers of `parsing` and `string-building`
  are filled only in part and are not covered.
- One more switch, run-corpus configuration and interpreter mode.

## 5. Questions for the owner (answered 2026-11-24: as recommended)

1. **Scope**: only the counted fill of §2.1 (recommended), or a general definite-write analysis (§3) now?
2. **Name of the switch**: `--no-fill-elision` (recommended), or another.
3. **`n` as `a.length`**: `for (j in 0..a.length)` reads `a`'s length, a constant for an array. Should rule 3 accept
   it as the constant it is? Recommended: yes, a `Length(a)` of an array is not a read of its elements.

## 6. Implementation (2026-11-24)

- `elideFills` (src/nir/fill_elision.cee) runs in `generateC` after temporary forwarding, so `build` and `run --interp`
  see the same NIR; `NirProgram.elidedFills` marks the statements. `c_body.cee` writes nothing for one; the
  interpreter's `Assign` marks the target's bytes uninitialised (`Memory.uninitialise`). The mention counting of
  temporary forwarding is now per statement (`forwardCountStatement`), shared by both passes.
- Two things found on the way, now in optimisations.md §7.1:
  - **Conditional blocks.** A block compiled under `cfg` (ADR 0080) or a `debugAssert`'s block (ADR 0087 §2.5) may
    be left out of a build; a store in one could vanish while the literal stays elided. Rule 5 refuses them.
  - **Rule 1's drop condition cannot fail**: a repeat `[e; N]` of a type that needs drop is already N0259
    (type-system.md §16.2). It stays in the pass as a guard; the test of an element with a `drop` that §2.4 listed
    cannot be written, and is not.
- `0..a.length` (§5 question 3) is already folded to the constant `N` for an array before NIR; the pass also accepts
  a `Length(a)` on the way, should a later change not fold it.
- **The oracle was tried**: a compiler built with rule 3 loosened to accept a loop to `N − 1` elided the literal of
  `for (j in 0..7usize) { a[j] = 1; } return a[7];` on `[9u64; 8]`. Natively it printed 0, a stale value (the right
  answer is 9); `run --interp` stopped with "fault: use of uninitialised memory", exit status 70. The source was
  restored and that compiler discarded.
- Runs, and the gain measured on rq, are in ROADMAP §10.
