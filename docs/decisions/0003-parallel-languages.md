# Decision 0003 — the reference and CeeSafe in parallel; no CeeSafe packages

- Status: **Accepted** by the owner, 2026-10-30 ("Go with your recommendation: option 1, track the reference until P8").
- Supersedes: Decision 0002 §1 (the legacy mode). Decision 0002 §2–§4 stand.

## Context

Decision 0002 kept the reference's names as a legacy mode so that CosmoOS could migrate from the reference compiler to ceesafec. The owner now
intends the reference and CeeSafe to exist in parallel, each a language with its own command-line compiler: CosmoOS need not
migrate, and CeeSafe need not read the reference's packages.

## Decisions

1. **CeeSafe does not read CeeSafe packages.** Package mode finds only `ceesafe.toml`; its sources are `*.cee` and its lock
   `ceesafe.lock`. A directory (or a path dependency) holding only `ceesafe.toml` is reported as a CeeSafe package that
   CeeSafe does not read.
2. **Files named on the command line are still read whatever their extension** (`.c` files go to the C compiler). This
   is not a reference mode: it keeps the reference's corpus, which is CeeSafe's reference, usable as it is by every comparison tool.
   Whether ceesafec later accepts only `.cee` files is P8's to decide, with the command line.
3. **CeeSafe tracks the reference until P8.** Until CeeSafe's tools reach parity (P8), the two are one language with two
   implementations: CeeSafe follows the reference's specifications, re-pins the reference compiler releases and keeps the reference's corpus as its
   reference, and gaps still go upstream (the N track). At P8's gate the owner decides the fork point; from then on
   CeeSafe has its own specifications and corpus, and the comparisons with the reference compiler become regression tests against the
   fork point's output.
4. **Package tests** rename the reference's package fixtures to CeeSafe's names and map the names back
   (`tools/compare_packages.py`), so ceesafec's output is still compared with the reference compiler's.
5. **CosmoOS** stays on the reference and the reference compiler. ROADMAP P9's migration item is optional and is the owner's to take up.
