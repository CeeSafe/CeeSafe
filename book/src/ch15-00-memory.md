---
title: Memory
summary: The opening of chapter 15 - how a program that has no garbage collector places, frees and shares its values, in five sections on Box, destruction, allocators and arenas, cells, and shared ownership.
keywords: [memory, heap, Box, drop, allocator, arena, Cell, RefCell, Arc, SlotMap]
---

# Memory

CeeSafe has no garbage collector. Chapter 4 gave the rule that replaces one: every value has one owner, and the
value is destroyed when its owner ends. With that rule, and with `String` and `Vector` from chapter 8, most
programs never think about memory again.

This chapter is for the programs that do. It answers five questions that the ownership rule leaves open.

1. [Box](./ch15-01-box): how does one value get memory of its own, and how is a type built that contains itself,
   such as a list or a tree?
2. [Destruction](./ch15-02-destruction): exactly when, and in what order, are values destroyed, and how does a
   program run its own code at that moment? This section covers `drop` blocks, `using` and `defer`.
3. [Allocators, arenas and scratch frames](./ch15-03-allocators): where does memory come from, and how can many
   values be freed in one step? It covers the allocator parameter of `Vector<T, A>`, `Arena`,
   `FixedBufferAllocator`, scratch frames and `Pool`.
4. [Cells](./ch15-04-cells): how can a value change while it is borrowed for reading? It covers `Cell`, `RefCell`
   and `OnceCell`, and the run-time check that some of them make.
5. [Shared ownership](./ch15-05-shared-ownership): what if a value has no single owner? It covers `Arc`, the leak
   that a cycle makes, and handles into a `SlotMap`, which are often the better answer.

The sections build on each other and are best read in order. They assume chapter 4, on ownership and borrowing,
[allocation failure](./ch09-03-allocation-failure) and [regions](./ch10-03-regions).

Two ideas run through all five. The first is that nothing is hidden: memory is asked for by a call that you can
see, the call returns a `Result`, and memory is given back at a place that you can point to. The second is that
the tools are ordered by cost. A local variable is cheaper than a `Box`, a borrow is cheaper than a cell, and a
handle is cheaper than shared ownership. Each section says when the dearer tool is worth it.
