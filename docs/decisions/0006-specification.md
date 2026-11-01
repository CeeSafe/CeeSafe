# Decision 0006 — CeeSafe's specification

- Status: **Accepted** by the owner, 2026-10-31 ("Fork the reference's text", "Inherit as CeeSafe decisions", "Yes, with
  checked examples"), after P8 (`docs/fork-point.md`).
- Relates to: Decision 0003 §3 (the fork point: CeeSafe and the reference become separate languages that started equal),
  Decision 0004 (CeeSafe's C names), Decision 0005 (`.cee` and `.c` on the command line), `docs/spec-plan.md`.

## Context

Until now ceesafec embedded the reference's documents at `v0.6.0` (its `docs/`, the guide and the agent template) and served
them, unchanged, through `explain` and the MCP server. They describe the reference: its names, its C names, its `.cee` files,
its compiler, its milestones and CosmoOS's history. Since the fork CeeSafe needs a specification of its own: the text
that "spec before code" (the reference's rule, which CeeSafe keeps) refers to, and the text its tools serve.

## Decision

1. **CeeSafe's specification is a fork of the reference's text at `v0.6.0`**, not a new text. At the fork the two languages are
   equal, so the reference's precise rules are CeeSafe's rules. The normative documents (the language specification, grammar,
   type system, ownership, lifetimes, memory model, C backend, intermediate representation, optimisations, `unsafe`,
   diagnostics, formatting, tooling, testing, architecture, the profiles and the `lib/core` API) become `spec/`. They are
   renamed under `tools/names.py`'s rules (Decision 0004's C names, `.cee`, `ceesafec`, `ceesafe.toml`, "CeeSafe"), and
   carry the fork-point changes: the ABI version, `.cee`/`.c` only, and CeeSafe's reserved C names. The reference's history (its
   milestones, roadmap, risks, release notes, research and CosmoOS reports) is not forked. Links to it point to the reference's
   repository at `v0.6.0`.
2. **the inherited ADRs 0001–0030 are inherited** as CeeSafe's language decisions, in `spec/decisions/`, so that the
   specification's "ADR NNNN" citations keep resolving. Each keeps the reference's text, which is a historical record, under a
   header saying that CeeSafe inherited it at the fork. CeeSafe's own language decisions continue the series from
   ADR 0031 in the same directory. The port's decisions (this one, 0001–0005) stay in `docs/decisions/` as "Decision
   NNNN".
3. **The guide is forked too**, as `guide/`, with its examples in `ceesafe` blocks. `tools/guide_check.py` compiles every
   one with ceesafec and checks what the page says it does, under the rules of the reference's `SiteTests`.
4. ceesafec embeds `spec/`, `guide/` and `templates/AGENTS.md` (`tools/gen_docs.py`). Milestone tags in the text (M1–M15,
   C0–C9, K1–K7) name the reference milestone that introduced a rule. They are kept for tracing a rule back to the reference's
   history.
5. From now on the specification is CeeSafe's: a change to the language is a change to `spec/` first, with an ADR when
   it is a decision, and a recorded exception in the comparisons with the reference compiler 0.6.0 (`docs/fork-point.md`).

## Consequences

- `explain` and the MCP server serve CeeSafe's text. The reference compiler's output is still the baseline for the algorithms that find
  and rank sections. A ceesafec built with the reference's documents (`tools/reference_docs.py`) is compared with the reference compiler, as
  before the fork, while the shipped ceesafec is checked against its own specification (`tools/spec_check.py`).
- The specification is written by `tools/fork_spec.py` once, from a clean the reference `v0.6.0` checkout. After that it is
  edited by hand, and the tool refuses to overwrite it.
