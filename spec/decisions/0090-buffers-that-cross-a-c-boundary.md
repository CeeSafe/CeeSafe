# ADR 0090 — Buffers that cross a C boundary: a vector's raw parts, its spare capacity, and blocks a host adopts

- Status: **Proposed**, 2026-11-11; awaiting the owner. Not implemented. Revised the same day after two adversarial
  reviews (soundness; fit with `bun_zstd`), whose findings are in §7.
  Raised by the Bun port's second crate swap, `bun_zstd` (`~/dev/CeeSafeApps/bun/ROADMAP.md` §2,
  `analysis/trials/csrf/REPORT.md` §7): its `decompress_alloc` returns a `Vec<u8>` that Rust owns, and its streaming
  decoder appends to a `Vec<u8>` the caller owns. ADR 0088 gave CeeSafe and its host one heap (§2.5, stage B, 0.29.0)
  and left this case out by name (its §2.5 rule 3 and §4: "`Vector.intoRaw` and moving a block to ADR 0060's
  `foreign held` line, when a swapped crate returns an owned buffer to Rust"). Each claim about ceesafec was checked
  on 2026-11-11 with ceesafec 0.29.0 by the probes in `~/dev/CeeSafeApps/bun/analysis/adr-evidence/0090/` (below,
  `evidence/`; `bash evidence/run.sh` runs them, `evidence/results.txt` is its output). Claims about Bun name the file
  at Bun's `main`, commit `89452d2ac137` (CeeBun's pin); counts are `evidence/buffers.py`'s over its hand-written Rust.
- Release: 0.31.0, in one stage (0.30.0 shipped with ADR 0089 alone).
- Scope: type-system.md §23.1 (`Vector`), §27.3 (unsafe operations), §34.2 (beside `Box.intoRaw`); memory-model.md
  §3.2 (the report); unsafe.md; `lib/core` (one struct); the runtime's accounting (`runtime/core/ceesafe_rt_core.c`,
  `ceesafe/rt.h`), the interpreter's, and `ceesafe/embed.h` (one more obligation on a host's `realloc`, §2.3);
  guide/unsafe-and-c.md. **Extends** ADR 0088 §2.5: its rule 2 ("each side frees what it allocated") stays the
  default, and this ADR adds the stated exception its rule 3 left open. No new diagnostic.

## 1. Context

### 1.1 What `bun_zstd` needs

`src/zstd/lib.rs` (860 lines) wraps libzstd. Its API moves buffers in three ways:

| Function | The buffer |
|---|---|
| `decompress_alloc(src) -> Result<Vec<u8>, ZstdError>` (7 call sites outside the crate), `decompress_alloc_with_prefix` (2), and through them `inflate_embedded -> Box<[u8]>` (19 uses of `embed_compressed!`) | The crate allocates the output (up to 16 MiB at once, or grown in 4,096-byte steps) and the caller owns it. |
| `StreamingDecoder::decompress(input, out: &mut Vec<u8>, …)` | The caller owns the vector; the crate grows it (`try_reserve`) and writes into its spare capacity, once per chunk of a response body. |
| `compress(dest: &mut [u8], …)`, `compress_append(out: &mut Vec<u8>, …)` | Lent for the call. `compress_append` never grows `out`: its three callers reserve first, and it writes into the spare capacity. |

- Every write goes into a vector's uninitialised tail: `out.spare_capacity_mut()`, a C call (`ZSTD_decompress`,
  `ZSTD_decompressStream`), then `bun_core::vec::commit_spare(out, written)`, which is `set_len`.
- The decoder itself lives across calls and Rust owns it (`Decompressor::Zstd(Box<StreamingDecoder>)`,
  `src/http/Decompressor.rs`). That needs nothing new: it is a box handed out with `Box.intoRaw` and given back
  through an export that Rust's `Drop` calls (ADR 0088 §2.5 rule 2).

### 1.2 What a CeeSafe `Vector` cannot do today

- **Its buffer cannot be handed out.** `Vector.intoRaw(v)` is N0200, "'Vector' has no function 'intoRaw'"
  (`evidence/p1_into_raw.cee`). `Box.intoRaw` (type-system.md §34.2) exists for a box only. A vector's address can be
  read (`v.viewMut().ptr`), but the vector still owns the buffer and frees it when it is dropped.
