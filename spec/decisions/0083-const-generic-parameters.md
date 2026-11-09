# ADR 0083 — Const generic parameters: `bool`, integer and enum values as generic arguments

- Status: **Accepted** by the owner, 2026-11-09 ("Accept all ADRs as recommended": every question for the owner answered as recommended), the day it was proposed; to ship in **0.26.0** (with ADR 0085 step 2 and ADR 0087 step 1), after ADR 0086
  (0.25.0), on which §2.3 relies. Raised by the Bun port's M0 (`~/dev/CeeSafeApps/bun/ROADMAP.md` §3, M0): Bun 1.4.4
  cannot be ported file by file while `fn pick<const TS: bool>(…)` is a syntax error. The evidence: counts measured in
  Bun's `src/` with two Python scripts run with `python3 -I` (§1.1), eight probe programs and one benchmark compiled
  by `out/ceesafec` 0.24.2 built from main (§1.2, §1.3), all kept in
  `~/dev/CeeSafeApps/bun/analysis/adr-evidence/0083/`, and the specification and ADRs cited below.
- Scope: type-system.md §20 (a new §20.5), §16.1 (array lengths), §10 (its last bullet); grammar.md `TypeParams` and
  "Generic arguments in expressions"; c-backend.md's "Generic code" row (C names); diagnostics.md (N0352–N0354); the
  parser, the checker (name resolution, generics, instantiation), the C backend's names (`src/backend/c_names.cee`),
  the interpreter; guide/generics.md, guide/coming-from/rust.md and zig.md.

## 1. Context

Bun began in Zig. Its `comptime` flags became Rust const generics in the rewrite: one parser source, compiled once
for TypeScript and once for JavaScript, with every `if TYPESCRIPT` folded away. The parser's state is
`pub struct P<'a, const TYPESCRIPT: bool, const SCAN_ONLY: bool>` (`src/js_parser/p.rs:242`), its methods sit in 30
`impl<…, const TS: bool, const SCAN: bool> P<'a, TS, SCAN>` blocks, and the entry point picks an instance from a
run-time option (`src/js_parser/parse/parse_entry.rs:462`):

```rust
if self.options.ts { self._parse::<true>() } else { self._parse::<false>() }
```

CeeSafe has type parameters only (type-system.md §20, ADR 0005), and `const` is a reserved word (grammar.md §1.3).

### 1.1 What Bun uses (measured 2026-11-09)

Method: `measure.py` and `measure2.py` (in `analysis/adr-evidence/0083/`) walk `upstream/src` with the same `SKIP`
set as `analysis/bun_rust_features.py`, so the file set is the 3,226 `.rs` files under `src/` outside `test`/`tests`
directories. They strip comments, and match a const parameter as `[<,]\s*const\s+NAME\s*:\s*TYPE`. The item kind is
the nearest preceding `fn|struct|enum|impl|trait|type` keyword. Body uses are regexes over the item's brace-balanced
body, so they are approximate (±10%). Comments account for the difference with a reviewer's recount over the same
files (1,456 parameters, 940 `bool`, 767 `const fn`): the same regex with comments kept gives 1,457, 949 and 799. The
roadmap's 1,607 came from a coarser regex that saw only a first parameter and folded `const fn` in; the split is below.

