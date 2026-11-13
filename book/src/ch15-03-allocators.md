---
title: Allocators, arenas and scratch frames
summary: Where memory comes from - the allocator parameter of a Vector or a Box, SystemHeap and FailingAllocator, and the regions that free everything at once (Arena, FixedBufferAllocator, scratch frames, Pool), whose allocations borrow them so that none outlives its memory.
assumes: [ch04-02-borrowing, ch09-03-allocation-failure, ch10-03-regions, ch15-01-box, ch15-02-destruction]
keywords: [allocator, SystemHeap, FailingAllocator, "Box.createIn", Arena, FixedBufferAllocator, Scratch, ScratchFrame, Pool, PoolBox, alloc, allocArray, reset, "--sweep alloc", "--sweep-strict", region]
diagnostics: [N0241, N0264, N0245]
specification: ["type-system.md §18.1", "type-system.md §19.2", "type-system.md §19.3", "type-system.md §19.4", "type-system.md §19.5", "type-system.md §19.6", "type-system.md §23.1", "type-system.md §30", "tooling.md §10.2"]
---

# Allocators, arenas and scratch frames

**This section teaches** how a program chooses where its memory comes from: the allocator parameter of `Box`,
`Vector` and `String`, and the four kinds of memory that are released all at once. It assumes
[borrowing](./ch04-02-borrowing), [allocation failure](./ch09-03-allocation-failure), [regions](./ch10-03-regions),
[`Box`](./ch15-01-box) and [destruction](./ch15-02-destruction).

CeeSafe never allocates behind your back. Every request for memory is a call that you wrote, it names the place
the memory comes from, and it returns a `Result`. This section is about the places.

## The allocator is part of the type

`Box<T>` is short for `Box<T, SystemHeap>`, `Vector<T>` for `Vector<T, SystemHeap>`, and `String` for
`String<SystemHeap>`. The last parameter is the **allocator**. `SystemHeap` is the heap of the platform, the C
library's `malloc` and `free`. `FailingAllocator` is the same heap with a countdown, for tests.

`Box.create(v)` uses `SystemHeap`. `Box.createIn(a, v)` takes a value of the allocator's type first. The built-in
allocators have no fields, so their values are written `SystemHeap {}` and `FailingAllocator {}`.

<span class="listing-caption"><strong>Listing 15-18</strong>, <code>main.cee</code>: Boxes and a string from <code>FailingAllocator</code>, which is told to refuse after one allocation.</span>

```ceesafe
struct Point {
    x: i32;
    y: i32;
}

fn boxed(x: i32) -> Result<Box<Point, FailingAllocator>, AllocError> {
    return Box.createIn(FailingAllocator {}, Point { x: x, y: 0 });
}

fn main() {
    FailingAllocator.failAfter(1);
    for (i in 0..3) {
        match (boxed(i)) {
            Ok(p) => { Console.writeLine(p.x); }
            Err(AllocError.OutOfMemory) => { Console.writeLine("out of memory"); }
        }
    }
    FailingAllocator.reset();
    var text = String<FailingAllocator>.create();
    text.append("hi")!;
    Console.writeLine(text.view());
}
```

```text
0
out of memory
out of memory
hi
```

The allocator is in the type so that the box knows where to return its memory without storing anything: a
`Box<Point, FailingAllocator>` is still one address. It also means that `Box<Point>` and
`Box<Point, FailingAllocator>` are different types, and one is not accepted for the other.

::: tip Rule
`Box<T, A>`, `Vector<T, A>` and `String<A>` take their memory from the allocator type `A`, and return it there.
Without `A` it is `SystemHeap`. `Vector` and `String` accept `SystemHeap` and `FailingAllocator`.

*Specification: `type-system.md` §18.1, §23.1.*
:::

You can write an allocator: a struct with no fields that implements the interface `Allocator`, with a method
`allocate` and an unsafe method `deallocate`. A `Box` accepts it as its `A`. A kernel wraps its own heap this way.
It needs unsafe code, which chapter 20 covers.

## Arenas

