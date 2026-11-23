# ADR 0224 — Draining a vector, and changing an `Arc`'s value through its only owner

- Status: **Proposed** 2026-11-24 and implemented on the branch `fix/findings-core`, at the owner's instruction (the
  SQLite port's findings 4, 13 and 22; findings 5 and 24 are answered by what the tree already has, §5). Not
  released. §7's questions are the owner's.
- Scope: type-system.md §23.1 (vector methods) and §38.3 (`Arc`); lib/core `slices.cee` (VectorDrain,
  VectorDrainFailing, VectorDrainSecret); lib/std `sync.cee` (`Arc.getMut`); `src/check/vectors.cee`
  (checkVectorDrain) and the method-name lists of `bytes_text.cee`; `src/main.cee` (markUncheckedLibrary);
  guide/collections.md, guide/builtins.md, guide/threads.md; book Appendix C; tools/collection_properties.py.

## 1. Context

**Consuming a vector in order** (findings 13 and 22). `for (x in v)` borrows each element. Moving every element out
of a vector, first to last, into another container or into a function that takes ownership, had two ways, both
poor: `while (v.length != 0) { w.push(v.remove(0))?; }`, which is quadratic, or an index loop that swaps each
element out with `replace(mut v[i], …)`, which needs a placeholder value of the element type. The port wrote
`takeRow` for its rows and `tablesInSqliteOrder` with `remove(0)`.

**Changing a shared value in place** (finding 4). `Arc<T>` lends `borrow T` only. The port's page cache shares pages
with readers; once every reader has dropped its clone, the cache could update a page in place, but `Arc` gave no
way to, so the writable pager copies each page into a new `Arc` per reader.

## 2. Decision: `v.drain()`

1. `v.drain()` on a `Vector<T, A>` (A one of `SystemHeap`, `FailingAllocator`, `SecretHeap`) is an iterator
   (§16.4) that moves the elements out first to last: `for (x in v.drain()) { … }`. Its `next()` is `T?`.
2. It **borrows** the vector exclusively (`borrow mut this`) for as long as the iterator lives, so the vector cannot
   be read or changed meanwhile (N0241), and it works on any mutable place: a local, a field through
   `borrow mut this` (`this.rows.drain()`), an element.
3. When the iterator is dropped, the vector is empty and keeps its capacity, ready to be filled again. Elements the
   loop did not take (it left with `break`, `return` or `?`) are destroyed then, first to last, as `clear` destroys
   them.
