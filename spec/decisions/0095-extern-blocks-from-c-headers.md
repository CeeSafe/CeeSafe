# ADR 0095 — `extern "C"` blocks generated from C headers

- Status: **Accepted** by the owner, 2026-11-11 ("Accept ADR 0095 as proposed, one file per header"). Not
  implemented. Raised by CosmoOS's roadmap 1.3 (its row C1, `docs/roadmap-1.3.md`): each port
  copies the C declarations it uses by hand.
- Scope: a tool, `tools/cbind.py`, shipped with a release, and its tests; `guide/unsafe-and-c.md`. No language,
  checker or emitter change.

## 1. Context

- **What a port writes today.** `kernel/syscall/sockets.cee` in CosmoOS (2026-11-11) is 924 lines as formatted. Its
  `extern "C"` block is 121 of them: 11 imported structs, 36 constants and 38 function prototypes, each copied from
  a header. `compat/linux/lxsock.cee` repeats most of them. The other ports of that day have the same shape.
- **What copying costs.**
  1. Time: the block is written before any code, and extended at every compile error.
  2. Values typed by hand. `const EINVAL: i32 = 22;` is checked: the generated C asserts it equal to C's (ADR 0080
     §3.1). But the number is still looked up and typed, and a wrong one costs a build to find.
  3. Drift between ports. Each port declares the fields of `struct process` it happens to use; ten ports have ten
     partial declarations of one struct, in ten spellings.
  4. Features missed. On 2026-11-11 a day of ports declared C structs opaque and held them in word arrays, when
     `var s: T;` (a C local) was available: the hand-written blocks hid that the struct could simply be declared.
- **What exists.** `integrations/cosmoos/c2cee.py` reads clang's AST (`-Xclang -ast-dump=json`) to analyse one C file
  and draft its port; its type table (`uint32_t` to `u32`, …) and its walk of the AST are what a generator needs.
  ADR 0081 lets one package own a C declaration that others use. Several `.cee` files compile as one unit.
- **Why not in the language** (`@import("kernel/handle.h")`): `ceesafec check` would need a C compiler, the kernel's
  include paths and its configuration to type-check a file, and a CeeSafe program's meaning would depend on them.
  Today `check` needs neither, and the C compiler's part is one assertion per declaration. That is worth keeping.

## 2. Proposal

### 2.1 The tool

```sh
python3 tools/cbind.py --header kernel/handle.h -I kernel/include [-D CONFIG_DEBUG=1 …] [--target aarch64-none-elf] \
    [--only handle_lookup,handle_install,HANDLE_RIGHT_READ] [--config NAME=flags …] -o handle.cee
```

It runs clang's front end on a file that includes the header, and writes a `.cee` file holding one `extern "C"`
block with `@header("kernel/handle.h")`. Nothing is compiled or run, so it works for a target the host cannot
execute.

### 2.2 What it emits

| C | CeeSafe |
|---|---|
| a function declaration, `static inline` included (ADR 0030) | `fn name(p: T, …) -> R;`; variadic as `...` |
| a struct with only representable members | `struct name { field: T; … }`, every member, in order |
| a struct with a bit-field, an anonymous union or struct, or a member of an unrepresentable type | `struct name;` (opaque), with a comment naming the member that made it so |
| a union | `struct name;` (opaque) |
| an enum | `enum name: uN;` and a `const` per enumerator |
| a typedef of a function pointer | `@ctype("name") struct name;` |
| an object-like macro whose value is an integer constant expression | `const NAME: T = value;`, `T` the expression's C type |
| an object-like macro that is a string literal | `const NAME: *CChar;` |
| `extern T name;` | `static name: T;` (`static mut` unless `const`) |
| a member or name that is a CeeSafe keyword | `@cname("type") type_: …` |

Types map as c2cee's table does: fixed-width integers, `size_t`/`uintptr_t` to `usize`, `bool`, `char *` to
`*mut CChar`, `const T *` to `*T`, `T *` to `*mut T`, `void *` to `*mut void`, a function pointer member to
`extern "C" fn(…) -> R`, `T name[N]` to `[T; N]`. A type the table has no entry for makes its declaration opaque
or, for a function, leaves the function out with a comment: the tool never guesses.

