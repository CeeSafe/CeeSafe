# CeeSafe in the Linux kernel

`mq/` is a Linux kernel module written in CeeSafe: `ceesafe_mq.ko`, a misc device `/dev/ceesafe_mq` that is a bounded
queue of messages. A `write` adds one message, a `read` takes the oldest. It is the first Linux use of the
freestanding profile ([spec/profiles.md](../../spec/profiles.md) §4, §5), built by Kbuild with the kernel's own
compiler, flags and headers, and loaded and tested in a running kernel.

It is a demonstration, not a Linux profile: there is no `--kernel linux`, and the bindings in `mq/src/linux.cee` cover
what this module needs.

## What is CeeSafe and what is C

| File | Lines | What |
|---|---:|---|
| `mq/src/mq.cee` | 205 | the driver: the queue, its ownership, its locking, the four entry points |
| `mq/src/linux.cee` | 177 | Linux for CeeSafe: `KernelHeap`, `KBox<T>`, `Mutex<T>`, `SpinLock<T>`, `Log` |
| `mq/module.c` | 14 | the module as one translation unit: the runtime, the generated C and the glue (why: "Found while doing this") |
| `mq/glue.c` | 81 | what Linux requires to be C macros: `module_init`, `MODULE_LICENSE`, the `file_operations` table, and the panic binding |
| `mq/include/ceesafe_linux.h` | 91 | one `static inline` function per kernel macro CeeSafe calls (`mutex_init`, `spin_lock_init`, `GFP_KERNEL`, `pr_info`, the user copies), with their contracts |
| `mq/include/std*.h`, `float.h` | 64 | the freestanding standard headers in the kernel's own types |
| `mq/control/sleep_under_spinlock.c` | 38 | a C module with the mistake `mq/negative/` makes, to show what a debug kernel reports at run time |

The kernel build needs no CeeSafe tool. `ceesafec build --emit c -o gen` runs on any host and writes `gen/main.c`,
`gen/ceesafe_rt_core.c` (the freestanding core runtime) and two headers; Kbuild compiles those, through `module.c`,
like any other C.

## What it shows

- **Struct layouts from the kernel's headers.** `struct mutex` and `spinlock_t` are imported from
  `<linux/mutex.h>` and `<linux/spinlock.h>` and embedded by value in `Mutex<T>` and `SpinLock<T>`. CeeSafe never
  states their size or fields: the C compiler lays them out for this kernel's configuration.
- **Ownership instead of cleanup code.** Each message is a kernel block owned by the queue or by the message before
  it. `rmmod` with messages still queued frees them all, and `mq.cee` has no code that walks the queue to do so.
- **Allocation failure is a value.** `kmalloc` returning `NULL` is `Err(AllocError)`, which becomes `-ENOMEM`.
- **Context effects against real Linux declarations.** `lx_kmalloc` (GFP_KERNEL), the user copies and
  `lx_mutex_lock` are declared `@effects(sleep)`. `mq/negative/` is the module's sources plus one function that
  allocates while holding a `SpinLock`; it does not compile:

  ```text
  sleep-under-spinlock.cee:13:12: error[N0295]: a 'SpinGuard<Latest>' is live here, which makes this an atomic context (type-system.md §32.3), but this may sleep: it calls 'KBox<u64>.create', which allocates a 'Box<Held<u64>, KernelHeap>', which calls 'lx_kmalloc', declared @effects(sleep, alloc)
  ```

## Build and run

On any host with `ceesafec`, in `mq/`:

```sh
ceesafec check                       # type, borrow and context checks
ceesafec build --emit c -o gen       # the C for Kbuild
(cd negative && ceesafec check)      # must fail with N0295
```

On a Linux machine with the kernel's headers and compiler, **one you can afford to crash** (a VM), in a copy of `mq/`
that has `gen/`:

```sh
make                                 # Kbuild: ceesafe_mq.ko (make LLVM=1 on a kernel built with clang)
(cd control && make)                 # optional: the C control module
sudo python3 test.py                 # insmod, the device from user space, rmmod, the kernel log
```

## Runs (2026-11-10)

The C was generated on a macOS arm64 host by `out/ceesafec` 0.28.0, and is the same for every row. All kernels are
Linux 6.1.190 (Debian 12's `6.1.0-54`, or built from Debian's `linux-source-6.1`).

| Machine | Kernel | Compiler | Kernel checkers | `make W=1` | `test.py` |
|---|---|---|---|---|---|
| AArch64, Lima VM (Apple `vz`), 8 CPUs | `6.1.0-54-cloud-arm64`, Debian's | gcc 12.2.0 | none | no warning | 24 pass, 0 fail |
| AArch64, the same VM | `6.1.190-ceesafe-debug`, built here | clang 14.0.6, lld | lockdep (`PROVE_LOCKING`), KASAN (generic), kmemleak, `DEBUG_ATOMIC_SLEEP` | no warning (`LLVM=1`) | 27 pass, 0 fail |
| x86-64, QEMU (emulated, TCG), 4 CPUs | `6.1.0-54-cloud-amd64`, Debian's | gcc 12.2.0 | none | no warning, objtool included | 24 pass, 0 fail |

