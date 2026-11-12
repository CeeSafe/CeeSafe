---
title: Errors to standard error
summary: Writing results to standard output and messages to standard error with Console.writeErrorLine, so that redirected output holds only results, and giving the program three exit statuses that a shell can test.
assumes: [ch12-05-environment, ch12-03-refactoring]
keywords: [standard error, standard output, Console.writeError, Console.writeErrorLine, redirect, exit status, Process.exit, main, i32]
diagnostics: [N0205]
specification: ["type-system.md §26.3"]
---

# Errors to standard error

**This section teaches** the two streams a program writes to, how to write to each, and how a program reports
its outcome as an exit status. It finishes the program of [chapter 12](./ch12-00-command-line-project), from
where [section 12.5](./ch12-05-environment) left it, and changes the `main` of
[section 12.3](./ch12-03-refactoring).

## Two streams

A program starts with two places to write:

- **standard output**, for its results;
- **standard error**, for messages about the run: errors, warnings, usage.

In a terminal both appear on the screen, so the difference does not show. It shows when the output goes
somewhere else. In a shell, `>` sends standard output to a file and leaves standard error on the screen. So far
`lookfor` writes everything with `Console.writeLine`, which is standard output. Build the program of section
12.5 and send its output to a file, with a file name that does not exist:

```sh
ceesafec build
./out/lookfor tide nothere.txt > found.txt
cat found.txt
```

```text
lookfor: No such file or directory
```

The screen showed nothing, and the error message is in `found.txt`, where the lines found should be. A person
sees no sign of a problem, and a program reading `found.txt` takes the message for a result.

## Writing to standard error

`Console.writeError(text)` and `Console.writeErrorLine(text)` write to standard error. Change `report` and
`main` to use them:

<span class="listing-caption"><strong>Listing 12-26</strong>, <code>src/main.cee</code>: Every message goes to standard error, and <code>main</code> exits with one of three statuses.</span>

<!-- ceesafe: skip -->
```ceesafe
fn report(error: LookError) {
    Console.writeError("lookfor: ");
    match (error) {
        LookError.MissingQuery => { Console.writeErrorLine("no text to look for"); }
        LookError.MissingPath => { Console.writeErrorLine("no file to look in"); }
        LookError.NotUtf8 => { Console.writeErrorLine("the file is not UTF-8 text"); }
        LookError.Io(e) => { Console.writeErrorLine(e.message()); }
        LookError.Memory(_) => { Console.writeErrorLine("out of memory"); }
    }
}

fn main() {
    let config = match (configure()) {
        Ok(config) => config,
        Err(error) => {
            report(error);
            Console.writeErrorLine("usage: lookfor <text> <file>");
            Process.exit(2);
        }
    };
    match (run(config)) {
        Ok(count) => {
            if (count == 0) {
                Process.exit(1);
            }
        }
        Err(error) => {
            report(error);
            Process.exit(2);
        }
    }
}
```

Because section 12.3 put every message in `report` and `main`, these are the only lines that change. The lines
found are still printed by `run` with `Console.writeLine`.

```sh
ceesafec build
./out/lookfor tide nothere.txt > found.txt
```

```text
lookfor: No such file or directory
```

Now the message is on the screen and `found.txt` is empty. With a file that exists, the screen shows nothing
and the result is in the file:

```sh
./out/lookfor tide harbour.txt > found.txt
cat found.txt
```

```text
Nets dry on the wall, and the tide is low.
```

::: tip Rule
Results go to standard output: `Console.write`, `Console.writeLine`. Everything said about the run goes to
standard error: `Console.writeError`, `Console.writeErrorLine`.

*Specification: `type-system.md` §26.3.*
:::

The two streams keep their order when both go to the same place: `Console.writeError` first writes out
anything that standard output is still holding.

## The exit status

The message is for a person. A script that runs `lookfor` wants a number. The program has used 0 and 2 since
section 12.1. One case is left: the run went well and found nothing. That is not an error, and it is not the
same as finding something. Tools of this kind have three statuses by convention, and `lookfor` follows it:

| Status | Meaning |
|---|---|
| 0 | at least one line was found |
| 1 | no line was found |
| 2 | the search could not be done |

`main` needs to know how many lines were printed, so `run` returns the count in place of `void`:

<span class="listing-caption"><strong>Listing 12-27</strong>, <code>src/look.cee</code>: <code>run</code> returns how many lines it printed.</span>

