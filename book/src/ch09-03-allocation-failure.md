---
title: Allocation failure is a value
summary: Every operation that allocates memory, such as push, append and Box.create, returns a Result with AllocError. How to pass it on with ?, stop with !, and test the out-of-memory path with FailingAllocator and with ceesafec test --sweep alloc.
assumes: [ch08-01-vectors, ch08-02-strings, ch09-02-result]
keywords: [AllocError, OutOfMemory, push, append, withCapacity, Box.create, FailingAllocator, failAfter, "--sweep alloc", allocation, "out of memory"]
diagnostics: [N0234, N0402]
specification: ["type-system.md §17.1", "type-system.md §18.1", "tooling.md §10.2"]
---

# Allocation failure is a value

**This section teaches** why adding to a vector or a string returns a `Result`, the two ways to deal with it, and
how to test what a program does when memory runs out. It assumes [vectors](./ch08-01-vectors),
[strings](./ch08-02-strings) and [`Result`, `?` and `!`](./ch09-02-result).

A vector keeps its elements in memory that it asks the system for while the program runs. Asking for memory is
called **allocation**, and it can fail: the machine, or the limit set for the process, has no more to give.

In many languages that failure ends the program, or raises an exception from a line that does not look as if it
could fail. In CeeSafe it is an ordinary error value. Every operation that may allocate returns
`Result<…, AllocError>`.

`AllocError` is a built-in enum with one variant, `AllocError.OutOfMemory`.

| Operation | Returns |
|---|---|
| `v.push(x)`, `v.insert(at, x)`, `v.reserve(extra)` | `Result<void, AllocError>` |
| `Vector<T>.withCapacity(n)` | `Result<Vector<T>, AllocError>` |
| `s.append(text)`, `s.appendNumber(n)` | `Result<void, AllocError>` |
| `String.withCapacity(n)`, `String.from(text)` | `Result<String, AllocError>` |
| `s.clone()`, `v.clone()` | `Result<String, AllocError>`, `Result<Vector<T>, AllocError>` |
| `Box.create(value)` | `Result<Box<T>, AllocError>` |

`Vector<T>.create()` and `String.create()` make an empty collection and allocate nothing, so they return the
collection itself. A `Box` owns one value in allocated memory; a later chapter covers it.

::: tip Rule
An operation that allocates returns `Result<…, AllocError>`. Running out of memory is an `Err`, not a crash, and
the caller chooses what it means.

*Specification: `type-system.md` §17.1.*
:::

This matters for the programs CeeSafe is made for. A kernel, a server that must stay up, or a device with a few
kilobytes of memory cannot stop because one request was too large. They need to refuse that request and carry on,
and they can only do that if the failure arrives as a value.

## Passing it on with `?`

A function that builds something returns `Result<…, AllocError>` itself and ends each allocating call with `?`.

<span class="listing-caption"><strong>Listing 9-19</strong>, <code>main.cee</code>: <code>squares</code> passes an allocation failure to its caller, which handles it.</span>

```ceesafe
fn squares(n: i32) -> Result<Vector<i32>, AllocError> {
    var v = Vector<i32>.create();
    for (i in 0..n) {
        v.push(i * i)?;
    }
    return Ok(v);
}

fn main() {
    match (squares(5)) {
        Ok(v) => { Console.writeLine(v[4]); }
        Err(AllocError.OutOfMemory) => { Console.writeLine("out of memory"); }
    }
}
```

```text
16
```

If a `push` fails, `?` returns `Err(AllocError.OutOfMemory)` from `squares`. On the way out the half-built vector
`v` is destroyed and its memory is given back, as for any `return`. The caller gets either a whole vector or an
error, never part of one.

This is the form to use in a library, or in any function that someone else will call: it leaves the decision to
the caller.

