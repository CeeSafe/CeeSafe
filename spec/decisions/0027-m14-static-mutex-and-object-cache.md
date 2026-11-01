# ADR 0027 — M14: a static `Mutex`, and `ObjectCache<T>`

> **Inherited.** ADR 0027 was accepted before CeeSafe's fork (Decision 0006), when the language had its
> earlier name. In this record, "the reference" is that language and project and "the reference compiler" its compiler:
> read them as CeeSafe and `ceesafec`. Links to documents CeeSafe did not keep are given as text.

- Status: **Accepted** (project owner, 2026-10-29), both questions as recommended (§5). The wording is the agent's.
- Scope: the two parts of ADR 0025's v1 API (§3.3) that §7 left open: a `Mutex<T>` in a `static` item, and
  `ObjectCache<T>`, a typed slab cache. Both need a language addition. CosmoOS's roadmap 2.0 needs both for SA2
  (object caches) and SA3 (locks and waits in statics).

## 1. Context

### 1.1 A static mutex

- **The C.** `struct mutex` (CosmoOS `kernel/include/kernel/mutex.h:20`) holds a spinlock, an owner, a
  `struct waitqueue`, a name, a lockdep class, `pi_active`, a `struct list_node pi_link` and `handoff`. `mutex_init`
  (`kernel/scheduler/mutex.c:13`) makes `pi_link` and the wait queue's two lists point to themselves
  (`list_init`, `waitqueue_init`).
- **CosmoOS's own static initialisers** for such state take the object's own name: `LIST_HEAD_INIT(name)` is
  `{ &(name), &(name) }` and `WAITQUEUE_INIT(name)` uses `(name).waiters` (`list.h:21`, `wait.h:58`). There is no
  `MUTEX_INIT`.
- **The reference's statics** (type-system §39.4) build C state only through a C constant macro whose arguments are static
  initialisers. None of those can name the static being defined, so a self-pointing list cannot be written. Static
  spin locks work because `SPINLOCK_INIT` has no self-pointers (ADR 0025 §7).
- **SA3 needs more of the same:** wait queues, completions and list heads in statics all point to themselves.

### 1.2 An object cache

- **The C.** `kmem_cache_create(name, object_size, align)` allocates and returns a cache, or NULL;
  `kmem_cache_alloc(cache, flags)` and `kmem_cache_free(cache, obj)` (`kernel/include/kernel/kmalloc.h:47-53`).
  CosmoOS creates its caches at start-up into globals (`kernel-services/network/mbuf.c:26-27`, `kmalloc.c:43`).
- **What the reference lacks:**
  - **Moving a value into raw memory.** `p.write(v)` and `p.read()` are defined only for `Copy` types (§27.3, N0282).
    A cache's objects are generally not `Copy` (they own boxes, or have a `drop`), so a cache box cannot be filled
    or emptied.
  - **A type's size and alignment as a value.** `kmem_cache_create` needs them. The compiler knows them
    (`TargetInfo`), and `Box` passes them to an allocator as a `Layout`, but CeeSafe code cannot ask for one.
- **Why not `Box<T, A>`:** a box's allocator is stateless (C5, §30), and a cache is state. A stateful allocator would
  make every box carry its allocator: two pointers instead of one, through placement, niche optionals and drop glue.

## 2. Options

### 2.1 Static mutex

| | A. `here`: a constant initialiser may take the address it initialises | B. Lazy: a `StaticMutex<T>` that runs `mutex_init` at its first lock | C. None: mutexes made at start-up |
|---|---|---|---|
| Language | one new static-initialiser form | none | none |
| CosmoOS | a `MUTEX_INIT_AT(p, name)` macro in `mutex.h` | none | none |
| Run-time cost | none | an acquire load per lock, and a spinlock for the first | an allocation and a pointer per lock |
| Also serves | wait queues, completions, list heads (SA3) | mutexes only | — |

### 2.2 Object cache

| | A. Moves through raw pointers, `Layout.of<T>()`, and a library cache | B. Stateful `Box` allocators | C. Wait |
|---|---|---|---|
| Language | lift `Copy` from `read`/`write` (unsafe); one built-in | `Box<T, A>` carries `A`'s state | — |
| Size of a cache box | two pointers (object, cache) | two pointers for every box with a stateful allocator | — |
| Reach | any unsafe container code (rings, intrusive lists) | caches and other stateful allocators | — |

## 3. Decision

### 3.1 `here` (option A for the mutex)

