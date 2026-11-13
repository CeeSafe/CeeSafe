---
title: The syntax, whole
summary: Every form of pattern with a listing - wildcard, names, literals and constants, ranges, variants and nesting, alternatives, name @ pattern, text, slices and structs - and how a name takes its value by copy, by move or by borrow.
assumes: [ch19-01-where-patterns-go, ch19-02-patterns-that-can-fail, ch04-02-borrowing]
keywords: [pattern, literal, range, variant, nested, "or-pattern", "@", var, slice pattern, "match (borrow x)", "match (this)", "struct pattern", N0203, N0229, N0250, N0356]
diagnostics: [N0203, N0229, N0250, N0356]
specification: ["type-system.md §15.2", "type-system.md §18.4", "grammar.md §2"]
---

# The syntax, whole

**This section teaches** every form a pattern can take, and how a name in a pattern gets its value. It is a
reference with a listing for each form. It assumes [Where patterns go](./ch19-01-where-patterns-go),
[Patterns that can fail](./ch19-02-patterns-that-can-fail), and moves and borrows from
[Borrowing](./ch04-02-borrowing).

## The forms

| Form | Examples | Fits |
|---|---|---|
| wildcard | `_` | any value, and binds nothing |
| name | `n`, `var n` | any value, and binds it |
| literal | `0`, `-1`, `0x20`, `'a'`, `true`, `null` | that value |
| constant | `LIMIT`, `Ascii.TAB` | the value of that `const` |
| range | `'0'..='9'`, `0..32` | the integers from the first bound to the second |
| variant | `Shape.Rect(w, h)`, `Shape.Rect`, `Some(p)`, `Ok(p)`, `Err(p)` | that variant, when each inner pattern fits |
| text | `"get"` | a `StringView` with that text |
| slice | `[]`, `[a, b]`, `[first, ..]`, `[x, rest @ ..]` | a slice or an array of that shape |
| struct | `Point { x, y: 0 }`, `Point { x: 0, .. }` | a value of that struct, when each field's pattern fits |
| alternatives | `p \| q` | what `p` fits or `q` fits |
| name and pattern | `d @ '0'..='9'` | what the pattern fits, and binds the whole value |
| parentheses | `(p \| q)` | what the pattern inside fits |

A guard, `if (cond)`, is not a pattern: it belongs to a `match` arm. A pattern has no types in it, and no
expressions: every value in a pattern is a literal or a named constant.

## Literals and constants

<span class="listing-caption"><strong>Listing 19-17</strong>, <code>main.cee</code>: Integer and character literals, a constant, and a constant of a struct.</span>

```ceesafe
const LIMIT: i32 = 100;

struct Ascii {
    const TAB: u8 = 9;
}

fn number(n: i32) -> StringView => match (n) {
    0 => "zero",
    -1 => "minus one",
    LIMIT => "the limit",
    _ => "another number",
};

fn byte(b: u8) -> StringView => match (b) {
    'a' => "the letter a",
    Ascii.TAB => "a tab",
    0x20 => "a space",
    _ => "another byte",
};

fn main() {
    Console.writeLine(number(-1));
    Console.writeLine(number(100));
    Console.writeLine(byte(9));
    Console.writeLine(byte(' '));
}
```

```text
minus one
the limit
a tab
a space
```

A literal takes the type of the value it is held against, so `'a'` is a `u8` here, and a literal that does not fit
the type is an error. A bare name is a constant when a `const` of that name is in scope, and a new name otherwise.
The constant is an integer or a `bool`. There are no float patterns, and a float cannot be matched at all.

## Ranges

<span class="listing-caption"><strong>Listing 19-18</strong>, <code>main.cee</code>: <code>..=</code> includes its end and <code>..</code> leaves it out.</span>

