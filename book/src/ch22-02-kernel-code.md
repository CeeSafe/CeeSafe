---
title: Kernel code
summary: How a driver is written in CeeSafe - device registers at a fixed address through Volatile and MmioPtr, statics and what may initialise them, an interrupt handler the compiler checks with @context(irq), and builds without floating point.
assumes: [ch22-01-the-freestanding-profile, ch16-03-shared-state]
keywords: [driver, register, MMIO, Volatile, MmioPtr, "@repr(C)", static, "static mut", "@constant", AtomicU64, "@context(irq)", "@effects", interrupt, "--no-float", "float = false", "@align", "Cpu.spinHint", "lib/kernel", UART]
diagnostics: [N0251, N0280, N0295, N0297, N0338]
specification: ["type-system.md §29", "type-system.md §32", "type-system.md §35.9", "type-system.md §39", "profiles.md §3"]
---

# Kernel code

**This section teaches** the parts of CeeSafe that exist for code that talks to hardware: reading and writing
device registers, data that lives for the whole run at a fixed address, code that an interrupt may run, and builds
that must not use floating point. It assumes [The freestanding profile](./ch22-01-the-freestanding-profile) and
the atomics of [Shared state](./ch16-03-shared-state). The example is a driver for a UART, a serial port, in the
package `uart` under `book/projects/ch22/`.

## Registers are memory that is not memory

A device shows itself to the processor as a few words at a fixed address, its **registers**. Writing a byte to
the UART's data register sends it down the wire. Reading the status register asks the device a question, and the
answer changes by itself. So the two things a compiler may do with ordinary memory are both wrong here: it may
not drop a write that nothing reads back, and it may not reuse the value of an earlier read.

An access that the compiler must perform exactly as written, once, in order, is a **volatile** access.
`lib/core` has two types that make every access to a register volatile and keep the unsafe part in one place.

<span class="listing-caption"><strong>Listing 22-5</strong>, <code>uart/src/lib.cee</code>: The registers as a struct, and a driver that owns their address.</span>

<!-- ceesafe: skip -->
```ceesafe
/// The device's registers, in the order and sizes its manual gives.
@repr(C)
struct UartRegisters {
    data: Volatile<u32>;
    status: Volatile<u32>;
    control: Volatile<u32>;
}

/// Status: the transmitter cannot take a byte yet.
const TRANSMIT_FULL: u32 = 0x20;
/// Status: no received byte is waiting.
const RECEIVE_EMPTY: u32 = 0x10;
/// Control: the device is switched on.
const ENABLE: u32 = 0x01;

struct Uart {
    registers: MmioPtr<UartRegisters>;

    /// Safety: `base` is the mapped address of a UART's registers, and no other `Uart` is in use.
    unsafe fn attach(base: usize) -> Uart => Uart { registers: MmioPtr<UartRegisters>.at(base) };

    fn enable(borrow mut this) {
        this.registers.getMut().control.write(ENABLE);
    }

    fn send(borrow mut this, byte: u8) {
        while ((this.registers.get().status.read() & TRANSMIT_FULL) != 0) {
            Cpu.spinHint();
        }
        this.registers.getMut().data.write(byte as u32);
    }

    fn receive(borrow mut this) -> u8? {
        if ((this.registers.get().status.read() & RECEIVE_EMPTY) != 0) {
            return null;
        }
        return truncate<u8>(this.registers.get().data.read());
    }
}
```

- **`Volatile<T>`** is a cell with two methods: `read()`, through a shared borrow, and `write(v)`, through a
  `borrow mut`. It has `T`'s size, so a `@repr(C)` struct of them has exactly the layout of the device. It cannot
  be copied, because a copy would be a plain read.
- **`MmioPtr<T>`** is the address of such a struct. `MmioPtr<T>.at(address)` is `unsafe`: you promise that the
  address is mapped, aligned, and reached through this value only. After that, `get()` gives a `borrow T` and
  `getMut()` a `borrow mut T`, and using the registers needs no `unsafe`.
- **`Cpu.spinHint()`** tells the processor that the loop is waiting. It is the right body for a busy wait.

In the C that `ceesafec` writes, `status.read()` is `*(volatile uint32_t *)p`: one load each time round the loop.

::: tip Rule
Reach a device's registers through `Volatile` fields of a `@repr(C)` struct behind an `MmioPtr`. The one unsafe
step is `MmioPtr<T>.at`, whose `/// Safety:` comment states where the address comes from.

*Specification: `type-system.md` §29.1, §29.2.*
:::

## Statics

A driver must remember things between calls: where its device is, how much it has received. A local cannot, and
there is no heap to put a value in. A **static** is one value for the whole run, at a fixed address, declared at
the top level.

