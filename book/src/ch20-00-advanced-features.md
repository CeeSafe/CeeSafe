---
title: Advanced features
summary: What this chapter covers - unsafe code and raw pointers, calling C and being called by it, the rest of interfaces and generics, arithmetic that wraps, checks or saturates, and derives, attributes and the absence of macros.
keywords: [unsafe, "raw pointer", "extern \"C\"", "const parameter", "associated type", supertrait, wrappingAdd, truncate, "@derive", attribute, macro]
---

# Advanced features

The chapters before this one teach the CeeSafe that most programs are written in. This chapter holds five subjects
that you need less often. Each section stands alone, so read the one you need when you need it.

Two of the sections are about leaving the checked language. Safe CeeSafe cannot read memory through an address it
was handed, and it cannot know what a C function does. For those there is `unsafe`: a marked block in which a
small, fixed list of extra operations is allowed, and in which you promise what the compiler can no longer check.
Everything else in the block is checked as before. The first section teaches the block and the raw pointer; the
second uses them to call C functions, pass buffers and strings to them, and let C call back.

The other three sections finish subjects that earlier chapters began:

- chapter 10 taught interfaces and generic types. Its third part here adds constants and types inside an
  interface, interfaces built on other interfaces, methods with type parameters of their own, and parameters that
  stand for a value instead of a type;
- chapter 3 said that integer overflow stops the program. The fourth section is the whole of arithmetic: the
  methods that wrap, check or saturate, the bit operators and shifts, every conversion between number types, and
  how floats print;
- chapters 5 and 8 used `@derive`. The last section says exactly what a derive writes, lists the other attributes,
  and explains why CeeSafe has no macros and what a program uses in their place.

## The sections

1. [Unsafe code](./ch20-01-unsafe-code): raw pointers, `unsafe { … }` and `unsafe fn`, what they permit and what
   you promise, `Box.intoRaw` and `Box.fromRaw`, and keeping unsafe code small behind a safe function.
2. [Calling C and being called by it](./ch20-02-calling-c): `extern "C"`, the types that cross, strings and
   buffers, callbacks, memory that C allocates, and structs imported from a C header.
3. [More on interfaces and generics](./ch20-03-more-interfaces-and-generics): generic methods, const parameters,
   interface constants, associated types, supertraits and generic impls, and what the language leaves out.
4. [Arithmetic: checked, wrapping, conversions](./ch20-04-arithmetic): the overflow methods, bits and shifts,
   `as`, `truncate`, `checkedTruncate`, `saturatingTruncate` and `round`, floats, and the checks the compiler
   removes.
5. [Derives and macros](./ch20-05-derives-and-macros): `@derive(Equal, HashKey, OrderKey)`, the other attributes,
   and what replaces macros.
