---
title: Hello, world
summary: A first program in one file - fn main, Console.writeLine, running it in the interpreter and building it, and the two mistakes newcomers make (a missing semicolon, and braces in a string that do not interpolate).
assumes: [ch01-01-installation]
keywords: [fn, main, Console.writeLine, Console.write, "run --interp", build, semicolon, string interpolation]
diagnostics: [N0100, N0400]
---

# Hello, world

**This section teaches** how to write a CeeSafe program in one file, run it, and build it. It assumes you have
[installed `ceesafec`](./ch01-01-installation).

## Writing the program

Make a directory for your work and, in it, a file called `main.cee`. CeeSafe source files end in `.cee`.

<span class="listing-caption"><strong>Listing 1-1</strong>, <code>main.cee</code>: A program that prints a line.</span>

```ceesafe
fn main() {
    Console.writeLine("Hello, world!");
}
```

```text
Hello, world!
```

## Running it

Run it with:

```sh
ceesafec run --interp main.cee
```

`run --interp` checks the program and runs it in the compiler's interpreter. It needs no C compiler and starts at
once, which makes it the quickest way to try something.

## What the program says

```ceesafe
fn main() {
    Console.writeLine("Hello, world!");
}
```

- `fn main()` declares a function called `main` that takes nothing and returns nothing. `fn` begins every function.
- The body is between `{` and `}`.
- `Console.writeLine(…)` prints its argument and ends the line. `Console` is built in: there is nothing to import.
- `"Hello, world!"` is a string.
- The statement ends with `;`.

::: tip Rule
A program starts at `fn main()`. Every executable program has exactly one.
:::

::: tip Rule
A statement ends with a semicolon. The four-space indentation is the style `ceesafec fmt` writes; the compiler
does not read indentation.
:::

`Console.writeLine` prints numbers and booleans as well as strings, and `Console.write` prints without ending the
line:

<span class="listing-caption"><strong>Listing 1-2</strong>, <code>main.cee</code>: Printing values of several types, and part of a line at a time.</span>

```ceesafe
fn main() {
    Console.writeLine(42);
    Console.writeLine(true);
    Console.write("six times seven is ");
    Console.writeLine(6 * 7);
}
```

```text
42
true
six times seven is 42
```

## Building an executable

To make a program you can run by itself:

```sh
ceesafec build main.cee
./main
```

`build` compiles the program to C, and the C to an executable named after the file: `main`, or `main.exe` on
Windows. This is the step that needs a [C compiler](./ch01-01-installation#a-c-compiler-for-build). The executable
is native code with no runtime to install beside it; you can copy it to another machine of the same kind and run it.

## Mistakes

::: warning Mistake: a missing semicolon
Every statement needs its `;`, the last one in a block too.
:::

<span class="listing-caption"><strong>Listing 1-3</strong>, <code>main.cee</code>: The statement has no semicolon. This does not compile.</span>

<!-- ceesafe: fail N0100 -->
```ceesafe
fn main() {
    Console.writeLine("Hello, world!")
}
```

```text
main.cee:2:39: error[N0100]: expected ';', found '}'
  |
2 |     Console.writeLine("Hello, world!")
  |                                       ^
```

The message names the file, the line and the column, then the code `N0100` and what was expected. The fix is the
`;` it asks for. `ceesafec explain N0100` says more about any code.

::: warning Mistake: braces in a string
A string is printed as it is written. CeeSafe has no string interpolation: `{name}` inside a string is those six
characters, not the value of `name`.
:::

<span class="listing-caption"><strong>Listing 1-4</strong>, <code>main.cee</code>: The braces are printed. The compiler warns, and the program still runs.</span>

<!-- ceesafe: warns N0400 -->
```ceesafe
fn main() {
    let name = "Ada";
    Console.writeLine("Hello, {name}!");
}
```

```text
main.cee:3:31: warning[N0400]: this string prints '{name}' as written (there is no string interpolation): write the value in a call of its own, or '\{' to mean the brace
  |
3 |     Console.writeLine("Hello, {name}!");
  |                               ^^^^^^
```

Write the value in a call of its own:

<span class="listing-caption"><strong>Listing 1-5</strong>, <code>main.cee</code>: The value printed by its own call.</span>

```ceesafe
fn main() {
    let name = "Ada";
    Console.write("Hello, ");
    Console.write(name);
    Console.writeLine("!");
}
```

```text
Hello, Ada!
```

`let name = "Ada";` gives a value a name; [chapter 3](./ch00-00-introduction#how-the-book-is-laid-out) is about
variables.

::: details Coming from Rust, C#, TypeScript or Python
There is no `println!("{}", x)`, `$"{x}"`, `` `${x}` `` or `f"{x}"`. Print each value with its own
`Console.write` call, or build the text with a `String`, which chapter 8 covers.
:::

## Summary

- A program is a file ending in `.cee` with a `fn main()`.
- `Console.writeLine(x)` prints `x` and ends the line; `Console.write(x)` does not end it.
- `ceesafec run --interp main.cee` runs a program at once; `ceesafec build main.cee` makes an executable.
- Statements end with `;` (`N0100` if one is missing).
- Braces in a string are printed as written (`N0400` warns).

Next: [a package](./ch01-03-hello-package), which is how programs of more than one file are kept.
