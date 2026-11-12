---
title: Error handling
summary: CeeSafe has no exceptions - a failure is either a panic, which is a bug and stops the program, or a Result value that the caller must deal with. This chapter teaches both, allocation failure, and how to choose.
assumes: [ch06-00-enums, ch08-00-collections]
keywords: [error, panic, Result, Ok, Err, "?", "!", AllocError, exception]
---

# Error handling

**This chapter teaches** what a CeeSafe program does when something goes wrong. It assumes [enums](./ch06-00-enums),
because an error is an enum value, and [collections](./ch08-00-collections), because growing one can fail.

CeeSafe has no exceptions. There is no `throw`, no `try` and no `catch`, and a function never leaves by a path that
its signature does not show. Instead the language has two kinds of failure, and it keeps them apart.

- **A panic** is for a bug: an index past the end of an array, an integer that overflows, taking the value out of
  an optional that has none. The program prints what happened and where, and stops. Nothing can catch a panic.
- **A `Result`** is for a failure that a correct program must expect: text that is not a number, a file that is
  not there, memory that has run out. The function returns a value that is either `Ok` with the answer or `Err`
  with the reason, and the caller decides what to do with it.

The difference is who is at fault. A panic means the program is wrong and a person must change it. An `Err` means
the program is right and the world did not cooperate: the user typed a letter, the disk is full.

Two short operators make `Result` values light to work with. `?` passes an error on to the caller. `!` says "this
cannot fail here", and panics if it does. Almost every line that can fail ends with one of them, so every place
where a function can fail is visible in the source.

One kind of failure deserves its own section. In CeeSafe, running out of memory is not a crash: every operation
that allocates, such as `push` on a vector, returns a `Result`. That is why programs in earlier chapters write
`names.push("ada")!;`.

## The sections

- [9.1 Panics](./ch09-01-panics): what panics, what the program prints, its exit status, and when to call `panic`
  yourself.
- [9.2 `Result`, `?` and `!`](./ch09-02-result): returning and matching errors, passing them on, an error type of
  your own, and converting one error type to another.
- [9.3 Allocation failure is a value](./ch09-03-allocation-failure): why `push` returns a `Result`, how to handle
  it or decline to, and how to test the path where memory runs out.
- [9.4 To panic or not](./ch09-04-to-panic-or-not): how to choose between the two for a failure in your own code.

After this chapter, [chapter 10](./ch10-00-generics) covers generics, which is what the `T` and `E` in
`Result<T, E>` are.
