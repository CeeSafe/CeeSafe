# lib/core

- [struct PhysAddr](#struct-physaddr)
- [struct VirtAddr](#struct-virtaddr)
- [struct Layout](#struct-layout)
- [interface Allocator](#interface-allocator)
- [enum AsyncError](#enum-asyncerror)
- [struct Timer](#struct-timer)
- [struct AsyncControl](#struct-asynccontrol)
- [struct AsyncLeaf](#struct-asyncleaf)
- [struct Raced](#struct-raced)
- [struct TaskSet](#struct-taskset)
- [struct AsyncTasks](#struct-asynctasks)
- [struct AsyncTasksIn](#struct-asynctasksin)
- [struct AsyncSignal](#struct-asyncsignal)
- [enum LoadOrdering](#enum-loadordering)
- [enum StoreOrdering](#enum-storeordering)
- [enum Ordering](#enum-ordering)
- [struct AtomicU64](#struct-atomicu64)
- [struct AtomicU32](#struct-atomicu32)
- [struct AtomicU8](#struct-atomicu8)
- [struct AtomicUSize](#struct-atomicusize)
- [struct AtomicBool](#struct-atomicbool)
- [struct ByteOrder](#struct-byteorder)
- [struct VectorBytes](#struct-vectorbytes)
- [struct RawCell](#struct-rawcell)
- [struct Cell](#struct-cell)
- [struct RefCell](#struct-refcell)
- [struct RefCellTaking](#struct-refcelltaking)
- [struct CellRef](#struct-cellref)
- [struct CellRefMut](#struct-cellrefmut)
- [struct OnceCell](#struct-oncecell)
- [struct CStr](#struct-cstr)
- [interface DerivedEqual](#interface-derivedequal)
- [struct DerivedEqualElements](#struct-derivedequalelements)
- [struct TextWriter](#struct-textwriter)
- [enum TextAlign](#enum-textalign)
- [struct Formatted](#struct-formatted)
- [interface AppendFormatted](#interface-appendformatted)
- [interface Iterator](#interface-iterator)
- [struct Volatile](#struct-volatile)
- [struct MmioPtr](#struct-mmioptr)
- [struct NumberParsing](#struct-numberparsing)
- [struct ResultMethods](#struct-resultmethods)
- [struct OptionalResults](#struct-optionalresults)
- [struct SliceHalves](#struct-slicehalves)
- [struct SliceHalvesMut](#struct-slicehalvesmut)
- [struct SliceSplitting](#struct-slicesplitting)
- [struct SliceAlgorithms](#struct-slicealgorithms)
- [struct VectorEditing](#struct-vectorediting)
- [struct VectorCopying](#struct-vectorcopying)
- [struct VectorParts](#struct-vectorparts)
- [struct VectorRaw](#struct-vectorraw)
- [struct VectorForeign](#struct-vectorforeign)
- [struct TextHalves](#struct-texthalves)
- [struct ByteHalves](#struct-bytehalves)
- [struct TextSplit](#struct-textsplit)
- [struct ByteSplit](#struct-bytesplit)
- [struct TextSearch](#struct-textsearch)
- [struct ByteSearch](#struct-bytesearch)
- [struct StringCopying](#struct-stringcopying)

## struct PhysAddr

```ceesafe
public struct PhysAddr
```

A physical address.

### PhysAddr.of

```ceesafe
public fn of(bits: u64) -> PhysAddr
```

The address whose value is `bits`.

### PhysAddr.bits

```ceesafe
public fn bits(this) -> u64
```

The address's value.

### PhysAddr.offset

```ceesafe
public fn offset(this, n: u64) -> PhysAddr
```

The address `n` bytes further on; panics if that overflows a `u64`.

## struct VirtAddr

```ceesafe
public struct VirtAddr
```

A virtual address.

### VirtAddr.of

```ceesafe
public fn of(bits: u64) -> VirtAddr
```

The address whose value is `bits`.

### VirtAddr.bits

```ceesafe
public fn bits(this) -> u64
```

The address's value.

### VirtAddr.offset

```ceesafe
public fn offset(this, n: u64) -> VirtAddr
```

The address `n` bytes further on; panics if that overflows a `u64`.

## struct Layout

```ceesafe
public struct Layout
```

The size in bytes and the alignment (a power of two) of a block of memory.

## interface Allocator

```ceesafe
public interface Allocator {
    fn allocate(borrow this, layout: Layout) -> Result<*mut u8, AllocError>;
    unsafe fn deallocate(borrow this, p: *mut u8, layout: Layout);
}
```

An allocator a `Box<T, A>` can use: a struct without fields that implements this (type-system.md §30.2).

## enum AsyncError

```ceesafe
public enum AsyncError {
    Cancelled,
    TimedOut,
    @from Io(IoError),
    Closed,
}
```

Why an async operation failed (type-system.md §40.4, §40.7). `Io` is `@from` (ADR 0111): in a function that
returns `Result<_, AsyncError>`, `?` on a `Result<_, IoError>` (a socket's `bind`, `port`, `shutdownWrite`)
returns `Err(AsyncError.Io(e))`. `Closed` (ADR 0128): the other side of this channel, or this endpoint, was closed,
so the operation can never succeed; it is what `?` makes of a send to a closed channel (`SendError.reason`).

## struct Timer

```ceesafe
public struct Timer
```

Time for tasks: the monotonic clock in milliseconds.

### Timer.now

```ceesafe
public fn now() -> u64
```

Milliseconds on the monotonic clock, from an arbitrary start.
Safety: ceesafe_async_now only reads the clock.

### Timer.sleep

```ceesafe
public async fn sleep(ms: u64) -> Result<void, AsyncError>
```

Waits at least `ms` milliseconds without blocking the thread: other tasks run meanwhile.
Safety: ceesafe_async_wait_until only records the deadline for the executor.
A cancelled task's sleep returns `Err(AsyncError.Cancelled)` at once (type-system.md §40.7).

## struct AsyncControl

```ceesafe
public struct AsyncControl<E>
```

What `Async.isCancelled()` and `Async.checkpoint(cancelled)` call (type-system.md §40.4).

### AsyncControl.isCancelled

```ceesafe
public fn isCancelled() -> bool
```

Whether the current task is cancelled and not shielded.
Safety: ceesafe_async_status only reads the executor's state.

### AsyncControl.enterShield

```ceesafe
public fn enterShield(ms: u64)
```

`Async.shield`'s start (type-system.md §40.7).
Safety: it only counts the current task's shields.

### AsyncControl.leaveShield

```ceesafe
public fn leaveShield()
```

`Async.shield`'s end.
Safety: it only counts the current task's shields, after enterShield.

### AsyncControl.checkpoint

```ceesafe
public fn checkpoint(cancelled: E) -> Result<void, E>
```

`Err(cancelled)` if the current task is cancelled and not shielded, else `Ok`.

## struct AsyncLeaf

```ceesafe
public struct AsyncLeaf
```

The library's own steps for its waiting operations: not for programs (type-system.md §40.4).

### AsyncLeaf.status

```ceesafe
public fn status() -> Result<void, AsyncError>
```

Err(Cancelled) if the current task is cancelled, Err(TimedOut) if its shield's time is up (type-system.md §40.7).
Safety: ceesafe_async_status only reads the executor's state.

## struct Raced

```ceesafe
public struct Raced<T> {
    public winner: u32;
    public value: T;
    public other: T?;
}
```

`Async.race`'s result (type-system.md §40.4): the child that returned `Ok` first (0 or 1), its value, and the
other's value if it also returned `Ok` before it saw its cancellation.

## struct TaskSet

```ceesafe
@noncopy // the one handle of its record
public struct TaskSet<E: Copy>
```

The set `Async.tasks`' body is given (ADR 0053 §3.1). The body, and the functions it passes the set to, start
children with `await set.spawn(async move () => …)`, which waits while the set's limit of children run, and
`set.trySpawn(async move () => …)`, which is false instead. A child owns what it captures: no loans, so it cannot
capture the set (N0349). The calls are the checker's (src/check/async.cee): these are the steps it joins.

## struct AsyncTasks

```ceesafe
public struct AsyncTasks<E: Copy + Send>
```

`await Async.tasks(limit, oom, async (set) => …)` (ADR 0053): `Async.tasksIn` with the system heap.

### AsyncTasks.run

```ceesafe
@effectsOf(body)
public async fn run(
    limit: u32,
    oom: E,
    body: borrow mut fn(borrow TaskSet<E>) -> Result<void, E>,
) -> Result<void, E>
```

## struct AsyncTasksIn

```ceesafe
public struct AsyncTasksIn<E: Copy + Send, A>
```

`await Async.tasksIn(alloc, limit, oom, async (set) => …)` (ADR 0053): the body runs in the awaiting task, as the
set's owner, with the set; then every child it started is waited for. The children's tasks and frames come from
`alloc`'s allocator type. The result is the body's error, else the first child's, else `oom` if a child's frame
could not be allocated, else Ok. The body's error cancels the children; a child's error cancels no sibling.

### AsyncTasksIn.run

```ceesafe
@effectsOf(body)
public async fn run(
    alloc: A,
    limit: u32,
    oom: E,
    body: borrow mut fn(borrow TaskSet<E>) -> Result<void, E>,
) -> Result<void, E>
```

Safety: the record is this frame's, which outlives every child: the set waits for them before it returns.

## struct AsyncSignal

```ceesafe
@noncopy // one signal, shared: a copy would never be raised
public struct AsyncSignal
```

A one-shot wake-up from anywhere, to a task (ADR 0039): `await signal.wait()` parks the task until `signal.raise()`.
`raise` has no effects, so an interrupt handler (`@context(irq)`) or another thread may call it, the way a driver
tells the task waiting for its device. A raise before the wait is kept: the wait then returns at once. One task
waits on a signal at a time.

### AsyncSignal.create

```ceesafe
public fn create() -> AsyncSignal
```

A signal not raised.

### AsyncSignal.constant

```ceesafe
@constant
public fn constant() -> AsyncSignal
```

The same as create, in a static's initialiser (type-system.md §39.4): `static READY: AsyncSignal =
AsyncSignal.constant();`, which an interrupt handler can raise.

### AsyncSignal.raise

```ceesafe
@effects()
public fn raise(borrow this)
```

Raises the signal: the task waiting on it, if any, runs again.
Safety: the runtime changes the record atomically, and only wakes the task it names.

### AsyncSignal.wait

```ceesafe
public async fn wait(borrow this) -> Result<void, AsyncError>
```

Waits until the signal is raised, then lowers it. A cancelled task returns `Err(AsyncError.Cancelled)` and no
longer waits (type-system.md §40.7).
Safety: the record names this task only while it waits here; ceesafe_async_signal_forget takes it out.

## enum LoadOrdering

```ceesafe
public enum LoadOrdering {
    Relaxed,
    Acquire,
    SequentiallyConsistent,
}
```

The ordering of an atomic load (ADR 0021): C11's memory_order_relaxed, _acquire and _seq_cst.

## enum StoreOrdering

```ceesafe
public enum StoreOrdering {
    Relaxed,
    Release,
    SequentiallyConsistent,
}
```

The ordering of an atomic store: C11's memory_order_relaxed, _release and _seq_cst.

## enum Ordering

```ceesafe
public enum Ordering {
    Relaxed,
    Acquire,
    Release,
    AcquireRelease,
    SequentiallyConsistent,
}
```

The ordering of an atomic read-modify-write (swap, compare-exchange, add, subtract): C11's memory_order_relaxed,
_acquire, _release, _acq_rel and _seq_cst. A compare-exchange that fails orders as a load: AcquireRelease as
Acquire, Release as Relaxed, the others as themselves.

## struct AtomicU64

```ceesafe
@threadSafe
@noncopy
public struct AtomicU64
```

A u64 that threads may read and change at once: every operation is atomic and sequentially consistent.

### AtomicU64.create

```ceesafe
public fn create(value: u64) -> AtomicU64
```

A new atomic holding `value`.

### AtomicU64.constant

```ceesafe
@constant
public fn constant(value: u64) -> AtomicU64
```

The same as create, in a static's initialiser (type-system.md §39.4): `static N: AtomicU64 = AtomicU64.constant(0);`.

### AtomicU64.load

```ceesafe
public fn load(borrow this) -> u64
```

Safety: the runtime accesses `value` only atomically, and safe code reaches it only through these methods.

### AtomicU64.store

```ceesafe
public fn store(borrow this, value: u64)
```

Safety: as load.

### AtomicU64.swap

```ceesafe
public fn swap(borrow this, value: u64) -> u64
```

Safety: as load.

### AtomicU64.compareExchange

```ceesafe
public fn compareExchange(borrow this, expected: u64, desired: u64) -> Result<u64, u64>
```

Ok(the old value) if it was `expected` and is now `desired`; else Err(the value it has).
Safety: as load.

### AtomicU64.fetchAdd

```ceesafe
public fn fetchAdd(borrow this, value: u64) -> u64
```

Adds, wrapping; returns the old value.
Safety: as load.

### AtomicU64.fetchSub

```ceesafe
public fn fetchSub(borrow this, value: u64) -> u64
```

Subtracts, wrapping; returns the old value.
Safety: as load.

### AtomicU64.fetchOr

```ceesafe
public fn fetchOr(borrow this, value: u64) -> u64
```

Sets the bits of `value`; the value before (ADR 0041).
Safety: as load.

### AtomicU64.fetchAnd

```ceesafe
public fn fetchAnd(borrow this, value: u64) -> u64
```

Keeps only the bits of `value`; the value before (ADR 0041).
Safety: as load.

### AtomicU64.loadWith

```ceesafe
public fn loadWith(borrow this, order: LoadOrdering) -> u64
```

load, with an ordering (ADR 0021).
Safety: as load.

### AtomicU64.storeWith

```ceesafe
public fn storeWith(borrow this, value: u64, order: StoreOrdering)
```

store, with an ordering.
Safety: as load.

### AtomicU64.swapWith

```ceesafe
public fn swapWith(borrow this, value: u64, order: Ordering) -> u64
```

swap, with an ordering.
Safety: as load.

### AtomicU64.compareExchangeWith

```ceesafe
public fn compareExchangeWith(borrow this, expected: u64, desired: u64, order: Ordering) -> Result<u64, u64>
```

compareExchange, with an ordering; a failure orders as Ordering says.
Safety: as load.

### AtomicU64.fetchAddWith

```ceesafe
public fn fetchAddWith(borrow this, value: u64, order: Ordering) -> u64
```

fetchAdd, with an ordering.
Safety: as load.

### AtomicU64.fetchSubWith

```ceesafe
public fn fetchSubWith(borrow this, value: u64, order: Ordering) -> u64
```

fetchSub, with an ordering.
Safety: as load.

### AtomicU64.fetchOrWith

```ceesafe
public fn fetchOrWith(borrow this, value: u64, order: Ordering) -> u64
```

fetchOr, with an ordering.
Safety: as load.

### AtomicU64.fetchAndWith

```ceesafe
public fn fetchAndWith(borrow this, value: u64, order: Ordering) -> u64
```

fetchAnd, with an ordering.
Safety: as load.

## struct AtomicU32

```ceesafe
@threadSafe
@noncopy
public struct AtomicU32
```

A u32 that threads may read and change at once, as AtomicU64.

### AtomicU32.create

```ceesafe
public fn create(value: u32) -> AtomicU32
```

A new atomic holding `value`.

### AtomicU32.constant

```ceesafe
@constant
public fn constant(value: u32) -> AtomicU32
```

The same as create, in a static's initialiser (type-system.md §39.4): `static N: AtomicU32 = AtomicU32.constant(0);`.

### AtomicU32.load

```ceesafe
public fn load(borrow this) -> u32
```

Safety: as AtomicU64.load.

### AtomicU32.store

```ceesafe
public fn store(borrow this, value: u32)
```

Safety: as AtomicU64.load.

### AtomicU32.swap

```ceesafe
public fn swap(borrow this, value: u32) -> u32
```

Safety: as AtomicU64.load.

### AtomicU32.compareExchange

```ceesafe
public fn compareExchange(borrow this, expected: u32, desired: u32) -> Result<u32, u32>
```

Safety: as AtomicU64.load.

### AtomicU32.fetchAdd

```ceesafe
public fn fetchAdd(borrow this, value: u32) -> u32
```

Safety: as AtomicU64.load.

### AtomicU32.fetchSub

```ceesafe
public fn fetchSub(borrow this, value: u32) -> u32
```

Safety: as AtomicU64.load.

### AtomicU32.fetchOr

```ceesafe
public fn fetchOr(borrow this, value: u32) -> u32
```

Sets the bits of `value`; the value before (ADR 0041).
Safety: as load.

### AtomicU32.fetchAnd

```ceesafe
public fn fetchAnd(borrow this, value: u32) -> u32
```

Keeps only the bits of `value`; the value before (ADR 0041).
Safety: as load.

### AtomicU32.loadWith

```ceesafe
public fn loadWith(borrow this, order: LoadOrdering) -> u32
```

load, with an ordering (ADR 0021).
Safety: as load.

### AtomicU32.storeWith

```ceesafe
public fn storeWith(borrow this, value: u32, order: StoreOrdering)
```

store, with an ordering.
Safety: as load.

### AtomicU32.swapWith

```ceesafe
public fn swapWith(borrow this, value: u32, order: Ordering) -> u32
```

swap, with an ordering.
Safety: as load.

### AtomicU32.compareExchangeWith

```ceesafe
public fn compareExchangeWith(borrow this, expected: u32, desired: u32, order: Ordering) -> Result<u32, u32>
```

compareExchange, with an ordering; a failure orders as Ordering says.
Safety: as load.

### AtomicU32.fetchAddWith

```ceesafe
public fn fetchAddWith(borrow this, value: u32, order: Ordering) -> u32
```

fetchAdd, with an ordering.
Safety: as load.

### AtomicU32.fetchSubWith

```ceesafe
public fn fetchSubWith(borrow this, value: u32, order: Ordering) -> u32
```

fetchSub, with an ordering.
Safety: as load.

### AtomicU32.fetchOrWith

```ceesafe
public fn fetchOrWith(borrow this, value: u32, order: Ordering) -> u32
```

fetchOr, with an ordering.
Safety: as load.

### AtomicU32.fetchAndWith

```ceesafe
public fn fetchAndWith(borrow this, value: u32, order: Ordering) -> u32
```

fetchAnd, with an ordering.
Safety: as load.

## struct AtomicU8

```ceesafe
@threadSafe
@noncopy
public struct AtomicU8
```

A u8 that threads may read and change at once, as AtomicU32: for a byte member that C reads and writes atomically
(ADR 0080 §3.5), such as a process's flag.

### AtomicU8.create

```ceesafe
public fn create(value: u8) -> AtomicU8
```

A new atomic holding `value`.

### AtomicU8.constant

```ceesafe
@constant
public fn constant(value: u8) -> AtomicU8
```

The same as create, in a static's initialiser (type-system.md §39.4).

### AtomicU8.load

```ceesafe
public fn load(borrow this) -> u8
```

Safety: as AtomicU64.load.

### AtomicU8.store

```ceesafe
public fn store(borrow this, value: u8)
```

Safety: as AtomicU64.load.

### AtomicU8.swap

```ceesafe
public fn swap(borrow this, value: u8) -> u8
```

Safety: as AtomicU64.load.

### AtomicU8.compareExchange

```ceesafe
public fn compareExchange(borrow this, expected: u8, desired: u8) -> Result<u8, u8>
```

Safety: as AtomicU64.load.

### AtomicU8.loadWith

```ceesafe
public fn loadWith(borrow this, order: LoadOrdering) -> u8
```

load, with an ordering (ADR 0021).
Safety: as load.

### AtomicU8.storeWith

```ceesafe
public fn storeWith(borrow this, value: u8, order: StoreOrdering)
```

store, with an ordering.
Safety: as load.

## struct AtomicUSize

```ceesafe
@threadSafe
@noncopy
public struct AtomicUSize
```

A usize that threads may read and change at once, as AtomicU64 (usize is 64 bits on every target, §29.3).

### AtomicUSize.create

```ceesafe
public fn create(value: usize) -> AtomicUSize
```

A new atomic holding `value`.

### AtomicUSize.constant

```ceesafe
@constant
public fn constant(value: usize) -> AtomicUSize
```

The same as create, in a static's initialiser (type-system.md §39.4).

### AtomicUSize.load

```ceesafe
public fn load(borrow this) -> usize
```

The value, sequentially consistent.

### AtomicUSize.store

```ceesafe
public fn store(borrow this, value: usize)
```

Stores `value`, sequentially consistent.

### AtomicUSize.swap

```ceesafe
public fn swap(borrow this, value: usize) -> usize
```

Stores `value` and returns the old value, sequentially consistent.

### AtomicUSize.compareExchange

```ceesafe
public fn compareExchange(borrow this, expected: usize, desired: usize) -> Result<usize, usize>
```

Ok(the old value) if it was `expected` and is now `desired`; else Err(the value it has).

### AtomicUSize.fetchAdd

```ceesafe
public fn fetchAdd(borrow this, value: usize) -> usize
```

Adds, wrapping; returns the old value.

### AtomicUSize.fetchSub

```ceesafe
public fn fetchSub(borrow this, value: usize) -> usize
```

Subtracts, wrapping; returns the old value.

### AtomicUSize.fetchOr

```ceesafe
public fn fetchOr(borrow this, value: usize) -> usize
```

As fetchOr.

### AtomicUSize.fetchAnd

```ceesafe
public fn fetchAnd(borrow this, value: usize) -> usize
```

As fetchAnd.

### AtomicUSize.loadWith

```ceesafe
public fn loadWith(borrow this, order: LoadOrdering) -> usize
```

As load, with `order`.

### AtomicUSize.storeWith

```ceesafe
public fn storeWith(borrow this, value: usize, order: StoreOrdering)
```

As store, with `order`.

### AtomicUSize.swapWith

```ceesafe
public fn swapWith(borrow this, value: usize, order: Ordering) -> usize
```

As swap, with `order`.

### AtomicUSize.compareExchangeWith

```ceesafe
public fn compareExchangeWith(
    borrow this,
    expected: usize,
    desired: usize,
    order: Ordering,
) -> Result<usize, usize>
```

As compareExchange, with `order` (a failure orders as a load, §38.3).

### AtomicUSize.fetchAddWith

```ceesafe
public fn fetchAddWith(borrow this, value: usize, order: Ordering) -> usize
```

As fetchAdd, with `order`.

### AtomicUSize.fetchSubWith

```ceesafe
public fn fetchSubWith(borrow this, value: usize, order: Ordering) -> usize
```

As fetchSub, with `order`.

### AtomicUSize.fetchOrWith

```ceesafe
public fn fetchOrWith(borrow this, value: usize, order: Ordering) -> usize
```

As fetchOrWith.

### AtomicUSize.fetchAndWith

```ceesafe
public fn fetchAndWith(borrow this, value: usize, order: Ordering) -> usize
```

As fetchAndWith.

## struct AtomicBool

```ceesafe
@threadSafe
@noncopy
public struct AtomicBool
```

A bool that threads may read and change at once.

### AtomicBool.create

```ceesafe
public fn create(value: bool) -> AtomicBool
```

A new atomic holding `value`.

### AtomicBool.constant

```ceesafe
@constant
public fn constant(value: bool) -> AtomicBool
```

The same as create, in a static's initialiser (type-system.md §39.4).

### AtomicBool.load

```ceesafe
public fn load(borrow this) -> bool
```

The value, sequentially consistent.

### AtomicBool.store

```ceesafe
public fn store(borrow this, value: bool)
```

Stores `value`, sequentially consistent.

### AtomicBool.swap

```ceesafe
public fn swap(borrow this, value: bool) -> bool
```

Stores `value` and returns the old value, sequentially consistent.

### AtomicBool.compareExchange

```ceesafe
public fn compareExchange(borrow this, expected: bool, desired: bool) -> Result<bool, bool>
```

Ok(the old value) if it was `expected` and is now `desired`; else Err(the value it has).

### AtomicBool.loadWith

```ceesafe
public fn loadWith(borrow this, order: LoadOrdering) -> bool
```

As load, with `order`.

### AtomicBool.storeWith

```ceesafe
public fn storeWith(borrow this, value: bool, order: StoreOrdering)
```

As store, with `order`.

### AtomicBool.swapWith

```ceesafe
public fn swapWith(borrow this, value: bool, order: Ordering) -> bool
```

As swap, with `order`.

### AtomicBool.compareExchangeWith

```ceesafe
public fn compareExchangeWith(borrow this, expected: bool, desired: bool, order: Ordering) -> Result<bool, bool>
```

As compareExchange, with `order` (a failure orders as a load, §38.3).

## struct ByteOrder

```ceesafe
public struct ByteOrder
```

Reading and writing integers in a byte order: static functions; the struct has no fields and is never made.

### ByteOrder.readU8BE

```ceesafe
public fn readU8BE(s: Slice<u8>, at: usize) -> u8
```

The `u8` in the 1 byte at `at`, big-endian.

### ByteOrder.readI8BE

```ceesafe
public fn readI8BE(s: Slice<u8>, at: usize) -> i8
```

The `i8` in the 1 byte at `at`, big-endian, in two's complement.

### ByteOrder.writeU8BE

```ceesafe
public fn writeU8BE(s: SliceMut<u8>, at: usize, x: u8)
```

Writes `x` to the 1 byte at `at`, big-endian.

### ByteOrder.writeI8BE

```ceesafe
public fn writeI8BE(s: SliceMut<u8>, at: usize, x: i8)
```

Writes `x` to the 1 byte at `at`, big-endian, in two's complement.

### ByteOrder.readU8LE

```ceesafe
public fn readU8LE(s: Slice<u8>, at: usize) -> u8
```

The `u8` in the 1 byte at `at`, little-endian.

### ByteOrder.readI8LE

```ceesafe
public fn readI8LE(s: Slice<u8>, at: usize) -> i8
```

The `i8` in the 1 byte at `at`, little-endian, in two's complement.

### ByteOrder.writeU8LE

```ceesafe
public fn writeU8LE(s: SliceMut<u8>, at: usize, x: u8)
```

Writes `x` to the 1 byte at `at`, little-endian.

### ByteOrder.writeI8LE

```ceesafe
public fn writeI8LE(s: SliceMut<u8>, at: usize, x: i8)
```

Writes `x` to the 1 byte at `at`, little-endian, in two's complement.

### ByteOrder.readU16BE

```ceesafe
public fn readU16BE(s: Slice<u8>, at: usize) -> u16
```

The `u16` in the 2 bytes at `at`, big-endian.

### ByteOrder.readI16BE

```ceesafe
public fn readI16BE(s: Slice<u8>, at: usize) -> i16
```

The `i16` in the 2 bytes at `at`, big-endian, in two's complement.

### ByteOrder.writeU16BE

```ceesafe
public fn writeU16BE(s: SliceMut<u8>, at: usize, x: u16)
```

Writes `x` to the 2 bytes at `at`, big-endian.

### ByteOrder.writeI16BE

```ceesafe
public fn writeI16BE(s: SliceMut<u8>, at: usize, x: i16)
```

Writes `x` to the 2 bytes at `at`, big-endian, in two's complement.

### ByteOrder.readU16LE

```ceesafe
public fn readU16LE(s: Slice<u8>, at: usize) -> u16
```

The `u16` in the 2 bytes at `at`, little-endian.

### ByteOrder.readI16LE

```ceesafe
public fn readI16LE(s: Slice<u8>, at: usize) -> i16
```

The `i16` in the 2 bytes at `at`, little-endian, in two's complement.

### ByteOrder.writeU16LE

```ceesafe
public fn writeU16LE(s: SliceMut<u8>, at: usize, x: u16)
```

Writes `x` to the 2 bytes at `at`, little-endian.

### ByteOrder.writeI16LE

```ceesafe
public fn writeI16LE(s: SliceMut<u8>, at: usize, x: i16)
```

Writes `x` to the 2 bytes at `at`, little-endian, in two's complement.

### ByteOrder.readU32BE

```ceesafe
public fn readU32BE(s: Slice<u8>, at: usize) -> u32
```

The `u32` in the 4 bytes at `at`, big-endian.

### ByteOrder.readI32BE

```ceesafe
public fn readI32BE(s: Slice<u8>, at: usize) -> i32
```

The `i32` in the 4 bytes at `at`, big-endian, in two's complement.

### ByteOrder.writeU32BE

```ceesafe
public fn writeU32BE(s: SliceMut<u8>, at: usize, x: u32)
```

Writes `x` to the 4 bytes at `at`, big-endian.

### ByteOrder.writeI32BE

```ceesafe
public fn writeI32BE(s: SliceMut<u8>, at: usize, x: i32)
```

Writes `x` to the 4 bytes at `at`, big-endian, in two's complement.

### ByteOrder.readU32LE

```ceesafe
public fn readU32LE(s: Slice<u8>, at: usize) -> u32
```

The `u32` in the 4 bytes at `at`, little-endian.

### ByteOrder.readI32LE

```ceesafe
public fn readI32LE(s: Slice<u8>, at: usize) -> i32
```

The `i32` in the 4 bytes at `at`, little-endian, in two's complement.

### ByteOrder.writeU32LE

```ceesafe
public fn writeU32LE(s: SliceMut<u8>, at: usize, x: u32)
```

Writes `x` to the 4 bytes at `at`, little-endian.

### ByteOrder.writeI32LE

```ceesafe
public fn writeI32LE(s: SliceMut<u8>, at: usize, x: i32)
```

Writes `x` to the 4 bytes at `at`, little-endian, in two's complement.

### ByteOrder.readU64BE

```ceesafe
public fn readU64BE(s: Slice<u8>, at: usize) -> u64
```

The `u64` in the 8 bytes at `at`, big-endian.

### ByteOrder.readI64BE

```ceesafe
public fn readI64BE(s: Slice<u8>, at: usize) -> i64
```

The `i64` in the 8 bytes at `at`, big-endian, in two's complement.

### ByteOrder.writeU64BE

```ceesafe
public fn writeU64BE(s: SliceMut<u8>, at: usize, x: u64)
```

Writes `x` to the 8 bytes at `at`, big-endian.

### ByteOrder.writeI64BE

```ceesafe
public fn writeI64BE(s: SliceMut<u8>, at: usize, x: i64)
```

Writes `x` to the 8 bytes at `at`, big-endian, in two's complement.

### ByteOrder.readU64LE

```ceesafe
public fn readU64LE(s: Slice<u8>, at: usize) -> u64
```

The `u64` in the 8 bytes at `at`, little-endian.

### ByteOrder.readI64LE

```ceesafe
public fn readI64LE(s: Slice<u8>, at: usize) -> i64
```

The `i64` in the 8 bytes at `at`, little-endian, in two's complement.

### ByteOrder.writeU64LE

```ceesafe
public fn writeU64LE(s: SliceMut<u8>, at: usize, x: u64)
```

Writes `x` to the 8 bytes at `at`, little-endian.

### ByteOrder.writeI64LE

```ceesafe
public fn writeI64LE(s: SliceMut<u8>, at: usize, x: i64)
```

Writes `x` to the 8 bytes at `at`, little-endian, in two's complement.

## struct VectorBytes

```ceesafe
public struct VectorBytes
```

Appending integers to a byte vector in a byte order: `v.pushBE(x)` and `v.pushLE(x)` (ADR 0071 §2.2). Each is one
`extend` of the bytes, so it grows the vector at most once and leaves it unchanged if that fails. A vector's
allocator is `SystemHeap` or `FailingAllocator` (type-system.md §23.1); the `…Failing` functions are the latter's.

### VectorBytes.pushU8BE

```ceesafe
public fn pushU8BE(v: borrow mut Vector<u8>, x: u8) -> Result<void, AllocError>
```

Appends `x` as 1 byte, big-endian.

### VectorBytes.pushI8BE

```ceesafe
public fn pushI8BE(v: borrow mut Vector<u8>, x: i8) -> Result<void, AllocError>
```

Appends `x` as 1 byte, big-endian, in two's complement.

### VectorBytes.pushU8LE

```ceesafe
public fn pushU8LE(v: borrow mut Vector<u8>, x: u8) -> Result<void, AllocError>
```

Appends `x` as 1 byte, little-endian.

### VectorBytes.pushI8LE

```ceesafe
public fn pushI8LE(v: borrow mut Vector<u8>, x: i8) -> Result<void, AllocError>
```

Appends `x` as 1 byte, little-endian, in two's complement.

### VectorBytes.pushU16BE

```ceesafe
public fn pushU16BE(v: borrow mut Vector<u8>, x: u16) -> Result<void, AllocError>
```

Appends `x` as 2 bytes, big-endian.

### VectorBytes.pushI16BE

```ceesafe
public fn pushI16BE(v: borrow mut Vector<u8>, x: i16) -> Result<void, AllocError>
```

Appends `x` as 2 bytes, big-endian, in two's complement.

### VectorBytes.pushU16LE

```ceesafe
public fn pushU16LE(v: borrow mut Vector<u8>, x: u16) -> Result<void, AllocError>
```

Appends `x` as 2 bytes, little-endian.

### VectorBytes.pushI16LE

```ceesafe
public fn pushI16LE(v: borrow mut Vector<u8>, x: i16) -> Result<void, AllocError>
```

Appends `x` as 2 bytes, little-endian, in two's complement.

### VectorBytes.pushU32BE

```ceesafe
public fn pushU32BE(v: borrow mut Vector<u8>, x: u32) -> Result<void, AllocError>
```

Appends `x` as 4 bytes, big-endian.

### VectorBytes.pushI32BE

```ceesafe
public fn pushI32BE(v: borrow mut Vector<u8>, x: i32) -> Result<void, AllocError>
```

Appends `x` as 4 bytes, big-endian, in two's complement.

### VectorBytes.pushU32LE

```ceesafe
public fn pushU32LE(v: borrow mut Vector<u8>, x: u32) -> Result<void, AllocError>
```

Appends `x` as 4 bytes, little-endian.

### VectorBytes.pushI32LE

```ceesafe
public fn pushI32LE(v: borrow mut Vector<u8>, x: i32) -> Result<void, AllocError>
```

Appends `x` as 4 bytes, little-endian, in two's complement.

### VectorBytes.pushU64BE

```ceesafe
public fn pushU64BE(v: borrow mut Vector<u8>, x: u64) -> Result<void, AllocError>
```

Appends `x` as 8 bytes, big-endian.

### VectorBytes.pushI64BE

```ceesafe
public fn pushI64BE(v: borrow mut Vector<u8>, x: i64) -> Result<void, AllocError>
```

Appends `x` as 8 bytes, big-endian, in two's complement.

### VectorBytes.pushU64LE

```ceesafe
public fn pushU64LE(v: borrow mut Vector<u8>, x: u64) -> Result<void, AllocError>
```

Appends `x` as 8 bytes, little-endian.

### VectorBytes.pushI64LE

```ceesafe
public fn pushI64LE(v: borrow mut Vector<u8>, x: i64) -> Result<void, AllocError>
```

Appends `x` as 8 bytes, little-endian, in two's complement.

### VectorBytes.pushU8BEFailing

```ceesafe
public fn pushU8BEFailing(v: borrow mut Vector<u8, FailingAllocator>, x: u8) -> Result<void, AllocError>
```

Appends `x` as 1 byte, big-endian.

### VectorBytes.pushI8BEFailing

```ceesafe
public fn pushI8BEFailing(v: borrow mut Vector<u8, FailingAllocator>, x: i8) -> Result<void, AllocError>
```

Appends `x` as 1 byte, big-endian, in two's complement.

### VectorBytes.pushU8LEFailing

```ceesafe
public fn pushU8LEFailing(v: borrow mut Vector<u8, FailingAllocator>, x: u8) -> Result<void, AllocError>
```

Appends `x` as 1 byte, little-endian.

### VectorBytes.pushI8LEFailing

```ceesafe
public fn pushI8LEFailing(v: borrow mut Vector<u8, FailingAllocator>, x: i8) -> Result<void, AllocError>
```

Appends `x` as 1 byte, little-endian, in two's complement.

### VectorBytes.pushU16BEFailing

```ceesafe
public fn pushU16BEFailing(v: borrow mut Vector<u8, FailingAllocator>, x: u16) -> Result<void, AllocError>
```

Appends `x` as 2 bytes, big-endian.

### VectorBytes.pushI16BEFailing

```ceesafe
public fn pushI16BEFailing(v: borrow mut Vector<u8, FailingAllocator>, x: i16) -> Result<void, AllocError>
```

Appends `x` as 2 bytes, big-endian, in two's complement.

### VectorBytes.pushU16LEFailing

```ceesafe
public fn pushU16LEFailing(v: borrow mut Vector<u8, FailingAllocator>, x: u16) -> Result<void, AllocError>
```

Appends `x` as 2 bytes, little-endian.

### VectorBytes.pushI16LEFailing

```ceesafe
public fn pushI16LEFailing(v: borrow mut Vector<u8, FailingAllocator>, x: i16) -> Result<void, AllocError>
```

Appends `x` as 2 bytes, little-endian, in two's complement.

### VectorBytes.pushU32BEFailing

```ceesafe
public fn pushU32BEFailing(v: borrow mut Vector<u8, FailingAllocator>, x: u32) -> Result<void, AllocError>
```

Appends `x` as 4 bytes, big-endian.

### VectorBytes.pushI32BEFailing

```ceesafe
public fn pushI32BEFailing(v: borrow mut Vector<u8, FailingAllocator>, x: i32) -> Result<void, AllocError>
```

Appends `x` as 4 bytes, big-endian, in two's complement.

### VectorBytes.pushU32LEFailing

```ceesafe
public fn pushU32LEFailing(v: borrow mut Vector<u8, FailingAllocator>, x: u32) -> Result<void, AllocError>
```

Appends `x` as 4 bytes, little-endian.

### VectorBytes.pushI32LEFailing

```ceesafe
public fn pushI32LEFailing(v: borrow mut Vector<u8, FailingAllocator>, x: i32) -> Result<void, AllocError>
```

Appends `x` as 4 bytes, little-endian, in two's complement.

### VectorBytes.pushU64BEFailing

```ceesafe
public fn pushU64BEFailing(v: borrow mut Vector<u8, FailingAllocator>, x: u64) -> Result<void, AllocError>
```

Appends `x` as 8 bytes, big-endian.

### VectorBytes.pushI64BEFailing

```ceesafe
public fn pushI64BEFailing(v: borrow mut Vector<u8, FailingAllocator>, x: i64) -> Result<void, AllocError>
```

Appends `x` as 8 bytes, big-endian, in two's complement.

### VectorBytes.pushU64LEFailing

```ceesafe
public fn pushU64LEFailing(v: borrow mut Vector<u8, FailingAllocator>, x: u64) -> Result<void, AllocError>
```

Appends `x` as 8 bytes, little-endian.

### VectorBytes.pushI64LEFailing

```ceesafe
public fn pushI64LEFailing(v: borrow mut Vector<u8, FailingAllocator>, x: i64) -> Result<void, AllocError>
```

Appends `x` as 8 bytes, little-endian, in two's complement.

### VectorBytes.appendHex

```ceesafe
public fn appendHex(v: borrow mut Vector<u8>, value: u64, minWidth: u32, upper: bool) -> Result<void, AllocError>
```

Appends `value` in hexadecimal, at least `minWidth` digits, uppercase when `upper` (ADR 0087 §2.9).

### VectorBytes.appendOctal

```ceesafe
public fn appendOctal(v: borrow mut Vector<u8>, value: u64) -> Result<void, AllocError>
```

Appends `value` in octal (ADR 0087 §2.9).

### VectorBytes.appendFixed

```ceesafe
public fn appendFixed(v: borrow mut Vector<u8>, value: f64, digits: u32) -> Result<void, AllocError>
```

Appends `value` with `digits` digits after the point, rounded half to even (ADR 0087 §2.9).

### VectorBytes.appendPadded

```ceesafe
public fn appendPadded(
    v: borrow mut Vector<u8>,
    text: StringView,
    width: usize,
    align: TextAlign,
    fill: u32,
) -> Result<void, AllocError>
```

Appends `text` in a field `width` characters wide, filled with `fill` where `align` puts it (ADR 0087 §2.9).

## struct RawCell

```ceesafe
@interiorMutable
@noncopy
public struct RawCell<T>
```

The primitive (§41.1): a `T` that may be read and written through `pointer()` while shared loans of the cell, or of
a value holding it, are live. It has `T`'s size and alignment. `T` may not hold loans (N0351).

### RawCell.create

```ceesafe
public fn create(value: T) -> RawCell<T>
```

A cell holding `value`.

### RawCell.pointer

```ceesafe
public fn pointer(borrow this) -> *mut T
```

The contents' address: safe to take, `unsafe` to use. Unsafe code must not make two live `borrow mut T` from
it, nor write while a `borrow T` it made is live (unsafe.md §2).

### RawCell.getMut

```ceesafe
public fn getMut(borrow mut this) -> borrow mut T
```

The contents, for as long as the cell is borrowed exclusively: no other loan of it can be live.

### RawCell.intoInner

```ceesafe
public fn intoInner(this) -> T
```

The contents, the cell consumed.

## struct Cell

```ceesafe
public struct Cell<T: Copy>
```

A `Copy` value that may be read and replaced through a shared reference (§41.2): `get` copies it out, `set` stores
a new one. No method lends a reference to the contents, so no change can be seen through a stale one, and a call
between two methods (a re-entrant callback) is harmless. `==` and `!=` compare the contents when `T` has them.

### Cell.create

```ceesafe
public fn create(value: T) -> Cell<T>
```

A cell holding `value`.

### Cell.get

```ceesafe
public fn get(borrow this) -> T
```

A copy of the contents.
Safety: no reference to the contents is ever lent, so nothing writes them during the read; `T` is Copy.

### Cell.set

```ceesafe
public fn set(borrow this, value: T)
```

Stores `value`; the old contents need no drop (`T` is Copy), so no user code runs.
Safety: as get; nothing reads the contents during the write.

### Cell.replace

```ceesafe
public fn replace(borrow this, value: T) -> T
```

Stores `value` and returns the old contents.
Safety: as set.

### Cell.getMut

```ceesafe
public fn getMut(borrow mut this) -> borrow mut T
```

The contents, for as long as the cell is borrowed exclusively.

### Cell.intoInner

```ceesafe
public fn intoInner(this) -> T
```

The contents, the cell consumed.

## struct RefCell

```ceesafe
public struct RefCell<T>
```

A value lent through a shared reference with checked access (§41.3): any number of `read` guards, or one `write`
guard, at a time. A conflict panics ("cell already borrowed"); `tryRead` and `tryWrite` return null instead. The
state is one word: 0 free, n > 0 readers, -1 writing.

### RefCell.create

```ceesafe
public fn create(value: T) -> RefCell<T>
```

A cell holding `value`, free.

### RefCell.read

```ceesafe
public fn read(borrow this) -> CellRef<T>
```

Shared access until the guard is dropped; panics while a `write` guard is live.

### RefCell.tryRead

```ceesafe
public fn tryRead(borrow this) -> CellRef<T>?
```

As read, or null while a `write` guard is live.

### RefCell.write

```ceesafe
public fn write(borrow this) -> CellRefMut<T>
```

Exclusive access until the guard is dropped; panics while any guard is live.

### RefCell.tryWrite

```ceesafe
public fn tryWrite(borrow this) -> CellRefMut<T>?
```

As write, or null while any guard is live.

### RefCell.replace

```ceesafe
public fn replace(borrow this, value: T) -> T
```

Stores `value` and returns the old contents, moved out before the return: their drop runs in the caller, with
no guard live. Panics while any guard is live.
Safety: no guard is live, so no reference to the contents is; the old value is read out before the write.

### RefCell.getMut

```ceesafe
public fn getMut(borrow mut this) -> borrow mut T
```

The contents, for as long as the cell is borrowed exclusively: no guard can be live.

### RefCell.intoInner

```ceesafe
public fn intoInner(this) -> T
```

The contents, the cell consumed.

## struct RefCellTaking

```ceesafe
public struct RefCellTaking<T>
```

`RefCell.take` on a `RefCell<T?>` (§41.3): the contents, leaving null.

### RefCellTaking.take

```ceesafe
public fn take(cell: borrow RefCell<T?>) -> T?
```

Panics while any guard is live, as replace.

## struct CellRef

```ceesafe
public struct CellRef<region r, T> {
    drop { … }
}
```

A `read` guard of a RefCell (§41.3): `data()` lends the contents shared; dropping it gives the access back.

### CellRef.data

```ceesafe
public fn data(borrow this) -> borrow T
```

The contents, for as long as the guard is borrowed.

## struct CellRefMut

```ceesafe
public struct CellRefMut<region r, T> {
    drop { … }
}
```

A `write` guard of a RefCell (§41.3): `data()` lends the contents exclusively; dropping it gives the access back.

### CellRefMut.data

```ceesafe
public fn data(borrow mut this) -> borrow mut T
```

The contents, for as long as the guard is borrowed exclusively.
Safety: the guard set the state to -1, so no other guard is live and none can be made while it is; the loan
ends before the guard's drop gives the access back (a RawCell's contents may change while the cell is
shared, type-system.md §41.1).

## struct OnceCell

```ceesafe
public struct OnceCell<T>
```

A value written at most once through a shared reference (§41.4, ADR 0096): empty at first, then full for as long
as the cell is shared, lending `borrow T`. `getOrInit` fills an empty cell from its initialiser; `set` fills it
with a value. A full cell changes only through an exclusive borrow (`take`, `getMut`), when no reference to the
contents can be live. The state is one byte: 0 empty, 1 the initialiser is running, 2 full.

### OnceCell.create

```ceesafe
public fn create() -> OnceCell<T>
```

An empty cell.

### OnceCell.get

```ceesafe
public fn get(borrow this) -> borrow T?
```

The value, or null while the cell is empty.

### OnceCell.getOrInit

```ceesafe
public fn getOrInit(borrow this, init: borrow mut fn() -> T) -> borrow(this) T
```

The value; an empty cell runs `init` and stores its result first. Panics if `init` asks this cell for its
value ("cell initialised recursively").
Safety: the state is 0, so no reference to the contents has been lent and `init` cannot get one (a nested
call panics); the old contents are null and need no drop.

### OnceCell.tryInit

```ceesafe
public fn tryInit<E>(borrow this, init: borrow mut fn() -> Result<T, E>) -> Result<void, E>
```

Fills an empty cell from an initialiser that can fail: on `Err` the cell stays empty and the error is
returned. A full cell is left as it is, and `init` does not run. `get` or `getOrInit` then lends the value.
Panics as getOrInit.
Safety: as getOrInit.

### OnceCell.set

```ceesafe
public fn set(borrow this, value: T) -> Result<void, T>
```

Stores `value` in an empty cell. A cell that is full, or whose initialiser is running, gives the value back.
Safety: as getOrInit.

### OnceCell.getMut

```ceesafe
public fn getMut(borrow mut this) -> (borrow mut T)?
```

The value, for as long as the cell is borrowed exclusively; null while the cell is empty.

### OnceCell.take

```ceesafe
public fn take(borrow mut this) -> T?
```

The value, leaving the cell empty: no reference to it can be live while the cell is borrowed exclusively.
Safety: the contents are read out and null written in their place, so they have one owner.

### OnceCell.intoInner

```ceesafe
public fn intoInner(this) -> T?
```

The value, the cell consumed.

## struct CStr

```ceesafe
public struct CStr<region r>
```

A view of UTF-8 text with a NUL byte after its end, valid for region `r` as a `StringView<r>` is: what a C
function taking a `const char *` needs. Copy; a field of this type needs a region parameter, as a view's does.

### CStr.fromBytesWithNul

```ceesafe
public fn fromBytesWithNul(b: Slice<r, u8>) -> CStr<r>?
```

Bytes ending in a NUL that is their only one, as valid UTF-8 text before it; else null.

### CStr.fromPtr

```ceesafe
public unsafe fn fromPtr(p: *CChar) -> CStr<r>?
```

The C string at `p` (ADR 0122): the bytes up to its first NUL, counted here, when they are UTF-8 text; else
null. A null `p` gives null too.
Safety: `p` is null, or points to bytes that end in a NUL and that are neither changed nor freed while the
result, or a view taken from it, is used.

### CStr.ptr

```ceesafe
public fn ptr(borrow this) -> *CChar
```

The address of the first byte, for C: the text is followed by its NUL.

### CStr.view

```ceesafe
public fn view(borrow this) -> StringView<r>
```

The text, without the NUL.

### CStr.length

```ceesafe
public fn length(borrow this) -> usize
```

The text's length in bytes, without the NUL.

### CStr.bytesWithNul

```ceesafe
public fn bytesWithNul(borrow this) -> Slice<r, u8>
```

The text and its NUL: `length + 1` bytes.

## interface DerivedEqual

```ceesafe
public interface DerivedEqual<T> {
    fn derivedEqual(borrow this, other: borrow T) -> bool;
}
```

The equality `@derive(Equal)` gives a struct or enum: `a == b` on the type calls `a.derivedEqual(b)`, and `a != b`
negates it. Only the compiler implements it: an `impl` written by hand is N0290, so `==` keeps one meaning
(language-spec.md §6).

## struct DerivedEqualElements

```ceesafe
public struct DerivedEqualElements<T: DerivedEqual<T>>
```

`==` on slices, arrays and vectors of a type `@derive(Equal)` gave equality (ADR 0087 §2.3): the same length, and
each element equal to the other's, in order. The compiler calls it for `a == b`; a program has no need to.

### DerivedEqualElements.equal

```ceesafe
public fn equal(a: Slice<T>, b: Slice<T>) -> bool
```

Whether `a` and `b` have the same length and equal elements.

## struct TextWriter

```ceesafe
public struct TextWriter<region r>
```

A writer of text into a caller's byte buffer, as C's `snprintf` writes: what does not fit is cut off, the text is
always zero-terminated in a buffer of at least one byte (so `cText` can be handed to C), and `wanted` counts every
byte asked for, as `snprintf`'s result does, so a caller can tell that the text was cut.

### TextWriter.create

```ceesafe
public fn create(buffer: SliceMut<r, u8>) -> TextWriter<r>
```

A writer at the start of `buffer`, which it empties (a zero first byte, if it has one).

### TextWriter.byte

```ceesafe
public fn byte(borrow mut this, b: u8)
```

Appends one byte.

### TextWriter.bytes

```ceesafe
public fn bytes(borrow mut this, data: Slice<u8>)
```

Appends bytes.

### TextWriter.text

```ceesafe
public fn text(borrow mut this, s: StringView)
```

Appends text.

### TextWriter.decimal

```ceesafe
public fn decimal(borrow mut this, value: u64)
```

Appends an unsigned integer in decimal.

### TextWriter.signed

```ceesafe
public fn signed(borrow mut this, value: i64)
```

Appends a signed integer in decimal, with a `-` if it is negative.

### TextWriter.hex

```ceesafe
public fn hex(borrow mut this, value: u64, width: u32)
```

Appends an unsigned integer in lowercase hexadecimal, with at least `width` digits (zeros before it), as
`%0*llx`; no `0x`.

### TextWriter.written

```ceesafe
public fn written(borrow this) -> Slice<u8>
```

The bytes written, without the terminating zero.

### TextWriter.length

```ceesafe
public fn length(borrow this) -> usize
```

How many bytes were written (at most the buffer's length less one).

### TextWriter.wanted

```ceesafe
public fn wanted(borrow this) -> usize
```

How many bytes were asked for: more than `length()` when the text was cut, as `snprintf`'s result.

### TextWriter.truncated

```ceesafe
public fn truncated(borrow this) -> bool
```

Whether some text did not fit.

### TextWriter.cText

```ceesafe
public fn cText(borrow this) -> *CChar
```

The text as a zero-terminated C string, valid while the writer's buffer is. An empty buffer, which can hold no
terminator, gives an empty string that is not in it.

## enum TextAlign

```ceesafe
public enum TextAlign {
    Left,
    Right,
    Center,
}
```

Where `appendPadded` puts text in a wider field (ADR 0087 §2.9): Rust's `{:<w}`, `{:>w}` and `{:^w}`. A centred
text's extra fill character goes after it.

## struct Formatted

```ceesafe
public struct Formatted
```

The formatting helpers of `String` and `VectorBytes` (ADR 0087 §2.9): what Rust's `{:x}`, `{:08X}`, `{:o}`,
`{:.3}` and `{:>8}` write. Each appends to a byte vector; static functions, the struct is never made.

### Formatted.hex

```ceesafe
public fn hex(out: borrow mut Vector<u8>, value: u64, minWidth: u32, upper: bool) -> Result<void, AllocError>
```

`value` in hexadecimal, at least `minWidth` digits (zeros before it), uppercase when `upper`; no `0x`.

### Formatted.octal

```ceesafe
public fn octal(out: borrow mut Vector<u8>, value: u64) -> Result<void, AllocError>
```

`value` in octal; no `0o`.

### Formatted.fixed

```ceesafe
public fn fixed(out: borrow mut Vector<u8>, value: f64, digits: u32) -> Result<void, AllocError>
```

`value` in decimal with exactly `digits` digits after the point (none, and no point, for 0), rounded half to
even on its exact binary value, as Rust's `{:.N}` and C's `%.Nf` round; `NaN`, `inf` and `-inf` as Rust writes
them. A negative zero keeps its sign.

### Formatted.padded

```ceesafe
public fn padded(
    out: borrow mut Vector<u8>,
    text: StringView,
    width: usize,
    align: TextAlign,
    fill: u32,
) -> Result<void, AllocError>
```

`text` in a field of `width` characters (Unicode scalar values, as Rust counts them), filled with the
character `fill` on the side `align` leaves; text at least as wide is appended as it is.

## interface AppendFormatted

```ceesafe
public interface AppendFormatted {
    fn appendHex(borrow mut this, value: u64, minWidth: u32, upper: bool) -> Result<void, AllocError>;
    fn appendOctal(borrow mut this, value: u64) -> Result<void, AllocError>;
    fn appendFixed(borrow mut this, value: f64, digits: u32) -> Result<void, AllocError>;
    fn appendPadded(borrow mut this, text: StringView, width: usize, align: TextAlign, fill: u32) -> Result<void, AllocError>;
}
```

`s.appendHex(…)` and the other helpers (ADR 0087 §2.9), through `Formatted`: implemented here for a `Vector<u8>`,
and in lib/std for a `String`. Each appends ASCII, or for `appendPadded` the text and its fill, so a string stays
UTF-8.

## interface Iterator

```ceesafe
public interface Iterator<T> {
    fn next(borrow mut this) -> T?;
}
```

A source of values, one at a time: `next()` gives each, then null. `for (x in e)` calls it on a struct that has such
a method (the shape is enough, ADR 0074 §2.8), on a type parameter bounded by `Iterator<T>`, and on a
`borrow mut dyn Iterator<T>` or a `Box<dyn Iterator<T>>` (type-system.md §16.4). The item is a parameter, so callers
fix it directly (`I: Iterator<u8>`) and `dyn Iterator<T>` has a table.

## struct Volatile

```ceesafe
@noncopy
public struct Volatile<T: Copy>
```

A cell read and written only by volatile accesses: a hardware register, or memory shared with a device.
Not Copy: a copy would read the register with an ordinary access, once, instead of each time it is used.

### Volatile.read

```ceesafe
public fn read(borrow this) -> T
```

Safety: `this.value` is a live, aligned `T` (it is a field), and the read is a single volatile access.

### Volatile.write

```ceesafe
public fn write(borrow mut this, v: T)
```

Safety: as `read`; `borrow mut this` makes the write exclusive.

## struct MmioPtr

```ceesafe
@noncopy
public struct MmioPtr<T>
```

The address of a memory-mapped `T`, typically a `@repr(C)` struct of `Volatile` registers. Not Copy: two
copies would be two owners of the same registers.

### MmioPtr.at

```ceesafe
public unsafe fn at(address: usize) -> MmioPtr<T>
```

Safety: `address` is mapped, aligned for `T`, and no other `MmioPtr` or reference aliases the registers.

### MmioPtr.get

```ceesafe
public fn get(borrow this) -> borrow T
```

Safety: `at`'s contract makes `base` a live `T` reached only through this value; the result borrows `this`.

### MmioPtr.getMut

```ceesafe
public fn getMut(borrow mut this) -> borrow mut T
```

Safety: as `get`; `borrow mut this` makes the access exclusive.

### MmioPtr.address

```ceesafe
public fn address(borrow this) -> usize
```

The address of the registers, as given to `at`.

## struct NumberParsing

```ceesafe
public struct NumberParsing
```

Parsing integers and floats from bytes: static functions; the struct has no fields and is never made.

### NumberParsing.parseU8

```ceesafe
public fn parseU8(s: Slice<u8>) -> u8?
```

Decimal text as a `u8`.

### NumberParsing.parseU8Radix

```ceesafe
public fn parseU8Radix(s: Slice<u8>, radix: u32) -> u8?
```

Text in `radix` (2 to 36; null otherwise) as a `u8`.

### NumberParsing.parseU16

```ceesafe
public fn parseU16(s: Slice<u8>) -> u16?
```

Decimal text as a `u16`.

### NumberParsing.parseU16Radix

```ceesafe
public fn parseU16Radix(s: Slice<u8>, radix: u32) -> u16?
```

Text in `radix` (2 to 36; null otherwise) as a `u16`.

### NumberParsing.parseU32

```ceesafe
public fn parseU32(s: Slice<u8>) -> u32?
```

Decimal text as a `u32`.

### NumberParsing.parseU32Radix

```ceesafe
public fn parseU32Radix(s: Slice<u8>, radix: u32) -> u32?
```

Text in `radix` (2 to 36; null otherwise) as a `u32`.

### NumberParsing.parseU64

```ceesafe
public fn parseU64(s: Slice<u8>) -> u64?
```

Decimal text as a `u64`.

### NumberParsing.parseU64Radix

```ceesafe
public fn parseU64Radix(s: Slice<u8>, radix: u32) -> u64?
```

Text in `radix` (2 to 36; null otherwise) as a `u64`.

### NumberParsing.parseUsize

```ceesafe
public fn parseUsize(s: Slice<u8>) -> usize?
```

Decimal text as a `usize`.

### NumberParsing.parseUsizeRadix

```ceesafe
public fn parseUsizeRadix(s: Slice<u8>, radix: u32) -> usize?
```

Text in `radix` (2 to 36; null otherwise) as a `usize`.

### NumberParsing.parseI8

```ceesafe
public fn parseI8(s: Slice<u8>) -> i8?
```

Decimal text as an `i8`.

### NumberParsing.parseI8Radix

```ceesafe
public fn parseI8Radix(s: Slice<u8>, radix: u32) -> i8?
```

Text in `radix` (2 to 36; null otherwise) as an `i8`.

### NumberParsing.parseI16

```ceesafe
public fn parseI16(s: Slice<u8>) -> i16?
```

Decimal text as an `i16`.

### NumberParsing.parseI16Radix

```ceesafe
public fn parseI16Radix(s: Slice<u8>, radix: u32) -> i16?
```

Text in `radix` (2 to 36; null otherwise) as an `i16`.

### NumberParsing.parseI32

```ceesafe
public fn parseI32(s: Slice<u8>) -> i32?
```

Decimal text as an `i32`.

### NumberParsing.parseI32Radix

```ceesafe
public fn parseI32Radix(s: Slice<u8>, radix: u32) -> i32?
```

Text in `radix` (2 to 36; null otherwise) as an `i32`.

### NumberParsing.parseI64

```ceesafe
public fn parseI64(s: Slice<u8>) -> i64?
```

Decimal text as an `i64`.

### NumberParsing.parseI64Radix

```ceesafe
public fn parseI64Radix(s: Slice<u8>, radix: u32) -> i64?
```

Text in `radix` (2 to 36; null otherwise) as an `i64`.

### NumberParsing.parseIsize

```ceesafe
public fn parseIsize(s: Slice<u8>) -> isize?
```

Decimal text as an `isize`.

### NumberParsing.parseIsizeRadix

```ceesafe
public fn parseIsizeRadix(s: Slice<u8>, radix: u32) -> isize?
```

Text in `radix` (2 to 36; null otherwise) as an `isize`.

### NumberParsing.parseF64

```ceesafe
public fn parseF64(s: Slice<u8>) -> f64?
```

Text as an `f64`, correctly rounded, or null. `[+-]digits[.digits][(e|E)[+-]digits]` with a digit on at least one
side of the point, or `inf`, `infinity` or `nan` in any case, with an optional sign. Too large is ±infinity.

### NumberParsing.parseF32

```ceesafe
public fn parseF32(s: Slice<u8>) -> f32?
```

Text as an `f32`, correctly rounded (once: not through an `f64`), or null; the grammar of parseF64.

## struct ResultMethods

```ceesafe
public struct ResultMethods<T, E>
```

`Result<T, E>`'s methods.

### ResultMethods.isOk

```ceesafe
public fn isOk(r: borrow Result<T, E>) -> bool
```

Whether `r` is `Ok`.

### ResultMethods.isErr

```ceesafe
public fn isErr(r: borrow Result<T, E>) -> bool
```

Whether `r` is `Err`.

### ResultMethods.ok

```ceesafe
public fn ok(r: Result<T, E>) -> T?
```

The `Ok` value, or null; an `Err` value is destroyed.

### ResultMethods.err

```ceesafe
public fn err(r: Result<T, E>) -> E?
```

The `Err` value, or null; an `Ok` value is destroyed.

## struct OptionalResults

```ceesafe
public struct OptionalResults<T, E>
```

`o.okOr(e)`: an optional as a `Result`.

### OptionalResults.okOr

```ceesafe
public fn okOr(o: T?, e: E) -> Result<T, E>
```

`Ok` with the value, or `Err(e)` for null; `e` is destroyed when there is a value.

## struct SliceHalves

```ceesafe
public struct SliceHalves<region r, T> {
    public left: Slice<r, T>;
    public right: Slice<r, T>;
}
```

The two parts of a slice that `splitAt` divides (type-system.md §16.5): the elements before the split, and from it on.

## struct SliceHalvesMut

```ceesafe
public struct SliceHalvesMut<region r, T> {
    public left: SliceMut<r, T>;
    public right: SliceMut<r, T>;
}
```

The two parts of a mutable slice that `splitAtMut` divides: disjoint, so both may be written at once.

## struct SliceSplitting

```ceesafe
public struct SliceSplitting<region r, T>
```

Splitting a slice in two (type-system.md §16.5): `s.splitAt(mid)` is `SliceSplitting<T>.splitAt(s, mid)`. The
struct's region ties the halves to the slice they come from; it has no fields and is never made.

### SliceSplitting.splitAt

```ceesafe
public fn splitAt(s: Slice<r, T>, mid: usize) -> SliceHalves<r, T>
```

The elements before `mid`, and from `mid` on; panics if `mid` is past the end.

### SliceSplitting.splitAtMut

```ceesafe
public fn splitAtMut(s: SliceMut<r, T>, mid: usize) -> SliceHalvesMut<r, T>
```

As splitAt, for a mutable slice: two mutable slices that do not overlap.
Safety: slicing `s[mid..]` checks `mid` against the length (it panics past the end), so `[0, mid)` and
`[mid, length)` are inside `s` and disjoint, and each is lent for as long as `s` is.

## struct SliceAlgorithms

```ceesafe
public struct SliceAlgorithms<T>
```

The algorithms over a slice of `T`, as static functions: the struct has no fields and is never made.

### SliceAlgorithms.sortBy

```ceesafe
@effectsOf(cmp)
public fn sortBy(s: SliceMut<T>, cmp: borrow mut fn(borrow T, borrow T) -> i32)
```

Orders `s` so that `cmp` of each element and the next is at most zero, for a consistent `cmp`. Not stable.
An introsort (ADR 0057): quicksort with a median-of-three pivot and a partition without a branch on `cmp`'s
result, the elements equal to an earlier pivot set aside at once, insertion sort for short runs, and heapsort
past a depth of 2·log₂ n, so O(n log n) calls of `cmp` in the worst case. A slice that is one ascending run is
left as it is, and one strictly descending run is reversed. Allocates nothing; the shorter side is sorted
first, so at most 64 ranges wait. An inconsistent `cmp` leaves some permutation of `s`.

### SliceAlgorithms.forEach

```ceesafe
@effectsOf(visit)
public fn forEach(s: Slice<T>, visit: borrow mut fn(borrow T))
```

Calls `visit` on each element, in index order.

### SliceAlgorithms.any

```ceesafe
@effectsOf(test)
public fn any(s: Slice<T>, test: borrow mut fn(borrow T) -> bool) -> bool
```

Does `test` return true for some element? Stops at the first that does.

### SliceAlgorithms.all

```ceesafe
@effectsOf(test)
public fn all(s: Slice<T>, test: borrow mut fn(borrow T) -> bool) -> bool
```

Does `test` return true for every element? Stops at the first that does not.

### SliceAlgorithms.partitionPoint

```ceesafe
@effectsOf(test)
public fn partitionPoint(s: Slice<T>, test: borrow mut fn(borrow T) -> bool) -> usize
```

The index of the first element for which `test` is false, in a slice partitioned by `test`: every element
for which it is true comes before every element for which it is false. The length, if it is true for all;
0 for an empty slice. A binary search: `test` is called at most ⌈log₂ n⌉ + 1 times, on elements chosen by
the length and the earlier answers. On a slice that is not partitioned the result is some index of it or its
length, with no panic. The number of steps depends on the length alone, not on the answers (ADR 0103): the
loop's own branch does not wait for `test`, which makes it faster than a loop that ends when two bounds meet.

### SliceAlgorithms.retain

```ceesafe
@effectsOf(keep)
public fn retain(s: SliceMut<T>, keep: borrow mut fn(borrow T) -> bool) -> usize
```

Moves the elements for which `keep` returns true to the front, in order, calling it once on each element in
index order; returns how many there are. `v.retain(keep)` then truncates the vector to that length.

## struct VectorEditing

```ceesafe
public struct VectorEditing<T>
```

`v.insert(at, x)`, `v.remove(at)` and `v.swapRemove(at)` (ADR 0075 §2.12). The index is checked at the call, so a
panic names the program's line: `at <= length` for `insert`, `at < length` for the others. The elements move by
`swap`, so they may be of any type. A vector's allocator is `SystemHeap` or `FailingAllocator` (type-system.md
§23.1); the `…Failing` functions are the latter's.

### VectorEditing.insert

```ceesafe
public fn insert(v: borrow mut Vector<T>, at: usize, x: T) -> Result<void, AllocError>
```

Puts `x` at `at`, moving the elements from `at` on one place up. O(length - at). If growing fails, `v` is
unchanged and `x` is destroyed.

### VectorEditing.remove

```ceesafe
public fn remove(v: borrow mut Vector<T>, at: usize) -> T
```

Takes the element at `at` out, moving the later elements one place down. O(length - at).

### VectorEditing.swapRemove

```ceesafe
public fn swapRemove(v: borrow mut Vector<T>, at: usize) -> T
```

Takes the element at `at` out and puts the last element in its place. O(1); the order is not kept.

### VectorEditing.insertFailing

```ceesafe
public fn insertFailing(v: borrow mut Vector<T, FailingAllocator>, at: usize, x: T) -> Result<void, AllocError>
```

`insert` for a vector of `FailingAllocator`.

### VectorEditing.removeFailing

```ceesafe
public fn removeFailing(v: borrow mut Vector<T, FailingAllocator>, at: usize) -> T
```

`remove` for a vector of `FailingAllocator`.

### VectorEditing.swapRemoveFailing

```ceesafe
public fn swapRemoveFailing(v: borrow mut Vector<T, FailingAllocator>, at: usize) -> T
```

`swapRemove` for a vector of `FailingAllocator`.

## struct VectorCopying

```ceesafe
public struct VectorCopying<T: Copy>
```

`v.clone()` (ADR 0108, type-system.md §23.1): a new vector holding a copy of the elements, so the element type is
Copy. One allocation, sized as `withCapacity` of the length sizes it, none for an empty slice. The `…Failing`
function makes a vector of `FailingAllocator`.

### VectorCopying.clone

```ceesafe
public fn clone(s: Slice<T>) -> Result<Vector<T>, AllocError>
```

A vector with a copy of the elements of `s`: its length is `s.length`, its capacity at least that.

### VectorCopying.cloneFailing

```ceesafe
public fn cloneFailing(s: Slice<T>) -> Result<Vector<T, FailingAllocator>, AllocError>
```

`clone` for a vector of `FailingAllocator`.

## struct VectorParts

```ceesafe
public struct VectorParts<T> {
    public pointer: *mut T;
    public length: usize;
    public capacity: usize;
}
```

What a vector is made of (ADR 0090 §2.1, type-system.md §23.3): `pointer` is null when `capacity` is 0.

## struct VectorRaw

```ceesafe
public struct VectorRaw<T>
```

`Vector.intoRaw` and `Vector.fromRaw` (ADR 0090, type-system.md §23.3). `releaseBuffer` and `adoptBuffer` are the
compiler's, for the libraries alone: the first makes a vector the empty one without freeing its buffer or dropping
its elements, the second gives an empty vector a buffer; their last argument moves the block out of, or into, the
allocation report's `live`. The `…Failing` functions are for a vector of `FailingAllocator`.

### VectorRaw.intoRaw

```ceesafe
public fn intoRaw(v: Vector<T>) -> VectorParts<T>
```

`Vector.intoRaw(v)`: the caller owns the buffer and its elements.

### VectorRaw.fromRaw

```ceesafe
public unsafe fn fromRaw(pointer: *mut T, length: usize, capacity: usize) -> Vector<T>
```

`Vector.fromRaw(pointer, length, capacity)`. A capacity of 0 gives the empty vector.
Safety: the caller's, as type-system.md §23.3 states it: the parts of one vector of this type, given once.

### VectorRaw.intoRawFailing

```ceesafe
public fn intoRawFailing(v: Vector<T, FailingAllocator>) -> VectorParts<T>
```

`Vector.intoRaw(v)` for a vector of `FailingAllocator`: the caller owns the buffer and its elements.

### VectorRaw.fromRawFailing

```ceesafe
public unsafe fn fromRawFailing(pointer: *mut T, length: usize, capacity: usize) -> Vector<T, FailingAllocator>
```

`Vector.fromRaw(pointer, length, capacity)` for a vector of `FailingAllocator`. A capacity of 0 gives the empty vector.
Safety: the caller's, as type-system.md §23.3 states it: the parts of one vector of this type, given once.

## struct VectorForeign

```ceesafe
public struct VectorForeign<T: Copy>
```

`Vector.intoForeign` and `Vector.fromForeign` (ADR 0090 §2.3, type-system.md §23.3): a buffer of plain values that
changes sides in a host program. The element type is Copy, also when a program names this struct itself.

### VectorForeign.intoForeign

```ceesafe
public unsafe fn intoForeign(v: Vector<T>) -> VectorParts<T>
```

`Vector.intoForeign(v)`: the host owns the buffer; it leaves the report's `live`.
Safety: the caller names the host that frees the block with the allocator both sides share (type-system.md
§23.3).

### VectorForeign.fromForeign

```ceesafe
public unsafe fn fromForeign(pointer: *mut T, length: usize, capacity: usize) -> Vector<T>
```

`Vector.fromForeign(pointer, length, capacity)`. A capacity of 0 gives the empty vector.
Safety: the caller's, as type-system.md §23.3 states it: the parts of one vector of this type, given once.

### VectorForeign.intoForeignFailing

```ceesafe
public unsafe fn intoForeignFailing(v: Vector<T, FailingAllocator>) -> VectorParts<T>
```

`Vector.intoForeign(v)` for a vector of `FailingAllocator`: the host owns the buffer; it leaves the report's `live`.
Safety: the caller names the host that frees the block with the allocator both sides share (type-system.md
§23.3).

### VectorForeign.fromForeignFailing

```ceesafe
public unsafe fn fromForeignFailing(
    pointer: *mut T,
    length: usize,
    capacity: usize,
) -> Vector<T, FailingAllocator>
```

`Vector.fromForeign(pointer, length, capacity)` for a vector of `FailingAllocator`. A capacity of 0 gives the empty vector.
Safety: the caller's, as type-system.md §23.3 states it: the parts of one vector of this type, given once.

## struct TextHalves

```ceesafe
public struct TextHalves<region r> {
    public left: StringView<r>;
    public right: StringView<r>;
}
```

The parts of a text before and after a separator: what `s.splitOnce(sep)` finds.

## struct ByteHalves

```ceesafe
public struct ByteHalves<region r> {
    public left: Slice<r, u8>;
    public right: Slice<r, u8>;
}
```

The parts of bytes before and after a separator: what `s.splitOnce(sep)` finds on a `Slice<u8>`.

## struct TextSplit

```ceesafe
public struct TextSplit<region r>
```

The pieces of a text between separators, in order: `s.split(sep)`. `next()` gives each piece, then null. `"a,,b"`
split by `","` is `"a"`, `""`, `"b"`; an empty text is one empty piece; an empty separator gives the text whole.

Implements `Iterator<StringView<r>>`.

## struct ByteSplit

```ceesafe
public struct ByteSplit<region r>
```

The pieces of bytes between separators, in order: `s.split(sep)` on a `Slice<u8>`, as TextSplit.

Implements `Iterator<Slice<r, u8>>`.

## struct TextSearch

```ceesafe
public struct TextSearch<region r>
```

The methods of a `StringView` that search it (ADR 0071 §2.4), as static functions: the struct is never made.

### TextSearch.indexOf

```ceesafe
public fn indexOf(s: StringView<r>, needle: StringView) -> usize?
```

### TextSearch.lastIndexOf

```ceesafe
public fn lastIndexOf(s: StringView<r>, needle: StringView) -> usize?
```

### TextSearch.indexOfByte

```ceesafe
public fn indexOfByte(s: StringView<r>, b: u8) -> usize?
```

### TextSearch.contains

```ceesafe
public fn contains(s: StringView<r>, needle: StringView) -> bool
```

### TextSearch.startsWith

```ceesafe
public fn startsWith(s: StringView<r>, prefix: StringView) -> bool
```

### TextSearch.endsWith

```ceesafe
public fn endsWith(s: StringView<r>, suffix: StringView) -> bool
```

### TextSearch.equalsIgnoreAsciiCase

```ceesafe
public fn equalsIgnoreAsciiCase(s: StringView<r>, other: StringView) -> bool
```

### TextSearch.trim

```ceesafe
public fn trim(s: StringView<r>) -> StringView<r>
```

### TextSearch.trimStart

```ceesafe
public fn trimStart(s: StringView<r>) -> StringView<r>
```

### TextSearch.trimEnd

```ceesafe
public fn trimEnd(s: StringView<r>) -> StringView<r>
```

### TextSearch.splitOnce

```ceesafe
public fn splitOnce(s: StringView<r>, separator: StringView) -> TextHalves<r>?
```

The text before the first `separator` and after it, or null if it does not occur. An empty separator is found
at the start.

### TextSearch.split

```ceesafe
public fn split(s: StringView<r>, separator: StringView<r>) -> TextSplit<r>
```

## struct ByteSearch

```ceesafe
public struct ByteSearch<region r>
```

The methods of a `Slice<u8>` that search it (ADR 0071 §2.4), as TextSearch's.

### ByteSearch.indexOf

```ceesafe
public fn indexOf(s: Slice<r, u8>, needle: Slice<u8>) -> usize?
```

### ByteSearch.lastIndexOf

```ceesafe
public fn lastIndexOf(s: Slice<r, u8>, needle: Slice<u8>) -> usize?
```

### ByteSearch.indexOfByte

```ceesafe
public fn indexOfByte(s: Slice<r, u8>, b: u8) -> usize?
```

### ByteSearch.contains

```ceesafe
public fn contains(s: Slice<r, u8>, needle: Slice<u8>) -> bool
```

### ByteSearch.startsWith

```ceesafe
public fn startsWith(s: Slice<r, u8>, prefix: Slice<u8>) -> bool
```

### ByteSearch.endsWith

```ceesafe
public fn endsWith(s: Slice<r, u8>, suffix: Slice<u8>) -> bool
```

### ByteSearch.equalsIgnoreAsciiCase

```ceesafe
public fn equalsIgnoreAsciiCase(s: Slice<r, u8>, other: Slice<u8>) -> bool
```

### ByteSearch.trim

```ceesafe
public fn trim(s: Slice<r, u8>) -> Slice<r, u8>
```

### ByteSearch.trimStart

```ceesafe
public fn trimStart(s: Slice<r, u8>) -> Slice<r, u8>
```

### ByteSearch.trimEnd

```ceesafe
public fn trimEnd(s: Slice<r, u8>) -> Slice<r, u8>
```

### ByteSearch.splitOnce

```ceesafe
public fn splitOnce(s: Slice<r, u8>, separator: Slice<u8>) -> ByteHalves<r>?
```

### ByteSearch.split

```ceesafe
public fn split(s: Slice<r, u8>, separator: Slice<r, u8>) -> ByteSplit<r>
```

## struct StringCopying

```ceesafe
public struct StringCopying
```

`String.from(text)` and `s.clone()` (ADR 0108, type-system.md §23.2): a new string holding a copy of a text. One
allocation, sized as `withCapacity` of the length in bytes sizes it, none for an empty text. The `…Failing`
function makes a string of `FailingAllocator`.

### StringCopying.from

```ceesafe
public fn from(text: StringView) -> Result<String, AllocError>
```

A string with a copy of `text`: its length is `text.length`, its capacity at least that.

### StringCopying.fromFailing

```ceesafe
public fn fromFailing(text: StringView) -> Result<String<FailingAllocator>, AllocError>
```

`from` for a string of `FailingAllocator`.
