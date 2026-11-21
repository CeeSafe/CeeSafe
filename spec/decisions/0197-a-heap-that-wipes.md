# ADR 0197 — A heap that wipes, and a wipe the C compiler keeps

- Status: **Accepted** by the owner, 2026-11-21 ("Accept all as recommended": every question as §5 or §6 recommends; release order: a patch first, then the next minor). Proposed the same day. Raised by CosmoStore's CEESAFE_GAPS G42 ("a value with a wiping `drop` leaves its
  bytes behind when moved, and a growing vector frees its old buffer unwiped") and G34 (`HmacSha256` leaves its key in
  freed memory); `~/dev/CeeSafeApps/cosmostore/CEESAFE_GAPS.md`. Measured with ceesafec 0.84.1 on an M1 (macOS, Apple
  clang 21), §1.3.
- Scope: the runtime (`runtime/core/ceesafe_rt_core.c`, `runtime/include/ceesafe/rt.h`: one allocator id and one wipe
  function), a third built-in allocator type `SecretHeap` (type-system.md §18.1, §23, §24, §30.2; the checker's
  allocator rules and `src/backend/c_helpers.cee`'s allocator ids), one built-in function `Secret.wipe` in lib/core,
  lib/std's key holders (`Aes256Gcm`, `HmacSha256`), the interpreter's model of the heap, a test-only freed-block scan
  (testing.md), the guide. No change to moves, drop elaboration or the IR. A type attribute that wipes on every move
  is weighed and not taken (§2.4).

## 1. Context

### 1.1 Where secret bytes are left

- **Growth.** `ceesafe_vec_grow` (`ceesafe_rt_core.c:162`) reallocates a full buffer with `ceesafe_realloc` (line
  176), which ends in the C library's `realloc` (`realloc_block`, line 1042; `ceesafe_platform_realloc`,
  `ceesafe_rt_hosted.c:232`). When the block moves, the old one goes back to `malloc` with every byte as it was. A
  `Vector<MasterKey>` of 64 keys frees five blocks (4, 8, 16, 32 elements) that still hold the keys pushed so far.
  `String<A>`, `SlotMap` and `StableVector`'s spine grow the same way.
- **Moves.** A move is a C copy of the value; the place it leaves keeps the bytes. `Aes256Gcm.create` builds its
  304-byte schedule in a local and returns it, so the callee's frame (and any temporary between) keeps a copy. Drop
  runs once, at the last place; G42's second point.
- **Drops that wipe.** `Aes256Gcm`'s `drop` (lib/std/ciphers.cee:117) zeroes its fields with a loop. §1.3 shows
  the C compiler removes that loop when the value is on the stack.

### 1.2 What CosmoStore does now

It reserves every vector of key bytes at its full size before writing one (the key file's keys, a set's copies,
decoded key text), so none grows. That covers the vectors it makes, not the library's, not a moved value's slot, and
not a vector whose final size it cannot know. It asks for a supported way for a key type to wipe every copy.

### 1.3 Measured: a wiping drop on the stack is removed

ceesafec 0.84.1, `ceesafec build` (its default `-O2`), Apple clang 21.0.0, arm64:

| Program | What the binary does at the drop |
|---|---|
| `struct Key { bytes: [u8; 32]; drop { for (i in 0..32) { this.bytes[i] = 0; } } }`, a local filled by `Random.fill`, summed, dropped | **nothing**: the 32 bytes are zeroed before `Random.fill` (the literal) and never after; the stores are dead to C |
| `Aes256Gcm.create(key)` in a function, `seal`, return | **nothing**: after `ceesafe_aes256gcm_seal` the code reads the tag and returns; the 304-byte schedule stays on the stack |
| `Vector<Key>` of 64, dropped | `bzero` of 2,048 bytes, then `ceesafe_free` or `free`: kept, because the free is opaque on one path. Not guaranteed: with LTO or a C library `free` the compiler knows, the same stores are dead |

So ADR 0135's "wiped when it is dropped" holds for a heap `Aes256Gcm` and not for the usual one on the stack. Any
wipe written as ordinary stores is the C compiler's to remove (C11 5.1.2.3: stores no later read observes). A wipe
must be a call the optimiser cannot see through: `explicit_bzero` (glibc 2.25, musl, the BSDs), `memset_s` (C11
Annex K; macOS), `SecureZeroMemory` (Windows), or `memset` followed by a compiler barrier that takes the pointer
(`__asm__ __volatile__("" : : "r"(p) : "memory")`), which is what glibc's `explicit_bzero` is.

