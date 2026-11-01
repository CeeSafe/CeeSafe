# Decision 0004 — The C prefix and ABI version

- Status: **Accepted** by the owner, 2026-10-30 ("Go with option 2, switch the prefix at P8"). Proposed at P7 S4.
  The names were confirmed on 2026-10-31 ("cee_ as proposed") and switched at P8 S7 (`docs/fork-point.md`), with the
  ABI version and the link-collision test (`tools/link_collision.py`).
- Relates to: ROADMAP §P7's third item ("Switch to the approved CeeSafe C prefix and ABI version, keeping a versioned
  legacy mode with link-collision tests"), Decision 0003 (the reference and CeeSafe in parallel; CeeSafe tracks the reference until P8).

## Context

ceesafec emits the C the reference compiler emits, byte for byte, and that identity is the port's strongest check: C over 308 programs
in four shapes and in both forms (P5's and the reference compiler's default), the four reports, and CeeSafe's own 404,458 lines
(`docs/p7-plan.md`). The C names are the reference's:

| Where | Names | Count in CeeSafe's own C (default form) |
|---|---|---:|
| Generated code | `ceeR_` `ceeT_` `ceeD_` `ceeF_` `ceeV_` `ceeM_` `ceeL_` `ceeE_` `ceeO_` `ceeVP_` … (about 20 families), `cee_` | 186,000 |
| Runtime and helpers | `ceesafe_*`, `CEE_*`, `CEESAFE_*`, `ceesafe/rt.h`, `ceesafe_rt_*.c` | 135,000 |
| Program interface | `ceesafe_exports.h`, `CEESAFE_PROGRAM_*_H`, `@export`ed names (unprefixed) | — |

ROADMAP P7 planned a switch to a CeeSafe prefix, with the reference's kept as a versioned legacy mode. Decision 0003 has since
removed the legacy package mode and made the two languages parallel: CeeSafe follows the reference until P8, where the owner
picks the fork point and the comparisons with the reference compiler become regression tests against the fork point's output.

A prefix matters only where C from both compilers is linked into one program (a CeeSafe module in a reference kernel, a
library built by each): two runtimes with the same `ceesafe_*` symbols and `ceesafe/rt.h` guard would collide. Nothing does
that today: CosmoOS stays on the reference (Decision 0003 §5).

## Options

1. **Switch now (P7).** Every comparison tool then maps CeeSafe's names back to the reference's before comparing. The map has
   to cover about 25 prefix families and the runtime's file and guard names, and a mapping bug can hide a real
   difference: the identity check gets weaker while CeeSafe still tracks the reference.
2. **Switch at the fork point (P8), recommended.** P7 keeps the reference's names, so every comparison stays byte-for-byte.
   At P8's gate, with the fork, one change renames the C names and the runtime, adds the ABI version, and the
   regression baseline is taken after it. Until then, linking the reference compiler's and ceesafec's C into one program is
   unsupported and documented as such.
3. **Never switch.** Simplest, but then the parallel languages can never share a program, and `ceesafe_*` in CeeSafe's
   output misnames it.

## Proposal

Option 2, with these names when the switch is made:

| The reference | CeeSafe |
|---|---|
| `cee_`, `nv<X>_` (generated code) | `cee_`, `cee<X>_` |
| `ceesafe_*` (runtime functions) | `ceesafe_*` |
| `CEE_*`, `CEESAFE_*` (macros) | `CEE_*`, `CEESAFE_*` |
| `ceesafe/rt.h`, `ceesafe_rt_*.c` | `ceesafe/rt.h`, `ceesafe_rt_*.c` |
| `ceesafe_exports.h`, `CEESAFE_PROGRAM_*_H` | `ceesafe_exports.h`, `CEESAFE_PROGRAM_*_H` |
| (none) | `CEESAFE_ABI_VERSION 1` in `rt.h`, checked by every generated unit with `#if`/`#error` |

`cs_` was considered and rejected: the Capstone disassembler's public API uses it. `cee_` is also used by the small
cee-utils library (of the Concord Discord client); that overlap is judged acceptable, but the prefix is the owner's choice. `@export`ed names stay as written.
The link-collision test (ROADMAP P7) moves to P8 with the switch: one program linking a the reference compiler-built and a
ceesafec-built library, which must link and run.

## Consequences

P7 closes without its third item, which moves to P8's list with the link-collision test. ROADMAP §P7 and §P8 and the
rename table say so. The names in the table above are the proposal's; the owner may still change the spelling at P8.