- **`here`** is a static initialiser (§39.4) valid only as an argument of a C constant macro whose result
  initialises a place. Its value is that place's address, and its type is `*mut T` for the place's type `T`. In
  ```ceesafe
  @constant
  public fn constant(name: *CChar, data: T) -> Mutex<T> => Mutex { raw: MUTEX_INIT_AT(here, name), data: data };
  ```
  `here` is the address of the field `raw` in whichever static the constructor builds. Through nested `@constant`
  constructors the place is the full path (`&ceeS_TABLE.f_lock.f_raw`).
- **C.** It lowers to an address constant (C11 §6.6p9: `&`, `.`, `->` and `*` may form one, and no object is read),
  so the macro may build self-pointers from it: `LIST_HEAD_INIT((p)->wq.waiters)` expands to
  `{ &((p)->wq.waiters), &((p)->wq.waiters) }`.
- **Rules.** `here` elsewhere is N0338. A `@constant` function that uses it is compile-time only (§39.4, as for C
  state), so it is never called at run time, where there is no fixed place. The interpreter cannot evaluate C
  constant macros already (§39.5), so nothing changes there.
- **lib/kernel.** `Mutex<T>.constant(name, data)` with CosmoOS's new macro:
  ```c
  /* A mutex in static storage, as mutex_init leaves one; p is its own address. */
  #define MUTEX_INIT_AT(p, n) { .lock = SPINLOCK_INIT(n), .owner = NULL, \
      .wq = { .lock = SPINLOCK_INIT(n), .waiters = LIST_HEAD_INIT((p)->wq.waiters), \
              .callbacks = LIST_HEAD_INIT((p)->wq.callbacks) }, \
      .name = (n), .class = 0, .pi_active = 0, .pi_link = LIST_HEAD_INIT((p)->pi_link), .handoff = NULL }
  ```
  A CosmoOS self-test compares a static mutex made by it with one made by `mutex_init`, member by member, and locks
  it under lockdep. A change to `struct mutex` must keep the two equal, as for `SPINLOCK_INIT`.

### 3.2 Moves through raw pointers, `Layout.of<T>()`, `ObjectCache<T>` (option A for the cache)

- **`p.write(v)` and `p.read()` for any `T`** (§27.3; unsafe, as now):
  - `p.write(v)` moves `v` to `p`, without reading or destroying what was there, for `p: *mut T`. A `Copy` `v` is
    copied, as now. For a type with a `@placed init` (§33.3) the store is followed by `init` at `p`, as
    `Box.create` does.
  - `p.read()` moves the `T` at `p` out. The memory then holds no value: the unsafe code must not read it again,
    drop it or let anything else do so before writing it (U-rules, `unsafe.md`). For `Copy` `T`, as now. For a type
    with C state (§33), `read` is N0282: C state never moves.
- **`Layout.of<T>()`**, a `@constant` built-in: `T`'s size and alignment from `TargetInfo` (in C,
  `sizeof`/`_Alignof` of its C type). It is a static initialiser too.