- **Its length cannot be committed.** There is no `setLength`: N0200, "'Vector<u8>' has no method 'setLength'"
  (`evidence/p2_spare.cee`). The way to let C write into a vector today is `v.resize(n, 0)`, the C call through
  `v.viewMut().ptr`, then `v.truncate(written)` (`evidence/p3_resize_then_truncate.cee`, which works). It writes every
  byte twice, and in a streaming loop it fills the whole spare capacity again on every iteration. One fill is cheap,
  about 0.3 ms for 16 MiB (`evidence/b_resize.cee` against `b_reserve.cee`: 0.07 s and 0.00 s of user time for 200
  buffers), so cost alone is not the reason for this ADR. The reason is that a port of `spare_capacity_mut` +
  `set_len` has nothing to translate to (§1.4).
- **A block cannot change sides.** `ceesafe_free` subtracts from `live` and then frees; a host that frees a CeeSafe
  block with its own `free` leaves `live` one too high, and the report calls it a leak
  (`runtime/core/ceesafe_rt_core.c`, `ceesafe_free`). The other way, a block the host allocated and CeeSafe frees
  subtracts what was never added. ADR 0088 §2.5 rule 2 forbids both for that reason.

### 1.3 The heap is already one

With ADR 0088 stage B the trial's `ceebun_init` gives `ceesafe_init` an allocator over mimalloc
(`mi_malloc_aligned`/`mi_zalloc_aligned`/`mi_realloc_aligned` at 16 bytes, `mi_free`), which is what Rust's
`#[global_allocator]` uses in Bun; under ASAN neither side is given one, and both use the C library's. The runtime
keeps no header in a block and no table of blocks: `ceesafe_alloc` returns the platform's block as it is, and counts
only when counting is on. So a CeeSafe vector's buffer already is a block the host's `free` and `realloc` accept. What
is missing is the language's way to give the buffer up, and the accounting's way to hear about it.

### 1.4 How often Bun does this

In Bun's hand-written Rust (`evidence/buffers.py`, 4,297 files; it does not strip comments, so a count may include a
mention in one):

