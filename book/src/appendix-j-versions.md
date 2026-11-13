---
title: "Appendix J: Versions"
summary: Every release of ceesafec from the newest back to the first, with its date, what it added to the language, the library or the tools, and the sections of the book that cover it.
keywords: [version, release, ceesafec --version, history, ADR, changelog]
---

# Appendix J: Versions

This appendix lists every release of `ceesafec`, newest first: its date, what it added, and the sections of this
book that teach what it added. Use it when a listing of the book does not compile with your compiler: run
`ceesafec --version`, find the feature's row, and compare. The book assumes ceesafec %ceesafec-version% or later, and
[Installation](./ch01-01-installation) says how to update.

How to read a row:

- **Release** is the version that `ceesafec --version` prints, and the release's tag is `v` and that number.
- **Added** names the decision record behind each change, as "ADR" and its number. The records are in the
  specification: https://ceesafe.github.io/CeeSafe/reference/decisions/.
- **Sections** are the sections of this book that use or teach what the release added. A dash means the book does
  not cover it. The guide and the library's pages do: https://ceesafe.github.io/CeeSafe/.
- A feature is in the row of the release that first had it. Later fixes to it are not listed, except where a
  release holds nothing else.

A package can state the oldest compiler it builds with, as `ceesafec = ">=0.42.0"` in its manifest, and an older
compiler then refuses it with a message that names the version it needs.
[Dependencies and the lock file](./ch07-04-dependencies) covers that line.

## Releases 0.25.0 to 0.43.0

