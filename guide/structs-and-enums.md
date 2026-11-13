# Structs, enums and patterns

## Structs

A struct groups named fields. Fields end with `;`. Functions declared inside are its methods (with a `this`
receiver) or static functions (without one).

```ceesafe
struct Point {
    x: i64;
    y: i64;

    fn origin() -> Point => Point { x: 0, y: 0 };            // static: Point.origin()

    fn manhattan(this) -> i64 {                               // `this` by value (Point is Copy)
        return abs(this.x) + abs(this.y);
    }

    fn moveBy(borrow mut this, dx: i64, dy: i64) {            // changes the point in place
        this.x += dx;
        this.y += dy;
    }
}

fn abs(v: i64) -> i64 => if (v < 0) -v else v;

fn main() {
    var p = Point.origin();
    p.moveBy(3, -4);
    Console.writeLine(p.manhattan());
    let q = Point { x: 1, y: 2 };       // every field, in any order
    Console.writeLine(q.x + q.y);
}
```

```text
7
3
```

- A **struct literal** names every field exactly once.
- The **receiver** says how a method gets its struct: `this` (a copy, or a move), `borrow this` (read-only access) or
  `borrow mut this` (changes the caller's value). Calling a `borrow mut this` method needs a `var` (or an exclusive
  borrow); the call borrows it for the duration of the call.
- A struct is **Copy**, copied on assignment, when every field is Copy and it has no `drop` block. Otherwise it
  moves: see [Ownership](ownership.md).
- Fields are laid out in declaration order with C's rules, so a struct is the same in C.

### Visibility

Items and members are `internal` by default: visible throughout their package. `public` makes them visible to other
packages (see [Packages](packages.md)); `private` on a field limits it to the file that declares the struct, which
lets a type guard an invariant, and `private` on a method, or on an item, limits it to the file that declares it.

## Enums

An enum is one of several variants, each with zero or more positional payloads. Variants are written with the enum's
name.

```ceesafe
enum Shape {
    Empty,
    Circle(i64),
    Rect(i64, i64),
}

fn area(s: Shape) -> i64 => match (s) {
    Shape.Empty => 0,
    Shape.Circle(r) => 3 * r * r,
    Shape.Rect(w, h) => w * h,
};

fn main() {
    Console.writeLine(area(Shape.Circle(2)));
    Console.writeLine(area(Shape.Rect(3, 4)));
    Console.writeLine(area(Shape.Empty));
}
```

```text
12
12
0
```

An enum whose variants have no payloads compares with `==` and `!=`. An enum with payloads compares with `match`, or
with `==` once it derives equality (below).

```ceesafe
enum Kind { User, Alias, List }

fn main() {
    let kind = Kind.Alias;
    Console.writeLine(kind == Kind.Alias);
    Console.writeLine(kind != Kind.User);
}
```

```text
true
true
```

### Methods on an enum

An enum's body holds its variants. Its methods and static functions are written in an `impl` block, with the same
receivers as a struct's, and are called the same way.

```ceesafe
enum Light {
    Red,
    Green,
    Blinking(u32),
}

impl Light {
    fn start() -> Light => Light.Red;                         // static: Light.start()

    fn name(borrow this) -> StringView => match (this) {
        Light.Red => "red",
        Light.Green => "green",
        Light.Blinking(_) => "blinking",
    };

    fn advance(borrow mut this) {                             // changes the caller's value
        match (this) {
            Light.Red => { this = Light.Green; }
            Light.Green => { this = Light.Blinking(1); }
            Light.Blinking(times) => { times += 1; }
        }
    }
}

fn main() {
    var light = Light.start();
    Console.writeLine(light.name());
    light.advance();
    Console.writeLine(light.name());
    light.advance();
    light.advance();
    match (light) {
        Light.Blinking(times) => { Console.writeLine(times); }
        _ => {}
    }
}
```

```text
red
green
2
```

- A `borrow mut this` method changes a payload through its `match` binding, or replaces the whole value with
  `this = …`.
- A generic enum's block names its parameters, `impl<T> Maybe<T> { … }`, and its static functions are called with
  the type arguments: `Maybe<i32>.empty()`.
- A function cannot have the name of a variant, and the block is written in the package that declares the enum.
- A struct takes `impl` blocks too: `impl Point { … }` adds to the functions in its body.