| Measure | Count |
|---|---|
| const generic parameters | **1,409**, in 1,156 items (947 with one, 169 with two, 40 with three or four) |
| by type | `bool` 902 (64%); `usize` 216, `u8` 191, `u32` 7, `i32` 2 (30%); enums 91 (6%: `NodeFSFunctionEnum` 15, `Lifetime` 12, `bake::Side` 11, `StringEncoding` 7, …; Bun enables `adt_const_params`) |
| by item | `fn` 600, `impl` 505, `struct` 242, `type` alias 50, `trait` 9, `enum` 2 |
| top crates | `runtime` 374, `lint` 191, `js_parser` 123, `sema` 103, `jsc` 90, `collections` 81, `paths` 63 |
| use in a `fn`'s body (600) | passed on to another const generic 244; a branch (`if`, `&&`, `match`, `==`) 205; not named (only passed through types) 143; arithmetic or `as` 19; an array length 3 |
| integer parameters used as an array length in their item | 32 (structs 13, impls 15: `BoundedArray<T, const BUFFER_CAPACITY: usize>`, 26 uses; `SmolList`, `ShortList`, `Grouped`) |
| defaults (`const X: bool = false`) | 15 |
| `const fn` (separate, §4) | 762; median body 3 lines, 382 of 723 bodies ≤ 3 lines, 153 with `if`/`match`, 45 with a loop |

How call sites supply the values (regexes over `::<…>` and `Name<…>`):

| Argument form | Count |
|---|---|
| `bool` literal in a turbofish (`::<true>`) | 799 (644 as the first argument); 189 more in type arguments (`P<'a, false, false>`) |
| integer literal in a turbofish | 313 |
| `{ Path::Variant }` or `{ path::CONST }` | 364 |
| `{ expression }` other than a path | about 30: `{ StmtTag::COUNT + 1 }` (`src/lint/runner.rs:712`), `{ cfg!(windows) }` 6, `{ Encoding::Utf16le as u8 }`, macro arguments |
| `{ !FLAG }` or `{ A + B }` over a const parameter (generic const expressions) | 0 (`generic_const_exprs` is enabled in 3 files, unused at these sites) |

So the values are literals, named constants, enum variants and the caller's own const parameters. Arithmetic on a
parameter inside a generic signature does not occur. Real examples:

- `src/http_jsc/websocket_client.rs:158`: `pub struct WebSocket<const SSL: bool> {`, with
  `impl<const SSL: bool> WebSocket<SSL> {` (`:227`) and `impl<const SSL: bool> Drop for WebSocket<SSL>` (`:1835`).
- `src/shell_parser/parse.rs:2198`: `pub struct Lexer<'bump, const ENCODING: StringEncoding> {`.
- `src/lint/runner.rs:22`: `pub(crate) struct Grouped<const ENDS: usize> { … starts: [u32; ENDS], …`.
- `src/install/lockfile.rs:1648`: `pub(crate) fn hoist<const METHOD: tree::BuilderMethod>(`, called as
  `(*lf).hoist::<{ tree::BuilderMethod::Filter }>(` (`src/install/prune.rs:856`).

### 1.2 What ceesafec 0.24.2 does (probes run 2026-11-09)

| Probe | Result |
|---|---|
| `fn pick<const TS: bool>(n: u32) -> u32`, `pick<true>(1)` (`analysis/probes/g1_const_generic.cee`) | N0100 "expected identifier, found 'const', which is a reserved word" |
| `const N: usize = 4;` then `let a: [u8; N] = [0u8; N];` | N0259 "an array length must be an integer literal" (§16.1; ADR 0074 §7 kept it so) |
| `const D: u32 = double(2);` | N0217 "constant expressions cannot contain calls" (§10) |
| an interface method without a receiver, `F.typescript()` in `pick<F: Flavor>` | N0267 "needs a receiver", N0200 "cannot find 'F'" (ADR 0086 §2.3 lifts this) |
| marker types: `struct Ts {}`, `impl Flavor for Ts { fn typescript(borrow this) -> bool => true; }`, `fn pick<F: Flavor>(f: borrow F, n: u32)` | accepted; C has `ceeG_4pick1S2Ts` and `ceeG_4pick1S2Js` |
| `struct Parser<F: Flavor> { flavor: F; … }`, and `struct Bare<F> { pos: usize; }` with `F` unused | accepted, runs |
| a plain run-time `ts: bool` parameter | accepted |

So today there are two ways around: a run-time parameter, or a type parameter bounded by an interface that answers
the flag (marker types). Nothing gives a `usize` value in a type.

