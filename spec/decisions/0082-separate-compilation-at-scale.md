# ADR 0082 — Separate compilation at scale: units that never merge, per-unit preludes, parallel and cached builds, per-package check

- Status: **Accepted** by the owner, 2026-11-09 ("Accept all ADRs as recommended": every question for the owner answered as recommended), the day it was proposed. Raised by the Bun port's M0 (`~/dev/CeeSafeApps/bun/ROADMAP.md` §3, M0). Its compiler
  scale test (`~/dev/CeeSafeApps/bun/analysis/scale/README.md`, findings 3 and 4; `scale_test.py`) found that the C step,
  not ceesafec, now bounds a Bun-sized build, and that `--split` does not yet help. The evidence: that README's
  measurements (Mac, 8 cores, Apple clang 21; hanan, Debian 12 x86-64, 12 cores, gcc 12.2); probes compiled on
  2026-11-09 with `~/dev/CeeSafe/out/ceesafec` 0.24.2 built from `fe4d0ca` (Mac), kept in
  `~/dev/CeeSafeApps/bun/analysis/adr-evidence/0082/` (below, `evidence/`); and counts of Bun 1.4.4 (`bun-v1.4.4`,
  `38afee6c84fc`) made by a small script (`evidence/bun_files2.py`, run with `python3 -I`) that groups every `.rs`
  file under the nearest `Cargo.toml`. The generated 100k program was not kept: `analysis/scale/scale_test.py`
  regenerates it.
- Release: stage A ships in 0.24.3, with ADR 0081's diagnostics and the bug of §4. Stages B–F are tooling work, with
  no language change, that can land with any later release; D first.
- Scope: c-backend.md §2.1 (names), §2.4 (split output), §7 (location tables); tooling.md §4.4 (commands), §5.1 (the
  JSON diagnostics), §8.3 (flags); type-system.md §32.1 (effects across packages); the C backend (`c_split.cee`,
  `c_skeleton.cee`, `c_names.cee`, `c_program.cee`, `c_backend.cee`), closure-call specialisation's variant names, the
  checker's package loop, and `src/host/build.cee`, `toolchain.cee`. **Extends** ADR 0022 (it is OD-29's second step,
  which ADR 0022 §1 left for later) and **amends** ADR 0013 D4 (building stays one program, but its C and its checking
  become per package). No language change.

## 1. Context

### 1.1 What M0 measured

| Lines | C | One `main.c`, one core | `--split`, 8 jobs by hand |
|---|---|---|---|
| 304,176 (Mac) | 109.5 MB | 134.9 s, 1.7 GB | 30.9 s emit + 68.9 s cc (199.6 s CPU) |
| 304,176 (hanan, fixed compiler) | same program | 225 s (222 CPU-s) | 12 units, 12 jobs: 292 s wall, 2,353 CPU-s |
| 707,469 (hanan, release 0.24.2) | 255.4 MB | 1,287 s, 7.9 GB; gcc turns off column tracking | not run |

On hanan with gcc, today's split output compiled in parallel is **slower** than one file: each of the 12 units parses
the 54.7 MB `ceesafe_program.h` (§1.2.4), so the CPU time is ten times one file's. The shared header must go. A
Bun-sized port is about 1.7M lines in about 120 packages: one C file of about 540 MB on one core.

### 1.2 Why `--split` does not scale yet

1. **Serial.** `compileExecutable` (`src/host/toolchain.cee:235`) puts every C file on one compiler command line
   (`:291–295`). clang and gcc compile them one after another. The scale test got its 8-way speed-up only by running
   `xargs -P` itself (`scale_test.py:267–286`). Only a package's own C files (ADR 0047 §3.3, `compileSeparate`, `:120`)
   get separate objects, still one at a time.
2. **Units merge.** `nameUnits` (`src/backend/c_split.cee:72–100`) adds directories to a file's stem until names
   differ. A dependency's file path is its path inside its package's sources, with no package, so `alpha/src/util.cee`
   and `beta/src/util.cee` never differ. After depth 32 the loop gives up silently (`:93`), and `unitOf` (`:103`) sends
   both files' functions into one `util.c`. The probe `evidence/p1` (two libraries, each with `util.cee` and
   `lib.cee`) gives `util.c` holding `ceeF_alpha_helper` and `ceeF_beta_helper`, and `lib.c` both `run`s. At 100k lines
   (generated with `scale_test.generate`), 833 source files became **8** units; at 300k, 2,500 became 12. c-backend.md
   §2.4 promises one unit per source file.
