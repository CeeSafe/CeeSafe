# ADR 0088 — Embedding CeeSafe in a host program: a library build mode, `ceesafe_init`, allocator and panic hooks, host bindings

- Status: **Proposed**, 2026-11-10. Raised by the Bun port's first crate swap (`~/dev/CeeSafeApps/bun/analysis/trials/
  csrf/REPORT.md`): `bun_csrf` was replaced by CeeSafe inside Bun 1.4.4 and Bun's tests passed, but the trial found
  that CeeSafe cannot yet live in a program whose `main` is not CeeSafe's (its gaps G8, G6, G2, G9, and the runtime
  that is never started). The trial's clone is `~/dev/CeeSafeApps/ceebun-trial`, branch `ceebun/csrf-trial`
  (`ceebun/`, `src/csrf/lib.rs`, `scripts/build/ceesafe.ts`). Each claim about ceesafec was checked on 2026-11-10 with
  ceesafec 0.27.0 (`~/dev/CeeSafe/out/ceesafec`), Apple clang 21 and rustc 1.99.1 on macOS arm64, by the probes in
  `~/dev/CeeSafeApps/bun/analysis/adr-evidence/0088/` (below, `evidence/`): `evidence/run.sh` builds and runs all of
  them, and `evidence/results.txt` is its output. Claims about Bun name the file of Bun 1.4.4 (`bun-v1.4.4`,
  `38afee6c84fc`) they come from.
- Release: stages A and B (§2.7) in 0.28.0, with ADR 0082 stage D; stage C in 0.29.0; stage D with any release that
  has a Windows run.
- Scope: profiles.md §4.2–§4.3 (the runtime, `--emit obj`); tooling.md §4.1, §8.3 and the `build` flags
  (`--no-runtime`, `--emit lib`, `--emit runtime`, `--emit rust`, `--ar`); c-backend.md §1.2 (the runtime's version),
  §2.1 (names), §2.3 (`ceesafe_exports.h`), §6 (the runtime surface); type-system.md §28.4, §28.6 and §29.6 (a program
  without a CeeSafe `main`); memory-model.md §3.2 (the report and failure injection, with a host allocator); a new
  runtime header `ceesafe/embed.h`; `runtime/core/ceesafe_rt_core.c` and `runtime/hosted/ceesafe_rt_hosted.c`;
  `src/host/build.cee`, `toolchain.cee`, `compilers.cee`. **Extends** ADR 0060 and ADR 0061 (foreign allocation) to a
  host allocator, and **amends** type-system.md §28.6's last sentence ("without a CeeSafe `main`, the CeeSafe runtime
  is not started"). No change to the language. G1 (a dependency's `@repr` types make invalid C) is a bug, fixed on
  its own; it is named here only because a host that includes `ceesafe_exports.h` meets it first.

## 1. Context

### 1.1 What the trial needed

Bun links one Rust static library, C and C++ objects (JavaScriptCore, BoringSSL, mimalloc, its own bindings) and,
in the trial, one CeeSafe object from `ceesafec build --emit obj` (profiles.md §4.3). The CeeSafe side exports two
functions; a Rust shim declares them in a hand-written `unsafe extern "C"` block (`src/csrf/lib.rs`). That works for
one crate. The trial's gaps, most important first, are about what happens next: a second crate, a crate that hands
memory to Rust, a panic, a test sweep, a wrong declaration.

### 1.2 Two CeeSafe objects do not link (G8)

`evidence/g8/` has two packages, `alpha` (one export that fills a `Vector`) and `beta` (one export that starts a
`Thread` and locks a `Mutex`), each built with `build --emit obj`, and a C host that calls both:

| Measured (`nm -gU`) | `alpha.o` | `beta.o` |
|---|---|---|
| Global symbols | 123 | 130 |
| … named `ceesafe_*` (the runtime) | 122, three of them tentative (`ceesafe_vec_empty`, `ceesafe_test_cancel_at`, `ceesafe_test_reports`) | 129: the same 122 and the 7 of the thread layer |
| … others | `alpha_sum`, the export | `beta_twice`, the export |

- **Linked together:** `ld: 119 duplicate symbols` (the 122 shared ones less the three tentative ones, which the
  linker merges), starting with `_ceesafe_scratch_enter_child`, as in the trial.
- **As two archives with one member each:** the same 119. An archive does not help while the runtime and the
  generated code are one relocatable object.
- **The generated code is not the problem.** Its own functions are `static`: an object's only non-runtime global is
  its export. With `--split`, functions are global but carry the program's name (`_ceeF_alpha_alphaSum`; c-backend.md
  §2.4, "so two CeeSafe programs linked into one image never define the same symbol"). That rule covered generated
  code and missed the runtime.
- **By hand it links today** (`evidence/g8/manual.sh`): each package's `main.c` from `--emit c` compiled alone, and
  beta's runtime files compiled once, link with the host and print `alpha_sum=6 beta_twice=42`. Each `main.c` checks
  `CEESAFE_ABI_VERSION != 1`, but only when it is compiled; nothing checks at link time that the objects and the
  runtime come from one compiler.
- **Archives with one member per runtime file also link** (`evidence/g8/archives.sh`), and show the risk: `ld
  -why_load` took `ceesafe_rt_core.o` and `ceesafe_rt_hosted.o` from `liba.a` and `ceesafe_rt_thread.o` from
  `libb.a`. The runtime the program runs is assembled from two builds, chosen by link order, silently.
- **The freestanding profile has the same clash, smaller:** two freestanding objects give `ld: 6 duplicate symbols`
  (`ceesafe_fba_over`, `ceesafe_panic_reason`, … ; `evidence/freestanding/`).
- **A package in two builds is two packages** (`evidence/shared/`): builds `one` and `two` both depend on `common`,
  whose static counts calls. Linked by hand with one runtime, calls one, two, one print `1 1 2`, not `1 2 3`: each
  build has its own copy of `common` and of its static. Split, `common`'s functions are `ceeF_one_common_bump` and
  `ceeF_two_common_bump`, so they do not clash either; the duplication is silent in both forms.

### 1.3 The runtime is never started (no initialiser)

The generated C `main` is the only caller of the runtime's start (a one-line program's `main.c`, printed by
`evidence/run.sh`):

```c
int main(int argc, char **argv) {
    ceesafe_rt_args((int32_t)argc, (const uint8_t *const *)(void *)argv);
    ceesafe_rt_start();
    return (int)ceesafe_rt_finish(ceeF_main());
}
```

`ceesafe_rt_start` (`runtime/core/ceesafe_rt_core.c`) reads `CEESAFE_FAIL_ALLOC_AT` and `CEESAFE_ALLOC_REPORT` and
decides once whether to count allocations (ADR 0059); `ceesafe_rt_finish` prints the report. A C host
(`evidence/nomain/host.c`) calling a CeeSafe object:

| Case | No CeeSafe `main` (0.27.0) | The host calls `ceesafe_rt_args`, `ceesafe_rt_start` and `ceesafe_rt_finish` itself |
|---|---|---|
| 5 boxes, `CEESAFE_ALLOC_REPORT=1` | 5 made, no report | `ceesafe alloc: attempts=5 live=0` |
| One box leaked (`Box.intoRaw`) | no report | `attempts=1 live=1` |
| 5 boxes, `CEESAFE_FAIL_ALLOC_AT=3` | 5 made: ignored | 2 made: the third attempt failed |
| `Process.argument` | 0 (C has 2) | 3 (C has 3) |

So the machinery works without a CeeSafe `main`; what is missing is an interface. Those three functions are global
only because the runtime is one C file among several; no header offers them to a host, and their names and
contracts are internal.

### 1.4 The allocator is fixed (G6)

- `ceesafe_alloc` counts and fault-injects, then calls `ceesafe_platform_alloc`, a strong symbol in
  `runtime/hosted/ceesafe_rt_hosted.c` that calls `malloc` (likewise `_alloc_zeroed`/`calloc`, `_realloc`, `_free`).
  The runtime's own buffers also call the platform functions directly: 27 sites in `ceesafe_rt_async.c`, 3 in
  `ceesafe_rt_io.c`; these are not counted.
- A host that defines `ceesafe_platform_alloc` gets `duplicate symbol '_ceesafe_platform_alloc'`, beside the object
  and beside an archive of it (`evidence/g8/hooks.c`).
- Bun allocates with mimalloc. On Linux mimalloc overrides `malloc`, so CeeSafe used it by accident; on macOS and
  Windows it does not (REPORT.md §2). Nothing went wrong in the trial because no memory crossed: Rust lent every
  buffer for the call.

### 1.5 A panic bypasses the host (G2)

- Hosted, `ceesafe_panic` and `ceesafe_panic_text` (`ceesafe_rt_core.c`) write `ceesafe panic: <reason> at
  <file>:<line>:<column>` to standard error and call `ceesafe_platform_abort`: `abort()`, or `_exit(134)` on Windows.
- In the C host, a bounds panic on the caller's thread, on a thread CeeSafe spawned (`Thread.spawn`) and inside a
  `@callback` C called each printed that line and ended with status 134 (`evidence/nomain/`).
- In Bun the line was printed and Bun's crash handler reported nothing (the trial's `panic_in_bun.log`). Bun's
  handler takes `SIGABRT` among its signals (`src/bun_core/Global.rs`, `CRASH_HANDLER_SIGNALS`), but a signal carries
  no message, file or line; on Windows `_exit` raises nothing a vectored exception handler sees.
- A host-defined `ceesafe_panic` clashes like the allocator (`evidence/g8/hooks.c`: 2 duplicate symbols).
- **The freestanding profile already has the hook:** there the runtime leaves `ceesafe_panic` undefined and the
  environment defines it (profiles.md §4.2). A C host defining it printed `host hook: index out of bounds at
  src/lib.cee:7:14` (`evidence/freestanding/`). But the profile rejects `Vector`, `Box`, `String` and threads (N0293,
  checked on `evidence/freestanding/vector.cee`), so it is no answer for Bun's crates.

### 1.6 Declarations are unchecked (G9)

- `ceesafe_exports.h` declares each export (`uint64_t alpha_sum(uint8_t const *, size_t);`). A C file that includes
  it and declares the function with its parameters swapped fails: `error: conflicting types for 'alpha_sum'`.
- A Rust host has no such check: the same swap in an `extern` block (`evidence/rust_host/main.rs`, `--cfg wrong`)
  compiles, links, and ends with SIGSEGV (status 139).
- A C++ host cannot include the header: it has no `extern "C"` block, so `alpha_sum` is looked up as a C++ name and
  the link fails (`"alpha_sum(unsigned char const*, unsigned long)"`, `evidence/cxx_host/`).
- Bun 1.4.4's `Cargo.lock` has no `bindgen` crate (only `wasm-bindgen` and `wit-bindgen`): a header-to-Rust generator
  would be a new tool in Bun's build.

### 1.7 Facts the proposal relies on

- `@export` names starting `ceesafe_` are N0292 (`evidence/names/reserved.cee`: `ceesafe_platform_alloc` and
  `ceesafe_init` both refused), so the runtime may add `ceesafe_*` entry points without clashing with a program.
- A weak default overridden by a strong host definition works on macOS from an object and from an archive
  (`evidence/weak/`); MSVC's C has no weak definitions (c-backend.md §1.1 lists what MSVC lacks; ADR 0082 §3 rejected
  weak symbols for the same reason).
- Runtime and generated code compiled with `-fvisibility=hidden` still link statically into one host
  (`evidence/g8/archives.sh`, last line).
- A Rust `extern "C"` function that panics aborts the process (status 134) instead of unwinding into its caller, and
  `catch_unwind` around the call does not catch it (`evidence/rust_host/hook_unwind.rs`).
- `--emit obj` joins objects with the compiler's `-r` or `<linker> -r` (`src/host/toolchain.cee`); MSVC's linker has
  no relocatable link, and tooling.md §8.3 says `--emit obj` needs clang or gcc. ceesafec takes a compiler to be MSVC
  only when its file name is `cl` (`isMsvcName`, `src/host/compilers.cee`); Bun builds Windows with `clang-cl`,
  `llvm-lib` and `lld-link` (`scripts/build/configure.ts`), which ceesafec has not been tried with.

## 2. Proposal

### 2.1 A library build mode

Three build outputs, which compose:

1. **`--no-runtime`** (with `--emit obj`, `--emit lib` or `--emit c`): the generated code only. The runtime's
   functions stay undefined references; `--emit c` writes no `ceesafe_rt_*.c`. Its exports are its only global
   symbols unless `--split` is given, whose names already carry the program's name (c-backend.md §2.4). A package
   with a CeeSafe `main` cannot be built so: there would be two C `main`s (a usage error, status 2).
2. **`--emit lib`**: a static archive (`.a`; `.lib` for MSVC-style drivers) of the build's objects, one member per C
   file (with `--split`, one per unit). The archiver is `--ar <tool>`, default `llvm-ar` beside a clang, `ar` beside
   gcc, `lib` beside `cl`, `llvm-lib` beside `clang-cl`. It is the one output that exists for every compiler, which
   `-r` does not (§2.6).
3. **`--emit runtime -o <archive>`**: the runtime alone, for the profile and flags given (`--profile`, `--cc`,
   `--cflag`, `-g`), as an archive with one member per runtime file and every layer included (core, hosted, I/O,
   threads, async, float). Archive semantics take only the members some object needs, so a host that uses no async
   links no executor (measured: `ceesafe_rt_io.o` was never loaded in `archives.sh`). It needs no package.

**One runtime per image.** Each object built with `--no-runtime` references one symbol that only `--emit runtime`
defines: `ceesafe_runtime_0_28_0` (the compiler's exact version; §6 question 2). An object from another compiler
fails to link with an undefined symbol naming the version it expects, rather than running with a mixed runtime. The
reference is kept alive with an `__attribute__((used))` pointer in each object, and with `#pragma comment(linker,
"/include:ceesafe_runtime_0_28_0")` under MSVC-style drivers. c-backend.md §1.2's compile-time check stays.

**One build per package.** A package that has statics defines a marker, `ceesafe_package_<package>`, in the object
that holds them. A second build containing the same package defines it again, so the link fails naming the package
instead of running with two copies of its state (§1.2's `1 1 2`). A package without statics may be in several builds;
its code is then duplicated, which only costs size.

**Visibility.** The runtime archive and `--no-runtime` objects are compiled with `-fvisibility=hidden` on clang and
gcc; exports are `__attribute__((visibility("default")))`. A static link is unchanged (measured, §1.7); a host that
is itself a shared library (a Node addon, a Python extension) does not publish the runtime's names, and two such
libraries, each with its own runtime, do not interpose on each other. COFF exports nothing by default, so MSVC needs
nothing.

**What a host's build does** (Bun's `scripts/build/ceesafe.ts`, today one `build --emit obj` edge):

```sh
ceesafec build --emit runtime --cc "$CC" $CFLAGS -o ceesafe_rt.a           # once per configuration
ceesafec build --emit lib --no-runtime --cc "$CC" $CFLAGS -o csrf.a          # in ceebun/bun_csrf
ceesafec build --emit lib --no-runtime --cc "$CC" $CFLAGS -o base64.a        # in another root package
# link: … csrf.a base64.a ceesafe_rt.a …
```

**When to use it.** The library mode is for builds that share nothing but the runtime: separately versioned
libraries, a prebuilt CeeSafe library, or two teams' crates. When swapped crates share dependencies with state (in
Bun, every crate depends on `bun_core`), one umbrella build per host binary, a package depending on every swapped
crate, stays the right shape, and the marker of §2.1 is what tells a host which case it is in. The umbrella build
also gets the runtime once with `--no-runtime` and `--emit runtime`, so the host's ASAN, debug and release flags apply
to the runtime as compiled by the host's build.

### 2.2 `ceesafe_init` and `ceesafe_shutdown`

A new runtime header, `ceesafe/embed.h`, written beside `ceesafe/rt.h` by every build that writes headers and by
`--emit runtime`. It is the whole embedding interface; the internal `ceesafe_rt_*` functions stay internal.

```c
#define CEESAFE_EMBED_VERSION 1

