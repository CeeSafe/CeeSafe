# ADR 0075 — The storage engine's findings: durable files, error conversion, an ordered map, checksums, a monotonic clock, `assert`/`panic`, `replace`, arrays as slices, `*r`, static tables, and tooling

- Status: **Accepted** by the owner, 2026-11-06 ("I want you to take over ~/dev/CeeSafe session and fix all the
  findings"). The findings come from the CosmoStore port (`CeeSafeApps/cosmostore/CEESAFE_GAPS.md`, G1–G22). Each
  open finding was reproduced with ceesafec 0.18.2 on 2026-11-06 (§1). The owner did not answer §6 one by one. It is
  decided as proposed, as ADR 0074 §6 was, and is open to revision before the release.
- Scope:
  - type-system.md §6, §13.3, §16, §17.4, §23, §26.3, §26.10, §26.11, §39.4;
  - spec/api/core.md and spec/api/std.md;
  - diagnostics.md;
  - the parser, checker, NIR lowering, C backend, interpreter and runtime;
  - lib/core and lib/std;
  - `ceesafec test --sweep alloc` and the package C build;
  - guide/builtins.md, guide/for-agents.md and templates/AGENTS.md;
  - the corpus and tests/support.

## 1. Context

CosmoStore is an S3-compatible object store whose storage core (a journal, extents, checkpoints and crash
recovery) is written in CeeSafe 0.18.1. To exist at all, it needed 130 lines of C (`durable_shim.c`). It also logged
22 findings. One is already fixed: `HashKey` for `String` and `Vector<u8>` (`lib/std/collections.cee:179`, `:187`).
The rest reproduce on 0.18.2:

| # | Today (0.18.2) | The port's workaround |
|---|---|---|
| G1–G5 | `File` opens for reading, or creates **truncating**; no positional I/O, sync, length, truncate, rename or directory sync | `durable_shim.c` and a wrapper |
| G6 | `AsyncFiles` has only `open`, `create`, `read` and `write` | the journal is synchronous |
| G7 | `r.isOk()` is N0234; `?` from `Result<_, A>` into `Result<_, B>` is N0261 | a generic adapter per error type (`jio(f.size())?`) |
| G8 | no ordered map | a full scan and sort for every LIST |
| G9 | no CRC32C, CRC64NVME, MD5, SHA-256 or HMAC in the libraries | its own CRC32C and MD5 |
| G10 | only `Timer.now()` in milliseconds | none (no latency histograms) |
| G11 | `m.keyAt(0).name.view()` is N0247 | a local per access |
| G12 | `assert` and `panic` are N0200 | `checkedTruncate<u32>(x)!` as an assertion |
| G13 | `[0u8; 0]` is N0259 | `buf[0..0]` |
| G14 | `[u8; 8]` has no `readLE`/`writeLE` (N0234) | `let w = borrow mut h[..]; w.writeLE(…)` |
| G15 | `--sweep alloc` counts a test's own `!` as a failure (580 of 639 runs) | read the noise by hand |
| G16 | a package's C file is compiled from a temporary copy, so `#include "x.h"` beside it fails | `include-dirs` |
| G17 | a borrow error elsewhere in a function also reports N0265 "cannot infer `T`" | none |
| G18 | `Vector` has no `insert`/`remove` | sorted vectors by hand |
| G19 | moving a value out of a `borrow mut` place is N0251, and there is no `replace` | `ObjectWriter.finish` copies 128 KiB instead of moving it |
| G20 | `a as u64 < b` is N0100 (`<` read as generic arguments) | parentheses |
| G21 | `[AtomicU64; 4]` in a static is N0259 | a `static mut` table behind `unsafe` accessors |
| G22 | `*r` is N0102 | `let id: u64 = r;` |

## 2. Proposal

### 2.1 Durable files (G1–G5; §26.3, §26.10)

`File` gains three ways to open, positional I/O, two syncs, a length and a truncate. `File.rename` and
`Directory.sync` are lib/std functions.

```ceesafe
var journal = File.openOrCreate(path)?;           // read and write; created if missing, never truncated
journal.writeAt(record.view(), offset)?;          // every byte at `offset`, or an error
journal.syncData()?;                              // the data, and the metadata needed to read it back
let n = journal.readAt(mut buffer[..], offset)?;  // bytes read; 0 at or past the end
journal.setLength(validEnd)?;                     // truncate a torn tail
File.rename(temporary, final)?;                   // atomic replace
Directory.sync(directory)?;                       // make the new names durable
```

- **Opening.**

  | Function | Access | Missing file | Existing file |
  |---|---|---|---|
  | `File.openReadWrite(path)` | read and write | `NotFound` | opened as is |
  | `File.openOrCreate(path)` | read and write | created | opened as is |
  | `File.createNew(path)` | read and write | created | `AlreadyExists` |

  - A file is created with mode 0666 & ~umask.
  - On Windows, every open shares read, write and delete.
- **Methods.** Each takes `borrow mut this` and returns `Result<…, IoError>`. `EINTR` is retried.
  - `f.readAt(mut buffer: SliceMut<u8>, offset: u64) -> usize`: one positional read (`pread`; `ReadFile` with an
    `OVERLAPPED` offset on Windows). It may return less than the buffer, and returns 0 at or past the end.
  - `f.writeAt(bytes: Slice<u8>, offset: u64) -> void`: positional writes until every byte is written.
  - `f.syncData() -> void`: the file's data, and the metadata needed to read it back, reach stable storage.
    - Linux: `fdatasync`.
    - macOS and iOS: `fcntl(F_FULLFSYNC)`, because `fsync` there does not flush the drive's cache. It falls back to
      `fsync` only where the file system refuses `F_FULLFSYNC` (`ENOTSUP`, `EINVAL`).
    - Other POSIX systems: `fsync`.
    - Windows: `FlushFileBuffers`.
  - `f.syncAll() -> void`: the same, with all metadata (`fsync`, or `F_FULLFSYNC` on Apple systems).
  - `f.length() -> u64`: `fstat`'s size.
  - `f.setLength(n: u64) -> void`: `ftruncate` (`SetFileInformationByHandle` on Windows), shrinking or
    zero-extending.
  - **A failed sync leaves the file's durable contents unknown.** After a failure, Linux may already have marked the
    dirty pages clean. A later successful sync does not mean the earlier writes reached the disk. A program that
    needs durability must treat the file as failed: rewrite it, or recover from what is known to be durable. The
    spec says this in so many words, and so does the guide.
  - `read` and `write` keep their sequential meaning on every `File`.
- **`File.rename(from, to)`** (lib/std): `rename(2)`, which atomically replaces `to`. On Windows it is
  `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`.
- **`Directory.sync(path)`** (lib/std): on POSIX, `open(O_RDONLY | O_DIRECTORY)` then `fsync`, which makes the
  directory's entries (creations, renames, removals) durable. It is `Ok` on Windows, where NTFS journals directory
  metadata and a directory handle cannot be flushed. The spec records this.
- **Runtime.** The C functions, each returning a value or a negated errno like the rest of
  `runtime/hosted/ceesafe_rt_io.c`:
  - `ceesafe_file_open`, with new modes 2, 3 and 4;
  - `ceesafe_fd_read_at`, `ceesafe_fd_write_at`, `ceesafe_fd_sync`, `ceesafe_fd_length`, `ceesafe_fd_set_length`;
  - `ceesafe_host_rename`, `ceesafe_host_dir_sync`.
- **Effects.** All of these have `sleep`, as `File.open` does. In freestanding code they are N0293, as `File` is.
- **The interpreter** runs them on the host file system, as it runs `File.open`. Its sandbox refuses them, and it
  counts a sync as done.

### 2.2 Async files (G6; spec/api/std.md `AsyncFiles`)

`AsyncFiles` gains `openReadWrite`, `openOrCreate`, `createNew`, `readAt`, `writeAt`, `syncData`, `syncAll`,
`length`, `setLength`, `rename` and `syncDirectory`, with §2.1's meaning. Each runs its blocking call on a worker
(`Async.blocking`), as `AsyncFiles.read` does today. io_uring's `IORING_OP_FSYNC` and positional reads are an
executor change for a later ADR. The API does not depend on how the work is done.

### 2.3 Results and conversion in `?` (G7; §17.4, spec/api/core.md)

- **Methods of `Result<T, E>`** (lib/core):
  - `r.isOk()` and `r.isErr()` take `borrow this`;
  - `r.ok() -> T?` and `r.err() -> E?` consume `r`;
  - `r.map(f)` and `r.mapErr(f)` consume `r` and call `f` with the payload.
- **Methods of `T?`:** `o.okOr(e) -> Result<T, E>` consumes `o`.
- **Conversion in `?`.** A variant of a user enum whose single payload is the error type `A` may be marked `@from`:

  ```ceesafe
  enum StoreError {
      @from Io(IoError),
      @from Alloc(AllocError),
      Corrupt,
  }
  fn open(path: StringView) -> Result<Store, StoreError> {
      var f = File.openReadWrite(path)?;      // an IoError becomes StoreError.Io(e)
      …
  }
  ```

  - In a function returning `Result<U, B>`, `e?` with `e: Result<T, A>` is allowed where `A` is `B`, or where `B`
    is an enum with an `@from` variant whose payload type is `A`. The `Err(x)` case then returns
    `Err(B.Variant(x))`.
  - At most one `@from` variant per payload type (N0262 otherwise; the diagnostic names both). A `@from` variant
    must have exactly one payload (N0262).
  - The conversion goes one step only, never `A → B → C`.
  - It applies to `?` only. `return Err(x)` still needs `B.Variant(x)`, so a construction always says what it
    builds.
  - N0261 now says which `@from` variant would fix the mismatch.
- **Why an attribute, not an interface.** A `From`-style interface for `IoError → StoreError` would have to live in
  the package that declares either `IoError` or the interface. That works here, but an interface adds a call the
  reader cannot see, and generic code would have to resolve it per instance. A variant is visible where the error
  type is declared, costs nothing at run time, and is checked once.

### 2.4 An ordered map (G8; spec/api/std.md)

lib/std gains `OrderedMap<K, V>`, a B-tree:
- It is built over an arena of nodes (a `Vector` of nodes, linked by index), so it uses no `unsafe`.
- Each node has at most 2B − 1 keys, for a fixed B of 16.
- Keys implement a new interface, `OrderKey<K>`, with `fn compare(borrow this, other: borrow K) -> i32` (negative,
  zero or positive). lib/std implements it for the integers, `bool`, `String` and `Vector<u8>` (bytewise).
- `create()`.
- `put(key, value) -> Result<V?, AllocError>`: the old value, if any.
- `get(borrow key) -> borrow V?` and `getMut(borrow key) -> borrow mut V?`.
- `contains(borrow key) -> bool`.
- `remove(borrow key) -> V?`, which rebalances: borrow from a sibling, else merge.
- `length() -> usize` and `isEmpty() -> bool`.
- `clear()`.
- **Ordered access with cursors.** A cursor is a plain value: a node, a slot and the map's version.
  - `first()`, `last()`, `seek(borrow key)` (the first entry ≥ key) and `seekAfter(borrow key)` return an
    `OrderedCursor?`.
  - `next(cursor) -> OrderedCursor?` and `previous(cursor) -> OrderedCursor?` step it.
  - `keyAt(cursor) -> borrow K` and `valueAt(cursor) -> borrow V` read through it, and `valueAtMut` writes.
  - **A cursor used after the map changed panics** ("ordered map changed under a cursor"): a mutation bumps the
    version. A cursor cannot outlive the map's validity silently, and the borrow checker needs no new rule.
  - A range scan is `seek(from)` followed by `next` until `keyAt(c).compare(to) >= 0`.
- Iteration order is the keys' order, so output never depends on insertion order or hashing.

### 2.5 Checksums and hashes (G9; spec/api/std.md)

lib/std gains, in pure CeeSafe:
- `Crc32c`: Castagnoli, reflected, slicing-by-8;
- `Crc64Nvme`: the CRC-64/NVME of the S3 checksum headers;
- `Md5`;
- `Sha256`;
- `HmacSha256`.

Each is incremental: `create()`, `update(bytes)`, then `finish()`, which returns a `u32`/`u64`, or a `[u8; 16]`/`[u8; 32]`
digest. `Crc32c.of(bytes)` and the like compute it in one call. The CRC tables are generated (`tools/gen_checksums.py`),
as the other tables are. MD5 is for ETags and Content-MD5 only, and the API doc says it is broken as a signature.
Hardware CRC (SSE4.2 `crc32`, ARMv8 `crc32c`) needs intrinsics, which CeeSafe does not have. It is left to a later
ADR, and the API will not change. Test vectors come from Python (`hashlib`, `hmac`, and an independent CRC model in
tools/oracles).

### 2.6 A monotonic clock in nanoseconds (G10; §26.11)

`Clock.monotonicNanos() -> u64` (lib/std): nanoseconds on the monotonic clock, from an arbitrary start.
- POSIX: `clock_gettime(CLOCK_MONOTONIC)`.
- Apple systems: `clock_gettime_nsec_np(CLOCK_UPTIME_RAW)`.
- Windows: `QueryPerformanceCounter`, scaled.

It never decreases within a process. The interpreter answers its virtual clock in nanoseconds. `Timer.now()` keeps
its milliseconds.

### 2.7 A field of a returned reference is a place (G11; §13.2)

A call whose result is a reference is a reference expression. A field reached through it, `m.keyAt(0).name`, is
"a place reached through a reference", which N0247's text already allows. So `m.keyAt(0).name.view()` borrows
`name` through `keyAt`'s result, and the loan is the call result's loan. The same holds for a method result
(`e.f()`) and for chains (`a.b().c.d()`).

### 2.8 `assert` and `panic` (G12; §8)

- `panic(message: StringView) -> never` panics with "panicked: " and the message. Like every panic, it reports the
  source position and aborts.
- `assert(condition)` panics with "assertion failed" when `condition` is false. `assert(condition, message)` uses
  the message instead.
- These are built-in function names, so a user function named `assert` or `panic` shadows them, with no warning.
- Assertions are always checked: there is no release mode that drops them, so a build does what its tests checked.
- The runtime gains `CEE_PANIC_ASSERT` and `ceesafe_panic_message(position, text)`.

### 2.9 Empty arrays (G13; §16)

`[T; 0]` is a type and `[e; 0]` a value: the empty array.
- Its size is 0. No C storage is emitted for it: a zero-length array member is left out of its struct, and a local
  is a `uint8_t` placeholder that nothing reads.
- `a[..]` is an empty slice, and any `a[i]` panics.
- `e` in `[e; 0]` is still evaluated and then destroyed, because it is an expression with effects.
- `[T; 0]` in a `@repr(C)` struct or an `extern "C"` signature is N0259, because C has no zero-length arrays.
- `Slice<T>.empty()` and `SliceMut<T>.empty()` are also added: the empty slice, with no array behind it.

### 2.10 An array's methods are its slice's (G14; §16)

A method call on an array place whose name is not an array method calls the slice method on `a[..]`. It borrows
mutably when the method needs a `SliceMut`, and needs a mutable place for that, as `a[..]` does. So `h.writeLE(0, x)`
and `h.readLE<u32>(4)` work on `[u8; 64]`, as do `sortBy`, `swap` and the rest.

### 2.11 Moving out of a mutable place: `replace` and `swap` (G19; §14)

- `replace(mut place, value) -> T` puts `value` in `place` and returns the old value.
- `swap(mut a, mut b)` exchanges two places of the same type.
- Both are built-in functions. The place is never left empty, so neither changes any move rule. They work for
  every type, including a field of a struct that has a `drop` block, which is how a consuming `finish` gives up a
  buffer:

  ```ceesafe
  fn finish(this: borrow mut ObjectWriter) -> Vector<u8> => replace(mut this.inline, Vector<u8>.create());
  ```

- `swap`'s two places are two mutable loans at once, so `swap(mut a.x, mut a.x)` is N0241 as always. For two
  elements of one slice there is `s.swap(i, j)`.
- Lowering: `T old = *p; *p = v;` in C, with `v` moved in. Drop elaboration sees a move into `*p` and a fresh
  value out.

### 2.12 `Vector.insert`, `remove` and `swapRemove` (G18; §23)

- `v.insert(at, x) -> Result<void, AllocError>` shifts later elements up. `at == length` appends.
- `v.remove(at) -> T` shifts later elements down.
- `v.swapRemove(at) -> T` moves the last element into `at`, which is O(1) and does not keep the order.
- `at` past the end panics (index out of bounds). The shifting is a `memmove` in C and a loop in the interpreter.

### 2.13 `as T <` (G20; grammar.md)

After `as`, a primitive type name (`u8` … `u64`, `i8` … `i64`, `usize`, `isize`, `f32`, `f64`, `bool`, `char`)
takes no type arguments, so a `<` after it is the comparison operator: `a as u64 < b` is `(a as u64) < b`.

For a type that does take type arguments, `as Box<T>` is still parsed as written. If its `<` cannot start type
arguments, N0100 now adds "write `(e as T) < x` for a comparison".

### 2.14 Prefix `*`: reading through a reference (G22; §13.3)

`*r` where `r: borrow T` or `borrow mut T`, with `T` a Copy type, is the value `r` refers to. It is a copy, of type
`T`, so `let id = *m.keyAt(0);` declares a `u64`.
- `*r` on a reference to a non-Copy type is N0251. The diagnostic says to borrow or `replace`.
- `*` on a non-reference is N0205.
- Assigning through `*r = v` is accepted where `r` is a `borrow mut T`, the same as `r = v` today.
- Raw pointers keep `p.read()` and `p.write(v)`. `*p` on a raw pointer is N0205, which points to them, because a
  raw read is `unsafe` and must look different.
- The binary `*` is unchanged: a prefix `*` is one with no left operand.

### 2.15 Static tables (G21; §39.4)

- **Arrays of non-Copy elements, in statics only.** A static's type may be `[T; N]` with a non-Copy `T` that has no
  `drop` block (atomics, structs of atomics). Its initialiser must be an array literal or `[e; N]` where `e` is a
  static initialiser. `[e; N]` there means N evaluations of `e` at compile time, not a copy, so it needs no Copy.
- `S[i]` is a place, and methods borrow through it (`SLOTS[i].fetchAdd(1)`). It cannot be moved out of, as no static
  can.
- Outside statics, arrays of non-Copy elements stay N0259. Locals, fields and parameters would need per-element drop
  flags, which is a separate decision.
- **`Mutex<T>.constant(value)`** (lib/std, `@constant`) for a `T` with a static initialiser, and
  **`Vector<T>.empty()`**, `@constant`: an empty vector owns no memory, so it is a static initialiser. Together they
  allow `static TABLE: Mutex<Vector<u64>> = Mutex<Vector<u64>>.constant(Vector<u64>.empty());`.
  - A static's `Mutex` is never destroyed. Its vector's memory is reclaimed at process exit, so the leak checker
    does not count a static's allocations.

### 2.16 Tooling and diagnostics (G15, G16, G17)

- **G15.** `ceesafec test --sweep alloc` puts each injected failure into one of three classes:
  - **handled**: the test passed;
  - **unwrapped by the test**: the panic is `!` or `?`'s unwrap at a position inside an `@test` function's own
    body (as implemented, in any file that declares an `@test` function: see "Changes on implementation", item 6;
    tooling.md §10.2 states the rule);
  - **failed**: any other panic, a leak, a crash or a wrong result.

  Only *failed* runs fail the sweep. The report shows the count of each, and lists the failed ones.
  `--sweep-strict` keeps today's behaviour.

  *Amended by ADR 0129:* the second class is an unwrap at a position in test-only code, which is an `@test` function
  or a file of a test directory; the by-file rule of item 6 is withdrawn.
- **G16.** A package's C files are compiled with their own source directory first on the quoted include path
  (`-iquote <dir>`; `/I<dir>` on MSVC), so a header beside a `.c` file is found. They are still compiled from the
  build copy. guide/packages.md says so.
- **G17.** A call whose arguments include an error expression no longer reports N0265 (cannot infer `T`), and no
  inference diagnostic comes from an error type. The first error is the one shown.

## 3. Not included

- io_uring for files (§2.2) and hardware CRC (§2.5): each needs its own design, and neither changes the API.
- Arrays of non-Copy elements outside statics (§2.15).
- An interface-based `From` (§2.3).
- `take(mut place)` for types with a default: CeeSafe has no `Default` interface. `replace(mut p, X.create())` says
  the same thing.

## 4. Tests

Each section gets run-pass and check-fail corpus programs. Every new library function gets tests/support programs
whose expected values come from Python oracles: hashes, CRCs and the ordered map against a sorted dict model. The file
API gets a hosted test that writes, syncs, truncates, reopens and renames. The tests run with `--check-leaks`, and
the interpreter must give the same results.

## 5. Consequences

- CosmoStore can delete `durable_shim.c` and the error adapters, and its fault registry no longer needs `unsafe`.
  It can also use the library CRC32C and MD5.
- Programs that already have their own functions named `assert`, `panic`, `replace` or `swap` keep them, since a
  local name wins (§2.8). There are no other source changes.

## 6. Questions, decided as proposed

1. Release: one release, 0.19.0, since the file API, `@from` and the statics are new features.
2. `@from` over a `From` interface (§2.3).
3. A cursor used after a mutation panics, rather than giving the borrow checker a new rule (§2.4).
4. `Directory.sync` is `Ok` on Windows (§2.1).
5. Assertions are always checked (§2.8).

## 7. As built (2026-11-06): changes from the accepted text

1. **§2.8.** The panic message has the existing format: `ceesafe panic: <message> at <file>:<line>:<column>`, not
   "panicked: …". `assert` without a message says "assertion failed". The runtime gains `ceesafe_panic_text` and
   `CEE_PANIC_EXPLICIT`. A freestanding environment still defines only `ceesafe_panic`, so the freestanding contract
   (profiles.md §4.2) is unchanged.
2. **§2.9.** `[T; 0]` is not added. C has no zero-sized object, so the type would be laid out one way in C and another
   in CeeSafe (offsetOf, the interpreter). The empty slice is spelled `[]` where a `Slice<T>` or `SliceMut<T>` is
   expected, rather than `Slice<T>.empty()`. That is what the finding asked for (`buf[0..0]`).
3. **§2.3.** `map` and `mapErr` are left out. A generic method in a generic struct cannot name the struct's type
   parameters yet (`unknown type 'T'`), and that is a checker change of its own. `isOk`, `isErr`, `ok`, `err`, `okOr`
   and `@from` are in. `@from` is recorded per `?` expression in a checker side table, so `HirExpr.Try` keeps its
   shape.
4. **§2.12.** The shifting is a loop of `swap`s in lib/core's `VectorEditing<T>`, not a `memmove`. That keeps it out
   of the C backend and the interpreter. The index check is emitted at the call, as `ByteOrder`'s is, so a panic
   names the program's line.
5. **§2.15.** `Mutex<T>.constant` and `Vector<T>.empty()` are left out. `Vector` is a built-in type, and making it a
   static initialiser is its own change. Static arrays of atomics are in, and they are what CosmoStore's registry
   needs.
6. **§2.16, G15.** A sweep panic counts as the tests' own when it is an unwrap in any file that declares an `@test`
   function, not only inside an `@test` body. Test helpers live in those files: CosmoStore's sweep went from 63
   failures in 200 runs to 0, all 63 being helpers' and tests' unwraps. `ceesafec test` writes `test-spans.txt` for
   this. `--sweep-strict` keeps the old count. *Withdrawn by ADR 0129:* the rule exempted production code whose
   file holds tests; a helper is test-only when it is in a file of a test directory.
7. **§2.16, G16.** The headers beside a package's C file are copied with it into the build's `c/` directory, instead
   of an `-iquote` flag, so `--emit c`'s output stays self-contained. Two different headers of one name are an error.
8. **§2.4.** `OrderKey`'s method is `compareKey`, not `compare`. A user's `impl Comparable<u8> for u8 { fn compare … }`
   (check-pass/interfaces.cee) would otherwise clash with lib/std's impl on `u8`.
9. **Library names (new, §5).** A package's item may now have a lib/core or lib/std item's name. Before, that was
   N0201, and it would have made every new library name a source change. It would have broken this compiler
   (`src/support/sha256.cee`), CeeSafeLibs' `sha256` package (`Sha256`, `HmacSha256`) and CosmoStore (`Md5`,
   `Crc32c`). The package's item shadows the library's in that package; the libraries still find their own
   (`packageItem` looks in the package first, a library file only in the libraries). In C the library's item is then
   `ceeT_lib_…`, `ceeF_lib_…` and so on (`appendShadowMark`).