```ceesafe
fn kind(b: u8) -> StringView => match (b) {
    '0'..='9' => "digit",
    'a'..='z' | 'A'..='Z' => "letter",
    0..32 => "control",
    _ => "other",
};

fn main() {
    Console.writeLine(kind('7'));
    Console.writeLine(kind('Q'));
    Console.writeLine(kind(10));
    Console.writeLine(kind('+'));
}
```

```text
digit
letter
control
other
```

Ranges are for integers. Both bounds are written: a range has no open end. A bound is an integer or character
literal, or the bare name of a constant of the matched type. With `const LOW: u8 = 10;` in scope, `0..LOW` and
`LOW..=99` are ranges on a `u8`. A local variable or an expression such as `LOW + 1` is not a bound: test it in a
guard.

## Variants, nesting and `_`

A variant pattern names the variant and gives a pattern for each piece of its data. Those are patterns of any
form, so patterns nest. `Some`, `null`, `Ok` and `Err` are the variant patterns of optionals and `Result`.

<span class="listing-caption"><strong>Listing 19-19</strong>, <code>main.cee</code>: Patterns three levels deep. Each arm reads as the value it takes.</span>

```ceesafe
enum Shape {
    Circle(i64),
    Rect(i64, i64),
}

enum Failure {
    Empty,
    Code(i32),
}

fn describe(r: Result<Shape?, Failure>) -> StringView => match (r) {
    Ok(Some(Shape.Circle(0))) => "a point",
    Ok(Some(Shape.Circle(_))) => "a circle",
    Ok(Some(Shape.Rect(w, h))) if (w == h) => "a square",
    Ok(Some(_)) => "a rectangle",
    Ok(null) => "no shape",
    Err(Failure.Code(404)) => "not found",
    Err(_) => "a failure",
};

fn main() {
    Console.writeLine(describe(Ok(Some(Shape.Circle(0)))));
    Console.writeLine(describe(Ok(Some(Shape.Rect(2, 2)))));
    Console.writeLine(describe(Ok(Some(Shape.Rect(2, 3)))));
    Console.writeLine(describe(Ok(null)));
    Console.writeLine(describe(Err(Failure.Code(404))));
    Console.writeLine(describe(Err(Failure.Empty)));
}
```

```text
a point
a square
a rectangle
no shape
not found
a failure
```

A variant pattern lists all of the variant's data (`Shape.Rect(w)` is an error) or none of it: `Shape.Rect`, with
no parentheses, fits any rectangle, and so do `Some`, `Ok` and `Err` alone. `_` stands for one position.

## Names, `var` and `@`

A name fits anything and binds it. The binding cannot be assigned unless it is written `var`. `name @ pattern`
binds the whole value when the pattern after `@` fits it.

<span class="listing-caption"><strong>Listing 19-20</strong>, <code>main.cee</code>: <code>@</code> names a value that is also tested, and <code>var</code> makes a binding that can change.</span>

```ceesafe
fn digit(b: u8) -> u8 => match (b) {
    d @ '0'..='9' => d - '0',
    x @ ('a' | 'b') => x - 'a' + 10,
    _ => 255,
};

fn main() {
    Console.writeLine(digit('7'));
    Console.writeLine(digit('b'));

    match (digit('7')) {
        var n => {
            n += 1;
            Console.writeLine(n);
        }
    }

    let port: i32? = 8080;
    if (port is Some(p @ 1024..=65535)) {
        Console.writeLine(p);
    }
}
```

```text
7
11
8
8080
```

`@` binds tighter than `|`, so alternatives after it need parentheses: `x @ ('a' | 'b')`.

## Alternatives

`p | q` fits what either fits. The alternatives are tried from the left. They may bind names, and then each must
bind the same names with the same types, because the arm has one body.

<span class="listing-caption"><strong>Listing 19-21</strong>, <code>main.cee</code>: Both alternatives bind <code>k</code>, so the arm can use it.</span>