<!-- ceesafe: skip -->
```ceesafe
/// Reads the file, searches it and prints the lines found. The result is how many lines it printed.
fn run(config: borrow Config) -> Result<usize, LookError> {
    let bytes = readAll(config.path)?;
    let text = match (StringView.fromUtf8(bytes.view())) {
        Some(text) => text,
        null => { return Err(LookError.NotUtf8); }
    };
    let found = if (config.ignoreCase) searchIgnoreCase(config.query, text)? else search(config.query, text)?;
    for (line in found) {
        Console.writeLine(line);
    }
    return Ok(found.length);
}
```

In Listing 12-26, `Ok(count)` takes the count, and `main` exits with 1 if it is 0. When `main` reaches its end
without calling `Process.exit`, the status is 0.

```sh
./out/lookfor whale harbour.txt
echo $?
```

```text
1
```

A shell can branch on the status. `||` runs its right side when the left side's status is not 0:

```sh
./out/lookfor whale harbour.txt || echo "no whale here"
```

```text
no whale here
```

## `main` may return the status

`Process.exit` ends the program from anywhere. From `main` itself there is a second way: `main` may return an
`i32`, and that value is the exit status.

<span class="listing-caption"><strong>Listing 12-28</strong>, <code>main.cee</code>: The value <code>main</code> returns is the program's exit status, here 3.</span>

<!-- ceesafe: exits 3 -->
```ceesafe
fn main() -> i32 {
    Console.writeLine("leaving");
    return 3;
}
```

```text
leaving
```

`lookfor` uses `Process.exit` because its first `match` must leave from inside an arm that gives no value.
Either way, `main` returns nothing or an `i32`. It cannot return a `Result`.

::: tip Rule
The exit status is the argument of `Process.exit`, or the `i32` that `main` returns, or 0 when `main` returns
nothing.
:::

## Mistakes

::: warning Mistake: a number to standard error
`Console.writeLine` prints numbers and booleans as well as text. `Console.writeError` and
`Console.writeErrorLine` take text only.
:::

<span class="listing-caption"><strong>Listing 12-29</strong>, <code>main.cee</code>: A number given to <code>Console.writeErrorLine</code>. This does not compile.</span>

<!-- ceesafe: fail N0205 -->
```ceesafe
fn main() {
    let count = 3;
    Console.writeErrorLine(count);
}
```

```text
main.cee:3:28: error[N0205]: expected 'StringView', found 'i32'
  |
3 |     Console.writeErrorLine(count);
  |                            ^^^^^
```

Build the message in a `String`, which can append a number, and write its view:

<span class="listing-caption"><strong>Listing 12-30</strong>, <code>main.cee</code>: The message is built in a <code>String</code> and written to standard error as a view.</span>

```ceesafe
fn main() {
    let count = 3;
    var message = String.from("lines found: ")!;
    message.appendNumber(count)!;
    Console.writeErrorLine(message.view());
}
```

The program writes nothing to standard output. On standard error it writes:

```text
lines found: 3
```

::: warning Mistake: an error message on standard output
`Console.writeLine("error: …")` works in a terminal and fails the first time someone writes `>` or `|` after
your program. The compiler cannot catch this one. Decide for each line the program prints whether it is a
result or a remark about the run.
:::

::: details Coming from Rust, C, Go or Python
`Console.writeErrorLine(text)` is `eprintln!`, `fputs(text, stderr)`, `fmt.Fprintln(os.Stderr, text)` and
`print(text, file=sys.stderr)`, except that it takes one text and does no formatting. `Process.exit` is
`std::process::exit`, `exit`, `os.Exit` and `sys.exit`. Returning an `i32` from `main` is C's `return` from
`main`; unlike Rust, `main` cannot return a `Result`.
:::

## Summary

- A program has two output streams. `>` in a shell redirects standard output only.
- `Console.write` and `Console.writeLine` write results to standard output; `Console.writeError` and
  `Console.writeErrorLine` write text to standard error (`N0205` for anything that is not text).
- Keeping every message in one function made this a change of a few lines.
- The exit status tells the caller how the run went: `Process.exit(status)`, or an `i32` returned from `main`.
  `lookfor` gives 0 for lines found, 1 for none, 2 for an error.

## The finished program

`lookfor` is two files and about 150 lines. It reads its arguments and its environment in one function,
reads a file and checks that it is text, searches it without copying a line, has three tests, reports every
failure in one place on the right stream, and tells its caller what happened. No part of it can read memory it
should not, and no error can pass unhandled, because the compiler checked both.

Some things to add, each with its test first:

- a line number before each line found;
- a `-i` argument that does what `LOOKFOR_IGNORE_CASE` does;
- several files, with the file's name before each line;
- a test for `containsIgnoreCase` with a query longer than the line.

This is the end of the course. The [introduction](./ch00-00-introduction#how-the-book-is-laid-out) says how
the rest of the book is laid out.