| Release | Date | Added | Sections |
|---|---|---|---|
| 0.43.0 | 2026-11-14 | Struct patterns, `P { x, var y, z: PATTERN, .. }`, in `match`, `is`, `let … else` and a plain `let`, and a struct with a `drop` block taken apart in its own file by a pattern that binds every field (ADR 0123). `ProcessSignals` for Ctrl+C and `SIGTERM` (ADR 0125), and `TcpListener.shutdown()` to end a waiting `accept` (ADR 0126). `ceesafec cbind` writes an `extern "C"` block from a C header (ADR 0127). `AsyncError.Closed`, and `?` on a send's error through an `@into` method (ADR 0128). The bounds of an inherent `impl<T: B> S<T>` block count: its functions exist on the instances that meet them (ADR 0130). A `RefCell` guard held across an `await` has its own warning, N0405 (ADR 0132). A function whose signature names a nested instance of its own type no longer hangs the checker (ADR 0133). Clearer diagnostics for an unknown method, `::` and an unknown generic name; `ceesafec fix` and `fmt` write each file where they read it. | [19.3](./ch19-03-the-syntax-whole), [15.2](./ch15-02-destruction), [21.5](./ch21-05-graceful-shutdown), [20.2](./ch20-02-calling-c), [17.4](./ch17-04-async-io), [10.1](./ch10-01-generic-types) |
| 0.42.1 | 2026-11-13 | A fix to the checker, and no new feature (ADR 0131). 0.42.0 accepted some programs without `unsafe` that read freed memory: an `impl` that lends its own storage through a result that the interface declares as a type parameter, generic code that makes a value of a type parameter's type with a function value or a closure, and a closure that stores what one capture holds through another. A value of a type parameter's type is now followed as a borrow is, and a type parameter bounded by `Send` or `Share` holds no borrow. `Async.tasks` and `Async.tasksIn` need an error type that is `Send`. Three kinds of program are known and not yet refused, and the next release covers them: a borrow stored by the built-ins `replace`, `swap`, `fill`, `copyFrom`, `extend` or `resize`, the value that `replace` returns, and a store through a `borrow mut` that a struct passed by value holds. | [10.2](./ch10-02-interfaces) |
| 0.42.0 | 2026-11-13 | The gaps found while this book was written (ADRs 0105 to 0122). A warning, `N0402`, for a `Result` that nothing reads (0105). `Console.readLine` and `Random.below` (0106). Methods and static functions on enums (0107). `clone` on strings and vectors, and `String.from` (0108). `private` items, private to their file (0109). Arithmetic on literals that would panic is an error, `N0217` (0110). `AsyncError.Io` converts from `IoError`, and `main` is not async (0111). A thread's closure runs once, and `Mutex.tryLock` (0112). A warning, `N0403`, for a `move` closure that changes its own copy (0113). After one field of a struct is moved, its other fields can still be read, and `N0250` notes where the value was moved (0114). The expected type passes through `!` and `?` to `Box.create`, `String.from` and the like, and a `String` that is a call's result prints and lends its text as a variable does (0115). `let PATTERN = e else { … };` (0117). A constant's name as the bound of a range pattern (0118). A package's `include-dirs` reach the generated C (0120). `--emit obj` writes the headers its user needs (0121). `CStr.fromPtr` (0122). | [2](./ch02-00-guessing-game), [3.2](./ch03-02-data-types), [4.1](./ch04-01-what-ownership-is), [6.1](./ch06-01-defining-an-enum), [7.2](./ch07-02-files-and-public), [8.1](./ch08-01-vectors), [8.2](./ch08-02-strings), [9.2](./ch09-02-result), [16.1](./ch16-01-starting-threads), [16.3](./ch16-03-shared-state), [17.1](./ch17-01-tasks-and-await), [17.4](./ch17-04-async-io), [19.1](./ch19-01-where-patterns-go), [19.3](./ch19-03-the-syntax-whole), [20.2](./ch20-02-calling-c), [20.4](./ch20-04-arithmetic), [22.1](./ch22-01-the-freestanding-profile) |
| 0.41.0 | 2026-11-13 | The limit on open files: `ProcessLimits.openFiles`, `ProcessLimits.raiseOpenFiles` and `IoError.TooManyOpenFiles` (ADR 0104). | — |
| 0.40.0 | 2026-11-12 | `partitionPoint` on slices and vectors, a binary search with a closure (ADR 0103). | [8.1](./ch08-01-vectors), [13.2](./ch13-02-iteration) |
| 0.39.0 | 2026-11-12 | No change to the language. The compiler removes more bounds checks: an index updated in place keeps what is known of its bounds, and a reborrowed slice its length (ADR 0102). | [13.4](./ch13-04-what-they-cost) |
| 0.38.0 | 2026-11-12 | No change to the language. `resize` of a vector with no buffer takes zeroed memory (ADR 0099), more bounds checks are removed after equality tests, in binary searches and on resized vectors (ADR 0100), and the system allocator's common path is inline (ADR 0101). | [13.4](./ch13-04-what-they-cost) |
| 0.37.0 | 2026-11-11 | A plain `static` of scalar data is a `const` object in the generated C, so it is placed in read-only memory (ADR 0098). | [22.2](./ch22-02-kernel-code) |
| 0.36.0 | 2026-11-11 | No change to the language or the library. `Async.blocking` runs its closures on a pool of worker threads that each `Async.run` keeps and reuses (ADR 0094). | [17.5](./ch17-05-tasks-and-threads) |
| 0.35.0 | 2026-11-11 | The unsafe pointer methods `readUnaligned` and `writeUnaligned` (ADR 0093). `OnceCell` and `OnceLock`, values written once and then lent (ADR 0096). `impl dep.I for T`, a dependency's interface named with its package (ADR 0097). | [15.4](./ch15-04-cells), [16.3](./ch16-03-shared-state), [20.1](./ch20-01-unsafe-code) |
| 0.34.0 | 2026-11-11 | `tools/cbind.py`, which writes an `extern "C"` block from a C header (ADR 0095). The compiler itself changes only in the documents it embeds. | [20.2](./ch20-02-calling-c) |
| 0.33.0 | 2026-11-11 | Read-only C objects: a plain `static` of C functions, imported structs or raw pointers is C's `const` object, an `extern "C" fn` value is `Send` and `Share`, and an array literal may name its indexes, `[i: e, j: f]`, in a static's initialiser (ADR 0091). | [22.2](./ch22-02-kernel-code) |
| 0.32.0 | 2026-11-11 | `x.mulHigh(y)`, the upper half of a full product, on every integer type (ADR 0092). | [20.4](./ch20-04-arithmetic) |
| 0.31.0 | 2026-11-11 | Buffers that cross to C: `Vector.intoRaw` and `Vector.fromRaw`, the unsafe `setLength` over spare capacity, and the unsafe `Vector.intoForeign` and `Vector.fromForeign` for a buffer a host adopts or gives up (ADR 0090). | — |
| 0.30.0 | 2026-11-11 | No change to the language or the library's names. `Crc32c` uses the processor's CRC-32C instruction where there is one (ADR 0089). | — |
| 0.29.0 | 2026-11-11 | A host's own allocator, given to `ceesafe_init`, takes every block the runtime allocates (ADR 0088, stage B). | [22.3](./ch22-03-inside-a-host-program) |
| 0.28.0 | 2026-11-10 | CeeSafe inside a host program: `build --emit lib` with its headers, `ceesafe_init` and `ceesafe_shutdown`, and a panic hook (ADR 0088, stage A). `check` reads each dependency from a cached record of its interface, with `check --all`, `--package` and `--whole-program`, and `N0364` for a dependency with errors (ADR 0082, stage D). | [14.3](./ch14-03-libraries-for-other-programs), [22.3](./ch22-03-inside-a-host-program) |
| 0.27.0 | 2026-11-10 | `embed(path)` and `embedText(path)`, a file's bytes or text built into the program, with `[embed] roots` in the manifest and `N0362`. `+` on constant string views (ADR 0087, step 2). | [14.1](./ch14-01-build-settings), [21.2](./ch21-02-requests-and-responses) |
| 0.26.0 | 2026-11-10 | Const parameters, values as generic arguments (ADR 0083). String patterns, slice patterns and the operator `is` (ADR 0085, step 2). `debugAssert` and `--debug-assertions`, C string literals `c"…"` and `CStr`, `@derive(Equal, HashKey, OrderKey)`, `==` on arrays, slices and vectors of `Equal` types, and `appendHex`, `appendOctal`, `appendFixed`, `appendPadded` (ADR 0087, step 1). | [6.2](./ch06-02-match), [6.3](./ch06-03-optionals), [8.2](./ch08-02-strings), [8.3](./ch08-03-maps), [14.1](./ch14-01-build-settings), [19.1](./ch19-01-where-patterns-go), [19.3](./ch19-03-the-syntax-whole), [20.2](./ch20-02-calling-c), [20.3](./ch20-03-more-interfaces-and-generics), [20.5](./ch20-05-derives-and-macros) |
| 0.25.0 | 2026-11-09 | Interfaces with default methods, `Self`, static and generic methods, constants, associated types and supertraits, and the interface `Iterator<T>` with `for` over it (ADR 0086). The cells `RawCell`, `Cell` and `RefCell` with their guards, and `dyn … + Share` (ADR 0084). Guards in `match` arms, or-patterns, range patterns and `name @ pattern` (ADR 0085, step 1). | [6.2](./ch06-02-match), [10.2](./ch10-02-interfaces), [13.2](./ch13-02-iteration), [15.4](./ch15-04-cells), [18.1](./ch18-01-dynamic-dispatch), [19.2](./ch19-02-patterns-that-can-fail), [19.3](./ch19-03-the-syntax-whole), [20.3](./ch20-03-more-interfaces-and-generics) |