```ceesafe
enum Event {
    KeyDown(u8),
    KeyUp(u8),
    Tick,
}

fn key(e: Event) -> u8 => match (e) {
    Event.KeyDown(k) | Event.KeyUp(k) => k,
    Event.Tick => 0,
};

fn main() {
    Console.writeLine(key(Event.KeyUp(65)));
}
```

```text
65
```

Alternatives may also stand inside another pattern: `Token.Symbol('+' | '-')`, `Some("yes" | "on")`.

## Text and slices

A text literal is a pattern for a `StringView`: it fits a view with the same bytes. A slice pattern, in square
brackets, has a pattern for each element, and at most one `..` that stands for any number of elements.
`rest @ ..` binds those elements as a slice.

<span class="listing-caption"><strong>Listing 19-22</strong>, <code>main.cee</code>: Text patterns at the top and inside an optional, and a slice pattern on the bytes of a text.</span>

```ceesafe
fn verb(word: StringView) -> StringView => match (word) {
    "get" | "head" => "read",
    "put" | "post" => "write",
    other => other,
};

fn flag(argument: StringView?) -> bool => match (argument) {
    Some("yes" | "on") => true,
    Some(_) => false,
    null => false,
};

fn main() {
    Console.writeLine(verb("head"));
    Console.writeLine(verb("trace"));
    Console.writeLine(flag(Some("on")));
    if ("\"quoted\"".bytes() is ['"', inner @ .., '"']) {
        Console.writeLine(inner.length);
    }
}
```

```text
read
trace
true
6
```

A slice pattern needs a slice or an array, and a `StringView` is neither: `.bytes()` gives its bytes as a
`Slice<u8>`.

<span class="listing-caption"><strong>Listing 19-23</strong>, <code>main.cee</code>: Slice patterns by length, by both ends, and with the rest bound.</span>

```ceesafe
fn describe(items: Slice<i32>) {
    match (items) {
        [] => { Console.writeLine("empty"); }
        [only] => {
            Console.write("one: ");
            Console.writeLine(only);
        }
        [first, .., last] if (first == last) => { Console.writeLine("same ends"); }
        [first, rest @ ..] => {
            Console.write(first);
            Console.write(" and ");
            Console.write(rest.length);
            Console.writeLine(" more");
        }
    }
}

fn main() {
    let values = [4, 8, 15, 4];
    describe(values[0..0]);
    describe(values[0..1]);
    describe(values[..]);
    describe(values[0..3]);
}
```

```text
empty
one: 4
same ends
4 and 2 more
```

Without `..`, a slice pattern fits exactly as many elements as it lists. With it, at least that many. On an array
the count must be the array's length.

## How a name takes its value

A name in a pattern becomes a local variable. What it holds depends on what was matched, not on how the name is
written.

::: tip Rule
- Matching a **value** whose bound part is Copy (a number, a `bool`, a view) copies the part. The value is
  untouched.
- Matching a **value** and binding a part that is not Copy (a `String`, a `Vector`) moves the part out. The
  `match` **consumes** the value: it cannot be used afterwards.
- Matching a **borrow** makes each name a borrow of its part: `borrow T` from a `borrow`, `borrow mut T` from a
  `borrow mut`. Nothing is copied or moved.

*Specification: `type-system.md` §15.2, §18.4.*
:::

To match a value you own without consuming it, borrow it in the parentheses: `match (borrow m)`.

<span class="listing-caption"><strong>Listing 19-24</strong>, <code>main.cee</code>: The same value matched three times: read through a borrow, changed through an exclusive borrow, then consumed.</span>

```ceesafe
enum Message {
    Text(String),
    Count(i32),
}

fn main() {
    var m = Message.Text(String.from("hello")!);

    match (borrow m) {
        Message.Text(s) => { Console.writeLine(s.length); }
        Message.Count(n) => { Console.writeLine(n); }
    }

    match (borrow mut m) {
        Message.Text(s) => { s.append("!")!; }
        Message.Count(n) => { n += 1; }
    }

    match (m) {
        Message.Text(s) => { Console.writeLine(s.view()); }
        Message.Count(n) => { Console.writeLine(n); }
    }
}
```

