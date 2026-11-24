# ADR 0232 — Shared mappings of a region of a file

- Status: **Proposed — implemented**, 2026-11-24, on the branch `fix/findings2-shm` (not merged, not released), at
  the owner's request: "Implement fixes for findings 6, 15, 19, 26 and 35 in CeeSafe". This ADR is finding 26 of the
  SQLite port ("No shared memory", CeeSafeApps/sqlite CEESAFE_FINDINGS.md). The names in §2 are this ADR's; the
  owner may rename them before a release.
- Scope: type-system.md §26.3 (files) and a new §26.20; lib/std's `SharedMapping`; the hosted runtime's
  `ceesafe_host_map_*` (runtime/hosted/ceesafe_rt_io.c, rt.h); the checker's name `f.mapShared(…)`; the
  interpreter's model of it; `tools/file_map_check.py`.

## 1. Context

SQLite's WAL mode keeps the wal-index in `<database>-shm`. Every connection, in every process, maps it with
`mmap(MAP_SHARED)` in regions of 32 KiB, one region at a time as the index grows (os_unix.c `unixShmMap`; os_win.c
`winShmMap` with `CreateFileMapping` and `MapViewOfFile`). Connections take byte-range locks on the file (ADR 0220's
`File.lock`) and read and write the memory with aligned 32-bit loads and stores and memory barriers (`unixShmBarrier`):
wal.c's `walIndexHdr` protocol writes two copies of the 48-byte header, the second after a barrier, and a reader
copies both and compares them, retrying when another process was writing. Without a shared mapping the port cannot
take part in that protocol (finding 26), and keeps one process at a time in WAL mode.

ADR 0172's `SharedMemory` is memory by a name, not a file; its §2 left "C. A file that is mapped" for later. Its
rule holds here too: **memory another process may write at any moment is never a `Slice<u8>`**. A shared borrow
promises that no one writes what it reads (§25), and nothing can keep that promise for another process.

## 2. Decision

### 2.1 The API (lib/std, hosted)

```ceesafe
@noncopy public struct SharedMapping { … }

f.mapShared(offset: u64, length: usize) -> Result<SharedMapping, IoError>   // SharedMapping.mapShared(f, …)

m.length() -> usize                     m.offset() -> u64
m.readAt(at: usize, into: SliceMut<u8>)              // copies out
m.writeAt(at: usize, bytes: Slice<u8>)               // copies in
m.load32(at: usize, order: LoadOrdering) -> u32
m.store32(at: usize, value: u32, order: StoreOrdering)
m.compareExchange32(at: usize, expected: u32, desired: u32, order: Ordering) -> Result<u32, u32>
m.fetchAdd32(at: usize, value: u32, order: Ordering) -> u32
m.load64 / store64 / compareExchange64 / fetchAdd64      // the same for u64
SharedMapping.fence()                                    // a sequentially consistent fence
m.sync() -> Result<void, IoError>                        // msync(MS_SYNC); FlushViewOfFile
```

- **`f.mapShared(offset, length)`** is the checker's name for `SharedMapping.mapShared(f, offset, length)`, as
  `f.lock(…)` is `FileLocks.lock(f, …)` (ADR 0220 §2.1); `f` is borrowed for the call only. It maps bytes `offset`
  to `offset + length - 1` of the file, shared: what this program writes there, every other mapping of those bytes
  sees, in this process and in others, and so does a read of the file. `length` 0, or a range that ends past
  2^63 - 1, is `InvalidInput`. `offset` need not be aligned to anything: the runtime maps from the page (on
  Windows the 64 KiB allocation granule) below it and hides the difference. The file must be open for reading and
  writing (`openReadWrite`, `openOrCreate`, `createNew`, `createReadWrite`); otherwise `BadDescriptor`, on every
  system. A file that is not a regular one is `InvalidInput` (POSIX `ENODEV`) or the system's error.
