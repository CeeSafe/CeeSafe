# ADR 0089 — Crc32c on the CPU's CRC-32C instruction

- Status: **Accepted** by the owner, 2026-11-11 ("CRC32C first as 0.30.0, accept as recommended": runtime functions
  with a run-time probe, one chain, released alone as 0.30.0, before ADR 0090). Implemented on branch `impl/0089` (§7). Raised by CosmoStore's CEESAFE_GAPS G31
  (`~/dev/CeeSafeApps/cosmostore/CEESAFE_GAPS.md`), and promised by ADR 0075 §2.5: "Hardware CRC … is left to a later
  ADR, and the API will not change."
- Scope: `lib/std/checksums.cee` (`Crc32c.update`), `runtime/hosted/ceesafe_rt_io.c` and `runtime/include/ceesafe/rt.h`
  (two functions), the interpreter's C library model (`src/interpreter/io.cee`). No language, checker or emitter
  change.

## 1. Context

- **What it costs today.** `Crc32c` is slicing-by-8 in CeeSafe: eight 256-entry tables, a bounds check per lookup.
  CosmoStore checks every extent it reads and every body it writes with it. In a profile of 1 MiB GETs (CosmoStore
  ADR-0034, M1), `Crc32c.update` was 28% of the server's busy time, more than the socket. It is what keeps
  CosmoStore's large GETs at half the rate of servers that use the instruction.
- **What the CPU offers.** x86-64 with SSE4.2 (2008 on; x86-64-v2) has `crc32` over 1, 2, 4 or 8 bytes. AArch64 with
  the CRC extension (optional in ARMv8.0, required from ARMv8.1; every Apple arm64 CPU since the M1, Graviton, Ampere,
  Cortex-A72 and later) has `crc32c[bhwx]`. Both compute exactly CRC-32C's reflected register step.
- **What stops lib/std using it.** CeeSafe has no intrinsics, and lib/std reaches the machine only through the runtime
  (`extern "C"` functions, as the atomics, `Random.fill` and the clock do). Neither baseline target guarantees the
  instruction: x86-64's baseline has no SSE4.2, and a generic `-march=armv8-a` has no CRC extension, so it must be
  asked for at run time.
- **Measured** (§7.3): 64 MiB, 20 times, best of five.

  | | M1 (macOS, Apple clang) | Xeon E3-1245 V2 (Debian 13) |
  |---|---:|---:|
  | `Crc32c.of`, ceesafec 0.29.0 | 1,652 MB/s | not measured |
  | `Crc32c.of`, this branch | **8,147 MB/s** | not measured |
  | the runtime function alone, C harness | 7,783 MB/s | 8,778 (gcc 14), 7,321 (clang 19) |

## 2. Proposal

### 2.1 Two runtime functions (`ceesafe_rt_io.c`, declared in `rt.h`)

| Function | Does |
|---|---|
| `uint32_t ceesafe_crc32c_available(void)` | 1 when the CPU has the instruction, else 0. Asked once per process, then a relaxed atomic load |
| `uint32_t ceesafe_crc32c_update(uint32_t crc, const uint8_t *bytes, uint64_t length)` | the reflected register after `length` bytes: 8 bytes per instruction, then one byte at a time |

- **How the CPU is asked**, with no helper library (CosmoOS links no compiler-rt, c-backend.md §1):
  - x86-64: `cpuid` leaf 1, ECX bit 20 (inline assembly; `__cpuid` on MSVC);
  - AArch64, where the compiler's target already has it (`__ARM_FEATURE_CRC32`, every macOS arm64 build): yes;
  - AArch64 macOS otherwise: `sysctlbyname("hw.optional.armv8_crc32")`;
  - AArch64 Linux: `getauxval(AT_HWCAP) & HWCAP_CRC32`;
  - Windows ARM64: `IsProcessorFeaturePresent(PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE)`;
  - anything else (CosmoOS userland, other systems or architectures): 0, and the tables.
- **How the instruction is reached:** inline assembly on GCC and clang (`crc32q`/`crc32b`; `crc32cx`/`crc32cb`, with
  `.arch_extension crc` when the target lacks it), so no file needs a `-msse4.2` or `+crc` flag; MSVC's intrinsics
  (`_mm_crc32_u64`, `__crc32cd`).
- **Contract** (AGENTS.md, a small C adapter behind a documented one): reads `bytes[0..length)` during the call only,
  keeps no pointer, writes nothing, allocates nothing, cannot fail. `@effects()`.

### 2.2 `Crc32c.update` (`lib/std`)

