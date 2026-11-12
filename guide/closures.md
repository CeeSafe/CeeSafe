# Functions and closures

## Functions are values

A function can be stored in a variable and passed as an argument. Its type is written `fn(parameter types) -> result`:

```ceesafe
fn double(x: i32) -> i32 => x * 2;
fn negate(x: i32) -> i32 => -x;

fn applyTwice(f: fn(i32) -> i32, value: i32) -> i32 => f(f(value));

fn main() {
    Console.writeLine(applyTwice(double, 3));
    var op: fn(i32) -> i32 = double;
    op = negate;
    Console.writeLine(op(5));
}
```

```text
12
-5
```

A function value is one pointer, and calling it is an indirect call.

## Closures

A closure is a function written inline, `(parameters) => body`, that can use the variables around it. It borrows
what it captures, so it is a kind of reference: its type is `borrow fn(…) -> R` if it only reads what it captures,
and `borrow mut fn(…) -> R` if it changes something.

```ceesafe
fn countIf(values: Slice<i32>, keep: borrow fn(i32) -> bool) -> i32 {
    var n = 0;
    for (v in values) {
        if (keep(v)) {
            n += 1;
        }
    }
    return n;
}

fn forEach(values: Slice<i32>, visit: borrow mut fn(i32)) {
    for (v in values) {
        visit(v);
    }
}

fn main() {
    let values = [3, 8, 1, 9, 4];
    let limit = 4;
    Console.writeLine(countIf(values[..], (v) => v > limit));     // captures `limit`
    var total = 0;
    forEach(values[..], (v) => { total += v; });                   // changes `total`
    Console.writeLine(total);
    let twice = (x: i32) => x * 2;                                  // captures nothing: a plain fn
    Console.writeLine(twice(21));
}
```

```text
2
25
42
```

- Parameter types can be left out where the expected type gives them, as in the calls above.
- The body is an expression, or a block: `(v) => { total += v; }`.
- A closure that captures nothing is an ordinary function value, and can be stored anywhere a function can.

### Closures do not outlive their block

Because a closure borrows its captures, the borrow checker treats it like any reference: it cannot be returned from
the function that made it, kept in a struct that outlives what it captured, or kept after a captured variable
changes. That makes closures free of allocation and safe to use in kernels.

A struct holds a borrowed closure as it holds any reference: with a region parameter. The field is called by its
name.

```ceesafe
struct Rule<region r> {
    test: borrow(r) fn(i32) -> bool;
}

fn main() {
    let limit = 10;
    let rule = Rule { test: (v) => v < limit };
    Console.writeLine(rule.test(4));
    Console.writeLine(rule.test(40));
}
```

```text
true
false
```

A struct that owns its closure holds a boxed one (next section): `onPress: Box<dyn fn(i32) -> i32>`, called as
`b.onPress(4)`.

### Owning closures

A `move` literal moves what it captures into the closure, so the closure can outlive the function that made it. It
is boxed, with an allocation you can see and handle, as a `Box<dyn fn(…) -> R>`. A closure that changes what it
captured is a `Box<dyn mut fn(…) -> R>`, and calling it needs mutable access.

```ceesafe
fn adder(n: u64) -> Box<dyn fn(u64) -> u64> {
    return Box.create(move (x) => x + n)!;
}

fn counter() -> Box<dyn mut fn() -> u64> {
    var count: u64 = 0;
    return Box.create(move () => {
        count += 1;
        return count;
    })!;
}

fn main() {
    let add2 = adder(2);
    Console.writeLine(add2(3));
    var next = counter();
    next();
    Console.writeLine(next());
    var steps = Vector<Box<dyn fn(u64) -> u64>>.create();
    steps.push(adder(1))!;
    steps.push(adder(10))!;
    var x: u64 = 0;
    for (step in steps) {
        x = step(x);
    }
    Console.writeLine(x);
}
```

```text
5
2
11
```

- The literal goes straight into `Box.create(…)` where a `Box<dyn fn…>` is expected; its parameter types come from
  there.
