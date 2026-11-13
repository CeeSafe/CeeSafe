---
title: "Appendix I: For AI models"
summary: How an AI model reads this book and writes CeeSafe - the machine-readable files and their addresses, how to find the section for a diagnostic, the compiler's tools for a model, and the mistakes models make with the right form of each.
keywords: [AI, model, agent, llms.txt, llms-full.txt, index.json, listings, MCP, ceesafec mcp, ceesafec agents, AGENTS.md, --diagnostics-format=json, ceesafec fix, ceesafec explain]
---

# Appendix I: For AI models

This appendix is for an AI model that writes or repairs CeeSafe, and for the person who sets one up. It lists the
files of this book that are made for a program to read, says in which order to use them, lists the tools `ceesafec`
gives a model, and ends with the mistakes models make most, each with the form that compiles. Everything here is
true of ceesafec %ceesafec-version%.

CeeSafe is new, so a model has seen little of it. What a model writes from memory is usually Rust, C# or TypeScript
that looks like CeeSafe. Read the section before writing, and check every edit with the compiler.

## The machine-readable files

The book is published at `https://ceesafe.github.io/CeeSafe/book/`. Beside its pages, the same build writes five
kinds of file from the same sources. Their addresses do not change when a title does.

| File | Address | What it holds |
|---|---|---|
| `llms.txt` | [https://ceesafe.github.io/CeeSafe/book/llms.txt](https://ceesafe.github.io/CeeSafe/book/llms.txt) | The table of contents as Markdown. One line for each written page, in reading order, with its number, its title, the address of its Markdown and its one-sentence summary. Then the pages that are planned and not written. |
| `llms-full.txt` | [https://ceesafe.github.io/CeeSafe/book/llms-full.txt](https://ceesafe.github.io/CeeSafe/book/llms-full.txt) | The whole book as one Markdown document, more than a megabyte. The pages follow each other in reading order, each under a first-level heading with its number, with a line `---` between two pages. |
| `index.json` | [https://ceesafe.github.io/CeeSafe/book/index.json](https://ceesafe.github.io/CeeSafe/book/index.json) | Every page's header and every numbered listing, as JSON. The next two tables give its fields. |
| A page's Markdown | the page's address with `.md` added, for example [https://ceesafe.github.io/CeeSafe/book/ch04-02-borrowing.md](https://ceesafe.github.io/CeeSafe/book/ch04-02-borrowing.md) | One page as Markdown, as its author wrote it, without the header. Its first line is a heading with the page's number and title. The title page is `index.md`. |
| A listing | `listings/`, then the page's id, the listing's number and an extension, for example [https://ceesafe.github.io/CeeSafe/book/listings/ch01-02-hello-world-1-1.cee](https://ceesafe.github.io/CeeSafe/book/listings/ch01-02-hello-world-1-1.cee) | The code of one numbered listing and nothing else. The extension is `.cee` for CeeSafe, and the language's name for another language, such as `.toml` or `.c`. |

A page's id is the name of its file: `ch04-02-borrowing` is section 4.2. An id never changes. A listing's number
can change when a listing is added before it, so keep the page's id with it.

### The fields of `index.json`

The document has four keys: `book` (the title), `base` (the address everything is under), `sections` and
`listings`. Each entry of `sections` is one page:

| Field of a section | Type | What it holds |
|---|---|---|
| `id` | text | The page's id, `ch04-02-borrowing`. The title page is `index`. |
| `number` | text | `4` for a chapter's opening page, `4.2` for a section, empty for the title page, the introduction and the appendices. |
| `title` | text | The page's title. |
| `summary` | text | One sentence that says what the page teaches. It is the sentence `llms.txt` shows. |
| `assumes` | list of ids | The pages this one assumes the reader knows. |
| `keywords` | list of text | Names and terms the page covers: keywords, types, functions, options, diagnostic codes. |
| `diagnostics` | list of codes | The diagnostic codes, such as `N0240`, that the page shows in a listing or a compiler message. |
| `specification` | list of text | The sections of the specification the page's rules come from, such as `type-system.md §25.4`. |
| `listings` | list of numbers | The numbers of the page's listings, such as `4-9`. |
| `url` | text | The page's address. |
| `markdown` | text | The address of the page's Markdown. |

Each entry of `listings` is one numbered listing:

| Field of a listing | Type | What it holds |
|---|---|---|
| `number` | text | The listing's number, chapter then count, `4-9`. |
| `section` | text | The id of the page that shows it. |
| `file` | text | The file's name as the caption gives it. `main.cee` is a program of one file. A path such as `scores/src/main.cee` is a file of a package. |
| `language` | text | `ceesafe`, or another language's name, such as `toml` or `c`. |
| `caption` | text | The sentence that says what the listing shows. |
| `check` | text | What the book's check does with it. The table below gives the values. |
| `output` | text or `null` | Exactly what the program prints, or exactly what the compiler says, as the page shows it beneath the listing. `null` when the page shows neither. |
| `url` | text | The address of the listing's file. |

| Value of `check` | What it means |
|---|---|
| `runs` | The listing is a whole program. It compiles, runs and exits with status 0, and `output` is what it prints. |
| `fail N0240` | It must not compile. The compiler reports the codes named, and `output` is the compiler's message. |
| `warns N0400` | It compiles and runs, and the compiler gives the warnings named. |
| `panics` | It compiles, and panics when it runs. |
| `exits 3` | It compiles, runs, and exits with that status. |
| `check` | It is type-checked and not run, because it reads input or calls C. |
| `freestanding` | It is checked with `--profile freestanding`. |
| `skip` | It is one file of a package of several files, or part of one, and may not compile alone. The page names the file. |

An entry of `sections`, shortened:

```json
{
  "id": "ch05-03-methods",
  "number": "5.3",
  "title": "Methods",
  "summary": "Functions that belong to a struct - methods written in the struct's body or in an impl block, …",
  "assumes": ["ch05-01-defining-structs", "ch04-02-borrowing", "ch03-03-functions"],
  "keywords": ["method", "this", "borrow this", "borrow mut this", "receiver", "static function", "constructor", "impl"],
  "diagnostics": ["N0200", "N0228", "N0233", "N0248", "N0250"],
  "specification": ["type-system.md §12", "type-system.md §21.1"],
  "listings": ["5-15", "5-16", "5-17", "5-18", "5-19", "5-20", "5-21", "5-22", "5-23"],
  "url": "https://ceesafe.github.io/CeeSafe/book/ch05-03-methods",
  "markdown": "https://ceesafe.github.io/CeeSafe/book/ch05-03-methods.md"
}
```

## How to use the book

| To do this | Read this |
|---|---|
| Find the page for a subject | `llms.txt` first. It is small, and each line's summary says what the page teaches. Then fetch that page's Markdown. Do not start with `llms-full.txt` unless the whole book fits your context. |
| Find the page for a diagnostic code | In `index.json`, the sections whose `diagnostics` list holds the code. Each shows the code in a listing that fails with it, beside the fix. [Appendix F](./appendix-f-diagnostics) lists every code. |
| See code that gets a diagnostic | In `index.json`, the listings whose `check` is `fail` with that code. `output` is the exact message, and the page shows the fix after it. |
| Find the page for a name | In `index.json`, the sections whose `keywords` list holds the name, such as `HashMap`, `borrow mut` or `--emit`. |
| Get a program to start from | A listing whose `check` is `runs`. Its file is a whole program: save it as `main.cee` and run `ceesafec run --interp main.cee`. |
| Learn what a section needs | Its `assumes` list, and the first paragraph of the page, which links the same pages. A section stands alone otherwise. |
| Quote a rule | The page's Markdown. A rule is in a box that starts `::: tip Rule`, in one or two sentences, with the specification's section as its last line. |
| Avoid a known mistake | The page's part `## Mistakes`. Each is a box that starts `::: warning Mistake`, then code that does not compile, the compiler's message, and the fix. |
| Learn what differs from another language | The boxes that start `::: details Coming from`, after a page's mistakes. |

Three things about the listings matter to a model:

- **A listing is a whole file.** A listing whose `check` is not `skip` compiles as it stands. There is nothing to
  add around it and nothing hidden.
- **A listing that fails is there on purpose.** Read `check` before copying code: about a third of the book's
  listings are mistakes, shown with the diagnostic they get.
- **The output is real.** Each `output` was compared with a run of the listing when the book was built.

The book teaches. For a name's exact signature, use the library's pages and the specification, which are the
authority where the book and they differ. They have their own files of the same kind:
[https://ceesafe.github.io/CeeSafe/llms.txt](https://ceesafe.github.io/CeeSafe/llms.txt) is their index, and
[https://ceesafe.github.io/CeeSafe/llms-full.txt](https://ceesafe.github.io/CeeSafe/llms-full.txt) holds the guide,
the specification and the library's pages in one file. The book is not inside `ceesafec`: the guide and the
specification are, and the MCP server below serves them.

## The tools a model has

`ceesafec` is built to be used by a program. [Hello, package](./ch01-03-hello-package) introduces the commands.

| Tool | What it does for a model |
|---|---|
| `ceesafec check` | Parses, type-checks and borrow-checks the package, or the files named. It is the fastest way to find errors. Run it after every edit. |
| `--diagnostics-format=json` | With `check`, `build`, `run`, `parse` or `fix`, writes the diagnostics as one JSON document on one line of standard output, in place of text on standard error. Each diagnostic has its code, its position, its message and its fixes. |
| `ceesafec fix` | Applies every fix the compiler is sure of, writes the files and checks again, for up to 8 rounds. `--dry-run` prints the edits and changes nothing. It exits with status 1 if errors remain. |
| `ceesafec explain N0249` | Prints what a diagnostic code means, with an example. An unknown code is exit status 2. |
| `ceesafec run --interp` | Checks the program and runs it in the compiler's interpreter, with no C compiler. A panic is exit status 134. |
| `ceesafec fmt` | Rewrites the sources in the one canonical style. `ceesafec fmt --check` lists the files that would change. |
| `ceesafec mcp` | Serves the Model Context Protocol on standard input and output. Its tools are in the next table. |
| `ceesafec new NAME --agent claude` | Makes a package with `AGENTS.md`, the instructions most coding agents read. With `--agent`, it also writes that agent's own file and its MCP configuration. The names are `agents` (the default), `claude`, `gemini`, `copilot` and `cursor`. |
| `ceesafec agents` | In a package, writes or refreshes the agent instructions. It replaces the text between `<!--CEESAFE START-->` and `<!--CEESAFE END-->` in `AGENTS.md` and keeps the rest. Run it after updating `ceesafec`. |

The JSON document for one error, printed here on several lines:

```json
{
  "version": 1,
  "errors": 1,
  "warnings": 0,
  "diagnostics": [
    {
      "code": "N0249",
      "severity": "error",
      "file": "main.cee",
      "start": { "line": 7, "column": 10 },
      "end": { "line": 7, "column": 11 },
      "message": "this parameter is 'borrow mut': write 'mut' before the argument to let the call modify it",
      "summary": "`mut` argument marker missing or misplaced",
      "notes": [],
      "fixes": [
        {
          "title": "write 'mut' before the argument",
          "edits": [
            { "file": "main.cee", "start": { "line": 7, "column": 10 }, "end": { "line": 7, "column": 10 }, "newText": "mut " }
          ]
        }
      ]
    }
  ]
}
```

Lines and columns start at 1, and `end` is the position after the last character. A fix is offered only where one
repair is certain: `mut` before an argument (`N0249`), `let` changed to `var` (`N0211`, `N0248`), `truncate<T>(…)`
for a narrowing `as` (`N0208`), a missing `;` (`N0100`), and a duplicate C declaration removed (`N0201`). Every
other diagnostic is for the model to repair, with its message, `ceesafec explain` and the section that shows the
code. The details are in the specification: https://ceesafe.github.io/CeeSafe/reference/tooling.

### The MCP server

Add the server to an MCP client with this configuration. `ceesafec new` with `--agent` writes it for you.

```json
{ "mcpServers": { "ceesafe": { "command": "ceesafec", "args": ["mcp"] } } }
```

A tool that compiles takes its program as `files`, a list of `{ "path": …, "text": … }` compiled as one package
with nothing read from disk, or as `paths`, a list of `.cee` files on disk.

| MCP tool | What it returns |
|---|---|
| `ceesafe_check` | The diagnostics of the program as the JSON document above, with fixes. |
| `ceesafe_fix` | Each file's text with every certain fix applied, the number of fixes, and the diagnostics that remain. It writes nothing to disk. |
| `ceesafe_format` | One file's text in the canonical style, or the syntax errors that stop it. |
| `ceesafe_run` | The program's output and exit code from a run in the interpreter, sandboxed: no files, sockets or processes, and a limit on steps, memory and output. It takes `stdin` and `args`. |
| `ceesafe_emit_c` | The C the compiler generates, and on request where it placed allocations, bounds checks and overflow checks. |
| `ceesafe_explain` | What a diagnostic code means, an example, and what the diagnostics reference says about it. |
| `ceesafe_spec_search` | The sections of the specification and the guide whose heading or text hold every word of a query. |
| `ceesafe_spec_section` | The text of one section of a document, such as `type-system` section `31.2`. |
| `ceesafe_symbol` | The type or signature of the name at a line and column, and where it is declared. |
| `ceesafe_api` | The signature and documentation of a library or built-in name, such as `Vector.push` or `Process.argument`, or the closest real names when there is none. Use it before deciding that a name does not exist. |

The server also has two prompts, `write-ceesafe` (the guide for agents, then a task) and `port-c-to-ceesafe`, and it
serves each document embedded in the compiler as a resource whose address starts `ceesafe://docs/`. Those documents
are the guide, the specification and its decision records, so their answers match the compiler that gives them.

### The loop

1. Write the code.
2. Check it: `ceesafe_check`, or `ceesafec check --diagnostics-format=json`.
3. Apply the certain fixes: `ceesafe_fix`, or `ceesafec fix`.
4. Repair the rest. For each code, read `ceesafec explain` and the section of this book whose header names the
   code. For a name the compiler does not know, look it up with `ceesafe_api`. A "did you mean" in a message names
   the real one.
5. Repeat until there are no errors. Then run `ceesafec fmt`, and run the program: `ceesafe_run`, or
   `ceesafec run --interp`.

## The mistakes models make

Each row is a form that a model writes from habit, what ceesafec %ceesafec-version% says about it, the form to write,
and the section that teaches it. Every wrong form was compiled to get its code, and every right form was compiled
and run. The first table is the list that `ceesafec new` writes into `AGENTS.md`.

| A model writes | The compiler says | Write instead | Section |
|---|---|---|---|
| `~x`, to complement the bits | `N0102`, expected expression | `x ^ 0xFFFFFFFF`, with as many `F`s as the type is wide. There is no `~`. | [20.4](./ch20-04-arithmetic) |
| `type Id = u32;` | `N0101` | The type itself, or a struct with one field. There are no type aliases. | [3.2](./ch03-02-data-types) |
| `let p = (1, 2);` | `N0100`, expected `)` | A struct with named fields. There are no tuples. | [3.2](./ch03-02-data-types) |
| `a == b` on two structs | `N0207` | `@derive(Equal)` on the line before the struct, or compare the fields. | [5.1](./ch05-01-defining-structs), [20.5](./ch20-05-derives-and-macros) |
| `o.unwrapOr(7)` on an optional | `N0234`, and the message lists the real methods | `o ?? 7`. `o.isSome()` and `o.isNull()` test it. | [6.3](./ch06-03-optionals) |
| `v.push(1);`, a `Result` that nothing reads | the warning `N0402` | `v.push(1)!;` to panic on failure, `v.push(1)?;` to return it, or `let _ = v.push(1);` to discard it. | [9.2](./ch09-02-result), [8.1](./ch08-01-vectors) |
| `a[i]` where `i` is an `i32` | `N0205`, expected `usize` | An index of type `usize`. In `for (i in 0..a.length)`, `i` is a `usize`. | [3.5](./ch03-05-control-flow), [8.1](./ch08-01-vectors) |
| `let s = if (c) { -1; } else { 1; };` | `N0225` and `N0235` | `let s = if (c) -1 else 1;`, or blocks that end with their value and no `;`. | [3.5](./ch03-05-control-flow), [6.2](./ch06-02-match) |
| `let n = 1;` and later `n = 2;` | `N0211` | `var n = 1;`. A `let` cannot be assigned. | [3.1](./ch03-01-variables) |
| `bump(n)` for a `borrow mut` parameter | `N0249` | `bump(mut n)`. The parameter's type says `borrow mut`, and the argument says `mut`. | [4.2](./ch04-02-borrowing) |
| `n as u8` where `n` is an `i32` | `N0208` | `truncate<u8>(n)` to wrap, `saturatingTruncate<u8>(n)` to clamp, `checkedTruncate<u8>(n)` for an optional. `as` converts only when nothing can be lost. | [3.2](./ch03-02-data-types), [20.4](./ch20-04-arithmetic) |
| `"x = {x}"`, to print a value | the warning `N0400`, and the program prints `{x}` | `Console.write("x = ");` then `Console.writeLine(x);`. There is no string interpolation. | [1.2](./ch01-02-hello-world), [8.2](./ch08-02-strings) |
| `let v = make().view();` and `v` used later | `N0243` | `let s = make();` then `let v = s.view();`. A temporary lasts until the end of its statement. | [4.3](./ch04-03-slices-and-views) |
| `enum Token { Word(StringView) }`, or a struct with such a field | `N0270` | `enum Token<region r> { Word(StringView<r>) }`. For fixed messages, an enum of reasons and a function that gives each its text. To own the text, a `String`. | [10.3](./ch10-03-regions) |
| A method with the name of a field | `N0228` | Another name for the method. Fields, methods and static functions of a struct share one set of names. | [5.3](./ch05-03-methods) |
| `let name: StringView = Process.argument(1);` | `N0205`, found `StringView?` | `Process.argument(1) ?? "nobody"`. A lookup that may find nothing returns an optional. | [12.1](./ch12-01-arguments), [6.3](./ch06-03-optionals) |
| `let type = 1;` | `N0100`, `type` is a reserved word | Another name. `own`, `type`, `move`, `drop` and `where` are reserved too. | [3.2](./ch03-02-data-types) |
| `Console.writeLine(…)` in an `async fn` | `N0295`, an async function must not block its thread | `await AsyncConsole.writeLine(…)?;` | [17.4](./ch17-04-async-io) |
| `fn die(m: StringView) { …; Process.exit(1); }`, called as the last statement of a function that returns a value | `N0213` in the caller, the end of its body is reachable | `fn die(m: StringView) -> never { …; Process.exit(1); }`. A function that always exits returns `never`. | [3.3](./ch03-03-functions) |
| `struct Rule { test: borrow fn(i32) -> bool; }`, a struct that holds a closure | `N0270` | `struct Rule<region r> { test: borrow(r) fn(i32) -> bool; }`. The closure in the field is called as `rule.test(5)`. | [13.1](./ch13-01-closures) |

The second table is habits from other languages that the first list does not name. They were found the same way.

| A model writes | The compiler says | Write instead | Section |
|---|---|---|---|
| `struct Point { x: i32, y: i32, }` | `N0100`, expected `;` | `struct Point { x: i32; y: i32; }`. A field ends with `;`. A struct literal, `Point { x: 1, y: 2 }`, uses commas. | [5.1](./ch05-01-defining-structs) |
| `1 => Console.writeLine("one"),` as an arm of a `match` statement | `N0100`, expected `{` | `1 => { Console.writeLine("one"); }`. An arm of a `match` statement is a block. An arm of a `match` that gives a value is an expression and a comma. | [6.2](./ch06-02-match) |
| `two().await` | `N0100`, `await` is a reserved word | `await two()`. `await` comes first. | [17.1](./ch17-01-tasks-and-await) |
| `length(&s)` | `N0102`, expected expression | `length(s)`. A shared borrow needs no mark at the call, and an exclusive one is `mut s`. | [4.2](./ch04-02-borrowing) |
| `let mut n = 1;` | `N0100`, `mut` is a reserved word | `var n = 1;` | [3.1](./ch03-01-variables) |
| `Colour::Red` | `N0100` | `Colour.Red`. Every path uses a dot. | [6.1](./ch06-01-defining-an-enum) |
| `return None;` for an absent value | `N0200`, cannot find `None` | `return null;`. A present value is `Some(v)`. | [6.3](./ch06-03-optionals) |
| `-> Option<i32>` | `N0204` | `-> i32?` | [6.3](./ch06-03-optionals) |
| `a.len()` | `N0200`, and the message lists the real methods | `a.length`, a field, with no parentheses. | [8.1](./ch08-01-vectors) |
| `if x > 2 { … }` | `N0100`, expected `(` | `if (x > 2) { … }`. The conditions of `if` and `while`, and the value of `match`, are in parentheses. | [3.5](./ch03-05-control-flow) |

One program with the right forms of the first table:

```ceesafe
fn parse(text: StringView) -> u32? {
    return text.parseInt<u32>();
}

fn add(total: borrow mut u32, amount: u32) {
    *total += amount;
}

fn main() {
    var parts = Vector<u32>.create();
    for (part in "12,x,300".split(",")) {
        let _ = parts.push(parse(part) ?? 0);
    }
    var total: u32 = 0;
    for (i in 0..parts.length) {
        add(mut total, parts[i]);
    }
    let size = if (total > 255) "large" else "small";
    Console.write("total = ");
    Console.writeLine(total);
    Console.writeLine(size);
    Console.writeLine(truncate<u8>(total));
    Console.writeLine(total ^ 0xFFFFFFFF);
}
```

```text
total = 312
large
56
4294966983
```

Four more points have no row, because there is no one wrong form that the compiler rejects:

- **A child task's captures.** A child written as an `async move` closure owns what it captures, and may move it
  on. A child written as a plain `async` closure borrows. [Scopes](./ch17-02-scopes) covers both.
- **A loop over the parts of a text.** `for (part in text.split(","))`, as in the program above. `for` loops over
  any value with a method `next` that returns an optional. [Iteration](./ch13-02-iteration) covers it.
- **A multi-line string.** It is a raw literal between `"""` lines. The indentation of the closing `"""` is
  removed from every line, and there are no escapes in it: `\n` stays a backslash and an `n`. The guide shows it:
  https://ceesafe.github.io/CeeSafe/guide/for-agents.
- **A name from another language.** Look the name up before deciding that it does not exist, and before inventing
  one: `ceesafe_api`, or the library's pages at https://ceesafe.github.io/CeeSafe/reference/api/core and
  https://ceesafe.github.io/CeeSafe/reference/api/std.

## Before a model says it is done

- `ceesafec check` reports no errors, after `ceesafec fix`.
- `ceesafec fmt --check` lists no file.
- The program does what was asked: `ceesafec run --interp`, or `ceesafec build` and a run of what it built.