**Macro values without running anything.** The preprocessor lists the header's macros (`clang -E -dM`). For each
object-like macro the tool writes `enum { cee_v_NAME = (NAME) };` into a second file and reads the enumerator's
value and type from clang's AST. A macro that is not an integer constant expression fails there, alone, and is left
out. Function-like macros are left out.

### 2.3 Which declarations

Those whose definition is in the named header, not in the headers it includes. A type from another header that a
declaration mentions is emitted opaque (`struct kobject;`) unless `--with` names its header's generated file, in
which case it is left to that file. `--only` narrows the output to the listed names and what they mention.

### 2.4 One output for every configuration

A kernel header differs by architecture and build. CosmoOS generates its C once for all four (`out/cee`), so a
binding cannot be one configuration's. With `--config x86_64-debug="…" --config aarch64-release="…"`, the tool
runs once per configuration and emits what is identical in all of them. What differs is not emitted; it is listed at
the end of the file as a comment (`// differs: struct arch_user_regs (members), ARCH_HAS_SMAP (absent in aarch64-*)`),
and is written by hand with `@cfg`, as today.

### 2.5 How a tree uses it

The generated files are committed, like any source, in a directory of their own (CosmoOS: `kernel/bind/handle.cee`).
A port names the ones it needs as extra sources, and they compile as one unit with it. A CI job regenerates them and
fails on a difference, so a header change that the bindings do not follow fails before any port does.

Committed rather than built: `ceesafec check` keeps needing no C compiler, a reviewer sees a header's change in the
binding's diff, and a release tarball builds without clang's JSON AST (gcc has none).

A port's own block shrinks to what the tool leaves out: the differing declarations, `@cvalue` layout checks it still
wants, and nothing else.

### 2.6 What it is not

- Not part of `ceesafec`: it is Python, as `c2cee.py` is, and needs clang. The compiler's algorithms stay in CeeSafe;
  this is a generator of source.
- Not a translator of C code (that is `c2cee port`).
- Not a judge of safety: every function it declares is as unsafe to call as a hand-written declaration.

## 3. Alternatives

- **`@import` in the language.** §1's last point.
- **Extend `c2cee.py`.** It lives in `integrations/cosmoos` and knows CosmoOS's tree. Declarations from headers are
  wanted by any C project (the Linux module of `integrations/linux` hand-writes them too), so the generator belongs
  in `tools/` and c2cee calls it.
- **Generate at build time, not committed.** Every build and every `check` in an editor would need clang and the
  include paths.
- **Whole-struct opacity as the default.** Safer but it is today's cost: the fields are what ports use.

## 4. Risks

- **One namespace.** Files compiled as one unit share names, so two generated files that both declare
  `struct kobject;` collide (N0201). §2.3's `--with` is the answer; it has to be right for the tree's header graph.
- **A full struct declares members no port uses**, and each is asserted in every generated C file that includes the
  binding: more assertions to compile. To be measured (§5).
- **clang only.** The tool needs clang's JSON AST. CosmoOS builds with clang; a gcc-only tree would run the tool
  elsewhere and commit the result.

## 5. Tests proposed (none run)

- `tools/cbind_check.py` in the contract suite: a fixture header with one of each row of §2.2, the expected `.cee`,
  and the output compiled by `ceesafec` and by the C compiler against the header (so every assertion holds).
- Two configurations of the fixture that differ in one struct and one macro: the differing names are listed, not
  emitted.
- CosmoOS, as the proof: `sockets.cee`'s and `lxsock.cee`'s blocks replaced by generated files for `handle.h`,
  `socket.h`, `unix.h`, `errno.h` and `uaccess.h`; the kernel's matrix; the build time of `make cee` and of the
  kernel before and after.

## 6. Decisions of the owner (2026-11-11)

1. The tool as proposed: Python in `tools/`, clang's AST, committed output with a CI freshness check.
2. §2.4 as proposed: emit only what is identical in every configuration; the rest is written by hand with `@cfg`.
3. One binding file per header.
