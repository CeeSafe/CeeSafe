# ADR 0064 — Generic methods, arrays and slices of views, `dyn` as a bound and scoped `dyn` (from the ClayWind port)

- Status: **Accepted** by the owner, 2026-11-03 ("I accept the ADR", then "Both 0063 and 0064"), with every question
  as proposed: §1 questions 1 and 2 (explicit or inferred type arguments; inherent `impl` in the struct's own
  package), §2 questions 1 and 2 (the collections' rule; `const` tables of static views), §3 question 1 (an inferred
  "behind references only" condition). For §3 question 2 the order is part 1, then OD-10a. Implementation follows in
  the sections' order. The smaller items from the same port are ADR 0063.
- Implemented on the working tree (2026-11-04), not released; §5 records how each part was built and checked.
- Source: `~/dev/CeeSafeApps/CEESAFE-FINDINGS.md` (A1–A3), from a ~25k-line pure CeeSafe port of ClayWind (layout,
  rasterizer, fonts, controls, PNG, hosts). Each section says what the port had to do instead, so the cost of
  leaving things as they are is concrete.

## 1. Methods with their own type parameters, and inherent `impl` blocks (finding A2)

**Today.**
- A method cannot declare type parameters (N0216, "use the struct's, or a top-level generic function"; §21.1).
- Methods live only in the struct body.

**Cost in the port.** `Controls.button(...)` is a method, but `textInputEx(ui, metrics, ...)` is a free function,
only because it is generic over the text metrics. The same goes for `endLayout<M>`, `endFrame<M>`, `renderInto<B>` and
`renderTree<B>`, which belong on `LayoutTree` and `Ui`. Users must learn which half of the API is which.

**Proposal.**
- **Generic methods.** `fn m<T: B>(borrow this, x: T)` is the top-level generic function
  `m<S-params…, T: B>(this: borrow S, x: T)`, instantiated the same way (P3 generics). The receiver adds no new rule.
  Type arguments come from the arguments, as for free functions, or explicitly: `x.m<u32>(…)`.
- **Inherent `impl S { … }` blocks.** Sugar for methods in the body, so a type's methods can be grouped or split
  across files of the package. They have the same visibility, the same "a method cannot have a field's name" rule,
  and no second definition of a name.
- **Not proposed: generic methods in interfaces.** A table cannot hold an unbounded family of instances, so interface
  methods stay monomorphic (§37).

**Questions.**
1. Explicit type arguments on a method call: `x.m<T>(…)` or only inference? (Proposed: both, as for functions.)
2. Inherent `impl` blocks only in the struct's own package (proposed), or anywhere?

## 2. Arrays and slices of scoped values (finding A3)

**Today.** §25.4 rejects array and slice elements of scoped type (N0246), so `Slice<StringView>` and
`[StringView; 3]` are errors. Yet `Vector`, `StableVector` and `SlotMap` elements may be scoped.

**Cost in the port.**
- The controls take item lists as one `"Low|Medium|High"` view, split with `itemCount`/`itemAt`. This is a public-API
  wart that will be hard to remove later.
- The browser page's HTML is 30 separate calls, since a table of lines cannot be written either. Multi-line string
  literals would also cover this case.

**Proposal.** Allow scoped element types in arrays and slices with the collections' rule.
- All elements of one array share its element type's regions. `[StringView<r>; N]` and `Slice<a, StringView<r>>` have
  `r` for every element; elements are not tracked separately. Arrays are already one place for the borrow checker
  (§16.3), so no per-element loans are needed.
- A slice of views has two regions, the slice's and the elements'. As for `Vector<StringView<r>>`, the elements' region
  must outlive the slice's (`r: a`), checked where the slice is made.
- Array literals of string literals (`["Low", "Medium", "High"]`) have `static` elements, so a constant table of
  views is possible. That is the common case, and it fits `const` too (§2, question 2).

**Questions.**
1. Is the collections' machinery (ADR for §25.4's Vector rule) reusable for arrays, or does `[T; N]` by value need
   its own place rule for moves out of elements of scoped type?
