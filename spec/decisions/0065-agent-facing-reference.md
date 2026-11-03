# ADR 0065 — Agent-facing reference: the library API in llms-full.txt, `ceesafe_api`, and "did you mean"

- Status: **Accepted** by the owner, 2026-11-04 ("Go ahead implement 1-4"), after the review of the agent-facing
  documentation that followed the ClayWind port. Implemented on the working tree; not released.
- Scope: documentation (`spec/api/std.md`, new; `guide/builtins.md`, new; `guide/for-agents.md`; `templates/AGENTS.md`;
  the site's `llms.txt` and `llms-full.txt`), the MCP server (`ceesafe_api`), and diagnostics (N0100, N0200, N0230,
  N0234 messages). No language change.

## 1. Context

An agent porting ClayWind (~25k lines) to CeeSafe missed names that existed, and guessed names from other languages
that did not:

- it looked for `TextWriter.unsigned`, but `TextWriter.decimal(u64)` existed (`spec/api/core.md`);
- it wrote `Environment.args()` for `Process.argument(i)`, and `Timer.now().since(…)` for `Timer.now()`, a `u64`;
- it learned the async socket API from another program's source.

The causes were in what an agent reads:

- `llms-full.txt`, the one file `AGENTS.md` tells an agent to load, had the guide and the specification but not the
  library reference;
- lib/std had no API page at all;
- the compiler's own types (`Console`, `File`, `Process`, `Vector`, `StringView`, the integer methods) are described
  only in specification prose;
- the MCP server had no lookup by name (`ceesafe_symbol` is a hover at a position);
- "has no method named 'unsigned'" named nothing that does exist.

## 2. Decision

1. **The reference, complete and in one file.**
   - `spec/api/std.md` is `ceesafec doc lib/std --title "lib/std"`, and `tools/spec_check.py` checks it as it checks
     `core.md`.
   - `guide/builtins.md` tabulates the built-in types and functions. Each section ends with a run example, and a
     final type-checked block calls every row with the type the tables give, so `guide_check` fails when a table
     drifts from the compiler. Writing that block found two wrong rows: `checkedTruncate` and `saturatingTruncate` take
     only floats.
   - `llms-full.txt` now holds the guide (with the new page), the specification, and both API pages. `llms.txt` links
     lib/std, and tells agents to look a name up before concluding it is missing.
2. **`ceesafe_api`**, the MCP server's tenth tool (`src/tooling/api_lookup.cee`, tooling.md §6.3).
   - **Input:** a type (`TextWriter`), a member (`TextWriter.decimal`) or a bare member name (`decimal`).
   - **Results:** the matching entries of the two API pages and the built-in types page: a type's section, a member's
     entry, or the table rows that name it.
   - **When nothing matches,** up to eight suggestions:
     - the closest names by edit distance;
     - for `Type.member`, the type's real members;
     - the entries whose documentation mentions the member's name (`unsigned` finds `TextWriter.decimal`).
   - `tools/mcp_api_check.py`, a new contract, holds 15 lookups against names the documents state.
3. **"Did you mean"** (`src/check/suggest.cee`, diagnostics.md).
   - **Unknown member** (N0200, N0230): a struct's close members, then those whose doc comment mentions the name,
     at most three; a small type with no close member lists all its members.
   - **Built-in types:** ranked by the longest shared start, then edits, from the same lookup as `ceesafe_api`, so
     the error and the tool agree.
   - **Numbers:** an unknown method on a number is "has no method 'm'", with its close methods. Before, it was
     "has no methods", which became false with ADR 0063.
   - **Reserved words:** N0100 says when the token is a reserved word.
4. **The agent guide and template.**
   - **New sections in `guide/for-agents.md`:** looking names up, reserved words that read like names, optionals
     against results, `Console` against `AsyncConsole`, and `mut` at a call against `borrow mut` elsewhere. Each
     section has a checked example with the compiler's exact output.
   - **`templates/AGENTS.md`:** four new lines on the same.

## 3. Evidence (macOS arm64)

- **Checks:** `guide_check` passes 117 examples, `spec_check` reports 0 problems (including `std.md`), and
  `mcp_api_check` reports 0 problems over 15 lookups. The corpus passes 596 tests with 0 failures; no expectation
  needed a change.
- **Messages the port actually hit**, before and after:
  - `w.unsigned(42)` gives "; did you mean 'signed', 'decimal' or 'hex'?";
  - `Process.args(1)` gives "'argument', 'argumentCount' or 'run'";
  - `Console.writeLn` gives "'writeLine', 'write' or 'writeBytes'";
  - `Environment.args()` gives "it has 'variable'";
  - `n.mx(1)` gives "'max'".

## 4. Consequences

- The built-in types page is written by hand, but it can't drift silently: its final block is checked against the
  compiler.
- Suggestions only append to existing messages. The codes are unchanged, and tools that match on codes are
  unaffected.
- `ceesafe_api` reads the documents embedded in `ceesafec`, so it answers for the compiler that serves it, as
  `ceesafe_spec_search` does.
