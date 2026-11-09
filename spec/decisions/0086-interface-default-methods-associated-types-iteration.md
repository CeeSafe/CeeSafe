# ADR 0086 — Interfaces: default methods, `Self`, associated types and constants, and an `Iterator<T>` interface

- Status: **Accepted** by the owner, 2026-11-09 ("Accept all ADRs as recommended": every question for the owner answered as recommended), the day it was proposed. Raised by the Bun port's M0 (`~/dev/CeeSafeApps/bun/ROADMAP.md` §3, M0: "default
  interface methods; decide on associated types and an iterator trait"). It rests on three things: counts of Bun
  1.4.4's 540 traits and 5,070 trait impls, measured on 2026-11-09 with small Python scripts (§1.1); probes compiled
  with ceesafec 0.24.2 the same day (§1.2); and the current checker, parser and C backend sources (cited by line).
- Scope: type-system.md §20.1 (generic methods), §20.2 (inference), §21.1–§21.2 (interfaces, bounds), §10 (associated
  constants), §16.4 (`for` over an iterator), §37.1 and §37.4 (dyn compatibility, tables); grammar.md §1.3 (`Self`)
  and §2 (`InterfaceDecl`, `ImplDecl`, the `T.Item` type); diagnostics.md (N0267, N0331, N0265); the parser
  (`src/syntax/parser.cee`), the checker (`src/check/interfaces.cee`, `declare.cee`, `arrays.cee`, `dyn.cee`), the C
  backend's tables (`src/backend/c_dyn.cee`), the interpreter; lib/core (`Iterator<T>`, `text.cee`'s iterators); the
  corpus; guide/coming-from/rust.md and guide/for-agents.md.

## 1. Context

### 1.1 What Bun uses

