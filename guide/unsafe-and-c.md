# Unsafe code and C

Safe CeeSafe cannot express everything a systems program needs: talking to hardware, calling C, implementing an
allocator. For those there is `unsafe`, kept small and visible. The contract unsafe code must keep is in
[Unsafe code](../spec/unsafe.md), and the C boundary in the type system's
[C interoperability](../spec/type-system.md#_28-c-interoperability-m8b) section.

## Raw pointers

`*T` and `*mut T` are raw pointers: plain addresses, Copy, with no borrow attached. **Making** a pointer is safe;
**using** one (`read`, `write`, `add`, turning it back into a borrow) is allowed only inside `unsafe`.

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

Making a pointer from a borrow does not use the borrow up: `let p = r as *mut T;` reborrows a `borrow mut`
parameter `r` for the cast, and `r` can be used afterwards. The pointer holds no loan, so nothing checks it against
later uses of `r`: that is what the `unsafe` block answers for.

`p.read()` and `p.write(v)` need `p` aligned for its type. For a value at any byte offset (a field of a packed C
struct, a word in the middle of a buffer) use `p.readUnaligned()` and `p.writeUnaligned(v)`, which take a Copy
type and compile to one load or store:

```ceesafe
/// Safety: the caller gives four readable bytes at `p`.
unsafe fn wordAt(p: *u8) -> u32 => (p as *u32).readUnaligned();

/// Safety: `bytes` has five bytes, and the word is bytes 1 to 4.
fn main() {
    let bytes = [9u8, 1, 0, 0, 0];
    Console.writeLine(unsafe { wordAt(borrow bytes[1] as *u8) });
}
```

```text
1
```

The value is in the machine's byte order. For a fixed order, `bytes.readLE<u32>(at)` and `readBE` on a byte slice
check the bounds and need no `unsafe`.

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
a static, and there `PREFIX + c"…"` joins constant text before it (`const RC: CStr = HOME + c"/.bunrc";`).
`CStr.fromBytesWithNul(bytes)` makes one from bytes that end in their only zero byte, or gives `null`; like
any view, it cannot outlive those bytes. For a `*CChar` that C returns, `unsafe { CStr.fromPtr(p) }` is the string
at `p`, up to its zero byte, or `null` when `p` is null or the bytes are not UTF-8; you promise that the bytes end in
a zero byte and stay as they are while you use the result, so copy the text (`String.from(s.view())`) before C frees
it. `"…\0".ptr as *CChar` also works, but nothing checks the `\0` is there.

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

In C, a `@repr(C)` struct or `@repr` enum keeps the name it is declared with, even when it comes from a dependency:
package `geo`'s `Point` is `struct Point` in `ceesafe_exports.h`, not `geo.Point`. So, like a C function, a `@repr`
type's name belongs to one package in a program: a second `@repr(C) struct Point` in another package is N0201
(type-system.md §28.5). Ordinary structs and enums have no such limit.

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
    struct timer;
    @effects()
    fn timer_start(t: *mut timer, delay_ns: u64);
}
```

See [C structs from headers](../spec/decisions/0008-c-struct-bindings-from-headers.md) and
[Kernel code](kernel.md).

### Generating the block from the header

`ceesafec cbind` writes the block for you. It reads the header with clang (nothing is compiled or run, so a header
for another target is read like any other) and writes one `.cee` file with every function, struct, enum, integer or
string macro and `extern` variable the header itself declares:

```sh
ceesafec cbind --header kernel/handle.h -I kernel/include -o kernel/bind/handle.cee
ceesafec cbind --header kernel/handle.h --header kernel/socket.h -I kernel/include --out-dir kernel/bind \
    --config x86_64="--target=x86_64-unknown-none-elf -DCONFIG_DEBUG=1" \
    --config aarch64="--target=aarch64-unknown-none-elf -DCONFIG_DEBUG=1"