10. **Library statics (new).** A library's static is emitted in C only where compiled code uses it (`usedStatics`),
    as a library function is. Otherwise every hosted program would carry the 24 KB of checksum tables. A program's
    own statics are still always emitted.
11. **Corpus.** A new directive, `// expect-library-panic: reason`, covers a panic that a library raises at its own
    line (the stale OrderedMap cursor). spec/testing.md documents it. `parse-fail/expression-errors.cee` line 6 now
    uses `/ 2`, because `* 2` is a dereference (a check error, not N0102). Four snapshots changed only because the
    library grew: the closure numbers in check-pass `async-timeout.hir`, `effects-of.hir` and
    `string-braces-warning.hir`, and the order of two type declarations in `emit-c/slice-bulk.c`. Each was compared
    with numbers normalised (or sorted) before it was regenerated.
12. **The checksum test** leaves out the million-byte "a" message, which took the interpreter 3 minutes (the runner
    times out). It passed in both modes before it was removed, and the 4,096 and 8,193-byte cases still cross many
    blocks.

### Found while building (fixed after 0.19.0, §8)

- **N0272 false positive.** A value of type parameter `T` taken from `this` by a generic call, or a struct literal of
  `Item<T>` built with a Copy field read through `this`, is reported as "a value borrowed from 'this'". Pushing it
  back into `this` is then N0272. The B-tree works around it. Repro: `take<T>(v: borrow mut Vector<T>) -> T` called
  as `let x = take(mut this.a); this.b.push(x)!;` in a method of `struct Two<T>`.
