# ADR 0072 — Optionals and equality: `??`, `isSome`/`isNull`, `==` on fieldless enums, slices and arrays

- Status: **Accepted** by the owner, 2026-11-05 ("Accept ADRs": §6 as proposed: `??` one token, no `valueOr`,
  equality on integers, `bool` and fieldless enums, one release as 0.16.0). Implemented with the changes listed in
  §7. Raised by the library work on 0.14–0.15 (sha256, tls, Postgres, mstore, mmime,
  msmtp, mdns, msieve: `CeeSafeLibs/NOTES-ceesafe-0.14.md` §2, §4, §5, §6, §8, §9), passed on by the owner.
- Scope: type-system.md §15.1 (enum values), §16.2 and §16.4 (arrays, `for`), §10 (constants), §22.1 (optionals),
  §9 (`let`); grammar.md §1 (the `??` token) and §3 (its precedence); the checker, NIR lowering, the C backend and the
  interpreter; guide/structs-and-enums.md, guide/basics.md, guide/unsafe-and-c.md; the corpus.

## 1. Context

The notes from eight libraries written on 0.14 and 0.15 repeat a handful of points. Each one costs a few lines, but
they come up on almost every page:

- **No fallback for an optional** (§2, §5, §6). `StringView.fromUtf8(b) ?? "fallback"` has to be a four-line
  `match`. A generic helper doesn't fix it: `fn orElse<T: Copy>(v: T?, d: T) -> T` cannot take a `StringView`
  (N0274: a scoped type is not a generic argument), so the libraries carry one helper per type (`textOr`,
  `numberOr`).
- **No test for presence as a method** (§6). `o == null` works (§22.1), but `o.isSome()` is N0234. Code coming
  from C#, TypeScript or Rust writes the method first.
- **No `==` on fieldless enums** (§5). `kind == RecipientKind.User` is N0207, so each enum gets an `isUser()` per
  variant, written with `match`. These enums are just tags.
- **No `==` on slices or arrays** (§7, §8). Tag parsers write `a.length == b.length && a.startsWith(b)`. A 16-byte
  digest comparison is a loop. `StringView == StringView` already compares bytes (§22.2), so the gap is surprising.
- **Three small checker gaps** (§8, §9):
  - `for (i in 0..25) { appendNumber(mut key, i); }`, with `appendNumber(…, n: usize)`, is N0205. Both bounds are
    flexible literals and the body doesn't index with `i`, so ADR 0063's rule types `i` as `i32`. The notes met
    this twice in one slice. (`for (i in 0..list.count())`, the first §9 example, already works: the non-literal
    bound fixes the type. Checked on 0.15.1, 2026-11-05.)
  - `TABLE.length` for a `const TABLE: [u32; N]` is N0216. The fix is a second constant for the length, or a copy
    of a 10,406-entry table into a local.
  - `let _ = f();` is N0100 ("expected identifier"). A value that must be bound but isn't needed takes a name.
- **Three things the guide doesn't say** (§3, §6, §8):
  - an `extern "C"` function without `@effects` counts as possibly sleeping, so calling it from async code is
    N0295;
  - the reserved words (`move`, `own`, …) are listed in grammar.md §1.3, but the guide doesn't link to it;
  - `Console.write` panics on bytes that aren't UTF-8, and `Console.writeBytes` is the answer.

## 2. Proposal

### 2.1 `a ?? b`, the fallback (§22.1)

```ceesafe
let name = StringView.fromUtf8(bytes) ?? "(not UTF-8)";
let port = config.port ?? 5432u16;
let first = cache.find(key) ?? store.find(key) ?? fail("missing");
```

- **Types.** `a` is a `T?`. `b` is checked against `T`, and the whole is a `T`. If `b` is itself a `T?`, the whole
  is a `T?`, so a chain can end in an optional. A non-optional `a` is N0262, the code `?` and `!` already use for
  that mistake.
- **Evaluation.** `a` first. `b` is evaluated only when `a` is `null`. It short-circuits, as `||` does, so
  `?? fail("…")` (a call of type *never*) is a way to stop with a message.
- **Ownership.** `a ?? b` consumes `a`, as `match (a)` does: for a non-Copy `T` its payload moves out. Optionals of
  Copy types, scoped ones like `StringView?` included, are copied. This is the case in all the libraries'
  examples.
- **Precedence.** `??` is right-associative (`a ?? b ?? c` is `a ?? (b ?? c)`). It binds less tightly than every
  other binary operator, `||` included, as in C#. Writing `a ?? b || c` needs parentheses, so the parser asks for them
  (N0100) whenever `??` and `||` or `&&` meet without them.
- **Token.** `??` becomes one token. In a type, `T??` still means an optional of an optional: the type parser reads
  the token as two `?`. In an expression, `x??` (propagate twice) must then be written `(x?)?`. No `.cee` file in
  this repository, in `CeeSafeLibs` or in `CeeSafeApps` writes it (searched 2026-11-05).
- **Lowering.** The checker lowers it to the `match` it replaces, so NIR, the C backend and the interpreter need
  nothing new.