```

- **Clang.** The command needs clang, the one C compiler that writes its syntax tree out: it uses `--cc`, else
  `$CC`, else `cc` as `ceesafec build` does, and when that is not clang and you named none, `clang` from `PATH`.
  With gcc or MSVC alone it stops and says so. `ceesafec cbind --help` lists the options.

- **Use.** Commit the generated files, and name them as extra sources beside the file that uses them
  (`ceesafec build port.cee kernel/bind/handle.cee`): several files compile as one unit. A CI step that generates
  them again and fails on a difference keeps them true to the headers.
- **What it leaves out.** A struct with a bit-field, an anonymous member, a flexible array or a `volatile` member is
  declared opaque, with a comment that names the member. A function that takes or returns a struct by value, or a
  `va_list`, is left out, as is a macro that is not an integer constant or a string. `union name` has no CeeSafe
  spelling, and neither has a type whose name C reserves (`struct __name`, `struct _Name`). Each is listed in a comment at the end of the file. The tool never guesses a type.
- **Several configurations.** With `--config`, given once per architecture and build, only what is the same in all
  of them is declared. The rest is listed, to be written by hand with `@cfg`; a struct whose members differ is
  declared by its name alone, and `--leave NAME` drops even that when your own block declares the struct.
- **Several headers.** Given together, each header gets one file, and a name is declared once among them: a type by
  the header that defines it. `--with FILE.cee` and `--leave NAME,…` leave out what another file already declares,
  such as the functions `lib/kernel` owns.
- **Members.** A `char` member or array of them is `u8`, a function pointer is `extern "C" fn(…)`, and a pointer to a
  typedef of one, such as a table, is a pointer to the typedef's name.

The C compiler still checks every generated line against the header when the program is built, exactly as for a
block written by hand. The rules are [tooling](../spec/tooling.md) §11 and
[ADR 0095](../spec/decisions/0095-extern-blocks-from-c-headers.md); before ceesafec had the command, the generator
was the script `tools/cbind.py` in CeeSafe's repository, which writes the same file.

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
compiler gives the enum. The type is an integer of its own: `as` converts it to and from `I`, and its
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

The C compiler checks that the enum is the integer declared. That integer depends on the target: GCC and Clang on Linux
and macOS make an enum without negative values `unsigned int` (`u32`), as this kernel's is, and one with a negative
value `int` (`i32`), while on Windows every enum is `int`. A header meant for every target gives its enum a negative
value, so that it is `i32` everywhere. The rules are [ADR 0079](../spec/decisions/0079-imported-c-enums.md) and
type-system.md §31.6.

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

**Read-only tables.** A plain `static` of a C type need not be exported: a table of C functions, an operations
struct private to its file, or an array of them is C's `static const` object, placed in read-only data. A C function
pointer is `Share`, so an array of them is an ordinary static, and a static's array may give its elements by index
(`[i: e, j: f]`), the rest zero. A type with an atomic member is not read-only and
stays a `static mut` ([ADR 0091](../spec/decisions/0091-read-only-c-objects.md)). A plain `static` of integers or
`bool`s, or an array of them, is `static const` in C as well
([ADR 0098](../spec/decisions/0098-plain-statics-of-scalar-data-are-const.md)):

<!-- ceesafe: check -->
```ceesafe
@header("kernel/process.h")
extern "C" {
    struct syscall_args {
        a: [u64; 6];
    }
    @ctype("syscall_fn")
    struct syscall_fn;
    struct personality {
        name: *CChar;
        table: *syscall_fn;
        count: u32;
    }
    fn sys_exit(a: *mut syscall_args) -> i64;
}

/// Safety: the dispatcher's.
@export("sys_getpid")
unsafe fn getpid(a: *mut syscall_args) -> i64 => 1;

/// `static const syscall_fn native_table[4]` in C: each entry by its number, as C's `[SYS_exit] = sys_exit`. Entries
/// 0 and 2 are not named, so they are null.
static TABLE: [extern "C" fn(*mut syscall_args) -> i64; 4] = [1: sys_exit, 3: getpid];

/// `extern const struct personality personality_native;` in C.
@export("personality_native")
static NATIVE: personality = personality {
    name: "native\0".ptr as *CChar,
    table: borrow TABLE as *[extern "C" fn(*mut syscall_args) -> i64; 4] as *syscall_fn,
    count: 4,
};
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

**Code only some builds have.** `if (cfg("CONFIG_DEBUG")) { … } else { … }` is C's `#if CONFIG_DEBUG`: a
build defines the macro with `--cflag -DCONFIG_DEBUG=1` or in a header, and one that does not takes it as 0. A member or a C
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

