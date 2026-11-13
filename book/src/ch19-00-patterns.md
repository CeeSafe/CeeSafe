---
title: Patterns
summary: What a pattern is, and the three questions this chapter answers - where a pattern may be written, which patterns can fail and how the compiler checks that none is missed, and every form a pattern takes.
keywords: [pattern, match, is, binding, exhaustive, guard]
specification: ["type-system.md §15.2", "type-system.md §15.4"]
---

# Patterns

A **pattern** describes a shape that a value may have. The program holds a value up against the pattern. Either
the value **fits**, and the names in the pattern receive parts of the value, or it does not fit, and the program
goes another way. `Some(n)` is a pattern: an optional that holds a value fits it, and `n` is that value. `null` is
another, and so are `0`, `'a'..='z'`, `Shape.Rect(w, h)` and `_`.

Earlier chapters used patterns where they needed them. [`match`](./ch06-02-match) introduced them with enums,
[Optionals](./ch06-03-optionals) used `Some(n)` and `null`, and [`Result`, `?` and `!`](./ch09-02-result) used
`Ok(value)` and `Err(error)`. This chapter gathers them into one account. It adds little that is new to use, and
a good deal that is new to know: the exact places, the exact rules, and the forms those chapters had no need of.

CeeSafe keeps patterns in three places only: the arms of a `match`, after the operator `is`, and a `let` with an
`else` block. A `for` and a parameter take a name and nothing more, and a plain `let` takes a name or a struct
pattern that cannot fail. That is fewer places than Rust
or C# have, and the first section says what to write where those languages would use a pattern.

The chapter has three sections:

1. [Where patterns go](./ch19-01-where-patterns-go): `match` arms, `value is pattern` and `let … else`, where the
   names of an `is` pattern can be used, and the places that take only a name.
2. [Patterns that can fail](./ch19-02-patterns-that-can-fail): which patterns fit every value and which do not,
   how the compiler decides that a `match` covers every value, and how it finds an arm that can never run.
3. [The syntax, whole](./ch19-03-the-syntax-whole): every form of pattern with a listing, and how a name in a
   pattern takes its value: by copy, by move or by borrow.

Read the first two in order. The third is written to be come back to.
