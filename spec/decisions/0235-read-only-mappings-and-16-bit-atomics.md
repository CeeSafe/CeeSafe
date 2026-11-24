# ADR 0235 — Read-only shared mappings, 16-bit atomics, and a file shortened under a mapping

- Status: **Proposed — implemented**, 2026-11-24, on the branch `fix/findings3-io` (not merged, not released), at the
  owner's request to close the SQLite port's finding 37 ("A shared mapping lacks what SQLite's wal-index uses beyond
  reads, writes and atomics", CeeSafeApps/sqlite CEESAFE_FINDINGS.md). It extends ADR 0232. The names in §2 are this
  ADR's; the owner may rename them before a release.
- Scope: type-system.md §26.3 and §26.20; lib/std's `SharedMapping` (16-bit atomics) and new `ReadOnlyMapping`; the
  hosted runtime's `ceesafe_host_map_file_read_only` and `ceesafe_host_map_*16` (runtime/hosted/ceesafe_rt_io.c,
  rt.h); the checker's name `f.mapSharedReadOnly(…)`; the interpreter's model; `tools/file_map_check.py`.

## 1. Context

ADR 0232's `SharedMapping` maps a region read-write and gives copies and 32- and 64-bit atomics. SQLite's wal-index
needs three more things (finding 37):

1. **A read-only mapping.** A database whose `-shm` cannot be written (a read-only directory, a file of another user,
   `?readonly_shm=1`) is still read in WAL mode: os_unix.c opens the `-shm` `O_RDONLY` and maps it `PROT_READ`
   (`unixShmMap`, `pShmNode->isReadonly`). `SharedMapping` needs a file open for reading and writing, and extends it,
   so the port fails with an `IoError` where SQLite reads.
