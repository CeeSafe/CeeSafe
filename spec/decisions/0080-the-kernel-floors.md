# ADR 0080 — The kernel floors: exported and imported C data, constants C checks, configured code, C locals

- Status: **Accepted** by the owner, 2026-11-08: §6's questions 1–3 as recommended ("Yes, as proposed", "Accept
  all"), so question 4's single release too. Raised by CosmoOS's storage port (roadmap 2.0 SA7): the owner, "fix CeeSafe so
  that it will accept the floors". Every SA7 tree (`kernel/block`, `kernel-services/filesystem`, `kernel-services/vfs`,
  `kernel-services/storage`) is CeeSafe except 40 `*_floor.c` files, about 2,800 lines of C. Each construct in them was
  classified on 2026-11-08 against this specification and ceesafec 0.23.0 (§1); small probes were compiled where the
  answer was not plain from the text.
- Scope: grammar.md §2 (`ExternBlock`), type-system.md §28 (§28.1's table, §28.2–§28.4), §30.1, §31.1–§31.3, §33,
  §38, §39; c-backend.md; the parser, checker, NIR lowering, C backend and interpreter; lib/core (`CChar`, `CLong`,
  `AtomicU8`); the corpus; guide/unsafe-and-c.md and guide/kernel.md.

## 1. Context

What keeps each floor C, by kind (constructs; files):

| | Kind | Constructs | Example |
|---|---|---|---|
| 1 | a constant CeeSafe writes, asserted equal to a C macro, enumerator, `sizeof` or `offsetof` | ~110 assert groups; 33 files | `_Static_assert(VNODE_DIR == 2 && EINVAL == 22)` |
| 2 | C data that other C names by symbol: an ops table, an `fs_type`, a sysctl's variable, a header's `extern` global | 16; 11 | `extern struct fs_type ramfs_fs_type;` (bootfs.c), `extern uint32_t g_pool_fault_errors;` |
| 3 | code, fields and functions that exist in some builds only, and the stand-ins for the others | ~60; 16 | `cfs->test_commit_armed` (a `CONFIG_DEBUG` field); `lockdep_is_held` (declared only with `CONFIG_LOCKDEP`); 34 self-test stand-ins the self-test table names in every build |
| 4 | a C object on the stack: a `struct blake2b`, a `wait_entry` for wait.h's loop, a `struct pmm_stats` | 10; 6 | `struct blake2b s; blake2b_init_key(&s, …)` |
| 5 | a C `char` by value | 10; 3 | `cfs_mac_tag(…, char domain, …)` |
| 6 | a C function whose name is a struct tag | 4; 4 | `cosmofs_stats`, `cfs_pace`, `bootarchive_entry` |
| 7 | an array of raw pointers as an imported struct's member | 3; 3 | `struct blkdev *held[12]`, `struct cfs *spare[4]` |
| 8 | C data CeeSafe reads that it cannot declare | 2; 2 | `extern const char *const procnet_names[8];` |
| 9 | an imported struct by value as a parameter | 1; 1 | `cfs_set_extent(…, struct cfs_extent run)` |
| 10 | a byte member read atomically; a C `unsigned long` | 1 each; 1 | `__atomic_load_n(&p->dumpable, …)`; `sched_loadavg(unsigned long out[3])` |

About 75 more constructs need no new feature (§5): ops tables only CeeSafe addresses (`static mut` + `@callback`,
§39.4), self-pointing list heads and wait queues (`here`, or `borrow mut S as *mut T`), `static inline` helpers (§28.2,
ADR 0030), `container_of` (`offsetOf<T>`, §30.1), `kwarn` (`KernelLog`), the readdir callbacks (ADR 0079), and config
switches that a folded C constant macro already gives.

Kind 3 is the one where this ADR reverses an earlier choice. ADR 0043 chose `@cfg` on exports only (its option A) over
conditional items (its option B): code inside a function reads a folded constant (`static DEBUG: bool =
COSMO_CONFIG_DEBUG();`) and is checked in every build. Two things break that here:
- a **member** that only some builds' struct has (`test_commit_armed`, `fbig_injected`) cannot be named in a branch the
  C compiler must still type-check, and a **function** declared only in some builds (`lockdep_is_held`,
  `faultinject_set`) cannot be called there either, even dead;
- the folded constant relies on the C compiler deleting the dead call: a static is a non-`const` C object (§39.5), so
  at `-O0` the call to a function that does not exist in that build is still emitted, and the link fails.

## 2. Options for configured code

| | A. Keep ADR 0043 (floors) | B. Configured items and blocks, checked in every build (proposed) | C. Rust-style `cfg`, checked per build |
|---|---|---|---|
| What | config-only members and calls stay in C floors | `@cfg` on functions, statics, extern declarations and imported fields; a `cfg("X")` block lowered to `#if` | items compiled away before checking |
| Checking | as today | every item and block is checked once, in every build, as if every `cfg` held; only C's `#if` differs | the checker must know or enumerate the build's configuration |
| Floors left | 16 files keep C | none of kind 3 | none |

**B is proposed.** CeeSafe stays one program, checked once; what changes is that C sees `#if` where C needs it. A
configured item may be used only under the same condition (§3.3), which the checker can verify without knowing the
configuration.

## 3. Proposed decision

### 3.1 C constants (kind 1)

In an `extern "C"` block:
- `const NAME: T = v;` declares that the C constant `NAME` (an object-like macro or an enumerator, from the block's
  `@header`) has the value `v`. `T` is an integer type (an imported enum included), `bool`, or `usize`/`isize`; `v` is a
  constant expression (§10). In CeeSafe it is an ordinary constant with that value, usable everywhere a constant is
  (patterns, other constants, the interpreter; an array length is still a literal, §16).
- `@cvalue("expression") const NAME: T = v;` names the C side by a C constant expression instead:
  `@cvalue("sizeof(struct cosmo_stat)") const STAT_SIZE: usize = 144;`,
  `@cvalue("offsetof(struct vnode, mode)")`, `@cvalue("CFS_DVA(0, 1)")`. The string may not contain a line break or
  `"` (N0290); it is emitted only inside the assertion below, so it cannot change what the program does.
- **Lowering.** Every generated C file that uses the constant asserts, once:
  `_Static_assert((NAME) == (CT)(v), "…")` (with the `@cvalue` expression in place of `NAME`), naming the constant
  and its declaration's file and line. A header that disagrees fails to compile; it cannot miscompile.
- `const NAME: T;` with no value imports the C constant as C's (an object-like macro): it is a run-time value only, not
  a constant expression (N0338 in a constant context), and its C spelling is `NAME`. `T` is an integer, `bool`, or a
  `*CChar` (a string macro: `COSMO_VERSION_STRING`). The interpreter cannot run a read of one (a fault).
- **A fix:** a zero-parameter `@constant fn NAME() -> T` in an extern block is emitted `NAME()` today, which does not
  compile for an object-like macro. It stays the function-like form; the object-like form is `const NAME: T;`.

### 3.2 Exported and imported C data (kinds 2 and 8)

- **`@export` and `@export("symbol")` on a `static` or `static mut`** (ADR 0024's "not yet"): the static is C data with
  external linkage under `symbol` (default: its name), declared in `ceesafe_exports.h`. Its type must have a C
  spelling: an integer, `bool`, an atomic (as its integer, §31.1's atomic fields), a raw pointer, an `extern "C" fn`
  type, an imported C type, or an array of these (N0291). The symbol follows §28.4's rules (N0292, N0201).
  - A `static mut` is C's `T symbol = …;`; C may read and write it, so every CeeSafe use stays unsafe (§39.3).
  - A plain `static` is C's `const T symbol = …;`, matching a header's `extern const struct vnode_ops cfs_dir_ops;`. It
    may be of an imported C type when exported (otherwise N0337, §39.3): C cannot write a `const` object, so CeeSafe
    reads it as unchanging. An atomic static is never `const`.
- **Imported statics.** In an `extern "C"` block, `static NAME: T;` and `static mut NAME: T;` declare C data the
  headers define (`extern T NAME;`), with the same types. A `static mut` is used as one (unsafe); a `static` is
  read-only, for `const` C data and for data C changes only atomically (an atomic type). `@cname("symbol")` gives the C
  name when it differs.

### 3.3 Configured code (kind 3)

- **`@cfg("NAME")`**, and **`@cfg("!NAME")`** for "where `NAME` is zero", on: any top-level function (not only an
  `@export`), a static, an extern function, an extern static, and a field of an imported struct or union.
  - A configured function or static is emitted inside `#if NAME` … `#endif` (or `#if !NAME`); an extern declaration's
    prototype likewise; a configured field's `_Generic` and `sizeof` checks (§31.3) likewise.
  - Two functions may share a name if one is `@cfg("X")` and the other `@cfg("!X")` (the configured definition and its
    stand-in): they are one function to CeeSafe callers, must have the same signature (N0201 otherwise), and both
    bodies are checked.
- **The `cfg` block**: `if (cfg("NAME")) { … } else { … }` (`!NAME` too), a statement whose condition is the built-in
  `cfg` with a string literal. It is emitted as `#if NAME` … `#else` … `#endif`. Both blocks are checked. It is a
  statement only, not an expression, and `cfg` elsewhere is N0290.
- **The rule that makes one check enough**: a configured item may be used only where its condition holds: inside a
  function or static with the same `@cfg`, or inside a `cfg` block of that condition (N0350, new). A configured field
  is used under the same rule. So every build's C compiles: what one build lacks is used only inside its `#if`.
- The interpreter runs every `cfg` as false unless `--cfg NAME` is given, and refuses a call to an extern function as
  it does today.

### 3.4 C objects on the stack (kind 4)

- A local `var s: T;` (no initialiser) of an imported C type, or of a C-laid-out struct (§33.1), is a **C local**: C's
  zero-initialised object on the stack (`T s = {0};`), at a fixed place for its scope. It is never moved, copied,
  assigned whole or returned (N0294, as for any C state); it is used through `borrow s`, `borrow mut s`, its fields,
  and `borrow mut s as *mut T`. Its address does not outlive the scope (the borrow checker's usual rule; a raw pointer
  made from it is the unsafe code's obligation, U-rules).
- It is not allowed in an `async` function (its frame moves), nor in a closure's captures (N0294).
- So wait.h's loop is CeeSafe (a lib/kernel `waitEvent` over a condition closure), and `blake2b`, `fi_stats`,
  `cosmofs_stats` and `pmm_stats` are locals.

### 3.5 Smaller items (kinds 5–7, 9, 10)

- **`CChar` by value** (kind 5): a `CChar` parameter, result, local or field is C's `char`. It is an 8-bit integer whose
  signedness is the target's, with no arithmetic: `b as CChar` from `u8` (C's conversion, which every target CeeSafe
  supports does modulo 256) and `c as u8` (the byte) convert; `==` and `!=` compare.
- **`@cname("symbol")` on an extern function** (kind 6): its C name, when its CeeSafe name must differ (a struct of that
  name, `struct cosmofs_stats`). The rules of §28.2 apply to the C name.
- **Raw pointer arrays in imported structs** (kind 7): a field `[*T; N]` or `[*mut T; N]` (and of `extern "C" fn`
  types), for `T *member[N]`: elements are read and assigned, not borrowed (§31.2's rule for pointer members).
  `[*CChar; N]` imports a C string table (with §3.2's imported statics).
- **An imported struct by value as an extern function's parameter** (kind 9): the argument must be a place of that type
  (`Ptr.toBorrow(p)` deref, a field, a C local); C copies it. An `@export` may take one too: its body sees it as a C
  local (§3.4).
- **`AtomicU8`** (kind 10), as `AtomicU32` for a `uint8_t` member (§31.1's atomic fields; `__atomic_*` on a byte).
- **`CLong` and `CULong`** (kind 10): C's `long` and `unsigned long`, integer types of the target's width (64 bits on
  every 64-bit target CeeSafe supports, 32 on Windows), converted with `as` as `usize` is.

### 3.6 What stays C

- **Module export tables** (`EXPORT_SYMBOL`, blk_floor.c's 15): a section, a used attribute and a Mach-O variant;
  ADR 0043 already leaves them to C, and they are a module ABI, not logic.
- **C-to-C invariants** (an on-disk enum equal to a uapi one): they compare two headers and name no CeeSafe constant;
  they belong in the headers.
- **Preprocessor defaults** (`#ifndef PROCFS_COMPILER`): a header line.

The CeeSafe rule (CosmoOS's roadmap 2.0) counts these as the named C floor.

## 4. Consequences

- Of the 40 SA7 floors, all but blk_floor.c's export table can go; the C left is headers and module export lines.
- New diagnostics: N0350 (a configured item used where its condition does not hold). N0290–N0294 and N0337–N0338
  cover the rest.
- `ceesafe_exports.h` gains data declarations and `#if` lines; generated C gains `_Static_assert`s.
- The compiler's own sources must still build with 0.8.0, the seed chain's last compiler (docs/release.md), so none of
  this may be used in `src/`.

## 5. Port-side work that needs nothing new

Recorded so the port does not wait for the release: the 13 ops/kobject/fs tables only CeeSafe addresses, every
self-pointing static (probed 2026-11-08: the emitted C is `{ .next = &ceeS_HEAD, … }`), `page_to_virt`/`virt_to_page`,
`faultinject_should_fail`, `vnode_get`, `container_of`, `kwarn`, the kobject name, `cfs_max_extents`, the
cosmofscrash readdir (ADR 0079), and `fsctl_open`/`blkctl_open` (a C-laid-out CeeSafe struct, §33.1).

## 6. Questions for the owner

1. Configured code as §3.3 (option B: `@cfg` on items and fields, `cfg` blocks, checked once in every build), reversing
   ADR 0043's option A? Recommended: yes.
2. C locals as §3.4 (zero-initialised C objects on the stack, never moved)? Recommended: yes.
3. The rest (§3.1, §3.2, §3.5) as proposed, and §3.6's floor left in C? Recommended: yes.
4. Release it all as ceesafec 0.24.0, then CosmoOS's pin, then the floors ported file by file with the usual
   verification? Recommended: yes.

## 7. Implementation plan (once accepted)

Spec first (grammar.md, type-system.md, c-backend.md), then in this order, each with corpus tests (ffi runs with C
halves, check-fail for every new diagnostic) and the contract suite: §3.1 constants; §3.2 exported and imported
statics; §3.5's small items; §3.4 C locals; §3.3 configured code (an ffi test built twice, the macro set and unset);
the guide; a release. Then CosmoOS: the pin, §5's port-side work, and the floors with §3's features.

## 8. Implementation notes (2026-11-08, ceesafec 0.24.0)

What the release has, and where it differs from §3 (each a narrowing; the floors need nothing more):

- §3.1 as decided. A `const` C checks is an ordinary constant, so it is usable wherever one is, but an array length is
  still a literal (§16). The C assertion is emitted once per program, beside the imported fields' (§31.3). A
  zero-parameter `@constant` extern stays the function-like macro form; `const NAME: T;` is the object-like one.
- §3.2 as decided, except that an exported static may not be an array yet (N0216): C's `T name[N]` is not CeeSafe's
  array struct. No floor exports an array (ramfs's operation tables are CeeSafe's alone). An imported or `const`
  static's address is taken through `uintptr_t` to CeeSafe's pointer type, as for a C array member.
- §3.3, narrowed to what C needs: `@cfg` (and `@cfg("!X")`) on exports, on extern functions and on imported fields,
  and `cfg` blocks. `@cfg` on other functions and on statics, and a pair of functions sharing a name, are not needed: a
  stand-in is the export's `else` block, and "A and B" is a nested block. N0350 (not N0349, which task sets have)
  applies to configured C functions and fields; an export's own `@cfg` does not open its body to them, since ADR
  0043 compiles that body in every build. `cfg` as a value is N0200, an unknown function, not N0290. The structurer
  emits the blocks as `#if`; a block left unstructured is wrapped in `#if` with its label kept. The interpreter takes
  every macro as 0.
- §3.4 as decided: `var s: T;` zero-fills with `s = (T){0};` where it is declared (no `memset`, so no library is
  needed). A closure may borrow one as any local; a move of it, a moving capture included, is the in-place checker's
  N0294.
- §3.5: `CChar`, `@cname`, pointer arrays, `AtomicU8` (load, store, swap, compareExchange and the ordered load and
  store: what C's byte flags need) and `CLong`/`CULong` (asserted `sizeof(long)` where used) as decided. **An
  imported struct by value as a parameter is not implemented.** Its one use, `cfs_set_extent`, is an internal
  cosmofs function that only a C host test calls with a struct; CosmoOS's port changes it to take a pointer.
- Corpus: ffi `c-data`, ffi `cfg-code`, check-fail `c-data-errors` and `cfg-code-errors`. Two expectations changed
  where this ADR allows what they refused (`ffi-declarations.cee:17`, a `CChar` parameter; `c-arrays.cee:22`, an
  array of pointers), and three HIR snapshots' closure ids moved with lib/core.
