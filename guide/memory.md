# Memory and allocation

CeeSafe has no hidden allocation. Values live on the stack unless the program asks for memory, and every request names
where the memory comes from and returns an error when there is none. This chapter goes from the simplest owner of
heap memory, `Box`, to arenas, pools and scratch frames, and ends with how the compiler moves boxes to the stack when
it can prove that is safe.

The rules are in [Memory model](../spec/memory-model.md) and the type system's sections on
[boxes](../spec/type-system.md#_18-boxes-and-allocators-m3b) and
[regions](../spec/type-system.md#_19-regions-m4).

## `Box`

`Box<T>` owns one `T` on the heap. It is one pointer, it moves (never copies), and destroying it destroys the `T`
and frees the memory. Fields and methods are reached through it as if it were the value.

```ceesafe
struct Account {
    id: u64;
    balance: i64;
    fn deposit(borrow mut this, amount: i64) {
        this.balance += amount;
    }
}

fn open(id: u64) -> Result<Box<Account>, AllocError> => Box.create(Account { id: id, balance: 0 });

fn main() {
    var account = open(7)!;
    account.deposit(100);                  // through the box
    Console.writeLine(account.balance);
}                                          // the Account is destroyed and its memory freed here
```

```text
100
```

The declared type of a box reaches the value under `!` and `?`: `let wide: Box<i64> = Box.create(5)!;` holds an
`i64`, and `let s: String<FailingAllocator> = String.from("a")!;` allocates from that allocator.

Boxes are how recursive types are built, since a type cannot contain itself directly:

```ceesafe
enum Tree {
    Leaf,
    Node(Box<Tree>, i32, Box<Tree>),
}

fn sum(t: borrow Tree) -> i32 => match (t) {
    Tree.Leaf => 0,
    Tree.Node(left, value, right) => sum(left) + value + sum(right),
};

fn leaf() -> Box<Tree> => Box.create(Tree.Leaf)!;

fn main() {
    let tree = Tree.Node(Box.create(Tree.Node(leaf(), 1, leaf()))!, 2, Box.create(Tree.Node(leaf(), 3, leaf()))!);
    Console.writeLine(sum(tree));
}
```

```text
6
```

A long list of boxes is destroyed with a loop, not recursion, so it cannot overflow the stack.

## Allocators

`Box<T>` is `Box<T, SystemHeap>`: memory from the platform's `malloc`. The allocator is part of the type, and
`Box.createIn(allocator, value)` chooses another one:

| Allocator | Gives | Frees |
|---|---|---|
| `SystemHeap` | the platform heap | each value when its owner is destroyed |
| `Arena` | chunks of one region | everything at once, when the arena is destroyed or reset |
| `FixedBufferAllocator` | a buffer you provide (an array on the stack, say) | nothing: the buffer is reused when the allocator ends |
| `Scratch` frames | a runtime stack of chunks, last in first out | everything in a frame when the frame ends |
| `Pool<T>` | slots for up to *n* values of one type | each slot when its box is destroyed |
| your own | any struct that implements `Allocator` | as you implement it |

`FailingAllocator` is a `SystemHeap` that can be told to fail after *n* allocations, for testing failure paths.
`SecretHeap` is a `SystemHeap` that wipes: see [Secrets](#secrets).

## Arenas

An arena hands out memory quickly and frees it all at once. Every allocation **borrows the arena**, so none can
outlive it: the compiler rejects any attempt to keep one after the arena is reset or destroyed.

```ceesafe
fn histogram(input: Slice<u8>) -> Result<u64, AllocError> {
    using arena = Arena.create(SystemHeap {}, 65536u64);
    let counts = arena.allocArray(256usize, 0u32)?;   // a SliceMut<u32> in the arena
    for (b in input) {
        counts[b as usize] += 1;
    }
    var distinct: u64 = 0;
    for (c in counts) {
        if (c != 0) {
            distinct += 1;
        }
    }
    return Ok(distinct);                               // a number, not arena memory
}                                                      // the arena releases everything here

fn main() {
    Console.writeLine(histogram("hello".bytes())!);
}
```

```text
4
```

Values in an arena must not need destruction (no `drop` blocks, no boxes inside), since the arena never runs their
destructors. Returning `counts` itself would be a compile error: it borrows an arena that ends with the function.

## A fixed buffer

```ceesafe
struct Span {
    start: u32;
    length: u32;
}

fn main() {
    var storage = [0u8; 1024];
    let fba = FixedBufferAllocator.over(mut storage);       // borrows `storage` exclusively
    let spans = fba.allocArray(16usize, Span { start: 0, length: 0 })!;
    spans[3].length = 5;
    Console.writeLine(spans[3].length);
    Console.writeLine(spans.length);
}
```

```text
5
16
```

When the buffer is full, an allocation returns `Err(AllocError.OutOfMemory)`; nothing is ever taken from the heap.

## Scratch frames

Scratch memory is a stack of frames: a function enters a frame, allocates temporaries from it, and they are all
released when the frame ends. Nested frames borrow their parent, so frames always end in order.

```ceesafe
fn render(n: usize) -> Result<usize, AllocError> {
    using frame = Scratch.enter();
    let buffer = frame.allocArray(n, 0u8)?;
    {
        using inner = frame.enter();
        let temporary = inner.allocArray(n * 2, 0u16)?;
        temporary[0] = 1;
    }                                                  // `inner`'s memory is released here
    return Ok(buffer.length);
}                                                      // `frame`'s memory is released here

fn main() {
    Console.writeLine(render(64)!);
}
```

```text
64
```

## Pools

A pool holds up to a fixed number of values of one type. `take` returns a box-like `PoolBox` that returns its slot
when destroyed, so pool values may have `drop` blocks.

```ceesafe
struct Particle {
    x: i32;
    v: i32;
}

fn simulate() -> Result<i32, AllocError> {
    using pool: Pool<Particle> = Pool.create(SystemHeap {}, 64u32);
    var a = pool.take(Particle { x: 0, v: 2 })?;
    let b = pool.take(Particle { x: 10, v: -1 })?;
    a.x += a.v;
    return Ok(a.x + b.x);
}

fn main() {
    Console.writeLine(simulate()!);
}
```

```text
12
```

## Your own allocator

An allocator is a struct with no fields that implements `lib/core`'s `Allocator` interface: `allocate` returns memory
for a `Layout` (size and alignment) or an `AllocError`, and `deallocate`, which is `unsafe` because it must be given
exactly what `allocate` returned, gives it back. `Box.createIn(MyAllocator {}, value)` then uses it. The CosmoOS
kernel's `KernelHeap` over `kmalloc` is written this way; see [Kernel code](kernel.md) and the
[lib/core reference](../spec/api/core.md).

## Placement: boxes on the stack

The compiler looks for boxes that provably never outlive their function, or one loop iteration, and puts them on the
stack instead. The program cannot tell the difference: destruction happens at the same points and in the same
order, and allocation failure can no longer happen for those boxes. `ceesafec build --explain-allocations` lists each
allocation and the decision; `--no-placement` turns it off. The rules are in
[Optimisations](../spec/optimisations.md).

## Secrets

A program that holds keys wants their bytes gone once it is done with them. Zeroing them with a loop is not enough:
the C compiler removes stores that nothing reads afterwards, and a value about to be dropped is never read again.
`Secret.wipe(mut place)` sets a place to zero bytes in a way the C compiler keeps, so it is how a key type's `drop`
clears what it holds:

```ceesafe
struct Key {
    bytes: [u8; 32];

    fn sum(borrow this) -> u32 {
        var total: u32 = 0;
        for (b in this.bytes) { total += b as u32; }
        return total;
    }

    drop {
        Secret.wipe(mut this.bytes);
    }
}

fn main() {
    var text: [u8; 6] = [1, 2, 3, 4, 5, 6];
    Secret.wipe(mut text[1..5]); // a slice's elements
    Console.writeLine(text[0] + text[1] + text[5]);
    let key = Key { bytes: [7u8; 32] };
    Console.writeLine(key.sum());
}
```

```text
7
224
```

The place must be **zeroable**: an integer, float, `bool`, raw pointer, an array of these, or a struct without a
`drop` whose fields are all zeroable. Zero bytes are then a value of its type, so the place can still be read and
nothing is `unsafe`. Anything else, such as a vector, an optional or a struct with a `drop`, is N0365; a struct with a
`drop` wipes its own fields.

A wipe clears the place it names, but a value that moves leaves its bytes in the place it left, and a vector that
grows frees its old buffer as it was. So keep the secret itself in a `SecretHeap` block: the third built-in
allocator, whose blocks are wiped before they are freed, whose buffers grow by copying and wiping instead of
`realloc`, and whose containers wipe the slots they give up (`pop`, `truncate`, `clear`, `remove`). A box's bytes do
not move when the box does, so a key in a `Box<[u8; 32], SecretHeap>` has one copy, in one block:

```ceesafe
struct MasterKey {
    id: u32;
    key: Box<[u8; 32], SecretHeap>;
}

fn main() {
    var keys = Vector<MasterKey>.create(); // growing moves the boxes' pointers only
    for (i in 0..3u32) {
        keys.push(MasterKey { id: i, key: Box.createIn(SecretHeap {}, [truncate<u8>(i); 32])! })!;
    }
    var text = Vector<u8, SecretHeap>.create(); // decoded key text: its growth wipes
    text.extend("a passphrase".bytes())!;
    Console.writeLine(keys[2].key[0] + text[0]);
}
```

```text
99
```

`Vector`, `String`, `SlotMap`, `StableVector`, `Arena` and `Box` all take `SecretHeap`. It is for keys, not bulk data:
each free writes the block once more. Its blocks never go to C (`Vector.intoForeign` is refused). A test can check
that nothing is left: `ceesafec test --scan-freed` keeps every freed block, and `Testing.freedHolds(key[..])` says
whether one of them holds the key.

What a wipe does not reach:

- **registers** and the C compiler's own temporaries and spills;
- **copies the program makes itself**: a local it read a key into, or the place an inline value was moved from (a
  move is a copy of the bytes);
- **swap and hibernation**, which may write pages to disk, and **core dumps**, which include live memory; a server
  that holds keys should turn core dumps off (`RLIMIT_CORE` 0, and on Linux `prctl(PR_SET_DUMPABLE, 0)`) and use
  encrypted swap;
- **`fork`**, whose child gets copies of the pages, and other processes' view of the memory (a debugger,
  `/proc/<pid>/mem`).

## Next

[Arrays, slices and collections](collections.md).
