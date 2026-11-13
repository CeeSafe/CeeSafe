---
title: Unsafe code
summary: Raw pointers *T and *mut T, the unsafe block and unsafe fn that allow reading and writing through them, the Safety comment the compiler demands, Box.intoRaw and Box.fromRaw, and how a safe function keeps unsafe code small.
assumes: [ch04-02-borrowing, ch04-03-slices-and-views, ch15-01-box]
keywords: [unsafe, "raw pointer", "*T", "*mut T", "unsafe fn", "Safety:", read, write, add, "Ptr.toBorrow", "Slice.fromRaw", "Box.intoRaw", "Box.fromRaw", "null pointer", "interpreter fault"]
diagnostics: [N0250, N0280, N0283]
specification: ["type-system.md §27", "type-system.md §34.2", "unsafe.md §1", "unsafe.md §2", "unsafe.md §5"]
---

# Unsafe code

**This section teaches** raw pointers and the `unsafe` blocks that use them: what the blocks allow, what you
promise when you write one, and what the compiler goes on checking. It assumes [Borrowing](./ch04-02-borrowing),
[Slices and views](./ch04-03-slices-and-views) and [`Box`](./ch15-01-box).

A program in safe CeeSafe cannot read freed memory, read past the end of an array, or change a value that
something else is reading. The compiler proves it. A few jobs cannot be proved that way: using an address that C
code handed over, writing an allocator, talking to a device. For those jobs CeeSafe has the **raw pointer** and
the **unsafe context**. Inside an unsafe context you may do a short list of things the compiler cannot check, and
you take over the proof.

## Raw pointers

A raw pointer is an address with a type: `*T` reads a `T`, `*mut T` reads and writes one. Unlike a borrow, it is
not tied to the value it points to. It is Copy, it may be null, it may outlive its target, and the borrow rule
does not see it.

<span class="listing-caption"><strong>Listing 20-1</strong>, <code>main.cee</code>: A pointer into an array, written through once and read through four times.</span>

```ceesafe
/// Safety: `p` points to `n` initialised `u32`s that nothing writes while this runs.
unsafe fn sum(p: *u32, n: usize) -> u64 {
    var total: u64 = 0;
    for (i in 0..n) {
        total += p.add(i).read() as u64;
    }
    return total;
}

/// Safety: `p` is the address of `values[0]`, `values` is live for the whole function and has four elements, so
/// `p.add(3)` and the four reads of `sum` stay inside it.
fn main() {
    var values = [1u32, 2, 3, 4];
    let p = borrow mut values[0] as *mut u32;
    Console.writeLine(p.isNull());
    unsafe {
        p.add(3).write(10);
    }
    let total = unsafe { sum(p, values.length) };
    Console.writeLine(total);
}
```

```text
false
16
```

- `borrow mut values[0] as *mut u32` makes a pointer from a borrow. The borrow ends at once: the pointer holds
  nothing.
- `p.add(3)` is the address three elements on. `p.write(10)` stores through a `*mut`; `p.read()` loads.
- `unsafe { … }` is a statement, and `unsafe { e }` is an expression with the value of `e`.
- `unsafe fn sum` is a function that only an unsafe context may call. Its whole body is an unsafe context.
- A `*mut u32` is accepted where a `*u32` is expected, as `sum`'s first argument is here.

**Making** a pointer is safe, because a pointer that is never used does no harm. These need no `unsafe`:

| Expression | What it gives |
|---|---|
| `borrow x as *T`, `borrow mut x as *mut T` | the address of `x` |
| `s.ptr` | the address of a slice's or view's first element |
| `p as *U`, `p as *mut U` | the same address with another type |
| `p as usize`, `n as *T` | the address as a number, and back |
| `0 as *T` | the null pointer |
| `p.isNull()`, `p == q`, `p != q` | tests |

**Using** one is allowed only in an unsafe context:

| Operation | What it does |
|---|---|
| `p.read()`, `p.write(v)` | load the `T` at `p`; store `v` there (`write` needs a `*mut T`) |
| `p.readUnaligned()`, `p.writeUnaligned(v)` | the same at an address that is not aligned for `T` |
| `p.add(n)`, `p.sub(n)`, `p.offset(n)` | the address `n` elements after or before `p` |
| `Ptr.toBorrow(p)`, `Ptr.toBorrowMut(p)` | a `borrow T` or `borrow mut T` of what `p` points to |
| `Slice.fromRaw(p, n)`, `SliceMut.fromRaw(p, n)` | a slice of the `n` elements at `p` |
| `Box.fromRaw(p)` | a box from a pointer that `Box.intoRaw` gave |
| a call of an `unsafe fn` or of a C function | whatever that function does |

::: tip Rule
An unsafe context is the inside of `unsafe { … }` or the body of an `unsafe fn`. It permits the operations of this
table and nothing else. Types, moves, borrows and regions are checked there exactly as in safe code.

*Specification: `type-system.md` §27.3, §27.4; `unsafe.md` §1.*
:::

## What you promise

Each operation has a condition that must hold when it runs. If it does not, the program has no defined
behaviour: it may crash, or go on with wrong data.

- `p.read()` needs `p` to be non-null, aligned for `T`, and to point to a live, initialised `T`.
- `p.write(v)` needs the same of the storage, and nothing may hold a borrow of it. The old value is not dropped.
- A borrow or slice made from a pointer must be used only while its target is live and nothing else changes it.
- The values safe code relies on must stay valid: a `bool` is 0 or 1, a `StringView` is UTF-8.

The compiler cannot check these, so it makes you write them down.

::: tip Rule: the `Safety:` comment
An `unsafe fn`, and every function that contains `unsafe`, has a doc comment with a line that starts `Safety:`.
For an `unsafe fn` it says what the caller must guarantee. For a safe function it says why the unsafe operations
inside are sound for every caller.

*Specification: `type-system.md` §27.4; `unsafe.md` §3.*
:::

The compiler checks that the line exists. A reviewer checks that it is true.

## A safe function around unsafe code

Unsafe code should be small, and safe code should be unable to misuse it. The way is a type whose fields only its
own functions touch, and safe methods whose signatures give the borrow checker something to check.

<span class="listing-caption"><strong>Listing 20-2</strong>, <code>main.cee</code>: The pointer and length are set by one unsafe function. Reading them is safe, because the slice borrows the struct.</span>

```ceesafe
struct Bytes {
    start: *u8;
    count: usize;

    /// Safety: the caller gives a pointer to `count` live bytes that stay live and unchanged while the
    /// `Bytes` exists.
    unsafe fn fromParts(start: *u8, count: usize) -> Bytes => Bytes { start: start, count: count };

    /// Safety: `fromParts`'s caller promised `count` live bytes at `start`. The result borrows `this`, so
    /// safe callers cannot keep the slice longer than the `Bytes`.
    fn view(borrow this) -> Slice<u8> => unsafe { Slice.fromRaw(this.start, this.count) };
}

/// Safety: `text` is a literal, which lives for the whole program, and `text.length` is its length.
fn main() {
    let text = "raw parts";
    let bytes = unsafe { Bytes.fromParts(text.ptr, text.length) };
    let view = bytes.view();
    Console.writeLine(view.length);
    Console.writeLine(view[0]);
    let none = 0 as *u8;
    Console.writeLine(none.isNull());
    Console.writeLine(none == text.ptr);
}
```

```text
9
114
true
false
```

A slice from `Slice.fromRaw` holds no borrow of anything, so on its own it could outlive its bytes. `view` returns
it from a method that takes `borrow this`, and a returned slice is tied to that parameter. Callers of `view` are
then checked against `bytes` like callers of any method. The promise that cannot be checked is made once, at
`fromParts`, which is `unsafe` because its caller is the only one who can keep it.

## A box as a raw pointer

`Box.intoRaw(box)` takes a box and returns the address of its contents as a `*mut T`. Nothing is dropped or freed:
the memory is now yours to give back. It is a safe call, since a leak is not undefined behaviour.
`Box.fromRaw(p)` makes the box again, and is unsafe: `p` must have come from `Box.intoRaw` of a box of exactly
that type, and be given back once. The box type comes from where the result goes.

