# P1 probe report

Date: 2026-10-30. Compiler: the reference compiler 0.3.1 (stage-0 pin). The probes live in `probes/` and use the corpus format, so
`tools/run_corpus.py --corpus probes` runs them like the reference's own corpus. This report is what they showed. It
finalises the N-track list (ROADMAP §4) and the patterns the port will use.

## 1. What was run

| Run | Result |
|---|---|
| `run_corpus.py --corpus probes` on macOS arm64, Apple clang 21 | run-pass 8/8, check-fail 3/3, parse-fail 1/1 |
| The same run-pass probes with `-O1 -fsanitize=address,undefined -fno-sanitize-recover=all` on macOS | 8/8 |
| On the Ryzen box (Debian 12 x86-64), GCC 12 | run-pass 8/8, check-fail 3/3, parse-fail 1/1 |
| On the Ryzen box, clang 18 with ASan+UBSan | run-pass 8/8 |
| `lexer` over every tracked `.cee` file in the reference `07d561f` (525 files, 583,844 bytes) | 525/525 lossless, about 120k tokens, 0.01 s on the Mac; also 525/525 on the Ryzen box |
| The same with the lexer built with ASan+UBSan on macOS | identical output, no sanitizer report |

Every expected output was derived by hand before the run. Where a run disagreed with an expectation, the
expectation was rechecked and the cause recorded; none of the disagreements was a compiler fault:

- `id-tables`: I had mis-evaluated the expression (11, not 3) and the depths.
- `lexer`: I had miscounted the punctuation (9 marks, not 10).
- `owning-enums`: the first expectation ignored that `match` by value consumes its scrutinee (type-system §18.4).

## 2. Findings by probe

| Probe | Question | Answer |
|---|---|---|
| `run-pass/id-tables` | Can trees live in tables with typed IDs, with ranges into shared tables, a pass reading one table while writing another, and in-place rewrites? | **Yes**, with no workaround. Enums with `Copy` payloads are `Copy`, so a node is copied out (`ast.node(id)`) before its table changes. |
| `run-pass/owning-enums` | Can vectors hold enums that own heap data, be matched without moving, changed in place, and destroyed in a known order? | **Yes.** An element is matched in place with `match (borrow v[i])`; matching by value consumes (§18.4). Drop order is deterministic: declaration order for fields, index order for vector elements. |
| `check-fail/index-aliasing` | Can one element of a table be read while another is changed, at run-time indexes? | **No**, as expected (N0241, lifetimes.md §4, §8). |
| `run-pass/aliasing-workarounds` | What is accepted instead? | **`splitAtMut`** on a vector of non-`Copy` elements (checked at run time), a copy-out, or separate tables. All three were accepted first time, so N5 (more borrow precision) is **not needed**. |
| `run-pass/generic-map` | Can a generic hash map be written in the reference? | **Yes:** `Map<K: Key<K>, V>` with insertion-ordered entries and an open-addressing index, used with two key types and 1,000 keys. Interfaces can be implemented for the program's own types. `get` must name the borrowed parameter: `-> borrow(this) V?` (N0245). So N2 is library code in CeeSafe, not an upstream need. |
| `run-pass/lexer` | Can a lexer, keyword table, interner and token table run over real source at scale? | **Yes**, on bytes. Source stays `Vector<u8>`, names are interned byte spans, and printing uses `Console.writeBytes`. The keyword table is a `match` on length because arrays cannot hold `StringView`s (N0246). |
| `run-pass/error-layers` | How do I/O, allocation and compiler errors reach one error type without `From`? | Through a generic lifting function per layer (`fromIo(File.open(p))?`), which keeps `?` at the call site. **`From` is not needed.** It exposed a compiler crash (§3). |
| `check-fail/error-mismatch` | Is `?` across error types rejected? | Yes, N0261. |
| `run-pass/host-process` | Can `build` write C, run `cc` with an argument vector, read its status and find its output? | **Yes**, without capturing the compiler's output: its messages go to the inherited stderr. |
| `run-pass/dataflow` | Bitsets and a worklist to a fixed point, as the safety passes need? | **Yes.** Live variables over a CFG with a loop reach the same fixed point in either block order. |
| `parse-fail/generic-impls` | Generic `impl<T>` and `Self`? | **Not parsed** (N0105). They are listed as future work in §21.1. The workaround is one `impl` per instantiation, and `interface Key<T>` bound as `K: Key<K>`. |
| `check-fail/text-gaps` | Which text operations are missing? | Each of these gives N0200 (no such member):<br>• `StringView.fromUtf8`;<br>• `String.appendBytes`;<br>• `clone`;<br>• `parse`;<br>• `find`.<br>Ordering (`<`) gives N0207, and arrays of views give N0246. |

## 3. Upstream defects found

