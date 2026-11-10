# ADR 0087 — Macros: none in the language; translation rules, `@derive`, `debugAssert`, `c"…"`, `embed`

- Status: **Accepted** by the owner, 2026-11-09 ("Accept all ADRs as recommended": every question for the owner answered as recommended), the day it was proposed. Raised by the Bun port's M0 (`~/dev/CeeSafeApps/bun/ROADMAP.md` §3, M0, "Macro
  plan"). It rests on a census of Bun 1.4.4's hand-written Rust made for this ADR (§1.1), on the source of Bun's four
  proc-macro crates and its codegen scripts, and on probes compiled with ceesafec 0.24.2 (`out/ceesafec` from main)
  on 2026-11-09.
- Scope: grammar.md §1.6 (`c"…"`); type-system.md §6 (`==` on derived types), §8 (`debugAssert`), §10, §10.1 and
  §39.4 (constant `+`, `embed`), §16.2 (equality of elements), §24.3 and ADR 0075 §2.4 (derived `HashKey`,
  `OrderKey`), §28.1 (`@derive`), §32.1 (the write fact of §2.5); tooling.md (`--debug-assertions`, `[embed] roots`);
  the checker, NIR lowering, the C backend and the interpreter; lib/core (`CStr`, formatting helpers); the corpus and
  the guide.

## 1. Context

CeeSafe has no macros: `macro` is reserved (grammar.md §1.3), and language-spec.md §6 lists macros as a deliberate
omission. Bun's Rust calls one every 38 lines.

### 1.1 Method and totals

`macro_census.py` (run with `python3 -I`; location in §5) walks `upstream/src` as `analysis/bun_rust_features.py` does,
skipping test directories and generated files. A small lexer drops comments and the contents of string and char
literals. It then counts `name!(`, `name![` and `name!{`, each `#[derive]` trait, each attribute path and each
`macro_rules!` name. `shapes.py` classifies arguments on raw text, so its counts are approximate.

| | This census | ROADMAP §1 (raw regex) |
|---|---|---|
| Invocations / distinct macros | 36,282 / 657 | 37,247 / 671 |
| `macro_rules!` definitions (names) | 697 (601) | 707 |

The roadmap's regex also matched comments. The formatter's IR defines its own `write!` and `format_args!`
(`src/format/ir/macros.rs:8`, `:22`); 1,315 calls in `src/format` are those. So about **13,040** invocations are of
Bun's own `macro_rules!`, and about **23,240** are of std, third-party crates and Bun's function-like proc macros.

### 1.2 Standard and third-party macros