| Idiom | Sites | Largest users |
|---|---|---|
| `spare_capacity_mut` | 42 | runtime 6, zstd 6, aot 6, libdeflate_sys 5 |
| `set_len` | 80 | bun_core 13, collections 12, runtime 11, bundler 10 |
| `commit_spare` (Bun's `set_len` helper) | 36 | runtime 10, bun_core 5, zstd 5 |
| `Vec::from_raw_parts` | 18 | collections 4, jsc 4, bundler 4 |
| `into_raw_parts`, `ManuallyDrop<Vec>` | 8 | runtime 4, bun_core 2 |
| `Box::into_raw` / `Box::from_raw` (and Bun's helpers) | 330 / 484 | runtime 158 / 285 |

The box pair is covered today (§34.2). The vector idioms are not, and the first three rows are in the hot paths of
every compression and I/O crate: `zstd`, `brotli`, `libdeflate_sys`, `zlib`, `io`.

## 2. Proposal

Three additions. The first two are the language's; the third joins them to the runtime's accounting.

### 2.1 `Vector.intoRaw` and `Vector.fromRaw`

```ceesafe
/// What a vector is made of: `pointer` is null when `capacity` is 0.
public struct VectorParts<T> {
    public pointer: *mut T;
    public length: usize;
    public capacity: usize;
}
```

- `Vector.intoRaw(v)` consumes `v: Vector<T, A>` and returns `VectorParts<T>` (in `lib/core`). Nothing is dropped or
  freed: the caller owns the buffer and its first `length` elements, and leaks them unless it gives them back. It is
  safe, as `Box.intoRaw` is. A vector that never allocated (capacity 0) gives a null `pointer` (`0 as *mut T`), never
  the runtime's shared empty address.
- `Vector.fromRaw(pointer, length, capacity)` (unsafe, §27.3) makes a vector again. The vector type, and with it the
  allocator, comes from the expected type (`let v: Vector<u8> = unsafe { Vector.fromRaw(p, n, c) }`); without one it
  is N0205, as for `Box.fromRaw`. Contract: the three values came from `Vector.intoRaw` of a vector of exactly that
  type, with `length` changed only to a value at most `capacity` below which every element is initialised, and are
  given back once. `capacity` 0 gives the empty vector, whatever `pointer` is.
- Neither has an effect. Both expose an address (memory-model.md §5.1 rule 1c). In the interpreter both work as in C.
- The block stays CeeSafe's in the accounting: it is in `live` until a vector made by `fromRaw` is dropped. This pair
  is for a round trip (a C library holds the buffer and returns it, a `@callback` receives it back), which is ADR
  0088's rule 2. `bun_zstd` itself does not use it; the 26 raw-parts sites of §1.4 do, and §2.3's pair is defined on
  it.

### 2.2 `v.setLength(n)`

- `v.setLength(n)` (unsafe, `borrow mut this`, two-phase like `resize`, `n: usize`) sets the length without touching
  the elements. Contract: `n <= v.capacity`; every element below `n` is initialised; and the vector has not grown
  since those elements were written. Elements at `n` and above are forgotten, not dropped.
- It panics ("index out of bounds") when `n > v.capacity`, in every build: the check is one comparison, and the
  alternative is a length the bounds checks then trust.
- **Spare capacity does not survive a growth.** Whatever is written above the length is lost when `push`, `reserve`
  or another growth moves the buffer: C's `realloc` happens to keep it, the interpreter's growth copies only the
  elements. The order is `reserve`, write, `setLength`, with no growth between the last two.
- With `v.reserve(extra)` and `v.viewMut().ptr`, which both exist, this is Rust's `spare_capacity_mut` and `set_len`.
  The pointer of an empty vector's view is its buffer's address once it has capacity
  (`evidence/p4_spare_pointer.cee`); §23.1 now says so.

```ceesafe
/// Safety: ZSTD_decompress writes at most `room` bytes from the pointer, inside the vector's capacity, and returns
/// how many it wrote; `setLength` then covers exactly those.
fn decompressAppend(out: borrow mut Vector<u8>, src: Slice<u8>) -> Result<void, ZstdError> {
    let at = out.length;
    let room = out.capacity - at;
    let to = unsafe { out.viewMut().ptr.add(at) };
    let written = unsafe { ZSTD_decompress(to as *mut void, room, src.ptr as *void, src.length) };
    if (ZSTD_isError(written) != 0) {
        return Err(ZstdError.DecompressionFailed);
    }
    unsafe { out.setLength(at + written); }
    return Ok();
}
```

`evidence/p6_append_today.cee` is this function on 0.29.0, as far as it goes: the bytes are written, and the length
stays 0.

### 2.3 Blocks that change sides: `Vector.intoForeign` and `Vector.fromForeign`

- `Vector.intoForeign(v)` (unsafe) is `Vector.intoRaw(v)` for a block that will not come back: the host adopts it and
  frees it with its own allocator. `Vector.fromForeign(pointer, length, capacity)` (unsafe) is `Vector.fromRaw` for a
  block the host allocated and gives up: CeeSafe grows it and frees it.
  - `intoForeign` is unsafe although it frees nothing: it takes the block out of the report, so in safe code it would
    hide a leak from `ceesafec test` and the failure sweep. Its `Safety:` line names the host that frees the block.
  - `T` must be Copy (N0259, as for `extend`): the pair is for buffers of plain values. A vector of values with drop
    glue comes back through an export (ADR 0088 rule 2).
  - `A` is `SystemHeap` or `FailingAllocator`, the only two a `Vector` can have (type-system.md §30.2); both take
    their blocks from the platform heap, so both can change sides.
  - The two pairs do not mix: parts from `intoForeign` are not valid for `fromRaw` (the drop would subtract from
    `live` twice), and parts from `intoRaw` are not the host's to free.
- **The accounting.** When counting is on, `intoForeign` takes the block out of `live` and `fromForeign` adds it, so
  the report's `live` stays "blocks CeeSafe must still free". Neither is an allocation attempt: they are not numbered,
  and `CEESAFE_FAIL_ALLOC_AT` never fails them. The report gains one line when either was used,
  `ceesafe alloc: given=<k> taken=<m>`, for information; no tool reads it as a leak. A vector with capacity 0 has no
  block and counts nothing. With counting off, nothing is counted, as for every allocation.
- **The interpreter** (`run --interp`, `ceesafec test`) has no host. Both operations work on its own blocks and count
  as above, so a test of code written for a host balances: a block given and never taken back is not in `live`.
- **The contract with the host**, stated in `ceesafe/embed.h` and type-system.md §28.6:
  1. Both sides use one allocator: the host's, given to `ceesafe_init`, or, when none was given, the C library's.
  2. A block CeeSafe gives (`intoForeign`) holds `capacity × size(T)` bytes, at least 1. It is aligned to
     max(16, `alignof(max_align_t)`) when CeeSafe allocated or grew it; a block that was taken and is given back
     without having grown is the block as taken. The host frees or reallocates it with the function its hook's
     `free` or `realloc` calls (the C library's when there is no hook). The host's functions must treat it as a block
     of their own of that size: an allocator that keeps the size or alignment it was asked for outside the block,
     and checks them at `free`, cannot adopt.
  3. A block CeeSafe takes (`fromForeign`) came from that same allocator, holds at least `capacity × size(T)` bytes,
     and is aligned for `T`. It need not be 16-aligned: a Rust `Vec<u8>` of 8 bytes is mimalloc's 8-aligned block.
  4. **A new obligation on the hook's `realloc`** (ADR 0088 §2.5 amended): given any block of its allocator, also
     one aligned below 16, it returns a block aligned to max(16, `alignof(max_align_t)`). The runtime grows a taken
     vector through it (`ceesafe_vec_grow`, `ceesafe_realloc`) and checks the result's alignment, so a `realloc`
     that grew an 8-aligned block in place would be reported as a broken hook (`CEE_PANIC_ALLOCATOR`).
     `mi_realloc_aligned(p, n, 16)`, the trial's, moves such a block. A host that never calls `fromForeign` is not
     affected. Without a hook, the C library's `realloc` already returns 16-aligned blocks on every target CeeSafe
     supports.
- **A Rust host** (guide/unsafe-and-c.md, a new section). `Vec::from_raw_parts(pointer, length, capacity)` over a
  given block, and `mem::take` of the caller's `Vec` with its raw parts for a taken one.
  - **This is outside Rust's documented contract.** `Vec::from_raw_parts` asks for a pointer the global allocator
    returned for exactly `T`'s alignment and `capacity` bytes, and `GlobalAlloc::dealloc` and `realloc` ask for the
    layout the block was allocated with. An adopted block was allocated at 16 for a `Vec<u8>` whose layout says 1.
    It is defined by the allocator, not by Rust, and Miri would reject it. Bun already does the same for buffers
    from C++ (`src/jsc/bindgen/extern_types.rs`, `into_vec`: "a mimalloc buffer of `capacity` elements …
    ownership was transferred").
  - **The conditions the host's `#[global_allocator]` must meet:** it is the allocator of rule 1; its `dealloc`
    frees by address alone; and its `realloc` is correct for any layout whose size is at most the block's real size
    and whose alignment is at most 16. Bun's `bun_alloc::Mimalloc` meets them (`dealloc` is `mi_free`; `realloc`
    reads the layout only to choose `mi_realloc` or `mi_realloc_aligned`, and to copy at most `layout.size()`
    bytes when it shrinks), and so does `std::alloc::System` on Unix, Bun's allocator under ASAN (`free`, and a
    `realloc` that copies `layout.size()` bytes). `System` on Windows is the process heap, not the C runtime's
    `malloc`: there, adoption needs a hook.
  - **Capacity 0.** A `Vec` with capacity 0 has a dangling pointer, not a block: the shim passes capacity 0 and
    CeeSafe ignores the pointer. For a returned capacity 0 the pointer is null: the shim makes `Vec::new()`, and
    never a slice from it.
- **The shape of an export that takes the caller's vector.** Take, work, give, with no early return between: the
  export calls `fromForeign`, passes the vector to the ported function by `borrow mut`, calls `intoForeign` whatever
  that function returned, and writes the three parts back. An error such as zstd's `ShortRead` is a normal result
  and keeps the bytes already appended. On the Rust side the shim does `mem::take(out)`, passes the parts, and
  rebuilds `*out` from what comes back. A CeeSafe panic aborts the process, and Bun is built with
  `panic = "abort"`, so nothing unwinds past a taken vector. The cost per call is three words each way and, with
  counting on, two atomic counts.
- A shim that adopts a returned buffer reads (Rust), for `decompress_alloc`:

```rust
let (mut pointer, mut length, mut capacity) = (null_mut::<u8>(), 0usize, 0usize);
let code = unsafe {
    bun_zstd__decompress_alloc(src.as_ptr(), src.len(), &mut pointer, &mut length, &mut capacity)
};
// SAFETY: on success CeeSafe gave the block up with Vector.intoForeign (ADR 0090 §2.3).
let out = if capacity == 0 { Vec::new() } else { unsafe { Vec::from_raw_parts(pointer, length, capacity) } };
```

### 2.4 What does not change

- ADR 0088 §2.5 rule 1 (lending is the default) and rule 2 (each side frees what it allocated through its own code)
  stand. A `Box<T>`, such as `bun_zstd`'s decoder, still comes back through an export.
- Nothing is added for `Box`, `String`, `StableVector` or `SlotMap` (§4).
- `ceesafe_exports.h` and FFI safety. `VectorParts<T>` is an ordinary struct, not `@repr(C)`: a generic struct cannot
  be (N0290, `evidence/p5_parts_export`), so it cannot be in an export's signature (N0291). An export passes the
  three values as three out-parameters, or in a `@repr(C)` struct the package declares for its element type.
- Freestanding builds: `Vector` is hosted-only (profiles.md §4.1, N0293), and so is everything here.

## 3. Alternatives considered

- **A Rust wrapper type whose `Drop` calls an export** (ADR 0088 rule 2, no language change). `decompress_alloc`
  would return `CeeVec` instead of `Vec<u8>`: its nine call sites and `inflate_embedded`'s `Box<[u8]>` change type or
  copy. It also cannot serve `&mut Vec<u8>`: the caller's vector is Rust's. Rejected as the only tool; it stays right
  for values with drop glue.
- **Copy at the boundary.** CeeSafe decompresses into its own vector and Rust copies it. Simple, and wrong for a
  port meant to be benchmarked against Rust: a second pass over up to 16 MiB per call.
- **Keep the loop in Rust.** The shim does `try_reserve` and passes the spare capacity as a lent slice; CeeSafe makes
  one C call per export. No ownership crosses, but the decoder's loop, state and error mapping stay in Rust: the
  crate is not ported.
- **A host callback that grows the caller's `Vec`.** CeeSafe keeps the loop and the state, and asks Rust for room
  through an `extern "C"` function. No ownership crosses and any host allocator works, also one that could not adopt.
  But the ported code could not use `Vector<u8>`: it would write to a host-buffer type made for the shim, and be
  rewritten when the crates above it are ported and the buffer becomes a CeeSafe vector. Each growth would also
  re-enter Rust from CeeSafe. Rejected for a port; it remains the way for a host whose allocator fails §2.3's
  conditions.
- **Reuse `foreign held` for given blocks**, as ADR 0088 §4 sketched. That line counts blocks C still holds at exit.
  A given block the host later frees would stay in it for ever, and a taken block was never in it, so the number
  would mean nothing. A separate `given`/`taken` line keeps both meanings.
- **Make the runtime copy a taken block that is not 16-aligned**, instead of §2.3's rule 4. `ceesafe_vec_grow` would
  test the address and do an allocation, a copy and a free. It puts a branch in every vector growth for the sake of
  hosts that adopt, and hides a hook that returns misaligned blocks behind it. Rejected: the host that opts into
  adoption carries the obligation.
- **A safe `v.spare()` returning `SliceMut<T>`.** A slice of uninitialised elements can be read in safe code. Rust
  returns `&mut [MaybeUninit<T>]` for that reason; CeeSafe has no such type, and one for this alone is not worth it.
  The pointer and an unsafe `setLength` say the same thing honestly.
- **Only `setLength`, no `intoRaw`.** It would serve the lent and the append cases, but not `decompress_alloc`.

## 4. Not in this ADR

- `Box.intoForeign`/`fromForeign`, and a `String`'s parts: no swapped crate needs them yet. A box with drop glue
  must not change sides at all (rule 2).
- Boxed slices (`Box<[T]>`, 655 `into_boxed_slice` sites in Bun): the Rust shim calls `into_boxed_slice` on the
  adopted `Vec`, in Rust's allocator.
- A vector over a host block that CeeSafe must not grow or free (a borrowed `Vec`): that is a `SliceMut`.
- `--emit rust` (ADR 0088 stage C) will generate the shim's declarations; until then they are written by hand.
- Binaries of the host that link a swapped crate without starting CeeSafe. In Bun, `bun_sema` and `bun_lint` depend
  on `bun_zstd`, and `src/sema/standalone` has its own `main` and its own `mi_*` functions. That is CeeBun's to
  solve in the trial (link the archive and call `ceebun_init` there, or keep that binary on the Rust crate).

## 5. Testing

- **Corpus, run-pass:** a round trip through `intoRaw`/`fromRaw` (lengths 0, 1, a grown vector; an element type with
  drop glue, dropped once, after `fromRaw`); `setLength` after writes through the pointer; `setLength` above the
  capacity panics; the empty vector's null `pointer`; `intoForeign` then `fromForeign` of the same parts, with the
  report's `live` and `given`/`taken`; a `Vector<u8, FailingAllocator>` through both. The same under the
  interpreter, ASan and the fault sweep (no new allocation attempts: the sweep's count does not change).
- **Corpus, check-fail:** `fromRaw`, `intoForeign`, `fromForeign` and `setLength` outside `unsafe` (N0280);
  `fromRaw` without an expected type (N0205); `intoForeign` of a `Vector<String>` (N0259); a view live across
  `setLength` (N0240).
- **`embed_check`** (ADR 0088's host fixture, whose tagged allocator aborts on a block it did not allocate): an export
  returns a buffer with `intoForeign` and the host frees it with the hook's `free`; the host passes a block it
  allocated, the export grows it past its capacity and returns it; the same with an 8-aligned block of 8 bytes,
  with a hook whose `realloc` meets rule 4, and with one that grows in place, which must panic with kind 17; with
  the report on, `live=0` and `given=1 taken=1`; all of it without a hook, where both sides use `malloc`. A
  deliberate wrong case, the host freeing an `intoRaw` block, must show `live=1`.
- **CeeBun:** `bun_zstd` swapped in the trial clone; Bun's zstd, fetch and archive tests in a release build and in a
  debug + ASAN build, where a block freed by the wrong allocator is an ASAN report.

## 6. Questions for the owner

1. **The names** `intoForeign`/`fromForeign` beside `intoRaw`/`fromRaw`, and `VectorParts<T>`. **Proposed: as
   written.**
2. **The report:** a new line `given=<k> taken=<m>`, not ADR 0060's `foreign held`. **Proposed: the new line.**
3. **`setLength` checks `n <= capacity` in every build** and panics. **Proposed: yes.**
4. **The hook's `realloc` must return a 16-aligned block for any block of its allocator** (§2.3 rule 4), an
   amendment to ADR 0088 §2.5 that only a host calling `fromForeign` can notice. **Proposed: yes.**
5. **`intoForeign` is unsafe and both need a Copy `T`.** **Proposed: yes.**
6. **Vectors only**, with `Box` and `String` left until a crate needs them. **Proposed: yes.**
7. **Release 0.31.0, one stage** (first proposed for 0.30.0, which shipped with ADR 0089 alone). **Proposed: yes.**

## 7. Review findings (2026-11-11)

Two adversarial reviewers read the first draft. Neither found a blocker. What changed:

- **Soundness.**
  - A taken block aligned below 16, grown through a hook that reallocates in place, would have tripped the runtime's
    alignment check and blamed the hook: rule 4 and its `embed_check` cases are new.
  - `intoForeign` was safe, so safe code could hide a leak from the report, and its parts given to `fromRaw` wrapped
    `live`: it is unsafe now, and the pairs are stated not to mix.
  - The Rust claim said both of Bun's allocators ignore the layout. `realloc` reads it in both. The section now says
    the adoption is outside Rust's contract and states the conditions under which the allocator defines it.
  - N0365 had no case: a `Vector` takes only `SystemHeap` and `FailingAllocator`, and both are on the platform heap.
    It is gone, and a Copy bound replaces "plain values in practice".
  - The freestanding claim was wrong (`Vector` is hosted-only), and the interpreter was not specified: spare
    capacity does not survive a growth there, and the Foreign pair counts as it does natively.
- **Fit with `bun_zstd`.**
  - `compress_append` never grows its vector, so it is lent; only `StreamingDecoder::decompress` takes the caller's
    vector. The call-site counts were wrong (7 and 2, not 5; 19 uses of the macro, not 20).
  - The decoder object, the error path of an export that takes a vector, the binaries that link the crate without
    starting CeeSafe, and the host-callback alternative were not mentioned.
  - Checked and correct: mimalloc's `mi_free` and `mi_realloc` accept blocks from `mi_malloc_aligned`, and
    `mi_realloc_aligned(p, n, 16)` moves an 8-aligned block; `Mimalloc::dealloc` ignores the layout; nothing in Bun
    calls `mi_expand` or relies on `mi_usable_size` for a global `Vec`; `reserve` and `withCapacity` return a
    `Result`, so the `try_reserve` paths port.
