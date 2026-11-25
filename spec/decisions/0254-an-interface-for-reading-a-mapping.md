# ADR 0254 — `MappedRead`: one interface for reading a shared or a read-only mapping

- Status: **Proposed — implemented**, 2026-11-25, on the branch `fix/g5-files` (from 0.95.10, not merged, not
  released), at the owner's request to close the SQLite port's finding 42 ("`SharedMapping` and `ReadOnlyMapping`
  share no interface", CeeSafeApps/sqlite CEESAFE_FINDINGS.md). It extends ADR 0232 and ADR 0235. The names in §2 are
  this ADR's; the owner may rename them before a release.
- Scope: type-system.md §26.20; lib/std's new `MappedRead` and the methods `SharedMapping` and `ReadOnlyMapping` give
  it (lib/std/mapping.cee); spec/api/std.md (written by `ceesafec doc`); the guide's built-in types page.

## 1. Context

SQLite's wal-index is a list of 32 KiB blocks of the `-shm` file. A block is mapped read-write (`SharedMapping`,
ADR 0232), mapped read-only when the `-shm` cannot be written (`ReadOnlyMapping`, ADR 0235), or kept in the heap
when the database is opened with `locking_mode=EXCLUSIVE` and no `-shm` exists (wal.c's heap-memory index). The code
that reads the index (the header copies, the hash slots, the page numbers) is the same for all three: `readAt`,
`load16`, `load32`. The two mapping types have those methods under the same names and signatures, but nothing names
what they share, so a function cannot take either. The port dispatches through an enum of the three (`IndexBlock`,
src/walindex.cee) whose methods match on the variant, and whose writes assert at run time that a read-only block is
never written: a check the types had made at compile time.

ADR 0235 §3 considered a generic `Mapping<Access>` and kept two concrete types. That choice stands; what is missing is
an interface both implement.

## 2. Decision

### 2.1 The interface (lib/std, hosted)

```ceesafe
public interface MappedRead {
    @effects() fn length(borrow this) -> usize;
    @effects() fn offset(borrow this) -> u64;
    @effects() fn readAt(borrow this, at: usize, into: SliceMut<u8>);
    @effects() fn load16(borrow this, at: usize, order: LoadOrdering) -> u16;
    @effects() fn load32(borrow this, at: usize, order: LoadOrdering) -> u32;
}

impl MappedRead for SharedMapping { … }
impl MappedRead for ReadOnlyMapping { … }
```

- **What it lists** is what both types can do without writing: the region's `length` and `offset`, a copy out
  (`readAt`), and the 16- and 32-bit atomic loads. The meaning of each is ADR 0232's and ADR 0235's, unchanged:
  bounds and alignment checks that panic, loads atomic between processes with `LoadOrdering`'s meaning, copies that
  may mix old and new bytes while another process writes.
- **`load64` is not in it**, since `ReadOnlyMapping` has none (ADR 0235 §2.2: a 32-bit x86 host's 64-bit atomic load
  may write, and faults on a read-only page). It stays a method of `SharedMapping` alone.
- **Nothing that writes is in it.** Code generic over `M: MappedRead` cannot write a block, whatever `M` is, and
  `ReadOnlyMapping` still has no method that writes: a call of `writeAt` or `store32` on one is the checker's N0200, as
  before. A function that writes takes a `SharedMapping`.
- **The methods move into the impls.** An inherent method and an interface method of one name on one type are N0228
  (§21.1), so `SharedMapping`'s and `ReadOnlyMapping`'s `length`, `offset`, `readAt`, `load16` and `load32` are now
  written in their `impl MappedRead for …` blocks. A call on the concrete type (`m.load32(0, LoadOrdering.Acquire)`)
  is unchanged: an impl's methods are the type's methods (§21.1), so no program changes. The bodies are the same.
- **No effects.** Each method is `@effects()`, as the extern calls under them are: a call through a bound has the
  instance's effects, and a call through `borrow dyn MappedRead` has none (§37.3), so a reader may run where
  effects are forbidden, as the concrete calls may. An implementation of the program's own (§2.2) must have none.
- **Any type may implement it.** The port's heap-memory index block implements it with an ordinary `Vector<u32>`, so
  its reading code is one function generic over `M: MappedRead` (or taking `borrow dyn MappedRead`) for the three
  kinds of block. The interface's documentation states the contract an implementation keeps: `length` bytes from
  `offset`, the copies and loads of those bytes, a panic for an access outside them or a load not aligned to its size.
- `SharedMapping.fence()` is a static function, not a method, and stays one: it orders the copies and loads of every
  mapping.

### 2.2 Use

```ceesafe
fn walVersion<M: MappedRead>(block: borrow M) -> u32 => block.load32(0, LoadOrdering.Acquire);

fn header(block: borrow dyn MappedRead, into: SliceMut<u8>) { block.readAt(0, into); }
```

Both `SharedMapping` and `ReadOnlyMapping` are Send and Share, so a generic `M: MappedRead + Share` is shared across
threads as either is.

## 3. Options not taken

- **A generic `Mapping<Access>`** (ADR 0235 §3): still two concrete types; the interface gives the generic code the
  port needs without changing either type.
- **A read-only view of a `SharedMapping`** (`m.reading() -> ReadOnlyMapping` or a borrow of one): a second value over
  the same pages would need its own unmapping rules, and a borrow of a `ReadOnlyMapping` cannot be made from a
  `SharedMapping`'s fields. The interface serves the same purpose without a new value.
- **`load64` in the interface, panicking for a read-only mapping**: a method that exists on a type only to panic
  would turn ADR 0235's compile-time refusal into a run-time one, which is what the port's enum does now.
- **A second interface for writes** (`MappedWrite: MappedRead` with `writeAt` and the stores): only `SharedMapping`
  writes, so the concrete type serves. It can be added if a second writable kind appears.

## 4. Tests

- `tests/reference/tests/corpus/run-pass/file-map-interface.cee`, the same output natively, in the interpreter and
  with `--check-leaks`: one function generic over `M: MappedRead` and one taking `borrow dyn MappedRead` read a
  header copy, a `u16` slot and a `u32` word through a `SharedMapping`, a `ReadOnlyMapping` of the same bytes and a
  heap block of the program's own that implements the interface; all three agree, and the values are those written
  (expected values from Python's `struct`: `0xBEEF`, `0x0BADF00D`).
- `check-fail/file-map-interface-write.cee`: `writeAt` and `store32` through `M: MappedRead` and through `borrow dyn
  MappedRead` are N0234 (the interface has no such method); `load64` through it likewise.
- `tools/file_map_check.py` part (5): Python's `mmap` writes the slots and the program reads them through the generic
  function from a read-write and a read-only mapping.
- The existing mapping tests (`file-map*.cee`, `tools/file_map_check.py`) are unchanged and pass: calls on the
  concrete types are unchanged.