Method: a walk of `upstream/src/**/*.rs` that skips `test`/`tests` directories and generated files, like
`analysis/bun_rust_features.py` (3,186 files). Comments and string literals are stripped, then trait bodies are found
by brace matching and their items classified at depth 1. Run with `python3 -I traits.py upstream` (and
`dyntraits.py`, `supers.py`) from the port's root; the scripts and probes are in
`~/dev/CeeSafeApps/bun/analysis/adr-evidence/0086/`. The counts are regex-based, so they are approximate, to a few
per cent, and cover only that file set: a count over every `.rs` file, tests and comments included, finds a little
more (547 traits, 5,120 `impl … for`, 49 `impl Iterator for` in one reviewer's count).

| What | Count | Notes |
|---|---:|---|
| Trait definitions / `impl … for` | 540 / 5,070 | 5,082 with comments counted (the census) |
| Required / default trait methods | 1,389 / 555 | 124 traits have defaults: 58 have one, 35 have 2–4, 31 have 5 or more |
| Defaults that call a required method of their trait | 122 | `self.m(…)` or `Self::m(…)` |
| Defaults in traits with no required method | 136 | all-hooks traits (the regex validator's `Handler`: 36 defaults) and extension traits |
| Trait functions without `self` (static) | 454 | 23% of trait functions; constructors `fn new(…) -> Self` |
| Trait functions naming `Self` in their signature | 496 | 117 return `Self`, `Option<Self>` or `Result<Self, _>` |
| Generic methods in traits | 75 | including `impl Trait` arguments |
| Associated types | 83 in 53 traits | 39 bounded, 10 with a default, 15 generic (GATs) |
| Associated constants | 166 in 87 traits | 58 with a default |
| Traits with supertraits | 247 | names: `Sized` 119, `Copy` 73, `Sync` 19, `Send` 10; 54 name a first-party trait |
| Traits used as `dyn` | 34 | 17 have defaults; at most 2 have a member a table cannot hold (one, a `Handler`, has 12 constants and a type), and both trait names are shared with other traits, so a `dyn` use may be the other trait |
| `impl Iterator for` / `impl IntoIterator for` | 51 / 25 | `lint` 15, `bun_core` 7, `format` 6, `collections` 6 |
| Functions returning `impl Iterator` | 270 | `lint` 141, `sema` 31, `format` 25 |
| `Iterator<Item = …>` / `IntoIterator<Item = …>` | 373 / 59 | in bounds and `impl Iterator<…>` results |
| Iterator sources (`.iter()`, `.chars()`, `.split(…)`, `.keys()`, …) | 15,772 | 10,452 followed by an adapter; 445 by 3 or more |

Adapter chains, by the number of chains that use each adapter: `map` 2,038, `any` 2,019, `enumerate` 1,405, `collect`
1,295, `filter` 888, `zip` 710, `all` 701, `find` 658, `count` 451, `filter_map` 420, `take_while` 399, `copied` 398,
`rev` 368, `position` 338, `flatten` 229, `chain` 227, `find_map` 164, `flat_map` 162, `skip` 144, `for_each` 137,
`fold` 102, `take` 100; the rest under 75 each.

Examples:
- A default that calls a required method, `src/watcher/Watcher.rs:146`–`156`:
  `fn on_error(&mut self, err: sys::Error);` then
  `fn on_watch_error(&mut self, err: sys::Error) { self.on_error(err); }`.
- The lint `Rule` trait (892 impls), `src/lint/rule.rs:638`: `const META: Meta;` (639), `type State<'a>;` (647),
  `fn new(options: &Options) -> Self;` (652), and 27 empty hooks such as
  `fn expr<'a>(&self, _expr: Expr<'a>, _cx: &mut Cx<'a, Self>) {}` (675), of which each rule overrides a few.
- uWS's socket handler, `src/uws_sys/vtable.rs:38`: `type Ext;` (40) and `const HAS_ON_DATA: bool = false;` (46):
  constants with defaults that select code at compile time.
- An iterator, `src/lint/types/symbol.rs:107`: `impl<'a> Iterator for SymbolIter<'a> { type Item = TsSymbol<'a>;
  fn next(&mut self) -> Option<TsSymbol<'a>> { … } }`.

### 1.2 What CeeSafe does today (ceesafec 0.24.2, probes of 2026-11-09)

| Probe | Result |
|---|---|
| `impl Named for A` without the interface's `greet`, which has a body (Bun's `analysis/probes/g4_default_method.cee`) | 0.24.2: N0267 "'impl Named for A' is missing method 'greet'", then N0200 at `a.greet()`; since `b7c9f41`: N0100 at the body |
| An interface method body that returns `"not a number"` from a `u32` method (`p1_body_unchecked.cee`) | 0.24.2: **accepted with no error**, the body parsed and never checked; since `b7c9f41`: N0100 "expected ';' after an interface method's signature … an interface method has no body" |
| `fn copyOf(borrow this) -> Self;` | N0105 "expected type, found 'Self'" |
| `type Item;` in an interface | N0100, N0101 |
| `interface B: A { … }` | N0100 "expected '{', found ':'" |
| `fn make() -> u32;` in an interface | N0267 "interface method 'make' needs a receiver" (`src/check/interfaces.cee:112`) |
| `fn visit<T>(…)` in an interface and its impl | N0216 (`src/check/declare.cee:620`; ADR 0064 §1) |
| `for (x in it)` with `it: I`, `I: Iter<u32>` (`p6`, `p8`) | N0205 (`src/check/arrays.cee:553`): the structural rule needs a struct |
| `for (x in it)` with `it: borrow mut dyn Iter<u32>` (`p9`) | N0205 |
| The same loop written with `loop`/`match (i.next())` through the bound (`p6b`) | works (prints 10) |
| `impl<region r> Iter<StringView<r>> for Words<r>` (`p11`) | works: a scoped item type is fine as an interface argument |

So the parts are mostly there. An impl's methods are declared on the target by `declareFunctionIn`
(`src/check/interfaces.cee:704`) and recorded as `ImplMethod`s (`:713`, looked up by `implMethodFor`, `:752`). Bound
calls and `dyn` tables both find methods there: `emitDynTables` emits one adapter per HIR method in `table.methods`
(`src/backend/c_dyn.cee:183`–`200`). Generic impls (ADR 0054) already check a method once and instantiate it per
target instance.

### 1.3 Earlier decisions on the subject

- type-system.md §21.1: every interface method has a receiver, methods have no type parameters, "v0 has no `Self`
  type", and an impl "must define exactly the interface's methods". Since commit `b7c9f41` (after the probes above),
  a signature ends with `;`: a body, a block or `=> e`, is N0100 at parse time, which closed the unchecked-body hole
  of `p1`. The same commit made every `dyn` box not `Share` (§38.1); nothing here changes that.
- ADR 0052 chose `HashKey<K>` with the bound `K: HashKey<K>` because there is no `Self`; `src/support/map.cee:5` says
  the same.
- ADR 0064 §1 did not propose generic methods in interfaces, because "a table cannot hold an unbounded family of
  instances".
- ADR 0074 §2.7 added associated constants to structs and inherent impls, read as `Type.NAME`, but not to
  interfaces. ADR 0074 §2.8 made `for` structural over `next(borrow mut this) -> T?`, and rejected an `Iterator<T>`
  interface because it "gives nothing over the shape rule today". The probes above show what it would give:
  iteration over a bound or a `dyn`.

This ADR **extends** §21 and ADR 0074 §2.7–§2.8. It **amends** §21.1's body rule (`b7c9f41`'s N0100), its receiver
rule and its rule against generic methods, ADR 0064 §1, and §37.1's N0331 rule (one rule for every member a table
cannot hold). It reverses none of them: every program accepted today keeps its meaning.

