---
title: Destruction
summary: When and in what order values are destroyed - drop blocks and what they may not do, temporaries, moved values, using for a value that stays for its block, and defer for a statement that runs when the block ends.
assumes: [ch04-01-what-ownership-is, ch05-03-methods, ch09-02-result]
keywords: [drop, "drop block", destruction, destructor, using, defer, move, "order of destruction", temporary, Process.exit, cleanup]
diagnostics: [N0252, N0253, N0254, N0261]
specification: ["type-system.md §14.2", "type-system.md §14.3", "type-system.md §14.5", "type-system.md §14.6", "type-system.md §14.7"]
---

# Destruction

**This section teaches** the whole of destruction: what a `drop` block is and what it cannot do, the order in
which values are destroyed, and the two statements `using` and `defer`. It assumes
[ownership](./ch04-01-what-ownership-is), which shows the first `drop` block, [methods](./ch05-03-methods) and
[`Result`](./ch09-02-result).

A value that owns something gives it back when it is destroyed: a `String` its text, a `Box` its memory, a file its
handle. The compiler puts the destruction in the program at the place where the value's owner ends. You write no
call, and you cannot forget one.

## `drop` blocks and the order

A struct may have one `drop` block. It runs when a value of the struct is destroyed, and inside it `this` is an
exclusive borrow of the value. After the block, the struct's fields are destroyed.

<span class="listing-caption"><strong>Listing 15-9</strong>, <code>main.cee</code>: Locals are destroyed in reverse. A struct runs its <code>drop</code> block, then destroys its fields in the order they are declared.</span>

```ceesafe
struct Part {
    name: String;

    drop {
        Console.write("  part ");
        Console.writeLine(this.name);
    }
}

struct Machine {
    engine: Part;
    wheels: Part;

    drop {
        Console.writeLine("machine");
    }
}

fn part(name: StringView) -> Part => Part { name: String.from(name)! };

fn main() {
    let first = part("first");
    let machine = Machine { engine: part("engine"), wheels: part("wheels") };
    let last = part("last");
    Console.writeLine("end of main");
}
```

```text
end of main
  part last
machine
  part engine
  part wheels
  part first
```

At the end of `main` the locals go in the reverse of the order they were declared in: `last`, `machine`, `first`.
Reverse order is what makes it safe for a later value to use an earlier one.

Destroying `machine` runs its `drop` block while both parts are still whole, so the block may use them. Then
`engine` and `wheels` are destroyed, in declaration order. Each `Part` runs its own block and then frees its
`name`.

::: tip Rule
Destroying a struct runs its `drop` block, then destroys its fields in declaration order. The locals of a block
are destroyed in reverse declaration order, and by-value parameters after the locals. A vector destroys its
elements from index 0 upward.

*Specification: `type-system.md` §14.2, §14.5.*
:::

A struct with a `drop` block is a move type even if all its fields are Copy: a copy would be destroyed twice. An
enum cannot have a `drop` block. Its payloads are destroyed as a struct's fields are.

## When a value is destroyed

A value is destroyed exactly once, at the first of these: the end of its owner's block, an assignment that
replaces it, or, for a value with no name, the end of its statement. A value that was moved is not destroyed
where it was: its new owner does that.

<span class="listing-caption"><strong>Listing 15-10</strong>, <code>main.cee</code>: Temporaries, a value moved on one path, and a value ended early.</span>

```ceesafe
struct Noisy {
    id: i32;

    drop {
        Console.write("destroy ");
        Console.writeLine(this.id);
    }
}

fn make(id: i32) -> Noisy => Noisy { id: id };

fn consume(n: Noisy) {
    let local = make(100);
    Console.writeLine("in consume");
}

fn main() {
    let a = make(1);
    make(2);
    let _ = make(3);
    Console.writeLine("after the temporaries");
    let b = make(4);
    if (a.id == 1) {
        consume(b);
    }
    let early = make(5);
    let _ = move early;
    Console.writeLine("end of main");
}
```

```text
destroy 2
destroy 3
after the temporaries
in consume
destroy 100
destroy 4
destroy 5
end of main
destroy 1
```

- `make(2);` makes a value that nothing owns. It is destroyed at the end of the statement. `let _ = make(3);`
  discards a value in writing, with the same effect.
- `consume(b)` moves `b` into the parameter `n`. When `consume` returns, its local `100` is destroyed, then `n`.
- `b` is moved only if the condition holds. The compiler cannot know that, so it keeps a hidden flag beside `b`
  and tests it at the end of `main`. Here the flag says "moved", and nothing happens.
- `move early` moves the value out of `early`, and `let _ =` discards it. Together they destroy a value before
  its block ends.

::: tip Rule
A value is destroyed exactly once: at the end of its owner's block, when its owner is assigned, or at the end of
its statement if it has no owner. A moved value is destroyed by whoever owns it last.

*Specification: `type-system.md` §14.5.*
:::

## What a `drop` block cannot do

A `drop` block is not a method. It takes no parameters, returns no value, and cannot be called: `a.drop()` is a
syntax error, because `drop` is a keyword. It runs once, when the compiler says so. Inside it:

- it cannot report a failure. `?` has nowhere to return an error to (`N0261`, shown below);
- it cannot move a field out of `this`, because `this` is a borrow;
- it cannot assign `this = …`, which would destroy the value a second time;
- it can read and change fields, call methods, print, and leave early with `return;`.

And a `drop` block does not run at all when the program ends through a panic or `Process.exit`. Both stop the
process at once. The operating system takes back the memory and closes the files, but a buffer that a `drop` block
would have written out stays unwritten.

When the last piece of work on a value can fail, put it in a method that returns a `Result` and records that it
was done. The caller calls it and handles the error. The `drop` block remains for the paths where nobody called it.

<span class="listing-caption"><strong>Listing 15-11</strong>, <code>main.cee</code>: <code>save</code> can fail and says so. The <code>drop</code> block runs later, and sees whether the work was done.</span>

```ceesafe
enum SaveError {
    Empty,
}

struct Document {
    words: i32;
    saved: bool;

    fn save(borrow mut this) -> Result<void, SaveError> {
        if (this.words == 0) {
            return Err(SaveError.Empty);
        }
        Console.writeLine("saved");
        this.saved = true;
        return Ok();
    }

    drop {
        if (!this.saved) {
            Console.writeLine("discarded without saving");
        }
    }
}

fn main() {
    var report = Document { words: 120, saved: false };
    report.save()!;
    var empty = Document { words: 0, saved: false };
    match (empty.save()) {
        Ok() => {}
        Err(SaveError.Empty) => { Console.writeLine("nothing to save"); }
    }
    Console.writeLine("end of main");
}
```

```text
saved
nothing to save
end of main
discarded without saving
```

`save` borrows the document exclusively, because it sets `saved`. A method that takes `this` by value would end the
document in the call, but it cannot change `this`, so it could not tell the `drop` block that the work was done.

## `using`: a value that stays for its block

`using x = e;` declares a local that cannot be moved. It is destroyed at the end of its block, like any local, and
the word tells the reader that the block is exactly how long the value lasts: a lock that is held, a file that is
open, an arena.

<span class="listing-caption"><strong>Listing 15-12</strong>, <code>main.cee</code>: A <code>using</code> binding can change, but it cannot leave.</span>

```ceesafe
struct Timer {
    ticks: i32;

    drop {
        Console.write("stopped after ");
        Console.writeLine(this.ticks);
    }
}

fn main() {
    using timer = Timer { ticks: 0 };
    for (i in 0..3) {
        timer.ticks += 1;
    }
    Console.writeLine("end of main");
}
```

```text
end of main
stopped after 3
```

A `using` binding is pinned, not read-only. Its fields can be assigned and it can be borrowed exclusively. It
cannot be moved or assigned as a whole.

::: tip Rule
`using x = e;` declares a local that cannot be moved or replaced, and is destroyed at the end of its block.

*Specification: `type-system.md` §14.6.*
:::

## `defer`: a statement for the end of the block

`defer s;` sets the statement `s` aside and runs it when the enclosing block ends, however it ends: at its `}`, by
`return`, by `break` or by `continue`. It is for cleanup that belongs to no struct.

<span class="listing-caption"><strong>Listing 15-13</strong>, <code>main.cee</code>: Deferred statements and destructions run together, last first, on every way out.</span>

```ceesafe
struct Noisy {
    id: i32;

    drop {
        Console.write("destroy ");
        Console.writeLine(this.id);
    }
}

fn work(stopEarly: bool) -> i32 {
    let a = Noisy { id: 1 };
    defer Console.writeLine("deferred first");
    let b = Noisy { id: 2 };
    defer {
        Console.write("deferred second, for ");
        Console.writeLine(b.id);
    }
    if (stopEarly) {
        return 1;
    }
    Console.writeLine("end of work");
    return 0;
}

fn main() {
    Console.writeLine(work(true));
    Console.writeLine(work(false));
}
```

```text
deferred second, for 2
destroy 2
deferred first
destroy 1
1
end of work
deferred second, for 2
destroy 2
deferred first
destroy 1
0
```

Deferred statements and the destruction of locals form one sequence, run in the reverse of the order they appear
in the block. So a deferred statement may use every local declared before it: `b` is still whole when the second
`defer` runs. With `return`, the value to return is computed first, then the sequence runs.

A `defer` inside a loop body runs at the end of each pass, because the body is a block.

::: tip Rule
`defer s;` runs `s` when the enclosing block ends, on every path, in reverse order with the destruction of the
block's locals. `s` may not contain `return`, `break`, `continue` or another `defer`.

*Specification: `type-system.md` §14.7.*
:::

Prefer a `drop` block when the cleanup belongs to a type: every value of the type then gets it, in every function.
Use `defer` for one function's own business.

## Mistakes

::: warning Mistake: moving a field out of a struct that has a `drop` block
The `drop` block will run on the whole value, so no part of it can be missing.
:::

<span class="listing-caption"><strong>Listing 15-14</strong>, <code>main.cee</code>: <code>name</code> is moved out of a struct whose <code>drop</code> block reads it. This does not compile.</span>

