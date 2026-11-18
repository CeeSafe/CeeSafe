# ADR 0172 — Named shared memory

- Status: **Proposed**, 2026-11-18. Written at the owner's request ("Write named-shared-memory ADR"): ADR 0169 §5
  question 2 left "a later ADR for named shared memory and for passing a descriptor", and httpserver's supervisor
  cannot leave `fork` for `Process.spawn` without the first (`~/dev/CeeSafeApps/httpserver/CEESAFE_FINDINGS.md`,
  "What ceesafec 0.65.1 to 0.70.0 changed"). Not accepted: §6 has the questions. Not implemented.
- Scope: lib/std (`SharedMemory`), the hosted runtime (`runtime/hosted/ceesafe_rt_io.c`: POSIX, Windows, CosmoOS),
  the interpreter's host services; type-system.md §26 (a new §26.17), `spec/api/std.md`, the guide's processes page.
  No change to the language, to the backends or to the allocator. **Passing a descriptor is not in it** (§4).

## 1. Context

- **What programs do now.** httpserver's workers count rate limits together and publish events to each other
  through memory they all map: `SharedWords` (a table of `AtomicU64`) and `Bus` (a ring of bytes for each worker,
  with two `AtomicU64` beside it). Both ask `mmap` for an anonymous shared mapping through `extern "C"` before the
  supervisor forks, and every worker has it because it is a copy of the supervisor.
- **What that rests on.**
  - `fork`. A worker started by `Process.spawn` (ADR 0169) is a new program: it has no mapping of its parent's, and
    an anonymous mapping has no name by which to ask for it.
  - `extern "C"` declarations of `mmap` and its constants, which differ between systems (`MAP_ANON` is 0x1000 on
    macOS and 0x20 on Linux) and do not exist on Windows.
  - A cast from an address to `borrow AtomicU64` in the program's own `unsafe`, with the bounds check the program
    remembered to write.
- **What lib/std has.** Nothing: no mapping of a file, of memory, or of anything by name. The runtime calls `mmap`
  for io_uring's rings only.
- **What is asked for.** Memory that two processes that share nothing else can both reach, found by a name one
  tells the other (an argument of `Process.spawn`, an environment variable, a file), on Linux, macOS and Windows.

## 2. Options

| | what a child is given | reaches | cost |
|---|---|---|---|
| **A. A name** (this ADR) | a string, in its arguments | any process of the same user that knows it | the name outlives a creator that is killed (POSIX), and has to be removed |
| B. An inherited handle | a descriptor or handle number, through a list in `ChildOptions` | children only | `Process.spawn` has to pass handles: `posix_spawn` file actions, Windows' handle list. It is the mechanism a listening socket needs too (§4) |
| C. A file that is mapped | a path | any process that can open it | a file system that may write the pages out; a path to choose and clean up |

- **B is the better fit for a supervisor alone**: nothing to name, nothing left behind, and one mechanism for
  memory and sockets. It does nothing for two programs that are not parent and child (a server and its control
  tool, a test and the program it watches).
- **A and B are not exclusive.** A `SharedMemory` could later be made inheritable by B's list; what this ADR fixes
  is the value and its safe surface, which B would use unchanged.
- C is `MappedFile`, a different thing with its own questions (length changes, flushing, read-only files), and is
  not proposed here.

## 3. Proposed decision

### 3.1 The API (lib/std)

```ceesafe
public struct SharedMemory {
    /// Makes memory of at least `bytes` bytes, zeroed, under `name`. `AlreadyExists` if the name is taken.
    public fn create(name: StringView, bytes: usize) -> Result<SharedMemory, IoError>;
    /// Maps memory another process (or this one) made under `name`. `NotFound` if there is none.
    public fn open(name: StringView) -> Result<SharedMemory, IoError>;
    /// Removes a name, for one a process that was killed left behind. `NotFound` if there is none.
    public fn remove(name: StringView) -> Result<void, IoError>;

    /// Its size in bytes: what `create` was asked for, rounded up to the system's pages.
    public fn length(borrow this) -> usize;
    /// How many 64-bit words it holds: `length() / 8`.
    public fn words(borrow this) -> usize;
    /// Word `index`, as an atomic. Panics if `index >= words()`, as an index out of range does.
    public fn word(borrow this, index: usize) -> borrow AtomicU64;
    /// The mapping's address, for code that lays out more than words: usable only in `unsafe`.
    public fn address(borrow this) -> *mut u8;
}
```

