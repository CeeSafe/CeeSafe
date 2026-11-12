---
title: Structs
summary: The opening of chapter 5 - what a struct is, why a program groups related values under one name, and the three sections that teach declaring structs, using one in a small program, and giving it methods.
assumes: [ch03-00-common-concepts, ch04-00-ownership]
keywords: [struct, field, method, this, struct literal]
---

# Structs

**This chapter teaches** how to make a type of your own out of several values. It assumes
[chapter 3](./ch03-00-common-concepts), on variables, types and functions, and [chapter 4](./ch04-00-ownership), on
ownership and borrowing.

A **struct** is a type that groups named values. Each value in it is a **field**, with a name and a type. A player
in a game has a score and a number of lives; an order has an item, a quantity and a price. Without a struct, a
program keeps such values in separate variables and passes them to every function one by one, and nothing in the
program says that they belong together. With a struct, they are one value with one name. You can keep it in a
variable, pass it to a function, borrow it, and return it.

A struct also gives the functions that work on those values a place to live. A function written inside a struct is
called on a value of it, as `order.total()`, and it says in its first parameter whether it reads the value, changes
it, or takes it.

The rules of chapter 4 apply to a struct as they do to any value. A struct that holds only numbers and booleans is
copied when you assign it. A struct that owns something, such as a `String`, moves. This chapter shows both.

## The sections

- [Defining and making structs](./ch05-01-defining-structs): declaring a struct, making a value of it, reading and
  changing its fields, when a struct is copied and when it moves, and comparing two structs.
- [An example program](./ch05-02-example-structs): one small program, written first with loose variables, then with
  a struct, then with a function that borrows the struct.
- [Methods](./ch05-03-methods): functions that belong to a struct, the three ways a method receives its value, and
  static functions that make a value.

A struct holds all of its fields at once. A type whose value is one of several alternatives is an enum, which
[chapter 6](./ch06-00-enums) teaches.
