# Basics

This chapter covers what every program uses: values and types, variables, functions, operators, and control flow.
Each example is a whole program you can run with `ceesafec run --interp file.cee`.

## A program

A program is one or more `.cee` files with a `main` function. `main` takes nothing and returns nothing or an `i32`
exit status.

```ceesafe
fn main() -> i32 {
    Console.writeLine("Hello, CeeSafe");
    return 0;
}
```

```text
Hello, CeeSafe
```

`Console.write` and `Console.writeLine` print integers, booleans, floats and text.

## Types

| Type | Values |
|---|---|
| `i8` `i16` `i32` `i64` | signed integers, two's complement |
| `u8` `u16` `u32` `u64` | unsigned integers |
| `isize` `usize` | the target's pointer width; lengths and indexes are `usize` |
| `f32` `f64` | IEEE 754 floating point |
| `bool` | `true`, `false` |
| `StringView` | a read-only view of UTF-8 text; string literals have this type |

Later chapters add structs, enums, arrays, references, optionals (`T?`), results (`Result<T, E>`), boxes and
collections. There is no `char`: text is UTF-8 bytes, and a character literal such as `'k'` is an integer.

## Variables

`let` declares a variable that cannot be assigned again; `var` one that can. The type is inferred from the
initialiser, or written after a colon.

```ceesafe
fn main() {
    let name = "CeeSafe";              // StringView
    let year: u16 = 2026;
    var count = 0;                  // i32: an integer literal's default type
    count += 1;
    var total: u64;                 // declared now, assigned before it is read
    total = 40 + 2;
    Console.writeLine(name);
    Console.writeLine(year);
    Console.writeLine(count);
    Console.writeLine(total);
}
```

```text
CeeSafe
2026
1
42
```

Reading a variable that might not be assigned yet is a compile error, and so is assigning to a `let`. Two locals in
scope cannot have the same name: there is no shadowing. `let _ = f();` evaluates `f()` and discards its value.

### Reserved words and built-in names

Some words that look like ordinary names are reserved, so they can't name a variable, parameter or field: `move`,
`own`, `type`, `namespace`, `macro`, `yield`, `class` and `internal`, among others. The full list is in
[grammar.md §1.3](../spec/grammar.md). The compiler says so when one is used as a name.

The built-in types' names are taken in every program, so a program can't declare its own `Handle` (a `SlotMap`'s
handle), `Check` (the test helpers), `TestFailure`, or the lib/core names listed in [the library API](builtins.md).

## Integer literals and the expected type

An integer literal takes its type from where it is used. In `let year: u16 = 2026;` it is a `u16`; in `x + 1` it is
`x`'s type; with nothing to go by it is an `i32`. A suffix fixes it: `7u8`, `100_000u64`. Literals can be written in
hex, binary and octal (`0xFF`, `0b1010`, `0o17`), with `_` between digits. A value that does not fit its type is a
compile error.

A character literal is the character's Unicode value, typed like any other literal:

```ceesafe
fn isDigit(c: u8) -> bool => c >= '0' && c <= '9';

fn main() {
    Console.writeLine(isDigit('7'));
    Console.writeLine(isDigit('x'));
    Console.writeLine('\n' as u32);
}
```

```text
true
false
10
```

## Functions

```ceesafe
fn add(a: i64, b: i64) -> i64 {
    return a + b;
}

fn square(x: i64) -> i64 => x * x;        // an expression body

fn greet(name: StringView) {              // no return type: returns nothing
    Console.write("Hello, ");
    Console.writeLine(name);
}

fn main() {
    Console.writeLine(add(2, square(3)));
    greet("reader");
}
```

```text
11
Hello, reader
```

Parameters are immutable. Arguments are evaluated left to right. A function that returns a value must return on
every path, which the compiler checks.

## Operators

Both operands of an operator must have the same type: there are **no implicit conversions**, not even between `i32`
and `i64`.

