# ADR 0079 — Imported C enums

- Status: **Accepted** by the owner, 2026-11-08 ("Go ahead and fix CeeSafe"). Raised by CosmoOS's VFS port (roadmap 2.0
  SA7): two calls through function pointers whose C type takes an `enum vnode_type`, which UBSan's function check
  refused in CI (CosmoOS run 42469167908, both arches).
- Scope: grammar.md §2 (`ExternEnum`), type-system.md §31.6, the parser, the formatter, the AST printer, the checker
  (declaration, scope, FFI rules), the type table, the C backend (spelling, mangling, the layout assertion), the
  corpus, guide/ffi.md.

## 1. Context

- A C header can type a parameter, a result or a member with an enum: `int (*mknod)(struct vnode *, …, enum
  vnode_type, …)`. CeeSafe has no spelling for that type. A port writes the enum's integer (`u32`) instead.
- C calls `enum vnode_type` and `unsigned int` compatible types (C11 §6.7.2.2 ¶4), so a prototype or a direct call
  that mixes them compiles without a warning. A call **through a function pointer** whose type says `enum vnode_type`,
  to a function defined with `uint32_t`, is still a call through a pointer of another type. Clang's
  `-fsanitize=function`, part of `-fsanitize=undefined` on Linux, refuses it at run time:
  `call to function ramfs_mknod through pointer to incorrect function type`. Two places in CosmoOS:
  1. ramfs's operation table pointed `mknod` at ramfs.cee's export, which takes a `uint32_t`.
  2. vfs_dir.cee called `ops->mknod` through a field it declared with a `u32` parameter, reaching C filesystems
     whose `mknod` takes the enum.
- The same gap made every filesystem's readdir callback (`vfs_dirent_cb`, whose last parameter is `enum vnode_type`)
  a C trampoline in a floor: ramfs, procfs, cosmofs and the VFS each have one.
- Today's workaround is C: a wrapper with the enum-typed signature that calls the CeeSafe function with an integer.
  The CeeSafe rule counts that as new C (CosmoOS's share gate failed on it, 9138b975).

## 2. Decision

1. **Declaration.** Inside an `extern "C"` block, `enum name: I;` imports a C enum type, where `I` is one of `u8`,
   `u16`, `u32`, `u64`, `i8`, `i16`, `i32`, `i64`: the integer type the C compiler gives the enum.
   - Its C spelling is `enum name`. With `@ctype("cname")` it is `cname`, for a typedef
     (`typedef enum {…} cname;`). `name` and `cname` must be valid, unreserved C identifiers (N0292).
   - A block that declares one needs a `@header`, as for an imported struct (N0290). `@ctype` is its one attribute
     (N0290 for any other). The integer must be one of the eight (N0291 otherwise: `usize`, `isize`, `bool` and
     every non-integer type are refused, since C does not give an enum any of them).
   - Visibility is that of a struct. The name is declared in the item scope like a struct's, so it cannot be declared
     twice (N0228 / "already defined").
   - Enumerators are not imported. A value is written as an integer constant of the type,
     `const VNODE_DIR: vnode_type = 2;`, which the C side asserts as it asserts any constant a port copies.
2. **The type is an integer type of its own.** `vnode_type` is an integer of `I`'s width and signedness, distinct from
   `I` and from every other type: what holds of integers holds of it (literals, arithmetic, comparison, `checked`,
   `wrapping` and `saturating` operations, `match` on integer patterns), and a value of one integer type is not a value
   of another without a conversion.
   - `as` converts between it and any integer type under §13's rules: to and from `I`, and to a wider integer, are
     lossless; anything else is `truncate`, `checkedTruncate` or `saturatingTruncate`.
   - It is `Copy`, has no `drop`, and may be a local, a parameter, a field (of a CeeSafe struct or an imported one),
     an array element or a type argument, as an integer may.
3. **Across the boundary** it is FFI-safe wherever `I` is (§28.3): in `extern "C"` prototypes, `@export` signatures,
   `@callback` functions, `extern "C" fn` types and imported struct fields. There it is spelled with its C spelling,
   so a function pointer type, a prototype and a definition all say `enum vnode_type`.
4. **Lowering** (§31.3).
   - Every generated C file that uses the type asserts it once, before its first use:
     `_Static_assert(sizeof(enum name) == sizeof(CI) && _Generic((enum name)0, CI: 1, default: 0), "…")`, where `CI`
     is `I`'s C type (`uint32_t`). C's compatible-type rule makes `_Generic` match exactly when the compiler's
     integer for the enum is `CI`; a header whose enum is another size or signedness fails to compile.
   - Its mangled name is its own (`N`, the length, the name), so generic instances over it never share a C name with
     ones over `I`.
   - The interpreter runs it as an integer of `I`'s width.

## 3. Consequences

- CosmoOS's `mknod` wrappers and its four readdir trampolines can be CeeSafe: an export typed with the enum is the
  table's entry itself, and a callback type with the enum is called directly.
- Nothing else changes: an `enum` in an `extern "C"` block was a syntax error before.

## 4. Rejected

- **Letting `u32` stand for any enum whose compatible type is `unsigned int`.** That is what failed: C's rule for
  compatible types and UBSan's function check disagree, and only the second is enforced at run time.
- **Importing the enumerators** (`enum vnode_type: u32 { REG = 1, DIR = 2 }`). They would be a second copy of the
  header's values with no way for CeeSafe to check them. Constants of the type, asserted by C, already do this.
- **A new kind of type** beside integers. It would need every rule that holds of integers restated for it; the C
  spelling is the only difference that matters.
- **A type alias** (`type vnode_type = u32;`). CeeSafe has none (§13), and an alias would still be spelled `uint32_t`.

## 5. Amendment (2026-11-10): the integer is the target's

Running the corpus natively on Windows (ADR 0069) showed three gaps. The rule of §2.1 stands: `I` is the integer the C
compiler gives the enum, and a header that disagrees fails to compile.

1. **The integer depends on the target.** C11 §6.7.2.2 ¶4 leaves an enum's integer to the implementation. GCC and
   Clang on Linux and macOS give an enum whose enumerators are all non-negative `unsigned int`, and one with a negative
   enumerator `int`; MSVC, and Clang targeting Windows, give every enum `int`. Values of the type are C's enum in the
   generated code, so the assertion of the width and the signedness is what keeps C's arithmetic and comparisons on
   them CeeSafe's; a width-only check was rejected for that reason. A declaration fits every target only where the
   header's enum has the same integer on all of them, as one with a negative enumerator does (`i32`). The corpus's
   headers do so (`ffi/imported-enums`, `ffi/c-data`); type-system.md §31.6 and guide/unsafe-and-c.md say it.
2. **The assertion states the width and the signedness, not the C type.** MSVC makes no enum compatible with any
   integer type, so §2.4's `_Generic((enum name)0, CI: 1, default: 0)` fails there for every enum. The assertion is
   now `sizeof(enum name) == sizeof(CI) && (enum name)-1 < (enum name)1` for a signed `I` (`>` for an unsigned one):
   what CeeSafe's integer rules depend on, on every compiler. It also accepts an enum whose C type is another integer
   of the same width and signedness (C's `char` for `i8` where `char` is signed, `long long` for `i64` where `int64_t`
   is `long`), which behave alike.
3. **A member's check is spelled through its address.** In `_Generic(m, enum color: 1, default: 0)` Clang targeting
   Windows reads `enum color:` as an enum with a fixed underlying type (a Microsoft extension) and fails. A member
   whose C type is spelled `enum name` is asserted as `_Generic(&m, enum name *: 1, default: 0)` (type-system.md
   §31.3).
