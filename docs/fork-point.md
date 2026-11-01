# The fork point

Status: **taken** at P8 S7 (2026-10-31), on the owner's word ("S7 next", with the choices below). CeeSafe and the reference
are now separate languages that started equal (Decision 0003 §3).

## Where the fork is

- **The reference side:** the reference `v0.6.0` (the reference compiler 0.6.0, sha256 in `bootstrap/reference.md`), the release CeeSafe tracked through
  P2–P8.
- **CeeSafe side:** commit `9c1669d` (P8 S6b) is the last whose output was byte-identical to the reference compiler 0.6.0's without any
  mapping. S7's commits switch the names below; nothing else in what ceesafec computes changed.

## What changed at the fork

| | Before (the reference's) | Since the fork (CeeSafe's) | Decided by |
|---|---|---|---|
| Generated C names | `cee_`, `ceeF_`, `ceeMk_`, … | `cee_`, `ceeF_`, `ceeMk_`, … | Decision 0004; owner, 2026-10-31 |
| Runtime | `ceesafe_*`, `CEE_*`, `CEESAFE_*`, `ceesafe/rt.h`, `ceesafe_rt_*.c`, `CEESAFE_ALLOC_REPORT`, … | `ceesafe_*`, `CEE_*`, `CEESAFE_*`, `ceesafe/rt.h`, `ceesafe_rt_*.c`, `CEESAFE_ALLOC_REPORT`, … | Decision 0004 |
| Program interface | `ceesafe_exports.h`, `CEESAFE_PROGRAM_*_H` | `ceesafe_exports.h`, `CEESAFE_PROGRAM_*_H` | Decision 0004 |
| ABI version | none | `CEESAFE_ABI_VERSION 1` in `rt.h`, checked after its `#include` in every generated unit | Decision 0004 |
| Reserved C names (N0292) | `ceesafe_`, `cee_`, `CEE_`, `nv` and a capital | `ceesafe_`, `cee_`, `CEE_`, `cee` and a capital | follows from Decision 0004 |
| Files on the command line | any extension (`.c` to the C compiler) | `.cee` and `.c` only; anything else is a usage error | Decision 0005; owner, 2026-10-31 |
| User-visible prose | "the reference" in messages, help, server texts, hover fences, the agent template | "CeeSafe" | owner, 2026-10-31 |
| Runtime messages | the reference's `panic:`, `alloc:`, `interp:` prefixes | `ceesafe panic:`, `ceesafe alloc:`, `ceesafe interp:` | prose and runtime |

Unchanged: the language, the diagnostic codes (N####) and every algorithm. The documents ceesafec embeds were the reference's
specification and guide at `v0.6.0` until Decision 0006 (2026-10-31) gave CeeSafe its own (`spec/`, `guide/`;
`docs/spec-plan.md`). Since then `explain` and the MCP server serve CeeSafe's text: their algorithms are compared with
The reference compiler's through a ceesafec built with the reference compiler's documents (`tools/reference_docs.py`), and the text is checked by
`tools/spec_check.py` and `tools/guide_check.py`.

How: the runtime, the libraries, the catalog and the agent template are generated from the reference's sources through
`tools/names.py`'s forward map; the compiler's own string literals were rewritten by the same map once, by script, with
three kinds of literal kept (CeeSafe package detection, references to the reference's repository); length-dependent slices of
renamed prefixes were fixed by hand (`ceesafe_host_`, `ceesafe_atomic_`, `ceeF_`, the reserved-name check).

## The regression baseline

The reference compiler 0.6.0's output, which equalled ceesafec's at the fork, is the frozen baseline. The reference compiler 0.6.0 is pinned and
deterministic, so it regenerates the baseline on demand instead of the repository storing hundreds of megabytes of it.

- `tools/names.py` holds one list of rules between CeeSafe's names and the reference's (since Decision 0006 also the
  specification's paths that the compiler's messages cite: `docs/X.md`↔`spec/X.md`, `cosmoos/profiles.md`↔`profiles.md`,
  `site/guide/`↔`guide/`). `map_back` turns ceesafec's output
  into the reference's names (and drops the ABI check, moving the `#line` directives after it back by its three lines);
  `map_forward` is its exact inverse, checked on the reference's 618 runtime, library, corpus, catalog and template files.
- Every comparison tool runs the reference compiler in the reference checkout and ceesafec in CeeSafe's corpus (`tests/reference/`, written
  by `tools/fork_corpus.py` from the reference `v0.6.0`: the same relative paths with `.cee` names, harness C under CeeSafe's
  names, snapshots kept as the reference's), and compares the reference compiler's output with ceesafec's mapped back.
- A mapping can hide a missed rename, so `ceesafe_names_in` (in `compare_c.py` and `compare_self.py`) fails on any the reference C
  name left in ceesafec's C outside string literals.
- **Fork divergences**: three check-fail programs test that the runtime's names are reserved (N0292). Their point
  changed with the names, so CeeSafe's copies use CeeSafe's reserved names and are checked only against their own
  annotations (`names.FORK_DIVERGENCES`).
- Two comparisons apply `map_back` to both sides: `compare_self.py` (CeeSafe's sources hold CeeSafe's names in string
  literals, which both compilers copy) and the MCP sessions (the reference's documents, served as they are, mention the CeeSafe
  port).
- From the fork on, an intended difference from the reference is a CeeSafe change with its own decision record, and it is added
  to the comparisons as a recorded exception (as `FORK_DIVERGENCES` and the four recorded check divergences are).

## Linking with the reference

Since the names are disjoint, a program may link C generated by the reference compiler and by ceesafec, each with its own runtime.
`tools/link_collision.py` builds a library with each, links both with one C `main`, and runs it; as a control, two
The reference compiler-built libraries must fail to link with duplicate symbols.
