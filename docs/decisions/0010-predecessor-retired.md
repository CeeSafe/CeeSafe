# Decision 0010 — The predecessor is retired, and named nowhere

- Status: **Accepted** by the owner, 2026-11-01 ("I want to remove all reference of [it] from CeeSafe, so any future
  sessions will use it"; then, asked how far: "Everything but licence").
- Relates to: Decision 0001 (identity and bootstrap), Decision 0006 (the specification), Decision 0007 (CeeSafe's own
  runtime and expectations), Decision 0009 (the release stage 0).

## Context

CeeSafe began as the port of an earlier language's C# compiler to that language, under a new name; it forked at that
compiler's 0.6.0 and has been developed on its own since Decision 0007. The earlier project is retired and its
repository private. Its name was still everywhere: agent instructions and an MCP server that told a session to use its
compiler, a bootstrap from its release, comparison tools, a name mapping that translated CeeSafe's output back to its
names before the corpus compared it, fixtures under its file names, and the record.

## Decision

1. **Nothing in the repository names it**, in any case or form (the name, its compiler's, its C prefixes, its file
   extension and manifest), except `COPYING.RUNTIME`, whose notice says where the code came from.
   `tools/spec_check.py` checks every tracked file's path and text, in the contract suite.
2. **The record keeps its meaning under a neutral name**: the earlier project is "the reference" and its compiler "the
   reference compiler", as the documents already called it; code written in the language is CeeSafe code. The
   inherited ADRs (`spec/decisions/0001`–`0030`) say so in their header. Links into its repository, which no longer
   resolve, became text.
3. **What only served the comparison is deleted**: the `compare_*` tools, the fork generators, `names.py` and the
   reference inventory, `tools/bootstrap.sh`, the oracles built from its source, and `tests/reference/lib` and
   `tests/reference/integrations` (copies of its libraries and CosmoOS checks).
4. **The corpus's snapshots are CeeSafe's**: each one that `run_corpus.py` compared after mapping ceesafec's output back
   to the old names (107 of them) was replaced by that same output unmapped, only where the mapped output equalled the
   snapshot byte for byte, and the mapping was removed. Test data that spelled the name now spells another word of the
   same length; the two expectations that depend on the bytes (an FNV-1a hash, a byte sum) were recomputed in
   Python.
5. **The compiler** no longer recognises the old manifest to explain that it does not read it; its identifiers and
   comments use CeeSafe's names. Agent instructions are `ceesafec agents`' own, and the MCP server is `ceesafec mcp`.

## Consequences

- A session in this repository is directed only to `ceesafec`, `bootstrap_release.sh` and CeeSafe's specification.
- The record loses the name of what it was compared with; the commits before this one keep it.