```text
5
hello!
```

- In the first `match`, `s` is a `borrow String`. In the second it is a `borrow mut String`, and `append` changes
  the text inside `m`.
- In the third, `s` is the `String` itself, moved out of `m`. After it, `m` is gone.

A parameter of type `borrow Message` is already a borrow, and so is `this` in a method that takes `borrow this` or
`borrow mut this`. Matching them needs no `borrow` in the parentheses.

<span class="listing-caption"><strong>Listing 19-25</strong>, <code>main.cee</code>: <code>match (this)</code> in two methods. In <code>tick</code>, <code>n</code> is an exclusive borrow of the count inside the enum.</span>

```ceesafe
enum Counter {
    Idle,
    Running(u32),
}

impl Counter {
    fn value(borrow this) -> u32 => match (this) {
        Counter.Idle => 0,
        Counter.Running(n) => n,
    };

    fn tick(borrow mut this) {
        match (this) {
            Counter.Idle => { this = Counter.Running(1); }
            Counter.Running(n) => { n += 1; }
        }
    }
}

fn main() {
    var c = Counter.Idle;
    c.tick();
    c.tick();
    c.tick();
    Console.writeLine(c.value());
}
```

```text
3
```

Three more facts about moves:

- Only names move. `_`, and a variant without its parentheses, leave the part where it is, so a `match` whose
  patterns bind nothing that is not Copy does not consume.
- `is` moves only when the value fits. In `if (m is Message.Text(s)) { … } else { … }`, `m` is whole in the `else`
  block, and gone after the `if`.
- A guard may read the arm's names and may not move them: the move happens once the guard is true.

A name bound through a borrow cannot be `var`. And `whole @ Err(inner)` cannot move the same `String` into both
names: bind one of them.

## Structs, and two values at once

A struct pattern is written like a struct literal: the struct's name, then its fields in braces. A field alone,
`x`, binds the field to the name `x`, and `var x` binds a name that can be assigned. `y: 0` tests the field with a
pattern, which may be any pattern. Every field is named, or the pattern ends with `..`, which stands for the
others. A struct pattern goes wherever a pattern goes, and inside other patterns.

There are no tuples, so there is no pattern for two values at once. To match two values together, put them in a
small struct.

<span class="listing-caption"><strong>Listing 19-26</strong>, <code>main.cee</code>: A struct pattern inside a variant, two enums matched together through a struct, and a plain <code>let</code> that takes a struct apart.</span>

```ceesafe
struct Point {
    x: i32;
    y: i32;
}

enum Event {
    Click(Point),
    Quit,
}

enum Light {
    Red,
    Green,
}

struct Crossing {
    north: Light;
    east: Light;
}

fn describe(e: Event) -> StringView => match (e) {
    Event.Click(Point { x: 0, y: 0 }) => "a click at the origin",
    Event.Click(Point { x: 0, .. }) => "a click on the left edge",
    Event.Click(_) => "a click",
    Event.Quit => "quit",
};

fn safe(north: Light, east: Light) -> bool => match (Crossing { north: north, east: east }) {
    Crossing { north: Light.Green, east: Light.Green } => false,
    Crossing { .. } => true,
};

fn main() {
    Console.writeLine(describe(Event.Click(Point { x: 0, y: 0 })));
    Console.writeLine(describe(Event.Click(Point { x: 0, y: 7 })));
    Console.writeLine(describe(Event.Click(Point { x: 3, y: 0 })));
    Console.writeLine(safe(Light.Green, Light.Red));
    Console.writeLine(safe(Light.Green, Light.Green));
    let Point { x, var y } = Point { x: 2, y: 3 };
    y += x;
    Console.writeLine(y);
}
```

```text
a click at the origin
a click on the left edge
a click
true
false
5
```

