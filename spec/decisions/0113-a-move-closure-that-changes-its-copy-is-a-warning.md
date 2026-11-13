# ADR 0113 — A `move` closure that changes its own copy is a warning

- Status: **Accepted** by the owner, 2026-11-13 ("Fix those new gaps too"), implemented on branch `fix/gaps2-a`; not
  yet released. Raised by the book (Decision 0011): chapter 16's Listing 16-9 teaches "the compiler says nothing and
  the program prints 0" as a mistake.
- Scope: one warning, N0403, in the checker (`src/check/closures.cee`, `src/check/expr.cee`,
  `src/check/unused_results.cee`); `spec/diagnostics.md`, `spec/type-system.md` §36.5; the guide. No change to what
  a program means, to the library or to the runtime.

## 1. Context

A `move` literal takes each variable it names: moved in, or **copied** if its type is Copy (§36.5). A body that
assigns to a capture changes the closure's own value. For a value that was moved in, the program cannot get this
wrong: the variable is moved out, and a later use is N0250. For a Copy value nothing stops it:

```ceesafe
fn main() {
    var hits = 0;
    let t = Thread.spawn(move () => { hits += 1; })!;
    t.join();
    Console.writeLine(hits);
}
```

checks without a word and prints 0. The program reads as "the thread counts and `main` prints the count"; what it
does is count in a copy that is thrown away. It is the mistake of someone who knows closures that capture by
reference (§36.2), which is what the same body without `move` would do; and with threads there is no such closure
to fall back on, so every beginner's first shared counter is written this way.

The closure itself is not wrong. `Box.create(move () => { count += 1; return count; })` (§36.5's own example) is a
counter that lives in its closure: nobody else reads `count` afterwards. What marks the mistake is the read after.

Measured 2026-11-13 with the warning implemented: the compiler's own sources and the libraries (`check --all`, 0);
the corpus, the support tests and the benchmarks (882 files: 0, the new test aside); the guide's 156 examples (0);
the book's 528 listings (1: Listing 16-9, which is there to show this mistake).

## 2. Decision

2.1 **N0403.** A `move` literal (a boxed closure's, `Thread.spawn`'s, or an `async move` child's) whose body changes
a captured variable of a Copy type (assigns to it, borrows it mutably, or calls a mutating method on it), where the
enclosing function names that variable again after the literal, is warning N0403, at the literal:

```text
main.cee:3:26: warning[N0403]: this 'move' closure changes its own copy of 'hits': the type is Copy, so the closure took a copy, and the 'hits' read after it is unchanged; return the value from the closure, or share it through an atomic, a 'Mutex' or a 'Channel'
```

2.2 **"After" is by position**: a use of the variable whose text starts at or after the literal's end, in the same
function. The check has no flow analysis and needs none for the case it is for. A use that comes before the literal
in the text but runs after it (in a loop) does not warn; a later assignment to the variable counts as a use.

2.3 **What does not warn.** A capture that is only read; a capture that is not Copy (N0250 covers it); a closure
that is not a `move` literal (it changes the variable itself); a variable nothing names after the literal.

2.4 **A warning**, reported as N0402 is (ADR 0105 §2.3, §2.4): it never stops `check`, `build`, `run` or `test`; root
package only; `--deny-warnings` makes it an error; once for a literal and variable, whatever the number of
instances of a generic function. No fix is offered: the right repair (return the value, an atomic, a mutex, a
channel) depends on the program.

## 3. Alternatives

- **An error.** It would refuse programs that compile and are right: a closure that starts from the variable's
  value and keeps its own running copy, while the function goes on using the original, is legitimate, if rare.
- **Warn for every `move` literal that changes a Copy capture.** It would warn for §36.5's counter and every
  closure like it.
- **Warn for any use of a Copy variable after a `move` literal copied it.** Reading the same number in the closure
  and after it is normal (`let limit = 5; Thread.spawn(move () => work(limit))`).
- **A flow-sensitive "read after"**, in the NIR. More exact in loops, far more machinery, and the diagnostic would
  come from a pass that runs only on programs that check. The positional rule catches the mistake as it is written.

## 4. Consequences

- The book's Listing 16-9 shows the warning instead of silence (the integrator updates the book).
- New code: **N0403**, the next free warning after N0402.
- New test: `check-pass/copied-change-warning.cee`.
