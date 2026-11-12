---
title: Shared ownership
summary: An Arc gives one value several owners and destroys it when the last one ends - clone and get, why the value is read-only, what a cycle of owners does, and when handles into a SlotMap are the better design.
assumes: [ch04-01-what-ownership-is, ch04-02-borrowing, ch06-03-optionals, ch11-02-running-tests, ch15-02-destruction, ch15-04-cells]
keywords: [Arc, clone, get, "shared ownership", "reference count", cycle, leak, Mutex, Share, SlotMap, Handle, "weak reference", Rc]
diagnostics: [N0334, N0230, N0211]
specification: ["type-system.md §38.1", "type-system.md §38.3", "type-system.md §24.1"]
---

# Shared ownership

**This section teaches** `Arc<T>`, the one type in CeeSafe whose value has several owners, what it costs, the
leak that a cycle of owners makes, and the alternative that most programs should try first: handles. It assumes
[ownership](./ch04-01-what-ownership-is), [borrowing](./ch04-02-borrowing), [optionals](./ch06-03-optionals),
[running tests](./ch11-02-running-tests), [destruction](./ch15-02-destruction) and [cells](./ch15-04-cells).

Every value has one owner, and other code borrows it. That covers most programs, because most values have an
obvious place to live: one function's local, one struct's field. A borrow then needs the owner to outlive it.

Sometimes no part of the program can promise that. A window and a printer both keep the same settings, and
either may be closed first. Threads that end at different times read one table. For these, CeeSafe has `Arc<T>`.

## `Arc<T>`

`Arc<T>.create(v)` moves `v` to the heap beside a count of owners, which starts at one. It allocates, so it
returns a `Result`. `a.clone()` adds one to the count and returns another owner of the same value. `a.get()`
lends a `borrow T`. When an owner is destroyed the count goes down by one, and the owner that brings it to zero
destroys the value and frees it.

<span class="listing-caption"><strong>Listing 15-36</strong>, <code>main.cee</code>: Three owners of one <code>Settings</code>. The value lives until the last of them ends.</span>

```ceesafe
struct Settings {
    width: u32;

    drop {
        Console.writeLine("settings destroyed");
    }
}

struct Window {
    settings: Arc<Settings>;
}

struct Printer {
    settings: Arc<Settings>;
}

fn setUp() -> Printer {
    let settings = Arc<Settings>.create(Settings { width: 80 })!;
    let window = Window { settings: settings.clone() };
    Console.writeLine(window.settings.get().width);
    return Printer { settings: settings.clone() };
}

fn main() {
    let printer = setUp();
    Console.writeLine("the first two owners are gone");
    Console.writeLine(printer.settings.get().width);
}
```

```text
80
the first two owners are gone
80
settings destroyed
```

`settings` and `window` end when `setUp` returns. The `Settings` value does not: the `Printer` that `setUp`
returns still owns it. It is destroyed once, at the end of `main`.

- `clone()` on an `Arc` copies no `Settings`. It makes a second owner. It does not allocate, so it returns the
  `Arc` itself and needs no `!`.
- An `Arc` is not read through as a `Box` is. The value is reached with `get()`.
- An `Arc` is a move type: `let b = a;` moves the one owner, and `a.clone()` makes another.

::: tip Rule
`Arc<T>` is one owner among several of a value on the heap. `clone()` makes another owner and `get()` lends a
shared borrow. The value is destroyed when its last owner is.

*Specification: `type-system.md` §38.3.*
:::

The name is short for "atomically reference counted": the count is changed with an atomic operation, so owners
may be on different threads. Chapter 16 covers threads. CeeSafe has no second, cheaper type for one thread: there
is no `Rc`.

## The value is read-only

Several owners may use the value at once, so `get()` lends a shared borrow only. Nothing lends a `borrow mut`.

A cell does not help here. A cell is for one thread and an `Arc` may be shared between threads, so the compiler
refuses an `Arc` of anything that holds a cell (the mistakes below show it). `Arc<T>` requires `T: Share`: a type
that several threads may read at once.

