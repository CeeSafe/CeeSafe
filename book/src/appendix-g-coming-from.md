---
title: "Appendix G: Coming from another language"
summary: Tables for readers of Rust, C and C++, Go, C#, Java and TypeScript, and Zig that give what you write there, what you write in CeeSafe, and the section that teaches it, with what has no counterpart.
keywords: [Rust, C, "C++", Go, "C#", Java, TypeScript, Zig, translation, counterpart, borrow, optional, Result, match]
---

# Appendix G: Coming from another language

This appendix is for a reader who knows another language and wants the CeeSafe spelling of something familiar.
Find the table for your language, look for what you would write there, and read across to what CeeSafe writes
and to the section that teaches it. After each table, two short lists name what has no counterpart, in each
direction.

A row gives the nearest spelling, not an equal one. CeeSafe has no garbage collector, no exceptions and no
implicit conversions, so the section in the last column is where the differences are taught. The notes titled
"Coming from" at the end of most sections say more, and the documentation has a longer page for each language
under [Coming from another language](https://ceesafe.github.io/CeeSafe/guide/coming-from/).

In every table, `…` stands for code that the row leaves out.

## Rust

CeeSafe's model is Rust's: one owner, moves, shared and exclusive borrows, destruction at the end of the scope.
Most rows are a change of spelling.

### Variables, functions and control flow

| Rust | CeeSafe | Section |
|---|---|---|
| `let x = 1;` | `let x = 1;` | [3.1](./ch03-01-variables) |
| `let mut x = 1;` | `var x = 1;` | [3.1](./ch03-01-variables) |
| `fn five() -> i32 { 5 }` | `fn five() -> i32 => 5;` or `fn five() -> i32 { return 5; }`. A block's last expression is not its value. | [3.3](./ch03-03-functions) |
| `fn fail() -> !` | `fn fail() -> never` | [3.3](./ch03-03-functions) |
| `if x > 0 { … }` | `if (x > 0) { … }`. The condition is in parentheses. | [3.5](./ch03-05-control-flow) |
| `let y = if a > b { a } else { b };` | `let y = if (a > b) a else b;` | [3.5](./ch03-05-control-flow) |
| `for i in 0..n { … }` | `for (i in 0..n) { … }` | [3.5](./ch03-05-control-flow) |
| `x as u8`, which truncates | `truncate<u8>(x)`. `as` converts only where nothing can be lost. | [20.4](./ch20-04-arithmetic) |
| `u8::try_from(x)` | `checkedTruncate<u8>(x)`, an optional | [20.4](./ch20-04-arithmetic) |
| `x.wrapping_add(y)`, `x.checked_add(y)`, `x.saturating_add(y)` | `x.wrappingAdd(y)`, `x.checkedAdd(y)`, `x.saturatingAdd(y)` | [20.4](./ch20-04-arithmetic) |
| `!x` on an integer | `x ^ 0xFFFFFFFF` for a `u32`. There is no complement operator. | [20.4](./ch20-04-arithmetic) |
| `println!("{}", x);` | `Console.writeLine(x);`. There are no format strings. | [1.2](./ch01-02-hello-world) |
| `eprintln!("{}", text);` | `Console.writeErrorLine(text);` | [12.6](./ch12-06-standard-error) |
| `std::process::exit(1)` | `Process.exit(1)` | [12.6](./ch12-06-standard-error) |

### Ownership, borrows and regions

| Rust | CeeSafe | Section |
|---|---|---|
| A reference: `&T`, `&mut T` | A borrow: `borrow T`, a shared borrow, and `borrow mut T`, an exclusive borrow | [4.2](./ch04-02-borrowing) |
| `f(&x)` | `f(x)`. A shared borrow is implicit at the call. | [4.2](./ch04-02-borrowing) |
| `f(&mut x)` | `f(mut x)` | [4.2](./ch04-02-borrowing) |
| `g(r)` for `r: &mut T`, a reborrow | `g(r)`. Passing an exclusive borrow on reborrows it, and `r` is usable again after the call. | [4.2](./ch04-02-borrowing) |
| `*r += 1;` for `r: &mut i32` | `r += 1;`. A borrow is read and written through with no `*`. `*r += 1;` is accepted and means the same. | [4.2](./ch04-02-borrowing) |
| `&[T]`, `&mut [T]`, `&str` | `Slice<T>`, `SliceMut<T>`, `StringView` | [4.3](./ch04-03-slices-and-views) |
| `&a[1..4]` | `a[1..4]` | [4.3](./ch04-03-slices-and-views) |
| `&mut a[1..4]` as an argument | `mut a[1..4]` | [4.3](./ch04-03-slices-and-views) |
| `fn f<'a>(a: &'a T, b: &T) -> &'a T` | `fn f(a: borrow T, b: borrow T) -> borrow(a) T`. The result names the parameters it borrows from. | [10.3](./ch10-03-regions) |
| `struct S<'a> { s: &'a str }` | `struct S<region r> { s: StringView<r>; }` | [10.3](./ch10-03-regions) |
| `'static`, as in `S<'static>` | `static`, as in `S<static>`. It is written in a result's type or in a body, never in a parameter's type. | [10.3](./ch10-03-regions) |
| `fn f<'a, 'b: 'a>(…)`: `'b: 'a` | `fn f<region a, region b: a>(…)`: `region b: a`, "`b` outlives `a`". A bound is always declared: none is implied by a type. | [10.3](./ch10-03-regions) |
| `impl Drop for S { fn drop(&mut self) { … } }` | `drop { … }` in the struct's body | [15.2](./ch15-02-destruction) |
| `drop(x);` | `let _ = move x;`, which destroys the value of `x` at once | [15.2](./ch15-02-destruction) |
| `Box::new(v)` | `Box.create(v)!`. `create` returns a `Result`, because it allocates. | [15.1](./ch15-01-box) |
| `Cell<T>`, `c.get()`, `c.set(v)` | `Cell<T>`, `c.get()`, `c.set(v)` | [15.4](./ch15-04-cells) |
| `cell.borrow()`, `cell.borrow_mut()` on a `RefCell` | `cell.read()`, `cell.write()`. Each returns a guard, and `guard.data()` lends the value. | [15.4](./ch15-04-cells) |
| `Arc::new(v)`, `*a` | `Arc<T>.create(v)!`, `a.get()`. `T` must be `Share`. | [15.5](./ch15-05-shared-ownership) |
| `static X: T = …;`, `static mut X: T = …;` | The same. The type of a `static` must be `Share`, and every use of a `static mut` is in `unsafe`. | [22.2](./ch22-02-kernel-code) |
| `const fn` | `@constant` before the function, which a static's initialiser may then call | [22.2](./ch22-02-kernel-code) |

### Structs, enums, patterns and optionals

| Rust | CeeSafe | Section |
|---|---|---|
| `struct P { x: i32, y: i32 }` | `struct P { x: i32; y: i32; }`. A field ends with `;`. | [5.1](./ch05-01-defining-structs) |
| `#[derive(PartialEq)]` | `@derive(Equal)` | [5.1](./ch05-01-defining-structs) |
| `#[derive(Clone, Copy)]` | Nothing. A struct whose fields are all Copy, and that has no `drop` block, is Copy. | [5.1](./ch05-01-defining-structs) |
| `#[derive(PartialEq, Eq, Hash)]` | `@derive(Equal, HashKey)` | [20.5](./ch20-05-derives-and-macros) |
| `self`, `&self`, `&mut self` | `this`, `borrow this`, `borrow mut this` | [5.3](./ch05-03-methods) |
| `Order::create()` | `Order.create()` | [5.3](./ch05-03-methods) |
| `Shape::Circle(2)` | `Shape.Circle(2)` | [6.1](./ch06-01-defining-an-enum) |
| `match x { E::A => …, }` | `match (x) { E.A => { … } }`. A statement's arms are blocks. | [6.2](./ch06-02-match) |
| `Some(n) if n > 3 => …` | `Some(n) if (n > 3) => …` | [6.2](./ch06-02-match) |
| `Option<T>`, `Some(x)`, `None` | `T?`, `Some(x)`, `null` | [6.3](./ch06-03-optionals) |
| `o.unwrap()` | `o!` | [6.3](./ch06-03-optionals) |
| `o.unwrap_or(d)` | `o ?? d` | [6.3](./ch06-03-optionals) |
| `if let Some(v) = e { … }` | `if (e is Some(v)) { … }` | [19.1](./ch19-01-where-patterns-go) |
| `while let Some(v) = e { … }` | `while (e is Some(v)) { … }` | [19.1](./ch19-01-where-patterns-go) |
| `matches!(e, P)` | `e is P` | [19.1](./ch19-01-where-patterns-go) |
| `let Some(x) = o else { return; };` | `let Some(x) = o else { return; };` | [19.1](./ch19-01-where-patterns-go) |
| `'0'..='9'`, `A \| B`, `n @ P`, `[first, rest @ ..]` | The same. A binding that can be assigned is `var n`, not `mut n`. | [19.3](./ch19-03-the-syntax-whole) |

### Collections, text and errors

| Rust | CeeSafe | Section |
|---|---|---|
| `Vec<T>`, `Vec::new()` | `Vector<T>`, `Vector<T>.create()` | [8.1](./ch08-01-vectors) |
| `v.push(x);` | `v.push(x)!;` or `v.push(x)?;`. `push` returns a `Result`. | [9.3](./ch09-03-allocation-failure) |
| `v.len()` | `v.length`, a field | [8.1](./ch08-01-vectors) |
| `String::from("a")`, `s.clone()` | `String.from("a")!`, `s.clone()!` | [8.2](./ch08-02-strings) |
| `s.push_str("b");` | `s.append("b")!;` | [8.2](./ch08-02-strings) |
| `HashMap<K, V>`, `BTreeMap<K, V>` | `HashMap<K, V>`, `OrderedMap<K, V>` | [8.3](./ch08-03-maps) |
| `m.insert(k, v);` | `let _ = m.put(k, v)!;` | [8.3](./ch08-03-maps) |
| `panic!("message")` | `panic("message")` | [9.1](./ch09-01-panics) |
| `Result<T, E>`, `Ok(v)`, `Err(e)`, `f()?` | The same | [9.2](./ch09-02-result) |
| `r.unwrap()` | `r!` | [9.2](./ch09-02-result) |
| `impl From<ParseError> for E` | `@from Parse(ParseError),` as a variant of the enum `E` | [9.2](./ch09-02-result) |
| `assert!(c, "m")`, `debug_assert!(c, "m")` | `assert(c, "m")`, `debugAssert(c, "m")` | [14.1](./ch14-01-build-settings) |
| `include_str!("f")`, `include_bytes!("f")` | `embedText("f")`, `embed("f")` | [14.1](./ch14-01-build-settings) |

### Generics, interfaces and closures

| Rust | CeeSafe | Section |
|---|---|---|
| `f::<u8>(x)` | `f<u8>(x)` | [10.1](./ch10-01-generic-types) |
| `fn f<T: A + B>(x: T)` | `fn f<T: A + B>(x: T)`. There is no `where` clause. | [10.1](./ch10-01-generic-types) |
| `trait Shape { … }`, `impl Shape for T { … }` | `interface Shape { … }`, `impl Shape for T { … }` | [10.2](./ch10-02-interfaces) |
| `trait Shape: Named { … }` | `interface Shape: Named { … }` | [20.3](./ch20-03-more-interfaces-and-generics) |
| `impl Iterator for S { type Item = u32; … }` | `impl Iterator<u32> for S { … }`. The item is a parameter of the interface. | [13.2](./ch13-02-iteration) |
| `fn f<const N: usize>()`, `f::<3>()` | `fn f<const N: usize>()`, `f<3>()` | [20.3](./ch20-03-more-interfaces-and-generics) |
| `\|x\| x + 1` | `(x) => x + 1` | [13.1](./ch13-01-closures) |
| `fn(i32) -> i32` | `fn(i32) -> i32`, for a function or a closure that captures nothing | [13.1](./ch13-01-closures) |
| `&dyn Fn(i32) -> bool`, `&mut dyn FnMut(i32)` | `borrow fn(i32) -> bool`, `borrow mut fn(i32)` | [13.1](./ch13-01-closures) |
| `impl Fn(i32) -> bool` as a parameter | `borrow fn(i32) -> bool`. There is no type parameter for a closure. | [13.1](./ch13-01-closures) |
| `let add: Box<dyn Fn(i32) -> i32> = Box::new(move \|n\| n + x);` | `let add: Box<dyn fn(i32) -> i32> = Box.create(move (n) => n + x)!;` | [13.1](./ch13-01-closures) |
| `Box<dyn FnMut() -> u64>` | `Box<dyn mut fn() -> u64>` | [13.1](./ch13-01-closures) |
| `Box<dyn Fn() -> u64 + Send>` | `Box<dyn fn() -> u64 + Send>` | [18.1](./ch18-01-dynamic-dispatch) |
| `Box<dyn Shape>`, `&dyn Shape` | `Box<dyn Shape>`, `borrow dyn Shape` | [18.1](./ch18-01-dynamic-dispatch) |

### Threads, tasks, unsafe code and tools

| Rust | CeeSafe | Section |
|---|---|---|
| `std::thread::spawn(move \|\| …)`, `h.join()` | `Thread.spawn(move () => …)!`, `h.join()` | [16.1](./ch16-01-starting-threads) |
| `std::thread::scope(\|s\| { s.spawn(a); s.spawn(b); })` | `Thread.both(() => …, () => …)`, for two closures that may borrow | [16.1](./ch16-01-starting-threads) |
| `mpsc::sync_channel(n)` | `Channel<T>.create(n)!` | [16.2](./ch16-02-channels) |
| `Arc<Mutex<T>>`, `a.lock().unwrap()` | `Arc<Mutex<T>>`, `var guard = a.get().lock();` and then `guard.data()` | [16.3](./ch16-03-shared-state) |
| `m.try_lock()` | `m.tryLock()`, an optional | [16.3](./ch16-03-shared-state) |
| `AtomicU64::new(0)`, `n.fetch_add(1, Ordering::SeqCst)` | `AtomicU64.create(0)`, `n.fetchAdd(1)` | [16.3](./ch16-03-shared-state) |
| `OnceLock<T>` | `OnceLock<T>` | [16.3](./ch16-03-shared-state) |
| `Send`, `Sync` | `Send`, `Share`. The compiler infers both, a program cannot declare either, and no borrow is `Send`. | [16.4](./ch16-04-send-and-share) |
| `async fn f() -> T`, `f().await` | `async fn f() -> T`, `await f()` | [17.1](./ch17-01-tasks-and-await) |
| `#[tokio::main]`, `block_on(f())` | `Async.run(f())` in `fn main()` | [17.1](./ch17-01-tasks-and-await) |
| `tokio::spawn(f())` | `s.spawn(async () => …);` in an `Async.scope`, or `await set.spawn(async move () => …)?;` in `Async.tasks` | [17.2](./ch17-02-scopes) |
| `tokio::join!(a, b)` | `await Async.all(async () => …, async () => …)` | [17.3](./ch17-03-timeouts-races-cancellation) |
| `tokio::select!` over two futures | `await Async.race(async () => …, async () => …)` | [17.3](./ch17-03-timeouts-races-cancellation) |
| `tokio::time::timeout(d, f())` | `await Async.timeout(ms, error, async () => …)` | [17.3](./ch17-03-timeouts-races-cancellation) |
| `tokio::task::spawn_blocking(f)` | `await Async.blocking(() => …)` | [17.5](./ch17-05-tasks-and-threads) |
| `tokio::sync::Mutex<T>` | `AsyncMutex<T>` | [17.5](./ch17-05-tasks-and-threads) |
| `unsafe { … }`, `unsafe fn` | The same | [20.1](./ch20-01-unsafe-code) |
| `*const T`, `*mut T`, `*p` | `*T`, `*mut T`, `p.read()` | [20.1](./ch20-01-unsafe-code) |
| `&x as *const T`, `std::ptr::null()`, `p.is_null()` | `borrow x as *T`, `0 as *T`, `p.isNull()` | [20.1](./ch20-01-unsafe-code) |
| `&*p`, `&mut *p` | `Ptr.toBorrow(p)`, `Ptr.toBorrowMut(p)` | [20.1](./ch20-01-unsafe-code) |
| `std::slice::from_raw_parts(p, n)` | `Slice.fromRaw(p, n)` | [20.1](./ch20-01-unsafe-code) |
| `Box::into_raw(b)`, `Box::from_raw(p)` | `Box.intoRaw(b)`, `Box.fromRaw(p)` | [20.1](./ch20-01-unsafe-code) |
| `extern "C" { fn abs(x: i32) -> i32; }` | The same | [20.2](./ch20-02-calling-c) |
| `// SAFETY:` by convention | `/// Safety:` on the function, required by the compiler | [20.1](./ch20-01-unsafe-code) |
| `#[no_mangle] pub extern "C" fn name()` | `@export("name")` on the function | [14.3](./ch14-03-libraries-for-other-programs) |
| `#[repr(C)]` | `@repr(C)` | [20.2](./ch20-02-calling-c) |
| `#[test]` | `@test` | [11.1](./ch11-01-writing-tests) |
| `assert_eq!(a, b);` in a test | `Check.equalU64(a, b)?;` in a test that returns `Result<void, TestFailure>` | [11.1](./ch11-01-writing-tests) |
| `use tally::Tally;` | `import tally.Tally;` | [7.3](./ch07-03-imports) |
| `pub` | `public` | [7.2](./ch07-02-files-and-public) |
| `Cargo.toml`, `Cargo.lock`, `target/` | `ceesafe.toml`, `ceesafe.lock`, `out/` | [1.3](./ch01-03-hello-package) |
| `cargo check`, `cargo build --release`, `cargo test`, `cargo fmt` | `ceesafec check`, `ceesafec build`, `ceesafec test`, `ceesafec fmt` | [1.3](./ch01-03-hello-package) |
| `cargo doc`, with `///` comments | `ceesafec doc`, with `///` comments | [3.4](./ch03-04-comments) |
| `#![no_std]` | `--profile freestanding` | [22.1](./ch22-01-the-freestanding-profile) |

### No counterpart

In Rust and not in CeeSafe:

- shadowing (`let x = x + 1;` is N0203), tuples, type aliases and a `char` type;
- macros, `format!` and derives for `Clone`, `Debug` and `Default`;
- iterator adapters (`map`, `filter`, `collect`), `iter()` and `into_iter()`, and a range as a value;
- `Rc` and `Weak`: use borrows, or a `SlotMap` and handles ([15.5](./ch15-05-shared-ownership));
- `impl Trait`, blanket implementations over a bare `T`, `where` clauses, and operator traits such as `Add`;
- tuple and struct patterns, `ref` and `&` patterns, and open ranges;
- `FnOnce`, and a `loop` with `break value`;
- `Pin`, and a `Future` kept as a value: a task is awaited, run or spawned where its call is written
  ([17.1](./ch17-01-tasks-and-await));
- `unsafe impl Send` and `unsafe impl Sync`: the compiler infers `Send` and `Share`
  ([16.4](./ch16-04-send-and-share)), and `std::mem::transmute`;
- unwinding and `catch_unwind`: a panic always ends the process;
- `mod`, `use crate::…`, `pub(crate)`, workspaces, features and build scripts.

In CeeSafe and not in Rust:

- every allocation returns a `Result`, and the allocator is part of the type in ordinary use
  ([9.3](./ch09-03-allocation-failure), [15.3](./ch15-03-allocators));
- `defer` and `using` ([15.2](./ch15-02-destruction));
- integer overflow and shifts that are checked in every build ([20.4](./ch20-04-arithmetic));
- tasks that always have an owner that waits for them ([17.2](./ch17-02-scopes));
- `@context` and `@effects`, which the compiler checks across calls ([22.2](./ch22-02-kernel-code)).

## C and C++

CeeSafe compiles to C and calls C. What it adds is a compiler that tracks who owns each value and who is
looking at it. What it removes is undefined behaviour.

### Variables, types and control flow

| C or C++ | CeeSafe | Section |
|---|---|---|
| `const int32_t x = 1;` | `let x: i32 = 1;` | [3.1](./ch03-01-variables) |
| `int32_t x = 1;` | `var x: i32 = 1;` | [3.1](./ch03-01-variables) |
| `#define LIMIT 10` | `const LIMIT: i32 = 10;` | [3.1](./ch03-01-variables) |
| `uint8_t`, `int64_t`, `size_t`, `double` | `u8`, `i64`, `usize`, `f64` | [3.2](./ch03-02-data-types) |
| `int a[4] = {1, 2, 3, 4};` | `let a: [i32; 4] = [1, 2, 3, 4];` | [3.2](./ch03-02-data-types) |
| `(int64_t)x`, widening | `x as i64` | [3.2](./ch03-02-data-types) |
| `(uint8_t)x`, narrowing | `truncate<u8>(x)`, or `checkedTruncate<u8>(x)`, or `saturatingTruncate<u8>(x)` | [20.4](./ch20-04-arithmetic) |
| `~x` | `x ^ 0xFFFFFFFF` for a `u32` | [20.4](./ch20-04-arithmetic) |
| `x++;` | `x += 1;` | [3.1](./ch03-01-variables) |
| `int f(int a) { return a; }` | `fn f(a: i32) -> i32 { return a; }` | [3.3](./ch03-03-functions) |
| `c ? a : b` | `if (c) a else b` | [3.5](./ch03-05-control-flow) |
| `for (int i = 0; i < n; i++) { … }` | `for (i in 0..n) { … }` | [3.5](./ch03-05-control-flow) |
| `while (1) { … }` | `loop { … }` | [3.5](./ch03-05-control-flow) |
| `switch (x) { case 1: …; break; default: …; }` | `match (x) { 1 => { … } _ => { … } }`. One arm runs, with no `break`. | [6.2](./ch06-02-match) |
| `printf("%d\n", x);` | `Console.writeLine(x);` | [1.2](./ch01-02-hello-world) |
| `fputs(text, stderr);` | `Console.writeError(text);`, or `Console.writeErrorLine(text);` to end the line | [12.6](./ch12-06-standard-error) |
| `argc`, `argv[i]` | `Process.argumentCount()`, `Process.argument(i)`, an optional | [12.1](./ch12-01-arguments) |
| `getenv("NAME")` | `Environment.variable("NAME")`, an optional | [12.5](./ch12-05-environment) |
| `exit(1);` | `Process.exit(1);` | [12.6](./ch12-06-standard-error) |
| `assert(c);` | `debugAssert(c, "message");`. `assert(c, "message");` is checked in every build. | [14.1](./ch14-01-build-settings) |
| `abort();` | `panic("message");` | [9.1](./ch09-01-panics) |

### Pointers, memory and structs

| C or C++ | CeeSafe | Section |
|---|---|---|
| `const T *p` as a parameter | `p: borrow T`. A borrow is never null and never dangles. | [4.2](./ch04-02-borrowing) |
| `T *p` as a parameter that is written through | `p: borrow mut T`, and `f(mut x)` at the call | [4.2](./ch04-02-borrowing) |
| `p->x` | `p.x`, for a borrow `p` | [4.2](./ch04-02-borrowing) |
| A pointer and a length | `Slice<T>`, or `SliceMut<T>` to write through | [4.3](./ch04-03-slices-and-views) |
| `const char *` and `strlen(s)` | `StringView` and `s.length` | [8.2](./ch08-02-strings) |
| `malloc`, `free` | `Box.create(v)!`. Its owner frees it. | [15.1](./ch15-01-box) |
| `realloc` to grow an array | `Vector<T>`, and `v.push(x)?` | [8.1](./ch08-01-vectors) |
| `NULL` for "no value" | `null`, in a `T?` only | [6.3](./ch06-03-optionals) |
| `struct P { int x; int y; };` | `struct P { x: i32; y: i32; }` | [5.1](./ch05-01-defining-structs) |
| `struct P p = {1, 2};` | `let p = P { x: 1, y: 2 };` | [5.1](./ch05-01-defining-structs) |
| A `union` with a tag | An `enum` whose variants carry data | [6.1](./ch06-01-defining-an-enum) |
| `int (*f)(int)` | `f: fn(i32) -> i32` | [13.1](./ch13-01-closures) |
| A function pointer and a `void *` context | A closure: `borrow fn(i32) -> i32` | [13.1](./ch13-01-closures) |
| A struct of function pointers | `interface`, and `dyn I` for a value | [18.1](./ch18-01-dynamic-dispatch) |
| `goto cleanup;` | A `drop` block, or `defer` | [15.2](./ch15-02-destruction) |
| `static int helper(void)` in a `.c` file | `private fn helper() -> i32` | [7.2](./ch07-02-files-and-public) |
| `#include "tally.h"` for a library | `import tally;`, for a dependency named in `ceesafe.toml` | [7.3](./ch07-03-imports) |
| `#if NAME` … `#else` … `#endif` | `if (cfg("NAME")) { … } else { … }`. Both blocks are type-checked. | [14.1](./ch14-01-build-settings) |
| A raw `T *` in low-level code | `*T` or `*mut T`, with `p.read()`, `p.write(v)` and `p.add(n)` in `unsafe` | [20.1](./ch20-01-unsafe-code) |
| `&x`, to get a `T *` | `borrow x as *T`, or `borrow mut x as *mut T` | [20.1](./ch20-01-unsafe-code) |
| `p[i]` on a raw pointer | `p.add(i).read()` in `unsafe` | [20.1](./ch20-01-unsafe-code) |
| `(T *)0`, `(uintptr_t)p` | `0 as *T`, `p as usize` | [20.1](./ch20-01-unsafe-code) |
| `void *` | `*void`, or `*mut void` | [20.2](./ch20-02-calling-c) |
| A struct that C code also uses | `@repr(C)` before the struct | [20.2](./ch20-02-calling-c) |
| A function passed to C as a function pointer | `@callback` before the function, and the type `extern "C" fn(…)` | [20.2](./ch20-02-calling-c) |
| `static int count;` at file level | `static mut COUNT: i32 = 0;`, used in `unsafe`, or an atomic in a `static` | [22.2](./ch22-02-kernel-code) |
| A prototype for a C function | A declaration in `extern "C" { … }` | [20.2](./ch20-02-calling-c) |
| `"text"` passed to C | `c"text"`, a `CStr`, and `.ptr()` | [20.2](./ch20-02-calling-c) |
| `volatile uint32_t status;` | `status: Volatile<u32>;` | [22.2](./ch22-02-kernel-code) |
| `pthread_create`, `pthread_join` | `Thread.spawn(move () => …)!`, `h.join()` | [16.1](./ch16-01-starting-threads) |
| `pthread_mutex_t` beside the data | `Mutex<T>` around the data | [16.3](./ch16-03-shared-state) |
| `_Atomic uint64_t` | `AtomicU64` | [16.3](./ch16-03-shared-state) |
| `-ffreestanding` | `--profile freestanding` | [22.1](./ch22-01-the-freestanding-profile) |

### C++

| C++ | CeeSafe | Section |
|---|---|---|
| `std::unique_ptr<T>` | `Box<T>` | [15.1](./ch15-01-box) |
| `std::shared_ptr<const T>` | `Arc<T>` | [15.5](./ch15-05-shared-ownership) |
| `std::vector<T>`, `v.push_back(x);` | `Vector<T>`, `v.push(x)!;` | [8.1](./ch08-01-vectors) |
| `std::string`, `std::string_view` | `String`, `StringView` | [8.2](./ch08-02-strings) |
| `std::unordered_map<K, V>`, `std::map<K, V>` | `HashMap<K, V>`, `OrderedMap<K, V>` | [8.3](./ch08-03-maps) |
| `std::optional<T>`, `std::nullopt` | `T?`, `null` | [6.3](./ch06-03-optionals) |
| A reference: `const T &`, `T &` | A borrow: `borrow T`, a shared borrow, and `borrow mut T`, an exclusive borrow | [4.2](./ch04-02-borrowing) |
| `auto y = std::move(x);` | `let y = x;`. Assignment moves, and using `x` afterwards is a compile error. | [4.1](./ch04-01-what-ownership-is) |
| `~S() { … }` | `drop { … }` in the struct's body | [15.2](./ch15-02-destruction) |
| `template <typename T> T id(T x)` | `fn id<T>(x: T) -> T`, checked once against its bounds | [10.1](./ch10-01-generic-types) |
| A class with `virtual` functions | `interface`, `impl I for T`, and `dyn I` | [18.1](./ch18-01-dynamic-dispatch) |
| `[&](int x) { return x + 1; }` | `(x) => x + 1` | [13.1](./ch13-01-closures) |
| `[=](int x) { return x + n; }` kept in a `std::function<int(int)>` | `Box.create(move (x) => x + n)!` where a `Box<dyn fn(i32) -> i32>` is expected | [13.1](./ch13-01-closures) |
| `std::shared_ptr<T>` that several threads change | `Arc<Mutex<T>>` | [16.3](./ch16-03-shared-state) |
| `std::call_once`, a local `static` built on first use | `OnceLock<T>` | [16.3](./ch16-03-shared-state) |
| A `constexpr` function | `@constant` before the function, which a static's initialiser may then call | [22.2](./ch22-02-kernel-code) |
| `throw`, `try`, `catch` | `Result<T, E>`, `Err(e)`, `?` and `match` | [9.2](./ch09-02-result) |
| A `mutable` member | A `Cell<T>` field | [15.4](./ch15-04-cells) |
| `std::lock_guard<std::mutex> g(m);` | `var guard = m.lock();` on a `Mutex<T>` | [16.3](./ch16-03-shared-state) |
| `extern "C" int name() { … }` | `@export("name")` on the function | [14.3](./ch14-03-libraries-for-other-programs) |

### No counterpart

In C or C++ and not in CeeSafe:

- the preprocessor: `#define` with arguments, `#include` between files of a package, and headers;
- implicit conversions and promotions: an `i32` plus an `i64` is an error;
- `++` and `--`, the three-part `for`, `?:`, `goto`, and fall-through in a `switch`;
- pointer arithmetic and casts between pointer types in safe code;
- unions and bit fields in CeeSafe's own types, and inline assembly;
- variadic functions, except to declare a C one in an `extern` block;
- in C++, classes with inheritance, overloads, operator overloading, default arguments and exceptions;
- in C++, `std::weak_ptr` (use a `SlotMap` and handles, [15.5](./ch15-05-shared-ownership)), and a `std::future`
  or a coroutine kept as a value.

In CeeSafe and not in C or C++:

- ownership and borrowing checked by the compiler ([4.1](./ch04-01-what-ownership-is),
  [4.2](./ch04-02-borrowing));
- optionals and `match`, which must cover every case ([6.2](./ch06-02-match), [6.3](./ch06-03-optionals));
- `?`, which returns an error to the caller ([9.2](./ch09-02-result));
- checks for overflow, bounds and shifts in every build ([9.1](./ch09-01-panics));
- packages with a manifest and a lock file ([7.1](./ch07-01-package-and-manifest)), and tests in the compiler
  ([11.1](./ch11-01-writing-tests)).

## Go

Go and CeeSafe both have errors as values, `defer`, one formatter and a small language. Go has a garbage
collector and a run time, and CeeSafe has neither.

| Go | CeeSafe | Section |
|---|---|---|
| `x := 1` | `let x = 1;`, or `var x = 1;` if it is assigned again | [3.1](./ch03-01-variables) |
| `var x int32` | `var x: i32 = 0;`. There are no zero values. | [3.1](./ch03-01-variables) |
| `const Limit = 10` | `const LIMIT: i32 = 10;` | [3.1](./ch03-01-variables) |
| `func f(a int32) int32` | `fn f(a: i32) -> i32` | [3.3](./ch03-03-functions) |
| `for i := 0; i < n; i++ { … }` | `for (i in 0..n) { … }` | [3.5](./ch03-05-control-flow) |
| `for cond { … }`, `for { … }` | `while (cond) { … }`, `loop { … }` | [3.5](./ch03-05-control-flow) |
| `for _, x := range xs { … }` | `for (x in xs) { … }` | [13.2](./ch13-02-iteration) |
| `switch x { case 1: … default: … }` | `match (x) { 1 => { … } _ => { … } }` | [6.2](./ch06-02-match) |
| `fmt.Println(x)` | `Console.writeLine(x);` | [1.2](./ch01-02-hello-world) |
| `fmt.Fprintln(os.Stderr, text)` | `Console.writeErrorLine(text);` | [12.6](./ch12-06-standard-error) |
| `os.Args[i]`, `len(os.Args)` | `Process.argument(i)`, an optional, and `Process.argumentCount()` | [12.1](./ch12-01-arguments) |
| `os.LookupEnv("NAME")` | `Environment.variable("NAME")`, an optional | [12.5](./ch12-05-environment) |
| `os.Exit(1)` | `Process.exit(1);` | [12.6](./ch12-06-standard-error) |
| `b := a` for a struct | `let b = a;`. It copies a struct whose fields are all Copy, and moves any other: `a` cannot be used afterwards. | [4.1](./ch04-01-what-ownership-is) |
| `*T` as a parameter | `borrow T`, or `borrow mut T` with `f(mut x)` at the call | [4.2](./ch04-02-borrowing) |
| `[]T` that grows, `append(s, x)` | `Vector<T>`, `v.push(x)!;` | [8.1](./ch08-01-vectors) |
| `[]T` as a parameter, `a[1:4]`, `len(a)` | `Slice<T>`, `a[1..4]`, `a.length` | [4.3](./ch04-03-slices-and-views) |
| `string` | `String`, which owns its text, and `StringView`, which borrows it | [8.2](./ch08-02-strings) |
| `map[K]V`, `m[k] = v` | `HashMap<K, V>`, `let _ = m.put(k, v)!;` | [8.3](./ch08-03-maps) |
| `v, ok := m[k]` | `m.get(k)`, an optional | [8.3](./ch08-03-maps) |
| `type P struct { X int32 }`, `P{X: 1}` | `struct P { x: i32; }`, `P { x: 1 }` | [5.1](./ch05-01-defining-structs) |
| `func (p *P) Add(n int32)` | `fn add(borrow mut this, n: i32)` in the struct's body or an `impl P` block | [5.3](./ch05-03-methods) |
| `nil` | `null`, in a `T?` only | [6.3](./ch06-03-optionals) |
| `func f() (int32, error)` | `fn f() -> Result<i32, E>` | [9.2](./ch09-02-result) |
| `x, err := f(); if err != nil { return err }` | `let x = f()?;` | [9.2](./ch09-02-result) |
| `panic("message")` | `panic("message");`. There is no `recover`. | [9.1](./ch09-01-panics) |
| `defer f()` | `defer f();`. It runs at the end of the block, not of the function. | [15.2](./ch15-02-destruction) |
| `type Shape interface { Area() int64 }` | `interface Shape { fn area(borrow this) -> i64; }`, and `impl Shape for T { … }` for each type | [10.2](./ch10-02-interfaces) |
| `func f[T any](x T) T` | `fn f<T>(x: T) -> T` | [10.1](./ch10-01-generic-types) |
| A value of an interface type | `borrow dyn Shape`, or `Box<dyn Shape>` | [18.1](./ch18-01-dynamic-dispatch) |
| `func(x int32) int32 { return x + 1 }` | `(x) => x + 1`. It borrows what it captures and cannot be returned. | [13.1](./ch13-01-closures) |
| A `func` value that is returned or kept in a field | `Box<dyn fn(i32) -> i32>`, made with `Box.create(move (x) => …)!` | [13.1](./ch13-01-closures) |
| `go f()` with a `sync.WaitGroup` | `s.spawn(async () => await f());` in an `Async.scope` | [17.2](./ch17-02-scopes) |
| An `errgroup.Group` | `Async.scope`: the first child that fails cancels the others | [17.2](./ch17-02-scopes) |
| `go f()` on a thread of its own | `Thread.spawn(move () => …)!` | [16.1](./ch16-01-starting-threads) |
| `make(chan T, n)` | `Channel<T>.create(n)!`, with `n` at least 1 | [16.2](./ch16-02-channels) |
| `ch <- v` | `ch.send(v)`, a `Result` | [16.2](./ch16-02-channels) |
| `v, ok := <-ch` | `ch.recv()`, an optional | [16.2](./ch16-02-channels) |
| `close(ch)` | `ch.close();` | [16.2](./ch16-02-channels) |
| `ch <- v`, `<-ch` in a task | `await ch.sendAsync(v)`, `await ch.recvAsync()` | [17.4](./ch17-04-async-io) |
| `sync.Mutex` beside the data | `Mutex<T>` around the data | [16.3](./ch16-03-shared-state) |
| `m.TryLock()` | `m.tryLock()`, an optional | [16.3](./ch16-03-shared-state) |
| `atomic.AddUint64(&n, 1)` | `n.fetchAdd(1)` on an `AtomicU64` | [16.3](./ch16-03-shared-state) |
| `sync.Once` | `OnceLock<T>` | [16.3](./ch16-03-shared-state) |
| `time.Sleep(d)` in a goroutine | `await Timer.sleep(ms)` in a task | [17.1](./ch17-01-tasks-and-await) |
| `context.WithTimeout` around a call | `Async.timeout(ms, error, async () => …)` | [17.3](./ch17-03-timeouts-races-cancellation) |
| `ctx.Err() != nil` | `Async.isCancelled()`, or `Async.checkpoint(error)?` to return | [17.3](./ch17-03-timeouts-races-cancellation) |
| `unsafe.Pointer` | `*void`, or `*mut void` | [20.2](./ch20-02-calling-c) |
| `C.f()` with cgo | A declaration in `extern "C" { … }`, and a call in `unsafe` | [20.2](./ch20-02-calling-c) |
| A name with a capital first letter | `public` | [7.2](./ch07-02-files-and-public) |
| `go.mod`, `go.sum` | `ceesafe.toml`, `ceesafe.lock` | [7.4](./ch07-04-dependencies) |
| `go mod download`, `go build`, `go test`, `gofmt` | `ceesafec fetch`, `ceesafec build`, `ceesafec test`, `ceesafec fmt` | [1.3](./ch01-03-hello-package) |

### No counterpart

In Go and not in CeeSafe:

- the garbage collector: a value has one owner, and others borrow it;
- a goroutine that outlives its caller: every task and thread has an owner that waits for it;
- `select` over several channels (`Async.race` takes two tasks), and a channel of capacity 0;
- an interface that a type meets by having the methods, with no `impl`;
- type switches and type assertions on an interface value;
- struct embedding, several results from one function, zero values, and `recover`.

In CeeSafe and not in Go:

- ownership and borrowing ([4.1](./ch04-01-what-ownership-is), [4.2](./ch04-02-borrowing));
- `T?`, with `null` that the compiler makes you handle ([6.3](./ch06-03-optionals));
- enums whose variants carry data, and a `match` that must cover them ([6.1](./ch06-01-defining-an-enum),
  [6.2](./ch06-02-match));
- `?` in place of `if err != nil` ([9.2](./ch09-02-result));
- `drop` blocks, which run where the owner ends ([15.2](./ch15-02-destruction));
- allocation failure as a value, and a choice of allocator ([9.3](./ch09-03-allocation-failure),
  [15.3](./ch15-03-allocators));
- integer overflow that stops the program, where Go wraps ([20.4](./ch20-04-arithmetic)).

## C#, Java and TypeScript

CeeSafe's syntax looks like C# and TypeScript. Underneath there is no garbage collector, no class, no
inheritance, no exception and no null reference. A row names its language where the three differ.

### Variables, functions and types

| C#, Java or TypeScript | CeeSafe | Section |
|---|---|---|
| C# `var x = 1;`, TypeScript `let x = 1;` | `var x = 1;` | [3.1](./ch03-01-variables) |
| Java `final int x = 1;`, TypeScript `const x = 1;` | `let x = 1;` | [3.1](./ch03-01-variables) |
| C# `int F(int a) => a + 1;` | `fn f(a: i32) -> i32 => a + 1;` | [3.3](./ch03-03-functions) |
| TypeScript `function f(a: number): number` | `fn f(a: i32) -> i32` | [3.3](./ch03-03-functions) |
| `c ? a : b` | `if (c) a else b` | [3.5](./ch03-05-control-flow) |
| `for (int i = 0; i < n; i++) { … }` | `for (i in 0..n) { … }` | [3.5](./ch03-05-control-flow) |
| C# `foreach (var x in xs)`, Java `for (var x : xs)`, TypeScript `for (const x of xs)` | `for (x in xs) { … }` | [13.2](./ch13-02-iteration) |
| `switch (x) { case 1: …; break; default: …; }` | `match (x) { 1 => { … } _ => { … } }` | [6.2](./ch06-02-match) |
| C# `(byte)x`, narrowing | `truncate<u8>(x)`, or `checkedTruncate<u8>(x)` | [20.4](./ch20-04-arithmetic) |
| C# `Console.WriteLine(x);`, Java `System.out.println(x);`, TypeScript `console.log(x);` | `Console.writeLine(x);` | [1.2](./ch01-02-hello-world) |
| C# `$"total {x}"`, TypeScript `` `total ${x}` `` | `Console.write("total ");` and then `Console.writeLine(x);` | [1.2](./ch01-02-hello-world) |
| `a + b` on strings | `s.append(b)!;` on a `String` | [8.2](./ch08-02-strings) |
| `args[i]` in `main` | `Process.argument(i)`, an optional | [12.1](./ch12-01-arguments) |

### Classes, interfaces and null

| C#, Java or TypeScript | CeeSafe | Section |
|---|---|---|
| `class Point { … }` | `struct Point { … }`. A struct is a value, not a reference to an object. | [5.1](./ch05-01-defining-structs) |
| `new Point(1, 2)` | `Point { x: 1, y: 2 }` | [5.1](./ch05-01-defining-structs) |
| A constructor | A function with no `this` that returns the struct: `fn create() -> Point` | [5.3](./ch05-03-methods) |
| `b = a` for an object | `let b = a;`. It moves the value, and `a` cannot be used afterwards. A struct whose fields are all Copy is copied. | [4.1](./ch04-01-what-ownership-is) |
| A method that reads fields | `fn total(borrow this) -> u32` | [5.3](./ch05-03-methods) |
| A method that changes fields | `fn add(borrow mut this, n: u32)` | [5.3](./ch05-03-methods) |
| A `static` method | A function in the struct with no `this`, called as `Point.create()` | [5.3](./ch05-03-methods) |
| C# `class C : I`, Java `class C implements I` | `impl I for C { … }`, a block of its own | [10.2](./ch10-02-interfaces) |
| C# `interface Shape : Named`, Java `interface Shape extends Named` | `interface Shape: Named { … }` | [20.3](./ch20-03-more-interfaces-and-generics) |
| A variable of an interface type | `borrow dyn Shape`, or `Box<dyn Shape>` | [18.1](./ch18-01-dynamic-dispatch) |
| C# `where T : Shape`, Java and TypeScript `T extends Shape` | `<T: Shape>` | [10.1](./ch10-01-generic-types) |
| Overriding `Equals` and `GetHashCode`, or `equals` and `hashCode` | `@derive(Equal, HashKey)` | [20.5](./ch20-05-derives-and-macros) |
| C# `string?`, TypeScript `string \| null` | `StringView?`. A type without `?` is never null. | [6.3](./ch06-03-optionals) |
| `a ?? b` | `a ?? b` | [6.3](./ch06-03-optionals) |
| TypeScript `x!` | `x!`, which tests when the program runs and panics on `null` | [6.3](./ch06-03-optionals) |
| C# `if (o is int n)`, Java `if (o instanceof Circle c)` | An `enum`, and `if (shape is Shape.Circle(r))` | [19.1](./ch19-01-where-patterns-go) |
| C# `case P when cond:` | `P if (cond) => { … }` | [6.2](./ch06-02-match) |
| A TypeScript union with a `kind` field | An `enum` whose variants carry data | [6.1](./ch06-01-defining-an-enum) |
| `public`, `internal`, `private` | `public`, the default, `private`. `private` is to the file, not to the type. | [7.2](./ch07-02-files-and-public) |
| C# `using Tally;`, Java `import tally.*;` | `import tally;`, which names a package of the manifest | [7.3](./ch07-03-imports) |

### Collections, errors and resources

| C#, Java or TypeScript | CeeSafe | Section |
|---|---|---|
| C# `List<T>`, Java `ArrayList<T>`, TypeScript `T[]` | `Vector<T>` | [8.1](./ch08-01-vectors) |
| `list.Add(x);`, `list.add(x);`, `list.push(x);` | `v.push(x)!;` | [8.1](./ch08-01-vectors) |
| `list.Count`, `list.size()`, `list.length` | `v.length` | [8.1](./ch08-01-vectors) |
| C# `list.Any(x => x > 8)`, TypeScript `list.some(x => x > 8)` | `v.any((x) => x > 8)` | [8.1](./ch08-01-vectors) |
| `string` | `String`, which owns its text, and `StringView`, which borrows it | [8.2](./ch08-02-strings) |
| C# `Dictionary<K, V>`, Java `HashMap<K, V>`, TypeScript `Map<K, V>` | `HashMap<K, V>` | [8.3](./ch08-03-maps) |
| `d[k] = v;`, `m.put(k, v);`, `m.set(k, v);` | `let _ = m.put(k, v)!;` | [8.3](./ch08-03-maps) |
| `x => x + 1` | `(x) => x + 1`. It borrows what it captures and cannot be returned. | [13.1](./ch13-01-closures) |
| C# `Func<int, bool>`, Java `Predicate<Integer>`, as a parameter | `borrow fn(i32) -> bool` | [13.1](./ch13-01-closures) |
| A lambda that is returned or kept in a field | `Box<dyn fn(i32) -> i32>`, made with `Box.create(move (x) => …)!` | [13.1](./ch13-01-closures) |
| `throw new E();` | `return Err(E.Reason);` in a function that returns `Result<T, E>` | [9.2](./ch09-02-result) |
| `try { … } catch (E e) { … }` | `match (f()) { Ok(v) => { … } Err(e) => { … } }` | [9.2](./ch09-02-result) |
| An exception that passes through a method | `f()?` | [9.2](./ch09-02-result) |
| C# `ref` and `out` parameters | `borrow mut T`, and `f(mut x)` at the call | [4.2](./ch04-02-borrowing) |
| C# `Dispose()`, Java `close()` | `drop { … }` in the struct's body. It runs where the owner ends. | [15.2](./ch15-02-destruction) |
| C# `using var f = Open();` | `using f = open();` | [15.2](./ch15-02-destruction) |
| `try { … } finally { f(); }` | `defer f();`, which runs at the end of its block | [15.2](./ch15-02-destruction) |
| C# `Lazy<T>` | `OnceCell<T>`, or `OnceLock<T>` between threads | [15.4](./ch15-04-cells) |
| C# `unsafe { … }` and `int*` | `unsafe { … }` and `*i32`, in a function with a `/// Safety:` comment | [20.1](./ch20-01-unsafe-code) |
| C# `[DllImport]`, Java JNI | A declaration in `extern "C" { … }`, and a call in `unsafe` | [20.2](./ch20-02-calling-c) |
| C# `[Test]`, Java `@Test` | `@test` | [11.1](./ch11-01-writing-tests) |
| `Assert.AreEqual(a, b);`, `assertEquals(a, b);` | `Check.equalU64(a, b)?;` | [11.1](./ch11-01-writing-tests) |

### Threads and tasks

| C#, Java or TypeScript | CeeSafe | Section |
|---|---|---|
| C# `async Task<int> F()`, TypeScript `async function f(): Promise<number>` | `async fn f() -> i32` | [17.1](./ch17-01-tasks-and-await) |
| `await F()` | `await f()` | [17.1](./ch17-01-tasks-and-await) |
| C# `async Task Main()` | `fn main()` that calls `Async.run(f())` | [17.1](./ch17-01-tasks-and-await) |
| C# `Task.WhenAll`, TypeScript `Promise.all` | `Async.scope`, or `Async.all` | [17.2](./ch17-02-scopes) |
| C# `Task.WhenAny`, TypeScript `Promise.race` | `Async.race`, which stops the loser | [17.3](./ch17-03-timeouts-races-cancellation) |
| C# `CancellationToken` | Nothing to pass: cancellation belongs to the task | [17.3](./ch17-03-timeouts-races-cancellation) |
| C# `cts.CancelAfter(ms)` | `Async.timeout(ms, error, async () => …)` | [17.3](./ch17-03-timeouts-races-cancellation) |
| C# `token.ThrowIfCancellationRequested()` | `Async.checkpoint(error)?;` | [17.3](./ch17-03-timeouts-races-cancellation) |
| C# `await Task.Delay(ms)` | `await Timer.sleep(ms)` | [17.1](./ch17-01-tasks-and-await) |
| C# `Task.Run(() => Work())` | `await Async.blocking(() => work())` | [17.5](./ch17-05-tasks-and-threads) |
| C# `SemaphoreSlim(1, 1)` held across an `await` | `AsyncMutex<T>` | [17.5](./ch17-05-tasks-and-threads) |
| C# `Interlocked.Increment(ref n)`, Java `AtomicLong` | `n.fetchAdd(1)` on an `AtomicU64` | [16.3](./ch16-03-shared-state) |
| C# `new Thread(…).Start()`, Java `new Thread(…).start()` | `Thread.spawn(move () => …)!` | [16.1](./ch16-01-starting-threads) |
| C# `lock (o) { … }`, Java `synchronized (o) { … }` | `var guard = m.lock();` on a `Mutex<T>` that holds the data | [16.3](./ch16-03-shared-state) |
| Java `ArrayBlockingQueue<T>`, C# `BlockingCollection<T>` | `Channel<T>` | [16.2](./ch16-02-channels) |
| `.csproj`, `pom.xml`, `package.json` | `ceesafe.toml` | [7.1](./ch07-01-package-and-manifest) |
| `dotnet add package`, `npm install` | A line under `[dependencies]`, then `ceesafec fetch` | [7.4](./ch07-04-dependencies) |

### No counterpart

In C#, Java or TypeScript and not in CeeSafe:

- classes, inheritance, constructors and `new`;
- the garbage collector, and two names for one object;
- exceptions, `try`, `catch` and `finally`;
- overloads, default values for parameters and named arguments;
- properties, events, reflection, and attributes that a program declares;
- string interpolation, `+` on strings and `ToString()`;
- generators and `yield`, LINQ's query syntax, and iterator chains;
- a `Task` or a `Promise` that is stored and awaited later;
- type tests on a value (`is int`, `instanceof`) and structural typing.

In CeeSafe and not in C#, Java or TypeScript:

- ownership, moves and borrows ([4.1](./ch04-01-what-ownership-is), [4.2](./ch04-02-borrowing));
- destruction at a known place, with no collector ([15.2](./ch15-02-destruction));
- errors in the return type, and `?` at each call that can fail ([9.2](./ch09-02-result));
- allocation failure as a value ([9.3](./ch09-03-allocation-failure));
- regions, for a struct that borrows ([10.3](./ch10-03-regions));
- const parameters ([20.3](./ch20-03-more-interfaces-and-generics));
- programs with no operating system under them ([22.1](./ch22-01-the-freestanding-profile)).

## Zig

Zig and CeeSafe agree on explicit allocation, errors as values, `defer`, no garbage collector and C interop.
Zig checks bounds, overflow and null when the program runs, in its safe build modes. CeeSafe also makes use
after free and dangling pointers compile errors, and has one build mode.

| Zig | CeeSafe | Section |
|---|---|---|
| `const x = 1;` | `let x = 1;` | [3.1](./ch03-01-variables) |
| `var x: u32 = 1;` | `var x: u32 = 1;` | [3.1](./ch03-01-variables) |
| `fn f(a: i32) i32` | `fn f(a: i32) -> i32` | [3.3](./ch03-03-functions) |
| `fn fail() noreturn` | `fn fail() -> never` | [3.3](./ch03-03-functions) |
| `pub fn` | `public fn` | [7.2](./ch07-02-files-and-public) |
| `while (i < n) : (i += 1) { … }` | `for (i in 0..n) { … }` | [3.5](./ch03-05-control-flow) |
| `for (items) \|x\| { … }` | `for (x in items) { … }` | [13.2](./ch13-02-iteration) |
| `switch (x) { 1 => …, else => … }` | `match (x) { 1 => { … } _ => { … } }` | [6.2](./ch06-02-match) |
| `[4]u8` | `[u8; 4]` | [3.2](./ch03-02-data-types) |
| `[]const u8`, `[]u8` | `Slice<u8>`, `SliceMut<u8>` | [4.3](./ch04-03-slices-and-views) |
| `*const T`, `*T` as a parameter | `borrow T`, `borrow mut T` | [4.2](./ch04-02-borrowing) |
| `?T`, `null` | `T?`, `null` | [6.3](./ch06-03-optionals) |
| `o orelse d` | `o ?? d` | [6.3](./ch06-03-optionals) |
| `o.?` | `o!` | [6.3](./ch06-03-optionals) |
| `if (o) \|v\| { … }` | `if (o is Some(v)) { … }` | [6.3](./ch06-03-optionals) |
| `!T`, `error.X` | `Result<T, E>`, `Err(E.X)`. An error is an ordinary enum. | [9.2](./ch09-02-result) |
| `try f()` | `f()?` | [9.2](./ch09-02-result) |
| `f() catch unreachable` | `f()!` | [9.2](./ch09-02-result) |
| `f() catch \|err\| { … }` | `match (f()) { Ok(v) => { … } Err(e) => { … } }` | [9.2](./ch09-02-result) |
| `@panic("message")` | `panic("message")` | [9.1](./ch09-01-panics) |
| `defer f();` | `defer f();` | [15.2](./ch15-02-destruction) |
| `defer list.deinit();` | Nothing. A local is destroyed at the end of its block. | [15.2](./ch15-02-destruction) |
| `union(enum)` | An `enum` whose variants carry data | [6.1](./ch06-01-defining-an-enum) |
| `@as(u64, x)`, widening | `x as u64` | [3.2](./ch03-02-data-types) |
| `@intCast(x)` | `x as u64` where nothing can be lost. Otherwise `checkedTruncate<u8>(x)`, an optional. | [20.4](./ch20-04-arithmetic) |
| `@truncate(x)` | `truncate<u8>(x)` | [20.4](./ch20-04-arithmetic) |
| `a +% b`, `a +\| b` | `a.wrappingAdd(b)`, `a.saturatingAdd(b)` | [20.4](./ch20-04-arithmetic) |
| `@addWithOverflow(a, b)` | `a.checkedAdd(b)`, an optional | [20.4](./ch20-04-arithmetic) |
| `allocator.create(T)`, `allocator.destroy(p)` | `Box.createIn(allocator, value)`. Its owner frees it. | [15.3](./ch15-03-allocators) |
| `std.ArrayList(T)` with an allocator | `Vector<T, A>`. The allocator is a type parameter. | [15.3](./ch15-03-allocators) |
| `std.heap.ArenaAllocator` | `Arena` | [15.3](./ch15-03-allocators) |
| `std.heap.FixedBufferAllocator` | `FixedBufferAllocator` | [15.3](./ch15-03-allocators) |
| `comptime T: type` as a parameter | `<T>`, with interface bounds | [10.1](./ch10-01-generic-types) |
| `comptime n: usize` as a parameter | `<const N: usize>` | [20.3](./ch20-03-more-interfaces-and-generics) |
| `@embedFile("f")` | `embed("f")`, or `embedText("f")` for text | [14.1](./ch14-01-build-settings) |
| `std.debug.print("{}\n", .{x});` | `Console.writeLine(x);` | [1.2](./ch01-02-hello-world) |
| `test "name" { … }` | `@test` on a function | [11.1](./ch11-01-writing-tests) |
| `try std.testing.expectEqual(a, b);` | `Check.equalU64(a, b)?;` | [11.1](./ch11-01-writing-tests) |
| `extern fn strlen(s: [*:0]const u8) usize;` | `fn strlen(text: *CChar) -> usize;` in `extern "C" { … }` | [20.2](./ch20-02-calling-c) |
| `export fn name() void` | `@export("name")` on the function | [14.3](./ch14-03-libraries-for-other-programs) |
| `fn visit(x: i32) callconv(.C) void`, passed to C | `@callback` on the function | [20.2](./ch20-02-calling-c) |
| `extern struct` | `@repr(C)` before the struct | [20.2](./ch20-02-calling-c) |
| `[*]T`, `p[i]`, `p + n` | `*mut T`, `p.add(i).read()`, `p.add(n)`, in `unsafe` | [20.1](./ch20-01-unsafe-code) |
| `@intFromPtr(p)`, `@ptrFromInt(n)` | `p as usize`, `n as *T` | [20.1](./ch20-01-unsafe-code) |
| `*volatile u32` | `Volatile<u32>` | [22.2](./ch22-02-kernel-code) |
| `var count: u32 = 0;` at the top level | `static mut COUNT: u32 = 0;`, used in `unsafe`, or an atomic in a `static` | [22.2](./ch22-02-kernel-code) |
| `*const fn (i32) i32` | `fn(i32) -> i32` | [13.1](./ch13-01-closures) |
| A function with a context pointer | A closure: `borrow fn(i32) -> i32` | [13.1](./ch13-01-closures) |
| `std.Thread.spawn(.{}, f, .{})`, `t.join()` | `Thread.spawn(move () => …)!`, `t.join()` | [16.1](./ch16-01-starting-threads) |
| `std.Thread.Mutex` beside the data | `Mutex<T>` around the data | [16.3](./ch16-03-shared-state) |
| `std.atomic.Value(u64)`, `v.fetchAdd(1, .seq_cst)` | `AtomicU64`, `v.fetchAdd(1)` | [16.3](./ch16-03-shared-state) |
| `zig build`, `zig test`, `zig fmt` | `ceesafec build`, `ceesafec test`, `ceesafec fmt` | [1.3](./ch01-03-hello-package) |
| `-target x86_64-freestanding` | `--profile freestanding` | [22.1](./ch22-01-the-freestanding-profile) |

### No counterpart

In Zig and not in CeeSafe:

- `comptime`: the compiler evaluates constants, and a `@constant` function in a static's initialiser
  ([22.2](./ch22-02-kernel-code)), and no other code. There is no `@typeInfo` and no reflection;
- `@cImport` of a whole header: C functions are declared in an `extern` block
  ([20.2](./ch20-02-calling-c));
- error sets that join by themselves: `?` needs the function's error type, or an `@from` variant;
- `errdefer`: a value built before the failure is destroyed by its owner;
- build modes: there is no ReleaseFast, and checks are on in every build;
- packed structs, integers of any width such as `u3`, SIMD vectors and inline assembly;
- the build system, and a cross-compiling toolchain in one program: CeeSafe uses a C compiler.

In CeeSafe and not in Zig:

- ownership and borrowing, which make use after free a compile error
  ([4.1](./ch04-01-what-ownership-is), [4.2](./ch04-02-borrowing));
- destruction with no `deinit` call ([15.2](./ch15-02-destruction));
- interfaces as bounds that a generic is checked against once ([10.2](./ch10-02-interfaces));
- closures that capture ([13.1](./ch13-01-closures));
- guards and slice patterns in a `match` ([19.3](./ch19-03-the-syntax-whole)).
