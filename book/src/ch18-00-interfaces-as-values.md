---
title: Interfaces as values
summary: The opening of the chapter on dyn - a value known only by the interface it implements, why a program wants one, what it costs beside a generic and an enum, and the three sections that teach it.
assumes: [ch10-02-interfaces, ch15-01-box]
keywords: [dyn, interface, dynamic dispatch, "Box<dyn I>", "borrow dyn I", generic, enum, design]
specification: ["type-system.md §37"]
---

# Interfaces as values

**This chapter teaches** how to hold and pass a value when all that is known about it is an interface it
implements. It assumes you can declare an interface and use it as a bound ([Interfaces](./ch10-02-interfaces)) and
know what a `Box` is ([Box](./ch15-01-box)). It also uses closures ([Closures](./ch13-01-closures)) and, in one
part, the thread properties `Send` and `Share` ([Send and Share](./ch16-04-send-and-share)).

Until now an interface has been a **bound**: `fn describe<S: Shape>(shape: borrow S)` works for any one type that
implements `Shape`, and the compiler makes a copy of `describe` for each type it is called with. The type is always
known when the program is compiled. That is fast, and it is the right default.

Some programs cannot know the type. A drawing is a list of shapes of different types. A document is a list of
blocks, and a library's user adds block types that the library never saw. A menu is a table of commands. For
these, CeeSafe has the type `dyn Shape`: a value of some type that implements `Shape`. A `Box<dyn Shape>` owns
one, a `borrow dyn Shape` borrows one, and a call of a method on it finds the right function while the program
runs.

A `dyn` is not the only way to put different kinds of value in one place, and often it is not the best. An enum
does it too, with every kind listed, and for many programs that is the better design. So this chapter has two
jobs: to teach `dyn`, and to teach when to choose it.

## The sections

1. [Dynamic dispatch](./ch18-01-dynamic-dispatch): the type `dyn I`, in a box and behind a borrow; how a value
   becomes one; what can be called through it and which interfaces allow it; boxed closures; `+ Send`; and the
   table of functions behind a call.
2. [When to use it](./ch18-02-when-to-use-it): a generic with a bound, an enum with `match`, and `dyn`, side by
   side, with what each costs when the program grows, what a call costs, and rules for choosing.
3. [A design worked through](./ch18-03-a-design-worked-through): one small program, a page of blocks, written
   all three ways, with its tests.
