# Decision 0008 — CeeSafe owns its library

- Status: **Accepted** by the owner, 2026-10-31 ("Library first", asked where the `Async` API should live; then the
  async/await implementation plan approved, whose A0 step 1 this is).
- Relates to: Decision 0006 (CeeSafe's own specification), Decision 0007 (CeeSafe owns its runtime), ADR 0031
  (async/await, which needs library code), `docs/async-await-proposal.md`.

## Context

The reference is no longer developed (Decision 0007). ceesafec still embedded its libraries (`lib/core`, `lib/std`,
`lib/kernel`) by reading them from a clean the reference checkout at `v0.6.0` and renaming them with `tools/names.py`'s
`map_forward` (`tools/gen_library.py`). No library change could be made in CeeSafe without editing generated output,
and the async/await work (ADR 0031) puts timers, console output, channels and `Async.Mutex` in `lib/std`.

## Decision

1. **`lib/` is CeeSafe's.** The thirteen files (`lib/core/{address,alloc,atomics,mmio,slices}.cee`,
   `lib/std/{host,sync}.cee`, `lib/kernel/{cache,heap,kobject,list,quiesce,sync}.cee`) were recovered from the text
   ceesafec embedded (`src/library/sources.cee`), which is the reference `v0.6.0`'s under CeeSafe's names.
2. **`tools/gen_library.py` reads `lib/`** from the tree: no checkout of the reference, no renaming.
3. **`tests/reference/lib/` stays** as the record of the reference's libraries at the fork, under the reference's names. Nothing reads it to build
   ceesafec any more. `tools/spec_check.py`'s API check now documents `lib/core` itself.
4. `spec/architecture.md` §6, `spec/README.md` §4, `docs/spec-plan.md` and `AGENTS.md` say so.

## Consequences

Checked 2026-10-31 before any library edit:

- The recovered files equal `map_forward` of `tests/reference/lib/` for all 13 (a second, independent route to the
  same text).
- Regenerating `src/library/sources.cee` from `lib/` changed only its header comment and one doc comment. Every path,
  group, chunk count and chunk literal is byte-identical.
- `tools/spec_check.py --ceesafec out/ceesafec`: 0 problems.
- `out/ceesafec fmt --check` is clean.

The contract suite result is recorded in ROADMAP §10.