A `match` on a struct is checked for coverage through its fields, as a `match` on an enum is through its
variants. Without its last arm, `safe` does not compile (`N0256`).

A struct pattern whose field patterns are all names, `_` or such struct patterns cannot fail. That is the one
pattern a plain `let` takes without an `else` block, as the last lines of `main` show.

Fields follow the rule for names above. A binding copies a Copy field. A field that is not Copy moves out of a
value: after `is` and in a `let`, the other fields can still be read and the value as a whole cannot be used
(`N0250`), and a `match` consumes the whole value. Through a borrow, the names are borrows of the fields. A struct
with a `drop` block cannot lose a field this way: [Destruction](./ch15-02-destruction) shows the one pattern that
takes such a struct apart. It gives every field a name or `_` and tests none, so a pattern that tests a field of
such a struct only reads it.

## Mistakes

::: warning Mistake: using a value after a `match` consumed it
A name that binds a `String` takes the `String`. The enum it came from has lost a part, and the compiler treats
the whole of it as moved.
:::

<span class="listing-caption"><strong>Listing 19-27</strong>, <code>main.cee</code>: The first <code>match</code> moves the text out of <code>m</code>. This does not compile.</span>

<!-- ceesafe: fail N0250 -->
```ceesafe
enum Message {
    Text(String),
    Stop,
}

fn main() {
    let m = Message.Text(String.from("hello")!);
    match (m) {
        Message.Text(s) => { Console.writeLine(s.view()); }
        Message.Stop => {}
    }
    if (m is Message.Stop) {
        Console.writeLine("stop");
    }
}
```

```text
main.cee:12:5: error[N0250]: 'm' is used here after it was moved (on at least one path)
   |
12 |     if (m is Message.Stop) {
   |     ^^^^^^^^^^^^^^^^^^^^^^^^
   = note: the value was moved here (8:5)
```

Write `match (borrow m)` in the first `match`. Then `s` is a `borrow String`, and `m` keeps its text.

::: warning Mistake: a pattern name that a local already has
A local variable cannot be shadowed, and a name in a pattern is a new local. So a pattern can never mean "equal to
this variable", and it cannot reuse a variable's name either.
:::

<span class="listing-caption"><strong>Listing 19-28</strong>, <code>main.cee</code>: <code>n</code> exists already. This does not compile.</span>

<!-- ceesafe: fail N0203 -->
```ceesafe
fn main() {
    let n = 1;
    let o: i32? = 7;
    if (o is Some(n)) {
        Console.writeLine(n);
    }
    Console.writeLine(n);
}
```

```text
main.cee:4:19: error[N0203]: 'n' is already declared in this function; locals cannot be shadowed
  |
4 |     if (o is Some(n)) {
  |                   ^
```

Choose another name. To test that the optional holds the value of `n`, bind and compare:
`o is Some(v) && v == n`. In a `match`, the comparison goes in a guard: `Some(v) if (v == n) =>`.

::: warning Mistake: alternatives that bind different names
The arm's body is compiled once, so a name must have a value whichever alternative fitted.
:::

<span class="listing-caption"><strong>Listing 19-29</strong>, <code>main.cee</code>: <code>Event.Tick</code> has no <code>k</code>. This does not compile.</span>

<!-- ceesafe: fail N0356 -->
```ceesafe
enum Event {
    KeyDown(u8),
    KeyUp(u8),
    Tick,
}

fn key(e: Event) -> u8 => match (e) {
    Event.KeyDown(k) | Event.Tick => k,
    Event.KeyUp(_) => 0,
};

fn main() {
    Console.writeLine(key(Event.KeyUp(65)));
}
```

```text
main.cee:8:24: error[N0356]: this alternative does not bind 'k', which the first one does: every alternative binds the same names
  |
8 |     Event.KeyDown(k) | Event.Tick => k,
  |                        ^^^^^^^^^^
```