## 2. Decision

Recommended: candidate (a), generalised from a vector to an allocator, with the wipe primitive it needs; candidate
(b), a type attribute that wipes on every move, is not adopted (§2.4). The rule for a key type becomes: **keep the
secret bytes in a `SecretHeap` block, and wipe what you hold inline with `Secret.wipe`.** A box's bytes do not move
when the box does; only its pointer is copied.

### 2.1 `ceesafe_wipe` (runtime)

`void ceesafe_wipe(void *p, size_t n)` in `rt.h`, out of line in `ceesafe_rt_core.c`. One implementation on every
target: `ceesafe_mem_set(p, 0, n)` then the barrier above under GCC and clang (hosted, Android, freestanding and the
CosmoOS kernel: no C library needed); `SecureZeroMemory` under MSVC's `cl`, which has no inline assembly on x64. It
does not depend on which C library is present, and survives LTO because the barrier, not the call boundary, is what
keeps the stores.

### 2.2 `Secret.wipe(mut place)` (lib/core, built in)

```
Secret.wipe(mut this.schedule);   // [u8; 304]: every byte zero, and the stores kept
Secret.wipe(mut this.h0);         // u64
```

- Sets the place to all zero bytes through `ceesafe_wipe(&place, sizeof place)`. `@effects()`: a `drop`, async
  code and the freestanding profile may call it.
- The place's type must be **zeroable**: an integer, float, `bool`, raw pointer, an array of a zeroable type,
  or a struct with no `drop` whose fields are all zeroable. All zero bytes are a valid value of each, so the place
  stays readable and nothing is `unsafe`. Anything else (an enum, a reference, a box, a vector, a type with a drop) is
  a new error, N0xxx, which names the first field that is not zeroable.
- A slice form, `Secret.wipe(mut bytes[..])` for a `SliceMut<u8>`, wipes the slice's elements.
- The interpreter sets the bytes to zero.

### 2.3 `SecretHeap`, the third built-in allocator type

