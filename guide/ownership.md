# Ownership and borrowing

CeeSafe manages memory without a garbage collector by tracking, at compile time, who owns each value and who is
looking at it. This chapter explains the rules, why they hold, and what the compiler says when a program breaks
them. The formal rules are in [Ownership](../spec/ownership.md) and
[References and the borrow checker](../spec/lifetimes.md).

## Copy types and move types

Integers, floats, `bool`, references and string views are **Copy**: assigning one copies it, and both copies stay
usable. A struct or enum is Copy when everything in it is Copy and it has no `drop` block.

Everything else is a **move type**: boxes, vectors, strings, files, and any struct that owns one or has a `drop`
block. Assigning a move-type value, passing it, returning it or storing it **moves** it. The old place is then empty,
and using it is a compile error.

<!-- ceesafe: fail N0250 -->
```ceesafe
struct Buffer {
    size: u32;
    drop { Console.writeLine("freed"); }
}

fn consume(b: Buffer) {}

fn main() {
    let b = Buffer { size: 64 };
    consume(b);
    Console.writeLine(b.size);
}
```

```text
main.cee:11:5: error[N0250]: 'b.size' is used here after it was moved (on at least one path)
   |
11 |     Console.writeLine(b.size);
   |     ^^^^^^^^^^^^^^^^^^^^^^^^^^
   = note: the value was moved here (10:5)
```

Moves are what make destruction safe: a value has exactly one owner, so it is destroyed exactly once.

A field can be moved out of a struct that has no `drop` block. The other fields stay owned and usable, the struct
as a whole does not, and at the end of the scope only the fields that are left are destroyed:

```ceesafe
struct Pair {
    first: String;
    second: String;
}

fn main() {
    let p = Pair { first: String.from("one")!, second: String.from("two")! };
    let first = p.first;                // p.first is moved; p.second is still p's
    Console.writeLine(p.second);
    Console.writeLine(first);
}
```

```text
two
one
```

## Destruction

A value is destroyed when its owner goes away: at the end of the variable's scope, when a temporary's statement
ends, or when an assignment overwrites it. Destroying a value runs its `drop` block, if it has one, and then destroys
its fields. Locals are destroyed in the reverse order of their declaration.

```ceesafe
struct Noisy {
    id: u32;
    drop {
        Console.write("drop ");
        Console.writeLine(this.id);
    }
}

fn main() {
    let first = Noisy { id: 1 };
    let second = Noisy { id: 2 };
    {
        let inner = Noisy { id: 3 };
    }                                   // inner is destroyed here
    var replaced = Noisy { id: 4 };
    replaced = Noisy { id: 5 };         // 4 is destroyed by the assignment
    let moved = first;                  // first is moved: it is destroyed once, as `moved`
    Console.writeLine("end of main");
}
```

```text
drop 3
drop 4
end of main
drop 1
drop 5
drop 2
```

At the end of `main`, the locals are destroyed in reverse order of declaration: `moved` (holding 1), `replaced`
(holding 5), then `second`. `first` was moved, so it is not destroyed again. A value moved on only some paths gets a
hidden flag, so it is still destroyed exactly once.

A method that takes `this` by value consumes its value, which is destroyed at the end of the method. `this` is
immutable there, as every parameter is, so a method that must change the value first (to tell its `drop` block that
it was finished properly, say) moves it to a variable: `var me = this; me.armed = false;`. A field of a struct with a
`drop` block cannot be moved out, since the block reads the whole value; take it with
`replace(mut me.name, String.create())`.

A **panic** or `Process.exit` does not run `drop` blocks: a panic stops the program at once (there is no unwinding).

### `defer` and `using`

`defer s;` runs `s` when the enclosing block exits by any path: its end, `break`, `continue` or `return`.
`using x = e;` declares a local that cannot be moved away: it lives exactly as long as its block, which documents
that its lifetime *is* the scope (a lock guard, an arena, a file).

```ceesafe
fn work(fail: bool) -> i32 {
    defer Console.writeLine("cleanup");
    if (fail) {
        return 1;
    }
    Console.writeLine("working");
    return 0;
}

fn main() {
    Console.writeLine(work(false));
    Console.writeLine(work(true));
}
```

```text
working
cleanup
0
cleanup
1
```

## Borrowing

Most code does not want to take ownership; it wants to look at a value or change it in place. A **reference** does
that without moving:

- `borrow T` is a shared reference: it can read, and any number can exist at once;
- `borrow mut T` is an exclusive reference: it can read and write, and while it is in use nothing else can touch
  the value.

