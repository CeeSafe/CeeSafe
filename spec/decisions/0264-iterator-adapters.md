# ADR 0264 — Iterator adapters: `map`, `filter`, `collect` and the rest, as library chains

- Status: **Accepted** by the owner, 2026-11-26 ("Accept all, release first": §6's six questions as recommended).
  Proposed the same day at the owner's instruction ("Go ahead with #2 next": the second gap of `ceesafegaps.md`, no
  iterator adapters). **Implemented** on the branch `adr/0264-iterator-adapters` (§7), not released.
- Scope: lib/core (a chain type, the adapters, the iterators that lend a slice's elements); guide/builtins.md,
  the book's 13.2 and appendix C, guide/for-agents.md and guide/coming-from/rust.md; corpus and benchmark tests.
  Possibly the compiler's closure specialisation (§2.6, §6 question 4).
  Revisits ADR 0086 §2.7 ("No adapter library in this ADR").

## 1. Context

CeeSafe has `Iterator<T>` (lib/core, ADR 0086 §2.7), `for` over any iterator, and on slices and vectors the
closure-taking methods `sortBy`, `forEach`, `any`, `all`, `partitionPoint` and `retain` (type-system.md §36.4). It
has no adapters. The book teaches the loop instead (13.2, "Mistake: a chain of adapters", N0200 on `[1, 2, 3].map`),
and the guide calls adapters "planned, or recorded as an open decision". Rust programmers, and agents trained on
Rust, write chains first.

ADR 0086 §2.7 declined an adapter library for two reasons:

1. "A lazy `Map<I, F>` would hold a closure in a field, and a closure in a field cannot be called." That is no longer
   so: a closure in a field is called as `c.step(n)` (AGENTS.md), and the book's own `Filter` (Listing 13-12) does.
2. "Without `impl Iterator` results, every chain would also need a named type." Still so, but the types are the
   library's; a chain used where it is written (`let v = xs.iter().map(f).collect()?;`) never names them.

Counted on 2026-11-26 over CeeSafeApps (2,557 files, 1,169,591 lines, worktrees and trials excluded): 15,127 `for`
loops, of which 2,352 have, as their whole body, one of an adapter's plain shapes:

| Loop body | Loops | As a chain |
|---|---|---|
| one `out.push(f(x))` | 1,266 | `map` + `collect` |
| `if (…) { return …; }` | 588 | `any`, `all`, `find` |
| `if (p(x)) { out.push(…); }` | 179 | `filter` (+ `map`) + `collect` |
| one `total += …` | 131 | `fold` |
| `if (…) { found = …; break; }` | 103 | `find`, `position` |
| `if (p(x)) { n += …; }` | 85 | `filter` + `count` or `fold` |

ADR 0086 counted Rust's own use in the Bun port: of 15,772 iterator sources, 10,452 were followed by an adapter; by
chains, `map` 2,038, `any` 2,019, `enumerate` 1,405, `collect` 1,295, `filter` 888, `zip` 710, `all` 701, `find`
658, `count` 451.

### 1.1 What a prototype found (2026-11-26, ceesafec 0.95.17)

- **Default methods of `Iterator<T>` cannot carry the adapters.** `fn map<region r, U>(this, f: borrow(r) fn(T) -> U)`
  in an interface is N0216: an interface's methods cannot declare region parameters (type-system.md §25.1). An
  adapter that holds a borrowed closure is a scoped type, and a scoped type cannot use a default method (N0274).
- **A chain type whose own methods return the next stage works**, with no language change:

```ceesafe
struct Chain<T, I: Iterator<T>> {
    inner: I;

    fn map<region r, U>(this, f: borrow(r) fn(T) -> U) -> Chain<U, Mapped<r, T, U, I>> =>
        Chain { inner: Mapped { inner: this.inner, f: f } };

    fn collect(this) -> Result<Vector<T>, AllocError> { … }
}

impl<T: Copy, I: Iterator<T>> Chain<T, I> {
    fn filter<region r>(this, keep: borrow(r) fn(T) -> bool) -> Chain<T, Filtered<r, T, I>> =>
        Chain { inner: Filtered { inner: this.inner, keep: keep } };
}
```

  `U` is inferred from the closure; the closures may capture the function's locals. Over an iterator that lends a
  `Vector<String>`'s elements (`Iterator<borrow(r) String>`, accepted),
  `….filter((s) => s.length > 2).map((s) => s.length).collect()!` gives `3 5`, interpreted and native.
- **`filter` takes the element by value, so it needs `T: Copy`.** A closure of `borrow T` would make, for a lent
  element, a reference to a reference (N0274). A lent element and a number are Copy, which are the common cases; an
  owned, non-Copy element (a `String` that `map` made) cannot be filtered without a language change (§5).
- **The cost.** The same work by a loop and by a chain, 2×10^8 elements through `map` and `filter`, built with `-O2`
  on an arm64 Mac: the loop 132–135 ms, the chain 432–435 ms (three runs each), **3.3 times slower**. The closures
  are called through their fields, which closure specialisation does not reach (optimisations.md §5.1: "Not
  specialised: a closure read from a field"). A type parameter cannot stand for a closure's type (a bound must be an
  interface, N0204), so the library cannot make the calls direct on its own.

## 2. Decision

### 2.1 Chains in lib/core

A `Chain<T, I>` holds an iterator `I` of `T` and is one itself, so `for (x in chain)` works. Its methods are the
adapters, each of which returns a `Chain` of the next stage, and the consumers, which run it. The stages are
lib/core structs (`Mapped`, `Filtered`, `Taken`, …) that a program never has to name. A chain borrows its closures,
so it lives in the statement or the block that makes it, as a loop over the same values would.

### 2.2 Where a chain starts

- `s.iter()` on a slice, an array (through `a[..]`) and a vector: the elements lent, `Chain<borrow T, …>`.
- `s.values()` for a `T: Copy`: the elements copied, `Chain<T, …>` (numbers without `*x`).
- `Chain.of(it)` for any iterator (`text.split(",")`, a program's own), since a default method cannot give every
  iterator a `.chain()` (§1.1).

### 2.3 The adapters (lazy)

| Method | Gives | Note |
|---|---|---|
| `map(f)` | `Chain<U>`, `f(x)` for each | `U` from `f` |
| `filter(p)` | the elements that pass | `T: Copy` |
| `take(n)`, `skip(n)` | the first `n`, or all after them | |
| `takeWhile(p)`, `skipWhile(p)` | while `p` holds, or after it stops | `T: Copy` |
| `enumerate()` | `Indexed<T> { index: usize; value: T; }` | a named struct, as there are no tuples (ADR 0216) |
| `zip(other)` | `Paired<A, B> { first: A; second: B; }` | ends with the shorter |
| `copied()` | `T` from `borrow T` | `T: Copy` |

### 2.4 The consumers

| Method | Gives |
|---|---|
| `collect()` | `Result<Vector<T>, AllocError>` (`?` or `!`; nothing in a package that says `out-of-memory = "panic"`, ADR 0261) |
| `count()` | `usize` |
| `fold(init, f)` | the accumulator, `f(acc, x)` for each |
| `any(p)`, `all(p)` | `bool`, stopping at the first that decides (`T: Copy`) |
| `find(p)` | `T?`, the first that passes (`T: Copy`) |
| `position(p)` | `usize?` (`T: Copy`) |
| `forEach(f)` | `void` |

No `sum`, `min` or `max`: a generic `T` has no `+` or `<` (no numeric interface); `fold` does them
(`fold(0, (a, x) => a + x)`). Amended by ADR 0269: `sum`, `min` and `max` through `Number<T>`, since `fold(0, …)` is an
`i32` accumulator and does not compile over other number types.

### 2.5 What stays

The loop stays CeeSafe's first form, and the book keeps teaching it first: it is as fast as C, it can `return`,
`break` and use `?` in its body, and it needs none of §2.3's limits. A chain is for the shapes of §1's table, where
it says the same thing in one line.

### 2.6 The cost of the closures

As built on the library alone, a chain's closure calls are indirect (§1.1: 3.3 times a loop in a tight numeric
loop; nothing measurable for a few elements). Two ways to close the gap, each a compiler change of its own:
closure specialisation (optimisations.md §5) reaching a closure that a known aggregate holds in a field, or closure
types as type arguments with a function-type bound. Either is a later ADR (§6 question 4).

## 3. Options not taken

- **Default methods of `Iterator<T>`** (Rust's shape, `it.map(f)` on any iterator). Blocked by §25.1 and N0274
  (§1.1); lifting both is a language change of its own, and `Chain.of(it)` covers the need.
- **Boxed closures** (`Box<dyn fn(T) -> U>` in each stage). An allocation per adapter, a `Result` on every adapter,
  and no faster: a `dyn fn` is not specialised either.
- **Eager adapters on vectors** (`v.map(f)` returning a new `Vector`). Simple, but every stage allocates, and
  `any`/`find` would build a vector to look at its first element.
- **Leaving it to loops** (ADR 0086 §2.7). The loops stay; this ADR adds the one-line form for the shapes that are
  most of them.

## 4. Consequences

- The book's 13.2 "Mistake: a chain of adapters" becomes a section on chains, N0200's example changes, and appendix C
  lists the new methods. guide/for-agents.md and guide/coming-from/rust.md say what a chain can and cannot do.
- An agent from Rust writes `xs.iter().map(f).filter(p).collect()?`, which compiles, where today it is N0200.
- Code with chains is up to 3.3 times slower than a loop where the closures are the work (§2.6). ADR 0267 makes the
  calls direct: 1.2 times.

## 5. What is not known

- How many of §1's 2,352 loops are hot: the cost of §2.6 matters only there.
- Filtering owned, non-Copy elements: a closure parameter that is a borrow of `T`, collapsing to `T` when `T` is
  already a borrow, would need a rule in type-system.md §13; not proposed here.
- Whether a chain that holds a region can be returned from a function as a named type in practice (a struct with a
  region parameter can, but the type is long).

## 6. Questions for the owner

1. **The shape**: a `Chain` type with its own adapter methods, started by `iter()`, `values()` and `Chain.of(it)`
   (§2.1, §2.2)? Recommended: yes; it is the shape that works today.
2. **The first set**: §2.3's adapters and §2.4's consumers? Recommended: yes; they cover §1's shapes and the most
   used of Bun's.
3. **`filter`, `any`, `all`, `find`, `position`, `takeWhile` and `skipWhile` for Copy elements only** (lent elements
   and numbers)? Recommended: yes for now (§5).
4. **The cost**: release the library with §2.6's cost stated, and propose the compiler change as a separate ADR?
   Or hold the release until a chain is within, say, 10% of the loop? Recommended: release first; the shapes in §1
   are mostly not hot loops, and the measurement goes in the book.
5. **Names**: `Chain`, `iter()`, `values()`, `Indexed` (`index`, `value`), `Paired` (`first`, `second`)?
   Recommended: yes; `iter()` and the adapter names are Rust's, which is what their readers will try first.
6. **Teaching**: the book keeps the loop first and shows chains after it, with the cost? Recommended: yes.

## 7. The design as built (2026-11-26)

- **lib/core/chains.cee**: `Chain<T, I>` with `of`, `map`, `take`, `skip`, `enumerate`, `zip`, `collect`, `count`,
  `fold` and `forEach`, and, in an `impl<T: Copy, I: Iterator<T>> Chain<T, I>`, `filter`, `takeWhile`, `skipWhile`,
  `any`, `all`, `find` and `position`; the stages `Mapped`, `Filtered`, `Taken`, `Skipped`, `TakenWhile`,
  `SkippedWhile`, `Enumerated` and `Zipped`; `Indexed` and `Paired`; `SliceItems` and `SliceValues`, and the holders
  `SliceIteration` (`iter`) and `SliceValueIteration` (`values`), as `SliceSplitting` holds `splitAt`.
- **Two differences from §2**:
  - **No `copied()`.** An impl block for `Chain<borrow(r) T, I>` only is N0267 (an impl of a struct's methods names
    it with its own parameters, ADR 0064 §1). `values()` copies a slice's elements, and `map((x) => *x)` any lent Copy
    values.
  - **`fold`'s accumulator is `A: Send`**: one that may hold a borrow could be derived from the chain (N0245), so an
    accumulator of lent elements (the longest name so far) is not allowed; `find` or a loop does it.
- **The checker** routes `iter` and `values` on a slice, an array (through its slice), a slicing expression and a
  vector to the holders (`isChainStart`, members.cee; `checkSliceMethod` and `checkVectorMethod`, vectors.cee;
  `checkSliceAlgorithm`, library.cee), lending the elements shared. `values()` of non-Copy elements is N0205 naming
  `iter()`. N0200 on a slice or a vector lists `iter` and `values`, and for a chain's method (`map`, `filter`,
  `collect`, `fold`, …) says to start one (`appendChainHint`, bytes_text.cee).
- **A program's own `Chain`, `Mapped` or another of these names** takes precedence in its own code, as with any lib/core
  name; three corpus programs declare one.
- **Documents**: type-system.md §36.4a; guide/builtins.md, guide/for-agents.md, guide/coming-from/rust.md and
  guide/introduction.md; the book's 13.2 (Listing 13-16 is now "a chain without `iter()`", followed by the chain
  that works and the loop).
- **Tests**: the corpus's `run-pass/iterator-chains.cee` (every adapter and consumer; built, interpreted, with leak
  checks) and `check-fail/iterator-chains.cee` (N0200 with the hint, N0205 for `values()`, N0268 for `filter` of a
  non-Copy value).
