# ADR 0128 — A send's error under `?`: `AsyncError.Closed` and `@into`

- Status: **Accepted** by the owner, 2026-11-13 ("I go with your suggestions"), implemented on branch `impl/0128`;
  not yet released. It completes ADR 0111, whose §4 left `sendAsync`'s error open. Raised by the book (Decision 0011;
  gaps2 A3): chapter 17's Listing 17-27 carries a four-line match whose only work is to turn a `SendError<i32>` into
  an `AsyncError`.
- Scope: one variant in `lib/core/async.cee`, one method in `lib/std/channel.cee`; one attribute, `@into`, read by
  the checker (`src/check/effects.cee`, `inherent.cee`) and used by `?` (`src/check/options.cee`, `structs.cee`);
  the lowering of `?` (`src/nir/nir_builder.cee`), which both backends share; the test harness's match
  (`src/host/test_runner.cee`); `ceesafec doc`, which now lists the public functions of an enum's `impl` blocks
  (`src/documentation/doc_generator.cee`: `SendError.reason` is the library's first); `spec/type-system.md` §17.4,
  §28.1 and §40.4; `spec/diagnostics.md`; `spec/api/`; the guide. No change to the runtime, no new diagnostic code.

## 1. Context

A task's send returns the value it could not send (ADR 0032 §3.3):

```ceesafe
public enum SendError<T> {
    Closed(T),
    Cancelled(T),
}
public async fn sendAsync(borrow this, value: T) -> Result<void, SendError<T>>
```

Everything else a task waits for returns `Result<_, AsyncError>`, so that is what an async function returns, and
there `await ch.sendAsync(v)?` was N0261: `?` converts an error through an `@from` variant of the function's error
enum (§17.4, ADR 0075 §2.3), and `AsyncError` has none for `SendError<T>`. It cannot have one:

- an `@from` variant holds the error it converts from, and `AsyncError` is not generic, so it cannot hold a
  `SendError<T>`;
- `AsyncError` is lib/core's and `SendError` lib/std's;
- and one variant would not do: `SendError.Cancelled` must become `AsyncError.Cancelled`, and only
  `SendError.Closed` something new.

So each send in such a function was

```ceesafe
match (await ch.sendAsync(n)) {
    Ok(_) => {}
    Err(_) => { return Err(AsyncError.Cancelled); } // or TimedOut: there was no honest variant for "closed"
}
```

ADR 0111 §4 recorded two designs: keep the value in `SendError<T>` and add `AsyncError.Closed` with a conversion, or
make `sendAsync` return `Result<void, AsyncError>` and drop the value. The owner chose the first: ADR 0032 §3.3
stands, a failed send gives the value back.

## 2. Decision

2.1 **`AsyncError.Closed`.** `AsyncError` gains a fourth variant, after the three it had (their numbers do not
change):

```ceesafe
public enum AsyncError {
    Cancelled,
    TimedOut,
    @from Io(IoError),
    Closed,
}
```

`Closed` means: **the other side of this channel, or this endpoint, was closed, so the operation can never
succeed.** It is not channel-only: an operation of the library that waits on something another part of the program
closed (a listener shut down while a task waits in `accept`, say) may return it. It is not the end of a stream (a
receive from a closed, drained channel is still `Ok(null)`, a read at the end of a connection still `Ok(0)`), and it
is not an error of the kernel (those stay `Io`).

2.2 **`SendError<T>.reason()`** drops the value and gives the reason:

```ceesafe
impl<T> SendError<T> {
    @into
    public fn reason(this) -> AsyncError   // Closed(_) => AsyncError.Closed, Cancelled(_) => AsyncError.Cancelled
}
```

It takes the error by value, so the value in it is destroyed when `reason` returns, once, by the ordinary rule for a
parameter.

2.3 **`@into`: the conversion `?` makes, declared on the error's side.** A method of a struct, or of an enum's
`impl E { … }` block (ADR 0107), may be marked `@into`. It must have the form `fn name(this) -> F`: the value as
its receiver and no other parameter, a result type, no type parameters of its own (those of its type are fine), and
neither `async` nor `unsafe`. A type has at most one `@into` method. Anything else is N0290, as for the other
attributes of a method.

§17.4's rule for `e?`, with `e: Result<T, E>` in a function that returns `Result<U, F>`, becomes: `E` is `F`; or
`F` is an enum with an `@from` variant whose payload is `E` (`?` returns `Err(F.V(x))`); or **`E` has an `@into`
method whose result type is `F` (`?` returns `Err(x.name())`)**; N0261 otherwise. They are tried in that order, so
a function's own `@from` variant wins over the error's `@into` method. The call is checked as if it were written:
the method must be visible where the `?` is, as for a call written there, and its effects are effects of the
function that holds the `?` (N0295 in an `@effects()` function, if the method allocates).

The conversion is still one step, and still only `?` makes it:

- in a function that returns `Result<_, AppError>`, where `AppError` has `@from Async(AsyncError)`,
  `await ch.sendAsync(v)?` is N0261: `SendError<T>` to `AsyncError` to `AppError` is two steps. The message says
  so ("'SendError<String>' converts into 'AsyncError' only (its '@into' method 'reason'), and '?' converts in one
  step"). Such a function writes the match, or gives `AppError` an `@from` variant for the `SendError` it sends;
- `return await ch.sendAsync(v);` in a function that returns `Result<void, AsyncError>` is N0205, as before.

2.4 **A full channel does not convert.** `SendError` has no "full" case: `sendAsync` waits while the channel is
full. The sends that do not wait for a task, `trySend` (full or closed) and the blocking `send` (closed), return
`Result<void, T>`: the value itself, with no reason attached. `?` on them in a function that returns
`Result<_, AsyncError>` stays N0261 (unless `T` is itself a type that converts, by the rules above, as it always
did). That is deliberate: a full channel is not a failure of the channel, it is an answer ("not now") that the
caller of `trySend` asked for and must read; turning it silently into `Closed` would report a healthy channel as a
dead one, and into `Cancelled` would be false. A program that wants `Closed` there tests `ch.isClosed()` and says
so.

## 3. Alternatives

- **`sendAsync` returns `Result<void, AsyncError>`** and drops the value (ADR 0111 §4's second design). Rejected by
  the owner: ADR 0032 §3.3 promises the value back, and a program that retries elsewhere needs it.
- **A generic `@from`**: a variant of `AsyncError` "convertible from `SendError<T>` for every `T`". It needs syntax
  for a variant that converts from a type it does not hold, lib/core naming a lib/std type (or `SendError` moving to
  lib/core), and it still maps a whole type to one variant, where `SendError`'s two cases need two. A method on the
  error's side says which case becomes what in ordinary code, and drops the value by the ordinary rule.
- **`reason()` alone, without `@into`**: `Result` has no `mapErr`, and CeeSafe has no closures-for-errors idiom, so
  the program would still write the match; it would be one line shorter.
- **A method found by its name** (as `for` finds `next`, ADR 0074 §2.8). A conversion that happens at every `?`
  should be visible where it is declared; `@from` set that style.
- **Several `@into` methods per type**, one per result type. Nothing needs it yet, and with generic result types
  two could be the same type in one instance; one per type cannot be ambiguous and can be relaxed later.
- **Chaining `@into` then `@from`** (two steps). ADR 0075 chose one step so that a reader of `?` can find the
  conversion by looking at two declarations; this keeps that.

## 4. Implementation

- `@into` is read with a method's other attributes (`readIntoAttribute`): the shape is checked on the syntax, and
  the function is recorded (`Checker.intoMethods`).
- `checkTry`: after `fromVariant` finds nothing, `intoMethod(E, F)` looks for the recorded method in this instance
  of `E` and compares its result type with `F`. The call is checked with `checkUserCallWith` (visibility, profile,
  the first-call position of an instance's method), and the callee is recorded for the `?` expression in
  `Checker.tryConversions` (bit 32 set: a method, not a variant).
- `lowerResultAccess` (NIR): on `Err`, the error moves out of the result into a call of the method, and the call's
  value into the function's `Err`. The interpreter and the C backend run the same NIR, and the move checker, the
  borrow checker and the effect checker see an ordinary call.

## 5. Compatibility

**A new variant breaks exhaustive matches.** A `match` that names `AsyncError.Cancelled`, `AsyncError.TimedOut` and
`AsyncError.Io(_)` and has no `_` arm is now N0256 ("this match does not cover Err(AsyncError.Closed)" or
"… AsyncError.Closed"). The owner accepted this. The one-line fix is an arm:

```ceesafe
AsyncError.Closed => { … }      // or, in a match on a Result:  Err(AsyncError.Closed) => { … }
```

(or a `_` arm, for a program that treats the errors it does not name alike). In the repository this was one corpus
test (`run-pass/async-tcp.cee`) and the harness `ceesafec test` generates for a test that returns
`Result<void, AsyncError>` (it now reports "closed"). No operation of the library returned `Closed` before, and
only `SendError.reason` produces it now, so no existing program sees a different value at run time.

`@into` is a new attribute name: `@into` was N0290 (unknown attribute) everywhere before, so no program that
compiled changes meaning. `into` is not a keyword. A program that already wrote the match around `sendAsync` is
unchanged; `?` is accepted where it was N0261.

## 6. Tests

- `run-pass/async-send-error-into.cee` (native and `--interp`, `--check-leaks`): a closed channel under `?` with a
  `String` payload; an instance of a generic function; a cancelled send; a struct that counts its drops (the unsent
  value is destroyed exactly once); the value kept by a match; a program's own `@into` method in a function that is
  not async.
- `check-fail/send-error-into.cee`: `trySend(v)?` and `send(v)?` (N0261); two steps (N0261); another error type
  (N0261); `return` of the send's result (N0205); every wrong shape of an `@into` method, a second one in a struct
  and in an enum, `@into` on a free function (N0290); an accepted method does not convert into another type (N0261).
- `check-fail/into-effects.cee`: the method's effects in an `@effects()` function (N0295).
- The existing channel tests are unchanged.
