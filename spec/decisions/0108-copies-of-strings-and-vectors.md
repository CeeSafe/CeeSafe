# ADR 0108 — Copies of strings and vectors: `clone` and `String.from`

- Status: **Accepted** by the owner, 2026-11-12 ("We fix all gaps 1-6 first"), implemented on branch `impl/0108`;
  not yet released. Raised while writing the language's book: two things every chapter with an owned string needs
  took a helper each.
- Scope: two structs in `lib/core` (`VectorCopying<T>` in `slices.cee`, `StringCopying` in `text.cee`); the checker's
  vector and string functions (`src/check/vectors.cee`); `spec/type-system.md` §23.1 and §23.2; the guide. No change
  to the language, the runtime, the interpreter or the C backend, and no new diagnostic code.

## 1. Context

Two gaps, both in §23:

- **No copy in one call.** `s.clone()` on a `String` or a `Vector<T>` was N0200 ("has no method"). Only `Arc` and
  `Channel` had a `clone`. A program wrote the copy out:

  ```ceesafe
  var copy = String.create();
  copy.append(text)!;
  ```

  and for a vector `Vector<T>.withCapacity(v.length)` and an `extend`.
- **No owned string from text in one call.** `String.from("…")` did not exist; the two lines above were also the only
  way from a literal, or from any other `StringView`, to a `String`.

What the existing design already says about each:

- Every operation that allocates returns `Result<…, AllocError>` (§23.1: `push`, `withCapacity`, `extend`, `resize`,
  `reserve`, `insert`; §23.2: `append`). Nothing allocates silently, and the allocation-failure sweep
  (`tools/fault_sweep.py`, `ceesafec test --sweep alloc`) counts on that.
- A vector's or a string's allocator is `SystemHeap` or `FailingAllocator` (§23.1). Methods written in the library
  have one function for each (`VectorEditing<T>.insert` and `insertFailing`, ADR 0075 §2.12).
- Copying elements in bulk asks for a Copy element type, N0259 (`extend` and `resize`, ADR 0071 §2.1).
- The word is `clone`: `Arc<T>` and `Channel<T>` have `a.clone()`, and N0290 on `@derive(Clone)` says that Clone "is
  written as a method". The type functions that make a vector or a string are `create()` and `withCapacity(n)`; the
  ones named `from…` take what the value is made from (`StringView.fromUtf8(bytes)`, `Vector.fromRaw(p, n, c)`).

## 2. Decision

```ceesafe
s.clone()            // s: String<A>              -> Result<String<A>, AllocError>
v.clone()            // v: Vector<T, A>, T Copy   -> Result<Vector<T, A>, AllocError>
String.from(text)    // text: StringView          -> Result<String, AllocError>
String<A>.from(text) //                           -> Result<String<A>, AllocError>
```

- **`v.clone()`** is a new vector of the same type, allocator included, holding a copy of the elements. `T` must be
  Copy; a vector of anything else is refused with N0259 and `extend`'s words ("'clone' copies elements, so the
  element type must be Copy; 'String' is not"). It takes `borrow this`: the vector is only read, so it may be cloned
  through a shared reference and while a view of it is live. The copy has its own buffer.
- **`s.clone()`** is a new string of the same type holding a copy of the text: `String<A>.from(s.view())`.
- **`String.from(text)`** is a new string holding a copy of `text`. A string literal is a `StringView`, a `String`
  argument is borrowed as one, and so is a slice of either, so `String.from("hello")`, `String.from(other)` and
  `String.from(other[0..3])` are all this function. The allocator is the written one (`String<A>.from(text)`), or
  that of an expected `Result<String<A>, _>`, or `SystemHeap`: the rule `String.withCapacity(n)` has.
- **One allocation each, none for nothing.** Each is `withCapacity(length)` followed by `extend` or `append`, which
  then finds room. The capacity is what `withCapacity` gives for that length: the length, and 4 for a length of 1
  to 3 (the smallest buffer a vector has, §23.1). An empty vector or text allocates nothing and gives capacity 0.
