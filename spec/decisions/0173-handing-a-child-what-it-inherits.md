# ADR 0173 — Handing a child what it inherits: sockets and shared memory

- Status: **Accepted** by the owner, 2026-11-18 ("I accept it as written."): the six questions of §7 as
  recommended. Implemented the same day (§9), **not as written in several places**, which §9 lists; not released.
  Its checks pass on Linux, macOS, Windows 11 x64 (built with GCC and with MSVC) and Windows on ARM64 (built with MSVC). Written at the owner's request ("Write the descriptor passing ADR"): it is the
  second half of ADR 0169 §5 question 2, which ADR 0172 §6 question 2 left as "the next ADR". Not accepted: §7 has
  the questions.
- Scope: lib/std (`Handover`, `Inherited`, `Process.spawnWith`, `Process.inherited`, `SharedMemory.anonymous`),
  the hosted runtime (`runtime/hosted/ceesafe_rt_io.c`, `ceesafe_rt_async.c`: POSIX, Windows), the interpreter's
  host services; type-system.md §26 (a new §26.18, and §26.16 and §26.17 where they are touched), `spec/api/std.md`,
  the guide's processes page. No change to the language or to the backends.

## 1. Context

- **What a worker needs from its supervisor.** httpserver's workers are processes that accept from one listening
  socket their supervisor bound, and count and publish in memory they all map. Today they have both because each
  is a copy of the supervisor: `fork`, declared through `extern "C"`.
- **What 0.70.0 and 0.71.0 gave.** `Process.spawn` (ADR 0169) starts a new program, which shares nothing with its
  parent but the environment, the working directory and the three standard streams. `SharedMemory` (ADR 0172) lets
  the new program find memory by a name it is told.
- **What is still missing.**
  - **The socket.** A spawned worker cannot be given the supervisor's listener. On Linux it can bind the port
    itself (`reusePort`, and the kernel spreads connections); on macOS and Windows it cannot: there the processes
    must share one socket. So the supervisor still forks on macOS and does not exist on Windows.
  - **Memory without a name.** A supervisor's memory is for its workers alone, and a name is something to choose,
    to leak when the supervisor is killed (POSIX), and for another process of the same user to open. httpserver
    makes its memory under a name and removes the name at once, which works only because its workers are forked.
- **What every system has for it.** A child can be started with chosen descriptors of its parent open in it
  (`posix_spawn`'s file actions; Windows' handle list, and `WSADuplicateSocket` for sockets). That is the mechanism
  systemd's socket activation and every prefork server use; it needs no name and leaves nothing behind.
- **What ADR 0169 left open on the way.** Its §2.2 asked that every descriptor the runtime opens be close-on-exec,
  so that a child gets nothing by accident; its notes say that was not audited. An explicit list of what a child
  gets is only worth having if nothing else gets through.

## 2. Options

| | the child is given | cost |
|---|---|---|
| **A. A list at spawn** (this ADR) | the listed sockets and memory, open in it from its first instruction | `Process.spawn` grows a second form; three systems' mechanisms |
| B. Sent over a Unix socket (`SCM_RIGHTS`) | any descriptor, at any time, also to a process that is not a child | a socket between the two, a protocol on it, async Unix sockets in lib/std; Windows has no equivalent for sockets but `WSADuplicateSocket` |
| C. Nothing: each worker binds, memory by name | | macOS and Windows cannot spread connections over processes that each bind; names (§1) |

- B is more general and much larger, and what it adds (a descriptor sent later, or to a stranger) no program here
  has asked for. It can be added beside A.
- C is what 0.71.0 allows, and is why this ADR exists.

## 3. Proposed decision

### 3.1 The API (lib/std)

```ceesafe
// The parent.
var handover = Handover.create();
let socket = handover.listener(borrow listener)?;      // u32: its place in the list, 0 for the first
let counters = handover.memory(borrow shared)?;        // 1
let child = Process.spawnWith(me.view(), arguments, ChildOptions.inherit(), borrow handover)?;

// The child, at its start.
var given = Process.inherited();                       // Inherited: what the parent handed over, or nothing
let listener = given.listener(0)?;                     // AsyncTcpListener: the same socket as the parent's
let shared = given.memory(1)?;                         // SharedMemory: the same memory
```

- **`Handover`** is a list of things the child is to have: `listener(borrow AsyncTcpListener)`,
  `datagrams(borrow AsyncUdpSocket)` and `memory(borrow SharedMemory)`, each returning the place it was given.
  It borrows what it lists, so the parent still has each one after the spawn, and one `Handover` serves every
  child that is to get the same things (a worker started again gets them as the first did). At most 16 places.
- **`Process.spawnWith(program, arguments, options, handover)`** is `Process.spawn` with the list. `spawn` is
  unchanged and hands over nothing.