| Macro | Uses | Notes |
|---|---|---|
| `matches!` | 8,125 | 3,761 with `\|`, 1,667 with an `if` guard (calls counted with balanced parentheses across lines, ADR 0085's `recount.py`; `shapes.py`, by line, gives 8,106 and 1,639) |
| `format_args!` (std) | ~2,940 | `global.throw(format_args!("Expected a connected socket"))` (`runtime/socket/socket_body.rs:5178`) |
| `debug_assert!`, `_eq!`, `_ne!` | 2,355 | 60 may have side effects (§1.6) |
| `assert_eq!`/`_ne!` / `assert!` | 1,520 / 929 | `assert_eq!` mostly in 83 test-heavy files |
| `vec!` | 1,496 | 384 are `vec![x; n]` |
| `write!` (std), `writeln!` | ~1,140, 175 | onto `Vec<u8>` and `fmt::Formatter` |
| `format!` | 807 | 708 use only `{}` or `{name}` |
| `unreachable!` / `panic!` | 749 / 307 | `todo!`, `unimplemented!`, `dbg!`: none |
| `cfg!` | 429 | `windows` 164, `debug_assertions` 148; also 5,082 `#[cfg]` attributes |
| `addr_of_mut!`, `addr_of!` / `offset_of!` | 343 / 157 | |
| `concat!`, `stringify!`, `concatcp!`, `formatcp!` | 169, 72, 61, 9 | compile-time text |
| `include_bytes!`, `include_str!` | 99, 17 | 26 files: `root_certs.der` (27,583 bytes), `typescript_libs.bin`, ESLint shims |
| `smallvec!`, `bitflags!`, `defer!`, `thread_local!` | 167, 86, 81, 56 | |
| `env!`, `asm!`, `compile_error!`, `line!`/`file!` | 15, 13, 11, 6 | |

Format specifiers other than `{}`/`{name}`, over all literals: `{:?}` 282, hex 172, `{:p}` 45, `{:.N}` 43, widths and
alignment about 120, `{:o}` 17.

### 1.3 Derives

16,258 trait names in 5,373 `#[derive]` lists: `Clone` 4,275 (474 without `Copy`), `Copy` 3,802, `PartialEq` 2,082,
`Eq` 1,977, `Debug` 1,543, `Default` 1,443 (232 `#[default]` variants), `Hash` 288, strum `IntoStaticStr` 234,
thiserror `Error` 134 (955 `#[error]`), `PartialOrd`/`Ord` 69 each, css_derive's six 157, Bun's ref-counting three 68,
`EnumTag` 13, `JsAffine` 11. Hand-written impls: `Default` 548, `Display` 210, `Clone` 106, `PartialEq` 90, `Debug` 71,
`Hash` 40. Typical: `#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)] pub struct Indent(usize);`
(`src/parsers/yaml.rs:243`). `Debug` is derived 1,543 times but formatted with `{:?}` only 282 times.

### 1.4 Attribute and proc macros

Four crates hold them (`src/jsc_macros`, `css_derive`, `bun_core_macros`, `clap_macros`; 4,067 lines):

- `#[bun_jsc::host_fn]`, with `(method|getter|setter)` or `(export = "Name")` (533 uses): it emits an `extern "C"`
  shim (`extern "sysv64"` on Windows x64) that turns JSC's raw pointers into references and calls
  `host_fn_result(__g, || name(__g, __f))` (`src/jsc_macros/lib.rs:81-102`). `#[JsClass]` (38), `#[uws_callback]`
  (45) and `#[host_call]` (19) do the same for classes, uWS and other ABIs.
- Bun generates the rest in TypeScript: `src/codegen/generate-host-exports.ts` scrapes `// HOST_EXPORT(Name)` comment
  markers and writes every thunk into one file (its lines 1-9); 12 of the 27 scripts in `src/codegen` emit Rust.
- Function-like: `pretty_fmt!` (183), `comptime_string_map!` (102, `src/options_types/bundle_enums.rs:59`), clap's
  `param!`/`parse_param!` (625).

### 1.5 Bun's own `macro_rules!`, top 20

| Macro | Uses | Defined in | What it does | Treatment (§2.6) |
|---|---|---|---|---|
| `t!` | 1,201 | `http_types/MimeType.rs:17` | MIME literal to table entry | generate (table) |
| `write!` (formatter IR) | 1,147 | `format/ir/macros.rs:22` | `Format::fmt(&args, f)` | expand, then port |
| `scoped_log!` | 857 | `bun_core/output.rs:1499` | debug-only scoped log | library + rule |
| `pretty_errorln!` | 451 | `bun_core/output.rs:2166` | `<red>…<r>` markup to ANSI, stderr | library + generate |
| `pretty!` | 396 | `bun_core/output.rs:2118` | the same, stdout | library + generate |
| `param!` | 394 | `clap/lib.rs:32` | parses a help line at compile time | generate (table) |
| `no_state!` | 314 | `lint/rule.rs:737` | `impl` lines of a stateless rule | default methods (ADR 0086) |
| `log!` | 307 | per file | forwards to `scoped_log!` | library + rule |
| `parse_param!` | 231 | `clap/lib.rs` | as `param!` | generate (table) |
| `cat!` | 218 | `sema/check/mod.rs:8` | `[&a[..], &b[..]].concat()` | expand, then port |
| `zstr!` | 217 | `bun_core/util.rs:562` | NUL-terminated literal | `c"…"` (§2.4) |
| `debug!` | 203 | `bun_core/output.rs` | debug-only log | library + rule |
| `prettyln!` | 201 | `bun_core/output.rs:2129` | `pretty!` and a newline | library + generate |
| `declare_scope!` | 176 | `bun_core/output.rs:1474` | `pub static X: ScopedLogger` | rule (a `static`) |
| `format_args!` (formatter IR) | 168 | `format/ir/macros.rs:8` | tuple of `&dyn Format` | expand, then port |
| `w!` | 128 | `bun_core/string/immutable.rs:1354` | ASCII literal as `&[u16]` | generate (array) |
| `node_entry!` | 122 | `resolve_builtins/HardcodedModule.rs:353` | builtin-module table row | generate (table) |
| `of!` | 116 | `lint/estree/schema.rs:160` | `match` with early `return None` | expand, then port |
| `mark_binding!` | 114 | `bun_core/Global.rs:426` | debug trace of the caller's file | library + rule |
| `pretty_fmt_const!` | 113 | `runtime/test_runner/pretty_format.rs` | markup to a constant | generate |

By purpose, over all 601 names: colour markup about 1,940 uses, logging about 1,700, tables and generated items about
2,400 (also `opaque_ffi!` 105, `env_var::new!` 109, `property_helper!` 100), literals 345, `impl` boilerplate about
600, and local syntax sugar (`cat!`, `of!`, `col_ref!`, `bail!`, …) most of the rest. 17 are never invoked.

### 1.6 `debug_assert!` and side effects

Release builds drop a `debug_assert!` with its arguments, so an argument with an effect changes behaviour between
builds; this caused one of Bun's regressions after its rewrite (ROADMAP §3, M0). 60 of the 2,355 have arguments that
may write (assignment, `&mut`, `next`, `pop`, `try_lock`, `fetch_*`, `unsafe`). For example:

- `debug_assert!(unsafe { (*first).debug_mutex.try_lock() });` (`runtime/bake/dev_server/hot_reload_event.rs:294`,
  and three more sites) takes a lock that release builds never take;
- `debug_assert!(events.next().is_none());` (`install/PackageInstaller.rs:1478`; also `install/yarn.rs:573`)
  advances an iterator;
- `debug_assert_eq!(unsafe { us_nq_vtable_size() }, …)` (`lsquic_sys/lib.rs`) calls C.

### 1.7 CeeSafe today (probes, ceesafec 0.24.2)

| Probe | Result |
|---|---|
| `a == b` on a struct; `@derive(Eq)`; `let macro = 1;` | N0207; N0290 (unknown attribute); N0100 (reserved word) |
| `match (t) { Tok.Plus => true, Tok.Minus => true, _ => false }`; `Tok.Num(v) => v > 2` | runs: `true`, `true` |
| `assert(bump(mut n), "…")` | allowed; the write happens in every build (§8: "assertions are always checked") |
| `debugAssert(…)`; `embed("f")` | N0200 (unknown function) |
| `strlen("abc\0".ptr as *CChar)` | 3, interpreted and native; `"hello\0".length` is 6 |
| `const MSG: StringView = PREFIX + "error";` | N0207 |
| `if (cfg("BUN_DEBUG")) {…} else {…}` in a hosted `main` | prints `off`; with `--cflag -DBUN_DEBUG=1`, `on` |

Prior decisions: ADR 0072 gave `==` to integers, `bool`, fieldless enums, slices and arrays, and deferred "an equality
interface" (§3, §4). `HashKey<K>` is ADR 0052, `OrderKey<K>` ADR 0075 §2.4, configured code ADR 0080 §3.3, `@test` ADR
0049. `embed("file")` was deferred twice (ADR 0072 §4; ADR 0074 §2.6 and §4) because checking would need file access in
the LSP and MCP. This set's ADR 0085 proposes `e is P`, `|`, ranges and guards; ADR 0086 proposes default methods and
`Self`; ADR 0084 proposes `Cell` and `RefCell`.

## 2. Proposal

### 2.1 No general macros

CeeSafe does not add macros, neither pattern macros nor procedural ones, and `macro` stays reserved. Why:

- **A reader sees what runs.** Bun's `pretty!` dispatches through `__pretty_dispatch!` with a pool of 32 identifiers
  (`bun_core/output.rs:2103-2116`). Port reviewers compare CeeSafe with Rust line by line, which works for expanded
  code and fails for a second language inside the first.
- **Diagnostics and tools stay exact.** Everything the checker reports, `fmt` formats and the LSP resolves is in the
  file.
- **The census doesn't need them.** About 20,000 invocations are standard forms with a fixed CeeSafe spelling (§2.2).
  Most of Bun's 13,000 are libraries, tables or boilerplate that the port generates (§2.6). The rest needs five fixed
  features, each specified once (§2.3–§2.5, §2.7, §2.8).
- **Generation stays in the open.** The compiler already does it this way: `tools/gen_*.py` write sources that say
  they are generated (AGENTS.md). Bun does too, with 12 codegen scripts.

### 2.2 Translation rules (the porting process; no compiler change)

These go into the port's `PORTING.md` (ROADMAP M1) and the reviewers' checklist.

| Rust | CeeSafe |
|---|---|
| `matches!(e, P)`; `matches!(e, P if g)` | from 0.26.0, `e is P`; `e is P && g` (ADR 0085 §2.8). In 0.25.0, `match (e) { P => true, _ => false }` with 0085's `\|` and ranges, and `P if (g) => true` for a guard. The 1,639 guarded calls (§1.2) need 0085's guards; before 0.25.0, one arm per alternative and the arm `P(x) => g` |
| `format!`, `write!`, `writeln!` | appends: `var s = String.create(); s.append("a ")?; s.appendNumber(x)?;` (`VectorBytes` for bytes); `writeln!` appends `"\n"` |
| `format_args!` passed to a function | the callee takes a `StringView` built by the caller in a local `String`; on a log path, inside the gate (§2.6) |
| `{:x}`, `{:08x}`, `{:.3}`, `{:>8}`; `{:?}` | §2.9's helpers; the generated `appendDebug` (§2.3) |
| `print!`, `println!`, `eprintln!` (43) | `Console.write…` |
| `vec![a, b]` / `vec![x; n]` | `Vector.withCapacity(2)?` and pushes / `withCapacity(n)?` then `resize(n, x)?` (§23.1) |
| `assert!(c, "m")`; with format arguments | `assert(c, "m")`; `if (!c) { var m = …; panic(m.view()); }` |
| `assert_eq!(a, b)` | in a `@test`, lib/std's `Check` (ADR 0049); elsewhere `assert(a == b, "a == b")` |
| `debug_assert!(c)` | `debugAssert(c)` (§2.5) |
| `unreachable!()`, `panic!("m")` | `panic("unreachable")`, `panic("m")` |
| `concat!`, `stringify!` | one literal, folded by the porting tool; with constants, `+` (§2.7) |
| `cfg!(x)`; `#[cfg(debug_assertions)]` code | `if (cfg("X")) { … }` (ADR 0080 §3.3); `debug_assertions` is `BUN_DEBUG`, defined by Bun's build |
| `offset_of!`; `addr_of!`, `addr_of_mut!` | `offsetOf<T>(f)` (§30.1); `borrow x as *T`, `borrow mut x as *mut T` (§27.2) |
| `defer!`; `include_bytes!`, `include_str!` | `defer` (§14.7); `embed` (§2.8) |
| `env!("X")`; `line!`, `file!` (6) | a constant in a `.cee` file Bun's build generates; written out |
| `quote!`, `compile_error!` | not ported (proc-macro crates and `#[cfg]` guards) |

A Rust type that is not `Copy` keeps its move semantics. The port adds `@noncopy` when every field is Copy and the
type has no `drop` block; otherwise §14.1 would make it Copy. `Clone` beside `Copy` (3,806) needs nothing.

### 2.3 `@derive(Equal, HashKey, OrderKey)`

These three are in the compiler because a hand-written or script-written version drifts silently: add a field and it
still compiles, but compares wrongly. A stale `Clone` or `Default` fails to compile, since a struct literal names every
field (§12), and a stale `Debug` is harmless. So those three stay generated source (table below).

`@derive(…)` on a struct or enum takes one or more of these names, and no others (N0290):

- **`Equal`** gives the type `==` and `!=`. The fields are compared in declaration order, joined by `&&`. An enum
  compares the variant, then its payloads. Each field's type must have `==`:
  - ADR 0072's types (integers, `bool`, fieldless enums, slices and arrays of them);
  - `StringView`, `String`, `Vector<u8>`;
  - another `Equal` type, and slices, arrays and vectors of one (this extends ADR 0072 §2.4);
  - `T?` of these (two `null`s are equal), `Box<T>` and `borrow T` (the referent), raw pointers (the address);
  - floats, compared as IEEE values (**Question 2**);
  - ADR 0084's `Cell<T>`, compared by `get()`, if 0084 gives `Cell<T: Copy>` `==` (Rust derives `PartialEq` through
    `Cell`); otherwise a `Cell` field is N0207 like any field without `==`. `RefCell` and `RawCell` fields are N0207.

  A field without `==` is N0207 at the attribute, naming the field. A generic type has `==` for the instances whose
  arguments have it, checked at the use; in generic code over an opaque `T` it is N0207. User code still cannot
  define `==` (language-spec.md §6: no operator overloading). Bun's 90 hand-written `PartialEq` impls become named
  methods, and `Eq` is dropped.