### 1.3 What the flag costs at run time (measured 2026-11-09)

`b_runtime.cee` and `b_marker.cee`: one recursive scanner over a 10 MB buffer of pseudo-random bytes, run 100 times,
with three flag tests per byte; the flag is a `bool` parameter in the first and a marker type in the second (the
shape this ADR's instances compile to). Built with `ceesafec build` (clang 21, `-O2`, Apple arm64), three runs each:

| Version | User time (s) | Mean |
|---|---|---|
| run-time `bool` parameter | 3.45, 3.62, 3.50 | 3.52 |
| monomorphised (marker type) | 2.97, 2.97, 2.92 | 2.95 |

The run-time flag is **19% slower** on this branch-dense loop: clang does not clone a recursive function for a
constant argument at `-O2`, so every test stays. A real parser does more work per test, so its cost is smaller, but a
struct-level flag is worse: `P`'s methods would read `this.typescript` from memory at each test. The benchmark is a
micro-benchmark; it shows the direction and the order of size, not Bun's number.

## 2. Proposal

Const parameters of `bool`, integer and fieldless-enum types on functions, structs, enums, interfaces, interface
methods and impls, checked once and monomorphised like type parameters. Alternatives are in §3.

### 2.1 Syntax

```text
TypeParams = '<' TypeParam (',' TypeParam)* '>'
TypeParam  = 'region' Identifier
           | 'const' Identifier ':' Type                       (* ADR 0083: a const parameter *)
           | Identifier (':' Type ('+' Type)*)?
GenericArg = Type | ConstArg
ConstArg   = 'true' | 'false' | IntLiteral | '(' Expr ')'      (* a name or E.V parses as a Type; §2.4 *)
                                                               (* '(' Expr ')' only in a type position *)
```

- `const` stays a reserved word; it gains this one position. No new keyword.
- Region parameters come first (§25.1); const and type parameters may then come in any order, as in Bun
  (`fn to_json<W: fmt::Write, const STYLE: ToJsonStyle>`, `enum ShortList<T, const N: usize, L>`).
- In a type position (`let g: Grouped<(StmtTag.COUNT + 1)>`), `<` cannot be a comparison, so every `ConstArg` is
  allowed. Generic arguments in expressions (grammar.md) gain only `true` and `false` among their tokens, not
  `( … )`: `f(i < (k), j > (k))` compiles today and would otherwise read as the call `i<(k), j>(k)`. A computed
  argument in an expression (Bun's `::<{ StmtTag::COUNT + 1 }>`, about 30 sites) names a constant instead:
  `const ENDS: usize = StmtTag.COUNT + 1;`, then `counted<ENDS>(…)`.
- Adding `true`/`false` still changes how one shape parses: `f(i < k, true, j > (k))` compiles today, and becomes the
  call `i<k, true, j>(k)`, an error. No program changes meaning silently: the generic reading needs `i` to name a
  generic item and the comparison reading an ordered value, so the program stops compiling and the error points at
  it; grammar.md's existing advice applies, `(i < k)`. Before 0.26.0, the release scans the corpus, the compiler and
  `~/dev/CeeSafeApps` for `<` … `true|false` … `>` followed by `(`, `.` or `{` inside an argument list, as ADR 0085
  §2.6 does for its change, and records the result.

```ceesafe
enum Encoding { Ascii, Utf16 }

struct Grouped<const ENDS: usize> {
    starts: [u32; ENDS];
}

struct Parser<const TS: bool, const SCAN: bool> {
    pos: usize;
    fn step(borrow mut this) {
        if (TS) { this.pos += 2; } else { this.pos += 1; }
    }
}

enum Method { Filter, All }

fn hoist<const M: Method>(n: u32) -> u32 => match (M) { Method.Filter => n, Method.All => n + 1 };

fn run<const TS: bool>() -> usize {
    var p = Parser<TS, false> { pos: 0 };
    p.step();
    return p.pos;
}

fn parse(ts: bool) -> usize {
    if (ts) { return run<true>(); }
    return run<false>();
}
```

### 2.2 Parameter types

A const parameter's type is `bool`, an integer type (`i8`–`i64`, `u8`–`u64`, `isize`, `usize`), or a fieldless enum
(§15.1) that the program or a dependency declares. Anything else (a float, `StringView`, a struct, an enum with
payloads, an imported C enum, another parameter `T`) is **N0352 const-parameter-type**. These are the types whose
values are compared with `==` exactly (ADR 0072), so two arguments name the same instance iff they are equal.

### 2.3 Inside the generic item (checked once, §20.3)

- A const parameter `N` is a value of its type, the same in every use: it may be read wherever a run-time value of
  that type may (operands, arguments, `if (TS)`, `match (M)`). It is not a place: assigning or borrowing mutably is
  N0211.
- A `usize` const parameter may be an **array length**: `[T; N]` and `[v; N]`. In the generic body, `[T; N]` is equal
  only to `[T; N]` with the same parameter; `a.length` is `N`.
- It may be passed on, bare, as a const argument: `run<TS>()`, `Parser<TS, true>`, `Grouped<ENDS>`. An expression over
  a parameter as an argument or an array length (`[u8; N + 1]`, `run<!TS>()`) is **N0353 const-argument** (§2.4): it
  would need type equality modulo arithmetic, which Bun does not use (§1.1) and §20.4's termination rule would lose.
- **Not a pattern.** A bare identifier in a pattern that names a const parameter in scope is N0358, whose help
  suggests a guard (`x if (x == TS) =>`, ADR 0085 §2.2) or an `if`. The arm is checked once with the value unknown,
  so it cannot count as a literal for coverage, and making it a binding would hide the parameter. ADR 0085 §2.6's
  rule ("a `const` in scope") follows this one: it looks up const parameters too and reports them so. A later ADR
  may make it a constant pattern; that only accepts more programs.
- The body is checked once with the value unknown, so **both branches of `if (TS)` are checked**, as for type
  parameters and as ADR 0080's `cfg` blocks. Code after `if (TS) { return 1; }` stays reachable.
- A closure in the body reads `TS` without capturing it: it is a constant of the instance, not a local, and adds no
  effect or loan.
- An interface may have const parameters (`interface Pools<const SSL: bool>`), as it may have type parameters, and
  so may an interface method, under ADR 0086 §2.3's rule for generic methods: instantiated per call through a bound
  or on a concrete type, and a method with a const parameter makes `dyn I` N0331, naming it, as a type parameter does.
  There is no reason to differ: a `dyn` table holds one function per method either way.
- A generic impl follows ADR 0054: `impl<const E: Encoding> CharIter for ShellCharIter<E> { … }`; the parameter
  appears in the target. An inherent impl names the struct's parameters in order (§21.1): `impl<const TS: bool, const
  SCAN: bool> Parser<TS, SCAN>`.
- **Interface constants** (ADR 0086 §2.4) are the other per-instance constant. The port maps each Rust form to its
  own: a const generic parameter (`<const SSL: bool>`) becomes a const parameter; a trait's associated constant
  (`const HAS_ON_DATA: bool;`, read as `H::HAS_ON_DATA`) becomes an interface constant, read as `H.HAS_ON_DATA`.
  In generic code `H.HAS_ON_DATA` is a value, not a constant (0086 §2.4), so it cannot be a const argument or an
  array length there (N0353); on a concrete type, `Ts.HAS_ON_DATA` is an associated constant and can be (§2.4).

### 2.4 Arguments

A const argument is evaluated at compile time to a value of the parameter's type (§10's rules: an overflow, or a value
out of the type's range, is N0217). It is one of:
- `true`, `false`, or an integer literal (typed by the parameter: `Grouped<3>` is `usize`);
- the name of a constant (§10, an associated constant `StmtTag.COUNT` included) or an enum variant `Method.Filter`;
- inside a generic item, one of its own const parameters, bare;
- in a type position only (§2.1), a parenthesised constant expression (§10) that names no const parameter:
  `let g: Grouped<(StmtTag.COUNT + 1)>`.

A name in argument position is resolved by the parameter's kind: a value for a const parameter, a type for a type
parameter. A type where a value is expected, or a value where a type is, is **N0354 generic-argument-kind**.

A run-time value is **N0353 const-argument**. Its message names the dispatch that Bun itself writes:

```text
error[N0353]: the argument for the const parameter 'TS' of 'run' must be a constant: 'ts' is a run-time value
  = help: choose the instance at run time: `if (ts) run<true>() else run<false>()` (type-system.md §20.5)
```

### 2.5 Inference

As §20.2: a const parameter is inferred only from argument **types** it appears in (`fn sum<const N: usize>(a:
[u8; N])` called with a `[u8; 8]`; `l: borrow Lexer<E>` with a `Lexer<Encoding.Ascii>`), and from an expected type
(`let g: Grouped<4> = Grouped { … }`). Never from a run-time value. Otherwise N0265, "write the generic arguments".

### 2.6 Instantiation and C names (§20.4)

- Each distinct argument list is an instance: a copy with the values substituted, checked again and compiled to C.
  `Parser<true, false>` and `Parser<false, false>` are different types. An instance that does not check is reported at
  its use (N0222, naming the values); in particular an array length that is 0 or too large is N0259 there.
- The 64-deep instantiation limit (N0266) applies; since arguments in generic bodies are bare parameters or
  constants, no chain can grow without bound.
- C names (c-backend.md "Generic code"): a value argument mangles as `L`, the type's code, the value in decimal (`n`
  before a negative one; `0`/`1` for `bool`; the variant's index for an enum), then `_`. `L` is no type code today
  (`appendMangle`, `src/backend/c_names.cee:92`), and `_` ends the number, so the codes stay prefix-free:
  `run<true>` is `ceeG_3run1Lbool1_`, `Grouped<5>` is `ceeT_7Grouped1Lusize5_`, `hoist<Method.Filter>` is
  `ceeG_5hoist1L` + `Method`'s code + `0_`. Members and generic methods follow
  (`src/backend/c_names.cee:467`).
- The rule only changes the mangled arguments, so it composes with ADR 0082 §2.3's package prefix (its stage B,
  `ceeG_<package>_<len><name><mangled arguments>`): `run<true>` instantiated in package `parser` is
  `ceeG_parser_3run1Lbool1_`.
- The C of an instance holds `if (1)` or `if (0)`, and the C compiler removes the dead side at `-O2`; `-g` builds
  (`-O0`) keep it, which is correct, since both sides were checked and compile in every instance.
- The interpreter runs instances as it runs type instances: it never sees a parameter.

### 2.7 Array lengths from named constants (amends §10 and ADR 0074)

§16.1 requires an integer literal, and ADR 0074 (§2.7, its implementation notes §7) kept named constants out. With a
`usize` parameter allowed in `[T; N]`, a `usize` constant must be allowed too, or `[u8; MAX_PATH]` stays refused while
`[u8; N]` is accepted. Proposed: an array length is an integer literal, a `usize` constant (top-level or associated,
`Oid.LENGTH`), or a `usize` const parameter in scope; the range rule of §16.1 applies to the value. This removes the
last bullet of §10 ("Array lengths in types and repeats stay integer literals").

### 2.8 Relation to earlier decisions

- **Extends** ADR 0005 and type-system.md §20: a second parameter kind, under the same "checked once, then
  monomorphised" model; a new §20.5 states §2.2–§2.6.
- **Amends** §16.1, §10 and ADR 0074 §2.7 (§2.7); grammar.md's `TypeParams` and generic-argument rule.
- **Builds on** ADR 0086 §2.3 (generic interface methods and N0331) and §2.4 (interface constants), and sets the
  rule ADR 0085 §2.6 follows for const parameters in patterns (§2.3).
- **Unaffected:** ADR 0043 and ADR 0080's `@cfg`, which is about a C build's macros, not CeeSafe values; the two
  compose (`if (cfg("X"))` inside a generic body).
- **Updates the guide:** guide/coming-from/zig.md ("Generics, not `comptime`") gains `comptime` flags →
  const parameters; guide/coming-from/rust.md's list of what is missing loses const generics.

## 3. Alternatives considered

| | A. Const parameters: `bool`, integers, fieldless enums (proposed) | B. `bool` only, or an attribute | C. A translation rule only | D. Marker types (today; cheaper with ADR 0086) |
|---|---|---|---|---|
| What | `<const N: T>` on functions, structs, enums, interfaces, their methods and impls; monomorphised like type parameters | `<const TS: bool>`; or `@specialise` on a `bool` parameter, cloned per literal at call sites | Bun's const parameters become run-time parameters or fields; the C compiler folds what it can | each flag value is a struct implementing an interface; with ADR 0086, `impl Flavor for Ts { const TS: bool = true; }` read as `F.TS`, no value passed |
| Bun sites covered | 1,409 (aliases written out, §4) | 902 (64%); the attribute form covers functions only (600) | all, by rewriting | `bool` and enum, with noise; integers in array lengths: none |
| Speed | as Rust | as Rust for what it covers | 19% slower in §1.3; fields cost more | as Rust |
| Mechanical port | yes | no for 507 sites | no: signatures and struct layouts change | no: every flag becomes 2–3 items |
| Language cost | a parameter kind, one argument rule, value codes in C names | smaller, but a second mechanism later | none | none beyond ADR 0086 |

**A is proposed.** It is Bun's own construct, kept to the values Bun uses, and it reuses §20's model: checked once,
then one copy per distinct argument. B leaves a third of the uses (integers, enums, `BoundedArray`) to a later ADR.
C is free but slower, and changes every signature, which breaks the method of the port (port file by file, same data
structures). D works today and is as fast. ADR 0086 makes it cheaper: a static interface function or an interface
constant (`F.TS`) answers the flag without a value passed down 30 impl blocks. It still turns `_parse::<true>()` into
a marker struct, an interface and two impls, gives `Parser<Ts, Js>` where Bun has `P<true, false>`, and no interface
constant reaches an array length in generic code (0086 §2.4), so `BoundedArray<T, N>` stays out.

## 4. Not in this ADR

- **`const fn`** (762 in Bun). CeeSafe evaluates constants, not functions (§10, N0217; guide/coming-from/zig.md). As a
  translation rule until a separate ADR: a `const fn` is an ordinary `fn`. The open question is constants computed
  by calls: about 3,900 `const`/`static` items in Bun start their initialiser with a call (a loose regex, an upper
  bound: it counts statics built by constructors too). That ADR should follow M1's error queue, which will give the
  real number.
- **Defaults** (`<const NODE_BUILTINS: bool = false>`, 15 sites): CeeSafe has no default type arguments either.
- **Type aliases** (`type Socket<const SSL: bool> = …`, 50 sites): CeeSafe has none; the port writes the target out.
- **Generic const expressions** (`[u8; N + 1]`, `run<!TS>()`): refused (§2.3); none at Bun's sites.
- **Parenthesised expressions as generic arguments in expressions** (§2.1): a named constant instead.
- **Other value types** (`StringView`, `char`, floats, structs, imported C enums), and const parameters as constant
  patterns (refused for now, §2.3).
- **An interface constant defined from a const parameter** in a generic impl (`impl<const SSL: bool> H for S<SSL> {
  const IS_SSL: bool = SSL; }`): ADR 0086 §2.4 refuses constants in generic impls (N0216); this ADR keeps that.
- **Specialisation** by value (`impl I for S<true>` beside `impl<const B: bool> I for S<B>`): the overlap rule of
  §21.1 refuses it, as for types.

## 5. Testing

Run (2026-11-09, `out/ceesafec` 0.24.2): the seven probes of §1.2 and the two benchmark programs of §1.3, with the
results given there; the parse probes of §2.1 (`f(i < (k), j > (k))` prints `f`; `f(i < k, true, j > (k))` compiles;
`g(i < k, j > (k))` already reads as a generic call, N0209/N0210). Bun's counts by `python3 -I measure.py upstream`
and `measure2.py`, §1.1. All in `~/dev/CeeSafeApps/bun/analysis/adr-evidence/0083/`.

Proposed, for the implementation:
- **run-pass:** a `bool` flag function and its run-time dispatch; `Parser<TS, SCAN>` with an inherent impl and four
  instances; a fieldless-enum parameter with `match`; `Grouped<N>` with `[u32; N]`, `a.length` and inference from
  an argument `[u8; N]`; a named-constant array length (§2.7); a generic impl over `<const E: Encoding>` and a
  `dyn` made from an instance; a closure reading `TS`; an async generic function instantiated twice; an interface
  with a const parameter; an interface method with a const parameter called through a bound; `f(i < (k), j > (k))`
  still a comparison. Each runs under `--interp` and through C at `-O0` and `-O2` (spec/testing.md).
- **check-fail:** N0352, N0353 (a run-time argument, with its help; `H.HAS_ON_DATA` as an argument in generic code),
  N0358 (a const parameter in a pattern, ADR 0085's code), N0354, an out-of-range argument (N0217), `[u8; N + 1]`, an instance with `N = 0`
  (N0259 via N0222), `dyn I` where a method of `I` has a const parameter (N0331), a mismatched `[T; N]`/`[T; M]`
  (N0205), `f(i < k, true, j > (k))` now a generic call.
- **Snapshots:** the HIR of an instance; the C names of §2.6, with and without ADR 0082's package prefix (a test that
  two instances differing only by value, and a value next to a type argument, get distinct names).
- **Benchmark:** the §1.3 scanner in `benchmarks/` with a const parameter, against the run-time flag and Rust's
  const-generic version, in guide/performance.md.
- **Bun:** `src/js_parser/p.rs`'s `P` and `src/bun_core/bounded_array.rs` port mechanically.

## 6. Questions for the owner

1. Adopt option A: const parameters of `bool`, integer and fieldless-enum types on functions, structs, enums,
   interfaces, interface methods and impls, checked once and monomorphised (§2.1–§2.6), in 0.26.0? Recommended:
   **yes**; B and D leave integers and array lengths out, and C costs 19% on §1.3's loop and changes every signature.
2. Allow `usize` constants as array lengths (§2.7), amending §10, §16.1 and ADR 0074? Recommended: **yes**; otherwise
   a parameter is accepted where a named constant is not.
3. Refuse expressions over a const parameter in arguments and array lengths (§2.3), keeping parenthesised constant
   expressions in type positions only (§2.1)? Recommended: **yes**; Bun has none, type equality stays syntactic, and
   expressions keep today's parse except for `true`/`false`, which the release scans for.
4. Refuse a const parameter in a pattern (N0358) until a later ADR, with ADR 0085 §2.6 following? Recommended:
   **yes**; it keeps a typo-like catch-all binding out, and lifting it later breaks nothing.
5. Leave `const fn` to a separate ADR, raised once M1's error queue counts constants computed by calls, and port
   `const fn` as `fn` until then? Recommended: **yes**.
6. Use `L<type><value>_` for value arguments in C names (§2.6)? Recommended: **yes**; it is prefix-free with today's
   codes, composes with ADR 0082's package prefix, and is readable in a debugger.
