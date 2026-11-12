---
title: Generics, interfaces and regions
summary: Why generic code exists - two functions that differ only in a type become one function with a type parameter - and what the three sections of the chapter teach about type parameters, interfaces and regions.
assumes: [ch03-03-functions, ch04-03-slices-and-views, ch06-03-optionals]
keywords: [generics, type parameter, interface, region, duplication, Copy]
---

# Generics, interfaces and regions

**This chapter teaches** how to write code once for many types. It assumes you can write a function
([Functions](./ch03-03-functions)), pass a slice ([Slices and views](./ch04-03-slices-and-views)) and use an
optional ([Optionals](./ch06-03-optionals)).

## The same function twice

A program wants the last element of a slice of numbers, and the last element of a slice of names. A slice may be
empty, so each function returns an optional.

<span class="listing-caption"><strong>Listing 10-1</strong>, <code>main.cee</code>: Two functions that differ only in the type of the elements.</span>

```ceesafe
fn lastNumber(values: Slice<i32>) -> i32? {
    if (values.length == 0) {
        return null;
    }
    return values[values.length - 1];
}

fn lastName(values: Slice<StringView>) -> StringView? {
    if (values.length == 0) {
        return null;
    }
    return values[values.length - 1];
}

fn main() {
    let numbers = [3, 1, 4];
    let names = ["Ada", "Grace"];
    Console.writeLine(lastNumber(numbers[..]) ?? 0);
    Console.writeLine(lastName(names[..]) ?? "nobody");
}
```

```text
4
Grace
```

The two bodies are the same, character for character. Only `i32` and `StringView` differ. A third element type
means a third copy, and a correction to one copy must be made in all of them.

## One function with a type parameter

A **generic** function names the type that varies. The name is a **type parameter**, written between `<` and `>`
after the function's name.

<span class="listing-caption"><strong>Listing 10-2</strong>, <code>main.cee</code>: One generic function replaces both.</span>

```ceesafe
fn last<T: Copy>(values: Slice<T>) -> T? {
    if (values.length == 0) {
        return null;
    }
    return values[values.length - 1];
}

fn main() {
    let numbers = [3, 1, 4];
    let names = ["Ada", "Grace"];
    Console.writeLine(last(numbers[..]) ?? 0);
    Console.writeLine(last(names[..]) ?? "nobody");
}
```

```text
4
Grace
```

- `T` stands for a type that each call chooses. The first call uses `last` with `T` as `i32`, the second with `T`
  as `StringView`. The compiler works that out from the arguments.
- `T: Copy` is a **bound**. It says that `last` works only for types whose values can be copied, because the body
  copies an element out of the slice.

The compiler checks `last` once, for every type that meets the bound, and then compiles it for each type the
program uses. The result runs as fast as Listing 10-1.

## What the chapter covers

A type parameter raises three questions, and each section answers one.

- [Generic types and functions](./ch10-01-generic-types): how to declare type parameters on functions, structs and
  enums, how the compiler finds the type arguments, and what a bound is.
- [Interfaces](./ch10-02-interfaces): how to say what a type can do, so that generic code can call methods on a
  `T`.
- [Regions](./ch10-03-regions): how a struct holds a borrow, and how the compiler knows that the struct does not
  outlive the value it borrows from.

Generic code in CeeSafe has no cost when the program runs: there is no boxing, and no table of methods is
consulted. A call through a table is a separate feature, `dyn`, which chapter 18 covers.

Next: [Generic types and functions](./ch10-01-generic-types).