typedef struct ceesafe_allocator {      /* §2.3 */
    void *(*alloc)(void *context, size_t size);                 /* malloc's contract */
    void *(*alloc_zeroed)(void *context, size_t size);          /* calloc(1, size)'s */
    void *(*realloc)(void *context, void *block, size_t size);  /* realloc's; a failure keeps the block */
    void (*free)(void *context, void *block);                   /* free's; never given NULL */
    void *context;
} ceesafe_allocator;

typedef struct ceesafe_panic_info {     /* §2.4 */
    uint32_t kind;                      /* cee_panic_kind */
    const char *reason;                 /* "index out of bounds", as ceesafe_panic_reason gives */
    const uint8_t *message;             /* the program's text for panic("…"), else NULL */
    size_t message_length;
    const char *file;                   /* the CeeSafe source path, zero-terminated */
    uint32_t line, column;              /* 1-based */
} ceesafe_panic_info;

typedef void (*ceesafe_panic_hook)(void *context, const ceesafe_panic_info *info);

typedef struct ceesafe_config {
    uint32_t size;                      /* sizeof(ceesafe_config): fields are only ever appended */
    uint32_t flags;                     /* CEESAFE_INIT_ENVIRONMENT: read CEESAFE_ALLOC_REPORT, CEESAFE_FAIL_ALLOC_AT */
    int32_t argc;                       /* Process.argument's; 0 and NULL: none */
    const char *const *argv;
    uint64_t fail_alloc_at;             /* as CEESAFE_FAIL_ALLOC_AT; the environment's wins when read */
    bool alloc_report;                  /* as CEESAFE_ALLOC_REPORT */
    const ceesafe_allocator *allocator; /* NULL: the C library's */
    ceesafe_panic_hook panic_hook;      /* NULL: print and abort, as today */
    void *panic_context;
} ceesafe_config;

