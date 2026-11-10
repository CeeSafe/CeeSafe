# ADR 0088 — Embedding CeeSafe in a host program: a library archive, `ceesafe_init`, panic and allocator hooks, host bindings

- Status: **Accepted** by the owner, 2026-11-10 ("Accept as recommended": every question for the owner answered as recommended). Proposed the same day and revised after two adversarial reviews (soundness; fit with Bun).
  Raised by the Bun port's first crate swap (`~/dev/CeeSafeApps/bun/analysis/trials/csrf/REPORT.md`): `bun_csrf` was
  replaced by CeeSafe inside Bun 1.4.4 and Bun's tests passed, but the trial found that CeeSafe cannot yet live in a
  program whose `main` is not CeeSafe's (its gaps G8, G6, G2, G9, and the runtime that is never started). The trial's
  clone is `~/dev/CeeSafeApps/ceebun-trial`, branch `ceebun/csrf-trial` (`ceebun/`, `src/csrf/lib.rs`,
  `scripts/build/ceesafe.ts`). Each claim about ceesafec was checked on 2026-11-10 with ceesafec 0.27.0
  (`~/dev/CeeSafe/out/ceesafec`), Apple clang 21, LLD 21 and rustc 1.99.1 on macOS arm64, by the probes in
  `~/dev/CeeSafeApps/bun/analysis/adr-evidence/0088/` (below, `evidence/`): `bash evidence/run.sh` builds and runs all
  of them, and `evidence/results.txt` is its output. Claims about Bun name the file of Bun 1.4.4 (`bun-v1.4.4`,
  `38afee6c84fc`) they come from; line numbers are those of the trial's clone.
- **Depends on** a runtime bug fix that is not part of this ADR: the runtime's per-process scratch and
  `FailingAllocator` state races between threads (§1.8). It is being fixed on branch `fix/runtime-thread-state`
  (per-thread or atomic state, with an audit of every runtime global). Stage A does not ship before it.
- Release (§2.9): stage A in 0.28.0, after that fix; stage B in 0.28.0 if ready, else 0.29.0; stage C in 0.29.0;
  stage D (separately built libraries) deferred until a host needs it (§6 question 8); stage E with any release that
  has a Windows run.
