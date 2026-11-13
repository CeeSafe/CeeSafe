# ADR 0115 — The expected type under `!` and `?`, and a string value as text

- Status: **Accepted** by the owner, 2026-11-13 ("Fix those new gaps too"), implemented on branch `fix/gaps2-b`; released in ceesafec
  0.42.0. Raised by the writers of the book's chapters 13 to 22 (gaps B2 and B4 of the second round).
- Scope: the checker (`src/check/options.cee`, `src/check/expr.cee`, `src/check/references.cee`);
  `spec/type-system.md` §17.4 and §23.2; the guide. No new diagnostic code, no new syntax, no change to the libraries,
  the runtime or the interpreter.

## 1. Context

Measured on ceesafec 0.40.0 at `d97381f`, 2026-11-13:

| Program | Result | With the type written twice |
|---|---|---|
| `let d: Box<i64> = Box.create(5)!;` | N0205 "expected 'Box<i64>', found 'Box<i32>'" | `Box.create(5i64)!` |
| `let a: String<FailingAllocator> = String.from("a")!;` | N0205 | `String<FailingAllocator>.from("a")!` |
| `let v: Vector<u8, FailingAllocator> = Vector.withCapacity(8)!;` | N0260 "the element type of 'Vector.withCapacity(...)' is unknown here" | `Vector<u8, FailingAllocator>.withCapacity(8)!` |
| `Console.writeLine(make().name);` (`name: String`) | N0207 "can print … string views, not 'String'" | `let n = make(); Console.writeLine(n.name);` |
| `Console.writeLine(opt ?? String.create());` | N0207 | a local |
| `show(String.from("x")!)` with `fn show(text: StringView)` | accepted by `check`; **the C did not compile** ("unknown type name 'ceeA18446744073709551615_u8'") and the interpreter did not finish | a local |

The functions that make a box, a vector or a string already read an expected `Result<T, AllocError>`
(`let r: Result<Box<i64>, AllocError> = Box.create(5);` held an i64). `e!` and `e?` did not pass their own expected
type on, so the commonest way to write these calls lost it. The last row is a bug of the same family as the
printing rows: the text of a `String` that is not a place was "copied to a temporary", which an unsized `str` cannot
be.

## 2. Decision

### 2.1 An expected box, vector or string type passes through `!` and `?`

Where `e!` or `e?` is checked against an expected type `T` that is a `Box`, `Vector` or `String` type, and `e` is a
call of a type's function or of a method (`T.f(…)`, `x.f(…)`), `e` is checked with the expected type
`Result<T, AllocError>`. For such an `e`, an expected `Box<U>` reaches `!`/`?` whole (elsewhere an expected box
gives its contents' type as the hint, §18.2).

So `Box.create(v)!` checks `v` against the expected contents, and `String.from(…)!`, `String.withCapacity(n)!`,
`Vector.withCapacity(n)!` and `Vector.create()` under them take the expected element and allocator types, exactly
as they do under `let r: Result<T, AllocError> = …`.

It is a hint and never a conversion: the value of `e!` is still coerced to `T` by the ordinary rule, so a call whose
result is another type is N0205 as before (`let b: Box<i64> = makesBoxOfI32()!`), and a literal that does not fit
the expected contents is N0206 (`let a: Box<u8> = Box.create(300)!`). Without an expected type nothing changes
(`let b = Box.create(5)!` is a `Box<i32>`). Only these three kinds of type are passed, because only their
constructors read the expectation; an expected `i32` or an expected struct gives `e` no expected type, as before.

### 2.2 A `String` value prints, and lends its text, like a `String` place

`Console.write`/`writeLine` print a `String` that is not a place: a call's result, `a ?? b`, a field of a call's
result, a field reached through a call that returns a reference. A `String` value given for a `StringView`
parameter, and a vector value given for a `Slice<T>` parameter, lend their contents the same way.

The value is held in a temporary to the end of the statement, where it is destroyed once (§14.5), and its text or
elements are lent from that temporary (ADR 0073 §2.1, the rule a method receiver has: `make().name.view()`). A view
of it cannot outlive the statement (N0243), as for `make().view()`.

**Why it is sound.** No new kind of loan is made: the lending is `BorrowPath`, the borrow of a path from a temporary
that method receivers have used since ADR 0073, with the same lifetime (the statement) and the same checks. The
temporary owns the string, so the string is destroyed exactly once at the statement's end, and not at all if a
panic comes first (as every temporary). The tests run under `--check-leaks`.

## 3. What was not done

- Passing every expected type through `!` (an expected `i32` giving `Result<i32, AllocError>`): no function reads
  it, and inventing the error type would let `Ok(5)!` type-check against an error type nobody wrote.
- `AsyncConsole` is unchanged (it takes a `StringView`, group A's item A4).

## 4. Tests

`tests/reference/tests/corpus/`: `run-pass/expected-type-through-unwrap.cee`,
`check-fail/expected-type-through-unwrap.cee` (2.1); `run-pass/string-value-as-view.cee` (2.2), which fails to
build before this change. Both run-pass tests pass natively under `--check-leaks` and in the interpreter.
