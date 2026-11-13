# Getting started

This page makes a package, builds and runs it, and shows the commands you will use every day. It assumes `ceesafec` is
[installed](installation.md) and on your `PATH`.

## A new package

```sh
ceesafec new hello
cd hello
```

`ceesafec new` writes a manifest, a first source file, and instructions for AI coding agents:

```text
hello/
├── ceesafe.toml
├── src/
│   └── main.cee
├── AGENTS.md
└── .gitignore
```

`AGENTS.md` tells an agent working in the package what CeeSafe is and how to check its work. With
`ceesafec new hello --agent claude` (or `gemini`, `copilot`, `cursor`) it also writes that agent's own file and
configures CeeSafe's MCP server for it; `--no-agent` writes none. See [CeeSafe for AI agents](for-agents.md).

`ceesafe.toml` names the package and says what it builds:

```toml
[package]
name = "hello"
version = "0.1.0"
kind = "executable"

[dependencies]
```

and `src/main.cee` is a program:

```ceesafe
fn main() {
    Console.writeLine("Hello from hello");
}
```

```text
Hello from hello
```

## Build and run

Inside a package, the commands need no file names:

```sh
ceesafec build            # compiles to C, then to out/hello
./out/hello
ceesafec run --interp     # runs the program in the reference interpreter instead
ceesafec check            # type and borrow checks only: the fastest way to find errors
ceesafec fmt              # formats the package's sources in the canonical style
```

Every `.cee` file under `src/` belongs to the package, and all of them share one scope: a function declared in one
file can be called from another without an import.

## A slightly larger program

Replace `src/main.cee` with:

```ceesafe
/// The number of steps the Collatz sequence takes from `n` to 1.
fn steps(n: u64) -> u32 {
    var value = n;
    var count: u32 = 0;
    while (value != 1) {
        value = if (value % 2 == 0) value / 2 else 3 * value + 1;
        count += 1;
    }
    return count;
}

fn main() {
    var longest: u64 = 1;
    for (n in 1..1000u64) {
        if (steps(n) > steps(longest)) {
            longest = n;
        }
    }
    Console.write("longest below 1000: ");
    Console.write(longest);
    Console.write(", ");
    Console.write(steps(longest));
    Console.writeLine(" steps");
}
```

```text
longest below 1000: 871, 178 steps
```

`3 * value + 1` is checked: if it overflowed a `u64`, the program would stop with a message naming the line, in every
build. CeeSafe never wraps silently.

## When the compiler says no

Errors name a code, the place, and usually the fix. This program keeps a borrow of a vector's element and then
grows the vector, which could move the element:

<!-- ceesafe: fail N0241 -->
```ceesafe
fn main() {
    var numbers = Vector<i32>.create();
    numbers.push(1)!;
    let first = borrow numbers[0];
    numbers.push(2)!;
    Console.writeLine(first);
}
```

```text
main.cee:5:5: error[N0241]: cannot borrow 'numbers' as mutable while 'numbers[0]' is borrowed
  |
5 |     numbers.push(2)!;
  |     ^^^^^^^
  = note: 'numbers[0]' is borrowed here (4:5)
  = note: the borrow is used later here (6:5)
```

The fix is to finish with `first` before the push, or to copy the value (`let first = numbers[0];`). Every code is
explained in [Diagnostics](../spec/diagnostics.md).

## Files without a package

For a quick experiment, a package is not needed: give the files to the command.

```sh
ceesafec build tool.cee helpers.cee -o tool
ceesafec run --interp tool.cee
```

## Next

- [Basics](basics.md): values, types, functions and control flow.
- [Packages](packages.md): dependencies, the lock file, and sharing code.
- [The ceesafec command](ceesafec.md): every command and option.
