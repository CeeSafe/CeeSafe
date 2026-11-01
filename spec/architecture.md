# CeeSafe Compiler Architecture

Status: describes `ceesafec` as it is. Normative semantics: `ownership.md`, `language-spec.md`, `type-system.md`.
`ceesafec` is written in CeeSafe (about 70,600 lines in `src/`) and compiles itself.

## 1. Pipeline

```text
 source files (.cee; the embedded libraries first)
     │  Lexer                 src/syntax/lexer.cee      → tokens, interned symbols
     │  Parser                src/syntax/parser.cee     → AST (spans; recovery at declarations and statements)
     │  Package names         src/check/packages.cee    → names across packages rewritten (ADR 0013)
     ▼
   AST ── Type checker        src/check/                → HIR: every expression typed, names resolved,
     │                                                    generic code checked once (type-system.md §20)
     ▼
   HIR ── NIR builder         src/nir/nir_builder.cee   → NIR: one control-flow graph per function
     ▼
   NIR    src/nir/passes.cee (analyzeProgram), in order:
     │   1. in-place check                  (Box.create built in place, type-system.md §22)
     │   2. move check                      (O2–O5)       src/nir/move_check.cee, move_paths.cee
     │   3. borrow check                    (B1–B5, E*)   src/nir/borrow.cee
     │      (1–3 run on generic versions, not on instances; an instance that does not check is N0222)
     │   4. drop elaboration                (O4)          src/nir/drops.cee      only if no error so far
     │   5. effect check                    (§32)         src/nir/effect_check.cee
     ▼
   C backend  src/backend/c_backend.cee (generateC):
     │   closure-call specialisation      (optimisations.md §5)    src/nir/closure_specialisation.cee
     │   check elimination                (optimisations.md §2–§3) src/backend/check_elimination.cee
     │   placement                        (memory-model.md §5)     src/backend/placement.cee
     │   structuring                      (c-backend.md §3)        src/backend/c_structurer.cee
     │   emission                         (c-backend.md)           src/backend/c_emitter.cee, c_body.cee, …
     ▼
   C files: main.c (or one file per function group with --split, ADR 0022), ceesafe/rt.h and the runtime's .c files
     │  src/host/build.cee, toolchain.cee, compilers.cee
     ▼
   C compiler (clang, gcc, MSVC's cl, or a cross toolchain) → .o → link
```

Safety checking is complete before the C backend runs. No later pass may introduce a borrow, a move or a drop that the
checkers have not seen. The optimisations decide what C is written; they do not change what the checkers accepted
(check elimination and placement record facts the emitter reads; closure-call specialisation adds functions whose
bodies are rewritten copies of checked ones).

## 2. Intermediate representations

### 2.1 AST

Concrete syntax with spans, kept in tables (`src/syntax/ast.cee`) indexed by node ids. Used for diagnostics, the
formatter (`src/formatting/`), `doc` (`src/documentation/`) and the language server's index
(`src/tooling/semantic_index.cee`). `ceesafec parse --dump-ast` prints it (`ast_printer.cee`).

### 2.2 HIR — typed high-level IR

- Every expression carries a resolved type (`src/types/types.cee`); every name its declaration.
- Desugared: `for`, `?`, `using`/`defer`, method calls with their receiver mode, automatic borrows and reborrows,
  `if`/`match` expressions (§15.3).
- `ceesafec check --dump-hir` prints it (`src/hir/hir_printer.cee`).

### 2.3 NIR — the ownership IR

A place-based control-flow graph per function, modelled on Rust's MIR (`intermediate-representation.md`). Every move,
copy, borrow, storage start and end and drop is an explicit statement at a point in the graph, so initialisation and
loan liveness are dataflow over blocks, and places make field-level disjointness (B1) and partial moves (O2) exact.
`ceesafec build --dump-nir` prints it (`src/nir/nir_printer.cee`).

## 3. Tables, not pointers

The compiler's data is held in vectors indexed by 32-bit ids (`src/support/`: the interner, maps, bit sets), not in
graphs of references. That keeps it within the borrow rules of `lifetimes.md` §4 and §8 (a non-constant index aliases
the whole vector), makes every walk ordered, and keeps the output a pure function of the input: no hash-order
iteration in an output path, no timestamps, no absolute host paths.

## 4. Diagnostics

- Codes `N0001…` (`src/diagnostics/catalog.cee`); every code has a row in `diagnostics.md` or another document of this
  specification, which `ceesafec explain N0123` prints (`src/tooling/explain.cee`).
- Borrow errors show the loan, the conflicting access and the later use.
- Recovery: the parser recovers at statement and declaration boundaries; a type error suppresses cascades; the NIR
  checkers run only on programs that type-check.
- `--diagnostics-format=json` for editors and CI; `ceesafec fix` applies the fixes diagnostics carry
  (`src/tooling/fixes.cee`, `tooling.md` §5).

## 5. Tools

The same front end serves the other commands (`tooling.md`): `fmt`, `doc`, `explain`, `fix`, `run --interp` (the NIR
interpreter, `src/interpreter/`), `lsp` and `mcp` (`src/tooling/language_server.cee`, `mcp_server.cee`, which compile
in memory through `src/tooling/compile.cee`), and the package commands (`src/packages/`: `ceesafe.toml`,
`ceesafe.lock`, the registry).

## 6. Repository structure

```text
CeeSafe/
  ceesafe.toml                 # ceesafec is a CeeSafe package
  src/                         # the compiler (the directories named above)
    library/                   # GENERATED: the libraries, runtime and documents ceesafec embeds
  lib/                         # the libraries: core, std, kernel (Decision 0008)
  runtime/                     # the C runtime (Decision 0007)
  spec/                        # this specification; spec/decisions/ holds the ADRs
  guide/                       # the guide; every `ceesafe` example is checked (tools/guide_check.py)
  templates/AGENTS.md          # what `ceesafec new` and `ceesafec agents` write
  editors/                     # the VS Code extension; lldb and gdb scripts
  integrations/cosmoos/        # ceesafec's C held to CosmoOS's build, tests and boots (C1–C9, K3)
  tests/reference/             # CeeSafe's corpus, benchmarks and programs
  tools/                       # generators, the corpus runner, the contract suite
  bootstrap/                   # the stage-0 pin and the fixed-point records
  docs/                        # the port's plans, evidence and decisions
```

The compiler, the runtime C and the libraries are versioned together: the runtime ABI between them is internal, and
`CEESAFE_ABI_VERSION` names it (`c-backend.md` §2). The libraries (`lib/core`, `lib/std`, `lib/kernel`) and the runtime
(`runtime/`) are CeeSafe's sources (Decisions 0007, 0008); `tools/gen_library.py` and `tools/gen_runtime.py` embed
them into `src/library/`. A path `lib/…` or `runtime/…` in this specification names one of these sources.