## 2. Proposal

### 2.1 Default methods

```ceesafe
interface WatcherContext {
    fn onError(borrow mut this, err: IoError);
    fn onWatchError(borrow mut this, err: IoError) { this.onError(err); }   // a default
}
```

- An interface method with a body (`{ … }` or `=> e;`) is a **default**: this lifts the N0100 that `b7c9f41` gives
  a body on an interface method (§21.1). An impl may define it (an **override**, with the same signature: N0267
  otherwise, as today) or leave it out (the impl **inherits** it). A missing method
  that has no default is still N0267.
- **Checking, once.** The default is checked at the interface, as a generic body (§20.3) with one type parameter,
  the implementing type `Self` (§2.2), bounded by the interface itself and its supertraits (§2.6). `this` is `Self`,
  `borrow Self` or `borrow mut Self`, as the receiver says. The body may call any method of the interface (required
  or default) and of its supertraits, and read its constants (§2.4). It cannot use fields or `Self`'s other methods
  (N0234). An error in a default is reported once, at the interface, whether or not any impl inherits it, so `p1`'s
  ill-typed body, N0100 today, becomes a type error at the interface, never an unchecked body.
- **Instantiation, per impl.** For every impl that inherits it, the default is instantiated with `Self` = the target
  (and, in a generic impl, once per instance, as the impl's own methods are: ADR 0054). The instance is an ordinary
  method of the target, recorded as the impl's methods are. So `a.onWatchError(e)` works on the concrete type; a call
  through a bound and a `dyn` table each name the instance, and dispatch stays static (§21.2). An instance that does
  not check for its target is N0222 at the impl, as for any instantiation (§20.4).
- **Names.** An inherited default is a member of the target like a written one. If the target already has a method
  of that name (its own, or another impl's), that is N0228 at the impl, as today for two impls that write one. An
  override does not help, since the written method would clash the same way, and there is no qualified call
  (`I.method(x)`). This is today's limit (one method name per type, §21.1), not a new one: a type cannot implement
  two interfaces that share a method name, default or not. The port renames one of the two methods where a Bun type
  implements both (not measured; Bun's shared trait *names*, §1.1, are a different matter). Qualified calls, if the
  port finds the renames costly, are a later ADR.
- **Effects.** The interface's `@effects` list bounds the default as it bounds every impl (§37.3). The instance has
  its own body's effects.
- **`dyn`.** A default is in the table like any method (§37.4). An inherited one's slot holds its instance's
  adapter, so nothing in `c_dyn.cee` changes. Of Bun's 34 `dyn` traits, 17 have defaults.
- There is no call from an override to the default it replaces (Rust has none either).
- **Derived impls** (ADR 0087's `@derive(Equal, HashKey, OrderKey)`) are ordinary impls: the compiler writes the
  required methods, the impl inherits the interface's defaults, and the written code may use `Self` (§2.2).

### 2.2 `Self`

- `Self` is already a keyword (grammar.md §1.3) with no meaning; this ADR gives it one. Declaring an item, a type
  parameter or a local named `Self` stays N0100 ("a reserved word"), as for `const`.
- **In an interface**, `Self` is the implementing type: opaque in the interface's signatures and defaults, and
  substituted by each impl's target. An impl writes either the target or `Self` where the interface has `Self`.
- **In an `impl` block (inherent or for an interface) and in a struct or enum body**, `Self` is the target type with
  its parameters (`Pair<T>`, `View<r>`). It is valid wherever that type's name is: a type, a struct literal
  (`Self { first: a, second: b }`), a static call (`Self.make()`) and a constant (`Self.MAX`). Elsewhere it is N0105
  in a type and N0200 in an expression.
- With `Self`, ADR 0052's `interface HashKey<K>` could become `interface HashKey`. Nothing here requires that change.

### 2.3 Static functions and generic methods in interfaces

- An interface may declare a function **without a receiver** (`fn create(options: borrow Options) -> Self;`), with
  or without a default. Through a bound it is called as `T.create(o)`, in a default as `Self.create(o)`, and on a
  concrete type as `Rule.create(o)`. This lifts §21.1's "needs a receiver" N0267.
- An interface method may have **type parameters of its own** (§20.1). Through a bound and on a concrete type, it is
  instantiated per call, as a struct's generic method is (ADR 0064 §1). This lifts N0216 for interfaces. Const
  parameters on an interface method (ADR 0083, from 0.26.0) follow the same rule as type parameters: allowed,
  instantiated per call, and N0331 for `dyn`.
- **One dyn-compatibility rule** (§37.1, amended): `dyn I` is N0331, naming the member, if any method of `I` or of its
  supertraits takes `this` by value (as today), has no receiver, has type or const parameters, or names `Self`
  outside its receiver, or if `I` or a supertrait has an associated type (§2.5) or a constant (§2.4). Defaults do not
  affect it. The rule is what keeps `T = dyn I` sound: `dyn I` implements `I` for bounds (§37.2, ADR 0064 §3.1;
  `fn g<T: I>(x: borrow T)` takes a `borrow dyn I` today: a probe of 2026-11-09 prints 7), and generic code may read
  `T.NAME`, call `T.create(…)` or name `T.Item`, none of which a table without a value can answer. Every member
  N0331 lets through is a method with a receiver, which the table holds. Bun's measure: of 34 `dyn` traits, at most 2
  have such a member (the constants add none: the one `dyn` trait with constants, a `Handler`, already has static
  functions), so the rule costs almost nothing in the port.

### 2.4 Constants in interfaces

- An interface may declare `const NAME: T;` or `const NAME: T = v;` (a default). An impl gives
  `const NAME: T = v;`; a missing constant without a default is N0267. The value is a constant expression (§10). A
  default may not use `Self`.
- Each constant becomes an associated constant of the target (ADR 0074 §2.7), read as `Type.NAME`, so the rules
  for names (N0228) and visibility are ADR 0074's. In generic code and defaults, it is read as `T.NAME` or
  `Self.NAME`. There it is a value, not a constant expression (not an array length or a pattern), because it is
  known only per instance. Once instantiated it is the constant, so `if (H.HAS_ON_DATA) { … }` costs nothing at run
  time.
- An interface with a constant has no `dyn` type (N0331, §2.3), so a constant is never read through a table and
  `T.NAME` never meets `T = dyn I`.
- As in ADR 0074, a generic impl cannot define one (N0216), but its instances get the interface's defaults.

### 2.5 Associated types

```ceesafe
interface Encoding {
    type Unit: Copy;
    fn decode(borrow this, bytes: Slice<u8>) -> Self.Unit?;
}
impl Encoding for Utf16 { type Unit = u16; fn decode(borrow this, bytes: Slice<u8>) -> u16? { … } }
fn first<E: Encoding>(e: borrow E, b: Slice<u8>) -> E.Unit? => e.decode(b);
```

- An interface may declare `type Name;` or `type Name: B1 + B2;` (bounds as in §21.2). Each impl must define
  `type Name = T;` (N0267 if it is missing or extra). The impl's type and region parameters may appear in `T`
  (`impl<region r> Source for Words<r> { type Item = StringView<r>; }`). `T` must meet the bounds (N0268, at the
  impl).
- **Projection.** In a type, `P.Name` names the associated type, where `P` is `Self` inside the interface or a type
  parameter whose bounds (with their supertraits) declare exactly one associated type `Name`. Zero is N0200; two or
  more is N0267, ambiguous, as for bound methods. In generic code, `P.Name` is an opaque type with the declared
  bounds (§20.3). Instantiation substitutes the impl's type. A dependency's name cannot be a type parameter
  (N0315), so `P.Name` never reads as `package.Type`.
- `Encoding.Unit` on a concrete type is not proposed: the port writes the type itself there.
- Not proposed: defaults, bounds that fix an associated type (`E: Encoding<Unit = u16>`), and associated types with
  parameters of their own (§4). An interface whose callers need to fix a type takes it as a parameter instead, as
  `Iterator<T>` does (§2.7).

### 2.6 Supertraits

- `interface B: A + Copy { … }`: every supertrait is an interface instantiation or `Copy`. `impl B for T` requires
  `T` to implement each one (N0268, at the impl). A bound `T: B` implies `T: A`: the methods, constants and
  associated types of `A` are reachable through it (§21.2's ambiguity rule applies to the union).
- `dyn B`'s table holds `A`'s methods after `B`'s, in declaration order, so `A`'s methods can be called through it, and
  `dyn B` implements `A` for bounds as well as `B` (ADR 0064 §3.1). There is still no conversion from `dyn B` to
  `dyn A` (§37.2).
- A cycle of supertraits is N0227. Rust's `Sized` supertrait (119 in Bun) translates to nothing: a CeeSafe type
  parameter is sized unless it is given a `dyn` (§37.2).

### 2.7 Iteration: `Iterator<T>` in lib/core, and loops instead of adapters

- lib/core declares `public interface Iterator<T> { fn next(borrow mut this) -> T?; }`. lib/core's iterators
  (`TextSplit`, `TextLines` and the other `next` structs in `lib/core/text.cee`) and lib/std's implement it.
- **`for` (§16.4, extended).** Beyond the structural rule, which stays (no impl is needed for a concrete struct),
  `for (x in e)` accepts an `e` of type `I` (or `borrow mut I`), where `I` is a type parameter bounded by
  `Iterator<T>`, and a `borrow mut dyn Iterator<T>` or a mutable place of `Box<dyn Iterator<T>>`. `x` has type `T`.
  The header, borrowing and destruction rules are §16.4's.
- **The item is a parameter, not an associated type.** Callers fix it directly (`I: Iterator<u8>`), with no
  equality bound, and `dyn Iterator<T>` stays dyn-compatible (§2.3). Rust's 373 `Iterator<Item = X>` become
  `Iterator<X>`.
- **Inference from a bound (§20.2, extended).** A type parameter that the arguments do not determine is inferred
  from a bound `I: J<…T…>` when `I`'s argument implements exactly one instantiation of `J`. Otherwise it is N0265, as
  today. So `fn count<T, I: Iterator<T>>(it: I) -> usize` is called as `count(text.split(","))`. For `Iterator`, the
  instantiation is always unique: two impls on one type would both define `next` (N0228).
- **No `IntoIterator`.** `for` already takes arrays, slices, vectors and iterators. A type with something to iterate
  gets an `iter()` method that returns its iterator struct (Bun's 25 `IntoIterator` impls).
- **No adapter library in this ADR.** A lazy `Map<I, F>` would hold a closure in a field, and a closure in a field
  cannot be called (AGENTS.md, "Common mistakes"). Without `impl Iterator` results, every chain would also need a
  named type. The port follows its roadmap (§3, "iterator chains become loops"), with this translation:

| Adapter (chains) | Loop form |
|---|---|
| `map`, `filter`, `filter_map`, `copied`, `cloned` (≈4,100) | the body, with `if` or `match` for the filter; copies are implicit for Copy elements |
| `collect` (1,295) | a `Vector` the loop pushes into (`?` on each `push`) |
| `any`, `all` (2,720) | lib/core's slice `any`/`all` (§36.4) on a slice, else a loop with `break` |
| `find`, `position`, `find_map`, `count`, `fold`, `sum`, `max`, `min` (≈1,800) | a loop with a result local, and `break` where Rust short-circuits |
| `enumerate`, `zip`, `rev`, `skip`, `take`, `step_by` (≈2,750) | `for (i in 0..n)` over indexes; `rev` counts down with `while` |
| `take_while`, `skip_while` (409) | `break`, or a flag |
| `chain`, `flatten`, `flat_map` (618) | two loops, or nested loops |
| `peekable` (61) | a local holding the looked-ahead optional |

- A function returning `impl Iterator` (270) returns a named iterator struct implementing `Iterator<T>`. If its
  chain has closures, the struct does their work in its `next`.

## 3. Alternatives considered

- **One shared default body, called through a table** (as Java and Swift do). It makes one C function, but every
  call on a concrete type or through a bound becomes indirect, against §21.2's static dispatch, and a `borrow this` of
  an unknown size needs a table even where no `dyn` exists. Monomorphising matches generic impls (ADR 0054), and the
  C compiler folds empty defaults like `Rule`'s hooks.
- **Defaults as free generic functions** (`fn onWatchError<W: WatcherContext>(w: borrow mut W, …)`), which work
  today (probe `p10`). Every caller has to know which half of the API is which (ADR 0064 §1's complaint). It also
  cannot be overridden, and Bun's lint rules override some of 27 hooks.
- **Associated types as extra interface parameters only.** `trait Handler { type Ext; }` would become
  `interface Handler<E>`, and every `H: Handler` would become `<E, H: Handler<E>>` in every signature and struct
  that holds one. With §2.7's inference rule this works, but it spreads a parameter through 53 traits' users. Both
  are proposed: associated types where the impl chooses the type, a parameter where callers fix it.
- **`Iterator` with an associated `Item` and equality bounds** (Rust's design). It needs `I: Iterator<Item = u8>`,
  a new bound form, and makes `dyn Iterator` need `dyn Iterator<Item = u8>`. The parameter gives the same
  uniqueness for free (N0228 on `next`).
- **Keep `for` structural only** (ADR 0074 §2.8). Then generic and `dyn` iteration stays a `loop`/`match`, which
  the port would write about 25 times for `dyn Iterator` and wherever `I: Iterator` is a bound (32).
- **Lazy adapters in lib/core, with closures as type parameters.** This needs callable closure fields and opaque
  result types, two language changes larger than this ADR, for chains the roadmap already turns into loops.
- **Rust's `where Self: Sized`, to keep a few methods out of the table.** It is more to learn for the ≤2 `dyn`
  traits that need it.

## 4. Not in this ADR

- Associated types with their own type or region parameters (15 GATs). The largest user is `Rule`'s
  `type State<'a>` (581 impls define it). That needs region-parameterised associated types and its own ADR; until
  then the port translates `Rule` by hand (open question for M1).
- Defaults for associated types (10), and bounds that fix one (`E: Encoding<Unit = u16>`, about 40 outside the
  iterator traits).
- Blanket impls, `impl<T: A> B for T` (24 in Bun). They need a new overlap rule. Extension traits translate to
  generic free functions or to defaults (§2.1).
- Opaque result types (`-> impl Iterator`), callable closure fields, and lazy adapters.
- A conversion from `dyn B` to `dyn A`; `dyn I` of an interface with associated types, constants or static functions.
- Constants in generic impls (ADR 0074 §4, unchanged); a default constant that uses `Self`.
- `Send` and `Sync` as supertraits (Bun: 29). Threads take `dyn I + Send` (§38); whether `Send` becomes a general
  bound is for the thread model, not here.
- Changing `HashKey<K>` to use `Self` (§2.2).
- The compiler's own sources: they must still build with the seed chain's compiler (ADR 0080 §4), so none of this
  is used in `src/`.

## 5. Testing

Run (2026-11-09, `~/dev/CeeSafe/out/ceesafec` 0.24.2, `check` or `run --interp`, the probes in
`~/dev/CeeSafeApps/bun/analysis/adr-evidence/0086/`): the twelve probes of §1.2, with the results shown there. Bun's
own probe `analysis/probes/g4_default_method.cee` gave N0267 and N0200, as stated in the port's roadmap; rerun after
`b7c9f41`, it and `p1` give N0100 at the body. A `borrow dyn I` passed to `g<T: I>` prints 7;
`struct Self` is N0100 "a reserved word".

Proposed (corpus, each run in the interpreter and built through C with `--check-leaks`):
- **run-pass `interface-defaults`:** an inherited default calls a required method; an override wins; a default calls
  another default; a generic impl's instances inherit; the same calls through a bound and through
  `Box<dyn I>`/`borrow mut dyn I` (the inherited slot); a default with a move-type `Self` whose instance drops it.
  Expected output written by hand from the program text.
- **run-pass `interface-self-static`:** `fn create(…) -> Self` through `T.create`; `Self` in an impl as a type,
  literal, static call and constant; a generic interface method instantiated at two types.
- **run-pass `interface-assoc`:** constants with and without defaults, read as `T.NAME` and `Type.NAME`, and a
  compile-time switch; an associated type with a bound used in a generic function; supertrait methods through a bound
  and through `dyn B`.
- **run-pass `iterator-interface`:** `for` over `I: Iterator<T>`, `borrow mut dyn Iterator<T>` and
  `Box<dyn Iterator<T>>`; `count(text.split(","))` inferred from the bound; lib/core's `TextSplit` through the
  interface; destruction order as in §16.4.
- **check-fail `interface-defaults-errors`:** an ill-typed default reported at the interface (`p1`, N0100 since
  `b7c9f41`, becomes a type error); a field read in a default (N0234); an override with another signature (N0267);
  an inherited default that clashes with an inherent method (N0228); a missing constant or associated type (N0267);
  `Self` outside an impl (N0105/N0200); a type named `Self` (N0100); an ambiguous projection (N0267); a supertrait the
  target does not implement (N0268); a supertrait cycle (N0227); `dyn I` with a static function, a generic method, a
  const parameter, a `Self` result, an associated type or a constant (N0331, naming the member).
- The contract suite and the fixed point, unchanged. HIR snapshots may move with lib/core's new interface; each such
  change is a reviewed edit.
- Port side: rerun Bun's `g4_default_method.cee` (expected to pass), and translate `src/watcher/Watcher.rs`'s
  `WatcherContext` and one `lint` rule as the first users.

## 6. Questions for the owner

1. Default methods as §2.1: checked once at the interface, instantiated per impl that inherits them, in `dyn` tables
   like any method? Recommended: **yes**. It is the generic-impl machinery reused, and dispatch stays static.
2. `Self` as §2.2: the reserved keyword given a meaning, the implementing type in interfaces and an alias of the
   target in impls and type bodies? Recommended: **yes**. Defaults and constructors cannot be written without it, and the alias makes a
   mechanical port shorter.
3. Static functions and generic methods (type and const parameters) in interfaces, with §2.3's single N0331 rule for
   `dyn`, constants included? Recommended: **yes**. 23% of Bun's trait functions are static, and almost no `dyn`
   trait is affected.
4. Constants in interfaces (§2.4) and associated types without defaults, fixing bounds or parameters (§2.5), with
   `P.Name` projections? Recommended: **yes**. GATs (Rule's `State<'a>`) get their own ADR when M1 reaches `lint`.
5. Supertraits (§2.6), limited to interfaces and `Copy`, with flattened `dyn` tables? Recommended: **yes**.
6. `Iterator<T>` in lib/core with the item as a parameter, `for` over bounds and `dyn`, and inference from a unique
   bound (§2.7)? Recommended: **yes**. It extends ADR 0074 §2.8 without changing a structural loop.
7. No adapter library for now: the port translates chains to loops by §2.7's table, recorded in the port's
   translation rules. Recommended: **yes**. Lazy adapters need closure fields and opaque results first.
8. Release all of it as one minor release (0.25.0), then the Bun port's pin? Recommended: **yes**. The parts share the
   checker's impl code, and N0331's amended rule should arrive in one step. 0.25.0 also carries ADR 0084 and step 1
   of ADR 0085 (after 0.24.3's ADR 0081 and 0082 stage A; ADR 0083 follows in 0.26.0).
