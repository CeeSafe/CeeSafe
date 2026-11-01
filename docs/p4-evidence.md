# P4 evidence: the invariants of ownership.md

Every invariant of the reference's `docs/ownership.md` (tag v0.6.0), with the corpus programs that must fail and the
near misses that must pass. The rows for O, B, E and R3 are the reference's own (`docs/milestone-2.md`, "Invariant /
Must-fail / Near-miss"); the others are chosen here from the corpus by their codes and subjects.

All evidence in this file was run on 2026-10-30 against ceesafec at P4 S6. Nothing in it is only proposed.

"Identical" means that `ceesafec check` and the reference compiler 0.6.0 give the same output for a file, under `tools/compare_parse.py
--command check`. That comparison covers the text diagnostics, the JSON diagnostics and `--dump-hir`. For a run-pass
program it also means `tools/compare_nir.py`: the same NIR after drop elaboration.

Results:

- **Check comparison:** 318 of 322 check-pass, check-fail and run-pass programs are identical.
- **The four that differ:** these are recorded divergences, and each one's plain `check` output is still identical:
  - `check-fail/enums`, `optionals` and `results`: the reference compiler's `check --dump-hir` printer crashes on them;
  - `check-fail/offset-of`: the reference issue #6.
- **Other families:** 84 of 84 identical.
- **NIR:** 284 of 284 identical.

## Ownership, borrowing, escape (M2)

| Invariant | Must fail (check-fail) | Near misses (pass) |
|---|---|---|
| O1 unique owner, O2 move de-initialises | `moves` (N0250, N0251, N0252), `box-moves`, `generic-moves`, `io-moves`, `optional-moves`, `c-array-moves`, `mmio-moves`, `move-closures-moves`, `static-moves` | check-pass `moves-accepted`; run-pass `drops-moves-and-returns` |
| O3 definite initialisation | `definite-assignment`, `field-of-unassigned`, `moves` (`wholeAfterPartial`) (N0212) | check-pass `definite-assignment`, `moves-accepted` |
| O4 exactly-once destruction | not applicable (it is a code-generation rule) | NIR identical after drop elaboration: run-pass `drop-order`, `drop-flags`, `drops-early-exit`, `drops-moves-and-returns`, `enum-drops`, `for-drops`, `using-and-defer`. Running these under ASan, and the fault sweep, wait on P5 (see below) |
| O5 partial initialisation | `moves` (`wholeAfterPartial`), `field-of-unassigned` | check-pass `moves-accepted` (`reinitializeMovedField`, `movedFieldOtherFieldStillUsable`) |
| B1 XOR aliasing | `borrow-conflicts`, `borrow-two-phase`, `precise-borrows`, `ownership-more` (N0241) | check-pass `borrows-accepted` (`disjointFields`, `manySharedBorrows`) |
| B2 loan validity | `borrow-conflicts`, `borrow-temporaries`, `shared-path-mutation` (N0240, N0242, N0243) | check-pass `borrows-accepted` |
| B3 liveness (NLL) | `borrow-conflicts` (`inLoop`), `for-borrowed-elements`, `reference-reassignment` | check-pass `borrows-accepted`, `scoped-payload-liveness` |
| B4 reborrowing | `borrow-reborrow` | check-pass `references` |
| B5 two-phase borrows | `borrow-two-phase`, `borrow-this-generic-key` | check-pass `borrows-accepted` (`twoPhaseMethodCall`) |
| E1 no escape beyond the owner | `borrow-returns` (N0244), `slice-returns`, `arena-escapes`, `region-escapes`, `pool-escapes` | check-pass `references`, `slice-borrows-accepted` |
| E2 return elision | `borrow-returns` (N0245), `reference-types` | check-pass `references` |
| E3 scoped fields only with region parameters | `reference-types`, `scoped-types`, `region-declarations` | check-pass `regions-accepted` |
| E4 no scoped globals or heap | `reference-types`, `scoped-types`, `region-flows` (N0272) | run-pass `region-near-misses` |

