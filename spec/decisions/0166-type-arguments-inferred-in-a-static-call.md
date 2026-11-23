# ADR 0166 — A generic type's arguments inferred in a static call

- Status: **Accepted** by the owner, 2026-11-18 ("I accept all the ADRs as written"): every question of the
  "Questions for the owner" section as recommended. Written at the owner's request ("go ahead and write those ADRs")
  from the findings of the HTTP server written in CeeSafe (`~/dev/CeeSafeApps/httpserver/CEESAFE_FINDINGS.md`,
  S15, second part). Implemented 2026-11-18 (§6); released in ceesafec 0.67.0.
- Scope: the checker's static calls (`src/check/generics.cee`, `staticOwner`; `src/check/inherent.cee`,
  `enumStaticOwner`; the call that follows them); type-system.md §20.2, diagnostics.md (N0265), the guide's generics
  page. No change to the grammar, to the backends, to a library or to the runtime.

## 1. Context

- **What is refused.** A static function of a generic struct or enum is called with the type's arguments written,
  whatever the call's own arguments say:

  ```ceesafe
  let words = Arc.create(AtomicU64.create(0))?;                    // N0265: 'Arc' is generic: write its type
                                                                   //        arguments, e.g. 'Arc<i32>.…'
  let words = Arc<AtomicU64>.create(AtomicU64.create(0))?;         // accepted
  let late = AsyncControl.checkpoint(HandlerError.Internal);       // N0265
  let late = AsyncControl<HandlerError>.checkpoint(HandlerError.Internal);
  ```

  `staticOwner` reports N0265 as soon as the type is named without arguments, before the call's arguments are
  looked at.