- **The value** is a move type that needs drop, and is Send and Share: what it gives safely is atomics. Dropping it
  unmaps the memory. Dropping the one `create` returned also removes the name; the memory itself lives until the
  last process that maps it lets go (both systems do this).
- **`word` is the safe surface.** An `AtomicU64` is what two processes can both read and write without a rule
  between them, and httpserver's shared counters are exactly a table of them. Every operation of `AtomicU64` works
  on it, `compareExchange` included.
- **Bytes are not given safely.** A `Slice<u8>` or `SliceMut<u8>` of memory another process writes at any moment
  breaks what those types promise (§25: no one writes what a shared borrow reads). A program with a protocol of its
  own (httpserver's `Bus`: a writer's bytes become visible only when it advances a word) uses `address()` in
  `unsafe` and says why it is sound, as it does today, without the `mmap` declarations.
- **Nothing in it is a pointer.** The memory is mapped at a different address in each process, so what is stored
  is numbers and offsets. A `Box`, a `Vector` or a `StringView` stored there is meaningless in the other process;
  the safe surface cannot store one.
- **The size is fixed** when it is made. `open` maps all of it.

### 3.2 Names

- 1 to 30 bytes of `A`–`Z`, `a`–`z`, `0`–`9`, `.`, `_`, `-`; anything else is `InvalidInput`. (macOS allows 31
  bytes with the `/` the runtime adds; the rule is the strictest system's, so a name that works on one works on
  all.)