int32_t ceesafe_init(const ceesafe_config *config);   /* 0, or a CEESAFE_INIT_* error; nothing changed on error */
uint64_t ceesafe_shutdown(void);                      /* prints the report if asked; returns the live blocks */
```

- **When.** Once, before any thread calls into CeeSafe, as C's `main` calls `ceesafe_rt_start` before `ceeF_main`.
  It copies what it is given (`argv` is kept as given, not copied, as `ceesafe_rt_args` keeps it today, so the host
  keeps it alive).
- **Errors.** `CEESAFE_INIT_AGAIN` (called before), `CEESAFE_INIT_LATE` (the runtime has already allocated: an
  allocator, or counting, cannot change under live blocks, ADR 0059), `CEESAFE_INIT_SIZE` (a `size` this runtime does
  not know), `CEESAFE_INIT_ALLOCATOR` (a hook with a NULL function). The default allocator functions record that an
  allocation happened, so `LATE` costs nothing once a hook is installed.
- **`ceesafe_shutdown`** prints the report when one was asked for (`ceesafe alloc: …`, memory-model.md §3.2), and
  returns the live count (0 when not counting). It frees nothing and tears nothing down: exports stay callable, and the
  host decides when to call it (Bun: in its exit path, before `exit`). The runtime registers no `atexit` handler.
- **A host that never calls `ceesafe_init`** gets exactly 0.27.0's behaviour, measured in §1.3: the C library's
  allocator, the printed panic, no arguments, and the environment variables ignored. Allocation is never refused
  because the runtime was not started. The variables are not read lazily: §3.
- **`ceesafec test`** is unchanged: its harness has its own `main` and calls `ceesafe_test_begin`. A host's own test
  suite (Bun's) gets sweeps through `CEESAFE_INIT_ENVIRONMENT` or `fail_alloc_at`, attempt by attempt, as
  `tools/fault_sweep.py` drives an executable.
- **Generated `main`** calls the same function (with the environment flag), so there is one start path, and
  `ceesafe_rt_start` goes.
- **Freestanding.** `ceesafe/embed.h` exists there too, without the environment flag (there is no environment),
  `argc`/`argv` (no `Process`) or the allocator (no heap; profiles.md §4.1). The panic hook stays the link-time
  `ceesafe_panic` of profiles.md §4.2, unchanged; `panic_hook` is hosted only.

### 2.3 The allocator hook

- **Where it sits.** Under the accounting: `ceesafe_alloc` counts and fault-injects as now (memory-model.md §3.2),
  then calls the hook instead of `ceesafe_platform_alloc`. Failure injection, `FailingAllocator` and the report
  therefore work with any host allocator, and the counters remain CeeSafe's own.
- **What it covers.** Everything the runtime allocates: the counted calls, and the 30 direct platform calls of the
  async and I/O layers (still uncounted, as now). The platform functions become the defaults the hook replaces. A
  C library given CeeSafe's allocator through `ceesafe/foreign.h` (ADR 0060) gets the host's allocator too, still
  counted under `CEE_ALLOC_FOREIGN`; `CEE_ALLOC_FOREIGN_INIT` (ADR 0061) is unchanged, neither counted nor failed.
- **The contract** is malloc's: memory aligned for `max_align_t`; NULL on failure, which CeeSafe turns into
  `AllocError` as it does for `malloc`; `realloc` keeps the block on failure. Whatever the runtime does above the
  platform functions for larger alignments is unchanged. Thread-safe: any CeeSafe thread may call it. mimalloc's
  `mi_malloc`, `mi_zalloc`, `mi_realloc` and `mi_free` fit with a one-line adapter each (the context is unused).
- **Cost.** An indirect call through a table that `ceesafe_init` writes once, where today there is a direct call
  across C files. Not measured; §5's benchmark measures it.
- **Ownership of memory that crosses.** Installing the host's allocator does not make the two sides' blocks
  interchangeable; it makes them come from one heap. The rules:
  1. **Lending is the default.** A pointer passed for the call (the trial's case) owns nothing.
  2. **Each side frees what it allocated, through its own code.** A block CeeSafe hands out (`Box.intoRaw`, §34.2)
     comes back through an export of the package that does `Box.fromRaw` and drops it, so its drop glue runs and the
     report stays right; a Rust wrapper's `Drop` calls that export. A block the host lends for longer is given back
     through a host function CeeSafe receives as an `extern "C" fn` (§29.5). Neither side calls the other's `free`,
     even when both are mimalloc.
  3. **Raw bytes the host adopts without a copy** (a CeeSafe buffer becoming a Rust `Vec<u8>` freed by Rust's
     allocator) are not in this ADR (§4): they need a way to take a block out of CeeSafe's `live` count, which ADR
     0060's `foreign held` line could carry, and a `Vector` that gives up its buffer.

### 2.4 The panic hook

- **Called with** the kind, the reason's text, the program's message for an explicit `panic("…")`, the file, the line
  and the column: what the printed line holds today, as data. Bun's hook passes it to its crash handler's
  `panic_impl` (`src/crash_handler/lib.rs`), which reports and exits.
- **It must not return**, and nothing unwinds. If it returns, the runtime prints the usual line and aborts. A Rust hook
  is an `extern "C" fn`, which aborts if it panics (measured, §1.7); a C++ hook is `noexcept`. CeeSafe's C has no
  unwind information to unwind through, and panic = abort stays the rule (OD-5).
- **Threads.** The hook runs on the panicking thread: the caller's, a thread CeeSafe spawned (`Thread.spawn`,
  `Thread.both`, `Async.runWorkers`' workers), or a thread C calls a `@callback` on. It is called on every thread that
  panics; serialising concurrent reports is the host's (Bun's crash handler already does, with `PANIC_MUTEX` and
  `sleep_forever_if_another_thread_is_crashing`, `src/crash_handler/lib.rs`). The hook therefore may not assume the host's main thread, nor a thread the host made.
- **Re-entry.** A panic on a thread already inside the hook (the hook called CeeSafe, which panicked) does not call it
  again: a `_Thread_local` flag, as the async layer already uses (`ceesafe_rt_async.c`), sends it to the printed line
  and abort.
- **Before `ceesafe_init`**, or with no hook, the printed line and abort, as today.
- **Windows.** The default abort stays `_exit(134)` (c-backend.md §1.1); with a hook, the host's handler runs first,
  which `_exit` would have skipped.

### 2.5 Host bindings (G9)

- **C and C++: the header, fixed.** `ceesafe_exports.h` gains `#ifdef __cplusplus` / `extern "C" {` … `}` around its
  declarations, so a C++ host can include it (§1.6). A C or C++ host that includes the header gets every signature
  mismatch as a compile error, as measured. No separate C++ output.