- **`Process.inherited()`** gives the child's `Inherited`: `count()`, `kind(place)` (`Listener`, `Datagrams`,
  `Memory`, or null for a place that is empty or was taken), and `listener(place)`, `datagrams(place)` and
  `memory(place)`, which move the thing out: each place is taken once, and asking again, or for the wrong kind, is
  `Err(IoError.NotFound)`. In a program that was not started with a handover it is empty.
- **What is shared is the thing itself, not a copy.** Both processes accept from the one socket, and the kernel
  gives each connection to one of them; both map the one memory. Closing or dropping it in one process does not
  close it in the other.
- **`SharedMemory.anonymous(bytes)`**: memory with no name, for handing over. `create` and `open` are as they are.
- **Places are numbers**, in the order they were listed: the parent and the child are one program, or two that
  agree, as they must agree on what the memory holds.

### 3.2 What a child gets, and what it does not

- **Only what is listed.** Every other descriptor and handle of the parent is closed in the child. This ADR does
  the audit ADR 0169 §2.2 left: each place the runtime opens a descriptor (files, sockets, the reactor, pipes,
  directories, windows) is checked to be close-on-exec or non-inheritable, with a test that a child started by
  `Process.spawn` has no descriptor open but 0, 1 and 2 (§8).
- **Not to its own children.** What a child was handed is close-on-exec in the child from the moment it is taken,
  and what it never takes is closed when `Process.inherited()` is first called. A grandchild is told nothing: the
  word the runtime leaves for the child names its parent, and a process whose parent is another ignores it.
- **A child that is not a CeeSafe program** (or is an older one) has the descriptors open and does not know it. They
  are numbered from 3 in list order on POSIX, with `CEESAFE_HANDOVER` in its environment saying how many and of
  which kinds, so such a program can use them (`LISTEN_FDS` is the same idea; question 4 asks whether to speak
  that convention instead).

### 3.3 Systems

| | the list reaches the child by | a socket | memory |
|---|---|---|---|
| Linux, macOS, FreeBSD | `posix_spawn` file actions: each listed descriptor duplicated to 3, 4, … in the child, which clears close-on-exec there; `CEESAFE_HANDOVER` in the child's environment | the descriptor | the descriptor of the shared object: `SharedMemory` keeps it open for as long as the value lives (it closes it after mapping today), and `anonymous` is `memfd_create` on Linux, `shm_open` with a name removed at once elsewhere |
| Windows | the child is created suspended with an explicit handle list (`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`), told through a small inherited section what it was given, and resumed | `WSADuplicateSocketW` for the child's process id; the child makes its socket from the protocol information (`WSASocketW`). Handle inheritance alone is not reliable for sockets | an inheritable duplicate of the mapping's handle |
| Android, CosmoOS | `Unsupported` | | |
| the interpreter | `spawnWith` is `Unsupported`, as `spawn` is; `Process.inherited()` is empty | | |

- An allocation budget (ADR 0170) counts none of it.
- Freestanding: none of it (`profiles.md` §4).

## 4. What it does not do

- **No connected streams, files or pipes in the list.** A listener, a datagram socket and memory are what a
  supervisor's workers share for their whole life. A connection or a file handed to a child is a different
  contract (who reads what was already buffered? who closes?), and option B is its place.
- **Nothing sent after the start**, and nothing to a process that is not a child (option B).
- **No resource limits** for the child. httpserver's supervisor limits a worker's address space with `setrlimit`
  after `fork`; a spawned worker would set its own limit at its start, still through `extern "C"`. ADR 0169 §3
  left limits to their own design, and they stay there.
- **No fairness.** Which of several processes that accept from one socket gets a connection is the kernel's
  choice. httpserver's CI met a three-core macOS machine on which one of four such workers was given none for
  seconds. That is a property of the shared socket, with `fork` as with this.

## 5. Consequences