```ceesafe
struct Counter {
    hits: u64;
}

fn show(c: borrow Counter) {               // looks at the counter
    Console.writeLine(c.hits);
}

fn record(c: borrow mut Counter) {         // changes it in place
    c.hits += 1;
}

fn main() {
    var counter = Counter { hits: 0 };
    record(mut counter);                   // `mut` at the call: this call changes `counter`
    record(mut counter);
    show(counter);                         // borrowed for the call; no marker needed
}
```

```text
2
```

A `borrow T` parameter accepts the value itself, borrowed implicitly. A `borrow mut T` parameter needs `mut` before
the argument, so that every place where a value may change is visible where it is called, as C#'s `ref` is.

### The rule

While a borrow is **live**, meaning the reference or anything made from it is still used later, the borrowed value
may not be:

- changed, mutably borrowed or destroyed, if the borrow is shared;
- used in any way, if the borrow is mutable.

The compiler checks this within each function, on every path. A borrow ends at its last use, not at the end of its
scope.

<!-- ceesafe: fail N0240 -->
```ceesafe
fn main() {
    var total = 0;
    let r = borrow total;
    total += 1;
    Console.writeLine(r);
}
```

```text
main.cee:4:5: error[N0240]: cannot assign to 'total' while 'total' is borrowed
  |
4 |     total += 1;
  |     ^^^^^^^^^^^
  = note: 'total' is borrowed here (3:5)
  = note: the borrow is used later here (5:5)
```

Moving the `Console.writeLine(r)` above the `+=` fixes it: the borrow is then dead before the write. The same rule
is what stops a vector from growing while an element is borrowed, since growing can move its elements.

### Returning references

A function that returns a reference must return something borrowed from its parameters, never from its own locals.
With one reference parameter, that is assumed. With several, the result names the ones it may borrow from:

```ceesafe
fn larger(a: borrow i64, b: borrow i64) -> borrow(a, b) i64 => if (a > b) a else b;

fn firstPositive(values: Slice<i64>) -> borrow i64 {
    for (i in 0..values.length) {
        if (values[i] > 0) {
            return borrow values[i];
        }
    }
    return borrow values[0];
}

fn main() {
    let x: i64 = 3;
    let y: i64 = 7;
    Console.writeLine(larger(x, y));
    let numbers = [-1i64, 4, 9];
    Console.writeLine(firstPositive(numbers[..]));
}
```

```text
7
4
```

The caller then knows how long the result stays valid: as long as the arguments it borrows from. Returning a
reference to a local is rejected, since the local is gone when the function returns:

<!-- ceesafe: fail N0244 -->
```ceesafe
fn pick(a: borrow i32) -> borrow i32 {
    let copy = a + 1;
    return borrow copy;
}

fn main() {
    Console.writeLine(pick(1));
}
```

```text
main.cee:3:5: error[N0244]: cannot return a reference to 'copy', which is local to 'pick'
  |
3 |     return borrow copy;
  |     ^^^^^^^^^^^^^^^^^^^
  = note: borrowed here (3:12)
```

### Two mutable borrows

Two `borrow mut` references to one value can never be live together, even as two arguments of one call:

<!-- ceesafe: fail N0241 -->
```ceesafe
fn addTo(target: borrow mut i32, amount: borrow mut i32) {
    target += amount;
}

fn main() {
    var x = 1;
    addTo(mut x, mut x);
}
```

```text
main.cee:7:18: error[N0241]: cannot borrow 'x' as mutable while 'x' is mutably borrowed
  |
7 |     addTo(mut x, mut x);
  |                  ^^^^^
  = note: 'x' is mutably borrowed here (7:11)
  = note: the borrow is used later here (7:5)
```

## References in structs: regions

A struct that holds a reference or a view must say so, with a **region parameter**: the region is the lifetime of
what the reference points into. Values of such a type cannot outlive that region.

```ceesafe
struct Word<region src> {
    text: StringView<src>;
    start: usize;
}

fn firstWord<region src>(line: StringView<src>) -> Word<src> {
    var end: usize = 0;
    while (end < line.length && line[end] != ' ') {
        end += 1;
    }
    return Word { text: line[0..end], start: 0 };
}

fn main() {
    let line = "hello wide world";
    let word = firstWord(line);
    Console.writeLine(word.text);
}
```

```text
hello
```

Plain functions that take and return references never need region parameters; only types that *store* references,
and functions that pass such types through, do. Regions are a compile-time idea: they cost nothing at run time.

## What the checker does not do

The borrow checker works within one function at a time, using the signatures of the functions it calls. It is
deliberately simpler than Rust's: some correct programs are rejected. When that happens, the usual fixes are to end
a borrow earlier, copy a value out, split a function, or use an index or a [slot map handle](collections.md) instead
of a stored reference.

## Next

[Errors](errors.md): `Result`, `?`, `!` and panics.
