---
title: Understanding ownership
summary: What ownership is for - memory that is freed at the right moment, exactly once, with no garbage collector and no manual free - and the three sections that teach owners, borrows, and slices and views.
assumes: [ch03-00-common-concepts]
keywords: [ownership, owner, memory safety, garbage collector, borrow, slice, view, move]
---

# Understanding ownership

**This chapter teaches** ownership, the idea that makes CeeSafe different from most languages you may know. It
assumes the basics of [chapter 3](./ch03-00-common-concepts): variables, types, functions and control flow.

## The problem

A program asks for memory while it runs: for text that a user types, for a list that grows. Memory that is no
longer needed must be given back, or the program grows until the machine has none left. Languages solve this in one
of two ways.

**A garbage collector.** In C#, Java, Go, Python, JavaScript and TypeScript, you make values and forget about them.
A part of the language's runtime, the garbage collector, looks from time to time for values that nothing can reach
any more, and frees them. This is safe and easy. It costs memory and processor time, the program pauses at moments
you do not choose, and the collector must be present wherever the program runs, which rules out the smallest
machines and the inside of an operating system.

**By hand.** In C, the programmer frees each piece of memory with a call. This costs nothing at run time, and it
is where the worst bugs come from:

- memory that is freed and then used again, which reads or damages whatever is there now;
- memory that is freed twice;
- memory that is never freed;
- a list that grows, and so moves in memory, while another part of the program still looks at the old place.

A C compiler accepts all four. They show up later, as crashes, wrong results and security holes.

## What CeeSafe does

CeeSafe has no garbage collector, and you do not free memory by hand. Instead, the compiler follows a few rules
about every value, and a program that breaks one does not compile:

- Every value has one **owner**, a variable or another value that holds it. When the owner goes away, the value is
  destroyed and its memory is given back, at that moment and exactly once.
- Code that wants to use a value without owning it **borrows** it. While a borrow is in use, the compiler does not
  let the value change under it or go away.
- A **slice** or a **view** is a borrow of part of a collection or of a piece of text, under the same rule.

The four bugs above become compile errors. The checks happen when the program is compiled: the program that runs
carries no collector and no bookkeeping for them, and frees each value at a place you can read from the source.

The price is that the compiler rejects some programs you would expect to work, most often in your first weeks.
Each rejection comes as a diagnostic that names the lines involved, and this chapter shows the ones you will meet
first and what to do about each.

Ownership is the base of the rest of the language. Structs, collections, errors, threads and files all follow its
rules, so the time spent on this chapter is repaid in every later one.

## The sections

1. [What ownership is](./ch04-01-what-ownership-is): owners, copies and moves, and when a value is destroyed.
2. [Borrowing](./ch04-02-borrowing): using a value without taking it, and the one rule about borrows.
3. [Slices and views](./ch04-03-slices-and-views): borrowing part of an array or a vector, and borrowing text.

Read them in order: each uses the one before.
