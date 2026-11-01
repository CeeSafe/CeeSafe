# ADR 0015 — M13 G2–G3: dynamic dispatch and owning closures

> **Inherited.** ADR 0015 was accepted before CeeSafe's fork (Decision 0006), when the language had its
> earlier name. In this record, "the reference" is that language and project and "the reference compiler" its compiler:
> read them as CeeSafe and `ceesafec`. Links to documents CeeSafe did not keep are given as text.

- Status: **Accepted** (project owner, 2026-10-28, taking every recommendation in §5). The wording is the agent's;
  the owner may revise it. Not yet implemented: the decisions go into `type-system.md` (§21, §36, and a new §37),
  `grammar.md`, `lifetimes.md`, `intermediate-representation.md` and `c-backend.md` before any code.
- Scope: M13 G2 and G3 (`milestone-13.md`). It closes OD-10 (`dyn Interface`) and OD-9b (owning, heap-allocated
  closures), which ADR 0012 deferred "only together with `dyn`". It amends `type-system.md` §21.1 ("Interfaces are not
  types").

## 1. Context

- **Interfaces are bounds only.** `type-system.md` §21.1: "there is no `dyn Shape` value in v0 (OD-10)". Generics are
  monomorphised (§21.2). A list of shapes of different types cannot be written. Probe at `b9fa451`:
  `Vector<Box<dyn Shape>>` is `expected '>', found identifier` (N0100).
- **Interface methods are dispatchable.** Every method has a receiver: `this`, `borrow this` or `borrow mut this`.
  Methods take no type parameters, and v0 has no `Self` type (§21.1). So every method that takes a reference receiver
  could be called through a table of function pointers. A method that takes `this` by value could not, since the
  caller does not know the value's size.
- **Closures are scoped.** A capturing closure is a reference to an environment in its maker's block. So it cannot be
  returned, stored past its block, or sent to another thread (§36.2; ADR 0012 §5a). Probe: returning
  `(x: u64) => x + n` as `fn(u64) -> u64` is N0299 ("this closure captures 'n', so it is a 'borrow fn'").
- **Allocation is explicit and fallible.** `Box.create` returns `Result<Box<T, A>, AllocError>` (§18). The kernel
  profile allows allocation only through an explicit allocator (`KernelHeap`, C5). So an owning closure must not
  allocate behind the programmer's back.
- **No C-level UB from function pointers.** ADR 0012 §3.6 generates an adapter instead of casting function pointer
  types. The same rule applies to tables of methods.
- **Threads (G4) will need both features.** A thread's body is an owning closure, sent to another thread. That ADR
  comes after this one. This one should not rule out marking a `dyn` type as safe to send (§3.8).

## 2. Options

### Dynamic dispatch

| | A. Fat pointers (proposed) | B. A vtable in the object | C. Enums only |
|---|---|---|---|
| A `dyn` value is | `{ data, vtable }` behind a reference or box | a thin pointer to an object whose first field is its vtable | no `dyn`: users write a closed `enum` and `match` |
| Works for existing types | yes (any `T` that implements `I`) | only types declared to carry a header | yes, but the set of types is closed |
| Cost | two words per pointer; one indirect call | one word; a header in every object | a branch per call |
| Precedent | Rust, Go's interface values | C++ | — (what the reference has today) |

A works with today's types and values, needs no layout change, and keeps `borrow dyn I` free (no allocation). B
changes object layouts. C is the status quo. **A is proposed.**

### Owning closures

| | D. `move` literals boxed as `Box<dyn fn(…) -> R, A>` (proposed) | E. Monomorphised closures with `Fn` bounds (OD-9a) | F. Garbage-collected or reference-counted closures |
|---|---|---|---|
| Storage | one explicit, fallible `Box` allocation | none: the closure is a value of an anonymous type | implicit heap |
| Can be returned or stored in a field | yes | only through generics | yes |
| Call | indirect | direct, inlinable | indirect |
| Needs | A (`dyn`), a `move` literal | anonymous types, `Fn` interfaces, bound syntax | a runtime the reference does not have |

D follows from A with little new: a `move` literal's environment is an ordinary value, and the box holds it behind a
`dyn` function type. E stays open as OD-9a, the faster path for generic code. F conflicts with the reference's memory model.
**D is proposed.**

## 3. Proposed decision

1. **`dyn I` types.** For an interface instantiation `I` (`Shape`, `Comparable<i32>`), `dyn I` is a type whose values
   are "some type that implements `I`". It has no size known to the compiler, so it appears **only behind a pointer**:
   - `borrow dyn I` and `borrow mut dyn I`: references, scoped like every reference (§13);
   - `Box<dyn I, A>`: an owning box, a move type that needs drop.

   A `dyn I` anywhere else is a new error: a local, a field, a parameter, an array or `Vector` element, or a type
   argument. `Vector<Box<dyn I>>` is allowed, since its elements are boxes.
2. **Dyn-compatible interfaces.** `dyn I` is allowed when every method of `I` takes `borrow this` or
   `borrow mut this`. A method that takes `this` by value makes `I` incompatible: `dyn I` is a new error, which names
   that method. A generic interface is compatible once its type arguments are given.
3. **Coercions** happen only where the expected type asks for them, never through inference:
   - `borrow T` → `borrow dyn I` and `borrow mut T` → `borrow mut dyn I`, when `T` implements `I`;
   - `Box<T, A>` → `Box<dyn I, A>` (a move of the box, with no new allocation);
   - a `T` that does not implement `I` is N0268, as for an unsatisfied bound.

   There is no way back: v1 has no downcasting and no `is`/`as` from `dyn I` to `T`.
4. **Calls.** `x.area()` on a `borrow dyn Shape`, a `borrow mut dyn Shape` or a `Box<dyn Shape>` calls through the
   table. The receiver rules are those of a reference to `T`: a `borrow mut this` method needs mutable access. A
   call's effects are those declared on the interface method. Without a declaration, it may `sleep` and `alloc`, as
   for C function pointers and closures without a list (§32.1, §36.3). An `impl` method's inferred effects must fit
   what its interface declares (N0295).
5. **What `T` may be.** In v1, `T` must not be scoped: no references, no closures and no region parameters, so the
   `dyn` value carries no loans other than the pointer's own. That keeps `lifetimes.md` unchanged: `borrow dyn I`
   carries the loan of the reference it came from, and `Box<dyn I>` carries none. A `dyn(r) I` holding scoped values
   is future work.
6. **Owning closures** (OD-9b):
   - **Types.** `dyn fn(T…) -> R` and `dyn mut fn(T…) -> R` are the `dyn` forms of the closure types. Behind a
     `Box`, they own their environment: `Box<dyn fn(u64) -> u64>` is an owning closure. The existing
     `borrow fn(…) -> R` keeps its meaning and representation (§36.2). It is not rewritten as `borrow dyn fn`.
   - **Literals.** `move (x) => x + n` is a **move literal**. It captures each named local **by move**, or by copy
     for a Copy type. Its value may appear only as the argument of `Box.create` or `Box<…, A>.createIn` where the
     expected result is a `Box<dyn fn…, A>` or `Box<dyn mut fn…, A>`. Anywhere else, it is a new error, until OD-9a
     gives move literals their own types.
   - **Mutation.** A body that changes a captured value makes the literal a `dyn mut fn`, called through a
     `borrow mut` of the box. A `dyn fn` is called through a shared one.
   - **Allocation.** `Box.create(move (x) => x + n)` returns `Result<Box<dyn fn(u64) -> u64>, AllocError>`.
     Allocation stays explicit and fallible, and the kernel profile can use it with `KernelHeap`.
   - **Captures.** v1 allows only non-scoped captures, by the same rule as 5. Capturing a reference is a new error.
7. **C representation.**
   - `borrow dyn I` is `struct { void *data; const ceeT_<T,I> *vt; }`, two words.
   - `Box<dyn I, A>` is the same pair with an owning `data`.
   - The table is a `static const` struct per (`T`, `I`). It holds the drop glue, the size and alignment (for a user
     allocator's `deallocate` layout), then one entry per method in declaration order.
   - Each entry is a generated adapter `R ceeTA_…(void *self, A…)`, which converts `self` to `T *` and calls `T`'s
     method. So no function pointer is ever called through a different type.
   - A call is `x.vt->m(x.data, args…)`. Dropping a `Box<dyn I, A>` calls `vt->drop(data)`, then frees `data` with `A`
     and the table's size and alignment.
   - A boxed move literal's environment is a struct of its captures. Its table holds the body as the call entry and
     the environment's drop glue.
8. **Room for threads (G4).** The thread ADR may add marker bounds to `dyn` types (`dyn fn() + Send`). Nothing here
   depends on that, and nothing here prevents it.
9. **NIR and the interpreter.** NIR gains a `dyn` pointer aggregate `{ data, vtable }`, a vtable constant per
   (`T`, `I`), and a call through a table entry. Coercion is building the pair. The borrow checker treats
   `borrow dyn I` as a reference, so it needs no new rule. The interpreter keeps a table of the implementing method
   per (`T`, `I`) and calls it.

## 4. Consequences

- Open sets of types become possible: `Vector<Box<dyn Shape>>`, plugin-style registries, callbacks stored in
  structs, and functions that return closures (`fn adder(n: u64) -> Box<dyn fn(u64) -> u64>`).
- A `dyn` call is indirect and not inlined. Hot code can still use generics. No performance claim without a
  benchmark: a `dyn` against generic dispatch comparison joins `benchmarks/`.
- The language gains one keyword in types (`dyn`), one literal prefix (`move`) and one generated table per
  (`T`, `I`) actually coerced, so code size grows only with use.
- Scoped `dyn` values (`dyn(r) I`), downcasting, and move literals outside a box remain open: OD-10a, OD-10b and
  OD-9a.

## 5. Questions for the owner (decided 2026-10-28: each as recommended)

1. **Fat pointers (A) for `dyn I`, behind references and `Box` only?** Recommended: yes.
2. **Owning closures as boxed `move` literals (D), `Box<dyn fn(…) -> R>`?** Or wait for monomorphised closures
   (OD-9a)? Recommended: D now. It is what returning or storing a closure needs. OD-9a can follow for speed.
3. **Syntax.** `dyn I` and `move (x) => …`, as in Rust? Or a reference-specific spelling, such as `any Shape` (Swift)
   or `interface Shape`? Recommended: `dyn` and `move`. They are short, widely known, and say what they mean.
4. **v1 limits:** no scoped `T` behind `dyn`, and no scoped captures in move literals (§3.5, §3.6). Recommended: yes.
   They keep the borrow checker unchanged, and `dyn(r) I` can follow once a use needs it.
5. **No downcasting in v1.** Recommended: yes. Closed sets of types are better served by enums and `match`.

## 6. Implementation plan (once accepted)

Spec first (type-system §21 amended, §37 `dyn`, §36.5 move literals; grammar; IR; c-backend), then:

- **G2a.** `dyn I` types, compatibility, coercions, calls through `borrow dyn I`: checker, NIR, C, interpreter.
- **G2b.** `Box<dyn I, A>`: coercion from `Box<T, A>`, drop through the table, user allocators.
- **G3.** Move literals, `dyn fn`/`dyn mut fn`, boxing, calls; the fuzzer generating `dyn` calls and boxed closures.

Tests to add (none run yet):

- run-pass, compiled and interpreted:
  - a heterogeneous `Vector<Box<dyn Shape>>`;
  - `borrow mut dyn` mutation;
  - a generic interface;
  - drop order through `Box<dyn I>`;
  - a returned adder closure;
  - a counter closure (`dyn mut fn`);
  - a closure stored in a struct field;
  - fault injection on `Box.create(move …)`.
- check-fail:
  - `dyn I` by value;
  - an incompatible interface;
  - a `T` that does not implement `I`;
  - scoped `T`;
  - a move literal outside a box;
  - a scoped capture;
  - a `dyn mut fn` called through a shared reference;
  - a `dyn` call in an atomic context without `@effects()`.
- The M2 mutation suite extended with `dyn` references.