- **`HashKey`** writes `impl HashKey<S> for S` (§24.3), and needs `Equal` (N0290 otherwise). `hash` feeds each field in
  order: through its `HashKey`, or a fieldless enum's variant index as a `u32`. An enum's index comes before its
  payloads. `sameAs` is `==`. A field type with neither is N0268.
- **`OrderKey`** writes `impl OrderKey<S> for S`: `compareKey` compares the fields lexicographically, in declaration
  order. For enums it compares the variant index first. ADR 0072 §2.3 refused declaration order as a default
  ordering; here the author asks for it. There is still no `<`.

Each impl is ordinary checked code: `--dump-hir` shows it, and the guide prints the expansion for a two-field struct.

Generated source for the other derives, from the port's generator:

| Rust | Generated CeeSafe |
|---|---|
| `Clone` without `Copy` (474) | `public fn clone(borrow this) -> Result<S, AllocError>` |
| `Default` (1,443) | `public static fn default() -> S`; with ADR 0086, an interface may replace it later |
| `Debug` (1,543) | `appendDebug(borrow this, out: borrow mut String) -> Result<void, AllocError>`, in Rust's derived layout, only for types a `{:?}` or `assert_eq!` reaches |
| thiserror `Error` (134), hand-written `Display` (210) | `appendTo(borrow this, out: borrow mut String) -> Result<void, AllocError>` |
| strum `IntoStaticStr`, `EnumString`, `FromRepr` | `name() -> StringView`, `static fn parse(StringView) -> E?`, `static fn fromRepr(n) -> E?` |
| `bitflags!` (86) | a struct over the integer, constants, `contains`/`insert`/`remove` |
| css_derive, `bun_core_macros` derives | those crates ported as generators that emit CeeSafe |

