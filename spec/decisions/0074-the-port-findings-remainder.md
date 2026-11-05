# ADR 0074 — The rest of the port's findings: unit payloads, `never`, run-once captures, method regions, optional borrows, raw literals, associated consts, iterator loops, integer conversions, shadowing a dependency

- Status: **Accepted** by the owner, 2026-11-05 ("Take over ~/dev/CeeSafe session and fix all those"; §6 answered as
  proposed: one release as 0.18.0, deep results with the new callee rule, C#-style raw `"""` literals, N0401 on an
  ambiguous shadowed member). Raised by the mail server port (`CeeSafeApps/mailserver/CEESAFE_FINDINGS.md`
  §3.7, §3.9, §3.10, §3.11, §3.12, §3.13, §3.14, §4, §4.1). The owner asked to fix every finding still open in
  0.17.0. Each was reproduced with ceesafec 0.17.0 on 2026-11-05 (the findings' status table).
- Scope: type-system.md §1, §7, §17, §20.4, §22.1, §25.1, §25.6, §36.5, §40.4–§40.5, the `for` statement; lifetimes.md
  §7 (elision); grammar.md §1.6 (string literals) and the struct and impl bodies; diagnostics.md; the lexer, parser,
  checker, NIR lowering, drop elaboration, C backend, interpreter, formatter, VS Code grammar and semantic index;
  guide/for-agents.md, templates/AGENTS.md; the corpus.

## 1. Context

0.16 and 0.17 fixed 14 of the port's 24 findings. Nine are still open. Each costs the port a helper, a rename or a
workaround structure:

| Finding | Today | The port's workaround |
|---|---|---|
| §3.7 a generic used with `void` | N0222: `Ok(v)` binds a payload that `Result<void, E>` lacks | a second, void-only adapter per generic (msmtp `oom`/`stored`, mqueue `grown`) |
| §3.9 `async move` cannot consume a capture | N0251 for any non-Copy capture, ADR 0053's own `async move () => await serve(stream)` included | the value travels in a one-element `Vector` that the child pops; tls gained `acceptWithin` |
| §3.10 method region parameters; a `borrow mut this` result | N0216; the result keeps the `this` loan (N0241 on the next call) | free functions, lexers that return offsets |
| §3.11 multi-line literals; consts in structs | N0005; N0107 | SQL in `.sql` files and a generator; `OID_…` top-level names |
| §3.12 an optional field as `borrow T?` | N0205 | always hold a configuration, pass a `useTls` flag |
| §3.13 `for` over `split` | N0205 | `loop { match (parts.next()) { … } }` |
| §3.14 `usize` to `i64` | N0208, and `checkedTruncate` is float-only | `truncate<i64>(n)`, which reads as lossy |
| §4 a dependency's name on a local | N0315 for every local, parameter and binding | renamed the `files` library to `diskio`; parameter names other than `tls` |
| §4.1 a function that never returns | `never` is not nameable (N0204); a `die()` wrapper in a value match is N0235 | a dead `return` after each call |

Most of the machinery exists: `TypeKind.Never` and divergence (`src/types/types.cee:12`, `src/check/body.cee:684`),
drop flags and partial moves (`src/nir/drops.cee`, `src/nir/move_paths.cee`), method region scopes
(`src/check/regions.cee:23`), header temporaries for `for` (`src/nir/nir_builder.cee:1109`).

## 2. Proposal

### 2.1 Unit payloads: `Ok(v)` with `T = void` (§20.4)

Where a variant's payload type is `void`, a pattern may bind one name to it and a construction may pass one argument
of type `void`:

```ceesafe
fn conv<T>(r: Result<T, A>) -> Result<T, B> => match (r) { Ok(v) => Ok(v), Err(_) => Err(B.Y) };
conv(act())   // act() -> Result<void, A>: now accepted
```

- A name bound to a unit payload is a **unit local**: it has type `void` and holds nothing. Reading it is a unit
  value. `let v = r?;` with `r: Result<void, E>` declares one too.
- A unit value may be passed where `void` is expected: a void payload, `return v` in a void function (it is
  `return;`), an expression statement.
- The checker erases unit values. A unit local becomes no HIR local, a unit read becomes no operand, and a
  construction `Ok(e)` with a unit `e` keeps `e`'s effects and builds the payload-less variant. NIR, the C backend
  and the interpreter are unchanged.
- This holds in generic instances and in ordinary code, so a generic body means the same written out by hand.
- Not included: user generic enums instantiated at `void` (N0226), `Some(v)` with `void`.

### 2.2 `never`, a return type (§1, §15.3)

`never` names the existing bottom type, as a function's return type only:

```ceesafe
fn die(message: StringView) -> never { Console.writeLine(message); Process.exit(1); }
let n = match (parse(text)) { Ok(v) => v, Err(_) => { die("bad number"); } };   // accepted
```

- A `never` function's end must be unreachable (N0213), and `return` in it is an error.
- A call of one diverges, as `Process.exit` does: the code after it is unreachable, and a branch ending in it gives no
  value and needs none.
- Anywhere else (a parameter, a field, a local, a type argument, a function type, an async function, a closure, an
  interface method, an `extern` function) `never` is N0226: "'never' can only be a function's return type".
- In C, the function is `_Noreturn void`. In NIR, a call of one ends its block with `Unreachable`.
- `never` is a type name, not a keyword: values named `never` (lib/std's `channel.cee:60`) are unaffected.

### 2.3 Run-once environments: a child body consumes its captures (§36.5, §40.5, ADR 0053)

A move environment whose body runs at most once is **run-once**. Its body may move each capture out, once:

```ceesafe
s.spawn(async move () => await serve(stream));                       // stream: AsyncTcpStream, moved into serve
let r = await Async.timeout(ms, E.Timeout, async move () => await TlsStream.accept(stream, cfg));
```

- Run-once bodies:
  - `s.spawn`;
  - `set.spawn`, `spawnSend` and `trySpawn`;
  - the bodies of `Async.timeout`, `deadline`, `race` and `all`, which may now be `async move`. Today they are
    N0209.
  - Not `Box<dyn fn>` move literals, which may be called again. They keep N0251.
- The environment's captures become move paths under `*env` in a run-once function (`src/nir/move_paths.cee`), as a
  box's contents are. A second move is N0250. A capture that holds a `drop` block cannot be partly moved (as today).
- The body drops what it did not move, when it returns, through the drop elaboration that locals use. Captures that
  may or may not have been moved get drop flags. The code that ran a child's whole environment drop after the body
  (`src/backend/c_async.cee:120`, `src/interpreter/io.cee:2029`) no longer does. A child that never started still
  drops its whole environment.
- A cancelled body returns along a normal path (§40.6: cancellation never destroys a frame), so its remaining
  captures are dropped there. A value moved into a timeout's body is gone after the deadline. That is the meaning of
  `move`.
- Field-precise captures stay references, and are never movable.

### 2.4 Regions on methods, and results tied to the struct's region (§25.1, §25.6, lifetimes.md §7)

**Method region parameters.** A struct's or an inherent impl's method may declare its own regions, as a free function
does:

```ceesafe
fn first<region a>(borrow this, other: borrow(a) Vector<u8>) -> Slice<a, u8> => other[0..1];
```

- Interface methods and methods that implement an interface keep N0216, because conformance compares the
  signatures and would have to match the region lists.

**Deep results.** Every reference parameter, the receiver included, gets its own elided outer name. A result that
names only regions the parameter's pointee carries is a **deep** result of that parameter. It is tied to those
regions, not to the parameter's loan:

```ceesafe
struct Lexer<region r> {
    text: Slice<r, u8>;
    at: usize;
    fn token(borrow mut this) -> Slice<r, u8> { let s = this.at; this.at += 1; return this.text[s..this.at]; }
}
let t = lexer.token();
lexer.skipSpace();   // accepted: t points into the text, not into lexer
```

- **Callee rule.** A deep result may carry only loans the parameter's pointee already held: a copy of a view field,
  or a slice of one. It may not carry a loan of a place reached through the parameter. Today `fn f(borrow mut this)
  -> Slice<r, u8> => this.buf[0..2]` is accepted, and is sound only because callers tie the result to the `this`
  loan. Under this ADR, it is N0245.
- Slicing a shared view place (`this.text[a..b]` with `text: Slice<r, u8>` or `StringView<r>`) copies the view's
  loans instead of making a new loan of the place. Slicing a `SliceMut` field stays a reborrow of the place.
- **Caller rule.** A deep source's argument `borrow p` contributes `p`'s own loans to the result, not the new loan of
  `p`.
- **What breaks.** Only a function returning a view of its parameter's own storage under a pointee region. It must
  name the parameter's elided region (`-> Slice<u8>` from `borrow this`) instead. The three repositories are searched
  before the release.

### 2.5 An optional place as an optional borrow (§22.1, §13.2)

- `o.asBorrow()` on a place of type `T?`, or on a `borrow (T?)`, is `borrow T?`: `Some` borrows the payload, and
  `null` is `null`. `o.asBorrowMut()` on a mutable place is `(borrow mut T)?`. These are optionals' third and fourth
  methods (ADR 0072 §2.2).
- At an argument whose parameter is `borrow T?` (or `(borrow mut T)?` with `mut`), a place of type `T?` converts
  implicitly, as a place of `T` converts to `borrow T` today (§13.2). A place of `T` converts to `Some(borrow t)`.
  So `f(relay.tlsConfig)` is accepted.
- The lowering is the hand-written `match`: a tag test, then the payload's address or the null niche. The borrow
  checker sees a shared or mutable loan of the place.

### 2.6 Raw multi-line string literals (grammar.md §1.6)

```ceesafe
let query = """
    SELECT id, name
    FROM users
    WHERE id = $1
    """;
```

- `"""` ends its line. The content is the lines that follow, up to a line holding only whitespace and `"""`.
- The closing line's indentation is removed from every content line. A content line that is not blank and indented
  less is N0011, a new code. Blank lines are empty. The newlines between content lines are `\n`, also in a CRLF file.
  There is no final newline.
- No escapes: a backslash is a backslash, and `{` is a brace. N0400 does not apply.
- The token is a `StringLiteral`, so everything after the lexer is unchanged. `fmt` moves the interior lines and the
  closing line with the opening line's indentation, and never reflows them.
- Not included: `embed("file")`, which needs file access during checking and in the LSP and MCP. It can come later.

### 2.7 Associated constants (§17)

```ceesafe
struct Oid { const RSA: [u8; 9] = [0x2A, …]; … }
impl Mode { const DEFAULT: u32 = 25; }
let a = Oid.RSA; let b = pkg.Oid.RSA;
```

- `const NAME: T = e;` is allowed in a struct body and in an inherent impl, so enums have them too. They are
  evaluated as top-level constants are (`src/check/consts.cee`), and reached as `Type.NAME`.
- Not allowed in a generic struct or impl: N0216, for now.
- A constant and a method of the same type cannot share a name (N0228, as a duplicate member). The visibility rules
  match methods'.

### 2.8 `for` over an iterator (§15, the `for` statement)

`for (x in e)`, where `e`'s type has a method `next(borrow mut this) -> T?`, calls `next` until it returns `null`:

```ceesafe
for (part in text.split(",")) { … }
```

- `e` is evaluated once into a header temporary, which lives for the whole loop (ADR 0073 §2.1).
- Each pass mutably borrows the iterator only for the call to `next`. The body may use what the iterator borrows,
  `text` above, as a shared borrow.
- `x` has type `T`, its region that of `next`'s result.
- `break` and `continue` keep their meaning.
- No interface is involved: the method's shape is the rule, as `drop` blocks name a shape. lib/core's `TextSplit`,
  `TextLines` and the other `next` iterators work without change.

### 2.9 Checked and saturating integer conversions (§7, §31)

- `checkedTruncate<T>(e) -> T?` and `saturatingTruncate<T>(e) -> T` take integer operands, as well as floats:
  `checkedTruncate<i64>(n)!` for a `usize`.
- `as` stays lossless only. The guide's conversions table names the three spellings:
  - `as`: never loses information;
  - `truncate`: wraps;
  - `checkedTruncate` and `saturatingTruncate`: check or clamp.
- Not included: making `as` depend on the narrowest supported target (§4).

### 2.10 A local may shadow a dependency (§4 of the findings; tooling.md §4)

- A parameter, local, loop variable or pattern binding may have a dependency's name. In its scope, `name.x` in an
  expression means the local's member.
- Items and type parameters keep N0315, because they share the namespace that `dep.Item` is resolved in.
- Types and patterns are unaffected (a local is never a type), so `tls.TlsConfig` in a type keeps naming the package.
- The join of `dep.X` in expressions (`src/check/packages.cee:635`) becomes scope-aware. It is skipped where a local
  of that name is in scope.
- A shadowed dependency's values are unreachable in that scope. Where `X` is both a member of the local and a public
  item of the dependency, warning N0401 says so.

## 3. Alternatives considered

- **§2.1:** a real unit type in NIR and C. It would be uniform, but would touch every pass for a value with no bits.
  Erasing units in the checker keeps the change in one layer.
- **§2.3:** an `Async.take(capture)` intrinsic to move one capture out explicitly. It is more to write, and a run-once
  rule is what `move` promises a reader.
- **§2.4:** relaxing only the caller. It is unsound (the `this.buf` example).
- **§2.6:** `embed("file")` only, or C-like `"a" "b"` concatenation. The first needs host access in every tool. The
  second keeps escapes and quotes on every line.
- **§2.8:** an `Iterator<T>` interface. That needs `dyn` or generic-bound dispatch, and gives nothing over the shape
  rule today.
- **§2.9:** `i64.fromSize(n)`, one function per pair. The two existing generic builtins cover every pair.

## 4. Not in this ADR

- `var` of a view type (N0246). Re-pointing a reference breaks "assigning to a reference writes through it", so it
  needs its own ADR. Its workaround, a struct field holding the view, works today.
- Generic enums instantiated at `void` (N0226), and `Some(v)` with `void`.
- `as` checked against the narrowest target: `u64 as usize` is accepted, and it is lossy on a 32-bit target. The
  compiler has about 3,900 `as usize`/`as isize` casts to sort first.
- `embed("file")`.
- Consts in generic structs.

## 5. Testing

Every new program runs compiled, with `--interp`, and with `--check-leaks`.

- **§2.1:** run-pass:
  - the finding's `conv(act())`;
  - `let v = r?; return Ok(v);`;
  - a void instance of a generic that also runs at `u32`.

  check-fail: a unit local used as a value of another type.
- **§2.2:** run-pass:
  - `die()` in a value `match`;
  - code after a call is unreachable.

  check-fail:
  - `never` as a parameter or field (N0226);
  - a `never` function whose end is reachable (N0213);
  - `return` in one.

  emit-c: the `_Noreturn` signature.
- **§2.3:** run-pass:
  - `s.spawn` and `set.spawn` moving a non-Copy capture into a call;
  - a capture moved on one path only (a drop flag across an `await`);
  - a timeout body that moves its capture and one that is cancelled before it does, each with a `drop` block
    printing the order;
  - a race loser;
  - a child never started (a full task set);
  - `--check-leaks` on all.

  check-fail: a second move (N0250); a `Box<dyn fn>` move literal (still N0251). The cancellation and fault sweeps run
  over the new programs.
- **§2.4:** run-pass:
  - a method with its own region;
  - the finding's lexer, `token` then `skipSpace`.

  check-fail:
  - the `this.buf` deep result (N0245);
  - a deep result kept past the end of its region's owner (N0243);
  - an interface method with a region (N0216).
- **§2.5:** run-pass:
  - `f(r.cfg)` with `Some` and `null`;
  - `asBorrow` and `asBorrowMut` writing through;
  - a `T` place passed to `borrow T?`.

  check-fail: `asBorrowMut` on a shared place.
- **§2.6:** lexer tests:
  - indentation stripping;
  - CRLF;
  - blank lines;
  - a content line indented less (N0011);
  - an unterminated literal (N0005);
  - `"""` inside a line.

  Also: a run-pass printing a SQL block; `fmt` snapshots; the VS Code grammar.
- **§2.7:** run-pass:
  - struct and impl constants;
  - through a package (`dep.T.C`);
  - in an array length.

  check-fail: a generic struct's constant (N0216); a name shared with a method (N0228).
- **§2.8:** run-pass:
  - `split`;
  - a user iterator struct;
  - `break` and `continue`;
  - the body reading the iterator's source;
  - a temporary iterator with a `drop` block destroyed after the loop.

  check-fail: the body mutating the source (N0241); `next` with the wrong shape (N0205, with a hint).
- **§2.9:** run-pass: every integer pair, at the bounds, against a Python oracle (`tools/oracles/`).
- **§2.10:** run-pass:
  - a local `tls` beside `tls.TlsConfig` in a type;
  - a package test with a parameter named like a dependency.

  check-fail: an item named like a dependency (N0315). warning: N0401.
- **Every change:** the fixed point, the contract suite, and `ceesafec check` plus `ceesafec test` across CeeSafeLibs
  and CeeSafeApps.

## 6. Questions for the owner

1. **All nine in one release, 0.18.0?** **Proposed: yes.** Each part has its own corpus tests, so one part can be held
   back if it isn't ready.
2. **§2.4 deep results**, the one change that can reject accepted code (a function returning its parameter's own
   storage under a pointee region)? **Proposed: yes**, after searching the three repositories. The rule it replaces
   is sound only by accident.
3. **§2.6 raw literal form:** C#-style `"""`, with the closing line setting the indentation and no escapes?
   **Proposed: yes.**
4. **§2.10:** a warning (N0401) when a shadowed dependency's item has the member's name, or silent shadowing?
   **Proposed: the warning.**

## 7. Implementation (2026-11-05)

### 7.1 Where

Five parts were built in parallel on branches `adr0074-types`, `-moves`, `-regions`, `-syntax` and `-loops`, then
merged.

- **§2.1, §2.2 (types):**
  - unit locals are scope entries with no HIR local (`src/check/checker.cee`, `patterns.cee`, `options.cee`,
    `body.cee`);
  - `never` is in the named types, allowed only as a return type (`allowNever`, `declare.cee`);
  - a call of a `never` function ends its NIR block with `Unreachable`;
  - C: `_Noreturn void`.
- **§2.3 (moves):**
  - `StructDef.isOnceEnvironment`, set by `finishMoveLiteral` when the caller marks the literal run-once
    (`Checker.runOnceLiteral`, in `src/check/async.cee`);
  - `*env` is a move path in a run-once function (`src/nir/move_paths.cee`), and remaining captures are dropped at
    `StorageDead(env)` (`src/nir/drops.cee`);
  - the C backend and the interpreter no longer drop a run-once environment after the body.
- **§2.4, §2.5 (regions):**
  - the method-region refusal is lifted except for interfaces and their impls;
  - `HirFunction.deepSources` comes from `resolveFlows`;
  - the borrow checker gives a deep parameter an outer placeholder and a referent placeholder (`LoanEdge.dropOuter`);
    the callee rule is `deepPlaceViolation` (N0245) and the caller rule `deepSourceEdge`;
  - `asBorrow`/`asBorrowMut` desugar to the hand-written `match` (`src/check/options.cee`), and the argument
    conversion is `optionalBorrowArgument` (`references.cee`).
- **§2.6, §2.7, §2.10 (syntax):**
  - `lexRawString` (`src/syntax/lexer.cee`), with the formatter's `verbatim` lines and the VS Code `raw-strings` rule;
  - associated constants in `src/check/member_consts.cee`, declared as `Type.NAME`;
  - the syntactic scope walk in `src/check/shadowing.cee`, which skips the `dep.X` join under a shadowing local and
    reports N0401.
- **§2.8, §2.9 (loops):**
  - `HirStmt.ForNext` and `lowerForNext` (`src/nir/nir_builder.cee`), with the iterator in the header scope and
    `next`'s loan ending before the body;
  - integer `checkedTruncate`/`saturatingTruncate` reuse the `FloatToInt` node, with exact bounds in the interpreter
    and `appendIntFitsTest`/`appendIntSaturation` in the C backend (no UB, no mixed-sign comparison).

### 7.2 Changes from the accepted text

1. **§2.1:**
   - `let x = voidCall();` stays N0224: a `let` declares a unit local only for `?`, `!` or a unit read;
   - a bare `v;` stays N0225;
   - `never` is allowed on struct methods as well as free functions;
   - `let x = die();` is N0224 (0.17.0 emitted C that did not compile for `let x = Process.exit(1);`).
2. **§2.3:**
   - run-once is decided when the literal is checked;
   - a local of the enclosing function cannot be moved into a `set.spawn` child, because the task set's body
     closure only borrows it. A value made inside the body (ADR 0053's `stream` from `accept()`) can be moved.
3. **§2.4:**
   - slicing a shared view field still makes a place loan. The refined callee rule (a loan is a violation only if it
     reaches no further reference) already accepts `this.text[a..b]`;
   - a `borrow (T?)` argument converts only with `asBorrow()`, because `check-fail/optional-references.cee` requires
     N0205 there. Places of `T?` and `T` convert as written;
   - a method region named like its struct's region is N0202.
4. **§2.6:** N0011 also covers text after the opening `"""`.
5. **§2.7:**
   - the spec text is in type-system.md §10 (Constants);
   - enum constants come from `impl E { const … }`;
   - array lengths stay integer literals, as for top-level constants;
   - a bare `NAME` does not reach an associated constant, even in the type's own methods.
6. **§2.8:** the iterator may be a `borrow mut T` (reborrowed, so its state survives the loop); a shared reference is
   N0248.
7. **§2.9:** the C bounds treat `isize`/`usize` as 64-bit, as the backend emits them.
8. **§2.10:**
   - N0401 is reported when `X` is a public item of the dependency, without checking that the local's type has `X`
     (the warning is a syntactic pass);
   - `dep.S { … }` struct literals keep naming the package in a shadowing scope.

## 8. Amendment (2026-11-05): a reference result's own region, and deep results through a call (B18)

Found by CeeSafe.Web on 0.18.0 (its `docs/ceesafe-upstream-work.md`, B18). `web_http`'s
`fn headers(borrow this) -> borrow HeaderList<r> => borrow this.fields` was N0245, and so was every package that
depends on it.

1. **The callee.** §25.6 elided a result's regions only when none were written. `borrow HeaderList<r>` names `r`
   inside, so the reference's own region got no flow name. The result named only `this`'s pointee region and was
   made a deep result of `this`, and `borrow this.fields` is a loan of the receiver's own storage. The result is a
   reference to the receiver's storage, as Rust's `fn headers(&self) -> &HeaderList<'r>` is. Now a result reference
   with no sources or regions written on it gets the elided region of the single scoped parameter, also when its
   pointee names regions, so it is not deep. With several scoped parameters nothing changes: `borrow(p)` names the
   source.
2. **The caller.** A deep result's argument contributed less than its loans only as `&p` or a reference parameter.
   In `o.inner().first()` the receiver of `first` is a temporary holding `inner`'s result, so it kept `o`'s own loan,
   and `fn f<region r>(o: borrow Outer<r>) -> Slice<r, u8> => o.inner().first()` was N0245. Now a call's result that
   passes through one reference parameter (a `borrow T` result tied to it alone, whose pointee names only regions
   that parameter's referent holds) is followed to that argument (type-system.md §25.6; lifetimes.md §7). What the
   result refers to holds no more than the argument's referent: the pointee names none of the parameter's own
   regions, and a referent cannot hold a loan of its own storage. A pointee that names no region yet may hold loans
   (`-> borrow Inner`) is not followed.
3. **Tests:** `run-pass/deep-results-through.cee` (both, with `borrow mut`, an optional and `borrow(this)`; 0.18.0
   refuses it) and `check-fail/deep-results-through.cee` (the result stays tied to the receiver, N0240; a view still
   blocks a push, N0241; the elided pointee is not followed, N0245).
4. **Why the release missed it:** 0.18.0 was checked against CeeSafeLibs and CeeSafeApps, which have no result of
   this shape, and the corpus had none.