2. `const ITEMS: [StringView; 3] = [...]`: allowed under this proposal (static elements)? Proposed: yes, which also
   answers finding B's "a `const` cannot hold a `StringView`".

## 3. `dyn I` satisfying `T: I`, and loans in `dyn` (finding A1)

**Today.**
- `dyn I` does not implement `I` for bounds (N0268, "'dyn Sink' does not implement 'Sink', which 'S' requires").
- A type that holds loans cannot be `dyn` (N0268; §37.2, OD-10a).

**Cost in the port.**
- The layout engine needs text metrics both from a font engine (which borrows its font pool, so it can only be used
  through a bound) and from simple test doubles (natural as `dyn`). With neither composition available, every
  function on the path became generic and free (`endLayout<M>`, `endFrame<M>`, `renderInto<B>`, `textInputEx<M>`).
- A host whose platform borrows its script copies it instead (`ScriptPlatform`, 4 KB per platform).

**Proposal (two independent parts).**
1. **`dyn I` implements `I`.** For an interface without generic methods (all of them, §1), `dyn I` gets the obvious
   impl: each method calls through the table. Instantiating a generic with `T = dyn I` is then allowed where the
   generic uses `T` only behind references (`borrow T`, `borrow mut T`), since `dyn I` is unsized (§37.2). A generic
   that needs `T` by value (moves it, makes an array of it, boxes it) is N0274 at the instantiation, as for scoped
   arguments. Instances are made per use, as now.
2. **OD-10a, scoped `dyn(r) I`.** A `dyn` value that carries the loans of region `r`, so a struct with region
   parameters can be one. The port's use is the common one: a borrowed engine passed to code that wants dynamic
   dispatch. This is the larger change (region erasure, §25.5, must keep `r` on the `dyn` type). Part 1 alone already
   removes most of the port's workarounds, because the generic route then accepts both kinds of caller.

**Questions.**
1. Is "uses `T` only behind references" checkable from the generic's body as P3 has it (the same analysis as
   N0274's "needs an unscoped type"), or does it need a declared bound such as `T: ?Sized`? Proposed: inferred, like
   N0274; no new syntax.
2. Take up OD-10a now, or after part 1?

## 4. Not proposed

- **Blocks as values** (`let y = { …; v };`). §15 G5 leaves this out deliberately, and the port's few cases read fine
  as statements.
- **`abs` on integers.** That is ADR 0063 §5, question 2.
- **The interpreter's speed** (the port's suite takes 18 minutes interpreted, seconds compiled). This is worth a
  benchmark item of its own, not a language change.

## 5. Implementation (2026-11-04)

### 5.1 Generic methods and inherent impls (§1)

- **Generic versions.** A struct method with type parameters is declared once, with those parameters opaque, as a
  `GenericFunctionEntry`. The entry gains `owner` and `ownerEnvironment`, and the generic version records it in
  `HirFunction.methodTemplate` (`src/check/structs.cee`). Interface and `impl I for T` methods keep N0216, with a
  message that says why.
- **Calls.** `x.m(args)` and `S.f(args)` infer from the arguments after the receiver, or take written type
  arguments (`instantiateMethod`, `src/check/generics.cee`). `instantiateFunction` declares the instance with its
  owner and the owner's environment.
- **Written type arguments.** `x.m<T>(…)` parses into `Ast.memberTypeArguments`, keyed by the member's identifier,
  so that `ExprNode.Member` and its many matches stay as they are. The formatter walks them. Type arguments on a
  function without type parameters are N0209.
- **C names.** An instance is `ceeMG_<owner>_<length><name><mangled arguments>` (`src/backend/c_names.cee`).
  `editors/debug/ceesafe_names.py` demangles it, and `tools/debugger_check.py` checks a real name.
- **Inherent impls.** `ItemKind.InherentImpl`. `mergeInherentImpls` (`src/check/inherent.cee`) appends a block's
  functions to its struct's list before anything is declared. The block must be in the struct's own package and
  name the struct with its own parameters (N0267).