What may change inside an `Arc` is a type built for threads. The atomic integers, such as `AtomicU32`, change with
single operations. A `Mutex<T>` lends its `T` exclusively to one holder at a time, through a guard, and makes the
others wait where a `RefCell` would panic. Chapter 16 covers both; this listing shows only the shape.

<span class="listing-caption"><strong>Listing 15-37</strong>, <code>main.cee</code>: Two owners change one tally through its mutex.</span>

```ceesafe
struct Tally {
    count: u32;
}

fn add(tally: borrow Arc<Mutex<Tally>>) {
    var guard = tally.get().lock();
    let data = guard.data();
    data.count += 1;
}

fn main() {
    let tally = Arc<Mutex<Tally>>.create(Mutex<Tally>.create(Tally { count: 0 })!)!;
    let other = tally.clone();
    add(tally);
    add(other);
    var guard = tally.get().lock();
    Console.writeLine(guard.data().count);
}
```

```text
2
```

## Cycles leak

An `Arc` frees its value when the count reaches zero. If two values each own an `Arc` of the other, each keeps
the other's count at one, for ever. Neither `drop` block runs and the memory is never returned. This is a leak:
it is memory-safe, and it is still a bug.

The package `book/projects/ch15/friends/` makes one. A `Person` may own another `Person`, in a mutex so that it
can be set after both exist.

<span class="listing-caption"><strong>Listing 15-38</strong>, <code>src/main.cee</code>: A person owns a friend. <code>befriend</code> stores a new owner of <code>b</code> inside <code>a</code>.</span>

<!-- ceesafe: skip -->
```ceesafe
struct Person {
    name: String;
    friend: Mutex<Arc<Person>?>;

    drop {
        Console.write(this.name);
        Console.writeLine(" destroyed");
    }
}

fn person(name: StringView) -> Result<Arc<Person>, AllocError> {
    return Arc<Person>.create(Person { name: String.from(name)?, friend: Mutex<Arc<Person>?>.create(null)? });
}

fn befriend(a: borrow Arc<Person>, b: borrow Arc<Person>) {
    var guard = a.get().friend.lock();
    let friend = guard.data();
    friend = b.clone();
}
```

<span class="listing-caption"><strong>Listing 15-39</strong>, <code>src/main.cee</code>: Two tests. The second makes each person own the other.</span>

<!-- ceesafe: skip -->
```ceesafe
@test
fn oneWay() {
    let ada = person("ada")!;
    let bob = person("bob")!;
    befriend(ada, bob);
}

@test
fn bothWays() {
    let ada = person("ada")!;
    let bob = person("bob")!;
    befriend(ada, bob);
    befriend(bob, ada);
}
```

`ceesafec test` counts the allocations that are still live when a test ends, as
[section 11.2](./ch11-02-running-tests) describes, and reports the cycle:

```text
test bothWays ... leaked
test oneWay ... ok

---- bothWays: leaked ----
6 allocation(s) still live at the end

ceesafec test: 1 passed, 1 failed
```

In `oneWay`, bob is owned by the local `bob` and by ada. When the locals end, ada's count reaches zero, ada is
destroyed and lets go of bob, and bob follows. In `bothWays`, the locals end and both counts are still one.

::: tip Rule
A cycle of `Arc` owners is never freed. The compiler does not detect it. `ceesafec test` reports the test that
made it as `leaked`.
:::

Some languages break a cycle with a weak reference: an `Arc` that does not count as an owner. CeeSafe has none.
The answer here is to not build ownership in a ring. Either one side owns and the other borrows or holds a key, or
neither owns and both live in a collection. That collection is the subject of the next part.

## Handles: the alternative

A `SlotMap<T>` is a collection that owns its values and gives each a `Handle<T>` when it is inserted. A handle
is a small Copy value, like an index. It is not a borrow and not an owner. It can be stored in a struct, copied,
compared, and kept for any time.

