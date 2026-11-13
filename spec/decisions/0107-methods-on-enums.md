# ADR 0107 — Methods on enums

- Status: **Accepted** by the owner, 2026-11-12 ("We fix all gaps 1-6 first"), implemented on branch `impl/0107`;
  released in ceesafec 0.42.0. Raised while writing the language's book: a newcomer writes `shape.area()` for an enum and is
  refused twice.
- Scope: type-system.md §21.1 (`impl E { … }`), §15.1, §13.3 (`this = v`), §10 and §2; grammar.md §2 (the
  assignment target); diagnostics.md (N0100, N0228, N0267: wording, no new code); the checker
  (`src/check/inherent.cee` and the places that find a type's methods); two lines of the parser; the corpus and the
  guide. No change to the AST, the HIR, the backends or the runtime.

## 1. Context

A struct has methods, in its body and in `impl S { … }` blocks (ADR 0064 §1). An enum had none of its own:

```ceesafe
enum Shape { Circle(f64), Rect(f64, f64) }

impl Shape {                                  // 0.40.0: N0267, "an 'impl' without 'for' of an enum holds constants
    fn area(borrow this) -> f64 { … }         //         only, not functions (ADR 0074 §2.7)"
}
… shape.area() …                              // and N0234, "type 'Shape' has no methods"
```

and a `fn` in the enum's body is N0100. The program had to use a free function, `area(shape)`, or declare an
interface with one implementation to get the method.

**Why the restriction was there.** Nothing in the design asks for it:

- ADR 0064 §1 introduced `impl S { … }` for structs and implemented it by appending the block's functions to the
  struct's list in the AST before anything is declared (`mergeInherentImpls`, `src/check/inherent.cee`). An enum's
  AST node has no such list (`ItemKind.Enum(variants, typeParameters, regionParameters)`), so there was nothing to
  append to. The ADR does not discuss enums.
- ADR 0074 §2.7 then allowed constants in the block and "so enums have them too" (`impl Mode { const … }`); the
  N0267 message for functions dates from that change and states the gap, not a reason.
- An enum already has methods when an interface gives them: `impl I for E` declares its functions with the enum as
  their owner (`implementFor`, `src/check/interfaces.cee`), a generic enum's instances get them (ADR 0054), and
  `E.f(…)` and `Self.f(…)` reach an interface's static function (ADR 0086 §2.3). The checker's bodies, the
  borrow checker, both backends and the interpreter therefore handle a function owned by an enum today.

So the gap is the declaration, and the fix is to declare an enum's own functions through the table an interface's
already use.

## 2. Decision

### 2.1 `impl E { … }` gives an enum functions

```ceesafe
impl Shape {
    fn unit() -> Shape => Shape.Rect(1.0, 1.0);          // static: Shape.unit()

    fn area(borrow this) -> f64 => match (this) {         // shape.area()
        Shape.Circle(r) => 3.14159 * r * r,
        Shape.Rect(w, h) => w * h,
    };
}
```

An inherent impl whose target is an enum of the block's package holds functions as well as constants, with the rules
of a struct's block (§21.1):

- **Receivers.** `this` (a copy, or a move), `borrow this`, `borrow mut this`; without one the function is static.
  Calls are a struct's: `value.f(…)`, `E.f(…)`, `Self.f(…)`; a method called on the type is N0233.
- **Generic methods** (`fn pick<T>(this, …)`, ADR 0064 §1), on a method or a static function, inferred or written.
- **Visibility.** Each function is `public` or not, as a struct's: N0312 from another package for one that is not,
  N0314 for a public function of a public enum whose signature names a type that is not public.
- **Attributes.** `@effects`, `@effectsOf`, `@context`, `@constant`; `@placed` is a struct's (N0290), `@test` a free
  function's (N0348).
- **Several blocks**, in any file of the package, as for a struct.
- **One member of each name.** A function named as a variant, as a constant of the enum, as another of its
  functions, or as a method an `impl I for E` gives it (a default it inherits included) is N0228. A variant and a
  function of one name would make `E.Name(…)` ambiguous, so the rule covers methods too.

### 2.2 Generic enums and regions

Structs' blocks support both, so enums' do:

- `impl<T: B> E<T> { … }` for `enum E<T: B>`, `impl<region r> E<r> { … }` for `enum E<region r>`: the target names
  the enum with its own parameters, in order (N0267 otherwise, as for a struct). `impl E<i32> { … }` is not a way
  to give one instance a method.
- Every instance has the functions. They are checked once, with the parameters opaque, and declared again for each
  instance, as a generic struct's methods are.
- A static function of a generic enum is called with the type arguments, `Maybe<i32>.empty()`, or as `Self.empty()`
  inside the block. `Maybe.empty()` is N0265, as `S.f()` is for a generic struct: a variant's type arguments are
  inferred from its payloads and the expected type (§20.2), a function's owner is not.
- The enum's regions are in scope in the functions and flow from the receiver (§25.6), as a struct's do:
  `fn text(this) -> StringView<r>`.

### 2.3 `this = v`

A struct's `borrow mut this` method changes fields. An enum has none: to go from one variant to another the method
must replace the value. §13.3 already says that assigning to a `borrow mut` reference writes the referent, and the
checker's assignment already takes `this` as a place; only the parser refused it (N0104). So:

- `this = v` is an assignment target. In a `borrow mut this` method it replaces the receiver and
  drops the value it held. In a `borrow this` method it is N0211 ("behind a shared reference"), in a by-value method
  N0211 ("'this' is immutable in a by-value method").
- It is an assignment through a reference like any other: while a `match` binding of a payload is still used, it is
  N0240.
- It holds for a struct's method too (`this = Point { x: 0, y: 0 };`): one rule for `this`.
- Not in a `drop` block (N0211): there the receiver is the value being destroyed, and the assignment would destroy
  it again, without end. (0.40.0 already lets a drop block do this through a second reference,
  `let me: borrow mut S = this; me = S { … };`, which overflows the stack when it runs; that is an older hole, noted
  here and not closed by this ADR.)

A method that only changes a payload needs none of this: `Light.Blinking(times) => { times += 1; }`.

### 2.4 No functions in the enum's body

A struct's members end with `;` and its functions stand among them. An enum's body is a comma-separated list of
variants; functions there would need a second form of member in that list, a new field in the enum's AST node, which
some thirty matches in the compiler and the tools take apart, and a second place to look for an enum's functions.
`impl E { … }` already exists, already holds the enum's constants, and reads the same as for a struct. So the body
stays as it is, and a `fn` in it is still N0100, now with the advice: "an enum's body holds its variants; write its
functions in an 'impl' block".

### 2.5 The built-in enums

`Result`, optionals, `AllocError` and `IoError` take no `impl` block, and neither does a dependency's enum: the block
is written in the package that declares the enum (N0267, "names none"). That is the rule for structs, the libraries'
included (`impl Vector<T> { … }` in a program is N0267), so nothing became extensible that was not. `impl I for E`
keeps the orphan rule of ADR 0054.

### 2.6 Messages

No new code. Changed wording:

| Code | Before | Now |
|---|---|---|
| N0267 | "an 'impl' without 'for' of an enum holds constants only, not functions (ADR 0074 §2.7)" | gone: the block is accepted |
| N0267 | "an 'impl' without 'for' adds methods to a struct of this package; this names none (ADR 0064 §1)" | "… to a struct or enum of this package; this names none (ADR 0064 §1, ADR 0107)" |
| N0267 | | new text for an enum: "an 'impl' of an enum's methods names the enum with its own parameters, in order: 'impl<T> E<T>' for 'enum E<T>' (ADR 0107)" |
| N0234 | "type 'Shape' has no methods; 'perimeter' cannot be called on it" for a declared enum | "type 'Shape' has no method 'perimeter'" |
| N0100 | "expected '}', found 'fn'" in an enum's body | "an enum's body holds its variants; write its functions in an 'impl' block: 'impl Name { fn … }' (type-system.md §21.1)" |
| N0104 | `this = v` | accepted by the parser; N0211 where `this` is not `borrow mut` |

## 3. Implementation

- `mergeInherentImpls` lists an enum block's functions in `Checker.enumMethods` (the enum's item, the function, the
  block's file) after checking the target's parameters; the constants go where they went.
- `resolveEnumVariants`, which runs for the enum and for each instance, then calls `declareEnumMethods`: each
  function is declared with the enum's type as owner and the entry's type environment, and recorded in
  `Checker.implMethods`, the table of an interface's methods. `findMethod`, `checkTypeCall` and the `E.f(…)` branch
  of calls already read it, so method calls, `Self.f(…)` and `E.f(…)` needed no new lookup. `processImpls` runs
  before the members are resolved, so a clash with an interface's method is seen there.
- `enumStaticOwner` resolves `E<A>.f(…)` to the instance, as `staticOwner` does for a struct. `checkTypeCall`
  instantiates a generic static function of an enum, as `checkStaticCall` does a struct's.
- `regionScope` and `resolveFlows` give an enum's own functions the enum's regions and type parameters, as they give
  a struct's methods the struct's. An `impl I for E` method is left as it was: it names the impl's parameters.
- `checkEnumMethodVisible` (N0312) and the enum case of `checkPublicSignatures` (N0314) are the struct's checks for
  functions that `isEnumMethod`. An interface's methods are not subject to them.
- `processEnumMethodAttributes` reads the attributes once per declaration.
- The parser accepts `ExprNode.This` as an assignment target and reports a `fn` in an enum's body with the advice.
- Nothing in the HIR, NIR, the C backend or the interpreter changed: an enum-owned function is what an interface's
  method on an enum already was. The compiler's own emitted C differs only by the new code.

Not done, and the same for a struct's blocks today: `ceesafec doc` and the editor's outline list the functions of a
struct's body but not those of any `impl S { … }` block (`src/documentation/doc_generator.cee` and
`src/tooling/language_server.cee` have no case for `ItemKind.InherentImpl`; read, not run); an enum's are equally
absent. That is a tooling gap of ADR 0064, left for its own change.

