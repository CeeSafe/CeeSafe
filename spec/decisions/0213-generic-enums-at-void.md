# ADR 0213 — A generic enum instantiated at `void`, and what else `void` reaches by inference

- Status: **Accepted** by the owner, 2026-11-23 ("commit. I accept ADR 0123", read as this ADR, 0213: it was the
  one proposed and asked about, and ADR 0123 was accepted on 2026-11-13), with §6's two questions taken as
  recommended; proposed the same day and implemented under the owner's instruction ("continue", the optimization and
  DX roadmap's Phase 3, item 7); not released.
- Scope: type-system.md §20.4; diagnostics.md N0226; `src/types/types.cee`, `src/check/structs.cee`,
  `src/check/patterns.cee`, `src/check/regions.cee`, `src/check/generics.cee`.

## 1. Context

ADR 0074 §2.1 gave `Result<void, E>` unit payloads, so that `Ok(v) => Ok(v)` means the same at `T = void` as at
any other `T`, and listed as not included "user generic enums instantiated at `void` (N0226)". Written, `Maybe<void>`
is N0226. **Inferred, it was not refused at all**, and neither was a struct:

```ceesafe
enum Maybe<T> { Just(T), Nothing }
fn wrap<T>(r: Result<T, i32>) -> Maybe<T> => match (r) { Ok(v) => Maybe<T>.Just(v), Err(_) => Maybe<T>.Nothing };
wrap(act())      // act() -> Result<void, i32>
```

With 0.94.1 this checks, runs in the interpreter, and the C build fails in the C compiler: `uint0_t tag;` for the
enum, and for a `Holder<T> { value: T }` reached the same way, "field has incomplete type 'void'". The checker's
`voidMisplaced` said "no other type holds a `void` from a parameter", which a user's generic type does.

## 2. Decision

### 2.1 A unit variant

In an instance of a generic enum, a variant whose **one** payload has the type `void` is a unit variant: it has no
payload slot, and is made and matched as ADR 0074's `Ok`:

- `Outcome<void>.Done()`, or `Outcome<T>.Done(e)` with any `void` `e`, evaluated for its effects (any other type:
  N0209);
- `Outcome.Done(v) => …` binds a unit local; `Outcome.Done()` and `Outcome.Done(_)` match it too.

The enum's other variants keep their payloads. The instance's layout, copy-ness and drop are those of what is left.

### 2.2 `void` may be written as a generic enum's type argument

`Outcome<void>` in a type and in an expression's type argument list. A generic function's or struct's explicit `void`
argument is no longer refused where it is written either: the instance is checked as an inferred one is (§2.3, and
ADR 0138 §7's rule for a function's signature).

### 2.3 What has no meaning is refused where the instance is used

N0226, at the use that makes the instance, written or inferred:

- a generic struct with a field whose type is `void` there: "'Holder<void>': the field 'value' would have the type
  'void'; …";
- a generic enum's variant that would have a `void` payload beside others: "'Two<void>': the variant 'Both' would
  have a 'void' payload beside others; …".

## 3. Options not taken

- **Refuse every user generic at `void`** (the smallest fix of the C failure). It keeps the two-adapter workaround
  ADR 0074 was written to remove, for the enum a program most often defines itself.
- **Unit positions in a variant with several payloads** (`Both(T, u32)` at `void` as `Both(u32)` with a unit first).
  Every pattern and construction would count positions two ways; no finding asks for it.
- **A unit field in a struct.** A struct literal, a field access and a struct pattern would each need the unit's
  erasure; ADR 0074 did not take it and nothing has asked since.

## 4. Consequences

- Programs that failed in the C compiler either compile (a unit variant) or are refused by `check` (the two cases of
  §2.3). None that built changes.
- `EnumVariant` has one more flag; nothing changes for an enum that is not an instance at `void`.

## 5. Implementation and tests (2026-11-23)

- `resolveEnumVariants` and the struct's field loop (src/check/structs.cee) see `void` from the environment;
  `EnumVariant.unit`; `isUnitPayload` (src/check/patterns.cee) answers for it, and `checkEnumValue` takes a `void`
  argument for a unit variant; `endInstance` (src/check/generics.cee) reports a struct's or enum's N0226 at the use.
- run-pass/generic-enum-at-void.cee: written and inferred instances, made with no value and with a `void` call,
  matched with `()`, a name and `_`, through `is`, in an array, beside the same generic code at `u32`; the C build and
  the interpreter. check-fail/generic-at-void-errors.cee: the struct, the two-payload variant, a value given to a
  unit variant. The results are in ROADMAP §10.

## 6. Questions for the owner (answered 2026-11-23: accepted as recommended)

1. Is "one payload only" the right limit (§2.1, §3's second option)? Yes, until a program needs more.
2. `Vector<T>`, `Box<T>` and `T?` at an inferred `void` are not looked at here. A follow-up that asks what each does
   today, as §1 did for user types. (Done 2026-11-23, below.)

## 7. The follow-up: built-in generics at an inferred `void` (2026-11-23)

Asked of 0.94.4 with this ADR's change, each in a generic function called with a `Result<void, i32>`:

| In the instance | `check` | The interpreter | The C build |
|---|---|---|---|
| `let o: T? = …` in the body | passed | ran, and `o.isSome()` was false for `Some(v)` | failed in the C compiler |
| `Vector<T>.create()` in the body, or `-> Vector<T>` | passed | ran | failed in the C compiler |
| `let a: [T; 2] = [x, x];` | the compiler stopped (an index out of bounds in its layout code) | | |
| a `Slice<T>` parameter | the compiler stopped, the same way | | |
| `-> T?` | N0226 at the call (ADR 0138 §7) | | |
| `Box.create(x)` | N0222 at the call | | |

**Decision.** There is no value of `void`, so in an instance a type parameter that is `void` cannot be part of
another type: an optional's value, an element, a referent, or a type argument other than a `Result`'s success type
and a generic enum's (§2). In the instance's body the type resolver refuses it where it resolves the part
(`Checker.typeDepth`, `resolveElementType`): N0222 at the call, as any error of an instance's body. In the instance's
signature it is refused at the call (N0226), by the check ADR 0138 §7 made for `T?` (`voidMisplaced`), now for
elements and referents too: not where the signature is declared, since a function of an instance that is never
called is no error (`ok` of a `Result<void, E>`'s methods returns `T?`, and `isOk()` on one must stay callable; a
first form that refused the declaration broke seven corpus programs).
check-fail/generic-at-void-builtins.cee and generic-at-void-signature.cee have the first three rows; the slice is
refused by the same code.
