---
title: Accepting command-line arguments
summary: Reading a program's arguments with Process.argumentCount and Process.argument, which gives an optional view, and ending the program with a usage line and an exit status when an argument is missing.
assumes: [ch12-00-command-line-project, ch06-02-match, ch06-03-optionals]
keywords: [Process.argument, Process.argumentCount, Process.exit, arguments, optional, usage, never]
diagnostics: [N0205]
specification: ["type-system.md §26.1"]
---

# Accepting command-line arguments

**This section teaches** how a program reads the words typed after its name, and what it does when there are too
few of them. It is the first step of the project that [chapter 12](./ch12-00-command-line-project) builds, and it
assumes [`match`](./ch06-02-match) and [optionals](./ch06-03-optionals).

`lookfor` takes two arguments: the text to look for and the file to look in.

```sh
lookfor tide harbour.txt
```

## Every argument

Two built-in functions give a program its arguments. Like `Console`, `Process` needs no import.

<span class="listing-caption"><strong>Listing 12-1</strong>, <code>main.cee</code>: A program that prints how many arguments it has, then each of them.</span>

```ceesafe
fn main() {
    let count = Process.argumentCount();
    Console.write("arguments: ");
    Console.writeLine(count);
    for (i in 0..count) {
        match (Process.argument(i)) {
            Some(text) => { Console.writeLine(text); }
            null => { Console.writeLine("(not UTF-8)"); }
        }
    }
}
```

```text
arguments: 1
main.cee
```

Run with nothing after its name, the program has one argument: argument 0 is the name the program was started
by. In the interpreter that is the source file; for a built executable it is what you typed, such as `./main`.
Give it two words:

```sh
ceesafec run --interp main.cee -- tide harbour.txt
```

```text
arguments: 3
main.cee
tide
harbour.txt
```

- `Process.argumentCount()` is a `usize`: the number of arguments, counting argument 0.
- `Process.argument(i)` is a `StringView?`, an optional view of the argument's text. It is `null` when there is
  no argument `i`. It is also `null` when the argument's bytes are not UTF-8, because a `StringView` is always
  valid UTF-8.
- The words after `--` go to your program. Without the `--`, `ceesafec` would read them as its own.

::: tip Rule
`Process.argument(i)` is an optional. The first argument the user typed is `Process.argument(1)`; argument 0 is
the program's name.

*Specification: `type-system.md` §26.1.*
:::

The views that `Process.argument` gives are of text that lasts for the whole run of the program. Nothing is
copied, and there is nothing to free. Section 12.3 uses that fact.

## The two arguments, or a usage line

In the `lookfor` package, replace `src/main.cee` with this:

<span class="listing-caption"><strong>Listing 12-2</strong>, <code>src/main.cee</code>: The program takes its two arguments, or says how it is used and exits.</span>

<!-- ceesafe: exits 2 -->
```ceesafe
fn main() {
    let query = match (Process.argument(1)) {
        Some(text) => text,
        null => {
            Console.writeLine("usage: lookfor <text> <file>");
            Process.exit(2);
        }
    };
    let path = match (Process.argument(2)) {
        Some(text) => text,
        null => {
            Console.writeLine("usage: lookfor <text> <file>");
            Process.exit(2);
        }
    };
    Console.write("looking for: ");
    Console.writeLine(query);
    Console.write("in the file: ");
    Console.writeLine(path);
}
```

```text
usage: lookfor <text> <file>
```

That is what it prints with no arguments. With both:

```sh
ceesafec run --interp -- tide harbour.txt
```

```text
looking for: tide
in the file: harbour.txt
```

Each `match` is an expression: its value becomes `query` or `path`. The `Some` arm gives the view. The `null` arm
gives nothing, and it does not need to, because `Process.exit` never returns: its return type is `never`, and
the compiler accepts an arm that cannot finish in place of a value of any type.

`Process.exit(2)` ends the program at once with the **exit status** 2. The status is a small number that the
program which started yours, usually a shell, can read. By convention 0 means success and anything else means
failure. In a POSIX shell, `echo $?` prints the status of the last command:

```sh
ceesafec run --interp -- tide
echo $?
```

```text
usage: lookfor <text> <file>
2
```

::: tip Rule
`Process.exit(status)` ends the program with that status and never returns. 0 means success.
:::

The usage line goes to standard output for now, and the code that prints it is written twice. Sections
[12.3](./ch12-03-refactoring) and [12.6](./ch12-06-standard-error) mend both.

## Mistakes

::: warning Mistake: using the argument as if it were always there
`Process.argument(1)` is a `StringView?`, not a `StringView`. The compiler does not let you use it until you have
said what happens when it is `null`.
:::

<span class="listing-caption"><strong>Listing 12-3</strong>, <code>main.cee</code>: An optional given where a view is wanted. This does not compile.</span>

<!-- ceesafe: fail N0205 -->
```ceesafe
fn main() {
    let query: StringView = Process.argument(1);
    Console.writeLine(query);
}
```

```text
main.cee:2:29: error[N0205]: expected 'StringView', found 'StringView?'
  |
2 |     let query: StringView = Process.argument(1);
  |                             ^^^^^^^^^^^^^^^^^^^
```

Use a `match`, as Listing 12-2 does, or give a fallback with `??`: `Process.argument(1) ?? "tide"`.

::: warning Mistake: `!` on input from outside
`!` takes the value out of an optional and panics if it is `null`. That is right for a thing that cannot happen.
A user who forgets an argument can happen.
:::

<span class="listing-caption"><strong>Listing 12-4</strong>, <code>main.cee</code>: The program panics when it is run without an argument.</span>

<!-- ceesafe: panics -->
```ceesafe
fn main() {
    let query = Process.argument(1)!;
    Console.writeLine(query);
}
```

Run with no arguments, it stops with this message and the status 134:

```text
ceesafe panic: unwrapped null at main.cee:2:17
```

A panic tells a programmer where a bug is. It tells a user nothing about what to type. For a missing argument,
print the usage line and exit, as Listing 12-2 does.

::: details Coming from Rust
There is no `std::env::args()` iterator to collect into a `Vec<String>`. `Process.argument(i)` gives a view
straight into the arguments the operating system passed, so no `clone()` is needed to keep one. An argument
that is not UTF-8 is `null`, where `args()` panics.
:::

::: details Coming from C, Go or Python
`Process.argumentCount()` and `Process.argument(i)` are `argc` and `argv[i]`, `len(os.Args)` and `os.Args[i]`,
`len(sys.argv)` and `sys.argv[i]`. The difference is that an index past the end is `null`, which the compiler
makes you handle, and not a crash or an exception.
:::

## Summary

- `Process.argumentCount()` counts the arguments, with the program's name as argument 0.
- `Process.argument(i)` is a `StringView?`: `null` if there is no such argument or it is not UTF-8 (`N0205` if you
  use it as a plain view).
- Arguments for your program follow `--` on the `ceesafec run` command line.
- `Process.exit(status)` ends the program and never returns, so a `match` arm that ends with it needs no value.
- Report a missing argument with a usage line and a status that is not 0; do not panic.

Next: [reading the file](./ch12-02-reading-a-file) that the second argument names.
