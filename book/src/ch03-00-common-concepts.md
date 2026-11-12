---
title: Common programming concepts
summary: The opening of chapter 3, which covers what almost every CeeSafe program uses - variables, the built-in data types, functions, comments and control flow - and lists the chapter's five sections.
assumes: [ch01-02-hello-world]
keywords: [variables, data types, functions, comments, control flow]
---

# Common programming concepts

**This chapter teaches** the parts of CeeSafe that almost every program uses: variables, the built-in data types,
functions, comments and control flow. It assumes you can write and run a program in one file
([Hello, world](./ch01-02-hello-world)).

Most languages have these parts, so the ideas will be familiar. The details are not. CeeSafe makes several choices
that differ from the languages you may know, and the compiler enforces each one:

- a variable cannot change unless it is declared with `var`;
- a name that is in scope cannot be declared again;
- two numbers of different types cannot be mixed, and a conversion that could lose information must say so;
- integer arithmetic that overflows stops the program instead of giving a wrong answer;
- the condition of an `if` or a loop is written in parentheses and must be a `bool`.

Each section shows the rule, a program that follows it, and the diagnostic the compiler gives when a program breaks
it. Every listing is a whole program. To try one, save it as `main.cee` and run `ceesafec run --interp main.cee`.

## The sections

- [Variables](./ch03-01-variables): `let`, `var` and `const`; types that are inferred or written; scope; why a name
  cannot be declared twice.
- [Data types](./ch03-02-data-types): integers, floats, `bool`, characters and text, arrays; checked overflow;
  conversions; what CeeSafe leaves out.
- [Functions](./ch03-03-functions): parameters and return types, expression bodies, statements and expressions, and
  functions that never return.
- [Comments](./ch03-04-comments): line comments, block comments and documentation comments.
- [Control flow](./ch03-05-control-flow): `if`, `while`, `loop`, `for`, `break` and `continue`, and a first look at
  `match`.

The sections build on each other in this order, but each one says what it assumes, so you can also read one alone.

After this chapter you can write small programs that compute with numbers and text. The next chapter,
[Ownership](./ch04-00-ownership), is about the idea that sets CeeSafe apart: how the compiler keeps memory safe
without a garbage collector.