### 2.4 `c"…"` and lib/core's `CStr`

```ceesafe
let fd = unsafe { open(c"/dev/null".ptr(), 0) };
```

- `c"…"` takes §1.6's escapes. Its type is **`CStr<static>`**: `CStr<region r>` is a Copy view of text with a NUL byte
  after its end, valid for region `r` like `StringView<r>` (§25). The compiler appends the NUL; a `\0` inside the
  literal is N0006.
- `CStr<r>` has `ptr() -> *CChar`, `view() -> StringView<r>` and `length()` (both without the NUL), and
  `CStr.fromBytesWithNul<region r>(b: Slice<r, u8>) -> CStr<r>?`, which requires one final NUL and no other. The view
  cannot outlive `b`: returning one made from a local `Vector`'s bytes is N0244, as for any view (probed on 0.24.2 with
  a library-level `CStr<region r>`). A `CStr` field in a struct needs a region parameter, as a `StringView` does.
- Allowed in constants and static initialisers (§10, §39.4).
- **Literal prefixes.** A single letter immediately before `"`, not part of a longer identifier, is a reserved literal
  prefix (N0100 unless defined). This ADR
  defines `c`. `b"…"` is not added: string patterns are typed by the scrutinee (ADR 0085 §2.7), and a byte-string
  expression would be its own ADR.
- Replaces `zstr!` (217) and Rust's `c"…"` (273 literals). `"…\0".ptr as *CChar` stays valid, but nothing checks that
  its `\0` is there, and its length counts the NUL (§1.7).

### 2.5 `debugAssert`

`debugAssert(condition)` and `debugAssert(condition, message)` are `void` built-ins (§8).

