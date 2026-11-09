# Unsafe code and C

Safe CeeSafe cannot express everything a systems program needs: talking to hardware, calling C, implementing an
allocator. For those there is `unsafe`, kept small and visible. The contract unsafe code must keep is in
[Unsafe code](../spec/unsafe.md), and the C boundary in the type system's
[C interoperability](../spec/type-system.md#_28-c-interoperability-m8b) section.

## Raw pointers

`*T` and `*mut T` are raw pointers: plain addresses, Copy, with no borrow attached. **Making** a pointer is safe;
**using** one (`read`, `write`, `add`, turning it back into a reference) is allowed only inside `unsafe`.

```ceesafe
/// Safety: `p` points to `n` initialised `u32`s that nothing else writes while this runs.
unsafe fn sum(p: *u32, n: usize) -> u64 {
    var total: u64 = 0;
    for (i in 0..n) {
        total += p.add(i).read() as u64;
    }
    return total;
}

/// Safety: the pointer comes from a live array and stays inside it.
fn main() {
    var values = [1u32, 2, 3, 4];
    let p = borrow mut values[0] as *mut u32;
    unsafe {
        p.add(3).write(10);
    }
    Console.writeLine(unsafe { sum(p, values.length) });
}
```

```text
16
```

## The `Safety:` rule

Every `unsafe fn`, and every function that contains an `unsafe` block, must say in its doc comment why it is sound, on
a line starting with `Safety:`. The compiler enforces it:

<!-- ceesafe: fail N0283 -->
```ceesafe
fn first(p: *u8) -> u8 {
    return unsafe { p.read() };
}
```

```text
main.cee:1:4: error[N0283]: function 'first' uses 'unsafe' and needs a '/// Safety:' doc comment saying why it is sound (type-system.md §27.4)
  |
1 | fn first(p: *u8) -> u8 {
  |    ^^^^^
```

For an `unsafe fn`, the line states what callers must guarantee; for a safe function with an `unsafe` block inside,
why the block cannot go wrong. Reviewers read these lines; the compiler checks that they exist.

`unsafe` permits only the pointer operations, calls of unsafe functions and C functions, and volatile accesses.
Everything else in an unsafe block is checked exactly as in safe code.

## Calling C

C functions are declared in an `extern "C"` block and called inside `unsafe`, since CeeSafe cannot check what C does.
Their types are C's: integers, `bool`, raw pointers, `*CChar` for C strings, `CChar` for a `char`, and `@repr(C)`
structs.

<!-- ceesafe: check -->
```ceesafe
extern "C" {
    fn strlen(s: *CChar) -> usize;
    fn puts(s: *CChar) -> i32;
}

/// Safety: a `c"…"` ends in a zero byte, as `puts` and `strlen` need.
fn main() -> i32 {
    let text = c"hello from C";
    let p = text.ptr();
    unsafe {
        puts(p);
    }
    return truncate<i32>(unsafe { strlen(p) });
}
```

`c"…"` is a C string: the compiler adds the zero byte that ends it, and refuses a `\0` inside. Its type is lib/core's
`CStr`, a view with `ptr()` for C, `view()` and `length()` for CeeSafe (without the zero byte). It can be a constant or
a static. `CStr.fromBytesWithNul(bytes)` makes one from bytes that end in their only zero byte, or gives `null`; like
any view, it cannot outlive those bytes. `"…\0".ptr as *CChar` also works, but nothing checks the `\0` is there.

C files are built with the program: `ceesafec build main.cee helpers.c -o program`, or the `c` list of a package's
manifest.

CeeSafe can't see what a C function does, so a call of one counts as possibly sleeping and allocating
(type-system.md §32). Async code may not sleep, so calling such a function from an `async fn` is N0295. When the C
function returns promptly, say so on its declaration: `@effects()` for none, or `@effects(alloc)` if it allocates.

<!-- ceesafe: skip -->
```ceesafe
extern "C" {
    @effects()
    fn crc32_update(crc: u32, data: *u8, length: usize) -> u32;
}
```

## Sharing C declarations between packages

A C function has one declaration in a program, so its CeeSafe types can't drift apart: a second declaration with the
same C name is N0201, even in another package, and so is an `@export` with that name. One package owns the
declaration and makes it `public`, and the others call it through that package
([ADR 0081](../spec/decisions/0081-c-declarations-shared-between-packages.md)):

<!-- ceesafe: skip -->
```ceesafe
// sys/src/lib.cee, a library
extern "C" {
    public fn strlen(s: *CChar) -> usize;
}
```

<!-- ceesafe: skip -->
```ceesafe
// app/src/main.cee, which depends on sys
/// Safety: the literal ends in a zero byte, as strlen needs.
fn main() {
    let text = "hello\0";
    Console.writeLine(unsafe { sys.strlen(text.ptr as *CChar) });
}
```

If `app` declares `strlen` too, N0201 is reported at `app`'s declaration, with a note at the one `sys` owns. The
message says what to do: ``use `sys.strlen`, which package 'sys' declares; remove this declaration``, or, when the
declaration in `sys` isn't `public`, to make it `public` there. When the two declarations are identical in CeeSafe
(the same parameter and result types, variadic-ness and attributes), `ceesafec fix` removes the redundant one. It
doesn't change the calls, which still need `sys.` in front.

To give a C function a stronger type, such as a `borrow` instead of a raw pointer, write a wrapper in the package that
wants it, rather than a second declaration. The wrapper calls the owner's declaration and says in its `Safety:` line
why that call is sound. Within one package, two declarations with one C name are N0201 too, reported at the later
one in the package's file order:

<!-- ceesafe: fail N0201 -->
```ceesafe
extern "C" {
    fn strlen(s: *CChar) -> usize;
    @cname("strlen")
    fn length(s: *CChar) -> usize;
}
```

```text
main.cee:4:8: error[N0201]: the C name 'strlen' of C function 'strlen' is already used by another C function or export (type-system.md §28.2)
  |
4 |     fn length(s: *CChar) -> usize;
  |        ^^^^^^
  = note: the other declaration is here (2:8)
```

## Being called from C

`@export` makes a CeeSafe function callable from C, and `@repr(C)` gives a struct C's layout. `ceesafec` writes
`ceesafe_exports.h` with their declarations for the C side to include.

<!-- ceesafe: check -->
```ceesafe
@repr(C)
struct Point {
    x: i32;
    y: i32;
}

/// The Manhattan length of a point, callable from C as `geo_length(struct Point)`.
@export("geo_length")
fn length(p: Point) -> i32 => abs(p.x) + abs(p.y);

fn abs(v: i32) -> i32 => if (v < 0) -v else v;

fn main() {
    Console.writeLine(length(Point { x: 3, y: -4 }));
}
```

`@callback` gives a function a C address without exporting a symbol, for C APIs that take a function pointer and a
`void *` argument, as kernels' timer and thread APIs do. `extern "C" fn(…) -> R` is the type of such a pointer.

## C structs from the C code's headers

A C codebase's own structs are not re-declared in CeeSafe, where the two copies could drift apart. They are imported from
its headers: CeeSafe uses them by pointer or embedded by value, names only the fields it uses, and the C compiler checks
each one at build time.

<!-- ceesafe: skip -->
```ceesafe
@header("kernel/timer.h")
extern "C" {
    @ctype("struct timer")
    struct timer;
    @effects()
    fn timer_start(t: *mut timer, delay_ns: u64);
}
```

See [C structs from headers](../spec/decisions/0008-c-struct-bindings-from-headers.md) and
[Kernel code](kernel.md).

### Arrays, keyword names and tables

A C array member is declared as a CeeSafe array (`uint8_t uuid[16]` is `uuid: [u8; 16]`) and used like one: indexed with
bounds checks, sliced, copied. A member whose name is a CeeSafe keyword takes `@cname`. An array of C structs is indexed in
place. A constant table can be exported to C as data with `@export`.

<!-- ceesafe: check -->
```ceesafe
@header("kernel/gpt.h")
extern "C" {
    struct gpt_part {
        first_lba: u64;
        @cname("type")
        kind: [u8; 16];
        uuid: [u8; 16];
    }
    struct gpt_table {
        nparts: u32;
        parts: [gpt_part; 32];
    }
}

/// `const uint8_t gpt_type_esp[16]` in C.
@export("gpt_type_esp")
const TYPE_ESP: [u8; 16] = [0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11, 0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b];

/// Is partition `i` an EFI system partition?
fn isEsp(table: borrow gpt_table, i: usize) -> bool {
    let kind = table.parts[i].kind; // a copy of the C array
    for (k in 0..kind.length) {
        if (kind[k] != TYPE_ESP[k]) { return false; }
    }
    return true;
}
```

This is how CosmoOS's GPT parser is written (`integrations/cosmoos/c9.py` checks it; the rules are
[ADR 0014](../spec/decisions/0014-c9-c-arrays-and-exported-constants.md)).