On every row: `ceesafec check` and `ceesafec fmt --check` in `mq/` report nothing; `ceesafec check` in
`mq/negative/` reports N0295 at `sleep-under-spinlock.cee:13:12` and nothing else; `nm -u ceesafe_mq.ko` lists
kernel exports only. `make W=1` is Kbuild's extra warning set (`-Wextra -Wmissing-prototypes -Wmissing-declarations
-Wold-style-definition` and more) on top of the kernel's own flags: `-std=gnu11 -nostdinc`, and
`-mgeneral-regs-only` on AArch64 or `-mno-sse -mno-red-zone -mcmodel=kernel` with return thunks on x86-64.

`test.py`'s checks: an empty queue reads 0 bytes; messages of 5, 14 and 512 bytes come back whole and in order; a
short read; `EMSGSIZE` over 512 bytes; `ENOSPC` on the 17th message; `EFAULT` for a bad user address on write and on
read, the failed read leaving its message queued; `rmmod` refused while the device is open; `rmmod` with 14 messages
queued. Then a second load with four processes writing and reading 5,000 times each. Any line from a kernel checker
in the log (an oops, a `BUG`, a `WARNING`, a lockdep, KASAN or atomic-sleep report) fails a check. The module's lines
were the same on all three rows:

```text
ceesafe_mq: ready, 16 messages of at most 512 bytes
ceesafe_mq: written 20, read 6, refused 3, freed with the device 14
ceesafe_mq: unloaded, 0 kernel block(s) live
ceesafe_mq: ready, 16 messages of at most 512 bytes
ceesafe_mq: written 20000, read 20000, refused 0, freed with the device 0
ceesafe_mq: unloaded, 0 kernel block(s) live
```

### The debug kernel

`6.1.190-ceesafe-debug` is Debian's 6.1 source with the running configuration trimmed to the loaded modules
(`localmodconfig`), plus `KASAN`, `KASAN_GENERIC`, `PROVE_LOCKING`, `DEBUG_ATOMIC_SLEEP`, `DEBUG_KMEMLEAK` and
`LKDTM=m`, built with `make LLVM=1`. Its boot log has `kasan: KernelAddressSanitizer initialized (generic)`, `Lock
dependency validator` and `kmemleak: Kernel memory leak detector initialized`. The module built for it is
instrumented: its undefined symbols include `__asan_load8_noabort`, `__kasan_check_write`, `mutex_lock_nested` and
`__might_fault`.

On it `test.py` makes three more checks:

- **kmemleak** finds no unreferenced block after both runs (two scans). This is the kernel's own count, beside the
  adapter's "0 kernel block(s) live".
- **Control, atomic sleep:** `control/sleep_under_spinlock.ko`, the C module that calls `kmalloc(GFP_KERNEL)` with a
  spinlock held, builds without a warning and is reported when it loads: `BUG: sleeping function called from invalid
  context`. On the two stock kernels the same module loads and nothing is reported. `mq/negative/` is that mistake in
  CeeSafe, and it does not compile.
- **Control, KASAN:** lkdtm's `READ_AFTER_FREE` is reported: `BUG: KASAN: use-after-free in
  lkdtm_READ_AFTER_FREE`.

The controls run last, and show that a silent log above them means the checkers found nothing, not that they were
off.

### Earlier, and what the runs found

- The first run of `test.py` (AArch64, gcc) failed one check and found a defect in the driver: a read to a bad user
  address took its message off the queue before the copy, so the message was lost. `dequeue` now takes the message
  off only after the copy succeeds, and `test.py` checks it.
- The first x86-64 build had objtool warnings; see "Found while doing this".

## Not done, and limits

- **Not run:** x86-64 with clang, or with lockdep or KASAN (the x86-64 guest is emulated, and has Debian's kernel
  only); gcc on the debug kernel; a kernel newer than 6.1; allocation-failure injection; real hardware.
- **The stress run did not fill the queue** (0 writes refused), so it contends the locks but not the `ENOSPC` path.
- **One lockdep class per lock type.** `lx_mutex_init` and `lx_spin_lock_init` are single functions, so every lock
  made through them shares a class. Lockdep ran with that, and this module has one lock of each type, so its
  silence is meaningful here; a real binding needs a class per lock, as C gets from the macro.
- **A panic is `BUG()`.** A failed bounds or overflow check in the module reports its CeeSafe source line and stops
  the thread. Whether that or an error return is right is the integration's decision (profiles.md §5).
- **`KBox<T>` is asserted thread-safe** (`@threadSafe`, in a package with `trusted = true`): a box with a user
  allocator is not `Send` by itself. The assertion is that `kfree` may run on any thread, which is Linux's rule.
- **The standard headers are replaced** for CeeSafe's C inside the kernel build (`mq/include/stdint.h` and three
  more), because the kernel defines `int64_t` and the rest itself. This belongs in a Linux profile, not in a module.
- **The `file_operations` table is C.** CeeSafe can state static C objects with their ops tables (ADR 0080); this
  module does not use that yet.

## Found while doing this

- **objtool and the panic function (x86-64).** Generated code calls `ceesafe_panic`, which never returns, and has
  nothing after the call. objtool knows that a function does not return only from its own list of names or from
  seeing the function's body in the same object. With the generated C, the runtime and the glue compiled as three
  objects, it warned for every caller: `objtool: ceeF_enqueue() falls through to next function ...`. The module
  worked, but those functions were not validated. `mq/module.c` now makes the three one translation unit, so objtool
  sees that `ceesafe_panic` ends in `BUG()`: no warning. Rust's kernel support met the same rule and added its
  functions to objtool's list. A Linux profile needs an answer that does not depend on a single unit.

- `guide/unsafe-and-c.md` showed `@ctype("struct timer")`, which is N0292 (`@ctype` names a typedef; a struct needs
  none). The example was not compiled (`ceesafe: skip`). Corrected there and in `docs/blog/`.
- `ceesafec fmt --check` in a package whose `sources` name a directory outside it (`mq/negative/`, `"../src"`)
  stops with `cannot read 'linux.cee'`, and `check` there prints those files' diagnostics without their directory.
  Not fixed.
