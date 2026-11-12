---
title: A project - a command-line program
summary: The opening of the project that ends the course - lookfor, a tool that prints the lines of a file that contain a text, what it is built from, and the six sections that build it step by step.
assumes: [ch01-03-hello-package, ch09-02-result, ch11-01-writing-tests]
keywords: [project, command line, lookfor, search, arguments, file, environment, standard error, exit status]
---

# A project: a command-line program

**This chapter builds** one small, complete program, and uses most of what chapters 1 to 11 teach to do it. It
assumes you can make a package ([Hello, package](./ch01-03-hello-package)), return a `Result`
([Recoverable errors with `Result`](./ch09-02-result)) and write a test ([Writing tests](./ch11-01-writing-tests)).

The program is called `lookfor`. You give it a text and the name of a file, and it prints every line of the file
that contains the text:

```sh
lookfor tide harbour.txt
```

```text
Nets dry on the wall, and the tide is low.
```

It is a small relative of the Unix tool `grep`. It is a good last project because a command-line tool touches the
world outside the program at every edge: it reads arguments, a file and the environment; it writes to two
different streams; and it tells the program that started it, with a number, whether it succeeded. Each of those
edges can fail, so the project is also practice in handling errors without stopping the program in the wrong
place.

## What the project draws on

| The program needs | The chapter that teaches it |
|---|---|
| a package of two files | [chapter 7](./ch07-00-packages) |
| a struct for its configuration | [chapter 5](./ch05-00-structs) |
| an enum for its errors, and `match` to report them | chapter 6 ([enums](./ch06-01-defining-an-enum), [`match`](./ch06-02-match)) |
| optionals, for an argument or a variable that may be missing | [section 6.3](./ch06-03-optionals) |
| a `Vector` of bytes for the file, and views of its text | chapter 8 ([vectors](./ch08-01-vectors), [strings](./ch08-02-strings)) |
| `Result` and `?` | [chapter 9](./ch09-00-error-handling) |
| a region parameter, so that a function can return views into its argument | [section 10.3](./ch10-03-regions) |
| tests | [chapter 11](./ch11-00-testing) |

Nothing here is new language. What is new is the library at the edges, which this chapter introduces as it goes:
`Process.argument`, `File`, `StringView.fromUtf8`, `Environment.variable`, `Console.writeErrorLine` and
`Process.exit`.

## The sections

1. [Accepting command-line arguments](./ch12-01-arguments): reading the text and the file name, and what to do
   when one is missing.
2. [Reading a file](./ch12-02-reading-a-file): the whole file into memory, the errors that can give, and bytes
   that are not text.
3. [Refactoring for clarity and errors](./ch12-03-refactoring): a configuration struct, an error enum, a `run`
   function, and a second file.
4. [Adding the search, test first](./ch12-04-test-first): a failing test, then the function that makes it pass.
5. [Working with the environment](./ch12-05-environment): an option from an environment variable, for a search
   that ignores case.
6. [Errors to standard error](./ch12-06-standard-error): results on one stream, messages on the other, and the
   exit status.

## The package

Make the package and a file to search:

```sh
ceesafec new lookfor
cd lookfor
```

Then put this text in `harbour.txt`, beside `ceesafe.toml`. Every section searches it.

```text
The harbour wakes before the town.
Nets dry on the wall, and the tide is low.
A boat goes out; the harbour bell rings twice.
Nobody on the quay looks up.
The Tide turns at noon.
By evening the boats are home.
```

The finished package is in the book's repository at `book/projects/ch12/lookfor`, and the program as it stands
at the end of each section is beside it, in `stage1` to `stage5`. `stage4-red` is section 12.4 at the moment
its test fails.

::: info A note on running it
While you build the program, run it in the interpreter. The arguments for your program come after `--`, so that
the compiler does not take them for its own:

```sh
ceesafec run --interp -- tide harbour.txt
```

`ceesafec build` makes the executable `out/lookfor`, which you run as `./out/lookfor tide harbour.txt`.
:::

Next: [accepting command-line arguments](./ch12-01-arguments).