- **Checked in every build**, like any expression.
- **Evaluated only when debug assertions are on**: under `build -g` (unless `--no-debug-assertions`) or
  `--debug-assertions`, under `ceesafec test`, and in the interpreter. When off, nothing of the call is emitted.
  A failure panics as `assert` does.
- **No writes and no moves** (N0360). Since the call may vanish, the condition and message may change nothing that
  exists before the call, and the checker's view of ownership must hold in both builds:
  - no assignment, `mut` argument or `borrow mut this` call on a place not declared inside the condition;
  - no move or consumption of a place declared outside it (`debugAssert(consume(v))`): the checker would treat `v`
    as moved, but a release build never drops it, which leaks it or, for a `MutexGuard`, never unlocks;
  - no write through a raw pointer or to a `static mut`;
  - no write through a shared borrow: atomics' stores, swaps, fetches and compare-exchanges, lock acquisition,
    `StableVector.push`, ADR 0084's cells. This includes `RefCell.read()`, though its guard's drop restores the
    count: the fact is per call, not per acquire–release pair, and `read` can panic ("cell already borrowed") in
    debug builds only. `Cell.get()` is a load and is allowed. To assert through a `RefCell`, take the guard before
    the call, inside `if (cfg("BUN_DEBUG"))` if release builds should not pay for it;
  - no `sleep` or `suspend` effect (§32.1), so no I/O and no `await`; `alloc` is allowed;
  - no call of an `extern` function (even one declared `@effects()`) or a C function pointer, and no call through a
    closure, function value or `dyn` method. An effect list (§36.3) does not help: it covers only `sleep`, `alloc`
    and `suspend`, so an `@effects()` `borrow mut fn` may write anything. The port binds such a result to a local
    before the call, or moves the assertion into a `cfg` block.
  - **Drops count.** A temporary or local made inside the condition or message is dropped there, so only in debug
    builds. Its drop glue (`drop` blocks, the contents' drops) is checked as a call under these rules; freeing memory
    the condition itself allocated is allowed, as `alloc` is.

  A CeeSafe function is allowed when its body, and everything it calls or drops, keeps these rules. The checker infers
  this with §32.1's machinery, as a fourth, internal fact. It is not an `@effects` name, so `@effects()` keeps its
  meaning. The message names the chain of calls, as N0295's does.
- `assert` is unchanged: always evaluated, writes allowed.

Bun's 60 suspect sites (§1.6) become compile errors, and the port fixes each on purpose. The `try_lock` moves into a
`cfg("BUN_DEBUG")` block that also releases the lock, the `next()` into a local, and the C call into a `cfg` block.

### 2.6 Bun's own macros and proc macros: four treatments (the porting process)

1. **Library and rule**, when a macro wraps a function. `declare_scope!(X, vis)` becomes `public static X:
   ScopedLogger = …;`. `scoped_log!(X, "f {}", a)` and the per-file `log!` become `if (cfg("BUN_DEBUG")) { if
   (X.isVisible()) { var line = …; X.log(line.view()); } }`, so the arguments are evaluated only in debug builds, as
   Bun requires ("MUST gate arg evaluation", `bun_core/output.rs:1497`). `debug!` and `mark_binding!` follow suit.
2. **Generate at port time**, when a macro computes from literals. The generator runs Bun's algorithm and writes
   the result. `pretty!` and its family become `Output.pretty(COLORED, PLAIN, …)`, with both texts made from the
   markup. `t!`, `param!`, `node_entry!`, `comptime_string_map!` and `w!` become constant tables (ADR 0066) with
   lookup functions.
3. **Expand, then port** for local syntax sugar (`cat!`, `of!`, `col_ref!`, `bail!`, the formatter's `write!`). Bun's
   pinned nightly (`rust-toolchain.toml`) prints expansions with `-Zunpretty=expanded`.
4. **Retarget Bun's codegen** for bindings. `#[host_fn]`, `#[JsClass]`, `#[uws_callback]` and `#[host_call]` become
   `// HOST_EXPORT(…)`-style comment markers. The ported generators (`generate-host-exports.ts`,
   `generate-classes.ts`, …) emit CeeSafe `@export` shims. Attributes stay closed (N0290).

### 2.7 Constant concatenation

In a constant or static initialiser (§10, §39.4), `a + b` with both operands constant `StringView`s is a constant
view: `a`'s bytes, then `b`'s. With a `c"…"` right operand, the result is a `CStr<static>`, and a NUL byte in `a`
(`"a\0b" + c"x"`) is N0006 at the `+`, as inside a `c"…"`. Elsewhere, `+` on views stays
N0207. This covers `concatcp!`/`formatcp!` (70) and the `concat!`s that name constants.

### 2.8 `embed("path")`

`static NAME: Slice<u8> = embed("path");` and `const NAME: StringView = embedText("path");` are a file's bytes, read
when the package is checked and built. `embedText` checks UTF-8 (N0361 for text that is not).

- The path is relative to the source file. It stays inside the package, or inside a directory the manifest lists in
  `[embed] roots` (for Bun, its codegen output). No absolute paths, no environment variables: the same files give the
  same program.
- **Symlinks.** The compiler resolves the path fully (every symlink, `..`) and checks the real path against the real
  paths of the package and the roots. A symlink is followed only if its target stays inside them.
- `check`, the LSP and MCP read the file as well. A missing file, or a path outside the package and roots, is N0362
  at the call. That answers ADR 0074's reason for deferring: tools that check a package already read its files.
- The C backend emits a `static const uint8_t` array; the interpreter loads the bytes at start.
- This replaces 116 sites, including the release path of `embed_compressed!` (`zstd/lib.rs:772`).

### 2.9 Library additions

lib/core gains `CStr` (§2.4). `String` and `VectorBytes` gain `appendHex(value, minWidth, upper)`,
`appendOctal(value)`, `appendFixed(value, digits)` and `appendPadded(text, width, align, fill)`, mirroring
`TextWriter`'s `hex` and `decimal` (lib/core/format.cee:44, :75). They cover §1.2's specifiers.

## 3. Alternatives considered

- **Pattern macros.** They would port Bun's 697 definitions as written, but import hygiene, dispatch pools and
  ambiguous diagnostic positions. **Procedural macros or compile-time evaluation:** the compiler would run user code
  while checking, which breaks determinism unless sandboxed; §2.6's scripts do that job in the open.
- **A type-checked built-in `format("…{}…", x)`.** Shorter than append chains, but it puts format strings in the
  language, and 708 of 807 `format!` calls need only the mechanical rule. It can be its own ADR later.
- **Deriving `Clone`, `Default` and `Debug` in the compiler too.** `Clone` needs fallible allocation and rules for boxes
  and borrows, `Default` needs static interface methods (ADR 0086), and `Debug` needs a formatting interface. Generated
  source works today, and drift fails to compile.
- **A user-implemented `Equal` interface behind `==`.** That is operator overloading (language-spec.md §6). Derived
  equality gives `==` one meaning.
- **`debug_assert!` as `assert`.** Safe, but it puts 2,355 checks, some of them linear (`is_sorted_by_key`,
  `lint/runner.rs`), on release paths. **As a `cfg` block around `assert`.** This works today but keeps Rust's
  hazard: the write vanishes in release builds, and nothing reports it.
- **`embed` by generated `[u8; N]` literals.** Feasible for Bun's small files, but it slows `check`, and it is the kind
  of workaround AGENTS.md rules out.
- **`"…\0"` by rule instead of `c"…"`.** A forgotten `\0` is undefined behaviour in C that nothing catches.

## 4. Not in this ADR

- `#[cfg]` items and debug-only fields (`debug_mutex`) beyond ADR 0080 §3.3.
- `extern "sysv64"` exports for JSC on Windows x64 (`jsc_macros/lib.rs:92-95`).
- `e is P`, guards and or-patterns (ADR 0085); default methods and `Self` (ADR 0086); const generics (ADR 0083).
- `thread_local!`, `asm!`, `smallvec!` (a port library type), and a general writer interface (`io::Write`).

## 5. Testing

- **Run on 2026-11-09:** `python3 -I macro_census.py ~/dev/CeeSafeApps/bun/upstream` (48 s), `shapes.py`, and §1.7's
  probes (`run --interp`; also `build` for the C-string and `cfg` ones). They are in
  `~/dev/CeeSafeApps/bun/analysis/adr-evidence/0087/`.
- **Proposed corpus, run-pass (also interpreted):** derived `==` on structs, payload enums, generic, nested, optional
  and float fields; `HashKey` in a `HashMap` and `OrderKey` in an `OrderedMap` against Python oracles; `c"…"` through
  `strlen`, in a static and through `fromBytesWithNul`; constant `+`; `embed`, `embedText`; `debugAssert` failing
  under `-g` and absent without it.
- **Proposed corpus, check-fail:** a field without `==`; `HashKey` without `Equal`; an unknown derive; `==` in generic
  code; `\0` in `c"…"` or in a constant `+`'s left operand; a `CStr` from `fromBytesWithNul` outliving its bytes;
  `+` on views outside a constant; a missing or escaping `embed` path, and a symlink out of the package; each write
  and move §2.5 refuses, one two calls deep, one in a temporary's `drop`, a closure and a `dyn` call, and §1.6's
  three Bun shapes.
- **Proposed, port:** the M1 trial files use every rule of §2.2. Reviewers check macro sites against the Rust (ROADMAP
  §5's macro row), and a rerun census gives the port its checklist.

## 6. Questions for the owner

1. No general macros. Bun's macros are handled by §2.2's rules and §2.6's four treatments, plus §2.3–§2.5 and
   §2.7–§2.8. **Recommended: yes.**
2. `@derive(Equal)` with float fields: IEEE `==`, as Rust does, or N0207, as ADR 0072 does for float slices?
   **Recommended: IEEE `==`.** Bun's CSS values compare that way.
3. `debugAssert`'s no-write, no-move rule, refusing `extern` calls even when declared `@effects()` and every call
   through a closure, function value or `dyn` method, for the same reason? **Recommended: yes.** The Bun sites move
   into locals or `cfg` blocks, and `@effects()` keeps today's meaning.
4. Debug assertions on by default under `-g`, `test` and the interpreter, and off otherwise? **Recommended: yes.**
   This matches Rust's `debug_assertions` profile.
5. `c"…"` with lib/core's `CStr`, rather than `"…\0"` by rule? **Recommended: yes.**
6. `embed` now, with package-relative paths and `[embed] roots`, reversing ADR 0074 §4's deferral? **Recommended:
   yes** (§2.8).

## 7. Releases and diagnostics

Released with the set's plan: 0.24.3 (ADRs 0081, 0082 stage A); 0.25.0 (0086, 0084, 0085 step 1); **0.26.0, step 1**:
`@derive` (§2.3), `c"…"` and `CStr` (§2.4), `debugAssert` (§2.5) and §2.9's helpers, beside 0083 and 0085 step 2, as
they use 0084's cells and benefit from `is`; **0.27.0, step 2**: constant `+` (§2.7) and `embed` (§2.8). New codes:
N0360, a write or move in `debugAssert`'s condition or message; N0361, an `embedText` file that is not UTF-8; N0362,
an `embed` path that is missing or outside the package and roots.

## 8. Implementation notes (step 1, 0.26.0)

Step 1 (§2.3, §2.4, §2.5 and §2.9) is implemented as written, with these precisions and the fixes after its review
(2026-11-09):

- **`CStr`.** `length()` is a method, beside `ptr()`, `view()` and `bytesWithNul()`, not a field. `fromBytesWithNul`
  also requires the text before the NUL to be UTF-8, since `view()` returns a `StringView`. The fields `text` and
  `bytes` are private to lib/core (N0296): a struct literal in a program could otherwise pair any text with bytes
  that have no NUL, and `ptr()` would hand C a string that never ends. lib/core's `TextWriter`, whose `cText()` relies
  on the zero after what was written, has private fields for the same reason. No other type this release adds keeps
  an invariant in public fields (`Formatted` has none, `TextAlign` is an enum, `DerivedEqualElements` has none).
- **`DerivedEqual<T>`** is an internal interface of lib/core: `@derive(Equal)` writes its impl, a hand-written one is
  N0290, and `a == b` on a derived type calls it. Programs do not name it.
- **`String == String`** compares the text everywhere, not only inside a derived `derivedEqual`, as `a.view() ==
  b.view()` does.
- **N0360 and statics.** Reading a `static mut` in a `debugAssert` is allowed; writing one, or lending it `mut`, is
  N0360.
- **N0360 and control flow.** The arguments' only way out is their end: `?` and `return`, a `break` or `continue` to
  a loop outside them, and a process exit (`Process.exit`, directly or through any call, which the write fact now
  counts) are N0360; a loop inside the arguments may break and continue. When a move inside the arguments is
  followed by a use, N0360 is reported at the assertion as well as N0250 at the use, since N0250 stops the effect
  checker that finds N0360 otherwise.
- **Sequences of derived types** (§2.3's "slices, arrays and vectors of one"). `==` and `!=` on a slice, array or
  vector whose element type `@derive(Equal)` gave equality call lib/core's `DerivedEqualElements<T>.equal(a, b)`:
  the same length and equal elements in order. This holds outside derives too (`a[..] == b[..]` on `Slice<P>`). In a
  generic type's `derivedEqual`, a `[T; 2]`, `[T; N]` or `Vector<T>` field compares as the instance's element type
  allows: N0222 at the first `==` on an instance whose element has no `==` (type-system.md §28.7). A field without
  `==` is reported once, though a generic type's impl is checked twice.
- **Fieldless enums in `HashKey` and `OrderKey`.** A field or payload whose type is a fieldless enum without its own
  `HashKey` (or `OrderKey`) is hashed (or compared) by its variant index as a `u32`, as §2.3 says for `HashKey`;
  `OrderKey` does the same, so `@derive(OrderKey)` on a struct with such a field needs no derive on the enum. The
  checker calls the `u32` instance of `deriveHash` or `deriveCompare` with the index.
- **Const parameters.** On a type with const parameters (ADR 0083), the `HashKey` and `OrderKey` impls bound only its
  type parameters: `impl<T: Copy + OrderKey<T>, const N: usize> OrderKey<Buf<T, N>> for Buf<T, N>`.
- **Not added:** `==` on a `CStr` field (a struct with one is N0207 under `@derive(Equal)`; compare `view()`s in a
  method), and `HashKey` or `OrderKey` for arrays, optionals, boxes and floats, which §2.3 does not list.

## 9. Implementation notes (step 2, 0.27.0)

Step 2 (§2.7, §2.8) is implemented as written, with these precisions (type-system.md §10, §10.1, §39.4; tooling.md
§4.1, §4.4.1; c-backend.md §2):

- **Constant `+`.** It folds in the checker, where the operands are already literals: a string literal, a constant
  (another package's too), an `embedText`, a `c"…"` or `CStr` constant on the right, or another `+`. A `CStr` on the
  left, or two `c"…"`, is N0207; an operand that is not constant is N0217 in a constant and N0338 in a static. The
  N0207 for `+` on views elsewhere now says where it works. A result longer than 16 MiB, `embed`'s limit, is N0217
  (N0338 in a static), with a message that says so: without a limit, a chain of constants that each double the one
  before took gigabytes. No code fitted better: N0217 already covers a constant evaluation that cannot be made.
- **Where `embed` may stand.** Anywhere an expression may, not only in a constant or a static: its value is static
  data like a literal's. A constant and a non-`mut` static may have the type `Slice<u8>` for it (a `static mut` may
  not: N0337); nothing else gives that type a constant value. The path is a string literal or a constant view.
- **Paths.** Besides §2.8's rules: `\` is refused on every system (a separator on Windows, a name's character
  elsewhere: the same files must give the same program), and so are `:` (a drive, an NTFS stream), `$` and `%`
  (nothing is expanded, so they would name a literal directory, which is surely a mistake) and a leading `~`.
- **Resolution.** The system's `realpath` (on Windows, `GetFinalPathNameByHandleW` on the opened path, every
  symbolic link and junction resolved), then a byte-for-byte prefix comparison with the real paths of the package's
  directory and its roots. Byte comparison may refuse a spelling that a case-insensitive file system would accept,
  never accept one outside. The file must be a regular file (a FIFO would block `check`), of at most **16 MiB**: Bun's
  largest is 339 kB, and a file is C text about five times its size. A file given alone (no package: the command
  line, the language server's lone document, the MCP server's `paths`) embeds only below its own directory; a source
  with no file (the MCP server's `files`, the libraries) embeds nothing.
- **The file read is the file checked** (the review found a race: a symbolic link swapped in between the check of
  the path and the opening of the real path it gave leaked a file outside in 28 runs of 300, and a FIFO swapped in
  hung `check`). The path is checked by its real path first, so that nothing outside is opened while nothing races;
  then it is opened with `O_RDONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC`, and the open descriptor is checked: its own
  path (`fcntl(F_GETPATH)` on macOS, `readlink("/proc/self/fd/N")` on Linux, a removed file's `… (deleted)` refused)
  against the same directories, and its kind (`stat` of `/dev/fd/N` or `/proc/self/fd/N`, which describe the open
  file); its bytes are read from it with `pread`, stopping at 16 MiB + 1 however the file grows. Measured after the
  change: no leak in 400 runs on macOS and 300 on Linux, and no hang with a FIFO swapped in.
- **One message for missing and outside.** A path that names no file and one that leads outside get the same N0362
  message, which names the path as written: telling them apart, or printing the real path, told whether a file
  exists outside the package and where a symbolic link leads.
- **How the compiler resolves.** The compiler's sources must still build with ceesafec 0.8.0, whose library has no
  real-path service and no `File.openRegular` (ADR 0078), and whose runtime cannot gain one, so
  `src/host/embedding.cee` declares the POSIX calls in an `extern "C"` block: `realpath`, `uname` (to tell macOS
  from Linux, whose flag values differ), `openat` and `fcntl` (variadic, as C declares them: ADR 0041 §3.3), `readlink`,
  `pread` and `close`. Windows has none of them but `close`: the runtime's `rt.h` maps each name to the hosted
  runtime's own (`ceesafe_win_realpath`, `_uname`, `_openat`, `_fcntl` for `F_GETPATH` only, `_readlink`, which fails,
  and `_pread`), which take UTF-8 paths through the wide calls (`MultiByteToWideChar`, `CreateFileW`,
  `GetFinalPathNameByHandleW`, `WideCharToMultiByte`), never the ANSI code page's; its `openat` opens only a regular
  file on a disk (`GetFileType`, the directory attribute). So the compiler builds on Windows from its seed. A program
  may use the same declarations. Another system is refused (N0362: ceesafec embeds only on Linux, macOS and Windows).
- **Roots.** Relative to the package, under the path rules (N0321 otherwise). They are resolved when a file is
  embedded: a root that is a symbolic link stands for its target, and one that does not exist yet (a build's
  generated directory) reaches nothing. Only the root package and the path dependencies it reaches through path
  dependencies alone, whose manifests the user writes, may have roots outside their directory. A git or registry
  dependency, and every package reached through one, at any depth, is confined to that checkout: a root, its own
  directory included, counts only if its real path is inside it, so neither a symbolic link committed in a
  dependency nor a path dependency it names (`inner = { path = "inner" }` whose roots climb out, which the review
  found) reaches the user's files. A package reached both ways is the user's. A package whose directory lies in the
  package cache (`$CEESAFE_HOME`, by real path) is confined to its own directory even as the root package, so the
  language server going to a definition in a checkout, or `check` run inside it, honours no root the build did not.
- **Hard links** cannot be detected: a hard link inside the package to a file outside has a real path inside, and is
  read. Making one needs write access to the package's directory and to the file, on the same volume, and git (so the
  registry) stores none: a fetched package holds no hard link.
- **When files are read.** By every command that checks, the language server and the MCP server included; once per
  compilation by real path, so every use agrees; nothing is kept between compilations, so a file changed after
  `check` is what `build` compiles. ADR 0082's object cache and interface records are not implemented yet (stages C
  and D); when they are, an object's key (its unit's C text) already holds the bytes, and a package's record must add
  the real path and contents of each embedded file to its key. The lock's hash still covers manifests and sources
  only.
- **Code generation.** An embedded file's bytes are a `static const uint8_t ceeLS_<k>[]` at any length but zero,
  once per distinct contents (literals over 4,095 bytes were already); an empty file is an empty literal. The
  interpreter needed nothing: it runs the NIR, which holds the bytes. A static `c"…"` over 4,095 bytes was a C
  literal before; it is an array now too.
- **Not done.** The language server does not watch embedded files (a change shows at the next check of the document).