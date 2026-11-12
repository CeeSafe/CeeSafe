---
title: Hello, package
summary: A package is a directory with ceesafe.toml and src/. How to make one with ceesafec new, and the four everyday commands - check, run --interp, build and fmt - with what each does and prints.
assumes: [ch01-02-hello-world]
keywords: [package, ceesafe.toml, "ceesafec new", check, "run --interp", build, fmt, out, AGENTS.md]
diagnostics: [N0200]
---

# Hello, package

**This section teaches** what a package is, how to make one, and the four commands you will use every day. It
assumes [Hello, world](./ch01-02-hello-world).

One file is enough for an experiment. A real program is a **package**: a directory that holds the program's source
files and a small file that describes it. The compiler's commands work on the package you are in, so you stop
naming files on the command line.

## Making a package

```sh
ceesafec new hello
cd hello
```

```text
ceesafec: made executable package 'hello' in hello
ceesafec: hello/AGENTS.md: wrote
```

That made a directory, `hello`, with four files:

```text
hello/
├── ceesafe.toml      the package's description
├── src/
│   └── main.cee      the program
├── AGENTS.md         instructions for AI coding agents working in this package
└── .gitignore        tells git to ignore out/, where builds go
```

`AGENTS.md` is for an AI assistant: it says what CeeSafe is, which commands to run, and the mistakes models make.
`ceesafec new hello --no-agent` leaves it out.

## The manifest: `ceesafe.toml`

```toml
[package]
name = "hello"
version = "0.1.0"
kind = "executable"

[dependencies]
```

- `name` is the package's name, and the name of the executable `build` makes.
- `version` is yours to change as the package changes.
- `kind` is `executable` for a program with a `main`, or `library` for code other packages use.
- `[dependencies]` lists the packages this one uses. It is empty; chapter 7 fills it.

::: tip Rule
A package is a directory with a `ceesafe.toml`. Its source is every `.cee` file under `src/`.
:::

## The program

`src/main.cee` already holds a program. Replace it with this one:

<span class="listing-caption"><strong>Listing 1-6</strong>, <code>src/main.cee</code>: A program with a function of its own.</span>

```ceesafe
fn greeting(name: StringView) {
    Console.write("Hello, ");
    Console.write(name);
    Console.writeLine("!");
}

fn main() {
    greeting("world");
    greeting("CeeSafe");
}
```

```text
Hello, world!
Hello, CeeSafe!
```

`greeting` is a function with one parameter, `name`, whose type is `StringView`: a piece of text that the function
looks at and does not own. Chapter 3 covers functions and chapter 8 covers text.

## The four commands

Run each of these in the package's directory, or any directory inside it.

### `ceesafec check`: is the program right?

```sh
ceesafec check
```

`check` reads the whole package and reports every error: syntax, types, and the ownership rules of chapter 4. It
prints nothing when there is nothing wrong. It builds nothing, so it is the fastest command, and the one to run
most.

### `ceesafec run --interp`: run it now

```sh
ceesafec run --interp
```

```text
Hello, world!
Hello, CeeSafe!
```

This checks the package and runs it in the interpreter, as in the last section, without naming a file.

### `ceesafec build`: make the executable

```sh
ceesafec build
./out/hello
```

`build` compiles the package through C to an executable, `out/hello` (`out\hello.exe` on Windows). The `out`
directory is the compiler's to write; do not keep your own files there, and do not commit it.

### `ceesafec fmt`: one style

```sh
ceesafec fmt
```

`fmt` rewrites the package's source files in CeeSafe's one style: four spaces, braces where this book puts them.
There are no options, so every CeeSafe program looks the same. `ceesafec fmt --check` changes nothing and lists the
files that `fmt` would change, which is what to run before sharing your code.

| Command | What it does | Needs a C compiler |
|---|---|---|
| `ceesafec check` | finds every error; builds nothing | no |
| `ceesafec run --interp` | checks, then runs in the interpreter | no |
| `ceesafec build` | compiles to `out/<name>` | yes |
| `ceesafec fmt` | rewrites the sources in the one style | no |

## Mistakes

::: warning Mistake: a name the compiler cannot find
A misspelt function is an error when you check, not when the program reaches the call.
:::

<span class="listing-caption"><strong>Listing 1-7</strong>, <code>src/main.cee</code>: The function is called <code>greeting</code>, not <code>greet</code>. This does not compile.</span>

<!-- ceesafe: fail N0200 -->
```ceesafe
fn greeting(name: StringView) {
    Console.write("Hello, ");
    Console.write(name);
    Console.writeLine("!");
}

fn main() {
    greet("world");
}
```

```text
main.cee:8:5: error[N0200]: cannot find function 'greet' in this scope
  |
8 |     greet("world");
  |     ^^^^^
```

::: warning Mistake: running a command outside a package
`ceesafec check` with no file, in a directory that has no `ceesafe.toml` in it or above it, has nothing to check:

```text
ceesafec: no CeeSafe input files, and no ceesafe.toml here or above
```

Change to the package's directory, or name a file as in the last section.
:::

::: details Coming from Rust
`ceesafec` is both `rustc` and `cargo`: `ceesafe.toml` is `Cargo.toml`, `ceesafe.lock` is `Cargo.lock`, `out/` is
`target/`. `ceesafec check` is `cargo check`. There is no `cargo run` of a built binary under that name:
`ceesafec run --interp` interprets, and `ceesafec build` then `./out/<name>` runs native code.
:::

::: details Coming from C
There is no makefile and no header. A package's files see each other's functions without declarations, and
`ceesafec build` drives the C compiler for you.
:::

## Summary

- A package is a directory with `ceesafe.toml` and `src/`; `ceesafec new <name>` makes one.
- `check` finds errors, `run --interp` runs, `build` makes `out/<name>`, `fmt` formats.
- The commands work on the package of the directory you are in.
- An unknown name is a compile-time error (`N0200`).

You have a compiler and a program of your own. Chapter 2 builds a game with them.