## Releases 0.10.0 to 0.24.3

| Release | Date | Added | Sections |
|---|---|---|---|
| 0.24.3 | 2026-11-09 | `N0201` is reported at the C declaration that clashes with a dependency's, with a fix (ADR 0081). `build --split` writes one C file for each source file, and `--jobs N` compiles them in parallel (ADR 0082, stage A). | [7.2](./ch07-02-files-and-public), [14.1](./ch14-01-build-settings) |
| 0.24.2 | 2026-11-09 | Fixes to `cfg` blocks in the generated C (ADR 0080). The formatter breaks attribute lists and `else` in its final way. | [14.1](./ch14-01-build-settings) |
| 0.24.1 | 2026-11-08 | An exported or imported atomic static is C's plain integer (ADR 0080). | — |
| 0.24.0 | 2026-11-08 | What a kernel needs from C: exported and imported C data, constants that C checks, `cfg` blocks for code that exists only in some builds, C locals, `@cname`, `CChar` by value, `AtomicU8` and `CLong` (ADR 0080). | [14.1](./ch14-01-build-settings), [20.2](./ch20-02-calling-c), [22.2](./ch22-02-kernel-code) |
| 0.23.0 | 2026-11-08 | Enums imported from C headers (ADR 0079). | [20.2](./ch20-02-calling-c) |
| 0.22.0 | 2026-11-08 | `File.openRegular`, and an open file's `modified` and `kind`. `sendFile` never raises SIGPIPE (ADR 0078). | — |
| 0.21.0 | 2026-11-06 | `Console.flush`, `Directory.space`, `IoError.StorageFull`, `File.openAppend`, `File.modified` and `File.setPermissions` (ADR 0077). | [21.1](./ch21-01-one-connection) |
| 0.20.0 | 2026-11-06 | A package's compiler requirement, `ceesafec = ">=X.Y.Z"` in the manifest (ADR 0076). | [7.4](./ch07-04-dependencies) |
| 0.19.1 | 2026-11-06 | Fixes only, to `N0272`, to `N0212` in library code, and to the receivers of `File`'s durable methods (ADR 0075 §8). | — |
| 0.19.0 | 2026-11-06 | `@from` variants, which let `?` convert an error. `OrderedMap`. `assert` and `panic`. Durable file writes, checksums, a monotonic clock, `replace`, arrays passed as slices, and `*r` to read through a borrow (ADR 0075). | [8.3](./ch08-03-maps), [9.1](./ch09-01-panics), [9.2](./ch09-02-result) |
| 0.18.2 | 2026-11-06 | A fix only: the compiler's temporaries are left out of the debug information. | — |
| 0.18.1 | 2026-11-05 | A fix only, to a `borrow` result with a region that a `borrow this` method returns. | — |
| 0.18.0 | 2026-11-05 | `Result<void, E>` with `Ok()`. The return type `never`. Raw `"""` string literals. Associated constants. `for` over any iterator. `checkedTruncate` and `saturatingTruncate` on integers. Regions on methods, optional borrows, and an `async move` child that may consume its captures (ADR 0074). | [3.2](./ch03-02-data-types), [3.3](./ch03-03-functions), [9.2](./ch09-02-result), [13.2](./ch13-02-iteration), [17.2](./ch17-02-scopes), [20.3](./ch20-03-more-interfaces-and-generics) |
| 0.17.0 | 2026-11-05 | A temporary as a method's receiver, `mut` arguments borrowed in two phases, blocks with a value as the branches of `if` and `match`, the first warning, `N0400`, for braces in a string, and `--deny-warnings` (ADR 0073). | [1.2](./ch01-02-hello-world), [3.5](./ch03-05-control-flow), [8.2](./ch08-02-strings), [14.1](./ch14-01-build-settings) |
| 0.16.1 | 2026-11-05 | A fix only, to a stale wake from `Async.blocking`. | — |
| 0.16.0 | 2026-11-05 | For optionals, `??`, `isSome` and `isNull`. `==` on enums without payloads, on slices and on arrays (ADR 0072). | [6.1](./ch06-01-defining-an-enum), [6.3](./ch06-03-optionals) |
| 0.15.2 | 2026-11-05 | Fixes only, four of them. | — |
| 0.15.1 | 2026-11-05 | A fix only: a method written in an `impl` block in another file keeps that file. | — |
| 0.15.0 | 2026-11-05 | Bytes and text: bulk appends, byte order, bit rotation and counting, searching, and number parsing (ADR 0071). | [8.2](./ch08-02-strings), [20.4](./ch20-04-arithmetic) |
| 0.14.0 | 2026-11-04 | `Window.presentChanged`, which presents only the changed parts of a window's frame (ADR 0070). | — |
| 0.13.1 | 2026-11-04 | A fix only: windows on macOS are sRGB (ADR 0069 §8). | — |
| 0.13.0 | 2026-11-04 | Native windows in the standard library (ADR 0069). | — |
| 0.12.0 | 2026-11-04 | Static tables of views (ADR 0066). A write to a `static mut` carries no loan (ADR 0067). The region `static` is rejected on parameters and enforced on results (ADR 0068). | [10.3](./ch10-03-regions), [22.2](./ch22-02-kernel-code) |
| 0.11.0 | 2026-11-04 | Ranges as indexes, `min` and `max` on integers, and a borrow reborrowed as `dyn` (ADR 0063). Generic methods, arrays and slices of views, and `dyn` as a bound (ADR 0064). For AI models, the library's pages in `llms-full.txt`, the MCP tool `ceesafe_api`, and "did you mean" in diagnostics (ADR 0065). | [4.3](./ch04-03-slices-and-views), [18.1](./ch18-01-dynamic-dispatch), [20.3](./ch20-03-more-interfaces-and-generics), [Appendix I](./appendix-i-for-ai-models) |
| 0.10.3 | 2026-11-03 | A fix only: a cancellation visits only the tasks of the cancelled scope (ADR 0062). | — |
| 0.10.2 | 2026-11-03 | A fix only, to a lost wake on kqueue. | — |
| 0.10.1 | 2026-11-03 | For C libraries, allocations at start-up that are counted and never failed (ADR 0061). | — |
| 0.10.0 | 2026-11-03 | `Random.fill`, `Channel.tryRecv`, C allocations counted with CeeSafe's, and `IoError.HostNotFound` (ADR 0060). | [16.2](./ch16-02-channels) |