With `SystemHeap`, each value is freed on its own when its owner ends. Often a piece of work makes many small
values that all die together: the nodes of one parse, the rows of one request. An **arena** takes memory in large
chunks, hands out pieces of them, and frees everything in one step.

<span class="listing-caption"><strong>Listing 15-19</strong>, <code>main.cee</code>: Slices and a number allocated in an arena, which is emptied and used again.</span>

```ceesafe
fn squares(arena: borrow Arena, n: usize) -> Result<SliceMut<u64>, AllocError> {
    let values = arena.allocArray(n, 0u64)?;
    for (i in 0..n) {
        values[i] = (i * i) as u64;
    }
    return Ok(values);
}

fn main() {
    using arena = Arena.create(SystemHeap {}, 4096u64);
    let first = squares(arena, 4)!;
    let total = arena.alloc(0u64)!;
    for (v in first) {
        total += v;
    }
    Console.writeLine(total);
    arena.reset();
    let second = squares(arena, 2)!;
    Console.writeLine(second[1]);
}
```

```text
14
1
```

- `Arena.create(SystemHeap {}, 4096u64)` makes an empty arena that will ask `SystemHeap` for chunks of 4,096
  bytes. It allocates nothing yet, so it cannot fail.
- `arena.allocArray(n, fill)` returns a `SliceMut<T>` of `n` copies of `fill`. `arena.alloc(v)` moves `v` into the
  arena and returns a `borrow mut T`. Both return a `Result`.
- `arena.reset()` releases everything and keeps the arena for reuse. The end of `main` destroys the arena, which
  releases everything too.