- **N0212 across `?`, kernel profile only.** In lib/core,
  `let length = v.length; v.push(x)?; var i = length;` was "may not have been assigned" when checked with
  `--profile freestanding --kernel cosmoos`, and not otherwise. `VectorEditing.insert` now computes the index after
  the push.

### Tests run (2026-11-06, macOS arm64)

- `python3 tools/run_corpus.py --compiler out/ceesafec-new --corpus tests/reference/tests/corpus --jobs 8`: every
  family passes (freestanding is "partial" as before). The new programs are
  run-pass `adr0075-language`, `durable-files`, `checksums` and `ordered-map`; run-fail `panic-message`,
  `assert-failed`, `vector-remove-past-end` and `ordered-map-stale-cursor`; and check-fail `adr0075-errors`. Each
  also passes with `--interp`.
- Fixed point: the new compiler built itself (gen2), gen2 built gen3, and their `--emit c` output is identical.
- `checksums` and `ordered-map` expectations come from Python (hashlib, hmac, tools/oracles/crc_model.py, and a
  sorted-dict model in tools/oracles/gen_ordered_map_cases.py).
- `python3 tools/contract_tests.py --ceesafec out/ceesafec` (gen2): every suite passes; Linux-only checks were skipped
  (fortify, io_uring). The suites are the fixed point, -Werror, asan, tsan, interp, the support, hashmap, fault and
  cancel sweeps, mutations, packages, spec, guide, debugger and mcp api. The corpus's one failure was
  check-fail/systems-capabilities.cee expecting N0201 for a `PhysAddr` of the program's own. That is §7.9's change:
  the expectation was edited (a reviewed change, noted on the line) and type-system.md §2, §26.8, §29.2 and §38.5 now
  say so.
