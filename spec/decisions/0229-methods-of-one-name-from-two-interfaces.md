# ADR 0229 — Methods of one name from two interfaces, and `I.m(x)` to name one

- Status: **Proposed** 2026-11-24, under the owner's instruction "Implement fixes for findings 6, 15, 19, 26 and 35 in
  CeeSafe" (`CeeSafeApps/sqlite/CEESAFE_FINDINGS.md` finding 6). Implemented 2026-11-24 on
  `fix/findings2-interfaces`.
- Scope: type-system.md §21.1 (impls), §21.2 (bounds), §21.4 (new: which method a call names), §37 (`dyn`);
  diagnostics.md N0228, N0267, N0268; c-backend.md §2.1 (the C names of such methods); the guide's generics page.
  No grammar change: `I.m(x)` is a call of a member of a name, which the parser already reads.

## 1. Context

The SQLite port has a pager that is read from and written to:

```ceesafe
interface PageSource { fn usableSize(borrow this) -> usize; fn read(borrow this, page: u32) -> u32; }
interface PageSink   { fn usableSize(borrow this) -> usize; fn write(borrow mut this, page: u32); }
impl PageSource for WritablePager { … }
impl PageSink for WritablePager { … }   // N0228: 'WritablePager' already has a method named 'usableSize'
```

Both impls are refused, even with identical signatures, because "a type has one method of each name" (§21.1, ADR
0086 §2.1): `Checker.implMethods` is keyed by type and name, and every lookup (a call on the type, a call through a
bound in an instance, a `dyn` table, the C name `ceeM_<owner>_<name>`) found a method by the type and the name alone.
Two interfaces written independently will share names (`size`, `name`, `close`, `next`), and a type is then forced to
rename one, which no caller of the interface can do.

What the spec says today and keeps:

- An **inherent** method (a struct's body, an `impl S { … }` block, an enum's `impl E { … }`) and an interface's method
  of the same name on one type are N0228 (§21.1, ADR 0064, ADR 0107). There is no "inherent wins" rule to keep or add:
  the two cannot coexist, so no call has to choose between them.
- Through a bound, two bounds that both declare the name make the call ambiguous (N0267, §21.2).
- One generic interface is implemented for one argument list a type (ADR 0165 §7).

## 2. Decision

### 2.1 A type may have methods of one name from several interfaces

An impl's methods are the type's methods **for that interface**. Two impls of different interfaces may each give a
method of the same name, with the same signature or a different one: required methods, overrides, inherited
defaults, static functions and generic methods alike. N0228 for an impl's method now means only what it says of the
type's own members:

- an impl's method, or an inherited default, named like an inherent method of the type (its body, an inherent block,
  an enum's block), a field or a variant: N0228, as before;
- two impls of the **same** interface (two argument lists of one generic interface): N0228 for the second, as before
  (ADR 0165 §7). The key is the interface, not its instantiation (§3).

### 2.2 Which method a call names

1. **Through a bound** (`fn f<T: PageSource>(p: borrow T) { p.usableSize(); }`): the bound's method, as before; two
   bounds that both declare the name are N0267, as before. **An instance calls the method of the interface the bound
   named**: with `T = WritablePager`, `p.usableSize()` is `PageSource`'s, never `PageSink`'s. Generic code means in
   each instance what it meant when it was checked (§20.3), so this is the only reading that keeps that promise.
   The same holds for an interface's default (`Self` bounded by the interface), a generic impl's methods and a
   generic struct's methods that call through their bounds, and for `T.create(o)`.
2. **Through `dyn I`**: the slot holds the type's method for `I`, so `borrow dyn PageSink` calls the sink's.
3. **Named**: `PageSink.usableSize(pager)` (§2.3) calls the sink's.
4. **On the concrete type** (`pager.usableSize()`): if the type has one method of the name, that one. If it has
   several, those of interfaces the call cannot see do not count: an interface that is file-private to another file
   (ADR 0109) or not public in another package, and a method that is `private` in another file. One visible
   candidate is the call's. Two or more are **N0267, ambiguous**: "'usableSize' is ambiguous: 'WritablePager' has
   it from both 'PageSource' and 'PageSink'; name the interface: 'PageSource.usableSize(value, …)' or
   'PageSink.usableSize(value, …)'", with a fix only where it is sure (§2.5).