<span class="listing-caption"><strong>Listing 15-40</strong>, <code>main.cee</code>: The same two friends, each naming the other by handle. The map owns both, and nothing leaks.</span>

```ceesafe
struct Person {
    name: String;
    friend: Handle<Person>?;

    drop {
        Console.write(this.name);
        Console.writeLine(" destroyed");
    }
}

fn person(name: StringView) -> Person => Person { name: String.from(name)!, friend: null };

fn befriend(people: borrow mut SlotMap<Person>, a: Handle<Person>, b: Handle<Person>) {
    match (people.getMut(a)) {
        Some(p) => { p.friend = b; }
        null => {}
    }
}

fn friendOf(people: borrow SlotMap<Person>, h: Handle<Person>) -> Handle<Person>? => match (people.get(h)) {
    Some(p) => p.friend,
    null => null,
};

fn describe(people: borrow SlotMap<Person>, h: Handle<Person>?) {
    match (h) {
        Some(handle) => {
            match (people.get(handle)) {
                Some(p) => { Console.writeLine(p.name); }
                null => { Console.writeLine("someone who left"); }
            }
        }
        null => { Console.writeLine("nobody"); }
    }
}

fn main() {
    var people = SlotMap<Person>.create();
    let ada = people.insert(person("ada"))!;
    let bob = people.insert(person("bob"))!;
    befriend(mut people, ada, bob);
    befriend(mut people, bob, ada);
    describe(people, friendOf(people, ada));
    let gone = people.remove(bob);
    describe(people, friendOf(people, ada));
    Console.writeLine("end of main");
}
```

```text
bob
someone who left
end of main
bob destroyed
ada destroyed
```

- `people.insert(v)` moves `v` into the map and returns its handle, in a `Result` because the map may need
  memory.
- `people.get(h)` returns an optional borrow, and `people.getMut(h)` an optional exclusive borrow. Every use of a
  handle is checked: if the value was removed, the answer is `null`. A handle cannot reach freed memory, and it
  cannot reach a newer value that took the same slot.
- `people.remove(h)` moves the value out and returns it as an optional, which `gone` owns until the end of
  `main`. Ada still holds bob's handle, and it now finds nothing.
- The friendship is a cycle of handles, and it costs nothing: handles own nothing. The map owns every person and
  destroys those it still holds when it ends.

Mutation needs no cell and no mutex either. Whoever has a `borrow mut` of the map may change any value in it, and
the compiler checks that as it checks any borrow.

::: tip Rule
A `Handle<T>` names a value in a `SlotMap<T>` without owning or borrowing it. `get(h)` and `getMut(h)` return
`null` for a handle whose value was removed.

*Specification: `type-system.md` §24.1.*
:::

## Choosing

| The value | Use |
|---|---|
| has one clear owner, and the others use it for a while | a borrow, or a struct with a region |
| is one of many that refer to each other: a graph, a tree with parent links, a scene | a `SlotMap` and handles |
| is read-only and its users end at times nobody can order | `Arc<T>` |
| is shared by threads | `Arc<T>`, with a `Mutex` or atomics if it changes (chapter 16) |

Prefer the upper rows. A borrow is free and checked at compile time. A handle costs one comparison for each
access and keeps mutation checked at compile time. An `Arc` costs an allocation, an atomic operation for each
`clone` and each owner's end, read-only contents, and the possibility of a leak.

## Mistakes

::: warning Mistake: a cell in an `Arc`
`Arc<RefCell<T>>` is the habit of another language. A cell cannot be shared between threads, and an `Arc` can.
:::

<span class="listing-caption"><strong>Listing 15-41</strong>, <code>main.cee</code>: An <code>Arc</code> of a <code>Cell</code>. This does not compile.</span>

<!-- ceesafe: fail N0334 -->
```ceesafe
fn main() {
    let hits = Arc<Cell<u32>>.create(Cell<u32>.create(0))!;
    hits.get().set(1);
}
```

