# CeeSafe for Go programmers

Go and CeeSafe share some taste: a small language, errors as values, `defer`, a canonical formatter, packages as
directories, and fast feedback. They differ in how memory is managed. Go has a garbage collector and a runtime; CeeSafe
has neither, and tracks ownership at compile time instead, which is what lets it run in a kernel.

## At a glance

| Go | CeeSafe | Notes |
|---|---|---|
| `x := 1`, `var x int32` | `let x = 1;`, `var x: i32 = 0;` | `let` cannot be reassigned |
| `func f(a int) (int, error)` | `fn f(a: i32) -> Result<i32, E>` | |
| `if err != nil { return err }` | `?` | |
| `panic`, `recover` | panics abort; no recovery | |
| `defer` | `defer` | runs at the end of the block, not the function |
| slices `[]T` (growable) | `Vector<T>` (owns, grows) and `Slice<T>` (borrows) | a slice cannot outlive what it views |
| maps | not built in yet | `SlotMap` for handles |
| `nil` | `null`, only for optionals (`T?`) | |
| interfaces, satisfied implicitly | interfaces, implemented explicitly with `impl` | generic bounds, resolved at compile time |
| generics `[T any]` | `<T>` | |
| goroutines, channels | async tasks in a scope, `Channel<T>` (`sendAsync`, `recvAsync`; `send`, `recv` between threads) | a task never outlives its scope; see [Async tasks](../async.md) |
| `gofmt` | `ceesafec fmt` | one canonical style, no options |
| `go.mod`, `go.sum` | `ceesafe.toml`, `ceesafe.lock` | path and git dependencies, pinned by commit and hash |
| `go vet`, the race detector | the compiler: ownership, borrows, context effects | |

## The same program

```go
package main

import (
    "errors"
    "fmt"
)

func divide(a, b int) (int, error) {
    if b == 0 {
        return 0, errors.New("division by zero")
    }
    return a / b, nil
}

func average(xs []int) (int, error) {
    sum := 0
    for _, x := range xs {
        sum += x
    }
    return divide(sum, len(xs))
}

func main() {
    avg, err := average([]int{3, 5, 10})
    if err != nil {
        fmt.Println(err)
        return
    }
    fmt.Println(avg)
}
```

```ceesafe
enum MathError {
    DivisionByZero,
}

fn divide(a: i64, b: i64) -> Result<i64, MathError> {
    if (b == 0) {
        return Err(MathError.DivisionByZero);
    }
    return Ok(a / b);
}

fn average(xs: Slice<i64>) -> Result<i64, MathError> {
    var sum: i64 = 0;
    for (x in xs) {
        sum += x;
    }
    return divide(sum, truncate<i64>(xs.length));    // usize to i64 could lose bits: say so
}

fn main() {
    let values = [3i64, 5, 10];
    match (average(values[..])) {
        Ok(avg) => { Console.writeLine(avg); }
        Err(MathError.DivisionByZero) => { Console.writeLine("division by zero"); }
    }
}
```

```text
6
```

`xs.length` is a `usize`, and CeeSafe has no implicit conversions: `as` converts only where nothing can be lost, so a
`usize` becomes an `i64` through `truncate<i64>`.

## What changes

**No garbage collector.** Every value has one owner and is destroyed when the owner goes away. Where Go lets any
number of pointers share an object and keeps it alive, CeeSafe lets one owner keep it and others *borrow* it for a while,
and checks that no borrow outlives the object.

**Allocation is visible and fallible.** Values live on the stack unless the code asks for a box, a vector or a string,
and each of those returns a `Result`: out of memory is an error, not a crash.

**Errors propagate with `?`** instead of `if err != nil`. The error type is part of the signature, usually an enum.

**Interfaces are explicit and static.** A type implements an interface with an `impl` block, and interfaces are used
as generic bounds, resolved at compile time. There is no interface value holding "some type", and no type switch.

**Overflow is checked.** Integer arithmetic that overflows stops the program; Go wraps silently.

## Things you will miss

Goroutines that outlive their caller, `select` over many channels (`Async.race` takes two tasks), maps, interface values and type switches, struct embedding, the standard library,
and Go's fast tooling for large codebases.
