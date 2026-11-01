# CeeSafe's reference corpus

Written by `tools/fork_corpus.py` from the reference v0.6.0 at the fork point (`docs/fork-point.md`): the reference's corpus, benchmarks,
programs and libraries as `.cee` files, with CeeSafe's names in the C harnesses; the snapshots stay the reference's, the frozen
baseline, and ceesafec's output is mapped back before it is compared with them. The comparison tools
run ceesafec here and the reference compiler in the reference checkout. Do not edit by hand before the fork's first CeeSafe-only change.

Since Decision 0007 (CeeSafe's runtime; the first C that differs from the reference compiler's), the `emit-c` snapshots are CeeSafe's
own: an intended change of the C updates them by a reviewed edit, recorded in the decision. Changed so far:
`tests/corpus/emit-c/arena.c` (region helpers call `ceesafe_region_alloc_fast`, i.e. `ceesafe_region_alloc_fast`).
