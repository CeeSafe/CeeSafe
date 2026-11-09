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
  - **TCP** (L1 A4a) is Winsock 2, started once (`InitOnceExecuteOnce`). A socket's descriptor is an id from
    `CEE_SOCKET_BASE` (`0x40000000`) in a table of `SOCKET`s under a slim reader-writer lock, so lib/std's `i32`
    descriptors never hold a truncated handle; `ceesafe_fd_read`, `_write` and `_drop` send an id to `recv`, `send`
    and `closesocket`, anything lower to the C runtime. Winsock errors become the C runtime's `errno` values
    (`WSAECONNREFUSED` is `ECONNREFUSED`), so `IoError`'s kinds are as elsewhere. Async sockets are nonblocking
    (`FIONBIO`); the executor's wait is described in §6.
- **Test.** The Windows checks (`tools/host_c_check.py`, and the run corpus under `--cc cl`) cover three things:
  - every run-pass and run-fail program builds with MSVC and gives the corpus expectations;
  - `nwc` reads files and standard input;
  - a program runs `cmd /c exit 7` with `Process.run`, and sees a missing program fail.

  **A workaround for MSVC's optimiser:** ceesafec builds with `/O2 /Oi-`. At plain `/O2`, MSVC 19.44 to 19.51 (for
  ARM64 and x64) miscompile a loop that stores through the pointer of a slice copy, followed by a `memmove` into the
  same array: `run-pass/slice-bulk.cee` printed 3 for 70003, and the standalone
  `tests/c-compilers/msvc-o2-store-through-copy.c` (valid C11) fails the same way. clang, GCC, `/Od`, `/O1`, inlining
  off (`/Ob0`) and intrinsics off (`/Oi-`) are right. `/Oi-` makes `memcpy` and `memmove` calls the optimiser does
  not see through, and keeps every other optimisation (2026-11-01). With it every run-pass and run-fail program
  passes. The `test-windows` CI job runs the tests on x64. Locally, a Windows 11
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
relies on) raises the version. Version 1 is the first.

## 2. Lowering of types

| CeeSafe | C |
|---|---|
| `i8..i64`, `u8..u64` | `int8_t..int64_t`, `uint8_t..uint64_t` |
| `never` as a function's result (ADR 0074 §2.2, type-system.md §15.3) | `void`, with `_Noreturn` before the function's prototype and definition. A call of one is a statement followed by `ceesafe_panic(CEE_PANIC_UNREACHABLE, loc)`, NIR's `Unreachable` |
| `isize`/`usize` | the fixed-width type of the target's pointer width (`int64_t`/`uint64_t` on LP64), plus `_Static_assert(sizeof(void *) == 8)`. `ptrdiff_t`/`size_t` are used at FFI boundaries (M8) |
| `bool` | `bool` (`_Bool`) |
| `char` | `uint32_t` |
| `borrow fn(T…) -> R` (M11 G8b) | `const ceeCK_<mangle> *`, a pointer to the first member, the code pointer `R (*)(const void *, T…)`, of an environment `ceeCE_<id>` that holds a pointer per capture; the body is `ceeCB_<id>`, which converts `env` back through `uintptr_t`; a call is `(*f)(f, args…)` (type-system.md §36.2) |
| `borrow dyn I`, `borrow mut dyn I`, `Box<dyn I, A>` (M13 G2) | `ceeDR_<D…>` = `struct { void *data; const ceeDT_<D…> *vt; }` (16 bytes; the dyn type mangles as `D<length><interface><arguments>`, `dyn Shape` → `D5Shape0`), typedef'd before every struct, so any struct may hold one. Not a niche optional: `Box<dyn I>?` has a tag. The table type `ceeDT_…` is defined after the structs: `drop` (NULL if `T` needs none), `size`, `align`, then `m0…` in the interface's method order, each taking `void *` for the receiver. Per (`T`, `I`) pair that a coercion names: `static const ceeDT_… ceeVT_<T>_<D…>`, with adapters `ceeVT_…_<slot>` that convert the address back through `uintptr_t` and call `T`'s method, and `ceeVT_…_drop` over `T`'s drop glue. A coercion is `((ceeDR_…){ .data = (void *)(uintptr_t)p, .vt = &ceeVT_… })`; a call through `x` is `x.vt->m<slot>(x.data, args…)`; borrowing `*x` is `x` itself. An async method's entry is `void (*m<slot>)(void *frame, void *self, args…)` with the size `s<slot>` beside it (§3a, ADR 0036). `Box<dyn I, A>`'s drop glue calls `vt->drop` and frees `data` with `A` (a user allocator's `deallocate` gets the table's size and alignment) (type-system.md §37.4). **`dyn fn`** (M13 G3, §36.5) mangles as `DF` (`DG` for `dyn mut fn`), its effect list and its signature; its table's one entry `m0` is a move literal's body, whose `self` is the boxed environment `ceeCE_<id>` (the code pointer, then the captured values; drop glue `ceeD_CE<id>_`) |
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
| `StringView` (M6c) | `cee_str` = `struct { const uint8_t *ptr; uint64_t len; }` from `ceesafe/rt.h` (16 bytes); element code `PZ` (`Z` is the unsized `str`). A literal is `((cee_str){ .ptr = (const uint8_t *)"…", .len = n })`, pointing to static storage. A literal longer than 4095 bytes, the most C99 and C11 require a compiler to accept (§5.2.4.1; clang and GCC warn beyond it under `-Wpedantic`, and MSVC rejects much longer ones), is instead a file-scope `static const uint8_t ceeLS_<k>[] = { 0x…, … };`, emitted once per distinct text, and `.ptr = ceeLS_<k>` (0.15.2). The same goes for a static's initialiser and `Console.write` of a literal. `s[lo..hi]` calls `cee_check_str_range` (range, then character boundaries; `CEE_PANIC_STR_BOUNDARY`); `==`/`!=` call `ceesafe_str_eq`; `s.bytes()` rebuilds the struct as `ceeSl_u8`; printing calls `ceesafe_write_view` |
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
`ceeAI_` initialisers and `ceeAR_` resume functions, §3a); locals `v_<name>_<n>` and `t_<n>`; fields `f_<name>`; variants `v_<name>`; labels `bb<n>`; files `cee_file_<n>`; source locations `ceeL_<n>` (§6); string literals over 4095 bytes `ceeLS_<n>` |

