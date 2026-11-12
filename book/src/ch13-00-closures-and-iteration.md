---
title: Closures and iteration
summary: The opening of chapter 13 - functions as values, closures that borrow or own what they capture, the for loop over ranges, sequences and iterators, the lookfor project rewritten with both, and what a closure call and a loop compile to.
assumes: [ch04-02-borrowing, ch08-01-vectors, ch10-02-interfaces, ch12-00-command-line-project]
keywords: [closure, function value, capture, iteration, iterator, for, next, sortBy, lookfor, performance]
---

# Closures and iteration

**This chapter teaches** two things that work together: passing a piece of code to a function, and visiting the
values of a sequence one at a time. It assumes borrowing ([Borrowing](./ch04-02-borrowing)), vectors
([Vectors](./ch08-01-vectors)) and interfaces ([Interfaces](./ch10-02-interfaces)), and its third section
continues the program of [chapter 12](./ch12-00-command-line-project).

A **closure** is a function written where it is used, `(v) => v > limit`, that can use the variables around
it. A function that takes a closure leaves one decision to its caller: which elements to count, how to order
two of them, what to do with each. The library's `sortBy`, `any` and `retain` are such functions, and you can
write your own.

**Iteration** is what a `for` loop does. It runs over a range of numbers, over the elements of an array, a
slice or a vector, and over an **iterator**: any value with a method `next` that gives the next value or
`null`. An iterator computes its values when the loop asks for them, so a program can visit the lines of a
text without first putting them in a vector.

CeeSafe keeps both features small, and the chapter says where the edges are:

- A closure borrows what it uses. It is checked as a borrow is, costs no allocation, and cannot leave the
  function that made it. A closure that must live longer owns what it uses and lives in a box, which you ask
  for in writing.
- There is no library of iterator adapters. A chain such as `map(…).filter(…).sum()` from another language is
  a `for` loop with an `if` in CeeSafe, or an iterator of your own.

The sections:

1. [Closures](./ch13-01-closures): function values, closure literals, the three ways to capture, the types
   `fn`, `borrow fn` and `borrow mut fn`, and closures in fields and boxes.
2. [Iteration](./ch13-02-iteration): everything `for` accepts, writing an iterator, iterators in generic
   code, and the slice and vector methods that take a closure.
3. [The project improved](./ch13-03-project-improved): `lookfor` loses a duplicated function to a closure
   and a vector to an iterator.
4. [What they cost](./ch13-04-what-they-cost): the C that a closure and a loop become, and measured times.
