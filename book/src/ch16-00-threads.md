---
title: Threads
summary: What the chapter on threads covers - starting and joining threads, passing values through channels, sharing state behind a Mutex or an atomic, and the two properties Send and Share that the compiler checks so that safe code has no data race.
keywords: [thread, Thread.spawn, Thread.both, Channel, Mutex, Arc, atomic, Send, Share, data race]
specification: ["type-system.md §38", "ownership.md T1–T4"]
---

# Threads

A program can do several things at once by running them on **threads**. Each thread runs its own code, and the
operating system runs the threads at the same time on the processor's cores, or switches between them.

Threads make two kinds of mistake possible that a program with one thread cannot make:

- A **data race**: two threads use the same memory at the same time and at least one of them writes it. What the
  program then does depends on timing, and the fault may show once in a thousand runs.
- A **deadlock**: two threads each wait for something the other holds, and neither goes on.

CeeSafe removes the first kind at compile time. The rule you know from
[Borrowing](./ch04-02-borrowing), many readers or one writer, is checked across threads too: a value moves to another
thread only if it is safe there, and two threads reach the same value only through a type made for that. A program
that compiles without `unsafe` has no data race. The second kind, deadlock, the compiler does not prevent, and the
chapter shows where it comes from.

The chapter assumes ownership and borrowing ([chapter 4](./ch04-00-ownership)), vectors
([Vectors](./ch08-01-vectors)), `Result` and the `!` operator ([Result](./ch09-02-result)), and generic bounds
([Generic types and functions](./ch10-01-generic-types)). It uses closures, which chapter 13 teaches, and says what
each one does where it appears.

## The sections

- [Starting threads](./ch16-01-starting-threads): `Thread.spawn` and `join`, what a thread may capture, a panic in a
  thread, and `Thread.both`, which lets two threads borrow.
- [Channels](./ch16-02-channels): passing values from thread to thread through a `Channel<T>`.
- [Shared state](./ch16-03-shared-state): one value that several threads change, behind a `Mutex<T>` or as an
  atomic, with `Arc<T>` to own it from several threads, and how a deadlock happens.
- [Send and Share](./ch16-04-send-and-share): the two properties of types behind every check in the chapter, which
  types have them, and what the compiler says when one does not.

## Where threads run

Threads, channels, `Mutex` and `Arc` are part of the hosted library: programs for Linux, macOS, Windows and CosmoOS
have them. A freestanding program, such as a kernel, does not (the chapter on programs without an operating system
covers those). The atomics are in every program.

::: info The interpreter runs one thread at a time
`ceesafec run --interp` runs a program's threads one after another, in a fixed order, so a program gives the same
output on every run there. A program built with `ceesafec build` runs its threads at the same time. The listings of
this chapter are written to print the same thing either way: they join their threads before they print a result.
:::

## Threads and tasks

A thread is the right tool for work that uses a processor: computing, compressing, searching. For a program that
mostly waits, for the network or for a timer, CeeSafe also has **tasks**: many of them run on one thread, and each
gives the thread up while it waits. Chapter 17, on async, teaches tasks, and how tasks and threads work together.
