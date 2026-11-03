# CeeSafe for AI agents

This page is for AI agents that write CeeSafe, and for the people who set them up. CeeSafe is new, so a model has seen
little of it: it will write C#, TypeScript or Rust that looks like CeeSafe and is not. The compiler is built to correct
that quickly. It says exactly what is wrong, fixes what it is sure of, and serves its own specification.

The guide and the specification are inside `ceesafec`: an agent reads them through `ceesafec explain` and the MCP
server's resources (`ceesafe://docs/…`, below), so it needs no network access to read the rules. On the web they are
also in machine-readable form: [llms.txt](https://ceesafe.github.io/CeeSafe/llms.txt) (an index) and
[llms-full.txt](https://ceesafe.github.io/CeeSafe/llms-full.txt) (the guide and the specification in one file), and
every page is available as Markdown at its address with `.md` added.

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

The prompt `write-ceesafe` gives an agent this page and the loop below. The specification and this guide are resources
(`ceesafe://docs/…`). The [tooling reference](../spec/tooling.md) §6 has the details.

## The loop

1. Write the code.
2. Check it: `ceesafe_check`, or `ceesafec check --diagnostics-format=json`.
3. Apply the fixes: `ceesafe_fix`, or `ceesafec fix`. A fix is offered only where the compiler is sure of it: `mut` before
   an argument, `let` to `var`, `truncate<T>(…)` for a narrowing cast, a missing `;`.
4. Fix the rest yourself. Each diagnostic has a code; `ceesafe_explain` (or `ceesafec explain N0249`) says what it means, and
   the message often names the specification section to read.
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

### Arrays are not compared with `==`

<!-- ceesafe: fail N0207 -->
```ceesafe
fn same(a: [u8; 4], b: [u8; 4]) -> bool => a == b;
```

```text
main.cee:1:44: error[N0207]: operator '==' cannot be applied to type '[u8; 4]'
  |
1 | fn same(a: [u8; 4], b: [u8; 4]) -> bool => a == b;
  |                                            ^^^^^^
```

Compare element by element:

```ceesafe
fn same(a: [u8; 4], b: [u8; 4]) -> bool {
    for (i in 0..a.length) {
        if (a[i] != b[i]) { return false; }
    }
    return true;
}
```

### Indexes are `usize`, and a range's type comes from its bounds

<!-- ceesafe: fail N0205 -->
```ceesafe
fn sum(values: Slice<u64>) -> u64 {
    var total: u64 = 0;
    for (i in 0..4) {
        total += values[i];
    }
    return total;
}
```

```text
main.cee:4:25: error[N0205]: expected 'usize', found 'i32'
  |
4 |         total += values[i];
  |                         ^
```

`0..4` is a range of `i32`. Use the length, which is a `usize`, or a typed literal:

```ceesafe
fn sum(values: Slice<u64>) -> u64 {
    var total: u64 = 0;
    for (i in 0..values.length) {
        total += values[i];
    }
    return total;
}
```

### A block is not a value

<!-- ceesafe: fail N0100 -->
```ceesafe
fn sign(x: i32) -> i32 {
    let s = if (x < 0) { -1 } else { 1 };
    return s;
}
```

```text
main.cee:2:28: error[N0100]: expected ';', found '}'
  |
2 |     let s = if (x < 0) { -1 } else { 1 };
  |                            ^

main.cee:2:39: error[N0100]: expected ';', found '}'
  |
2 |     let s = if (x < 0) { -1 } else { 1 };
  |                                       ^
```

An `if` or `match` expression's branches are expressions, written without braces:

```ceesafe
fn sign(x: i32) -> i32 {
    let s = if (x < 0) -1 else 1;
    return s;
}
```

### A temporary is not sliced or measured

<!-- ceesafe: fail N0216 -->
```ceesafe
const TABLE: [u8; 4] = [1, 2, 3, 4];

fn main() {
    Console.writeLine(TABLE.length);
}
```

```text
main.cee:4:23: error[N0216]: 'length' needs an array variable or field; bind the value to a local first
  |
4 |     Console.writeLine(TABLE.length);
  |                       ^^^^^^^^^^^^
```

Bind it to a local first, as the message says:

```ceesafe
const TABLE: [u8; 4] = [1, 2, 3, 4];

fn main() {
    let table = TABLE;
    Console.writeLine(table.length);
}
```

```text
4
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

### What the compiler fixes for you

`ceesafe_fix` and `ceesafec fix` repair these without being asked twice: a `let` that is assigned (`var`), an argument for a
`borrow mut` parameter without `mut`, a `mut` where the parameter is a shared `borrow`, `as` between integer types that
can lose information (`truncate<T>(…)`, which wraps), and a `;` missing after a statement.

## Where to read more

The [guide](introduction.md) teaches the language; [Coming from another language](coming-from/index.md) maps what you know.
The rules themselves are the [specification](../spec/type-system.md), with the [grammar](../spec/grammar.md) and every
[diagnostic](../spec/diagnostics.md).