- **The same information is used elsewhere.**
  - A generic function's type arguments are inferred from its arguments' types (§20): `wrap(5u8)` is
    `wrap<u8>(5u8)`.
  - A struct literal's and an enum value's are inferred from their fields or payloads and the expected type
    (N0265's own text): `Pair { a: 1u8, b: 2u8 }`, `Maybe.Just(3)`.
  - `Box.create(v)` is a built-in that takes its argument's type (§18).
  So `Arc.create(v)` is the one place where a value's type is at hand and not used, and `Box.create(v)` beside it
  reads as if the rule were general.
- **What it costs.** A line. It is here because it is met on the first day (`Arc`, `Mutex`, `Channel`, a program's
  own `Cache<K, V>.of(pairs)`), the message does not say why this call differs from `wrap(v)`, and the written
  arguments repeat what the call's argument says, at length for a nested type:
  `Mutex<Vector<Arc<Kept>?>>.create(table)`.

## 2. Decision

### 2.1 The rule

**In a static call `C.f(a…)` where `C` is a generic struct or enum written without arguments, `C`'s type
parameters are inferred from the call's arguments, as a generic function's are.**

- `C.f` is taken as a function generic over `C`'s type parameters (and const parameters, ADR 0083), followed by
  `f`'s own if it has any (`x.m<T>(…)`, ADR 0064 §1). Its parameter types are the declaration's, with `C`'s
  parameters as the unknowns.
- Inference is the generic call's (§20): each argument is checked against its parameter type with the unknowns
  open, and the pairs found must determine every parameter of `C`. Bounds are checked on what was found.
- If they do, the call is `C<A…>.f(a…)`. If they do not, it is N0265 as today, with what could not be found named:
  "cannot infer 'K' of 'Cache': write 'Cache<…>.empty()'".
- `C<A…>.f(…)` with the arguments written is unchanged, and is how a call that determines nothing is written:
  `Vector<u8>.create()`.

### 2.2 What is not used

- **The expected type**, as for a generic function (N0265: "a call's expected type is not used"). So
  `let v: Vector<u8> = Vector.create();` stays N0265. Question 2 asks whether a static call should differ.
- **Regions.** They are erased and never written in a static call.
- **A partly written list.** `Cache<StringView>.of(pairs)` for a `Cache<K, V>` is N0204 as it is today (two type arguments, one given): all of
  the arguments or none.

### 2.3 Variants and constants

- An enum's value (`Maybe.Just(3)`) keeps its own rule, which uses the expected type as well.
- A member constant of a generic type (`Limits.max`) has no arguments to infer from: N0265 as today.

## 3. Consequences

- `Arc.create(v)`, `Mutex.create(v)`, `Channel.create(…)` where an argument names the element, and a program's own
  constructors read as `Box.create(v)` does. The explicit form is still there for a reader who wants the type on
  the line.
- **Nothing that compiles today changes meaning:** the rule applies only where N0265 was reported.
- **A type is named less often.** A reader finds a value's type from the constructor's argument, as with a generic
  function; the language server's hover shows the instance.
- **No new diagnostic code.** N0265's message for a static call names the parameter that was not found.

## 4. Questions for the owner

1. A static call of a generic type infers the type's arguments from the call's arguments (§2.1)? Recommended: yes.
2. Should it also use the expected type, so that `let v: Vector<u8> = Vector.create();` is accepted? Recommended:
   no, in this ADR: a generic function does not, and the two should change together or not at all. If the owner
   wants it, it is an ADR for both.
3. All of the arguments or none (§2.2)? Recommended: yes.

## 5. Implementation plan (once accepted)

1. Spec: §20.2 (the static owner without arguments), diagnostics.md (N0265's message), the guide's generics page.
2. `src/check/`: `staticOwner` and `enumStaticOwner` return "the declaration, arguments to be inferred" instead of
   an error when the name is followed by a call; the call collects inference pairs against the declaration's
   function with the type's parameters open (`typeArgumentsFor`, as `genericInstanceNamed` does for a function),
   resolves the instance (`resolveRegionItemType`), and goes on as the written form does.
3. Tests:
   - run-pass: `Arc`, `Mutex` and a program's own generic struct and enum, constructed both ways with the same
     result; a nested argument (`Mutex.create(Vector<u8>.create())`); a const parameter inferred from an array
     argument; a static function with a type parameter of its own;
   - check-fail: a call that determines nothing (`Vector.create()`), one that determines one of two parameters, an
     argument that does not fit any instance, an unmet bound on what was inferred, a member constant;
   - the contract suite; the corpus and the libraries build unchanged.
4. ROADMAP §10.

## 6. Implementation notes (2026-11-18)

- `inferredStaticOwner` and `inferredEnumOwner` (`src/check/generics.cee`) take the declaration's own function
  (the template's, whose parameter types mention the type's parameters as they are declared), pair each of its
  parameters with the call's argument, and call `typeArgumentsFor` with no written and no expected arguments, as
  `checkGenericCall` does. What it finds instantiates the struct or the enum (`instantiateStruct`,
  `instantiateEnum`, which check the bounds), and the call goes on as the written form does. They are tried in
  `src/check/expr.cee` only when the name has no arguments and the declaration is generic.
- **Where nothing is inferred.** A name with no such static function (`Cache.nothing(…)`), or one that is a method,
  is left to `staticOwner`, which reports N0265 "is generic: write its type arguments" as before. `Vector`, `Box`
  and the other built-in types keep their own rules (`Vector.create()` is N0260).
- **Messages.** A parameter not determined is the generic call's N0265: "cannot infer 'V' for 'Cache' here; write
  the type arguments: 'Cache<…>'". §2.1's wording ("write 'Cache<…>.empty()'") was not added.
- **A member constant** of a generic type was not touched.
- **Tests:** run-pass/adr0166-static-call-inference (`Arc`, `Mutex`, a struct with two parameters, one through a
  nested type, the written form beside it, an enum's own function) and check-fail/adr0166-static-call-inference
  (nothing determined, one of two, the expected type not used, a partly written list, no such function, an unmet
  bound on what was inferred). The corpus: 0 failures.
