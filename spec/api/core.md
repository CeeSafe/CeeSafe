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
- [struct AsyncSignal](#struct-asyncsignal)
- [enum LoadOrdering](#enum-loadordering)
- [enum StoreOrdering](#enum-storeordering)
- [enum Ordering](#enum-ordering)
- [struct AtomicU64](#struct-atomicu64)
- [struct AtomicU32](#struct-atomicu32)
- [struct AtomicUSize](#struct-atomicusize)
- [struct AtomicBool](#struct-atomicbool)
- [struct TextWriter](#struct-textwriter)
- [struct Volatile](#struct-volatile)
- [struct MmioPtr](#struct-mmioptr)
- [struct SliceHalves](#struct-slicehalves)
- [struct SliceHalvesMut](#struct-slicehalvesmut)
- [struct SliceSplitting](#struct-slicesplitting)
- [struct SliceAlgorithms](#struct-slicealgorithms)

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
    Io(IoError),
}
```

Why an async operation failed (type-system.md §40.4, §40.7).

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
An introsort: quicksort with a median-of-three pivot, insertion sort for short runs, and heapsort past a
depth of 2·log₂ n, so O(n log n) calls of `cmp` in the worst case. Allocates nothing; the shorter side is
sorted first, so at most 64 ranges wait. An inconsistent `cmp` leaves some permutation of `s`.

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

### SliceAlgorithms.retain

```ceesafe
@effectsOf(keep)
public fn retain(s: SliceMut<T>, keep: borrow mut fn(borrow T) -> bool) -> usize
```

Moves the elements for which `keep` returns true to the front, in order, calling it once on each element in
index order; returns how many there are. `v.retain(keep)` then truncates the vector to that length.