<span class="listing-caption"><strong>Listing 20-3</strong>, <code>main.cee</code>: A struct that keeps a heap block as a raw pointer, and frees it by making the box again in its <code>drop</code>.</span>

```ceesafe
struct Slot {
    place: *mut u64;

    fn create(value: u64) -> Slot {
        let box = Box.create(value)!;
        return Slot { place: Box.intoRaw(box) };
    }

    /// Safety: `place` came from `Box.intoRaw` in `create`, only these functions use the field, and the block
    /// is freed only by `drop`, so it holds a live `u64` for as long as the `Slot` exists.
    fn get(borrow this) -> u64 => unsafe { this.place.read() };

    /// Safety: as for `get`. `borrow mut this` means no other use of the `Slot` is live during the write.
    fn set(borrow mut this, value: u64) {
        unsafe {
            this.place.write(value);
        }
    }

    /// Safety: `place` came from `Box.intoRaw` of a `Box<u64>`, and a `Slot` is dropped once, so the box is
    /// made again once and frees the block.
    drop {
        let box: Box<u64> = unsafe { Box.fromRaw(this.place) };
    }
}

fn main() {
    var slot = Slot.create(41);
    slot.set(slot.get() + 1);
    Console.writeLine(slot.get());
}
```

```text
42
```

`main` has no `unsafe` and no `Safety:` line. The pair is most used to hand a value to C, which keeps the pointer
and returns it later: [Calling C and being called by it](./ch20-02-calling-c).

## Mistakes

::: warning Mistake: using a pointer outside an unsafe context
Making `p` is safe. `add` and `read` are not.
:::

<span class="listing-caption"><strong>Listing 20-4</strong>, <code>main.cee</code>: A read through a pointer with no <code>unsafe</code>. This does not compile.</span>

<!-- ceesafe: fail N0280 -->
```ceesafe
fn main() {
    let values = [1u32, 2, 3, 4];
    let p = borrow values[0] as *u32;
    Console.writeLine(p.add(1).read());
}
```

```text
main.cee:4:23: error[N0280]: 'add' through a raw pointer is unsafe: use it inside 'unsafe { ... }' or an 'unsafe fn' (type-system.md §27.3)
  |
4 |     Console.writeLine(p.add(1).read());
  |                       ^^^^^^^^
```

Here the fix is not `unsafe`: `values[1]` does the same and is checked. Use a pointer only where no safe form
exists.

::: warning Mistake: `unsafe` without a `Safety:` line
The block compiles only when its function says why it is sound.
:::

<span class="listing-caption"><strong>Listing 20-5</strong>, <code>main.cee</code>: The function has an <code>unsafe</code> expression and no doc comment. This does not compile.</span>

<!-- ceesafe: fail N0283 -->
```ceesafe
fn second(values: Slice<u32>) -> u32 {
    return unsafe { values.ptr.add(1).read() };
}

fn main() {
    let values = [1u32, 2, 3, 4];
    Console.writeLine(second(values[..]));
}
```

```text
main.cee:1:4: error[N0283]: function 'second' uses 'unsafe' and needs a '/// Safety:' doc comment saying why it is sound (type-system.md §27.4)
  |
1 | fn second(values: Slice<u32>) -> u32 {
  |    ^^^^^^
```

Writing the line shows the problem with this function: nothing makes `values` have two elements. An honest line
would be "the caller passes at least two elements", and then the function must be an `unsafe fn`, or check the
length first.

::: warning Mistake: expecting `unsafe` to switch the checks off
An unsafe context adds the pointer operations. It removes no rule.
:::

<span class="listing-caption"><strong>Listing 20-6</strong>, <code>main.cee</code>: A value is used after a move, inside <code>unsafe</code>. This does not compile.</span>

<!-- ceesafe: fail N0250 -->
```ceesafe
/// Safety: the block uses no pointer at all.
fn main() {
    let name = String.from("ada")!;
    unsafe {
        let taken = name;
        Console.writeLine(taken.length);
        Console.writeLine(name.length);
    }
}
```