<span class="listing-caption"><strong>Listing 22-6</strong>, <code>uart/src/lib.cee</code>: Three statics, and the exported functions that use them.</span>

<!-- ceesafe: skip -->
```ceesafe
/// The address of the registers, 0 until `uart_start` is called.
static BASE: AtomicUSize = AtomicUSize.constant(0);
/// The bytes received so far, counted by the interrupt handler.
static RECEIVED: AtomicU64 = AtomicU64.constant(0);
/// The last byte received. Only the interrupt handler writes it.
static mut LAST: u8 = 0;

/// Switches the UART at `base` on.
/// Safety: `base` is the mapped address of a UART's registers; the boot code calls this once, first.
@export("uart_start")
fn start(base: usize) {
    BASE.store(base);
    var uart = unsafe { Uart.attach(base) };
    uart.enable();
}

/// Sends `length` bytes at `text`, and returns how many it sent.
/// Safety: `uart_start` ran, the caller passes `length` readable bytes, and one caller sends at a time.
@export("uart_write")
fn write(text: *u8, length: usize) -> u32 {
    var uart = unsafe { Uart.attach(BASE.load()) };
    var sent: u32 = 0;
    for (byte in unsafe { Slice.fromRaw(text, length) }) {
        uart.send(byte);
        sent += 1;
    }
    return sent;
}

/// The interrupt handler: takes the byte that arrived.
/// Safety: `uart_start` ran; the handler is the only code that reads the data register and writes LAST.
@context(irq)
@export("uart_on_interrupt")
fn onInterrupt() {
    var uart = unsafe { Uart.attach(BASE.load()) };
    match (uart.receive()) {
        Some(byte) => {
            unsafe { LAST = byte; }
            let _ = RECEIVED.fetchAdd(1);
        }
        null => {}
    }
}

/// How many bytes have arrived.
@export("uart_received")
fn received() -> u64 => RECEIVED.load();

/// The last byte that arrived.
/// Safety: a one-byte read; the caller accepts a value that an interrupt may replace at once.
@export("uart_last")
fn last() -> u8 => unsafe { LAST };
```

There are two kinds, and the difference is who checks the sharing.

A plain **`static`** can be reached by any code at any moment, an interrupt handler included, so its type must be
safe to share: an atomic, a lock, or data that never changes such as a `StringView` or a table of integers. You
use it as you use a value behind a shared borrow. `BASE.store(base)` needs no `unsafe`, because an atomic changes
through a shared borrow.

A **`static mut`** may hold any type and may be assigned. The compiler cannot know that two uses do not collide,
so every use is `unsafe`, and the `/// Safety:` comment must say why it is sound. Prefer an atomic. `LAST` is a
`static mut` here to show the form.

**Initialisers.** There is no code that runs before the first export is called: an environment such as a kernel
runs no constructors. So a static's value is computed by the compiler and placed in the object as data. The
initialiser may be a constant expression, a string literal, a struct or array literal of such things, or a call
of a function marked `@constant` (a function whose body is one such expression). `AtomicU64.constant(0)` is one;
`AtomicU64.create(0)`, the run-time form of chapter 16, is not.

::: tip Rule
A `static` has a type that is safe to share and is used through shared borrows. Every use of a `static mut` is
unsafe. Both are initialised at compile time, and neither is ever dropped.

*Specification: `type-system.md` §39.1 to §39.4.*
:::

## Code an interrupt runs

An interrupt handler runs between any two instructions of the code it interrupts. It must not wait for a lock
that code may hold, and it must not allocate, since the allocator may be the code it interrupted. In C these are
rules in a comment. In CeeSafe, `@context(irq)` on a function makes the compiler check them through every
function the handler calls.

The compiler knows what CeeSafe code does. For a C function you say it: `@effects()` before a declaration in an
`extern "C"` block states that the function neither sleeps nor allocates, `@effects(sleep)` that it may block.
A C function without `@effects` is taken to do both. `@context(atomic)` is the weaker context, for code that
holds a spin lock: it may allocate but not sleep.

::: tip Rule
A function marked `@context(irq)` may not perform, or call anything that may perform, an operation that sleeps
or allocates.

*Specification: `type-system.md` §32.3.*
:::

## Running the driver without the device

The registers are three `u32` words, so three words of ordinary memory can play the device.

<span class="listing-caption"><strong>Listing 22-7</strong>, <code>uart/host/main.c</code>: A test bench that hands the driver an array as its registers and raises the "interrupt" by calling the handler.</span>

