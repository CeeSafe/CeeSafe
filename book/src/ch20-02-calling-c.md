---
title: Calling C and being called by it
summary: Declaring C functions in an extern "C" block and calling them in unsafe, the types that cross, strings and buffers, @repr types, @export and @callback so that C calls CeeSafe, memory that C allocates, and structs imported from a header with cbind.
assumes: [ch20-01-unsafe-code, ch07-01-package-and-manifest, ch14-03-libraries-for-other-programs]
keywords: [extern, C, unsafe, CChar, CStr, "C string literal", "@repr(C)", "@repr(u8)", "@export", "@callback", "function pointer", "@header", cbind, "ceesafe_exports.h", "ceesafe/foreign.h", strlen]
diagnostics: [N0280, N0291]
specification: ["type-system.md §28", "type-system.md §31", "type-system.md §34.1", "type-system.md §22.3", "unsafe.md §2"]
---

# Calling C and being called by it

**This section teaches** how a CeeSafe program calls functions written in C, hands them strings and buffers, and
lets them call back. It assumes [Unsafe code](./ch20-01-unsafe-code) and
[A package and its manifest](./ch07-01-package-and-manifest), and that you can read a little C.
[Libraries and objects for other programs](./ch14-03-libraries-for-other-programs) covers the opposite
arrangement, a C program that links a CeeSafe library, and is not repeated here.

`ceesafec` compiles CeeSafe to C, so a C function is close by: it is declared, and then called. What CeeSafe
cannot do is see inside it. Every call of a C function is therefore an unsafe operation, and the function that
makes it says in a `Safety:` line why the call keeps C's rules.

## Declaring and calling a C function

<span class="listing-caption"><strong>Listing 20-8</strong>, <code>main.cee</code>: Three functions of the C library, declared and called.</span>

```ceesafe
extern "C" {
    fn strlen(text: *CChar) -> usize;
    fn puts(text: *CChar) -> i32;
    fn abs(value: i32) -> i32;
}

/// Safety: a `c"…"` literal ends in a zero byte, which `strlen` and `puts` need. `abs` takes any `i32`
/// but the least one, and -42 is not that.
fn main() {
    let greeting = c"hello from C";
    let length = unsafe { strlen(greeting.ptr()) };
    Console.writeLine(length);
    Console.writeLine(greeting.length());
    Console.writeLine(unsafe { abs(-42) });
    unsafe {
        puts(greeting.ptr());
    }
}
```

```text
12
12
42
hello from C
```

- `extern "C" { … }` lists functions that C defines: a signature and `;`, no body. The CeeSafe name is the C
  name. When the two must differ, `@cname("c_name")` before the declaration gives the C one.
- The types are C's. `*CChar` is `const char *`, `usize` is `size_t`, `i32` is `int32_t`.
- `c"hello from C"` is a **C string literal**: the compiler adds the zero byte that ends a C string. Its type is
  `CStr`, with `ptr()` for C, and `view()` and `length()` for CeeSafe, which do not count the zero byte.

The declaration is a promise too. CeeSafe trusts that `strlen` has this signature, so write it from the C header.
A C name has one declaration in a whole program: a second one, in any package, is an error.

::: tip Rule
A call of an `extern "C"` function is allowed only in an unsafe context. You promise what the C function's
documentation asks, that the declared types are the C function's, and that pointers you pass are valid for as
long as C uses them.

*Specification: `type-system.md` §28.2; `unsafe.md` §2.*
:::

## What crosses

Only values that C has can be parameters and results:

| CeeSafe | C |
|---|---|
| `i8` to `i64`, `u8` to `u64`, `usize`, `isize` | `int8_t` to `uint64_t`, `size_t`, `ptrdiff_t` |
| `f32`, `f64`, `bool` | `float`, `double`, `bool` |
| `CChar`, `CLong`, `CULong` | `char`, `long`, `unsigned long` |
| `*T`, `*mut T` | `const T *`, `T *` |
| `*void`, `*mut void` | `const void *`, `void *` |
| a struct marked `@repr(C)` | `struct` of the same name and fields |
| an enum without payloads marked `@repr(u8)`, or another integer type | that integer type |
| `extern "C" fn(A) -> R` | a pointer to a function, `R (*)(A)` |

Borrows, slices, `StringView`, `String`, `Vector`, optionals, `Result` and ordinary structs and enums do not
cross. A slice crosses as two values, its `ptr` and its `length`. Text crosses as a `*CChar` to bytes that end in
zero.

## A package with a C file

The package `gauge` (in `book/projects/ch20/`) has one C file. The manifest names it, and `ceesafec build`
compiles it with the program:

```toml
[package]
name = "gauge"
version = "0.1.0"
kind = "executable"
c = ["c/gauge.c"]
```

<span class="listing-caption"><strong>Listing 20-9</strong>, <code>gauge/src/c.cee</code>: The types both sides share, the C functions, and one CeeSafe function that C may call.</span>

<!-- ceesafe: skip -->
```ceesafe
/// A range of values, `struct Range` in C.
@repr(C)
struct Range {
    low: i32;
    high: i32;
}

/// A unit, one byte in C.
@repr(u8)
enum Unit {
    Celsius,
    Kelvin,
}

extern "C" {
    fn gauge_peak(values: *i32, count: usize, range: Range) -> i32;
    fn gauge_label(range: Range, unit: Unit, out: *mut CChar, capacity: usize) -> usize;
    fn gauge_each(values: *i32, count: usize, visit: extern "C" fn(*mut void, i32), context: *mut void);
    fn gauge_title(name: *CChar) -> *mut CChar;
    fn gauge_release(title: *mut CChar);
}

/// `value`, moved into `range`. C calls it as `gauge_clamp`.
@export("gauge_clamp")
fn clamp(value: i32, range: Range) -> i32 => value.max(range.low).min(range.high);
```

`@repr(C)` gives a struct C's layout, and `@repr(u8)` makes an enum one byte whose values are the variants'
positions, 0 and 1. `@export("gauge_clamp")` makes `clamp` a C function of that name. For these, the compiler
writes a header, `ceesafe_exports.h`, at every build. For `gauge` it holds:

```c
struct Range {
    int32_t low;
    int32_t high;
};

typedef uint8_t Unit;
enum { Unit_Celsius = 0, Unit_Kelvin = 1 };

int32_t gauge_clamp(int32_t, struct Range);
```

The C file includes it, so both languages have one definition of each type.

<span class="listing-caption"><strong>Listing 20-10</strong>, <code>gauge/c/gauge.c</code>: Three of the C functions. The first calls back into CeeSafe.</span>

<!-- ceesafe: skip -->
```c
/* The largest of `count` values, each first clamped by CeeSafe's gauge_clamp. */
int32_t gauge_peak(const int32_t *values, size_t count, struct Range range) {
    int32_t peak = range.low;
    for (size_t i = 0; i < count; i++) {
        int32_t value = gauge_clamp(values[i], range);
        if (value > peak) {
            peak = value;
        }
    }
    return peak;
}

/* Writes "low..high unit" to `out`, at most `capacity` bytes with the zero byte. Returns the text's length. */
size_t gauge_label(struct Range range, Unit unit, char *out, size_t capacity) {
    int length = snprintf(out, capacity, "%d..%d %s", range.low, range.high, unit == Unit_Kelvin ? "K" : "C");
    if (length < 0) {
        return 0;
    }
    return (size_t)length < capacity ? (size_t)length : capacity - 1;
}

/* Calls `visit(context, value)` for each value. */
void gauge_each(const int32_t *values, size_t count, void (*visit)(void *, int32_t), void *context) {
    for (size_t i = 0; i < count; i++) {
        visit(context, values[i]);
    }
}
```

## Buffers and strings

<span class="listing-caption"><strong>Listing 20-11</strong>, <code>gauge/src/main.cee</code>: Two safe functions. One passes a slice to C, the other gives C a buffer to fill.</span>

<!-- ceesafe: skip -->
```ceesafe
/// The largest of `values` after clamping to `range`.
/// Safety: a slice's `ptr` and `length` are a pointer to that many live `i32`s, which C only reads.
fn peak(values: Slice<i32>, range: Range) -> i32 => unsafe { gauge_peak(values.ptr, values.length, range) };

/// Appends C's label for `range` to `out`.
/// Safety: C gets the address and the size of `buffer` and writes at most that many bytes. It returns the
/// number of bytes of text, which is less than the size, so the slice is inside `buffer`.
fn label(range: Range, unit: Unit, out: borrow mut String) -> Result<void, AllocError> {
    var buffer = [0u8; 32];
    let space = buffer[..];
    let length = unsafe { gauge_label(range, unit, space.ptr as *mut CChar, space.length) };
    return out.append(StringView.fromUtf8(buffer[..length]) ?? "?");
}
```

`peak` and `label` are safe functions. Their callers pass a slice and a `String`, and cannot get the pointer or
the length wrong. That is the shape to aim for: one small function for each C function, which turns CeeSafe's
types into C's and back.

To **give** C bytes, pass `ptr` and `length`. To **get** bytes from C, pass a buffer and its size, and take the
count C returns. What comes back is bytes, not text: `StringView.fromUtf8` checks them and gives an optional.
`buffer[..length]` is bounds-checked, so a wrong count from C panics instead of reading past the buffer.

