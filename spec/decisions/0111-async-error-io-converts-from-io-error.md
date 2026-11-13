# ADR 0111 — `AsyncError.Io` converts from `IoError`; `main` is not async

- Status: **Accepted** by the owner, 2026-11-13 ("Fix those new gaps too"), implemented on branch `fix/gaps2-a`; released in ceesafec
  0.42.0. Raised by the book (Decision 0011): chapter 17's Listings 17-27 and 17-29 each carried a match whose
  only work was to wrap an error, and its first listing could not be written as `async fn main`.
- Scope: one attribute in `lib/core/async.cee`; one refusal in the checker (`src/check/declare.cee`, N0219);
  `spec/type-system.md` §11 and §40.4; `spec/diagnostics.md`; the guide. No change to the runtime or to either
  backend.

## 1. Context

**The conversion.** The operations of a task that wait return `Result<T, AsyncError>`, and `AsyncError` has
`Io(IoError)` for the kernel's errors. The socket calls that do not wait (`AsyncTcpListener.bind`, `port`,
`shutdownWrite`) return `Result<T, IoError>`, as their blocking forms do. A function that uses both kinds and returns
`Result<_, AsyncError>` could not write `?` on the second kind: N0261, since `AsyncError` had no `@from` variant
(§17.4, ADR 0075 §2.3). Each such call needed

```ceesafe
let listener = match (AsyncTcpListener.bind("127.0.0.1", 0)) {
    Ok(l) => l,
    Err(e) => { return Err(AsyncError.Io(e)); }
};
```

The variant that holds the error exists, and holds nothing else: the conversion `?` would make is the only one there is.

**`async fn main`.** The specification did not mention it. `check` accepted it, the interpreter ran it (and exited
with 0 whatever an `-> i32` main returned), and `build` wrote C that did not compile ("call to undeclared function
'ceeF_main'"). Three tools, three answers.

## 2. Decision

2.1 **`AsyncError.Io` is `@from`.** In a function that returns `Result<_, AsyncError>`, `e?` on a
`Result<_, IoError>` returns `Err(AsyncError.Io(error))`. Nothing else changes: `AsyncError` keeps its three
variants, and an enum of the program with `@from Io(IoError)` and `@from Async(AsyncError)` converts as before
(`?` looks for a variant of the function's own error enum whose payload is the error's type).

2.2 **`main` cannot be async** (N0219, with the other rules for `main`'s signature, §11):

```text
main.cee:1:10: error[N0219]: 'main' cannot be async: no task calls it; write 'fn main()' and run the async function with 'Async.run(…)' (type-system.md §40.4)
```

A call of an async fn is a task, which runs only when something awaits or runs it (§40.1). The program's entry is a
plain call, so somebody must choose the executor (`Async.run`, `Async.runWorkers(n, …)`, ADR 0038) and say what an
error of the first task does to the exit code. That somebody is `fn main()`, in two lines the program can read.

## 3. Alternatives

- **Support `async fn main`**, as `Async.run(main())` with the result as the exit code. It hides two choices the
  language otherwise makes the program write: which executor (one thread, or workers), and what `Err` means for a
  main that returns a `Result` (which `main` may not return today, §11). It would also need the entry in both
  backends and in the freestanding profile, where there may be no `Async.run` at all. Refusing costs a program two
  lines and can be relaxed later; supporting it could not be taken back.
- **`@from` for `SendError<T>` too** (the book's Listing 17-27: `await ch.sendAsync(n)?` in a function that returns
  `AsyncError`). Not possible as `@from`: `SendError<T>` is generic, lives in lib/std and carries the value that was
  not sent, and `AsyncError` (lib/core) has no variant that means "the channel is closed". See §4.
- **A general `From` interface.** `@from` is the language's one conversion at `?` (ADR 0075); a second mechanism is
  not needed for a variant that already exists.

## 4. Consequences

- The matches around `bind`, `port` and `shutdownWrite` in async functions that return `AsyncError` become `?`.
- A program that declared `async fn main` and only ever ran in the interpreter now gets N0219. No such program is
  in the corpus, the guide or the book.
- **Open: `sendAsync`'s error.** `Result<void, SendError<T>>` still needs a match where the function returns
  `AsyncError`. Two designs are possible and they differ for existing programs, so neither is taken here: a new
  variant `AsyncError.Closed` with `SendError<T>.reason() -> AsyncError` (every exhaustive `match` on `AsyncError`
  must then gain an arm), or `sendAsync` returning `Result<void, AsyncError>` and dropping the value (it would no
  longer give the value back, which ADR 0032 §3.3 promises).
- Tests: `run-pass/async-error-from-io.cee`, `check-fail/async-main.cee`.