- A captured variable is moved in, and cannot be used afterwards (a Copy one is copied). It cannot hold a reference.
- The body changes the closure's own copy of what it captured, and cannot move it out.

## Methods that take closures

Slices, arrays (through `a[..]`) and vectors have `sortBy`, `forEach`, `any`, `all` and `partitionPoint`, and
vectors `retain`:

```ceesafe
struct Reading {
    sensor: u32;
    celsius: f64;
}

fn main() {
    var readings = Vector<Reading>.create();
    for (i in 0..8u32) {
        readings.push(Reading { sensor: i % 3, celsius: 18.5 + (i as f64) * 0.75 })!;
    }
    let limit = 20.0;
    readings.retain((r) => r.celsius > limit);
    readings.sortBy((a, b) => if (a.celsius > b.celsius) -1 else if (a.celsius < b.celsius) 1 else 0);
    var sum = 0.0;
    readings.forEach((r) => { sum += r.celsius; });
    Console.writeLine(readings.length);
    Console.writeLine(readings[0].celsius);
    Console.writeLine(readings.any((r) => r.sensor == 0));
}
```

```text
5
23.75
true
```

`sortBy` is an introsort: it allocates nothing and runs in O(n log n), but it is not stable. Sorted, reversed and
duplicate-heavy input take about one pass. Its comparator returns a
negative number, zero or a positive number. These methods are written in CeeSafe, in `lib/core/slices.cee`.

`partitionPoint` is the binary search: on a slice whose elements pass a test and then fail it, it returns the index
of the first that fails, or the length. For a slice sorted by a key, `entries.partitionPoint((e) => e.key < k)` is
where `k` is, or where it would go. Prefer it to a search loop of your own: its number of steps depends on the
length alone, which made it nearly twice as fast as a loop that ends when two bounds meet.

In an async fn, they take async closures too: `await readings.forEach(async (r) => await upload(r))` calls the
closure on each element and waits for each call. Such a call is a task, so it needs its `await`. The same holds for
your own functions: a function marked `@effectsOf(f)` for its closure parameter `f` waits for an async `f`. It must
then only call `f`, or pass it on to another such function. It must not store it.

## Effect lists

In kernel code, a callback may run where sleeping or allocating is forbidden. A function type can say what its
functions may do: `@effects() fn(i32)` may neither sleep nor allocate, and `@effects(alloc) fn(i32)` may allocate but
not sleep. The compiler checks every function and closure given for it. See [Kernel code](kernel.md).

```ceesafe
fn onEach(values: Slice<u32>, f: borrow mut @effects() fn(u32)) {
    for (v in values) {
        f(v);
    }
}

fn main() {
    var largest: u32 = 0;
    let values = [4u32, 11, 7];
    onEach(values[..], (v) => {
        if (v > largest) {
            largest = v;
        }
    });
    Console.writeLine(largest);
}
```

```text
11
```

## Floats

`f32` and `f64` follow IEEE 754: no operation on them panics, and NaN and the infinities behave as specified. They
print as the shortest text that reads back as the same value.

```ceesafe
fn mean(values: Slice<f64>) -> f64 {
    var sum = 0.0;
    for (v in values) {
        sum += v;
    }
    return sum / round<f64>(values.length);
}

fn main() {
    let samples = [1.5, 2.25, 4.0];
    let m = mean(samples[..]);
    Console.writeLine(m);
    Console.writeLine(m.sqrt());
    Console.writeLine(truncate<i64>(m * 1000.0));
    Console.writeLine(0.1 + 0.2);
}
```

```text
2.5833333333333335
1.6072751268321592
2583
0.30000000000000004
```

Integers become floats with `as` when exact (`i32 as f64`) or `round<f64>(n)`; floats become integers with
`truncate<T>(x)` (toward zero; out of range stops the program), `checkedTruncate` or `saturatingTruncate`. The math
methods are `sqrt`, `abs`, `floor`, `ceil`, `min` and `max`. Kernel code compiles with `--no-float`, which rejects
float types altogether.

## Next

[Threads](threads.md).