## 4. Compatibility

- A program that compiled still compiles and means the same: the change accepts programs that were errors.
- `this = v` was N0104 everywhere, so no program contained it.
- One corpus expectation changed, deliberately: `check-fail/associated-constants.cee` expected N0267 for
  `impl Mode { fn port() -> u32 => 25; }`. That block is now accepted; the test keeps it and adds a constant named
  `port` beside it, which is N0228.
- The compiler stays within what the bootstrap compiler accepts: its sources use no enum methods and no `this = v`.

## 5. Tests

- `tests/corpus/run-pass/enum-methods.cee`: methods by value, `borrow this` and `borrow mut this`; a static
  function; `match (this)`; `Self`; two blocks for one enum; a constant beside functions; a generic method and a
  generic static function; a generic enum (`impl<T: Copy> Maybe<T>`: static functions, `Self`, `Maybe<T>.of`, a
  generic method); an enum with a region; an interface's method beside the enum's own; `this = v` dropping an owned
  payload, in an enum and in a struct. Compiled with `--check-leaks` and run in the interpreter.
- `tests/corpus/check-fail/enum-methods.cee`: N0228 (a variant's name, an interface's method's, another function's, a
  constant's), N0211 (`this = v` under `borrow this`, by value and in a drop block), N0267 (`impl Maybe<i32>`,
  `impl<U> Maybe<U>`, `impl Result<i32, i32>`, `impl AllocError`), N0234, N0233, N0265, N0204.