```text
main.cee:7:27: error[N0250]: 'name' is used here after it was moved (on at least one path)
  |
7 |         Console.writeLine(name.length);
  |                           ^^^^^^^^^^^
  = note: the value was moved here (5:9)
```

::: warning Mistake: a pointer that leaves its object
The compiler accepts any pointer arithmetic in an unsafe context. Whether the address is valid is your promise.
:::

<span class="listing-caption"><strong>Listing 20-7</strong>, <code>main.cee</code>: The array has four elements and the program reads a fifth.</span>

<!-- ceesafe: exits 70 -->
```ceesafe
/// Safety: none. The array has four elements and this reads a fifth, to show what happens.
fn main() {
    let values = [1u32, 2, 3, 4];
    let p = borrow values[0] as *u32;
    Console.writeLine(unsafe { p.add(4).read() });
}
```

Run with `ceesafec run --interp`, the program stops with exit status 70:

```text
ceesafe interp: fault: access out of bounds of local _0 (4 bytes at offset 16 of 16) at main.cee:5:23
```

The interpreter keeps a model of memory and reports a null pointer, an address outside a live object, a
misaligned access and a read of bytes never written. A compiled program has none of these checks: it reads
whatever is at the address. So run unsafe code in the interpreter, and its tests with `ceesafec test --interp`.
The interpreter does not check everything: it does not notice a write through a pointer while safe code holds a
borrow of the same value.

Two smaller things the compiler does refuse, both with `N0282`: `write` through a `*T`, which needs a `*mut T`,
and `read` through a `*void`, which has no type to read. Cast the pointer first.

::: details Coming from Rust
`*T` is `*const T` and `*mut T` is `*mut T`; `unsafe { }` and `unsafe fn` mean what they mean in Rust, and making
a pointer is safe in both. What differs:

- there is no `*p`: `p.read()` and `p.write(v)`, and `Ptr.toBorrow(p)` for `&*p`;
- the `Safety:` comment is not a convention. Without it the function does not compile;
- an `unsafe fn` body is an unsafe context, with no inner block needed;
- `p.add(n)` never has undefined behaviour by itself: the address wraps, and only using it can be wrong;
- there are no `unsafe impl` and no `unsafe` interfaces for `Send` and `Share`: only the library marks its types.
:::

::: details Coming from C
A raw pointer is a C pointer, with `p.add(n)` for `p + n` and `p.read()` for `*p`. The null pointer is written
`0 as *T`, because `null` means an empty optional. Pointers compare with `==` and `!=` only; to order two, compare
`p as usize`. Everything you may do with a pointer in C you may do here, inside `unsafe`, and the function that
does it has to say why it is right.
:::

::: details Coming from C#, Java, TypeScript or Go
C# has `unsafe` blocks and pointers with nearly this meaning. The other languages have no counterpart: their
references are always checked by the run time. Most CeeSafe programs need none of this section. It matters when
you call C or write the inside of a collection.
:::

## Summary

- `*T` and `*mut T` are raw pointers: Copy, possibly null, not seen by the borrow rule. Making, casting and
  comparing them is safe.
- Reading, writing, pointer arithmetic, `Ptr.toBorrow`, `Slice.fromRaw`, `Box.fromRaw` and calls of an `unsafe fn`
  or a C function need an unsafe context: `unsafe { … }` or the body of an `unsafe fn` (`N0280`).
- An unsafe context relaxes no other rule: moves, borrows and types are checked as before.
- Every function with `unsafe` in it carries a `/// Safety:` line (`N0283`): what the caller must guarantee, or
  why the code is sound for every caller.
- Keep unsafe code behind a safe function whose signature ties results to a parameter, so callers are checked.
- `Box.intoRaw` gives up a box as a pointer without freeing it; `Box.fromRaw` takes it back, once.
- The interpreter finds many invalid accesses (exit status 70). A compiled program does not.

Next: [Calling C and being called by it](./ch20-02-calling-c), which is what most unsafe code is for.