<!-- ceesafe: skip -->
```c
#include "ceesafe/rt.h"
#include "ceesafe_exports.h"
#include <stdio.h>
#include <stdlib.h>

_Noreturn void ceesafe_panic(cee_panic_kind kind, cee_loc loc) {
    fprintf(stderr, "board: stopped: %s at %s:%u:%u\n", ceesafe_panic_reason(kind), loc.file, (unsigned)loc.line,
        (unsigned)loc.column);
    exit(3);
}

/* The "device": three words of ordinary memory, laid out as the registers are. */
static uint32_t device[3];

int main(void) {
    uart_start((size_t)(uintptr_t)device);
    printf("control: %u\n", (unsigned)device[2]);

    const uint8_t text[] = {'h', 'e', 'l', 'l', 'o'};
    uint32_t sent = uart_write(text, sizeof text);
    printf("sent: %u, data register: '%c'\n", (unsigned)sent, (char)device[0]);

    device[1] = 0x10; /* status: nothing received */
    uart_on_interrupt();
    printf("received: %llu\n", (unsigned long long)uart_received());

    device[0] = 'k'; /* a byte arrives */
    device[1] = 0;
    uart_on_interrupt();
    printf("received: %llu, last: '%c'\n", (unsigned long long)uart_received(), (char)uart_last());
    return 0;
}
```

```sh
ceesafec build
cc -I out host/main.c out/uart.o -o out/host
./out/host
```

```text
control: 1
sent: 5, data register: 'o'
received: 0
received: 1, last: 'k'
```

This shows that the driver's logic and its register layout are right. It cannot show what only hardware shows:
a transmitter that is really busy, an interrupt that arrives in the middle of `uart_write`. On a board,
`uart_start` gets the address from the board's manual, and the interrupt table points at `uart_on_interrupt`.

## Without floating point, and other settings

- **No floats.** A kernel is usually compiled so that it never touches the floating-point registers, which
  belong to the programs it runs. `float = false` in the manifest, or `--no-float`, rejects every float type and
  literal with `N0297`. The package `uart` sets it.
- **Alignment.** `@align(n)` on a struct, with `n` a power of two up to 4096, raises its alignment, for a value
  that must fill a cache line or a page.
- **Entry names.** An export's symbol may begin with one underscore and a lowercase letter, so
  `@export("_start")` gives a boot stub the name it jumps to.
- **The target.** `Target.pointerBits()`, `Target.isX86_64()` and `Target.isAArch64()` answer for the machine the
  C is compiled for.
- **Sections.** CeeSafe has no attribute that places a function or a static in a named linker section. A table
  that a linker script collects stays a few lines of C beside the package.

The compiler also embeds a library named `lib/kernel` (spin locks and mutexes whose guards are atomic contexts,
per-processor data, a kernel heap, a log). It binds the C interface of one kernel, CosmoOS, and
`--kernel cosmoos` adds it to a freestanding build. It was not run for this book, which has no such kernel to
link against. For another kernel you write the same thin layer over its C functions with `extern "C"` and
`@effects`, as Listing 22-3 does for an allocator; `profiles.md` §3 and §5 describe both cases.

## Mistakes

::: warning Mistake: calling an unknown C function from an interrupt handler
A C function with no `@effects` may do anything, so the handler may sleep.
:::

<span class="listing-caption"><strong>Listing 22-8</strong>, <code>main.cee</code>: An interrupt handler that calls a C function the compiler knows nothing about.</span>

<!-- ceesafe: fail N0295 -->
```ceesafe
extern "C" {
    fn board_log(value: u64);
}

static TICKS: AtomicU64 = AtomicU64.constant(0);

/// The timer's interrupt handler.
/// Safety: board_log only reads its argument.
@context(irq)
@export("timer_on_interrupt")
fn onInterrupt() {
    let ticks = TICKS.fetchAdd(1) + 1;
    unsafe { board_log(ticks); }
}
```

```text
main.cee:13:14: error[N0295]: 'onInterrupt' is @context(irq), but this may sleep: it calls 'board_log', a C function without @effects, which may do anything
   |
13 |     unsafe { board_log(ticks); }
   |              ^^^^^^^^^^^^^^^^^
```

If `board_log` really neither sleeps nor allocates, declare it so: `@effects()` on the line before `fn board_log`.
The compiler trusts that line as it trusts the function's parameter types.

::: warning Mistake: using a `static mut` as an ordinary variable
:::

<span class="listing-caption"><strong>Listing 22-9</strong>, <code>main.cee</code>: A <code>static mut</code> changed outside <code>unsafe</code>.</span>

<!-- ceesafe: fail N0280 -->
```ceesafe
static mut TICKS: u64 = 0;

/// Counts one tick of the timer.
@export("timer_tick")
fn tick() {
    TICKS += 1;
}
```

```text
main.cee:6:5: error[N0280]: using 'static mut TICKS' is unsafe: use it inside 'unsafe { ... }' or an 'unsafe fn' (type-system.md §27.3)
  |
6 |     TICKS += 1;
  |     ^^^^^
```