- `tests/corpus/check-fail/enum-method-assigns-borrowed-this.cee`: N0240.
- `tests/corpus/parse-fail/enum-body-function.cee`: N0100 for a `fn` in the body.
- `guide/structs-and-enums.md`, "Methods on an enum": a checked example.
- Run 2026-11-12, macOS arm64, on this branch:
  - `out/ceesafec fmt --check`: no file listed.
  - The compiler built by the previous one, then by itself twice: the second and third generations emit identical C
    (`build --emit c`, `diff -r`).
  - `python3 tools/run_corpus.py --compiler out/ceesafec --corpus tests/reference/tests/corpus`: 0 failures.
  - `python3 tools/guide_check.py --ceesafec out/ceesafec`: 158 examples (8 skipped): 0 problems.
  - By hand, in a copy of `tests/packages/visibility` with a public enum in the dependency: a public method, a public
    static function and a generic enum's are called from the dependent, compiled and interpreted; the ones that are
    not public are N0312 (a method, a static function, a generic instance's method); `impl geo.Shape { … }` in the
    dependent is N0267; a public method returning an internal struct is N0314.
- Not run here: `tools/contract_tests.py` (the integrator runs it), and no package test was added to
  `tools/package_tests.py` for the visibility cases above: proposed.

## Addendum: one error for a function in the body

A `fn` in an enum's body is N0100 with the advice to use an `impl` block, and the parser then passes over the rest
of the body to the enum's closing brace, so that no second error (N0101 at that brace) follows from the first
(`parse-fail/enum-body-function.cee`).
