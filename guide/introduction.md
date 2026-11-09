# What CeeSafe is

CeeSafe is a memory-safe systems programming language. Its syntax will look familiar if you know C# or TypeScript; its
safety comes from ownership and borrowing, as in Rust. It is compiled ahead of time to portable C11, so it runs
wherever a C compiler does: on Linux, macOS and Windows, on bare metal, and inside an operating system kernel.

```ceesafe
struct Account {
    id: u64;
    balance: i64;

    public fn deposit(borrow mut this, amount: i64) {
        this.balance += amount;
    }

    drop {
        Console.write("closing account ");
        Console.writeLine(this.id);
    }
}

fn main() {
    var account = Account { id: 7, balance: 0 };
    account.deposit(100);
    Console.writeLine(account.balance);
}
```

```text
100
closing account 7
```

## What it promises

In safe code, which is all code outside an `unsafe` block, CeeSafe rules out:

- **use after free and double free:** every value has one owner, and it is destroyed exactly once;
- **dangling references:** a reference cannot outlive what it refers to, which the compiler checks without lifetime
  annotations in most code;
- **data races between a reference and a write:** while something is borrowed it cannot be changed, and while it is
  borrowed mutably nothing else can read it;
- **out-of-bounds access and reading uninitialised memory:** indexes are checked, and a variable must be assigned
  before it is read;
- **silent integer overflow:** arithmetic that overflows stops the program, in every build. Where wrapping is meant,
  the code says so (`x.wrappingAdd(y)`).

It does this **without a garbage collector or reference counting.** Destruction is deterministic: a value is destroyed
at the end of its scope, or when its owner is, in an order the language defines.

## What makes it different

- **Lifetimes without annotations, mostly.** A function that takes and returns references names the parameters its
  result borrows from (`-> borrow(a, b) T`). Only types that store references, and functions over them, declare
  region parameters.
- **Allocation is explicit and pluggable.** Each allocation chooses its allocator: the system heap, an arena, a pool,
  a fixed buffer, a scratch frame, or your own. Allocation failure is a value (`Result<T, AllocError>`), handled or
  propagated with `?`.
- **The compiler shows its work.** It can explain which boxes it placed on the stack and which bounds and overflow
  checks it removed, and why.
- **C you can read.** The generated C is structured and warning-free, maps back to CeeSafe lines for debuggers, and has
  no undefined behaviour by design. A reference interpreter runs the same programs and stops on any memory error,
  which makes it a second implementation to test the compiler against.
- **Built for kernels too.** A freestanding profile without a C library, context effects that reject sleeping while a
  spin lock is held, C structs imported from the C codebase's own headers, and lock guards and reference-counted
  kernel objects. All of this is exercised inside [CosmoOS](https://github.com/vkuttyp/CosmoOS), an operating system
  written in C.

## Status

CeeSafe is **pre-1.0**. Its compiler, `ceesafec`, is written in CeeSafe and compiles itself. The language and its tools
work end to end, and are tested against a corpus of about 550 programs. But nobody has used it in production. What is
done and what is next are in the repository's `ROADMAP.md`.

Some things it does not have yet: trigonometry and logarithms, iterator adapters
(`map`, `filter` and the like; loops do their work). Each is planned, or recorded as an open decision.

## Where to go next

- [Install the compiler](installation.md) and [write a first program](getting-started.md).
- Read [the language guide](basics.md), chapter by chapter.
- Coming from another language? Start with [CeeSafe for Rust, Zig, C, C# and Go programmers](coming-from/index.md).
- The full rules are in the [specification](../spec/README.md): the grammar, the type system and the ownership rules.
