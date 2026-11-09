# ADR 0081 — C declarations shared between packages: one owner per C symbol, typed views as wrappers

- Status: **Proposed**, 2026-11-09. Raised by the Bun port's M0 (`~/dev/CeeSafeApps/bun/ROADMAP.md` §3, M0). Its compiler
  scale test (`~/dev/CeeSafeApps/bun/analysis/scale/README.md`, finding 1) hit N0201 when every package declared
  `strlen`. The counts below were measured on Bun 1.4.4 (`bun-v1.4.4`, `38afee6c84fc`) on 2026-11-09, with the
  scripts named in each place. Each claim about ceesafec was checked with ceesafec 0.24.2 built from `fe4d0ca`.
- Scope: type-system.md §28.2 (N0201's message and position) and §27 (U1, unchanged); diagnostics.md (N0201); the
  checker's C-name table; guide/unsafe-and-c.md (a section on sharing declarations); the Bun port's `PORTING.md` (two
  translation rules). There is no change to the grammar, the C backend or the interpreter.

## 1. Context

A program has one C name for each extern function. Two declarations with that name, or a declaration and an `@export`,
are N0201 (§28.2), even in different packages. Rust allows the same C function in any number of `extern` blocks, in any
number of crates. Bun relies on that:

| Measure (Bun 1.4.4, `src/**/*.rs`) | Count |
|---|---|
| Functions declared in `extern "C"` blocks (all 443 blocks are `unsafe extern`) | 2,971 declarations of 2,819 names |
| Names declared in more than one crate | 128 (133 extra declarations) |
| Names declared in two files of one crate (7 more are declared twice in one file, once per platform under `#[cfg]`) | 10 |
| The 128 whose declarations are identical as text | 8 |
| … identical once `&T` is `*const T` and `&mut T` is `*mut T` | 27 |
| … identical ignoring pointer constness too | 33 |
| Declarations marked `safe fn` (callable without `unsafe`) | 1,068; about 2,850 call sites |
| … of which take a pointer or a reference | 877 |
| … of which take plain values only (integers, `bool`, enums) | 136; 55 more take a `JSValue` by value |

The counts cover the 3,238 `.rs` files under Bun's `src/`, test directories included. The scripts behind them are in
`~/dev/CeeSafeApps/bun/analysis/adr-evidence/0081/` (`dup_extern2.py`, `extern_safe.py`, `safe_kinds.py`), run with
`python3 -I`. They scan the `extern "C"` blocks, and each crate's `Cargo.toml` for the ownership counts.

What the 128 are:
- **libuv:** 90, mostly `uv_*` declared in `bun_libuv_sys` and again in `bun_runtime` or `bun_io`.
- **BoringSSL:** 17, mostly `SSL_*` in `bun_boringssl_sys` and again in `bun_runtime`.
- **The rest:** Bun's own C++ bindings (`Bun__X509__toJSLegacyEncoding`, three crates), `kqueue`, `getpid` and
  `clock_gettime`.

Most redeclarations exist to give the same C function a **stronger Rust type** at the call site, not by accident:

```rust
// src/boringssl_sys/boringssl.rs
pub fn SSL_is_init_finished(ssl: *const SSL) -> c_int;
// src/runtime/ (the same symbol, callable without `unsafe`, taking a reference)
pub safe fn SSL_is_init_finished(ssl: &SSL) -> c_int;
```

The rest has drifted:
- `getpid` returns `c_int` in `bun_core` and `pid_t` in `bun_io`.
- `SSL_CTX_get0_certificate` returns `*X509` in one crate and `*c_void` in another.
- `JSFunction__createFromZig` takes a `*String` in one crate and a `*BunString` in another.

C sees one function in every case, and nothing checks that the declarations agree.

What CeeSafe already offers. Each point was checked on 2026-11-09 with a two-package probe:
- **A public declaration is shared.** `public fn strlen(…);` in `sys`'s `extern "C"` block is called from a dependant
  as `unsafe { sys.strlen(p) }`, natively and in the interpreter (§28.2: "each optionally preceded by attributes and
  a visibility").
- **A second declaration in the dependant is N0201.** The message is reported at the *first* declaration
  (`sys/src/lib.cee`), not at the one that clashes, and it doesn't say which package owns the name.
- **References don't cross.** A `borrow T` parameter in an extern declaration is N0291 (§28.3). The only FFI-safe
  pointer types are `*T` and `*mut T`.
- **Calling C is always unsafe** (U1, §27; N0280). There is no safe C declaration.

## 2. Proposal

### 2.1 One declaration per C symbol: N0201 stays

A program keeps exactly one declaration for each C name.
- **Packages:** other packages call that declaration through its package, as `sys.strlen`. The owner makes it
  `public`.
- **Identity:** the C symbol has one CeeSafe signature, so the drift above (`c_int` against `pid_t`, `*X509` against
  `*c_void`) cannot exist.
- **Code generation:** the C backend already emits one prototype per extern (c-backend.md §2), so nothing changes there.

This is the rule CeeSafe has today. The ADR makes it a deliberate decision, with three reasons:
1. **Signatures can't drift.** C cannot check that two declarations of one function agree, and CeeSafe could compare
   only the C types it maps them to. The drift above is in the Rust types, which is exactly what a single declaration
   removes.
2. **Identical redeclarations wouldn't cover Bun.** Only 8 to 33 of Bun's 128 shared declarations would qualify (the
   table above). The rest differ in what the port has to decide once per symbol anyway: which pointee type and which
   constness.
3. **Ports already have a single owner available** (§2.3): 124 of the 128 have one.

### 2.2 Better N0201

N0201 changes only in where it points and what it says:
- **Position:** it is reported at the declaration that clashes: in a dependant, the dependant's own. For two
  declarations in one package, it is the later one in the package's file order (tooling.md §4).
- **Note:** a note names the other declaration's package and position.
- **Hint:** when that package is a dependency and its declaration is `public`, the hint is the fix: ``use
  `sys.strlen`, which package 'sys' declares; remove this declaration``. When it is not public, the hint says to make
  it `public` there.
- **Fix:** with `--diagnostics-format=json`, the fix removes the redundant declaration when the two declarations are
  identical in CeeSafe: the same parameter and result types, variadic-ness and attributes (`@cname`, `@effects`). An
  identical C signature is not enough: by §2.1's own argument, `*void` and `*X509` are one C type and two meanings.
  `ceesafec fix` (tooling.md §5.3) can then apply it. The call sites still name the item, so they need
  `sys.` in front. The fix does not rewrite them: a rename across files is the porting tool's job (§2.3).

### 2.3 Translation rules for the port (`PORTING.md`)

These are rules for the Bun port. They need nothing more from CeeSafe:

1. **The owner of a C symbol** is the declaring crate that every other declaring crate depends on directly. That
   gives 124 of the 128 an owner: `bun_libuv_sys` 87, `bun_boringssl_sys` 16, `bun_jsc` 5, `bun_core` 5, `bun_sys` 5,
   `bun_io` 2, and 4 among other crates.
   - **Two** have an owner every declarer reaches only indirectly. The port adds that direct dependency edge.
   - **Two have no common owner**: `Bun__X509__toJSLegacyEncoding`, in `bun_http_jsc`, `bun_runtime` and
     `bun_sql_jsc`; and `Yarr__RegularExpression__deinit`, in `bun_jsc` and `bun_yarr`. They move to the lowest
     package that all their users depend on, adding an edge if necessary.
   - **Names in two files of one crate (10)** are merged into one declaration in the crate. The 7 declared once per
     platform in one file keep their `cfg` blocks (ADR 0080 §3.3).
2. **The owner's declaration uses raw pointers,** with the constness of the `-sys` crate's declaration, or of the
   first declaration where there is no `-sys` crate. `*const T` becomes `*T`, and `*mut T` and `&mut T` become
   `*mut T`.
3. **A typed or safe view becomes a CeeSafe wrapper.** A Rust `safe fn`, or a redeclaration with references, becomes
   one `public fn` in the package that declared the view. It takes `borrow mut T` where the Rust took `&mut T`, and
   `borrow T` where the Rust took `&T` **only if C only reads the object** through it. A shared borrow freezes the
   value (unsafe.md §2), so a C function that writes through the pointer, or keeps it, gets a raw-pointer parameter or
   an opaque handle type instead (ADR 0084's `RawCell` is the tool for Bun's opaque C++ objects). It calls the owner's
   declaration in `unsafe`. Its `/// Safety:` line states the contract the
   reference type gave in Rust:

   ```ceesafe
   /// Safety: `ssl` is a live SSL the borrow keeps alive; SSL_is_init_finished only reads it.
   public fn sslIsInitFinished(ssl: borrow Ssl) -> bool =>
       unsafe { boringssl_sys.SSL_is_init_finished(ssl.raw()) } != 0;
   ```

   Call sites of a Rust `safe fn` call the wrapper and need no `unsafe`, as in Rust. The 1,068 declarations get
   about 1,000 wrappers. Each is mechanical, and the porting process generates them from the Rust signature.
4. **Value-only safe functions** (136, such as `BrotliEncoderMaxCompressedSize(usize) -> usize`) get wrappers too,
   with the one-line reason for their safety. §6 question 2 asks whether to add a language form for them instead.

### 2.4 Cost

- **Wrapper overhead:** one call per wrapper, which the C compiler inlines when the wrapper and its caller are in one
  translation unit. That is always true today, because the program is one `main.c`.
- **With split output:** across units it needs link-time optimisation or a header copy. ADR 0082 proposes split
  output as the default and names these wrappers among its cross-unit inlining cases (§2.4): a release build uses
  `--lto`.
- **No new checks:** the owner rule adds nothing at run time.

## 3. Alternatives considered

- **Allow redeclarations whose C signatures are identical.** These are the C types after §28.3's mapping, plus
  variadic-ness and `@cname`. Each declaring package would keep its own block, as Rust crates do.
  - **For:** a closer mechanical port, for the 8–33 that agree.
  - **Against:** it brings back the drift for every declaration that agrees in C but not in meaning (`*void` against
    `*X509` is the same C pointer only after a cast). It covers a quarter of Bun's cases at best. And it makes "which
    declaration does this call use?" a question with several answers.
  - Rejected for now; §6 question 1.
- **Allow any redeclaration, with the C compiler as the judge** (C11 §6.2.7: compatible types). This is what Rust
  does. It moves a safety-relevant check to a tool that reports it late, in generated C, and only within one
  translation unit. Rejected.
- **References in extern signatures** (`borrow T` as FFI-safe, crossing as a pointer). This would let a declaration
  itself carry Rust's `&T` contract and remove most wrappers. But a C function can keep the pointer past the call, and
  CeeSafe's regions can't say whether it does. The wrapper states it in a `Safety:` line instead. Rejected. A later
  ADR could add `borrow` parameters for C functions marked as not retaining.
- **Safe C declarations: `@safe` on an extern function whose parameters and result are plain values** (integers,
  `bool`, `CChar`, `CLong`, `@repr` enums), with a `/// Safety:` line on the declaration. Calls would then need no
  `unsafe`. It is sound for value-only signatures, because no pointer means no memory is reachable through the
  arguments. But U1 would no longer be absolute: some C calls would be safe, and C can still do anything inside, such
  as write a global or call `exit`. It saves one wrapper for each of about 190 functions (136, and 55 taking a
  `JSValue`). Deferred to the owner: §6
  question 2.

## 4. Not in this ADR

- **Inlining wrappers across translation units:** ADR 0082.
- **C *data* shared between packages:** imported constants and C statics, ADR 0080. The same one-owner rule applies,
  but nothing in Bun needs it yet.
- **`@export` from several packages of one program:** unchanged; N0201 as today.
- **Two packages from different authors** that each declare the same C name privately cannot be used in one program
  without editing one of them. Nothing in the Bun port hits this, since one porting process writes every package. A
  later ADR could allow identical, non-public redeclarations if other ports do.
- **Bun's own C++ binding generator:** `src/codegen` emits many of the `Bun__*` declarations. The port runs it to
  emit CeeSafe, and each generated symbol has one owner by construction.

## 5. Testing

- **check-fail (corpus):** two packages declaring one C name.
  - Expected: N0201 at the dependant's declaration, a note at the owner's, and the `sys.strlen` hint.
  - The same with a non-public owner gives the "make it public" hint.
  - Two files of one package give N0201 at the later file.
- **fix:** `ceesafec fix` on the first case removes the dependant's identical declaration, and leaves a non-identical
  one alone.
- **run-pass:** a dependant calls a `public` extern through its package, natively and with `--interp`.
  - It also calls a wrapper with a `borrow` parameter.
  - The test pins down the behaviour the port relies on.
- **The Bun port:** the porting tool's owner computation is checked against the counts above (124, 2, 2, 17). The
  scale test (`analysis/scale/scale_test.py`) already uses one owner, `p000`.

## 6. Questions for the owner

1. **Keep one declaration per C symbol (N0201), rather than allowing identical redeclarations across packages?**
   Recommended: keep it (§2.1). Identical redeclarations cover at most 33 of Bun's 128, and the rule prevents drift.
2. **`@safe` on value-only C declarations (§3), or wrappers only?** Recommended: wrappers only for now. It keeps U1
   ("calling C is unsafe") without exception. About 190 three-line wrappers are a small cost, and the question can
   come back if other ports show many more.
3. **N0201 at the clashing declaration, with the owner's hint and a fix (§2.2)?** Recommended: yes, in 0.24.3, with
   ADR 0082's stage A. It changes only diagnostics, so no program's meaning changes.