- Scope: profiles.md §4.2–§4.3 (the runtime, `--emit obj`); tooling.md §4.1, §8.3 and the `build` flags (`--emit lib`,
  `--ar`, and in stage C `--emit rust`); c-backend.md §1.2 (the runtime's version), §2.3 (`ceesafe_exports.h`), §6
  (the runtime surface); type-system.md §28.4, §28.6 and §29.6 (a program without a CeeSafe `main`); memory-model.md
  §3.2 (the report and failure injection, with a host allocator); the runtime headers `ceesafe/rt.h` and
  `ceesafe/foreign.h` (the alignment wording, §2.5) and a new one, `ceesafe/embed.h`; `runtime/core/ceesafe_rt_core.c`,
  `runtime/hosted/ceesafe_rt_hosted.c`, `ceesafe_rt_async.c`, `ceesafe_rt_window.c`; `src/host/build.cee`,
  `toolchain.cee`, `compilers.cee`. **Extends** ADR 0060 and ADR 0061 (foreign allocation) to a host allocator, and
  **amends** type-system.md §28.6's last sentence ("without a CeeSafe `main`, the CeeSafe runtime is not started"). No
  change to the language. G1 (a dependency's `@repr` types make invalid C) is a bug, fixed on its own; it is named
  here only because a host that includes `ceesafe_exports.h` meets it first.

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
  `ceeF_two_common_bump`, so they do not clash either, and the split host prints the same `1 1 2`: the duplication is
  silent in both forms.

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
contracts are internal. `ceesafe_rt_args` keeps the host's `argv` pointer (`ceesafe_rt_io.c`), which a host that
rewrites its arguments for the process title (libuv's `uv_setup_args`) overwrites later.

### 1.4 The allocator is fixed (G6)

- `ceesafe_alloc` counts and fault-injects, then calls `ceesafe_platform_alloc`, a strong symbol in
  `runtime/hosted/ceesafe_rt_hosted.c` that calls `malloc` (likewise `_alloc_zeroed`/`calloc`, `_realloc`, `_free`).
  The runtime's own buffers also call the platform functions directly: 27 sites in `ceesafe_rt_async.c`, 3 in
  `ceesafe_rt_io.c`; these are not counted. The window layer (`ceesafe_rt_window.c`, ADR 0069) calls the C library's
  `malloc`, `calloc`, `realloc` and `free` itself (15 allocation sites), for buffers it frees itself.
- A host that defines `ceesafe_platform_alloc` gets `duplicate symbol '_ceesafe_platform_alloc'`, beside the object
  and beside an archive of it (`evidence/g8/hooks.c`).
- Bun allocates with mimalloc. mimalloc overrides `malloc` only on Linux and only without ASAN (`override = linux &&
  !asan`, `scripts/build/deps/mimalloc.ts:43`), so CeeSafe uses Bun's heap only by accident and only in Linux release
  builds; on macOS and Windows, and in every ASAN build, it uses the C library's (REPORT.md §2). Nothing went wrong in
  the trial because no memory crossed: Rust lent every buffer for the call.

### 1.5 A panic reaches the host without its message (G2)

- Hosted, `ceesafe_panic` and `ceesafe_panic_text` (`ceesafe_rt_core.c`) write `ceesafe panic: <reason> at
  <file>:<line>:<column>` to standard error and call `ceesafe_platform_abort`: `abort()`, or `_exit(134)` on Windows.
- In the C host, a bounds panic on the caller's thread, on a thread CeeSafe spawned (`Thread.spawn`) and inside a
  `@callback` C called each printed that line and ended with status 134 (`evidence/nomain/`).
- **In the trial, Bun printed the line and its crash handler reported nothing** (`evidence/bun/panic_in_bun.log`:
  `ceesafe panic: ceebun trial: deliberate panic at src/exports.cee:82:9`, then nothing). The cause is the build, not
  CeeSafe: the trial ran a debug + ASAN Bun, and Bun installs no signal handlers under ASAN (`reset_on_posix` starts
  `if Environment::ENABLE_ASAN { return; }`, `src/crash_handler/lib.rs:1626`). A release Bun installs them for
  `CRASH_HANDLER_SIGNALS` (`src/bun_core/Global.rs:51`), `SIGABRT` among them, and would report
  `CrashReason::Abort` with a stack. That is read from the code, not run: the trial built no release Bun.
- **What the signal still loses** is the message, the file and the line, which only the printed line has; and on
  Windows `_exit(134)` raises nothing a vectored exception handler sees. Bun already has a C entry point for a foreign
  panic with a message: `Bun__crashHandler(message_ptr, message_len) -> !` (`src/crash_handler/lib.rs:3640`), which
  reports `CrashReason::Panic` with a stack.
- **Three runtime aborts bypass `ceesafe_panic` altogether** (`ceesafe_rt_async.c`): the executor out of memory for
  its bookkeeping (line 381), a task waiting on a descriptor and a wake at once (700), and every task parked with
  nothing to wake it (1587). Hosted, each writes its own line and calls `ceesafe_platform_abort`; freestanding, two of
  them already call `ceesafe_panic(CEE_PANIC_ASYNC, …)`.
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
  (`evidence/weak/`); MSVC's C has no weak functions (ADR 0082 §2.3 and §3 rejected weak symbols for that reason).
- Runtime and generated code compiled with `-fvisibility=hidden` still link statically into one host
  (`evidence/g8/archives.sh`, last line).
- A Rust `extern "C"` function that panics aborts the process (status 134) instead of unwinding into its caller, and
  `catch_unwind` around the call does not catch it (`evidence/rust_host/hook_unwind.rs`).
- `--emit obj` joins objects with the compiler's `-r` or `<linker> -r` (`src/host/toolchain.cee`); MSVC's linker has
  no relocatable link, and tooling.md §8.3 says `--emit obj` needs clang or gcc. ceesafec takes a compiler to be MSVC
  only when its file name is `cl` (`isMsvcName`, `src/host/compilers.cee`). Bun builds Windows with `clang-cl`,
  `llvm-lib` and `lld-link`, chosen by the *target*, and cross-builds Windows from Linux and macOS (`msvcTarget =
  targetOs === "windows"`, `scripts/build/tools.ts:547`); ceesafec has not been tried with any of them.

### 1.8 Runtime state is shared between threads (a runtime bug)

The scratch arena's state (`scratch_chunk`, `scratch_cur`, `scratch_end`, `scratch_depth`) and `FailingAllocator`'s
(`failing_armed`, `failing_left`) are plain process-wide statics in `ceesafe_rt_core.c` (lines 256–257, 452–455). Two
threads that each enter a root scratch frame, in safe code only (`evidence/threads/`):

| Case | Result |
|---|---|
| One CeeSafe program, `Thread.spawn` and the main thread (`spawn.cee`), no embedding | `ceesafe panic: a scratch frame is already active` at the spawned thread's `Scratch.enter()`, status 134 |
| A C host whose two pthreads call one export that enters a frame (`host.c`) | the same panic, status 134 |
| The same host, runtime from `--emit c`, ThreadSanitizer | `data race ceesafe_rt_core.c:458 in ceesafe_scratch_enter_root` |

This is a bug of the runtime with or without a host (type-system.md §38 lets any thread use `Scratch`); it is being
fixed on its own (header). It matters here because a host calls exports from threads CeeSafe never saw: Bun's
thread pool, its HTTP threads, JavaScriptCore's. The allocation counters are already atomic (`CEE_COUNT`,
`__atomic_add_fetch`, relaxed).

### 1.9 The runtime needs 16-byte blocks; `max_align_t` can be 8

`max_align_t` is aligned to 8 on macOS arm64 (measured) and under MSVC, where `rt.h` defines `cee_max_align_t` as
`double`. The runtime needs 16: `cee_task`'s frame is `_Alignas(16)` (`ceesafe_rt_async.c:104`, whose comment says
why), `ceesafe_task_slot` is `_Alignas(16) unsigned char bytes[192]` (`rt.h:1380`), and pool and arena blocks are laid
out assuming it. The C libraries' `malloc` gives 16 on every target CeeSafe supports, so nothing failed so far.
`rt.h:783` ("aligned for any CeeSafe type (max_align_t)") and `foreign.h:12` ("aligned for any C type
(max_align_t)") state the weaker contract.

`evidence/align/`: an allocator that keeps that contract exactly (8-aligned, never 16-aligned blocks, `align8.c`), in
place of the platform functions, under a program with async tasks: UBSan stops at `member access within misaligned
address … for type 'cee_task' … which requires 16 byte alignment`, status 134.

### 1.10 Link-time checks under Bun's link flags

Bun links Linux with `-Wl,--gc-sections` (`scripts/build/flags.ts:1390`) and `--icf=safe`, macOS with `-dead_strip`
(`flags.ts:1206`), and release builds with ThinLTO (§1.11). A link-time check that only an unused reference carries
must survive those. `evidence/elf/` (ELF, cross-built, linked with `ld.lld`) and `evidence/rust_sig/`:

| Reference to a symbol nothing defines | plain | `--gc-sections` | LTO | LTO + `--gc-sections` | ld64 `-dead_strip` (± LTO) |
|---|---|---|---|---|---|
| `__attribute__((used))` static pointer (this ADR's first draft) | fails | **links** | fails | **links** | fails |
| `__attribute__((used, retain))` static pointer | fails | fails | fails | fails | — |
| Rust `#[used]` static (aarch64-linux-android, `no_std`) | fails | fails | — | — | — |
| Rust `#[link_name]` to a signature-named alias, called | fails | fails | — | — | — |

A **marker symbol** defined in two objects is reported as a duplicate only when it is a strong initialised
definition: as a tentative definition (`char m;` with `-fcommon`) or a weak one, ld.lld and ld64 both merge it and the
link succeeds.

### 1.11 The host's compiler flags, and LTO

- `--cflag` reaches the runtime, not only the generated code: with `--emit obj --cflag -fsanitize=address`, the
  runtime's `ceesafe_scratch_enter_root` has 4 ASan shadow loads, against none without it (`evidence/lto/`). An
  umbrella build already compiles the runtime with the host's flags.
- **LTO does not reach CeeSafe through `--emit obj`.** Bun's release builds use ThinLTO (`-flto=thin`, `flags.ts:561`
  and `:572`, Windows included), and its Rust joins it as bitcode (`-Clinker-plugin-lto`, `scripts/build/rust.ts:517`).
  `--emit obj --cflag -flto=thin` yields a native Mach-O object: the `-r` step runs LTO itself, so CeeSafe is
  optimised alone and never inlined into or across Bun's code.
- **An archive of bitcode members does join it:** each C file of `--emit c` compiled with `-flto=thin` is LLVM
  bitcode; archived with `ar` and linked with `clang -flto=thin`, the host prints `alpha_sum=6`.

### 1.12 How CeeBun will use this

- **Now: one umbrella build.** Every Bun crate depends on `bun_core`, so swapped crates share stateful packages and
  must be one CeeSafe build per host binary (§1.2's `1 1 2` is otherwise what Bun would get). The trial already did
  this. The second, separately built CeeSafe library that would need G8's full answer has no user yet.
- **Later: a CeeSafe `main`.** The roadmap ports the core whole-program (M2's "a CeeSafe binary that transpiles and
  bundles", M3's full `bun`), so CeeBun ends with a CeeSafe `main`, Bun's C++ (JavaScriptCore, BoringSSL) as foreign
  libraries, and a CeeSafe crash handler. The embedding interface is for the years between; §4 says what the end state
  needs.

## 2. Proposal

### 2.1 The library archive: `--emit lib` (stage A)

- **What.** `ceesafec build --emit lib -o <name>.a` writes a static archive of the build: one member per C file it
  compiles — the generated code (one member, or one per unit with `--split`) and each runtime file the program needs
  (core, hosted, I/O, threads, async, float, window), with the same flags `--emit obj` uses today, `--cflag` included.
  Member names are unique (`<package>_main.o`, `ceesafe_rt_core.o`, …).
- **Why an archive, not `-r`.** It is the only form that keeps the host's LTO: with `--cflag -flto=thin` (or `-flto`)
  the members are bitcode and the host's link optimises across CeeSafe and its own code (§1.11); a relocatable object
  has already been through LTO. It also exists for every compiler, which `-r` does not (§2.8), and archive semantics
  link only the members something uses.
- **The archiver** is `--ar <tool>`; by default `llvm-ar` beside an LLVM clang, `ar` otherwise (Apple's `ar` indexes
  bitcode members: measured, §1.11), and in stage E `llvm-lib` beside `clang-cl` and `lib` beside `cl`.
- **One archive per host binary.** For CeeBun this is the umbrella build of §1.12: `scripts/build/ceesafe.ts`'s one
  `build --emit obj` edge becomes one `build --emit lib` edge, linked where the object was. Two archives built
  separately still clash exactly as two objects do; that case is stage D's (§2.7).
- **`--emit obj` stays** as it is, for hosts that want one object and no LTO.
- **New usage errors** (status 2, tooling.md §4.1): `--ar` without `--emit lib`; `--emit lib` when no archiver is
  found (naming `--ar`).

### 2.2 `ceesafe_init` and `ceesafe_shutdown` (stage A)

A new runtime header, `ceesafe/embed.h`, written beside `ceesafe/rt.h` by every build that writes headers. It is the
whole embedding interface; the internal `ceesafe_rt_*` functions stay internal.

```c
#define CEESAFE_EMBED_VERSION 1

typedef struct ceesafe_allocator {      /* §2.5, stage B */
    void *(*alloc)(void *context, size_t size);                 /* malloc's contract, aligned as §2.5 says */
    void *(*alloc_zeroed)(void *context, size_t size);          /* calloc(1, size)'s */
    void *(*realloc)(void *context, void *block, size_t size);  /* realloc's; a failure keeps the block */
    void (*free)(void *context, void *block);                   /* free's; never given NULL */
    void *context;
} ceesafe_allocator;

typedef struct ceesafe_panic_info {     /* §2.4 */
    uint32_t kind;                      /* cee_panic_kind */
    const char *reason;                 /* "index out of bounds", as ceesafe_panic_reason gives */
    const uint8_t *message;             /* panic("…")'s text, or a runtime abort's (§2.4); else NULL */
    size_t message_length;
    const char *file;                   /* the CeeSafe source path, zero-terminated; NULL for a runtime abort */
    uint32_t line, column;              /* 1-based; 0 when file is NULL */
} ceesafe_panic_info;

typedef void (*ceesafe_panic_hook)(void *context, const ceesafe_panic_info *info);

typedef struct ceesafe_config {
    uint32_t size;                      /* sizeof(ceesafe_config): fields are only ever appended */
    uint32_t flags;                     /* CEESAFE_INIT_ENVIRONMENT: read CEESAFE_ALLOC_REPORT, CEESAFE_FAIL_ALLOC_AT */
    int32_t argc;                       /* Process.argument's; 0 and NULL: none */
    const char *const *argv;            /* copied by ceesafe_init */
    uint64_t fail_alloc_at;             /* as CEESAFE_FAIL_ALLOC_AT; the environment's wins when read */
    bool alloc_report;                  /* as CEESAFE_ALLOC_REPORT */
    const ceesafe_allocator *allocator; /* NULL: the C library's. Stage A: must be NULL */
    ceesafe_panic_hook panic_hook;      /* NULL: print and abort, as today */
    void *panic_context;
} ceesafe_config;

int32_t ceesafe_init(const ceesafe_config *config);   /* 0, or a CEESAFE_INIT_* error; nothing changed on error */
uint64_t ceesafe_shutdown(void);                      /* prints the report if asked; returns the live blocks */
```

- **When, and the ordering contract.** Once, and `ceesafe_init` must *happen before* every call into CeeSafe on every
  thread: called before the host starts the threads that call CeeSafe, or published to them by the host's own
  synchronisation (a mutex, a thread start, a release store the threads acquire). C's `main` calls it before
  `ceeF_main` for the same reason.
- **The state word.** The runtime keeps one atomic state, `UNSTARTED → STARTING → STARTED`. `ceesafe_init` moves
  `UNSTARTED → STARTING` with a compare-and-swap (any other state: `CEESAFE_INIT_AGAIN`, including a second
  concurrent call), validates and copies the config, and publishes it with a release store of `STARTED`; on an error it
  stores `UNSTARTED` back and changes nothing else. Runtime paths that read the config off the hot path (the panic
  path, `ceesafe_shutdown`) load the state with acquire; the allocation path relies on the ordering contract and reads
  the copied config without a fence.
- **It copies what it is given:** the config fields, and `argv` — the array and its strings, into one block from the
  C library's allocator (or the hook's, in stage B), not counted and never freed — so the host may change or free its
  own `argv` afterwards (a libuv-style host rewrites it for the process title, §1.3).
- **Errors.** `CEESAFE_INIT_AGAIN` (called before, or concurrently); `CEESAFE_INIT_LATE` (the runtime has already
  allocated: counting, and in stage B the allocator, cannot change under live blocks, ADR 0059); `CEESAFE_INIT_SIZE`;
  `CEESAFE_INIT_ALLOCATOR` (a hook with a NULL function; in stage A, any allocator). The default allocator functions
  set a relaxed "allocated" flag, so `LATE` costs nothing once a hook is installed. **`LATE` is best effort:** it sees
  allocations that happened before the call; an allocation racing with `ceesafe_init` on another thread breaks the
  ordering contract and is not detected.
- **`size`.** A `size` smaller than this runtime's `sizeof(ceesafe_config)`, from an older `embed.h`, is accepted if
  it is at least the first version's and ends on a field boundary; the fields it lacks take their defaults. A larger
  `size` (a newer header than the runtime) or any other value is `CEESAFE_INIT_SIZE`.
- **Lifetimes.** The panic hook, its context, and in stage B the allocator's functions and context must stay valid
  until the process exits, not only until `ceesafe_shutdown`: CeeSafe threads (`Thread.spawn`, `Async.runWorkers`'
  workers) may still run after it and while `exit` runs the host's `atexit` handlers and destructors.
- **`ceesafe_shutdown`** prints the report when one was asked for (`ceesafe alloc: …`, memory-model.md §3.2), reading
  the counters with atomic loads, and returns the live count (0 when not counting). It frees nothing and tears nothing
  down: exports stay callable, and the host decides when to call it (Bun: in its exit path, before `exit`). The runtime
  registers no `atexit` handler. Live counts are exact only once the host's threads have stopped calling CeeSafe.
- **A host that never calls `ceesafe_init`** gets exactly 0.27.0's behaviour, measured in §1.3: the C library's
  allocator, the printed panic, no arguments, and the environment variables ignored. Allocation is never refused
  because the runtime was not started. The variables are not read lazily: §3.
- **Failure injection with host threads.** Attempts are numbered process-wide in the order they happen. A sweep
  (`fail_alloc_at = k` for each k, as `tools/fault_sweep.py` drives an executable) is deterministic only while one
  thread at a time allocates through CeeSafe; when host threads allocate concurrently, the k-th attempt is a different
  allocation from run to run. Such a sweep still finds crashes on failure paths, but a failing k does not reproduce
  attempt for attempt.
- **`ceesafec test`** is unchanged: its harness has its own `main` and calls `ceesafe_test_begin`.
- **Generated `main`** calls `ceesafe_init` (with the environment flag and its `argv`), so there is one start path,
  and `ceesafe_rt_start` and `ceesafe_rt_args` go.
- **Freestanding.** `ceesafe/embed.h` exists there too, without the environment flag (there is no environment),
  `argc`/`argv` (no `Process`) or the allocator (no heap; profiles.md §4.1). The panic hook stays the link-time
  `ceesafe_panic` of profiles.md §4.2, unchanged; `panic_hook` is hosted only.

### 2.3 Exports on any host thread (stage A, after the runtime fix)

- **The guarantee.** Exports may be called concurrently from any number of host threads, which need not be threads
  CeeSafe made and need no registration. Per-thread runtime state (scratch frames, `FailingAllocator`'s arming, the
  async executor, the panic flags of §2.4) is `_Thread_local`; process-wide state (the counters, the config, the state
  word) is atomic or written once before any call (§2.2).
- **This is the runtime fix's guarantee** (header, §1.8), stated here as the embedding contract. Stage A does not ship
  until that fix has landed and §5's two-host-thread test passes under ThreadSanitizer.
- **Data the program shares between threads** is the language's business, as for CeeSafe's own threads
  (type-system.md §38): a static is `Mutex`, atomic or immutable; a host that passes the same pointer to two threads at
  once owns that race, as any C caller does.

### 2.4 The panic hook (stage A)

- **Called with** the kind, the reason's text, the message (the program's for `panic("…")`, the runtime's for a
  runtime abort), the file, the line and the column: what the printed line holds today, as data. Bun's hook formats
  them and calls `Bun__crashHandler(ptr, len)` (§1.5), which reports `CrashReason::Panic` with a stack and does not
  return.
- **Runtime aborts go through it.** The three hosted aborts of `ceesafe_rt_async.c` (§1.5) call
  `ceesafe_panic_text(CEE_PANIC_ASYNC, <their message>, …)` instead of writing and aborting, as the freestanding
  branches already call `ceesafe_panic(CEE_PANIC_ASYNC, …)`. `CEE_PANIC_ASYNC` exists (`rt.h:90`), so no kind is added;
  the message tells the three apart, and `file` is NULL. Afterwards `ceesafe_platform_abort` is called only from the
  panic functions, which a contract test checks.
- **It must end the process or never return.** It may call `abort`, `_exit` or a `-> !` crash handler, or block for
  ever (Bun's `sleep_forever_if_another_thread_is_crashing`). If it returns, the runtime prints the usual line and
  aborts.
- **Escaping is undefined behaviour.** `longjmp` out of the hook, a C++ `throw`, an SEH exception, `pthread_exit` or
  `ExitThread`, or a Rust unwind (an `extern "C" fn` hook aborts instead: §1.7) would leave the panicking thread's
  CeeSafe frames abandoned mid-statement: an object the host holds through `Box.intoRaw` half updated, a `Mutex` held
  that the next caller relocks, drops never run. Generated C has no unwind information, and panic = abort stays the
  rule (OD-5).
- **It must not call CeeSafe.** A per-thread **panicked** flag is set before the hook runs. Every export and every
  `@callback` entry checks it first and, if it is set, prints the line and aborts. This turns the two ways a thread
  could re-enter after a panic — the hook calling an export, and a host that escaped the hook anyway and calls in
  again on that thread — into an abort instead of running on broken state. It costs one thread-local load and an
  untaken branch per export call (§5's benchmark). Other threads are not stopped: a lock the panicked thread held stays
  held, which is the escaping host's undefined behaviour, not a CeeSafe state.
- **Re-entry.** A panic on a thread whose panicked flag is already set (the hook itself faulted into CeeSafe) does not
  call the hook again: the printed line and abort.
- **Threads.** The hook runs on the panicking thread: the caller's, a thread CeeSafe spawned (`Thread.spawn`,
  `Thread.both`, `Async.runWorkers`' workers), or a thread C calls a `@callback` on. It is called on every thread that
  panics, possibly at once; serialising concurrent reports is the host's (Bun's crash handler already does, with
  `PANIC_MUTEX` and `sleep_forever_if_another_thread_is_crashing`, `src/crash_handler/lib.rs`). The hook therefore may
  not assume the host's main thread, nor a thread the host made.
- **Before `ceesafe_init`**, or with no hook, the printed line and abort, as today.
- **Windows.** The default abort stays `_exit(134)` (c-backend.md §1.1); with a hook, the host's handler runs first,
  which `_exit` would have skipped.

### 2.5 The allocator hook (stage B)

- **Where it sits.** Under the accounting: `ceesafe_alloc` counts and fault-injects as now (memory-model.md §3.2),
  then calls the hook instead of `ceesafe_platform_alloc`. Failure injection, `FailingAllocator` and the report
  therefore work with any host allocator, and the counters remain CeeSafe's own.
- **What it covers.** Everything the runtime allocates: the counted calls; the 30 direct platform calls of the async
  and I/O layers (still uncounted, as now); and the window layer's 15 direct calls to the C library, which move to the
  platform functions in this stage. The platform functions become the defaults the hook replaces. A C library given
  CeeSafe's allocator through `ceesafe/foreign.h` (ADR 0060) gets the host's allocator too, still counted under
  `CEE_ALLOC_FOREIGN`; `CEE_ALLOC_FOREIGN_INIT` (ADR 0061) is unchanged, neither counted nor failed.
- **The contract** is malloc's, with one change: blocks are aligned to **`max(16, alignof(max_align_t))`**, not to
  `max_align_t` alone (8 on macOS arm64 and MSVC, §1.9). NULL on failure, which CeeSafe turns into `AllocError` as it
  does for `malloc`; `realloc` keeps the block on failure. Thread-safe: any CeeSafe or host thread may call it. The
  same wording replaces `rt.h:783` and `foreign.h:12` (the platform functions' contract, and what a C library gets).
  mimalloc's `mi_malloc`, `mi_zalloc`, `mi_realloc` and `mi_free` give 16 and fit with a one-line adapter each.
- **A debug check.** A runtime built with `-g` checks each block a hook returns and panics, naming the hook, when it
  is not 16-aligned; a release runtime does not check.
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
     allocator) are not in this ADR (§4).

### 2.6 Host bindings (G9)

- **C and C++: the header, fixed (stage A).** `ceesafe_exports.h` gains `#ifdef __cplusplus` / `extern "C" {` … `}`
  around its declarations, so a C++ host can include it (§1.6). A C or C++ host that includes the header gets every
  signature mismatch as a compile error, as measured. No separate C++ output.
- **Rust: `--emit rust` (stage C)** writes `ceesafe_exports.rs` beside the header, from ceesafec's own types, with
  Rust's rules, not the header's:
  - each export in an `unsafe extern "C"` block; `#[repr(C)]` structs for `@repr(C)` structs;
  - **C function pointers** as `Option<unsafe extern "C" fn(…) -> …>`: a CeeSafe `extern "C" fn` value may be null,
    and a bare Rust `fn` type may not (the `Option` has the same ABI, by Rust's null-pointer guarantee);
  - **`@repr` enums as integer newtypes**, `#[repr(transparent)] pub struct Mode(pub u8);` with `impl Mode { pub
    const READ: Mode = Mode(0); … }`, never Rust `enum`s: a C value outside the variants is undefined behaviour in a
    Rust `enum` and an ordinary value in a newtype (CeeSafe itself checks it, `CEE_PANIC_FFI_ENUM`);
  - **atomic fields and statics** as `core::sync::atomic` types (`AtomicU32`, …, same size and alignment as the
    integer), so Rust never reads them non-atomically;
  - only `core` types (`u8`, `usize`, `*const T`, `bool`, `core::ffi::c_char`), so it is `no_std`-friendly. The host
    includes it with `include!` from its build directory. A Rust wrapper giving a safe API (slices for pointer and
    length) stays hand-written: what is safe is a contract the export's `/// Safety:` line states in words.
- **A stale binding fails to link.** Each export also defines `ceesafe_sig_<symbol>_<hash>`, a byte whose name holds a
  hash of the export's C signature **and of every `@repr(C)` layout and `@repr` enum (names and values) reachable from
  it**, so a changed field or variant is a changed hash. The generated Rust references it from a `#[used]` static,
  which survives `--gc-sections` (measured, §1.10). A binding generated before the change names a symbol that no
  longer exists, and the link fails naming the export. A hand-written binding is not checked: the rule for hosts is to
  use the generated one.
- **Why not bindgen on the header.** It would work for Rust, but adds a libclang-based tool to every host's build
  (Bun has none, §1.6), maps `size_t` and `bool` by the host's configuration, cannot know which function pointers may
  be null or that an enum's C value may be out of range, and checks nothing at link time.
- **In scope:** functions, `@repr` types, exported constants and statics (§28.4, §39.6). A configured export (`@cfg`,
  ADR 0043) is declared unconditionally in the Rust file; where its symbol is absent, calling it is a link error.
  **Out of scope:** other host languages, which read the header.

### 2.7 Separately built libraries (stage D, deferred)

Two CeeSafe archives built separately and linked into one image (two teams' crates, a prebuilt CeeSafe library) need
one runtime between them and must not each carry a package with state. CeeBun does not need this (§1.12), so it is
deferred until a host does (§6 question 8). What the reviews settled, so the stage starts from it:

- **`--no-runtime`** (with `--emit lib`, `--emit obj` or `--emit c`): the generated code only, the runtime's functions
  left undefined; and **`--emit runtime -o <archive>`**: the runtime alone, one member per runtime file. Usage errors:
  `--no-runtime` for a package with a CeeSafe `main` or with `--emit exe`; `--emit runtime` given a source file.
- **The version check** cannot use an `__attribute__((used))` reference: ld.lld's `--gc-sections` drops it and the
  link succeeds (§1.10). It uses `__attribute__((used, retain))` (measured to hold under `--gc-sections` and LTO) and
  `#pragma comment(linker, "/include:…")` under MSVC-style drivers; or, sturdier, the runtime's own symbol names carry
  the version, so every real call is the check. The version is the **runtime ABI version** and the profile, not the
  compiler's exact version: an exact version would force every prebuilt library to be rebuilt with each ceesafec
  release, which is the opposite of what the stage is for. That needs a test that diffs the runtime's interface so the
  ABI version is bumped when it changes. Flags that change layouts without ceesafec seeing them (`-D` macros of `rt.h`,
  `-fpack-struct`, another data model) are out of its reach: an image's CeeSafe parts are built with one flag set, the
  host's rule, as for any C library.
- **The package marker** is a strong, initialised, `const` definition (a tentative or weak one is merged silently,
  §1.10), defined for packages with *mutable* statics only (`std`'s immutable tables may be duplicated), and
  referenced, with `retain`, from every unit that holds such a static, so whichever archive member is linked carries
  it. Its name is the package's name and version; two unrelated packages with one name and version clash falsely, a
  link error naming the package that the host resolves by renaming, which is preferred to silent duplication. Shared
  libraries are not covered: each has its own copy, as for any static C library.
- **Visibility.** The runtime and `--no-runtime` code compiled with `-fvisibility=hidden` on clang and gcc, exports
  `visibility("default")` (a static link is unchanged, §1.7), so a host that is a shared library does not publish the
  runtime's names. COFF exports nothing by default.

### 2.8 Windows and MSVC (stage E)

| Choice | clang, gcc (ELF, Mach-O) | MSVC `cl`, `clang-cl` (COFF) |
|---|---|---|
| `--emit obj` (`-r`) | as today | impossible: no relocatable link; a usage error naming `--emit lib`, instead of today's failing `-r` |
| `--emit lib` | `ar`/`llvm-ar` | `lib`/`llvm-lib`; the Windows form of the embedding; with `-flto=thin` (clang-cl), bitcode members, as Bun's Windows link expects |
| Allocator and panic hooks | run-time registration | the same; this is why they are not weak symbols |
| `extern "C"` in the header, `--emit rust` | as proposed | the same (Rust's `x86_64-pc-windows-msvc` uses the same `extern "C"`) |
| Default abort | `abort()` | `_exit(134)`; the hook runs before it |

- **The driver style follows the target, not the host or the file name.** `clang-cl` is recognised as an MSVC-style
  driver (`/c`, `/Fo`, `/std:c11`), with `llvm-lib` as its archiver; a Windows target selects it on a Linux or macOS
  host too, since Bun cross-builds Windows from both (§1.7). Today only a file named `cl` is.
- None of this was run on Windows for this ADR; stage E's tests are, natively and cross-built.

### 2.9 The staged plan

| Stage | What | Closes | Release |
|---|---|---|---|
| A | §2.1 `--emit lib`; §2.2 `ceesafe_init`/`ceesafe_shutdown` (state word, argv copy, report, failure injection), generated `main` through it; §2.3 the thread contract; §2.4 the panic hook, the panicked flag, the async aborts routed; §2.6 the `extern "C"` guards | the initialiser, G2, the C++ part of G9; G8 for one umbrella build per binary | 0.28.0, after `fix/runtime-thread-state` |
| B | §2.5 the allocator hook (the config's `allocator`, NULL-only in stage A, so the struct does not change), the 16-byte contract in `rt.h` and `foreign.h`, the debug check, the window layer's calls | G6 | 0.28.0 if ready, else 0.29.0 |
| C | §2.6 `--emit rust`, signature symbols | G9 for Rust | 0.29.0 |
| D | §2.7 `--no-runtime`, `--emit runtime`, the version check, the package marker, visibility | G8 for separately built libraries | deferred (§6 question 8) |
| E | §2.8 `clang-cl` and `llvm-lib` by target, MSVC archives, the Windows tests | Windows | any release with a Windows run |

**Size, against ADR 0082's stages that touch the same files** (an estimate, not measured). Stage A is about 150 lines
of the build host (`--emit lib`: compile each C file to its own object instead of one `-r` link, find and run the
archiver), about 150 lines of runtime C (`embed.h`, the state word, the argv copy, the hook and flags, the three
aborts), one line per export in the generated code (the panicked check) and the header guards, and its tests. ADR
0082's B (per-unit preludes, per-package instances) and E (per-package C generation) rework the C generator and are
several times larger; its C (the object cache) is a build-host feature of about this size in the same
`toolchain.cee`; its F (`--lto`) overlaps: `--emit lib` with bitcode members is what `--lto` needs for a host link,
so F should reuse it rather than add a second path. Stage A and 0082's C and F all change `toolchain.cee`'s compile
and link steps, so they land one after another, not interleaved. ADR 0082 stage D (the checker) does not touch them.
The G1 fix is independent and should land first: a host that includes `ceesafe_exports.h` needs it.

## 3. Alternatives considered

- **`--emit obj` only, as the trial did.** It works for one umbrella build, and an umbrella build already compiles
  the runtime with the host's `--cflag`s (§1.11). But the `-r` step runs LTO on its own, so CeeSafe never joins the
  host's ThinLTO, and MSVC has no `-r`. Kept as an option; `--emit lib` is the form for hosts.
- **The full library mode now** (`--no-runtime`, `--emit runtime`, version symbol, package marker in stage A, as
  first proposed). It answers a case CeeBun does not have (§1.12), and its link-time checks were weaker than they
  looked (§1.10). Deferred to stage D.
- **One self-contained archive per build**, each with its own runtime, one member per runtime file. It links today
  (§1.2), with no compiler change but `ar`. But which build's runtime runs is decided by link order, a runtime
  member from a newer compiler can sit beside an older one, and the two builds' runtimes have two sets of state if a
  member is pulled from each. Rejected for separately built libraries; for one build per binary it is exactly
  `--emit lib`.
- **An exact-compiler-version symbol** for the runtime check: simplest to keep correct, but it makes every prebuilt
  library stale at each release, which defeats separately built libraries. Rejected for stage D in favour of the ABI
  version with an interface diff (§2.7).
- **Documenting exports as single-threaded** instead of fixing the runtime: a host could serialise calls itself, but
  CeeSafe's own `Thread.spawn` hits the same race (§1.8), so the runtime must be fixed anyway. Rejected.
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
  hosts could no longer choose not to be swept. Rejected; §6 question 2 asks.
- **`__attribute__((constructor))`** to start the runtime: CosmoOS runs no constructors (profiles.md §2.1), order
  against the host's own constructors is unspecified, and argv is not available portably. Rejected.
- **Unwinding a panic into the host** (a Rust panic or C++ exception from CeeSafe frames). Generated C runs no
  cleanup when unwound through, so drops and unlocks would be skipped; freestanding builds have no unwind tables at
  all; and the language's model is panic = abort. Rejected, and escaping the hook is undefined behaviour (§2.4).
- **Keeping the host's `argv`** as `ceesafe_rt_args` does: no copy, but a host that rewrites its argument memory
  changes `Process.argument` under the program. Rejected; the copy is made once.
- **bindgen or cbindgen instead of `--emit rust`.** §2.6.
- **Generating safe Rust wrappers** (slices instead of pointer and length). The safety of a wrapper is the export's
  documented contract, which ceesafec cannot read; generating `unsafe` declarations and leaving wrappers to the host
  is what the trial's shim did. Deferred.

## 4. Not in this ADR

- **The end state: a CeeSafe `main` with a host's hooks** (§1.12). When CeeBun's `main` is CeeSafe's, the generated
  `main` calls `ceesafe_init`, and the program needs a way to fill the config that this ADR gives only to C. The
  direction, for a later ADR: the allocator stays a start-up decision, named in the build (a `ceesafe.toml` entry
  naming a C allocator such as mimalloc, linked as a foreign library and passed by the generated `main`); the panic
  hook becomes something the program installs itself, a std function taking an `extern "C" fn` (a `@callback`, so a
  CeeSafe crash handler can be the hook) that stores into the same slot `ceesafe_init` fills. The C interface of this
  ADR is the layer both lower to, so neither changes it.
- **Shared libraries** (`--emit dylib`, `.so`, `.dll`): stage D's visibility rule prepares for them; `dllexport`,
  sonames and a runtime in its own shared library are a later ADR.
- **Adoption of CeeSafe buffers by the host's allocator** without a copy (§2.5 rule 3): `Vector.intoRaw` and moving a
  block to ADR 0060's `foreign held` line, when a swapped crate returns an owned buffer to Rust.
- **Calling Rust APIs from CeeSafe** (the trial's D1): a limit of crate swapping, not of embedding.
- **A host-provided thread-start hook** (Bun's threads have names, stack sizes and its own pool): CeeSafe's threads
  are its own; a later ADR if a swapped crate spawns.
- **The runtime thread-state fix** (§1.8): a bug, fixed on its own branch; this ADR depends on it.
- **G1** (a dependency's `@repr` types), fixed as a bug; G3, G4, G5, G7, G10, which are language gaps of their own.

## 5. Testing

- **Contract tests (`tools/embed_check.py`, new, in `contract_tests.py`), stage A:**
  - two exports of one package (one with threads, one with async) built with `--emit lib`, linked into a C host: both
    run; and `--emit lib` with `--cflag -flto=thin`: every member is bitcode and a `-flto=thin` host link runs them;
  - **Bun's link flags:** the same archive linked with `-Wl,--gc-sections` and `--icf=safe` (ld.lld) and with
    `-dead_strip` (ld64), with and without ThinLTO: the exports run and the panic hook still fires;
  - **two host threads:** two pthreads calling one export that enters a scratch frame and arms a `FailingAllocator`,
    200,000 calls each, under ThreadSanitizer: no report, every result right (`evidence/threads/` is the failing
    form today);
  - `ceesafe_init` with `fail_alloc_at = k`, for every k up to the attempts of a single-threaded run, against an
    executable built from the same code: the same outcomes; with `CEESAFE_INIT_ENVIRONMENT`, `CEESAFE_ALLOC_REPORT=1`
    prints the report from `ceesafe_shutdown`;
  - `ceesafe_init` after an allocation returns `CEESAFE_INIT_LATE` and changes nothing; called twice, `AGAIN`; two
    threads calling it at once: exactly one gets 0; a larger `size`, `SIZE`; the first version's `size`, accepted;
  - `argv`: the host overwrites and frees its argument strings after `ceesafe_init`; `Process.argument` is unchanged;
  - no `ceesafe_init`: the 0.27.0 table of §1.3, exactly;
  - **the panic hook:** a hook that writes the info to a pipe and calls `abort()`, for a bounds panic on the caller's
    thread, on a `Thread.spawn` thread, on an `Async.runWorkers` worker and inside a `@callback` called from C: the
    parent reads kind, file, line and column, and the status is 134; a hook that returns ends in the printed line and
    134; a hook that calls an export ends in the printed line, once; **two threads panicking at once**, with a hook
    that takes a mutex and records both: both reports arrive whole, one process exit;
  - **the three async aborts** (executor out of memory through `fail_alloc_at`, a task waiting on a descriptor and a
    wake, every task parked) reach the hook with `CEE_PANIC_ASYNC`, their message and `file == NULL`; and a grep of
    the runtime finds `ceesafe_platform_abort` called only from the panic functions;
  - `ceesafe_exports.h` compiled as C++ (`clang++ -std=c++17`) and linked against the export;
  - every corpus program with `@export`s built as a library and linked into a generated C host, at `-O2` and under
    ASan and UBSan.
- **Stage B:** an allocator hook that counts its calls and wraps `malloc`: every CeeSafe allocation and free reaches
  it, under ASan, including the async layer's buffers and a scripted window's (`CEESAFE_WINDOW_SCRIPT`); with
  `fail_alloc_at`, the injected failure never reaches the hook; `evidence/align/`'s 8-aligned hook in a `-g` runtime
  stops with the alignment panic, not a misaligned access.
- **Stage C, a Rust host** (cheap: `rustc` alone, no Cargo; skipped where there is no `rustc`): `--emit rust` included
  by a `main.rs` linked with the archive; a changed export signature, a changed `@repr(C)` field and a changed enum
  value each make the stale binding fail to link, also under `--gc-sections`; an export taking a nullable `extern "C"
  fn` called with `None`; an export returning an enum value outside its variants, read and matched by the Rust host;
  a Rust panic hook calling `std::process::abort` after printing.
- **The freestanding check** (`tools/freestanding_check.py`) gains an embedded build: `embed.h`'s freestanding form
  compiles, and the allowlist of profiles.md §4.4 still holds.
- **Stage D:** two packages with `--emit lib --no-runtime` and one `--emit runtime`, linked into a C host: `nm` shows
  one copy of each runtime function; an object with another ABI version fails to link naming it, also under
  `--gc-sections` and LTO; two builds containing a package with a mutable static fail to link naming
  `ceesafe_package_<name>`, also when the static's unit is an archive member other than the marker's; two containing a
  package with only immutable statics link.
- **Stage E (Windows):** `--emit lib` with `cl` and with `clang-cl`/`llvm-lib`/`lld-link`, natively and cross-built
  from Linux and macOS, a C host, ThinLTO with clang-cl, and the panic hook, in `tools/host_c_check.py`.
- **Benchmark:** `benchmarks/run.py`'s allocation-heavy programs (`vector-growth`, `string-building`) with the
  allocator hook set to the C library's `malloc` against no hook, and a call-heavy export loop with and without the
  panicked check: within noise, or the indirect call or the check is reconsidered.
- **CeeBun:** the trial's build moves to one `--emit lib` archive (ThinLTO in release), its shim includes the
  generated header's declarations (and `--emit rust` once stage C lands), and Bun's crash handler gets the panic hook
  through `Bun__crashHandler`. `csrf.test.ts` and the five other test files of REPORT.md §1 pass unchanged, and a
  deliberate panic produces Bun's crash report with the CeeSafe message, file and line **in both an ASAN debug build
  and a release build** (the trial measured only the first, §1.5).

## 6. Questions for the owner

1. **The embedding form:** one `--emit lib` archive per host binary (bitcode members under `-flto`, so CeeSafe joins
   the host's LTO), with `--emit obj` kept, rather than `--emit obj` only? **Proposed: yes.**
2. **Start:** explicit `ceesafe_init` with an atomic state word, copied `argv` and the happens-before contract of
   §2.2, a host that never calls it getting 0.27.0's behaviour, rather than reading the environment lazily at the
   first allocation? **Proposed: explicit.**
3. **Threads:** exports callable concurrently from any host thread as a runtime guarantee, with stage A held until
   `fix/runtime-thread-state` lands and a two-thread ThreadSanitizer test passes, rather than documenting exports as
   single-threaded? **Proposed: the guarantee.**
4. **Hooks at run time** through `ceesafe_init`'s config, not weak symbols, for the hosted runtime; the freestanding
   `ceesafe_panic` unchanged? **Proposed: yes.**
5. **The panic hook's contract:** it ends the process or never returns, escaping it is undefined behaviour, it must
   not call CeeSafe, and every export checks a per-thread panicked flag on entry (one thread-local load per call);
   the runtime's three async aborts go through it? **Proposed: yes.**
6. **The allocator contract (stage B):** 16-byte alignment (`max(16, alignof(max_align_t))`) in the hook and in
   `rt.h`/`foreign.h`, with a debug check; hooks valid until process exit; each side frees what it allocated, and
   adoption without a copy left for a later ADR? **Proposed: yes.**
7. **Bindings:** the `extern "C"` guards in stage A; `--emit rust` in stage C with `Option` function pointers, integer
   newtype enums, atomic types and a signature hash over reachable layouts kept by a `#[used]` static, rather than
   relying on bindgen? **Proposed: yes**; other host languages use the header.
8. **Separately built libraries** (`--no-runtime`, `--emit runtime`, the version check, the package marker,
   visibility): defer as stage D until a host links two independently built CeeSafe libraries, and then check the
   runtime ABI version (with an interface-diff test) through a retained reference, not the exact compiler version?
   **Proposed: defer**, with that design.
9. **The plan:** A in 0.28.0 after the runtime fix, B with it if ready (else 0.29.0), C in 0.29.0, D deferred, E with
   a Windows run? **Proposed: yes.**