## Embedding in a host program

A program whose `main` is not CeeSafe's (a C, C++ or Rust program calling CeeSafe exports) links CeeSafe as a static
library. `ceesafec build --emit lib -o build/libtext.a` writes one archive holding the generated code and the runtime,
one member per C file; with `--cflag -flto=thin` the members are bitcode and join the host's ThinLTO. Link one archive
per host program. Beside the archive it writes the headers: `build/ceesafe_exports.h` declares the exports, and
`build/ceesafe/embed.h` is the runtime's interface for a host. A package with a CeeSafe `main` gets no C `main` in the
archive, since the host has its own (`--lib-main` keeps it).

<!-- ceesafe: check -->
```ceesafe
/// The number of bytes of `text` that are ASCII letters.
/// Safety: the host passes `length` readable bytes at `text`.
@export("text_letters")
fn letters(text: *u8, length: usize) -> u64 {
    let bytes = unsafe { Slice.fromRaw(text, length) };
    var count: u64 = 0;
    for (b in bytes) {
        if ((b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z')) {
            count += 1;
        }
    }
    return count;
}
```

The host calls `ceesafe_init` once, before any thread calls CeeSafe; after it, exports may be called from any of its
threads at once. Its config gives the program arguments (copied), the allocation report and failure injection, a
panic hook, and the host's allocator, and `ceesafe_shutdown` prints the report and returns the blocks still live:

```c
#include "ceesafe/embed.h"
#include "ceesafe_exports.h"
#include <stdio.h>
#include <stdlib.h>

static void on_panic(void *context, const ceesafe_panic_info *info) {
    (void)context;
    fprintf(stderr, "panic: %s at %s:%u\n", info->reason, info->file ? info->file : "(runtime)", info->line);
    abort(); /* a hook must end the process or never return */
}

int main(int argc, char **argv) {
    ceesafe_config config = {0};
    config.size = sizeof config;
    config.flags = CEESAFE_INIT_ENVIRONMENT; /* CEESAFE_ALLOC_REPORT and CEESAFE_FAIL_ALLOC_AT */
    config.argc = argc;
    config.argv = (const char *const *)argv;
    config.panic_hook = on_panic;
    if (ceesafe_init(&config) != 0) return 1;
    printf("%llu\n", (unsigned long long)text_letters((const uint8_t *)"a1b2", 4));
    return ceesafe_shutdown() == 0 ? 0 : 1; /* 1 if blocks leaked under a report */
}
```

`cc -I build host.c build/libtext.a -o host` links it. A panic calls the hook on the thread that panicked, with the
reason, the message and the CeeSafe location; threads that panic at once are in the hook at once, so a hook that
writes a report takes a lock first. The hook must not return into CeeSafe, call an export, `longjmp` out or throw:
CeeSafe's frames have no unwinding, so after a panic that thread may not call CeeSafe again, and an export called on
it prints the panic and aborts. Without `ceesafe_init` everything still works, but the panic is only printed, the
environment's settings are ignored and `Process.argument` has no arguments
([ADR 0088](../spec/decisions/0088-embedding-ceesafe-in-a-host-program.md)).

A host with its own allocator (mimalloc, an arena) gives it in `config.allocator`: four functions with malloc's
contracts and a context, called from any thread. Every block CeeSafe and its runtime allocate then comes from it,
and the report and failure injection work as before: an injected failure never reaches it. Its blocks must be aligned
to 16 bytes (more than `max_align_t` promises on macOS arm64 and under MSVC); one that is not stops the program
with a panic that names the function. Install it before CeeSafe allocates anything, or `ceesafe_init` returns
`CEESAFE_INIT_LATE`. Each side still frees what it allocated: a block CeeSafe hands out goes back through an export.

```c
/* mimalloc's process-wide functions: any thread may call them (a mimalloc heap belongs to one thread). */
static void *my_alloc(void *context, size_t size) { (void)context; return mi_malloc_aligned(size, 16); }
static void *my_zeroed(void *context, size_t size) { (void)context; return mi_zalloc_aligned(size, 16); }
static void *my_realloc(void *context, void *p, size_t size) { (void)context; return mi_realloc_aligned(p, size, 16); }
static void my_free(void *context, void *p) { (void)context; mi_free(p); }

static const ceesafe_allocator mimalloc_allocator = {my_alloc, my_zeroed, my_realloc, my_free, NULL};
/* in main, before ceesafe_init: */
config.allocator = &mimalloc_allocator;
```