| Defect | Reproducer | Severity |
|---|---|---|
| **Crash.** A generic `match` that rebuilds `Ok(v)` from a `Result<T, E>` crashes the reference compiler when instantiated with `T = void`. The message is "System.InvalidOperationException: cannot lower HirError; the program has errors" (`NirBuilder.LowerRvalueCore`), and no diagnostic is issued. It affects `check`, `build` and `run --interp`, in 0.3.1 and in `07d561f`. | `probes/upstream/void-result-rebuild-crash.cee` (8 lines), the reference #2 | high: a compiler crash on a valid program, in a pattern the port uses. The workaround is non-generic lifts for `void` results. |
| N0245 is reported twice at one position for `-> borrow V?` in a method of a generic struct. It happens only in the full probe; every reduction tried reports it once. | `probes/run-pass/generic-map.cee` with `borrow(this)` removed from `get` | low (cosmetic) |
| When a `match` scrutinee fails to check (N0248, `let` where `var` is needed), the arm bindings are also reported as undefined (N0200), as cascading errors. | the first draft of `lexer.cee`'s `readFile` | low (diagnostic quality) |
| `type-system.md:702` still says "N0258 otherwise", but N0258 was retired in M3c (§18.4). | — | doc nit |
| **Missing drop.** A temporary that needs drop, made in an `if` or `while` condition, is never destroyed. The `while` condition below runs twice but destroys one temporary. `let` and `match` are correct. Native code and `--interp` behave the same, so this is lowering, not the C backend. lifetimes.md: a temporary lives to the end of its statement. The reference's corpus does not cover it: `run_corpus.py --check-leaks` finds no leak in its 199 programs. | `probes/upstream/condition-temporary-not-dropped.cee`, the reference #1 | **high**: destructors and frees are skipped (a resource leak, against the drop semantics the port must reproduce). Found by LeakSanitizer in the P1 gate. Workaround: bind to a local. |
| **Spec and checker disagree.** §23.1 makes `truncate`'s arguments two-phase, like `push`, but `v.truncate(v.length - 1)` is rejected (N0242), while `v.push(…v.length…)` is accepted. | `probes/upstream/truncate-two-phase.cee`, the reference #4 | medium: a valid program is rejected. Workaround: a local. |
| **Imprecision.** `-> borrow(this) T?` is ignored when the other reference parameter has a type-parameter type (`key: borrow K`): a temporary key is then rejected with N0243. With a concrete key type it is accepted. | `probes/upstream/generic-param-borrow-annotation.cee`, the reference #5 | medium: a valid program is rejected. Workaround: a local. |
| **Crash on deep nesting.** 100,000 nested parentheses crash the reference compiler with SIGSEGV (exit 139), with no message. 10,000 are accepted. There is no nesting limit or diagnostic. | `docs/limits.md` §1, the reference #3 | medium (robustness). CeeSafe needs a specified limit, as an N-track spec item. |
| The formatter wraps chained `if` expressions inside `match` arms unreadably, with operands split across lines. | the first `phaseOf` in `src/main.cee` | low (formatter quality) |
| There is no explicit panic or abort for internal-error assertions. | `src/support/bits.cee` | low (a missing feature: an assertion needs a deliberate index fault) |

Status at the the reference compiler 0.4.0 re-pin (2026-10-30): the crash (now N0222), the missing drop, the `truncate` and
`borrow(this)` rejections and the nesting crash (now N0110 at 1,000 levels) are fixed. The four `probes/upstream/`
reproducers became `probes/check-fail/void-result-rebuild.cee`, `probes/run-pass/condition-temporaries.cee`,
`truncate-two-phase.cee` and `generic-param-borrow.cee`. The low-severity rows are unchanged. N1a and N4a shipped in 0.4.0 as well; §4 below is the list as
drafted, and `ROADMAP.md`'s N track has the current status.

## 4. Revised N-track list

Priorities are by what blocks a phase. "Workaround" means the port can proceed without the item, at a stated cost.

| Item | Needed by | Blocking? | Evidence |
|---|---|---|---|
| **N0. Fix the `T = void` crash** | P1 (support library), P3 | yes: a crash, not a missing feature | §3 |
| **N0b. Fix the condition-temporary drop bug**, with a corpus regression and a leak check in the reference's run corpus | P4–P5 (drop parity) | yes: the port must reproduce correct drops, and the oracle drops wrongly | §3 |
| N0c. Fix the `truncate` two-phase disagreement and the `borrow(this)` imprecision | — | no: workarounds exist | §3 |
| N0d. Specify a nesting limit and its diagnostic | P2 | yes, for P2's robustness gate | `docs/limits.md` |
| **N1a. Bytes to text**: `StringView.fromUtf8(Slice<u8>) -> StringView?` (validating) and appending checked bytes to a `String` | P2 diagnostics that quote source; P8 tools | partly. Without it, messages are printed piece by piece and nothing that contains a source name can be built as a `String`, so JSON output, fixes and LSP/MCP replies (P8) cannot be built. | `text-gaps`, `lexer` |
| N1b. `Vector` bulk append from a slice | P2 onward (performance) | no: push in a loop | `lexer`, `error-layers` |
| N1c. Text ordering, `clone`, `find`/`startsWith`, number parsing | — | no: byte-wise functions in CeeSafe's support library | `text-gaps` |
| N1d. Arrays of `StringView` (N0246) | — | no: `match` tables | `lexer` |
| **N4a. Process output capture and stdin** (`CBackend.cs:257-259`); environment (`CC`, `PATH`, `CEESAFE_HOME`, `CEESAFE_INDEX`, `INCLUDE`); directories (create, list); file existence; temporary directories (`Program.cs:614`) | P5 (build), P8 (packages) | yes, for P5 parity | `host-process`, the source audit |
| N4b. Line or length reads from stdin | P8 (LSP, MCP framing) | yes, for P8 | spec §26.3 |
| N2. Library hash map | — | **no longer an upstream need**: written in the reference | `generic-map` |
| N3. `impl<T>`, `Self`, `From` | — | **no**: per-instantiation impls, `K: Key<K>`, lift functions | `generic-impls`, `error-layers` |
| N5. More borrow precision | — | **no** | `aliasing-workarounds` |

