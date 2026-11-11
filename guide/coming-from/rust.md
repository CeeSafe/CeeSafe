# CeeSafe for Rust programmers

CeeSafe's safety model is Rust's: single ownership, moves, shared and exclusive borrows, deterministic destruction, no
garbage collector. What differs is how much you write down, where memory comes from, and how much the language does
for you. This page assumes you know Rust well.

## At a glance

| Rust | CeeSafe | Notes |
|---|---|---|
| `let x = 1;` / `let mut x = 1;` | `let x = 1;` / `var x = 1;` | |
| shadowing (`let x = x + 1;`) | not allowed | two locals in scope never share a name |
| `&T`, `&mut T` | `borrow T`, `borrow mut T` | |
| `f(&x)`, `f(&mut x)` | `f(x)`, `f(mut x)` | a shared borrow is implicit at the call; a mutable one is marked |
| `fn f<'a>(a: &'a T, b: &T) -> &'a T` | `fn f(a: borrow T, b: borrow T) -> borrow(a) T` | the result names its sources; no lifetime parameters |
| `struct S<'a> { s: &'a str }` | `struct S<region r> { s: StringView<r>; }` | only types that store references declare regions |
| `Box<T>`, `Vec<T>`, `String`, `&str` | `Box<T>`, `Vector<T>`, `String`, `StringView` | |
| `Option<T>`: `Some(x)`, `None` | `T?`: `Some(x)`, `null` | a `T` converts to `T?` where one is expected |
| `Result<T, E>`, `?` | `Result<T, E>`, `?` | the same |
| `.unwrap()` | `!` (postfix) | `r!`, `o!` |
| `panic!` (unwinds by default) | panics always abort | no unwinding, no `catch_unwind` |
| `impl Drop for S` | `drop { … }` inside the struct | |
| `trait`, `impl Trait for T`, `T: Trait` | `interface`, `impl Interface for T`, `T: Interface` | default methods, `Self`, associated types and constants, supertraits |
| `Box<dyn Trait>`, `&dyn Trait` | `Box<dyn I>`, `borrow dyn I` | |
| `fn f<const N: usize>()`, `f::<3>()`, `f::<{ X + 1 }>()` | `fn f<const N: usize>()`, `f<3>()`, `f<Y>()` with `const Y: usize = X + 1;` | `bool`, integer and fieldless-enum values; a `const fn` is an ordinary `fn` |
| `#[derive(Clone, Copy)]` | automatic | a struct is Copy when all its fields are and it has no `drop` |
| `x as u8` (truncates) | `truncate<u8>(x)` | `as` only converts where nothing can be lost |
| `wrapping_add`, `checked_add`, `saturating_add` | `wrappingAdd`, `checkedAdd`, `saturatingAdd` | |
| `\|x\| x + 1` | `(x) => x + 1` | a closure literal borrows its captures and cannot escape |
| `Box::new(move \|x\| x + n)` | `Box.create(move (x) => x + n)!` | a `Box<dyn fn(u64) -> u64>`, which owns its captures |
| `match x { A => …, }` | `match (x) { E.A => { … } }` | variants are qualified; statement arms are blocks |
| `Some(n) if n > 3 =>`, `A \| B`, `0..=9`, `x @ P` | `Some(n) if (n > 3) =>`, `A \| B`, `0..=9`, `x @ P` | the guard is parenthesised |
| `"get" =>`, `b"cjs" =>`, `[first, rest @ ..] =>` | `"get" =>`, `"cjs" =>`, `[first, rest @ ..] =>` | a string pattern is typed by the value: a `StringView` or a `Slice<u8>` |
| `if let Some(v) = e`, `while let …`, `matches!(e, P)` | `if (e is Some(v))`, `while (e is …)`, `e is P` | one operator; it chains with `&&` |
| `std::thread::spawn(move \|\| …)` | `Thread.spawn(move () => …)!` | with `Arc`, `Mutex` and atomics |
| `Send`, `Sync` | `Send`, `Share` | |
| `println!("{}", x)` | `Console.writeLine(x)` | no macros, no format strings yet |
| `#![no_std]` | `--profile freestanding` | |
| `unsafe { }` with a `// SAFETY:` convention | `unsafe { }` with a required `/// Safety:` doc line | the compiler enforces it |
| Cargo, `Cargo.toml`, `Cargo.lock` | `ceesafec`, `ceesafe.toml`, `ceesafe.lock` | path, git and registry dependencies |
| `rustfmt`, `rustdoc` | `ceesafec fmt`, `ceesafec doc` | |

## The same program

```rust
fn longest_word(text: &str) -> Option<&str> {
    let mut best: Option<&str> = None;
    for word in text.split(' ') {
        if best.map_or(true, |b| word.len() > b.len()) {
            best = Some(word);
        }
    }
    best
}

fn main() {
    match longest_word("the quick brownish fox") {
        Some(w) => println!("{}", w),
        None => println!("empty"),
    }
}
```