`SecretHeap` is a field-less Copy struct beside `SystemHeap` and `FailingAllocator` (§18.1), passed to the runtime
as `CEE_ALLOC_SECRET` (4u; 2u and 3u are C's, ADR 0060, 0061). Every container that takes a built-in allocator takes
it: `Box<T, SecretHeap>`, `Vector<T, SecretHeap>`, `String<SecretHeap>`, `SlotMap`, `StableVector`, `Arena`, `Pool`.

- **A block is wiped before it goes back.** `ceesafe_free` of a `SecretHeap` block wipes it whole, then frees it.
  Each block carries a 16-byte header (`CEE_HEAP_ALIGN`) with its size, so the wipe knows its length without
  `malloc_usable_size` (absent under a host's allocator hook, ADR 0088, and on the kernel target).
- **Growth never uses `realloc`.** `realloc_block` for `SecretHeap` allocates the new block, copies, wipes the old
  block and frees it. A failed allocation leaves the old block live and unchanged, as now (one attempt).
- **Slots given up are wiped.** `pop`, `truncate`, `clear`, `remove` and `resize` down on a `SecretHeap` container
  wipe the element slots they vacate (after moving or destroying the element), so a live secret buffer holds no
  bytes of an element it no longer has.
- **Never stack-placed.** Placement (c-backend.md §2.2) treats `SecretHeap` boxes as it treats user allocators:
  they stay on the heap, so their bytes never take a stack slot.
- **Never on the fast path.** rt.h's inline `malloc`/`free` path (ADR 0101) is for `CEE_ALLOC_SYSTEM` alone; secret
  blocks always go through `ceesafe_rt_core.c`.
- **Counted like the others.** Live-block counts and the leak report, `CEESAFE_FAIL_ALLOC_AT`, and allocation
  budgets (ADR 0170: `budget_counts` gains it) include it. `FailingAllocator.failAfter` does not.
- **Not given to C.** A conversion that hands a buffer to C ownership (`CEE_ALLOC_FOREIGN`) is refused for a
  `SecretHeap` container (N0263's text gains the case): C would free it with `free`, unwiped, past the header.
- `SecretHeap` becomes a reserved name (type-system.md line 103), as the other two are.

What CosmoStore then writes, and no reservation workaround:

```
struct MasterKey { id: u32; key: Box<[u8; 32], SecretHeap>; }    // a Vector<MasterKey> moves pointers only
var text = Vector<u8, SecretHeap>.create();                       // decoded key text; growth wipes
```

### 2.4 Candidate (b), a `@secret` type attribute, and why not now

The attribute would make the C backend wipe the place a value of the type leaves at each move (an assignment, an
argument, a return, a payload moved out in a `match`), and wipe in its drop glue.

- **Cost.** A small key is two to four stores and the barrier per move. The barrier is the larger cost: it forces
  the value into memory and stops the C compiler keeping it in registers or merging copies (SROA), in every
  function a moved value passes through, generics included.
- **What it cannot reach.** The copies CeeSafe sees are not all the copies. The C compiler makes its own: an
  `sret` temporary for a returned struct, a spill slot, a value held in vector registers (AES rounds), a copy in a
  function it inlined. None has a CeeSafe place to wipe. So (b) cannot promise "every copy" either, while making
  every move of the type slower and changing drop elaboration and the IR.
- **What (a) gives instead.** Bytes in a `SecretHeap` box are not copied by moves at all. A key type that keeps its
  key there has one copy, in one block, wiped at its free.

(b) stays open for a later ADR if a program shows a need a `SecretHeap` box does not meet (§5 question 5).

### 2.5 lib/std

- `Aes256Gcm`'s drop calls `Secret.wipe` on `schedule`, `h0` and `h1` (fixes §1.3's second row for its last place).
- `HmacSha256`'s wiping `drop` (G34) is ADR 0194's, built on this ADR's `Secret.wipe`. That ADR also makes it a
  move type with `clone()`.
- Both stay inline values with infallible or optional `create`; the copies their by-value returns leave are §3's
  stated limit. A boxed form is question 3.

## 3. Consequences

**Guaranteed** (every target, every optimisation level, LTO included):
- every `SecretHeap` block is all zeros before it is returned to the platform allocator or the host's hook, by a
  free, by growth, or by a container's drop;
- every slot a `SecretHeap` container vacates is zeroed;
- every `Secret.wipe` is performed, at the place named.

**Not guaranteed**, documented in the guide's "Secrets" section:
- **registers**, general and vector, and the C compiler's own temporaries and spills;
- **places a program copies to itself**: a `[u8; 32]` local it reads a key into, a moved inline value's old slot
  (the reason to keep keys in a box);
- **swap and hibernation**: pages may be written to disk; **core dumps** include live secret blocks;
- **`fork`**: a child gets copies of the pages;
- **the kernel's and other processes' view** of the memory (ptrace, `/proc/pid/mem`).

**Optional extras, not in this ADR:** `mlock`/`VirtualLock` (keeps pages out of swap; bounded by `RLIMIT_MEMLOCK`,
page granular, so it needs a page allocator for secret blocks) and `madvise(MADV_DONTDUMP)` (Linux; FreeBSD
`MADV_NOCORE`; Windows `WerRegisterExcludedMemoryBlock`; macOS has none) would let `SecretHeap` keep blocks out of
swap and dumps. Until then the guide tells servers to turn core dumps off (`RLIMIT_CORE` 0, Linux
`prctl(PR_SET_DUMPABLE, 0)`) and use encrypted swap.

**Cost.** A `SecretHeap` growth copies where `realloc` might have extended in place, and every free writes the
block once. For keys (tens of bytes to a few KiB) this is not measurable; `SecretHeap` is not for bulk data.
`Secret.wipe` is a call and a barrier where it is written. Nothing changes for code that does not use them.

**Compatibility.** A package that declares an item named `SecretHeap` or a type `Secret` gets N0201 (a reserved
name); the corpus and the known applications have neither (to be checked by `tools/corpus_check.py`).

## 4. Implementation and checks

1. **Runtime.** `ceesafe_wipe`; `CEE_ALLOC_SECRET` in `ceesafe_alloc`, `ceesafe_alloc_zeroed`, `realloc_block`,
   `ceesafe_free` and the vector slot helpers; the header; `budget_counts`. `src/library/runtime.cee` regenerated
   (`tools/gen_runtime.py`).
2. **Checker and backend.** `SecretHeap` wherever `FailingAllocator` is accepted (`src/check/boxes.cee`,
   `vectors.cee`, `containers.cee`, `declare.cee`, `types.cee`); its id in `c_helpers.cee`; placement off for it;
   `Secret.wipe` with the zeroable rule and its N-code; the C-ownership refusal.
3. **Freed-block scan, for tests.** `ceesafec test --scan-freed` (and `CEESAFE_SCAN_FREED=1` for a built program)
   makes the runtime keep every freed block of every CeeSafe allocator in a quarantine, never reused, and makes
   growth allocate, copy and free for every allocator (so C's `realloc` cannot free a block out of sight). lib/std's
   `Testing.freedHolds(pattern: Slice<u8>) -> bool` searches the quarantine. Tests:
   - G42's repro with `Vector<Key>` (SystemHeap): `freedHolds(key)` is **true**, which documents the gap;
   - the same with `Vector<Key, SecretHeap>`, `String<SecretHeap>`, `Box<[u8; 32], SecretHeap>`, `SlotMap` and
     `StableVector` growth, `pop`/`truncate`/`clear`/`remove`, and each container's drop: **false**;
   - `CEESAFE_FAIL_ALLOC_AT` swept over a secret vector's growth: no leak, the old block live and intact on failure;
   - `Aes256Gcm` and `HmacSha256` in a `Box<_, SecretHeap>` and dropped: false.
4. **The wipe is kept.** `tools/wipe_check.py` builds §1.3's two programs (stack `Key`, stack `Aes256Gcm`) with
   `Secret.wipe` at `-O0`, `-O2`, `-O3` and `-O2 -flto`, under clang and gcc (and `cl` where CI has it), and checks
   in the disassembly that `ceesafe_wipe` is called after the last use; without `Secret.wipe` it reports the stores
   gone, so the check fails if a compiler change makes it pass vacuously.
5. **Targets.** The scan and growth tests on Linux (glibc, musl), macOS and Windows CI; `freestanding_check.py`
   builds a freestanding and a `--kernel cosmoos` program that wipes, and checks `ceesafe_wipe` links without a C
   library.
6. **Interpreter.** `SecretHeap` as `SystemHeap` plus the wipes, and `freedHolds` over its own freed blocks, so
   `ceesafec test --interp` agrees.
7. **Spec and guide.** type-system.md §18.1, §23, §30.2 and a new section for `Secret.wipe`; c-backend.md §2.2, §6;
   diagnostics.md; the guide's "Secrets" page with §3's lists. CosmoStore then drops its reservation workaround and
   closes G42 and G34 against the release.

## 5. Questions for the owner

1. **Mechanism:** a third built-in allocator, `SecretHeap`, with `Secret.wipe` (proposed); a secret-vector
   constructor alone (`Vector<T>.createSecret()`), which leaves boxes and strings out and needs a flag in every
   vector; or the `@secret` attribute of §2.4? Recommended: `SecretHeap` and `Secret.wipe`.
2. **Names:** `SecretHeap` and `Secret.wipe`, or `WipingHeap` and `Memory.wipe`? Recommended: `SecretHeap` and
   `Secret.wipe`; the names say why, which is what a reader of a key type needs.
3. **lib/std's key holders:** wipe inline with `Secret.wipe` and document the by-value returns (proposed); or move
   `Aes256Gcm`'s schedule and `HmacSha256`'s state into `SecretHeap` boxes (one allocation per `create`, and
   `HmacSha256.create` would need to report out of memory)? Recommended: inline now; a caller that needs one copy
   puts the value in a `Box<_, SecretHeap>` itself, which this ADR makes possible.
4. **`mlock` and `MADV_DONTDUMP`:** in this ADR, or a later one with a page allocator for secret blocks?
   Recommended: later; the guide tells servers to disable core dumps meanwhile.
5. **Candidate (b):** declined for now and reopened only on a program's measured need (proposed), or scheduled as
   phase 2? Recommended: declined for now; §2.4's copies it cannot reach make it a cost without a guarantee.
6. **Release:** the runtime wipe and lib/std's drops (§2.1, §2.5) fix ADR 0135's stated behaviour, and need no
   compiler change if lib/std calls `ceesafe_wipe` as it calls `ceesafe_aes256gcm_*`. Ship them in the next patch
   release ahead of `SecretHeap` and `Secret.wipe`, or all together? Recommended: the fix first, as a patch;
   `SecretHeap` in the next minor release once §4's checks pass on Linux, macOS and Windows.
