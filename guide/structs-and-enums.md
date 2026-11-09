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
  `borrow mut this` (changes the caller's value). Calling a `borrow mut this` method needs a `var` (or a mutable
  reference); the call borrows it for the duration of the call.
- A struct is **Copy**, copied on assignment, when every field is Copy and it has no `drop` block. Otherwise it
  moves: see [Ownership](ownership.md).
- Fields are laid out in declaration order with C's rules, so a struct is the same in C.

### Visibility

Items and members are `internal` by default: visible throughout their package. `public` makes them visible to other
packages (see [Packages](packages.md)); `private` on a field limits it to the file that declares the struct, which
lets a type guard an invariant.

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

An enum whose variants have no payloads compares with `==` and `!=`. An enum with payloads compares with `match`.

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
- A reference can be optional (`borrow T?`), and so can a box (`Box<T>?`); both are one pointer.

## Results

`Result<T, E>` is the other built-in enum: `Ok(value)` or `Err(error)`. It is how functions report failure; see
[Errors](errors.md).

## Next

[Ownership and borrowing](ownership.md): what happens when a value is assigned, passed and destroyed.