## Callbacks

A C function that takes a pointer to a function can be given a CeeSafe function. `@callback` gives the function
a C address, and where an `extern "C" fn(…)` type is expected, its name is a value of that type.

<span class="listing-caption"><strong>Listing 20-12</strong>, <code>gauge/src/main.cee</code>: C calls <code>addTo</code> once for each value, with the pointer it was given.</span>

<!-- ceesafe: skip -->
```ceesafe
/// Adds `value` to the `i64` that `context` points to.
/// Safety: C passes the `context` it was given, which `total` makes from a live `i64`.
@callback
unsafe fn addTo(context: *mut void, value: i32) {
    let sum = Ptr.toBorrowMut(context as *mut i64);
    sum += value as i64;
}

/// The sum of `values`, added up by a callback that C calls once for each.
/// Safety: `sum` is live during the call and C does not keep the pointer. `addTo` has the type C expects.
fn total(values: Slice<i32>) -> i64 {
    var sum: i64 = 0;
    unsafe {
        gauge_each(values.ptr, values.length, addTo, borrow mut sum as *mut i64 as *mut void);
    }
    return sum;
}
```

A callback is a top-level function, so it captures nothing. C's convention for that is the `void *` beside the
function pointer: `total` passes the address of `sum`, and `addTo` casts it back. A closure literal is not
accepted where C's function pointer is expected. If C keeps the pointer after the call returns, a local will not
do: put the value in a box and pass `Box.intoRaw(box)`, as
[Unsafe code](./ch20-01-unsafe-code#a-box-as-a-raw-pointer) shows.

`@export` and `@callback` differ in one thing. An exported function has a C name that C code calls directly, as
`gauge.c` calls `gauge_clamp`. A callback has no name in C, only an address.

## Memory that C allocates

<span class="listing-caption"><strong>Listing 20-13</strong>, <code>gauge/src/main.cee</code>: C allocates a string. CeeSafe copies it and hands it back to C to free.</span>

<!-- ceesafe: skip -->
```ceesafe
/// C's title for `name`, copied into a `String`.
/// Safety: `name` ends in a zero byte. C returns null or a zero-terminated string that is ours until
/// `gauge_release`; the copy is made before that, and the pointer is not used after it.
fn title(name: CStr) -> Result<String, AllocError> {
    let raw = unsafe { gauge_title(name.ptr()) };
    if (raw.isNull()) {
        return Err(AllocError.OutOfMemory);
    }
    let text = unsafe { CStr.fromPtr(raw) };
    let copy = String.from(if (text is Some(t)) t.view() else "?");
    unsafe {
        gauge_release(raw);
    }
    return copy;
}
```

In C, `gauge_title` calls `malloc` and `gauge_release` calls `free`.

::: tip Rule
Memory is freed by the side that allocated it. A block from C goes back to a C function, and a `Box` or `Vector`
is dropped by CeeSafe. Never pass one side's block to the other side's `free`.

*Specification: `type-system.md` §28.6.*
:::

A string that C returns has no length. `CStr.fromPtr(p)` counts the bytes up to the zero and gives a `CStr?` that
views them where they are. It is `null` for a null pointer and for bytes that are not UTF-8. It is unsafe: the
bytes must end in a zero, and must not change or be freed while the view is used. So the copy must be finished
before the block is released. Check a returned pointer with `isNull()` before any other use.

A package's own C files can also allocate through CeeSafe's allocator, with `ceesafe_alloc`, `ceesafe_realloc`
and `ceesafe_free` from the header `ceesafe/foreign.h`. Then `ceesafec test --sweep alloc`, which
[Allocation failure](./ch09-03-allocation-failure) describes, reaches the C code's failure paths too.

<span class="listing-caption"><strong>Listing 20-14</strong>, <code>gauge/src/main.cee</code>: The program. It has no <code>unsafe</code>.</span>

<!-- ceesafe: skip -->
```ceesafe
fn main() {
    let readings = [18, 25, 41, -3];
    let range = Range { low: 0, high: 30 };
    Console.writeLine(peak(readings[..], range));
    Console.writeLine(total(readings[..]));
    var text = String.create();
    label(range, Unit.Celsius, mut text)!;
    Console.writeLine(text.view());
    Console.writeLine(title(c"north wall")!.view());
}
```

```sh
ceesafec build
./out/gauge
```

```text
30
81
0..30 C
gauge: north wall
```

`ceesafec run --interp` cannot run this program: the interpreter runs no C. It knows only a few functions of the
C library by name, `strlen`, `puts`, `abs`, `malloc`, `free`, `memcmp`, `memcpy` and `memset`, which is why
Listing 20-8 runs there.

## Types from a C header

When the C code has its own structs, do not write them again with `@repr(C)`, where the two copies could come to
differ. Import them. An `extern "C"` block with `@header("clock.h")` may declare `struct name { … }` with the
fields CeeSafe uses, under their C names, and the C compiler checks each field against the header at every
build. The package `clock` has this header:

<span class="listing-caption"><strong>Listing 20-15</strong>, <code>clock/c/clock.h</code>: A C header with a macro, an enum, a struct and four functions.</span>

<!-- ceesafe: skip -->
```c
#define CLOCK_MAX_LAPS 8

enum clock_mode { CLOCK_STOPPED = -1, CLOCK_RUNNING = 1 };

struct clock {
    uint64_t ticks;
    uint32_t laps;
    enum clock_mode mode;
    const char *name;
};

struct clock *clock_main(void);
void clock_start(struct clock *clock, const char *name);
bool clock_tick(struct clock *clock, uint64_t ticks);
uint64_t clock_elapsed(const struct clock *clock);
```

The block can be written by hand, or generated. `ceesafec cbind` reads a header with `clang`, which must be
installed, and writes the block. It compiles and runs nothing. Run it in the package's directory:

```sh
ceesafec cbind --header clock.h -I c -o src/clock.cee
```

<span class="listing-caption"><strong>Listing 20-16</strong>, <code>clock/src/clock.cee</code>: What <code>ceesafec cbind</code> wrote for the header.</span>

<!-- ceesafe: skip -->
```ceesafe
@header("clock.h")
extern "C" {
    struct clock {
        ticks: u64;
        laps: u32;
        mode: clock_mode;
        name: *CChar;
    }
    enum clock_mode: i32;
    const CLOCK_STOPPED: clock_mode = -1;
    const CLOCK_RUNNING: clock_mode = 1;

    const CLOCK_MAX_LAPS: i32 = 8;

    fn clock_main() -> *mut clock;
    fn clock_start(clock: *mut clock, name: *CChar);
    fn clock_tick(clock: *mut clock, ticks: u64) -> bool;
    fn clock_elapsed(clock: *clock) -> u64;
}
```

An imported struct has C's layout, which CeeSafe does not know. So no value of it exists in CeeSafe: it is never
a local, a parameter or a struct literal, and it is used only behind a pointer or a borrow. C makes the `clock`,
and CeeSafe reads its fields through a borrow made from C's pointer. An imported enum is an integer type of its
own, compared with `==`. A constant in the block is checked against the macro when the C is compiled.

<span class="listing-caption"><strong>Listing 20-17</strong>, <code>clock/src/main.cee</code>: C owns the struct. CeeSafe calls C with its address and reads its fields.</span>

<!-- ceesafe: skip -->
```ceesafe
/// Safety: `clock_main` returns the address of a C static, which lives for the whole program and which
/// nothing else uses. The name is a literal that ends in a zero byte. The borrow `stopwatch` is made after
/// the last call that changes the clock.
fn main() {
    let p = unsafe { clock_main() };
    unsafe {
        clock_start(p, c"kitchen".ptr());
    }
    while (unsafe { clock_tick(p, 250) }) {}
    let stopwatch = unsafe { Ptr.toBorrow(p) };
    Console.writeLine(stopwatch.laps);
    Console.writeLine(stopwatch.ticks);
    Console.writeLine(stopwatch.mode == CLOCK_RUNNING);
    Console.writeLine(unsafe { clock_elapsed(p) });
    Console.writeLine(CLOCK_MAX_LAPS);
}
```

The generated C includes `clock.h`, so the C compiler must find it. The manifest names the directory, beside the
C file:

```toml
c = ["c/clock.c"]
include-dirs = ["c"]
```

```sh
ceesafec build
./out/clock
```

```text
8
2000
true
2000
8
```

## Mistakes

::: warning Mistake: calling a C function outside `unsafe`
Even a function as harmless as `abs` has a condition the compiler cannot check.
:::

<span class="listing-caption"><strong>Listing 20-18</strong>, <code>main.cee</code>: A C function called from safe code. This does not compile.</span>

<!-- ceesafe: fail N0280 -->
```ceesafe
extern "C" {
    fn abs(value: i32) -> i32;
}

fn main() {
    Console.writeLine(abs(-42));
}
```

```text
main.cee:6:23: error[N0280]: calling C function 'abs' is unsafe: use it inside 'unsafe { ... }' or an 'unsafe fn' (type-system.md §27.3)
  |
6 |     Console.writeLine(abs(-42));
  |                       ^^^^^^^^
```

Write `unsafe { abs(-42) }`, and the `Safety:` line that Listing 20-8 has.

::: warning Mistake: a CeeSafe type in a C signature
C has no `StringView`. The declaration is refused, before any call.
:::

<span class="listing-caption"><strong>Listing 20-19</strong>, <code>main.cee</code>: A C function declared with a <code>StringView</code> parameter. This does not compile.</span>

<!-- ceesafe: fail N0291 -->
```ceesafe
extern "C" {
    fn count_spaces(text: StringView) -> usize;
}

/// Safety: none yet. The declaration does not compile.
fn main() {
    Console.writeLine(unsafe { count_spaces("a b c") });
}
```

```text
main.cee:2:27: error[N0291]: C function 'count_spaces' cannot take a 'StringView': only integers, bool, raw pointers, @repr(C) structs and @repr enums cross to C (type-system.md §28.3)
  |
2 |     fn count_spaces(text: StringView) -> usize;
  |                           ^^^^^^^^^^
```

Declare `text: *u8, length: usize` and pass `view.ptr` and `view.length`, or `text: *CChar` and pass a `CStr`.
The same code is reported for an `@export` function that takes an ordinary struct: mark the struct `@repr(C)`.

::: warning Mistake: text without its zero byte
A `StringView` has a length and no zero byte at its end. C's string functions read until they find one.
:::

<span class="listing-caption"><strong>Listing 20-20</strong>, <code>main.cee</code>: The pointer of an ordinary literal, passed where C expects a string.</span>

<!-- ceesafe: exits 70 -->
```ceesafe
extern "C" {
    fn puts(text: *CChar) -> i32;
}

/// Safety: wrong. The text has no zero byte at its end.
fn main() {
    let text = "no zero byte";
    unsafe {
        puts(text.ptr as *CChar);
    }
}
```

This compiles. The interpreter stops it:

```text
ceesafe interp: fault: access out of bounds of a string literal (1 bytes at offset 12 of 12) at main.cee:9:9
```

A compiled program has no such check: `puts` reads on past the text until it meets a zero byte. Use
`c"no zero byte"`, or
for text made while the program runs, `CStr.fromBytesWithNul(bytes)`, which checks that the bytes end in their
only zero byte and gives an optional.

::: details Coming from Rust
`extern "C" { fn … }` is the same block, and calling is unsafe in both. What differs:

- `@export("name")` is `#[no_mangle] pub extern "C" fn`, `@repr(C)` is `#[repr(C)]`, and `c"…"` with `CStr` is
  Rust's `c"…"` with `&CStr`. There is no `CString`: copy into a `String` or a buffer;
- a callback is marked `@callback`, and its type is written `extern "C" fn(…)`;
- `ceesafec cbind` does `bindgen`'s work, and the exports header is written by the compiler, with no `cbindgen`;
- an imported struct is seen through the header at build time. Its layout is never copied into CeeSafe.
:::

::: details Coming from C
You are writing prototypes, in another spelling, and the compiler emits C that calls them. The differences are
what CeeSafe refuses to pass: no array decays to a pointer, no struct crosses unless it is `@repr(C)` or
imported, and a string is not a pointer until you ask for `ptr()`.
:::

::: details Coming from C#, Java, TypeScript or Go
This is P/Invoke, JNI or cgo with less machinery: no marshalling layer, no pinned objects, no second stack.
Values cross as they are, which is why only C's own types may cross.
:::

## Summary

- `extern "C" { fn name(…) -> R; }` declares a C function. Calling it needs an unsafe context (`N0280`) and a
  `Safety:` line.
- Integers, floats, `bool`, raw pointers, `@repr(C)` structs, `@repr(u8)` enums and `extern "C" fn` pointers
  cross. Nothing else does (`N0291`).
- A slice crosses as `ptr` and `length`. Text crosses as `c"…"` or a `CStr`, and comes back through
  `CStr.fromPtr`.
- `c = ["file.c"]` in the manifest builds C files with the program. They include `ceesafe_exports.h` for the
  `@repr` types and the `@export` functions. `include-dirs = ["dir"]` names the directories of their headers.
- `@export("name")` gives a function a C name. `@callback` gives it a C address, to pass where C wants a function
  pointer, with a `void *` for its data.
- Each side frees what it allocated.
- `@header` imports a C struct by the fields you name. `ceesafec cbind` writes the block from the header.
- Wrap each C function in a small safe function. The interpreter runs no C.

Next: [More on interfaces and generics](./ch20-03-more-interfaces-and-generics), back in safe code.
