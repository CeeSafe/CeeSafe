# CeeSafe for AI agents

This page is for AI agents that write CeeSafe, and for the people who set them up. CeeSafe is new, so a model has seen
little of it: it will write C#, TypeScript or Rust that looks like CeeSafe and is not. The compiler is built to correct
that quickly. It says exactly what is wrong, fixes what it is sure of, and serves its own specification.

The guide and the specification are inside `ceesafec`: an agent reads them through `ceesafec explain` and the MCP
server's resources (`ceesafe://docs/…`, below), so it needs no network access to read the rules. On the web they are
also in machine-readable form: [llms.txt](https://ceesafe.github.io/CeeSafe/llms.txt) (an index) and
[llms-full.txt](https://ceesafe.github.io/CeeSafe/llms-full.txt) (the guide, the specification and the library
reference in one file), and every page is available as Markdown at its address with `.md` added. The library reference
is [lib/core](../spec/api/core.md) and [lib/std](../spec/api/std.md), and the compiler's own types and functions
(`Console`, `File`, `Vector`, `StringView`, integer methods, …) are on [built-in types and functions](builtins.md).

## In a new package

`ceesafec new` writes `AGENTS.md`, which most coding agents read: what CeeSafe is, the package's layout, the commands that
check the work, how to use the MCP server, and the common mistakes below. Name your agent to get its own file and its
MCP configuration too:

```sh
ceesafec new hello --agent claude      # also CLAUDE.md (importing AGENTS.md) and .mcp.json
```

| `--agent` | Writes |
|---|---|
| `agents` (the default) | `AGENTS.md` |
| `claude` | `CLAUDE.md`, `.mcp.json` |
| `gemini` | `GEMINI.md`, `.gemini/settings.json` |
| `copilot` | `.github/copilot-instructions.md`, `.vscode/mcp.json` |
| `cursor` | `.cursor/rules/ceesafe.mdc`, `.cursor/mcp.json` |

CeeSafe's part of `AGENTS.md` sits between `<!--CEESAFE START-->` and `<!--CEESAFE END-->`. After updating `ceesafec`, run
`ceesafec agents` in the package to refresh that part; whatever you wrote outside it stays as it is.

## Set up the MCP server

`ceesafec mcp` is a [Model Context Protocol](https://modelcontextprotocol.io) server. Add it to your agent once:

```sh
claude mcp add ceesafe -- ceesafec mcp          # Claude Code
```

or, for any MCP client:

```json
{ "mcpServers": { "ceesafe": { "command": "ceesafec", "args": ["mcp"] } } }
```

Its tools:

| Tool | What it does |
|---|---|
| `ceesafe_check` | type-checks code and returns its diagnostics as JSON, with fixes |
| `ceesafe_fix` | applies every fix the compiler is sure of, and returns the files and what remains |
| `ceesafe_format` | formats a file in the one canonical style |
| `ceesafe_run` | runs a program in a sandboxed interpreter: no files, sockets or processes, and a step and memory budget |
| `ceesafe_emit_c` | shows the generated C, and where allocations, bounds checks and overflow checks went |
| `ceesafe_explain` | what a diagnostic code means |
| `ceesafe_spec_search`, `ceesafe_spec_section` | search and read the specification, which is embedded in `ceesafec` |
| `ceesafe_symbol` | the type and declaration of what is at a position |
| `ceesafe_api` | a library or built-in name looked up (`TextWriter.decimal`, `Process.argument`): its signature and documentation, or the closest names |

The prompt `write-ceesafe` gives an agent this page and the loop below. The specification and this guide are resources
(`ceesafe://docs/…`). The [tooling reference](../spec/tooling.md) §6 has the details.

## The loop

1. Write the code.
2. Check it: `ceesafe_check`, or `ceesafec check --diagnostics-format=json`.
3. Apply the fixes: `ceesafe_fix`, or `ceesafec fix`. A fix is offered only where the compiler is sure of it: `mut` before
   an argument, `let` to `var`, `truncate<T>(…)` for a narrowing cast, a missing `;`.
4. Fix the rest yourself. Each diagnostic has a code; `ceesafe_explain` (or `ceesafec explain N0249`) says what it means, and
   the message often names the specification section to read. For a function or method you are not sure of, look it
   up with `ceesafe_api` before writing it, rather than guessing a name from another language.
5. Repeat until there are no errors. Then run it with `ceesafe_run`, or build it with `ceesafec build`.

Without MCP, the same loop works from a shell: `ceesafec check --diagnostics-format=json main.cee` prints one JSON
document with every diagnostic, its position and its fixes, and `ceesafec fix main.cee` applies them.

## Common mistakes

These are the mistakes models make most often in CeeSafe, each with the compiler's answer and what to write instead.

### There is no `~`

<!-- ceesafe: fail N0102 -->
```ceesafe
fn invert(x: u32) -> u32 => ~x;
```

```text
main.cee:1:29: error[N0102]: expected expression, found '~'
  |
1 | fn invert(x: u32) -> u32 => ~x;
  |                             ^
```

Complement with an exclusive or of all ones:

```ceesafe
fn invert(x: u32) -> u32 => x ^ 0xFFFFFFFF;
```

### There are no type aliases

<!-- ceesafe: fail N0101 -->
```ceesafe
type Count = u32;
```

```text
main.cee:1:1: error[N0101]: expected 'fn', 'const', 'struct' or 'enum', found 'type'
  |
1 | type Count = u32;
  | ^^^^
```

Write the type itself, or wrap it in a struct when it deserves a name of its own.

### There are no tuples

<!-- ceesafe: fail N0100 -->
```ceesafe
fn main() {
    let (a, b) = (1, 2);
}
```

```text
main.cee:2:9: error[N0100]: expected identifier, found '('
  |
2 |     let (a, b) = (1, 2);
  |         ^
```

Declare two variables, or return a struct with named fields.

### Arrays of structs are not compared with `==`

`==` compares arrays and slices of integers, `bool` and fieldless enums. Arrays of structs are a different matter:

<!-- ceesafe: fail N0207 -->
```ceesafe
struct P { x: i32; }

fn same(a: [P; 2], b: [P; 2]) -> bool => a == b;
```

```text
main.cee:3:42: error[N0207]: '==' compares slices and arrays of integers, bools and fieldless enums, not of 'P' (type-system.md §16.2)
  |
3 | fn same(a: [P; 2], b: [P; 2]) -> bool => a == b;
  |                                          ^^^^^^
```

Compare element by element:

```ceesafe
struct P { x: i32; }

fn same(a: [P; 2], b: [P; 2]) -> bool {
    for (i in 0..a.length) {
        if (a[i].x != b[i].x) { return false; }
    }
    return true;
}
```

### Indexes are `usize`, and a range's type comes from its bounds

A range of two literals that the body indexes with is a `usize` range, so this works as written:

```ceesafe
fn sum(values: Slice<u64>) -> u64 {
    var total: u64 = 0;
    for (i in 0..4) {
        total += values[i];
    }
    return total;
}
```

The variable then is a `usize` everywhere in the body. Using it as an `i32` as well is an error:

<!-- ceesafe: fail N0205 -->
```ceesafe
fn fill(values: SliceMut<i32>) {
    for (i in 0..4) {
        let v: i32 = i;
        values[i] = v;
    }
}
```

```text
main.cee:3:22: error[N0205]: expected 'i32', found 'usize'
  |
3 |         let v: i32 = i;
  |                      ^
```

A range's type otherwise comes from its bounds: `0..4` with no index use is a range of `i32`, and
`0..values.length` is a `usize` range. Convert explicitly where the two meet:

```ceesafe
fn fill(values: SliceMut<i32>) {
    for (i in 0..values.length) {
        values[i] = truncate<i32>(i);
    }
}
```

### A block has a value only as a branch

A branch of an `if` or `match` expression may be a block that ends with its value, written without `;`:

```ceesafe
fn sign(x: i32) -> i32 {
    let s = if (x < 0) {
        let magnitude = 0 - x;
        magnitude / magnitude * -1
    } else 1;
    return s;
}

fn main() {
    Console.writeLine(sign(-4));
}
```

```text
-1
```

A statement's block has no value: a `match` statement whose arms end in a value is an error.

<!-- ceesafe: fail N0100 -->
```ceesafe
fn sign(x: i32) {
    match (x < 0) {
        true => { -1 }
        false => { 1 }
    }
}
```

```text
main.cee:3:21: error[N0100]: expected ';', found '}': a block ends in a value only as a branch of an 'if' or 'match' expression (let x = match …)
  |
3 |         true => { -1 }
  |                     ^

main.cee:4:21: error[N0100]: expected ';', found '}': a block ends in a value only as a branch of an 'if' or 'match' expression (let x = match …)
  |
4 |         false => { 1 }
  |                     ^
```

Make it an expression, `let s = match (x < 0) { … };`, and return its value.

### A view of a temporary lasts until the end of its statement

A call's result can be borrowed, sliced or measured where it is made, `use(make().view())`, but it is destroyed at the
end of the statement, so a view of it cannot be kept:

<!-- ceesafe: fail N0243 -->
```ceesafe
fn make() -> String {
    var s = String.create();
    s.append("hello")!;
    return s;
}

fn main() {
    let v = make().view();
    Console.writeLine(v);
}
```

```text
main.cee:8:5: error[N0243]: a temporary value is dropped at the end of this statement while still borrowed; bind it to a local first
  |
8 |     let v = make().view();
  |     ^^^^^^^^^^^^^^^^^^^^^^
  = note: a temporary value is borrowed here (8:5)
  = note: the borrow is used later here (9:5)
```

Bind the value to a local, and keep the view of the local:

```ceesafe
fn make() -> String {
    var s = String.create();
    s.append("hello")!;
    return s;
}

fn main() {
    let text = make();
    let v = text.view();
    Console.writeLine(v);
}
```

```text
hello
```

### There is no string interpolation

`{x}` in a string is printed as written. When `x` is a name in scope, the compiler warns (N0400):

<!-- ceesafe: warns N0400 -->
```ceesafe
fn main() {
    let x = 3;
    Console.writeLine("x = {x}");
}
```

```text
main.cee:3:28: warning[N0400]: this string prints '{x}' as written (there is no string interpolation): write the value in a call of its own, or '\{' to mean the brace
  |
3 |     Console.writeLine("x = {x}");
  |                            ^^^
```

Write the value in a call of its own:

```ceesafe
fn main() {
    let x = 3;
    Console.write("x = ");
    Console.writeLine(x);
}
```

```text
x = 3
```

### A multi-line string is a raw `"""` literal

A `"..."` literal ends on its line. For several lines, open with `"""` at the end of a line and close with `"""` on a
line of its own. The closing line's indentation is removed from every line, there is no final newline, and there are
no escapes:

```ceesafe
fn main() {
    let query = """
        SELECT id, name
          FROM users
        """;
    Console.writeLine(query);
}
```

```text
SELECT id, name
  FROM users
```

### `for` loops over iterators too

`for` takes arrays, slices, ranges, and any value with `fn next(borrow mut this) -> T?`, such as a `split`:

```ceesafe
fn main() {
    let text = "a,b,c";
    for (part in text.split(",")) {
        Console.writeLine(part);
    }
}
```

```text
a
b
c
```

lib/core's `Iterator<T>` names that shape as an interface, so generic code and `dyn` can iterate too:
`fn total<I: Iterator<u32>>(it: I) -> u32 { … for (x in it) { … } … }`, and `for (x in boxed)` over a
`Box<dyn Iterator<u32>>` in a `var`. Rust's iterator chains (`map`, `filter`, `collect`, …) become the loop's body:
there is no adapter library.

### A function that always exits returns `never`

A call of a `never` function ends its branch, so a `match` that gives a value needs no value from that branch:

```ceesafe
fn die(message: StringView) -> never {
    Console.writeLine(message);
    Process.exit(1);
}

fn main() {
    let n: Result<u32, u32> = Ok(7);
    let value = match (n) {
        Ok(v) => v,
        Err(_) => {
            die("no number");
        }
    };
    Console.writeLine(value);
}
```

```text
7
```

### Converting with a check: `checkedTruncate` and `saturatingTruncate`

`as` only allows conversions that never lose information, so `usize` to `i64` is not one. `checkedTruncate<T>(x)` is
`null` when the value does not fit, and `saturatingTruncate<T>(x)` clamps it:

```ceesafe
fn main() {
    let n: usize = 300;
    let wide = checkedTruncate<i64>(n) ?? -1;
    let small = checkedTruncate<u8>(n) ?? 0;
    Console.writeLine(wide);
    Console.writeLine(small);
    Console.writeLine(saturatingTruncate<u8>(n));
}
```

```text
300
0
255
```

### A function in a field is not called through the field

<!-- ceesafe: fail N0210 -->
```ceesafe
struct Counter {
    step: fn(i32) -> i32;
}

fn advance(c: borrow Counter, n: i32) -> i32 => (c.step)(n);
```

```text
main.cee:5:49: error[N0210]: this expression cannot be called
  |
5 | fn advance(c: borrow Counter, n: i32) -> i32 => (c.step)(n);
  |                                                 ^^^^^^^^
```

Pass the function or closure as a parameter, and call the parameter:

```ceesafe
fn advance(step: fn(i32) -> i32, n: i32) -> i32 => step(n);
```

### A borrowed string in an enum needs a region

<!-- ceesafe: fail N0270 -->
```ceesafe
enum Failure {
    Refused(StringView),
    Io,
}
```

```text
main.cee:2:13: error[N0270]: a payload of scoped type 'StringView' needs a region parameter on the enum: 'enum Failure<region r>' (type-system.md §25.4)
  |
2 |     Refused(StringView),
  |             ^^^^^^^^^^
```

A `StringView` borrows its text, so a type holding one says from where. For fixed messages, an enum of reasons and a
function that words them is simpler:

```ceesafe
enum Reason {
    TooLarge,
    BadMagic,
}

enum Failure {
    Refused(Reason),
    Io,
}

fn describe(reason: Reason) -> StringView => match (reason) {
    Reason.TooLarge => "too large",
    Reason.BadMagic => "bad magic number",
};
```

### A method cannot share a field's name

<!-- ceesafe: fail N0228 -->
```ceesafe
struct Account {
    balance: i64;

    fn balance(borrow this) -> i64 => this.balance;
}
```

```text
main.cee:4:8: error[N0228]: 'Account' already has a member named 'balance'
  |
4 |     fn balance(borrow this) -> i64 => this.balance;
  |        ^^^^^^^
```

Name the method for what it does (`current`), or make the field `public`.

### Look a name up before deciding it is missing

A model guesses names from the languages it knows (`unsigned`, `args`, `writeLn`). CeeSafe's names are often
different, and the compiler says which ones exist:

<!-- ceesafe: fail N0200 -->
```ceesafe
fn main() {
    var buffer = [0u8; 32];
    var w = TextWriter.create(mut buffer[..]);
    w.unsigned(42);
}
```

```text
main.cee:4:7: error[N0200]: 'TextWriter' has no method named 'unsigned'; did you mean 'signed', 'decimal' or 'hex'?
  |
4 |     w.unsigned(42);
  |       ^^^^^^^^
```

`decimal` is suggested because its documentation says "Appends an unsigned integer in decimal". Before writing a call
you are unsure of, look the name up: `ceesafe_api` with `TextWriter`, `TextWriter.decimal`, `decimal` or
`Process.argument` returns the entries, or the closest names. Without MCP, the [built-in types](builtins.md) and the
[lib/core](../spec/api/core.md) and [lib/std](../spec/api/std.md) pages list every name.

### Reserved words are not names

Some keywords read like ordinary names: `own`, `type`, `move`, `drop`, `where`, `class`, `yield`, `macro`, `using`,
`static` and `in` are among them ([grammar](../spec/grammar.md) §1.3 lists them all).

<!-- ceesafe: fail N0100 -->
```ceesafe
fn main() {
    let own = 1;
}
```

```text
main.cee:2:9: error[N0100]: expected identifier, found 'own', which is a reserved word: choose another name (grammar.md §1.3)
  |
2 |     let own = 1;
  |         ^^^
```

### A lookup that may find nothing is an optional, not a `Result`

`Process.argument(i)`, `StringView.fromUtf8(bytes)`, `Environment.variable(name)`, `v.pop()` and `x.checkedAdd(y)`
give a `T?`: match it with `Some(…)` and `null`, not `Ok` and `Err`. A `!` unwraps an optional as well as a result.

```ceesafe
fn main() {
    match (Process.argument(1)) {
        Some(path) => { Console.writeLine(path); }
        null => { Console.writeLine("no file given"); }
    }
    let bytes = [104u8, 105];
    let text = StringView.fromUtf8(bytes[..])!; // `!` unwraps an optional too, and panics on null
    Console.writeLine(text);
}
```

```text
no file given
hi
```

### `Console` blocks its thread, so async code uses `AsyncConsole`

<!-- ceesafe: fail N0295 -->
```ceesafe
async fn greet() {
    Console.writeLine("hello");
}

fn main() {
    Async.run(greet());
}
```

```text
main.cee:2:5: error[N0295]: 'greet' is an async fn, which must not block its thread (type-system.md §40.1), but this may sleep: it does console I/O
  |
2 |     Console.writeLine("hello");
  |     ^^^^^^^^^^^^^^^^^^^^^^^^^^^
```

The same holds for `File` and `TcpStream`: async code uses `AsyncFiles`, `AsyncTcpListener` and `AsyncTcpStream`
from lib/std. A call through `dyn` counts as blocking unless the interface method is declared `@effects()`.

```ceesafe
async fn greet() -> Result<void, AsyncError> {
    await AsyncConsole.writeLine("hello")?;
    return Ok();
}

fn main() {
    match (Async.run(greet())) {
        Ok() => {}
        Err(_) => { Console.writeLine("failed"); }
    }
}
```

```text
hello
```

### `mut` at a call, `borrow mut` anywhere else

A mutable borrow passed as an argument is written `mut x` (or `mut a[lo..hi]`). The same borrow in a `let` or any
other expression is `borrow mut x`.

```ceesafe
fn bump(n: borrow mut i32) {
    n += 1;
}

fn zero(bytes: SliceMut<u8>) {
    bytes.fill(0);
}

fn main() {
    var n = 1;
    bump(mut n); // an argument: `mut`
    let r = borrow mut n; // anywhere else: `borrow mut`
    r += 1;
    var buffer = [7u8; 4];
    zero(mut buffer[0..2]);
    let tail = borrow mut buffer[2..];
    tail[0] = 9;
    Console.writeLine(n);
    Console.writeLine(buffer[2]);
}
```

```text
3
9
```

### What the compiler fixes for you

`ceesafe_fix` and `ceesafec fix` repair these without being asked twice: a `let` that is assigned (`var`), an argument for a
`borrow mut` parameter without `mut`, a `mut` where the parameter is a shared `borrow`, `as` between integer types that
can lose information (`truncate<T>(…)`, which wraps), and a `;` missing after a statement.

## Where to read more

The [guide](introduction.md) teaches the language; [Coming from another language](coming-from/index.md) maps what you know.
[Built-in types and functions](builtins.md), [lib/core](../spec/api/core.md) and [lib/std](../spec/api/std.md) list every
name.
The rules themselves are the [specification](../spec/type-system.md), with the [grammar](../spec/grammar.md) and every
[diagnostic](../spec/diagnostics.md).
