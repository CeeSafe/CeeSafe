# ADR 0194 — HMAC wipes its key; Crc32 and Sha1; an OrderedMap full after in-order puts

- Status: **Accepted** by the owner, 2026-11-21 ("Accept all as recommended": every question as §5 or §6 recommends; release order: a patch first, then the next minor). Proposed the same day. Implemented in part for 0.84.2: §2.1, G34 (`HmacSha256`'s wiping `drop`, `clone()`, its temporaries wiped) on ADR 0197's wipe; `Crc32`, `Sha1` and the edge splits are for 0.85.0. Raised by CosmoStore's CEESAFE_GAPS G34, G26 and G29
  (`~/dev/CeeSafeApps/cosmostore/CEESAFE_GAPS.md`): three small library gaps, one ADR because each is a few lines of
  lib/std and they can ship together. Measured on `origin/main` at v0.84.1 (§1.4); nothing here is built yet. G34's
  wipe is ADR 0197's `ceesafe_wipe` and `Secret.wipe` (proposed the same day, for G42 and G34): this ADR uses them
  and adds what 0197 §2.5 leaves out of HMAC.
- Scope: `lib/std/checksums.cee` (`HmacSha256`'s `drop` and `clone`, `Sha256.finish`, new `Crc32` and `Sha1`),
  `lib/std/checksum_tables.cee` and `tools/gen_checksums.py` (CRC-32's slicing tables), `lib/std/ordered_map.cee`
  (`split`, `put`), `runtime/hosted/ceesafe_rt_io.c` and `runtime/include/ceesafe/rt.h` (two functions), the
  interpreter's C library
  model (`src/interpreter/io.cee`), `tools/oracles/gen_checksum_cases.py`, `spec/api/std.md`, the guide and the
  book's Appendix C. No language, checker or emitter change.

## 1. Context

### 1.1 G34: HMAC keeps its key, and a `drop` that zeroes is removed by the C compiler

- `HmacSha256` (`lib/std/checksums.cee:364`) holds `inner: Sha256`, which has absorbed the key XOR 0x36, and
  `outerKey: [u8; 64]`, the key XOR 0x5C. Each is as good as the key: the first block's state alone lets anyone make
  codes. It has no `drop`, so both stay in freed memory or a dead stack slot. So do `create`'s locals (`padded`,
  `innerKey`, a long key's digest) and `finish`'s `outer`, a `Sha256` fed `outerKey`. CosmoStore keys HMACs with
  SSE-C customer keys, data keys and SigV4 signing keys.
- **The model G34 points to does not work either** (found here, and by ADR 0197 §1.3 the same day). `Aes256Gcm`'s `drop`
  (`ciphers.cee:117`) zeroes its 304-byte schedule with plain stores. The emitter gives them to C as they are
  (`(*v_this_0).f_schedule.e[t_5] = 0`). For a cipher that is a local, clang -O2 inlines the drop and removes the
  stores as dead: in `ceesafec build` of a function that creates one, seals 16 bytes and returns, the disassembly
  (M1, Apple clang 21) has no store to the schedule after `ceesafe_aes256gcm_seal` returns. LLVM also removes stores
  before a `free`, so the same is expected for a cipher on the heap (not checked). CosmoStore's own wipes (`sse.cee`'s
  `wipe`, a loop of stores) are open to the same removal. No CeeSafe code can write a wipe the C compiler must keep.
- `HmacSha256` is Copy today (no `drop`; every field Copy, type-system.md §14.1): `let fork = mac;` compiles on
  0.84.1 and gives two codes that share a prefix. A `drop` makes it a move type, so that line becomes a move.
- Out of scope: G42 (a move or a vector's growth leaves the old bytes). A wipe clears only the value's last place.

### 1.2 G26: no CRC-32 (IEEE) and no SHA-1

S3's flexible checksums are CRC-32, CRC-32C, CRC-64/NVME, SHA-1 and SHA-256. The AWS CLI and boto3 use CRC-32 when
asked for a checksum without naming one. lib/std has all of them but CRC-32 (reflected 0xEDB88320; also gzip's, zip's
and PNG's, so the inflater and gzip writer of G35 need it) and SHA-1. CosmoStore carries both in plain CeeSafe
(`src/storage/checksums/crc32.cee`, a byte at a time; `sha1.cee`, FIPS 180-4), and CeeSafe.Web's `web_ws` has its own
`Sha1` for the WebSocket handshake. A package's type shadows lib/std's (checked on 0.84.1 with a package
`struct Crc32c`), so adding the names breaks neither.

### 1.3 G29: an OrderedMap built in key order is half empty

`OrderedMap` is a top-down B-tree, B = 16 (`most()` 31, `fewest()` 15, `ordered_map.cee:175`). Each node's vectors are
reserved once at `most() + 1` entries and `most() + 2` children (`grow`, `:246`), so no put allocates after its first
step. `split` (`:278`) always leaves B − 1 entries on each side. Keys put in order go only into the rightmost leaf, so
each node it leaves behind keeps 15 or 16 entries of 32 places. Loading from sorted data (a checkpoint, a snapshot, an
import) is the common way to build a large map, and it gives the worst fill. CosmoStore's version index is filled in
key order, and recovery puts a checkpoint's entries in key order. At 1M objects it takes about 1.6 times a HashMap.

### 1.4 Measured (M1, macOS, Apple clang 21, `ceesafec build` 0.84.1; load average about 5, other sessions)

| 1M puts of `u64` → `[u8; 64]` | peak footprint | 1M in-order puts | 1M lookups after them |
|---|---:|---:|---:|
| `OrderedMap`, in order / scrambled (`i * 2654435761 % 4294967311`) | 184 / 128 MB | 118 ms | 51 ms |
| the same with §2.4's edge splits, in order / scrambled | **97** / 128 MB | **75 ms** | 51 ms |
| `HashMap`, for comparison | 74 MB | | |

| 256 MiB, one call per MiB | MB/s |
|---|---:|
| CosmoStore's `Crc32`, a byte at a time | 377 |
| lib/std `Crc64Nvme`, slicing-by-8 (what a table `Crc32` would run at) | 1,430 |
| lib/std `Crc32c`, on the CRC instruction (ADR 0089) | 8,085 |
| CosmoStore's `Sha1`; lib/std `Sha256`; lib/std `Md5` | 215; 207; 437 |

## 2. Decision

### 2.1 The wipe: ADR 0197's

- Every wipe below is `Secret.wipe(mut place)`, ADR 0197 §2.2. It zeroes the place through `ceesafe_wipe` (§2.1
  there): `ceesafe_mem_set`, then `__asm__ __volatile__("" : : "r"(p) : "memory")` under GCC and clang, or
  `SecureZeroMemory` under MSVC. The barrier keeps the stores even when the call is inlined. `Aes256Gcm`'s drop moves
  onto it too (0197 §2.5).
- **If this ADR is built first**, it brings `ceesafe_wipe` as 0197 §2.1 defines it. lib/std calls it through
  `extern "C"`, as it calls `ceesafe_aes256gcm_*` (0197's question 6), until `Secret.wipe` exists; the calls are then
  replaced. No second public wipe is added. **(Question 1.)**

### 2.2 G34: `HmacSha256` wipes what it holds

- `drop { … }` on `HmacSha256` wipes `outerKey` and the inner `Sha256` whole (`state`, `block`, `used`, `length`:
  all zeroable, 0197 §2.2), as 0197 §2.5 says.
- `create` wipes `padded`, `innerKey` and a long key's digest before it returns. `finish` wipes its `outer`.
  `Sha256.finish` wipes its working copy `h`, which for an HMAC holds a midstate (104 bytes, a few nanoseconds against
  about 0.3 µs for one 64-byte block at §1.4's 207 MB/s). `Sha256` keeps no `drop` and stays Copy: hashing is not
  keyed, and copying a hash to fork a prefix is common.
- `HmacSha256` becomes a move type. For the prefix fork that Copy allowed, `public fn clone(borrow this) ->
  HmacSha256` (the name `Channel` and `Arc` use). Code that copied a MAC gets N0250 (used after it was moved) at its
  second use; the fix is `.clone()`. **(Question 2.)**
- Not wiped: the value's earlier places (G42), registers, and the C compiler's own spills. This is best effort, as in
  every library that wipes.

### 2.3 G26: `Crc32` and `Sha1` beside the others (`checksums.cee`)

- **`Crc32`**: reflected 0xEDB88320, initial value and final XOR all ones. `create`, `update`, `finish` and `of`, as
  `Crc32c`. `Crc32.of("123456789".bytes())` is 0xCBF43926. Its doc names S3's `x-amz-checksum-crc32`, gzip, zip and
  PNG.
- **Tables:** `CRC32_SLICES: [u32; 2048]`, slicing-by-8, written by `tools/gen_checksums.py` into
  `checksum_tables.cee` as Crc32c's are. They run where the CPU has no instruction and in the interpreter, at about
  1.4 GB/s (Crc64Nvme's rate, §1.4).
- **Runtime, two functions** (as ADR 0089 §2.1):
  - `uint32_t ceesafe_crc32_available(void)` returns how many bytes the instruction path takes at a time: 1 on AArch64
    with the CRC extension, 64 on x86-64 with PCLMULQDQ, 0 elsewhere. The answer is asked once, then a relaxed load.
  - `uint32_t ceesafe_crc32_update(uint32_t crc, const uint8_t *bytes, uint64_t length)` is called with a multiple
    of that size.
  - `Crc32.update` passes the largest such prefix and runs the tables on the rest (less than 64 bytes, x86-64 only).
- **AArch64:** the CRC extension that has `crc32cx` for ADR 0089 also has `crc32x` and `crc32b`, which compute
  exactly this polynomial. The probe is ADR 0089's: `__ARM_FEATURE_CRC32`, `sysctlbyname("hw.optional.armv8_crc32")`,
  `HWCAP_CRC32`, `PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE`. It is the same inline assembly with the other mnemonic,
  and the same one chain: about Crc32c's 8 GB/s on the M1 is expected (not yet run).
- **x86-64:** SSE4.2's `crc32` is CRC-32C only, so CRC-32 takes carry-less multiply folding (Gopal et al., Intel,
  "Fast CRC Computation for Generic Polynomials Using PCLMULQDQ", 2009, as zlib-ng and Chromium do it). Four 128-bit
  lanes are folded by x^(4·128±64) mod P, 64 bytes a step. At the end they are folded to one, then reduced to 32 bits
  by Barrett's method; the constants are computed by the generator and checked by the oracle. The mechanism is ADR
  0135's: intrinsics (`_mm_clmulepi64_si128`) in functions marked `target("pclmul,sse4.1")`, with no flag on any
  file. The probe is CPUID.1:ECX bit 1 (PCLMULQDQ) and bit 19 (SSE4.1). Expected several GB/s (not yet run).
  **(Question 3.)**
- **`Sha1`** (FIPS 180-4 §6.1): `create`, `update`, `finish` and `of` → `[u8; 20]`, in CeeSafe, shaped as
  `Sha256`. Its doc says first that SHA-1 is broken for collisions (SHAttered, 2017) and is here for protocols that
  name it: S3's `x-amz-checksum-sha1`, WebSocket's handshake, Git's object ids. For anything else, use `Sha256`. No
  instruction path: ARMv8's SHA1 and SHA-NI are left out, as they are for SHA-256 (ADR 0089 §4). No `HmacSha1`.
  **(Question 4.)**

### 2.4 G29: splits at the edges of the tree (`ordered_map.cee`)

- **The rule.** `put` already descends once to count the full nodes on the key's path. It also notes whether the key
  is past the tree's last key (the slot is the node's length at every level) or before its first (the slot is 0 at
  every level). When it is, each split on that path is uneven:
  - past the last key: `split` keeps `most() − 2` = 29 entries in the left node, moves one up, and leaves one entry
    (and two children) in the right node, which the key goes into;
  - before the first key: the mirror, one entry on the left and 29 on the right;
  - elsewhere: the middle, as today.

  `split` takes how many entries the left node keeps. Nothing else in `put` changes, nor what it allocates.
- **The invariant.** The doc comment changes from "a node other than the root holds at least B − 1 entries" to: every
  node holds at least B − 1 entries, except the root and the nodes on the tree's first and last paths, which hold at
  least one. `remove`'s `fill` and `merge` already work by comparison with `fewest()`. A thin node is never above
  `fewest()`, so it is borrowed into or merged as one at the minimum is. A merge that takes it in stays within
  `most()`, and every node keeps at least one entry, which is what `last()` and the cursors need. A node off those
  two paths only ever gets a middle split or a borrow from a node above the minimum, so it keeps B − 1 or more. The
  height is still O(log n): below the root's children, each thin node has a child at the minimum.
- **Prototyped and checked** (a copy of the file, renamed, in a scratch package, on 0.84.1):
  - 0 differences from lib/std's `OrderedMap` over 300 rounds, compared in both directions by cursor. Each round
    loads 50 to 29,053 keys in order (ascending or descending), makes 6,000 random puts, removals and appends, and
    then removes everything in a random order.
  - Peak footprint for 1M entries (base → edges): in order 184 → 97 MB, descending 184 → 97, scrambled 128 → 128,
    in order with local disorder (`i * 64 + rand % 512`) 183 → 128, two interleaved sorted streams 184 → 141.
  - Time: in-order puts 0.64 times as long (half the splits), scrambled puts and every lookup within noise (§1.4).
- **What it costs other workloads:** no pattern measured got worse. Removals at the ends of a map built in order find
  a thin node at once, so they borrow from a full sibling on the first removal, not the sixteenth. That is the same
  O(B) shift, earlier. A map that alternates appends and removals at its end (a queue) borrows at most once per
  removal, as today at the minimum. The cost in code is about 20 lines, and the invariant is less simple to state.
- **Not chosen:**
  - A bulk loader (`OrderedMap.fromSorted`, Rust's `bulk_build_from_sorted_iter`). It packs nodes fully and fixes
    the right border afterwards, but only for a whole sorted input into an empty map. It is a second API and a
    second tree builder, and it does not help appends after the load (CosmoStore's later versions). It can be added
    later on top of the edge rule.
  - Reserving a node's vectors for its current count (G29's third idea). That breaks "only the first step of a put
    allocates": a split or an insert would grow vectors after the map has changed. **(Question 5.)**

## 3. Consequences

- **G34:** an HMAC's key, inner and outer, is gone from its last place once the value is dropped, and so are
  `create`'s and `finish`'s copies on the stack. With 0197, `Aes256Gcm` does what ADR 0135 §2.1 said it did.
  **Source change:** code that copied an `HmacSha256` needs `.clone()`. None was found in
  lib/std, the corpus (its `Five` holds one and is never copied), CosmoStore (13 uses, each create-update-finish or
  `of`) or CeeSafe.Web (no use).
- **G26:** `Crc32` and `Sha1` in lib/std, with the same API as the others. CosmoStore and `web_ws` can delete
  theirs: CRC-32 goes from 377 MB/s to the instruction's rate, SHA-1 stays as fast. G35's gzip work then has its CRC.
- **G29:** maps loaded in key order, either way, are about 94% full instead of 50%. Their memory falls from 2.5 to 1.3
  times a HashMap's in the measured case, and their loads take 0.64 times as long. No API change.
- Two new `extern` declarations in lib/std (three if `ceesafe_wipe` comes with this ADR) move the global closure
  numbering of some HIR snapshots, as in ADR 0089 §7.2. Each changed line differs only in `(closure N)`, by the same
  amount, and is reviewed line by line.

## 4. Implementation and checks

- **Runtime:** `ceesafe_crc32_available` and `ceesafe_crc32_update` after Crc32c's section
  (`ceesafe_rt_io.c:4042`). x86-64 reuses AES-GCM's `cpuid` code (`cee_aesgcm_probe`, `:4198`), and AArch64 reuses
  `cee_crc32c_probe`. `src/library/runtime.cee` is regenerated by `tools/gen_runtime.py`, and `sources.cee` by
  `gen_library.py`.
- **Interpreter** (`src/interpreter/io.cee`, beside `ceesafe_crc32c_*` at `:1671` and `:2049`):
  - signatures `":u"` and `"uPs:u"`;
  - `ceesafe_crc32_available` answers 0, so the tables run;
  - `ceesafe_crc32_update` is modelled bit by bit with 0xEDB88320;
  - `ceesafe_wipe`, if this ADR brings it, writes zeros to the model's memory (signature `"Ps:-"`), as 0197 says.
- **Oracles:** `gen_checksum_cases.py` adds `Crc32` (Python's `zlib.crc32`) and `Sha1` (`hashlib.sha1`) at its 29
  lengths and offset feeds. It also adds an `HmacSha256.clone` case: a fork's code and the original's, each against
  the oracle. `crc_model.py` checks the folding constants. In the C harness, a CRC-32 of 200,000 random lengths (0 to
  4,095) and offsets against the bitwise model, as ADR 0089 §7.2 did. That runs on arm64, on x86-64 under Rosetta,
  and on a real x86-64 CPU (the owner's Ryzen or Xeon hosts), which also gives the throughput. `-Werror` under
  c-backend.md §1's set, with clang, gcc and MSVC.
- **The wipe is kept:** 0197 §4's disassembly check gets an HMAC case (create, update, finish, drop, at the default
  `-O2`): `ceesafe_wipe` is called after the last use. A `check-fail` case shows a copied `HmacSha256` used twice
  (N0250).
- **OrderedMap:** `run-pass/ordered-map.cee` gains a load in each order followed by removals from both ends. §2.4's
  comparison program becomes `tests/support` run-pass against a model in `tools/oracles/support_models.py`, with
  `--check-leaks`. The guide's performance page gets the footprint table.
- **Docs:** `spec/api/std.md` for `Crc32`, `Sha1`, and `HmacSha256.clone`, regenerated with
  `tools/gen_docs.py`, and the book's Appendix C (`tools/gen_appendix_c.py`). CosmoStore's G34, G26 and G29 entries
  are answered when this is released.

## 5. Questions for the owner

1. **The wipe:** HMAC's `drop` on ADR 0197's `ceesafe_wipe` and `Secret.wipe`, this ADR bringing `ceesafe_wipe` if
   it is built first (recommended)? Or a `drop` of plain stores now, which the C compiler removes (§1.1)?
   *Recommended: on 0197's wipe; a wipe that disappears at `-O2` only looks like one.*
2. **HMAC as a move type:** a `drop` plus `clone()` (recommended), or keep it Copy and offer only `mac.wipe()`, which
   callers must remember (G34's alternative)? *Recommended: `drop` + `clone()`; no known caller copies one.*
3. **CRC-32 on the CPU:** AArch64's `crc32x` and x86-64's PCLMULQDQ folding in this ADR (recommended), or AArch64 alone
   now and x86-64's folding as a follow-up, which keeps this ADR to inline assembly? *Recommended: both, x86-64 being
   where CosmoStore's Linux servers run, provided §4's x86-64 run passes before release; else AArch64 alone.*
4. **SHA-1:** in lib/std in software, with the doc's warning first (recommended), or left to packages because it is
   broken for collisions? *Recommended: in; two packages (CosmoStore, `web_ws`) already wrote it, and S3 names it.*
5. **OrderedMap:** edge splits at both ends in `put` (recommended), only at the right end (the more common appends;
   descending loads stay half full), or a bulk loader instead (§2.4)? *Recommended: both ends; the mirror is the
   same `split` with another count, and was checked the same way.*
6. **Release:** the three together in the next minor release once §4 passes, with G34 in the patch that 0197's
   question 6 proposes for its runtime wipe (recommended), or all of this ADR in that patch? *Recommended: G34 with
   the wipe fix; `Crc32`, `Sha1` and the edge splits in the minor release, since they add API and a runtime path
   that needs an x86-64 run. None blocks CosmoStore, which logs them as P2 and P3.*
