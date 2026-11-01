# Limits and allocation failure

The maximum sizes CeeSafe supports, what happens past them, and how allocation failure is handled (ROADMAP P1:
"Specify the maximum supported sizes and IDs, and the allocation-failure behaviour"). Each limit says where it is
enforced and whether that enforcement exists yet. A limit that is only written here is a plan, not a check.

## 1. Sizes and IDs

| Quantity | Type | Limit | Reference (the reference compiler 0.3.1) | Enforced |
|---|---|---|---|---|
| Source file size | byte offsets `u32` | 2³¹ − 1 bytes | spans are `int` UTF-16 offsets (`SourceText.cs:3`), and a .NET string holds at most about 2³⁰ characters | **not yet.** `SourceText.create` narrows with `truncate<u32>`. P2 must reject a larger file with a diagnostic before lexing. |
| Lines per file | `u32` line starts | follows from the file size | `int` | by the file size |
| Columns | `u32`, UTF-16 units | follows from the file size | `int` | by the file size |
| Interned names | `Symbol { id: u32 }` | 2³² − 2. The index stores id + 1, so the count must stay below 2³² − 1. | `int` ids (`SymbolTable.cs`) | **not yet**, P2. The count is bounded far earlier by memory; an explicit check before 2³¹ keeps parity with the reference's `int`. |
| Bytes of all interned names | `u32` offsets | 2³² − 1 | — | **not yet**, P2 (with the file limit, total input is bounded per compilation) |
| Map entries | `u32` slots | 2³² − 2 | `Dictionary` (`int` count) | **not yet**, P3 |
| Table IDs (AST, HIR, NIR, types) | `u32` per table, typed | 2³² − 1 per table | object references | at each table's `add`, from P2 on |
| Bitset size | `usize` bits | memory | `BitArray` (`int`) | memory |
| Exact integers | `BigNat`, `u32` limbs | memory. Constant evaluation stays within a 128-bit magnitude; float formatting needs about 1,200 bits. | `BigInteger` | memory |
| Nesting depth (expressions, blocks, types, patterns) | recursion | **to decide in P2**: a fixed limit with a diagnostic | **none.** 100,000 nested parentheses crash the reference compiler 0.3.1 with SIGSEGV (exit 139) and no message; 10,000 are accepted (measured 2026-10-30) | P2. Matching the reference here would mean matching a crash, so this is a recorded divergence: the limit and its diagnostic need a reference spec entry first (N track). |

Every narrowing to `u32` or `u8` uses `truncate<T>` today. Each one must become a checked conversion, or rest on one
of the checks above, before the table that uses it is exposed to input. P2's review lists them.

## 2. Allocation failure

- Every operation in `src/support/` that allocates returns `Result<…, AllocError>`.
- A failed operation leaves its structure as it was before the call: nothing half-added is visible, and everything
  added before is still there. Specifically:
  - `Map.put` pushes the key and value before placing the index slot, and truncates the key on a failed value push;
  - `Interner.intern` stores the text before the name becomes findable, and truncates all three tables on failure;
  - the `grow` methods build a new slot vector before replacing the old one.
- Nothing leaks after a failure: the runtime's `live=0` at exit.
- The compiler (P2 on) turns an `AllocError` into one controlled diagnostic and exit status, and publishes no
  partial output. `!` (panic on error) is allowed only in tests and probes.

Evidence (run 2026-10-30, macOS arm64, the reference compiler 0.3.1). `tools/fault_sweep.py` builds
`tests/support/fault/alloc-sweep.cee` with `src/support/`, which exercises every support type. It then fails each of
the 573 allocation attempts in turn (`CEESAFE_FAIL_ALLOC_AT`). Results:
- all 573 came back as errors with the structures consistent, and with `live=0`;
- a planted rollback bug in a copy of `interner.cee` (the name made findable before its text is stored) was caught
  as panics at the failure points.

## 3. What is not limited

- Hash collisions affect only speed. Iteration order is insertion order, never hash order.
- Determinism does not depend on any of these sizes.