- **The file covers the mapping before it is mapped** (§2.3): a file shorter than `offset + length` is extended
  with zero bytes, never shortened.
- **The value** is a move type that needs drop, and is Send and Share: like `SharedMemory`, it holds its address
  as a number, and everything it gives is a copy or an atomic. It does not borrow the `File`: the mapping stays
  valid after the `File` is dropped (both systems keep it). Dropping it unmaps the region; a change already made
  is the file's whether or not `sync` ran (the system writes it out in its own time).
- **Growing** is mapping again: a program maps each region it needs as a value of its own, as SQLite maps one
  32 KiB region at a time and keeps an array of them. Regions may overlap; the bytes they share are one memory.
- **Copies, not views.** `readAt` copies `into.length` bytes from `at`, `writeAt` copies `bytes` to `at`. They are
  plain copies (`memcpy`): while another process writes the same bytes a copy may hold some old and some new
  bytes. That is SQLite's case, and its protocol (two header copies, a checksum) is what detects it; the copy is
  never undefined behaviour of the program, since the program never holds a reference into the memory.
- **Atomics.** `load32`, `store32`, `compareExchange32` and `fetchAdd32` are lib/core's `AtomicU32` operations
  (`loadWith`, `storeWith`, `compareExchangeWith`, `fetchAddWith`) at byte `at` of the mapping, with lib/core's
  ordering enums (`LoadOrdering`, `StoreOrdering`, `Ordering`, ADR 0021) and their meaning, including a failed
  compare-exchange's ordering. They are atomic between processes: the hosts' 32-bit and 64-bit atomics are
  lock-free instructions on the memory itself (on a host whose 64-bit atomics are not lock-free the 64-bit
  operations panic rather than take a lock that only this process sees; none of CeeSafe's hosts is one).
- **Byte order** is the host's, as for any integer in memory: SQLite's wal-index is in native order too.
- **`SharedMapping.fence()`** is a sequentially consistent fence (`atomic_thread_fence(memory_order_seq_cst)`;
  `MemoryBarrier` with MSVC): SQLite's `unixShmBarrier`. A copy before it is ordered before a copy or an atomic
  after it.
- **Bounds and alignment are checked; a failure panics**, as a slice index out of range does (§12), and as
  `SharedMemory.word` does: they are mistakes in the program, not conditions of the system. `readAt` and `writeAt`
  panic when `at + n > length()`. An atomic panics when its bytes do not lie inside the mapping, or when
  `offset() + at` is not a multiple of its size (4 or 8): alignment is the file's, so two mappings of the same bytes
  agree on it.
- **`sync()`** writes the region's changed pages to the file and waits (`msync(MS_SYNC)`; Windows
  `FlushViewOfFile`, after which `f.syncAll()` makes the file durable). SQLite never syncs its `-shm`.
- **Effects.** `mapShared` and `sync` have `sleep` (they wait for the file system); the copies, atomics and the
  fence have none, as memory accesses have none.

### 2.2 The runtime (a C adapter, runtime/hosted/ceesafe_rt_io.c)

| Function | Contract |
|---|---|
| `ceesafe_host_map_file(file, offset, length, out) -> int64_t` | `file` points at a `File` (its first field is the descriptor). lib/std has checked `length > 0` and `offset + length <= 2^63 - 1`. Extends the file to cover the range (§2.3), maps it shared for reading and writing, and writes `out[0]` the address of byte `offset`, `out[1]` and `out[2]` the start and length of what was mapped, `out[3]` the Windows mapping handle (0 on POSIX). 0, or a negated errno (`EACCES` from `mmap`/`CreateFileMapping` is reported as `EBADF`). |
| `ceesafe_host_map_unmap(base, length, handle)` | Unmaps what `out[1]`, `out[2]`, `out[3]` named, once. |
| `ceesafe_host_map_read(address, at, into, length)`, `_write(address, at, bytes, length)` | `memcpy` between bytes `at .. at + length - 1` of the mapping at `address` and the caller's buffer, which lib/std has checked against the mapping's length. |
| `ceesafe_host_map_load32(address, at, order)`, `_store32`, `_cas32`, `_add32`, and the `64` forms | rt.h's `ceesafe_atomic_*_ord_u32` / `_u64` at `address + at`, which lib/std has checked is inside and aligned. `order` is lib/core's code (0 relaxed … 4 sequentially consistent). |
| `ceesafe_host_map_fence()` | `__atomic_thread_fence(__ATOMIC_SEQ_CST)`; `MemoryBarrier()` with MSVC. |
| `ceesafe_host_map_sync(base, length) -> int64_t` | `msync(MS_SYNC)` / `FlushViewOfFile` over what was mapped; 0 or a negated errno. |