When the function has an error enum of its own, give the enum a variant for the allocation failure and mark it
`@from`, as [section 9.2](./ch09-02-result#converting-one-error-type-to-another) shows:
`@from Memory(AllocError)`. Then the same `?` works.

## Stopping with `!`

A command-line tool or an example has nothing better to do without memory than to stop. Such a program writes `!`.

<span class="listing-caption"><strong>Listing 9-20</strong>, <code>main.cee</code>: A program that would rather stop than handle a failed allocation.</span>

```ceesafe
fn main() {
    var line = String.from("total: ")!;
    line.appendNumber(6 * 7)!;
    Console.writeLine(line.view());
}
```

```text
total: 42
```

`String.from` makes a string that owns a copy of the text, and `appendNumber` adds to it. Both may allocate. If
either fails, the program panics with `unwrapped an Err value` and the line it happened on. That is why
programs throughout this book write `names.push("ada")!;`: the `!` is the program's decision about memory, in one
character, at each place where memory is asked for.

::: tip Rule
End a call that allocates with `?` in a function that returns `Result<…, AllocError>`, or with `!` in a program
that stops when memory runs out. Do not leave it bare.
:::

## Testing the failure path

Code that handles an error is code like any other, and it can be wrong. But a test machine rarely runs out of
memory. CeeSafe gives you two ways to make an allocation fail on purpose.

### `FailingAllocator`

A vector's full type is `Vector<T, A>`, where `A` is the **allocator**: where its memory comes from. `Vector<T>`
alone means the system's heap. `FailingAllocator` is a built-in allocator that takes memory from the same heap,
but can be told to refuse.

- `FailingAllocator.failAfter(n)`: the next `n` allocations through it succeed, and every later one fails.
- `FailingAllocator.reset()`: allocations succeed again.

<span class="listing-caption"><strong>Listing 9-21</strong>, <code>main.cee</code>: The first run is allowed one allocation and fails at the second. The second run has no limit.</span>

```ceesafe
fn squares(n: i32) -> Result<Vector<i32, FailingAllocator>, AllocError> {
    var v = Vector<i32, FailingAllocator>.create();
    for (i in 0..n) {
        v.push(i * i)?;
    }
    return Ok(v);
}

fn report(n: i32) {
    match (squares(n)) {
        Ok(v) => { Console.writeLine(v.length); }
        Err(AllocError.OutOfMemory) => { Console.writeLine("out of memory"); }
    }
}

fn main() {
    FailingAllocator.failAfter(1);
    report(100);
    FailingAllocator.reset();
    report(100);
}
```

```text
out of memory
100
```

A vector does not allocate on every `push`. It allocates when it is full, and takes room for several elements at
a time. A hundred elements need more than one allocation, so the first run fails part of the way through, and
`squares` returns the error and frees what it had.

The countdown is one for the whole process. Call `reset()` when the test is over.

### `ceesafec test --sweep alloc`

`FailingAllocator` tests one failure that you choose. The test runner can try them all. A function marked `@test`
is a test, and `ceesafec test` runs a package's tests; [chapter 11](./ch11-00-testing) covers them.

This package is in `book/projects/ch09-squares/`.

<span class="listing-caption"><strong>Listing 9-22</strong>, <code>src/main.cee</code>: A function that allocates, and a test of it.</span>

```ceesafe
fn squares(n: i32) -> Result<Vector<i32>, AllocError> {
    var v = Vector<i32>.create();
    for (i in 0..n) {
        v.push(i * i)?;
    }
    return Ok(v);
}

@test
fn makesTen() -> Result<void, AllocError> {
    let v = squares(10)?;
    assert(v.length == 10);
    return Ok();
}

fn main() {
    match (squares(10)) {
        Ok(v) => { Console.writeLine(v.length); }
        Err(AllocError.OutOfMemory) => { Console.writeLine("out of memory"); }
    }
}
```

```text
10
```

Run the test with the sweep:

```sh
ceesafec test --sweep alloc
```

It prints:

```text
test makesTen ... ok

ceesafec test: 1 passed, 0 failed, 0 sweep failure(s) in 3 sweep run(s)
```

The test passes once in the ordinary way. It made three allocations, so the sweep runs it three more times: with
the first allocation failing, then the second, then the third. In each of those runs the test returns
`Err(AllocError.OutOfMemory)`, which is the correct answer to a failed allocation. A sweep run fails if the
program crashes, or ends with memory it never gave back.

The sweep uses the ordinary `Vector<i32>`: there is no need to write `FailingAllocator` in the code. It builds the
tests as executables, so it needs a C compiler and does not run with `--interp`.

## Mistakes

::: warning Mistake: using the `Result` as if it were the collection
`Vector<T>.withCapacity(n)` allocates, so it returns a `Result`, not a vector. The vector is inside it.
:::

<span class="listing-caption"><strong>Listing 9-23</strong>, <code>main.cee</code>: <code>v</code> is a <code>Result</code>, and a <code>Result</code> has no <code>push</code>. This does not compile.</span>

<!-- ceesafe: fail N0234 -->
```ceesafe
fn main() {
    var v = Vector<i32>.withCapacity(10);
    v.push(1)!;
}
```

```text
main.cee:3:7: error[N0234]: 'Result<Vector<i32>, AllocError>' has no method 'push'; it has 'isOk()', 'isErr()', 'ok()' and 'err()', and 'match', '?' and '!' do the rest (type-system.md §17)
  |
3 |     v.push(1)!;
  |       ^^^^
```

Take the vector out where it is made:

<span class="listing-caption"><strong>Listing 9-24</strong>, <code>main.cee</code>: <code>!</code> after <code>withCapacity</code> gives the vector.</span>

```ceesafe
fn main() {
    var v = Vector<i32>.withCapacity(10)!;
    v.push(1)!;
    Console.writeLine(v.length);
}
```

```text
1
```

::: warning Mistake: a bare `push`
`v.push(1);` with nothing after it compiles. If the allocation fails, the element is not added and the program
carries on with a shorter vector than it believes it has. The compiler warns about the call, and the program
still builds and runs.
:::

<span class="listing-caption"><strong>Listing 9-25</strong>, <code>main.cee</code>: Nothing reads the <code>Result</code> of <code>push</code>. The compiler warns.</span>

<!-- ceesafe: warns N0402 -->
```ceesafe
fn main() {
    var v = Vector<i32>.create();
    v.push(1);
    Console.writeLine(v.length);
}
```

```text
main.cee:3:5: warning[N0402]: the 'Result' of this call is not used, so an error it reports is lost: handle it with '?', '!' or 'match', or write 'let _ = ...;' to discard it
  |
3 |     v.push(1);
  |     ^^^^^^^^^
```

Write `v.push(1)?;` or `v.push(1)!;`. `let _ = v.push(1);` silences the warning and keeps the bug, so it is not
the answer for an allocation. [Section 9.2](./ch09-02-result#mistakes) shows the same mistake with a function of
your own, and `--deny-warnings`, which makes the warning an error.

::: details Coming from Rust
`Vec::push` aborts the process when allocation fails. Here every such method is what Rust calls the `try_` form:
`v.push(x)` is closer to `v.try_reserve(1)` followed by a push, and it returns a `Result`. `v.push(x)!` gives the
behaviour of Rust's `push`.
:::

::: details Coming from C
`push` is a `realloc` whose return value has a type the compiler knows. `?` is the `if (p == NULL) return …;`
that every call needs, including the cleanup of what the function had already built.
:::

::: details Coming from C#, Java, Go, Python or TypeScript
Those languages report this failure with an exception or by ending the process, and code rarely handles it.
CeeSafe has no garbage collector and is meant for programs that must not stop, so the failure is in the type of
every call that allocates.
:::

## Summary

- An operation that allocates (`push`, `append`, `withCapacity`, `String.from`, `clone`, `Box.create`) returns `Result<…, AllocError>`.
  `AllocError` has one variant, `OutOfMemory`.
- In a function that returns `Result<…, AllocError>`, end the call with `?`. What the function had built is
  destroyed on the way out.
- In a program that stops when memory runs out, end the call with `!`.
- A bare `v.push(1);` loses the failure. The compiler warns about it (`N0402`).
- `FailingAllocator.failAfter(n)` makes a later allocation through `FailingAllocator` fail, for a test of one
  failure path.
- `ceesafec test --sweep alloc` runs each passing test again once for each allocation, with that allocation
  failing.

Next: [to panic or not](./ch09-04-to-panic-or-not), on choosing between a panic and a `Result` in your own code.