| Operators | On | Notes |
|---|---|---|
| `+ - * / %` | integers, floats | integer overflow stops the program; so does dividing by zero |
| `& \| ^ << >>` | unsigned integers | a shift by the width or more stops the program |
| `== != < <= > >=` | integers, floats, `bool` (`==`, `!=`), text (`==`, `!=`) | |
| `== !=` | enums without payloads; arrays and slices of integers, `bool` or such enums | compare the elements |
| `&& \|\| !` | `bool` | `&&` and `\|\|` short-circuit |
| `??` | an optional and a fallback | `o ?? d`: `o`'s value, or `d` when `o` is `null` |

When overflow is expected, say what should happen:

```ceesafe
fn main() {
    let big: u8 = 250;
    Console.writeLine(big.wrappingAdd(10));       // 4: modulo 256
    Console.writeLine(big.saturatingAdd(10));     // 255: clamped
    match (big.checkedAdd(10)) {                  // u8?: null on overflow
        Some(sum) => { Console.writeLine(sum); }
        null => { Console.writeLine("overflow"); }
    }
}
```

```text
4
255
overflow
```

The same three families exist for `Sub` and `Mul`, on every integer type.

## Conversions

`as` converts only when no value can be lost: `u8 as u32`, `i32 as i64`, `u32 as i64`, `bool as u8`, `i32 as f64`.
Anything that could lose information is spelled out:

```ceesafe
fn main() {
    let big: u32 = 300;
    Console.writeLine(truncate<u8>(big));             // 44: the low 8 bits
    let x: f64 = 2.75;
    Console.writeLine(truncate<i32>(x));              // 2: toward zero; out of range stops the program
    Console.writeLine(round<f64>(1234567890123u64));  // the nearest f64
    let n: i32 = -1;
    Console.writeLine(n as i64);
}
```

```text
44
2
1234567890123
-1
```

## Control flow

`if`, `while`, `for` and `match` put their condition in parentheses, and their bodies are blocks.

```ceesafe
fn main() {
    var n = 0;
    while (n < 3) {
        n += 1;
    }
    for (i in 0..3) {                    // 0, 1, 2
        if (i == 1) {
            continue;
        }
        Console.write(i);
    }
    Console.writeLine();
    var tries = 0;
    loop {                               // until a break
        tries += 1;
        if (tries == 5) {
            break;
        }
    }
    Console.writeLine(n + tries);
}
```

```text
02
8
```

`for (x in values)` walks an array, a slice or a vector (see [Collections](collections.md)).

### `if` and `match` are expressions too

```ceesafe
fn describe(n: i32) -> StringView => match (n) {
    0 => "zero",
    1 => "one",
    _ => if (n < 0) "negative" else "many",
};

fn main() {
    Console.writeLine(describe(0));
    Console.writeLine(describe(7));
    Console.writeLine(describe(-2));
    let sign = if (-2 < 0) -1 else 1;
    Console.writeLine(sign);
}
```

```text
zero
many
negative
-1
```

An `if` expression needs its `else`. A branch can also be a block. It either ends with its value, written without `;`,
or it leaves, such as `{ return; }`, which is how an early exit fits inside an expression:

```ceesafe
fn parse(text: StringView) -> i32 {
    let n = match (text.parseInt<i32>()) {
        Some(v) => {
            let doubled = v * 2;
            doubled
        }
        null => {
            return -1;
        }
    };
    return n;
}

fn main() {
    Console.writeLine(parse("21"));
    Console.writeLine(parse("x"));
}
```

```text
42
-1
```

## Constants

```ceesafe
const SECONDS_PER_DAY: u32 = 24 * 60 * 60;

fn main() {
    Console.writeLine(SECONDS_PER_DAY);
}
```

```text
86400
```

A constant is evaluated at compile time. An overflow there is a compile error rather than a run-time stop.

## Comments

`//` comments run to the end of the line, `/* … */` comments may span lines, and `///` before an item is its
documentation, which [`ceesafec doc`](formatting-and-docs.md) collects.

## Next

[Structs, enums and patterns](structs-and-enums.md).