The address is passed with `at` apart, rather than added in lib/std, so the interpreter (§2.5) can model every access
by name without an address of the host's.

| System | Mapping |
|---|---|
| Linux, Android, macOS, FreeBSD and the other BSDs, 32-bit Linux | `fstat`; if the file is shorter, `pwrite` one zero byte at `offset + length - 1` (SQLite's way: it never shortens a file another process grew meanwhile, as `ftruncate` could); `mmap(NULL, …, PROT_READ \| PROT_WRITE, MAP_SHARED, fd, page-aligned offset)`. A 32-bit Linux build has a 64-bit `off_t` (`_FILE_OFFSET_BITS`, set in ceesafe_rt_io.c), so offsets past 2 GiB map; a `length` that does not fit the address space is `ENOMEM`. |
| Windows | `CreateFileMappingW(handle, PAGE_READWRITE, offset + length)`, which extends a shorter file and never shortens it; `MapViewOfFile(FILE_MAP_ALL_ACCESS, granule-aligned offset)`; the mapping handle is kept for drop (`UnmapViewOfFile`, `CloseHandle`). |
| CosmoOS, freestanding | `Unsupported` / not there (profiles.md §4). |

### 2.3 A file shortened under a mapping (SIGBUS)

On POSIX, touching a mapped page that lies wholly past the end of the file raises `SIGBUS`. The runtime **avoids**
it for everything CeeSafe does: a mapping is made only over bytes the file holds, extending the file first, and lib/std
never shortens a file. What it does not control is **another party shortening the file** while it is mapped
(another process's `ftruncate`, or this program's own `f.setLength` smaller than a live mapping). Then the access
faults, and the process ends by `SIGBUS`, the system's default action, as a C program's would. It is **contained**
in the sense that matters for safety: the faulting load or store does not complete, so no wrong value is read and no
other memory is written; the program stops, as at a panic. The runtime installs no `SIGBUS` handler: turning the
fault into an error would need every access to be restartable, which a `memcpy` is not, and a handler is the
program's (or the embedding host's) to own. On Linux and macOS the bytes of the last page past a shortened end read
as zeros and do not fault. Windows refuses to shorten a file that has a mapped view (`ERROR_USER_MAPPED_FILE`), so
there is no such case there. SQLite has the same exposure and the same answer: no one shortens a `-shm` that
another connection maps, which its DMS lock (a byte-range lock, ADR 0220) ensures.

### 2.4 Two processes, and the interpreter

Natively, a `SharedMapping` is the memory itself: a compare-exchange by two processes on the same bytes is atomic
between them, and a store with `Release` in one is seen by an `Acquire` load in the other with what was written
before it. Two `SharedMapping`s of one file in one process are two addresses of the same memory, and behave as two
processes' do.

### 2.5 The interpreter