### 2.2 `o.isSome()` and `o.isNull()` (§22.1)

Two methods on every `T?`, both of type `bool`. Like `o == null`, they only read `o`, so a non-Copy optional is
not consumed. `isNull` is `o == null`; `isSome` is `o != null`. A method of another name on an optional is still
N0234, now with a hint naming these two, `??` and `match`.

No `valueOr(d)` (§3).

### 2.3 `==` and `!=` on fieldless enums (§15.1)

```ceesafe
enum RecipientKind { User, Alias, List }
if (kind == RecipientKind.User) { … }
```

- Defined when both operands have the same enum type and no variant of that enum has a payload. It compares the
  variant. Each operand is only read.
- An enum with any payload is still N0207, with a hint to use `match`. Its equality would have to compare payloads,
  which is an equality interface's job (§4).
- `<`, `>` and the other orderings stay undefined (N0207): the declaration order of variants isn't an ordering.
- `Result` and optionals are enums with payloads, so nothing changes for them (`o == null` stays as it is).

### 2.4 `==` and `!=` on slices and arrays (§16.2, §22.2)

```ceesafe
if (tag == "BODY[]".bytes()) { … }
if (computed == expected) { … }       // [u8; 32] digests
```

- **Slices.** `a == b` with `a` and `b` both `Slice<T>` (a `SliceMut<T>` or a vector, lent as one, also counts) is
  true when the lengths are equal and the elements are equal in order. `T` must be an integer type, `bool`, or a
  fieldless enum (2.3). Floats are N0207: `NaN != NaN` would make a slice unequal to itself. Other element types are
  N0207 too: their equality is the interface's (§4).
- **Arrays.** `a == b` with `a` and `b` both `[T; N]`, same `T` and `N`, for the same element types. An array of
  another length is N0205, as for any type mismatch.
- **Other combinations.** `Slice<u8> == StringView` is N0205: write `s.bytes() == t` or `s == StringView.fromUtf8(t)`.
  Each operand is only read.
- **Cost and constant time.** The C backend compares integer, `bool` and fieldless-enum elements with
  `ceesafe_mem_equal` (the length check first, then `memcmp` under hosted profiles and a loop otherwise), and the
  interpreter compares in a loop. The result is not constant-time. The guide says so: a digest comparison against a
  secret wants a constant-time function, which belongs in a crypto package, not in `==`.

### 2.5 Three checker gaps

1. **Index ranges follow `usize` arguments** (§16.4, extending ADR 0063). When both bounds are flexible literals,
   the range is also `usize` if the body passes `i` directly as a call argument whose parameter is `usize`. The
   callee must be known before the body is checked:
   - a function or static function named directly (`appendNumber(mut key, i)`, `Table.at(i)`);
   - a method on a receiver whose type is already known: `this`, a parameter, or a local declared before the `for`
     (`list.host(i)`).

   Like ADR 0063's rule, this is syntactic and decided before the body is checked. A call whose callee can't be
   resolved then doesn't count, and `i` stays `i32` there, as today.
2. **`.length` of a constant array** (§10, §16.2). `TABLE.length` is the constant `N` for a `const` array. §16.2
   requires a place because evaluating the operand might have effects that the constant would skip. A constant has
   no effects, so the rule doesn't apply to it. Slicing a constant still needs a local or a `static` (§10).
3. **`let _ = e;`** (§9). `e` is evaluated and its value destroyed at the end of the statement. `_` binds nothing,
   so it can appear any number of times. It is the explicit way to discard a value, and nothing else changes.
   `var _` is N0100.

### 2.6 Guide additions (documentation only)

- **guide/unsafe-and-c.md**, beside the async rule: an `extern "C"` function without `@effects` is assumed to sleep;
  `@effects()` or `@effects(alloc)` on the declaration lets async code call it.
- **guide/basics.md**: a short "Reserved words" section that links to grammar.md §1.3 and calls out the ones that
  look like ordinary names (`move`, `own`, `type`, `namespace`, `macro`, `yield`). It also lists the built-in type
  names a program can't reuse (`Handle`, `Check`, and lib/core's names), pending the naming ADR (§4).
- **guide/builtins.md**, at `Console.write`: text must be UTF-8 (it panics otherwise); `Console.writeBytes` writes
  bytes as they are.

## 3. Alternatives considered

- **`o.valueOr(d)` instead of `??`.** A method evaluates `d` every time, so it can't be `?? fail("…")`, and it costs
  the default's construction on every call. Having both would be two ways to say the same thing. `??` is what C#
  and TypeScript programmers already write. **(Question 2.)**
- **A generic `orElse<T>` in lib/core.** This needs scoped generic arguments first (N0274, §4), and it still
  evaluates the default eagerly.
- **`==` through an equality interface now.** It's the general answer, covering structs, payload enums and any
  element type, but it needs derived implementations and a design for `Self` in interfaces (§21). 2.3 and 2.4 cover
  the cases the libraries hit. When the interface comes, it will give these types the same meaning, so nothing here
  has to change.
- **Defaulting a character literal to `u8` in `pushBE`.** This is already settled in 0.15.2: it is N0265, with a
  hint (type-system.md §16.6).

