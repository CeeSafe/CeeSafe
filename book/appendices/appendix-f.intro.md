---
title: "Appendix F: Diagnostics"
summary: Every diagnostic code that ceesafec can report, error or warning, with one line on what went wrong and the usual fix, and the section of the book that shows it.
keywords: [diagnostic, error, warning, code, explain, fix, "--diagnostics-format", "--deny-warnings", N0100, N0200, N0205, N0241, N0249, N0250, N0402]
---

# Appendix F: Diagnostics

This appendix lists every diagnostic code of `ceesafec` %ceesafec-version%: 154 errors and 6 warnings. Find the code from your
message (`error[N0249]`) in the tables, which are in the order of the numbers. Each row says what went wrong and the
usual fix, and links to the sections of the book that show the diagnostic in a listing, with its message and the
corrected program. A code that no section shows links to its table in the specification.

## How to read a diagnostic

This program leaves out `mut` in a call:

<!-- ceesafe: fail N0249 -->
```ceesafe
fn main() {
    var line = String.create();
    if (Console.readLine(line)!) {
        Console.writeLine(line);
    }
}
```

```text
main.cee:3:26: error[N0249]: this parameter is 'borrow mut': write 'mut' before the argument to let the call modify it
  |
3 |     if (Console.readLine(line)!) {
  |                          ^^^^
```

The parts of the first line, in order:

| Part | Here | What it is |
|---|---|---|
| The place | `main.cee:3:26` | The file, the line and the column where the fault starts. Both count from 1. |
| The kind | `error` | `error` or `warning`. An error stops the command. A warning does not. |
| The code | `N0249` | The diagnostic's code. It never changes its meaning from one release to the next. |
| The message | `this parameter is …` | What is wrong in this program, with its names and types. One code can have several messages. |

Under it is the source line, with `^` marks under the part the diagnostic is about. Some diagnostics have notes
after that, each with its own place: where a value was moved, where a borrow was taken.
[Hello, world](./ch01-02-hello-world) reads a first message in full, and [Borrowing](./ch04-02-borrowing) one with a
note.

Diagnostics come in the order the compiler finds them. Correct the first one first: a later one is often a
result of it.

### `ceesafec explain`

`ceesafec explain` prints what a code means, with an example, from the specification inside the compiler. It needs
no network ([1.1](./ch01-01-installation)).

```sh
$ ceesafec explain N0249
N0249: `mut` argument marker missing or misplaced
example: `take(x)` for `borrow mut`, `look(mut x)` for `borrow`
```

For some codes it also prints the specification's paragraphs that name the code.

### `ceesafec fix`

Where only one repair is possible, the compiler offers it as a fix. `ceesafec fix` applies every fix to the files,
checks again, and repeats until no fix is left. Then it prints the diagnostics that remain. `ceesafec fix --dry-run`
changes no file, and prints the edits:

```sh
$ ceesafec fix --dry-run main.cee
main.cee:3:26: write 'mut' before the argument: '' -> 'mut '
```

Release %ceesafec-version% has fixes for six codes: N0100 (a missing `;`), N0201 (an extern declaration that repeats a
dependency's), N0208 (`as` to `truncate`), N0211 and N0248 (`let` to `var`), and N0249 (`mut` added or removed). No
chapter of the book teaches `fix`. The page
[The ceesafec command](https://ceesafe.github.io/CeeSafe/guide/ceesafec) of the guide does.

### `--diagnostics-format=json`

A program that reads diagnostics, an AI model among them, asks for them as JSON. With
`--diagnostics-format=json`, the commands `check`, `build`, `run` and `fix` write one JSON document, on one line,
to standard output. For the program above:

```sh
$ ceesafec check --diagnostics-format=json main.cee
{"version":1,"errors":1,"warnings":0,"diagnostics":[{"code":"N0249","severity":"error","file":"main.cee","start":{"line":3,"column":26},"end":{"line":3,"column":30},"message":"this parameter is 'borrow mut': write 'mut' before the argument to let the call modify it","summary":"`mut` argument marker missing or misplaced","notes":[],"fixes":[{"title":"write 'mut' before the argument","edits":[{"file":"main.cee","start":{"line":3,"column":26},"end":{"line":3,"column":26},"newText":"mut "}]}]}]}
```

Each diagnostic has its `code`, its `severity`, its place (`file`, `start`, and `end`, which is the position after
the last character), the `message`, the code's one-line `summary`, its `notes` and its `fixes`, each fix a list of
edits. The specification gives the format in full:
[Tooling, section 5](https://ceesafe.github.io/CeeSafe/reference/tooling). The options of each command are in
[Appendix E](./appendix-e-ceesafec).

## How to read the tables

- **Code.** The tables are in the order of the numbers, in the groups of the specification. Search the page for
  your code.
- **Kind.** `error` or `warning`.
- **What went wrong, and the usual fix.** One line in the book's words. The compiler's message for your program
  says more, and `ceesafec explain` gives the specification's wording.
- **Shown in.** The sections with a listing that produces the diagnostic, by number: `4.2` is chapter 4, section
  2. For a code that no section shows, the link goes to the code's table in
  [the specification's list of diagnostics](https://ceesafe.github.io/CeeSafe/reference/diagnostics).