The interpreter (`ceesafec run --interp`) has no address of the host's to hand out, and its files reopen for each
positional operation (§26.3). It **models a mapping by the file itself**: every copy and every atomic is a
positional read and write of the file at `offset + at` (stage 0's `File.openReadWrite`, `readAt`, `writeAt`), so

- the program's own mappings, its threads and its reads of the file see one memory, and a compare-exchange is
  atomic among the program's threads (the interpreter runs one at a time);
- other processes see the bytes, since Linux, macOS and the BSDs keep a file's mapped pages and its reads and writes
  the same; **but an interpreted atomic is not atomic against another process**, which may change the bytes between
  the model's read and write. A test of interoperation runs natively (`tools/file_map_check.py`);
- integers are little-endian (every host the interpreter runs on is);
- `fence` does nothing (its accesses are already in order); `sync` is done once the call returns;
- an access past the end of a file shortened under the mapping is a fault (the native `SIGBUS`); a file removed or
  renamed while mapped faults at the next access, since the model reopens it by its path;
- `--sandbox` refuses it, as it refuses files.

## 3. Options not taken

- **A `Slice<u8>` or `SliceMut<u8>` of the mapping**, or `Ptr` access in `unsafe` as the only way. A slice breaks
  §25 the moment another process writes; `unsafe` would make every WAL reader unsafe code. Copies and atomics are
  what SQLite does with the memory anyway (it `memcpy`s the header out before it trusts it).
- **Views of `AtomicU32`** (`m.word32(at) -> borrow AtomicU32`, as `SharedMemory.word`). The interpreter cannot make
  one (ADR 0172 §8 answers `Unsupported` for this reason), so a WAL reader could not run interpreted; the methods
  here give the same operations, and the corpus runs them both ways.
- **A remap that grows one mapping** (`mremap`). Not portable (Linux only), and it moves the address, which nothing
  here exposes but which would invalidate nothing either; mapping another region is SQLite's own way and works on
  every system.
- **Making the mapping borrow the `File`.** Both systems keep a mapping after its descriptor closes, and SQLite
  closes nothing while mapped anyway; a region parameter would only make an array of regions harder to keep.
- **A `SIGBUS` handler that turns a fault into an error** (§2.3).
- **Errors for a bad `at`**, `Result` on every access. A WAL reader checks every offset it computes from the file's
  bytes before using it (the port's own rule); an offset past the mapping is then a bug, and a panic says so, as for
  slices.
- **`ftruncate` to extend.** Two processes extending at once could shorten what the other just grew (SIGBUS for it);
  a one-byte `pwrite` at the new last byte only ever lengthens.

## 4. Tests

- `tests/reference/tests/corpus/run-pass/file-map-shared.cee`, natively and under `--interp` with the same output:
  copies in and out; two mappings of overlapping bytes see each other's writes; a read of the file sees what was
  written through the mapping; the file grows to cover a mapping past its end; an unaligned `offset`; the 32- and
  64-bit atomics with each ordering; four threads, two per mapping, counting to 40 000 with `compareExchange32`
  loops; a child process (the program itself, `Process.spawn` of `Process.executable()`) counting through its own
  mapping beside the parent, 2 × 10 000 (where a child cannot be started, as in the interpreter, a thread does the
  child's part, so the output is the same); `InvalidInput` for length 0 and a range past 2^63 - 1; `BadDescriptor`
  for a file open only for reading.
- `run-fail/file-map-out-of-range.cee` and `run-fail/file-map-misaligned.cee`: the panics.
- `tools/file_map_check.py`, in the contract suite as `file maps`: (1) against Python's `mmap` module in another
  process (an independent implementation of `MAP_SHARED`), both ways: Python writes a pattern and a counter through
  its mapping and the program reads them through its own, and the program writes and Python reads, natively and in
  the interpreter; (2) a file shortened by another process under a live mapping ends the program by `SIGBUS`
  natively (POSIX); (3) where `sqlite3` is installed, a database in WAL mode held open by a `sqlite3` process: the
  program maps the first 32 KiB of its `-shm` and finds the two copies of the wal-index header equal, with
  version 3007000 and the `mxFrame` that `PRAGMA wal_checkpoint` (passive) reports.