- `ceesafec check` is clean in all 44 packages of CeeSafeLibs and CeeSafeApps. Their 24 test suites pass, and so
  does CosmoStore's (54 tests).

## 8. Amendment (2026-11-06): the two bugs of §7, fixed after 0.19.0

The owner: "Fix the two bugs first, then migrate CosmoStore".

1. **N0272 on a referent's own contents.**
   - **Callee.** A result whose type is a type parameter (`fn take<T>(v: borrow mut Vector<T>) -> T`) was tied to
     `v` by elision, so it carried `v`'s own loan. `T` cannot name the reference's elided region, so each such
     reference parameter is now a **deep** source: the result carries only what the referent holds
     (`src/check/references.cee`; type-system.md §25.6).
   - **Store.** Rule 2 of §25.7 made a write through `borrow mut p` take only loans of parameters that flow into
     `p`, and a parameter never flows into itself. Now `p`'s deep placeholder may go back into `p`: what the referent
     held may return to it (`checkParameterWrites`, `src/nir/borrow.cee`). The loan of `p` itself (its outer
     placeholder) and loans of `p`'s own storage still may not.
   - **Tests.** run-pass `generic-values-back-into-this.cee` (both of §7's repros; 0.19.0 rejects it) and check-fail
     `values-into-this-still-checked.cee` (a view of `this.data` stored into `this`, and another parameter's view,
     still N0272).
2. **N0212 under the kernel profile.** In a freestanding build, a lib/core function over `Vector` gets error types,
   and its N0293s are dropped with the libraries' other diagnostics (main.cee). Nothing was then left to stop the
   ownership analyses, which reported the consequences: `let length = v.length; v.push(x)?; var i = length;` was "may
   not have been assigned". Library functions with an error inside them are now recorded before those diagnostics are
   dropped (`markUncheckedLibrary`, `Checker.uncheckedLibrary`), and the NIR checks skip them. Nothing can call them:
   a caller would have the same type errors. `VectorEditing.insert` is back to that natural form, so the kernel
   corpus programs (check-pass and check-fail `kernel-library`, `kernel-percpu*`, check-fail `async-recursion`) test
   it.
3. **`File`'s durable methods take `borrow this`.** CosmoStore's journal and store read through shared references to
   their files. `readAt`, `writeAt`, `syncData`, `syncAll`, `length` and `setLength` never move the position that
   `read` and `write` use, as in Rust's `FileExt::read_at(&self)`, so they no longer need `borrow mut this`. This is a
   relaxation: code written for 0.19.0 still checks. `AsyncFiles` keeps its `borrow mut File` parameters, since
   loosening a parameter would make its callers' `mut f` arguments N0249.
