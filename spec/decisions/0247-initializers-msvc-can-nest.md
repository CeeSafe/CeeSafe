# ADR 0247 — Initializers that MSVC can nest

- Status: **Proposed — implemented** 2026-11-25 on the branch `fix/windows-processes`, for 0.95.8, found by the
  SQLite port (its CEESAFE_FINDINGS.md, finding 49).
- Scope: the C backend's compound literals and zero initializers (`src/backend/c_helpers.cee`: `appendUnitValue`,
  `appendZeroValue`, `appendZeroInitializer`, `zeroElisionDepth`, `emitZeroHelper`; their callers in `c_body.cee`,
  `c_async.cee`, `c_dyn.cee` and `c_skeleton.cee`), c-backend.md §1.1, §2 and §5,
  `run-pass/adr0247-deep-initializers.cee` (new; built with cl by windows.yml).

## 1. Context

The SQLite port's test program does not compile with MSVC 19.44:
`main.c(35841): fatal error C1054: compiler limit: initializers nested too deeply`, at
`v_over_29 = ((ceeO_S10OverClause){ .tag = 0 });`, the null of an `OverClause?`. The literal names only the tag. But the
optional holds `OverClause` by value, and that type nests structs and unions (a frame, its bounds, expressions)
deeper than MSVC lets an initializer reach. gcc and clang accept it. CI did not see it: its Windows jobs build programs
with gcc when `CC` is not set, and no corpus program had a deep type.

**MSVC's rule, measured** (cl 19.44 x64, `/std:c11 /O2`; C files made by a script, 2026-11-25). A type nests `n`
aggregates when `n` structs, unions or arrays hold each other by value:

1. An initializer list may reach at most **15** nested aggregates; the 16th is C1054. This counts the object's own
   type, as `T v = { … }` or `(T){ … }`.
2. A **designated** initializer reaches every level of each member it **leaves out** or gives a **brace list**:
   `(T){ .tag = 0 }`, `(S){ .b = 1 }` with a deep member left out, `(S){ .a = 1, .f = {0} }` and
   `(E){ .tag = 2, .u = {0} }` all fail once that member's type nests 15 deep. A union left out reaches through its
   first member only.
3. A member given by an **expression** is not reached: `(S){ .a = 1, .f = x }` and `.f = ((F){0})` are fine at
   300 levels.
4. A list **without designators** does not reach into the members it leaves out: `T v = {0}` and `T v = { 1 }`
   are fine at 300 levels when `T`'s first member is a scalar.
5. **Brace elision** goes as deep as it must: `{0}` for a type whose first member is a struct whose first member is
   a struct, and so on, fails when that chain of first members is 16 aggregates long.

gcc and clang have no such limit. An initializer without designators that leaves members out, `{ 2 }`, would avoid
rule 2, but gcc and clang warn about it (`-Wmissing-field-initializers`, in the backend's `-Wextra`), and `{ 2, {0} }`
gives gcc's `-Wmissing-braces`.

## 2. Decision

1. **A value of a variant without a payload** (an optional's `null`, an enum's unit variant) is `((T){0})` when its
   tag is 0: a list without designators whose first member, the tag, is a scalar (rules 4 and 5). With another tag,
   the literal also names the union's first member, every field given its zero value:
   `((E){ .tag = 1, .u.v_Leaf = { .f__0 = ((ceeT_L1){0}) } })`. The per-type helpers that return `Ok` of a
   `Result<void, E>` (vector push, append, resize, a thread's start) write `{0}` too.
2. **A zero value as an expression** (ZeroValue) is a scalar's zero (`0`, `false`, `NULL`), `((T){0})`, or by 3, the
   zero helper's call. A struct literal gives a field of C state that is an aggregate this value instead of leaving
   it to C's zero-filling. A scalar one is still left out, since leaving out a scalar reaches one level. A literal
   whose every field is zero is the struct's zero value, instead of `{ .f0 = { 0 } }`.
3. **`{0}` reaches at most 8 aggregates by brace elision.** zeroElisionDepth counts the aggregates from a type to its
   first scalar through each first member: 1 for an enum or optional (its tag first), 2 for an array (its struct and
   C array) plus its element, 1 for a struct plus its first field, and 2 for a type the runtime or a C header defines.
   A type for which that is more than 8 gets a **zero helper** after its definition,
   `static inline CEE_UNUSED T ceeZ_<mangled>(void) { T r; ceesafe_mem_set(&r, 0, sizeof r); return r; }`, and its
   zero values and zeroed declarations call it (`T v = ceeZ_…();`). 8 rather than 15 leaves a margin for other
   versions of MSVC. Only a chain of struct first members makes such a type, so programs seldom have one.
4. Every other initializer the backend writes already names each member with an expression or a designated list
   that does (a variant with a payload, `Some`, a constructor `ceeMk_`), or is `{0}` of a fixed type whose first
   member is a scalar (an async frame, a scope, a field-less allocator).

The generated C is the same on every host; only its text changes. At -O2 the new forms cost what the old did
(measured 2026-11-25, Apple clang 21 arm64 and MinGW-w64 gcc 16.2 x64): `{ .tag = 0 }`, `{0}` and the zero helper's
`memset` of a 520-byte optional are one `bzero`/`memset` call with both; a 24-byte unit variant is the same four
stores with clang, and three stores with gcc for both (with the new form gcc leaves the padding unwritten, which C
never promised to zero).

## 3. Alternatives

- **A zeroed `static const` object per type** (`static const T ceeZ_T;`, then `x = ceeZ_T;`): the same machine code
  with clang, but one object per type in every program, and clang's `-Weverything` warns
  (`-Wdefault-const-init-var`). It also gives no tag but 0.
- **`memset` for every aggregate zero:** a statement, not an expression, and MSVC's `/Oi-` (§1.1) makes it a call
  where `{0}` is inline stores.
- **A threshold on the whole type's depth**, rewriting only deep types: two forms of every literal, one of them seen
  only by programs as deep as SQLite's. The forms of §2.1 and §2.2 cost nothing for shallow types.

## 4. Consequences

- The SQLite port's test program builds with cl. `run-pass/adr0247-deep-initializers.cee` (a 20-struct chain held by
  an optional, an enum and a struct, and an 18-struct chain of first members) builds with cl in windows.yml; the
  C of 0.95.6 for it fails with C1054.
- emit-c snapshots change where they held `{ .tag = 0 }` (now `{0}`) and a unit variant of an enum with a payload
  (now naming the union's first member).
- **Not covered:** a static's initializer (§39) that holds a null optional, an empty vector or a C-state field inside
  a designated list, where the member's type nests 15 deep. C requires a constant initializer there, so neither a
  compound literal nor the zero helper can stand in; such a static would need a positional initializer. A struct
  literal also still leaves out a C array member (ADR 0029), which MSVC reaches through its element type. Neither
  occurs in the corpus, the compiler or the SQLite port.
