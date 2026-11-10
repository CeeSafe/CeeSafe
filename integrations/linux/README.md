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
| `mq/glue.c` | 81 | what Linux requires to be C macros: `module_init`, `MODULE_LICENSE`, the `file_operations` table, and the panic binding |
| `mq/include/ceesafe_linux.h` | 91 | one `static inline` function per kernel macro CeeSafe calls (`mutex_init`, `spin_lock_init`, `GFP_KERNEL`, `pr_info`, the user copies), with their contracts |
| `mq/include/std*.h`, `float.h` | 64 | the freestanding standard headers in the kernel's own types |

The kernel build needs no CeeSafe tool. `ceesafec build --emit c -o gen` runs on any host and writes `gen/main.c`,
`gen/ceesafe_rt_core.c` (the freestanding core runtime) and two headers; Kbuild compiles those like any other C.

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
make                                 # Kbuild: ceesafe_mq.ko
sudo python3 test.py                 # insmod, the device from user space, rmmod, the kernel log
```

## Run (2026-11-10)

Debian 12 in a Lima VM on macOS arm64 (4 CPUs): Linux `6.1.0-54-cloud-arm64` (Debian 6.1.190-1), gcc 12.2.0,
`linux-headers-6.1.0-54-cloud-arm64`. The C was generated on the macOS host by `out/ceesafec` 0.28.0.

| Check | Result |
|---|---|
| `ceesafec check`, `ceesafec fmt --check` in `mq/` | no diagnostics, no file listed |
| `ceesafec check` in `mq/negative/` | N0295 at `sleep-under-spinlock.cee:13:12`, and no other diagnostic |
| `make` | `ceesafe_mq.ko` built, no warning |
| `make W=1` (Kbuild's extra warnings: `-Wextra -Wmissing-prototypes -Wmissing-declarations -Wold-style-definition` and more, with the kernel's `-std=gnu11 -nostdinc -mgeneral-regs-only -Wframe-larger-than=2048`) | no warning from `glue.c`, `gen/main.c` or `gen/ceesafe_rt_core.c` |
| `nm -u ceesafe_mq.ko` | kernel exports only: `__kmalloc`, `kfree`, `__mutex_init`, `mutex_lock`, `mutex_unlock`, `queued_spin_lock_slowpath`, `__arch_copy_from_user`, `__arch_copy_to_user`, `__check_object_size`, `_printk`, `misc_register`, `misc_deregister`, `noop_llseek`, `memcpy`, `memset`, `strlen`, `__stack_chk_fail`, `alt_cb_patch_nops` |
| `sudo python3 test.py` | 24 checks pass, 0 fail |

`test.py`'s checks: an empty queue reads 0 bytes; messages of 5, 14 and 512 bytes come back whole and in order; a
short read; `EMSGSIZE` over 512 bytes; `ENOSPC` on the 17th message; `EFAULT` for a bad user address on write and on
read, the failed read leaving its message queued; `rmmod` refused while the device is open; `rmmod` with 14 messages
queued. Then a second load with four processes writing and reading 5,000 times each. The kernel log of that run:

```text
ceesafe_mq: ready, 16 messages of at most 512 bytes
ceesafe_mq: written 20, read 6, refused 3, freed with the device 14
ceesafe_mq: unloaded, 0 kernel block(s) live
ceesafe_mq: ready, 16 messages of at most 512 bytes
ceesafe_mq: written 20000, read 20000, refused 0, freed with the device 0
ceesafe_mq: unloaded, 0 kernel block(s) live
```

The first run of `test.py` failed one check and found a defect in the driver: a read to a bad user address took its
message off the queue before the copy, so the message was lost. `dequeue` now takes the message off only after the
copy succeeds, and `test.py` checks it.

## Not done, and limits

- **Not run:** x86-64; a kernel with lockdep, KASAN or kmemleak (Debian's cloud kernel has none, so "no block live"
  is the adapter's own count, and no lock-order checking ran); allocation-failure injection; clang as the kernel's
  compiler.
- **The stress run did not fill the queue** (0 writes refused), so it contends the locks but not the `ENOSPC` path.
- **One lockdep class per lock type.** `lx_mutex_init` and `lx_spin_lock_init` are single functions, so every lock
  made through them shares a class. A real binding needs a class per lock, as C gets from the macro.
- **A panic is `BUG()`.** A failed bounds or overflow check in the module reports its CeeSafe source line and stops
  the thread. Whether that or an error return is right is the integration's decision (profiles.md §5).
- **`KBox<T>` is asserted thread-safe** (`@threadSafe`, in a package with `trusted = true`): a box with a user
  allocator is not `Send` by itself. The assertion is that `kfree` may run on any thread, which is Linux's rule.
- **The standard headers are replaced** for CeeSafe's C inside the kernel build (`mq/include/stdint.h` and three
  more), because the kernel defines `int64_t` and the rest itself. This belongs in a Linux profile, not in a module.
- **The `file_operations` table is C.** CeeSafe can state static C objects with their ops tables (ADR 0080); this
  module does not use that yet.

## Found while doing this

- `guide/unsafe-and-c.md` showed `@ctype("struct timer")`, which is N0292 (`@ctype` names a typedef; a struct needs
  none). The example was not compiled (`ceesafe: skip`). Corrected there and in `docs/blog/`.
- `ceesafec fmt --check` in a package whose `sources` name a directory outside it (`mq/negative/`, `"../src"`)
  stops with `cannot read 'linux.cee'`, and `check` there prints those files' diagnostics without their directory.
  Not fixed.