2. **16-bit atomics.** wal.c's hash tables are arrays of `u16` slots (`ht_slot`) that readers load while the writer
   stores. The port loads the aligned 32-bit word that holds a slot and its neighbour, and stores whole words: correct,
   but a workaround, and a 16-bit store by another process (SQLite's own) next to a 32-bit store of the port is not
   the same access.
3. **SIGBUS.** A file shortened under a mapping ends the program at the next access to a page past the new end
   (ADR 0232 §2.3). The finding asks whether it can be an `IoError` instead.

## 2. Decision

### 2.1 16-bit atomics on `SharedMapping`

```ceesafe
m.load16(at: usize, order: LoadOrdering) -> u16
m.store16(at: usize, value: u16, order: StoreOrdering)
m.compareExchange16(at: usize, expected: u16, desired: u16, order: Ordering) -> Result<u16, u16>
m.fetchAdd16(at: usize, value: u16, order: Ordering) -> u16              // wraps
```

They are the 32-bit operations' twins (ADR 0232 §2.1): bounds and alignment are checked and a failure panics, the
alignment being the file's (`offset() + at` a multiple of 2); byte order is the host's; they are atomic between
processes. Every host CeeSafe builds for has lock-free 16-bit atomics on aligned memory (x86 and x86-64 `mov`/
`lock cmpxchg` on a word, ARMv8's `ldarh`/`stlrh`/`casalh` or exclusive pairs, ARMv7's `ldrexh`/`strexh`); a host
whose compiler reports otherwise (`__GCC_ATOMIC_SHORT_LOCK_FREE` < 2) panics rather than take a lock only this process
sees, as the 64-bit ones do. With MSVC they are `_InterlockedCompareExchange16`, `_InterlockedExchange16` and
`_InterlockedExchangeAdd16`, and the load is §2.2's.

`fetchAdd16` is included because it is the natural fourth of the set and costs nothing; SQLite itself does not need it.

### 2.2 `ReadOnlyMapping`

```ceesafe
@noncopy public struct ReadOnlyMapping { … }

f.mapSharedReadOnly(offset: u64, length: usize) -> Result<ReadOnlyMapping, IoError>  // ReadOnlyMapping.mapSharedReadOnly(f, …)

r.length() -> usize                     r.offset() -> u64
r.readAt(at: usize, into: SliceMut<u8>)                // copies out
r.load16(at: usize, order: LoadOrdering) -> u16
r.load32(at: usize, order: LoadOrdering) -> u32
```

- **Writes are impossible by type**: `ReadOnlyMapping` has no `writeAt`, no store, no read-modify-write and no
  `sync`, and a call of one is an error of the checker (no such method). The pages are also mapped without write
  access (`PROT_READ`; Windows `PAGE_READONLY` and `FILE_MAP_READ`), so not even `unsafe` code writes them.
- **The file is not extended.** A read-only mapping covers bytes the file holds: a range that ends past the file's
  current length is `InvalidInput`. SQLite does the same: a `readonly_shm` connection that finds the `-shm` too short
  does not map it (`SQLITE_READONLY_CANTINIT`); the caller asks `f.length()` first.
- **Any `File` open for reading** may be mapped: `File.open`, `openRegular`, and the read-and-write opens. One open
  only for writing (`File.create`, `openAppend`) is `BadDescriptor`, as for a read. `length` 0, a range past 2^63 - 1,
  and a file that is not a regular one are `InvalidInput`, as for `mapShared`.
- **What it sees** is ADR 0232's one memory: writes by any read-write mapping of those bytes, in this process or
  another, and by the file's writes. Its `load16` and `load32` are atomic between processes with those mappings'
  atomics, with `LoadOrdering`'s meaning; `SharedMapping.fence()` orders its copies and loads as it orders a
  `SharedMapping`'s. Its copies may mix old and new bytes while another process writes, as ADR 0232's do.
- **No `load64`.** On a 32-bit x86 host a 64-bit atomic load may be a `lock cmpxchg8b` (clang without SSE2; MSVC's
  `_InterlockedCompareExchange64`), which is a write access and faults on a read-only page. A program that needs a
  64-bit value from a read-only mapping reads it with two `load32` and a protocol, as SQLite's 64-bit fields are
  read through its header copies. Added later if a host gains a load that never writes, which every 64-bit host has.
- **The loads never write**, on every host: GCC's and clang's `__atomic_load_n` on 16- and 32-bit words is a plain or
  acquiring load (x86 `mov`, ARMv8 `ldar`, ARMv7 `ldr` and `dmb`). With MSVC, whose `_InterlockedCompareExchange`
  load (rt.h's) is a locked write, the mapping loads are a `volatile` load (`__iso_volatile_load16`/`32`, which an
  aligned word makes atomic) with `MemoryBarrier()` around it for the orderings that need one; `SharedMapping`'s
  `load16` and `load32` use the same code, which is correct for both.
- **The value** is a move type that needs drop, Send and Share, as `SharedMapping`; it does not borrow the `File`;
  dropping it unmaps the region. Bounds and alignment checks panic, with `SharedMapping`'s messages' twins.
- **Effects.** `mapSharedReadOnly` has `sleep`; the copies and loads none.

### 2.3 A file shortened under a mapping stays `SIGBUS`

The finding asks for an `IoError` instead. It is **not done**, and ADR 0232 §2.3 stands, for these reasons:

- **Only a signal handler can see the fault.** Turning it into a value means a process-wide `SIGBUS` handler that,
  for a fault inside a mapping access, jumps back (`siglongjmp`) to a `sigsetjmp` taken before the access, in every
  `readAt`, `writeAt`, load, store and compare-exchange, on every thread (a per-thread jump buffer).
- **The jump abandons a `memcpy` halfway** and an atomic instruction that the C standard does not allow to be
  interrupted that way; a store that partly happened, or a copy that wrote some of `into`, is then the program's
  state. `memcpy` and the atomics are not async-signal-safe to leave by a jump (POSIX lists `siglongjmp` out of a
  handler as undefined after a non-async-signal-safe function was interrupted).
- **The handler is not the runtime's to own.** A program, a library it links, or the host embedding it (a JVM on
  Android, a sanitizer, a crash reporter) installs its own `SIGBUS` handler; replacing or chaining it changes their
  behaviour, and a fault elsewhere (an unaligned access on some ARM hosts, a hardware error) must not be mistaken for
  a mapping's.
- **A check before each access does not close the race.** `fstat` and compare, then access: the file can be shortened
  between the two, so the program can still fault, and every access would pay a system call.
- **The fault is contained**: the access does not complete, so no wrong value is read and no other memory is
  written, and the program ends, as at a panic. That is the safety CeeSafe promises; an error value would be a
  convenience.
- **The protocol prevents it.** SQLite shortens a `-shm` only while it holds the DMS byte exclusively, which no
  connection that maps it allows (ADR 0234's `testLock` and ADR 0220's locks), and CeeSafe itself never shortens a
  file under a mapping. Windows refuses to shorten a mapped file at all.

The ADR records the fault in `ReadOnlyMapping`'s documentation as in `SharedMapping`'s. If a host gains a way to
make a mapping's faults into errors without a handler (none of CeeSafe's hosts has one), this can be revisited.

### 2.4 The runtime (a C adapter, runtime/hosted/ceesafe_rt_io.c)

