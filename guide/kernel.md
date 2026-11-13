# Kernel code

CeeSafe is designed to run inside an operating system kernel as well as in user programs. Its kernel support was
developed against [CosmoOS](https://github.com/vkuttyp/CosmoOS), a kernel written in C, where the language runs as
kernel components and as loadable modules on x86-64 and AArch64 (CosmoOS builds those with the ceesafec it pins).
Nothing in it is specific to CosmoOS: the same mechanisms fit any C kernel or firmware.

## The freestanding profile

`--profile freestanding` (or `profile = "freestanding"` in a package's manifest) compiles for an environment without a
C library or an operating system:

- there is no `main`: the entry points are `@export` functions that the environment calls;
- everything that needs a hosted runtime is unavailable, and naming it is an error: `Console`, files, sockets,
  `SystemHeap` and the collections built on it;
- memory comes from an allocator the environment provides, written in CeeSafe over the kernel's C functions;
- `--no-float` (or `float = false`) rejects floating point, for code compiled without FP registers.

`ceesafec build --emit obj` then writes one relocatable object, with only the undefined symbols the environment
provides, ready for the kernel's own build to link. Beside the object it writes the headers the kernel's C includes:
`ceesafe/rt.h`, which declares the `ceesafe_panic` the environment defines, and `ceesafe_exports.h` when the code has
exports (`-o build/crc.o` gives `build/ceesafe/rt.h`: compile the C with `-I build`).

<!-- ceesafe: freestanding -->
```ceesafe
/// A CRC-32C over `n` bytes at `data`, for C callers.
/// Safety: `data` points to `n` readable bytes, as the C prototype's contract says.
@export("crc32c_sum")
unsafe fn crc32c(data: *u8, n: usize) -> u32 {
    return checksum(Slice.fromRaw(data, n));
}

fn checksum(bytes: Slice<u8>) -> u32 {
    var crc: u32 = 0xFFFFFFFF;
    for (b in bytes) {
        crc = crc ^ (b as u32);
        for (bit in 0..8) {
            let mask = if ((crc & 1) != 0) 0x82F63B78u32 else 0u32;
            crc = (crc >> 1) ^ mask;
        }
    }
    return crc ^ 0xFFFFFFFF;
}
```

The unsafe part is confined to the boundary: the exported function turns C's pointer and length into a slice under a
stated contract, and everything behind it is safe, checked CeeSafe.

## Context effects

Kernel code runs in contexts with rules: while a spin lock is held it must not sleep; in an interrupt handler it must
not sleep or allocate. CeeSafe checks these rules at compile time, across function calls:

- an operation's **effects** are `sleep` (it may block) and `alloc` (it may allocate), inferred for every function
  from what it does and calls, its destructors included;
- `@effects(…)` declares what a C function may do, or bounds what a CeeSafe function may do;
- `@context(atomic)` on a function forbids `sleep` in it; `@context(irq)` forbids `sleep` and `alloc`;
- `@context(atomic)` on a **struct** makes holding one an atomic context: a spin-lock guard.

<!-- ceesafe: fail N0295 -->
```ceesafe
@header("kernel/sched.h")
extern "C" {
    @effects(sleep)
    fn cosmo_sleep_ms(ms: u32);
}

/// Holding a spin-lock guard makes the code that runs meanwhile atomic: it must not sleep.
@context(atomic)
struct SpinGuard { cpu: u32; drop {} }

/// Safety: cosmo_sleep_ms has no preconditions; it may sleep.
fn backOff() { unsafe { cosmo_sleep_ms(1); } }

fn update(counter: borrow mut u64) {
    let guard = SpinGuard { cpu: 0 };
    counter += 1;
    backOff(); // sleeps while the guard is held
}
```

```text
main.cee:17:5: error[N0295]: a 'SpinGuard' is live here, which makes this an atomic context (type-system.md §32.3), but this may sleep: it calls 'backOff', which calls 'cosmo_sleep_ms', declared @effects(sleep)
   |
17 |     backOff(); // sleeps while the guard is held
   |     ^^^^^^^^^^
```

This proves that CeeSafe code does nothing its context forbids, given that the `@effects` of C functions are declared
correctly. It does not prove the absence of deadlocks or lock-order inversions: the kernel's own lockdep stays the
check for those.

## Hardware registers

`lib/core` provides `Volatile<T>`, a cell read and written only by volatile accesses, and `MmioPtr<T>`, the address of
memory-mapped registers. Making an `MmioPtr` is the one unsafe step; register access through it is safe.

<!-- ceesafe: freestanding -->
```ceesafe
@repr(C)
struct UartRegs {
    data: Volatile<u32>;
    status: Volatile<u32>;
}

struct Uart {
    regs: MmioPtr<UartRegs>;

    /// Safety: `base` is the mapped address of a UART that no other `Uart` aliases.
    public unsafe fn attach(base: usize) -> Uart => Uart { regs: MmioPtr<UartRegs>.at(base) };

    public fn putByte(borrow mut this, b: u8) {
        while ((this.regs.get().status.read() & 0x20) != 0) {
            Cpu.spinHint();
        }
        this.regs.getMut().data.write(b as u32);
    }
}
```

## Kernel allocators, locks and objects

The CosmoOS integration (the repository's `integrations/cosmoos/`) builds the rest from these pieces:

- `KernelHeap`, an allocator over `kmalloc`/`kfree`, so `Box.createIn(KernelHeap {}, value)` allocates kernel memory
  and returns `Err` when the kernel has none;
- `SpinLock<T>`, `IrqSpinLock<T>` and `Mutex<T>` that hold the data they protect: the data is reachable only through a
  guard, the guard unlocks when it is destroyed, and a spin guard is an atomic context;
- `KRef<T>`, a reference-counted kernel object whose final release may sleep, so destroying one under a spin lock does
  not compile;
- C kernel state (lists, locks, kernel objects) embedded by value in CeeSafe structs, built in place and never moved;
- a loadable module whose state, `init` and teardown are CeeSafe.

How these were built and tested inside CosmoOS, including a stress test under lockdep, is in the
CosmoOS reports and the [profiles](../spec/profiles.md).

## Async tasks in kernel code

Async tasks run in the freestanding profile too. The environment gives the executor four functions:
- the time;
- a way to idle until a deadline;
- a wake that is safe in an interrupt;
- a slot for each thread's executor.

A scope's children come from an allocator you name, `await Async.scopeIn(KernelHeap {}, oom, (s) => { … })`, since
there is no system heap. Timers, `race`, `all`, `shield` and cancellation work as in a hosted program.

An interrupt handler resumes a task through an `AsyncSignal`. The task waits with `await signal.wait()`. The handler,
an `@context(irq)` function, calls `signal.raise()`, which has no effects. A raise that comes before the wait is kept.
`Async.blocking`, `Async.runWorkers`, `spawnSend` and async I/O need threads or descriptors, so they stay hosted.
CosmoOS's boot self-test `cee-async` runs these over the kernel's clock, a wait queue and a timer's interrupt.

## Next

[The ceesafec command](ceesafec.md).
