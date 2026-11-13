# ADR 0130 — The bounds of an inherent `impl` block

- Status: **Accepted** by the owner, 2026-11-13 ("Go ahead and tackle those still open"), implemented on branch
  `fix/open-checker`; the owner accepted conditional methods (§2.3) on 2026-11-13 ("go"), on the condition that the
  decision of §2.6 be explicit, which branch `fix/interface-result-loans` implements; released in ceesafec 0.43.0. It completes ADR 0064 §1 and ADR 0107, which let an `impl<T: B> S<T> { … }`
  block write a bound and said nothing of what it means when the struct or enum does not have it.
- Scope: the checker (`src/check/bounded_impls.cee`, new; `inherent.cee`, `structs.cee`, `generics.cee`,
  `declare.cee`, `members.cee`, `self_types.cee`, `closures.cee`), one flag of a HIR function (`src/hir/hir.cee`);
  `spec/type-system.md` §21.1; `spec/diagnostics.md`; the guide. No new diagnostic code, no change to the backends,
  the interpreter or the libraries.

## 1. Context

```ceesafe
struct Pair<T> {
    left: T;
    right: T;
}

impl<T: Copy> Pair<T> {
    fn first(borrow this) -> T => this.left;
}
```

was N0251, "cannot move a 'T' out of the value 'this' refers to", although the block says `T: Copy`. The same
function in the body of a `struct Pair<T: Copy>`, in a generic function `fn first<T: Copy>(p: borrow Pair<T>)` and in
an interface's impl `impl<T: Copy> First<T> for Pair<T>` checks.

The cause was not the move check. An inherent block's functions were merged into the struct's own list "as if
written in its body" (ADR 0064 §1), and the block's type parameters were compared with the struct's by name alone:
the bounds the block wrote were never read. So every bound was ignored, not only `Copy`: `impl<T: Named> Pair<T>`
could not call a method of `Named` on a `T` (N0234), `impl<T: Send> …` got no `Send`, and `impl<T: Missing> …`,
naming no interface, was accepted. An enum's blocks (ADR 0107) were the same.

## 2. Decision

2.1 **A block may bound its parameters**, by interfaces and by `Copy`, `Send` and `Share`. The bounds are resolved
as any type parameter's are: one that names no interface is N0204.

2.2 **A block that restates the declaration's bounds changes nothing.** If the struct's or enum's own parameters
have every bound the block writes (`impl<T: B> S<T>` for `struct S<T: B>`, the form §21.1 already showed), its
functions are every instance's, declared and checked as before. Existing programs of that form compile to the same
code.

2.3 **Any other block is bounded.** In its functions `T` has the declaration's bounds and the block's. The functions
belong to the instances whose type arguments meet the block's bounds, and to no other instance:

```ceesafe
let numbers = Pair<i32> { left: 1, right: 2 };
numbers.first();                     // 1
let words = Pair<String> { … };
words.first();                       // error[N0268]
```

```text
error[N0268]: 'Pair<String>' does not have 'first', which its 'impl' block gives the instances that meet the block's
bounds: 'String' is not Copy, but 'T' must be ('T: Copy') (type-system.md §21.1; ADR 0130)
```

N0334 takes N0268's place for `Send` and `Share`, as it does for a type argument (§38.1). The declaration's own
generic type is an instance that does not meet the bounds: a function in the struct's body, or in a block without
the bound, cannot call a bounded block's function, and neither can generic code whose parameter lacks the bound
(`fn f<U>(p: borrow Pair<U>) => p.first()`). With the bound (`fn f<U: Copy>(…)`) it can.

2.4 **One member of each name.** A name that two blocks give is N0228 whatever their bounds, as is a name the
declaration's body, a variant or an interface's impl already gives. Two blocks with disjoint bounds cannot give one
name two meanings: no rule picks between them when both hold.

2.5 **How it is checked.** A bounded block's functions are checked once, in the instance of the struct or enum for
the block's own parameters (`Pair<T>` with the block's `T: Copy`), where their diagnostics are reported. They are
declared again for each other instance that meets the bounds, as a generic struct's functions are for each instance
(§20.3). The other functions of the instance a block is checked in repeat the declaration's generic version and
report nothing twice.

2.6 **How the decision is made.** Whether an instance meets a block's bounds has three answers: met, not met, and
not yet known (`decideBounds`, `src/check/generics.cee`). An interface bound is decided by the program's impls, in
whatever file or package they are; `Copy` by the argument's fields and drop block; `Send` and `Share` by its fields.
So there is no answer before every impl is processed, every declared struct and enum has its fields and its Copy
and drop, and, for a type made later, before its instantiation is complete. Until then the ordinary bound check
(`checkBounds`) puts itself off and reports nothing, and that silence is never read as "met": the decision asks the
three conditions first, and takes an answer only from a check that ran and did not put itself off. The blocks'
functions are given to the declared types after all three hold (`attachBoundedToAll`) and to each later instance
when it is complete (`noteBoundedEntry`). If ceesafec's own order ever asked earlier, the instance would get no
function and N0216 would say so ("ceesafec's internal check: the bounds of this 'impl' block were asked for …
before every impl, field and Copy type was known"), instead of an instance silently having a function it must not
have. The error for a missing function (§2.3) is given only for a decided "not met".

## 3. Alternatives

- **Refuse a bound the declaration does not have** (N0267: "write the bound on the struct"). Sound and small, and
  what the old text of §21.1 could be read to mean. Rejected: it makes `Copy`-only accessors of a container
  impossible without making the container `Copy`-only, which is the program in §1, and it is what an interface's
  generic impl already does not require (ADR 0054).
- **Keep the functions on every instance and check the bounds at each call.** The function would then exist, with a
  body that does not check, for instances that cannot have it; the body of `Pair<String>.first` moves out of a
  reference. Not declaring it leaves nothing to compile.
- **A `where` clause on each function** (`fn first(borrow this) -> T where T: Copy`). `where` is reserved and unused;
  a block's parameter list is where the language already writes these bounds.

## 4. Consequences

- A program that wrote a bound on a block and relied on its being ignored now has it: its bounded functions are
  missing from the instances that do not meet the bound (N0268 where one is used), and a bound that names no
  interface is N0204. Neither compiled to anything different before; both now fail to check. The libraries and the
  compiler have no such block.
- `ceesafec doc` lists a bounded block's functions with the struct's, as before: the block's bounds are not yet
  shown there.
- Not in this ADR: bounds on an interface impl's parameters, which ADR 0054 decided; specialisation; a method whose
  signature the bound changes.

## 5. Tests

`run-pass/impl-bounds` (structs and enums, methods and static functions, a generic method, a block that adds to the
declaration's bounds, one that restates them, a nested instance, generic code with the bound, an interface's generic
impl beside them); `check-fail/impl-bounds` (every instance that does not meet a bound, N0228 across blocks, N0204);
`check-fail/impl-bounds-moves` (the reads that stay moves without the `Copy` bound: N0251);
`run-pass/generic-send-loans` and `check-fail/generic-send-arguments` use a `Send` block.
`run-pass/impl-bounds-order` and `check-fail/impl-bounds-order` (§2.6: the uses first, the impls and the deciding
fields last in the file, for an interface bound, `Copy` and `Send`); `tests/packages/impl-bounds-order` (the same
with the deciding impls in another file of the package and in a dependency, whose struct and enum the blocks are:
`app` runs, `unmet` is N0268 and N0334).
