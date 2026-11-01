# CeeSafe for Zig programmers

Zig and CeeSafe agree on a lot: no hidden allocation, allocators as explicit values, errors as values, `defer`, no
garbage collector, first-class C interop, and code that can run in a kernel. They differ on what guarantees safety.
Zig checks bounds, overflow and null at run time in its safe build modes, and leaves use-after-free and dangling
pointers to the programmer. CeeSafe makes those compile errors, through ownership and borrowing, and keeps its run-time
checks on in every build.

## At a glance

| Zig | CeeSafe | Notes |
|---|---|---|
| `const x = 1;` / `var x: u32 = 1;` | `let x = 1;` / `var x: u32 = 1;` | |
| `[4]u8`, `[]u8`, `[]const u8` | `[u8; 4]`, `SliceMut<u8>`, `Slice<u8>` | slices are borrows: they cannot outlive their array |
| `?T`, `null`, `orelse`, `.?` | `T?`, `null`, `match`, `!` | |
| `!T`, `error.X`, `try`, `catch` | `Result<T, E>`, `Err(E.X)`, `?`, `match` | errors are ordinary enums |
| `catch unreachable` | `!` (postfix) | |
| `defer`, `errdefer` | `defer` | locals are destroyed automatically: most `defer`s are unnecessary |
| `allocator.create(T)`, `allocator.destroy(p)` | `Box.createIn(allocator, value)`; destroyed automatically | the allocator is part of the box's type |
| `std.ArrayList(T)` with an allocator | `Vector<T, A>` | |
| `std.heap.ArenaAllocator` | `Arena` | allocations borrow the arena: none survives it |
| `std.heap.FixedBufferAllocator` | `FixedBufferAllocator` | |
| `+%`, `+\|`, `@addWithOverflow` | `wrappingAdd`, `saturatingAdd`, `checkedAdd` | |
| `@intCast`, `@truncate` | `as` (lossless only), `truncate<T>` | |
| `comptime` generics | `<T>` generics with interface bounds | no compile-time execution |
| `@cImport` | `extern "C"` declarations; C structs imported by `@header` | |
| `export fn`, `callconv(.C)` | `@export`, `@callback` | |
| Debug / ReleaseSafe / ReleaseFast | one mode, always checked | proved checks are removed |
| `std.debug.print` | `Console.writeLine` | |

## The same program

```zig
// Zig 0.13
const std = @import("std");

fn sumAll(allocator: std.mem.Allocator, n: usize) !u64 {
    var list = std.ArrayList(u64).init(allocator);
    defer list.deinit();
    var i: u64 = 0;
    while (i < n) : (i += 1) try list.append(i * i);
    var total: u64 = 0;
    for (list.items) |x| total += x;
    return total;
}

pub fn main() !void {
    const total = try sumAll(std.heap.page_allocator, 10);
    std.debug.print("{}\n", .{total});
}
```

```ceesafe
fn sumAll(n: u64) -> Result<u64, AllocError> {
    var list = Vector<u64>.create();          // freed at the end of the function, by its owner
    for (i in 0..n) {
        list.push(i * i)?;
    }
    var total: u64 = 0;
    for (x in list) {
        total += x;
    }
    return Ok(total);
}

fn main() {
    Console.writeLine(sumAll(10)!);
}
```

```text
285
```

There is no `deinit`: the vector is destroyed when its owner goes out of scope, on every path, including the early
return of `?`. Another allocator goes in the type, `Vector<u64, A>` for an allocator type `A`. An arena works the Zig
way instead: `arena.allocArray(n, 0u64)` returns a slice that borrows the arena, and everything is freed with it.

## What changes

**Lifetimes are checked.** Returning a slice of a local array, keeping a pointer into an `ArrayList` across an
`append`, or using an arena allocation after `arena.deinit()`: each is a compile error in CeeSafe, with a message naming
the borrow. In Zig these compile and fail at run time, if they are noticed at all.

**Destruction is automatic.** A struct with a `drop` block, or that owns a box, vector or file, is destroyed exactly
once when its owner goes away. Ownership moves on assignment, so there is no double free to avoid by convention.

**Errors carry types.** An error is any type, usually an enum with payloads (`ParseError.BadDigit(c)`), not a global
error set. `?` requires the function's error type to match; there are no implicit error-set unions.

**Generics, not `comptime`.** CeeSafe's generics are type parameters checked once, for every type argument, against the
bounds you declare. There is no compile-time evaluation of arbitrary code, no `@typeInfo` and no reflection. Constants
are evaluated at compile time; functions are not.

**Safety is not a build mode.** There is no ReleaseFast. Overflow and bounds checks are always on, and the compiler
removes those it proves cannot fail.

## Things you will miss

`comptime`, `@cImport` of whole headers, packed structs and arbitrary-width integers (`u3`), SIMD vectors, inline
assembly, the cross-compiling toolchain in one binary, and Zig's build system. CeeSafe relies on a C compiler for
code generation and cross compilation.
