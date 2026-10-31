# C Backend Design (v0)

Status: Milestone 0 proposal. Applies from M1. CosmoOS-specific flags are covered in
`cosmoos/abi.md` and `cosmoos/build-integration.md`.

## 1. Target dialect

- **C11** (ISO/IEC 9899:2011), compiled with `-std=c11` (or `gnu11` where the
  host project requires it). Generated code must also compile under C17.
- Headers used by generated code: **only** freestanding headers (`<stdint.h>`,
  `<stddef.h>`, `<stdbool.h>`, `<stdalign.h>`, `<limits.h>`, `<stdnoreturn.h>`)
  plus `ceesafe/rt.h`. Everything hosted lives behind runtime functions.
- Compiler-specific features are allowed only through macros in `ceesafe/rt.h`
  with a portable fallback: `__builtin_*_overflow`, `__builtin_expect`,
  `__attribute__((noreturn, cold))`, `_Static_assert`.
- Supported compilers: clang ≥ 15 (CosmoOS's floor; its CI uses Debian clang 19.1.7)
  and gcc ≥ 11 for hosted builds. **CosmoOS profiles are clang/lld only**
  (`research/cosmoos-platform-audit.md` §3). MSVC is best-effort for the hosted profile
  (C11 mode, VS 2022), without the kernel profile.
- The generated C must compile warning-free under
  `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror`
  **and** under CosmoOS's own warning set (`-Wcast-qual -Wmissing-prototypes
  -Wstrict-prototypes -Wvla -Wundef -Wimplicit-fallthrough`, `-Wframe-larger-than=2048`
  for the kernel; see `cosmoos/abi.md`). All non-exported functions are `static`, and every
  exported one has a prototype in the generated header.
- Correct both with and without `-fno-strict-aliasing`. CosmoOS compiles with it,
  but CeeSafe does not rely on it.
- **No floating-point contraction or fast-math** (M11 G7, type-system.md §35.6). `rt.h` turns contraction off where
  a pragma can (clang, MSVC) and requires `FLT_EVAL_METHOD == 0`, in programs that use floats (`CEE_FLOATS`; a
  program without floats compiles under `-mgeneral-regs-only`); builds pass `-ffp-contract=off` to clang and gcc
  and never `-ffast-math` or its parts, so each float operation rounds once, as in the interpreter.
- **No compiler-rt / libgcc.** CosmoOS links no builtins library, so generated code
  must never need one. That rules out `__int128` division, `long double` arithmetic,
  16-byte atomics and, in AArch64 userland, outline-atomic helpers (to be verified in C1;
  pass `-mno-outline-atomics` if needed). The runtime header supplies portable
  fallbacks instead of builtins when a builtin would lower to a libcall.
- `-nostdinc` builds: freestanding headers come from `-isystem $(clang
  -print-resource-dir)/include` (CosmoOS invariant B2). Generated code must not include libc headers
  directly; only `ceesafe/rt*.h` does, and only in hosted and userland profiles.
- **Runtime per profile (M8c, `profiles.md` §4.2).** Hosted programs get `ceesafe/rt.h`, the
  core runtime `ceesafe_rt_core.c`, and the platform layer `ceesafe_rt_hosted.c` / `ceesafe_rt_io.c` (C library
  calls). A hosted program that prints a float also gets `ceesafe_rt_float.c`, the float formatter and its table
  (type-system.md §35.7; M11 G7b). Freestanding programs get `ceesafe/rt.h` and `ceesafe_rt_core.c` compiled with `CEE_FREESTANDING`
  (a `#define` at its top), which leaves out everything that needs a platform service; the
  environment defines `ceesafe_panic`. Freestanding builds add `-ffreestanding -fno-stack-protector
  -fno-asynchronous-unwind-tables -fno-unwind-tables` (the stack protector's `__stack_chk_*` symbols
  come from the C library; unwind tables are useless with panic = abort and have no unwinder to read
  them in a kernel).
- Plain C `char` is **never** used for CeeSafe data (`char` is signed on x86-64 and
  unsigned on AArch64; CosmoOS's `limits.h` is also wrong about it on AArch64). Bytes are `uint8_t`.

## 1.1 MSVC, best effort (M7e)

`ceesafec build --cc cl` uses it, and so does `ceesafec build` on Windows when no clang or gcc is found (`tooling.md` §8).
The hosted profile also builds with Microsoft's C compiler: `cl /std:c11 /W3 /WX`, checked on
x64 (CI) and ARM64 (a Windows 11 VM). Programs are linked with an 8 MiB stack
(`/link /STACK:8388608`), as Linux and macOS give the main thread. Windows' default is 1 MiB, which
`run-pass/deep-recursion.cee` overflows. This is
**best effort**: no CosmoOS profile uses MSVC, and the strict warning set (§1) is checked with clang and
GCC only. What differs:

- **`rt.h`** uses compiler builtins only behind a test for GCC or clang, with portable code otherwise:
  - checked arithmetic falls back to the portable overflow checks, as with `CEE_NO_BUILTIN_OVERFLOW`;
  - counting leading zeros (stable vectors) uses `cee_clz64`, a shift loop off GCC and clang;
  - `CEE_UNUSED` and `CEE_COLD` are empty;
  - `cee_max_align_t` stands for C11's `max_align_t`, which MSVC's `<stddef.h>` lacks in C mode.
    There it is `double`. The generated alignment assertions use it.
- **Process behaviour** (`ceesafe_rt_hosted.c`, under `_WIN32`):
  - standard output and standard error are switched to binary mode at the first write, so
    `"\n"` stays one byte;
  - a panic ends with exit status 134 (`_exit(134)` after flushing), the status `abort()` gives
    on Linux and macOS. Windows' own `abort()` would give 3.
- **I/O** (`ceesafe_rt_io.c`, under `_WIN32`):
  - program arguments work as elsewhere;
  - files use the C runtime's `_open`, `_read`, `_write`, `_close` and `_unlink` in binary mode;
  - `Process.run` uses `_spawnvp(_P_WAIT, …)`;
  - **TCP is not supported**: after the same address check as elsewhere, every socket call fails
    with `ENOSYS` (`IoError.Other`).
- **Test.** Nova's `MsvcTests` checks three things (CeeSafe's Windows checks are `tools/host_c_check.py` and the run corpus under `--cc cl`):
  - every run-pass and run-fail program builds with MSVC and gives the corpus expectations;
  - `nwc` reads files and standard input;
  - a program runs `cmd /c exit 7` with `Process.run`, sees a missing program fail, and sees TCP
    reported as unsupported.

  No corpus program needs an exception: `io-errors.cee` passes only invalid addresses, which fail
  the address check first. The `test-windows` CI job runs the tests on x64. Locally, a Windows 11
  ARM64 VM (MSVC 19.51) runs them. On other hosts they return early.

## 1.2 The runtime ABI version

The runtime and the C that `ceesafec` generates are versioned together, and nothing between them is stable across
versions. `ceesafe/rt.h` defines the version right after its include guard:

```c
#define CEESAFE_ABI_VERSION 1
```

and every generated unit that includes it (`main.c`, and each unit of the split form, §2.4) checks it on the three
lines after the `#include`:

```c
#if CEESAFE_ABI_VERSION != 1
#error "this C was generated for CeeSafe's runtime ABI version 1"
#endif
```

so C generated by one `ceesafec` and compiled against another's runtime fails to compile instead of linking wrong
code. A change to the runtime's interface (a helper's name, signature or meaning, a layout the generated code
relies on) raises the version. Version 1 is the fork's (Nova `v0.6.0`'s runtime under CeeSafe's names).