- **Rust: `--emit rust`** writes `ceesafe_exports.rs` beside the header, from the same table: an `unsafe extern "C"`
  block with each export, `#[repr(C)]` structs for `@repr(C)` structs, `#[repr(u8)]` (…) enums for `@repr` enums, and
  `extern "C" fn` types for C function pointers. It uses only `core` types (`u8`, `usize`, `*const T`, `bool`,
  `core::ffi::c_char`), so it is `no_std`-friendly. The host includes it with `include!` from its build directory.
  A Rust wrapper giving a safe API (slices for pointer and length) stays hand-written: what is safe is a contract the
  export's `/// Safety:` line states in words.
- **A stale binding fails to link.** Each export also defines `ceesafe_sig_<symbol>_<hash>`, a byte whose name holds
  a hash of the export's C signature, and the generated Rust declares and references that symbol. A binding
  generated before a signature change then names a symbol that no longer exists, and the link fails naming the export.
  A hand-written binding is not checked: the rule for hosts is to use the generated one.
- **Why not bindgen on the header.** It would work for Rust, but adds a libclang-based tool to every host's build
  (Bun has none, §1.6), maps `size_t` and `bool` by the host's configuration, and checks nothing at link time.
  ceesafec already knows the exact types.
- **In scope:** functions, `@repr` types, exported constants and statics (§28.4, §39.6). A configured export (`@cfg`,
  ADR 0043) is declared unconditionally in the Rust file; where its symbol is absent, calling it is a link error.
  **Out of scope:** other host languages, which read the header.

