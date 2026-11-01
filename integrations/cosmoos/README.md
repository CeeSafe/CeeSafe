# CosmoOS integration

The CosmoOS checks (milestones C1–C9 and K3) hold ceesafec's C to what CosmoOS demands of kernel and userland code,
inside CosmoOS's own build, tests and QEMU boots. Since 2026-10-31 CosmoOS's kernel code is CeeSafe, built by its
pinned ceesafec (its `scripts/cee-gen.py`): these checks only read its tree.

Everything runs **outside** the CosmoOS tree. The checkout is only read; its Make writes into CeeSafe's `out/cosmoos/`
through `OUT=`, and each script checks that CosmoOS's `git status` is unchanged.

The compiler under test is `$CEESAFEC`, or the tree's `out/ceesafec` (build it first: `tools/bootstrap_release.sh`, or
`out/ceesafec build`). These scripts do not build the compiler.

| File | Purpose |
|---|---|
| `c1.py` | C1: builds CosmoOS's release image, compiles CeeSafe programs with CosmoOS's own `USER_CFLAGS`/`USER_LDFLAGS`, links them against its `crt0.o` and `libc.a`, adds them to a copy of the boot archive, boots QEMU and runs them at the shell. `--corpus` also runs the whole run-pass and run-fail corpus (`tests/reference/tests/corpus`) in the guest |
| `c1/ceesafe-hello.cee`, `c1/ceesafe-panic.cee` | The C1 programs: a pass marker, and a panic that reports its location and aborts under CosmoOS's libc |
| `c2.py` | C2: the fault-injection sweep in the guest (each allocation failing in turn, `CEESAFE_FAIL_ALLOC_AT`), in QEMU or over ssh |
| `c3.py`, `c3/` | C3: files, sockets and processes in the guest |
| `c4.py` | C4: a freestanding object (`c4/kdemo.cee`) compiled with CosmoOS's own `MODULE_CFLAGS`, joined with its `ld.lld` and `MODULE_LDFLAGS`, and checked against the module loader's ELF, relocation and symbol rules, the frame limit and `scripts/check-fpregs.sh`; eight negative controls. No QEMU |
| `c4/ceesafe_rt_kernel.c` | The kernel's `ceesafe_panic`: a CeeSafe panic is a kernel `panic()` |
| `c5.py`, `c5/` | C5: a signed, out-of-tree kernel module (`ceesafe_c5`) with `KernelHeap` (`c5/kernel.cee`) and an in-kernel selftest, booted in QEMU |
| `c6.py`, `c6/` | C6: CosmoOS's LZ4 codec and framebuffer validator as CeeSafe ports, held to CosmoOS's host tests and fuzz targets with both implementations, a differential test, and C4's object checks. No QEMU |
| `c7.py`, `c7/` | C7: the kernel concurrency bindings (`c7/sync.cee`, `kobject.cee`, `quiesce.cee`) against `c7/negative/*.cee`, a host test over CosmoOS's spinlock shim and object code, C4's checks; unless `--no-boot`, CosmoOS's debug build booted twice with the module `ceesafe_c7` (the stress, and the lockdep inversion control) |
| `c8.py`, `c8/` | C8: a kernel module written in CeeSafe (`c8/module.cee`): negative cases, a host test, the module loaded at boot in the debug build, and with `--cycles N` load/unload cycles through CosmoOS's module-cycle hook |
| `c9.py`, `c9/` | C9: CosmoOS's GPT parser and writer (`kernel/block/gpt.cee`): ceesafec compiles a `.cee` copy, held to CosmoOS's `test_gpt` and `fuzz_gpt`, a differential test against the C original, and C4's checks. No QEMU |
| `k3.py`, `k3/` | K3: `lib/kernel` (`--kernel cosmoos`) compiled against CosmoOS's own headers with its kernel flags. Compile only |
| `c2cee.py`, `c2cee_port.py`, `c2cee/` | Porting CosmoOS's C: `analyse` a C file's port needs, `port` drafts its `.cee` file, floor and build lines out of the tree, `verify` runs CosmoOS's own checks of a landed port (with its pinned ceesafec: `make cee-check` and the rest), `selftest` checks the tool against `c2cee/fixture.c` (`spec/tooling.md` §9) |
| `ports.py` | CosmoOS's CeeSafe ports through a ceesafec: its own `scripts/cee-gen.py`, at a commit read with `git archive`, compiles every port with the ceesafec under test and checks the negative sources' errors; with `--pinned <ceesafec>`, every generated file must equal that compiler's output but for the banner's version (the regression check before a release CosmoOS would move to). Refuses a tree from before CosmoOS's move to CeeSafe |
| `proposals/` | Changes proposed to CosmoOS (`module-cycle/`, merged into CosmoOS as #457); kept as the record |

Requirements: a CosmoOS checkout that builds on this host (`gmake check-tools` there), QEMU with UEFI firmware for the
boots, and Python 3.

```sh
python3 integrations/cosmoos/c4.py --arch aarch64
python3 integrations/cosmoos/c6.py
python3 integrations/cosmoos/c7.py --no-boot
python3 integrations/cosmoos/c1.py --arch x86_64 --corpus
```

Outputs go to `out/cosmoos/`: the CosmoOS builds (`<arch>-release/`, `<arch>-debug/`) and each check's work directory
(`c1-<arch>/` with the image and `serial.log`, `c4-<arch>/`, …). Images are signed with the CosmoOS developer key
(`MODSIGN_KEY`); they are test artefacts, not releases.