- **lib/kernel** (ADR 0025's API, §3.3 there):
  - `ObjectCache<T: Send>`, `@threadSafe`: `constant()` (empty, for a static), `setUp(name) -> Result<void,
    AllocError>` (once, at start-up: `kmem_cache_create(name, Layout.of<T>().size, Layout.of<T>().align)`, stored
    atomically), and `alloc(value) -> Result<CacheBox<T>, AllocError>` (`kmem_cache_alloc`, then `p.write(value)`;
    on failure the value is dropped, as `Box.create` does). `alloc` before `setUp` is `Err(OutOfMemory)`.
  - `CacheBox<T>`, `@noncopy`, two pointers: `get`, `getMut`, and a `drop` that reads the value out, drops it and
    returns the memory (`kmem_cache_free`). It is `Send` when `T` is.
  - A cache in a box, not a static, would let its boxes outlive it. So v1 has static caches only, the form CosmoOS
    uses, and a cache is never destroyed.

### 3.3 What changes elsewhere

- Type-system §27.3 (`read`/`write`), §30.1 (`Layout.of<T>()` beside `Layout`), §39.4 (`here`, and `Layout.of` as
  a static initialiser), `unsafe.md` (the moved-out state) and ownership.md's U-rules.
- Diagnostics: no new codes. `here` misused is N0338, and `read` of C state is N0282.
- Tests: corpus check-fail for `here` outside a macro argument, a run-time call of a function using it, and `read`
  of C state; run-pass for `read`/`write` of a type with a drop (each dropped once, under ASan and the interpreter);
  `k3.py` for a static `Mutex` and an `ObjectCache`; CosmoOS self-tests `cee-kcore` extended (in a later pin bump).

## 4. Consequences

- Static mutexes, and later static wait queues and completions, cost nothing at run time and are made as CosmoOS's
  own C makes them, with lockdep's classes from their names.
- `read` and `write` of non-`Copy` values are new ways for unsafe code to duplicate or leak ownership. They are
  unsafe, and each use needs its `/// Safety:` argument (U2). The safety-audit pack (`safety-audit.md`) gains them
  for K6.
- `ObjectCache<T>` objects are not `Box`es: code that takes a `Box<T, A>` does not take a `CacheBox<T>`.
- **Not decided here:** destroying a cache (needs boxes scoped to their cache), stateful `Box` allocators, and
  per-CPU object caches.

## 6. Implementation (2026-10-29)

- **Moves through raw pointers** (type-system §27.3, `unsafe.md` U2): `p.read()` and `p.write(v)` take any `T`; the
  volatile forms still copy. The NIR move checker lets a value leave `*p` for a raw `p` (`NirPlace.IsRawDeref`), the
  drop elaborator does not drop the old contents of `*p`, and a write of a type with `@placed` methods is followed
  by `PtrPlace`, emitted as its `ceePL_` helper (the in-place checker accepts the literal there, as for `Box.create`).
  `p.read()` of a CeeSafe struct holding C state is N0282.
- **`layoutOf<T>()`** (§30.1), not `Layout.of<T>()`: the reference has no type arguments on a member call, so it is a function,
  like `truncate<T>`. Folded from `TargetInfo` to a `Layout` literal; for a C-laid-out `T`, C's `sizeof`/`_Alignof`.
- **`here`** (§39.4–§39.5): typed by the macro parameter it is given to, checked against the macro's result type,
  emitted as the address of the place being initialised. lib/kernel's `Mutex<T>.constant` uses CosmoOS's
  `MUTEX_INIT_AT`, which `integrations/cosmoos/k3/mutex_init_at.h` supplies to `k3.py` until CosmoOS's `mutex.h`
  defines it.
- **`ObjectCache<T>`** and **`CacheBox<T>`** (`lib/kernel/cache.cee`) as §3.2 says, with two differences found in
  the implementation:
  - `T` holds no C state in v1: a cache box moves its value out to drop it, and C state never moves. A drop in place
    would lift this.
  - `CacheBox<T>` is not `Send`, like a `KernelHeap` box: raw pointers are not `Send` (§38.1), and a `@threadSafe`
    declaration would claim `Share` for any `T`.
- **Tests:**
  - run-pass `raw-moves.cee` and `layout-of.cee`, every C configuration and the interpreter;
  - check-fail `raw-moves-c-state.cee`, `layout-of.cee` and `static-here.cee`; the changed
    `raw-pointer-types.cee`, whose non-`Copy` `read`/`write` are now valid;
  - emit-c `static-here.cee`: `MUTEX_INIT_AT((&ceeS_TABLE.f_raw), …)`;
  - `k3.py` (`k3/statics.cee`: a static `Mutex` and an `ObjectCache`), both architectures, one file and split.
- **Found in a boot** (2026-10-29, the reference compiler 0.3.0, CosmoOS branch `ceesafec-0.3.0`): `MUTEX_INIT_AT` matches `mutex_init`
  member by member, but `ObjectCache.setUp` failed for `T` aligned below 16: CosmoOS's `kmem_cache_create` refuses an
  alignment below `KMALLOC_MIN_ALIGN` (`slab.c`, `cache_setup`), and asks for that minimum with 0. lib/kernel now
  passes 0 for alignments up to 16; `cee-kcore` then passes. `k3.py` only compiles, so it could not see this.
- **Not yet run (superseded by the above):** a static mutex compared with `mutex_init`'s, and the cache, in a booted kernel. That is CosmoOS's
  side, once a the reference compiler release carries this: `MUTEX_INIT_AT` in `mutex.h` and `cee-kcore` extended.

## 5. Questions for the owner

1. A static mutex through `here` and a CosmoOS `MUTEX_INIT_AT` macro (A)? (Recommended; B needs no language change but
   costs a check per lock and covers mutexes only.)
2. Lift `Copy` from unsafe `p.read()`/`p.write(v)` and add `Layout.of<T>()`, for a lib/kernel `ObjectCache<T>` with
   static caches only (A)? (Recommended.)
