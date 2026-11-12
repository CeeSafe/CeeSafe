---
title: Common collections
summary: The opening of chapter 8 - what a collection is, that vectors, strings and maps keep their elements on the heap and own them, and that growing one can fail, which the chapter handles with an exclamation mark.
assumes: [ch04-00-ownership, ch06-03-optionals]
keywords: [collection, Vector, String, StringView, HashMap, OrderedMap, heap, AllocError]
---

# Common collections

**This chapter teaches** the four collections that most programs use: the vector, the string, the hash map and the
ordered map. It assumes [ownership](./ch04-00-ownership) and [optionals](./ch06-03-optionals).

An array, `[i32; 4]`, holds a number of values that is fixed when the program is compiled. A **collection** holds a
number of values that is decided while the program runs, and that can change. Three things are true of every
collection in this chapter.

**A collection keeps its elements on the heap.** The heap is the memory a program asks for while it runs. The
collection itself is a small value, a few words that say where the elements are and how many there are. It can
live in a variable, in a struct, or inside another collection.

**A collection owns its elements.** A value that you put into a collection is moved into it. When the collection
goes away, at the end of the block that owns it, it destroys its elements and gives the memory back. There is no
garbage collector, and nothing to free by hand.

**Growing a collection can fail.** A collection that needs more memory asks for it, and the answer can be no. So
every method that may grow a collection returns a `Result`: the success, or an `AllocError`. This chapter writes
`!` after such a call:

```ceesafe
fn main() {
    var numbers = Vector<i32>.create();
    numbers.push(42)!;
    Console.writeLine(numbers.length);
}
```

```text
1
```

`!` takes the success out of the `Result`, and stops the program if there was a failure. For a small program that
is the right answer to "there is no memory left". [Chapter 9](./ch09-00-error-handling) covers `Result`, and the
other answer, `?`, which hands the failure to the caller;
[section 9.3](./ch09-03-allocation-failure) is about allocation failure.

## The sections

- [8.1 Vectors](./ch08-01-vectors): `Vector<T>`, a list of values of one type that grows at its end.
- [8.2 Strings and string views](./ch08-02-strings): `String`, which owns text, and `StringView`, which borrows it.
- [8.3 Hash maps and ordered maps](./ch08-03-maps): `HashMap<K, V>` and `OrderedMap<K, V>`, which find a value by
  its key.

The vector and the string are built into the language. The two maps are in the standard library, lib/std, which
every hosted program can use without an import.

One rule from [borrowing](./ch04-02-borrowing) comes back in every section: while you hold a borrow of an element,
the collection cannot change. A collection that grows may move its elements to a larger piece of memory, and a
borrow of an element would then point at the old one. The compiler rejects such a program.
