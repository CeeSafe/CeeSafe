# ADR 0105 — A `Result` nothing reads is a warning

- Status: **Accepted** by the owner, 2026-11-12 ("Fix 1 and 2 as you suggested", then "We fix all gaps 1-6 first"),
  implemented on branch `impl/0105`; released in ceesafec 0.42.0. Raised by the book (Decision 0011): chapters 8, 9 and 11 each
  had to teach "the compiler says nothing" as a mistake.
- Scope: one warning, N0402, in the checker (`src/check/body.cee`, `src/check/unused_results.cee`);
  `spec/diagnostics.md`, `spec/type-system.md` §17.5; the guide. No change to what a program means, to the library or
  to the runtime.

## 1. Context

Every operation that can fail returns a `Result`, and a call may stand as a statement (`type-system.md` §8). So

```ceesafe
fn main() {
    var v = Vector<i32>.create();
    v.push(1);
    Console.writeLine(v.length);
}
```

checks without a word. `push` returns `Result<void, AllocError>`; the statement evaluates it and destroys it
(§17.5). If the allocation fails, the program goes on with a vector that is one element short, and nothing says so.
The same holds for `file.write(bytes);`, for `parse(text);`, and in a test for `check(got == 3);` where
`check(got == 3)?;` was meant: the test passes whatever `got` is.

This is the one place where CeeSafe lets an error disappear without the program's text showing it. Everywhere else
the text says what happens to an error: `?` passes it on, `!` panics, `match` handles it, `r.ok()` drops it by name,
and since ADR 0072 `let _ = e;` discards a value on purpose.

Measured 2026-11-12 with the warning implemented: the compiler's own sources (`src/`, 0 statements); the corpus's
832 files and the 15 benchmarks (4 statements in 4 files: `await Timer.sleep(1);`, a `makeRes(8, false);` that is
there to test §17.5, and two in `check-fail` programs); the guide's 157 examples (0). So code written with care
already never does this, and the warning costs existing programs next to nothing.

## 2. Decision

2.1 **N0402.** An expression statement (`type-system.md` §8: a call, an `await` of one, a method call) whose value has the type
`Result<T, E>`, for any `T` and `E`, is warning N0402:

```text
main.cee:3:5: warning[N0402]: the 'Result' of this call is not used, so an error it reports is lost: handle it with '?', '!' or 'match', or write 'let _ = ...;' to discard it
```

2.2 **What does not warn.** `e?;`, `e!;` (their value is the `Ok` value, not a `Result`); `let _ = e;` (ADR 0072:
the discard is written); `let r = e;` (an unused local is not this ADR's matter); a statement whose value is `never`;
a `Result` that is an argument, an operand or a returned value; a statement that has an error of its own (the
warning would say nothing more). An optional (`T?`) used as a statement does not
warn: a lookup that finds nothing is not an error that is lost.

2.3 **A warning, not an error.** It is reported as N0400 and N0401 are (ADR 0073 §2.4): it never stops `check`,
`build`, `run` or `test`; it is reported for the root package's files only; `--deny-warnings` makes it an error. An
error would break programs that compile today, and the bootstrap chain's compilers know nothing of it.

2.4 **Once for a statement.** A generic function's body is checked for each instance, and a body may be checked
more than once speculatively; the statement is noted when it is checked and reported after every body, once for a
file and position.

2.5 **No fix is offered.** `ceesafec fix` applies fixes the compiler is sure of. Whether `?`, `!` or `let _ =` is
right depends on what the program means.

## 3. Alternatives

- **An error.** The right end state for a language whose point is that failure is visible; not now (§2.3). Once
  programs in the field are clean under the warning, a later ADR can raise it.
- **An attribute, `@mustUse`, on types or functions.** More general and more to learn. `Result` is the type the
  language itself gives failure (§17), and the only one for which the loss is silent and wrong by default. If other
  types need it, the attribute can be added and `Result` described as carrying it.
- **Warn for an unused optional too.** Rejected (§2.2): `map.remove(k);` and `v.pop();` are normal statements.

## 4. Consequences

- The book's and the guide's "mistake: forgetting `?`" boxes show the warning instead of silence.
- Four files of the corpus warned. `run-pass/async-dyn-send-frames.cee` now writes `let _ = await Timer.sleep(1);`
  and `run-fail/result-unwrap-err.cee` `let _ = makeRes(8, false);` (the value is still destroyed at the end of the
  statement, §17.5; under the interpreter the warning would stand before the panic the test expects).
  `check-fail/results.cee` and `check-fail/debug-assert-writes.cee` keep their bare calls and expect the warning.
- New test: `check-pass/unused-result-warning.cee`.