`update` asks `ceesafe_crc32c_available()`; on 1 it passes the slice to `ceesafe_crc32c_update` in one `unsafe` call
(its safety comment: the runtime reads at most the slice's length, during the call), else it runs the tables as
today. `create`, `finish` and `of` are unchanged, and so are all results. Crc64Nvme is unchanged (its instruction
would be carry-less multiplication, a different ADR).

### 2.3 The interpreter

Its C library model answers `ceesafe_crc32c_available` with 0, so interpreted programs run the tables, as on a CPU
without the instruction; `ceesafe_crc32c_update` is modelled bit by bit all the same, so no call can fault. Outputs
are the same in both, since the CRC is the same function.

## 3. Alternatives considered

- **A. Intrinsics in the language** (`Cpu.crc32c(crc, word)` safe built-ins, with target-feature queries). General,
  and the right home for SIMD later, but a new built-in kind through the checker, NIR, the emitter and the
  interpreter, plus a design for per-function target features. Not needed for one checksum; this ADR does not rule it
  out, and lib/std could move onto it without an API change.
- **B. Compile-time selection only** (`#if __SSE4_2__` / `__ARM_FEATURE_CRC32`). No probe, but a default x86-64 or
  Linux AArch64 build would never use the instruction: only macOS arm64 would gain.
- **C. A faster table** (slicing-by-16, no bounds checks through `unsafe`). Perhaps 1.5 to 2 times, against about 5.
- **D. Three interleaved chains** (as zlib and Linux do: the instruction's latency is 3 cycles and its throughput 1 per
  cycle), combined by a carry-less multiply or shift tables: about 3 times more again on long buffers, at the cost of
  a combine step to get right. **(Question 2.)**

## 4. Not in this ADR

- CRC-64/NVME and CRC-32 (ISO-HDLC) by carry-less multiplication (`pclmulqdq`, `pmull`).
- SHA-256 and MD5 by instruction (SHA-NI, ARMv8 SHA2).
- Kernel profiles: lib/std is hosted only (ADR 0026), so `Crc32c` does not exist there.

## 5. Testing

- **The corpus:** `run-pass/checksums.cee` (generated by `tools/oracles/gen_checksum_cases.py`, expectations from
  Python's `hashlib` and the bitwise model in `tools/oracles/crc_model.py`) covers 29 lengths around every block
  boundary and incremental feeds that start at odd offsets. Compiled, it now runs the instruction path on any CPU that
  has it; interpreted, the tables. Both must print the oracle's lines.
- **The runtime functions alone** (§7.2): 200,000 random lengths (0 to 4,095), offsets (0 to 15) and seeds against a
  bitwise CRC-32C, on arm64 (Apple clang, with and without `__ARM_FEATURE_CRC32`) and x86-64 (gcc 14, clang 19).
- **Strict warnings:** `ceesafe_rt_io.c` compiles under c-backend.md §1's set with `-Werror` on Apple clang, gcc 14
  and clang 19.
- **Not covered by a machine here:** MSVC (x64, ARM64) and AArch64 Linux; the CI's Windows job and an AArch64 Linux
  runner would be (Question 3).

## 6. Questions for the owner

1. **Mechanism:** runtime functions with a run-time probe, as proposed, or wait for language intrinsics (A)?
2. **Interleaving (D):** one chain now (proposed: 4.9 times, simple), and three chains as a follow-up when a profile
   asks for it, or three chains in this ADR?
3. **Release:** 0.30.0 with this alone (proposed, so CosmoStore can take it), or with whatever is next?

## 7. Implementation and evidence (2026-11-11, branch `impl/0089`)

### 7.1 Where it is

- `runtime/hosted/ceesafe_rt_io.c`: the probe, the two functions, after `Random.fill`'s section;
- `runtime/include/ceesafe/rt.h`: their prototypes;
- `lib/std/checksums.cee`: the `extern "C"` block and `Crc32c.update`;
- `src/interpreter/io.cee`: the two functions in the C library model;
- `src/library/runtime.cee`, `src/library/sources.cee`: regenerated (`tools/gen_runtime.py`, `tools/gen_library.py`).

### 7.2 Runs

- **The runtime section in a C harness** (the section extracted, `ceesafe/rt.h` included): check value 0xE3069283;
  0 mismatches in 200,000 random cases on each of: M1, Apple clang, default target; M1, `-march=armv8-a` (no
  `__ARM_FEATURE_CRC32`: the `.arch_extension` and `sysctlbyname` path); Xeon E3-1245 V2, gcc 14.2 and clang 19.1.7
  (`cpuid` path).
- **`ceesafe_rt_io.c`** with `-std=c11 -O2 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror`:
  clean with Apple clang (arm64), gcc 14.2 and clang 19.1.7 (x86-64).
- **ceesafec** built from the branch by 0.29.0 (`out/ceesafec build`, 56 s); `run_corpus.py --family run-pass
  --filter checksum --check-leaks`: 1 passed.
- **Three HIR snapshots changed, by review:** `check-pass/async-timeout.hir`, `effects-of.hir` and
  `string-braces-warning.hir` number closures by a global count that lib/std's two new `extern` functions move by 2.
  Every changed line differs only in `(closure N)`, and every N by +2 (checked line by line); nothing else in them
  changed. With them, check-pass passes 38 of 38, as 0.29.0 does.
- **The contract suite** (`tools/contract_tests.py`) on the first build: every check passed except those three
  snapshots and `spec_check`'s embedded documents (fixed by `tools/gen_docs.py` and a rebuild), among them run-pass
  317, run-fail 50 and ffi 18 under `-Werror`, ASan and TSan, the interpreter, and the fault, cancel and allocation
  sweeps. On the final build: §7.4.

### 7.3 Throughput

`Crc32c.of` over 64 MiB, 20 times, best of five, a CeeSafe program built by each compiler, on the M1 (load average
8 to 18, other sessions): 1,652 MB/s with 0.29.0, 8,147 MB/s with this branch; the same CRCs.

### 7.4 The final build (2026-11-11)

`tools/run_corpus.py` over the whole corpus: parse-pass 50, parse-fail 38, check-pass 38, check-fail 249, run-pass
317, run-fail 50, emit-c 29, ffi 18, bounds-checks 4, overflow-checks 2, placement 3, closure-calls 1, frames 3, all
passed (freestanding 4 partial, as on main). `tools/contract_tests.py`: every check passed but `spec_check`, which
found only this ADR's last edit not yet embedded; after `tools/gen_docs.py` and a rebuild, `spec_check`: 0 problems.
Not run: the fixed point (`tools/fixed_point.py`), which belongs to the release.
