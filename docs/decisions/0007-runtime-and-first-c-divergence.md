# Decision 0007 — CeeSafe owns its runtime; the first C that differs from the reference compiler's

- Status: **Accepted** by the owner, 2026-10-31 ("yes, implement 1 and 2"; then "Fix those things you noticed", which
  took the recommended option (a) of §4).
- Relates to: Decision 0003 §3 (the fork point), Decision 0004 (CeeSafe's C names, the ABI version), Decision 0006
  (CeeSafe's own specification), `docs/fork-point.md` ("an intended difference from the reference is a CeeSafe change with its
  own decision record"), `benchmarks/README.md`.

## Context

The reference is no longer developed. ceesafec still embedded the C runtime by reading it from a reference checkout at `v0.6.0`
(`tools/gen_runtime.py`), so no runtime change could be made in CeeSafe without editing generated output.

The benchmark suite (`benchmarks/`, CeeSafe against Rust and C, 2026-10-31) found two costs in the runtime and the
generated C, measured by editing copies of ceesafec's C:

- **Arena allocation** went through two out-of-line calls (`ceesafe_region_alloc`, then `ceesafe_arena_alloc`) for
  every node; the hand-written C bumps a pointer inline. `arena` took 1.37× Rust's time.
- **A box of zero bytes** (`Box.create([0u8; 65536])`) was `ceesafe_alloc` followed by a zeroing loop. clang makes the
  C version's `malloc` + `memset` one `calloc`, and Rust's `Box::new([0u8; N])` is `alloc_zeroed`; a C library's
  `calloc` can skip zeroing memory that already is. `large-objects` took 1.26× Rust's time.

## Decision

1. **`runtime/` is CeeSafe's.** The six files (`include/ceesafe/rt.h`, `core/ceesafe_rt_core.c`,
   `hosted/ceesafe_rt_{hosted,io,float,thread}.c`) were written from the reference `v0.6.0` with `tools/names.py`'s
   `map_forward` and the ABI define, exactly the text ceesafec embedded; `tools/gen_runtime.py` now reads them from
   `runtime/`. Regenerating `src/library/runtime.cee` from them changed only its header comment (checked before any
   runtime edit).
2. **Arena fast path inline.** `ceesafe/rt.h` gains `static inline ceesafe_region_alloc_fast`: an arena's bump within
   its current chunk (the same test and result as `ceesafe_arena_alloc`'s fast path), anything else through the
   exported `ceesafe_region_alloc`, which is unchanged, so the runtime's exported ABI is unchanged. The generated
   region helpers call it (`src/backend/c_helpers.cee`; `spec/c-backend.md`, the `FixedBufferAllocator` row).
3. **Zeroed boxes.** A box built in place from an array repeat of a constant whose bytes are all zero (an integer or
   `bool` 0, a float +0.0), from a heap allocator rather than a user-defined one, calls a new per-type helper
   `ceeBZ_X<T><A>()`, which returns `ceesafe_alloc_zeroed(sizeof(T), id)`, and writes nothing. `ceesafe_alloc_zeroed` is
   counted and fault-injected exactly like `ceesafe_alloc` and freed by `ceesafe_free`; the hosted platform's
   `ceesafe_platform_alloc_zeroed` is `calloc` (`src/backend/c_body.cee`, `c_helpers.cee`; `spec/c-backend.md`, the
   `Box<T, A>` row).
4. **ceesafec's C is no longer compared byte for byte with the reference compiler's.** the reference compiler's C cannot change, so from here on
   ceesafec's C differs from it as intended: for every program with a region allocation or a zeroed box (13 of 249
   corpus, benchmark and program files), and for ceesafec itself. The `emit-c` snapshots in `tests/reference/` become
   CeeSafe's own: an intended change updates them by a reviewed edit recorded in its decision
   (`tests/reference/README.md`); `emit-c/arena.c` is the first. No test compares with the reference compiler any more ("Drop the suite's
   comparisons with the reference compiler … We have migrated from the reference for good", the owner, 2026-10-31): `tools/contract_tests.py`
   takes only `--ceesafec` and checks CeeSafe's own expectations (the fixed point, the corpus's annotations and
   snapshots, the support library, the sweeps, the specification); `spec/testing.md` §3. The `tools/compare_*.py`
   scripts remain only as the record of the port's parity with the reference compiler 0.6.0.

## Consequences

Measured 2026-10-31 on arm64 macOS, Apple clang 21, 5 runs (`python3 benchmarks/run.py`): `arena` 1.37× → 0.90× Rust's
time, `large-objects` 1.26× → 1.00×.

Checked 2026-10-31 with the new compiler: the fixed point (stage 1 and stage 2 emit identical C for ceesafec);
`tests/reference/tests/corpus` all families pass except `emit-c/arena` (its snapshot, the reference's, has the out-of-line
call); `CEESAFE_FAIL_ALLOC_AT` fails a zeroed box's allocation and `CEESAFE_ALLOC_REPORT` balances
(`attempts=400001 live=0` for `large-objects`); `ceesafec fmt --check` is clean. 13 of 249 corpus, benchmark and
program files emit different C, all through region allocation or a zeroed box.

After the snapshot edit (only the two region helpers' calls changed, the rest of the mapped-back C identical) the
whole reference corpus passes with leak checks (2026-10-31). The full benchmark run is in `guide/performance.md`.

Also with this change: `src/support/` builds on its own again (`none()` moved from `src/syntax/ast.cee` to
`src/support/none.cee`); `tests/support` became `.cee` and runs with ceesafec (8/8 with leak checks; fault sweep 573
of 573 failure points); `tools/fault_sweep.py` uses the `CEESAFE_` variables; `benchmarks/run.py` builds the CeeSafe
programs from `tests/reference/benchmarks/suite/` instead of copies.
