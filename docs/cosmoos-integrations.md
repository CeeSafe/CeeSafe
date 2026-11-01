# The CosmoOS integrations in CeeSafe

`integrations/cosmoos/` holds the reference's CosmoOS checks (C1–C9, K3) at the reference `v0.6.0`, forked by
`tools/fork_integrations.py` (run once) and adapted to `ceesafec`. CosmoOS keeps its own CeeSafe code and builds it with
The reference compiler (Decision 0003); these checks only read its tree and hold ceesafec's C to the same build, tests and boots.

## What was adapted

| Change | Why |
|---|---|
| Programs, harness C and scripts renamed by `tools/names.py`'s forward map (`.cee`, `ceesafe_panic`, `ceesafe_c5/c7/c8` modules, `CEESAFE_FAIL_ALLOC_AT`, …); the proposal patch copied as it is | they meet ceesafec's C names; the patch is a change to CosmoOS's tree |
| The compiler is `$CEESAFEC` or `out/ceesafec`; `require_compiler()` replaces the `dotnet build` of the reference compiler | ceesafec is built beforehand |
| `-DCEE_COSMOOS` for the userland threads runtime | the runtime's macro under CeeSafe's names |
| The corpus, `nwc` and `kernel-library.cee` from `tests/reference/` | CeeSafe's copies |
| C9: CosmoOS's own names restored (`kernel/block/gpt.cee`, `kernel/ceesafe/ceesafe_rt_core.c`, `tests/host/ceesafe_panic.c`, `make cee`); `c6.check_component` compiles a `.cee` copy of a `.cee` port | CosmoOS's files are the reference's; ceesafec reads only `.cee` (Decision 0005) |
| C2: a panic is found anywhere in a serial line | standard error shares the console line with a partial line of output (`run-pass/host-services`, which joined the reference's corpus after C2's last run) |

## c2cee

`c2cee.py`, `c2cee_port.py` and `c2cee/` are the reference's `c2cee` tool (on the reference's main after the fork, `3ba382b`), renamed
(`c2cee` → `c2cee` first, then `tools/names.py`'s forward map). Kept as the reference's: CosmoOS's own names (`make cee`,
`cee-check`, `scripts/cee-gen.py`, `kernel/ceesafe/ceesafe_rt_core.c`, `tests/host/ceesafe_panic.c`) and `verify`'s `--the reference compiler`,
since a port lands in CosmoOS as the reference, built by its pinned the reference compiler. Changed: the keywords come from CeeSafe's lexer
(`src/syntax/token.cee`); a draft's `BUILD.md` first checks the port with ceesafec and copies it into the tree as
`.cee`. Documented in `spec/tooling.md` §9.

Run 2026-10-31: `selftest` passes (the fixture's drafts identical to the reference's snapshots under CeeSafe's names, the
analysis equal as data; its JSON re-blessed for the key order `"ceesafe"` takes); `port` of CosmoOS's
`kernel/scheduler/semaphore.c` and `kernel/ipc/pipe.c` gives `.cee` and floor files byte-identical to `c2cee`'s
mapped forward, `BUILD.md` differing only as intended. `verify` ran on CosmoOS `f17a90f3` (main, #506; 17 CeeSafe ports, the reference compiler 0.6.0 pinned) in a worktree: 14 of 14 steps pass
(`cee-check`; `host-test` with the handles trace `0xfd5772457a0ad4a1` and the sync trace `0xc4ef20be17b35afa`;
`check-docs`; the debug and release builds and `analyze` on x86_64 and aarch64; `fuzz-build`; the debug and release
boots on both arches, `boot-test: PASS`).

`ports.py` is the CeeSafe side: at `f17a90f3`, ceesafec compiles the 16 ports cee-gen.py generates from `.cee` copies,
and all 27 generated files (16 ports' C, 9 exports headers, the runtime's two files) equal the checked-in ones after
mapping CeeSafe's names back, the banner's version aside; the one negative source gives exactly its errors.

## A compiler bug it found

C9 compiles CosmoOS's `gpt.cee`, which has a closure whose body is an `unsafe { e }` expression. ceesafec crashed
(`index out of bounds at src/check/closures.cee:175`): the walker that decides whether a closure captures
(NamesOuterLocal) read `ExprNode.Unsafe`'s operand, an expression, as a statement. When the index existed it scanned
the wrong statement, so a closure's capture decision could silently be wrong. The reference's corpus has no such closure.
Fixed in `src/check/closures.cee`; `tests/check/check-pass/closure-unsafe.cee` keeps it (HIR identical to the reference compiler
0.6.0's; the old ceesafec aborts on it).

## Runs (2026-10-31, macOS arm64, Apple clang 21, QEMU; CosmoOS `9c0d0f84`, its tree unchanged by every run)

| Check | Result |
|---|---|
| C1 x86_64 `--corpus` | the two C1 programs and all 199 run-pass and run-fail programs in the guest; clean shutdown |
| C2 x86_64 | the corpus in the guest (199 programs), and the fault-injection sweep in the guest: 49 allocating programs, 412 runs, each `live=0` or the unwrap panic |
| C3 x86_64 | `nwc` matches BusyBox `wc` (26/26), `c3-io` (arguments, files, loopback TCP, processes), `c3-host` (environment, directories, `File.kind`, `Process.capture`) |
| C4 aarch64, x86_64 | 9/9 each: MODULE_CFLAGS, loader ELF rules, relocations, kernel symbols, no constructors or mutable state, frames ≤ 2,048 bytes, no FP registers, 8/8 negative controls |
| C5 x86_64 | the signed module `ceesafe_c5` booted: five in-kernel tests pass, 0 TestHeap blocks live |
| C6 | lz4 and fbvalid: CosmoOS's host tests and fuzz targets (20,000 runs) with the C original and the CeeSafe port, differential tests (20,000 cases, 0 mismatches), C4's checks |
| C7 x86_64 | 11/11 negative bindings, the host test, C4's checks; the debug build booted with `ceesafe_c7` (stress on 4 CPUs, no lockdep report) and the lockdep inversion control reported |
| C8 x86_64 | 3/3 negative cases, the host test, the module loaded at boot in the debug build (timer and thread), CosmoOS's 522 selftests pass |
| C9 | CosmoOS's `test_gpt` and `fuzz_gpt` with both implementations, the differential test (20,000 cases, 0 mismatches), C4's checks |
| K3 | `lib/kernel` in four programs, flat and split, under both arches' MODULE_CFLAGS |

Not run: aarch64 boots (QEMU aarch64 or the CosmoOS VM, `--ssh`), C8's `--cycles`.
