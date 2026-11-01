# ADR 0012 — M11 G8: function values and scoped closures

> **Inherited.** ADR 0012 was accepted before CeeSafe's fork (Decision 0006), when the language had its
> earlier name. In this record, "the reference" is that language and project and "the reference compiler" its compiler:
> read them as CeeSafe and `ceesafec`. Links to documents CeeSafe did not keep are given as text.

- Status: **Accepted** (project owner, 2026-10-25, taking every recommendation in §5; OD-9a and OD-9b record the
  deferred options). The wording is the agent's; the owner may revise it. Not yet implemented: the decisions are
  written into `type-system.md` (a new §36), `grammar.md`, `lifetimes.md`, `intermediate-representation.md` and
  `c-backend.md` before any code.
- Scope: M11 G8 (`milestone-11.md`), closing OD-9 (`open-decisions.md`: "Non-capturing `fn` pointers only. Scoped
  capturing closures in M6+"). It amends `type-system.md` §8 ("Functions are not values in M1") and §29.5.

## 1. Context

- **No CeeSafe function values today.** A callee must be a package function or a built-in (`type-system.md` §8). The
  only function-typed values are C function pointers, `extern "C" fn(…) -> R` (§29.5). Calls through them are
  unsafe and limited to FFI-safe types. `@callback` gives a CeeSafe function a C address, with the same limits (§34.1).
  Probing `fn` in an argument position gives `expected ')', found ':'` (N0100) (`milestone-11.md`, G8).
- **No dynamic dispatch.** Interfaces are bounds, not types (§21; `dyn` is OD-10). Generics are monomorphised and
  "checked once, generically" (§20).
- **Loans already flow through values.** A scoped value carries the loans it was made from: references, views,
  slices, and since M6x aggregates with region parameters (§25, ADR 0006). The NIR borrow checker propagates them
  through moves, copies and calls, and a signature summarises a callee completely (OD-3 goal 3).
- **Kernel constraints.** The kernel profile has no implicit allocation and a 2 KiB frame limit
  (`profiles.md` §1, §3). CosmoOS kernel APIs take callbacks as a C function plus a `void *` argument, for
  example the thread and timer bindings in `integrations/cosmoos/c8/`.
- **Effects.** A call through a C function pointer is assumed to have both effects, `sleep` and `alloc` (§32.1).

What programs need, from M9's suite and the README tour: comparators for sorting, `forEach`/`map`/`filter`-style
helpers over slices and vectors, retain-if on vectors, visitors over trees, and callbacks that capture a little local
state (a counter, a threshold). None of these needs the closure to outlive the function that made it.

## 2. Options

| | A. Scoped closures with one type per signature (proposed) | B. Monomorphised closures with an `Fn` interface | C. Owning, heap-allocated closures |
|---|---|---|---|
| A closure value is | `{ code, env }`, with `env` pointing at the captures in the maker's frame | an anonymous struct, one type per literal | a box holding the captures, with a vtable |
| Captures | by reference; the value carries their loans | by reference or by move | by move |
| Can outlive its maker | no (scoped, like a reference) | by move, through generics only | yes |
| Call | indirect | direct, inlinable | indirect |
| Needs | function types, a literal syntax | anonymous types, an `Fn` bound with function-type syntax, inference of closure types in generics | `dyn` (OD-10), allocation (not in the kernel) |
| Fits the kernel | yes (no allocation, a small env) | yes | no |

A covers every use in §1 with the least new machinery: the closure is a scoped value, and scoped values are already
checked. B is faster at a call (inlining), but every function that takes a closure becomes generic, and a field
holding one needs a type parameter. C needs `dyn` and the heap. **A is proposed now. B and C stay open (§5 Q5)**,
and A does not block them.

## 3. Proposed decision

1. **Function types.** `fn(T1, …, Tn) -> R` is the type of a **plain function value**: one code address, Copy, no
   loans, and not FFI-safe. The name of any package function (generic ones with type arguments) is a value of that
   type where one is expected, and so is a closure literal that captures nothing. `f(args)` calls it, safely. It is
   distinct from `extern "C" fn(…)` (§29.5), which stays the FFI type.
2. **Closure types.** `borrow fn(T…) -> R` and `borrow mut fn(T…) -> R` are **closures**. The value is a code address
   and a reference to an environment. As with references (§13), the shared form is Copy and is called through a shared
   loan of the environment. The `mut` form is unique: each call reborrows it, and it may change what it captured. A
   plain `fn(…) -> R` converts implicitly to either closure type. Closure types are scoped (§19.1): they live within
   the loans of their captures, follow the reference rules for where they may appear (§13.1: parameters, `let`
   locals, results through a signature), and may be fields of aggregates with region parameters (§25).
   `borrow(r) fn(…) -> R` names the region.
3. **Literals.** TypeScript-style arrow functions:

   ```ceesafe
   (x: i32, y: i32) => x + y
   (x) => x > limit
   (item) => { total += item; }
   ```

   - The parameter types may be left out where a function or closure type is expected, and are taken from it.
   - The body is an expression, or a block with `return`. It may not use `break` or `continue` out of the closure,
     nor `?` and `return` for the enclosing function: `return` returns from the closure.
   - The parser reads `(` … `)` `=>` as a closure when the parentheses hold only names, each with an optional `: T`.
     A parenthesised expression cannot be followed by `=>`, so the two cannot be confused.
4. **Captures.** A literal captures the enclosing locals (and parameters) that it names. Each is captured **by
   reference**: shared if the body only reads it, mutable if the body assigns to it or borrows it mutably. Any
   mutable capture makes the literal a `borrow mut fn`, so assigning it to a `borrow fn` is an error (a new code).
   Copy captures that are only read may be copied into the environment instead, as an optimisation. The borrow checker
   cannot tell the difference, since nothing may write them while the closure lives. A closure value carries the
   loans of all its captures, so the existing rules (B1–B5) forbid, for example, changing `total` while a closure that
   reads it is live. Moving a capture out (`take(x)` inside the body) is an error (a new code). Owning captures are
   option B or C.
5. **Environment.** The captures live in a hidden local of the block that holds the literal, not in a statement
   temporary, so `let f = (x) => x + k;` works. The environment is a C struct of pointers (and copied values) in the
   maker's frame, so there is no allocation, and its size counts against the kernel profile's frame limit.
6. **C representation.**
   - A plain function value is a function pointer, `R (*)(A…)`.
   - A closure is `struct { R (*code)(void *env, A…); void *env; }`, with one `static` function per literal.
   - A plain function converted to a closure gets a small generated adapter, which ignores `env`.
   - Calls are indirect calls, which a C compiler may inline when it sees the target (inferred).
   - No casts between function pointer types are needed, so there is no UB from calling through a mismatched type.
7. **Effects** (§32). A call through a function or closure value has the effects of its type. A function or closure
   type may carry an effect list, `@effects() fn(i32) -> bool`, which every value assigned to it must satisfy (the
   checker knows a literal's effects, and a named function's). Without the list, a call is assumed to `sleep` and
   `alloc`, as for C function pointers. Code in `@context(atomic)` therefore calls only closures whose type declares
   no `sleep`.
8. **Interpreter and NIR.** NIR gains a function-address operand (it exists for C pointers), a closure aggregate
   `{ code, env }`, and an indirect call. Captures lower to references to the captured places, so moves, drops and
   loans need no new analysis, only the closure value's loan set. The interpreter executes closure bodies as NIR
   functions with an environment argument.
9. **Library.** With closures in place, these may follow in a separate increment, each spec-first:
   - `Slice.sortBy(cmp: borrow fn(borrow T, borrow T) -> i32)`, implemented in the reference over the slice;
   - `forEach`, `any` and `all` on slices and vectors;
   - `Vector.retain(keep: borrow mut fn(borrow T) -> bool)`.

## 4. Consequences

- Callbacks, comparators and small visitors are written directly, with checked captures, and without `unsafe` or
  `@callback` plumbing. The C-callback pattern (`fn` plus `void *`) also gets a safe the reference form: a later binding may
  pass a closure's `code` and `env` to C APIs that take exactly that pair (to be designed with the kernel bindings).
- A closure cannot be returned to a caller unless its captures come from the caller (named regions), nor stored past
  its captures, nor sent to another thread. Those need owning captures (B or C).
- Calls through closures are indirect. Hot loops may still prefer a generic function with an interface bound, which
  is monomorphised. No performance claim is made without a benchmark; a comparator sort against C's `qsort` would
  be the first.
- The parser gains one lookahead (arrow functions), and type syntax gains `fn(…) -> R`, also inside `borrow`.
  `borrow fn(…)` contains no `(` right after `borrow`, so it does not touch G6's source-list rule.

## 5. Questions for the owner (decided 2026-10-25: each as recommended)

1. **Approach A**, scoped closures with one type per signature, now? Recommended: yes.
2. **Literal syntax:** TypeScript/C# arrows `(x) => x + 1`, or a keyword form `fn(x) => x + 1`? Recommended: arrows,
   matching the language's TypeScript/C# style. `=>` already marks expression bodies in the reference (`fn f() => e`) and in
   match arms.
3. **Capture mode inference:** shared or mutable per variable, inferred from the body, as Swift and Rust do? Or
   explicit capture lists (`mut total => …`, C++-style)? Recommended: inferred. The borrow checker reports any
   conflict at the use that causes it, and both kinds of closure type are visible in signatures.
4. **Effects on function types** (§3.7) in G8, or calls through closures always assumed to `sleep` and `alloc` until
   later? Recommended: in G8. Without it, kernel code in atomic contexts cannot call closures at all.
5. **Owning closures later:** B (monomorphised, `F: Fn(i32) -> i32` bounds) when performance needs it, and C only
   with `dyn` (OD-10)? Recommended: record both as open decisions (OD-9a, OD-9b) and decide when a use appears.

## 5a. Amendments during implementation (2026-10-26)

- **One pointer, not two** (§3.6). A closure's value points at its environment, whose first member is the code
  pointer (C11 6.7.2.1p15). So `borrow fn(…)` is an ordinary reference type, whose pointee is the callable object, and
  every reference rule applies unchanged (type-system.md §36.2). A reference to a plain function value is a different
  type, so the two representations cannot be confused.
- **Closures never leave their block** (§4 said a closure could be returned when its captures come from the caller).
  The environment is a local of the block that makes the closure, so returning it is N0244 even then. Returning
  closures needs owning captures: OD-9a or OD-9b.
- **A moved capture** is N0251 (a move out of a reference), not a new code.
- **Effect lists** (§3.7) come in their own step after closures (G8b′, type-system.md §36.3), with these choices:
  - the list goes before `fn`, also inside `borrow` (`borrow @effects() fn(i32)`), since it describes the callable;
  - inference does not narrow a value's type: a named function or literal gets the type without a list unless one
    is expected;
  - a value whose type's list is too wide is N0299 (the wrong kind of value), not a new code. A named function's own
    effects, known only after inference, are N0295 where it is named.
- **The library** (§3.9, G8c, type-system.md §36.4):
  - every method's closure is a `borrow mut fn`, `sortBy`'s too (§3.9 proposed `borrow fn`): a shared closure converts
    to it, and a comparator may count its calls;
  - `sortBy` is an unstable introsort, which needs no allocation and runs on move-type elements through a new
    `swap`;
  - `retain` is lib/core code over the elements followed by a new `truncate`, since generic code cannot name a
    `Vector<T, A>` over an unknown allocator;
  - the methods have the effects of a closure without an effect list. Taking a closure argument's effects instead is
    OD-9c.

## 6. Implementation plan (once accepted)

Spec first (`type-system.md` §36, `grammar.md`, `lifetimes.md` for closure loans, `intermediate-representation.md`,
`c-backend.md`), then:

- **G8a.** Plain function values: types, named functions as values, calls, C emission and the interpreter.
- **G8b.** Closure literals, captures and loans, environments, `borrow fn`/`borrow mut fn`, and effects on function
  types.
- **G8c.** `sortBy`, `forEach`/`any`/`all` and `retain`, a sort benchmark against `qsort`, and the fuzzer generating
  closures.

Tests to add (none run yet):

- run-pass: captures read and written, a closure passed down two calls, a generic higher-order function, a closure
  in a struct with a region parameter, effects, compiled and interpreted;
- check-fail:
  - a write to a captured variable while a closure that reads it is live;
  - a `mut` closure where a shared one is expected;
  - a closure escaping its captures (returned, stored in a box);
  - a moved capture;
  - `break` out of a closure;
  - an atomic context calling a closure without `@effects()`;
- M2's mutation suite extended with closure loans.