**Packages** (M10 T5, ADR 0013 D4): an item of a dependency, rather than of the package being built, has its
package's name in its C name: `ceeF_geometry_area`, `ceeT_geometry_Point`, `ceeM_14geometry_Point_norm`. The package
name is part of the item's name as the backend sees it (`geometry.Point`, written with `_` in C), so every rule above
applies unchanged, and two packages may declare the same name.

Before M7d, top-level functions were `cee_<name>`. A function named `add_i32` then collided with
the runtime's `cee_add_i32`, and the C failed to compile. The random program generator found this,
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
- **One unit per CeeSafe source file**, at `c/<package>/<path in the package>.c`, its extension replaced (ADR 0082
  §2.1): `c/alpha/src/util.c` and `c/beta/src/util.c`, `c/alpha/src/a/mod.c` and `c/alpha/src/b/mod.c`. Package names
  are unique in a build and paths unique in a package, so two units of packages never share a path. Files given
  without a manifest are the package named by the program's name (`--program`, below): `src/demo.cee` of a program
  `demo` is `c/demo/src/demo.c`. A `..` in such a file's path becomes `__`, so a unit stays under `c/`, and a file
  outside the current directory is named by its file name (§7). Two such files can then give one path (`/p/util.cee`
  and `/q/util.cee`; `../x.cee` and `__/x.cee`): the later, in the order of the files, gets `-2` before `.c`
  (`c/demo/util-2.c`), or the first of `-3`, `-4`, … that no earlier unit has ignoring letter case, so units never
  merge (ADR 0082 §7). A unit holds the functions declared in its file and
  the closures written in them; lib/core's and lib/std's go to `ceesafe_instances.c`. Two units whose paths differ only
  in letter case would be one file on macOS's and Windows's file systems: **N0363**.
- **`ceesafe_instances.c`** holds what is defined once and belongs to no user file: lib/core's functions, generic
  instances, `@export` constants, callbacks, and C's `main`.
- **Linkage.** Every function defined in a unit or in `ceesafe_instances.c` has external linkage, and its prototype in
  the header has no `static`. The header renames each of them with the program's name, `#define ceeF_main
  ceeF_<program>_main` (the name after the family prefix; `ceeARG` becomes `ceeARG_<program>`), so two CeeSafe programs
  linked into one image never define the same symbol. The program's name is `--program`, the package's, or the first
  file's. `@export` names are not renamed.
- **Determinism.** The units, their names and each function's placement are a function of the packages' file lists
  and the program, and two builds are byte-identical.
- **Compiling** (ADR 0082 §2.5). `ceesafec build --split` compiles each C file of the build (the units,
  `ceesafe_instances.c`, the runtime's, the packages' own) to its own object with `-c`, at most `--jobs N` at once
  (default: the number of CPUs), then links the objects in the bytewise order of the files' paths. The C compiler's
  output is printed in that order too, whatever finishes first; `--jobs 1` compiles in that order. The C and the
  executable's inputs do not depend on `--jobs`. MSVC's `cl` is run the same way, `/c` and `/Fo:` per file (one
  `cl` command for every file would give `c/alpha/src/util.c` and `c/beta/src/util.c` one `util.obj`).
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

- **Frames are emitted child-first,** so a frame that embeds another's follows it. A cycle cannot occur: an await on a
  cycle of the await graph is **stacked** (ADR 0056; its NIR role `taskStacked`), and its awaiter's union member is a
  pointer `struct ceeAF_<C> *cee_a<k>`, which needs only the struct's name. The await sets it to
  `ceesafe_async_stack_push(sizeof(struct ceeAF_<C>), _Alignof(struct ceeAF_<C>))`, starts and resumes the callee
  through it, reads its result, and calls `ceesafe_async_stack_pop` on it. The runtime keeps the task's top chunk in
  `cee_task.stack` (the task record is 192 bytes since ADR 0062, `ceesafe_task_slot` with it) and each executor's
  free chunks, which it frees as it ends. Freestanding code has no stack: there a cycle is N0343.
- **Which locals are frame fields** (L1 A3): the parameters, every local that is ever borrowed, every local live just
  after a suspension point (backward liveness over the NIR, where a `StorageDead` reads nothing), and the locals a
  scope's end mentions (it reads its oom operand after resuming). Inside the resume function such a local's C name is
  defined as its field (`#define v_x_3 (cee_frame->v_x_3)`, undefined after the function), so statements are emitted
  as in a sync function, and a pointer to it stays valid while the frame does (type-system.md §40.6). Every other
  local is a C local of the resume function, zero-initialised as a sync function's.