### 2.6 Windows and MSVC

| Choice | clang, gcc (ELF, Mach-O) | MSVC `cl`, `clang-cl` (COFF) |
|---|---|---|
| `--emit obj` (`-r`) | as today | impossible: no relocatable link; a usage error naming `--emit lib`, instead of today's failing `-r` |
| `--emit lib`, `--emit runtime` | `ar`/`llvm-ar` | `lib`/`llvm-lib`; this is the Windows form of the library mode |
| `--no-runtime` | as proposed | as proposed |
| Runtime version reference | `__attribute__((used))` pointer | `#pragma comment(linker, "/include:…")` |
| Package marker (§2.1) | a defined data symbol | the same; COFF reports the duplicate (`LNK2005`) |
| Visibility | `-fvisibility=hidden`, exports default | nothing: COFF exports only `dllexport`ed names |
| Allocator and panic hooks | run-time registration | the same; this is why they are not weak symbols |
| `extern "C"` in the header, `--emit rust` | as proposed | the same (Rust's `x86_64-pc-windows-msvc` uses the same `extern "C"`) |
| Default abort | `abort()` | `_exit(134)`; the hook runs before it |

- **`clang-cl`** is recognised as an MSVC-style driver (`/c`, `/Fo`, `/std:c11`), with `llvm-lib` as its archiver,
  since it is Bun's Windows compiler; today only a file named `cl` is.
- None of this was run on Windows for this ADR; stage D's tests are.

### 2.7 The staged plan

| Stage | What | Closes | Release |
|---|---|---|---|
| A | §2.1 `--no-runtime`, `--emit lib`, `--emit runtime`, the version symbol, the package marker, visibility; §2.2 `ceesafe_init`/`ceesafe_shutdown` with argv, report and failure injection; generated `main` through it | G8, the initialiser | 0.28.0, with ADR 0082 stage D |
| B | §2.3 allocator hook, §2.4 panic hook: the `allocator` and `panic_hook` fields of the same config (present but required NULL in stage A, so the struct does not change) | G6, G2 | 0.28.0 if ready, else 0.29.0 |
| C | §2.5 `extern "C"` guards (could go first: one line), `--emit rust`, signature symbols | G9 | 0.29.0; the guards in 0.28.0 |
| D | §2.6 `clang-cl` and `llvm-lib`, MSVC archives, the Windows tests | Windows | any release with a Windows run |

ADR 0082 stage D (interface records, `check --all`) and stage A here touch different parts (the checker; the build
host and the runtime). The G1 fix is independent and should land first: a host that includes `ceesafe_exports.h`
needs it.

## 3. Alternatives considered

- **The umbrella build only** (the trial's workaround): one package depending on every swapped crate, one object.
  It works today and stays recommended where builds share stateful packages (§2.1). As the only way, it forbids two
  independently built CeeSafe libraries in one program, ties every CeeSafe part of a host to one compiler version and
  one build edge, and still compiles the runtime with ceesafec's flags rather than the host's. Rejected as the only
  mode.
- **One self-contained archive per build**, each with its own runtime, one member per runtime file. It links today
  (§1.2), with no compiler change but `ar`. But which build's runtime runs is decided by link order, a runtime
  member from a newer compiler can sit beside an older one, and the two builds' runtimes have two sets of state if a
  member is pulled from each. Rejected; `--emit lib` with `--no-runtime` keeps the archive and drops the copies.
- **`--emit runtime` as one relocatable object** rather than an archive: every layer would be linked, used or not, and
  MSVC cannot make one. Rejected.
- **Weak runtime symbols** (`ceesafe_platform_alloc`, `ceesafe_panic` weak, overridden by the host). They work on ELF
  and Mach-O (measured), are free at run time, and need no API. But MSVC's C has none (COFF's `/alternatename` is an
  undocumented linker directive), a host can override `alloc` and forget `free` and link fine, and a link-time hook has
  no context pointer. Rejected for the hosted runtime; the freestanding `ceesafe_panic` stays a plain extern, as now.
- **Overriding an archive member** (the host defines `ceesafe_platform_alloc`, so the runtime archive's member is
  never pulled): portable and free, but the same half-override hazard, and a member pulled for any other symbol makes
  it a duplicate again. Rejected.
- **Lazy start** (read the environment at the first allocation instead of in `ceesafe_init`). It makes failure
  injection work with no host change, but the decision to count must be made before the first allocation on any
  thread (ADR 0059), so every allocation would pay a once-check with acquire ordering on the hottest runtime path, and
  hosts could no longer choose not to be swept. Rejected; §6 question 3 asks.
- **`__attribute__((constructor))`** to start the runtime: CosmoOS runs no constructors (profiles.md §2.1), order
  against the host's own constructors is unspecified, and argv is not available portably. Rejected.
- **Unwinding a panic into the host** (a Rust panic or C++ exception from CeeSafe frames). Generated C runs no
  cleanup when unwound through, so drops and unlocks would be skipped; freestanding builds have no unwind tables at
  all; and the language's model is panic = abort. Rejected.
- **bindgen or cbindgen instead of `--emit rust`.** §2.5.
- **Generating safe Rust wrappers** (slices instead of pointer and length). The safety of a wrapper is the export's
  documented contract, which ceesafec cannot read; generating `unsafe` declarations and leaving wrappers to the host
  is what the trial's shim did. Deferred.

## 4. Not in this ADR

- **Shared libraries** (`--emit dylib`, `.so`, `.dll`): the visibility rule prepares for them; `dllexport`, sonames
  and a runtime in its own shared library are a later ADR.
- **Adoption of CeeSafe buffers by the host's allocator** without a copy (§2.3 rule 3): `Vector.intoRaw` and moving a
  block to `foreign held`, when a swapped crate returns an owned buffer to Rust.
- **Calling Rust APIs from CeeSafe** (the trial's D1): a limit of crate swapping, not of embedding.
- **A host-provided thread-start hook** (Bun's threads have names, stack sizes and its own pool): CeeSafe's threads
  are its own; a later ADR if a swapped crate spawns.
- **G1** (a dependency's `@repr` types), fixed as a bug; G3, G4, G5, G7, G10, which are language gaps of their own.

## 5. Testing

- **Contract tests (`tools/embed_check.py`, new, in `contract_tests.py`):**
  - two packages built with `--emit lib --no-runtime` (one with threads, one with async) and one `--emit runtime`,
    linked into a C host: both exports run, and `nm` of the host shows one copy of each runtime function;
  - the same with `--emit obj --no-runtime`; and two `--emit obj` objects without `--no-runtime` still fail to link,
    so the old behaviour is pinned;
  - an object from a different compiler version (a renamed version symbol) fails to link naming the symbol;
  - two builds containing a package with a static fail to link naming `ceesafe_package_<name>`; two containing a
    package without statics link;
  - `ceesafe_init` with `fail_alloc_at = k`, for every k up to the attempts of a run, against an executable built
    from the same code: the same outcomes (the sweep of `tools/fault_sweep.py`, without a CeeSafe `main`); with
    `CEESAFE_INIT_ENVIRONMENT`, `CEESAFE_ALLOC_REPORT=1` prints the report from `ceesafe_shutdown`;
  - `ceesafe_init` after an allocation returns `CEESAFE_INIT_LATE` and changes nothing; called twice, `AGAIN`; an
    unknown `size`, `SIZE`;
  - no `ceesafe_init`: the 0.27.0 table of §1.3, exactly;
  - an allocator hook that counts its calls and wraps `malloc`: every CeeSafe allocation and free reaches it, under
    ASan, including the async layer's buffers; with `fail_alloc_at`, the injected failure never reaches the hook;
  - a panic routed to the host: a hook that writes the info to a pipe and calls `abort()`, for a bounds panic on the
    caller's thread, on a `Thread.spawn` thread, on an `Async.runWorkers` worker and inside a `@callback` called from
    C: the parent reads kind, file, line and column, and the status is 134; a hook that returns ends in the printed
    line and 134; a hook that calls a panicking export ends in the printed line, once;
  - `ceesafe_exports.h` compiled as C++ (`clang++ -std=c++17`) and linked against the export;
  - every corpus program with `@export`s built as a library and linked into a generated C host, at `-O2` and under
    ASan and UBSan.
- **A Rust host** (cheap: `rustc` alone, no Cargo; skipped where there is no `rustc`): `--emit rust` included by a
  `main.rs` linked with the archives; a changed export signature makes the stale binding fail to link; a Rust panic
  hook calling `std::process::abort` after printing.
- **The freestanding check** (`tools/freestanding_check.py`) gains `--no-runtime` objects: two linked with one core
  runtime, and the allowlist of profiles.md §4.4 still holds for each.
- **Windows (stage D):** the library mode with `cl` and with `clang-cl`/`llvm-lib`/`lld-link`, a C host, and the panic
  hook, in `tools/host_c_check.py`.
- **Benchmark:** `benchmarks/run.py`'s allocation-heavy programs (`vector-growth`, `string-building`) with the hook
  set to the C library's `malloc` against no hook: within noise, or the indirect call is reconsidered.
- **CeeBun:** the trial's build moves to `--emit runtime` and `--emit lib --no-runtime`, its shim's `extern` block to
  `--emit rust`, and Bun's crash handler gets the panic hook; `csrf.test.ts` and the five other test files of
  REPORT.md §1 pass unchanged, and a deliberate panic produces Bun's crash report.

## 6. Questions for the owner

1. **The library mode:** `--no-runtime`, `--emit lib` and `--emit runtime` (an archive), as in §2.1, rather than the
   umbrella build only or self-contained archives? **Proposed: yes.**
2. **The version check:** objects reference a symbol named for the compiler's exact version, so objects and runtime
   from different ceesafec releases never link, rather than `CEESAFE_ABI_VERSION` (still 1 after many runtime changes,
   since runtime and C were always emitted together)? **Proposed: the exact version**, until the ABI version is kept by
   a test that diffs the runtime's interface.
3. **Start:** explicit `ceesafe_init`, with a host that never calls it getting 0.27.0's behaviour, rather than reading
   the environment lazily at the first allocation? **Proposed: explicit.**
4. **Hooks at run time** through `ceesafe_init`'s config, not weak symbols, for the hosted runtime; the freestanding
   `ceesafe_panic` unchanged? **Proposed: yes.**
5. **Ownership:** each side frees what it allocated through its own code, even with one heap, and adoption without a
   copy left for a later ADR (§2.3)? **Proposed: yes.**
6. **The package marker:** a link error when two builds contain one package with statics (§2.1)? **Proposed: yes**;
   without it the duplication is silent.
7. **Bindings:** `extern "C"` guards in the header, and `--emit rust` with link-checked signature symbols, rather than
   relying on bindgen? **Proposed: yes**; other host languages use the header.
8. **The plan:** stages A and B in 0.28.0 with ADR 0082 stage D, C in 0.29.0 (its header guards in 0.28.0), D with a
   Windows run? **Proposed: yes.**