### Buffers that change sides

A C function that fills a buffer writes into a vector's spare capacity: `reserve`, write through the view's pointer,
then `setLength`. Nothing is filled twice. What is written above the length is lost if the vector grows before
`setLength`.

To return a buffer the host will own, give it up with `Vector.intoForeign`; to work on a vector the host owns, take
it with `Vector.fromForeign` and give it back the same way. Both need one allocator on both sides (the host's, given
to `ceesafe_init`, or the C library's when none was given) and a Copy element type, and both are unsafe: the block
leaves, or joins, the blocks CeeSafe must free. A value whose destruction runs code still goes back through an export.

<!-- ceesafe: check -->
```ceesafe
extern "C" {
    /// Writes up to `room` bytes at `to` and returns how many.
    fn source_read(to: *mut u8, room: usize) -> usize;
}

/// Reads up to 4096 bytes more into `out`.
/// Safety: `source_read` writes at most `room` bytes from the pointer, inside the capacity `reserve` made, and
/// `setLength` covers exactly the bytes it wrote.
fn readMore(out: borrow mut Vector<u8>) -> Result<usize, AllocError> {
    out.reserve(4096)?;
    let at = out.length;
    let room = out.capacity - at;
    let written = unsafe { source_read(out.viewMut().ptr.add(at), room) };
    unsafe { out.setLength(at + written); }
    return Ok(written);
}

/// Appends to the host's vector: take, work, give, with no early return between. False when out of memory; the
/// bytes read before that are kept.
/// Safety: the three pointers hold the parts of a block of the allocator both sides share, given up by the host for
/// the call; the host owns the block again when this returns.
@export("source_read_into")
fn readInto(pointer: *mut *mut u8, length: *mut usize, capacity: *mut usize) -> bool {
    var out: Vector<u8> = unsafe { Vector.fromForeign(pointer.read(), length.read(), capacity.read()) };
    let ok = readMore(mut out).isOk();
    let parts = unsafe { Vector.intoForeign(out) };
    unsafe {
        pointer.write(parts.pointer);
        length.write(parts.length);
        capacity.write(parts.capacity);
    }
    return ok;
}
```

In a Rust host the shim takes the caller's `Vec` apart and rebuilds it from what comes back. A `Vec` with capacity 0
has a dangling pointer, so it is passed as capacity 0, and a returned capacity 0 has a null pointer:

```rust
unsafe extern "C" {
    fn source_read_into(pointer: *mut *mut u8, length: *mut usize, capacity: *mut usize) -> bool;
}

pub fn read_into(out: &mut Vec<u8>) -> bool {
    let mut taken = core::mem::ManuallyDrop::new(core::mem::take(out));
    let (mut pointer, mut length, mut capacity) = (taken.as_mut_ptr(), taken.len(), taken.capacity());
    // SAFETY: CeeSafe takes the block with Vector.fromForeign and gives one back with Vector.intoForeign.
    let ok = unsafe { source_read_into(&mut pointer, &mut length, &mut capacity) };
    if capacity != 0 {
        // SAFETY: a block of the allocator both sides share, with `length` initialised bytes.
        *out = unsafe { Vec::from_raw_parts(pointer, length, capacity) };
    }
    ok
}
```

This is outside what Rust documents for `Vec::from_raw_parts`: the block was not allocated by Rust with the `Vec`'s
layout. The allocator defines it, so it holds only for a `#[global_allocator]` that is the allocator CeeSafe was
given, frees by address alone, and reallocates correctly for any layout no larger than the block and aligned to at
most 16 (mimalloc and the C library's `malloc` on Unix do). The hook's `realloc` must also return a 16-aligned block
for a less-aligned one it is given, since a small `Vec<u8>`'s block may be aligned to 8: `mi_realloc_aligned(p, n,
16)` does. A CeeSafe panic aborts the process, so nothing unwinds past a taken vector
([ADR 0090](../spec/decisions/0090-buffers-that-cross-a-c-boundary.md)).

## Next

[Kernel code](kernel.md).