The M2 mutation suite (`tools/mutation_suite.py`) gives O2, O3, B1–B3 and R3-free straight-line code a second,
independent check. It reproduces the reference's 1,152 mutants of its 5 seeds: 120 per seed resolve their names, and
between 54 and 83 per seed are rejected. Its oracle shares no code with either compiler. Both ceesafec and the reference compiler
match the oracle on every mutant. The suite's seeded .NET `System.Random` was checked against .NET 10 itself: 120
draws over 3 seeds were identical.

## Regions (M4) and the heap (M3, M6)

| Invariant | Must fail | Near misses |
|---|---|---|
| R1 allocation borrows the region | `arena-escapes`, `region-escapes`, `pool-escapes`, `pool-borrows`, `region-instances` | check-pass `arena-accepted`, `regions-accepted`; run-pass `readme-arena`, `region-graph`, `region-tokenizer` |
| R2 trivially-destructible contents | `arena-types`, `region-types` | check-pass `arena-accepted`; run-pass `arena-bulk-release`, `fixed-buffer-and-scratch` |
| R3 pinned `using` bindings | `moves` (`fromUsing`, `fieldFromUsing`, N0253) | check-pass `moves-accepted` (`usingIsBorrowable`); run-pass `using-and-defer` |
| R4 guard-protected borrows | `collection-invalidation`, `vector-invalidation`, `slot-map-invalidation`, `truncate-invalidation`, `slice-bulk-loans`, `string-view-from-utf8-loans` | run-pass `vectors`, `slot-maps`, `stable-vectors`, `pools` |
| H1 box ownership | `box-moves`, `box-borrows`, `boxes` | check-pass `boxes`; run-pass `boxes` |
| H2 collections | `vector-invalidation`, `collection-invalidation`, `vectors`, `slot-maps` | run-pass `vectors`, `ex9-vectors`, `stable-vectors` |
| H3 no owning cycles | not applicable: it holds by construction, since only `Box` and the owning collections own (O1) | run-pass `region-graph` (back-references as indices) |

## Threads (M13, ADR 0016, ADR 0018)

| Invariant | Must fail | Near misses |
|---|---|---|
| T1 only `Send` values cross | `threads` (N0334) | run-pass `threads` |
| T2 sharing through `Arc` of `Share` types | `threads` | run-pass `threads` |
| T3 the library's `@threadSafe` types | lib/std is checked with every hosted program and gives no diagnostics in either compiler (`--library-diagnostics`) | run-pass `threads` |
| T4 fork-join borrows end with the call | `thread-both-borrows`, `thread-both` | run-pass `thread-both` |

## Unsafe code (M8, ADR 0007)

| Invariant | Must fail | Near misses |
|---|---|---|
| U1 capabilities | `unsafe-capabilities`, `systems-capabilities`, `raw-pointer-types` | run-pass `raw-pointers`, `raw-slices` |
| U2 contract (`/// Safety:`) | `unsafe-safety-comments` (N0283) | run-pass `raw-moves` |
| U3 raw pointers are inert | `c-state-moves` (N0294), `raw-moves-c-state` (N0282: `p.read()` of C state) | run-pass `raw-moves`, `raw-pointers` |

The sticky shared deref (N0248, the reference `3a99b4a`) is in `check-fail/shared-path-mutation` (6 cases), which is identical.

## What P4 does not show

- **O4 at run time.** P4 shows that the drops are placed exactly as the reference places them: the NIR after
  elaboration is identical. The reference also checks that every program runs with every drop in order. That needs
  ceesafec's C, so it moved to P5's gate, which passed on 2026-10-30 (`docs/p5-plan.md`: the run corpus under ASan+UBSan
  with leak checks, the fault sweep's 789 points, the collection properties' drop orders); the interpreter's part is
  P8's:
  - run-pass under ASan and UBSan;
  - the M3 fault-injection sweep (`FaultInjectionTests.cs`);
  - the interpreter's agreement with native runs, which is P8's.
- **Soundness beyond the reference.** Matching the reference is not proof of the invariants. `docs/safety-audit.md`,
  The reference's audit pack, names where holes are most likely. A hole there is a hole here too.
