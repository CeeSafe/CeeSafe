---
title: Enums and pattern matching
summary: The opening of chapter 6 - what an enum is, why a value that is one of several cases needs match to be used, and how optionals replace null, with links to the three sections.
assumes: [ch05-00-structs]
keywords: [enum, variant, match, pattern, optional, "T?", null, Some]
---

# Enums and pattern matching

**This chapter teaches** enums, the `match` statement that takes them apart, and optionals, which are how CeeSafe
says "there may be no value". It assumes [chapter 5, Structs](./ch05-00-structs).

A struct holds several values **together**: a point has an `x` and a `y`. An **enum** is a value that is **one of
several** cases: a command is a move, or a wait, or a quit. Each case is called a **variant**, and a variant can
carry data of its own. A move carries two distances; a quit carries nothing.

A program cannot read a variant's data without first asking which variant the value is. `match` asks that
question. It lists the cases, runs the code for the one that fits, and gives that code the variant's data by name.
The compiler checks that the list is complete: if you add a variant to an enum, every `match` that forgot it stops
compiling until you say what to do with it.

The same two ideas remove a whole class of errors. In CeeSafe an ordinary value is never null. A value that may be
absent has a different type, written `T?`, and the compiler does not let you use it as a `T` until you have handled
the case where nothing is there.

## The sections

1. [Defining an enum](./ch06-01-defining-an-enum): variants with and without data, making and passing enum values,
   comparing them, giving an enum methods, and when to choose an enum or a struct.
2. [`match`](./ch06-02-match): matching on an enum, binding a variant's data, covering every case, the catch-all,
   `match` as an expression, and matching on numbers, text and ranges.
3. [Optionals](./ch06-03-optionals): `T?`, `Some` and `null`, testing, the fallback `??`, unwrapping with `!`,
   `is Some(x)` in a condition, and `?` to pass an absent value on.

After this chapter you can model data that has cases, and you can read the two built-in enums that most of the
standard library returns: the optional, and `Result`, which chapter 9 covers.