| Function | Contract |
|---|---|
| `ceesafe_host_map_file_read_only(file, offset, length, out) -> int64_t` | As `ceesafe_host_map_file` (ADR 0232 §2.2), but: `fstat` (`GetFileSizeEx`) first, and a range that ends past the file's length is `EINVAL`, never an extension; POSIX `mmap(PROT_READ, MAP_SHARED)`, Windows `CreateFileMappingW(PAGE_READONLY, offset + length)` and `MapViewOfFile(FILE_MAP_READ)`; a file not open for reading is `EBADF` (`EACCES` from `mmap` or `CreateFileMapping`). `out` as there. |
| `ceesafe_host_map_load16(address, at, order)`, `_store16`, `_cas16`, `_add16` | §2.1, on the `u16` at `address + at`, which lib/std has checked is inside and aligned. |
| `ceesafe_host_map_load32` | Unchanged on GCC and clang; with MSVC, §2.2's non-writing load. |

`ReadOnlyMapping` uses `ceesafe_host_map_unmap`, `ceesafe_host_map_read` and the loads of ADR 0232 and §2.1, which
take an address and never write through it.

### 2.5 The interpreter

ADR 0232 §2.5's model, extended:

- a read-only mapping is the file's path and region, made from a handle open for reading (a `File.open` one, or a
  read-and-write one; a `File.create` or `openAppend` one is `EBADF`), refused with `EINVAL` past the file's end, and
  never extends the file; its copies and loads reopen the file **for reading** (the copies of a read-write mapping
  do too since this ADR), so a file the program may not write can be mapped read-only;
- the 16-bit operations are positional reads and writes of two bytes, little-endian, atomic among the program's
  threads and not against other processes, as the others.

## 3. Options not taken

- **A mode argument** (`f.mapShared(offset, length, MapAccess.Read)`) returning `SharedMapping`, whose writes then
  panic or return errors. The task is "impossible by type": a separate type makes a write to a read-only mapping a
  compile-time error, and leaves `mapShared`'s signature as released.
- **A read-only view of a `SharedMapping`** (`m.readOnly() -> borrow ReadOnlyMapping`). Useful only inside one
  program, which can already pass `SharedMapping` and not call its writes; the need is a file that cannot be
  written, which a read-write mapping cannot map.
- **Extending the file under a read-only mapping.** It needs write access, which the case does not have.
- **Making `ReadOnlyMapping` a generic `Mapping<Access>`.** Two concrete types are simpler to read and to document,
  and share their runtime.
- **`SIGBUS` as an `IoError`** (§2.3).
- **8-bit atomics.** wal.c has no byte-wide shared fields it reads atomically; `readAt` of one byte cannot tear.

## 4. Tests

- `tests/reference/tests/corpus/run-pass/file-map-read-only.cee`, the same output natively and in the interpreter:
  a read-only mapping of a file opened with `File.open` sees what a read-write mapping writes (a copy, `load16`,
  `load32` of a slot pair), and what the file is written with, also from an offset not on a page; the 16-bit atomics
  of `SharedMapping` (`store16`, `load16`, `compareExchange16` success and failure, `fetchAdd16` wrapping at 0xFFFF)
  with the neighbouring `u16`s untouched; four threads, two through each of two mappings, counting to 40 000 with
  `compareExchange16` loops at one slot between two others that stay; a read-write `File` mapped read-only; the
  errors (`InvalidInput` past the file's end, which is not extended, for length 0 and a range past 2^63 - 1;
  `BadDescriptor` for a `File.create` file). The expected integers are Python's (`0xBEEF`, `0x1BEEF`, …).
- `run-fail/file-map-misaligned16.cee`: a 16-bit atomic at an odd file offset panics.
- `check-fail/file-map-read-only-write.cee`: `writeAt`, `store32`, `store16`, `compareExchange32`, `fetchAdd16` and
  `sync` on a `ReadOnlyMapping` are N0200.
- `tools/file_map_check.py`, natively and in the interpreter: (1) Python's `mmap` writes `u16` slots
  (`struct.pack("=H")`) and a text, makes the file mode 0444, and the program, opening it with `File.open`, reads them
  through a read-only mapping (`load16`, `load32` of the pair, a copy); (2) Python maps the file `ACCESS_READ` and
  holds it while the program's `store16`, `compareExchange16` and `fetchAdd16` run through a read-write mapping, and
  sees them, the wrap, and the untouched neighbour; (3) a read-only mapping of a file shortened by another process
  ends the program by `SIGBUS` natively and faults in the interpreter (§2.3); (4) natively, where `sqlite3` is
  installed, the live `-shm` of a WAL database opened with `File.open` and mapped read-only gives the same answers as
  the read-write mapping: equal header copies, `iVersion` 3007000 by `load32`, `isInit`, `mxFrame`.
- Natively, `--interp` and `--check-leaks`. Windows: the code is written for §2.2 and §2.4 and compiled by the
  release workflow; it has not been run here.