Nova's runtime and generated names (`nova_*`, `nv*`, `NV_*`, `NOVA_*`) and CeeSafe's (§2.1) are disjoint, so one
program may link C generated by novac and by `ceesafec`, each with its own runtime.

## 2. Lowering of types

| CeeSafe | C |
|---|---|
| `i8..i64`, `u8..u64` | `int8_t..int64_t`, `uint8_t..uint64_t` |
| `isize`/`usize` | the fixed-width type of the target's pointer width (`int64_t`/`uint64_t` on LP64), plus `_Static_assert(sizeof(void *) == 8)`. `ptrdiff_t`/`size_t` are used at FFI boundaries (M8) |
| `bool` | `bool` (`_Bool`) |
| `char` | `uint32_t` |
| `borrow fn(T…) -> R` (M11 G8b) | `const ceeCK_<mangle> *`, a pointer to the first member, the code pointer `R (*)(const void *, T…)`, of an environment `ceeCE_<id>` that holds a pointer per capture; the body is `ceeCB_<id>`, which converts `env` back through `uintptr_t`; a call is `(*f)(f, args…)` (type-system.md §36.2) |
| `borrow dyn I`, `borrow mut dyn I`, `Box<dyn I, A>` (M13 G2) | `ceeDR_<D…>` = `struct { void *data; const ceeDT_<D…> *vt; }` (16 bytes; the dyn type mangles as `D<length><interface><arguments>`, `dyn Shape` → `D5Shape0`), typedef'd before every struct, so any struct may hold one. Not a niche optional: `Box<dyn I>?` has a tag. The table type `ceeDT_…` is defined after the structs: `drop` (NULL if `T` needs none), `size`, `align`, then `m0…` in the interface's method order, each taking `void *` for the receiver. Per (`T`, `I`) pair that a coercion names: `static const ceeDT_… ceeVT_<T>_<D…>`, with adapters `ceeVT_…_<slot>` that convert the address back through `uintptr_t` and call `T`'s method, and `ceeVT_…_drop` over `T`'s drop glue. A coercion is `((ceeDR_…){ .data = (void *)(uintptr_t)p, .vt = &ceeVT_… })`; a call through `x` is `x.vt->m<slot>(x.data, args…)`; borrowing `*x` is `x` itself. `Box<dyn I, A>`'s drop glue calls `vt->drop` and frees `data` with `A` (a user allocator's `deallocate` gets the table's size and alignment) (type-system.md §37.4). **`dyn fn`** (M13 G3, §36.5) mangles as `DF` (`DG` for `dyn mut fn`), its effect list and its signature; its table's one entry `m0` is a move literal's body, whose `self` is the boxed environment `ceeCE_<id>` (the code pointer, then the captured values; drop glue `ceeD_CE<id>_`) |
| `fn(T…) -> R` (M11 G8a) | a typedef `ceeN_<mangle>` of a pointer to the functions' C signature; a value is a function's address, a call an indirect call (type-system.md §36.1) |
| `f32`/`f64` | `float`/`double` (`rt.h` checks the IEEE formats; literals by their bits through `cee_f32_from_bits`/`cee_f64_from_bits`; float→integer conversions through range-checked helpers; type-system.md §35). The CosmoOS kernel forbids floats (`profiles.md`; `--no-float` in G7c) |
| `struct S` | `typedef struct ceeT_S ceeT_S;` then `struct ceeT_S { … }` with fields `f_<name>` in declaration order, structs emitted after the structs they contain. Each struct is followed by `_Static_assert`s on `sizeof`, `_Alignof` and every field's `offsetof`, against `TypeTable.LayoutOf`. An empty struct gets a `uint8_t cee_empty` member (C has no empty structs). `@repr(C)` is the default in v0; field reordering is an M9 opt-in (`@repr(auto)`) |
| `Box<T, A>` with a user allocator (C5, type-system §30) | `T *` as below; `ceeB_` calls `A`'s `allocate` method with `{ sizeof(T), _Alignof(T) }` (a `{ 0 }` value of the field-less allocator struct as `this`), and the drop glue and `BoxFree` call `ceeBF_<box>`, which calls `deallocate` with the same layout |
| `Box<T, A>` (M3b) | `T *` (a shared reference to a box is `T *const *`). Drop glue `ceeD_X<T><A>` destroys `*p`, then calls `ceesafe_free(p, CEE_ALLOC_SYSTEM \| CEE_ALLOC_FAILING)`. `Box.create`/`createIn` call a per-type helper `ceeB_X<T><A>(T v)`: `ceesafe_alloc(sizeof(T), id)`; on NULL it destroys `v` and returns `Err(OutOfMemory)`, otherwise it stores `v` and returns `Ok(p)`. It asserts `_Alignof(T) <= _Alignof(max_align_t)`. **Built in place** (M9e′): when the argument is a temporary assigned by the statement just before, by a construction (an array repeat, an array, a struct or enum aggregate), and used nowhere else, and `T` needs no drop and holds no C state, the site calls `ceeBA_X<T><A>()` (the allocation alone: `p` or NULL) and then writes the construction into `*p`, with no temporary and no by-value copy. When the construction is an array repeat of a constant whose bytes are all zero (an integer or `bool` 0, a float +0.0) and `A` is a heap allocator rather than a user-defined one (§30), the site calls `ceeBZ_X<T><A>()` instead, which returns `ceesafe_alloc_zeroed(sizeof(T), id)` (counted and fault-injected like `ceesafe_alloc`; the hosted platform's `calloc`), and writes nothing: a C library can skip zeroing memory that already is. The construction's operands are already evaluated, so it cannot panic or allocate, and the order of allocation and construction is unobservable; `T` has no drop, so a failed allocation leaves nothing to destroy. A stack-placed box (§2.2) is likewise built in its slot |
| Deep boxes (M3c) | If destroying `T` ends, as its last step, in a `Box<T>` (followed through struct fields and enum variants' last payloads), the glue of `Box<T>` is a loop: destroy the node except that tail (`next = tail pointer`), `ceesafe_free` the node, continue with `next` (type-system.md §18.5). `Unpack` becomes one assignment per target; `BoxFree` is `ceesafe_free` |
| `Arena` (M4a) | `cee_arena` from `ceesafe/rt.h` (40 bytes, asserted there), created by `ceesafe_arena_create(allocator, chunk)`, dropped by `ceesafe_arena_drop`. A shared reference to a region is a plain `cee_arena *`: allocation mutates the arena through a shared borrow. `alloc`/`allocArray` call per-type helpers `ceeAA_<T>`/`ceeAR_<T>`, which bump-allocate with `ceesafe_arena_alloc` (chunks come from `ceesafe_alloc`, so they are counted and fault-injectable) and return the scoped result. Element names: `P`/`Q` + T for shared/mutable references, `U` + T for `[T]`, `G<len><Name>` for regions |
| `FixedBufferAllocator`, `ScratchFrame` (M4b) | `cee_fba` (24 bytes) and `cee_scratch` (32 bytes) from `ceesafe/rt.h`. Allocation helpers take `(void *region, uint32_t kind, …)` and call `ceesafe_region_alloc_fast`, an inline function of `ceesafe/rt.h` that bumps an arena's pointer within its current chunk itself and otherwise calls `ceesafe_region_alloc`, which dispatches to the arena, the fixed buffer, or the runtime's scratch stack (heap chunks via `ceesafe_alloc`; a frame's drop frees the chunks newer than its entry state). `Scratch.enter()` is `ceesafe_scratch_enter_root(loc)`, which panics if a frame is live; `frame.enter()` is `ceesafe_scratch_enter_child`. A frame's borrow-checker parts (`memory`, `allocation`) are places of the frame itself in C |
| `Pool<T>`, `PoolBox<T>` (M4c) | `cee_pool` (48 bytes; `ceesafe_pool_create`/`ceesafe_pool_take`/`ceesafe_pool_release`/`ceesafe_pool_drop`; slots of one size, allocated at the first take, with a free list through them). `PoolBox<T>` is `ceePB_<T>` = `struct { T *ptr; cee_pool *pool; }`, and its contents are `(*b.ptr)`. Its drop glue `ceeD_Y<T>` destroys `*ptr`, then releases the slot; `BoxFree` of a partially moved pool box releases only. `pool.take` calls `ceePT_<T>(pool, v)`, which destroys `v` when no slot is free |
| Interfaces (M6b) | No runtime representation: dispatch is static. An `impl` block's methods are methods of their type, named like other members (`impl Comparable<i32> for i32` → `ceeM_3i32_compare`). Calls through a bound in generic code exist only in generic versions, which are not emitted; each instantiation calls the concrete method |
| Generic code (M6a) | Generic versions are type-, move- and borrow-checked but never emitted. Each instantiation is a separate C type or function. Struct and enum instances are `ceeT_`/`ceeE_` + `<len><Base><count><args>`, using the element codes (`Pair<i32>` → `ceeT_4Pair1i32`; `S4Pair1i32` inside other names). Generic function instances are `ceeG_<len><name><count><args>` (`id<Pair<i32>>` → `ceeG_2id1S4Pair1i32`). Members of an instance are `ceeM_<mangled owner>_<name>`. Every non-generic form starts with a letter or a length, so the forms cannot collide |
| `Result<T, E>` (M3a) | like an enum, named `ceeR_<T><E>` with the element codes below (`Result<Res, Code>` → `ceeR_S3ResE4Code`, `void` → `v`). Its drop glue is `ceeD_R<T><E>`. `e!` on `Err` calls `ceesafe_panic(CEE_PANIC_UNWRAP, loc)` |
| `enum E { A, B(T) }` | `struct { uint32_t tag; union { T B; } u; }`. The tag width is the smallest unsigned that fits, unless `@repr(u8)` etc. A value without a payload is `((E){ .tag = n })`. A value with a payload of scalars and field-less enums is a compound literal too. A variant whose payload may hold a pointer (anything else) has a constructor `static inline ceeMk_<E>_v_<variant>(payload…)`, defined after the type, which zero-initialises the value and stores the tag and each payload field; its values are built by it, in the helpers too. Clang's static analyzer loses a pointer read back from a union built by a compound literal, so a `Box`'s allocation looked leaked (M14, ADR 0025 §7). The constructors are kept off scalar payloads because gcc inlined less around them (`string-building` ran 1.8× slower with one on its cold growth path, inferred from inlining). Static initialisers (§39) keep compound literals, which C requires there |
| `T?` (M6c) | like an enum with variants `null` (tag 0) and `Some` (tag 1, field `f__0`), named `ceeO_<T>` with the element codes (`i32?` → `ceeO_i32`; `O<T>` inside other names). A niche optional (type-system §22.1, M9e′) is `struct ceeO_<T> { T f__0; }`: its tag is `(o.f__0 != NULL)`, `Some`'s payload is `o.f__0`, and `null` is `{ .f__0 = NULL }`; drop glue `ceeD_O<T>`. `o!` on `null` calls `ceesafe_panic(CEE_PANIC_UNWRAP_NULL, loc)`; `o == null` compares the tag with 0. The niche layout for `Box` and references is future work (type-system.md §22.1) |
| `Vector<T, A>`, `String<A>` (M6d) | `ceeV_<[T] or Z><A>` = `struct { T *ptr; uint64_t len; uint64_t cap; }` (24 bytes, asserted), element code `V<U<T> or Z><A>` (`Vector<i32>` → `ceeV_Ui32S10SystemHeap`). An empty vector points to the runtime's `ceesafe_vec_empty` (a `max_align_t` object, never dereferenced) with `cap == 0`, so slices of it never do arithmetic on NULL. Growth is `ceesafe_vec_grow` (doubling, at least 4; a first buffer is allocated, a later one resized with `ceesafe_realloc`, which keeps the old bytes; NULL with nothing changed on overflow or allocation failure). Push and append call it only when the buffer is full, and pass the capacity through a local, so the vector's address does not escape the helper and the C compiler can keep its length in a register. Per type: `ceeVP_` push (destroys the value on failure), `ceeVO_` pop, `ceeVC_` clear, `ceeVA_` append (strings; it copies a text of up to 16 bytes with a byte loop and a longer one with `ceesafe_mem_copy`, since a `memcpy` of a length unknown to the C compiler is a library call for gcc, also for one byte), `ceeVW_` withCapacity; drop glue `ceeD_V…` destroys the elements in index order, then frees the buffer if `cap != 0`. The elements place `*v` is the vector itself, so elements are `v.ptr[i]` and views are rebuilt from `ptr` and `len` |
| `SlotMap<T, A>`, `Handle<T>` (M6e) | `ceeK_<T><A>` = `struct { ceeKS_… *ptr; uint64_t len, cap, count; uint32_t free; }` (40 bytes) with slots `ceeKS_…` = `{ uint32_t gen; uint32_t next; T value; }`; element code `K<T><A>`. `Handle<T>` is `cee_handle` = `{ uint32_t index; uint32_t gen; }` from `ceesafe/rt.h` (code `H<T>`), compared with `ceesafe_handle_eq`. Generations are odd while a slot is live; a removal whose generation reaches `UINT32_MAX - 1` retires the slot (never put back on the free list), so generations never wrap. Helpers `ceeKI_` insert (grows the slots with `ceesafe_vec_grow`, at most `UINT32_MAX - 1` slots), `ceeKV_` validity, `ceeKG_`/`ceeKM_` get (an optional pointer), `ceeKR_` remove; drop glue destroys values in odd-generation slots |
| `StableVector<T, A>` (M6e) | `ceeSV_<T><A>` = `struct { void *chunks[CEE_SVEC_CHUNKS]; uint64_t len; }` (264 bytes); element code `W<T><A>`. Chunk `k` holds `8 << k` elements and is allocated by `ceesafe_svec_slot` when its first element is pushed; chunks never move or shrink. Element `i` is `(*ceeSVE_…(&sv, i))` (`ceesafe_svec_at`: chunk `60 - clz(i + 8)`). A shared reference to a stable vector is a non-const pointer, since `push` mutates through it. Drop glue destroys the elements, then `ceesafe_svec_free` |
| `File`, `TcpListener`, `TcpStream` (C3) | `int32_t` descriptors; drop glue is the runtime's `ceesafe_fd_drop` (close). I/O built-ins call `ceesafe_*` functions declared in `ceesafe/rt.h` and implemented in `runtime/hosted/ceesafe_rt_io.c`; each returns `cee_io_result { value, error }`, and a per-type helper `ceeIOR_<Result>` turns it into `Result<X, IoError>` (written only for an `X` an I/O built-in returns: `void`, an integer or a descriptor) (the runtime maps errno to the variant; `Other` keeps the number). The generated `main` is `int main(int argc, char **argv)` — the one place plain `char` appears — and passes argv to `ceesafe_rt_args` as bytes |
| `StringView` (M6c) | `cee_str` = `struct { const uint8_t *ptr; uint64_t len; }` from `ceesafe/rt.h` (16 bytes); element code `PZ` (`Z` is the unsized `str`). A literal is `((cee_str){ .ptr = (const uint8_t *)"…", .len = n })`, pointing to static storage. `s[lo..hi]` calls `cee_check_str_range` (range, then character boundaries; `CEE_PANIC_STR_BOUNDARY`); `==`/`!=` call `ceesafe_str_eq`; `s.bytes()` rebuilds the struct as `ceeSl_u8`; printing calls `ceesafe_write_view` |
| `*T`, `*mut T` (M8a) | `T *` for both, `void *` for `*void`: read-only is a CeeSafe rule, and casting `const` away would trip `-Wcast-qual`. Element codes `N<T>` / `M<T>`. Every conversion to or from a pointer goes through `uintptr_t` (`(U *)(uintptr_t)p`), and `add`/`sub`/`offset` compute the address in `uintptr_t`, so no C pointer arithmetic or pointer-to-pointer cast is ever emitted; `p.read()` / `p.write(v)` are the place `(*p)`; `s.ptr` is the slice's `ptr` field; `Slice.fromRaw` builds the slice struct (`unsafe.md` §4) |
| `borrow T` | `const T *`. `borrow mut T` → `T *` (`restrict` only where the borrow checker proves exclusivity *and* M9 enables it). `Ref(k, p)` is `&p`, a deref place is `(*r)`, and reference locals are zero-initialised to `NULL` (never read before assignment) |
| `Slice<T>` / `SliceMut<T>` | `ceeSl_<elem>` = `struct { const T *ptr; uint64_t len; }` and `ceeSm_<elem>` = `struct { T *ptr; uint64_t len; }` (M2d; size 16, align 8, asserted). Slices are only made from arrays (N ≥ 1), so `ptr` is never NULL; `a[n..]` is the one-past-the-end pointer with length 0. An element is `s.ptr[i]`; a reborrow or `SliceMut`→`Slice` coercion rebuilds the struct from `ptr` and `len` |
| `Box<T, A>` | `T*` when `A` is zero-sized; `struct { T* ptr; A a; }` otherwise |
| Arrays `[T; N]` | `ceeA<N>_<elem>` = `struct { T e[N]; }`, so assignment, parameters and return by value work (M2d). An element is `a.e[i]`; `[v; N]` is a `for` loop over `e`; zero-initialised with `{0}`. Element names: integers and `bool` by name, `S<len><Name>` for structs, `E<len><Name>` for enums, `A<N>_<elem>` for arrays (`[[i32; 3]; 2]` → `ceeA2_A3_i32`) |
| Function pointers | C function pointer types via typedef |

Names (as implemented in M2a):
- functions: `cee_<name>`;
- struct members: `ceeM_<length><Struct>_<name>`, where the length prefix makes the boundary
  unambiguous;
- struct types: `ceeT_<name>`;
- fields: `f_<name>`;
- locals: `v_<name>_<id>` and `t_<id>`.

The prefixes keep the namespaces apart and avoid C keywords. Planned: package/namespace
qualification, plus a stable hash of the type arguments for monomorphised instances (not .NET
`GetHashCode`). `@export("sym")` overrides the name.

Arrays and slices are defined with the structs, each after the types it contains by value; slices
come after their element types. Every array and slice type the program uses is emitted, in the
order the checker created it.

Bounds checks (M2d) call `cee_check_index(i, len, loc)` and `cee_check_range(lo, hi, len, loc)`
from `ceesafe/rt.h`; they panic with "index out of bounds" and "slice range out of bounds". A
constant index below a constant length, and a constant range within a constant length, are not
checked. Since M9c, an index check that bounds-check elimination proved cannot fail is not emitted either
(`optimisations.md` §2; `--no-bce` keeps them all). Likewise `+`, `-`, `*` and negation proved not to overflow are
written without their helper, as `(T)(a + b)` (`optimisations.md` §3; `--no-oce` keeps them all).

`d.swap(i, j)` (M11 G8c), after its two index checks, exchanges the elements through a temporary of the element's
C type, which moves them whatever the type. `v.truncate(n)` is an inline block that lowers the length and calls the
element type's drop glue on each removed element, lowest index first.

Bulk slice operations (M9b, type-system §16.5): `d.copyFrom(s)` is `cee_check_length(s.len, d.len, loc)`,
which panics with "slice lengths differ" (`CEE_PANIC_LENGTH`), then `cee_slice_copy(d.ptr, s.ptr, d.len,
sizeof *d.ptr)`, which calls `ceesafe_mem_move` unless the length is 0 (C11 leaves `memmove` on a null
pointer undefined even for 0 bytes, and a slice from raw parts may have one). `d.fill(v)` is
`cee_slice_fill_byte`, a `ceesafe_mem_set`, for a one-byte integer or `bool` element, and otherwise an element
loop in the generated C, which the C compiler may itself turn into `memset`. `ceesafe_mem_move` and
`ceesafe_mem_set` are `__builtin_memmove` and `__builtin_memset` under clang and gcc, which may lower to calls
to `memmove` and `memset`: both are in the freestanding allowlist (`profiles.md` §4.4). Under MSVC
they are `<string.h>`'s functions.

Layout verification: for every emitted struct the backend emits
`_Static_assert(sizeof(S) == N && _Alignof(S) == A, "...")` and `offsetof`
assertions, where `N`/`A` come from `TargetInfo`. If the C compiler for the actual target
disagrees with CeeSafe's model, the build fails, and nothing miscompiles silently.
The message names the CeeSafe type. It is an unevaluated string, which may not contain numeric escapes (C23; clang
rejects them in every mode), so `?` is written `\?` (otherwise `Box<T>??` would contain the trigraph `??)`), and any
byte outside printable ASCII becomes `_` (M9a).

## 2.1 C names (M7d)

Generated code and the runtime share one C namespace, so their names are kept disjoint by
prefix:

| Owner | Names |
|---|---|
| **Runtime** (`ceesafe/rt.h`, `ceesafe_rt_*.c`) | `ceesafe_*`; `cee_` followed by a lowercase letter (`cee_add_i32`, `cee_check_index`, `cee_str`, `cee_loc`); `CEE_*` |
| **Generated** | `cee` followed by a capital letter: `ceeF_name` (top-level functions), `ceeM_` (members), `ceeG_` (generic instances), `ceeXS<n>_<name>` (variant `n` of the function named `<name>` made by closure-call specialisation, optimisations.md §5), `ceeT_`/`ceeE_` (types), and the helper families (`ceeD_`, `ceeB_`, `ceeVP_`, …; for async fns `ceeAF_` frames,
`ceeAI_` initialisers and `ceeAR_` resume functions, §3a); locals `v_<name>_<n>` and `t_<n>`; fields `f_<name>`; variants `v_<name>`; labels `bb<n>`; files `cee_file_<n>`; source locations `ceeL_<n>` (§6) |

**Packages** (M10 T5, ADR 0013 D4): an item of a dependency, rather than of the package being built, has its
package's name in its C name: `ceeF_geometry_area`, `ceeT_geometry_Point`, `ceeM_14geometry_Point_norm`. The package
name is part of the item's name as the backend sees it (`geometry.Point`, written with `_` in C), so every rule above
applies unchanged, and two packages may declare the same name.

In Nova before M7d, top-level functions were `nv_<name>`. A function named `add_i32` then collided with
the runtime's `nv_add_i32`, and the C failed to compile. The random program generator found this,
and `run-pass/runtime-names.cee` keeps it fixed.

## 2.2 Stack-placed boxes (M5; on by default since M9e, `--no-placement`)

When `CEmitOptions.Placement` is on (the default since M9e, `optimisations.md` §4), `PlacementAnalysis` decides each
`Box.create` site (`memory-model.md` §5.1).
- **Slot.** A stack-placed site has a slot `CEE_UNUSED T cee_box_<n> = {0};` among the function's
  locals. A site in a loop reuses its slot in every iteration; the analysis places it only when the previous box is
  gone by then.
- **Creation.** `R = ceeB_…(v)` becomes `cee_box_<n> = v;` (or the construction written into `cee_box_<n>`, as for a
  box built in place, §2) followed by
  `R = ceeMk_<Result>_v_Ok(&cee_box_<n>);`. Nothing is allocated, so
  nothing can fail.
- **Destruction.**
  - A `Drop` of a box local of the site calls the contents' drop glue `ceeD_T(b)` if `T` needs drop,
    and nothing else. A drop flag still guards it.
  - A `BoxFree` of one emits nothing.

Everything else is unchanged. Heap boxes, and every other site, use the drop glue of §2.

## 2.3 C interoperability (M8b; type-system.md §28)

- **Extern functions.** Each `extern "C"` function gets a prototype with the C types of §28.3
  (`size_t strlen(const char *);`) and a `static` wrapper `ceeC_<name>` with CeeSafe's own C types,
  which converts the arguments (`(size_t)n`; a `T *` converts to `const T *` implicitly), calls the
  function and converts the result back (a `const T *` through `uintptr_t`; a `@repr` enum through
  a check that panics with `CEE_PANIC_FFI_ENUM`). CeeSafe code calls the wrapper like any function. The
  prototypes use C's own types so that they are compatible with the C library's declarations: the
  generated code includes no C library header (unless a `@header` names one), but clang and GCC know the library functions'
  types (`-Wincompatible-library-redeclaration`, `-Wbuiltin-declaration-mismatch`), and C11
  6.5.2.2 makes a call through an incompatible type undefined.
- **Exports.** Each `@export` function gets a non-`static` wrapper under its symbol, with the C
  types, that converts the arguments (checking `@repr` enums), calls `ceeF_<name>`, and converts the
  result. Nothing else in the generated code has external linkage except `main`.
- **`@repr(C)` structs** are `struct S` with members named as in CeeSafe (other structs are `struct
  ceeT_S` with members `f_<name>`), and `typedef struct S ceeT_S;`, so generated code uses them like
  any struct, and C code that includes `ceesafe_exports.h` sees the same type (C11 6.2.7 makes struct
  types in two translation units compatible only if their tags and members match).
  **One definition** (ADR 0030): the structs are defined only in the exports header (with the CeeSafe structs they hold,
  such as lib/core's `Volatile<T>`), which `main.c` (or, split, `ceesafe_program.h`) includes after the `@header`s; its
  layout assertions follow the include. So a C header may include the exports header and a CeeSafe program may include
  that C header. `--exports-header <name>` sets the name the header is included by and written under, and its guard
  (`kernel/wait_types.h` → `KERNEL_WAIT_TYPES_H`); without it, `ceesafe_exports.h`.
  An array field is a C array member (`CE f[N]`) in both, used in CeeSafe's C as its array struct,
  `(*(ceeA<N>_<E> *)(uintptr_t)&p->f)`, after an assertion that the two have one size and alignment; a struct
  literal assigns it after the compound literal, and a static's initialiser writes it as `{ … }` (ADR 0029). An
  imported member is its C spelling (`spinlock_t lock;`); a `@repr(C)` struct that holds one is C-laid-out (§33.1),
  so neither file asserts its layout.
- **`@repr(uN)` enums** keep the internal form `struct { uintN_t tag; }` (the tag has the repr's
  width and signedness); wrappers convert to and from the integer.
- **`ceesafe_exports.h`**: an include guard; `<stdbool.h>`, `<stddef.h>`, `<stdint.h>`; each `@repr(C)`
  struct (after the structs it contains); each `@repr` enum as `typedef <int> E;` and constants
  `E_Variant`; each export's prototype. It is generated only when there is something to declare.
- **Imported C types (C6b, type-system.md §31)**: `main.c` and `ceesafe_exports.h` `#include "…"` each
  `@header` after `ceesafe/rt.h`, once, in declaration order; they are the only headers generated code
  includes besides the runtime's. An imported type has no definition, typedef or layout assertion in
  generated code: it is its C spelling (`struct name` or the `@ctype` name), and a field place is a
  member access. Pointer, `usize` and `isize` fields convert through `uintptr_t` or a cast when read
  and written, as wrappers do. For each declared field, two `_Static_assert`s (on `_Generic` of the
  member, with the field's C type, and on its `sizeof`) make a header that disagrees a C compile error
  that names the struct, the field and the CeeSafe declaration's file and line.
- **C arrays in imported types (C9, ADR 0014)**: a field `f: [E; N]` is the member seen as CeeSafe's array
  struct, `(*(ceeA<N>_<E> *)(uintptr_t)&p->f)`: no copy, and every array operation works on it. Its checks are
  `_Generic(&member, CE (*)[N]: 1, default: 0)` (the member itself would decay to a pointer) and equal
  `sizeof` and `_Alignof` with the array struct. A `@cname` field uses its C name in both. An array of an
  imported type is emitted as an array struct of the C type, with no layout assertion (C lays it out), and
  `add`/`sub`/`offset` on a pointer to an imported type scale by C's `sizeof`.
- **Exported constants (C9, type-system.md §28.4)** are read-only C data, `const CE symbol[N] = { … };` or
  `const CT symbol = v;`, each after an `extern` declaration of itself (so `-Wmissing-variable-declarations` is
  quiet), and declared `extern` in `ceesafe_exports.h`. CeeSafe code uses the constant's value, not the object.
- **C sources** given to `ceesafec build` are copied unchanged to `c/<file>` beside `main.c` and
  compiled with it (`-I` names the directory, so `#include "ceesafe_exports.h"` works).
- **Function pointers (M8d, §29.5)** are `typedef R (*ceeP_<code>)(A, …);` with the C types of
  §28.3 (element code `F<n><parameters><result>`), declared after the struct names and before the
  structs. A value is the extern function's name or the export's symbol (exports are declared
  before every function); a call converts its arguments and result like an extern wrapper.
  `ceesafe_exports.h` declares the struct names first, then the function pointer types.
- **`lib/core` (M8d, §29.2)** is compiled with every package, but only the functions that emitted
  code calls, the types it mentions, and their files' names are emitted, so a program that uses
  none of it gets the same C as before.
- **Volatile accesses** (§29.1) are `(*(volatile T *)p)`; `@align(n)` (§29.4) is `_Alignas(n)` on a
  struct's first member; `Cpu.spinHint()` and the target queries are `ceesafe/rt.h` inlines.

## 2.4 Split output (M14 K2a, ADR 0022)

`ceesafec build --split` writes the program's C in several translation units, so a C compiler can build it in parallel
and rebuild only what changed. Without it the output is one `main.c`, as above, byte for byte.

- **`ceesafe_program.h`**, included by every unit, holds what one `main.c` has before its first function body (the
  source-location tables, the types, the prototypes, the C extern wrappers, `dyn` tables, thread and fork-join helpers),
  and the per-type helpers that `main.c` defines after its functions: drop glue, the `Box`, vector, container and I/O
  helpers. Each of them is `static`, the per-type helpers `static inline`, so every unit has its own inlinable copy. It
  starts with an include guard named after the program.
- **One unit per CeeSafe source file**, named after it (`tcp.c`), with as many directories before the name as it takes to
  make the names distinct (`net_tcp.c`, `vfs_tcp.c`). It holds the functions declared in that file and the closures
  written in them.
- **`ceesafe_instances.c`** holds what is defined once and belongs to no user file: lib/core's functions, generic
  instances, `@export` constants, callbacks, and C's `main`.
- **Linkage.** Every function defined in a unit or in `ceesafe_instances.c` has external linkage, and its prototype in
  the header has no `static`. The header renames each of them with the program's name, `#define ceeF_main
  ceeF_<program>_main` (the name after the family prefix; `ceeARG` becomes `ceeARG_<program>`), so two CeeSafe programs
  linked into one image never define the same symbol. The program's name is `--program`, the package's, or the first
  file's. `@export` names are not renamed.
- **Determinism.** The units, their names and each function's placement are a function of the package's file list and
  the program, and two builds are byte-identical.
- **Inlining.** A call from one unit to a CeeSafe function in another (lib/core's included) is not inlined without
  link-time optimisation. The helpers in the header are in every unit, so calls to them still inline: with the
  per-type helpers in `ceesafe_instances.c` instead, `vector-growth` and `string-building` ran 1.7–1.9× slower, and with
  them in the header as fast as one file (Mac, 2026-10-29).
- **Checked by** the run corpus's `clang -O2 split` configuration (`CToolchains`): every run-pass and run-fail program
  is built split, with the strict warnings, and must behave as in every other configuration.

## 3. Lowering of control flow and cleanup

**Flat form** (M1; `--flat-c`): each NIR block becomes a C label. Terminators become `goto`, `switch`
(SwitchInt) or `return`. It is the reference form: simple enough to check by reading.

**Structured form** (M7b; the default). It is a readability pass and adds nothing that
correctness depends on. One invariant makes it safe: **every block's code ends in an explicit
transfer**: `goto`, `break`, `continue`, `return`, or a `_Noreturn` call. Code therefore never
falls into the next block by accident. Where a block is placed changes only how the code reads,
and a transfer may be dropped only where falling through provably reaches the same label.

1. **Analysis.**
   - Reverse postorder, dominators (Cooper–Harvey–Kennedy), and *forward predecessors*: in-edges
     that are not back edges, counted with multiplicity, so a switch with two cases to one block
     counts twice.
   - A *back edge* `u → h` is one where `h` dominates `u`. A retreating edge that is not a back
     edge makes the graph irreducible. The function is then emitted flat. CeeSafe source has no
     `goto`, so this is not expected; it is a fallback.
   - The natural loop of a header `h` is `h` plus every block that reaches a back-edge source
     without passing through `h`.
2. **Placement.** Each block's code is emitted exactly once, inside the code of its immediate
   dominator `d`, in one of three ways:
   - **Inline**, at the branch to it: when it has one forward predecessor and leaves no loop that
     contains `d`.
   - **Exit follow**: when it leaves loops. Let `h` be the outermost loop header whose loop
     contains `d` but not the block. The block is emitted after `h`'s loop statement. One
     exception keeps early returns in place: a block that leaves a loop from a block other than
     the header itself is inlined if it has one forward predecessor and its dominator subtree is
     *closed* and small. Closed means no edge leaves the subtree; small means at most 2 blocks
     and 8 statements.
   - **Follow**: otherwise (a merge point). It is emitted after `d`'s own code, inside the same
     loops. Follows and exit follows are emitted in reverse postorder.
3. **Shapes.**
   - A loop header's code (with its inline and in-loop placements) becomes `for (;;) { … }`.
   - `NirBranch` becomes `if (c) { … } else { … }`, and `NirSwitch` becomes `switch` with one
     `case` body per target.
   - A branch to an inline block emits that block's code there. Every other branch is first
     emitted as `goto bbN;`.
4. **Transfer elimination**, in tree order and repeated to a fixpoint, since removing a `goto`
   can lengthen what falls through before it:
   - A `goto L` is removed when falling through reaches `L`. Falling through means passing over
     only labels, the end of an `if` arm, or the end of a loop body back to its own header's
     label.
   - Otherwise it becomes `continue` if `L` is the innermost loop's header.
   - Otherwise it becomes `break` if `L` is what falls through after the innermost loop, unless
     a `switch` lies in between.
   - Otherwise it becomes `break` if `L` falls through after the innermost `switch`.
   - The end of a `case` body never falls through: its transfer is kept, so no `case` falls into
     the next (`-Wimplicit-fallthrough`).
5. **Labels** that no remaining `goto` names are removed (`-Wunused-label`). A label at the end of
   a block is written `bbN: ;`.
6. **Guard clauses.**
   - An `if` arm that cannot complete normally is hoisted: `if (c) { A } else { B }` with `A`
     ending in a transfer becomes `if (c) { A } B`. Symmetrically, when only `B` ends in a
     transfer it becomes `if (!c) { B } A`. When both arms end in a transfer, the shorter arm is
     the guarded one.
   - An empty `then` arm is negated away.
   - An `else` holding a single `if` is printed as `else if`.

The run corpus is built in both forms under every C configuration: structured by default, flat as
the oracle in a `clang -O2 flat` configuration. Both must agree.

Shape of `gcd` (structured):

```c
    v_x_2 = v_a_0;
    v_y_3 = v_b_1;
    for (;;) {
        t_5 = (v_y_3 != ((uint64_t)0));
        if (!t_5) {
            break;
        }
        v_t_4 = cee_rem_u64(v_x_2, v_y_3, &ceeL_0);
        v_x_2 = v_y_3;
        v_y_3 = v_t_4;
    }
    t_6 = v_x_2;
    return t_6;
```

Flat form of the same function (M1), shown in an earlier, illustrative notation:

```c
int32_t cee_demo_gcd(uint64_t a, uint64_t b) {
    uint64_t x; uint64_t y; uint64_t t;
bb0:
    x = a; y = b;
    goto bb1;
bb1:
    if (!(y != 0u)) goto bb3;
    goto bb2;
bb2:
    if (y == 0u) ceesafe_panic_div0(CEE_SRC(3, 17));   /* inserted check (statically elidable later) */
    t = x % y; x = y; y = t;
    goto bb1;
bb3:
    return x;
}
```

Cleanup: drop elaboration has already put `Drop` calls on every exit edge,
so early `return`, `?` and `break` need no special backend support. Drop flags are
`bool` locals initialised to `false`. No `setjmp/longjmp` and no C++.

Panics are `noreturn` calls into the runtime (`ceesafe_panic(kind, src)`), so destructors are
not run (panic = abort, OD-5).

## 3a. Async functions (L1; ADR 0031 §3.6, type-system.md §40)

An async fn `F` whose C function name would be `<F>` (§2.1) lowers to four C items instead of one function
(`src/backend/c_async.cee`):

```c
struct ceeAF_<F> {
    uint32_t cee_state;         /* 0 at entry; k after suspension point k (intermediate-representation.md §3) */
    /* the parameters, then every local, as fields named like the locals (v_<name>_<n>, t_<n>) */
    /* one field per awaited call holding the awaited task's frame: struct ceeAF_<G> cee_a<k>; */
    /* the result, if F's result type is not void: R cee_result; */
};
static void ceeAI_<F>(struct ceeAF_<F> *cee_frame, /* the parameters */);  /* zeroes the frame, stores the arguments */
static bool ceeAR_<F>(struct ceeAF_<F> *cee_frame);  /* runs F until it returns (true) or suspends (false) */
static bool ceeAT_<F>(void *frame);                  /* ceeAR_<F> as the runtime calls it */
```

- **Frames are emitted child-first,** so a frame that embeds another's follows it. A cycle cannot occur (N0343).
- **Every local is a frame field in v1.** Inside the resume function each local's C name is defined as its field
  (`#define v_x_3 (cee_frame->v_x_3)`, undefined after the function), so statements are emitted as in a sync function,
  and a pointer to a local stays valid while the frame does, which is until the task returns (type-system.md §40.6).
  L1 A3 keeps only the locals live across a suspension point.
- **The resume function is always in flat form** (§3), whatever `--flat-c` says. It opens with
  `switch (cee_frame->cee_state) { case k: goto cee_resume_k; … default: break; }`. Jumping into the middle of loops
  makes the graph irreducible, so structuring it would fall back to flat form anyway.
- **An awaited call** `dest = G(args…)`, suspension point k, is

  ```c
  ceeAI_<G>(&cee_frame->cee_a<k>, args…);
  cee_frame->cee_state = k;
  cee_resume_<k>:
  if (!ceeAR_<G>(&cee_frame->cee_a<k>)) return false;
  dest = cee_frame->cee_a<k>.cee_result;
  ```
- **`Async.park()`** is `cee_frame->cee_state = k; return false; cee_resume_<k>: ;`.
- **`return e`** stores `cee_frame->cee_result = e` and returns `true`. Drop elaboration has already dropped the
  locals.
- **`Async.run(G(args))`** declares the root frame `struct ceeAF_<G> cee_root` in a block of the caller's C function,
  then calls `ceeAI_<G>(&cee_root, args…)`, `ceesafe_async_run(&cee_root, ceeAT_<G>)`, and reads
  `cee_root.cee_result`. The thunk adapts `ceeAR_<G>` to the runtime's `bool (*)(void *)`, so no function is called
  through another type.
- **Placement** (§2.2) and closure specialisation skip async fns: a box's C-stack slot would not survive a suspension.

The task header and executor are in `runtime/hosted/ceesafe_rt_async.c`, linked when the generated C calls
`ceesafe_async_*` (§6).

## 4. Evaluation order and temporaries

NIR has already sequenced every side effect into separate statements, so the C
backend never emits an expression whose meaning depends on unspecified C
evaluation order (function-argument order, `a[i] = i++`-style). Arguments
with side effects are always assigned to temporaries first.

## 5. UB avoidance checklist (enforced by construction + tests)

| Hazard | Strategy |
|---|---|
| Signed overflow | `ceesafe_add_i32_chk(a,b,src)` via `__builtin_add_overflow`, with a portable fallback that computes in unsigned and compares. Wrapping ops compute in unsigned and convert back through a well-defined helper (implementation-defined conversion avoided by `memcpy`-free bit tricks verified per compiler) |
| Division | Check for zero and `MIN/-1` before `/` and `%` |
| Shifts | Check `0 <= n < width`. Left-shifting a negative signed value is done in unsigned |
| Unsigned promotions | `uint8_t`/`uint16_t` arithmetic is promoted to `int`. All narrow arithmetic is performed as `uint32_t` and truncated explicitly |
| Pointer arithmetic | Never form out-of-range pointers. Bounds checks compare indices before computing `ptr + i`. Raw-pointer arithmetic (M8a) is done on `uintptr_t`, and raw pointers have no ordering comparisons (`unsafe.md` §4) |
| Strict aliasing | No type punning. Reinterpretation (`transmute`, unsafe only) goes through `memcpy` (a builtin; supplied by the runtime in freestanding builds) |
| Uninitialised reads | Definite-init analysis forbids them. A local of aggregate type (struct, enum, array, slice, vector, …) is zero-initialised (`{0}`) in C when any part of it can be written other than by assigning it whole: a write to a field, element or payload, a borrow of it or of a part (`&x`, `x.e + i`, shared or mutable), or being an
`Unpack` target. So is one whose variant payload is read (a `match` arm, an `Unpack` source): the arm for a variant the
local never holds reads bytes its assigned variants do not cover, and gcc's `-Wmaybe-uninitialized` (with `-Werror`)
rejects that at `-O0`, although the tag test means it never runs (found by the M9d fuzz campaign); so indeterminate members or padding are never copied. Every other aggregate local is assigned whole before each read (definite assignment, which the interpreter checks on every run), and is declared without an initializer (M9d: gcc 12 does not remove the zeroing, which was most of `fbvalid`'s gcc gap, `milestone-9.md`). Scalars and references are always zero-initialised; the C compilers drop those stores |
| Null | References are never null (type system). `T?` niche checks are explicit |
| Alignment | All accesses are through properly typed lvalues. `@packed` fields are accessed via `memcpy` helpers |
| Lifetime of compound literals / temporaries | Not used across statements. Temporaries are named locals |
| `memcpy` overlap | Moves use `memmove` when the source and destination may alias; NIR knows when they cannot |
| Float→int conversion out of range (UB in C) | Only via `toIntSaturating`/`checkedCast` helpers that range-check first |
| Warnings on valid programs (M7d) | CeeSafe accepts code that C compilers warn about, and generated C must still be warning-free. `x = x;` emits nothing (`-Wself-assign`). A comparison of a place with itself is the constant `true`/`false` (`-Wtautological-compare`). So is an ordering comparison against the type's own minimum or maximum, such as `u < 0` or `x <= MAX` (GCC's `-Wtype-limits`, found in Linux CI by `FuzzTests`). A constant index past a fixed array's end, after the check that always panics, goes through `cee_opaque_index` (`-Warray-bounds`). So does a constant slice or vector index too large for any object (index × element size > 2⁶³ − 1), found by the M7e campaign (seeds 1009815, 1014252, 1016777). In the structured form, statements that print nothing are dropped, so no label ends a block (`-Wc23-extensions`). Found by the random program generator; each case is in `run-pass/warning-free.cee` or `run-fail/constant-index-past-end.cee` |
| Infinite side-effect-free loops (C11 6.8.5p6 permits assuming termination) | Loops whose controlling expression is not a constant expression and that have no side effects get a `ceesafe_loop_barrier()` (an empty `asm volatile` / volatile access) |

## 6. Runtime surface (`ceesafe/rt.h`)

Heap (M3b):
- `ceesafe_alloc(size, allocator)`, `ceesafe_realloc(p, size, allocator)` and `ceesafe_free(p, allocator)` go
  through the platform's `ceesafe_platform_alloc`/`ceesafe_platform_realloc`/`ceesafe_platform_free` (hosted:
  `malloc`/`realloc`/`free`, which also serve CosmoOS userland). They count attempts and live
  allocations; a reallocation is one attempt, and on failure its block is unchanged and still live. `ceesafe_fail_after`/`ceesafe_fail_reset`
  implement `FailingAllocator`.
- **Threads** (M13 G4, type-system.md §38): `runtime/hosted/ceesafe_rt_thread.c`, linked only when the generated C calls
  `ceesafe_thread_*`, `ceesafe_mutex_*` or `ceesafe_atomic_*`.
  - `ceesafe_thread_start`/`join` run over POSIX threads, or Win32 threads on Windows.
  - Mutexes are allocated: `pthread_mutex_t`, or `SRWLOCK` on Windows.
  - The atomics are sequentially consistent: `__atomic` builtins, or `Interlocked*` under MSVC.
  - Thread records, environments and mutexes come from `ceesafe_alloc`, so fault injection reaches them.
  - The allocation counters change with relaxed atomic adds (`CEE_COUNT`), since any thread may allocate.
  - A `Thread<T>` is `ceeTB_<T> *`, a record of `{ cee_thread *thread; void *data; const void *vt; T result; }`.
  - `ceeTS_<env>` boxes a spawned move literal's environment and starts `ceeTR_<T>`, which calls the body through the
    `dyn mut fn() -> T` table and stores the result.
  - `ceeTJ_<T>` joins, takes the result, then drops and frees the environment and the record. A `Thread<T>`'s drop
    glue joins, then drops the result.
  - `Thread.both(a, b)` (§38.6) is a block, not a call: it starts `ceeTF_<closure>` on `a` with `ceesafe_thread_start`,
    calls `b`, then joins; if the start returned `NULL` it calls `a` first, then `b`. `ceeTF_<closure>` takes the
    closure as `void *`, converts it back to its own type and calls it, so no function pointer is called through
    another type. A `fn()` argument is passed as the address of a local holding it, which lives until the join.
- **Async** (L1, type-system.md §40): `runtime/hosted/ceesafe_rt_async.c`, linked only when the generated C calls
  `ceesafe_async_*`.
  - `ceesafe_async_run(frame, resume)` resumes the root frame until it returns. Between resumes it waits, in `poll()`,
    for what the parked task registered: a deadline on the monotonic clock (`ceesafe_async_wait_until`, from
    `ceesafe_async_now`) or a descriptor becoming writable (`ceesafe_async_wait_writable`). A task that parks with
    nothing registered aborts with a message.
  - `ceesafe_async_write(fd, bytes, length)` writes at most `PIPE_BUF` bytes if `fd` is writable now, after flushing
    standard output or error: a count, 0 if it is not writable, or a negated errno (ADR 0031 §3.5).
  - A1 runs one task per `Async.run` and allocates nothing. A2 adds a task header with the cancel flag, shield depth
    and deadline, scheduler links and waker; `ceesafe_async_wake(id)`; kqueue and epoll beside `poll()`; and
    `CEESAFE_CANCEL_AT=k`, which cancels the root task at its k-th suspension, with `CEESAFE_CANCEL_REPORT` printing
    `ceesafe cancel: points=N` (`testing.md`).
- The generated `main` calls `ceesafe_rt_start()` (reads `CEESAFE_FAIL_ALLOC_AT`, `CEESAFE_ALLOC_REPORT`
  via `ceesafe_platform_config`) and returns through `ceesafe_rt_finish(code)`, which prints the
  allocation report if asked. `Process.exit` and panics skip the report.


- `ceesafe_panic(kind, src)`: `noreturn`. Provided by the profile: hosted (write to
  stderr + `abort()`), CosmoOS userland (same, through CosmoOS libc), kernel
  (calls the kernel panic routine; C5).
- Checked-arithmetic helpers (`static inline`).
- `ceesafe_mem_copy/move/set` (M9b): `__builtin_memcpy`, `__builtin_memmove` and `__builtin_memset` under clang
  and gcc, which may become calls to the C library's or the kernel's `memcpy`, `memmove` and `memset`;
  `<string.h>`'s functions under MSVC. Used by the bulk slice operations (§2) and by vector growth.
- Allocator vtable ABI for dynamic allocators (`memory-model.md` §3.2).
- Source locations (M9d). A location is an `cee_loc`: the CeeSafe path as written to the compiler (a file
  table `cee_file_<n>`, so no absolute host path is embedded unless one was given), and the 1-based line and
  column. Each location a checked helper (`cee_add_*`, `cee_check_index`, …) can panic with is a file-scope
  constant `static const cee_loc ceeL_<n>`, one per distinct location, in the order of first use; the helper
  takes `const cee_loc *` and reads it only when the check fails. A direct `ceesafe_panic` passes the location by
  value (`CEE_LOC(file, line, column)`), and `ceesafe_panic` itself still takes `cee_loc` by value
  (`profiles.md` §4.2). Why a pointer: with the location by value, gcc 12 at `-O2` loads each
  check's location into registers on the path where the check passes, about half of what the checks cost
  in `data-oriented` (`milestone-9.md`, 9d). The table holds pointers to the file names, so it is in
  `.rodata` (`.data.rel.ro` in position-independent code), with absolute 64-bit relocations, which the
  CosmoOS module loader applies (`cosmoos/c4-report.md`).

## 7. Debugging and source maps (M7a)

- **`#line` directives** map generated C back to CeeSafe source. They are off by default and turned
  on by `--line-directives`; `ceesafec build -g` turns them on and compiles the C with `-g -O0`
  instead of `-O2`.
- With directives on, each function body maps to CeeSafe lines:
  - the function's opening line maps to the line of its name;
  - each statement and terminator maps to the first line of the source construct it came from
    (its span). A directive is written only when the line or file changes. Code without a span of
    its own, such as compiler temporaries, keeps the line before it.
  - A block's label shares the line of the block's first statement, so labels need no directive
    of their own. The local declarations share one line, attributed to the function's name.
  - After each function, `#line <n> "main.c"` hands the following lines back to the C file.
    Everything outside function bodies (types, drop glue, helpers, `main`) is attributed to
    `main.c`.
- **Paths.** Directives and panic locations name a source by its *source path*. This is the path
  given to the compiler, with `/` separators. The CLI rewrites an absolute path under the current
  directory to a relative one. Any other absolute path is reduced to its file name. Generated C
  therefore never contains a host path (`build-integration.md` §6).
- `ceesafec build` runs the C compiler with the current directory as its working directory. The
  debug info's compilation directory then makes the source paths resolve, so a debugger finds
  the `.cee` files.
- Locals keep stable names: `v_<name>_<n>` for a named local, `t_<n>` for a temporary.
  A C debugger therefore shows recognisable variables (`v_count_3`).
- Directives never change the code itself. A program with directives must behave exactly like
  one without them. The run corpus is built both ways (§8).

## 7a. Implementation notes (M1, increment 1c)

- Every CeeSafe function becomes `static CEE_UNUSED` with a prototype. CeeSafe allows unused functions,
  parameters and locals, and `-Wunused-*` must stay clean under `-Werror`.
- Locals are declared at the top of the function, zero-initialised as in §5 (scalars only in M1). Only
  blocks that are jump targets get a label.
- Runtime files:
  - `ceesafe/rt.h`: freestanding checked helpers `cee_add_i32`, `cee_div_u64`, `cee_shl_u8`,
    `cee_shift_amount_s/u`, `cee_as_iN`, …;
  - `ceesafe_rt_core.c`: output formatting and the panic message, with no libc;
  - `ceesafe_rt_hosted.c`: the platform layer over stdio.

  `ceesafec build --emit c` writes all of them next to `main.c`. Build with
  `cc -I <dir> <dir>/*.c`.
- Panic output: `ceesafe panic: <reason> at <file>:<line>:<column>`. Stdout is flushed first, then
  `abort()` (exit status 134).
- `main() -> i32` becomes `int main(void) { return (int)ceeF_main(); }` (M7d renamed `cee_` to `ceeF_`, §2.1). `Process.exit(c)` calls
  `exit(c)`, which flushes stdio.

## 8. Validation

- Golden-file tests for representative C output (mainly to review diffs).
- Every `run-pass` test is compiled with clang and gcc (host) at `-O0` and `-O2`,
  with and without `-fsanitize=address,undefined`. Outputs must agree
  (differential testing).
- M7a: every run-corpus program is also built with `#line` directives (clang `-O0 -g`) and must
  produce the same results. Nova's `SourceMapTests` compiles programs with `-g` and checks the
  debug line table: it names the `.cee` file and the lines of the statements.
- M7: a Csmith-style random program generator for CeeSafe; three-way
  differential testing (clang/gcc × opt levels), plus a reference
  interpreter on NIR (`ceesafec run --interp`) as an oracle (`milestone-7.md`).