<!-- ceesafe: fail N0252 -->
```ceesafe
struct Named {
    name: String;

    drop {
        Console.writeLine(this.name);
    }
}

fn main() {
    let n = Named { name: String.from("x")! };
    let s = n.name;
    Console.writeLine(s);
}
```

```text
main.cee:11:5: error[N0252]: cannot move 'n.name' out of 'n': 'Named' has a drop block, which needs the whole value
   |
11 |     let s = n.name;
   |     ^^^^^^^^^^^^^^^
```

Borrow the field (`n.name.view()`), clone it, or give the struct a method that takes `this` by value and returns
what the caller needs.

::: warning Mistake: `?` in a `drop` block
A `drop` block has no caller to give an error to.
:::

<span class="listing-caption"><strong>Listing 15-15</strong>, <code>main.cee</code>: The <code>drop</code> block tries to pass on a failed <code>push</code>. This does not compile.</span>

<!-- ceesafe: fail N0261 -->
```ceesafe
struct Log {
    codes: Vector<i32>;

    drop {
        this.codes.push(0)?;
    }
}

fn main() {
    let log = Log { codes: Vector<i32>.create() };
}
```

```text
main.cee:5:9: error[N0261]: '?' cannot be used in a 'drop' block: it returns 'Err' from the function, and a 'drop' block returns nothing, so the error has nowhere to go; handle it here with 'match', or discard it with 'let _ = …;'
  |
5 |         this.codes.push(0)?;
  |         ^^^^^^^^^^^^^^^^^^^
```

Decide in the block what a failure means, as the message says: `match` on the result, or discard it with
`let _ = …;`. Better, do the work that can fail in a method such as `save` in Listing 15-11.

::: warning Mistake: moving a `using` binding
A `using` binding stays where it is until its block ends.
:::

<span class="listing-caption"><strong>Listing 15-16</strong>, <code>main.cee</code>: <code>timer</code> is passed by value. This does not compile.</span>

<!-- ceesafe: fail N0253 -->
```ceesafe
struct Timer {
    ticks: i32;

    drop {
        Console.writeLine("stopped");
    }
}

fn keep(t: Timer) {}

fn main() {
    using timer = Timer { ticks: 0 };
    keep(timer);
}
```

```text
main.cee:13:5: error[N0253]: cannot move out of 'timer': it is a 'using' binding, which lives until the end of its block
   |
13 |     keep(timer);
   |     ^^^^^^^^^^^^
```

Pass a borrow, or declare the value with `let` if it is meant to move.

::: warning Mistake: leaving a block from a deferred statement
A deferred statement runs while the block is already ending. It cannot choose another way out.
:::

<span class="listing-caption"><strong>Listing 15-17</strong>, <code>main.cee</code>: A deferred <code>continue</code>. This does not compile.</span>

<!-- ceesafe: fail N0254 -->
```ceesafe
fn main() {
    for (i in 0..3) {
        defer {
            if (i == 1) {
                continue;
            }
        }
        Console.writeLine(i);
    }
}
```

```text
main.cee:5:17: error[N0254]: a deferred statement cannot contain 'return', 'break', 'continue' or 'defer'
  |
5 |                 continue;
  |                 ^^^^^^^^^
```

::: details Coming from Rust
A `drop` block is `impl Drop`, and the order is Rust's: locals in reverse, then a struct's own block, then its
fields in declaration order. `defer` has no counterpart in the language. There is no `std::mem::drop(x)`: write
`let _ = move x;`. There is no `mem::forget` and no `ManuallyDrop` in safe code. A panic does not unwind, so
no `drop` block runs after one.
:::

::: details Coming from C#, Java, Go, Python or TypeScript
A `drop` block is not a finalizer: it runs at a known place, at once, on the thread that owned the value. `using`
is C#'s `using` declaration, but every local with a `drop` block is already disposed of at the end of its block,
so `using` adds only "and it cannot be moved". `defer` is Go's `defer` with one difference: it belongs to the
enclosing block, not to the function.
:::

::: details Coming from C or C++
A `drop` block is a destructor, and the order is C++'s for locals. Members are destroyed in declaration order, not
in reverse. `defer` replaces the `goto cleanup;` chain.
:::

## Summary

- A struct may have one `drop` block. Destroying a value runs it, then destroys the fields in declaration order.
- Locals are destroyed in reverse declaration order, then by-value parameters. A temporary is destroyed at the end
  of its statement. A moved value is destroyed by its last owner.
- A `drop` block cannot be called, cannot fail (`N0261`), and cannot move out of or replace `this`. A field cannot
  be moved out of a struct that has one (`N0252`).
- A panic and `Process.exit` run no `drop` block and no deferred statement.
- `using x = e;` declares a local that cannot be moved (`N0253`) and lasts for its block.
- `defer s;` runs `s` at the end of the block on every path, and `s` cannot leave the block (`N0254`).

Next: [allocators, arenas and scratch frames](./ch15-03-allocators), on where memory comes from.