```ceesafe
fn longestWord<region r>(text: StringView<r>) -> StringView<r>? {
    var best: StringView<r>? = null;
    for (word in text.split(" ")) {
        match (best) {
            Some(b) if (b.length >= word.length) => {}
            _ => { best = word; }
        }
    }
    return best;
}

fn main() {
    match (longestWord("the quick brownish fox")) {
        Some(w) => { Console.writeLine(w); }
        null => { Console.writeLine("empty"); }
    }
}
```

```text
brownish
```

The CeeSafe version loops over the `split` iterator where Rust would call `map_or` (there are no adapters), tests
with a guard, returns explicitly, and its signature says the result views the input through the region `r`. With a
single reference parameter, as here, the region could also be left out:
`fn longestWord(text: StringView) -> StringView?` means the same.

## Ownership and borrowing: what is the same

Moves, Copy types, "one writer or many readers", borrows ending at their last use, two-phase borrows for
`v.push(v.length)`, references that cannot outlive their referent, and destruction at scope end in reverse order: all
as in Rust. Error messages name the borrow, where it was made and where it is used later.

## What is different

**Less annotation.** A function that returns a reference names the parameters it may borrow from,
`-> borrow(a, b) T`, instead of declaring lifetime parameters. Types that store references declare `region`
parameters, and so do functions that pass such types through; plain functions over references never do. Regions are
erased: `Token<a>` and `Token<b>` are the same type.

**A simpler checker.** CeeSafe's borrow checker is intraprocedural and keeps one set of loans per value, where Rust
reasons about lifetimes. Borrows end at their last use, as with NLL, but some programs Rust accepts are rejected; the
usual fixes are to end a borrow earlier, copy a value out, or use an index or a slot-map handle. Reference locals are
`let`, never `var`: assigning to a reference writes through it.

**Allocators per allocation.** Every allocating type takes its allocator as a type parameter (`Box<T, A>`,
`Vector<T, A>`), and every allocation returns a `Result`: there is no infallible `push`. Arenas, pools, fixed buffers
and scratch frames are built in, and allocations from a region borrow it, so they cannot outlive it.

**Checks in every build.** Integer overflow panics in release builds too; there is no `--release` that turns it off.
Bounds and overflow checks the compiler proves unnecessary are removed, and `--explain-bounds-checks` shows which.

**Interfaces are traits without the corners.** Interfaces have default methods, `Self`, associated types and
constants, and supertraits, and `impl<T> I for Wrapper<T>` implements one for a generic type; there are no blanket
`impl<T: A> B for T` and no `impl Trait` in argument or result position. A collection of different types is an enum or
holds `Box<dyn I>`.

**Closures borrow, unless they are boxed.** A closure literal borrows what it captures and has a reference type
(`borrow fn(i32) -> bool`, `borrow mut fn(i32)`), so it cannot be returned or stored. A `move` closure owns its
captures and is boxed where it is made: `Box.create(move (x: u64) => x + n)!` is a `Box<dyn fn(u64) -> u64>`, and one
that changes its captures a `Box<dyn mut fn()>`. There is no `Fn`/`FnMut`/`FnOnce` trait or `impl Fn`: a parameter
takes `borrow fn(…)` or `borrow mut fn(…)`.

**Threads and tasks.** `Thread.spawn(move () => …)` and `Thread.both` run threads; `Arc`, `Mutex` and lib/core's
atomics share state, and `Send` and `Share` are Rust's `Send` and `Sync`. `async fn` and `await` run tasks on lib/std's
executor, on one thread or several (type-system.md §38, §40).

**No macros, no iterator chains.** `for` walks ranges, arrays, slices, vectors and iterators: lib/core's
`Iterator<T>` (`fn next(borrow mut this) -> T?`), through a bound `I: Iterator<T>` or a `dyn Iterator<T>`, and any
struct with such a `next`. The item is an interface parameter, not an associated type: `Iterator<u8>`, not
`Iterator<Item = u8>`. There are no adapters: a chain of `map`, `filter` and `collect` is the loop's body, and slices
and vectors have `sortBy`, `forEach`, `any`, `all` and `retain`.

**Through C.** CeeSafe compiles to C11, which any C toolchain or kernel build can take, including MSVC and compilers for
targets LLVM does not support. The generated C is readable and maps back to CeeSafe lines.

**Kernel checking.** Context effects (`@context(atomic)`, `@effects(sleep)`) reject sleeping under a spin lock across
calls, at compile time. Rust has no equivalent in the language.

## Things you will miss

`Rc` (CeeSafe has `Arc`, and lib/core's `Cell` and `RefCell`, whose `read()` and `write()` are Rust's `borrow()` and
`borrow_mut()`: type-system.md §41; `OnceCell` and `OnceLock` are there too, with `getOrInit` for `get_or_init`),
macros, iterator adapters, blanket impls, `impl Trait`, shadowing, the
standard library's breadth, and crates.io. Several are planned or recorded as open decisions.