So the upstream work before P2 is **N0** (and N0b, N0d) and **N1a**; N4 is needed before P5. The rest is optional
ergonomics.

## 5. Patterns the port adopts

- **Typed IDs into tables.** Nodes are `Copy` enums of IDs and ranges. A node is read by copying it out, then other
  tables are written. There are no references inside tables.
- **Non-`Copy` elements** are matched with `match (borrow v[i])` and changed through `borrow mut v[i]`. Two elements
  of one table are reached at once with `splitAtMut`, or one is copied first.
- **Source text is bytes.** Names are interned byte spans (first-seen IDs), and output goes through
  `Console.writeBytes`. Since 0.4.0 (N1a), `StringView.fromUtf8` turns checked bytes into text where a `String` is needed.
- **Maps** are CeeSafe's own insertion-ordered `Map<K: Key<K>, V>`. Iteration is insertion order, so output never
  depends on hashes.
- **Errors**: one error enum per layer, lifted with small functions. `void` results use non-generic lifts until N0
  is fixed.
- **Tables of names** (keywords, operators) are `match` expressions.
- Probe binaries and their outputs stay out of the repository. Build output goes to `out/` or a temporary directory.

## 6. Measurements of the reference's text handling

These were run on 2026-10-30 against the reference compiler 0.3.1. They fixed `src/support/source.cee`'s rules.

- **Ill-formed UTF-8.** Twelve inputs went in a comment before an error token:
  - valid 2-, 3- and 4-byte sequences;
  - a lone continuation byte;
  - truncated 3- and 4-byte sequences;
  - an overlong form;
  - a surrogate;
  - a code point above U+10FFFF;
  - `FF FE`;
  - a mixed run.

  The reference compiler's reported column equalled Python's `bytes.decode("utf-8", "replace")` counted in UTF-16 code units in all 12
  cases. So .NET replaces each maximal subpart with one U+FFFD, and Python is a valid oracle for it.
- **BOMs.** A file starting with two UTF-8 BOMs reports the same positions as one starting with one: `File.ReadAllText`
  strips the first and `SourceText` the second. A BOM inside a line is N0001.
- **JSON escaping.** File names with special characters showed that `"` and control characters are escaped, with DEL
  as `\u007F` and uppercase hex. Some BMP characters stay raw (é, U+200B); NBSP, U+2028 and U+FEFF are escaped, and
  astral characters are escaped surrogate pairs. The full table (345 BMP ranges) was extracted from .NET 10.0.12
  (`tools/oracles/JsonEscapes.cs`, `tools/oracles/data/json-escaped-bmp.txt`). `\b \f \n \r \t` use short forms,
  and U+FFFD is raw.

## 7. Support library results (P1, 2026-10-30)

| Module | Test | Oracle | Result |
|---|---|---|---|
| `map.cee` | `tests/support/run-pass/map-model.cee`: 20,000 seeded operations | an in-program linear model at every step; summary from `tools/oracles/support_models.py` | match |
| `interner.cee` | `interner-model.cee`: 5,000 seeded interns | in-program list model; Python summary | match |
| `bits.cee`, `bytes.cee` | `bits-model.cee`: 30,000 seeded set/clear, then union, intersection and difference; ordinal order | boolean-vector model; Python | match |
| `source.cee` | `source-positions.cee`: 11 inputs (BOMs, line endings, ill-formed UTF-8, astral) | Python model of SourceText (`gen_source_positions.py`) | match |
| `bigint.cee` | `bigint-cases.cee`: 329 operand pairs (edges and random values up to 400 bits) and 17 wrap/fits values | Python integers (`gen_bigint_cases.py`) | match |
| `json.cee` | `json-writer.cee`: every BMP scalar, every 997th astral one, and ill-formed input (418,506 bytes); two the reference compiler documents | .NET 10.0.12's Utf8JsonWriter (`JsonSweep.cs`), the reference compiler 0.3.1 | byte-identical |
| all | `tests/support/fault/alloc-sweep.cee` through `tools/fault_sweep.py`: each of 573 allocations failed in turn | the program's own consistency check, and the runtime's `live=0` | 573/573; a planted rollback bug was caught |

The modules were run on macOS (Apple clang, with and without ASan+UBSan) and on the Ryzen box (GCC 12, and clang 18
with ASan+UBSan+LeakSanitizer), all with `--check-leaks`.