- **Failure is a value.** If the allocation fails the result is `Err(AllocError.OutOfMemory)`; nothing was made and
  the original is as it was. The sweep sees one allocation attempt per call, like any other.
- **They are library code**, safe CeeSafe over existing operations: `VectorCopying<T: Copy>.clone(s)` and
  `cloneFailing(s)` take the elements as a `Slice<T>`; `StringCopying.from(text)` and `fromFailing(text)` take a
  `StringView`. The checker lends the receiver's elements or text and calls the function for the allocator, as it
  does for `insert`. A program may call them by those names too.

The names: `clone`, because that is the word the language already uses for "another one of these" and tells its
users to write; it returns a `Result` here because it allocates, where `Arc.clone` does not. `String.from`, because
it sits with `String.create()` and `String.withCapacity(n)` as a function of the type, and names what the string is
made from. `from` is not a keyword.

Not included:

- **A clone of a vector whose elements are not Copy** (`Vector<String>`, `Vector<Box<T>>`). It needs an interface
  for "can be cloned, and may fail" that elements implement, and a decision on what happens to the half-made copy
  when the k-th element fails. Until then such a program writes the loop, pushing `x.clone()?` for each element.
- **`Vector<T>.from(slice)`.** `v.clone()` covers a vector, and a slice is two calls (`withCapacity`, `extend`) or
  `VectorCopying<T>.clone(slice)`. It can be added in the same way if programs ask for it.
- **`view.toString()`, `slice.toVector()`** and other second names for the same thing.
- **`clone` on `HashMap`, `SlotMap`, `StableVector`, `Box`.** Each is its own decision.

## 3. Compatibility

A program whose string or vector method was called `clone`, or whose `String` function was called `from`, could not
exist: those types take no user methods (N0200 before). A program that declares its own struct named `VectorCopying`
or `StringCopying` is not affected, as with the other library structs: the checker looks the library's up by its own
table. The messages for a method or a function that does not exist now list `clone` and `String.from(text)`.

## 4. Tests

- `tests/corpus/run-pass/copies-of-strings-and-vectors.cee`: a string from a literal, from a view of a string, from a
  slice of one, through a reference and in a function with `?`; copies changed apart from their originals, strings
  and vectors, Copy structs as elements; the empty string and the empty vector (capacity 0); a string of 3,890 bytes
  and a vector of 5,000 elements (capacity equal to the length); a vector and a string of `FailingAllocator`, whose
  copies have that allocator, and the failure path with `FailingAllocator.failAfter(0)`: `Err`, and an empty text
  still succeeds because it does not allocate.
- `tests/corpus/check-fail/copies-of-strings-and-vectors.cee`: `clone` of a `Vector<Owned>` and of a
  `Vector<String>` (N0259); an argument to `clone`, none to `String.from` (N0209); a result used without unwrapping,
  and a vector given as text (N0205); `Vector<u8>.from(…)` and `"text".clone()` (N0200).
- `guide/collections.md` has a checked example of each; `spec/api/core.md` gains the two structs.
- Three HIR snapshots (`check-pass/async-timeout`, `effects-of`, `string-braces-warning`) number their closures four
  higher, since the library has four more functions: checked by adding 4 to every closure number of the old
  snapshot, which gives the new one byte for byte.
- The allocation-failure sweep (`tools/fault_sweep.py`) on a program that calls `String.from`, `s.clone()`, an
  `append` that grows, a `push` and `v.clone()`: 5 allocation attempts, one for each, and 5 of 5 failure points
  handled with nothing live.
- Run 2026-11-12, macOS arm64, on this branch: `tools/run_corpus.py --corpus tests/reference/tests/corpus`, 0
  failures (the 4 `freestanding` partials are as before); the new run-pass test also with `--check-leaks`;
  `tools/guide_check.py`, 0 problems; `ceesafec fmt --check` lists nothing; the compiler built by itself twice, and
  both generations emit identical C. `tools/contract_tests.py` is run by the integrator after the merge.
