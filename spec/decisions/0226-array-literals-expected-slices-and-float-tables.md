# ADR 0226 — Array literals: element types from an expected slice, tables of floats, returned tables

- Status: **Proposed** 2026-11-24, under the owner's instruction to fix the type-checker findings of the SQLite port
  (`CeeSafeApps/sqlite/CEESAFE_FINDINGS.md` findings 34, 16 and 17). Implemented 2026-11-24 on
  `fix/findings-types`.
- Scope: type-system.md §10 (constant arrays), §16.2 (array values), §25.6 (N0245's message); the C backend's static
  initialisers (`appendStaticFloat`, src/backend/c_skeleton.cee).

## 1. Context

- **Finding 34.** `let a = [0, 2, 3, 5]; return a[i] + y;` with `y: i64` is N0205: the literal's integers are `i32`.
  So is `sum([1, 2][..])` for a `Slice<u64>` parameter: an expected slice gave its literal no element type (only an
  expected `[T; N]` did), and `sum([1, 2])` said only "expected 'Slice<u64>', found '[i32; 2]'".
- **Finding 16.** A `const` array of `f64` was N0216; a plain struct with float fields was already valid (ADR 0180).
  Trying it showed a second defect: a `static` of `f64` (an array or a scalar) passed the checker and failed in the C
  compiler, since its initialiser was `cee_f64_from_bits(…)`, a function call, which C does not allow there.
- **Finding 17.** `fn names() -> Slice<StringView> => ["a", "b"][..];` is refused: the literal is a temporary.

## 2. Decision

### 2.1 An expected slice gives an array literal its element type

An array literal's expected element type is that of an expected `[T; N]`, as before, or of an expected `Slice<T>`:
where a slice of the literal is expected (`[1, 2][..]` returned as, or passed for, a `Slice<u64>`). A literal indexed
where a number type `T` is expected has `T` elements (`let g: u64 = [7, 8][i];`). An array passed for a slice
parameter without `[..]` stays N0205, now with a sure fix that writes `[..]`: an array is lent as a slice by slicing
it (§16.3), and an implicit array-to-slice coercion is a separate question that this ADR does not open.

### 2.2 No inference from later uses

`let a = [1, 2];` stays `[i32; 2]` however `a` is used later, exactly as `let x = 1;` stays `i32` (§4, §5): typing a
local from its later uses is inference the checker does nowhere else (the one exception, a `for` range's bounds,
ADR 0063, looks only at its own body). Instead the N0205 on an element of an `i32` array read where another number is
expected says to give the type where the array is declared (`let a: [i64; 4] = …`) or a suffix to its first element
(`[0i64, 2, 3, 5]`), which types the rest (§16.2).

### 2.3 Tables of floats and `CChar`

A constant array's element may be a float type or `CChar`, as a plain struct's field may (ADR 0180); arrays of arrays
stay N0216. A float in a static's initialiser is emitted as a C constant expression: a finite value as a hexadecimal
floating literal of its exact bits (`0x1.8000000000000p0`, `0x0.0000000000001p-1022` for the least subnormal), which C
converts exactly, so the value still never depends on the C compiler's decimal conversion (§35.6); an infinity as
`(1e300 * 1e300)`, signed; a NaN as `(0.0 / 0.0)`, a NaN whose sign and payload the C compiler chooses (a NaN in a
static is still a NaN; its bits are not kept). A `const`'s floats are folded into each use as before, by their bits.

The test of `-0.0` in a static found a defect of the constant folder, fixed here: it negated a float as `0.0 - f`,
which gives `+0.0` for `0.0`, so `const Z: f64 = -0.0;` (and the same in a static) was `+0.0`, unlike `-0.0` in a
body. Negation now flips the sign, as IEEE negation and the run time do.

### 2.4 Returned tables: a `static`, and messages that say so

A slice of an array literal stays a slice of a temporary, constants or not: N0244 when it is returned. Promoting a
literal of constants to an anonymous static was considered (§3) and not done. Instead the messages name the way that
works: N0245 for a function with no scoped parameter that returns a slice, and N0244 for a returned slice of a
temporary, say `static NAMES: [StringView; 2] = ["a", "b"];` and `fn names() -> Slice<static, StringView> =>
NAMES[..];`.

## 3. Options not taken

- **Inferring a local's array type from its first use** (finding 34's request). It would make arrays differ from
  every other literal-initialised local, and it reorders checking (the declaration's type is known only after a later
  statement). §2.2's message names the two one-token fixes.
- **Promoting constant literals to statics** (Rust's rvalue static promotion). Statics are named items, declared
  before bodies are checked and folded in their own phase (src/check/statics.cee); each function reaches one through
  a hidden local holding its address. An anonymous static made from inside a body needs a new kind of static entry
  with no item behind it, and rules for which literals qualify (no drop, no interior mutability, every element a
  constant). It is left for when a second use appears; the `static` that works costs one line.

## 4. Implementation and tests (2026-11-24)

- `expectedElement`, `synthSliceExpecting`, `synthIndexExpecting` (src/check/arrays.cee), `writtenArrayLiteral` and
  the `[..]` fix in `checkArgument` (src/check/references.cee), `readsI32Element` and `appendLiteralElementHint`
  (src/check/expr.cee); `constElementOk` (src/check/declare.cee); `appendStaticFloat` (src/backend/c_skeleton.cee);
  the N0245 and N0244 messages (src/check/references.cee, src/nir/borrow.cee).
- `run-pass/adr0226-array-literals.cee`: literals for `Slice<u64>` with values beyond `i32`, a `static` table returned
  as `Slice<static, u64>`, a `const` of `f64`, statics of `f64` (−0.0, the least subnormal, 1e300 compared with the
  same literals) and `f32`; expected values worked by hand. `check-fail/adr0226-array-literals.cee` (N0205 ×3, N0245,
  N0216 for an array of arrays) and `check-fail/adr0226-returned-literal.cee` (N0244).