Wrapping it in `unsafe` compiles, and is wrong if an interrupt can also call `tick`. Make it
`static TICKS: AtomicU64 = AtomicU64.constant(0);` and write `TICKS.fetchAdd(1)`.

::: warning Mistake: initialising a static with a run-time call
:::

<span class="listing-caption"><strong>Listing 22-10</strong>, <code>main.cee</code>: A static whose value would need code to run before the program starts.</span>

<!-- ceesafe: fail N0338 -->
```ceesafe
fn bootTime() -> u64 => 1700000000;

static STARTED: u64 = bootTime();

/// The time the system started.
@export("timer_started")
fn started() -> u64 => STARTED;
```

```text
main.cee:3:23: error[N0338]: 'bootTime' is not '@constant': not a static initialiser (type-system.md §39.4)
  |
3 | static STARTED: u64 = bootTime();
  |                       ^^^^^^^^^^
```

Put `@constant` before `fn bootTime` if its body is a constant expression, as it is here. If the value is only
known at run time, start the static at 0 as an atomic and store the value in an export the boot code calls.

::: warning Mistake: taking a register out of its struct
A `Volatile` cannot be copied or moved out from behind a borrow.
:::

<span class="listing-caption"><strong>Listing 22-11</strong>, <code>main.cee</code>: A register bound to a local.</span>

<!-- ceesafe: fail N0251 -->
```ceesafe
@repr(C)
struct TimerRegisters {
    count: Volatile<u32>;
    control: Volatile<u32>;
}

/// The timer's count.
/// Safety: `base` is the mapped address of the timer's registers.
@export("timer_count")
fn count(base: usize) -> u32 {
    let timer = unsafe { MmioPtr<TimerRegisters>.at(base) };
    let register = timer.get().count;
    return register.read();
}
```

```text
main.cee:12:5: error[N0251]: cannot move a 'Volatile<u32>' out of the value 'a temporary' refers to; only Copy values can be read through a reference
   |
12 |     let register = timer.get().count;
   |     ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
```

Read it where it is: `return timer.get().count.read();`.

::: warning Mistake: a float in a kernel build
:::

<span class="listing-caption"><strong>Listing 22-12</strong>, <code>variants/no-float/src/lib.cee</code>: Float arithmetic in a package whose manifest says <code>float = false</code>.</span>

<!-- ceesafe: skip -->
```ceesafe
/// A raw reading as millivolts.
@export("sensor_millivolts")
fn millivolts(raw: u32) -> u32 {
    let volts = raw as f64 * 3.3 / 4096.0;
    return saturatingTruncate<u32>(volts * 1000.0);
}
```

The compiler reports each float type and literal. The first of the four messages:

```text
src/lib.cee:4:24: error[N0297]: type 'f64': floats are disabled in this build (--no-float, type-system.md §35.9)
  |
4 |     let volts = raw as f64 * 3.3 / 4096.0;
  |                        ^^^
```

Use integers in a smaller unit: `truncate<u32>(raw as u64 * 3300 / 4096)`.

::: details Coming from C
`Volatile<u32>` is a `volatile uint32_t` member, and `MmioPtr<T>.at(a)` is the cast `(T *)a`, done once and
named. A plain `static` is closer to a `const` or `_Atomic` object than to a C global: the C global is
`static mut`. `@context(irq)` has no C counterpart but a comment and a run-time check such as `might_sleep()`.
:::

::: details Coming from Rust
`Volatile` and `MmioPtr` do the work of `read_volatile`/`write_volatile` and of a register crate, and are in the
core library. `static` and `static mut` mean what they mean in Rust, with `@constant` in the place of `const fn`.
Rust has no checked counterpart of `@context(irq)`.
:::

## Summary

- A device's registers are a `@repr(C)` struct of `Volatile<T>` fields behind an `MmioPtr<T>`. Each `read()` and
  `write(v)` happens exactly once. Only `MmioPtr<T>.at(address)` is unsafe.
- `static NAME: T = …;` is one value for the whole run. Its type must be safe to share. Every use of a
  `static mut` is unsafe (`N0280`).
- A static's initialiser is evaluated by the compiler: constants, literals and `@constant` functions (`N0338`
  otherwise).
- `@context(irq)` forbids sleeping and allocating in a function and in all it calls, and `@context(atomic)`
  forbids sleeping (`N0295`). `@effects(…)` says what a C function may do.
- `float = false` or `--no-float` rejects floats (`N0297`). `@align(n)` raises a struct's alignment.
- A C test bench with an array for registers runs a driver's logic on your own machine. It does not replace the
  device.

Next, [CeeSafe inside a host program](./ch22-03-inside-a-host-program) returns to a hosted system, where the
environment is a C program that has a heap of its own.
