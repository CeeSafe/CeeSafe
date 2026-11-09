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
| `trait`, `impl Trait for T`, `T: Trait` | `interface`, `impl Interface for T`, `T: Interface` | bounds only: no `dyn` |
| `#[derive(Clone, Copy)]` | automatic | a struct is Copy when all its fields are and it has no `drop` |
| `x as u8` (truncates) | `truncate<u8>(x)` | `as` only converts where nothing can be lost |
| `wrapping_add`, `checked_add`, `saturating_add` | `wrappingAdd`, `checkedAdd`, `saturatingAdd` | |
| `\|x\| x + 1` | `(x) => x + 1` | closures borrow their captures and cannot escape |
| `match x { A => …, }` | `match (x) { E.A => { … } }` | variants are qualified; statement arms are blocks |
| `println!("{}", x)` | `Console.writeLine(x)` | no macros, no format strings yet |
| `#![no_std]` | `--profile freestanding` | |
| `unsafe { }` with a `// SAFETY:` convention | `unsafe { }` with a required `/// Safety:` doc line | the compiler enforces it |
| Cargo, `Cargo.toml`, `Cargo.lock` | `ceesafec`, `ceesafe.toml`, `ceesafe.lock` | path and git dependencies only; no registry |
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
    var bestStart: usize = 0;
    var bestLength: usize = 0;
    var start: usize = 0;
    for (i in 0..text.length + 1) {
        if (i == text.length || text[i] == ' ') {
            if (i - start > bestLength) {
                bestStart = start;
                bestLength = i - start;
            }
            start = i + 1;
        }
    }
    if (bestLength == 0) {
        return null;
    }
    return text[bestStart..bestStart + bestLength];
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

The CeeSafe version has no iterator adaptors (there is no `split` yet), returns explicitly, and its signature says the
result views the input through the region `r`. With a single reference parameter, as here, the region could also be
left out: `fn longestWord(text: StringView) -> StringView?` means the same.

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

**No traits as types.** Interfaces are bounds and nothing else: no `dyn`, no default methods, no associated types, no
blanket `impl<T>`. Heterogeneous collections are enums.

**Closures do not escape.** A closure borrows what it captures and has a reference type (`borrow fn(i32) -> bool`,
`borrow mut fn(i32)`), so it cannot be returned or stored. There is no `move` closure, `Fn`/`FnMut`/`FnOnce` trait or
`impl Fn`. Calls through a closure are indirect.

**No macros, no iterator chains.** `for` walks ranges, arrays, slices and vectors; slices and vectors have `sortBy`,
`forEach`, `any`, `all` and `retain`. There is no `Iterator` trait yet.

**Through C.** CeeSafe compiles to C11, which any C toolchain or kernel build can take, including MSVC and compilers for
targets LLVM does not support. The generated C is readable and maps back to CeeSafe lines.

**Kernel checking.** Context effects (`@context(atomic)`, `@effects(sleep)`) reject sleeping under a spin lock across
calls, at compile time. Rust has no equivalent in the language.

## Things you will miss

`Rc` (CeeSafe has `Arc`, and lib/core's `Cell` and `RefCell`, whose `read()` and `write()` are Rust's `borrow()` and
`borrow_mut()`: type-system.md §41), threads and `Send`/`Sync` (there are no user threads yet),

`dyn Trait`, iterators, pattern guards (`if` in a match arm), slice patterns, `impl` blocks on generic types, the
standard library's breadth, and crates.io. Several are planned or recorded as open decisions.