### C enums

A header that types a parameter or a member with an enum is imported with `enum name: I;`, where `I` is the integer the C
compiler gives the enum (usually `u32`). The type is an integer of its own: `as` converts it to and from `I`, and its
values are constants of the type. CeeSafe spells it `enum name` (or a typedef's name, with `@ctype`) everywhere C sees it,
so a function pointer type whose C type names the enum, such as an operation table's entry, is the same C type as the
CeeSafe function's. With `u32` in its place, C would accept the call, but UBSan's `-fsanitize=function` refuses a call
through a pointer of another type.

<!-- ceesafe: check -->
```ceesafe
@header("kernel/vfs.h")
extern "C" {
    enum vnode_type: u32;
    struct vnode {
        @cname("type")
        kind: vnode_type;
    }
}

const VNODE_DIR: vnode_type = 2;

fn isDirectory(vn: borrow vnode) -> bool => vn.kind == VNODE_DIR;

/// A readdir callback: its type, `vfs_dirent_cb`, takes the enum.
@callback
fn emit(arg: *mut void, name: *CChar, len: usize, ino: u64, kind: vnode_type) -> i32 => if (kind == VNODE_DIR) 1 else 0;
```

The C compiler checks that the enum is the integer declared. The rules are
[ADR 0079](../spec/decisions/0079-imported-c-enums.md).

## C constants, C data and configured code

A port of a C file keeps using what the C side defines: its constants, its globals, its build switches. The
[C data rules](../spec/decisions/0080-the-kernel-floors.md) let CeeSafe name each of them without a C file of its own.

**Constants C checks.** A constant written in an `extern "C"` block is an ordinary CeeSafe constant, and the C compiler
asserts that the header agrees. `@cvalue` asserts a C expression instead, such as a size or an offset. `const NAME: T;`
with no value reads C's own value at run time, for a string macro:

<!-- ceesafe: check -->
```ceesafe
@header("kernel/vfs.h")
extern "C" {
    const EINVAL: i32 = 22;
    @cvalue("sizeof(struct cosmo_stat)")
    const STAT_SIZE: usize = 144;
    const COSMO_VERSION: *CChar;
}

fn invalid() -> i32 => -EINVAL;
```

**Data C names, and data C defines.** `@export` on a static makes it C data under that symbol: a plain `static` of a
C type is C's `const` object, such as an operation table a header declares `extern const`, and a `static mut` is a
variable C may write, such as a sysctl's. In an `extern "C"` block, `static NAME: T;` reads data C defines:

<!-- ceesafe: check -->
```ceesafe
@header("kernel/storage.h")
extern "C" {
    struct fs_type {
        flags: u32;
    }
    static procnet_names: [*CChar; 8];
}

/// `extern const struct fs_type pool_fs_type;` in C.
@export("pool_fs_type")
static POOL_FS: fs_type = fs_type { flags: 1 };

/// `extern uint32_t g_pool_rate;`: the sysctl writes it.
@export("g_pool_rate")
static mut RATE: u32 = 100;
```

**C locals.** `var s: T;` of a C type is C's zero-filled object on the stack, for state a C API keeps in the caller's
frame, as a hash state is. It is used by its address and never moved:

<!-- ceesafe: check -->
```ceesafe
@header("kernel/blake2b.h")
extern "C" {
    struct blake2b;
    fn blake2b_init(s: *mut blake2b, outlen: usize);
    fn blake2b_final(s: *mut blake2b, out: *mut u8);
}

/// Safety: the state is a live C local for each call.
fn digest(out: borrow mut [u8; 32]) {
    var s: blake2b;
    unsafe {
        blake2b_init(borrow mut s as *mut blake2b, 32);
        blake2b_final(borrow mut s as *mut blake2b, borrow mut out[0] as *mut u8);
    }
}
```

**Code only some builds have.** `if (cfg("CONFIG_DEBUG")) { … } else { … }` is C's `#if CONFIG_DEBUG`. A member or a C
function that only some builds have is declared `@cfg("CONFIG_DEBUG")` and used only inside such a block, which the
compiler checks (N0350). Both blocks are checked in every build:

<!-- ceesafe: check -->
```ceesafe
@header("kernel/cosmofs.h")
extern "C" {
    struct cfs {
        @cfg("CONFIG_DEBUG")
        test_armed: bool;
    }
    @cfg("CONFIG_DEBUG")
    fn cfs_debug_note(fs: *mut cfs);
}

/// Safety: `fs` is a live filesystem.
@export("cfs_test_arm")
unsafe fn arm(fs: *mut cfs) {
    if (cfg("CONFIG_DEBUG")) {
        let f = Ptr.toBorrowMut(fs);
        f.test_armed = true;
        cfs_debug_note(fs);
    }
}
```

`CChar` is C's `char` by value as well as behind a pointer (`b as CChar`, `c as u8`), `CLong` and `CULong` are C's
`long` and `unsigned long`, an imported struct's member may be an array of pointers, and `AtomicU8` is a byte member C
changes atomically.

## C code that allocates

A package's C files can allocate through CeeSafe's allocator, so the allocation-failure sweep reaches their
out-of-memory paths too. `ceesafe/foreign.h` is on every C file's include path:

```c
#include "ceesafe/foreign.h"

void *block = ceesafe_alloc(size, CEE_ALLOC_FOREIGN);      /* malloc */
block = ceesafe_realloc(block, bigger, CEE_ALLOC_FOREIGN); /* realloc; NULL allocates */
ceesafe_free(block, CEE_ALLOC_FOREIGN);                    /* free; NULL does nothing */
```

These allocations are counted and can be failed (`CEESAFE_FAIL_ALLOC_AT`, `ceesafec test --sweep alloc`) like
CeeSafe's own, but a block C still holds at exit is not a leak, since C libraries keep global state until the process
ends. The allocation report shows those blocks on their own line, `ceesafe alloc: foreign held=<k>`. A library with
allocation hooks can be pointed at them in test builds; OpenSSL's are `CRYPTO_set_mem_functions`
([ADR 0060](../spec/decisions/0060-web-random-tryrecv-foreign-host.md)).

Some libraries cannot survive a failed allocation during their one-time setup: OpenSSL's initialisation re-enters
itself when one fails. Allocate during that setup with `CEE_ALLOC_FOREIGN_INIT` instead: those allocations are never
failed and do not count as attempts, so the sweep tests the program's own out-of-memory paths, and the blocks they
keep are reported as held, like `CEE_ALLOC_FOREIGN`'s. Either number may reallocate or free a block made under the
other. Use it for the setup only: an allocation made under it is never tested
([ADR 0061](../spec/decisions/0061-foreign-allocations-for-initialisation.md)).

## Next

[Kernel code](kernel.md).
