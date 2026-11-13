# ADR 0133 — The functions of a nested instance wait for a use; an instantiation has a size

- Status: **Accepted** by the owner, 2026-11-13 ("Go ahead and tackle those still open"; "Also fix" the checker
  hang), implemented on branch `fix/generic-instances`; released in ceesafec 0.43.0.
- Scope: when a generic struct's or enum's instance gets its functions (`src/check/lazy_instances.cee`,
  `generics.cee`, `structs.cee`), what N0266 covers, and how often one fault in generic code is reported;
  `spec/type-system.md` §20.3, §20.4, `spec/diagnostics.md` (N0266). No new diagnostic code.

## 1. Context

`ceesafec check` never answered for a generic struct with a function whose signature names a larger or a fresh
instance of the struct (present in 0.42.0):

```ceesafe
struct Pair<T: Copy> {
    left: T;
    right: T;

    fn nest(borrow this) -> Pair<Pair<T>> => …;          // Pair<T> needs Pair<Pair<T>> needs Pair<Pair<Pair<T>>> …
    fn with<U: Copy>(borrow this, a: U, b: U) -> Pair<U> => …; // each instance's `with` has a fresh `U`
}
```

An instance got its declaration's functions when it was made, and declaring a function resolves its signature's
types, which makes instances. §20.4 limited only the nesting of *function* instantiations (64, N0266), and that
limit itself did not end `fn grow<T: Copy>(x: T) -> u32 => grow(Two<T> { left: x, right: x })`: the 64th type has
2^64 members, and the checker walks a type's members.

## 2. Decision

2.1 **Naming an instance does not need its functions.** An instance named in the signature of a function of an
instance of the same declaration (directly, or through instances of other declarations) gets its fields or
variants and its drop block at once, and its functions, generic impls and bounded blocks' functions at its **first
use** (§20.4 lists the uses). Every other instance gets them when it is made, as before, so the order in which
existing programs' functions are declared, checked and emitted is unchanged.

2.2 **Depth.** The functions of an instance first named or first used in a body are one instantiation deeper than
the body; a closure is as deep as the function it is written in. More than 64 is N0266, at the use. A field or
payload whose type names a larger instance of its own declaration is N0266 at the 65th nested instance, even
behind a box: each must be dropped.

2.3 **Size.** A new instantiation whose type arguments are made of more than 1048576 types and members (§20.4
defines the count), or the first use of a waiting instance that large, is N0266.

2.4 **One fault, one diagnostic** (§20.3). A copy of generic code in a type that still names type parameters is
checked as an instantiation is; what its declaration reported is not reported again. Diagnostics equal in code,
place and message are reported once.

## 3. What a program can notice

- Programs that hung now check, and run when the instances they use are finitely many.
- A program accepted by 0.42.0 is refused only if it instantiates a generic with type arguments of more than
  1048576 types and members (for example `Two<…>` nested 21 deep, a value of 2^21 fields), or reaches the 64th
  nested instantiation through struct or enum instances first made in bodies, which counted as depth 0 before. No
  such program is known; the corpus, the libraries and the compiler are unchanged.
- A diagnostic that was reported two or more times is reported once (`check-fail/impl-bounds-moves.cee` line 50).
  The exit status does not change: one error is an error.
- An instance's functions are still declared and checked together: one unused method that needs instances without
  end (`depth` above) is N0266 once another method of that instance is used. Per-method waiting was not needed to
  end the hang and would change which functions are emitted.

## 4. Alternatives

- **Every instance's functions wait.** Simpler to state, but it reorders the functions of every program (HIR and C
  snapshots) for no gain: only a declaration met again inside itself can recur.
- **Only a depth limit, counting the instances made in signatures.** It ends the hang by refusing `nest`, which
  names one more instance and needs none.
- **Memoise every walk of a type instead of a size limit.** The size of the value (2^64 members) is the fault, not
  the time of one walk; layouts are 32-bit.

## 5. Tests

`run-pass/generic-nested-instances.cee`, `check-fail/generic-nested-instances.cee` (each N0266 once),
`check-fail/generic-single-diagnostics.cee`, `check-fail/impl-bounds-moves.cee` (line 50: one N0251, was two),
`check-fail/generics.cee` (N0266 for a function, unchanged).