- A name is the user's on POSIX (the object is made with mode 0600) and the session's on Windows (`Local\`).
  Another user's process cannot open it.
- **A program picks a name that is its own**: `"myserver-" + Process.id()`. Two instances of a program then do not
  meet, and a name left by an instance that was killed is not the next one's.

### 3.3 What is left behind

- A creator that ends normally, by returning, by `Process.exit` or by a signal it subscribed to, drops its value
  and the name goes.
- **A creator that is killed or traps leaves the name on POSIX** (`shm_unlink` never ran); the memory stays until
  the name is removed or the machine restarts. Windows has no such case: the object goes with its last handle.
  `SharedMemory.remove(name)` is for a supervisor that knows which name its predecessor used (it wrote it in a
  file, or it uses a fixed one). Nothing is removed automatically: the runtime cannot know that a name it did not
  make is no longer wanted.

### 3.4 Systems

| | `create` | `open` | drop | `remove` |
|---|---|---|---|---|
| Linux, macOS, FreeBSD | `shm_open(O_CREAT \| O_EXCL \| O_RDWR, 0600)`, `ftruncate`, `mmap(MAP_SHARED)`, close | `shm_open(O_RDWR)`, `fstat` for the size, `mmap`, close | `munmap`; the creator `shm_unlink` | `shm_unlink` |
| Windows | `CreateFileMappingW(INVALID_HANDLE_VALUE, …, "Local\name")`, `AlreadyExists` from `ERROR_ALREADY_EXISTS`; `MapViewOfFile` | `OpenFileMappingW`, `MapViewOfFile`, `VirtualQuery` for the size | `UnmapViewOfFile`, `CloseHandle` | `NotFound` (there is nothing to remove) |
| Android | `Unsupported` (no `shm_open`; `ASharedMemory` has no names, and is option B's) | | | |
| CosmoOS, freestanding | `Unsupported` / not there (`profiles.md` §4) | | | |
| the interpreter | a table of the program's own: `create` and `open` in one interpreted program reach the same words; no other process does. Sandboxed (`--sandbox`): `Unsupported` | | | |

- An allocation budget (ADR 0170) does not count it: it is not the allocator's block.

## 4. What it does not do

- **It does not let httpserver's supervisor leave `fork` on macOS.** A spawned worker also needs the listening
  socket. On Linux each worker can bind the port itself (`reusePort` spreads connections); on macOS it cannot, and
  the socket must be handed over: option B's list in `ChildOptions`, or a descriptor sent over a Unix socket. That
  is the other half of ADR 0169's question 2 and is not here (question 2 below).
- No locks, condition variables or queues in shared memory. A mutex that a process can die holding is a design of
  its own (robust mutexes on POSIX, nothing alike on Windows); words and compare-and-swap are what is offered.
- No resizing, no read-only mapping, no mapping of a file (option C).
- No protection against a process of the same user that knows the name writing nonsense: it is shared memory.

## 5. Consequences

- `SharedWords` in httpserver becomes a `SharedMemory` and loses its `extern "C"` block and its cast; `Bus` keeps
  its own `unsafe` over `address()` and loses the block. Both then work for spawned workers, and on Windows.
- On Linux the supervisor can be written with `Process.spawn` alone: each worker is told the names and binds the
  port. On macOS the inherit mode still forks (§4).
- Two unrelated programs can share counters: a server and a tool that reads its statistics while it runs.
- **A new thing a killed program leaves behind** (§3.3), on POSIX. `/dev/shm` on Linux shows them; macOS has no
  listing, which is why a program should be able to say its name again.

## 6. Questions for the owner

1. Named memory (option A) now, with the value and its safe surface as §3.1 has them? Recommended: yes. It is what
   was asked for, it serves programs that are not parent and child, and option B can reuse the type.
2. Should the descriptor half (option B: a list of handles a child inherits, for sockets and for this) be the next
   ADR, or be done first instead? Recommended: next, as its own ADR. It changes `Process.spawn` on three systems,
   and it is what actually removes `fork` from the supervisor on macOS; this ADR alone does that on Linux only.
3. The safe surface is words of `AtomicU64` only, and bytes are reached through `address()` in `unsafe` (§3.1)?
   Recommended: yes. The alternative, `AtomicU8` and `AtomicU32` views as well, adds surface without adding what a
   byte ring needs, which is a protocol.
4. The creator's drop removes the name (§3.1), and nothing else does unless `remove` is called (§3.3)?
   Recommended: yes. The alternative, removing the name as soon as every expected process has opened it, is the
   program's to do with `remove` if it wants it: a worker started later would then find nothing.
5. One rule for names on every system, the strictest (§3.2)? Recommended: yes.

## 7. Implementation plan (once accepted)

1. Spec: §26.17, `spec/api/std.md`, the guide; profiles.md (hosted only); Appendix C.
2. Runtime: `ceesafe_host_shared_create`, `_open`, `_close`, `_remove` for the systems of §3.4. lib/std's
   `SharedMemory` over them. The interpreter's table.
3. Tests:
   - a check like `tools/child_check.py`, since it needs two processes: a parent creates memory under a name with
     its own id in it, spawns three children that each add to one word 100 000 times with `fetchAdd` and to another
     with a `compareExchange` loop, and reads 300 000 in both after they end; a child that opens a name that is not
     there (`NotFound`); `create` twice (`AlreadyExists`); a bad name (`InvalidInput`); a word out of range
     (the panic); after the parent ends, the name is gone (`open` from a second run is `NotFound`); a parent that
     is killed leaves the name, and `remove` takes it;
   - the interpreter: `create` and `open` in one program see one word;
   - on Windows the same programs by the release workflow's smoke job, which would also be the first run there of
     ADR 0169's `Process.spawn`;
   - httpserver's `SharedWords` and `Bus` on it, with `tools/ratelimit_check.py` and `tools/bus_check.py`.
4. ROADMAP §10.