## Releases 0.1.0 to 0.9.4

| Release | Date | Added | Sections |
|---|---|---|---|
| 0.9.4 | 2026-11-03 | No change to the language. A faster `sortBy` (ADR 0057), a faster `HashMap`, and allocation counters only when something reads them (ADR 0059). | — |
| 0.9.3 | 2026-11-02 | Recursion through `await`, with frames from a stack for each task (ADR 0056). Fixes to `ceesafec test`. | [17.1](./ch17-01-tasks-and-await) |
| 0.9.2 | 2026-11-02 | Fixes only, four of them, to generic impls and `dyn`. | — |
| 0.9.1 | 2026-11-02 | Wall-clock time, `Clock` and `UnixTime` (ADR 0055). | — |
| 0.9.0 | 2026-11-02 | UDP sockets for tasks (ADR 0050). Files sent on a stream without copying (ADR 0051). `HashMap` (ADR 0052). Task sets, which spawn while a task runs (ADR 0053). Generic impls, and impls for types with region parameters (ADR 0054). | [8.3](./ch08-03-maps), [17.2](./ch17-02-scopes), [20.3](./ch20-03-more-interfaces-and-generics) |
| 0.8.0 | 2026-11-02 | `ceesafec test`, with `@test` functions (ADR 0049). System libraries named in the manifest (ADR 0047). Name lookup for tasks (ADR 0048). | [11.1](./ch11-01-writing-tests), [11.2](./ch11-02-running-tests), [11.3](./ch11-03-organising-tests), [14.3](./ch14-03-libraries-for-other-programs) |
| 0.7.0 | 2026-11-02 | Socket addresses, IPv6, socket options and network error kinds (ADR 0044). Vectored sends (ADR 0045). `Async.timeout` and `Async.deadline` (ADR 0046). | [17.3](./ch17-03-timeouts-races-cancellation) |
| 0.6.0 | 2026-11-02 | Exports that exist only in some builds, and C function pointers from addresses (ADR 0043). | — |
| 0.5.1 | 2026-11-02 | A fix only: async programs build with Ubuntu's gcc. | — |
| 0.5.0 | 2026-11-01 | Statics that are C state: imported structs, tables of functions, and function addresses (ADR 0042). | [22.2](./ch22-02-kernel-code) |
| 0.4.1 | 2026-11-01 | No change to the compiler's behaviour. The release's own build is checked on one more system. | — |
| 0.4.0 | 2026-11-01 | For C interop, atomic fields shared with C, imported unions and variadic C calls (ADR 0041). The hosted executor runs on one more operating system (ADR 0040). | [20.2](./ch20-02-calling-c) |
| 0.3.2 | 2026-11-01 | No change to the compiler's behaviour. The editor extension is packaged differently. | — |
| 0.3.1 | 2026-11-01 | No change to the compiler's behaviour. The release is built from the release before it. | — |
| 0.3.0 | 2026-11-01 | Async: `async fn` and `await`, scopes, cancellation, timeouts and races (ADR 0031). Channels between threads and between tasks (ADR 0032). A scope child for each loop iteration, and `async move` children (ADR 0033). C `char` arrays in imported structs (ADR 0034). Async I/O on every system (ADR 0035), async methods through `dyn` (ADR 0036), closures that suspend when their caller's closure does (ADR 0037), work stealing with `spawnSend` (ADR 0038), and an executor for freestanding code (ADR 0039). | [16.2](./ch16-02-channels), [17.1](./ch17-01-tasks-and-await), [17.2](./ch17-02-scopes), [17.3](./ch17-03-timeouts-races-cancellation), [17.4](./ch17-04-async-io), [17.5](./ch17-05-tasks-and-threads) |
| 0.2.0 | 2026-10-31 | No change to the language. The runtime and the libraries become the compiler's own, and the benchmarks against Rust and C are added. | — |
| 0.1.0 | 2026-10-31 | The first release: the language, the libraries and the tools that this book teaches, except what a later row of this appendix names. That is ownership, borrows and regions, structs, enums and `match`, generics and interfaces, closures, `dyn`, threads, packages and the registry, unsafe code and C interop, the freestanding profile, and the commands `check`, `build`, `run`, `fmt`, `fix`, `explain`, `doc`, `lsp` and `mcp` (ADRs 0001 to 0030). | chapters 1 to 16, 18, 20 and 22 |

## What the table leaves out

- **Fixes.** A release usually fixes faults as well. They are listed only for a release that holds nothing else.
- **Speed.** Most releases make the compiler or the programs it builds faster. A row says so only when that is the
  release's subject.
- **ADR 0058,** which would have compiled the generated C at `-O3`, was withdrawn before a release. `-O2` stays.
