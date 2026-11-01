# ADR 0022 — M14 K2a: generated C in several translation units

> **Inherited.** ADR 0022 was accepted before CeeSafe's fork (Decision 0006), when the language had its
> earlier name. In this record, "the reference" is that language and project and "the reference compiler" its compiler:
> read them as CeeSafe and `ceesafec`. Links to documents CeeSafe did not keep are given as text.

- Status: **Accepted** (project owner, 2026-10-29, taking every recommendation in §5). The wording is the agent's.
- Scope: M14 K2a (`milestone-14.md`), the first step of OD-29: the reference compiler stays whole-program, but its C is split so that
  a C compiler can build it in parallel, and rebuild only what changed.

## 1. Context

- **Today** (c-backend §2.1): `ceesafec build --emit c` writes one `main.c` holding every function of the program,
  lib/core's included, each with internal linkage (`static`), plus the runtime files (`ceesafe_rt_core.c`, …) and, with
  `@export`, `ceesafe_exports.h`. Internal linkage is what lets CosmoOS link several separately generated programs (its
  four ports) into one kernel: each has its own copy of the lib/core functions it uses, and the copies never collide.
- **The cost at kernel scale** (M14, measured 2026-10-29, Mac M-series): a synthetic 202,004-line package became one
  1,076,433-line `main.c`. The reference compiler took 13.1 s, and `clang -O2 -c` took 33.8 s and 1.25 GB on one core. A kernel
  written in the reference would pay both on every change. CosmoOS's C kernel compiles each file separately, in parallel, and
  only what changed.
- **What does not change here.** Checking stays whole-program (ADR 0013 D4), and so does the reference compiler's own time. Separate
  compilation of CeeSafe packages is OD-29's second step, decided later with K2a's measurements.

## 2. Options

| | A. Split by source file (proposed) | B. Split into N equal parts | C. One file, as today |
|---|---|---|---|
| Unit of rebuild | the CeeSafe file that changed, if no other unit's declarations changed | any change can move functions between parts | everything |
| Determinism | each function's unit is a function of its source file | needs a stable partition | — |
| Readable | `kernel/net/tcp.cee` → `tcp.c` | no | — |

## 3. Proposed decision: A, behind a switch

### 3.1 The layout

`ceesafec build --emit c --split -o DIR` writes:

- `ceesafe_program.h`, the program's declarations: every type, every function prototype, the runtime's header, and the
  `@export` prototypes (the content of `ceesafe_exports.h`, which is still written as before);
- one `<name>.c` per CeeSafe source file, holding the functions declared in it, the closures written in them, and their
  string literals and source locations. Each includes `ceesafe_program.h`;
- `ceesafe_instances.c`, holding the instantiations of generic code, lib/core's functions and the drop glue: everything
  that belongs to no single user file. One file, so that two units never define the same instance;
- the runtime files, as today.

A unit's name is its source file's name, with the shortest distinguishing directory prefix when two files share a
name (`net_tcp.c`, `vfs_tcp.c`), so the layout is a pure function of the package's file list.

### 3.2 Linkage and names

- A function called from another unit has external linkage. Every other function stays `static`, as today.
- **External names carry the program's prefix**, so two programs linked into one image cannot collide: `ceeF_` becomes
  `ceeF_<program>_`. The program's name is the package's (`ceesafe.toml`), or `--program NAME` for a file build. The
  prefix also applies to the instances in `ceesafe_instances.c`, which every program has a copy of.
- Small helpers that the emitter generates per use (`ceeVP_`, `ceeVA_`, the check helpers) become `static inline` in
  `ceesafe_program.h`, so every unit can inline them. A call across units to a CeeSafe function cannot be inlined without
  link-time optimisation. §4 says what that may cost.
- `@export` names are unchanged: they are the C names the user chose.

### 3.3 Determinism

The output stays a pure function of the inputs: the units, their order, each function's placement and the prefix are
derived from the sources and the switch, never from hash order or time. A test builds each `emit-c` corpus program
with `--split` twice and compares, and builds the result in every C configuration.

### 3.4 Without `--split`

Nothing changes: one `main.c`, internal linkage, today's names. CosmoOS's four ports, and every snapshot, stay
byte-identical. CosmoOS opts in per program when its build moves to the reference (its roadmap 2.0's SA1).

## 4. Consequences

- **Parallel and incremental C builds.** The acceptance measures them on the 202,004-line program: `make -j` over
  the units, and a rebuild after a one-file change. A change to a function's signature changes `ceesafe_program.h`, which
  every unit includes, so it rebuilds them all. A body-only change rebuilds one unit, plus `ceesafe_instances.c` if it
  changes an instance.
- **Performance.** Calls across units are no longer inlinable at `-O2` without LTO. The ports' benchmarks
  (`benchmarks/lz4`, `fbvalid`, `gpt`) are run with and without `--split` to measure it. Kernel code built with LTO
  regains it.
- **Tools.** The debugger helpers (`editors/debug/ceesafe_names.py`) learn the prefix. Source maps (`#line`) are
  unchanged, because each unit's `#line` directives name its own CeeSafe file.
- **Not decided here:** separate checking of packages, the rest of OD-29.

## 5. Questions for the owner

1. Split by source file, behind `--split`, with one-file output unchanged by default? (Recommended.)
2. External names prefixed with the program's name (`ceeF_<program>_`)? (Recommended: the alternative, hidden
   visibility, does not prevent duplicate symbols in a static kernel link.)
3. Generic instances and lib/core in one `ceesafe_instances.c`? (Recommended; the alternative is one unit per instance.)

## 6. Implementation and results (2026-10-29)

- `ceesafec build --split [--program NAME]` (`CEmitOptions.Split`, `CEmitter.EmitSplit`), specified in c-backend §2.4.
  The run corpus builds every program split as a C configuration of its own (`clang -O2 split`), with the strict
  warnings. The debugger helpers read the renamed functions when `CEESAFE_PROGRAM` is set (tooling §3.3).
- **As §3.2 says, the per-type helpers belong in the header.** A first version put them in `ceesafe_instances.c` with
  external linkage. Every vector push and drop then became a call across units, and `vector-growth` and
  `string-building` ran 1.7–1.9× slower. In the header, `static inline`, both run as fast as one file.
- **Measurements** (Mac M-series, Apple clang 21, `-O2`; the synthetic program of `milestone-14.md` spread over 21
  CeeSafe files): the reference compiler 14.2 s. The C:

  | Build | Wall | CPU |
  |---|---|---|
  | one file, as before | 33.8 s | 33.8 s |
  | `--split`, `make -j10` | 14.0 s | 80.3 s |
  | `--split`, one unit changed | 2.4 s | 2.3 s |

  The program printed the same value either way. CPU time grows because every unit parses the 99,484-line header. A
  smaller header per unit, holding only what that unit uses, would cut it, and is left for when kernel builds show the
  need.
- Two `--split` builds of the same sources are byte-identical.