5. **A static function on the concrete type** (`WritablePager.create(o)` where both interfaces declare `create`):
   N0267 saying to call it through a bound (`fn make<T: PageSource>(o: …) -> T => T.create(o);`). §3 says why no
   qualified static form is added.

### 2.3 The qualified call `I.m(x, args…)`

`I.m(x, a, b)` calls interface `I`'s method `m` with `x` as `this`: the method a bound `I` would call. It is the
spelling for case 4 above, and it reads the same in generic code (`PageSink.usableSize(p)` for `p: borrow T` with
`T: PageSource + PageSink`, whose `p.usableSize()` is N0267).

- `I` is an interface in scope, or `I<A, …>` with its arguments; the method is `I`'s or a supertrait's (§21.1:
  supertraits).
- The first argument is the receiver, written as an argument to the receiver's parameter is: `x` for `this` (moved
  or copied) and `borrow this` (a value is borrowed, a reference passed as it is), `mut x` for `borrow mut this` (a
  `borrow mut` reference may be passed as it is). `mut` before the receiver of a method that does not take
  `borrow mut this` is N0249 with the fix that removes it; a `borrow mut this` receiver given as a value without
  `mut` is N0249 with the fix that writes it. Auto-dereferencing and a box's contents are as in a method call.
- The receiver's type implements `I` (a concrete type, a type parameter bounded by `I` or by an interface that has
  `I` as a supertrait, a `dyn I`); otherwise N0268, "'X' does not implement 'I'". With `I<A>` written, the type must
  implement `I` with those arguments (N0268).
- Errors of the form itself: no argument (N0209: the receiver is missing), a name that `I` and its supertraits do not
  declare (N0200), a static function (N0233: "'I.create' is a static function, so there is no value to name its type
  by"). A generic method takes its own type arguments as in a method call: `I.m<U>(x)`.
- Visibility is the method call's: `I.m(x)` is valid where `x.m()` would be, had `m` been `I`'s alone.

### 2.4 C names

An impl's method whose type has another method of the same name from another interface is named with the interface:
`ceeMI_<owner>_<interface>_<name>` (`ceeMI_13WritablePager_10PageSource_usableSize`), and an instance of a generic
method or an inherited default (which is named as an instance, `ceeMG_`, today)
`ceeMGI_<owner>_<interface>_<length><name><arguments>`. The interface is written as a type's name is (its length,
its package's name for a dependency's, `lib_` before a library interface a package shadows), so two such methods of
one type never share a C name. `ceeMI_` and `ceeMGI_` are new families: after `ceeM`, `I_` and `GI_` differ from `_`
and `G_`. A method whose name is its type's alone keeps its `ceeM_`/`ceeMG_` name, so no program that compiled emits
different C.

### 2.5 The fix

tooling.md §5.2 offers a fix only where the intent is unambiguous. Which interface the writer meant is theirs to
say, so the ambiguous call has a fix only when exactly one candidate is a method taking as many arguments as the call
has (`pager.label()` with `label(borrow this)` from one interface and `label(borrow this, extra: u32)` from the
other): then the other could not be meant. The fix writes `I.m(` before the receiver (`I.m(mut ` for a
`borrow mut this` method) and replaces the text from the receiver's end to the first argument with `, `, or to the
closing parenthesis with nothing: two edits, applied together. A call with type arguments on its method
(`x.m<U>()`) has no fix. Otherwise the message names both forms, and no fix is offered. The arguments never choose
the method itself (§3, overloading): the fix only writes what the call must say.

## 3. Options not taken

- **"Inherent wins"** over interface methods of the same name, as in Rust. CeeSafe has no such pair (N0228), and
  allowing one would add a rule that changes what a call means when a method is added to the type. Not part of this
  ADR; the owner may take it on its own.
- **Choosing by signature** (overloading) when two candidates differ in their parameters. CeeSafe has no overloading;
  a call would change meaning when an argument's type changes.
- **A type-qualified spelling** (`WritablePager as PageSource`, `WritablePager.PageSource.create()`) for static
  functions. It needs new grammar for a case a bound already covers, and `I.create(…)` cannot know the type. If ports
  show static functions clash often, `I<…>.create<T>` or similar can be designed then.
- **Keying by the instantiation** (`Conv<u8>` and `Conv<u16>` on one type, ADR 0165 §7 option A). The machinery here
  is most of it, but an instance would need its generic version's bound substituted to choose between instantiations
  of one interface, and the overlap, inference (ADR 0165) and record rules would change with it. Left to that ADR's
  option A; this ADR does not make it harder.