## 4. Not in this ADR

Each of these needs its own ADR. These are the notes' remaining items (groups D and E of the triage on
2026-11-05):

- multi-line or raw string literals, or `embed("file")` (§5; ADR 0064's finding A3);
- `match` arms that are blocks with a value (§7);
- temporaries that live to the end of their statement (`f(make().view())`, §8);
- two-phase borrows for free-function arguments (§9; ADR 0071 has them for `borrow mut this` methods and
  `VectorBytes`);
- `async move` children that move their captures out, and a `move` form of `Async.timeout` (§7);
- a method result that borrows only the struct's region, and region parameters on methods (§6);
- generics over scoped types and over `void` (§5, §9);
- built-in names (`Handle`, `Check`) moved into namespaces, and a local shadowing a dependency's name with a
  warning instead of N0315 (§3, §5, §8);
- an equality interface (§3 above).

## 5. Testing

- **The corpus:**
  - run-pass programs for `??` (short-circuit, chains, a non-Copy payload moved out, a `never` fallback),
    `isSome`/`isNull`, enum, slice and array equality, the new range rule, `const` `.length` and `let _`, each also
    in the interpreter;
  - check-fail programs for:
    - `??` on a non-optional;
    - `??` mixed with `||` without parentheses;
    - `==` on an enum with payloads;
    - `==` on float slices;
    - `Slice<u8> == StringView`;
    - arrays of different lengths;
    - `var _`.
- **Parsing:** parse-pass snapshots for `??` precedence and for `T??` in types; a parse-fail program for `x??` in an
  expression, with the `(x?)?` hint.
- **The notes' own examples:** each example in §1 compiles to what it says, as a corpus program.
- **The libraries:** after the release, CeeSafeLibs' helpers (`textOr`, the `isUser` family, the digest loops) are
  replaced and their tests rerun. This is a check of the design, not a gate for the release.

## 6. Questions for the owner

1. **`??` as one token.** It makes the expression `x??` (propagate twice) a parse error that needs `(x?)?`. No code
   in the three repositories writes it. **Proposed: yes.**
2. **`valueOr(d)` as well as `??`?** **Proposed: no**, one way (§3).
3. **Slice and array equality element types:** integers, `bool` and fieldless enums only, or `StringView` elements
   too (comparing their bytes)? **Proposed: the first set.** `StringView` elements can come with the equality
   interface.
4. **Release:** everything here as 0.16.0, after 0.15.2 (the bug fixes). **Proposed: yes.**

## 7. Implementation (2026-11-05)

### 7.1 Where

- **`??`:**
  - `src/syntax/token.cee` and `lexer.cee`: the token `QuestionQuestion`;
  - `src/syntax/parser.cee`: precedence 0, right-associative, the `(x?)?` hint and the parentheses rule (`startsOperand`,
    `requireParenthesesAround`), and `T??` in types read as two optionals;
  - `BinaryOp.Coalesce` in the AST, lowered by `synthCoalesce` (`src/check/options.cee`) to
    `{ let coalesced = a; if (coalesced == null) b else coalesced! }`, so NIR, the C backend and the interpreter are
    unchanged.
- **`isSome` and `isNull`:** `checkOptionalMethod` (`src/check/members.cee`), as `IsNull`, the HIR of `o == null`.
- **Equality:**
  - `src/check/equality.cee` (`checkEquality`): a vector operand is lent as its elements;
  - NIR compares fieldless enums' tags (`enumTag`, `src/nir/nir_builder.cee`);
  - the C backend calls `cee_mem_eq` (arrays) and `cee_slice_eq` (slices) from `rt.h`;
  - the interpreter compares the bytes.
- **The checker gaps:**
  - `passesAsUsize` (`src/check/range_index.cee`);
  - `isConstantArray` (`src/check/arrays.cee`);
  - `let _` parsed as a `let` named `_`, and checked as a discarded expression (`src/check/body.cee`).
- **Spec and guide:**
  - grammar.md §1.7, §2 and §2.1; type-system.md §3, §8, §10, §15.1, §16.2, §16.4 and §22.1; diagnostics.md
    (N0207, N0234, N0262);
  - guide/basics.md, guide/structs-and-enums.md, guide/builtins.md and guide/unsafe-and-c.md; templates/AGENTS.md.
- **Tests:**
  - run-pass `optional-coalesce`, `optional-methods`, `enum-slice-equality`, `checker-gaps-0072` and `coalesce-types`;
  - check-fail `optionals-equality-errors`; parse-fail `coalesce-syntax`.

  The expected outputs were checked by hand against the programs. The compiled programs and the interpreter agree.

### 7.2 Changes from the accepted text

1. **Arrays of different lengths** are N0207 ("requires operands of the same type"), the code every other operator
   uses for mismatched operand types, not N0205.
2. **`ceesafe_mem_equal` became `cee_mem_eq`,** a byte loop in `rt.h` like `ceesafe_str_eq`, not `memcmp`. It has no
   profile split, and C compilers recognise the loop.
3. **`let _: T = e`** is N0100, like `var _` (§2.5 only named `var _`).