### 5.2 Arrays and slices of views; constant views (§2)

- **Elements.** `isScoped` of an array is its element's (`src/types/types.cee`). `checkElement` allows views and
  values holding loans, and rejects bare `borrow T` (`src/check/arrays.cee`). The Copy rule for elements is
  unchanged, so an array of `SliceMut` is still rejected: two mutable views cannot share an array.
- **Constants.** `StringView` and `[StringView; N]` (`isConstantView`, `constElementOk`). `fold` and the constant's
  use carry `HirExpr.StringConst`, which is static data in both backends.
- **Adversarial checks.** Each of these is rejected:
  - an element outliving its data (N0243);
  - an element's loan stored through a parameter (N0272);
  - writing data an array still views (N0241);
  - returning an array of a local's views (N0245).
  The signature that N0272 suggests, `fn keep<region r>(items: Slice<StringView<r>>, into: borrow mut
  Vector<StringView<r>>)`, compiles and runs.

### 5.3 `dyn I` implements `I` (§3.1)

- **Bounds.** `implements` accepts `dyn I` for `I`, with the same arguments (`src/check/interfaces.cee`).
- **Function instances.** An instance whose parameters, locals or result hold a `dyn` by value is N0274 inside the
  instance, which `endInstance` gives as N0222 at the use (`checkDynInstance`). The check came from a test:
  `fn copyOf<M: Metrics + Copy>(m: borrow M) -> M` with `M = dyn Metrics` compiled before it and made a `dyn` local.
- **User types.** Their type arguments may be `dyn` where the instance holds it only behind references
  (`ByRef<dyn I>`); otherwise N0330 (`holdsBareDyn`). Instances made before the members are resolved are checked
  afterwards (`pendingDynInstances`).

### 5.4 Scoped `T` behind `borrow dyn I` (§3.2, OD-10a)

- **The rule.** A type with region parameters coerces to `borrow dyn I` and `borrow mut dyn I`. `Box<dyn I>` still
  refuses it (N0268), since a box keeps no region. Nothing new is tracked. A `borrow(a) View<r>` already requires
  `r` to outlive `a`, and borrowing the value keeps its loans alive (B4), so the `dyn` reference may carry only its
  own loan.
- **Adversarial checks.** Each of these is rejected:
  - changing data while a `dyn` view of it lives (N0240);
  - a struct field holding a `dyn` past its data (N0243, for both the view and its data);
  - a `Vector` of `dyn` references outliving their data (N0243);
  - overwriting a borrowed field (N0240);
  - returning a `dyn` of a local (N0244);
  - storing a parameter's view into a longer-lived holder (N0272).
  A references-to-references swap is not expressible at all (N0246).

### 5.5 Evidence (macOS arm64)

- **New corpus tests:**
  - `run-pass/generic-methods`, `check-fail/generic-methods`;
  - `run-pass/slices-of-views`, `check-fail/slices-of-views`, `check-fail/slices-of-views-loans`;
  - `run-pass/dyn-as-bound`, `check-fail/dyn-as-bound`, `check-fail/dyn-as-bound-by-value`;
  - `run-pass/dyn-scoped`, `check-fail/dyn-scoped`.
  Expectations come from Python, and the run-pass tests pass compiled and with `--interp`.
- **Existing expectations that encoded the old rules**, each changed in place with a note saying so:
  - `check-fail/generics.cee:18` (a generic method);
  - `check-fail/closure-methods.cee:17` (an array of views);
  - `check-fail/region-instances.cee:15` (`[T; 2]` of views);
  - `check-fail/scoped-types.cee:5–6` (arrays and slices of slices);
  - `check-fail/impl-generic.cee:66` (a scoped value behind `borrow dyn`).
- **Full corpus:** 596 tests, 0 failures. **Fixed point:** byte-identical C. **Downstream:** ClayWind (all seven
  packages) and CosmoStack build. The contract suite is recorded in ROADMAP §10.