What makes this safe is that **an allocation is a borrow of the arena**. `first` and `total` are borrows, and the
rule of [borrowing](./ch04-02-borrowing#the-rule) does the rest. `reset` needs an exclusive borrow of the arena,
so it is refused while an allocation is still in use. An allocation cannot be returned from the function that
owns the arena, or outlive it in any other way. `squares` can return one because the arena is its caller's: its
result borrows from its one borrowed parameter. To keep an allocation in a struct, the struct declares a region
parameter, as [section 10.3](./ch10-03-regions) shows for views.

The arena never runs a `drop` block. So it accepts only values that need no destruction: numbers, and structs and
arrays of them. A `String` or a `Box` in an arena is refused.

::: tip Rule
`arena.alloc(v)` and `arena.allocArray(n, fill)` return borrows of the arena. The arena cannot be reset, moved or
destroyed while one is live. A value in an arena must not need destruction and must not hold a borrow.

*Specification: `type-system.md` §19.2, §19.3.*
:::

The diagnostics call an arena, and the three kinds of memory below, a **region**. It is the same word as in
"region parameter": an arena is a region that you can see.

## A fixed buffer

`FixedBufferAllocator.over(mut storage)` allocates from bytes that you provide, such as an array in a local. It
never takes memory from the heap.

<span class="listing-caption"><strong>Listing 15-20</strong>, <code>main.cee</code>: Sixty-four bytes on the stack hold four spans, and have no room for eight more.</span>

```ceesafe
struct Span {
    start: u32;
    length: u32;
}

fn main() {
    var storage = [0u8; 64];
    let buffer = FixedBufferAllocator.over(mut storage);
    let spans = buffer.allocArray(4usize, Span { start: 0, length: 0 })!;
    spans[3].length = 5;
    Console.writeLine(spans[3].length);
    match (buffer.allocArray(8usize, Span { start: 0, length: 0 })) {
        Ok(more) => { Console.writeLine(more.length); }
        Err(AllocError.OutOfMemory) => { Console.writeLine("the buffer is full"); }
    }
}
```

```text
5
the buffer is full
```

The allocator holds an exclusive borrow of `storage`, and so does each allocation. It has `alloc` and `allocArray`
as an arena has, and no `reset`: to reuse the bytes, let the allocator end and make a new one.

## Scratch frames

Scratch memory is for temporaries of one call. `Scratch.enter()` opens a **frame**, allocations come from the
frame, and the end of the frame's block releases them. The memory comes from a stack of chunks that the runtime
keeps for each thread.

<span class="listing-caption"><strong>Listing 15-21</strong>, <code>main.cee</code>: A function with a frame of its own, and a frame nested in another.</span>

```ceesafe
fn digits(n: u64) -> Result<usize, AllocError> {
    using frame = Scratch.enter();
    let buffer = frame.allocArray(20usize, 0u8)?;
    var count: usize = 0;
    var rest = n;
    while (rest > 0) {
        buffer[count] = truncate<u8>(rest % 10);
        rest /= 10;
        count += 1;
    }
    return Ok(count);
}

fn main() {
    Console.writeLine(digits(1234)!);
    using frame = Scratch.enter();
    let lengths = frame.allocArray(2usize, 0usize)!;
    {
        using inner = frame.enter();
        let work = inner.allocArray(100usize, 0u8)!;
        work[0] = 3;
        lengths[0] = work[0] as usize;
    }
    let more = frame.allocArray(2usize, 0usize)!;
    Console.writeLine(lengths[0] + more[0]);
}
```

```text
4
3
```

- A frame is bound with `using`: it lasts for its block and cannot be moved.
- `Scratch.enter()` opens a **root frame**. A thread has one root frame at a time. `digits` has finished with its
  frame before `main` opens one.
- `frame.enter()` opens a frame nested in `frame`. While `inner` is live, `frame` cannot allocate. What it
  allocated before stays usable: `lengths` is written inside the inner block.
- A function that needs scratch memory while its caller has a frame takes the frame as a parameter. With
  `borrow ScratchFrame` it can allocate in the caller's frame. With `borrow mut ScratchFrame` it can nest.

::: tip Rule
A scratch frame releases its allocations when its block ends. Frames nest, and end in the reverse of the order
they were entered in. A second root frame on one thread is a panic.

*Specification: `type-system.md` §19.5.*
:::

## Pools

A `Pool<T>` holds up to a fixed number of values of one type. `pool.take(v)` moves `v` into a free slot and returns
a `PoolBox<T>`, which behaves as a `Box` does and gives its slot back when it is destroyed.

<span class="listing-caption"><strong>Listing 15-22</strong>, <code>main.cee</code>: A pool of two slots. A slot that is given back can be taken again.</span>

```ceesafe
struct Particle {
    id: i32;
    x: i32;

    drop {
        Console.write("particle ");
        Console.write(this.id);
        Console.writeLine(" returns its slot");
    }
}

fn main() {
    using pool: Pool<Particle> = Pool.create(SystemHeap {}, 2u32);
    var a = pool.take(Particle { id: 1, x: 0 })!;
    a.x += 5;
    {
        let b = pool.take(Particle { id: 2, x: 0 })!;
        match (pool.take(Particle { id: 3, x: 0 })) {
            Ok(c) => { Console.writeLine("took a third"); }
            Err(AllocError.OutOfMemory) => { Console.writeLine("the pool is full"); }
        }
    }
    let d = pool.take(Particle { id: 4, x: 0 })!;
    Console.writeLine(a.x + d.x);
}
```

```text
particle 3 returns its slot
the pool is full
particle 2 returns its slot
5
particle 4 returns its slot
particle 1 returns its slot
```

The pool's element type comes from the declared type, `Pool<Particle>`. A full pool answers
`Err(AllocError.OutOfMemory)`, and the value that found no slot is destroyed: particle 3 says so first. Each
`PoolBox` is destroyed on its own, so a pool, unlike an arena, accepts values with `drop` blocks. A `PoolBox`
borrows its pool and cannot outlive it.

| Memory | Comes from | Released | Values that need destruction |
|---|---|---|---|
| `SystemHeap` | the platform | each value, when its owner ends | yes |
| `Arena` | chunks from an allocator | all at once: `reset()`, or the arena's end | no |
| `FixedBufferAllocator` | bytes you provide | never: the bytes are yours | no |
| scratch frame | the thread's scratch stack | all at once, when the frame ends | no |
| `Pool<T>` | slots from an allocator | each slot, when its `PoolBox` ends | yes |

## Testing every failure

[Section 9.3](./ch09-03-allocation-failure#testing-the-failure-path) introduces `ceesafec test --sweep alloc`: it
runs each passing test again once for each allocation, with that allocation failing. A sweep run may return an
error. It may not crash, or end with memory still allocated.

The package `book/projects/ch15/tree/` holds the tree of [Listing 15-3](./ch15-01-box#recursive-types) and this
test.

<span class="listing-caption"><strong>Listing 15-23</strong>, <code>src/main.cee</code>: A test that builds a tree of four nodes.</span>

<!-- ceesafe: skip -->
```ceesafe
@test
fn holdsFour() -> Result<void, AllocError> {
    var root: Box<Node>? = null;
    for (v in [5, 2, 8, 1]) {
        insert(mut root, v)?;
    }
    assert(count(root) == 4);
    return Ok();
}
```

`ceesafec test --sweep alloc` prints:

```text
test holdsFour ... ok

ceesafec test: 1 passed, 0 failed, 0 sweep failure(s) in 4 sweep run(s)
```

Four nodes are four allocations, so there are four sweep runs. In each, `insert` returns the error, the test
returns it, and the half-built tree is destroyed.

The package `book/projects/ch15/tree-stops/` differs in two lines: inside `insert`, the recursive calls end with
`!` where they should end with `?`. The same command reports those runs apart:

```text
test holdsFour ... ok

ceesafec test: 1 passed, 0 failed, 0 sweep failure(s) in 4 sweep run(s) (3 unwrapped by the test itself)
```

A run that ends in the panic of a `!` is counted as "unwrapped", not as a failure. Add `--sweep-strict` to make
each one a failure and see where it was:

```text
test holdsFour ... ok
holdsFour: allocation 2 panicked: unwrapped an Err value at src/main.cee:11:17
holdsFour: allocation 3 panicked: unwrapped an Err value at src/main.cee:13:17
holdsFour: allocation 4 panicked: unwrapped an Err value at src/main.cee:11:17

ceesafec test: 1 passed, 0 failed, 3 sweep failure(s) in 4 sweep run(s)
```

For a library that must never stop the program, sweep with `--sweep-strict`.

## Mistakes

::: warning Mistake: emptying an arena that is still in use
`reset` would free the memory under a live borrow.
:::

<span class="listing-caption"><strong>Listing 15-24</strong>, <code>main.cee</code>: <code>values</code> is read after the arena is reset. This does not compile.</span>

<!-- ceesafe: fail N0241 -->
```ceesafe
fn main() {
    using arena = Arena.create(SystemHeap {}, 4096u64);
    let values = arena.allocArray(4usize, 0u64)!;
    arena.reset();
    Console.writeLine(values[0]);
}
```

```text
main.cee:4:5: error[N0241]: cannot borrow 'arena' as mutable while 'arena' is borrowed
  |
4 |     arena.reset();
  |     ^^^^^
  = note: 'arena' is borrowed here (3:18)
  = note: the borrow is used later here (5:30)
```

Finish with the allocations before the reset, as Listing 15-19 does.

::: warning Mistake: a value that needs destruction in an arena
An arena frees memory without destroying what is in it, so a `String` there would never free its text.
:::

<span class="listing-caption"><strong>Listing 15-25</strong>, <code>main.cee</code>: A <code>String</code> is put in an arena. This does not compile.</span>

<!-- ceesafe: fail N0264 -->
```ceesafe
fn main() {
    using arena = Arena.create(SystemHeap {}, 4096u64);
    let name = arena.alloc(String.from("ada")!)!;
    Console.writeLine(name.length);
}
```

```text
main.cee:3:28: error[N0264]: a region cannot hold a 'String': it needs drop, and a region releases its memory without destroying values (R2)
  |
3 |     let name = arena.alloc(String.from("ada")!)!;
  |                            ^^^^^^^^^^^^^^^^^^^
```

A type **needs drop**, in the compiler's words, when destroying a value of it runs code: it has a `drop` block,
or a part that needs drop. Keep the bytes of the text in the arena with `allocArray`, or keep the `String` outside
it.

::: warning Mistake: returning memory of a local arena
The arena ends with the function, and its allocations with it.
:::

<span class="listing-caption"><strong>Listing 15-26</strong>, <code>main.cee</code>: The result would borrow from an arena that <code>squares</code> destroys. This does not compile.</span>

<!-- ceesafe: fail N0245 -->
```ceesafe
fn squares(n: usize) -> Result<SliceMut<u64>, AllocError> {
    using arena = Arena.create(SystemHeap {}, 4096u64);
    let values = arena.allocArray(n, 0u64)?;
    return Ok(values);
}

fn main() {
    let v = squares(4)!;
    Console.writeLine(v.length);
}
```

```text
main.cee:1:25: error[N0245]: a function without reference parameters cannot return a reference: there is nothing it could borrow from
  |
1 | fn squares(n: usize) -> Result<SliceMut<u64>, AllocError> {
  |                         ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
```

The compiler stops at the signature: a slice is a borrow, and the function has no parameter to borrow it from.
Take the arena as a parameter, as `squares` does in Listing 15-19, or return a `Vector<u64>`, which owns its
memory.

::: warning Mistake: a second root frame
`Scratch.enter()` in a function that is called while its caller has a frame open panics.
:::

<span class="listing-caption"><strong>Listing 15-27</strong>, <code>main.cee</code>: <code>helper</code> enters a root frame while <code>main</code> has one. The program panics.</span>

<!-- ceesafe: panics -->
```ceesafe
fn helper() -> Result<u8, AllocError> {
    using frame = Scratch.enter();
    let b = frame.alloc(7u8)?;
    return Ok(b);
}

fn main() {
    using frame = Scratch.enter();
    Console.writeLine(helper()!);
}
```

It prints nothing, and on standard error:

```text
ceesafe panic: a scratch frame is already active; nest with frame.enter() at main.cee:2:5
```

Give `helper` a parameter `frame: borrow mut ScratchFrame` and nest in it with `frame.enter()`.

::: details Coming from Rust
`Vector<T, A>` is `Vec<T, A>` of the unstable `allocator_api`, with `A` a type and never a stored value. An arena
is `bumpalo::Bump`: `alloc` takes a shared borrow and returns `&mut T`, and `reset` takes `&mut self`. The rule
that an arena's values need no destruction is checked by the compiler, where `Bump` leaks them.
:::

::: details Coming from C#, Java, Go, Python or TypeScript
Those languages have one heap and a collector that decides when memory comes back. Here you choose, and for the
common choice, `SystemHeap`, you write nothing. Reach for an arena or a frame when a profile shows that many small
allocations are the cost.
:::

::: details Coming from C or C++
An arena is the usual bump allocator, a scratch frame is a mark and release on a second stack, and a pool is a
free list. What is new is that a pointer into one cannot be used after the reset: it is a borrow, and the compiler
checks it.
:::

## Summary

- The allocator is the last type parameter of `Box`, `Vector` and `String`. It is `SystemHeap` when left out, and
  `Box.createIn(a, v)` chooses another.
- An `Arena` hands out memory and frees it all at once, by `reset()` or at its end. Its allocations are borrows of
  it, so it cannot be reset while one is live (`N0241`), and none can be returned past it (`N0245`).
- An arena, a fixed buffer and a scratch frame hold only values that need no destruction (`N0264`). A `Pool<T>`
  holds any `T`, in a fixed number of slots.
- `FixedBufferAllocator.over(mut storage)` allocates from bytes you provide and never from the heap.
- `Scratch.enter()` opens a root frame, one for a thread at a time, and `frame.enter()` nests.
- `ceesafec test --sweep alloc` fails each allocation of each test in turn, and `--sweep-strict` counts a panic
  from `!` as a failure.

Next: [cells](./ch15-04-cells), on changing a value through a shared borrow.
