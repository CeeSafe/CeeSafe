# ADR 0054 — Generic impls, and impls for types with region parameters

- Status: **Accepted** by the owner, 2026-11-02 ("Yes, I accept all five ADRs with your recommendations"): every
  question of §5 as recommended. Asked for by the owner for CeeSafe.Web
  (`~/dev/CeeSafe.Web/docs/ceesafe-upstream-work.md`, item U10). CeeSafe.Web's M0 designed around the gap: `Box<dyn
  Transport>` at the connection edge, concrete body types instead of an interface, hook-style middleware instead of
  generic wrappers.
- Scope: the parser, the checker's impls, bounds and `dyn` tables, the backends' view of instantiated impl methods.

## 1. Context

An impl names one concrete, unscoped type (type-system.md §21; `src/check/interfaces.cee`, `processImpl`):

- `impl<T: Transport> Transport for Wrap<T>` does not parse: N0105, since `parseImpl` reads a type right after `impl`.
- `impl Body for RequestBody`, where `RequestBody<region r>`, is N0267: "v0 implements interfaces only for concrete,
  unscoped types". The reason is §37: a `dyn I` value carries no loans of its own.

What exists to build on:

- **Monomorphisation.** A generic function or struct is instantiated per type argument (`instantiateFunction`,
  `instantiateStruct`), and calls through a bound resolve to the concrete type's impl method (`findMethod`, then
  `implMethodFor`).
- **Impls are found by exact type id** (`implements`, `implMethodFor`), so an impl for `Pair<u8>` exists and one for
  `Pair<T>` cannot be written.
- **`dyn I` tables** are recorded per (implementing type, interface type) when a value is coerced (`coerceToDyn`), and
  the backend emits one table per pair.

## 2. Options

**Overlap.** A generic impl `impl<T> I for Wrap<T>` and a concrete `impl I for Wrap<u8>` would both apply to
`Wrap<u8>`. Choosing between them is specialisation, which needs rules for "more specific". **Proposed:** no overlap,
at most one impl per (interface, type constructor). A later ADR may add specialisation.

**Where an impl may be written.** With generic impls, two packages could each write `impl I for Wrap<T>` and both be
linked. **Proposed:** the orphan rule. An impl is written in the package that declares the interface or the type
constructor, so two packages never both provide one.

## 3. Proposed decision

### 3.1 Generic impls

```ceesafe
impl<T: Transport> Transport for Logged<T> {
    async fn read(borrow mut this, buffer: SliceMut<u8>) -> Result<usize, NetError> => await this.inner.read(buffer);
    // ...
}
```

- **Syntax:** `impl<T: B, U> I<…> for C<…T…, …U…> { … }`. The impl's type parameters must each appear in the target
  type; their bounds are as a generic function's (§20).
- **Matching.** A type `C<A>` implements `I` when an impl's target unifies with it and the bounds hold for the
  arguments found. `implements` and `implMethodFor` unify against impl templates as well as concrete impls.
- **Instantiation.** An impl's methods are instantiated per type argument, as a generic struct's are, and cached. A
  call through a bound, a call on the concrete type, and a `dyn I` table all use the instance. The backend sees
  ordinary functions and needs no change.
- **Overlap** (§2): two impls of one interface whose targets have the same type constructor are N0267, generic or not.
  So `impl I for Wrap<u8>` beside `impl<T> I for Wrap<T>` is refused.
- **Orphan rule** (§2): an impl whose package declares neither the interface nor the target's type constructor is
  N0267. Built-in types (integers, `String`, `Vector`) count as lib/core's, so a program implements its own interfaces
  for them, as today.

### 3.2 Impls for types with region parameters

```ceesafe
impl<region r> Body for RequestBody<r> { … }
```

- A struct or enum with region parameters may implement an interface, with the regions named in the impl header. The
  methods' signatures use them as the struct's own methods do (§25).
- **Such a type is used through bounds, statically.** Coercing it to `dyn I` stays N0268, since a `dyn` value carries no
  loans (§37). Scoped `dyn` is OD-10a, separate.
- An interface method that declares its own region parameter (`async fn handle<region r>(…)`, N0216 today) stays
  refused: methods declare no type or region parameters of their own in v0 (`src/check/declare.cee`, N0216).

### 3.3 Compile time

Each instance of a generic impl is checked and compiled once per distinct type argument list, as generic functions
are. The depth limit for instantiation (N0266, 64) applies to impls too.

## 4. Consequences

- CeeSafe.Web can wrap a transport generically (`Logged<T: Transport>`), and implement `Body` for its scoped request
  body.
- **The orphan rule may refuse an impl that compiles today:** one written in a package that declares neither the
  interface nor the type. The implementation searches the corpus, the tests and CeeSafe.Web for such impls and reports
  each before the rule lands. The check is N0267, with a message saying where the impl belongs.
- New diagnostics: none. N0267 and N0268 gain the cases above.

## 5. Questions for the owner

1. `impl<T: B> I for C<T>`, monomorphised per type argument, with at most one impl per (interface, type constructor)
   and no specialisation? Recommended: yes.
2. The orphan rule: an impl lives in the package of its interface or of its type constructor? Recommended: yes.
3. Impls for types with region parameters, usable through bounds but not as `dyn I`? Recommended: yes.

## 6. Implementation plan (once accepted)

1. Spec: §21 (impls, matching, overlap, the orphan rule), §25 (scoped impls), §37 (`dyn` unchanged), diagnostics.md
   (N0267, N0268 cases), the guide's generics page.
2. `src/`:
   - the parser (`impl<…>`);
   - the checker:
     - impl templates;
     - unification in `implements` and `implMethodFor`;
     - instantiation and caching of impl methods;
     - the overlap and orphan checks;
     - scoped impls and the `dyn` refusal;
   - the HIR printer.
3. Tests:
   - check-pass with `.hir` snapshots: a generic impl used through a bound, on a concrete instance and as `dyn I`; an
     impl with two parameters; a scoped impl through a bound;
   - run-pass: the same, natively and in the interpreter, with expected output from the program's specification (a
     wrapper that counts calls);
   - check-fail: overlap; an orphan impl; a scoped type coerced to `dyn I`; a parameter that does not appear in the
     target; an unmet bound;
   - the contract suite, and CeeSafe.Web's three probes (`tests/probes/impl-generic-fail.cee`,
     `impl-scoped-fail.cee`) turned into passing tests.
4. ROADMAP §10.
