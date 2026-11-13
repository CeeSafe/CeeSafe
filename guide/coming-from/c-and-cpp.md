# CeeSafe for C and C++ programmers

CeeSafe compiles to C, calls C directly, and is designed to live inside C codebases, kernels included. What it adds is a
compiler that tracks who owns each piece of memory and who is looking at it, and rejects the programs that would use
it wrongly. What it removes is undefined behaviour.

## At a glance

| C / C++ | CeeSafe | Notes |
|---|---|---|
| `int32_t x = 1;` | `let x: i32 = 1;` (or `var` to change it) | |
| `T *`, `const T *` | `borrow mut T`, `borrow T` in safe code; `*mut T`, `*T` in `unsafe` | references are never null and never dangle |
| `malloc` / `free`, `new` / `delete` | `Box.create(v)`; freed by its owner | allocation failure is a `Result`, not `NULL` or an exception |
| `std::unique_ptr<T>` | `Box<T>` | |
| `std::vector<T>` | `Vector<T>` | growing it while an element is referenced is a compile error |
| `std::string`, `std::string_view` | `String`, `StringView` | UTF-8 only |
| `std::optional<T>` | `T?` | |
| destructors, RAII | `drop { … }` blocks; destruction at scope end | moves leave nothing behind to destroy twice |
| `std::move(x)` | assignment moves by default; `move x` to end a use early | using a moved value is a compile error |
| exceptions, `errno`, return codes | `Result<T, E>` and `?` | |
| `assert`, `abort` | `assert(cond)`, `panic(message)`; the built-in checks panic too: bounds, overflow, unwrap | always on; `debugAssert` is the one that release builds leave out |
| templates | generics with interface bounds | checked once, before use; no SFINAE |
| `#include`, the preprocessor | packages and `import` | no macros |
| `#if CONFIG_DEBUG` … `#else` … `#endif` | `if (cfg("CONFIG_DEBUG")) { … } else { … }` | both branches are checked; the C compiler keeps one. See [Unsafe code and C](../unsafe-and-c.md) |
| `struct` layout | the same as C's, in declaration order | `@repr(C)` to cross the boundary |
| `volatile` | `Volatile<T>`, `p.readVolatile()` | |
| `-fsanitize=address,undefined` | needed only for `unsafe` code; `ceesafec run --interp` checks it | |

## The same program

```cpp
#include <cstdio>
#include <vector>

int main() {
    std::vector<int> v = {1, 2, 3};
    int &first = v[0];
    v.push_back(4);          // may reallocate: `first` now dangles
    std::printf("%d\n", first);
}
```

In C++ this compiles and has undefined behaviour. In CeeSafe it does not compile:

<!-- ceesafe: fail N0241 -->
```ceesafe
fn main() {
    var v = Vector<i32>.create();
    v.push(1)!;
    let first = borrow v[0];
    v.push(4)!;
    Console.writeLine(first);
}
```

```text
main.cee:5:5: error[N0241]: cannot borrow 'v' as mutable while 'v[0]' is borrowed
  |
5 |     v.push(4)!;
  |     ^
  = note: 'v[0]' is borrowed here (4:5)
  = note: the borrow is used later here (6:5)
```

The fixes are the ones you would reach for anyway: copy the value (`let first = v[0];`), or take the reference after
the last push.

## No undefined behaviour

Every operation that is undefined in C is defined in CeeSafe, by a check or by a rule:

| Undefined in C | In CeeSafe |
|---|---|
| signed overflow | stops the program (`wrappingAdd` and friends where wrapping is meant) |
| out-of-bounds access | stops the program |
| use after free, double free | compile errors |
| reading uninitialised memory | a compile error |
| null dereference | there are no null references; optionals must be checked |
| shift by the width or more | stops the program |
| unsequenced modifications | evaluation order is left to right |
| strict aliasing violations | the generated C never relies on them |

The generated C is written so that it has no undefined behaviour itself, and is warning-free under strict flags, so
the C compiler's optimiser has nothing to exploit.

## Working with an existing C codebase

- Call C functions from an `extern "C"` block, inside `unsafe`.
- Export CeeSafe functions with `@export`; `ceesafec` writes `ceesafe_exports.h` for your C code to include.
- Use your codebase's own structs from its headers (`@header`, `@ctype`) instead of re-declaring them: the C compiler
  checks every field CeeSafe uses.
- Build a freestanding object with `--profile freestanding --emit obj` and link it with your build.

See [Unsafe code and C](../unsafe-and-c.md) and [Kernel code](../kernel.md).

## Things you will miss

Macros, inline assembly, unions of your own (enums with payloads replace tagged unions; a C union is imported from
its header), bit fields, pointer arithmetic in safe code, varargs functions of your own (C's can be called), goto,
and in C++, classes with inheritance (an interface and `dyn` give virtual calls), operator overloading, overloads,
exceptions, and `constexpr` evaluation.