## 4. Implementation (2026-11-24)

- `ImplMethod` records its interface (none for an inherent block's or an enum's function); `implMethodIn` finds a
  type's method for one interface (src/check/same_name_methods.cee, the new file that holds this ADR's checker
  code). N0228 in `implementFor` and `inheritDefault` (src/check/interfaces.cee) counts only methods of no interface
  or of the same one (`clashingImplMethod`); `addImplMethod` records a method and marks both of a shared name.
- `findMethodVia` (src/check/interfaces.cee; `findMethod` is it without an interface) resolves a name with several
  candidates by `chooseImplMethod`: the visible ones; then the interface a bound gave the same call in its generic
  version (`Checker.boundCalls`, recorded by the bound branch of `findMethodVia`, keyed by the method name's file
  and position); then the interfaces of the bounds of the type parameters that the instance's environment maps to
  the type (`EnvPair.parameter`, the parameter's syntax), and in an inherited default the default's interface (its
  `Self`'s bound), so an instance checked before its generic version's call was recorded chooses the same; else N0267
  (`reportAmbiguousCall`, with its fix, from `checkMethodCallVia`; `chooseStaticFunction` from `checkStaticCall`
  and `checkTypeCall`).
- `dyn` tables (`coerceToDyn`), the allocator's `allocate`/`deallocate`, `derivedEqual`, an `@into` method and an
  open impl's method (ADR 0165) take the method of their interface.
- `checkInterfaceCall` (from the `Entity.Interface` case of `synthCall`, src/check/expr.cee) and
  `checkMethodCallVia` (src/check/members.cee; `checkMethodCall` is it with `plainCall()`) check `I.m(x, …)`: the
  receiver as a method call's, no built-in method of the type, the lookup restricted to the interface that declares
  `m` (`qualifiedMethod`); `passReceiver` is the receiver passing both share.
- `HirFunction.implInterface` and `sharedName` (src/hir/hir.cee); `appendCFunctionName` (src/backend/c_names.cee)
  writes `ceeMI_` / `ceeMGI_` names for them (`sharedImplInterface`, `appendInterfaceId`).
- The two ways an instance chooses were tried apart: a trial build with the recorded calls switched off (not
  committed) ran the generic struct, the generic impls and the one-bound functions of the tests by the environment
  alone, and failed the defaults (N0222: no environment names `Self`, hence the default's own interface above) and
  `both<S: PageSource, K: PageSink>` with `S = K` (which only the recorded call can tell apart). In the tests here
  each generic version is checked before its instances (the order of `--dump-hir`, also with an instance named in a
  struct's field and in a function's signature before the generic struct is declared), so the recorded call answers
  first; no order in which it could not was found.
- Not done: `I.m(x)` for an impl whose parameters the interface's arguments choose (ADR 0165) finds the method
  only once the impl is made for the type (a bound, a `dyn` or a plain call made it), and is N0268 before.

## 5. Tests

- `run-pass/adr0229-same-name-methods.cee`: the finding's pager with different bodies for the two `usableSize`s,
  called through each bound, a `dyn`, the qualified form (`borrow`, `borrow mut` with `mut`, by value on an enum),
  and on the type for a name it has once; inherited defaults of one name from both interfaces, each calling the
  shared name through its own interface; two signatures of one name; a generic struct whose instance is made (by an
  earlier struct's field) before its declaration is read; two type parameters bound by the two interfaces and
  instantiated with the one type; both bounds on one parameter with qualified calls; a supertrait's method through
  the subinterface, by a bound and qualified; a static function of one name from two interfaces called through
  bounds. Native and interpreted; the expected output is worked by hand from the bodies.
- `check-fail/adr0229-same-name-methods.cee`: the ambiguous call on the type (N0267, without a fix, and with one where
  the arities differ), the ambiguous static function (N0267), `I.m(x)` on a type that does not implement `I` and on
  one that implements its supertrait only (N0268), a name `I` lacks (N0200), a static function through `I.f()`
  (N0233), no receiver (N0209), `mut` missing and misplaced (N0249, each with its fix), and, unchanged, an inherent
  method beside an interface's (N0228), one generic interface for two argument lists (N0228 and N0267), two bounds
  with the name (N0267, whose message now names the qualified form).