3. **The same ambiguity reaches people.** The location table names both files `"util.cee"` (`cee_file_3`, `cee_file_5`
   in the probe's header), so a panic cannot say which package it is in. `check --diagnostics-format=json` reports an
   error in `beta/src/util.cee` as `"file":"util.cee"` too: an agent cannot tell which of 120 packages to open.
4. **One header for everything.** `ceesafe_program.h` holds every type, prototype, `dyn` table, location constant and
   `static inline` per-type helper (c-backend.md §2.4; `c_skeleton.cee:301–302`), and every unit includes it. At 100k
   lines it is 17.7 MB of 36.5 MB of C, with 40,162 lines that start `static inline`. **clang -O2 takes 1.20 s to
   compile a unit that only includes it** (Mac). With one unit per file, as §2.4 intends, that is 833 × 1.2 s ≈ 1,000
   CPU seconds of header alone, against about 37 s of C compile for the whole program as one file (52.0 s build less
   15.0 s emit, release 0.24.2). The cost is units × program
   size: quadratic. At 300k lines the header is 54.7 MB.
5. **Global numbering.** Several C names are numbered across the whole program: closure bodies `ceeCB_<id>` and async
   instances `ceeAS<index>_` (`c_names.cee:470–482`), variants `ceeXS<n>_` from a per-callee counter in worklist order
   (`src/nir/closure_specialisation.cee:53–60`), location constants `ceeL_<n>` and file ids `cee_file_<id>`
   (`c_program.cee:566–582`), long literals `ceeLS_<k>`. A closure added in one package renumbers the C of every
   package after it, so no cache could ever hit. Generic instances and per-type helpers are already named from their
   content (`ceeG_<len><name><mangled arguments>`, `ceeVP_VUu64S10SystemHeap`).

### 1.3 Checking

`ceesafec check` in a package checks it **with all its dependencies, as one program** (ADR 0013 D4). In the 100k
program, checking the leaf `p000` takes 0.05 s, but `p119`, whose lock lists 110 dependencies, takes 1.35 s, more
than `app` (1.12 s). Parsing all 833 files takes 0.14 s. The work is the checking, and every agent pays the whole
program on every check. Bun's own rewrite used `cargo check` per crate as its work queue (ROADMAP.md §1).

### 1.4 Bun's shape (measured; `bun_files2.py`)

- 129 crates with Rust code, 3,248 `.rs` files; median 4 files per crate, the largest `src/runtime` with 470.
- 766 files share their file name with another file: 126 `lib.rs`, 91 `mod.rs`, 38 `error.rs`
  (`src/dotenv/error.rs`, `src/watcher/error.rs`, `src/sql_jsc/error.rs`). 239 clash inside one crate.
- 10,226 `#[inline]` or `#[inline(always)]` attributes, for example `src/collections/hive_array.rs:60`:
  `#[inline] pub(crate) fn is_set(&self, index: usize) -> bool`. 8,340 functions with type or lifetime parameters
  (regex `\bfn\s+\w+\s*<`); `ArrayHashMap<K, V>`
  (`src/collections/array_hash_map.rs:382`) is named in 22 crates.
- Bun's release build makes each crate one ThinLTO unit and runs LTO over all of them at the link, because without it
  there is "NO cross-crate inlining beyond `#[inline]`-annotated leaf fns" (`upstream/Cargo.toml:136–145`).

### 1.5 Which analyses are whole-program today

| Analysis | Where | Scope | Per package? |
|---|---|---|---|
| type, move, borrow checks | `src/check/`, `src/nir/passes.cee` | per function; generic code checked once (type-system.md §20.4) | yes, given the dependencies' declarations |
| effect inference | type-system.md §32.1 | "inferred over the whole package", fixed point | yes, given each dependency function's inferred effects |
| instance checks (N0222) | §20.4 | per instance | in the package that instantiates |
| closure-call specialisation | `closure_specialisation.cee` | per function; variants of callees anywhere | yes, if a variant of another package's function is made in the caller's package |
| check elimination | `check_elimination.cee:458–511` | per function | yes, as is |
| placement | `placement.cee`; memory-model.md §5.1 | per function | yes, as is |
| used library functions | `c_program.cee:337`, `usedLibrary` | reachability over the program | the union of each package's needs |
| generic and async instances | `unitOf`, all into `ceesafe_instances.c` | program | per package (§2.3) |
| `dyn` tables, per-type helpers | `c_dyn.cee:150`, `c_helpers.cee` | program, in the header | per unit that uses them |
| async frames through `dyn I` | `c_async.cee:353`, `dynFrameParts`; type-system.md §40.1 | the union of the frames of every type coerced to `dyn I` anywhere in the program | **no**: a program-level input to each package that awaits through `dyn I` (§2.7) |
| runtime file selection | `c_backend.cee:132–180` | a search of all the C | the union of each package's needs |
| the trial emission | `c_backend.cee:83–123` | the program, twice | per unit |

Nothing whole-program decides *what a program does*: every analysis that does is per function or per package. What is
whole-program is naming, placement of shared code, reachability, and one layout: an async frame through `dyn I`, which
a coercion in any package can grow. That is what makes this ADR possible without a language change, provided the
build tracks that one layout (§2.7).

## 2. Proposal

### 2.1 Units that never merge

- A unit is one CeeSafe source file, at `c/<package>/<path in the package>.c` in the build directory:
  `c/alpha/src/util.c`, `c/beta/src/util.c`. Package names are unique in a build (ADR 0013 D1) and paths are unique
  in a package, so two units never share a path. The embedded libraries are packages `core`, `std`, `kernel`; a build
  of files without a manifest is the package `--program`.
- Program-level files stay at the top: `ceesafe_main.c` (C's `main`, callbacks, `@export` constants), the runtime.
- Two unit paths that differ only in letter case are an error at `build --split`, **N0363** (new), since macOS and
  Windows file systems would merge them.
- The location table, `#line` and panics name a dependency's file by its package: `alpha/src/util.cee`. The JSON
  diagnostics gain `"package"` (tooling.md §5.1 allows new fields within version 1), and `file` is the path from the
  current directory (`../alpha/src/util.cee`), so it can be opened (Question 6).

### 2.2 Per-unit preludes instead of one header

Each unit starts with `#include "ceesafe/rt.h"` and a **prelude** holding exactly what its functions use, transitively:
the types it names and those they contain by value; prototypes of the functions it calls or takes the address of;
the per-type helpers it calls; the `dyn` tables it coerces to; its own location constants and file names. The order
is the program's one deterministic order, filtered. `ceesafe_program.h` goes away in split mode, except for the
`#define`s that rename external symbols with the program's name (ADR 0022 §3.2), which every unit still includes.

- **Per-type helpers stay `static inline`**, copied into each unit that uses one. ADR 0022 §6 measured 1.7–1.9× slower
  `vector-growth` and `string-building` when they left the header. Their names are mangled from their type, so copies
  are identical text and, being internal, never collide.
- A struct defined in several units is the same text in each: compatible types across translation units (C11
  §6.2.7). So type names, generic type instances included (`ceeT_7Grouped1Lusize5_`), stay program-wide content
  names, **never** named per package (§2.3): a `ceeT_` struct passed between two packages' units must have the same tag
  in both.
- The prelude is text inside the unit, not a separate header: one generated input per compiler run, so a unit's hash,
  with the `@header` files it includes, is the key of its object (§2.6).
- **Content names.** Every C name becomes a function of what it names, never of a program-wide counter:
  `ceeCB_<enclosing function's name>_<k>` (the k-th closure written in it), `ceeAS_<original>_<mangled bindings>`,
  `ceeXS_<callee>_<mangled bound bodies>`; `ceeL_<n>`, `cee_file_<n>` and `ceeLS_<k>` numbered within their unit.

### 2.3 Generic instances, variants and library code

- **Instantiated in the using package** (Rust's default). Each package has `c/<package>/ceesafe_instances.c` holding
  the generic and async instances, and the closure variants of other packages' functions, that its own code needs,
  named with that package, length-prefixed like the rest of the name: `ceeG_<len><package>_<len><name><mangled
  arguments>`, the same for `ceeAS_` and `ceeXS_`. The arguments keep today's mangling, value arguments included
  (ADR 0083's `L<type><value>_`): `run<true>` used in `alpha` is `ceeG_5alpha_3run1Lbool1_`, still prefix-free. Two
  packages that both use `ArrayHashMap<K, V>.put` for the same `K`, `V` each have a copy under its own name: no
  duplicate symbols, no weak linkage (MSVC's C has none for functions), and a package's C depends only on its own
  sources and its dependencies' interfaces. Only functions are named per package; types are not (§2.2). The cost is
  code size. The linker's identical-code folding can recover it: `-Wl,--icf=safe` with lld, which folds only
  functions whose address is not taken (from clang's address-significance tables), so function-pointer comparisons
  stay true; `--icf=all` would break them. §5 measures it.
- A variant of a function of the same package goes in that function's unit, as today.
- **Library code** (non-generic functions of `core`, `std`): their own units, holding the union of the functions each
  package reaches. Each package records what it reaches (§2.7), so the union is cheap and deterministic. Runtime
  files are chosen by the union of each unit's needs instead of a search of all the C.
- **Not now:** reusing an instance a dependency already defines (Rust's `share-generics`). It saves code, but moves
  an instance's home whenever a dependency stops using it. It can follow, measured, without changing this layout.

### 2.4 Inlining across units

A call from one unit to a CeeSafe function in another is not inlined without LTO. One `main.c` inlined everything,
so a mechanical Bun port, which leans on 10,226 `#[inline]` hints, would slow down. The sharpest case is small
functions called from other packages, above all ADR 0081 §2.4's safe C wrappers (about 1,000 one-line CeeSafe
functions, each calling an extern in `unsafe`, defined in the package that owns the C library): without LTO every
call to one becomes a real call to a call.
- **`build --lto`**: `-flto=thin` (clang), `-flto=auto` (gcc), `/GL` and `/LTCG` (MSVC), at compile and link. It is
  how Bun builds its release, and it keeps parallel compiles. Off by default; Bun's release builds turn it on.
- No `@inline` attribute here: it is a language change (today N0290) and would get its own ADR if the measurements
  of §5 show split + LTO is not enough.

### 2.5 Parallel compilation: `--jobs N`

`build` compiles each unit to its own object, at most `N` at once (default: the number of CPUs), then links the
objects **in the order of their unit paths**, bytewise. Diagnostics from the C compiler are printed in that order too,
whatever finishes first. `--jobs 1` is today's order. The host already starts processes; the jobs follow
`ceesafec test --jobs` (ADR 0049).

### 2.6 The object cache

- An object's key is a SHA-256 of: the unit's text; ceesafec's banner; the C compiler's path and its `--version`
  output; every flag; the target; and the contents of every header the unit reads besides the runtime's (the `@header`
  files, c-backend.md §2.3, and what they include). As in ccache's direct mode, a miss compiles with the compiler's
  dependency list (`-MD`; MSVC `/showIncludes`) and stores the list with the entry; a lookup hashes the listed files
  again, and any that changed or vanished is a miss. A changed header with the same unit text never reuses an object.
  A hit copies the object; a miss compiles and stores it.
- The cache is content-addressed, under `$CEESAFE_HOME/cache/objects/` (Question 5). An entry is written to a temporary
  name and renamed, so agents that build at once can share it: two writers of one key write the same bytes, and a
  reader never sees half a file. An entry that cannot be read is a miss.
- Each `build` works in its own temporary directory and renames its output into `out/` at the end, so two agents
  building the same package do not overwrite each other's C.
- The cache never changes output: the C is generated in full on every build and is byte-identical; only the C
  compiler's run is skipped.

### 2.7 Per-package checking with interface records

- **An interface record** is what a dependent needs from a package that its sources cannot give cheaply: whether it
  checked without errors; the inferred effects (§32.1) of every function another package can reach (public items, and
  internal ones a public generic body calls); what it reaches in the libraries; the instances and variants it needs
  from others; the coercions to `dyn I` it makes, with each impl's async frame size, and the `dyn I` async methods it
  awaits. Declarations and bodies are **not** serialised: they are parsed again from the package's sources (0.14 s
  for 100k lines), so there is no second format of the language to keep in step.
- Its key is a SHA-256 of the package's sources and manifest, ceesafec's banner, the profile, and its dependencies'
  **interface hashes**. Its own interface hash covers its declarations, effects and coercions to `dyn`, and every
  body a dependent compiles into its own C: generic bodies, the bodies of functions with a closure parameter
  (dependents build `ceeXS_` variants of them, §2.3) and of async functions (dependents build `ceeAS_` instances). A
  change inside any other non-generic body does not invalidate its dependents.
- **Async frames through `dyn I`** are the one layout that depends on the whole program (§1.5): the frame `ceeAF_` of
  `I.m` is the union of the frames of every type coerced to `dyn I` in any package. A build computes these sizes from
  all the records (a cheap union), and a package's C depends on the sizes of the `dyn I` methods it awaits as on its
  dependencies' interface hashes: a coercion added in another package re-emits every package that awaits through that
  `dyn I`, even when no interface record it depends on changed. The frame is in the unit's prelude, so the object
  cache (§2.6) sees the change in the unit's text.
- `ceesafec check` in a package fully checks that package; each dependency comes from its record, checked and
  recorded first if missing. A dependency with errors is one diagnostic, at its line in the manifest. Records live
  beside the objects, with the same atomic writes.
- **`ceesafec check --all`**: every package of the lock, in dependency order, with up to `--jobs` independent ones
  at once; one JSON document whose diagnostics are sorted by package, file and position, with a `"packages"` summary
  (name, errors, warnings). This is the work queue for many agents.
- **Safety stays whole-program.** `build` links only packages whose record says "checked" for this compiler and these
  inputs; any doubt is a recheck. A record is a cache of a result ceesafec proved, never a promise from outside.
  `$CEESAFE_HOME` is per user: ceesafec creates its cache with mode 0700 and ignores (as a miss) any record or object
  not owned by the user or writable by group or others. Sharing is between that user's agents only; a process running
  as the same user could as well replace ceesafec itself.

### 2.8 The staged plan

| Stage | What | Needs | Measured by |
|---|---|---|---|
| A | §2.1 naming and package paths; §2.5 `--jobs` | — | 300k split units = files; C step ≤ the hand-run 69 s |
| B | §2.2 preludes and content names; §2.3 per-package instances | A | header bytes per unit; CPU of a split build ≤ 1.5 × one file |
| C | §2.6 object cache | B | one-body change at 700k: one unit (and maybe its package's instances) recompiled |
| D | §2.7 interface records; `check --all` | A's paths | leaf-package check < 1 s at 1.5M; JSON grouped |
| E | per-package C generation: emit only packages whose record, dependencies' interface hashes or awaited `dyn` frame sizes changed; drop the trial pass per unit | B, D | rebuild after a one-file change at 1.5M < 30 s |
| F | §2.4 `--lto`; then decide `share-generics` and `@inline` | B | `benchmarks/run.py`: split + LTO within 5% of one file |

Stage A ships in 0.24.3. B–F are tooling work, not language, and can land with any later release (0.25.0–0.27.0
carry the other ADRs of this set); D comes first, because the next M0 item, the agents' work queue, needs it, then B
and C. Split output becomes the default for package builds after B (Question 2).

## 3. Alternatives considered

- **One unit per package.** Inlines within a package and has 120 units, not 3,000. But `src/runtime` alone would be a
  unit of about 200,000 lines: one core again, and any change recompiles it. Per file, with LTO for release, is what
  Bun's own build approximates.
- **Fix the names only and keep one header.** Restores 3,000 units, each paying the whole header: 1.2 s per unit at
  100k lines, growing with the program (§1.2.4). It makes the split build slower than one file: on hanan, 12 units
  of the 300k program already take 292 s and 2,353 CPU-s against one file's 225 s (§1.1).
- **Precompiled headers.** clang and gcc each have a format, MSVC another; they are tied to the exact compiler build,
  cannot hold `static inline` copies cheaply across many units, and leave determinism to the C compiler.
- **One home for each instance** (the first package, in a fixed order, that uses it), or **weak symbols.** A home
  moves when a use is added elsewhere, rebuilding unrelated packages; weak functions do not exist in MSVC's C, and a
  stale object with an old weak body would link silently.
- **Helpers out of line, in one unit.** Measured 1.7–1.9× slower (ADR 0022 §6).
- **A serialised interface (HIR or a "header" language).** Faster to load, but a second representation of the
  language for every change to keep exact. Parsing is cheap; checking is what the record saves.
- **Leave caching to `ccache` and the build system.** It would cache objects, but cannot see packages, interfaces or
  effects, so it cannot make `check` per package, and every agent would need it set up.

## 4. Not in this ADR

- `@inline` or any other inlining control in the language (§2.4).
- Sharing instances between packages (`share-generics`, §2.3), and the instance unit split by size.
- One C symbol declared in several packages: ADR 0081.
- Separate compilation for CosmoOS's kernel images: their own build keeps passing `--split` and flags, unchanged.
- The language server's per-package checking: it can use the records once D lands.
- **A bug found while probing, to fix on its own (0.24.3, with stage A):** explicit type arguments on a call through a package are ignored.
  In `evidence/p1`, `alpha.twice<u64>(5, inc)` fails with N0205 "expected 'fn(i32) -> i32'", and
  `let a: u64 = alpha.pick<u64>(1, 2, true);` with "expected 'u64', found 'i32'"; in one file, `twice<u64>(5, inc)`
  checks. Bun writes such calls everywhere (`crate::f::<T>()`).

## 5. Testing

- **Contract tests (proposed):**
  - two packages with the same file names, and one with `a/mod.cee` and `b/mod.cee`: one unit per file, named by
    package path; a case-only clash is N0363;
  - every corpus program built split twice, with `--jobs 1` and `--jobs 8`, cold and warm cache: byte-identical C, and
    the same executable where the linker is deterministic;
  - each unit compiled alone with `-fsyntax-only` and the strict warnings (c-backend.md §1): a prelude that misses
    anything fails here;
  - JSON diagnostics in a dependency carry `"package"` and an openable path; `check --all` against a reviewed file;
  - per-package check against whole-program check: for every package of the corpus and of the scale test, the same
    diagnostics (the whole-program check is the oracle);
  - a stale record (sources changed, compiler banner changed, a dependency's interface changed) is never used; nor
    is a record or object not owned by the user or writable by others;
  - a dependent rebuilt after a change only in a body it compiles itself (a function with a closure parameter, an
    async function): its `ceeXS_` variants and `ceeAS_` instances are regenerated;
  - an `async` call through `dyn I` in package `a`, then a larger impl coerced to `dyn I` in package `b` only: `a` is
    re-emitted with the larger frame, and the program runs under the sanitizers;
  - an `@header` file changed with the same unit text: the object is a miss.
- **Run corpus:** the `clang -O2 split` configuration stays; add `gcc -O2 split --jobs 4` and `clang -O2 split --lto`.
- **Benchmarks:** `benchmarks/run.py` with one file, split, and split + LTO; `vector-growth` and `string-building`
  must stay as fast as one file (ADR 0022 §6). A new one calls, in a hot loop, small functions of another package,
  shaped like ADR 0081 §2.4's safe C wrappers: split without LTO measures what those calls cost, split + LTO must
  match one file.
- **Scale:** `scale_test.py` at 300k, 700k and 1.5M lines, Mac and hanan: build wall and CPU time, peak memory under
  hanan's 12 GB cap, bytes of C per unit, binary size with and without ICF, a one-body rebuild, a leaf `check`.

## 6. Questions for the owner

1. **Unit layout:** one unit per source file at `c/<package>/<path>.c`, never merged, with a case-only clash an
   error? **Proposed: yes.**
2. **Default:** `build` of a package splits by default once stage B lands (`--single-file` keeps one `main.c`;
   single-file builds and CosmoOS's flags unchanged)? This reverses ADR 0022 §3.4's default. **Proposed: yes**: at
   Bun's size one file is 21 minutes on one core.
3. **Instances:** a copy per using package, named with it, rather than one home or weak symbols? **Proposed: yes**;
   sharing from dependencies later, if the binary sizes of §5 ask for it.
4. **`--jobs`:** default the number of CPUs (like `make -j` in a build system, or Cargo), `--jobs 1` serial?
   **Proposed: yes.** Output does not depend on it.
5. **Cache location:** `$CEESAFE_HOME/cache/`, shared by every package and agent on the machine, rather than each
   package's `out/`? **Proposed: `$CEESAFE_HOME`**: entries are content-addressed and safe to share, and a dependency
   checked for one package is then checked for all.
6. **Diagnostic paths:** a dependency's `file` as the path from the current directory, plus `"package"`? It changes
   the text form for dependency files only. **Proposed: yes.**
7. **`--lto`:** opt-in, used by Bun's release builds, not the default? **Proposed: yes**, until §5's benchmarks say
   otherwise.
8. **Order of work:** stages A and D first (the scale test and the agents' queue need them), then B, C, E, F?
   **Proposed: yes.**