### Deriving equality, hashing and ordering

Structs and enums have no `==` of their own. `@derive(Equal)` gives one: the fields are compared in declaration order
(an enum's variant, then its payloads), and `!=` is the opposite. `@derive(HashKey)` (which needs `Equal`) makes the
type a `HashMap` key, and `@derive(OrderKey)` an `OrderedMap` key ordered field by field.

```ceesafe
@derive(Equal, HashKey, OrderKey)
struct Point {
    x: i32;
    y: i32;
}

@derive(Equal)
enum Shape {
    Dot(Point),
    Box(Point, Point),
}

fn main() {
    let a = Point { x: 1, y: 2 };
    let b = Point { x: 1, y: 5 };
    Console.writeLine(a == b);
    Console.writeLine(Shape.Box(a, b) == Shape.Box(a, b));
    Console.writeLine(a.compareKey(b));
    var seen = HashMap<Point, u32>.create();
    seen.put(a, 1)!;
    Console.writeLine(seen.contains(Point { x: 1, y: 2 }));
}
```

```text
false
true
-1
true
```

Every field needs `==` itself: numbers, `bool`, strings and views, fieldless enums, byte vectors and slices, optionals
of these (two `null`s are equal), and other types that derive `Equal`, with arrays, slices and vectors of them (`==`
works on those anywhere: same length, equal elements). Floats compare as IEEE values, so a `NaN` field
makes a value unequal to itself. A field without `==` is an error at the attribute that names it; write a method of
your own for such a type. The derived code is ordinary CeeSafe that `ceesafec check --dump-hir` prints: for `Point`,
`impl DerivedEqual<Point> for Point { fn derivedEqual(borrow this, other: borrow Point) -> bool => this.x == other.x
&& this.y == other.y; }`, and `a == b` calls it. For `HashKey` and `OrderKey`, a field whose type is a fieldless enum
needs no derive of its own: its variant's position in the declaration is hashed and compared.

## `match`

`match` tries its arms in order. It must be **exhaustive**: every possible value matches some arm, or the compiler
names one that does not. An arm that can never be reached is an error too.

As a statement, each arm is a block; as an expression (above), each arm is a value, separated by commas.

```ceesafe
enum Command {
    Move(i32, i32),
    Wait(u32),
    Quit,
}

fn run(command: Command) {
    match (command) {
        Command.Move(0, 0) => { Console.writeLine("stay"); }
        Command.Move(x, _) => {
            Console.write("move x by ");
            Console.writeLine(x);
        }
        Command.Wait(seconds) => {
            Console.write("wait ");
            Console.writeLine(seconds);
        }
        Command.Quit => {}
    }
}

fn main() {
    run(Command.Move(0, 0));
    run(Command.Move(5, 1));
    run(Command.Wait(3));
    run(Command.Quit);
}
```

```text
stay
move x by 5
wait 3
```

Patterns can be nested: a variant inside a variant (`Ok(Shape.Circle(r))`), a literal in a payload's position
(`Command.Move(0, 0)`), `null` or `Some(…)` for optionals, and `_` for anything. Integers and booleans can be matched
directly, with `_` for the rest.

```ceesafe
fn kind(n: u8) -> StringView => match (n) {
    0 => "zero",
    'a' => "the letter a",
    _ => "something else",
};

fn main() {
    Console.writeLine(kind(97));
    Console.writeLine(kind(3));
}
```

```text
the letter a
something else
```

### Alternatives

`A | B` matches when either pattern does, at the top of an arm or inside a payload. When the alternatives bind
names, each binds the same ones, with the same types.

```ceesafe
enum Token {
    Word(u32),
    Number(u32),
    End,
}

fn isSpace(c: u8) -> bool => match (c) {
    ' ' | '\t' | '\n' => true,
    _ => false,
};

fn length(t: Token) -> u32 => match (t) {
    Token.Word(n) | Token.Number(n) => n,
    Token.End => 0,
};

fn main() {
    Console.writeLine(isSpace('\t'));
    Console.writeLine(length(Token.Number(3)));
}
```

```text
true
3
```

### Constants and names

A bare name in a pattern is a constant when a `const` of that name is in scope, and otherwise binds the value at
that position, at the top of an arm too. `Type.NAME` names an associated constant.

```ceesafe
const LF: u8 = 10;

fn describe(c: u8) -> StringView => match (c) {
    LF => "newline",
    other => if (other < 32) "control" else "text",
};

fn main() {
    Console.writeLine(describe(10));
    Console.writeLine(describe(65));
}
```

```text
newline
text
```

A misspelt constant binds instead and matches everything; the compiler then reports the arms after it as
unreachable.

### Ranges

`lo..=hi` matches the integers from `lo` to `hi`, both included; `lo..hi` leaves `hi` out. The bounds are integer or
character literals, or the names of constants of the value's type (`LOW..=HIGH`, `0..LOW`). Literals and ranges that together hold every value of the type need no `_`.

```ceesafe
fn kind(c: u8) -> StringView => match (c) {
    'a'..='z' | 'A'..='Z' | '_' => "letter",
    '0'..='9' => "digit",
    _ => "other",
};

fn half(b: u8) -> u32 => match (b) {
    0x00..=0x7F => 1,
    0x80..=0xFF => 2,
};

fn main() {
    Console.writeLine(kind('q'));
    Console.writeLine(kind('7'));
    Console.writeLine(half(200));
}
```

```text
letter
digit
2
```

A match on literals and ranges compiles to a C `switch`.

### Naming what matched

`name @ pattern` tests the pattern and binds the value it matched:

```ceesafe
fn digit(c: u8) -> u32 => match (c) {
    d @ '0'..='9' => (d - '0') as u32,
    q @ ('"' | '\'') => 100 + q as u32,
    _ => 999,
};

fn main() {
    Console.writeLine(digit('7'));
}
```

```text
7
```

A value moves at most once: in a match that moves payloads out, `whole @ Slot.Full(inner)` cannot move the value into
`whole` and its payload into `inner` too (N0250); bind one of them.

### Guards

An arm can add a condition after its pattern, `if (…)`: the arm is taken only when the pattern matches and the
condition holds; otherwise the next arm is tried. The guard sees the arm's bindings.

```ceesafe
fn size(x: u32?) -> StringView => match (x) {
    Some(n) if (n > 100) => "big",
    Some(0) => "zero",
    Some(_) => "small",
    null => "none",
};

fn main() {
    Console.writeLine(size(Some(500)));
    Console.writeLine(size(Some(7)));
}
```

```text
big
small
```

A guarded arm never counts toward exhaustiveness: the compiler cannot know when its condition holds, so the other
arms must still cover every value. A guard only reads: it cannot move, assign or lend as `mut` the matched value or
the arm's bindings (N0355). In a match that moves a payload into its binding, the move happens only once the guard
holds.

### Strings and slices

A string literal is a pattern on a `StringView`, and on a `Slice<u8>`, where it matches the text's bytes. A slice
pattern, `[…]`, matches a slice or an array by its elements: `..` stands for any number of them, and `rest @ ..`
binds those as a slice.

```ceesafe
fn command(word: StringView) -> u32 => match (word) {
    "get" | "head" => 1,
    "put" => 2,
    _ => 0,
};

fn ends(s: Slice<i32>) -> i32 => match (s) {
    [] => 0,
    [only] => only,
    [first, .., last] => first + last,
};

fn unquote(text: Slice<u8>) -> Slice<u8> => match (text) {
    ['"', inner @ .., '"'] => inner,
    _ => text,
};

fn main() {
    Console.writeLine(command("head"));
    let numbers = [3, 5, 8];
    Console.writeLine(ends(numbers[..]));
    Console.writeLine(unquote("\"quoted\"".bytes()).length);
}
```

```text
1
11
6
```

Strings never cover their type, so a match on a `StringView` ends with `_` or a binding. Slice patterns cover by
length: `[]` and `[_, ..]` together match every slice, and a missing case is reported with one:

<!-- ceesafe: fail N0256 -->
```ceesafe
fn size(s: Slice<i32>) -> u32 => match (s) {
    [] => 0,
    [_] => 1,
};

fn main() {}
```

```text
main.cee:1:41: error[N0256]: this match does not cover [_, _, ..]; add the missing arms or a '_' arm
  |
1 | fn size(s: Slice<i32>) -> u32 => match (s) {
  |                                         ^
```

A slice pattern never moves an element: one of a Copy type is copied, any other is bound as a `borrow T`.

### Patterns as conditions

`e is P` tests a value against a pattern and gives a `bool`. In an `if` or `while` condition, the names the pattern
binds are in scope in the rest of the `&&` chain and in the then-branch or the loop body:

```ceesafe
fn find(key: u32) -> u32? => if (key > 2) Some(key * 10) else null;

fn main() {
    let c = 'q';
    let letter = c is 'a'..='z' | 'A'..='Z';
    Console.writeLine(letter);
    if (find(5) is Some(found) && found > 20) {
        Console.writeLine(found);
    }
    var stack = Vector<u32>.create();
    stack.push(1)!;
    stack.push(2)!;
    while (stack.pop() is Some(top)) {
        Console.writeLine(top);
    }
}
```

```text
true
50
2
1
```

Nowhere else is the test known to be true, so a pattern that binds under `!` or `||`, or as a whole `let`
initialiser, is an error (N0359). In a chain outside a condition, `let big = find(5) is Some(v) && v > 20;`, the
names end with the statement, so a `RefCell` guard bound that way is released before the next one. A test that binds a payload that is not Copy moves the value only when it succeeds:
after `if (t is Token.Word(w)) { … } else { … }`, the `else` branch still has `t`.

### Bind or leave: `let … else`

When the rest of a function needs what a pattern binds, `let PATTERN = value else { … };` binds the names for the
rest of the block, and runs the `else` block when the value does not match. That block must leave: `return`, `break`,
`continue`, or a call that does not return, such as `Process.exit` (N0213 otherwise). It does not see the names.

```ceesafe
enum Shape {
    Circle(u32),
    Rect(u32, u32),
}

fn area(s: Shape) -> u32 {
    let Shape.Rect(w, h) = s else {
        return 0;
    };
    return w * h;
}

fn main() {
    Console.writeLine(area(Shape.Rect(3, 4)));
    Console.writeLine(area(Shape.Circle(3)));
    let items: [u32?; 4] = [1, null, 3, 100];
    var total: u32 = 0;
    for (item in items) {
        let Some(n) = item else {
            continue;
        };
        let 0..=9 = n else {
            break;
        };
        total += n;
    }
    Console.writeLine(total);
}
```

```text
12
0
4
```

Any pattern may stand after `let`; the statement ends with `;` like every `let`. A payload that is not Copy moves
out of the value when the pattern matches, and the `else` block still has the whole value. A pattern that cannot
fail because it tests nothing (`let x = e else { … };`, and `if (n is _)` too) is a warning (N0404).

## Optionals

`T?` is a `T` or `null`. A plain `T` converts to `T?` wherever one is expected.

```ceesafe
fn find(values: Slice<i32>, wanted: i32) -> usize? {
    for (i in 0..values.length) {
        if (values[i] == wanted) {
            return i;                  // converts to usize?
        }
    }
    return null;
}

fn firstOfTwo(values: Slice<i32>) -> usize? {
    let a = find(values, 2)?;           // `?`: if null, return null
    return a;
}

fn main() {
    let numbers = [5, 2, 9];
    match (find(numbers[..], 9)) {
        Some(i) => { Console.writeLine(i); }
        null => { Console.writeLine("absent"); }
    }
    Console.writeLine(find(numbers[..], 7) == null);
    Console.writeLine(firstOfTwo(numbers[..])!);   // `!`: the value, or stop the program if null
    Console.writeLine(find(numbers[..], 7) ?? 99); // `??`: the value, or a fallback
    Console.writeLine(find(numbers[..], 5).isSome());
}
```

```text
2
true
1
99
true
```

- `o == null` and `o != null` test for absence; so do `o.isNull()` and `o.isSome()`.
- `o ?? fallback` is the value, or `fallback` when `o` is `null`. The fallback is evaluated only then, and a chain
  `a ?? b ?? c` takes the first value present.
- `o!` is the value; on `null` it stops the program with "unwrapped null".
- `o?` is the value; on `null` the enclosing function returns `null` (it must return an optional).
- A borrow can be optional (`borrow T?`), and so can a box (`Box<T>?`); both are one pointer.

## Results

`Result<T, E>` is the other built-in enum: `Ok(value)` or `Err(error)`. It is how functions report failure; see
[Errors](errors.md).

## Next

[Ownership and borrowing](ownership.md): what happens when a value is assigned, passed and destroyed.