4. It is O(n) for n elements and allocates nothing: `drain` reverses the elements once by swaps, `next` pops the
   last, and the drop reverses what is left back before clearing it. It is safe code in lib/core: `VectorDrain<r,
   T>` holds `borrow mut(r) Vector<T>`; `VectorDrainFailing` and `VectorDrainSecret` hold the other allocators'
   vectors (a `SecretHeap` vector's `pop` wipes each slot as its element leaves).
5. The checker routes `v.drain()` to `VectorDrain` + the allocator's suffix (`drain`), as it routes `v.remove(at)`
   to VectorEditing (ADR 0075 §2.12).

**Why a borrowing drain rather than `for (x in move v)`.** A consuming loop would need the vector itself, so a field
would have to be `replace`d first, and the emptied buffer would be freed rather than reused. The borrowing form
covers both: `for (x in v.drain())` leaves `v` empty and usable, and a vector that is not needed afterwards is simply
dropped at the end of its scope.

**`drain(range)`** is left out: the port needed the whole vector, and a range form needs a rotation and a range
argument type that slices do not yet take as a value. It can be added later without changing `drain()`.

**The freestanding profile.** `Vector` is hosted only, so in `--profile freestanding` VectorDrain's field does not
resolve (N0293, dropped with the libraries' diagnostics) and its methods' bodies have errors that no diagnostic
inside them explains. markUncheckedLibrary now also marks the methods and drop of a library struct with an
unresolved field as unchecked, as it marks a library function with an error inside it, so they are not lowered and
do not report N0216. Nothing in a freestanding program can name a vector to drain.

## 3. Decision: `a.getMut()`

1. `a.getMut()` on an `Arc<T>` returns `(borrow mut T)?`: the value, lent exclusively for as long as `a` is
   borrowed, when `a` is the value's only owner, and `null` while another owner exists.
2. It takes `borrow mut this`. A new owner is only made by `clone` of an existing one, through a borrow of it; the
   other owners do not exist when the count is 1, and `a` cannot be borrowed while it is lent mutably, so the count
   stays 1 for the whole loan. `Arc` has no weak references, which in Rust are the other way a count can rise.
3. The count is read with a sequentially consistent load. An owner on another thread that was dropped decremented
   the count with a sequentially consistent `fetchSub` after its last use of the value, so that use happens before
   `getMut`'s load reads 1: the same ordering the last owner's drop relies on to destroy the value.
4. Values that several owners change at once are put in a `Mutex`: `Arc<Mutex<T>>`, which the spec and the guide
   now say next to `getMut`.

**Not added: `makeUnique` / Rust's `make_mut`** (clone the value when it is shared, then lend it). CeeSafe has no
general clone interface for `T` (a vector's `clone` needs Copy elements and returns a `Result`), and the program
already writes it in three lines with `getMut`, choosing how to copy: `match (a.getMut()) { Some(p) => …, null => {
a = Arc<T>.create(copy(a.get())?)?; … } }`. **Not added: `isUnique()`**: through a shared borrow of `a`, another
thread may clone `a` at once, so its answer could be stale; `getMut` holds the borrow that makes the answer true.

## 4. Tests

- `run-pass/vector-drain.cee`: elements with a printing drop, a loop that leaves early (the rest destroyed first to
  last), capacity kept, a field drained through `borrow mut this`, strings moved out, an empty vector, a
  `FailingAllocator` and a `SecretHeap` vector; with `--check-leaks`, and with `run --interp`.
- `tools/collection_properties.py`: the vector model now drains (half taken, the rest destroyed) and sorts stably
  (ADR 0225) among its random operations, the model being Python's list.
- `run-pass/arc-get-mut.cee`: `getMut` before and after a clone, a clone moved into a thread and joined, and the
  `Arc<Mutex<T>>` pattern; built and interpreted.
- The borrow rule: `for (x in v.drain()) { v.push(x)!; }` is N0241 ("cannot borrow 'v' as mutable while 'v' is
  mutably borrowed"), checked by hand.

## 5. Findings the tree already answers

- **Finding 24, process-wide statics with interior mutability.** ADR 0211 (released in ceesafec 0.94.5; the port
  used 0.94.4) implemented `Mutex<T>.constant(v)`, `OnceLock<T>.constant()` and `Vector<T>.empty()` as static
  initialisers, as the guide (guide/threads.md, "In a static") describes; `run-pass/static-locks.cee` has a static
  `Mutex<Vector<u64>>` that three threads push to at once. A registry keyed by file needs a map in a static, which
  is not yet a static initialiser (ADR 0211 §2.2 made only empty vectors and strings constants): a `Mutex<Vector<Entry>>` searched linearly, or a `OnceLock<Mutex<HashMap<…>>>`
  filled with `tryInit`, works today.
- **Finding 5, appending a character.** `s.appendChar(c)` and `s.appendUtf8(bytes)` (ADR 0138 §6) are documented in
  guide/builtins.md ("`StringView` and `String`"), spec/api/core.md (`interface AppendText`) and type-system.md
  §23.2. `s.appendChar(b as u32)` appends an ASCII byte and keeps the string UTF-8, so no `appendByte` is added: a
  byte above 0x7F would either be refused (a second way to write `appendChar`) or break the invariant.

## 6. Alternatives not taken

- An owning iterator over `move v` with an index and unsafe reads: faster by the n/2 swaps, but unsafe code for no
  measurable gain in the port.
- `Arc.getMut` returning a guard: the borrow of `a` already keeps the count at 1, so a guard would add nothing.

## 7. Questions for the owner

1. Accept `drain()` as a borrowing iterator, and leave `drain(range)` for later?
2. Accept `getMut` without `makeUnique`?