Give `Event.Tick` an arm of its own. The same message appears for `x @ 'a' | 'b'`, which reads as
`(x @ 'a') | 'b'`: write `x @ ('a' | 'b')`.

::: warning Mistake: leaving a field out of a struct pattern
A struct pattern names every field. To leave fields out, say so with `..`.
:::

<span class="listing-caption"><strong>Listing 19-30</strong>, <code>main.cee</code>: The pattern says nothing about <code>y</code>. This does not compile.</span>

<!-- ceesafe: fail N0229 -->
```ceesafe
struct Point {
    x: i32;
    y: i32;
}

fn main() {
    let p = Point { x: 0, y: 4 };
    if (p is Point { x: 0 }) {
        Console.writeLine("on the y axis");
    }
}
```

```text
main.cee:8:14: error[N0229]: this pattern does not name 'y' of 'Point': name every field, or end the pattern with '..' (ADR 0123)
  |
8 |     if (p is Point { x: 0 }) {
  |              ^^^^^^^^^^^^^^
```

Write `Point { x: 0, .. }`. The rule means that a field added to the struct later is never ignored by accident.

::: details Coming from Rust
What is the same: `_`, literals, `..=` and `..` ranges, nested variants, `|`, `name @ pattern`, slice patterns with
`..` and `rest @ ..`, guards (here with parentheses). What is not there:

- **Tuple patterns.** No `(a, b)`: match a small struct, as Listing 19-26 does. A struct pattern names every
  field or ends with `..`, as in Rust.
- **`ref`, `ref mut`, `&` and `box` patterns.** How a name binds is decided by the matched value: write
  `match (borrow x)` or `match (borrow mut x)`. A payload that is itself a borrow is bound and matched in a second
  `match`.
- **Open ranges.** No `10..` or `..=9`: write both bounds.
- **Paths and expressions as range bounds.** `LOW..=HIGH` is accepted for two constants in scope. `T.LOW..=9` and
  `LOW + 1..=9` are not: name the value with a `const`, or write a guard.
- **`mut` bindings** are written `var n`.
- **Shadowing.** A pattern name cannot reuse a local's name.
:::

::: details Coming from C#
`_`, constants, `or` (written `|`), list patterns (`[first, .., last]`) and `case … when` (a guard) have their
counterparts. What is not there:

- **Type patterns** (`is int n`, `is Circle c`). A value's type is known when the program is compiled. The cases
  of an enum are told apart by variant patterns.
- **Positional patterns** (`(0, _)`). A property pattern, `{ X: 0 }`, is the struct pattern `Point { x: 0, .. }`:
  it starts with the struct's name, and `..` stands for the fields it leaves out.
- **Relational patterns** (`> 5`, `>= 0 and < 10`). Write a range, `0..10`, or a guard.
- **`and` and `not`.** Write a guard, or `!(e is P)` around the test.
- `var n` does not mean "of any type": it means the binding can be assigned.
:::

## Summary

- The forms: `_`, a name (`var` for one that changes), literals and constants, ranges `a..=b` and `a..b`, variants
  with a pattern in each position, text, slices, structs, alternatives `p | q`, `name @ p`, and parentheses.
- Patterns nest to any depth. A variant pattern lists all of its data or none.
- Alternatives bind the same names (`N0356`). `@` binds tighter than `|`.
- A name copies a Copy part and moves any other part out of a value, which consumes the value (`N0250` on a later
  use). On a borrow, names are borrows: `match (borrow x)`, `match (borrow mut x)`, `match (this)`.
- A pattern name is a new local and cannot repeat an existing one (`N0203`).
- A struct pattern, `P { x, y: 0, .. }`, names every field or ends with `..` (`N0229`). One that cannot fail may
  stand in a plain `let`. Two values are matched together through a small struct.
- There are no tuple, float or type patterns: bind and use a guard, or match a small struct.

This is the end of the chapter. Chapter 20 turns to advanced features.
