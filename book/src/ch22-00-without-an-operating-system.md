---
title: Without an operating system
summary: How CeeSafe code is built for a machine with no operating system or inside another program - the freestanding profile, drivers over memory-mapped registers, and a library that takes its memory and its panics from a host.
assumes: [ch14-03-libraries-for-other-programs]
keywords: [freestanding, kernel, firmware, "--profile freestanding", "--emit obj", ceesafe_panic, Volatile, MmioPtr, static, "@context(irq)", "--no-float", "--emit lib", ceesafe_init, allocator]
---

# Without an operating system

**This chapter teaches** how to write CeeSafe for places where the usual program does not fit: a kernel, firmware
on a small board, a module that a larger program loads. It continues
[Libraries and objects for other programs](./ch14-03-libraries-for-other-programs), which builds CeeSafe code into
a C program on an ordinary system.

Every program in the book so far is **hosted**: an operating system starts it, gives it memory when it asks,
carries its output to a terminal and ends it when it panics. `Console.writeLine`, `Vector`, files and threads
all rest on those services. Take the operating system away and three questions need new answers:

- **Who calls the code?** There is no `main`, because nothing is there to call it. The code is entered through
  exported functions, by a boot stub, a kernel, or an interrupt.
- **Where does memory come from?** There is no heap until somebody writes one. Code uses the stack, memory it is
  handed, and allocators that the environment supplies.
- **What does a panic do?** It still ends the work, but the function that ends it is yours.

CeeSafe's answer is a **profile**: one option, `--profile freestanding`, that removes everything needing an
operating system from the language's reach, at compile time, and leaves the rest as it was. Ownership, borrowing,
bounds checks, overflow checks, generics and `Result` cost no runtime service, so all of them remain.

The chapter's programs are built for a kernel or a board, yet each of them runs on your own machine: a few lines
of C stand in for the hardware, as a test bench does. They are in the book's repository under
`book/projects/ch22/`:

- `gauge`, a freestanding object, and the C that supplies its panic function;
- `uart`, a driver for a serial port's registers, run against registers made of ordinary memory;
- `journal`, a hosted library that takes its allocator from the C program around it.

What the chapter does not do is boot. Linking an object into a real kernel image needs that kernel's linker
script, start-up code and build flags, which belong to the kernel and not to CeeSafe. Each section says where
its example stops and what comes after.

The chapter reads a little C, and its CeeSafe uses raw pointers and `unsafe` at the edges, which chapter 20
teaches. The text says what each use does.

## The sections

1. [The freestanding profile](./ch22-01-the-freestanding-profile): what remains without an operating system,
   `ceesafec build --profile freestanding --emit obj`, the panic function you supply, memory without a heap, and
   building for another processor.
2. [Kernel code](./ch22-02-kernel-code): registers at fixed addresses with `Volatile` and `MmioPtr`, statics and
   what may initialise them, interrupt handlers checked with `@context(irq)`, and builds without floating point.
3. [CeeSafe inside a host program](./ch22-03-inside-a-host-program): `ceesafe_init` in full: the host's
   allocator, the allocation report, `ceesafe_shutdown`, memory that crosses between the two sides, and the
   freestanding form of the same interface.