- **httpserver's supervisor can be written without `fork`**, on Linux, macOS and Windows: it binds the listener,
  makes anonymous memory, and starts itself again as each worker with both handed over
  (`Process.executable()`, an argument for the worker's number). `fork`, `waitpid` and `kill` go, for
  `ChildProcess`; `mmap` went with 0.71.0; `setrlimit` stays (§4).
- **A worker is a new program, not a copy.** It starts from `main`, so what the supervisor built before forking
  (routes, tables, parsed configuration) each worker builds for itself: slower to start, by the time `main`
  takes, and nothing is shared by accident. The cost is measured on httpserver's supervisor check before this
  lands (§8).
- **`SharedMemory` holds a descriptor** on POSIX for each value, where it held none: a program with many mappings
  uses that many descriptors.
- **`SharedMemory.anonymous`** replaces httpserver's create-then-remove, and with it a supervisor that is killed
  leaves nothing behind on any system.
- **The runtime's descriptors are all close-on-exec**, checked by a test, which also protects `Process.run` and
  `Process.capture`.

## 6. What it costs to get wrong

- A descriptor that leaks to a child is a socket that stays open after its owner closed it (a port that cannot be
  bound again, a peer that never sees the end). The test of §3.2 is what stands against it; it is in the contract
  suite on each system that has a runner.
- A place taken with the wrong kind is an error, not a socket treated as memory: the kinds travel with the list.

## 7. Questions for the owner

1. A list at spawn (option A), as `Handover`, `Process.spawnWith` and `Process.inherited()` (§3.1)? Recommended:
   yes.
2. The kinds: a TCP listener, a UDP socket and shared memory, and no connected streams, files or pipes (§4)?
   Recommended: yes. The UDP socket is there because an HTTP/3 server's workers share one as they share the
   listener; leaving it out would leave `fork` in exactly those servers.
3. `SharedMemory.anonymous(bytes)` in this ADR, with `SharedMemory` keeping its descriptor open on POSIX (§3.3)?
   Recommended: yes: memory to hand over should not need a name, and it cannot be handed over without the
   descriptor.
4. The runtime's own word to the child (`CEESAFE_HANDOVER`), not systemd's `LISTEN_FDS` and `LISTEN_PID`?
   Recommended: yes. `LISTEN_FDS` says sockets only and has no kinds, and a CeeSafe program started by systemd with
   real socket activation would then find its sockets through `Process.inherited()` only by accident of the same
   name. Reading `LISTEN_FDS` as a source of listeners can be its own small ADR.
5. Do the close-on-exec audit here (§3.2), not separately? Recommended: yes: the list means nothing without it.
6. Windows in the same release as POSIX, or after it? Recommended: the same release, written against §3.3 and
   compiled by the release workflow, and **said to be untested until something runs it**: there is still no
   Windows machine that runs ADR 0169's or 0172's checks, and that gap grows with each ADR. A Windows job that
   runs `child_check`, `shared_check` and this ADR's check is the first step of the implementation, not the last.

## 8. Implementation plan (once accepted)

1. A Windows job in the checks workflow that builds `ceesafec` and runs `tools/child_check.py` and
   `tools/shared_check.py` (question 6); what it finds in ADR 0169 and 0172 is fixed first.
2. The audit of §3.2 and its test: a program spawned by `Process.spawn` lists its open descriptors
   (`/proc/self/fd`, `/dev/fd`; `NtQuerySystemInformation` is not needed on Windows, where the handle list is the
   guarantee) and has only 0, 1 and 2.
3. Spec: §26.18; §26.16 (`spawnWith`), §26.17 (`anonymous`, the descriptor); `spec/api/std.md`; the guide;
   Appendix C.
4. Runtime and lib/std: `SharedMemory`'s descriptor and `anonymous`; the handover in `ceesafe_host_child_start`
   (a list of descriptors and kinds beside the streams); the child's side; the interpreter's answers.
5. Tests, in a check like `tools/shared_check.py`:
   - a parent binds a listener on a free port and makes anonymous memory, starts itself three times with both,
     and each child accepts connections and counts them in the memory: a client that makes 300 connections is
     answered 300 times, the counts sum to 300, and (Linux, where the kernel spreads them) every child answered
     some;
   - the parent drops its listener after the spawn and the children go on accepting; the children end and a new
     bind of the port succeeds (nothing leaked);
   - a place taken twice, a place of the other kind and a place past the end are `NotFound`; a program started
     without a handover has an empty `Inherited`; a grandchild started by `Process.spawn` has an empty one too;
   - a UDP socket handed to two children, each of which receives;
   - the interpreter: `Unsupported`, and empty.
6. httpserver's supervisor on it, with its checks, and the time to start four workers by `fork` and by spawn
   recorded in ROADMAP §10.
7. ROADMAP §10.

## 9. Implementation notes (2026-11-18)

What is there is §3.1's API on Linux, macOS, FreeBSD and Windows. It differs from what was accepted in these
places; the first four are the ones to know.

- **A `Handover` holds what it lists; it does not borrow it** (§3.1 said it borrows). Listing a thing duplicates
  its descriptor into the handover (`ceesafe_host_handover_hold`: a duplicate at 64 or above, close-on-exec; on
  Windows a second handle), which the handover's drop lets go of. A struct that keeps borrows of three kinds needs
  vectors of borrows, which a struct's initialiser cannot name (`Vector<borrow(r) T>.create()` does not parse);
  and the duplicate is the better contract: the parent may drop its listener and go on handing it to new workers,
  and no handover can outlive what it lists.
- **The word to the child names no parent** (§3.2 said it would, and that a process whose parent is another
  ignores it). `CEESAFE_HANDOVER` holds the kinds, a letter a place. A grandchild is told nothing because every
  program the runtime starts is given an environment without the word, and none of this process's descriptors
  but the three and those it is handed. A parent's id would have failed a child that asks after its parent has
  ended.
- **A place that is never taken stays open in the child** until it ends, close-on-exec (§3.2 said it is closed
  when `Process.inherited()` is first called, which cannot know what will be taken later).
- **Windows does not use a handle list** (§3.3). The child is created suspended; a mapping's handle is duplicated
  into it (`DuplicateHandle`), a socket is duplicated for its process id (`WSADuplicateSocketW`), both are written
  into a record, a small section under a name of the parent's own that the child finds through
  `CEESAFE_HANDOVER` and holds a handle of; then it is resumed. `CreateProcessW` still inherits handles as
  `Process.spawn` did, for the three streams: the runtime's sockets are made non-inheritable where they enter its
  table, and its files and mappings never were, but **nothing lists a Windows child's handles to prove it**
  (§8 step 2 said the handle list would be the guarantee).
- **`SharedMemory.anonymous` on Linux** is `shm_open` under a name of the process's own that is removed in the
  same call, as on macOS and FreeBSD, not `memfd_create` (§3.3): one path for the three systems.
- **What closes the parent's other descriptors.** The system, where it can be asked: macOS
  (`POSIX_SPAWN_CLOEXEC_DEFAULT`), glibc 2.34 and later and FreeBSD 14 (`closefrom` as a file action). Elsewhere
  the audit holds it: TCP sockets, accepted connections, UDP sockets, the executor's wake pipe and its kqueue were
  not close-on-exec and are now (`cee_cloexec`, set after the call that makes the descriptor, so another thread
  that starts a program in between still passes it on); files, directories, epoll, eventfd and the signal pipe
  already were.
- **Async code can start and watch a child.** `Process.spawn` and `ChildProcess.poll` were declared as calls that
  may sleep (ADR 0169), so an async function could not call them (N0295), and ADR 0169 §2.1 says async code polls
  on a timer. `poll` and `spawn` no longer are; `wait` is, through a runtime function of its own.
- **§8 step 1, Windows first, was done on a machine, not in a workflow.** `.github/workflows/windows.yml` is
  written (it builds `ceesafec` from the tree, compiles its C with MSVC and runs `child_check`, `shared_check` and
  `handover_check`) and **has not run**: nothing was pushed. The owner gave a Windows 11 laptop instead, where
  `ceesafec` was built from its own C with GCC 16 (MinGW-w64, UCRT) and the checks run: `child_check` 6 cases,
  `shared_check` 7, `handover_check` 5, `budget_check` 3, no problems. Then, in the owner's Windows on ARM64
  virtual machine, `ceesafec` was built from its C with MSVC by the release workflow's command, with no warning,
  and the same four checks pass with `cl` as the C compiler (`budget_check` 5 cases there). Last, with Visual Studio's
  build tools added to the laptop, the same with MSVC for x64: no warning, and the four checks pass. Before that the three programs were
  cross-compiled and run under Wine, which found the first of the faults below.
- **Three faults found by running on Windows**, the last two in ADR 0169's and ADR 0125's code, which had never
  run there:
  - unnamed memory was refused as "already exists": a successful `CreateFileMappingW` need not clear the last
    error, which is now cleared before the call and read only for a name;
  - **a child told to `terminate` heard `Interrupt`.** ADR 0125 reports Ctrl+C and Ctrl+Break as `Interrupt`, and
    Ctrl+Break is the one console event a process can send to another's group, so it is what `terminate` sends
    (ADR 0169 §2.3), whose §2.1 says the child hears `Terminate`. **Ctrl+Break is now `Terminate`**, which changes
    ADR 0125's rule for a user's own Ctrl+Break too: §26.15 and `ProcessSignal`'s comment say so;
  - with that, the child never ended: the handler of a terminate request does not return, which a console's
    close needs and Ctrl+Break does not. It returns for Ctrl+Break.
  `child_check` skipped on Windows and now runs there (a killed child has status 1; children are found by the
  program's name).
- **The test of §8 that every worker answers some connections on Linux** is not made: with one socket, which
  process is given a connection is the kernel's choice on Linux too.
- **Tests:** `tools/handover_check.py`, in the contract suite as `handover`: 6 cases on macOS and on Linux
  (ceesafec built there from its own C), 5 on Windows (no listing of descriptors there). httpserver's supervisor
  is not moved onto it yet (§8 step 6): it needs a released compiler.