```text
main.cee:2:16: error[N0334]: 'Cell<u32>' is not Share, but 'T' must be ('T: Share'; type-system.md §38.1)
  |
2 |     let hits = Arc<Cell<u32>>.create(Cell<u32>.create(0))!;
  |                ^^^^^^^^^^^^^^
```

For a counter use `Arc<AtomicU32>`. For a larger value use `Arc<Mutex<T>>`, as Listing 15-37 does. If only one
thread is involved, ask first whether a borrow of a cell, or a `SlotMap`, does the job with no `Arc`.

::: warning Mistake: reaching through an `Arc` without `get()`
A `Box` is read through. An `Arc` is not.
:::

<span class="listing-caption"><strong>Listing 15-42</strong>, <code>main.cee</code>: A field of the value is asked of the <code>Arc</code>. This does not compile.</span>

<!-- ceesafe: fail N0230 -->
```ceesafe
struct Settings {
    width: u32;
}

fn main() {
    let shared = Arc<Settings>.create(Settings { width: 80 })!;
    Console.writeLine(shared.width);
}
```

```text
main.cee:7:30: error[N0230]: 'Arc<Settings>' has no field named 'width'; it has 'inner'
  |
7 |     Console.writeLine(shared.width);
  |                              ^^^^^
```

Write `shared.get().width`. The field `inner` that the message names is the `Arc`'s own, and it is private.

::: warning Mistake: changing the value of an `Arc`
`get()` lends a shared borrow, and a field cannot be assigned through one.
:::

<span class="listing-caption"><strong>Listing 15-43</strong>, <code>main.cee</code>: The shared settings are assigned to. This does not compile.</span>

<!-- ceesafe: fail N0211 -->
```ceesafe
struct Settings {
    width: u32;
}

fn main() {
    let shared = Arc<Settings>.create(Settings { width: 80 })!;
    let settings = shared.get();
    settings.width = 100;
    Console.writeLine(settings.width);
}
```

```text
main.cee:8:5: error[N0211]: cannot assign to 'settings.width': it is behind a shared reference ('borrow'); a 'borrow mut' reference is needed to modify it
  |
8 |     settings.width = 100;
  |     ^^^^^^^^^^^^^^
```

Build the value completely before it goes into the `Arc`, or put the part that changes in a `Mutex` or an atomic.

::: details Coming from Rust
`Arc<T>` is `Arc<T>` with `T: Send + Sync` always required, and `get()` where Rust dereferences. There is no
`Rc`, no `Weak`, no `Arc::get_mut` and no `Arc::try_unwrap`. So the patterns `Rc<RefCell<T>>` and parent links
through `Weak` do not carry over: use a `SlotMap` and handles, which is what the `slotmap` and `generational-arena`
crates give Rust.
:::

::: details Coming from C#, Java, Go, Python or TypeScript
Every object reference in those languages is shared ownership, and the collector finds cycles. Translating each
reference to an `Arc` gives a slow program that leaks its cycles. Translate by asking who owns each object. The
answer is nearly always one struct or one collection, and the other references become borrows or handles.
:::

::: details Coming from C or C++
`Arc<T>` is `std::shared_ptr<const T>`, without `weak_ptr`. A `Handle<T>` is an index with a generation number:
the checked form of keeping an index into an array, and of the dangling pointer it replaces.
:::

## Summary

- `Arc<T>.create(v)` returns `Result<Arc<T>, AllocError>`. `clone()` makes another owner, `get()` lends a
  `borrow T`, and the value is destroyed with its last owner.
- The value is read-only (`N0211`) and is reached only through `get()` (`N0230`). To change it, it holds a
  `Mutex` or an atomic. It cannot hold a cell (`N0334`).
- There is no `Rc` and no weak reference. A cycle of `Arc` owners leaks, and `ceesafec test` reports it.
- A `SlotMap<T>` owns values and names them by `Handle<T>`: Copy, storable, checked at each use, and free of
  cycles of ownership.
- Prefer a borrow, then handles, then `Arc`.

Next: chapter 16, on threads, where `Arc` and `Mutex` do the work they were made for.