- **Waiting storage.** A task waits at one suspension point at a time, so what each needs only while it waits (an
  awaited child's frame `cee_a<k>`, a join `cee_j<site>`, a worker `cee_b<site>`) shares one union `cee_u`. A scope's
  struct is its own field: it must be zero when its first child is spawned.
- **Layout.** ceesafec computes each frame's size and alignment (`frameReport`, from the fields' C layouts and the
  runtime structs') and asserts them after the struct (`_Static_assert(sizeof(struct ceeAF_<F>) == n, …)`), so the C
  compiler checks the arithmetic; `ceesafec build --explain-frames` prints the same numbers, the locals kept and the
  largest ones (the `frames` corpus family's `.frames` snapshots).
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
  then calls `ceeAI_<G>(&cee_root, args…)`, `ceesafe_async_run(&cee_root, ceeAT_<G>, &ceeAN_<G>)`, and reads
  `cee_root.cee_result`. The thunk adapts `ceeAR_<G>` to the runtime's `bool (*)(void *)`, so no function is called
  through another type.
- **Scopes.** Scope `site` is a field `struct ceeAS_<site> cee_s<site>` of the awaiting frame: the runtime's
  `ceesafe_scope`, then the first error, a `Result<void, E>`. `s.spawn(child)` calls
  `ceesafe_async_spawn(&cee_frame->cee_s<site>.rt, sizeof(struct ceeAF_<C>), ceeAC_<C>, NULL, NULL, &ceeAN_<C>)` for
  the child's async fn `C`
  and initialises the frame it returns with the closure's environment (the closure value, the environment's first
  member, converted back). `ceeAC_<C>` resumes the child and, once it returns, makes an `Err` the scope's first error,
  dropping a later one. The scope's end is a suspension point: `ceesafe_async_scope_done` parks the task until the
  last child returns, then the result is the first error, else `Err(oom)` if a child could not start, else `Ok`, and
  the scope is zeroed for its next run. An async closure's environment keeps a null code pointer: it runs only through
  its frame.
- **Task sets** (ADR 0053). The record is lib/core's `TaskSetRecord<E>` in the frame of `AsyncTasks<E>.run`'s task:
  the runtime's `ceesafe_task_set` (a `ceesafe_scope` with `set` true, the limit, the oom flag, the error slot, the
  task waiting for room, the allocator), then the first child error. `AsyncSetSpawn` emits, in any function (sync too,
  for `trySpawn`): if the admission code is 0, `ceesafe_async_set_spawn(record, sizeof(struct ceeAF_<C>), ceeAC_<C>,
  &ceeAN_<C>)` and the frame's initialisation, or code 3 if it is NULL; a child not started drops its environment.
  `spawnSend` calls `ceesafe_async_set_spawn_send`, then `ceesafe_async_publish`. `Async.setAllocator(record, alloc)`
  (lib/core's, for `tasksIn`; `Async.tasks` is `tasksIn` with `SystemHeap`) is `Builtin(AsyncSetAllocator, [A,
  record])`: `ceesafe_async_set_allocator(record, ceeAA_<A>, ceeAD_<A>)`, whose adapters are emitted as a
  `scopeIn` scope's are. A shared set (spawnSend children) admits and waits for room under the run's lock.
  The child's thunk copies an `Err` to `ceesafe_async_set_error(scope)` if `ceesafe_async_claim_failure` says it is
  the first. In the runtime a set's child error cancels no sibling, a failed allocation stops nothing, and a returning
  child wakes the task waiting for room (`ceesafe_async_set_admit`), as the last one wakes the task waiting for the
  set (`ceesafe_async_set_done`).
- **`s.spawnSend(child)`** (ADR 0038) is a spawn whose Builtin has the role `taskSend`: it calls
  `ceesafe_async_spawn_send` (same arguments as `ceesafe_async_spawn`), initialises the frame, then
  `ceesafe_async_publish(frame)`, which queues the child for any worker. A child records its scope's first error only
  if `ceesafe_async_claim_failure(scope)` says it is the first, which takes the run's lock for a shared scope.
  `Async.runWorkers(n, task)` is `ceesafe_async_workers(n)` before the run's code.
- **Async instances** (ADR 0037, type-system.md §32.5) are async fns like any other, named `ceeAS<n>_<f>` after the
  function they copy (`<n>` the instance's HIR number). The caller awaits the instance; inside it, a call through a
  bound closure is an awaited call of the async closure's body, its frame a member of the instance's union.
- **Calls through `dyn`** (ADR 0036). An async method `m` in slot `k` of a dyn type `D…` has its own frame,
  `struct ceeAF_dyn_<D…>_m<k>`: `cee_state` (always 0, for the debugger), `cee_resume`, the resume adapter of the
  receiver's method, `cee_size`, the result, then the union `cee_impl` with one member `i_<T.m>` for the method of
  every type coerced to the dyn type (and `cee_none`, so it is never empty). Its `ceeAI_` calls the table's entry,
  `x.vt->m<k>(frame, x.data, args…)`. The entry `ceeVT_<T>_<D…>_<k>` stores `ceeVT_…_<k>_r` and the size, then
  starts `T.m` in its member. `ceeAR_` calls `cee_resume`, which resumes `T.m` and, once it returns, copies its result
  into the frame. So an await, `Async.run` and `Async.boxed` lower as for a direct call. `Async.boxed` allocates the
  table's `s<k>`, `offsetof(…, cee_impl)` plus `T.m`'s frame, and gives back `cee_size`. The frame is emitted after
  every impl's, and its size and the union's offset are asserted; `--explain-frames` reports it at each await.
- **Joins.** `race` or `all` at `site` is a field `struct ceeAJ_<site> cee_j<site>` of the awaiting frame: the
  runtime's `ceesafe_scope`, `first` (the child that returned first, 2 for neither), both results, and both children's
  task slots (`ceesafe_task_slot`) and frames, defined before the frame that holds it. The join zeroes it, initialises
  both frames, starts both with `ceesafe_async_start` (the caller's storage: nothing is allocated, nothing freed), and
  waits like a scope's end. `ceeAJ<site>_<k>` resumes child k and, once it returns, keeps its result, notes it if it
  was first, and calls `ceesafe_async_cancel` on the join: always for race and for a timeout, on an `Err` for all. A
  timeout's second child (ADR 0046) is a function, not a closure: its initialiser is given the deadline, and its
  result field has its own type.
- **Blocking.** `Async.blocking(body)` at `site` is a field `struct ceeAB_<site> cee_b<site>`: the runtime's
  `ceesafe_blocking`, the closure value and its result. `ceesafe_async_blocking_start` runs `ceeABT_<site>` on a worker
  (`ceesafe_thread_start`; inline if none can start), which calls the closure through its code pointer, stores the
  result and calls `ceesafe_async_blocking_finish`. The task waits at a suspension point on
  `ceesafe_async_blocking_ready`, which joins the worker once it is done.
- **Frames from an allocator** (L1 A3). For each allocator type `A` async code takes frames from, `ceeAA_<A>(size,
  align)` and `ceeAD_<A>(p, size, align)` call its `allocate` and `deallocate` with a `Layout` (§30.3), or the runtime's
  heap for `SystemHeap` and `FailingAllocator`. `Async.boxed(alloc, g(…))` is a suspension point whose waiting storage
  is a pointer `cee_x<k>`: the frame from `ceeAA_<A>` (on `NULL` the value is `Err` and `g` does not run), then
  `ceeAI_<G>` and `ceeAR_<G>` through the pointer, then the result in `Ok` and the frame back through `ceeAD_<A>`. A
  `scopeIn` scope's spawns pass its adapters to `ceesafe_async_spawn`, which takes the child's block from them and gives
  it back when the child returns.
- **Placement** (§2.2) and closure specialisation skip async fns: a box's C-stack slot would not survive a suspension.
- **Frame information** (L1 A3, for debuggers). After its resume function, each async fn `F` has
  `static const uint32_t ceeAL_<F>[]`, the source line of each resume state (`[0]` the line of `F`'s name, `[k]` the
  line of suspension point k's statement), and `const ceesafe_frame_info ceeAN_<F> = {name, file, ceeAL_<F>, count}`:
  `F`'s name as `ceesafe-bt` shows it (`Counter.next`, or `async closure`), and its path as `#line` names it. Every
  task carries its fn's record: `ceesafe_async_run`, `_spawn` and `_start` take it. The runtime reads `cee_state`, the
  frame's first field, to find the line (§6). With `--split` the record has external linkage and an `extern`
  declaration in the header; the thunks there are `CEE_UNUSED`.

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
  - `ceesafe_async_run(frame, resume, info)` resumes the root frame until it returns. Between resumes it waits, in `poll()`,
    for what the parked task registered: a deadline on the monotonic clock (`ceesafe_async_wait_until`, from
    `ceesafe_async_now`) or a descriptor becoming writable (`ceesafe_async_wait_writable`). A task that parks with
    nothing registered aborts with a message.
  - `ceesafe_async_write(fd, bytes, length)` writes at most `PIPE_BUF` bytes if `fd` is writable now, after flushing
    standard output or error: a count, 0 if it is not writable, or a negated errno (ADR 0031 §3.5).
  - One executor per thread: the root task and the scopes' children run in turn from a queue; a parked task waits on
    what it registered or on its scope's last child (`ceesafe_async_spawn`, `ceesafe_async_scope_done`). A child's
    task and frame are one `ceesafe_alloc` block, so fault injection reaches it; `NULL` sets the scope's `oom`.
  - Freestanding (ADR 0039): the same file, compiled with `CEE_FREESTANDING`, keeps the queue, scopes, joins,
    shields, cancellation and `AsyncSignal`, and leaves out descriptors, reactors, operations, channels, workers and
    `Async.blocking`. Time, idling and waking are the environment's `ceesafe_env_*` hooks (profiles.md §4.2). The
    executor is reached through `ceesafe_env_slot()`, the wake order and the sweep's root are its fields, and there is
    no global state. Deadlines are found by walking the parked tasks.
  - `AsyncSignal` (ADR 0039): `ceesafe_async_signal_raise` sets `raised`, takes the waiting task and wakes it, `busy`
    meanwhile. A task that loses that race waits until the raise is done before it goes on, so no raise touches a
    task that has returned.
  - Work stealing (ADR 0038): with `ceesafe_async_workers(n)`, n > 1, the run starts `n − 1` threads, each with its own
    executor (reactor, timers, wake pipe), and joins them when the root returns. The run shares a queue of published
    `spawnSend` children, which a worker with nothing ready takes, oldest first. A started task stays on its worker. A
    scope with such children is shared: its count, first error and waiter change under the run's mutex. Its
    cancellation is atomic and is announced to every worker (an epoch each worker compares when it wakes). Its last
    child wakes the waiting parent through the parent's executor (`cee_wake_task`). Each worker counts the run as a
    reason to wait for a wake, so its non-blocking wake pipe is watched. The `tasks` command lists the stopped thread's
    executor.
  - Cancellation (L1 A2b): a task is cancelled if its own flag is set, or its scope's (a child failed or could not
    start), or the scope's owner is, transitively. `ceesafe_async_status()` is 0, 1 (cancelled) or 2 (inside
    `ceesafe_async_shield_enter(ms)` … `_leave()`, past its deadline; while shielded, cancellation reads as 0). A
    cancellation unparks the tasks it reaches that wait on a deadline, a descriptor or a cancellable wake; a
    shield's deadline wakes its task. It finds them without walking the executor's parked tasks (ADR 0062). Each
    scope lists its children that run on its worker, linked through `cee_task`'s siblings, and each task lists the
    scopes it owns from their first child until they are done (`ceesafe_scope`'s `outer`). A cancellation visits the
    scope's children and their scopes, depth first. A shared scope's `spawnSend` children are not listed: a visit that
    meets one walks every parked task, as the cancellation sweep and another worker's epoch do. `ceesafe_scope` is 48
    bytes.
  - `CEESAFE_CANCEL_AT=k` cancels the running `Async.run`'s root task at the program's k-th suspension, and
    `CEESAFE_CANCEL_REPORT=1` prints `ceesafe cancel: points=N woken=W visited=V` after each `Async.run`
    (`testing.md`): the suspensions, the tasks cancellations woke, and the tasks they looked at.
  - `ceesafe_async_start(scope, slot, frame, resume, info)` starts a child in the caller's storage (`race`, `all`), and
    `ceesafe_async_cancel(scope)` cancels a scope's children.
  - For debuggers (L1 A3): `ceesafe_async_tasks()` describes the calling thread's tasks in a static buffer, one line
    each, `name at file:line (how)`: the running task, the ready ones, then the parked ones with what they wait on
    (their scope, a wake, a descriptor or a deadline). The line is where the task's own async fn last suspended
    (`ceeAL_<F>[cee_state]`), and a child is indented two spaces per scope under its owner. `ceesafe_async_dump()`
    writes the same text to standard error.
  - Waking from another thread (A2c): a task parked on a wake has `woken` set atomically by the waking thread, which
    then writes a byte to its executor's wake pipe (opened on first need); `poll` watches the pipe while a task waits
    to be woken, and the executor readies every woken task. A task woken between its check and its park is readied at
    once. Programs with async link the thread runtime.
  - Channels (ADR 0032 §5): `ceesafe_chan_*`, element-blind, under a spin lock; a task's wait node lives in its frame,
    and every change wakes the queued tasks through their executors.
  - Readiness (A2d): a task waits for a descriptor to be readable or writable (`ceesafe_async_wait_readable`,
    `_wait_writable`); `poll()` is the reactor on every POSIX host, with any number of descriptors. Sockets are
    `ceesafe_async_tcp_*`, `ceesafe_async_read`, `_send`, `_shutdown_write` and `_close` in `ceesafe_rt_io.c`: a value,
    `CEE_ASYNC_WOULD_BLOCK`, or a negated errno. ADR 0044's addresses cross as rt.h's `ceesafe_endpoint` (family 4
    or 6, port, scope, 16 bytes; lib/std passes its fields): `ceesafe_async_tcp_listen_at` (with a backlog and the
    `CEE_LISTEN_*` flags), `_connect_at`, `ceesafe_async_op_connect_at`, `ceesafe_async_socket_name` (the result
    `(family << 16) | port`), `ceesafe_async_socket_option` (`CEE_SOCKET_OPTION_*`), `ceesafe_async_shutdown_read`
    and `ceesafe_interface_index`; the blocking `TcpStream`'s setters are `ceesafe_tcp_option` and
    `ceesafe_tcp_shutdown_read`. The text calls parse IPv4 or IPv6 (`ceesafe_endpoint_parse`). IOCP's AcceptEx
    address buffer, sized for IPv6, shares the operation record with a read's WSABUF, so the record stays 192
    bytes. `ceesafe_async_poll` serves code with its own wait loop (the
    interpreter). Woken tasks run in the order they were woken.
  - Portability (L1 A4a): the runtime's atomics are rt.h's `ceesafe_atomic_*` (§38.3: the `__atomic` builtins, or
    `_Interlocked*` under MSVC), and a block's alignment is `cee_max_align_t`. On Linux the file defines
    `_POSIX_C_SOURCE 200809L` before its includes: under `-std=c11` glibc hides `clock_gettime` otherwise. On Windows
    the wake pipe is an auto-reset event: a wake calls `SetEvent`, and an executor whose tasks wait to be woken waits
    in `WaitForSingleObject` until the next deadline.
  - Windows sockets (A4a): a task waiting on a socket has it signal the same event (`WSAEventSelect`, `FD_READ`,
    `FD_ACCEPT` and `FD_CLOSE`, or `FD_WRITE`, `FD_CONNECT` and `FD_CLOSE`); the executor then asks `WSAPoll` with no
    wait which are ready, and waits on the event only if none is. A socket that becomes ready between the poll and the
    wait has already signalled, so no readiness is lost. An unparked task's socket is detached (`WSAEventSelect(s,
    NULL, 0)`). A console or file descriptor has no readiness there, so its task retries at once.
    `ceesafe_async_poll` uses `WSAPoll` for sockets, as the interpreter needs.
  - The executor's structures (L1 A4b). Nothing a wait does walks the parked list, which only cancellation and
    `ceesafe_async_tasks` walk; it is doubly linked, so a task leaves it in constant time:
    - deadlines (a shield's end counting as one) are a binary min-heap, so due tasks run earliest first;
    - descriptor waiters are a table by descriptor, readers and writers each in the order they parked;
    - the tasks waiting to be woken are a list of their own.
  - The reactor (A4b): kqueue on macOS and the BSDs, epoll on Linux, created at the first wait. A parking task arms
    its descriptor and direction one-shot (kqueue `EV_ONESHOT`, queued and sent with the next `kevent` wait; epoll
    `EPOLLONESHOT` through `EPOLL_CTL_MOD`, or `ADD` for a descriptor it does not have). A readiness event readies every
    task waiting in that direction; each retries. A task that leaves early (cancelled, or due) disarms what no task
    waits for any more. One-shot arming is one call per park, and no state outlives the waits, so a descriptor closed
    and reopened under the same number needs nothing. epoll refuses regular files (`EPERM`): their tasks count as
    ready. A failed arming readies its tasks, whose retry sees the error. The wake pipe stays registered.
    `CEESAFE_REACTOR_POLL=1`, or a host with neither, uses `poll()` over the waited descriptors.
  - Windows (A4b): a waiting socket is selected for the executor's event for every network event once, when its task
    parks (`ceesafe_socket_select`; a call only when that changes), since two tasks may wait on one socket in opposite
    directions; the end of `Async.run` deselects them (`ceesafe_socket_unselect`) before closing the event.
  - Measured by `benchmarks/async-reactor/run.py` (idle connections whose both ends wait, and one-byte round trips on
    another; ROADMAP §10): the time per round trip stays flat with kqueue and epoll as the idle connections grow, and
    grows with them with `poll()` and on Windows.
  - **Operations** (ADR 0035): lib/std's socket operations (accept, connect, read, send) are steps over an
    `AsyncOp` record in the awaiting frame (`ceesafe_async_op`, 192 bytes): `ceesafe_async_op_read`, `_send`,
    `_accept` and `_connect` return the result once the operation is complete, or `CEE_ASYNC_PENDING` (park and
    step again). On a readiness backend a step is an attempt that registers the wait if it would block. On a
    completion backend the first step submits, and the task waits on the operation (`on_op`, counted apart from
    wakes, so no wait scans for it) until its completion readies it. `ceesafe_async_op_cancel` cancels an operation
    in flight (`CancelIoEx`, `IORING_OP_ASYNC_CANCEL`) and returns true: the task then waits, not cancellable, for
    the completion, so a lent buffer outlives the kernel's use (type-system.md §40.7). A value completed meanwhile is
    kept: the operation returns it, and the next one sees the cancellation. Otherwise it releases what the record
    holds (a half-made connection) and returns false.
  - **DNS** (ADR 0048): `ceesafe_dns_start` copies the name into a record of platform memory and queues it; at most
    `CEE_DNS_THREADS` (4) detached resolver threads take lookups first come first served, and a thread ends when the
    queue is empty. One lock guards the queue, each record's state and its waiter.
    - `ceesafe_dns_poll` registers the current task to be woken, cancellably, as a channel's wait is, and the thread
      wakes it under the lock when the lookup is done.
    - The resolver is in `ceesafe_rt_async.c`, which only programs that use async link; `ceesafe_rt_io.c`, which
      every hosted program links, gives it `ceesafe_winsock_ready` on Windows.
    - `ceesafe_dns_end` unregisters: a done or queued lookup is freed, a running one marked abandoned, which its thread
      frees without waking anyone.
    - The test hook `CEESAFE_DNS_DELAY_MS` delays each lookup (tools/dns_check.py).
  - **Message operations** (ADR 0045): a vectored send's record is lib/std's `AsyncMsgOp` (`ceesafe_async_msg_op`,
    512 bytes), which starts with an operation's header, so `ceesafe_async_op_cancel` and the completion paths take
    it too. Before the first step `ceesafe_async_msg_part(op, index, bytes, length)` puts each non-empty part in it
    (at most `CEE_MSG_PARTS`, 8; each capped at 2^30 bytes, as a send's chunk); then `ceesafe_async_op_sendv(op, fd,
    count)` steps: `sendmsg` with `MSG_NOSIGNAL` (Linux) or `writev` (macOS and the BSDs, whose sockets have
    `SO_NOSIGPIPE`), an overlapped or plain `WSASend` with the record's `WSABUF`s (IOCP, `WSAPoll`), or
    `IORING_OP_SENDMSG` with the record's `msghdr`. CosmoOS userland sends the first non-empty part. The record also
    holds a peer address and a control buffer (128 bytes; 96 on Windows) for datagram messages (U6).
  - **Sending a file** (ADR 0051): `ceesafe_async_op_sendfile(op, socket, file, offset, length)`, with the plain
    operation record and `file` pointing at a `File` (its `int32_t` descriptor), steps: `sendfile(socket, fd, &offset,
    count)` on Linux (epoll, and io_uring through the same readiness step), each call at most 0x7FFFF000 bytes;
    `sendfile(fd, socket, offset, &len, NULL, 0)` on macOS, where `EAGAIN` with `len > 0` is a partial count; an
    overlapped `TransmitFile` (found with `WSAID_TRANSMITFILE`) on IOCP, the offset in the `OVERLAPPED` and at most
    2^31 − 2 bytes a call, the handle `_get_osfhandle(fd)`. It returns `CEE_ASYNC_NO_SENDFILE` (`INT64_MIN + 2`) on
    `WSAPoll`, on CosmoOS, and where `sendfile` refuses the file (`EINVAL`, `ENOSYS`; macOS `ENOTSUP`, `ENOTSOCK`,
    `EOPNOTSUPP`): lib/std then copies through `ceesafe_file_read_at(file, buffer, length, offset)`, `pread` (at most
    2^30 bytes) or a `ReadFile` with the offset in an `OVERLAPPED` on Windows, which every hosted program links
    (`ceesafe_rt_io.c`). The interpreter has no file send: it copies, reading at the offset by opening the file's path
    again (its stage 0 `File` cannot seek).
  - **Datagrams** (ADR 0050): `ceesafe_async_udp_bind(family, bytes, port, scope, flags, send_buffer,
    receive_buffer)` makes a nonblocking `SOCK_DGRAM` socket with the `CEE_UDP_*` options (`IP_PKTINFO` or
    `IP_RECVPKTINFO`, `IPV6_RECVPKTINFO`; `IP_RECVTOS`, `IPV6_RECVTCLASS`; Windows `IP_RECVECN`, `IPV6_RECVECN`, and
    `SIO_UDP_CONNRESET` off). macOS needs `__APPLE_USE_RFC_3542` for the IPv6 packet info. A send fills the message
    record's parts, then `ceesafe_async_msg_send_to` writes the destination (IPv4-mapped for an IPv6 socket) into the
    record's name and builds the control messages: packet info for a source address, `IP_TOS`/`IPV6_TCLASS` for ECN on
    Linux (`IP_ECN`/`IPV6_ECN` on Windows; elsewhere the socket's option is set for the send and cleared after it), and
    `UDP_SEGMENT` on Linux. `ceesafe_async_op_sendmsg` steps it: `sendmsg`, `IORING_OP_SENDMSG`, or `WSASendMsg`
    (overlapped on IOCP); `CEE_ASYNC_NO_SEGMENTS` (`INT64_MIN + 3`) where segments cannot go in one call, and lib/std
    sends them one by one. An empty datagram is one empty part. `ceesafe_async_op_recvmsg` receives into one part with
    the record's name and control buffer: `recvmsg`, `IORING_OP_RECVMSG`, or `WSARecvMsg` (found with
    `WSAID_WSARECVMSG`), where `WSAEMSGSIZE` is a truncated datagram, not an error. Then `ceesafe_async_msg_address`
    reads the peer from the name and the local address from the packet info, and `ceesafe_async_msg_flags` the
    truncation (`MSG_TRUNC`) and the ECN bits. `EMSGSIZE` is `InvalidInput`. Windows' control buffer is 72 bytes, so
    the record still fits 512. `ceesafe_async_udp_send` and `ceesafe_async_udp_recv` are one nonblocking
    `sendto`/`recvmsg` each, for the interpreter.
  - **IOCP** (Windows' default): each thread has one completion port for its life, because a socket can be tied to
    only one port. A socket is tied to its thread's port at its first operation (`ceesafe_socket_associate`).
    Operations are `WSARecv`, `WSASend`, `AcceptEx` (a new overlapped socket, given an id when it completes) and
    `ConnectEx` (bound, tied, then connected). The executor waits in `GetQueuedCompletionStatusEx`, and another
    thread's wake posts to the port. A socket tied to another thread's port is used by readiness steps, polled
    between slices of 1 ms. `CEESAFE_REACTOR_POLL=1` keeps `WSAPoll` and the event.
  - **io_uring** (Linux, opt-in with `CEESAFE_REACTOR_URING=1`): a ring set up through the raw system calls (no
    liburing), with a completion queue of 16384, signalling an eventfd in the epoll set, so the executor waits in
    epoll as always and drains the ring when the eventfd fires. `IORING_OP_RECV`, `_SEND` (`MSG_NOSIGNAL`), `_ACCEPT`
    (nonblocking) and `_CONNECT` (a new nonblocking socket, the address in the record). A submission refused with
    `EBUSY` (completions overflowed) is retried after a drain, and a drain that leaves `IORING_SQ_CQ_OVERFLOW` set
    flushes with `IORING_ENTER_GETEVENTS`. Deadlines, consoles and the wake pipe stay on epoll. A kernel that refuses
    the ring leaves epoll alone.
  - The interpreter's steps are readiness ones over its real sockets.
- The generated `main` calls `ceesafe_rt_start()` (reads `CEESAFE_FAIL_ALLOC_AT`, `CEESAFE_ALLOC_REPORT`
  via `ceesafe_platform_config`) and returns through `ceesafe_rt_finish(code)`, which prints the
  allocation report if asked. `Process.exit` and panics skip the report.
- The allocation counters are kept only when something reads them (ADR 0059): `CEESAFE_ALLOC_REPORT`,
  `CEESAFE_FAIL_ALLOC_AT`, or `ceesafec test`'s `ceesafe_test_begin`. That is decided before the program's first
  allocation and never changes afterwards. Without any of them, an allocation, reallocation or free touches no
  counter, and so makes no atomic operation.


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
  directory to a relative one. Any other absolute path is reduced to its file name. A dependency's
  file is named by its package and its path in it, `alpha/src/util.cee` (ADR 0082 §2.1), wherever the
  package is. Generated C therefore never contains a host path (`build-integration.md` §6).
- `ceesafec build` runs the C compiler with the current directory as its working directory. The
  debug info's compilation directory then makes the source paths resolve, so a debugger finds
  the `.cee` files of a build of files and of the root package (run from its directory). It does
  not find a dependency's: `alpha/src/util.cee` is relative to the root package, not to the current
  directory, so the debugger needs a source map (lldb's `target.source-map`, gdb's `directory`)
  until a later stage of ADR 0082 fixes it (ADR 0082 §7).
- Locals keep stable names: `v_<name>_<n>` for a named local, `t_<n>` for a temporary.
  A C debugger therefore shows recognisable variables (`v_count_3`).
- **Temporaries and debug info.** Without directives, a temporary is declared `CEE_TEMP` (`rt.h`): under clang
  `__attribute__((unused, nodebug))`, under GCC `unused` (it has no `nodebug` for variables), elsewhere nothing. An
  optimised build otherwise gives each of a function's temporaries a debug variable with a location list over the
  whole function, repeated at every inlined call; in CosmoOS's kernel, built `-O2 -g`, the location lists were 4.8
  bytes per byte of the generated C's code, against 0.6 for its hand-written C, and most of the kernel's 12 MB
  (2026-11-05). `nodebug` changes no code. With directives (`ceesafec build -g`), temporaries stay `CEE_UNUSED` and
  visible to a debugger. Named locals are visible either way.
- Directives never change the code itself. A program with directives must behave exactly like
  one without them. The run corpus is built both ways (§8).

## 7a. Implementation notes (M1, increment 1c)

- Every CeeSafe function becomes `static CEE_UNUSED` with a prototype. CeeSafe allows unused functions,
  parameters and locals, and `-Wunused-*` must stay clean under `-Werror`.
- Locals are declared at the top of the function, zero-initialised as in §5 (scalars only in M1); a temporary is
  `CEE_TEMP` rather than `CEE_UNUSED` without directives (§7). Only blocks that are jump targets get a label.
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
  produce the same results. `tools/debugger_check.py` builds a program with `-g` and checks that a debugger stops at
  its `.cee` lines (`tooling.md` §3.4).
- M7: a Csmith-style random program generator for CeeSafe; three-way
  differential testing (clang/gcc × opt levels), plus a reference
  interpreter on NIR (`ceesafec run --interp`) as an oracle (`milestone-7.md`).
